#!/usr/bin/env bash
# Build dgtony_adapter.so — dgtony/orderbook-rs behind the harness
# matching_engine_api.h ABI. The engine is pure Rust; the whole adapter is one
# Rust cdylib that exports the harness engine_* extern-C symbols and calls
# straight into the engine's `orderbook` crate. No C++ shim.
#
# Clones dgtony/orderbook-rs at the pinned commit, applies a minimal, additive
# engine-source correctness patch (new pub methods that thread a caller-supplied
# order id into the engine's own matching helpers — bypassing the rotating
# [1,1000] id generator that otherwise drops the 1001st resting order — plus
# best_bid / best_ask / depth_at query accessors; see the Source patch note in
# README.md and https://github.com/dgtony/orderbook-rs/issues/9), then compiles
# the engine + this adapter into a single .so at the harness repo root.
#
# Override the upstream checkout: ME_DGTONY_SRC=/path/to/existing/clone
# (the engine repo root, i.e. the dir that contains src/engine/orderbook.rs).
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

DGTONY_URL="https://github.com/dgtony/orderbook-rs.git"
# Pinned to the SNAPSHOTS.md commit. Reproducible across rebuilds; rerunning
# build.sh on an existing third_party clone hard-resets to this exact SHA so the
# correctness patch below always lands on pristine source.
DGTONY_REF="cba8329b1f6cb2156c734b4cfab8ab0cc5566cc6"

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
if [ -n "${ME_DGTONY_SRC:-}" ]; then
    SRC="$ME_DGTONY_SRC"
else
    SRC="$TP/dgtony-orderbook-rs"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$DGTONY_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$DGTONY_REF"
fi

# ----- Engine-source correctness patch (minimal, additive) ------------------
# Correctness fix (not performance). Three closely-related API gaps the harness
# exercises, all closed by ADDING new pub methods that reuse the engine's own
# existing (private) matching helpers — NO existing engine line is changed, so
# the matching/resting/cancel path stays byte-for-byte the upstream behaviour:
#
#   1. Caller-supplied order ids. process_order assigns ids from an internal
#      TradeSequence that ROTATES in [1,1000] (src/engine/sequence.rs) and
#      ignores the caller; once 1000 orders are live the generated ids wrap and
#      collide, and OrderQueue::insert silently refuses the duplicate so the
#      1001st order rests nowhere (Accepted, then DuplicateOrderID). The harness
#      supplies 32-bit ids up to the order count (300k). New pub `submit_limit`
#      / `submit_cancel` thread the harness id straight into the engine's own
#      process_limit_order / process_order_cancel, so the rotating generator and
#      its [1,1000] validation bound are never on the path. (This is also why
#      the benchmark adapter never reaches the engine's separate AmendOrder
#      crossing bug — it does modify = cancel + reinsert via these wrappers, per
#      the harness contract.) See https://github.com/dgtony/orderbook-rs/issues/9.
#   2. best_bid / best_ask. current_spread() returns None unless BOTH sides are
#      populated; the harness queries each side independently. Added as a
#      peek()-of-one-side read.
#   3. depth_at(price, side). No native aggregated-depth query exists. Added as
#      a sum over the side queue's live `orders` map (which needs a pub iterator
#      on OrderQueue, since its `orders` field is private to order_queues.rs).
#
# Applied with anchored Python replaces + a marker guard so an upstream change
# fails the build loudly rather than silently shipping the bounded engine, and a
# re-apply (e.g. under an ME_DGTONY_SRC override) is a no-op. Idempotent: the
# git reset --hard above restores pristine source each run on the default
# checkout.
python3 - "$SRC" <<'PYEOF'
import sys, io
root = sys.argv[1]

# (A) order_queues.rs — add a pub iterator over the live `orders` map so the
#     orderbook can sum depth at a price (the field is private to this module).
qpath = root + "/src/engine/order_queues.rs"
q = io.open(qpath, encoding="utf-8").read()
marker_q = "dgtony_adapter: iterate live resting orders for depth aggregation"
if marker_q in q:
    print("order_queues.rs already patched (iter_orders)")
else:
    anchor_q = "    /// Return ID of current order in queue\n"
    if anchor_q not in q:
        sys.exit("dgtony build.sh iter_orders patch: anchor not found in order_queues.rs (upstream changed?)")
    inject_q = (
        "    /// dgtony_adapter: iterate live resting orders for depth aggregation.\n"
        "    /// Reads only the orders map, which holds exactly the live orders\n"
        "    /// (cancel/pop remove from it); used by the adapter's depth_at.\n"
        "    /// See https://github.com/dgtony/orderbook-rs/issues/9.\n"
        "    pub fn iter_orders(&self) -> std::collections::hash_map::Values<u64, T> {\n"
        "        self.orders.values()\n"
        "    }\n\n\n"
    )
    q = q.replace(anchor_q, inject_q + anchor_q, 1)
    io.open(qpath, "w", encoding="utf-8").write(q)
    print("patched order_queues.rs: + pub fn iter_orders")

# (B) orderbook.rs — add caller-id submit wrappers + best_bid/best_ask/depth_at.
opath = root + "/src/engine/orderbook.rs"
o = io.open(opath, encoding="utf-8").read()
marker_o = "dgtony_adapter: submit a limit order under a CALLER-SUPPLIED id"
if marker_o in o:
    print("orderbook.rs already patched (submit_limit/best_bid/best_ask/depth_at)")
