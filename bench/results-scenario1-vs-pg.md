# The backtest workload across three row-set sizes — ckdbs against PostgreSQL

Every read shape, every sweep and every matrix in `tools/scenario1_backtest.py`,
measured at **252, 1,008 and 10,080 bar rows** against the PostgreSQL twin at
the same three sizes, alternating inside each size so both engines see the
same machine. How to run either driver is in
[`bench/docs/README.md`](docs/README.md); this file states what the run found.

**Thesis: ckdbs's fixed per-statement cost is roughly half PostgreSQL's on
every read shape, and which engine wins is then decided by the per-row cost —
which splits the shapes into two classes with opposite answers.** On a simple
fold over a whole relation PostgreSQL's per-row cost is 15–35% lower than
ckdbs's, so the two cross over between **1,300 and 3,000 rows** — inside the
range this run covers, and the reason a measurement at one cardinality would
have reported either engine as the winner. On the join, grouped and wide-result
shapes ckdbs's per-row cost is **3.5–4.7× lower** and there is no crossover at
any size.

Two findings sit beside that and neither is about a shape.

**A Cabin converts a per-row cost into a fixed one here, and beats
PostgreSQL's index doing it.** At 10,080 bars a day-slice goes 580.2 → **37.9
µs with a Cabin declared, 15.3×**, against PostgreSQL's index at 68.7 µs; a
cross-join goes 594.9 → **48.5 µs, 12.3×**, against PostgreSQL's 155.4.
Dropping the Cabin returns both to their cold cost, which is what proves the
Cabin was doing the work. **This is the opposite of what the same structure
does in `bench/results-scenario3-library.md`**, and §7 explains why the two
results are consistent.

**ckdbs does not scale with connections on a read workload, and PostgreSQL
does.** From 1 to 8 connections ckdbs moves 1,655 → 1,707 aggregate QPS —
flat — while PostgreSQL moves 1,610 → 2,911 and holds it. Beyond one
connection PostgreSQL is **1.7× ahead**, and the mechanism is the
single-threaded dispatcher: this workload has no fsync for concurrency to
amortise, so nothing here overlaps.

## 1. The run

| | |
|---|---|
| executed | **2026-08-18 05:45:25 → 05:52:37 UTC**, 10 cells — 5 ckdbs, 5 PostgreSQL, alternating |
| branch / worktree | `worktree-bench-scenario2-postgres`, in the worktree `bench-scenario2-postgres` |
| commit measured | **`1cbba76`**, recorded by every ckdbs cell, `dirty: false` in all of them |
| **binary measured** | a **copy**, `sha256 7312b75f095e8d64…`, taken before the first cell and never rewritten — the build tree is shared with other sessions. It was linked at `b1bbec0`; every commit between that and `1cbba76` touches `bench/`, `docs/` or `README.md` only (`git diff b1bbec0 1cbba76 -- src include tests` is **empty**), so the binary is the engine at `1cbba76` |
| build | Release (`-O3 -DNDEBUG`), gcc 13.3.0, `KDS_WITH_TLS=ON` (OpenSSL 3.0.13) |
| PostgreSQL | **16.14**, extracted rootless into `$HOME/pg16` (`bench/docs/README.md` carries the recipe), port 15433, **PostgreSQL's own defaults** |
| device | ext4 on `/dev/root`; ckdbs data files under `$HOME/bench-s1/db/`, WAL under `$HOME/bench-s1/wal/<cell>/`, PostgreSQL under `$HOME/pg-bench/data`. **Not tmpfs** |
| kernel / host | 6.17.0-1022-azure, Ubuntu 24.04, AMD EPYC 9V74, **2 vCPUs** |
| server config | `cores = 1`, `durability = group`, `indexes = on`. One server process and one **fresh data file** per cell; one **fresh database** per PostgreSQL cell |
| contention control | every cell gates on `bench/wait_quiet.sh`, and both runners sample `pgrep -c cc1plus` before and after, discarding the cell if a compiler ran. **No cell was discarded** in this run |
| correctness | `--verify` on both engines — every model's P&L read back through the comparison join and checked against the driver's running total. **`verify_problems` empty and 0 errors in all 10 cells**, over 161,632 phase operations — the QPS, write and connection sweeps of §7–§9 are counted separately by their own drivers |

