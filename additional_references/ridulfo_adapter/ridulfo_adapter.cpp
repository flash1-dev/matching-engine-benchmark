/*
 * ridulfo_adapter.cpp — ridulfo/order-matching-engine behind the harness
 * matching_engine_api.h ABI.
 *
 * ridulfo is a PURE-PYTHON matching engine (ordermatchinengine.Orderbook,
 * a price-time-priority book over two sortedcontainers.SortedList instances).
 * There is no native C/Rust core — the README's "400k orders/s, 2.5us" figure
 * is the PyPy interpreter running this same Python. So the adapter embeds
 * CPython 3.12 (python3-config --embed), imports a thin driver module
 * (ridulfo_helper.py), and calls into the engine per message.
 *
 * ALL MATCHING IS THE ENGINE'S. The helper only marshals arguments, reads the
 * engine's own Orderbook.trades / .bids / .asks back out, and applies the two
 * rules the engine API cannot express (IOC must not rest its remainder; cancel/
 * modify liveness, which the engine reports via no result code). Report
 * synthesis (the six me_report_t kinds) lives here in C++, exactly mirroring the
 * mansoor / exchange-core reference adapters.
 *
 * Engine source carries ONE correctness patch, applied by build.sh: a stable
 * total order in LimitOrder.__lt__ (a final tiebreak on order_id), fixing a
 * comparator that fell to a size compare and returned None on a full tie —
 * which inverts equal-price priority and loses cancels. See
 * ridulfo/order-matching-engine#10. Everything else the adapter needs is in the
 * helper, documented there and in the adapter README. This C++ bridge is
 * engine-agnostic.
 *
 * Threading: every engine_* call runs on the harness matcher thread (the one
 * that initialises the interpreter); the GIL is held throughout via a single
 * cached thread state. The engine matches synchronously, so engine_flush() is a
 * no-op.
 */
#include "matching_engine_api.h"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

/* Strictly increasing arrival counter -> Order.time, giving the engine
 * deterministic FIFO tie-breaking among equal-price orders (its intended
 * arrival-time semantics; the live engine uses wall-clock microseconds). */
uint64_t g_arrival = 0;

/* Cached references into ridulfo_helper. Borrowed from the module dict where
 * noted; the callables are kept as owned (new) refs for the run's lifetime. */
PyObject* g_mod          = nullptr;
PyObject* g_fn_reset     = nullptr;
PyObject* g_fn_submit_l  = nullptr;
PyObject* g_fn_submit_m  = nullptr;
PyObject* g_fn_cancel    = nullptr;
PyObject* g_fn_modify    = nullptr;
PyObject* g_fn_best_bid  = nullptr;
PyObject* g_fn_best_ask  = nullptr;
PyObject* g_fn_depth_at  = nullptr;

[[noreturn]] void fatal(const char* msg) {
    std::fprintf(stderr, "ridulfo_adapter: %s\n", msg);
    if (PyErr_Occurred()) PyErr_Print();
    std::fflush(stderr);
    std::abort();   /* harness SIGABRT guard -> engine reported as failed */
}

inline void check_err(const char* where) {
    if (PyErr_Occurred()) {
        std::fprintf(stderr, "ridulfo_adapter: Python error in %s\n", where);
        PyErr_Print();
        std::fflush(stderr);
        std::abort();
    }
}

inline void emit(const me_report_t& r) {
    while (!g_transport->push(g_sink, &r)) { /* spin: queue full */ }
}

inline void emit_simple(uint8_t type, uint64_t seq, uint64_t order_id,
                        uint8_t side, int64_t price, uint32_t qty) {
    me_report_t r{};
    r.type            = type;
    r.sequence_number = seq;
    r.order_id        = order_id;
    r.side            = side;
    r.price_ticks     = price;
    r.quantity        = qty;
    emit(r);
}

/* Emit one Trade per (price, qty, maker_id) tuple the helper returned, in match
 * order. seq = the aggressive order's sequence_number; taker = the aggressive
 * order id; price = the maker's resting price (the engine fills Trade.price with
 * bookOrder.price). */
