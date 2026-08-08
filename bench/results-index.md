# Secondary indexes, measured

`docs/workplan-index.md` IX14. What a secondary index buys on the read side,
what `COVERING` buys on top of that, what it costs on the write side, and how
all three compare against PostgreSQL.

The thesis, and every number below is in service of it: **a secondary index in
this engine converts a per-row cost into a fixed one, and that conversion is
the entire value of the structure.** It is worth 9.7× on a selective equality
over 10,000 rows, 1.9× over 1,000, and 1.11× over 200 — where on a range it
is in fact an 11% *loss*. Everything else the feature does, `COVERING` included,
moves the constant rather than the exponent.

| | |
|---|---|
| **Run** | 2026-08-08, 08:55:34 – 08:58:10 UTC |
| **Branch / commit** | `feat-index` / `c4dda90` ("index: the equivalence suite (IX12)") |
| **Tree** | at the moment of the run, dirty by exactly two **untracked** files — `tools/index_benchmark.py`, `tools/pg_index_benchmark.py`, both written for it. No tracked file, and no engine source, differs from `c4dda90`; nothing in `src/`, `include/` or `tests/` was touched to produce any number here. |
| **Binary provenance** | `build-release/kds_server`, mtime 2026-08-08 08:30:45 UTC, against a HEAD committed 08:27:26 UTC — the binary is **newer than HEAD** and so measures the engine at `c4dda90`. |
| **Build type** | Release (`build-release`). `./build` is Debug and is not used here. |
| **Device** | `/dev/nvme0n1p1` (NVMe SSD, `ROTA=0`) mounted at `/`, data files under `/home/ec2-user/bench-index`. **Not tmpfs**: `/tmp` on this host is tmpfs and would make every fsync free. |
| **Machine** | AMD EPYC 7571, 2 vCPU, 7 GiB, kernel 6.18.38. Load average across the run 0.44 → 1.96 (1-minute), recorded per configuration; no build running (`pgrep cc1plus` empty). |
| **Server config** | `cores = 1`, `inline_cell_width = 64`, `waystone_recording/replay = on`, `access_statistics = on`, `cabins` unused. `indexes` and `durability` are the two keys varied — one server process and one **fresh data file** per combination. |
| **Baseline** | PostgreSQL 17.10, `tools/pg_setup.sh` cluster on port 15433, **default tuning**, `synchronous_commit = on`. |
| **Correctness** | `--verify` passed on every one of the twelve runs: every read shape's reply from an indexed relation equals the unindexed one's **row for row and in order**, and both indexed write relations answer a `region` equality identically to the unindexed one's walk. `build-release/tests/kds_tests --gtest_filter='Index*:*Index*'` — 121 tests, all passing, on the same binary. |

Drivers, flags and exact invocations: `bench/docs/README.md`
(`index_benchmark.py` and its twin `pg_index_benchmark.py`). This file states
findings; it does not re-explain how to run the tools.

---

## 0. How the comparison is kept honest

Three read relations hold **byte-identical contents** and differ only in what
is declared over them: `none` carries no index, `idx` one on `cust_id`, `cov`
the same key with `COVERING (status)`. Every shape is driven with the **same
argument against all three inside one operation**, so the three columns of
every table below are one comparison rather than three runs. The write half
is the same arrangement: `w0`/`w1`/`w2` with 0, 1 and 2 indexes, loaded and
updated with identical values, interleaved.

**Customers scale with rows** (`rows / 6`), so `WHERE cust_id = ?` answers
with 3–4 rows at every size and only the relation grows; the range spans a
fixed 10 customers, so it answers with ~60–69 rows at every size. Without
that, a bigger relation would also be a less selective predicate and the sweep
would move two variables at once.

**The noise floor is measured from inside the run.** `eq` is executed twice on
each relation — once as `eq`, once as `eq-again` — and the two are the same
configuration by construction:

| rows | `none` | `idx` | `cov` | PostgreSQL `idx` |
|---:|---:|---:|---:|---:|
| 200 | 0.38% | 0.77% | 0.84% | 0.46% |
| 1,000 | 0.99% | 0.28% | 0.42% | 1.16% |
| 10,000 | 0.89% | 1.65% | 1.82% | 0.83% |

