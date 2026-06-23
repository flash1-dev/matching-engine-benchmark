#!/usr/bin/env bash
# Build jxm35_adapter.so. Clones jxm35/LimitOrderBook-MatchingEngine at a
# pinned commit, applies the source patches documented below, and compiles the
# result + this adapter into a single .so at the harness repo root.
#
# Patches (all applied AFTER `git reset --hard` to the pin, so the reset can
# never clobber them; each is idempotent and fails loud if its anchor is gone):
#
#  P0  Maker/taker-id hook. OrderBook<>::TryMatch is the only fill site and it
#      never exposes which two orders traded, so the adapter injects one
#      extern "C" __jxm35_adapter_trade_hook(...) call there to capture the
#      maker/taker ids per fill (the engine declares MDAdapter::notify_trade
#      but never calls it). Pre-existing; this is what the adapter drains into
#      Trade reports.
#
#  P1  CORRECTNESS — emit trade events (upstream issue
#      https://github.com/jxm35/LimitOrderBook-MatchingEngine/issues/1).
#      TryMatch fills the book and bumps matchedQuantity_ but never calls
#      notify_trade, so the market-data feed sees price-level updates but no
#      executions. OrderBook has no trade-id counter, so add `nextTradeId_` and
#      call md_adapter_.notify_trade(...) at the fill site next to the existing
#      level-change call. (Functionally the adapter reads trades via P0's hook
#      and uses the Null publisher, but this restores the engine's own feed so
#      the shipped benchmark engine is the FIXED engine.)
#
#  P2  CORRECTNESS — double-unlink in RemoveOrder (upstream issue
#      https://github.com/jxm35/LimitOrderBook-MatchingEngine/issues/2, the
#      decisive fix). Cancelling/modifying a
#      non-head order on a level of >=2 orders unlinks the node TWICE: a
#      hand-splice touches only prev/next (skipping the owning Limit's
#      counters) and then limit->RemoveOrder() re-walks from head_ to do the
#      real size_/orderQuantity_ accounting — but the hand-splice already
#      pulled the target out of that chain, so the walk bails ("Order not
#      found") and size_/orderQuantity_ are never decremented. The level then
#      overstates depth and hides makers from TryMatch (51 fewer trades than
#      the consensus on a deep book; AmendOrder is cancel+reinsert so modify
#      inherits it). Fix: drop the hand-splice and let limit->RemoveOrder() be
#      the sole unlink (the head path already relies on it).
#
# Override the upstream checkout: ME_JXM35_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

JXM35_URL="https://github.com/jxm35/LimitOrderBook-MatchingEngine.git"
JXM35_REF="b5984aacb1f9a1816855df4942752711866dbfbf"

if [ -n "${ME_JXM35_SRC:-}" ]; then
    SRC="$ME_JXM35_SRC"
else
    SRC="$TP/jxm35_limit_order_book_matching_engine"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$JXM35_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$JXM35_REF"
fi

# P0: Patch OrderBook.cpp in place: inject one extern "C" hook call inside
# TryMatch right after the matched-quantity bookkeeping line. Idempotent —
# reset --hard above restores the file before re-patching.
OBPATH="$SRC/lib/OrderBook/src/core/OrderBook.cpp"
OBHPATH="$SRC/lib/OrderBook/include/core/OrderBook.h"
python3 - "$OBPATH" <<'PY'
import re, sys, pathlib
p = pathlib.Path(sys.argv[1])
src = p.read_text()
if "__jxm35_adapter_trade_hook" in src:
    sys.exit(0)
hook_decl = (
    '\nextern "C" void __jxm35_adapter_trade_hook'
    '(uint64_t maker_id, uint64_t taker_id, uint64_t qty, int64_t price);\n'
)
last_inc = max(m.end() for m in re.finditer(r'^#include.*$', src, re.MULTILINE))
src = src[:last_inc] + hook_decl + src[last_inc:]
needle = "incomingOrder.DecreaseQuantity(matchedQty);"
inject = (
    "incomingOrder.DecreaseQuantity(matchedQty);\n"
    "            __jxm35_adapter_trade_hook("
    "static_cast<uint64_t>(restingOrder.OrderId()),"
    "static_cast<uint64_t>(incomingOrder.OrderId()),"
    "static_cast<uint64_t>(matchedQty),"
    "static_cast<int64_t>(opposingPrice));"
)
assert src.count(needle) == 1, "patch needle not unique"
src = src.replace(needle, inject)
p.write_text(src)
PY

# P1: CORRECTNESS — emit trade events (upstream issue
# https://github.com/jxm35/LimitOrderBook-MatchingEngine/issues/1).
# (a) Add a trade-id counter to the OrderBook class private section, next to
#     matchedQuantity_. (b) Call md_adapter_.notify_trade(...) at the fill site
#     in TryMatch, right after the existing notify_price_level_change call, so
#     the engine's own market-data feed now publishes executions (price, qty,
#     aggressor side) instead of silently swallowing every fill. Idempotent
#     (marker guards); fails loud if either anchor is gone.
python3 - "$OBHPATH" "$OBPATH" <<'PY'
import sys, pathlib
hpath, cpath = sys.argv[1], sys.argv[2]

