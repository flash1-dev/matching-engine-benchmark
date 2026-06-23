#!/usr/bin/env bash
# Build asthamishra_adapter.so — AsthaMishra/matching-engine behind the harness
# matching_engine_api.h ABI. Pure-Rust cdylib that calls into the engine's
# matching-core crate (the only leaf crate needed; the workspace's tokio/
# crossbeam/axum gateway crates are NOT built).
#
# Clones AsthaMishra/matching-engine at the pinned commit, applies a one-
# constant engine-source correctness fix (widen the direct-indexed price array
# so wide-swing orders above the 100k-tick ceiling are no longer dropped — see
# the Source patch note below and https://github.com/AsthaMishra/matching-engine/issues/1),
# then compiles matching-core + this adapter into a single .so at the harness
# repo root.
#
# Override the upstream checkout: ME_ASTHAMISHRA_SRC=/path/to/existing/clone
# (the engine repo root, i.e. the dir that contains matching-core/).
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

ASTHA_URL="https://github.com/AsthaMishra/matching-engine.git"
# Pinned to the SNAPSHOTS.md commit. Reproducible across rebuilds; rerunning
# build.sh on an existing third_party clone hard-resets to this exact SHA so the
# correctness patch below always lands on pristine source.
ASTHA_REF="317c092843d3a5cc6730ceed6c56bb5598ab8fb7"

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
if [ -n "${ME_ASTHAMISHRA_SRC:-}" ]; then
    SRC="$ME_ASTHAMISHRA_SRC"
else
    SRC="$TP/asthamishra-matching-engine"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$ASTHA_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$ASTHA_REF"
fi

# ----- Engine-source correctness fix: widen the price domain ----------------
# Correctness fix (not performance). The order book is a direct-indexed price
# array: a tick IS the array index, sized MAX_PRICE / TICK_SIZE slots, and
# price_to_idx() REJECTS any price >= MAX_PRICE (utils.rs). MAX_PRICE = 100_000
# (TICK_SIZE = 1), so any order priced at or above 100,000 ticks is dropped
# before it enters the book — no rest, no trace — and a later cancel/modify of
# that id then rejects with OrderIdNotFound. The benchmark's wide-swing tapes
# breach the ceiling (the flash-crash scenario reaches ~153k ticks on some
# seeds), so the engine silently under-fills and its report stream diverges
# from consensus (https://github.com/AsthaMishra/matching-engine/issues/1).
#
# The fix is in ENGINE source, not an adapter workaround (the adapter must use
# the engine's own id index as its liveness/reject oracle, so the engine has to
# actually hold every order). It is the issue's recommended option 1 — widen the
# domain — applied as a single constant: MAX_PRICE 100_000 -> 2_000_000 (the
# array grows to 2,000,000 slots, ~160 MB for the single benchmark book). 2M
# ticks ($20,000 at TICK_SIZE = 1 cent) is ~13x the worst tick observed across
# the benchmark tapes, covering every scenario/seed with headroom while keeping
# the engine's O(1) direct indexing. TICK_SIZE, the array-sizing expression
# (slots = MAX_PRICE / TICK_SIZE), and the price_to_idx bound all key off this
# one constant, so nothing else changes; the price_to_idx <= 0 / out-of-domain
# rejections are KEPT (genuinely out-of-range prices still reject distinctly).
#
# Applied with an anchored Python replace + a marker guard so an upstream change
# fails loud instead of silently shipping the bug, and a re-apply (e.g. under an
# ME_ASTHAMISHRA_SRC override) is a no-op. Idempotent: the git reset --hard above
# restores pristine source each run on the default checkout.
python3 - "$SRC/matching-core/src/utils.rs" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
marker = "asthamishra_adapter: widen the direct-indexed price domain"
if marker in s:
    print("utils.rs already patched (widened MAX_PRICE)")
    sys.exit(0)
needle = "pub const MAX_PRICE: i64 = 100_000; // $1,000 in cents - array has MAX_PRICE / TICK_SIZE slots\n"
repl = (
    "// asthamishra_adapter: widen the direct-indexed price domain so wide-swing\n"
    "// orders above the original 100k-tick ceiling are no longer dropped before\n"
    "// they enter the book. See https://github.com/AsthaMishra/matching-engine/issues/1.\n"
    "pub const MAX_PRICE: i64 = 2_000_000; // $20,000 in cents - array has MAX_PRICE / TICK_SIZE slots\n"
)
if needle not in s:
    sys.exit("asthamishra build.sh widen-MAX_PRICE patch: anchor not found in utils.rs (upstream changed?)")
s = s.replace(needle, repl, 1)
open(p, "w").write(s)
print("patched utils.rs: MAX_PRICE 100_000 -> 2_000_000 (price array widened)")
PYEOF
# Hard-verify the fix landed so a future tooling change can never quietly ship
# the bounded engine.
if ! grep -q "pub const MAX_PRICE: i64 = 2_000_000;" "$SRC/matching-core/src/utils.rs"; then
    echo "utils.rs patch applied but the widened MAX_PRICE is absent — refusing to ship" >&2
    exit 1
fi

# ----- Point the wrapper crate at $SRC (if overridden) ----------------------
# wrapper/Cargo.toml commits the default third_party path. If ME_ASTHAMISHRA_SRC
# overrides the checkout, swap the matching-core path for this build and restore
# it on exit so a follow-up default build keeps working. Pure source-level edit,
# idempotent on rerun.
WRAPPER="$DIR/wrapper"
ORIG_PATH="../../../third_party/asthamishra-matching-engine/matching-core"
NEW_PATH="$SRC/matching-core"
if [ "$NEW_PATH" != "$REPO/third_party/asthamishra-matching-engine/matching-core" ]; then
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

SO_SRC="$CARGO_TARGET_DIR/release/libasthamishra_adapter.so"
if [ ! -f "$SO_SRC" ]; then
    echo "build produced no .so at $SO_SRC" >&2
    exit 1
fi
cp -f "$SO_SRC" "$REPO/asthamishra_adapter.so"
echo "built: asthamishra_adapter.so"
