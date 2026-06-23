//! dgtony_adapter — dgtony/orderbook-rs behind the harness
//! matching_engine_api.h ABI.
//!
//! The engine is pure Rust, so this whole adapter is one Rust cdylib that
//! exports the harness `engine_*` extern-C symbols directly. No C++ shim.
//!
//! ENGINE SHAPE (src/engine/orderbook.rs). The whole public surface is
//! `Orderbook::process_order(OrderRequest) -> Vec<Result<Success, Failed>>`:
//! a synchronous, single-threaded reactor that consumes one request and
//! returns the events it generated. Bids/asks are price-time-priority
//! `BinaryHeap` index queues with a side `HashMap<id, Order>` (order_queues.rs).
//!
//! WHAT THE ADAPTER USES NATIVELY:
//!   - matching + resting + cancel: the engine's own `process_limit_order` /
//!     `process_order_cancel` (reached through two thin pub wrappers added by
//!     the build-time patch — see below; they bypass ONLY the engine's
//!     id-generator, nothing in the matching path).
//!   - best bid / best ask: `bid_queue.peek()` / `ask_queue.peek()` (the heap
//!     top, with the engine's own lazy stale-index cleanup).
//!   - depth at a price: a sum over the side queue's live `orders` map.
//!
//! ENGINE PATCH (documented in build.sh, applied idempotently). The engine has
//! three API gaps the harness exercises; all three are added as *new* pub
//! methods that reuse the engine's existing private matching helpers — no
//! existing line is changed:
//!   1. Caller-supplied order ids. `process_order` assigns ids from an internal
//!      `TradeSequence` that ROTATES in [1, 1000] (sequence.rs) and ignores the
//!      caller; past 1000 live orders the generated ids collide and `insert`
//!      silently drops the order as a duplicate. The harness supplies 32-bit
//!      ids up to the order count (300k). `submit_limit` / `submit_cancel`
//!      thread the harness id straight into `process_limit_order` /
//!      `process_order_cancel`, so the rotating generator and its [1,1000]
//!      validation bound are never on the path.
//!   2. best_bid / best_ask. `current_spread()` returns `None` unless BOTH
//!      sides are populated; the harness needs each side independently.
//!   3. depth_at(price, side). No native aggregated-depth query exists.
//!
//! SYNTHESISED ABOVE THE ENGINE (engine emits none of these in the harness
//! wire format): OrderAck, CancelAck (incl. IOC residual), ModifyAck,
//! CancelReject, ModifyReject. IOC is synthesised too — the engine has no
//! IOC/FOK time-in-force, so an IOC order is matched as a plain limit and its
//! rested residual is then cancelled out of the engine and reported as a
//! CancelAck (the documented "match what you can, drop the rest").
//!
//! LIVENESS SHADOW (permitted minimal per-order state). The engine's cancel
//! takes the order's SIDE to pick a queue, and its `Cancelled` event carries
//! only the id+ts — but the harness CancelAck line is `2,seq,side,order_id,
//! price`, needing side + price, and `cancel_t` supplies neither. So the
//! adapter keeps `order_id -> (side, price)` for every RESTING order: it is the
//! side argument for the engine cancel and the side/price echo for the
//! CancelAck. The ENGINE stays authoritative for liveness — a cancel of an id
//! the engine has already filled returns OrderNotFound and we reject; the
//! shadow is only ever a superset, reconciled from the fill events (a maker
//! reported `Filled`, i.e. exhausted, is dropped from the shadow).

use std::cell::UnsafeCell;
use std::collections::HashMap;
use std::os::raw::{c_uchar, c_uint, c_void};
use std::time::SystemTime;

// The engine crate re-exports these at its root (src/lib.rs):
//   pub use engine::domain::OrderSide;
//   pub use engine::orderbook::{Orderbook, OrderProcessingResult, Success, Failed};
use orderbook::{Failed, Orderbook, OrderSide, Success};

// =============================================================================
// Asset — the engine is generic over an asset enum (must be Debug+Clone+Copy+Eq).
// The harness is one instrument, so a single-variant asset suffices; every
// request uses (ASSET, ASSET) and the engine's asset validation always passes.
// =============================================================================
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Asset {
    Instrument,
}
const ASSET: Asset = Asset::Instrument;

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
// Adapter state — single matcher thread, no synchronization (mirrors the
// orderbookrs reference adapter's ThreadOwned pattern).
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
    /// SAFETY: single matcher thread, init phase, no other live access.
    #[inline(always)]
    unsafe fn init(&self, value: T) {
        *self.0.get() = Some(value);
    }
    /// SAFETY: single matcher thread, `init` ran first.
    #[inline(always)]
    unsafe fn get_mut(&self) -> &mut T {
        (*self.0.get()).as_mut().unwrap_unchecked()
    }
}

