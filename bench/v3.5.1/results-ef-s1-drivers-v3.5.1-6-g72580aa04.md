# EF-S1: the scenario drivers' timed paths made their twins', and the series break that makes

**One line.** With the drivers of scenarios 2, 3 and 4 timing what their PostgreSQL twins time - values decoded to strings for scenarios 2 and 3, rows counted for scenario 4 - a tuple key lookup through the values pair is 22.8 µs on KDS against 22.8 µs on PostgreSQL 18.7 (1.00x; it was 0.63x through the old pair at EF-S0), a booking's four reads are 7.4 µs to 11.7 µs cheaper each, three of its five writes and its commit inside their spreads and two writes 0.6 µs and 0.9 µs dearer through a write path neither driver changed, and ten of scenario 3's twelve read shapes are 10 µs to 47 µs cheaper (`exists-correlated` is inside its 17 % spread and `count-by-user` moves 2.1 µs, inside its 2.5 %). **The lookup's parity is on the wall clock only**: the KDS client still spends 11.5 µs of CPU a statement against PostgreSQL's 8.6 µs (spreads 4.3 % and 3.0 %), offset by a server that spends 7.6 µs against 10.6 µs. **Every figure these three drivers produce from here on is a new series**: no delta is taken against a scenario file measured before this stage. The engine is unchanged: the measured binary is EF-S0's.

Measured by CLA in the session that opened EF (no `ck-tester` agent was spawned).

## Stamp

| Field | Value |
|---|---|
| Version, commit | engine `v3.5.1-6-g72580aa04` (`git describe --tags` at `72580aa04`), the Release binary EF-S0 built and measured (same sha256, `binary.sha256`). **Old driver**: `tools/` of `72580aa04`. **New driver**: `tools/` as the commit that adds this file has them; the copies run 2 ran (pass D2, rounds 4 to 6) are hashed in the archive (`tools-old.sha256`, `tools-new.sha256`), and `kwp.py` and `ckdbs_cli.py` there are byte-identical to the committed ones. **Run 1's new copy (pass D, rounds 1 to 3: `fetch` before its cache) is neither hashed nor archived**; `tools-new.sha256` names run 2's. After the run the three drivers gained a `timed_client` entry in the JSON's `meta` and a comment each, outside every timer, and `parity_bench.py` gained a comment and a reordering of `host_pid`'s checks that reads `cmdline` only for a matching process, which changes no cell |
| Date and time | 2026-11-28; run 1 (pass D, overlap rounds 1 to 3) 01:54:25 to 01:56:15 UTC; run 2 (pass D2, overlap rounds 4 to 6) 01:57:40 to 01:59:13 UTC. **About 3.5 minutes**, inside the 20-minute stage budget |
| KDS | `cores = 1`, `buffer_pool_frames = 65536`, port 15791 (chosen), a fresh server and data file a cell, `taskset -c 0-7`; `group` for pass D, D2 and scenario 3, `strict` for scenario 2 |
| PostgreSQL | 18.7, a fresh `docker.io/library/postgres:18` container (run by `podman`) at its defaults, host network, on CPUs 0 to 7; passes D and D2 only |
| Driver | `taskset -c 8-23`. Overlap: old and new alternate, old first in odd rounds |
| Host and device | Intel Core Ultra 7 270K Plus, 24 CPUs (P-cores 0 to 7), `powersave`; `/dev/mapper/fedora-root`, xfs (`df -T` at run time, `device.txt`) |
| Quiet gate | before every cell: no build or test process and at most 8 % of all CPUs busy over 3 s; **every cell passed at once** (gate wait 0 s in all 26 `.precheck` files, 1-minute load 0.60 to 1.12) |
| Rule | spread = (max - min) / median over a cell's runs; a difference smaller than the larger spread is inside the spread; a spread over 20 % carries no claim |
| Verification | every scenario 2 run, old and new: 3,000 committed, 328 rejected for capacity, 74 for credit, 0 conflicted, 0 failed, and `--verify` 400 checks, 0 failures - **the same outcomes through both drivers**; every return code 0 |
| Comparator | this driver's own old path, in the same window (the overlap); and EF-S0's pass C for the parity shapes |
| Suite | not executed: no build input changed |

