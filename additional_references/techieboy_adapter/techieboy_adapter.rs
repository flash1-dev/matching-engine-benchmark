//! techieboy_adapter — TechieBoy/rust-orderbook behind the harness
//! matching_engine_api.h ABI.
//!
//! The engine (crate `orderbook`, lib `orderbooklib`) is pure Rust, so this
//! whole adapter is one Rust cdylib that exports the harness `engine_*`
//! extern-C symbols directly. No C++ shim.
//!
//! What the engine provides natively (used here, after the small build-time
//! source patch in patch_engine.py — see build.sh / README.md):
//!   - `OrderBook::add_limit_order(side, price, qty, order_id)` — synchronous;
//!     matches the incoming order against the opposite book in strict
//!     price-then-time priority, then rests any residual at `price` keyed by
//!     `order_id`. Returns a `FillResult` whose patched `maker_fills` field
//!     lists one `(maker_order_id, fill_qty, maker_price)` per maker consumed,
//!     in match order, and whose `remaining_qty` is the taker's unfilled
//!     remainder. (Upstream minted the id internally and recorded only a
//!     per-level (qty,price) aggregate — the patch threads the harness id in
//!     and stops discarding the per-maker ids; the matching logic is
//!     unchanged.)
//!   - `OrderBook::cancel_order(order_id)` — synchronous, natively id-keyed via
//!     the engine's own `order_loc` index. `Ok(_)` = was resting and removed;
//!     `Err(_)` = not resting (already filled / cancelled / never seen). This
//!     is the native existence signal that drives CancelReject / ModifyReject.
//!   - `OrderBook::eng_best_bid_raw()` / `eng_best_ask_raw()` — the engine's
//!     own cached BBO fields, returned verbatim.
//!   - `OrderBook::eng_depth_at(price, side)` — live aggregated qty at one
//!     price level.
//!
//! Synthesised above the engine (the engine emits none of these in the
//! harness wire format): OrderAck, CancelAck (incl. IOC residual), ModifyAck,
//! CancelReject, ModifyReject.
//!
//! IOC: the engine has no native IOC. The adapter submits the order as a plain
//! limit (which matches what it can and rests the residual), then — for IOC
//! only — cancels the just-rested residual by id and reports it as the harness
//! IOC-residual CancelAck. No residual ever survives an IOC.
//!
//! Modify: the harness defines modify as cancel + reinsert (queue priority
//! lost). The adapter performs exactly that: cancel_order(id) is the resting
//! test (Err -> ModifyReject), then add_limit_order on the message's side at
//! the new price/qty, emitting any crossing fills.

use orderbooklib::{OrderBook, Side};
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

// Compile-time layout assertions.
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

struct Transport {
    vtable: *const MeTransport,
    sink: *mut c_void,
}

// Single-thread-owned global with interior mutability. The harness drives every
// engine_on_* / engine_query_* call from ONE matcher thread (the drainer only
// touches the transport), so no synchronization is needed; the `Sync` bound
// exists solely to satisfy the `static` requirement. Same pattern as the
// orderbookrs reference adapter, but the engine's methods take `&mut self`, so
// this also exposes get_mut.
struct ThreadOwned<T>(UnsafeCell<Option<T>>);
// SAFETY: every access is from the single matcher thread.
unsafe impl<T> Sync for ThreadOwned<T> {}
impl<T> ThreadOwned<T> {
    const fn new() -> Self {
        Self(UnsafeCell::new(None))
    }
    /// SAFETY: single matcher thread, init phase, no other live access.
    #[inline(always)]
    unsafe fn init(&self, value: T) {
        *self.0.get() = Some(value);
    }
    /// SAFETY: single matcher thread, init ran first, no aliasing borrow live.
    #[inline(always)]
    #[allow(clippy::mut_from_ref)]
    unsafe fn get_mut(&self) -> &mut T {
        (*self.0.get()).as_mut().unwrap_unchecked()
    }
    /// SAFETY: single matcher thread, init ran first.
    #[inline(always)]
    unsafe fn get_ref(&self) -> &T {
        (*self.0.get()).as_ref().unwrap_unchecked()
    }
}

static TRANSPORT: ThreadOwned<Transport> = ThreadOwned::new();
static BOOK: ThreadOwned<OrderBook> = ThreadOwned::new();

