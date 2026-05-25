#!/usr/bin/env bash
# Build orderbookrs_adapter.so. Installs a stable Rust toolchain into
# $HOME/.cargo if cargo is not on PATH, clones joaquinbejar/OrderBook-rs at a
# pinned commit, then builds a single cdylib via cargo into a stable .so at
# the harness repo root.
#
# Override the upstream checkout: ME_ORDERBOOKRS_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

ORDERBOOKRS_URL="https://github.com/joaquinbejar/OrderBook-rs.git"
# Pinned to today's main HEAD. Reproducible across rebuilds; rerunning
# build.sh on an existing third_party clone hard-resets to this exact SHA.
ORDERBOOKRS_REF="53b4d2b0a657f4260e316d3a8ac3f0df0fc068bf"

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
if [ -n "${ME_ORDERBOOKRS_SRC:-}" ]; then
    SRC="$ME_ORDERBOOKRS_SRC"
else
    SRC="$TP/OrderBook-rs"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$ORDERBOOKRS_URL" "$SRC"
    fi
    git -C "$SRC" fetch --quiet origin
    git -C "$SRC" reset --hard --quiet "$ORDERBOOKRS_REF"
fi

# Cargo.toml in the wrapper crate points at ../../../third_party/OrderBook-rs
# via a relative path. If ME_ORDERBOOKRS_SRC overrides that, swap the path
# atomically — pure source-level edit, idempotent on rerun.
WRAPPER="$DIR/wrapper"
ORIG_PATH="../../../third_party/OrderBook-rs"
if [ "$SRC" != "$REPO/third_party/OrderBook-rs" ]; then
    # Point Cargo.toml at $SRC for this build.
    sed -i "s|path = \"$ORIG_PATH\"|path = \"$SRC\"|g" "$WRAPPER/Cargo.toml"
    # Restore on exit so a follow-up default build keeps working.
    trap 'sed -i "s|path = \"$SRC\"|path = \"$ORIG_PATH\"|g" "$WRAPPER/Cargo.toml"' EXIT
fi

# ----- Build ---------------------------------------------------------------
# Keep the wrapper's target dir inside the adapter folder so repeated builds
# share the cargo cache and stay reproducible. RUSTFLAGS=-C target-cpu=native
# is the Rust equivalent of g++ -march=native (the default for all C++
# adapters in this tree).
export CARGO_TARGET_DIR="$WRAPPER/target"
export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
cargo build --release --manifest-path "$WRAPPER/Cargo.toml"

# The cdylib lands at target/release/liborderbookrs_adapter.so on Linux.
# Copy to the canonical name the harness expects.
SO_SRC="$CARGO_TARGET_DIR/release/liborderbookrs_adapter.so"
if [ ! -f "$SO_SRC" ]; then
    echo "build produced no .so at $SO_SRC" >&2
    exit 1
fi
cp -f "$SO_SRC" "$REPO/orderbookrs_adapter.so"
echo "built: orderbookrs_adapter.so"
