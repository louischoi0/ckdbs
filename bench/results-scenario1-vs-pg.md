# The backtest workload across three row-set sizes — ckdbs against PostgreSQL 17.10

Every read shape, every sweep and every matrix in `tools/scenario1_backtest.py`,
measured at **252, 1,008 and 10,080 bar rows**, against the PostgreSQL twin at
the same three sizes. How to run either driver is in
[`bench/docs/README.md`](docs/README.md); this file states what the run found.

**Thesis: at this scale ckdbs's per-statement cost is a small fixed number and
PostgreSQL's is a larger one, while ckdbs's per-row cost is slightly the
higher of the two — so which engine wins is decided by the row count, and the
crossover sits inside the range this run covers.** One measurement at one
cardinality would have reported either "ckdbs is 1.43× faster on a FilterScan"
(true at 252 rows) or "PostgreSQL is 1.16× faster on a FilterScan" (true at
10,080), both measured here, neither useful on its own. The
three sizes turn those two claims into two numbers per shape — a fixed cost and
a per-row cost — and almost every finding below is one of them.

---

## The run

| | |
|---|---|
| executed | 2026-08-07, ckdbs 06:10:26–06:11:37 UTC, PostgreSQL 06:12:14–06:13:45 UTC (sequential, never concurrent) |
| branch | `feat-additional-types` |
| commit | `55f7b30` (`types: the bare numeric literal (TY10, TY3 phase 2)`, committed 05:49:40 UTC) |
| tree | **dirty** — 5 modified files: `include/kds/wire/row_codec.hpp`, `src/wire/row_codec.cpp`, `tests/wire_row_codec_test.cpp` (a DECIMAL wire encoding), `include/kds/catalog/well_known.hpp` (comment only), `.claude/agents/ck-tester.md`. None is on scenario1's path: KWP/1 has no caller and the server speaks the newline text protocol |
| binary | `build-release/kds_server`, rebuilt 2026-08-07 05:58:55 UTC — **newer than HEAD**, and it contains the uncommitted diff above. It is not a binary older than HEAD; it is HEAD plus that diff |
| build type | Release (`CMAKE_BUILD_TYPE:STRING=Release` in `build-release/CMakeCache.txt`) |
| test suite | 1,555 tests, 149 suites, **all pass** on this tree and this build |
| device | `/dev/nvme0n1p1`, xfs, non-rotational, 256 GB, 247 GB free. Data files `/home/ec2-user/bench-s1/*.db`, WAL `*.db.wal` alongside. PostgreSQL's data directory `/home/ec2-user/pg-bench/data` is on the **same volume**. Nothing was placed on `/tmp`, which is tmpfs here |
| machine | 2 vCPU, 7 GiB RAM. 1-minute load average 0.36 / 0.41 / 0.46 at the start of the three ckdbs runs and 0.03 after the last; `pgrep cc1plus` empty before and after |
| ckdbs config | every key at its default: `cores = 1`, `durability = group`, `isolation = read committed`, `inline_cell_width = 64`, `checkpoint_interval_ms = 5000`, `wal_drain_interval_us = 1000`. Superblock format version 11. No `--config` was passed; log level `info` |
| PostgreSQL | 17.10, `tools/pg_setup.sh` cluster on port 15433, **untouched defaults** — `synchronous_commit = on`, `shared_buffers = 128MB`. Nothing was tuned |
| data files | fresh file and fresh server per configuration; 3.5 / 3.0 / 8.0 MiB after the run (allocation is by 64-page extents, so file size is granular rather than proportional) |
| verification | `--verify` passed on both engines at all three sizes, and `tools/compare_scenario1.py` reports `identity: OK` for all three pairs — **all eight models scored identically to the basis point on both engines** |

## The size ladder, and where the numbers come from

`daily_bars` is the bulk relation. Its row count is `years × 252 × symbols` —
`SESSIONS_PER_YEAR = 252` at `tools/scenario1_backtest.py:349`,
`session_count = args.years * SESSIONS_PER_YEAR` and
`bar_total = session_count * args.symbols` in `main()`. Verified against the
driver's own echoed line rather than the arithmetic alone.

**Only `--years` moves.** Holding `--symbols 1` keeps every result set the same
size at every tier, so a shape whose latency grows is growing on the *relation*
and not on what it returns — which is precisely the fixed-versus-per-row
separation this ladder exists to make.

| tier | flags | sessions | `daily_bars` | `daily_stats` | `model_results` | rows resident |
|---|---|---:|---:|---:|---:|---:|
| 200 | `--symbols 1 --years 1` | 252 | **252** | 252 | 88 | 765 |
| 1K | `--symbols 1 --years 4` | 1,008 | **1,008** | 1,008 | 376 | 3,033 |
| 10K | `--symbols 1 --years 40` | 10,080 | **10,080** | 10,080 | 3,832 | 30,249 |

Everything else is the driver's default: `--rebalance 21`, `--top-k 3`,
`--bars-clustered btree`, `--batch 200`, `--ops 200`, `--qps-ops 100`,
`--warm-keys 8`, `--write-ops 2000`, `--write-batches 1,10,100,1000`,
`--connections 1,2,4,8`, `--conn-ops 200`, `--seed 1`. **Equal work at every
tier**: the op counts are fixed by those flags and do not shrink when a size
gets slower, so the 10,080-row run is the same 200 lookups and the same 100
statements per matrix cell as the 252-row run.

Three consequences of `--symbols 1` are stated here rather than discovered in a
table. The sweep's `symbol-history` shape has exactly **one** distinct argument
at every tier, so its `cold` cell is a single statement and its cold/warm gap
is meaningless — its `warm` cell (100 statements) is the one to read.
`model-join` has eight distinct arguments at every tier for the same structural
reason (there are eight models), which is a property of the schema and not of
the ladder. And `agg-by-symbol` folds into **one** group, which makes it a
control against `agg-global` rather than a low-cardinality grouping case.

