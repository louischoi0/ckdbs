# scenario3: what a non-primary-key equality costs

`tools/scenario3_library.py` over a library circulation schema — `users`,
`books`, `reservations`, `loans`, all BTREE-clustered — asking one narrow
question at three cardinalities. A `WHERE user_id = ?` has no primary-key
index behind it. KDS can answer it three ways, in three different trust
classes: walk the chain (`FilterScan`), consult a **Cabin** (authoritative
only for values queries have already observed), or descend a **secondary
index** (authoritative for every value). PostgreSQL answers it one way, with
a btree index, which is what makes it a baseline rather than a second pile of
numbers.

Three findings, in the order of how much they should change what gets built
next.

**The index converts the cost, and it converts it further than PostgreSQL
does.** At 10,000 rows a `WHERE user_id = ?` goes from **1,563 to 21,322
statements a second — 13.6×** — and lands **1.8× above PostgreSQL's own
indexed answer** (11,848/s). Every equality shape in the matrix behaves the
same way. On this workload, with an index declared, KDS is the faster engine
on nine of ten read shapes.

**The tenth shape is a 40× loss, and it is one missing optimization.** A join
whose restriction sits on the *other* relation — `loans JOIN users ON
l.user_id = u.id WHERE u.id = 3` — compiles to a full `Scan` of `loans` and a
pk `Probe` per row: 10,000 opens, 10,086 pages, to return 6 rows. The
identical predicate written against one relation compiles to an `IndexProbe`
and reads **7 pages**. KDS does not push an equality through a join key, so
the index it has is invisible to the join. PostgreSQL pushes it, reads 14
buffers, and serves **8,850 statements a second against KDS's 223**.
*(2026-08-18: closed — the propagation landed as `881f69a` and §9a measures
it by interleaved A/B at **84.8× on p50**, with every other shape inside the
replicate floor.)*

**The Cabin still does not convert the cost, and at 10,000 rows it halves
the throughput.** On the same column and the same predicate it serves **926
statements a second against the 1,563/s of the walk it was meant to
replace** — 0.59× of doing nothing, and 23× short of the index. This
reproduces the inversion the previous measurement of this file found, on a
different engine build and a cleaner noise floor. *(2026-08-19: closed —
the recording miss's full-row decode was the cost, the fix landed as
`a44c5cc`, and §7a measures it by interleaved A/B: the miss falls from
2.04× the walk to 1.06×. The hit-rate economics — when a Cabin is worth
declaring at all — stand unchanged.)*

## 1. The run

| | |
|---|---|
| executed | **2026-08-18 05:01:51 → 05:20:10 UTC**, 35 cells — 25 ckdbs, 10 PostgreSQL |
| branch / worktree | `worktree-bench-scenario2-postgres`, in the worktree `bench-scenario2-postgres` |
| commit measured | **`9f762a3`** — recorded by every cell, `dirty: false` in all 25 |
| **binary measured** | a **copy**, `sha256 7312b75f095e8d64…`, taken from `build-release/kds_server` (linked 2026-08-18 04:36:09 UTC) before the first cell and never rewritten — the build tree is shared with other sessions, and a rebuild landing mid-matrix would put two engines under one heading. The link predates `9f762a3` by two commits, both of which touch `bench/run_pg_cell.sh` only: `git diff b1bbec0 9f762a3 -- src include tests` is **empty**, so the binary is the engine at `9f762a3` |
| build | Release (`-O3 -DNDEBUG`), gcc 13.3.0, `KDS_WITH_TLS=ON` (OpenSSL 3.0.13) |
| PostgreSQL | **16.14**, extracted rootless into `$HOME/pg16` (`bench/docs/README.md` carries the recipe), cluster on port 15433, **PostgreSQL's own defaults** — including `ANALYZE` after each load, which is not tuning: without statistics the planner may decline its own index and the baseline becomes a coin toss |
| device | ext4 on `/dev/root`; ckdbs data files under `$HOME/bench-s3/db/`, WAL under `$HOME/bench-s3/wal/<cell>/`, PostgreSQL under `$HOME/pg-bench/data`. **Not tmpfs** — that would make §10's durability finding fiction |
| kernel / host | 6.17.0-1022-azure, Ubuntu 24.04, AMD EPYC 9V74, **2 vCPUs** |
| server config | `cores = 1`, `durability = group`, `indexes = on`; `indexes = off` for the `*-off-*` cells; `relaxed` and `strict` for §10. One server process and one **fresh data file** per cell |
| PostgreSQL isolation | one **fresh database** per cell, not fresh relations — dropping relations leaves the cluster's bloat behind and a fresh data file does not |
| contention control | every cell gates on `bench/wait_quiet.sh` first, and `run_cell.sh` / `run_pg_cell.sh` both sample `pgrep -c cc1plus` before **and** after the cell, moving a contended cell's artefacts to `.contended` siblings and exiting 8. **One cell was discarded and re-run**: `ck-none-rep-10000`, whose evidence is still at `$HOME/bench-s3/{json,logs}/ck-none-rep-10000.*.contended`. Other sessions built on this box throughout |
| correctness | **376,523 operations across all 35 cells, 0 errors**, and `verify_problems` empty in every ckdbs cell — including the check that a `WHERE user_id = ?` reply equals a client-side-filtered full scan, which is what would catch an index or a Cabin serving an incomplete set. `--assert-index-reads` was passed on the three `ck-single-*` cells and checks the **plan** through `ANALYZE`, so a regression to a scan fails the run rather than passing quietly |

Drivers, flags and exact invocations: `bench/docs/README.md`, entry
`scenario3_library.py`. The per-cell runners are `bench/run_cell.sh` and
`bench/run_pg_cell.sh`.

## 2. The sizes, and why they move only one variable

`--loans N` is the row-set axis. The other three relations are derived from it
so that **matches per key stays constant at 5**:

| `--loans` | `users` | `books` | `reservations` | `loans` | rows loaded |
|---:|---:|---:|---:|---:|---:|
| 200 | 40 | 40 | 100 | 200 | 380 |
| 1,000 | 200 | 200 | 500 | 1,000 | 1,900 |
| 10,000 | 2,000 | 2,000 | 5,000 | 10,000 | 19,000 |

`users = books = loans / --matches`, `reservations = loans / 2`.

Holding matches constant is the whole point. If `users` were fixed while
`loans` grew, a larger relation would also be a *less selective* predicate,
and a scan's O(rows) cost would be confounded with a probe's
O(log rows + matches): two variables at once, and no way to tell a fixed cost
from a per-row one.

`books-by-genre` is the deliberate counter-case — genre cardinality is fixed
at 16, so its match count grows with the relation, and it is in the matrix to
show what happens to an index when the *result* scales. `pk-user` is the
control: one row through the Keystone pk, which nothing in this matrix should
move.

## 3. The noise floor

Nine ckdbs replicate pairs and four PostgreSQL ones, each a re-run of an
identical configuration against a fresh data file or database. The floor is
the widest disagreement between a pair over the ten shapes, on the same
throughput basis every matrix below uses, **dividing by the smaller of each
pair** — the larger of the two possible percentages, and therefore the
conservative one.

| replicate pair | N | engine | max abs. Δ QPS | median abs. Δ QPS |
|---|---:|---|---:|---:|
| `ck-none-200` vs `ck-none-rep-200` | 200 | ckdbs | 6.7% | 2.0% |
| `ck-single-200` vs `ck-single-rep-200` | 200 | ckdbs | 9.4% | 4.4% |
| `ck-cabin-200` vs `ck-cabin-rep-200` | 200 | ckdbs | 7.3% | 1.2% |
| `ck-none-1000` vs `ck-none-rep-1000` | 1,000 | ckdbs | 8.5% | 2.5% |
| `ck-single-1000` vs `ck-single-rep-1000` | 1,000 | ckdbs | 8.4% | 1.6% |
| `ck-cabin-1000` vs `ck-cabin-rep-1000` | 1,000 | ckdbs | 9.1% | 4.1% |
| `ck-none-10000` vs `ck-none-rep-10000` | 10,000 | ckdbs | 23.6% | 7.0% |
| `ck-single-10000` vs `ck-single-rep-10000` | 10,000 | ckdbs | 18.1% | 2.3% |
| `ck-cabin-10000` vs `ck-cabin-rep-10000` | 10,000 | ckdbs | 10.7% | 2.3% |
| `pg-none-200` vs `pg-none-rep-200` | 200 | postgresql | 44.0% | 10.2% |
| `pg-single-200` vs `pg-single-rep-200` | 200 | postgresql | 17.3% | 9.8% |
| `pg-none-10000` vs `pg-none-rep-10000` | 10,000 | postgresql | 5.6% | 1.9% |
| `pg-single-10000` vs `pg-single-rep-10000` | 10,000 | postgresql | 11.5% | 5.0% |

**The floors adopted below: ckdbs ±9.4% at 200, ±9.1% at 1,000, ±23.6% at
10,000; PostgreSQL ±44.0% at 200 and ±11.5% at 10,000.** PostgreSQL's 200-row
floor is the widest cell in the run and is why no small-N PostgreSQL
difference is reported as a result.

**The floor is far tighter than the previous measurement of this file
managed** — this run's worst ckdbs pair disagrees by 23.6% where that one's
worst disagreed by 40%, on the same machine, with other sessions compiling
throughout. The difference is the quiet gate plus discarding any cell a
compiler ran during. That is worth recording as a method result: on a
two-vCPU box shared with other build sessions, gating each cell on an idle
machine is the difference between a matrix that can resolve 10% and one that
could not resolve a factor of two.

**Where the floor is still too generous.** `pk-user` reaches one row through
the pk, and no configuration in this matrix should move it:

| N | spread of `pk-user` across the four configurations | floor | inside? |
|---:|---|---:|---|
| 200 | 23,866–24,938/s — **4.5%** | 9.4% | yes |
| 1,000 | 24,096–25,773/s — **7.0%** | 9.1% | yes |
| 10,000 | 24,155–29,412/s — **21.8%** | 23.6% | yes, barely |

At 10,000 the control moves almost as far as the floor, driven by one cell
(`ck-single-10000` at 29,412/s against ~24,500 elsewhere). **Consequence: no
finding below rests on anything under 25%.** Every one that is reported is a
factor, not a percentage.

## 4. What the engine actually did

`ANALYZE <statement>` is this engine's `EXPLAIN`; the driver runs it on seven
shapes every cell. Plans were identical at all three cardinalities within
each `--index-mode`, which is itself the first result: **KDS has no cost model
that declines its own index on a small relation. It descends whenever one is
declared.** PostgreSQL does decline — at 200 rows its planner picks a
`Seq Scan` over its own index — and §7 is where that shows up as a latency.

| shape | `--index-mode none` | `single` | `--cabin` |
|---|---|---|---|
| loans-by-user | FilterScan | **IndexProbe** | **CabinProbe** |
| loans-by-book | FilterScan | **IndexProbe** | FilterScan |
| resv-by-user | FilterScan | **IndexProbe** | FilterScan |
| books-by-author | FilterScan | **IndexProbe** | FilterScan |
| books-by-genre | FilterScan | **IndexProbe** | FilterScan |
| loans-by-daterange | Scan | Scan | Scan |
| overdue | FilterScan | FilterScan | FilterScan |

Two shapes are never served by `single`: `loans-by-daterange` is a range over
an unindexed column, and `overdue` is a two-column predicate that no
single-column index satisfies. §8 is where a composite key changes that.

## 5. The whole matrix, at three sizes

**Statements per second**, derived from each phase's mean as
`1,000,000 / mean µs` — the driver is a serial single-connection loop, so
that is exactly its `ops / elapsed`. Higher is better in every cell.
`off` is `indexes = off`: the index declared, backfilled and maintained,
with only the *read* path disabled, which is what isolates the read benefit
from the write-side cost.

**N = 200**

| shape | ck none | ck single | ck cabin | ck off | pg none | pg single |
|---|---:|---:|---:|---:|---:|---:|
| pk-user | 24,691 | 24,938 | 23,866 | 23,981 | 15,823 | 15,723 |
| loans-by-user | 20,284 | 21,505 | 22,173 | 19,802 | 13,228 | 12,422 |
| loans-by-book | 20,619 | 22,523 | 20,284 | 20,040 | 13,699 | 12,755 |
| resv-by-user | 23,095 | 26,385 | 23,810 | 26,882 | 16,260 | 14,493 |
| books-by-author | 24,272 | 24,331 | 23,981 | 23,256 | 15,674 | 14,620 |
| books-by-genre | 24,691 | 25,316 | 25,000 | 28,986 | 15,480 | 14,993 |
| loans-by-daterange | 19,646 | 20,000 | 19,380 | 19,305 | 11,919 | 11,249 |
| overdue | 20,450 | 19,084 | 19,157 | 18,975 | 12,210 | 11,806 |
| **join-loan-user** | 7,675 | 7,955 | 7,911 | 7,937 | **9,718** | **9,950** |
| count-by-user | 21,186 | 24,510 | 28,090 | 20,408 | 13,569 | 12,723 |

**N = 1,000**

| shape | ck none | ck single | ck cabin | ck off | pg none | pg single |
|---|---:|---:|---:|---:|---:|---:|
| pk-user | 24,570 | 24,390 | 25,773 | 24,096 | 15,129 | 14,388 |
| loans-by-user | 11,123 | **22,173** | 9,208 | 10,799 | 9,425 | 11,682 |
| loans-by-book | 11,962 | **22,222** | 10,977 | 10,823 | 9,398 | 11,834 |
| resv-by-user | 15,480 | 22,779 | 15,748 | 14,903 | 12,285 | 13,423 |
| books-by-author | 19,194 | 23,256 | 18,939 | 19,120 | 13,514 | 12,706 |
| books-by-genre | 20,243 | 21,097 | 18,832 | 18,939 | 11,468 | 11,136 |
| loans-by-daterange | 9,542 | 9,560 | 9,276 | 9,579 | 5,831 | 5,750 |
| overdue | 9,141 | 9,025 | 8,850 | 9,191 | 6,258 | 6,098 |
| **join-loan-user** | 2,160 | 2,136 | 2,103 | 2,138 | **7,283** | **8,826** |
| count-by-user | 11,377 | **23,981** | 14,388 | 10,977 | 9,001 | 12,346 |

**N = 10,000** — the column every conclusion rests on

| shape | ck none | ck single | ck cabin | ck off | pg none | pg single |
|---|---:|---:|---:|---:|---:|---:|
| pk-user | 24,155 | 29,412 | 24,876 | 27,248 | 15,337 | 15,480 |
| loans-by-user | 1,563 | **21,322** | **926** | 1,804 | 2,208 | 11,848 |
| loans-by-book | 1,556 | **22,173** | 1,791 | 1,759 | 2,104 | 12,500 |
| resv-by-user | 2,783 | **23,148** | 3,190 | 3,184 | 3,915 | 13,423 |
| books-by-author | 5,831 | **22,075** | 6,154 | 5,910 | 6,562 | 12,642 |
| books-by-genre | 5,200 | 8,905 | 5,230 | 5,461 | 2,995 | 3,798 |
| loans-by-daterange | 1,305 | 1,429 | 1,406 | 1,436 | 986 | 910 |
| overdue | 1,176 | 1,318 | 1,274 | 1,309 | 1,062 | 943 |
| **join-loan-user** | 216 | 223 | 220 | 209 | **2,084** | **8,850** |
| count-by-user | 1,817 | **25,189** | **992** | 1,777 | 2,206 | 13,351 |

## 6. The index converts a per-row cost into a fixed one

The five equality shapes and the fold, `none` against `single`, at 10,000
rows, statements per second:

| shape | none | single | ratio | clears the 23.6% floor? |
|---|---:|---:|---:|---|
| loans-by-user | 1,563 | 21,322 | **13.6×** | yes |
| loans-by-book | 1,556 | 22,173 | **14.2×** | yes |
| resv-by-user | 2,783 | 23,148 | **8.3×** | yes |
| books-by-author | 5,831 | 22,075 | **3.8×** | yes |
| books-by-genre | 5,200 | 8,905 | 1.7× | yes |
| count-by-user | 1,817 | 25,189 | **13.9×** | yes |

**The indexed answer is the same 21,000–26,000 statements a second at every
cardinality** — 21,505/s at 200 rows, 22,173 at 1,000, 21,322 at 10,000 for
`loans-by-user` — while the un-indexed one falls 20,284 → 11,123 → 1,563.
That is the thesis of this file measured directly: the walk is O(rows) and
the probe is not, and a probe against 10,000 rows serves what a probe against
200 rows serves.

**`books-by-genre` is the counter-case and behaves like one.** Genre
cardinality is fixed at 16, so its result set grows with the relation; the
index still helps (1.7×), but it cannot make the shape fixed-cost, because
the rows must still be returned. An index removes the cost of *finding*
rows, never of *having* them.

**The read benefit and the whole cost are not the same number.** `ck off`
keeps the index declared, backfilled and maintained and disables only the
read path. At 10,000 rows `loans-by-user` serves 1,804/s there against
1,563/s with no index at all — the two agree within the floor, which is the
expected result and the useful control: it says the 13.6× above is the read
path and nothing else. The index's own cost is elsewhere, and it is a
one-shot build rather than a rate, so it stays in milliseconds:

| N | ckdbs `create-index`, 5 indexes | PostgreSQL, 9 indexes | per index, ckdbs | per index, PG |
|---:|---:|---:|---:|---:|
| 200 | 0.6 ms | 28.1 ms | 0.12 ms | 3.12 ms |
| 1,000 | 2.1 ms | 31.4 ms | 0.41 ms | 3.49 ms |
| 10,000 | 18.6 ms | 64.8 ms | 3.71 ms | 7.20 ms |

The backfill is where an index is paid for on this workload, and ckdbs's is
cheaper at every size — though the two engines declare different numbers of
indexes, so the per-index column is the comparable one.

## 7. The Cabin serves less than the walk it replaces

> **2026-08-19: this inversion is closed.** The cost this section measures
> was the recording miss walk decoding **every column of every walked row**;
> the fix landed as `a44c5cc` (`src/exec/step_vm.cpp`) and §7a below
> measures it by interleaved A/B against `d84fdc3`: the miss's p50 halves,
> from 2.04× the walk it shadows to **1.06×**. This section stands as the
> account of the pre-fix engine; its figures describe `9f762a3`, not the
> current engine. What it says about *hit rates* — the closing paragraph —
> is not superseded and §7a leans on it.

Same column, same predicate, at 10,000 rows, statements per second:

| | walk (`none`) | Cabin | index (`single`) |
|---|---:|---:|---:|
| loans-by-user | 1,563 | **926** | 21,322 |
| count-by-user | 1,817 | **992** | 25,189 |
| vs the walk | — | **0.59× / 0.55×** | 13.6× / 13.9× |
| vs the index | 0.07× | **0.04×** | — |

Both figures are far outside the 23.6% floor. The Cabin is doing what
`ANALYZE` says it is doing — `loans-by-user` compiles to `CabinProbe`, the
only shape it changes — and that path serves fewer statements than the
`FilterScan` it replaced. At 1,000 rows the same shape is 9,208/s against a
walk's 11,123, 0.83×; at 200 rows it is 22,173 against 20,284, marginally
better and inside the floor. **The Cabin's disadvantage grows with the
relation**, which is the signature of a per-row cost, not of a fixed setup
charge amortised over repeats.

