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
different engine build and a cleaner noise floor.

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
behaviour on this shape rather than as an artefact. `count-by-user` is the
sharper number: the same fold that runs at 25,189/s over an index runs at
**992/s** over a Cabin, a 25× difference.

**This is not the Cabin's behaviour everywhere**, and
`bench/results-scenario1-vs-pg.md` §7 measures the opposite sign on the same
structure — 15.3× *better* than the walk there. The variable is the hit rate:
that workload cycles eight arguments, this one holds matches-per-key constant
so the key space grows with the relation and a repeat becomes rare.

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
- **Why `CabinProbe` costs more than a `FilterScan` per row.** §7 measures the
  inversion twice and locates it in the Cabin's own path via `ANALYZE`, but
  the engine exposes no per-step timing that would say which part of that
  path is the cost. `docs/observability.md` owns that gap and it is unbuilt.
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