## The noise floor, established from inside the run

The 10,080 configuration was run **twice**, each on a fresh server and a fresh
data file, and the second run is the control. Every delta below is measured
against these:

| measurement class | repeat spread | notes |
|---|---:|---|
| p50 of a 200-op phase | ≤ **3.9 %** | four of five within 1.1 %; `agg-day-slice` was the outlier at 3.9 % |
| p50 of a phase with ≤ 10 ops | ≤ **6.8 %** | ten samples is ten samples; the worst was `load-exchanges`, which has two |
| a QPS-matrix cell (100 statements) | ≤ **11.1 %** | median 2 %; the worst was `model-join` warm |
| INSERT sweep, any batch size | ≤ **1.4 %** | the tightest table in the run |
| concurrency sweep, any count | ≤ **5.1 %** | |
| PING floor across all four servers | ≤ **1.1 %** | p50 90.8 / 91.2 / 91.8 / 91.8 µs |

**Nothing under 5 % on a matrix cell, 4 % on a 200-op p50 or 12 % on the
shortest cells is reported below as a finding.** Two things are called out
because they sit at that boundary and would otherwise be read as results:

- The `bar-lookup` **cold** cell at the 1K tier reads 9,446 QPS against 7,590
  and 7,822 at the other two tiers, with a p50 of 95.5 µs — barely above the
  bare PING floor. The `read-bar-lookup` phase in the same run reads a normal
  126.8 µs p50, and the 10K repeat reproduces to 1.1 %. It is a transient in
  one cell, not a cardinality effect, and it is excluded.
- The **autocommit** row of the INSERT sweep read 798 / 1,152 / 798 rows·s⁻¹ at
  the three tiers, on a `write_probe` relation that is created fresh and is
  identical at all three. The within-configuration repeat put that row at
  0.1 %, so the 1K value is *between-run* variance the control did not capture.
  The device's own fsync latency is not the cause: three separate 300-sample
  probes of an 8 KiB write + fsync on this volume returned p50 926.8, 928.1 and
  929.8 µs. The 1K autocommit figure is reported and not explained.

---

## A primary-key lookup does not scale with the row count, and here is the evidence

The claim is easy to assert and cheap to check, so it is checked. Four shapes
touch a fixed amount of data whatever the relation holds — a pk equality, a
three-relation join anchored on a pk, a semi-join that stops at the first
qualifying row, and a pk `BETWEEN` 200 rows wide. All four are flat across a
40× change in relation size, on both engines.

| shape | rows | engine | ops | mean µs | p0 | p25 | p50 | p95 | p99 |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|
| read-bar-lookup | 252 | ckdbs | 200 | 126.6 | 77.7 | 124.0 | 126.7 | 144.7 | 161.0 |
| read-bar-lookup | 1,008 | ckdbs | 200 | 127.5 | 87.7 | 125.1 | 126.8 | 145.8 | 166.6 |
| read-bar-lookup | 10,080 | ckdbs | 200 | 129.8 | 120.1 | 126.3 | 127.4 | 142.0 | 157.8 |
| read-bar-lookup | 252 | pg | 200 | 227.1 | 184.7 | 224.1 | 225.0 | 243.1 | 256.9 |
| read-bar-lookup | 1,008 | pg | 200 | 207.9 | 202.8 | 203.9 | 204.7 | 219.9 | 242.7 |
| read-bar-lookup | 10,080 | pg | 200 | 211.2 | 169.6 | 205.2 | 206.8 | 240.0 | 279.0 |
| read-join-point | 252 | ckdbs | 200 | 146.4 | 91.6 | 141.5 | 143.6 | 160.5 | 180.9 |
| read-join-point | 1,008 | ckdbs | 200 | 146.5 | 127.2 | 142.5 | 143.7 | 162.6 | 178.2 |
| read-join-point | 10,080 | ckdbs | 200 | 144.8 | 119.6 | 141.5 | 142.6 | 156.4 | 178.0 |
| read-join-point | 252 | pg | 200 | 318.8 | 298.1 | 312.0 | 312.8 | 342.5 | 354.9 |
| read-join-point | 1,008 | pg | 200 | 303.9 | 292.7 | 295.1 | 297.1 | 335.4 | 381.6 |
| read-join-point | 10,080 | pg | 200 | 304.8 | 287.3 | 294.8 | 296.2 | 336.6 | 395.6 |
| read-join-exists | 252 | ckdbs | 10 | 135.6 | 131.4 | 131.7 | 132.4 | 143.6 | 143.6 |
| read-join-exists | 1,008 | ckdbs | 10 | 135.1 | 132.3 | 132.5 | 133.3 | 142.2 | 142.2 |
| read-join-exists | 10,080 | ckdbs | 10 | 136.3 | 125.5 | 127.2 | 129.2 | 167.3 | 167.3 |
| read-join-exists | 252 | pg | 10 | 242.1 | 237.5 | 239.0 | 239.2 | 252.4 | 252.4 |
| read-join-exists | 1,008 | pg | 10 | 250.4 | 238.1 | 239.1 | 242.1 | 303.6 | 303.6 |
| read-join-exists | 10,080 | pg | 10 | 247.2 | 239.4 | 239.9 | 241.1 | 276.6 | 276.6 |
| read-bar-range | 252 | ckdbs | 200 | 394.8 | 368.5 | 382.1 | 383.9 | 440.9 | 542.5 |
| read-bar-range | 1,008 | ckdbs | 200 | 419.7 | 375.6 | 388.8 | 399.7 | 510.9 | 624.9 |
| read-bar-range | 10,080 | ckdbs | 200 | 406.0 | 375.3 | 387.5 | 397.6 | 459.9 | 498.6 |
| read-bar-range | 252 | pg | 200 | 2,272.7 | 2,225.0 | 2,253.8 | 2,267.0 | 2,316.6 | 2,409.3 |
| read-bar-range | 1,008 | pg | 200 | 2,336.1 | 2,233.0 | 2,253.4 | 2,268.8 | 2,630.5 | 3,532.2 |
| read-bar-range | 10,080 | pg | 200 | 2,353.3 | 2,259.3 | 2,285.5 | 2,299.0 | 2,528.5 | 3,358.2 |

