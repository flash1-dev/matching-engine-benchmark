#!/usr/bin/env bash
# Build philipgreat_adapter.so. Installs a stable Rust toolchain into
# $HOME/.cargo if cargo is not on PATH, clones
# philipgreat/lighting-match-engine-core at a pinned commit, patches in a
# one-line library target (the upstream is a binary-only crate), then builds a
# single cdylib via cargo into a stable .so at the harness repo root.
#
# Override the upstream checkout: ME_PHILIPGREAT_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

PHILIPGREAT_URL="https://github.com/philipgreat/lighting-match-engine-core.git"
# Pinned commit. Reproducible across rebuilds; rerunning build.sh on an
# existing third_party clone hard-resets to exactly this SHA before patching.
PHILIPGREAT_REF="381aeda4298524758db37d90c9a69f0fa5c8ca6c"

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
if [ -n "${ME_PHILIPGREAT_SRC:-}" ]; then
    SRC="$ME_PHILIPGREAT_SRC"
else
    SRC="$TP/lighting-match-engine-core"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$PHILIPGREAT_URL" "$SRC"
    fi
    git -C "$SRC" fetch --quiet origin
    git -C "$SRC" reset --hard --quiet "$PHILIPGREAT_REF"
fi

# ----- Source patch: expose a library target --------------------------------
# The upstream crate is binary-only (src/main.rs, no src/lib.rs), so its types
# cannot be linked as a dependency as-is. We add a minimal src/lib.rs that
# re-exports the engine modules the adapter needs (and the modules they depend
# on), mirroring main.rs's module set so the dependency graph resolves
# identically. Idempotent: `git reset --hard` above restores tracked files and
# this heredoc overwrites the untracked lib.rs on every run. Documented in
# README.md.
cat > "$SRC/src/lib.rs" <<'RUST'
//! Library target injected by philipgreat_adapter/build.sh so the binary-only
//! upstream crate can be linked as a dependency. Re-exports the same top-level
//! modules main.rs declares; no engine logic is changed.
pub mod config;
pub mod orderbook;
pub mod protocol;
pub mod stats;
pub mod system;
pub mod timer;
pub mod types;
pub mod utils;
RUST

# ----- Source patch: skip stale orders in the sparse inner match loop -------
# The sparse book prunes cancelled/depleted orders only at a bucket's FRONT,
# once, before its inner matching `while` (in match_buy/match_sell). Inside
# that loop it pops *fully filled* orders but never re-prunes, so a cancelled
# order with remaining_quantity==0 sitting at the front after the order ahead
# of it is consumed gets "matched" for min(taker, 0) == 0 units — emitting a
# phantom zero-quantity Trade (first divergence vs the baseline at seq 3911 in
# the normal workload). The dense book is immune because it prunes the front on
# every iteration. This patch adds the same front-prune inside the sparse inner
# loop. Idempotent: `git reset --hard` above restores the pristine file first;
# the marker guard makes a re-apply a no-op. Documented in README.md.
python3 - "$SRC/src/orderbook/sparse.rs" <<'PY'
import sys
path = sys.argv[1]
src = open(path, encoding="utf-8").read()
marker = "PATCH(philipgreat_adapter): skip cancelled / depleted"
if marker not in src:
    needle = (
        "        while taker.remaining_quantity > 0 && !bucket.orders.is_empty() {\n"
        "            let resting = bucket.orders.front_mut().unwrap();\n"
    )
    repl = (
        "        while taker.remaining_quantity > 0 && !bucket.orders.is_empty() {\n"
        "            // PATCH(philipgreat_adapter): skip cancelled / depleted orders left\n"
        "            // at the bucket front by lazy pruning, so a stale resting order is\n"
        "            // never matched for 0 units (which would emit a phantom\n"
        "            // zero-quantity trade). Mirrors the dense book, which prunes the\n"
        "            // front on every iteration.\n"
        "            Self::prune_bucket_front(bucket, order_map);\n"
        "            if bucket.orders.is_empty() {\n"
        "                break;\n"
        "            }\n"
        "            let resting = bucket.orders.front_mut().unwrap();\n"
    )
    if needle not in src:
        sys.stderr.write("philipgreat patch: anchor not found in sparse.rs (upstream changed?)\n")
        sys.exit(1)
    src = src.replace(needle, repl, 1)
    open(path, "w", encoding="utf-8").write(src)
    print("patched sparse.rs: front-prune in inner match loop")
