# range100: a strict tuple key window, A against B, with a PostgreSQL 18 floor

**Thesis.** Before `da44376a`, `id >= lo AND id < lo + 100` walked the whole relation; after it, the statement is a two-page range walk whose server cost does not depend on the relation size. At 10,000 rows the server CPU per statement falls from about 430 us to about 72 us, at 100,000 rows from about 3,500 us to about 73 us. At 200 rows nothing changes (the relation is two pages), and the control spelling `BETWEEN`, which both arms already range-walked, is unchanged. B's cost on the strict spelling equals its cost on `BETWEEN` at every size.

## Run stamp

| Item | Value |
|---|---|
| Executed | 2026-10-15, 12:29 to 12:36 UTC (five runs, one per size per run) |
| Arm A | `9b7d53d7`, `v3.2.0-64-g9b7d53d7`: a strict window walks the relation |
| Arm A' | the same bytes as A, a second server (the in-run null) |
| Arm B | `da44376a`, `v3.2.0-65-gda44376a`: a strict window is a range walk. `a1de2db5` (`v3.2.0-68-ga1de2db5`, main) differs from B in docs/bench only |
| Build | plain Release `kds_server`, `git archive <commit>` into a scratch directory, never a worktree |
| Binary A | sha256 `ed1e4fd476a9cb17d32204861f697b97483b9eb9f12aca7504f9215577d8b804`, built 2026-10-15 12:27:27 UTC (after both commits' timestamps, so it is at its commit) |
| Binary B | sha256 `75900be9cb20432e78e7301365228204b2fe15c91cdaac408c333a1b6292387a`, built 2026-10-15 12:29:02 UTC |
| Measured copies | both binaries were copied before the first cell and every server (A, A' both from A's copy) started from the copy |
| Tree | the measuring worktree `bench-range100-pg` at `a1de2db5`, clean; servers ran from archived exports, so the tree state does not enter |
| Server | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536` (every page resident), fresh data file per server per run |
| Device | `/dev/root`, ext4 (not tmpfs); data files under `/home/cdkbs/r100-work`, PostgreSQL's cluster `/home/cdkbs/pg-bench-ba` on the same device. A relaxed run never syncs on the statement path, so the device matters little here |
| Pinning | server CPU 2, driver CPU 4, PostgreSQL backend CPU 2; A, A', B and PG share CPU 2 and only one is ever busy |
| PostgreSQL | PG 18 (`PG_VERSION` 18), port 15434, default tuning, `id bigint PRIMARY KEY`, `VACUUM (ANALYZE)` after load, simple query protocol |
| Tests | no code changed by this measurement; the correctness suite was not run by this stage |

Driver and runner: `archive/range100-v3.2.0-65-gda44376a/drivers/` (`r100_ab.py`, `run_all.sh`, `pass2.sh`, `assemble.py`); raw JSON and logs in `archive/range100-v3.2.0-65-gda44376a/raw/`. Not yet documented in `bench/docs/`.

## Method

Relation `be_r (id int64, v int64, w int64) BTREE`, rows `(i, i, i)` for `i = 1..N`, loaded with named ascending keys: BK's `be_r` layout (`bk_overhead_ab.py`). The PostgreSQL twin has the same three columns. `N` is the `--rows` flag: 200, 1,000, 10,000, 100,000 (sizes 200 to 10,000 are the bench sweep rule; 100,000 is where A's walk is largest). `lo = 1 + (q * 7919) % (N - 100)` varies per statement across the key space; `lo + 100` is rendered as a literal. Statements are literal text over an unnamed parse (the `literal` path of `bn_census.py`).

| Shape | SQL |
|---|---|
| `range100` | `SELECT COUNT(*), MIN(id), MAX(id) FROM be_r WHERE id >= lo AND id < lo+100` (1 row out) |
| `win100-rows` | `SELECT * FROM be_r WHERE id >= lo AND id < lo+100` (100 rows out) |
| `between100` (control) | `... WHERE id BETWEEN lo AND lo+99` (1 row out) |

Each run starts three KDS servers (A, A', B) and uses one PG backend. Each cell is 400 statements in 20 blocks of 20; every block runs the same statements on all four arms, the arm order rotating per block and per run. Five runs (0 to 4), so 2,000 statements per arm per cell. Server CPU is the sum over all server threads of `/proc/<pid>/task/*/schedstat` over the arm's blocks (the PG backend's own schedstat for PG), divided by statements; the server's idle burn is not subtracted. The headline tables give the median over runs of each run's figure, and percentiles are medians of per-run percentiles. Throughput is derived (1e6 / p50 wall); the driver is a serial single connection. The wall clock includes the Python client, which is the same for A, A' and B and differs for PG.

## Plan evidence and correctness

`ANALYZE` of the statement at `lo = 5` (every size and run, recorded in the raw JSON; shown here for the 100,000 row relation):

| Arm | Plan |
|---|---|
| A, `range100` and `win100-rows` | `class=JoinSelect`, `step 0 Scan be_r`, filters `id >= 5`, `id < 105`; `examined=100000 pages=603` (examined 1,000 / 10,000 at the smaller sizes, 7 / 61 pages) |
| B, both shapes | `class=RangeSelect`, `step 0 Range be_r range=[5, 104]`, same filters kept as residuals; `examined=166 pages=2 range_stopped_early=1`, at every size |
| A and B, `between100` | `class=RangeSelect`, `Range be_r range=[5, 104]`, `examined=166 pages=2` (A and B agree, the code being unchanged) |
| PG, 200 rows | `Seq Scan` with filter (the table is one page) |
| PG, 1,000 rows and up | `Index Only Scan using be_r_pkey` (aggregates and `between`), `Index Scan using be_r_pkey` (`win100-rows`) |

Results agree: before timing, each run executed 24 window positions per shape (the edges `lo = 1` and `lo = N - 99`, `lo = 2`, the middle and 20 spread positions) on A, A' and B and compared raw row bytes: 0 mismatches in all 20 size-run cells. Every row count was also checked on PG, and every timed statement's row count was checked on every arm: 0 bad replies.

## Findings

1. **A strict window now costs what `BETWEEN` costs.** B's `range100` server CPU is 72.5, 69.2, 71.7 and 72.8 us at 200 / 1,000 / 10,000 / 100,000 rows, against 65.6, 72.0, 73.7 and 64.0 us for `between100` on B. The window walk is flat in relation size, as `examined=166 pages=2` says.
2. **The saving is the walk, and it grows with the relation.** B/A server CPU is 0.97 (200), 0.67 (1,000), 0.17 (10,000) and 0.02 (100,000) on `range100`; on `win100-rows` 0.99, 0.75, 0.20 and 0.03. The 200 row figure is inside the noise floor (below).
3. **Control agrees.** `between100` B/A is 1.03, 0.98, 1.00 and 1.02: A and B behave alike where the code did not change.
4. **Against the floor.** B's server CPU is 0.61 to 0.82 of the PG backend's on every shape and size (a PG statement spends about 100 us of backend CPU in this driver whatever the plan), and the p50 wall of `range100` is 0.87 to 0.94 of PG's. `win100-rows` wall is 1.11 to 1.21 of PG's: 100 rows of three int64 columns cost the KDS client path more wall time than the PG client path, while the server CPU is lower (0.81 to 0.84). PG is the floor and says only that B is in the league; A at 10,000 rows and up was outside it on `range100` (4 to 34 times PG's CPU).
5. **The remaining cost is fixed.** B's 52 to 84 us per `range100` (across runs) is the same as `between100` and is almost independent of whether 100 or 1 rows come out of the aggregate; `win100-rows` adds 10 to 21 us of server CPU for emitting 100 rows, and 218 to 234 us of wall in the client. The window walk itself (two pages, 100 keys) is not separable from the per-statement fixed cost with this design.

## Noise floor and host

The null is A'/A (identical bytes, second server). The per-run server CPU ratio A'/A stays within 0.95 to 1.04 in 7 of the 12 size-shape cells and spans 0.82 to 1.04 overall (worst: 10,000 `win100-rows`, 0.823 in run 4; 10,000 `range100`, 0.905 in one run); the paired table below gives the range per cell. **Any B/A within 0.9 to 1.1 is not a finding**; this covers the 200 row cells (B/A 0.97, 0.99, 1.03) and the three `between100` controls. Only the 1,000 row `range100` and `win100-rows` cells (B/A 0.655 to 0.751 over the five runs) and the larger strict-window cells have a delta outside the null.

**Host caveat.** The host is shared. Runs 0 to 3 ran while another session was building and running its test suite on other CPUs (the load average at cell start fell from 11 to 5.5; the other CPUs' mean busy % was 30 to 65 in those runs); run 4 ran with the others near 0 % (load 4.9 to 5.2 decaying from the earlier work). Absolute microseconds therefore drift between runs by up to 1.6 times (200 row `range100` A: 50.2 to 81.8 us) while the arms in one run, interleaved block by block, drift together. Read ratios, not absolutes. CPU 2 itself was also shared in time with the in-run arms only. A's 100,000 row walk fell from 5,078 us (run 0) to 2,813 us (run 4) with the others' load, so B/A at that size is 0.016 to 0.021 (48 to 62 times) across runs, not one number. Per-cell load, CPU 2 / CPU 4 busy % and the other CPUs' mean busy % are listed in the last section. This stage's own builds finished before the first cell; the other session's compile was still running when the binaries were copied and a test run of theirs overlapped the first pass.

## Insight

The strict half-open window is the spelling most clients write, and until this change it was the one that missed the btree's key order: at 100,000 rows the engine read 603 pages for 100 keys. The change brings the cost of that spelling to the cost of the `BETWEEN` spelling, which at 72 us per statement is already dominated by the per-statement fixed cost (parse, plan, reply), not by the tree. That also bounds what any further planner work on this shape can buy at `cores = 1`: B has about 52 to 84 us of server CPU to give, PG's backend about 80 to 115 us. The remaining open decision, where a strict range belongs against an index, a Cabin, a filter scan and a built join (`docs/spec/index.md` §9, named in the commit message), is not exercised: nothing else serves this relation, so these cells say nothing about it.

## Cut and not measured

- Cores above 1, `strict`/`group` durability, the prepared path, a cold buffer pool, concurrent writers.
- A wide window (a strict range covering most of the relation) and a window starting at the low or high edge only; the 24 verification positions include the edges but were not timed.
- Percentiles are medians of per-run percentiles over 5 runs of 400 statements, not percentiles of a pooled sample.
- No prior results file for this shape at a v3.2.0 describe: this run is the baseline that the next `range100` run is read against. BK's close figure for A's shape (86 / 146 / 805 us wall at 200 / 1,000 / 10,000 rows, quoted in the commit message of `da44376a`) is another version's record and is not compared here.

# Tables

All figures below are generated by `archive/range100-v3.2.0-65-gda44376a/drivers/assemble.py` from the raw JSON (copy in `assembled.md`). `cpu` is server CPU per statement in us; `p50` is the client's wall time per statement in us; B/A and B/PG are ratios of the same metric. Throughput is derived (1e6 / p50 wall).

## Headline: p50 wall (us) and server CPU (us/stmt), median of runs

| rows | shape | runs | A cpu | A' cpu | B cpu | PG cpu | B/A cpu | B/PG cpu | A p50 | A' p50 | B p50 | PG p50 | B/A p50 | B/PG p50 | A-A' cpu | B-A cpu |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 200 | range100 | 5 | 74.6 | 73.2 | 72.5 | 107.6 | 0.971 | 0.67 | 111.8 | 111.8 | 109.5 | 125.5 | 0.980 | 0.87 | -1.4 | -2.1 |
| 200 | win100-rows | 5 | 93.6 | 91.0 | 92.3 | 108.8 | 0.985 | 0.85 | 333.8 | 323.5 | 337.1 | 277.8 | 1.010 | 1.21 | -2.6 | -1.4 |
| 200 | between100 | 5 | 63.7 | 63.2 | 65.6 | 102.2 | 1.030 | 0.64 | 106.1 | 99.2 | 103.4 | 122.6 | 0.975 | 0.84 | -0.5 | +1.9 |
| 1000 | range100 | 5 | 103.3 | 102.7 | 69.2 | 101.1 | 0.670 | 0.68 | 150.7 | 150.7 | 108.9 | 121.9 | 0.723 | 0.89 | -0.5 | -34.1 |
| 1000 | win100-rows | 5 | 124.3 | 117.5 | 92.9 | 110.3 | 0.747 | 0.84 | 370.0 | 364.5 | 325.0 | 292.3 | 0.878 | 1.11 | -6.8 | -31.4 |
| 1000 | between100 | 5 | 73.2 | 73.4 | 72.0 | 105.0 | 0.984 | 0.69 | 112.8 | 113.4 | 111.4 | 125.8 | 0.988 | 0.89 | +0.3 | -1.2 |
| 10000 | range100 | 5 | 433.1 | 438.3 | 71.7 | 103.7 | 0.166 | 0.69 | 520.1 | 435.7 | 111.4 | 124.1 | 0.214 | 0.90 | +5.2 | -361.4 |
| 10000 | win100-rows | 5 | 461.7 | 475.5 | 92.9 | 110.1 | 0.201 | 0.84 | 765.6 | 775.1 | 345.8 | 296.0 | 0.452 | 1.17 | +13.8 | -368.9 |
| 10000 | between100 | 5 | 73.8 | 73.7 | 73.7 | 106.7 | 0.999 | 0.69 | 113.0 | 113.0 | 112.2 | 127.2 | 0.993 | 0.88 | -0.2 | -0.1 |
| 100000 | range100 | 5 | 3508.4 | 3662.7 | 72.8 | 105.0 | 0.021 | 0.69 | 3146.5 | 3247.3 | 115.5 | 122.6 | 0.037 | 0.94 | +154.4 | -3435.6 |
| 100000 | win100-rows | 5 | 3291.3 | 3253.4 | 83.2 | 102.3 | 0.025 | 0.81 | 3181.0 | 3178.9 | 333.3 | 281.0 | 0.105 | 1.19 | -37.8 | -3208.1 |
| 100000 | between100 | 5 | 62.5 | 61.4 | 64.0 | 97.5 | 1.023 | 0.66 | 92.0 | 92.0 | 91.9 | 105.7 | 0.999 | 0.87 | -1.1 | +1.5 |

## Throughput (derived 1e6 / p50 wall), stmts/s

| rows | shape | A | A' | B | PG |
|---|---|---|---|---|---|
| 200 | range100 | 8,946 | 8,941 | 9,131 | 7,970 |
| 200 | win100-rows | 2,996 | 3,091 | 2,967 | 3,599 |
| 200 | between100 | 9,424 | 10,077 | 9,671 | 8,154 |
| 1000 | range100 | 6,637 | 6,636 | 9,184 | 8,207 |
| 1000 | win100-rows | 2,702 | 2,743 | 3,077 | 3,421 |
| 1000 | between100 | 8,866 | 8,821 | 8,976 | 7,950 |
| 10000 | range100 | 1,923 | 2,295 | 8,973 | 8,055 |
| 10000 | win100-rows | 1,306 | 1,290 | 2,892 | 3,378 |
| 10000 | between100 | 8,852 | 8,852 | 8,912 | 7,860 |
| 100000 | range100 | 318 | 308 | 8,661 | 8,154 |
| 100000 | win100-rows | 314 | 315 | 3,000 | 3,558 |
| 100000 | between100 | 10,871 | 10,864 | 10,877 | 9,458 |

## Distribution (median over runs), us

| rows | shape | arm | ops/run | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| 200 | range100 | A | 400 | 76.0 | 107.9 | 111.8 | 172.7 | 256.2 |
| 200 | range100 | Ap | 400 | 75.6 | 108.4 | 111.8 | 159.0 | 192.1 |
| 200 | range100 | B | 400 | 74.5 | 105.8 | 109.5 | 178.6 | 260.9 |
| 200 | range100 | PG | 400 | 93.0 | 118.6 | 125.5 | 204.5 | 335.3 |
| 200 | win100-rows | A | 400 | 214.0 | 268.2 | 333.8 | 471.7 | 981.3 |
| 200 | win100-rows | Ap | 400 | 214.9 | 264.8 | 323.5 | 409.1 | 667.4 |
| 200 | win100-rows | B | 400 | 214.4 | 270.9 | 337.1 | 460.3 | 915.2 |
| 200 | win100-rows | PG | 400 | 175.3 | 225.3 | 277.8 | 444.0 | 687.5 |
| 200 | between100 | A | 400 | 71.7 | 86.7 | 106.1 | 153.6 | 193.3 |
| 200 | between100 | Ap | 400 | 71.8 | 85.2 | 99.2 | 166.8 | 386.8 |
| 200 | between100 | B | 400 | 72.1 | 85.5 | 103.4 | 145.1 | 187.1 |
| 200 | between100 | PG | 400 | 89.5 | 100.1 | 122.6 | 226.1 | 425.6 |
| 1000 | range100 | A | 400 | 105.0 | 121.1 | 150.7 | 191.7 | 290.1 |
| 1000 | range100 | Ap | 400 | 103.1 | 120.0 | 150.7 | 189.6 | 237.6 |
| 1000 | range100 | B | 400 | 79.0 | 97.5 | 108.9 | 165.5 | 265.8 |
| 1000 | range100 | PG | 400 | 86.3 | 109.9 | 121.9 | 190.1 | 390.9 |
| 1000 | win100-rows | A | 400 | 232.9 | 327.4 | 370.0 | 441.9 | 533.1 |
| 1000 | win100-rows | Ap | 400 | 235.1 | 326.6 | 364.5 | 421.9 | 455.2 |
| 1000 | win100-rows | B | 400 | 219.1 | 303.5 | 325.0 | 380.9 | 465.0 |
| 1000 | win100-rows | PG | 400 | 185.3 | 258.2 | 292.3 | 355.8 | 515.0 |
| 1000 | between100 | A | 400 | 80.0 | 106.7 | 112.8 | 168.9 | 199.1 |
| 1000 | between100 | Ap | 400 | 80.9 | 107.1 | 113.4 | 161.8 | 174.8 |
| 1000 | between100 | B | 400 | 80.7 | 105.0 | 111.4 | 164.6 | 188.0 |
| 1000 | between100 | PG | 400 | 91.8 | 118.7 | 125.8 | 185.2 | 215.5 |
| 10000 | range100 | A | 400 | 324.8 | 384.9 | 520.1 | 660.6 | 696.9 |
| 10000 | range100 | Ap | 400 | 335.1 | 388.0 | 435.7 | 653.9 | 809.3 |
| 10000 | range100 | B | 400 | 75.2 | 97.4 | 111.4 | 191.1 | 263.4 |
| 10000 | range100 | PG | 400 | 84.6 | 110.6 | 124.1 | 204.0 | 375.4 |
| 10000 | win100-rows | A | 400 | 546.3 | 636.0 | 765.6 | 964.3 | 1057.7 |
| 10000 | win100-rows | Ap | 400 | 549.4 | 642.7 | 775.1 | 876.0 | 978.5 |
| 10000 | win100-rows | B | 400 | 217.9 | 322.7 | 345.8 | 448.3 | 936.9 |
| 10000 | win100-rows | PG | 400 | 186.5 | 275.7 | 296.0 | 383.5 | 526.3 |
| 10000 | between100 | A | 400 | 92.4 | 108.4 | 113.0 | 152.0 | 189.0 |
| 10000 | between100 | Ap | 400 | 91.6 | 108.8 | 113.0 | 146.5 | 168.8 |
| 10000 | between100 | B | 400 | 90.0 | 106.3 | 112.2 | 145.9 | 178.7 |
| 10000 | between100 | PG | 400 | 106.7 | 119.6 | 127.2 | 171.9 | 224.2 |
| 100000 | range100 | A | 400 | 2647.1 | 2910.9 | 3146.5 | 5247.1 | 5582.4 |
| 100000 | range100 | Ap | 400 | 2626.4 | 2904.0 | 3247.3 | 5366.5 | 5994.8 |
| 100000 | range100 | B | 400 | 75.8 | 97.6 | 115.5 | 257.7 | 306.7 |
| 100000 | range100 | PG | 400 | 93.8 | 108.4 | 122.6 | 309.0 | 404.6 |
| 100000 | win100-rows | A | 400 | 2774.0 | 3039.2 | 3181.0 | 5563.9 | 5772.4 |
| 100000 | win100-rows | Ap | 400 | 2760.6 | 3026.0 | 3178.9 | 5571.3 | 5771.4 |
| 100000 | win100-rows | B | 400 | 214.1 | 311.0 | 333.3 | 418.7 | 510.4 |
| 100000 | win100-rows | PG | 400 | 180.6 | 265.5 | 281.0 | 450.8 | 547.7 |
| 100000 | between100 | A | 400 | 73.5 | 83.6 | 92.0 | 158.2 | 205.0 |
| 100000 | between100 | Ap | 400 | 71.9 | 83.3 | 92.0 | 150.2 | 185.9 |
| 100000 | between100 | B | 400 | 73.1 | 82.4 | 91.9 | 153.9 | 176.6 |
| 100000 | between100 | PG | 400 | 86.6 | 98.4 | 105.7 | 188.4 | 249.7 |

## Per-run server CPU (us/stmt): A | A' | B | PG, by run

- 200 range100: A=[74.6, 60.2, 81.8, 79.6, 50.2]; Ap=[73.2, 59.5, 79.7, 82.6, 50.0]; B=[72.5, 63.1, 81.5, 77.3, 51.0]; PG=[107.6, 96.6, 115.1, 114.5, 80.0]
- 200 win100-rows: A=[94.9, 90.5, 103.2, 93.6, 58.6]; Ap=[91.0, 87.7, 103.1, 95.7, 59.7]; B=[92.3, 87.8, 101.1, 101.0, 60.0]; PG=[108.8, 105.0, 116.5, 115.7, 73.7]
- 200 between100: A=[69.8, 63.7, 51.0, 69.9, 47.4]; Ap=[70.4, 63.2, 49.1, 71.7, 48.2]; B=[69.2, 65.6, 49.9, 75.9, 48.4]; PG=[107.0, 102.2, 83.7, 108.4, 79.7]
- 1000 range100: A=[103.3, 118.3, 76.4, 109.8, 77.6]; Ap=[102.7, 117.8, 77.0, 111.6, 75.4]; B=[69.2, 77.5, 55.8, 81.7, 52.5]; PG=[101.1, 109.9, 84.3, 112.5, 84.4]
- 1000 win100-rows: A=[136.8, 133.9, 92.1, 124.3, 86.0]; Ap=[135.4, 131.6, 89.0, 117.5, 85.1]; B=[95.0, 93.7, 68.2, 92.9, 64.6]; PG=[110.9, 110.3, 89.7, 115.8, 80.7]
- 1000 between100: A=[73.2, 57.8, 78.9, 77.6, 52.8]; Ap=[73.4, 56.0, 78.0, 76.3, 51.1]; B=[72.0, 59.7, 77.8, 74.5, 50.1]; PG=[105.0, 93.5, 112.4, 117.3, 83.1]
- 10000 range100: A=[433.1, 431.3, 545.6, 451.6, 326.0]; Ap=[438.3, 439.0, 541.6, 408.7, 330.0]; B=[71.7, 71.3, 84.1, 74.0, 54.6]; PG=[101.2, 103.7, 115.9, 114.3, 84.1]
- 10000 win100-rows: A=[533.7, 459.4, 461.7, 477.9, 407.0]; Ap=[523.0, 475.5, 475.0, 487.6, 334.8]; B=[92.9, 90.7, 104.0, 97.8, 66.9]; PG=[110.1, 106.9, 121.6, 119.9, 82.1]
- 10000 between100: A=[73.8, 73.8, 76.2, 82.2, 54.0]; Ap=[73.5, 73.7, 76.7, 79.0, 52.2]; B=[72.8, 73.7, 76.2, 75.9, 52.2]; PG=[105.8, 106.7, 110.0, 113.5, 82.9]
- 100000 range100: A=[5078.0, 5014.1, 3508.4, 2930.3, 2812.8]; Ap=[5055.9, 4966.2, 3662.7, 2889.7, 2794.9]; B=[84.1, 82.7, 72.8, 63.0, 59.9]; PG=[117.1, 116.0, 105.0, 96.0, 92.2]
- 100000 win100-rows: A=[3291.3, 3959.9, 3599.1, 2803.5, 2780.1]; Ap=[3253.4, 3730.5, 3500.3, 2796.9, 2800.3]; B=[83.2, 90.0, 91.5, 68.5, 68.7]; PG=[102.3, 112.2, 119.8, 87.0, 88.3]
- 100000 between100: A=[62.5, 76.1, 75.0, 54.7, 53.0]; Ap=[61.4, 74.6, 75.3, 55.3, 52.8]; B=[64.0, 75.0, 74.1, 56.7, 52.3]; PG=[97.5, 108.4, 111.3, 88.2, 83.6]

## Host per cell (loadavg at start, CPU 2 / CPU 4 busy %, other-session processes seen)

- 200 range100: r0: load 11.08, c2 20, c4 12, others-busy 54 | r1: load 8.85, c2 5, c4 3, others-busy 33 | r2: load 5.63, c2 12, c4 9, others-busy 29 | r3: load 6.14, c2 68, c4 41, others-busy 65 | r4: load 5.57, c2 54, c4 44, others-busy 2
- 200 win100-rows: r0: load 11.08, c2 19, c4 36, others-busy 57 | r1: load 8.85, c2 8, c4 11, others-busy 33 | r2: load 5.63, c2 39, c4 72, others-busy 66 | r3: load 6.14, c2 51, c4 71, others-busy 61 | r4: load 5.57, c2 26, c4 76, others-busy 1
- 200 between100: r0: load 11.08, c2 58, c4 44, others-busy 62 | r1: load 9.02, c2 5, c4 5, others-busy 31 | r2: load 5.63, c2 56, c4 50, others-busy 67 | r3: load 6.14, c2 68, c4 52, others-busy 62 | r4: load 5.20, c2 57, c4 53, others-busy 1
- 1000 range100: r0: load 10.60, c2 17, c4 9, others-busy 43 | r1: load 8.57, c2 16, c4 14, others-busy 41 | r2: load 5.50, c2 62, c4 48, others-busy 48 | r3: load 6.14, c2 66, c4 48, others-busy 62 | r4: load 5.20, c2 63, c4 35, others-busy 2
- 1000 win100-rows: r0: load 10.60, c2 18, c4 37, others-busy 47 | r1: load 8.57, c2 12, c4 24, others-busy 44 | r2: load 5.50, c2 8, c4 23, others-busy 29 | r3: load 6.14, c2 61, c4 70, others-busy 59 | r4: load 5.20, c2 35, c4 68, others-busy 4
- 1000 between100: r0: load 10.60, c2 62, c4 46, others-busy 54 | r1: load 8.57, c2 9, c4 7, others-busy 38 | r2: load 5.50, c2 79, c4 71, others-busy 67 | r3: load 6.14, c2 69, c4 48, others-busy 63 | r4: load 5.20, c2 60, c4 47, others-busy 0
- 10000 range100: r0: load 10.15, c2 19, c4 10, others-busy 42 | r1: load 8.44, c2 82, c4 23, others-busy 63 | r2: load 5.54, c2 86, c4 21, others-busy 68 | r3: load 6.05, c2 72, c4 60, others-busy 58 | r4: load 5.20, c2 84, c4 16, others-busy 1
- 10000 win100-rows: r0: load 10.15, c2 26, c4 24, others-busy 53 | r1: load 8.16, c2 42, c4 40, others-busy 56 | r2: load 5.54, c2 67, c4 49, others-busy 67 | r3: load 6.05, c2 63, c4 59, others-busy 59 | r4: load 5.20, c2 59, c4 42, others-busy 1
- 10000 between100: r0: load 9.82, c2 60, c4 41, others-busy 67 | r1: load 8.16, c2 62, c4 38, others-busy 62 | r2: load 5.54, c2 58, c4 48, others-busy 66 | r3: load 6.05, c2 63, c4 48, others-busy 64 | r4: load 4.87, c2 62, c4 36, others-busy 1
- 100000 range100: r0: load 9.83, c2 87, c4 9, others-busy 62 | r1: load 8.23, c2 92, c4 6, others-busy 60 | r2: load 5.65, c2 95, c4 25, others-busy 63 | r3: load 5.97, c2 96, c4 12, others-busy 13 | r4: load 4.87, c2 97, c4 4, others-busy 1
- 100000 win100-rows: r0: load 9.44, c2 64, c4 12, others-busy 50 | r1: load 8.05, c2 80, c4 13, others-busy 59 | r2: load 6.24, c2 86, c4 49, others-busy 60 | r3: load 5.57, c2 89, c4 13, others-busy 4 | r4: load 4.56, c2 89, c4 12, others-busy 1
- 100000 between100: r0: load 9.17, c2 6, c4 8, others-busy 28 | r1: load 7.97, c2 61, c4 41, others-busy 66 | r2: load 6.24, c2 58, c4 48, others-busy 64 | r3: load 5.57, c2 61, c4 38, others-busy 2 | r4: load 4.56, c2 59, c4 44, others-busy 0

bad replies: 0

## Paired per-run ratios (server CPU): A'/A (null) and B/A, min..max over runs; quiet run 4 alone

| rows | shape | A'/A min..max | B/A min..max | run-4 (quiet) A'/A | run-4 B/A | run-4 B/PG cpu | run-4 B/A p50 wall | run-4 A cpu us | run-4 B cpu us |
|---|---|---|---|---|---|---|---|---|---|
| 200 | range100 | 0.974..1.037 | 0.971..1.049 | 0.995 | 1.015 | 0.64 | 1.001 | 50.2 | 51.0 |
| 200 | win100-rows | 0.958..1.022 | 0.969..1.079 | 1.018 | 1.024 | 0.81 | 1.009 | 58.6 | 60.0 |
| 200 | between100 | 0.962..1.026 | 0.978..1.086 | 1.016 | 1.020 | 0.61 | 1.014 | 47.4 | 48.4 |
| 1000 | range100 | 0.972..1.016 | 0.655..0.744 | 0.972 | 0.677 | 0.62 | 0.752 | 77.6 | 52.5 |
| 1000 | win100-rows | 0.945..0.990 | 0.694..0.751 | 0.990 | 0.751 | 0.80 | 0.910 | 86.0 | 64.6 |
| 1000 | between100 | 0.967..1.003 | 0.949..1.032 | 0.967 | 0.949 | 0.60 | 0.963 | 52.8 | 50.1 |
| 10000 | range100 | 0.905..1.018 | 0.154..0.167 | 1.012 | 0.167 | 0.65 | 0.223 | 326.0 | 54.6 |
| 10000 | win100-rows | 0.823..1.035 | 0.164..0.225 | 0.823 | 0.164 | 0.82 | 0.392 | 407.0 | 66.9 |
| 10000 | between100 | 0.961..1.005 | 0.924..0.999 | 0.967 | 0.968 | 0.63 | 0.973 | 54.0 | 52.2 |
| 100000 | range100 | 0.986..1.044 | 0.016..0.021 | 0.994 | 0.021 | 0.65 | 0.029 | 2812.8 | 59.9 |
| 100000 | win100-rows | 0.942..1.007 | 0.023..0.025 | 1.007 | 0.025 | 0.78 | 0.076 | 2780.1 | 68.7 |
| 100000 | between100 | 0.980..1.011 | 0.985..1.036 | 0.996 | 0.988 | 0.63 | 0.986 | 53.0 | 52.3 |
