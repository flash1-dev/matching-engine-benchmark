#!/usr/bin/env bash
# Build cointossx_adapter.so — the CoinTossX (dharmeshsing/CoinTossX) matching
# core, embedded via JNI. CoinTossX is a full Java JSE exchange on Aeron/Agrona,
# but its MATCHING ENGINE is a separable in-process core: a per-security
# `orderBook.OrderBook` (custom B+Tree price index + off-heap sun.misc.Unsafe
# order lists) crossed by
# `crossing.tradingSessions.ContinuousTradingProcessor.process(OrderBook,
# OrderEntry)` under the price-time-priority strategy. The harness is native, so
# the adapter embeds a JVM (JNI) and drives that core through the HarnessCoinTossX
# helper — no Aeron, UDP, or disruptor in the loop.
#
# This script:
#   1. clones dharmeshsing/CoinTossX at the pinned commit into
#      third_party/cointossx_CoinTossX (or uses ME_CTX_SRC), then `git reset
#      --hard` to that pin (no git submodules; the one vendored jar,
#      lib/ObjectLayout-1.0.5-SNAPSHOT.jar, is a tracked file the reset restores);
#   2. applies three minimal, idempotent, anchor-checked engine patches (see
#      PATCHES below) — two real matching-bug FIXES (filed upstream as
#      https://github.com/dharmeshsing/CoinTossX/issues/10) plus one
#      observation-only per-fill sink the harness Trade report needs;
#   3. resolves CoinTossX's eight matcher dependency jars (hppc, aeron-client,
#      aeron-driver, Agrona, sbe, joda-time, HdrHistogram, commons-csv) into
#      third_party/cointossx_deps — from the local ~/.m2 cache if present, else by
#      download from Maven Central — plus the vendored ObjectLayout snapshot jar
#      from the clone's lib/;
#   4. compiles ONLY the matcher core (LimitOrderBook + MatchingEngine + Messages
#      + Socket main sources) + HarnessCoinTossX.java on JDK 11, jars the result,
#      writes cointossx.classpath at the repo root for the adapter to read at run
#      time, and compiles cointossx_adapter.cpp into cointossx_adapter.so at the
#      repo root (with the JDK's libjvm.so path baked in).
#
# JDK 11 is used deliberately: CoinTossX (2015, Java 8) reads sun.misc.Unsafe via
# theUnsafe-field reflection (off-heap order nodes), which JDK 11 exposes without
# --add-opens; this matches the exchange-core adapter's JDK 11 baseline. Sources
# are compiled -source 8 -target 8 (the engine's Java level).
#
# PATCHES (engine source, applied post-reset, idempotent, anchor-checked):
#  1) BPlusTree.getFirstKey() — real matching-bug FIX (issue #10). The accessor
#     OrderBook.getBestBid()/getBestOffer() read returned null on any book deeper
#     than nodeSize (=100) price levels: on a settled BRANCH root, root.firstKey()
#     reads the transient, one-shot DESTRUCTIVE split-key slot (null when settled)
#     instead of the subtree's smallest key. Best-bid/offer then collapsed to 0
#     and marketable orders rested instead of crossing -> crossed/locked book. The
#     fix descends the leftmost-child path to the first leaf using only
#     non-destructive accessors; no split/merge/rebalance logic is changed.
#  2) AddOrderPreProcessor — real matching-bug FIX (issue #10). preProcess()
#     decided whether a LIMIT order crosses by comparing its price against its OWN
#     side's best (buy vs bestBid, sell vs bestOffer) instead of the CONTRA side;
#     when a side is empty its best is 0 and the guard is skipped, so a
#     non-marketable order falls through to AGGRESS, and the ADD_AND_AGGRESS touch
#     branch mutates the book in a different order than aggress-then-rest. Fixed to
#     test the contra-side best (buy vs bestOffer, sell vs bestBid) and route every
#     marketable visible LIMIT order through AGGRESS_ORDER.
#  3) PriceTimePriorityStrategy — OBSERVATION-ONLY (not a matching change).
#     CoinTossX's own fill record (ExecutionReportData.addFillGroup(price,qty))
#     collapses same-price fills and keeps NO counterparty ids, so it cannot supply
#     the per-fill maker/taker order ids the harness Trade report needs. The patch
#     adds a public static FillSink and emits one fill (maker=currentOrder,
#     taker=aggOrder, price, qty — all already in scope) per fill in
#     processOrdersInList. It changes no matching logic and can neither hide nor
#     create a matching bug.
#
# Overrides:
#   ME_CTX_SRC=/path/to/existing/CoinTossX   use an existing checkout, skip clone
#   ME_JDK=/path/to/jdk-11                    use a specific JDK 11 (else detect/install)
#   ME_M2=/path/to/.m2/repository             local jar cache to copy from (default ~/.m2)
#
# Toolchain: JDK 11 (sun.misc.Unsafe reflection; engine sources are Java 8),
# g++ -std=c++17. JDK 11 is auto-installed (apt openjdk-11-jdk-headless) only if
# no JDK 11 is found.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