## What changed in the drivers

- **`kwp.Connection.fetch`** (`tools/kwp.py`): PARSE and the constant tail `query` sends, each value rendered to a string through renderers resolved once per row description - the description's bytes are the cache key. `execute`, which `send_command` uses, builds four frames and a portal name and decodes the description on every statement.
- **`ServerConnection.rows` and `.count`** (`tools/ckdbs_cli.py`): rows of strings, and the flat line `pg_wire`'s `send_command` answers.
- **Scenario 2**: its four booking reads and three manifest reads go through `rows_timed`, as the twin's do; writes, DDL and verification are untouched.
- **Scenario 3**: `Client.timed`, which every read shape uses, returns rows of strings, as the twin's does.
- **Scenario 4**: `Arm.timed` counts, as the twin's does; the replies its verification compares byte for byte still come through `send_command`, outside the timer.
- **Scenario 1 is not changed** (EF-Q7 (a)): its twin already decodes and renders as the KDS client does.

## The values pair on the parity shapes

### Pass D2: the final values path (`kwp.Connection.fetch`, row descriptions cached) against `pg_wire` `fetch` and a decode, 3 reps

| shape | rows a statement | KDS rate (spread) | PostgreSQL rate (spread) | KDS / PG | read | KDS mean | PG mean | KDS client CPU | PG client CPU |
|---|---|---|---|---|---|---|---|---|---|
| `pk-lookup` | 1 rows | 43,758.2 qps (9.9 %) | 43,608.8 qps (4.8 %) | **1.00x** | inside the spread | 22.8 µs | 22.8 µs | 11.5 µs | 8.6 µs |
| `bar-range` | 201 rows | 2,012.1 qps (1.6 %) | 2,088.3 qps (3.6 %) | **0.96x** | PostgreSQL ahead | 496.8 µs | 478.7 µs | 377.7 µs | 339.0 µs |
| `symbol-history` | 7,560 rows | 62.5 qps (3.0 %) | 69.9 qps (7.0 %) | **0.89x** | PostgreSQL ahead | 15,990.2 µs | 14,302.5 µs | 14,602.4 µs | 13,251.9 µs |
| `day-slice` | 8 rows | 1,494.6 qps (2.2 %) | 1,004.2 qps (9.8 %) | **1.49x** | KDS ahead | 668.8 µs | 995.4 µs | 57.7 µs | 87.2 µs |
| `agg-by-session` | 7,560 rows | 81.5 qps (3.6 %) | 93.0 qps (2.3 %) | **0.88x** | PostgreSQL ahead | 12,272.0 µs | 10,751.6 µs | 6,318.1 µs | 6,721.4 µs |

| shape | engine | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `pk-lookup` | KDS | 22.8 µs | 20.2 µs | 21.5 µs | 21.8 µs | 23.3 µs | 63.0 µs |
| `pk-lookup` | PostgreSQL | 22.8 µs | 20.4 µs | 21.8 µs | 22.0 µs | 22.8 µs | 30.3 µs |
| `bar-range` | KDS | 496.8 µs | 411.1 µs | 431.2 µs | 528.6 µs | 600.4 µs | 705.0 µs |
| `bar-range` | PostgreSQL | 478.7 µs | 370.0 µs | 392.5 µs | 506.5 µs | 564.7 µs | 604.4 µs |
| `symbol-history` | KDS | 15,990.2 µs | 15,491.2 µs | 15,535.4 µs | 15,745.0 µs | 17,280.3 µs | 17,280.3 µs |
| `symbol-history` | PostgreSQL | 14,302.5 µs | 13,465.3 µs | 13,773.5 µs | 14,067.6 µs | 16,594.6 µs | 16,594.6 µs |
| `day-slice` | KDS | 668.8 µs | 625.9 µs | 655.7 µs | 674.7 µs | 692.0 µs | 722.5 µs |
| `day-slice` | PostgreSQL | 995.4 µs | 879.7 µs | 945.4 µs | 980.3 µs | 1,196.4 µs | 1,425.4 µs |
| `agg-by-session` | KDS | 12,272.0 µs | 12,033.9 µs | 12,121.9 µs | 12,214.7 µs | 12,531.7 µs | 12,531.7 µs |
| `agg-by-session` | PostgreSQL | 10,751.6 µs | 10,380.1 µs | 10,536.4 µs | 10,637.6 µs | 11,254.4 µs | 11,254.4 µs |