**The floor is 1.8% at p50.** Nothing smaller than that is reported as a
result below. A `PING` phase (`SELECT 1` on the PostgreSQL side) measures the
client and socket round trip with no engine work behind it, and a pk lookup is
a second control: it descends the clustered tree and touches no secondary
structure, so it must not move when an index is added. It does not — 130.0 /
130.5 / 130.6 µs across `none` / `idx` / `cov` at 10,000 rows, a 0.5% spread.

---

## 1. The headline: a selective non-pk equality

`SELECT id, region, status, amount FROM t WHERE cust_id = ?`, ~4 matching
rows at every size.

**rows = 200** — the index is 1.11× faster, a 9.6% cut. That is five times
the noise floor and still not a reason to declare an index.

| config | ops | p0 | p25 | p50 | p95 | p99 | mean |
|---|---:|---:|---:|---:|---:|---:|---:|
| no index — `FilterScan` | 300 | 124 | 154 | 157 | 176 | 290 | 160 |
| index — `IndexProbe` | 300 | 91 | 137 | 142 | 160 | 190 | 141 |
| index — `IndexProbe`, repeated (the floor) | 300 | 96 | 137 | 141 | 157 | 176 | 141 |
| index, `indexes = off` — walks | 300 | 108 | 154 | 157 | 182 | 225 | 159 |
| covering index — `IndexProbe` | 300 | 88 | 138 | 143 | 157 | 193 | 141 |
| pk lookup (control) | 300 | 87 | 127 | 131 | 155 | 188 | 133 |
| `PING` (client + socket floor) | 300 | 51 | 93 | 93 | 100 | 110 | 94 |

**rows = 1,000** — 1.9×.

| config | ops | p0 | p25 | p50 | p95 | p99 | mean |
|---|---:|---:|---:|---:|---:|---:|---:|
| no index — `FilterScan` | 300 | 241 | 260 | 271 | 449 | 1218 | 326 |
| index — `IndexProbe` | 300 | 100 | 131 | 142 | 178 | 231 | 144 |
| index — `IndexProbe`, repeated (the floor) | 300 | 95 | 128 | 141 | 178 | 354 | 164 |
| index, `indexes = off` — walks | 300 | 214 | 261 | 265 | 354 | 443 | 310 |
| covering index — `IndexProbe` | 300 | 93 | 129 | 142 | 192 | 270 | 176 |
| pk lookup (control) | 300 | 86 | 113 | 129 | 168 | 297 | 144 |
| `PING` (client + socket floor) | 300 | 46 | 85 | 93 | 110 | 178 | 97 |

**rows = 10,000** — 9.7×.

| config | ops | p0 | p25 | p50 | p95 | p99 | mean |
|---|---:|---:|---:|---:|---:|---:|---:|
| no index — `FilterScan` | 300 | 1413 | 1434 | 1472 | 2292 | 2937 | 1689 |
| index — `IndexProbe` | 300 | 103 | 145 | 152 | 189 | 326 | 156 |
| index — `IndexProbe`, repeated (the floor) | 300 | 107 | 144 | 149 | 184 | 234 | 152 |
| index, `indexes = off` — walks | 300 | 1427 | 1477 | 1599 | 2326 | 2792 | 1728 |
| covering index — `IndexProbe` | 300 | 110 | 140 | 148 | 178 | 218 | 149 |
| pk lookup (control) | 300 | 87 | 125 | 130 | 159 | 220 | 134 |
| `PING` (client + socket floor) | 300 | 45 | 63 | 75 | 106 | 138 | 78 |

The shape of the win is clearer once the client is subtracted. Using each
run's own `PING` p50 as the client-and-socket term:

| rows | walk, engine µs | probe, engine µs | ratio | rows examined, walk → probe |
|---:|---:|---:|---:|---|
| 200 | 64 | 49 | 1.3× | 200 → 4 |
| 1,000 | 178 | 49 | 3.6× | 1,000 → 3 |
| 10,000 | 1,397 | 77 | 18× | 10,000 → 4 |

**The probe's engine cost is 49 µs at 200 rows and 77 µs at 10,000** — a 1.6×
rise over a 50× rise in relation size, which is the `log n` the structure
promises plus a colder buffer pool. The walk's rises 22×, i.e. linearly, as it
must. `ANALYZE` says the same thing without any timing at all: `examined` goes
`10000 → 4` and the index scans exactly the four entries it resolves.

