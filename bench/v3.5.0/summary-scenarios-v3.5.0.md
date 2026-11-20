# Scenarios 1 to 4 at `v3.5.0`: summary

**One line.** On scenarios 1 to 3 the engine at the tag `v3.5.0` (`07f4552a`) is the engine of `v3.4.14`. Every matrix cell is inside its spread of the `v3.4.14` run on this host, and the same-host interleaved A/B against the `v3.4.14` binary reads 0.96x to 1.03x (one reading over its spread, scenario 3's `count-by-user` at 1.07x, not attributed). DI, DH, DJ and the two audits' fixes have no price these workloads can read. **Scenario 4's `on` arm now measures a walk.** Since CX-S3b (`v3.4.13`) a Cabin the controller makes is read only through an administrator's plan that names it (`cabin-controller.md` PO11), and the driver names none. So the controller creates one-page Cabins that nothing probes, and `on` runs at the speed of `off` (0.98x to 1.02x, where `v3.3.0-38` read 1.90x to 2.51x). This is the contract and the same on the `v3.4.14` binary; it is not a regression. A declared Cabin still serves (1.46x to 1.92x). Every byte comparison and every verification passed. PostgreSQL was not measured: this host has no server.

This file indexes the four results files and carries no number they do not. Measured directly by CLA in a scheduled session (fired 2026-11-20 18:00 UTC = 2026-11-21 03:00 KST) from 18:00:46 UTC to 18:44:28 UTC: build 18:00:46 to 18:02:04; scenario 1 18:03:19 to 18:09:53; scenario 2 to 18:15:44; scenario 3 to 18:18:53; scenario 4 (two runs) to 18:28:27; engine A/B 18:28 to 18:36; extras to 18:38:27; scenario 4 on the `v3.4.14` binary 18:39:40 to 18:44:28. **44 minutes, inside the budget of about one hour.** Hold: nil (every gate wait 0 s, 117 gates). One start-up fault cost 60 s (see Watchdog).

**Comparators.** Scenarios 1 to 3: this engine's `v3.4.14` run, `bench/v3.4.14/results-scenario{1-backtest,2-freight,3-library}-v3.4.14.md`, measured 2026-11-19 **on this host**. The same-host A/B pits the `v3.4.14` binary measured then (sha256 `78b3aede...`, the same file) against the measured binary, with one driver, interleaved old, new, old, new, old, new. Scenario 4: `bench/v3.3.0/results-scenario4-cabinopt-days-v3.3.0-38-ga40882af.md`, measured 2026-10-20 **on another host** (AMD EPYC 9V74), so a delta against it is engine plus host. Its engine reading is the one `v3.4.14`-binary run on this host.

## Stamp (common to the four results files)

| Field | Value |
|---|---|
| Version, commit | **`v3.5.0` = `07f4552a`** (`git describe --tags 07f4552a`; annotated tag on `main`, operator-named on 2026-11-20, "tag 3.5.0", **local and not pushed** on the operator's word: `instructions/v3.2.0/raft-marks-2026-11-20.md` §26). The run synced local `main` to `origin/main` (`07f4552a`) at 18:00 UTC, as the order said, and tagged it. Built from `git archive 07f4552a` extracted under `/home/louis/bench-runs/scenarios-v3.5.0/src/`. No build ran in any worktree's `build` or `build-release` |
| Tree cleanliness | an extraction has no working tree; the drivers are the tag's unmodified `tools/scenario{1,2,3,4}_*.py` |
| Binary provenance | Release (`CMAKE_BUILD_TYPE=Release` in `CMakeCache.txt`), built 18:00:46 to 18:02:04 UTC. **Measured a copy**, `kds_server`, **sha256 `ef42b562751ebcb355291eb3c4e0e8274ebf4b4b148b80a7ecaf64378bfd0f8a`**, and every server started from the copy. A/B arm: `kds_server_old`, a copy of `/home/louis/bench-runs/scenarios-v3.4.14/kds_server` (the `v3.4.14` binary, `35086008`, Release), sha256 `78b3aede97192d165094e5051a80958b026d36e2bb92d7a13a394412399d3fb3`, the hash that run recorded |
| Device | data files under `/home/louis/bench-runs/scenarios-v3.5.0/data/`, **`/dev/nvme0n1p1`, ext4** (`df -T` at 18:00 UTC, `/` 85 % used), never tmpfs; a fresh server and data file per cell, deleted when the cell finished |
| Host | 8 logical CPUs, Intel Xeon Platinum 8581C @ 2.10 GHz, 31 GiB, Linux 6.12.116+deb13-cloud-amd64, a shared development VM; one unrelated sandbox `kds_server` up and idle throughout; several idle `claude` sessions |
| Server config | `log_level = warn`, `buffer_pool_frames = 65536`, `cores` and `durability` per cell, all else default but scenario 4's three keys; ports 15604 to 15607, 15610, 15620, 15651 to 15653, never 15432 |
| Quiet host | before each cell a gate waited for no `cc1plus` / `cmake --build` / `ctest` / `kds_tests` process and at most 8 % busy CPU over 3 s: all 117 waits were 0 s. A 1-second monitor ran throughout and logged every foreign process above 10 % of one CPU |
| 60 s watchdog | `watchdog.sh` (the main run), `watchdog2.sh` (the A/B and the extras) and `watchdog3.sh` (the `v3.4.14`-binary scenario 4 run) poll a status file's age and each script's recorded PID, never a process pattern |
| PostgreSQL | **not measured**: the host has the PostgreSQL client tools and no server (`pg_ctl`, `initdb` absent) |
| Suite | not run by this measurement. The tagged tree's build inputs last ran the full suite 0 failed of 4,997 at `d2338bd7` |
| Time budget | 44 minutes. **Cut**: the extreme cells of scenario 2 (20,000 bookings, 32 bookers, one and two client processes), scenario 1's join-order probe and scenario 3's plan replies (both also cut at `v3.4.14`), scenario 4's six-run interleaved A/B (one `v3.4.14`-binary run instead), and PostgreSQL |

## What counts as a finding

Unchanged: a cell is run three times (the booker sweep, the saturation and extreme cells and scenario 4 once each). **Spread = (max - min) / median over the runs of one cell.** A difference between two medians smaller than the larger of the two spreads is not a finding, and a cell above 20 % carries no delta claim. The A/B follows the same rule on its own spreads. For scenario 4, the `pk` control's on-over-off ratio (0.96x to 1.08x) sets the floor: a ratio under about 1.10x is not a finding.

## Headlines

Warm qps (scenario 1), tps (2) and derived qps (3), median of three runs, `cores = 1` unless stated. Delta = now over the `v3.4.14` run on this host; the A/B column is new over old on the same host:

| measure | v3.5.0 | v3.4.14 | delta | engine A/B |
|---|---|---|---|---|
| 1 backtest, `day-slice` (60,480 bars) | 677.6 qps | 668.7 qps | +1.3 % (inside spread) | 0.98x |
| 1 backtest, `cross-join` | 640.7 qps | 650.8 qps | -1.6 % (inside spread) | 0.98x |
| 1 backtest, `bar-range` | 1,160.4 qps | 1,114.0 qps | +4.2 % (inside spread) | 0.99x |
| 1 backtest, `model-join` | 898.4 qps | 905.4 qps | -0.8 % (inside spread) | 1.00x |
| 1 backtest, `result-insert` phase | 1,798.6 qps | 1,789.2 qps | +0.5 % (inside spread) | 0.99x |
| 2 freight, 8 bookers, `cores = 1`, `group` | 1,172.7 tps | 1,183.3 tps | -0.9 % (inside spread) | 0.99x |
| 2 freight, 8 bookers, `cores = 8`, `group` | 2,612.6 tps | 2,548.8 tps | +2.5 % (inside spread) | n/a |
| 2 freight, 8 bookers, `cores = 8`, `strict` | 2,384.5 tps | 2,321.0 tps | +2.7 % (inside spread) | n/a |
| 2 freight, 1 booker, `strict` | 631.3 tps | 625.8 tps | +0.9 % (inside spread) | 1.02x |
| 2 freight, 32 bookers, `cores = 8`, `group` (one run) | 3,735.5 tps | 3,841.6 tps | -2.8 % (no claim) | n/a |
| 3 library, `loans-by-user` (10,000 loans, unindexed) | 3,180 qps | 3,186 qps | -0.2 % (inside spread) | 1.01x |
| 3 library, `join-no-literal` (10,000 loans, unindexed) | 1,954 qps | 1,934 qps | +1.0 % (inside spread) | 0.97x |
| 3 library, `exists-correlated` (10,000 loans, unindexed) | 1,520 qps | 1,588 qps | -4.3 % (inside spread) | 1.03x |
| 3 library, `loans-by-user` (10,000 loans, indexed) | 12,180 qps | 12,346 qps | -1.3 % (inside spread) | n/a |

Scenario 4, run A, ratio of mean latencies over the `off` arm, days 1 to 3 (comparator `v3.3.0-38`, another host):

| group | `on` over `off`, v3.5.0 | `on` over `off`, v3.4.14 binary, this host | `on` over `off`, v3.3.0-38 | `declared` over `off`, v3.5.0 |
|---|---|---|---|---|
| `board` (10,000 rows, hot equality) | 0.99x / 1.00x / 1.00x | 0.99x / 1.02x / 0.98x | 1.90x / 1.94x / 2.29x | 1.46x / 1.52x / 1.84x |
| `tape10k` | 1.00x / 1.01x / 1.00x | 1.02x / 1.02x / 0.97x | 2.04x / 2.43x / 2.51x | 1.53x / 1.92x / 1.69x |

Files: `results-scenario1-backtest-v3.5.0.md`, `results-scenario2-freight-v3.5.0.md`, `results-scenario3-library-v3.5.0.md`, `results-scenario4-cabinopt-days-v3.5.0.md` (all under `bench/v3.5.0/`).

## The movers, and what they are

- **Nothing moved in the engine between `v3.4.14` and `v3.5.0` on scenarios 1 to 3** (`35086008..07f4552a`: DA, DF's tail, DI, DH, DJ, CV, the second and the v3.4.* audits' fixes). `SELECT ... FOR UPDATE`'s successors, the packed page image and the instance's checkpoint cadence, the statistics re-keying and the connection accounting have no readable price here. The four walk-bound gains `v3.4.14` found against `v3.3.0-38` (6 % to 11 %) are held: the A/B against `v3.4.14` is level.
- **Scenario 4's controller arm prices nothing the engine now offers unasked** (PO11, CX-Q6 (a), CX-S3b `2d297055`): the controller creates Cabins on heat that no statement's step tries without a plan naming them, so they stay at one page, record no lookup and are never extended. `on` is the walk, and its server CPU is the `off` arm's (2.0 s on the boards, against 0.8 s at `v3.3.0-38`). The `v3.4.14` binary reads the same, as the rule predicts. To measure the controller again, the driver would need a `CREATE PLAN ... ACCESS <relation> CABIN (<column>)` per probed shape, which is not built. No byte comparison failed.
- **Scenario 3's small cells are unreadable this window** (200 and 1,000 loans, spreads 28 % to 67 %, one slow run in three, no foreign process seen).

## What this run does not show

- **No PostgreSQL number** (no server on this host).
- **The controller's effect under a plan that names its Cabins**: the scenario 4 driver writes no `CREATE PLAN`.
- **Scenario 2's extreme cells** (cut) and the engine's own ceiling under them.
- **DJ's limits at work**: no cell reaches a core's 1,024 in-flight slots, sets `max_connections` or idles a session to its timeout.
- Anything after `07f4552a`.

## Watchdog

At 18:02:19 UTC the first launch of `run_all.sh`, `ab.sh` and `extras.sh` failed because their redirect into `logs/` ran before the directory existed. The watchdogs logged each as `ORPHAN` and relaunched them at their next minute, 18:03:19 to 18:03:21, after the directory was created by hand. No cell had started, so nothing was lost or re-run. After that no watchdog found an orphan or a stall.

## Contamination

No cell was discarded or re-run. Two cells carry a foreign sample of 50 % or more of one CPU, and both are kept and disclosed: `s3-c1-n10000-all` run 1 (`apt-get --just-print` at 63.0 %, 18:16:24 UTC; its `pk-user` is the one 10,000-loan reading over its spread, -11.3 %, not read as an engine change) and `ab-new3-b1-s2-c1-s` (another session's `claude` at 74.3 %, 18:35:43 UTC; not an outlier of its group). Every other foreign sample was below 50 %: another session's `claude` process, the measuring session itself and the host's agents. **Disclosure:** the documents were drafted by this session while scenario 4 and the A/B were live, so the agent's own CPU is in the monitor at 15 % to 42 % of one CPU at those times. The table generator ran after every cell had finished.

## Archive

- `bench/v3.5.0/archive/scenarioN-v3.5.0/`: each scenario's per-cell JSON, driver stdout, precheck (load average, process scan, gate wait) and server configs and logs. The A/B cells (`ab-old*`, `ab-new*`), the extras and the `v3.4.14`-binary scenario 4 run (`ab-old1-s4`) are among them.
- `bench/v3.5.0/archive/run-v3.5.0/`: the run scripts (`lib.sh`, `run_all.sh`, `ab.sh`, `ab4.sh`, `extras.sh`), the watchdogs and the monitor, their logs, the timeline, the binary hashes and build times, the table generator `gen.py` and its output under `tables/`. No data file is kept.
