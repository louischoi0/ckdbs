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
PostgreSQL's index doing it.** At 10,080 bars a day-slice goes from 1,724 to
**27,181 statements a second with a Cabin declared, 15.8×**, against
PostgreSQL's indexed 12,604/s; a cross-join goes from 1,670 to **19,982/s,
12.0×**, against PostgreSQL's 6,179. Dropping the Cabin returns both to their
cold rate, which is what proves the Cabin was doing the work. **This is the
opposite of what the same structure does in
`bench/results-scenario3-library.md`**, and §7 explains why the two results
are consistent.

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
or database, on the same throughput basis every matrix below uses. Dividing
by the smaller of each pair, which is the conservative direction:

| replicate pair | bars | engine | read shapes: max Δ QPS | median | durable-write phases: max Δ QPS |
|---|---:|---|---:|---:|---:|
| `ck-y1` vs `ck-y1-rep` | 252 | ckdbs | 18.9% | 3.7% | 55.0% |
| `pg-y1` vs `pg-y1-rep` | 252 | postgresql | **57.3%** | 3.4% | **144.1%** |
| `ck-y40` vs `ck-y40-rep` | 10,080 | ckdbs | 13.5% | 2.7% | 12.2% |
| `pg-y40` vs `pg-y40-rep` | 10,080 | postgresql | 20.5% | 3.7% | **132.7%** |

**The read shapes are tight in the middle and the durable-write phases are
not.** The median read shape repeats to within 2.7–3.7% on both engines; the
load and `result-insert` phases — one fsync per statement — disagree between
two runs of the same configuration by up to 144%. **So the floor adopted
below is ±20.5% for a read shape at 10,080 bars, ±57.3% at 252, and no claim
rests on a single load-phase number at all.** §8 prices the write side
through the batch sweep instead, where the fsync is amortised and the numbers
are stable.

PostgreSQL's 57.3% at 252 bars is one shape — a small-relation read whose
absolute rate is high enough that a few microseconds of scheduling move it a
long way — and it is why nothing at the bottom of the ladder is claimed on
the PostgreSQL side without a factor behind it.

## 4. Every shape, at three sizes

**Statements per second**, which is what the driver's own `ops / elapsed`
reports. The eight `read-*`/`agg-*` rows are the read matrix; `backtest-*`,
`compare-*` and `result-insert` are the workload's own phases. Higher is
better in every cell.

| phase | ck 252 | pg 252 | ck 1,008 | pg 1,008 | ck 10,080 | pg 10,080 |
|---|---:|---:|---:|---:|---:|---:|
| read-bar-lookup | **25,126** | 13,831 | **28,409** | 14,599 | **26,247** | 14,881 |
| read-bar-range | **8,418** | 1,392 | **7,746** | 1,389 | **7,874** | 1,304 |
| read-symbol-history | **6,614** | 903 | **1,826** | 252 | **101** | 24 |
| read-day-slice | **19,881** | 13,680 | **10,438** | 9,524 | 1,692 | **1,867** |
| read-join-point | **21,459** | 7,862 | **21,459** | 8,130 | **22,422** | 7,782 |
| read-join-exists | **25,907** | 10,627 | **23,419** | 10,846 | **24,390** | 10,341 |
| agg-global | **14,556** | 8,795 | **7,097** | 5,914 | **1,055** | 1,018 |
| agg-by-symbol | **13,624** | 8,726 | **6,188** | 4,773 | **788** | 699 |
| agg-by-session | **6,309** | 1,761 | **1,834** | 502 | **184** | 53 |
| agg-day-slice | **19,417** | 11,338 | **9,524** | 7,680 | 1,639 | **1,916** |
| agg-distinct | **16,502** | 11,186 | **7,524** | 6,667 | 921 | **1,160** |
| backtest-read | **10,582** | 4,110 | **7,364** | 4,137 | **1,582** | 1,465 |
| backtest-replay | **12,987** | 5,647 | **8,084** | 5,198 | **1,647** | 1,621 |
| compare-all | **8,446** | 2,011 | **2,999** | 805 | **393** | 82 |
| compare-one | **17,331** | 7,981 | **10,811** | 4,760 | **2,337** | 771 |
| result-insert | 804 | 736 | 918 | 937 | 961 | 789 |

**ckdbs wins every shape at 252 and 1,008 rows.** At 10,080 three have
flipped — `read-day-slice`, `agg-day-slice`, `agg-distinct` — by 10–26%, and
`backtest-replay` by 1.6%, which is inside the floor. Every flipped shape is
a fold or slice over the whole relation. None of the join, range or grouped
shapes flips, and two of them end up far in ckdbs's favour at the top of the
ladder: `read-symbol-history` at **4.2×** and `agg-by-session` at **3.5×**.
`agg-global` and `agg-by-symbol` are the two the crossover has almost reached
— 1,055 against 1,018 and 788 against 699 — both inside the floor and neither
claimed as a win.

`result-insert` is the same on both engines at every size, within the floor,
because it is one fsync per row on both — the same result
`bench/results-scenario2-freight.md` reaches for its commit and
`bench/results-scenario3-library.md` for its load.

## 5. The primary-key lookup does not scale, and that is the control

