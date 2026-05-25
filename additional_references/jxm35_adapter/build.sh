#!/usr/bin/env bash
# Build jxm35_adapter.so. Clones jxm35/LimitOrderBook-MatchingEngine at a
# pinned commit, patches OrderBook.cpp to expose maker/taker ids via a single
# adapter-side hook (the engine declares notify_trade but never calls it from
# TryMatch), and compiles the result + this adapter into a single .so at the
# harness repo root.
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

# Patch OrderBook.cpp in place: inject one extern "C" hook call inside
# TryMatch right after the matched-quantity bookkeeping line. Idempotent —
# reset --hard above restores the file before re-patching.
OBPATH="$SRC/lib/OrderBook/src/core/OrderBook.cpp"
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
