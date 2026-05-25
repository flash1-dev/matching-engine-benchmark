//! orderbookrs_adapter — joaquinbejar/OrderBook-rs behind the harness
//! matching_engine_api.h ABI.
//!
//! The engine is pure Rust, so this whole adapter is one Rust cdylib that
//! exports the harness `engine_*` extern-C symbols directly. No C++ shim.
//!
//! What the engine provides natively (used here):
//!   - `OrderBook::<()>::add_limit_order(id, price, qty, side, GTC, None)` —
//!     synchronous, matches the incoming order against the book and rests
//!     any residual. Returns a Result that we read to detect IOC residuals
//!     and rejects.
//!   - `OrderBook::cancel_order(id)` — synchronous, returns `Ok(Some(_))`
//!     when the order was found and removed, `Ok(None)` when not present.
//!   - `OrderBook::best_bid()` / `best_ask()` — `Option<u128>`.
//!   - `OrderBook::bids` / `asks` (pub(super) skip-map; reached indirectly
//!     via `total_depth_at_levels` — but we want exact depth at one price,
//!     so we walk via `levels_in_range(price..=price, side)`).
//!   - `TradeListener` callback fires per `add_limit_order` with the full
//!     `MatchResult`; we drain its `trades()` into harness Trade reports.
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
//! (queue-priority lost) — the upstream `update_order` API is in-place,
//! which would not match the canonical behaviour, so the adapter does
//! the cancel + reinsert explicitly.

use orderbook_rs::OrderBook;
use orderbook_rs::orderbook::trade::TradeResult;
use pricelevel::{Id, Side, TimeInForce};
use std::cell::RefCell;
use std::os::raw::{c_uchar, c_uint, c_void};
use std::sync::Mutex;
use std::sync::OnceLock;

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

// Transport vtable + sink, set in engine_init. Stored as raw pointers/integers
// inside an atomic-friendly container so we never need to box-and-clone.
struct Transport {
    vtable: *const MeTransport,
    sink: *mut c_void,
}
// SAFETY: harness owns both pointers; once engine_init returns they are stable
// until engine_shutdown. We use them from the matcher thread only.
unsafe impl Send for Transport {}
unsafe impl Sync for Transport {}

static TRANSPORT: OnceLock<Transport> = OnceLock::new();
// The OrderBook itself: single instance, single-threaded matcher.
// Mutex is only contended by the matcher thread; OrderBook is &-method-only
// for the hot path, so we'd ideally hold &OrderBook directly, but a OnceLock
// of OrderBook would need it to be Sync — it is, but we also need the trade
// listener to capture g_cur_seq via a thread-local, which is fine.
static BOOK: OnceLock<OrderBook<()>> = OnceLock::new();

// Shadow map: per-order {side, price, remaining, alive}. Drives the reject
// path and CancelAck/ModifyAck side/price echo, mirroring the C++ adapters'
// approach. The engine has no public "is this order resting?" predicate that
// also returns side/price without an Arc clone, so the shadow is the simplest
// and fastest source of truth for the adapter's bookkeeping.
struct Shadow {
    price: i64,
    side: u8,
    remaining: u32,
    alive: bool,
}
// HashMap behind Mutex — single-matcher hot path, no real contention. Sized
// up front to skip rehashes in the workload.
static SHADOW: OnceLock<Mutex<std::collections::HashMap<u64, Shadow>>> = OnceLock::new();

// Per-call context — the seq of the currently-processing harness message
// and its order id. The TradeListener installed on the book reads these to
// stamp each Trade report. Single matcher thread, so a thread-local is the
// natural fit.
thread_local! {
    static CUR_SEQ: RefCell<u64> = const { RefCell::new(0) };
    static CUR_TAKER: RefCell<u64> = const { RefCell::new(0) };
}

