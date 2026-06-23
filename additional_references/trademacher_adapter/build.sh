#!/usr/bin/env bash
# Build trademacher_adapter.so — the TradeMatcher (TradeMatcher/match-engine,
# Maven artifact match-engine-core, package com.tradematcher) Java matching core,
# embedded via JNI. TradeMatcher is a Java price-time-priority engine: a per-symbol
# OrderBookImpl with a TreeMap of price "buckets", a single cross-bucket
# doubly-linked order list per side carrying time priority, and a
# HashMap<String,Order> id index, with an LMAX-Disruptor command pipeline on top.
# The harness is native, so the adapter embeds a JVM (JNI) and drives that
# separable matching core (OrderBookImpl) directly — exactly as the engine's own
# GTCTest/FAKTest do — through the HarnessTradeMacher helper, with NO Disruptor,
# WebSocket, journal, or snapshot in the loop.
#
# This script:
#   1. clones TradeMatcher/match-engine at the pinned commit into
#      third_party/trademacher_match_engine (or uses ME_TM_SRC), then
#      `git reset --hard` to that pin;
#   2. builds its self-contained shaded jar (target/match-engine-core-1.0-SNAPSHOT.jar,
#      which the maven-shade plugin bundles with all deps: LMAX Disruptor, gson,
#      logback, ...) with Maven, skipping tests + javadoc;
#   3. compiles HarnessTradeMacher.java against that jar into
#      third_party/trademacher_build/classes;
#   4. writes trademacher.classpath (the shaded jar + that classes dir) at the repo
#      root for the adapter to read at run time, and compiles
#      trademacher_adapter.cpp into trademacher_adapter.so at the repo root, baking
#      in the JDK's libjvm.so path.
#
# NO TradeMatcher source is patched (this engine is conforming as shipped — see
# CORRECTNESS_FINDINGS.md / CONSENSUS_CONFORMING_ENGINES.md). All harness glue
# (per-message API, report derivation, modify = cancel + reinsert) lives in the
# adapter (trademacher_adapter.cpp) and the helper (HarnessTradeMacher.java).
# `git reset --hard` to the pin leaves the cloned engine tree byte-for-byte
# pristine; all build output lands under the gitignored third_party/ tree.
#
# Toolchain: JDK 17 — the engine pom targets bytecode 17 AND pins Lombok 1.18.26,
# which does not understand the JDK 21 compiler internals; JDK 17 is what the
# engine's own Dockerfile uses (maven:3.8.5-openjdk-17). Maven (mvn). g++ -std=c++20.
# JDK 17 is auto-installed (apt openjdk-17-jdk-headless) only if no JDK 17 is found.
#
# Overrides:
#   ME_TM_SRC=/path/to/existing/match-engine   use an existing checkout, skip clone
#   ME_JDK17=/path/to/jdk-17                    use a specific JDK 17
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

TM_URL="https://github.com/TradeMatcher/match-engine"
TM_REF="552c71a83f0d28808048189a1153a6463ea661ef"

# ---- 1. TradeMatcher source at the pinned commit --------------------------
if [ -n "${ME_TM_SRC:-}" ]; then
    SRC="$ME_TM_SRC"
    echo ">>> using ME_TM_SRC=$SRC"
else
    SRC="$TP/trademacher_match_engine"
    if [ ! -d "$SRC/.git" ]; then
        echo ">>> cloning TradeMatcher/match-engine into $SRC"
        git clone --quiet "$TM_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$TM_REF"
    echo ">>> TradeMatcher pinned at $TM_REF"
fi
[ -f "$SRC/pom.xml" ] || {
    echo "ERROR: $SRC does not look like a match-engine checkout (pom.xml missing)"; exit 1; }

