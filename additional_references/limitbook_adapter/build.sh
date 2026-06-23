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
    # Idempotent: hard-reset to the pinned SHA so a rerun starts clean before
    # the source patch below is applied.
    git -C "$SRC" reset --hard --quiet "$LIMITBOOK_REF"
fi

# ----- Source patch: write back the resting order's reduced size on a partial
# ----- fill (over-matching correctness fix) ---------------------------------
# Bug: a resting order that takes a PARTIAL fill is never shrunk, so it keeps
# claiming its original size and can be matched again and again until one fill
# happens to equal that original size — the book hands out more shares than
# ever rested. In each of the three matching loops (buy-side and sell-side in
# add_limit_order, plus execute_market_order) `resting_order` is a front_mut()
# `&mut Order` whose `quantity` is decremented from `remaining_quantity`,
# `orders.total_volume` and the book volume counter, but never written back to
# the maker itself. The removal test `if fill_quantity == resting_order.quantity`
# therefore stays false on a partial fill and the maker sits at the queue front
# at full size while the cached level/book counters drift below it. On the
# `normal` benchmark workload this over-matches ~4.3x (268,154 trades vs the
# 62,474-trade 3-baseline consensus) and fails the trade-stream hash on all
# five scenarios. Fix: on a partial fill, decrement the maker's own quantity;
# only pop the order on full consumption. This is exactly the patch proposed in
# the upstream bug report (https://github.com/solarpx/limitbook/issues/1; see
# CORRECTNESS_FINDINGS.md at the repo root). `resting_order` is
# already `&mut Order` and `Order::quantity` is a public Decimal, so the change
# is a single `else` branch added at each of the three sites. Idempotent: the
# `git reset --hard` above restores pristine sources each rerun, and the marker
# guard makes a re-apply a no-op; fails loud if the three sites are not found.
python3 - "$SRC/src/order_book.rs" <<'PY'
import sys, re
path = sys.argv[1]
src = open(path, encoding="utf-8").read()
marker = "PATCH(limitbook_adapter): write back partial-fill remainder"
if marker in src:
    print(f"{path} already patched (partial-fill write-back)")
    sys.exit(0)
# The pristine removal block appears identically at all three matching sites,
# at two indentation depths (the two add_limit_order loops are nested one level
# deeper than execute_market_order). Match the block while CAPTURING its indent
# so the injected `else` lines line up at whatever depth each site uses.
pat = re.compile(
    r"^(?P<ind>[ \t]*)if fill_quantity == resting_order\.quantity \{\n"
    r"(?P=ind)    let removed_order = orders\.orders\.pop_front\(\)\.unwrap\(\);\n"
    r"(?P=ind)    orders\.order_count -= 1;\n"
    r"(?P=ind)    self\.order_lookup\.remove\(&removed_order\.id\);\n"
    r"(?P=ind)\}\n",
    re.MULTILINE,
)
def replacement(m):
    ind = m.group("ind")
    return (
        f"{ind}if fill_quantity == resting_order.quantity {{\n"
        f"{ind}    let removed_order = orders.orders.pop_front().unwrap();\n"
        f"{ind}    orders.order_count -= 1;\n"
        f"{ind}    self.order_lookup.remove(&removed_order.id);\n"
        f"{ind}}} else {{\n"
        f"{ind}    // PATCH(limitbook_adapter): write back partial-fill remainder so a\n"
        f"{ind}    // partly-filled resting order shrinks instead of being matchable at\n"
        f"{ind}    // its original size (over-matching fix;\n"
        f"{ind}    // https://github.com/solarpx/limitbook/issues/1).\n"
        f"{ind}    resting_order.quantity -= fill_quantity;\n"
        f"{ind}}}\n"
    )
new, n = pat.subn(replacement, src)
if n != 3:
    sys.stderr.write(
        f"limitbook patch: expected 3 removal blocks in order_book.rs, found {n} "
        "(upstream changed?)\n"
    )
    sys.exit(1)
open(path, "w", encoding="utf-8").write(new)
print(f"patched {path}: partial-fill write-back at all 3 matching sites")
PY

# ----- Source patch: relax the now-inaccurate Order doc-comment -------------
# The fix above mutates Order::quantity as a resting order fills, so the
# "All fields are immutable after creation" line in order.rs is no longer true.
# Cosmetic only (a doc comment), but kept accurate. Idempotent (git reset +
# exact-string no-op on rerun).
python3 - "$SRC/src/order.rs" <<'PY'
import sys
path = sys.argv[1]
src = open(path, encoding="utf-8").read()
old = "/// All fields are immutable after creation to maintain order integrity."
new = ("/// `id`, `order_type` and `order_side` are set at creation; `quantity` is set\n"
       "/// at creation and reduced as the order is filled.")
if old in src:
    open(path, "w", encoding="utf-8").write(src.replace(old, new, 1))
    print(f"patched {path}: relaxed immutable-fields doc comment")
else:
    print(f"{path}: doc comment already relaxed")
PY

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
