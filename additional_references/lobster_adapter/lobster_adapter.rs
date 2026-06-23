//! lobster_adapter — rubik/lobster behind the harness matching_engine_api.h ABI.
//!
//! The engine (`lobster`) is pure Rust, so this whole adapter is one Rust
//! cdylib that exports the harness `engine_*` extern-C symbols directly. No C++
//! shim.
//!
//! What the engine provides natively (used here):
//!   - `OrderBook::execute(OrderType) -> OrderEvent` — synchronous price-time
//!     matcher. For a `Limit` it matches the incoming order against the book
//!     and rests any residual, returning the event plus a `Vec<FillMetadata>`
//!     describing every fill (`order_1` = taker id, `order_2` = maker id,
//!     `price` = maker's resting price, `qty`, `total_fill`). That fill vector
//!     IS the harness Trade stream, field for field.
//!   - `OrderBook::execute(OrderType::Cancel{id})` — removes the order by its
//!     external u128 id. (It always returns `Canceled` regardless of whether
//!     the id was resting — see the liveness shadow note below.)
//!   - `OrderBook::min_ask()` / `max_bid()` — `Option<u64>`, the live best
//!     ask / bid, used directly for the best-ask / best-bid queries.
//!   - `OrderBook::depth(levels)` — a `BookDepth` of every non-empty price
//!     level on both sides (the `levels` arg is ignored upstream), scanned for
//!     the exact price the depth query asks for.
//!
//! Native order ids: lobster keys every order by a caller-supplied `u128`
//! external id (its `OrderArena.order_map`), and echoes that same id back in
//! every `FillMetadata` (`order_1` / `order_2`). The harness `uint64_t` ids
//! widen losslessly to `u128`, so trade reports recover the harness ids
//! directly with no sidecar map.
//!
//! Synthesised above the engine (lobster emits none of these in the harness
//! wire format): OrderAck, CancelAck (incl. IOC-residual), ModifyAck,
//! CancelReject, ModifyReject.
//!
//! IOC: lobster has no native immediate-or-cancel. A harness IOC new order is
//! submitted as a plain `Limit` — which matches what it can and RESTS the
//! residual — and the adapter then removes that residual with a follow-up
//! `Cancel{id}` (its event discarded) and emits the harness IOC-residual
//! CancelAck. An IOC id is never recorded as live, so it never rests in the
//! harness view.
//!
//! Modify contract: the harness defines modify as cancel + reinsert (queue
//! priority lost). The adapter does exactly that against lobster: `Cancel{id}`
//! then a fresh `Limit{id, new_price, new_qty}` on the same side, so any
//! crossing fills on the reinsert emit as Trades, followed by one ModifyAck.
//!
//! Liveness shadow: lobster's public `execute(Cancel{..})` ALWAYS returns
//! `OrderEvent::Canceled`, even for an id that is not resting (already filled,
//! already cancelled, or never seen) — its internal not-found bool is dropped
//! before it reaches the public API. The harness needs CancelReject /
//! ModifyReject for exactly those cases, so the adapter keeps a MINIMAL
//! per-order liveness set (a flat `Vec<bool>` indexed by the dense harness id):
//! an id is set live when a GTC limit rests a residual, cleared when it is
//! fully consumed by a later incoming order, cancelled, or modified. This is
//! the only adapter-side order state, and it exists solely because the engine
//! API cannot report not-resting — exactly the case the brief permits a shadow.

use lobster::{OrderBook, OrderEvent, OrderType, Side};
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

struct Transport {
    vtable: *const MeTransport,
    sink: *mut c_void,
}

// Single-thread-owned global. The harness drives every engine_on_* / query_*
// call from ONE matcher thread (the drainer touches only the transport), so the
// adapter state needs no synchronization. A `static` still requires `Sync`, so
// this cell provides interior mutability via `UnsafeCell` with the
// single-thread-ownership invariant documented at each accessor. No lock and no
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
    /// SAFETY: single matcher thread, `init` ran first.
    #[inline(always)]
    #[allow(clippy::mut_from_ref)]
    unsafe fn get_mut(&self) -> &mut T {
        (*self.0.get()).as_mut().unwrap_unchecked()
    }
}

static TRANSPORT: ThreadOwned<Transport> = ThreadOwned::new();
// The lobster OrderBook itself: single instance, owned by the matcher thread.
// execute() takes &mut self, so this needs the mutable accessor.
static BOOK: ThreadOwned<OrderBook> = ThreadOwned::new();
// Minimal liveness shadow — see the module note. Flat vector indexed by the
// dense harness order id. `live` flags whether this id currently has a resting
// residual; `side` / `price` echo the resting order's identity into a
// successful CancelAck (the canonical CancelAck line is `2,seq,side,id,price`,
// and lobster's own Cancel event carries neither field). Sized once in
// engine_init (capacity only, untimed). The only adapter-side order state,
// present solely because lobster's public API reports neither not-resting nor
// the cancelled order's side/price.
#[derive(Copy, Clone, Default)]
struct Shadow {
    live: bool,
    side: u8,
    price: i64,
}
static LIVE: ThreadOwned<Vec<Shadow>> = ThreadOwned::new();

