/*
 * trademacher_adapter.cpp — matching_engine_api.h backed by TradeMatcher.
 *
 * TradeMatcher (https://github.com/TradeMatcher/match-engine, Maven artifact
 * match-engine-core, package com.tradematcher) is a Java price-time-priority
 * matching engine (per-symbol OrderBookImpl: a TreeMap of price "buckets" + a
 * single cross-bucket doubly-linked order list per side carrying time priority +
 * a HashMap<String,Order> id index, with an LMAX-Disruptor command pipeline on
 * top). This adapter does NOT run the Disruptor / WebSocket / journal stack: it
 * embeds a JVM via JNI and drives the engine's separable matching core
 * (OrderBookImpl) through the HarnessTradeMacher helper — exactly as the
 * engine's own GTCTest/FAKTest drive it.
 *
 * No TradeMatcher source is patched — all harness glue (per-message API, report
 * derivation, modify = cancel + reinsert) lives in this adapter and the helper.
 *
 * The adapter emits the report stream itself. HarnessTradeMacher writes one
 * record per MAKER fill the engine produces into an adapter-owned staging
 * buffer; this adapter turns each into a Trade report and adds the OrderAck /
 * CancelAck / ModifyAck (and CancelReject / ModifyReject) reports, pushing all
 * into the harness report transport.
 *
 * Threading: every engine_* call runs on the harness matcher thread — the same
 * thread that creates the JVM — so one cached JNIEnv is valid throughout. The
 * engine matches synchronously on that thread, so engine_flush() is a no-op. The
 * JVM's own service threads (GC, JIT) are created during engine_init; an engine
 * is free to use threads, so they are not policed.
 *
 * Modelled on the sibling JNI reference adapters in additional_references/
 * (coralme_adapter, cointossx_adapter, jlob_adapter). The engine-specific
 * differences: onCancel returns the side as 1(buy)/2(sell) with the
 * price staged in record 0 (like cointossx), and onNew/onModify return a fill
 * count (the filled quantity is recovered by summing the staged trades).
 */
#include "matching_engine_api.h"

#include <jni.h>
#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__aarch64__)
static inline void cpu_pause() { asm volatile("yield" ::: "memory"); }
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
static inline void cpu_pause() { _mm_pause(); }
#else
static inline void cpu_pause() {}
#endif

/* Full path to libjvm.so, baked in by build.sh. Overridable at run time with
 * the ME_JVM_LIB environment variable. */
#ifndef ME_JVM_LIB
#define ME_JVM_LIB "libjvm.so"
#endif

namespace {

JavaVM* g_jvm    = nullptr;
JNIEnv* g_env    = nullptr;
jobject g_engine = nullptr;      /* HarnessTradeMacher instance (global ref) */

jmethodID m_onNew, m_onCancel, m_onModify, m_setBuf, m_bestBid, m_bestAsk, m_depthAt;

const me_transport_t* g_transport = nullptr;   /* harness report transport */
void*                 g_sink      = nullptr;

/* Adapter-owned staging buffer: HarnessTradeMacher writes one record per fill,
 * this adapter converts each to a Trade report. 40-byte little-endian layout:
 *   sequence_number u64 @0, price_ticks i64 @8, quantity u32 @16,
 *   (pad @20), maker_order_id u64 @24, taker_order_id u64 @32.
 * onCancel additionally stages the cancelled order's price in record 0's price
 * field — read below as g_stage[0].price. */
struct StageTrade {
    uint64_t seq;
    int64_t  price;
    uint32_t qty;
    uint32_t _pad;
    uint64_t maker;
    uint64_t taker;
};
static_assert(sizeof(StageTrade) == 40, "StageTrade must be 40 bytes");

constexpr int STAGE_CAP = 4096;      /* far more than any one order fills */
StageTrade g_stage[STAGE_CAP];
jobject    g_byteBuf = nullptr;      /* direct ByteBuffer over g_stage (global) */

[[noreturn]] void fatal(const char* msg) {
    std::fprintf(stderr, "trademacher_adapter: %s\n", msg);
    if (g_env && g_env->ExceptionCheck()) g_env->ExceptionDescribe();
    std::fflush(stderr);
    std::abort();   /* the harness SIGABRT guard reports the engine as failed */
}

void check_exception(const char* where) {
    if (g_env->ExceptionCheck()) {
        std::fprintf(stderr, "trademacher_adapter: Java exception in %s\n", where);
        g_env->ExceptionDescribe();
        std::fflush(stderr);
        std::abort();
    }
}

/* Resolve the classpath: ./trademacher.classpath (written by build.sh), else the
 * ME_TM_CLASSPATH env var. */
std::string load_classpath() {
    if (FILE* f = std::fopen("trademacher.classpath", "rb")) {
        std::string cp;
        char buf[8192];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) cp.append(buf, n);
        std::fclose(f);
        while (!cp.empty() &&
               (cp.back() == '\n' || cp.back() == '\r' || cp.back() == ' '))
            cp.pop_back();
        if (!cp.empty()) return cp;
    }
    if (const char* env = std::getenv("ME_TM_CLASSPATH")) return env;
    fatal("no classpath: expected ./trademacher.classpath or $ME_TM_CLASSPATH "
          "(run: ./build.sh)");
}