ckdbs's pk lookup moves by **0.6 %** across a 40× relation, its point join by
0.7 %, its semi-join by 3.2 % end to end (on ten samples, inside the floor for
that class) and its 200-wide pk range by 4.1 %. That is the clustered B+ tree
behaving
exactly as `docs/heap-and-tuple.md` says it should: the descent's cost is the
tree's depth, which does not move over three orders of magnitude of rows this
small. The one shape whose flatness is *not* structural is `bar-range` — a pk
`BETWEEN` descends to its low bound and walks siblings, so it is flat because
the **window** is fixed at 200 rows, not because the relation is small.

The ratios are stable with it: ckdbs is 1.6–1.8× faster than PostgreSQL on the
pk lookup, 2.1–2.2× on the point join, 1.8–1.9× on the semi-join and 5.5–5.9×
on the pk range, at every size. **This is the part of the comparison the row
count does not touch, and it is worth separating from everything that follows.**

## The FilterScan is where cardinality lives — and the two engines cross over inside this range

`read-day-slice` is the cleanest instrument in the run: one equality on a
non-pk, unindexed column of `daily_stats`, matching exactly one row at every
tier while the relation grows 40×. Its latency is therefore a straight line in
relation size, and the line has two coefficients worth naming separately.

| shape | rows | engine | ops | mean µs | p0 | p25 | p50 | p95 | p99 |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|
| read-day-slice | 252 | ckdbs | 200 | 159.8 | 143.3 | 156.9 | 158.1 | 171.4 | 189.0 |
| read-day-slice | 1,008 | ckdbs | 200 | 263.0 | 246.5 | 254.3 | 256.1 | 287.4 | 327.4 |
| read-day-slice | 10,080 | ckdbs | 200 | 1,455.8 | 1,385.6 | 1,404.5 | 1,418.0 | 1,695.4 | 2,135.9 |
| read-day-slice | 252 | pg | 200 | 231.0 | 212.2 | 221.9 | 223.7 | 269.5 | 317.4 |
| read-day-slice | 1,008 | pg | 200 | 317.9 | 302.3 | 304.3 | 307.0 | 379.8 | 454.5 |
| read-day-slice | 10,080 | pg | 200 | 1,347.9 | 1,255.2 | 1,282.4 | 1,293.4 | 1,781.7 | 1,944.3 |

Subtracting each client's own round-trip floor (measured below: 91.8 µs for
ckdbs, 135.5 µs for PostgreSQL) and fitting the two larger sizes gives a fixed
cost and a per-row cost. The 252-row point is then *predicted* rather than
fitted, which is the only validation two coefficients from three points allow:

| shape | engine | 252 | 1,008 | 10,080 | **ns per row** | **fixed µs** | predicted @252 | fit error |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| read-day-slice | ckdbs | 66.3 | 164.3 | 1,326.2 | **128.1** | **35.2** | 67.5 | +1.8 % |
| read-day-slice | pg | 88.2 | 171.5 | 1,157.9 | **108.7** | **61.9** | 89.3 | +1.2 % |
| agg-day-slice | ckdbs | 73.2 | 173.1 | 1,342.4 | 128.9 | 43.2 | 75.7 | +3.4 % |
| agg-day-slice | pg | 109.7 | 194.7 | 1,190.8 | 109.8 | 84.0 | 111.7 | +1.8 % |
| backtest-read (3-rel join) | ckdbs | 101.9 | 201.5 | 1,418.4 | 134.1 | 66.3 | 100.1 | −1.8 % |
| backtest-read (3-rel join) | pg | 312.7 | 359.1 | 1,395.5 | 114.2 | 243.9 | 272.7 | −12.8 % |
| agg-global | ckdbs | 93.2 | 273.9 | 2,459.0 | 240.9 | 31.1 | 91.8 | −1.5 % |
| agg-global | pg | 127.7 | 261.4 | 1,937.4 | 184.7 | 75.2 | 121.7 | −4.7 % |
| read-bar-lookup | ckdbs | 34.9 | 35.0 | 35.6 | **0.1** | 34.9 | — | — |
| read-join-point | ckdbs | 51.8 | 51.9 | 50.8 | **−0.1** | 52.0 | — | — |
| read-join-exists | ckdbs | 40.6 | 41.5 | 37.4 | **−0.5** | 42.0 | — | — |

*(µs figures are p50 with the client floor removed; the pk rows are in the same
table so the zero slope can be read against a non-zero one.)*

Three readings, in order of how much they license.

**The model holds.** A two-coefficient fit from the 1K and 10K points predicts
the 252-row point to within 1.2–3.4 % on every well-behaved shape — inside the
noise floor. A FilterScan in this engine really is a fixed cost plus a constant
per row, with no visible non-linearity across 40× and no page-cache cliff (the
whole data file is 8 MiB against 7 GiB of RAM, so nothing here touches the
device on the read path — see the wait section).

**ckdbs's fixed cost is 35 µs and PostgreSQL's is 62 µs; ckdbs's per-row cost
is 128 ns and PostgreSQL's is 109 ns.** ckdbs starts a statement for 27 µs less
and then pays 19 ns more for each row it walks. The crossover implied by those
four numbers is near **1,400 rows**, and it is not an extrapolation: ckdbs is
ahead at 1,008 rows (1.28× on warm QPS) and behind at 10,080 (0.86×), so the
sign change is bracketed by measurement and the fit only says where inside the
bracket it falls.

