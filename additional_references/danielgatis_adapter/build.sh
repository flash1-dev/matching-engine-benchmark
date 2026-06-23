#!/usr/bin/env bash
# Build danielgatis_adapter.so. Installs a Go toolchain (user-local, no sudo) if
# one is not already on PATH, clones danielgatis/go-orderbook at a pinned commit,
# applies a correctness fix to the price-level map key (see patch below), then
# builds a Go cgo c-shared library at the harness repo root.
#
# danielgatis/go-orderbook (https://github.com/danielgatis/go-orderbook) is an
# importable pure-Go package (`package orderbook`); the cgo wrapper imports it
# directly through a `replace` directive that points at the pinned third_party
# checkout.
#
# Override the upstream checkout: ME_DANIELGATIS_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

DANIELGATIS_URL="https://github.com/danielgatis/go-orderbook.git"
DANIELGATIS_REF="7640955559eb5473c36a56507d3eadf830c66713"   # "update deps", repo HEAD

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
# Upstream: clone or use ME_DANIELGATIS_SRC.
# ---------------------------------------------------------------------------
if [ -n "${ME_DANIELGATIS_SRC:-}" ]; then
    SRC="$ME_DANIELGATIS_SRC"
else
    SRC="$TP/danielgatis_src"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$DANIELGATIS_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$DANIELGATIS_REF"
fi

# ---------------------------------------------------------------------------
# Correctness fix (same-price orders share one price level).
#
# OrderSide keys its price levels by a `map[decimal.Decimal]*OrderQueue`
# (order_side.go:16). shopspring/decimal.Decimal is a struct `{value *big.Int;
# exp int32}`, and a Go map compares struct keys field-by-field — for the
# *big.Int field that is a POINTER comparison, not a numeric one. Two equal
# prices built independently (e.g. decimal.NewFromInt(100) for two different
# orders) hold different *big.Int pointers, so they are DISTINCT map keys even
# though decimal.Cmp == 0:
#
#     m := map[decimal.Decimal]...{}
#     m[decimal.NewFromInt(100)] = x
#     _, ok := m[decimal.NewFromInt(100)]   // ok == false  (pointer mismatch)
#
# Append (order_side.go:37) therefore misses the existing queue for a price that
# is already resting, creates a SECOND OrderQueue, and os.tree.Put(price, ...)
# (whose comparator IS numeric) overwrites the tree node for that price with the
# new queue — orphaning every order already resting in the first queue. The
# orphaned orders vanish from the book: they never match and a later same-price
# crossing finds nothing (~61% under-match on the moving scenarios; `static`,
# which only ever rests one order per price, passes).
#
# Filed upstream: https://github.com/danielgatis/go-orderbook/issues/2
#
# Fix: key the price-level map by the price's canonical decimal STRING
# (decimal.String()), which is identical for equal prices, so they share one
# OrderQueue. Seven edits, all in order_side.go (the map's field type, its
# allocation, and the five places it is indexed by the raw decimal price); the
# red-black tree — already correctly numeric via .Cmp — is untouched, as is all
# matching/queue logic. Applied in Python with loud-fail anchors so an upstream
# change can't silently no-op the fix; idempotent via a marker guard (the
# `git reset --hard` above also restores pristine source each run on the default
# checkout).
# ---------------------------------------------------------------------------
python3 - "$SRC/order_side.go" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
marker = "danielgatis_adapter: key price levels by the canonical decimal string"
if marker in s:
    print("order_side.go already patched (string price-level key)")
    sys.exit(0)

# Each (needle, replacement) is applied exactly once; a missing anchor is a hard
# failure so an upstream refactor cannot leave the engine silently unpatched.
edits = [
    # 1. field declaration: the price-level map.
    ("\tqueue  map[decimal.Decimal]*OrderQueue\n",
     "\t// danielgatis_adapter: key price levels by the canonical decimal string\n"
     "\t// so equal prices share one queue (a raw map[decimal.Decimal] compares\n"
     "\t// the *big.Int pointer, not the value). See\n"
     "\t// https://github.com/danielgatis/go-orderbook/issues/2\n"
     "\tqueue  map[string]*OrderQueue\n"),
    # 2. constructor: allocate the string-keyed map.
    ("make(map[decimal.Decimal]*OrderQueue)",
     "make(map[string]*OrderQueue)"),
    # 3. Append: existing-queue lookup.
    ("\tpriceQueue, ok := os.queue[price]\n",
     "\tpriceQueue, ok := os.queue[price.String()]\n"),
    # 4. Append: new-queue insert.
    ("\t\tos.queue[order.price] = priceQueue\n",
     "\t\tos.queue[order.price.String()] = priceQueue\n"),
    # 5. Remove: queue lookup.
    ("\tpriceQueue := os.queue[price]\n\to := priceQueue.Remove(e)\n",
     "\tpriceQueue := os.queue[price.String()]\n\to := priceQueue.Remove(e)\n"),
    # 6. Remove: queue delete when emptied.
    ("\t\tdelete(os.queue, price)\n",
     "\t\tdelete(os.queue, price.String())\n"),
    # 7. UpdateAmount: queue lookup.
    ("\tpriceQueue := os.queue[price]\n\to := priceQueue.UpdateAmount(e, amount)\n",
     "\tpriceQueue := os.queue[price.String()]\n\to := priceQueue.UpdateAmount(e, amount)\n"),
]
for needle, repl in edits:
    if needle not in s:
        sys.exit("danielgatis build.sh price-key patch: anchor not found in "
                 "order_side.go (upstream changed?):\n  " + repr(needle))
    s = s.replace(needle, repl, 1)
open(p, "w").write(s)
print("patched order_side.go: price levels keyed by the canonical decimal string")
PYEOF

# ---------------------------------------------------------------------------
# Build the cgo wrapper module. The wrapper imports
# github.com/danielgatis/go-orderbook through a `replace` directive its own
# go.mod declares; point that replace at the pinned checkout. Written as a
# RELATIVE path so go.mod stays machine-independent — an absolute path would
# bake the build host's layout into a tracked file.
# ---------------------------------------------------------------------------
WRAP="$DIR/wrapper"
cd "$WRAP"
SRC_REL="$(realpath --relative-to="$WRAP" "$SRC")"
sed -i.bak \
    "s|^replace github.com/danielgatis/go-orderbook =>.*|replace github.com/danielgatis/go-orderbook => ${SRC_REL}|" \
    go.mod
rm -f go.mod.bak

go mod tidy >/dev/null 2>&1 || true

CGO_ENABLED=1 \
    go build -buildmode=c-shared \
        -ldflags="-s -w" \
        -o "$REPO/danielgatis_adapter.so" \
        .

# The cgo build also drops a danielgatis_adapter.h alongside; the harness
# doesn't use it, so clean it up.
rm -f "$REPO/danielgatis_adapter.h"

echo "built: danielgatis_adapter.so"
