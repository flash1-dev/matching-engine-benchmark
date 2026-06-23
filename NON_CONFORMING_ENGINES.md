# Non-conforming engines

Companion to the [`README.md`](README.md) engine roster. A one-line finding per engine is in [`CORRECTNESS_FINDINGS.md`](CORRECTNESS_FINDINGS.md) (full mechanism in the linked issue); the pre-run conformance gate is described in [`docs/CONFORMANCE.md`](docs/CONFORMANCE.md).

These **8** engines either diverge from the byte-identical consensus on the canonical workload (over-matching, lost or orphaned orders, wrong execution price, quantity non-conservation, crashes, deadlocks) **or** conform on the canonical workload but carry a known latent defect the workload never triggers (caught by the pre-run conformance gate or by source review).

Non-conforming means only **"the output differs from the consensus"** (or a gate/source-review defect the timed run never exercises); it is not a judgment of engineering quality (`CORRECTNESS_FINDINGS.md` and the audit notes carry the mechanics).

| Engine        | Language | How it diverges | Worst-case M/s | Published figure | Notes |
|:--------------|:---------|:----------------|:---------------|-----------------:|:------|
| femto_go      | Go       | VALID on `static`/`normal`; diverges deterministically on the three wide-swing scenarios | 2.24 (normal) | >10 M/s | flat price array + slot index |
| cheetah       | Rust     | cancel searches the opposite book; a 10k-id de-dup window drops non-monotonic ids | 5.25 (normal) | — | BTreeMap keyed (price, id) |
| lanpishu (385★) | C++    | depth non-conservation + broken RB-tree delete-fixup (wrong best price; can hang) | 2.66 (static) | — | hand-rolled RB-tree + std::map levels |
| laffini       | Java     | crashes all five — `\|\|` should be `&&`, plus missing price-cross/empty-list guards | crash (all 5) | — | two sorted ArrayLists, re-sorted per insert |
| piquette      | Go       | bid/ask maps never populated → every cancel rejected; best-ask uses `>` not `<` | 1.28 (flash-crash) | — | AVL tree + DLL FIFO + hash |
| zackienzle    | C++      | hierarchical 4-tier bitmap, bounded price domain — crashes (OOB) on `swing-40`/`flash-crash` | crash (wide swings) | — | tick array + 4-tier bitmap + FIFO |
| mtengine      | Rust     | flat array + bitmap, bounded price domain — diverges on `flash-crash` / the widest swings | 6.82 (static) | — | 3-tier bitset + level array + FIFO |
| gavincyi (358★) | Python | crashes on a cancel/modify of a fully-filled order (conformance gate) | 0.01 (static) | ~92 ns/op | dict + list FIFO (best by min/max scan) |

In the *Worst-case M/s* column, **crash** marks an engine that aborts in its
weakest scenario (on seed 23; a note flags an engine that can hang);
a number is the lowest of the engine's five scenario throughputs. 

These findings are offered back, not aimed at anyone — each is a reproducible,
time-stamped *snapshot* of a specific commit, not a verdict on a project's
quality, and several integrated engines ship with a fix the reference adapter
applies (`CORRECTNESS_FINDINGS.md` carries the one-line finding and filed-issue link for each). 
