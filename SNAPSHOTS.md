# Snapshots — pinned upstream commits

This file records the upstream commit each surveyed engine was observed at, so
every observation in
[`CORRECTNESS_FINDINGS.md`](CORRECTNESS_FINDINGS.md) is reproducible against a
fixed point in time. That document carries a one-line correctness verdict and the
filed-issue link per engine (the full mechanism now lives in each issue); this one
carries the provenance.

Source observations for the original eleven shipped engines were first recorded on
**2026-05-24** (the original eight) and **2026-06-09** (three high-throughput-claim
engines) against these upstreams. On **2026-06-11** two things changed and every
verdict for those eleven was re-derived in a single confined session on the result
(the author-contributed cpp-orderbook adapter — the twelfth — and the
wider-audit engines were examined later, 2026-06-20 onward, against the same
canonical workload):

- **All adapter shims were reworked** to a minimal-overhead form —
  flat-vector id translation, no adapter-side locks or per-message
  allocation, and the engine's own API wherever the engine provides one
  (each adapter README documents its mapping). One verdict was corrected in
  the process (Tzadiko — see `CORRECTNESS_FINDINGS.md`).
- **The canonical workload was re-anchored.** The generator implements the
  paper benchmark's construction; how deep a standing book a run carries is
  a property of the seed's price-path realisation (see
  `docs/METHODOLOGY.md`, *The standing book*). The previously published
  canonical seed produced realisations whose moving scenarios ran against a
  nearly empty resting book, exercising none of the resting-set pressure the
  paper's own benchmark runs carried. The canonical seed is now **23**,
  chosen so each scenario's realisation matches the paper benchmark's
  standing-book and fill profile scenario for scenario; a marketable-order
  fraction that the earlier harness workload added on top of the paper's
  model was removed at the same time. All five reference hashes were
  regenerated from the byte-identical consensus.

## Pinned commits

Every engine driven through the harness, with the upstream commit it was observed
at. The forty engines that ship pinned reference adapters in
[`additional_references/`](additional_references/) — listed in the
[`CORRECTNESS_FINDINGS.md`](CORRECTNESS_FINDINGS.md) roster — are cloned at the commit below by
their `build.sh`; the remaining audited engines were examined from source and are
not shipped here. The
four reference engines (the three baselines — Liquibook, QuantCup, Exchange-core —
plus FlashOne, the reference target) are not
pinned third-party adapters and are omitted.

