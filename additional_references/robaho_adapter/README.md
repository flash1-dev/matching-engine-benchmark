# robaho_adapter — integration example

Wraps [robaho/cpp_orderbook](https://github.com/robaho/cpp_orderbook) behind
`api/matching_engine_api.h`. The engine depends on
[robaho/cpp_fixed](https://github.com/robaho/cpp_fixed) for its `Fixed<7>`
price type.

Pinned commits:
- `robaho/cpp_orderbook` — `f42358145e40015f709f1caa04670f88c8b8be40`
- `robaho/cpp_fixed`     — `e6bdb17d4ac9bb871ac34666a5bcb0563a027703`

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `discoveries.md` at the
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

A one-token source-level fix is required for C++20 conformance:
`exchange.h` and `bookmap.h` declare `std::vector<const std::string>`, which
is ill-formed under C++20+ (vector of const elements). `build.sh` `sed`s out
the `const`. The methods that use those vectors are not called from the
adapter; pure source-level fix, no semantics change.

## Build / run

```bash
bash additional_references/robaho_adapter/build.sh
./harness --engine robaho_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

`build.sh` clones the two repos into `third_party/robaho_cpp_orderbook/` and
`third_party/robaho_cpp_fixed/` at the pinned commits, then patches the two
headers. Overrides: `ME_ROBAHO_SRC` and `ME_FIXED_SRC` use existing
checkouts in place of cloning.
