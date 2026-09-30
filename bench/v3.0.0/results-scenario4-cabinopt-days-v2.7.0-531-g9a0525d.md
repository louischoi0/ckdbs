# Scenario 4 (cabin optimizer over business days) at `v2.7.0-531-g9a0525d`

**One line.** The autonomous Cabin controller delivers most of what a
hand-declared Cabin does on a hot non-pk equality, losing the rest to a
larger share of probes that still walk: on a 10,000-row board the `on` arm serves at
2.4 to 3.3 kqps and the `declared` arm at 3.0 to 3.8 kqps, against 0.92 to
0.98 kqps for the `off` arm that walks, with byte-identical answers on all
three days in both runs. The controller creates, extends, decays and
recovers as specified, **but with the driver's stated compression
(`decay_half_life = 5`) and the default `cabin_optimizer_cooldown_half_lives
= 128` it never drops a Cabin**; a second run with the cooldown at 2
half-lives exercises DROP and re-nomination and finds re-nomination costs
day 3 about a quarter of its board throughput.

Driver: `tools/scenario4_cabinopt_days.py`, unmodified, three servers (arms
`off`, `on`, `declared`) on fresh data files, blocks interleaved so only
one arm is active at a time. **No v3 predecessor: no delta.** This file is
the baseline the next scenario4 run is read against.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-09-30; run A 06:52:14 to 06:56:54 UTC (279.2 s), run B 06:57:33 to 07:02:15 |
| Worktree / branch | `bench-rerun-scenarios` / `worktree-bench-rerun-scenarios` |
| HEAD | `fc2d343`, which over `9a0525d` only deletes `bench/v3.0.0/` and edits `bench/README.md`; `src/`, `include/`, `tools/` and `CMakeLists.txt` are identical |
| Engine commit measured | `9a0525d`, `git describe --tags` = `v2.7.0-531-g9a0525d` ("AY-S11: AY closed") |
| Tree cleanliness | Clean at the start; nothing edited or built during the cells |
| Binary provenance | `/home/cdkbs/bench-runs/rebaseline-9a0525d/kds_server`, copy of `build-release/kds_server`, **sha256 `1d07d33386541ec3a13090d37918e229957e29c2c777ff15f2a63db6957f8746`**, source binary mtime 2026-09-30 06:30:43 UTC (after `9a0525d` at 06:19:05 and `fc2d343` at 06:29:21); all three servers of both runs started from the copy |
| Device | data files under `/home/cdkbs/bench-runs/rebaseline-9a0525d/data/`, `/dev/root`, `ext4` (`df -T`). Not tmpfs. Fresh data files, deleted after each run |
| Build type | Release (`CMakeCache.txt`), built to completion before the first cell |
| Host | 8 logical CPUs, AMD EPYC 9V74, 1 socket x 4 cores x 2 threads, Linux 7.0.0-1014-azure |
| Server config (all three servers) | `cores = 1`, `durability = group`, `log_level = warn`, `decay_half_life = 5` (seconds; the driver docstring's 120x compression of the 600 s default), `cabin_optimizer_snapshot_interval_ms = 500`; **run B only** `cabin_optimizer_cooldown_half_lives = 2`. Ports 15651 to 15653, never 15432. The `on` arm runs `SET CABIN_OPTIMIZER ON` at day 1 open; the `declared` arm declares `CREATE CABIN` on every board and tape symbol column up front |
| Driver arguments | defaults: `--days 3 --blocks 12 --session-seconds 45 --overnight-seconds 45 --open-inserts 240 --board-probes 2400 --tape-probes 396 --pk-ops 240 --close-rounds 3 --seed 20260810 --suffix a`, plus `--port-off/-on/-declared` and the three server pids |
| Host stamp | precheck loadavg 1.69 / 1.65 / 1.54 (run A, the tail of the scenario1 attempt and the preceding passes) and 0.15 / 0.76 / 1.18 (run B); no `cc1plus`, `cmake --build` or `ctest` process at either |
| PostgreSQL | not installed on this host when this file was written and **not measured here**; the PostgreSQL 18.6 floor for this shape was measured afterwards, at `v2.7.0-545-gf2f1ee7`, and is in `results-scenario4-cabinopt-days-pg18-v2.7.0-545-gf2f1ee7.md` (summary: `results-kds-vs-pg18-summary-v2.7.0-545-gf2f1ee7.md`) |

## 2. What was run

Two complete runs of the driver, three days of three arms each: **run A**
at the server defaults with the driver's compression, **run B** identical
but for one server key, `cabin_optimizer_cooldown_half_lives = 2`. Run B
exists because run A cannot exercise DROP (section 6). Per day: an open
(240 inserts into the day's board), a paced 45 s trading session (2,400
hot-board probes, 396 probes on each of three tape sizes, 240 pk controls,
per arm, interleaved in 12 blocks), a close (`COUNT` / `SUM` / `GROUP BY`
full scans, three rounds) and a 45 s idle night. The equal work per arm is
fixed by the driver's counts, not by time.

**Row-set axis (rule 9).** The tape relations are 200, 1,000 and 10,000
rows (`tape_200`, `tape_1k`, `tape_10k`); the boards are 10,000 rows each
(`board_a`, `board_b`). Every phase is a row of the matrix and the size is a
row label. The measured statements are serial on one connection, so qps is
derived as 1,000,000 / mean us.

**The hot set rotates**: day 1 hot on `board_a`, day 2 on `board_b`, day 3 on
`board_a` with a different symbol set; the tapes are hot every day with
disjoint values.

No build or test process was live at any point.

## 3. Throughput and latency by arm

p50 and derived qps per group, day and arm, run A / run B. The last column
is the ratio of mean latencies off over on.

| group | day | off A | off B | on A | on B | declared A | declared B | on/off speed-up (A / B) |
|---|---|---|---|---|---|---|---|---|
| board | 1 | 1,040.4 µs / 960 qps | 1,020.8 µs / 978 qps | 187.4 µs / 2,403 qps | 182.7 µs / 2,586 qps | 182.6 µs / 3,039 qps | 180.1 µs / 3,053 qps | 2.50x / 2.64x |
| board | 2 | 1,043.2 µs / 954 qps | 1,018.5 µs / 979 qps | 189.5 µs / 2,374 qps | 179.7 µs / 2,624 qps | 203.3 µs / 2,990 qps | 183.3 µs / 3,143 qps | 2.49x / 2.68x |
| board | 3 | 1,083.3 µs / 921 qps | 1,039.9 µs / 961 qps | 178.3 µs / 3,283 qps | 187.2 µs / 2,467 qps | 177.3 µs / 3,827 qps | 178.7 µs / 3,758 qps | 3.56x / 2.57x |
| tape200 | 1 | 119.0 µs / 7,868 qps | 120.4 µs / 7,669 qps | 106.5 µs / 8,696 qps | 103.4 µs / 9,285 qps | 106.1 µs / 8,905 qps | 106.9 µs / 8,606 qps | 1.11x / 1.21x |
| tape200 | 2 | 120.4 µs / 7,800 qps | 117.1 µs / 8,143 qps | 104.8 µs / 9,066 qps | 102.3 µs / 9,443 qps | 133.8 µs / 7,994 qps | 104.9 µs / 8,889 qps | 1.16x / 1.16x |
| tape200 | 3 | 118.3 µs / 8,084 qps | 118.8 µs / 7,937 qps | 103.8 µs / 8,945 qps | 104.3 µs / 8,913 qps | 104.3 µs / 9,225 qps | 104.0 µs / 8,953 qps | 1.11x / 1.12x |
| tape1k | 1 | 187.0 µs / 5,128 qps | 187.8 µs / 5,084 qps | 111.0 µs / 7,391 qps | 104.0 µs / 7,968 qps | 106.7 µs / 8,197 qps | 110.0 µs / 7,776 qps | 1.44x / 1.57x |
| tape1k | 2 | 188.8 µs / 5,112 qps | 184.2 µs / 5,269 qps | 103.9 µs / 8,666 qps | 102.3 µs / 8,873 qps | 134.4 µs / 7,722 qps | 104.4 µs / 8,532 qps | 1.69x / 1.68x |
| tape1k | 3 | 189.7 µs / 5,160 qps | 183.9 µs / 5,208 qps | 104.0 µs / 8,569 qps | 102.8 µs / 8,643 qps | 103.8 µs / 9,141 qps | 104.2 µs / 8,803 qps | 1.66x / 1.66x |
| tape10k | 1 | 974.1 µs / 1,024 qps | 938.1 µs / 1,051 qps | 112.9 µs / 2,738 qps | 109.6 µs / 2,843 qps | 111.6 µs / 3,636 qps | 118.4 µs / 3,501 qps | 2.67x / 2.71x |
| tape10k | 2 | 967.3 µs / 1,030 qps | 940.6 µs / 1,055 qps | 108.9 µs / 4,045 qps | 105.1 µs / 4,394 qps | 135.3 µs / 4,299 qps | 106.6 µs / 4,562 qps | 3.93x / 4.16x |
| tape10k | 3 | 971.5 µs / 1,015 qps | 936.7 µs / 1,058 qps | 108.6 µs / 3,865 qps | 110.2 µs / 4,032 qps | 105.8 µs / 4,274 qps | 106.3 µs / 3,550 qps | 3.81x / 3.81x |
| pk | 1 | 71.8 µs / 12,315 qps | 70.4 µs / 12,077 qps | 69.7 µs / 12,870 qps | 68.3 µs / 13,870 qps | 69.7 µs / 13,316 qps | 73.8 µs / 12,107 qps | 1.05x / 1.15x |
| pk | 2 | 71.5 µs / 12,626 qps | 69.3 µs / 13,228 qps | 69.6 µs / 13,605 qps | 68.3 µs / 13,986 qps | 97.0 µs / 11,223 qps | 69.6 µs / 12,920 qps | 1.08x / 1.06x |
| pk | 3 | 69.0 µs / 13,210 qps | 70.2 µs / 12,953 qps | 69.4 µs / 12,937 qps | 71.1 µs / 12,516 qps | 69.8 µs / 13,459 qps | 69.0 µs / 13,405 qps | 0.98x / 0.97x |
| open | 1 | 1,328.2 µs / 648 qps | 1,311.2 µs / 704 qps | 1,353.1 µs / 691 qps | 1,313.5 µs / 648 qps | 1,335.2 µs / 616 qps | 1,338.1 µs / 624 qps | 1.07x / 0.92x |
| open | 2 | 1,277.9 µs / 711 qps | 1,335.1 µs / 636 qps | 1,296.2 µs / 703 qps | 1,347.2 µs / 629 qps | 1,296.7 µs / 652 qps | 1,308.8 µs / 714 qps | 0.99x / 0.99x |
| open | 3 | 1,407.0 µs / 674 qps | 1,327.7 µs / 669 qps | 1,425.9 µs / 603 qps | 1,341.9 µs / 663 qps | 1,416.3 µs / 566 qps | 1,328.8 µs / 678 qps | 0.90x / 0.99x |
| close | 1 | 897.6 µs / 596 qps | 943.3 µs / 590 qps | 983.3 µs / 577 qps | 995.2 µs / 583 qps | 943.8 µs / 598 qps | 976.6 µs / 596 qps | 0.97x / 0.99x |
| close | 2 | 925.8 µs / 601 qps | 925.5 µs / 609 qps | 1,024.6 µs / 568 qps | 979.9 µs / 584 qps | 957.6 µs / 538 qps | 996.2 µs / 531 qps | 0.95x / 0.96x |
| close | 3 | 985.4 µs / 589 qps | 947.0 µs / 588 qps | 1,062.6 µs / 559 qps | 988.7 µs / 583 qps | 1,044.0 µs / 576 qps | 1,004.3 µs / 580 qps | 0.95x / 0.99x |

(the last column is the ratio of mean latencies, off over on; qps derived as 1,000,000 / mean us, one serial connection)

Busy-time TPS over all timed operations of a day (the driver's own
figure; ops 4,080 per arm per day):

| day | off A | on A | declared A | off B | on B | declared B |
|---|---|---|---|---|---|---|
| 1 | 1,191 tps | 2,504 tps | 2,876 tps | 1,221 tps | 2,602 tps | 2,871 tps |
| 2 | 1,198 tps | 2,585 tps | 2,900 tps | 1,213 tps | 2,701 tps | 3,094 tps |
| 3 | 1,161 tps | 2,995 tps | 3,215 tps | 1,202 tps | 2,611 tps | 3,311 tps |

**What is resolved and what is not.** The pk control (`pk`, which touches no
Cabin) reads 0.97x to 1.15x off over on in the mean across both runs, and
its p50 is 68.3 to 73.8 us in every arm and day except one 97.0 us cell
(`declared`, day 2, run A: the host's slow mode, and not repeated in run B).
**So a ratio below about 1.15x is not a finding.** That puts `tape200`
(1.11x to 1.21x) at the floor, `tape1k` (1.44x to 1.69x), `tape10k`
(2.67x to 4.16x) and `board` (2.49x to 3.56x) clear of it, and `open` and
`close` (0.90x to 1.07x, and 0.95x to 0.99x) as *not measurably different
between arms*.

## 4. Percentiles

Every phase, in the driver's own order, microseconds. `d<day>-<group>[<arm>]`;
`max` and `err` as the driver reports them.

Run A (server defaults):

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | err |
|---|---|---|---|---|---|---|---|---|
| load[off] | 31,200 ops | 40.7 µs | 49.8 µs | 50.6 µs | 61.8 µs | 70.7 µs | 750.7 µs | 0 errors |
| load[on] | 31,200 ops | 40.8 µs | 47.2 µs | 50.3 µs | 70.0 µs | 78.0 µs | 159.3 µs | 0 errors |
| load[declared] | 31,200 ops | 40.4 µs | 49.7 µs | 51.0 µs | 67.2 µs | 74.8 µs | 38,842.8 µs | 0 errors |
| d1-open[off] | 240 ops | 1,094.9 µs | 1,247.2 µs | 1,328.2 µs | 2,529.9 µs | 6,035.6 µs | 11,831.9 µs | 0 errors |
| d1-open[on] | 240 ops | 1,063.7 µs | 1,285.9 µs | 1,353.1 µs | 1,653.6 µs | 4,560.2 µs | 8,426.4 µs | 0 errors |
| d1-open[declared] | 240 ops | 1,128.0 µs | 1,261.8 µs | 1,335.2 µs | 2,737.7 µs | 9,220.7 µs | 10,052.6 µs | 0 errors |
| d1-board[off] | 2,400 ops | 949.0 µs | 1,019.7 µs | 1,040.4 µs | 1,104.5 µs | 1,138.7 µs | 1,432.0 µs | 0 errors |
| d1-tape200[off] | 396 ops | 113.0 µs | 115.7 µs | 119.0 µs | 158.2 µs | 181.6 µs | 205.8 µs | 0 errors |
| d1-tape1k[off] | 396 ops | 178.8 µs | 183.6 µs | 187.0 µs | 224.4 µs | 242.2 µs | 270.2 µs | 0 errors |
| d1-tape10k[off] | 396 ops | 927.0 µs | 955.0 µs | 974.1 µs | 1,037.6 µs | 1,089.1 µs | 1,117.1 µs | 0 errors |
| d1-pk[off] | 240 ops | 66.1 µs | 68.3 µs | 71.8 µs | 107.3 µs | 131.9 µs | 147.8 µs | 0 errors |
| d1-board[on] | 2,400 ops | 103.6 µs | 169.7 µs | 187.4 µs | 1,130.3 µs | 1,175.0 µs | 1,363.1 µs | 0 errors |
| d1-tape200[on] | 396 ops | 99.2 µs | 101.5 µs | 106.5 µs | 155.2 µs | 178.1 µs | 198.4 µs | 0 errors |
| d1-tape1k[on] | 396 ops | 98.7 µs | 101.8 µs | 111.0 µs | 226.0 µs | 258.1 µs | 275.0 µs | 0 errors |
| d1-tape10k[on] | 396 ops | 99.0 µs | 103.0 µs | 112.9 µs | 1,107.9 µs | 1,166.2 µs | 1,183.0 µs | 0 errors |
| d1-pk[on] | 240 ops | 65.0 µs | 68.0 µs | 69.7 µs | 104.1 µs | 122.9 µs | 131.6 µs | 0 errors |
| d1-board[declared] | 2,400 ops | 102.6 µs | 168.6 µs | 182.6 µs | 1,060.5 µs | 1,102.2 µs | 1,270.4 µs | 0 errors |
| d1-tape200[declared] | 396 ops | 99.5 µs | 102.5 µs | 106.1 µs | 139.2 µs | 160.8 µs | 187.9 µs | 0 errors |
| d1-tape1k[declared] | 396 ops | 99.6 µs | 102.6 µs | 106.7 µs | 202.1 µs | 232.3 µs | 267.8 µs | 0 errors |
| d1-tape10k[declared] | 396 ops | 100.1 µs | 103.3 µs | 111.6 µs | 1,084.9 µs | 1,193.6 µs | 1,538.3 µs | 0 errors |
| d1-pk[declared] | 240 ops | 66.3 µs | 68.5 µs | 69.7 µs | 98.4 µs | 123.0 µs | 124.0 µs | 0 errors |
| d1-close[off] | 12 ops | 841.5 µs | 860.7 µs | 897.6 µs | 4,189.2 µs | 4,189.2 µs | 4,189.2 µs | 0 errors |
| d1-close[on] | 12 ops | 921.6 µs | 929.9 µs | 983.3 µs | 4,184.9 µs | 4,184.9 µs | 4,184.9 µs | 0 errors |
| d1-close[declared] | 12 ops | 837.6 µs | 880.6 µs | 943.8 µs | 4,097.5 µs | 4,097.5 µs | 4,097.5 µs | 0 errors |
| d2-open[off] | 240 ops | 1,095.4 µs | 1,215.7 µs | 1,277.9 µs | 1,870.0 µs | 4,591.4 µs | 6,710.2 µs | 0 errors |
| d2-open[on] | 240 ops | 1,157.3 µs | 1,257.3 µs | 1,296.2 µs | 1,599.3 µs | 4,366.8 µs | 8,636.9 µs | 0 errors |
| d2-open[declared] | 240 ops | 1,067.0 µs | 1,216.8 µs | 1,296.7 µs | 3,010.9 µs | 6,760.5 µs | 10,156.4 µs | 0 errors |
| d2-board[off] | 2,400 ops | 949.5 µs | 1,020.8 µs | 1,043.2 µs | 1,097.1 µs | 1,435.4 µs | 1,496.3 µs | 0 errors |
| d2-tape200[off] | 396 ops | 112.8 µs | 115.9 µs | 120.4 µs | 157.9 µs | 186.1 µs | 239.2 µs | 0 errors |
| d2-tape1k[off] | 396 ops | 178.4 µs | 183.5 µs | 188.8 µs | 224.1 µs | 241.6 µs | 252.4 µs | 0 errors |
| d2-tape10k[off] | 396 ops | 922.8 µs | 956.2 µs | 967.3 µs | 1,004.5 µs | 1,067.0 µs | 1,112.3 µs | 0 errors |
| d2-pk[off] | 240 ops | 65.7 µs | 68.4 µs | 71.5 µs | 107.5 µs | 132.5 µs | 133.9 µs | 0 errors |
| d2-board[on] | 2,400 ops | 103.0 µs | 171.3 µs | 189.5 µs | 1,139.3 µs | 1,211.7 µs | 5,478.9 µs | 0 errors |
| d2-tape200[on] | 396 ops | 99.4 µs | 102.1 µs | 104.8 µs | 136.8 µs | 156.6 µs | 188.9 µs | 0 errors |
| d2-tape1k[on] | 396 ops | 99.5 µs | 101.5 µs | 103.9 µs | 202.9 µs | 219.4 µs | 275.4 µs | 0 errors |
| d2-tape10k[on] | 396 ops | 99.2 µs | 102.7 µs | 108.9 µs | 1,088.9 µs | 1,505.8 µs | 1,620.7 µs | 0 errors |
| d2-pk[on] | 240 ops | 65.5 µs | 68.1 µs | 69.6 µs | 91.6 µs | 108.9 µs | 114.7 µs | 0 errors |
| d2-board[declared] | 2,400 ops | 103.5 µs | 181.5 µs | 203.3 µs | 1,081.2 µs | 1,128.6 µs | 1,315.8 µs | 0 errors |
| d2-tape200[declared] | 396 ops | 100.6 µs | 104.6 µs | 133.8 µs | 153.6 µs | 178.0 µs | 189.4 µs | 0 errors |
| d2-tape1k[declared] | 396 ops | 100.3 µs | 105.1 µs | 134.4 µs | 194.6 µs | 249.5 µs | 263.0 µs | 0 errors |
| d2-tape10k[declared] | 396 ops | 100.5 µs | 105.2 µs | 135.3 µs | 1,045.2 µs | 1,126.6 µs | 1,279.2 µs | 0 errors |
| d2-pk[declared] | 240 ops | 67.5 µs | 70.4 µs | 97.0 µs | 118.6 µs | 129.5 µs | 142.4 µs | 0 errors |
| d2-close[off] | 12 ops | 847.0 µs | 886.7 µs | 925.8 µs | 3,979.1 µs | 3,979.1 µs | 3,979.1 µs | 0 errors |
| d2-close[on] | 12 ops | 915.3 µs | 971.8 µs | 1,024.6 µs | 4,253.3 µs | 4,253.3 µs | 4,253.3 µs | 0 errors |
| d2-close[declared] | 12 ops | 834.9 µs | 890.6 µs | 957.6 µs | 6,359.1 µs | 6,359.1 µs | 6,359.1 µs | 0 errors |
| d3-open[off] | 240 ops | 1,159.4 µs | 1,310.3 µs | 1,407.0 µs | 2,031.9 µs | 3,610.5 µs | 4,096.6 µs | 0 errors |
| d3-open[on] | 240 ops | 1,070.7 µs | 1,356.3 µs | 1,425.9 µs | 2,890.2 µs | 8,805.2 µs | 10,405.8 µs | 0 errors |
| d3-open[declared] | 240 ops | 1,128.0 µs | 1,327.0 µs | 1,416.3 µs | 3,716.1 µs | 7,491.5 µs | 9,687.2 µs | 0 errors |
| d3-board[off] | 2,400 ops | 967.4 µs | 1,061.2 µs | 1,083.3 µs | 1,170.4 µs | 1,198.5 µs | 1,505.1 µs | 0 errors |
| d3-tape200[off] | 396 ops | 112.3 µs | 115.5 µs | 118.3 µs | 152.0 µs | 170.8 µs | 185.6 µs | 0 errors |
| d3-tape1k[off] | 396 ops | 178.9 µs | 183.6 µs | 189.7 µs | 218.4 µs | 235.6 µs | 240.9 µs | 0 errors |
| d3-tape10k[off] | 396 ops | 933.8 µs | 962.1 µs | 971.5 µs | 1,049.7 µs | 1,093.1 µs | 1,137.6 µs | 0 errors |
| d3-pk[off] | 240 ops | 64.0 µs | 67.7 µs | 69.0 µs | 101.4 µs | 117.7 µs | 135.3 µs | 0 errors |
| d3-board[on] | 2,400 ops | 100.8 µs | 169.6 µs | 178.3 µs | 1,136.3 µs | 1,220.7 µs | 6,352.2 µs | 0 errors |
| d3-tape200[on] | 396 ops | 98.9 µs | 101.5 µs | 103.8 µs | 142.7 µs | 161.5 µs | 187.6 µs | 0 errors |
| d3-tape1k[on] | 396 ops | 98.5 µs | 101.3 µs | 104.0 µs | 194.5 µs | 247.9 µs | 261.1 µs | 0 errors |
| d3-tape10k[on] | 396 ops | 98.8 µs | 102.1 µs | 108.6 µs | 1,115.6 µs | 1,163.4 µs | 1,213.1 µs | 0 errors |
| d3-pk[on] | 240 ops | 62.3 µs | 67.9 µs | 69.4 µs | 102.5 µs | 120.5 µs | 126.7 µs | 0 errors |
| d3-board[declared] | 2,400 ops | 102.2 µs | 168.4 µs | 177.3 µs | 1,064.0 µs | 1,125.5 µs | 1,450.6 µs | 0 errors |
| d3-tape200[declared] | 396 ops | 100.0 µs | 102.5 µs | 104.3 µs | 130.3 µs | 148.1 µs | 168.3 µs | 0 errors |
| d3-tape1k[declared] | 396 ops | 100.0 µs | 102.3 µs | 103.8 µs | 125.3 µs | 210.6 µs | 221.4 µs | 0 errors |
| d3-tape10k[declared] | 396 ops | 100.4 µs | 102.7 µs | 105.8 µs | 1,026.1 µs | 1,100.9 µs | 1,115.5 µs | 0 errors |
| d3-pk[declared] | 240 ops | 67.4 µs | 68.6 µs | 69.8 µs | 93.6 µs | 114.3 µs | 118.7 µs | 0 errors |
| d3-close[off] | 12 ops | 870.3 µs | 938.0 µs | 985.4 µs | 4,015.7 µs | 4,015.7 µs | 4,015.7 µs | 0 errors |
| d3-close[on] | 12 ops | 1,002.9 µs | 1,021.0 µs | 1,062.6 µs | 4,121.5 µs | 4,121.5 µs | 4,121.5 µs | 0 errors |
| d3-close[declared] | 12 ops | 918.9 µs | 960.3 µs | 1,044.0 µs | 4,040.9 µs | 4,040.9 µs | 4,040.9 µs | 0 errors |

Run B (cooldown 2 half-lives):

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | err |
|---|---|---|---|---|---|---|---|---|
| load[off] | 31,200 ops | 41.1 µs | 49.8 µs | 50.6 µs | 71.1 µs | 84.3 µs | 1,012.7 µs | 0 errors |
| load[on] | 31,200 ops | 40.7 µs | 49.8 µs | 50.8 µs | 71.3 µs | 84.0 µs | 316.8 µs | 0 errors |
| load[declared] | 31,200 ops | 40.3 µs | 49.6 µs | 50.8 µs | 72.6 µs | 86.5 µs | 19,507.1 µs | 0 errors |
| d1-open[off] | 240 ops | 1,043.5 µs | 1,207.0 µs | 1,311.2 µs | 1,645.5 µs | 3,395.8 µs | 14,544.3 µs | 0 errors |
| d1-open[on] | 240 ops | 1,111.5 µs | 1,224.4 µs | 1,313.5 µs | 2,813.7 µs | 7,234.0 µs | 13,896.3 µs | 0 errors |
| d1-open[declared] | 240 ops | 1,154.3 µs | 1,278.7 µs | 1,338.1 µs | 3,116.6 µs | 7,054.8 µs | 12,357.9 µs | 0 errors |
| d1-board[off] | 2,400 ops | 931.3 µs | 999.0 µs | 1,020.8 µs | 1,074.4 µs | 1,333.6 µs | 1,493.6 µs | 0 errors |
| d1-tape200[off] | 396 ops | 111.4 µs | 114.7 µs | 120.4 µs | 165.0 µs | 178.5 µs | 198.6 µs | 0 errors |
| d1-tape1k[off] | 396 ops | 176.8 µs | 180.0 µs | 187.8 µs | 233.0 µs | 243.1 µs | 247.5 µs | 0 errors |
| d1-tape10k[off] | 396 ops | 901.1 µs | 926.2 µs | 938.1 µs | 1,007.3 µs | 1,186.4 µs | 1,481.0 µs | 0 errors |
| d1-pk[off] | 240 ops | 65.4 µs | 67.9 µs | 70.4 µs | 110.9 µs | 132.0 µs | 132.4 µs | 0 errors |
| d1-board[on] | 2,400 ops | 101.6 µs | 167.6 µs | 182.7 µs | 1,018.5 µs | 1,069.5 µs | 1,277.1 µs | 0 errors |
| d1-tape200[on] | 396 ops | 93.4 µs | 100.7 µs | 103.4 µs | 126.4 µs | 137.3 µs | 148.9 µs | 0 errors |
| d1-tape1k[on] | 396 ops | 98.0 µs | 100.5 µs | 104.0 µs | 196.4 µs | 214.5 µs | 217.2 µs | 0 errors |
| d1-tape10k[on] | 396 ops | 98.5 µs | 101.6 µs | 109.6 µs | 1,000.4 µs | 1,064.0 µs | 4,561.9 µs | 0 errors |
| d1-pk[on] | 240 ops | 64.0 µs | 67.2 µs | 68.3 µs | 90.5 µs | 100.8 µs | 117.5 µs | 0 errors |
| d1-board[declared] | 2,400 ops | 101.1 µs | 166.8 µs | 180.1 µs | 1,050.5 µs | 1,122.3 µs | 1,516.0 µs | 0 errors |
| d1-tape200[declared] | 396 ops | 98.4 µs | 101.4 µs | 106.9 µs | 151.1 µs | 165.2 µs | 183.7 µs | 0 errors |
| d1-tape1k[declared] | 396 ops | 98.7 µs | 101.4 µs | 110.0 µs | 225.6 µs | 242.9 µs | 257.6 µs | 0 errors |
| d1-tape10k[declared] | 396 ops | 98.7 µs | 102.7 µs | 118.4 µs | 1,101.8 µs | 1,347.7 µs | 1,406.1 µs | 0 errors |
| d1-pk[declared] | 240 ops | 65.8 µs | 67.9 µs | 73.8 µs | 113.2 µs | 129.0 µs | 185.7 µs | 0 errors |
| d1-close[off] | 12 ops | 838.0 µs | 849.3 µs | 943.3 µs | 4,249.6 µs | 4,249.6 µs | 4,249.6 µs | 0 errors |
| d1-close[on] | 12 ops | 897.4 µs | 931.3 µs | 995.2 µs | 4,086.9 µs | 4,086.9 µs | 4,086.9 µs | 0 errors |
| d1-close[declared] | 12 ops | 850.3 µs | 909.1 µs | 976.6 µs | 4,035.5 µs | 4,035.5 µs | 4,035.5 µs | 0 errors |
| d2-open[off] | 240 ops | 1,121.6 µs | 1,271.9 µs | 1,335.1 µs | 2,664.6 µs | 7,628.7 µs | 10,423.4 µs | 0 errors |
| d2-open[on] | 240 ops | 1,123.2 µs | 1,279.3 µs | 1,347.2 µs | 3,175.5 µs | 6,777.3 µs | 10,707.9 µs | 0 errors |
| d2-open[declared] | 240 ops | 1,089.4 µs | 1,235.7 µs | 1,308.8 µs | 1,646.6 µs | 3,701.1 µs | 10,419.2 µs | 0 errors |
| d2-board[off] | 2,400 ops | 933.0 µs | 1,000.9 µs | 1,018.5 µs | 1,088.1 µs | 1,135.8 µs | 1,463.4 µs | 0 errors |
| d2-tape200[off] | 396 ops | 111.5 µs | 114.6 µs | 117.1 µs | 149.9 µs | 170.9 µs | 177.9 µs | 0 errors |
| d2-tape1k[off] | 396 ops | 176.2 µs | 180.6 µs | 184.2 µs | 215.9 µs | 238.4 µs | 281.7 µs | 0 errors |
| d2-tape10k[off] | 396 ops | 909.1 µs | 928.1 µs | 940.6 µs | 996.6 µs | 1,051.1 µs | 1,073.1 µs | 0 errors |
| d2-pk[off] | 240 ops | 65.0 µs | 68.0 µs | 69.3 µs | 101.0 µs | 118.2 µs | 129.0 µs | 0 errors |
| d2-board[on] | 2,400 ops | 101.7 µs | 166.8 µs | 179.7 µs | 1,029.5 µs | 1,084.4 µs | 1,235.9 µs | 0 errors |
| d2-tape200[on] | 396 ops | 97.7 µs | 100.5 µs | 102.3 µs | 123.8 µs | 136.2 µs | 144.1 µs | 0 errors |
| d2-tape1k[on] | 396 ops | 97.4 µs | 100.2 µs | 102.3 µs | 191.2 µs | 206.0 µs | 224.3 µs | 0 errors |
| d2-tape10k[on] | 396 ops | 98.3 µs | 101.0 µs | 105.1 µs | 995.5 µs | 1,080.9 µs | 1,115.8 µs | 0 errors |
| d2-pk[on] | 240 ops | 64.0 µs | 67.3 µs | 68.3 µs | 86.5 µs | 100.6 µs | 112.1 µs | 0 errors |
| d2-board[declared] | 2,400 ops | 101.7 µs | 167.4 µs | 183.3 µs | 1,056.6 µs | 1,104.3 µs | 1,479.7 µs | 0 errors |
| d2-tape200[declared] | 396 ops | 98.0 µs | 101.3 µs | 104.9 µs | 143.0 µs | 159.8 µs | 171.6 µs | 0 errors |
| d2-tape1k[declared] | 396 ops | 99.0 µs | 101.0 µs | 104.4 µs | 197.5 µs | 233.9 µs | 264.4 µs | 0 errors |
| d2-tape10k[declared] | 396 ops | 99.1 µs | 101.7 µs | 106.6 µs | 1,030.8 µs | 1,117.0 µs | 1,136.6 µs | 0 errors |
| d2-pk[declared] | 240 ops | 61.6 µs | 67.7 µs | 69.6 µs | 103.1 µs | 124.3 µs | 131.1 µs | 0 errors |
| d2-close[off] | 12 ops | 820.1 µs | 857.0 µs | 925.5 µs | 3,869.2 µs | 3,869.2 µs | 3,869.2 µs | 0 errors |
| d2-close[on] | 12 ops | 871.3 µs | 927.3 µs | 979.9 µs | 4,058.3 µs | 4,058.3 µs | 4,058.3 µs | 0 errors |
| d2-close[declared] | 12 ops | 881.0 µs | 930.3 µs | 996.2 µs | 6,275.8 µs | 6,275.8 µs | 6,275.8 µs | 0 errors |
| d3-open[off] | 240 ops | 1,098.7 µs | 1,262.8 µs | 1,327.7 µs | 1,959.8 µs | 5,338.9 µs | 9,511.4 µs | 0 errors |
| d3-open[on] | 240 ops | 1,130.3 µs | 1,286.9 µs | 1,341.9 µs | 2,119.1 µs | 5,668.2 µs | 8,540.4 µs | 0 errors |
| d3-open[declared] | 240 ops | 1,121.0 µs | 1,276.1 µs | 1,328.8 µs | 2,330.4 µs | 3,530.4 µs | 8,331.3 µs | 0 errors |
| d3-board[off] | 2,400 ops | 956.2 µs | 1,024.1 µs | 1,039.9 µs | 1,088.5 µs | 1,183.6 µs | 1,665.7 µs | 0 errors |
| d3-tape200[off] | 396 ops | 111.8 µs | 115.1 µs | 118.8 µs | 155.7 µs | 172.7 µs | 193.1 µs | 0 errors |
| d3-tape1k[off] | 396 ops | 176.5 µs | 180.1 µs | 183.9 µs | 221.8 µs | 241.8 µs | 242.4 µs | 0 errors |
| d3-tape10k[off] | 396 ops | 906.7 µs | 927.6 µs | 936.7 µs | 994.3 µs | 1,066.2 µs | 1,092.3 µs | 0 errors |
| d3-pk[off] | 240 ops | 66.0 µs | 68.2 µs | 70.2 µs | 101.6 µs | 109.7 µs | 131.2 µs | 0 errors |
| d3-board[on] | 2,400 ops | 101.0 µs | 174.0 µs | 187.2 µs | 1,089.8 µs | 1,140.0 µs | 1,593.1 µs | 0 errors |
| d3-tape200[on] | 396 ops | 98.3 µs | 101.1 µs | 104.3 µs | 139.5 µs | 160.4 µs | 186.6 µs | 0 errors |
| d3-tape1k[on] | 396 ops | 98.6 µs | 100.6 µs | 102.8 µs | 189.0 µs | 240.2 µs | 256.6 µs | 0 errors |
| d3-tape10k[on] | 396 ops | 98.0 µs | 101.4 µs | 110.2 µs | 1,037.0 µs | 1,114.9 µs | 1,145.5 µs | 0 errors |
| d3-pk[on] | 240 ops | 65.8 µs | 67.9 µs | 71.1 µs | 106.9 µs | 125.3 µs | 134.1 µs | 0 errors |
| d3-board[declared] | 2,400 ops | 101.1 µs | 169.5 µs | 178.7 µs | 1,062.4 µs | 1,125.9 µs | 1,267.1 µs | 0 errors |
| d3-tape200[declared] | 396 ops | 98.7 µs | 101.3 µs | 104.0 µs | 139.5 µs | 158.1 µs | 167.7 µs | 0 errors |
| d3-tape1k[declared] | 396 ops | 98.9 µs | 101.2 µs | 104.2 µs | 150.4 µs | 219.8 µs | 238.5 µs | 0 errors |
| d3-tape10k[declared] | 396 ops | 98.5 µs | 101.8 µs | 106.3 µs | 1,034.3 µs | 1,112.7 µs | 18,436.7 µs | 0 errors |
| d3-pk[declared] | 240 ops | 65.5 µs | 67.7 µs | 69.0 µs | 98.5 µs | 115.9 µs | 124.4 µs | 0 errors |
| d3-close[off] | 12 ops | 873.6 µs | 891.2 µs | 947.0 µs | 4,079.0 µs | 4,079.0 µs | 4,079.0 µs | 0 errors |
| d3-close[on] | 12 ops | 903.3 µs | 917.0 µs | 988.7 µs | 4,045.4 µs | 4,045.4 µs | 4,045.4 µs | 0 errors |
| d3-close[declared] | 12 ops | 912.9 µs | 926.0 µs | 1,004.3 µs | 4,028.6 µs | 4,028.6 µs | 4,028.6 µs | 0 errors |

## 5. Where the time goes

A probe is one round trip on one connection, so the decomposition is fixed
cost plus rows walked:

| Component | Estimate | How derived |
|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | about 70 us | `pk` p50, 68.3 to 73.8 us, in every arm |
| Rows walked by an unserved equality | about **87 ns per row** plus about 102 us | `off` tape p50 at 200 rows 119.0 us and at 10,000 rows 974.1 us (day 1, run A): (974.1 - 119.0) / 9,800 = 87 ns per row; scenario3's index-less equality gives 70 ns per row on a different shape |
| A Cabin-served equality | about 104 to 113 us at any size | `on` and `declared` `tape200` / `tape1k` / `tape10k` p50 after the first day-1 observation: 106.5, 111.0, 112.9 us; flat in n |
| Unobserved values and coverage misses | both served arms keep a walked tail: `board` p95 1,130 us `on`, 1,061 us `declared` (day 1, run A; 1,019 and 1,051 us in run B). What separates them is the share walked, visible only in the mean: 416 us `on` against 329 us `declared` (day 1, run A) | a Cabin is authoritative only for observed values (`docs/spec/cabin.md` C1), so a probe of a value not yet observed walks in either arm, and the uniform draw (20 % of each board draw, `BOARD_HOT_SHARE = 0.8` in the driver) keeps meeting new ones; the controller also has to create and extend its Cabin first |
| Write-side upkeep | not resolvable: `open` p50 1,328.2 / 1,353.1 / 1,335.2 us (`off` / `on` / `declared`, day 1, run A) | inside the floor |
| Durability / commit | not applicable to the probes; `open` and `load` are the only writers and are not decomposed | |

Server CPU seconds by arm (the driver's `/proc` reading), run A / run B:

| arm | board | tape | open | close |
|---|---|---|---|---|
| off | 6.71 s / 6.43 s | 1.29 s / 1.31 s | 0.06 s / 0.07 s | 0.03 s / 0.03 s |
| on | 1.87 s / 1.96 s | 0.35 s / 0.28 s | 0.06 s / 0.07 s | 0.04 s / 0.04 s |
| declared | 1.33 s / 1.29 s | 0.32 s / 0.33 s | 0.06 s / 0.07 s | 0.03 s / 0.04 s |

`off` burns 2.2 s of server CPU on a day's boards, 0.75 s under the
controller and 0.49 s declared: the CPU ratio (2.9x and 4.5x) is larger
than the latency ratio (2.5x and 3.2x) because the walk is CPU and the
served probe is mostly round trip.

## 6. The controller's lifecycle

Snapshots of `SHOW CABIN_OPTIMIZER` on the `on` arm at chosen points, run A
then run B:

run A:
| t (s) | day | tag | managed | creates | extends | drops | entries |
|---|---|---|---|---|---|---|---|
| 14.6 s | 1 | trading-block-3 | 4 Cabins | 4 creates | 32 extends | 0 drops | board_a_a:ACTIVE/3p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/1p; tape_10k_a:ACTIVE/1p |
| 37.1 s | 1 | trading-block-9 | 4 Cabins | 4 creates | 106 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 48.4 s | 1 | trading-block-12 | 4 Cabins | 4 creates | 106 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 54.8 s | 1 | overnight | 4 Cabins | 4 creates | 106 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 105.8 s | 2 | trading-block-3 | 5 Cabins | 5 creates | 137 extends | 0 drops | board_a_a:ACTIVE/9p; board_b_a:ACTIVE/3p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 128.3 s | 2 | trading-block-9 | 5 Cabins | 5 creates | 144 extends | 0 drops | board_a_a:DECAYING/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 139.5 s | 2 | trading-block-12 | 5 Cabins | 5 creates | 144 extends | 0 drops | board_a_a:DECAYING/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 145.9 s | 2 | overnight | 5 Cabins | 5 creates | 144 extends | 0 drops | board_a_a:DECAYING/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 197.0 s | 3 | trading-block-3 | 5 Cabins | 5 creates | 166 extends | 0 drops | board_a_a:ACTIVE/18p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/5p |
| 219.5 s | 3 | trading-block-9 | 5 Cabins | 5 creates | 173 extends | 0 drops | board_a_a:ACTIVE/18p; board_b_a:DECAYING/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 230.8 s | 3 | trading-block-12 | 5 Cabins | 5 creates | 173 extends | 0 drops | board_a_a:ACTIVE/18p; board_b_a:DECAYING/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 237.2 s | 3 | overnight | 5 Cabins | 5 creates | 173 extends | 0 drops | board_a_a:ACTIVE/18p; board_b_a:DECAYING/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |

final header: `cabin_optimizer=on managed=5 pages_committed=32 page_budget=1024 ticks=547 creates=5 extends=173 heals=0 drops=0 deferred=0 failures=0`

busy-time TPS is in the driver's stdout; byte-identical verify per day: PASS, PASS, PASS

run B:
| t (s) | day | tag | managed | creates | extends | drops | entries |
|---|---|---|---|---|---|---|---|
| 14.8 s | 1 | trading-block-3 | 4 Cabins | 4 creates | 31 extends | 0 drops | board_a_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/1p; tape_10k_a:ACTIVE/1p |
| 37.3 s | 1 | trading-block-9 | 4 Cabins | 4 creates | 103 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 48.5 s | 1 | trading-block-12 | 4 Cabins | 4 creates | 103 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 55.0 s | 1 | overnight | 4 Cabins | 4 creates | 103 extends | 0 drops | board_a_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 106.0 s | 2 | trading-block-3 | 5 Cabins | 5 creates | 133 extends | 0 drops | board_a_a:ACTIVE/9p; board_b_a:ACTIVE/3p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 128.5 s | 2 | trading-block-9 | 5 Cabins | 5 creates | 140 extends | 0 drops | board_a_a:DECAYING/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 139.7 s | 2 | trading-block-12 | 4 Cabins | 5 creates | 140 extends | 1 drop | board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 146.2 s | 2 | overnight | 4 Cabins | 5 creates | 140 extends | 1 drop | board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 197.2 s | 3 | trading-block-3 | 5 Cabins | 6 creates | 161 extends | 1 drop | board_a_a:ACTIVE/3p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/5p |
| 219.7 s | 3 | trading-block-9 | 5 Cabins | 6 creates | 184 extends | 1 drop | board_a_a:ACTIVE/6p; board_b_a:DECAYING/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 230.9 s | 3 | trading-block-12 | 4 Cabins | 6 creates | 192 extends | 2 drops | board_a_a:ACTIVE/14p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 237.4 s | 3 | overnight | 4 Cabins | 6 creates | 192 extends | 2 drops | board_a_a:ACTIVE/14p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |

final header: `cabin_optimizer=on managed=4 pages_committed=24 page_budget=1024 ticks=547 creates=6 extends=192 heals=0 drops=2 deferred=0 failures=0`

busy-time TPS is in the driver's stdout; byte-identical verify per day: PASS, PASS, PASS

Run A, defaults. Four Cabins are **created** in the first 14.6 s of day 1
(`board_a`, `tape_200`, `tape_1k`, `tape_10k`: 4 creates, 32 extends by
block 3), grown by 106 extends by the end of the day, and a fifth
(`board_b`) on day 2's first block. `board_a` goes **DECAYING** on day 2
(by block 9, at t = 128.3 s; day 2 opened at 96.8 s) and **returns to ACTIVE** on
day 3 with its pages doubled (9 to 18) when its hot set comes back; then
`board_b` goes DECAYING. At the end: `creates=5 extends=173 heals=0
drops=0 deferred=0 failures=0`. **No DROP occurs in a 3-day run at the
defaults.** The reason is arithmetic: the DECAYING dwell before a DROP is
`cabin_optimizer_cooldown_half_lives`, default 128 (`docs/spec/physical-
optimizer.md` II.6), which at `decay_half_life = 5` is 640 s; the driver's
whole run is 279 s and its nights are 45 s. **The driver's docstring
assumes a 2-half-life cooldown** ("DROP after a 2-half-life cooldown",
"45 s is already 9 half-lives"), which is not the engine's default and is
what run B sets.

Run B, cooldown 2 half-lives. Same creates on day 1, `board_a` first seen
DECAYING at 128.5 s (ACTIVE at 117.2 s) and **DROPPED** by 139.7 s
(`drops=1`); the snapshots bound its DECAYING dwell to 0 to 22.5 s, which
contains the configured 10 s (two half-lives) but does not measure it. Its
Cabin is gone while `board_b` is hot; on day 3 `board_a` is
**re-nominated from scratch** (`creates=6`, a new cabin id, 3 pages at
197 s, 14 by the end) and `board_b` is dropped at day 3's last block
(`drops=2`). Final: `creates=6 extends=192 heals=0 drops=2 failures=0`,
`pages_committed=24`. The cost of re-nomination shows in the served rate of
the day it happens: `d3-board[on]` runs 3,283 qps in run A (the Cabin
survived the night, DECAYING then ACTIVE) and 2,467 qps in run B (rebuilt),
against a run-to-run spread of 8 to 11 % on days 1 and 2 (2,403 / 2,586
and 2,374 / 2,624 qps): **-25 %, outside that**. Single draw per run.

## 7. Correctness

The driver compares an identical verification set (hot probes, a zero-row
probe, `COUNT` / `SUM`, 18 statements) across the three arms byte for byte
each day: **PASS on all three days in both runs**, `client_errors 0`,
`slot_overruns 0`. A Cabin, however created, dropped or re-nominated,
changed no reply.

## 8. Noise floor

Inside the run: the `pk` control, 0.97x to 1.15x. Across the two runs: the
`off` arm, which does not depend on any run difference, has `board` p50
1,040.4 / 1,043.2 / 1,083.3 us in A and 1,020.8 / 1,018.5 / 1,039.9 us in
B (day by day up to 4.2 %), and `tape10k` p50 974.1 / 967.3 / 971.5 against
938.1 / 940.6 / 936.7 us (up to 3.8 %): **the `off` walk is reproducible to about 4 %**;
the `on` and `declared` arms are noisier (`board[on]` day 3 3,283 vs
2,467 qps, above). One declared cell at 97.0 us on the pk control is the
slow mode. `recovery_checkpoint_us` was not sampled; the loads (`load[..]`
p50 50.3 to 51.0 us per row in batches of 500, run A) agree across arms, so no
device stall is indicated.

## 9. What the run says about the engine

1. **A served equality costs one round trip; a walked one costs the
   relation.** About 110 us at any size against 119 us to 974 us on 200 to
   10,000 rows; the controller's own worth therefore scales with the row
   count, and at 200 rows is at the floor.
2. **The controller gets most of the declared ceiling without being told**:
   on the board, 2.4 to 3.3 kqps against 3.0 to 3.8 kqps declared. The gap
   is in how many probes walk, not how slow they are: p50 (187 against
   183 us) and p95 (1,130 against 1,061 us) barely differ, the mean (416
   against 329 us, day 1, run A) does.
3. **Nothing in the default configuration ever retires a Cabin in a
   working week the driver models.** With a 128-half-life cooldown DECAYING
   is reversible and DROP is out of reach of any run shorter than 128
   half-lives. This is a fact about the default, not a defect; the
   driver's stated intent (DROP and re-nomination) needs
   `cabin_optimizer_cooldown_half_lives` lowered, and
   `docs/spec/physical-optimizer.md` §II.4 states the price: a cooldown
   shorter than the workload's longest quiet period retires live Cabins.
   Run B's 2 half-lives against a 9-half-life night is that case, and its
   day-3 board (-25 %) is what it cost here.
4. **DROP and re-nomination work end to end**: `board_a` dropped within
   one snapshot interval of first being seen DECAYING, rebuilt on day 3,
   answers byte-identical throughout.
5. **The write side shows no cost at this resolution**: `open` is inside
   the floor in all arms, so the per-insert directory upkeep of a Cabin
   (or of 5 Cabins on the declared arm) is not visible over 240 inserts
   per day. Not resolved does not mean zero.

Archive: `bench/v3.0.0/archive/scenario4-v2.7.0-531-g9a0525d/`: both runs'
JSON (with the full evidence and plan record), driver stdout, prechecks,
server logs, the six configs, run scripts, the summariser and the timeline.
