#!/usr/bin/env bash
# Build jiang_adapter.so — JiangYongKang/FastMatchingEngine, embedded via JNI.
# FastMatchingEngine is a dependency-free Java digital-currency matching-engine
# POC: a price-time-priority CLOB (TreeMap<BigDecimal,OrderBucket> per side,
# each bucket a FIFO LinkedHashMap, plus a HashMap id index). The harness is
# native, so the adapter embeds a JVM (JNI) and drives a
# com.fast.matching.engine.OrderBook through the HarnessJiang Java helper.
#
# This script:
#   1. clones JiangYongKang/FastMatchingEngine at the pinned commit into
#      third_party/jiang_FastMatchingEngine (or uses ME_JIANG_SRC), then
#      `git reset --hard` to that pin;
#   2. applies three minimal, idempotent engine-source patches (see PATCHES
#      below) to OrderBook.java;
#   3. compiles the five engine sources + HarnessJiang.java into
#      third_party/jiang_build/classes with the system javac (the engine has no
#      third-party dependencies);
#   4. writes jiang.classpath (the classes dir) at the repo root for the adapter
#      to read at run time, and compiles jiang_adapter.cpp into jiang_adapter.so
#      at the repo root, baking in the JDK's libjvm.so path.
#
# All generated output lands under the gitignored third_party/ tree (plus the
# regenerated jiang.classpath + jiang_adapter.so at the repo root); the adapter
# directory itself holds only the authored jiang_adapter.cpp, HarnessJiang.java,
# build.sh, and README.md.
#
# PATCHES (engine source, applied idempotently after `git reset --hard <pin>`,
# each guarded by a marker so a re-apply — e.g. under ME_JIANG_SRC — is a no-op,
# and anchor-checked so an upstream change can't silently no-op the fix):
#  1) OrderBook.cancelOrder(): add `idMaps.remove(id)`. REAL BUG FIX, reported
#     upstream as
#     https://github.com/JiangYongKang/FastMatchingEngine/issues/3 .
#     cancelOrder removed the order from its price bucket but never pruned the id
#     index, so a cancelled id was permanently burned: re-adding it was silently
#     dropped (newOrder rests only if the id is absent) and a second cancel of it
#     NPE'd (bucketMap.get returned null after the now-empty bucket was removed).
#     This breaks the modify path entirely (modify = cancel + reinsert under the
#     same id) and crashes on a stale re-cancel.
#  2) OrderBook.newOrder() match loop: prune idMaps for a maker that is fully
#     consumed. REAL BUG FIX — the same burned-id defect on the MATCH path:
#     OrderBucket.doExchange removes a fully-filled maker from its bucket but
#     never from idMaps, so a later cancel/modify of that maker would NPE the
#     same way. After each doExchange, any returned-trade maker whose remaining
#     volume is now zero is removed from idMaps. The maker Order in idMaps is the
#     same reference held by the bucket, so the remaining-volume test is exact.
#     This changes no matching logic — it only keeps the id index in sync with
#     the book, so idMaps is the authoritative resting set.
#  3) OrderBook.getOrder(): add a one-line read-only accessor returning
#     idMaps.get(id). The harness cancel/modify carry only an order_id, but the
#     helper needs (a) a not-resting test for CancelReject/ModifyReject and
#     (b) the resting order's price+side for the CancelAck — and cancelOrder()
#     itself NPEs on a non-resting id (idMaps.get(id) == null -> order.action()).
#     With PATCHES 1+2 making idMaps authoritative, this accessor lets the helper
#     ask the engine its own state instead of keeping a parallel adapter shadow.
#     Pure observation — no matching logic touched.
#
# Overrides:
#   ME_JIANG_SRC=/path/to/existing/FastMatchingEngine  use an existing checkout,
#                                                       skip the clone
#   ME_JDK=/path/to/jdk                                 use a specific JDK
#                                                       (else auto-detect/install)
#
# Toolchain: JDK 21 (the engine targets Java 1.8; any modern JDK builds and runs
# it), g++ -std=c++20. JDK is auto-installed (apt openjdk-21-jdk-headless) only
# if no JDK is found.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

JIANG_URL="https://github.com/JiangYongKang/FastMatchingEngine"
JIANG_REF="8a3b597a042e402cd8bd5c95fc2d3b0884913022"

# ---- 1. FastMatchingEngine source at the pinned commit --------------------
if [ -n "${ME_JIANG_SRC:-}" ]; then
    SRC="$ME_JIANG_SRC"
    echo ">>> using ME_JIANG_SRC=$SRC"
else
    SRC="$TP/jiang_FastMatchingEngine"
    if [ ! -d "$SRC/.git" ]; then
        echo ">>> cloning FastMatchingEngine into $SRC"
        git clone --quiet "$JIANG_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$JIANG_REF"
    echo ">>> FastMatchingEngine pinned at $JIANG_REF"
fi
OB="$SRC/src/main/java/com/fast/matching/engine/OrderBook.java"
[ -f "$OB" ] || {
    echo "ERROR: $SRC does not look like a FastMatchingEngine checkout (OrderBook.java missing)"
    exit 1; }

