#!/usr/bin/env bash
# Build mansoor_adapter.so. Clones mansoor-mamnoon/limit-order-book at a
# pinned commit, applies the one source patch documented below (out-of-band
# price bounds-check in PriceLevelsContig), and compiles its sources + this
# adapter into a single .so at the harness repo root.
#
# Override the upstream checkout: ME_MANSOOR_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

MANSOOR_URL="https://github.com/mansoor-mamnoon/limit-order-book.git"
MANSOOR_REF="78e1fb0e0563388456e5030d858ef43d6407bed3"

if [ -n "${ME_MANSOOR_SRC:-}" ]; then
    SRC="$ME_MANSOOR_SRC"
else
    SRC="$TP/mansoor_limit_order_book"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$MANSOOR_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$MANSOOR_REF"
fi

# ----- Source patch: bounds-check out-of-band prices in PriceLevelsContig ---
# CORRECTNESS fix (engine source, not the adapter).
# Filed upstream: https://github.com/mansoor-mamnoon/limit-order-book/issues/3
# ("Out-of-bounds price-array access in PriceLevelsContig crashes on prices
# outside the configured band").
#
# Bug: PriceLevelsContig allocates exactly one LevelFIFO per tick in the
# configured [min_tick, max_tick] band, then hands back &levels_[idx(px)] from
# get_level()/has_level() with an UNCHECKED index. A limit priced outside the
# band (idx(px) past the end of levels_) reads/writes out of bounds — the
# resting path enqueues into it and corrupts the heap, or for a larger
# excursion walks onto an unmapped page and SIGSEGVs. This is the QuantCup-class
# representational ceiling: an out-of-domain price becomes memory corruption
# instead of a clean rejection. Not seen on the canonical seed-23 audit (it
# stays in band); it breaks on wide-swing inputs that price beyond the band.
#
# Fix (the issue's primary recommendation, applied to the engine header):
#   - add an in_band(px) helper + an isolated oob_ sentinel LevelFIFO;
#   - get_level(): out-of-band -> return oob_ (asserts under -DLOB_DEBUG_BOUNDS,
#     rejects silently in the release/benchmark build — "assert in debug, reject
#     in release"), so the order drops into an invisible slot instead of running
#     off the end of levels_;
#   - has_level(): out-of-band -> false;
#   - set_best_bid()/set_best_ask(): an out-of-band price is never cached as
#     best-of-book (otherwise &levels_[idx(px)] forms a pointer past the end
#     that best_level_ptr() would later dereference) — treat it as "no best on
#     this side". Net: an out-of-domain order is invisible to the book (never
#     scanned, never best, zero depth) instead of corrupting memory / crashing.
#
# Single-header change, in-band hot path byte-for-byte unchanged. Idempotent:
# the `git reset --hard` above restores a pristine header each rerun on the
# default checkout, and the marker guard makes a re-apply a no-op (so an
# ME_MANSOOR_SRC override is safe too). Fails loud if an anchor is missing, so
# an upstream change cannot silently no-op the fix.
python3 - "$SRC/cpp/include/lob/price_levels.hpp" <<'PY'
import sys
path = sys.argv[1]
src = open(path, encoding="utf-8").read()
marker = "PATCH(mansoor_adapter): out-of-band bounds-check"
if marker in src:
    print("price_levels.hpp already patched (out-of-band bounds-check)")
    sys.exit(0)

def replace_once(s, needle, repl, what):
    if needle not in s:
        sys.stderr.write(f"mansoor patch: anchor not found ({what}) in price_levels.hpp "
                         f"(upstream changed?)\n")
        sys.exit(1)
    if s.count(needle) != 1:
        sys.stderr.write(f"mansoor patch: anchor not unique ({what}) in price_levels.hpp\n")
        sys.exit(1)
    return s.replace(needle, repl, 1)

# (a) pull in <cassert> for the debug assert.
src = replace_once(
    src,
    '#include <functional>\n#include "types.hpp"',
    '#include <functional>\n#include <cassert>\n#include "types.hpp"',
    "include block",
)

# (b) bounds-check get_level(): out-of-band -> oob_ sentinel.
src = replace_once(
    src,
    "  LevelFIFO& get_level(Tick px) override { return levels_[idx(px)]; }",
    "  // PATCH(mansoor_adapter): out-of-band bounds-check (upstream issue mansoor-mamnoon/limit-order-book#3).\n"
    "  // A price outside the configured [min_tick, max_tick] band would otherwise\n"
    "  // index levels_ out of bounds and corrupt the heap / SIGSEGV. Out-of-domain\n"
    "  // prices are routed to an isolated sentinel level instead, so the order\n"
    "  // drops cleanly (never scanned in-band, never best-of-book, zero depth)\n"
    "  // rather than resting out of band. -DLOB_DEBUG_BOUNDS also asserts on the\n"
    "  // misconfiguration; the release/benchmark build rejects silently.\n"
    "  LevelFIFO& get_level(Tick px) override {\n"
    "    if (!in_band(px)) {\n"
    "#ifdef LOB_DEBUG_BOUNDS\n"
    '      assert(false && "price outside PriceLevelsContig band");\n'
    "#endif\n"
    "      return oob_;\n"
    "    }\n"
    "    return levels_[idx(px)];\n"
    "  }",
    "get_level",
)

