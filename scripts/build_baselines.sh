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

ENGINE="${1:-all}"
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
    # /usr/lib/jvm. Verify each candidate by reading $jdk/release — a
    # substring match on the path name accepts JDK 21 builds whose version
    # string contains "11" (e.g. jdk-21.0.11+9).
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

case "$ENGINE" in
    liquibook)                   build_liquibook ;;
    quantcup)                    build_quantcup ;;
    exchange_core|exchange-core) build_exchange_core ;;
    all)  build_liquibook; build_quantcup; build_exchange_core ;;
    *) echo "usage: $0 {liquibook|quantcup|exchange_core|all}" >&2; exit 2 ;;
esac

say "done"
