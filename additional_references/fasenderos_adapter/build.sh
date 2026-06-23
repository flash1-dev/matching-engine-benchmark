#!/usr/bin/env bash
# Build fasenderos_adapter.so — fasenderos/nodejs-order-book ("nodejs-order-book")
# embedded via libnode/V8.
#
# The engine is a TypeScript/Node.js limit order book, so there is no native C ABI
# to link against: the adapter (fasenderos_adapter.cpp) embeds the system libnode's
# V8 in-process. At engine_init() it spins up a single V8 isolate, evaluates a
# self-contained JS bundle (the engine's compiled `src/` + its two pure-JS deps
# `denque` and `functional-red-black-tree` + a thin entry shim `entry.js`), and
# caches handles to the flat `globalThis.LOB` API the shim exposes. Every harness
# hot-path call runs the matching synchronously on the calling thread — no Node
# event loop, no out-of-process server, no matcher worker thread.
#
# This script:
#   1. clones fasenderos/nodejs-order-book at the pinned commit into
#      third_party/fasenderos_nodejs_order_book (or uses ME_FASENDEROS_SRC), then
#      `git reset --hard` to that pin;
#   2. applies ONE source patch — a per-fill trade-event hook (adapter
#      instrumentation, NOT a correctness fix; this engine is conforming as
#      shipped, see README "Source patch" and CORRECTNESS_FINDINGS.md);
#   3. `npm install`s the engine's two runtime deps + esbuild, bundles
#      entry.js + engine + deps into one IIFE, and embeds that bundle as a C
#      header — all under the gitignored third_party/ build tree so the committed
#      adapter dir holds only authored files (fasenderos_adapter.cpp, entry.js,
#      crypto_stub.js, build.sh, README.md);
#   4. compiles fasenderos_adapter.cpp against the system libnode V8 embed headers
#      into fasenderos_adapter.so at the repo root.
#
# Toolchain note (deviation from the C++ reference recipe, like the jlob JVM and
# pyme CPython embeds): this links against the host's installed libnode
# (-lnode, headers under /usr/include/node) — the analogue of jlob embedding a JVM
# or pyme embedding CPython. libnode (and its dev headers) cannot be provisioned
# root-free from a tarball the way the Go/Rust adapters install their toolchains,
# so it must already be present; build.sh checks and fails loud if it is not.
# `node` + `npm` are needed only at BUILD time (to run esbuild and fetch the two
# pure-JS engine deps); they are not used at run time.
#
# Overrides:
#   ME_FASENDEROS_SRC=/path/to/existing/clone   skip the clone (dir must be a
#                                               nodejs-order-book checkout with src/)
#   ME_NODE_INC=/path/to/node/headers           V8 embed headers (default /usr/include/node)
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

FASENDEROS_URL="https://github.com/fasenderos/nodejs-order-book.git"
FASENDEROS_REF="f8e285bd2179392abe358ecb02f0fd3b76486178"
NODE_INC="${ME_NODE_INC:-/usr/include/node}"

# ---------------------------------------------------------------------------
# 1. Upstream source at the pinned commit (clone or ME_FASENDEROS_SRC).
# ---------------------------------------------------------------------------
if [ -n "${ME_FASENDEROS_SRC:-}" ]; then
    SRC="$ME_FASENDEROS_SRC"
    echo ">>> using ME_FASENDEROS_SRC=$SRC"
else
    SRC="$TP/fasenderos_nodejs_order_book"
    if [ ! -d "$SRC/.git" ]; then
        echo ">>> cloning nodejs-order-book into $SRC"
        git clone --quiet "$FASENDEROS_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$FASENDEROS_REF"
    echo ">>> nodejs-order-book pinned at $FASENDEROS_REF"
fi
[ -f "$SRC/src/orderbook.ts" ] || {
    echo "ERROR: $SRC does not look like a nodejs-order-book checkout (src/orderbook.ts missing)" >&2
    exit 1; }

# ---------------------------------------------------------------------------
# 2. Build toolchain checks: system libnode (run + build) and node/npm (build).
# ---------------------------------------------------------------------------
[ -f "$NODE_INC/v8.h" ] && [ -f "$NODE_INC/libplatform/libplatform.h" ] || {
    echo "ERROR: V8 embed headers not found under $NODE_INC (need v8.h + libplatform/libplatform.h)." >&2
    echo "       Install the libnode development headers (e.g. apt-get install libnode-dev)," >&2
    echo "       or point ME_NODE_INC at an existing node header tree." >&2
    exit 1; }
