#!/usr/bin/env bash
# Build matchingo_adapter.so. Installs a Go toolchain (user-local, no sudo) if
# one is not already on PATH, clones GOnevo/matchingo at a pinned commit, applies
# a correctness fix to the price-level volume accounting (see patch below) and
# drops in a read-only query file the audit needs, then builds a Go cgo c-shared
# library at the harness repo root.
#
# matchingo (https://github.com/GOnevo/matchingo) is an importable pure-Go
# package (`package matchingo`); the cgo wrapper imports it directly through a
# `replace` directive that points at the pinned third_party checkout.
#
# Override the upstream checkout: ME_MATCHINGO_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

MATCHINGO_URL="https://github.com/GOnevo/matchingo.git"
MATCHINGO_REF="7aa642f0ffc8dfd509119b1d432b8745fb1dfcc5"   # tag v0.0.1, repo HEAD

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
# Upstream: clone or use ME_MATCHINGO_SRC.
# ---------------------------------------------------------------------------
if [ -n "${ME_MATCHINGO_SRC:-}" ]; then
    SRC="$ME_MATCHINGO_SRC"
else
    SRC="$TP/matchingo_src"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$MATCHINGO_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$MATCHINGO_REF"
fi

# ---------------------------------------------------------------------------
# Correctness fix (resting-depth conservation): a price level's tracked volume
# must drop by the CONSUMED quantity when its front order is partially filled.
#
# OrderQueue.UpdateVolume (orderqueue.go:51) subtracted o.Quantity(), but its
# only caller, OrderBook.processQueue (orderbook.go:306-308), calls it AFTER
# o.DecreaseQuantity(quantity), so o.Quantity() is already the post-decrement
# REMAINDER, not the consumed amount:
#
#     done.appendOrder(o, quantity, price)
#     o.DecreaseQuantity(quantity)     // o.quantity := orderQty - consumed
#     orderQueue.UpdateVolume(o)       // volume -= REMAINDER   (bug)
#
# So a partial fill of the front order leaves the level volume at
# prev - front_remaining instead of prev - consumed (it can even go negative).
# The trade/report stream is unaffected (matching iterates Len()/First(), never
# volume), so report hashes match the baseline; the corruption only surfaces
# through a depth-at-price read (the harness state audit) — and would also
# corrupt FOK CanOrderBeFilled / CalculateMarketPrice, which read level volume.
#
# Filed upstream: https://github.com/GOnevo/matchingo/issues/1
#
# Fix: pass the consumed quantity to UpdateVolume explicitly and subtract that.
# Two edits, no behaviour change beyond the volume accounting. Applied in Python
# with loud-fail anchors so an upstream change can't silently no-op the fix;
# idempotent via a marker guard (the `git reset --hard` above also restores
# pristine source each run on the default checkout).
# ---------------------------------------------------------------------------
python3 - "$SRC/orderqueue.go" "$SRC/orderbook.go" <<'PYEOF'
import sys
oq_path, ob_path = sys.argv[1], sys.argv[2]
marker = "matchingo_adapter: subtract the CONSUMED quantity"

# --- orderqueue.go: UpdateVolume takes the consumed amount and subtracts it ---
oq = open(oq_path).read()
if marker in oq:
    print("orderqueue.go already patched (consumed-qty volume)")
else:
    needle = (
        "// UpdateVolume updates volume\n"
        "func (oq *OrderQueue) UpdateVolume(o *Order) {\n"
        "\toq.volume = oq.volume.Sub(o.Quantity())\n"
        "}\n"
    )
    repl = (
        "// UpdateVolume subtracts the CONSUMED quantity from the level volume.\n"
        "// matchingo_adapter: subtract the CONSUMED quantity, not o.Quantity()\n"
        "// (the post-DecreaseQuantity remainder). See\n"
        "// https://github.com/GOnevo/matchingo/issues/1\n"
        "func (oq *OrderQueue) UpdateVolume(consumed fpdecimal.Decimal) {\n"
        "\toq.volume = oq.volume.Sub(consumed)\n"
        "}\n"
    )
    if needle not in oq:
        sys.exit("matchingo build.sh UpdateVolume patch: anchor not found in "
                 "orderqueue.go (upstream changed?)")
    oq = oq.replace(needle, repl, 1)
    open(oq_path, "w").write(oq)
    print("patched orderqueue.go: UpdateVolume subtracts the consumed quantity")

# --- orderbook.go: pass the consumed quantity at the partial-fill call site ---
ob = open(ob_path).read()
if "orderQueue.UpdateVolume(quantity)" in ob:
    print("orderbook.go already patched (UpdateVolume call site)")
