//! gocronx_adapter — gocronx/matcher behind the harness
//! matching_engine_api.h ABI.
//!
//! The engine is pure Rust, so this whole adapter is one Rust cdylib that
//! exports the harness `engine_*` extern-C symbols directly. No C++ shim.
//! (Path A in docs/INTEGRATION.md, same shape as the orderbookrs reference.)
//!
//! ## Engine shape
//!
//! `matcher::OrderBook` is a single-threaded, lock-free, I/O-free price-time
//! book: `BTreeMap<Price, PriceLevel>` per side, `AHashMap<OrderId, Order>` for
//! id lookup, `SmallVec<[OrderId; 8]>` FIFO per level. It matches synchronously
//! on the calling thread and returns the *full* execution event stream from one
//! call — exactly what the harness needs.
//!
//! What the engine provides natively (used here):
//!   - `OrderBook::submit_events(order, ts) -> Vec<BookEvent>` — matches the
//!     incoming order, rests any residual (Limit/Iceberg), and returns the
//!     ordered event stream: `Accepted`, `Trade`(s), `Rested`, or `Rejected`.
//!     We map `Accepted -> OrderAck` and each `Trade -> ME_TRADE` (maker resting
//!     price + maker/taker ids). `Rested` carries no harness report.
//!   - `OrderBook::cancel_events(id, ts) -> Vec<BookEvent>` — id-keyed; returns
//!     `Canceled` (the native cancel-ack) or `CancelRejected{UnknownOrderId}`
//!     (the native not-resting signal). We map those to CancelAck / CancelReject
//!     directly — the engine itself is the reject adjudicator.
//!   - `OrderBook::best_bid()` / `best_ask()` -> `Option<Price>` (u64).
//!   - `OrderBook::level_qty(side, price) -> Quantity` — exact depth at one
//!     price level; answers engine_query_depth_at directly.
//!   - `OrderBook::get_order_se(id) -> Option<(side, price)>` — a minimal
//!     read-only accessor added to the engine by an idempotent build.sh patch
//!     (the engine ships no public id->order getter, and its CancelAck wire line
//!     `2,seq,side,order_id,price_ticks` needs the resting order's side+price,
//!     which `cancel_events`' `Canceled` event omits). Reading the engine's own
//!     `orders` map keeps the adapter free of any duplicated book state, so a
//!     resting/side/price disagreement surfaces as an ENGINE bug rather than
//!     being masked (or invented) by an adapter-side shadow. See gocronx
//!     adapter README + build.sh for the patch.
//!
//! Synthesised above the engine (the engine emits none of these in the harness
//! wire format):
//!   - OrderAck (= `Accepted`), CancelAck-for-IOC-residual, ModifyAck,
//!     ModifyReject. The engine drops an IOC residual internally with NO event;
//!     the adapter computes the residual = order_qty - sum(filled) and emits the
//!     harness CancelAck for it (Trades first, then the CancelAck — same
//!     emission order as the C++ reference adapters).
//!
//! ## Modify
//!
//! The harness defines modify as cancel + reinsert with queue priority lost,
//! and every canonical modify is a quantity *increase* (often also a reprice).
//! The engine's native `amend` is unsuitable: it rejects quantity increases and
//! rejects crossing reprices (maker-only semantics) — NOT the harness contract.
//! So the adapter performs the contract literally: `cancel_events(id)`; on
//! `Canceled` emit ModifyAck then re-`submit_events` a fresh GTC Limit at the
//! new price/qty (any crossing fills emit as Trades tagged with the modify's
//! seq; the reinsert's `Accepted` is dropped — a modify yields one ModifyAck,
//! not an OrderAck); on `CancelRejected` emit ModifyReject. This is exactly what
//! the liquibook baseline and every reference adapter do.
//!
//! ## Threads / state
//!
//! The harness drives every `engine_on_*` / `engine_query_*` call from ONE
//! matcher thread (only the drainer touches the transport), so the adapter
//! state needs no synchronization. The book + transport live in
//! single-thread-owned cells (`UnsafeCell` behind a `Sync` shim — the
//! `ThreadOwned` idiom shared with the orderbookrs/philipgreat adapters); no
//! lock and no atomic on the hot path.