That reproduces, on a different engine build and a floor an order of
magnitude tighter, the inversion the previous measurement of this file
reported. It is now measured twice and should be treated as the Cabin's
behaviour on this shape rather than as an artefact *(2026-08-19: no longer —
this was the pre-`a44c5cc` engine's behaviour, and §7a supersedes the
sentence; quote §7a, not this)*. `count-by-user` is the
sharper number: the same fold that runs at 25,189/s over an index runs at
**992/s** over a Cabin, a 25× difference.

**This is not the Cabin's behaviour everywhere**, and
`bench/results-scenario1-vs-pg.md` §7 measures the opposite sign on the same
structure — 15.3× *better* than the walk there. The variable is the hit rate:
that workload cycles eight arguments, this one holds matches-per-key constant
so the key space grows with the relation and a repeat becomes rare.

## 7a. Addendum, 2026-08-19 — the recording miss decodes what it reads, and the inversion is gone

The cost §7 measured twice was located by reading the path, not by timing
it: `AcceptTupleAt` forced a **full row decode for every walked row while a
Cabin recording was live** — eight columns per rejected row on `loans`,
where the plain `FilterScan` decodes only the filter's column, on a path
whose cost is decode-dominated. The fix landed as `a44c5cc`
(`src/exec/step_vm.cpp`; `docs/feat-cabin.md` §4 carries the dated
amendment): the recording walk now decodes the filter's columns per row —
the cabined equality *is* the filter, so the key column is already in the
mask — and the pk on demand, only for a row whose key matches, with the key
compare reading the step's own frame slots directly. This addendum answers
the two questions that change raises and nothing else: **how much of §7's
inversion the fix removes, and what the changed decode path costs the plain
`FilterScan` it also runs under.** Both by interleaved A/B, BASE `d84fdc3`
against NEW `a44c5cc`, alternating cell by cell within seven minutes on one
box.

**The answers: the miss's p50 halves — 2.04× the walk it shadows at BASE,
1.06× at NEW, on all three recording-miss shapes — and no walk shape moves
outside the replicate floor.**

### 7a.1 The A/B run

| | |
|---|---|
| executed | **2026-08-19 01:17:10 → 01:23:38 UTC**, 10 ckdbs cells, alternating BASE, NEW |
| branch / worktree | `worktree-enhence-join-perf`, in the worktree `enhence-join-perf` |
| commit measured | **`a44c5cc`** (recorded by every cell, `dirty: false`) — the fix commit, directly on top of `d84fdc3` |
| BASE binary | a **copy**, `sha256 4f054cbff19f23d0…`, built fresh for this run from a temporary worktree at **`d84fdc3`** (removed after the build), linked 2026-08-19 01:16:24 UTC. `d84fdc3` already carries §9a's propagation and §9b's IX17, which is why the join shape below is a Cabin shape |
| NEW binary | a **copy**, `sha256 7d8e57d09a849058…`, from this worktree's `build-release/kds_server`, linked 01:13:26 UTC — 32 s *before* `a44c5cc` was committed at 01:13:58; the tree was clean and `cmake --build` immediately before the run was a no-op against HEAD's sources, so it is the engine at `a44c5cc` |
| build | Release (`-O3 -DNDEBUG`), gcc 13.3.0, **`KDS_WITH_TLS=OFF`** — the §1 run was built TLS ON; §7a.6 bridges before comparing anything across |
| device | ext4 on `/dev/root` (checked with `df -T`; `/tmp` is ext4 on this host too); data files under `$HOME/bench-s3-cabinfix/db/`, WAL under `$HOME/bench-s3-cabinfix/wal/<cell>/` |
| server config | `cores = 1`, `durability = group`, `indexes = on`, port 15499. One server process and one **fresh data file** per cell, started from the run's own binary copy |
| driver | `tools/scenario3_library.py --loans N --index-mode none [--cabin] --ops 200 --verify 25`, seed 1 — so BASE and NEW draw **identical key sequences** and do equal work. At `--loans 10000` the cabined key space is 2,000 users and 200 ops draw ~190 distinct keys: **nearly every Cabin probe is a recording miss**, which is the path under test |
| cells | per mode and size, BASE/NEW/BASE/NEW: 4 × `--cabin` at 10,000, 4 × no-cabin at 10,000 (the walk baseline and the overhead check), 2 × `--cabin` at 1,000 (§7a.5) |
| contention control | every cell gated on `bench/wait_quiet.sh`; `run_cell.sh` sampled `pgrep -c cc1plus` before and after each cell — **0 in all 20 samples; all 10 cells passed on the first attempt, none discarded** |
| correctness | **175,840 operations, 0 errors**, `verify_problems` empty in all 10 cells; every cabin cell's `ANALYZE` says `CabinProbe`, every no-cabin cell's says `FilterScan` |

### 7a.2 The plan did not change — the cost was never in the plan

`ANALYZE`, replayed after the run by reopening two measured cells' own data
files with the binaries that produced them, is **byte-equivalent between
BASE and NEW** on the cabined shape:

```
step 0 CabinProbe loans cabin=1 on=col1 value=3
step 0 CabinProbe loans opens=1 examined=10000 matched=6 sel=0% pages=86 cabin_misses=1 cabin_recorded=1
```

Same access kind, same 10,000 rows examined, same 86 pages, on both engines.
Everything §7 priced sat *below* the plan: per-row decode work inside the
recording walk, which no counter `ANALYZE` reports can see. The join's
replay also confirms why it is a Cabin shape here: its outer step is
`CabinProbe … filter 0:0.1 = 3 derived` — §9a's propagated equality landing
on the cabined column, since `--index-mode none` gives it no index.

### 7a.3 The recording miss, BASE against NEW

Three shapes probe the Cabin on a miss at 10,000 loans: `loans-by-user`,
`count-by-user` (the same probe plus the fold), and `join-loan-user` (the
same probe as the outer step, plus six memoized pk probes). p50 µs per
statement, 200 ops per cell, two cells per binary; statements/s derived as
`1e6 / mean µs` (serial single-connection driver — the driver did not
report it directly):

| shape | BASE p50 (cell 1 / 2) | NEW p50 (cell 1 / 2) | NEW/BASE p50 | BASE ≈ stmts/s | NEW ≈ stmts/s |
|---|---|---|---:|---:|---:|
| loans-by-user | 1,134.9 / 1,134.2 | 587.0 / 588.6 | **0.52×** | 936 | 1,804 |
| count-by-user | 1,124.3 / 1,155.1 | 581.9 / 585.8 | **0.51×** | 1,118 | 2,127 |
| join-loan-user | 1,153.7 / 1,154.1 | 594.3 / 596.0 | **0.52×** | 968 | 1,885 |
| loans-by-user, **walk** (no-cabin cells) | 556.4 / 556.8 | 559.6 / 550.8 | 1.00× | 1,790 | 1,797 |

The within-binary replicate pairs on the cabined shapes disagree by
**0.0–2.7%**, so the halving is ~20× the floor under it. Against the walk
measured in the same run's no-cabin cells, same binary each side:

| miss ÷ walk (p50) | BASE (`d84fdc3`) | NEW (`a44c5cc`) |
|---|---:|---:|
| loans-by-user | 2.04× | **1.06×** |
| count-by-user | 2.06× | **1.05×** |
| join-loan-user | 2.03× | **1.06×** |

The chain, dated: §7 measured the inversion at **1.69×** the walk at
`9f762a3`; by `d84fdc3` it stood at **2.04×** — the Cabin side bridges the
two runs within 1.1% (936 against §7's 926/s), while the walk itself is
14.5% faster here than §7's 1,563/s, inside that run's 23.6% floor at this
size, so how much of the widening is engine and how much is run-to-run is
not resolvable across runs and this addendum's claims rest on its own
interleaved cells only. At `a44c5cc` the miss costs **+5.1–5.9%** over the
walk, consistently across all three shapes — the honest residual, about one
key comparison per walked row, the price of building the entry set rather
than of decoding for it. (`docs/feat-cabin.md` §4's amendment quotes ~14%
from the pre-A/B measurement chain; this run's interleaved number is the
5–6% above, and the spec's figure should be read as superseded.)

The full distributions, with the hit/miss mixture they carry:

| cell | shape | ops | p0 | p25 | p50 | p95 | p99 | mean |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| cab10k-base-1 | loans-by-user | 200 | 42.8 | 1,131.1 | 1,134.9 | 1,147.6 | 1,191.5 | 1,067.8 |
| cab10k-base-2 | loans-by-user | 200 | 41.4 | 1,130.0 | 1,134.2 | 1,165.5 | 1,208.4 | 1,069.9 |
| cab10k-new-1 | loans-by-user | 200 | 43.1 | 579.3 | 587.0 | 599.3 | 621.8 | 554.1 |
| cab10k-new-2 | loans-by-user | 200 | 37.6 | 581.6 | 588.6 | 603.1 | 624.0 | 554.3 |
| cab10k-base-1 | count-by-user | 200 | 37.8 | 1,118.1 | 1,124.3 | 1,137.6 | 1,144.0 | 878.6 |
| cab10k-base-2 | count-by-user | 200 | 37.2 | 1,146.2 | 1,155.1 | 1,179.9 | 1,411.6 | 909.6 |
| cab10k-new-1 | count-by-user | 200 | 29.4 | 571.3 | 581.9 | 617.4 | 649.9 | 463.8 |
| cab10k-new-2 | count-by-user | 200 | 37.4 | 572.9 | 585.8 | 727.7 | 774.9 | 476.4 |

The p0 of ~37–43 µs in every row is a **hit** — the ~5% of draws that
repeat a key within 200 ops over 2,000 users — and it is the same figure as
`pk-user`'s p50, the fixed client round-trip floor: a Cabin hit costs the
round trip and nothing measurable above it, on both engines. `count-by-user`
runs last, after `loans-by-user` and the join have observed ~400 keys, so
~21% of its probes hit — which is why its mean sits below its p25 on both
engines and why its derived stmts/s outruns `loans-by-user`'s. On waits:
single-connection reads, so no commit/fsync or lock wait exists in the
unit; it is round-trip floor (~38 µs) plus walk plus, at BASE, the decode
overhead the fix removed. No per-step timing exists to split the walk
further (`docs/observability.md`).

### 7a.4 The walk cells — the changed decode path costs nothing measurable

The rewritten decode (`partial` computation) runs for **every** plain
`FilterScan`, Cabin or not, so the overhead check is the no-cabin cells.
p50 ratio NEW/BASE of pair means, all ten shapes, 10,000 loans; the floor
is the widest within-binary replicate disagreement, dividing by the smaller
of each pair:

| shape | NEW/BASE | worst rep. floor |
|---|---:|---:|
| pk-user | −1.2% | 1.9% |
| loans-by-user | −0.3% | 1.6% |
| loans-by-book | −1.0% | 1.4% |
| resv-by-user | −2.0% | 0.2% |
| books-by-author | +6.4% | 10.0% |
| books-by-genre | +5.9% | 8.6% |
| loans-by-daterange | −1.6% | 0.7% |
| overdue | −0.5% | 1.3% |
| join-loan-user | −0.9% | 1.3% |
| count-by-user | +0.2% | 1.6% |

**No shape moves outside its floor.** The two +6% rows are one cell
(`none10k-new-1`) disagreeing with its own NEW replicate by 8.6–10.0% on
exactly those two shapes; the same two shapes run as plain `FilterScan`s in
the four cabin cells too, where four more cells put them at **+1.0% and
−0.5% against floors of ≤2.1%** — the corroboration that the +6% is that
cell's noise, not the decode change. Every other row sits within ±2% at a
±0.2–1.9% floor. Verdict: **the fix's cost to the plain walk is
unmeasurable at a ~2% floor**, with the two noisy rows bounded by ~10%.

### 7a.5 At 1,000 loans the ratio is the same — the residual is per-row

§7 recorded the inversion growing with the relation (1.69× at 10,000, 1.21×
at 1,000, inside the floor at 200), the signature of a per-row cost. The fix
should therefore hold the miss ratio flat across sizes, and one interleaved
pair at `--loans 1000` says it does. The in-cell walk yardstick is
`loans-by-book` — the identical statement shape on the same relation at the
same ~5 matches, cabinless — since this run carries no 1,000-row no-cabin
cell:

| `--loans 1000`, p50 µs | BASE | NEW |
|---|---:|---:|
| loans-by-user (CabinProbe, miss-heavy) | 147.8 | **94.2** |
| loans-by-book (FilterScan, the yardstick) | 90.6 | 89.0 |
| miss ÷ yardstick | 1.63× | **1.06×** |

1.06× at 1,000 and 1.06× at 10,000: the residual scales with the rows
walked, as one comparison per walked row must. At 1,000 loans the key space
is 200 users, so 200 ops repeat heavily — `count-by-user`'s p50 there is
37.6 µs, a *hit*, on both binaries, which is the hit-cost measurement
§7a.3 cites. One pair only, so no within-size floor exists at this size;
the two cells agree with the 10,000-row story rather than establishing
their own. **200 loans was not re-run**: §7 found the pre-fix inversion
already inside the floor there, and a fix to a per-row cost has nothing to
show at a size where the per-row cost was invisible.

### 7a.6 Versus PostgreSQL — still no twin, by design

§11's statement stands: there is no PostgreSQL equivalent of a structure
authoritative only for observed values, so the Cabin columns have no
PostgreSQL twin and none was invented for this run (`tools/pg_scenario3_library.py`
has no `--cabin`). The comparable pair is the walk: this run's
`FilterScan` at 1,790–1,797 stmts/s brackets §11's `ck-none` 1,563/s within
14.5% — inside that run's 23.6% floor at this size, TLS build difference
included — so §11's factor-level reads (walk ≈ 1.7× `pg-none`'s 910/s on
this shape) carry over, and nothing tighter than a factor is claimed across
the runs.

### 7a.7 What §7a changes, and what it does not

**Changed: what a miss costs.** A Cabin's recording miss now prices at the
walk plus ~6%, not the walk times two. The break-even repeat rate follows
directly, with this run's own numbers (walk 556 µs, miss 1,135 → 588 µs,
hit ≈ 39 µs): at BASE a key had to repeat on **~53% of probes** before the
Cabin beat doing nothing at this size; at NEW that threshold is **~6%**.
The run corroborates it from inside: at `loans-by-user`'s own ~5% observed
repeat rate, NEW's derived throughput (1,804/s) already sits level with the
walk (1,797/s, inside the floor), and at `count-by-user`'s ~21% it is
**1.18× ahead** — the sign §7 measured is gone at repeat rates this
workload actually produces.

**Not changed: when a Cabin is worth declaring.** §7's warm-keys analysis
stands whole — a Cabin's benefit is still a function of how often an
argument repeats, `scenario1`'s 15.3× on eight cycling arguments and this
workload's growing key space still bracket the answer, and the hit rate is
still unreported by the engine (§12). The `CABIN AUTO` threshold
(`docs/feat-cabin.md` §11, CLAUDE.md's open decision) remains open; what
this addendum moves is only where that threshold's economics start — a
structure that costs ~6% on a cold key and round-trip-floor on a warm one
is cheap enough to declare at far lower repeat rates than the pre-fix
engine justified. Also unchanged and not re-measured: the index columns
(§4–§6), which at 21,000+ stmts/s remain ~12× beyond what any miss-side
fix can reach; the durability matrix (§10); and everything else in this
file, which describes `9f762a3`.

## 7b. Addendum, 2026-08-19 — the join with no literal and no index: the Cabin, probed per outer row

§9b closed the no-literal join for an *indexed* join column, and its §9b.7
left the other half open by name: a join key on an unindexed non-pk column,
where IX17 has nothing to probe and IX3 refuses a heap relation an index at
all. **CB12** (`8f3f730`, `docs/feat-cabin.md` §4a) closes that half with
the other structure: a cabined join column bound by equality to an earlier
step's column probes the Cabin **per outer row**, the key read from the
frame (`CabinProbe::key_from`) instead of a compile-time literal — the same
shape as IX17 one trust class over, and the only acceleration an unindexed
join column can have. This addendum measures it by interleaved A/B, BASE
`6c5bb14` against NEW `8f3f730`, at `--index-mode none --cabin`, and
accounts for the serve-order bug the same commit fixed.

**The answers: the warm no-literal join's p50 falls from 8,570 µs to 67 µs
at 10,000 loans and 16 outer rows — 127× — because the inner side's
marginal cost falls from ~535 µs per outer row (one full scan of `loans`)
to ~1.7 µs (one Cabin serve); no driver shape moves outside the replicate
floor; and the correlated EXISTS improves only by inheriting keys other
statements observed, never by its own recording — §4a's non-convergence,
measured flat across 50 repetitions.**

### 7b.1 The A/B run

| | |
|---|---|
| executed | **2026-08-19 02:37:45 → 02:40:20 UTC**, 4 ckdbs cells, alternating BASE, NEW, BASE, NEW |
| branch / worktree | `worktree-enhence-join-perf`, in the worktree `enhence-join-perf` |
| commit measured | **`8f3f730`** (CB12), recorded by every cell, `dirty: false` |
| BASE binary | a **copy**, `sha256 7d8e57d09a849058…`, built fresh for this run from a temporary worktree at **`6c5bb14`** (removed after), linked 02:29:32 UTC — **byte-identical to §7a's NEW binary**, so §7a's tables chain directly to this run's BASE column. `6c5bb14` carries §7a's miss-decode fix, §9a's propagation and §9b's IX17 |
| NEW binary | a **copy**, `sha256 859ff728df5cadc5…`, from this worktree's `build-release/kds_server`, linked 02:21:33 UTC — 17 s *before* `8f3f730` was committed at 02:21:50; the tree was clean and `cmake --build` immediately before the run was a no-op against HEAD's sources |
| build | Release (`-O3 -DNDEBUG`), gcc 13.3.0, **`KDS_WITH_TLS=OFF`** both sides |
| device | ext4 on `/dev/root` (`df -T`; `/tmp` is ext4 on this host too); data files under `$HOME/bench-s3-cb12ab/db/`, WAL under `$HOME/bench-s3-cb12ab/wal/<cell>/`, binary copies under `$HOME/bench-s3-cb12ab/bin/` |
| server config | `cores = 1`, `durability = group`, `indexes = on`, port 15499. One server and one **fresh data file** per cell, started from the run's own copy |
| driver | `tools/scenario3_library.py --suffix s3 --loans 10000 --index-mode none --cabin --ops 200 --verify 25`, seed 1 (`users`/`books` 2,000, `reservations` 5,000) — the walk-vs-cabin configuration, no index anywhere |
| the join shapes | driven against each cell's already-loaded server by a session scratch harness (statements below, not checked in — §9b.7's fold-into-the-driver task now covers three shapes): the correlated EXISTS (50 ops), then the k-sweep **twice** — each k pays its first statement separately (the one that records unobserved keys) and then samples 50 — then the EXISTS again. Run 2 of the sweep is fully warm |
| contention control | every cell gated on `bench/wait_quiet.sh` (loadavg 0.54–0.58 at each start); `pgrep -c cc1plus` 0 before and after every cell. **All 4 cells passed on the first attempt; none discarded** |
| correctness | 4 × 21,004 driver ops, 0 errors, `verify_problems` empty in all 4. After the run, both cell-1 data files were re-mounted with their own binaries: the k=16 join reply is **byte-identical** between BASE's walk and NEW's serve (79 rows), and between NEW's own recording walk and its serve — the serve-order fix holding on real data (§7b.6) |

