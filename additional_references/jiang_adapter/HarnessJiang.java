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
        ByteBuffer scratch = ByteBuffer.allocateDirect(4096 * TRADE_SIZE)
                                       .order(ByteOrder.LITTLE_ENDIAN);
        ByteBuffer saved = tradeBuf;
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
        book = new OrderBook();   // discard the warmed book; start the run clean
        tradeBuf = saved;
    }
}
