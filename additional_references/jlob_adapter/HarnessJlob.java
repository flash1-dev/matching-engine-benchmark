/*
 * HarnessJlob.java — Java side of the jLOB benchmark adapter.
 *
 * jLOB (https://github.com/eliquinox/jLOB) is an L3 limit order book: bids and
 * offers are price-sorted fastutil Long2ObjectRBTreeMaps of `Limit` price
 * levels, each level an insertion-ordered (FIFO) ArrayList of `Placement`s,
 * with a UUID->Placement index. Matching (state.LimitOrderBook.place) crosses an
 * incoming order against the best contra levels price-time, then rests the
 * residual. This class drives that core directly. The native jlob_adapter.cpp
 * embeds a JVM (JNI) and calls these methods to implement
 * api/matching_engine_api.h.
 *
 * Native API used (minimal shim, NO re-implemented matching):
 *   - state.LimitOrderBook.place(Placement)   — the engine's own price-time
 *                                                matcher (aggress, then rest).
 *   - state.LimitOrderBook.cancel(Cancellation) — the engine's own removal
 *                                                (throws JLOBException on an
 *                                                unknown/oversized cancel).
 *   - state.LimitOrderBook.getBestBid/getBestOffer + getBestBidAmount/
 *     getBestOfferAmount + streamBids/streamOffers for the audit queries.
 *   - state.LimitOrderBookListener (engine-shipped interface) for the per-fill
 *     onMatch and the onPlacement / onCancellation hooks.
 *
 * NO engine source is patched. The only engine-private access is reflective:
 * LimitOrderBook's public ctor requires a live Redis-backed Cache and its
 * empty() factory hardwires a no-op DummyLimitOrderBookListener, so we invoke
 * the engine's own private LimitOrderBook(LimitOrderBookListener) ctor by
 * reflection to install OUR listener. That is adapter glue (constructing the
 * engine object), not a change to any matching logic.
 *
 * order_id <-> UUID: the harness identifies orders by a dense uint64 order_id;
 * jLOB identifies Placements by UUID. We mint a deterministic UUID =
 * new UUID(0, order_id) per order, so cancel/modify find the Placement by id and
 * a Match's maker/taker UUID recovers the harness id from its low 64 bits.
 *
 * Per-order liveness/price/side shadow (Long2ObjectOpenHashMap id -> [price,
 * side]): jLOB's Match carries no maker price and its cancel throws (no
 * "unknown order" code) — the shadow supplies the maker's resting price for the
 * Trade report, the cancelled order's price+side for the CancelAck, and the
 * not-resting test for CancelReject/ModifyReject. It is the minimal state the
 * reference adapters (liquibook/exchange-core) keep for the same reasons; it
 * never matches. A partial reduction is tracked so a later cancel of the same id
 * still reports the right resting price/side.
 *
 * Modify = cancel + reinsert (the harness rule): remove the resting order, re-add
 * at the new price/qty with fresh (later) time priority, emit each crossing fill
 * + one ModifyAck, or a ModifyReject if it was not resting.
 *
 * IOC: jLOB has no IOC order type, so an IOC new order is matched by place() and
 * its residual (if any) is immediately removed via the engine's own cancel; the
 * adapter emits the residual CancelAck.
 */
import dto.Cancellation;
import dto.Match;
import dto.Placement;
import dto.Side;
import exceptions.JLOBException;
import it.unimi.dsi.fastutil.longs.Long2ObjectMap;
import it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap;
import state.Limit;
import state.LimitOrderBook;
import state.LimitOrderBookListener;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.UUID;

public final class HarnessJlob implements LimitOrderBookListener {

    /* One staging-buffer fill record (jlob_adapter.cpp StageTrade) is 40 bytes,
     * little-endian:
     *   sequence_number u64 @0, price_ticks i64 @8, quantity u32 @16,
     *   (pad @20), maker_order_id u64 @24, taker_order_id u64 @32.            */
    private static final int TRADE_SIZE = 40;

    /* Harness side: 0 = buy/BID, 1 = sell/OFFER. */
    private static final int H_BID = 0;
    private static final int H_OFFER = 1;

    private LimitOrderBook book;

