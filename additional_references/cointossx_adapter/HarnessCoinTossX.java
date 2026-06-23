/*
 * HarnessCoinTossX.java — Java side of the CoinTossX benchmark adapter.
 *
 * CoinTossX (https://github.com/dharmeshsing/CoinTossX) is a full JSE exchange
 * built on Aeron/Agrona messaging. Its MATCHING ENGINE, however, is a separable
 * in-process core: a per-security `orderBook.OrderBook` (custom B+Tree price
 * index + off-heap Unsafe order lists) crossed by
 * `crossing.tradingSessions.ContinuousTradingProcessor.process(OrderBook,
 * OrderEntry)` under the price-time-priority strategy. This class drives that
 * core directly — exactly as CoinTossX's own ContinuousTradingProcessorTest
 * does — with NO Aeron, UDP, or disruptor in the loop. The native
 * cointossx_adapter.cpp embeds a JVM (JNI) and calls these methods to implement
 * api/matching_engine_api.h.
 *
 * Native API used (minimal shim, no re-implemented matching):
 *   - ContinuousTradingProcessor.process()  — the engine's own continuous-trading
 *                                              matcher (aggress then rest).
 *   - OrderEntryFactory.getOrderEntry()      — the engine's off-heap order node.
 *   - The cancel preprocessor's removal path (engine-native, keyed on
 *     clientOrderId==origClientOrderId at the order's price+side).
 *   - OrderBook.getBestBid()/getBestOffer()/getBidTree()/getOfferTree() for the
 *     audit queries.
 *
 * Per-order liveness/price/side shadow (LongLongHashMap order_id -> packed
 * (price, side, residual) in one primitive long): the harness cancel/modify carry
 * only an order_id, but CoinTossX's cancel needs the order's price AND side to find
 * it (CancelOrderPreProcessor looks up tree[price] on the order's side); CoinTossX
 * also returns no "unknown order" code (a missing cancel is silently a no-op). The
 * shadow supplies the price+side for the engine cancel and the not-resting test for
 * CancelReject/ModifyReject — the minimal state the examples (liquibook/
 * exchange-core) keep for the same reason. It never matches. The value is a
 * primitive long (no per-message long[] allocation; see the `resting` field), the
 * value-typed-shadow design the as-shipped reference adapters (geseq/fmstephe/
 * danielgatis) use — their `map[uint64]shadowEntry` stores the {price, side,
 * residual} struct inline with no per-entry allocation.
 *
 * Modify = cancel + reinsert (the harness rule): remove the resting order, re-add
 * at the new price/qty with a fresh (later) time priority, emit each crossing
 * fill + one ModifyAck, or a ModifyReject if it was not resting.
 *
 * ENGINE PATCH (one line, applied in build.sh, documented in PATCHES below):
 * CoinTossX's fill record (ExecutionReportData.addFillGroup(price,qty)) collapses
 * same-price fills into a map and keeps NO counterparty ids, so it cannot yield
 * the per-fill maker/taker order ids the harness Trade report requires. The patch
 * adds an observation-only sink in PriceTimePriorityStrategy.processOrdersInList
 * that records (maker, taker, price, qty) per fill. It changes no matching logic
 * (records data already in scope: currentOrder=maker, aggOrder=taker) and so can
 * neither hide nor create a matching bug.
 */
