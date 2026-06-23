//! pgellert_adapter — pgellert/matching-engine behind the harness
//! `api/matching_engine_api.h` ABI.
//!
//! The engine is pure Rust. Its production matcher is
//! `engine::algos::optimised_fifo::FIFOBook` — the book
//! `engine/src/rpc/me_state_machine.rs` constructs. This adapter vendors that
//! book and its `book` module verbatim (only the three protobuf/raft bridge
//! methods on `Order` are stripped, so the tonic/prost/raft server stack does
//! not have to build) and drives it exactly as the engine's state machine does:
//! for each message, `book.apply(order)` then `book.check_for_trades()`
//! (me_state_machine.rs:97-99). No matching logic lives in this adapter.
//!
//! Engine shape (and why each shim exists):
//!   * The `Book` trait is **symmetric / batch-style**, not aggressor-driven:
//!     `apply` only rests the order; `check_for_trades` repeatedly pops the best
//!     bid + best ask and merges them. Because the harness delivers one message
//!     at a time and this adapter (like the engine) drains every cross after
//!     each `apply`, the book is non-crossing between messages, so the only
//!     order that can cross in a given `check_for_trades` is the one just
//!     applied — that is the aggressor (taker); every order it trades against is
//!     resting (a maker). The adapter stamps each `Trade { ask, bid }` from the
//!     known aggressor side: aggressor Buy => taker is `bid`, maker is `ask`,
//!     fill price = ask.price; aggressor Sell => taker is `ask`, maker is `bid`,
//!     fill price = bid.price. (The engine's own state machine instead reports
//!     `trade.bid.price` for *every* fill — me_state_machine.rs:103 — which is
//!     not the maker's resting price for a Sell aggressor; the adapter emits the
//!     correct maker price, since the harness wire format demands it.)
//!   * No native IOC. Harness IOC is submitted as a normal limit, matched, and
//!     any resting residual is then cancelled and dropped (one CancelAck) —
//!     exactly the residual-cancel pattern the harness template prescribes.
//!   * `cancel(id, side)` returns only `bool` and *requires* the side as input,
//!     while the harness `cancel_t` carries no side and the CancelAck/ModifyAck
//!     wire lines echo the resting order's side + price. So the adapter keeps a
//!     minimal per-order liveness shadow (side, price, resting qty, alive) — the
//!     same shadow the liquibook / quantcup reference adapters keep for the same
//!     reason — to drive the side argument, the ack field echo, and the
//!     CancelReject / ModifyReject decision.
//!
//! Order ids: the engine keys an order by `(client_id, seq_number)`. The harness
//! identifies an order by a single `order_id`, so the adapter maps it to the
//! engine key `(0, order_id)`. The harness `sequence_number` is a *separate*
//! field used only to stamp reports (the aggressor's seq); it is carried
//! out-of-band and never used as the engine key.

use std::cell::UnsafeCell;
use std::collections::HashMap;
use std::os::raw::{c_uchar, c_uint, c_void};

mod algos;
use algos::book::{Book, Order, Side};
use algos::optimised_fifo::FIFOBook;

/// Re-exports for the standalone bug reproduction (examples/repro.rs). Not part
/// of the cdylib ABI; pure convenience so the repro drives the real vendored
/// engine book the same way the adapter does.
pub mod repro_support {
    pub use crate::algos::book::{Book, Order, Side};
    pub use crate::algos::optimised_fifo::FIFOBook;

    /// Build an order with client_id 0 and the given seq as its id (matches how
    /// the adapter keys orders).
    pub fn ord(seq: u64, side: Side, price: u64, size: u64) -> Order {
        Order {
            client_id: 0,
            seq_number: seq,
            price,
            size,
            side,
        }
    }
}

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
// Adapter state — single matcher thread owns it (the harness drives every
// engine_on_* / query from one thread; the drainer touches only the transport).
// =============================================================================

struct Transport {
    vtable: *const MeTransport,
    sink: *mut c_void,
}

/// Minimal per-order liveness shadow. Needed because the engine's
/// `cancel(id, side)` requires the side as input and returns only `bool`, while
/// the harness CancelAck/ModifyAck lines echo the resting order's side + price.
#[derive(Copy, Clone)]
struct OrderShadow {
    price: i64,
    qty: u32,
    side: u8, // 0 = buy, 1 = sell
    alive: bool,
}

struct State {
    book: FIFOBook,
    shadow: HashMap<u64, OrderShadow>,
}

// Single-thread-owned global with interior mutability (same idiom as the
// orderbookrs reference adapter). The `Sync` bound only satisfies the `static`
// requirement; every access is from the single matcher thread.
struct ThreadOwned<T>(UnsafeCell<Option<T>>);
unsafe impl<T> Sync for ThreadOwned<T> {}
impl<T> ThreadOwned<T> {
    const fn new() -> Self {
        Self(UnsafeCell::new(None))
    }
    #[inline(always)]
    unsafe fn init(&self, value: T) {
        *self.0.get() = Some(value);
    }
    #[inline(always)]
    unsafe fn get(&self) -> &mut T {
        (*self.0.get()).as_mut().unwrap_unchecked()
    }
}