thread_local! {
    static CUR_SEQ: Cell<u64> = const { Cell::new(0) };
}

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

/// Record a resting order in the shadow (live, with its side + price for the
/// CancelAck echo), growing the table if needed. Growth is off the steady-state
/// hot path: the table is pre-sized in engine_init to cover the dense id range.
#[inline]
fn set_resting(live: &mut Vec<Shadow>, id: u64, side: u8, price: i64) {
    let i = id as usize;
    if i >= live.len() {
        live.resize(i + 1, Shadow::default());
    }
    live[i] = Shadow { live: true, side, price };
}

/// Clear an id's liveness (no longer resting). Side/price are left stale but
/// unread while `live` is false.
#[inline]
fn clear_live(live: &mut Vec<Shadow>, id: u64) {
    let i = id as usize;
    if i < live.len() {
        live[i].live = false;
    }
}

#[inline]
fn get_resting(live: &[Shadow], id: u64) -> Option<(u8, i64)> {
    let i = id as usize;
    if i < live.len() && live[i].live {
        Some((live[i].side, live[i].price))
    } else {
        None
    }
}

/// Emit one harness Trade per lobster FillMetadata, in match order, and clear
/// the liveness flag of any maker this fill fully consumed (so a later
/// cancel/modify of that maker correctly rejects). Returns the total filled
/// quantity (used to size the IOC residual).
#[inline]
fn emit_fills(
    live: &mut Vec<Shadow>,
    seq: u64,
    taker_oid: u64,
    fills: &[lobster::FillMetadata],
) -> u64 {
    let mut filled: u64 = 0;
    for f in fills {
        let r = Report {
            r#type: ME_TRADE,
            side: 0,
            _reserved: [0; 6],
            sequence_number: seq,
            order_id: 0,
            price_ticks: f.price as i64,
            quantity: f.qty as u32,
            _reserved2: 0,
            maker_order_id: f.order_2 as u64, // resting / maker
            taker_order_id: taker_oid,
            _reserved3: 0,
        };
        emit(&r);
        filled += f.qty;
        // A maker that this fill totally filled is gone from the book.
        if f.total_fill {
            clear_live(live, f.order_2 as u64);
        }
    }
    filled
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
    unsafe {
        TRANSPORT.init(Transport { vtable: transport, sink });
        // arena_capacity / queue_capacity sized generously up front so the
        // engine's own preallocation covers the standard workload without a
        // mid-run regrow. track_stats stays OFF — the harness never reads
        // last_trade / traded_volume, and tracking only adds per-call work.
        BOOK.init(OrderBook::new(1 << 21, 16, false));
        // Liveness shadow pre-sized to the dense id range (capacity only).
        let mut live: Vec<Shadow> = Vec::new();
        live.resize(1 << 21, Shadow::default());
        LIVE.init(live);
    }
}

#[no_mangle]
pub extern "C" fn engine_shutdown() {
    // The harness loads, runs once, and tears down. The ThreadOwned cells live
    // for the process lifetime; nothing to free here.
}

#[no_mangle]
pub extern "C" fn engine_flush() {
    // Synchronous matcher: every execute() runs and emits inline before
    // returning. Nothing to drain.
}

// =============================================================================
// Hot path
// =============================================================================

