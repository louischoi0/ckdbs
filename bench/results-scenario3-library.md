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
does.** At 10,000 rows a `WHERE user_id = ?` goes from a 631 µs walk to a
**45.6 µs probe — 13.8×** — and lands **1.8× faster than PostgreSQL's own
indexed answer** (82.0 µs). Every equality shape in the matrix behaves the
same way. On this workload, with an index declared, KDS is the faster engine
on nine of ten read shapes.

**The tenth shape is a 45× loss, and it is one missing optimization.** A join
whose restriction sits on the *other* relation — `loans JOIN users ON
l.user_id = u.id WHERE u.id = 3` — compiles to a full `Scan` of `loans` and a
pk `Probe` per row: 10,000 opens, 10,086 pages, to return 6 rows. The
identical predicate written against one relation compiles to an `IndexProbe`
and reads **7 pages**. KDS does not push an equality through a join key, so
the index it has is invisible to the join. PostgreSQL pushes it, reads 14
buffers, and finishes in 99.4 µs against KDS's 4,457.

**The Cabin still does not convert the cost, and at 10,000 rows it doubles
it.** On the same column and the same predicate it costs **1,122.8 µs against
the 631.3 µs walk it was meant to replace** — 1.8× worse than doing nothing
and 24.6× worse than the index. This reproduces the inversion the previous
measurement of this file found, on a different engine build and a cleaner
noise floor.

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
the widest disagreement between a pair over the ten shapes, **dividing by the
smaller of each pair**, which is the larger of the two possible percentages
and therefore the conservative one.

| replicate pair | N | engine | max abs. Δ p50 | median abs. Δ p50 | max abs. Δ mean |
|---|---:|---|---:|---:|---:|
| `ck-none-200` vs `ck-none-rep-200` | 200 | ckdbs | 3.1% | 1.4% | 6.7% |
| `ck-single-200` vs `ck-single-rep-200` | 200 | ckdbs | 2.4% | 0.7% | 9.4% |
| `ck-cabin-200` vs `ck-cabin-rep-200` | 200 | ckdbs | 3.7% | 1.2% | 7.3% |
| `ck-none-1000` vs `ck-none-rep-1000` | 1,000 | ckdbs | 10.4% | 2.6% | 8.5% |
| `ck-single-1000` vs `ck-single-rep-1000` | 1,000 | ckdbs | 4.6% | 1.8% | 8.4% |
| `ck-cabin-1000` vs `ck-cabin-rep-1000` | 1,000 | ckdbs | 16.6% | 2.1% | 9.1% |
| `ck-none-10000` vs `ck-none-rep-10000` | 10,000 | ckdbs | 18.8% | 10.6% | 23.6% |
| `ck-single-10000` vs `ck-single-rep-10000` | 10,000 | ckdbs | 27.5% | 1.0% | 18.1% |
| `ck-cabin-10000` vs `ck-cabin-rep-10000` | 10,000 | ckdbs | 2.9% | 0.8% | 10.7% |
| `pg-none-200` vs `pg-none-rep-200` | 200 | postgresql | 17.4% | 12.9% | 44.0% |
| `pg-single-200` vs `pg-single-rep-200` | 200 | postgresql | 17.4% | 12.5% | 17.3% |
| `pg-none-10000` vs `pg-none-rep-10000` | 10,000 | postgresql | 2.2% | 1.7% | 5.6% |
| `pg-single-10000` vs `pg-single-rep-10000` | 10,000 | postgresql | 9.4% | 1.1% | 11.5% |

**The floors adopted below: ckdbs ±3.7% at 200, ±16.6% at 1,000, ±27.5% at
10,000; PostgreSQL ±17.4% at 200 and ±9.4% at 10,000.** The mean is noisier
than p50 in twelve of thirteen pairs, so no claim here rests on a mean.

**The floor is an order of magnitude tighter than the previous measurement of
this file managed** — 3.7% against 29.4% at 200 rows — and the difference is
the quiet gate, not the machine, which is the same one. That is worth
recording as a method result: on a two-vCPU box shared with other build
sessions, gating each cell on an idle machine and discarding any cell a
compiler ran during is the difference between a matrix that can resolve 10%
and one that could not resolve a factor of two.

