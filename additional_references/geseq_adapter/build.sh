#!/usr/bin/env bash
# Build geseq_adapter.so. Installs a Go toolchain (user-local, no sudo) if one
# is not already on PATH, clones geseq/orderbook at a pinned commit, and builds
# a Go cgo c-shared library at the harness repo root.
#
# Override the upstream checkout: ME_GESEQ_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

GESEQ_URL="https://github.com/geseq/orderbook.git"
GESEQ_REF="3b9e9cd93cbaac02ba8359d2c3443a962d04c05f"

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
# Upstream: clone or use ME_GESEQ_SRC.
# ---------------------------------------------------------------------------
if [ -n "${ME_GESEQ_SRC:-}" ]; then
    SRC="$ME_GESEQ_SRC"
else
    SRC="$TP/geseq_orderbook"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$GESEQ_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$GESEQ_REF"
fi

# Patch pricelevel.go in place. The shipped processLimitOrder matches the
# best-price queue, then iterates pl.GetQueue() without re-checking the
# price-cross predicate, so an aggressor that partially fills the best
# level keeps consuming non-crossing levels too. Inject the predicate
# check that the inner loop is missing. Idempotent — reset --hard above
# restores the file before re-patching.
PL="$SRC/pricelevel.go"
python3 - "$PL" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
src = p.read_text()
# Check for the inserted segment (a unique post-patch substring) — if it's
# already there, the file has been patched and re-running is a no-op.
already_patched_marker = "&& compare(orderQueue.Price()); orderQueue = pl.GetQueue()"
if already_patched_marker in src:
    sys.exit(0)
old = (
    "\torderQueue = pl.GetQueue()\n"
    "\tqtyLeft := qty\n"
    "\tqtyProcessed = decimal.Zero\n"
    "\tfor orderQueue := pl.GetQueue(); qtyLeft.GreaterThan(decimal.Zero) && orderQueue != nil; orderQueue = pl.GetQueue() {\n"
)
new = (
    "\torderQueue = pl.GetQueue()\n"
    "\tqtyLeft := qty\n"
    "\tqtyProcessed = decimal.Zero\n"
    "\tfor orderQueue := pl.GetQueue(); qtyLeft.GreaterThan(decimal.Zero) && orderQueue != nil && compare(orderQueue.Price()); orderQueue = pl.GetQueue() {\n"
)
if old not in src:
    sys.stderr.write("patch needle not found in pricelevel.go\n")
    sys.exit(1)
p.write_text(src.replace(old, new, 1))
PY

# ---------------------------------------------------------------------------
# Build the cgo wrapper module. The wrapper imports geseq/orderbook through
# its module path; the wrapper's own go.mod declares the dependency.
# ---------------------------------------------------------------------------
WRAP="$DIR/wrapper"
cd "$WRAP"

# Make the wrapper module resolve geseq/orderbook to the pinned local checkout.
# (Wrapper's go.mod has a `replace` directive that points at $SRC.)
sed -i.bak \
    "s|^replace github.com/geseq/orderbook =>.*|replace github.com/geseq/orderbook => ${SRC}|" \
    go.mod
rm -f go.mod.bak

go mod tidy >/dev/null 2>&1 || true

CGO_ENABLED=1 \
    go build -buildmode=c-shared \
        -ldflags="-s -w" \
        -o "$REPO/geseq_adapter.so" \
        .

# The cgo build also drops a geseq_adapter.h alongside; it's not used by the
# harness, so clean it up.
rm -f "$REPO/geseq_adapter.h"

echo "built: geseq_adapter.so"
