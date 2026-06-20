/*
 * harness.cpp — the matching-engine benchmark runner.
 *
 * Loads an engine shared library, replays a deterministic workload through it
 * one message at a time while the engine emits its own report stream over an
 * inter-thread transport drained on an adjacent core, and verifies the full
 * report output against the published SHA-256 hash.
 *
 * Two run modes (--mode):
 *   perf   — the measured run. Times the workload (engine_flush() included) and
 *            reports throughput; verifies the output hash. The challenge takes
 *            the median of several perf runs.
 *   audit  — the anti-cheat run. Not timed; replays the workload through a
 *            trusted baseline engine and compares order-book state at random
 *            probe points (see docs/ANTI_CHEAT.md).
 * Both modes probe engine_query_* at the same random points, so an engine
 * cannot tell a measured run from an audited one.
 *
 *   ./harness --baseline liquibook --scenario normal
 *   ./harness --engine ./my_engine.so --scenario normal --mode audit
 *   ./harness --baseline quantcup --scenario normal --write-reference
 */
#include "harness.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef __aarch64__
#define cpu_pause() asm volatile("yield" ::: "memory")
#else
#include <immintrin.h>
#define cpu_pause() _mm_pause()
#endif

namespace {

constexpr uint64_t WORKLOAD_MAGIC   = 0x4D4542575F303031ULL; // "MEBW_001"
constexpr uint32_t WORKLOAD_VERSION = 1;                    // bumped on format change
constexpr uint32_t WORKLOAD_MAX     = 100u * 1000u * 1000u; // sanity cap, well above any
                                                            //   plausible workload size
constexpr uint32_t QUEUE_CAPACITY = 1u << 20;               // reports in flight
constexpr uint32_t DRAIN_BATCH    = 256;
constexpr unsigned RUN_TIMEOUT_S  = 600;                    // watchdog
constexpr int      AUDIT_POINTS   = 64;                     // state-audit probes

enum class Mode { PERF, AUDIT };

/* ---- engine shared-library binding --------------------------------------- */
template <typename Fn>
bool bind(void* h, const char* sym, Fn& out, bool required = true) {
    out = reinterpret_cast<Fn>(dlsym(h, sym));
    if (!out && required) {
        std::fprintf(stderr, "ERROR: engine missing required symbol '%s'\n", sym);
        return false;
    }
    return true;
}

bool load_engine(const std::string& path, EngineLib& e) {
    e.handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!e.handle) {
        std::fprintf(stderr, "ERROR: dlopen(%s): %s\n", path.c_str(), dlerror());
        return false;
    }
    bool ok = bind(e.handle, "engine_init",           e.init)
           && bind(e.handle, "engine_shutdown",       e.shutdown)
           && bind(e.handle, "engine_on_new_order",   e.on_new_order)
           && bind(e.handle, "engine_on_cancel",      e.on_cancel)
           && bind(e.handle, "engine_on_modify",      e.on_modify)
           && bind(e.handle, "engine_flush",          e.flush)
           && bind(e.handle, "engine_query_best_bid", e.query_best_bid)
           && bind(e.handle, "engine_query_best_ask", e.query_best_ask)
           && bind(e.handle, "engine_query_depth_at", e.query_depth_at);
    bind(e.handle, "engine_get_transport", e.get_transport, /*required=*/false);
    bind(e.handle, "engine_prebuild",      e.prebuild,      /*required=*/false);
    bind(e.handle, "engine_on_batch",      e.on_batch,      /*required=*/false);
    // Debug/measurement knob: ME_NO_BATCH forces per-message delivery even for
    // an engine that exports engine_on_batch (for before/after comparisons).
    if (std::getenv("ME_NO_BATCH")) e.on_batch = nullptr;
    if (!ok) {
        dlclose(e.handle);
        e.handle = nullptr;
    }
    return ok;
}

/* ---- thread pinning ------------------------------------------------------ */
/* Returns true on success or if the caller opted out (core < 0). On failure
 * the run is correct but the throughput is meaningless; callers must propagate
 * the false return through to the VALID gate so an unpinned run is INVALID. */
bool pin_to_core(int core) {
    if (core < 0) return true;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        std::fprintf(stderr,
            "ERROR: could not pin thread to core %d — affinity is required for "
            "valid benchmark numbers. The run will be marked INVALID.\n", core);
        return false;
    }
    return true;
}

/* ---- workload file ------------------------------------------------------- */
struct RawRecord {            // the 40-byte on-disk record (naturally packed)
    uint8_t  type, side, ioc, pad;
    uint32_t qty;
    uint64_t seq;
    uint64_t order_id;
    int64_t  price_ticks;
    int64_t  reserved;
};
static_assert(sizeof(RawRecord) == 40, "RawRecord must be 40 bytes");

