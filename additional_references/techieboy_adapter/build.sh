#!/usr/bin/env bash
# Build techieboy_adapter.so — TechieBoy/rust-orderbook behind the harness
# matching_engine_api.h ABI. Pure-Rust cdylib that calls into the engine crate
# (crate `orderbook`, lib `orderbooklib`) by path. No C++ shim.
#
# Clones TechieBoy/rust-orderbook at the pinned commit into
# third_party/techieboy_rust_orderbook, hard-resets the engine source to that
# SHA (so the patch always lands on pristine source), applies a small documented
# engine-source patch (patch_engine.py — caller-supplied order ids, per-maker
# fill capture, public read accessors, plus two genuine correctness bug fixes;
# the matching algorithm is otherwise untouched — see the Source patch note in
# README.md and https://github.com/TechieBoy/rust-orderbook/issues/1), then
# compiles the engine + this adapter into a single .so at the harness repo root.
#
# Override the upstream checkout: ME_TECHIEBOY_SRC=/path/to/existing/clone (the
# engine repo root, i.e. the dir that contains src/lib.rs and Cargo.toml). The
# engine source there is hard-reset to the pin and re-patched on every run, so an
# override checkout that is already patched (or dirty) is restored first.
#
# Rerunning is safe: the reset-to-pin + idempotent anchored patch restore
# pristine upstream before re-patching.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

NAME="techieboy"
TB_URL="https://github.com/TechieBoy/rust-orderbook.git"
# Pinned to the SNAPSHOTS.md commit. Reproducible across rebuilds.
TB_REF="468fef7fb86c6191d8a2fb4c4ad1d9fb88ec0a26"

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
if [ -n "${ME_TECHIEBOY_SRC:-}" ]; then
    SRC="$ME_TECHIEBOY_SRC"
else
    SRC="$TP/techieboy_rust_orderbook"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$TB_URL" "$SRC"
    fi
fi
# Restore pristine engine source at the pin before patching. This is essential:
# patch_engine.py anchors on exact upstream text and asserts each replacement
# matches exactly once, so it MUST land on the unpatched source. The reset also
# makes a rerun (or an already-patched override checkout) idempotent.
git -C "$SRC" reset --hard --quiet "$TB_REF"

# ----- Engine-source patch (caller ids + per-maker fills + accessors + fixes) -
# patch_engine.py is self-checking: each replacement asserts it matched exactly
# once (a hard failure if upstream text drifts), and it re-applies cleanly only
# on pristine source — which the reset above guarantees. The two correctness
# fixes (spurious zero-quantity fills on maker-list exhaustion; stale best-bid/
# ask after a side empties or a cancel) are documented in README.md and filed as
# https://github.com/TechieBoy/rust-orderbook/issues/1.
python3 "$DIR/patch_engine.py" "$SRC/src/lib.rs"
# Hard-verify the patch landed so a future tooling change can never quietly ship
# the unpatched engine (the caller-id signature is the load-bearing API change).
if ! grep -q "fn add_limit_order(&mut self, s: Side, price: u64, order_qty: u64, order_id: u64)" "$SRC/src/lib.rs"; then
    echo "engine patch applied but the caller-id add_limit_order signature is absent — refusing to ship" >&2
    exit 1
fi

# ----- Point the wrapper crate at $SRC (if overridden) ----------------------
# wrapper/Cargo.toml commits the default third_party path. If ME_TECHIEBOY_SRC
# overrides the checkout, swap the engine path for this build and restore it on
# exit so a follow-up default build keeps working. Pure source-level edit,
# idempotent on rerun.
WRAPPER="$DIR/wrapper"
ORIG_PATH="../../../third_party/techieboy_rust_orderbook"
NEW_PATH="$SRC"
if [ "$NEW_PATH" != "$TP/techieboy_rust_orderbook" ]; then
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

SO_SRC="$CARGO_TARGET_DIR/release/lib${NAME}_adapter.so"
if [ ! -f "$SO_SRC" ]; then
    echo "build produced no .so at $SO_SRC" >&2
    exit 1
fi
cp -f "$SO_SRC" "$REPO/${NAME}_adapter.so"
echo "built: ${NAME}_adapter.so"
