#!/usr/bin/env bash
# Build jlob_adapter.so — the jLOB (eliquinox/jLOB) matching core, embedded via
# JNI. jLOB is a Java L3 limit order book: bids/offers are price-sorted fastutil
# Long2ObjectRBTreeMaps of `Limit` price levels (each a FIFO ArrayList of
# `Placement`s) with a UUID->Placement index; its matcher,
# state.LimitOrderBook.place(), crosses an incoming order against the best contra
# levels price-time then rests the residual. The harness is native, so the adapter
# embeds a JVM (JNI) and drives that core through the HarnessJlob helper.
#
# This script:
#   1. clones eliquinox/jLOB at the pinned commit into third_party/jlob_jLOB (or
#      uses ME_JLOB_SRC), then `git reset --hard` to that pin;
#   2. resolves jLOB's five matcher-subset dependency jars (fastutil, guice,
#      javax.inject, guava, commons-lang3) into third_party/jlob_deps — from the
#      local ~/.m2 cache if present, else by download from Maven Central;
#   3. compiles ONLY jLOB's self-contained matcher subset (state.{LimitOrderBook,
#      Limit,LimitOrderBookListener,DummyLimitOrderBookListener}, dto.{Placement,
#      Match,Cancellation,Side}, exceptions.JLOBException) + HarnessJlob.java into
#      ./classes against those jars;
#   4. writes jlob.classpath (classes dir + jars) at the repo root for the adapter
#      to read at run time, and compiles jlob_adapter.cpp into jlob_adapter.so at
#      the repo root, baking in the JDK's libjvm.so path.
#
# NO jLOB source is patched (this engine is conforming as shipped). The matcher
# subset deliberately excludes jLOB's Redis cache (cache.Cache) and the
# PostgreSQL/jOOQ persistence listener — the full `gradle build` stands up a live
# Postgres+Redis and runs jOOQ codegen, none of which the matcher needs.
# state.LimitOrderBook references cache.Cache only from a public constructor the
# adapter never calls (it builds the book via the engine's own *private*
# LimitOrderBook(LimitOrderBookListener) ctor by reflection), so a tiny no-op
# cache.Cache COMPILE STUB — generated into ./classes-src, never written into the
# cloned engine tree — satisfies that compile-time reference. The Redis-backed
# ctor is never invoked, so Redisson is never loaded at run time. The stub is
# build scaffolding that detaches the matcher from its datastore; it is not a
# change to any matching logic. See README.md ("Source patch": none).
#
# Overrides:
#   ME_JLOB_SRC=/path/to/existing/jLOB   use an existing checkout, skip the clone
#   ME_JDK=/path/to/jdk                  use a specific JDK (else auto-detect/install)
#   ME_M2=/path/to/.m2/repository        local jar cache to copy from (default ~/.m2)
#
# Toolchain: JDK 21 (jLOB targets Java 11+; JDK 21 builds and runs it),
# g++ -std=c++20. JDK is auto-installed (apt openjdk-21-jdk-headless) only if no
# JDK is found.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

JLOB_URL="https://github.com/eliquinox/jLOB"
JLOB_REF="c78c2a2ce77c339b2343a1678f881fc9749fbd87"

# ---- 1. jLOB source at the pinned commit ----------------------------------
if [ -n "${ME_JLOB_SRC:-}" ]; then
    SRC="$ME_JLOB_SRC"
    echo ">>> using ME_JLOB_SRC=$SRC"
else
    SRC="$TP/jlob_jLOB"
    if [ ! -d "$SRC/.git" ]; then
        echo ">>> cloning jLOB into $SRC"
        git clone --quiet "$JLOB_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$JLOB_REF"
    echo ">>> jLOB pinned at $JLOB_REF"
fi
[ -f "$SRC/src/main/java/state/LimitOrderBook.java" ] || {
    echo "ERROR: $SRC does not look like a jLOB checkout (state/LimitOrderBook.java missing)"; exit 1; }

