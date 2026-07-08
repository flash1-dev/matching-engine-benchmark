/* cpp_orderbook_adapter.cpp — geseq/cpp-orderbook behind the harness
 * matching_engine_api.h ABI.
 *
 * Engine native API. cpp-orderbook is a header-template C++ limit-order book.
 * OrderBook<Notification> exposes addOrder(id, type, side, qty, price, flag) /
 * cancelOrder(id) / hasOrder(id). It reports every event through a single
 * onExecutionReport(ExecutionReport) callback reached via the CRTP
 * NotificationInterface<Implementation> (non-virtual static dispatch, no
 * std::function at the notification boundary). Internally the engine DOES route
 * per-fill trade/fill notifications through std::function — pricelevel.cpp is
 * explicitly instantiated against the TradeNotification / PostOrderFill
 * std::function aliases — so that indirection is engine-level and not removable
 * from the adapter.
 *
 * Trades. The Trade ExecType carries maker/taker ids + last_qty + last_price;
 * the harness Trade is derived from it directly — the maker price is the
 * resting order's price (last_price), and the aggressor's seq is the in-flight
 * message's sequence_number, threaded through gCurSeq.
 *
 * Reports synthesised above the engine. The Trade and Canceled/Rejected
 * callbacks lack the side/price the harness wire format requires, so
 * OrderAck / CancelAck / ModifyAck / CancelReject / ModifyReject are all
 * synthesised in the adapter (the engine's New/Accepted notification is
 * ignored). IOC is delegated via Flag::IoC; the engine discards the unfilled
 * residual without notifying, so the adapter emits the residual CancelAck
 * itself when the taker's filled tally falls short of the input quantity. The
 * engine has no native modify, so Modify is cancel + reinsert — the cancel half
 * is adjudicated by the engine (Rejected → ModifyReject) and the reinsert's
 * crossing fills cross with the modify message's seq, matching the harness
 * contract (the reinsert loses queue priority).
 *
 * Per-order shadow. A flat array indexed by order id holds
 * {oid -> price, side, remaining, alive}: it supplies the side/price echoed on
 * the synthesised acks (the engine's reports carry neither) -- reject/ack
 * adjudication only. The audit queries (engine_query_best_bid/best_ask/
 * depth_at) do NOT read the shadow: they forward to the engine's own book,
 * parsing OrderBook::toString() -- the engine's public dump of its private
 * bids_/asks_ price trees -- so a stale/phantom engine book state would
 * surface to the gate instead of being masked by the adapter's bookkeeping.
 *
 * Decimals. Workload prices/quantities are int64 ticks / uint32. The engine
 * stores Decimal = decimal::U8; Decimal(ticks, 0) carries each tick as internal
 * fp = ticks * 10^8 (a strictly increasing, bit-for-bit mapping) and
 * d.to_int() recovers the tick exactly. This mirrors the geseq Go adapter's
 * udecimal.New(tick,0) / d.Int().
 *
 * Pin. NO source patch is applied: the price-cross bug we reported was fixed
 * upstream, and the pinned commit (cpp-orderbook main HEAD) already contains
 * the fix, so build.sh clones and resets without any sed step (the
 * mansoor/piyush pattern, not robaho's).
 *
 * This mirrors the geseq Go adapter (additional_references/geseq_adapter)
 * one-for-one, except cpp-orderbook's addOrder/cancelOrder take no monotonic
 * token (the Go engine does), so there is no token counter here.
 */

// PERFORMANCE NOTES
// -----------------
// The engine's per-event report callback (onExecutionReport) is reached through
// the CRTP NotificationInterface<Implementation>, i.e. a non-virtual, non-
// std::function static dispatch the compiler can inline. This adapter's
// HarnessNotification is a concrete struct with a single non-virtual handler,
// so the engine inlines straight into it and the adapter adds NO indirection of
// its own (no virtual, no std::function, no type erasure).
//
// The engine DOES route per-fill trade/fill notifications through std::function
// internally: OrderQueue::process / PriceLevel::process{Market,Limit}Order take
// `const TradeNotification&` / `const PostOrderFill&` (std::function aliases in
// orderqueue.hpp), and src/pricelevel.cpp is *explicitly instantiated* against
// those std::function types. That indirection is therefore ENGINE-level and
// cannot be removed from the adapter without re-templating the engine; the
// adapter just hands the engine the same onExecutionReport sink it always uses.
//
// Everything the adapter controls is on the stack and branch-lean: the report
// struct is a stack local, the shadow store is a flat array indexed by order id
// (O(1), pre-sized once, single cold growth branch), and the hot handlers are
// always-inlined.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "matching_engine_api.h"
#include "orderbook.hpp"
#include "types.hpp"