/* A workload message with its engine ABI struct pre-built. Converting the
 * harness's WorkloadMsg into a new_order_t / cancel_t / modify_t is harness
 * marshaling, not matcher work, so it is done before the timed window — the
 * timed loop dispatches pre-built PreparedMsg values, never constructs them. */
struct PreparedMsg {
    uint8_t type;                 /* 0 = NEW, 1 = CANCEL, 2 = MODIFY */
    union {
        new_order_t no;
        cancel_t    c;
        modify_t    md;
    };
};
/* engine_on_batch receives a const me_msg_t*; PreparedMsg is the same tagged
 * layout (type at 0, union at 8), so the prepared array is handed over by
 * reinterpret_cast with no per-message copy. Keep the layouts identical. */
static_assert(sizeof(PreparedMsg) == sizeof(me_msg_t),
              "PreparedMsg must match the me_msg_t ABI layout");

bool file_exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

/* ---- the drainer thread -------------------------------------------------- */
struct DrainState {
    std::atomic<bool> running{true};
    std::atomic<bool> pin_failed{false};
    const me_transport_t* transport = nullptr;
    void* queue = nullptr;
    int   core  = -1;
    // Per-type tallies for the run summary.
    uint64_t order_acks = 0, trades = 0, cancel_acks = 0, modify_acks = 0;
    uint64_t cancel_rejects = 0, modify_rejects = 0;
    std::vector<CollectedReport> collected;  // every report, for the canonical hash
};

void drainer_main(DrainState* s) {
    // If the drainer can't pin, keep draining anyway so the engine doesn't
    // deadlock on a full queue — the run will complete but be marked INVALID.
    if (!pin_to_core(s->core))
        s->pin_failed.store(true, std::memory_order_release);
    me_report_t batch[DRAIN_BATCH];
    // The canonical hash now covers the whole output stream, so every report is
    // collected — in both perf and audit mode — and counted by type. The
    // drainer runs on its own core with ample headroom for this.
    auto consume = [&](uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) {
            const me_report_t& r = batch[i];
            s->collected.push_back({ r.type, r.sequence_number, r.side,
                                     r.order_id, r.price_ticks, r.quantity,
                                     r.maker_order_id, r.taker_order_id });
            switch (r.type) {
                case ME_ORDER_ACK:     ++s->order_acks;     break;
                case ME_TRADE:         ++s->trades;         break;
                case ME_CANCEL_ACK:    ++s->cancel_acks;    break;
                case ME_MODIFY_ACK:    ++s->modify_acks;    break;
                case ME_CANCEL_REJECT: ++s->cancel_rejects; break;
                case ME_MODIFY_REJECT: ++s->modify_rejects; break;
            }
        }
    };
    while (s->running.load(std::memory_order_acquire)) {
        uint32_t n = s->transport->drain(s->queue, batch, DRAIN_BATCH);
        if (n == 0) cpu_pause();
        else consume(n);
    }
    for (uint32_t n; (n = s->transport->drain(s->queue, batch, DRAIN_BATCH)) > 0; )
        consume(n);
}

/* ---- defensive guards ---------------------------------------------------- */
[[noreturn]] void on_fatal_signal(int sig) {
    const char* msg = (sig == SIGALRM)
        ? "\nERROR: engine run exceeded the time limit — reporting as failed.\n"
        : "\nERROR: engine crashed (fatal signal) — reporting as failed.\n";
    ssize_t w = write(STDERR_FILENO, msg, std::strlen(msg));
    (void)w;
    _exit(3);
}

void install_guards() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_fatal_signal;
    for (int sig : { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGALRM }) {
        if (sigaction(sig, &sa, nullptr) != 0) {
            std::fprintf(stderr,
                "ERROR: sigaction(%d) failed: %s — fatal-signal reporting will "
                "not function; aborting.\n", sig, std::strerror(errno));
            std::exit(1);
        }
    }
}

}  // namespace