void push_report(const me_report_t& r) {
    while (!g_transport->push(g_sink, &r)) cpu_pause();
}

/* Emit a non-trade report (OrderAck / CancelAck / ModifyAck / *Reject). */
void emit_ack(uint8_t type, uint64_t seq, uint64_t order_id,
              uint8_t side, int64_t price, uint32_t qty) {
    me_report_t r{};
    r.type            = type;
    r.sequence_number = seq;
    r.order_id        = order_id;
    r.side            = side;
    r.price_ticks     = price;
    r.quantity        = qty;
    push_report(r);
}

/* Convert the first `n` staged trades to Trade reports; return total filled. */
uint64_t emit_staged_trades(int n) {
    uint64_t filled = 0;
    for (int k = 0; k < n; ++k) {
        const StageTrade& t = g_stage[k];
        me_report_t r{};
        r.type            = ME_TRADE;
        r.sequence_number = t.seq;
        r.price_ticks     = t.price;
        r.quantity        = t.qty;
        r.maker_order_id  = t.maker;
        r.taker_order_id  = t.taker;
        push_report(r);
        filled += t.qty;
    }
    return filled;
}

}  // namespace

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport,
                 void* report_sink) {
    g_transport = transport;
    g_sink      = report_sink;

    std::memset(g_stage, 0, sizeof(g_stage));   /* first-touch staging pages */

    /* Load libjvm with RTLD_NODELETE so its lifetime is decoupled from this .so:
     * the harness's dlclose() at the end then cannot unload a live JVM. */
    const char* jvm_lib = std::getenv("ME_JVM_LIB");
    if (!jvm_lib) jvm_lib = ME_JVM_LIB;
    void* jvmlib = dlopen(jvm_lib, RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE);
    if (!jvmlib) fatal(dlerror());

    using CreateVM_t = jint (*)(JavaVM**, void**, void*);
    auto create_vm = reinterpret_cast<CreateVM_t>(dlsym(jvmlib, "JNI_CreateJavaVM"));
    if (!create_vm) fatal("dlsym(JNI_CreateJavaVM) failed");

    std::string cp_opt = "-Djava.class.path=" + load_classpath();
    /* SerialGC and a fixed, pre-touched 2 GiB heap remove heap-resize and
     * page-fault noise from the measured pass. */
    const char* opt_strings[] = {
        cp_opt.c_str(),
        "-XX:+UseSerialGC",
        "-Xms2g",
        "-Xmx2g",
        "-XX:+AlwaysPreTouch",
    };
    constexpr int NOPT = sizeof(opt_strings) / sizeof(opt_strings[0]);
    JavaVMOption opts[NOPT];
    for (int i = 0; i < NOPT; ++i)
        opts[i].optionString = const_cast<char*>(opt_strings[i]);

    JavaVMInitArgs args;
    args.version            = JNI_VERSION_1_8;
    args.nOptions           = NOPT;
    args.options            = opts;
    args.ignoreUnrecognized = JNI_FALSE;

    if (create_vm(&g_jvm, reinterpret_cast<void**>(&g_env), &args) != JNI_OK)
        fatal("JNI_CreateJavaVM failed (check the classpath and JDK)");

    jclass cls = g_env->FindClass("HarnessTradeMacher");
    if (!cls) fatal("HarnessTradeMacher not found on the classpath");

    /* Adjudicate each lookup immediately: a failed GetMethodID leaves a pending
     * NoSuchMethodError, and the JNI spec forbids calling most JNI functions
     * (including the next GetMethodID) with an exception pending. */
    auto lookup = [&](const char* name, const char* sig) -> jmethodID {
        jmethodID id = g_env->GetMethodID(cls, name, sig);
        if (!id) fatal(name);   /* fatal() prints the pending NoSuchMethodError */
        return id;
    };
    jmethodID ctor = lookup("<init>", "()V");
    m_onNew    = lookup("onNew",          "(JJJIII)I");
    m_onCancel = lookup("onCancel",       "(J)I");
    m_onModify = lookup("onModify",       "(JJJII)I");
    m_setBuf   = lookup("setTradeBuffer", "(Ljava/nio/ByteBuffer;)V");
    m_bestBid  = lookup("bestBid",        "()J");
    m_bestAsk  = lookup("bestAsk",        "()J");
    m_depthAt  = lookup("depthAt",        "(JI)J");
    jmethodID m_warmup = lookup("warmup", "()V");

    jobject obj = g_env->NewObject(cls, ctor);
    check_exception("HarnessTradeMacher.<init>");
    if (!obj) fatal("NewObject(HarnessTradeMacher) failed");
    g_engine = g_env->NewGlobalRef(obj);

    /* Hand the Java helper the adapter-owned staging buffer (wrapped once). */
    jobject bb = g_env->NewDirectByteBuffer(g_stage, sizeof(g_stage));
    if (!bb) fatal("NewDirectByteBuffer failed");
    g_byteBuf = g_env->NewGlobalRef(bb);
    g_env->CallVoidMethod(g_engine, m_setBuf, g_byteBuf);
    check_exception("setTradeBuffer");

    /* Warm the hot path now, while the harness is not timing. */
    g_env->CallVoidMethod(g_engine, m_warmup);
    check_exception("HarnessTradeMacher.warmup");
}