#[inline]
fn emit(r: &Report) {
    let t = TRANSPORT.get().expect("transport not initialised");
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
    TRANSPORT
        .set(Transport {
            vtable: transport,
            sink,
        })
        .ok();

    // Install the book with a trade listener that converts every match-result
    // transaction into a harness Trade report. The listener fires inline from
    // add_limit_order on the matcher thread, so reading the per-call seq /
    // taker via thread-locals is well-defined.
    let listener = std::sync::Arc::new(|tr: &TradeResult| {
        let seq = CUR_SEQ.with(|s| *s.borrow());
        let taker_oid = CUR_TAKER.with(|s| *s.borrow());
        let mut shadow_borrow = SHADOW.get().unwrap().lock().unwrap();
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
            // Update shadow for the maker — partial fills decrement, full fills
            // mark dead. The harness's CancelReject contract depends on this.
            if let Some(s) = shadow_borrow.get_mut(&maker) {
                if s.remaining > qty {
                    s.remaining -= qty;
                } else {
                    s.remaining = 0;
                    s.alive = false;
                }
            }
        }
    });

    // with_trade_listener installs the closure at construction time so we
    // never need a `&mut OrderBook` after the OnceLock has taken ownership.
    let book = OrderBook::<()>::with_trade_listener("HARNESS", listener);
    BOOK.set(book).ok();

    SHADOW
        .set(Mutex::new(std::collections::HashMap::with_capacity(
            1 << 21,
        )))
        .ok();
}

