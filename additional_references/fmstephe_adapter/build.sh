#!/usr/bin/env bash
# Build fmstephe_adapter.so. Installs a Go toolchain (user-local, no sudo) if
# one is not already on PATH, clones fmstephe/matching_engine at a pinned
# commit, materialises a local copy of its flib dependency with three small
# arm64 port files added (a portability port — the engine matcher is built
# UNMODIFIED), then builds a Go cgo c-shared library at the harness repo root.
#
# fmstephe/matching_engine (https://github.com/fmstephe/matching_engine) is an
# importable pure-Go price-time-priority limit-order book (`package matcher`,
# `package msg`); the cgo wrapper imports it directly through a `replace`
# directive that points at the pinned third_party checkout.
#
# Override the upstream checkout: ME_FMSTEPHE_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

FMSTEPHE_URL="https://github.com/fmstephe/matching_engine.git"
FMSTEPHE_REF="fdc2088cfe508d78e2ec5fa6dfa2d8cb3a189873"   # "Adding go.mod", repo HEAD

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
# Upstream: clone or use ME_FMSTEPHE_SRC. The matcher is built UNMODIFIED;
# `git reset --hard` pins the default checkout each run.
# ---------------------------------------------------------------------------
if [ -n "${ME_FMSTEPHE_SRC:-}" ]; then
    SRC="$ME_FMSTEPHE_SRC"
else
    SRC="$TP/fmstephe_matching_engine"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$FMSTEPHE_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$FMSTEPHE_REF"
fi

# ---------------------------------------------------------------------------
# Local flib copy with arm64 ports (portability port — NOT an engine change).
#
# The matcher imports `coordinator`, which imports github.com/fmstephe/flib's
# spscq queue, which imports flib's fatomic / padded / ftime packages. Those
# were written in 2017 with amd64-only implementations:
#   - fsync/fatomic/lazy.go       `+build amd64`            -> LazyStore
#   - fsync/padded/const_amd64.go (implicit amd64 tag)      -> CacheLineBytes
#   - ftime/ftime.go + ftime_amd64.s (amd64 asm only)       -> Counter/cpuid/Pause
# On an aarch64 host those packages have NO buildable Go files, so the module
# fails to compile ("build constraints exclude all Go files"). The adapter
# drives the matcher SINGLE-THREADED and never constructs an spscq queue, so
# this code is dead on our path — it only has to compile and link.
#
# third_party/fmstephe_flib_local is a verbatim copy of the pinned flib with
# three arm64 port files ADDED (no upstream file edited). The wrapper's go.mod
# `replace`s flib to this local copy. This is the same kind of portability port
# the harness's tzadiko (Windows->POSIX) and robaho (C++20 conformance)
# reference adapters apply; the matcher itself is untouched. Materialised only
# if missing; idempotent.
# ---------------------------------------------------------------------------
FLIB_LOCAL="$TP/fmstephe_flib_local"
FLIB_VER="v0.0.0-20170802081819-76e5765dde32"
FLIB_SRC="$GOMODCACHE/github.com/fmstephe/flib@${FLIB_VER}"
if [ ! -f "$FLIB_LOCAL/ftime/ftime_arm64.s" ]; then
    echo "build.sh: materialising third_party/fmstephe_flib_local with arm64 ports"
    # Populate the module cache with the pinned flib, then copy it out.
    ( cd "$SRC" && GOFLAGS=-mod=mod go mod download github.com/fmstephe/flib )
    rm -rf "$FLIB_LOCAL"
    cp -r "$FLIB_SRC" "$FLIB_LOCAL"
    chmod -R u+w "$FLIB_LOCAL"

    # go.mod so the wrapper's `replace` can target the local dir.
    printf 'module github.com/fmstephe/flib\n\ngo 1.18\n' > "$FLIB_LOCAL/go.mod"

    # arm64 LazyStore (identical relaxed store to the amd64 original).
    cat > "$FLIB_LOCAL/fsync/fatomic/lazy_arm64.go" <<'EOF'
//go:build arm64

package fatomic

//go:nosplit
//go:noinline
func LazyStore(addr *int64, val int64) {
	*addr = val
}
EOF

    # arm64 CacheLineBytes.
    cat > "$FLIB_LOCAL/fsync/padded/const_arm64.go" <<'EOF'
package padded

const CacheLineBytes = 64
EOF

    # arm64 ftime asm (dead code on the synchronous path; link-only).
    cat > "$FLIB_LOCAL/ftime/ftime_arm64.s" <<'EOF'
#include "textflag.h"

// func Counter() (count int64)
TEXT ·Counter(SB),NOSPLIT,$0-8
	WORD	$0xd53be040 // MRS CNTVCT_EL0, R0
	MOVD	R0, count+0(FP)
	RET

// func cpuid(eaxi uint32) (eax, ebx, ecx, edx uint32)
TEXT ·cpuid(SB),NOSPLIT,$0-24
	MOVW	$0, eax+8(FP)
	MOVW	$0, ebx+12(FP)
	MOVW	$0, ecx+16(FP)
	MOVW	$0, edx+20(FP)
	RET

// func Pause(ticks int64)
TEXT ·Pause(SB),NOSPLIT,$0-8
	RET
EOF
fi
# On amd64 hosts the upstream files already cover the build; the
# //go:build arm64 / *_arm64.* selectors make these local ports inert there.

# ---------------------------------------------------------------------------
# Build the cgo wrapper module. Its go.mod `replace`s point at the pinned
# checkouts; rewrite them to RELATIVE paths so go.mod stays machine-independent
# (an absolute path would bake the build host's layout into a tracked file).
# ---------------------------------------------------------------------------
WRAP="$DIR/wrapper"
cd "$WRAP"
SRC_REL="$(realpath --relative-to="$WRAP" "$SRC")"
FLIB_REL="$(realpath --relative-to="$WRAP" "$FLIB_LOCAL")"
sed -i.bak \
    -e "s|^replace github.com/fmstephe/matching_engine =>.*|replace github.com/fmstephe/matching_engine => ${SRC_REL}|" \
    -e "s|^replace github.com/fmstephe/flib =>.*|replace github.com/fmstephe/flib => ${FLIB_REL}|" \
    go.mod
rm -f go.mod.bak

export GOFLAGS=-mod=mod
go mod tidy >/dev/null 2>&1 || true

CGO_ENABLED=1 \
    go build -buildmode=c-shared \
        -ldflags="-s -w" \
        -o "$REPO/fmstephe_adapter.so" \
        .

# The cgo build also drops a fmstephe_adapter.h alongside; the harness doesn't
# use it, so clean it up.
rm -f "$REPO/fmstephe_adapter.h"

echo "built: fmstephe_adapter.so"