void engine_shutdown(void) {
    /* No DestroyJavaVM: the process exits immediately after, and tearing the JVM
     * down here would race the harness's dlclose() of this library. */
}

/* TradeMatcher matches synchronously on the calling thread — nothing pending. */
void engine_flush(void) {}

void engine_on_new_order(const new_order_t* o) {
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);
    jint n = g_env->CallIntMethod(g_engine, m_onNew,
        static_cast<jlong>(o->order_id), static_cast<jlong>(o->sequence_number),
        static_cast<jlong>(o->price_ticks), static_cast<jint>(o->quantity),
        static_cast<jint>(o->side), static_cast<jint>(o->ioc));
    check_exception("onNew");
    uint64_t filled = emit_staged_trades(n);
    if (o->ioc && filled < o->quantity)              // IOC residual cancellation
        emit_ack(ME_CANCEL_ACK, o->sequence_number, o->order_id, o->side,
                 o->price_ticks, static_cast<uint32_t>(o->quantity - filled));
}

void engine_on_cancel(const cancel_t* c) {
    /* onCancel answers from TradeMatcher's own id index (getOrderByID): returns
     * 1(buy)/2(sell) if the order was resting and is now removed, with its price
     * staged in g_stage[0].price; or 0 when the order is not resting. No
     * adapter-side order state exists. */
    jint s = g_env->CallIntMethod(g_engine, m_onCancel,
                                  static_cast<jlong>(c->order_id));
    check_exception("onCancel");
    if (s) {
        emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id,
                 static_cast<uint8_t>(s - 1), g_stage[0].price, 0);
    } else {
        // Order is not resting — already filled, already cancelled, or never
        // seen (a duplicate/stale cancel). Answer with a reject, not an ack.
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
    }
}

void engine_on_modify(const modify_t* m) {
    jint n = g_env->CallIntMethod(g_engine, m_onModify,
        static_cast<jlong>(m->order_id), static_cast<jlong>(m->sequence_number),
        static_cast<jlong>(m->new_price_ticks), static_cast<jint>(m->new_quantity),
        static_cast<jint>(m->side));
    check_exception("onModify");
    if (n < 0) {
        // Order not resting — a duplicate/stale modify. Answer with a reject.
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        return;
    }
    emit_staged_trades(n);
    emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
             m->side, m->new_price_ticks, m->new_quantity);
}

int64_t engine_query_best_bid(void) {
    jlong v = g_env->CallLongMethod(g_engine, m_bestBid);
    check_exception("bestBid");
    return static_cast<int64_t>(v);
}

int64_t engine_query_best_ask(void) {
    jlong v = g_env->CallLongMethod(g_engine, m_bestAsk);
    check_exception("bestAsk");
    return static_cast<int64_t>(v);
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    jlong v = g_env->CallLongMethod(g_engine, m_depthAt,
        static_cast<jlong>(price_ticks), static_cast<jint>(side));
    check_exception("depthAt");
    return static_cast<uint64_t>(v);
}

}  // extern "C"
