/*
 * hroptatyr_clob_adapter.c — matching_engine_api.h ABI backed by
 * hroptatyr/clob (Sebastian Freundt's "clob": a b+tree-based central limit
 * order book with pluggable uncrossing schemes), repo
 *   https://github.com/hroptatyr/clob  pinned at 812137a (v0.1.0).
 *
 * The engine's continuous-trading entry point is unxs_order(book, ord, ref):
 * it crosses the incoming order against the contra book in price-time
 * priority, records every fill into the book's attached execution stream
 * (book.exe, a MODE_BI unxs_t holding {price,qty} + maker/taker oids + maker
 * side per fill), and returns the residual order. The residual, if any, is
 * placed with clob_add(), which returns a clob_oid_t (type, side, price,
 * queue-id, usr) that clob_del() later uses to cancel.
 *
 * Numeric type: the engine is built in its DEFAULT configuration,
 * CLOB_TYPE = _Decimal64 (IEEE-754 DFP64), which is what ./configure selects
 * when the compiler has DFP support (it does here: HAVE_DFP754_BID_LITERALS).
 * The workload's integer ticks and quantities are small and convert exactly
 * to and from _Decimal64. We build the engine UNMODIFIED from its pinned HEAD;
 * this file is the only glue. (Its "double" mode would need the source's 36
 * hard-coded _Decimal64 `.dd` literals sanitised — patching the engine — so we
 * use the supported decimal mode instead and patch nothing.)
 *
 * What the adapter does:
 *   - new order: emit OrderAck, unxs_order() to cross, drain book.exe into one
 *     Trade per fill (maker price = exe price; maker/taker ids = the usr fields
 *     we stamp on every order), then clob_add() the residual and remember its
 *     oid. An IOC residual is reported as a CancelAck and never rested.
 *   - cancel: clob_del() the remembered oid; CancelAck on success, CancelReject
 *     if the order is not resting.
 *   - modify = cancel + reinsert at the new price/qty (losing time priority,
 *     the harness contract): clob_del() the old oid, then run the new-order
 *     crossing+rest path; ModifyAck, or ModifyReject if not resting.
 *
 * Liveness shadow (REQUIRED): clob offers no id->order index — an order is
 * addressed by its full clob_oid_t. Its queue-ids (plqu qids) are
 * per-price-level and are RECYCLED when a level empties and is freed
 * (free_plqu returns the slab to a pool; a later level reuses it), so a stale
 * oid could spuriously match a different live order at a recycled queue. We
 * therefore keep, per harness order_id, the live clob_oid_t plus a one-bit
 * "resting" flag, and adjudicate cancel/modify against that flag — the pattern
 * the mansoor / liquibook reference adapters use. The flag is cleared when the
 * order is fully filled (the maker-remainder the engine reports for a fill is
 * zero), cancelled, or modified.
 */
#include "matching_engine_api.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* engine headers — built with WITH_DECIMAL (CLOB_TYPE = _Decimal64).
 * We deliberately do NOT include dfp754_d64.h: the adapter needs no DFP
 * helpers (only exact int<->_Decimal64 casts, which are built in), and that
 * header's plain `inline` math functions would add duplicate definitions to
 * the adapter TU. */
#include "clob.h"
#include "clob_val.h"
#include "unxs.h"

/* exact integer <-> _Decimal64 helpers (workload values are small integers) */
static inline px_t  to_dec(int64_t v)  { return (px_t)v; }
static inline int64_t to_i64(px_t v)   { return (int64_t)v; }

/* --------------------------------------------------------------------------
 * The execution stream's concrete layout. unxs.h exposes unxs_t as a const
 * view; the producing routines cast it to a private struct (unxs.c:
 * struct _unxs_s). We only READ it after unxs_order has populated it, exactly
 * as cloe.c iterates book.exe->x[i]. Field order mirrors _unxs_s so that
 * o[m*i+0] = maker oid, o[m*i+1] = taker oid, q[m*i+0] = maker remainder.
 * ------------------------------------------------------------------------ */
struct unxs_view_s {
    unxs_mode_t   m;     /* MODE_BI == 2 */
    size_t        n;     /* number of fills */
    unxs_exe_t   *x;     /* x[i] = {price, qty} */
    uint_fast8_t *s;     /* s[i] = maker side */
    clob_oid_t   *o;     /* o[m*i + party] */
    qty_t        *q;     /* q[m*i + party] = remaining qty after the fill */
    /* size_t z; capacity, unread */
};

static clob_t                 g_book;
static const me_transport_t  *g_transport = NULL;
static void                   *g_sink      = NULL;

