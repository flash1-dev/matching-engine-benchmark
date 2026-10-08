#!/usr/bin/env bash
# Build intrepidkarthi_adapter.so. Installs a Go toolchain (user-local, no sudo) if
# one is not already on PATH, clones intrepidkarthi/orderbook at a pinned commit,
# adds the adapter's two vendored Go files to it, and builds cmd/meadapter as a cgo
# c-shared library at the harness repo root.
#
# Override the upstream checkout: ME_INTREPIDKARTHI_SRC=/path/to/existing/clone.
# The two vendored files are added to that checkout too.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

SRC_URL="https://github.com/intrepidkarthi/orderbook.git"
SRC_REF="eaf40498e09bed01779521592a583bcff4d09d6e"

# The module declares go 1.23.5; a newer toolchain on PATH is used as it is.
GO_VERSION="1.23.5"
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

export GOCACHE="$TP/go-cache"
export GOMODCACHE="$TP/go-modcache"
mkdir -p "$GOCACHE" "$GOMODCACHE"

if [ -n "${ME_INTREPIDKARTHI_SRC:-}" ]; then
    SRC="$ME_INTREPIDKARTHI_SRC"
else
    SRC="$TP/intrepidkarthi_orderbook"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$SRC_URL" "$SRC"
    fi
    git -C "$SRC" fetch --quiet origin "$SRC_REF" || true
    git -C "$SRC" reset --hard --quiet "$SRC_REF"
fi

# The adapter's two Go files are vendored in this folder. Copy them into the engine
# module (cmd/meadapter imports internal/meadapter, which Go allows only from inside
# the module), so the files built are the ones here.
mkdir -p "$SRC/cmd/meadapter" "$SRC/internal/meadapter"
cp "$DIR/cmd/meadapter/wrapper.go"      "$SRC/cmd/meadapter/wrapper.go"
cp "$DIR/internal/meadapter/adapter.go" "$SRC/internal/meadapter/adapter.go"

(cd "$SRC" && CGO_ENABLED=1 go build -trimpath -buildmode=c-shared \
    -o "$REPO/intrepidkarthi_adapter.so" ./cmd/meadapter)
rm -f "$REPO/intrepidkarthi_adapter.h"

echo "built: intrepidkarthi_adapter.so"