#[no_mangle]
pub unsafe extern "C" fn engine_on_new_order(order: *const NewOrder) {
    let o = unsafe { &*order };
    // OrderAck first so the canonical (Ack, Trades, optional residual CancelAck)
    // emission order matches the reference adapters.
    emit_ack(
        ME_ORDER_ACK,
        o.sequence_number,
        o.order_id,
        o.side,
        o.price_ticks,
        o.quantity,
    );

    CUR_SEQ.with(|s| s.set(o.sequence_number));
    // SAFETY: single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_mut() };
    let live = unsafe { LIVE.get_mut() };
    let side = if o.side == 0 { Side::Bid } else { Side::Ask };

    // Every harness new order is a limit order (the workload sends no market
    // orders). lobster has no native IOC, so an IOC is matched as a plain limit
    // and its rested residual is removed below.
    let ev = book.execute(OrderType::Limit {
        id: o.order_id as u128,
        side,
        qty: o.quantity as u64,
        price: o.price_ticks as u64, // harness ticks are non-negative: lossless
    });

    let filled = match &ev {
        OrderEvent::Filled { fills, .. } | OrderEvent::PartiallyFilled { fills, .. } => {
            emit_fills(live, o.sequence_number, o.order_id, fills)
        }
        _ => 0,
    };

    if o.ioc != 0 {
        // Native lobster rested any residual; the harness drops it. Remove the
        // resting residual (event discarded — it is not part of the IOC report
        // contract) and synthesise the IOC-residual CancelAck. The id is never
        // recorded live, so a (contractually absent) later cancel rejects.
        let residual = (o.quantity as u64).saturating_sub(filled);
        if residual > 0 {
            let _ = book.execute(OrderType::Cancel { id: o.order_id as u128 });
            emit_ack(
                ME_CANCEL_ACK,
                o.sequence_number,
                o.order_id,
                o.side,
                o.price_ticks,
                residual as u32,
            );
        }
        return;
    }

    // GTC limit: a residual rested iff the order did not fully fill. Placed and
    // PartiallyFilled both leave a resting remainder; Filled leaves nothing.
    match &ev {
        OrderEvent::Placed { .. } | OrderEvent::PartiallyFilled { .. } => {
            set_resting(live, o.order_id, o.side, o.price_ticks);
        }
        _ => {
            // Filled (no residual) or Unfilled (unreachable for a limit): the
            // order is not resting, so a later cancel of it must reject.
            clear_live(live, o.order_id);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_cancel(cancel: *const CancelMsg) {
    let c = unsafe { &*cancel };
    // SAFETY: single matcher thread.
    let book = unsafe { BOOK.get_mut() };
    let live = unsafe { LIVE.get_mut() };

    if let Some((side, price)) = get_resting(live, c.order_id) {
        // Resting; remove it from the book. The canonical CancelAck line is
        // `2,seq,side,id,price` (quantity omitted), so echo the resting order's
        // side + price recorded in the shadow — lobster's own Cancel event
        // carries neither.
        let _ = book.execute(OrderType::Cancel { id: c.order_id as u128 });
        clear_live(live, c.order_id);
        emit_ack(ME_CANCEL_ACK, c.sequence_number, c.order_id, side, price, 0);
    } else {
        // Not resting — already filled, already cancelled, or never seen.
        emit_ack(ME_CANCEL_REJECT, c.sequence_number, c.order_id, 0, 0, 0);
    }
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_modify(modify: *const ModifyMsg) {
    let m = unsafe { &*modify };
    // SAFETY: single matcher thread.
    let book = unsafe { BOOK.get_mut() };
    let live = unsafe { LIVE.get_mut() };

    if get_resting(live, m.order_id).is_some() {
        // Cancel + reinsert (the harness modify contract — queue priority lost).
        let _ = book.execute(OrderType::Cancel { id: m.order_id as u128 });
        clear_live(live, m.order_id);

        CUR_SEQ.with(|s| s.set(m.sequence_number));
        let side = if m.side == 0 { Side::Bid } else { Side::Ask };
        let ev = book.execute(OrderType::Limit {
            id: m.order_id as u128,
            side,
            qty: m.new_quantity as u64,
            price: m.new_price_ticks as u64,
        });

        // Crossing fills on the reinsert emit as Trades (before the ModifyAck —
        // the canonical sort groups them under this message's seq either way).
        match &ev {
            OrderEvent::Filled { fills, .. } | OrderEvent::PartiallyFilled { fills, .. } => {
                emit_fills(live, m.sequence_number, m.order_id, fills);
            }
            _ => {}
        }
        // A residual rests unless the reinsert fully filled.
        match &ev {
            OrderEvent::Placed { .. } | OrderEvent::PartiallyFilled { .. } => {
                set_resting(live, m.order_id, m.side, m.new_price_ticks);
            }
            _ => {}
        }

        emit_ack(
            ME_MODIFY_ACK,
            m.sequence_number,
            m.order_id,
            m.side,
            m.new_price_ticks,
            m.new_quantity,
        );
    } else {
        emit_ack(ME_MODIFY_REJECT, m.sequence_number, m.order_id, 0, 0, 0);
    }
}

// =============================================================================
// Audit queries
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_query_best_bid() -> i64 {
    // SAFETY: queries run on the same single matcher thread.
    let book = unsafe { BOOK.get_mut() };
    match book.max_bid() {
        Some(p) => p as i64,
        None => i64::MIN,
    }
}

#[no_mangle]
pub extern "C" fn engine_query_best_ask() -> i64 {
    // SAFETY: queries run on the same single matcher thread.
    let book = unsafe { BOOK.get_mut() };
    match book.min_ask() {
        Some(p) => p as i64,
        None => i64::MAX,
    }
}

#[no_mangle]
pub extern "C" fn engine_query_depth_at(price_ticks: i64, side: c_uchar) -> u64 {
    if price_ticks < 0 {
        return 0;
    }
    // SAFETY: queries run on the same single matcher thread.
    let book = unsafe { BOOK.get_mut() };
    let p = price_ticks as u64;
    // lobster's depth() walks both sides and merges same-price levels, skipping
    // empty (zero-qty) levels; scan it for the exact price requested. The
    // `levels` argument only pre-sizes depth()'s result Vec (the walk covers
    // the whole book regardless), so pass 0 and let the Vec grow — usize::MAX
    // would overflow Vec::with_capacity. Audit queries are rare, so the
    // per-call BookDepth allocation is immaterial.
    let d = book.depth(0);
    let levels = if side == 0 { &d.bids } else { &d.asks };
    for lvl in levels {
        if lvl.price == p {
            return lvl.qty;
        }
    }
    0
}