static TRANSPORT: ThreadOwned<Transport> = ThreadOwned::new();
static BOOK: ThreadOwned<Orderbook<Asset>> = ThreadOwned::new();
// Liveness shadow: resting order_id -> (side, price_ticks). See module docs.
static SHADOW: ThreadOwned<HashMap<u64, (u8, i64)>> = ThreadOwned::new();

#[inline]
fn emit(r: &Report) {
    // SAFETY: matcher thread only; TRANSPORT.init ran in engine_init.
    let t = unsafe { TRANSPORT.get_mut() };
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
fn emit_trade(seq: u64, price: i64, qty: u32, maker: u64, taker: u64) {
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
        taker_order_id: taker,
        _reserved3: 0,
    };
    emit(&r);
}

#[inline]
fn side_to_u8(s: OrderSide) -> u8 {
    match s {
        OrderSide::Bid => 0,
        OrderSide::Ask => 1,
    }
}

#[inline]
fn u8_to_side(s: u8) -> OrderSide {
    if s == 0 {
        OrderSide::Bid
    } else {
        OrderSide::Ask
    }
}

/// Translate the engine's event vector for a NEW limit order into harness
/// reports, reconcile the liveness shadow, and return the taker's total filled
/// quantity. `events[0]` is the `Accepted`; the remainder is a flat run of
/// (taker_event, maker_event) pairs, one pair per maker consumed in match
/// order (see order_matching in orderbook.rs). Each pair becomes exactly one
/// Trade; the maker event (`pair.1`) carries the maker id, the fill price
/// (maker's resting price) and the fill quantity, and whether the maker was
/// `Filled` (exhausted -> drop from shadow) or `PartiallyFilled` (still
/// resting). The OrderAck is emitted by the caller, not here.
fn translate_fills(
    seq: u64,
    taker_id: u64,
    events: &[Result<Success, Failed>],
    shadow: &mut HashMap<u64, (u8, i64)>,
) -> u64 {
    let mut total_filled: u64 = 0;
    // Skip events[0] (Accepted); walk the rest in (taker, maker) pairs.
    let mut i = 1;
    while i + 1 < events.len() {
        // The maker event is the second of the pair.
        let maker_ev = match &events[i + 1] {
            Ok(ev) => ev,
            _ => break, // not a well-formed pair (no further fills)
        };
        let (maker_id, price, qty, maker_exhausted) = match maker_ev {
            Success::Filled {
                order_id,
                price,
                qty,
                ..
            } => (*order_id, *price, *qty, true),
            Success::PartiallyFilled {
                order_id,
                price,
                qty,
                ..
            } => (*order_id, *price, *qty, false),
            _ => break,
        };
        let price_ticks = price.round() as i64;
        let fill_qty = qty.round() as u32;
        emit_trade(seq, price_ticks, fill_qty, maker_id, taker_id);
        total_filled += fill_qty as u64;
        if maker_exhausted {
            shadow.remove(&maker_id);
        }
        i += 2;
    }
    total_filled
}

// =============================================================================
// engine_init / engine_shutdown
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_init(_seed: u64, transport: *const MeTransport, sink: *mut c_void) {
    // SAFETY: init phase — the harness has not started delivering messages yet.
    unsafe {
        TRANSPORT.init(Transport {
            vtable: transport,
            sink,
        });
        BOOK.init(Orderbook::new(ASSET, ASSET));
        SHADOW.init(HashMap::with_capacity(1 << 16));
    }
}

#[no_mangle]
pub extern "C" fn engine_shutdown() {}

#[no_mangle]
pub extern "C" fn engine_flush() {
    // Synchronous engine: process_order runs and every event is translated and
    // pushed inline before each hot-path call returns. Nothing to drain.
}

// =============================================================================
// Hot path
// =============================================================================

