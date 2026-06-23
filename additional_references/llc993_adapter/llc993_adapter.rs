//! llc993_adapter — llc-993/matching-core behind the harness
//! `matching_engine_api.h` ABI.
//!
//! The engine is pure Rust, so this whole adapter is one Rust cdylib that
//! exports the harness `engine_*` extern-C symbols directly. No C++ shim.
//!
//! ENGINE UNDER TEST: `DirectOrderBook` — the engine's production / default
//! matcher (the book `MatchingEngineRouter::add_symbol` instantiates, the one
//! the source comments describe as "逻辑参考 exchange-core" / referencing
//! exchange-core, and the headline implementation in `exchange_bench`). It is
//! a price-time-priority CLOB: BTreeMap price -> bucket index, a Slab order
//! pool with an intrusive doubly-linked time queue, an AHashMap id index, and
//! cached best-bid / best-ask order handles. We drive it through its native
//! `OrderBook` trait, which IS the matcher (the Disruptor pipeline around it
//! only moves commands across threads — no matching logic lives there), so
//! this is a faithful exercise of the engine's real matching code path.
//!
//! What the engine provides natively (used here):
//!   - `OrderBook::new_order(&mut OrderCommand)` — synchronous; matches the
//!     incoming order against the book, rests a GTC residual or (OrderType::Ioc)
//!     drops it. Each fill is pushed onto `cmd.matcher_events` as a
//!     `MatcherTradeEvent { event_type: Trade, size, price (= MAKER price),
//!     matched_order_id (= maker id), .. }`. The taker id is `cmd.order_id`.
//!   - `OrderBook::cancel_order(&mut OrderCommand)` — natively id-keyed;
//!     returns `Success` (and sets `cmd.action` to the order's side + pushes a
//!     Reject bookkeeping event) or `MatchingUnknownOrderId` when the order is
//!     not resting (already filled / cancelled / never seen).
//!   - `OrderBook::get_order_by_id(id) -> Option<(Price, OrderAction)>` — the
//!     native resting test + the side/price echo source for CancelAck /
//!     ModifyAck and the reject decision. The adapter keeps NO per-order
//!     shadow of its own; every liveness/echo fact comes from the engine.
//!   - `DirectOrderBook` fields are private, but the trait exposes
//!     `get_l2_data`, `get_total_*_volume`, `get_*_buckets_count`. Best
//!     bid/ask + per-price depth are read by walking `get_l2_data(depth)`
//!     against a depth large enough to cover the workload's price band.
//!
//! Synthesised above the engine (engine emits none of these in the harness's
//! wire format): OrderAck, CancelAck (incl. IOC-residual), ModifyAck,
//! CancelReject, ModifyReject.
//!
//! Modify contract: the harness defines modify as cancel + reinsert (queue
//! priority lost). The adapter performs exactly that with two native calls —
//! `cancel_order` then `new_order` (GTC) at the new price/qty on the SAME side.
//! It deliberately does NOT use the engine's `move_order`: that path applies a
//! reserve-price risk check (`cmd.price > order.reserve_price` rejects a bid
//! move), and with reserve_price defaulted to 0 every bid reprice-up would be
//! rejected — not the harness's cancel+reinsert semantics. cancel+reinsert is
//! exactly what every reference adapter does.

use matching_core::api::*;
use matching_core::core::orderbook::{DirectOrderBook, OrderBook};
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

// Single-thread-owned global. The harness drives every engine_on_* / query_*
// call from ONE matcher thread (the drainer touches only the transport), so the
// adapter state needs no synchronization. A `static` requires `Sync`, so this
// cell provides interior mutability via `UnsafeCell` with the
// single-thread-ownership invariant documented at each accessor. No lock and no
// atomic on the hot path — the Rust expression of the C++ reference adapters'
// plain globals (same pattern as the orderbookrs reference adapter).
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
    /// Shared borrow (audit queries — read-only book methods).
    /// SAFETY: single matcher thread, `init` ran first.
    #[inline(always)]
    unsafe fn get_ref(&self) -> &T {
        (*self.0.get()).as_ref().unwrap_unchecked()
    }
    /// Exclusive borrow (hot path — the engine's matcher methods take `&mut`).
    /// SAFETY: single matcher thread, `init` ran first, no other live borrow.
    #[inline(always)]
    #[allow(clippy::mut_from_ref)]
    unsafe fn get_mut(&self) -> &mut T {
        (*self.0.get()).as_mut().unwrap_unchecked()
    }
}

