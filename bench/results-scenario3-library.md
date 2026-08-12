# scenario3: what a non-primary-key equality costs

`tools/scenario3_library.py` over a library circulation schema — `users`,
`books`, `reservations`, `loans`, all BTREE-clustered — asking one narrow
question at three cardinalities. A `WHERE user_id = ?` has no primary-key
index behind it. KDS can answer it three ways in three different trust
classes: walk the chain (`FilterScan`), consult a **Cabin** (authoritative
only for values queries have already observed), or descend a **secondary
index** (authoritative for every value).

The thesis this file has always existed to test: **a non-pk equality in this
engine is a per-row cost, all three accelerators exist to convert it into a
fixed one, and only the secondary index converts it unconditionally.** This
re-measurement confirms the first and third clauses and **overturns the
middle one**. At 10,000 rows the index turns a 587.0 µs walk into a 47.1 µs
probe — **12.5×** — with the same compiled plan at every cardinality. The
Cabin, on the same column and the same predicate, does not convert the cost
at all: it costs **1,209.7 µs**, which is **2.06× the un-indexed walk it was
meant to replace** and 25.7× the index. That is not the "first probe is
dearer, repeat probes are free" trade the previous measurement described.
It is a per-row cost that grew faster than the scan's, and it is the finding
this run exists to report.

| | |
|---|---|
| **Status — measured, ckdbs only (2026-08-12)** | All 24 ckdbs cells ran to completion on 2026-08-12, 01:36:27 – 02:05:48 UTC, and every number below is from that run. **The PostgreSQL half was not executed** — no `initdb`, `psql` or `postgres` binary exists on this host and none could be installed — so §10 of the previous version has no counterpart here, and **no cross-engine ratio in this file is current**. This supersedes the aborted 2026-08-08 refresh: that one never measured a cell, this one measured all of them. |
| **Branch / commit** | `worktree-benchmark-update` / **`dd32cb8`**, `dirty: true` in all 24 cells. The dirt was `bench/run_cell.sh` alone, committed immediately after as `53fa225`; `git diff --name-only ed03b44 dd32cb8` is exactly `bench/run_cell.sh` and nothing else. |
| **What engine this is** | `dd32cb8`'s engine source is **byte-identical to `ed03b44`**, which is the commit now tagged **`v1.0.0`** — `git diff ed03b44 HEAD -- src/ include/ tests/` is empty. These numbers therefore describe the tagged release, not a tree adjacent to it. |
| **Not comparable to the previous version of this file** | The superseded tables were taken at `94727ee` on an **AMD EPYC 7571 / EC2 / 7.6 GiB** host. This run is a different machine *and* a different engine (the superblock format has moved 12 → **14**, so a data file from either build will not mount on the other). The old tables now live in **`bench/results-scenario3-library-2026-08-08.md`**, kept because they hold the only PostgreSQL comparison scenario3 has ever had. Nothing here is a delta against them; it is a fresh measurement, and the two must not be differenced. |
| **Machine** | **AMD EPYC 9V74 80-Core**, 2 vCPU allocated, 15 GiB, kernel 6.17.0-1022-azure. `uptime` is recorded immediately before and after every cell in its log. **This box is materially noisier than the EC2 host the old numbers came from** — see §2, which is why the floor is up to 3.5× the old host's. |
| **Contention control** | `bench/run_cell.sh` samples `pgrep -c cc1plus` **before and after** every cell and exits 8 if a compiler ran at either end; the matrix discards such a cell and re-runs it. Two other Claude sessions were compiling on this host throughout, and **one cell (`ck-single-off-200`) was discarded and re-measured** on that rule — recorded in the matrix driver's own log, which is not retained in-repo, because at the time the re-run overwrote both the cell's log and its JSON. `run_cell.sh` now moves a discarded cell's artefacts to `.contended` siblings instead, so the next such discard leaves evidence. No cell in the tables below saw a compiler at either boundary. |
| **Build type** | **Release**. `build-release/CMakeCache.txt` carries `CMAKE_BUILD_TYPE:STRING=Release`; the binary's mtime is 2026-08-12 01:12:23 UTC, after the tree it measures. `./build` is Debug and produced no number here. |
| **Device** | `/dev/nvme0n1p1`, NVMe SSD (`ROTA=0`), mounted at `/`; data files under `/home/cdkbs/bench-s3/db/`, WAL under `/home/cdkbs/bench-s3/wal/<cell>/`. **Not tmpfs** — that would make every fsync free and turn §8's durability finding into fiction. |
| **Server config** | `cores = 1`, `durability = group`, `indexes = on` for the read-benefit cells; `indexes = off` for the `*-off-*` cells; `durability = relaxed` and `strict` for §8. One server process and one **fresh data file** per cell, torn down after it. |
| **Baseline** | **None. Not executed.** |
| **Correctness** | **291,374 operations across all 24 cells, 0 errors**, and `verify_problems` empty in every one — including the check that a `WHERE user_id = ?` reply equals a client-side-filtered full scan, which is the check that would catch an index or a Cabin serving an incomplete set. `--assert-index-reads` was passed on **5 of the 17 cells that declare an index** — `ck-single-200/1000/10000`, `ck-composite-10000`, `ck-covering-10000` — and it checks the **plan** through `ANALYZE` rather than the latency, so a silent regression to a scan fails those runs instead of passing as a flat result. The other 12 index cells (every `*-rep-*`, every `*-off-*`, and the three durability cells) omit the flag; their plans are recorded in `meta.plans` regardless, §3 reads all 24 from there, and every index cell shows `IndexProbe`/`IndexRange`. The check is therefore asserted on 5 cells and merely *observed* on the rest. |