CTX_URL="https://github.com/dharmeshsing/CoinTossX"
CTX_REF="89090edcd15a06f4ed821890adfc8f377ed7d7c7"

# ---- 1. CoinTossX source at the pinned commit -----------------------------
if [ -n "${ME_CTX_SRC:-}" ]; then
    SRC="$ME_CTX_SRC"
    echo ">>> using ME_CTX_SRC=$SRC"
else
    SRC="$TP/cointossx_CoinTossX"
    if [ ! -d "$SRC/.git" ]; then
        echo ">>> cloning CoinTossX into $SRC"
        git clone --quiet "$CTX_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$CTX_REF"
    echo ">>> CoinTossX pinned at $CTX_REF"
fi
[ -f "$SRC/MatchingEngine/src/main/java/crossing/strategy/PriceTimePriorityStrategy.java" ] || {
    echo "ERROR: $SRC does not look like a CoinTossX checkout (PriceTimePriorityStrategy.java missing)"; exit 1; }

# ---- 2. engine patches (idempotent, anchor-checked, post-reset) ------------
echo ">>> applying engine patches (issue #10 matching fixes + observation sink)"
python3 - "$SRC" <<'PY'
import sys, io, os
root = sys.argv[1]

# --- PATCH 3 (observation-only): PriceTimePriorityStrategy per-fill sink ----
# CoinTossX's fill record collapses same-price fills and keeps no counterparty
# ids, so it cannot supply the per-fill maker/taker order ids the harness Trade
# report needs. Add a public static sink + emit one fill (data already in scope:
# currentOrder=maker, aggOrder=taker, price, quantity) per fill. No matching
# logic is changed.
f = os.path.join(root, "MatchingEngine/src/main/java/crossing/strategy/PriceTimePriorityStrategy.java")
s = io.open(f, encoding="utf-8").read()
anchor_field = "public class PriceTimePriorityStrategy implements MatchingLogic {\n    private long targetPrice;\n"
inject_field = ("public class PriceTimePriorityStrategy implements MatchingLogic {\n"
                "    // HARNESS PATCH: per-fill observation sink (maker, taker, price, qty).\n"
                "    public interface FillSink { void onFill(long maker, long taker, long price, long qty); }\n"
                "    public static FillSink HARNESS_SINK = null;\n"
                "    private long targetPrice;\n")
if "HARNESS_SINK" not in s:
    assert anchor_field in s, "PriceTimePriorityStrategy field anchor not found (upstream changed?)"
    s = s.replace(anchor_field, inject_field, 1)
anchor_fill = ("                addTrade(price, quantity, currentOrder.getClientOrderId(), java.time.Instant.now().toEpochMilli());\n")
inject_fill = (anchor_fill +
               "                if (HARNESS_SINK != null) HARNESS_SINK.onFill(currentOrder.getClientOrderId(), aggOrder.getClientOrderId(), price, quantity);\n")
