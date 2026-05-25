#!/usr/bin/env bash
# Build robaho_adapter.so. Clones robaho/cpp_orderbook and robaho/cpp_fixed at
# pinned commits, applies a one-token C++20 conformance fix to two engine
# headers (vector<const string> → vector<string>; the affected accessors are
# not called by the adapter), and compiles the result + this adapter into a
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

cd "$DIR"
g++ -std=c++20 -O3 -march=native -fPIC -shared -fno-permissive \
    -I"$SRC" -I"$FIXED" -I"$REPO/api" \
    -o "$REPO/robaho_adapter.so" \
    robaho_adapter.cpp \
    "$SRC/orderbook.cpp" \
    "$SRC/exchange.cpp" \
    -pthread
echo "built: robaho_adapter.so"
