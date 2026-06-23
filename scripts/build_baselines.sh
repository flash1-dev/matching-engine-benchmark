#!/usr/bin/env bash
#
# build_baselines.sh — fetch and build the three reference engine adapters.
#
#   scripts/build_baselines.sh [liquibook|quantcup|exchange_core|all]
#
# Each engine's public source is cloned into third_party/<engine>/ at a pinned
# commit and compiled, together with its adapter, into <engine>_adapter.so at
# the repository root. The harness loads those .so files with --baseline.
#
# Every deviation from upstream is recorded in docs/PATCHES.md. Liquibook and
# Exchange-core need no source patch; QuantCup needs patches/quantcup.patch,
# which this script applies after cloning.
#
# Overrides (use an existing checkout instead of cloning):
#   ME_LIQUIBOOK_SRC, ME_QUANTCUP_SRC, ME_EXCHANGE_CORE_SRC
# Toolchain:
#   ME_JDK11   path to a JDK 11 install (required to build exchange_core)
#
set -euo pipefail

ENGINES=("${@:-all}")    # one or more of: liquibook quantcup exchange_core all
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

# Pinned upstream commits — see docs/PATCHES.md.
LIQUIBOOK_URL="https://github.com/ObjectComputing/liquibook.git"
LIQUIBOOK_REF="2427613b32f1667abae68a01df6af9ba8270f8e7"
QUANTCUP_URL="https://github.com/ajtulloch/quantcup-orderbook.git"
QUANTCUP_REF="f860e0b831a7dd2d0c07a5dbc3723ef15d1067ed"
EXCHANGE_CORE_URL="https://github.com/exchange-core/exchange-core.git"
EXCHANGE_CORE_REF="2f8548749839e9095c8dc597e4b61521d259fa5d"

CXXFLAGS="-std=c++20 -O3 -march=native -fPIC -shared"

say() { printf '\n>>> %s\n' "$*"; }

# clone_pinned <url> <ref> <dir> — clone if absent, then hard-reset to <ref> so
# a re-run always starts from pristine upstream source (any patch a previous run
# applied is discarded; untracked files are left for the caller to git-clean).
# Always `fetch` before the reset so a previously-cloned tree can advance to a
# bumped pinned <ref> without the user having to delete third_party/<engine>/.
clone_pinned() {
    local url="$1" ref="$2" dir="$3"
    if [ ! -d "$dir/.git" ]; then
        say "cloning $url"
        git clone --quiet "$url" "$dir"
    else
        # Fetch the specific ref if the remote advertises it; otherwise pull
        # everything. Tolerate offline reruns when the desired ref is already
        # present locally.
        git -C "$dir" fetch --quiet origin "$ref" 2>/dev/null \
            || git -C "$dir" fetch --quiet origin 2>/dev/null \
            || true
    fi
    git -C "$dir" reset --hard --quiet "$ref"
}

build_liquibook() {
    say "liquibook"
    local src
    if [ -n "${ME_LIQUIBOOK_SRC:-}" ]; then
        src="$ME_LIQUIBOOK_SRC"
    else
        src="$TP/liquibook"
        clone_pinned "$LIQUIBOOK_URL" "$LIQUIBOOK_REF" "$src"
    fi
    # Liquibook is header-only apart from simple_order.cpp. No source patch:
    # IOC handling and cancel+reinsert modify live in the adapter.
    g++ $CXXFLAGS -fexceptions -frtti \
        -I "$REPO/api" -I "$src/src" \
        "$REPO/adapters/liquibook_adapter.cpp" \
        "$src/src/simple/simple_order.cpp" \
        -o "$REPO/liquibook_adapter.so"
    echo "built: liquibook_adapter.so"
}

