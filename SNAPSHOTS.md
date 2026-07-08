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

Every third-party engine driven through the harness, with the upstream commit it
was observed at — both the engines that ship a pinned reference adapter in
[`additional_references/`](additional_references/) (cloned at the commit below by
their `build.sh`) and the wider-audit engines examined from source. Ordered
alphabetically by engine name. The four reference engines (the three baselines —
Liquibook, QuantCup, Exchange-core — plus FlashOne, the reference target) are not
pinned third-party adapters and are omitted.

| Engine | Repository | Commit | Lang |
|:--|:--|:--|:--|
| 820 | oldfifteenpoundy/weekend-orderbook | `c5a207246b32e0842c4509a10e25571376094c8a` | — |
| aas2015001 | aas2015001/ordermatchingengine | `a1978a1265cb0903902cce39079dd9d3ebd3b85f` | — |
| abides | jpmorganchase/abides-jpmc-public | `f9cbe51342b7dedd9587e4e069040d68a5c6477f` | Python |
| abyssbook | aldrin-labs/abyssbook | `0d31547974e4aa40b3aacf4e2beced7dfe9b97bb` | Zig |
| afterworkguinness | afterworkguinness/matching-engine | `c9d4aa42a6372af5866c0cab5ab62d52a68b530c` | Java |
| alphatrade | KangOxford/AlphaTrade | `2b1951fb47392db0e6d832c81a21a39a6b5eed05` | Python |
| amansardana | amansardana/matching-engine | `87687738c7e67e82e455013930db095cad4b744b` | Go |
| amer | AmerSurkovic/MatchingEngine | `facc8ee628b301d58b6db55d3d2dbdd6a61839e4` | Java |
| aodr3w | aodr3w/zero-alloc-lob | `cfc55aba9f64c15f2bc59f9269009a9dfd329726` | — |
| apex | crypto-zero/apex-engine | `9b0300d70f60939c41da7af5bcd9808797a1312d` | Rust |
| apexmatch | luka2049/apexmatch | `e9c9bcaefec1ee74ea4e3e1423c6f94170232192` | — |
| arjun | ArjunXvarma/mini-quant-trading-engine | `3b57751ce7ac8c2eea024f85f05c542b153e865e` | C++ |
| asthamishra | AsthaMishra/matching-engine | `317c092843d3a5cc6730ceed6c56bb5598ab8fb7` | Rust |
| auralshin | auralshin/orderbook | `cb8049181aa18d01e8f968bd68b8efb5391d9118` | Rust |
| bahbah94 | bahbah94/order-book-haskell | `4167845f124318ca595f820b74c53769baaa4ef7` | Haskell |
| baoyingwang | baoyingwang/orderbook | `90bca09c2c9b69faf85dc3efb347607a7e054adc` | Java |
| betting_exchange | danielkorzekwa/betting-exchange | `09230c92f1f2ba3d97b0cdc49e0d5443f8cf1753` | — |
| bexchange | bhomnick/bexchange | `4962bbc922bf15a6cfeab15773d16a8cd9cb6b76` | Go |
| big_order_book | capitalisk/big-order-book | `80dbfa143ee52f440f1a99b9b0ab979caa10da06` | JavaScript |
| bitex | blinktrade/bitex | `a4896e7faef9c4aa0ca5325f18b77db67003764e` | Python |
| bozoslav | bozoslav/order-book | `2ca8dc41289bf6d9a7668560fa17d436930b95d4` | C++ |
| brprojects | brprojects/Limit-Order-Book | `af6e5349874649fe196bd6c26653d357f5a751f2` | C++ |
| buttercoin | buttercoin/buttercoin-engine | `02ba2aa93be4ac07151a642d06aec0772ee40c06` | CoffeeScript |
| charles | CharlesMfouapon/limit-order-book | `471e19a54e19083c266ee1aa2266d0460fde6039` | Java |
| cheetah | CheetahExchange/orderbook-rs | `caa33f34440056211105ac9933d2d8bc35f94e92` | Rust |
| chessbr | chessbr/rust-exchange | (commit not retained) | Rust |
| chronex | osamaahmad00/chronex | `d55362f0aecfb994f59e7e564e447a38f3f9debe` | C++ |
| circus | seanoflynn/circus | `8f881b6b0f0b9b70d5faac0816e64fe68a70b47b` | C# |
| cjboxing | cjboxing/match | `ff26d5b59f02c9fa5e4fb8cd8a39fa37bd3db74a` | — |
| clober | clober-dex/v2-core | `984774e3336d0bac0a4118c0441fb08557349787` | Solidity |
| cltwski | cltwski/OrderBookSimulatorWithOpenCL | `b3d9d3d5360120e3d31121d6845e0c134720a06f` | C++ |
| coinexchange | jammy928/CoinExchange_CryptoExchange_Java | `8adf508b996020d3efbeeb2473d7235bd01436fa` | Java |
| cointossx | dharmeshsing/CoinTossX | `89090edcd15a06f4ed821890adfc8f377ed7d7c7` | Java/JNI |
| coralme | coralblocks/CoralME | `6d0f94898f05ca7059a79551132be16c17785863` | Java |
| cpp-orderbook | geseq/cpp-orderbook | `81e5a29fc6f0f64b75f2e9534ee39ab5e66fe2aa` | C++ |
| CppTrader | chronoxor/CppTrader | `831d10e2a6dd96ac7b063f1d418f6563cbf74c50` | C++ |
| cryptonstudio | cryptonstudio/crypton-matching-engine | `632ff123a7ecb48d61287ca577e5d0b8ecb84ce2` | Go |
| cspooner | christian-spooner/trading-server | `d0b6e271101cb24c55bb297790b2dbb8e5141c0b` | Rust |
| dabrowdev | codeberg.org/dabrowdev/nodejs-order-book-app | `d7c5f3ab14ced873ad9fc8fe4615105f397bac43` | TypeScript |
| damian | damian1000/orderbook | `91fb472a7caa5d7b515dd9031430eb3aa040ee55` | Kotlin |
| daniele | Daniele122898/Trading-Engine | `305a14f9860c5677aa79fc37323ebb14cd240de4` | C++ |
| danielgatis | danielgatis/go-orderbook | `7640955559eb5473c36a56507d3eadf830c66713` | Go |
| darkpool | dendisuhubdy/dark_pool | `92bc3382bda9375829a2267ac3e96a80802b60cf` | C++ |
| dazzz1 | dazzz1/warp-exchange | `27fea50012afa84c5ed6d26da836c10179a144bc` | — |
| deepbook | MystenLabs/deepbookv3 | `ed1c05449ba70b766cc731b8b98607f774fc6797` | Move |
| devashishpuri | devashishpuri/exchangematchingengine | `616bf057c16319e1c4e7649dbcddc58a26512db2` | — |
| dgtony | dgtony/orderbook-rs | `cba8329b1f6cb2156c734b4cfab8ab0cc5566cc6` | Rust |
| dhyey | dhyey-mehta/order-book | `9e24fe452bff663b5c8a6b5bc906a5e14651e44e` | C++ |
| dsirotkin | dsirotkin256/matching-cpp | `ebb442085f6b93d8eb0393ff8b145ebbe96f30af` | C++ |
| dx1ngy | dx1ngy/trading | `53d273657a8d3f066558fa6333f5df94f25252e9` | Java |
| dydx | dydxprotocol/v4-chain | `377842177d4a1d1a4078c9f01a92db3060c6d736` | Go |
| dylanlott | dylanlott/orderbook | `f82fb7ca33fc3d8c796497cf7f94a7e6a4d0c4cd` | Go |
| dyn4mik3 | dyn4mik3/OrderBook | `a802407d12d2a21d0c8d65d44cc93dc5634f576b` | Python |
| e2q | E2Quant/e2q | `4d82493d726f870663cb20c0bc1559609000d849` | C++ |
| econia | econia-labs/econia | `06af0a7f645402cce2cfbbbeef845e5c06737b0f` | Move |
| eneiand | eneiand/ordermatchingengine | `ded44cc6c95c5d77e70f7b137a46aaaf1e7b4667` | — |
| fasenderos | fasenderos/nodejs-order-book | `f8e285bd2179392abe358ecb02f0fd3b76486178` | TypeScript |
| faulaire | faulaire/matching-engine | `090a7e7414f478f1424a50668e3f5c2417646ba7` | C++ |
| femto_go | ejyy/femto_go | `46667a95064bd028e8f0ec1bc6a2f776d86721e3` | Go |
| ffhan | ffhan/tome | `d3d81c4f78fa712455a2f817bd0ddd3cfd61fdeb` | Go |
| figgie | bmillwood/figgie | `e38a28abcd4ce68865c351b14651fb3a40049f94` | OCaml |
| fjmurcia | fjmurcia/orderbook-rust | `b3192228861b2b121fe29abed5f72ea7941572b6` | Rust |
| fmstephe | fmstephe/matching_engine | `fdc2088cfe508d78e2ec5fa6dfa2d8cb3a189873` | Go |
| forever803 | forever803/trading_exchange | `0d77a53ed6e0ad5bbadf76609cbcff5835ce90f7` | C++ |
| fractal | fractalfintech/orderbook | `44bf9b24988b13afe70a8e1208805091fbde9005` | Java |
| gavincyi | gavincyi/LightMatchingEngine | `5e210a809e62a802107831d0ca12498ed32d4717` | Python |
| geseq | geseq/orderbook | `88e80980c691bcb62be8bd59ef9b2c04706e7c51` | Go |
| ghosh | PacktPublishing/Building-Low-Latency-Applications-with-CPP | `11e3925f431f7c8c39739ed90b874d6ff2aa2966` | C++ |
| glinscott | glinscott/jsorderbook | `2ed4674c030d690d5def28d3f6ac988ea7ac195b` | — |
| gocronx | gocronx/matcher | `b8d48356c8a2677e0d8a1965d754e3c4884bb947` | Rust |
| gotrader | robaho/go-trader | `1d34bc8206d7931939e02142f582a0a009b1da3b` | Go |
| harsh4786 | harsh4786/agnostic-orderbook-pinocchio | `af1ab1edc98c941de691b718e628273a8bf3a48e` | Rust |
| harshsuiiii | harshsuiiii/low-latency-trading-matching-engine-orderbook- | `8b4892540ea619c40cfe99f886577385f31c60c7` | TypeScript |
| hillside6 | hillside6/matching | `45216676a7f2ba6e84a1ee4851e4864821ca8f53` | — |
| hinokamikagura | hinokamikagura/crypto-wallet-engine | `7bee1565215d81f12eabca48d53998e25ae510c4` | — |
| hnodomar | hnodomar/spot-exchange | `f83d246f7c284ef007118337f4aa3a99cc24609b` | — |
| hroptatyr/clob | hroptatyr/clob | `812137a3edca4e00f05ac8b3ff2212c5deb545a5` | C |
| hyobyun | hyobyun/exchangeengine | `55ad630ae37e2b92aab851c3284b0508c85e86d1` | JavaScript |
| i25959341 | i25959341/orderbook | `0d883ab1157580d58ba9f2b9c537a3363310231c` | Go |
| instrument_spot | andry-ralambomanantsoa/instrument_spot | `139ffc555002bb01711c6df52c07a03170d7243c` | Rust |
| ironcrypto | ironcrypto/imlob | `f1dadeaa4ef64099f923fa806fcae6ba3a31aa97` | — |
| isaaccheng | IsaacCheng9/order-book-simulator | `65530f7ef39cda0f48cf3bdde3cc7b0c762d4212` | Python |
| isaaruwu | isaaruwu/ordermatchingengine | `a9b1b811e379b978b8f6bf9474d03b76242f1744` | — |
| ismailfer | ismailfer/exchange-simulator | `7aba33e6918b16ab01017f70e1bbdda8acc04ee0` | Java |
| iwtxokhtd83 | iwtxokhtd83/matchengine | `e0275e34174f4ed2d043715e1081e3dcba967d25` | Go |
| javalob | DrAshBooth/JavaLOB | `75af06836bc279289e6bad5c24fdc0440bb4cc57` | Java |
| jcwangjc | jcwangjc/exchange-matching-engine | `f137ef3c75099f5c4ce6a6d1fe05bbe89384ca82` | — |
| jenyayel | jenyayel/exchangematchingengine | `33ba539dc4672bfc526227cf1a6df889881e81ae` | C# |
| jeog | jeog/SimpleOrderbook | `3411cebb9756b80fd2cb3b442cfb109ca853068b` | C++ |
| jiang | JiangYongKang/FastMatchingEngine | `8a3b597a042e402cd8bd5c95fc2d3b0884913022` | Java |
| jiker_burce | jiker-burce/matching-engine | `29f9e7d728d35f0481552b9338993ea32de3a0fd` | Rust |
| jlob | eliquinox/jLOB | `c78c2a2ce77c339b2343a1678f881fc9749fbd87` | Java/JNI |
| jlome | alessandro-salerno/jlome | `1886de6ecfffffe825ec6001013f37861a34e8a4` | Java |
| joaquinbejar | joaquinbejar/hft-clob-core | `3a3793018cb07e2141450cde023af8ff4c3ea63f` | Rust |
| jogeshwar | jogeshwar01/exchange | `ed9f044dc79ee713da9518648524e0c68a70ddf7` | Rust |
| johannestampere | johannestampere/order_book_simulator | `4e753188f6985f8dab345f7a8a97aef03b69907d` | — |
| jpalounek | JPalounek/order-book | `dd3215d4f778e4f7ab48715b24e03254490cd93a` | Python |
| jugutier | jugutier/orderbook | `7fd61ff6f6366a014598d7c9eda267b078e8e54d` | Java |
| jxm35 | jxm35/LimitOrderBook-MatchingEngine | `b5984aacb1f9a1816855df4942752711866dbfbf` | C++ |
| jxxxq | Jxxxq/ocaml-orderbook-engine | `aff67154f9d334ecb801f31e137aa2de89f7c830` | OCaml |
| kartikeya | Kartikeya2710/order-matching-engine | `a44d9489d742ecaa047fb117e2922724684b0dc0` | C++ |
| Kautenja | Kautenja/limit-order-book | `88416a12a0b34b026cbf1d598823fd315a1f2dbf` | C++ |
| kennethzhang | kennethZhangML/TradingClientExchange | `2e9f6966e7ba91516e354aff6c9d6c6c2abdd6b0` | C++ |
| khrapovs | khrapovs/orderbookmatchingengine | `ba7825615948ea4528e2cc9dac4a047f6be388cd` | Python |
| knocte_fx | gitlab.com/knocte/fx | `548bc50279a0dc82e2b22c1cabe77fc6ab1c695e` | F# |
| kodoh | Kodoh/Orderbook | `e378705e1074193d1726f720a16b1bf97036411a` | C++ |
| konqr | konqr/lobSimulations | `f0d5b22a69d9cd0b7d9b3e881514c571c7189e39` | Python |
| koral | koralkulacoglu/fix-exchange | `8f42eb29f8a692d9ff9d2ba2f7ce880a4adb34fb` | C++ |
| laffini | Laffini/Java-Matching-Engine-Core | `20d5e162f2c605773f7f1bf37d5a0287b8b24c8c` | Java |
| landakram | landakram/orderbook-rs | `fd2c479bccc7fdd0a528c9bc9039a7f3cae6aa87` | Rust |
| lanpishu | lanpishu6300/crypto-exchange | `fff19262bc7b54531dbe13fe26da86725659acc5` | C++ |
| laymats | laymats/auto.trade.engie | `94855710a1f0acd17b99c1da690ce7166b0fb824` | — |
| lethalazo | lethalazo/cpp-order-matching-engine | `71309e7a973b27a29ec28f7e8f5f3cc98da1a111` | C++ |
| lightning | xiiiew/lightning-engine | `9ab661f4b2dee7ce4838291aee2ac92aaa8d0092` | Go |
| lightning (754liam) | 754liam/lightning | `c08e428d0c549cf6f98b74479016403f5f8ff693` | C++ |
| limitbook | solarpx/limitbook | `943eadc181d1e35a26abaa5217eeb32bf3304267` | Rust |
| liqian | QuantTradingWithLi/high_perf_order_matching | `016a3585ac26722208d19640eb72eab25815767b` | C++ |
| lirezap | lirezap/oms | `58786cc5470759725d92e63104f5505919ea165c` | Java |
| lirezap_oms | lirezap/oms | `58786cc5470759725d92e63104f5505919ea165c` | — |
| llc993 | llc-993/matching-core | `2cb21c0a67b34b01ad97e2394a649fc77e33aa8b` | Rust |
| lll | northwesternfintech/low-latency-league | `a3f1609f3172f68d1cf8e6e1b8886ab25d41d270` | C++ |
| lmxdawn | lmxdawn/exchange | `7d331e5013e08c122c5d498dc22375b5f74d9e6e` | Java |
| lobrs | rafalpiotrowski/lob-rs | `9f669ba1b72f6c052faf7a90ba4f440c62f915a1` | Rust |
| lobsim | kpetridis24/lobsim | `0cb48ed89a9cd5568e974d988214cfbebf51ca51` | C++ |
| lobster | rubik/lobster | `0b9720ca1e7dd1f81ecd35d1062c0d3044d5607d` | Rust |
| loom | alphagodzilla/loom | `c2c65987b5f5e3ac9598a06adc0ba3201f4eeb69` | — |
| lsamber | ls-amber/financial-trading-system | `edadb0617ca6bb26e7f8bb542b02d0c5fb4bf958` | — |
| lua_matcher | geek-sajjad/crypto-matching-engine-lua | `50b952cc895095dc179b59de0a40d492a81febe5` | — |
| luminengine | 0xhappyboy/luminengine | `5a6a4a115471f72cc1c50883e2521f09ec76b098` | Rust |
| luo4neck | luo4neck/MatchingEngine | `69fe6d21fda621f5f6a0e2e4a07decbb59793dbf` | C++ |
| lykke | LykkeCity/MatchingEngine | `937b360b10d461e444ebc7133dd9ab6b64ae4cb5` | Kotlin |
| lyqingye | lyqingye/match-engine | `4836cf628fbf32fc401c0c42fdc2d92cc288595c` | Java |
| m15102785298 | 15102785298/matching-algorithm | `9b11089b8360880ede4ff133f1323ec1acffdcba` | — |
| m5487 | 0x5487/matching-engine | `e5d3129ce195da0e6045f7d6a00ef26972d46afd` | Go |
| magenta_mice | UOA-CS732-SE750-Students-2022/project-group-magenta-mice | `0e1ca36bbee786a943ea538781bac3625d236722` | C++ |
| makersu | makersu/go-exchange-matching | `c68784bc5adb3834d9d35f4a1ede394d55826e25` | Go |
| manifest | Bonasa-Tech/manifest | `59568b3ea2dc286030e8ae6b6d2a4b34112bd790` | Rust |
| mansoor | mansoor-mamnoon/limit-order-book | `78e1fb0e0563388456e5030d858ef43d6407bed3` | C++ |
| masroor47 | masroor47/limit-order-book | `6c03c24e1f680b36ccfcb01d574ffd8311c7cc8c` | Python |
| matchcore | minyukim/matchcore | `ce465dadb3c5a8dab1c1bcc3c27dd28fcb00491b` | Rust |
| matchina | fran0x/matchina | `0484d1b1c190cca9891306a31fb2906eafbbf155` | Rust |
| matchingo | GOnevo/matchingo | `7aa642f0ffc8dfd509119b1d432b8745fb1dfcc5` | Go |
| mattdavey | mattdavey/EuronextClone | `0ae7d79a2c3832b8404d3f09709074c3f0df1657` | Java |
| maxe | maxe-team/maxe | `eaee05cf761c5894ec5383c8c996f344fef4d784` | C++ |
| melin | melin-engine/melin | `396059091e031c5a242fb9af7fc806b428e2a422` | Rust |
| mercury | eelixir/mercury | `4742cc43e4b6233b77a7b848eccbb8cd778003b0` | C++ |
| mercury_match | notayessir/mercury-match-engine | `ba02a1a1f393c9cfb2d0f6d43774f6f3c0fff46d` | — |
| mh2rashi | mh2rashi/Trading-Engine | `a6631ab944f53427effc6cd5171d3201f183ba31` | C++ |
| michaelliao | michaelliao/simple-match-engine | `61a7372bf5b0dc1574b4290a5c3df128afbf443e` | Java |
| microexchange | Leotaby/MicroExchange | `edb6765c728370f44af0d85575399c95204ca1f1` | C++ |
| mkhoshkam | mkhoshkam/orderbook | `3200fefbeaf2a2d186df5b7464013ebb6071d2f0` | Go |
| mkxzy | mkxzy/match-making | `d4b9004656d42be8f6f7c1e5f97e376055032a26` | — |
| mmrath | mmrath/oms | `343d46322cacf79d84cf74b8ba6e3aaed506b63b` | — |
| ms_engine | ? | (commit not retained) | TypeScript |
| mtengine | JiaoziExchange/mt-engine | `c51d3de089bf7afd5ed39b24dfcd5e11a2d8fc8a` | Rust |
| murtyjones | murtyjones/typescript-order-matcher-poc | `d9fd6773ddf99e7f858167e7ce2597e8977ca501` | TypeScript |
| muzykantov | muzykantov/orderbook | `f93a3deda34386b8af0d1100be128b07c3d273b7` | Go |
| nanobook | boringquantsystems/nanobook | `f22808c6149a3e981cce4ed0ff2af39a3074369e` | Rust |
| ndfex | matthewbelcher/NDFEX | `82486603dc9df75aa34dafd41747cc70f1b01acf` | C++ |
| newbigdeng | newbigdeng/tradesystem | `db376b107ca3fbb7ba04a663ca56944cf7f61d47` | C++ |
| nexbook | milczarekit/nexbook | `1095e422004a05f6c60f2b28678e8377015bc394` | Scala |
| nilesh05apr | nilesh05apr/TradeSim | `5b8662bdf48a86d32bab051710cdfd04415f10f8` | C++ |
| nirvanasu | nirvanasu00-cpu/Go-Exchange-Core | `680ed67e451ccc7fb8e0dac004237eda1ddeed6a` | Go |
| oceanbook | draveness/oceanbook | `a7768eed53a239faf883144090fd48931129f145` | Go |
| oldfritter | oldfritter/matching | `efdf83ea2ab068cadc478cd50fc3209692ba7644` | Go |
| omerhalid | omerhalid/Real-Time-Market-Data-Feed-Handler-and-Order-Matching-Engine | `fe74ae27457d4c09252d02f3231c029575e11934` | C++ |
| omx | 0xae/omx-engine | `bfc0139092957d89191083f7dccab74850bdbfc2` | C |
| onewhitedevil | 1white-devil/lob-matching-engine | `a5fa14b5734a74049f98870ada7d65701f68d5a5` | C++ |
| opencx | mit-dci/opencx | `7ad0b1f700eeb66c31aafa2efe33c9bbb98afae2` | Go |
| opexdev | opexdev/core | `acccd9b462067998ee5c257a5cdcc056233c08cc` | Kotlin |
| OrderBook-rs | joaquinbejar/OrderBook-rs | `53b4d2b0a657f4260e316d3a8ac3f0df0fc068bf` | Rust |
| osmosis | osmosis-labs/orderbook | `f49dffce99b2c46284b8ec15de5e16a3efaf4e56` | Rust |
| pantelwar | Pantelwar/matching-engine | `12c779494814187c7c9c10a6731537011792f716` | Go |
| parity | paritytrading/parity | `4671f8fda265c6d16b4b7e9b88ab807ee40712c5` | Java |
| peatio | openware/peatio | `bafe53030bfeaef1655154cc53b60ddaf3f74dcf` | Ruby |
| pgellert | pgellert/matching-engine | `de195a8227b942f10fd5cb41934d1ce325dd8dd9` | Rust |
| philipgreat | philipgreat/lighting-match-engine-core | `381aeda4298524758db37d90c9a69f0fa5c8ca6c` | Rust |
| phoenix | Ellipsis-Labs/phoenix-v1 | `5a34f7f901fd9e04057198d4fc7b7286f78b53f2` | Rust |
| php_matcher | nicolasguzca/php-trade-matching-engine | `6bb08e958044f79d5c264d061850e6adaee501f1` | — |
| piquette | piquette/orderbook | `df1b8fe8f12ae6cb7205b66f595b8be4f4607a66` | Go |
| piyush | PIYUSH-KUMAR1809/order-matching-engine | `033d7859186bdc7e265b76883da5515722f7f249` | C++ |
| plutus | bxptr/plutus | `27d34a2e66195a1dc271aa8faac93a525f90598c` | C++ |
| prystupa | prystupa/scala-cucumber-matching-engine | `f57443801403392acd746f0070e6647872e6f0d2` | — |
| pylob | DrAshBooth/PyLOB | `c0dd9328027b6d4b39a514b6458dc5589ba245e4` | Python |
| pyme | Surbeivol/PythonMatchingEngine | `f94150294a85d7b415ca4518590b5a661d6f9958` | Python |
| pyob | wegar-2/pyob | `5fa5d0d71ce6d3cdc1ff0709720224e5b4f94e70` | Python |
| pyobsim | jmcph4/pyobsim | `b3e7ffdf2625113b7ee240b1430989c4a2c84388` | Python |
| pyrsquant | tombelieber/py-rs-quant | `800aa5d82aa1dc439db82d175ff07cadfdcbe26d` | Rust |
| pyxchange | pavelschon/PyXchange | `b35f0ebeb8ce008e605987305a2d52194785fbb8` | C++ |
| qa-rs | yutiansut/qa-rs | `863ed065eecf6179522150a902c4de7c27b7bc77` | Rust |
| rabbittrix | rabbittrix/Ultra-Low-Latency-FX-eTrading-Platform | `d184a8a927ddaea40889fc3f228643423637f492` | Rust |
| rakuzen25 | rakuzen25/low-latency-league | `b2798cceeb24ca6ffc409c9278d20f0007ca012d` | C++ |
| ranjan2829 | ranjan2829/High-Frequency-Trading-Exchange-Engine | `82dc67811d7fd5c8ad7f1b19f6da45f3c664af88` | C++ |
| raunakchopra | raunakchopra/OrderBook | `119f9035fbff095e74ce6cb83d8b3c4696657ccc` | C++ |
| raymondshe | raymondshe/matchengine-raft | `5597532b82e7dd6e8684f77389190a0605665b73` | Rust |
| realyarilabs | realyarilabs/exchange | `b8b44fc9a44a264ea1e8cc607efcaa06caca55fa` | Elixir |
| redisexchange | jayjaychicago/RedisExchange | `ba625506b6e80bcc3433128588ca790505e7660a` | C++ |
| rhodey | rhodey/limit-order-book | `b1c700092bb2fd8110e2f3e390052dc884b26393` | JavaScript |
| ridulfo | ridulfo/order-matching-engine | `30fdbf579671325cf682492037d804b03b5baceb` | Python |
| rinok | film42/rinok | `1e9d606c6fdc83893d6f867b4851aceaad3e0fab` | Clojure |
| rishib064 | RishiB064/Rust-Limit-Order-Book | `cd5ce8d9662dfa62507fc2957620dc2a9d6c374b` | Rust |
| robaho | robaho/cpp_orderbook | `f42358145e40015f709f1caa04670f88c8b8be40` | C++ |
| robdev | rob-DEV/match-engine | `60895d73c4f342925d2d5cd1165336844a5a0d1f` | Rust |
| rust_ob | toyota-corolla0/rust_ob | `f8b5055c91a008cfe03b165fc80cda6d4ce119d9` | Rust |
| sadhbh | sadhbh-c0d3/cpp20-orderbook | `da4dfde5cc8169b9c38a5c540784da1fbe992e1f` | C++ |
| sculd | sculd/orderbook_practice_python | `e14b4f7c6ecada33ce0252865b5891c823f13c09` | Python |
| serum | project-serum/serum-dex | `92992b308885f5323b3f51eb1a0c899e35c62cb3` | Rust |
| shal | shal/orderbook | `ab921b1a0c2e83ba781053a167909fbb41fd5019` | Go |
| shaunlwm | shaunlwm/limitorderbook | `8179bedc3519f7d3fcca157e36c78c6ab4b4545f` | TypeScript |
| shilun | shilun/matchmaking | `d3744220ad5c763684996e3fd3fee42a663ea4c7` | Java |
| shivaganapathy | ShivaGanapathy/StockExchange | `bbf995f79fa1beebee5dc90305c65abaaf6f3e79` | C++ |
| shivamkachhadiya | gitlab.com/shivamkachhadiya/stock-order-matching-engine-cpp | `72014134c4c99ae53f369b9230e6c2f853445afa` | C++ |
| silue | silue-dev/limit-order-book-market-making | `e6108ff4eb40887bd5080f33dca2afb26f7b2e15` | — |
| slmolenaar | SLMolenaar/orderbook-simulator-cpp | `a7ff7404c34aba6e22ac5f5675114c95fb97bb53` | C++ |
| sohaibelkarmi | sohaibelkarmi/high-frequency-trading-simulator | `780809e655fe6ba7a3bd74d91966ed3b408da273` | — |
| soham | Soham109/binary-matching-engine | `0eee27677cc77eb97b919aa11274bdddd12973b9` | C++ |
| ssuchichen | ssuchichen/order-matching | `26da03a509a5357c98cb216a9df49469b73be0fa` | Go |
| stocksharp | StockSharp/StockSharp | `94f38b440eae5d06d961ab221cec49fc50a1617e` | C# |
| swirly | markaylett/swirly-java | `b41de6aa8c1a840f01f680e9aa8e4ac74a5686a6` | Java |
| techieboy | TechieBoy/rust-orderbook | `468fef7fb86c6191d8a2fb4c4ad1d9fb88ec0a26` | Rust |
| tembolo | tembolo1284/matching-engine-c | `e7da89b1ca844bf5bcc01bab1b69eaf860a6bc46` | C |
| thelilypad | thelilypad/orderbook_simulator | `a4827857dbf03fb2eb9f938c127840e4e10d55e5` | Python |
| timothewt | timothewt/orderbook | `14ee65089c1ff4c4617d1a35f04214023057e4af` | C++ |
| trademacher | TradeMatcher/match-engine | `552c71a83f0d28808048189a1153a6463ea661ef` | Java/JNI |
| trusted | JunbeomL22/trusted | `0ea128d76c491285ffb66281e98e488e666e742e` | Rust |
| turbo | sluggard6/turbo | `92adea51e6dd3b48d2c211d0046e5616bd5cbf8f` | Java |
| Tzadiko | Tzadiko/Orderbook | `dd136dd219ead95796f0e396e9e1395542bf673f` | C++ |
| vdt | vdt/matching-engine | `c75e7f2675f6bb4936313e9ae27a00b6ab8c4627` | JavaScript |
| vega | vegaprotocol/vega | `29d7f5e667fb1ec9aff09d0600d597f78c33c8ad` | Rust |
| viabtc | viabtc/viabtc_exchange_server | `2289c53f7d181b6c4ede1bd82edac6e1386d8434` | C |
| vinci217 | vinci-217/trading-system | `42bbf478ab89cdf43b8720a81698e46760240b38` | — |
| vincurious | vincurious/ordermatchingengine | `e244231d90b2a44828167bf7a23d06a693e3a6e1` | — |
| vllob | renruize12306/vllimitorderbook.jl | `8a7c14cb57ad2babd82bf68c63a34e50d70424fe` | Julia |
| volt | selimozten/volt | `b3abdaefdf096109d4d86387da5c56f783f7b125` | Zig |
| wailo | wailo/orderbook-matching-engine | `eb850dfaee5a00c457f3517e2cd9c785bea94a2b` | C++ |
| weblazy | weblazy/trade | `99fd34fc36d43bb112a2d7d57fabbef3dd1cdb71` | Go |
| wezrule | wezrule/wezostradingengine | `1dbc25caec72e856d7f648b3710a62c2e98767e4` | — |
| xingxing | crazyzym/xingxing-match-trading | `5263a8c8229036c6af82f348eee57ca55a3dc017` | Java |
| yashkukrecha | yashkukrecha/stock-trading-platform | `2b632593f9a241b5cc51caab7f0ec9f43d7861a6` | C++ |
| yihuang | yihuang/pyorderbook | `42d1671b7bcc78d925d5ff9e7095190bd2f68b61` | — |
| yllvar | yllvar/clob-exchange | `73aef2ce6aea21a2204a81f78bd98af12a981c2b` | — |
| zackienzle | ZacKienzle2/Orderbooks | `ab78aa8346bd2a4e7345c4e6223d22ca8023817d` | C++ |
| zhaocong6 | zhaocong6/match | `ddeb860007f8e7c8e57c15050ee89e5e379f7d37` | Go |
| zorrofix | dsec-capital/zorro-fix | `326481ee58be26b3dd88e2f159cbb9fa16284564` | C++ |
| zzsun777 | zzsun777/cpp_multithreaded_order_matching_engine | `a46209bfcac366eab83bd575ebdaa1a3bb93549c` | C++ |

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