**Where the floor is still too generous.** `pk-user` reaches one row through
the pk, and no configuration in this matrix should move it:

| N | spread of `pk-user` p50 across the four configurations | floor | inside? |
|---:|---|---:|---|
| 200 | 38.5–39.6 µs — **2.9%** | 3.7% | yes |
| 1,000 | 36.3–39.3 µs — **8.3%** | 16.6% | yes |
| 10,000 | 30.5–39.2 µs — **28.5%** | 27.5% | **no, marginally** |

At 10,000 the control moves fractionally more than the floor, driven by one
cell (`ck-single-10000`, 30.5 µs against ~39 µs elsewhere). **Consequence: no
finding below rests on anything under 30%.** Every one that is reported is a
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

p50 µs per shape. `off` is `indexes = off` — the index declared, backfilled
and maintained, with only the *read* path disabled, which is what isolates
the read benefit from the write-side cost.

**N = 200**

| shape | ck none | ck single | ck cabin | ck off | pg none | pg single |
|---|---:|---:|---:|---:|---:|---:|
| pk-user | 38.6 | 38.5 | 39.1 | 39.6 | 59.6 | 60.0 |
| loans-by-user | 48.7 | 42.4 | 40.2 | 50.0 | 73.8 | 79.0 |
| loans-by-book | 47.6 | 42.2 | 48.6 | 49.0 | 72.0 | 77.2 |
| resv-by-user | 42.8 | 39.1 | 42.1 | 33.9 | 60.4 | 66.9 |
| books-by-author | 40.4 | 40.6 | 40.7 | 32.1 | 62.6 | 67.5 |
| books-by-genre | 40.1 | 39.3 | 39.4 | 31.0 | 60.7 | 65.6 |
| loans-by-daterange | 50.0 | 49.8 | 50.6 | 50.9 | 82.4 | 85.0 |
| overdue | 50.2 | 51.3 | 51.6 | 52.2 | 80.9 | 83.7 |
| **join-loan-user** | 126.0 | 123.6 | 123.8 | 124.1 | **97.2** | **99.1** |
| count-by-user | 46.5 | 40.2 | 37.0 | 47.9 | 71.6 | 76.7 |

**N = 1,000**

| shape | ck none | ck single | ck cabin | ck off | pg none | pg single |
|---|---:|---:|---:|---:|---:|---:|
| pk-user | 38.8 | 39.3 | 36.3 | 39.2 | 62.4 | 66.4 |
| loans-by-user | 90.0 | **44.4** | 145.6 | 91.2 | 104.3 | 79.9 |
| loans-by-book | 80.9 | **44.4** | 89.6 | 90.7 | 105.2 | 78.1 |
| resv-by-user | 63.3 | 43.1 | 63.2 | 64.9 | 79.6 | 73.1 |
| books-by-author | 48.6 | 42.2 | 50.3 | 51.3 | 71.5 | 76.7 |
| books-by-genre | 50.0 | 47.0 | 51.8 | 52.2 | 84.2 | 87.8 |
| loans-by-daterange | 103.2 | 103.8 | 103.8 | 103.5 | 169.9 | 172.0 |
| overdue | 108.1 | 109.2 | 108.4 | 108.1 | 155.5 | 158.3 |
| **join-loan-user** | 457.6 | 467.7 | 468.6 | 464.1 | **135.0** | **110.0** |
| count-by-user | 86.7 | **41.4** | 38.7 | 89.8 | 103.6 | 79.0 |

**N = 10,000** — the column every conclusion rests on

| shape | ck none | ck single | ck cabin | ck off | pg none | pg single |
|---|---:|---:|---:|---:|---:|---:|
| pk-user | 39.2 | 30.5 | 38.8 | 35.4 | 62.8 | 62.3 |
| loans-by-user | 631.3 | **45.6** | **1,122.8** | 555.5 | 449.1 | 82.0 |
| loans-by-book | 632.7 | **44.6** | 552.9 | 555.3 | 467.6 | 78.8 |
| resv-by-user | 354.6 | **42.3** | 292.6 | 304.0 | 251.9 | 72.3 |
| books-by-author | 168.8 | **44.1** | 158.1 | 154.5 | 148.9 | 76.9 |
| books-by-genre | 190.2 | 108.6 | 183.5 | 176.2 | 329.3 | 260.0 |
| loans-by-daterange | 778.9 | 705.9 | 705.9 | 702.3 | 959.0 | 985.0 |
| overdue | 842.6 | 763.7 | 763.1 | 759.9 | 943.7 | 1,034.5 |
| **join-loan-user** | 4,591.7 | 4,457.1 | 4,499.8 | 4,684.8 | **476.3** | **99.4** |
| count-by-user | 550.8 | **40.6** | 1,113.3 | 550.1 | 448.4 | 71.1 |

