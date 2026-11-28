# PostgreSQL 18.7 against KDS on one host, scenarios 1 to 4: a comparison of two existing measurements

**Superseded on 2026-11-28 by `bench/v3.5.1/results-ef-s2-matched-v3.5.1-6-g72580aa04.md`**, which measured the matched cells on `main` beside PostgreSQL in one window through equal clients: no short read and no booking is behind PostgreSQL there, and what this file's matrix read as behind was the KDS scenario client. The matrix below stands as what the old drivers read and is not to be quoted as the engine's.

**One line.** On this host, with both servers pinned to the P-cores, the scenario drivers read KDS at `6c98f7c32` ahead of PostgreSQL 18.7 where a statement examines many rows and discards them (1.35x to 2.94x) and behind it on a short read (0.73x to 0.76x), a one-booker booking (0.84x) and a full-scan aggregate (0.47x). **Only the last of the three "behind" readings is the engine's on the evidence in the tree, and the matrix understates KDS wherever a statement of scenarios 2 to 4 returns rows**: the KDS drivers time a client that decodes and renders every field, which costs 16.6 µs a statement on this host, and the twins of scenarios 2 to 4 time clients that do less - 0.8 µs a statement where they decode (scenarios 2 and 3), none where they count (scenario 4); scenario 1's twin decodes and renders too, and its pair is not measured (bound 1 below); the commit's sync is at parity (135.5 µs against 138.2 µs). **Nothing was measured for this file**: it sets two results files side by side, and the KDS side is an engine 28 first-parent commits older than `main`.

**Corrected 2026-11-28**, the day it was written: the first version read "the fixed cost of a short statement" and "the durable commit path" as PostgreSQL's leads. Both readings were CLA's and neither is supported: see "What the matrix says".

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

