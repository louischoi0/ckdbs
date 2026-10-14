# Scenario 4 (cabin optimizer over business days) at `v3.1.0-30-gcd7c7c4e`

**One line.** The autonomous Cabin controller still delivers what a hand-declared
Cabin does on a hot non-pk equality, and the walk it replaces got twice as cheap. On a 10,000-row board
(run A, days 1 to 3) the `off` arm that walks serves 1,936 to 1,976 qps (comparator, cross-version: 921 to 960), the `on` arm
3,858 to 4,640 qps (2,374 to 3,283) and the `declared` arm 3,506 to 4,189 qps (2,990 to 3,827), with byte-identical
answers on all three days in both runs. A Cabin's multiple over the walk fell with the walk's cost: `on` over `off` on the board is
1.95x to 2.40x (2.49x to 3.56x at the comparator) and on the 10,000-row tape 2.15x to 2.84x
(2.67x to 3.93x). **At the default `cabin_optimizer_cooldown_half_lives = 128` the run still never DROPs a Cabin
(`drops=0`)**; run B at cooldown 2 drops and re-nominates `board_a`, which costs day 3's board throughput
16 % (the comparator: 25 %).

Driver: `tools/scenario4_cabinopt_days.py`, unmodified, three servers (arms `off`, `on`, `declared`) on fresh data
files, blocks interleaved so only one arm is active at a time; the 2026-09-30 rebaseline's arguments and two
runs. Comparator (cross-version): `bench/v3.0.0/results-scenario4-cabinopt-days-v2.7.0-531-g9a0525d.md`, held at
`36449d28`. PostgreSQL 18.6 figures are **reused, not re-measured**, from
`results-scenario4-cabinopt-days-pg18-v2.7.0-545-gf2f1ee7.md` (held at `36449d28`, measured 2026-09-30,
**14 days before this run**). The time budget was lifted by the operator for this run.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-14; run A 00:34:29 to 00:39:15 UTC (277.9 s), run B 00:39:23 to 00:44:10 UTC (278.7 s) |
| Worktree / branch | `bench-v3.1.0-scenarios` / `worktree-bench-v3.1.0-scenarios` |
| HEAD | `cd7c7c4e`, `git describe --tags` = `v3.1.0-30-gcd7c7c4e` ("Merge sysview-strict-compare: a catalog view's WHERE takes the relation's literal rule, compared under each column's declared type"), committed 2026-10-13 12:24:08 UTC |
| Tree cleanliness | Clean at the start (`git status` empty); nothing under `src/`, `include/`, `tools/` or `CMakeLists.txt` was edited, and the drivers are the unmodified `tools/` scripts (the one exception is stated where it applies). The results files under `bench/v3.1.0/` are uncommitted additions |
| Binary provenance | `build-release/kds_server` built from this tree at HEAD (`cmake -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, `-O3 -DNDEBUG`), finished 2026-10-13 23:26:41 UTC, 14 hours after the HEAD commit, so the binary is not older than the engine. Copied to `/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/kds_server` before the first cell; **copy sha256 `4752f56285aba5e7b1adbd881facc8a7a7dadfab6269b468d30f0b29ce53befa`** (equal to the source binary's), source binary mtime 2026-10-13 23:26:41.5 UTC. Every server started from the copy; no build ran during any reported cell |
| Device | data files under `/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/data/`, `/dev/root`, `ext4` (`df -T`); never tmpfs. A fresh data file per cell (per server, in scenario 4), deleted after |
| Build type | Release (`CMakeCache.txt`: `CMAKE_BUILD_TYPE=Release`) |
| Host | 8 logical CPUs, AMD EPYC 9V74 (1 socket x 4 cores x 2 threads), Linux 7.0.0-1014-azure, a shared development VM: other sessions build and run `ctest` on it |
| Time budget | **Lifted by the operator for this run** (order of 2026-10-13: "this work is an exception to the time-limit rule"); the rebaseline's arguments and repeat counts ran in full, no cell was cut |
| Prior run of this shape | the 2026-09-30 rebaseline at `9a0525d` (`v2.7.0-531-g9a0525d`, "AY closed"), file held at `36449d28` (`git show 36449d28:bench/v3.0.0/results-scenario*-v2.7.0-531-g9a0525d.md`). **495 commits and 176 first-parent merges apart** (`git rev-list --count 9a0525d..HEAD`), with 256 files changed under `src/`, `include/` and `CMakeLists.txt`, so **no delta below is attributable to one change** |
| Server config (all three servers) | `cores = 1`, `durability = group`, `log_level = warn`, **`buffer_pool_frames = 65536`** (required since BE; the comparator's config predates the key), `decay_half_life = 5` (seconds; the driver docstring's 120x compression of the 600 s default), `cabin_optimizer_snapshot_interval_ms = 500`; **run B only** `cabin_optimizer_cooldown_half_lives = 2`. Ports 15651 to 15653, never 15432. The `on` arm runs `SET CABIN_OPTIMIZER ON` at day 1 open; the `declared` arm declares `CREATE CABIN` on every board and tape symbol column up front |
| Driver arguments | defaults: `--days 3 --blocks 12 --session-seconds 45 --overnight-seconds 45 --open-inserts 240 --board-probes 2400 --tape-probes 396 --pk-ops 240 --close-rounds 3 --seed 20260810 --suffix a`, plus `--port-off/-on/-declared` and the three server pids |
| Quiet host | before each cell a gate waited for no `cc1plus` / `cmake --build` / `ctest` / `kds_tests` process and at most 8 % busy CPU over 3 s (`/proc/loadavg` and the process list are in each cell's precheck, tables below). **A 2-second monitor ran through the whole run** (`monitor.log`) and a cell with a build/test process or a foreign process at 50 % CPU or more inside its window was **discarded and re-run** (up to 8 attempts, 8 happened, `contaminated/` in the archive). Foreign `claude` processes at 10 to 30 % of one CPU were present in many windows and are counted as minor, not excluded. The monitor's limit: it samples every 2 to 3 s, so a shorter burst is not seen |
| PostgreSQL | **reused, not re-measured**: PostgreSQL 18.6 at its defaults, no index and no Cabin, day 1 only, `results-scenario4-cabinopt-days-pg18-v2.7.0-545-gf2f1ee7.md` (held at `36449d28`), measured 2026-09-30 at `f2f1ee7`. Section 8 |

## 2. What was run

Two complete runs of the driver, three days of three arms each: **run A** at the server defaults with the driver's
compression, **run B** identical but for `cabin_optimizer_cooldown_half_lives = 2` (run A cannot exercise DROP:
section 7). Per day: an open (240 inserts into the day's board), a paced 45 s trading session (2,400 hot-board
probes, 396 probes on each of three tape sizes, 240 pk controls, per arm, interleaved in 12 blocks), a close
(`COUNT` / `SUM` / `GROUP BY` full scans, three rounds) and a 45 s idle night. The equal work per arm is fixed by the
driver's counts, not by time. The driver verifies an identical 18-statement set across the three arms byte for byte
each day. Command (`run_all4.sh`, `lib.sh` in the archive):

    python3 tools/scenario4_cabinopt_days.py --port-off 15651 --port-on 15652 --port-declared 15653 \
        --pid-off <p> --pid-on <p> --pid-declared <p> --suffix a --json <run>.json

**Row-set axis (rule 9).** The tape relations are 200, 1,000 and 10,000 rows (`tape_200`, `tape_1k`, `tape_10k`);
the boards are 10,000 rows each (`board_a`, `board_b`). Every phase is a row of the matrix and the size is a
row label. The measured statements are serial on one connection, so qps is derived as 1,000,000 / mean µs. The
driver paces its sessions by wall clock (45 s per session), so its own busy-time TPS (section 3) is a derived
figure as well.

**The hot set rotates**: day 1 hot on `board_a`, day 2 on `board_b`, day 3 on `board_a` with a different symbol set;
the tapes are hot every day with disjoint values. No build or test process was live in any reported window (section 9).

## 3. Throughput and latency by arm

p50 and derived qps per group, day and arm, run A / run B. The last column is the ratio of mean latencies, off
over on:

| group | day | off A | off B | on A | on B | declared A | declared B | on over off, mean (A / B) |
|---|---|---|---|---|---|---|---|---|
| board | 1 | 509.0 µs / 1,976 qps | 508.1 µs / 1,977 qps | 190.3 µs / 3,858 qps | 189.7 µs / 3,854 qps | 173.9 µs / 3,506 qps | 178.3 µs / 3,452 qps | 1.95x / 1.95x |
| board | 2 | 510.4 µs / 1,958 qps | 506.2 µs / 1,986 qps | 175.8 µs / 3,929 qps | 176.8 µs / 3,908 qps | 176.3 µs / 3,578 qps | 172.2 µs / 3,625 qps | 2.01x / 1.97x |
| board | 3 | 518.5 µs / 1,936 qps | 525.3 µs / 1,920 qps | 173.2 µs / 4,640 qps | 176.1 µs / 3,905 qps | 173.1 µs / 4,189 qps | 171.1 µs / 4,259 qps | 2.40x / 2.03x |
| tape200 | 1 | 103.2 µs / 9,091 qps | 102.7 µs / 9,372 qps | 103.8 µs / 8,961 qps | 103.9 µs / 9,074 qps | 98.2 µs / 9,775 qps | 98.5 µs / 9,515 qps | 0.99x / 0.97x |
| tape200 | 2 | 103.8 µs / 8,905 qps | 102.3 µs / 9,355 qps | 98.6 µs / 9,662 qps | 98.9 µs / 9,497 qps | 98.5 µs / 9,597 qps | 97.4 µs / 9,843 qps | 1.09x / 1.02x |
| tape200 | 3 | 103.2 µs / 9,174 qps | 104.0 µs / 8,993 qps | 97.9 µs / 9,747 qps | 98.1 µs / 9,814 qps | 99.2 µs / 9,302 qps | 97.6 µs / 9,843 qps | 1.06x / 1.09x |
| tape1k | 1 | 130.9 µs / 7,294 qps | 129.7 µs / 7,402 qps | 124.7 µs / 8,389 qps | 112.7 µs / 8,467 qps | 97.8 µs / 9,066 qps | 99.0 µs / 8,921 qps | 1.15x / 1.14x |
| tape1k | 2 | 131.7 µs / 7,158 qps | 130.4 µs / 7,326 qps | 98.1 µs / 9,390 qps | 99.1 µs / 9,183 qps | 98.2 µs / 9,294 qps | 96.9 µs / 9,533 qps | 1.31x / 1.25x |
| tape1k | 3 | 130.6 µs / 7,369 qps | 132.4 µs / 7,231 qps | 99.0 µs / 9,407 qps | 98.0 µs / 9,643 qps | 99.1 µs / 9,276 qps | 97.2 µs / 9,833 qps | 1.28x / 1.33x |
| tape10k | 1 | 441.5 µs / 2,238 qps | 442.2 µs / 2,237 qps | 125.3 µs / 4,808 qps | 114.4 µs / 4,826 qps | 102.3 µs / 4,369 qps | 104.1 µs / 4,354 qps | 2.15x / 2.16x |
| tape10k | 2 | 439.4 µs / 2,238 qps | 440.6 µs / 2,255 qps | 100.0 µs / 6,353 qps | 102.9 µs / 6,196 qps | 101.1 µs / 5,247 qps | 98.9 µs / 5,339 qps | 2.84x / 2.75x |
| tape10k | 3 | 439.3 µs / 2,257 qps | 447.4 µs / 2,228 qps | 101.7 µs / 6,192 qps | 101.0 µs / 6,207 qps | 104.5 µs / 4,817 qps | 101.4 µs / 4,757 qps | 2.74x / 2.79x |
| pk | 1 | 65.8 µs / 13,405 qps | 63.0 µs / 14,065 qps | 65.6 µs / 13,333 qps | 65.3 µs / 13,387 qps | 61.9 µs / 14,948 qps | 62.6 µs / 14,430 qps | 0.99x / 0.95x |
| pk | 2 | 66.5 µs / 13,175 qps | 62.6 µs / 14,903 qps | 62.2 µs / 15,038 qps | 63.3 µs / 14,245 qps | 63.1 µs / 14,430 qps | 62.0 µs / 15,175 qps | 1.14x / 0.96x |
| pk | 3 | 62.3 µs / 14,620 qps | 63.6 µs / 13,870 qps | 62.4 µs / 14,663 qps | 63.7 µs / 14,430 qps | 62.9 µs / 14,045 qps | 62.5 µs / 14,970 qps | 1.00x / 1.04x |
| open | 1 | 1,021.0 µs / 815 qps | 1,338.1 µs / 670 qps | 1,029.4 µs / 798 qps | 1,348.5 µs / 653 qps | 1,024.9 µs / 839 qps | 1,279.4 µs / 697 qps | 0.98x / 0.97x |
| open | 2 | 1,035.8 µs / 818 qps | 1,280.8 µs / 632 qps | 1,043.1 µs / 830 qps | 1,301.5 µs / 674 qps | 1,042.0 µs / 842 qps | 1,245.6 µs / 706 qps | 1.02x / 1.07x |
| open | 3 | 1,280.7 µs / 691 qps | 1,323.0 µs / 728 qps | 1,286.8 µs / 710 qps | 1,244.5 µs / 729 qps | 1,343.3 µs / 646 qps | 1,332.9 µs / 615 qps | 1.03x / 1.00x |
| close | 1 | 758.9 µs / 661 qps | 742.2 µs / 664 qps | 830.3 µs / 658 qps | 860.9 µs / 654 qps | 834.9 µs / 653 qps | 839.3 µs / 652 qps | 1.00x / 0.98x |
| close | 2 | 828.8 µs / 663 qps | 756.8 µs / 668 qps | 844.8 µs / 664 qps | 803.2 µs / 668 qps | 859.6 µs / 587 qps | 870.7 µs / 574 qps | 1.00x / 1.00x |
| close | 3 | 740.2 µs / 670 qps | 760.1 µs / 668 qps | 864.1 µs / 646 qps | 828.9 µs / 649 qps | 850.7 µs / 647 qps | 835.0 µs / 643 qps | 0.96x / 0.97x |

Driver's busy-time TPS (timed ops / summed latencies, 4,080 ops per arm per day):

| day | off A | on A | declared A | off B | on B | declared B |
|---|---|---|---|---|---|---|
| 1 | 2,241 tps | 3,601 tps | 3,458 tps | 2,170 tps | 3,403 tps | 3,260 tps |
| 2 | 2,227 tps | 3,779 tps | 3,541 tps | 2,154 tps | 3,536 tps | 3,415 tps |
| 3 | 2,155 tps | 3,935 tps | 3,555 tps | 2,157 tps | 3,630 tps | 3,541 tps |

**What is resolved and what is not.** The `pk` control (touches no Cabin) reads 0.97x to 1.29x against the comparator and
13,175 to 15,175 qps across arms, days and runs; the on-over-off ratio on `pk` is 0.95x to 1.14x, so
**a ratio below about 1.15x is not a finding**, as at the comparator. That puts `tape200` (0.97x to 1.09x) at the floor, `tape1k`
(1.14x to 1.33x) barely over it, `tape10k` (2.15x to 2.84x) and `board` (1.95x to 2.40x) clear of it, and `open` and `close` (0.96x to
1.07x) as *not measurably different between arms*.

**Cross-version (now over `9a0525d`, derived qps, per arm; run A and run B):**

| group | day | off A | on A | declared A | off B | on B | declared B |
|---|---|---|---|---|---|---|---|
| board | 1 | 2.06x | 1.61x | 1.15x | 2.02x | 1.49x | 1.13x |
| board | 2 | 2.05x | 1.66x | 1.20x | 2.03x | 1.49x | 1.15x |
| board | 3 | 2.10x | 1.41x | 1.09x | 2.00x | 1.58x | 1.13x |
| tape200 | 1 | 1.16x | 1.03x | 1.10x | 1.22x | 0.98x | 1.11x |
| tape200 | 2 | 1.14x | 1.07x | 1.20x | 1.15x | 1.01x | 1.11x |
| tape200 | 3 | 1.13x | 1.09x | 1.01x | 1.13x | 1.10x | 1.10x |
| tape1k | 1 | 1.42x | 1.14x | 1.11x | 1.46x | 1.06x | 1.15x |
| tape1k | 2 | 1.40x | 1.08x | 1.20x | 1.39x | 1.03x | 1.12x |
| tape1k | 3 | 1.43x | 1.10x | 1.01x | 1.39x | 1.12x | 1.12x |
| tape10k | 1 | 2.19x | 1.76x | 1.20x | 2.13x | 1.70x | 1.24x |
| tape10k | 2 | 2.17x | 1.57x | 1.22x | 2.14x | 1.41x | 1.17x |
| tape10k | 3 | 2.22x | 1.60x | 1.13x | 2.10x | 1.54x | 1.34x |
| pk | 1 | 1.09x | 1.04x | 1.12x | 1.16x | 0.97x | 1.19x |
| pk | 2 | 1.04x | 1.11x | 1.29x | 1.13x | 1.02x | 1.17x |
| pk | 3 | 1.11x | 1.13x | 1.04x | 1.07x | 1.15x | 1.12x |
| open | 1 | 1.26x | 1.15x | 1.36x | 0.95x | 1.01x | 1.12x |
| open | 2 | 1.15x | 1.18x | 1.29x | 0.99x | 1.07x | 0.99x |
| open | 3 | 1.03x | 1.18x | 1.14x | 1.09x | 1.10x | 0.91x |
| close | 1 | 1.11x | 1.14x | 1.09x | 1.13x | 1.12x | 1.10x |
| close | 2 | 1.10x | 1.17x | 1.09x | 1.10x | 1.14x | 1.08x |
| close | 3 | 1.14x | 1.16x | 1.12x | 1.14x | 1.11x | 1.11x |

## 4. Percentiles

Every phase, in the driver's own order, microseconds; `max` and `err` as the driver reports them. Run A (server defaults):

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | err |
|---|---|---|---|---|---|---|---|---|
| load[off] | 31,200 ops | 33.6 µs | 41.2 µs | 41.8 µs | 59.1 µs | 71.1 µs | 172.5 µs | 0 errors |
| load[on] | 31,200 ops | 35.9 µs | 41.3 µs | 41.8 µs | 56.4 µs | 64.8 µs | 104.2 µs | 0 errors |
| load[declared] | 31,200 ops | 38.1 µs | 41.6 µs | 42.1 µs | 57.9 µs | 67.1 µs | 263.1 µs | 0 errors |
| d1-open[off] | 240 ops | 913.2 µs | 990.5 µs | 1,021.0 µs | 2,676.9 µs | 4,764.5 µs | 8,892.0 µs | 0 errors |
| d1-open[on] | 240 ops | 938.8 µs | 994.9 µs | 1,029.4 µs | 2,643.2 µs | 6,018.1 µs | 9,105.0 µs | 0 errors |
| d1-open[declared] | 240 ops | 905.8 µs | 983.0 µs | 1,024.9 µs | 1,687.2 µs | 6,327.0 µs | 9,371.2 µs | 0 errors |
| d1-board[off] | 2,400 ops | 430.7 µs | 490.0 µs | 509.0 µs | 556.1 µs | 590.4 µs | 870.1 µs | 0 errors |
| d1-tape200[off] | 396 ops | 97.6 µs | 100.3 µs | 103.2 µs | 139.4 µs | 156.2 µs | 172.9 µs | 0 errors |
| d1-tape1k[off] | 396 ops | 124.2 µs | 127.1 µs | 130.9 µs | 163.1 µs | 179.3 µs | 189.3 µs | 0 errors |
| d1-tape10k[off] | 396 ops | 420.3 µs | 431.4 µs | 441.5 µs | 482.7 µs | 518.2 µs | 536.7 µs | 0 errors |
| d1-pk[off] | 240 ops | 59.6 µs | 61.6 µs | 65.8 µs | 98.9 µs | 120.8 µs | 235.8 µs | 0 errors |
| d1-board[on] | 2,400 ops | 96.8 µs | 165.6 µs | 190.3 µs | 509.6 µs | 844.5 µs | 1,007.6 µs | 0 errors |
| d1-tape200[on] | 396 ops | 93.7 µs | 96.9 µs | 103.8 µs | 144.7 µs | 156.3 µs | 158.6 µs | 0 errors |
| d1-tape1k[on] | 396 ops | 93.5 µs | 96.8 µs | 124.7 µs | 165.1 µs | 198.3 µs | 208.4 µs | 0 errors |
| d1-tape10k[on] | 396 ops | 93.2 µs | 98.5 µs | 125.3 µs | 485.9 µs | 804.6 µs | 821.2 µs | 0 errors |
| d1-pk[on] | 240 ops | 59.1 µs | 61.1 µs | 65.6 µs | 101.9 µs | 121.6 µs | 246.8 µs | 0 errors |
| d1-board[declared] | 2,400 ops | 95.8 µs | 162.5 µs | 173.9 µs | 840.2 µs | 905.5 µs | 1,224.1 µs | 0 errors |
| d1-tape200[declared] | 396 ops | 93.5 µs | 95.9 µs | 98.2 µs | 124.2 µs | 138.2 µs | 152.4 µs | 0 errors |
| d1-tape1k[declared] | 396 ops | 93.3 µs | 95.8 µs | 97.8 µs | 180.6 µs | 196.1 µs | 217.7 µs | 0 errors |
| d1-tape10k[declared] | 396 ops | 93.1 µs | 97.0 µs | 102.3 µs | 836.9 µs | 935.8 µs | 960.8 µs | 0 errors |
| d1-pk[declared] | 240 ops | 59.2 µs | 60.9 µs | 61.9 µs | 87.5 µs | 107.1 µs | 240.2 µs | 0 errors |
| d1-close[off] | 12 ops | 686.1 µs | 706.3 µs | 758.9 µs | 4,115.9 µs | 4,115.9 µs | 4,115.9 µs | 0 errors |
| d1-close[on] | 12 ops | 699.4 µs | 718.5 µs | 830.3 µs | 3,971.0 µs | 3,971.0 µs | 3,971.0 µs | 0 errors |
| d1-close[declared] | 12 ops | 685.9 µs | 736.6 µs | 834.9 µs | 3,958.9 µs | 3,958.9 µs | 3,958.9 µs | 0 errors |
| d2-open[off] | 240 ops | 930.1 µs | 1,000.4 µs | 1,035.8 µs | 1,451.8 µs | 3,694.6 µs | 17,118.1 µs | 0 errors |
| d2-open[on] | 240 ops | 867.9 µs | 996.1 µs | 1,043.1 µs | 2,072.8 µs | 4,039.0 µs | 5,481.5 µs | 0 errors |
| d2-open[declared] | 240 ops | 950.1 µs | 1,004.6 µs | 1,042.0 µs | 1,677.5 µs | 3,857.4 µs | 11,843.5 µs | 0 errors |
| d2-board[off] | 2,400 ops | 429.6 µs | 491.1 µs | 510.4 µs | 560.0 µs | 590.7 µs | 831.2 µs | 0 errors |
| d2-tape200[off] | 396 ops | 97.7 µs | 100.2 µs | 103.8 µs | 142.4 µs | 158.1 µs | 164.4 µs | 0 errors |
| d2-tape1k[off] | 396 ops | 124.3 µs | 127.3 µs | 131.7 µs | 168.3 µs | 181.5 µs | 196.0 µs | 0 errors |
| d2-tape10k[off] | 396 ops | 419.0 µs | 427.7 µs | 439.4 µs | 485.5 µs | 524.7 µs | 547.5 µs | 0 errors |
| d2-pk[off] | 240 ops | 59.3 µs | 61.7 µs | 66.5 µs | 107.3 µs | 121.9 µs | 137.6 µs | 0 errors |
| d2-board[on] | 2,400 ops | 98.0 µs | 162.5 µs | 175.8 µs | 519.3 µs | 840.2 µs | 1,063.2 µs | 0 errors |
| d2-tape200[on] | 396 ops | 92.8 µs | 96.0 µs | 98.6 µs | 126.4 µs | 150.1 µs | 180.5 µs | 0 errors |
| d2-tape1k[on] | 396 ops | 93.2 µs | 95.8 µs | 98.1 µs | 141.6 µs | 183.3 µs | 769.1 µs | 0 errors |
| d2-tape10k[on] | 396 ops | 93.7 µs | 96.2 µs | 100.0 µs | 459.2 µs | 819.0 µs | 933.0 µs | 0 errors |
| d2-pk[on] | 240 ops | 59.4 µs | 61.0 µs | 62.2 µs | 84.2 µs | 94.1 µs | 124.0 µs | 0 errors |
| d2-board[declared] | 2,400 ops | 96.5 µs | 162.6 µs | 176.3 µs | 847.9 µs | 894.1 µs | 1,117.1 µs | 0 errors |
| d2-tape200[declared] | 396 ops | 93.5 µs | 95.8 µs | 98.5 µs | 131.2 µs | 153.0 µs | 159.6 µs | 0 errors |
| d2-tape1k[declared] | 396 ops | 93.2 µs | 95.7 µs | 98.2 µs | 169.7 µs | 201.0 µs | 218.0 µs | 0 errors |
| d2-tape10k[declared] | 396 ops | 93.5 µs | 96.2 µs | 101.1 µs | 825.0 µs | 928.0 µs | 985.0 µs | 0 errors |
| d2-pk[declared] | 240 ops | 59.0 µs | 61.2 µs | 63.1 µs | 95.4 µs | 115.3 µs | 129.9 µs | 0 errors |
| d2-close[off] | 12 ops | 698.0 µs | 733.7 µs | 828.8 µs | 3,766.4 µs | 3,766.4 µs | 3,766.4 µs | 0 errors |
| d2-close[on] | 12 ops | 680.8 µs | 721.3 µs | 844.8 µs | 3,908.3 µs | 3,908.3 µs | 3,908.3 µs | 0 errors |
| d2-close[declared] | 12 ops | 704.7 µs | 730.5 µs | 859.6 µs | 6,122.3 µs | 6,122.3 µs | 6,122.3 µs | 0 errors |
| d3-open[off] | 240 ops | 1,068.5 µs | 1,180.3 µs | 1,280.7 µs | 2,396.9 µs | 5,026.3 µs | 6,929.0 µs | 0 errors |
| d3-open[on] | 240 ops | 1,083.8 µs | 1,230.2 µs | 1,286.8 µs | 1,928.8 µs | 4,560.9 µs | 8,719.2 µs | 0 errors |
| d3-open[declared] | 240 ops | 1,063.0 µs | 1,255.0 µs | 1,343.3 µs | 2,508.4 µs | 6,584.3 µs | 12,040.3 µs | 0 errors |
| d3-board[off] | 2,400 ops | 440.5 µs | 503.1 µs | 518.5 µs | 563.6 µs | 713.3 µs | 1,110.3 µs | 0 errors |
| d3-tape200[off] | 396 ops | 97.2 µs | 100.3 µs | 103.2 µs | 136.7 µs | 153.5 µs | 166.5 µs | 0 errors |
| d3-tape1k[off] | 396 ops | 124.4 µs | 127.2 µs | 130.6 µs | 161.1 µs | 179.2 µs | 183.5 µs | 0 errors |
| d3-tape10k[off] | 396 ops | 420.2 µs | 428.4 µs | 439.3 µs | 479.4 µs | 508.9 µs | 536.3 µs | 0 errors |
| d3-pk[off] | 240 ops | 59.6 µs | 61.2 µs | 62.3 µs | 93.9 µs | 111.5 µs | 119.0 µs | 0 errors |
| d3-board[on] | 2,400 ops | 96.1 µs | 164.7 µs | 173.2 µs | 474.1 µs | 859.3 µs | 1,010.2 µs | 0 errors |
| d3-tape200[on] | 396 ops | 93.2 µs | 95.9 µs | 97.9 µs | 125.7 µs | 149.1 µs | 157.2 µs | 0 errors |
| d3-tape1k[on] | 396 ops | 93.3 µs | 96.1 µs | 99.0 µs | 141.3 µs | 189.8 µs | 192.3 µs | 0 errors |
| d3-tape10k[on] | 396 ops | 93.7 µs | 97.1 µs | 101.7 µs | 475.0 µs | 807.5 µs | 827.3 µs | 0 errors |
| d3-pk[on] | 240 ops | 59.0 µs | 61.2 µs | 62.4 µs | 90.5 µs | 114.8 µs | 118.2 µs | 0 errors |
| d3-board[declared] | 2,400 ops | 96.2 µs | 163.5 µs | 173.1 µs | 852.8 µs | 904.0 µs | 1,108.9 µs | 0 errors |
| d3-tape200[declared] | 396 ops | 93.0 µs | 96.2 µs | 99.2 µs | 136.9 µs | 174.1 µs | 215.2 µs | 0 errors |
| d3-tape1k[declared] | 396 ops | 93.2 µs | 95.9 µs | 99.1 µs | 143.5 µs | 183.3 µs | 202.9 µs | 0 errors |
| d3-tape10k[declared] | 396 ops | 92.7 µs | 96.3 µs | 104.5 µs | 840.4 µs | 953.2 µs | 964.2 µs | 0 errors |
| d3-pk[declared] | 240 ops | 59.3 µs | 61.2 µs | 62.9 µs | 96.8 µs | 113.9 µs | 132.5 µs | 0 errors |
| d3-close[off] | 12 ops | 687.0 µs | 714.1 µs | 740.2 µs | 3,730.3 µs | 3,730.3 µs | 3,730.3 µs | 0 errors |
| d3-close[on] | 12 ops | 732.6 µs | 741.0 µs | 864.1 µs | 3,956.7 µs | 3,956.7 µs | 3,956.7 µs | 0 errors |
| d3-close[declared] | 12 ops | 711.7 µs | 747.1 µs | 850.7 µs | 3,841.0 µs | 3,841.0 µs | 3,841.0 µs | 0 errors |

Run B (cooldown 2 half-lives):

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | err |
|---|---|---|---|---|---|---|---|---|
| load[off] | 31,200 ops | 34.2 µs | 41.5 µs | 42.0 µs | 59.3 µs | 69.2 µs | 431.9 µs | 0 errors |
| load[on] | 31,200 ops | 39.6 µs | 41.5 µs | 55.3 µs | 65.1 µs | 79.1 µs | 236.1 µs | 0 errors |
| load[declared] | 31,200 ops | 34.2 µs | 41.2 µs | 41.8 µs | 60.1 µs | 76.2 µs | 1,958.0 µs | 0 errors |
| d1-open[off] | 240 ops | 1,056.9 µs | 1,243.9 µs | 1,338.1 µs | 2,063.7 µs | 5,624.1 µs | 8,495.8 µs | 0 errors |
| d1-open[on] | 240 ops | 1,072.5 µs | 1,265.2 µs | 1,348.5 µs | 2,290.8 µs | 5,408.5 µs | 11,486.5 µs | 0 errors |
| d1-open[declared] | 240 ops | 1,045.5 µs | 1,211.6 µs | 1,279.4 µs | 2,426.9 µs | 5,065.1 µs | 11,872.0 µs | 0 errors |
| d1-board[off] | 2,400 ops | 429.3 µs | 490.3 µs | 508.1 µs | 556.2 µs | 616.5 µs | 856.4 µs | 0 errors |
| d1-tape200[off] | 396 ops | 97.9 µs | 100.1 µs | 102.7 µs | 127.8 µs | 139.7 µs | 156.3 µs | 0 errors |
| d1-tape1k[off] | 396 ops | 124.6 µs | 127.2 µs | 129.7 µs | 162.0 µs | 180.8 µs | 184.1 µs | 0 errors |
| d1-tape10k[off] | 396 ops | 418.4 µs | 429.9 µs | 442.2 µs | 486.9 µs | 551.8 µs | 579.4 µs | 0 errors |
| d1-pk[off] | 240 ops | 59.6 µs | 61.3 µs | 63.0 µs | 98.9 µs | 123.0 µs | 249.3 µs | 0 errors |
| d1-board[on] | 2,400 ops | 95.9 µs | 166.8 µs | 189.7 µs | 515.8 µs | 815.4 µs | 982.2 µs | 0 errors |
| d1-tape200[on] | 396 ops | 93.0 µs | 96.5 µs | 103.9 µs | 138.8 µs | 153.2 µs | 167.7 µs | 0 errors |
| d1-tape1k[on] | 396 ops | 93.2 µs | 97.5 µs | 112.7 µs | 165.8 µs | 195.2 µs | 199.1 µs | 0 errors |
| d1-tape10k[on] | 396 ops | 93.4 µs | 97.9 µs | 114.4 µs | 483.4 µs | 833.9 µs | 906.7 µs | 0 errors |
| d1-pk[on] | 240 ops | 59.9 µs | 61.8 µs | 65.3 µs | 104.3 µs | 119.5 µs | 242.1 µs | 0 errors |
| d1-board[declared] | 2,400 ops | 95.3 µs | 163.0 µs | 178.3 µs | 848.0 µs | 893.9 µs | 1,130.0 µs | 0 errors |
| d1-tape200[declared] | 396 ops | 92.5 µs | 95.8 µs | 98.5 µs | 131.9 µs | 152.9 µs | 160.7 µs | 0 errors |
| d1-tape1k[declared] | 396 ops | 92.6 µs | 95.7 µs | 99.0 µs | 177.2 µs | 199.9 µs | 212.1 µs | 0 errors |
| d1-tape10k[declared] | 396 ops | 92.8 µs | 96.9 µs | 104.1 µs | 834.5 µs | 934.8 µs | 952.4 µs | 0 errors |
| d1-pk[declared] | 240 ops | 59.3 µs | 60.8 µs | 62.6 µs | 94.3 µs | 116.8 µs | 238.3 µs | 0 errors |
| d1-close[off] | 12 ops | 677.5 µs | 706.9 µs | 742.2 µs | 4,109.4 µs | 4,109.4 µs | 4,109.4 µs | 0 errors |
| d1-close[on] | 12 ops | 696.5 µs | 759.4 µs | 860.9 µs | 3,894.1 µs | 3,894.1 µs | 3,894.1 µs | 0 errors |
| d1-close[declared] | 12 ops | 693.6 µs | 753.0 µs | 839.3 µs | 3,991.1 µs | 3,991.1 µs | 3,991.1 µs | 0 errors |
| d2-open[off] | 240 ops | 1,050.8 µs | 1,209.7 µs | 1,280.8 µs | 2,808.6 µs | 7,466.5 µs | 8,991.9 µs | 0 errors |
| d2-open[on] | 240 ops | 1,108.0 µs | 1,249.3 µs | 1,301.5 µs | 1,780.9 µs | 6,000.5 µs | 12,436.7 µs | 0 errors |
| d2-open[declared] | 240 ops | 1,085.2 µs | 1,197.1 µs | 1,245.6 µs | 1,534.0 µs | 6,705.3 µs | 12,499.8 µs | 0 errors |
| d2-board[off] | 2,400 ops | 431.0 µs | 490.3 µs | 506.2 µs | 541.8 µs | 614.5 µs | 835.2 µs | 0 errors |
| d2-tape200[off] | 396 ops | 97.2 µs | 99.9 µs | 102.3 µs | 129.4 µs | 150.4 µs | 180.3 µs | 0 errors |
| d2-tape1k[off] | 396 ops | 124.5 µs | 127.2 µs | 130.4 µs | 161.0 µs | 238.9 µs | 268.4 µs | 0 errors |
| d2-tape10k[off] | 396 ops | 418.5 µs | 429.5 µs | 440.6 µs | 477.9 µs | 523.5 µs | 615.0 µs | 0 errors |
| d2-pk[off] | 240 ops | 59.6 µs | 61.3 µs | 62.6 µs | 89.7 µs | 104.5 µs | 119.8 µs | 0 errors |
| d2-board[on] | 2,400 ops | 96.9 µs | 161.9 µs | 176.8 µs | 520.3 µs | 839.0 µs | 1,056.6 µs | 0 errors |
| d2-tape200[on] | 396 ops | 93.2 µs | 96.2 µs | 98.9 µs | 130.2 µs | 146.2 µs | 497.4 µs | 0 errors |
| d2-tape1k[on] | 396 ops | 93.3 µs | 96.2 µs | 99.1 µs | 149.5 µs | 185.4 µs | 223.1 µs | 0 errors |
| d2-tape10k[on] | 396 ops | 93.2 µs | 96.4 µs | 102.9 µs | 485.9 µs | 846.0 µs | 923.5 µs | 0 errors |
| d2-pk[on] | 240 ops | 59.7 µs | 61.7 µs | 63.3 µs | 94.0 µs | 116.4 µs | 137.0 µs | 0 errors |
| d2-board[declared] | 2,400 ops | 96.5 µs | 160.6 µs | 172.2 µs | 841.7 µs | 917.3 µs | 1,700.4 µs | 0 errors |
| d2-tape200[declared] | 396 ops | 92.9 µs | 95.4 µs | 97.4 µs | 123.4 µs | 147.1 µs | 157.6 µs | 0 errors |
| d2-tape1k[declared] | 396 ops | 92.8 µs | 95.2 µs | 96.9 µs | 168.5 µs | 191.6 µs | 206.0 µs | 0 errors |
| d2-tape10k[declared] | 396 ops | 92.5 µs | 95.7 µs | 98.9 µs | 817.3 µs | 918.1 µs | 930.9 µs | 0 errors |
| d2-pk[declared] | 240 ops | 59.3 µs | 61.0 µs | 62.0 µs | 84.8 µs | 97.5 µs | 110.4 µs | 0 errors |
| d2-close[off] | 12 ops | 674.3 µs | 732.3 µs | 756.8 µs | 3,762.6 µs | 3,762.6 µs | 3,762.6 µs | 0 errors |
| d2-close[on] | 12 ops | 685.9 µs | 725.9 µs | 803.2 µs | 3,771.6 µs | 3,771.6 µs | 3,771.6 µs | 0 errors |
| d2-close[declared] | 12 ops | 747.0 µs | 768.2 µs | 870.7 µs | 6,285.6 µs | 6,285.6 µs | 6,285.6 µs | 0 errors |
| d3-open[off] | 240 ops | 1,157.8 µs | 1,262.9 µs | 1,323.0 µs | 1,682.5 µs | 2,977.6 µs | 3,530.4 µs | 0 errors |
| d3-open[on] | 240 ops | 1,080.4 µs | 1,195.7 µs | 1,244.5 µs | 1,846.2 µs | 4,558.0 µs | 8,920.6 µs | 0 errors |
| d3-open[declared] | 240 ops | 1,055.3 µs | 1,275.9 µs | 1,332.9 µs | 4,074.5 µs | 5,813.0 µs | 16,779.2 µs | 0 errors |
| d3-board[off] | 2,400 ops | 442.0 µs | 507.3 µs | 525.3 µs | 561.9 µs | 588.5 µs | 851.6 µs | 0 errors |
| d3-tape200[off] | 396 ops | 97.4 µs | 100.4 µs | 104.0 µs | 138.8 µs | 158.9 µs | 162.3 µs | 0 errors |
| d3-tape1k[off] | 396 ops | 124.6 µs | 127.5 µs | 132.4 µs | 165.7 µs | 181.2 µs | 182.3 µs | 0 errors |
| d3-tape10k[off] | 396 ops | 419.0 µs | 435.1 µs | 447.4 µs | 480.2 µs | 514.0 µs | 546.2 µs | 0 errors |
| d3-pk[off] | 240 ops | 59.6 µs | 61.2 µs | 63.6 µs | 99.8 µs | 114.8 µs | 138.7 µs | 0 errors |
| d3-board[on] | 2,400 ops | 96.5 µs | 167.2 µs | 176.1 µs | 529.0 µs | 842.9 µs | 1,032.6 µs | 0 errors |
| d3-tape200[on] | 396 ops | 92.9 µs | 96.0 µs | 98.1 µs | 120.5 µs | 136.6 µs | 143.5 µs | 0 errors |
| d3-tape1k[on] | 396 ops | 93.6 µs | 95.5 µs | 98.0 µs | 131.0 µs | 190.1 µs | 203.7 µs | 0 errors |
| d3-tape10k[on] | 396 ops | 93.8 µs | 96.6 µs | 101.0 µs | 479.8 µs | 793.2 µs | 839.9 µs | 0 errors |
| d3-pk[on] | 240 ops | 59.5 µs | 61.3 µs | 63.7 µs | 91.3 µs | 105.3 µs | 122.8 µs | 0 errors |
| d3-board[declared] | 2,400 ops | 95.5 µs | 161.9 µs | 171.1 µs | 849.5 µs | 916.2 µs | 1,131.7 µs | 0 errors |
| d3-tape200[declared] | 396 ops | 93.0 µs | 95.4 µs | 97.6 µs | 122.5 µs | 137.5 µs | 186.3 µs | 0 errors |
| d3-tape1k[declared] | 396 ops | 93.2 µs | 95.5 µs | 97.2 µs | 116.9 µs | 186.0 µs | 203.8 µs | 0 errors |
| d3-tape10k[declared] | 396 ops | 93.2 µs | 96.4 µs | 101.4 µs | 856.6 µs | 1,310.5 µs | 1,351.7 µs | 0 errors |
| d3-pk[declared] | 240 ops | 59.0 µs | 61.2 µs | 62.5 µs | 85.5 µs | 105.2 µs | 154.5 µs | 0 errors |
| d3-close[off] | 12 ops | 690.6 µs | 724.3 µs | 760.1 µs | 3,748.8 µs | 3,748.8 µs | 3,748.8 µs | 0 errors |
| d3-close[on] | 12 ops | 743.0 µs | 752.2 µs | 828.9 µs | 3,775.7 µs | 3,775.7 µs | 3,775.7 µs | 0 errors |
| d3-close[declared] | 12 ops | 692.7 µs | 723.2 µs | 835.0 µs | 4,221.2 µs | 4,221.2 µs | 4,221.2 µs | 0 errors |

## 5. Where the time goes

A probe is one round trip on one connection, so the decomposition is fixed cost plus rows walked:

| Component | Now | Comparator | How derived |
|---|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | **69.3 µs** | about 70 µs | `pk` mean across arms, days and runs (65.9 to 75.9 µs) |
| Rows walked by an unserved equality | **35 ns per row** plus about 96 µs | 87 ns per row plus about 102 µs | `off` tape p50 at 200 rows 103.2 µs and at 10,000 rows 441.5 µs (day 1, run A): (441.5 - 103.2) / 9,800 |
| A Cabin-served equality | about 98 to 100 µs at any size once built (day 2); 104 to 125 µs on day 1 while the Cabins are still being created and extended | about 104 to 113 µs | `on` and `declared` `tape200` / `tape1k` / `tape10k` p50 after the first day-1 observation: 103.8, 124.7, 125.3 µs on day 1 (still building), 98.6, 98.1, 100.0 µs on day 2; flat in n |
| Unobserved values and coverage misses | a walked tail on both served arms; see the tail table | `board` p95 1,130 µs `on`, 1,061 µs `declared` | a Cabin is authoritative only for observed values (`docs/spec/cabin.md` C1), so a probe of a value not yet observed walks in either arm; the uniform draw (20 % of each board draw) keeps meeting new ones |
| Write-side upkeep | not resolvable: `open` p50 1,021 / 1,029 / 1,025 µs (`off` / `on` / `declared`, day 1, run A) | 1,328 / 1,353 / 1,335 µs | inside the floor between arms; **run B's `open` is 1,338 / 1,348 / 1,279 µs**, 31 % slower than run A's for the same code, five minutes later: the device's sync moves by about a third between runs (section 9) |
| Durability / commit | not applicable to the probes; `open` and `load` are the only writers and each statement is one commit | | |
| Lock or conflict wait | none: one connection | | |

The board's latency tail by arm (run A day by day; run B and the comparator for the p95), microseconds:

| day | arm | run A mean | run A p50 | run A p95 | run A p99 | run B p95 | 9a0525d run A p95 |
|---|---|---|---|---|---|---|---|
| 1 | off | 506.2 µs | 509.0 µs | 556.1 µs | 590.4 µs | 556.2 µs | 1,104.5 µs |
| 1 | on | 259.2 µs | 190.3 µs | 509.6 µs | 844.5 µs | 515.8 µs | 1,130.3 µs |
| 1 | declared | 285.2 µs | 173.9 µs | 840.2 µs | 905.5 µs | 848.0 µs | 1,060.5 µs |
| 2 | off | 510.6 µs | 510.4 µs | 560.0 µs | 590.7 µs | 541.8 µs | 1,097.1 µs |
| 2 | on | 254.5 µs | 175.8 µs | 519.3 µs | 840.2 µs | 520.3 µs | 1,139.3 µs |
| 2 | declared | 279.5 µs | 176.3 µs | 847.9 µs | 894.1 µs | 841.7 µs | 1,081.2 µs |
| 3 | off | 516.4 µs | 518.5 µs | 563.6 µs | 713.3 µs | 561.9 µs | 1,170.4 µs |
| 3 | on | 215.5 µs | 173.2 µs | 474.1 µs | 859.3 µs | 529.0 µs | 1,136.3 µs |
| 3 | declared | 238.7 µs | 173.1 µs | 852.8 µs | 904.0 µs | 849.5 µs | 1,064.0 µs |

**The declared arm now has the heavier tail.** In every day and in both runs the `declared` arm's board p95 is 840 to 853 µs against
the `on` arm's 474 to 529 µs and the walk's 542 to 564 µs; at the comparator the three arms' p95 lay within 5 % of each other
(1,018 to 1,139 µs). A probe that the declared Cabin cannot answer therefore costs about 1.5x a plain walk, and the controller's
does not. This is why the `on` arm's mean (259 µs, day 1 run A) is lower than the `declared` arm's (285 µs) although the `declared`
p50 is lower (174 against 190 µs): the order of the two arms in the mean is the reverse of the comparator's (416 against 329 µs). This run does not say why the
declared arm's miss path is costlier; the controller arm's tail is not.

Server CPU seconds by arm (the driver's `/proc` reading), now and at the comparator:

| arm | board A | board B | board 9a0525d A / B | tape A | tape B | tape 9a0525d A / B | open A | close A |
|---|---|---|---|---|---|---|---|---|
| off | 2.80 s | 2.81 s | 6.71 s / 6.43 s | 0.52 s | 0.58 s | 1.29 s / 1.31 s | 0.07 s | 0.03 s |
| on | 0.84 s | 0.96 s | 1.87 s / 1.96 s | 0.25 s | 0.23 s | 0.35 s / 0.28 s | 0.06 s | 0.04 s |
| declared | 1.11 s | 1.08 s | 1.33 s / 1.29 s | 0.19 s | 0.27 s | 0.32 s / 0.33 s | 0.07 s | 0.03 s |

`off` burns 3.32 s of server CPU on a run's boards and tapes (comparator
8.00 s), the controller
1.09 s and the declared arm 1.30 s.

## 6. The controller's lifecycle

Snapshots of `SHOW CABIN_OPTIMIZER` on the `on` arm at chosen points, run A then run B:

run A:
| t (s) | day | tag | managed | creates | extends | drops | entries |
|---|---|---|---|---|---|---|---|
| 13.5 s | 1 | trading-block-3 | 4 Cabins | 4 creates | 31 extends | 0 drops | board_a_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/1p; tape_10k_a:ACTIVE/1p |
| 36.0 s | 1 | trading-block-9 | 4 Cabins | 4 creates | 103 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 47.2 s | 1 | trading-block-12 | 4 Cabins | 4 creates | 103 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 53.8 s | 1 | overnight | 4 Cabins | 4 creates | 103 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 104.4 s | 2 | trading-block-3 | 5 Cabins | 5 creates | 133 extends | 0 drops | board_a_a:ACTIVE/9p; board_b_a:ACTIVE/3p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 126.9 s | 2 | trading-block-9 | 5 Cabins | 5 creates | 141 extends | 0 drops | board_a_a:DECAYING/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 138.2 s | 2 | trading-block-12 | 5 Cabins | 5 creates | 141 extends | 0 drops | board_a_a:DECAYING/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 144.8 s | 2 | overnight | 5 Cabins | 5 creates | 141 extends | 0 drops | board_a_a:DECAYING/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 195.6 s | 3 | trading-block-3 | 5 Cabins | 5 creates | 163 extends | 0 drops | board_a_a:ACTIVE/18p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/5p |
| 218.1 s | 3 | trading-block-9 | 5 Cabins | 5 creates | 170 extends | 0 drops | board_a_a:ACTIVE/18p; board_b_a:DECAYING/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 229.3 s | 3 | trading-block-12 | 5 Cabins | 5 creates | 170 extends | 0 drops | board_a_a:ACTIVE/18p; board_b_a:DECAYING/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 235.9 s | 3 | overnight | 5 Cabins | 5 creates | 170 extends | 0 drops | board_a_a:ACTIVE/18p; board_b_a:DECAYING/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
final header: `cabin_optimizer=on managed=5 pages_committed=32 page_budget=1024 ticks=546 creates=5 extends=170 heals=0 drops=0 deferred=0 failures=0`
verify per day: PASS, PASS, PASS

run B:
| t (s) | day | tag | managed | creates | extends | drops | entries |
|---|---|---|---|---|---|---|---|
| 14.1 s | 1 | trading-block-3 | 4 Cabins | 4 creates | 32 extends | 0 drops | board_a_a:ACTIVE/3p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/1p; tape_10k_a:ACTIVE/1p |
| 36.6 s | 1 | trading-block-9 | 4 Cabins | 4 creates | 106 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 47.8 s | 1 | trading-block-12 | 4 Cabins | 4 creates | 106 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 54.4 s | 1 | overnight | 4 Cabins | 4 creates | 106 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 105.2 s | 2 | trading-block-3 | 5 Cabins | 5 creates | 136 extends | 0 drops | board_a_a:ACTIVE/9p; board_b_a:ACTIVE/3p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 127.7 s | 2 | trading-block-9 | 5 Cabins | 5 creates | 144 extends | 0 drops | board_a_a:DECAYING/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 139.0 s | 2 | trading-block-12 | 4 Cabins | 5 creates | 144 extends | 1 drops | board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 145.5 s | 2 | overnight | 4 Cabins | 5 creates | 144 extends | 1 drops | board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 196.4 s | 3 | trading-block-3 | 5 Cabins | 6 creates | 167 extends | 1 drops | board_a_a:ACTIVE/5p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 218.9 s | 3 | trading-block-9 | 5 Cabins | 6 creates | 189 extends | 1 drops | board_a_a:ACTIVE/14p; board_b_a:DECAYING/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 230.1 s | 3 | trading-block-12 | 4 Cabins | 6 creates | 196 extends | 2 drops | board_a_a:ACTIVE/14p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 236.7 s | 3 | overnight | 4 Cabins | 6 creates | 196 extends | 2 drops | board_a_a:ACTIVE/14p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
final header: `cabin_optimizer=on managed=4 pages_committed=24 page_budget=1024 ticks=547 creates=6 extends=196 heals=0 drops=2 deferred=0 failures=0`
verify per day: PASS, PASS, PASS

Run A, defaults: four Cabins are created in the first 13.5 s of day 1 (`board_a`, `tape_200`, `tape_1k`, `tape_10k`: 4 creates,
31 extends by block 3), grown by 103 extends by the end of the day, and a fifth (`board_b`) on day 2's first block. `board_a`
goes **DECAYING** on day 2 and **returns to ACTIVE** on day 3 with its pages doubled (9 to 18) when its hot set comes back; then
`board_b` goes DECAYING. At the end: `creates=5 extends=170 heals=0 drops=0 deferred=0 failures=0`. **No DROP occurs in a 3-day
run at the defaults**: the DECAYING dwell before a DROP is `cabin_optimizer_cooldown_half_lives`, default 128
(`docs/spec/physical-optimizer.md` II.6), which at `decay_half_life = 5` is 640 s; the driver's whole run is 278 s and its nights are
45 s. The driver's docstring assumes a 2-half-life cooldown, which is not the engine's default and is what run B sets.

Run B, cooldown 2 half-lives: the same creates on day 1, `board_a` DECAYING at 127.7 s and **DROPPED** by 139.0 s (`drops=1`); its Cabin is gone
while `board_b` is hot; on day 3 `board_a` is **re-nominated from scratch** (`creates=6`, 5 pages at 196 s, 14 by the end) and
`board_b` is dropped at day 3's last block (`drops=2`). Final: `creates=6 extends=196 heals=0 drops=2 failures=0`. The cost of
re-nomination shows in the served rate of the day it happens: `d3-board[on]` runs 4,640 qps in run A (the Cabin survived the night)
and 3,905 qps in run B (rebuilt), -16 %, against a run A / run B difference of 0.1 % and 0.5 % on days 1 and 2 (3,858 / 3,854 and 3,929 / 3,908
qps, run A / run B). The comparator's same cell was -25 % (3,283 and 2,467 qps). Single draw per run.

## 7. Correctness

The driver compares an identical verification set (hot probes, a zero-row probe, `COUNT` / `SUM`, 18 statements) across the
three arms byte for byte each day: **PASS on all three days in both runs**, `client_errors 0`, `slot_overruns 0`. A Cabin,
however created, dropped or re-nominated, changed no reply.

## 8. Against PostgreSQL 18.6 (reused, not re-measured)

PostgreSQL stays the floor. The PG twin (`tools/pg_scenario4_cabinopt_days.py`) runs **day 1 only, unpaced, one server, no
index**, by its own design: at defaults a PostgreSQL probe seq-scans on day 1 as on day 300. Its figures are the three
runs measured at `f2f1ee7` on 2026-09-30, 14 days before this run, and are not re-measured. KDS side: run A day 1,
derived qps, ratio KDS over PostgreSQL (above 1.00x KDS is faster):

| group | PG18 (reused, no index) | KDS off (walks) | KDS on (controller) | KDS declared (Cabin) | KDS off / PG | KDS on / PG | KDS declared / PG |
|---|---|---|---|---|---|---|---|
| open | 823 qps | 815 qps | 798 qps | 839 qps | 0.99x | 0.97x | 1.02x |
| board | 1,994 qps | 1,976 qps | 3,858 qps | 3,506 qps | 0.99x | 1.93x | 1.76x |
| tape200 | 12,690 qps | 9,091 qps | 8,961 qps | 9,775 qps | 0.72x | 0.71x | 0.77x |
| tape1k | 8,985 qps | 7,294 qps | 8,389 qps | 9,066 qps | 0.81x | 0.93x | 1.01x |
| tape10k | 2,113 qps | 2,238 qps | 4,808 qps | 4,369 qps | 1.06x | 2.28x | 2.07x |
| pk | 15,649 qps | 13,405 qps | 13,333 qps | 14,948 qps | 0.86x | 0.85x | 0.96x |
| close | 1,183 qps | 661 qps | 658 qps | 653 qps | 0.56x | 0.56x | 0.55x |

PostgreSQL's three-run spread was 1.7 % to 11.4 % per group (its file, section 3). **The walk is no longer the weak side**: KDS's `off`
arm on the board and `tape10k` is 0.99x and 1.06x of PostgreSQL's seqscan (it was 0.48x on both), and the
controller and the declared arm are 1.93x / 1.76x on the board and 2.28x / 2.07x on `tape10k`
(they were 1.20x / 1.52x and 1.30x / 1.72x). `close` (full-scan aggregates) is 0.56x to 0.55x, a walk that
decodes columns to fold (it was 0.49x to 0.51x). The fixed round trip (`pk`) is 0.86x to 0.96x (it was 0.79x to
0.85x). **Not measured**: PostgreSQL with an index on `symbol`, the fair counterpart of a *declared* Cabin; the comparison is against
PostgreSQL's seqscan only.

## 9. Noise floor and host stamps

Inside the run: the `pk` control, 0.97x to 1.29x of the comparator and 0.95x to 1.14x between arms. Across the two runs: the `off` arm,
which does not depend on any run difference, has `board` p50 509.0 / 510.4 / 518.5 µs in A and 508.1 / 506.2 / 525.3 µs in B (day by
day up to 1.3 %), and `tape10k` p50 441.5 / 439.4 / 439.3 against 442.2 / 440.6 / 447.4 µs (up to 1.8 %): **the `off` walk is reproducible
to about 2 %**, tighter than the comparator's 4 %. The served arms are noisier (`board[on]` day 3 4,640 against 3,905 qps, section 6).

**The durable-write thermometer moved.** `open` (240 durable inserts per day, one commit each) has p50 1,021 to 1,025 µs on day 1 of run A and
1,338 to 1,279 µs on day 1 of run B, with the same binary, same code and the same host five minutes apart; day 3 of run A is 1,281 to 1,343 µs.
The device's sync, not the engine, moves about 30 % here, which is the floor for any durable-write delta read from this file. `load` (non-durable
batches) is stable: p50 41.8 / 41.8 / 42.1 µs in run A and 42.0 / 55.3 / 41.8 µs in run B.
`recovery_checkpoint_us` was not sampled.

| run | arm precheck | window UTC | loadavg 1/5/15 | gate wait | build/test at precheck | monitor samples | build/test | foreign >= 50 % | minor |
|---|---|---|---|---|---|---|---|---|---|
| `s4` | `s4-off` (the three arms' prechecks are identical in time) | 2026-10-14 00:34:29 to 00:39:15 | 1.20 / 1.24 / 2.78 | 0 s | none | 129 | 0 | 0 | 0 |
| `cd2-s4` | `cd2-s4-off` (the three arms' prechecks are identical in time) | 2026-10-14 00:39:23 to 00:44:10 | 0.16 / 0.62 / 2.09 | 0 s | none | 129 | 0 | 0 | 0 |

Both runs were clean (no build or test sample in either window).

## 10. What the run says about the engine

1. **A walked row is half what it was**: 35 ns per row (87 at the comparator), so the hot-equality `off` arm is 2.06x to 2.19x
   faster and the Cabin's multiple over it fell. A Cabin is still worth 2.1x to 2.8x at 10,000 rows and nothing at 200; its
   value is the row count times the per-row cost, and both of those moved together with the walk.
2. **The controller gets the declared ceiling without being told and, in the mean, now exceeds it** (3,858 against 3,506 qps on day 1), because the
   declared arm's coverage-miss path costs more than a plain walk (p95 840 against 556 µs) and the controller's does not. The
   mechanism is not shown by this run; `docs/spec/cabin.md` §6 is where a declared set's miss path is specified.
3. **Nothing in the default configuration ever retires a Cabin in the working week the driver models.** With a 128-half-life cooldown DECAYING is reversible
   and DROP is out of reach of any run shorter than 128 half-lives; a fact about the default, not a defect.
   The driver's stated intent (DROP and re-nomination) needs `cabin_optimizer_cooldown_half_lives` lowered, and `docs/spec/physical-optimizer.md`
   §II.4 states the price.
4. **Re-nomination costs less than it did**: -16 % on the rebuilt day against -25 %, because the walk it falls back on is half as slow.
5. **Nothing here approached a limit of the engine**: one connection, one core, 10,000-row relations, `page_budget=1024` with 24 to 32 pages
   committed. The scenario measures a mechanism, not a ceiling.

Archive: `bench/v3.1.0/archive/scenario4-v3.1.0-30-gcd7c7c4e/`.
