# What the MVCC read path costs — business stress, 2026-08-04

**There is no transaction feature to switch on yet.** `docs/txn-workplan.md`
T07-T10 are unbuilt: `Dispatch()` is stateless, there is no
`BEGIN`/`COMMIT`, no session, and every row is still stamped
`kBootstrapXid`. This measures the one thing that *did* land on an
executing path — the visibility predicate now sitting in
`ChainRunner::AcceptTupleAt` — against `HEAD` without it.

The delta under test is exactly two things:

- one `txn::Classify(view, tuple)` per **examined tuple**, before the decode
- one `txn::Snapshot` copied per `ChainRunner` construction

Nothing else. The dispatcher passes no snapshot, so `Classify` always takes
its first arm (`view.Visible(trx_id)` is true for every writer under
`ReadView::Everything()`), the undo walk is never entered, and the R1
scratch copy of workplan amendment A2 is never taken. **This is the floor
cost, not the cost of MVCC** — the walk and the copy are only paid by a
reader that cannot see a tuple's writer, which no statement can produce
until T09 stamps a real `trx_id`.

## Setup

```
tools/stress_business.py --users 10000 --assets 10000 --seconds 120 \
                         --traders 4 --seed 1
```

2 cores, ext4 on nvme (**not** tmpfs — the tool refuses to be believed on
tmpfs, where fsync is free). Release builds both sides. `durability=group`,
`cabins` off. One server alive at a time; the other arm's server was
stopped so its checkpointer could not perturb the run. Zero `ERR` replies
in all four runs. Baseline is the worktree at `9272034`; current is that
plus `T01`-`T06`, `T11`.

**n = 2 per arm.** Enough to reject the first pair's apparent +1.5%, not
enough to resolve 1% (see "What this cannot say").

## Result

| phase | metric | base (n=2) | current (n=2) | Δ |
|---|---|---|---|---|
| **txn** | **TPS** | **367.9** | **367.6** | **−0.07%** |
| txn | mean µs | 10850.8 | 10857.1 | +0.06% |
| txn | p50 µs | 9420.9 | 9598.2 | +1.88% |
| trade-insert | mean µs | 3234.9 | 3235.9 | +0.03% |
| trade-insert | p50 µs | 2924.4 | 2948.8 | +0.83% |
| account-update | mean µs | 2176.8 | 2178.8 | +0.09% |
| account-update | p50 µs | 1946.8 | 1967.4 | +1.06% |
| **profit-scan** | **mean µs** | **12314.8** | **12476.8** | **+1.32%** |
| profit-scan | p50 µs | 12067.9 | 12443.2 | +3.11% |
| profit-insert | mean µs | 5601.1 | 5548.6 | −0.94% |

Per-run, for the phase that matters:

| profit-scan | run 1 | run 2 |
|---|---|---|
| base mean µs | 12217.4 | 12412.3 |
| current mean µs | 12395.9 | 12557.8 |

## Reading it

**TPS does not move: 367.9 → 367.6.** The business transaction is two
logged INSERTs and two UPDATEs, and none of them goes through
`AcceptTupleAt` — an INSERT does not read, and `HandleUpdate` walks with its
own `apply` lambda rather than compiling to a chain. The write path is
fsync-bound anyway. There was no mechanism for this number to move and it
did not.

**`profit-scan` is the only phase that can move, and it moves ~1-2%.** It is
a `FilterScan`: `WHERE user_id = <n>` on a non-pk column, so it walks all
20,012 accounts per query and pays one `Classify` per row. +1.32% on the
mean, +3.11% on p50.

**But ~1% of that is drift, not code.** `txn` p50 (+1.88%), `trade-insert`
p50 (+0.83%) and `account-update` p50 (+1.06%) also rose, and the change
cannot reach any of them. Something systematic separates the two arms'
runs — they were taken in sequence, so page-cache and file-layout state
differ. Subtracting it, the attributable cost of the predicate on a
full-relation scan is roughly **1-2%**, consistent with one predictable
branch per row against a decode.

## What this cannot say

- **n = 2.** The first pair alone showed +1.5% on `profit-scan` mean and
  looked like signal; the second baseline sample then landed at 12412.3,
  *above* the current arm's 12395.9. One sample per arm would have recorded
  a number that the next run contradicts. Treat 1-2% as an order of
  magnitude, not a measurement.
- **Nothing here exercises MVCC.** No undo chain is walked, no version is
  reconstructed, no read view excludes anything. When T09 lands, the cost
  that matters is the A2 scratch copy plus the chain walk on rows whose
  writer is invisible — and that is a function of update churn, which this
  scenario does not create against a reader.
- **2 cores for 4 traders + 1 reporter.** The scenario is CPU-oversubscribed
  by design on this box, which compresses differences.

## Two things found while running this, unrelated to the code

**19 orphaned `kds_server` processes** from earlier sessions were burning
27.4% CPU between them, crash-looping on `ERROR [wal] segment 1 does not
exist`. The first "clean" run was taken under that load and is discarded.

**`tools/kdb.log` had reached 3.7 GB** — the crash-loop output of those same
servers — and filled the root filesystem to 100%. The run that followed
failed 115,732 statements with `FileLogDevice: posix_fallocate ... No space
left on device`, which is not a result about anything. Deleted; it was
gitignored and untracked.

Both are worth knowing before trusting any earlier number taken on this
box, including `bench/results/business-stress-4t.txt` (167 TPS), which was
recorded with those processes running.