# ---- 2. JDK (auto-install only if absent) ---------------------------------
find_jdk() {
    if [ -n "${ME_JDK:-}" ] && [ -x "$ME_JDK/bin/javac" ]; then echo "$ME_JDK"; return; fi
    # Prefer an installed JDK 21; fall back to any JDK with javac.
    for d in /usr/lib/jvm/java-21-openjdk-* /usr/lib/jvm/temurin-21-* \
             /usr/lib/jvm/java-21-* /usr/lib/jvm/*; do
        [ -x "$d/bin/javac" ] && { echo "$d"; return; }
    done
    # Derive from javac on PATH if present.
    if command -v javac >/dev/null 2>&1; then
        local jc; jc="$(readlink -f "$(command -v javac)")"
        echo "$(dirname "$(dirname "$jc")")"; return
    fi
    echo ""
}
JDK="$(find_jdk)"
if [ -z "$JDK" ]; then
    echo ">>> no JDK found — installing openjdk-21-jdk-headless via apt"
    if command -v apt-get >/dev/null 2>&1; then
        sudo apt-get update -qq
        sudo apt-get install -y -qq openjdk-21-jdk-headless
        JDK="$(find_jdk)"
    fi
fi
[ -n "$JDK" ] && [ -x "$JDK/bin/javac" ] || {
    echo "ERROR: no usable JDK (set ME_JDK=/path/to/jdk-21)"; exit 1; }
echo ">>> JDK: $JDK"
JVM_LIB="$JDK/lib/server/libjvm.so"
[ -f "$JVM_LIB" ] || { echo "ERROR: libjvm.so not found under $JDK/lib/server"; exit 1; }

# ---- 3. dependency jars (local ~/.m2 fast-path, else Maven Central) --------
DEPDIR="$TP/jlob_deps"
mkdir -p "$DEPDIR"
M2="${ME_M2:-$HOME/.m2/repository}"
MAVEN_CENTRAL="https://repo1.maven.org/maven2"

# name|maven-path-under-repo
DEP_SPECS=(
  "fastutil-8.3.0.jar|it/unimi/dsi/fastutil/8.3.0"
  "guice-4.2.3.jar|com/google/inject/guice/4.2.3"
  "javax.inject-1.jar|javax/inject/javax.inject/1"
  "guava-14.0.1.jar|com/google/guava/guava/14.0.1"
  "commons-lang3-3.12.0.jar|org/apache/commons/commons-lang3/3.12.0"
)
DEP_JARS=()
for spec in "${DEP_SPECS[@]}"; do
    jar="${spec%%|*}"; rel="${spec##*|}"
    dst="$DEPDIR/$jar"
    if [ ! -s "$dst" ]; then
        if [ -f "$M2/$rel/$jar" ]; then
            cp "$M2/$rel/$jar" "$dst"
            echo ">>> dep $jar (from local cache)"
        else
            echo ">>> dep $jar (download from Maven Central)"
            curl -fsSL --retry 3 -o "$dst" "$MAVEN_CENTRAL/$rel/$jar" \
                || { echo "ERROR: could not fetch $jar"; rm -f "$dst"; exit 1; }
        fi
    fi
    [ -s "$dst" ] || { echo "ERROR: dependency jar empty: $dst"; exit 1; }
    DEP_JARS+=("$dst")
done
DEP_CP="$(IFS=:; echo "${DEP_JARS[*]}")"

# ---- 4. compile the matcher subset + helper -------------------------------
# Build output (the generated stub source + compiled classes) lives under the
# gitignored third_party/ tree, so the committed adapter directory holds only
# authored files (jlob_adapter.cpp, HarnessJlob.java, build.sh, README.md).
BUILD="$TP/jlob_build"
echo ">>> generating the no-op cache.Cache compile stub (build scaffolding, not engine source)"
STUBSRC="$BUILD/classes-src"
rm -rf "$STUBSRC"
mkdir -p "$STUBSRC/cache"
cat > "$STUBSRC/cache/Cache.java" <<'JAVA'
package cache;
import state.LimitOrderBook;
/* Build-only stub: detaches jLOB's matcher from its Redis datastore. The adapter
 * never constructs the engine's Redis-backed book (it uses the engine's own
 * private LimitOrderBook(LimitOrderBookListener) ctor by reflection), so these
 * methods are never called at run time; this stub only satisfies the compile-time
 * reference LimitOrderBook makes to cache.Cache. No matching logic is involved. */
public class Cache {
    public boolean bookKeyExists() { return false; }
    public LimitOrderBook getLimitOrderBook() { return null; }
    public void cacheLimitOrderBook(LimitOrderBook lob) { }
}
JAVA

echo ">>> compiling jLOB matcher subset + HarnessJlob ($("$JDK/bin/javac" -version 2>&1))"
CLASSES="$BUILD/classes"
rm -rf "$CLASSES"; mkdir -p "$CLASSES"
SRCS=(
  "$SRC/src/main/java/state/LimitOrderBook.java"
  "$SRC/src/main/java/state/Limit.java"
  "$SRC/src/main/java/state/LimitOrderBookListener.java"
  "$SRC/src/main/java/state/DummyLimitOrderBookListener.java"
  "$SRC/src/main/java/dto/Placement.java"
  "$SRC/src/main/java/dto/Match.java"
  "$SRC/src/main/java/dto/Cancellation.java"
  "$SRC/src/main/java/dto/Side.java"
  "$SRC/src/main/java/exceptions/JLOBException.java"
  "$STUBSRC/cache/Cache.java"
  "$DIR/HarnessJlob.java"
)
"$JDK/bin/javac" -encoding UTF-8 -nowarn -cp "$DEP_CP" -d "$CLASSES" "${SRCS[@]}"

# ---- 5. classpath + adapter .so -------------------------------------------
echo ">>> writing $REPO/jlob.classpath"
# Classpath the adapter reads at run time: the compiled matcher+helper, then deps.
printf '%s' "$CLASSES:$DEP_CP" > "$REPO/jlob.classpath"

echo ">>> compiling jlob_adapter.so"
g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -I"$REPO/api" -I"$JDK/include" -I"$JDK/include/linux" \
    -DME_JVM_LIB="\"$JVM_LIB\"" \
    "$DIR/jlob_adapter.cpp" \
    -o "$REPO/jlob_adapter.so" -ldl
echo "built: $REPO/jlob_adapter.so (+ $REPO/jlob.classpath, $CLASSES/HarnessJlob.class)"
ls -la "$REPO/jlob_adapter.so"