## 6. The index converts a per-row cost into a fixed one

The five equality shapes, `none` against `single`, at 10,000 rows:

| shape | none | single | ratio | clears the 27.5% floor? |
|---|---:|---:|---:|---|
| loans-by-user | 631.3 | 45.6 | **13.8×** | yes |
| loans-by-book | 632.7 | 44.6 | **14.2×** | yes |
| resv-by-user | 354.6 | 42.3 | **8.4×** | yes |
| books-by-author | 168.8 | 44.1 | **3.8×** | yes |
| books-by-genre | 190.2 | 108.6 | 1.8× | yes |
| count-by-user | 550.8 | 40.6 | **13.6×** | yes |

**The indexed answer is the same 40–46 µs at every cardinality** — 42.4 µs at
200 rows, 44.4 at 1,000, 45.6 at 10,000 for `loans-by-user` — while the
un-indexed one grows 48.7 → 90.0 → 631.3. That is the thesis of this file
measured directly: the walk is O(rows) and the probe is not, and a probe at
10,000 rows costs what a probe at 200 rows costs.

**`books-by-genre` is the counter-case and behaves like one.** Genre
cardinality is fixed at 16, so its result set grows with the relation; the
index still helps (1.8×), but it cannot make the shape fixed-cost, because
the rows must still be returned. An index removes the cost of *finding*
rows, never the cost of *having* them.

**The read benefit and the whole cost are not the same number.** `ck off`
keeps the index declared, backfilled and maintained and disables only the
read path. At 10,000 rows `loans-by-user` reads 555.5 µs there against
631.3 µs with no index at all — the two agree within the floor, which is the
expected result and the useful control: it says the 13.8× in the table above
is the read path and nothing else. The index's own cost is elsewhere:

| N | ckdbs `create-index`, 5 indexes | PostgreSQL, 9 indexes | per index, ckdbs | per index, PG |
|---:|---:|---:|---:|---:|
| 200 | 0.6 ms | 28.1 ms | 119 µs | 3,120 µs |
| 1,000 | 2.1 ms | 31.4 ms | 410 µs | 3,494 µs |
| 10,000 | 18.6 ms | 64.8 ms | 3,713 µs | 7,199 µs |

The backfill is where an index is paid for on this workload, and ckdbs's is
cheaper at every size — though the two engines declare different numbers of
indexes, so the per-index column is the comparable one.

## 7. The Cabin costs more than the walk it replaces

Same column, same predicate, at 10,000 rows:

| | walk (`none`) | Cabin | index (`single`) |
|---|---:|---:|---:|
| loans-by-user p50 | 631.3 | **1,122.8** | 45.6 |
| count-by-user p50 | 550.8 | **1,113.3** | 40.6 |
| vs the walk | — | **1.78× / 2.02× worse** | 13.8× / 13.6× better |
| vs the index | 13.8× worse | **24.6× worse** | — |

Both figures are far outside the 27.5% floor. The Cabin is doing what
`ANALYZE` says it is doing — `loans-by-user` compiles to `CabinProbe`, the
only shape it changes — and that path is slower than the `FilterScan` it
replaced. At 1,000 rows the same shape is 145.6 µs against a 90.0 µs walk,
1.6× worse; at 200 rows it is 40.2 against 48.7, marginally better and inside
the floor. **The Cabin's disadvantage grows with the relation**, which is the
signature of a per-row cost, not of a fixed setup charge amortised over
repeats.

That reproduces, on a different engine build and a floor an order of
magnitude tighter, the inversion the previous measurement of this file
reported. It is now measured twice and should be treated as the Cabin's
behaviour on this shape rather than as an artefact.

