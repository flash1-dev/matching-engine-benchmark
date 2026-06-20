#!/usr/bin/env bash
# Build cpp_orderbook_adapter.so. Clones geseq/cpp-orderbook and its two header
# dependencies (geseq/cpp-decimal, geseq/cpp-pool) at pinned commits and
# compiles the four engine matching translation units + this adapter into a
# single .so at the harness repo root.
#
# NO patch is applied: the price-cross bug we reported was fixed upstream and
# the pinned engine commit (main HEAD) already contains the fix, so each repo is
# only cloned and reset to its pin (the mansoor/piyush pattern, not robaho's).
#
# The engine and the harness both need Boost intrusive headers; by default we
# rely on system Boost (libboost-all-dev, which the harness already requires),
# so no -I is needed. ME_BOOST_SRC points at a Boost source/superproject root
# for boxes without system Boost (see the override below).
#
# Overrides:
#   ME_CPP_ORDERBOOK_SRC=/path/to/existing/cpp-orderbook
#   ME_DECIMAL_SRC=/path/to/existing/cpp-decimal
#   ME_POOL_SRC=/path/to/existing/cpp-pool
#   ME_BOOST_SRC=/path/to/boost  (source tree or CPM superproject root; only
#                                 needed where system Boost is unavailable)
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

CPP_ORDERBOOK_URL="https://github.com/geseq/cpp-orderbook.git"
CPP_ORDERBOOK_REF="b58d931b02928a83b4038fa2125edce14adbd90e"
DECIMAL_URL="https://github.com/geseq/cpp-decimal.git"
DECIMAL_REF="88646b353a4ef191b4936bf765554c726dcaf9fb"  # tag v2.1.0
POOL_URL="https://github.com/geseq/cpp-pool.git"
POOL_REF="730fe13f2c473b8ef4fe73c58dad048016c1fffd"  # tag v0.5.0

if [ -n "${ME_CPP_ORDERBOOK_SRC:-}" ]; then
    SRC="$ME_CPP_ORDERBOOK_SRC"
else
    SRC="$TP/cpp_orderbook"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$CPP_ORDERBOOK_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$CPP_ORDERBOOK_REF"
fi

if [ -n "${ME_DECIMAL_SRC:-}" ]; then
    DECIMAL="$ME_DECIMAL_SRC"
else
    DECIMAL="$TP/cpp_decimal"
    if [ ! -d "$DECIMAL/.git" ]; then
        git clone --quiet "$DECIMAL_URL" "$DECIMAL"
    fi
    git -C "$DECIMAL" reset --hard --quiet "$DECIMAL_REF"
fi

if [ -n "${ME_POOL_SRC:-}" ]; then
    POOL="$ME_POOL_SRC"
else
    POOL="$TP/cpp_pool"
    if [ ! -d "$POOL/.git" ]; then
        git clone --quiet "$POOL_URL" "$POOL"
    fi
    git -C "$POOL" reset --hard --quiet "$POOL_REF"
fi

# Both deps expose their single header from include/ (decimal.hpp, pool.hpp),
# included by the engine as "decimal.hpp" / "pool.hpp".
DECIMAL_INC="$DECIMAL/include"
POOL_INC="$POOL/include"

# Boost: default to system headers (no -I). ME_BOOST_SRC collects include dirs
# from a Boost source tree or a CPM superproject (boost-src with libs/*/include).
BOOST_INCS=()
if [ -n "${ME_BOOST_SRC:-}" ]; then
    if [ -d "$ME_BOOST_SRC/libs" ]; then
        while IFS= read -r inc; do
            BOOST_INCS+=("-I$inc")
        done < <(find "$ME_BOOST_SRC/libs" -maxdepth 2 -type d -name include)
    fi
    if [ -d "$ME_BOOST_SRC/include" ]; then
        BOOST_INCS+=("-I$ME_BOOST_SRC/include")
    fi
fi

cd "$DIR"
g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -I"$REPO/api" \
    -I"$SRC/include" \
    -I"$SRC/src" \
    -I"$DECIMAL_INC" \
    -I"$POOL_INC" \
    ${BOOST_INCS[@]+"${BOOST_INCS[@]}"} \
    -o "$REPO/cpp_orderbook_adapter.so" \
    "$DIR/cpp_orderbook_adapter.cpp" \
    "$SRC/src/pricelevel.cpp" \
    "$SRC/src/orderqueue.cpp" \
    "$SRC/src/order.cpp" \
    "$SRC/src/types.cpp" \
    -pthread
echo "built: cpp_orderbook_adapter.so"
