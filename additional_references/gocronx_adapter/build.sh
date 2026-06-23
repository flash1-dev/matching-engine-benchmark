#!/usr/bin/env bash
# Build gocronx_adapter.so — gocronx/matcher (a pure-Rust, single-threaded,
# price-time order book) behind the harness matching_engine_api.h ABI. The
# engine is pure Rust, so the whole adapter is one Rust cdylib that exports the
# harness engine_* extern-C symbols directly and calls into the engine crate.
# No C++ shim.
#
# Clones gocronx/matcher at the pinned commit, applies one minimal idempotent
# engine-source change (a read-only id->(side,price) accessor used only to fill
# the CancelAck wire line — NOT a matching/correctness fix; see the Source patch
# note below and the adapter README), then compiles the engine + this adapter
# into a single .so at the harness repo root.
#
# Override the upstream checkout: ME_GOCRONX_SRC=/path/to/existing/clone
# (the engine repo root, i.e. the dir that contains src/book/mod.rs).
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

GOCRONX_URL="https://github.com/gocronx/matcher.git"
# Pinned to the SNAPSHOTS.md commit. Reproducible across rebuilds; rerunning
# build.sh on an existing third_party clone hard-resets to this exact SHA so the
# accessor patch below always lands on pristine source.
GOCRONX_REF="b8d48356c8a2677e0d8a1965d754e3c4884bb947"

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
if [ -n "${ME_GOCRONX_SRC:-}" ]; then
    SRC="$ME_GOCRONX_SRC"
else
    SRC="$TP/gocronx-matcher"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$GOCRONX_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$GOCRONX_REF"
fi

# ----- Engine-source accessor (adapter support, NOT a correctness fix) -------
# The harness CancelAck wire line is `2,seq,side,order_id,price_ticks`, so a
# successful cancel must echo the resting order's side AND price. The engine's
# public cancel API (`cancel_events`) returns only `Canceled{order_id,
# remaining, ts}` — no side, no price — and the engine ships NO public
# id->order getter (its `orders` map is `pub(super)`). Rather than duplicate the
# book in an adapter-side shadow (which could mask or invent an engine bug), we
# expose a tiny read-only accessor that reads the engine's own authoritative
# `orders` map. It does NOT change matching, book state, or any existing
# behavior — gocronx is consensus-conforming as shipped (CORRECTNESS_FINDINGS.md:
# "No fix required"); this is purely an adapter-visibility shim.
#
# Applied post-reset with an anchored Python replace + a marker guard, so an
# upstream change fails the build loudly instead of silently shipping the wrong
# code, and a re-apply (e.g. under an ME_GOCRONX_SRC override) is a no-op.
# Idempotent: the `git reset --hard` above restores pristine source each run on
# the default checkout.
python3 - "$SRC/src/book/mod.rs" <<'PYEOF'
import sys
path = sys.argv[1]
src = open(path).read()
if "get_order_se" in src:
    print("accessor already present; skipping")
    sys.exit(0)
anchor = "    pub fn best_ask(&self) -> Option<Price> {\n        self.best_ask\n    }\n"
if anchor not in src:
    sys.exit("gocronx build.sh accessor patch: anchor (best_ask getter) not found in src/book/mod.rs (upstream changed?)")
inject = anchor + (
    "\n"
    "    /// Read-only (side, price) of a resting order by id, or None if no\n"
    "    /// such order is on the book. Added for the matching-engine harness\n"
    "    /// adapter: the CancelAck wire line echoes the resting order's side\n"
    "    /// and price, which `cancel_events` omits. Pure read of `self.orders`\n"
    "    /// — no matching, no mutation.\n"
    "    pub fn get_order_se(&self, id: impl Into<OrderId>) -> Option<(Side, Price)> {\n"
    "        self.orders.get(&id.into()).map(|o| (o.side, o.price))\n"
    "    }\n"
)
src = src.replace(anchor, inject, 1)
open(path, "w").write(src)
print("patched src/book/mod.rs: OrderBook::get_order_se accessor added")
PYEOF
# Hard-verify the accessor landed so a future tooling change can never quietly
# break the CancelAck side/price.
if ! grep -q "pub fn get_order_se" "$SRC/src/book/mod.rs"; then
    echo "src/book/mod.rs patch applied but get_order_se is absent — refusing to ship" >&2
    exit 1
fi

# ----- Point the wrapper crate at $SRC (if overridden) ----------------------
# wrapper/Cargo.toml commits the default third_party path. If ME_GOCRONX_SRC
# overrides the checkout, swap the matcher path for this build and restore it on
# exit so a follow-up default build keeps working. Pure source-level edit,
# idempotent on rerun.
WRAPPER="$DIR/wrapper"
ORIG_PATH="../../../third_party/gocronx-matcher"
NEW_PATH="$SRC"
if [ "$NEW_PATH" != "$REPO/third_party/gocronx-matcher" ]; then
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

SO_SRC="$CARGO_TARGET_DIR/release/libgocronx_adapter.so"
if [ ! -f "$SO_SRC" ]; then
    echo "build produced no .so at $SO_SRC" >&2
    exit 1
fi
cp -f "$SO_SRC" "$REPO/gocronx_adapter.so"
echo "built: gocronx_adapter.so"
