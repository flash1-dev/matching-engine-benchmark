#!/usr/bin/env bash
# Build hroptatyr_clob_adapter.so. Clones hroptatyr/clob at a pinned commit and
# compiles the engine's matcher translation units + this adapter into a single
# .so at the harness repo root. The engine source is compiled UNMODIFIED — there
# is no source patch (see CORRECTNESS_FINDINGS.md: "No fix required").
#
# Engine: hroptatyr/clob (Sebastian Freundt), a b+tree-based central limit order
# book in C with pluggable uncrossing schemes. Pinned at 812137a (tag v0.1.0).
# It is built in its DEFAULT _Decimal64 configuration — the mode ./configure
# selects when the compiler has IEEE-754 DFP support, which gcc-14/aarch64 does.
# (The engine's alternative "double" mode would require rewriting the source's
# hard-coded _Decimal64 literals — i.e. patching the engine — so we use the
# supported, unmodified decimal mode and patch nothing.)
#
# Build approach (why we do NOT just run the engine's Makefile):
#   - We run the engine's own autoreconf + ./configure ONLY to generate
#     src/config.h and src/clob_type.h (autotools feature-detection that picks
#     the DFP encoding and the price/quantity type). These two headers are not
#     committed upstream; clob_type.h is produced from src/clob_type.h.in.
#   - We then compile the matcher TUs directly into the adapter .so. We skip the
#     engine Makefile because it also builds the `cloe` CLI + version.c (both
#     need the `yuck` option-parser generator, which is not installed and not
#     needed) and the dfp754_d32.c TU (32-bit decimal we don't use, and which
#     trips a GCC-14 implicit-declaration error). Only these TUs are compiled:
#       btree.c plqu.c clob.c unxs.c quos.c dfp754_d64.c
#
# Link note: dfp754_d64.h defines a few plain `inline __attribute__((pure,
# const))` math helpers (e.g. nand64) that GCC-14, under default C99 inline
# semantics, emits as an out-of-line external symbol in EVERY TU that includes
# the header, so a multi-TU link reports "multiple definition of nand64". The
# upstream autotools build links the engine as a static archive (selective
# member extraction), which sidesteps it; we instead pass
# -Wl,--allow-multiple-definition. Every copy is byte-identical (same header,
# same flags, a pure/const function), so taking the first is exact. This is a
# LINKER flag only — the engine source stays byte-for-byte unmodified.
#
# Override the upstream checkout: ME_ENG_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

CLOB_URL="https://github.com/hroptatyr/clob.git"
CLOB_REF="812137a3edca4e00f05ac8b3ff2212c5deb545a5"

if [ -n "${ME_ENG_SRC:-}" ]; then
    SRC="$ME_ENG_SRC"
else
    SRC="$TP/hroptatyr_clob"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$CLOB_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$CLOB_REF"
    # Discard any prior in-tree config/build products so configure re-runs clean.
    # Keep nothing untracked but the engine's own files (there are no adapter
    # files inside the clone — the adapter lives in this directory).
    git -C "$SRC" clean -fdxq || true
fi

# No engine source patch. Assert the matcher TUs and the configure inputs that
# the build below depends on are present at the pin (loud-fail if upstream drifts).
for f in src/clob.c src/btree.c src/plqu.c src/unxs.c src/quos.c src/dfp754_d64.c \
         src/clob.h src/unxs.h src/clob_val.h src/clob_type.h.in configure.ac; do
    test -f "$SRC/$f" || { echo "hroptatyr/clob: expected source $f missing (upstream changed?)" >&2; exit 1; }
done

# Toolchain check: the engine's autotools build needs autoreconf; the _Decimal64
# config needs a DFP-capable C compiler. Fail loud rather than silently misbuild.
command -v autoreconf >/dev/null 2>&1 || { echo "autoreconf not found (need autoconf/automake)" >&2; exit 1; }
CC=${CC:-gcc}
command -v "$CC" >/dev/null 2>&1 || { echo "$CC not found (need a DFP-capable C compiler)" >&2; exit 1; }

# Generate config.h + clob_type.h via the engine's own configure (idempotent;
# the clean above removes stale products on the default checkout, and a re-run
# under ME_ENG_SRC simply regenerates them in place).
( cd "$SRC"
  autoreconf -vfi >/tmp/clob_autoreconf.log 2>&1
  ./configure     >/tmp/clob_configure.log   2>&1 )
test -f "$SRC/src/config.h" && test -f "$SRC/src/clob_type.h"
grep -q 'WITH_DECIMAL 1' "$SRC/src/config.h"   # assert decimal mode was selected

CFLAGS="-O3 -march=native -fPIC -std=gnu11 -DHAVE_CONFIG_H \
  -D_POSIX_C_SOURCE=201001L -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -I$SRC/src"
API="$REPO/api"
ENGINE_TUS="$SRC/src/btree.c $SRC/src/plqu.c $SRC/src/clob.c \
            $SRC/src/unxs.c $SRC/src/quos.c $SRC/src/dfp754_d64.c"

$CC $CFLAGS -I"$API" -shared -Wl,--allow-multiple-definition \
    "$DIR/hroptatyr_clob_adapter.c" $ENGINE_TUS \
    -o "$REPO/hroptatyr_clob_adapter.so"

echo "built: $REPO/hroptatyr_clob_adapter.so"