| Engine         | Repository                                                             | Commit                                     | Lang       |
|:---------------|:-----------------------------------------------------------------------|:-------------------------------------------|:-----------|
| cpp-orderbook  | geseq/cpp-orderbook                                                    | `81e5a29fc6f0f64b75f2e9534ee39ab5e66fe2aa` | C++        |
| CppTrader      | chronoxor/CppTrader                                                    | `831d10e2a6dd96ac7b063f1d418f6563cbf74c50` | C++        |
| Kautenja       | Kautenja/limit-order-book                                              | `88416a12a0b34b026cbf1d598823fd315a1f2dbf` | C++        |
| llc993         | llc-993/matching-core                                                  | `2cb21c0a67b34b01ad97e2394a649fc77e33aa8b` | Rust       |
| hroptatyr/clob | hroptatyr/clob                                                         | `812137a3edca4e00f05ac8b3ff2212c5deb545a5` | C          |
| mercury        | eelixir/mercury                                                        | `4742cc43e4b6233b77a7b848eccbb8cd778003b0` | C++        |
| microexchange  | Leotaby/MicroExchange                                                  | `edb6765c728370f44af0d85575399c95204ca1f1` | C++        |
| Tzadiko        | Tzadiko/Orderbook                                                      | `dd136dd219ead95796f0e396e9e1395542bf673f` | C++        |
| fmstephe       | fmstephe/matching_engine                                               | `fdc2088cfe508d78e2ec5fa6dfa2d8cb3a189873` | Go         |
| coralme        | coralblocks/CoralME                                                    | `6d0f94898f05ca7059a79551132be16c17785863` | Java       |
| gocronx        | gocronx/matcher                                                        | `b8d48356c8a2677e0d8a1965d754e3c4884bb947` | Rust       |
| geseq          | geseq/orderbook                                                        | `ba3a635425eb910fdf018643ccac92fb4aca526a` | Go         |
| jiang          | JiangYongKang/FastMatchingEngine                                       | `8a3b597a042e402cd8bd5c95fc2d3b0884913022` | Java       |
| m5487          | 0x5487/matching-engine                                                 | `e5d3129ce195da0e6045f7d6a00ef26972d46afd` | Go         |
| jlob           | eliquinox/jLOB                                                         | `c78c2a2ce77c339b2343a1678f881fc9749fbd87` | Java/JNI   |
| i25959341      | i25959341/orderbook                                                    | `0d883ab1157580d58ba9f2b9c537a3363310231c` | Go         |
| mh2rashi       | mh2rashi/Trading-Engine                                                | `a6631ab944f53427effc6cd5171d3201f183ba31` | C++        |
| gotrader       | robaho/go-trader                                                       | `1d34bc8206d7931939e02142f582a0a009b1da3b` | Go         |
| pyme           | Surbeivol/PythonMatchingEngine                                         | `f94150294a85d7b415ca4518590b5a661d6f9958` | Python     |
| oceanbook      | draveness/oceanbook                                                    | `a7768eed53a239faf883144090fd48931129f145` | Go         |
| dsirotkin      | dsirotkin256/matching-cpp                                              | `ebb442085f6b93d8eb0393ff8b145ebbe96f30af` | C++        |
| brprojects     | brprojects/Limit-Order-Book                                            | `af6e5349874649fe196bd6c26653d357f5a751f2` | C++        |
| dyn4mik3       | dyn4mik3/OrderBook                                                     | `a802407d12d2a21d0c8d65d44cc93dc5634f576b` | Python     |
| trademacher    | TradeMatcher/match-engine                                              | `552c71a83f0d28808048189a1153a6463ea661ef` | Java/JNI   |
| dgtony         | dgtony/orderbook-rs                                                    | `cba8329b1f6cb2156c734b4cfab8ab0cc5566cc6` | Rust       |
| pantelwar      | Pantelwar/matching-engine                                              | `12c779494814187c7c9c10a6731537011792f716` | Go         |
| jeog           | jeog/SimpleOrderbook                                                   | `3411cebb9756b80fd2cb3b442cfb109ca853068b` | C++        |
| philipgreat    | philipgreat/lighting-match-engine-core                                 | `381aeda4298524758db37d90c9a69f0fa5c8ca6c` | Rust       |
| cointossx      | dharmeshsing/CoinTossX                                                 | `89090edcd15a06f4ed821890adfc8f377ed7d7c7` | Java/JNI   |
| pyxchange      | pavelschon/PyXchange                                                   | `b35f0ebeb8ce008e605987305a2d52194785fbb8` | C++        |
| auralshin      | auralshin/orderbook                                                    | `cb8049181aa18d01e8f968bd68b8efb5391d9118` | Rust       |
| gavincyi       | gavincyi/LightMatchingEngine                                           | `5e210a809e62a802107831d0ca12498ed32d4717` | Python     |
| fasenderos     | fasenderos/nodejs-order-book                                           | `f8e285bd2179392abe358ecb02f0fd3b76486178` | TypeScript |
| ridulfo        | ridulfo/order-matching-engine                                          | `30fdbf579671325cf682492037d804b03b5baceb` | Python     |
| lobster        | rubik/lobster                                                          | `0b9720ca1e7dd1f81ecd35d1062c0d3044d5607d` | Rust       |
| techieboy      | TechieBoy/rust-orderbook                                               | `468fef7fb86c6191d8a2fb4c4ad1d9fb88ec0a26` | Rust       |
| piyush         | PIYUSH-KUMAR1809/order-matching-engine                                 | `033d7859186bdc7e265b76883da5515722f7f249` | C++        |
| limitbook      | solarpx/limitbook                                                      | `943eadc181d1e35a26abaa5217eeb32bf3304267` | Rust       |
| robaho         | robaho/cpp_orderbook                                                   | `f42358145e40015f709f1caa04670f88c8b8be40` | C++        |
| jxm35          | jxm35/LimitOrderBook-MatchingEngine                                    | `b5984aacb1f9a1816855df4942752711866dbfbf` | C++        |
| OrderBook-rs   | joaquinbejar/OrderBook-rs                                              | `53b4d2b0a657f4260e316d3a8ac3f0df0fc068bf` | Rust       |
| femto_go       | ejyy/femto_go                                                          | `46667a95064bd028e8f0ec1bc6a2f776d86721e3` | Go         |
| mansoor        | mansoor-mamnoon/limit-order-book                                       | `78e1fb0e0563388456e5030d858ef43d6407bed3` | C++        |
| cheetah        | CheetahExchange/orderbook-rs                                           | `caa33f34440056211105ac9933d2d8bc35f94e92` | Rust       |
| danielgatis    | danielgatis/go-orderbook                                               | `7640955559eb5473c36a56507d3eadf830c66713` | Go         |
| lanpishu       | lanpishu6300/crypto-exchange                                           | `fff19262bc7b54531dbe13fe26da86725659acc5` | C++        |
| lightning      | xiiiew/lightning-engine                                                | `9ab661f4b2dee7ce4838291aee2ac92aaa8d0092` | Go         |
| apex           | crypto-zero/apex-engine                                                | `9b0300d70f60939c41da7af5bcd9808797a1312d` | Rust       |
| darkpool       | dendisuhubdy/dark_pool                                                 | `92bc3382bda9375829a2267ac3e96a80802b60cf` | C++        |
| matchina       | fran0x/matchina                                                        | `0484d1b1c190cca9891306a31fb2906eafbbf155` | Rust       |
| laffini        | Laffini/Java-Matching-Engine-Core                                      | `20d5e162f2c605773f7f1bf37d5a0287b8b24c8c` | Java       |
| luo4neck       | luo4neck/MatchingEngine                                                | `69fe6d21fda621f5f6a0e2e4a07decbb59793dbf` | C++        |
| piquette       | piquette/orderbook                                                     | `df1b8fe8f12ae6cb7205b66f595b8be4f4607a66` | Go         |
| cspooner       | christian-spooner/trading-server                                       | `d0b6e271101cb24c55bb297790b2dbb8e5141c0b` | Rust       |
| zackienzle     | ZacKienzle2/Orderbooks                                                 | `ab78aa8346bd2a4e7345c4e6223d22ca8023817d` | C++        |
| mtengine       | JiaoziExchange/mt-engine                                               | `c51d3de089bf7afd5ed39b24dfcd5e11a2d8fc8a` | Rust       |
| asthamishra    | AsthaMishra/matching-engine                                            | `317c092843d3a5cc6730ceed6c56bb5598ab8fb7` | Rust       |
| pgellert       | pgellert/matching-engine                                               | `de195a8227b942f10fd5cb41934d1ce325dd8dd9` | Rust       |
| matchingo      | GOnevo/matchingo                                                       | `7aa642f0ffc8dfd509119b1d432b8745fb1dfcc5` | Go         |
| omerhalid      | omerhalid/Real-Time-Market-Data-Feed-Handler-and-Order-Matching-Engine | `fe74ae27457d4c09252d02f3231c029575e11934` | C++        |

A second Tzadiko/Orderbook finding (the `~Orderbook()` teardown lost-wakeup,
`dd136dd219ead95796f0e396e9e1395542bf673f`) is **not** given a separate row: it is the same repository and pinned
commit as the integrated Tzadiko adapter in the table above, observed at the same
point.

Eight rows — `mercury`, `microexchange`, `auralshin`, `m5487`, `apex`,
`darkpool`, `luo4neck`, `omerhalid` — had their audit clones reaped, but their
upstream repositories were identifiable, so the commit was resolved as the
repository's default-branch HEAD as it stood at the 2026-06-21 audit. Each of
those repositories was dormant — its latest commit predates the audit by months
— so the resolved commit is the one that was audited.

The shipped reference adapters are not maintained, and every engine here was
driven at the commit named — if any upstream advances past its pinned commit the
source-level observations may no longer apply; treat this as a record of one
point in time.