else:
    anchor_o = "    /// Get current spread as a tuple: (bid, ask)\n"
    if anchor_o not in o:
        sys.exit("dgtony build.sh submit/query patch: anchor not found in orderbook.rs (upstream changed?)")
    inject_o = r'''    /// dgtony_adapter: submit a limit order under a CALLER-SUPPLIED id,
    /// bypassing the rotating [1,1000] TradeSequence id generator (and its
    /// validator) that process_order would otherwise impose. Reuses the
    /// engine's own process_limit_order, so matching/resting is identical.
    /// See https://github.com/dgtony/orderbook-rs/issues/9.
    pub fn submit_limit(
        &mut self,
        order_id: u64,
        side: OrderSide,
        price: f64,
        qty: f64,
        ts: SystemTime,
    ) -> OrderProcessingResult {
        let mut proc_result: OrderProcessingResult = vec![];
        proc_result.push(Ok(Success::Accepted {
            id: order_id,
            order_type: OrderType::Limit,
            ts: SystemTime::now(),
        }));
        self.process_limit_order(
            &mut proc_result,
            order_id,
            self.order_asset,
            self.price_asset,
            side,
            price,
            qty,
            ts,
        );
        proc_result
    }

    /// dgtony_adapter: cancel a resting order by CALLER-SUPPLIED id + side,
    /// reusing the engine's own process_order_cancel.
    pub fn submit_cancel(&mut self, order_id: u64, side: OrderSide) -> OrderProcessingResult {
        let mut proc_result: OrderProcessingResult = vec![];
        self.process_order_cancel(&mut proc_result, order_id, side);
        proc_result
    }

    /// dgtony_adapter: best (highest) bid price, independent of the ask side
    /// (current_spread requires both sides populated).
    pub fn best_bid(&mut self) -> Option<f64> {
        self.bid_queue.peek().map(|o| o.price)
    }

    /// dgtony_adapter: best (lowest) ask price, independent of the bid side.
    pub fn best_ask(&mut self) -> Option<f64> {
        self.ask_queue.peek().map(|o| o.price)
    }

    /// dgtony_adapter: aggregated resting quantity at one price on one side.
    pub fn depth_at(&self, price: f64, side: OrderSide) -> f64 {
        let queue = match side {
            OrderSide::Bid => &self.bid_queue,
            OrderSide::Ask => &self.ask_queue,
        };
        queue
            .iter_orders()
            .filter(|ord| ord.price == price)
            .map(|ord| ord.qty)
            .sum()
    }


'''
    o = o.replace(anchor_o, inject_o + anchor_o, 1)
    io.open(opath, "w", encoding="utf-8").write(o)
    print("patched orderbook.rs: + submit_limit/submit_cancel/best_bid/best_ask/depth_at")
PYEOF

# Hard-verify the patch landed so a future tooling change can never quietly ship
# the un-bypassed engine (whose rotating ids would drop every order past 1000).
for needle in \
    "pub fn submit_limit(" \
    "pub fn submit_cancel(" \
    "pub fn best_bid(" \
    "pub fn best_ask(" \
    "pub fn depth_at(" ; do
    if ! grep -q "$needle" "$SRC/src/engine/orderbook.rs"; then
        echo "engine patch applied but '$needle' is absent — refusing to ship" >&2
        exit 1
    fi
done
if ! grep -q "pub fn iter_orders(" "$SRC/src/engine/order_queues.rs"; then
    echo "engine patch applied but 'pub fn iter_orders' is absent — refusing to ship" >&2
    exit 1
fi

# ----- Point the wrapper crate at $SRC (if overridden) ----------------------
# wrapper/Cargo.toml commits the default third_party path. If ME_DGTONY_SRC
# overrides the checkout, swap the orderbook path for this build and restore it
# on exit so a follow-up default build keeps working. Pure source-level edit,
# idempotent on rerun.
WRAPPER="$DIR/wrapper"
ORIG_PATH="../../../third_party/dgtony-orderbook-rs"
NEW_PATH="$SRC"
if [ "$(cd "$NEW_PATH" && pwd)" != "$REPO/third_party/dgtony-orderbook-rs" ]; then
    sed -i "s|path = \"$ORIG_PATH\"|path = \"$NEW_PATH\"|g" "$WRAPPER/Cargo.toml"
    trap 'sed -i "s|path = \"$NEW_PATH\"|path = \"$ORIG_PATH\"|g" "$WRAPPER/Cargo.toml"' EXIT
fi

# ----- Build ----------------------------------------------------------------
# Keep the wrapper's target dir inside the adapter folder so repeated builds
# share the cargo cache and stay reproducible. RUSTFLAGS=-C target-cpu=native is
# the Rust equivalent of g++ -march=native (the default for all C++ adapters in
# this tree). The engine declares no [profile.release], so neither does the
# wrapper — plain --release, nothing more.
export CARGO_TARGET_DIR="$WRAPPER/target"
export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
cargo build --release --manifest-path "$WRAPPER/Cargo.toml"

SO_SRC="$CARGO_TARGET_DIR/release/libdgtony_adapter.so"
if [ ! -f "$SO_SRC" ]; then
    echo "build produced no .so at $SO_SRC" >&2
    exit 1
fi
cp -f "$SO_SRC" "$REPO/dgtony_adapter.so"
echo "built: dgtony_adapter.so"