# Confirm the linker can find libnode. Prefer the ldconfig cache, but fall back
# to scanning the standard library search dirs + any ld config dirs, because
# `ldconfig -p` can return an empty/denied cache inside restricted/containerised
# shells even when the .so is present (and resolvable by ld at link time).
have_libnode() {
    ldconfig -p 2>/dev/null | grep -q 'libnode\.so' && return 0
    local d
    for d in /usr/lib /usr/local/lib /lib \
             "/usr/lib/$(uname -m)-linux-gnu" "/lib/$(uname -m)-linux-gnu" \
             ${LD_LIBRARY_PATH//:/ }; do
        ls "$d"/libnode.so* >/dev/null 2>&1 && return 0
    done
    return 1
}
if ! have_libnode; then
    echo "ERROR: libnode.so not found — install libnode (e.g. apt-get install libnode-dev)," >&2
    echo "       or add its directory to LD_LIBRARY_PATH." >&2
    exit 1
fi
command -v node >/dev/null 2>&1 || { echo "ERROR: 'node' not on PATH (needed at build time to run esbuild)." >&2; exit 1; }
command -v npm  >/dev/null 2>&1 || { echo "ERROR: 'npm' not on PATH (needed at build time to fetch deps + esbuild)." >&2; exit 1; }

# ---------------------------------------------------------------------------
# 3a. Engine patch: per-fill trade-event hook (adapter instrumentation).
#
# nodejs-order-book reports fills only as a post-hoc IProcessOrder summary; the
# harness needs one trade event per fill carrying the maker's resting price + the
# fill qty, in match order. OrderBook.processQueue (src/orderbook.ts) consumes one
# maker per loop iteration; we inject a single guarded global hook call
# (__ME_onFill) at each of its two fill sites — partial-fill (qty filled = the
# pre-zeroing response.quantityLeft) and full-fill (qty filled = headOrder.size).
# This is the same "the engine declares the event but never emits it" instrumentation
# pattern as the jxm35 / kautenja trade-hook injections — NOT a correctness fix:
# nodejs-order-book is conforming AS SHIPPED (CORRECTNESS_FINDINGS.md: "No fix
# required"). Matching logic, prices and quantities are otherwise byte-identical to
# the pinned source. The `git reset --hard` above restores a pristine orderbook.ts
# each run; the marker guard makes a re-apply (e.g. under ME_FASENDEROS_SRC) a no-op;
# loud-fail anchors stop an upstream change silently no-opping the hook.
# ---------------------------------------------------------------------------
echo ">>> patching src/orderbook.ts (2 fill-hook sites)"
python3 - "$SRC/src/orderbook.ts" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()

marker = "__ME_onFill"
if marker in s:
    print(">>> orderbook.ts already carries the fill hook — skipping")
    sys.exit(0)

# 1) partial-fill branch: emit right after partialQuantityProcessed is set
#    (qty filled against this maker = response.quantityLeft, before it is zeroed).
needle1 = "\t\t\t\t\t\tresponse.partialQuantityProcessed = response.quantityLeft;\n"
inject1 = ("\t\t\t\t\t\tif (typeof (globalThis as any).__ME_onFill === \"function\") "
           "(globalThis as any).__ME_onFill(headOrder.id, headOrder.price, response.quantityLeft);\n")
if needle1 not in s:
    sys.exit("fasenderos build.sh fill-hook patch: partial-fill anchor not found in orderbook.ts (upstream changed?)")
s = s.replace(needle1, needle1 + inject1, 1)

# 2) full-fill branch: emit right after quantityLeft is decremented by the full
#    maker size, before the maker order is cancelled/removed
#    (qty filled against this maker = headOrder.size).
needle2 = "\t\t\t\t\t\tresponse.quantityLeft = response.quantityLeft - headOrder.size;\n"
inject2 = ("\t\t\t\t\t\tif (typeof (globalThis as any).__ME_onFill === \"function\") "
           "(globalThis as any).__ME_onFill(headOrder.id, headOrder.price, headOrder.size);\n")
if needle2 not in s:
    sys.exit("fasenderos build.sh fill-hook patch: full-fill anchor not found in orderbook.ts (upstream changed?)")
s = s.replace(needle2, needle2 + inject2, 1)

open(p, "w", encoding="utf-8").write(s)
print(">>> patched src/orderbook.ts (2 fill-hook sites)")
PY

# ---------------------------------------------------------------------------
# 3b. Install engine deps + esbuild, bundle, and embed the bundle as a C header.
#     All build scaffolding (staged entry shim, bundle.js, bundle_js.h) lives
#     under the gitignored third_party/ tree, never in the committed adapter dir.
# ---------------------------------------------------------------------------
echo ">>> installing engine deps + esbuild (build-time only)"
( cd "$SRC" && npm install --no-audit --no-fund --loglevel=error >/dev/null 2>&1 )
ESBUILD="$SRC/node_modules/.bin/esbuild"
[ -x "$ESBUILD" ] || ( cd "$SRC" && npm install --no-save --no-audit --no-fund --loglevel=error esbuild >/dev/null 2>&1 )
[ -x "$ESBUILD" ] || { echo "ERROR: esbuild not available after npm install." >&2; exit 1; }

BUILD="$TP/fasenderos_build"
rm -rf "$BUILD"; mkdir -p "$BUILD"
# Stage the authored entry shim + crypto stub, rewriting the shim's two engine
# imports to ABSOLUTE paths into the pinned clone's src/ (the committed entry.js
# imports them as "../src/...js" relative to the engine's own adapter_src/; here
# the shim is staged outside the clone, so we absolutise instead of relying on a
# fixed relative layout). esbuild resolves the .js specifiers against the engine's
# transpiled TS via its TS resolver.
cp "$DIR/crypto_stub.js" "$BUILD/crypto_stub.js"
python3 - "$DIR/entry.js" "$BUILD/entry.js" "$SRC" <<'PY'
import sys
infile, outfile, src = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(infile, encoding="utf-8").read()
s = s.replace('from "../src/orderbook.js"', 'from "%s/src/orderbook.js"' % src)
s = s.replace('from "../src/types.js"',     'from "%s/src/types.js"'     % src)
assert ("%s/src/orderbook.js" % src) in s, "entry.js orderbook import anchor not found"
assert ("%s/src/types.js" % src) in s,     "entry.js types import anchor not found"
open(outfile, "w", encoding="utf-8").write(s)
print(">>> staged entry shim -> %s (engine imports absolutised to %s/src)" % (outfile, src))
PY

echo ">>> bundling entry.js + engine + deps -> $BUILD/bundle.js"
# node:crypto is marked external and shimmed: the adapter always supplies an
# explicit order id, so randomUUID is never reached at run time; the stub only
# satisfies the import so the bundle has no Node-builtin dependency.
"$ESBUILD" "$BUILD/entry.js" \
    --bundle \
    --format=iife \
    --platform=node \
    --main-fields=main \
    --target=es2020 \
    --legal-comments=none \
    --alias:node:crypto="$BUILD/crypto_stub.js" \
    --outfile="$BUILD/bundle.js"

echo ">>> embedding bundle.js as a C header ($BUILD/bundle_js.h)"
python3 - "$BUILD/bundle.js" "$BUILD/bundle_js.h" <<'PY'
import sys, json
src = open(sys.argv[1], encoding="utf-8").read()
with open(sys.argv[2], "w", encoding="utf-8") as f:
    f.write("// auto-generated from bundle.js by build.sh -- do not edit\n")
    f.write("static const char ENGINE_BUNDLE_JS[] =\n")
    for line in src.splitlines(keepends=True):
        f.write(json.dumps(line)); f.write("\n")
    f.write(";\n")
print(">>> wrote bundle_js.h (%d bytes of JS)" % len(src))
PY

# ---------------------------------------------------------------------------
# 4. Compile the adapter .so at the repo root (bundle_js.h comes from $BUILD).
# ---------------------------------------------------------------------------
echo ">>> compiling fasenderos_adapter.so"
g++ -std=c++17 -O3 -march=native -fPIC -shared \
    -I"$REPO/api" \
    -I"$NODE_INC" \
    -I"$BUILD" \
    "$DIR/fasenderos_adapter.cpp" \
    -o "$REPO/fasenderos_adapter.so" \
    -lnode
echo "built: $REPO/fasenderos_adapter.so"
ls -la "$REPO/fasenderos_adapter.so"