static TRANSPORT: ThreadOwned<Transport> = ThreadOwned::new();
static STATE: ThreadOwned<State> = ThreadOwned::new();

#[inline]
fn emit(r: &Report) {
    // SAFETY: matcher thread only; TRANSPORT.init ran in engine_init.
    let t = unsafe { TRANSPORT.get() };
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

/// The engine OrderId for a harness order_id.
#[inline(always)]
fn key(order_id: u64) -> (u64, u64) {
    (0, order_id)
}

/// Apply one aggressor order to the book, drain every cross, and emit a Trade
/// per fill with the correct maker price / maker id / taker id. Returns the
/// aggressor's unfilled residual quantity (what is left resting in the book
/// under `agg_oid`, or what an IOC must drop).
///
/// `agg_side`: 0 = buy, 1 = sell. Mirrors me_state_machine.rs:97-99 (apply then
/// check_for_trades), but stamps each Trade from the harness wire format
/// (maker's resting price + maker/taker ids) instead of the engine's
/// trade.bid.price-for-both convention.
fn match_aggressor(agg_seq: u64, agg_side: u8, order: Order) -> u32 {
    // SAFETY: single matcher thread.
    let st = unsafe { STATE.get() };

    let agg_total = order.size;
    st.book.apply(order);
    let trades = st.book.check_for_trades();

    let mut agg_filled: u64 = 0;
    for trade in &trades {
        // The aggressor is the just-applied order; the maker is the order on
        // the opposite side. Resolve maker vs taker from the known aggressor
        // side (the symmetric Trade carries both ask and bid).
        let (maker_price, maker_id, taker_id) = if agg_side == 0 {
            // Aggressor is a buy => it is the `bid`; maker is the `ask`.
            (
                trade.ask.price as i64,
                pack_id(trade.ask.client_id, trade.ask.seq_number),
                pack_id(trade.bid.client_id, trade.bid.seq_number),
            )
        } else {
            // Aggressor is a sell => it is the `ask`; maker is the `bid`.
            (
                trade.bid.price as i64,
                pack_id(trade.bid.client_id, trade.bid.seq_number),
                pack_id(trade.ask.client_id, trade.ask.seq_number),
            )
        };

        agg_filled += trade.quantity;

        // Maintain the maker's shadow (decrement; mark dead when fully filled).
        if let Some(sh) = st.shadow.get_mut(&maker_id) {
            if u64::from(sh.qty) > trade.quantity {
                sh.qty -= trade.quantity as u32;
            } else {
                sh.qty = 0;
                sh.alive = false;
            }
        }

        let r = Report {
            r#type: ME_TRADE,
            side: 0,
            _reserved: [0; 6],
            sequence_number: agg_seq,
            order_id: 0,
            price_ticks: maker_price,
            quantity: trade.quantity as u32,
            _reserved2: 0,
            maker_order_id: maker_id,
            taker_order_id: taker_id,
            _reserved3: 0,
        };
        emit(&r);
    }

    (agg_total.saturating_sub(agg_filled)) as u32
}

/// Recover the harness order_id from an engine (client_id, seq_number) key.
/// This adapter always inserts with client_id = 0 and seq_number = order_id, so
/// the recovered id is the seq_number.
#[inline(always)]
fn pack_id(client_id: u64, seq_number: u64) -> u64 {
    debug_assert_eq!(client_id, 0);
    seq_number
}

// =============================================================================
// engine_init / engine_shutdown
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_init(_seed: u64, transport: *const MeTransport, sink: *mut c_void) {
    // SAFETY: init phase — no messages delivered yet, single thread.
    unsafe { TRANSPORT.init(Transport { vtable: transport, sink }) };
    let mut shadow: HashMap<u64, OrderShadow> = HashMap::new();
    shadow.reserve(1usize << 21); // ~2M-message canonical workload
    unsafe {
        STATE.init(State {
            book: FIFOBook::new(),
            shadow,
        })
    };
}

#[no_mangle]
pub extern "C" fn engine_shutdown() {
    // Load-run-teardown: the cells live for the process lifetime. Nothing to do
    // (mirrors the reference adapters).
}

#[no_mangle]
pub extern "C" fn engine_flush() {
    // Fully synchronous matcher: every engine_on_* applies, matches, and emits
    // inline before returning. Nothing to drain.
}

// =============================================================================
// Hot path
// =============================================================================

#[no_mangle]
pub unsafe extern "C" fn engine_on_new_order(order: *const NewOrder) {
    let o = unsafe { &*order };

    // OrderAck first (engine has accepted the new order), matching the
    // canonical report order Ack -> Trades -> optional residual CancelAck.
    emit_ack(
        ME_ORDER_ACK,
        o.sequence_number,
        o.order_id,
        o.side,
        o.price_ticks,
        o.quantity,
    );

    let side = if o.side == 0 { Side::Buy } else { Side::Sell };
    let (cid, seq) = key(o.order_id);
    let book_order = Order {
        client_id: cid,
        seq_number: seq,
        price: o.price_ticks as u64, // harness ticks are non-negative
        size: o.quantity as u64,
        side,
    };

    let residual = match_aggressor(o.sequence_number, o.side, book_order);

    if o.ioc != 0 {
        // No native IOC: any residual rested in the book — cancel + drop it,
        // emitting exactly one CancelAck for the unfilled remainder.
        if residual > 0 {
            // SAFETY: single matcher thread.
            let st = unsafe { STATE.get() };
            st.book.cancel(key(o.order_id), side);
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

    // GTC: a non-zero residual rests. Record its shadow so a later
    // cancel/modify can find its side + price and decide reject vs ack.
    if residual > 0 {
        // SAFETY: single matcher thread.
        let st = unsafe { STATE.get() };
        st.shadow.insert(
            o.order_id,
            OrderShadow {
                price: o.price_ticks,
                qty: residual,
                side: o.side,
                alive: true,
            },
        );
    }
    // A fully filled aggressor was never rested; a later cancel of it correctly
    // rejects (no live shadow entry).
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_cancel(cancel: *const CancelMsg) {
    let c = unsafe { &*cancel };
    // SAFETY: single matcher thread.
    let st = unsafe { STATE.get() };

    match st.shadow.get(&c.order_id) {
        Some(sh) if sh.alive => {
            let side = if sh.side == 0 { Side::Buy } else { Side::Sell };
            let price = sh.price;
            let echo_side = sh.side;
            // Remove from the engine book; the shadow said it is resting.
            st.book.cancel(key(c.order_id), side);
            if let Some(sh) = st.shadow.get_mut(&c.order_id) {
                sh.alive = false;
                sh.qty = 0;
            }
            emit_ack(ME_CANCEL_ACK, c.sequence_number, c.order_id, echo_side, price, 0);
        }
        _ => {
            // Not resting — already filled, already cancelled, or never seen.
            emit_ack(ME_CANCEL_REJECT, c.sequence_number, c.order_id, 0, 0, 0);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_modify(modify: *const ModifyMsg) {
    let m = unsafe { &*modify };
    // SAFETY: single matcher thread.
    let st = unsafe { STATE.get() };

    let resting = matches!(st.shadow.get(&m.order_id), Some(sh) if sh.alive);
    if !resting {
        // Not resting — the canonical workload injects stale modifies.
        emit_ack(ME_MODIFY_REJECT, m.sequence_number, m.order_id, 0, 0, 0);
        return;
    }

    // Modify = cancel + reinsert (losing time priority). Cancel the old order
    // on its recorded side, then reinsert at the new price/quantity on the SAME
    // side; the reinsert may cross and produce Trades.
    let old_side_u8 = st.shadow.get(&m.order_id).unwrap().side;
    let old_side = if old_side_u8 == 0 { Side::Buy } else { Side::Sell };
    st.book.cancel(key(m.order_id), old_side);
    if let Some(sh) = st.shadow.get_mut(&m.order_id) {
        sh.alive = false;
        sh.qty = 0;
    }

    // Exactly one ModifyAck. The harness stable-sorts reports by
    // (sequence_number, type) before hashing, so the within-message emission
    // order relative to the reinsert's Trades does not affect the hash.
    emit_ack(
        ME_MODIFY_ACK,
        m.sequence_number,
        m.order_id,
        old_side_u8,
        m.new_price_ticks,
        m.new_quantity,
    );

    let book_order = Order {
        client_id: 0,
        seq_number: m.order_id,
        price: m.new_price_ticks as u64,
        size: m.new_quantity as u64,
        side: old_side,
    };
    let residual = match_aggressor(m.sequence_number, old_side_u8, book_order);

    if residual > 0 {
        // SAFETY: single matcher thread.
        let st = unsafe { STATE.get() };
        st.shadow.insert(
            m.order_id,
            OrderShadow {
                price: m.new_price_ticks,
                qty: residual,
                side: old_side_u8,
                alive: true,
            },
        );
    }
}

// =============================================================================
// Audit queries — must reflect the live engine book.
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_query_best_bid() -> i64 {
    // SAFETY: same single matcher thread.
    let st = unsafe { STATE.get() };
    match st.book.best_bid() {
        Some(p) => p as i64,
        None => i64::MIN,
    }
}

#[no_mangle]
pub extern "C" fn engine_query_best_ask() -> i64 {
    // SAFETY: same single matcher thread.
    let st = unsafe { STATE.get() };
    match st.book.best_ask() {
        Some(p) => p as i64,
        None => i64::MAX,
    }
}

#[no_mangle]
pub extern "C" fn engine_query_depth_at(price_ticks: i64, side: c_uchar) -> u64 {
    if price_ticks < 0 {
        return 0;
    }
    // SAFETY: same single matcher thread.
    let st = unsafe { STATE.get() };
    let s = if side == 0 { Side::Buy } else { Side::Sell };
    st.book.depth_at(price_ticks as u64, s)
}
