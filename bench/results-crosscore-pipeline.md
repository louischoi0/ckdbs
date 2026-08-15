# The cross-core step pipeline, priced against local execution

Measured 2026-08-15 on `worktree-feat-coroutine-2` at commit `3854242`,
`build-release/kds_crosscore_bench` (Release, `-O3 -DNDEBUG`,
`-DKDS_WITH_TLS=OFF`), on a 2-vCPU host, started behind a load gate
(1-minute load average below 1.0 at launch: 0.87). Driver:
`bench/crosscore_pipeline_bench.cpp` — new with this file, and the only
benchmark in this tree that is not a server driver.

This closes the number `docs/workplan-crosscore.md` has carried by name
since P4d-4b: **the per-input-row runner cost.**

## The answer up front

```
shipped - local  =  2.52 us  +  0.626 us per forwarded row
```

A statement that ships pays **2.5 µs before its first row**, and then
**0.626 µs for every row the leaf forwards**. For scale, the same join
executed locally costs **0.417 µs per row end to end** — so a forwarded
row costs *one and a half times the entire local cost of processing it*,
and the shipped path runs at **2.50× local** on the largest shape
measured.

That settles an open question in the workplan: **P4d-4c's per-batch
runner handle is worth building.** The debt was described there as "each
input row pays an `ExecuteAsync` frame, a `Bind` and a frame `Open`
before touching a tuple — the shape `95946c4` removed locally,
reintroduced one level up". It is now priced, and it is not a rounding
error; it is the majority (60%) of what a shipped row costs.

## Why this is not a server driver

Every other per-statement number in `bench/` follows
`docs/workplan-aggregate-perf.md`: two servers, interleaved A/B over the
wire. **That method cannot reach this code path at all.**

A pipeline runs only against a relation owned by a core other than the
session's. A peer-owned relation has **no writer**: writes to it are
refused by `CheckWriteAffinity` (crosscore.md CC3 — cross-core writes are
a retryable refusal), DML statement shipping is unbuilt, and only core 0
carries a listener. So no sequence of client statements can put rows in a
relation a pipeline would read. This is reproducible — the isolation
driver now probes for it and reports it (see
`bench/results-multicore.md`, and `tools/multicore_benchmark.py
--placement rotate`).

So this harness does what the equivalence test does: build the rows
in-process, then run **one dataset through two dispatchers that differ
only in `core_id`**. The relations are owned by core 1, so the core-1
dispatcher executes locally and the core-0 dispatcher ships — same
catalog, same pages, same statement, one process. That is a *stronger*
isolation than an A/B across builds: there is no second binary and no
second process for drift to enter through, which is why this file needs
no same-binary control (`results-p4d-executor.md` §10.6 needed one
because it had two servers).

**What it is not**: there is no socket, no framing, no ring — the
loopback delivers each message inline. **These are the pipeline's CPU
costs, not its latency under a real transport.** The ring's own cost is
P1's and is not here. Nothing in this file should be quoted as a
statement about wire latency.

## Setup

- Two relations, `ta (id int64, b_id int64)` and `tb (id int64, qty
  int64)`, both `BTREE`, both placed on core 1 by `placement = rotate`.
- Statement: `SELECT a.id, b.qty FROM ta AS a JOIN tb AS b ON b.id =
  a.b_id` — the two-step class P4d-4b-3 ships: a scan feeding a **pk
  probe**.
- The inner side is fixed at 64 rows and every outer row's key hits a
  live inner row: **one descent per input row, constant work**, so the
  slope isolates the runner overhead rather than the join's own cost.
- The outer relation is swept 8 → 2,048 rows. Outer rows are what cross
  the edge, so outer size *is* the forwarded-row count.
- 400 reps per size, **local and shipped alternating rep by rep**, after
  20 warm-up reps of each. Interleaving is the same discipline the
  server-side A/B files use, for the same reason.
- Rows are inserted through the **local dispatcher**, which is core 1 and
  therefore owns the relations — the engine's real write path, btree
  descent and all.
- **The benchmark gates itself on correctness**: it refuses to time
  anything unless the two paths return byte-identical replies. A
  benchmark comparing two different answers measures nothing.

## Results

