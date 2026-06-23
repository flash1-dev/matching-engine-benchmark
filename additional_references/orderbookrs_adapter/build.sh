#!/usr/bin/env bash
# Build orderbookrs_adapter.so. Installs a stable Rust toolchain into
# $HOME/.cargo if cargo is not on PATH, clones joaquinbejar/OrderBook-rs at a
# pinned commit, applies the engine-source price-time-priority fix to the
# `pricelevel` crate OrderBook-rs matches in (see below), then builds a single
# cdylib via cargo into a stable .so at the harness repo root.
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

# pricelevel is a crates.io dependency (pricelevel = "0.7"); the engine fix
# lands in it and is pinned to this exact published version so the patch hunks
# always match.
PRICELEVEL_VER="0.7.0"

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

# ----- Engine-source patch: price-time priority across partial fills --------
# Correctness fix (not performance). OrderBook-rs does its actual per-price-
# level matching in the `pricelevel` crate (pricelevel = "0.7"). On a partial
# fill, PriceLevel::match_order re-queues the maker's residual with
# `self.orders.push(...)`, which lands it at the TAIL of the level's FIFO — so
# a same-price order that arrived AFTER the original maker is matched ahead of
# the maker's remainder on the next trade at that price. That is a price-time-
# priority violation: total resting quantity at the level stays correct, but
# the maker_order_id of subsequent same-price fills is wrong, which fails the
# benchmark report-stream hash on all five scenarios. Filed upstream as
# https://github.com/joaquinbejar/OrderBook-rs/issues/88 (see CORRECTNESS_FINDINGS.md).
#
# The fix is in engine (crate) source, NOT an adapter workaround: pricelevel-
# pricetime.patch gives OrderQueue a deque-like backing (Mutex<VecDeque<Id>>
# instead of the tail-only SegQueue<Id>), adds a push_front primitive, and
# re-queues the partial-fill residual at the FRONT so the head keeps its time
# priority. (The unrelated tail push in update_order — explicit modify — is
# left as push; modify-to-back is correct.)
#
# Because pricelevel comes from crates.io and is used by BOTH this wrapper's
# direct dep AND OrderBook-rs's transitive dep, we cannot patch a single clone
# the way the C++ adapters patch their one engine checkout. Instead we
# materialize a pristine pricelevel-${PRICELEVEL_VER} source tree under
# wrapper/pricelevel-patched (git-ignored), apply the patch to it, and the
# wrapper Cargo.toml's [patch.crates-io] substitutes that patched copy for the
# crates.io pricelevel everywhere (cargo tree -i pricelevel resolves to a single
# patched node). This survives any `git reset --hard` of the engine clone (the
# fix lives outside it) and a fresh checkout (the tree is rebuilt every run).
PATCH_FILE="$DIR/pricelevel-pricetime.patch"
PL_DST="$DIR/wrapper/pricelevel-patched"

# Always rebuild the patched tree from pristine so the patch applies exactly
# once against unmodified source (mirrors the `git reset --hard` the other
# adapters rely on). Ensure the crate is in the local registry cache first; a
# throwaway manifest makes `cargo fetch` download pricelevel-${PRICELEVEL_VER}
# (no-op if already cached) without involving the wrapper manifest, whose
# [patch.crates-io] points at the dir we are about to (re)create.
FETCH_TMP="$DIR/wrapper/.pricelevel-fetch"
rm -rf "$PL_DST" "$FETCH_TMP"
mkdir -p "$FETCH_TMP/src"
cat > "$FETCH_TMP/Cargo.toml" <<TOML
[package]
name = "pricelevel-fetch"
version = "0.0.0"
edition = "2021"
publish = false

[dependencies]
pricelevel = "=${PRICELEVEL_VER}"
TOML
echo 'fn main() {}' > "$FETCH_TMP/src/main.rs"
cargo fetch --quiet --manifest-path "$FETCH_TMP/Cargo.toml"
rm -rf "$FETCH_TMP"

# Locate the freshly-fetched .crate tarball (registry hash dir is environment-
# specific, so glob it) and extract a pristine tree.
PL_CRATE="$(find "${CARGO_HOME:-$HOME/.cargo}/registry/cache" \
    -name "pricelevel-${PRICELEVEL_VER}.crate" 2>/dev/null | head -n1)"
if [ -z "$PL_CRATE" ]; then
    echo "could not find pricelevel-${PRICELEVEL_VER}.crate in the cargo registry cache" >&2
    exit 1
fi
mkdir -p "$PL_DST"
tar -xzf "$PL_CRATE" -C "$PL_DST" --strip-components=1

# Apply the price-time-priority fix with `patch -p1 -d` (not `git apply
# --directory`, whose path handling silently no-ops when the target sits inside
# another git work tree). --dry-run first so an upstream version drift (hunks no
# longer matching) fails loud instead of silently shipping the bug.
if ! patch -p1 -d "$PL_DST" --dry-run < "$PATCH_FILE" >/dev/null 2>&1; then
    echo "pricelevel-pricetime.patch did not apply cleanly to pricelevel-${PRICELEVEL_VER}" >&2
    echo "(the pinned crate version may have changed; refresh the patch)" >&2
    exit 1
fi
patch -p1 -d "$PL_DST" < "$PATCH_FILE" >/dev/null
# Hard-verify the fix actually landed (the residual is re-queued at the FRONT),
# so a future change to the patch tooling can never quietly ship the buggy crate.
if ! grep -q 'push_front(Arc::new(updated))' "$PL_DST/src/price_level/level.rs"; then
    echo "pricelevel patch applied but the front-requeue fix is absent — refusing to ship" >&2
    exit 1
fi
echo "patched pricelevel-${PRICELEVEL_VER}: partial-fill residual keeps time priority"

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