if "HARNESS_SINK.onFill" not in s:
    assert anchor_fill in s, "PriceTimePriorityStrategy fill anchor not found (upstream changed?)"
    s = s.replace(anchor_fill, inject_fill, 1)
io.open(f, "w", encoding="utf-8").write(s)
print("    patched PriceTimePriorityStrategy (observation sink)")

# --- PATCH 1: BPlusTree.getFirstKey() destructive-read FIX (issue #10) -------
# On a settled BRANCH root (book depth > nodeSize=100), root.firstKey() reads the
# transient one-shot DESTRUCTIVE split-key slot (null when settled) instead of the
# subtree's smallest key, so getFirstKey() returns null on every deep book and
# getBestBid()/getBestOffer() collapse to 0. Fix: descend the leftmost-child path
# to the first leaf using only non-destructive accessors (no split/merge logic
# touched). A root collapse keeps Branch roots at size >= 1, so firstValue() is
# always a valid leftmost child.
g = os.path.join(root, "LimitOrderBook/src/main/java/bplusTree/BPlusTree.java")
t = io.open(g, encoding="utf-8").read()
bt_anchor = ("    public K getFirstKey() {\n"
             "        return (K)root.firstKey();\n"
             "    }\n")
bt_inject = ("    public K getFirstKey() {\n"
             "        // HARNESS PATCH (issue #10): return the smallest key by descending the\n"
             "        // leftmost-child path to the first leaf. The original body called\n"
             "        // root.firstKey() directly, but Branch.firstKey() is a one-shot\n"
             "        // DESTRUCTIVE reader of the transient split-key slot, so on a settled\n"
             "        // Branch root (book depth > nodeSize) it returns null even though the\n"
             "        // leaf chain/descent index are intact, collapsing getBestBid/getBestOffer\n"
             "        // to 0. Leftmost-child descent uses only non-destructive accessors and\n"
             "        // changes no split/merge logic.\n"
             "        Node node = root;\n"
             "        while (node instanceof Branch) {\n"
             "            node = (Node) ((Branch) node).firstValue();\n"
             "        }\n"
             "        if (node.size() == 0) {\n"
             "            return null;\n"
             "        }\n"
             "        return (K) node.firstKey();\n"
             "    }\n")
if "HARNESS PATCH (issue #10)" not in t:
    assert bt_anchor in t, "BPlusTree.getFirstKey anchor not found (upstream changed?)"
    t = t.replace(bt_anchor, bt_inject, 1)
    io.open(g, "w", encoding="utf-8").write(t)
    print("    patched BPlusTree.getFirstKey (destructive-read fix)")

# --- PATCH 2: AddOrderPreProcessor contra-side marketable test FIX (issue #10)
# preProcess() compared the incoming LIMIT order's price against its OWN side's
# best (buy vs bestBid, sell vs bestOffer). A buy is marketable iff it can hit an
# ASK (price >= bestOffer) and a sell iff it can hit a BID (price <= bestBid) --
# the CONTRA side. The own-side test mis-routes (notably when a side is empty, its
# best is 0 and the "best != 0" guard is skipped) and the ADD_AND_AGGRESS touch
# branch adds-then-sweeps in a different order than aggress-then-rest. Fix: test
# the CONTRA-side best and route every marketable visible LIMIT order through
# AGGRESS_ORDER (the ADD_AND_AGGRESS/bestVisible touch branch exists for the
# hidden/iceberg interaction; for plain visible orders AGGRESS_ORDER is the
# correct, equivalent action). The empty-tree short-circuits are kept.
a = os.path.join(root, "MatchingEngine/src/main/java/crossing/preProcessor/AddOrderPreProcessor.java")
au = io.open(a, encoding="utf-8").read()
bid_anchor = ("            if(bestBid != 0 && price < bestBid){\n"
              "                return MATCHING_ACTION.ADD_ORDER;\n"
              "            }\n"
              "\n"
              "            if(price == bestVisibleBid){\n"
              "                return  MATCHING_ACTION.ADD_AND_AGGRESS;\n"
              "            }\n")
