#!/usr/bin/env bash
# Build lobster_adapter.so — rubik/lobster behind the harness
# matching_engine_api.h ABI. Pure-Rust cdylib that calls into the lobster
# crate's public OrderBook API. No C++ shim, NO upstream source patch (lobster's
# public API — execute(), min_ask(), max_bid(), depth() — supplies everything
# the harness needs; the engine is conforming as shipped, see
# CORRECTNESS_FINDINGS.md).
#
# Clones rubik/lobster at the pinned commit, then compiles lobster + this
# adapter into a single .so at the harness repo root.
#
# Override the upstream checkout: ME_LOBSTER_SRC=/path/to/existing/clone
# (the engine repo root, i.e. the dir that contains src/orderbook.rs).
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

LOBSTER_URL="https://github.com/rubik/lobster.git"
# Pinned to the SNAPSHOTS.md commit. Reproducible across rebuilds; rerunning
# build.sh on an existing third_party clone hard-resets to this exact SHA.
LOBSTER_REF="0b9720ca1e7dd1f81ecd35d1062c0d3044d5607d"

# ----- Rust toolchain (idempotent) ------------------------------------------
if ! command -v cargo >/dev/null 2>&1; then
    if [ -f "$HOME/.cargo/env" ]; then
        # shellcheck disable=SC1091
        . "$HOME/.cargo/env"
    fi
fi
if ! command -v cargo >/dev/null 2>&1; then
    echo "rustup not found; installing stable toolchain into \$HOME/.cargo"
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
        sh -s -- -y --default-toolchain stable --profile minimal
    # shellcheck disable=SC1091
    . "$HOME/.cargo/env"
fi

# ----- Upstream checkout ----------------------------------------------------
if [ -n "${ME_LOBSTER_SRC:-}" ]; then
    SRC="$ME_LOBSTER_SRC"
else
    SRC="$TP/lobster"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$LOBSTER_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$LOBSTER_REF"
fi

# ----- No source patch ------------------------------------------------------
# lobster is conforming as shipped (CORRECTNESS_FINDINGS.md: "No fix required").
# Nothing to apply here — the checkout above is the engine exactly as upstream
# ships it at the pinned commit.

# ----- Point the wrapper crate at $SRC (if overridden) ----------------------
# wrapper/Cargo.toml commits the default third_party path. If ME_LOBSTER_SRC
# overrides the checkout, swap the lobster path for this build and restore it on
# exit so a follow-up default build keeps working. Pure source-level edit,
# idempotent on rerun.
WRAPPER="$DIR/wrapper"
ORIG_PATH="../../../third_party/lobster"
NEW_PATH="$SRC"
if [ "$NEW_PATH" != "$REPO/third_party/lobster" ]; then
    sed -i "s|path = \"$ORIG_PATH\"|path = \"$NEW_PATH\"|g" "$WRAPPER/Cargo.toml"
    trap 'sed -i "s|path = \"$NEW_PATH\"|path = \"$ORIG_PATH\"|g" "$WRAPPER/Cargo.toml"' EXIT
fi

# ----- Build ----------------------------------------------------------------
# Keep the wrapper's target dir inside the adapter folder so repeated builds
# share the cargo cache and stay reproducible. RUSTFLAGS=-C target-cpu=native is
# the Rust equivalent of g++ -march=native (the default for all C++ adapters in
# this tree).
export CARGO_TARGET_DIR="$WRAPPER/target"
export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
cargo build --release --manifest-path "$WRAPPER/Cargo.toml"

SO_SRC="$CARGO_TARGET_DIR/release/liblobster_adapter.so"
if [ ! -f "$SO_SRC" ]; then
    echo "build produced no .so at $SO_SRC" >&2
    exit 1
fi
cp -f "$SO_SRC" "$REPO/lobster_adapter.so"
echo "built: lobster_adapter.so"
