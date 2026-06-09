#!/usr/bin/env bash
# Build limitbook_adapter.so. Installs a stable Rust toolchain into
# $HOME/.cargo if cargo is not on PATH, clones solarpx/limitbook at a pinned
# commit, then builds a single cdylib via cargo into a stable .so at the
# harness repo root.
#
# Override the upstream checkout: ME_LIMITBOOK_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

LIMITBOOK_URL="https://github.com/solarpx/limitbook.git"
# Pinned commit. Reproducible across rebuilds; rerunning build.sh on an
# existing third_party clone hard-resets to this exact SHA before building.
LIMITBOOK_REF="943eadc181d1e35a26abaa5217eeb32bf3304267"

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
if [ -n "${ME_LIMITBOOK_SRC:-}" ]; then
    SRC="$ME_LIMITBOOK_SRC"
else
    SRC="$TP/limitbook"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$LIMITBOOK_URL" "$SRC"
    fi
    git -C "$SRC" fetch --quiet origin
    # Idempotent: hard-reset to the pinned SHA so a rerun starts clean (no
    # source patch is applied for this engine, but keep the convention).
    git -C "$SRC" reset --hard --quiet "$LIMITBOOK_REF"
fi

# Cargo.toml in the wrapper crate points at ../../../third_party/limitbook via
# a relative path. If ME_LIMITBOOK_SRC overrides that, swap the path
# atomically — pure source-level edit, idempotent on rerun.
WRAPPER="$DIR/wrapper"
ORIG_PATH="../../../third_party/limitbook"
if [ "$SRC" != "$REPO/third_party/limitbook" ]; then
    sed -i "s|path = \"$ORIG_PATH\"|path = \"$SRC\"|g" "$WRAPPER/Cargo.toml"
    trap 'sed -i "s|path = \"$SRC\"|path = \"$ORIG_PATH\"|g" "$WRAPPER/Cargo.toml"' EXIT
fi

# ----- Build ---------------------------------------------------------------
# Keep the wrapper's target dir inside the adapter folder so repeated builds
# share the cargo cache and stay reproducible. RUSTFLAGS=-C target-cpu=native
# is the Rust equivalent of g++ -march=native (the convention for every
# adapter in this tree). No other optimisation flags — limitbook's own
# Cargo.toml declares no [profile.release], so the wrapper adds none.
export CARGO_TARGET_DIR="$WRAPPER/target"
export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
cargo build --release --manifest-path "$WRAPPER/Cargo.toml"

# The cdylib lands at target/release/liblimitbook_adapter.so on Linux.
# Copy to the canonical name the harness expects.
SO_SRC="$CARGO_TARGET_DIR/release/liblimitbook_adapter.so"
if [ ! -f "$SO_SRC" ]; then
    echo "build produced no .so at $SO_SRC" >&2
    exit 1
fi
cp -f "$SO_SRC" "$REPO/limitbook_adapter.so"
echo "built: limitbook_adapter.so"
