#!/usr/bin/env bash
# Build coralme_adapter.so — the CoralME (coralblocks/CoralME) matching core,
# embedded via JNI. CoralME is a garbage-free Java matching engine: a
# price-ordered doubly-linked list of price levels, a per-level FIFO order list
# (time priority), and a LongMap id index, all backed by object pools. The
# harness is native, so this adapter embeds a JVM (JNI) and drives a CoralME
# OrderBook through a small Java helper (HarnessCoralMe).
#
# This script:
#   1. clones coralblocks/CoralME at the pinned commit into
#      third_party/coralme_CoralME (or uses ME_CORALME_SRC), then `git reset
#      --hard` to that pin;
#   2. builds CoralME's self-contained shaded jar (coralme-all.jar) with Maven
#      (-DskipTests, javadoc/sources skipped). The shaded jar bundles CoralME's
#      two runtime deps (CoralPool + CoralDS), which Maven resolves from the
#      project's own JitPack repository (or the local ~/.m2 cache if present);
#   3. compiles HarnessCoralMe.java against that jar into
#      third_party/coralme_build/classes;
#   4. writes coralme.classpath (the shaded jar + the helper's class dir) at the
#      repo root for the adapter to read at run time, and compiles
#      coralme_adapter.cpp into coralme_adapter.so at the repo root, baking in
#      the JDK's libjvm.so path.
#
# NO CoralME source is patched (this engine is conforming as shipped — see
# CORRECTNESS_FINDINGS.md / README.md "Source patch"). The clone is left
# byte-for-byte pristine (only `mvn` writes into its target/); all adapter glue
# lives in coralme_adapter.cpp and HarnessCoralMe.java.
#
# Overrides:
#   ME_CORALME_SRC=/path/to/existing/CoralME  use an existing checkout, skip clone
#   ME_JDK=/path/to/jdk                        use a specific JDK (else auto-detect/install)
#
# Toolchain: JDK 21 (CoralME targets bytecode 17 — JDK 21 builds and runs it),
# Maven, g++ -std=c++20. The JDK is auto-installed (apt openjdk-21-jdk-headless)
# only if no JDK is found; Maven must already be on PATH.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

CORALME_URL="https://github.com/coralblocks/CoralME"
CORALME_REF="6d0f94898f05ca7059a79551132be16c17785863"

# ---- 1. CoralME source at the pinned commit -------------------------------
if [ -n "${ME_CORALME_SRC:-}" ]; then
    SRC="$ME_CORALME_SRC"
    echo ">>> using ME_CORALME_SRC=$SRC"
else
    SRC="$TP/coralme_CoralME"
    if [ ! -d "$SRC/.git" ]; then
        echo ">>> cloning CoralME into $SRC"
        git clone --quiet "$CORALME_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$CORALME_REF"
    echo ">>> CoralME pinned at $CORALME_REF"
fi
[ -f "$SRC/src/main/java/com/coralblocks/coralme/OrderBook.java" ] || {
    echo "ERROR: $SRC does not look like a CoralME checkout (OrderBook.java missing)"; exit 1; }

# ---- 2. JDK (auto-install only if absent) ---------------------------------
find_jdk() {
    if [ -n "${ME_JDK:-}" ] && [ -x "$ME_JDK/bin/javac" ]; then echo "$ME_JDK"; return; fi
    # Prefer an installed JDK 21; fall back to any JDK with javac.
    for d in /usr/lib/jvm/java-21-openjdk-* /usr/lib/jvm/temurin-21-* \
             /usr/lib/jvm/java-1.21.0-openjdk-* /usr/lib/jvm/java-21-* /usr/lib/jvm/*; do
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

command -v mvn >/dev/null 2>&1 || { echo "ERROR: maven (mvn) not found on PATH"; exit 1; }

# ---- 3. CoralME shaded jar (Maven resolves CoralPool + CoralDS) -----------
echo ">>> building the CoralME shaded jar (mvn package -DskipTests)"
JAR="$(ls "$SRC"/target/coralme-all.jar 2>/dev/null || true)"
if [ -z "$JAR" ]; then
    ( cd "$SRC" && JAVA_HOME="$JDK" mvn -q -DskipTests -Dmaven.javadoc.skip=true \
        -Dsource.skip=true package )
    JAR="$(ls "$SRC"/target/coralme-all.jar)"
fi
[ -s "$JAR" ] || { echo "ERROR: CoralME shaded jar not produced: $JAR"; exit 1; }
echo ">>> CoralME jar: $JAR"

# ---- 4. compile the helper into a gitignored build dir --------------------
# Output (the compiled helper class) lives under the gitignored third_party/
# tree so the committed adapter directory holds only authored files
# (coralme_adapter.cpp, HarnessCoralMe.java, build.sh, README.md).
BUILD="$TP/coralme_build"
CLASSES="$BUILD/classes"
rm -rf "$CLASSES"; mkdir -p "$CLASSES"
echo ">>> compiling HarnessCoralMe.java ($("$JDK/bin/javac" -version 2>&1))"
"$JDK/bin/javac" -encoding UTF-8 -nowarn -cp "$JAR" -d "$CLASSES" "$DIR/HarnessCoralMe.java"

# ---- 5. classpath + adapter .so -------------------------------------------
echo ">>> writing $REPO/coralme.classpath"
# Classpath the adapter reads at run time: the self-contained shaded jar (engine
# + bundled deps), then the helper's class dir.
printf '%s' "$JAR:$CLASSES" > "$REPO/coralme.classpath"

echo ">>> compiling coralme_adapter.so"
g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -I"$REPO/api" -I"$JDK/include" -I"$JDK/include/linux" \
    -DME_JVM_LIB="\"$JVM_LIB\"" \
    "$DIR/coralme_adapter.cpp" \
    -o "$REPO/coralme_adapter.so" -ldl
echo "built: $REPO/coralme_adapter.so (+ $REPO/coralme.classpath, $CLASSES/HarnessCoralMe.class)"
ls -la "$REPO/coralme_adapter.so"
