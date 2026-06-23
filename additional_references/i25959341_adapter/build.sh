#!/usr/bin/env bash
# Build i25959341_adapter.so. Installs a Go toolchain (user-local, no sudo) if
# one is not already on PATH, clones i25959341/orderbook at a pinned commit,
# applies a correctness fix to the per-side volume accounting (see patch below),
# then builds a Go cgo c-shared library at the harness repo root.
#
# i25959341/orderbook (https://github.com/i25959341/orderbook) is an importable
# pure-Go package (`package orderbook`); the cgo wrapper imports it directly
# through a `replace` directive that points at the pinned third_party checkout.
#
# Override the upstream checkout: ME_I25959341_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

OB_URL="https://github.com/i25959341/orderbook.git"
OB_REF="0d883ab1157580d58ba9f2b9c537a3363310231c"   # add go mod and fix tests (#19)

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
# Upstream: clone or use ME_I25959341_SRC.
# ---------------------------------------------------------------------------
if [ -n "${ME_I25959341_SRC:-}" ]; then
    SRC="$ME_I25959341_SRC"
else
    SRC="$TP/i25959341_src"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$OB_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$OB_REF"
fi

# ---------------------------------------------------------------------------
# Correctness fix (per-side resting volume conservation after a partial fill).
#
# OrderSide.volume is the per-side aggregate quantity. It is incremented in
# OrderSide.Append (orderside.go:67) and decremented in OrderSide.Remove
# (orderside.go:86) — but a PARTIAL fill of a resting order never goes through
# Remove: OrderBook.processQueue (orderbook.go:206-210) shaves the consumed
# quantity off the front order with OrderQueue.Update(), which adjusts the
# per-PRICE-LEVEL volume (orderqueue.go:60-65) but leaves OrderSide.volume
# untouched. So after any partial fill OrderSide.Volume() over-reports by the
# consumed quantity (it only re-converges when the order is later fully removed).
#
# The per-price-level OrderQueue.Volume() — which the adapter's depth-at-price
# query reads — stays correct (Update maintains it), so the harness state audit
# is unaffected. This fix corrects the OTHER reader, OrderSide.Volume() (the
# whole-side aggregate), for completeness. Reported upstream (a duplicate of an
# already-open report of the same per-side accounting issue).
#
# Fix: decrement OrderSide.volume by the consumed quantity at the partial-fill
# site in processQueue. One edit; no behaviour change beyond the per-side
# aggregate. Applied in Python with loud-fail anchors so an upstream change
# can't silently no-op the fix; idempotent via a marker guard (the
# `git reset --hard` above also restores pristine source each run on the default
# checkout).
# ---------------------------------------------------------------------------
python3 - "$SRC/orderbook.go" "$SRC/orderside.go" <<'PYEOF'
import sys
ob_path, osd_path = sys.argv[1], sys.argv[2]
marker = "i25959341_adapter: keep the per-side aggregate volume"

ob = open(ob_path).read()
if marker in ob:
    print("orderbook.go already patched (per-side volume on partial fill)")
    sys.exit(0)

# OrderBook needs a side handle at the partial-fill site to adjust its
# aggregate. processQueue currently has no reference to the OrderSide. We thread
# it through: the two callers (ProcessMarketOrder / ProcessLimitOrder) already
# hold the side being consumed (sideToProcess), so pass it in and subtract the
# consumed quantity from OrderSide.volume when the front order is partially
# filled.

