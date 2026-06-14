/*
 * quantcup_adapter.cpp — matching_engine_api.h backed by QuantCup.
 *
 * Upstream: https://github.com/ajtulloch/quantcup-orderbook — a C++/Boost port
 *   of voyager's winning entry from the 2011 QuantCup matching-engine contest
 *   (original C gist: https://gist.github.com/druska/d6ce3f2bac74db08ee9007cdf98106ef)
 *   — a flat price-indexed array book.
 *
 * Built as a shared library by scripts/build_baselines.sh. Deviations from the
 * upstream source are documented in docs/PATCHES.md (one behavioural change —
 * executeTrade reports each fill at the resting/maker price, not the aggressor
 * price, the standard price-time-priority convention; the patch's other hunks
 * restore the missing contest build skeleton).
 *
 * QuantCup's API exposes no order-state introspection, so this adapter keeps
 * a light shadow of each order (price/qty/side/alive) — populated purely from
 * QuantCup's own execution() reports and the adapter's own limit()/cancel()
 * calls. The shadow is bookkeeping for liveness + the audit queries; it does no
 * matching of its own.
 *
 * The adapter emits the report stream itself — OrderAck, Trade (from QuantCup's
 * execution() callback), CancelAck (per cancel and per IOC residual), ModifyAck,
 * and CancelReject / ModifyReject for a cancel or modify of an order that is
 * not resting — into the harness report transport. QuantCup matches
 * synchronously on the calling thread, so engine_flush() is a no-op.
 */
#include <algorithm>
// <cstddef> must precede QuantCup's headers (engine.h/types.h below): their
// constants.h uses size_t without including <cstddef> and only compiles if
// size_t is already in scope.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include "engine.h"
#include "types.h"

#include "matching_engine_api.h"

#if defined(__aarch64__)
static inline void cpu_pause() { asm volatile("yield" ::: "memory"); }
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
static inline void cpu_pause() { _mm_pause(); }
#else
static inline void cpu_pause() {}
#endif

