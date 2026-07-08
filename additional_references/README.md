# Additional references

40 worked adapter examples that wrap third-party matching engines behind
`api/matching_engine_api.h` — the permissively-licensed subset of the ~134 adapters
built across the 20+-language survey (the rest are built and measured but held
data-only). Their purpose is
to show how an engine with a very different shape from the harness ABI can
be made to drive the harness — and, by extension, to make the observations
in `CORRECTNESS_FINDINGS.md` reproducible from running code rather than asserted
from text.

**The additional references are not the reference engines.** The published
correctness reference comes from the three engines under `scripts/build_baselines.sh`
(Liquibook, QuantCup, Exchange-core), and is reproduced across the conforming field. The adapters here are
point-in-time **snapshots** — each `build.sh` clones the engine's upstream
at a pinned commit and patches it where needed. Every adapter's upstream repo
and pinned commit is listed per engine in [`../SNAPSHOTS.md`](../SNAPSHOTS.md) —
the single source of truth, one row per engine, kept in step with each adapter's
`build.sh`.

The adapters are not maintained: if an upstream moves past its pinned commit
the adapter may not build against the newer source, and any observation in
`CORRECTNESS_FINDINGS.md` describes the pinned snapshot and only that snapshot. A
project's current `main` may already differ.

Most subfolders have a short README that documents the engine's native API
shape and how the adapter maps it to the harness ABI. Correctness findings —
bug observations and divergence mechanisms — live in
[`../CORRECTNESS_FINDINGS.md`](../CORRECTNESS_FINDINGS.md) (open findings) and
[`../RESOLVED_FINDINGS.md`](../RESOLVED_FINDINGS.md) (findings fixed upstream) so they have one
place to read, update, and (if a project's behavior changes upstream) retire.

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