bid_inject = ("            // HARNESS PATCH (issue #10): a buy crosses only if it can hit an\n"
              "            // ASK, i.e. price >= bestOffer (the CONTRA side). The original tested\n"
              "            // the own side (bestBid) and added an ADD_AND_AGGRESS touch branch;\n"
              "            // both mis-route on the canonical (visible-only) workload. Route\n"
              "            // marketable buys to AGGRESS_ORDER.\n"
              "            if(bestOffer != 0 && price < bestOffer){\n"
              "                return MATCHING_ACTION.ADD_ORDER;\n"
              "            }\n")
off_anchor = ("            if(bestOffer != 0 &&  price > bestOffer){\n"
              "                return MATCHING_ACTION.ADD_ORDER;\n"
              "            }\n"
              "\n"
              "            if(price == bestVisibleOffer){\n"
              "                return  MATCHING_ACTION.ADD_AND_AGGRESS;\n"
              "            }\n")
off_inject = ("            // HARNESS PATCH (issue #10): a sell crosses only if it can hit a\n"
              "            // BID, i.e. price <= bestBid (the CONTRA side).\n"
              "            if(bestBid != 0 && price > bestBid){\n"
              "                return MATCHING_ACTION.ADD_ORDER;\n"
              "            }\n")
if "HARNESS PATCH (issue #10)" not in au:
    assert bid_anchor in au, "AddOrderPreProcessor BID anchor not found (upstream changed?)"
    assert off_anchor in au, "AddOrderPreProcessor OFFER anchor not found (upstream changed?)"
    au = au.replace(bid_anchor, bid_inject, 1)
    au = au.replace(off_anchor, off_inject, 1)
    io.open(a, "w", encoding="utf-8").write(au)
    print("    patched AddOrderPreProcessor (contra-side marketable test)")
PY

# ---- 3. JDK 11 (auto-install only if absent) ------------------------------
find_jdk11() {
    if [ -n "${ME_JDK:-}" ] && [ -x "$ME_JDK/bin/javac" ]; then echo "$ME_JDK"; return; fi
    for d in /usr/lib/jvm/java-11-openjdk-* /usr/lib/jvm/temurin-11-* \
             /usr/lib/jvm/java-1.11.0-openjdk-* /usr/lib/jvm/openjdk-11; do
        [ -x "$d/bin/javac" ] && { echo "$d"; return; }
    done
    echo ""
}
JDK="$(find_jdk11)"
if [ -z "$JDK" ]; then
    echo ">>> no JDK 11 found — installing openjdk-11-jdk-headless via apt"
    if command -v apt-get >/dev/null 2>&1; then
        sudo apt-get update -qq
        sudo apt-get install -y -qq openjdk-11-jdk-headless
        JDK="$(find_jdk11)"
    fi
fi
[ -n "$JDK" ] && [ -x "$JDK/bin/javac" ] || {
    echo "ERROR: no usable JDK 11 (set ME_JDK=/path/to/jdk-11)"; exit 1; }
echo ">>> JDK: $JDK ($("$JDK/bin/javac" -version 2>&1))"
JAVAC="$JDK/bin/javac"
JAR="$JDK/bin/jar"
JVM_LIB="$JDK/lib/server/libjvm.so"
[ -f "$JVM_LIB" ] || { echo "ERROR: libjvm.so not found under $JDK/lib/server"; exit 1; }

# ---- 4. dependency jars (local ~/.m2 fast-path, else Maven Central) --------
DEPDIR="$TP/cointossx_deps"
mkdir -p "$DEPDIR"
M2="${ME_M2:-$HOME/.m2/repository}"
MAVEN_CENTRAL="https://repo1.maven.org/maven2"

