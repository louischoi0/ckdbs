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
ever wanted.

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
(`docs/feat-cabin.md` §11), not an executor one. Also unchanged: the
EXISTS non-convergence (§7b.4 above, recorded in §4a with the reason the
narrow fix is wrong); the `correlated_scans` counter blind spot (§4a: a
still-quadratic correlated CabinProbe reports zero); everything §7a.7
lists as not re-measured; and the rest of this file, which describes
`9f762a3`.

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
  full-matrix run measures them without ceremony.

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