**The three-relation join adds one fixed cost, not a per-row one.**
`backtest-read` is `read-day-slice` plus two pk probes, and its slope is
134 ns/row against the FilterScan's 128 — a difference at the edge of the
floor — while its fixed cost rises 35 → 66 µs. That is the compiled chain doing
what `docs/parser-v2.md` says it does: the two probes fire once per *surviving*
row, and exactly one row survives here, so they cost a constant. It also means
the whole cost of the backtest's headline read at 10,080 rows is the walk of
the relation it filters, not the joining.

For the record, the warm QPS ratios that all of this produces, per size:

| shape | 252 | 1,008 | 10,080 |
|---|---:|---:|---:|
| bar-lookup | 1.79× | 1.62× | 1.63× |
| bar-range | 5.91× | 5.52× | 5.64× |
| point-join | 2.24× | 2.09× | 2.09× |
| model-join | 2.05× | 2.51× | 3.57× |
| symbol-history | 6.72× | 7.82× | 6.31× |
| **day-slice** | **1.43×** | **1.28×** | **0.86×** |
| **cross-join** | **2.46×** | **1.70×** | **0.90×** |

*(ckdbs warm QPS ÷ PostgreSQL warm QPS; above 1.00× is ckdbs ahead. Rates, not
latency distributions, so no percentiles — those are in the tables above.)*

The two bolded rows are the two shapes that cross over, and they are the two
whose cost is dominated by walking a relation that grows. Every other row is
flat in size because its shape is.

## The Cabin converts a per-row cost into a per-matching-row cost

This is the largest effect in the run, and its shape is only visible across
sizes. The sweep declares a Cabin on the filter column, warms it so its values
are observed, measures, drops it and measures again — one DDL statement between
two measurements over identical bytes. PostgreSQL's twin does the same thing
with a btree index, which is the honest counterpart: both are structures the
operator adds for a non-pk equality.

| rows | shape | ckdbs warm | ckdbs **cabin** | gain | pg warm | pg **index** | gain | cabin ÷ index |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 252 | day-slice | 6,249 | 7,865 | **1.26×** | 4,383 | 4,186 | 0.95× | 1.88× |
| 1,008 | day-slice | 3,762 | 7,732 | **2.06×** | 2,928 | 5,553 | 1.90× | 1.39× |
| 10,080 | day-slice | 663 | 8,695 | **13.11×** | 772 | 4,442 | 5.75× | 1.96× |
| 252 | cross-join | 5,374 | 6,499 | 1.21× | 2,183 | 2,221 | 1.02× | 2.93× |
| 1,008 | cross-join | 3,479 | 6,349 | 1.82× | 2,043 | 2,702 | 1.32× | 2.35× |
| 10,080 | cross-join | 641 | 6,464 | **10.08×** | 716 | 2,668 | 3.72× | 2.42× |
| 252 | model-join | 5,938 | 6,329 | 1.07× | 2,899 | 3,062 | 1.06× | 2.07× |
| 1,008 | model-join | 4,164 | 4,797 | 1.15× | 1,659 | 1,643 | 0.99× | 2.92× |
| 10,080 | model-join | 909 | 1,349 | **1.48×** | 255 | 271 | 1.06× | 4.98× |
| 252 | symbol-history | 1,970 | 1,821 | 0.92× | 293 | 294 | 1.00× | 6.20× |
| 1,008 | symbol-history | 602 | 529 | 0.88× | 77 | 78 | 1.01× | 6.81× |
| 10,080 | symbol-history | 47 | 45 | 0.96× | 7 | 7 | 0.99× | 6.11× |

*(QPS, 100 statements per cell. Rates, so no percentiles.)*

**The number that matters is not the gain, it is the Cabin-served column.**
`day-slice` served from a Cabin reads 7,865 / 7,732 / 8,695 QPS across a 40×
relation — a 12 % spread, above the floor but with no trend in the direction the
relation grew — while the same statement without one falls 6,249 → 663, a factor
of 9.4. `cross-join` is cleaner still: 6,499 / 6,349 / 6,464 served, a 2.4 %
spread and inside the floor, against 5,374 → 641 unserved. The Cabin does not
make the walk faster; it
**removes the walk**, and what is left has no dependence on relation size at
all. At 10,080 rows a Cabin-served `day-slice` (8,695 QPS, 113.9 µs mean) is
*faster than the pk lookup on the same data set* (7,826 QPS, 126.7 µs): reaching
an observed value's entry set costs less than descending the clustered tree. That
is what `docs/feat-cabin.md` §1's "observed ⇒ complete" is worth in practice —
the relation is not opened at all — measured rather than asserted.

**`model-join` is the exception, and it is the exception that states the rule.**
Its Cabin-served number is *not* flat — 6,329 / 4,797 / 1,349 — because
`model_results` has eight distinct `model_id` values and 3,832 rows at the 10K
tier, so a hit resolves ~479 entries and emits ~479 rows. The Cabin removed the
scan and left the resolve: 1,098.7 µs warm → 740.4 µs served, i.e. it took away
358 µs of walking and the remaining 740 µs is per-matching-row work. Read the
two together and the correct statement is that **a Cabin converts a cost
proportional to the relation into a cost proportional to the match set** — which
is exactly the two-phase "resolve every entry, then emit" path
`docs/feat-cabin.md` specifies, showing up as a slope instead of as prose.

**`symbol-history` is the control, and it behaves.** Its filter matches every
row in the relation, so there is nothing for an authoritative value set to
remove; ckdbs measures 0.92 / 0.88 / 0.96× and PostgreSQL's index measures
1.00 / 1.01 / 0.99×. PostgreSQL's three are inside the floor outright. ckdbs's
are a consistent small loss, of which only the 1,008 figure (−12 %) is outside
the worst-cell floor of 11.1 % — so the honest statement is that declaring a
structure for a predicate that selects everything buys nothing on either engine,
and that whether it costs ckdbs a few per cent is not resolvable at 100
statements per cell.

**The `dropped` column returns.** Across all twelve Cabin cells the post-`DROP`
measurement lands at 0.92–1.02× of the original warm figure, inside the floor
for that class. Un-observing is a performance event and never a correctness
one, and the run has no counterexample: `--verify` passed and the identity check
passed at every size with the Cabin created and dropped mid-run.

