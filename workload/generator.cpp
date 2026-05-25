/*
 * generator.cpp — deterministic order-flow workload generator.
 *
 * Published with "The World's Fastest Matching Engine Algorithm"
 * (Flash One Technologies, 2026). See docs/METHODOLOGY.md.
 *
 * The model is calibrated to a liquid US equity (NVDA): a geometric-Brownian-
 * motion mid-price, power-law order depth, and a realistic cancel/modify/IOC
 * lifecycle. The generator is fully deterministic given (scenario, count, seed).
 *
 * REPRODUCIBILITY NOTE. Only std::mt19937 is bit-portable across C++ standard
 * libraries; std::normal_distribution / std::discrete_distribution / etc. are
 * NOT. To keep the published correctness hash platform-independent, every
 * distribution below is implemented by hand on top of std::mt19937. The same
 * (scenario, count, seed) therefore yields a byte-identical workload on any
 * compiler/platform.
 *
 * Usage:  generator <scenario> <out.bin> [count] [seed]
 *         scenario in {static, normal, swing-25, swing-40, flash-crash}
 *         count defaults to 1000000, seed defaults to 12345
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <random>
#include <vector>
#include <algorithm>
#include <numeric>
#include <string>

namespace {

// --- Calibration constants (see docs/METHODOLOGY.md; mirror workload/scenarios.json)
constexpr double   TICK_SIZE       = 0.005;     // $ per tick (SEC sub-penny, Nov 2025)
constexpr double   START_MID       = 167.52;    // NVDA reference close
constexpr double   ALPHA           = 2.23;      // power-law depth exponent
constexpr size_t   MAX_LEVEL       = 799;       // deepest resting level
constexpr double   HUMP_AMP        = 5.0;       // depth "hump" amplitude
constexpr double   HUMP_CENTER     = 8.0;       // hump centred 8 ticks out
constexpr double   CANCEL_PCT      = 0.95;      // non-IOC orders later cancelled
constexpr double   MODIFY_PCT      = 0.20;      // non-IOC orders that get a modify
constexpr double   DUP_CANCEL_PCT  = 0.02;      // cancels re-sent as a duplicate cancel
constexpr double   DUP_MODIFY_PCT  = 0.02;      // modified+cancelled orders given a stale modify
constexpr double   IOC_PCT         = 0.15;      // new orders marked IOC
constexpr double   MEDIAN_LIFE_MS  = 0.431;     // non-IOC lifetime median (paper §6.1)
constexpr double   P_AGGRESSIVE    = 0.05;      // new orders priced to cross the spread
constexpr uint32_t AGGR_MAX_DEPTH  = 4;         // a marketable order crosses 1..4 ticks

constexpr uint64_t WORKLOAD_MAGIC   = 0x4D4542575F303031ULL;  // "MEBW_001"
constexpr uint32_t WORKLOAD_VERSION = 1;

// Workload message types (the on-disk record's `type` byte).
enum : uint8_t { MSG_NEW = 0, MSG_CANCEL = 1, MSG_MODIFY = 2 };

struct Scenario { const char* name; double volatility; double target_swing; };
constexpr Scenario SCENARIOS[] = {
    { "static",      0.00, 0.00 },
    { "normal",      0.15, 0.02 },
    { "swing-25",    0.50, 0.25 },
    { "swing-40",    0.50, 0.40 },
    { "flash-crash", 0.50, 0.60 },
};

// --- Portable RNG: every distribution hand-rolled on std::mt19937 ------------
struct Rng {
    std::mt19937 mt;
    // mt19937's seed type is uint32_t. The CLI accepts up to UINT32_MAX and
    // refuses larger values up front (see main()) — never silently truncate.
    explicit Rng(uint32_t seed) : mt(seed) {}

    // Uniform real in [0,1) using 53 random bits — portable.
    double uniform() {
        uint64_t hi = mt() >> 5;          // 27 bits
        uint64_t lo = mt() >> 6;          // 26 bits
        return (hi * 67108864.0 + lo) * (1.0 / 9007199254740992.0);
    }
    // Uniform integer in [lo, hi].
    uint32_t uniform_int(uint32_t lo, uint32_t hi) {
        return lo + static_cast<uint32_t>(uniform() * (hi - lo + 1));
    }
    // Standard normal via Box-Muller (cos branch; 2 uniforms per draw).
    double normal() {
        double u1 = uniform();
        double u2 = uniform();
        if (u1 < 1e-300) u1 = 1e-300;     // guard log(0)
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
    }
    // Exponential with the given rate (lambda).
    double exponential(double lambda) {
        double u = uniform();
        if (u < 1e-300) u = 1e-300;
        return -std::log(u) / lambda;
    }
};

// Power-law depth sampler with a near-touch hump: weight(k) =
// 1/(k+1)^alpha + HUMP_AMP * exp(-0.5*((k-HUMP_CENTER))^2). Sampled by
// binary search over the precomputed CDF — portable.
struct LevelSampler {
    std::vector<double> cdf;
    LevelSampler() {
        cdf.resize(MAX_LEVEL + 1);
        double acc = 0.0;
        for (size_t k = 0; k <= MAX_LEVEL; ++k) {
            double base = 1.0 / std::pow(k + 1.0, ALPHA);
            double z    = (static_cast<double>(k) - HUMP_CENTER);
            double bump = HUMP_AMP * std::exp(-0.5 * z * z);
            acc += base + bump;
            cdf[k] = acc;
        }
    }
    int sample(Rng& r) const {
        double x = r.uniform() * cdf.back();
        auto it = std::lower_bound(cdf.begin(), cdf.end(), x);
        int k = static_cast<int>(it - cdf.begin());
        return k > static_cast<int>(MAX_LEVEL) ? static_cast<int>(MAX_LEVEL) : k;
    }
};

struct Msg {
    int64_t  ts;            // synthetic timeline timestamp (microseconds)
    uint8_t  type;
    uint8_t  side;          // 0 buy, 1 sell
    uint8_t  ioc;
    uint32_t qty;
    uint64_t order_id;
    int64_t  price_ticks;
};

// price_ticks of a dollar price, snapped to the tick grid.
inline int64_t to_ticks(double price) {
    return static_cast<int64_t>(std::llround(price / TICK_SIZE));
}

std::vector<Msg> generate(const Scenario& sc, size_t count, uint32_t seed) {
    Rng rng(seed);
    LevelSampler levels;

    const double dt = (sc.volatility > 0.0)
        ? std::pow(sc.target_swing / sc.volatility, 2.0) / static_cast<double>(count)
        : 0.0;
    const double sigma_step = sc.volatility * std::sqrt(dt);

    // --- Phase 1: NEW orders along a GBM mid-price path ----------------------
    struct NewOrder { uint8_t side; int64_t price_ticks; uint32_t qty; int lvl; };
    std::vector<NewOrder> news;
    news.reserve(count);

    double mid = START_MID;
    for (size_t i = 0; i < count; ++i) {
        double dW = rng.normal();
        mid *= std::exp(-0.5 * sigma_step * sigma_step + sigma_step * dW);
        int64_t mid_ticks = to_ticks(std::round(mid / TICK_SIZE) * TICK_SIZE);

        int  lvl    = levels.sample(rng);
        bool is_buy = (i < count / 2);
        bool aggr   = rng.uniform() < P_AGGRESSIVE;

        int64_t px;
        if (aggr) {
            // Marketable: priced through the mid into the opposite side so it
            // crosses resting liquidity even when the mid is static.
            uint32_t depth = rng.uniform_int(1, AGGR_MAX_DEPTH);
            px = is_buy ? mid_ticks + depth : mid_ticks - depth;
        } else {
            // Resting: 1+ ticks away from the mid on the order's own side.
            px = is_buy ? mid_ticks - (lvl + 1) : mid_ticks + (lvl + 1);
        }
        if (px < 1) px = 1;

        news.push_back({ uint8_t(is_buy ? 0 : 1), px, rng.uniform_int(1, 100), lvl });
    }

    // --- Phase 2: interleave buys/sells, expand the cancel/modify lifecycle --
    std::vector<size_t> order(count);
    std::iota(order.begin(), order.end(), 0);
    // Hand-rolled Fisher-Yates on Rng::uniform_int. std::shuffle uses
    // std::uniform_int_distribution internally, whose mt19937 consumption is
    // implementation-defined — libstdc++, libc++, and MSVC produce different
    // shuffles from the same seed, breaking the bit-portability promise. The
    // explicit swap below is identical across stdlibs.
    for (size_t i = count - 1; i > 0; --i) {
        uint32_t j = rng.uniform_int(0, static_cast<uint32_t>(i));
        std::swap(order[i], order[j]);
    }

    std::vector<Msg> tl;
    // 1 NEW + cancels + modifies, plus headroom for the duplicate cancels/modifies.
    tl.reserve(static_cast<size_t>(count * (1.0 + CANCEL_PCT + MODIFY_PCT + 0.05)));

    const double base_lambda = std::log(2.0) / (MEDIAN_LIFE_MS * 1000.0);
    int64_t now = 0;

    for (size_t k = 0; k < count; ++k) {
        const NewOrder& n = news[order[k]];
        uint64_t oid = order[k] + 1;                     // 1-based order id

        bool is_ioc = rng.uniform() < IOC_PCT;
        tl.push_back({ now, MSG_NEW, n.side, uint8_t(is_ioc ? 1 : 0), n.qty, oid, n.price_ticks });
        if (is_ioc) { now += 1; continue; }              // IOC: no modify, no cancel

        // Optional modify: a quantity increase, ~80% of the time also a 1-tick
        // reprice. Under the production modify rule both a price change and a
        // quantity increase are handled as cancel + reinsert; only a pure
        // quantity decrease at an unchanged price keeps queue priority, and the
        // canonical workload deliberately contains none (see docs/METHODOLOGY.md)
        // so the three-engine reference consensus is exact.
        bool     modified = false;
        int64_t  mprice   = n.price_ticks;
        uint32_t mqty     = n.qty + 1;                        // quantity increase
        if (rng.uniform() < MODIFY_PCT) {
            int64_t tmod = now + 30 + static_cast<int64_t>(rng.uniform() * 70);
            if (rng.uniform() < 0.80)
                mprice += (n.side == 0 ? +1 : -1);           // ~80%: also reprice
            tl.push_back({ tmod, MSG_MODIFY, n.side, 0, mqty, oid, mprice });
            now = tmod;
            modified = true;
        }

        // Cancel most resting orders; deeper orders live longer.
        bool cancelled = false;
        if (rng.uniform() < CANCEL_PCT) {
            double lambda = base_lambda / std::sqrt(1.0 + n.lvl);
            int64_t ttl   = static_cast<int64_t>(rng.exponential(lambda));
            tl.push_back({ now + ttl, MSG_CANCEL, n.side, 0, n.qty, oid, n.price_ticks });
            now += ttl;
            cancelled = true;

            // Duplicate ("false") cancel: a trading system re-sending a cancel
            // to be certain the order is gone. A small fraction of cancels get
            // a second request a moment later — by then the order is already
            // gone, so the engine answers it with a CancelReject.
            if (rng.uniform() < DUP_CANCEL_PCT) {
                int64_t gap = 1 + static_cast<int64_t>(rng.uniform() * 20);
                tl.push_back({ now + gap, MSG_CANCEL, n.side, 0, n.qty, oid, n.price_ticks });
                now += gap;
            }
        }

        // Duplicate ("false") modify: a stale amend arriving after the order
        // is already gone — a real trading system emits these too. Applied to
        // a small fraction of the orders that were modified and then cancelled,
        // so it stays well under 3% of all modify messages; the engine answers
        // it with a ModifyReject.
        if (modified && cancelled && rng.uniform() < DUP_MODIFY_PCT) {
            int64_t gap = 1 + static_cast<int64_t>(rng.uniform() * 20);
            tl.push_back({ now + gap, MSG_MODIFY, n.side, 0, mqty, oid, mprice });
            now += gap;
        }
        now += 1;
    }

    // Stable sort into wire order; std::stable_sort is a deterministic function
    // of its input, so ties resolve identically on every run/platform.
    std::stable_sort(tl.begin(), tl.end(),
                     [](const Msg& a, const Msg& b) { return a.ts < b.ts; });
    return tl;
}

// On-disk format: 16-byte header then `count` fixed 40-byte records.
//   header : [u64 magic][u32 version][u32 count]
//   record : [u8 type][u8 side][u8 ioc][u8 pad][u32 qty]
//            [u64 sequence_number][u64 order_id][i64 price_ticks][i64 reserved]
bool write_binary(const std::vector<Msg>& tl, const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::perror("fopen"); return false; }

    uint64_t magic = WORKLOAD_MAGIC;
    uint32_t ver = WORKLOAD_VERSION, count = static_cast<uint32_t>(tl.size());
    bool ok = std::fwrite(&magic, 8, 1, f) == 1
           && std::fwrite(&ver,   4, 1, f) == 1
           && std::fwrite(&count, 4, 1, f) == 1;

    for (uint64_t seq = 0; ok && seq < tl.size(); ++seq) {
        const Msg& m = tl[seq];
        uint8_t  hdr[4] = { m.type, m.side, m.ioc, 0 };
        int64_t  reserved = 0;
        ok = std::fwrite(hdr,            4, 1, f) == 1
          && std::fwrite(&m.qty,         4, 1, f) == 1
          && std::fwrite(&seq,           8, 1, f) == 1
          && std::fwrite(&m.order_id,    8, 1, f) == 1
          && std::fwrite(&m.price_ticks, 8, 1, f) == 1
          && std::fwrite(&reserved,      8, 1, f) == 1;
    }
    // fclose flushes the final stdio block — a disk-full or NFS commit failure
    // shows up here and would otherwise leave a silently truncated file.
    if (std::fclose(f) != 0) {
        std::perror("fclose");
        ok = false;
    }
    if (!ok) std::fprintf(stderr, "ERROR: short write to %s\n", path);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s <scenario> <out.bin> [count] [seed]\n"
            "  scenario: static | normal | swing-25 | swing-40 | flash-crash\n",
            argv[0]);
        return 2;
    }
    const char* scenario = argv[1];
    const char* out_path = argv[2];
    size_t   count   = (argc > 3) ? std::strtoull(argv[3], nullptr, 10) : 1000000;
    uint64_t seed_in = (argc > 4) ? std::strtoull(argv[4], nullptr, 10) : 12345;

    const Scenario* sc = nullptr;
    for (const auto& s : SCENARIOS)
        if (std::strcmp(s.name, scenario) == 0) { sc = &s; break; }
    if (!sc) { std::fprintf(stderr, "ERROR: unknown scenario '%s'\n", scenario); return 2; }
    if (count < 2) { std::fprintf(stderr, "ERROR: count must be >= 2\n"); return 2; }
    if (seed_in > UINT32_MAX) {
        std::fprintf(stderr,
            "ERROR: seed %llu exceeds the supported 32-bit range "
            "(max %u); std::mt19937 takes a uint32_t. Pick a seed in "
            "[0, %u].\n",
            (unsigned long long)seed_in, UINT32_MAX, UINT32_MAX);
        return 2;
    }
    uint32_t seed = static_cast<uint32_t>(seed_in);

    std::vector<Msg> tl = generate(*sc, count, seed);
    if (!write_binary(tl, out_path)) return 1;

    size_t news = 0, cancels = 0, modifies = 0, iocs = 0;
    for (const auto& m : tl) {
        if (m.type == MSG_NEW)         { ++news; if (m.ioc) ++iocs; }
        else if (m.type == MSG_CANCEL)  ++cancels;
        else                            ++modifies;
    }
    std::fprintf(stderr,
        "generated %s seed=%u : %zu messages (%zu new [%zu ioc], %zu cancel, %zu modify) -> %s\n",
        scenario, seed, tl.size(), news, iocs, cancels, modifies, out_path);
    return 0;
}
