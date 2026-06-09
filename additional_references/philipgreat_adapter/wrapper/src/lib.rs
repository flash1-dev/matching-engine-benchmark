//! philipgreat_adapter — philipgreat/lighting-match-engine-core behind the
//! harness `api/matching_engine_api.h` ABI.
//!
//! The engine is pure Rust, so this whole adapter is one Rust cdylib that
//! exports the harness `engine_*` extern-C symbols directly. No C++ shim.
//!
//! ## Which order book
//!
//! The engine ships two books behind a common trait:
//!   - `DenseOrderBook` — the advertised "8 ns / order" path: a flat
//!     `Vec<OrdersBucket>` indexed by `(price - base_price)/tick`, so every
//!     price level must fall inside a fixed `[base_price, base_price +
//!     tick*(levels-1)]` window AND be on-tick, or `match_order` returns
//!     `Err(PriceOutOfRange|PriceNotOnTick)` and the order is dropped.
//!   - `SparseOrderBook` — a `BTreeMap<price, OrdersBucket>` per side, with
//!     *no* range/tick validation (`seed_order`/`match_order` skip it).
//!
//! The harness `normal` workload prices span the **entire** `[0, ~4.34e11]`
//! tick range (GBM mid drift; median ~3.3e5, p90 ~2.2e11 ticks). A dense
//! `Vec` of 4.34e11 buckets is many terabytes — infeasible. So this adapter
//! drives the **sparse** book, the only one of the two that can represent the
//! workload at all. (The advertised throughput number is the *dense* book on
//! a single-price micro-benchmark, not this order flow, so a collapse versus
//! the headline is expected and is the point of the measurement.)
//!
//! ## What the engine provides natively (used here)
//!   - `SparseOrderBook::new(tick, base_price, max_levels, trade_cap)`.
//!   - `match_order(OrderRequest) -> Result<(), OrderSubmitError>` — matches
//!     the incoming order against the book (front-of-`VecDeque` = time
//!     priority), rests any limit residual, and leaves the resulting fills in
//!     `self.last_outcome.trades` (a `Vec<Trade>`, cleared each call). Sparse
//!     never returns `Err`. We read the trade vec right after the call.
//!   - `cancel_order(id) -> bool` — `true` if the order was resting & removed.
//!   - `Trade { buy_order_id, sell_order_id, price (= maker's resting price),
//!     quantity }` — the resting order is always the maker; the taker is the
//!     incoming order, so we recover maker/taker ids from the incoming side.
//!   - `bids` / `asks` (`pub BTreeMap<u64, OrdersBucket>`) for best-bid/ask
//!     and exact-price depth queries.
//!
//! ## Synthesised above the engine (the engine emits none of these in the
//! harness wire format)
//!   - OrderAck, CancelAck (incl. IOC-residual), ModifyAck, CancelReject,
//!     ModifyReject. A shadow map `oid -> {side, price, remaining}` drives the
//!     reject paths and the side/price echo on acks, mirroring the orderbookrs
//!     Rust adapter and the C++ adapters.
//!
//! ## Threading
//! The harness drives every `engine_*` entry point — the `on_*` hot path AND
//! the `query_*` audit probes — from a single thread (verified in harness.cpp:
//! both are called from one replay loop; the only other thread, the drainer,
//! touches just the report transport). So the book / shadow / transport globals
//! are single-owner and need NO synchronisation — there is no lock and no
//! atomic on the matching hot path, matching the C++ reference adapters' plain
//! globals. See `ThreadOwned` below.
//!
//! Order ids and prices round-trip the harness `u64`/`i64` directly: harness
//! prices are non-negative ticks (the `normal` workload min is 0), so the
//! `i64 -> u64` cast for the engine's `u64` price field is lossless.

use lighting_match_engine_core::types::{
    OrderFlags, OrderRequest, OrderSide, PriceType, SparseOrderBook,
};
use std::cell::UnsafeCell;
use std::collections::HashMap;
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

// Compile-time guarantee the ABI mirrors are the right size.
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