Drivers, flags and exact invocations: `bench/docs/README.md`, entry
`scenario3_library.py`. The per-cell runner is `bench/run_cell.sh`.

---

## 1. The sizes, and why they move only one variable

`--loans N` is the row-set axis. The other three relations are derived from it
so that **matches per key stays constant at 5** (`meta.matches_per_key`):

| `--loans` | `users` | `books` | `reservations` | `loans` | rows loaded |
|---|---|---|---|---|---|
| 200 | 40 | 40 | 100 | 200 | 380 |
| 1000 | 200 | 200 | 500 | 1000 | 1,900 |
| 10000 | 2000 | 2000 | 5000 | 10000 | 19,000 |

`users = books = loans / --matches` and `reservations = loans / 2`, read out of
each cell's `meta.sizes`.

Holding matches constant is the whole point of the sweep. If `users` were
fixed while `loans` grew, a larger relation would also be a *less selective*
predicate, and a scan's O(rows) cost would be confounded with an index probe's
O(log rows + matches): two variables moving at once, and no way to tell a
fixed cost from a per-row one.

`books-by-genre` is the deliberate counter-case — genre cardinality is fixed
at 16, so its match count grows with the relation, and it is in the matrix to
show what happens to an index when the *result* scales with the relation.

`pk-user` is the control: one row reached through the Keystone pk, so nothing
in this matrix should move it. §2 uses that property to audit the floor
itself.

---

## 2. The noise floor, and why only the 10,000-row column carries a finding

Six cells were re-run as replicates under identical configuration and a fresh
data file. The floor is the widest disagreement between a replicate pair,
taken over the ten read shapes.

Two replicates disagree by some amount; which of them is the denominator
changes the percentage. **Every figure here divides by the smaller of the
pair**, which is the larger of the two possible percentages and therefore the
conservative one. Stating the convention matters: dividing by the first-named
cell instead would report 22.7% / 28.6% / 25.7% and understate the two small
sizes by a quarter to a third.

