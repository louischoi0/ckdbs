# Scenarios 1 to 4 at `v3.4.17-159-g9f8f8b354`: summary

**One line.** @@SUM_ONE_LINE@@

This file indexes the four results files and carries no number they do not. Measured directly by CLA in a scheduled session (ordered 2026-11-26 14:58 UTC, "in 6 hours, run the scenario 1, 2, 3, 4 benchmarks on the latest version of kdbs"; fired 20:58:08 UTC): @@SUM_TIMELINE@@

**Comparators.** The comparator run is the `v3.5.0` run (`07f4552a`), `bench/v3.5.0/results-scenario{1-backtest,2-freight,3-library,4-cabinopt-days}-v3.5.0.md`, measured 2026-11-20 **on another host** (8 logical CPUs, Intel Xeon Platinum 8581C at 2.10 GHz, `/dev/nvme0n1p1` ext4). This host is not that host and this device is not that device, so every "now over `v3.5.0` run" column is host plus engine and none of it is read as an engine change. **The engine reading is the same-host A/B**: a `07f4552a` binary built here from `git archive 07f4552a` (Release) against the measured binary, one driver (the four drivers and `bench_common.py` are byte-identical in the two commits), interleaved old, new; three pairs unpinned as at `v3.5.0`, and @@PIN_PAIRS@@ pairs again with the server pinned to the P-cores, which is the series that resolves on this host. Scenario 4's engine reading is one `07f4552a`-binary run.

## Stamp (common to the four results files)