/* Per-harness-order liveness shadow. Order ids are dense and 1-based, so a
 * flat array indexed by order_id is a bounds-check + load. Sized untimed in
 * engine_init / engine_prebuild; written on the clock in engine_on_*. */
typedef struct {
    clob_oid_t oid;      /* the engine handle for clob_del() */
    uint8_t    live;     /* 1 iff currently resting in the book */
} shadow_t;

static shadow_t *g_shadow = NULL;
static size_t    g_shadow_n = 0;

static void
shadow_ensure(uint64_t id)
{
    if (id < g_shadow_n) {
        return;
    }
    size_t nuz = g_shadow_n ? g_shadow_n * 2U : (1U << 21);
    while (nuz <= id) {
        nuz *= 2U;
    }
    shadow_t *tmp = realloc(g_shadow, nuz * sizeof(*g_shadow));
    memset(tmp + g_shadow_n, 0, (nuz - g_shadow_n) * sizeof(*g_shadow));
    g_shadow = tmp;
    g_shadow_n = nuz;
}

static inline void
emit(const me_report_t *r)
{
    while (!g_transport->push(g_sink, r)) {
        /* spin: queue full */
    }
}

static inline void
emit_ack(uint8_t type, uint64_t seq, uint64_t order_id,
         uint8_t side, int64_t price, uint32_t qty)
{
    me_report_t r;
    memset(&r, 0, sizeof r);
    r.type            = type;
    r.sequence_number = seq;
    r.order_id        = order_id;
    r.side            = side;
    r.price_ticks     = price;
    r.quantity        = qty;
    emit(&r);
}

/* Drain the fills unxs_order just recorded into book.exe, emitting one Trade
 * per fill and clearing the liveness of any maker that was fully consumed.
 * taker_id is the aggressor (the order_id of the message being processed). */
static void
drain_executions(uint64_t taker_seq, uint64_t taker_id)
{
    struct unxs_view_s *x = (struct unxs_view_s *)g_book.exe;
    const size_t m = (size_t)x->m;            /* MODE_BI -> 2 */

    for (size_t i = 0U; i < x->n; i++) {
        const clob_oid_t *maker = &x->o[m * i + 0U];
        uint64_t maker_id = (uint64_t)maker->usr;

        me_report_t r;
        memset(&r, 0, sizeof r);
        r.type            = ME_TRADE;
        r.sequence_number = taker_seq;
        r.order_id        = taker_id;              /* report concerns the taker */
        r.price_ticks     = to_i64(x->x[i].prc);   /* maker resting price */
        r.quantity        = (uint32_t)to_i64(x->x[i].qty);
        r.maker_order_id  = maker_id;
        r.taker_order_id  = taker_id;
        emit(&r);

        /* Maker fully consumed for this fill -> its queue head has advanced;
         * clear liveness so a later cancel/modify of it rejects. q[m*i+0] is
         * the maker remainder (a qty_t) after the fill. */
        if (to_i64(qty(x->q[m * i + 0U])) <= 0) {
            if (maker_id < g_shadow_n) {
                g_shadow[maker_id].live = 0U;
            }
        }
    }
    unxs_clr(g_book.exe);
}

/* Cross + rest a new resting order (shared by new-order and modify-reinsert).
 * Emits Trades for every fill; rests the residual and records its oid. Does
 * NOT emit OrderAck/ModifyAck (the caller does that, in the right order). */
static void
cross_and_rest(uint64_t seq, uint64_t id, uint8_t side,
               int64_t price, uint32_t quan, int ioc)
{
    clob_ord_t o;
    memset(&o, 0, sizeof o);
    o.typ     = CLOB_TYPE_LMT;
    o.sid     = side ? CLOB_SIDE_ASK : CLOB_SIDE_BID;   /* harness 1=sell=ask */
    o.qty.dis = to_dec((int64_t)quan);
    o.qty.hid = to_dec(0);
    o.lmt     = to_dec(price);
    o.usr     = (uintptr_t)id;

    clob_ord_t rem = unxs_order(g_book, o, NANPX);

    drain_executions(seq, id);

    int64_t left = to_i64(qty(rem.qty));
    if (left <= 0) {
        if (id < g_shadow_n) {
            g_shadow[id].live = 0U;
        }
        return;
    }
    if (ioc) {
        /* IOC residual is cancelled, not rested */
        emit_ack(ME_CANCEL_ACK, seq, id, side, price, (uint32_t)left);
        if (id < g_shadow_n) {
            g_shadow[id].live = 0U;
        }
        return;
    }
    /* rest the remainder and remember its engine handle */
    clob_oid_t oid = clob_add(g_book, rem);
    shadow_ensure(id);
    g_shadow[id].oid  = oid;
    g_shadow[id].live = 1U;
}