Two secondary shapes, at 10,000 rows and p50:

| shape | no index | index | ratio |
|---|---:|---:|---:|
| `WHERE cust_id BETWEEN a AND a+10` (69 rows) | 1,818 | 258 | 7.1× |
| `SELECT COUNT(*) ... WHERE cust_id = ?` | 1,469 | 146 | 10.1× |

**A range at 200 rows is the one place the index loses**: 206 µs against the
walk's 187 µs, +11%, well outside the floor. Sixty entries scanned and sixty
pks descended costs more than reading 200 rows straight through.
That is not a defect and it is not fixable by tuning — it is the crossover,
and it says the compiler's `f(shape, catalog)` selection rule (`feat-index.md`
§9, no statistics, no cardinality estimate) will always pick the index there.
The engine has no mechanism to decline one and, by IX9's deliberate design,
no data with which to.

### 1a. One attempt to tune it anyway, and why it failed

**Tested and reverted 2026-08-08.** The 19 µs gap looks like it should be the
sixty `BtreeLookup` calls: each descends from the root, and IX8a already sorts
those pks into primary-key order, which on a btree relation *is* leaf order —
so a one-leaf memo in phase 2 should collapse a run of them into one descent.
It was built (`BtreeLookupInLeaf` plus a memo, ~40 lines) and measured. Both
halves of the hypothesis were wrong:

| | 200 rows | 10,000 rows |
|---|---:|---:|
| descents avoided | **57 of 60** | **11 of 69** |
| `range` p50, with memo | 202.9 µs | 355.4 µs |
| `range` p50, without | 183.4 µs | **250.2 µs** |

At 200 rows the memo removed 95% of the descents and **the latency did not
move** — the relation is one leaf, so a descent was already "fetch this page
and search it", which is exactly what the memo replaced it with. At 10,000
rows it is a **42% regression**: a secondary index's matching pks are
*scattered across the relation by construction*, so the hit rate collapses to
16% and each of the other 84% pays a wasted page fetch and leaf search before
descending anyway.

What the 19 µs actually is, with the client subtracted: the walk costs
**0.46 µs per row examined** and the index **1.85 µs per row resolved**. At
60 matches in 200 rows — 30% selectivity — reading everything sequentially
inside one already-fetched page beats resolving each row through the tree.
That is the ordinary shape of the result rather than an artifact, and it is
why PostgreSQL's planner picks a `Seq Scan` on the same shape at the same
size.

The lesson is `workplan-aggregate-perf.md`'s, earned a third time: **the
stated cause was plausible, cheap to test, and wrong.** Anyone reaching for
this again should measure the memo's hit rate at the size they care about
first — it is one `ANALYZE` line, and it is 16%.

---

## 2. The `indexes` switch is a faithful proxy for not having an index

`indexes = off` makes an index step walk while leaving the compiled chain
untouched (`feat-index.md` §12.3): `ANALYZE` still says `IndexProbe`, and
`examined` becomes 10,000. Within a single `indexes = off` run, where all
three relations walk over identical data, the downgraded index step costs what
the compiled `FilterScan` costs:

| rows | `eq` `FilterScan` | `eq` downgraded `IndexProbe` | delta | `range` delta | `count-eq` delta |
|---:|---:|---:|---:|---:|---:|
| 200 | 156.5 | 157.2 | +0.4% | +0.2% | +0.7% |
| 1,000 | 263.6 | 264.6 | +0.4% | +0.8% | +0.4% |
| 10,000 | 1,566.6 | 1,599.0 | +2.1% | −0.0% | +0.9% |

At 200 and 1,000 rows the penalty is a consistent +0.4%, at or just under the
floor; at 10,000 the twelve cells (six shapes × two indexed relations) straddle
zero, from −1.3% to +2.9%. **Turning the switch off gives back the
FilterScan's cost and no more.** That is worth
stating because it means the switch is a legitimate A/B for the read path, and
because it says the index step's fall-through walk is not carrying overhead
from the branch it did not take.

One methodological finding worth more than it looks. Comparing `eq[none]`
*across* runs — 1,472 µs in the `indexes = on` run against 1,566 µs in the
`indexes = off` run — suggests a 6.4% difference in a workload that is
identical in both. It is drift between two server processes on two data files
minutes apart, and it is 3.5× the in-run noise floor. Every cross-run
comparison in this document is therefore backed by an in-run one, and the
tables above are built from the in-run form.