needle_sig = (
    "func (ob *OrderBook) processQueue(orderQueue *OrderQueue, "
    "quantityToTrade decimal.Decimal) "
    "(done []*Order, partial *Order, partialQuantityProcessed, "
    "quantityLeft decimal.Decimal) {\n"
    "\tquantityLeft = quantityToTrade\n"
)
repl_sig = (
    "func (ob *OrderBook) processQueue(orderQueue *OrderQueue, "
    "quantityToTrade decimal.Decimal) "
    "(done []*Order, partial *Order, partialQuantityProcessed, "
    "quantityLeft decimal.Decimal) {\n"
    "\treturn ob.processQueueSide(nil, orderQueue, quantityToTrade)\n"
    "}\n"
    "\n"
    "// processQueueSide is processQueue with the consumed OrderSide threaded in\n"
    "// so a PARTIAL fill can keep the per-side aggregate volume in step.\n"
    "// i25959341_adapter: keep the per-side aggregate volume correct on a\n"
    "// partial fill (Update maintains the per-level volume; OrderSide.volume\n"
    "// was only ever adjusted on Append/Remove). Reported upstream (duplicate).\n"
    "func (ob *OrderBook) processQueueSide(side *OrderSide, orderQueue "
    "*OrderQueue, quantityToTrade decimal.Decimal) "
    "(done []*Order, partial *Order, partialQuantityProcessed, "
    "quantityLeft decimal.Decimal) {\n"
    "\tquantityLeft = quantityToTrade\n"
)
if needle_sig not in ob:
    sys.exit("i25959341 build.sh per-side-volume patch: processQueue anchor not "
             "found in orderbook.go (upstream changed?)")
ob = ob.replace(needle_sig, repl_sig, 1)

# At the partial-fill branch, subtract the consumed quantity from the side
# aggregate (when a side handle was threaded in).
needle_partial = (
        "\t\t\tpartialQuantityProcessed = quantityLeft\n"
        "\t\t\torderQueue.Update(headOrderEl, partial)\n"
        "\t\t\tquantityLeft = decimal.Zero\n"
)
repl_partial = (
        "\t\t\tpartialQuantityProcessed = quantityLeft\n"
        "\t\t\torderQueue.Update(headOrderEl, partial)\n"
        "\t\t\tif side != nil {\n"
        "\t\t\t\tside.volume = side.volume.Sub(quantityLeft)\n"
        "\t\t\t}\n"
        "\t\t\tquantityLeft = decimal.Zero\n"
)
if needle_partial not in ob:
    sys.exit("i25959341 build.sh per-side-volume patch: partial-fill anchor not "
             "found in orderbook.go (upstream changed?)")
ob = ob.replace(needle_partial, repl_partial, 1)

# Route both call sites through processQueueSide with their consumed side.
ob = ob.replace(
    "\t\tordersDone, partialDone, partialProcessed, quantityLeft := "
    "ob.processQueue(bestPrice, quantity)\n",
    "\t\tordersDone, partialDone, partialProcessed, quantityLeft := "
    "ob.processQueueSide(sideToProcess, bestPrice, quantity)\n",
    1,
)
ob = ob.replace(
    "\t\tordersDone, partialDone, partialQty, quantityLeft := "
    "ob.processQueue(bestPrice, quantityToTrade)\n",
    "\t\tordersDone, partialDone, partialQty, quantityLeft := "
    "ob.processQueueSide(sideToProcess, bestPrice, quantityToTrade)\n",
    1,
)
open(ob_path, "w").write(ob)
print("patched orderbook.go: per-side aggregate volume drops by the consumed "
      "quantity on a partial fill")
PYEOF

# ---------------------------------------------------------------------------
# Build the cgo wrapper module. The wrapper imports the engine (module
# `orderbook`) through a `replace` directive its own go.mod declares; point that
# replace at the pinned checkout. Written as a RELATIVE path so go.mod stays
# machine-independent — an absolute path would bake the build host's layout into
# a tracked file.
# ---------------------------------------------------------------------------
WRAP="$DIR/wrapper"
cd "$WRAP"
SRC_REL="$(realpath --relative-to="$WRAP" "$SRC")"
sed -i.bak \
    "s|^replace orderbook =>.*|replace orderbook => ${SRC_REL}|" \
    go.mod
rm -f go.mod.bak

go mod tidy >/dev/null 2>&1 || true

CGO_ENABLED=1 \
    go build -buildmode=c-shared \
        -ldflags="-s -w" \
        -o "$REPO/i25959341_adapter.so" \
        .

# The cgo build also drops an i25959341_adapter.h alongside; the harness doesn't
# use it, so clean it up.
rm -f "$REPO/i25959341_adapter.h"

echo "built: i25959341_adapter.so"