void emit_trades(PyObject* trade_list, uint64_t seq, uint64_t taker_id,
                 uint8_t taker_side) {
    Py_ssize_t n = PyList_GET_SIZE(trade_list);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* t = PyList_GET_ITEM(trade_list, i);     /* (price, qty, maker) */
        long long price  = PyLong_AsLongLong(PyTuple_GET_ITEM(t, 0));
        long long qty    = PyLong_AsLongLong(PyTuple_GET_ITEM(t, 1));
        long long maker  = PyLong_AsLongLong(PyTuple_GET_ITEM(t, 2));
        me_report_t r{};
        r.type            = ME_TRADE;
        r.sequence_number = seq;
        r.side            = taker_side;
        r.price_ticks     = static_cast<int64_t>(price);
        r.quantity        = static_cast<uint32_t>(qty);
        r.maker_order_id  = static_cast<uint64_t>(maker);
        r.taker_order_id  = taker_id;
        r.order_id        = taker_id;
        emit(r);
    }
    check_err("emit_trades");
}

}  // namespace

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport, void* sink) {
    g_transport = transport;
    g_sink      = sink;
    g_arrival   = 0;

    if (!Py_IsInitialized()) {
        PyConfig config;
        PyConfig_InitIsolatedConfig(&config);
        PyStatus st = Py_InitializeFromConfig(&config);
        PyConfig_Clear(&config);
        if (PyStatus_Exception(st)) fatal("Py_InitializeFromConfig failed");
    }

    /* Prepend the dirs baked in by build.sh to sys.path so the driver
     * (ridulfo_helper.py), the engine package (ordermatchinengine/, from the
     * upstream clone) and the vendored sortedcontainers all resolve, regardless
     * of the harness's working directory. RIDULFO_REPO_DIR is the engine clone;
     * RIDULFO_ADAPTER_DIR is this adapter dir (the driver); RIDULFO_VENDOR_DIR
     * holds the vendored sortedcontainers under third_party/. Any of these may
     * coincide, in which case the duplicate insert is harmless. The legacy
     * single-dir macro RIDULFO_DIR is honored as a fallback. */
    PyObject* sys_path = PySys_GetObject("path");   /* borrowed */
    if (!sys_path) fatal("sys.path missing");
    auto add_path = [&](const char* p) {
        PyObject* dir = PyUnicode_FromString(p);
        PyList_Insert(sys_path, 0, dir);
        Py_DECREF(dir);
    };
#if defined(RIDULFO_REPO_DIR) || defined(RIDULFO_ADAPTER_DIR) || defined(RIDULFO_VENDOR_DIR)
#  ifdef RIDULFO_REPO_DIR
    add_path(RIDULFO_REPO_DIR);
#  endif
#  ifdef RIDULFO_ADAPTER_DIR
    add_path(RIDULFO_ADAPTER_DIR);
#  endif
#  ifdef RIDULFO_VENDOR_DIR
    add_path(RIDULFO_VENDOR_DIR);
#  endif
#else
    add_path(RIDULFO_DIR);
#endif

    g_mod = PyImport_ImportModule("ridulfo_helper");
    if (!g_mod) fatal("import ridulfo_helper failed");

    auto load = [](const char* name) -> PyObject* {
        PyObject* fn = PyObject_GetAttrString(g_mod, name);
        if (!fn || !PyCallable_Check(fn)) {
            std::fprintf(stderr, "ridulfo_adapter: helper.%s missing\n", name);
            std::abort();
        }
        return fn;
    };
    g_fn_reset    = load("reset");
    g_fn_submit_l = load("submit_limit");
    g_fn_submit_m = load("submit_market");
    g_fn_cancel   = load("cancel");
    g_fn_modify   = load("modify");
    g_fn_best_bid = load("best_bid");
    g_fn_best_ask = load("best_ask");
    g_fn_depth_at = load("depth_at");

    /* Fresh book. */
    PyObject* r = PyObject_CallNoArgs(g_fn_reset);
    if (!r) fatal("helper.reset failed");
    Py_DECREF(r);
}

void engine_shutdown(void) {
    /* Leave the interpreter initialised (a re-init within the same process is
     * fragile); just drop the book so a re-run starts clean. */
    if (g_fn_reset) {
        PyObject* r = PyObject_CallNoArgs(g_fn_reset);
        Py_XDECREF(r);
        if (PyErr_Occurred()) PyErr_Clear();
    }
}

void engine_flush(void) { /* synchronous engine — nothing in flight */ }

