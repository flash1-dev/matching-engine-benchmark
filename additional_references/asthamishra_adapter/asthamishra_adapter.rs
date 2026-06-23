//! asthamishra_adapter — AsthaMishra/matching-engine's `matching-core` behind
//! the harness `matching_engine_api.h` ABI.
//!
//! The engine is pure Rust, so this whole adapter is one Rust cdylib that
//! exports the harness `engine_*` extern-C symbols directly. No C++ shim.
//!
//! Engine shape (matching-core):
//!   - `OrderBook::new()` — a single-symbol book over a direct-indexed price
//!     array (price ticks 1..MAX_PRICE, MAX_PRICE = 100_000, TICK_SIZE = 1, so
//!     a price tick IS the array index) with a bitmap of occupied levels and a
//!     `Vec<Option<(side,price,qty,idx)>>` order index keyed by the order's id.
//!     The harness's order id is a u64 that fits in 32 bits and we use it
//!     verbatim as the engine's `usize` id, so the engine's own id index serves
//!     as our liveness/reject oracle — the adapter keeps NO per-order shadow.
//!   - `matching::match_order(&mut book, Order, CommandType)` — synchronous;
//!     matches the incoming order against the book, rests a Limit residual or
//!     (IOC) drops it, and returns a `Vec<OrderEvent>` of `Executed(Trade)`
//!     (one per fill, in match order) plus at most one `Accepted`/`Replace`.
//!     Each `Trade` carries the maker id, taker id (= incoming id), and the
//!     maker's resting price — exactly the harness Trade fields.
//!   - `OrderBook::cancel_order(id)` — id-keyed; `Ok(OrderEvent::Canceled{..})`
//!     when the order was resting, `Ok(OrderEvent::Rejected{..})` when not
//!     (already filled / already cancelled / never seen).
//!   - `OrderBook::get_order_by_id(id)` — `Option<&(Side, price, qty, idx)>`,
//!     read just before a cancel to recover the resting order's side+price for
//!     the CancelAck (the engine's Canceled event does not echo side/price).
//!   - `OrderBook::best_bid()/best_ask()` — `Option<i64>` ticks.
//!   - `OrderBook::volume_at_price(side, price)` — `Option<u64>` resting qty.
//!
//! Self-trade prevention: the engine's matcher CANCELS resting orders that
//! share the incoming order's `trader_id` (matching.rs) instead of crossing
//! them — production STP, but the harness models a single anonymous flow with
//! plain price-time priority and no STP. We neutralise it by giving every order
//! a UNIQUE trader_id (= its order id), so two distinct orders never share a
//! trader and the STP branch can never fire.
//!
//! Synthesised above the engine (the engine emits none of these in the
//! harness's wire format):
//!   - OrderAck, CancelAck (incl. IOC-residual), ModifyAck,
//!     CancelReject, ModifyReject.
//!
//! Modify contract: the harness defines modify as cancel + reinsert (queue
//! priority lost). We do exactly that against the engine — `cancel_order` then
//! a fresh `match_order` at the new price/qty — rather than the engine's own
//! `replace_order`, whose same-price / qty-decrease fast paths keep priority
//! and so deviate from the harness's unconditional cancel+reinsert.

use matching_core::matching::match_order;
use matching_core::order_book::OrderBook;
use matching_core::types::{CommandType, Order, OrderEvent, OrderType, Side};
use matching_core::utils::now_nanos;

use std::cell::UnsafeCell;
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
// call from ONE matcher thread (the drainer touches only the transport), so the
// adapter state needs no synchronization. A `static` still requires `Sync`, so
// this cell provides interior mutability via `UnsafeCell` with the
// single-thread-ownership invariant documented at each accessor. No lock, no
// atomic on the hot path — the Rust expression of the C++ reference adapters'
// plain globals (same pattern as the orderbookrs / philipgreat adapters).
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
    /// Shared borrow (queries).
    /// SAFETY: single matcher thread, `init` ran first.
    #[inline(always)]
    unsafe fn get_ref(&self) -> &T {
        (*self.0.get()).as_ref().unwrap_unchecked()
    }
    /// Mutable borrow (hot path — the engine book mutates on every op).
    /// SAFETY: single matcher thread, `init` ran first, no aliasing borrow.
    #[inline(always)]
    #[allow(clippy::mut_from_ref)]
    unsafe fn get_mut(&self) -> &mut T {
        (*self.0.get()).as_mut().unwrap_unchecked()
    }
}

