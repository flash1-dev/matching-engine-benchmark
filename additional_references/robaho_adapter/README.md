# robaho_adapter — integration example

Wraps [robaho/cpp_orderbook](https://github.com/robaho/cpp_orderbook) behind
`api/matching_engine_api.h`. The engine depends on
[robaho/cpp_fixed](https://github.com/robaho/cpp_fixed) for its `Fixed<7>`
price type.

Pinned commits:
- `robaho/cpp_orderbook` — `f42358145e40015f709f1caa04670f88c8b8be40`
- `robaho/cpp_fixed`     — `e6bdb17d4ac9bb871ac34666a5bcb0563a027703`

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

`Exchange` facade + `ExchangeListener` callback, single-threaded matcher.
Native APIs visible from the adapter:

- `Exchange::buy/sell(sessionId, instrument, F price, qty, orderId)` —
  submits a limit order; returns an `exchangeId` (long).
- `Exchange::marketBuy/marketSell` — submits a market order.
- `Exchange::cancel(exchangeId, sessionId)` — returns `int` (0 success).
- `ExchangeListener::onTrade(Trade)` — fires synchronously inside the
  matching loop; carries `aggressor` (taker) and `opposite` (maker) refs.
- `Exchange::book(instrument)` — `Book` snapshot with bid/ask levels.

Not provided natively: no IOC / FOK / POST-ONLY; no native modify;
`std::string` order ids (harness uses `uint64_t`); no Reject events.

## Adapter strategy

- `HarnessListener : ExchangeListener` captures every `onTrade` into a
  global fill buffer (the single matcher thread is the only caller) that
  the adapter drains into Trade reports after each submit; the taker id is
  parsed from the engine's own `aggressor` payload on each trade.
- Prices: workload `int64_t` ticks written into `Fixed<7>(int64_t, 0)`
  (stores `ticks * 10^7` as fp). Tick ordering is preserved bit-for-bit
  through Fixed's internal comparator.
- Order-id mapping: `std::to_string(oid)` on submit; `std::stoull` on the
  trade callback.
- **IOC**: post-submit `Exchange::cancel` of the residual + emit `CancelAck`
  (the engine has no IOC flag).
- **Modify**: explicit cancel + re-submit (engine has no native modify).
- A per-order shadow (a flat vector indexed by the dense harness order id)
  remembers the engine-assigned `exchangeId` — the engine names orders with
  its own ids, so the translation map is unavoidable — plus the side/price/
  remaining echoed on CancelAck/ModifyAck. For any *recorded* exchangeId the
  engine's own `cancel` return code adjudicates success vs reject; only ids
  with no recorded exchangeId (never seen, or IOC — never resting) reject in
  the adapter as a translation gap.

## Source patch

`build.sh` applies **two** source patches to the engine, each after
`git reset --hard` to the pin so the reset can never clobber them. The first is
a build-time conformance fix with no semantic effect; the second is the engine
**correctness** patch, applied unconditionally — so `robaho_adapter.so` is the
fixed engine, and the harness classifies robaho **"with fix"**.

1. **C++20 conformance** (`sed`, `exchange.h` + `bookmap.h`). Both headers
   declare `std::vector<const std::string>`, which is ill-formed under C++20+
   (`std::vector` of a `const` element type). `build.sh` `sed`s the `const` out
   of those vector types. The accessor methods that use these vectors are never
   called from the adapter; this is a pure source-level fix with no change to
   matching, prices, or quantities. (Not a correctness finding — purely to make
   the engine compile under the harness's C++20 toolchain.)

2. **Maker-priced fill** (Python anchored-replace, `orderbook.cpp`). Filed
   upstream as
   [robaho/cpp_orderbook#2](https://github.com/robaho/cpp_orderbook/issues/2)
   (and recorded in `CORRECTNESS_FINDINGS.md` at the repo root). This is the
   real engine defect that makes robaho "with fix". `OrderBook::matchOrders`
   prices every fill as `F price = MIN(bid->_price, ask->_price)`
   (`orderbook.cpp:24`). A cross only happens when `bid->_price >= ask->_price`
   (`orderbook.cpp:22`), so that `MIN` always returns `ask->_price` — correct
   when a buyer lifts a resting ask (the ask is the maker), but wrong when a
   seller hits a higher resting bid: the print is then the aggressor's own lower
   limit instead of the resting maker's price, and `fill()` folds that wrong
   price into both orders' `averagePrice()`. Under price-time priority the
   resting (maker) order sets the execution price. `matchOrders` already names
   the resting side as `opposite`; the patch hoists the
   `aggressor`/`opposite` computation above the price line and prices the fill
   from `opposite->_price`. The aggressive-buy path is unchanged
   (`opposite == ask`, so `opposite->_price == MIN(bid,ask)`); the sell-aggressor
   path is fixed. Quantities, `remaining`, and book state are untouched. The
   replace is anchored so an upstream change fails the build loudly rather than
   silently no-op'ing the fix, and is idempotent (a marker guard makes a
   re-apply under an `ME_ROBAHO_SRC` override a no-op).

   This patch is **necessary and sufficient** on `normal`: built against the
   unmodified engine the report-stream hash diverges (the state audit still
   passes — only the *printed price* is wrong, not the book), and with the patch
   the hash matches and the verdict is `VALID`. The first baseline divergence on
   `normal` is a sell crossing a higher resting bid (same qty/maker/taker,
   different price). `static` never crosses a resting bid with a lower-priced
   sell, so it passes either way.

## Build / run

```bash
bash additional_references/robaho_adapter/build.sh
./harness --engine robaho_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

`build.sh` clones the two repos into `third_party/robaho_cpp_orderbook/` and
`third_party/robaho_cpp_fixed/` at the pinned commits, applies the two source
patches above (the C++20 conformance `sed` on the two headers and the
maker-price correctness patch on `orderbook.cpp`), and compiles the engine +
this adapter into `robaho_adapter.so` at the repo root with the system `g++`
(C++20). Overrides: `ME_ROBAHO_SRC` and `ME_FIXED_SRC` use existing
checkouts in place of cloning (both patches re-apply idempotently).