## 2. The ladder

Bars = `--years × 252 × --symbols`. Holding `--symbols` at 1 and moving
`--years` keeps every result set the same size while the relations grow —
one variable, which is what makes §6's two-parameter fit meaningful.

| `--years` | bars | sessions | rows loaded | rebalance periods |
|---:|---:|---:|---:|---:|
| 1 | 252 | 252 | 765 | 11 |
| 4 | 1,008 | 1,008 | 3,033 | 47 |
| 40 | 10,080 | 10,080 | 30,249 | 479 |

`daily_stats` gets one feature row per bar and `sessions` one row per trading
day, so a bar count sets seven relations at once. 49 columns are spent per
run, which is why each cell gets its own data file rather than a suffix in a
shared one.

## 3. The noise floor, and the two classes it splits into

One repeat of each end of the ladder, per engine, against a fresh data file
or database. Dividing by the smaller of each pair, which is the conservative
direction:

| replicate pair | bars | engine | read shapes: max Δ p50 | median | durable-write phases: max Δ p50 |
|---|---:|---|---:|---:|---:|
| `ck-y1` vs `ck-y1-rep` | 252 | ckdbs | 15.0% | 1.8% | 21.8% |
| `pg-y1` vs `pg-y1-rep` | 252 | postgresql | 11.3% | 3.6% | **144.1%** |
| `ck-y40` vs `ck-y40-rep` | 10,080 | ckdbs | 13.2% | 2.1% | 10.0% |
| `pg-y40` vs `pg-y40-rep` | 10,080 | postgresql | 17.6% | 1.0% | **83.6%** |

**The read shapes are tight and the durable-write phases are not.** Every
read shape in the matrix repeats to within 17.6% and the median repeat is
1–3.6%; the load and `result-insert` phases — one fsync per statement —
disagree between two runs of the same configuration by up to 144%, on
PostgreSQL more than on ckdbs. **So the floor adopted below is ±17.6% for a
read shape, and no claim rests on a single load-phase number at all.** §8
prices the write side through the batch sweep instead, where the fsync is
amortised and the numbers are stable.

## 4. Every shape, at three sizes

p50 µs. The eight `read-*`/`agg-*` rows are the read matrix; `backtest-*`,
`compare-*` and `result-insert` are the workload's own phases.

| phase | ck 252 | pg 252 | ck 1,008 | pg 1,008 | ck 10,080 | pg 10,080 |
|---|---:|---:|---:|---:|---:|---:|
| read-bar-lookup | **38.5** | 72.0 | **32.2** | 66.5 | **37.0** | 62.4 |
| read-bar-range | **116.7** | 708.6 | **127.3** | 713.2 | **125.8** | 735.1 |
| read-symbol-history | **143.8** | 1,041.5 | **537.6** | 3,882.9 | **8,599.2** | 41,328.6 |
| read-day-slice | **49.7** | 71.2 | **94.0** | 103.6 | 588.2 | **509.9** |
| read-join-point | **44.4** | 122.9 | **44.6** | 118.7 | **43.9** | 122.7 |
| read-join-exists | **37.3** | 91.8 | **40.2** | 89.5 | **39.2** | 92.3 |
| agg-global | **64.9** | 91.9 | **133.6** | 148.1 | 944.9 | **857.8** |
| agg-by-symbol | **70.5** | 111.4 | **159.8** | 206.8 | **1,278.0** | 1,372.9 |
| agg-by-session | **142.5** | 561.3 | **489.6** | 1,834.8 | **5,136.7** | 18,258.0 |
| agg-day-slice | **52.6** | 86.3 | **98.9** | 120.3 | 601.4 | **518.3** |
| agg-distinct | **60.1** | 85.5 | **130.5** | 142.7 | 1,072.0 | **839.3** |
| backtest-read | **91.1** | 212.9 | **131.0** | 235.5 | **627.9** | 659.3 |
| backtest-replay | **71.5** | 170.5 | **117.6** | 188.6 | 599.7 | **591.5** |
| compare-all | **118.4** | 497.2 | **333.5** | 1,242.6 | **2,544.8** | 12,250.0 |
| compare-one | **53.4** | 118.6 | **89.1** | 202.1 | **427.3** | 1,281.9 |
| result-insert | 1,105.9 | 1,052.9 | 997.9 | 1,011.5 | 972.0 | 1,040.8 |