# ---- 2. JDK 17 (auto-install only if absent) ------------------------------
# The engine pins Lombok 1.18.26 (broken against the JDK 21 javac internals) and
# targets bytecode 17, so the engine jar MUST be built with JDK 17.
find_jdk17() {
    if [ -n "${ME_JDK17:-}" ] && [ -x "$ME_JDK17/bin/javac" ]; then echo "$ME_JDK17"; return; fi
    for d in /usr/lib/jvm/java-17-openjdk-* /usr/lib/jvm/java-1.17.0-openjdk-* \
             /usr/lib/jvm/temurin-17-* /usr/lib/jvm/java-17-* /usr/lib/jvm/*-17-*; do
        [ -x "$d/bin/javac" ] || continue
        # Confirm it really is a 17 javac.
        "$d/bin/javac" -version 2>&1 | grep -q ' 17' && { echo "$d"; return; }
    done
    echo ""
}
JDK="$(find_jdk17)"
if [ -z "$JDK" ]; then
    echo ">>> no JDK 17 found — installing openjdk-17-jdk-headless via apt"
    if command -v apt-get >/dev/null 2>&1; then
        sudo apt-get update -qq
        sudo apt-get install -y -qq openjdk-17-jdk-headless
        JDK="$(find_jdk17)"
    fi
fi
[ -n "$JDK" ] && [ -x "$JDK/bin/javac" ] || {
    echo "ERROR: no usable JDK 17 (set ME_JDK17=/path/to/jdk-17; apt install openjdk-17-jdk-headless)"; exit 1; }
echo ">>> JDK 17: $JDK"
JVM_LIB="$JDK/lib/server/libjvm.so"
[ -f "$JVM_LIB" ] || { echo "ERROR: libjvm.so not found under $JDK/lib/server"; exit 1; }

command -v mvn >/dev/null 2>&1 || { echo "ERROR: maven (mvn) not found on PATH"; exit 1; }

# ---- 3. the engine's self-contained shaded jar ----------------------------
JAR="$SRC/target/match-engine-core-1.0-SNAPSHOT.jar"
if [ ! -f "$JAR" ]; then
    echo ">>> building the shaded jar (mvn -DskipTests clean package)"
    ( cd "$SRC" && JAVA_HOME="$JDK" mvn -q -DskipTests -Dmaven.javadoc.skip=true clean package )
fi
[ -f "$JAR" ] || { echo "ERROR: engine jar not built: $JAR"; exit 1; }
echo ">>> TradeMatcher jar: $JAR"

# ---- 4. compile the Java helper -------------------------------------------
# Build output (the compiled helper class) lives under the gitignored third_party/
# tree, so the committed adapter directory holds only authored files
# (trademacher_adapter.cpp, HarnessTradeMacher.java, build.sh, README.md).
BUILD="$TP/trademacher_build"
CLASSES="$BUILD/classes"
rm -rf "$CLASSES"; mkdir -p "$CLASSES"
echo ">>> compiling HarnessTradeMacher.java ($("$JDK/bin/javac" -version 2>&1))"
"$JDK/bin/javac" -encoding UTF-8 -cp "$JAR" -d "$CLASSES" "$DIR/HarnessTradeMacher.java"

# ---- 5. classpath + adapter .so -------------------------------------------
# Classpath the adapter reads at run time: the self-contained shaded jar, then the
# compiled helper class directory.
echo ">>> writing $REPO/trademacher.classpath"
printf '%s' "$JAR:$CLASSES" > "$REPO/trademacher.classpath"

echo ">>> compiling trademacher_adapter.so"
g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -I"$REPO/api" -I"$JDK/include" -I"$JDK/include/linux" \
    -DME_JVM_LIB="\"$JVM_LIB\"" \
    "$DIR/trademacher_adapter.cpp" \
    -o "$REPO/trademacher_adapter.so" -ldl
echo "built: $REPO/trademacher_adapter.so (+ $REPO/trademacher.classpath, $CLASSES/HarnessTradeMacher.class)"
ls -la "$REPO/trademacher_adapter.so"
