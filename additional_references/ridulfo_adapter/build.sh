#!/usr/bin/env bash
# Build ridulfo_adapter.so — ridulfo/order-matching-engine ("ridulfo") embedded
# via CPython.
#
# ridulfo is a pure-Python matching engine (a price-time-priority Orderbook over
# two sortedcontainers.SortedList instances), so there is no native engine
# library to link: the adapter (ridulfo_adapter.cpp) embeds the system CPython
# (python3-config --embed) and, at runtime, imports the engine package
# (ordermatchinengine/) plus the orchestration driver (ridulfo_helper.py),
# bridging the harness C ABI to the engine's NATIVE Orderbook API. The engine
# clone dir (under third_party/) and this adapter dir are both baked into the
# .so's sys.path so it loads regardless of the harness's working directory.
#
# Pinned upstream commit (ridulfo/order-matching-engine):
#   30fdbf579671325cf682492037d804b03b5baceb
#
# Source patch (ONE, applied below, post-reset, idempotent, anchor-checked):
#   LimitOrder.__lt__ is corrected to a consistent total order — a final tiebreak
#   on the unique order_id. The upstream comparator fell to a size compare (a
#   smaller order jumps ahead of an older equal-price one) and returned None when
#   price, time AND size all tied, which silently breaks SortedList.discard() so a
#   cancel can no longer locate its order (lost cancels) and inverts equal-price
#   time priority. Reported upstream:
#     https://github.com/ridulfo/order-matching-engine/issues/10
#
# Overrides:
#   ME_RIDULFO_SRC=/path/to/existing/checkout   (skips the clone; the dir must
#                                                contain ordermatchinengine/)
#
# Runtime deps the embedded interpreter must import: sortedcontainers==2.4.0
# (the exact pin in the engine's requirements.txt) — VENDORED into this dir from
# a pip source download, so the embedded interpreter resolves it without a system
# install (the box's python3 is an externally-managed env, PEP 668). The CPython
# embed headers/lib (python3-dev, for `python3-config --embed`) cannot be
# auto-installed without root and must already be present.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

RIDULFO_URL="https://github.com/ridulfo/order-matching-engine.git"
RIDULFO_REF="30fdbf579671325cf682492037d804b03b5baceb"

# ---------------------------------------------------------------------------
# Upstream: clone or use ME_RIDULFO_SRC. The engine package is ordermatchinengine/
# at the repo ROOT, so the repo root is what goes on sys.path.
# ---------------------------------------------------------------------------
if [ -n "${ME_RIDULFO_SRC:-}" ]; then
    SRC="$ME_RIDULFO_SRC"
else
    SRC="$TP/ridulfo_order_matching_engine"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$RIDULFO_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$RIDULFO_REF"
fi

if [ ! -d "$SRC/ordermatchinengine" ]; then
    echo "build.sh: $SRC has no ordermatchinengine/ package (wrong checkout?)" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Source patch: LimitOrder.__lt__ -> consistent total order (issue #10).
# Applied in Python with a loud-fail anchor so an upstream change cannot silently
# no-op the fix; idempotent via a marker guard (so re-running with an
# ME_RIDULFO_SRC override does not double-apply). The `git reset --hard` above
# restores a pristine Order.py each run on the default clone.
# ---------------------------------------------------------------------------
python3 - "$SRC/ordermatchinengine/Order.py" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
marker = "issue #10: stable total order"
if marker in s:
    print("Order.py already patched (__lt__ total order)")
    sys.exit(0)
needle = (
    "        elif self.time != other.time:\n"
    "             return self.time < other.time\n"
    "\n"
    "        elif self.size != other.size:\n"
    "            return self.size < other.size\n"
)
repl = (
    "        elif self.time != other.time:\n"
    "            return self.time < other.time\n"
    "\n"
    "        # issue #10: stable total order on a unique field (order_id).\n"
    "        # The upstream comparator fell to a size compare (priority\n"
    "        # inversion) and returned None on a full price/time/size tie, which\n"
    "        # breaks SortedList.discard() -> lost cancels.\n"
    "        # https://github.com/ridulfo/order-matching-engine/issues/10\n"
    "        else:\n"
    "            return self.order_id < other.order_id\n"
)
if needle not in s:
    sys.exit("ridulfo build.sh __lt__ patch: anchor not found in Order.py (upstream changed?)")
s = s.replace(needle, repl, 1)
open(p, "w").write(s)
print("patched Order.py: __lt__ is a consistent total order (order_id tiebreak)")
PYEOF

# ---------------------------------------------------------------------------
# Toolchain: the system CPython is the engine runtime. There is no clean,
# root-free way to provision a full CPython from a tarball the way the Go/Rust
# adapters do, so we require an embeddable system python3.
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
# Vendor sortedcontainers==2.4.0 (pure Python) under third_party/ (which is
# gitignored — a build product, not a committed source). Source download (no
# wheels), so it is resolvable by the embedded interpreter without a system/site
# install. Kept independent of the engine checkout so an ME_RIDULFO_SRC override
# does not get this written into it.
# ---------------------------------------------------------------------------
VENDOR="$TP/ridulfo_vendor"
if [ ! -d "$VENDOR/sortedcontainers" ]; then
    mkdir -p "$VENDOR"
    TMP="$(mktemp -d)"
    (
        cd "$TMP"
        pip3 download sortedcontainers==2.4.0 --no-deps --no-binary :all: -d . >/dev/null 2>&1
        tar xzf sortedcontainers-2.4.0.tar.gz
    )
    cp -r "$TMP/sortedcontainers-2.4.0/sortedcontainers" "$VENDOR/sortedcontainers"
    find "$TMP" -delete 2>/dev/null || true
fi
if [ ! -d "$VENDOR/sortedcontainers" ]; then
    echo "build.sh: failed to vendor sortedcontainers==2.4.0 (pip3 download" \
         "unavailable?). Place the 'sortedcontainers' package dir under" \
         "$VENDOR and re-run." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Compile flags from the embeddable interpreter, then build ridulfo_adapter.so
# at the repo root.
#   RIDULFO_REPO_DIR    = engine clone     (puts ordermatchinengine/ on sys.path)
#   RIDULFO_ADAPTER_DIR = this dir         (puts ridulfo_helper.py on sys.path)
#   RIDULFO_VENDOR_DIR  = third_party vendor (puts sortedcontainers on sys.path)
# ---------------------------------------------------------------------------
PYCFLAGS="$(python3-config --embed --cflags)"
PYLDFLAGS="$(python3-config --embed --ldflags)"

g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -I"$REPO/api" $PYCFLAGS \
    -DRIDULFO_REPO_DIR="\"$SRC\"" \
    -DRIDULFO_ADAPTER_DIR="\"$DIR\"" \
    -DRIDULFO_VENDOR_DIR="\"$VENDOR\"" \
    "$DIR/ridulfo_adapter.cpp" \
    -o "$REPO/ridulfo_adapter.so" \
    $PYLDFLAGS

echo "built: ridulfo_adapter.so"
