/*
 * HarnessJiang.java — Java side of the FastMatchingEngine benchmark adapter.
 *
 * FastMatchingEngine (https://github.com/JiangYongKang/FastMatchingEngine) is a
 * dependency-free Java digital-currency matching engine POC: a price-time-
 * priority CLOB with a TreeMap<BigDecimal,OrderBucket> per side (ask = natural
 * order, bid = reverse order), each bucket a LinkedHashMap<Long,Order> (FIFO
 * time priority), plus a HashMap<Long,Order> id index. The benchmark harness is
 * a native program, so the adapter is split in two: this class drives a single
 * com.fast.matching.engine.OrderBook through a small per-message API, and
 * jiang_adapter.cpp embeds a JVM (JNI) and calls these methods to implement
 * api/matching_engine_api.h.
 *
 * This class produces the trades; jiang_adapter.cpp turns them into the report
 * stream. OrderBook.newOrder() returns crossing Trades whose targetOrderId is
 * the maker (resting) order, sourceOrderId the taker (aggressor), and
 * commissionPrice the maker's resting price — exactly the harness Trade
 * semantics. onNew / onModify stage one fill record per trade into the adapter-
 * owned buffer and return the fill count; onCancel stages the cancelled order's
 * price + side. The C++ side reads the buffer, builds the Trade reports, and
 * adds the OrderAck / CancelAck / ModifyAck (and CancelReject / ModifyReject)
 * reports. Modify is cancel + reinsert (the harness rule).
 *
 * Native API used (minimal shim, no re-implemented matching):
 *   - OrderBook.newOrder()           — the engine's own price-time matcher.
 *   - OrderBook.cancelOrder()        — the engine's own removal (post-fix).
 *   - OrderBook.getOrder()           — id-keyed liveness/price/side lookup
 *                                       (an observation-only accessor added by
 *                                       the build.sh PATCH 3 over the private
 *                                       idMaps; see below).
 *   - OrderBook.askOrderBucket()/bidOrderBucket() for the audit queries.
 *
 * Self-trade: the engine suppresses a fill when source.uid().equals(target.uid())
 * (OrderBucket.doExchange). The harness has no notion of a user and every order
 * must be able to match every other, so each order is given a UNIQUE uid (its
 * own order id) — distinct uids never collide, so matching is never suppressed.
 *
 * IOC: the engine has no IOC type — newOrder() always rests any residual. For an
 * IOC order this class matches, then pulls the rested residual back out so the
 * book never holds it; the C++ side emits the residual CancelAck from filled<qty.
 *
 * ENGINE PATCHES (applied idempotently in build.sh, documented there):
 *   1) OrderBook.cancelOrder(): add idMaps.remove(id) — real bug FIX (the
 *      cancelled id was never freed, so a reinsert was dropped and the next
 *      cancel NPE'd; this is the modify path).
 *   2) OrderBook.newOrder() match loop: prune idMaps for a maker that is fully
 *      consumed — real bug FIX (the same burned-id defect on the match path; a
 *      filled maker stayed in idMaps and a later cancel of it NPE'd).
 *   3) OrderBook.getOrder(): add a one-line read-only accessor over idMaps so
 *      this class can ask the engine whether an id is resting (and read its
 *      price/side) instead of keeping a parallel adapter shadow. No matching
 *      logic is touched by any patch.
 */
import com.fast.matching.engine.Order;
import com.fast.matching.engine.OrderAction;
import com.fast.matching.engine.OrderBook;
import com.fast.matching.engine.OrderBucket;
import com.fast.matching.engine.Trade;

import java.math.BigDecimal;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.List;

public final class HarnessJiang {

    /* One staging-buffer fill record (jiang_adapter.cpp StageTrade) is 40 bytes,
     * little-endian:
     *   sequence_number u64 @0, price_ticks i64 @8, quantity u32 @16,
     *   (pad @20), maker_order_id u64 @24, taker_order_id u64 @32.            */
    private static final int TRADE_SIZE = 40;

