# Scenario 4 (cabin optimizer over business days) at `v3.3.0-33-g9682b725`

**One line.** Three business days on three servers (a Cabin controller `off`, `on`, and a Cabin `declared` up front), run twice: the controller still lifts an equality filter that a walk serves slowly by 1.6x to 2.3x (`board` 1.78x on, 1.58x declared; `tape10k` 2.31x and 1.84x, medians over the six days) and changes nothing it cannot serve
(`tape200`, `pk`, `open`, `close` inside the floor), and its lifecycle is identical to v3.1.0's: four Cabins in the first block, a fifth on day 2, `board_a` DECAYING on day 2 and back to ACTIVE on day 3 with its pages
doubled, and with `cabin_optimizer_cooldown_half_lives = 2` two DROPs and one re-nomination. The change against v3.1.0 (cross-version) is in the walk: the arm that has no Cabin, `off`, is
**1.18x on `board` and 1.20x on `tape10k`** (2,329 qps against 1,967 qps), the per-row cost of an unserved equality falls from 34.6 to 26.5 ns, and the server CPU an `off` run spends on its boards
falls from 2.80 s to 2.22 s, while the `pk` control does not move (mean 73.4 µs against 70.1 µs). Against PostgreSQL 18.6, **re-measured in the same window**, day 1 with no index: `board`
1.19x walking, 2.11x with the controller; `close` is 0.57x.