# (a) trade-id counter in OrderBook.h
h = pathlib.Path(hpath)
hsrc = h.read_text()
if "nextTradeId_" not in hsrc:
    member = "    long matchedQuantity_;"
    assert hsrc.count(member) == 1, "P1 header anchor not unique"
    hsrc = hsrc.replace(
        member,
        member + "\n"
        "    // jxm35_adapter P1 (upstream issue jxm35/LimitOrderBook-MatchingEngine#1):\n"
        "    // trade-id source for the notify_trade emission the engine was missing.\n"
        "    uint64_t nextTradeId_ = 1;")
    h.write_text(hsrc)

# (b) notify_trade call at the fill site in OrderBook.cpp
c = pathlib.Path(cpath)
csrc = c.read_text()
if "nextTradeId_++" not in csrc:
    needle = ("md_adapter_.notify_price_level_change(opposingPrice, "
              "restingQty - matchedQty, restingQty,\n"
              "                !isBuy); // TODO: This should happen within the limit")
    assert csrc.count(needle) == 1, "P1 fill-site anchor not unique"
    inject = (needle + "\n"
              "            // jxm35_adapter P1 (upstream issue jxm35/LimitOrderBook-MatchingEngine#1):\n"
              "            // emit the trade the engine never published. buyerAggressed ==\n"
              "            // isBuy (the incoming order is the aggressor).\n"
              "            md_adapter_.notify_trade(nextTradeId_++, "
              "static_cast<uint64_t>(opposingPrice), "
              "static_cast<uint64_t>(matchedQty), isBuy);")
    csrc = csrc.replace(needle, inject)
    c.write_text(csrc)
PY

# P2: CORRECTNESS — double-unlink in RemoveOrder (upstream issue
# https://github.com/jxm35/LimitOrderBook-MatchingEngine/issues/2, the decisive
# fix). Drop the hand-splice block in
# OrderBook<>::RemoveOrder so limit->RemoveOrder() is the SOLE unlink and the
# size_/orderQuantity_ accounting is no longer skipped for non-head
# cancels/modifies. Idempotent (marker guard); fails loud if the block is gone.
python3 - "$OBPATH" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
src = p.read_text()
marker = "jxm35_adapter P2 (upstream issue jxm35/LimitOrderBook-MatchingEngine#2)"
if marker not in src:
    block = (
        "    auto prev = obe->previous.lock();\n"
        "    auto next = obe->next;\n"
        "\n"
        "    if (prev && next) {\n"
        "        next->previous = obe->previous;\n"
        "        prev->next = next;\n"
        "    }\n"
        "    else if (prev) {\n"
        "        prev->next = nullptr;\n"
        "    }\n"
        "    else if (next) {\n"
        "        next->previous.reset();\n"
        "    }\n"
        "\n"
        "    limit->RemoveOrder(")
    repl = (
        "    // jxm35_adapter P2 (upstream issue jxm35/LimitOrderBook-MatchingEngine#2):\n"
        "    // the hand-splice that used to live here unlinked the node a SECOND time,\n"
        "    // touching only prev/next and skipping the owning Limit's counters,\n"
        "    // so limit->RemoveOrder()'s re-walk from head_ never reached the\n"
        "    // target for non-head cancels/modifies and size_/orderQuantity_ were\n"
        "    // never decremented. Dropped — limit->RemoveOrder() is now the sole\n"
        "    // unlink (the head path already relied on it).\n"
        "    limit->RemoveOrder(")
    assert src.count(block) == 1, "P2 hand-splice block anchor not unique"
    src = src.replace(block, repl)
    p.write_text(src)
PY

cd "$DIR"
# jxm35 uses std::expected in OrderBookEntry, which only became standard
# library in C++23 — this adapter is the only one that needs -std=c++23.
g++ -std=c++23 -O3 -march=native -fPIC -shared \
    -I"$SRC/lib/OrderBook/include" \
    -I"$SRC/lib/MDFeed/include" \
    -I"$REPO/api" \
    -include cstdint \
    -DSPDLOG_HEADER_ONLY=1 -DSPDLOG_FMT_EXTERNAL=0 \
    -o "$REPO/jxm35_adapter.so" \
    jxm35_adapter.cpp \
    "$OBPATH" \
    "$SRC/lib/OrderBook/src/orders/OrderCore.cpp" \
    "$SRC/lib/OrderBook/src/orders/Order.cpp" \
    "$SRC/lib/OrderBook/src/entries/OrderBookEntry.cpp" \
    "$SRC/lib/OrderBook/src/securities/Security.cpp" \
    "$SRC/lib/MDFeed/src/publisher/MarketDataPublisher.cpp" \
    -pthread -lfmt
echo "built: jxm35_adapter.so"
