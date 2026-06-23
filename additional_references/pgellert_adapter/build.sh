#!/usr/bin/env bash
# Build pgellert_adapter.so — pgellert/matching-engine behind the harness
# matching_engine_api.h ABI. Pure-Rust cdylib that drives the engine's
# production matcher (engine::algos::optimised_fifo::FIFOBook — the book
# engine/src/rpc/me_state_machine.rs constructs).
#
# Engine: pgellert/matching-engine, pinned at de195a8227b942f10fd5cb41934d1ce325dd8dd9
# Repo:   https://github.com/pgellert/matching-engine
# Language: Rust.
#
# WHY A VENDORED WRAPPER (not a path dependency on the engine crate) ----------
# The engine is a multi-crate Raft workspace. Its production matcher (FIFOBook)
# needs nothing but std collections, but the crate that defines it
# (engine/) pulls in tonic / prost / tokio 0.2 / a git-pinned ART crate and a
# protobuf codegen build.rs — none of which the matcher uses. So the wrapper
# crate (wrapper/) VENDORS exactly the two engine source files the matcher lives
# in, under wrapper/src/algos/, and builds them with no external dependency:
#   * wrapper/src/algos/optimised_fifo.rs — engine/src/algos/optimised_fifo.rs
#     verbatim, MINUS the trailing #[cfg(test)] module (which uses the engine's
#     `rand` dev-dependency), PLUS three appended READ-ONLY query accessors
#     (best_bid / best_ask / depth_at) for the harness audit. The matcher state
#     machine (apply / check_for_trades / cancel) is byte-identical to the
#     patched engine source.
#   * wrapper/src/algos/book.rs — engine/src/algos/book.rs with ONLY
#     `use crate::protobuf;` and the three protobuf/raft bridge methods
#     (from_proto / into_proto / from_command) removed. Order::merge, the
#     Buy/Sell Ord impls, Side, Trade and the Book trait are byte-identical to
#     upstream — the matching logic under test is the engine's own.
#
# The vendored copies are hand-curated and committed (so the strip stays exact).
# This build.sh does NOT regenerate them; instead it clones the pinned engine,
# applies the one ENGINE correctness patch below, and then VERIFIES the committed
# vendored matcher is identical to the patched engine source — so the vendoring
# can never silently diverge from the pin, and the patched matcher is exactly
# what ships.
#
# Override the upstream checkout: ME_PGELLERT_SRC=/path/to/existing/clone
# (the engine repo root — the dir that contains engine/src/algos/).
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

PGE_URL="https://github.com/pgellert/matching-engine.git"
# Pinned to the SNAPSHOTS.md commit. Reproducible across rebuilds; rerunning
# build.sh on an existing third_party clone hard-resets to this exact SHA so the
# correctness patch below always lands on pristine source.
PGE_REF="de195a8227b942f10fd5cb41934d1ce325dd8dd9"

# ----- Rust toolchain (idempotent) ------------------------------------------
if ! command -v cargo >/dev/null 2>&1; then
    if [ -f "$HOME/.cargo/env" ]; then
        # shellcheck disable=SC1091
        . "$HOME/.cargo/env"
    fi
fi
if ! command -v cargo >/dev/null 2>&1; then
    echo "rustup not found; installing stable toolchain into \$HOME/.cargo"
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
        sh -s -- -y --default-toolchain stable --profile minimal
    # shellcheck disable=SC1091
    . "$HOME/.cargo/env"
fi

# ----- Upstream checkout ----------------------------------------------------
if [ -n "${ME_PGELLERT_SRC:-}" ]; then
    SRC="$ME_PGELLERT_SRC"
else
    SRC="$TP/pgellert-matching-engine"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$PGE_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$PGE_REF"
fi

