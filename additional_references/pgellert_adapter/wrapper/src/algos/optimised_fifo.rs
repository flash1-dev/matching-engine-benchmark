use std::cmp::{max, min};
use std::collections::{HashMap, VecDeque};
use std::iter::FromIterator;

use crate::algos::book::*;

/// Price-time priority (or FIFO) matching engine implemented using a [HashMap] to index by price.
///
/// Implemented as a state-machine to be used for replication with Raft.
#[derive(Debug, Clone)]
pub struct FIFOBook {
    ask_price_buckets: HashMap<u64, VecDeque<Order>>,
    bid_price_buckets: HashMap<u64, VecDeque<Order>>,
    min_ask_price: u64,
    max_ask_price: u64,
    min_bid_price: u64,
    max_bid_price: u64,
    orders: HashMap<OrderId, Price>,
}

/// Order book interface implementation
impl FIFOBook {
    pub fn new() -> Self {
        Self {
            ask_price_buckets: HashMap::new(),
            bid_price_buckets: HashMap::new(),
            min_ask_price: u64::MAX,
            max_ask_price: u64::MIN,
            min_bid_price: u64::MAX,
            max_bid_price: u64::MIN,
            orders: Default::default(),
        }
    }

    fn pop_bid(&mut self) -> Option<Order> {
        for bid_price in (self.min_bid_price..=self.max_bid_price).rev() {
            match self.bid_price_buckets.get_mut(&bid_price) {
                Some(orders) if !orders.is_empty() => {
                    // Commit the bound only to a price that actually holds a live
                    // order; a price merely scanned over (empty bucket) must not
                    // ratchet the bound away from the real best level.
                    self.max_bid_price = bid_price;
                    let order = orders.pop_front().unwrap();
                    return Some(order);
                }
                _ => continue,
            }
        }
        None
    }

    fn pop_ask(&mut self) -> Option<Order> {
        for ask_price in self.min_ask_price..=self.max_ask_price {
            match self.ask_price_buckets.get_mut(&ask_price) {
                Some(orders) if !orders.is_empty() => {
                    // Commit the bound only to a price that actually holds a live
                    // order; a price merely scanned over (empty bucket) must not
                    // ratchet the bound away from the real best level.
                    self.min_ask_price = ask_price;
                    let order = orders.pop_front().unwrap();
                    return Some(order);
                }
                _ => continue,
            }
        }
        None
    }

    fn merge(&mut self, ask: Order, bid: Order) -> Option<(Trade, Option<Order>)> {
        let ask_id = ask.id();
        let bid_id = bid.id();
        let result = Order::merge(ask, bid);
        if let Some((_, remainder)) = &result {
            if remainder.as_ref().map_or(true, |rem| !rem.has_id(ask_id)) {
                self.orders.remove(&ask_id);
            }
            if remainder.as_ref().map_or(true, |rem| !rem.has_id(bid_id)) {
                self.orders.remove(&bid_id);
            }
        }
        result
    }

    /// Re-inserts a popped-but-unconsumed order at the FRONT of its price bucket
    /// (restoring its FIFO position) so quantity is conserved when a pop does not
    /// lead to a trade.
    fn unpop(&mut self, order: Order) {
        let buckets = match order.side {
            Side::Buy => &mut self.bid_price_buckets,
            Side::Sell => &mut self.ask_price_buckets,
        };
        buckets
            .entry(order.price)
            .or_insert_with(VecDeque::new)
            .push_front(order);
    }
}

impl Book for FIFOBook {
    /// Adds a buy or sell order to the book
    fn apply(&mut self, order: Order) {
        self.orders
            .insert((order.client_id, order.seq_number), order.price);

        match order.side {
            Side::Buy => {
                // Refresh the bound on EVERY insert, not only when a new bucket
                // is created: an order landing in a bucket that already exists
                // (including one emptied by an earlier scan) must still be able
                // to widen the live-best bound back out.
                self.min_bid_price = min(self.min_bid_price, order.price);
                self.max_bid_price = max(self.max_bid_price, order.price);
                let bucket_opt = self.bid_price_buckets.get_mut(&order.price);
                match bucket_opt {
                    None => {
                        self.bid_price_buckets
                            .insert(order.price, VecDeque::from_iter(vec![order]));
                    }
                    Some(bucket) => bucket.push_back(order),
                }
            }
            Side::Sell => {
                // Refresh the bound on EVERY insert, not only when a new bucket
                // is created (see the Buy arm above).
                self.min_ask_price = min(self.min_ask_price, order.price);
                self.max_ask_price = max(self.max_ask_price, order.price);
                let bucket_opt = self.ask_price_buckets.get_mut(&order.price);
                match bucket_opt {
                    None => {
                        self.ask_price_buckets
                            .insert(order.price, VecDeque::from_iter(vec![order]));
                    }
                    Some(bucket) => bucket.push_back(order),
                }
            }
        };
    }