**ckdbs wins every shape at 252 and 1,008 rows.** At 10,080 four have flipped
— `read-day-slice`, `agg-global`, `agg-day-slice`, `agg-distinct` — by 10–28%,
and `backtest-replay` by 1.4%, which is inside the floor. Every flipped shape
is a fold or slice over the whole relation. None of the join, range or
grouped shapes flips, and two of them end up far in ckdbs's
favour at the top of the ladder: `read-symbol-history` at **4.8×** and
`agg-by-session` at **3.6×**.

`result-insert` is the same on both engines at every size, within the floor,
because it is one fsync per row on both — the same result
`bench/results-scenario2-freight.md` reaches for its commit and
`bench/results-scenario3-library.md` for its load.

## 5. The primary-key lookup does not scale, and that is the control

`read-bar-lookup` is a pk equality — one btree descent. Across a **40×**
growth in the relation it reads 38.5, 32.2, 37.0 µs on ckdbs and 72.0, 66.5,
62.4 on PostgreSQL: flat on both, within the floor on both. `read-join-point`
(44.4 / 44.6 / 43.9) and `read-join-exists` (37.3 / 40.2 / 39.2) are flatter
still.

That is the control this ladder needs. A shape that does not move with the
row count says the harness is measuring the row count where it should and not
somewhere else — and it prices the fixed cost directly: **a ckdbs statement
that touches one row costs ~37 µs of client-measured round trip against
PostgreSQL's ~62 µs.**

## 6. Fixed cost, per-row cost, and where they cross

Fitting `p50 = fixed + per-row × bars` over the three sizes, per shape and per
engine. The crossover column is the row count at which the two engines' lines
meet — where it falls inside or near the ladder, the shape has no
size-independent winner:

| shape | ckdbs fixed µs | ckdbs µs/row | PG fixed µs | PG µs/row | crossover |
|---|---:|---:|---:|---:|---:|
| read-bar-lookup | **35.4** | 0.0001 | 69.8 | −0.0008 | — (both flat) |
| read-join-point | **44.5** | −0.0001 | 120.8 | 0.0002 | — (both flat) |
| read-join-exists | **38.7** | 0.0001 | 90.6 | 0.0002 | — (both flat) |
| read-bar-range | **121.5** | 0.0005 | 709.2 | 0.0026 | never |
| agg-distinct | **30.4** | 0.1033 | 65.8 | **0.0767** | **1,332 rows** |
| read-day-slice | **37.4** | 0.0547 | 59.3 | **0.0447** | **2,200 rows** |
| agg-global | **42.8** | 0.0895 | 70.9 | **0.0781** | **2,451 rows** |
| agg-day-slice | **40.6** | 0.0557 | 75.6 | **0.0439** | **2,984 rows** |
| backtest-read | **76.6** | 0.0547 | 195.5 | **0.0460** | 13,632 rows |
| agg-by-symbol | **37.7** | **0.1230** | 78.2 | 0.1284 | never |
| compare-one | **47.3** | **0.0377** | 85.7 | 0.1186 | never |
| compare-all | **70.7** | **0.2456** | 115.2 | 1.2032 | never |
| agg-by-session | −4.4 | **0.5099** | 62.9 | 1.8047 | never |
| read-symbol-history | −203.5 | **0.8722** | −122.5 | 4.1112 | never |

