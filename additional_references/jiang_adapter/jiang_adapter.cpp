/*
 * jiang_adapter.cpp — matching_engine_api.h backed by FastMatchingEngine.
 *
 * FastMatchingEngine (https://github.com/JiangYongKang/FastMatchingEngine) is a
 * dependency-free Java digital-currency matching engine POC: a price-time-
 * priority CLOB (TreeMap<BigDecimal,OrderBucket> per side, each bucket a FIFO
 * LinkedHashMap, plus a HashMap id index). The harness is native, so this
 * adapter embeds a JVM via JNI and drives a com.fast.matching.engine.OrderBook
 * through the HarnessJiang Java helper. Built as a shared library by build.sh.
 *
 * Three minimal engine patches are applied in build.sh — two real bug fixes
 * (idMaps was never pruned on the cancel path or when a maker was fully filled,
 * burning the id so a reinsert was dropped and a later cancel NPE'd) and one
 * read-only accessor (getOrder) so the helper can ask the engine whether an id
 * is resting. No matching logic is changed. See build.sh PATCHES.
 *
 * The adapter emits the report stream itself. HarnessJiang writes one fill
 * record per trade the engine produces into an adapter-owned staging buffer;
 * this adapter turns each into a Trade report and adds the OrderAck / CancelAck /
 * ModifyAck (and CancelReject / ModifyReject) reports, pushing all of them into
 * the harness report transport. Modify is cancel + reinsert, in the helper.
 *
 * Threading: every engine_* call runs on the harness matcher thread — the same
 * thread that creates the JVM — so one cached JNIEnv is valid throughout. The
 * engine matches synchronously on that thread, so engine_flush() is a no-op. The
 * JVM's own service threads (GC, JIT) are created during engine_init; an engine
 * is free to use threads, so they are not policed.
 *
 * Mirrors adapters/exchange_core_adapter.cpp and the coralme / cointossx JNI
 * adapters; the only engine-specific differences are the helper method
 * signatures (onCancel returns 1/0 with the cancelled side via lastCancelSide()).
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
jobject g_engine = nullptr;      /* HarnessJiang instance (global ref) */

jmethodID m_onNew, m_onCancel, m_onModify, m_setBuf, m_bestBid, m_bestAsk, m_depthAt;
jmethodID m_lastCancelSide;
jmethodID m_setBatchIn, m_setBatchOut, m_onBatch;   /* batch path */

const me_transport_t* g_transport = nullptr;   /* harness report transport */
void*                 g_sink      = nullptr;

/* Adapter-owned staging buffer: HarnessJiang writes one record per fill, this
 * adapter converts each to a Trade report. 40-byte little-endian layout:
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

/* Max fills one order can stage. Must exceed the deepest single-order sweep the
 * harness can deliver: the conformance suite's deep_recursive_sweep_5000 crosses
 * one IOC through 5000 resting levels — 5000 fills in a single engine_on_new_order
 * — so the previous 4096 overflowed this staging buffer (an IndexOutOfBoundsException
 * on the ~4096th fill, on BOTH the per-message and the batch path). 8192 clears
 * that worst case with headroom; the canonical workloads fill far fewer. Kept in
 * sync with HarnessJiang.STAGE_CAP (the Java side sizes its report reserve off it). */
constexpr int STAGE_CAP = 8192;
StageTrade g_stage[STAGE_CAP];
jobject    g_byteBuf = nullptr;      /* direct ByteBuffer over g_stage (global) */

/* Batch path (engine_on_batch): HarnessJiang.onBatch writes the FULL me_report_t
 * stream for a run of messages straight into this buffer; this side just drains
 * it into the transport. 65536 reports = 4 MB. */
constexpr int OUT_CAP = 1 << 16;
me_report_t g_outbuf[OUT_CAP];
jobject     g_outBuf = nullptr;      /* direct ByteBuffer over g_outbuf (global) */