// The harness owns both transport pointers; once engine_init returns they are
// stable until engine_shutdown, and only the matcher thread pushes reports.
static TRANSPORT: ThreadOwned<Transport> = ThreadOwned::new();
// The OrderBook itself: single instance, owned by the matcher thread. The engine
// keys each resting order by the harness id in its own order_index, so the
// adapter keeps NO per-order state: rejects and CancelAck side/price come from
// the engine itself.
static BOOK: ThreadOwned<OrderBook> = ThreadOwned::new();

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
fn side_to_u8(s: Side) -> u8 {
    match s {
        Side::Buy => 0,
        _ => 1,
    }
}

#[inline]
fn u8_to_side(s: u8) -> Side {
    if s == 0 { Side::Buy } else { Side::Sell }
}

// Drain a match_order result vector into harness Trade reports, returning the
// total filled quantity (so an IOC residual can be derived). Trades are pushed
// in the order the engine produced them — i.e. match order, which the canonical
// (seq, type) sort preserves within a single message's Trade group.
#[inline]
fn emit_trades(events: &[OrderEvent], seq: u64) -> u64 {
    let mut filled: u64 = 0;
    for ev in events {
        if let OrderEvent::Executed(tr) = ev {
            filled += tr.qty;
            let r = Report {
                r#type: ME_TRADE,
                side: 0, // Trade.side is not part of the canonical line
                _reserved: [0; 6],
                sequence_number: seq,
                order_id: 0,
                price_ticks: tr.price, // maker's resting price
                quantity: tr.qty as u32,
                _reserved2: 0,
                maker_order_id: tr.maker_order_id as u64,
                taker_order_id: tr.taker_order_id as u64,
                _reserved3: 0,
            };
            emit(&r);
        }
    }
    filled
}

// =============================================================================
// engine_init / engine_shutdown
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_init(_seed: u64, transport: *const MeTransport, sink: *mut c_void) {
    // SAFETY: init phase — the harness has not started delivering messages yet.
    unsafe {
        TRANSPORT.init(Transport { vtable: transport, sink });
        BOOK.init(OrderBook::new());
    }
}

#[no_mangle]
pub extern "C" fn engine_shutdown() {
    // The harness loads, runs once, and tears down. The ThreadOwned cells live
    // for the process lifetime; nothing to free here.
}

#[no_mangle]
pub extern "C" fn engine_flush() {
    // Synchronous matcher: every match_order / cancel_order runs and emits
    // inline before returning. Nothing to drain.
}

// =============================================================================
// Hot path
// =============================================================================