*(the two negative intercepts are shapes whose result set grows with the
relation, so their cost is not linear in bars and the fit's intercept has no
physical reading; their per-row columns still compare)*

**ckdbs's fixed cost is lower on every shape without exception** — 35–45 µs
against 60–121 µs on the point shapes, and 121 against 709 µs on the range.
That is the engine's structural advantage on this workload and it is why it
wins the whole table at 252 and 1,008 rows.

**The per-row cost divides the shapes into two classes.** Where the work is a
simple fold or slice over the relation, PostgreSQL's per-row is 15–35% lower
and the crossover lands between **1,332 and 2,984 rows** — four shapes, all
inside the ladder, which is precisely why one cardinality could not have
answered this. Where the work is a join, a grouped fold or a wide result,
ckdbs's per-row is **2.6× to 4.7× lower** (`compare-one` 0.038 against 0.119,
`compare-all` 0.246 against 1.203, `agg-by-session` 0.510 against 1.805,
`read-symbol-history` 0.872 against 4.111) and no crossover exists at any
size this workload can reach.

## 7. The Cabin converts the cost here, and it did not in scenario3

The QPS matrix runs each shape cold, warm, with an accelerator declared, and
after dropping it. `--warm-keys 8` cycles eight distinct arguments, so a
declared Cabin sees each value again. At **10,080 bars**, p50 µs:

| shape | ck cold | ck warm | **ck cabin** | ck dropped | pg cold | pg warm | **pg index** | pg dropped |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| bar-lookup | 37.7 | 37.1 | — | — | 69.5 | 68.3 | — | — |
| bar-range | 125.0 | 121.1 | — | — | 753.7 | 720.8 | — | — |
| day-slice | 580.2 | 579.1 | **37.9** | 593.1 | 505.3 | 499.7 | **68.7** | 491.3 |
| symbol-history | 8,644.5 | 6,706.8 | 7,505.1 | 7,203.6 | 54,929.2 | 40,375.3 | 40,061.8 | 40,516.1 |
| cross-join | 594.9 | 596.0 | **48.5** | 597.3 | 596.3 | 592.4 | **155.4** | 593.0 |
| point-join | 44.4 | 44.2 | — | — | 123.2 | 127.7 | — | — |
| model-join | 435.0 | 422.6 | **299.5** | 424.6 | 1,148.3 | 1,193.8 | 1,123.4 | 1,255.0 |

*(a dash is a shape whose column is a pk — a Cabin on a pk column is refused)*

**The Cabin is worth 15.3× on the day slice and 12.3× on the cross-join**, and
in both cases it beats PostgreSQL's btree index on the same column — 37.9 µs
against 68.7, and 48.5 against 155.4. `dropped` returns both to within 3% of
cold, which is the control: the Cabin was the whole difference.

At **252 bars** the same Cabins are worth 1.34× and 1.25× (50.3 → 37.5, 61.7 →
49.3), because the walk they replace is already cheap. The Cabin's benefit
therefore tracks the relation, which is the per-row-to-fixed conversion in the
same form §6 measures for the index-less shapes.

**Why this does not contradict `bench/results-scenario3-library.md`**, where
the same structure was 1.8× *worse* than no accelerator at all. A Cabin is
authoritative only for values a query has already observed, so its benefit is
a function of how often a probe's argument repeats. Here `--warm-keys 8`
cycles eight arguments through hundreds of operations and the hit rate is
effectively total. There, `--matches 5` holds matches-per-key constant while
the relation grows, so the key space grows with it — 2,000 distinct users at
10,000 loans — and the hit rate collapses. **The two results are the same
mechanism read at opposite ends of one variable, and neither is a property of
the Cabin alone.** What decides it is the workload's argument distribution,
which is the input the `CABIN AUTO` threshold in `docs/feat-cabin.md` §11 is
still open on.

`symbol-history` is the counter-case on both engines: its result set grows
with the relation, so neither the Cabin (7,505 against 8,645 cold) nor
PostgreSQL's index (40,062 against 54,929) can make it fixed-cost. An
accelerator removes the cost of *finding* rows, never of *having* them.