---

## 3. `COVERING` buys the avoided descents, and measurably nothing else

`feat-index.md` §7 makes a design claim that a benchmark can falsify: a
covering index avoids the **base descent for rows that will be filtered out**,
never the base read for rows that are returned, because visibility is decided
only at `ChainRunner::AcceptTupleAt` from the tuple itself. `ANALYZE` prices
it directly — `index_filtered` counts descents the covered columns avoided.

**The claim survives.** Split the four read shapes by whether the entry can
decide anything:

| shape | `index_filtered` (10K) | plain index p50 | covering p50 | delta |
|---|---:|---:|---:|---:|
| `eq` — no residual on a covered column | 0 | 151.9 | 148.0 | −2.6% |
| `range` — no residual on a covered column | 0 | 257.8 | 258.3 | +0.2% |
| `eq-covered` — `AND status = ?`, 4 of 4 dropped | 4 | 148.8 | 141.5 | −4.9% |
| `range-covered` — `AND status = ?`, 60 of 69 dropped | 60 | 215.7 | 170.2 | **−21.1%** |

Across all three sizes, the two shapes with `index_filtered = 0` come out at
+0.4%, −0.2%, −2.6%, +0.7%, +1.3% and +0.2% — six cells straddling zero with
no consistent sign, inside or barely outside a 1.8% floor. **Where a covered
column has nothing to filter, `COVERING` bought exactly the write cost it
added.** That is §7's claim stated as a null result, and it is the harder half
to demonstrate.

Where it does filter, the win is proportional to the descents dropped and
stable across the sweep:

| rows | entries scanned | descents avoided | rows resolved | plain p50 | covering p50 | saved | **µs per avoided descent** |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 200 | 60 | 48 | 12 | 189.7 | 161.7 | 28.0 | 0.58 |
| 1,000 | 68 | 56 | 12 | 200.5 | 164.8 | 35.7 | 0.64 |
| 10,000 | 69 | 60 | 9 | 215.7 | 170.2 | 45.5 | 0.76 |

**A base descent on a btree relation costs 0.58–0.76 µs, and it gets dearer as
the relation grows** — the same `log n` that makes the probe itself cheap
makes each avoided descent worth more. This is the number `COVERING` should be
budgeted against: it pays only in proportion to `index_filtered`, and
`ANALYZE` reports that per statement before anyone commits to the write cost.

**There is no index-only scan, and the numbers show it rather than assert
it.** `SELECT COUNT(*) ... WHERE cust_id = ?` on the covering relation costs
144.3 µs at 10,000 rows against `eq`'s 148.0 µs for returning the same rows'
full contents — a 2.5% difference, inside the floor. Counting costs what
fetching costs, because the base tuple is read either way. PostgreSQL, which
has the visibility map KDS does not, serves that statement with an `Index Only
Scan` and `Heap Fetches: 0`, and pays 215 µs against 240 for `eq` — 10%
cheaper. **The measured price of the missing visibility witness is that 10%,
on the one shape where a witness would help most.** It is a small number here
for a reason that is a property of this workload rather than of the design: at
3–4 matching rows there are only 3–4 descents for an index-only scan to save.
A `COUNT(*)` over a key matching thousands of rows would separate the two
engines much further, and this run does not measure that shape.

---

## 4. The write path: 2–4.5 µs per index, invisible under an fsync

Index maintenance is not switchable — an index that stops being maintained is
wrong rather than slow, so `DROP INDEX` is the only way to stop paying
(`feat-index.md` §12.3). The cost is therefore measured against relations
carrying 0, 1 and 2 indexes, loaded with identical rows, interleaved.

At the server's **default `durability = group`** with one connection — a batch
of one is a batch, so this is one fsync per INSERT — the cost is not
resolvable at all:

| rows | 0 indexes p50 | 1 index p50 | 2 indexes p50 | delta, 1 / 2 indexes |
|---:|---:|---:|---:|---|
| 200 | 440 | 451 | 453 | +2.5% / +3.0% |
| 1,000 | 1,025 | 1,036 | 1,036 | +1.1% / +1.1% |
| 10,000 | 1,045 | 1,051 | 1,054 | +0.6% / +0.9% |