    /* Fill-record capacity of the adapter's staging buffer — MUST match
     * jiang_adapter.cpp STAGE_CAP (the C++ side sizes the direct ByteBuffer this
     * class writes fills into). Sized to exceed the deepest single-order sweep
     * the harness can deliver (conformance deep_recursive_sweep_5000 = 5000 fills
     * in one message); 4096 overflowed it. */
    private static final int STAGE_CAP = 8192;

    /* Harness side: 0 = buy, 1 = sell. Engine: BID = buy, ASK = sell. */
    private static OrderAction action(int side) {
        return side == 0 ? OrderAction.BID : OrderAction.ASK;
    }

    private OrderBook book;
    private ByteBuffer tradeBuf;          // adapter-owned direct staging buffer

    public HarnessJiang() {
        book = new OrderBook();
    }

    /** The adapter hands over its staging buffer wrapped as a direct ByteBuffer. */
    public void setTradeBuffer(ByteBuffer bb) {
        tradeBuf = bb.order(ByteOrder.LITTLE_ENDIAN);
    }

    /* ---- per-message API -------------------------------------------------- */

    /** NEW order. Writes one fill record per trade into tradeBuf; returns the
     *  fill count. The order's uid is its own id (unique) so the engine never
     *  suppresses a fill as a self-trade. For an IOC order any rested residual
     *  is pulled back out so the book never holds it. */
    public int onNew(long orderId, long seq, long price, int qty, int side, int ioc) {
        // newOrder() returns the engine's cumulative trade list; take the slice
        // produced by THIS call via the size delta.
        int before = book.trades().size();
        book.newOrder(orderId, orderId, BigDecimal.valueOf(price), BigDecimal.valueOf(qty),
                      action(side), seq);
        List<Trade> all = book.trades();
        int after = all.size();
        int n = after - before;
        for (int k = 0; k < n; k++) {
            Trade t = all.get(before + k);
            int off = k * TRADE_SIZE;
            tradeBuf.putLong(off,      seq);
            tradeBuf.putLong(off + 8,  t.commissionPrice().longValueExact());
            tradeBuf.putInt (off + 16, t.commissionVolume().intValueExact());
            tradeBuf.putLong(off + 24, t.targetOrderId());
            tradeBuf.putLong(off + 32, t.sourceOrderId());
        }
        if (ioc == 1) {
            // IOC never rests: if the residual rested (engine recorded it in the
            // id index), remove it from the book so nothing is left standing.
            Order resting = book.getOrder(orderId);
            if (resting != null) book.cancelOrder(orderId);
        }
        return n;
    }

    /** CANCEL. Returns 1 if the order was resting and is now cancelled (its price
     *  staged in record 0's price field, its side bit available via
     *  lastCancelSide()), or 0 if the order was not resting. Uses the engine's
     *  own id index (getOrder) — no adapter-side order state. */
    public int onCancel(long orderId) {
        Order o = book.getOrder(orderId);
        if (o == null) return 0;                  // not resting -> adapter CancelReject
        lastCancelPrice   = o.commissionPrice().longValueExact();
        lastCancelSideBit = (o.action() == OrderAction.BID) ? 0 : 1;
        tradeBuf.putLong(0 * TRADE_SIZE + 8, lastCancelPrice);  // g_stage[0].price
        book.cancelOrder(orderId);
        return 1;
    }

    private long lastCancelPrice;
    private int  lastCancelSideBit;       // 0 = buy, 1 = sell

    /** The side bit (0 buy / 1 sell) of the order most recently cancelled by
     *  onCancel — the C++ side reads it for the CancelAck. */
    public int lastCancelSide() {
        return lastCancelSideBit;
    }