use matcher::{BookEvent, Order, OrderBook, Price, Quantity, Side};
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

// const-assert the layout at compile time.
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
// Adapter state — single-thread-owned globals (see module doc).
// =============================================================================

struct Transport {
    vtable: *const MeTransport,
    sink: *mut c_void,
}

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
    /// Shared borrow. SAFETY: single matcher thread, `init` ran first.
    #[inline(always)]
    unsafe fn get_ref(&self) -> &T {
        (*self.0.get()).as_ref().unwrap_unchecked()
    }
    /// Mutable borrow. SAFETY: single matcher thread, `init` ran first, and the
    /// harness never re-enters an engine_on_* call from inside another.
    #[inline(always)]
    #[allow(clippy::mut_from_ref)]
    unsafe fn get_mut(&self) -> &mut T {
        (*self.0.get()).as_mut().unwrap_unchecked()
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

/// Translate a `BookEvent::Trade` into a harness ME_TRADE report and emit it.
/// `seq` is the aggressive (incoming) message's sequence_number. The maker is
/// the resting order, the taker is the aggressor; the engine tags each Trade
/// with the aggressor side and the two ids (buy_id / sell_id), so the taker is
/// the buy_id for a Buy aggressor and the sell_id for a Sell aggressor.
#[inline]
fn emit_trade(seq: u64, t: &matcher::Trade) {
    let (taker, maker) = match t.aggressor {
        Side::Buy => (t.buy_id.get(), t.sell_id.get()),
        Side::Sell => (t.sell_id.get(), t.buy_id.get()),
    };
    let r = Report {
        r#type: ME_TRADE,
        side: 0, // not part of the Trade wire line (1,seq,price,qty,maker,taker)
        _reserved: [0; 6],
        sequence_number: seq,
        order_id: 0,
        price_ticks: t.price.get() as i64, // maker's resting price
        quantity: t.quantity.get() as u32,
        _reserved2: 0,
        maker_order_id: maker,
        taker_order_id: taker,
        _reserved3: 0,
    };
    emit(&r);
}

// =============================================================================
// engine_init / engine_shutdown / engine_flush
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_init(_seed: u64, transport: *const MeTransport, sink: *mut c_void) {
    // SAFETY: init phase — the harness has not started delivering messages yet.
    unsafe { TRANSPORT.init(Transport { vtable: transport, sink }) };
    unsafe { BOOK.init(OrderBook::new()) };
}

#[no_mangle]
pub extern "C" fn engine_shutdown() {
    // The harness loads, runs once, and tears down. The ThreadOwned cells live
    // for the process lifetime; do nothing here (mirrors the C++/Rust adapters).
}

#[no_mangle]
pub extern "C" fn engine_flush() {
    // Synchronous matcher: every submit_events / cancel_events runs and emits
    // inline before returning. Nothing is pending.
}

// =============================================================================
// Hot path
// =============================================================================

#[no_mangle]
pub unsafe extern "C" fn engine_on_new_order(order: *const NewOrder) {
    let o = unsafe { &*order };
    let seq = o.sequence_number;

    // SAFETY: single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_mut() };
    let side = if o.side == 0 { Side::Buy } else { Side::Sell };

    // Engine-native order kind: harness IOC -> the engine's Ioc (matches what
    // it can, drops the residual itself with no event); otherwise a GTC Limit.
    let order = if o.ioc != 0 {
        Order::ioc(o.order_id, side, o.price_ticks as u64, o.quantity as u64)
    } else {
        Order::limit(o.order_id, side, o.price_ticks as u64, o.quantity as u64)
    };

    let events = book.submit_events(order, seq);

    // Map the event stream. Accepted -> OrderAck; Trade -> ME_TRADE; Rested is
    // not a harness report. Accumulate filled qty for the IOC residual.
    let mut filled: u64 = 0;
    for ev in &events {
        match ev {
            BookEvent::Accepted { order_id } => {
                emit_ack(ME_ORDER_ACK, seq, order_id.get(), o.side, o.price_ticks, o.quantity);
            }
            BookEvent::Trade(t) => {
                filled = filled.saturating_add(t.quantity.get());
                emit_trade(seq, t);
            }
            // Rested: residual is on the book; the engine owns it. No report.
            // Rejected: a valid canonical new order is never rejected (unique
            // ids, valid price/qty, no PostOnly/FOK in the workload); if one
            // ever were, no OrderAck is emitted (Accepted was not produced),
            // which is the faithful "not accepted" outcome.
            _ => {}
        }
    }

    // IOC residual: the engine already dropped it (native Ioc), emitting no
    // event. Synthesise the harness CancelAck for the unfilled remainder.
    if o.ioc != 0 {
        let residual = (o.quantity as u64).saturating_sub(filled) as u32;
        if residual > 0 {
            emit_ack(ME_CANCEL_ACK, seq, o.order_id, o.side, o.price_ticks, residual);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_cancel(cancel: *const CancelMsg) {
    let c = unsafe { &*cancel };
    // SAFETY: single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_mut() };

    // Read the resting order's side+price BEFORE removing it (the CancelAck
    // wire line needs both). get_order_se reads the engine's own orders map; a
    // miss here matches cancel_events' CancelRejected below.
    let se = book.get_order_se(c.order_id);

    match book.cancel_events(c.order_id, c.sequence_number).first() {
        Some(BookEvent::Canceled { .. }) => {
            let (side, price) = se.expect("Canceled implies the order was resting");
            emit_ack(
                ME_CANCEL_ACK,
                c.sequence_number,
                c.order_id,
                match side {
                    Side::Buy => 0,
                    Side::Sell => 1,
                },
                price.get() as i64,
                0, // CancelAck wire line omits quantity (2,seq,side,id,price)
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
    let book = unsafe { BOOK.get_mut() };

    // Modify = cancel + reinsert (harness contract). The engine's id-keyed
    // cancel is the resting test: Canceled -> proceed; CancelRejected -> reject.
    match book.cancel_events(m.order_id, m.sequence_number).first() {
        Some(BookEvent::Canceled { .. }) => {
            let side = if m.side == 0 { Side::Buy } else { Side::Sell };
            emit_ack(
                ME_MODIFY_ACK,
                m.sequence_number,
                m.order_id,
                m.side,
                m.new_price_ticks,
                m.new_quantity,
            );
            // Re-add a fresh GTC Limit at the new price/qty on the same side.
            // Any crossing fills emit as Trades tagged with the modify's seq;
            // the reinsert's Accepted/Rested carry no harness report (a modify
            // yields exactly one ModifyAck, not an OrderAck).
            let reinsert = Order::limit(
                m.order_id,
                side,
                m.new_price_ticks as u64,
                m.new_quantity as u64,
            );
            for ev in &book.submit_events(reinsert, m.sequence_number) {
                if let BookEvent::Trade(t) = ev {
                    emit_trade(m.sequence_number, t);
                }
            }
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
        Some(p) => p.get() as i64,
        None => i64::MIN,
    }
}

#[no_mangle]
pub extern "C" fn engine_query_best_ask() -> i64 {
    // SAFETY: queries run on the same single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_ref() };
    match book.best_ask() {
        Some(p) => p.get() as i64,
        None => i64::MAX,
    }
}

#[no_mangle]
pub extern "C" fn engine_query_depth_at(price_ticks: i64, side: c_uchar) -> u64 {
    if price_ticks < 0 {
        return 0; // engine prices are u64 >= 0; no negative level exists
    }
    // SAFETY: queries run on the same single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_ref() };
    let s = if side == 0 { Side::Buy } else { Side::Sell };
    book.level_qty(s, Price::new(price_ticks as u64)).get()
}

// Suppress an unused-import lint if Quantity ends up referenced only via traits.
#[allow(dead_code)]
fn _quantity_marker(_: Quantity) {}