1. **The two sides are timed through unequal clients.** Every KDS scenario driver times `ckdbs_cli.send_command` (`tools/scenario1_backtest.py:400`, `scenario2_freight.py:298`, `scenario3_library.py:232`, `scenario4_cabinopt_days.py:250`), which decodes every field into a Python object and renders it to text (`tools/ckdbs_cli.py:191-228`); of the PostgreSQL twins only scenario 4's times a client that counts rows and decodes no field (`pg_wire`'s `send_command`, which is `query`: `tools/pg_scenario4_cabinopt_days.py:56-70`, `tools/pg_wire.py:224-262`, `:313-322`). Scenario 2's and 3's twins time their reads, and every `INSERT ... RETURNING id` (scenario 2's `freight-insert` among them), through `fetch` and a decode of every value to a string, without rendering (`tools/pg_scenario2_freight.py:140-145`, `:164-168`, `:196-200`, `tools/pg_scenario3_library.py:108-121`, `:186-190`), and their other writes through `query`; scenario 1's decodes and renders as the KDS client does (`tools/pg_scenario1_backtest.py:78-83`, `:266-299`). **So the clients are unequal in scenarios 2 to 4, by a different amount in each, and scenario 1's pair is not measured** (corrected a second time on 2026-11-28, at EF-S0; this bound first said every twin counts, then scenarios 2 to 4). **Measured on this host at `72580aa04`** (`bench/v3.5.1/results-ef-s0-premise-v3.5.1-6-g72580aa04.md`): the KDS client adds 16.6 µs a statement and 1.4 µs to 1.7 µs a returned 8-column row over a counting client, PostgreSQL's decoding client 0.8 µs and 0.7 µs to 0.8 µs; on counting clients the tuple key lookup reads 1.10x of PostgreSQL's rate and no shape but the global fold (`agg-global`, 0.71x) is behind. `bench/v3.2.0/results-pg-parity-v3.2.0-171-ge81ac7cd.md` measured the difference on its host, at `e81ac7cd`: **29.0 µs against 5.0 µs of client CPU per point statement and 6.3 µs against 2.5 µs per returned 9-column row**, and on equal clients the tuple key lookup read 1.30x and `bar-range` 1.42x of PostgreSQL's rate. Every KDS figure below for a statement that returns rows carries it; a write, which returns none, does not, and scenario 2's `freight-insert` carries PostgreSQL's decode of its one returned id instead.
2. **The KDS engine is not `main`.** `6c98f7c32` is 28 first-parent commits and 75 files of `src/` and `include/` behind `7556a5596`; EA (a prepared statement parsed once) and EB (the key forms in the executor) landed in between. Whether any of them moved a shape below is unverified.
3. **The two sides are about 24 hours apart**, on a host that runs other projects' containers throughout.
4. **Scenario 4 is not the same procedure on the two sides**: KDS runs three paced days and the figure is the median of nine day-runs of the `off` arm (no Cabin) or the `declared` arm; PostgreSQL runs day 1 once per run, unpaced, with no index on `symbol`.
5. **Scenario 1's KDS cells are `group` durability** and PostgreSQL's are `synchronous_commit = on`. The read shapes are unaffected; `result-insert` may carry the difference (unverified).
6. **The KDS scenario-3 A/B's size is `[assumed]` 10,000 loans**: the interim file does not state it, the `9f8f8b354` pinned A/B it follows was 10,000 loans, and the raw JSON of the `6c98f7c32` run is no longer on the host.
7. **No percentile is compared.** The KDS side survives as medians and spreads only.

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

1. **A short read is 8.7 µs slower through these drivers, and the tree does not support reading that as the engine's.** The `pk` control is 23.2 µs against 31.9 µs and the 200-row tape differs by 11.9 µs the same way. Bound 1 puts more than that difference in the KDS client on the parity file's host, where the same lookup read 1.30x on equal clients; a text point `SELECT` is 6.4 µs of server CPU at `481d101aa` (`bench/v3.4.17/results-ea-close-v3.4.17-205-g2c5c195f7.md`). The writes agree: they return no rows and show no such term (item 3's table).
2. **KDS is cheaper per row examined, and the lead grows with the rows examined**: the tape reads 0.74x at 200 rows, 0.91x at 1,000 and 1.49x at 10,000, and scenario 3's unindexed equalities and correlated subquery read 1.35x to 2.78x. Bound 1 makes each of these a lower bound.
3. **The commit is at parity; the booking's 16 % is in its four reads.** From the archived JSON (KDS: the pinned A/B's new arm at `9f8f8b354`, five runs, `bench/v3.5.0/archive/scenario2-v3.4.17-159-g9f8f8b354/json/pin-ab-new*-b1-s2-c1-s.json`, the engine the interim file reads as `6c98f7c32`'s; PostgreSQL: the control run's three), mean per statement:

   | statement | KDS | PostgreSQL |
   |---|---|---|
   | `commit` | 135.5 µs | 138.2 µs |
   | `cargo-lookup` | 35.4 µs | 25.1 µs |
   | `credit-lookup` | 30.2 µs | 22.6 µs |
   | `capacity-read` | 29.5 µs | 21.9 µs |
   | `recipe-read` | 67.3 µs | 38.8 µs |
   | `freight-insert` | 24.4 µs | 29.6 µs |
   | `charge-insert` (5.6 a booking) | 21.0 µs | 19.6 µs |
   | `operation-update` / `org-update` | 23.3 µs / 21.8 µs | 23.2 µs / 20.9 µs |

   The reads are bound 1's statements. `recipe-read` is also a different access on the two sides: an index on PostgreSQL, a walk of 93 rows on KDS, by the drivers' design. **`result-insert`'s 0.81x is not attributed**: scenario 2's autocommit loads at `strict` read 136.0 µs to 152.4 µs on KDS against 146.7 µs to 153.6 µs on PostgreSQL, so an autocommit insert is not slower as such; scenario 1's cell is `group` and its statement is not examined here.
4. **The full-scan aggregate is behind, and part of it is the engine's.** `close` is four statements, three rounds: `COUNT(*)` and `SUM(qty)` over the board, `COUNT(*)` over the 10,000-row tape, and `symbol, COUNT(*) ... GROUP BY symbol`, which returns 1,000 rows (`tools/scenario4_cabinopt_days.py:434-443`), so bound 1 is in the 0.47x and its share is not measured. On equal clients at `e81ac7cd` the parity file read `agg-global` at 0.61x of PostgreSQL's rate on 1.63x its server CPU, about 95 ns against 59 ns per row folded; that is the engine, and nothing since is known to have moved it.

Items 1, 3 and 4 are taken up by the work orders EF, EG and EH (`instructions/v3.2.0/`, written 2026-11-28, none opened), each of which re-measures its premise on `main` on equal terms before anything is built.

## What this file does not carry

- No measurement of its own, no percentile table and no wait breakdown: both are in the PostgreSQL results file for its side, and absent for the KDS side.
- No KDS number at `main`, and none through an equal client. A same-window, same-pinning KDS run of the matched cells (the PostgreSQL file's section 2 names them), **timed through `kwp.Connection.query`** (`tools/kwp.py:654`, the counterpart of `pg_wire`'s `query`), is what would replace this file.
