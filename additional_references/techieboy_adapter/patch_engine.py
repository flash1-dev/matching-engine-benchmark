#!/usr/bin/env python3
"""Idempotent source patch for TechieBoy/rust-orderbook (engine crate
`orderbook`, lib `orderbooklib`) so it satisfies the harness contract.

WHY each patch (all are minimal; none reimplement matching — they expose the
engine's real behaviour to the adapter):

  P1  Caller-supplied order id. Upstream `create_new_limit_order` mints the id
      with `rand::thread_rng().gen()` and returns it, so a caller cannot key a
      later cancel/modify by its own id. The harness owns the ids and needs
      cancel/modify keyed natively by them. Patch: pass `order_id` in, drop the
      RNG. `add_limit_order` gains an `order_id` parameter and forwards it.

  P2  Per-maker fill capture. Upstream's `FillResult.filled_orders` records one
      (total_qty, price) pair PER PRICE LEVEL — the individual maker order ids
      consumed during matching are discarded inside `match_at_price_level`
      (it only does `order_loc.remove(&o.order_id)`). The harness needs one
      Trade per maker with that maker's order id. Patch: also record
      `(maker_order_id, fill_qty, level_price)` per maker into a new
      `FillResult.maker_fills` vector. The matching logic itself is untouched;
      we only stop throwing the ids away.

  P3  Public read accessors. Upstream exposes best-bid/ask and depth only via
      `get_bbo()` (which prints) and the private fields. The harness queries
      need values. Patch: add `pub fn eng_best_bid_raw/eng_best_ask_raw`
      (return the engine's own cached BBO fields verbatim — so a staleness bug
      in `update_bbo` is observed, not masked) and `pub fn eng_depth_at`
      (sums live qty at one price level, mirroring `get_total_qty` but
      tolerating a missing/empty level by returning 0).

Idempotent: build.sh runs `git reset --hard <pin>` before invoking this, so the
file is always the pristine upstream when we start. Each replacement asserts it
matched exactly once.
"""
import sys

path = sys.argv[1]
with open(path, "r") as f:
    src = f.read()


def replace_once(s, old, new, tag):
    n = s.count(old)
    if n != 1:
        sys.stderr.write(
            f"PATCH FAIL [{tag}]: expected exactly 1 match, found {n}\n"
        )
        sys.exit(1)
    return s.replace(old, new)


# --- P2a: FillResult struct gains a per-maker fills vector ------------------
src = replace_once(
    src,
    """pub struct FillResult {
    // Orders filled (qty, price)
    pub filled_orders: Vec<(u64, u64)>,
    pub remaining_qty: u64,
    pub status: OrderStatus,
}""",
    """pub struct FillResult {
    // Orders filled (qty, price)
    pub filled_orders: Vec<(u64, u64)>,
    // Harness patch: one entry per maker order consumed: (maker_order_id, fill_qty, maker_price)
    pub maker_fills: Vec<(u64, u64, u64)>,
    pub remaining_qty: u64,
    pub status: OrderStatus,
}""",
    "P2a FillResult struct",
)

# --- P2b: FillResult::new initialises the new field ------------------------
src = replace_once(
    src,
    """        FillResult {
            filled_orders: Vec::new(),
            remaining_qty: u64::MAX,
            status: OrderStatus::Uninitialized,
        }""",
    """        FillResult {
            filled_orders: Vec::new(),
            maker_fills: Vec::new(),
            remaining_qty: u64::MAX,
            status: OrderStatus::Uninitialized,
        }""",
    "P2b FillResult::new",
)

# --- P1a: create_new_limit_order takes an explicit order_id ----------------
src = replace_once(
    src,
    """    fn create_new_limit_order(&mut self, s: Side, price: u64, qty: u64) -> u64 {
        let mut rng = rand::thread_rng();
        let order_id: u64 = rng.gen();
        let book = match s {""",
    """    fn create_new_limit_order(&mut self, s: Side, price: u64, qty: u64, order_id: u64) -> u64 {
        let book = match s {""",
    "P1a create_new_limit_order signature",
)

