#!/usr/bin/env bash
# Build robaho_adapter.so. Clones robaho/cpp_orderbook and robaho/cpp_fixed at
# pinned commits, applies a one-token C++20 conformance fix to two engine
# headers (vector<const string> → vector<string>; the affected accessors are
# not called by the adapter), applies a correctness fix to the matchOrders
# fill price (see patch below), and compiles the result + this adapter into a
# single .so at the harness repo root.
#
# Overrides:
#   ME_ROBAHO_SRC=/path/to/existing/cpp_orderbook
#   ME_FIXED_SRC=/path/to/existing/cpp_fixed
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

ROBAHO_URL="https://github.com/robaho/cpp_orderbook.git"
ROBAHO_REF="f42358145e40015f709f1caa04670f88c8b8be40"
FIXED_URL="https://github.com/robaho/cpp_fixed.git"
FIXED_REF="e6bdb17d4ac9bb871ac34666a5bcb0563a027703"

if [ -n "${ME_ROBAHO_SRC:-}" ]; then
    SRC="$ME_ROBAHO_SRC"
else
    SRC="$TP/robaho_cpp_orderbook"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$ROBAHO_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$ROBAHO_REF"
fi

if [ -n "${ME_FIXED_SRC:-}" ]; then
    FIXED="$ME_FIXED_SRC"
else
    FIXED="$TP/robaho_cpp_fixed"
    if [ ! -d "$FIXED/.git" ]; then
        git clone --quiet "$FIXED_URL" "$FIXED"
    fi
    git -C "$FIXED" reset --hard --quiet "$FIXED_REF"
fi

# C++20 conformance fix: vector<const T> is ill-formed. The methods that
# use these vectors are never called from the adapter; pure source-level fix.
sed -i 's/std::vector<const std::string>/std::vector<std::string>/g' \
    "$SRC/exchange.h" "$SRC/bookmap.h"

# Correctness fix (price-time priority): trades must execute at the resting
# MAKER price, not the aggressor's limit. OrderBook::matchOrders priced fills
# as `F price = MIN(bid->_price, ask->_price)` (orderbook.cpp:24). Since a
# match only happens when `bid->_price >= ask->_price` (orderbook.cpp:22),
# that MIN always returns ask->_price — correct when a buyer lifts a resting
# ask, but wrong when a seller hits a higher resting bid: the cross then prints
# at the seller's own lower limit instead of the maker's price (and fill()
# folds it into both orders' averagePrice()). matchOrders already names the
# resting side as `opposite`; this patch computes aggressor/opposite first and
# prices the fill from `opposite->_price`. The buy-aggressor case is unchanged
# (opposite == ask, so opposite->_price == MIN(...)); the sell-aggressor case
# is fixed. Qty/remaining/book state are untouched. Filed upstream as
# https://github.com/robaho/cpp_orderbook/issues/2 (see CORRECTNESS_FINDINGS.md
# at the repo root); first baseline divergence on `normal` is a sell
# crossing a resting bid (same qty/maker/taker, different price). Applied in
# Python with a loud-fail anchor so an upstream change cannot silently no-op
# the fix. Idempotent: the `git reset --hard` above restores a pristine
# orderbook.cpp each run on the default checkout, and the marker guard makes a
# re-apply (e.g. under an ME_ROBAHO_SRC override) a no-op.
python3 - "$SRC/orderbook.cpp" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
marker = "robaho_adapter: execute at the resting (maker) price"
if marker in s:
    print("orderbook.cpp already patched (maker price)")
    sys.exit(0)
needle = (
    "            int qty = MIN(bid->remaining, ask->remaining);\n"
    "            F price = MIN(bid->_price, ask->_price);\n"
    "\n"
    "            Order* aggressor = aggressorSide == Order::BUY ? bid : ask;\n"
    "            Order* opposite = aggressorSide == Order::BUY ? ask : bid;\n"
)
repl = (
    "            Order* aggressor = aggressorSide == Order::BUY ? bid : ask;\n"
    "            Order* opposite = aggressorSide == Order::BUY ? ask : bid;\n"
    "\n"
    "            int qty = MIN(bid->remaining, ask->remaining);\n"
    "            // robaho_adapter: execute at the resting (maker) price, not\n"
    "            // MIN(bid,ask) (which is the aggressor's limit on a sell that\n"
    "            // crosses a higher resting bid). Filed upstream as\n"
    "            // https://github.com/robaho/cpp_orderbook/issues/2 .\n"
    "            F price = opposite->_price;\n"
)
if needle not in s:
    sys.exit("robaho build.sh maker-price patch: anchor not found in orderbook.cpp (upstream changed?)")
s = s.replace(needle, repl, 1)
open(p, "w").write(s)
print("patched orderbook.cpp: fill executes at the resting maker price")
PYEOF

cd "$DIR"
g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -I"$SRC" -I"$FIXED" -I"$REPO/api" \
    -o "$REPO/robaho_adapter.so" \
    robaho_adapter.cpp \
    "$SRC/orderbook.cpp" \
    "$SRC/exchange.cpp" \
    -pthread
echo "built: robaho_adapter.so"