    /** MODIFY, handled as cancel + reinsert — the harness modify rule. The order
     *  is removed and re-added at the new price/quantity, losing queue priority.
     *  Writes one fill record per crossing trade into tradeBuf; returns the fill
     *  count, or -1 if the order was not resting (the adapter emits a
     *  ModifyReject). */
    public int onModify(long orderId, long seq, long price, int qty, int side) {
        Order o = book.getOrder(orderId);
        if (o == null) return -1;                 // not resting -> ModifyReject
        book.cancelOrder(orderId);                // remove; no trades, no report
        return onNew(orderId, seq, price, qty, side, 0);   // reinsert (rests/crosses)
    }

    /* ---- batch path (api/matching_engine_api.h: engine_on_batch) -----------
     * Process a RUN of messages in ONE JNI crossing instead of one crossing per
     * message. inB wraps the harness's me_msg_t array (40-byte stride, tag at 0,
     * payload at 8); outB is the adapter's report buffer, into which this side
     * writes the COMPLETE me_report_t (64-byte) stream — the acks that
     * jiang_adapter.cpp's emit_ack() would have written, interleaved with the
     * trades in exactly the same order — so the C++ side only has to transport
     * it. That batches the outbound direction too: no per-report boundary work.
     *
     * The loop calls the SAME onNew / onCancel / onModify handlers the
     * per-message path calls, in array order, with NO lookahead — message i is
     * fully matched and its reports written before message i+1 is read. The
     * report stream is therefore byte-identical to per-message delivery.        */
    private ByteBuffer inB, outB;
    public void setBatchIn (ByteBuffer in)  { inB  = in.order(ByteOrder.LITTLE_ENDIAN); }
    public void setBatchOut(ByteBuffer out) { outB = out.order(ByteOrder.LITTLE_ENDIAN); }

    private static final int MSG_SIZE = 40;   // me_msg_t stride
    private static final int REP_SHIFT = 6;   // me_report_t = 64 bytes
    /* Worst case one message can emit: an ack + a full staging buffer of fills
     * + an IOC-residual CancelAck. */
    private static final int REP_RESERVE = STAGE_CAP + 2;

    private int wc;                           // me_report_t records written this call

    /* One me_report_t. Field offsets mirror the struct in matching_engine_api.h:
     * type@0, side@1, sequence_number@8, order_id@16, price_ticks@24,
     * quantity@32, maker_order_id@40, taker_order_id@48. The reserved bytes are
     * never written — the C++ side zeroes g_outbuf once, so they stay zero,
     * matching the value-initialized me_report_t of the per-message path. */
    private void rep(int type, int side, long seq, long oid, long price, int qty,
                     long maker, long taker) {
        int o = wc << REP_SHIFT;
        outB.put    (o,      (byte) type);
        outB.put    (o + 1,  (byte) side);
        outB.putLong(o + 8,  seq);
        outB.putLong(o + 16, oid);
        outB.putLong(o + 24, price);
        outB.putInt (o + 32, qty);
        outB.putLong(o + 40, maker);
        outB.putLong(o + 48, taker);
        wc++;
    }

    /* Turn the n fills a handler just staged in tradeBuf into Trade reports —
     * the same conversion emit_staged_trades() does natively. Returns the total
     * filled quantity (the IOC-residual test). */
    private long repStagedTrades(int n) {
        long filled = 0;
        for (int k = 0; k < n; k++) {
            int o = k * TRADE_SIZE;
            long tseq   = tradeBuf.getLong(o);
            long tprice = tradeBuf.getLong(o + 8);
            int  tqty   = tradeBuf.getInt (o + 16);
            long maker  = tradeBuf.getLong(o + 24);
            long taker  = tradeBuf.getLong(o + 32);
            rep(1 /*ME_TRADE*/, 0, tseq, 0, tprice, tqty, maker, taker);
            filled += tqty;
        }
        return filled;
    }