### Pass D: the same before the row description was cached, 3 reps

| shape | rows a statement | KDS rate (spread) | PostgreSQL rate (spread) | KDS / PG | read | KDS mean | PG mean | KDS client CPU | PG client CPU |
|---|---|---|---|---|---|---|---|---|---|
| `pk-lookup` | 1 rows | 30,968.6 qps (3.9 %) | 42,567.4 qps (16.9 %) | **0.73x** | PostgreSQL ahead | 32.2 µs | 23.4 µs | 21.3 µs | 9.0 µs |
| `bar-range` | 201 rows | 1,955.1 qps (5.2 %) | 2,083.2 qps (11.2 %) | **0.94x** | inside the spread | 511.3 µs | 479.9 µs | 378.3 µs | 342.9 µs |
| `symbol-history` | 7,560 rows | 61.4 qps (4.2 %) | 71.8 qps (5.9 %) | **0.86x** | PostgreSQL ahead | 16,292.1 µs | 13,932.1 µs | 14,779.1 µs | 12,938.8 µs |
| `day-slice` | 8 rows | 1,462.4 qps (1.5 %) | 1,004.2 qps (3.5 %) | **1.46x** | KDS ahead | 683.5 µs | 995.4 µs | 69.9 µs | 81.8 µs |
| `agg-by-session` | 7,560 rows | 83.5 qps (1.2 %) | 91.4 qps (1.5 %) | **0.91x** | PostgreSQL ahead | 11,982.0 µs | 10,940.0 µs | 6,241.2 µs | 7,003.2 µs |

The lookup's client CPU is 11.5 µs through the final path against 21.3 µs before the cache and 26.1 µs through `send_command` (EF-S0's pass C); PostgreSQL's is 8.6 µs to 9.0 µs. **What is left between the two clients is a statement term of about 3 µs of client CPU (11.5 µs against 8.6 µs, not attributed; through the two counting clients it was 8.8 µs against 7.8 µs at EF-S0's pass A, another window) and a row term, the wire's**: a KDS value arrives typed and is rendered by the client (377.7 µs of client CPU for 201 rows of 8 columns), a PostgreSQL value arrives as text (339.0 µs), so `bar-range` and `symbol-history` read 0.96x and 0.89x through this pair where two counting clients read them 1.74x and 1.50x (EF-S0's pass A).

## The series break, on the scenario cells

Rounds 4 to 6 are the final driver; rounds 1 to 3 ran the values path before its row description was cached and are shown for the record.

### Overlap, scenario 2: one booker, one core, `strict`, 3,000 bookings