/* ---- load_workload (declared in harness.h) ------------------------------- */
bool load_workload(const std::string& path, std::vector<WorkloadMsg>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::perror(("fopen " + path).c_str()); return false; }

    uint64_t magic = 0; uint32_t version = 0, count = 0;
    if (std::fread(&magic, 8, 1, f) != 1 || std::fread(&version, 4, 1, f) != 1 ||
        std::fread(&count, 4, 1, f) != 1 || magic != WORKLOAD_MAGIC) {
        std::fprintf(stderr, "ERROR: %s is not a valid workload file\n", path.c_str());
        std::fclose(f);
        return false;
    }
    if (version != WORKLOAD_VERSION) {
        std::fprintf(stderr,
            "ERROR: %s has workload version %u; this harness expects version %u. "
            "Re-generate the workload with the matching ./generator.\n",
            path.c_str(), version, WORKLOAD_VERSION);
        std::fclose(f);
        return false;
    }
    if (count == 0 || count > WORKLOAD_MAX) {
        std::fprintf(stderr,
            "ERROR: %s declares %u records; valid range is [1, %u]. "
            "File is corrupt or was produced by an incompatible generator.\n",
            path.c_str(), count, WORKLOAD_MAX);
        std::fclose(f);
        return false;
    }
    std::vector<RawRecord> raw(count);
    bool ok = std::fread(raw.data(), sizeof(RawRecord), count, f) == count;
    std::fclose(f);
    if (!ok) { std::fprintf(stderr, "ERROR: short read from %s\n", path.c_str()); return false; }

    out.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const RawRecord& r = raw[i];
        if (r.type > 2) {
            std::fprintf(stderr,
                "ERROR: %s record %u has invalid type %u (expected 0=NEW, "
                "1=CANCEL, 2=MODIFY). File is corrupt.\n",
                path.c_str(), i, r.type);
            return false;
        }
        out[i] = { r.type, r.side, r.ioc, r.qty, r.seq, r.order_id, r.price_ticks };
    }
    return true;
}

