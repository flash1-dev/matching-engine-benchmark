#!/usr/bin/env bash
# Build gotrader_adapter.so. Installs a Go toolchain (user-local, no sudo) if one
# is not already on PATH, clones robaho/go-trader at a pinned commit, drops in the
# adapter's two added Go files (an exported shim over the engine's unexported
# matcher + the cgo //export wrapper), and builds a Go cgo c-shared library at the
# harness repo root.
#
# go-trader (https://github.com/robaho/go-trader) is a FIX/gRPC exchange, but its
# limit-order book (internal/exchange: orderBook.add / .remove / matchTrades) is a
# separable in-process unit. This build drives that book directly via cgo
# c-shared — no FIX/gRPC/QuickFIX runtime is stood up. orderBook, sessionOrder,
# add, and remove are all UNEXPORTED, so the adapter cannot reach them from an
# out-of-package main; both added files therefore live INSIDE the cloned engine
# module (a new file in package exchange + a new command), which is the only way
# an in-process Go caller can drive the matcher.
#
# Override the upstream checkout: ME_GOTRADER_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

GOTRADER_URL="https://github.com/robaho/go-trader.git"
GOTRADER_REF="1d34bc8206d7931939e02142f582a0a009b1da3b"

# ---------------------------------------------------------------------------
# Toolchain: install a Go SDK under third_party/ if one is not already on PATH.
# ---------------------------------------------------------------------------
GO_VERSION="1.23.4"
GO_PREFIX="$TP/go-${GO_VERSION}"
GO_BIN="$GO_PREFIX/go/bin/go"

if ! command -v go >/dev/null 2>&1; then
    if [ ! -x "$GO_BIN" ]; then
        case "$(uname -m)" in
            aarch64|arm64) GO_ARCH=arm64 ;;
            x86_64|amd64)  GO_ARCH=amd64 ;;
            *) echo "build.sh: unsupported arch $(uname -m)" >&2; exit 1 ;;
        esac
        TARBALL="go${GO_VERSION}.linux-${GO_ARCH}.tar.gz"
        mkdir -p "$GO_PREFIX"
        echo "build.sh: installing Go ${GO_VERSION} into $GO_PREFIX"
        curl -fsSL -o "$GO_PREFIX/$TARBALL" "https://go.dev/dl/${TARBALL}"
        tar -C "$GO_PREFIX" -xzf "$GO_PREFIX/$TARBALL"
        rm -f "$GO_PREFIX/$TARBALL"
    fi
    export PATH="$GO_PREFIX/go/bin:$PATH"
fi

go version >/dev/null

# Keep build state under third_party/ so reruns don't pollute $HOME caches.
export GOCACHE="$TP/go-cache"
export GOMODCACHE="$TP/go-modcache"
mkdir -p "$GOCACHE" "$GOMODCACHE"

# ---------------------------------------------------------------------------
# Upstream: clone or use ME_GOTRADER_SRC, then reset to the pin.
# ---------------------------------------------------------------------------
if [ -n "${ME_GOTRADER_SRC:-}" ]; then
    SRC="$ME_GOTRADER_SRC"
else
    SRC="$TP/go_trader_src"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$GOTRADER_URL" "$SRC"
    fi
fi
git -C "$SRC" reset --hard --quiet "$GOTRADER_REF"
git -C "$SRC" clean -fdq -e go.sum 2>/dev/null || true

# ---------------------------------------------------------------------------
# Add the adapter's two Go files INTO the engine module.
#
# Neither file PATCHES the matcher — both are pure ADDITIONS in their own
# locations, so the `git reset --hard` above (or a fresh clone) removes them and
# this copy restores them each run (idempotent). The matcher source stays
# pristine at the pin; every cross/fill/removal the adapter produces is the
# engine's own.
#
#   - internal/exchange/me_shim.go : exported shim over the unexported matcher
#     (package exchange). Carries NO matching logic. It also supplies the
#     CONFORMANCE FIX for go-trader #23 — see README.md "Source patch": a modify
#     of a fully-filled order is rejected (the shim exposes the order's own
#     IsActive() state through MeIsActive; the wrapper's doModify gates on it,
#     symmetric with the cancel path), rather than swallow-ack'd.
#   - cmd/meadapter/wrapper.go     : package main + //export cgo ABI. Same module
#     as internal/exchange, so the internal import is allowed.
# ---------------------------------------------------------------------------
mkdir -p "$SRC/internal/exchange" "$SRC/cmd/meadapter"
cp "$DIR/internal/exchange/me_shim.go" "$SRC/internal/exchange/me_shim.go"
cp "$DIR/cmd/meadapter/wrapper.go"     "$SRC/cmd/meadapter/wrapper.go"

# Anchor check: confirm the conformance fix is present in the shipped shim
# (loud-fail if the source drifts so the fix can't silently vanish).
grep -q "func (b \*MeBook) MeIsActive" "$SRC/internal/exchange/me_shim.go" \
    || { echo "build.sh: MeIsActive gate (go-trader #23 fix) missing from me_shim.go" >&2; exit 1; }
grep -q "MeIsActive(int64(oid))" "$SRC/cmd/meadapter/wrapper.go" \
    || { echo "build.sh: doModify does not consult MeIsActive (go-trader #23 fix)" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Build the cgo c-shared library from inside the engine module. -mod=mod lets
# the build resolve the engine's own go.sum-pinned deps from the module cache.
# ---------------------------------------------------------------------------
OUT="$REPO/gotrader_adapter.so"
cd "$SRC"
export GOFLAGS=-mod=mod
export CGO_ENABLED=1

go build -buildmode=c-shared \
    -ldflags="-s -w" \
    -o "$OUT" \
    ./cmd/meadapter

# The cgo build also drops a header next to the .so; the harness doesn't use it.
rm -f "$REPO/gotrader_adapter.h" "$SRC/cmd/meadapter/meadapter.h"

echo "built: gotrader_adapter.so"