# (c) has_level(): out-of-band -> false.
src = replace_once(
    src,
    "  bool has_level(Tick px) const override {\n"
    "    const auto& L = levels_[idx(px)];\n"
    "    return L.head != nullptr;\n"
    "  }",
    "  bool has_level(Tick px) const override {\n"
    "    if (!in_band(px)) return false;  // PATCH(mansoor_adapter): out-of-band\n"
    "    const auto& L = levels_[idx(px)];\n"
    "    return L.head != nullptr;\n"
    "  }",
    "has_level",
)

# (d) set_best_bid()/set_best_ask(): an out-of-band price is never cached as best.
src = replace_once(
    src,
    "  void set_best_bid(Tick px) override {\n"
    "    best_bid_ = px;\n"
    "    best_bid_ptr_ = (px == std::numeric_limits<Tick>::min()) ? nullptr : &levels_[idx(px)];\n"
    "  }\n"
    "  void set_best_ask(Tick px) override {\n"
    "    best_ask_ = px;\n"
    "    best_ask_ptr_ = (px == std::numeric_limits<Tick>::max()) ? nullptr : &levels_[idx(px)];\n"
    "  }",
    "  // PATCH(mansoor_adapter): an out-of-band price must never be cached as\n"
    "  // best-of-book — &levels_[idx(px)] would form (and best_level_ptr() would\n"
    "  // later dereference) a pointer past the end of levels_. Treat out-of-band\n"
    "  // as \"no valid best on this side\" (empty sentinel, null level pointer),\n"
    "  // consistent with the order having been dropped into the invisible oob_\n"
    "  // slot by get_level().\n"
    "  void set_best_bid(Tick px) override {\n"
    "    if (px != std::numeric_limits<Tick>::min() && !in_band(px)) {\n"
    "      best_bid_ = std::numeric_limits<Tick>::min();\n"
    "      best_bid_ptr_ = nullptr;\n"
    "      return;\n"
    "    }\n"
    "    best_bid_ = px;\n"
    "    best_bid_ptr_ = (px == std::numeric_limits<Tick>::min()) ? nullptr : &levels_[idx(px)];\n"
    "  }\n"
    "  void set_best_ask(Tick px) override {\n"
    "    if (px != std::numeric_limits<Tick>::max() && !in_band(px)) {\n"
    "      best_ask_ = std::numeric_limits<Tick>::max();\n"
    "      best_ask_ptr_ = nullptr;\n"
    "      return;\n"
    "    }\n"
    "    best_ask_ = px;\n"
    "    best_ask_ptr_ = (px == std::numeric_limits<Tick>::max()) ? nullptr : &levels_[idx(px)];\n"
    "  }",
    "set_best",
)

# (e) in_band() helper next to idx().
src = replace_once(
    src,
    "  size_t idx(Tick px) const { return static_cast<size_t>(px - band_.min_tick); }",
    "  size_t idx(Tick px) const { return static_cast<size_t>(px - band_.min_tick); }\n"
    "  bool in_band(Tick px) const { return px >= band_.min_tick && px <= band_.max_tick; }",
    "idx helper",
)

# (f) oob_ catch-all member at the end of PriceLevelsContig.
src = replace_once(
    src,
    "  LevelFIFO*             best_bid_ptr_;\n"
    "  LevelFIFO*             best_ask_ptr_;\n"
    "};",
    "  LevelFIFO*             best_bid_ptr_;\n"
    "  LevelFIFO*             best_ask_ptr_;\n"
    "  // PATCH(mansoor_adapter): catch-all for out-of-band prices. get_level()\n"
    "  // returns this instead of running off the end of levels_; anything\n"
    "  // enqueued here is invisible to the book, so an out-of-domain order is\n"
    "  // effectively rejected.\n"
    "  LevelFIFO              oob_{};\n"
    "};",
    "members",
)

open(path, "w", encoding="utf-8").write(src)
print("patched price_levels.hpp: out-of-band bounds-check in PriceLevelsContig")
PY

cd "$DIR"
g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -I"$SRC/cpp/include" -I"$REPO/api" \
    -o "$REPO/mansoor_adapter.so" \
    mansoor_adapter.cpp \
    "$SRC/cpp/src/book_core.cpp" \
    "$SRC/cpp/src/price_levels.cpp" \
    "$SRC/cpp/src/logging.cpp" \
    "$SRC/cpp/src/util.cpp" \
    -pthread
echo "built: mansoor_adapter.so"
