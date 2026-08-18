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
| **join-loan-user** | 223 | 8,850 | **0.03× — 40× short** |
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

- **Whether the join's 45× survives its fix.** §9 identifies the missing
  rewrite and prices the gap; it does not prove that propagating the equality
  would reach the 21,322 statements a second the single-relation form gets,
  because the join still has a second step to run. The measurement to make after the fix is
  this same cell.
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