`read-bar-lookup` is a pk equality — one btree descent. Across a **40×**
growth in the relation it serves 25,126, 28,409, 26,247 statements a second
on ckdbs and 13,831, 14,599, 14,881 on PostgreSQL: flat on both, within the
floor on both. `read-join-point` (21,459 / 21,459 / 22,422) and
`read-join-exists` (25,907 / 23,419 / 24,390) are flatter still.

That is the control this ladder needs. A shape that does not move with the
row count says the harness is measuring the row count where it should and not
somewhere else — and it prices the fixed cost directly: **a ckdbs statement
that touches one row runs at ~26,000 a second against PostgreSQL's ~14,500**,
a 1.8× ratio that holds across the whole ladder.

## 6. Fixed cost, per-row cost, and where they cross

Fitting `p50 µs = fixed + per-row × bars` over the three sizes, per shape and
per engine — **a cost model, not a matrix**, so it is stated in delay. The crossover column is the row count at which the two engines' lines
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
(This is the one table in the file that is not a throughput matrix, and
deliberately: a fitted fixed cost and a fitted per-row cost are the two
parameters of a cost model, and they only add in the delay domain. Rule 5a's
exception covers it — the rows it feeds, §4 and §7, are throughput.)
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
declared Cabin sees each value again. At **10,080 bars**, statements a second
as the driver reports them:

| shape | ck cold | ck warm | **ck cabin** | ck dropped | pg cold | pg warm | **pg index** | pg dropped |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| bar-lookup | 25,857 | 26,410 | — | — | 13,568 | 13,718 | — | — |
| bar-range | 7,917 | 8,108 | — | — | 1,284 | 1,366 | — | — |
| day-slice | 1,724 | 1,724 | **27,181** | 1,666 | 1,952 | 1,947 | **12,604** | 2,022 |
| symbol-history | 116 | 136 | 132 | 138 | 18 | 24 | 24 | 25 |
| cross-join | 1,670 | 1,677 | **19,982** | 1,674 | 1,617 | 1,633 | **6,179** | 1,630 |
| point-join | 21,932 | 22,172 | — | — | 7,688 | 7,515 | — | — |
| model-join | 2,274 | 2,355 | **1,499** | 2,300 | 863 | 799 | 858 | 794 |

*(a dash is a shape whose column is a pk — a Cabin on a pk column is refused)*

**The Cabin is worth 15.8× on the day slice and 12.0× on the cross-join**, and
in both cases it beats PostgreSQL's btree index on the same column — 27,181/s
against 12,604, and 19,982 against 6,179. `dropped` returns both to within 3%
of cold, which is the control: the Cabin was the whole difference.

**On `model-join` the same Cabin costs 34%** — 1,499/s against a cold 2,274 —
and that row is worth as much as the two wins. The shape's argument is a model
id drawn from a wider pool than `--warm-keys` cycles, so its hit rate is low
and what remains is the probe's own cost paid on top of the walk. It is the
scenario3 result in miniature, inside the same cell as the two that go the
other way.

At **252 bars** the two winning Cabins are worth 1.37× and 1.29× (19,315 →
26,476 and 15,557 → 20,135), because the walk they replace is already cheap.
The Cabin's benefit therefore tracks the relation, which is the
per-row-to-fixed conversion in the same form §6 measures for the shapes with
no accelerator at all.

**Why this does not contradict `bench/results-scenario3-library.md`**, where
the same structure serves 0.59× of what no accelerator at all does — and why
the `model-join` row above is that same result appearing here. A Cabin is
authoritative only for values a query has already observed, so its benefit is
a function of how often a probe's argument repeats. Here `--warm-keys 8`
cycles eight arguments through hundreds of operations and the hit rate is
effectively total. There, `--matches 5` holds matches-per-key constant while
the relation grows, so the key space grows with it — 2,000 distinct users at
10,000 loans — and the hit rate collapses. **The two results are the same
mechanism read at opposite ends of one variable, and neither is a property of
the Cabin alone.** What decides it is the workload's argument distribution,
which is the input the `CABIN AUTO` threshold in `docs/spec/cabin.md` §11 is
still open on.

`symbol-history` is the counter-case on both engines: its result set grows
with the relation, so neither the Cabin (132/s against 116 cold) nor
PostgreSQL's index (24/s against 18) can make it fixed-cost. An accelerator
removes the cost of *finding* rows, never of *having* them.

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
**Cross-core execution (`docs/spec/crosscore.md`, P4d/P4e) is the work that would
change this row**, and until a peer-owned relation has a writer it cannot be
measured on a workload that writes.

## 10. What this run does not answer

- **Whether the four crossovers move with hardware.** §6 puts them between
  1,332 and 2,984 rows on two vCPUs of an EPYC 9V74. The crossover is a ratio
  of per-row costs, and nothing here says how it behaves on a machine with
  more memory bandwidth or more cores.
- **Why ckdbs's per-row fold cost is higher.** The four flipped shapes are all
  simple folds; the engine exposes no per-step timing that would separate the
  walk from the fold from the render. `docs/inflight/in-progress/observability.md` owns that and it
  is unbuilt. `docs/inflight/in-progress/workplan-aggregate-perf.md` AP05 is the open work.
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