#[no_mangle]
pub extern "C" fn engine_shutdown() {
    // The harness loads, runs once, and tears down. OnceLocks live for the
    // process lifetime; do nothing here. Mirrors the C++ adapters' shutdown
    // (which typically just resets unique_ptrs that the dlclose path would
    // free anyway).
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

    CUR_SEQ.with(|s| *s.borrow_mut() = o.sequence_number);
    CUR_TAKER.with(|s| *s.borrow_mut() = o.order_id);

    let book = BOOK.get().unwrap();
    let side = if o.side == 0 { Side::Buy } else { Side::Sell };
    let id = Id::Sequential(o.order_id);
    // GTC for both paths; IOC residual is handled below via the harness's
    // CancelAck synthesis, not via engine TimeInForce::Ioc. Doing so lets the
    // engine emit any trades through the listener first, then we synthesise
    // the residual CancelAck at a known point — keeping report ordering
    // deterministic and aligned with the C++ adapters.
    let result = book.add_limit_order(
        id,
        o.price_ticks as u128,
        o.quantity as u64,
        side,
        TimeInForce::Gtc,
        None,
    );

    let remaining: u32 = match result {
        Ok(_) => {
            // Engine accepted; remaining quantity is the book's resting balance
            // for this order — but `add_limit_order` doesn't return the
            // pre-/post-match decomposition. Read it back from the shadow via
            // total_quantity at the price level filtered to this id… or read
            // it from the engine's get_order. The latter is one DashMap lookup
            // and keeps the adapter independent of how matching distributed
            // partial fills across resting orders.
            book.get_order(id)
                .map(|arc| arc.visible_quantity() as u32)
                .unwrap_or(0)
        }
        Err(orderbook_rs::OrderBookError::InsufficientLiquidity { available, .. }) => {
            // Only reachable for IOC/FOK in the engine; we don't pass those
            // flags, so this branch in practice never fires. Treat as fully
            // filled by `available` units.
            (o.quantity as u64).saturating_sub(available) as u32
        }
        Err(_) => 0,
    };

    if o.ioc != 0 {
        // IOC: the engine accepted as GTC and the residual rested. Cancel the
        // residual and emit one CancelAck for the un-filled portion. The trade
        // reports for the filled portion were already pushed by the listener.
        if remaining > 0 {
            let _ = book.cancel_order(id);
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

    // GTC: remember resting state for cancel/modify reject paths.
    let mut shadow = SHADOW.get().unwrap().lock().unwrap();
    shadow.insert(
        o.order_id,
        Shadow {
            price: o.price_ticks,
            side: o.side,
            remaining,
            alive: remaining > 0,
        },
    );
}

#[no_mangle]
pub unsafe extern "C" fn engine_on_cancel(cancel: *const CancelMsg) {
    let c = unsafe { &*cancel };
    let mut shadow = SHADOW.get().unwrap().lock().unwrap();
    let s_opt = shadow.get(&c.order_id).map(|s| (s.side, s.price, s.remaining, s.alive));
    match s_opt {
        Some((side, price, remaining, true)) => {
            let book = BOOK.get().unwrap();
            let id = Id::Sequential(c.order_id);
            match book.cancel_order(id) {
                Ok(Some(_)) => {
                    emit_ack(
                        ME_CANCEL_ACK,
                        c.sequence_number,
                        c.order_id,
                        side,
                        price,
                        remaining,
                    );
                    if let Some(s) = shadow.get_mut(&c.order_id) {
                        s.alive = false;
                    }
                }
                _ => {
                    // Engine disagrees with shadow — the order was filled away
                    // between the shadow update and now. Reject.
                    emit_ack(ME_CANCEL_REJECT, c.sequence_number, c.order_id, 0, 0, 0);
                    if let Some(s) = shadow.get_mut(&c.order_id) {
                        s.alive = false;
                    }
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
    let book = BOOK.get().unwrap();

    let (cur_side, cur_alive) = {
        let shadow = SHADOW.get().unwrap().lock().unwrap();
        match shadow.get(&m.order_id) {
            Some(s) => (s.side, s.alive),
            None => (0u8, false),
        }
    };
    if !cur_alive {
        emit_ack(ME_MODIFY_REJECT, m.sequence_number, m.order_id, 0, 0, 0);
        return;
    }

    let id = Id::Sequential(m.order_id);
    // The harness modify contract is cancel + reinsert; do the cancel against
    // the engine, emit ModifyAck at the new price/qty, then re-submit as a
    // new order on the same side so any crossing fills emit through the trade
    // listener tagged with the modify's seq.
    if book.cancel_order(id).map(|opt| opt.is_some()).unwrap_or(false) {
        emit_ack(
            ME_MODIFY_ACK,
            m.sequence_number,
            m.order_id,
            cur_side,
            m.new_price_ticks,
            m.new_quantity,
        );

        CUR_SEQ.with(|s| *s.borrow_mut() = m.sequence_number);
        CUR_TAKER.with(|s| *s.borrow_mut() = m.order_id);

        let side = if cur_side == 0 { Side::Buy } else { Side::Sell };
        let _ = book.add_limit_order(
            id,
            m.new_price_ticks as u128,
            m.new_quantity as u64,
            side,
            TimeInForce::Gtc,
            None,
        );
        let remaining = book
            .get_order(id)
            .map(|arc| arc.visible_quantity() as u32)
            .unwrap_or(0);
        let mut shadow = SHADOW.get().unwrap().lock().unwrap();
        shadow.insert(
            m.order_id,
            Shadow {
                price: m.new_price_ticks,
                side: cur_side,
                remaining,
                alive: remaining > 0,
            },
        );
    } else {
        emit_ack(ME_MODIFY_REJECT, m.sequence_number, m.order_id, 0, 0, 0);
        let mut shadow = SHADOW.get().unwrap().lock().unwrap();
        if let Some(s) = shadow.get_mut(&m.order_id) {
            s.alive = false;
        }
    }
}

// =============================================================================
// Audit queries
// =============================================================================

#[no_mangle]
pub extern "C" fn engine_query_best_bid() -> i64 {
    let book = BOOK.get().unwrap();
    match book.best_bid() {
        Some(p) => p as i64,
        None => i64::MIN,
    }
}

#[no_mangle]
pub extern "C" fn engine_query_best_ask() -> i64 {
    let book = BOOK.get().unwrap();
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
    let book = BOOK.get().unwrap();
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

