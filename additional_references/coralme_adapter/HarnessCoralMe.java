/*
 * HarnessCoralMe.java — Java side of the CoralME benchmark adapter.
 *
 * CoralME (https://github.com/coralblocks/CoralME) is a garbage-free Java
 * matching engine: a doubly-linked list of price levels (price-ordered), a FIFO
 * order list per level (time priority), and a LongMap id index, all backed by
 * object pools. The benchmark harness is a native program, so the adapter is
 * split in two: this class wraps a CoralME OrderBook behind a small per-message
 * API, and coralme_adapter.cpp embeds a JVM (JNI) and calls these methods to
 * implement api/matching_engine_api.h.
 *
 * This class produces the trades; coralme_adapter.cpp turns them into the report
 * stream. It registers itself as the book's OrderBookListener and, during each
 * matching call, captures one fill record per MAKER-side execution into the
 * adapter-owned staging buffer (CoralME fires onOrderExecuted twice per fill —
 * once MAKER, once TAKER — both with the maker's resting price; capturing the
 * MAKER side gives the maker's id and price directly, and the taker is the
 * order currently aggressing). onNew / onModify return the fill count; onCancel
 * stages the cancelled order's price + side. The C++ side reads the buffer,
 * builds the Trade reports, and adds the OrderAck / CancelAck / ModifyAck (and
 * CancelReject / ModifyReject) reports. Modify is cancel + reinsert (the harness
 * rule); the per-message API is byte-identical to the C++ liquibook / Java
 * exchange-core baselines on the canonical workload.
 *
 * No CoralME source is patched.
 */
