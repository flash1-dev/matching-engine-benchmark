/*
 * HarnessTradeMacher.java — Java side of the TradeMatcher benchmark adapter.
 *
 * TradeMatcher (https://github.com/TradeMatcher/match-engine, Maven artifact
 * match-engine-core, package com.tradematcher) is a Java price-time-priority
 * matching engine: a per-symbol OrderBookImpl with a TreeMap of price "buckets"
 * (Buckets, reverse-ordered for bids), a single cross-bucket doubly-linked order
 * list per side carrying time priority, a HashMap<String,Order> id index, and an
 * LMAX-Disruptor command pipeline on top. This class drives the engine's
 * MATCHING CORE — OrderBookImpl — directly, exactly as the engine's own
 * GTCTest/FAKTest do (they call match.doAction -> OrderBook methods), with NO
 * Disruptor, WebSocket, journal, or snapshot in the loop. The native
 * trademacher_adapter.cpp embeds a JVM (JNI) and calls these methods to
 * implement api/matching_engine_api.h.
 *
 * Native engine API used (minimal shim, no re-implemented matching):
 *   - OrderBookImpl.matchByUnitPriceAndSize(id, price, size, action) — the
 *     engine's own limit-price + size price-time matcher (used for both a
 *     harness new order and the cross step of a harness IOC/modify);
 *   - OrderBook.createOrder(...) — the engine's own rest-the-residual path
 *     (this is exactly what PlaceGTCOrder.action() does after the match);
 *   - OrderBook.getOrderByID(id) — the engine's own id index, for the cancel /
 *     modify existence test and the resting order's price + side;
 *   - OrderBookImpl.cancelOrder(order) — the engine's own removal path;
 *   - OrderBook.getMarketOrderBook(depth) / getMarkets() — the engine's own
 *     order-book snapshot, for the best-bid / best-ask / depth audit queries.
 *
 * The harness new order is a GTC limit order: match marketable, rest the rest
 * (PlaceGTCOrder). A harness IOC is a limit order that matches what it can and
 * drops the residual: the SAME engine match (matchByUnitPriceAndSize), then we
 * simply do not rest the residual — the C++ side emits the IOC-residual
 * CancelAck. (The engine's own FAK uses total-price / wanted-size semantics, a
 * different order kind; the harness IOC is limit-price + size, so reusing the
 * GTC matcher minus the rest step is the faithful mapping and re-implements no
 * matching.) Modify = cancel + reinsert (the harness rule).
 *
 * Per-fill Trade records are written into an adapter-owned direct ByteBuffer;
 * the engine's MAKER Event list gives the maker id + maker price + filled size
 * per fill, and the aggressor's id/seq are known here. No engine source is
 * patched.
 */
import com.tradematcher.entity.Event;
import com.tradematcher.entity.Market;
import com.tradematcher.entity.MatchResult;
import com.tradematcher.entity.Order;
import com.tradematcher.entity.OrderBookImpl;
import com.tradematcher.entity.Symbol;
import com.tradematcher.util.Constants;

