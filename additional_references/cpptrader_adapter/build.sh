#!/usr/bin/env bash
# Build cpptrader_adapter.so. Clones chronoxor/CppTrader and its CppCommon
# dependency at pinned commits, then compiles the three CppTrader matching
# translation units (market_manager / order / order_book) together with this
# adapter into a single .so at the harness repo root.
#
# We do not install the upstream "gil" tool; instead CppCommon is cloned
# directly into modules/CppCommon next to CppTrader's CMake CppCommon.cmake.
#
# Overrides:
#   ME_CPPTRADER_SRC=/path/to/existing/CppTrader
#   ME_CPPCOMMON_SRC=/path/to/existing/CppCommon
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

CPPTRADER_URL="https://github.com/chronoxor/CppTrader.git"
CPPTRADER_REF="831d10e2a6dd96ac7b063f1d418f6563cbf74c50"
CPPCOMMON_URL="https://github.com/chronoxor/CppCommon.git"
CPPCOMMON_REF="e14011974b8d463cc854239bf351275b5a857de6"

if [ -n "${ME_CPPTRADER_SRC:-}" ]; then
    SRC="$ME_CPPTRADER_SRC"
else
    SRC="$TP/CppTrader"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$CPPTRADER_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$CPPTRADER_REF"
fi

if [ -n "${ME_CPPCOMMON_SRC:-}" ]; then
    CC="$ME_CPPCOMMON_SRC"
else
    CC="$SRC/modules/CppCommon"
    if [ ! -d "$CC/.git" ]; then
        mkdir -p "$CC"
        git clone --quiet "$CPPCOMMON_URL" "$CC"
    fi
    git -C "$CC" reset --hard --quiet "$CPPCOMMON_REF"
fi

cd "$DIR"
g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -I"$REPO/api" \
    -I"$SRC/include" \
    -I"$CC/include" \
    -I"$SRC/modules" \
    -o "$REPO/cpptrader_adapter.so" \
    cpptrader_adapter.cpp \
    "$SRC/source/trader/matching/market_manager.cpp" \
    "$SRC/source/trader/matching/order.cpp" \
    "$SRC/source/trader/matching/order_book.cpp" \
    -pthread
echo "built: cpptrader_adapter.so"
