#!/usr/bin/env bash
# Build femtogo_adapter.so. Installs a Go toolchain (user-local, no sudo) if one
# is not already on PATH, clones ejyy/femto_go at a pinned commit, copies its
# engine sources into the cgo wrapper package (femto_go is `package main` with
# unexported types/fields, so it is copied in rather than imported), and builds
# a Go cgo c-shared library at the harness repo root.
#
# Override the upstream checkout: ME_FEMTOGO_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

FEMTOGO_URL="https://github.com/ejyy/femto_go.git"
FEMTOGO_REF="46667a95064bd028e8f0ec1bc6a2f776d86721e3"

# ---------------------------------------------------------------------------
# Toolchain: install a Go SDK under third_party/ if one is not already on PATH.
# (Reuses the toolchain the geseq adapter installs, if present.)
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
# Upstream: clone or use ME_FEMTOGO_SRC, pinned to FEMTOGO_REF.
# ---------------------------------------------------------------------------
if [ -n "${ME_FEMTOGO_SRC:-}" ]; then
    SRC="$ME_FEMTOGO_SRC"
else
    SRC="$TP/femto_go"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$FEMTOGO_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$FEMTOGO_REF"
fi

# ---------------------------------------------------------------------------
# Copy the engine sources into the wrapper package. femto_go is `package main`
# with unexported types (Order, OrderBook, Side, Price, ...) and an unexported
# outputRing field, so the wrapper compiles the engine in the same package
# rather than importing it. We copy every .go file EXCEPT:
#   - main.go     : has its own func main() (the demo) — would collide with the
#                   wrapper's required buildmode=c-shared func main().
#   - *_test.go   : test files, not needed for the library.
# Copied files are prefixed `femto_` and wiped first so reruns start clean (the
# pinned source is unmodified — no patch is applied).
# ---------------------------------------------------------------------------
WRAP="$DIR/wrapper"
rm -f "$WRAP"/femto_*.go

for f in "$SRC"/*.go; do
    base="$(basename "$f")"
    case "$base" in
        main.go)   continue ;;
        *_test.go) continue ;;
    esac
    cp "$f" "$WRAP/femto_${base}"
done

# ---------------------------------------------------------------------------
# Build the cgo c-shared library.
# ---------------------------------------------------------------------------
cd "$WRAP"

CGO_ENABLED=1 \
    go build -buildmode=c-shared \
        -ldflags="-s -w" \
        -o "$REPO/femtogo_adapter.so" \
        .

# The cgo build also drops a femtogo_adapter.h alongside; the harness doesn't
# use it, so clean it up.
rm -f "$REPO/femtogo_adapter.h"

echo "built: femtogo_adapter.so"