#[inline]
fn emit(r: &Report) {
    // SAFETY: matcher thread only; TRANSPORT.init ran in engine_init.
    let t = unsafe { TRANSPORT.get_ref() };
    unsafe {
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
fn emit_trade(seq: u64, taker_oid: u64, maker_oid: u64, price: i64, qty: u32, taker_side: u8) {
    let r = Report {
        r#type: ME_TRADE,
        side: taker_side,
        _reserved: [0; 6],
        sequence_number: seq,
        order_id: 0,
        price_ticks: price,
        quantity: qty,
        _reserved2: 0,
        maker_order_id: maker_oid,
        taker_order_id: taker_oid,
        _reserved3: 0,
    };
    emit(&r);
}

#[inline(always)]
fn side_of(side_byte: u8) -> Side {
    if side_byte == 0 {
        Side::Bid
    } else {
        Side::Ask
    }
}

// =============================================================================
// engine_init / engine_shutdown
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_init(_seed: u64, transport: *const MeTransport, sink: *mut c_void) {
    // SAFETY: init phase — no messages delivered yet.
    unsafe { TRANSPORT.init(Transport { vtable: transport, sink }) };
    unsafe { BOOK.init(OrderBook::new("HARNESS".to_string())) };
}

#[no_mangle]
pub extern "C" fn engine_shutdown() {
    // Process runs once and is torn down by the harness; nothing to free here.
}

#[no_mangle]
pub extern "C" fn engine_flush() {
    // Fully synchronous matcher: every call matched + emitted inline already.
}

// =============================================================================
// Hot path
// =============================================================================

#[no_mangle]
pub unsafe extern "C" fn engine_on_new_order(order: *const NewOrder) {
    let o = unsafe { &*order };

    // OrderAck first (canonical order: Ack, then Trades, then optional residual
    // CancelAck for an IOC).
    emit_ack(
        ME_ORDER_ACK,
        o.sequence_number,
        o.order_id,
        o.side,
        o.price_ticks,
        o.quantity,
    );

    // Harness ticks for these scenarios are non-negative (mid path stays above
    // $1.00); clamp a negative defensively to 0 so the u64 cast is lossless.
    let price: u64 = if o.price_ticks < 0 { 0 } else { o.price_ticks as u64 };
    let side = side_of(o.side);

    // SAFETY: single matcher thread.
    let book = unsafe { BOOK.get_mut() };
    let fr = book.add_limit_order(side, price, o.quantity as u64, o.order_id);

    // Trades in match order (the patched per-maker fill list).
    for (maker_oid, fill_qty, maker_price) in &fr.maker_fills {
        emit_trade(
            o.sequence_number,
            o.order_id,
            *maker_oid,
            *maker_price as i64,
            *fill_qty as u32,
            o.side,
        );
    }

    if o.ioc != 0 {
        // Native engine has no IOC: it rested the residual under o.order_id.
        // Remove it and report it as the IOC-residual CancelAck. fr.remaining_qty
        // is the taker's unfilled remainder.
        if fr.remaining_qty > 0 {
            // The residual was rested by add_limit_order; cancel it by id.
            // (Discard the cancel's own bookkeeping result — we emit the
            // IOC-specific CancelAck below, not a plain cancel ack.)
            let _ = book.cancel_order(o.order_id);
            emit_ack(
                ME_CANCEL_ACK,
                o.sequence_number,
                o.order_id,
                o.side,
                o.price_ticks,
                fr.remaining_qty as u32,
            );
        }
    }
    // Non-IOC: any residual rests under o.order_id in the engine's order_loc,
    // so a later cancel/modify keys to it natively.
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_cancel(cancel: *const CancelMsg) {
    let c = unsafe { &*cancel };
    // SAFETY: single matcher thread.
    let book = unsafe { BOOK.get_mut() };
    // Native id-keyed cancel: Ok((side, price, qty)) = removed (the engine's own
    // record of the resting order's fields), Err(()) = not resting.
    match book.cancel_order(c.order_id) {
        Ok((side, price, qty)) => emit_ack(
            ME_CANCEL_ACK,
            c.sequence_number,
            c.order_id,
            side,
            price as i64,
            qty as u32,
        ),
        Err(_) => emit_ack(ME_CANCEL_REJECT, c.sequence_number, c.order_id, 0, 0, 0),
    }
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_modify(modify: *const ModifyMsg) {
    let m = unsafe { &*modify };
    // SAFETY: single matcher thread.
    let book = unsafe { BOOK.get_mut() };

    // Modify = cancel + reinsert. cancel_order is the resting test.
    match book.cancel_order(m.order_id) {
        Ok(_) => {
            emit_ack(
                ME_MODIFY_ACK,
                m.sequence_number,
                m.order_id,
                m.side,
                m.new_price_ticks,
                m.new_quantity,
            );
            // Reinsert on the message's side at the new price/qty. Any crossing
            // fills are emitted as Trades (after the ModifyAck), and any residual
            // rests under the same id.
            let price: u64 = if m.new_price_ticks < 0 {
                0
            } else {
                m.new_price_ticks as u64
            };
            let side = side_of(m.side);
            let fr = book.add_limit_order(side, price, m.new_quantity as u64, m.order_id);
            for (maker_oid, fill_qty, maker_price) in &fr.maker_fills {
                emit_trade(
                    m.sequence_number,
                    m.order_id,
                    *maker_oid,
                    *maker_price as i64,
                    *fill_qty as u32,
                    m.side,
                );
            }
        }
        Err(_) => emit_ack(ME_MODIFY_REJECT, m.sequence_number, m.order_id, 0, 0, 0),
    }
}

// =============================================================================
// Audit queries
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_query_best_bid() -> i64 {
    // SAFETY: queries run on the same single matcher thread.
    let book = unsafe { BOOK.get_ref() };
    let b = book.eng_best_bid_raw();
    // Engine's empty-bid sentinel is u64::MIN (0); the harness wants INT64_MIN.
    if b == u64::MIN {
        i64::MIN
    } else {
        b as i64
    }
}

#[no_mangle]
pub extern "C" fn engine_query_best_ask() -> i64 {
    // SAFETY: queries run on the same single matcher thread.
    let book = unsafe { BOOK.get_ref() };
    let a = book.eng_best_ask_raw();
    // Engine's empty-ask sentinel is u64::MAX; the harness wants INT64_MAX.
    if a == u64::MAX {
        i64::MAX
    } else {
        a as i64
    }
}

#[no_mangle]
pub extern "C" fn engine_query_depth_at(price_ticks: i64, side: c_uchar) -> u64 {
    if price_ticks < 0 {
        return 0;
    }
    // SAFETY: queries run on the same single matcher thread.
    let book = unsafe { BOOK.get_ref() };
    book.eng_depth_at(price_ticks as u64, side_of(side))
}