| phase | ops a run | old driver, mean (spread) | new driver, mean (spread) | new less old | read | new p0 | new p25 | new p50 | new p95 | new p99 | rounds 1-3: old | rounds 1-3: new before the cache |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `cargo-lookup` | 3,402 ops | 35.5 µs (2.5 %) | 26.0 µs (3.1 %) | -9.5 µs | cheaper | 21.1 µs | 24.6 µs | 25.1 µs | 27.5 µs | 37.2 µs | 38.5 µs (9.4 %) | 32.9 µs (10.0 %) |
| `credit-lookup` | 3,402 ops | 30.3 µs (4.0 %) | 22.9 µs (2.6 %) | -7.4 µs | cheaper | 19.3 µs | 21.6 µs | 21.9 µs | 23.4 µs | 29.3 µs | 33.0 µs (8.8 %) | 27.0 µs (11.1 %) |
| `capacity-read` | 3,402 ops | 29.4 µs (2.4 %) | 21.9 µs (0.5 %) | -7.5 µs | cheaper | 19.2 µs | 20.9 µs | 21.2 µs | 22.4 µs | 30.7 µs | 31.2 µs (6.7 %) | 26.0 µs (9.6 %) |
| `recipe-read` | 3,402 ops | 50.7 µs (1.8 %) | 39.0 µs (1.3 %) | -11.7 µs | cheaper | 33.6 µs | 37.1 µs | 38.1 µs | 40.7 µs | 50.7 µs | 53.0 µs (6.0 %) | 45.9 µs (6.5 %) |
| `freight-insert` | 3,000 ops | 24.5 µs (2.4 %) | 24.9 µs (3.6 %) | +0.4 µs | inside the spread | 21.8 µs | 23.3 µs | 23.7 µs | 25.6 µs | 36.8 µs | 25.7 µs (8.9 %) | 25.4 µs (9.4 %) |
| `charge-insert` | 16,907 ops | 20.7 µs (1.9 %) | 21.1 µs (2.4 %) | +0.4 µs | inside the spread | 18.6 µs | 20.2 µs | 20.5 µs | 21.8 µs | 31.2 µs | 21.0 µs (2.9 %) | 21.2 µs (5.2 %) |
| `operation-update` | 3,000 ops | 23.0 µs (1.7 %) | 23.6 µs (2.5 %) | +0.6 µs | dearer | 21.0 µs | 22.6 µs | 22.9 µs | 24.4 µs | 30.6 µs | 23.6 µs (3.4 %) | 23.8 µs (3.4 %) |
| `org-update` | 3,000 ops | 21.6 µs (2.3 %) | 22.5 µs (1.3 %) | +0.9 µs | dearer | 19.9 µs | 21.4 µs | 21.7 µs | 22.8 µs | 26.8 µs | 21.9 µs (2.3 %) | 22.3 µs (2.2 %) |
| `commit` | 3,000 ops | 135.9 µs (9.6 %) | 135.8 µs (13.0 %) | -0.1 µs | inside the spread | 121.1 µs | 128.9 µs | 129.6 µs | 156.5 µs | 169.4 µs | 304.2 µs (55.6 %) | 212.7 µs (80.4 %) |
| `manifest-scan` | 40 ops | 123.9 µs (24.0 %) | 100.0 µs (36.9 %) | -23.9 µs | no claim | 49.0 µs | 53.1 µs | 59.6 µs | 245.1 µs | 422.4 µs | 242.4 µs (87.6 %) | 106.7 µs (31.4 %) |
| `voyage-rollup` | 40 ops | 94.7 µs (40.4 %) | 93.3 µs (27.3 %) | -1.4 µs | no claim | 44.9 µs | 49.7 µs | 57.1 µs | 186.5 µs | 381.5 µs | 92.8 µs (29.7 %) | 82.8 µs (42.8 %) |
| `customer-statement` | 20 ops | 332.9 µs (21.1 %) | 383.2 µs (11.9 %) | +50.3 µs | no claim | 75.1 µs | 114.8 µs | 197.6 µs | 726.2 µs | 769.2 µs | 414.7 µs (24.0 %) | 377.9 µs (25.8 %) |

Old driver: booking rate 1,829.8 a second (runs 1,772.4, 1,829.8, 1,841.2; spread 3.8 %)
New driver: booking rate 1,978.6 a second (runs 1,981.0, 1,897.1, 1,978.6; spread 4.2 %)