ENGINE_FIFO="$SRC/engine/src/algos/optimised_fifo.rs"
ENGINE_BOOK="$SRC/engine/src/algos/book.rs"
if [ ! -f "$ENGINE_FIFO" ] || [ ! -f "$ENGINE_BOOK" ]; then
    echo "engine source not found under $SRC (expected engine/src/algos/{optimised_fifo,book}.rs)" >&2
    exit 1
fi

# ----- Engine-source correctness fix: optimised_fifo matcher ----------------
# Correctness fix (not performance). The production FIFOBook
# (engine/src/algos/optimised_fifo.rs) has two hard-invariant matcher bugs that
# make it under-match (~70% of trades missing on the benchmark tapes), so its
# report stream diverges from the 3-baseline consensus. Filed upstream:
# https://github.com/pgellert/matching-engine/issues/2.
#
#   1. Dropped popped order. check_for_trades pops the best bid + best ask off
#      their buckets BEFORE deciding whether they trade; on the not-crossing and
#      one-side-empty exit paths the popped order(s) are not re-inserted, so
#      resting liquidity is silently destroyed (quantity non-conservation). The
#      original code only push_front's a partial-fill remainder, and never the
#      whole popped order. The fix adds `unpop()` (re-insert at the FRONT of the
#      bucket, restoring FIFO position) and routes every non-trading / empty
#      exit through it.
#   2. Stale price bounds. pop_bid/pop_ask ratchet the cached min/max bound to
#      every price they SCAN OVER (including empty buckets), and apply() only
#      refreshes the bound when it creates a NEW bucket — so an order landing in
#      a previously-emptied bucket cannot widen the best back out. The bounds
#      therefore drift stale-permissive: the cross guard lets a non-crossing
#      pair through, and a genuinely marketable order can be hidden. The fix
#      commits the pop bound only to a price that actually holds a live order,
#      refreshes the bound on EVERY insert, and re-checks the real cross
#      (ask.price <= bid.price) inside the match loop, unpop'ing both when the
#      popped pair does not actually cross.
#
# This is an ENGINE-source fix, not an adapter workaround: the adapter drives the
# vendored matcher exactly as the engine's MEStateMachine does (apply ->
# check_for_trades per message), so the conservation/priority invariants have to
# hold in the matcher itself. The patch touches ONLY optimised_fifo.rs; book.rs
# and every other engine file are untouched. The wrapper/examples/verify_fix.rs
# example asserts both bugs are gone on the patched matcher
# (cargo run --release --example verify_fix from wrapper/).
#
# Applied with `git apply` against the freshly hard-reset pin: git's own context
# matching is the anchor guard (a drifted upstream fails loud rather than
# silently shipping the bug). Idempotent — the reset above restores pristine
# source each default run, and an already-patched override checkout is detected
# and skipped via the reverse --check probe below.
PATCH_FILE="$(mktemp)"
trap 'rm -f "$PATCH_FILE"' EXIT
cat > "$PATCH_FILE" <<'PATCH_EOF'
--- a/engine/src/algos/optimised_fifo.rs
+++ b/engine/src/algos/optimised_fifo.rs
@@ -34,9 +34,12 @@ impl FIFOBook {

     fn pop_bid(&mut self) -> Option<Order> {
         for bid_price in (self.min_bid_price..=self.max_bid_price).rev() {
-            self.max_bid_price = bid_price;
             match self.bid_price_buckets.get_mut(&bid_price) {
                 Some(orders) if !orders.is_empty() => {
+                    // Commit the bound only to a price that actually holds a live
+                    // order; a price merely scanned over (empty bucket) must not
+                    // ratchet the bound away from the real best level.
+                    self.max_bid_price = bid_price;
                     let order = orders.pop_front().unwrap();
                     return Some(order);
                 }
@@ -48,9 +51,12 @@ impl FIFOBook {

     fn pop_ask(&mut self) -> Option<Order> {
         for ask_price in self.min_ask_price..=self.max_ask_price {
-            self.min_ask_price = ask_price;
             match self.ask_price_buckets.get_mut(&ask_price) {
                 Some(orders) if !orders.is_empty() => {
+                    // Commit the bound only to a price that actually holds a live
+                    // order; a price merely scanned over (empty bucket) must not
+                    // ratchet the bound away from the real best level.
+                    self.min_ask_price = ask_price;
                     let order = orders.pop_front().unwrap();
                     return Some(order);
                 }
@@ -74,6 +80,20 @@ impl FIFOBook {
         }
         result
     }
+
+    /// Re-inserts a popped-but-unconsumed order at the FRONT of its price bucket
+    /// (restoring its FIFO position) so quantity is conserved when a pop does not
+    /// lead to a trade.
+    fn unpop(&mut self, order: Order) {
+        let buckets = match order.side {
+            Side::Buy => &mut self.bid_price_buckets,
+            Side::Sell => &mut self.ask_price_buckets,
+        };
+        buckets
+            .entry(order.price)
+            .or_insert_with(VecDeque::new)
+            .push_front(order);
+    }
 }

 impl Book for FIFOBook {
@@ -84,11 +104,15 @@ impl Book for FIFOBook {

         match order.side {
             Side::Buy => {
+                // Refresh the bound on EVERY insert, not only when a new bucket
+                // is created: an order landing in a bucket that already exists
+                // (including one emptied by an earlier scan) must still be able
+                // to widen the live-best bound back out.
+                self.min_bid_price = min(self.min_bid_price, order.price);
+                self.max_bid_price = max(self.max_bid_price, order.price);
                 let bucket_opt = self.bid_price_buckets.get_mut(&order.price);
                 match bucket_opt {
                     None => {
-                        self.min_bid_price = min(self.min_bid_price, order.price);
-                        self.max_bid_price = max(self.max_bid_price, order.price);
                         self.bid_price_buckets
                             .insert(order.price, VecDeque::from_iter(vec![order]));
                     }
@@ -96,11 +120,13 @@ impl Book for FIFOBook {
                 }
             }
             Side::Sell => {
+                // Refresh the bound on EVERY insert, not only when a new bucket
+                // is created (see the Buy arm above).
+                self.min_ask_price = min(self.min_ask_price, order.price);
+                self.max_ask_price = max(self.max_ask_price, order.price);
                 let bucket_opt = self.ask_price_buckets.get_mut(&order.price);
                 match bucket_opt {
                     None => {
-                        self.min_ask_price = min(self.min_ask_price, order.price);
-                        self.max_ask_price = max(self.max_ask_price, order.price);
                         self.ask_price_buckets
                             .insert(order.price, VecDeque::from_iter(vec![order]));
                     }
@@ -118,52 +144,87 @@ impl Book for FIFOBook {

         let mut trades = vec![];

+        // pop_bid / pop_ask REMOVE the front order from its bucket before
+        // returning it. Every exit path below therefore re-inserts (via unpop)
+        // any order it popped but did not consume in a trade — otherwise that
+        // resting liquidity is silently dropped (quantity non-conservation).
         let (mut bid, mut ask) = match (self.pop_bid(), self.pop_ask()) {
             (Some(bid_new), Some(ask_new)) => (bid_new, ask_new),
-            _ => return trades,
+            (Some(bid_new), None) => {
+                self.unpop(bid_new);
+                return trades;
+            }
+            (None, Some(ask_new)) => {
+                self.unpop(ask_new);
+                return trades;
+            }
+            (None, None) => return trades,
         };

-        while let Some((trade, remainder)) = self.merge(ask, bid) {
+        loop {
+            // The cached price bounds can be stale-permissive (a pop never moves
+            // a bound to a price it merely skipped over, and a cancel/empty does
+            // not eagerly recompute it), so the guard above can let us in — and a
+            // pop can hand back the best-on-each-side pair — even when that pair
+            // does NOT actually cross. Re-insert both and stop; nothing here
+            // trades. This is what makes the bounds safe to leave permissive.
+            if ask.price > bid.price {
+                self.unpop(bid);
+                self.unpop(ask);
+                return trades;
+            }
+
+            // ask.price <= bid.price: a real cross. merge() consumes both and,
+            // for a partial fill, returns the unfilled remainder on one side.
+            let (trade, remainder) = self
+                .merge(ask, bid)
+                .expect("crossing ask<=bid must produce a trade");
             trades.push(trade);

-            if let Some(rem) = remainder {
-                match rem.side {
-                    Side::Buy => {
-                        if let Some(ask_new) = self.pop_ask() {
+            match remainder {
+                Some(rem) => match rem.side {
+                    // The leftover stays in hand; pull a fresh order from the
+                    // opposite side. If that side is empty, the leftover rests.
+                    Side::Buy => match self.pop_ask() {
+                        Some(ask_new) => {
                             ask = ask_new;
                             bid = rem;
-                        } else {
-                            self.bid_price_buckets
-                                .get_mut(&rem.price)
-                                .unwrap()
-                                .push_front(rem);
+                        }
+                        None => {
+                            self.unpop(rem);
                             return trades;
                         }
-                    }
-                    Side::Sell => {
-                        if let Some(bid_new) = self.pop_bid() {
+                    },
+                    Side::Sell => match self.pop_bid() {
+                        Some(bid_new) => {
                             bid = bid_new;
                             ask = rem;
-                        } else {
-                            self.ask_price_buckets
-                                .get_mut(&rem.price)
-                                .unwrap()
-                                .push_front(rem);
+                        }
+                        None => {
+                            self.unpop(rem);
                             return trades;
                         }
-                    }
-                }
-            } else {
-                match (self.pop_bid(), self.pop_ask()) {
+                    },
+                },
+                // Exact fill: both orders are gone. Re-pop a fresh pair,
+                // re-inserting a one-sided survivor instead of dropping it.
+                None => match (self.pop_bid(), self.pop_ask()) {
                     (Some(bid_new), Some(ask_new)) => {
                         bid = bid_new;
                         ask = ask_new;
                     }
-                    _ => return trades,
-                };
+                    (Some(bid_new), None) => {
+                        self.unpop(bid_new);
+                        return trades;
+                    }
+                    (None, Some(ask_new)) => {
+                        self.unpop(ask_new);
+                        return trades;
+                    }
+                    (None, None) => return trades,
+                },
             }
         }
-        trades
     }

     /// Cancels the given order from the book
PATCH_EOF

if git -C "$SRC" apply --reverse --check "$PATCH_FILE" >/dev/null 2>&1; then
    echo "optimised_fifo.rs already carries the issue#2 matcher fix (skipping apply)"
elif git -C "$SRC" apply --check "$PATCH_FILE" >/dev/null 2>&1; then
    git -C "$SRC" apply "$PATCH_FILE"
    echo "patched optimised_fifo.rs: conserve popped orders + correct stale price bounds (issue #2)"
else
    echo "issue#2 matcher patch does not apply cleanly to $ENGINE_FIFO (upstream changed?)" >&2
    exit 1
fi

# Hard-verify the fix is present so a future tooling change can never quietly
# ship the unbounded-under-match engine.
if ! grep -q "fn unpop(&mut self, order: Order)" "$ENGINE_FIFO"; then
    echo "patch reported applied but unpop() is absent from $ENGINE_FIFO — refusing to ship" >&2
    exit 1
fi

# ----- Verify the vendored matcher logic matches the PATCHED pin -------------
# A guard, not a generator: confirm the matcher code in the committed vendored
# files is identical to the (now patched) pinned engine source. Catches any
# drift between the pin+fix and the committed vendored copy. The accessor block /
# test strip / protobuf strip are excluded from the comparison because they are
# the documented, intended differences.
python3 - "$SRC" "$DIR" <<'PY'
import sys
eng = sys.argv[1]   # engine clone (patched)
ad  = sys.argv[2]   # adapter dir (vendored)

def norm(s):
    # Compare on non-blank, non-comment content so doc/blank-line cosmetics
    # (and the vendored-file headers) don't trip the guard.
    out = []
    for line in s.splitlines():
        t = line.strip()
        if not t or t.startswith("//"):
            continue
        out.append(t)
    return out

# optimised_fifo: the matcher portion is everything up to the appended accessor
# block in the vendored file, vs everything up to #[cfg(test)] upstream.
up = open(f"{eng}/engine/src/algos/optimised_fifo.rs").read()
ve = open(f"{ad}/wrapper/src/algos/optimised_fifo.rs").read()
up_match = up.split("#[cfg(test)]")[0]
ve_match = ve.split("// Read-only query accessors")[0]
if norm(up_match) != norm(ve_match):
    a, b = norm(up_match), norm(ve_match)
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            sys.exit(f"DRIFT optimised_fifo.rs line~{i}: patched-engine {x!r} != vendored {y!r}")
    sys.exit(f"DRIFT optimised_fifo.rs length {len(a)} vs {len(b)}")

# book.rs: strip the three bridge methods + the protobuf use from upstream, then
# compare the remainder to the vendored file.
up = open(f"{eng}/engine/src/algos/book.rs").read()
ve = open(f"{ad}/wrapper/src/algos/book.rs").read()

def strip_braced(text, marker):
    i = text.find(marker)
    if i < 0:
        return text
    # Walk start back over the immediately-preceding attribute / doc lines
    # (#[inline], /// ...) that belong to this method, plus its leading blank.
    line_start = text.rfind("\n", 0, i) + 1
    prefix_lines = text[:line_start].rstrip("\n").splitlines()
    j = len(prefix_lines) - 1
    while j >= 0 and (prefix_lines[j].lstrip().startswith("#[")
                      or prefix_lines[j].lstrip().startswith("///")):
        j -= 1
    start = sum(len(l) + 1 for l in prefix_lines[:j + 1])
    b = text.find("{", i)
    depth = 0
    k = b
    while k < len(text):
        if text[k] == "{":
            depth += 1
        elif text[k] == "}":
            depth -= 1
            if depth == 0:
                break
        k += 1
    return text[:start] + text[k + 1:]

up2 = up.replace("use crate::protobuf;", "")
for m in ("fn from_proto(", "pub(crate) fn into_proto(", "pub(crate) fn from_command("):
    up2 = strip_braced(up2, m)
if norm(up2) != norm(ve):
    a, b = norm(up2), norm(ve)
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            sys.exit(f"DRIFT book.rs line~{i}: upstream {x!r} != vendored {y!r}")
    sys.exit(f"DRIFT book.rs length {len(a)} vs {len(b)}")

print("vendored matcher logic verified identical to patched pin", flush=True)
PY

# ----- Build ----------------------------------------------------------------
# Keep the wrapper's target dir inside the adapter folder so repeated builds
# share the cargo cache and stay reproducible. RUSTFLAGS=-C target-cpu=native is
# the Rust equivalent of g++ -march=native (the default for all C++ adapters in
# this tree). The wrapper is a self-contained crate (vendored matcher, std-only),
# detached from the engine workspace, so this builds ONLY the adapter.
WRAPPER="$DIR/wrapper"
export CARGO_TARGET_DIR="$WRAPPER/target"
export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
cargo build --release --manifest-path "$WRAPPER/Cargo.toml"

SO_SRC="$CARGO_TARGET_DIR/release/libpgellert_adapter.so"
if [ ! -f "$SO_SRC" ]; then
    echo "build produced no .so at $SO_SRC" >&2
    exit 1
fi
cp -f "$SO_SRC" "$REPO/pgellert_adapter.so"
echo "built: pgellert_adapter.so"