#[no_mangle]
pub unsafe extern "C" fn engine_on_new_order(order: *const NewOrder) {
    let o = unsafe { &*order };
    // OrderAck first (canonical (seq,type) sort makes intra-message order moot,
    // but keep the reference convention: Ack, then Trades, then IOC residual).
    emit_ack(
        ME_ORDER_ACK,
        o.sequence_number,
        o.order_id,
        o.side,
        o.price_ticks,
        o.quantity,
    );

    // SAFETY: single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_mut() };
    let shadow = unsafe { SHADOW.get_mut() };
    let side = u8_to_side(o.side);
    let events = book.submit_limit(
        o.order_id,
        side,
        o.price_ticks as f64,
        o.quantity as f64,
        SystemTime::now(),
    );

    let filled = translate_fills(o.sequence_number, o.order_id, &events, shadow);
    let residual = (o.quantity as u64).saturating_sub(filled);

    if o.ioc != 0 {
        // The engine has no IOC: any residual rested in the book. Remove it and
        // report the harness CancelAck for the unfilled remainder.
        if residual > 0 {
            // The residual rested under this id on `side`; cancel it back out.
            let _ = book.submit_cancel(o.order_id, side);
            emit_ack(
                ME_CANCEL_ACK,
                o.sequence_number,
                o.order_id,
                o.side,
                o.price_ticks,
                residual as u32,
            );
        }
        // IOC never rests -> not added to the shadow.
        return;
    }

    // GTC limit: if anything is left unfilled the engine rested it at its own
    // limit price; record it for cancel/modify side+price echo + liveness.
    if residual > 0 {
        shadow.insert(o.order_id, (o.side, o.price_ticks));
    }
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_cancel(cancel: *const CancelMsg) {
    let c = unsafe { &*cancel };
    // SAFETY: single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_mut() };
    let shadow = unsafe { SHADOW.get_mut() };

    // Not in the shadow => the order is not resting (never seen, fully filled,
    // or already cancelled). The engine cancel also needs the side to pick a
    // queue, which only the shadow has — so this is the reject path.
    match shadow.get(&c.order_id).copied() {
        Some((side_u8, price)) => {
            let side = u8_to_side(side_u8);
            let events = book.submit_cancel(c.order_id, side);
            // The engine is authoritative: Cancelled => ack; OrderNotFound (it
            // was consumed by a fill the shadow had not yet reconciled, which
            // cannot happen here because makers are reconciled on Filled, but
            // we honour the engine's answer regardless) => reject.
            let cancelled = events
                .iter()
                .any(|e| matches!(e, Ok(Success::Cancelled { .. })));
            shadow.remove(&c.order_id);
            if cancelled {
                emit_ack(
                    ME_CANCEL_ACK,
                    c.sequence_number,
                    c.order_id,
                    side_u8,
                    price,
                    0,
                );
            } else {
                emit_ack(ME_CANCEL_REJECT, c.sequence_number, c.order_id, 0, 0, 0);
            }
        }
        None => {
            emit_ack(ME_CANCEL_REJECT, c.sequence_number, c.order_id, 0, 0, 0);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_modify(modify: *const ModifyMsg) {
    let m = unsafe { &*modify };
    // SAFETY: single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_mut() };
    let shadow = unsafe { SHADOW.get_mut() };

    // Modify = cancel + reinsert (harness contract: lose queue priority, and
    // re-match if the reprice now crosses). The shadow is the resting test and
    // the source of the order's current side.
    let cur = shadow.get(&m.order_id).copied();
    let side_u8 = match cur {
        Some((s, _)) => s,
        None => {
            emit_ack(ME_MODIFY_REJECT, m.sequence_number, m.order_id, 0, 0, 0);
            return;
        }
    };
    let side = u8_to_side(side_u8);

    // Cancel the resting order out of the engine.
    let cancel_events = book.submit_cancel(m.order_id, side);
    let was_resting = cancel_events
        .iter()
        .any(|e| matches!(e, Ok(Success::Cancelled { .. })));
    if !was_resting {
        // Engine disagrees that it was resting -> reject (shadow stale).
        shadow.remove(&m.order_id);
        emit_ack(ME_MODIFY_REJECT, m.sequence_number, m.order_id, 0, 0, 0);
        return;
    }

    // ModifyAck carries the modify's side + NEW price/qty (line 3,seq,side,
    // order_id,price,qty). The harness modify keeps the order's side; echo it.
    emit_ack(
        ME_MODIFY_ACK,
        m.sequence_number,
        m.order_id,
        side_u8,
        m.new_price_ticks,
        m.new_quantity,
    );

    // Reinsert at the new price/qty, same id, same side. Routes through the
    // engine's matching, so any crossing fills emit as Trades stamped with the
    // modify's seq.
    let events = book.submit_limit(
        m.order_id,
        side,
        m.new_price_ticks as f64,
        m.new_quantity as f64,
        SystemTime::now(),
    );
    let filled = translate_fills(m.sequence_number, m.order_id, &events, shadow);
    let residual = (m.new_quantity as u64).saturating_sub(filled);
    if residual > 0 {
        shadow.insert(m.order_id, (side_u8, m.new_price_ticks));
    } else {
        shadow.remove(&m.order_id);
    }
}

// =============================================================================
// Audit queries
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_query_best_bid() -> i64 {
    // SAFETY: queries run on the same single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_mut() };
    match book.best_bid() {
        Some(p) => p.round() as i64,
        None => i64::MIN,
    }
}

#[no_mangle]
pub extern "C" fn engine_query_best_ask() -> i64 {
    // SAFETY: queries run on the same single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_mut() };
    match book.best_ask() {
        Some(p) => p.round() as i64,
        None => i64::MAX,
    }
}

#[no_mangle]
pub extern "C" fn engine_query_depth_at(price_ticks: i64, side: c_uchar) -> u64 {
    // SAFETY: queries run on the same single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_mut() };
    let s = u8_to_side(side);
    book.depth_at(price_ticks as f64, s).round() as u64
}