else:
    print("sparse.rs already patched")
PY

# ----- Source patch: cancel_order must target the ACTIVE instance -----------
# Both books soft-delete on cancel (set remaining=0 + is_cancelled, prune only
# at the front), so a price bucket can briefly hold a dead tombstone AND a live
# order with the same id — which the harness's modify == cancel + reinsert
# rule produces whenever a modify keeps the same price (a quantity increase):
# the modify cancels the original (leaving a non-front tombstone) and reinserts
# under the same id. A later real cancel then runs `find(|o| o.order_id == id)`,
# which returns the *first* match — the tombstone — and zeroes it again,
# leaving the live reinserted order resting forever (first matching-state
# divergence vs baseline at seq 6076 in the normal workload: order 932614,
# modified then cancelled, still matches). Fix: cancel the ACTIVE instance.
# Applies to both sparse.rs and dense.rs. Idempotent (git reset + marker).
python3 - "$SRC/src/orderbook/sparse.rs" "$SRC/src/orderbook/dense.rs" <<'PY'
import sys
needle = "bucket.orders.iter_mut().find(|o| o.order_id == order_id)"
repl   = "bucket.orders.iter_mut().find(|o| o.order_id == order_id && o.is_active())"
for path in sys.argv[1:]:
    src = open(path, encoding="utf-8").read()
    if repl in src:
        print(f"{path}: cancel_order already targets active instance")
        continue
    if needle not in src:
        sys.stderr.write(f"philipgreat patch: cancel anchor not found in {path}\n")
        sys.exit(1)
    src = src.replace(needle, repl, 1)
    open(path, "w", encoding="utf-8").write(src)
    print(f"patched {path}: cancel_order finds the active instance")
PY

# Cargo.toml in the wrapper crate points at ../../../third_party/... via a
# relative path. If ME_PHILIPGREAT_SRC overrides that, swap the path for this
# build and restore it on exit. Pure source-level edit, idempotent on rerun.
WRAPPER="$DIR/wrapper"
ORIG_PATH="../../../third_party/lighting-match-engine-core"
if [ "$SRC" != "$REPO/third_party/lighting-match-engine-core" ]; then
    sed -i "s|path = \"$ORIG_PATH\"|path = \"$SRC\"|g" "$WRAPPER/Cargo.toml"
    trap 'sed -i "s|path = \"$SRC\"|path = \"$ORIG_PATH\"|g" "$WRAPPER/Cargo.toml"' EXIT
fi

# ----- Build ---------------------------------------------------------------
# Keep the wrapper's target dir inside the adapter folder so repeated builds
# share the cargo cache and stay reproducible. RUSTFLAGS=-C target-cpu=native
# is the Rust equivalent of g++ -march=native (the house default for all
# adapters in this tree). House rule: nothing beyond the default release
# profile + target-cpu=native, since the upstream ships no effective
# [profile.release] (see wrapper/Cargo.toml note).
export CARGO_TARGET_DIR="$WRAPPER/target"
export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
cargo build --release --manifest-path "$WRAPPER/Cargo.toml"

# The cdylib lands at target/release/libphilipgreat_adapter.so on Linux.
SO_SRC="$CARGO_TARGET_DIR/release/libphilipgreat_adapter.so"
if [ ! -f "$SO_SRC" ]; then
    echo "build produced no .so at $SO_SRC" >&2
    exit 1
fi
cp -f "$SO_SRC" "$REPO/philipgreat_adapter.so"
echo "built: philipgreat_adapter.so"