# --- P2c: match_at_price_level records per-maker fills ----------------------
# Add a maker_price param and a &mut Vec to push (maker_id, fill_qty, price).
src = replace_once(
    src,
    """        fn match_at_price_level(
            price_level: &mut VecDeque<Order>,
            incoming_order_qty: &mut u64,
            order_loc: &mut HashMap<u64, (Side, usize)>,
        ) -> u64 {
            let mut done_qty = 0;
            for o in price_level.iter_mut() {
                if o.qty <= *incoming_order_qty {
                    *incoming_order_qty -= o.qty;
                    done_qty += o.qty;
                    o.qty = 0;
                    order_loc.remove(&o.order_id);
                } else {
                    o.qty -= *incoming_order_qty;
                    done_qty += *incoming_order_qty;
                    *incoming_order_qty = 0;
                }
            }
            price_level.retain(|x| x.qty != 0);
            done_qty
        }""",
    """        fn match_at_price_level(
            price_level: &mut VecDeque<Order>,
            incoming_order_qty: &mut u64,
            order_loc: &mut HashMap<u64, (Side, usize)>,
            maker_price: u64,
            maker_fills: &mut Vec<(u64, u64, u64)>,
        ) -> u64 {
            let mut done_qty = 0;
            for o in price_level.iter_mut() {
                // Harness patch (ENGINE BUG fix): stop once the incoming order is
                // exhausted. Upstream loops over EVERY maker in the level even
                // after *incoming_order_qty hits 0; the trailing makers fall into
                // the `else` arm and execute `o.qty -= 0` / `done_qty += 0` —
                // harmless to the per-level qty sum upstream computed, but a
                // spurious zero-quantity fill once the per-maker fills are
                // recorded (and wasted work either way). A correct matcher
                // breaks here.
                if *incoming_order_qty == 0 {
                    break;
                }
                if o.qty <= *incoming_order_qty {
                    *incoming_order_qty -= o.qty;
                    done_qty += o.qty;
                    maker_fills.push((o.order_id, o.qty, maker_price));
                    o.qty = 0;
                    order_loc.remove(&o.order_id);
                } else {
                    o.qty -= *incoming_order_qty;
                    done_qty += *incoming_order_qty;
                    maker_fills.push((o.order_id, *incoming_order_qty, maker_price));
                    *incoming_order_qty = 0;
                }
            }
            price_level.retain(|x| x.qty != 0);
            done_qty
        }""",
    "P2c match_at_price_level body",
)

# --- P2d: Bid-side call site passes price + maker_fills ---------------------
src = replace_once(
    src,
    """                        let matched_qty = match_at_price_level(
                            &mut price_levels[curr_level],
                            &mut remaining_order_qty,
                            &mut self.order_loc,
                        );
                        if matched_qty != 0 {
                            dbgp!("Matched {} qty at level {}", matched_qty, x);
                            fill_result.filled_orders.push((matched_qty, *x));
                        }
                        if let Some((a, _)) = price_map_iter.next() {""",
    """                        let matched_qty = match_at_price_level(
                            &mut price_levels[curr_level],
                            &mut remaining_order_qty,
                            &mut self.order_loc,
                            *x,
                            &mut fill_result.maker_fills,
                        );
                        if matched_qty != 0 {
                            dbgp!("Matched {} qty at level {}", matched_qty, x);
                            fill_result.filled_orders.push((matched_qty, *x));
                        }
                        if let Some((a, _)) = price_map_iter.next() {""",
    "P2d bid-side call site",
)

# --- P2e: Ask-side call site passes price + maker_fills ---------------------
src = replace_once(
    src,
    """                        let matched_qty = match_at_price_level(
                            &mut price_levels[curr_level],
                            &mut remaining_order_qty,
                            &mut self.order_loc,
                        );
                        if matched_qty != 0 {
                            dbgp!("Matched {} qty at level {}", matched_qty, x);
                            fill_result.filled_orders.push((matched_qty, *x));
                        }
                        if let Some((a, _)) = price_map_iter.next_back() {""",
    """                        let matched_qty = match_at_price_level(
                            &mut price_levels[curr_level],
                            &mut remaining_order_qty,
                            &mut self.order_loc,
                            *x,
                            &mut fill_result.maker_fills,
                        );
                        if matched_qty != 0 {
                            dbgp!("Matched {} qty at level {}", matched_qty, x);
                            fill_result.filled_orders.push((matched_qty, *x));
                        }
                        if let Some((a, _)) = price_map_iter.next_back() {""",
    "P2e ask-side call site",
)