else:
    needle = (
        "\t\t\tdone.appendOrder(o, quantity, price)\n"
        "\t\t\to.DecreaseQuantity(quantity)\n"
        "\t\t\torderQueue.UpdateVolume(o)\n"
    )
    repl = (
        "\t\t\tdone.appendOrder(o, quantity, price)\n"
        "\t\t\torderQueue.UpdateVolume(quantity)\n"
        "\t\t\to.DecreaseQuantity(quantity)\n"
    )
    if needle not in ob:
        sys.exit("matchingo build.sh UpdateVolume call-site patch: anchor not "
                 "found in orderbook.go (upstream changed?)")
    ob = ob.replace(needle, repl, 1)
    open(ob_path, "w").write(ob)
    print("patched orderbook.go: UpdateVolume(consumed) at the partial-fill site")
PYEOF

# ---------------------------------------------------------------------------
# Drop in the engine query file: read-only numeric accessors the audit needs.
# matchingo exposes no numeric best-bid/ask/depth accessor — its only public
# reader, OrderBook.Depth(), fmt.Println's on every call and returns
# map[string]string. This new file (same `package matchingo`, so it can read the
# unexported bids/asks/prices fields) exports three read-only, side-effect-free
# accessors that read the LIVE engine book exactly as the engine maintains it;
# no matching logic is touched. Rewritten from scratch each run (idempotent).
# ---------------------------------------------------------------------------
cat > "$SRC/harness_query.go" <<'EOF'
package matchingo

// harness_query.go — added by the matching-engine-benchmark adapter build.
//
// Read-only, side-effect-free numeric accessors the harness audit queries need.
// They read the LIVE engine book as the engine maintains it; no matching logic
// is touched. fpdecimal.Decimal wraps a single int64 `v` (FromIntScaled / Scaled
// expose it directly), so scaled int64 prices/quantities round-trip exactly.

import "github.com/nikolaydubina/fpdecimal"

// HarnessBestBid returns the highest resting bid price (scaled int64), ok=false
// if there are no bids. The bids side's price tree is reverse-ordered, so its
// BestPriceQueue() (tree Min under that order) is the highest bid.
func (ob *OrderBook) HarnessBestBid() (int64, bool) {
	q := ob.bids.BestPriceQueue()
	if q == nil {
		return 0, false
	}
	return q.Price().Scaled(), true
}

// HarnessBestAsk returns the lowest resting ask price (scaled int64), ok=false
// if there are no asks.
func (ob *OrderBook) HarnessBestAsk() (int64, bool) {
	q := ob.asks.BestPriceQueue()
	if q == nil {
		return 0, false
	}
	return q.Price().Scaled(), true
}

// HarnessDepthAt returns the engine's aggregated resting volume (scaled int64)
// at one price level on the given side, 0 if the level is empty. It reads the
// price level's own `volume` field — exactly what the engine keeps — so the
// audit sees the engine's real depth accounting.
func (ob *OrderBook) HarnessDepthAt(scaledPrice int64, side Side) int64 {
	var os *OrderSide
	if side == Buy {
		os = ob.bids
	} else {
		os = ob.asks
	}
	q, ok := os.prices[fpdecimal.FromIntScaled(scaledPrice)]
	if !ok {
		return 0
	}
	return q.Volume().Scaled()
}
EOF

# ---------------------------------------------------------------------------
# Build the cgo wrapper module. The wrapper imports github.com/gonevo/matchingo
# through a `replace` directive its own go.mod declares; point that replace at
# the pinned checkout. Written as a RELATIVE path so go.mod stays
# machine-independent — an absolute path would bake the build host's layout into
# a tracked file.
# ---------------------------------------------------------------------------
WRAP="$DIR/wrapper"
cd "$WRAP"
SRC_REL="$(realpath --relative-to="$WRAP" "$SRC")"
sed -i.bak \
    "s|^replace github.com/gonevo/matchingo =>.*|replace github.com/gonevo/matchingo => ${SRC_REL}|" \
    go.mod
rm -f go.mod.bak

go mod tidy >/dev/null 2>&1 || true

CGO_ENABLED=1 \
    go build -buildmode=c-shared \
        -ldflags="-s -w" \
        -o "$REPO/matchingo_adapter.so" \
        .

# The cgo build also drops a matchingo_adapter.h alongside; the harness doesn't
# use it, so clean it up.
rm -f "$REPO/matchingo_adapter.h"

echo "built: matchingo_adapter.so"
