#!/usr/bin/env bash
# Build dyn4mik3_adapter.so — dyn4mik3/OrderBook (a pure-Python price-time
# matching engine) embedded via CPython.
#
# dyn4mik3/OrderBook is pure Python, so there is no native engine library to
# compile or link: the adapter (dyn4mik3_adapter.cpp) embeds the system CPython
# (python3-config --embed), and at runtime imports the engine package
# (orderbook/, which ships INSIDE the upstream clone) plus the thin translation
# driver (dyn4mik3_driver.py, next to this script), bridging the harness C ABI
# to the engine's NATIVE OrderBook API (process_order / cancel_order +
# OrderTree.order_exists/get_order). No matching is reimplemented adapter-side.
#
# Three absolute dirs are baked into the .so's sys.path (-D defines) so it loads
# regardless of the harness's working directory:
#   ME_REPO_DIR    = the engine clone (puts orderbook/ on sys.path)
#   ME_ADAPTER_DIR = this dir         (puts dyn4mik3_driver.py on sys.path)
#   ME_VENDOR_DIR  = the vendored sortedcontainers tree
#
# Pinned upstream commit (dyn4mik3/OrderBook):
#   a802407d12d2a21d0c8d65d44cc93dc5634f576b
#
# ENGINE SOURCE PATCH (one line, applied post-reset, idempotent, anchor-checked):
#   OrderBook.get_volume_at_price (orderbook/orderbook.py) calls
#   self.{bids,asks}.get_price(price).volume, but OrderTree defines NO get_price
#   method (only get_price_list, ordertree.py). The engine's public depth-at-a-
#   price query therefore raises AttributeError and CRASHES whenever the queried
#   level exists — and the harness's engine_query_depth_at exercises exactly
#   that. The fix is the obvious one: get_price -> get_price_list (the method
#   that returns the level's OrderList, which carries .volume). Reported
#   upstream: https://github.com/dyn4mik3/OrderBook/issues/22
#   Unpatched the engine is INVALID (crashes on the state audit's depth query);
#   patched it is VALID. See CORRECTNESS_FINDINGS.md ("dyn4mik3 ... 1-line fix").
#
# Override the upstream checkout: ME_DYN4MIK3_SRC=/path/to/existing/clone
# (skips the clone; the dir must contain the orderbook/ engine package).
#
# Runtime dep the embedded interpreter must import: sortedcontainers (the
# engine's only third-party dependency). It is vendored locally from its sdist
# so the build is hermetic and does not touch the system Python. The CPython
# embed headers/lib (python3-dev) cannot be auto-installed without root and must
# already be present.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

DYN4MIK3_URL="https://github.com/dyn4mik3/OrderBook.git"
DYN4MIK3_REF="a802407d12d2a21d0c8d65d44cc93dc5634f576b"

# ---------------------------------------------------------------------------
# Upstream: clone or use ME_DYN4MIK3_SRC. The engine package is the orderbook/
# subdirectory of the clone, so the clone ROOT is what goes on sys.path (the
# driver does `from orderbook import OrderBook`).
# ---------------------------------------------------------------------------
if [ -n "${ME_DYN4MIK3_SRC:-}" ]; then
    SRC="$ME_DYN4MIK3_SRC"
else
    SRC="$TP/dyn4mik3_orderbook"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$DYN4MIK3_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$DYN4MIK3_REF"
fi

if [ ! -f "$SRC/orderbook/orderbook.py" ]; then
    echo "build.sh: $SRC has no orderbook/orderbook.py (wrong checkout?)" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# ENGINE PATCH: get_price -> get_price_list in get_volume_at_price.
# Applied AFTER the reset above (so the default checkout is pristine each run),
# idempotent (a no-op once already fixed — also covers the ME_DYN4MIK3_SRC
# override case), and loud-fail anchored so an upstream change cannot silently
# leave the bug in place. https://github.com/dyn4mik3/OrderBook/issues/22
# ---------------------------------------------------------------------------
python3 - "$SRC/orderbook/orderbook.py" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
bad  = ".get_price(price).volume"
good = ".get_price_list(price).volume"
if bad not in s:
    # Already patched (or upstream changed). Accept only if the fixed form is
    # present; otherwise the anchor is gone and we must fail loudly.
    if good in s:
        print("orderbook.py already patched (get_price_list); no-op")
        sys.exit(0)
    sys.exit("dyn4mik3 build.sh depth-query patch: anchor '%s' not found in "
             "orderbook.py and the fixed form is absent (upstream changed?)" % bad)