| replicate pair | N | max abs. Δ p50 | median abs. Δ p50 | max abs. Δ mean |
|---|---|---|---|---|
| `ck-single-200` vs `ck-single-rep-200` | 200 | 29.4% | 1.2% | 17.4% |
| `ck-single-1000` vs `ck-single-rep-1000` | 1000 | 40.0% | 32.1% | 39.1% |
| `ck-strict-1000` vs `ck-strict-rep-1000` | 1000 | 26.0% | 9.3% | 24.8% |
| `ck-single-10000` vs `ck-single-rep-10000` | 10000 | 21.8% | 2.4% | 32.8% |
| `ck-none-10000` vs `ck-none-rep-10000` | 10000 | 4.0% | 1.7% | 13.2% |
| `ck-single-off-rep-10000` vs `ck-single-off-rep2-10000` | 10000 | 25.7% | 2.1% | 89.5% |

**The floor adopted below is ±29.4% at 200 rows, ±40.0% at 1,000 and ±25.7% at
10,000, on p50.** Every comparison table carries a "clears floor?" column so
the refusal is visible rather than inferred. The mean is noisier than p50 at
every size — **89.5% between two runs of an identical configuration** — so it
never grounds a claim.

**The floor is 3.3× the old EC2 host's at 200 rows and 3.5× at 10,000** (8.8%
/ 20.8% / 7.3%), though only 1.9× at 1,000. Two vCPUs sliced off an 80-core
part, with other work resident on the host, is simply a noisier place to
measure than a dedicated 2-vCPU instance was.

### 2.1 The control says the floor is still too generous at small N

`pk-user` reaches one row through the pk. No cell in this matrix should move
it. Comparing it across configurations that should be identical for it:

| N | `pk-user`, `indexes=off` cell | same, `indexes=on` cell | Δ | floor | within floor? |
|---|---|---|---|---|---|
| 200 | 29.9 µs | 39.5 µs | **32.1%** | 29.4% | **no** |
| 1000 | 40.3 µs | 30.1 µs | 33.9% | 40.0% | yes |
| 10000 | 40.4 µs | 40.7 µs | **0.7%** | 25.7% | yes |

At 200 rows the control moves **more than the floor**: two cells that differ
only in a key irrelevant to `pk-user` disagree about it by a third, so at that
size even the conservative floor understates true cross-cell variance. At
1,000 the control stays inside its floor only because that floor is 40% —
which is a statement about how little 1,000-row cell can resolve, not a
reassurance.

**Consequence, and it governs every table below: at 200 and 1,000 rows this
run supports no finding smaller than a factor of two.** The 10,000-row column
is where the control is genuinely flat (0.7% against a 25.7% floor) and the
effects are enormous (up to 93%), and that is the only column any conclusion
here rests on. Both smaller sizes are reported for shape, never for magnitude.
§8 records the one place a control does move at 10,000 rows.

---

## 3. What the engine actually did

`ANALYZE <statement>` is this engine's `EXPLAIN`. The driver runs it on seven
shapes every cell, and `--assert-index-reads` fails the run on a plan that
regressed to a scan. Plans were identical at all three cardinalities within
each `--index-mode`, which is the first substantive result: **KDS does not
have a cost model that declines its own index on a small relation.** It
descends whenever one is declared.

| shape | `none` | `single` | `composite` | `covering` | `none --cabin` |
|---|---|---|---|---|---|
| `loans-by-user` | FilterScan | **IndexProbe** | FilterScan | **IndexProbe** | **CabinProbe** |
| `loans-by-book` | FilterScan | **IndexProbe** | FilterScan | FilterScan | FilterScan |
| `resv-by-user` | FilterScan | **IndexProbe** | FilterScan | FilterScan | FilterScan |
| `books-by-author` | FilterScan | **IndexProbe** | FilterScan | FilterScan | FilterScan |
| `books-by-genre` | FilterScan | **IndexProbe** | FilterScan | FilterScan | FilterScan |
| `overdue` | FilterScan | FilterScan | **IndexRange** | FilterScan | FilterScan |
| `loans-by-daterange` | Scan | Scan | Scan | Scan | Scan |

