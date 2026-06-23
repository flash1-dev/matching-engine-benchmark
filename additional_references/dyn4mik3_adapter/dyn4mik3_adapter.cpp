/*
 * dyn4mik3_adapter.cpp — dyn4mik3/OrderBook (pure-Python limit-order-book
 * matching engine) behind the harness matching_engine_api.h ABI by EMBEDDING
 * CPython.
 *
 * The engine is Python (OrderBook.process_order / cancel_order, red-black
 * OrderTree over sortedcontainers.SortedDict). This adapter boots an embedded
 * interpreter in engine_init, imports a thin translation module
 * (dyn4mik3_driver.py) that calls the engine's NATIVE methods, and converts the
 * report tuples that module returns into the wire me_report_t records, pushing
 * each into the harness report transport. NO matching is done in C++: every
 * fill, cancel, and modify is the engine's own.
 *
 * Faithfulness notes (see dyn4mik3_driver.py for the per-message mapping):
 *   - harness order_id/sequence_number are fed to the engine as the order's
 *     order_id/timestamp (from_data=True), so ids/labels are the harness's.
 *   - IOC = submit limit then cancel any rested remainder (engine has no IOC).
 *   - modify = engine cancel + engine crossing process_order (engine's native
 *     update_order does NOT cross on a reprice; this honours the harness's
 *     "Trade per crossing fill" modify contract with native calls only).
 *   - cancel/modify rejects come from the engine's own order_exists().
 *
 * The GIL is held throughout (single embedded interpreter, single matcher
 * thread = the harness's calling thread). engine_flush is a no-op: the engine
 * matches synchronously inside each call.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "matching_engine_api.h"

#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace {

const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

// Borrowed/owned references kept for the process lifetime.
PyObject* g_driver_mod  = nullptr;  // the dyn4mik3_driver module
PyObject* g_inst        = nullptr;  // the Driver() instance
// Cached bound-method callables (owned).
PyObject* g_m_on_new    = nullptr;
PyObject* g_m_on_cancel = nullptr;
PyObject* g_m_on_modify = nullptr;
PyObject* g_m_best_bid  = nullptr;
PyObject* g_m_best_ask  = nullptr;
PyObject* g_m_depth_at  = nullptr;

inline void emit(const me_report_t& r) {
    while (!g_transport->push(g_sink, &r)) { /* spin: queue full */ }
}

[[noreturn]] void die_py(const char* where) {
    std::fprintf(stderr, "dyn4mik3_adapter: fatal at %s\n", where);
    if (PyErr_Occurred()) PyErr_Print();
    std::abort();
}

// Convert one report tuple from the Python driver into a me_report_t and push.
// Tuple layouts (first element is the me_report_type_t value):
//   (0, seq, side, oid, price, qty)            OrderAck
//   (1, seq, price, qty, maker_id, taker_id)   Trade
//   (2, seq, side, oid, price)                 CancelAck
//   (3, seq, side, oid, price, qty)            ModifyAck
//   (4, seq, oid)                              CancelReject
//   (5, seq, oid)                              ModifyReject
void push_report_tuple(PyObject* t) {
    // Fast unchecked access — the driver always returns well-formed tuples.
    long type = PyLong_AsLong(PyTuple_GET_ITEM(t, 0));
    me_report_t r{};
    r.type = static_cast<uint8_t>(type);
    switch (type) {
        case ME_ORDER_ACK: {
            r.sequence_number = PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(t, 1));
            r.side            = static_cast<uint8_t>(PyLong_AsLong(PyTuple_GET_ITEM(t, 2)));
            r.order_id        = PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(t, 3));
            r.price_ticks     = static_cast<int64_t>(PyLong_AsLongLong(PyTuple_GET_ITEM(t, 4)));
            r.quantity        = static_cast<uint32_t>(PyLong_AsUnsignedLong(PyTuple_GET_ITEM(t, 5)));
            break;
        }
        case ME_TRADE: {
            r.sequence_number = PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(t, 1));
            r.price_ticks     = static_cast<int64_t>(PyLong_AsLongLong(PyTuple_GET_ITEM(t, 2)));
            r.quantity        = static_cast<uint32_t>(PyLong_AsUnsignedLong(PyTuple_GET_ITEM(t, 3)));
            r.maker_order_id  = PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(t, 4));
            r.taker_order_id  = PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(t, 5));
            break;
        }
        case ME_CANCEL_ACK: {
            r.sequence_number = PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(t, 1));
            r.side            = static_cast<uint8_t>(PyLong_AsLong(PyTuple_GET_ITEM(t, 2)));
            r.order_id        = PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(t, 3));
            r.price_ticks     = static_cast<int64_t>(PyLong_AsLongLong(PyTuple_GET_ITEM(t, 4)));
            break;
        }
        case ME_MODIFY_ACK: {
            r.sequence_number = PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(t, 1));
            r.side            = static_cast<uint8_t>(PyLong_AsLong(PyTuple_GET_ITEM(t, 2)));
            r.order_id        = PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(t, 3));
            r.price_ticks     = static_cast<int64_t>(PyLong_AsLongLong(PyTuple_GET_ITEM(t, 4)));
            r.quantity        = static_cast<uint32_t>(PyLong_AsUnsignedLong(PyTuple_GET_ITEM(t, 5)));
            break;
        }
        case ME_CANCEL_REJECT:
        case ME_MODIFY_REJECT: {
            r.sequence_number = PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(t, 1));
            r.order_id        = PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(t, 2));
            break;
        }
        default:
            return;
    }
    emit(r);
}