Driver: `tools/scenario4_cabinopt_days.py`, unmodified, two complete runs: **run A** at the server defaults (`s4`) and **run B** with `cabin_optimizer_cooldown_half_lives = 2` (`cd2-s4`; run A cannot exercise DROP, section 6),
both with `decay_half_life = 5` and `cabin_optimizer_snapshot_interval_ms = 500`. Measured by `ck-tester`. Comparator (cross-version): `bench/v3.1.0/results-scenario4-cabinopt-days-v3.1.0-30-gcd7c7c4e.md`, re-derived from
its archived JSON. PostgreSQL 18.6: `tools/pg_scenario4_cabinopt_days.py`, **fresh, three runs**.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; run A 04:08:47 to 04:13:37 UTC, run B 04:13:41 to 04:18:30 UTC; PostgreSQL 04:24:54 to 04:32:17 UTC |
| Worktree / branch | `bench-scenarios-1-4` / `bench-scenarios-1-4` (the main checkout is not used) |
| HEAD | `9682b725`, `git describe --tags` = `v3.3.0-33-g9682b725` ("Merge fix-fk-self-reference-refused: a self-referencing foreign key is refused NotImplemented at both doors"), committed 2026-10-20 03:33:40 UTC; equal to `origin/main` when the run began |
| Tree cleanliness | Clean (`git status` empty) at the start and at the end of the measurement; nothing under `src/`, `include/`, `tools/` or `CMakeLists.txt` was edited, and the drivers are the unmodified `tools/` scripts. `build-release/` is ignored by git. The results files under `bench/v3.3.0/` are uncommitted additions until the commit that carries them |
| Binary provenance | `build-release/kds_server`, built in this worktree at HEAD with `cmake -DCMAKE_BUILD_TYPE=Release` (`CMakeCache.txt`: `Release`), target `kds_server`, `-j4`, finished 2026-10-20 03:36:57 UTC, 3 min 17 s **after** the HEAD commit, so the binary is not older than the engine. Copied to `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/kds_server` before the first cell; **copy sha256 `7532bd14afb2acd4f9e48cac00732f899cb8e59b26fa1d9b982047fc89272f2a`** (equal to the source binary's, checked again after the run); source binary mtime 2026-10-20 03:36:57.38 UTC. Every server started from the copy; no build ran during any reported cell (the only build of the session finished before the first cell) |
| Device | data files under `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/data/`, `/dev/root`, `ext4` (`df -T`); never tmpfs. A fresh data file per cell (per server in scenario 4), deleted when the cell finished. The PostgreSQL cluster (`/home/cdkbs/pg-bench-ba`) is on the same device |
| Build type | Release |
| Host | 8 logical CPUs, AMD EPYC 9V74 (1 socket x 4 cores x 2 threads), Linux 7.0.0-1014-azure, a shared development VM; `/` was 95 % full (14 GB free) |
| Time budget | **Budget lifted by the operator for this run** (order of 2026-10-20: "lift the budget, re-measure PG too"). Every cell of the v3.1.0 rebaseline's plan ran, with its repeat counts, and PostgreSQL was re-measured fresh in the same window. Nothing was cut |
| Prior run of this shape | `bench/v3.1.0/results-scenario4-cabinopt-days-v3.1.0-30-gcd7c7c4e.md` (KDS binary `cd7c7c4e`, sha256 `4752f562...`, measured 2026-10-13 23:38 UTC to 2026-10-14 01:39 UTC). The two commits are 240 commits and 101 first-parent merges apart (`git rev-list --count cd7c7c4e..HEAD`), with 178 files changed under `src/`, `include/` and `CMakeLists.txt` (milestones BV to CD among them), so **every KDS delta below is cross-version and attributable to no single change**. The host and its device state are also a week-old comparison: v3.1.0's cells ran on the same VM, 6.5 days earlier |
| Server config | `log_level = warn`, `buffer_pool_frames = 65536` (required since BE), `cores` and `durability` per cell, all else default. Ports 15604 to 15607, 15610, 15620, 15651 to 15653, never 15432 (held by an unrelated `kds_server`) |
| Quiet host | before each cell a gate waited for no `cc1plus` / `cmake --build` / `ctest` / `kds_tests` process and at most 8 % busy CPU over 3 s (`lib.sh` in the archive); `/proc/loadavg` and the process list are in each cell's precheck. **A 1-second monitor ran through the whole run** (`monitor.log`: 1,500+ samples of foreign processes above 10 % CPU) and every cell was checked against it after it ended (`cellvalid.py`): a build/test process or a foreign process at 50 % CPU or more inside the window discards and re-runs the cell. **No cell was discarded**: the monitor saw only a `claude` process at 11 to 23 % of one CPU in 23 samples of the 1,518, all outside the 50 % line. One batch of PostgreSQL cells failed on a connection error and is listed under contamination below |
| PostgreSQL | PostgreSQL 18.6 (`/home/cdkbs/pg-bench-ba`, port 15434), defaults (`shared_buffers = 128MB`, `synchronous_commit = on`, `fsync = on`, `max_wal_size = 1GB`), not started or stopped by this run. **Re-measured fresh in this window** (the twin runs follow the KDS cells, never concurrent with them) |
| Driver | **unmodified** `tools/scenario4_cabinopt_days.py`, `--port-off/-on/-declared` with the three servers' PIDs for the CPU readings, `--suffix a`; exit 0 in both runs, `client_errors` 0 and 0, `slot_overruns` 0 and 0; the driver's byte-for-byte comparison of 18 statements per day across the three arms passed on all 6 days |
| Server config | the common config plus `decay_half_life = 5`, `cabin_optimizer_snapshot_interval_ms = 500` on all three servers, and in run B `cabin_optimizer_cooldown_half_lives = 2`; `cores = 1`, `durability = group`; ports 15651 to 15653 |

## 2. What was run

Two complete runs of the driver, three days of three arms each. Per day: an open (240 inserts into the day's board), a paced 45 s trading session (2,400 hot-board probes, 396 probes on each of three tape sizes,
240 pk controls, per arm, interleaved in 12 blocks), a close (`COUNT` / `SUM` / `GROUP BY` full scans, three rounds) and a 45 s idle night. The equal work per arm is fixed by the driver's counts, not by time.

    python3 tools/scenario4_cabinopt_days.py --port-off 15651 --port-on 15652 --port-declared 15653 \
        --pid-off <p> --pid-on <p> --pid-declared <p> --suffix a --json <run>.json

**Row-set axis (rule 9).** The tape relations are 200, 1,000 and 10,000 rows (`tape_200`, `tape_1k`, `tape_10k`); the boards are 10,000 rows each (`board_a`, `board_b`). Every phase is a row of the matrix, the size is
a row label. The probes are serial on one connection, so qps is derived as 1,000,000 / mean µs (the table cells give the p50 in µs beside the derived qps). The hot set rotates: day 1 hot on `board_a`, day 2 on `board_b`,
day 3 on `board_a` with a different symbol set; the tapes are hot every day with disjoint values.

Cut: nothing. Time budget lifted by the operator for this run; the v3.1.0 plan (two runs) ran in full. PostgreSQL's twin runs day 1 only, unpaced, no index, by its own design.

## 3. Throughput and latency by arm

p50 and derived qps (from the mean) per group, day and arm for **run A**; the last two columns are the ratio of mean latencies, off over on and off over declared, for run A and run B:

| group | day | off A (p50 / qps) | on A (p50 / qps) | declared A (p50 / qps) | on over off, mean (A / B) | declared over off, mean (A / B) |
|---|---|---|---|---|---|---|
| board | 1 | 426.8 µs / 2,344 qps | 182.4 µs / 4,189 qps | 175.7 µs / 3,621 qps | 1.79x / 1.77x | 1.54x / 1.55x |
| board | 2 | 429.0 µs / 2,349 qps | 186.3 µs / 4,129 qps | 179.2 µs / 3,685 qps | 1.76x / 1.78x | 1.57x / 1.58x |
| board | 3 | 440.1 µs / 2,279 qps | 178.1 µs / 4,742 qps | 178.4 µs / 4,195 qps | 2.08x / 1.74x | 1.84x / 1.86x |
| tape200 | 1 | 102.0 µs / 9,276 qps | 102.4 µs / 9,217 qps | 100.3 µs / 9,461 qps | 0.99x / 1.03x | 1.02x / 1.04x |
| tape200 | 2 | 101.4 µs / 9,497 qps | 102.1 µs / 9,294 qps | 100.0 µs / 9,285 qps | 0.98x / 1.03x | 0.98x / 1.02x |
| tape200 | 3 | 106.2 µs / 8,811 qps | 102.2 µs / 9,302 qps | 100.7 µs / 9,225 qps | 1.06x / 1.00x | 1.05x / 1.08x |
| tape1k | 1 | 123.2 µs / 7,722 qps | 107.1 µs / 8,741 qps | 101.8 µs / 8,787 qps | 1.13x / 1.17x | 1.14x / 1.18x |
| tape1k | 2 | 123.1 µs / 7,868 qps | 101.4 µs / 9,124 qps | 102.2 µs / 8,881 qps | 1.16x / 1.20x | 1.13x / 1.18x |
| tape1k | 3 | 131.4 µs / 7,241 qps | 101.9 µs / 9,132 qps | 102.3 µs / 9,091 qps | 1.26x / 1.14x | 1.26x / 1.25x |
| tape10k | 1 | 364.5 µs / 2,679 qps | 109.4 µs / 5,376 qps | 105.1 µs / 4,521 qps | 2.01x / 1.94x | 1.69x / 1.69x |
| tape10k | 2 | 360.9 µs / 2,740 qps | 104.5 µs / 6,557 qps | 102.3 µs / 5,359 qps | 2.39x / 2.38x | 1.96x / 1.93x |
| tape10k | 3 | 367.2 µs / 2,693 qps | 104.6 µs / 6,523 qps | 106.8 µs / 4,854 qps | 2.42x / 2.25x | 1.80x / 1.89x |
| pk | 1 | 67.1 µs / 13,210 qps | 65.4 µs / 13,477 qps | 63.6 µs / 14,225 qps | 1.02x / 1.07x | 1.08x / 1.10x |
| pk | 2 | 64.7 µs / 14,065 qps | 64.9 µs / 14,045 qps | 64.1 µs / 13,812 qps | 1.00x / 1.05x | 0.98x / 1.09x |
| pk | 3 | 67.0 µs / 13,263 qps | 65.0 µs / 14,065 qps | 64.9 µs / 13,793 qps | 1.06x / 0.97x | 1.04x / 1.07x |
| open | 1 | 1,210.4 µs / 795 qps | 1,164.4 µs / 839 qps | 1,139.5 µs / 827 qps | 1.06x / 1.09x | 1.04x / 1.06x |
| open | 2 | 1,179.5 µs / 786 qps | 1,185.0 µs / 794 qps | 1,199.9 µs / 813 qps | 1.01x / 1.04x | 1.03x / 1.03x |
| open | 3 | 1,158.0 µs / 837 qps | 1,234.8 µs / 778 qps | 1,162.1 µs / 830 qps | 0.93x / 1.05x | 0.99x / 0.97x |
| close | 1 | 755.9 µs / 671 qps | 776.5 µs / 687 qps | 797.7 µs / 681 qps | 1.02x / 0.99x | 1.02x / 0.97x |
| close | 2 | 716.0 µs / 692 qps | 787.9 µs / 680 qps | 786.5 µs / 601 qps | 0.98x / 0.99x | 0.87x / 0.88x |
| close | 3 | 701.0 µs / 697 qps | 799.8 µs / 678 qps | 815.1 µs / 670 qps | 0.97x / 1.00x | 0.96x / 1.00x |

**What is resolved and what is not.** The `pk` control touches no Cabin, and its on-over-off ratio ranges 0.97x to 1.07x (declared 0.98x to 1.10x) across days and runs: **a ratio below about 1.10x is not a finding**. That puts `tape200`
(0.98x to 1.08x) at the floor, `tape1k` (1.13x to 1.26x) over it by a little, and `tape10k` (1.69x to 2.42x), `board` (1.54x to 2.08x) clear of it; `open` and `close` (0.87x to 1.09x) are not measurably different between arms. The
`tape200` result is the row-count lesson (rule 9): at 200 rows the walk is 102 µs against 100 µs for the Cabin, and the fixed round trip is all there is.

**Cross-version (now over v3.1.0, derived qps per arm, run A and run B):**

Derived qps (1,000,000 / mean µs), run A, now over v3.1.0 (cross-version), and run B:

| group | day | off A | on A | declared A | off B | on B | declared B |
|---|---|---|---|---|---|---|---|
| board | 1 | 1.19x | 1.09x | 1.03x | 1.18x | 1.08x | 1.05x |
| board | 2 | 1.20x | 1.05x | 1.03x | 1.17x | 1.06x | 1.01x |
| board | 3 | 1.18x | 1.02x | 1.00x | 1.20x | 1.02x | 1.00x |
| tape200 | 1 | 1.02x | 1.03x | 0.97x | 0.95x | 1.01x | 0.97x |
| tape200 | 2 | 1.07x | 0.96x | 0.97x | 0.95x | 0.96x | 0.92x |
| tape200 | 3 | 0.96x | 0.95x | 0.99x | 0.97x | 0.89x | 0.96x |
| tape1k | 1 | 1.06x | 1.04x | 0.97x | 1.03x | 1.05x | 1.00x |
| tape1k | 2 | 1.10x | 0.97x | 0.96x | 1.02x | 0.97x | 0.92x |
| tape1k | 3 | 0.98x | 0.97x | 0.98x | 1.03x | 0.88x | 0.95x |
| tape10k | 1 | 1.20x | 1.12x | 1.03x | 1.22x | 1.10x | 1.06x |
| tape10k | 2 | 1.22x | 1.03x | 1.02x | 1.20x | 1.04x | 0.98x |
| tape10k | 3 | 1.19x | 1.05x | 1.01x | 1.20x | 0.97x | 1.07x |
| pk | 1 | 0.99x | 1.01x | 0.95x | 0.94x | 1.06x | 1.02x |
| pk | 2 | 1.07x | 0.93x | 0.96x | 0.85x | 0.93x | 0.91x |
| pk | 3 | 0.91x | 0.96x | 0.98x | 0.95x | 0.89x | 0.94x |
| open | 1 | 0.97x | 1.05x | 0.99x | 1.13x | 1.27x | 1.15x |
| open | 2 | 0.96x | 0.96x | 0.97x | 1.20x | 1.17x | 1.10x |
| open | 3 | 1.21x | 1.10x | 1.29x | 1.10x | 1.15x | 1.27x |
| close | 1 | 1.02x | 1.04x | 1.04x | 1.03x | 1.04x | 1.02x |
| close | 2 | 1.04x | 1.03x | 1.02x | 1.03x | 1.02x | 1.06x |
| close | 3 | 1.04x | 1.05x | 1.04x | 1.01x | 1.04x | 1.05x |

The `pk` control reads 0.85x to 1.07x, so ratios inside that are not findings. The `off` arm on `board` (1.17x to 1.20x) and `tape10k` (1.19x to 1.22x) is above it in all six days of both runs; the `on` and `declared` arms there are
0.97x to 1.12x, mostly inside it. `open` in run B (1.10x to 1.27x) is the device's sync moving between runs, section 9.

## 4. Percentiles

Run A, every phase (microseconds; the five percentiles are the driver's; PostgreSQL's day-1 phases follow):

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load[off] | 31,200 ops | 35.9 µs | 42.9 µs | 43.4 µs | 59.5 µs | 68.6 µs | 234.1 µs | 0 errors |
| load[on] | 31,200 ops | 36.9 µs | 43.5 µs | 44.6 µs | 65.4 µs | 81.7 µs | 145.8 µs | 0 errors |
| load[declared] | 31,200 ops | 37.5 µs | 42.9 µs | 43.6 µs | 62.3 µs | 74.3 µs | 221.5 µs | 0 errors |
| d1-open[off] | 240 ops | 975.0 µs | 1,155.1 µs | 1,210.4 µs | 1,538.3 µs | 2,431.7 µs | 4,653.1 µs | 0 errors |
| d1-open[on] | 240 ops | 966.3 µs | 1,066.6 µs | 1,164.4 µs | 1,505.2 µs | 1,624.0 µs | 2,507.4 µs | 0 errors |
| d1-open[declared] | 240 ops | 979.9 µs | 1,078.9 µs | 1,139.5 µs | 1,555.5 µs | 2,702.2 µs | 5,979.6 µs | 0 errors |
| d1-board[off] | 2,400 ops | 350.2 µs | 409.0 µs | 426.8 µs | 474.6 µs | 617.0 µs | 918.1 µs | 0 errors |
| d1-tape200[off] | 396 ops | 96.6 µs | 99.6 µs | 102.0 µs | 134.2 µs | 150.5 µs | 170.9 µs | 0 errors |
| d1-tape1k[off] | 396 ops | 118.2 µs | 120.5 µs | 123.2 µs | 156.4 µs | 177.9 µs | 196.2 µs | 0 errors |
| d1-tape10k[off] | 396 ops | 342.9 µs | 353.1 µs | 364.5 µs | 432.8 µs | 537.6 µs | 546.4 µs | 0 errors |
| d1-pk[off] | 240 ops | 60.6 µs | 63.4 µs | 67.1 µs | 105.7 µs | 124.5 µs | 262.1 µs | 0 errors |
| d1-board[on] | 2,400 ops | 100.2 µs | 167.0 µs | 182.4 µs | 440.2 µs | 758.6 µs | 930.8 µs | 0 errors |
| d1-tape200[on] | 396 ops | 96.3 µs | 98.8 µs | 102.4 µs | 136.5 µs | 151.9 µs | 195.2 µs | 0 errors |
| d1-tape1k[on] | 396 ops | 96.1 µs | 99.3 µs | 107.1 µs | 157.5 µs | 179.1 µs | 201.1 µs | 0 errors |
| d1-tape10k[on] | 396 ops | 96.2 µs | 99.7 µs | 109.4 µs | 417.3 µs | 772.7 µs | 798.8 µs | 0 errors |
| d1-pk[on] | 240 ops | 61.5 µs | 63.5 µs | 65.4 µs | 99.5 µs | 120.8 µs | 280.3 µs | 0 errors |
| d1-board[declared] | 2,400 ops | 97.4 µs | 163.8 µs | 175.7 µs | 779.4 µs | 827.6 µs | 1,065.9 µs | 0 errors |
| d1-tape200[declared] | 396 ops | 94.0 µs | 97.1 µs | 100.3 µs | 130.5 µs | 160.2 µs | 185.9 µs | 0 errors |
| d1-tape1k[declared] | 396 ops | 94.3 µs | 97.4 µs | 101.8 µs | 172.2 µs | 193.3 µs | 206.6 µs | 0 errors |
| d1-tape10k[declared] | 396 ops | 94.0 µs | 97.8 µs | 105.1 µs | 782.1 µs | 867.7 µs | 945.1 µs | 0 errors |
| d1-pk[declared] | 240 ops | 60.3 µs | 61.8 µs | 63.6 µs | 93.2 µs | 114.7 µs | 239.7 µs | 0 errors |
| d1-close[off] | 12 ops | 649.5 µs | 692.2 µs | 755.9 µs | 3,999.6 µs | 3,999.6 µs | 3,999.6 µs | 0 errors |
| d1-close[on] | 12 ops | 669.0 µs | 718.3 µs | 776.5 µs | 3,736.6 µs | 3,736.6 µs | 3,736.6 µs | 0 errors |
| d1-close[declared] | 12 ops | 675.7 µs | 715.9 µs | 797.7 µs | 3,811.8 µs | 3,811.8 µs | 3,811.8 µs | 0 errors |
| d2-open[off] | 240 ops | 985.0 µs | 1,112.8 µs | 1,179.5 µs | 1,551.3 µs | 3,245.4 µs | 6,726.1 µs | 0 errors |
| d2-open[on] | 240 ops | 988.5 µs | 1,113.4 µs | 1,185.0 µs | 1,493.1 µs | 1,831.8 µs | 12,582.5 µs | 0 errors |
| d2-open[declared] | 240 ops | 999.5 µs | 1,109.9 µs | 1,199.9 µs | 1,504.4 µs | 1,837.2 µs | 2,976.0 µs | 0 errors |
| d2-board[off] | 2,400 ops | 349.9 µs | 411.4 µs | 429.0 µs | 467.0 µs | 503.5 µs | 781.1 µs | 0 errors |
| d2-tape200[off] | 396 ops | 96.4 µs | 99.2 µs | 101.4 µs | 124.9 µs | 142.4 µs | 152.5 µs | 0 errors |
| d2-tape1k[off] | 396 ops | 118.3 µs | 120.8 µs | 123.1 µs | 146.2 µs | 158.2 µs | 172.0 µs | 0 errors |
| d2-tape10k[off] | 396 ops | 343.3 µs | 352.5 µs | 360.9 µs | 402.3 µs | 445.6 µs | 500.9 µs | 0 errors |
| d2-pk[off] | 240 ops | 60.8 µs | 62.6 µs | 64.7 µs | 95.7 µs | 119.5 µs | 130.9 µs | 0 errors |
| d2-board[on] | 2,400 ops | 100.7 µs | 168.3 µs | 186.3 µs | 444.9 µs | 774.0 µs | 1,150.6 µs | 0 errors |
| d2-tape200[on] | 396 ops | 96.4 µs | 99.1 µs | 102.1 µs | 135.3 µs | 152.3 µs | 164.2 µs | 0 errors |
| d2-tape1k[on] | 396 ops | 96.3 µs | 98.7 µs | 101.4 µs | 142.8 µs | 172.6 µs | 185.9 µs | 0 errors |
| d2-tape10k[on] | 396 ops | 96.5 µs | 99.1 µs | 104.5 µs | 412.5 µs | 761.6 µs | 834.1 µs | 0 errors |
| d2-pk[on] | 240 ops | 61.4 µs | 63.5 µs | 64.9 µs | 97.6 µs | 115.8 µs | 127.2 µs | 0 errors |
| d2-board[declared] | 2,400 ops | 98.2 µs | 164.3 µs | 179.2 µs | 784.8 µs | 857.6 µs | 981.5 µs | 0 errors |
| d2-tape200[declared] | 396 ops | 94.7 µs | 97.1 µs | 100.0 µs | 138.4 µs | 155.5 µs | 164.8 µs | 0 errors |
| d2-tape1k[declared] | 396 ops | 94.5 µs | 97.3 µs | 102.2 µs | 170.9 µs | 209.2 µs | 238.8 µs | 0 errors |
| d2-tape10k[declared] | 396 ops | 94.5 µs | 97.9 µs | 102.3 µs | 781.5 µs | 868.0 µs | 1,018.1 µs | 0 errors |
| d2-pk[declared] | 240 ops | 60.2 µs | 62.0 µs | 64.1 µs | 100.8 µs | 116.2 µs | 154.7 µs | 0 errors |
| d2-close[off] | 12 ops | 662.0 µs | 689.7 µs | 716.0 µs | 3,625.3 µs | 3,625.3 µs | 3,625.3 µs | 0 errors |
| d2-close[on] | 12 ops | 673.8 µs | 704.8 µs | 787.9 µs | 3,765.4 µs | 3,765.4 µs | 3,765.4 µs | 0 errors |
| d2-close[declared] | 12 ops | 691.0 µs | 726.8 µs | 786.5 µs | 6,102.6 µs | 6,102.6 µs | 6,102.6 µs | 0 errors |
| d3-open[off] | 240 ops | 963.3 µs | 1,106.1 µs | 1,158.0 µs | 1,515.1 µs | 1,761.5 µs | 1,819.8 µs | 0 errors |
| d3-open[on] | 240 ops | 975.5 µs | 1,166.0 µs | 1,234.8 µs | 1,499.8 µs | 2,481.6 µs | 8,122.3 µs | 0 errors |
| d3-open[declared] | 240 ops | 956.7 µs | 1,062.5 µs | 1,162.1 µs | 1,505.2 µs | 2,350.2 µs | 5,748.6 µs | 0 errors |
| d3-board[off] | 2,400 ops | 359.1 µs | 423.3 µs | 440.1 µs | 486.3 µs | 555.4 µs | 800.7 µs | 0 errors |
| d3-tape200[off] | 396 ops | 96.8 µs | 99.9 µs | 106.2 µs | 142.8 µs | 161.4 µs | 168.4 µs | 0 errors |
| d3-tape1k[off] | 396 ops | 118.3 µs | 121.9 µs | 131.4 µs | 171.7 µs | 179.7 µs | 215.8 µs | 0 errors |
| d3-tape10k[off] | 396 ops | 344.3 µs | 353.2 µs | 367.2 µs | 406.1 µs | 459.6 µs | 475.1 µs | 0 errors |
| d3-pk[off] | 240 ops | 60.4 µs | 63.1 µs | 67.0 µs | 101.9 µs | 128.1 µs | 162.5 µs | 0 errors |
| d3-board[on] | 2,400 ops | 98.1 µs | 169.3 µs | 178.1 µs | 395.9 µs | 795.7 µs | 1,672.2 µs | 0 errors |
| d3-tape200[on] | 396 ops | 96.6 µs | 99.1 µs | 102.2 µs | 132.8 µs | 150.4 µs | 166.7 µs | 0 errors |
| d3-tape1k[on] | 396 ops | 95.7 µs | 99.1 µs | 101.9 µs | 143.9 µs | 184.4 µs | 205.4 µs | 0 errors |
| d3-tape10k[on] | 396 ops | 96.0 µs | 99.5 µs | 104.6 µs | 408.6 µs | 738.4 µs | 762.8 µs | 0 errors |
| d3-pk[on] | 240 ops | 61.6 µs | 63.2 µs | 65.0 µs | 97.1 µs | 117.5 µs | 123.3 µs | 0 errors |
| d3-board[declared] | 2,400 ops | 97.2 µs | 167.5 µs | 178.4 µs | 793.7 µs | 855.7 µs | 1,024.2 µs | 0 errors |
| d3-tape200[declared] | 396 ops | 94.2 µs | 97.2 µs | 100.7 µs | 137.4 µs | 153.1 µs | 168.9 µs | 0 errors |
| d3-tape1k[declared] | 396 ops | 94.8 µs | 97.8 µs | 102.3 µs | 138.7 µs | 194.1 µs | 229.0 µs | 0 errors |
| d3-tape10k[declared] | 396 ops | 94.2 µs | 97.9 µs | 106.8 µs | 785.4 µs | 1,069.9 µs | 1,168.2 µs | 0 errors |
| d3-pk[declared] | 240 ops | 60.4 µs | 62.2 µs | 64.9 µs | 100.1 µs | 120.9 µs | 123.4 µs | 0 errors |
| d3-close[off] | 12 ops | 663.1 µs | 681.6 µs | 701.0 µs | 3,620.1 µs | 3,620.1 µs | 3,620.1 µs | 0 errors |
| d3-close[on] | 12 ops | 694.9 µs | 712.6 µs | 799.8 µs | 3,704.4 µs | 3,704.4 µs | 3,704.4 µs | 0 errors |
| d3-close[declared] | 12 ops | 672.6 µs | 707.2 µs | 815.1 µs | 3,816.3 µs | 3,816.3 µs | 3,816.3 µs | 0 errors |

PostgreSQL 18.6, day 1, run 1 of 3:

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load[pg] | 31,200 ops | 47.1 µs | 48.9 µs | 50.0 µs | 75.1 µs | 93.5 µs | 1,041.0 µs | 0 errors |
| d1-open[pg] | 240 ops | 990.2 µs | 1,077.1 µs | 1,120.0 µs | 1,460.2 µs | 2,191.3 µs | 5,628.7 µs | 0 errors |
| d1-board[pg] | 2,400 ops | 465.7 µs | 492.3 µs | 501.2 µs | 525.5 µs | 547.2 µs | 852.0 µs | 0 errors |
| d1-tape200[pg] | 396 ops | 73.7 µs | 75.1 µs | 75.6 µs | 91.8 µs | 105.5 µs | 115.1 µs | 0 errors |
| d1-tape1k[pg] | 396 ops | 105.9 µs | 107.6 µs | 108.5 µs | 136.8 µs | 194.3 µs | 392.4 µs | 0 errors |
| d1-tape10k[pg] | 396 ops | 456.3 µs | 464.2 µs | 472.7 µs | 504.5 µs | 542.1 µs | 607.3 µs | 0 errors |
| d1-pk[pg] | 240 ops | 59.3 µs | 60.7 µs | 61.6 µs | 88.1 µs | 110.7 µs | 187.3 µs | 0 errors |
| d1-close[pg] | 12 ops | 355.1 µs | 374.8 µs | 442.7 µs | 2,259.9 µs | 2,259.9 µs | 2,259.9 µs | 0 errors |

## 5. Where the time goes

A probe is one round trip on one connection, so the decomposition is fixed cost plus rows walked; there is no commit or lock in the read phases and one durable commit per `open` statement.

| Component | Now | v3.1.0 (cross-version) | PostgreSQL | How derived |
|---|---|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | **73.4 µs** (68.2 to 79.3) | 70.1 µs | 69.4 µs | `pk` mean across arms, days and runs |
| Rows walked by an unserved equality | **26.5 ns per row** plus about 103 µs | 34.6 ns | 40.5 ns | `off` tape p50 at 200 and 10,000 rows, day 1: (p50 at 10,000 - p50 at 200) / 9,800 |
| A Cabin-served equality | **about 102 µs** at any size once built (day 2) | about 98 to 100 µs | - | `on` and `declared` tape p50 on day 2; flat in n |
| Write statement with its commit (`open`) | **1,188 µs** p50 per autocommit insert | 1,021 to 1,280 µs | 1,161 µs | `open` p50 over arms and days: the durable commit is nearly all of it |
| Lock or conflict wait | none: one connection | none | none | - |

The board's latency tail by arm (run A day by day; run B and v3.1.0 for the p95), microseconds:

| day | arm | run A mean | run A p50 | run A p95 | run A p99 | run B p95 | v3.1.0 run A p95 |
|---|---|---|---|---|---|---|---|
| 1 | off | 426.6 µs | 426.8 µs | 474.6 µs | 617.0 µs | 476.4 µs | 556.1 µs |
| 1 | on | 238.7 µs | 182.4 µs | 440.2 µs | 758.6 µs | 431.1 µs | 509.6 µs |
| 1 | declared | 276.2 µs | 175.7 µs | 779.4 µs | 827.6 µs | 773.1 µs | 840.2 µs |
| 2 | off | 425.8 µs | 429.0 µs | 467.0 µs | 503.5 µs | 478.8 µs | 560.0 µs |
| 2 | on | 242.2 µs | 186.3 µs | 444.9 µs | 774.0 µs | 443.3 µs | 519.3 µs |
| 2 | declared | 271.4 µs | 179.2 µs | 784.8 µs | 857.6 µs | 787.2 µs | 847.9 µs |
| 3 | off | 438.7 µs | 440.1 µs | 486.3 µs | 555.4 µs | 481.1 µs | 563.6 µs |
| 3 | on | 210.9 µs | 178.1 µs | 395.9 µs | 795.7 µs | 460.9 µs | 474.1 µs |
| 3 | declared | 238.4 µs | 178.4 µs | 793.7 µs | 855.7 µs | 789.1 µs | 852.8 µs |

**The declared arm has the heavier tail**, as at v3.1.0: its board p95 is 779 to 794 µs against the `on` arm's 396 to 445 µs and the walk's 467 to 486 µs, in every day and both runs. A probe the declared Cabin cannot answer costs about 1.6x a plain
walk and the controller's does not. This is why the `on` arm's mean (239 µs, day 1 run A) is lower than the `declared` arm's (276 µs) although the `declared` p50 is lower (176 against 182 µs). This run does not say why the declared arm's miss path is costlier.

Server CPU seconds by arm (the driver's `/proc` reading), now and at v3.1.0:

| arm | board A | board B | board v3.1.0 A / B | tape A | tape B | tape v3.1.0 A / B | open A | close A |
|---|---|---|---|---|---|---|---|---|
| off | 2.22 s | 2.24 s | 2.80 s / 2.81 s | 0.43 s | 0.47 s | 0.52 s / 0.58 s | 0.07 s | 0.04 s |
| on | 0.78 s | 0.92 s | 0.84 s / 0.96 s | 0.23 s | 0.28 s | 0.25 s / 0.23 s | 0.06 s | 0.03 s |
| declared | 1.03 s | 1.03 s | 1.11 s / 1.08 s | 0.27 s | 0.24 s | 0.19 s / 0.27 s | 0.04 s | 0.02 s |

## 6. The controller's lifecycle

Snapshots of `SHOW CABIN_OPTIMIZER` on the `on` arm at chosen points:

run A:

| t (s) | day | tag | managed | creates | extends | drops | entries |
|---|---|---|---|---|---|---|---|
| 13.6 s | 1 | trading-block-3 | 4 Cabins | 4 creates | 28 extends | 0 drops | board_a_a:ACTIVE/3p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/1p; tape_10k_a:ACTIVE/1p |
| 36.1 s | 1 | trading-block-9 | 4 Cabins | 4 creates | 104 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 47.4 s | 1 | trading-block-12 | 4 Cabins | 4 creates | 104 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 54.0 s | 1 | overnight | 4 Cabins | 4 creates | 104 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 104.6 s | 2 | trading-block-3 | 5 Cabins | 5 creates | 134 extends | 0 drops | board_a_a:ACTIVE/9p; board_b_a:ACTIVE/3p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 127.1 s | 2 | trading-block-9 | 5 Cabins | 5 creates | 142 extends | 0 drops | board_a_a:DECAYING/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 138.4 s | 2 | trading-block-12 | 5 Cabins | 5 creates | 142 extends | 0 drops | board_a_a:DECAYING/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 145.0 s | 2 | overnight | 5 Cabins | 5 creates | 142 extends | 0 drops | board_a_a:DECAYING/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 195.6 s | 3 | trading-block-3 | 5 Cabins | 5 creates | 164 extends | 0 drops | board_a_a:ACTIVE/18p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/5p |
| 218.1 s | 3 | trading-block-9 | 5 Cabins | 5 creates | 172 extends | 0 drops | board_a_a:ACTIVE/18p; board_b_a:DECAYING/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 229.3 s | 3 | trading-block-12 | 5 Cabins | 5 creates | 172 extends | 0 drops | board_a_a:ACTIVE/18p; board_b_a:DECAYING/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 235.9 s | 3 | overnight | 5 Cabins | 5 creates | 172 extends | 0 drops | board_a_a:ACTIVE/18p; board_b_a:DECAYING/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |

final header: `managed=5 pages_committed=32 page_budget=1024 ticks=546 creates=5 extends=172 heals=0 drops=0 deferred=0 failures=0`
verify per day: PASS, PASS, PASS

run B:

| t (s) | day | tag | managed | creates | extends | drops | entries |
|---|---|---|---|---|---|---|---|
| 13.7 s | 1 | trading-block-3 | 4 Cabins | 4 creates | 31 extends | 0 drops | board_a_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/1p; tape_10k_a:ACTIVE/1p |
| 36.2 s | 1 | trading-block-9 | 4 Cabins | 4 creates | 104 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 47.5 s | 1 | trading-block-12 | 4 Cabins | 4 creates | 104 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 54.1 s | 1 | overnight | 4 Cabins | 4 creates | 104 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 104.8 s | 2 | trading-block-3 | 5 Cabins | 5 creates | 133 extends | 0 drops | board_a_a:ACTIVE/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 127.3 s | 2 | trading-block-9 | 5 Cabins | 5 creates | 140 extends | 0 drops | board_a_a:DECAYING/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 138.5 s | 2 | trading-block-12 | 4 Cabins | 5 creates | 140 extends | 1 drops | board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 145.1 s | 2 | overnight | 4 Cabins | 5 creates | 140 extends | 1 drops | board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 195.7 s | 3 | trading-block-3 | 5 Cabins | 6 creates | 164 extends | 1 drops | board_a_a:ACTIVE/5p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 218.2 s | 3 | trading-block-9 | 5 Cabins | 6 creates | 186 extends | 1 drops | board_a_a:ACTIVE/14p; board_b_a:DECAYING/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 229.5 s | 3 | trading-block-12 | 4 Cabins | 6 creates | 193 extends | 2 drops | board_a_a:ACTIVE/14p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 236.1 s | 3 | overnight | 4 Cabins | 6 creates | 193 extends | 2 drops | board_a_a:ACTIVE/14p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |

final header: `managed=4 pages_committed=24 page_budget=1024 ticks=546 creates=6 extends=193 heals=0 drops=2 deferred=0 failures=0`
verify per day: PASS, PASS, PASS

Run A, defaults: four Cabins are created in the first 13.6 s of day 1 (`board_a`, `tape_200`, `tape_1k`, `tape_10k`), grown to 104 extends by the end of the day, and a fifth (`board_b`) on day 2's first block. `board_a` goes **DECAYING** on day 2 and **returns to ACTIVE** on
day 3 with its pages doubled (9 to 18) when its hot set comes back; then `board_b` goes DECAYING. At the end: `creates=5 extends=172 heals=0 drops=0 deferred=0 failures=0`. **No DROP occurs in a 3-day run at the defaults**: the DECAYING dwell before a DROP is
`cabin_optimizer_cooldown_half_lives`, default 128 (`docs/spec/physical-optimizer.md` II.6), which at `decay_half_life = 5` is 640 s; the driver's whole run is 278 s.

Run B, cooldown 2 half-lives: the same creates on day 1, `board_a` DECAYING at 127.3 s and **DROPPED** by 138.5 s (`drops=1`); its Cabin is gone while `board_b` is hot; on day 3 `board_a` is **re-nominated from scratch** (`creates=6`, 5 pages at 195.7 s, 14 by the end)
and `board_b` is dropped at day 3's last block (`drops=2`). Final: `creates=6 extends=193 heals=0 drops=2 failures=0`. The cost of re-nomination shows in the served rate of the day it happens: `d3-board[on]` runs 4,742 qps in run A (the Cabin survived the night) and 4,002 qps in
run B (rebuilt), -15.6 %, against a run A / run B difference of -0.9 % and +0.1 % on days 1 and 2. Single draw per run. The lifecycle is, snapshot for snapshot, the one v3.1.0 records
(4 / 5 / 5 Cabins, the same DECAYING and ACTIVE transitions, 2 DROPs in run B), with extends within 3 of it (172 against 170, 193 against 196).

## 7. Correctness

The driver ran its 18-statement comparison across the three arms each day: **PASS on all three days in both runs** (`verify` in the JSON), so a Cabin, declared or controller-built, changed no reply. `client_errors` 0 in both runs.

## 8. Against PostgreSQL 18.6 (fresh, same window)

PostgreSQL is the floor. The PG twin runs **day 1 only, unpaced, one server, no index**, by its own design: at defaults a PostgreSQL probe seq-scans on day 1 as on day 300 (the `explain` blocks in its JSON say `Seq Scan`). Three runs, KDS side: run A and run B day 1, derived qps,
the median of the two, ratio above 1.00x = KDS faster:

PG runs: 3

| group | PG18 day 1 (no index): p50 / median qps (spread) | KDS off | KDS on | KDS declared | off / PG | on / PG | declared / PG |
|---|---|---|---|---|---|---|---|
| open | 1,160.8 µs / 831 qps (10.9 %) | 777 qps | 833 qps | 814 qps | 0.93x | 1.00x | 0.98x |
| board | 504.4 µs / 1,976 qps (2.7 %) | 2,343 qps | 4,170 qps | 3,625 qps | 1.19x | 2.11x | 1.84x |
| tape200 | 75.9 µs / 12,484 qps (6.7 %) | 9,110 qps | 9,204 qps | 9,360 qps | 0.73x | 0.74x | 0.75x |
| tape1k | 108.8 µs / 8,726 qps (2.5 %) | 7,655 qps | 8,799 qps | 8,870 qps | 0.88x | 1.01x | 1.02x |
| tape10k | 472.7 µs / 2,100 qps (1.4 %) | 2,704 qps | 5,339 qps | 4,566 qps | 1.29x | 2.54x | 2.17x |
| pk | 61.9 µs / 14,409 qps (4.0 %) | 13,245 qps | 13,811 qps | 14,444 qps | 0.92x | 0.96x | 1.00x |
| close | 442.7 µs / 1,185 qps (0.2 %) | 678 qps | 683 qps | 673 qps | 0.57x | 0.58x | 0.57x |

The comparison with a Cabin is not a like-for-like with PostgreSQL's *indexed* case, which this twin does not run (no `symbol` index on PostgreSQL); that is a gap of this scenario, so "KDS on / PG" says what a Cabin buys over a PostgreSQL seq scan, not over a PostgreSQL index.
`tape200` is 0.73x to 0.75x on every arm: at 200 rows the statement is a round trip and PostgreSQL's is 26 µs cheaper (69 against 73 µs for the `pk` control the same gap, 0.92x). `close` (three full-relation folds) is 0.57x on every arm, the whole-relation fold cost
that scenario 1 measures as 0.65x to 0.69x on three of its four `agg-*` folds.

## 9. Noise floor, contamination and host

The floor is the `pk` control: on-over-off 0.97x to 1.07x, per-arm absolute 12,610 to 14,663 qps across arms, days and runs; PostgreSQL's three runs spread 0.2 % to 10.9 % (`open` 10.9 %, the durable group). The two runs of KDS give the repeat:
`open` p50 is 1,188 µs overall and run B's differs from run A's by up to 9 % (the device's sync); every probe group and arm differs between runs by up to 10.3 % (the largest is a `pk` control cell; day 3 `on` is excluded, where run B re-nominated its Cabin) (derived qps, e.g. `off` on day 1: 2,344 qps and 2,341 qps).

**Contamination.** No cell was discarded. Per-cell host stamps (run A and B hold three servers each; the precheck of the `-off` server stands for the cell, the monitor covered the whole window):

| cell | start (UTC) | end (UTC) | load average before (1 / 5 / 15 min) | gate wait | build or test process | foreign samples >= 10 % CPU | exit |
|---|---|---|---|---|---|---|---|
| `s4` | 04:08:50 | 04:13:37 | 1.41 / 1.02 / 1.21 | 0 s | none | 1 of 132 samples | 0 |
| `cd2-s4` | 04:13:44 | 04:18:30 | 0.16 / 0.54 / 0.95 | 0 s | none | 0 of 131 samples | 0 |
| `pg-s4` | 04:24:57 | 04:25:01 | 0.81 / 0.69 / 0.80 | 0 s | none | 0 of 4 samples | 0 |
| `r2-pg-s4` | 04:28:37 | 04:28:41 | 0.94 / 0.79 / 0.82 | 0 s | none | 0 of 4 samples | 0 |
| `r3-pg-s4` | 04:32:13 | 04:32:17 | 1.20 / 0.98 / 0.89 | 0 s | none | 0 of 4 samples | 0 |

## 10. What the run says about the engine

1. **The unserved walk got cheaper and nothing else about the Cabin story changed.** `off` is 1.18x and 1.20x v3.1.0 on the 10,000-row shapes, and the per-row cost of an unserved equality fell from 34.6 to 26.5 ns, against
   PostgreSQL's 40.5 ns. The served arms did not move (a Cabin-served probe is a round trip, about 102 µs), so the Cabin's multiple over a walk moved from 1.99x/2.77x (v3.1.0, medians over the six days) to 1.78x/2.31x on `board`/`tape10k` where it moved at all because the *denominator* improved.
   Scenario 3's integer-column equality walk does not show the same gain (`loans-by-user` 1.00x at 10,000 loans); the two differ in the filtered column's type (`varchar` symbol here, `int64` there) among other things, and this run does not separate them.
2. **Below about 1,000 rows a Cabin is no tool** (`tape200` 0.98x to 1.08x; `tape1k` near the floor); at 10,000 rows it doubles the probe. The controller found the same four Cabins in the same 13.6 s that it did at v3.1.0, and created none for the pk control.
3. **The declared arm's tail is the open question this scenario keeps asking**: p95 779 to 794 µs against a 467 to 486 µs walk. It repeated across both runs, three days and two versions; it is a property of the declared Cabin's miss path, and nothing in this run explains it.
4. **DROP exists only at a 2-half-life cooldown.** The default dwell (128 half-lives) never fires inside a three-day run, so a 3-day business cycle at the defaults will not reclaim a Cabin whose hot set has moved; that is the controller's documented default, now with a second data point.

## 11. Archive

`bench/v3.3.0/archive/scenario4-v3.3.0-33-g9682b725/`: both runs' JSON (including the full controller evidence and the 18-statement comparison), driver stdout, the three servers' configs per run, PostgreSQL's three JSON. What the four scenarios share is in `bench/v3.3.0/archive/run-v3.3.0-33-g9682b725/`.