namespace {

struct OrderState {
    int64_t   price;
    uint32_t  qty;       // current resting quantity
    uint8_t   side;      // 0 = buy, 1 = sell
    bool      alive;
    t_orderid qcid;
};

/* Harness order ids are dense and 1-based (a permutation of 1..N_new), so a
 * flat vector indexed by order_id holds the shadow: no per-insert node
 * allocation on the timed path (a hash map would malloc a node per resting
 * new order), and a never-rested id is just a default slot (alive=false) —
 * the same reject outcome a map miss produced. Sized in engine_init, grown
 * in engine_prebuild (both untimed). */
std::vector<OrderState> g_orders;     // harness order_id -> state
std::vector<t_order>    g_pre;        // pre-built new orders (prebuild)
size_t                  g_pre_idx = 0;
std::vector<t_order>    g_pre_modify; // pre-built modify reinserts (prebuild)
size_t                  g_pre_modify_idx = 0;
size_t                  g_limit_calls = 0;  // prospective limit() calls (prebuild)

const me_transport_t* g_transport = nullptr;         // harness report transport
void*                 g_sink      = nullptr;

uint64_t g_seq        = 0;   // aggressive order's sequence number
uint64_t g_taker_id   = 0;   // aggressive order's harness id
uint32_t g_taker_left = 0;   // aggressive order's unfilled quantity
uint8_t  g_taker_side = 0;   // 0 = buy, 1 = sell

void push_report(const me_report_t& r) {
    while (!g_transport->push(g_sink, &r)) cpu_pause();
}

/* Emit a non-trade report (OrderAck / CancelAck / ModifyAck). */
void emit_ack(uint8_t type, uint64_t seq, uint64_t order_id,
              uint8_t side, int64_t price, uint32_t qty) {
    me_report_t r{};
    r.type            = type;
    r.sequence_number = seq;
    r.order_id        = order_id;
    r.side            = side;
    r.price_ticks     = price;
    r.quantity        = qty;
    push_report(r);
}

/* The harness order_id (<= ~10^6) is carried in QuantCup's 4-char trader Field
 * as little-endian bytes so the execution() callback can recover the maker. */
Field encode_id(uint64_t id) {
    Field f;
    f.fill('\0');
    f[0] = char(id & 0xFF);
    f[1] = char((id >> 8) & 0xFF);
    f[2] = char((id >> 16) & 0xFF);
    f[3] = char((id >> 24) & 0xFF);
    return f;
}
uint64_t decode_id(const Field& f) {
    return  uint64_t(uint8_t(f[0]))
         | (uint64_t(uint8_t(f[1])) << 8)
         | (uint64_t(uint8_t(f[2])) << 16)
         | (uint64_t(uint8_t(f[3])) << 24);
}

// The harness contract uses int64 price_ticks; QuantCup's price-indexed book
// stores prices in `t_price` (unsigned short). The canonical workload stays in
// range, but a custom scenario could walk a price out of t_price — silent
// modulo-2^16 truncation would corrupt the book invisibly, so we fail loud.
// The usable domain is [1, t_price_max - 1]: the engine's pricePoints array
// has t_price_max slots (valid indices stop one short of the max value) and
// t_price_max itself doubles as its empty-ask sentinel, so the max value is
// out of bounds on one side and sentinel-colliding on the other.
constexpr int64_t QC_PRICE_MAX =
    static_cast<int64_t>(std::numeric_limits<t_price>::max()) - 1;
constexpr int64_t QC_PRICE_MIN = 1;

/* Bounds-checked flat-vector lookup. nullptr / alive=false both mean "not
 * resting" — exactly the outcomes a map miss / dead entry produced. */
inline OrderState* find_order(uint64_t ext_id) {
    return ext_id < g_orders.size() ? &g_orders[ext_id] : nullptr;
}

inline void check_qc_price(int64_t price, const char* context) {
    if (price < QC_PRICE_MIN || price > QC_PRICE_MAX) {
        std::fprintf(stderr,
            "ERROR: QuantCup price %lld out of t_price range [%lld, %lld] (%s). "
            "QuantCup's flat price-indexed book caps at uint16_t; use a scenario "
            "whose price walk stays in range.\n",
            (long long)price, (long long)QC_PRICE_MIN,
            (long long)QC_PRICE_MAX, context);
        std::abort();
    }
}

/* Arena-capacity guard, the id-space twin of check_qc_price: QuantCup consumes
 * one monotonically increasing arena slot per limit() call with no bounds
 * check of its own (order_book.cpp: arenaBookEntries.begin() + (++curOrderID),
 * pre-increment — so the arena's kMaxNumOrders slots support at most
 * kMaxNumOrders - 1 calls), and walking past it is silent heap corruption.
 * Counted in engine_prebuild (untimed); news + modifies is a conservative
 * upper bound on limit() calls (a rejected modify never reaches limit()). */
inline void check_qc_arena(void) {
    if (++g_limit_calls >= static_cast<size_t>(OB::kMaxNumOrders)) {
        std::fprintf(stderr,
            "ERROR: workload exceeds QuantCup's arena capacity of %d limit() "
            "calls (one per new order or modify; engine built with "
            "kMaxNumOrders=%d — see -DQC_MAX_NUM_ORDERS in "
            "scripts/build_baselines.sh). QuantCup has no bounds check of its "
            "own, so we fail loud here instead.\n",
            OB::kMaxNumOrders - 1, OB::kMaxNumOrders);
        std::abort();
    }
}

}  // namespace

/* QuantCup's fill callback. executeTrade() invokes it twice per match (once per
 * side); the maker-side call is the one whose trader Field is the resting order.
 * Defined at namespace scope with C++ linkage to match engine.h. */
void execution(t_execution exec) {
    const bool taker_is_buy = (g_taker_side == 0);
    const bool maker_call   = taker_is_buy ? (exec.side == 1) : (exec.side == 0);
    if (!maker_call) return;

    uint64_t maker_id = decode_id(exec.trader);
    uint32_t qty      = uint32_t(exec.size);

    me_report_t r{};
    r.type            = ME_TRADE;
    r.sequence_number = g_seq;
    r.price_ticks     = int64_t(exec.price);   // maker's resting price (patched)
    r.quantity        = qty;
    r.maker_order_id  = maker_id;
    r.taker_order_id  = g_taker_id;
    push_report(r);

    if (g_taker_left >= qty) g_taker_left -= qty;

    OrderState* s = find_order(maker_id);       // decrement the maker's shadow
    if (s) {
        if (s->qty > qty) s->qty -= qty;
        else { s->qty = 0; s->alive = false; }
    }
}

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport,
                 void* report_sink) {
    g_transport = transport;
    g_sink      = report_sink;
    init();
    /* resize (not reserve): value-initialises every slot (alive=false) and
     * faults the pages in, both outside the timed window. engine_prebuild
     * grows past this if a workload ever uses ids beyond 2M. */
    g_orders.resize(1u << 21);
    g_pre.reserve(1u << 21);
    g_pre_modify.reserve(1u << 21);
}

void engine_shutdown(void) {
    destroy();
    g_orders.clear();
}

/* QuantCup matches synchronously on the calling thread — nothing is pending. */
void engine_flush(void) {}