    /** Process messages [start, n) of the batch wrapped in inB, stopping early
     *  if outB has no room left for one more message's worst case. Returns
     *  (messagesConsumed << 32) | reportsWritten. */
    public long onBatch(int start, int n) {
        wc = 0;
        final int cap = outB.capacity() >> REP_SHIFT;
        int i = start;
        for (; i < n; i++) {
            if (cap - wc < REP_RESERVE) break;          // drain and resume
            final int  b    = i * MSG_SIZE;
            final int  type = inB.get(b) & 0xff;
            final long oid  = inB.getLong(b + 8);
            final long seq  = inB.getLong(b + 16);
            if (type == 0) {                            // NEW
                final long price = inB.getLong(b + 24);
                final int  qty   = inB.getInt (b + 32);
                final int  side  = inB.get(b + 36) & 0xff;
                final int  ioc   = inB.get(b + 37) & 0xff;
                rep(0 /*ME_ORDER_ACK*/, side, seq, oid, price, qty, 0, 0);
                int k = onNew(oid, seq, price, qty, side, ioc);   // same handler
                long filled = repStagedTrades(k);
                if (ioc == 1 && filled < qty)           // IOC residual cancellation
                    rep(2 /*ME_CANCEL_ACK*/, side, seq, oid, price,
                        (int) (qty - filled), 0, 0);
            } else if (type == 1) {                     // CANCEL
                if (onCancel(oid) != 0)                 // same handler
                    rep(2 /*ME_CANCEL_ACK*/, lastCancelSide(), seq, oid,
                        lastCancelPrice, 0, 0, 0);
                else
                    rep(4 /*ME_CANCEL_REJECT*/, 0, seq, oid, 0, 0, 0, 0);
            } else {                                    // MODIFY = cancel + reinsert
                final long nprice = inB.getLong(b + 24);
                final int  nqty   = inB.getInt (b + 32);
                final int  side   = inB.get(b + 36) & 0xff;
                int k = onModify(oid, seq, nprice, nqty, side);   // same handler
                if (k < 0) {                            // not resting -> reject
                    rep(5 /*ME_MODIFY_REJECT*/, 0, seq, oid, 0, 0, 0, 0);
                    continue;
                }
                repStagedTrades(k);
                rep(3 /*ME_MODIFY_ACK*/, side, seq, oid, nprice, nqty, 0, 0);
            }
        }
        return ((long) i << 32) | (wc & 0xffffffffL);
    }

    /* ---- queries ---------------------------------------------------------- */

    /** Best (highest) bid in ticks, or Long.MIN_VALUE if there are no bids. The
     *  bid map is reverse-ordered, so its first key is the highest bid. */
    public long bestBid() {
        java.util.SortedMap<BigDecimal, OrderBucket> bids = book.bidOrderBucket();
        return bids.isEmpty() ? Long.MIN_VALUE : bids.firstKey().longValueExact();
    }

    /** Best (lowest) ask in ticks, or Long.MAX_VALUE if there are no asks. The
     *  ask map is natural-ordered, so its first key is the lowest ask. */
    public long bestAsk() {
        java.util.SortedMap<BigDecimal, OrderBucket> asks = book.askOrderBucket();
        return asks.isEmpty() ? Long.MAX_VALUE : asks.firstKey().longValueExact();
    }

    /** Aggregated resting quantity at one price level (0 if the level is empty). */
    public long depthAt(long price, int side) {
        java.util.SortedMap<BigDecimal, OrderBucket> map =
                (side == 0) ? book.bidOrderBucket() : book.askOrderBucket();
        OrderBucket bucket = map.get(BigDecimal.valueOf(price));
        return bucket == null ? 0 : bucket.volume().longValueExact();
    }