static TRANSPORT: ThreadOwned<Transport> = ThreadOwned::new();
static BOOK: ThreadOwned<DirectOrderBook> = ThreadOwned::new();
// One reusable engine command, owned by the single matcher thread alongside
// BOOK. The engine's matcher methods take `&mut OrderCommand` and only PUSH
// onto `cmd.matcher_events` (they never read prior entries), so the adapter
// holds a single command and resets its scalar fields + `clear()`s the event
// Vec at the top of each hot-path call instead of constructing a fresh
// `OrderCommand`. `OrderCommand::default()` allocates (`matcher_events:
// Vec::with_capacity(4)`); doing that per message — once for new/cancel, twice
// for modify (cancel+reinsert) — was an adapter-side per-message heap alloc on
// the timed path. With reuse that backing Vec is allocated exactly once, in
// engine_init, and `clear()` resets its length without freeing capacity, so the
// hot path does no adapter allocation. CMD and BOOK are disjoint statics, so a
// `&mut CMD` never aliases a `&mut BOOK`.
static CMD: ThreadOwned<OrderCommand> = ThreadOwned::new();

// The engine's L2 query (`get_l2_data(depth)`) returns the top `depth` levels
// per side. The canonical workload's price band is small and centred on the
// mid; this depth comfortably covers every resting level so best-bid/ask and
// per-price depth queries are exact. (The engine exposes no direct
// best_bid/price-keyed-depth getter on the trait.)
const L2_DEPTH: usize = 1 << 20;

// Fixed symbol id used for the single book.
const SYMBOL_ID: SymbolId = 1;

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
fn side_to_u8(a: OrderAction) -> u8 {
    match a {
        OrderAction::Bid => 0,
        OrderAction::Ask => 1,
    }
}

#[inline]
fn action_from_side(side: u8) -> OrderAction {
    if side == 0 {
        OrderAction::Bid
    } else {
        OrderAction::Ask
    }
}

/// Reset the reusable command into a PlaceOrder for a new/reinserted limit
/// order. Sets every field the engine's place path reads and resets the rest to
/// the `Default` values (so a reused command carries no stale state from a prior
/// cancel/place), then `clear()`s the event Vec — keeping its capacity, so no
/// allocation happens here. Mirrors `OrderCommand::default()` field-for-field
/// for the non-place-relevant members.
#[inline]
fn reset_place_cmd(
    cmd: &mut OrderCommand,
    order_id: u64,
    price: i64,
    size: i64,
    action: OrderAction,
    order_type: OrderType,
) {
    cmd.command = OrderCommandType::PlaceOrder;
    cmd.result_code = CommandResultCode::ValidForMatchingEngine;
    cmd.uid = 1;
    cmd.order_id = order_id;
    cmd.symbol = SYMBOL_ID;
    cmd.price = price;
    cmd.reserve_price = 0;
    cmd.size = size;
    cmd.action = action;
    cmd.order_type = order_type;
    cmd.timestamp = 0;
    cmd.events_group = 0;
    cmd.service_flags = 0;
    cmd.stop_price = None;
    cmd.visible_size = None;
    cmd.expire_time = None;
    cmd.matcher_events.clear();
}

/// Reset the reusable command into a CancelOrder (id-keyed) for the resting
/// order. Same single-allocation discipline: `clear()` the event Vec, never
/// reconstruct it.
#[inline]
fn reset_cancel_cmd(cmd: &mut OrderCommand, order_id: u64) {
    cmd.command = OrderCommandType::CancelOrder;
    cmd.result_code = CommandResultCode::New;
    cmd.uid = 1;
    cmd.order_id = order_id;
    cmd.symbol = SYMBOL_ID;
    cmd.price = 0;
    cmd.reserve_price = 0;
    cmd.size = 0;
    cmd.action = OrderAction::Bid;
    cmd.order_type = OrderType::Gtc;
    cmd.timestamp = 0;
    cmd.events_group = 0;
    cmd.service_flags = 0;
    cmd.stop_price = None;
    cmd.visible_size = None;
    cmd.expire_time = None;
    cmd.matcher_events.clear();
}

