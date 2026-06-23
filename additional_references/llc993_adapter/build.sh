#!/usr/bin/env bash
# Build llc993_adapter.so — llc-993/matching-core behind the harness
# matching_engine_api.h ABI. Pure-Rust cdylib that calls into the engine's
# `matching-core` crate.
#
# Clones llc-993/matching-core at the pinned commit and compiles it together
# with this adapter into a single .so at the harness repo root. The engine
# builds and runs as-is — there is NO engine-source patch (see "Source patch"
# below and the engine's verdict in CONSENSUS_CONFORMING_ENGINES.md /
# CORRECTNESS_FINDINGS.md: conforming as shipped, no fix required).
#
# Override the upstream checkout: ME_LLC993_SRC=/path/to/existing/clone
# (the engine repo root, i.e. the dir that contains src/ and Cargo.toml).
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

LLC993_URL="https://github.com/llc-993/matching-core.git"
# Pinned to the SNAPSHOTS.md commit. Reproducible across rebuilds; rerunning
# build.sh on an existing third_party clone hard-resets to this exact SHA.
LLC993_REF="2cb21c0a67b34b01ad97e2394a649fc77e33aa8b"

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
if [ -n "${ME_LLC993_SRC:-}" ]; then
    SRC="$ME_LLC993_SRC"
else
    SRC="$TP/llc993_matching_core"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$LLC993_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$LLC993_REF"
fi

# ----- Source patch ---------------------------------------------------------
# None. The engine compiles and runs against the harness as shipped at the
# pinned commit; the harness ABI is satisfied entirely by the wrapper crate.
# llc-993/matching-core's verdict in this repo is "conforming as shipped — no
# fix required" (DirectOrderBook: BTreeMap + slab pool + intrusive time-queue,
# exchange-core-inspired). So there is nothing to apply here, and no upstream
# issue to cite. (If an upstream change ever broke the API the adapter binds to,
# the build below would fail to compile — loud, not silent.)

# ----- Point the wrapper crate at $SRC (if overridden) ----------------------
# wrapper/Cargo.toml commits the default third_party path. If ME_LLC993_SRC
# overrides the checkout, swap the matching-core path for this build and restore
# it on exit so a follow-up default build keeps working. Pure source-level edit,
# idempotent on rerun.
WRAPPER="$DIR/wrapper"
ORIG_PATH="../../../third_party/llc993_matching_core"
NEW_PATH="$SRC"
if [ "$NEW_PATH" != "$TP/llc993_matching_core" ]; then
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

SO_SRC="$CARGO_TARGET_DIR/release/libllc993_adapter.so"
if [ ! -f "$SO_SRC" ]; then
    echo "build produced no .so at $SO_SRC" >&2
    exit 1
fi
cp -f "$SO_SRC" "$REPO/llc993_adapter.so"
echo "built: llc993_adapter.so"