n = s.count(bad)                      # expect 2 (bid + ask branches)
s = s.replace(bad, good)
open(p, "w").write(s)
print("patched orderbook.py: get_volume_at_price get_price -> get_price_list "
      "(%d sites)" % n)
PYEOF
# Post-condition: the fixed form is present and the buggy form is gone.
grep -q 'get_price_list(price)\.volume' "$SRC/orderbook/orderbook.py" \
    && ! grep -q '[^_]get_price(price)\.volume' "$SRC/orderbook/orderbook.py" \
    || { echo "build.sh: PATCH VERIFY FAILED (get_volume_at_price)" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Toolchain: the system CPython is the engine runtime. There is no clean,
# root-free way to provision a full CPython from a tarball the way the Go/Rust
# adapters do, so we require an embeddable system python3 and vendor the one
# pure-Python dependency (sortedcontainers) locally.
# ---------------------------------------------------------------------------
if ! command -v python3 >/dev/null 2>&1; then
    echo "build.sh: python3 not found on PATH" >&2
    exit 1
fi
if ! python3-config --embed --cflags >/dev/null 2>&1; then
    echo "build.sh: 'python3-config --embed' unavailable — install the CPython" \
         "development headers (e.g. apt-get install python3-dev)." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Vendor sortedcontainers (pure-Python, the engine's only third-party dep) from
# its sdist so the embedded interpreter can import it without touching the
# system site-packages. It lands under third_party/ (gitignored, like the
# engine clone) so this adapter directory stays source-only.
# ---------------------------------------------------------------------------
VENDOR="$TP/dyn4mik3_vendor"
DL="$TP/dyn4mik3_vendor_dl"
rm -rf "$VENDOR"
mkdir -p "$VENDOR" "$DL"
SC_TGZ="$(ls "$DL"/sortedcontainers-*.tar.gz 2>/dev/null | head -1 || true)"
if [ -z "$SC_TGZ" ]; then
    echo "build.sh: fetching sortedcontainers sdist (pip download)"
    python3 -m pip download --no-deps --no-binary :all: -d "$DL" sortedcontainers \
        >/dev/null 2>&1 || {
        echo "build.sh: could not download sortedcontainers; place its sdist" \
             "(sortedcontainers-*.tar.gz) in $DL and re-run." >&2
        exit 1
    }
    SC_TGZ="$(ls "$DL"/sortedcontainers-*.tar.gz | head -1)"
fi
# Extract only the package directory into $VENDOR/sortedcontainers.
tar -xzf "$SC_TGZ" -C "$VENDOR" --strip-components=1 \
    --wildcards '*/sortedcontainers/*'
test -f "$VENDOR/sortedcontainers/__init__.py" || {
    echo "build.sh: vendored sortedcontainers is incomplete" >&2; exit 1; }
echo "vendored sortedcontainers -> $VENDOR/sortedcontainers"

# ---------------------------------------------------------------------------
# Compile flags from the embeddable interpreter.
# ---------------------------------------------------------------------------
PYCFLAGS="$(python3-config --embed --cflags)"
PYLDFLAGS="$(python3-config --embed --ldflags)"
# The libpython soname to re-dlopen with RTLD_GLOBAL at init so CPython's
# runtime-dlopen'd C extensions (_decimal — the engine prices via Decimal, ...)
# can resolve libpython symbols under the harness's RTLD_LOCAL dlopen of this
# adapter (see dyn4mik3_adapter.cpp).
LIBPY_SONAME="$(python3 -c 'import sysconfig; print(sysconfig.get_config_var("INSTSONAME"))')"

# ---------------------------------------------------------------------------
# Build dyn4mik3_adapter.so at the repo root.
# ---------------------------------------------------------------------------
g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -I"$REPO/api" $PYCFLAGS \
    -DME_REPO_DIR="\"$SRC\"" \
    -DME_ADAPTER_DIR="\"$DIR\"" \
    -DME_VENDOR_DIR="\"$VENDOR\"" \
    -DME_LIBPYTHON="\"$LIBPY_SONAME\"" \
    "$DIR/dyn4mik3_adapter.cpp" \
    -o "$REPO/dyn4mik3_adapter.so" \
    $PYLDFLAGS -ldl

echo "built: dyn4mik3_adapter.so"