/// Run a placed command through the engine and convert its Trade events into
/// harness Trade reports. `seq` is the aggressive order's sequence number;
/// `taker_oid` is the aggressive order's id. Returns the total filled size
/// (summed over the engine's Trade events), which the caller uses to derive an
/// IOC residual.
#[inline]
fn emit_trades(cmd: &OrderCommand, seq: u64, taker_oid: u64) -> i64 {
    let mut filled: i64 = 0;
    for ev in &cmd.matcher_events {
        // The engine emits Trade events for fills and Reject events for
        // internal removed-quantity bookkeeping (IOC residual, cancel). Only
        // Trade events are harness fills.
        if ev.event_type == MatcherEventType::Trade {
            filled += ev.size;
            let r = Report {
                r#type: ME_TRADE,
                side: 0,
                _reserved: [0; 6],
                sequence_number: seq,
                order_id: 0,
                price_ticks: ev.price, // maker's resting price
                quantity: ev.size as u32,
                _reserved2: 0,
                maker_order_id: ev.matched_order_id,
                taker_order_id: taker_oid,
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
    unsafe { TRANSPORT.init(Transport { vtable: transport, sink }) };

    // A spot pair with zero fees and unit scale. zero taker/maker fee and a
    // CurrencyExchangePair type keep the bare price-time matcher in play with
    // no fee/scale arithmetic. (move_order's reserve-price guard is irrelevant
    // here — the adapter never calls move_order.)
    let spec = CoreSymbolSpecification {
        symbol_id: SYMBOL_ID,
        symbol_type: SymbolType::CurrencyExchangePair,
        base_currency: 1,
        quote_currency: 2,
        base_scale_k: 1,
        quote_scale_k: 1,
        taker_fee: 0,
        maker_fee: 0,
        margin_buy: 0,
        margin_sell: 0,
    };
    // SAFETY: init phase, single thread, no other live access.
    unsafe { BOOK.init(DirectOrderBook::new(spec)) };

    // Allocate the one reusable command up front (its `matcher_events` Vec is
    // allocated exactly once here, off the timed path). Each hot-path call
    // resets this command's fields and clears the Vec instead of constructing a
    // new one. SAFETY: init phase, single thread, no other live access.
    unsafe { CMD.init(OrderCommand::default()) };
}

#[no_mangle]
pub extern "C" fn engine_shutdown() {
    // The harness loads, runs once, and tears down. Mirrors the C++/Rust
    // reference adapters' no-op shutdown.
}

#[no_mangle]
pub extern "C" fn engine_flush() {
    // Synchronous matcher: new_order / cancel_order run and emit inline before
    // returning. Nothing to drain.
}

// =============================================================================
// Hot path
// =============================================================================

#[no_mangle]
pub unsafe extern "C" fn engine_on_new_order(order: *const NewOrder) {
    let o = unsafe { &*order };

    // OrderAck first so the (pre-sort) report order matches the reference
    // adapters; the harness sorts by (seq, type) anyway.
    emit_ack(
        ME_ORDER_ACK,
        o.sequence_number,
        o.order_id,
        o.side,
        o.price_ticks,
        o.quantity,
    );

    let action = action_from_side(o.side);
    let order_type = if o.ioc != 0 { OrderType::Ioc } else { OrderType::Gtc };

    // SAFETY: single matcher thread (see ThreadOwned). BOOK and CMD are disjoint
    // statics, so these two `&mut` borrows never alias. The reusable command is
    // reset (no allocation) instead of constructed fresh.
    let book = unsafe { BOOK.get_mut() };
    let cmd = unsafe { CMD.get_mut() };
    reset_place_cmd(
        cmd,
        o.order_id,
        o.price_ticks,
        o.quantity as i64,
        action,
        order_type,
    );
    book.new_order(cmd);

    let filled = emit_trades(cmd, o.sequence_number, o.order_id);

    if o.ioc != 0 {
        // Native IOC drops its residual; synthesise the harness CancelAck for
        // the unfilled remainder.
        let residual = (o.quantity as i64) - filled;
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
    // GTC: the engine rested any residual under the harness id in its own id
    // index — a later cancel/modify resolves through get_order_by_id /
    // cancel_order, so there is nothing to record here. A fully filled taker
    // was never placed, which is exactly what makes a later cancel of it a
    // CancelReject.
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_cancel(cancel: *const CancelMsg) {
    let c = unsafe { &*cancel };
    // SAFETY: single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_mut() };

    // Native resting test + side/price echo source: get_order_by_id returns
    // the resting order's (price, action) or None (not resting).
    match book.get_order_by_id(c.order_id) {
        Some((price, action)) => {
            // SAFETY: single matcher thread; CMD disjoint from BOOK. Reset the
            // reusable command (no allocation) instead of constructing fresh.
            let cmd = unsafe { CMD.get_mut() };
            reset_cancel_cmd(cmd, c.order_id);
            let res = book.cancel_order(cmd);
            // get_order_by_id said Some, so the native cancel must succeed.
            debug_assert_eq!(res, CommandResultCode::Success);
            let _ = res;
            emit_ack(
                ME_CANCEL_ACK,
                c.sequence_number,
                c.order_id,
                side_to_u8(action),
                price,
                0, // CancelAck carries no quantity in the canonical form
            );
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

    // Harness modify = cancel + reinsert. The engine's id index is the resting
    // test: get_order_by_id Some -> the order's side seeds the reinsert; None
    // -> ModifyReject.
    match book.get_order_by_id(m.order_id) {
        Some((_old_price, action)) => {
            // The reusable command is used twice here (cancel then reinsert),
            // each reset/`clear()`ed in place — no allocation on either step.
            // SAFETY: single matcher thread; CMD disjoint from BOOK.
            let cmd = unsafe { CMD.get_mut() };

            // 1. Native cancel (removes the resting order, losing time priority).
            reset_cancel_cmd(cmd, m.order_id);
            let _ = book.cancel_order(cmd);

            // 2. Reinsert as a fresh GTC limit on the SAME side at the new
            //    price/qty; any crossing fills emit through the trade path
            //    tagged with the modify's seq + the order's id as taker. The
            //    reset clears the cancel's bookkeeping event + restores `action`.
            reset_place_cmd(
                cmd,
                m.order_id,
                m.new_price_ticks,
                m.new_quantity as i64,
                action,
                OrderType::Gtc,
            );
            book.new_order(cmd);
            let _ = emit_trades(cmd, m.sequence_number, m.order_id);

            // 3. Exactly one ModifyAck (side, new price, new qty).
            emit_ack(
                ME_MODIFY_ACK,
                m.sequence_number,
                m.order_id,
                side_to_u8(action),
                m.new_price_ticks,
                m.new_quantity,
            );
        }
        None => {
            emit_ack(ME_MODIFY_REJECT, m.sequence_number, m.order_id, 0, 0, 0);
        }
    }
}

// =============================================================================
// Audit queries
//
// The engine's `OrderBook` trait exposes book state through `get_l2_data`
// only; we read the top L2_DEPTH levels (a band wide enough to cover the
// workload) and answer best-bid/ask and per-price depth from it. All three
// run on the single matcher thread.
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_query_best_bid() -> i64 {
    // SAFETY: queries run on the same single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_ref() };
    let l2 = book.get_l2_data(L2_DEPTH);
    // bid_prices come back highest-first (the trait iterates bids in
    // descending price), so the first entry is the best bid.
    match l2.bid_prices.first() {
        Some(p) => *p,
        None => i64::MIN,
    }
}

#[no_mangle]
pub extern "C" fn engine_query_best_ask() -> i64 {
    // SAFETY: queries run on the same single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_ref() };
    let l2 = book.get_l2_data(L2_DEPTH);
    // ask_prices come back lowest-first.
    match l2.ask_prices.first() {
        Some(p) => *p,
        None => i64::MAX,
    }
}

#[no_mangle]
pub extern "C" fn engine_query_depth_at(price_ticks: i64, side: c_uchar) -> u64 {
    // SAFETY: queries run on the same single matcher thread (see ThreadOwned).
    let book = unsafe { BOOK.get_ref() };
    let l2 = book.get_l2_data(L2_DEPTH);
    let (prices, volumes) = if side == 0 {
        (&l2.bid_prices, &l2.bid_volumes)
    } else {
        (&l2.ask_prices, &l2.ask_volumes)
    };
    for (i, p) in prices.iter().enumerate() {
        if *p == price_ticks {
            let v = volumes[i];
            return if v > 0 { v as u64 } else { 0 };
        }
    }
    0
}