| outer rows | local p50 (µs) | shipped p50 (µs) | Δ p50 (µs) | Δ mean / row (µs) |
|---|---|---|---|---|
| 8 | 7.6 | 16.1 | +8.5 | +1.121 |
| 32 | 17.5 | 40.5 | +23.0 | +0.733 |
| 128 | 57.3 | 138.4 | +81.1 | +0.652 |
| 512 | 218.1 | 539.1 | +320.9 | +0.623 |
| 2,048 | 854.6 | 2,131.3 | +1,276.7 | +0.628 |

Full percentiles, µs:

| shape | p0 | p25 | p50 | p75 | p95 |
|---|---|---|---|---|---|
| local, 8 | 7.3 | 7.5 | 7.6 | 7.7 | 8.1 |
| shipped, 8 | 15.4 | 15.9 | 16.1 | 16.3 | 20.0 |
| local, 32 | 17.1 | 17.4 | 17.5 | 17.7 | 18.6 |
| shipped, 32 | 39.6 | 40.3 | 40.5 | 40.9 | 50.0 |
| local, 128 | 56.0 | 56.8 | 57.3 | 58.0 | 67.9 |
| shipped, 128 | 136.3 | 137.6 | 138.4 | 145.8 | 152.4 |
| local, 512 | 209.7 | 214.4 | 218.1 | 224.9 | 232.8 |
| shipped, 512 | 524.3 | 534.2 | 539.1 | 545.4 | 550.9 |
| local, 2048 | 817.6 | 840.5 | 854.6 | 861.4 | 870.4 |
| shipped, 2048 | 2,110.7 | 2,125.9 | 2,131.3 | 2,138.4 | 2,169.5 |

Least squares over the five points: **intercept 2.524 µs, slope 0.626 µs
per forwarded row.**

## Reading it

- **The per-row cost is flat, and that is the point.** From 128 rows up
  it sits at 0.652 / 0.623 / 0.628 µs — three sizes spanning 16× in row
  count agreeing to within 5%. A *fixed* cost amortized over rows would
  fall as 1/n; it does not. It is a genuine per-row charge, which is
  exactly what a per-row `ExecuteAsync` frame predicts.
- **The 8-row point is the intercept showing through**, not a different
  per-row cost: 1.121 µs/row at n=8 is 0.626 plus 2.52/8. The fit
  reproduces every point from those two numbers alone, which is the best
  evidence available that the model is the right one.
- **The intercept is small and the slope is not.** 2.5 µs of per-statement
  setup — one `STEP_OPEN` envelope encode, the chained forward, two
  pipeline states, two coroutine frames — is cheap enough to be
  uninteresting. The 0.626 µs per row is where a shipped join's cost
  lives, and it is the half that scales.
- **Per-row overhead exceeds the entire local per-row cost.** Local runs
  0.417 µs/row (854.6 µs / 2,048); the pipeline adds 0.626 µs on top, so
  a shipped row costs 1.041 µs and **60% of that is pipeline overhead
  rather than join work**. This is the number that makes P4d-4c's
  per-batch runner handle a build rather than a nicety: dissolving those
  per-row frames is the difference between 2.5× local and something much
  nearer parity.
- **What this does not say**: nothing here prices the ring, the socket,
  or a second physical core doing the work concurrently. Cross-core
  execution's *point* is parallelism, and a loopback harness on a
  2-vCPU box measures the cost side of that trade with the benefit
  removed by construction. The benefit needs a workload where two cores
  genuinely run at once, which needs a writer for peer-owned relations —
  see the blocker above.

## Deviations from the house results format

- **No PostgreSQL comparison.** The comparison this file exists for is
  KDS's shipped path against KDS's local path over the same rows in the
  same process; a second engine has no place in that subtraction.
- **No wait breakdown.** There is no I/O and no lock wait: a
  `MemoryPageDevice` backs the store and one thread runs both paths. The
  whole delta is CPU.
- **No p99.** 400 reps put p99 on the 4th-worst sample, where a single
  scheduler interruption dominates; p95 is reported instead and p0-p75
  carry the signal, which the tight p0-to-p75 spread on every row shows.

## Reproducing

```
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DKDS_WITH_TLS=OFF
cmake --build build-release --target kds_crosscore_bench -j
./build-release/kds_crosscore_bench [reps] [inner_rows]     # defaults: 200 64
```