| Field | Value |
|---|---|
| Version, commit | **`v3.4.17-159-g9f8f8b354`** (`git describe --tags` at `9f8f8b354`, the tip of `origin/main` after `git fetch origin` at 20:58 UTC; local `main` was at `60dc593af`, 9 commits behind, and was not moved). **The files are under `bench/v3.5.0/`, the version of record `CLAUDE.md` names, but the `v3.5.0` tag is absent from this clone** (it is local to the clone that made it), so the describe string counts from `v3.4.17`; `07f4552a`, the commit that tag names, is an ancestor, 142 commits back. No tag was created and nothing was pushed. Built from `git archive 9f8f8b354` extracted under `/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354/src/`; no build ran in any worktree's `build` or `build-release` |
| What is between the two commits | `07f4552a..9f8f8b354`: DL, DK, DD, DN (superblock 39), DO, DP (40), DQ, DR (41), DU, and DG, DM and CW closed with nothing built. A data file of one binary does not mount on the other; every cell creates its own |
| Tree cleanliness | an extraction has no working tree; the drivers are the measured commit's unmodified `tools/scenario{1,2,3,4}_*.py` |
| Binary provenance | Release (`CMAKE_BUILD_TYPE=Release` in both `CMakeCache.txt`), GCC 16.2.1, target `kds_server` only (`KDS_BUILD_TESTS=OFF`, `KDS_BUILD_SIM=OFF`), built 20:59:12 to 20:59:40 UTC (measured commit) and 20:59:40 to 21:00:04 UTC (`07f4552a`). **Measured copies**: `bin/kds_server`, **sha256 `a333ee48127aff3eab91a1f115a8e4bf9cd2b35d725a4e1d4a44c1f68052f917`**; A/B arm `bin/kds_server_old`, sha256 `03c8c2e946deaa30d57bfdbdcf1802b962451df12ba1200487c21d9988257ee8`; every server started from a copy |
| Device | data files under `/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354/data/`, **`/dev/mapper/fedora-root`, xfs, an LVM volume on `nvme0n1p3` (Sandisk Optimus 5100 500GB NVMe)** (`df -T` at 21:02 UTC, 12 % used), never tmpfs (`/tmp` is tmpfs on this host and was not used); a fresh server and data file per cell, deleted when the cell finished |
| Host | 24 logical CPUs, Intel Core Ultra 7 270K Plus, **a hybrid CPU: 8 P-cores (CPUs 0 to 7, up to 5.4 to 5.5 GHz) and 16 E-cores (CPUs 8 to 23, up to 4.7 GHz)**, `intel_pstate` active, governor `powersave`, 30 GiB, Linux 7.3.4-300.fc45.x86_64 (Fedora 45), Python 3.15.2; a shared development workstation with other `claude` sessions, one of them building and testing in this repository throughout (section Contamination), and an idle `sqlservr` and an Oracle instance |
| Server config | `log_level = warn`, `buffer_pool_frames = 65536`, `cores` and `durability` per cell, all else default but scenario 4's keys; ports 15704 to 15707, 15710, 15720, 15751 to 15753, never 15432 |
| Quiet host | section Quiet gate |
| 60 s watchdog | `watchdog.sh` (the main run), `watchdog2.sh` (the pinned A/B) and `watchdog3.sh` (scenario 4's run A again) poll the status file's age, the current cell's output age and the runner's recorded PID, never a process pattern; section Watchdog |
| PostgreSQL | **not measured**: the host has no PostgreSQL server or client (`pg_ctl`, `initdb`, `postgres`, `psql` absent) |
| Suite | **not executed** by this measurement. No test was built or run at `9f8f8b354` in this session |
| Time budget | @@SUM_BUDGET@@ |

## What counts as a finding

As at `v3.5.0`: a cell is run three times (the booker sweep, the saturation and extras cells and each scenario 4 run once). **Spread = (max - min) / median over the runs of one cell.** A difference between two medians smaller than the larger of the two spreads is not a finding, and a cell above 20 % carries no delta claim. The A/B follows the same rule on its own spreads. **A ratio against the `v3.5.0` run is never a finding here**, whatever its spread: the two runs are on different hosts.

## Headlines

Warm qps (scenario 1), tps (2) and derived qps (3), median of three runs. "Now over `v3.5.0` run" is this host over the other host:

@@SUM_HEADLINES@@

The engine, isolated (new over old on this host, pinned series; the unpinned series is in each results file):

@@SUM_ENGINE_TABLE@@

Files: `results-scenario1-backtest-v3.4.17-159-g9f8f8b354.md`, `results-scenario2-freight-v3.4.17-159-g9f8f8b354.md`, `results-scenario3-library-v3.4.17-159-g9f8f8b354.md`, `results-scenario4-cabinopt-days-v3.4.17-159-g9f8f8b354.md` (all under `bench/v3.5.0/`).

## The movers, and what they are

@@SUM_MOVERS@@

## What this run does not show

- **No PostgreSQL number** (no server on this host).
- **No engine delta from the cross-host columns.** The measured cells are 1.3x to 3.8x the `v3.5.0` run's numbers cell by cell, and that is this workstation against that VM.
- **A quiet-host reading.** Another session built and tested in this repository for the whole window; the gate kept those processes off a cell's start, not out of its middle (section Contamination).
- **The controller's effect under a plan that names its Cabins**: the scenario 4 driver writes no `CREATE PLAN`, as at `v3.5.0`.
- **Scenario 2's extreme cells** (cut) and the engine's own ceiling under them.
- **The suite at `9f8f8b354`**: not executed here.
- Anything after `9f8f8b354`.

## Quiet gate

Before each cell a gate waited for no build or test process and at most 4 % busy CPU over 1 s (24 CPUs: about one core; the `v3.5.0` run's gate was 8 % of 8 CPUs over 3 s). Processes are matched by `comm` (`cc1plus`, `ctest`, `kds_tests`, `cmake`, `ninja`, `lto1`), never by argv. **The gate's rule changed once, at 21:38 UTC**: until then the existence of such a process held the gate, and another session's `kds_tests --gtest_repeat=20`, asleep at 0.9 % of one CPU, had held `ab-old2-s1-c1-g`'s gate for 315 s with no end in sight; since then a process holds the gate only while it uses 5 % or more of one CPU over the sample or exits inside it. @@SUM_GATE@@

## Watchdog

@@SUM_WATCHDOG@@

## Contamination

@@SUM_CONTAM@@

## Archive

- `bench/v3.5.0/archive/scenarioN-v3.4.17-159-g9f8f8b354/`: each scenario's per-cell JSON, driver stdout, precheck (load average, process scan, gate wait), start and end times and server configs and logs. The A/B cells (`ab-old*`, `ab-new*`), the pinned ones (`pin-ab-*`), the extras and the `07f4552a`-binary scenario 4 run (`ab-old1-s4`) are among them.
- `bench/v3.5.0/archive/run-v3.4.17-159-g9f8f8b354/`: the run scripts (`run.sh`, `run_pin.sh`, `run_s4r.sh`, the two pin wrappers), the watchdogs and the monitor, their logs, the status timeline and the gate holds, the binary hashes and build times, the table generator `gen.py`, `contam.py`, the document templates and the generated tables under `tables/`. No data file is kept.