Two things to read off it. `overdue` is served by an index in exactly one
configuration, `composite` — §7 prices that. And `loans-by-daterange` is a
`Scan` in every configuration including `composite`, so wherever it moves
below, it is measuring the machine and not a structure.

---

## 4. The whole cost of an index: `--index-mode none` vs `single`

Comparing a server with no index declared against one with `single` prices the
index's *entire* cost — backfill, per-write maintenance, space — against its
read benefit.

**At 10,000 rows** (floor 25.7%):

| shape | `none` p50 | `single` p50 | ratio | Δ | clears floor? |
|---|---|---|---|---|---|
| `loans-by-book` | 581.8 µs | 38.6 µs | **15.07×** | −93.4% | yes |
| `count-by-user` | 576.4 µs | 44.7 µs | **12.89×** | −92.2% | yes |
| `loans-by-user` | 587.0 µs | 47.1 µs | **12.46×** | −92.0% | yes |
| `resv-by-user` | 303.2 µs | 44.6 µs | **6.80×** | −85.3% | yes |
| `books-by-author` | 163.6 µs | 46.1 µs | **3.55×** | −71.8% | yes |
| `books-by-genre` | 186.3 µs | 114.6 µs | **1.63×** | −38.5% | yes |
| `pk-user` | 39.5 µs | 40.7 µs | 0.97× | +3.0% | no |
| `join-loan-user` | 3473.8 µs | 3568.1 µs | 0.97× | +2.7% | no |
| `loans-by-daterange` | 748.1 µs | 898.0 µs | 0.83× | +20.0% | no |
| `overdue` | 804.9 µs | 965.9 µs | 0.83× | +20.0% | no |

The gradient across the first six rows is the entire point. `loans-by-book`,
`count-by-user` and `loans-by-user` all resolve five matching rows, and all
win by more than twelve times. `books-by-genre` resolves a match count that
*grows with the relation* — 16 fixed genres over 2,000 books — and wins only
1.63×. **The index converts a per-row cost into a fixed one; when the result
itself is per-row, there is proportionally less to convert.**

`pk-user` and `join-loan-user` do not move, and should not: one is the pk
control, the other is dominated by join cost the index does not touch.

`loans-by-daterange` and `overdue` are both **20.0% slower** with the index
declared. Neither clears the 25.7% floor, so this file does not claim the
index made them slower. It is worth naming rather than dropping, because both
are `FilterScan`/`Scan` shapes that pay the index's write-side maintenance
and receive nothing back, the sign is consistent across two independent
shapes, and it is exactly the direction §5 predicts.

At **1,000 rows** (floor 40.0%) **four** shapes clear: the same three
five-match shapes at 2.71×, 2.60× and 2.44×, plus `resv-by-user` at 1.71×
(57.0 → 33.3 µs, −41.6%). `resv-by-user` clears the floor but not §2.1's
factor-of-two rule for this size, so it is reported and not relied on. At
**200 rows** (floor 29.4%) **nothing clears** — `books-by-author`'s 1.32× is
the widest effect at that size and it falls short — which is exactly what
§2.1's control predicts.

---

## 5. The read benefit alone: `indexes = off`

`indexes = off` (the IX13 config key, read at server start) leaves the index
**declared, backfilled and maintained** and disables only the read path.
Comparing it against `indexes = on` isolates the read benefit with every
write-side cost still being paid. Reporting only §4 would credit the index for
a saving whose cost sits in a phase that table does not show.

**At 10,000 rows** (floor 25.7%):