#[no_mangle]
pub unsafe extern "C" fn engine_on_new_order(order: *const NewOrder) {
    let o = unsafe { &*order };

    // OrderAck first (canonical (seq,type) sort puts it ahead of trades anyway).
    emit_ack(
        ME_ORDER_ACK,
        o.sequence_number,
        o.order_id,
        o.side,
        o.price_ticks,
        o.quantity,
    );

    let side = u8_to_side(o.side);
    let order_type = if o.ioc != 0 { OrderType::IOC } else { OrderType::Limit };
    // trader_id = order_id => globally unique => engine STP never fires.
    let eng_order = Order::new(
        o.order_id as usize,
        o.order_id,          // unique trader_id
        side,
        order_type,
        o.price_ticks,
        o.quantity as u64,
        o.quantity as u64,
        now_nanos(),
    );

    // SAFETY: single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_mut() };
    let events = match_order(book, eng_order, CommandType::Add);
    let filled = emit_trades(&events, o.sequence_number);

    if o.ioc != 0 {
        // Engine dropped any IOC residual internally; synthesise the harness's
        // CancelAck for the unfilled remainder.
        let residual = (o.quantity as u64).saturating_sub(filled);
        if residual > 0 {
            emit_ack(
                ME_CANCEL_ACK,
                o.sequence_number,
                o.order_id,
                o.side,
                o.price_ticks,
                residual as u32,
            );
        }
    }
    // GTC: the engine rested any residual under the harness id in its own index
    // (Accepted/Replace events are not part of the wire format). A fully filled
    // taker was never indexed, which makes a later cancel of it the native
    // Rejected reject. Nothing to record here.
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_cancel(cancel: *const CancelMsg) {
    let c = unsafe { &*cancel };
    // SAFETY: single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_mut() };

    // Recover the resting order's side + price for the CancelAck BEFORE removing
    // it (cancel_order's Canceled event echoes neither). get_order_by_id reads
    // the engine's own id index — Some iff the order is currently resting.
    let resting = book.get_order_by_id(c.order_id as usize).map(|&(s, p, _q, _i)| (s, p));

    match book.cancel_order(c.order_id as usize) {
        Ok(OrderEvent::Canceled { .. }) => {
            // resting was read an instant earlier in the same single-threaded
            // call, so it is Some whenever cancel succeeded.
            let (s, p) = resting.unwrap_or((Side::Buy, 0));
            emit_ack(
                ME_CANCEL_ACK,
                c.sequence_number,
                c.order_id,
                side_to_u8(s),
                p,
                0, // CancelAck quantity is not part of the canonical line
            );
        }
        // Rejected (OrderIdNotFound / OrderNotActive) or any error => not resting.
        _ => {
            emit_ack(ME_CANCEL_REJECT, c.sequence_number, c.order_id, 0, 0, 0);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_modify(modify: *const ModifyMsg) {
    let m = unsafe { &*modify };
    // SAFETY: single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_mut() };

    // Modify = cancel + reinsert. Read the resting order's true side first
    // (authoritative over the message's side field); Some iff resting.
    let resting_side = book.get_order_by_id(m.order_id as usize).map(|&(s, _p, _q, _i)| s);

    let Some(side) = resting_side else {
        emit_ack(ME_MODIFY_REJECT, m.sequence_number, m.order_id, 0, 0, 0);
        return;
    };

    // Remove the old resting order (loses queue priority — the harness contract).
    let cancelled = book.cancel_order(m.order_id as usize);
    if !matches!(cancelled, Ok(OrderEvent::Canceled { .. })) {
        // Defensive: get_order_by_id said resting, so cancel must succeed; if it
        // somehow did not, treat as not-resting.
        emit_ack(ME_MODIFY_REJECT, m.sequence_number, m.order_id, 0, 0, 0);
        return;
    }

    // ModifyAck at the NEW price/qty (the canonical ModifyAck carries side, new
    // price, new quantity); (seq,type) sort places it ahead of any reinsert
    // trades regardless of emit order.
    emit_ack(
        ME_MODIFY_ACK,
        m.sequence_number,
        m.order_id,
        side_to_u8(side),
        m.new_price_ticks,
        m.new_quantity,
    );

    // Re-add as a fresh GTC limit on the order's own side, under the same id and
    // a unique trader_id, so any crossing fills emit as Trades tagged with the
    // modify's seq. Every canonical modify is a qty increase (+ optional 1-tick
    // reprice), so it re-enters as a Limit.
    let new_order = Order::new(
        m.order_id as usize,
        m.order_id, // unique trader_id => no STP
        side,
        OrderType::Limit,
        m.new_price_ticks,
        m.new_quantity as u64,
        m.new_quantity as u64,
        now_nanos(),
    );
    let events = match_order(book, new_order, CommandType::Replace);
    emit_trades(&events, m.sequence_number);
}

// =============================================================================
// Audit queries
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_query_best_bid() -> i64 {
    // SAFETY: queries run on the same single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_ref() };
    book.best_bid().unwrap_or(i64::MIN)
}

#[no_mangle]
pub extern "C" fn engine_query_best_ask() -> i64 {
    // SAFETY: queries run on the same single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_ref() };
    book.best_ask().unwrap_or(i64::MAX)
}

#[no_mangle]
pub extern "C" fn engine_query_depth_at(price_ticks: i64, side: c_uchar) -> u64 {
    if price_ticks <= 0 {
        return 0;
    }
    // SAFETY: queries run on the same single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_ref() };
    book.volume_at_price(u8_to_side(side), price_ticks).unwrap_or(0)
}