The swept statement, verbatim (k ∈ {1, 2, 4, 8, 16}; ids 1…k all exist),
and the EXISTS:

```
SELECT l.book_id, u.member_code FROM users_s3 AS u
  JOIN loans_s3 AS l ON l.user_id = u.id WHERE u.id BETWEEN 1 AND k

SELECT id FROM users_s3 WHERE EXISTS
  (SELECT l.id FROM loans_s3 AS l WHERE l.user_id = users_s3.id) LIMIT 20
```

**One size, deliberately.** This addendum runs at 10,000 loans only; the
swept axis is k, the outer-row count. The walk's proportionality to the
relation is already §9b's measured table (BASE 12.1 / 53.6 / 527 µs per
outer row at 200 / 1,000 / 10,000), and this run's BASE reproduces its
10,000-row point at ~535 µs; the serve side's independence of relation size
is *asserted from the structure* (a Cabin serve reads an entry set, not the
relation), not measured across sizes here.

### 7b.2 The plan, before and after

Captured by the harness at k=4 in every cell. BASE (`6c5bb14`) — no
literal to propagate, no index to probe, so the inner side is a full scan
per outer row:

```
step 1 Scan loans_s3 AS l   opens=4 examined=40000 matched=21 sel=0% pages=344
  filter 0:1.1 = 0:0.0
```

NEW (`8f3f730`), the same statement cold — the probe key is the outer
row's `u.id`, and the misses' recording walks are visible (one of keys 1–4
was already observed by the driver's shapes; seed 1 makes this identical in
both NEW cells):

```
step 1 CabinProbe loans_s3 AS l cabin=1 on=col1 key=0:0.0
       opens=4 examined=30005 matched=21 cabin_hits=1 cabin_misses=3 cabin_recorded=3
```

And warm, after the sweep: `examined=21 matched=21 sel=100% cabin_hits=4`
— 21 rows examined for 21 returned, the authoritative serve, against
BASE's 40,000.

### 7b.3 The no-literal join, BASE against NEW

p50 µs per statement, both cells shown, 50 sampled ops per point; BASE's
two sweep runs are statistically identical (it walks either way), NEW's
**run 2** (fully warm) is tabled; stmts/s derived as 1e6/p50 of the pair
mean (single serial connection — the harness did not report it directly):

| k | BASE p50 (c1 / c2) | NEW warm p50 (c1 / c2) | BASE ≈stmts/s | NEW ≈stmts/s | speedup |
|---:|---:|---:|---:|---:|---:|
| 1 | 584.5 / 583.3 | 43.5 / 44.5 | 1,713 | 22,730 | 13.3× |
| 2 | 1,117.8 / 1,113.5 | 45.4 / 46.1 | 896 | 21,860 | 24.4× |
| 4 | 2,185.9 / 2,171.4 | 48.5 / 50.4 | 459 | 20,220 | 44.1× |
| 8 | 4,285.3 / 4,282.9 | 53.0 / 54.7 | 233 | 18,570 | 79.6× |
| 16 | 8,545.5 / 8,593.9 | 67.2 / 67.3 | 117 | 14,870 | **127×** |

The replicate pairs disagree by 0.1–3.9% — floors the smallest factor in
the table clears thirtyfold. The marginal cost per outer row — the
k=8→16 slope, which excludes the fixed part (round trip plus the outer
range walk, ~42 µs on NEW, identical in both binaries):

| | BASE µs/outer row | NEW warm µs/outer row |
|---|---:|---:|
| slope, k=8→16 | 532.7 / 538.9 | **1.78 / 1.58** |
| p50 ÷ k at k=16 | 534.1 / 537.1 | 4.20 / 4.21 |

BASE pays one full scan of `loans` per outer row, §9b's 527 µs
reproduced. NEW's warm serve is **~1.7 µs per outer row, independent of
the relation by construction** — the third conversion of a per-row cost
into a fixed one in this file (§6 the equality, §9a/§9b the indexed join),
and the first available to a column no index can serve.

**What the first statement pays — the observation charge.** Run 1's
unsampled first statement at each k carries the recording walks for keys
not yet observed, and seed 1 makes it deterministic across both NEW cells:
k=8's first statement cost 2,295.8 / 2,286.0 µs (four ~560 µs recording
walks, keys 5–8), k=16's cost 2,316.2 / 2,307.3 µs (four more — the other
four of 9–16 had been observed by the driver). §4a's account, visible in
one number: a single join statement can push many keys past observation,
each at one miss walk (~1.06× the plain walk, §7a), and every repeat
thereafter serves at ~1.7 µs — so a key that repeats even **once** has
already paid for itself, where the pre-CB12 engine walked 535 µs every
time. Run 1's *sampled* p50s agree with run 2's within −6.2% to +1.5%
across both cells, with run 1 the **faster** side wherever they differ:
after the first statement the sweep is already warm, and the residue is
the box, not recording.

Distributions at k=16, run 2, all four cells (50 ops each):

| cell | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| cab10k-base-1 | 8,443.5 | 8,466.3 | 8,545.5 | 8,902.9 | 9,274.7 |
| cab10k-base-2 | 8,531.9 | 8,565.1 | 8,593.9 | 9,161.9 | 15,162.6 |
| cab10k-new-1 | 65.2 | 66.5 | 67.2 | 82.4 | 705.7 |
| cab10k-new-2 | 65.6 | 66.5 | 67.3 | 77.4 | 80.0 |

On waits: single-connection reads — no commit/fsync wait, no lock or
conflict wait exists in the unit. NEW's 67 µs decomposes as the ~42 µs
fixed part (round trip plus the 32-row outer range, measured at k→0 by the
intercept; `pk-user`'s p50 is 37–38 µs in the same cells) plus 16 serves
at ~1.7 µs; no per-step timing exists to split a serve further
(`docs/observability.md`). The two p99 outliers (one 15.2 ms BASE sample,
one 706 µs NEW sample, each a single draw) are scheduling noise on a 2-vCPU
box, not a path.

### 7b.4 The correlated EXISTS — the recorded limitation, measured

*(2026-08-19, later the same day: CB13 — `fa1f320` — closes this
limitation; §7c measures the convergence. This section stands as the BASE
side of that A/B and as the record of the pre-CB13 engine.)*

`docs/feat-cabin.md` §4a records that a correlated EXISTS over a cabined
column **re-observes without ever recording** when every probed outer key
has a qualifying match: the stopping sink halts the inner walk at its first
match, a partial walk cannot commit an authoritative set, so the statement
pays the recording setup per outer row and banks nothing. This run measures
exactly that. The EXISTS ran *before* the join sweep (pre) and *after* it
(post), 50 ops each; p50 µs:

| cell | pre p50 | pre first-10 mean | pre last-10 mean | post p50 |
|---|---:|---:|---:|---:|
| cab10k-base-1 | 1,699.2 | 1,699.1 | 1,693.6 | 1,698.5 |
| cab10k-base-2 | 1,392.5 | 1,399.8 | 1,388.4 | 1,391.4 |
| cab10k-new-1 | 932.0 | 933.6 | 931.7 | **176.1** |
| cab10k-new-2 | 941.4 | 1,203.7 | 935.1 | **175.2** |

Full pre-join distributions (50 ops): BASE p0/p25/p95/p99 =
1,682.6/1,693.7/1,722.4/2,062.4 (c1) and 1,381.0/1,389.9/1,408.7/1,429.6
(c2); NEW = 916.0/930.1/1,118.5/1,667.4 (c1) and 909.5/933.6/998.0/2,850.3
(c2). Post-join NEW: 173.3/175.0/188.7/189.0 (c1), 167.8/173.0/189.1/340.6
(c2).

Three readings, in honesty order:

- **The limitation is confirmed: the EXISTS never converges on its own.**
  On NEW the first-10 and last-10 sample means are flat across 50
  repetitions (933.6 → 931.7), and the counters say why: every one of its
  20 probed users has a loan, so `cabin_misses=14` walk on *every*
  execution and `cabin_recorded` never appears. The statement cannot warm
  itself, exactly as §4a records.
- **The pre-join improvement it does show is inherited, not earned.** NEW
  pre reads ~1.5–1.8× faster than BASE (pair means 936.7 vs 1,545.9;
  inner rows examined 15,248 vs 25,050 per statement) — but `cabin_hits=6`
  are six keys the *driver's* earlier shapes observed, serving at hit
  cost. BASE's own two cells disagree by 22% (1,699 vs 1,392 at identical
  `examined=25,050`, each internally replicating within 0.5% — a machine
  mode, not different work), so the factor is quoted at factor level only.
  It is an improvement, not a regression, and it is not the 16.5× an index
  gave the same statement in §9b.4.
- **Post-join, the inheritance is dramatic:** after the sweep recorded keys
  1–16, `cabin_hits=17 cabin_misses=3` and p50 falls to **176 µs** — the
  three remaining misses still walk every time, still flat. A cabined
  EXISTS is fast exactly insofar as *other* statements have observed its
  keys; alone, it stays at the walk's price forever. The narrow fix and
  why it is not narrow are in §4a: skipping recording under a stopping
  sink would also skip the completed walk that commits an authoritative
  **empty** set — the one case (a key with no matches) where this
  statement *can* record, and the case worth keeping.

### 7b.5 Every other shape — the correlated arm costs the rest of the workload nothing measurable

CB12 adds a selection arm to step compilation (after both index forms and
the literal Cabin) and the `key_from` path to the executor, so the claim
needing evidence is the null one, on the walk-and-literal-cabin
configuration this run measures. p50 NEW/BASE of pair means, all ten driver
shapes at 10,000 loans; the floor is the widest within-binary replicate
disagreement for the shape:

| shape | NEW/BASE | worst rep. floor |
|---|---:|---:|
| pk-user | −1.4% | 3.5% |
| loans-by-user | +0.0% | 1.3% |
| loans-by-book | +0.6% | 1.9% |
| resv-by-user | −1.4% | 1.9% |
| books-by-author | −5.8% | 17.2% |
| books-by-genre | −3.3% | 19.7% |
| loans-by-daterange | +0.2% | 0.8% |
| overdue | −0.3% | 1.1% |
| join-loan-user | +0.2% | 2.3% |
| count-by-user | −0.0% | 0.2% |

**No shape moves outside its floor.** Seven of ten sit within ±1.4% at
floors of 0.2–3.5%. The two book shapes repeat §7a.4's pattern exactly —
one BASE cell disagreeing with its own replicate by 17–20% on those two
shapes and no others — and their deltas sit well inside those floors.
`join-loan-user` is the row that was *required* not to move: its literal
lands on the cabined column by §9a's propagation on **both** binaries
(`CabinProbe … value=3`, a compile-time key, the recording-miss shape §7a
priced), so CB12's arm never fires for it; +0.2% against a 2.3% floor says
the added selection arm costs it nothing measurable. `loans-by-user` and
`count-by-user`, the other two miss-heavy Cabin shapes, are +0.0% and
−0.0% — the literal path is untouched.

### 7b.6 The serve-order bug — latent since v1, fixed with the feature

Found while building CB12, fixed in the same commit, and worth its own
account because it was **reachable by plain single-relation SQL** on every
pre-`8f3f730` engine: the Cabin serve emitted pks in **entry order**,
which stops being the walk's order after a write-hook append. An UPDATE
that moves an earlier pk into an observed value appends that pk at the
set's end (the superset invariant makes every mandatory action an append);
a subsequent serve then emitted rows in an order no walk of the relation
could produce, violating I12's within-step contract. It went unseen
because the original contract queries happened to filter every exposed set
to one row. The fix sorts the serve to the walk's order before emission,
**key-mode-conditional** per `docs/heap-and-tuple.md` §4.1: pk order for
an `ASSIGNED` relation, page-then-slot for `EXPLICIT`, whose
caller-supplied ids need not ascend. This run's evidence is §7b.1's replay
check — NEW's serve byte-identical to BASE's walk on the 79-row k=16 reply
— plus the driver's `--verify` (three invariants, 25 draws, all four
cells clean). The correctness suite was run in the CB12 build session, not
re-run in this measurement session.

### 7b.7 Versus PostgreSQL — no twin for the structure, and the indexed twin is §9b's

