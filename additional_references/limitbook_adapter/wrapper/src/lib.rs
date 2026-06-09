//! limitbook_adapter — solarpx/limitbook behind the harness
//! matching_engine_api.h ABI.
//!
//! The engine is pure Rust, so this whole adapter is one Rust cdylib that
//! exports the harness `engine_*` extern-C symbols directly. No C++ shim.
//! Mirrors the structure of `orderbookrs_adapter`.
//!
//! What the engine provides natively (used here):
//!   - `OrderBook::new(tick_size: Decimal)` — single mutable book, NOT Sync
//!     (BTreeMap/HashMap + `&mut self` methods), so the adapter holds it
//!     behind a Mutex driven only by the single matcher thread.
//!   - `add_limit_order(side, price, qty) -> Result<(OrderId, Vec<Fill>)>` —
//!     SYNCHRONOUS. Matches the incoming order against the book and rests any
//!     residual. Returns the fills BY VALUE — so trades are emitted directly
//!     from the return value; no trade callback / thread-local needed (unlike
//!     orderbookrs). `Fill { quantity, price, taker_order_id, maker_order_id }`
//!     exposes both ids.
//!   - `cancel_limit_order(id) -> Result<()>` — Ok if removed, Err if not.
//!   - `best_bid()` / `best_ask()` — `Option<Decimal>`.
//!   - `execute_market_order(...)` — NOT used; the harness workload is limit
//!     orders + IOC, and the engine's market path errors on insufficient
//!     liquidity, so IOC is composed as add-limit + cancel-residual instead.
//!
//! THE KEY DIFFERENCE FROM orderbookrs: limitbook's `add_limit_order` IGNORES
//! any caller-supplied id and assigns its OWN sequential `OrderId` (starting
//! at 0, incremented on every add). The returned `Fill`s reference those
//! ENGINE ids for both maker and taker. The harness, however, has its own
//! order ids (`new_order_t.order_id`) and hashes Trade reports as
//! `1,seq,price,qty,maker_order_id,taker_order_id` — so every report must
//! carry the HARNESS ids. The adapter therefore keeps a bidirectional map
//! engine_id <-> harness_id and translates every fill's maker/taker back to
//! harness space.
//!
//! Synthesised above the engine (engine emits none of these in the harness's
//! wire format):
//!   - OrderAck, CancelAck (incl. IOC-residual), ModifyAck,
//!     CancelReject, ModifyReject.
//!
//! Price mapping: the harness speaks INTEGER ticks (`price_ticks: i64`); the
//! engine speaks `Decimal` on a tick grid. We construct the book with
//! tick_size = 1 and pass `Decimal::from(price_ticks)` straight through, so
//! the engine's price grid IS the harness's integer-tick grid: the round-trip
//! is exact (no fractional Decimal, no rounding), and `best_bid/ask/depth`
//! come back as the same integers the harness compares against its baseline.
//!
//! Modify contract: the harness defines modify as cancel + reinsert (queue
//! priority lost). limitbook has no native modify, so the adapter cancels
//! through the engine, emits ModifyAck at the new price/qty, then re-adds as a
//! new limit so any crossing fills are emitted, exactly like the C++ /
//! orderbookrs reference adapters.

use limitbook::{OrderBook, OrderSide};
use rust_decimal::prelude::ToPrimitive;
use rust_decimal::Decimal;
use std::collections::HashMap;
use std::os::raw::{c_uchar, c_uint, c_void};
use std::sync::{Mutex, OnceLock};

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
// SAFETY: the harness owns both pointers; once engine_init returns they are
// stable until engine_shutdown, and only the matcher thread touches them.
unsafe impl Send for Transport {}
unsafe impl Sync for Transport {}

static TRANSPORT: OnceLock<Transport> = OnceLock::new();