## Aggregation: cost tracks group count, and ckdbs's per-group cost is ~6× lower

`docs/feat-aggregate.md` states that the fold's cost tracks *group count*, not
row count. This run is a second, independent test of that on a different
workload, and it holds — with a number attached on both sides.

| shape | rows | engine | ops | mean µs | p0 | p25 | p50 | p95 | p99 |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|
| agg-global (1 group) | 252 | ckdbs | 10 | 187.1 | 184.2 | 184.8 | 185.0 | 195.2 | 195.2 |
| agg-global | 1,008 | ckdbs | 10 | 372.3 | 345.8 | 359.2 | 365.7 | 437.0 | 437.0 |
| agg-global | 10,080 | ckdbs | 10 | 2,727.8 | 2,526.9 | 2,536.1 | 2,550.8 | 3,445.1 | 3,445.1 |
| agg-global | 252 | pg | 10 | 1,087.6 | 254.7 | 256.9 | 263.2 | 8,462.8 | 8,462.8 |
| agg-global | 1,008 | pg | 10 | 425.1 | 394.1 | 394.6 | 396.9 | 624.7 | 624.7 |
| agg-global | 10,080 | pg | 10 | 2,100.3 | 2,049.5 | 2,057.2 | 2,072.9 | 2,339.4 | 2,339.4 |
| agg-by-symbol (1 group) | 252 | ckdbs | 10 | 200.8 | 197.2 | 197.7 | 200.5 | 210.1 | 210.1 |
| agg-by-symbol | 1,008 | ckdbs | 10 | 413.3 | 410.2 | 411.2 | 411.5 | 425.6 | 425.6 |
| agg-by-symbol | 10,080 | ckdbs | 10 | 3,221.6 | 2,934.8 | 2,945.9 | 2,952.7 | 4,216.1 | 4,216.1 |
| agg-by-symbol | 252 | pg | 10 | 299.3 | 292.4 | 294.1 | 297.4 | 321.7 | 321.7 |
| agg-by-symbol | 1,008 | pg | 10 | 523.9 | 511.9 | 512.4 | 514.2 | 588.6 | 588.6 |
| agg-by-symbol | 10,080 | pg | 10 | 3,175.1 | 3,112.3 | 3,133.3 | 3,150.5 | 3,367.9 | 3,367.9 |
| **agg-by-session** (N groups) | 252 | ckdbs | 10 | 391.3 | 364.4 | 367.6 | 370.2 | 500.8 | 500.8 |
| agg-by-session | 1,008 | ckdbs | 10 | 1,162.4 | 1,097.4 | 1,102.6 | 1,108.2 | 1,604.1 | 1,604.1 |
| agg-by-session | 10,080 | ckdbs | 10 | 12,347.8 | 11,356.4 | 11,624.8 | 11,811.0 | 15,614.9 | 15,614.9 |
| agg-by-session | 252 | pg | 10 | 1,716.0 | 1,681.8 | 1,688.4 | 1,696.5 | 1,834.1 | 1,834.1 |
| agg-by-session | 1,008 | pg | 10 | 6,123.3 | 5,948.3 | 5,993.0 | 6,070.1 | 6,471.9 | 6,471.9 |
| agg-by-session | 10,080 | pg | 10 | 59,322.6 | 58,610.2 | 58,810.9 | 59,014.0 | 61,610.5 | 61,610.5 |
| agg-distinct | 252 | ckdbs | 10 | 177.8 | 168.6 | 169.9 | 177.6 | 191.7 | 191.7 |
| agg-distinct | 1,008 | ckdbs | 10 | 341.1 | 326.8 | 330.4 | 333.9 | 371.7 | 371.7 |
| agg-distinct | 10,080 | ckdbs | 10 | 2,383.2 | 2,218.6 | 2,228.1 | 2,239.4 | 2,982.3 | 2,982.3 |
| agg-distinct | 252 | pg | 10 | 253.2 | 242.3 | 243.3 | 244.5 | 320.1 | 320.1 |
| agg-distinct | 1,008 | pg | 10 | 396.4 | 378.2 | 380.5 | 383.1 | 488.6 | 488.6 |
| agg-distinct | 10,080 | pg | 10 | 2,104.9 | 2,043.5 | 2,053.4 | 2,099.8 | 2,230.1 | 2,230.1 |

At `--symbols 1`, `agg-by-symbol` produces **one** group, so the first two
blocks are a controlled pair: same relation, same walk, one grouping key and
one fewer aggregate. The difference is +39 ns/row on ckdbs and +106 ns/row on
PostgreSQL. It bounds rather than isolates the grouping cost — the select lists
differ by one aggregate as well as by the `GROUP BY` — but the direction and the
magnitude are clear, and 39 ns/row for key-encode plus map-probe is consistent
with `docs/feat-aggregate.md`'s "zero allocations per row into an existing
group".

`agg-by-session` is the high-cardinality end, and here every row founds its own
group (one bar per session at `--symbols 1`), so group count *equals* row count:

| segment | ckdbs, ns per group | PostgreSQL, ns per group |
|---|---:|---:|
| 252 → 1,008 groups | **976** | 5,785 |
| 1,008 → 10,080 groups | **1,180** | 5,836 |

**PostgreSQL's cost per group is 4.9–5.9× ckdbs's, and flat to 0.9 % between the
two segments; ckdbs's rises 21 %.** That rise is not established: each slope is
a difference of two ten-sample p50s, which propagates to roughly ±10 % per
slope, so 976 against 1,180 is suggestive and no more — a map that outgrows
cache is the obvious candidate and this run does not test it. What is not
marginal is the level: at 10,080 groups ckdbs finishes the fold in 11.8 ms
against PostgreSQL's 59.0 ms, a 5.0× win on the shape where a planned
HashAggregate should be at its best. It is worth being precise about what that
does and does not say: ckdbs *loses* the low-cardinality folds slightly (`agg-global`
2,551 vs 2,073 µs at 10K, `agg-distinct` 2,239 vs 2,100) because those are the
scan, where PostgreSQL's 109 ns/row beats its 128 — and wins the
high-cardinality one by 5× because the fold, not the scan, is what dominates
there. **Both facts are the same two coefficients from the FilterScan section
reappearing with a third term added for groups.**