using orderbook::Decimal;
using orderbook::ExecType;
using orderbook::ExecutionReport;
using orderbook::Flag;
using orderbook::OrderBook;
using orderbook::OrderID;
using orderbook::Side;
using orderbook::Type;

#define HOT_INLINE __attribute__((always_inline, hot)) inline

// ---------------------------------------------------------------------------
// Decimal conversions.
//
// Workload ticks are positive signed integers. Decimal = decimal::U8 carries
// each tick as Decimal(tick, 0) which produces internal fp = tick * 10^8. The
// engine compares fp directly, so a strictly increasing tick mapping is
// preserved bit-for-bit, and d.to_int() recovers fp / 10^8 = tick exactly
// (round-trips). This mirrors the geseq Go adapter's udecimal.New(tick,0) /
// d.Int().
//
// Decimal(i, 0) is a single constant multiply (i * 10^8 via precomputed_pow_10)
// and to_int() is a single constant divide (fp / 10^8) — no string work, no
// allocation, fully inlined.
// ---------------------------------------------------------------------------

HOT_INLINE Decimal toDecPrice(int64_t ticks) {
    if (ticks <= 0) [[unlikely]] {
        return Decimal(uint64_t(0));
    }
    return Decimal(uint64_t(ticks), 0);
}

HOT_INLINE Decimal toDecQty(uint32_t q) { return Decimal(uint64_t(q), 0); }

HOT_INLINE int64_t fromDecPrice(const Decimal& d) { return int64_t(d.to_int()); }

HOT_INLINE uint32_t fromDecQty(const Decimal& d) {
    uint64_t v = d.to_int();
    if (v > UINT32_MAX) [[unlikely]] {
        return UINT32_MAX;
    }
    return uint32_t(v);
}

// ---------------------------------------------------------------------------
// Shadow state.
//
// 16 bytes, packed: int64 price + uint32 remaining + uint8 side + bool alive
// (+2 pad). Order quantities are uint32_t at the ABI boundary, so remaining
// never exceeds uint32_t. Keeping the slot to 16 bytes (vs 24) halves the
// cache footprint of the linear audit-query scans.
// ---------------------------------------------------------------------------

struct Shadow {
    int64_t price = 0;
    uint32_t remaining = 0;
    uint8_t side = 0;  // 0 = buy, 1 = sell
    bool alive = false;
};