| shape | `off` p50 | `on` p50 | `none` p50 | on vs off | clears floor? |
|---|---|---|---|---|---|
| `loans-by-book` | 597.4 µs | 38.6 µs | 581.8 µs | −93.5% | yes |
| `count-by-user` | 599.3 µs | 44.7 µs | 576.4 µs | −92.5% | yes |
| `loans-by-user` | 602.3 µs | 47.1 µs | 587.0 µs | −92.2% | yes |
| `resv-by-user` | 317.2 µs | 44.6 µs | 303.2 µs | −85.9% | yes |
| `books-by-author` | 167.9 µs | 46.1 µs | 163.6 µs | −72.5% | yes |
| `books-by-genre` | 192.4 µs | 114.6 µs | 186.3 µs | −40.4% | yes |
| `pk-user` | 40.4 µs | 40.7 µs | 39.5 µs | +0.7% | no |
| `join-loan-user` | 3497.2 µs | 3568.1 µs | 3473.8 µs | +2.0% | no |
| `loans-by-daterange` | 767.3 µs | 898.0 µs | 748.1 µs | +17.0% | no |
| `overdue` | 829.2 µs | 965.9 µs | 804.9 µs | +16.5% | no |

**The `off` column and the `none` column agree to within 4.7% on every shape**
(widest: `resv-by-user`, +4.62%; median +2.65% over all ten shapes).
An index that is maintained but not read costs a query exactly what having no
index costs it. That is the cleanest statement this run makes: the entire
benefit measured in §4 is a *read-path* benefit, and the read path is the only
thing the IX13 key removes.

It also settles the plan question §3 raised. Every `off` cell still reports
`IndexProbe` from `ANALYZE`, because the compiled chain is identical either
way — but it executes at scan cost. **`ANALYZE` names the compiled plan, not
the path taken at run time**, and any future reading of an `indexes = off`
plan must not take `IndexProbe` as evidence that an index was descended.

---

## 6. The Cabin against an index, on the same column and the same predicate

A Cabin is authoritative only for values queries have already observed; an
index is authoritative for every value. No other document measures the two
against the same column and the same predicate, which is why this section
exists.

| N | shape | Cabin | index | scan (`none`) | Cabin vs scan | Cabin vs index |
|---|---|---|---|---|---|---|
| 200 | `loans-by-user` | 42.7 µs | 44.9 µs | 51.6 µs | −17.2% | −4.9% |
| 1000 | `loans-by-user` | 149.7 µs | 35.8 µs | 97.1 µs | **+54.2%** | **+318%** |
| 10000 | `loans-by-user` | 1209.7 µs | 47.1 µs | 587.0 µs | **+106.1%** | **+2468%** |
| 10000 | `count-by-user` | 1197.0 µs | 44.7 µs | 576.4 µs | **+107.7%** | **+2578%** |

**At 10,000 rows a declared Cabin makes the query it accelerates 2.06× slower
than having no accelerator at all**, and it does so through a confirmed
`CabinProbe` plan — the driver's `ANALYZE` names it, so this is the Cabin path
executing, not a silent fall-through to a scan. The effect clears the 25.7%
floor by a factor of four and reproduces on a second shape.

The direction of the trend is what makes this a finding rather than a bad
cell. Cabin-vs-scan moves −17.2% → +54.2% → +106.1% as the relation grows
10× and then 10× again, while the index's advantage grows over the same span.
**The Cabin's probe cost is scaling with the relation, which is the one thing
an accelerator for a per-row cost must not do.** The 200-row cell, the only
one where the Cabin is nominally ahead, does not clear its floor and §2.1
disqualifies that size anyway.

The load phase is flat across all three configurations at every size (998.8 /
1011.6 / 1007.1 µs for Cabin, against 1034.8 / 1127.0 / 1019.9 µs for `none`),
so **this is not write-through recording cost**. The previous version of this
file attributed the Cabin's expense to its write-through path making the first
probe dearer; on this engine at `dd32cb8` that explanation does not fit — the
writes are free and the *reads* are what cost.

This file does not diagnose the cause; it is a benchmark. What it establishes
is that **the observed-⇒-complete invariant is not being bought at a fixed
read price at this commit**, and that `docs/feat-cabin.md` §11's open budget
and cap decisions now have a measurement to argue with rather than an
expectation. Gap **G1** in §9 names the experiment that would separate a
per-probe cost from a per-entry one.

---

## 7. Composite and covering