// A lazily-initialised global owned exclusively by the single matcher thread
// (see the module "Threading" note). No lock and no atomic: just interior
// mutability via `UnsafeCell`, with the single-thread-ownership invariant
// asserted by a hand-written `Sync` impl (a `static` must be `Sync`). This is
// the Rust expression of the C++ reference adapters' plain `g_book` / `g_shadow`
// / `g_transport` globals — nothing on the hot path but a memory access.
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
    /// Mutable borrow of the value (matcher hot path).
    /// SAFETY: single matcher thread, no reentrancy, `init` ran first — so this
    /// is the only live borrow. Each entry point calls this once.
    #[inline(always)]
    #[allow(clippy::mut_from_ref)]
    unsafe fn get(&self) -> &mut T {
        (*self.0.get()).as_mut().unwrap_unchecked()
    }
    /// Shared borrow of the value (audit-query read path).
    /// SAFETY: as `get`, but shared.
    #[inline(always)]
    unsafe fn get_ref(&self) -> &T {
        (*self.0.get()).as_ref().unwrap_unchecked()
    }
}

// Transport vtable + sink, captured in engine_init. Raw pointers owned by the
// harness; stable from engine_init until engine_shutdown.
struct Transport {
    vtable: *const MeTransport,
    sink: *mut c_void,
}
static TRANSPORT: ThreadOwned<Transport> = ThreadOwned::new();

// The fixed product id the harness's single book maps to (main.rs uses 7).
const PRODUCT_ID: u16 = 7;

// Shadow map: per resting order {side, price, remaining}. Drives the reject
// path and the CancelAck/ModifyAck side/price echo. Maintained from the trades
// produced by each match (partial fills decrement the maker, full fills remove
// the entry), exactly like the orderbookrs / C++ reference adapters. An absent
// id reads as not-resting on the reject paths, so terminal orders are dropped
// rather than tombstoned — this bounds the map to the live resting set (no
// unbounded growth past the reserve -> no rehash on multi-million-msg runs).
struct Shadow {
    price: i64,
    side: u8,
    remaining: u32,
}

// Book + shadow map, owned by the matcher thread (no lock — see ThreadOwned).
// `SparseOrderBook::match_order` takes `&mut self`; the shadow map is mutated
// in the same logical step, so they sit together and are split-borrowed where
// the matcher needs both at once.
struct EngineState {
    book: SparseOrderBook,
    shadow: HashMap<u64, Shadow>,
}
static ENGINE: ThreadOwned<EngineState> = ThreadOwned::new();

