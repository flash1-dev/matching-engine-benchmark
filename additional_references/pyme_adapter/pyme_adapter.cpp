/*
 * pyme_adapter.cpp — connects Surbeivol/PythonMatchingEngine ("pyme") to the
 * matching-engine benchmark C ABI.
 *
 * pyme is a pure-Python matching engine (price-time priority, FIFO doubly
 * linked lists per price level). This adapter embeds CPython, boots the
 * interpreter once in engine_init, imports a thin orchestration module
 * (pyme_driver.py — which calls the engine's NATIVE Orderbook.send / .cancel /
 * ._orders API and never reimplements matching), and bridges each engine_*
 * ABI call to the matching driver function. The driver returns, per message, a
 * Python list of report tuples
 *     (rtype, side, seq, order_id, price_ticks, quantity, maker_id, taker_id)
 * which this file unpacks and pushes into the harness report transport.
 *
 * IOC = limit then cancel the rested residual (driver); modify = cancel +
 * reinsert (driver). Trade.price_ticks is the maker's resting price and
 * Trade.sequence_number is the aggressor's, both set in the driver.
 *
 * No engine source is patched: pyme already exposes everything the harness
 * needs (per-order ids, cancel-by-id, native crossing with maker price + maker/
 * taker ids, an authoritative per-order active flag).
 *
 * GIL: Py_Initialize leaves the GIL held on the init thread. The harness drives
 * the engine single-threaded — engine_init, every engine_on_* / query, flush and
 * engine_shutdown all run on the one pinned matcher thread (see src/harness.cpp:
 * init/hot-loop/flush/shutdown are all on the main thread; only the report
 * drainer is separate and never touches Python). So we acquire the GIL exactly
 * once (it is already held after Py_InitializeEx — we simply keep it, no
 * PyEval_SaveThread) and never re-acquire it per message; there is no per-call
 * PyGILState_Ensure/Release on the hot path. The GIL is released at
 * engine_shutdown.
 *
 * Build: see build.sh (python3-config --embed; the pyme repo dir + this dir are
 * baked into sys.path so the .so works regardless of the harness's cwd).
 */
#include "matching_engine_api.h"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

namespace {

const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

// Borrowed/owned references to the driver module and its callables, resolved
// once at init. These are owned references held for the process lifetime.
PyObject* g_mod              = nullptr;
PyObject* g_fn_on_new        = nullptr;
PyObject* g_fn_on_cancel     = nullptr;
PyObject* g_fn_on_modify     = nullptr;
PyObject* g_fn_best_bid      = nullptr;
PyObject* g_fn_best_ask      = nullptr;
PyObject* g_fn_depth_at      = nullptr;
PyObject* g_fn_init          = nullptr;
PyObject* g_fn_shutdown      = nullptr;

void fatal_py(const char* where) {
    fprintf(stderr, "[pyme_adapter] fatal Python error at %s\n", where);
    if (PyErr_Occurred()) PyErr_Print();
    fflush(stderr);
    abort();
}

void push_report(const me_report_t& r) {
    while (!g_transport->push(g_sink, &r)) { /* spin: queue momentarily full */ }
}

// Unpack the driver's return value — a list of 8-element report tuples — and
// push each into the transport. Steals nothing; `result` is decref'd by caller.
void emit_reports(PyObject* result) {
    if (result == nullptr) fatal_py("driver call returned NULL");
    const Py_ssize_t n = PyList_Size(result);
    if (n < 0) fatal_py("driver result not a list");
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* tup = PyList_GET_ITEM(result, i);   // borrowed
        // Layout: rtype, side, seq, order_id, price_ticks, quantity, maker, taker
        long long rtype = PyLong_AsLongLong(PyTuple_GET_ITEM(tup, 0));
        long long side  = PyLong_AsLongLong(PyTuple_GET_ITEM(tup, 1));
        unsigned long long seq =
            (unsigned long long)PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(tup, 2));
        unsigned long long oid =
            (unsigned long long)PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(tup, 3));
        long long px    = PyLong_AsLongLong(PyTuple_GET_ITEM(tup, 4));
        long long qty   = PyLong_AsLongLong(PyTuple_GET_ITEM(tup, 5));
        unsigned long long maker =
            (unsigned long long)PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(tup, 6));
        unsigned long long taker =
            (unsigned long long)PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(tup, 7));
        if (PyErr_Occurred()) fatal_py("report tuple field decode");

        me_report_t r{};
        r.type            = (uint8_t)rtype;
        r.side            = (uint8_t)side;
        r.sequence_number = (uint64_t)seq;
        r.order_id        = (uint64_t)oid;
        r.price_ticks     = (int64_t)px;
        r.quantity        = (uint32_t)qty;
        r.maker_order_id  = (uint64_t)maker;
        r.taker_order_id  = (uint64_t)taker;
        push_report(r);
    }
}

}  // namespace