## 8. The write side: identical at batch 1, 1.73× apart at batch 1,000

Rows per second inserted into a relation of the sweep's own, at 10,080 bars:

| rows per `BEGIN`/`COMMIT` | ckdbs | PostgreSQL | ratio |
|---:|---:|---:|---:|
| 1 | 861.4 | 864.6 | **1.00×** |
| 10 | 6,677.8 | 5,525.0 | 1.21× |
| 100 | 21,104.1 | 14,342.5 | 1.47× |
| 1,000 | 31,056.6 | 17,910.1 | **1.73×** |

**At one row per transaction the two engines are indistinguishable** — 0.4%
apart — because both are paying one fsync per row to the same filesystem and
nothing else is visible behind it. That is the same equality
`bench/results-scenario2-freight.md` finds in its commit row and
`bench/results-scenario3-library.md` finds in its load, now measured a third
way.

**Every batch size above 1 is where the engines differ**, and the gap widens
with the batch: once the fsync is amortised, what remains is per-row pipeline
cost, and ckdbs's is lower. A 1,000-row batch is 36× ckdbs's own unbatched
rate and 21× PostgreSQL's — on both engines the batch size, not the engine, is
the first-order decision.

## 9. Concurrency: ckdbs is flat and PostgreSQL is not

Aggregate QPS of the cross-section join by connection count, at 10,080 bars:

| connections | ckdbs | PostgreSQL | ratio |
|---:|---:|---:|---:|
| 1 | 1,654.9 | 1,610.4 | 1.03× |
| 2 | 1,736.9 | 2,911.3 | **0.60×** |
| 4 | 1,697.0 | 2,882.8 | **0.59×** |
| 8 | 1,706.7 | 2,864.9 | **0.60×** |

**ckdbs gains 3% going from one connection to eight; PostgreSQL gains 81%
going from one to two and then holds.** This is the clearest engine-level
result in the file and it is not favourable: the server dispatches every
client on one thread, so a workload with no durability point to batch has
nothing to overlap. PostgreSQL's gain saturating at two is the two vCPUs;
its plateau at 2,865–2,911 is the box, not the engine.

The contrast with `bench/results-scenario2-freight.md` is the mechanism in
one line. There, concurrency buys ckdbs 1.35× — because that workload commits,
and `durability = group` batches concurrent commits into one flush. Here there
is nothing to batch, and the dispatcher's single thread is the whole answer.
**Cross-core execution (`docs/crosscore.md`, P4d/P4e) is the work that would
change this row**, and until a peer-owned relation has a writer it cannot be
measured on a workload that writes.

## 10. What this run does not answer

- **Whether the four crossovers move with hardware.** §6 puts them between
  1,332 and 2,984 rows on two vCPUs of an EPYC 9V74. The crossover is a ratio
  of per-row costs, and nothing here says how it behaves on a machine with
  more memory bandwidth or more cores.
- **Why ckdbs's per-row fold cost is higher.** The four flipped shapes are all
  simple folds; the engine exposes no per-step timing that would separate the
  walk from the fold from the render. `docs/observability.md` owns that and it
  is unbuilt. `docs/workplan-aggregate-perf.md` AP05 is the open work.
- **What a secondary index does to these shapes.** This run declares Cabins,
  not indexes — `--index-mode` is scenario3's axis, not this driver's. The
  comparison of a Cabin against an index on the same column is
  `bench/results-scenario3-library.md` §7's, at a different hit rate.
- **Anything about a durable write at scale.** §8's batch-1 row is one fsync
  per statement and §3 shows those phases repeating to ±144%. The batched
  rows are stable and are what the section rests on.
- **Whether the connection result survives cross-core.** §9 measures the
  single-threaded dispatcher as it is today. P4d's pipeline exists but has no
  writer for a peer-owned relation, so a scaling claim cannot be made from
  either side of it yet.