/* Pre-build hook: build each new order's native t_order before the timed
 * window, so engine_on_new_order's measured work is the match alone. */
void engine_prebuild(uint8_t msg_type, const void* msg) {
    if (msg_type == 0) {
        const new_order_t* o = static_cast<const new_order_t*>(msg);
        check_qc_price(o->price_ticks, "engine_prebuild new_order");
        check_qc_arena();
        t_order qo;
        qo.symbol = Field{};
        qo.trader = encode_id(o->order_id);
        qo.side   = (o->side == 0) ? 0 : 1;
        qo.price  = static_cast<t_price>(o->price_ticks);
        qo.size   = o->quantity;
        g_pre.push_back(qo);
        if (o->order_id >= g_orders.size())
            g_orders.resize(std::max<size_t>(g_orders.size() * 2,
                                             static_cast<size_t>(o->order_id) + 1));
    } else if (msg_type == 2) {
        /* Modify is cancel + reinsert: pre-build the reinsert t_order here so
         * engine_on_modify's measured work is the match alone. Exactly one
         * entry per modify MESSAGE (consumed unconditionally below). The
         * price-range guard stays on the timed path's apply branch, so an
         * out-of-range price on a rejected modify still does not abort —
         * the cast below is harmless on that path (the entry goes unused). */
        const modify_t* m = static_cast<const modify_t*>(msg);
        check_qc_arena();
        t_order qo;
        qo.symbol = Field{};
        qo.trader = encode_id(m->order_id);
        qo.side   = (m->side == 0) ? 0 : 1;
        qo.price  = static_cast<t_price>(m->new_price_ticks);
        qo.size   = m->new_quantity;
        g_pre_modify.push_back(qo);
    }
}

void engine_on_new_order(const new_order_t* o) {
    g_seq        = o->sequence_number;
    g_taker_id   = o->order_id;
    g_taker_left = o->quantity;
    g_taker_side = o->side;
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);

    t_orderid qcid = limit(g_pre[g_pre_idx++]);  // t_order built by engine_prebuild

    if (o->ioc) {
        if (g_taker_left > 0) {                // drop + report the IOC residual
            cancel(qcid);
            emit_ack(ME_CANCEL_ACK, o->sequence_number, o->order_id,
                     o->side, o->price_ticks, g_taker_left);
        }
    } else if (g_taker_left > 0) {
        g_orders[o->order_id] = { o->price_ticks, g_taker_left,
                                  o->side, true, qcid };   // indexed store, no alloc
    }
}

void engine_on_cancel(const cancel_t* c) {
    OrderState* s = find_order(c->order_id);
    if (s && s->alive) {
        cancel(s->qcid);
        emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id,
                 s->side, s->price, 0);
        s->alive = false;
        s->qty   = 0;
    } else {
        // Order is not resting — already filled, already cancelled, or never
        // seen (a duplicate/stale cancel). Answer with a reject, not an ack.
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
    }
}

void engine_on_modify(const modify_t* m) {
    g_seq = m->sequence_number;
    /* Consume the reinsert t_order pre-built by engine_prebuild. Advance the
     * index UNCONDITIONALLY — exactly one entry was built per modify message,
     * so both the reinsert and the reject branch must step it. */
    const t_order& qo = g_pre_modify[g_pre_modify_idx++];
    OrderState* s = find_order(m->order_id);
    if (s && s->alive) {
        check_qc_price(m->new_price_ticks, "engine_on_modify");
        cancel(s->qcid);                           /* cancel + reinsert */

        g_taker_id   = m->order_id;
        g_taker_left = m->new_quantity;
        g_taker_side = m->side;

        t_orderid qcid = limit(qo);

        if (g_taker_left > 0)
            *s = { m->new_price_ticks, g_taker_left, m->side, true, qcid };
        else
            s->alive = false;

        emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
                 m->side, m->new_price_ticks, m->new_quantity);
    } else {
        // Order not resting — a duplicate/stale modify. Answer with a reject.
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
    }
}

int64_t engine_query_best_bid(void) {
    int64_t best = INT64_MIN;
    for (const auto& st : g_orders)
        if (st.alive && st.side == 0 && st.price > best)
            best = st.price;
    return best;
}

int64_t engine_query_best_ask(void) {
    int64_t best = INT64_MAX;
    for (const auto& st : g_orders)
        if (st.alive && st.side == 1 && st.price < best)
            best = st.price;
    return best;
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    uint64_t total = 0;
    for (const auto& st : g_orders)
        if (st.alive && st.side == side && st.price == price_ticks)
            total += st.qty;
    return total;
}

}  // extern "C"