# --- P1b: add_limit_order takes order_id and forwards it -------------------
src = replace_once(
    src,
    """    pub fn add_limit_order(&mut self, s: Side, price: u64, order_qty: u64) -> FillResult {""",
    """    pub fn add_limit_order(&mut self, s: Side, price: u64, order_qty: u64, order_id: u64) -> FillResult {""",
    "P1b add_limit_order signature",
)

src = replace_once(
    src,
    """            self.create_new_limit_order(s, price, remaining_order_qty);
        } else {""",
    """            self.create_new_limit_order(s, price, remaining_order_qty, order_id);
        } else {""",
    "P1c create_new_limit_order call",
)

# --- P3: public accessors for the harness queries --------------------------
# Insert right before the closing brace of `impl OrderBook` (which is the last
# `}` of the file). We anchor on `get_bbo`'s end + the impl close.
src = replace_once(
    src,
    """        println!(
            "Spread is {:.6},",
            ((self.best_offer_price - self.best_bid_price) as f64 / self.best_offer_price as f64)
                as f32
        );
    }
}""",
    """        println!(
            "Spread is {:.6},",
            ((self.best_offer_price - self.best_bid_price) as f64 / self.best_offer_price as f64)
                as f32
        );
    }

    // Harness patches: read accessors. These return the engine's OWN cached
    // BBO fields verbatim (no recomputation) so any staleness in update_bbo is
    // observed by the audit, not masked by the adapter.
    pub fn eng_best_bid_raw(&self) -> u64 {
        self.best_bid_price
    }
    pub fn eng_best_ask_raw(&self) -> u64 {
        self.best_offer_price
    }
    // Live aggregated qty at one price on one side (0 if no such level / empty).
    // Mirrors get_total_qty but tolerates a missing level.
    pub fn eng_depth_at(&self, price: u64, s: Side) -> u64 {
        let book = match s {
            Side::Bid => &self.bid_book,
            Side::Ask => &self.ask_book,
        };
        match book.price_map.get(&price) {
            Some(idx) => book.price_levels[*idx].iter().map(|o| o.qty).sum(),
            None => 0,
        }
    }
}""",
    "P3 public accessors",
)

# --- P4: cancel_order returns the removed order's (side, price, qty) --------
# WHY: upstream cancel_order returns only Result<&str,&str>, discarding the
# cancelled order's side / price / quantity. The harness CancelAck report
# carries all three (`2,seq,side,order_id,price`), and they are values the
# engine already holds — it just throws them away. We thread the resting price
# into the order_loc index (which already holds side + level) so cancel_order
# can hand them back. This is a read-path enhancement of the engine's own API,
# NOT a change to matching logic.

# P4a: order_loc field type carries the resting price.
src = replace_once(
    src,
    """    // For fast cancels Order id -> (Side, Price_level)
    order_loc: HashMap<u64, (Side, usize)>,""",
    """    // For fast cancels Order id -> (Side, Price_level, price)  [price added by harness patch]
    order_loc: HashMap<u64, (Side, usize, u64)>,""",
    "P4a order_loc field type",
)

# P4b: match_at_price_level's order_loc param type (in the P2c-patched body).
src = replace_once(
    src,
    """            order_loc: &mut HashMap<u64, (Side, usize)>,
            maker_price: u64,""",
    """            order_loc: &mut HashMap<u64, (Side, usize, u64)>,
            maker_price: u64,""",
    "P4b match_at_price_level param type",
)

# P4c: both order_loc inserts in create_new_limit_order carry the price.
src = replace_once(
    src,
    """            book.price_levels[*val].push_back(order);
            self.order_loc.insert(order_id, (s, *val));""",
    """            book.price_levels[*val].push_back(order);
            self.order_loc.insert(order_id, (s, *val, price));""",
    "P4c order_loc insert (existing level)",
)
src = replace_once(
    src,
    """            book.price_levels.push(vec_deq);
            self.order_loc.insert(order_id, (s, new_loc));""",
    """            book.price_levels.push(vec_deq);
            self.order_loc.insert(order_id, (s, new_loc, price));""",
    "P4d order_loc insert (new level)",
)

