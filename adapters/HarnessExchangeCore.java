/*
 * HarnessExchangeCore.java — Java side of the Exchange-core benchmark adapter.
 *
 * Exchange-core (https://github.com/exchange-core/exchange-core) is a Java
 * matching engine. The benchmark harness is a native program, so the adapter
 * is split in two: this class wraps exchange-core's OrderBookDirectImpl behind
 * a small per-message API, and exchange_core_adapter.cpp embeds a JVM (JNI)
 * and calls these methods to implement api/matching_engine_api.h.
 *
 * This class produces the trades; exchange_core_adapter.cpp turns them into the
 * report stream. onNew / onModify write one fill record per trade into the
 * adapter-owned staging buffer and return the fill count; onCancel stages the
 * cancelled order's price in record 0. The C++ side reads the buffer, builds
 * the Trade reports, and adds the OrderAck / CancelAck / ModifyAck (and
 * CancelReject / ModifyReject) reports. Modify is cancel + reinsert (see
 * onModify).
 *
 * The matching logic was verified to produce byte-identical report output —
 * the full six-type stream, not only its trades — to the other reference
 * engines on the harness's canonical workload. No exchange-core source is
 * patched; see docs/PATCHES.md.
 */
import exchange.core2.collections.objpool.ObjectsPool;
import exchange.core2.core.common.*;
import exchange.core2.core.common.cmd.CommandResultCode;
import exchange.core2.core.common.cmd.OrderCommand;
import exchange.core2.core.common.cmd.OrderCommandType;
import exchange.core2.core.common.config.LoggingConfiguration;
import exchange.core2.core.orderbook.IOrderBook;
import exchange.core2.core.orderbook.OrderBookDirectImpl;
import exchange.core2.core.orderbook.OrderBookEventsHelper;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public final class HarnessExchangeCore {

    /* One staging-buffer fill record (exchange_core_adapter.cpp StageTrade) is
     * 40 bytes, little-endian:
     *   sequence_number u64 @0, price_ticks i64 @8, quantity u32 @16,
     *   (pad @20), maker_order_id u64 @24, taker_order_id u64 @32.            */
    private static final int TRADE_SIZE = 40;

    /* A futures contract with no fees and no margin scaling — a pure matching
     * symbol, chosen so the engine's only work is order matching itself. */
    private static final CoreSymbolSpecification SYMBOL_SPEC =
            CoreSymbolSpecification.builder()
                    .symbolId(1)
                    .type(SymbolType.FUTURES_CONTRACT)
                    .baseCurrency(840)
                    .quoteCurrency(840)
                    .baseScaleK(1)
                    .quoteScaleK(1)
                    .marginBuy(1)
                    .marginSell(1)
                    .takerFee(0)
                    .makerFee(0)
                    .build();

    /* Single trader id. OrderBookDirectImpl matches without self-trade
     * prevention, so one uid for all orders does not change the trade output. */
    private static final long UID = 1001;

    private IOrderBook book;
    private final OrderCommand cmd = new OrderCommand();
    private ByteBuffer tradeBuf;          // adapter-owned direct staging buffer

    public HarnessExchangeCore() {
        book = newBook();
    }

    private static IOrderBook newBook() {
        return new OrderBookDirectImpl(
                SYMBOL_SPEC,
                ObjectsPool.createDefaultTestPool(),
                OrderBookEventsHelper.NON_POOLED_EVENTS_HELPER,
                LoggingConfiguration.DEFAULT);
    }

    /** The adapter hands over its staging buffer wrapped as a direct ByteBuffer. */
    public void setTradeBuffer(ByteBuffer bb) {
        tradeBuf = bb.order(ByteOrder.LITTLE_ENDIAN);
    }

    /** NEW order. Writes one fill record per trade into tradeBuf; returns the
     *  fill count. */
    public int onNew(long orderId, long seq, long price, int qty, int side, int ioc) {
        cmd.command         = OrderCommandType.PLACE_ORDER;
        cmd.orderId         = orderId;
        cmd.uid             = UID;
        cmd.price           = price;
        cmd.reserveBidPrice = price;
        cmd.size            = qty;
        cmd.action          = (side == 0) ? OrderAction.BID : OrderAction.ASK;
        cmd.orderType       = (ioc == 1) ? OrderType.IOC : OrderType.GTC;
        cmd.resultCode      = CommandResultCode.VALID_FOR_MATCHING_ENGINE;
        cmd.matcherEvent    = null;
        book.newOrder(cmd);
        return writeTrades(cmd.matcherEvent, orderId, seq);
    }

    /** CANCEL. Returns the cancelled order's side — 1 (bid) or 2 (ask) — with
     *  its price staged in fill record 0's price field, or 0 if the order was
     *  not resting. Both come from the engine itself: cancelOrder's native
     *  id-keyed lookup fills cmd.action with the removed order's side and
     *  attaches a REDUCE MatcherTradeEvent carrying its price, so the adapter
     *  needs no order state of its own to echo them. */
    public int onCancel(long orderId) {
        cmd.command      = OrderCommandType.CANCEL_ORDER;
        cmd.orderId      = orderId;
        cmd.uid          = UID;
        cmd.resultCode   = CommandResultCode.VALID_FOR_MATCHING_ENGINE;
        cmd.matcherEvent = null;
        if (book.cancelOrder(cmd) != CommandResultCode.SUCCESS) return 0;
        // Record 0's price field — the slot the C++ side reads as g_stage[0].price.
        tradeBuf.putLong(0 * TRADE_SIZE + 8, cmd.matcherEvent.price);
        return cmd.action == OrderAction.BID ? 1 : 2;
    }

    /** MODIFY, handled as cancel + reinsert — the harness modify rule
     *  (api/matching_engine_api.h). The order is removed and re-added at the
     *  new price/quantity, losing queue priority; this is byte-identical, on
     *  the canonical workload, to a native modify because every modify there is
     *  a reprice or a quantity increase. Writes one fill record per crossing
     *  trade into tradeBuf; returns the fill count, or -1 if the order was not
     *  resting (the adapter then emits a ModifyReject). */
    public int onModify(long orderId, long seq, long price, int qty, int side) {
        cmd.command      = OrderCommandType.CANCEL_ORDER;
        cmd.orderId      = orderId;
        cmd.uid          = UID;
        cmd.resultCode   = CommandResultCode.VALID_FOR_MATCHING_ENGINE;
        cmd.matcherEvent = null;
        if (book.cancelOrder(cmd) != CommandResultCode.SUCCESS)
            return -1;                 // not resting — adapter emits a ModifyReject
        return onNew(orderId, seq, price, qty, side, 0);
    }

    /* Walk exchange-core's matcher event chain; write one fill record per TRADE
     * event into the staging buffer. The only non-TRADE event a newOrder chain
     * can carry is REJECT (an IOC's unfilled residual — or a duplicate-id GTC,
     * which this workload never sends); it carries no trade and is skipped, and
     * the adapter derives the IOC-residual CancelAck itself. The trade price is
     * the resting (maker) order's price, as exchange-core reports it; this
     * matches the other reference engines.                                     */
    private int writeTrades(MatcherTradeEvent evt, long takerId, long seq) {
        int n = 0;
        for (; evt != null; evt = evt.nextEvent) {
            if (evt.eventType != MatcherEventType.TRADE) continue;
            int off = n * TRADE_SIZE;
            tradeBuf.putLong(off,      seq);                 // sequence_number
            tradeBuf.putLong(off + 8,  evt.price);           // price_ticks (maker)
            tradeBuf.putInt (off + 16, (int) evt.size);      // quantity
            tradeBuf.putLong(off + 24, evt.matchedOrderId);  // maker_order_id
            tradeBuf.putLong(off + 32, takerId);             // taker_order_id
            n++;
        }
        return n;
    }

    /* ---- Batch path --------------------------------------------------------
     * Process a run of messages in ONE JNI call, producing the full
     * me_report_t (64-byte) report stream directly into outB; the C++ side just
     * transports it. This amortizes the per-message JNI crossing the way the Go
     * adapters amortize cgo. inB wraps the harness's me_msg_t (40-byte) batch;
     * onBatch processes [start, n) until the input is exhausted or outB is near
     * full, returning (messagesConsumed << 32) | reportsWritten so the C++ side
     * can drain outB and resume. Report fields mirror emit_ack /
     * emit_staged_trades in exchange_core_adapter.cpp exactly. The per-message
     * onNew/onCancel/onModify above are untouched (the ME_NO_BATCH path). */
    private ByteBuffer inB, outB;
    public void setBatchIn(ByteBuffer in)   { inB  = in.order(ByteOrder.LITTLE_ENDIAN); }
    public void setBatchOut(ByteBuffer out)  { outB = out.order(ByteOrder.LITTLE_ENDIAN); }

    private int wc;                                  // me_report_t records written this call
    private void rep(int type, int side, long seq, long oid, long price, int qty,
                     long maker, long taker) {
        int o = wc << 6;                             // record size = 64 bytes
        outB.put(o,       (byte) type);
        outB.put(o + 1,   (byte) side);
        outB.putLong(o + 8,  seq);
        outB.putLong(o + 16, oid);
        outB.putLong(o + 24, price);
        outB.putInt (o + 32, qty);
        outB.putLong(o + 40, maker);
        outB.putLong(o + 48, taker);
        wc++;
    }
    private long repTrades(MatcherTradeEvent evt, long taker, long seq) {
        long filled = 0;
        for (; evt != null; evt = evt.nextEvent) {
            if (evt.eventType != MatcherEventType.TRADE) continue;
            rep(1 /*ME_TRADE*/, 0, seq, 0, evt.price, (int) evt.size,
                evt.matchedOrderId, taker);
            filled += evt.size;
        }
        return filled;
    }

    public long onBatch(int start, int n) {
        wc = 0;
        final int cap = outB.capacity() >> 6;        // record capacity
        int i = start;
        for (; i < n; i++) {
            if (cap - wc < 2048) break;              // reserve one message's worst case
            final int  b    = i * 40;                // me_msg_t stride
            final int  type = inB.get(b) & 0xff;
            final long oid  = inB.getLong(b + 8);
            final long seq  = inB.getLong(b + 16);
            if (type == 0) {                          // NEW
                final long price = inB.getLong(b + 24);
                final int  qty   = inB.getInt(b + 32);
                final int  side  = inB.get(b + 36) & 0xff;
                final int  ioc   = inB.get(b + 37) & 0xff;
                rep(0 /*ME_ORDER_ACK*/, side, seq, oid, price, qty, 0, 0);
                cmd.command         = OrderCommandType.PLACE_ORDER;
                cmd.orderId         = oid;   cmd.uid = UID;
                cmd.price           = price; cmd.reserveBidPrice = price;
                cmd.size            = qty;
                cmd.action          = (side == 0) ? OrderAction.BID : OrderAction.ASK;
                cmd.orderType       = (ioc == 1) ? OrderType.IOC : OrderType.GTC;
                cmd.resultCode      = CommandResultCode.VALID_FOR_MATCHING_ENGINE;
                cmd.matcherEvent    = null;
                book.newOrder(cmd);
                long filled = repTrades(cmd.matcherEvent, oid, seq);
                if (ioc == 1 && filled < qty)
                    rep(2 /*ME_CANCEL_ACK*/, side, seq, oid, price,
                        (int) (qty - filled), 0, 0);
            } else if (type == 1) {                   // CANCEL
                cmd.command      = OrderCommandType.CANCEL_ORDER;
                cmd.orderId      = oid; cmd.uid = UID;
                cmd.resultCode   = CommandResultCode.VALID_FOR_MATCHING_ENGINE;
                cmd.matcherEvent = null;
                if (book.cancelOrder(cmd) != CommandResultCode.SUCCESS)
                    rep(4 /*ME_CANCEL_REJECT*/, 0, seq, oid, 0, 0, 0, 0);
                else
                    rep(2 /*ME_CANCEL_ACK*/, cmd.action == OrderAction.BID ? 0 : 1,
                        seq, oid, cmd.matcherEvent.price, 0, 0, 0);
            } else {                                  // MODIFY = cancel + reinsert
                final long nprice = inB.getLong(b + 24);
                final int  nqty   = inB.getInt(b + 32);
                final int  side   = inB.get(b + 36) & 0xff;
                cmd.command      = OrderCommandType.CANCEL_ORDER;
                cmd.orderId      = oid; cmd.uid = UID;
                cmd.resultCode   = CommandResultCode.VALID_FOR_MATCHING_ENGINE;
                cmd.matcherEvent = null;
                if (book.cancelOrder(cmd) != CommandResultCode.SUCCESS) {
                    rep(5 /*ME_MODIFY_REJECT*/, 0, seq, oid, 0, 0, 0, 0);
                    continue;
                }
                cmd.command         = OrderCommandType.PLACE_ORDER;
                cmd.orderId         = oid;    cmd.uid = UID;
                cmd.price           = nprice; cmd.reserveBidPrice = nprice;
                cmd.size            = nqty;
                cmd.action          = (side == 0) ? OrderAction.BID : OrderAction.ASK;
                cmd.orderType       = OrderType.GTC;
                cmd.resultCode      = CommandResultCode.VALID_FOR_MATCHING_ENGINE;
                cmd.matcherEvent    = null;
                book.newOrder(cmd);
                repTrades(cmd.matcherEvent, oid, seq);
                rep(3 /*ME_MODIFY_ACK*/, side, seq, oid, nprice, nqty, 0, 0);
            }
        }
        return ((long) i << 32) | (wc & 0xffffffffL);
    }

    /** Best (highest) bid in ticks, or Long.MIN_VALUE if there are no bids. */
    public long bestBid() {
        L2MarketData d = book.getL2MarketDataSnapshot(1);
        return d.bidSize > 0 ? d.bidPrices[0] : Long.MIN_VALUE;
    }

    /** Best (lowest) ask in ticks, or Long.MAX_VALUE if there are no asks. */
    public long bestAsk() {
        L2MarketData d = book.getL2MarketDataSnapshot(1);
        return d.askSize > 0 ? d.askPrices[0] : Long.MAX_VALUE;
    }

    /** Aggregated resting quantity at one price level (0 if the level is empty). */
    public long depthAt(long price, int side) {
        L2MarketData d = book.getL2MarketDataSnapshot(Integer.MAX_VALUE);
        if (side == 0) {
            for (int i = 0; i < d.bidSize; i++)
                if (d.bidPrices[i] == price) return d.bidVolumes[i];
        } else {
            for (int i = 0; i < d.askSize; i++)
                if (d.askPrices[i] == price) return d.askVolumes[i];
        }
        return 0;
    }

    /* Exchange-core is JIT-compiled and the harness runs a single measured
     * pass, so the adapter warms the hot path here, during engine_init (which
     * the harness does not time) — the way exchange-core's own benchmark uses
     * explicit warmup passes. The warmed book is then discarded and a fresh one
     * installed, so warmup leaves no state behind. */
    public void warmup() {
        ByteBuffer scratch = ByteBuffer.allocateDirect(64 * TRADE_SIZE)
                                       .order(ByteOrder.LITTLE_ENDIAN);
        ByteBuffer saved = tradeBuf;
        tradeBuf = scratch;
        long id = 1;
        final long mid = 100_000;
        for (int i = 0; i < 100_000; i++) {
            long restBid = id++;
            onNew(restBid, 0, mid - 1, 10, 0, 0);   // resting buy
            long restAsk = id++;
            onNew(restAsk, 0, mid + 1, 10, 1, 0);   // resting sell
            onModify(restBid, 0, mid - 2, 12, 0);   // reprice the resting buy
            // Queries while BOTH sides are populated, so the snapshot walk and
            // the depthAt scan-hit branch get JIT profiles (not just the
            // empty-book early-outs).
            if ((i & 15) == 0) { bestBid(); bestAsk(); depthAt(mid - 2, 0); }
            onNew(id++,    0, mid + 1,  7, 0, 0);   // crossing buy -> one trade
            onNew(id++,    0, mid + 3, 20, 0, 1);   // IOC buy: fills the ask's
                                                    //   residual 3, rejects 17 ->
                                                    //   warms the workload's IOC
                                                    //   + REJECT-skip path
            onCancel(restBid);                       // live cancel (side+price echo)
            onCancel(restAsk);                       // consumed above -> miss path
            onModify(restAsk, 0, mid + 2, 9, 1);     // stale modify -> miss path
        }
        book = newBook();      // discard the warmed book; start the run clean
        tradeBuf = saved;
    }
}
