#!/usr/bin/env bash
# Build oceanbook_adapter.so. Installs a Go toolchain (user-local, no sudo) if
# one is not already on PATH, clones draveness/oceanbook at a pinned commit,
# applies a correctness fix to the engine's Depth accounting (see patch below)
# and drops in a read-only query file the audit needs, then builds a Go cgo
# c-shared library at the harness repo root.
#
# oceanbook (https://github.com/draveness/oceanbook) is an importable pure-Go
# package (its pkg/* are `package orderbook` / `order` / `trade`, NOT package
# main); the cgo wrapper imports it directly through a `replace` directive that
# points at the pinned third_party checkout.
#
# Override the upstream checkout: ME_OCEANBOOK_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

OCEANBOOK_URL="https://github.com/draveness/oceanbook.git"
OCEANBOOK_REF="a7768eed53a239faf883144090fd48931129f145"   # repo HEAD (audited)

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
# Upstream: clone or use ME_OCEANBOOK_SRC.
# ---------------------------------------------------------------------------
if [ -n "${ME_OCEANBOOK_SRC:-}" ]; then
    SRC="$ME_OCEANBOOK_SRC"
else
    SRC="$TP/oceanbook"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$OCEANBOOK_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$OCEANBOOK_REF"
fi

# ---------------------------------------------------------------------------
# Correctness fix (Depth price-level accounting): the engine's market-data
# Depth writes the trade/order QUANTITY into the PriceLevel.Price field and
# leaves PriceLevel.Quantity (and Count) at zero, so every depth level reports
# price=±qty / quantity=0 — i.e. the aggregated resting quantity the harness's
# depth audit reads is always 0, and the price key is garbage. The match path
# is unaffected (matching iterates the Bids/Asks order trees and the per-order
# remaining quantity, never the Depth struct), so the trade/report stream is
# byte-identical to the liquibook baseline; the corruption surfaces only through
# a depth-at-price read.
#
# OrderBook.insertOrder (orderbook.go) updates Depth in two places:
#   - on each fill:  UpdatePriceLevel({Side, Price: newTrade.Quantity.Neg()})
#   - on rest:       UpdatePriceLevel({Side, Price: newOrder.PendingQuantity()})
# Both pass the quantity as Price and omit Quantity/Count.
#
# Filed upstream: https://github.com/draveness/oceanbook/issues/44
#
# Fix: put the real price in Price, the quantity in Quantity, and maintain Count
# (+1 when a new level rests; -1 when a maker is fully filled and leaves the
# book) so the level prunes correctly. Two edits, no behaviour change beyond the
# Depth accounting. Applied in Python with loud-fail anchors so an upstream
# change can't silently no-op the fix; idempotent via a marker guard (the
# `git reset --hard` above also restores pristine source each run on the default
# checkout).
# ---------------------------------------------------------------------------
python3 - "$SRC/pkg/orderbook/orderbook.go" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
marker = "oceanbook_adapter: real Price + Quantity + Count"

if marker in s:
    print("orderbook.go already patched (Depth Price/Quantity/Count)")
    sys.exit(0)

# --- fill site: maker's level loses the consumed quantity; prune on full fill.
fill_needle = (
        "\t\tod.depth.UpdatePriceLevel(&PriceLevel{\n"
        "\t\t\tSide:  bestOrder.Side,\n"
        "\t\t\tPrice: newTrade.Quantity.Neg(),\n"
        "\t\t})\n"
)
fill_repl = (
        "\t\t// oceanbook_adapter: real Price + Quantity + Count (Count=-1 when\n"
        "\t\t// the maker is fully filled and leaves the book). See\n"
        "\t\t// https://github.com/draveness/oceanbook/issues/44\n"
        "\t\treduce := &PriceLevel{\n"
        "\t\t\tSide:     bestOrder.Side,\n"
        "\t\t\tPrice:    bestOrder.Price,\n"
        "\t\t\tQuantity: newTrade.Quantity.Neg(),\n"
        "\t\t}\n"
        "\t\tif bestOrder.Filled() {\n"
        "\t\t\treduce.Count = ^uint64(0) // -1: maker leaves the book\n"
        "\t\t}\n"
        "\t\tod.depth.UpdatePriceLevel(reduce)\n"
)

# --- rest site: the taker's residual rests; its level gains Quantity + Count.
rest_needle = (
        "\tod.depth.UpdatePriceLevel(&PriceLevel{\n"
        "\t\tSide:  newOrder.Side,\n"
        "\t\tPrice: newOrder.PendingQuantity(),\n"
        "\t})\n"
)
rest_repl = (
        "\tod.depth.UpdatePriceLevel(&PriceLevel{\n"
        "\t\tSide:     newOrder.Side,\n"
        "\t\tPrice:    newOrder.Price,\n"
        "\t\tQuantity: newOrder.PendingQuantity(),\n"
        "\t\tCount:    1,\n"
        "\t})\n"
)

if fill_needle not in s:
    sys.exit("oceanbook build.sh Depth fill-site patch: anchor not found in "
             "orderbook.go (upstream changed?)")
