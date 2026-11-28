# PostgreSQL 18.7 against KDS on one host, scenarios 1 to 4: a comparison of two existing measurements

**One line.** On this host, with both servers pinned to the P-cores, KDS at `6c98f7c32` is ahead of PostgreSQL 18.7 where a statement examines many rows and discards them (an unindexed equality, a correlated subquery, a join, a 10,000-row tape: 1.35x to 2.94x), and behind it on three things: the fixed cost of a short statement (0.73x to 0.76x), the durable commit path (0.81x to 0.84x) and a full-scan aggregate (0.47x). **Nothing was measured for this file**: it sets two results files side by side, and the KDS side is an engine 28 first-parent commits older than `main`.

Every ratio is KDS over PostgreSQL in throughput terms (above 1, KDS is faster); for the scenario-4 latencies it is PostgreSQL's mean latency over KDS's.

## Stamp

| Field | Value |
|---|---|
| Written | 2026-11-28, on the operator's choice of "compare at once with the existing numbers" over a fresh KDS run (a fresh run exceeds the 20-minute stage budget) |
| PostgreSQL side | `results-pg-control-scenarios-v3.4.17-202-g481d101aa.md`: PostgreSQL 18.7 at its defaults in a container on the host network, `synchronous_commit = on`, drivers of `481d101aa`, **2026-11-28 00:18:31 to 00:32:03 UTC**, three runs per cell |
| KDS side | `interim-scenarios-v3.4.17-166-g6c98f7c32.md`, the pinned series: engine `6c98f7c32` (`v3.4.17-166-g6c98f7c32`), Release, **2026-11-26 23:28:50 to 2026-11-27 01:10:03 UTC**; the three-engine A/B's "new" column (five rounds) for scenarios 1 and 3 and the 1-booker `strict` row, the "Scenario 2, pinned" table (three runs) for the 1-booker `group` row, the "Scenario 4, pinned" table (median of 9 day-runs) for scenario 4 |
| Host and pinning, both sides | Intel Core Ultra 7 270K Plus, 24 CPUs; server on CPUs 0 to 7 (the P-cores), driver on CPUs 8 to 23; data on `/dev/mapper/fedora-root`, xfs |
| Rule | spread = (max - min) / median over a cell's runs; a difference of medians smaller than the larger of the two spreads is **inside the spread** and no finding; a spread over 20 % on either side carries **no claim** |
| Suite | not executed; no code changed |

## What bounds every ratio below

1. **The KDS engine is not `main`.** `6c98f7c32` is 28 first-parent commits and 75 files of `src/` and `include/` behind `7556a5596`; EA (a prepared statement parsed once) and EB (the key forms in the executor) landed in between. Whether any of them moved a shape below is unverified.
2. **The two sides are about 24 hours apart**, on a host that runs other projects' containers throughout.
3. **Scenario 4 is not the same procedure on the two sides**: KDS runs three paced days and the figure is the median of nine day-runs of the `off` arm (no Cabin) or the `declared` arm; PostgreSQL runs day 1 once per run, unpaced, with no index on `symbol`.
4. **Scenario 1's KDS cells are `group` durability** and PostgreSQL's are `synchronous_commit = on`. The read shapes are unaffected; `result-insert` may carry the difference (unverified).
5. **The KDS scenario-3 A/B's size is `[assumed]` 10,000 loans**: the interim file does not state it, the `9f8f8b354` pinned A/B it follows was 10,000 loans, and the raw JSON of the `6c98f7c32` run is no longer on the host.
6. **No percentile is compared.** The KDS side survives as medians and spreads only.

## The matrix