/* ========================================================================= */

void
engine_init(uint64_t seed, const me_transport_t *transport, void *report_sink)
{
    (void)seed;
    g_transport = transport;
    g_sink      = report_sink;

    g_book = make_clob();
    g_book.exe = make_unxs(MODE_BI);   /* track both parties of each fill */
    g_book.quo = NULL;                 /* no quote stream needed */

    g_shadow_n = (1U << 21);
    g_shadow = calloc(g_shadow_n, sizeof(*g_shadow));
}

void
engine_shutdown(void)
{
    if (g_book.exe != NULL) {
        free_unxs(g_book.exe);
        g_book.exe = NULL;
    }
    free_clob(g_book);
    free(g_shadow);
    g_shadow = NULL;
    g_shadow_n = 0U;
}

/* Pre-build hook: size the liveness shadow to the workload's id range,
 * untimed. Translation/capacity only — no book insertion, no matching. */
void
engine_prebuild(uint8_t msg_type, const void *msg)
{
    uint64_t id;
    switch (msg_type) {
    case 0: id = ((const new_order_t *)msg)->order_id; break;
    case 1: id = ((const cancel_t   *)msg)->order_id; break;
    case 2: id = ((const modify_t   *)msg)->order_id; break;
    default: return;
    }
    shadow_ensure(id);
}

void
engine_flush(void)
{
    /* fully synchronous matcher — nothing deferred */
}

void
engine_on_new_order(const new_order_t *o)
{
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);
    cross_and_rest(o->sequence_number, o->order_id, o->side,
                   o->price_ticks, o->quantity, o->ioc);
}

void
engine_on_cancel(const cancel_t *c)
{
    shadow_t *s = (c->order_id < g_shadow_n) ? &g_shadow[c->order_id] : NULL;
    if (s == NULL || !s->live) {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
        return;
    }
    int rc = clob_del(g_book, s->oid);
    s->live = 0U;
    if (rc < 0) {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
        return;
    }
    emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id,
             (uint8_t)(s->oid.sid == CLOB_SIDE_ASK ? 1 : 0),
             to_i64(s->oid.prc), 0);
}

void
engine_on_modify(const modify_t *m)
{
    shadow_t *s = (m->order_id < g_shadow_n) ? &g_shadow[m->order_id] : NULL;
    if (s == NULL || !s->live) {
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        return;
    }
    /* cancel the resting order, then reinsert at the new price/qty (crossing
     * if now marketable), losing time priority — the harness modify model. */
    (void)clob_del(g_book, s->oid);
    s->live = 0U;
    cross_and_rest(m->sequence_number, m->order_id, m->side,
                   m->new_price_ticks, m->new_quantity, /*ioc=*/0);
    emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
             m->side, m->new_price_ticks, m->new_quantity);
}

int64_t
engine_query_best_bid(void)
{
    /* bid btree is sorted descending, so the first level is the best (highest) */
    clob_aggiter_t i = clob_aggiter(g_book, CLOB_TYPE_LMT, CLOB_SIDE_BID);
    if (clob_aggiter_next(&i)) {
        return to_i64(i.p);
    }
    return INT64_MIN;
}

int64_t
engine_query_best_ask(void)
{
    /* ask btree is sorted ascending, so the first level is the best (lowest) */
    clob_aggiter_t i = clob_aggiter(g_book, CLOB_TYPE_LMT, CLOB_SIDE_ASK);
    if (clob_aggiter_next(&i)) {
        return to_i64(i.p);
    }
    return INT64_MAX;
}

uint64_t
engine_query_depth_at(int64_t price_ticks, uint8_t side)
{
    clob_side_t sid = side ? CLOB_SIDE_ASK : CLOB_SIDE_BID;
    clob_aggiter_t i = clob_aggiter(g_book, CLOB_TYPE_LMT, sid);
    while (clob_aggiter_next(&i)) {
        if (to_i64(i.p) == price_ticks) {
            return (uint64_t)to_i64(qty(i.q));
        }
    }
    return 0U;
}

void
engine_on_batch(const me_msg_t *msgs, uint32_t n)
{
    for (uint32_t i = 0U; i < n; i++) {
        const me_msg_t *m = &msgs[i];
        switch (m->type) {
        case 0: engine_on_new_order(&m->no); break;
        case 1: engine_on_cancel(&m->c);     break;
        default: engine_on_modify(&m->md);   break;
        }
    }
}