namespace {

// Pre-sized once to a generous upper bound (the canonical workload is ~1M new
// orders). A raw base pointer over the vector's storage avoids re-reading the
// vector control block per access; gShadowCap bounds it with a single cold
// branch that almost never fires.
constexpr size_t kShadowInit = size_t(1) << 22;

std::vector<Shadow> gShadow;
Shadow* gShadowBase = nullptr;
size_t gShadowCap = 0;

const me_transport_t* gTransport = nullptr;
void* gSink = nullptr;

// Per-call context: onExecutionReport(Trade) reads gCurSeq, accumulates into
// gTakerFill, and decrements the maker's shadow. onExecutionReport(Canceled /
// Rejected) records the engine's cancel verdict in gCancelOK/gCancelQty.
uint64_t gCurSeq = 0;
uint64_t gTakerFill = 0;
bool gCancelOK = false;
uint64_t gCancelQty = 0;

// Forward decls used by the notification handler.
HOT_INLINE void emitTrade(uint64_t seq, uint64_t makerID, uint64_t takerID, int64_t price, uint32_t qty);

// O(1) shadow slot. The growth path is a single predicted-cold branch; the
// store is pre-sized to kShadowInit so it never fires in the canonical run.
HOT_INLINE Shadow* shadowSlot(uint64_t oid) {
    if (oid >= gShadowCap) [[unlikely]] {
        gShadow.resize(oid + 1);
        gShadowBase = gShadow.data();
        gShadowCap = gShadow.size();
    }
    return gShadowBase + oid;
}

// ---------------------------------------------------------------------------
// Notification handler (cpp-orderbook CRTP NotificationInterface).
//
// The engine fires onExecutionReport:
//  - ExecType::New (CreateOrder, Accepted) once per addOrder, before matching.
//    Ignored — engine_on_new_order eagerly emits ME_ORDER_ACK with the
//    side+price the report doesn't carry.
//  - ExecType::Trade per fill: maker/taker ids + last_qty + last_price.
//  - ExecType::Canceled (CancelOrder) on a successful cancelOrder: the engine's
//    cancel verdict. Recorded into gCancelOK/gCancelQty; engine_on_cancel
//    emits the ME_CANCEL_ACK itself with the side/price the report lacks.
//  - ExecType::Rejected on a cancel of a not-resting order (CancelOrder +
//    Error::OrderNotExists) or on duplicate-ID / zero-qty / zero-price create
//    errors. The cancel reject is the source of the gCancelOK = false verdict;
//    create rejects don't occur in the canonical workload and are ignored.
// ---------------------------------------------------------------------------

class HarnessNotification : public orderbook::NotificationInterface<HarnessNotification> {
   public:
    HOT_INLINE void onExecutionReport(const ExecutionReport& r) {
        switch (r.exec_type) {
            case ExecType::Trade: {
                uint32_t q = fromDecQty(r.last_qty);
                int64_t p = fromDecPrice(r.last_price);
                emitTrade(gCurSeq, r.maker_order_id, r.taker_order_id, p, q);
                gTakerFill += q;

                Shadow* e = shadowSlot(r.maker_order_id);
                uint32_t rem = e->remaining;
                rem = (rem >= q) ? uint32_t(rem - q) : 0u;
                e->remaining = rem;
                if (rem == 0) {
                    e->alive = false;
                }
                break;
            }
            case ExecType::Canceled: {
                // Public cancelOrder verdict: removed.
                gCancelOK = true;
                gCancelQty = fromDecQty(r.qty);
                break;
            }
            case ExecType::Rejected: {
                // cancelOrder of a not-resting order reports
                // CancelOrder + OrderNotExists. gCancelOK stays false (set by
                // the caller before cancelOrder); nothing to record.
                break;
            }
            case ExecType::New:
            default:
                // Accept notification carries no payload the adapter needs;
                // the OrderAck is synthesised above the engine.
                break;
        }
    }
};

HarnessNotification gNotification;
OrderBook<HarnessNotification>* gBook = nullptr;

// ---------------------------------------------------------------------------
// Report emission.
// ---------------------------------------------------------------------------

HOT_INLINE void cpu_pause() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

HOT_INLINE void emit(const me_report_t* r) {
    while (gTransport->push(gSink, r) == 0) [[unlikely]] {
        cpu_pause();  // spin until the transport accepts the report
    }
}

HOT_INLINE void emitAck(uint8_t rtype, uint64_t seq, uint64_t oid, uint8_t side, int64_t price, uint32_t qty) {
    me_report_t r{};
    r.type = rtype;
    r.side = side;
    r.sequence_number = seq;
    r.order_id = oid;
    r.price_ticks = price;
    r.quantity = qty;
    emit(&r);
}

HOT_INLINE void emitTrade(uint64_t seq, uint64_t makerID, uint64_t takerID, int64_t price, uint32_t qty) {
    me_report_t r{};
    r.type = ME_TRADE;
    r.side = 0;
    r.sequence_number = seq;
    r.order_id = makerID;
    r.price_ticks = price;
    r.quantity = qty;
    r.maker_order_id = makerID;
    r.taker_order_id = takerID;
    emit(&r);
}

// ---------------------------------------------------------------------------
// Per-message handlers (shared by the per-message ABI and engine_on_batch).
// ---------------------------------------------------------------------------

HOT_INLINE void onNewOrder(const new_order_t* o) {
    const uint64_t seq = o->sequence_number;
    const uint64_t oid = o->order_id;
    const uint8_t side = o->side;
    const int64_t price = o->price_ticks;
    const uint32_t qty = o->quantity;
    const uint8_t ioc = o->ioc;

    // 1. OrderAck. (Engine fires Accepted too; we ignore that.)
    emitAck(ME_ORDER_ACK, seq, oid, side, price, qty);

    // 2. Drive the engine. onExecutionReport(Trade) reads gCurSeq and writes
    //    gTakerFill / decrements the maker shadow.
    gCurSeq = seq;
    gTakerFill = 0;

    const Side sideT = (side == 0) ? Side::Buy : Side::Sell;
    const Flag flag = (ioc != 0) ? Flag::IoC : Flag::None;

    gBook->addOrder(oid, Type::Limit, sideT, toDecQty(qty), toDecPrice(price), flag);

    const uint64_t filled = gTakerFill;
    const uint32_t residual = (filled < qty) ? uint32_t(qty - filled) : 0;

    if (ioc != 0) [[unlikely]] {
        // IoC residual cancellation: emit a CancelAck for the unfilled
        // remainder. The engine discards the residual without notifying.
        if (residual > 0) {
            emitAck(ME_CANCEL_ACK, seq, oid, side, price, residual);
        }
        return;
    }

    // GTC: shadow tracks the resting remainder.
    Shadow* e = shadowSlot(oid);
    e->price = price;
    e->side = side;
    e->remaining = residual;
    e->alive = (residual > 0);
}

HOT_INLINE void onCancel(const cancel_t* c) {
    const uint64_t seq = c->sequence_number;
    const uint64_t oid = c->order_id;

    // The engine adjudicates: cancelOrder reports Canceled on removal, or
    // Rejected(OrderNotExists) for never-seen / already-terminal ids.
    gCancelOK = false;
    gCancelQty = 0;
    gBook->cancelOrder(oid);
    if (!gCancelOK) [[unlikely]] {
        emitAck(ME_CANCEL_REJECT, seq, oid, 0, 0, 0);
        return;
    }

    // Payload echo: side/price from the shadow (the engine's report carries
    // neither); the quantity is the engine's own reported remainder.
    Shadow* e = shadowSlot(oid);
    emitAck(ME_CANCEL_ACK, seq, oid, e->side, e->price, uint32_t(gCancelQty));
    e->alive = false;  // audit queries scan alive/remaining
}

HOT_INLINE void onModify(const modify_t* m) {
    const uint64_t seq = m->sequence_number;
    const uint64_t oid = m->order_id;
    const int64_t newPrice = m->new_price_ticks;
    const uint32_t newQty = m->new_quantity;

    // The engine adjudicates the cancel half of cancel + reinsert: Rejected
    // (never seen / already terminal) maps to ModifyReject.
    gCancelOK = false;
    gCancelQty = 0;
    gBook->cancelOrder(oid);
    if (!gCancelOK) [[unlikely]] {
        emitAck(ME_MODIFY_REJECT, seq, oid, 0, 0, 0);
        return;
    }

    Shadow* e = shadowSlot(oid);
    const uint8_t side = e->side;  // payload echo (report has no side)

    emitAck(ME_MODIFY_ACK, seq, oid, side, newPrice, newQty);

    // Reinsert at the new price/quantity; its crossing fills emit ME_TRADE.
    gCurSeq = seq;
    gTakerFill = 0;

    const Side sideT = (side == 0) ? Side::Buy : Side::Sell;
    gBook->addOrder(oid, Type::Limit, sideT, toDecQty(newQty), toDecPrice(newPrice), Flag::None);

    const uint64_t filled = gTakerFill;
    const uint32_t residual = (filled < newQty) ? uint32_t(newQty - filled) : 0;

    // The reinsert may have grown the shadow store (cold), so re-fetch the slot.
    e = shadowSlot(oid);
    e->price = newPrice;
    e->side = side;
    e->remaining = residual;
    e->alive = (residual > 0);
}

// ---------------------------------------------------------------------------
// toString() parsing -- the audit-query forwarding path.
//
// OrderBook::toString() is the engine's only public accessor over its private
// bids_/asks_ price trees. It walks both boost::intrusive::rbtrees best-first
// (bids_ compares with std::greater -> begin() is the highest bid; asks_
// compares with std::less -> begin() is the lowest ask) and prints one price
// level per line, best-first, until both sides are exhausted:
//
//   "<bidTotalQty>\t<bidPrice> | <askPrice>\t<askTotalQty>\n"
//
// Note the bid/ask column order is reversed (qty-then-price vs price-then-
// qty) -- that is the engine's own toString(), not this adapter's choice. A
// side that runs out before the other prints a blank column: "\t\t\t" for a
// bid, nothing at all for an ask. Every price/qty in this workload is
// Decimal(ticks, 0) -- a whole multiple of the fixed-point scale -- and
// Decimal::to_string() (which operator<< uses) always trims the fractional
// part and its point entirely when it is zero, so every field toString()
// emits is a bare non-negative base-10 integer: a plain digit scan recovers
// the tick exactly, no float/decimal parsing needed. The engine prunes an
// OrderQueue from its price tree the instant its last order is removed
// (pricelevel.cpp PriceLevel::remove), so there are no phantom zero-qty
// levels for this parse to trip over.
// ---------------------------------------------------------------------------

// Splits one toString() line at the literal " | " separator it always emits
// between the bid and ask columns.
HOT_INLINE void splitBidAsk(std::string_view line, std::string_view& bidPart, std::string_view& askPart) {
    auto pos = line.find(" | ");
    if (pos == std::string_view::npos) {
        bidPart = {};
        askPart = {};
        return;
    }
    bidPart = line.substr(0, pos);
    askPart = line.substr(pos + 3);
}

// Parses a run of leading ASCII digits into an unsigned integer. Returns
// false if there are none (a blank/placeholder column, e.g. the "\t\t\t"
// bid side of a row where only the ask side still has a level).
HOT_INLINE bool parseU64(std::string_view s, uint64_t& out) {
    size_t i = 0, n = s.size();
    size_t start = i;
    uint64_t v = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + uint64_t(s[i] - '0');
        ++i;
    }
    if (i == start) return false;
    out = v;
    return true;
}

