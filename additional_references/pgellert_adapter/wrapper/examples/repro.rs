// Minimal deterministic reproductions of TWO hard-invariant bugs in
// engine::algos::optimised_fifo::FIFOBook (pin de195a8) — the engine's
// PRODUCTION book (engine/src/rpc/me_state_machine.rs constructs it). Driven
// exactly as the engine's MEStateMachine drives it: per message,
// book.apply(order) then book.check_for_trades()
// (engine/src/rpc/me_state_machine.rs:97-99).
//
// Run from wrapper/:  cargo run --release --example repro
//
// The matching code under test is byte-identical to upstream (build.sh verifies
// it against the pin); only read-only best_bid/best_ask/depth_at accessors were
// added so this repro can observe the live book.

use pgellert_adapter::repro_support::*;

fn drive(book: &mut FIFOBook, tag: &str, o: Order) -> usize {
    let (side, price, qty) = (o.side, o.price, o.size);
    book.apply(o);
    let trades = book.check_for_trades();
    println!(
        "  {tag:<10} apply {:?} @{price} x{qty} -> {} trade(s)  [best_bid={:?} best_ask={:?} depth@{price}={}]",
        side, trades.len(), book.best_bid(), book.best_ask(),
        book.depth_at(price, side)
    );
    trades.len()
}

fn book_total(book: &FIFOBook, side: Side, lo: u64, hi: u64) -> u64 {
    (lo..=hi).map(|p| book.depth_at(p, side)).sum()
}

fn main() {
    // =====================================================================
    // BUG 1 — check_for_trades DROPS a popped order when the opposite side
    // turns up empty. optimised_fifo.rs:121-124:
    //     let (mut bid, mut ask) = match (self.pop_bid(), self.pop_ask()) {
    //         (Some(bid_new), Some(ask_new)) => (bid_new, ask_new),
    //         _ => return trades,                       // <-- popped order lost
    //     };
    // pop_bid()/pop_ask() REMOVE the order from its bucket; if the partner
    // pop returns None, the function returns WITHOUT re-inserting the order it
    // already popped. The resting order silently disappears — quantity is not
    // conserved.
    // =====================================================================
    println!("BUG 1 — popped order dropped on a one-sided match attempt:");
    let mut b = FIFOBook::new();
    // Rest a buy @50 and cross it fully with a sell @50. This leaves the @50
    // ask bucket present-but-EMPTY, and leaves min_ask_price = 50.
    drive(&mut b, "buy50", ord(1, Side::Buy, 50, 1));
    drive(&mut b, "sell50", ord(2, Side::Sell, 50, 1)); // both fully consumed
    // Now rest a fresh BUY @100 qty 5. check_for_trades pops this bid (best is
    // 100), then pop_ask scans from min_ask_price=50, finds the EMPTY @50
    // bucket, returns None -> the (Some(bid), None) arm returns and the buy @100
    // is DROPPED, never put back.
    let before = book_total(&b, Side::Buy, 1, 200);
    drive(&mut b, "buy100", ord(3, Side::Buy, 100, 5));
    let after = book_total(&b, Side::Buy, 1, 200);
    println!(
        "    resting buy qty before={before}, after applying a fresh non-crossing buy@100 x5: after={after}",
    );
    if after < before + 5 {
        println!(
            "    ==> BUG 1 CONFIRMED: the buy @100 x5 was accepted but is NOT in the book \
             (best_bid={:?}); 5 units of liquidity vanished. Quantity non-conservation.",
            b.best_bid()
        );
    }

    // =====================================================================
    // BUG 2 — stale price bounds make check_for_trades MISS a real cross. The
    // guard optimised_fifo.rs:115  `if self.max_bid_price < self.min_ask_price
    // { return Vec::new(); }` trusts cached bounds that pop_bid/pop_ask corrupt
    // (each pop unconditionally assigns the bound as it scans; a pop that finds
    // nothing walks the bound to the far end) and that apply() only refreshes
    // on the NEW-bucket path. A marketable incoming order is then hidden by the
    // guard and rests instead of trading, leaving the book CROSSED.
    // =====================================================================
    println!("\nBUG 2 — stale bounds hide a marketable order (book left crossed, no trade):");
    let mut c = FIFOBook::new();
    drive(&mut c, "buyA", ord(10, Side::Buy, 50, 1));
    drive(&mut c, "sellA", ord(11, Side::Sell, 50, 1));   // empties bid side; bounds corrupt
    drive(&mut c, "buyB", ord(12, Side::Buy, 100, 5));    // (also hit by BUG 1)
    drive(&mut c, "sellB", ord(13, Side::Sell, 90, 1));
    drive(&mut c, "sellC", ord(14, Side::Sell, 90, 4));
    drive(&mut c, "buyC", ord(15, Side::Buy, 100, 5));    // a genuine resting buy @100
    println!("    now a SELL @90 x5 — guaranteed to cross the resting buy @100:");
    let n = drive(&mut c, "sellD", ord(16, Side::Sell, 90, 5));
    if n == 0
        && c.best_bid().zip(c.best_ask()).map_or(false, |(bb, ba)| bb >= ba)
    {
        println!(
            "    ==> BUG 2 CONFIRMED: 0 trades, book left CROSSED (best_bid={:?} >= best_ask={:?}). \
             A marketable order was not matched — price-time priority violated, under-match.",
            c.best_bid(), c.best_ask()
        );
    }
}