`aggregate_max_groups` is ratified at 65,536 in `docs/feat-aggregate.md`. At
1.18 µs per group, a statement at that ceiling would spend ~77 ms in the fold
alone — visibly slow before it is refused, which is the argument the ratification
rested on, now measured on a second workload.

## The write side, and where an INSERT's time actually goes

The INSERT sweep is the one table that does not move with the read-set size —
it writes 2,000 rows into a `write_probe` relation of its own at each of four
transaction batch sizes — and that is exactly why it is the right place to
decompose a write.

| rows in the file | rows/txn | ckdbs rows·s⁻¹ | pg rows·s⁻¹ | ckdbs vs its own autocommit | pg vs its own |
|---:|---:|---:|---:|---:|---:|
| 252 | 1 (autocommit) | 800 | 739 | 1.00× | 1.00× |
| 252 | 10 | 4,275 | 3,426 | 5.34× | 4.64× |
| 252 | 100 | 8,184 | 5,379 | 10.23× | 7.28× |
| 252 | 1,000 | 8,827 | 5,676 | 11.03× | 7.68× |
| 1,008 | 1 | 1,152 | 740 | 1.00× | 1.00× |
| 1,008 | 10 | 4,386 | 3,378 | 3.81× | 4.57× |
| 1,008 | 100 | 7,478 | 5,504 | 6.49× | 7.44× |
| 1,008 | 1,000 | 9,078 | 5,770 | 7.88× | 7.80× |
| 10,080 | 1 | 798 | 738 | 1.00× | 1.00× |
| 10,080 | 10 | 4,375 | 3,368 | 5.48× | 4.56× |
| 10,080 | 100 | 8,065 | 5,402 | 10.10× | 7.32× |
| 10,080 | 1,000 | 8,845 | 5,702 | 11.08× | 7.73× |

*(Throughput, not a latency distribution — no percentiles. The 1K autocommit
row is the between-run outlier flagged in the noise-floor section; its three
derived ratios inherit the anomaly and should not be read.)*

Both engines are dominated by one durability point per transaction, and batching
buys ckdbs 11× against PostgreSQL's 7.7× — the comparable half, since the
absolute numbers carry two different default durability settings (`group`
against `synchronous_commit = on`) and the ratios do not.

## Wait accounting

A latency here is a sum of five things. Four are measured in this session; the
fifth is named and is zero by construction. The decomposition is built from
three independent measurements: the PING round trip (client + socket, touching
no relation), the INSERT sweep's batch-1000 row (a write that pays essentially
no durability wait), and its batch-1 row (the same write paying one).

| wait type | how it was measured | ckdbs | PostgreSQL |
|---|---|---:|---:|
| **client + socket round trip** | 500 × `PING` (ckdbs) / `SELECT 1` (pg), p50 | **91.8 µs** | **135.5 µs** |
| **write statement** (parse, compile, insert, WAL append) | batch-1000 µs/row minus the floor | **21.3 µs** | 39.9 µs |
| **durability / commit fsync** | batch-1 µs/row minus batch-1000 µs/row | **1,139.3 µs** | 1,179.8 µs |
| **read** (walk, decode, filter, fold, emit) | per-shape p50 minus the floor | 35 µs + 128 ns/row | 62 µs + 109 ns/row |
| **lock / conflict wait** | error counts, single connection | **0** | **0** |

The PostgreSQL floor is an upper bound rather than a true floor: `SELECT 1` is
the closest available twin to `PING` through the same driver, but PostgreSQL
still parses and plans it where ckdbs's `PING` does not reach the parser.

For an autocommitted INSERT at the 10,080-row tier, the shares are:

| component | µs per row | share |
|---|---:|---:|
| durability / commit fsync | 1,139.3 | **91.0 %** |
| client + socket round trip | 91.8 | 7.3 % |
| write statement | 21.3 | 1.7 % |
| **total (measured, batch 1)** | **1,252.3** | 100 % |

**The decomposition is self-checking and it checks out.** If the durability term
is one fsync amortized over the batch, then batch 10 should cost
113.1 + 1,139/10 = 227 µs per row and batch 100 should cost 124.5 — measured
228.6 and 124.0 — two predicted points, two measured, both within 1 %. The same
113 µs per row shows up in a completely different phase against a different
relation: `load-bars` runs at 200 rows per transaction and reports a p50 of
115.1 / 114.9 / 114.1 µs at the three sizes.

Two things this pins down. **The durability wait is the device, not the
engine** — 1,139 µs against a raw 8 KiB write+fsync of 926.8 µs p50 (mean 1,041,
p95 1,983) measured three times on this volume. And **the engine's own write
path is 1.7 % of an autocommitted insert**, which means no amount of work on
`InsertInner` moves an unbatched INSERT and every proportional gain available is
in batching or in the durability class.

One observation the instrumentation cannot resolve: `result-insert`, an
autocommitted INSERT inside the backtest, has a **p0 of 379.7 µs** at the 10K
tier — below the device's own fsync p0 of 833 µs. So not every commit pays a
full device fsync, which is what `durability = group` is for; but with one
connection there is no second committer to batch with, and nothing in today's
engine reports how many fsyncs a statement waited on. It is recorded as
unexplained rather than attributed.

**Wait types this engine cannot measure today**, and why:

- **Read I/O against read CPU.** The whole data file is 8 MiB against 7 GiB of
  RAM and is written by the same process that reads it, so the read path never
  touches the device here — the "read" row above is CPU and page-cache hits
  only. Nothing in the engine accounts per-statement I/O, so on a data set that
  did not fit this row could not be split.