#[inline]
fn emit(r: &Report) {
    // SAFETY: TRANSPORT is initialised in engine_init before any emit, and only
    // the single matcher thread ever touches it.
    let t = unsafe { TRANSPORT.get_ref() };
    unsafe {
        // Spin until accepted. Matches the reference adapters' pattern.
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
fn make_order(side: u8, oid: u64, price: i64, qty: u32) -> OrderRequest {
    OrderRequest {
        product_id: PRODUCT_ID,
        side: if side == 0 { OrderSide::Buy } else { OrderSide::Sell },
        price_type: PriceType::Limit,
        flags: OrderFlags::default(),
        quantity: qty,
        order_id: oid,
        // Harness ticks are non-negative (workload min is 0); cast is lossless.
        price: price as u64,
        submit_time: 0,
        expire_time: 0,
        _padding: [0u8; 24],
    }
}

// Run one incoming order through the engine and emit a Trade per fill. `seq`
// is stamped on each Trade (the aggressive order's seq); `taker_side`
// disambiguates which leg of the engine's Trade is the maker. Returns the
// taker's unfilled remaining quantity (what the engine rested / would rest).
//
// Caller passes the matcher-owned `book` + `shadow` (split-borrowed from the
// single ENGINE cell), so the engine's `last_outcome.trades` is read and the
// maker bookkeeping updated together while the matcher thread owns both.
fn match_and_emit(
    book: &mut SparseOrderBook,
    shadow: &mut HashMap<u64, Shadow>,
    seq: u64,
    taker_side: u8,
    taker_oid: u64,
    price: i64,
    qty: u32,
) -> u32 {
    let order = make_order(taker_side, taker_oid, price, qty);
    // Sparse match_order never returns Err (no range/tick validation).
    let _ = book.match_order(order);

    for tr in book.last_outcome.trades.iter() {
        // Resting order = maker; taker = the incoming order on `taker_side`.
        let (maker_oid, _taker) = if taker_side == 0 {
            // incoming is buy -> taker is the buy leg, maker the sell leg
            (tr.sell_order_id, tr.buy_order_id)
        } else {
            (tr.buy_order_id, tr.sell_order_id)
        };
        let r = Report {
            r#type: ME_TRADE,
            side: taker_side,
            _reserved: [0; 6],
            sequence_number: seq,
            order_id: 0,
            price_ticks: tr.price as i64,
            quantity: tr.quantity,
            _reserved2: 0,
            maker_order_id: maker_oid,
            taker_order_id: taker_oid,
            _reserved3: 0,
        };
        emit(&r);

        // Maintain the maker's shadow: partial fill decrements, full fill
        // removes (terminal -> no longer resting). Removing bounds the map to
        // the live resting set; an absent id reads as not-resting on the reject
        // paths, identical to the old alive=false sentinel.
        match shadow.get_mut(&maker_oid) {
            Some(s) if s.remaining > tr.quantity => s.remaining -= tr.quantity,
            Some(_) => {
                shadow.remove(&maker_oid);
            }
            None => {}
        }
    }

    // The taker's resting remainder: total minus everything it filled.
    let filled: u32 = book
        .last_outcome
        .trades
        .iter()
        .map(|t| t.quantity)
        .sum();
    qty.saturating_sub(filled)
}

// =============================================================================
// engine_init / engine_shutdown
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_init(_seed: u64, transport: *const MeTransport, sink: *mut c_void) {
    // SAFETY: engine_init runs once, before any other engine_* entry point, on
    // the matcher thread — no concurrent access to either global.
    unsafe {
        TRANSPORT.init(Transport {
            vtable: transport,
            sink,
        });

        // tick=1, base_price=0 -> the engine's price == harness tick directly,
        // and sparse skips range/tick checks anyway. trade_cap pre-sizes the
        // per-call trade Vec; a generous cap avoids reallocations on deep sweeps.
        let book = SparseOrderBook::new(1, 0, 1, 4096);
        ENGINE.init(EngineState {
            book,
            shadow: HashMap::with_capacity(1 << 21),
        });
    }
}

#[no_mangle]
pub extern "C" fn engine_shutdown() {
    // Load-run-once lifecycle; the globals live for the process. Nothing to do.
}

#[no_mangle]
pub extern "C" fn engine_flush() {
    // Synchronous matcher: every match/cancel runs and emits inline before the
    // hot-path call returns. Nothing deferred to drain.
}

// =============================================================================
// Hot path
// =============================================================================

#[no_mangle]
pub unsafe extern "C" fn engine_on_new_order(order: *const NewOrder) {
    let o = unsafe { &*order };

    // OrderAck first, so the canonical order (Ack, then Trades, then optional
    // IOC-residual CancelAck) matches the reference adapters.
    emit_ack(
        ME_ORDER_ACK,
        o.sequence_number,
        o.order_id,
        o.side,
        o.price_ticks,
        o.quantity,
    );

    // SAFETY: single matcher thread (see the module Threading note); this is
    // the only live borrow of ENGINE for the duration of this call.
    let st = unsafe { ENGINE.get() };

    let remaining = match_and_emit(
        &mut st.book,
        &mut st.shadow,
        o.sequence_number,
        o.side,
        o.order_id,
        o.price_ticks,
        o.quantity,
    );

    if o.ioc != 0 {
        // IOC: the engine matched what it could and rested the remainder as a
        // limit order. Pull that residual back out and emit one CancelAck for
        // it (the canonical IOC-residual report). The filled portion already
        // emitted Trades above.
        if remaining > 0 {
            let _ = st.book.cancel_order(o.order_id);
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

    // GTC: record resting state for the cancel/modify reject + echo paths.
    // Only insert when something actually rests; if the order fully filled on
    // entry nothing rests, so leave it ABSENT — a later cancel/modify finds no
    // entry and rejects, identical to the old alive=false sentinel but without
    // leaking a dead entry into the map.
    if remaining > 0 {
        st.shadow.insert(
            o.order_id,
            Shadow {
                price: o.price_ticks,
                side: o.side,
                remaining,
            },
        );
    }
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_cancel(cancel: *const CancelMsg) {
    let c = unsafe { &*cancel };
    // SAFETY: single matcher thread (see the module Threading note); this is
    // the only live borrow of ENGINE for the duration of this call.
    let st = unsafe { ENGINE.get() };

    let info = st
        .shadow
        .get(&c.order_id)
        .map(|s| (s.side, s.price, s.remaining));

    match info {
        Some((side, price, remaining)) => {
            // Shadow says it should be resting; confirm with the engine.
            if st.book.cancel_order(c.order_id) {
                emit_ack(
                    ME_CANCEL_ACK,
                    c.sequence_number,
                    c.order_id,
                    side,
                    price,
                    remaining,
                );
                // Terminal: drop the shadow entry so the map tracks only the
                // live resting set.
                st.shadow.remove(&c.order_id);
            } else {
                // Engine disagrees (filled away since the last shadow update).
                emit_ack(ME_CANCEL_REJECT, c.sequence_number, c.order_id, 0, 0, 0);
                st.shadow.remove(&c.order_id);
            }
        }
        None => {
            // Unknown / already filled / already cancelled -> reject.
            emit_ack(ME_CANCEL_REJECT, c.sequence_number, c.order_id, 0, 0, 0);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_modify(modify: *const ModifyMsg) {
    let m = unsafe { &*modify };
    // SAFETY: single matcher thread (see the module Threading note); this is
    // the only live borrow of ENGINE for the duration of this call.
    let st = unsafe { ENGINE.get() };

    // Absent shadow entry == not currently resting (terminal orders are
    // removed, never tombstoned) -> reject.
    let cur_side = match st.shadow.get(&m.order_id) {
        Some(s) => s.side,
        None => {
            emit_ack(ME_MODIFY_REJECT, m.sequence_number, m.order_id, 0, 0, 0);
            return;
        }
    };

    // Harness modify == cancel + reinsert (queue priority lost). Cancel through
    // the engine; if that succeeds, emit exactly one ModifyAck, then re-submit
    // on the same side at the new price/qty so any crossing fills emit as
    // Trades stamped with the modify's seq.
    if st.book.cancel_order(m.order_id) {
        emit_ack(
            ME_MODIFY_ACK,
            m.sequence_number,
            m.order_id,
            cur_side,
            m.new_price_ticks,
            m.new_quantity,
        );

        // Replace the old shadow id: drop it first so the map never carries
        // both the pre- and post-modify state for this order.
        st.shadow.remove(&m.order_id);

        let remaining = match_and_emit(
            &mut st.book,
            &mut st.shadow,
            m.sequence_number,
            cur_side,
            m.order_id,
            m.new_price_ticks,
            m.new_quantity,
        );

        // Re-insert only if the resubmitted residual actually rests; if it
        // fully filled, leave it ABSENT so a later cancel/modify rejects.
        if remaining > 0 {
            st.shadow.insert(
                m.order_id,
                Shadow {
                    price: m.new_price_ticks,
                    side: cur_side,
                    remaining,
                },
            );
        }
    } else {
        // Shadow thought it was resting but the engine had already removed it.
        emit_ack(ME_MODIFY_REJECT, m.sequence_number, m.order_id, 0, 0, 0);
        st.shadow.remove(&m.order_id);
    }
}

// =============================================================================
// Audit queries
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_query_best_bid() -> i64 {
    // SAFETY: single matcher thread (see the module Threading note).
    let st = unsafe { ENGINE.get_ref() };
    // Highest bid price with live resting quantity. The engine prunes lazily,
    // so skip levels whose only orders are cancelled/empty.
    for (&price, bucket) in st.book.bids.iter().rev() {
        if bucket.orders.iter().any(|o| o.is_active()) {
            return price as i64;
        }
    }
    i64::MIN
}

#[no_mangle]
pub extern "C" fn engine_query_best_ask() -> i64 {
    // SAFETY: single matcher thread (see the module Threading note).
    let st = unsafe { ENGINE.get_ref() };
    for (&price, bucket) in st.book.asks.iter() {
        if bucket.orders.iter().any(|o| o.is_active()) {
            return price as i64;
        }
    }
    i64::MAX
}

#[no_mangle]
pub extern "C" fn engine_query_depth_at(price_ticks: i64, side: c_uchar) -> u64 {
    if price_ticks < 0 {
        return 0;
    }
    // SAFETY: single matcher thread (see the module Threading note).
    let st = unsafe { ENGINE.get_ref() };
    let p = price_ticks as u64;
    let ladder = if side == 0 { &st.book.bids } else { &st.book.asks };
    match ladder.get(&p) {
        Some(bucket) => bucket
            .orders
            .iter()
            .filter(|o| o.is_active())
            .map(|o| o.remaining_quantity as u64)
            .sum(),
        None => 0,
    }
}