### Overlap, scenario 3: 10,000 loans, unindexed, one core, `group`

| phase | ops a run | old driver, mean (spread) | new driver, mean (spread) | new less old | read | new p0 | new p25 | new p50 | new p95 | new p99 | rounds 1-3: old | rounds 1-3: new before the cache |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `pk-user` | 200 ops | 35.2 µs (13.9 %) | 22.8 µs (12.7 %) | -12.4 µs | cheaper | 19.9 µs | 20.9 µs | 21.6 µs | 23.2 µs | 26.9 µs | 37.2 µs (54.0 %) | 39.5 µs (34.7 %) |
| `loans-by-user` | 200 ops | 146.7 µs (5.9 %) | 135.7 µs (1.5 %) | -11.0 µs | cheaper | 129.3 µs | 133.1 µs | 135.2 µs | 141.3 µs | 146.4 µs | 146.8 µs (7.4 %) | 147.0 µs (3.9 %) |
| `loans-by-book` | 200 ops | 152.6 µs (7.6 %) | 134.0 µs (0.5 %) | -18.6 µs | cheaper | 126.2 µs | 131.7 µs | 133.6 µs | 139.4 µs | 142.4 µs | 143.7 µs (2.5 %) | 143.2 µs (11.0 %) |
| `resv-by-user` | 200 ops | 86.4 µs (3.0 %) | 76.4 µs (0.3 %) | -10.0 µs | cheaper | 70.9 µs | 75.0 µs | 76.1 µs | 80.0 µs | 81.8 µs | 85.6 µs (3.2 %) | 81.2 µs (14.8 %) |
| `books-by-author` | 200 ops | 59.0 µs (12.0 %) | 48.4 µs (1.0 %) | -10.6 µs | cheaper | 42.2 µs | 46.7 µs | 48.0 µs | 53.0 µs | 55.8 µs | 57.9 µs (8.5 %) | 52.9 µs (5.1 %) |
| `books-by-genre` | 200 ops | 167.9 µs (1.5 %) | 149.1 µs (0.6 %) | -18.8 µs | cheaper | 134.7 µs | 144.1 µs | 148.6 µs | 158.4 µs | 161.7 µs | 168.8 µs (3.4 %) | 151.7 µs (1.5 %) |
| `loans-by-daterange` | 200 ops | 660.4 µs (3.4 %) | 613.1 µs (0.6 %) | -47.3 µs | cheaper | 213.4 µs | 545.8 µs | 628.3 µs | 726.7 µs | 742.7 µs | 663.3 µs (1.7 %) | 611.2 µs (3.3 %) |
| `overdue` | 200 ops | 517.4 µs (8.3 %) | 470.7 µs (3.4 %) | -46.7 µs | cheaper | 172.8 µs | 423.1 µs | 442.8 µs | 612.2 µs | 651.9 µs | 513.3 µs (4.1 %) | 478.2 µs (7.9 %) |
| `join-loan-user` | 200 ops | 152.1 µs (3.5 %) | 139.3 µs (6.2 %) | -12.8 µs | cheaper | 131.4 µs | 135.4 µs | 137.2 µs | 146.3 µs | 168.2 µs | 150.2 µs (17.2 %) | 146.9 µs (2.4 %) |
| `join-no-literal` | 200 ops | 240.6 µs (3.4 %) | 228.5 µs (2.3 %) | -12.1 µs | cheaper | 222.7 µs | 225.6 µs | 226.5 µs | 234.7 µs | 269.7 µs | 243.1 µs (2.8 %) | 230.6 µs (0.8 %) |
| `exists-correlated` | 200 ops | 279.8 µs (17.0 %) | 279.5 µs (15.3 %) | -0.3 µs | inside the spread | 267.9 µs | 274.0 µs | 278.0 µs | 288.8 µs | 317.5 µs | 264.5 µs (7.6 %) | 255.1 µs (5.6 %) |
| `count-by-user` | 200 ops | 133.8 µs (2.5 %) | 131.7 µs (1.7 %) | -2.1 µs | inside the spread | 128.2 µs | 130.0 µs | 131.2 µs | 135.9 µs | 137.9 µs | 141.0 µs (5.5 %) | 134.1 µs (3.2 %) |