// Per-order state, keyed by the HARNESS order id. `engine_id` is the id the
// engine assigned (used to cancel and to translate fills back). `remaining`
// drives the incremental depth map and the CancelAck/echo quantity. `alive`
// drives the cancel/modify reject path.
#[derive(Copy, Clone)]
struct OrderState {
    engine_id: u64,
    price: i64,
    side: u8,
    remaining: u32,
    alive: bool,
}

// Whole-adapter state behind one Mutex (single matcher thread -> uncontended;
// the Mutex exists only because limitbook's OrderBook is `&mut`-method-only
// and therefore not Sync). Bundling everything under one lock keeps the
// engine, the id maps and the depth index mutually consistent with no
// lock-ordering questions.
struct State {
    book: OrderBook,
    // harness_oid -> per-order state.
    by_harness: HashMap<u64, OrderState>,
    // engine_oid -> harness_oid, for translating fills back to harness ids.
    eng_to_harness: HashMap<u64, u64>,
    // Aggregated resting quantity, keyed by (price_ticks, side). Maintained
    // incrementally so engine_query_depth_at is O(1). side: 0 = buy, 1 = sell.
    depth: HashMap<(i64, u8), u64>,
}
// SAFETY: only ever reached through the Mutex below, from the matcher thread.
unsafe impl Send for State {}

static STATE: OnceLock<Mutex<State>> = OnceLock::new();

#[inline]
fn side_of(s: u8) -> OrderSide {
    if s == 0 {
        OrderSide::Buy
    } else {
        OrderSide::Sell
    }
}

// Harness integer tick -> engine Decimal price. tick_size is 1, so the price
// IS the integer tick; conversion is exact and lossless.
#[inline]
fn ticks_to_price(t: i64) -> Decimal {
    Decimal::from(t)
}

// Engine Decimal price -> harness integer tick. With tick_size 1 every level
// is an integer, so to_i64() round-trips exactly.
#[inline]
fn price_to_ticks(p: Decimal) -> i64 {
    p.to_i64().unwrap_or(0)
}

