#!/usr/bin/env bash
# Build piyush_adapter.so. Clones PIYUSH-KUMAR1809/order-matching-engine at a
# pinned commit and compiles its sources + this adapter into a single .so at
# the harness repo root.
#
# Override the upstream checkout: ME_PIYUSH_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

PIYUSH_URL="https://github.com/PIYUSH-KUMAR1809/order-matching-engine.git"
PIYUSH_REF="033d7859186bdc7e265b76883da5515722f7f249"

if [ -n "${ME_PIYUSH_SRC:-}" ]; then
    SRC="$ME_PIYUSH_SRC"
else
    SRC="$TP/piyush_order_matching_engine"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$PIYUSH_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$PIYUSH_REF"
fi

# ----- Source patch: re-seat the cached best ask after a buy empties a level --
# Bug (CORRECTNESS, not performance): in StandardMatchingStrategy::match()'s
# OrderSide::Buy branch, when an aggressive buy exhausts itself on the SAME step
# that empties the current best-ask level, `if (incoming.quantity == 0) break;`
# exits the outer while before the p++ advance, so `book.bestAsk` is left
# pointing at the now-emptied, mask-cleared level. The post-loop corrective only
# zeroes bestAsk when the WHOLE ask side is gone — findFirstSet scans upward
# *inclusive*, so if any higher ask is still set it returns that index
# (< MAX_PRICE), the guard is false, and bestAsk stays stale. Trades are correct
# (the next aggressor re-derives best from the mask), but any BBO/spread query
# between operations reads the stale ask. The sell side already self-heals
# (L157-161 tests bestBid itself and walks down via findFirstSetDown).
# Fix: re-seat bestAsk to the lowest still-set ask at-or-above its current value
# (exactly what findFirstSet(bestAsk) returns), mirroring OrderBook::cancelOrder
# L84-86 and the sell-side corrective — still resetting to -1 when no asks
# remain. 2-line change, no effect on matching results. Reported upstream
# (issue: "Cached best ask goes stale after a buy clears a price level").
# Idempotent: `git reset --hard` above restores the pristine file each rerun;
# the marker guard makes a re-apply a no-op, and the anchor check fails loud if
# upstream ever changes the block (so the fix cannot silently no-op).
python3 - "$SRC/src/MatchingStrategy.hpp" <<'PY'
import sys
path = sys.argv[1]
src = open(path, encoding="utf-8").read()
marker = "PATCH(piyush_adapter): re-seat stale best ask"
if marker in src:
    print("MatchingStrategy.hpp already patched (best-ask re-seat)")
    sys.exit(0)
needle = (
    "      if (book.askMask.findFirstSet(book.bestAsk) >= OrderBook::MAX_PRICE) {\n"
    "        book.bestAsk = -1;\n"
    "      }\n"
)
repl = (
    "      // PATCH(piyush_adapter): re-seat stale best ask. A buy that empties\n"
    "      // the best-ask level and exhausts on the same step leaves bestAsk on\n"
    "      // the now-cleared level; the original guard only reset it when the\n"
    "      // whole ask side was gone. findFirstSet scans upward inclusive, so\n"
    "      // this advances bestAsk to the lowest still-set ask (or -1 if none),\n"
    "      // mirroring OrderBook::cancelOrder and the sell-side corrective.\n"
    "      size_t nextAsk = book.askMask.findFirstSet(book.bestAsk);\n"
    "      book.bestAsk = (nextAsk >= OrderBook::MAX_PRICE) ? -1 : (Price)nextAsk;\n"
)
if needle not in src:
    sys.stderr.write(
        "piyush patch: best-ask anchor not found in MatchingStrategy.hpp "
        "(upstream changed?)\n")
    sys.exit(1)
src = src.replace(needle, repl, 1)
open(path, "w", encoding="utf-8").write(src)
print("patched MatchingStrategy.hpp: best-ask re-seat after buy empties a level")
PY

cd "$DIR"
g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -I"$SRC/src" -I"$REPO/api" \
    -o "$REPO/piyush_adapter.so" \
    piyush_adapter.cpp \
    "$SRC/src/OrderBook.cpp" \
    "$SRC/src/Order.cpp" \
    -pthread
echo "built: piyush_adapter.so"