// One resting price level exactly as toString() prints one column:
// bid = "qty\tprice", ask = "price\tqty" (reversed field order -- see above).
HOT_INLINE bool parseBidLevel(std::string_view part, int64_t& price, uint64_t& qty) {
    auto tab = part.find('\t');
    if (tab == std::string_view::npos) return false;
    uint64_t q = 0, p = 0;
    if (!parseU64(part.substr(0, tab), q)) return false;
    if (!parseU64(part.substr(tab + 1), p)) return false;
    qty = q;
    price = int64_t(p);
    return true;
}

HOT_INLINE bool parseAskLevel(std::string_view part, int64_t& price, uint64_t& qty) {
    auto tab = part.find('\t');
    if (tab == std::string_view::npos) return false;
    uint64_t p = 0, q = 0;
    if (!parseU64(part.substr(0, tab), p)) return false;
    if (!parseU64(part.substr(tab + 1), q)) return false;
    price = int64_t(p);
    qty = q;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Exported ABI.
// ---------------------------------------------------------------------------

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport, void* report_sink) {
    gTransport = transport;
    gSink = report_sink;
    gCurSeq = 0;
    gTakerFill = 0;
    gCancelOK = false;
    gCancelQty = 0;

    gShadow.assign(kShadowInit, Shadow{});
    gShadowBase = gShadow.data();
    gShadowCap = gShadow.size();

    delete gBook;
    // Generous pools: the canonical workload has up to ~millions of orders;
    // the AdaptiveObjectPool grows on demand so these are just initial sizes.
    gBook = new OrderBook<HarnessNotification>(gNotification, 1 << 16, 1 << 21);
}