    /// Fills tradeable orders in the book and returns the generated trades.
    fn check_for_trades(&mut self) -> Vec<Trade> {
        if self.max_bid_price < self.min_ask_price {
            return Vec::new();
        }

        let mut trades = vec![];

        // pop_bid / pop_ask REMOVE the front order from its bucket before
        // returning it. Every exit path below therefore re-inserts (via unpop)
        // any order it popped but did not consume in a trade — otherwise that
        // resting liquidity is silently dropped (quantity non-conservation).
        let (mut bid, mut ask) = match (self.pop_bid(), self.pop_ask()) {
            (Some(bid_new), Some(ask_new)) => (bid_new, ask_new),
            (Some(bid_new), None) => {
                self.unpop(bid_new);
                return trades;
            }
            (None, Some(ask_new)) => {
                self.unpop(ask_new);
                return trades;
            }
            (None, None) => return trades,
        };

        loop {
            // The cached price bounds can be stale-permissive (a pop never moves
            // a bound to a price it merely skipped over, and a cancel/empty does
            // not eagerly recompute it), so the guard above can let us in — and a
            // pop can hand back the best-on-each-side pair — even when that pair
            // does NOT actually cross. Re-insert both and stop; nothing here
            // trades. This is what makes the bounds safe to leave permissive.
            if ask.price > bid.price {
                self.unpop(bid);
                self.unpop(ask);
                return trades;
            }

            // ask.price <= bid.price: a real cross. merge() consumes both and,
            // for a partial fill, returns the unfilled remainder on one side.
            let (trade, remainder) = self
                .merge(ask, bid)
                .expect("crossing ask<=bid must produce a trade");
            trades.push(trade);

            match remainder {
                Some(rem) => match rem.side {
                    // The leftover stays in hand; pull a fresh order from the
                    // opposite side. If that side is empty, the leftover rests.
                    Side::Buy => match self.pop_ask() {
                        Some(ask_new) => {
                            ask = ask_new;
                            bid = rem;
                        }
                        None => {
                            self.unpop(rem);
                            return trades;
                        }
                    },
                    Side::Sell => match self.pop_bid() {
                        Some(bid_new) => {
                            bid = bid_new;
                            ask = rem;
                        }
                        None => {
                            self.unpop(rem);
                            return trades;
                        }
                    },
                },
                // Exact fill: both orders are gone. Re-pop a fresh pair,
                // re-inserting a one-sided survivor instead of dropping it.
                None => match (self.pop_bid(), self.pop_ask()) {
                    (Some(bid_new), Some(ask_new)) => {
                        bid = bid_new;
                        ask = ask_new;
                    }
                    (Some(bid_new), None) => {
                        self.unpop(bid_new);
                        return trades;
                    }
                    (None, Some(ask_new)) => {
                        self.unpop(ask_new);
                        return trades;
                    }
                    (None, None) => return trades,
                },
            }
        }
    }

    /// Cancels the given order from the book
    fn cancel(&mut self, order_id: OrderId, side: Side) -> bool {
        if let Some(price) = self.orders.remove(&order_id) {
            let side_buckets = match side {
                Side::Buy => &mut self.bid_price_buckets,
                Side::Sell => &mut self.ask_price_buckets,
            };

            if let Some(bucket) = side_buckets.get_mut(&price) {
                if let Some(index) = bucket
                    .iter()
                    .position(|order| (order.client_id, order.seq_number) == order_id)
                {
                    bucket.remove(index);
                    return true;
                }
            }
        }

        false
    }
}

// ---------------------------------------------------------------------------
// Read-only query accessors added for the harness adapter. These do NOT touch
// the matcher's state machine (apply / check_for_trades / cancel above are
// byte-identical to upstream). They scan the LIVE buckets so the harness audit
// queries reflect the real book, independent of the cached
// min_/max_ price bounds (which pop_bid/pop_ask mutate as a scan cursor and
// which apply() only updates on the bucket-creation path).
impl FIFOBook {
    /// Highest bid price with resting quantity, or None if no bids rest.
    pub fn best_bid(&self) -> Option<u64> {
        self.bid_price_buckets
            .iter()
            .filter(|(_, b)| !b.is_empty())
            .map(|(p, _)| *p)
            .max()
    }

    /// Lowest ask price with resting quantity, or None if no asks rest.
    pub fn best_ask(&self) -> Option<u64> {
        self.ask_price_buckets
            .iter()
            .filter(|(_, b)| !b.is_empty())
            .map(|(p, _)| *p)
            .min()
    }

    /// Total resting quantity at one (price, side); 0 if empty.
    pub fn depth_at(&self, price: u64, side: Side) -> u64 {
        let buckets = match side {
            Side::Buy => &self.bid_price_buckets,
            Side::Sell => &self.ask_price_buckets,
        };
        match buckets.get(&price) {
            Some(bucket) => bucket.iter().map(|o| o.size).sum(),
            None => 0,
        }
    }
}
