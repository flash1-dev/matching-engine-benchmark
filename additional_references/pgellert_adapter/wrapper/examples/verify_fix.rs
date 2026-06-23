// Native-API verification that the two drafted FIFOBook bugs are GONE on the
// patched engine. Drives the vendored engine book exactly as the adapter /
// MEStateMachine does (apply -> check_for_trades per message). Asserts the
// quantity-conservation and price-priority invariants the draft says were
// violated. Exits non-zero (panic) if any drafted bug is still present.
//
// Run from wrapper/:  cargo run --release --example verify_fix

use pgellert_adapter::repro_support::*;

fn drive(book: &mut FIFOBook, o: Order) -> usize {
    book.apply(o);
    book.check_for_trades().len()
}

fn book_total(book: &FIFOBook, side: Side, lo: u64, hi: u64) -> u64 {
    (lo..=hi).map(|p| book.depth_at(p, side)).sum()
}

fn main() {
    // ---- Drafted Bug 1: a popped order must NOT be dropped when the partner
    //      side turns up empty. Rest buy@50, fully cross with sell@50 (leaves an
    //      empty @50 ask bucket + stale min_ask=50), then a non-crossing buy@100
    //      x5 must REST (5 units conserved), not vanish.
    {
        let mut b = FIFOBook::new();
        drive(&mut b, ord(1, Side::Buy, 50, 1));
        drive(&mut b, ord(2, Side::Sell, 50, 1));
        let before = book_total(&b, Side::Buy, 1, 200);
        drive(&mut b, ord(3, Side::Buy, 100, 5));
        let after = book_total(&b, Side::Buy, 1, 200);
        assert_eq!(
            after,
            before + 5,
            "BUG 1 STILL PRESENT: non-crossing buy@100 x5 was dropped (before={before} after={after})"
        );
        assert_eq!(b.best_bid(), Some(100), "BUG 1: buy@100 not resting as best bid");
        assert_eq!(b.depth_at(100, Side::Buy), 5, "BUG 1: buy@100 qty not conserved");
        println!("Bug 1 (popped order dropped on one-sided match): FIXED — buy@100 x5 conserved & resting.");
    }

    // ---- Drafted Bug 2: stale price bounds must NOT hide a marketable order.
    //      A genuinely crossing incoming sell must trade and never leave the book
    //      crossed.
    {
        let mut c = FIFOBook::new();
        drive(&mut c, ord(10, Side::Buy, 50, 1));
        drive(&mut c, ord(11, Side::Sell, 50, 1));
        drive(&mut c, ord(12, Side::Buy, 100, 5));
        drive(&mut c, ord(13, Side::Sell, 90, 1));
        drive(&mut c, ord(14, Side::Sell, 90, 4));
        drive(&mut c, ord(15, Side::Buy, 100, 5));
        let n = drive(&mut c, ord(16, Side::Sell, 90, 5));
        let crossed = c
            .best_bid()
            .zip(c.best_ask())
            .map_or(false, |(bb, ba)| bb >= ba);
        assert!(n > 0, "BUG 2 STILL PRESENT: crossing sell@90 produced 0 trades");
        assert!(
            !crossed,
            "BUG 2 STILL PRESENT: book left crossed (best_bid={:?} >= best_ask={:?})",
            c.best_bid(),
            c.best_ask()
        );
        println!("Bug 2 (stale bounds hide a marketable order): FIXED — cross matched, book not crossed.");
    }

    // ---- The residual modify-path case the canonical exposed (price-equal pop
    //      of a NON-crossing pair must not drop the resting bid): rest buy@100,
    //      empty the @100 ask bucket so min_ask is stale at 100, then confirm a
    //      later equal-price reprice still finds the resting buy@100 to cross.
    {
        let mut d = FIFOBook::new();
        // Create an ask@100 then consume it so the @100 ask bucket is empty but
        // min_ask is parked at 100.
        drive(&mut d, ord(20, Side::Sell, 100, 1));
        drive(&mut d, ord(21, Side::Buy, 100, 1)); // fully trades the ask@100
        // Now a resting buy@100: guard passes (max_bid 100 == min_ask 100), the
        // popped (buy@100, ask@>100) pair does not cross — buy@100 must REST.
        drive(&mut d, ord(22, Side::Sell, 110, 3)); // a higher ask so a pop finds a non-crossing partner
        let n0 = drive(&mut d, ord(23, Side::Buy, 100, 7));
        assert_eq!(n0, 0, "setup: buy@100 should not cross ask@110");
        assert_eq!(d.depth_at(100, Side::Buy), 7, "RESIDUAL BUG: resting buy@100 x7 was dropped on a non-crossing equal-price pop");
        // A crossing sell@100 must now fill the resting buy@100.
        let n1 = drive(&mut d, ord(24, Side::Sell, 100, 7));
        assert!(n1 > 0, "RESIDUAL BUG: crossing sell@100 found no resting buy@100 to trade");
        assert_eq!(d.depth_at(100, Side::Buy), 0, "RESIDUAL BUG: buy@100 not consumed");
        println!("Residual (equal-price non-crossing pop dropped resting order): FIXED — buy@100 conserved & filled.");
    }

    println!("\nALL DRAFTED BUGS VERIFIED GONE (native FIFOBook API).");
}