| Scenario | Shape | KDS `6c98f7c32` (spread) | PostgreSQL 18.7 (spread) | KDS / PG | Read |
|---|---|---|---|---|---|
| 1, 60,480 bars, `c1-g` | `day-slice` warm | 1,395.8 qps (7.2 %) | 812 qps (10.4 %) | **1.72x** | KDS ahead |
| 1 | `cross-join` warm | 1,356.0 qps (6.2 %) | 716 qps (27.0 %) | 1.89x | no claim |
| 1 | `bar-range` warm | 1,740.7 qps (5.3 %) | 1,709 qps (2.0 %) | 1.02x | inside the spread |
| 1 | `model-join` warm | 1,519.6 qps (2.2 %) | 1,446 qps (5.1 %) | 1.05x | inside the spread |
| 1 | phase `backtest-read` | 1,343.2 qps (3.5 %) | 778 qps (11.6 %) | **1.73x** | KDS ahead |
| 1 | phase `result-insert` (autocommit) | 4,859.0 qps (1.7 %) | 5,981 qps (4.7 %) | **0.81x** | PostgreSQL ahead |
| 1 | phase `load-bars` (batched) | 45,174.8 qps (1.3 %) | 38,023 qps (2.6 %) | **1.19x** | KDS ahead |
| 2, 1 booker, 1 core | `strict` | 1,751.4 tps (0.8 %) | 2,087.1 tps (3.9 %) | **0.84x** | PostgreSQL ahead |
| 2, 1 booker, 1 core | `group` | 1,747.5 tps (0.5 %) | 2,087.1 tps (3.9 %) | **0.84x** | PostgreSQL ahead |
| 3, 10,000 loans, unindexed, 1 core | `pk-user` | 30,487.8 qps (7.5 %) | 26,042 qps (119.9 %) | 1.17x | no claim |
| 3, unindexed | `loans-by-user` | 6,877.6 qps (1.9 %) | 4,924 qps (7.7 %) | **1.40x** | KDS ahead |
| 3, unindexed | `join-no-literal` | 4,115.2 qps (2.2 %) | 3,044 qps (8.6 %) | **1.35x** | KDS ahead |
| 3, unindexed | `exists-correlated` | 4,132.2 qps (2.2 %) | 1,484 qps (7.5 %) | **2.78x** | KDS ahead |
| 3, unindexed | `count-by-user` | 7,429.4 qps (2.8 %) | 5,198 qps (11.3 %) | **1.43x** | KDS ahead |
| 3, indexed | `pk-user` | 18,115.9 qps (24.1 %) | 22,727 qps (39.9 %) | 0.80x | no claim |
| 3, indexed | `loans-by-user` | 24,630.5 qps (3.6 %) | 29,674 qps (20.7 %) | 0.83x | no claim |
| 3, indexed | `join-no-literal` | 8,583.7 qps (3.2 %) | 2,923 qps (13.6 %) | **2.94x** | KDS ahead |
| 3, indexed | `exists-correlated` | 16,420.4 qps (1.6 %) | 11,287 qps (12.1 %) | **1.45x** | KDS ahead |
| 3, indexed | `count-by-user` | 31,055.9 qps (18.7 %) | 39,216 qps (3.1 %) | **0.79x** | PostgreSQL ahead (the difference, 21 %, is just over the larger spread) |
| 4, `off` arm | `board` (10,240 rows) | 248.5 µs (3.6 %) | 250.4 µs (2.3 %) | 1.01x | inside the spread |
| 4, `declared` arm | `board` | 109.4 µs (19.4 %) | 250.4 µs (2.3 %) | **2.29x** | KDS ahead (a declared Cabin against a sequential scan) |
| 4, `off` | `tape200` | 45.3 µs (5.3 %) | 33.4 µs (10.9 %) | **0.74x** | PostgreSQL ahead |
| 4, `declared` | `tape200` | 43.7 µs (2.5 %) | 33.4 µs (10.9 %) | **0.76x** | PostgreSQL ahead |
| 4, `off` | `tape1k` | 53.7 µs (0.9 %) | 48.9 µs (2.7 %) | **0.91x** | PostgreSQL ahead |
| 4, `declared` | `tape1k` | 46.5 µs (9.0 %) | 48.9 µs (2.7 %) | 1.05x | inside the spread |
| 4, `off` | `tape10k` | 155.4 µs (3.9 %) | 231.8 µs (4.7 %) | **1.49x** | KDS ahead |
| 4, `declared` | `tape10k` | 80.1 µs (23.0 %) | 231.8 µs (4.7 %) | 2.89x | no claim |
| 4, `off` | `pk` control | 31.9 µs (5.6 %) | 23.2 µs (9.2 %) | **0.73x** | PostgreSQL ahead |
| 4, `off` | `open` (autocommit inserts) | 180.5 µs (15.0 %) | 149.8 µs (66.2 %) | 0.83x | no claim |
| 4, `off` | `close` (`COUNT` / `SUM` / `GROUP BY`) | 1,011.4 µs (13.1 %) | 474.4 µs (1.2 %) | **0.47x** | PostgreSQL ahead |

PostgreSQL's scenario-4 latencies are 1,000,000 / qps of its results file's medians (3,994, 29,940, 20,450, 4,314, 43,103, 6,676 and 2,108 qps).

## What the matrix says

1. **PostgreSQL's fixed cost of a statement is about 8.7 µs lower.** The `pk` control is 23.2 µs against 31.9 µs, and the 200-row tape differs by 11.9 µs the same way. A statement that returns a few rows is decided by that term.
2. **KDS is cheaper per row examined, and the lead grows with the rows examined**: the tape reads 0.74x at 200 rows, 0.91x at 1,000 and 1.49x at 10,000, and scenario 3's unindexed equalities and correlated subquery read 1.35x to 2.78x. The per-row term in nanoseconds is not derived here (no KDS percentile or latency split survives).
3. **PostgreSQL's durable commit path is 16 % to 19 % faster**: one booker reads 0.84x and an autocommit insert 0.81x. PostgreSQL's commit is 138.2 µs, 29.0 % of a booking; the KDS commit's share on this host is not in the interim file.
4. **PostgreSQL's full-scan aggregate is 2.1x faster** (`close`, 0.47x), the largest gap in the matrix. Its cause is not attributed.

## What this file does not carry

- No measurement of its own, no percentile table and no wait breakdown: both are in the PostgreSQL results file for its side, and absent for the KDS side.
- No KDS number at `main`. A same-window, same-pinning KDS run of the matched cells (the PostgreSQL file's section 2 names them) is what would replace this file.