import com.carrotsearch.hppc.LongLongHashMap;
import com.carrotsearch.hppc.LongObjectHashMap;
import common.OrderType;
import common.TimeInForce;
import crossing.MatchingUtil;
import crossing.strategy.PriceTimePriorityStrategy;
import crossing.tradingSessions.TradingSessionFactory;
import crossing.tradingSessions.TradingSessionProcessor;
import data.ExecutionReportData;
import data.MarketData;
import leafNode.OrderEntry;
import leafNode.OrderEntryFactory;
import orderBook.OrderBook;
import orderBook.Stock;
import sbe.msg.OrderCancelRequestEncoder;
import sbe.msg.NewOrderEncoder;
import sbe.msg.marketData.TradingSessionEnum;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public final class HarnessCoinTossX {

    /* One staging-buffer fill record (cointossx_adapter.cpp StageTrade) is
     * 40 bytes, little-endian:
     *   sequence_number u64 @0, price_ticks i64 @8, quantity u32 @16,
     *   (pad @20), maker_order_id u64 @24, taker_order_id u64 @32.            */
    private static final int TRADE_SIZE = 40;

    private static final int STOCK_ID = 1;

    /* CoinTossX side encoding: BID(buy)=1, OFFER(sell)=2 (sbe.msg.SideEnum,
     * OrderBook.getSide). Harness side: 0=buy, 1=sell. */
    private static final byte CTX_BID = 1;
    private static final byte CTX_OFFER = 2;

    /* CoinTossX template ids select the order action in CrossingProcessor; the
     * continuous-trading pipeline only branches new-vs-cancel via the
     * CancelOrderPreProcessor (which fires when OrderType is left null). We drive
     * process() directly, so the template id only needs to be the non-cancel
     * (new-order) value for adds and is unused for our engine-native removal. */
    private static final int NEW_ORDER_TEMPLATE = NewOrderEncoder.TEMPLATE_ID;

    private final LongObjectHashMap<OrderBook> orderBooks = new LongObjectHashMap<>();
    private TradingSessionProcessor matcher;
    private OrderBook book;

    /* Liveness/price/side shadow: order_id -> packed (price, side, residual) in
     * ONE primitive long (a LongLongHashMap stores the value inline, so there is
     * NO per-message allocation — the previous LongObjectHashMap<long[]> allocated
     * a fresh long[3] on every resting order). Pre-sized to the run high-water
     * (1<<21), exactly as the as-shipped Go reference adapters pre-size their
     * `map[uint64]shadowEntry` (geseq/fmstephe/danielgatis, make(map,1<<21)).
     *
     * Packing (lossless over the canonical + flash-crash domain, with margin):
     *   bits  [0,32) residual  — full uint32 (the harness quantity field is u32)
     *   bit   [32]   side      — 0 = buy, 1 = sell
     *   bits  [33,64) price    — 31 bits of int64 price_ticks (prices are >= 0;
     *                            the deepest observed tape tops out near 2e6, so
     *                            31 bits gives ~1000x headroom). */
    private static final int  RESTING_CAP   = 1 << 21;
    private static final long RESIDUAL_MASK = 0xFFFFFFFFL;   // low 32 bits
    /* A live entry is inserted only with residual > 0, and residual occupies the
     * low 32 bits, so a packed value is always >= 1 — 0 is a safe "absent"
     * sentinel for getOrDefault (one lookup instead of containsKey + get). */
    private static final long ABSENT = 0L;
    private final LongLongHashMap resting = new LongLongHashMap(RESTING_CAP);

    private static long packShadow(long price, int side, long residual) {
        return (price << 33) | ((long) (side & 1) << 32) | (residual & RESIDUAL_MASK);
    }
    private static long unpackPrice(long v)    { return v >> 33; }
    private static int  unpackSide(long v)     { return (int) ((v >>> 32) & 1L); }
    private static long unpackResidual(long v) { return v & RESIDUAL_MASK; }

    private ByteBuffer tradeBuf;            // adapter-owned direct staging buffer

    /* Monotone arrival clock: CoinTossX keeps time priority by sorting each price
     * level on OrderEntry.getSubmittedTime() (OrderListImpl.timePriorityCompare).
     * The harness expresses time priority as arrival order and has no timestamp,
     * so we stamp a strictly increasing counter — equal timestamps would break
     * FIFO in the level's binary insertion. */
    private long clock = 0;

    public HarnessCoinTossX() {
        // No circuit breaker, no auctions: pure continuous-trading matching.
        MatchingUtil.setEnableCircuitBreaker(false);
        newEngine();
    }

    private void newEngine() {
        orderBooks.clear();
        book = new OrderBook(STOCK_ID);
        Stock stock = new Stock();
        stock.setStockCode(STOCK_ID);
        stock.setMRS(0);          // min reserve size 0: only matters for hidden orders
        stock.setTickSize(1);
        book.setStock(stock);
        orderBooks.put(STOCK_ID, book);

        TradingSessionFactory.reset();
        TradingSessionFactory.initTradingSessionProcessors(orderBooks);
        matcher = TradingSessionFactory.getTradingSessionProcessor(TradingSessionEnum.ContinuousTrading);

        // Route the engine's per-fill observation sink into our staging writer.
        PriceTimePriorityStrategy.HARNESS_SINK = this::onFill;
        resting.clear();
        clock = 0;
        fillN = 0;
    }

    /** The adapter hands over its staging buffer wrapped as a direct ByteBuffer. */
    public void setTradeBuffer(ByteBuffer bb) {
        tradeBuf = bb.order(ByteOrder.LITTLE_ENDIAN);
    }

    /* ---- per-fill capture (called by the patched engine) ------------------- */
    private int fillN;          // fills written into tradeBuf for the current op
    private long curSeq;        // sequence_number of the order being processed

    /** Engine callback: one invocation per crossing fill, maker/taker = clientOrderId. */
    private void onFill(long maker, long taker, long price, long qty) {
        int off = fillN * TRADE_SIZE;
        tradeBuf.putLong(off,      curSeq);          // sequence_number
        tradeBuf.putLong(off + 8,  price);           // price_ticks (maker resting)
        tradeBuf.putInt (off + 16, (int) qty);       // quantity
        tradeBuf.putLong(off + 24, maker);           // maker_order_id
        tradeBuf.putLong(off + 32, taker);           // taker_order_id
        fillN++;

        // Keep the liveness shadow current: the maker (the resting order being
        // consumed) is decremented by the fill qty in the engine's book, so its
        // shadow must follow. When the maker is fully consumed the engine drops it
        // from its level (shouldRemoveExistingOrder, line 259), so evict it here —
        // otherwise a later stale cancel/modify of the filled order would be wrongly
        // Acked (and a modify would re-rest a filled order) against a stale shadow.
        // Mirrors the Kautenja adapter's trade-hook maker decrement.
        long v = resting.getOrDefault(maker, ABSENT);
        if (v != ABSENT) {
            long rem = unpackResidual(v) - qty;
            if (rem > 0) {                            // partially consumed: still resting
                resting.put(maker, packShadow(unpackPrice(v), unpackSide(v), rem));
            } else {
                resting.remove(maker);               // fully consumed: no longer resting
            }
        }
    }

    /* ---- order construction ------------------------------------------------ */
    private OrderEntry makeOrder(long orderId, long price, int qty, int harnessSide,
                                 boolean ioc) {
        OrderEntry oe = OrderEntryFactory.getOrderEntry();
        oe.setOrderId(orderId);
        oe.setClientOrderId(orderId);          // durable id CoinTossX cancels on
        oe.setOrigClientOrderId(0);
        oe.setSide(harnessSide == 0 ? CTX_BID : CTX_OFFER);
        oe.setType(OrderType.LIMIT.getOrderType());
        oe.setTimeInForce(ioc ? TimeInForce.IOC.getValue() : TimeInForce.DAY.getValue());
        oe.setSubmittedTime(++clock);          // strictly increasing -> FIFO
        oe.setMinExecutionSize(0);
        oe.setStopPrice(0);
        oe.setExpireTime(0);
        oe.setTrader(1);
        oe.setPrice(price);
        oe.setQuantity(qty);                   // also sets executeVolume + displayQty
        return oe;
    }

    /** NEW order. Writes one fill record per trade into tradeBuf; returns fill count. */
    public int onNew(long orderId, long seq, long price, int qty, int side, int ioc) {
        ExecutionReportData.INSTANCE.reset();
        MarketData.INSTANCE.reset();
        fillN = 0;
        curSeq = seq;

        OrderEntry oe = makeOrder(orderId, price, qty, side, ioc == 1);
        matcher.process(book, oe);

        // After matching, the residual (if any) rests for a non-IOC order; record
        // it in the shadow at its (possibly unchanged) price+side. An IOC order
        // never rests — its residual is cancelled by the adapter (CancelAck).
        int residual = oe.getQuantity();
        if (ioc == 0 && residual > 0) {
            // Inline primitive value — no per-message long[] allocation.
            resting.put(orderId, packShadow(price, side, residual));
        }
        // The engine copies the OrderEntry into its off-heap level on add, so this
        // transient node can be freed.
        freeNode(oe);
        return fillN;
    }

    /** CANCEL. Returns the cancelled order's side — 1(bid)/2(offer) — with its
     *  price staged in fill record 0's price field, or 0 if it was not resting. */
    public int onCancel(long orderId) {
        ExecutionReportData.INSTANCE.reset();
        long v = resting.getOrDefault(orderId, ABSENT);
        if (v == ABSENT) return 0;             // not resting -> adapter CancelReject
        long price = unpackPrice(v);
        int side = unpackSide(v);
        boolean removed = removeFromBook(orderId, price, side);
        resting.remove(orderId);
        if (!removed) return 0;                // shadow/engine disagree -> reject
        tradeBuf.putLong(0 * TRADE_SIZE + 8, price);   // g_stage[0].price echo
        return side == 0 ? 1 : 2;
    }

    /** MODIFY = cancel + reinsert. Writes crossing fills; returns fill count, or
     *  -1 if the order was not resting (adapter then emits a ModifyReject). */
    public int onModify(long orderId, long seq, long price, int qty, int side) {
        long v = resting.getOrDefault(orderId, ABSENT);
        if (v == ABSENT) return -1;            // not resting -> ModifyReject
        removeFromBook(orderId, unpackPrice(v), unpackSide(v));
        resting.remove(orderId);
        // Reinsert at new price/qty with fresh time priority (loses queue pos).
        return onNew(orderId, seq, price, qty, side, 0);
    }

    /* Remove a resting order by id at its known price/side, using the engine's
     * own off-heap level + B+Tree removal (mirrors CancelOrderPreProcessor's
     * inner loop: scan the price level, drop the entry whose clientOrderId
     * matches, prune the empty level). Returns true if an entry was removed. */
    private boolean removeFromBook(long orderId, long price, int side) {
        OrderBook.SIDE ctxSide = (side == 0) ? OrderBook.SIDE.BID : OrderBook.SIDE.OFFER;
        leafNode.OrderList list = (ctxSide == OrderBook.SIDE.BID)
                ? book.getBidTree().get(price) : book.getOfferTree().get(price);
        if (list == null) return false;
        boolean removed = false;
        java.util.Iterator<leafNode.OrderListCursor> it = list.iterator();
        while (it.hasNext()) {
            if (it.next().value.getClientOrderId() == orderId) {
                it.remove();
                removed = true;
            }
        }
        if (removed && list.total() == 0) {
            book.removePrice(price, ctxSide);
        }
        return removed;
    }

    private void freeNode(OrderEntry oe) {
        unsafe.UnsafeUtil.freeOrderEntryMemory(oe);
    }

    /* ---- audit queries ----------------------------------------------------- */

    /** Best (highest) bid in ticks, or Long.MIN_VALUE if there are no bids. */
    public long bestBid() {
        long b = book.getBestBid();
        return b != 0 ? b : Long.MIN_VALUE;
    }

    /** Best (lowest) ask in ticks, or Long.MAX_VALUE if there are no asks. */
    public long bestAsk() {
        long a = book.getBestOffer();
        return a != 0 ? a : Long.MAX_VALUE;
    }

    /** Aggregated resting quantity at one price level (0 if the level is empty). */
    public long depthAt(long price, int side) {
        leafNode.OrderList list = (side == 0)
                ? book.getBidTree().get(price) : book.getOfferTree().get(price);
        return list == null ? 0 : list.total();
    }

    /* ---- warmup ------------------------------------------------------------
     * CoinTossX is JIT-compiled and the harness runs a single measured pass, so
     * the adapter warms the hot path during engine_init (untimed), mirroring the
     * exchange-core adapter. The warmed book is discarded and a fresh engine
     * installed, so warmup leaves no state behind. */
    public void warmup() {
        ByteBuffer scratch = ByteBuffer.allocateDirect(2048 * TRADE_SIZE)
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
            onNew(id++, 0, mid + 1,  7, 0, 0);       // crossing buy -> one trade
            onNew(id++, 0, mid + 3, 20, 0, 1);       // IOC buy: partial + residual
            onCancel(restBid);                        // live cancel (side+price echo)
            onCancel(restAsk);                        // consumed above -> miss path
            onModify(restAsk, 0, mid + 2, 9, 1);      // stale modify -> miss path
        }
        newEngine();             // discard the warmed book; start the run clean
        tradeBuf = saved;
    }
}
