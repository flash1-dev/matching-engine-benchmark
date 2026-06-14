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

cd "$DIR"
g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -I"$SRC/src" -I"$REPO/api" \
    -o "$REPO/piyush_adapter.so" \
    piyush_adapter.cpp \
    "$SRC/src/OrderBook.cpp" \
    "$SRC/src/Order.cpp" \
    -pthread
echo "built: piyush_adapter.so"