At 10,000 rows that is **+6 µs and +9 µs on a 1,045 µs statement**, of which
933 µs is the device. The same phases with `durability = relaxed`, which takes
the fsync out of the statement, resolve it:

| config | ops | p0 | p25 | p50 | p95 | p99 | mean |
|---|---:|---:|---:|---:|---:|---:|---:|
| **rows = 200** | | | | | | | |
| insert, 0 indexes | 200 | 60 | 110 | 113 | 134 | 190 | 111 |
| insert, 1 index | 200 | 64 | 114 | 117 | 133 | 168 | 115 |
| insert, 2 indexes | 200 | 66 | 116 | 119 | 136 | 159 | 117 |
| **rows = 1,000** | | | | | | | |
| insert, 0 indexes | 1000 | 61 | 101 | 112 | 132 | 154 | 107 |
| insert, 1 index | 1000 | 64 | 106 | 117 | 140 | 175 | 114 |
| insert, 2 indexes | 1000 | 66 | 107 | 119 | 141 | 180 | 115 |
| **rows = 10,000** | | | | | | | |
| insert, 0 indexes | 10000 | 61 | 107 | 112 | 131 | 159 | 111 |
| insert, 1 index | 10000 | 64 | 112 | 117 | 137 | 167 | 115 |
| insert, 2 indexes | 10000 | 67 | 114 | 119 | 139 | 167 | 118 |

**An index costs a few microseconds per INSERT and the number does not move
with the relation size.** Absolute p50 deltas against the 0-index relation:

| rows | 1 index | 2 indexes | first index | second index |
|---:|---:|---:|---:|---:|
| 200 | +4.0 µs | +5.9 µs | +4.0 | +1.9 |
| 1,000 | +4.4 µs | +6.5 µs | +4.4 | +2.1 |
| 10,000 | +4.5 µs | +7.0 µs | +4.5 | +2.5 |

Two things to read out of it. **Flat in row count across a 50× range** —
which is what a two-level tree predicts, and `SHOW INDEXES` reports
`height = 2` at both 1,000 and 10,000 rows. And **the marginal index costs
about half the first one**, consistently, at every size — so "the cost of an
index" is not one number and multiplying by index count overstates it.

Two hypotheses fit that and **this run separates neither**, which is worth
saying rather than picking one. Either the cost is *per index and these two
indexes differ* — `w1`'s is on `region` (8 distinct values, 13-byte entries),
`w2`'s second is on `cust_id` (rows/6 distinct values, 17-byte entries), and
both key cardinality and entry width plausibly change the per-append
`memmove` — or there is a **fixed cost of entering the maintenance hook at
all**, paid once by any relation carrying an index, plus a smaller per-index
term. §4.1's non-appending UPDATE leans slightly toward the second: it costs
+1.5 µs with one index and +1.4 with two, flat. Settling it needs two indexes
differing in exactly one variable, which this driver does not build. p0
carries the same deltas with less noise: 60–61 / 64 / 66–67 µs at the three
sizes.

### 4.1 An UPDATE that moves an indexed key, against one that does not

`feat-index.md` §2's second rule is load-bearing rather than tidy: an UPDATE
touching no key or covered column of an index **must not append to it**, or
the entry set grows by one per write forever. `w1` carries one index on
`region`; `w2` carries that one plus a second on `cust_id` that **neither**
update shape touches. Both shapes address their row by pk, so their read half
is one identical clustered descent.

| rows = 10,000, `durability = relaxed` | ops | p0 | p25 | p50 | p95 | p99 | mean |
|---|---:|---:|---:|---:|---:|---:|---:|
| `SET region = ?` (moves a key), 0 indexes | 300 | 72 | 109 | 120 | 137 | 162 | 116 |
| `SET region = ?` (moves a key), 1 index | 300 | 76 | 112 | 126 | 150 | 167 | 125 |
| `SET region = ?` (moves a key), 2 indexes | 300 | 77 | 110 | 126 | 149 | 187 | 128 |
| `SET amount = ?` (moves nothing), 0 indexes | 300 | 72 | 104 | 120 | 141 | 198 | 116 |
| `SET amount = ?` (moves nothing), 1 index | 300 | 72 | 110 | 121 | 143 | 196 | 119 |
| `SET amount = ?` (moves nothing), 2 indexes | 300 | 73 | 103 | 121 | 137 | 172 | 117 |