void engine_on_new_order(const new_order_t* o) {
    /* OrderAck first (one per accepted new order). */
    emit_simple(ME_ORDER_ACK, o->sequence_number, o->order_id,
                o->side, o->price_ticks, o->quantity);

    const uint64_t arrival = g_arrival++;

    /* Always a limit order in the canonical workload; IOC handled natively as a
     * limit that does not rest its remainder. (submit_market is wired for
     * completeness should a market order ever appear.) */
    PyObject* res = PyObject_CallFunction(
        g_fn_submit_l, "KiILKi",
        (unsigned long long)o->order_id,
        (int)o->side,
        (unsigned int)o->quantity,
        (long long)o->price_ticks,
        (unsigned long long)arrival,
        (int)(o->ioc ? 1 : 0));
    if (!res) fatal("submit_limit failed");

    PyObject* trades   = PyTuple_GET_ITEM(res, 0);             /* borrowed */
    long long residual = PyLong_AsLongLong(PyTuple_GET_ITEM(res, 1));
    emit_trades(trades, o->sequence_number, o->order_id, o->side);

    if (o->ioc && residual > 0) {
        /* IOC residual -> CancelAck for the unfilled remainder. */
        emit_simple(ME_CANCEL_ACK, o->sequence_number, o->order_id,
                    o->side, o->price_ticks, (uint32_t)residual);
    }
    Py_DECREF(res);
}

void engine_on_cancel(const cancel_t* c) {
    PyObject* res = PyObject_CallFunction(g_fn_cancel, "K",
                                          (unsigned long long)c->order_id);
    if (!res) fatal("cancel failed");
    /* (resting, side, price) — side/price echo the cancelled order's, read from
     * the engine's own book. */
    int       resting = PyObject_IsTrue(PyTuple_GET_ITEM(res, 0));
    uint8_t   side    = (uint8_t)PyLong_AsLong(PyTuple_GET_ITEM(res, 1));
    int64_t   price   = (int64_t)PyLong_AsLongLong(PyTuple_GET_ITEM(res, 2));
    Py_DECREF(res);

    if (resting)
        emit_simple(ME_CANCEL_ACK, c->sequence_number, c->order_id, side, price, 0);
    else
        emit_simple(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
}

void engine_on_modify(const modify_t* m) {
    const uint64_t arrival = g_arrival++;
    PyObject* res = PyObject_CallFunction(
        g_fn_modify, "KiILK",
        (unsigned long long)m->order_id,
        (int)m->side,
        (unsigned int)m->new_quantity,
        (long long)m->new_price_ticks,
        (unsigned long long)arrival);
    if (!res) fatal("modify failed");

    PyObject* ok_obj = PyTuple_GET_ITEM(res, 0);    /* borrowed */
    PyObject* trades = PyTuple_GET_ITEM(res, 1);    /* borrowed */
    int ok = PyObject_IsTrue(ok_obj);

    if (!ok) {
        emit_simple(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        Py_DECREF(res);
        return;
    }
    /* Trades from the reinsert (in match order), then exactly one ModifyAck. */
    emit_trades(trades, m->sequence_number, m->order_id, m->side);
    emit_simple(ME_MODIFY_ACK, m->sequence_number, m->order_id,
                m->side, m->new_price_ticks, m->new_quantity);
    Py_DECREF(res);
}

int64_t engine_query_best_bid(void) {
    PyObject* r = PyObject_CallNoArgs(g_fn_best_bid);
    if (!r) fatal("best_bid failed");
    int64_t out = INT64_MIN;
    if (r != Py_None) out = (int64_t)PyLong_AsLongLong(r);
    Py_DECREF(r);
    check_err("best_bid");
    return out;
}

int64_t engine_query_best_ask(void) {
    PyObject* r = PyObject_CallNoArgs(g_fn_best_ask);
    if (!r) fatal("best_ask failed");
    int64_t out = INT64_MAX;
    if (r != Py_None) out = (int64_t)PyLong_AsLongLong(r);
    Py_DECREF(r);
    check_err("best_ask");
    return out;
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    PyObject* r = PyObject_CallFunction(g_fn_depth_at, "Li",
                                        (long long)price_ticks, (int)side);
    if (!r) fatal("depth_at failed");
    uint64_t out = (uint64_t)PyLong_AsUnsignedLongLong(r);
    Py_DECREF(r);
    check_err("depth_at");
    return out;
}

}  // extern "C"