extern "C" {

void engine_init(uint64_t seed, const me_transport_t* t, void* sink) {
    g_transport = t;
    g_sink      = sink;

    // Promote libpython's symbols into the GLOBAL namespace before booting the
    // interpreter. The harness dlopen()s this adapter without RTLD_GLOBAL, so
    // libpython (an NEEDED dep of this .so) is loaded local-only; numpy/pandas
    // C-extensions, dlopen()ed later by the interpreter, then fail to resolve
    // libpython symbols (e.g. PyObject_SelfIter). Re-opening libpython with
    // RTLD_GLOBAL fixes the resolution for every extension loaded afterwards.
#ifdef PYME_LIBPYTHON_SONAME
    if (!dlopen(PYME_LIBPYTHON_SONAME, RTLD_NOW | RTLD_GLOBAL)) {
        fprintf(stderr, "[pyme_adapter] dlopen(%s, RTLD_GLOBAL) failed: %s\n",
                PYME_LIBPYTHON_SONAME, dlerror());
        // Not fatal on its own — a statically/globally linked libpython would
        // still work — but numpy will likely fail to import if this missed.
    }
#endif

    // Boot the interpreter. The pyme repo + adapter dir are baked in by
    // build.sh via -DPYME_REPO_DIR / -DPYME_ADAPTER_DIR so sys.path is correct
    // no matter where the harness runs the .so from.
    //
    // Py_InitializeEx leaves the GIL held on this thread, which is exactly what
    // we want — we hold it for the whole run. On a *re-init* (the interpreter is
    // already resident because a prior engine_shutdown released the GIL with
    // PyEval_SaveThread), Py_InitializeEx is skipped, so re-acquire the GIL here
    // before touching any Python state.
    if (!Py_IsInitialized()) {
        Py_InitializeEx(0);   // 0 = do not install signal handlers (harness owns them)
    } else {
        PyGILState_Ensure();  // re-init: take the GIL back and keep it
    }

    // Prepend the engine repo and the adapter dir to sys.path.
    PyObject* sys_path = PySys_GetObject("path");   // borrowed
    if (!sys_path) fatal_py("PySys_GetObject(path)");
#ifdef PYME_REPO_DIR
    {
        PyObject* p = PyUnicode_FromString(PYME_REPO_DIR);
        PyList_Insert(sys_path, 0, p);
        Py_DECREF(p);
    }
#endif
#ifdef PYME_ADAPTER_DIR
    {
        PyObject* p = PyUnicode_FromString(PYME_ADAPTER_DIR);
        PyList_Insert(sys_path, 0, p);
        Py_DECREF(p);
    }
#endif

    g_mod = PyImport_ImportModule("pyme_driver");
    if (!g_mod) fatal_py("import pyme_driver");

    g_fn_on_new    = PyObject_GetAttrString(g_mod, "on_new_order");
    g_fn_on_cancel = PyObject_GetAttrString(g_mod, "on_cancel");
    g_fn_on_modify = PyObject_GetAttrString(g_mod, "on_modify");
    g_fn_best_bid  = PyObject_GetAttrString(g_mod, "query_best_bid");
    g_fn_best_ask  = PyObject_GetAttrString(g_mod, "query_best_ask");
    g_fn_depth_at  = PyObject_GetAttrString(g_mod, "query_depth_at");
    g_fn_init      = PyObject_GetAttrString(g_mod, "engine_init");
    g_fn_shutdown  = PyObject_GetAttrString(g_mod, "engine_shutdown");
    if (!g_fn_on_new || !g_fn_on_cancel || !g_fn_on_modify || !g_fn_best_bid ||
        !g_fn_best_ask || !g_fn_depth_at || !g_fn_init || !g_fn_shutdown)
        fatal_py("resolve driver functions");

    PyObject* r = PyObject_CallFunction(g_fn_init, "K", (unsigned long long)seed);
    if (!r) fatal_py("driver engine_init");
    Py_DECREF(r);

    // Keep the GIL held for the rest of the run. The harness drives every
    // engine_* call (and shutdown) on this same pinned thread, so holding the
    // GIL once here — rather than re-acquiring it per message — is correct and
    // removes a mutex acquire/release from the hot path. (Py_InitializeEx leaves
    // the GIL held, so there is nothing to acquire; we just do NOT release it.)
}

void engine_shutdown(void) {
    // The GIL is held continuously since engine_init (this runs on the same
    // pinned thread), so call straight through — no per-call Ensure/Release.
    if (g_fn_shutdown) {
        PyObject* r = PyObject_CallNoArgs(g_fn_shutdown);
        if (r) Py_DECREF(r); else PyErr_Clear();
    }
    // Release the GIL we have held for the whole run. The interpreter stays
    // resident; a subsequent engine_init re-acquires the GIL (see there).
    PyEval_SaveThread();
}

void engine_on_new_order(const new_order_t* o) {
    // GIL is already held (acquired once in engine_init); no per-call ensure.
    PyObject* result = PyObject_CallFunction(
        g_fn_on_new, "KKLIii",
        (unsigned long long)o->order_id,
        (unsigned long long)o->sequence_number,
        (long long)o->price_ticks,
        (unsigned int)o->quantity,
        (int)o->side,
        (int)o->ioc);
    emit_reports(result);
    Py_XDECREF(result);
}

void engine_on_cancel(const cancel_t* c) {
    // GIL is already held (acquired once in engine_init); no per-call ensure.
    PyObject* result = PyObject_CallFunction(
        g_fn_on_cancel, "KK",
        (unsigned long long)c->order_id,
        (unsigned long long)c->sequence_number);
    emit_reports(result);
    Py_XDECREF(result);
}

void engine_on_modify(const modify_t* m) {
    // GIL is already held (acquired once in engine_init); no per-call ensure.
    PyObject* result = PyObject_CallFunction(
        g_fn_on_modify, "KKLIi",
        (unsigned long long)m->order_id,
        (unsigned long long)m->sequence_number,
        (long long)m->new_price_ticks,
        (unsigned int)m->new_quantity,
        (int)m->side);
    emit_reports(result);
    Py_XDECREF(result);
}

void engine_flush(void) {
    // Fully synchronous: matching and report emission complete inside each
    // engine_on_* call (driver returns after the engine has finished), so the
    // pipeline is already drained here.
}

int64_t engine_query_best_bid(void) {
    // GIL is already held (acquired once in engine_init); no per-call ensure.
    PyObject* r = PyObject_CallNoArgs(g_fn_best_bid);
    int64_t out = INT64_MIN;
    if (!r) fatal_py("query_best_bid");
    if (r != Py_None) out = (int64_t)PyLong_AsLongLong(r);
    Py_DECREF(r);
    return out;
}

int64_t engine_query_best_ask(void) {
    // GIL is already held (acquired once in engine_init); no per-call ensure.
    PyObject* r = PyObject_CallNoArgs(g_fn_best_ask);
    int64_t out = INT64_MAX;
    if (!r) fatal_py("query_best_ask");
    if (r != Py_None) out = (int64_t)PyLong_AsLongLong(r);
    Py_DECREF(r);
    return out;
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    // GIL is already held (acquired once in engine_init); no per-call ensure.
    PyObject* r = PyObject_CallFunction(g_fn_depth_at, "Li",
                                        (long long)price_ticks, (int)side);
    uint64_t out = 0;
    if (!r) fatal_py("query_depth_at");
    out = (uint64_t)PyLong_AsUnsignedLongLong(r);
    if (PyErr_Occurred()) { PyErr_Clear(); out = 0; }
    Py_DECREF(r);
    return out;
}

}  // extern "C"