### 7.1 A composite key is the only structure that helps `overdue`

`overdue` is a two-predicate shape, and §3 shows it compiles to `IndexRange`
in exactly one configuration.

| shape | `none` | `single` | `composite` |
|---|---|---|---|
| `overdue` | 804.9 µs | 965.9 µs | **219.1 µs** |

**3.67× against `none`, and 4.41× against `single`** — both far clear of the
25.7% floor. It is the only measurement in this run where `single` is the
*wrong* index and a composite key is the right one, and it costs `composite`
its `loans-by-user` probe to get it: that shape falls back to `FilterScan` at
585.9 µs, indistinguishable from the 587.0 µs un-indexed walk.

`composite` also has the worst `join-loan-user` in the matrix at 5,252.1 µs
against 3,473.8 µs for `none` (+51.2%). That clears the floor and is not
explained by anything in §3, where the join's constituent shapes are
`FilterScan` in both. It is recorded as a measurement, not as a claim about
mechanism, and gap **G2** names it.

### 7.2 `COVERING` bought nothing measurable, and cost the most to build

| shape | `single` | `covering` |
|---|---|---|
| `loans-by-user` | 47.1 µs | 48.2 µs (+2.3%, under floor) |
| `count-by-user` | 44.7 µs | 42.6 µs (−4.7%, under floor) |
| `loans-by-book` | 38.6 µs | 608.1 µs |
| `resv-by-user` | 44.6 µs | 314.0 µs |

On the one shape it covers, `covering` and `single` are indistinguishable —
both differences are an order of magnitude inside the floor. On every shape it
does not cover, it falls back to `FilterScan` at full un-indexed cost, which
§3's plan table predicts exactly.

The backfill is where it differs, and it must be compared at equal entry
counts. `single` issues **five** `CREATE INDEX` statements over relations of
different sizes, so its p50 is the median of five different jobs and not a
unit price: from `SHOW INDEXES` in the cell log, the five build indexes of
2,000 / 2,000 / 5,000 / 10,000 / 10,000 entries, and the five statement times
recover from the phase percentiles and mean as 1,357.2 / 1,508.3 / 3,203.0 /
6,597.1 / 6,670.4 µs — about 0.67 µs per entry throughout. `covering` issues
**one** statement, over 10,000 entries.

| index built | entries | backfill |
|---|---|---|
| `single`, `loans_user` | 10,000 | 6,597.1 µs |
| `single`, `loans_book` | 10,000 | 6,670.4 µs |
| `covering`, `loans_user_cov` | 10,000 | **7,204.3 µs** |

**Like for like at 10,000 entries, `COVERING` costs 1.08× a plain index to
build — 0.720 µs per entry against 0.667 — and returns nothing measurable on
the query it covers.** The extra is the wider entry: 33 bytes against 17, for
the three covered columns.

The case against declaring one therefore rests on the *read* side, not the
build: the index-only scan that would make a covering index pay is gated on a
visibility witness (`docs/feat-index.md` §13), and until that lands the
covered columns are carried and never used.

---

## 8. Durability: one boundary is resolvable, the other is not

The load phase is the write-bound phase; the read shapes are not.

| cell | `durability` | load p50 | load mean | `loans-by-user` | `pk-user` |
|---|---|---|---|---|---|
| `ck-single-10000` | `group` | 1021.2 µs | 1085.4 µs | 47.1 µs | 40.7 µs |
| `ck-strict-10000` | `strict` | 999.9 µs | 1075.5 µs | 46.3 µs | 40.4 µs |
| `ck-relaxed-10000` | `relaxed` | **27.5 µs** | 79.6 µs | 37.1 µs | **29.7 µs** |

**`relaxed` is 37.1× cheaper than `group` on the load phase**, which is the
fsync. That is the expected shape of the result and it is not the interesting
half.