    /** Warm the JIT-compiled hot path during engine_init (untimed), the way the
     *  CoralME / CoinTossX adapters warm before measuring. Exercises new/modify/
     *  cancel, both match and rest, IOC residual, the miss paths, and the
     *  queries, then discards the warmed book so the run starts clean. */
    public void warmup() {
        ByteBuffer scratch = ByteBuffer.allocateDirect(STAGE_CAP * TRADE_SIZE)
                                       .order(ByteOrder.LITTLE_ENDIAN);
        ByteBuffer savedTrade = tradeBuf, savedIn = inB, savedOut = outB;
        tradeBuf = scratch;
        long id = 1;
        final long mid = 100_000;
        for (int i = 0; i < 50_000; i++) {
            long restBid = id++;
            onNew(restBid, 0, mid - 1, 10, 0, 0);    // resting buy
            long restAsk = id++;
            onNew(restAsk, 0, mid + 1, 10, 1, 0);    // resting sell
            onModify(restBid, 0, mid - 2, 12, 0);    // reprice the resting buy
            if ((i & 15) == 0) { bestBid(); bestAsk(); depthAt(mid - 2, 0); }
            onNew(id++, 0, mid - 2,  7, 1, 0);       // crossing sell -> one trade
            onNew(id++, 0, mid + 3, 20, 0, 1);       // IOC buy: partial + residual
            onCancel(restBid);                        // live cancel (side+price echo)
            onCancel(restAsk);                        // consumed above -> miss path
            onModify(restAsk, 0, mid + 2, 9, 1);      // stale modify -> miss path
        }

        // Warm the BATCH loop too — the onBatch dispatch, the inB decode and the
        // rep()/repStagedTrades() report writes are hot-path code on that arm and
        // would otherwise compile inside the measured window. Same op mix as
        // above, driven through onBatch off a synthetic me_msg_t buffer, so every
        // report kind (ack / trade / IOC-residual / cancel-ack / cancel-reject /
        // modify-ack / modify-reject) is exercised.
        book = new OrderBook();
        final int WMSGS = 256;
        ByteBuffer win  = ByteBuffer.allocateDirect(WMSGS * MSG_SIZE)
                                    .order(ByteOrder.LITTLE_ENDIAN);
        ByteBuffer wout = ByteBuffer.allocateDirect(16384 << REP_SHIFT)
                                    .order(ByteOrder.LITTLE_ENDIAN);
        inB = win; outB = wout;
        for (int r = 0; r < 400; r++) {
            win.clear();
            for (int j = 0; j + 8 <= WMSGS; j += 8) {
                long restBid = id++, restAsk = id++;
                putMsg(win, j,     0, restBid, 0, mid - 1, 10, 0, 0);  // resting buy
                putMsg(win, j + 1, 0, restAsk, 0, mid + 1, 10, 1, 0);  // resting sell
                putMsg(win, j + 2, 2, restBid, 0, mid - 2, 12, 0, 0);  // reprice
                putMsg(win, j + 3, 0, id++,    0, mid - 2,  7, 1, 0);  // crossing sell
                putMsg(win, j + 4, 0, id++,    0, mid + 3, 20, 0, 1);  // IOC partial
                putMsg(win, j + 5, 1, restBid, 0, 0,        0, 0, 0);  // live cancel
                putMsg(win, j + 6, 1, restAsk, 0, 0,        0, 0, 0);  // miss -> reject
                putMsg(win, j + 7, 2, restAsk, 0, mid + 2,  9, 1, 0);  // miss -> reject
            }
            int start = 0;
            while (start < WMSGS) {
                long packed = onBatch(start, WMSGS);
                int consumed = (int) (packed >>> 32);
                if (consumed <= start) break;
                start = consumed;
            }
        }

        book = new OrderBook();   // discard the warmed book; start the run clean
        tradeBuf = savedTrade; inB = savedIn; outB = savedOut;
    }

    /** Write one me_msg_t (tag at 0, payload at 8) into a warm-up batch buffer. */
    private static void putMsg(ByteBuffer b, int i, int type, long oid, long seq,
                               long price, int qty, int side, int ioc) {
        int o = i * MSG_SIZE;
        for (int k = 0; k < MSG_SIZE; k += 8) b.putLong(o + k, 0L);
        b.put    (o,      (byte) type);
        b.putLong(o + 8,  oid);
        b.putLong(o + 16, seq);
        if (type == 1) return;                      // cancel_t stops here
        b.putLong(o + 24, price);
        b.putInt (o + 32, qty);
        b.put    (o + 36, (byte) side);
        if (type == 0) b.put(o + 37, (byte) ioc);   // new_order_t only
    }
}