#[inline]
fn emit(r: &Report) {
    let t = TRANSPORT.get().expect("transport not initialised");
    unsafe {
        // Spin until accepted. Matches the other reference adapters' pattern.
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

// Apply the fills returned by an add_limit_order call:
//   * emit one Trade per fill (price = maker's resting price, seq = aggressor),
//     translating maker/taker engine ids back to harness ids;
//   * decrement the maker's resting `remaining` + the aggregate depth index,
//     pruning the maker when it is fully consumed.
// `taker_harness` / `taker_side` identify the aggressor (already known by the
// caller, so the taker translation never needs the map). Returns the total
// quantity filled, so the caller can compute the resting residual.
fn apply_fills(
    st: &mut State,
    fills: &[limitbook::Fill],
    seq: u64,
    taker_harness: u64,
    taker_side: u8,
) -> u32 {
    let maker_side = taker_side ^ 1; // maker is the opposite side of the taker
    let mut filled: u32 = 0;
    for f in fills {
        let qty = f.quantity.to_u32().unwrap_or(0);
        let price = price_to_ticks(f.price);
        let maker_harness = st
            .eng_to_harness
            .get(&f.maker_order_id)
            .copied()
            .unwrap_or(f.maker_order_id);

        let r = Report {
            r#type: ME_TRADE,
            side: 0,
            _reserved: [0; 6],
            sequence_number: seq,
            order_id: 0,
            price_ticks: price,
            quantity: qty,
            _reserved2: 0,
            maker_order_id: maker_harness,
            taker_order_id: taker_harness,
            _reserved3: 0,
        };
        emit(&r);

        filled = filled.saturating_add(qty);

        // The maker's resting quantity at its price level drops by this fill.
        if let Some(e) = st.depth.get_mut(&(price, maker_side)) {
            *e = e.saturating_sub(qty as u64);
        }
        // Maintain the maker's per-order shadow; prune when fully filled.
        if let Some(s) = st.by_harness.get_mut(&maker_harness) {
            if s.remaining > qty {
                s.remaining -= qty;
            } else {
                s.remaining = 0;
                s.alive = false;
                let eng = s.engine_id;
                st.by_harness.remove(&maker_harness);
                st.eng_to_harness.remove(&eng);
            }
        }
    }
    filled
}

// =============================================================================
// engine_init / engine_shutdown
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_init(_seed: u64, transport: *const MeTransport, sink: *mut c_void) {
    TRANSPORT.set(Transport { vtable: transport, sink }).ok();

    // tick_size = 1: the engine's Decimal price grid coincides with the
    // harness's integer-tick grid (see ticks_to_price / price_to_ticks).
    let book = OrderBook::new(Decimal::ONE).expect("tick size must be positive");
    STATE
        .set(Mutex::new(State {
            book,
            by_harness: HashMap::with_capacity(1 << 21),
            eng_to_harness: HashMap::with_capacity(1 << 21),
            depth: HashMap::with_capacity(1 << 16),
        }))
        .ok();
}

#[no_mangle]
pub extern "C" fn engine_shutdown() {
    // Load-once / run-once / dlclose process model: nothing to free explicitly
    // (OnceLocks live for the process). Mirrors the other reference adapters.
}

#[no_mangle]
pub extern "C" fn engine_flush() {
    // Fully synchronous matcher: every add/cancel runs and emits inline before
    // returning. Nothing to drain.
}

// =============================================================================
// Hot path
// =============================================================================

#[no_mangle]
pub unsafe extern "C" fn engine_on_new_order(order: *const NewOrder) {
    let o = unsafe { &*order };

    // OrderAck first so the canonical report order (Ack, then Trades, then an
    // optional IOC-residual CancelAck) matches the reference adapters.
    emit_ack(
        ME_ORDER_ACK,
        o.sequence_number,
        o.order_id,
        o.side,
        o.price_ticks,
        o.quantity,
    );

    let mut st = STATE.get().unwrap().lock().unwrap();

    let (engine_id, fills) = st
        .book
        .add_limit_order(side_of(o.side), ticks_to_price(o.price_ticks), Decimal::from(o.quantity))
        .expect("add_limit_order rejected a workload order");

    // Record the taker's engine->harness mapping BEFORE applying fills (a fill
    // can reference the taker as a maker only in pathological self-cross cases,
    // which this workload does not produce — but keeping the map complete is
    // harmless and cheap).
    st.eng_to_harness.insert(engine_id, o.order_id);

    let filled = apply_fills(&mut st, &fills, o.sequence_number, o.order_id, o.side);
    let residual = o.quantity.saturating_sub(filled);

    if o.ioc != 0 {
        // IOC: the engine rested any residual (no native IOC). Cancel it and
        // emit one CancelAck for the unfilled remainder. The map entry for the
        // taker is dropped since it never rests.
        st.eng_to_harness.remove(&engine_id);
        if residual > 0 {
            let _ = st.book.cancel_limit_order(engine_id);
            emit_ack(
                ME_CANCEL_ACK,
                o.sequence_number,
                o.order_id,
                o.side,
                o.price_ticks,
                residual,
            );
        }
        return;
    }

    if residual > 0 {
        // GTC residual rests: record the shadow + bump the depth index.
        st.by_harness.insert(
            o.order_id,
            OrderState {
                engine_id,
                price: o.price_ticks,
                side: o.side,
                remaining: residual,
                alive: true,
            },
        );
        *st.depth.entry((o.price_ticks, o.side)).or_insert(0) += residual as u64;
    } else {
        // Fully filled on arrival -> never rested; drop the taker mapping.
        st.eng_to_harness.remove(&engine_id);
    }
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_cancel(cancel: *const CancelMsg) {
    let c = unsafe { &*cancel };
    let mut st = STATE.get().unwrap().lock().unwrap();

    let info = st.by_harness.get(&c.order_id).copied();
    match info {
        Some(s) if s.alive => {
            match st.book.cancel_limit_order(s.engine_id) {
                Ok(()) => {
                    emit_ack(
                        ME_CANCEL_ACK,
                        c.sequence_number,
                        c.order_id,
                        s.side,
                        s.price,
                        s.remaining,
                    );
                    if let Some(e) = st.depth.get_mut(&(s.price, s.side)) {
                        *e = e.saturating_sub(s.remaining as u64);
                    }
                    st.by_harness.remove(&c.order_id);
                    st.eng_to_harness.remove(&s.engine_id);
                }
                Err(_) => {
                    // Engine disagrees with the shadow (filled away in between).
                    emit_ack(ME_CANCEL_REJECT, c.sequence_number, c.order_id, 0, 0, 0);
                    st.by_harness.remove(&c.order_id);
                    st.eng_to_harness.remove(&s.engine_id);
                }
            }
        }
        _ => {
            emit_ack(ME_CANCEL_REJECT, c.sequence_number, c.order_id, 0, 0, 0);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_modify(modify: *const ModifyMsg) {
    let m = unsafe { &*modify };
    let mut st = STATE.get().unwrap().lock().unwrap();

    let info = st.by_harness.get(&m.order_id).copied();
    let s = match info {
        Some(s) if s.alive => s,
        _ => {
            emit_ack(ME_MODIFY_REJECT, m.sequence_number, m.order_id, 0, 0, 0);
            return;
        }
    };

    // Harness modify == cancel + reinsert. Cancel the existing engine order.
    if st.book.cancel_limit_order(s.engine_id).is_err() {
        // Filled away between the shadow update and now.
        emit_ack(ME_MODIFY_REJECT, m.sequence_number, m.order_id, 0, 0, 0);
        st.by_harness.remove(&m.order_id);
        st.eng_to_harness.remove(&s.engine_id);
        return;
    }
    // Drop the old resting quantity from the depth index + the old maps.
    if let Some(e) = st.depth.get_mut(&(s.price, s.side)) {
        *e = e.saturating_sub(s.remaining as u64);
    }
    st.by_harness.remove(&m.order_id);
    st.eng_to_harness.remove(&s.engine_id);

    // Exactly one ModifyAck at the new price/qty, then re-add on the same side
    // so any crossing fills emit tagged with the modify's seq.
    emit_ack(
        ME_MODIFY_ACK,
        m.sequence_number,
        m.order_id,
        s.side,
        m.new_price_ticks,
        m.new_quantity,
    );

    let (engine_id, fills) = st
        .book
        .add_limit_order(side_of(s.side), ticks_to_price(m.new_price_ticks), Decimal::from(m.new_quantity))
        .expect("modify reinsert rejected");
    st.eng_to_harness.insert(engine_id, m.order_id);

    let filled = apply_fills(&mut st, &fills, m.sequence_number, m.order_id, s.side);
    let residual = m.new_quantity.saturating_sub(filled);

    if residual > 0 {
        st.by_harness.insert(
            m.order_id,
            OrderState {
                engine_id,
                price: m.new_price_ticks,
                side: s.side,
                remaining: residual,
                alive: true,
            },
        );
        *st.depth.entry((m.new_price_ticks, s.side)).or_insert(0) += residual as u64;
    } else {
        st.eng_to_harness.remove(&engine_id);
    }
}

// =============================================================================
// Audit queries
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_query_best_bid() -> i64 {
    let st = STATE.get().unwrap().lock().unwrap();
    match st.book.best_bid() {
        Some(p) => price_to_ticks(p),
        None => i64::MIN,
    }
}

#[no_mangle]
pub extern "C" fn engine_query_best_ask() -> i64 {
    let st = STATE.get().unwrap().lock().unwrap();
    match st.book.best_ask() {
        Some(p) => price_to_ticks(p),
        None => i64::MAX,
    }
}

#[no_mangle]
pub extern "C" fn engine_query_depth_at(price_ticks: i64, side: c_uchar) -> u64 {
    let st = STATE.get().unwrap().lock().unwrap();
    st.depth.get(&(price_ticks, side)).copied().unwrap_or(0)
}