// Drain a Python list of report tuples, pushing each, then drop the list.
void drain_reports(PyObject* list) {
    if (!list) die_py("driver returned NULL");
    Py_ssize_t n = PyList_GET_SIZE(list);
    for (Py_ssize_t i = 0; i < n; ++i) {
        push_report_tuple(PyList_GET_ITEM(list, i));
    }
    if (PyErr_Occurred()) die_py("report conversion");
    Py_DECREF(list);
}

}  // namespace

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport, void* sink) {
    g_transport = transport;
    g_sink      = sink;

    // The harness dlopen()s this adapter with RTLD_LOCAL, so libpython's
    // symbols (loaded as our NEEDED dependency) are NOT in the global namespace.
    // CPython loads its C extension modules (_decimal, _contextvars, ...) at
    // runtime via dlopen(), and those resolve core symbols (PyFloat_Type, ...)
    // from the global scope — which fails under RTLD_LOCAL. Re-open libpython
    // with RTLD_GLOBAL | RTLD_NOLOAD to promote the already-mapped image's
    // symbols to global, so every later extension-module load resolves. (The
    // engine's Decimal arithmetic is _decimal, hence required.)
    if (!dlopen(ME_LIBPYTHON, RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD)) {
        // NOLOAD failed (not yet mapped under this name) — try a plain global
        // load so the symbols still become visible.
        dlopen(ME_LIBPYTHON, RTLD_NOW | RTLD_GLOBAL);
    }

    if (!Py_IsInitialized()) {
        Py_InitializeEx(0);  // 0 = do not install signal handlers (harness owns them)
    }

    // Build sys.path with three absolute dirs, all baked in at build time via
    // -D so the .so is independent of the CWD the harness runs from:
    //   ME_REPO_DIR    — the upstream engine clone (holds the `orderbook/`
    //                    package; under third_party/<clone> after build.sh)
    //   ME_ADAPTER_DIR — this adapter's own directory (holds dyn4mik3_driver.py)
    //   ME_VENDOR_DIR  — the vendored sortedcontainers tree
    const char* repo_dir    = ME_REPO_DIR;      // dir holding orderbook/
    const char* adapter_dir = ME_ADAPTER_DIR;   // dir holding dyn4mik3_driver.py
    const char* vendor_dir  = ME_VENDOR_DIR;    // dir holding sortedcontainers/

    PyObject* sys_path = PySys_GetObject("path");  // borrowed
    if (!sys_path) die_py("sys.path");
    PyObject* p1 = PyUnicode_FromString(repo_dir);
    PyObject* p2 = PyUnicode_FromString(adapter_dir);
    PyObject* p3 = PyUnicode_FromString(vendor_dir);
    PyList_Insert(sys_path, 0, p1);
    PyList_Insert(sys_path, 0, p2);
    PyList_Insert(sys_path, 0, p3);
    Py_XDECREF(p1);
    Py_XDECREF(p2);
    Py_XDECREF(p3);

    g_driver_mod = PyImport_ImportModule("dyn4mik3_driver");
    if (!g_driver_mod) die_py("import dyn4mik3_driver");

    PyObject* DriverCls = PyObject_GetAttrString(g_driver_mod, "Driver");
    if (!DriverCls) die_py("Driver class");
    g_inst = PyObject_CallNoArgs(DriverCls);
    Py_DECREF(DriverCls);
    if (!g_inst) die_py("Driver()");

    g_m_on_new    = PyObject_GetAttrString(g_inst, "on_new");
    g_m_on_cancel = PyObject_GetAttrString(g_inst, "on_cancel");
    g_m_on_modify = PyObject_GetAttrString(g_inst, "on_modify");
    g_m_best_bid  = PyObject_GetAttrString(g_inst, "best_bid");
    g_m_best_ask  = PyObject_GetAttrString(g_inst, "best_ask");
    g_m_depth_at  = PyObject_GetAttrString(g_inst, "depth_at");
    if (!g_m_on_new || !g_m_on_cancel || !g_m_on_modify ||
        !g_m_best_bid || !g_m_best_ask || !g_m_depth_at) {
        die_py("bind methods");
    }
}