Read the rows in pairs. On `w0`, where nothing is indexed, the two shapes are
120.2 and 119.7 µs — 0.4%, which is the write path's own floor. The key-moving
UPDATE costs **+5.4 µs on `w1` and +5.8 µs on `w2`, the same number**, because
both append to exactly one index; the second index on `w2` is not touched and
is not paid for. The non-appending UPDATE costs **+1.5 µs and +1.4 µs**, flat
between one index and two — that is the hook encoding each index's key to
discover it need not append (`AppendIndexEntry` builds the key before testing
`touched`), and at 1.2% it is at the edge of what this harness resolves.

The decisive evidence for that rule is not a latency at all. It is an entry
count, read out of `SHOW INDEXES` after the run:

| index | inserts | key-moving UPDATEs | non-key UPDATEs | entries after |
|---|---:|---:|---:|---:|
| `w1 (region)` | 10,000 | 300 | 300 | **10,268** |
| `w2 (region)` | 10,000 | 300 | 300 | **10,268** |
| `w2 (cust_id)` | 10,000 | 300 | 300 | **10,000** |

The `cust_id` index holds exactly one entry per inserted row: 600 UPDATEs
appended nothing to it, because neither touched its key. The `region` indexes
grew by 268 of the 300 key-moving UPDATEs — the missing 32 are the draws that
re-assigned the same region out of eight, which produce a byte-identical entry
that `IndexInsert` reports rather than stores (IX4b). Both halves of §2 are
visible in one number, and neither needed a timer.

That growth is also the standing cost the spec is honest about: nothing
reclaims a superseded entry, so an index over a key-churning column grows
without bound until purge exists.

### 4.2 The backfill

`CREATE INDEX` on a loaded relation, one statement, walking every tuple and
its undo chain:

| rows | ckdbs, per index | PostgreSQL, per index | per row (ckdbs) |
|---:|---:|---:|---:|
| 200 | 209 µs | 597 µs | 1.04 µs |
| 1,000 | 1.13 ms | 807 µs | 1.13 µs |
| 10,000 | 8.68 ms | 4.06 ms | 0.87 µs |

Linear in rows, ~1 µs each, and about **2× slower than PostgreSQL's** at
10,000 rows.

---

## 5. Where the time goes

A latency here is a sum of four terms, and three of them are separable with
the phases this run already carries.

| wait | how it is isolated | INSERT, 10K rows, default `group` | selective `eq`, 10K rows, indexed |
|---|---|---:|---:|
| **client + socket round trip** | the `PING` phase — one send, one receive, a dispatcher that opens no relation | 75 µs (7%) | 75 µs (49%) |
| **durability / commit (fsync)** | `group` p50 minus `relaxed` p50, same phase — the one cross-run subtraction here, because the switch is a startup key | 933 µs (89%) | not applicable — a SELECT logs nothing |
| **write-statement work** — id allocation, encode, page write, WAL append, index maintenance | the remainder: total minus the two above | 37 µs (4%) | — |
| *of which index maintenance* | `w1`/`w2` minus `w0` | 4.5 µs for the first index, 2.5 for the second (12% and 7% of the 37) | — |
| **read work** — descent, entry scan, base resolve, residual | shape p50 minus `PING` p50 | — | 77 µs (51%) |
| **lock / conflict wait** | — | **structurally zero**, see below | **structurally zero** |

Two of these deserve their own sentence.

**The fsync is 89% of an INSERT at the engine's default settings, and it is
why the write cost of an index is a non-issue in practice.** Adding two
indexes to a relation costs 0.9% of a default-durability INSERT. It costs 6.2%
of a `relaxed` one. Both numbers are true and they answer different questions:
the first is what an operator's client sees, the second is what the engine
actually does.

**The client is 49% of an indexed read and 5% of an unindexed one.** These are
Python-client latencies over the newline text protocol and they always were
(`bench/docs/README.md`); the point of measuring `PING` in the same run with
the same client is that the subtraction is then valid *within* a run even
though the client term itself moved between runs (59–94 µs p50 across the
nine ckdbs runs here). Every engine-time figure in this document is an in-run
subtraction.