# P4e: cancel_order body — return (side_u8, price, removed_qty) or Err(()).
src = replace_once(
    src,
    """    pub fn cancel_order(&mut self, order_id: u64) -> Result<&str, &str> {
        if let Some((side, price_level)) = self.order_loc.get(&order_id) {
            let currdeque = match side {
                Side::Bid => self.bid_book.price_levels.get_mut(*price_level).unwrap(),
                Side::Ask => self.ask_book.price_levels.get_mut(*price_level).unwrap(),
            };
            currdeque.retain(|x| x.order_id != order_id);
            self.order_loc.remove(&order_id);
            Ok("Successfully cancelled order")
        } else {
            Err("No such order id")
        }
    }""",
    """    // Returns Ok((side_byte, price, removed_qty)) when the order was resting
    // and is now removed, Err(()) when no such order is resting. (Harness patch:
    // upstream returned Result<&str,&str> and discarded the order's fields.)
    pub fn cancel_order(&mut self, order_id: u64) -> Result<(u8, u64, u64), ()> {
        if let Some((side, price_level, price)) = self.order_loc.get(&order_id) {
            let side_byte: u8 = match side {
                Side::Bid => 0,
                Side::Ask => 1,
            };
            let price: u64 = *price;
            let price_level: usize = *price_level;
            let currdeque = match side_byte {
                0 => self.bid_book.price_levels.get_mut(price_level).unwrap(),
                _ => self.ask_book.price_levels.get_mut(price_level).unwrap(),
            };
            let removed_qty: u64 = currdeque
                .iter()
                .find(|x| x.order_id == order_id)
                .map(|x| x.qty)
                .unwrap_or(0);
            currdeque.retain(|x| x.order_id != order_id);
            self.order_loc.remove(&order_id);
            Ok((side_byte, price, removed_qty))
        } else {
            Err(())
        }
    }""",
    "P4e cancel_order body",
)

# --- P6: BBO correctness (two ENGINE bugs) ---------------------------------
# B1: update_bbo never resets best_bid_price / best_offer_price before its
#     scan, so when a side empties completely the loop finds no non-empty level
#     and the stale prior best survives. A best_bid/best_ask query then returns
#     a price that no longer exists in the book.
# B2: cancel_order never calls update_bbo, so even a cancel that merely empties
#     the current best level leaves the cached BBO stale until the next add.
# Both are real query-correctness bugs (the audit's best_bid/best_ask probes
# catch them). Minimal faithful fix: reset-then-scan in update_bbo, and refresh
# the BBO after a successful cancel.

# P6a: reset-then-scan in update_bbo.
src = replace_once(
    src,
    """    fn update_bbo(&mut self) {
        for (p, u) in self.bid_book.price_map.iter().rev() {""",
    """    fn update_bbo(&mut self) {
        // Harness patch (ENGINE BUG B1): reset to the empty sentinels first, so
        // a fully-emptied side reports no-bid/no-ask instead of a stale price.
        self.best_bid_price = u64::MIN;
        self.best_offer_price = u64::MAX;
        for (p, u) in self.bid_book.price_map.iter().rev() {""",
    "P6a update_bbo reset",
)

# P6b: cancel_order refreshes the BBO after a successful removal.
src = replace_once(
    src,
    """            currdeque.retain(|x| x.order_id != order_id);
            self.order_loc.remove(&order_id);
            Ok((side_byte, price, removed_qty))""",
    """            currdeque.retain(|x| x.order_id != order_id);
            self.order_loc.remove(&order_id);
            // Harness patch (ENGINE BUG B2): upstream never refreshed the BBO on
            // cancel, leaving best_bid/best_ask stale after the best level emptied.
            self.update_bbo();
            Ok((side_byte, price, removed_qty))""",
    "P6b cancel_order update_bbo",
)

with open(path, "w") as f:
    f.write(src)

sys.stderr.write("engine patched OK\n")