void engine_shutdown(void) {
    Py_XDECREF(g_m_on_new);
    Py_XDECREF(g_m_on_cancel);
    Py_XDECREF(g_m_on_modify);
    Py_XDECREF(g_m_best_bid);
    Py_XDECREF(g_m_best_ask);
    Py_XDECREF(g_m_depth_at);
    Py_XDECREF(g_inst);
    Py_XDECREF(g_driver_mod);
    g_m_on_new = g_m_on_cancel = g_m_on_modify = nullptr;
    g_m_best_bid = g_m_best_ask = g_m_depth_at = nullptr;
    g_inst = g_driver_mod = nullptr;
    // Leave the interpreter up; the harness process exits after shutdown.
}

void engine_flush(void) { /* synchronous matcher — no deferred work */ }

void engine_on_new_order(const new_order_t* o) {
    // on_new(oid, seq, price, qty, side, ioc)
    PyObject* args = Py_BuildValue(
        "(KKLIii)",
        (unsigned long long)o->order_id,
        (unsigned long long)o->sequence_number,
        (long long)o->price_ticks,
        (unsigned int)o->quantity,
        (int)o->side,
        (int)o->ioc);
    PyObject* res = PyObject_CallObject(g_m_on_new, args);
    Py_DECREF(args);
    drain_reports(res);
}

void engine_on_cancel(const cancel_t* c) {
    // on_cancel(oid, seq)
    PyObject* args = Py_BuildValue(
        "(KK)",
        (unsigned long long)c->order_id,
        (unsigned long long)c->sequence_number);
    PyObject* res = PyObject_CallObject(g_m_on_cancel, args);
    Py_DECREF(args);
    drain_reports(res);
}

void engine_on_modify(const modify_t* m) {
    // on_modify(oid, seq, new_price, new_qty, side)
    PyObject* args = Py_BuildValue(
        "(KKLIi)",
        (unsigned long long)m->order_id,
        (unsigned long long)m->sequence_number,
        (long long)m->new_price_ticks,
        (unsigned int)m->new_quantity,
        (int)m->side);
    PyObject* res = PyObject_CallObject(g_m_on_modify, args);
    Py_DECREF(args);
    drain_reports(res);
}

int64_t engine_query_best_bid(void) {
    PyObject* r = PyObject_CallNoArgs(g_m_best_bid);
    if (!r) die_py("best_bid");
    int64_t v;
    if (r == Py_None) {
        v = INT64_MIN;
    } else {
        v = static_cast<int64_t>(PyLong_AsLongLong(r));
    }
    Py_DECREF(r);
    return v;
}

int64_t engine_query_best_ask(void) {
    PyObject* r = PyObject_CallNoArgs(g_m_best_ask);
    if (!r) die_py("best_ask");
    int64_t v;
    if (r == Py_None) {
        v = INT64_MAX;
    } else {
        v = static_cast<int64_t>(PyLong_AsLongLong(r));
    }
    Py_DECREF(r);
    return v;
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    PyObject* args = Py_BuildValue("(Li)", (long long)price_ticks, (int)side);
    PyObject* r = PyObject_CallObject(g_m_depth_at, args);
    Py_DECREF(args);
    if (!r) die_py("depth_at");
    uint64_t v = static_cast<uint64_t>(PyLong_AsUnsignedLongLong(r));
    Py_DECREF(r);
    return v;
}

}  // extern "C"
