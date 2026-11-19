# Scenario 1 (backtest) at `v3.4.14`

**One line.** On a new host the same driver reads 12 % slower on the many-row shapes and 30 % faster on the scans than the recorded `v3.3.0-38-ga40882af` numbers, and **an interleaved A/B of the old engine against this one on this host says the engine is responsible for none of the first and for 6 % to 11 % of the second**: the pk and join reads, the writes and the loads are 0.97x to 1.02x of the old binary (inside their spreads), and the four walk-bound shapes (`day-slice`, `cross-join`, `symbol-history`, `backtest-read`) are 1.06x to 1.11x faster. The recorded-number deltas below are therefore mostly a host effect and are labelled so; the A/B (section 9) is the reading of the engine.

Driver: `tools/scenario1_backtest.py` from the `35086008` extraction, unmodified, `--seed 1 --bars-clustered btree --verify --sync --analyze` (no `bench/docs/` entry is needed beyond the driver's own `--help`), run by `ck-tester`.
Version measured: **`v3.4.14` = `35086008`** (`git describe --tags 35086008` = `v3.4.14`). **`origin/main` is ahead of this tag** (`12887a14`, `v3.4.14-22-g12887a14`): nothing after the tag is in this measurement.
Comparator (this engine's own last run for this shape): `bench/v3.3.0/results-scenario1-backtest-v3.3.0-38-ga40882af.md`, measured on **another host** (see the stamp), so every delta against it is engine plus host; section 9 separates them with a same-host A/B.
PostgreSQL (the floor, rule 4): **not measured for this run** - this host has the PostgreSQL 17 client tools and no server (`pg_ctl` and `initdb` are absent; the twin was started and every cell exited 1 with "connection refused", no result was written). The PostgreSQL column is the v3.3.0-33 run's PostgreSQL 18.6 on the other host, shown for the league question only; a ratio against it carries the host difference.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-11-19; matrix (9 cells) 18:58:08 to 19:03:29 UTC; size sweep (9 cells) 19:03:29 to 19:04:41 UTC; engine A/B (6 cells of this scenario) inside 19:14:53 to 19:22:28 UTC. Per-cell precheck (load average, process scan, gate wait) is in the archive, `logs/<cell>.precheck` |
| Branch / commit | extraction of tag `v3.4.14` = `35086008` (`git archive 35086008`) under `/home/louis/bench-runs/scenarios-v3.4.14/src/`; no worktree, no branch; the result files were written in the worktree `scenarios-v3-4-14` (branch `worktree-scenarios-v3-4-14`), which is not the measured tree. **`origin/main` (`12887a14`, `v3.4.14-22-g12887a14`) is ahead of the tag** |
| Tree cleanliness | an extraction has no working tree to be dirty; the drivers are the unmodified `tools/` of the extraction |
| Binary provenance | built Release from the extraction (`cmake -DCMAKE_BUILD_TYPE=Release`, `build-release/CMakeCache.txt` says `Release`), 18:55:35 to 18:56:53 UTC; source binary mtime 2026-11-19 18:56:53.09 UTC, built from the commit measured, so it is not older than HEAD. **Measured a copy**: `kds_server`, **sha256 `78b3aede97192d165094e5051a80958b026d36e2bb92d7a13a394412399d3fb3`**; every server started from the copy |
| A/B arm binary | `kds_server_old`, built Release from `git archive a40882af` (the commit `v3.3.0-38-ga40882af` names), **sha256 `42041f0b8d681dcffeb271495b1626e06b9b4feb39d5561a7615619845d1e2a8`**, built 19:13:47 to 19:14:53 UTC, driven by the **same** `35086008` drivers |
| Device | data files under `/home/louis/bench-runs/scenarios-v3.4.14/data/<cell>/`, **`/dev/nvme0n1p1`, ext4** (`df -T`, 2026-11-19 18:55 UTC; `/` 56 % used); never tmpfs; a fresh server and data file per cell, deleted afterwards. The binaries live in the same directory |
| Build type | Release |
| Host | 8 logical CPUs, Intel Xeon Platinum 8581C @ 2.10 GHz, 31 GiB, Linux 6.12.116 (`6.12.116+deb13-cloud-amd64`), a shared development VM; one unrelated `kds_server` (a sandbox under `build-sandbox`) was up throughout and idle. **The v3.3.0-38 run was on an AMD EPYC 9V74, Linux 7.0.0-1014-azure, another device** |
| Server config | `cores` and `durability` per cell (`c1-g` = 1 core `group`, `c8-g` = 8 cores `group`, `c1-s` = 1 core `strict`), `log_level = warn`, `buffer_pool_frames = 65536`, all else default; port 15610, never the default; one server per cell |
| Quiet host | before each cell a gate waited for no `cc1plus` / `cmake --build` / `ctest` / `kds_tests` process and at most 8 % busy CPU over 3 s: every gate wait was 0 s. A 1-second monitor ran throughout (`monitor.log`): the largest foreign sample was 27.7 % of one CPU (this session's own `claude` process), none at 50 % or more; no cell was discarded |
| 60 s watchdog | `watchdog.sh` / `watchdog2.sh` poll a status file's age and a recorded PID, never a process pattern; none found an orphan or a stall |
| Time budget | about 1 hour for everything including the build (the build 1.5 min); what was cut is in section 2 |

## 2. What was run

| cell | cores | durability | runs | arguments |
|---|---|---|---|---|
| `s1-c1-g`, `r2-`, `r3-` | 1 | group | 3 | `--seed 1 --bars-clustered btree --verify --sync --analyze` (60,480 bars) |
| `s1-c8-g`, `r2-`, `r3-` | 8 | group | 3 | same |
| `s1-c1-s`, `r2-`, `r3-` | 1 | strict | 3 | same |
| `s1s-n252-c1-g`, `s1s-n1008-c1-g`, `s1s-n10080-c1-g` (+ `r2-`, `r3-`) | 1 | group | 3 each | same plus `--years 1 --symbols 1 --exchanges 1` (252 bars), `--years 1 --symbols 4 --exchanges 2` (1,008), `--years 5 --symbols 8 --exchanges 2` (10,080) |
| `ab-old{1,2,3}-s1-c1-g`, `ab-new{1,2,3}-s1-c1-g` | 1 | group | 3 pairs, interleaved old, new, old, new, old, new | the `s1-c1-g` arguments; `old` = the `a40882af` binary, `new` = the measured binary, one driver |

**Row-count axis (rule 9).** Bars = 252 sessions per year x years x symbols; `daily_stats` has one row per bar; the flags above give 252, 1,008, 10,080 and (default `--years 30 --symbols 8`) 60,480 bars. A returned-row count is fixed per shape: `bar-range` 200 rows, `symbol-history` one symbol's sessions (252, 252, 1,260 and 7,560 rows at the four sizes), `model-join` 359, `day-slice` 8.

**Cut, and why.** The v3.3.0-38 run's join-order probe (`join_order = planned` against `written`, six cells) and its old-client A/B were not repeated: the client is unchanged between the two versions (`git diff a40882af 35086008 -- tools/kwp.py` is a two-line error-code addition, the scenario-1 driver differs only in a printed note), so the client A/B has nothing to separate, and the engine A/B above replaced it. The PostgreSQL twin was not run (no server on this host). Every cell of the matrix and the size sweep ran with its three repeats.

## 3. Results

The driver's controlled sweep (100 statements per cell, 8 distinct arguments in the warm cases), median of three runs. `cold` = every argument seen for the first time, `warm` = the same few arguments cycled, `cabin` = warm with a Cabin declared on the filter column. **Read** applies the rule of `bench/v3.3.0/summary-scenarios-v3.3.0-38-ga40882af.md` ("What counts as a finding"): a delta smaller than the larger of the two spreads is not a finding and a spread above 20 % carries no claim. **Every delta in the tables against `v3.3.0-38` is engine plus host (another CPU, kernel and device); only section 9's A/B reads the engine.**

#### cell c1-g: cores = 1, group

| shape | cold now | warm now | spread | warm v3.3.0-38 | spread | warm delta | read | cabin now | cabin v3.3.0-38 | cabin delta |
|---|---|---|---|---|---|---|---|---|---|---|
| bar-lookup | 13,166.0 qps | 13,605.2 qps | 1.8 % | 12,619.9 qps | 29.0 % | +7.8 % | spread > 20 %, no claim | - | - | - |
| bar-range | 1,077.5 qps | 1,114.0 qps | 2.9 % | 1,260.3 qps | 4.3 % | -11.6 % | **finding** | - | - | - |
| day-slice | 659.0 qps | 668.7 qps | 3.9 % | 508.7 qps | 18.6 % | +31.5 % | **finding** | 7,811.8 qps | 8,109.3 qps | -3.7 % |
| symbol-history | 27.2 qps | 27.1 qps | 1.5 % | 27.3 qps | 0.7 % | -0.7 % | inside the spread | 26.5 qps | 26.2 qps | +1.1 % |
| cross-join | 628.7 qps | 650.8 qps | 7.3 % | 496.9 qps | 20.6 % | +31.0 % | spread > 20 %, no claim | 6,301.8 qps | 6,722.3 qps | -6.3 % |
| point-join | 13,285.1 qps | 13,692.0 qps | 7.0 % | 10,589.7 qps | 37.8 % | +29.3 % | spread > 20 %, no claim | - | - | - |
| model-join | 902.5 qps | 905.4 qps | 4.9 % | 1,030.2 qps | 3.4 % | -12.1 % | **finding** | 897.6 qps | 1,041.0 qps | -13.8 % |

#### cell c8-g: cores = 8, group

| shape | cold now | warm now | spread | warm v3.3.0-38 | spread | warm delta | read | cabin now | cabin v3.3.0-38 | cabin delta |
|---|---|---|---|---|---|---|---|---|---|---|
| bar-lookup | 13,064.0 qps | 13,400.3 qps | 6.0 % | 12,463.9 qps | 2.8 % | +7.5 % | **finding** | - | - | - |
| bar-range | 1,086.0 qps | 1,113.4 qps | 2.8 % | 1,223.2 qps | 2.8 % | -9.0 % | **finding** | - | - | - |
| day-slice | 616.5 qps | 623.6 qps | 2.2 % | 480.6 qps | 18.8 % | +29.8 % | **finding** | 7,578.1 qps | 7,875.8 qps | -3.8 % |
| symbol-history | 26.9 qps | 26.8 qps | 0.7 % | 26.7 qps | 0.4 % | +0.4 % | inside the spread | 26.0 qps | 25.5 qps | +2.0 % |
| cross-join | 590.2 qps | 593.9 qps | 1.3 % | 472.8 qps | 17.6 % | +25.6 % | **finding** | 6,270.1 qps | 6,360.4 qps | -1.4 % |
| point-join | 12,723.0 qps | 13,514.6 qps | 6.9 % | 11,908.3 qps | 0.6 % | +13.5 % | **finding** | - | - | - |
| model-join | 812.8 qps | 833.0 qps | 5.4 % | 991.4 qps | 2.0 % | -16.0 % | **finding** | 819.9 qps | 1,001.1 qps | -18.1 % |

#### cell c1-s: cores = 1, strict

| shape | cold now | warm now | spread | warm v3.3.0-38 | spread | warm delta | read | cabin now | cabin v3.3.0-38 | cabin delta |
|---|---|---|---|---|---|---|---|---|---|---|
| bar-lookup | 13,449.1 qps | 13,169.9 qps | 5.7 % | 13,025.9 qps | 32.7 % | +1.1 % | spread > 20 %, no claim | - | - | - |
| bar-range | 1,141.0 qps | 1,142.3 qps | 4.9 % | 1,224.3 qps | 9.8 % | -6.7 % | inside the spread | - | - | - |
| day-slice | 679.9 qps | 671.5 qps | 4.4 % | 504.8 qps | 1.6 % | +33.0 % | **finding** | 8,064.8 qps | 8,030.8 qps | +0.4 % |
| symbol-history | 27.3 qps | 27.4 qps | 2.9 % | 27.1 qps | 8.1 % | +1.1 % | inside the spread | 26.4 qps | 26.2 qps | +0.8 % |
| cross-join | 660.1 qps | 663.4 qps | 5.2 % | 486.7 qps | 2.7 % | +36.3 % | **finding** | 6,533.5 qps | 6,324.6 qps | +3.3 % |
| point-join | 13,358.7 qps | 13,737.7 qps | 7.7 % | 9,020.4 qps | 43.6 % | +52.3 % | spread > 20 %, no claim | - | - | - |
| model-join | 926.0 qps | 905.1 qps | 3.1 % | 1,010.9 qps | 7.8 % | -10.5 % | **finding** | 914.6 qps | 1,035.1 qps | -11.6 % |

**The deltas carry both signs by workload, which is the signature of a host and not of an engine.** The shapes that return many rows (`bar-range`, `model-join`) are 7 % to 16 % *below* the recorded numbers in all three cells (the 7 % is inside its spread), the shapes that walk many rows and return few (`day-slice`, `cross-join`) are 26 % to 36 % *above* them, and the one-row reads sit between (`bar-lookup` +1 % to +8 %). A faster Xeon core scanning and a slower 2.1 GHz Python client decoding returned rows give exactly that pattern, and section 5 shows it in per-row costs. The Cabin arms (8 rows by a cache hit) are -6 % to +3 % except `model-join`, which does not use one (-12 % to -18 %, the same as its warm arm).
`bar-lookup` and `point-join` carry spreads of 1.8 % to 8 % here and are steady, where `v3.3.0-38` recorded them bimodal (29 % to 44 %); on this host the 60,480-bar matrix cells did not show it (3 runs each) but the 252- and 1,008-bar size cells (section 7) and the A/B (8 % to 16 % on them) do, so it is rarer here, not gone.

Every phase of the driver, median over the three `c1-g` runs (qps = the driver's own ops over elapsed; the PostgreSQL column is the other-host PostgreSQL 18.6):

| phase | ops | now | spread | v3.3.0-38 | spread | delta | read | PG 18.6 (v3.3.0-33 run) | KDS/PG now | KDS/PG v3.3.0-38 |
|---|---|---|---|---|---|---|---|---|---|---|
| load-exchanges | 2 ops | 1,858.9 qps | 9.0 % | 819.4 qps | 17.2 % | +126.9 % | **finding** | 700.0 qps | 2.66x | 1.17x |
| load-symbols | 8 ops | 2,153.4 qps | 13.9 % | 850.0 qps | 4.2 % | +153.3 % | **finding** | 806.7 qps | 2.67x | 1.05x |
| load-sessions | 7,560 ops | 23,826.3 qps | 5.9 % | 21,905.7 qps | 2.0 % | +8.8 % | **finding** | 13,292.7 qps | 1.79x | 1.65x |
| load-bars | 60,480 ops | 22,397.4 qps | 6.0 % | 19,626.3 qps | 1.8 % | +14.1 % | **finding** | 13,336.9 qps | 1.68x | 1.47x |
| load-stats | 60,480 ops | 21,824.7 qps | 1.0 % | 19,196.6 qps | 1.5 % | +13.7 % | **finding** | 12,948.5 qps | 1.69x | 1.48x |
| load-models | 8 ops | 1,647.0 qps | 17.5 % | 742.4 qps | 23.7 % | +121.8 % | spread > 20 %, no claim | 797.3 qps | 2.07x | 0.93x |
| backtest-read | 360 ops | 615.0 qps | 1.1 % | 464.2 qps | 17.3 % | +32.5 % | **finding** | 367.8 qps | 1.67x | 1.26x |
| result-insert | 2,872 ops | 1,789.2 qps | 3.0 % | 749.7 qps | 1.2 % | +138.7 % | **finding** | 779.8 qps | 2.29x | 0.96x |
| backtest-replay | 360 ops | 658.4 qps | 15.8 % | 491.8 qps | 17.3 % | +33.9 % | **finding** | 387.8 qps | 1.70x | 1.27x |
| compare-all | 1 ops | 92.4 qps | 4.1 % | 101.0 qps | 1.0 % | -8.5 % | **finding** | 120.0 qps | 0.77x | 0.84x |
| compare-one | 32 ops | 915.3 qps | 2.6 % | 1,026.9 qps | 1.2 % | -10.9 % | **finding** | 1,091.5 qps | 0.84x | 0.94x |
| read-bar-lookup | 200 ops | 13,081.4 qps | 30.0 % | 11,892.5 qps | 28.3 % | +10.0 % | spread > 20 %, no claim | 14,120.8 qps | 0.93x | 0.84x |
| read-bar-range | 200 ops | 1,071.5 qps | 9.2 % | 1,278.0 qps | 2.2 % | -16.2 % | **finding** | 1,383.6 qps | 0.77x | 0.92x |
| read-symbol-history | 10 ops | 27.3 qps | 0.4 % | 27.1 qps | 3.3 % | +0.7 % | inside the spread | 32.7 qps | 0.83x | 0.83x |
| read-day-slice | 200 ops | 666.4 qps | 1.2 % | 507.3 qps | 18.6 % | +31.4 % | **finding** | 415.1 qps | 1.61x | 1.22x |
| read-join-point | 200 ops | 13,645.8 qps | 4.2 % | 12,546.8 qps | 31.7 % | +8.8 % | spread > 20 %, no claim | 6,924.8 qps | 1.97x | 1.81x |
| read-join-exists | 10 ops | 202.8 qps | 3.7 % | 166.7 qps | 4.6 % | +21.7 % | **finding** | 123.9 qps | 1.64x | 1.35x |
| agg-global | 10 ops | 172.8 qps | 3.4 % | 165.2 qps | 1.8 % | +4.6 % | **finding** | 244.4 qps | 0.71x | 0.68x |
| agg-by-symbol | 10 ops | 130.4 qps | 4.7 % | 133.1 qps | 1.7 % | -2.0 % | inside the spread | 145.5 qps | 0.90x | 0.91x |
| agg-by-session | 10 ops | 40.6 qps | 1.5 % | 45.9 qps | 3.1 % | -11.5 % | **finding** | 50.1 qps | 0.81x | 0.92x |
| agg-day-slice | 200 ops | 688.1 qps | 2.8 % | 514.0 qps | 18.6 % | +33.9 % | **finding** | 413.4 qps | 1.66x | 1.24x |
| agg-distinct | 10 ops | 167.9 qps | 4.8 % | 167.2 qps | 3.7 % | +0.4 % | inside the spread | 244.6 qps | 0.69x | 0.68x |
| sync | 1 ops | 281.3 qps | 10.3 % | 229.1 qps | 74.2 % | +22.8 % | spread > 20 %, no claim | n/a | n/a | n/a |

**Writes: the durability wait is the cost of a row, and it is much cheaper on this device.** Rows per `BEGIN`/`COMMIT`, median of three with the spread in brackets (writes return no rows):

| rows per BEGIN/COMMIT | c1-g now | c8-g now | c1-s now | c1-g v3.3.0-38 | c8-g v3.3.0-38 | c1-s v3.3.0-38 | PG |
|---|---|---|---|---|---|---|---|
| 1 | 1,767 rows/s (2.8 %) | 1,650 rows/s (4.9 %) | 1,791 rows/s (6.9 %) | 768 rows/s (1.9 %) | 751 rows/s (1.8 %) | 764 rows/s (4.3 %) | 739 rows/s |
| 10 | 9,221 rows/s (8.0 %) | 8,965 rows/s (5.3 %) | 9,459 rows/s (4.9 %) | 5,534 rows/s (3.4 %) | 5,325 rows/s (16.3 %) | 5,768 rows/s (0.7 %) | 5,248 rows/s |
| 100 | 19,223 rows/s (4.6 %) | 18,535 rows/s (1.3 %) | 18,920 rows/s (2.6 %) | 15,799 rows/s (1.0 %) | 15,448 rows/s (9.5 %) | 16,044 rows/s (5.3 %) | 12,149 rows/s |
| 1000 | 22,878 rows/s (3.1 %) | 22,050 rows/s (1.6 %) | 22,276 rows/s (1.9 %) | 20,868 rows/s (19.9 %) | 19,074 rows/s (19.9 %) | 20,823 rows/s (8.2 %) | 16,505 rows/s |

**Concurrency.** One join (`cross-join`) issued over 1, 2, 4 and 8 connections:

| connections | c1-g now | c8-g now | c1-s now | c1-g v3.3.0-38 | c8-g v3.3.0-38 | c1-s v3.3.0-38 | PG |
|---|---|---|---|---|---|---|---|
| 1 | 653 qps (4.7 %) | 591 qps (3.0 %) | 657 qps (2.5 %) | 485 qps (21.1 %) | 466 qps (18.1 %) | 492 qps (0.7 %) | 456 qps |
| 2 | 712 qps (5.6 %) | 895 qps (9.5 %) | 742 qps (3.1 %) | 525 qps (19.4 %) | 886 qps (16.0 %) | 520 qps (1.5 %) | 870 qps |
| 4 | 745 qps (2.1 %) | 996 qps (8.3 %) | 729 qps (10.3 %) | 527 qps (20.6 %) | 1,196 qps (25.8 %) | 522 qps (0.9 %) | 892 qps |
| 8 | 751 qps (6.2 %) | 988 qps (10.3 %) | 768 qps (5.6 %) | 528 qps (19.0 %) | 1,516 qps (24.9 %) | 522 qps (0.6 %) | 875 qps |

The `cores = 1` cells do not scale with connections (the aggregate is round-trip overlap, 653 to 751 qps) and `cores = 8` reaches 1.7x at eight connections (591 to 988 qps; the previous run's 3.25x was measured on a different CPU and its own spread was 22 % to 42 % at 4 and 8 connections, so this scale-out figure is not compared). All concurrency cells are reads on one connection set; none is a saturation measurement of the engine, see section 11.

## 4. Percentiles

Run 1 of each of the three cells, every phase of the driver (microseconds, the driver's own five percentiles plus the maximum; ops per phase in the table):

#### phases, run 1 of `s1-c1-g` (cores = 1, group)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load-exchanges | 2 ops | 476.6 µs | 476.6 µs | 476.6 µs | 654.8 µs | 654.8 µs | 654.8 µs | 0 errors |
| load-symbols | 8 ops | 353.4 µs | 385.7 µs | 441.0 µs | 589.4 µs | 589.4 µs | 589.4 µs | 0 errors |
| load-sessions | 7,560 ops | 37.6 µs | 39.6 µs | 40.5 µs | 55.5 µs | 106.6 µs | 382.4 µs | 0 errors |
| load-bars | 60,480 ops | 34.7 µs | 41.6 µs | 42.4 µs | 55.6 µs | 72.2 µs | 2,466.4 µs | 0 errors |
| load-stats | 60,480 ops | 35.9 µs | 42.0 µs | 42.8 µs | 56.1 µs | 74.6 µs | 1,775.5 µs | 0 errors |
| load-models | 8 ops | 494.5 µs | 513.8 µs | 523.7 µs | 744.4 µs | 744.4 µs | 744.4 µs | 0 errors |
| backtest-read | 360 ops | 1,488.6 µs | 1,556.2 µs | 1,595.3 µs | 1,768.2 µs | 2,433.1 µs | 3,543.0 µs | 0 errors |
| result-insert | 2,872 ops | 319.1 µs | 459.6 µs | 526.9 µs | 756.8 µs | 903.1 µs | 2,995.7 µs | 0 errors |
| backtest-replay | 360 ops | 1,404.7 µs | 1,471.5 µs | 1,501.1 µs | 1,634.9 µs | 1,925.6 µs | 2,101.9 µs | 0 errors |
| compare-all | 1 ops | 10,815.0 µs | 10,815.0 µs | 10,815.0 µs | 10,815.0 µs | 10,815.0 µs | 10,815.0 µs | 0 errors |
| compare-one | 32 ops | 1,011.5 µs | 1,033.6 µs | 1,066.5 µs | 1,220.6 µs | 1,250.7 µs | 1,250.7 µs | 0 errors |
| read-bar-lookup | 200 ops | 68.9 µs | 71.7 µs | 72.8 µs | 90.5 µs | 165.4 µs | 211.1 µs | 0 errors |
| read-bar-range | 200 ops | 814.5 µs | 852.4 µs | 884.6 µs | 1,324.3 µs | 1,560.0 µs | 1,601.4 µs | 0 errors |
| read-symbol-history | 10 ops | 35,285.0 µs | 35,648.5 µs | 36,343.1 µs | 39,058.8 µs | 39,058.8 µs | 39,058.8 µs | 0 errors |
| read-day-slice | 200 ops | 1,375.6 µs | 1,435.6 µs | 1,475.7 µs | 1,664.5 µs | 2,349.6 µs | 2,557.5 µs | 0 errors |
| read-join-point | 200 ops | 66.6 µs | 68.8 µs | 69.7 µs | 79.8 µs | 100.3 µs | 174.0 µs | 0 errors |
| read-join-exists | 10 ops | 4,550.1 µs | 4,641.4 µs | 4,790.0 µs | 5,599.1 µs | 5,599.1 µs | 5,599.1 µs | 0 errors |
| agg-global | 10 ops | 5,634.9 µs | 5,659.9 µs | 5,678.7 µs | 5,897.7 µs | 5,897.7 µs | 5,897.7 µs | 0 errors |
| agg-by-symbol | 10 ops | 7,363.1 µs | 7,402.1 µs | 7,427.3 µs | 7,558.7 µs | 7,558.7 µs | 7,558.7 µs | 0 errors |
| agg-by-session | 10 ops | 23,781.3 µs | 24,049.5 µs | 24,133.0 µs | 25,241.1 µs | 25,241.1 µs | 25,241.1 µs | 0 errors |
| agg-day-slice | 200 ops | 1,324.6 µs | 1,382.5 µs | 1,420.7 µs | 1,630.5 µs | 2,064.0 µs | 2,969.3 µs | 0 errors |
| agg-distinct | 10 ops | 5,626.4 µs | 5,764.4 µs | 5,794.6 µs | 5,990.9 µs | 5,990.9 µs | 5,990.9 µs | 0 errors |
| sync | 1 ops | 3,317.1 µs | 3,317.1 µs | 3,317.1 µs | 3,317.1 µs | 3,317.1 µs | 3,317.1 µs | 0 errors |

#### phases, run 1 of `s1-c8-g` (cores = 8, group)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load-exchanges | 2 ops | 480.0 µs | 480.0 µs | 480.0 µs | 572.6 µs | 572.6 µs | 572.6 µs | 0 errors |
| load-symbols | 8 ops | 442.0 µs | 443.0 µs | 518.8 µs | 912.9 µs | 912.9 µs | 912.9 µs | 0 errors |
| load-sessions | 7,560 ops | 38.1 µs | 41.2 µs | 50.9 µs | 65.4 µs | 97.7 µs | 481.6 µs | 0 errors |
| load-bars | 60,480 ops | 32.2 µs | 42.5 µs | 43.7 µs | 60.0 µs | 92.9 µs | 2,501.9 µs | 0 errors |
| load-stats | 60,480 ops | 32.3 µs | 42.9 µs | 44.1 µs | 61.5 µs | 97.1 µs | 5,448.4 µs | 0 errors |
| load-models | 8 ops | 506.6 µs | 587.7 µs | 610.9 µs | 874.4 µs | 874.4 µs | 874.4 µs | 0 errors |
| backtest-read | 360 ops | 1,643.3 µs | 1,721.9 µs | 1,763.4 µs | 1,894.6 µs | 2,223.6 µs | 3,044.6 µs | 0 errors |
| result-insert | 2,872 ops | 353.6 µs | 518.7 µs | 592.8 µs | 836.5 µs | 960.1 µs | 1,826.9 µs | 0 errors |
| backtest-replay | 360 ops | 1,556.8 µs | 1,624.2 µs | 1,661.5 µs | 1,812.2 µs | 1,880.0 µs | 2,045.6 µs | 0 errors |
| compare-all | 1 ops | 11,671.5 µs | 11,671.5 µs | 11,671.5 µs | 11,671.5 µs | 11,671.5 µs | 11,671.5 µs | 0 errors |
| compare-one | 32 ops | 1,085.1 µs | 1,119.7 µs | 1,159.1 µs | 1,335.1 µs | 1,362.0 µs | 1,362.0 µs | 0 errors |
| read-bar-lookup | 200 ops | 69.5 µs | 72.0 µs | 73.0 µs | 92.6 µs | 137.0 µs | 175.1 µs | 0 errors |
| read-bar-range | 200 ops | 806.4 µs | 850.9 µs | 878.4 µs | 1,049.0 µs | 1,138.1 µs | 1,390.0 µs | 0 errors |
| read-symbol-history | 10 ops | 35,904.6 µs | 36,324.6 µs | 36,692.5 µs | 40,526.1 µs | 40,526.1 µs | 40,526.1 µs | 0 errors |
| read-day-slice | 200 ops | 1,521.2 µs | 1,593.1 µs | 1,628.0 µs | 1,831.9 µs | 2,063.3 µs | 2,144.4 µs | 0 errors |
| read-join-point | 200 ops | 68.6 µs | 72.0 µs | 74.1 µs | 112.1 µs | 170.8 µs | 282.5 µs | 0 errors |
| read-join-exists | 10 ops | 4,814.8 µs | 4,851.4 µs | 4,898.7 µs | 6,059.3 µs | 6,059.3 µs | 6,059.3 µs | 0 errors |
| agg-global | 10 ops | 5,862.7 µs | 5,911.3 µs | 5,918.4 µs | 6,230.7 µs | 6,230.7 µs | 6,230.7 µs | 0 errors |
| agg-by-symbol | 10 ops | 7,706.8 µs | 7,777.6 µs | 7,901.8 µs | 8,615.6 µs | 8,615.6 µs | 8,615.6 µs | 0 errors |
| agg-by-session | 10 ops | 24,456.3 µs | 24,463.7 µs | 24,715.6 µs | 26,225.8 µs | 26,225.8 µs | 26,225.8 µs | 0 errors |
| agg-day-slice | 200 ops | 1,469.6 µs | 1,538.9 µs | 1,571.1 µs | 1,634.7 µs | 1,676.0 µs | 3,121.8 µs | 0 errors |
| agg-distinct | 10 ops | 5,885.1 µs | 5,918.8 µs | 5,930.4 µs | 6,135.4 µs | 6,135.4 µs | 6,135.4 µs | 0 errors |
| sync | 1 ops | 2,003.9 µs | 2,003.9 µs | 2,003.9 µs | 2,003.9 µs | 2,003.9 µs | 2,003.9 µs | 0 errors |

#### phases, run 1 of `s1-c1-s` (cores = 1, strict)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load-exchanges | 2 ops | 466.3 µs | 466.3 µs | 466.3 µs | 518.1 µs | 518.1 µs | 518.1 µs | 0 errors |
| load-symbols | 8 ops | 330.8 µs | 364.0 µs | 449.6 µs | 552.5 µs | 552.5 µs | 552.5 µs | 0 errors |
| load-sessions | 7,560 ops | 37.1 µs | 39.3 µs | 40.0 µs | 46.2 µs | 61.3 µs | 238.5 µs | 0 errors |
| load-bars | 60,480 ops | 38.5 µs | 41.3 µs | 42.2 µs | 56.5 µs | 85.2 µs | 2,393.3 µs | 0 errors |
| load-stats | 60,480 ops | 39.3 µs | 41.9 µs | 42.7 µs | 55.9 µs | 83.0 µs | 622.1 µs | 0 errors |
| load-models | 8 ops | 475.7 µs | 502.5 µs | 554.4 µs | 737.2 µs | 737.2 µs | 737.2 µs | 0 errors |
| backtest-read | 360 ops | 1,491.2 µs | 1,558.6 µs | 1,597.4 µs | 1,775.8 µs | 2,484.5 µs | 3,420.2 µs | 0 errors |
| result-insert | 2,872 ops | 318.5 µs | 461.9 µs | 527.5 µs | 749.5 µs | 860.0 µs | 2,461.9 µs | 0 errors |
| backtest-replay | 360 ops | 1,384.6 µs | 1,445.9 µs | 1,485.9 µs | 1,724.3 µs | 2,226.3 µs | 2,351.5 µs | 0 errors |
| compare-all | 1 ops | 10,892.6 µs | 10,892.6 µs | 10,892.6 µs | 10,892.6 µs | 10,892.6 µs | 10,892.6 µs | 0 errors |
| compare-one | 32 ops | 1,004.8 µs | 1,031.9 µs | 1,090.4 µs | 1,269.7 µs | 1,301.2 µs | 1,301.2 µs | 0 errors |
| read-bar-lookup | 200 ops | 68.9 µs | 71.0 µs | 71.9 µs | 80.2 µs | 101.6 µs | 169.9 µs | 0 errors |
| read-bar-range | 200 ops | 797.5 µs | 829.2 µs | 857.2 µs | 993.9 µs | 1,031.8 µs | 1,078.8 µs | 0 errors |
| read-symbol-history | 10 ops | 35,336.8 µs | 35,728.4 µs | 36,079.7 µs | 38,907.2 µs | 38,907.2 µs | 38,907.2 µs | 0 errors |
| read-day-slice | 200 ops | 1,360.3 µs | 1,410.6 µs | 1,444.5 µs | 1,600.9 µs | 1,932.5 µs | 2,074.2 µs | 0 errors |
| read-join-point | 200 ops | 66.4 µs | 68.8 µs | 69.8 µs | 84.1 µs | 102.5 µs | 195.9 µs | 0 errors |
| read-join-exists | 10 ops | 4,652.9 µs | 4,678.1 µs | 4,831.4 µs | 5,773.6 µs | 5,773.6 µs | 5,773.6 µs | 0 errors |
| agg-global | 10 ops | 5,591.3 µs | 5,630.9 µs | 5,698.8 µs | 5,843.0 µs | 5,843.0 µs | 5,843.0 µs | 0 errors |
| agg-by-symbol | 10 ops | 7,264.2 µs | 7,356.0 µs | 7,367.4 µs | 7,447.4 µs | 7,447.4 µs | 7,447.4 µs | 0 errors |
| agg-by-session | 10 ops | 23,506.6 µs | 23,664.5 µs | 23,797.6 µs | 25,292.3 µs | 25,292.3 µs | 25,292.3 µs | 0 errors |
| agg-day-slice | 200 ops | 1,319.2 µs | 1,371.0 µs | 1,403.2 µs | 1,561.3 µs | 1,722.6 µs | 3,030.4 µs | 0 errors |
| agg-distinct | 10 ops | 5,750.4 µs | 5,802.9 µs | 5,829.5 µs | 6,084.1 µs | 6,084.1 µs | 6,084.1 µs | 0 errors |
| sync | 1 ops | 3,152.9 µs | 3,152.9 µs | 3,152.9 µs | 3,152.9 µs | 3,152.9 µs | 3,152.9 µs | 0 errors |

## 5. Where the time goes

The matrix is serial reads on one connection, so a statement is a fixed cost plus a per-row cost, and a write is a statement plus the durability wait. What the cells measure of each, `c1-g` run 1:

| Component | now | v3.3.0-38 | PG 18.6 (v3.3.0-33 run) | derived from |
|---|---|---|---|---|
| Fixed round trip | 76.4 µs mean, p0 68.9 µs | 106.4 µs, p0 73.3 µs | 70.9 µs, p0 65.0 µs | `read-bar-lookup`, run 1 |
| Per returned row (200-row range) | 4.32 µs | 3.38 µs | 3.26 µs | (`read-bar-range` mean - fixed)/200 |
| Per returned row (symbol history, 7,560 rows) | 4.84 µs | 4.79 µs | 4.04 µs | (`read-symbol-history` mean - fixed)/7,560 |
| Per examined, unreturned row | 23.7 ns | 26.1 ns | 39.0 ns | (`read-day-slice` mean - fixed)/60,480 |

| Wait | Reading |
|---|---|
| Durability / commit | autocommit insert 576.1 µs per row, 1,000-row batch 43.7 µs per row (run 1); one durable commit is about 532 µs. The `sync` phase (one `SYNC`) takes 3,317 µs in run 1 |
| Lock or conflict wait | none: one connection, reads after the load, no concurrent writer |
| Read statements | fixed round trip (client, socket, parse, one pk descent) plus per-row terms above |
| Client and socket round trip | the 76 µs fixed term (p0 68.9 µs) is mostly this; it is not separable from the engine's parse and descent by this driver |

**The returned-row cost moved the wrong way against the recorded number and the same way as the host.** A returned row costs 4.32 µs here on `bar-range` against 3.38 µs recorded (4.84 against 4.79 µs on `symbol-history`), while the examined-row cost is 23.7 ns against 26.1 ns. The A/B (section 9) puts `bar-range` at 1.02x between the two engines on this host, so the 0.9 µs is the Python client on a slower core, not the engine. The fixed round trip is lower here (76.4 against 106.4 µs, and p0 68.9 against 73.3 µs).

## 6. Joins and the plan the engine chose

Not re-measured: the join-order probe was cut (section 2). The cross-engine A/B's `compare-one` (the reordered join) reads 1.01x and `cross-join` 1.08x, so the plan the engine chooses did not slow either; no plan figure is claimed. The ANALYZE replies of the matrix cells are in the stdout logs of the archive.

## 7. The size sweep

`c1-g`, 252 / 1,008 / 10,080 bars (60,480 is section 3), three runs each, now against `v3.3.0-38` (host included) and against PostgreSQL's warm qps (other host):

| bars | shape | warm now | spread | warm v3.3.0-38 | delta | read | PG warm | KDS/PG now | KDS/PG v3.3.0-38 |
|---|---|---|---|---|---|---|---|---|---|
| 252 | bar-lookup | 13,555.0 qps | 41.3 % | 13,408.1 qps | +1.1 % | spread > 20 %, no claim | 9,989.7 qps | 1.36x | 1.34x |
| 252 | bar-range | 1,103.2 qps | 3.3 % | 1,279.3 qps | -13.8 % | **finding** | 1,318.8 qps | 0.84x | 0.97x |
| 252 | day-slice | 11,151.9 qps | 41.2 % | 11,105.7 qps | +0.4 % | spread > 20 %, no claim | 13,721.8 qps | 0.81x | 0.81x |
| 252 | symbol-history | 740.3 qps | 0.6 % | 826.3 qps | -10.4 % | **finding** | 927.4 qps | 0.80x | 0.89x |
| 252 | cross-join | 9,698.6 qps | 1.9 % | 8,264.1 qps | +17.4 % | **finding** | 5,666.7 qps | 1.71x | 1.46x |
| 252 | point-join | 13,579.0 qps | 47.5 % | 12,568.8 qps | +8.0 % | spread > 20 %, no claim | 6,353.6 qps | 2.14x | 1.98x |
| 252 | model-join | 8,349.3 qps | 41.1 % | 8,485.1 qps | -1.6 % | spread > 20 %, no claim | 6,596.8 qps | 1.27x | 1.29x |
| 1,008 | bar-lookup | 13,715.9 qps | 38.8 % | 13,149.9 qps | +4.3 % | spread > 20 %, no claim | 11,445.5 qps | 1.20x | 1.15x |
| 1,008 | bar-range | 1,108.2 qps | 3.8 % | 1,276.6 qps | -13.2 % | **finding** | 1,278.9 qps | 0.87x | 1.00x |
| 1,008 | day-slice | 7,920.5 qps | 38.5 % | 7,661.0 qps | +3.4 % | spread > 20 %, no claim | 7,652.4 qps | 1.04x | 1.00x |
| 1,008 | symbol-history | 702.5 qps | 1.6 % | 814.8 qps | -13.8 % | **finding** | 879.8 qps | 0.80x | 0.93x |
| 1,008 | cross-join | 7,159.7 qps | 38.0 % | 6,330.0 qps | +13.1 % | spread > 20 %, no claim | 3,398.9 qps | 2.11x | 1.86x |
| 1,008 | point-join | 14,380.3 qps | 21.5 % | 12,521.3 qps | +14.8 % | spread > 20 %, no claim | 7,119.6 qps | 2.02x | 1.76x |
| 1,008 | model-join | 8,944.7 qps | 38.2 % | 8,688.0 qps | +3.0 % | spread > 20 %, no claim | 8,067.8 qps | 1.11x | 1.08x |
| 10,080 | bar-lookup | 13,958.9 qps | 0.7 % | 12,962.2 qps | +7.7 % | spread > 20 %, no claim | 14,170.7 qps | 0.99x | 0.91x |
| 10,080 | bar-range | 1,138.3 qps | 4.7 % | 1,252.6 qps | -9.1 % | **finding** | 1,355.2 qps | 0.84x | 0.92x |
| 10,080 | day-slice | 2,679.8 qps | 6.1 % | 2,285.3 qps | +17.3 % | **finding** | 2,058.3 qps | 1.30x | 1.11x |
| 10,080 | symbol-history | 153.6 qps | 2.8 % | 164.4 qps | -6.6 % | **finding** | 194.7 qps | 0.79x | 0.84x |
| 10,080 | cross-join | 2,423.0 qps | 18.0 % | 2,127.3 qps | +13.9 % | inside the spread | 1,681.7 qps | 1.44x | 1.26x |
| 10,080 | point-join | 14,041.9 qps | 49.3 % | 12,855.2 qps | +9.2 % | spread > 20 %, no claim | 7,010.1 qps | 2.00x | 1.83x |
| 10,080 | model-join | 4,093.3 qps | 25.6 % | 4,187.3 qps | -2.2 % | spread > 20 %, no claim | 4,189.7 qps | 0.98x | 1.00x |
| 60,480 | bar-lookup | 13,605.2 qps | 1.8 % | 12,619.9 qps | +7.8 % | spread > 20 %, no claim | 14,673.3 qps | 0.93x | 0.86x |
| 60,480 | bar-range | 1,114.0 qps | 2.9 % | 1,260.3 qps | -11.6 % | **finding** | 1,399.8 qps | 0.80x | 0.90x |
| 60,480 | day-slice | 668.7 qps | 3.9 % | 508.7 qps | +31.5 % | **finding** | 413.5 qps | 1.62x | 1.23x |
| 60,480 | symbol-history | 27.1 qps | 1.5 % | 27.3 qps | -0.7 % | inside the spread | 32.3 qps | 0.84x | 0.85x |
| 60,480 | cross-join | 650.8 qps | 7.3 % | 496.9 qps | +31.0 % | spread > 20 %, no claim | 393.1 qps | 1.66x | 1.26x |
| 60,480 | point-join | 13,692.0 qps | 7.0 % | 10,589.7 qps | +29.3 % | spread > 20 %, no claim | 7,071.3 qps | 1.94x | 1.50x |
| 60,480 | model-join | 905.4 qps | 4.9 % | 1,030.2 qps | -12.1 % | **finding** | 1,092.2 qps | 0.83x | 0.94x |

**Fixed and per-row costs separate at every size, and the shapes keep their classes.** The pk shapes are flat (`bar-lookup` 13,555 to 13,959 qps across the four sizes); the scan shapes scale with rows (`day-slice` 11,152 -> 7,921 -> 2,680 -> 669 qps); the row-returning shapes are flat in relation size (`bar-range` 1,103 to 1,138 qps) and scale with rows returned (`symbol-history` 740 -> 703 -> 154 -> 27 qps as the symbol's sessions go 252 -> 252 -> 1,260 -> 7,560). The 252- and 1,008-bar cells carry spreads up to 47 % on the shapes that complete in under 150 µs (one slow run of three), so no delta is claimed on them.

## 8. Against PostgreSQL (the floor)

Not measured on this host (section header). The columns of sections 3 and 7 carry the other-host PostgreSQL 18.6 for orientation: on the same-shape comparison KDS is 0.80x to 0.84x of it on the three many-row shapes (`bar-range`, `symbol-history`, `model-join` at 60,480 bars), 1.6x to 1.9x on the walk and pk-join shapes, and 1.7x to 2.7x on the writes. Because the fsync on this device is 2.3x cheaper than on the other host (section 5), the write ratios are not a league reading, and the other ratios drift with the CPU; the only thing the figures support is that KDS is in the same league as the floor on reads. This is not a result.

## 9. The engine, isolated: v3.3.0-38 against v3.4.14, interleaved on this host

The A/B that the recorded-number deltas need: the binary built from `a40882af` and the binary measured, the same `35086008` driver, same device, alternating old, new, old, new, old, new (three pairs, one fresh server per run), medians and spreads over three runs each. "New over old" is the engine; a ratio inside both spreads is not a finding.

| measure | v3.3.0-38 engine | spread | v3.4.14 engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s1 bar-lookup warm | 13,277.8 qps | 8.2 % | 13,162.2 qps | 15.6 % | 0.99x | inside the spread |
| s1 bar-range warm | 1,114.6 qps | 3.6 % | 1,140.9 qps | 4.1 % | 1.02x | inside the spread |
| s1 day-slice warm | 629.7 qps | 4.5 % | 679.9 qps | 4.2 % | 1.08x | **finding** |
| s1 symbol-history warm | 25.8 qps | 0.8 % | 27.4 qps | 0.4 % | 1.06x | **finding** |
| s1 cross-join warm | 599.1 qps | 3.4 % | 648.7 qps | 2.7 % | 1.08x | **finding** |
| s1 point-join warm | 13,985.4 qps | 9.6 % | 14,004.4 qps | 7.4 % | 1.00x | inside the spread |
| s1 model-join warm | 928.3 qps | 3.4 % | 924.6 qps | 2.3 % | 1.00x | inside the spread |
| s1 phase backtest-read | 558.3 qps | 1.0 % | 617.2 qps | 2.8 % | 1.11x | **finding** |
| s1 phase result-insert | 1,821.7 qps | 2.0 % | 1,810.5 qps | 2.3 % | 0.99x | inside the spread |
| s1 phase compare-one | 912.6 qps | 5.8 % | 921.9 qps | 3.4 % | 1.01x | inside the spread |
| s1 phase read-day-slice | 620.1 qps | 5.5 % | 669.0 qps | 3.5 % | 1.08x | **finding** |
| s1 phase load-bars | 22,946.5 qps | 3.5 % | 22,268.3 qps | 0.8 % | 0.97x | inside the spread |
| s1 phase agg-by-session | 40.9 qps | 2.4 % | 41.6 qps | 1.4 % | 1.02x | inside the spread |

**The engine is flat on every pk, join, aggregate and write measure, and 6 % to 11 % faster on the shapes that walk rows.** `day-slice` (8 returned of 60,480 walked) 1.08x, `cross-join` 1.08x, `symbol-history` 1.06x (a 7,560-row filtered walk) and the `backtest-read` phase 1.11x are outside their spreads (3 % to 5 %); the `read-day-slice` phase repeats the first at 1.08x. The controls that cannot be affected by a scan change read 0.97x to 1.02x (`result-insert` 0.99x, `load-bars` 0.97x, `agg-by-session` 1.02x, `bar-lookup` and `point-join` 0.99x to 1.00x). That sets the noise floor of this A/B at about 3 %, and `bar-range` (1.02x) and `model-join` (1.00x) are inside it. The cause of the scan gain is not attributed here: 214 source and header files changed between the two commits and this run does not bisect them.

## 10. What the run says about the engine

1. **No regression across 432 commits.** Between `a40882af` and `35086008` the engine's pk read, join, aggregate, write and bulk-load numbers are the same inside a 3 % floor, and the walk-bound reads are 6 % to 11 % faster. What landed in between (BEGIN READ ONLY, FOR UPDATE, the key-set access, managed plans and statistics, the checkpoint changes) has no visible price on this scenario's statements.
2. **The recorded v3.3.0-38 deltas are not engine deltas.** The same engine reads -12 % on `bar-range` and +31 % on `day-slice` against the recorded run, and 1.02x and 1.08x against its own old binary on this host. The scenario's per-row client cost (4.3 µs per returned row) is a property of the host's CPU running Python, which is why the many-row shapes are the ones that trail PostgreSQL's recorded ratio, and why a cross-host comparison of them is meaningless.
3. **A durable commit is about half a millisecond on this device**, so a single-row insert is 1,767 rows/s (`c1-g`) against the recorded 768 rows/s at 1,254 µs a commit; at 1,000 rows per commit the figures are 22,878 against 20,868 rows/s, so the amortised insert path is within 10 %. The engine's share of a durable row is the 44 µs insert, not the wait.

## 11. Noise floor, contamination and host

* Spread (max - min over median) of the 18 cold, warm and cabin figures of each matrix cell: median 4.8 % (`c1-g`), 2.9 % (`c8-g`), 5.7 % (`c1-s`), with 1, 0 and 2 of the 18 above 20 %. The A/B controls give a floor of about 3 %.
* **Contamination:** none. No cell was discarded; the host monitor saw no foreign process at 50 % CPU or more inside any cell window; the largest was this session's own agent process at 27.7 % of one CPU in one sample. **One disclosure:** the table generator (`gen.py`, one thread, 1 to 3 s) was run while measured cells were live, including `r2-s1-c1-g` of this scenario and `ab-old1-s1-c1-g`; no monitor sample listed it (it is below the monitor's 10 % line), and neither cell is an outlier of its group (`r2-s1-c1-g` warm `bar-range` 1,094 qps against 1,127 and 1,114 qps).
* **What no limit was found for.** Scenario 1 reads on one connection or eight; no cell approached a limit of the engine (the serial client is the bound at 13,600 qps on `bar-lookup`, and the eight-connection `cross-join` at `cores = 8` scales 1.7x). The saturation measurement of rule 4b was done on scenario 2 (its section 7), where the write path is.

## 12. Archive

`bench/v3.4.14/archive/scenario1-v3.4.14/`: per-cell JSON (`json/`, including the six A/B cells), driver stdout (`logs/`, which include each cell's ANALYZE replies and precheck), server configs (`conf/`). What the three scenarios share is in `bench/v3.4.14/archive/run-v3.4.14/`: the run scripts, the timeline, the monitor and watchdog logs, the binary hashes and the table generators. No data file is kept.
