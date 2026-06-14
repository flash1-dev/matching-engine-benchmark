# Additional references

Eleven worked adapter examples that wrap third-party matching engines (six
C++, three Rust, two Go) behind `api/matching_engine_api.h`. Their purpose is
to show how an engine with a very different shape from the harness ABI can
be made to drive the harness — and, by extension, to make the observations
in `discoveries.md` reproducible from running code rather than asserted
from text.

**The additional references are not baselines.** The correctness consensus
is carried by the three baselines (Liquibook, QuantCup, Exchange-core)
under `scripts/build_baselines.sh`. The adapters here are
point-in-time **snapshots** — each `build.sh` clones the engine's upstream
at a pinned commit and patches it where needed:

| Adapter                | Lang | Upstream                                                        | Pinned commit                              |
|------------------------|------|-----------------------------------------------------------------|--------------------------------------------|
| `piyush_adapter/`      | C++  | https://github.com/PIYUSH-KUMAR1809/order-matching-engine       | `033d7859186bdc7e265b76883da5515722f7f249` |
| `mansoor_adapter/`     | C++  | https://github.com/mansoor-mamnoon/limit-order-book             | `78e1fb0e0563388456e5030d858ef43d6407bed3` |
| `jxm35_adapter/`       | C++  | https://github.com/jxm35/LimitOrderBook-MatchingEngine          | `b5984aacb1f9a1816855df4942752711866dbfbf` |
| `robaho_adapter/`      | C++  | https://github.com/robaho/cpp_orderbook (+ `robaho/cpp_fixed`)  | `f42358145e40015f709f1caa04670f88c8b8be40` |
| `cpptrader_adapter/`   | C++  | https://github.com/chronoxor/CppTrader                          | `831d10e2a6dd96ac7b063f1d418f6563cbf74c50` |
| `tzadiko_adapter/`     | C++  | https://github.com/Tzadiko/Orderbook                            | `dd136dd219ead95796f0e396e9e1395542bf673f` |
| `orderbookrs_adapter/` | Rust | https://github.com/joaquinbejar/OrderBook-rs                    | `53b4d2b0a657f4260e316d3a8ac3f0df0fc068bf` |
| `limitbook_adapter/`   | Rust | https://github.com/solarpx/limitbook                            | `943eadc181d1e35a26abaa5217eeb32bf3304267` |
| `philipgreat_adapter/` | Rust | https://github.com/philipgreat/lighting-match-engine-core       | `381aeda4298524758db37d90c9a69f0fa5c8ca6c` |
| `geseq_adapter/`       | Go   | https://github.com/geseq/orderbook                              | `3b9e9cd93cbaac02ba8359d2c3443a962d04c05f` |
| `femtogo_adapter/`     | Go   | https://github.com/ejyy/femto_go                                | `46667a95064bd028e8f0ec1bc6a2f776d86721e3` |

The adapters are not maintained: if an upstream moves past its pinned commit
the adapter may not build against the newer source, and any observation in
`discoveries.md` describes the snapshot above and only the snapshot above. A
project's current `main` may already differ.

Each subfolder has a short README that documents the engine's native API
shape and how the adapter maps it to the harness ABI. Findings — bug
observations, performance numbers — live exclusively in
[`../discoveries.md`](../discoveries.md) so they have one place to
read, update, and (if a project's behavior changes upstream) retire.

## Quick start

```bash
# build any single adapter, e.g.:
bash additional_references/piyush_adapter/build.sh
bash additional_references/cpptrader_adapter/build.sh
bash additional_references/orderbookrs_adapter/build.sh
bash additional_references/geseq_adapter/build.sh

# run against one of them:
./harness --engine piyush_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

Each `build.sh` honors a `ME_<name>_SRC=/path/to/local/checkout` environment
override to skip the clone (for offline use or to point at a different
revision).