- **WAL append against WAL fsync.** The write-statement row above contains the
  log append; only the *wait for durability* is separable, by the batching
  differential. There is no per-record instrumentation to split them.
- **Server-side queueing.** A statement's time between arrival and dispatch is
  not recorded, which is exactly the term the concurrency sweep below would
  need to interpret cleanly.
- **Lock and conflict wait is structurally zero**, not merely small: there is no
  lock manager, write conflicts are first-updater-wins with no waiting, every
  measurement here ran on one connection except the concurrency sweep, and both
  engines reported **zero error replies** across all six runs.

## Concurrency: ckdbs's aggregate gain is round-trip overlap, and it disappears as statements get longer

| rows | connections | ckdbs q/s | pg q/s | ckdbs vs its own 1-conn | pg vs its own |
|---:|---:|---:|---:|---:|---:|
| 252 | 1 | 5,269 | 2,359 | 1.00× | 1.00× |
| 252 | 2 | 7,854 | 3,736 | 1.49× | 1.58× |
| 252 | 4 | 8,036 | 3,902 | **1.53×** | 1.65× |
| 252 | 8 | 7,575 | 3,501 | 1.44× | 1.48× |
| 1,008 | 1 | 3,389 | 2,079 | 1.00× | 1.00× |
| 1,008 | 2 | 4,072 | 3,135 | 1.20× | 1.51× |
| 1,008 | 4 | 4,240 | 3,344 | **1.25×** | 1.61× |
| 1,008 | 8 | 4,130 | 3,242 | 1.22× | 1.56× |
| 10,080 | 1 | 675 | 698 | 1.00× | 1.00× |
| 10,080 | 2 | 655 | 1,145 | 0.97× | 1.64× |
| 10,080 | 4 | 677 | 1,112 | **1.00×** | 1.59× |
| 10,080 | 8 | 691 | 1,108 | 1.02× | 1.59× |

*(Aggregate throughput of the 3-relation join, 200 statements spread across the
connections. Rates, so no percentiles.)*

**PostgreSQL's scaling is flat at ~1.6× across all three sizes; ckdbs's decays
from 1.53× to 1.00× as the statement gets longer.** That is `cores = 1` being
reported correctly, and the arithmetic behind it is simple enough to check: with
one dispatch thread, the only concurrency available is overlapping one client's
think time with another's server time, so the ceiling is
`statement ÷ server-side`. At 252 rows the join is ~185 µs of which ~92 is the
client, predicting ≤2.0× and measuring 1.53×; at 10,080 rows it is ~1,470 µs of
which 92 is the client, predicting ≤1.06× and measuring 1.02×. PostgreSQL's
1.6× is two vCPUs' worth of backend processes and stops there for the same
reason.

Read the ckdbs column as **the single-thread dispatch ceiling made visible**,
and note that it takes three sizes to see it: measured only at 252 rows it looks
like ckdbs scales with connections, and measured only at 10,080 it looks like it
cannot. Neither statement alone is true. Caveat: the sweep drives every
connection from one Python process on a 2-vCPU machine, so at 8 connections the
client is itself contended; the comparison between engines is still fair because
both are driven that way.

## What the large-result shapes measure, and why they are not an engine result

Two shapes return their whole relation: `symbol-history` (all
252 / 1,008 / 10,080 feature rows, since there is one symbol) and `compare-all`
(every `model_results` row joined to its model). ckdbs looks 6.3× and 5.9×
faster on them respectively. **That number is mostly the two client libraries,
and it is not claimed as an engine result.**

The evidence for the caveat is inside the fit. `read-day-slice` walks the same
relation and returns one row, at 128 ns per row walked. `read-symbol-history`
walks the same relation and returns every row, at **2,106 ns per row** on ckdbs
and 13,270 ns on PostgreSQL. The extra 1,978 ns and 13,161 ns per row are
emission, wire format and client-side parsing — ckdbs returns one
newline-delimited blob that Python splits, PostgreSQL sends a `DataRow` message
per row that `pg_wire.py` parses individually. A protocol difference of that
size swamps whatever the engines are doing.

| shape | rows | engine | ops | mean µs | p0 | p25 | p50 | p95 | p99 |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|
| read-symbol-history | 252 | ckdbs | 10 | 507.0 | 493.7 | 497.4 | 500.6 | 541.5 | 541.5 |
| read-symbol-history | 1,008 | ckdbs | 10 | 1,680.8 | 1,627.1 | 1,634.6 | 1,647.1 | 1,828.4 | 1,828.4 |
| read-symbol-history | 10,080 | ckdbs | 10 | 24,119.1 | 20,155.0 | 20,459.8 | 20,748.6 | 45,111.9 | 45,111.9 |
| read-symbol-history | 252 | pg | 10 | 3,443.7 | 3,272.6 | 3,295.2 | 3,333.1 | 4,544.9 | 4,544.9 |
| read-symbol-history | 1,008 | pg | 10 | 13,168.8 | 12,496.4 | 12,542.2 | 12,585.4 | 15,009.6 | 15,009.6 |
| read-symbol-history | 10,080 | pg | 10 | 134,931.9 | 131,875.9 | 132,753.3 | 132,974.1 | 141,087.5 | 141,087.5 |
| compare-one | 252 | ckdbs | 32 | 165.7 | 159.1 | 161.0 | 162.3 | 180.3 | 184.7 |
| compare-one | 1,008 | ckdbs | 32 | 243.0 | 225.6 | 232.9 | 238.8 | 271.6 | 285.6 |
| compare-one | 10,080 | ckdbs | 32 | 1,089.4 | 1,042.0 | 1,053.8 | 1,069.2 | 1,143.1 | 1,494.1 |
| compare-one | 252 | pg | 32 | 339.3 | 325.3 | 328.4 | 330.8 | 360.9 | 464.5 |
| compare-one | 1,008 | pg | 32 | 650.3 | 567.7 | 595.1 | 598.7 | 1,010.8 | 1,035.6 |
| compare-one | 10,080 | pg | 32 | 4,248.4 | 3,769.2 | 3,822.3 | 4,044.2 | 5,441.7 | 5,835.5 |