/* ========================================================================= */
int main(int argc, char** argv) {
    std::string engine_arg, baseline, scenario = "normal";
    uint64_t seed = 23, count = 1000000;   // 23 = the canonical seed (docs/METHODOLOGY.md)
    int matcher_core = 2, drainer_core = 3;
    bool write_reference = false;
    Mode mode = Mode::PERF;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : "";
        };
        if (a == "--engine")            engine_arg = next();
        else if (a == "--baseline")     baseline = next();
        else if (a == "--scenario")     scenario = next();
        else if (a == "--seed")         seed = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--count")        count = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--matcher-core") matcher_core = std::atoi(next().c_str());
        else if (a == "--drainer-core") drainer_core = std::atoi(next().c_str());
        else if (a == "--write-reference") write_reference = true;
        else if (a == "--mode") {
            std::string m = next();
            if (m == "perf")       mode = Mode::PERF;
            else if (m == "audit") mode = Mode::AUDIT;
            else {
                std::fprintf(stderr, "unknown --mode '%s' (use perf|audit)\n", m.c_str());
                return 2;
            }
        }
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return 2; }
    }
    if (engine_arg.empty() && baseline.empty()) {
        std::fprintf(stderr,
            "usage: %s (--engine <path.so> | --baseline <name>) "
            "[--scenario s] [--mode perf|audit] [--seed n] [--count n] "
            "[--write-reference]\n", argv[0]);
        return 2;
    }
    if (write_reference && baseline.empty()) {
        std::fprintf(stderr,
            "--write-reference refuses to run with --engine: the reference "
            "hash and canonical_output.txt are the byte-identical "
            "three-baseline consensus, not the output of a single engine "
            "under test. Re-run with --baseline liquibook (or quantcup, "
            "exchange_core) to regenerate.\n");
        return 2;
    }
    const char* mode_name = (mode == Mode::PERF) ? "perf" : "audit";

    std::string engine_path = !engine_arg.empty()
        ? engine_arg : ("./" + baseline + "_adapter.so");
    std::string engine_name;
    {
        std::string b = !baseline.empty() ? baseline : engine_arg;
        size_t s = b.find_last_of('/');
        engine_name = (s == std::string::npos) ? b : b.substr(s + 1);
        for (const char* suf : { ".so", "_adapter" }) {
            size_t p = engine_name.rfind(suf);
            if (p != std::string::npos && p + std::strlen(suf) == engine_name.size())
                engine_name.erase(p);
        }
    }

    // --- validate the scenario name before it reaches a path or the shell ---
    // scenario is interpolated into a generated filename and the generator
    // command below; restrict it to a safe set so a crafted --scenario cannot
    // inject a path component or a shell command.
    if (scenario.empty() || scenario.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
            != std::string::npos) {
        std::fprintf(stderr,
            "ERROR: invalid --scenario '%s' (allowed: letters, digits, '_', '-')\n",
            scenario.c_str());
        return 1;
    }

    // --- ensure the workload exists -----------------------------------------
    // Key the workload file on scenario + seed + count so a held-out / private
    // seed never silently replays a cached canonical workload — the file's
    // identity must encode everything that determines its bytes. (scenario is
    // charset-validated above; seed/count are numeric, so this is shell-safe.)
    std::string workload_path = "orders_" + scenario + "_s" + std::to_string(seed) +
                                "_n" + std::to_string(count) + ".bin";
    if (!file_exists(workload_path)) {
        std::string cmd = "./generator " + scenario + " " + workload_path + " " +
                          std::to_string(count) + " " + std::to_string(seed);
        std::fprintf(stderr, "Generating workload: %s\n", cmd.c_str());
        if (std::system(cmd.c_str()) != 0) {
            std::fprintf(stderr, "ERROR: workload generation failed\n");
            return 1;
        }
    }
    std::vector<WorkloadMsg> workload;
    if (!load_workload(workload_path, workload)) return 1;

    // --- load the engine ----------------------------------------------------
    std::printf("Loading engine: %s\n", engine_path.c_str());
    EngineLib eng;
    if (!load_engine(engine_path, eng)) return 1;

    // --- transport + drainer ------------------------------------------------
    // The engine emits its own reports; the harness only drains them. An engine
    // may supply its own transport, otherwise the harness default is used.
    const me_transport_t* transport = (eng.get_transport && eng.get_transport())
        ? eng.get_transport() : harness_default_transport();
    void* queue = transport->create(QUEUE_CAPACITY);
    if (!queue) { std::fprintf(stderr, "ERROR: transport creation failed\n"); return 1; }

    DrainState drain;
    drain.transport = transport;
    drain.queue = queue;
    drain.core = drainer_core;
    // The canonical hash covers every report, so the drainer collects them all;
    // reserve generously so it does not reallocate inside the timed window. This
    // is a heuristic, not a bound (a sweep-heavy custom workload emits 1 ack + K
    // trades per crossing order, which can exceed 2 reports/message), so we
    // capture the reserved capacity and verify it held after the run — a
    // mid-window reallocation perturbs the throughput and gates the run INVALID.
    drain.collected.reserve(workload.size() * 2 + 1024);
    const size_t collected_cap0 = drain.collected.capacity();

    // Install the fatal-signal guards BEFORE spawning the drainer or running
    // any engine-supplied code (the drainer immediately touches the engine's
    // transport), so a crash in either is reported as a failed run rather than
    // killing the harness through the unguarded gap.
    install_guards();
    std::thread drainer(drainer_main, &drain);

    // --- run ----------------------------------------------------------------
    bool matcher_pinned = pin_to_core(matcher_core);
    eng.init(seed, transport, queue);
    int threads_after_init = current_thread_count();

    // Pre-build the engine ABI struct for every message — before the timed
    // window. This marshaling (harness WorkloadMsg -> the ABI structs) is not
    // matcher work; the timed loop below only dispatches the pre-built structs.
    std::vector<PreparedMsg> prepared(workload.size());
    for (size_t i = 0; i < workload.size(); ++i) {
        const WorkloadMsg& m = workload[i];
        PreparedMsg& p = prepared[i];
        p.type = m.type;
        if (m.type == 0) {                              // NEW
            p.no = new_order_t{};
            p.no.order_id    = m.order_id;    p.no.sequence_number = m.seq;
            p.no.price_ticks = m.price_ticks; p.no.quantity        = m.qty;
            p.no.side        = m.side;        p.no.ioc             = m.ioc;
        } else if (m.type == 1) {                       // CANCEL
            p.c = cancel_t{};
            p.c.order_id = m.order_id;        p.c.sequence_number  = m.seq;
        } else {                                        // MODIFY
            p.md = modify_t{};
            p.md.order_id        = m.order_id;    p.md.sequence_number = m.seq;
            p.md.new_price_ticks = m.price_ticks; p.md.new_quantity    = m.qty;
            p.md.side            = m.side;
        }
    }

    // Optional engine pre-build: let the engine marshal each message into its
    // native form before the timed window, mirroring the ABI-struct pre-build
    // above — so the timed loop measures matching work alone.
    // Time the prebuild pass: an honest translation is a small fraction of the
    // timed matching run, so T_prebuild / T_timed << 1. An engine that hides
    // matcher work here (pre-matching into a private shadow, then replaying in
    // the timed call — invisible to the book-empty assert) front-loads the cost
    // and the ratio climbs past 1. See the prebuild-time bound below.
    int64_t prebuild_ns = 0;
    if (eng.prebuild) {
        auto pb0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < workload.size(); ++i) {
            const PreparedMsg& p = prepared[i];
            const void* m = (p.type == 0) ? static_cast<const void*>(&p.no)
                          : (p.type == 1) ? static_cast<const void*>(&p.c)
                                          : static_cast<const void*>(&p.md);
            eng.prebuild(p.type, m);
        }
        prebuild_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - pb0).count();
    }

    // Anti-cheat pre-flight: no message has been dispatched yet, so the book
    // MUST be empty. engine_prebuild is contractually translation-only (marshal
    // each ABI struct into the engine's native order representation); the book
    // may come alive only in the timed engine_on_* calls below. An engine that
    // inserted resting orders during engine_prebuild (or engine_init) — i.e.
    // pre-matching ahead of the clock to flatter its timed number — leaves a
    // non-empty book here and is gated INVALID. The query doubles as a
    // synchronization point for an engine that matches asynchronously.
    const int64_t pre_bid = eng.query_best_bid();
    const int64_t pre_ask = eng.query_best_ask();
    const bool prestart_book_empty =
        (pre_bid == INT64_MIN && pre_ask == INT64_MAX);

    // Dispatch one pre-built message into the engine — the timed hot path.
    auto dispatch = [&](const PreparedMsg& p) {
        if (p.type == 0)      eng.on_new_order(&p.no);
        else if (p.type == 1) eng.on_cancel(&p.c);
        else                  eng.on_modify(&p.md);
    };

    // Anti-cheat: probe the order book at unpredictable points. EVERY run
    // probes — perf runs discard the answers, the audit run records and
    // compares them — so an engine cannot tell a measured run from an audited
    // one and must therefore maintain a real book on every run.
    std::random_device rd;
    uint64_t verify_seed =
          (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd())
        ^ (static_cast<uint64_t>(getpid()) << 48)
        ^ static_cast<uint64_t>(
              std::chrono::steady_clock::now().time_since_epoch().count());
    std::vector<size_t> audit_idx =
        audit_pick_indices(verify_seed, workload.size(), AUDIT_POINTS);
    std::vector<QuerySnapshot> ut_snaps;
    ut_snaps.reserve(audit_idx.size());
    size_t  audit_cur   = 0;
    int64_t excluded_ns = 0;        // time spent in audit probes, not throughput

    // A book-state probe taken after workload message `i` — timed separately and
    // excluded from throughput. Identical in perf and audit mode; only whether
    // the answer is recorded differs. Shared by both dispatch paths below.
    auto probe = [&](size_t i) {
        auto p0 = std::chrono::steady_clock::now();
        const WorkloadMsg& m = workload[i];
        [[maybe_unused]] int64_t  bid = eng.query_best_bid();
        [[maybe_unused]] int64_t  ask = eng.query_best_ask();
        [[maybe_unused]] uint64_t dep = eng.query_depth_at(m.price_ticks, m.side);
        if (mode == Mode::AUDIT)
            ut_snaps.push_back({ i, bid, ask, m.price_ticks, m.side, dep });
        excluded_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - p0).count();
    };

    std::printf("Running benchmark (%zu messages, scenario=%s, mode=%s)...\n",
                workload.size(), scenario.c_str(), mode_name);
    alarm(RUN_TIMEOUT_S);
    auto t0 = std::chrono::steady_clock::now();

    if (eng.on_batch) {
        // Audit-aligned batch delivery: end each batch exactly at the next probe
        // index, probe, then continue — so the book is inspected at the same
        // unpredictable per-message points as one-at-a-time delivery, while each
        // ABI crossing carries a run of messages. The probe call sequence is
        // identical in perf and audit mode, so the two remain indistinguishable.
        // (api/matching_engine_api.h: engine_on_batch.)
        const me_msg_t* base = reinterpret_cast<const me_msg_t*>(prepared.data());
        size_t start = 0;
        for (size_t k = 0; k < audit_idx.size(); ++k) {
            size_t b = audit_idx[k];                       // boundary = a probe index
            eng.on_batch(base + start, static_cast<uint32_t>(b - start + 1));
            probe(b);
            start = b + 1;
        }
        if (start < workload.size())                       // tail after the last probe
            eng.on_batch(base + start, static_cast<uint32_t>(workload.size() - start));
    } else {
        for (size_t i = 0; i < workload.size(); ++i) {
            dispatch(prepared[i]);
            // Probe happens identically in perf and audit mode; only what the
            // harness does with the answer differs.
            if (audit_cur < audit_idx.size() && i == audit_idx[audit_cur]) {
                probe(i);
                ++audit_cur;
            }
        }
    }
    eng.flush();               // pipeline barrier — any deferred work is counted here
    transport->flush(queue);   // publish any reports a batching transport buffered

    auto t1 = std::chrono::steady_clock::now();
    alarm(0);
    int threads_after_run = current_thread_count();

    drain.running.store(false, std::memory_order_release);
    drainer.join();
    eng.shutdown();

    double elapsed = std::chrono::duration<double>(t1 - t0).count()
                   - static_cast<double>(excluded_ns) / 1e9;
    double mps = elapsed > 0 ? workload.size() / elapsed / 1e6 : 0.0;

    // Prebuild-time bound (engines exporting engine_prebuild only). Translation
    // is a small fraction of matching, so an honest prebuild runs well under the
    // timed window: T_prebuild / T_timed << 1. A prebuild that rivals the run
    // has hidden matcher work there (e.g. shadow pre-matching, which the
    // book-empty assert cannot see). A loud flag fires above WARN; the INVALID
    // gate trips only when egregious (FAIL) — a level no honest engine reaches,
    // since it would mean translating slower than matching. docs/ANTI_CHEAT.md.
    // Generous: an honest prebuild also pays one-time buffer allocation /
    // page-faulting, which on a fast engine (cheap matching) can approach the
    // timed cost. A shadow pre-matcher front-loads the whole match and lands
    // several-fold higher, so the gap is wide.
    constexpr double PREBUILD_WARN_RATIO = 2.0;   // loud flag
    constexpr double PREBUILD_FAIL_RATIO = 4.0;   // INVALID gate (egregious)
    const double prebuild_s     = static_cast<double>(prebuild_ns) / 1e9;
    const double prebuild_ratio = (eng.prebuild && elapsed > 0.0)
                                      ? prebuild_s / elapsed : 0.0;
    const bool prebuild_time_ok = (prebuild_ratio <= PREBUILD_FAIL_RATIO);

    // --- correctness --------------------------------------------------------
    std::string computed = compute_canonical_hash(drain.collected);
    std::string expected, status;
    const std::string hash_path = "reference/correctness_hash.txt";
    const std::string want_tag =
        scenario + "_seed" + std::to_string(seed) + "_reports.txt";

    // The hash file is one `<sha256>  <tag>` line per (scenario, seed). Each
    // call reads the existing entries into a map so the file accumulates
    // across scenarios — and a single line is still a valid file.
    auto read_hash_file = [&hash_path]() {
        std::map<std::string, std::string> out;
        FILE* fp = std::fopen(hash_path.c_str(), "rb");
        if (!fp) return out;
        // Read line by line (fgets, not fscanf) so an over-long or malformed
        // token cannot desync the stream and hide later valid entries, and
        // accept an entry only when the hash is exactly 64 lowercase hex chars.
        char line[512];
        while (std::fgets(line, sizeof(line), fp)) {
            char h[256], t[256];
            if (std::sscanf(line, "%255s %255s", h, t) != 2) continue;
            std::string hash(h);
            if (hash.size() != 64 ||
                hash.find_first_not_of("0123456789abcdef") != std::string::npos)
                continue;
            out[t] = hash;
        }
        std::fclose(fp);
        return out;
    };

    if (write_reference) {
        mkdir("reference", 0755);
        // canonical_output.txt is the canonical published dump. Don't clobber
        // it on a non-canonical write; capture a per-scenario copy instead.
        const bool is_canonical = (scenario == "normal" && seed == 23ULL);
        const std::string out_path = is_canonical
            ? std::string("reference/canonical_output.txt")
            : std::string("reference/canonical_output_") + scenario + ".txt";
        write_canonical_output(drain.collected, out_path.c_str());

        // Merge the new entry into the existing hash file. Sorted on rewrite.
        // Write to a temp file then atomically rename — a crash between the
        // truncate and the close would otherwise leave the shipped reference
        // empty.
        auto entries = read_hash_file();
        entries[want_tag] = computed;
        const std::string tmp_path = hash_path + ".tmp";
        FILE* hfp = std::fopen(tmp_path.c_str(), "wb");
        if (hfp) {
            for (const auto& [tag, hash] : entries) {
                std::fprintf(hfp, "%s  %s\n", hash.c_str(), tag.c_str());
            }
            bool flushed = (std::fflush(hfp) == 0);
            std::fclose(hfp);
            if (flushed && std::rename(tmp_path.c_str(), hash_path.c_str()) != 0) {
                std::fprintf(stderr,
                    "ERROR: could not rename %s -> %s: %s\n",
                    tmp_path.c_str(), hash_path.c_str(), std::strerror(errno));
                std::remove(tmp_path.c_str());
            } else if (!flushed) {
                std::fprintf(stderr,
                    "ERROR: could not flush %s: %s\n",
                    tmp_path.c_str(), std::strerror(errno));
                std::remove(tmp_path.c_str());
            }
        } else {
            std::fprintf(stderr, "ERROR: could not open %s: %s\n",
                         tmp_path.c_str(), std::strerror(errno));
        }
        status = "REFERENCE WRITTEN";
    } else if (file_exists(hash_path.c_str())) {
        auto entries = read_hash_file();
        auto it = entries.find(want_tag);
        if (it == entries.end()) {
            status = "NO REFERENCE";
        } else {
            expected = it->second;
            status = (expected == computed) ? "PASS" : "FAIL";
        }
    } else {
        status = "NO REFERENCE";
    }

    // --- anti-cheat: random-point order-book state audit (audit mode only) --
    AuditReport audit;
    if (mode == Mode::AUDIT) {
        std::string base_name = (engine_name == "liquibook") ? "quantcup" : "liquibook";
        std::string base_path = "./" + base_name + "_adapter.so";
        audit.baseline = base_name;
        if (!file_exists(base_path)) {
            audit.note = "baseline '" + base_name +
                         "' not built — run scripts/build_baselines.sh";
        } else {
            EngineLib base;
            if (!load_engine(base_path, base)) {
                audit.note = "baseline '" + base_name + "' failed to load";
            } else {
                alarm(120);
                audit = run_state_audit(base, workload, ut_snaps);
                alarm(0);
                audit.baseline    = base_name;
                audit.verify_seed = verify_seed;
                dlclose(base.handle);
            }
        }
    }

    // --- verdict ------------------------------------------------------------
    // An engine may use threads freely; the thread count is reported but never
    // gates the verdict. A perf run is valid on correctness alone; an audit run
    // additionally requires the state audit to pass. VALID requires an
    // affirmative PASS: NO REFERENCE (e.g. on a custom seed/scenario), audit
    // SKIPPED, or REFERENCE WRITTEN all map to INVALID — the harness will not
    // mark a run VALID when no validation actually happened. Pinning failure
    // also forces INVALID: an unpinned matcher or drainer makes the throughput
    // number meaningless.
    int engine_threads = (threads_after_run > threads_after_init)
        ? threads_after_run - threads_after_init : 0;
    bool drainer_pinned = !drain.pin_failed.load(std::memory_order_acquire);
    bool affinity_ok    = matcher_pinned && drainer_pinned;
    bool correctness_ok = (status == "PASS");
    bool audit_ok       = (mode == Mode::PERF) || (audit.ran && audit.passed);
    // The collected-report vector must not have grown past its reservation
    // during the timed window (capacity only changes on reallocation).
    bool report_buffer_ok = (drain.collected.capacity() == collected_cap0);
    bool valid          = correctness_ok && audit_ok && affinity_ok
                          && prestart_book_empty && prebuild_time_ok
                          && report_buffer_ok;

    // --- report -------------------------------------------------------------
    std::printf("  Messages processed: %zu\n", workload.size());
    std::printf("  Trades emitted:     %llu\n",
                (unsigned long long)drain.trades);
    if (mode == Mode::AUDIT)
        std::printf("  Reports:            order_ack %llu  trade %llu  "
                    "cancel_ack %llu  modify_ack %llu  "
                    "cancel_reject %llu  modify_reject %llu\n",
                    (unsigned long long)drain.order_acks,
                    (unsigned long long)drain.trades,
                    (unsigned long long)drain.cancel_acks,
                    (unsigned long long)drain.modify_acks,
                    (unsigned long long)drain.cancel_rejects,
                    (unsigned long long)drain.modify_rejects);
    if (mode == Mode::PERF) {
        std::printf("  Wall time:          %.4f s\n", elapsed);
        std::printf("  Throughput:         %.2f M msgs/s\n", mps);
    }
    std::printf("  Engine threads:     after_init %d, after_run %d (+%d) — informational\n",
                threads_after_init, threads_after_run, engine_threads);
    if (!affinity_ok)
        std::printf("  Affinity:           matcher=%s, drainer=%s — gating the run INVALID\n",
                    matcher_pinned  ? "OK" : "FAILED",
                    drainer_pinned ? "OK" : "FAILED");
    if (!report_buffer_ok)
        std::printf("  Measurement:        report buffer reallocated inside the timed "
                    "window (%zu reports > %zu reserved) — throughput perturbed; gating "
                    "the run INVALID\n",
                    drain.collected.size(), collected_cap0);
    if (!prestart_book_empty)
        std::printf("  Anti-cheat:         pre-start book not empty by the API sentinels "
                    "(best_bid=%lld, best_ask=%lld; expected INT64_MIN / INT64_MAX) — "
                    "either engine_prebuild rested orders ahead of the timed window "
                    "(see docs/ANTI_CHEAT.md), or the engine returns a non-standard "
                    "empty-book sentinel (see api/matching_engine_api.h). Gating the run "
                    "INVALID\n",
                    (long long)pre_bid, (long long)pre_ask);
    if (eng.prebuild) {
        const double pb_ns_msg = prebuild_s / double(workload.size()) * 1e9;
        const double tm_ns_msg = elapsed     / double(workload.size()) * 1e9;
        std::printf("  Pre-build cost:     %.1f ns/msg (%.0f%% of the timed run) "
                    "— informational\n", pb_ns_msg, prebuild_ratio * 100.0);
        if (prebuild_ratio > PREBUILD_WARN_RATIO)
            std::printf("  Anti-cheat:         pre-build ran %.2fx the timed window "
                        "(%.1f vs %.1f ns/msg) — engine_prebuild must translate, "
                        "not match (see docs/ANTI_CHEAT.md).%s\n",
                        prebuild_ratio, pb_ns_msg, tm_ns_msg,
                        prebuild_time_ok ? " [flag]"
                                         : " Gating the run INVALID.");
    }

    std::printf("\nCorrectness check:\n");
    if (!expected.empty()) std::printf("  Expected hash: %s\n", expected.c_str());
    std::printf("  Computed hash: %s\n", computed.c_str());
    std::printf("  Status: %s\n", status.c_str());

    if (mode == Mode::AUDIT) {
        std::printf("\nAnti-cheat state audit:\n");
        if (!audit.ran)
            std::printf("  State audit: SKIPPED — %s\n", audit.note.c_str());
        else if (audit.passed)
            std::printf("  State audit: PASS — %d state checks matched baseline '%s'\n",
                        audit.checks, audit.baseline.c_str());
        else
            std::printf("  State audit: FAIL — %d of %d state checks mismatched baseline '%s'\n",
                        audit.mismatches, audit.checks, audit.baseline.c_str());
    }

    std::printf("\nVerdict: %s\n", valid ? "VALID" : "INVALID");

    mkdir("results", 0755);
    char ts[32];
    std::time_t now = std::time(nullptr);
    std::tm tmv;
    std::strftime(ts, sizeof(ts), "%Y%m%dT%H%M%S", localtime_r(&now, &tmv));
    std::string result_path = "results/" + engine_name + "_" + scenario + "_" +
                              mode_name + "_" + ts + ".json";
    FILE* jf = std::fopen(result_path.c_str(), "wb");
    if (jf) {
        std::fprintf(jf,
            "{\n"
            "  \"engine\": \"%s\",\n"
            "  \"scenario\": \"%s\",\n"
            "  \"mode\": \"%s\",\n"
            "  \"seed\": %llu,\n"
            "  \"messages\": %zu,\n"
            "  \"wall_time_s\": %.6f,\n"
            "  \"throughput_msgs_per_s\": %.0f,\n"
            "  \"reports\": {\"order_ack\": %llu, \"trade\": %llu, "
            "\"cancel_ack\": %llu, \"modify_ack\": %llu, "
            "\"cancel_reject\": %llu, \"modify_reject\": %llu},\n"
            "  \"correctness\": {\"status\": \"%s\", \"expected_hash\": \"%s\", "
            "\"computed_hash\": \"%s\"},\n"
            "  \"threads\": {\"after_init\": %d, \"after_run\": %d, "
            "\"engine_spawned\": %d},\n"
            "  \"audit\": {\"ran\": %s, \"passed\": %s, \"checks\": %d, "
            "\"mismatches\": %d, \"baseline\": \"%s\", \"verify_seed\": %llu, "
            "\"note\": \"%s\"},\n"
            "  \"affinity\": {\"matcher\": %s, \"drainer\": %s},\n"
            "  \"verdict\": \"%s\",\n"
            "  \"platform\": %s\n"
            "}\n",
            engine_name.c_str(), scenario.c_str(), mode_name,
            (unsigned long long)seed, workload.size(), elapsed,
            mode == Mode::PERF ? mps * 1e6 : 0.0,
            (unsigned long long)drain.order_acks,
            (unsigned long long)drain.trades,
            (unsigned long long)drain.cancel_acks, (unsigned long long)drain.modify_acks,
            (unsigned long long)drain.cancel_rejects, (unsigned long long)drain.modify_rejects,
            status.c_str(), expected.c_str(), computed.c_str(),
            threads_after_init, threads_after_run, engine_threads,
            audit.ran ? "true" : "false", audit.passed ? "true" : "false",
            audit.checks, audit.mismatches, audit.baseline.c_str(),
            (unsigned long long)audit.verify_seed, audit.note.c_str(),
            matcher_pinned  ? "true" : "false",
            drainer_pinned  ? "true" : "false",
            valid ? "VALID" : "INVALID",
            platform_fingerprint_json().c_str());
        std::fclose(jf);
        std::printf("\nResult file: %s\n", result_path.c_str());
    }

    transport->destroy(queue);
    dlclose(eng.handle);
    return valid ? 0 : 1;
}