One read shape does move: **`pk-user` differs by 37.0% between the `relaxed`
cell and the `group` one** (29.7 vs 40.7 µs, on §2's smaller-denominator
convention), which clears the 25.7% floor.
`pk-user` is the pk control and a durability class cannot plausibly reach it,
so this is cross-cell variance that happens to exceed the floor rather than an
effect — the same failure §2.1 documents at 200 rows, appearing once at
10,000. It is recorded rather than dropped precisely because §2.1 leans on
this control being flat, and here, in one cell pair, it is not.

The interesting half is that **`group` and `strict` are indistinguishable** —
1021.2 µs against 999.9 µs, a 2.1% difference against a 25.7% floor, and the
same at 1,000 rows (1011.9 vs 996.1 µs). At `cores = 1` with a single client
there is no second transaction to group *with*, so the group commit has
nothing to amortise and degenerates to the per-commit fsync `strict` already
performs. **This run therefore prices `group` against `strict` at nothing, and
that null result is a property of the single-client shape of this benchmark,
not a general claim about the two classes.** Gap **G3** names the concurrent
measurement that would actually separate them.

---

## 9. What this run teaches, and the gaps

### The engine

1. **The secondary index is the only accelerator here that works
   unconditionally.** 12.5× on the headline shape at 10,000 rows, up to
   15.1×, through the same compiled plan at every cardinality, and §5 shows
   the entire benefit is a read-path benefit.
2. **The benefit is a conversion, not a discount.** Shapes resolving a fixed
   five rows win 12–15×; `books-by-genre`, whose result grows with the
   relation, wins 1.63×. An index converts a per-row cost into a fixed one and
   can only convert the part that is per-row.
3. **The Cabin does not currently convert that cost.** At 10,000 rows it is
   2.06× the un-indexed walk through a confirmed `CabinProbe`, and its
   disadvantage grows with the relation. This contradicts the previous
   version of this file and is the single most important thing here.
4. **`ANALYZE` reports the compiled plan, not the executed path.** Every
   `indexes = off` cell prints `IndexProbe` and runs at scan cost. Any future
   diagnosis that reads a plan as proof of a descent will be wrong.
5. **KDS has no small-relation cost model.** It descends a declared index at
   200 rows as readily as at 10,000. Whether that is a defect is a planner
   decision this file does not own.
6. **`COVERING` costs 1.08× a plain index to build and returns nothing** until the
   index-only scan lands.

### Named gaps — measurements this document does not contain

- **G0 — the PostgreSQL baseline was not executed.** No `initdb`, `psql` or
  `postgres` binary exists on this host. Every cross-engine claim in
  `results-scenario3-library-2026-08-08.md` is stale, and this file replaces
  none of them. Until a cluster exists here, scenario3 has no baseline.
- **G1 — the Cabin's cost is unattributed.** §6 establishes that the probe
  scales with the relation and that the load phase does not. Separating a
  per-probe cost from a per-entry one needs a sweep of Cabin entry count at
  fixed relation size, which this matrix does not contain.
- **G2 — `composite`'s +51.2% on `join-loan-user` is unexplained.** It clears
  the floor and §3's plans do not account for it.
- **G3 — `group` vs `strict` was measured at one client and is therefore
  untested.** §8's null result cannot be read as "the classes cost the same";
  it needs a concurrent load.
- **G4 — the two small cardinalities carry no findings.** §2.1 shows the
  control moving 32% at 200 rows. Reproducing the old file's small-N
  resolution needs a quieter host, not more replicates.
- **G5 — this host is shared.** Two other build-and-test sessions were
  resident throughout. The per-cell guard discarded and re-ran what it could
  detect (one cell), but a floor up to 3.5× the old host's is the price, and
  no amount of in-run control removes it.

---

The superseded 2026-08-08 measurement at `94727ee` — different machine,
different engine, and the only PostgreSQL comparison scenario3 has ever
had — is kept verbatim in
[`results-scenario3-library-2026-08-08.md`](results-scenario3-library-2026-08-08.md).
Do not quote it as current and do not difference it against anything above.