# name|maven-path-under-repo
DEP_SPECS=(
  "hppc-0.7.1.jar|com/carrotsearch/hppc/0.7.1"
  "aeron-client-0.9.4.jar|uk/co/real-logic/aeron-client/0.9.4"
  "aeron-driver-0.9.4.jar|uk/co/real-logic/aeron-driver/0.9.4"
  "Agrona-0.4.12.jar|uk/co/real-logic/Agrona/0.4.12"
  "sbe-1.1.7-RC2.jar|uk/co/real-logic/sbe/1.1.7-RC2"
  "joda-time-2.3.jar|joda-time/joda-time/2.3"
  "HdrHistogram-2.1.6.jar|org/hdrhistogram/HdrHistogram/2.1.6"
  "commons-csv-1.1.jar|org/apache/commons/commons-csv/1.1"
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
# CoinTossX vendors ObjectLayout-1.0.5-SNAPSHOT (not on Maven Central) as a
# tracked file under lib/; the git reset restored it.
OBJLAYOUT="$SRC/lib/ObjectLayout-1.0.5-SNAPSHOT.jar"
[ -f "$OBJLAYOUT" ] || { echo "ERROR: vendored $OBJLAYOUT missing from the checkout"; exit 1; }
DEP_JARS+=("$OBJLAYOUT")
DEP_CP="$(IFS=:; echo "${DEP_JARS[*]}")"

# ---- 5. compile the matcher core + helper ---------------------------------
# Build output (compiled classes + jar) lives under the gitignored third_party/
# tree, so the committed adapter directory holds only authored files
# (cointossx_adapter.cpp, HarnessCoinTossX.java, build.sh, README.md).
BUILD="$TP/cointossx_build"
CLASSES="$BUILD/classes"
rm -rf "$CLASSES"; mkdir -p "$CLASSES"
SRCLIST="$BUILD/srcs.txt"
find "$SRC/LimitOrderBook/src/main/java" \
     "$SRC/MatchingEngine/src/main/java" \
     "$SRC/Messages/src/main/java" \
     "$SRC/Socket/src/main/java" -name "*.java" > "$SRCLIST"
echo ">>> compiling CoinTossX matcher core ($(wc -l < "$SRCLIST") sources)"
"$JAVAC" -encoding UTF-8 -source 8 -target 8 -nowarn -cp "$DEP_CP" \
         -d "$CLASSES" @"$SRCLIST"
echo ">>> compiling HarnessCoinTossX"
"$JAVAC" -encoding UTF-8 -source 8 -target 8 -nowarn -cp "$DEP_CP:$CLASSES" \
         -d "$CLASSES" "$DIR/HarnessCoinTossX.java"

# ---- 6. jar + classpath + adapter .so -------------------------------------
MATCHER_JAR="$BUILD/cointossx-matcher.jar"
"$JAR" cf "$MATCHER_JAR" -C "$CLASSES" .

echo ">>> writing $REPO/cointossx.classpath"
# Classpath the adapter reads at run time: matcher+helper jar, then deps.
printf '%s' "$MATCHER_JAR:$DEP_CP" > "$REPO/cointossx.classpath"

echo ">>> compiling cointossx_adapter.so"
g++ -std=c++17 -O3 -march=native -fPIC -shared \
    -I"$REPO/api" -I"$JDK/include" -I"$JDK/include/linux" \
    -DME_JVM_LIB="\"$JVM_LIB\"" \
    "$DIR/cointossx_adapter.cpp" \
    -o "$REPO/cointossx_adapter.so" -ldl
echo "built: $REPO/cointossx_adapter.so (+ $REPO/cointossx.classpath, $CLASSES/HarnessCoinTossX.class)"
ls -la "$REPO/cointossx_adapter.so"
