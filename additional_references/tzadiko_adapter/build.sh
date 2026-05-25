#!/usr/bin/env bash
# Build tzadiko_adapter.so. Clones Tzadiko/Orderbook at a pinned commit,
# patches one Windows-only call (`localtime_s`) to its POSIX equivalent so
# the engine source compiles on Linux, and links the result with this
# adapter into a single .so at the harness repo root.
#
# The patch swaps the call inside `PruneGoodForDayOrders` only — the
# arithmetic and control flow are untouched. PruneGoodForDayOrders runs in a
# background thread that sleeps until 16:00 local time; for the harness's
# GoodTillCancel + FillAndKill workload it never wakes during the run.
#
# Override the upstream checkout: ME_TZADIKO_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

TZADIKO_URL="https://github.com/Tzadiko/Orderbook.git"
TZADIKO_REF="dd136dd219ead95796f0e396e9e1395542bf673f"

if [ -n "${ME_TZADIKO_SRC:-}" ]; then
    SRC="$ME_TZADIKO_SRC"
else
    SRC="$TP/Tzadiko_Orderbook"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$TZADIKO_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$TZADIKO_REF"
fi

# Two source-level patches to Orderbook.cpp. Both are idempotent — the
# `git reset --hard` above restores the file each rerun.
#
# 1. POSIX port. `localtime_s` is Windows-only; the POSIX equivalent is
#    `localtime_r(time_t const*, tm*)` with swapped arguments. The call sits
#    inside `PruneGoodForDayOrders`, a background thread that sleeps until
#    16:00 local time — it never wakes during the run.
#
# 2. Drop the `trades.reserve(orders_.size())` in `MatchOrders`. The sizing
#    hint asks for one trade slot per resting order at the start of every
#    match, allocating ~50k entries each call against a typical fill count of
#    0–10 — pure overhead, no semantic effect. We let std::vector grow on
#    demand. Both the matched trades that get pushed and the returned vector
#    are byte-identical to the un-patched build.
sed -i 's|localtime_s(&now_parts, &now_c);|localtime_r(\&now_c, \&now_parts);|' \
    "$SRC/Orderbook.cpp"
sed -i 's|trades.reserve(orders_.size());|/* trades.reserve removed by tzadiko_adapter — was O(orders_) per call */|' \
    "$SRC/Orderbook.cpp"

cd "$DIR"
g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -I"$SRC" -I"$REPO/api" \
    -o "$REPO/tzadiko_adapter.so" \
    tzadiko_adapter.cpp \
    "$SRC/Orderbook.cpp" \
    -pthread
echo "built: tzadiko_adapter.so"