    /* jLOB's Placement.Builder.withUuid is private and Placement mints a random
     * UUID in its ctor; we need a DETERMINISTIC UUID = new UUID(0, order_id) so
     * cancel/modify find the Placement by id and a Match's maker/taker UUID
     * recovers the harness id from its low 64 bits. We build the Placement
     * normally, then overwrite its (final) uuid field through this cached, once-
     * unlocked reflective handle. Adapter glue (constructing the engine's order
     * object with a chosen id), not a change to matching logic. */
    private static final Field PLACEMENT_UUID;
    static {
        try {
            PLACEMENT_UUID = Placement.class.getDeclaredField("uuid");
            PLACEMENT_UUID.setAccessible(true);
        } catch (ReflectiveOperationException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    /* Liveness/price/side shadow: order_id -> packed long[]{price, side,
     * remainingSize}. Present iff the order currently rests in the book. */
    private final Long2ObjectOpenHashMap<long[]> resting = new Long2ObjectOpenHashMap<>();

    private ByteBuffer tradeBuf;            // adapter-owned direct staging buffer

    /* ---- per-op fill capture (written by onMatch) -------------------------- */
    private int fillN;          // fills written into tradeBuf for the current op
    private long curSeq;        // sequence_number of the order being processed
    private long takerFilled;   // quantity the aggressor filled this op

    public HarnessJlob() {
        newEngine();
    }

    /* Construct a fresh LimitOrderBook wired to THIS listener via the engine's
     * own private LimitOrderBook(LimitOrderBookListener) ctor (reflection — the
     * public ctor needs a live Redis Cache, empty() hardwires a no-op listener).
     * No engine logic is touched; we only build the engine object. */
    private void newEngine() {
        try {
            Constructor<LimitOrderBook> c =
                    LimitOrderBook.class.getDeclaredConstructor(LimitOrderBookListener.class);
            c.setAccessible(true);
            book = c.newInstance(this);
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException("cannot construct jLOB LimitOrderBook", e);
        }
        resting.clear();
        fillN = 0;
    }

    /** The adapter hands over its staging buffer wrapped as a direct ByteBuffer. */
    public void setTradeBuffer(ByteBuffer bb) {
        tradeBuf = bb.order(ByteOrder.LITTLE_ENDIAN);
    }

    private static UUID idToUuid(long orderId) {
        return new UUID(0L, orderId);
    }

    /* ---- LimitOrderBookListener (engine callbacks) ------------------------- */

    @Override
    public void onPlacement(Placement placement, LimitOrderBook lob) {
        // Not needed: the adapter emits OrderAck itself and tracks resting state
        // around place(); the engine fires this after matching+resting.
    }

    @Override
    public void onCancellation(Cancellation cancellation, LimitOrderBook lob) {
        // Not needed: cancels are adapter-driven and acked on the adapter side.
    }

    /** Engine callback: one invocation per crossing fill. maker/taker UUIDs carry
     *  the harness id in their low 64 bits; the fill price is the maker's resting
     *  price, read from the shadow (the maker entry is still present — the shadow
     *  is updated only after place() returns). */
    @Override
    public void onMatch(Match match) {
        long maker = match.getMakerPlacementUuid().getLeastSignificantBits();
        long taker = match.getTakerPlacementUuid().getLeastSignificantBits();
        long qty = match.getSize();
        long[] makerInfo = resting.get(maker);
        long price = (makerInfo != null) ? makerInfo[0] : 0L;

        int off = fillN * TRADE_SIZE;
        tradeBuf.putLong(off,      curSeq);          // sequence_number (aggressor)
        tradeBuf.putLong(off + 8,  price);           // price_ticks (maker resting)
        tradeBuf.putInt (off + 16, (int) qty);       // quantity
        tradeBuf.putLong(off + 24, maker);           // maker_order_id
        tradeBuf.putLong(off + 32, taker);           // taker_order_id
        fillN++;
        takerFilled += qty;

        // Maintain the maker's resting shadow: a fully-consumed maker is gone; a
        // partially-consumed maker keeps the same price/side (size isn't tracked).
        if (makerInfo != null && qty >= makerSize(maker)) {
            // fully filled — engine removed it from its book; drop the shadow too.
            resting.remove(maker);
        } else {
            makerRemainingReduce(maker, qty);
        }
    }

    /* The shadow stores [price, side]; maker remaining size is tracked separately
     * so a partial fill leaves the right resting state. Packed as long[]{price,
     * side, remainingSize}. */
    private long makerSize(long id) {
        long[] v = resting.get(id);
        return (v != null && v.length >= 3) ? v[2] : 0L;
    }
    private void makerRemainingReduce(long id, long by) {
        long[] v = resting.get(id);
        if (v != null && v.length >= 3) v[2] -= by;
    }

    /* ---- new order --------------------------------------------------------- */

    /** NEW order. Writes one fill record per trade into tradeBuf; returns fill
     *  count. The adapter handles OrderAck and the IOC residual CancelAck. */
    public int onNew(long orderId, long seq, long price, int qty, int side, int ioc) {
        fillN = 0;
        curSeq = seq;
        takerFilled = 0;

        Side jside = (side == H_BID) ? Side.BID : Side.OFFER;
        Placement p = Placement.placement()
                .withSide(jside)
                .withPrice(price)
                .withSize(qty)
                .build();
        try {
            PLACEMENT_UUID.set(p, idToUuid(orderId));   // deterministic id-keyed UUID
        } catch (IllegalAccessException e) {
            throw new RuntimeException(e);
        }
        book.place(p);

        long residual = qty - takerFilled;
        if (ioc == 1) {
            // IOC never rests: remove any residual via the engine's own cancel.
            if (residual > 0) {
                try {
                    book.cancel(new Cancellation(idToUuid(orderId), residual));
                } catch (JLOBException ignore) {
                    // Fully filled exactly at the boundary -> nothing rests.
                }
            }
            // (no shadow entry: an IOC order is never resting afterwards)
        } else if (residual > 0) {
            resting.put(orderId, new long[]{ price, side, residual });
        }
        return fillN;
    }

    /* ---- cancel ------------------------------------------------------------ */

    /** CANCEL. Returns 1 if the order was resting and is now cancelled (its price
     *  staged in fill record 0's price field, its side in record 0 @16), or 0 if
     *  it was not resting (-> adapter CancelReject). */
    public int onCancel(long orderId) {
        long[] h = resting.get(orderId);
        if (h == null) return 0;               // not resting -> CancelReject
        long price = h[0];
        long sside = h[1];
        long remaining = (h.length >= 3) ? h[2] : 0L;
        try {
            book.cancel(new Cancellation(idToUuid(orderId), remaining));
        } catch (JLOBException e) {
            // Shadow/engine disagree (should not happen) -> treat as not resting.
            resting.remove(orderId);
            return 0;
        }
        resting.remove(orderId);
        tradeBuf.putLong(8, price);            // g_stage[0].price echo
        tradeBuf.putInt(16, (int) sside);      // g_stage[0] side echo (record 0 @16)
        return 1;
    }

    /* ---- modify = cancel + reinsert ---------------------------------------- */

    /** MODIFY = cancel + reinsert. Writes crossing fills; returns fill count, or
     *  -1 if the order was not resting (-> adapter ModifyReject). */
    public int onModify(long orderId, long seq, long price, int qty, int side) {
        long[] h = resting.get(orderId);
        if (h == null) return -1;              // not resting -> ModifyReject
        long remaining = (h.length >= 3) ? h[2] : 0L;
        try {
            book.cancel(new Cancellation(idToUuid(orderId), remaining));
        } catch (JLOBException e) {
            resting.remove(orderId);
            return -1;
        }
        resting.remove(orderId);
        // Reinsert at new price/qty with fresh time priority (loses queue pos).
        return onNew(orderId, seq, price, qty, side, 0);
    }

    /* ---- audit queries ----------------------------------------------------- */

    /** Best (highest) bid in ticks, or Long.MIN_VALUE if there are no bids. */
    public long bestBid() {
        try {
            return book.getBestBid();
        } catch (java.util.NoSuchElementException e) {
            return Long.MIN_VALUE;
        }
    }

    /** Best (lowest) ask in ticks, or Long.MAX_VALUE if there are no asks. */
    public long bestAsk() {
        try {
            return book.getBestOffer();
        } catch (java.util.NoSuchElementException e) {
            return Long.MAX_VALUE;
        }
    }

    /** Aggregated resting quantity at one price level (0 if the level is empty). */
    public long depthAt(long price, int side) {
        java.util.Iterator<Long2ObjectMap.Entry<Limit>> it =
                (side == H_BID) ? book.streamBids().iterator() : book.streamOffers().iterator();
        while (it.hasNext()) {
            Long2ObjectMap.Entry<Limit> e = it.next();
            if (e.getLongKey() == price) return e.getValue().getVolume();
        }
        return 0L;
    }

    /* ---- warmup ------------------------------------------------------------
     * jLOB is JIT-compiled and the harness runs a single measured pass, so the
     * adapter warms the hot path during engine_init (untimed), mirroring the
     * other JNI adapters. The warmed book is discarded and a fresh engine
     * installed, so warmup leaves no state behind. */
    public void warmup() {
        ByteBuffer scratch = ByteBuffer.allocateDirect(4096 * TRADE_SIZE)
                                       .order(ByteOrder.LITTLE_ENDIAN);
        ByteBuffer saved = tradeBuf;
        tradeBuf = scratch;
        long id = 1;
        final long mid = 100_000;
        for (int i = 0; i < 50_000; i++) {
            long restBid = id++;
            onNew(restBid, 0, mid - 1, 10, H_BID, 0);    // resting buy
            long restAsk = id++;
            onNew(restAsk, 0, mid + 1, 10, H_OFFER, 0);  // resting sell
            onModify(restBid, 0, mid - 2, 12, H_BID);    // reprice the resting buy
            if ((i & 15) == 0) { bestBid(); bestAsk(); depthAt(mid - 2, H_BID); }
            onNew(id++, 0, mid + 1,  7, H_BID, 0);        // crossing buy -> one trade
            onNew(id++, 0, mid + 3, 20, H_BID, 1);        // IOC buy: partial + residual
            onCancel(restBid);                            // live cancel (price/side echo)
            onCancel(restAsk);                            // consumed above -> miss path
            onModify(restAsk, 0, mid + 2, 9, H_OFFER);    // stale modify -> miss path
        }
        newEngine();             // discard the warmed book; start the run clean
        tradeBuf = saved;
    }
}
