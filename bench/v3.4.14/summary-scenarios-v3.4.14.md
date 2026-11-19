# Scenarios 1 to 3 at `v3.4.14`: summary

**One line.** The engine at the tag `v3.4.14` (`35086008`) is the same engine as `v3.3.0-38-ga40882af` on the pk, join, aggregate, write and bulk-load paths of all three scenarios (an interleaved A/B on one host, 0.97x to 1.04x, inside the spreads) and 6 % to 11 % faster on four walk-bound read shapes of scenario 1; **every larger delta against the recorded `v3.3.0-38` numbers is the host** (a different CPU, kernel and a device whose commit is 559 µs where the recorded one's was 1,294 µs): scenario 2 reads +28 % to +88 %, scenario 3's integer-column walks +30 % to +38 %, scenario 1's many-row shapes -12 %, and none of them is the engine. At the extreme (20,000 bookings, 32 bookers) scenario 2 tops out at 3,079 to 3,411 tps with the host 88 % to 93 % busy at its peak seconds, and a second client process adds nothing: the bound on this 8-vCPU VM is the shared CPU. **`origin/main` (`12887a14`, `v3.4.14-22-g12887a14`) is ahead of the measured tag; nothing after `35086008` is in these numbers.** PostgreSQL was not measured: this host has no server.

This file indexes the three results files and carries no number they do not. Measured by `ck-tester` from 2026-11-19 18:55 UTC to 19:33 UTC (build 18:55 to 18:57; scenario 1 18:58 to 19:05; scenario 2 19:05 to 19:11; scenario 3 19:11 to 19:14; engine A/B 19:15 to 19:22; the `cores = 8` repeats of scenario 3, the isolation probe and the 16-booker cells 19:22 to 19:25; the extreme cells of scenario 2 19:27 to 19:33). The budget of about one hour for everything was kept: **one hold, nil** (the host was quiet at the start: no `cc1plus`, `cmake`, `ctest` or benchmark process of any session; the gate waited 0 s before each of 114 cells).

**Comparators.** KDS: this engine's own last run of each scenario, `bench/v3.3.0/results-scenario{1-backtest,2-freight,3-library}-v3.3.0-38-ga40882af.md`, measured 2026-10-20 on **another host** (AMD EPYC 9V74, Linux 7.0.0-1014-azure), so a delta against it is engine plus host; the same-host A/B (the `a40882af` binary built here, the measured binary, one driver, interleaved old, new, old, new, old, new, three pairs) is the engine's own reading. PostgreSQL: **not measured**; the tables carry the v3.3.0-33 run's PostgreSQL 18.6 from the other host for orientation only (rule 4: the floor, and a missing twin is not a hole).

## Stamp (common to the three results files)

| Field | Value |
|---|---|
| Version, commit | **`v3.4.14` = `35086008`** (`git describe --tags 35086008`; annotated tag on `main`, "DC: `SELECT ... FOR UPDATE`; with CZ's checkpoint that does not wait out a segment roll"). Built from `git archive 35086008` extracted under `/home/louis/bench-runs/scenarios-v3.4.14/src/`; no git worktree was created for the build, nothing was built in any worktree's `build` or `build-release`. **`origin/main` is at `12887a14` (`v3.4.14-22-g12887a14`), 22 commits ahead of the tag, and is not measured** |
| Tree cleanliness | an extraction has no working tree; drivers are the unmodified `tools/scenario{1,2,3}_*.py` of the extraction (`scenario1_backtest.py` / `scenario3_library.py` differ from `a40882af`'s by a printed note, `kwp.py` by one error code) |
| Binary provenance | Release build (`CMAKE_BUILD_TYPE=Release` in `CMakeCache.txt`), built 18:55:35 to 18:56:53 UTC, source binary mtime 2026-11-19 18:56:53.09 UTC (not older than the commit); **measured a copy**, `kds_server`, **sha256 `78b3aede97192d165094e5051a80958b026d36e2bb92d7a13a394412399d3fb3`**; every server started from the copy. A/B arm: `kds_server_old`, `git archive a40882af` built Release 19:13:47 to 19:14:53, sha256 `42041f0b8d681dcffeb271495b1626e06b9b4feb39d5561a7615619845d1e2a8` |
| Device | data files under `/home/louis/bench-runs/scenarios-v3.4.14/data/`, **`/dev/nvme0n1p1`, ext4** (`df -T` at 18:55 UTC, `/` 56 % used); never tmpfs; a fresh server and data file per cell, deleted when the cell finished |
| Build type | Release |
| Host | 8 logical CPUs, Intel Xeon Platinum 8581C @ 2.10 GHz, 31 GiB, Linux 6.12.116+deb13-cloud-amd64, a shared development VM; one unrelated sandbox `kds_server` up and idle throughout |
| Server config | `log_level = warn`, `buffer_pool_frames = 65536` (required), `cores` and `durability` per cell, all else default; ports 15604 to 15607, 15610, 15620, never 15432 |
| Quiet host | before each cell a gate waited for no `cc1plus` / `cmake --build` / `ctest` / `kds_tests` process and at most 8 % busy CPU over 3 s: every wait was 0 s. A 1-second monitor ran throughout; the largest foreign-process sample was 27.7 % of one CPU (this session's agent) and none reached 50 % inside a cell; nothing was discarded |
| 60 s watchdog | `watchdog.sh` (the main run) and `watchdog2.sh` (the A/B, the extras and the extreme cells) poll a status file's age and each script's recorded PID, never a process pattern; neither found an orphan or a stall |
| PostgreSQL | **not measured**: the host has the PostgreSQL 17 client tools and no server (`pg_ctl`, `initdb` absent); the twin was attempted and every cell exited 1 on "connection refused", producing no result |
| Time budget | within about one hour of 18:55 UTC. Cut (stated in each file): the v3.3.0-38 run's join-order probe, its old-client A/B (replaced by the engine A/B, the client being unchanged) and PostgreSQL |

## What counts as a finding

Unchanged from the v3.3.0-38 summary: a cell is run three times (the booker sweep and the saturation and extreme cells once); **spread = (max - min) / median over the runs of one cell**; a difference between two medians smaller than the larger of the two spreads is not a finding; a cell above 20 % carries no delta claim. The A/B pairs follow the same rule on their own spreads; its controls (shapes the change cannot reach) read 0.97x to 1.02x, so the engine A/B's floor is about 3 % to 5 %.

## Headlines

Warm qps (scenario 1), tps (2), derived qps (3), median of three runs, `cores = 1` unless stated. Delta = now over the recorded `v3.3.0-38` (host included); the last column is the same-host engine A/B, new over old (n/a where no A/B cell exists):

| measure | v3.4.14 now | v3.3.0-38 recorded | delta (engine + host) | engine A/B, same host |
|---|---|---|---|---|
| 1 backtest, `day-slice` (60,480 bars) | 668.7 qps | 508.7 qps | +31.5 % | 1.08x (**finding**) |
| 1 backtest, `cross-join` | 650.8 qps | 496.9 qps | +31.0 % (recorded spread 20.6 %, no claim) | 1.08x (**finding**) |
| 1 backtest, `point-join` | 13,692.0 qps | 10,589.7 qps | +29.3 % (recorded bimodal, no claim) | 1.00x |
| 1 backtest, `bar-range` | 1,114.0 qps | 1,260.3 qps | -11.6 % | 1.02x |
| 1 backtest, `symbol-history` | 27.1 qps | 27.3 qps | -0.7 % | 1.06x (**finding**) |
| 1 backtest, `model-join` | 905.4 qps | 1,030.2 qps | -12.1 % | 1.00x |
| 1 backtest, `result-insert` phase (autocommit insert) | 1,789.2 qps | 749.7 qps | +138.7 % (the device's sync) | 0.99x |
| 2 freight, 8 bookers, `cores = 1`, `group` | 1,183.3 tps | 681.3 tps | +73.7 % | 1.02x |
| 2 freight, 8 bookers, `cores = 8`, `group` | 2,548.8 tps | 1,990.6 tps | +28.0 % | n/a |
| 2 freight, 8 bookers, `cores = 8`, `strict` | 2,321.0 tps | 1,716.7 tps | +35.2 % | n/a |
| 2 freight, 1 booker, `group` | 628.7 tps | 425.4 tps | +47.8 % | n/a |
| 2 freight, 1 booker, `strict` | 625.8 tps | 434.3 tps | +44.1 % | 1.00x |
| 2 freight, 20,000 bookings, 32 bookers, `cores = 8`, `group` (single run) | 3,322 tps | n/a (not run recorded) | - | n/a |
| 3 library, `loans-by-user` (10,000 loans, unindexed) | 3,186 qps | 2,449 qps | +30.1 % | 1.01x |
| 3 library, `books-by-genre` (10,000 loans, unindexed) | 2,742 qps | 2,925 qps | -6.3 % (inside spread) | 1.01x |
| 3 library, `loans-by-daterange` (10,000 loans, unindexed) | 900 qps | 913 qps | -1.4 % (inside spread) | 1.00x |
| 3 library, `overdue` (10,000 loans, unindexed) | 1,126 qps | 1,126 qps | +0.1 % (inside spread) | 1.02x |
| 3 library, `join-no-literal` (10,000 loans, unindexed) | 1,934 qps | 1,666 qps | +16.1 % | 1.04x |
| 3 library, `exists-correlated` (10,000 loans, unindexed) | 1,588 qps | 1,608 qps | -1.2 % (inside spread) | 0.98x |
| 3 library, `loans-by-user` (10,000 loans, indexed) | 12,346 qps | 11,682 qps | +5.7 % | n/a |

Files: `results-scenario1-backtest-v3.4.14.md`, `results-scenario2-freight-v3.4.14.md`, `results-scenario3-library-v3.4.14.md` (all under `bench/v3.4.14/`). Scenario 4 (cabinopt) was not part of this run.

## The movers, and what they are

- **Nothing in the engine regressed across 432 commits** (`a40882af..35086008`; 214 source and header files). The A/B's pk reads, joins, aggregates, writes, bulk loads and the whole of scenario 3's twelve shapes are 0.97x to 1.04x, and scenario 2's booking path 1.00x to 1.02x, each inside its spread. `BEGIN READ ONLY`, `SELECT ... FOR UPDATE`, the key-set access, managed plans and statistics and the checkpoint changes that landed in between have no readable price on these workloads.
- **Four walk-bound shapes of scenario 1 are 6 % to 11 % faster in the engine** (`day-slice` and `cross-join` 1.08x, `symbol-history` 1.06x, the `backtest-read` phase 1.11x, spreads 1 % to 5 %); the cause is not attributed, since 214 files changed and this run does not bisect.
- **The host moved everything else, with signs that depend on the workload**: a faster-sync device lifts every durable path (scenario 2 +28 % to +88 %, an autocommit insert 1,789 against 750 qps, the commit mean 559 against 1,294 µs); a faster scanning core lifts the integer-column walks of scenarios 1 and 3 by 30 % to 38 % (24 ns per examined row against 33 ns); and a slower Python core costs the many-row shapes 6 % to 16 % (4.3 µs per returned row on `bar-range` against 3.4 µs recorded). **A comparison across these two hosts cannot read an engine change; the same-host A/B can, to about 3 % to 5 %.**
- **The saturation bound of scenario 2 is found**: with 20,000 bookings and 32 bookers the engine reaches 3,079 to 3,411 tps (`group` 3,322 tps one process, 3,372 tps two processes; `strict` 3,411 and 3,079 tps) with the whole host 88 % to 93 % busy at its peak seconds in the middle seconds; a second client process adds nothing (so one Python client's GIL is not the bound), and `strict` against `group` is +2.7 % and -8.7 % in two single runs (no reading). It is the 8 vCPUs shared by the 8 reactor threads and the clients. The server's ceiling alone needs the clients on another host.

## Where KDS stands against PostgreSQL now

Not measurable on this host. On the other-host PostgreSQL 18.6 figures the same orientation as before holds (the many-row shapes at 0.7x to 0.85x of it, the pk-anchored joins and the correlated `EXISTS` ahead of it, one durable client even with it when the fsync is the same), but the ratios here divide a 559 µs sync by a 1,279 µs one and a 2.1 GHz Xeon by an EPYC, so **no ratio is offered as a result.** The floor stays unmeasured until a PostgreSQL server is installed on the measuring host (`tools/pg_setup.sh`, port 15433).

## What this run does not show

- **No PostgreSQL number of this host** (above).
- **The scan gain's cause** (the four 6 % to 11 % shapes) and **its stability**: three pairs, one host window.
- **The engine's own ceiling on scenario 2**: the host is saturated by server and clients together.
- **Scenario 1's join-order probe and scenario 3's plan replies** were not re-captured (cut); scenario 4 was not run.
- The host-effect decomposition (CPU frequency, kernel, device) is not separated; the A/B proves the engine is the same without saying which part of the host did what.

## Contamination

None: no cell was discarded or re-run, and the monitor never saw a foreign process at 50 % CPU or more inside a cell window (largest sample 27.7 %, this session's agent). **Disclosure:** the table generator (a single thread, 1 to 3 s) was run by hand while measured cells were live on five occasions (`r2-s1-c1-g`, `x-b16-s2-c8-s`/`b1-s2-c1-g`, `s3-c1-n1000-all`, `ab-old1-s1-c1-g`, `ab-new1-s3-c1-n10000-none`), below the monitor's 10 % line; none of those cells is an outlier of its group. An old-binary build (19:13:47 to 19:14:53) ran after the main matrix and before the A/B, with no cell live.

## Archive

- `bench/v3.4.14/archive/scenarioN-v3.4.14/`: each scenario's per-cell JSON, driver stdout, precheck (load average, process scan, gate wait), server configs; the A/B cells (`ab-old*`, `ab-new*`), the isolation, saturation and extreme cells of scenario 2 are among them.
- `bench/v3.4.14/archive/run-v3.4.14/`: the run scripts (`lib.sh`, `run_all.sh`, `ab.sh`, `extras.sh`, `sat2.sh`, `pg.sh` - the last attempted and failed, see Stamp), the watchdogs and monitors, the timeline, the binary hashes, the table generators (`gen.py`, `run2.py`) and the monitor log. No data file is kept.