# widen_quantcup_price_domain <src> — post-patch engine-source edit that lifts
# QuantCup's price domain from uint16_t to a 32-bit type and sizes its flat
# price-indexed book to 2^18 = 262144 ticks (~$1310 at $0.005/tick from a
# $167.52 start, ~8x — far beyond any GBM realization; ~6 MiB array). The
# canonical seed-23 workload stays well inside the original uint16_t range, so
# these edits do not change matching output on it (verified: byte-identical
# report-stream hashes); they only stop QuantCup aborting on wide-swing seeds
# (e.g. flash-crash seed 711116612 reaches tick 68910 > 65534).
#
# Idempotent: run after `git reset --hard <pin>` + `git apply quantcup.patch`,
# so it always rewrites pristine, freshly-patched source. Recorded in
# docs/PATCHES.md. See QC_PRICE_MAX in adapters/quantcup_adapter.cpp (kept in
# lockstep with kNumPricePoints below).
widen_quantcup_price_domain() {
    local src="$1"
    python3 - "$src" <<'PY'
import sys, re, pathlib
src = pathlib.Path(sys.argv[1])

# --- constants.h: widen t_price; add the array-dimension constant; keep the
#     live-order cap off t_price's (now 4.29e9) max. ---
c = (src / "constants.h").read_text()

# 1. Widen the price word.  unsigned short (16-bit) -> uint32_t (32-bit).
new = c.replace("typedef unsigned short t_price;", "typedef uint32_t t_price;")
assert new != c, "constants.h: t_price typedef not found (already widened?)"
c = new

# 2. Decouple the flat book's dimension from kMaxPrice (which is t_price's max,
#    now ~4.29e9 — far too large to allocate).  kNumPricePoints is the array
#    size AND the empty-ask sentinel: valid price indices are [1, N-1], and
#    askMin == N means "no resting ask" — exactly the original design where the
#    array had kMaxPrice (65535) slots and askMin started at kMaxPrice.
if "kNumPricePoints" not in c:
    anchor = "constexpr t_price kMaxPrice = std::numeric_limits<t_price>::max();\n"
    assert anchor in c, "constants.h: kMaxPrice anchor not found"
    c = c.replace(anchor, anchor +
        "\n"
        "// Flat price-indexed book dimension (== empty-ask sentinel). The usable\n"
        "// price domain is [1, kNumPricePoints - 1]. 2^18 ticks spans ~8x from a\n"
        "// $167.52 start at $0.005/tick — beyond any workload realization. (Was\n"
        "// implicitly kMaxPrice == 65535 when t_price was uint16_t.)\n"
        "constexpr t_price kNumPricePoints = 262144;  // 2^18\n")

# 3. kMaxLiveOrders aliased t_price's max (was 65535).  With a 32-bit t_price it
#    would balloon to 4.29e9; it is the live-order cap, not a price, so pin it to
#    the arena capacity instead.  (Defined-but-unused upstream, but kept sane.)
c = c.replace(
    "constexpr uint32_t kMaxLiveOrders = std::numeric_limits<t_price>::max();",
    "constexpr uint32_t kMaxLiveOrders = static_cast<uint32_t>(kMaxNumOrders);")
(src / "constants.h").write_text(c)

# --- order_book.cpp: size the array and the empty-ask sentinel by the new
#     dimension instead of kMaxPrice. ---
o = (src / "order_book.cpp").read_text()
o2 = o.replace("pricePoints.resize(kMaxPrice);", "pricePoints.resize(kNumPricePoints);")
assert o2 != o, "order_book.cpp: pricePoints.resize(kMaxPrice) not found"
o = o2
o2 = o.replace("askMin = kMaxPrice;", "askMin = kNumPricePoints;")
assert o2 != o, "order_book.cpp: askMin = kMaxPrice not found"
o = o2
(src / "order_book.cpp").write_text(o)

print("widened QuantCup price domain: t_price=uint32_t, kNumPricePoints=262144 "
      "(usable [1, 262143])")
PY
}

build_quantcup() {
    say "quantcup"
    local src
    if [ -n "${ME_QUANTCUP_SRC:-}" ]; then
        src="$ME_QUANTCUP_SRC"          # used as-is — assumed already patched
    else
        src="$TP/quantcup"
        clone_pinned "$QUANTCUP_URL" "$QUANTCUP_REF" "$src"
        git -C "$src" clean -fdq        # drop files added by a previous patch run
        git -C "$src" apply "$REPO/patches/quantcup.patch"
        echo "applied patches/quantcup.patch"
        widen_quantcup_price_domain "$src"
    fi
    g++ $CXXFLAGS -fexceptions -frtti -DQC_MAX_NUM_ORDERS=2200000 \
        -I "$REPO/api" -I "$src" \
        "$REPO/adapters/quantcup_adapter.cpp" \
        "$src/engine.cpp" "$src/order_book.cpp" \
        -o "$REPO/quantcup_adapter.so"
    echo "built: quantcup_adapter.so"
}