There is still no PostgreSQL equivalent of a value-observed authoritative
structure (§7a.6, §11), so the Cabin column has no twin and none was
invented. For this *statement shape* PostgreSQL's answer is an index, and
that comparison is already measured: §9b.6 puts PostgreSQL at 769.4 µs p50
on the identical k=16 statement (its optimizer's merge-join flip) against
ckdbs-with-index at 90.2 µs — and this run's warm cabin serves the same
statement at 67 µs on a column that *has* no index. What was not measured,
and would complete the picture, is PostgreSQL at `--index-mode none` on
this shape — its seq-scan-per-outer-row against BASE's walk; the twin
driver supports the mode, and that cell is the named task if the number is
ever wanted. *(Measured 2026-08-19 — §7e: six cells at three sizes. The
anticipated seq-scan-per-outer-row is a plan PostgreSQL never produces —
it hash-joins over one scan per statement, 1,314/s at k=16 and 10,000
loans against the walk's 117/s and the warm serve's 14,870/s.)*

### 7b.8 What §7b changes, and what it does not

**Changed: the unindexed join key has an answer.** §9's join story closes
its last open shape for repeating keys: the same no-literal join now costs
~3.3 µs per outer row with an index (§9b, `4f304fd`) and ~1.7 µs marginal
(~4.2 µs p50-per-outer-row at k=16) with a Cabin and no index (this run,
`8f3f730`) — different runs on different days, so the two figures compare
at factor level only, but they are the same order and both independent of
the relation's size, against a walk that pays ~535 µs *per outer row per
10,000 rows*. A heap relation's join column, which IX3 refuses an index,
now has exactly one acceleration, and it works.

**Not changed: the hit-rate economics, and who pays the observation.**
§7's closing analysis and §7a.7's break-even arithmetic stand whole — a
Cabin still profits only where values repeat, and CB12 *widens the
exposure*: one join statement can push up to `cabin_max_values` keys past
observation in a single execution (§4a's addition to §8's open budget),
each imposing the write-hook cost thereafter. A join whose outer keys
never repeat pays ~6% per key (§7a's miss surcharge) for nothing — **the
never-repeating-key distribution on an unindexed column remains the
uncovered case**, and it is a `CABIN AUTO` policy question
(`docs/feat-cabin.md` §11), not an executor one *(closed on the Cabin's
side later by CB14 — §7d measures it)*. Also unchanged: the
EXISTS non-convergence (§7b.4 above, recorded in §4a with the reason the
narrow fix is wrong) *(closed later the same day by CB13 — §7c measures
it)*; the `correlated_scans` counter blind spot (§4a: a still-quadratic
correlated CabinProbe reports zero); everything §7a.7 lists as not
re-measured; and the rest of this file, which describes `9f762a3`.

## 7c. Addendum, 2026-08-19 — the correlated EXISTS converges: sub-chain mode commits whole sets, and the caps bound the license

§7b.4 measured the limitation `docs/feat-cabin.md` §4a recorded: a
correlated EXISTS over a cabined column re-observed forever, because the
stopping sink halted every recording walk at its first match and a partial
walk may never commit a set (C1). **CB13** (`fa1f320`, §4a's closed
paragraph) removes the limitation without touching C1: inside a **sub-chain**
every stop is the sub-chain's own short-circuit — V09 refuses `LIMIT` at
subquery depth, so no stop there can be a quota — and a walk carrying a
live recording therefore runs on *through* the stop, visiting the remaining
rows for the recording block alone, and commits a whole set. Top-level
runners never get the mode, which is what keeps the quota's bounded-work
property. This addendum measures CB13 by interleaved A/B, BASE `82ff9b7`
(CB12, §7b's engine) against NEW `fa1f320`, eight cells at
`--index-mode none --cabin`, 10,000 loans. CB13's first candidate carried
a per-row cost §7c.5 records; the fix was amended into the commit before
it landed, and this run measures the commit as landed.

**The answers: the correlated EXISTS now converges — one observation-charge
execution at 6.4–9.3 ms, then a steady state of ~74 µs against BASE's flat
~979 µs, a 13.1× steady-state win with byte-identical replies; the
observation charge no longer bills the statement's row budget, and the caps
bound what the license can spend. The cost question §7c.5 owns closed the
other way at the landed commit: CB13's first candidate taxed every
walk-heavy shape a resolvable +2.9–4.0% (~2 ns per examined row), the
fix moved the completion exit inside the recording block and made the row
charge unconditional again, and at `fa1f320` all ten driver shapes read
inside their replicate floors. CB12's headline is untouched: the warm
no-literal join serves at 67–68 µs on both binaries.** *(CB14, later the
same day, moves the correlated observation to a key's second touch — the
charge lands on execution 2 and steady state on execution 3; §7d measures
the shifted series against this engine.)*

### 7c.1 The A/B run

| | |
|---|---|
| executed | **2026-08-19 05:18:48 → 05:38:52 UTC**, 8 ckdbs cells, alternating BASE, NEW ×3, then a fourth pair to pin two machine-mode replicates; replay checks by 05:41 UTC. Supersedes (rule 1a) the same morning's 6-cell sitting (04:54–04:59 UTC) against CB13's pre-amend candidate, whose one divergent finding §7c.5 records |
| branch / worktree | `worktree-enhence-join-perf`, in the worktree `enhence-join-perf` |
| commit measured | **`fa1f320`** (CB13 as landed, the §7c.5 fix amended in), recorded by every cell, `dirty: false` |
| BASE binary | a **copy**, `sha256 859ff728df5cadc5…`, built from a temporary worktree at **`82ff9b7`** (removed after) at 04:51 UTC, sha re-verified before this run — **byte-identical to §7b's NEW binary**, so §7b's tables chain directly to this run's BASE column |
| NEW binary | a **copy**, `sha256 93a35d33efdf64b9…`, from this worktree's `build-release/kds_server`, source mtime 05:10:56 — 25 s *before* `fa1f320` was committed at 05:11:21; a `cmake --build` at 05:12 recompiled nothing and left the hash unchanged, so the copy is HEAD's sources |
| build | Release (`-O3 -DNDEBUG`), gcc 13.3.0, **`KDS_WITH_TLS=OFF`** both sides |
| device | ext4 on `/dev/root` (`df -T`); data files under `$HOME/bench-s3-cb13ab/db/`, WAL under `$HOME/bench-s3-cb13ab/wal/<cell>/`, binary copies under `$HOME/bench-s3-cb13ab/bin/` |
| server config | `cores = 1`, `durability = group`, `indexes = on`, port 15499. One server and one **fresh data file** per cell, started from the run's own copy |
| driver | `tools/scenario3_library.py --suffix s3 --loans 10000 --index-mode none --cabin --ops 200 --verify 25`, seed 1 — §7b's configuration exactly |
| the measured statements | driven against each cell's already-loaded server by session scratch harnesses (statements below, not checked in — §9b.7's fold-into-the-driver task now covers them): the correlated EXISTS, **10 timed executions in a row** plus a closing `ANALYZE` (the convergence series), then the §7b k-sweep once, 50 sampled ops per k (the CB12 no-regression check) |
| contention control | every cell gated on `bench/wait_quiet.sh` (loadavg 0.18–0.68 at each start); `pgrep -c cc1plus` 0 before and after every cell. **All 8 cells passed on the first attempt; none discarded.** Three replicates ran in a shifted machine mode and are kept and named: `base-1` and `new-4` whole-cell fast, `new-3`'s loans phases ~3–20% high |
| correctness | 8 × 21,004 driver ops, 0 errors, `verify_problems` empty in all 8; every EXISTS execution's reply equal to its cell's first, all 80 executions; the closing `ANALYZE` counters identical across all four cells per binary. After the run, both cell-1 data files were re-mounted with their own binaries: the EXISTS reply (20 rows) and the k=16 join reply (79 rows) are **byte-identical** between BASE's walk and NEW's serve. The full Debug suite is 2384/2384 green at `fa1f320` from the CB13 build session; it was not re-run in this measurement session |

The measured statement, verbatim (every id 1…20 has at least one loan, so
on the pre-CB13 engine no probed key can ever record):

```
SELECT id FROM users_s3 WHERE id BETWEEN 1 AND 20 AND EXISTS
  (SELECT l.id FROM loans_s3 AS l WHERE l.user_id = users_s3.id)
```

**One size, deliberately, again.** As in §7b.1, this addendum runs at
10,000 loans only; the swept axis is the execution count. Convergence is a
per-key property — at 200 or 1,000 loans the same series would differ only
in the size of the observation charge, whose proportionality to the
relation is §9b's measured table (the walk at 12.1 / 53.6 / 527 µs per
outer row at 200 / 1,000 / 10,000) — and the steady state's independence
of relation size is the Cabin serve's, asserted from the structure as in
§7b.1.

### 7c.2 The plan, before and after

The closing `ANALYZE` of each series, identical across all four cells per
binary (seed 1). BASE (`82ff9b7`), the 10th execution — six keys inherited
from the driver's shapes serve, the other fourteen walk to first match and
bank nothing, forever:

```
step 0 Range users_s3 opens=1 examined=32 matched=20 sel=62% pages=2 range_stopped_early=1
step 1 CabinProbe loans_s3 AS l opens=20 examined=15248 matched=20 sel=0% pages=173
       sub_runs=20 cabin_hits=6 cabin_misses=14 cabin_entries=32 hint_hits=32
```

NEW (`fa1f320`), the 10th execution — every key serves, the short-circuit
landing on each set's first entry:

```
step 0 Range users_s3 opens=1 examined=32 matched=20 sel=62% pages=2 range_stopped_early=1
step 1 CabinProbe loans_s3 AS l opens=20 examined=20 matched=20 sel=100% pages=117
       sub_runs=20 cabin_hits=20 cabin_entries=97 hint_hits=97
```

15,248 inner rows per execution against 20. `cabin_entries` 32 → 97 is the
fix visible in the store: the fourteen keys the EXISTS could never commit
on BASE hold their ~4.6-row sets on NEW (65 entries committed by
execution 1).

### 7c.3 The convergence series, BASE against NEW

Per-execution latency in µs, all eight cells; every reply equal to its
cell's first:

| exec | base-1 | base-2 | base-3 | base-4 | new-1 | new-2 | new-3 | new-4 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1,109.8 | 1,061.7 | 1,192.4 | 1,060.0 | **6,408.5** | **6,461.4** | **7,235.0** | **9,324.8** |
| 2 | 988.4 | 970.5 | 976.6 | 967.3 | 99.2 | 103.4 | 106.6 | 104.8 |
| 3 | 968.0 | 1,126.0 | 967.1 | 967.7 | 92.6 | 82.7 | 99.3 | 74.6 |
| 4 | 972.4 | 958.8 | 965.7 | 951.1 | 77.1 | 76.7 | 81.4 | 81.4 |
| 5 | 971.5 | 959.5 | 964.8 | 957.5 | 74.9 | 73.7 | 78.0 | 67.8 |
| 6 | 970.6 | 1,136.4 | 962.1 | 964.6 | 73.7 | 73.9 | 76.1 | 65.7 |
| 7 | 959.9 | 959.5 | 963.7 | 955.0 | 73.1 | 72.6 | 74.2 | 65.0 |
| 8 | 951.6 | 960.0 | 948.4 | 956.1 | 73.3 | 73.0 | 74.4 | 63.2 |
| 9 | 949.7 | 1,124.3 | 967.2 | 959.9 | 71.9 | 72.8 | 74.3 | 65.3 |
| 10 | 1,024.3 | 961.6 | 968.8 | 964.2 | 71.9 | 72.6 | 74.6 | 62.6 |

This is a convergence series, not a distribution — ten ordered executions
per cell is the unit of evidence, so no percentiles are tabled over it.
The steady state (executions 3–10, mean; ≈stmts/s derived as 1e6/mean,
single serial connection — the harness reports latency, so the throughput
is derived, per this file's matrix rule):

| | BASE (c1 / c2 / c3 / c4) | NEW (c1 / c2 / c3 / c4) |
|---|---:|---:|
| steady mean µs | 971.0 / 1,023.3 / 963.5 / 959.5 | 76.1 / 74.8 / 79.0 / 68.2 |
| pooled mean µs | 979.3 | 74.5 |
| ≈stmts/s (derived) | 1,021 | 13,423 |

**Steady-state ratio: 13.1× pooled, 12.2–14.1× per cell.** BASE is flat —
execution 10 is statistically execution 2, and the counters say why
(`cabin_misses=14` on every execution, `cabin_recorded` never appears);
base-2 alone shows a periodic ~165 µs bump on executions 3/6/9, a machine
mode, not a path. NEW pays execution 1, takes one intermediate execution
(~99–107 µs — the first serve of the fresh sets; the residue over steady
is a single ~25 µs step, present in all four cells, unattributed beyond
first-serve hint and page-cache warming), and is steady from execution 3
at ~74 µs. Cells 1–3 replicate within 5.6%; new-4 runs its whole cell
fast (68.2 µs steady, and its k-sweep in §7c.6 shows the same mode), so
the spread across all four is 15.8% — modes, not the path, since every
cell's counters are identical. The NEW floor of the statement is the fixed
part: ~42 µs of round trip plus the 32-row outer range (§7b.3's
intercept), plus 20 serves.

### 7c.4 The observation charge, and who pays it

Execution 1 on NEW costs 6,408–9,325 µs — **14 recording walks of the
whole relation**, ~460–670 µs per newly observed key (the spread is
new-4's one high draw; its steady state is the run's fastest), run
*through* each sub-chain's stop: ~140,000 inner rows visited against
BASE's 15,248, the difference (~125,000 rows) being completion rows the
license alone visits. It is §7b.3's observation-charge account with the
sign §4a wanted: per key, ~500 µs once against ~65 µs saved on every
subsequent execution (BASE's short-circuit walks ~1,089 rows per
unobserved key on average), so this statement repays its own charge
inside six to ten repetitions (per cell: 5.9 / 5.7 / 6.8 / 9.3) — and a
*join* probing the same keys repays it faster still, since §7b's shape
saves a full ~535 µs walk per key per statement. The charge is priced at
the walk it licenses, not at the short-circuit it replaces.

**Completion rows do not charge the statement's row budget.** The rows a
licensed walk visits past the stop are the Cabin's work, not the
statement's answer: charging them made the cabined side of the review's
reproduction answer `ResourceExhausted` where the cabin-free side answered
rows — an accelerator changing a result, which `docs/feat-cabin.md` §1
forbids. The uncharged work is bounded per key: a commitable set completes
once ever, and the caps stop the doomed forms before they walk.

The caps bound the license on both sides: a value the per-cabin value cap
could never admit is refused by `MayObserve` *before* a walk is paid for,
counted in `cap_refusals`. A set that outgrows the per-value entry cap
mid-walk revokes the license there — the walk ends at the next stop, the
commit guard refuses the partial set, and the key is marked sticky so an
append-only set that can only grow is never re-attempted (`Unobserve` and
the sighting table's reset are the clears).

### 7c.5 The candidate's per-walk tax — found, fixed by ordering, re-measured to the floor *(amended 2026-08-19)*

The expectation going in was §7b.5's: no driver shape moves outside its
floor, because the driver's shapes are literal probes and scans on paths
CB13 "does not touch". **The first candidate broke that expectation, and
the fix restored it before the commit landed.** This section records both
halves, because the finding is what the landed ordering is *for*.

**The finding (2026-08-19, morning, against the pre-amend candidate).**
Six shapes resolved outside their floors — exactly the six whose measured
statement walks the 10,000-row `loans` relation — at +2.9–4.0%, a
consistent **1.8–2.4 ns per examined row** (+17 to +24 µs over 10,000
rows). The uncabined `overdue`, a plain `FilterScan` with no Cabin on
either column, read +2.9% against a 0.8% floor: the walk loop itself, not
the Cabin arm. The shift was a body shift — p0 through p95 moving
together — which is what a per-row cost looks like.

**The cause.** The candidate exempted completion rows from the statement's
row budget by *branching before the charge*: `ChargeRow` in
`AcceptTupleAt` sat behind an `if (!stopped_)` test on the generic
per-examined-row path. Every walking statement in the engine paid that
predicate on every row, cabined or not, to buy an exemption only a
licensed completion walk ever uses.

**The fix, an ordering and nothing more.** The completion exit moved
*inside* the `recording_here` block — sound because `stopped_` can only be
true in `AcceptTupleAt` while this step's own recording is live: the walk
visitor and both phase-2 loops gate on `stopped_` otherwise, and a stopped
recording walk never descends, so no other step's `AcceptTupleAt` runs at
all. `ChargeRow` sits below that exit, unconditional again on the plain
walk. Completion rows stay uncharged (the exit still precedes the charge),
the EXISTS convergence semantics are untouched, and the fix was amended
into the CB13 commit — `fa1f320` is the landed form, and the full Debug
suite is 2384/2384 green on it.

**The re-measurement, this run's eight cells.** p50 µs over four
replicates per binary, delta of means against the widest within-binary
replicate spread:

| shape | BASE p50 ×4 | NEW p50 ×4 | Δ% | BASE floor | NEW floor | outside? |
|---|---:|---:|---:|---:|---:|---|
| pk-user | 27.7/37.6/38.4/37.5 | 37.7/37.4/38.9/38.1 | +7.7% | 38.6% | 4.0% | no |
| loans-by-user | 595.8/599.1/597.2/597.1 | 603.7/596.8/732.9/601.1 | +6.1% | 0.6% | 22.8% | no |
| loans-by-book | 560.2/564.2/560.5/564.0 | 564.5/564.0/610.0/564.8 | +2.4% | 0.7% | 8.2% | no |
| resv-by-user | 295.9/296.6/346.1/300.0 | 297.2/297.2/314.3/296.9 | −2.7% | 17.0% | 5.9% | no |
| books-by-author | 146.9/156.4/156.9/162.4 | 156.7/158.2/159.6/157.1 | +1.4% | 10.6% | 1.9% | no |
| books-by-genre | 168.3/176.5/176.2/183.1 | 176.1/177.7/181.9/178.9 | +1.5% | 8.8% | 3.3% | no |
| loans-by-daterange | 708.2/714.9/708.0/714.8 | 707.3/708.1/732.1/712.1 | +0.5% | 1.0% | 3.5% | no |
| overdue | 772.8/772.2/767.0/763.3 | 781.6/773.8/778.0/770.4 | +0.9% | 1.2% | 1.5% | no |
| join-loan-user | 603.7/599.4/602.9/604.5 | 608.4/600.7/622.8/606.5 | +1.2% | 0.9% | 3.7% | no |
| count-by-user | 603.2/587.8/597.0/591.0 | 590.5/597.1/596.6/601.3 | +0.3% | 2.6% | 1.8% | no |

**All ten shapes read inside their floors, the six walk-heavy ones
included.** Two rows need their floors explained rather than trusted:
`loans-by-user` and `loans-by-book` carry a NEW floor inflated by new-3,
whose loans phases ran ~3–20% high whole-phase (the recurring
single-replicate machine mode §7a.4 and §7c.3 both see; its `overdue` and
`count-by-user` are normal, and pair 4 was added to pin it). Excluding
that one replicate, the loans rows read **+0.5% and +0.4% against
0.6–1.2% floors** — the candidate's +4.0%/+3.2% on the same rows is gone,
not hidden under a wide floor. The tightest witnesses need no exclusion:
`overdue` +0.9% (floors 1.2/1.5%), `loans-by-daterange` +0.5% (1.0/3.5%),
`count-by-user` +0.3% (2.6/1.8%). `overdue`'s paired per-cell deltas are
consistently positive (+1.6 to +11.0 µs), so a residual of order 0.5 ns
per examined row may survive the fix — but at 0.9% under a 1.2% floor it
does not resolve, and per this file's rules it is not claimed. The full
shape of the two heaviest rows, since a delta table hides it (ops = 200
per cell per shape):

| cell | shape | p0 | p25 | p50 | p95 | p99 |
|---|---|---:|---:|---:|---:|---:|
| base-1/2/3/4 | overdue | 684.7/688.1/688.6/680.8 | 759.8/760.6/756.9/753.8 | 772.8/772.2/767.0/763.3 | 846.6/794.9/788.1/787.2 | 1,050.8/1,047.3/804.4/1,179.7 |
| new-1/2/3/4 | overdue | 677.5/691.6/694.2/684.1 | 767.9/760.2/763.3/759.8 | 781.6/773.8/778.0/770.4 | 809.3/791.0/801.2/804.1 | 819.3/817.1/897.1/1,385.5 |
| base-1/2/3/4 | loans-by-user | 33.9/43.1/42.8/39.8 | 587.1/590.1/589.8/590.1 | 595.8/599.1/597.2/597.1 | 624.4/665.7/613.5/626.3 | 1,200.3/963.4/659.7/668.7 |
| new-1/2/3/4 | loans-by-user | 42.5/41.9/44.1/41.5 | 595.4/588.7/724.4/594.6 | 603.7/596.8/732.9/601.1 | 626.8/616.5/778.1/623.3 | 668.0/874.4/907.8/668.1 |

new-3's p25/p50/p95 move together at ~+130 µs while its p0 and its other
shapes do not — the whole body of one replicate shifted, which is what a
machine mode looks like; the candidate's tax was a +15–25 µs body shift
*in every replicate*, and no replicate of this run shows that signature.
The load phase (19,000 inserts) reads +0.3% at 11–13% floors — nothing.
Wait accounting for the walk shapes is §3's unchanged: single-connection
reads, no commit or lock wait in the unit; any delta is server CPU inside
the walk.

**The trade, restated for the landed commit: convergence for the
correlated shapes costs the plain walk nothing this run can resolve.**
The candidate's ~2 ns/row tax was real, measured, and is the reason the
completion exit lives where it does; the hoisting question the earlier
version of this section raised is answered by the ordering itself.

### 7c.6 The CB12 headline stands, and inheritance now flows both ways

The §7b k-sweep, run once per cell after the EXISTS series. Warm k=16,
all eight cells, 50 ops each:

| cell | first µs | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|
| base-1 | 2,452.5 | 54.0 | 55.2 | 55.9 | 79.3 | 145.2 |
| base-2 | 2,426.0 | 56.2 | 58.7 | 65.3 | 85.3 | 742.9 |
| base-3 | 2,363.7 | 63.4 | 67.3 | 68.4 | 79.7 | 83.1 |
| base-4 | 2,345.5 | 62.7 | 67.1 | 68.1 | 78.4 | 95.4 |
| new-1 | 79.3 | 63.4 | 66.9 | 67.6 | 76.9 | 83.5 |
| new-2 | 78.4 | 64.7 | 66.4 | 67.2 | 77.1 | 80.4 |
| new-3 | 85.8 | 69.2 | 70.9 | 71.6 | 82.0 | 85.1 |
| new-4 | 105.0 | 56.8 | 58.2 | 59.1 | 79.5 | 86.0 |

Warm p50 means 64.4 (BASE) against 66.4 (NEW): **+3.0% inside 21–22%
floors — §7b's 67 µs serve and its 127× headline stand.** The wide floors
are the two whole-cell fast modes this run's stamp names — base-1 and
new-4 ran their entire sweeps ~15% fast at every k (base-1's k=1 p50 is
34.9 against 44–46 on every other cell, new-4's 35.5 the same), one per
binary, so they cancel in the mean; the four mode-free cells cluster at
65.3–68.4 (BASE) against 67.2–71.6 (NEW). The marginal cost per outer
row (k=8→16 slope of p50) is 1.53–1.86 µs on BASE against 1.56–1.95 µs
on NEW — the same number within a spread the modes set. The serve does
not walk, so no §7c.5 ordering can reach it. The *first* statements are
the standing evidence: BASE's k=8 and k=16 firsts pay ~2.3–2.5 ms of
top-level recording walks, §7b.3's observation charge reproduced — NEW's
firsts are already warm (53.8–105.1 µs at every k), because the EXISTS
ran first and committed keys 1–20. §7b.4 measured inheritance flowing one
way only (the EXISTS served keys other statements observed, earning
nothing itself); CB13 closes the asymmetry — a sub-chain's own walks now
stock the store for the join that follows.

### 7c.7 Versus PostgreSQL — unchanged, and still no twin

§7b.7's position stands whole: no PostgreSQL structure answers a
value-observed authoritative store, so the Cabin column has no twin and
none was invented; PostgreSQL's answer to this statement shape is an
index (a hashed semi-join over it), measured against ckdbs-with-index in
§9b. The named task is also unchanged: PostgreSQL at `--index-mode none`
on the correlated shapes — its seq-scan-per-outer-row against BASE's walk
— is the cell that would complete the picture, and the twin driver
supports the mode. *(Measured 2026-08-19 — §7e: on the EXISTS PostgreSQL
is flat at 681/s, behind even this run's BASE at 1,021/s, and 19.7×
behind NEW's converged 13,423/s.)*

### 7c.8 What §7c changes, and what it does not

**Changed: the last non-converging cabined shape converges, and C1 was
never bent to get it.** The commit guard still refuses every partial set;
what changed is that a sub-chain's licensed walk no longer *produces*
partial sets, because it finishes the relation before the guard asks. The
EXISTS paragraph of §7b.4, and §7b.8's "EXISTS non-convergence" line,
describe the BASE side of this table and are superseded as descriptions
of HEAD. Nearly changed, and caught: CB13's first candidate priced every
walking statement ~2 ns per examined row — the first CB-series change to
resolve above this workload's floor — and §7c.5 records the finding, the
one-ordering fix amended into the commit, and the re-measurement that
puts every walk shape back inside its floor at `fa1f320`.

**Not changed:** §8's per-key economics — the observation is still paid,
now once even for a stopping shape, and the never-repeating-key
distribution remains the uncovered case (`CABIN AUTO`, §11) *(closed on
the Cabin's side the same day by CB14 — §7d)*; the
`correlated_scans` blind spot (§4a — a still-quadratic correlated
CabinProbe still reports zero, though fewer shapes now stay quadratic);
the serve path and its §7b numbers; the indexed answers (§9a/§9b); the
sticky mark is per-store-lifetime with `Unobserve` and the sighting reset
as the only clears, so a key refused at the entry cap stays refused until
policy says otherwise — a §11 question, not measured here; and the rest
of this file, which describes `9f762a3`.

## 7d. Addendum, 2026-08-19 — the correlated probe earns observation per key: sight, then charge, then serve

§7b.8 and §7c.8 both left the same case uncovered: a join whose outer
keys never repeat paid the observation surcharge for nothing, and one
SELECT could push up to `cabin_max_values` keys into observation — each a
dead set carrying a standing write-hook cost for a key nobody would probe
again. **CB14** (`8420242`, `docs/feat-cabin.md` §4a and §8.1 amended)
closes it at the admission seam: the correlated probe now takes the
`n = 2` threshold whatever the Cabin's declaration says, because a
declaration is evidence about the value the operator *named* — the
literal shape — and says nothing about a value a walk produced. Per key:
the first touch costs one sighting insert and records nothing; a
repeating key records on its second touch and serves from its third; the
literal probe's `n = 1` is untouched. This addendum measures CB14 by
interleaved A/B — BASE **`e9531ce`** (§7c's engine plus the
`correlated_scans` counter fix) against NEW **`8420242`** — and this run
is the file's record of it on `worktree-enhence-join-perf`.

**The answers: the EXISTS convergence shifts exactly one execution —
sight at ~969 µs (the pre-CB13 cost, paid once), charge at ~6.4 ms,
steady from execution 3 at ~79 µs against BASE's ~77, the same steady
state one execution later. Sixteen keys touched exactly once grow the
store by 11 sets and 49 entries on BASE and by nothing on NEW — and NEW's
first touch is ~9.6% *cheaper*, because declining to record 11 dead sets
saves ~56 µs per set on the very walk that would have built them. The
literal path is untouched at the strongest level this run can state:
after 21,004 identical driver ops, the store's four counters are
byte-equal across the two engines, and every captured reply — 74-row
single-shot, EXISTS, all five sweep widths — is byte-identical across all
eight cells.**

### 7d.1 The A/B run

| | |
|---|---|
| executed | **2026-08-19 07:34:35 → 07:40:57 UTC**, 8 ckdbs cells, alternating BASE, NEW ×3, then a replacement pair after one NEW cell was discarded for contamination (below) |
| branch / worktree | `worktree-enhence-join-perf`, in the worktree `enhence-join-perf` |
| commit measured | **`8420242`** (CB14), recorded by every cell, `dirty: false` |
| BASE binary | a **copy**, `sha256 e4d96806…`, built at 07:33:27 from a temporary worktree at **`e9531ce`** (clean, removed after) — §7c's NEW engine plus the counter-only `correlated_scans` fix, so §7c's tables chain to this run's BASE column |
| NEW binary | a **copy**, `sha256 a20d2cbf…`, from this worktree's `build-release/kds_server`, source mtime 07:26:14 — 28 s *before* `8420242` was committed at 07:26:42; a `cmake --build` at 07:29 recompiled nothing and left the hash unchanged, so the copy is HEAD's sources |
| build | Release (`-O3 -DNDEBUG`), gcc 13.3.0, **`KDS_WITH_TLS=OFF`** both sides |
| device | ext4 on `/dev/root` (`df -T`); data files under `$HOME/bench-s3-cb14ab/db/`, WAL under `$HOME/bench-s3-cb14ab/wal/<cell>/`, binary copies under `$HOME/bench-s3-cb14ab/bin/` |
| server config | `cores = 1`, `durability = group`, `indexes = on`, port 15499. One server and one **fresh data file** per cell, started from the run's own copy |
| driver | `tools/scenario3_library.py --suffix s3 --loans 10000 --index-mode none --cabin --ops 200 --verify 25`, seed 1 — §7b/§7c's configuration exactly |
| the measured statements | against each cell's loaded server, in order: `SHOW CABINS`, the **single-shot never-repeating probe** (the §7b join over `u.id BETWEEN 41 AND 56` — 16 keys, each touched exactly once — executed **once**), `SHOW CABINS` again; the correlated EXISTS **8 timed executions** plus `ANALYZE`; the §7b k-sweep once at 3 sampled ops per k (a control, not this run's serve measurement — §7c.6 owns that at 50 ops); one reply capture per cell; a final `SHOW CABINS` |
| contention control | every cell gated on `bench/wait_quiet.sh` (loadavg 0.39–0.67 at starts); `pgrep -c cc1plus` 0 before and after all 8 cells. **`new-2` is discarded**: every phase of its cell ran 2–4× wide (single-shot 12,908 µs against 5,857–5,891 on its siblings, EXISTS steady oscillating 260–1,533 µs, four driver shapes +30–50%) with nothing in the gates to show for it — sar reports no steal, the journal shows Azure's `collect-logs` scope only before its window — yet its counters, store snapshots and replies are byte-identical to the other NEW cells: a machine mode, not a path, replaced by the `base-4`/`new-4` pair. Named modes kept: `base-4` ran whole-cell high (EXISTS charge 7,743 against 6,356–6,535, single-shot 7,537), and `overdue` ran +80 µs across its whole distribution in `new-1`/`new-4` but not `new-3` (§7d.4) |
| correctness | 8 × 21,004 driver ops, 0 errors, `verify_problems` empty in all 8; every EXISTS reply equal to its cell's first, all 64 executions; the closing `ANALYZE` identical across all clean cells of both binaries (`cabin_hits=20 cabin_entries=97 hint_hits=97`). Cross-binary: the single-shot's 74-row reply and the captured EXISTS + k=1/2/4/8/16 replies are **byte-identical across all eight cells**. The Debug test suite was **not executed in this measurement session** (no engine code changed here; CB14 landed with its own suite run) |

**One size, deliberately, again.** As in §7b.1 and §7c.1 this addendum
runs at 10,000 loans only; the swept axes are *touches per key* (one, for
the sixteen single-shot keys; two-then-more, for the EXISTS keys) and
*executions* (eight). What CB14 changes is which touch records — a count,
not a row cost: the store-growth evidence below is in keys and entries
and would read identically at 200 or 1,000 loans, while the observation
charge's proportionality to relation size is §9b's measured walk table
(12.1 / 53.6 / 527 µs per outer row at 200 / 1K / 10K), unchanged by
when the charge lands. The k-sweep control sweeps the outer cardinality
1→16 as before.

### 7d.2 The convergence series — shifted one execution, to the same steady state

The §7c statement, verbatim, eight executions per cell. BASE (CB13)
charges on execution 1 and serves from 2; NEW sights on 1, charges on 2,
serves from 3. Per-execution latency in µs, the seven clean cells:

| exec | base-1 | base-2 | base-3 | base-4 | new-1 | new-3 | new-4 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 6,355.7 | 6,382.3 | 6,534.5 | 7,742.7 | **971.8** | **955.2** | **979.5** |
| 2 | 101.1 | 103.0 | 101.5 | 121.8 | **6,394.6** | **6,390.2** | **6,359.4** |
| 3 | 84.3 | 80.1 | 80.5 | 76.7 | 100.1 | 96.7 | 99.9 |
| 4 | 77.7 | 77.0 | 74.8 | 68.4 | 78.3 | 79.5 | 79.2 |
| 5 | 76.5 | 75.8 | 72.3 | 68.1 | 75.5 | 75.9 | 76.4 |
| 6 | 74.9 | 75.9 | 73.2 | 79.3 | 74.1 | 74.5 | 74.4 |
| 7 | 74.4 | 72.9 | 73.0 | 89.3 | 72.9 | 73.0 | 73.0 |
| 8 | 73.1 | 72.7 | 71.4 | 93.0 | 73.0 | 74.9 | 73.4 |

A convergence series, not a distribution — no percentiles over ordered
executions. The steady state (executions 3–8, mean; stmts/s derived as
1e6/mean, single serial connection, per this file's matrix rule):

| | BASE (c1 / c2 / c3 / c4) | NEW (c1 / c3 / c4) |
|---|---:|---:|
| steady mean µs | 76.8 / 75.7 / 74.2 / 79.1 | 79.0 / 79.1 / 79.4 |
| pooled mean µs | 76.5 | 79.2 |
| ≈stmts/s (derived) | 13,072 | 12,626 |

The pooled steady delta, +3.5%, sits inside BASE's own 6.6% replicate
spread — not a finding. The shift is exact and the sum decomposes (each
execution is ~42 µs of client/socket round trip plus the 32-row outer
range — §7b.3's intercept — plus the probe work; no commit/fsync wait,
these are read statements on one connection, and no lock wait exists to
account for): **NEW's execution 1** costs 969 µs pooled — 6
driver-inherited keys serve, 14 miss into sighting inserts with the
short-circuited prefix kept (~1,089 inner rows per missed key, §7c.4's
account) — which is precisely the cost the pre-CB13 engine paid on
*every* execution (§7c.3's flat ~979 µs), paid once here. **Execution 2**
is §7c.4's observation charge, unchanged in size (6,359–6,395 against
BASE's 6,356–6,535 on the mode-free cells): 14 recording walks run
through the stop. Execution 3 carries the same ~25 µs first-serve residue
§7c.3 noted, and from there the two engines are the same machine. CB14
delays the charge by one execution and one short-circuit pass (~890 µs);
it never re-prices it.

### 7d.3 The store after sixteen keys touched once — what BASE keeps and NEW declines

The single-shot probe is the §7b join over `u.id BETWEEN 41 AND 56`,
executed exactly once per cell against the freshly driven store: sixteen
correlated keys, each touched once — the never-repeating distribution
distilled. Five of the sixteen were already observed sets (the driver's
literal phases had named them; hits 87→92 on both engines), so eleven
keys arrive genuinely unseen. `SHOW CABINS` (`observed/entries/hits/
misses` — counts, no percentiles), identical across **every** cell of
its binary including the discarded one (the mode moved time, never
state):

| snapshot (after) | BASE `e9531ce` | NEW `8420242` |
|---|---|---|
| the driver's 21,004 ops | 539 / 2,709 / 87 / 539 | **539 / 2,709 / 87 / 539** — byte-equal |
| + single-shot, 16 keys × 1 touch | **550 / 2,758** / 92 / 550 | **539 / 2,709** / 92 / 550 |
| + EXISTS ×8 (keys 1–20) | 564 / 2,823 / 258 / 564 | 553 / 2,774 / 244 / 578 |
| + k-sweep and captures (final) | 564 / 2,823 / 390 / 564 | 553 / 2,774 / 376 / 578 |

**BASE records 11 sets and 49 entries for keys it will never be asked
again; NEW records nothing** — its 11 misses are the sighting inserts,
its store numerically unmoved. Each of BASE's 11 sets is a standing
obligation: every future write of a `loans` row carrying one of those
`user_id`s pays the write hook to maintain a set nobody reads (§8's
economics), and the 49 entries count against the same `cabin_max_values`
/ entry budgets §7b.8 warned one SELECT could flood. That flood is now
bounded to intra-statement repeats — this statement, sixteen keys, bought
zero. The EXISTS rows show the repeating side pays as before: both
engines record the same 14 sets and 65 entries for keys 1–20, NEW one
execution later and 14 sighting-misses richer, converging on stores that
differ by exactly the once-touched keys.

The single-shot's own latency makes the finer point — one execution per
cell by design (a first touch happens once per data file), so these are
single timings, not distributions:

| cell | BASE µs | cell | NEW µs |
|---|---:|---|---:|
| base-1 | 6,496.3 | new-1 | 5,856.8 |
| base-2 | 6,528.1 | new-3 | 5,864.0 |
| base-3 | 6,449.4 | new-4 | 5,891.3 |
| base-4 | 7,536.6 † | | |

† base-4's whole-cell high mode (its EXISTS charge reads +18% the same
way). Against the three mode-free BASE cells (spread 1.2%; NEW's 0.6%):
**NEW is 9.6% faster on the first touch itself** — 6,491 µs against
5,871 — because the walk no longer builds what it records: ~56 µs per
declined set (620 µs / 11 keys) of entry inserts and set commits. CB14
is not a trade of first-touch cost against later convergence: the
never-repeating statement is cheaper *and* the store stays clean; the
only price is borne by genuinely repeating keys, whose charge arrives
one execution later (§7d.2). The 74-row reply is byte-identical across
all eight cells — sighting against recording never changes an answer.

### 7d.4 Every shape inside floors — and the literal path untouched twice over

The overhead question: does the per-key threshold test (one branch and a
`key_from` check per probe) or the sighting traffic cost the rest of the
workload anything? Ten driver shapes, 200 timed ops each, p50 per cell
with replicate floors, delta of means, and throughput derived as 1e6/p50
(single serial connection, per the matrix rule):

| shape | BASE p50 µs (c1/c2/c3/c4) | NEW p50 µs (c1/c3/c4) | Δ means | BASE floor | NEW floor | BASE q/s | NEW q/s |
|---|---:|---:|---:|---:|---:|---:|---:|
| pk-user | 36.8/38.4/38.6/38.1 | 38.2/38.6/37.6 | +0.4% | 4.9% | 2.7% | 26,333 | 26,224 |
| loans-by-user | 603.3/599.5/633.3/598.3 | 594.4/599.0/594.9 | −2.1% | 5.8% | 0.8% | 1,643 | 1,678 |
| loans-by-book | 561.7/561.1/593.7/558.9 | 556.8/561.7/557.1 | −1.8% | 6.2% | 0.9% | 1,758 | 1,790 |
| resv-by-user | 296.8/295.8/298.7/297.0 | 286.6/295.3/292.4 | −1.9% | 1.0% | 3.0% | 3,366 | 3,431 |
| books-by-author | 155.7/154.7/155.7/154.6 | 155.0/155.6/154.1 | −0.2% | 0.7% | 1.0% | 6,444 | 6,456 |
| books-by-genre | 176.0/174.4/176.0/176.2 | 177.0/175.4/177.1 | +0.5% | 1.0% | 1.0% | 5,693 | 5,666 |
| loans-by-daterange | 710.4/720.2/740.0/712.3 | 709.3/705.4/714.0 | −1.5% | 4.2% | 1.2% | 1,387 | 1,409 |
| overdue | 768.1/770.7/796.8/768.7 | 854.3/768.1/852.7 | +6.3% | 3.7% | 11.2% | 1,289 | 1,212 |
| join-loan-user | 607.9/596.4/631.6/600.0 | 598.7/605.5/601.5 | −1.2% | 5.9% | 1.1% | 1,642 | 1,661 |
| count-by-user | 592.2/594.1/594.7/594.6 | 588.7/605.3/590.0 | +0.1% | 0.4% | 2.8% | 1,684 | 1,682 |

**No shape resolves outside its floors; nine of ten sit within ±2.1%.**
`overdue` is the one to look hard at, and it survives the look: its +6.3%
sits inside an 11.2% NEW floor that is wide for a stated reason — in
`new-1` and `new-4` the *entire* distribution shifted +80 µs (p0 774–779
against 684–706 everywhere else) while `new-3` sits exactly on the BASE
distribution (p0 688, p50 768, from the same binary), so the shift is a
per-process machine mode, not a path: one binary cannot alternate code
paths between identical cells. Mechanism agrees — `overdue` walks `loans`
with no equality on the cabined column, so no CabinProbe and no CB14
branch runs in it.

The stronger form of "the literal probe is untouched" is in §7d.3's first
table row: after 21,004 driver ops — 626 literal cabin probes among them,
539 distinct keys recorded at `n = 1`, 87 repeat hits — **both engines'
stores read 539 / 2,709 / 87 / 539 exactly**. Had any driver probe taken
the correlated path, NEW's observed count would differ. The declaration
still speaks for every value an operator names; CB14 changed only whom it
speaks for.

The k-sweep control (3 ops per k — a byte-identity and store-state
control this run; §7c.6's 50-op measurement owns the serve numbers, and
every key here is already observed by the EXISTS on both engines).
Pooled across clean cells, 12 BASE / 9 NEW samples per k:

| k | BASE p0/p25/p50/p95/p99 (n=12) | NEW p0/p25/p50/p95/p99 (n=9) |
|---:|---|---|
| 1 | 44.8 / 47.2 / 51.0 / 53.8 / 60.6 | 45.2 / 45.8 / 49.7 / 58.8 / 58.8 |
| 2 | 44.1 / 45.6 / 46.8 / 54.7 / 70.4 | 45.2 / 46.0 / 46.4 / 48.8 / 48.8 |
| 4 | 48.2 / 49.0 / 50.0 / 58.7 / 62.4 | 48.8 / 49.2 / 51.7 / 57.8 / 57.8 |
| 8 | 53.8 / 54.8 / 55.6 / 57.8 / 59.2 | 55.2 / 55.3 / 56.8 / 66.5 / 66.5 |
| 16 | 66.0 / 68.7 / 68.9 / 74.5 / 78.0 | 68.6 / 69.1 / 71.1 / 80.7 / 80.7 |

k=16 p50 +3.2% pooled (68.9 → 71.1), inside per-cell floors of 4.8% on
both sides; firsts
79–93 µs on BASE against 84–86 on NEW — warm on both engines, §7c.6's
inheritance unchanged. The serve path does not run CB14's branch to a
different verdict and does not read differently.

### 7d.5 Versus PostgreSQL — unchanged, and still no twin

§7b.7 and §7c.7 stand whole: no PostgreSQL structure answers a
value-observed authoritative store, so the Cabin column has no twin and
none was invented; PostgreSQL's answer to these shapes is an index,
measured against ckdbs-with-index in §9b. The named task is unchanged:
`tools/pg_scenario3_library.py` at the no-index configuration on the
correlated shapes — its seq-scan-per-outer-row against the walk column —
is the cell that would complete the picture, and the twin driver
supports the mode. *(Measured 2026-08-19 — §7e, which also qualifies
this addendum's "an index or nothing": PostgreSQL's per-statement hash
build is a third answer for the never-repeating key, at a tenth of the
banked structures' ceiling.)*

### 7d.6 What §7d changes, and what it does not

**Changed: the never-repeating correlated key is closed on the Cabin's
side, and admission became evidence-scoped.** §7b.8's uncovered case and
§7c.8's restatement of it are superseded as descriptions of HEAD: on
`8420242` a cabined join column probed with keys that never repeat costs
one hash insert per key — no dead set, no standing write hook, no
`cabin_max_values` flood from a single SELECT (bounded now to
intra-statement repeats) — and the first touch is measurably cheaper
than the recording it declines. What remains uncovered is named
precisely: **the *uncabined, unindexed* never-repeating join key has no
accelerator by nature** — every observational structure earns its keep
on the second touch, and a key with no second touch leaves nothing to
serve, so the answer there is an index (§9b) or nothing. That is a
statement about the shape, not a gap in the engine.

**Not changed:** the steady states — §7c's 13.1× EXISTS headline and
§7b's serve numbers describe NEW from execution 3 exactly as they
described BASE from execution 2, one execution later and never cheaper
or dearer; the observation charge's size (§7c.4's account, re-measured
within replicate spread here); the literal probe's `n = 1` and every
driver shape (this run's floors); `CABIN AUTO`'s §11 threshold question
— narrowed, since wrong admission no longer buys dead sets, so what
remains of it is hit-rate and eviction policy, not flood control; the
sighting window's crude reset (`kMaxSightings` — §4a's qualifier: a key
whose repeats are separated by more distinct keys than the window holds
can oscillate below threshold; untested here, this run's ~570 distinct
keys sit far under the 4,096-entry window, and it stays §8's open
budget); §7c's sticky entry-cap mark; and the rest of this file, which
describes its own stamped commits.

## 7e. Addendum, 2026-08-19 — versus PostgreSQL at `--index-mode none`: the cells §7b.7 named, and the three-way account

§7b.7 named one missing cell and §7c.7 and §7d.5 repeated it: PostgreSQL
at the no-index configuration on the two correlated shapes, set against
the walk column the whole §7 family measures. This addendum runs those
cells — six of them, three sizes, two replicates, a fresh database each —
and closes the task. No ckdbs cell was re-run: §7b/§7c/§7d's cells ran
the same morning on the same box under the same background load, and
their numbers are cited below with their commits. The PG cells were not
interleaved with them, which the tightness of the PG pairs (≤2.9%
disagreement) and factor-level reading make tolerable, as §9b.6's twin
already was.

**The answers: the plan the named task anticipated does not exist —
PostgreSQL never produces a scan per outer row. At every k and every size
it plans one hash join with a single seq scan of `loans` per statement,
so its cost is nearly flat in k: 761 µs p50 at 10,000 loans and k=16
(1,314 stmts/s derived), which beats the ckdbs walk's 117/s by 11.2× and
loses to the warm Cabin serve's 14,870/s by 11.3×. On the correlated
EXISTS PostgreSQL is flat at ~1,469 µs (681/s) — slower than even the
pre-CB13 ckdbs short-circuit walk (1,021/s), and 19.7× behind CB13's
converged 13,423/s. The floor both engines share is the pass over the
relation: a 10,000-row pass costs ~530–550 µs on either engine; the
engines differ only in how many passes a statement pays and whether a
pass can be banked.**

### 7e.1 The run

| | |
|---|---|
| executed | **2026-08-19 08:00:14 → 08:00:22 UTC**, six PostgreSQL cells — 3 sizes × 2 replicates, each database created seconds before its load. A prior 200-loan smoke cell (`ops=20`) validated the harnesses and was dropped: a harness check, not a measurement, and none of its numbers appear here. **All six measured cells passed on the first attempt; none discarded** |
| branch / worktree | `worktree-enhence-join-perf` at **`41f8dff`** (clean) — the tree the harnesses and loader ran from; no ckdbs server ran in this addendum |
| server | **PostgreSQL 16.14**, the standing scratch cluster of `tools/pg_setup.sh` on port 15433 (the rootless dpkg-extracted tree, recipe in `bench/docs/README.md`), data directory `/home/cdkbs/pg-bench/data` on ext4 `/dev/root` (`df -T`; `/tmp` is ext4 on this host too). **Configuration at PostgreSQL defaults** — the loader's `--synchronous-commit off` is a per-session `SET` on the load connection only; every measured statement ran on a fresh connection at cluster defaults, and every measured statement is a read |
| databases | `s3pgnone_{200,1000,10000}_{1,2}`, one per cell, `createdb`-fresh, never reused |
| loader | `tools/pg_scenario3_library.py --host 127.0.0.1 --port 15433 --user cdkbs --database <db> --suffix s3 --loans N --matches 5 --index-mode none --ops 200 --synchronous-commit off --seed 1` — seed and row counts identical to the §7b/§7c/§7d ckdbs cells (10,000 / 2,000 / 2,000 / 5,000 rows at N=10,000, verified by `count(*)`). `--index-mode none` leaves exactly the four pk btrees (`pg_indexes` verified), the twin of ckdbs's always-present Keystone; the loader's closing `ANALYZE` ran, the driver's honest default, not tuning |
| the measured statements | per cell, in §7c's order: the §7c correlated EXISTS (**10 timed executions**, then 50 sampled ops, reply checked byte-level before and after), then the §7b k-sweep (per k ∈ {1,2,4,8,16}: `EXPLAIN (ANALYZE, BUFFERS)`, one first statement, 50 sampled ops) — session scratch twins of the §7b/§7c harnesses over `tools/pg_wire.py`'s benchmark path; §9b.7's fold-into-the-driver task now covers these twins too |
| contention control | every cell gated on `bench/wait_quiet.sh` (loadavg 0.15–0.36 at starts); `pgrep -c cc1plus` 0 before and after every cell. Background load, noted not killed: the resident autotrade `kds_server` on port 15432 (idle, warn-level logging) and resident agent processes — the same residents the ckdbs cells sat beside |
| correctness | 0 errors across all six driver runs (12 phases each); every EXISTS timed reply equal to its cell's first (60/60) and the closing full fetch byte-equal to the opening one in all six; the k=16 join returns **79 / 83 / 79 rows** at 200 / 1,000 / 10,000 — equal to the ckdbs replies (§9b.1) — and the EXISTS returns 20 rows in every cell |
| protocol asymmetry | both sides are Python clients on localhost TCP without TLS (the ckdbs cells were `KDS_WITH_TLS=OFF`); PostgreSQL replans every statement (simple-query protocol, no prepared statements) as ckdbs recompiles every statement. The client floors differ: `pk-user` p50 is ~60 µs in these cells against 37–38 µs in §7c's ckdbs cells, so ~22 µs of every PG number below is client and protocol — negligible against the 160–1,470 µs bodies |

### 7e.2 The plan — one hash join at every k, and nothing to flip to

Captured per k in every cell; the headline is identical across all 30
captures (6 cells × 5 ks): a hash join whose probe side is **one seq scan
of `loans` per statement**, with the pk btree serving the `users` range.
At 10,000 loans, k=16:

```
Hash Join  (actual rows=79)
  Hash Cond: (l.user_id = u.id)
  ->  Seq Scan on loans_s3 l  (actual rows=10000)   <- once per statement, any k
  ->  Hash  (rows=16)
        ->  Index Scan using users_s3_pkey on users_s3 u
              Index Cond: ((id >= 1) AND (id <= 16))
```

And the EXISTS — one seq scan feeding a `HashAggregate` over the 1,988
distinct `user_id`s, hash-joined to the 20 outer keys, every execution:

```
Hash Join  (actual rows=20)
  ->  HashAggregate  (rows=1988, Memory 241kB)
        ->  Seq Scan on loans_s3 l  (actual rows=10000)
  ->  Hash (rows=20)  <- Index Only Scan users_s3_pkey, id 1..20
```

Two readings. First, **the plan the named task anticipated — a
seq-scan-per-outer-row — is a plan PostgreSQL will not produce** at any k
or size measured: the nested-loop-over-scans shape that ckdbs's
pre-CB12/CB13 engine executes (§7b.2's BASE, `examined=40000` for 21
rows) has no PostgreSQL counterpart here, so the walk column's true twin
is a plan PostgreSQL's optimizer refuses. Second, **no flip anywhere**:
unlike §9b.6's indexed twin, where the optimizer left its own index at
k=16 for a merge semi-join and lost 4×, the unindexed matrix gives it
exactly one shape and it keeps it — flat plans, flat costs.
`EXPLAIN ANALYZE`'s own timings (1.04 ms execution at k=16 against the
761 µs measured statement) carry instrumentation and are used here for
shape only, never as the cost.

### 7e.3 The k-sweep: PostgreSQL pays the scan once per statement

p50 µs of 50 sampled ops per point, both replicates; stmts/s derived as
1e6/p50 of the pair mean (single serial connection — the harness reports
latency, so throughput is derived, per this file's matrix rule). The
ckdbs walk column is **cited**, not re-run: §9b.3 BASE (`9af3c8d`,
median of 4 cells) at 200 and 1,000; §7b.3 BASE (`6c5bb14`) at 10,000 —
and the warm Cabin column is §7b.3 NEW (`8f3f730`), measured at 10,000
only, its size-independence asserted from the structure (§7b.1):

| N | k | PG p50 (r1 / r2) | PG ≈stmts/s | ckdbs walk ≈stmts/s | PG ÷ walk |
|---:|---:|---:|---:|---:|---:|
| 200 | 1 | 124.7 / 128.3 | 7,905 | 20,680 | 0.38× |
| 200 | 4 | 138.4 / 136.2 | 7,283 | 11,760 | 0.62× |
| 200 | 16 | 202.7 / 198.5 | 4,985 | 4,380 | 1.14× |
| 1,000 | 1 | 196.1 / 194.7 | 5,118 | 10,400 | 0.49× |
| 1,000 | 4 | 204.0 / 204.4 | 4,897 | 3,880 | 1.26× |
| 1,000 | 16 | 270.3 / 272.1 | 3,687 | 1,105 | 3.3× |
| 10,000 | 1 | 678.4 / 680.1 | 1,472 | 1,713 | 0.86× |
| 10,000 | 4 | 698.9 / 697.3 | 1,433 | 459 | 3.1× |
| 10,000 | 8 | 707.4 / 707.1 | 1,414 | 233 | 6.1× |
| 10,000 | 16 | 761.5 / 760.2 | **1,314** | 117 | **11.2×** |

(k=2 and k=8 were measured at every size and sit on the same curves; the
10,000-row k=8 row is shown because the slope below uses it. Replicate
floors: worst pair disagreement 0.4% at 10,000, 1.4% at 1,000, 2.9% at
200 — the k=16 factors clear them by orders.)

The structure of the table is the finding. PostgreSQL's k=8→16 slope at
10,000 loans is **6.7 µs per outer row** (4.6–5.2 at the smaller sizes) —
output projection and hash probes, not scanning — against the ckdbs
walk's **527–539 µs per outer row** (§9b.3, §7b.3), because PostgreSQL's
scan happens once per statement while the walk happens k times. So the
crossing sits exactly where one pass amortizes: **at k=1 the two engines
pay one pass each and ckdbs is ahead** (584 µs walk against 679 µs
scan+hash+plan+client at 10,000; 0.86× in the table), and every k beyond
one multiplies the walk while PostgreSQL's line stays flat. Meanwhile the
warm Cabin serve (§7b.3 NEW, 67.2/67.3 µs at k=16, 14,870/s) is **11.3×
ahead of PostgreSQL's best unindexed plan** — it pays no pass at all.

The distributions at 10,000, k=16 (50 ops each):

| cell | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| s3pgnone_10000_1 | 744.1 | 757.6 | 761.5 | 786.5 | 815.4 |
| s3pgnone_10000_2 | 739.7 | 756.6 | 760.2 | 795.3 | 809.8 |

On waits: single-connection reads, no commit/fsync wait, no lock wait in
the unit. The 761 µs decomposes at best effort as ~60 µs of client and
round trip (the `pk-user` floor in these cells), ~70 µs of per-statement
planning (`EXPLAIN`'s planning time at k=16, uninstrumented estimate),
and ~630 µs of scan, hash build and 79-row projection; PostgreSQL
publishes no finer split of an executed statement and none is invented.

### 7e.4 The correlated EXISTS: flat by construction

The §7c statement, verbatim, 10 timed executions then 50 sampled ops per
cell. The series is flat — executions 2–10 spread at most 2.3% at 10,000
(9.9% at 1,000, 15.3% at 200, a declining warm-up tail, no step anywhere)
— because **there is nothing to converge: PostgreSQL rebuilds the
HashAggregate every execution and banks nothing**. The first execution
reads +3% at 10,000 and up to +29% at the smaller sizes — a fresh
connection's cache and catalog warm-up, not structure; the sampled table
below is taken after the series, past all of it. Sampled
distributions and the cited ckdbs columns (§7c.3, BASE `82ff9b7` flat /
NEW `fa1f320` steady, measured at 10,000 only):

| N | PG p0 / p25 / p50 / p95 / p99 (r1) | PG p50 r2 | PG ≈stmts/s |
|---:|---|---:|---:|
| 200 | 156.9 / 160.0 / 161.7 / 178.1 / 203.0 | 163.0 | 6,159 |
| 1,000 | 282.0 / 291.9 / 295.4 / 314.6 / 318.0 | 300.8 | 3,354 |
| 10,000 | 1,437.4 / 1,454.6 / 1,464.1 / 1,516.3 / 1,538.8 | 1,473.3 | **681** |

At 10,000 the three-way reads: PostgreSQL 681/s; the pre-CB13 ckdbs
engine — **flat forever at 1,021/s** (§7c.3 BASE pooled 979.3 µs), ahead
of PostgreSQL because its short-circuit walks ~15,248 rows per execution
where PostgreSQL scans all 10,000 *and* builds a 1,988-group hash; and
CB13's converged serve at **13,423/s** (§7c.3 NEW pooled 74.5 µs) —
19.7× past PostgreSQL — after an observation charge PostgreSQL never
pays and never banks. The ckdbs columns exist at 10,000 only (§7c.1's
"one size, deliberately"); the 200 and 1,000 rows above stand as
PostgreSQL's own scaling record, with no same-statement ckdbs twin
measured.

### 7e.5 The three-way account

Three engines' answers to an unindexed join column, priced at 10,000
loans (stmts/s derived from the cited p50s; observation charge from
§7c.3/§7c.4, CB14's split from §7d.2):

| | ckdbs walk (§7b/§7c BASE) | PostgreSQL, no index (this run) | ckdbs Cabin steady (§7b/§7c NEW) |
|---|---:|---:|---:|
| join k=16, stmts/s | 117 | 1,314 | 14,870 |
| correlated EXISTS, stmts/s | 1,021 | 681 | 13,423 |
| cost model | k passes per statement | one pass + hash build per statement, discarded at statement end | zero passes after observation |
| cross-statement memory | none | none | the store: ~6.4–9.3 ms charged once (CB14: on a key's second touch) |

The honest framing: **PostgreSQL's best unindexed plan is a
per-statement build with no cross-statement memory; the Cabin amortizes
across statements at the price of observation; and the pass over the
relation is the floor both engines share** — PostgreSQL's k-intercept
puts its 10,000-row pass at ~550 µs (679 µs at k=1 minus its ~125 µs
size-independent fixed part), against ckdbs's measured 527–539 µs per
walk of the same relation. Neither engine scans meaningfully faster;
they differ in how often they scan. The break-even arithmetic follows:
against PostgreSQL, the EXISTS's observation charge repays in **5–7
executions** (6.4–9.3 ms ÷ the ~1,394 µs/execution saving) and the k=16
join's in 9–13; against ckdbs's own walk it was 6–10 (§7c.4). A shape
that repeats a handful of times has already beaten both per-statement
engines.

One reading cuts against a sentence this file already carries. §7d.6
says of the uncabined, unindexed never-repeating join key that "the
answer there is an index (§9b) or nothing." This run measures a third
answer in the baseline: a per-statement hash build needs no repetition
and no index, and at k=16 it beats the walk 11.2× on first touch —
PostgreSQL's every touch is a first touch. ckdbs has no such operator;
whether it ever should is a design question this file only prices: the
per-statement build tops out at 1,314/s where the banked structures run
11,000–22,000/s (§9b.3, §7b.3), so the operator would buy the
never-repeating distribution only, at roughly a tenth of the banked
ceiling.

### 7e.6 What §7e changes, and what it does not

**Changed: the §7-family's PostgreSQL column exists.** §7b.7, §7c.7 and
§7d.5's named task is closed by this run's six cells; those sections now
point here and stand otherwise, because their central claim — no
PostgreSQL structure answers a value-observed authoritative store — is
confirmed, not weakened, by the measurement: PostgreSQL's unindexed best
is per-statement and memoryless, and the Cabin column still has no twin.
§7d.6's "an index or nothing" is qualified as §7e.5 states.

**Not changed:** every ckdbs number in the §7 family (cited here, not
re-measured); §9b.6's indexed twin and its k=16 flip, which this run's
flip-free unindexed matrix complements; §11, which compares the `single`
columns and is not amended — these cells' driver phases ran at
`pg-none` and sit in the run's JSONs, but no §11 claim rests on them;
and the rest of this file, which describes its own stamped commits.

## 8. Composite is the only structure that helps `overdue`

`overdue` filters two columns (`due_day BETWEEN ? AND ?` and `status = ?`),
which no single-column index satisfies — §4 shows it staying a `FilterScan`
under `single`. At 10,000 rows, statements per second:

| shape | `single` | `composite` | `covering` |
|---|---:|---:|---:|
| **overdue** | 1,318 | **4,127** | 1,303 |
| loans-by-user | 21,322 | 1,785 | 21,786 |
| loans-by-book | 22,173 | 1,777 | 1,728 |
| resv-by-user | 23,148 | 3,166 | 3,233 |
| books-by-author | 22,075 | 4,693 | 5,981 |
| `create-index` total | 18.6 ms (5) | 8.2 ms (2) | 6.9 ms (1) |

**A composite key turns `overdue` from a walk into a 3.1× faster access**,
and it is the only structure in this matrix that touches that shape. It buys
that by declaring two indexes instead of five, so the single-column shapes it
no longer covers fall back to the walk — which is not a defect of composite
keys but of this matrix's `--index-mode` being one choice for the whole
schema rather than per shape.

**`covering` bought nothing measurable and cost the most per index.** Its one
index leaves `loans-by-user` where `single` had it (21,786 against 21,322,
inside the floor) and leaves every other shape on the walk, while costing
6.9 ms to build one index against `single`'s 3.7 ms each. On this workload
the covering form has no read benefit to show; a shape whose projection is
entirely inside the key is what would demonstrate one, and this matrix does
not contain one.

## 9. The join is the engine's one large loss, and it is one optimization

> **2026-08-18: this loss is closed.** The rewrite this section asks for
> shipped as `881f69a` (equality propagation through a join key), and §9a
> below measures it by interleaved A/B against the engine state this section
> describes. This section stands as the account of *why* the pre-`881f69a`
> plan cost what it did; its throughput figures describe `9f762a3`, not the
> current engine.

`join-loan-user` is `SELECT l.book_id, l.due_day, u.member_code FROM loans l
JOIN users u ON l.user_id = u.id WHERE u.id = ?` — six rows out. At 10,000
rows ckdbs serves **223 statements a second against PostgreSQL's 8,850, a
40× gap**, and the index makes no difference at all (216/s without it, 223
with it, inside the floor).

`ANALYZE` on the measured data file says exactly why:

```
analyze rows=6 class=JoinSelect steps=2 examined=20000 pages=10086 opens=10001
step 0 Scan  loans AS l   opens=1      examined=10000 matched=10000 sel=100% pages=86
step 1 Probe users AS u   opens=10000  examined=10000 matched=6     sel=0%   pages=10000
  filter 0:0.1 = 0:1.0        <- the join key
  filter 0:1.0 = 3            <- the restriction, applied on the inner side
```

The restriction `u.id = 3` is applied as a filter on the *inner* Probe, so
the outer relation is scanned in full: 10,000 opens and 10,086 pages to
return six rows. The identical predicate written against one relation
compiles to a single `IndexProbe`:

```
analyze rows=6 class=JoinSelect steps=1 examined=6 pages=7 opens=1
step 0 IndexProbe loans   opens=1 examined=6 matched=6 sel=100% pages=7 index_scanned=6 index_resolved=6
```

**Seven pages against 10,086, for the same six rows.** PostgreSQL's plan for
the same statement propagates the equality through the join key and reaches
the index:

```
Nested Loop (rows=6)  Buffers: shared hit=14
  -> Index Scan using users_pkey on users u   Index Cond: (id = 3)
  -> Bitmap Heap Scan on loans l              Recheck Cond: (user_id = 3)
       -> Bitmap Index Scan on loans_user     Index Cond: (user_id = 3)
Execution Time: 0.125 ms
```

So the gap is not the executor, the storage or the index — KDS's own index
serves this predicate at 21,322 statements a second when it is written
without a join. **The missing piece is equality propagation through a join
key**: deriving `l.user_id = 3` from `l.user_id = u.id` and `u.id = 3`, which
would turn step 0 from a `Scan` into the `IndexProbe` the engine already has.
That is one plan-time rewrite, and on this shape it is worth 40×.

It is also the one shape where ckdbs loses at *every* size — 7,955/s against
9,950 at 200 rows, 2,136 against 8,826 at 1,000 — and the only shape whose
gap widens with the relation, because the scanned side grows and the
propagated form would not.

## 9a. Addendum, 2026-08-18 — the join gap is closed, and the fix costs nothing measurable

The rewrite §9 named — deriving `l.user_id = 3` from `l.user_id = u.id` and
`u.id = 3` at compile — landed as `881f69a` (`perf(exec): propagate a join
key's literal equality to the step that can be keyed on it`,
`src/exec/step_compiler.cpp`, `docs/parser-v2.md` §5). This addendum
answers the two questions that change raises and nothing else: **how much of
§9's loss the propagation recovers, and what the new compile pass costs
every other statement.** Both by interleaved A/B — the base engine and the
patched engine alternating cell by cell on the same box within the same
half hour — which the published matrix above, one engine at one commit,
could not do. Nothing else in this file is re-measured; every section above
still describes `9f762a3`.

**The answers: 84.8× on the join's p50 at 10,000 rows, and no other shape
moved outside the replicate floor.**

### 9a.1 The A/B run

| | |
|---|---|
| executed | **2026-08-18 08:10:59 → 08:13:56 UTC**, 12 ckdbs cells, alternating BASE, NEW, BASE, NEW at each size |
| branch / worktree | `worktree-enhence-join-perf`, in the worktree `enhence-join-perf` |
| commit measured | **`b058d5d`** (recorded by every cell, `dirty: false`) — a merge of `881f69a` with `eb680e4`; `git diff 881f69a b058d5d -- src include tests` is **empty**, so the engine code measured as NEW is exactly `881f69a`'s |
| BASE binary | a **copy**, `sha256 fababa23532cd60f…`, built fresh for this run from a temporary worktree at **`eb680e4`** (origin/main, the merge's other parent — the same engine minus the propagation commit), linked 2026-08-18 08:09:43 UTC |
| NEW binary | a **copy**, `sha256 f9eee9abe6a0e83b…`, from this worktree's `build-release/kds_server` (linked 08:04:15 UTC — one minute *before* `881f69a` was committed at 08:05:13; the tree was clean and `cmake --build` immediately before the run considered the binary current against HEAD's sources, so it is the engine at `b058d5d`) |
| build | Release (`-O3 -DNDEBUG`), gcc 13.3.0, **`KDS_WITH_TLS=OFF`** — the published run above was built `KDS_WITH_TLS=ON`; this box has no libssl-dev. §9a.5 is where that difference is priced before any cross-run comparison is made |
| device | ext4 on `/dev/root` (checked with `df -T`; `/tmp` is ext4 on this host too, not tmpfs); data files under `$HOME/bench-s3-joinab/db/`, WAL under `$HOME/bench-s3-joinab/wal/<cell>/`, binary copies under `$HOME/bench-s3-joinab/bin/` |
| server config | `cores = 1`, `durability = group`, `indexes = on`, port 15499. One server process and one **fresh data file** per cell, started from the run's own binary copy |
| driver | `tools/scenario3_library.py --loans N --index-mode single --ops 200 --verify 25 --assert-index-reads`, N ∈ {200, 1,000, 10,000} — which sizes `loans` at N, `users` and `books` at N/5, `reservations` at N/2, and runs 200 ops per shape, so BASE and NEW do **equal work**, not equal time |
| contention control | every cell gated on `bench/wait_quiet.sh`, and `bench/run_cell.sh` sampled `pgrep -c cc1plus` before and after each cell. **All 12 cells passed on the first attempt; none was discarded** |
| correctness | 12 cells, 0 errors, `verify_problems` empty in all (25 verified statements per shape per cell, join replies checked against a client-side join of full scans), `--assert-index-reads` green in all 12 |

### 9a.2 The plan, before and after

Captured after the run by reopening two measured cells' own data files with
the binaries that produced them. BASE (`eb680e4`), cell `ab-base-10000-1` —
§9's plan, reproduced:

```
analyze rows=6 class=JoinSelect steps=2 examined=20000 pages=10086 opens=10001
step 0 Scan  loans AS l   opens=1     examined=10000 matched=10000 sel=100% pages=86
step 1 Probe users AS u   opens=10000 examined=10000 matched=6     sel=0%   pages=10000
```

NEW (`881f69a`), cell `ab-new-10000-1`:

```
analyze rows=6 class=JoinSelect steps=2 examined=12 pages=13 opens=7
step 0 IndexProbe loans AS l  opens=1 examined=6 matched=6 sel=100% pages=7 index_scanned=6 index_resolved=6
  filter 0:0.1 = 3 derived      <- the propagated equality, marked as such
step 1 Probe users AS u       opens=6 examined=6 matched=6 sel=100% pages=6 memo_hits=5
```

**13 pages against 10,086, for the same six rows.** Step 0 is now the exact
`IndexProbe` §9 showed for the single-relation form (7 pages), and the join
adds only its inner side: six pk probes, five of them memoized. The
`derived` marker on the filter is the propagation pass naming itself.

### 9a.3 The join, BASE against NEW

p50 µs per statement, 200 ops per cell, two cells per binary per size:

| N | BASE p50 (cell 1 / 2) | NEW p50 (cell 1 / 2) | speedup (p50) | BASE ≈ stmts/s | NEW ≈ stmts/s |
|---:|---|---|---:|---:|---:|
| 200 | 122.4 / 120.9 | 51.6 / 51.3 | **2.4×** | 8,090 | 18,975 |
| 1,000 | 453.5 / 449.6 | 52.0 / 52.7 | **8.6×** | 2,210 | 18,710 |
| 10,000 | 4,381.1 / 4,398.8 | 51.9 / 51.6 | **84.8×** | 227 | 18,850 |

The full distributions, because a p50 alone cannot show that the *shape* of
the latency changed:

| cell | N | ops | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|---:|
| ab-base-200-1 | 200 | 200 | 110.3 | 119.9 | 122.4 | 135.3 | 141.0 |
| ab-base-200-2 | 200 | 200 | 110.7 | 118.8 | 120.9 | 136.2 | 141.5 |
| ab-new-200-1 | 200 | 200 | 41.8 | 49.1 | 51.6 | 63.3 | 73.2 |
| ab-new-200-2 | 200 | 200 | 34.3 | 48.9 | 51.3 | 62.2 | 68.4 |
| ab-base-1000-1 | 1,000 | 200 | 430.6 | 446.8 | 453.5 | 472.7 | 487.6 |
| ab-base-1000-2 | 1,000 | 200 | 429.5 | 441.3 | 449.6 | 474.0 | 492.9 |
| ab-new-1000-1 | 1,000 | 200 | 42.1 | 47.8 | 52.0 | 66.1 | 75.4 |
| ab-new-1000-2 | 1,000 | 200 | 39.8 | 49.5 | 52.7 | 63.5 | 75.4 |
| ab-base-10000-1 | 10,000 | 200 | 4,338.1 | 4,370.8 | 4,381.1 | 4,477.6 | 4,755.1 |
| ab-base-10000-2 | 10,000 | 200 | 4,333.2 | 4,386.8 | 4,398.8 | 4,451.1 | 4,629.8 |
| ab-new-10000-1 | 10,000 | 200 | 45.3 | 49.8 | 51.9 | 60.6 | 74.6 |
| ab-new-10000-2 | 10,000 | 200 | 44.0 | 49.9 | 51.6 | 63.0 | 79.9 |

Two readings. **First, the propagation converts §9's per-row cost into a
fixed one**: NEW's p50 is 51.3–52.7 µs at *every* size — the join no longer
scales with the relation, which is the definition of the conversion this
file's §6 credits the index with on the single-relation shapes. BASE's p50
grows 122 → 454 → 4,390 µs over the same sweep. **Second, the residual over
the single-relation form is small and explicable**: `loans-by-user` (one
`IndexProbe`, no join) runs at ~45 µs p50 in these same cells, so the join's
second step — six memoized pk probes and the projection across two relations
— costs ~7 µs. §12 asked whether the fix would reach the single-relation
form's throughput; the answer is ~89% of it, and the missing 11% is the
inner step, not the propagation.

On waits: every measured statement is a single-connection read, so there is
no commit/fsync wait and no lock wait in these numbers; the unit is a client
round trip on localhost. NEW's join p0 of ~44 µs is the same fixed
round-trip floor every ~40 µs shape in §5 pays, which is what bounds the
decomposition available here — engine time above the floor is ~7 µs and no
per-step timing exists to split it further (`docs/observability.md` owns
that gap).

### 9a.4 Every other shape is unchanged — the pass's overhead is below the floor

The propagation pass runs at compile time on every statement, so the claim
that needs evidence is the null one. p50 per shape, NEW/BASE as the ratio of
pair means; the floor at each size is the **widest within-binary replicate
disagreement across all ten shapes** (dividing by the smaller of the pair),
i.e. the noise measured from inside this run:

| shape | N=200 Δ | N=1,000 Δ | N=10,000 Δ |
|---|---:|---:|---:|
| pk-user | −1.2% | +4.8% | −0.5% |
| loans-by-user | −1.2% | +1.8% | −1.1% |
| loans-by-book | −1.0% | +0.8% | −1.2% |
| resv-by-user | −0.7% | +0.5% | −2.7% |
| books-by-author | −2.3% | −0.4% | −1.6% |
| books-by-genre | +0.1% | −0.1% | −1.4% |
| loans-by-daterange | −0.3% | +1.3% | +1.6% |
| overdue | +0.6% | 0.0% | +1.7% |
| count-by-user | −2.3% | −0.8% | −0.5% |
| **replicate floor** | **2.4%** | **10.3%** | **4.1%** |

**No shape at any size moves outside its floor**, in either direction, and
the deltas carry no consistent sign. The 1,000-row floor of 10.3% is one
BASE `pk-user` pair disagreeing with itself (35.8 vs 39.5 µs); the shape's
NEW pair disagrees by 0.8%, and every other 1,000-row pair is under 3.3% —
the wide floor is a property of one cell, not of the change under test. The
verdict this table supports: **the compile-time cost of the propagation
pass is unmeasurable at a 2.4–4.1% floor.** Deltas inside the floor are not
findings, so none of the rows above is one.

### 9a.5 Versus PostgreSQL — cited from §11, not re-measured

This addendum ran no PostgreSQL cells. The published twin figures in §11
stand for the comparison, and the cross-run bridge is BASE itself: this
run's BASE join at 10,000 rows (227/s, mean 4,402 µs) reproduces the
published `ck-single-10000` figure (223/s) within **1.9%**, across a
different worktree, a different binary, and the TLS-OFF build — so at the
*factor* level, comparing this run's NEW against §11's PostgreSQL column is
admissible, and build-difference effects are below that 2% on this shape.
On those terms: NEW serves **≈18,850 statements a second where §11's
`pg-single` serves 8,850** — the shape moves from a 40× loss to **≈2.1×
ahead**, in line with the 1.4–2.3× every other indexed shape in §11 already
showed. The caveat stays: the published PG number was measured beside a
TLS-ON ckdbs build on 2026-08-18 05:01–05:20 UTC, and a fresh interleaved
PG twin at this commit is the measurement that would retire it.

### 9a.6 What this addendum does not re-measure

- **Everything else in this file.** §1–§8 and §10–§12 describe `9f762a3`.
  The propagation touches statement compilation only; §9a.4 is the evidence
  that the other nine shapes did not move, but the `none`/`cabin`/`off`/
  `covering` columns, the durability matrix and the backfill cost were not
  re-run.
- **The PostgreSQL twin** (§9a.5 above: cited, bridged by BASE, not re-run).
- **The join at `--index-mode none`.** §9 measured 216/s without the index;
  propagation without an index to propagate *into* would have no
  `IndexProbe` to reach. This run declared the index in every cell, so what
  a propagated-but-unindexed join costs is unmeasured.
- **Non-pk or literal-free join keys.** The workload's restriction is
  `u.id = ?` against the pk; a join with no literal at all, or one whose
  derived equality lands on an unindexed column, is not in this driver.
  *(2026-08-18, later the same day: the literal-free case is now measured —
  §9b below. 2026-08-19: the unindexed-column case is closed for repeating
  keys by CB12's correlated Cabin probe — §7b.)*

## 9b. Addendum, 2026-08-18 — the join with no literal: the inner index, probed per outer row

§9a closed the join whose restriction is a literal, and its §9a.6 named
what that pass cannot reach: a join with no literal at all. `ON l.user_id =
u.id WHERE u.id BETWEEN ? AND ?` gives the propagation nothing to derive —
there is no constant to push onto `loans` — so the inner side stayed a full
scan *per outer row*, and the same is true of a correlated `EXISTS`. **IX17**
(`4f304fd`, `perf(index): the correlated probe`, `docs/feat-index.md` §8a)
closes that shape differently: when the inner side of a join step carries a
secondary index on the join column, the executor probes that index keyed by
each outer row's value (`IndexProbe::key_from`) instead of walking the
relation. This addendum measures exactly two things by interleaved A/B —
**what the probe wins on the shapes propagation cannot reach, and what its
compile-side selection costs every other statement** — against BASE
`9af3c8d`, which already contains the propagation, so the delta is IX17
alone.

**The answers: 95.8× on the no-literal join's p50 at 10,000 loans and 16
outer rows — the inner side's cost falls from ~527 µs per outer row,
proportional to the relation, to ~3.3 µs per outer row at every size — 16.5×
on the correlated EXISTS, and no other shape outside this run's replicate
floor.** The floor itself needs an honest paragraph: it came out at 16–42%,
an order worse than §9a's, because the box's round-trip latency was bimodal
for the whole window; §9b.5 shows the p0 evidence that the modes are the
machine and not the engine, and bounds any real overhead at ≈2% by
comparing mode-matched cells.

### 9b.1 The A/B run

| | |
|---|---|
| executed | **2026-08-18 09:04:36 → 09:13:40 UTC** (24 ckdbs cells in two passes of 12, alternating BASE, NEW, BASE, NEW at each size), PostgreSQL twin cells 09:16–09:17 UTC |
| branch / worktree | `worktree-enhence-join-perf`, in the worktree `enhence-join-perf` |
| commit measured | **`4f304fd`** (IX17), recorded by every cell, `dirty: false` |
| BASE binary | a **copy**, `sha256 f9eee9abe6a0e83b…`, built fresh for this run (09:02:39 UTC) from a temporary worktree at **`9af3c8d`** — the commit §9a landed at. The hash is **byte-identical to §9a's NEW binary**, which both proves the rebuild reproduces that engine exactly and makes §9a's tables directly comparable to this run's BASE column |
| NEW binary | a **copy**, `sha256 74ecca3d0301e307…`, from this worktree's `build-release/kds_server`, linked 08:59:34 UTC — the same minute `4f304fd` was committed; `cmake --build` immediately before the run was a no-op |
| build | Release (`-O3 -DNDEBUG`), gcc 13.3.0, `KDS_WITH_TLS=OFF` (no libssl-dev on this box, as in §9a) |
| device | ext4 on `/dev/root` (`df -T`; `/tmp` is ext4 on this host too); data files under `$HOME/bench-s3-ix17ab/db/`, WAL under `$HOME/bench-s3-ix17ab/wal/<cell>/`, binary copies under `$HOME/bench-s3-ix17ab/bin/` |
| server config | `cores = 1`, `durability = group`, `indexes = on`, port 15499. One server process and one **fresh data file** per cell, started from the run's own binary copy |
| driver, standard shapes | `tools/scenario3_library.py --suffix s3 --loans N --index-mode single --ops 200 --verify 25 --assert-index-reads`, N ∈ {200, 1,000, 10,000} (`loans` = N, `users`/`books` = N/5, `reservations` = N/2) — equal work per cell, 4 cells per binary per size |
| the two new shapes | driven against each cell's already-loaded server over `tools/ckdbs_cli.py`'s `ServerConnection`, percentiles by `bench_common.Phase` — 50 ops per point after 3 warm-ups. The harness is a session scratch script, **not checked in**; the statements are quoted in full below and §9b.7 names the task that folds them into the driver |
| contention control | every cell gated on `bench/wait_quiet.sh`; `pgrep -c cc1plus` sampled before and after each cell was 0 throughout. **All 24 ckdbs cells passed on the first attempt; none was discarded.** The box was *not* otherwise idle — see §9b.5 |
| correctness | 24 cells, 0 driver errors, `verify_problems` empty, `--assert-index-reads` green in all 24. Per cell, the k=16 no-literal join reply was checked as a **multiset against rows assembled from single-relation probes** (`loans WHERE user_id = i` joined client-side to `users WHERE id = i`): 24/24 equal (79 rows at N=200 and 10,000, 83 at N=1,000), and the EXISTS returned exactly 20 rows in every cell |

The two statements, verbatim (suffix `s3`; `k` ∈ {1, 2, 4, 8, 16} is the
outer-row count, since ids 1…k all exist):

```
SELECT l.book_id, u.member_code FROM users_s3 AS u
  JOIN loans_s3 AS l ON l.user_id = u.id WHERE u.id BETWEEN 1 AND k

SELECT id FROM users_s3 WHERE EXISTS
  (SELECT l.id FROM loans_s3 AS l WHERE l.user_id = users_s3.id) LIMIT 20
```

### 9b.2 The plan, before and after

Captured by each cell's own harness at N=10,000, k=4. BASE (`9af3c8d`) —
the inner side is a full scan **per outer row**, which §9a's propagation
cannot touch because there is no literal:

```
analyze rows=21 class=JoinSelect steps=2 examined=40032 pages=346 opens=5
step 0 Range users AS u  opens=1 examined=32    matched=4  sel=12% pages=2 range_stopped_early=1
step 1 Scan  loans AS l  opens=4 examined=40000 matched=21 sel=0%  pages=344
  filter 0:1.1 = 0:0.0       <- the join key, no constant anywhere
```

NEW (`4f304fd`) — the same statement, the inner side entered through
`loans_user` keyed by the outer row:

```
analyze rows=21 class=JoinSelect steps=2 examined=53 pages=27 opens=5
step 0 Range      users AS u  opens=1 examined=32 matched=4  sel=12% pages=2 range_stopped_early=1
step 1 IndexProbe loans AS l  opens=4 examined=21 matched=21 sel=100% pages=25 index_scanned=21 index_resolved=21
  key=0:0.0                  <- the probe key is the outer row's u.id
```

**53 rows examined against 40,032, 27 pages against 346, for the same 21
rows.** The correlated EXISTS converts identically: BASE examined 25,050
inner rows over 20 correlated scans (221 pages); NEW examined 20 over 20
probes (40 pages), `index_scanned=97 index_resolved=20`.

### 9b.3 The no-literal join, BASE against NEW

p50 µs per statement, **median of 4 cells** per binary per size, 50 ops per
cell per point; stmts/s derived as 1e6/p50 (single serial connection, so the
conversion is exact):

| N | k | BASE p50 | NEW p50 | BASE ≈stmts/s | NEW ≈stmts/s | speedup |
|---:|---:|---:|---:|---:|---:|---:|
| 200 | 1 | 48.4 | 45.4 | 20,680 | 22,030 | 1.06× |
| 200 | 4 | 85.0 | 53.2 | 11,760 | 18,820 | 1.60× |
| 200 | 16 | 228.4 | 86.5 | 4,380 | 11,560 | **2.64×** |
| 1,000 | 1 | 96.2 | 46.2 | 10,400 | 21,670 | 2.08× |
| 1,000 | 4 | 257.6 | 53.7 | 3,880 | 18,620 | 4.80× |
| 1,000 | 16 | 905.3 | 90.5 | 1,105 | 11,040 | **10.0×** |
| 10,000 | 1 | 600.5 | 46.1 | 1,665 | 21,690 | 13.0× |
| 10,000 | 4 | 2,234.0 | 55.2 | 448 | 18,120 | 40.5× |
| 10,000 | 16 | 8,635.2 | 90.2 | 116 | 11,090 | **95.8×** |

(k=2 and k=8 were measured too and sit on the same curves: at 10,000 rows,
23.3× and 69.4×.)

The number that explains the whole table is the **marginal cost per outer
row** — the k=8→k=16 slope, which excludes the statement's fixed part (the
round trip and the outer range walk, identical in both binaries):

| N | BASE µs/outer row | NEW µs/outer row |
|---:|---:|---:|
| 200 | 12.1 | 3.1 |
| 1,000 | 53.6 | 3.0 |
| 10,000 | 527.0 | 3.3 |

BASE's inner side costs one full scan of `loans` per outer row —
proportional to the relation, tripling the sweep's decade steps. NEW's is a
~3.2 µs probe **independent of the relation's size**: the same conversion of
a per-row cost into a fixed one that §6 measured for the single-relation
equality and §9a for the literal join, now on the last shape that lacked it.
NEW's p50 at any k is ≈ 42 µs + 3.2·k µs at every size measured.

The full distributions at N=10,000, k=16, because the shape of BASE's
latency (tight around a huge mean) is itself evidence that the cost is the
scan and not a stall:

| cell | ops | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|
| ab-base-10000-1 | 50 | 8,426.2 | 8,465.5 | 8,514.3 | 10,342.9 | 10,781.8 |
| ab-base-10000-2 | 50 | 9,135.1 | 9,164.2 | 9,196.5 | 9,272.7 | 9,291.4 |
| ab-base-10000-3 | 50 | 8,462.0 | 8,500.7 | 8,514.2 | 8,568.7 | 8,584.8 |
| ab-base-10000-4 | 50 | 8,704.0 | 8,743.3 | 8,756.2 | 8,808.2 | 9,802.2 |
| ab-new-10000-1 | 50 | 88.7 | 91.2 | 92.0 | 101.5 | 105.5 |
| ab-new-10000-2 | 50 | 83.9 | 88.6 | 89.9 | 101.9 | 105.2 |
| ab-new-10000-3 | 50 | 87.9 | 88.7 | 89.1 | 100.9 | 104.1 |
| ab-new-10000-4 | 50 | 87.3 | 89.3 | 90.4 | 106.1 | 107.0 |

On waits: single-connection reads — no commit/fsync wait, no lock wait; the
unit is one localhost round trip. NEW's p0 of ~84–89 µs at k=16 decomposes
as the ~27 µs socket floor every §5 shape pays, plus 16 index probes and pk
resolutions at ~3.2 µs each, plus the two-relation projection of 79 rows;
no per-step timing exists to split it further (`docs/observability.md` owns
that gap).

### 9b.4 The correlated EXISTS

Same treatment: p50 median of 4, stmts/s derived. The statement returns 20
rows at every size (`LIMIT 20`).

| N | BASE p50 | NEW p50 | BASE ≈stmts/s | NEW ≈stmts/s | speedup |
|---:|---:|---:|---:|---:|---:|
| 200 | 88.2 | 84.2 | 11,340 | 11,880 | 1.05× |
| 1,000 | 238.8 | 85.1 | 4,190 | 11,760 | 2.81× |
| 10,000 | 1,403.9 | 85.2 | 710 | 11,730 | **16.5×** |

Distributions at N=10,000 (50 ops per cell):

| cell | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| ab-base-10000-1 | 1,359.7 | 1,369.5 | 1,376.2 | 1,824.8 | 2,928.9 |
| ab-base-10000-2 | 1,427.9 | 1,437.3 | 1,476.8 | 1,499.9 | 1,508.0 |
| ab-base-10000-3 | 1,370.9 | 1,377.8 | 1,383.8 | 1,418.3 | 1,426.9 |
| ab-base-10000-4 | 1,416.0 | 1,421.1 | 1,424.1 | 1,436.7 | 1,444.3 |
| ab-new-10000-1 | 82.9 | 84.2 | 85.6 | 97.3 | 403.6 |
| ab-new-10000-2 | 85.1 | 85.8 | 86.5 | 96.0 | 98.0 |
| ab-new-10000-3 | 79.3 | 83.7 | 84.9 | 94.9 | 98.0 |
| ab-new-10000-4 | 81.8 | 83.6 | 84.1 | 95.4 | 97.9 |

The N=200 row is the instructive one: **1.05× is not a miss, it is the
walk being short**. A correlated EXISTS short-circuits at the first inner
match, and at 200 loans the scan finds one within ~30 rows, so there was
little for the probe to save; NEW's p50 is flat at ~85 µs across the sweep
(20 probes plus the round trip) while BASE grows with the relation. The
probe pays exactly where §9's argument predicts: when the walk it replaces
is long.

### 9b.5 Every other shape, and the floor this run could actually reach

The overhead question: IX17 extends step compilation (index selection now
runs for the probe side of a join) and the executor's probe setup, so the
claim needing evidence is again the null one. p50 per shape, **median of 4
cells**, NEW/BASE − 1; the floor at each size is the widest within-binary
disagreement across the 4 replicates of any shape:

| shape | N=200 Δ | N=1,000 Δ | N=10,000 Δ |
|---|---:|---:|---:|
| pk-user | +13.6% | +0.1% | +2.0% |
| loans-by-user | +15.4% | −0.8% | +0.7% |
| loans-by-book | +15.0% | +0.8% | +0.9% |
| resv-by-user | +17.3% | +0.0% | +0.1% |
| books-by-author | +14.2% | +0.8% | −0.5% |
| books-by-genre | +15.8% | +0.1% | −0.4% |
| loans-by-daterange | +11.3% | −0.0% | −0.2% |
| overdue | +10.9% | −0.5% | −0.0% |
| join-loan-user | +13.5% | +3.3% | +0.4% |
| count-by-user | +14.7% | +0.4% | +1.9% |
| **replicate floor** | **42.1%** | **16.3%** | **30.5%** |

No shape at any size moves outside its floor — but floors of 16–42% against
§9a's 2.4–4.1% would make that verdict hollow without an account, so here
it is. **The box's round-trip body was bimodal through the whole window.**
The per-shape best case barely moved between binaries — min-of-4 p0 differs
by under ~3 µs with no consistent sign on every shape at every size, e.g.
pk-user 26.0→26.6/27.4→28.7/26.9→27.1 across the sweep — but a cell's p50
landed either ~2 µs above its p0 or ~10 µs above it, and mode membership
tracked *time, not binary* (BASE's own pk-user replicates read
27.3/29.2/37.9/38.8 at N=200 — that pair disagreement *is* the 42% floor). The machine was not idle: an unrelated resident `kds_server`
belonging to another project on this host started inside the window (09:04:41,
port 15432, ~1% CPU) alongside resident agent processes, and 1-minute load
ran 0.2–0.7 — under `wait_quiet.sh`'s 0.70 gate, with zero compilers, but
enough wakeup traffic on 2 vCPUs to add ~10 µs of scheduling delay to a
serial ping-pong in the affected stretches.

Two measurements cut through the modes. First, **N=1,000's column, where
by luck both binaries' cells landed predominantly in the same mode: every
shape within ±0.8% except `join-loan-user` at +3.3%.** Second, comparing
only mode-matched cells (classified by pk-user p50−p0, threshold 5 µs): at
N=10,000 all four NEW cells and three BASE cells share the slow mode, and
the worst shape delta between them is **+1.2%**; at N=1,000 the tight-mode
cells (3 BASE vs 2 NEW) put the worst at +2.2%; N=200's systematic-looking
+11–17% column dissolves under the same treatment (tight vs tight ≤ +4.8%
on a single-cell comparison, loose vs loose ≤ +2.2%). The verdict this
supports: **any per-statement cost of IX17's added compile-side selection
is below ≈2% (≈1 µs) — unmeasurable on this box this day — and
`join-loan-user`, the shape that takes the propagation path on both
binaries and was required not to regress, is +0.4% at N=10,000, dead inside
every floor.** Deltas inside the floor are not findings; none of the rows
above is one.

### 9b.6 Versus PostgreSQL — measured this time, on the same shapes

Unlike §9a.5 this addendum ran its own twin: the same two statements against
PostgreSQL 16.14 (the scratch cluster of `tools/pg_setup.sh`, port 15433,
**defaults untouched**), loaded by `tools/pg_scenario3_library.py --loans N
--matches 5 --index-mode single --suffix s3` — same seed, same row counts
(79/83/79 rows at k=16 across the sizes, equal to ckdbs's replies). Two
replicates per size, 09:16–09:17 UTC on the same box, quiet-gated; not
interleaved with the ckdbs cells, which the stability of both sides' pairs
(≤1% disagreement on PG, table below from pair means) makes tolerable for
factor-level reading. Both columns are the same Python client discipline,
so both carry their client's floor.

N=10,000, p50 µs and derived stmts/s:

| shape | KDS NEW p50 | PG p50 | KDS ≈stmts/s | PG ≈stmts/s | factor |
|---|---:|---:|---:|---:|---:|
| join-nolit k=1 | 46.1 | 152.0 | 21,690 | 6,580 | 3.3× |
| join-nolit k=4 | 55.2 | 181.0 | 18,120 | 5,520 | 3.3× |
| join-nolit k=8 | 63.7 | 186.6 | 15,710 | 5,360 | 2.9× |
| join-nolit k=16 | 90.2 | 769.4 | 11,090 | 1,300 | **8.5×** |
| exists-corr | 85.2 | 171.8 | 11,730 | 5,820 | 2.0× |

At k ≤ 8 PostgreSQL plans exactly what IX17 now executes — a nested loop
probing `loans_user` per outer row (`Bitmap Index Scan … Index Cond:
(user_id = u.id)`, 0.147 ms execution at k=4) — and the ~3× factor is
mostly the two clients' fixed costs plus per-statement planning, which the
simple-query protocol repays every time. **At k=16 PostgreSQL's optimizer
flips to a merge semi-join over full index scans and loses 4× to its own
k=8 number** (186.6 → 769.4 µs), while ckdbs's fixed rule — first index,
first conjunct — has no flip available and stays on the probe. At N=200 and
1,000 the same table reads 2.4×/3.1× at k=16 (86.5 vs 206.1, 90.5 vs 284.6)
and 1.9× on the EXISTS. The caveat cuts the other way from §9a.5's: this
twin measured only the two new shapes; §11's published matrix for the
standard shapes was not re-run.

### 9b.7 What this addendum does not re-measure, and what it opens

- **Everything §9a.6 already lists.** §1–§8 and §10–§12 still describe
  `9f762a3`; §9a's tables still describe `9af3c8d`'s engine — which this
  run's BASE binary reproduced byte-for-byte, so the two addenda chain.
- **The no-literal join without the index.** Every cell declared
  `--index-mode single`; with no index on the join column IX17 has nothing
  to probe and the walk remains. What that walk costs was measured here as
  BASE; that it is *still* the plan at `4f304fd` when no index exists is
  asserted by the guard tests (`tests/index_compile_test.cpp`), not by a
  cell in this run.
- **A join key on an unindexed non-pk column** — §9a.6's other half.
  *(2026-08-19: closed for repeating keys by CB12's correlated Cabin probe,
  measured in §7b at ~1.7 µs marginal per outer row warm; what remains
  uncovered is the never-repeating-key distribution, a `CABIN AUTO` policy
  question.)*
- **Multi-core.** `cores = 1` throughout. On a multi-core instance an index
  step cannot ship, so a peer-owned join on an indexed column is *refused*
  by the affinity check — opened at `881f69a`, widened by IX17, recorded in
  `docs/known-gaps.md` with the ship-time-downgrade fix. A cross-core cell
  of this workload is not currently runnable.
- **The driver does not carry these shapes.** The no-literal join and the
  correlated EXISTS were driven by a session scratch harness (statements in
  §9b.1). The task this opens: fold both into `tools/scenario3_library.py`
  and `tools/pg_scenario3_library.py` as first-class phases so the next
  full-matrix run measures them without ceremony. *(2026-08-19: closed —
  both drivers now carry `join-no-literal` (k fixed at 16) and
  `exists-correlated` as standard phases, under the same names and from
  one set of statement builders. On the ckdbs side both are in the
  `ANALYZE` block, under `--assert-index-reads`, and `--verify`-checked row
  for row against a client-side expectation; the twin has no counterpart to
  any of those three and prints `EXPLAIN` for both instead. The full
  k-sweep remains a harness shape, and the phases' numbers will appear in
  future runs, not retroactively here.)*

## 10. Durability decides the load, and nothing else here

The read shapes are identical under all three durability classes — every
figure in `ck-dur-relaxed-10000` and `ck-dur-strict-10000` sits inside the
floor of `ck-single-10000`, which is what should happen to a read. The load
is a different matter. 19,000 rows, one row per statement, no batching:

| `durability` | rows a second | vs `relaxed` |
|---|---:|---:|
| `relaxed` | **29,412** | — |
| `group` | 958 | **1/30.7** |
| `strict` | 802 | **1/36.7** |

An unbatched row-at-a-time load is entirely fsync-bound: **relaxing the
durability promise is worth 30.7×**, which is the same finding
`bench/results-scenario2-freight.md` reaches from the other direction when it
prices autocommit at 5.6×. `group` and `strict` differ by only 19% because a
group batch formed from one connection is a batch of one — the same reason
that file's concurrency section gives.

**PostgreSQL's load runs at the same rate as ckdbs's `group`**: 903 rows a
second against 922, 2% apart, both engines paying one fsync per statement to
the same filesystem. That is the cross-engine confirmation the number needs —
this is the device's price for a durable single-row insert, not either
engine's.

## 11. Versus PostgreSQL

Nine of ten shapes go to ckdbs at 10,000 rows, the tenth by 40×. Ratios are
`ck-single` ÷ `pg-single`, so above 1 means ckdbs serves more:

| shape | ck single | pg single | ratio |
|---|---:|---:|---:|
| pk-user | 29,412 | 15,480 | **1.90×** |
| loans-by-user | 21,322 | 11,848 | **1.80×** |
| loans-by-book | 22,173 | 12,500 | **1.77×** |
| resv-by-user | 23,148 | 13,423 | **1.72×** |
| books-by-author | 22,075 | 12,642 | **1.75×** |
| books-by-genre | 8,905 | 3,798 | **2.34×** |
| loans-by-daterange | 1,429 | 910 | **1.57×** |
| overdue | 1,318 | 943 | **1.40×** |
| **join-loan-user** | 223 | 8,850 | **0.03× — 40× short** *(closed 2026-08-18, §9a: ≈18,850/s at `881f69a`)* |
| count-by-user | 25,189 | 13,351 | **1.89×** |

**Read the 1.4×–2.3× column with the same caution scenario2's does.** A
single-shape read here is 11,000–29,000 statements a second, which is 35–90 µs
of client-measured round trip, and KDS's newline text protocol is a lighter
round trip than PostgreSQL's v3 wire; part of every ratio above is protocol
rather than engine. What the column establishes is that KDS is not paying a
penalty anywhere in this workload's read paths — not that it is twice the
engine.

Three rows do carry more than protocol, because their absolute cost is large
enough to swamp the round trip:

- **`loans-by-daterange` and `overdue`, 1,318–1,429/s against 910–943.** Both
  are full walks on both engines — PostgreSQL's own `EXPLAIN` shows a
  `Seq Scan` for `overdue` — so this is scan against scan, and KDS's serves
  ~1.4–1.6× more over the same 10,000 rows.
- **`join-loan-user`, the 40×.** §9 is the whole explanation and it is a
  missing plan-time rewrite, not a slow executor.

**PostgreSQL declines its own index at 200 rows and KDS does not**, which
§4 predicted and this table prices: at 200 rows `pg-single` serves *fewer*
statements than `pg-none` on eight of ten shapes (12,422/s against 13,228 on
`loans-by-user`), because the planner correctly judges the index not worth
its setup on a 40-row relation and pays for the declaration anyway. KDS
descends unconditionally and comes out at 21,505 against 20,284 — better
here, but for a reason that is not a cost model, and a shape can be
constructed where descending unconditionally is the wrong choice.

**The `--cabin` cells have no PostgreSQL column**, and the twin does not
invent one. There is no PostgreSQL equivalent of a structure that is
authoritative only for values a query has already observed, which is why §7
is a ckdbs-only comparison against ckdbs's own alternatives.

## 12. What this run does not answer

- **Whether the join's 45× survives its fix.** *(Answered 2026-08-18, §9a:
  the fix landed as `881f69a` and the same cell, run A/B, gives 84.8× on p50
  at 10,000 rows — ~89% of the single-relation form's throughput, the
  missing 11% being the join's second step.)* §9 identifies the missing
  rewrite and prices the gap; it does not prove that propagating the equality
  would reach the 21,322 statements a second the single-relation form gets,
  because the join still has a second step to run.
- **Why `CabinProbe` costs more than a `FilterScan` per row.** *(Answered
  2026-08-19, §7a: the recording miss walk decoded every column of every
  walked row where the plain walk decodes only the filtered ones — found by
  reading the path, since per-step timing still does not exist. Fixed at
  `a44c5cc`; the residual is ~5–6%, one key comparison per walked row.)*
  §7 measures the inversion twice and locates it in the Cabin's own path via
  `ANALYZE`, but the engine exposes no per-step timing that would say which
  part of that path is the cost. `docs/observability.md` owns that gap and
  it is unbuilt.
- **The Cabin's hit rate, and the space each structure costs.** This run
  measures neither, and they are the mechanism behind §7's inversion: a Cabin
  is authoritative only for values already observed, so its benefit is a
  function of how often a probe's argument repeats, and holding
  matches-per-key constant necessarily grows the key space with the relation.
  Measuring that needs the driver to report hit rate per cell, which it does
  not. The page counts each structure adds to the data file are equally
  unmeasured here.
- **What a covering index is worth.** §8 shows this matrix contains no shape
  whose projection fits inside a covering key, so the `covering` column
  measures its cost and none of its benefit.
- **Anything about concurrency.** Every cell is one connection.
  `scenario0_stockmarket.py` and scenario2's §11 are where contention is
  measured; nothing here shares a row with anyone.
- **Whether the index's write-side cost is acceptable.** §6 prices the
  backfill. The per-write maintenance cost is inside the load phase and is
  not separated from it here: `ck-single-10000` loads 958 rows a second
  against `ck-none-10000`'s 922, which says only that both are fsync-bound at
  a granularity that hides the difference.