**Lock and conflict wait is structurally zero and could not have been measured
here.** KDS has no lock manager and no waiting: write conflicts are
first-updater-wins and the loser gets `kTxnConflict` immediately
(`docs/txn.md`). This run is single-connection, so nothing conflicts and
nothing waits. What an index costs under contention — in particular whether
two writers appending to one index leaf conflict more often than they would
without it — is **not measured by this document**, and would need a
multi-process driver in the shape of `scenario0_stockmarket.py`.

---

## 6. Against PostgreSQL

Same six relations, same row generator, same shapes, same seeded arguments;
`INCLUDE (status)` is PostgreSQL's spelling of `COVERING (status)`. `VACUUM
ANALYZE` runs after the load and is not tuning — without statistics the
planner may not choose its index at all, and without the vacuum an index-only
scan is impossible even where it is right, so both would make the baseline a
coin toss. Everything else is at the cluster's defaults.

p50 µs, and the ratio of **engine time** (each side's own `PING` subtracted,
because the two clients are different Python programs over different
protocols):

| shape, rows = 10,000 | ckdbs | PostgreSQL | ckdbs engine | PG engine | PG plan |
|---|---:|---:|---:|---:|---|
| pk lookup | 130 | 195 | 55 | 60 | Index Scan |
| `eq`, no index | 1,472 | 1,122 | 1,397 | 985 | Seq Scan |
| `eq`, indexed | 152 | 240 | 77 | 103 | Bitmap Heap Scan |
| `range`, indexed | 258 | 532 | 183 | 395 | Bitmap Heap Scan |
| `range-covered`, covering | 170 | 326 | 95 | 189 | Bitmap Heap Scan |
| `COUNT(*)` by key, indexed | 146 | 215 | 71 | 78 | **Index Only Scan**, Heap Fetches: 0 |

And the same at the other two sizes, as ratios of engine time (>1 means ckdbs
is faster):

| shape | 200 | 1,000 | 10,000 |
|---|---:|---:|---:|
| pk lookup | 1.84× | 1.64× | 1.09× |
| `eq`, indexed | 2.05× | 1.98× | 1.34× |
| `range`, indexed | 3.14× | 2.80× | 2.16× |
| `range-covered`, covering | 1.51× | 1.43× | 1.33× |
| `COUNT(*)` by key | 1.74× | 1.61× | 1.11× |
| `eq`, **no index** | 1.46× | 0.96× | 0.70× |

The 200-row column carries an asterisk: PostgreSQL declined its own index at
that size and answered every row-returning shape with a `Seq Scan`, so those
ratios compare a ckdbs index probe against a PostgreSQL sequential scan. That
is the honest comparison of what each engine actually did, and point 3 below
is about the difference.

Four things the baseline says.

1. **ckdbs's index probe is faster than PostgreSQL's, and the margin narrows
   as the relation grows** — 2.05× at 200 rows down to 1.34× at 10,000. The
   fixed cost of a statement is where this engine is strong; the per-element
   cost is where PostgreSQL closes.
2. **ckdbs's unindexed scan is 1.4× slower than a `Seq Scan`** at 10,000 rows
   (1,397 µs against 985 for 10,000 rows, i.e. 140 ns per row against 98).
   That gap is what an index is being asked to hide, and it is why the index
   matters more here than the same index matters there.
3. **PostgreSQL declines its own index below ~1,000 rows.** At 200 rows every
   row-returning shape, the pk lookup included, is a `Seq Scan` — only
   `COUNT(*)` gets an `Index Only Scan` — and its indexed and unindexed
   relations land within 3% of each other. At 1,000 rows it switches to
   `Bitmap Heap Scan` and `Index Scan`. It has statistics and a cost
   model, and it uses them to *not* use the index — which is precisely the
   decision `feat-index.md` IX9 removes from KDS by design, and precisely the
   case (§1's `range` at 200 rows) where KDS loses 10% by taking the index
   anyway.
4. **PostgreSQL never chose a plain `Index Scan` for the equalities** — it
   chose `Bitmap Heap Scan`, which sorts matched tuple ids before touching the
   heap. That is the same reordering KDS performs for a different reason
   (IX8a's mandatory pk-order sort, which exists so an index cannot change a
   reply's row order) and arrives at the same benefit: heap access in physical
   order.

On the write side, at default durability on both, p50 µs at 10,000 rows:

| | 0 indexes | 1 index | 2 indexes |
|---|---:|---:|---:|
| ckdbs INSERT | 1,045 | 1,051 | 1,054 |
| PostgreSQL INSERT | 1,133 | 1,139 | 1,144 |
| ckdbs UPDATE (key-moving) | 1,053 | 1,063 | 1,061 |
| PostgreSQL UPDATE (key-moving) | 1,163 | 1,175 | 1,180 |

Both engines are fsync-bound and both charge about +6 µs per index per row.
This is the row of the comparison with the least in it: at one connection with
`synchronous_commit = on`, the device is the answer on both sides.

---

## 7. What this says about the engine

**The index is the first structure in this engine that changes the *shape* of
a non-pk read rather than its constant.** Waystone does something of that
magnitude (22–31× on a heap relation, `bench/results-waystone-v2.md`) but
invariant 9 confines it to lookup-class steps, so a non-pk predicate has never
had anything but a walk. The Cabin measurement
(`bench/results-cabin.md`) reported a 23.9% hit rate and a wash on throughput,
because a Cabin is authoritative only for values a query has already asked
about, and the hit rate is a property of the workload. An index has no hit
rate: it is authoritative for every value, and every one of the 300 probes per
cell here was served. The two structures are correctly described in
`feat-index.md` §1 as the same invariant with the observation precondition
struck out, and this run is what that difference is worth — 9.7× against a
wash, on comparable shapes.

**The measured crossover is between 200 and 1,000 rows, and the compiler
cannot see it.** `f(shape, catalog)` (IX9) means an index is used whenever one
matches, with no cardinality estimate. At 200 rows that is an 11% loss on a
range and a 9.6% win on an equality — small either way, but the sign is set by
the data and the compiler is deciding without it. The design reason is sound
and should not be traded away lightly: a recorded `pattern_id` must not
compile differently as data changes, or it stops naming a plan. But this run
is the first datum for the open question of whether a *statistics-aware*
selection could ever be added without breaking that, and it says the stake is
small at these sizes: the loss is bounded by the walk's cost, which is exactly
what the index step falls through to.

**`COVERING`'s value is a linear function of one counter the engine already
reports.** 0.58–0.76 µs per avoided descent, times `index_filtered`. That is
an unusually actionable finding: an operator can run `ANALYZE` on a candidate
statement *before* declaring the covering index and multiply. It also means
the honest advice is narrow — cover a column only when a residual on it drops
most of the entry set, which was §7's claim and is now a number.

**Two design expectations were checked and both held.** IX8a's pk-order
requirement is verified at 20 argument draws per run (10 in the `relaxed`
ones) × 7 shapes × 2 indexed relations, by comparing the indexed reply against
the unindexed walk's **in order**; no mismatch in any of the twelve runs. And
§2's "an UPDATE that touches no key column must not append" is visible as an
exact entry count — 10,000 entries after 10,000 inserts and 600 updates — not
merely as an unchanged latency. An entry count is the better instrument for
that rule: a violation would cost about 2.5 µs per write, which is inside this
harness's floor, and would only become visible as unbounded growth much
later.

**A note on emphasis rather than a contradiction.** `COVERING` is the most
visible thing in the feature's grammar and the workplan's summary leads with
it ("a `COVERING` clause drops 30 of 40 descents"), which is true and is a
count of descents, not of microseconds. Priced in time, it is worth 21% at
best here and a provable *zero* on any shape whose residual the entry cannot
decide, while the plain `kIndexProbe` underneath it is worth 9.7×. Both
numbers are consistent with `feat-index.md` §7, which claims exactly the
avoided descents and nothing more. Worth writing down only so a reader sizing
the feature reaches for the index first and the clause second.

**And one gap this document cannot close.** Every number here is single
connection. The index's write cost is 0.9% of a default-durability INSERT
*because a single connection makes `group` durability behave as `strict`* — a
batch of one is a batch. Under concurrency the fsync amortizes across
committers and the 933 µs denominator shrinks, at which point 4.5 µs per index
becomes a visible fraction rather than a rounding error. Sizing an index's
write cost for a real workload wants the multi-process treatment
`scenario0_stockmarket.py` gives writes, and that measurement does not exist
yet.