One thing in this block *is* an engine finding: ckdbs's `read-symbol-history`
p95 at 10,080 rows is 45,112 µs against a p50 of 20,749. Its p95/p50 goes
1.08 → 1.11 → **2.17** across the three sizes while PostgreSQL's goes
1.36 → 1.19 → 1.06 — the two engines' tails move in opposite directions as the
result set grows. Ten samples is inside the 6.8 % floor for a *median*, not for a
p95, so this is flagged as worth a dedicated run rather than reported as a
result: something in the 10,080-row emission path occasionally costs double,
and nothing in this run says what.

---

## What this run teaches about the engine

**A statement's cost here is two numbers, and the engine is strong on the first
and slightly weak on the second.** ckdbs starts a statement for 35 µs where
PostgreSQL needs 62, and walks a row for 128 ns where PostgreSQL needs 109. Every
headline in this document follows from those four numbers: the pk shapes win
because they are all fixed cost, the FilterScans cross over near 1,400 rows
because that is where 27 µs of saved setup is eaten by 19 ns × n, and the
aggregate shapes split the same way because a fold adds a third term without
changing the first two. **A single-cardinality benchmark cannot distinguish
those two coefficients at all**, which is the concrete answer to why this
document has three sizes.

**The layer that dominates moves with cardinality, and it moves fast.** At 252
rows a `day-slice` statement is 58 % client-and-socket, 22 % engine fixed cost
and 20 % walking. At 10,080 rows it is 6 % client, 2 % fixed and 91 % walking.
Any optimization argument on this engine has to name the cardinality it is
about, and an optimization aimed at the fixed cost is worth almost nothing at
10,000 rows while one aimed at the per-row cost is worth almost nothing at 250.

**The Cabin is the one structure here that changes which of those two terms you
pay.** It does not make the walk cheaper — it removes it and leaves a cost
proportional to the *match set*, which is why the served QPS varies by 12 % over
a 40× relation for `day-slice` (one match) where the unserved statement falls
9.4×, and why it does fall — 4.7× — for `model-join` (~479 matches). That is
`docs/feat-cabin.md`'s superset-and-verify design appearing as a slope: what a
Cabin removes is the relation, and what it leaves is the match set.

**A real data point lands on an open decision.** `CLAUDE.md`'s Cabin section
lists **"the `CABIN AUTO` threshold (§8.1): what `use_count` × cardinality earns
a column a Cabin"** as open, with the policy named, stored and consumed by
nothing. This run says a threshold in `use_count` × cardinality alone would be
wrong: the same column, the same predicate and the same eight repeated arguments
bought **1.26× at 252 rows, 2.06× at 1,008 and 13.11× at 10,080**. The variable
that moved was neither use count nor cardinality — it was the size of the walk
avoided per hit. Any auto-promotion rule that does not carry relation size (or
better, rows-walked-per-match) will create Cabins that pay for themselves 13×
and Cabins that pay 1.26×, and will not be able to tell them apart. The
corollary for the sibling open item, the per-cabin budget: a budget spent on a
small relation is worth an order of magnitude less than the same budget spent on
a large one.

**Two design expectations were tested and held.** `docs/feat-aggregate.md`'s
claim that fold cost tracks group count rather than row count is confirmed on a
workload it was not measured on — 1 group costs 241 ns/row and 10,080 groups
cost 1,180 ns/row over the identical scan — and its per-group cost is 4.9–5.9×
below PostgreSQL's. `docs/parser-v2.md`'s claim that a compiled chain's probes
fire per surviving row is confirmed by `backtest-read` adding 31 µs of fixed cost
and 6 ns/row over the FilterScan it contains. Neither needed a new mechanism to
verify; both fell out of measuring the same shapes at three sizes.

**`cores = 1` has a measurable ceiling now, and it is the statement length.**
Aggregate throughput against connection count is 1.53× at 252 rows and 1.00× at
10,080, matching a model in which the only concurrency available is overlapping
one client's round trip with another's server time. That cores above 0 do not
serve statements is the documented state (`docs/workplan-crosscore.md`, P2 and
P6); what this run adds is a number for what it costs as a function of statement
weight, and the answer is that the cost is small for short statements and total
for long ones.

**Where the data supports no insight, it is left alone.** The 21 % rise in
ckdbs's per-group fold cost between the two segments is above the floor and
unexplained; the `read-symbol-history` p95 outlier at 10,080 rows is
unexplained; the 1K autocommit write throughput is unexplained. All three are
recorded as observations with the measurement that produced them, and none is
attributed to a cause this run can support.

## Reproducing

Driver flags, defaults and exact invocations are in
[`bench/docs/README.md`](docs/README.md). The six runs behind this document were:

```bash
# ckdbs — fresh server and fresh data file per size, on the block device
./build-release/kds_server ~/bench-s1/<tag>.db --port <port>
python3 tools/scenario1_backtest.py --port <port> --symbols 1 --years <1|4|40> \
    --seed 1 --verify --json ~/bench-s1/<tag>.ck.json

# PostgreSQL — the same three sizes, defaults, sequentially and never alongside
./tools/pg_setup.sh start
python3 tools/pg_scenario1_backtest.py --port 15433 --database bench \
    --symbols 1 --years <1|4|40> --seed 1 --verify --json ~/bench-s1/<tag>.pg.json

python3 tools/compare_scenario1.py <tag>.ck.json <tag>.pg.json
```

The 10,080 configuration was run a second time, whole, to establish the noise
floor. The client + socket floor is 500 × `PING` on the same server after the
run; the device fsync figure is three separate 300-sample probes of an 8 KiB
write plus `fsync` on the same volume.
