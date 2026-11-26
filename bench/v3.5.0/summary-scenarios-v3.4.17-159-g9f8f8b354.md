# Scenarios 1 to 4 at `v3.4.17-159-g9f8f8b354`: summary

**One line.** **On these four workloads the engine at `9f8f8b354` is the engine of `v3.5.0` (`07f4552a`).** In the same-host A/B with the server pinned to the P-cores (five interleaved pairs), every shape and phase of scenarios 1 and 3 that resolves reads 0.95x to 1.03x, scenario 2's single-booker transaction reads 1.00x with every statement at 1.00x to 1.02x, and the eight-booker cells read 1.01x (one core) and 1.03x (eight cores) without a claim. Two rows are marked by the spread rule, both at the floor: scenario 3's `count-by-user` unindexed (-1.1 %) and scenario 2's `org-update` at eight cores (+4.6 %). DL, DK, DD, DN, DO, DP, DQ, DR and DU have no price these workloads can read. **The cells themselves read 1.3x to 3.8x the `v3.5.0` run's, and that is the host**: this run is on a 24-CPU hybrid workstation, that one on an 8-CPU VM. **Scenario 4's `on` arm is still the walk** (PO11: five or six one-page Cabins, no lookup recorded, `extends` 0) on both binaries, and a declared Cabin serves 1.65x to 2.35x on `board`. Every byte comparison passed, and every verification of scenarios 1 and 3; scenario 2's invariant check fails under more than one booker as at `v3.5.0` (the driver's client-side lost update) and passes 400 of 400 in every single-booker and repeatable-read cell. PostgreSQL was not measured: this host has no server. **Unpinned, this host is a poor instrument**: a hybrid CPU under `powersave` with another session building and testing throughout; three-run spreads of 20 % to 80 % are common, most unpinned A/B rows fail the spread rule, and of the eight rows the rule marks unpinned the pinned series confirms none (seven read 0.95x to 1.02x inside their spreads, one carries no claim).

This file indexes the four results files and carries no number they do not. Measured directly by CLA in a scheduled session (ordered 2026-11-26 14:58 UTC, "in 6 hours, run the scenario 1, 2, 3, 4 benchmarks on the latest version of kdbs"; fired 20:58:08 UTC): fetch and reading the `v3.5.0` run 20:58:08 to 20:59; build 20:59:12 to 21:00:04; a first launch 21:02:43 to 21:03:49, stopped and discarded (section Watchdog); scenario 1 21:05:49 to 21:09:38; scenario 2 to 21:11:56; scenario 3 to 21:15:08; scenario 4 run A 21:15:10 to 21:19:43; the unpinned A/B 21:19:44 to 21:41:59 (1,035 s of it held at two gates); the `07f4552a`-binary scenario 4 run 21:42:01 to 21:46:34; extras 21:46:35 to 21:46:54; scenario 4 run B 21:46:55 to 21:51:28; the pinned A/B 21:52:11 to 22:07:44 (522 s held); scenario 4 run A again 22:07:46 to 22:12:20. **74 minutes of wall clock, 28 minutes of it quiet-gate hold (1,691 s), so 46 minutes inside the budget of about one hour.** 186 cells, every driver exit 0.

**Comparators.** The comparator run is the `v3.5.0` run (`07f4552a`), `bench/v3.5.0/results-scenario{1-backtest,2-freight,3-library,4-cabinopt-days}-v3.5.0.md`, measured 2026-11-20 **on another host** (8 logical CPUs, Intel Xeon Platinum 8581C at 2.10 GHz, `/dev/nvme0n1p1` ext4). This host is not that host and this device is not that device, so every "now over `v3.5.0` run" column is host plus engine and none of it is read as an engine change. **The engine reading is the same-host A/B**: a `07f4552a` binary built here from `git archive 07f4552a` (Release) against the measured binary, one driver (the four drivers and `bench_common.py` are byte-identical in the two commits), interleaved old, new; three pairs unpinned as at `v3.5.0`, and five pairs again with the server pinned to the P-cores, which is the series that resolves on this host. Scenario 4's engine reading is one `07f4552a`-binary run.

## Stamp (common to the four results files)

| Field | Value |
|---|---|
| Version, commit | **`v3.4.17-159-g9f8f8b354`** (`git describe --tags` at `9f8f8b354`, the tip of `origin/main` after `git fetch origin` at 20:58 UTC; local `main` was at `60dc593af`, 9 commits behind, and was not moved). **The files are under `bench/v3.5.0/`, the version of record `CLAUDE.md` names, but the `v3.5.0` tag is absent from this clone** (it is local to the clone that made it), so the describe string counts from `v3.4.17`; `07f4552a`, the commit that tag names, is an ancestor, 142 commits back. No tag was created and nothing was pushed. Built from `git archive 9f8f8b354` extracted under `/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354/src/`; no build ran in any worktree's `build` or `build-release` |
| What is between the two commits | `07f4552a..9f8f8b354`: DL, DK, DD, DN (superblock 39), DO, DP (40), DQ, DR (41), DU, and DG, DM and CW closed with nothing built. The superblock moved from 38 to 41 between them (each binary's start-up line names its own); every cell creates its own data file |
| Tree cleanliness | an extraction has no working tree; the drivers are the measured commit's unmodified `tools/scenario{1,2,3,4}_*.py` |
| Binary provenance | Release (`CMAKE_BUILD_TYPE=Release` in both `CMakeCache.txt`), GCC 16.2.1, target `kds_server` only (`KDS_BUILD_TESTS=OFF`, `KDS_BUILD_SIM=OFF`), built 20:59:12 to 20:59:40 UTC (measured commit) and 20:59:40 to 21:00:04 UTC (`07f4552a`). **Measured copies**: `bin/kds_server`, **sha256 `a333ee48127aff3eab91a1f115a8e4bf9cd2b35d725a4e1d4a44c1f68052f917`**; A/B arm `bin/kds_server_old`, sha256 `03c8c2e946deaa30d57bfdbdcf1802b962451df12ba1200487c21d9988257ee8`; every server started from a copy |
| Device | data files under `/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354/data/`, **`/dev/mapper/fedora-root`, xfs, an LVM volume on `nvme0n1p3` (Sandisk Optimus 5100 500GB NVMe)** (`df -T` at 21:02 UTC, 12 % used), never tmpfs (`/tmp` is tmpfs on this host and was not used); a fresh server and data file per cell, deleted when the cell finished |
| Host | 24 logical CPUs, Intel Core Ultra 7 270K Plus, **a hybrid CPU: 8 P-cores (CPUs 0 to 7, up to 5.4 to 5.5 GHz) and 16 E-cores (CPUs 8 to 23, up to 4.7 GHz)**, `intel_pstate` active, governor `powersave`, 30 GiB, Linux 7.3.4-300.fc45.x86_64 (Fedora 45), Python 3.15.2; a shared development workstation with other `claude` sessions, one of them building and testing in this repository throughout (section Contamination), and an idle `sqlservr` and an Oracle instance |
| Server config | `log_level = warn`, `buffer_pool_frames = 65536`, `cores` and `durability` per cell, all else default but scenario 4's keys; ports 15704 to 15707, 15710, 15720, 15751 to 15753, never 15432 |
| Quiet host | section Quiet gate |
| 60 s watchdog | `watchdog.sh` (the main run), `watchdog2.sh` (the pinned A/B) and `watchdog3.sh` (scenario 4's run A again) poll the status file's age, the current cell's output age and the runner's recorded PID, never a process pattern; section Watchdog |
| PostgreSQL | **not measured**: the host has no PostgreSQL server or client (`pg_ctl`, `initdb`, `postgres`, `psql` absent) |
| Suite | **not executed** by this measurement. No test was built or run at `9f8f8b354` in this session |
| Time budget | 46 minutes, building included, outside 28 minutes of gate hold (`CLAUDE.md`, 2026-11-18: the time a quiet gate holds a measurement is outside the budget). **Cut**, as at `v3.5.0`: the extreme cells of scenario 2 (20,000 bookings, 32 bookers, one and two client processes), scenario 1's join-order probe, scenario 3's plan replies, scenario 4's six-run interleaved A/B, and PostgreSQL. **Added** to the `v3.5.0` shape: an eight-core booking cell and an indexed library cell in the A/B; the pinned A/B series (60 cells); scenario 4's run A a second time |

## What counts as a finding

As at `v3.5.0`: a cell is run three times (the booker sweep, the saturation and extras cells and each scenario 4 run once). **Spread = (max - min) / median over the runs of one cell.** A difference between two medians smaller than the larger of the two spreads is not a finding, and a cell above 20 % carries no delta claim. The A/B follows the same rule on its own spreads. **A ratio against the `v3.5.0` run is never a finding here**, whatever its spread: the two runs are on different hosts.

## Headlines

Warm qps (scenario 1), tps (2) and derived qps (3), median of three runs. "Now over `v3.5.0` run" is this host over the other host:

| measure | now (`9f8f8b354`, this host) | spread | `v3.5.0` run (`07f4552a`, other host) | spread | now over `v3.5.0` run (host + engine) |
|---|---|---|---|---|---|
| 1 backtest, `day-slice` warm (60,480 bars) | 1,079.9 qps | 35.9 % | 677.6 qps | 2.6 % | 1.59x |
| 1 backtest, `cross-join` warm | 1,053.7 qps | 22.7 % | 640.7 qps | 5.4 % | 1.64x |
| 1 backtest, `bar-range` warm | 1,939.4 qps | 28.8 % | 1,160.4 qps | 4.9 % | 1.67x |
| 1 backtest, `model-join` warm | 1,622.1 qps | 12.5 % | 898.4 qps | 5.5 % | 1.81x |
| 1 backtest, `result-insert` phase | 4,621.7 qps | 6.1 % | 1,798.6 qps | 3.4 % | 2.57x |
| 2 freight, 8 bookers, `cores = 1`, `group` | 3,174.5 tps | 3.2 % | 1,172.7 tps | 4.3 % | 2.71x |
| 2 freight, 8 bookers, `cores = 8`, `group` | 8,111.5 tps | 10.3 % | 2,612.6 tps | 4.8 % | 3.10x |
| 2 freight, 8 bookers, `cores = 8`, `strict` | 7,521.4 tps | 55.1 % | 2,384.5 tps | 5.1 % | 3.15x |
| 2 freight, 1 booker, `strict` | 1,333.0 tps | 28.0 % | 631.3 tps | 2.1 % | 2.11x |
| 2 freight, 32 bookers, `cores = 8`, `group` (one run) | 10,475.1 tps | n/a | 3,735.5 tps | n/a | 2.80x |
| 3 library, `loans-by-user` (10,000 loans, unindexed) | 5,388 qps | 28.1 % | 3,180 qps | 4.8 % | 1.69x |
| 3 library, `join-no-literal` (10,000 loans, unindexed) | 3,396 qps | 24.8 % | 1,954 qps | 4.0 % | 1.74x |
| 3 library, `exists-correlated` (10,000 loans, unindexed) | 2,945 qps | 40.0 % | 1,520 qps | 2.9 % | 1.94x |
| 3 library, `loans-by-user` (10,000 loans, indexed) | 18,083 qps | 37.3 % | 12,180 qps | 8.0 % | 1.48x |

The engine, isolated (new over old on this host, pinned series; the unpinned series is in each results file):

| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| 1 backtest, `day-slice` warm | 1,376.2 qps | 3.5 % | 1,377.9 qps | 8.7 % | 1.00x | inside the spread |
| 1 backtest, `cross-join` warm | 1,342.0 qps | 6.7 % | 1,373.5 qps | 4.2 % | 1.02x | inside the spread |
| 1 backtest, `bar-range` warm | 1,739.0 qps | 6.4 % | 1,737.3 qps | 4.9 % | 1.00x | inside the spread |
| 1 backtest, `model-join` warm | 1,515.3 qps | 1.8 % | 1,517.4 qps | 3.2 % | 1.00x | inside the spread |
| 1 backtest, `symbol-history` warm | 41.7 qps | 1.0 % | 41.9 qps | 1.2 % | 1.00x | inside the spread |
| 1 backtest, phase `backtest-read` | 1,309.2 qps | 4.3 % | 1,317.9 qps | 9.8 % | 1.01x | inside the spread |
| 1 backtest, phase `result-insert` | 4,805.1 qps | 8.2 % | 4,759.9 qps | 50.3 % | 0.99x | spread > 20 %, no claim |
| 1 backtest, phase `load-bars` | 45,136.1 qps | 1.3 % | 44,837.0 qps | 3.0 % | 0.99x | inside the spread |
| 1 backtest, phase `agg-by-session` | 79.7 qps | 1.5 % | 79.3 qps | 3.3 % | 0.99x | inside the spread |
| 2 freight, 8 bookers, `cores = 1`, `group` | 4,192.8 tps | 88.3 % | 4,228.0 tps | 6.3 % | 1.01x | spread > 20 %, no claim |
| 2 freight, 1 booker, `cores = 1`, `strict` | 1,749.5 tps | 0.9 % | 1,749.7 tps | 1.1 % | 1.00x | inside the spread |
| 2 freight, 8 bookers, `cores = 8`, `group` | 9,543.2 tps | 3.9 % | 9,801.8 tps | 58.4 % | 1.03x | spread > 20 %, no claim |
| 3 library, `pk-user` (10,000 loans, unindexed) | 30,303.0 qps | 15.4 % | 30,864.2 qps | 4.2 % | 1.02x | inside the spread |
| 3 library, `loans-by-user` (10,000 loans, unindexed) | 6,887.1 qps | 5.5 % | 6,839.9 qps | 1.0 % | 0.99x | inside the spread |
| 3 library, `join-no-literal` (10,000 loans, unindexed) | 4,147.7 qps | 2.9 % | 4,116.9 qps | 5.4 % | 0.99x | inside the spread |
| 3 library, `exists-correlated` (10,000 loans, unindexed) | 4,095.0 qps | 2.1 % | 4,122.0 qps | 4.6 % | 1.01x | inside the spread |
| 3 library, `count-by-user` (10,000 loans, unindexed) | 7,501.9 qps | 0.6 % | 7,418.4 qps | 1.0 % | 0.99x | **finding** |
| 3 library, `pk-user` (10,000 loans, indexed) | 18,975.3 qps | 10.1 % | 18,181.8 qps | 13.8 % | 0.96x | inside the spread |
| 3 library, `loans-by-user` (10,000 loans, indexed) | 25,062.7 qps | 6.4 % | 24,813.9 qps | 2.2 % | 0.99x | inside the spread |
| 3 library, `join-no-literal` (10,000 loans, indexed) | 8,888.9 qps | 3.1 % | 8,635.6 qps | 7.5 % | 0.97x | inside the spread |
| 3 library, `exists-correlated` (10,000 loans, indexed) | 17,064.8 qps | 5.4 % | 16,420.4 qps | 2.9 % | 0.96x | inside the spread |
| 3 library, `count-by-user` (10,000 loans, indexed) | 34,722.2 qps | 2.1 % | 34,482.8 qps | 3.1 % | 0.99x | inside the spread |

Scenario 4, ratio of mean latencies over the `off` arm, days 1 to 3 (the last column is the `v3.5.0` run on its own host):

| group | run | `on` over `off`, days 1 / 2 / 3 | `declared` over `off`, days 1 / 2 / 3 | `on` over `off`, `v3.5.0` run |
|---|---|---|---|---|
| `board` | A (`s4`, overlapped a build) | 1.12x / 1.17x / 1.16x | 1.77x / 1.87x / 2.35x | 0.99x / 1.00x / 1.00x |
| `board` | A again (`r2-s4`) | 1.21x / 1.08x / 1.25x | 1.69x / 1.65x / 2.27x | 0.99x / 1.00x / 1.00x |
| `board` | B (`cd2-s4`) | 1.24x / 1.19x / 1.12x | 1.79x / 1.82x / 2.06x | 0.98x / 0.98x / 1.00x |
| `board` | `07f4552a` binary (`ab-old1-s4`) | 1.20x / 1.22x / 1.12x | 1.84x / 1.84x / 2.11x | 0.99x / 1.00x / 1.00x |
| `tape10k` | A (`s4`, overlapped a build) | 0.98x / 1.00x / 1.02x | 1.80x / 1.98x / 2.02x | 1.00x / 1.01x / 1.00x |
| `tape10k` | A again (`r2-s4`) | 0.98x / 0.91x / 1.06x | 1.66x / 2.12x / 2.05x | 1.00x / 1.01x / 1.00x |
| `tape10k` | B (`cd2-s4`) | 1.04x / 1.02x / 0.97x | 1.71x / 2.21x / 1.89x | 0.95x / 1.02x / 0.98x |
| `tape10k` | `07f4552a` binary (`ab-old1-s4`) | 1.06x / 0.98x / 0.97x | 1.79x / 2.16x / 1.67x | 1.00x / 1.01x / 1.00x |
| `pk` | A (`s4`, overlapped a build) | 1.04x / 0.97x / 0.91x | 1.17x / 0.99x / 1.12x | 0.96x / 1.04x / 1.08x |
| `pk` | A again (`r2-s4`) | 1.08x / 0.87x / 1.19x | 1.16x / 1.39x / 1.15x | 0.96x / 1.04x / 1.08x |
| `pk` | B (`cd2-s4`) | 1.10x / 1.02x / 0.86x | 1.12x / 1.34x / 1.16x | 0.89x / 1.02x / 0.91x |
| `pk` | `07f4552a` binary (`ab-old1-s4`) | 0.97x / 0.96x / 0.97x | 1.10x / 1.18x / 0.94x | 0.96x / 1.04x / 1.08x |

Files: `results-scenario1-backtest-v3.4.17-159-g9f8f8b354.md`, `results-scenario2-freight-v3.4.17-159-g9f8f8b354.md`, `results-scenario3-library-v3.4.17-159-g9f8f8b354.md`, `results-scenario4-cabinopt-days-v3.4.17-159-g9f8f8b354.md` (all under `bench/v3.5.0/`).

## The movers, and what they are

- **Nothing moved in the engine between `07f4552a` and `9f8f8b354` that scenarios 1 to 3 can read** (DL's statistics paths, DK's write descent, DD's index purge, DN's heap, DO's frame table holds, DP's views, DQ's Cabin miss, DR's primary keys, DU's keyed writes; superblock 38 to 41). The pinned A/B is level on every row it resolves.
- **Two rows are marked, at the floor.** Scenario 3 `count-by-user` unindexed, -1.1 % against spreads of 1.0 % and 0.6 % (its indexed twin 0.99x, inside). Scenario 2 `org-update` at eight cores, +4.6 % against 4.2 %, beside `charge-insert` and `operation-update` at 1.05x inside their spreads and a tps of 1.03x without a claim. Neither is attributed to a milestone.
- **Not resolved**: scenario 1's statements under about 100 µs and its two Cabin-served shapes above 10,000 qps (`day-slice` cabin 0.91x, `cross-join` cabin 0.85x pinned, new-arm spreads over 20 %; 1.02x and 0.98x unpinned). Five pairs do not settle them.
- **Two stalled runs in the pinned scenario 2 cells**, one in each engine (526.1 tps where the other nine runs read 3,991.7 to 4,257.2 tps; 4,149.1 tps where the others read 9,288.8 to 9,874.0 tps), with no foreign CPU sample. Cause unverified; the monitor does not sample I/O.
- **Scenario 4 reads as at `v3.5.0`**: the controller's Cabins are unprobed (PO11) and a declared Cabin serves. `on over off` on `board` is 1.08x to 1.25x here on both binaries with no lookup recorded on either, so it is not a Cabin and not a change between the engines; the `07f4552a` binary read 0.99x to 1.00x on the `v3.5.0` run's host, so the ratio follows the host, and its cause here is unverified.
- **Both engines refuse a few inserts `TXN_CONFLICT` at eight cores from a btree descent that gave up after 5 attempts** (24 over the old arm's eight eight-core A/B cells, 13 over the new arm's); the driver retries them.
- **One abandoned booking** in 80 scenario 2 cells (`r2-s2-c8-s`, out of retries at `--max-retries 5`).

## What this run does not show

- **No PostgreSQL number** (no server on this host).
- **No engine delta from the cross-host columns.** The measured cells are 1.3x to 3.8x the `v3.5.0` run's numbers cell by cell, and that is this workstation against that VM.
- **A quiet-host reading.** Another session built and tested in this repository for the whole window; the gate kept those processes off a cell's start, not out of its middle (section Contamination).
- **The controller's effect under a plan that names its Cabins**: the scenario 4 driver writes no `CREATE PLAN`, as at `v3.5.0`.
- **Scenario 2's extreme cells** (cut) and the engine's own ceiling under them.
- **The suite at `9f8f8b354`**: not executed here.
- Anything after `9f8f8b354`.

## Quiet gate

Before each cell a gate waited for no build or test process and at most 4 % busy CPU over 1 s (24 CPUs: about one core; the `v3.5.0` run's gate was 8 % of 8 CPUs over 3 s). Processes are matched by `comm` (`cc1plus`, `ctest`, `kds_tests`, `cmake`, `ninja`, `lto1`), never by argv. **The gate's rule changed once, at 21:38 UTC**: until then the existence of such a process held the gate, and another session's `kds_tests --gtest_repeat=20`, asleep at 0.9 % of one CPU, had held `ab-old2-s1-c1-g`'s gate for 315 s with no end in sight; since then a process holds the gate only while it uses 5 % or more of one CPU over the sample or exits inside it. Of the 186 cells, 173 started with no wait and 13 were held, 1,691 s in all: 134 s over six cells of the main run, 1,035 s over two of the unpinned A/B (`ab-new1-s2-c1-g` 720 s, `ab-old2-s1-c1-g` 315 s) and 522 s over five cells of the pinned A/B's first pair. 95 cells started under the first rule and 91 under the second; 41 of the 91 started with an idle build or test process present. The gate guards a cell's start only: section Contamination lists what ran inside one.

## Watchdog

No watchdog found an orphan or a stall: `watchdog.sh` logged 45 lines of one-minute checks, `watchdog2.sh` and `watchdog3.sh` the rest, and none logged `ORPHANED`. The run was stopped and restarted by hand three times, each time between cells or at a gate, and each restart resumed from the per-cell `.rc` files:

1. **21:03:49 UTC: the first launch (three cells, from 21:02:43) was stopped and discarded whole.** Its scenario 2 cell would have run one invocation where the `v3.5.0` run ran three (`--schema-only`, `--load-only`, the run), its size sweep lacked `--exchanges`, and it had no foreign-process monitor. The `v3.5.0` run's own scripts (`bench/v3.5.0/archive/run-v3.5.0/`) were read at that point and the runner rewritten to their cells, arguments and suffixes. Nothing of the first launch is in any table; its status file is archived as `logs/attempt1-status`.
2. **21:38:36 UTC: stopped at a gate to change the gate's rule** (section Quiet gate).
3. **21:59:38 UTC: the pinned series was stopped at a gate** so that its deadline would follow the holds inside it.

The pinned series and scenario 4's second run A were queued behind the main run as separate scripts (`run_pin.sh`, `run_s4r.sh`), each with its own watchdog.

**Disclosure.** At 21:59 UTC this session ran `ctest -N` against the other session's build directory (`.claude/worktrees/dt-write-path-yield/build`) to count the suite holding the gate (5,286 tests). It ran no test, but that directory's `Testing/Temporary/LastTest.log` was 121 bytes afterwards, so the listing appears to have replaced the log of the suite then running there.

## Contamination

No cell was discarded. A monitor sampled the host about every 2 s for foreign processes above 10 % of one CPU. Seven cells carry samples in which those processes sum to 50 % or more of one CPU (the `claude` CLI's own processes not counted), all from the other session's build and test cycle in the worktree `dt-write-path-yield`, kept and disclosed:

- **scenario 4 run A (`s4`)**: 43 of 123 samples, 8.3 CPUs' worth on average (a full build). Run again as `r2-s4`, which carries none.
- **scenario 4 run B (`cd2-s4`)**: 19 of 122 samples, `kds_tests` at about one CPU.
- `s1-c8-g` run 1 (5 of 8 samples, `cc1plus` and `ld`), `ab-new1-s1-c1-g` (2 of 9, `cc1plus`), `r2-s3-c1-n10000-none` (2 of 2), `r2-s3-c8-n10000-all` (2 of 3) and `r3-s3-c8-n10000-none` (3 of 3; `cc1plus`).

One sample inside `s2-c1-g` run 1 shows six `python3 -c` processes (pids 629971 to 629977) at 10.9 % each: the scenario 2 driver's own booker processes, which the monitor's filter did not name. They are not foreign and `contam.py` leaves them out. No scenario 2 cell, no pinned A/B cell and not the `07f4552a`-binary scenario 4 run carries a foreign one. **What the monitor does not see is I/O**: the two stalled cells of the pinned series (scenario 2, section 9) show no foreign CPU, and a device stall from another process is not excluded. **Disclosure:** the table generator ran and the documents were drafted by this session while cells were live, so `claude` processes are in the monitor at up to 38.6 % of one CPU (87 entries in 86 of the 1,784 samples); they are excluded from the counts above.

## Archive

- `bench/v3.5.0/archive/scenarioN-v3.4.17-159-g9f8f8b354/`: each scenario's per-cell JSON, driver stdout, precheck (load average, process scan, gate wait), start and end times and server configs and logs. The A/B cells (`ab-old*`, `ab-new*`), the pinned ones (`pin-ab-*`), the extras and the `07f4552a`-binary scenario 4 run (`ab-old1-s4`) are among them.
- `bench/v3.5.0/archive/run-v3.4.17-159-g9f8f8b354/`: the run scripts (`run.sh`, `run_pin.sh`, `run_s4r.sh`, the two pin wrappers), the watchdogs and the monitor, their logs, the status timeline and the gate holds, the binary hashes and build times, the table generator `gen.py`, `contam.py`, the document templates and the generated tables under `tables/`. No data file is kept.