## What the run teaches

1. **The statement term is cut, not gone, and the row term is not cut.** A lookup's client CPU over PostgreSQL's is 2.9 µs (11.5 µs against 8.6 µs) where it was 17.5 µs through `send_command` (EF-S0's pass C, 26.1 µs against 8.6 µs, another window). A point read through the values pair is at parity with PostgreSQL's on the wall clock, with the KDS client still about 3 µs of CPU a statement dearer and its server about 3 µs cheaper; a read that returns hundreds of rows still pays a Python render per value that PostgreSQL's text protocol does not ask of its client.
2. **A booking's writes and its commit move by under 1 µs**: `freight-insert`, `charge-insert` and `commit` inside their spreads, `operation-update` (+0.6 µs, 2.6 % against a 2.5 % spread) and `org-update` (+0.9 µs, 4.2 % against 2.3 %) dearer by the rule through a write path that is the same code in both drivers - not attributed. The booking rate rises from 1,829.8 to 1,978.6 a second.
3. **`exists-correlated` and `count-by-user` do not move by the rule**: `exists-correlated` (up to 20 rows) is inside its 17.0 % and 15.3 % spreads, and `count-by-user` (one row) is 2.1 µs cheaper inside its 2.5 %. Neither is explained by its row count: `pk-user` also returns one row and is 12.4 µs cheaper.
4. **The row description cost about 10 µs a statement of client CPU on the lookup** (21.3 µs in pass D against 11.5 µs in pass D2; 32.2 µs against 22.8 µs on the wall clock). In scenario 2 the cache is worth less: its reads were 5 µs to 7 µs cheaper than the old driver before the cache and 7 µs to 12 µs after, 1.4 µs to 4.6 µs more, each saving read in its own window.

## What is cut, absent or unverified

- **Scenario 4 has no overlap cell**: its change is `count`, which pass A of EF-S0 measured (16.6 µs a statement, 1.4 µs to 1.7 µs a row against `send_command`), and its cells are EF-S2's.
- **Writes still go through `send_command`, and are not their twins' mode**: a statement that returns no rows builds no row objects, but `send_command` reaches it through `kwp.Connection.execute` (a BIND built and a portal named per statement, a second read loop to `S_READY`) where the twin's writes other than `RETURNING` go through `pg_wire`'s `query`, which counts; the twin's `RETURNING` inserts (scenario 2's `freight-insert` and six load phases, scenario 3's users and books) decode one value through `fetch`, which the KDS side reads from its tag. The overlap cannot show the difference - both KDS drivers time the same write code - and the difference is not measured on this host. `[measured]` off the bench host, on a mock socket and no server: `send_command` of a write 3.8 µs of Python against `count`'s 1.9 µs (`count` answers the same tag).
- **Three runs a cell.** A difference under about 3 % of a phase is not resolved.
- **`load` and `pk-user`** (scenario 3's first phases) have spreads over 20 % in rounds 1 to 3 and carry no claim there.

## After this run

The review of this stage found the timed writes still on `send_command` where the twins count, and the drivers of scenarios 2 and 3 now time their writes and DDL through `ServerConnection.count` (the commit after `189c901fa`). **Nothing in this file measures that form**: every "new driver" write above went through `send_command`. EF-S2's cells run it.

## Archive

`bench/v3.5.1/archive/ef-s1-v3.5.1-6-g72580aa04/`: `json/` (the two parity passes and the 24 overlap cells), `logs/` (each cell's stdout, return code, precheck and server configuration), `run.sh`, `run2.sh`, both watchdogs and their log, the timeline, the device, the binary's hash and the hashes of the driver copies that ran.
