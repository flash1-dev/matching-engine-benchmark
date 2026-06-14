//! orderbookrs_adapter — joaquinbejar/OrderBook-rs behind the harness
//! matching_engine_api.h ABI.
//!
//! The engine is pure Rust, so this whole adapter is one Rust cdylib that
//! exports the harness `engine_*` extern-C symbols directly. No C++ shim.
//!
//! What the engine provides natively (used here):
//!   - `OrderBook::<()>::add_limit_order(id, price, qty, side, tif, None)` —
//!     synchronous, matches the incoming order against the book, then rests
//!     a GTC residual or (TimeInForce::Ioc) drops it, reporting the fill
//!     through `Err(InsufficientLiquidity)`. We read the Result to derive
//!     IOC residuals and rejects.
//!   - `OrderBook::cancel_order(id)` — synchronous and natively id-keyed
//!     (the engine keeps its own id index), returning `Ok(Some(order))` —
//!     the removed order itself, carrying its side / price / live remaining
//!     quantity — or `Ok(None)` as the native not-resting signal. That
//!     payload drives the CancelAck/ModifyAck field echo and both reject
//!     paths directly; the adapter keeps NO per-order state of its own.
//!   - `OrderBook::best_bid()` / `best_ask()` — `Option<u128>`.
//!   - `OrderBook::bids` / `asks` (pub(super) skip-map; reached indirectly
//!     via `total_depth_at_levels` — but we want exact depth at one price,
//!     so we walk via `levels_in_range(price, price, side)`).
//!   - `TradeListener` callback fires from `add_limit_order` whenever fills
//!     occurred, with the full
//!     `MatchResult`; we drain its `trades()` into harness Trade reports and
//!     read its `remaining_quantity()` — the taker's post-match residual,
//!     the same field the engine rests the residual from — which closes
//!     `add_limit_order`'s residual ambiguity (its Ok return does not
//!     decompose fills) without a `get_order` readback.
//!
//! Synthesised above the engine (engine emits none of these in the
//! harness's wire format):
//!   - OrderAck, CancelAck (incl. IOC-residual), ModifyAck,
//!     CancelReject, ModifyReject.
//!
//! Order ids: the engine's `Id` is a 3-variant enum (UUID / ULID /
//! Sequential u64). `Id::Sequential` round-trips a u64 losslessly via
//! `as_u64()`, so trade callbacks recover the harness's order ids
//! directly without a sidecar map. The other two variants are not used.
//!
//! Modify contract: the harness defines modify as cancel + reinsert
//! (queue-priority lost). The upstream `update_order`'s price-change
//! variants are themselves cancel_order + add_order internally, routed
//! through an extra get_order + Arc clone and returning only the
//! post-match order — the adapter performs the same two native calls
//! directly, because cancel_order's returned payload also seeds the
//! ModifyAck side echo.

use orderbook_rs::OrderBook;
use orderbook_rs::orderbook::trade::TradeResult;
use pricelevel::{Id, Side, TimeInForce};
use std::cell::{Cell, UnsafeCell};
use std::os::raw::{c_uchar, c_uint, c_void};

// =============================================================================
// Harness ABI mirrors. Layout MUST match api/matching_engine_api.h exactly.
// =============================================================================