[[noreturn]] void fatal(const char* msg) {
    std::fprintf(stderr, "jiang_adapter: %s\n", msg);
    if (g_env && g_env->ExceptionCheck()) g_env->ExceptionDescribe();
    std::fflush(stderr);
    std::abort();   /* the harness SIGABRT guard reports the engine as failed */
}

void check_exception(const char* where) {
    if (g_env->ExceptionCheck()) {
        std::fprintf(stderr, "jiang_adapter: Java exception in %s\n", where);
        g_env->ExceptionDescribe();
        std::fflush(stderr);
        std::abort();
    }
}

/* Resolve the classpath: ./jiang.classpath (written by build.sh), else the
 * ME_JIANG_CLASSPATH env var. */
std::string load_classpath() {
    if (FILE* f = std::fopen("jiang.classpath", "rb")) {
        std::string cp;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) cp.append(buf, n);
        std::fclose(f);
        while (!cp.empty() &&
               (cp.back() == '\n' || cp.back() == '\r' || cp.back() == ' '))
            cp.pop_back();
        if (!cp.empty()) return cp;
    }
    if (const char* env = std::getenv("ME_JIANG_CLASSPATH")) return env;
    fatal("no classpath: expected ./jiang.classpath or $ME_JIANG_CLASSPATH "
          "(run: build.sh)");
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

    /* First-touch the staging buffer now so its pages never minor-fault inside
     * the timed window. */
    std::memset(g_stage, 0, sizeof(g_stage));

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

    jclass cls = g_env->FindClass("HarnessJiang");
    if (!cls) fatal("HarnessJiang not found on the classpath");

    /* Adjudicate each lookup immediately: a failed GetMethodID leaves a pending
     * NoSuchMethodError, and the JNI spec forbids calling most JNI functions
     * (including the next GetMethodID) with an exception pending. */
    auto lookup = [&](const char* name, const char* sig) -> jmethodID {
        jmethodID id = g_env->GetMethodID(cls, name, sig);
        if (!id) fatal(name);   /* fatal() prints the pending NoSuchMethodError */
        return id;
    };
    jmethodID ctor   = lookup("<init>", "()V");
    m_onNew          = lookup("onNew",          "(JJJIII)I");
    m_onCancel       = lookup("onCancel",       "(J)I");
    m_lastCancelSide = lookup("lastCancelSide", "()I");
    m_onModify       = lookup("onModify",       "(JJJII)I");
    m_setBuf         = lookup("setTradeBuffer", "(Ljava/nio/ByteBuffer;)V");
    m_bestBid        = lookup("bestBid",        "()J");
    m_bestAsk        = lookup("bestAsk",        "()J");
    m_depthAt        = lookup("depthAt",        "(JI)J");
    m_setBatchIn     = lookup("setBatchIn",     "(Ljava/nio/ByteBuffer;)V");
    m_setBatchOut    = lookup("setBatchOut",    "(Ljava/nio/ByteBuffer;)V");
    m_onBatch        = lookup("onBatch",        "(II)J");
    jmethodID m_warmup = lookup("warmup", "()V");

    jobject obj = g_env->NewObject(cls, ctor);
    check_exception("HarnessJiang.<init>");
    if (!obj) fatal("NewObject(HarnessJiang) failed");
    g_engine = g_env->NewGlobalRef(obj);

    /* Hand the Java helper the adapter-owned staging buffer (wrapped once). */
    jobject bb = g_env->NewDirectByteBuffer(g_stage, sizeof(g_stage));
    if (!bb) fatal("NewDirectByteBuffer failed");
    g_byteBuf = g_env->NewGlobalRef(bb);
    g_env->CallVoidMethod(g_engine, m_setBuf, g_byteBuf);
    check_exception("setTradeBuffer");

    /* Hand over the batch-path report buffer (wrapped once). Pre-touched here so
     * its pages never minor-fault inside the timed window. */
    std::memset(g_outbuf, 0, sizeof(g_outbuf));
    jobject obb = g_env->NewDirectByteBuffer(g_outbuf, sizeof(g_outbuf));
    if (!obb) fatal("NewDirectByteBuffer(outbuf) failed");
    g_outBuf = g_env->NewGlobalRef(obb);
    g_env->CallVoidMethod(g_engine, m_setBatchOut, g_outBuf);
    check_exception("setBatchOut");

    /* Warm the hot path now, while the harness is not timing. Warms BOTH the
     * per-message handlers and the batch loop, so neither arm of the A/B pays
     * JIT-compilation cost inside the measured window. */
    g_env->CallVoidMethod(g_engine, m_warmup);
    check_exception("HarnessJiang.warmup");
}