void engine_shutdown(void) {
    delete gBook;
    gBook = nullptr;
    gShadow.clear();
    gShadow.shrink_to_fit();
    gShadowBase = nullptr;
    gShadowCap = 0;
}

void engine_on_new_order(const new_order_t* order) { onNewOrder(order); }

void engine_on_cancel(const cancel_t* cancel) { onCancel(cancel); }

void engine_on_modify(const modify_t* modify) { onModify(modify); }

void engine_flush(void) {
    // Synchronous matcher: engine_on_* has already produced every report.
}

int64_t engine_query_best_bid(void) {
    const std::string dump = gBook->toString();
    const std::string_view sv(dump);
    const auto nl = sv.find('\n');
    const std::string_view firstLine = sv.substr(0, nl == std::string_view::npos ? sv.size() : nl);

    std::string_view bidPart, askPart;
    splitBidAsk(firstLine, bidPart, askPart);

    int64_t price;
    uint64_t qty;
    if (!parseBidLevel(bidPart, price, qty)) return INT64_MIN;
    return price;
}

int64_t engine_query_best_ask(void) {
    const std::string dump = gBook->toString();
    const std::string_view sv(dump);
    const auto nl = sv.find('\n');
    const std::string_view firstLine = sv.substr(0, nl == std::string_view::npos ? sv.size() : nl);

    std::string_view bidPart, askPart;
    splitBidAsk(firstLine, bidPart, askPart);

    int64_t price;
    uint64_t qty;
    if (!parseAskLevel(askPart, price, qty)) return INT64_MAX;
    return price;
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    const std::string dump = gBook->toString();
    const std::string_view sv(dump);

    uint64_t total = 0;
    size_t pos = 0;
    while (pos < sv.size()) {
        const auto nl = sv.find('\n', pos);
        const std::string_view line = (nl == std::string_view::npos) ? sv.substr(pos) : sv.substr(pos, nl - pos);

        std::string_view bidPart, askPart;
        splitBidAsk(line, bidPart, askPart);

        int64_t lvlPrice;
        uint64_t lvlQty;
        if (side == 0) {
            if (parseBidLevel(bidPart, lvlPrice, lvlQty) && lvlPrice == price_ticks) total += lvlQty;
        } else {
            if (parseAskLevel(askPart, lvlPrice, lvlQty) && lvlPrice == price_ticks) total += lvlQty;
        }

        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return total;
}

void engine_on_batch(const me_msg_t* msgs, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const me_msg_t& m = msgs[i];
        switch (m.type) {
            case 0:
                onNewOrder(&m.no);
                break;
            case 1:
                onCancel(&m.c);
                break;
            case 2:
                onModify(&m.md);
                break;
            default:
                break;
        }
    }
}

}  // extern "C"
