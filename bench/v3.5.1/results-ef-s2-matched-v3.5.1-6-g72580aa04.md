# EF-S2: scenarios 1 to 4 on `main` beside PostgreSQL 18.7 in one window, each side through its twin's client

**One line.** At `72580aa04`, with each KDS driver timing what its PostgreSQL twin times, **neither read EF-Q1's gate names is behind PostgreSQL**: scenario 4's `pk` control is 20.5 µs against 21.2 µs, and the 200-row tape 26.2 µs to 27.1 µs against 32.8 µs to 51.9 µs a round (no claim: PostgreSQL's rounds spread 49.4 %); a booking's three point reads are 21.8 µs to 26.1 µs against 21.9 µs to 25.4 µs, and scenario 1's lookup is 24.9 µs to 27.2 µs against 28.8 µs. **Two other short reads are behind by more than the larger spread, by under 1 µs each**: `cargo-lookup` at `strict` (26.1 µs against 25.4 µs, 0.97x) and `books-by-genre` over 200 unindexed loans (22.7 µs against 21.9 µs, 0.96x), each less than the 2.9 µs of client CPU a decoded read still costs the KDS side. One durable booker is 2,077.5 bookings a second against 2,070.2, with every write and the commit ahead. Scenario 3 is ahead by a geometric mean of 1.09x to 1.42x over its six cells. **EF-Q1's gate reads "the engine stage does not open"**, and EF closes here. What is behind PostgreSQL on these clients is, besides those two, chiefly three things: folds over a whole relation (`close` 0.70x on the one arm that resolves, `agg-distinct` 0.73x to 0.83x from 1,008 bars up, `agg-global` 0.81x to 0.88x where it resolves), scenario 1's autocommit insert (`result-insert` 0.81x at `strict`), and a read that returns thousands of rows through the rendering pair (`read-symbol-history` 0.88x). The first is EH's, the second EG's, and the third is the per-row render the KDS client does and PostgreSQL's does not. Two more rows read PostgreSQL ahead with no cause named here: `books-by-genre` over 10,000 indexed loans (0.88x; KDS's unindexed cell of the same statement is 148.5 µs against its indexed 164.6 µs) and `read-bar-range` at 1,008 bars (0.96x); `manifest-scan` differs by design (item 6).

This file replaces `bench/v3.5.0/comparison-pg-kds-scenarios-v3.4.17-166-g6c98f7c32.md`, which set two measurements a day and an engine apart side by side. **It opens the drivers' new series**: no delta is taken against a scenario file measured before EF-S1.

Measured by CLA in the session that opened EF (no `ck-tester` agent was spawned).

## Stamp

| Field | Value |
|---|---|
| Version, commit | engine `v3.5.1-6-g72580aa04` (`git describe --tags` at `72580aa04`), the Release binary EF-S0 built from `git archive` and measured (one sha256 in every run's `binary.sha256`) |
| Drivers | run a: `tools/` of `2ba797526`; run b: `tools/` of `3321c61fb` (timed writes through the counting client); run c: `tools/` as the commit that adds this file has them (scenario 1 through `rendered`). Each run's copies are hashed in its `tools.sha256`. **Which run a cell is taken from**: scenario 4 from run a, but round 2's KDS cell from run b; scenarios 2 and 3 from run b; scenario 1 from run c. Run a's scenario 1, 2 and 3 cells stay in the archive and are not used |
| Date and time | 2026-11-28. Run a 02:00:25 to 02:23:20 UTC (78 cells); run b 02:23:35 to 02:32:07 UTC (46 cells); run c 02:33:25 to 02:38:11 UTC (27 cells). **About 36 minutes of measurement**: over the 20-minute stage budget, which EF-Q5 (a) lifted for this stage by adoption on the operator's standing go-ahead, not by the operator's own word for this measurement |
| KDS | `cores = 1`, `buffer_pool_frames = 65536`, a fresh server and data file a cell, `taskset -c 0-7`. Scenario 2: `strict` and `group`, one booker. Scenario 3: `group`. Scenario 1: `group` and `strict` at 60,480 bars, `group` for the size sweep. Scenario 4: three servers (`off`, `on`, `declared`), `group`, `decay_half_life = 5`, `cabin_optimizer_snapshot_interval_ms = 500`. Every cell's configuration is in the archive |
| PostgreSQL | 18.7 (Debian 18.7-1.pgdg13+5), one fresh `docker.io/library/postgres:18` container a run (run by `podman`) at its defaults - port 15434 and the listen address only - host network, every server process on CPUs 0 to 7 (`pg-affinity.txt`); `synchronous_commit = on` |
| Procedure | three rounds; in a round each KDS cell is followed by its PostgreSQL twin in odd rounds and preceded by it in even ones; drivers `taskset -c 8-23`; the arguments are those of the PostgreSQL control run (`bench/v3.5.0/results-pg-control-scenarios-v3.4.17-202-g481d101aa.md`, section 2) |
| Client modes | scenario 4: every timed statement counted on both sides. Scenarios 2 and 3: reads decoded to strings on both sides, writes and DDL counted on both; the twins' `RETURNING` inserts decode one value where KDS reads its tag. Scenario 1: every statement decoded and rendered on both sides. Each KDS JSON of scenarios 2 to 4 carries its mode in `meta.timed_client` |
| Host and device | Intel Core Ultra 7 270K Plus, 24 CPUs (P-cores 0 to 7), `powersave`; `/dev/mapper/fedora-root`, xfs (`df -T` at each run's start, `device.txt`) |
| Quiet gate | before every cell: no build or test process and at most 8 % of all CPUs busy over 3 s. **Every one of the 151 cells passed at once** (gate wait 0 s); 1-minute load 0.21 to 1.25. Another project's containers and `kds_server` run on this host throughout |
| What CLA did on the host during the runs | **a driver smoke test on the P-cores inside run a's round-2 KDS scenario 4 cell**, which is why that cell is taken from run b; and short unpinned analysis scripts during runs a and b |
| Rule | rate = 1,000,000 / mean latency; spread = (max - min) / median over the three rounds' means; a difference smaller than the larger spread is inside the spread; a spread over 20 % carries no claim |
| Verification | every return code 0; 0 errors over every phase of every archived cell; scenario 2 `--verify` 400 checks, 0 failures in all nine runs of both engines, the same outcomes in all six KDS runs; scenario 3 `verify_problems` empty in all 18 KDS cells and `--assert-index-reads` passing in the indexed ones; scenario 1 `--verify` OK in all 27 runs of both engines; scenario 4's byte comparison across its arms PASS on each of its three days in all four KDS runs |
| Comparator | none in the drivers' own series: this file opens it. EF-S1's overlap (`results-ef-s1-drivers-v3.5.1-6-g72580aa04.md`) states what the old drivers read on the same engine and host |
| Suite | not executed: no build input changed |

## EF-Q1's gate: the short reads

| cell | ops a run | KDS mean (spread) | PostgreSQL mean (spread) | KDS rate | PG rate | KDS / PG | read |
|---|---|---|---|---|---|---|---|
| scenario 4, `pk`, `off` | 240 ops | 20.5 µs (2.4 %) | 21.2 µs (11.8 %) | 48,780 qps | 47,170 qps | **1.03x** | inside the spread |
| scenario 4, `tape200`, `off` | 396 ops | 26.6 µs (3.4 %) | 38.7 µs (49.4 %) | 37,594 qps | 25,840 qps | **1.45x** | no claim |
| scenario 2 `strict`, `cargo-lookup` | 3,402 ops | 26.1 µs (0.8 %) | 25.4 µs (2.0 %) | 38,314 qps | 39,370 qps | **0.97x** | PostgreSQL ahead |
| scenario 2 `strict`, `credit-lookup` | 3,402 ops | 22.6 µs (2.2 %) | 22.8 µs (1.8 %) | 44,248 qps | 43,860 qps | **1.01x** | inside the spread |
| scenario 2 `strict`, `capacity-read` | 3,402 ops | 21.8 µs (1.4 %) | 21.9 µs (0.9 %) | 45,872 qps | 45,662 qps | **1.00x** | inside the spread |
| scenario 3, 200 unindexed, `pk-user` | 200 ops | 27.4 µs (3.6 %) | 27.6 µs (35.1 %) | 36,496 qps | 36,232 qps | **1.01x** | no claim |
| scenario 3, 200 indexed, `pk-user` | 200 ops | 24.1 µs (11.2 %) | 27.8 µs (8.6 %) | 41,494 qps | 35,971 qps | **1.15x** | KDS ahead |
| scenario 3, 1,000 unindexed, `pk-user` | 200 ops | 26.3 µs (27.8 %) | 31.2 µs (52.2 %) | 38,023 qps | 32,051 qps | **1.19x** | no claim |
| scenario 3, 1,000 indexed, `pk-user` | 200 ops | 28.6 µs (19.6 %) | 31.7 µs (56.5 %) | 34,965 qps | 31,546 qps | **1.11x** | no claim |
| scenario 3, 10,000 unindexed, `pk-user` | 200 ops | 31.0 µs (23.5 %) | 40.9 µs (11.5 %) | 32,258 qps | 24,450 qps | **1.32x** | no claim |
| scenario 3, 10,000 indexed, `pk-user` | 200 ops | 44.6 µs (61.7 %) | 44.4 µs (17.1 %) | 22,422 qps | 22,523 qps | **1.00x** | no claim |
| scenario 1, 60,480 bars `group`, `read-bar-lookup` | 200 ops | 27.2 µs (10.7 %) | 28.8 µs (9.7 %) | 36,765 qps | 34,722 qps | **1.06x** | inside the spread |
| scenario 1, 60,480 bars `strict`, `read-bar-lookup` | 200 ops | 24.9 µs (5.6 %) | 28.8 µs (9.7 %) | 40,161 qps | 34,722 qps | **1.16x** | KDS ahead |
| scenario 1, 252 bars, `read-bar-lookup` | 200 ops | 24.0 µs (6.7 %) | 28.6 µs (39.2 %) | 41,667 qps | 34,965 qps | **1.19x** | no claim |
| scenario 1, 1,008 bars, `read-bar-lookup` | 200 ops | 24.3 µs (1.6 %) | 24.0 µs (4.6 %) | 41,152 qps | 41,667 qps | **0.99x** | inside the spread |
| scenario 1, 10,080 bars, `read-bar-lookup` | 200 ops | 23.6 µs (3.8 %) | 23.9 µs (0.8 %) | 42,373 qps | 41,841 qps | **1.01x** | inside the spread |

No row reads "PostgreSQL ahead" but `cargo-lookup` at `strict`, 0.7 µs behind on a 2.0 % spread, beside a `group` cell of the same statement inside its spread; EF-S1's review put about 2.9 µs of client CPU a statement on the KDS side of a decoded read. The 200-row tape carries no claim (PostgreSQL's three runs spread 49.4 %), and KDS is the faster in every round of it (26.2 µs to 27.1 µs against 32.8 µs to 51.9 µs), as it is of `pk` (20.0 µs to 20.5 µs against 21.2 µs to 23.7 µs). **The gate's condition - `pk` or the 200-row tape behind PostgreSQL by more than the larger spread (EF-Q1 (a)) - is not met, and EF-S3 does not open.** Read on every short read instead, it would be met by `cargo-lookup` at `strict`, and outside this table by `books-by-genre` over 200 unindexed loans (0.96x on a 3.2 % spread), each by under 1 µs.

## Scenario 4: day 1, every timed statement through a client that counts

KDS: day 1 of three paced days, each arm on its own server; PostgreSQL: its one unpaced day, no index on `symbol`.

| group, KDS arm | ops a run | KDS mean (spread) | PostgreSQL mean (spread) | KDS rate | PG rate | KDS / PG | read |
|---|---|---|---|---|---|---|---|
| `pk`, `off` | 240 ops | 20.5 µs (2.4 %) | 21.2 µs (11.8 %) | 48,780 qps | 47,170 qps | **1.03x** | inside the spread |
| `pk`, `declared` | 240 ops | 19.4 µs (4.1 %) | 21.2 µs (11.8 %) | 51,546 qps | 47,170 qps | **1.09x** | inside the spread |
| `pk`, `on` | 240 ops | 19.5 µs (13.8 %) | 21.2 µs (11.8 %) | 51,282 qps | 47,170 qps | **1.09x** | inside the spread |
| `tape200`, `off` | 396 ops | 26.6 µs (3.4 %) | 38.7 µs (49.4 %) | 37,594 qps | 25,840 qps | **1.45x** | no claim |
| `tape200`, `declared` | 396 ops | 24.0 µs (3.7 %) | 38.7 µs (49.4 %) | 41,667 qps | 25,840 qps | **1.61x** | no claim |
| `tape200`, `on` | 396 ops | 27.3 µs (6.2 %) | 38.7 µs (49.4 %) | 36,630 qps | 25,840 qps | **1.42x** | no claim |
| `tape1k`, `off` | 396 ops | 34.8 µs (2.3 %) | 49.8 µs (4.2 %) | 28,736 qps | 20,080 qps | **1.43x** | KDS ahead |
| `tape1k`, `declared` | 396 ops | 27.4 µs (10.2 %) | 49.8 µs (4.2 %) | 36,496 qps | 20,080 qps | **1.82x** | KDS ahead |
| `tape1k`, `on` | 396 ops | 34.8 µs (10.1 %) | 49.8 µs (4.2 %) | 28,736 qps | 20,080 qps | **1.43x** | KDS ahead |
| `tape10k`, `off` | 396 ops | 132.6 µs (0.9 %) | 230.3 µs (2.9 %) | 7,541 qps | 4,342 qps | **1.74x** | KDS ahead |
| `tape10k`, `declared` | 396 ops | 72.4 µs (5.5 %) | 230.3 µs (2.9 %) | 13,812 qps | 4,342 qps | **3.18x** | KDS ahead |
| `tape10k`, `on` | 396 ops | 135.4 µs (3.1 %) | 230.3 µs (2.9 %) | 7,386 qps | 4,342 qps | **1.70x** | KDS ahead |
| `board`, `off` | 2,400 ops | 210.8 µs (5.6 %) | 256.8 µs (3.1 %) | 4,744 qps | 3,894 qps | **1.22x** | KDS ahead |
| `board`, `declared` | 2,400 ops | 83.7 µs (3.3 %) | 256.8 µs (3.1 %) | 11,947 qps | 3,894 qps | **3.07x** | KDS ahead |
| `board`, `on` | 2,400 ops | 148.1 µs (2.3 %) | 256.8 µs (3.1 %) | 6,752 qps | 3,894 qps | **1.73x** | KDS ahead |
| `open`, `off` | 240 ops | 156.7 µs (140.3 %) | 144.5 µs (3.0 %) | 6,382 qps | 6,920 qps | **0.92x** | no claim |
| `open`, `declared` | 240 ops | 162.3 µs (118.7 %) | 144.5 µs (3.0 %) | 6,161 qps | 6,920 qps | **0.89x** | no claim |
| `open`, `on` | 240 ops | 153.7 µs (150.7 %) | 144.5 µs (3.0 %) | 6,506 qps | 6,920 qps | **0.94x** | no claim |
| `close`, `off` | 12 ops | 849.7 µs (35.5 %) | 495.0 µs (12.2 %) | 1,177 qps | 2,020 qps | **0.58x** | no claim |
| `close`, `declared` | 12 ops | 604.0 µs (27.1 %) | 495.0 µs (12.2 %) | 1,656 qps | 2,020 qps | **0.82x** | no claim |
| `close`, `on` | 12 ops | 705.2 µs (14.1 %) | 495.0 µs (12.2 %) | 1,418 qps | 2,020 qps | **0.70x** | PostgreSQL ahead |

| group | engine | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `pk` | KDS `off` | 20.5 µs | 17.8 µs | 19.0 µs | 19.3 µs | 27.1 µs | 30.8 µs |
| `pk` | PostgreSQL | 21.2 µs | 18.9 µs | 20.3 µs | 20.7 µs | 24.5 µs | 39.4 µs |
| `tape200` | KDS `off` | 26.6 µs | 23.2 µs | 24.9 µs | 25.7 µs | 31.7 µs | 36.9 µs |
| `tape200` | PostgreSQL | 38.7 µs | 29.8 µs | 31.6 µs | 32.6 µs | 47.8 µs | 259.1 µs |
| `tape1k` | KDS `off` | 34.8 µs | 31.5 µs | 33.3 µs | 34.1 µs | 39.6 µs | 41.7 µs |
| `tape1k` | PostgreSQL | 49.8 µs | 45.9 µs | 47.5 µs | 48.6 µs | 61.1 µs | 64.2 µs |
| `tape10k` | KDS `off` | 132.6 µs | 122.2 µs | 126.3 µs | 127.8 µs | 149.7 µs | 210.5 µs |
| `tape10k` | PostgreSQL | 230.3 µs | 220.2 µs | 225.7 µs | 227.7 µs | 247.1 µs | 294.0 µs |
| `board` | KDS `off` | 210.8 µs | 126.6 µs | 139.7 µs | 148.1 µs | 603.9 µs | 1,328.9 µs |
| `board` | PostgreSQL | 256.8 µs | 224.4 µs | 243.0 µs | 248.2 µs | 305.7 µs | 494.5 µs |
| `open` | KDS `off` | 156.7 µs | 130.1 µs | 133.3 µs | 149.5 µs | 218.0 µs | 516.9 µs |
| `open` | PostgreSQL | 144.5 µs | 131.1 µs | 134.0 µs | 134.6 µs | 166.7 µs | 273.1 µs |
| `close` | KDS `off` | 849.7 µs | 277.7 µs | 394.2 µs | 613.5 µs | 2,465.5 µs | 2,465.5 µs |
| `close` | PostgreSQL | 495.0 µs | 164.7 µs | 172.3 µs | 214.4 µs | 1,595.7 µs | 1,595.7 µs |

## Scenario 2: one booker, one core; reads decoded on both sides, writes counted on both

**KDS `strict` against PostgreSQL `synchronous_commit = on`**: 2,077.5 bookings a second (runs 2,059.6, 2,086.1, 2,077.5; spread 1.3 %) against 2,070.2 (2,092.2, 2,069.1, 2,070.2; 1.1 %): **1.00x**, inside the spread.

| statement | ops a run | KDS mean (spread) | PostgreSQL mean (spread) | KDS rate | PG rate | KDS / PG | read |
|---|---|---|---|---|---|---|---|
| `cargo-lookup` | 3,402 ops | 26.1 µs (0.8 %) | 25.4 µs (2.0 %) | 38,314 qps | 39,370 qps | **0.97x** | PostgreSQL ahead |
| `credit-lookup` | 3,402 ops | 22.6 µs (2.2 %) | 22.8 µs (1.8 %) | 44,248 qps | 43,860 qps | **1.01x** | inside the spread |
| `capacity-read` | 3,402 ops | 21.8 µs (1.4 %) | 21.9 µs (0.9 %) | 45,872 qps | 45,662 qps | **1.00x** | inside the spread |
| `recipe-read` | 3,402 ops | 39.0 µs (1.3 %) | 38.6 µs (2.1 %) | 25,641 qps | 25,907 qps | **0.99x** | inside the spread |
| `freight-insert` | 3,000 ops | 21.4 µs (2.3 %) | 29.6 µs (1.4 %) | 46,729 qps | 33,784 qps | **1.38x** | KDS ahead |
| `charge-insert` | 16,907 ops | 18.3 µs (1.6 %) | 19.6 µs (1.5 %) | 54,645 qps | 51,020 qps | **1.07x** | KDS ahead |
| `operation-update` | 3,000 ops | 21.2 µs (3.3 %) | 23.4 µs (1.3 %) | 47,170 qps | 42,735 qps | **1.10x** | KDS ahead |
| `org-update` | 3,000 ops | 19.6 µs (1.0 %) | 21.3 µs (2.3 %) | 51,020 qps | 46,948 qps | **1.09x** | KDS ahead |
| `commit` | 3,000 ops | 134.2 µs (3.8 %) | 141.1 µs (0.6 %) | 7,452 qps | 7,087 qps | **1.05x** | KDS ahead |
| `manifest-scan` | 40 ops | 85.4 µs (31.1 %) | 41.8 µs (11.5 %) | 11,710 qps | 23,923 qps | **0.49x** | no claim |
| `voyage-rollup` | 40 ops | 78.5 µs (40.6 %) | 44.3 µs (15.1 %) | 12,739 qps | 22,573 qps | **0.56x** | no claim |
| `customer-statement` | 20 ops | 385.1 µs (3.2 %) | 178.2 µs (45.7 %) | 2,597 qps | 5,612 qps | **0.46x** | no claim |

**KDS `group` against PostgreSQL `synchronous_commit = on`**: 2,077.9 bookings a second (runs 2,077.9, 2,106.6, 2,067.6; spread 1.9 %) against 2,070.2 (2,092.2, 2,069.1, 2,070.2; 1.1 %): **1.00x**, inside the spread.

| statement | ops a run | KDS mean (spread) | PostgreSQL mean (spread) | KDS rate | PG rate | KDS / PG | read |
|---|---|---|---|---|---|---|---|
| `cargo-lookup` | 3,402 ops | 25.7 µs (2.7 %) | 25.4 µs (2.0 %) | 38,911 qps | 39,370 qps | **0.99x** | inside the spread |
| `credit-lookup` | 3,402 ops | 22.4 µs (3.6 %) | 22.8 µs (1.8 %) | 44,643 qps | 43,860 qps | **1.02x** | inside the spread |
| `capacity-read` | 3,402 ops | 21.6 µs (2.3 %) | 21.9 µs (0.9 %) | 46,296 qps | 45,662 qps | **1.01x** | inside the spread |
| `recipe-read` | 3,402 ops | 38.6 µs (3.4 %) | 38.6 µs (2.1 %) | 25,907 qps | 25,907 qps | **1.00x** | inside the spread |
| `freight-insert` | 3,000 ops | 21.2 µs (0.9 %) | 29.6 µs (1.4 %) | 47,170 qps | 33,784 qps | **1.40x** | KDS ahead |
| `charge-insert` | 16,907 ops | 18.2 µs (1.6 %) | 19.6 µs (1.5 %) | 54,945 qps | 51,020 qps | **1.08x** | KDS ahead |
| `operation-update` | 3,000 ops | 20.8 µs (1.4 %) | 23.4 µs (1.3 %) | 48,077 qps | 42,735 qps | **1.12x** | KDS ahead |
| `org-update` | 3,000 ops | 19.4 µs (1.5 %) | 21.3 µs (2.3 %) | 51,546 qps | 46,948 qps | **1.10x** | KDS ahead |
| `commit` | 3,000 ops | 136.5 µs (2.4 %) | 141.1 µs (0.6 %) | 7,326 qps | 7,087 qps | **1.03x** | KDS ahead |
| `manifest-scan` | 40 ops | 91.0 µs (19.6 %) | 41.8 µs (11.5 %) | 10,989 qps | 23,923 qps | **0.46x** | PostgreSQL ahead |
| `voyage-rollup` | 40 ops | 76.9 µs (38.2 %) | 44.3 µs (15.1 %) | 13,004 qps | 22,573 qps | **0.58x** | no claim |
| `customer-statement` | 20 ops | 358.7 µs (5.6 %) | 178.2 µs (45.7 %) | 2,788 qps | 5,612 qps | **0.50x** | no claim |

| statement | engine | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `cargo-lookup` | KDS `strict` | 26.1 µs | 20.8 µs | 24.3 µs | 24.8 µs | 27.5 µs | 49.7 µs |
| `cargo-lookup` | PostgreSQL | 25.4 µs | 20.2 µs | 23.8 µs | 24.4 µs | 27.6 µs | 49.8 µs |
| `credit-lookup` | KDS `strict` | 22.6 µs | 19.4 µs | 21.2 µs | 21.6 µs | 23.3 µs | 38.9 µs |
| `credit-lookup` | PostgreSQL | 22.8 µs | 18.6 µs | 21.3 µs | 22.3 µs | 24.5 µs | 32.6 µs |
| `capacity-read` | KDS `strict` | 21.8 µs | 19.1 µs | 20.7 µs | 21.0 µs | 22.3 µs | 33.7 µs |
| `capacity-read` | PostgreSQL | 21.9 µs | 17.9 µs | 20.9 µs | 21.5 µs | 23.2 µs | 27.8 µs |
| `recipe-read` | KDS `strict` | 39.0 µs | 33.5 µs | 37.2 µs | 38.2 µs | 40.8 µs | 59.9 µs |
| `recipe-read` | PostgreSQL | 38.6 µs | 33.2 µs | 36.6 µs | 37.7 µs | 40.7 µs | 52.1 µs |
| `freight-insert` | KDS `strict` | 21.4 µs | 18.5 µs | 20.1 µs | 20.4 µs | 22.6 µs | 35.3 µs |
| `freight-insert` | PostgreSQL | 29.6 µs | 25.8 µs | 27.4 µs | 28.0 µs | 33.9 µs | 54.5 µs |
| `charge-insert` | KDS `strict` | 18.3 µs | 15.7 µs | 17.3 µs | 17.6 µs | 18.8 µs | 29.9 µs |
| `charge-insert` | PostgreSQL | 19.6 µs | 17.0 µs | 18.4 µs | 18.7 µs | 20.7 µs | 31.5 µs |
| `operation-update` | KDS `strict` | 21.2 µs | 18.2 µs | 19.8 µs | 20.1 µs | 21.7 µs | 33.8 µs |
| `operation-update` | PostgreSQL | 23.4 µs | 20.6 µs | 22.1 µs | 22.6 µs | 25.5 µs | 34.4 µs |
| `org-update` | KDS `strict` | 19.6 µs | 17.1 µs | 18.6 µs | 18.8 µs | 20.0 µs | 26.9 µs |
| `org-update` | PostgreSQL | 21.3 µs | 19.0 µs | 20.4 µs | 20.7 µs | 22.6 µs | 27.4 µs |
| `commit` | KDS `strict` | 134.2 µs | 118.4 µs | 128.7 µs | 129.6 µs | 153.7 µs | 176.4 µs |
| `commit` | PostgreSQL | 141.1 µs | 123.7 µs | 131.7 µs | 133.1 µs | 158.4 µs | 181.0 µs |

KDS outcomes, all six runs: {'committed': 3000, 'rejected-capacity': 328, 'rejected-credit': 74, 'conflicted': 0, 'failed': 0}; identical across runs: True.

## Scenario 3: the library; every read shape decoded on both sides

### 200 loans, unindexed

| shape | ops a run | KDS mean (spread) | PostgreSQL mean (spread) | KDS rate | PG rate | KDS / PG | read |
|---|---|---|---|---|---|---|---|
| `pk-user` | 200 ops | 27.4 µs (3.6 %) | 27.6 µs (35.1 %) | 36,496 qps | 36,232 qps | **1.01x** | no claim |
| `loans-by-user` | 200 ops | 30.1 µs (3.7 %) | 34.0 µs (14.1 %) | 33,223 qps | 29,412 qps | **1.13x** | inside the spread |
| `loans-by-book` | 200 ops | 27.2 µs (2.9 %) | 27.4 µs (11.3 %) | 36,765 qps | 36,496 qps | **1.01x** | inside the spread |
| `resv-by-user` | 200 ops | 23.3 µs (6.9 %) | 22.9 µs (1.7 %) | 42,918 qps | 43,668 qps | **0.98x** | inside the spread |
| `books-by-author` | 200 ops | 24.4 µs (5.3 %) | 23.5 µs (2.1 %) | 40,984 qps | 42,553 qps | **0.96x** | inside the spread |
| `books-by-genre` | 200 ops | 22.7 µs (3.1 %) | 21.9 µs (3.2 %) | 44,053 qps | 45,662 qps | **0.96x** | PostgreSQL ahead |
| `loans-by-daterange` | 200 ops | 31.1 µs (2.9 %) | 33.0 µs (3.0 %) | 32,154 qps | 30,303 qps | **1.06x** | KDS ahead |
| `overdue` | 200 ops | 28.5 µs (17.9 %) | 30.9 µs (1.6 %) | 35,088 qps | 32,362 qps | **1.08x** | inside the spread |
| `join-loan-user` | 200 ops | 31.3 µs (7.7 %) | 35.7 µs (1.7 %) | 31,949 qps | 28,011 qps | **1.14x** | KDS ahead |
| `join-no-literal` | 200 ops | 87.0 µs (10.7 %) | 106.6 µs (3.5 %) | 11,494 qps | 9,381 qps | **1.23x** | KDS ahead |
| `exists-correlated` | 200 ops | 45.4 µs (22.7 %) | 67.5 µs (16.6 %) | 22,026 qps | 14,815 qps | **1.49x** | no claim |
| `count-by-user` | 200 ops | 21.8 µs (2.8 %) | 24.0 µs (1.7 %) | 45,872 qps | 41,667 qps | **1.10x** | KDS ahead |

Geometric mean of the twelve ratios: 1.09x.

### 200 loans, indexed

| shape | ops a run | KDS mean (spread) | PostgreSQL mean (spread) | KDS rate | PG rate | KDS / PG | read |
|---|---|---|---|---|---|---|---|
| `pk-user` | 200 ops | 24.1 µs (11.2 %) | 27.8 µs (8.6 %) | 41,494 qps | 35,971 qps | **1.15x** | KDS ahead |
| `loans-by-user` | 200 ops | 33.5 µs (17.0 %) | 35.6 µs (19.1 %) | 29,851 qps | 28,090 qps | **1.06x** | inside the spread |
| `loans-by-book` | 200 ops | 26.6 µs (0.8 %) | 29.4 µs (34.7 %) | 37,594 qps | 34,014 qps | **1.11x** | no claim |
| `resv-by-user` | 200 ops | 22.9 µs (1.3 %) | 26.2 µs (16.4 %) | 43,668 qps | 38,168 qps | **1.14x** | inside the spread |
| `books-by-author` | 200 ops | 25.0 µs (1.2 %) | 25.9 µs (3.9 %) | 40,000 qps | 38,610 qps | **1.04x** | inside the spread |
| `books-by-genre` | 200 ops | 23.0 µs (1.3 %) | 27.3 µs (45.4 %) | 43,478 qps | 36,630 qps | **1.19x** | no claim |
| `loans-by-daterange` | 200 ops | 31.1 µs (0.6 %) | 35.4 µs (9.9 %) | 32,154 qps | 28,249 qps | **1.14x** | KDS ahead |
| `overdue` | 200 ops | 27.5 µs (0.7 %) | 35.1 µs (20.5 %) | 36,364 qps | 28,490 qps | **1.28x** | no claim |
| `join-loan-user` | 200 ops | 31.3 µs (23.6 %) | 39.1 µs (1.5 %) | 31,949 qps | 25,575 qps | **1.25x** | no claim |
| `join-no-literal` | 200 ops | 100.2 µs (9.7 %) | 118.0 µs (20.3 %) | 9,980 qps | 8,475 qps | **1.18x** | no claim |
| `exists-correlated` | 200 ops | 53.1 µs (2.1 %) | 73.2 µs (9.8 %) | 18,832 qps | 13,661 qps | **1.38x** | KDS ahead |
| `count-by-user` | 200 ops | 21.9 µs (32.0 %) | 26.7 µs (1.1 %) | 45,662 qps | 37,453 qps | **1.22x** | no claim |

Geometric mean of the twelve ratios: 1.17x.

### 1,000 loans, unindexed

| shape | ops a run | KDS mean (spread) | PostgreSQL mean (spread) | KDS rate | PG rate | KDS / PG | read |
|---|---|---|---|---|---|---|---|
| `pk-user` | 200 ops | 26.3 µs (27.8 %) | 31.2 µs (52.2 %) | 38,023 qps | 32,051 qps | **1.19x** | no claim |
| `loans-by-user` | 200 ops | 39.7 µs (1.3 %) | 44.5 µs (19.6 %) | 25,189 qps | 22,472 qps | **1.12x** | inside the spread |
| `loans-by-book` | 200 ops | 36.4 µs (6.0 %) | 41.3 µs (1.0 %) | 27,473 qps | 24,213 qps | **1.13x** | KDS ahead |
| `resv-by-user` | 200 ops | 29.3 µs (13.7 %) | 30.7 µs (36.2 %) | 34,130 qps | 32,573 qps | **1.05x** | no claim |
| `books-by-author` | 200 ops | 27.7 µs (53.8 %) | 27.0 µs (2.6 %) | 36,101 qps | 37,037 qps | **0.97x** | no claim |
| `books-by-genre` | 200 ops | 33.4 µs (1.5 %) | 35.1 µs (1.7 %) | 29,940 qps | 28,490 qps | **1.05x** | KDS ahead |
| `loans-by-daterange` | 200 ops | 70.9 µs (4.4 %) | 82.1 µs (0.5 %) | 14,104 qps | 12,180 qps | **1.16x** | KDS ahead |
| `overdue` | 200 ops | 60.2 µs (11.1 %) | 70.5 µs (0.6 %) | 16,611 qps | 14,184 qps | **1.17x** | KDS ahead |
| `join-loan-user` | 200 ops | 39.6 µs (1.0 %) | 52.3 µs (2.5 %) | 25,253 qps | 19,120 qps | **1.32x** | KDS ahead |
| `join-no-literal` | 200 ops | 103.0 µs (7.0 %) | 145.4 µs (7.4 %) | 9,709 qps | 6,878 qps | **1.41x** | KDS ahead |
| `exists-correlated` | 200 ops | 62.9 µs (1.3 %) | 137.4 µs (1.0 %) | 15,898 qps | 7,278 qps | **2.18x** | KDS ahead |
| `count-by-user` | 200 ops | 30.7 µs (9.4 %) | 37.6 µs (1.9 %) | 32,573 qps | 26,596 qps | **1.22x** | KDS ahead |

Geometric mean of the twelve ratios: 1.22x.

### 1,000 loans, indexed

| shape | ops a run | KDS mean (spread) | PostgreSQL mean (spread) | KDS rate | PG rate | KDS / PG | read |
|---|---|---|---|---|---|---|---|
| `pk-user` | 200 ops | 28.6 µs (19.6 %) | 31.7 µs (56.5 %) | 34,965 qps | 31,546 qps | **1.11x** | no claim |
| `loans-by-user` | 200 ops | 32.4 µs (2.2 %) | 35.5 µs (15.8 %) | 30,864 qps | 28,169 qps | **1.10x** | inside the spread |
| `loans-by-book` | 200 ops | 27.5 µs (3.6 %) | 28.3 µs (1.8 %) | 36,364 qps | 35,336 qps | **1.03x** | inside the spread |
| `resv-by-user` | 200 ops | 24.3 µs (2.9 %) | 24.1 µs (1.2 %) | 41,152 qps | 41,494 qps | **0.99x** | inside the spread |
| `books-by-author` | 200 ops | 25.8 µs (3.1 %) | 28.5 µs (1.1 %) | 38,760 qps | 35,088 qps | **1.10x** | KDS ahead |
| `books-by-genre` | 200 ops | 34.7 µs (0.9 %) | 36.7 µs (1.6 %) | 28,818 qps | 27,248 qps | **1.06x** | KDS ahead |
| `loans-by-daterange` | 200 ops | 71.5 µs (2.4 %) | 83.6 µs (1.1 %) | 13,986 qps | 11,962 qps | **1.17x** | KDS ahead |
| `overdue` | 200 ops | 52.2 µs (20.5 %) | 55.3 µs (8.1 %) | 19,157 qps | 18,083 qps | **1.06x** | no claim |
| `join-loan-user` | 200 ops | 31.3 µs (14.7 %) | 41.0 µs (9.5 %) | 31,949 qps | 24,390 qps | **1.31x** | KDS ahead |
| `join-no-literal` | 200 ops | 105.0 µs (2.2 %) | 164.9 µs (12.1 %) | 9,524 qps | 6,064 qps | **1.57x** | KDS ahead |
| `exists-correlated` | 200 ops | 53.9 µs (4.6 %) | 86.3 µs (1.9 %) | 18,553 qps | 11,587 qps | **1.60x** | KDS ahead |
| `count-by-user` | 200 ops | 22.0 µs (15.0 %) | 25.3 µs (2.8 %) | 45,455 qps | 39,526 qps | **1.15x** | KDS ahead |

Geometric mean of the twelve ratios: 1.17x.

### 10,000 loans, unindexed

| shape | ops a run | KDS mean (spread) | PostgreSQL mean (spread) | KDS rate | PG rate | KDS / PG | read |
|---|---|---|---|---|---|---|---|
| `pk-user` | 200 ops | 31.0 µs (23.5 %) | 40.9 µs (11.5 %) | 32,258 qps | 24,450 qps | **1.32x** | no claim |
| `loans-by-user` | 200 ops | 136.8 µs (3.4 %) | 192.0 µs (3.4 %) | 7,310 qps | 5,208 qps | **1.40x** | KDS ahead |
| `loans-by-book` | 200 ops | 134.3 µs (1.6 %) | 200.0 µs (8.5 %) | 7,446 qps | 5,000 qps | **1.49x** | KDS ahead |
| `resv-by-user` | 200 ops | 77.2 µs (4.0 %) | 105.1 µs (10.8 %) | 12,953 qps | 9,515 qps | **1.36x** | KDS ahead |
| `books-by-author` | 200 ops | 48.3 µs (1.7 %) | 60.1 µs (5.0 %) | 20,704 qps | 16,639 qps | **1.24x** | KDS ahead |
| `books-by-genre` | 200 ops | 148.5 µs (1.6 %) | 193.3 µs (11.5 %) | 6,734 qps | 5,173 qps | **1.30x** | KDS ahead |
| `loans-by-daterange` | 200 ops | 608.2 µs (1.7 %) | 647.6 µs (2.8 %) | 1,644 qps | 1,544 qps | **1.06x** | KDS ahead |
| `overdue` | 200 ops | 473.0 µs (3.5 %) | 576.7 µs (3.2 %) | 2,114 qps | 1,734 qps | **1.22x** | KDS ahead |
| `join-loan-user` | 200 ops | 138.7 µs (0.8 %) | 195.0 µs (1.3 %) | 7,210 qps | 5,128 qps | **1.41x** | KDS ahead |
| `join-no-literal` | 200 ops | 233.8 µs (5.2 %) | 325.0 µs (2.6 %) | 4,277 qps | 3,077 qps | **1.39x** | KDS ahead |
| `exists-correlated` | 200 ops | 241.9 µs (10.8 %) | 680.6 µs (13.0 %) | 4,134 qps | 1,469 qps | **2.81x** | KDS ahead |
| `count-by-user` | 200 ops | 129.5 µs (4.2 %) | 199.1 µs (14.2 %) | 7,722 qps | 5,023 qps | **1.54x** | KDS ahead |

Geometric mean of the twelve ratios: 1.42x.

| shape | engine | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `pk-user` | KDS | 31.0 µs | 24.6 µs | 25.8 µs | 27.1 µs | 34.3 µs | 41.0 µs |
| `pk-user` | PostgreSQL | 40.9 µs | 29.3 µs | 29.9 µs | 34.4 µs | 51.6 µs | 85.7 µs |
| `loans-by-user` | KDS | 136.8 µs | 127.9 µs | 132.7 µs | 135.2 µs | 146.2 µs | 180.0 µs |
| `loans-by-user` | PostgreSQL | 192.0 µs | 175.1 µs | 183.0 µs | 187.8 µs | 203.0 µs | 327.3 µs |
| `loans-by-book` | KDS | 134.3 µs | 126.7 µs | 131.9 µs | 133.9 µs | 140.2 µs | 145.1 µs |
| `loans-by-book` | PostgreSQL | 200.0 µs | 183.7 µs | 192.3 µs | 196.9 µs | 212.1 µs | 215.0 µs |
| `resv-by-user` | KDS | 77.2 µs | 71.7 µs | 75.6 µs | 76.7 µs | 81.0 µs | 85.4 µs |
| `resv-by-user` | PostgreSQL | 105.1 µs | 96.9 µs | 102.7 µs | 104.5 µs | 112.2 µs | 130.3 µs |
| `books-by-author` | KDS | 48.3 µs | 42.6 µs | 46.7 µs | 47.8 µs | 52.2 µs | 53.8 µs |
| `books-by-author` | PostgreSQL | 60.1 µs | 53.5 µs | 58.0 µs | 59.5 µs | 65.6 µs | 69.7 µs |
| `books-by-genre` | KDS | 148.5 µs | 135.1 µs | 143.9 µs | 148.0 µs | 156.8 µs | 175.2 µs |
| `books-by-genre` | PostgreSQL | 193.3 µs | 157.9 µs | 169.2 µs | 176.3 µs | 331.4 µs | 365.9 µs |
| `loans-by-daterange` | KDS | 608.2 µs | 193.3 µs | 542.9 µs | 636.2 µs | 718.1 µs | 750.5 µs |
| `loans-by-daterange` | PostgreSQL | 647.6 µs | 384.4 µs | 561.9 µs | 672.1 µs | 740.5 µs | 770.5 µs |
| `overdue` | KDS | 473.0 µs | 171.3 µs | 426.0 µs | 446.5 µs | 607.1 µs | 638.6 µs |
| `overdue` | PostgreSQL | 576.7 µs | 210.7 µs | 506.9 µs | 530.5 µs | 709.9 µs | 739.0 µs |
| `join-loan-user` | KDS | 138.7 µs | 131.4 µs | 135.5 µs | 137.3 µs | 144.6 µs | 178.5 µs |
| `join-loan-user` | PostgreSQL | 195.0 µs | 184.1 µs | 188.7 µs | 191.7 µs | 204.8 µs | 231.4 µs |
| `join-no-literal` | KDS | 233.8 µs | 223.8 µs | 226.0 µs | 227.3 µs | 238.4 µs | 355.1 µs |
| `join-no-literal` | PostgreSQL | 325.0 µs | 305.8 µs | 310.2 µs | 313.2 µs | 421.2 µs | 532.6 µs |
| `exists-correlated` | KDS | 241.9 µs | 226.7 µs | 236.0 µs | 238.6 µs | 248.4 µs | 277.6 µs |
| `exists-correlated` | PostgreSQL | 680.6 µs | 628.6 µs | 644.1 µs | 673.0 µs | 828.4 µs | 930.1 µs |
| `count-by-user` | KDS | 129.5 µs | 125.3 µs | 127.8 µs | 129.2 µs | 134.1 µs | 139.1 µs |
| `count-by-user` | PostgreSQL | 199.1 µs | 181.2 µs | 188.0 µs | 190.8 µs | 201.5 µs | 443.0 µs |

### 10,000 loans, indexed

| shape | ops a run | KDS mean (spread) | PostgreSQL mean (spread) | KDS rate | PG rate | KDS / PG | read |
|---|---|---|---|---|---|---|---|
| `pk-user` | 200 ops | 44.6 µs (61.7 %) | 44.4 µs (17.1 %) | 22,422 qps | 22,523 qps | **1.00x** | no claim |
| `loans-by-user` | 200 ops | 31.4 µs (8.0 %) | 37.5 µs (61.9 %) | 31,847 qps | 26,667 qps | **1.19x** | no claim |
| `loans-by-book` | 200 ops | 27.3 µs (3.7 %) | 28.6 µs (82.5 %) | 36,630 qps | 34,965 qps | **1.05x** | no claim |
| `resv-by-user` | 200 ops | 24.3 µs (4.5 %) | 24.1 µs (7.9 %) | 41,152 qps | 41,494 qps | **0.99x** | inside the spread |
| `books-by-author` | 200 ops | 26.5 µs (6.8 %) | 27.0 µs (3.0 %) | 37,736 qps | 37,037 qps | **1.02x** | inside the spread |
| `books-by-genre` | 200 ops | 164.6 µs (5.6 %) | 144.9 µs (6.0 %) | 6,075 qps | 6,901 qps | **0.88x** | PostgreSQL ahead |
| `loans-by-daterange` | 200 ops | 613.1 µs (4.3 %) | 655.7 µs (1.2 %) | 1,631 qps | 1,525 qps | **1.07x** | KDS ahead |
| `overdue` | 200 ops | 386.5 µs (8.4 %) | 412.2 µs (9.5 %) | 2,587 qps | 2,426 qps | **1.07x** | inside the spread |
| `join-loan-user` | 200 ops | 33.5 µs (2.4 %) | 41.0 µs (2.7 %) | 29,851 qps | 24,390 qps | **1.22x** | KDS ahead |
| `join-no-literal` | 200 ops | 100.7 µs (4.0 %) | 327.5 µs (67.6 %) | 9,930 qps | 3,053 qps | **3.25x** | no claim |
| `exists-correlated` | 200 ops | 53.6 µs (0.4 %) | 85.3 µs (33.6 %) | 18,657 qps | 11,723 qps | **1.59x** | no claim |
| `count-by-user` | 200 ops | 22.1 µs (0.5 %) | 25.1 µs (3.6 %) | 45,249 qps | 39,841 qps | **1.14x** | KDS ahead |

Geometric mean of the twelve ratios: 1.20x.

| shape | engine | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `pk-user` | KDS | 44.6 µs | 21.9 µs | 23.0 µs | 28.0 µs | 81.2 µs | 117.3 µs |
| `pk-user` | PostgreSQL | 44.4 µs | 20.9 µs | 33.4 µs | 33.8 µs | 65.5 µs | 105.3 µs |
| `loans-by-user` | KDS | 31.4 µs | 22.0 µs | 27.5 µs | 30.1 µs | 37.8 µs | 45.8 µs |
| `loans-by-user` | PostgreSQL | 37.5 µs | 24.7 µs | 30.5 µs | 34.0 µs | 56.2 µs | 64.3 µs |
| `loans-by-book` | KDS | 27.3 µs | 20.0 µs | 25.2 µs | 27.1 µs | 32.7 µs | 36.5 µs |
| `loans-by-book` | PostgreSQL | 28.6 µs | 20.7 µs | 26.0 µs | 28.0 µs | 35.0 µs | 40.0 µs |
| `resv-by-user` | KDS | 24.3 µs | 18.5 µs | 23.0 µs | 24.0 µs | 28.0 µs | 29.6 µs |
| `resv-by-user` | PostgreSQL | 24.1 µs | 19.1 µs | 22.2 µs | 23.5 µs | 29.2 µs | 33.6 µs |
| `books-by-author` | KDS | 26.5 µs | 19.4 µs | 24.7 µs | 26.2 µs | 31.6 µs | 34.6 µs |
| `books-by-author` | PostgreSQL | 27.0 µs | 20.2 µs | 24.8 µs | 26.5 µs | 32.2 µs | 38.2 µs |
| `books-by-genre` | KDS | 164.6 µs | 139.6 µs | 150.2 µs | 155.5 µs | 175.9 µs | 310.7 µs |
| `books-by-genre` | PostgreSQL | 144.9 µs | 127.4 µs | 137.1 µs | 142.2 µs | 152.6 µs | 192.0 µs |
| `loans-by-daterange` | KDS | 613.1 µs | 194.4 µs | 539.3 µs | 633.7 µs | 734.1 µs | 760.1 µs |
| `loans-by-daterange` | PostgreSQL | 655.7 µs | 341.6 µs | 568.9 µs | 682.8 µs | 786.1 µs | 855.8 µs |
| `overdue` | KDS | 386.5 µs | 25.2 µs | 340.3 µs | 365.8 µs | 536.5 µs | 569.9 µs |
| `overdue` | PostgreSQL | 412.2 µs | 38.4 µs | 334.1 µs | 454.3 µs | 526.6 µs | 549.8 µs |
| `join-loan-user` | KDS | 33.5 µs | 25.7 µs | 29.7 µs | 31.4 µs | 36.6 µs | 49.2 µs |
| `join-loan-user` | PostgreSQL | 41.0 µs | 31.7 µs | 35.5 µs | 37.2 µs | 43.2 µs | 123.2 µs |
| `join-no-literal` | KDS | 100.7 µs | 97.1 µs | 99.0 µs | 99.8 µs | 102.7 µs | 133.7 µs |
| `join-no-literal` | PostgreSQL | 327.5 µs | 314.4 µs | 319.3 µs | 321.9 µs | 336.2 µs | 507.6 µs |
| `exists-correlated` | KDS | 53.6 µs | 50.8 µs | 52.4 µs | 53.0 µs | 54.6 µs | 77.8 µs |
| `exists-correlated` | PostgreSQL | 85.3 µs | 81.4 µs | 82.9 µs | 83.7 µs | 87.3 µs | 99.2 µs |
| `count-by-user` | KDS | 22.1 µs | 19.3 µs | 21.4 µs | 21.9 µs | 23.5 µs | 24.4 µs |
| `count-by-user` | PostgreSQL | 25.1 µs | 23.2 µs | 24.4 µs | 24.9 µs | 26.0 µs | 27.6 µs |

## Scenario 1: the backtest; both sides decode and render, as before EF

### 60,480 bars, KDS `group`

| phase | ops a run | KDS mean (spread) | PostgreSQL mean (spread) | KDS rate | PG rate | KDS / PG | read |
|---|---|---|---|---|---|---|---|
| `load-bars` | 60,480 ops | 19.5 µs (3.6 %) | 26.2 µs (0.8 %) | 51,282 qps | 38,168 qps | **1.34x** | KDS ahead |
| `load-stats` | 60,480 ops | 18.6 µs (1.1 %) | 26.1 µs (0.4 %) | 53,763 qps | 38,314 qps | **1.40x** | KDS ahead |
| `backtest-read` | 360 ops | 756.3 µs (11.3 %) | 1,285.0 µs (2.2 %) | 1,322 qps | 778 qps | **1.70x** | KDS ahead |
| `result-insert` | 2,872 ops | 204.8 µs (98.6 %) | 171.5 µs (7.3 %) | 4,883 qps | 5,831 qps | **0.84x** | no claim |
| `backtest-replay` | 360 ops | 727.0 µs (3.6 %) | 1,352.7 µs (4.3 %) | 1,376 qps | 739 qps | **1.86x** | KDS ahead |
| `compare-all` | 1 ops | 8,655.5 µs (2.3 %) | 10,794.4 µs (12.1 %) | 116 qps | 93 qps | **1.25x** | KDS ahead |
| `read-bar-lookup` | 200 ops | 27.2 µs (10.7 %) | 28.8 µs (9.7 %) | 36,765 qps | 34,722 qps | **1.06x** | inside the spread |
| `read-bar-range` | 200 ops | 593.9 µs (3.0 %) | 579.8 µs (1.2 %) | 1,684 qps | 1,725 qps | **0.98x** | inside the spread |
| `read-symbol-history` | 10 ops | 24,468.5 µs (1.4 %) | 21,443.2 µs (1.8 %) | 41 qps | 47 qps | **0.88x** | PostgreSQL ahead |
| `read-day-slice` | 200 ops | 724.9 µs (2.8 %) | 1,286.1 µs (3.1 %) | 1,380 qps | 778 qps | **1.77x** | KDS ahead |
| `read-join-point` | 200 ops | 40.5 µs (5.7 %) | 70.9 µs (8.6 %) | 24,691 qps | 14,104 qps | **1.75x** | KDS ahead |
| `read-join-exists` | 10 ops | 2,075.6 µs (3.9 %) | 4,311.6 µs (1.2 %) | 482 qps | 232 qps | **2.08x** | KDS ahead |
| `agg-global` | 10 ops | 2,471.2 µs (4.8 %) | 2,382.5 µs (5.1 %) | 405 qps | 420 qps | **0.96x** | inside the spread |
| `agg-by-symbol` | 10 ops | 3,790.2 µs (9.8 %) | 3,660.6 µs (1.7 %) | 264 qps | 273 qps | **0.97x** | inside the spread |
| `agg-by-session` | 10 ops | 13,681.2 µs (6.6 %) | 12,482.6 µs (2.2 %) | 73 qps | 80 qps | **0.91x** | PostgreSQL ahead |
| `agg-day-slice` | 200 ops | 708.9 µs (4.0 %) | 1,261.6 µs (4.1 %) | 1,411 qps | 793 qps | **1.78x** | KDS ahead |
| `agg-distinct` | 10 ops | 2,707.2 µs (6.1 %) | 2,116.2 µs (11.4 %) | 369 qps | 473 qps | **0.78x** | PostgreSQL ahead |

| phase | engine | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `load-bars` | KDS `group` | 19.5 µs | 16.4 µs | 17.9 µs | 18.3 µs | 23.6 µs | 49.6 µs |
| `load-bars` | PostgreSQL | 26.2 µs | 22.8 µs | 24.4 µs | 24.6 µs | 27.1 µs | 59.2 µs |
| `load-stats` | KDS `group` | 18.6 µs | 16.5 µs | 18.0 µs | 18.3 µs | 19.1 µs | 26.4 µs |
| `load-stats` | PostgreSQL | 26.1 µs | 23.6 µs | 25.5 µs | 25.7 µs | 26.8 µs | 31.3 µs |
| `backtest-read` | KDS `group` | 756.3 µs | 699.2 µs | 725.5 µs | 735.4 µs | 815.8 µs | 1,186.4 µs |
| `backtest-read` | PostgreSQL | 1,285.0 µs | 1,202.7 µs | 1,251.2 µs | 1,271.3 µs | 1,325.9 µs | 1,477.2 µs |
| `result-insert` | KDS `group` | 204.8 µs | 180.7 µs | 191.2 µs | 194.2 µs | 241.3 µs | 327.4 µs |
| `result-insert` | PostgreSQL | 171.5 µs | 134.0 µs | 144.9 µs | 151.4 µs | 261.2 µs | 489.4 µs |
| `backtest-replay` | KDS `group` | 727.0 µs | 689.8 µs | 723.7 µs | 731.7 µs | 741.4 µs | 759.7 µs |
| `backtest-replay` | PostgreSQL | 1,352.7 µs | 1,231.2 µs | 1,287.0 µs | 1,309.6 µs | 1,519.8 µs | 1,555.3 µs |
| `compare-all` | KDS `group` | 8,655.5 µs | 8,655.5 µs | 8,655.5 µs | 8,655.5 µs | 8,655.5 µs | 8,655.5 µs |
| `compare-all` | PostgreSQL | 10,794.4 µs | 10,794.4 µs | 10,794.4 µs | 10,794.4 µs | 10,794.4 µs | 10,794.4 µs |
| `read-bar-lookup` | KDS `group` | 27.2 µs | 22.5 µs | 23.5 µs | 24.0 µs | 25.4 µs | 187.9 µs |
| `read-bar-lookup` | PostgreSQL | 28.8 µs | 23.0 µs | 24.0 µs | 24.4 µs | 27.8 µs | 191.6 µs |
| `read-bar-range` | KDS `group` | 593.9 µs | 515.8 µs | 531.0 µs | 622.5 µs | 674.8 µs | 684.8 µs |
| `read-bar-range` | PostgreSQL | 579.8 µs | 462.7 µs | 579.1 µs | 601.9 µs | 655.1 µs | 682.3 µs |
| `read-symbol-history` | KDS `group` | 24,468.5 µs | 23,614.5 µs | 23,854.8 µs | 24,075.4 µs | 25,505.4 µs | 25,505.4 µs |
| `read-symbol-history` | PostgreSQL | 21,443.2 µs | 20,630.1 µs | 20,996.3 µs | 21,248.1 µs | 22,247.8 µs | 22,247.8 µs |
| `read-day-slice` | KDS `group` | 724.9 µs | 661.8 µs | 687.5 µs | 707.5 µs | 826.8 µs | 1,463.3 µs |
| `read-day-slice` | PostgreSQL | 1,286.1 µs | 1,145.8 µs | 1,218.9 µs | 1,242.7 µs | 1,454.2 µs | 1,593.5 µs |
| `read-join-point` | KDS `group` | 40.5 µs | 30.0 µs | 39.6 µs | 39.9 µs | 43.6 µs | 49.5 µs |
| `read-join-point` | PostgreSQL | 70.9 µs | 53.7 µs | 56.4 µs | 59.0 µs | 82.2 µs | 286.0 µs |
| `read-join-exists` | KDS `group` | 2,075.6 µs | 1,931.1 µs | 1,941.2 µs | 1,970.6 µs | 2,637.9 µs | 2,637.9 µs |
| `read-join-exists` | PostgreSQL | 4,311.6 µs | 4,120.3 µs | 4,270.1 µs | 4,305.5 µs | 4,515.0 µs | 4,515.0 µs |
| `agg-global` | KDS `group` | 2,471.2 µs | 2,452.5 µs | 2,458.8 µs | 2,465.4 µs | 2,509.1 µs | 2,509.1 µs |
| `agg-global` | PostgreSQL | 2,382.5 µs | 2,252.8 µs | 2,358.6 µs | 2,369.7 µs | 2,470.1 µs | 2,470.1 µs |
| `agg-by-symbol` | KDS `group` | 3,790.2 µs | 3,468.0 µs | 3,749.6 µs | 3,789.8 µs | 4,039.8 µs | 4,039.8 µs |
| `agg-by-symbol` | PostgreSQL | 3,660.6 µs | 3,517.6 µs | 3,632.7 µs | 3,684.1 µs | 3,765.5 µs | 3,765.5 µs |
| `agg-by-session` | KDS `group` | 13,681.2 µs | 12,538.1 µs | 12,562.1 µs | 12,587.5 µs | 20,081.0 µs | 20,081.0 µs |
| `agg-by-session` | PostgreSQL | 12,482.6 µs | 11,766.5 µs | 11,843.7 µs | 11,915.3 µs | 17,181.2 µs | 17,181.2 µs |
| `agg-day-slice` | KDS `group` | 708.9 µs | 633.9 µs | 662.0 µs | 686.3 µs | 924.4 µs | 1,023.5 µs |
| `agg-day-slice` | PostgreSQL | 1,261.6 µs | 1,137.7 µs | 1,188.8 µs | 1,219.6 µs | 1,469.2 µs | 1,623.9 µs |
| `agg-distinct` | KDS `group` | 2,707.2 µs | 2,669.6 µs | 2,673.5 µs | 2,687.8 µs | 2,795.9 µs | 2,795.9 µs |
| `agg-distinct` | PostgreSQL | 2,116.2 µs | 1,986.3 µs | 2,021.7 µs | 2,050.2 µs | 2,368.3 µs | 2,368.3 µs |

### 60,480 bars, KDS `strict`

| phase | ops a run | KDS mean (spread) | PostgreSQL mean (spread) | KDS rate | PG rate | KDS / PG | read |
|---|---|---|---|---|---|---|---|
| `load-bars` | 60,480 ops | 19.4 µs (1.0 %) | 26.2 µs (0.8 %) | 51,546 qps | 38,168 qps | **1.35x** | KDS ahead |
| `load-stats` | 60,480 ops | 18.6 µs (1.6 %) | 26.1 µs (0.4 %) | 53,763 qps | 38,314 qps | **1.40x** | KDS ahead |
| `backtest-read` | 360 ops | 753.7 µs (2.5 %) | 1,285.0 µs (2.2 %) | 1,327 qps | 778 qps | **1.70x** | KDS ahead |
| `result-insert` | 2,872 ops | 210.5 µs (12.4 %) | 171.5 µs (7.3 %) | 4,751 qps | 5,831 qps | **0.81x** | PostgreSQL ahead |
| `backtest-replay` | 360 ops | 739.8 µs (3.9 %) | 1,352.7 µs (4.3 %) | 1,352 qps | 739 qps | **1.83x** | KDS ahead |
| `compare-all` | 1 ops | 9,847.9 µs (24.8 %) | 10,794.4 µs (12.1 %) | 102 qps | 93 qps | **1.10x** | no claim |
| `read-bar-lookup` | 200 ops | 24.9 µs (5.6 %) | 28.8 µs (9.7 %) | 40,161 qps | 34,722 qps | **1.16x** | KDS ahead |
| `read-bar-range` | 200 ops | 588.7 µs (5.2 %) | 579.8 µs (1.2 %) | 1,699 qps | 1,725 qps | **0.98x** | inside the spread |
| `read-symbol-history` | 10 ops | 24,432.5 µs (0.6 %) | 21,443.2 µs (1.8 %) | 41 qps | 47 qps | **0.88x** | PostgreSQL ahead |
| `read-day-slice` | 200 ops | 726.6 µs (3.6 %) | 1,286.1 µs (3.1 %) | 1,376 qps | 778 qps | **1.77x** | KDS ahead |
| `read-join-point` | 200 ops | 40.8 µs (7.6 %) | 70.9 µs (8.6 %) | 24,510 qps | 14,104 qps | **1.74x** | KDS ahead |
| `read-join-exists` | 10 ops | 2,061.4 µs (7.2 %) | 4,311.6 µs (1.2 %) | 485 qps | 232 qps | **2.09x** | KDS ahead |
| `agg-global` | 10 ops | 2,716.0 µs (5.4 %) | 2,382.5 µs (5.1 %) | 368 qps | 420 qps | **0.88x** | PostgreSQL ahead |
| `agg-by-symbol` | 10 ops | 3,912.5 µs (0.8 %) | 3,660.6 µs (1.7 %) | 256 qps | 273 qps | **0.94x** | PostgreSQL ahead |
| `agg-by-session` | 10 ops | 13,697.8 µs (4.7 %) | 12,482.6 µs (2.2 %) | 73 qps | 80 qps | **0.91x** | PostgreSQL ahead |
| `agg-day-slice` | 200 ops | 725.9 µs (2.1 %) | 1,261.6 µs (4.1 %) | 1,378 qps | 793 qps | **1.74x** | KDS ahead |
| `agg-distinct` | 10 ops | 2,784.2 µs (6.7 %) | 2,116.2 µs (11.4 %) | 359 qps | 473 qps | **0.76x** | PostgreSQL ahead |

### 252 bars, KDS `group`

| phase | ops a run | KDS mean (spread) | PostgreSQL mean (spread) | KDS rate | PG rate | KDS / PG | read |
|---|---|---|---|---|---|---|---|
| `load-bars` | 252 ops | 21.4 µs (22.0 %) | 27.1 µs (4.1 %) | 46,729 qps | 36,900 qps | **1.27x** | no claim |
| `load-stats` | 252 ops | 19.8 µs (17.2 %) | 25.9 µs (1.9 %) | 50,505 qps | 38,610 qps | **1.31x** | KDS ahead |
| `backtest-read` | 12 ops | 78.9 µs (32.2 %) | 114.5 µs (11.0 %) | 12,674 qps | 8,734 qps | **1.45x** | no claim |
| `result-insert` | 88 ops | 140.1 µs (11.2 %) | 145.0 µs (137.9 %) | 7,138 qps | 6,897 qps | **1.03x** | no claim |
| `backtest-replay` | 12 ops | 46.7 µs (13.1 %) | 74.7 µs (15.5 %) | 21,413 qps | 13,387 qps | **1.60x** | KDS ahead |
| `compare-all` | 1 ops | 268.1 µs (2.4 %) | 270.8 µs (11.0 %) | 3,730 qps | 3,693 qps | **1.01x** | inside the spread |
| `read-bar-lookup` | 200 ops | 24.0 µs (6.7 %) | 28.6 µs (39.2 %) | 41,667 qps | 34,965 qps | **1.19x** | no claim |
| `read-bar-range` | 200 ops | 595.7 µs (4.6 %) | 602.2 µs (3.7 %) | 1,679 qps | 1,661 qps | **1.01x** | inside the spread |
| `read-symbol-history` | 10 ops | 886.2 µs (5.1 %) | 825.6 µs (6.3 %) | 1,128 qps | 1,211 qps | **0.93x** | PostgreSQL ahead |
| `read-day-slice` | 200 ops | 30.0 µs (11.0 %) | 30.2 µs (4.0 %) | 33,333 qps | 33,113 qps | **1.01x** | inside the spread |
| `read-join-point` | 200 ops | 25.9 µs (1.9 %) | 45.7 µs (1.3 %) | 38,610 qps | 21,882 qps | **1.76x** | KDS ahead |
| `read-join-exists` | 10 ops | 26.0 µs (1.9 %) | 31.5 µs (5.1 %) | 38,462 qps | 31,746 qps | **1.21x** | KDS ahead |
| `agg-global` | 10 ops | 35.4 µs (2.5 %) | 42.8 µs (3.3 %) | 28,249 qps | 23,364 qps | **1.21x** | KDS ahead |
| `agg-by-symbol` | 10 ops | 39.0 µs (1.8 %) | 41.8 µs (2.4 %) | 25,641 qps | 23,923 qps | **1.07x** | KDS ahead |
| `agg-by-session` | 10 ops | 313.6 µs (11.8 %) | 426.0 µs (18.6 %) | 3,189 qps | 2,347 qps | **1.36x** | KDS ahead |
| `agg-day-slice` | 200 ops | 25.7 µs (1.9 %) | 30.9 µs (4.5 %) | 38,911 qps | 32,362 qps | **1.20x** | KDS ahead |
| `agg-distinct` | 10 ops | 33.1 µs (2.7 %) | 32.6 µs (2.1 %) | 30,211 qps | 30,675 qps | **0.98x** | inside the spread |

### 1,008 bars, KDS `group`

| phase | ops a run | KDS mean (spread) | PostgreSQL mean (spread) | KDS rate | PG rate | KDS / PG | read |
|---|---|---|---|---|---|---|---|
| `load-bars` | 1,008 ops | 21.3 µs (11.7 %) | 28.8 µs (3.5 %) | 46,948 qps | 34,722 qps | **1.35x** | KDS ahead |
| `load-stats` | 1,008 ops | 19.2 µs (7.3 %) | 27.2 µs (2.9 %) | 52,083 qps | 36,765 qps | **1.42x** | KDS ahead |
| `backtest-read` | 12 ops | 103.0 µs (8.4 %) | 162.5 µs (3.3 %) | 9,709 qps | 6,154 qps | **1.58x** | KDS ahead |
| `result-insert` | 88 ops | 138.6 µs (49.4 %) | 144.0 µs (6.1 %) | 7,215 qps | 6,944 qps | **1.04x** | no claim |
| `backtest-replay` | 12 ops | 67.6 µs (3.8 %) | 119.6 µs (2.3 %) | 14,793 qps | 8,361 qps | **1.77x** | KDS ahead |
| `compare-all` | 1 ops | 256.0 µs (0.5 %) | 264.9 µs (3.1 %) | 3,906 qps | 3,775 qps | **1.03x** | KDS ahead |
| `read-bar-lookup` | 200 ops | 24.3 µs (1.6 %) | 24.0 µs (4.6 %) | 41,152 qps | 41,667 qps | **0.99x** | inside the spread |
| `read-bar-range` | 200 ops | 612.8 µs (1.1 %) | 585.8 µs (1.4 %) | 1,632 qps | 1,707 qps | **0.96x** | PostgreSQL ahead |
| `read-symbol-history` | 10 ops | 940.6 µs (2.8 %) | 827.7 µs (1.6 %) | 1,063 qps | 1,208 qps | **0.88x** | PostgreSQL ahead |
| `read-day-slice` | 200 ops | 50.6 µs (5.9 %) | 59.0 µs (2.2 %) | 19,763 qps | 16,949 qps | **1.17x** | KDS ahead |
| `read-join-point` | 200 ops | 26.5 µs (17.0 %) | 49.4 µs (4.7 %) | 37,736 qps | 20,243 qps | **1.86x** | KDS ahead |
| `read-join-exists` | 10 ops | 59.0 µs (1.2 %) | 68.9 µs (2.2 %) | 16,949 qps | 14,514 qps | **1.17x** | KDS ahead |
| `agg-global` | 10 ops | 66.5 µs (2.9 %) | 66.3 µs (1.7 %) | 15,038 qps | 15,083 qps | **1.00x** | inside the spread |
| `agg-by-symbol` | 10 ops | 87.7 µs (2.7 %) | 86.9 µs (16.5 %) | 11,403 qps | 11,507 qps | **0.99x** | inside the spread |
| `agg-by-session` | 10 ops | 358.7 µs (10.6 %) | 445.9 µs (7.2 %) | 2,788 qps | 2,243 qps | **1.24x** | KDS ahead |
| `agg-day-slice` | 200 ops | 37.2 µs (5.4 %) | 50.2 µs (4.4 %) | 26,882 qps | 19,920 qps | **1.35x** | KDS ahead |
| `agg-distinct` | 10 ops | 67.8 µs (2.8 %) | 56.6 µs (2.1 %) | 14,749 qps | 17,668 qps | **0.83x** | PostgreSQL ahead |

### 10,080 bars, KDS `group`

| phase | ops a run | KDS mean (spread) | PostgreSQL mean (spread) | KDS rate | PG rate | KDS / PG | read |
|---|---|---|---|---|---|---|---|
| `load-bars` | 10,080 ops | 22.2 µs (2.3 %) | 29.5 µs (3.4 %) | 45,045 qps | 33,898 qps | **1.33x** | KDS ahead |
| `load-stats` | 10,080 ops | 18.8 µs (3.7 %) | 26.0 µs (4.6 %) | 53,191 qps | 38,462 qps | **1.38x** | KDS ahead |
| `backtest-read` | 60 ops | 191.3 µs (49.3 %) | 313.8 µs (4.5 %) | 5,227 qps | 3,187 qps | **1.64x** | no claim |
| `result-insert` | 472 ops | 142.6 µs (125.8 %) | 147.9 µs (3.0 %) | 7,013 qps | 6,761 qps | **1.04x** | no claim |
| `backtest-replay` | 60 ops | 164.2 µs (21.9 %) | 275.2 µs (2.7 %) | 6,090 qps | 3,634 qps | **1.68x** | no claim |
| `compare-all` | 1 ops | 1,087.6 µs (1.9 %) | 991.9 µs (9.5 %) | 919 qps | 1,008 qps | **0.91x** | inside the spread |
| `read-bar-lookup` | 200 ops | 23.6 µs (3.8 %) | 23.9 µs (0.8 %) | 42,373 qps | 41,841 qps | **1.01x** | inside the spread |
| `read-bar-range` | 200 ops | 583.2 µs (3.5 %) | 593.8 µs (3.2 %) | 1,715 qps | 1,684 qps | **1.02x** | inside the spread |
| `read-symbol-history` | 10 ops | 4,169.3 µs (3.8 %) | 3,594.3 µs (2.0 %) | 240 qps | 278 qps | **0.86x** | PostgreSQL ahead |
| `read-day-slice` | 200 ops | 165.3 µs (0.5 %) | 238.2 µs (19.5 %) | 6,050 qps | 4,198 qps | **1.44x** | KDS ahead |
| `read-join-point` | 200 ops | 26.7 µs (3.4 %) | 49.3 µs (1.4 %) | 37,453 qps | 20,284 qps | **1.85x** | KDS ahead |
| `read-join-exists` | 10 ops | 356.1 µs (1.2 %) | 705.5 µs (3.8 %) | 2,808 qps | 1,417 qps | **1.98x** | KDS ahead |
| `agg-global` | 10 ops | 428.3 µs (3.5 %) | 348.3 µs (0.5 %) | 2,335 qps | 2,871 qps | **0.81x** | PostgreSQL ahead |
| `agg-by-symbol` | 10 ops | 604.9 µs (1.1 %) | 573.1 µs (1.5 %) | 1,653 qps | 1,745 qps | **0.95x** | PostgreSQL ahead |
| `agg-by-session` | 10 ops | 2,305.7 µs (1.4 %) | 2,214.0 µs (6.3 %) | 434 qps | 452 qps | **0.96x** | inside the spread |
| `agg-day-slice` | 200 ops | 141.0 µs (8.5 %) | 217.9 µs (2.3 %) | 7,092 qps | 4,589 qps | **1.55x** | KDS ahead |
| `agg-distinct` | 10 ops | 471.1 µs (2.5 %) | 344.1 µs (2.1 %) | 2,123 qps | 2,906 qps | **0.73x** | PostgreSQL ahead |

## Tally of the verdicts

| scenario | KDS ahead | inside the spread | PostgreSQL ahead | no claim |
|---|---|---|---|---|
| 4 | 9 | 3 | 1 | 8 |
| 2 | 10 | 9 | 2 | 5 |
| 3 | 36 | 15 | 2 | 19 |
| 1 | 42 | 16 | 17 | 10 |

PostgreSQL ahead:

- scenario 4, `close`, on: 0.70x
- scenario 2, `cargo-lookup`, s: 0.97x
- scenario 2, `manifest-scan`, g: 0.46x
- scenario 3, `books-by-genre`, 200-none: 0.96x
- scenario 3, `books-by-genre`, 10000-all: 0.88x
- scenario 1, `read-symbol-history`, 60,480 bars, KDS `group`: 0.88x
- scenario 1, `agg-by-session`, 60,480 bars, KDS `group`: 0.91x
- scenario 1, `agg-distinct`, 60,480 bars, KDS `group`: 0.78x
- scenario 1, `result-insert`, 60,480 bars, KDS `strict`: 0.81x
- scenario 1, `read-symbol-history`, 60,480 bars, KDS `strict`: 0.88x
- scenario 1, `agg-global`, 60,480 bars, KDS `strict`: 0.88x
- scenario 1, `agg-by-symbol`, 60,480 bars, KDS `strict`: 0.94x
- scenario 1, `agg-by-session`, 60,480 bars, KDS `strict`: 0.91x
- scenario 1, `agg-distinct`, 60,480 bars, KDS `strict`: 0.76x
- scenario 1, `read-symbol-history`, 252 bars, KDS `group`: 0.93x
- scenario 1, `read-bar-range`, 1,008 bars, KDS `group`: 0.96x
- scenario 1, `read-symbol-history`, 1,008 bars, KDS `group`: 0.88x
- scenario 1, `agg-distinct`, 1,008 bars, KDS `group`: 0.83x
- scenario 1, `read-symbol-history`, 10,080 bars, KDS `group`: 0.86x
- scenario 1, `agg-global`, 10,080 bars, KDS `group`: 0.81x
- scenario 1, `agg-by-symbol`, 10,080 bars, KDS `group`: 0.95x
- scenario 1, `agg-distinct`, 10,080 bars, KDS `group`: 0.73x

Errors over every phase of every archived cell: 0.

## What the run teaches

1. **On its twins' client modes the engine is behind PostgreSQL on no short statement by more than the client CPU that is still unequal**: two short reads read PostgreSQL ahead, by 0.7 µs and 0.8 µs (`cargo-lookup` at `strict`, `books-by-genre` over 200 unindexed loans), under the 2.9 µs a decoded read still costs the KDS client, and no short write is behind. The comparison file's "0.73x to 0.76x on a short read" and "0.84x on a booking" were the KDS Python client: 16.6 µs a statement through `send_command` (EF-S0), gone through `fetch`.
2. **The booking is at parity because its reads are, and its writes are ahead**: a `freight-insert` is 21.4 µs against 29.6 µs (the twin's decodes its `RETURNING` id, under 1 µs of that), the updates and `charge-insert` 1.07x to 1.10x, and the commit 134.2 µs against 141.1 µs.
3. **KDS's lead grows with the rows a statement examines and shrinks with the rows it returns.** Scenario 3's geometric mean rises from 1.09x at 200 loans to 1.42x at 10,000 unindexed; `read-symbol-history`, 7,560 returned rows, is 0.88x through a pair that renders, where two counting clients read the same shape 1.50x (EF-S0).
4. **Folds over a whole relation are the engine's deficit**, and they are smaller than the scenario files said: `close` 0.70x on the `on` arm (0.58x and 0.82x on the two arms whose spread carries no claim), `agg-distinct` 0.73x to 0.83x from 1,008 bars up, `agg-global` 0.81x at 10,080 bars and 0.88x at 60,480 `strict` (inside the spread at `group`, ahead at 252 bars), `agg-by-session` 0.91x.
5. **Scenario 1's autocommit insert is behind and nothing here says why**: `result-insert` is 210.5 µs against 171.5 µs at `strict` (0.81x) and unresolved at `group` (a 98.6 % spread), while scenario 2's autocommit inserts are ahead. **It is the 60,480-bar cell's alone**: at 252, 1,008 and 10,080 bars the same statement is 138.6 µs to 142.6 µs against 144.0 µs to 147.9 µs (no claim, KDS's or PostgreSQL's spread over 20 % in each), so its cost rises with the data on both engines and by more on KDS. It is EG-S0's question.
6. **A two-statement manifest reporter is the one place the drivers differ by design**: `manifest-scan` walks on KDS and reads an index on PostgreSQL (0.46x to 0.49x, 40 operations, mostly without a claim).

## What is cut, absent or unverified

- **No percentile table for four of scenario 3's six cells and for scenario 1's sweep sizes**: the 10,000-loan cells and the 60,480-bar `group` cell carry them, the rest are in the JSON.
- **Scenario 4's days 2 and 3, and its `on` and `declared` arms beyond day 1's table**, are in the JSON and not tabulated; PostgreSQL's twin has one day.
- **The three runs are consecutive, not one**: scenario 4 was measured 10 to 30 minutes before scenarios 2 and 3, and scenario 1 after them, each beside its own PostgreSQL cells.
- **Still unequal** (EF-S1): a decoded read charges KDS about 2.9 µs more client CPU a statement, not attributed; a rendered row costs the KDS client a typed render PostgreSQL's does not do.
- **No server CPU**: these are the scenario drivers, which read none. EF-S0's parity passes carry it for seven shapes.
- **`result-insert` at `group`, `open`, the 200-row tape and `pk-user`** have a spread over 20 % on one side and carry no claim.
- **`ops a run` is KDS's.** From the same seed scenario 2's twin rejects fewer bookings (289 on capacity and 51 on credit against 328 and 74, in every run of each engine), so its reads are 3,340 a run against 3,402 and its `charge-insert` 16,889 against 16,907, and its manifest reporter makes one pass (20, 20 and 10 ops) against KDS's two. Scenarios 1, 3 and 4 run the same counts on both sides.
- **No scenario 4 run with an index on `symbol` on PostgreSQL**, the twin's known gap.

## Archive

`bench/v3.5.1/archive/ef-s2-v3.5.1-6-g72580aa04/`: `run-a/`, `run-b/`, `run-c/`, each with `json/` (every cell), `logs/` (each cell's stdout, return code, precheck and server configuration; the PostgreSQL version, affinity and server log), `run.sh`, the watchdog and its log, the timeline, the device, the binary's hash and the hashes of the driver copies that ran.