import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public final class HarnessTradeMacher {

    /* One staging-buffer fill record (trademacher_adapter.cpp StageTrade) is
     * 40 bytes, little-endian:
     *   sequence_number u64 @0, price_ticks i64 @8, quantity u32 @16,
     *   (pad @20), maker_order_id u64 @24, taker_order_id u64 @32.
     * onCancel additionally stages the cancelled order's price in record 0's
     * price field — read by the C++ side as g_stage[0].price.                  */
    private static final int TRADE_SIZE = 40;

    private static final int SYMBOL_ID = 1;
    /* The forward-only metadata the engine carries on each Order; irrelevant to
     * matching (price/size/action are the only matching inputs). */
    private static final int VOLUME_DIGITS = 0;
    private static final int TRADE_TYPE = 0;
    private static final int SYMBOL_DECIMAL = 0;
    /* Audit-query book depth: deep enough to cover any level the state audit
     * probes (the engine's own market snapshot caps at MAX_MARKET_DEPTH=200, but
     * getMarketOrderBook(depth) honours an explicit depth). */
    private static final int QUERY_DEPTH = Integer.MAX_VALUE;

    /* Harness side: 0 = buy, 1 = sell. Engine action: BID(buy)=1, ASK(sell)=0
     * (Constants.Action). */
    private static byte engineAction(int harnessSide) {
        return harnessSide == 0 ? Constants.Action.BID : Constants.Action.ASK;
    }

    private OrderBookImpl book;
    private ByteBuffer tradeBuf;            // adapter-owned direct staging buffer

    /* Per-call state set before each match so the fill writer can label the
     * Trade it stages (the engine's Event list carries the maker leg; the
     * aggressor's seq/id are known only here). */
    private long curSeq;                    // aggressive order's sequence number
    private long curTakerId;                // aggressive order's id
    private int  fillN;                     // fills staged for the current op

    public HarnessTradeMacher() {
        book = newBook();
    }

    private OrderBookImpl newBook() {
        return new OrderBookImpl(new Symbol(SYMBOL_ID, VOLUME_DIGITS, TRADE_TYPE));
    }

    /** The adapter hands over its staging buffer wrapped as a direct ByteBuffer. */
    public void setTradeBuffer(ByteBuffer bb) {
        tradeBuf = bb.order(ByteOrder.LITTLE_ENDIAN);
    }

    /* ---- fill staging ------------------------------------------------------ *
     * Walk the engine's MAKER Event chain (one node per maker leg, in match
     * order) and stage one Trade record per leg. price = maker's resting
     * unitPrice, qty = the leg's filled size, maker id = the maker order's id,
     * taker id/seq = the aggressor.                                            */
    private int stageEvents(Event event) {
        int n = 0;
        while (event != null) {
            if (event.getType() == Constants.EventType.MAKER) {
                int off = n * TRADE_SIZE;
                tradeBuf.putLong(off,      curSeq);                       // sequence_number (aggressor)
                tradeBuf.putLong(off + 8,  event.getUnitPrice().longValue()); // price_ticks (maker resting)
                tradeBuf.putInt (off + 16, event.getSize().intValue());  // quantity (this leg)
                tradeBuf.putLong(off + 24, Long.parseLong(event.getOrderID())); // maker_order_id
                tradeBuf.putLong(off + 32, curTakerId);                  // taker_order_id (aggressor)
                n++;
            }
            event = event.getNext();
        }
        return n;
    }

    /* ---- per-message API --------------------------------------------------- */

    /** Core matcher shared by NEW(GTC), the IOC cross, and the modify reinsert.
     *  Runs the engine's own limit-price + size price-time match, stages the
     *  resulting fills, optionally rests the unfilled residual (GTC) or leaves
     *  it to be dropped (IOC). Returns the filled size. */
    private long matchCore(long orderId, long seq, long price, int qty, int side, boolean rest) {
        curSeq     = seq;
        curTakerId = orderId;
        fillN      = 0;

        byte action = engineAction(side);
        BigInteger unitPrice = BigInteger.valueOf(price);
        BigInteger size = BigInteger.valueOf(qty);
        String id = Long.toString(orderId);

        MatchResult mr = book.matchByUnitPriceAndSize(id, unitPrice, size, action);

        BigInteger filledSize = mr.getFilledSize();      // null only on the not-marketable early return
        long filled = (filledSize == null) ? 0L : filledSize.longValue();

        fillN = stageEvents(mr.getEvent());

        if (rest) {
            BigInteger remained = size.subtract(BigInteger.valueOf(filled));
            if (remained.compareTo(BigInteger.ZERO) > 0) {
                book.createOrder(unitPrice, remained, action, id, VOLUME_DIGITS, TRADE_TYPE, SYMBOL_DECIMAL);
            }
        }
        return filled;
    }

    /** NEW order. ioc==0 -> GTC limit (match, rest residual). ioc==1 -> IOC
     *  (match, residual dropped; the C++ side emits the residual CancelAck).
     *  Writes one fill record per trade into tradeBuf; returns the fill count.
     *  The filled quantity is read back by the C++ side from the staged sizes
     *  (it only needs filled<qty for the IOC residual), so we return fillN. */
    public int onNew(long orderId, long seq, long price, int qty, int side, int ioc) {
        matchCore(orderId, seq, price, qty, side, ioc == 0);
        return fillN;
    }

    /** CANCEL. Returns the cancelled order's harness side (0 buy / 1 sell) + 1,
     *  i.e. 1 for buy or 2 for sell, with its price staged in record 0's price
     *  field; or 0 if the order is not resting. Uses the engine's own id index
     *  (getOrderByID) and removal path (cancelOrder) — no adapter order state. */
    public int onCancel(long orderId) {
        Order o = book.getOrderByID(Long.toString(orderId));
        if (o == null) return 0;                         // not resting -> CancelReject
        long price = o.getUnitPrice().longValue();
        int harnessSide = (o.getAction() == Constants.Action.BID) ? 0 : 1;
        book.cancelOrder(o);                             // engine-native removal
        tradeBuf.putLong(0 * TRADE_SIZE + 8, price);     // g_stage[0].price echo
        return harnessSide + 1;                          // 1 = buy, 2 = sell
    }

    /** MODIFY = cancel + reinsert (the harness rule). Removes the resting order
     *  and re-adds it at the new price/qty with fresh (later) time priority,
     *  crossing on reinsert. Writes one fill record per crossing trade; returns
     *  the fill count, or -1 if the order was not resting (-> ModifyReject). */
    public int onModify(long orderId, long seq, long price, int qty, int side) {
        Order o = book.getOrderByID(Long.toString(orderId));
        if (o == null) return -1;                        // not resting -> ModifyReject
        book.cancelOrder(o);                             // remove; no trades, no report
        matchCore(orderId, seq, price, qty, side, true); // reinsert as GTC
        return fillN;
    }

    /* ---- audit queries ----------------------------------------------------- *
     * Read the engine's own order-book snapshot (getMarketOrderBook -> Market):
     * bids are best-first (reverse-ordered TreeMap), asks best-first (natural).
     * These are sparse, off-hot-path probes, so a snapshot walk is fine and
     * keeps the engine unpatched. */

    private Market snapshot() {
        return book.getMarketOrderBook(QUERY_DEPTH).getMarket();
    }

    /** Best (highest) bid in ticks, or Long.MIN_VALUE if there are no bids. */
    public long bestBid() {
        Market m = snapshot();
        return m.getBidDepth() > 0 ? m.getBidUnitPrices()[0].longValue() : Long.MIN_VALUE;
    }

    /** Best (lowest) ask in ticks, or Long.MAX_VALUE if there are no asks. */
    public long bestAsk() {
        Market m = snapshot();
        return m.getAskDepth() > 0 ? m.getAskUnitPrices()[0].longValue() : Long.MAX_VALUE;
    }

    /** Aggregated resting quantity at one price level (0 if the level is empty). */
    public long depthAt(long price, int side) {
        Market m = snapshot();
        BigInteger p = BigInteger.valueOf(price);
        if (side == 0) {
            BigInteger[] prices = m.getBidUnitPrices();
            BigInteger[] sizes = m.getBidSizes();
            for (int i = 0; i < m.getBidDepth(); i++)
                if (prices[i].compareTo(p) == 0) return sizes[i].longValue();
        } else {
            BigInteger[] prices = m.getAskUnitPrices();
            BigInteger[] sizes = m.getAskSizes();
            for (int i = 0; i < m.getAskDepth(); i++)
                if (prices[i].compareTo(p) == 0) return sizes[i].longValue();
        }
        return 0;
    }

    /* ---- warmup ------------------------------------------------------------ *
     * The engine is JIT-compiled and the harness runs one measured pass, so the
     * adapter warms the hot path during engine_init (untimed), mirroring the
     * coralme / exchange-core adapters. The warmed book is discarded so the run
     * starts clean. */
    public void warmup() {
        ByteBuffer scratch = ByteBuffer.allocateDirect(4096 * TRADE_SIZE)
                                       .order(ByteOrder.LITTLE_ENDIAN);
        ByteBuffer saved = tradeBuf;
        tradeBuf = scratch;
        long id = 1_000_000_000L;       // disjoint from any real run id range
        final long mid = 100_000;
        for (int i = 0; i < 100_000; i++) {
            long restBid = id++;
            onNew(restBid, 0, mid - 1, 10, 0, 0);    // resting buy
            long restAsk = id++;
            onNew(restAsk, 0, mid + 1, 10, 1, 0);    // resting sell
            onModify(restBid, 0, mid - 2, 12, 0);    // reprice the resting buy
            if ((i & 15) == 0) { bestBid(); bestAsk(); depthAt(mid - 2, 0); }
            onNew(id++, 0, mid + 1,  7, 0, 0);       // crossing buy -> one trade
            onNew(id++, 0, mid + 3, 20, 0, 1);       // IOC buy: partial + residual
            onCancel(restBid);                        // live cancel
            onCancel(restAsk);                        // consumed above -> miss path
            onModify(restAsk, 0, mid + 2, 9, 1);      // stale modify -> miss path
        }
        book = newBook();                // discard the warmed book; start clean
        tradeBuf = saved;
    }
}
