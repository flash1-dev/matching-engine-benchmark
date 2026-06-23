#!/usr/bin/env bash
# Build kautenja_adapter.so. Clones Kautenja/limit-order-book at a pinned
# commit, applies one source patch to the engine's match loop (a per-fill hook
# documented below), and compiles the result + this adapter into a single .so
# at the harness repo root. The engine is header-only, so there is no engine
# translation unit to compile — only the adapter.
#
# Patch (applied AFTER `git reset --hard` to the pin, so the reset can never
# clobber it; idempotent and fails loud if its anchor drifts):
#
#   Per-fill trade hook. The match loop LimitTree::market
#   (include/limit_tree.hpp) reports a consumed maker to its did_fill callback
#   by uid ONLY — it carries no per-fill price or quantity — and for the LAST
#   maker, when that maker is only PARTIALLY consumed, it does not invoke the
#   callback at all. The harness needs one Trade per fill carrying the maker's
#   resting price and the fill quantity, in match order. So we inject a one-line
#   call __kautenja_trade_hook(maker_uid, maker_price, fill_qty) into market()
#   at BOTH fill sites. The adapter implements the hook. The patch ONLY adds
#   emit points — it changes no matching logic, prices, or quantities. This is
#   adapter instrumentation (surfacing fills the shipped engine never exposes),
#   not the engine's duplicate-id correctness fix; see README.md "Source patch".
#
# Override the upstream checkout: ME_KAUTENJA_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

KAUTENJA_URL="https://github.com/Kautenja/limit-order-book.git"
KAUTENJA_REF="88416a12a0b34b026cbf1d598823fd315a1f2dbf"

if [ -n "${ME_KAUTENJA_SRC:-}" ]; then
    SRC="$ME_KAUTENJA_SRC"
else
    SRC="$TP/kautenja_limit_order_book"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$KAUTENJA_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$KAUTENJA_REF"
fi

# The book pulls three header-only deps in as git submodules; a plain clone
# leaves them empty. Init exactly the three the adapter needs (skip the heavy
# test-only submodules: Catch2, hopscotch-map, sparse-map, cpptqdm). Safe to
# re-run; if an ME_KAUTENJA_SRC override already has them populated, the headers
# below short-circuit it. Each path is pinned by the engine commit's gitlink.
for sub in vendor/binary-search-tree vendor/doubly-linked-list vendor/robin-map; do
    if [ ! -e "$SRC/$sub/include" ]; then
        git -C "$SRC" submodule update --init --quiet "$sub"
    fi
done

# Patch include/limit_tree.hpp: insert the per-fill hook declaration before the
# LimitTree struct and instrument LimitTree::market at both fill sites. The
# original market() body is a single unique contiguous block (matched verbatim
# below); the whole patch is a no-op if the hook is already present, so an
# ME_KAUTENJA_SRC override re-runs cleanly.
LTPATH="$SRC/include/limit_tree.hpp"
python3 - "$LTPATH" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
src = p.read_text()
if "__kautenja_trade_hook" in src:
    print("limit_tree.hpp already patched (trade hook)")
    sys.exit(0)

# Forward declaration of the hook, inserted just before the LimitTree struct.
decl_anchor = "/// A single side (buy/sell) of the LimitOrderBook.\ntemplate<Side side>\nstruct LimitTree {"
decl = (
    '/// Per-fill hook injected for the matching-engine benchmark adapter.\n'
    '/// Implemented in kautenja_adapter.cpp; called once per fill in match\n'
    '/// order with the consumed maker\'s uid, the maker\'s resting price, and\n'
    '/// the filled quantity. The shipped engine surfaces none of these per fill.\n'
    'extern "C" void __kautenja_trade_hook(uint64_t maker_uid, uint64_t maker_price, uint32_t fill_qty);\n\n'
    + decl_anchor
)
assert src.count(decl_anchor) == 1, "decl anchor not unique"
src = src.replace(decl_anchor, decl)

# The original market() body, verbatim from the pinned source.
orig = """    template<typename Callback>
    void market(Order* order, Callback did_fill) {
        // find orders until there are none
        while (best != nullptr && can_match<side>(best->key, order->price)) {
            // get the next match as the front of the best price
            auto match = best->order_head;
            if (match->quantity >= order->quantity) {  // current match can fill
                if (match->quantity == order->quantity) {  // limit order filled
                    // remove the current match from the book
                    cancel(match);
                    did_fill(match->uid);
                } else {  // limit order partially filled
                    // remove the market order quantity from the limit quantity
                    match->quantity -= order->quantity;
                    // update the match's limit volume
                    match->limit->volume -= order->quantity;
                    // update the volume for the entire tree
                    volume -= order->quantity;
                }
                // clear the remaining quantity for the order
                order->quantity = 0;
                return;
            }  // else: current match can NOT fill
            // decrement the remaining quantity of the market order
            order->quantity -= match->quantity;
            // remove the current match from the book
            cancel(match);
            did_fill(match->uid);
        }

    }"""

# Instrumented body. Only adds two __kautenja_trade_hook calls; the matching
# logic, prices, and quantities are otherwise byte-identical to the original.
#  - First hook (maker can fully fill the incoming remainder): fill_qty is the
#    incoming order's remaining quantity (order->quantity), captured before it is
#    zeroed; maker price is match->price (the resting price).
#  - Second hook (maker smaller than the incoming remainder): fill_qty is the
#    maker's full quantity (match->quantity), captured before cancel().
instr = """    template<typename Callback>
    void market(Order* order, Callback did_fill) {
        // find orders until there are none
        while (best != nullptr && can_match<side>(best->key, order->price)) {
            // get the next match as the front of the best price
            auto match = best->order_head;
            if (match->quantity >= order->quantity) {  // current match can fill
                // adapter hook: one fill of the incoming remainder against this maker
                __kautenja_trade_hook(match->uid, match->price, order->quantity);
                if (match->quantity == order->quantity) {  // limit order filled
                    // remove the current match from the book
                    cancel(match);
                    did_fill(match->uid);
                } else {  // limit order partially filled
                    // remove the market order quantity from the limit quantity
                    match->quantity -= order->quantity;
                    // update the match's limit volume
                    match->limit->volume -= order->quantity;
                    // update the volume for the entire tree
                    volume -= order->quantity;
                }
                // clear the remaining quantity for the order
                order->quantity = 0;
                return;
            }  // else: current match can NOT fill
            // adapter hook: one fill consuming this maker fully
            __kautenja_trade_hook(match->uid, match->price, match->quantity);
            // decrement the remaining quantity of the market order
            order->quantity -= match->quantity;
            // remove the current match from the book
            cancel(match);
            did_fill(match->uid);
        }

    }"""

assert src.count(orig) == 1, "market() body anchor not unique / drifted (upstream changed?)"
src = src.replace(orig, instr)
p.write_text(src)
print("patched limit_tree.hpp: per-fill trade hook at both market() fill sites")
PY

# C++ toolchain: this repo's C++ adapters use the system g++ (>= C++20). No
# toolchain is auto-installed; fail loud if g++ is missing.
command -v g++ >/dev/null 2>&1 || { echo "g++ not found (need a C++20 compiler)" >&2; exit 1; }

cd "$DIR"
g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -I"$REPO/api" \
    -I"$SRC/include" \
    -I"$SRC/vendor/binary-search-tree/include" \
    -I"$SRC/vendor/doubly-linked-list/include" \
    -I"$SRC/vendor/robin-map/include" \
    -o "$REPO/kautenja_adapter.so" \
    kautenja_adapter.cpp
echo "built: kautenja_adapter.so"