if rest_needle not in s:
    sys.exit("oceanbook build.sh Depth rest-site patch: anchor not found in "
             "orderbook.go (upstream changed?)")

s = s.replace(fill_needle, fill_repl, 1)
s = s.replace(rest_needle, rest_repl, 1)
open(p, "w").write(s)
print("patched orderbook.go: Depth carries real Price + Quantity + Count")
PYEOF

# ---------------------------------------------------------------------------
# Drop in the engine query file: read-only numeric accessors the audit needs.
# oceanbook exposes no public best-bid/ask/depth accessor. This new file (same
# `package orderbook`, so it can read the unexported fields) exports three
# read-only, side-effect-free accessors that read the engine's AUTHORITATIVE
# resting book — the Bids/Asks red-black trees of live orders — exactly as the
# engine maintains it; no matching logic is touched. Rewritten from scratch each
# run (idempotent).
#
# Why the order trees and not the engine's Depth struct: oceanbook keeps two
# views of resting liquidity. (1) The Bids/Asks order trees are authoritative —
# the match loop crosses against them (makerBooks.Right()) and CancelOrder
# removes from them. (2) The separate `depth *Depth` aggregate is a market-data
# surface that (a) has the #44 quantity-in-the-Price-field bug the patch above
# fixes AND (b) is never updated by CancelOrder, so it goes stale after a cancel
# even once patched. The harness audit must observe the engine's real resting
# book, so these accessors read view (1): best = the order tree's right-most
# node (the exact node the match loop crosses against), depth = the sum of the
# live resting orders' remaining quantity at the price. (The #44 fix is still
# applied so the engine's own Depth market-data surface is correct; the audit
# simply reads the authoritative order trees rather than that aggregate.)
# ---------------------------------------------------------------------------
cat > "$SRC/pkg/orderbook/harness_query.go" <<'EOF'
package orderbook

// harness_query.go — added by the matching-engine-benchmark adapter build.
//
// Read-only, side-effect-free numeric accessors the harness audit queries need.
// They read the engine's AUTHORITATIVE resting book — the Bids/Asks red-black
// trees of live orders, which the match loop crosses against and CancelOrder
// removes from — exactly as the engine maintains it; no matching logic is
// touched. Workload prices/quantities are positive integers carried as
// decimal.New(n, 0), so IntPart() recovers the scaled int64 exactly.

import "github.com/draveness/oceanbook/pkg/order"

// HarnessBestBid returns the highest resting bid price (int64 ticks), ok=false
// if there are no bids. The Bids order tree's right-most node is the highest
// bid (order.Comparator sorts the higher bid to the right) — the same node the
// match loop crosses against via makerBooks.Right().
func (od *OrderBook) HarnessBestBid() (int64, bool) {
	n := od.Bids.Right()
	if n == nil {
		return 0, false
	}
	return n.Value.(*order.Order).Price.IntPart(), true
}

// HarnessBestAsk returns the lowest resting ask price (int64 ticks), ok=false
// if there are no asks. The Asks order tree's right-most node is the lowest ask
// (order.Comparator sorts the lower ask to the right).
func (od *OrderBook) HarnessBestAsk() (int64, bool) {
	n := od.Asks.Right()
	if n == nil {
		return 0, false
	}
	return n.Value.(*order.Order).Price.IntPart(), true
}

// HarnessDepthAt returns the engine's aggregated resting quantity (int64 ticks)
// at one price level on the given side, 0 if the level is empty. It sums the
// remaining quantity of the live resting orders at the price in the
// authoritative Bids/Asks order tree (which reflects cancels), so the audit
// sees the engine's real resting depth.
func (od *OrderBook) HarnessDepthAt(scaledPrice int64, side order.Side) int64 {
	orders := od.Asks
	if side == order.SideBid {
		orders = od.Bids
	}
	var total int64
	it := orders.Iterator()
	for it.Next() {
		o := it.Value().(*order.Order)
		if o.Price.IntPart() == scaledPrice {
			total += o.PendingQuantity().IntPart()
		}
	}
	return total
}
EOF

# ---------------------------------------------------------------------------
# Build the cgo wrapper module. The wrapper imports
# github.com/draveness/oceanbook through a `replace` directive its own go.mod
# declares; point that replace at the pinned checkout. Written as a RELATIVE
# path so go.mod stays machine-independent — an absolute path would bake the
# build host's layout into a tracked file.
# ---------------------------------------------------------------------------
WRAP="$DIR/wrapper"
cd "$WRAP"
SRC_REL="$(realpath --relative-to="$WRAP" "$SRC")"
sed -i.bak \
    "s|^replace github.com/draveness/oceanbook =>.*|replace github.com/draveness/oceanbook => ${SRC_REL}|" \
    go.mod
rm -f go.mod.bak

go mod tidy >/dev/null 2>&1 || true

CGO_ENABLED=1 \
    go build -buildmode=c-shared \
        -ldflags="-s -w" \
        -o "$REPO/oceanbook_adapter.so" \
        .

# The cgo build also drops an oceanbook_adapter.h alongside; the harness doesn't
# use it, so clean it up.
rm -f "$REPO/oceanbook_adapter.h"

echo "built: oceanbook_adapter.so"
