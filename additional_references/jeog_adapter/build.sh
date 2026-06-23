#!/usr/bin/env bash
# Build jeog_adapter.so. Clones jeog/SimpleOrderbook at a pinned commit and
# compiles the engine's seven library translation units + this adapter into a
# single .so at the harness repo root.
#
# No source patch. The engine matches consensus exactly as shipped (it is
# classified "as shipped" in CORRECTNESS_FINDINGS.md / CONSENSUS_CONFORMING_
# ENGINES.md), so nothing in the upstream tree is modified — the adapter is the
# only glue. The engine's own makefile builds at -std=c++11; we compile at
# -std=c++14 (newer than the engine's floor) so the harness header and adapter
# build cleanly. The library uses std::thread / std::promise / std::future, so
# link -lpthread (the engine's makefile links -lpthread -ldl -lutil; only
# pthread is needed for the matcher path the adapter drives).
#
# Override the upstream checkout: ME_JEOG_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

JEOG_URL="https://github.com/jeog/SimpleOrderbook.git"
JEOG_REF="3411cebb9756b80fd2cb3b442cfb109ca853068b"

if [ -n "${ME_JEOG_SRC:-}" ]; then
    SRC="$ME_JEOG_SRC"
else
    SRC="$TP/jeog_simpleorderbook"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$JEOG_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$JEOG_REF"
fi

# The engine has no submodules and no source patch (it conforms as shipped), so
# the checkout above is build-ready. Sanity-check the layout so a moved/renamed
# upstream fails loud here rather than mid-compile.
for f in include/simpleorderbook.hpp src/simpleorderbook.cpp src/orderbook/core.cpp; do
    [ -f "$SRC/$f" ] || { echo "jeog build.sh: expected source $SRC/$f missing (upstream moved?)" >&2; exit 1; }
done

# C++ toolchain: this repo's C++ adapters use the system g++ (>= C++14 here). No
# toolchain is auto-installed; fail loud if g++ is missing.
command -v g++ >/dev/null 2>&1 || { echo "g++ not found (need a C++14 compiler)" >&2; exit 1; }

cd "$DIR"
g++ -std=c++14 -O3 -march=native -fPIC -shared \
    -I"$SRC/include" \
    -I"$REPO/api" \
    -o "$REPO/jeog_adapter.so" \
    jeog_adapter.cpp \
    "$SRC/src/advanced_order.cpp" \
    "$SRC/src/simpleorderbook.cpp" \
    "$SRC/src/orderbook/core.cpp" \
    "$SRC/src/orderbook/orders.cpp" \
    "$SRC/src/orderbook/objects.cpp" \
    "$SRC/src/orderbook/query.cpp" \
    "$SRC/src/orderbook/advanced.cpp" \
    -lpthread
echo "built: jeog_adapter.so"