#[repr(C)]
#[derive(Copy, Clone)]
pub struct NewOrder {
    pub order_id: u64,
    pub sequence_number: u64,
    pub price_ticks: i64,
    pub quantity: u32,
    pub side: u8,
    pub ioc: u8,
    pub _reserved: [u8; 2],
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct CancelMsg {
    pub order_id: u64,
    pub sequence_number: u64,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct ModifyMsg {
    pub order_id: u64,
    pub sequence_number: u64,
    pub new_price_ticks: i64,
    pub new_quantity: u32,
    pub side: u8,
    pub _reserved: [u8; 3],
}

// me_report_type_t values
const ME_ORDER_ACK: u8 = 0;
const ME_TRADE: u8 = 1;
const ME_CANCEL_ACK: u8 = 2;
const ME_MODIFY_ACK: u8 = 3;
const ME_CANCEL_REJECT: u8 = 4;
const ME_MODIFY_REJECT: u8 = 5;

#[repr(C)]
#[derive(Copy, Clone)]
pub struct Report {
    pub r#type: u8,
    pub side: u8,
    pub _reserved: [u8; 6],
    pub sequence_number: u64,
    pub order_id: u64,
    pub price_ticks: i64,
    pub quantity: u32,
    pub _reserved2: u32,
    pub maker_order_id: u64,
    pub taker_order_id: u64,
    pub _reserved3: u64,
}

// const_assert at compile time that the layout is right.
const _: () = assert!(std::mem::size_of::<NewOrder>() == 32);
const _: () = assert!(std::mem::size_of::<CancelMsg>() == 16);
const _: () = assert!(std::mem::size_of::<ModifyMsg>() == 32);
const _: () = assert!(std::mem::size_of::<Report>() == 64);

#[repr(C)]
pub struct MeTransport {
    pub create: extern "C" fn(capacity: c_uint) -> *mut c_void,
    pub push: extern "C" fn(handle: *mut c_void, report: *const Report) -> i32,
    pub drain: extern "C" fn(handle: *mut c_void, out: *mut Report, max: c_uint) -> c_uint,
    pub flush: extern "C" fn(handle: *mut c_void),
    pub destroy: extern "C" fn(handle: *mut c_void),
}

// =============================================================================
// Adapter state
// =============================================================================

// Transport vtable + sink, set in engine_init.
struct Transport {
    vtable: *const MeTransport,
    sink: *mut c_void,
}

// Single-thread-owned global. The harness drives every engine_on_* / query_*
// call from ONE matcher thread (the drainer touches only the transport), so
// the adapter state needs no synchronization at all. A `static` still
// requires `Sync`, so this cell provides interior mutability via `UnsafeCell`
// with the single-thread-ownership invariant documented at each accessor.
// No lock and no atomic on the hot path — the Rust expression of the C++
// reference adapters' plain globals (same pattern as the philipgreat adapter).
struct ThreadOwned<T>(UnsafeCell<Option<T>>);
// SAFETY: every access is from the single matcher thread; the `Sync` bound
// exists only to satisfy the `static` requirement, not to permit sharing.
unsafe impl<T> Sync for ThreadOwned<T> {}
impl<T> ThreadOwned<T> {
    const fn new() -> Self {
        Self(UnsafeCell::new(None))
    }
    /// Initialise once, from engine_init, before any other entry point runs.
    /// SAFETY: single matcher thread, init phase, no other live access.
    #[inline(always)]
    unsafe fn init(&self, value: T) {
        *self.0.get() = Some(value);
    }
    /// Shared borrow (hot path + audit queries — the engine book is
    /// `&self`-method-only, so no mutable accessor is needed here).
    /// SAFETY: single matcher thread, `init` ran first.
    #[inline(always)]
    unsafe fn get_ref(&self) -> &T {
        (*self.0.get()).as_ref().unwrap_unchecked()
    }
}

// The harness owns both transport pointers; once engine_init returns they are
// stable until engine_shutdown, and only the matcher thread pushes reports.
static TRANSPORT: ThreadOwned<Transport> = ThreadOwned::new();
// The OrderBook itself: single instance, owned by the matcher thread. Every
// hot-path method takes `&self`. The engine natively keys each resting order
// by id (its internal order_locations index), and cancel_order(id) returns
// the removed order's side/price/remaining — so the adapter keeps NO
// per-order state: rejects and ack field echo come from the engine itself.
static BOOK: ThreadOwned<OrderBook<()>> = ThreadOwned::new();

// Per-call context — the seq of the currently-processing harness message,
// its order id, and the taker's post-match residual. The TradeListener
// installed on the book reads the first two to stamp each Trade report and
// writes the third from MatchResult::remaining_quantity(). Single matcher
// thread, so Cell thread-locals are the natural fit.
thread_local! {
    static CUR_SEQ: Cell<u64> = const { Cell::new(0) };
    static CUR_TAKER: Cell<u64> = const { Cell::new(0) };
    static CUR_REMAINING: Cell<u32> = const { Cell::new(0) };
}

#[inline]
fn emit(r: &Report) {
    // SAFETY: matcher thread only; TRANSPORT.init ran in engine_init.
    let t = unsafe { TRANSPORT.get_ref() };
    unsafe {
        // Spin until accepted. Matches the C++ adapters' pattern.
        while ((*t.vtable).push)(t.sink, r as *const Report) == 0 {
            std::hint::spin_loop();
        }
    }
}

#[inline]
fn emit_ack(rep_type: u8, seq: u64, order_id: u64, side: u8, price: i64, qty: u32) {
    let r = Report {
        r#type: rep_type,
        side,
        _reserved: [0; 6],
        sequence_number: seq,
        order_id,
        price_ticks: price,
        quantity: qty,
        _reserved2: 0,
        maker_order_id: 0,
        taker_order_id: 0,
        _reserved3: 0,
    };
    emit(&r);
}

#[inline]
fn id_to_u64(id: Id) -> u64 {
    // Every Id this adapter inserts is Id::Sequential(oid); the engine
    // hands back the same Id in trade callbacks. as_u64() returns Some
    // for Sequential and None for UUID / ULID, which we never create.
    id.as_u64().unwrap_or(0)
}

// =============================================================================
// engine_init / engine_shutdown
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_init(
    _seed: u64,
    transport: *const MeTransport,
    sink: *mut c_void,
) {
    // SAFETY: init phase — the harness has not started delivering messages yet.
    unsafe { TRANSPORT.init(Transport { vtable: transport, sink }) };

    // Install the book with a trade listener that converts every match-result
    // transaction into a harness Trade report. The listener fires inline from
    // add_limit_order on the matcher thread, so reading the per-call seq /
    // taker via thread-locals is well-defined. It also records the taker's
    // post-match residual — the same field the engine rests the residual
    // from — so the adapter never needs a get_order readback. The engine
    // maintains every maker's remaining quantity itself (pricelevel re-stores
    // partially filled makers at their reduced quantity), so no maker
    // bookkeeping happens here either.
    let listener = std::sync::Arc::new(|tr: &TradeResult| {
        let seq = CUR_SEQ.with(|s| s.get());
        let taker_oid = CUR_TAKER.with(|s| s.get());
        CUR_REMAINING.with(|s| s.set(tr.match_result.remaining_quantity() as u32));
        for tx in tr.match_result.trades().as_vec() {
            let price = tx.price().as_u128() as i64;
            let qty = tx.quantity().as_u64() as u32;
            let maker = id_to_u64(tx.maker_order_id());
            let r = Report {
                r#type: ME_TRADE,
                side: 0,
                _reserved: [0; 6],
                sequence_number: seq,
                order_id: 0,
                price_ticks: price,
                quantity: qty,
                _reserved2: 0,
                maker_order_id: maker,
                taker_order_id: taker_oid,
                _reserved3: 0,
            };
            emit(&r);
        }
    });

    // with_trade_listener installs the closure at construction time so the
    // book never needs `&mut` after the cell has taken ownership.
    let book = OrderBook::<()>::with_trade_listener("HARNESS", listener);
    // SAFETY: init phase, single thread, no other live access.
    unsafe { BOOK.init(book) };
}

#[no_mangle]
pub extern "C" fn engine_shutdown() {
    // The harness loads, runs once, and tears down. The ThreadOwned cells
    // live for the process lifetime; do nothing here. Mirrors the C++
    // adapters' shutdown (which typically just resets unique_ptrs that the
    // dlclose path would free anyway).
}

#[no_mangle]
pub extern "C" fn engine_flush() {
    // Synchronous matcher: every add_limit_order / cancel_order runs and
    // emits inline before returning. Nothing to drain.
}

// =============================================================================
// Hot path
// =============================================================================

#[no_mangle]
pub unsafe extern "C" fn engine_on_new_order(order: *const NewOrder) {
    let o = unsafe { &*order };
    // OrderAck first so the canonical report order (Ack, then Trades, then
    // optional residual CancelAck) matches the C++ adapters and canonical
    // reference output.
    emit_ack(
        ME_ORDER_ACK,
        o.sequence_number,
        o.order_id,
        o.side,
        o.price_ticks,
        o.quantity,
    );

    CUR_SEQ.with(|s| s.set(o.sequence_number));
    CUR_TAKER.with(|s| s.set(o.order_id));
    // Preset the residual to the full quantity; the trade listener overwrites
    // it with the engine's post-match remaining_quantity() iff fills occur.
    CUR_REMAINING.with(|s| s.set(o.quantity));

    // SAFETY: single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_ref() };
    let side = if o.side == 0 { Side::Buy } else { Side::Sell };
    let id = Id::Sequential(o.order_id);
    // Engine-native time-in-force: harness IOC is the engine's Ioc. Trades
    // emit through the listener during matching either way; an Ioc residual
    // is dropped by the engine itself (never rests).
    let tif = if o.ioc != 0 { TimeInForce::Ioc } else { TimeInForce::Gtc };
    let result = book.add_limit_order(
        id,
        o.price_ticks as u128,   // harness ticks are non-negative: lossless
        o.quantity as u64,
        side,
        tif,
        None,
    );

    let remaining: u32 = match result {
        // Engine accepted (GTC, or an IOC that fully filled). The trade
        // listener (fired inline iff any fills occurred) recorded the taker's
        // post-match remaining_quantity() — the very value the engine rests
        // the residual from; with no fills the preset full quantity stands.
        // add_limit_order's own Ok return does not decompose fills.
        Ok(_) => CUR_REMAINING.with(|s| s.get()),
        // The engine's native IOC answer: a partially-filled or unfilled Ioc
        // drops its residual internally and reports the fill through
        // `available` (= quantity - remaining), after the listener has
        // already emitted the trades.
        Err(orderbook_rs::OrderBookError::InsufficientLiquidity { available, .. }) => {
            (o.quantity as u64).saturating_sub(available) as u32
        }
        // Defensive: no other error is reachable for a plain GTC/Ioc limit
        // add under the harness contract (no tick/lot/STP/risk validation
        // is configured); 0 yields the same rejects downstream either way.
        Err(_) => 0,
    };

    if o.ioc != 0 {
        // The engine already discarded the residual (native Ioc semantics);
        // synthesise the harness's CancelAck for the unfilled remainder.
        if remaining > 0 {
            emit_ack(
                ME_CANCEL_ACK,
                o.sequence_number,
                o.order_id,
                o.side,
                o.price_ticks,
                remaining,
            );
        }
        return;
    }

    // GTC: the engine rested any residual under the harness id in its own
    // id index — cancel/modify rejects and ack echo come straight from
    // cancel_order's return, so there is nothing to record here. A fully
    // filled taker was never placed, which is exactly what makes a later
    // cancel of it the native Ok(None) reject.
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_cancel(cancel: *const CancelMsg) {
    let c = unsafe { &*cancel };
    // SAFETY: single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_ref() };
    // The engine's cancel is natively id-keyed and returns the removed order
    // itself — side, price, and live remaining quantity (pricelevel re-stores
    // partially filled makers at their reduced quantity). Ok(None) is the
    // native not-resting signal: already filled, already cancelled, or never
    // seen. The CancelAck echo and the reject decision both come straight
    // from that one call.
    match book.cancel_order(Id::Sequential(c.order_id)) {
        Ok(Some(o)) => {
            emit_ack(
                ME_CANCEL_ACK,
                c.sequence_number,
                c.order_id,
                match o.side() {
                    Side::Buy => 0,
                    Side::Sell => 1,
                },
                o.price().as_u128() as i64,
                o.visible_quantity() as u32,
            );
        }
        _ => {
            emit_ack(ME_CANCEL_REJECT, c.sequence_number, c.order_id, 0, 0, 0);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_modify(modify: *const ModifyMsg) {
    let m = unsafe { &*modify };
    // SAFETY: single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_ref() };

    let id = Id::Sequential(m.order_id);
    // The harness modify contract is cancel + reinsert. The engine's id-keyed
    // cancel doubles as the resting test: Ok(Some(order)) hands back the
    // removed order — its side seeds the reinsert — while Ok(None) is the
    // native not-resting reject. (update_order's price-change variants are
    // the same cancel + add internally, but return no removed-order payload
    // for the side echo.)
    match book.cancel_order(id) {
        Ok(Some(o)) => {
            let side = o.side();
            emit_ack(
                ME_MODIFY_ACK,
                m.sequence_number,
                m.order_id,
                match side {
                    Side::Buy => 0,
                    Side::Sell => 1,
                },
                m.new_price_ticks,
                m.new_quantity,
            );

            CUR_SEQ.with(|s| s.set(m.sequence_number));
            CUR_TAKER.with(|s| s.set(m.order_id));

            // Re-add on the same side so any crossing fills emit through the
            // trade listener tagged with the modify's seq. The engine rests
            // any residual itself — no adapter bookkeeping follows.
            // Result discarded: a GTC reinsert of a just-cancelled id cannot
            // fail (no duplicate, no validation configured), and the listener
            // has already emitted any crossing trades by the time it returns.
            let _ = book.add_limit_order(
                id,
                m.new_price_ticks as u128,   // non-negative ticks: lossless
                m.new_quantity as u64,
                side,
                TimeInForce::Gtc,
                None,
            );
        }
        _ => {
            emit_ack(ME_MODIFY_REJECT, m.sequence_number, m.order_id, 0, 0, 0);
        }
    }
}

// =============================================================================
// Audit queries
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_query_best_bid() -> i64 {
    // SAFETY: queries run on the same single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_ref() };
    match book.best_bid() {
        Some(p) => p as i64,
        None => i64::MIN,
    }
}

#[no_mangle]
pub extern "C" fn engine_query_best_ask() -> i64 {
    // SAFETY: queries run on the same single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_ref() };
    match book.best_ask() {
        Some(p) => p as i64,
        None => i64::MAX,
    }
}

#[no_mangle]
pub extern "C" fn engine_query_depth_at(price_ticks: i64, side: c_uchar) -> u64 {
    if price_ticks < 0 {
        return 0;
    }
    // SAFETY: queries run on the same single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_ref() };
    let p = price_ticks as u128;
    // Walk levels-in-range filtered to this exact price. The engine's
    // levels_in_range yields LevelInfo { price, quantity } per level.
    let s = if side == 0 { Side::Buy } else { Side::Sell };
    let mut total: u64 = 0;
    for level in book.levels_in_range(p, p, s) {
        total = total.saturating_add(level.quantity);
    }
    total
}