find_jdk11() {
    if [ -n "${ME_JDK11:-}" ]; then echo "$ME_JDK11"; return; fi
    local c v
    # Try the conventional naming first, then fall back to anything under
    # /usr/lib/jvm. Verify each candidate by reading its $jdk/release file and
    # requiring JAVA_VERSION to *begin* with "11." (an anchored match) — so a
    # JDK 21 build like jdk-21.0.11+9 (whose version merely contains "11") is
    # correctly rejected even though the /usr/lib/jvm/* fallback iterates it.
    for c in /usr/lib/jvm/java-11-openjdk-* /usr/lib/jvm/temurin-11-* \
             /usr/lib/jvm/jdk-11* /usr/lib/jvm/*; do
        [ -x "$c/bin/javac" ] || continue
        [ -r "$c/release" ]   || continue
        v="$(sed -n 's/^JAVA_VERSION="\(11\.[^"]*\)"$/\1/p' "$c/release")"
        [ -n "$v" ] && { echo "$c"; return; }
    done
}

build_exchange_core() {
    say "exchange_core"
    local jdk; jdk="$(find_jdk11)"
    [ -n "$jdk" ] && [ -x "$jdk/bin/javac" ] || {
        echo "ERROR: JDK 11 not found — set ME_JDK11=/path/to/jdk11" >&2; exit 1; }
    command -v mvn >/dev/null || { echo "ERROR: maven (mvn) not found" >&2; exit 1; }
    echo "JDK 11: $jdk"

    local src
    if [ -n "${ME_EXCHANGE_CORE_SRC:-}" ]; then
        src="$ME_EXCHANGE_CORE_SRC"
    else
        src="$TP/exchange-core"
        clone_pinned "$EXCHANGE_CORE_URL" "$EXCHANGE_CORE_REF" "$src"
    fi

    # exchange-core is consumed via its jar (no source patch).
    local jar
    jar="$(ls "$src"/target/exchange-core-*.jar 2>/dev/null \
           | grep -Ev 'sources|javadoc' | head -1 || true)"
    if [ -z "$jar" ]; then
        say "building the exchange-core jar (mvn package -DskipTests)"
        ( cd "$src" && JAVA_HOME="$jdk" mvn -q package -DskipTests )
        jar="$(ls "$src"/target/exchange-core-*.jar | grep -Ev 'sources|javadoc' | head -1)"
    fi
    echo "exchange-core jar: $jar"

    # Resolve exchange-core's runtime dependency classpath via maven.
    local deps_file; deps_file="$(mktemp)"
    JAVA_HOME="$jdk" mvn -q -f "$src/pom.xml" \
        dependency:build-classpath -Dmdep.outputFile="$deps_file"
    local deps; deps="$(cat "$deps_file")"; rm -f "$deps_file"

    # The adapter reads ./exchange_core.classpath at run time:
    #   EC jar : runtime deps : repo root (holds the compiled HarnessExchangeCore).
    printf '%s' "$jar:$deps:$REPO" > "$REPO/exchange_core.classpath"

    "$jdk/bin/javac" -cp "$jar:$deps" -d "$REPO" \
        "$REPO/adapters/HarnessExchangeCore.java"

    g++ $CXXFLAGS \
        -I "$REPO/api" -I "$jdk/include" -I "$jdk/include/linux" \
        -DME_JVM_LIB="\"$jdk/lib/server/libjvm.so\"" \
        "$REPO/adapters/exchange_core_adapter.cpp" \
        -o "$REPO/exchange_core_adapter.so" -ldl
    echo "built: exchange_core_adapter.so + HarnessExchangeCore.class + exchange_core.classpath"
}

for ENGINE in "${ENGINES[@]}"; do
    case "$ENGINE" in
        liquibook)                   build_liquibook ;;
        quantcup)                    build_quantcup ;;
        exchange_core|exchange-core) build_exchange_core ;;
        all)  build_liquibook; build_quantcup; build_exchange_core ;;
        *) echo "usage: $0 {liquibook|quantcup|exchange_core|all} [...]" >&2; exit 2 ;;
    esac
done

say "done"
