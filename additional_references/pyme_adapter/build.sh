#!/usr/bin/env bash
# Build pyme_adapter.so — Surbeivol/PythonMatchingEngine ("pyme") embedded via
# CPython.
#
# pyme is a pure-Python matching engine, so there is no native engine source to
# compile: the adapter (pyme_adapter.cpp) embeds the system CPython
# (python3-config --embed), and at runtime imports the engine package
# (marketsimulator/) plus the orchestration driver (pyme_driver.py), bridging
# the harness C ABI to the engine's NATIVE Orderbook API. The engine repo dir
# (the clone under third_party/) and this adapter dir are both baked into the
# .so's sys.path so it loads regardless of the harness's working directory.
#
# NO engine source is patched. pyme already exposes everything the harness
# needs: per-order ids, cancel-by-id, native crossing with the maker (resting)
# price + maker/taker ids, and an authoritative per-order `active` flag. See
# CORRECTNESS_FINDINGS.md at the repo root ("pyme ... No fix required").
#
# Pinned upstream commit (Surbeivol/PythonMatchingEngine):
#   f94150294a85d7b415ca4518590b5a661d6f9958
#
# Override the upstream checkout: ME_PYME_SRC=/path/to/existing/clone (skips the
# clone; the dir must contain the marketsimulator/ engine package).
#
# Runtime deps the embedded interpreter must be able to import: numpy, pandas,
# pyyaml. build.sh tries `pip install --user` for any that are missing; the
# CPython embed headers/lib (python3-dev) cannot be auto-installed without root
# and must already be present.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

PYME_URL="https://github.com/Surbeivol/PythonMatchingEngine.git"
PYME_REF="f94150294a85d7b415ca4518590b5a661d6f9958"

# ---------------------------------------------------------------------------
# Upstream: clone or use ME_PYME_SRC. The whole repo is the engine package
# (the driver imports `from marketsimulator.orderbook import Orderbook`), so the
# repo ROOT is what goes on sys.path.
# ---------------------------------------------------------------------------
if [ -n "${ME_PYME_SRC:-}" ]; then
    SRC="$ME_PYME_SRC"
else
    SRC="$TP/pyme_python_matching_engine"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$PYME_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$PYME_REF"
fi

if [ ! -d "$SRC/marketsimulator" ]; then
    echo "build.sh: $SRC has no marketsimulator/ package (wrong checkout?)" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Toolchain: the system CPython is the engine runtime. There is no clean,
# root-free way to provision a full CPython + numpy/pandas/pyyaml stack from a
# tarball the way the Go/Rust adapters do, so we require an embeddable system
# python3 and remediate only the importable deps via `pip install --user`.
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

# Ensure numpy / pandas / pyyaml are importable; pip --user any that are absent.
for spec in "numpy:numpy" "pandas:pandas" "yaml:pyyaml"; do
    mod="${spec%%:*}"; pkg="${spec##*:}"
    if ! python3 -c "import ${mod}" >/dev/null 2>&1; then
        echo "build.sh: installing missing runtime dep '${pkg}' (pip --user)"
        python3 -m pip install --user --quiet "${pkg}" || {
            echo "build.sh: failed to install ${pkg}; install it for python3 and re-run." >&2
            exit 1
        }
    fi
done

# ---------------------------------------------------------------------------
# Compile flags from the embeddable interpreter.
# ---------------------------------------------------------------------------
PYCFLAGS="$(python3-config --embed --cflags)"
PYLDFLAGS="$(python3-config --embed --ldflags)"
# The libpython soname to re-dlopen with RTLD_GLOBAL at init so numpy/pandas
# C-extensions can resolve libpython symbols (see pyme_adapter.cpp).
LIBPY_SONAME="$(python3 -c 'import sysconfig; print(sysconfig.get_config_var("INSTSONAME"))')"

# ---------------------------------------------------------------------------
# Build pyme_adapter.so at the repo root.
#   PYME_REPO_DIR    = engine clone (puts marketsimulator/ on sys.path)
#   PYME_ADAPTER_DIR = this dir     (puts pyme_driver.py on sys.path)
# These are distinct directories here (the engine clone lives under
# third_party/, the driver ships next to this script), so both are baked in.
# ---------------------------------------------------------------------------
g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -I"$REPO/api" $PYCFLAGS \
    -DPYME_REPO_DIR="\"$SRC\"" \
    -DPYME_ADAPTER_DIR="\"$DIR\"" \
    -DPYME_LIBPYTHON_SONAME="\"$LIBPY_SONAME\"" \
    "$DIR/pyme_adapter.cpp" \
    -o "$REPO/pyme_adapter.so" \
    $PYLDFLAGS -ldl

echo "built: pyme_adapter.so"
