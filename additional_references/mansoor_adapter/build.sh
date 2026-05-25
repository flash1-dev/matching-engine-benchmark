#!/usr/bin/env bash
# Build mansoor_adapter.so. Clones mansoor-mamnoon/limit-order-book at a
# pinned commit and compiles its sources + this adapter into a single .so at
# the harness repo root.
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