# ---- 2. JDK (auto-install only if absent) ---------------------------------
find_jdk() {
    if [ -n "${ME_JDK:-}" ] && [ -x "$ME_JDK/bin/javac" ]; then echo "$ME_JDK"; return; fi
    # Prefer an installed JDK 21; fall back to any JDK with javac.
    for d in /usr/lib/jvm/java-21-openjdk-* /usr/lib/jvm/temurin-21-* \
             /usr/lib/jvm/java-1.21.0-* /usr/lib/jvm/java-21-* /usr/lib/jvm/*; do
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

# ---- 3. engine-source patches (idempotent, anchor-checked) ----------------
echo ">>> applying engine patches to OrderBook.java"
python3 - "$OB" <<'PY'
import sys, io
f = sys.argv[1]
s = io.open(f, encoding="utf-8").read()

# PATCH 2 — newOrder match loop: prune idMaps for a fully-consumed maker.
m_anchor = ("            List<Trade> trades = orderBucket.doExchange(order);\n"
            "            this.trades.addAll(trades);\n")
m_inject = ("            List<Trade> trades = orderBucket.doExchange(order);\n"
            "            this.trades.addAll(trades);\n"
            "            for (Trade __t : trades) { // HARNESS PATCH 2 (see build.sh): prune filled makers from idMaps\n"
            "                Order __maker = idMaps.get(__t.targetOrderId());\n"
            "                if (__maker != null && __maker.commissionVolume().compareTo(BigDecimal.ZERO) == 0) {\n"
            "                    idMaps.remove(__t.targetOrderId());\n"
            "                }\n"
            "            }\n")
if "HARNESS PATCH 2" not in s:
    assert m_anchor in s, "PATCH 2: newOrder doExchange anchor not found (upstream changed?)"
    s = s.replace(m_anchor, m_inject, 1)

# PATCH 3 — add a read-only id-index accessor just before cancelOrder.
g_anchor = "    public void cancelOrder(Long id) {\n"
g_inject = ("    public Order getOrder(Long id) { // HARNESS PATCH 3 (see build.sh): read-only resting lookup\n"
            "        return idMaps.get(id);\n"
            "    }\n"
            "\n"
            "    public void cancelOrder(Long id) {\n")
if "HARNESS PATCH 3" not in s:
    assert g_anchor in s, "PATCH 3: cancelOrder anchor not found (upstream changed?)"
    s = s.replace(g_anchor, g_inject, 1)

# PATCH 1 — cancelOrder: prune the id index once the order has been read.
c_anchor = ("    public void cancelOrder(Long id) {\n"
            "        Order order = idMaps.get(id);\n")
c_inject = ("    public void cancelOrder(Long id) {\n"
            "        Order order = idMaps.get(id);\n"
            "        idMaps.remove(id); // HARNESS PATCH 1 (see build.sh): free the cancelled id\n")
if "HARNESS PATCH 1" not in s:
    assert c_anchor in s, "PATCH 1: cancelOrder anchor not found (upstream changed?)"
    s = s.replace(c_anchor, c_inject, 1)

io.open(f, "w", encoding="utf-8").write(s)
print("    patched OrderBook.java (cancelOrder prune, match-path prune, getOrder accessor)")
PY

# ---- 4. compile engine sources + HarnessJiang.java -> classes -------------
BUILD="$TP/jiang_build"
CLASSES="$BUILD/classes"
echo ">>> compiling engine sources + HarnessJiang ($("$JDK/bin/javac" -version 2>&1))"
rm -rf "$CLASSES"; mkdir -p "$CLASSES"
find "$SRC/src/main/java" -name "*.java" > "$BUILD/.jiang_srcs.txt"
"$JDK/bin/javac" -encoding UTF-8 -nowarn -d "$CLASSES" @"$BUILD/.jiang_srcs.txt"
"$JDK/bin/javac" -encoding UTF-8 -nowarn -cp "$CLASSES" -d "$CLASSES" "$DIR/HarnessJiang.java"
rm -f "$BUILD/.jiang_srcs.txt"

# ---- 5. classpath + adapter .so -------------------------------------------
echo ">>> writing $REPO/jiang.classpath"
# Single classpath entry the adapter reads at run time: the compiled engine +
# HarnessJiang classes. (The engine has no third-party jars.)
printf '%s' "$CLASSES" > "$REPO/jiang.classpath"

echo ">>> compiling jiang_adapter.so"
g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -I"$REPO/api" -I"$JDK/include" -I"$JDK/include/linux" \
    -DME_JVM_LIB="\"$JVM_LIB\"" \
    "$DIR/jiang_adapter.cpp" \
    -o "$REPO/jiang_adapter.so" -ldl
echo "built: $REPO/jiang_adapter.so (+ $REPO/jiang.classpath, $CLASSES/HarnessJiang.class)"
ls -la "$REPO/jiang_adapter.so"