import com.coralblocks.coralme.Order;
import com.coralblocks.coralme.Order.CancelReason;
import com.coralblocks.coralme.Order.ExecuteSide;
import com.coralblocks.coralme.Order.RejectReason;
import com.coralblocks.coralme.Order.Side;
import com.coralblocks.coralme.Order.TimeInForce;
import com.coralblocks.coralme.OrderBook;
import com.coralblocks.coralme.OrderBookListener;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public final class HarnessCoralMe implements OrderBookListener {

    /* One staging-buffer fill record (coralme_adapter.cpp StageTrade) is
     * 40 bytes, little-endian:
     *   sequence_number u64 @0, price_ticks i64 @8, quantity u32 @16,
     *   (pad @20), maker_order_id u64 @24, taker_order_id u64 @32.            */
    private static final int TRADE_SIZE = 40;

    /* A single security and a single client id. CoralME allows trade-to-self by
     * default, so one client id for every order does not change the matching or
     * the trade output — exactly as the exchange-core baseline uses one uid. */
    private static final String SECURITY = "BENCH";
    private static final long   CLIENT_ID = 1001L;
    /* A fixed, non-empty clientOrderId — CoralME copies it into a per-order
     * StringBuilder; the value never reaches the report stream. */
    private static final String CLOID = "x";

    private OrderBook book;
    private ByteBuffer tradeBuf;          // adapter-owned direct staging buffer

    /* Per-call matching state, set before each createLimit so the listener can
     * label the trade it captures. */
    private long curSeq;                  // aggressive order's sequence number
    private long curTakerId;              // aggressive order's id
    private int  tradeCount;              // MAKER executions captured this call

    /* Cancel echo: the resting order's price + side, read by the C++ side from
     * record 0 after onCancel/onModify-cancel. */
    private long lastCancelPrice;
    private int  lastCancelSideBit;       // 0 = buy, 1 = sell

    public HarnessCoralMe() {
        book = newBook();
    }

    private OrderBook newBook() {
        OrderBook b = new OrderBook(SECURITY);   // allowTradeToSelf = true
        b.addListener(this);
        return b;
    }

    /** The adapter hands over its staging buffer wrapped as a direct ByteBuffer. */
    public void setTradeBuffer(ByteBuffer bb) {
        tradeBuf = bb.order(ByteOrder.LITTLE_ENDIAN);
    }

    /* ---- OrderBookListener: capture trades on the MAKER side only --------- */

    @Override
    public void onOrderExecuted(OrderBook ob, long time, Order order, ExecuteSide execSide,
                                long execSize, long execPrice, long execId, long matchId) {
        // CoralME executes the maker first, then the taker, for every fill, both
        // with the maker's resting price. Capture exactly the MAKER leg so each
        // fill yields one Trade, in match order. order.getId() on the MAKER leg
        // is the resting (maker) order's id; curTakerId is the aggressor.
        if (execSide != ExecuteSide.MAKER) return;
        if (batchMode) {
            // Write the Trade record straight into the batch output buffer.
            rep(1 /*ME_TRADE*/, 0, curSeq, 0, execPrice, (int) execSize,
                order.getId(), curTakerId);
            batchFilled += execSize;
        } else {
            int off = tradeCount * TRADE_SIZE;
            tradeBuf.putLong(off,      curSeq);          // sequence_number (aggressor)
            tradeBuf.putLong(off + 8,  execPrice);       // price_ticks (maker resting)
            tradeBuf.putInt (off + 16, (int) execSize);  // quantity
            tradeBuf.putLong(off + 24, order.getId());   // maker_order_id (this leg)
            tradeBuf.putLong(off + 32, curTakerId);      // taker_order_id (aggressor)
            tradeCount++;
        }
    }

    @Override public void onOrderReduced(OrderBook ob, long t, Order o, long cs, long ns) {}
    @Override public void onOrderCanceled(OrderBook ob, long t, Order o, long cs, CancelReason r) {}
    @Override public void onOrderAccepted(OrderBook ob, long t, Order o) {}
    @Override public void onOrderRejected(OrderBook ob, long t, Order o, RejectReason r) {}
    @Override public void onOrderRested(OrderBook ob, long t, Order o, long rs, long rp) {}
    @Override public void onOrderTerminated(OrderBook ob, long t, Order o) {}

    /* ---- per-message API -------------------------------------------------- */

    /** NEW order. Writes one fill record per trade into tradeBuf; returns the
     *  fill count. Prices are the harness's signed integer ticks, passed through
     *  as CoralME's opaque comparable long price. */
    public int onNew(long orderId, long seq, long price, int qty, int side, int ioc) {
        curSeq     = seq;
        curTakerId = orderId;
        tradeCount = 0;
        Side s = (side == 0) ? Side.BUY : Side.SELL;
        TimeInForce tif = (ioc == 1) ? TimeInForce.IOC : TimeInForce.GTC;
        book.createLimit(CLIENT_ID, CLOID, orderId, s, qty, price, tif);
        return tradeCount;
    }

    /** CANCEL. Returns 1 if the order was resting and is now cancelled (its
     *  price staged in record 0's price field, its side bit available via
     *  lastCancelSide()), or 0 if the order was not resting. Uses CoralME's own
     *  id index (getOrder) — no adapter-side order state. */
    public int onCancel(long orderId) {
        Order o = book.getOrder(orderId);
        if (o == null || !o.isResting()) return 0;
        lastCancelPrice   = o.getPrice();
        lastCancelSideBit = (o.getSide() == Side.BUY) ? 0 : 1;
        tradeBuf.putLong(0 * TRADE_SIZE + 8, lastCancelPrice);  // g_stage[0].price
        o.cancel(CancelReason.USER);   // book removes it via the listener chain
        return 1;
    }

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
        if (o == null || !o.isResting()) return -1;
        o.cancel(CancelReason.USER);                 // remove; no trades, no report
        return onNew(orderId, seq, price, qty, side, 0);   // reinsert as GTC
    }

    /* ---- queries ---------------------------------------------------------- */

    /** Best (highest) bid in ticks, or Long.MIN_VALUE if there are no bids. */
    public long bestBid() {
        return book.hasBids() ? book.getBestBidPrice() : Long.MIN_VALUE;
    }

    /** Best (lowest) ask in ticks, or Long.MAX_VALUE if there are no asks. */
    public long bestAsk() {
        return book.hasAsks() ? book.getBestAskPrice() : Long.MAX_VALUE;
    }

    /** Aggregated resting quantity at one price level (0 if the level is empty).
     *  Sums the open size of every resting order at (price, side) via CoralME's
     *  public id index — depthAt is a sparse audit query, never on the hot path,
     *  so a public-API walk is fine and keeps the engine unpatched. */
    public long depthAt(long price, int side) {
        Side s = (side == 0) ? Side.BUY : Side.SELL;
        long total = 0;
        for (Order o : book.getOrders()) {
            if (o.getSide() == s && o.getPrice() == price) total += o.getOpenSize();
        }
        return total;
    }

    /* ---- batch path: process a run of messages in ONE JNI call ------------ *
     * Produces the full me_report_t (64-byte) report stream directly into outB;
     * the C++ side just transports it. Amortizes the per-message JNI crossing
     * the way the Go adapters amortize cgo. inB wraps the harness's me_msg_t
     * (40-byte) batch; onBatch processes [start, n) until the input is exhausted
     * or outB is near full, returning (messagesConsumed << 32) | reportsWritten.
     * Report fields mirror emit_ack / emit_staged_trades exactly; the per-message
     * onNew/onCancel/onModify above are untouched. */
    private ByteBuffer inB, outB;
    public void setBatchIn(ByteBuffer in)   { inB  = in.order(ByteOrder.LITTLE_ENDIAN); }
    public void setBatchOut(ByteBuffer out) { outB = out.order(ByteOrder.LITTLE_ENDIAN); }

    /* Batch-mode trade staging: the listener writes me_report_t Trade records
     * straight into outB when batchMode is on, so no per-fill copy step. */
    private boolean batchMode = false;
    private int     wc;                            // me_report_t records written

    private void rep(int type, int sideBit, long seq, long oid, long price, int qty,
                     long maker, long taker) {
        int o = wc << 6;                            // record size = 64 bytes
        outB.put(o,        (byte) type);
        outB.put(o + 1,    (byte) sideBit);
        outB.putLong(o + 8,  seq);
        outB.putLong(o + 16, oid);
        outB.putLong(o + 24, price);
        outB.putInt (o + 32, qty);
        outB.putLong(o + 40, maker);
        outB.putLong(o + 48, taker);
        wc++;
    }

    public long onBatch(int start, int n) {
        wc = 0;
        batchMode = true;
        final int cap = outB.capacity() >> 6;       // record capacity
        int i = start;
        for (; i < n; i++) {
            if (cap - wc < 2048) break;             // reserve one message's worst case
            final int  b    = i * 40;               // me_msg_t stride
            final int  type = inB.get(b) & 0xff;
            final long oid  = inB.getLong(b + 8);
            final long seq  = inB.getLong(b + 16);
            if (type == 0) {                         // NEW
                final long price = inB.getLong(b + 24);
                final int  qty   = inB.getInt(b + 32);
                final int  side  = inB.get(b + 36) & 0xff;
                final int  ioc   = inB.get(b + 37) & 0xff;
                rep(0 /*ME_ORDER_ACK*/, side, seq, oid, price, qty, 0, 0);
                long filled = matchNew(oid, seq, price, qty, side, ioc);
                if (ioc == 1 && filled < qty)
                    rep(2 /*ME_CANCEL_ACK*/, side, seq, oid, price,
                        (int) (qty - filled), 0, 0);
            } else if (type == 1) {                  // CANCEL
                Order o = book.getOrder(oid);
                if (o == null || !o.isResting()) {
                    rep(4 /*ME_CANCEL_REJECT*/, 0, seq, oid, 0, 0, 0, 0);
                } else {
                    long cp = o.getPrice();
                    int  cs = (o.getSide() == Side.BUY) ? 0 : 1;
                    o.cancel(CancelReason.USER);
                    rep(2 /*ME_CANCEL_ACK*/, cs, seq, oid, cp, 0, 0, 0);
                }
            } else {                                 // MODIFY = cancel + reinsert
                final long nprice = inB.getLong(b + 24);
                final int  nqty   = inB.getInt(b + 32);
                final int  side   = inB.get(b + 36) & 0xff;
                Order o = book.getOrder(oid);
                if (o == null || !o.isResting()) {
                    rep(5 /*ME_MODIFY_REJECT*/, 0, seq, oid, 0, 0, 0, 0);
                    continue;
                }
                o.cancel(CancelReason.USER);
                matchNew(oid, seq, nprice, nqty, side, 0);
                rep(3 /*ME_MODIFY_ACK*/, side, seq, oid, nprice, nqty, 0, 0);
            }
        }
        batchMode = false;
        return ((long) i << 32) | (wc & 0xffffffffL);
    }

    /* Batch-mode NEW: matches and writes Trade records straight into outB via the
     * listener (batchMode == true); returns the filled quantity. */
    private long matchNew(long oid, long seq, long price, int qty, int side, int ioc) {
        curSeq      = seq;
        curTakerId  = oid;
        batchFilled = 0;
        Side s = (side == 0) ? Side.BUY : Side.SELL;
        TimeInForce tif = (ioc == 1) ? TimeInForce.IOC : TimeInForce.GTC;
        book.createLimit(CLIENT_ID, CLOID, oid, s, qty, price, tif);
        return batchFilled;
    }
    private long batchFilled;

    /** Warm the JIT-compiled hot path during engine_init (untimed), the way
     *  CoralME's own NoGCTest warms before measuring. Exercises new/modify/
     *  cancel, both match and rest, IOC residual, the miss paths, and the
     *  queries, then discards the warmed book so the run starts clean. */
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
            if ((i & 15) == 0) { bestBid(); bestAsk(); depthAt(mid - 2, 0); }
            onNew(id++, 0, mid + 1,  7, 0, 0);      // crossing buy -> one trade
            onNew(id++, 0, mid + 3, 20, 0, 1);      // IOC buy: fills residual, cancels rest
            onCancel(restBid);                       // live cancel
            onCancel(restAsk);                       // consumed above -> miss path
            onModify(restAsk, 0, mid + 2, 9, 1);     // stale modify -> miss path
        }
        book = newBook();      // discard the warmed book; start the run clean
        tradeBuf = saved;
    }
}