void engine_shutdown(void) {
    /* Intentionally no DestroyJavaVM: the process exits immediately after, and
     * tearing the JVM down here would race the harness's dlclose() of this
     * library. The OS reclaims the JVM at process exit. */
}

/* The engine matches synchronously on the calling thread — nothing pending. */
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
    /* onCancel answers from the engine's own id index (getOrder): returns 1 if
     * the order was resting and is now cancelled (its price in g_stage[0].price,
     * its side bit via lastCancelSide()), or 0 when it is not resting. */
    jint resting = g_env->CallIntMethod(g_engine, m_onCancel,
                                        static_cast<jlong>(c->order_id));
    check_exception("onCancel");
    if (resting) {
        jint side = g_env->CallIntMethod(g_engine, m_lastCancelSide);
        check_exception("lastCancelSide");
        emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id,
                 static_cast<uint8_t>(side), g_stage[0].price, 0);
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

/* ---------------------------------------------------------------------------
 * OPTIONAL batch delivery (api/matching_engine_api.h: engine_on_batch).
 *
 * The per-message path above crosses into the JVM once per message (twice for a
 * cancel that hits: onCancel + lastCancelSide). This path crosses ONCE for the
 * whole run: the me_msg_t array is wrapped in a direct ByteBuffer, and
 * HarnessJiang.onBatch loops over it ON THE JAVA SIDE, calling exactly the same
 * per-message handlers (onNew / onCancel / onModify) in array order, with no
 * lookahead.
 *
 * Outbound is batched the same way. Java writes the FULL me_report_t stream —
 * the acks this file's emit_ack() would have written, interleaved with the
 * trades in the same order — straight into g_outbuf (a shared direct
 * ByteBuffer, not a JNI upcall), and this side drains it into the transport
 * once per crossing. So an N-message run costs ~3 JNI crossings instead of ~N.
 *
 * onBatch sub-chunks against OUT_CAP and returns (messagesConsumed << 32) |
 * reportsWritten, so a run whose reports would overflow g_outbuf is drained and
 * resumed rather than truncated.
 * ---------------------------------------------------------------------------*/
void engine_on_batch(const me_msg_t* msgs, uint32_t n) {
    jobject inbb = g_env->NewDirectByteBuffer(const_cast<me_msg_t*>(msgs),
                                              static_cast<jlong>(n) * sizeof(me_msg_t));
    if (!inbb) fatal("NewDirectByteBuffer(batch) failed");
    g_env->CallVoidMethod(g_engine, m_setBatchIn, inbb);
    check_exception("setBatchIn");
    uint32_t start = 0;
    while (start < n) {
        jlong packed = g_env->CallLongMethod(g_engine, m_onBatch,
                           static_cast<jint>(start), static_cast<jint>(n));
        check_exception("onBatch");
        uint32_t consumed = static_cast<uint32_t>(static_cast<uint64_t>(packed) >> 32);
        uint32_t written  = static_cast<uint32_t>(static_cast<uint64_t>(packed) & 0xffffffffu);
        for (uint32_t k = 0; k < written; ++k) push_report(g_outbuf[k]);
        if (consumed <= start) fatal("onBatch made no forward progress");
        start = consumed;
    }
    g_env->DeleteLocalRef(inbb);
}

}  // extern "C"
