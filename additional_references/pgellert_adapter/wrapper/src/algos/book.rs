// Vendored verbatim from pgellert/matching-engine engine/src/algos/book.rs
// (pinned de195a8). ONLY the three protobuf/raft-bridge methods
// (from_proto / into_proto / from_command) are removed, because they pull in
// the engine's tonic/prost/raft stack which is irrelevant to the matcher and
// would force the whole gRPC/Raft server to build. Every byte of the matching
// logic — Order, Side, Trade, Order::merge, the Buy/Sell Ord impls, and the
// Book trait — is unchanged from upstream, so the matcher under test is the
// engine's own.
use std::cmp::{Ord, Ordering};

pub type ClientId = u64;
pub type SequenceNum = u64;
pub type OrderId = (ClientId, SequenceNum);
pub type Price = u64;

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum Side {
    Buy,
    Sell,
}

/// Container for data about an order
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Order {
    pub client_id: ClientId,
    pub seq_number: SequenceNum,
    pub price: Price,
    pub size: u64,
    pub side: Side,
}

impl Order {
    fn partial_cmp_buy(&self, other: &Self) -> Option<Ordering> {
        Some(self.price.cmp(&other.price))
    }

    fn partial_cmp_sell(&self, other: &Self) -> Option<Ordering> {
        Some(other.price.cmp(&self.price))
    }

    /// Merges two orders and, if they are tradeable, returns the generated trade and an optional
    /// order that remains from filling the two inputs.
    pub fn merge(self, other: Self) -> Option<(Trade, Option<Self>)> {
        let (ask, bid) = match (self.side, other.side) {
            (Side::Buy, Side::Sell) => (other, self),
            (Side::Sell, Side::Buy) => (self, other),
            (_, _) => return None,
        };

        if ask.price > bid.price {
            return None;
        }

        match ask.size.cmp(&bid.size) {
            Ordering::Equal => {
                let quantity = ask.size;
                Some((Trade { quantity, ask, bid }, None))
            }
            Ordering::Greater => {
                let quantity = bid.size;
                let mut remainder = ask.clone();
                remainder.size -= quantity;
                Some((Trade { quantity, ask, bid }, Some(remainder)))
            }
            Ordering::Less => {
                let quantity = ask.size;
                let mut remainder = bid.clone();
                remainder.size -= quantity;
                Some((Trade { quantity, ask, bid }, Some(remainder)))
            }
        }
    }

    #[inline]
    pub(crate) fn has_id(&self, order_id: OrderId) -> bool {
        (self.client_id, self.seq_number) == order_id
    }

    #[inline]
    pub(crate) fn id(&self) -> OrderId {
        (self.client_id, self.seq_number)
    }
}

impl PartialOrd for Order {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        match (&self.side, &other.side) {
            (&Side::Buy, &Side::Buy) => self.partial_cmp_buy(other),
            (&Side::Sell, &Side::Sell) => self.partial_cmp_sell(other),
            (_, _) => None,
        }
    }
}

impl Ord for Order {
    fn cmp(&self, other: &Self) -> Ordering {
        self.partial_cmp(other).unwrap_or(Ordering::Equal) // Sell and Buy are non-comparable
    }
}

/// Container for data about generated trades
#[derive(Debug)]
pub struct Trade {
    pub quantity: u64,
    pub ask: Order,
    pub bid: Order,
}

/// Standardised order book interface
pub trait Book {
    fn apply(&mut self, order: Order);
    fn check_for_trades(&mut self) -> Vec<Trade>;
    fn cancel(&mut self, order_id: OrderId, side: Side) -> bool;
}