`count-by-user` under the Cabin is the sharper number: the same fold that
costs 40.6 µs over an index costs **1,113.3 µs** over a Cabin, 27×.

## 8. Composite is the only structure that helps `overdue`

`overdue` filters two columns (`due_day BETWEEN ? AND ?` and `status = ?`),
which no single-column index satisfies — §4 shows it staying a `FilterScan`
under `single`. At 10,000 rows:

| shape | `single` | `composite` | `covering` |
|---|---:|---:|---:|
| **overdue** | 763.7 | **218.2** | 761.0 |
| loans-by-user | 45.6 | 555.2 | 44.7 |
| loans-by-book | 44.6 | 554.7 | 569.5 |
| resv-by-user | 42.3 | 292.2 | 291.9 |
| books-by-author | 44.1 | 188.3 | 156.3 |
| `create-index` total | 18.6 ms (5) | 8.2 ms (2) | 6.9 ms (1) |

**A composite key turns `overdue` from a walk into a 3.5× cheaper access**,
and it is the only structure in this matrix that touches that shape. It buys
that by declaring two indexes instead of five, so the single-column shapes it
no longer covers fall back to the walk — which is not a defect of composite
keys but of this matrix's `--index-mode` being one choice for the whole
schema rather than per shape.

**`covering` bought nothing measurable and cost the most per index.** Its one
index leaves `loans-by-user` where `single` had it (44.7 against 45.6, inside
the floor) and leaves every other shape on the walk, while costing 6,872 µs
to build against `single`'s 3,713 µs per index. On this workload the covering
form has no read benefit to show; a shape whose projection is entirely inside
the key is what would demonstrate one, and this matrix does not contain one.

## 9. The join is the engine's one large loss, and it is one optimization

`join-loan-user` is `SELECT l.book_id, l.due_day, u.member_code FROM loans l
JOIN users u ON l.user_id = u.id WHERE u.id = ?` — six rows out. At 10,000
rows ckdbs takes **4,457 µs against PostgreSQL's 99.4 µs, 45×**, and the
index makes no difference at all (4,591.7 without it, 4,457.1 with it, inside
the floor).

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
serves this predicate in 45.6 µs when it is written without a join. **The
missing piece is equality propagation through a join key**: deriving
`l.user_id = 3` from `l.user_id = u.id` and `u.id = 3`, which would turn step
0 from a `Scan` into the `IndexProbe` the engine already has. That is one
plan-time rewrite, and on this shape it is worth 45×.

It is also the one shape where ckdbs loses at *every* size — 126.0 against
97.2 µs at 200 rows, 457.6 against 135.0 at 1,000 — and the only shape whose
gap widens with the relation, because the scanned side grows and the
propagated form would not.

## 10. Durability decides the load, and nothing else here

The read shapes are identical under all three durability classes — every
figure in `ck-dur-relaxed-10000` and `ck-dur-strict-10000` sits inside the
floor of `ck-single-10000`, which is what should happen to a read. The load
is a different matter. 19,000 rows, one row per statement, no batching:

| `durability` | per row | whole load | vs `relaxed` |
|---|---:|---:|---:|
| `relaxed` | **34 µs** | 0.64 s | — |
| `group` | 1,044 µs | 19.84 s | **31×** |
| `strict` | 1,247 µs | 23.69 s | **37×** |

An unbatched row-at-a-time load is entirely fsync-bound: **relaxing the
durability promise is worth 31×**, which is the same finding
`bench/results-scenario2-freight.md` reaches from the other direction when it
prices autocommit at 5.6×. `group` and `strict` differ by only 19% because a
group batch formed from one connection is a batch of one — the same reason
that file's concurrency section gives.

**PostgreSQL's load costs the same as ckdbs's `group`**: 1,108 µs per row
against 1,085 (`none`), 2% apart, both engines paying one fsync per
statement to the same filesystem. That is the cross-engine confirmation the
number needs — this is the device's price for a durable single-row insert,
not either engine's.

## 11. Versus PostgreSQL

Nine of ten shapes go to ckdbs at 10,000 rows, the tenth by 45×. Ratios are
`pg-single` ÷ `ck-single`, so above 1 means ckdbs is faster:

| shape | ck single | pg single | ratio |
|---|---:|---:|---:|
| pk-user | 30.5 | 62.3 | **2.04×** |
| loans-by-user | 45.6 | 82.0 | **1.80×** |
| loans-by-book | 44.6 | 78.8 | **1.77×** |
| resv-by-user | 42.3 | 72.3 | **1.71×** |
| books-by-author | 44.1 | 76.9 | **1.74×** |
| books-by-genre | 108.6 | 260.0 | **2.39×** |
| loans-by-daterange | 705.9 | 985.0 | **1.40×** |
| overdue | 763.7 | 1,034.5 | **1.35×** |
| **join-loan-user** | 4,457.1 | 99.4 | **0.02× — 45× slower** |
| count-by-user | 40.6 | 71.1 | **1.75×** |

**Read the 1.4×–2.4× column with the same caution scenario2's does.** A
single-shape read here is 40–100 µs of client-measured round trip, and KDS's
newline text protocol is a lighter round trip than PostgreSQL's v3 wire; part
of every ratio above is protocol rather than engine. What the column
establishes is that KDS is not paying a penalty anywhere in this workload's
read paths — not that it is twice the engine.

Three rows do carry more than protocol, because their absolute cost is large
enough to swamp the round trip:

- **`loans-by-daterange` and `overdue`, 700–760 µs against 960–1,030.** Both
  are full walks on both engines — PostgreSQL's own `EXPLAIN` shows a
  `Seq Scan` for `overdue` — so this is scan against scan, and KDS's is
  ~1.35× faster over the same 10,000 rows.
- **`join-loan-user`, the 45×.** §9 is the whole explanation and it is a
  missing plan-time rewrite, not a slow executor.

**PostgreSQL declines its own index at 200 rows and KDS does not**, which
§4 predicted and this table prices: at 200 rows `pg-single` is *slower* than
`pg-none` on eight of ten shapes (79.0 against 73.8 on `loans-by-user`),
because the planner correctly judges the index not worth its setup on a
40-row relation and pays for the declaration anyway. KDS descends
unconditionally and comes out at 42.4 against 48.7 — better here, but for a
reason that is not a cost model, and a shape can be constructed where
descending unconditionally is the wrong choice.

**The `--cabin` cells have no PostgreSQL column**, and the twin does not
invent one. There is no PostgreSQL equivalent of a structure that is
authoritative only for values a query has already observed, which is why §7
is a ckdbs-only comparison against ckdbs's own alternatives.

## 12. What this run does not answer

- **Whether the join's 45× survives its fix.** §9 identifies the missing
  rewrite and prices the gap; it does not prove that propagating the equality
  would reach the 45.6 µs the single-relation form gets, because the join
  still has a second step to run. The measurement to make after the fix is
  this same cell.
- **Why `CabinProbe` costs more than a `FilterScan` per row.** §7 measures the
  inversion twice and locates it in the Cabin's own path via `ANALYZE`, but
  the engine exposes no per-step timing that would say which part of that
  path is the cost. `docs/observability.md` owns that gap and it is unbuilt.
- **The Cabin's hit rate, and the space each structure costs.** This run
  measures neither. The superseded
  `bench/results-scenario3-library-2026-08-08.md` §11 does — it models the
  hit rate as a coupon-collector function of key space against probe count,
  and it counts the pages a Cabin and an index each add to the data file.
  Those are the mechanism behind §7's inversion and they are why that file is
  still in the tree; its **numbers** are a different machine and a different
  engine and must not be differenced against anything here.
- **What a covering index is worth.** §8 shows this matrix contains no shape
  whose projection fits inside a covering key, so the `covering` column
  measures its cost and none of its benefit.
- **Anything about concurrency.** Every cell is one connection.
  `scenario0_stockmarket.py` and scenario2's §11 are where contention is
  measured; nothing here shares a row with anyone.
- **Whether the index's write-side cost is acceptable.** §6 prices the
  backfill. The per-write maintenance cost is inside the load phase and is
  not separated from it here: `ck-single-10000`'s load is 1,044 µs per row
  against `ck-none-10000`'s 1,085, which says only that both are fsync-bound
  at a granularity that hides the difference.
