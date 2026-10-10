#!/usr/bin/env bash
# Build lcv3_adapter.so. Clones lc-lukaszczerwinski/public (LC++ v3) at
# fix cancelACK qty (commit f956a09190c41f8873438f9e3a0a386777a87647) into
# third_party/lcv3/ and compiles this adapter against its headers into
# lcv3_adapter.so at the harness repo root.
#
# The engine is header-only; order_book_v3.cpp is a standalone benchmark
# and is not compiled into the adapter.  No source patch is required.
#
# Override:
#   ME_LCV3_SRC=/path/to/existing/public-checkout
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

LCV3_URL="https://github.com/lc-lukaszczerwinski/public.git"
LCV3_REF="f956a09190c41f8873438f9e3a0a386777a87647"

if [ -n "${ME_LCV3_SRC:-}" ]; then
    SRC="$ME_LCV3_SRC"
else
    SRC="$TP/lcv3"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$LCV3_URL" "$SRC"
    fi
    git -C "$SRC" fetch --quiet origin "$LCV3_REF" 2>/dev/null \
        || git -C "$SRC" fetch --quiet origin 2>/dev/null \
        || true
    git -C "$SRC" reset --hard --quiet "$LCV3_REF"
fi

g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -fexceptions -frtti \
    -I "$REPO/api" \
    -I "$SRC" \
    "$DIR/lcv3_adapter.cpp" \
    -o "$REPO/lcv3_adapter.so"

echo "built: lcv3_adapter.so"