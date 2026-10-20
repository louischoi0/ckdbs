# Scenario 4 (cabin optimizer over business days) at `v3.3.0-38-ga40882af`

**One line.** Three business days on three servers (a Cabin controller `off`, `on`, and a Cabin `declared` up front), run twice, on the morning's binary with the new client decoder. The controller's lifecycle is the morning's, snapshot for snapshot
(four Cabins in the first block, a fifth on day 2, `board_a` DECAYING on day 2 and ACTIVE again on day 3 with its pages doubled, two DROPs and one re-nomination under `cabin_optimizer_cooldown_half_lives = 2`), the 18-statement
byte comparison across arms passed on all six days, and the engine's cost did not move: the unserved equality walk costs 26.1 ns per row (morning 26.5 ns) and the server CPU an `off` run spends on its boards is 2.20 s (2.22 s).
What moved is the client's cost of the replies: every group that returns rows is faster, by +10 % to +22 % on `board`, +15 % to +18 % on `tape200`, +14 % to +17 % on `tape1k`, +5 % to +11 % on `tape10k` and +17 % to +19 % on `close`
(per arm, medians over three days and two runs, now over the morning), and the `pk` control (one row) and the durable `open` did not move (+0 % to +1 % and +1 % to +3 %). The interleaved A/B (section 8) agrees arm by arm: `board` 1.07x / 1.21x / 1.17x
(`off` / `on` / `declared`). Because a Cabin-served probe is almost all fixed cost and round trip, the Cabin's multiple over the walk grows (1.90x on `board` day 1 against 1.79x in the morning).
Against PostgreSQL 18.6, **reused from the morning**, day 1 with no index: `board` 1.30x walking and 2.49x with the controller; `close` is 0.67x (from 0.57x).

Driver: `tools/scenario4_cabinopt_days.py`, unmodified, two complete runs: **run A** at the server defaults (`s4`) and **run B** with `cabin_optimizer_cooldown_half_lives = 2` (`cd2-s4`), both with `decay_half_life = 5` and
`cabin_optimizer_snapshot_interval_ms = 500`. Measured by `ck-tester`. Comparator (this engine's own last run, same binary, old client): `bench/v3.3.0/results-scenario4-cabinopt-days-v3.3.0-33-g9682b725.md`. PostgreSQL 18.6: that file's twin cells
(`tools/pg_scenario4_cabinopt_days.py`, three runs), **not re-run**: budget lifted by the operator for this measurement; PostgreSQL reused from the same-day run at v3.3.0-33-g9682b725.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; run A 07:16:16 to 07:21:05 UTC, run B 07:21:09 to 07:26:00 UTC; A/B pairs (six runs of the driver, interleaved with the scenario 1 and 3 A/B cells) 07:31:11 to 08:07:27 UTC |
| Worktree / branch | `kwp-decoder-fast-path` / `kwp-decoder-fast-path` (the main checkout is not used) |
| HEAD | `a40882af`, `git describe --tags` = `v3.3.0-38-ga40882af` ("kwp: decode a row batch in one offset loop and resolve each column's renderer once"); it changes `tools/kwp.py` and `tools/ckdbs_cli.py` and nothing else; the engine source is that of `9682b725` |
| Engine measured | the morning's binary copy, byte for byte: `/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/kds_server` was copied from `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/kds_server`, **sha256 `7532bd14afb2acd4f9e48cac00732f899cb8e59b26fa1d9b982047fc89272f2a`** (checked equal before the first cell and after the last), source binary mtime 2026-10-20 03:36:57.38 UTC; nothing was rebuilt |
| Client | this worktree's `tools/` (`kwp.py`, `ckdbs_cli.py` as of `a40882af`) drove every cell of the matrix. The A/B arm `old` ran `a40882af^`'s (`f829964c`) `tools/` from a separate directory, `bench-runs/v3.3.0-38-ga40882af/oldtools/tools/`, put first on `PYTHONPATH`; the tree was not modified |
| Comparator | this engine's own morning run, `v3.3.0-33-g9682b725` (`bench/v3.3.0/results-scenario*-v3.3.0-33-g9682b725.md`): **same binary, old client**, 3 h earlier. PostgreSQL 18.6 is **reused** from that run; it was not re-run |
| Time budget | **Budget lifted by the operator for this measurement; PostgreSQL reused from the same-day run at v3.3.0-33-g9682b725.** Every KDS cell of the morning's plan ran with its repeat counts |
| Server config | the common config plus `decay_half_life = 5`, `cabin_optimizer_snapshot_interval_ms = 500` on all three servers, and in run B `cabin_optimizer_cooldown_half_lives = 2`; `cores = 1`, `durability = group`; ports 15651 to 15653 |
| Driver | **unmodified** `tools/scenario4_cabinopt_days.py`, `--port-off/-on/-declared` with the three servers' PIDs for the CPU readings, `--suffix a`; exit 0 in every run, `client_errors` 0 and `slot_overruns` 0 over 8 runs; the driver's byte-for-byte comparison of 18 statements per day across the three arms passed in 8 of 8 runs (3 days each) |
| Unchanged from the morning | Everything else (build type, host, device, server config, quiet-host gate, monitor, PostgreSQL setup) is unchanged from the morning and is stamped once in `summary-scenarios-v3.3.0-38-ga40882af.md`, section Stamp. |

## 2. What was run

Two complete runs of the driver, three days of three arms each, plus six more runs of the first for the A/B. Per day: an open (240 inserts into the day's board), a paced 45 s trading session (2,400 hot-board probes, 396 probes on each of three tape sizes,
240 pk controls, per arm, interleaved in 12 blocks), a close (`COUNT` / `SUM` / `GROUP BY` full scans, three rounds) and a 45 s idle night. The equal work per arm is fixed by the driver's counts, not by time.

    python3 tools/scenario4_cabinopt_days.py --port-off 15651 --port-on 15652 --port-declared 15653 \
        --pid-off <p> --pid-on <p> --pid-declared <p> --suffix a --json <run>.json

**Row-set axis.** The tape relations are 200, 1,000 and 10,000 rows (`tape_200`, `tape_1k`, `tape_10k`); the boards are 10,000 rows each (`board_a`, `board_b`). Every phase is a row of the matrix, the size is
a row label. The probes are serial on one connection, so qps is derived as 1,000,000 / mean µs (the table cells give the p50 in µs beside it). The hot set rotates: day 1 hot on `board_a`, day 2 on `board_b`,
day 3 on `board_a` with a different symbol set; the tapes are hot every day with disjoint values.

Cut: nothing. PostgreSQL's twin was not re-run (it runs day 1 only, unpaced, no index, by its own design).

## 3. Throughput and latency by arm

p50 and derived qps (from the mean) per group, day and arm for **run A**; the last two columns are the ratio of mean latencies, off over on and off over declared, for run A and run B:

| group | day | off A (p50 / qps) | on A (p50 / qps) | declared A (p50 / qps) | on over off, mean (A / B) | declared over off, mean (A / B) |
|---|---|---|---|---|---|---|
| board | day 1 | 386.6 µs / 2,562 qps | 149.5 µs / 4,876 qps | 148.9 µs / 4,002 qps | 1.90x / 1.93x | 1.56x / 1.62x |
| board | day 2 | 382.4 µs / 2,589 qps | 133.4 µs / 5,020 qps | 128.8 µs / 4,413 qps | 1.94x / 2.00x | 1.70x / 1.69x |
| board | day 3 | 393.0 µs / 2,530 qps | 131.0 µs / 5,790 qps | 128.8 µs / 5,126 qps | 2.29x / 1.99x | 2.03x / 2.03x |
| tape200 | day 1 | 89.7 µs / 10,101 qps | 90.3 µs / 10,142 qps | 91.7 µs / 9,921 qps | 1.00x / 0.99x | 0.98x / 1.04x |
| tape200 | day 2 | 87.5 µs / 10,460 qps | 85.6 µs / 10,846 qps | 84.0 µs / 11,390 qps | 1.04x / 1.04x | 1.09x / 1.05x |
| tape200 | day 3 | 87.3 µs / 10,460 qps | 86.0 µs / 10,593 qps | 85.0 µs / 10,953 qps | 1.01x / 1.06x | 1.05x / 1.06x |
| tape1k | day 1 | 110.3 µs / 8,467 qps | 97.5 µs / 9,728 qps | 91.9 µs / 9,328 qps | 1.15x / 1.17x | 1.10x / 1.18x |
| tape1k | day 2 | 108.0 µs / 8,688 qps | 84.3 µs / 10,684 qps | 83.9 µs / 11,013 qps | 1.23x / 1.30x | 1.27x / 1.28x |
| tape1k | day 3 | 110.5 µs / 8,418 qps | 86.2 µs / 10,460 qps | 85.0 µs / 10,858 qps | 1.24x / 1.26x | 1.29x / 1.25x |
| tape10k | day 1 | 345.5 µs / 2,837 qps | 99.7 µs / 5,800 qps | 100.9 µs / 4,664 qps | 2.04x / 2.11x | 1.64x / 1.69x |
| tape10k | day 2 | 343.5 µs / 2,853 qps | 86.2 µs / 6,940 qps | 85.1 µs / 6,068 qps | 2.43x / 2.68x | 2.13x / 2.08x |
| tape10k | day 3 | 347.7 µs / 2,828 qps | 90.8 µs / 7,097 qps | 88.7 µs / 5,470 qps | 2.51x / 2.61x | 1.93x / 1.91x |
| pk | day 1 | 65.6 µs / 13,280 qps | 65.5 µs / 13,072 qps | 69.3 µs / 12,626 qps | 0.98x / 1.07x | 0.95x / 1.06x |
| pk | day 2 | 64.2 µs / 13,717 qps | 64.8 µs / 14,006 qps | 64.6 µs / 14,815 qps | 1.02x / 1.06x | 1.08x / 1.01x |
| pk | day 3 | 65.7 µs / 13,280 qps | 65.8 µs / 13,405 qps | 64.4 µs / 14,085 qps | 1.01x / 1.09x | 1.06x / 1.06x |
| open | day 1 | 1,217.1 µs / 739 qps | 1,164.4 µs / 848 qps | 1,200.0 µs / 780 qps | 1.15x / 1.00x | 1.06x / 1.01x |
| open | day 2 | 1,159.0 µs / 845 qps | 1,163.6 µs / 799 qps | 1,204.8 µs / 813 qps | 0.95x / 1.07x | 0.96x / 1.04x |
| open | day 3 | 1,172.1 µs / 826 qps | 1,178.6 µs / 839 qps | 1,185.6 µs / 825 qps | 1.02x / 1.08x | 1.00x / 1.05x |
| close | day 1 | 851.8 µs / 776 qps | 839.7 µs / 796 qps | 802.4 µs / 806 qps | 1.03x / 0.99x | 1.04x / 0.98x |
| close | day 2 | 720.8 µs / 825 qps | 790.2 µs / 803 qps | 806.2 µs / 703 qps | 0.97x / 0.95x | 0.85x / 0.83x |
| close | day 3 | 713.0 µs / 811 qps | 829.2 µs / 787 qps | 823.6 µs / 774 qps | 0.97x / 0.98x | 0.95x / 0.96x |

**What is resolved and what is not.** The `pk` control touches no Cabin and returns one row; its on-over-off ratio ranges 0.98x to 1.09x across days and runs,
so **a ratio below about 1.10x is not a finding**, as in the morning. That puts `tape200` at the floor (the Cabin does not help a 200-row walk: 0.98x to 1.09x), `tape1k` over it by a little and `tape10k` and `board` clear of it.

**Against the morning (now over morning, derived qps (1,000,000 / mean µs) per arm, run A and run B):**

Derived qps now over the morning (same binary, old client), per arm, run A and run B:

| group | day | off A | on A | declared A | off B | on B | declared B |
|---|---|---|---|---|---|---|---|
| board | 1 | 1.09x | 1.16x | 1.11x | 1.10x | 1.19x | 1.15x |
| board | 2 | 1.10x | 1.22x | 1.20x | 1.11x | 1.24x | 1.18x |
| board | 3 | 1.11x | 1.22x | 1.22x | 1.10x | 1.26x | 1.20x |
| tape200 | 1 | 1.09x | 1.10x | 1.05x | 1.17x | 1.12x | 1.18x |
| tape200 | 2 | 1.10x | 1.17x | 1.23x | 1.21x | 1.22x | 1.25x |
| tape200 | 3 | 1.19x | 1.14x | 1.19x | 1.17x | 1.25x | 1.16x |
| tape1k | 1 | 1.10x | 1.11x | 1.06x | 1.13x | 1.13x | 1.13x |
| tape1k | 2 | 1.10x | 1.17x | 1.24x | 1.14x | 1.24x | 1.24x |
| tape1k | 3 | 1.16x | 1.15x | 1.19x | 1.15x | 1.27x | 1.15x |
| tape10k | 1 | 1.06x | 1.08x | 1.03x | 1.05x | 1.14x | 1.05x |
| tape10k | 2 | 1.04x | 1.06x | 1.13x | 1.05x | 1.18x | 1.13x |
| tape10k | 3 | 1.05x | 1.09x | 1.13x | 1.05x | 1.22x | 1.06x |
| pk | 1 | 1.01x | 0.97x | 0.89x | 1.00x | 1.01x | 0.96x |
| pk | 2 | 0.98x | 1.00x | 1.07x | 1.11x | 1.12x | 1.03x |
| pk | 3 | 1.00x | 0.95x | 1.02x | 1.00x | 1.12x | 1.00x |
| open | 1 | 0.93x | 1.01x | 0.94x | 1.08x | 0.99x | 1.03x |
| open | 2 | 1.07x | 1.01x | 1.00x | 1.05x | 1.09x | 1.06x |
| open | 3 | 0.99x | 1.08x | 0.99x | 1.00x | 1.03x | 1.08x |
| close | 1 | 1.16x | 1.16x | 1.18x | 1.19x | 1.19x | 1.20x |
| close | 2 | 1.19x | 1.18x | 1.17x | 1.22x | 1.17x | 1.15x |
| close | 3 | 1.16x | 1.16x | 1.15x | 1.21x | 1.19x | 1.17x |

Pooled per group and arm over the six days (median, with the range):

| group | arm | median of 6 (3 days x 2 runs) now/morning | min | max |
|---|---|---|---|---|
| board | off | 1.102x | 1.09x | 1.11x |
| board | on | 1.219x | 1.16x | 1.26x |
| board | declared | 1.189x | 1.11x | 1.22x |
| tape200 | off | 1.171x | 1.09x | 1.21x |
| tape200 | on | 1.153x | 1.10x | 1.25x |
| tape200 | declared | 1.181x | 1.05x | 1.25x |
| tape1k | off | 1.136x | 1.10x | 1.16x |
| tape1k | on | 1.158x | 1.11x | 1.27x |
| tape1k | declared | 1.171x | 1.06x | 1.24x |
| tape10k | off | 1.050x | 1.04x | 1.06x |
| tape10k | on | 1.112x | 1.06x | 1.22x |
| tape10k | declared | 1.096x | 1.03x | 1.13x |
| pk | off | 1.003x | 0.98x | 1.11x |
| pk | on | 1.002x | 0.95x | 1.12x |
| pk | declared | 1.011x | 0.89x | 1.07x |
| open | off | 1.026x | 0.93x | 1.08x |
| open | on | 1.020x | 0.99x | 1.09x |
| open | declared | 1.014x | 0.94x | 1.08x |
| close | off | 1.190x | 1.16x | 1.22x |
| close | on | 1.173x | 1.16x | 1.19x |
| close | declared | 1.169x | 1.15x | 1.20x |

The moved groups are the ones that return rows (`board` and `tape*` return the hot symbol's rows, `close` returns grouped rows). `tape10k` moves least in the `off` arm (1.05x) because 345 µs of its 10,000-row walk is engine
time the client cannot shorten; its served arms move 1.10x to 1.11x, a round trip plus a handful of rows. The `pk` and `open` rows are the control: 1.00x to 1.01x and 1.01x to 1.03x, inside the 0.89x to 1.12x
that `pk` ranges over cell by cell.

## 4. Percentiles

Run A, every phase (microseconds; the five percentiles are the driver's):

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load[off] | 31,200 ops | 39.3 µs | 43.1 µs | 43.9 µs | 63.3 µs | 78.7 µs | 1,221.7 µs | 0 errors |
| load[on] | 31,200 ops | 35.2 µs | 43.2 µs | 43.7 µs | 62.5 µs | 73.7 µs | 142.1 µs | 0 errors |
| load[declared] | 31,200 ops | 35.3 µs | 43.1 µs | 44.5 µs | 67.1 µs | 83.1 µs | 406.2 µs | 0 errors |
| d1-open[off] | 240 ops | 1,021.0 µs | 1,159.0 µs | 1,217.1 µs | 1,488.7 µs | 5,566.4 µs | 10,969.9 µs | 0 errors |
| d1-open[on] | 240 ops | 1,024.0 µs | 1,100.5 µs | 1,164.4 µs | 1,354.1 µs | 1,600.6 µs | 1,758.7 µs | 0 errors |
| d1-open[declared] | 240 ops | 1,022.9 µs | 1,144.1 µs | 1,200.0 µs | 1,448.1 µs | 2,458.1 µs | 9,087.2 µs | 0 errors |
| d1-board[off] | 2,400 ops | 334.7 µs | 374.2 µs | 386.6 µs | 431.9 µs | 454.8 µs | 739.6 µs | 0 errors |
| d1-tape200[off] | 396 ops | 81.8 µs | 84.5 µs | 89.7 µs | 129.5 µs | 146.2 µs | 152.9 µs | 0 errors |
| d1-tape1k[off] | 396 ops | 102.5 µs | 105.7 µs | 110.3 µs | 148.5 µs | 162.6 µs | 164.4 µs | 0 errors |
| d1-tape10k[off] | 396 ops | 325.8 µs | 336.0 µs | 345.5 µs | 392.6 µs | 427.0 µs | 462.0 µs | 0 errors |
| d1-pk[off] | 240 ops | 60.1 µs | 62.1 µs | 65.6 µs | 101.2 µs | 123.2 µs | 280.1 µs | 0 errors |
| d1-board[on] | 2,400 ops | 83.9 µs | 126.1 µs | 149.5 µs | 394.8 µs | 760.6 µs | 1,958.9 µs | 0 errors |
| d1-tape200[on] | 396 ops | 80.5 µs | 83.5 µs | 90.3 µs | 132.8 µs | 146.6 µs | 166.4 µs | 0 errors |
| d1-tape1k[on] | 396 ops | 80.6 µs | 83.4 µs | 97.5 µs | 147.4 µs | 183.2 µs | 197.0 µs | 0 errors |
| d1-tape10k[on] | 396 ops | 80.3 µs | 83.9 µs | 99.7 µs | 406.8 µs | 734.3 µs | 750.3 µs | 0 errors |
| d1-pk[on] | 240 ops | 60.2 µs | 62.7 µs | 65.5 µs | 106.4 µs | 133.2 µs | 228.0 µs | 0 errors |
| d1-board[declared] | 2,400 ops | 83.7 µs | 126.1 µs | 148.9 µs | 786.1 µs | 808.8 µs | 1,040.9 µs | 0 errors |
| d1-tape200[declared] | 396 ops | 80.6 µs | 83.6 µs | 91.7 µs | 134.8 µs | 152.3 µs | 207.5 µs | 0 errors |
| d1-tape1k[declared] | 396 ops | 80.4 µs | 83.0 µs | 91.9 µs | 181.4 µs | 209.6 µs | 223.1 µs | 0 errors |
| d1-tape10k[declared] | 396 ops | 80.2 µs | 84.9 µs | 100.9 µs | 771.0 µs | 868.2 µs | 881.1 µs | 0 errors |
| d1-pk[declared] | 240 ops | 60.8 µs | 63.4 µs | 69.3 µs | 112.0 µs | 127.3 µs | 229.4 µs | 0 errors |
| d1-close[off] | 12 ops | 653.7 µs | 692.3 µs | 851.8 µs | 3,048.3 µs | 3,048.3 µs | 3,048.3 µs | 0 errors |
| d1-close[on] | 12 ops | 689.9 µs | 729.1 µs | 839.7 µs | 2,847.4 µs | 2,847.4 µs | 2,847.4 µs | 0 errors |
| d1-close[declared] | 12 ops | 635.0 µs | 694.4 µs | 802.4 µs | 2,924.2 µs | 2,924.2 µs | 2,924.2 µs | 0 errors |
| d2-open[off] | 240 ops | 1,002.0 µs | 1,096.3 µs | 1,159.0 µs | 1,422.2 µs | 1,863.8 µs | 2,137.4 µs | 0 errors |
| d2-open[on] | 240 ops | 973.6 µs | 1,111.3 µs | 1,163.6 µs | 1,385.5 µs | 3,109.2 µs | 16,732.8 µs | 0 errors |
| d2-open[declared] | 240 ops | 952.4 µs | 1,124.1 µs | 1,204.8 µs | 1,373.0 µs | 2,411.1 µs | 4,391.2 µs | 0 errors |
| d2-board[off] | 2,400 ops | 332.4 µs | 371.5 µs | 382.4 µs | 427.1 µs | 451.0 µs | 746.1 µs | 0 errors |
| d2-tape200[off] | 396 ops | 82.0 µs | 84.4 µs | 87.5 µs | 123.1 µs | 142.4 µs | 160.8 µs | 0 errors |
| d2-tape1k[off] | 396 ops | 102.5 µs | 105.1 µs | 108.0 µs | 143.7 µs | 161.9 µs | 169.3 µs | 0 errors |
| d2-tape10k[off] | 396 ops | 325.6 µs | 334.6 µs | 343.5 µs | 391.5 µs | 438.5 µs | 458.0 µs | 0 errors |
| d2-pk[off] | 240 ops | 59.9 µs | 61.8 µs | 64.2 µs | 100.6 µs | 120.3 µs | 123.9 µs | 0 errors |
| d2-board[on] | 2,400 ops | 84.5 µs | 123.0 µs | 133.4 µs | 397.5 µs | 776.4 µs | 1,025.9 µs | 0 errors |
| d2-tape200[on] | 396 ops | 80.6 µs | 82.9 µs | 85.6 µs | 120.7 µs | 138.0 µs | 148.0 µs | 0 errors |
| d2-tape1k[on] | 396 ops | 80.6 µs | 82.6 µs | 84.3 µs | 126.2 µs | 165.3 µs | 175.5 µs | 0 errors |
| d2-tape10k[on] | 396 ops | 80.3 µs | 82.9 µs | 86.2 µs | 388.7 µs | 767.6 µs | 3,521.6 µs | 0 errors |
| d2-pk[on] | 240 ops | 61.4 µs | 63.1 µs | 64.8 µs | 98.3 µs | 110.2 µs | 119.2 µs | 0 errors |
| d2-board[declared] | 2,400 ops | 83.1 µs | 121.3 µs | 128.8 µs | 755.3 µs | 782.6 µs | 1,086.7 µs | 0 errors |
| d2-tape200[declared] | 396 ops | 80.7 µs | 82.8 µs | 84.0 µs | 107.2 µs | 122.7 µs | 138.1 µs | 0 errors |
| d2-tape1k[declared] | 396 ops | 80.3 µs | 82.4 µs | 83.9 µs | 148.0 µs | 173.1 µs | 188.4 µs | 0 errors |
| d2-tape10k[declared] | 396 ops | 80.5 µs | 82.9 µs | 85.1 µs | 740.2 µs | 829.8 µs | 848.0 µs | 0 errors |
| d2-pk[declared] | 240 ops | 61.4 µs | 63.2 µs | 64.6 µs | 85.8 µs | 99.5 µs | 107.6 µs | 0 errors |
| d2-close[off] | 12 ops | 637.7 µs | 685.0 µs | 720.8 µs | 2,699.8 µs | 2,699.8 µs | 2,699.8 µs | 0 errors |
| d2-close[on] | 12 ops | 689.2 µs | 698.2 µs | 790.2 µs | 2,877.2 µs | 2,877.2 µs | 2,877.2 µs | 0 errors |
| d2-close[declared] | 12 ops | 662.0 µs | 699.2 µs | 806.2 µs | 5,031.7 µs | 5,031.7 µs | 5,031.7 µs | 0 errors |
| d3-open[off] | 240 ops | 972.6 µs | 1,096.7 µs | 1,172.1 µs | 1,441.1 µs | 2,064.8 µs | 6,939.5 µs | 0 errors |
| d3-open[on] | 240 ops | 1,033.3 µs | 1,139.6 µs | 1,178.6 µs | 1,354.4 µs | 1,484.3 µs | 1,520.9 µs | 0 errors |
| d3-open[declared] | 240 ops | 1,039.5 µs | 1,132.5 µs | 1,185.6 µs | 1,361.7 µs | 2,020.7 µs | 3,314.2 µs | 0 errors |
| d3-board[off] | 2,400 ops | 341.2 µs | 381.4 µs | 393.0 µs | 433.4 µs | 447.4 µs | 757.1 µs | 0 errors |
| d3-tape200[off] | 396 ops | 82.1 µs | 84.2 µs | 87.3 µs | 127.6 µs | 149.6 µs | 152.7 µs | 0 errors |
| d3-tape1k[off] | 396 ops | 102.6 µs | 105.7 µs | 110.5 µs | 149.9 µs | 164.7 µs | 184.8 µs | 0 errors |
| d3-tape10k[off] | 396 ops | 326.5 µs | 335.9 µs | 347.7 µs | 391.3 µs | 444.3 µs | 461.7 µs | 0 errors |
| d3-pk[off] | 240 ops | 59.3 µs | 62.0 µs | 65.7 µs | 102.9 µs | 124.3 µs | 141.9 µs | 0 errors |
| d3-board[on] | 2,400 ops | 83.1 µs | 124.7 µs | 131.0 µs | 386.9 µs | 791.1 µs | 941.2 µs | 0 errors |
| d3-tape200[on] | 396 ops | 80.6 µs | 83.3 µs | 86.0 µs | 123.6 µs | 141.3 µs | 161.7 µs | 0 errors |
| d3-tape1k[on] | 396 ops | 80.7 µs | 83.0 µs | 86.2 µs | 132.6 µs | 162.8 µs | 181.4 µs | 0 errors |
| d3-tape10k[on] | 396 ops | 80.8 µs | 84.0 µs | 90.8 µs | 395.1 µs | 748.3 µs | 836.6 µs | 0 errors |
| d3-pk[on] | 240 ops | 61.1 µs | 63.0 µs | 65.8 µs | 104.7 µs | 120.6 µs | 145.6 µs | 0 errors |
| d3-board[declared] | 2,400 ops | 82.8 µs | 123.2 µs | 128.8 µs | 779.7 µs | 816.3 µs | 1,144.0 µs | 0 errors |
| d3-tape200[declared] | 396 ops | 80.7 µs | 83.1 µs | 85.0 µs | 119.8 µs | 143.1 µs | 147.3 µs | 0 errors |
| d3-tape1k[declared] | 396 ops | 80.2 µs | 82.9 µs | 85.0 µs | 122.3 µs | 165.5 µs | 193.2 µs | 0 errors |
| d3-tape10k[declared] | 396 ops | 80.6 µs | 83.4 µs | 88.7 µs | 751.6 µs | 841.1 µs | 863.7 µs | 0 errors |
| d3-pk[declared] | 240 ops | 61.4 µs | 63.1 µs | 64.4 µs | 97.3 µs | 117.1 µs | 124.2 µs | 0 errors |
| d3-close[off] | 12 ops | 687.2 µs | 703.9 µs | 713.0 µs | 2,786.7 µs | 2,786.7 µs | 2,786.7 µs | 0 errors |
| d3-close[on] | 12 ops | 706.0 µs | 742.8 µs | 829.2 µs | 2,840.5 µs | 2,840.5 µs | 2,840.5 µs | 0 errors |
| d3-close[declared] | 12 ops | 701.2 µs | 726.8 µs | 823.6 µs | 2,902.6 µs | 2,902.6 µs | 2,902.6 µs | 0 errors |

PostgreSQL 18.6, day 1, no index, run 1 of the morning's twin:

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

A probe is one round trip on one connection, so the decomposition is fixed cost plus rows walked plus rows returned; there is no commit or lock in the read phases and one durable commit per `open` statement.

| Component | Now | Morning (old client) | PostgreSQL (morning's run) | How derived |
|---|---|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | **71.4 µs** | 72.6 µs | 69.4 µs | `pk` mean across arms, days and runs (one row returned: the client change does not reach it) |
| Rows walked by an unserved equality | **26.1 ns per row** plus about 89 µs | 26.5 ns plus about 103 µs | 40.5 ns plus about 76 µs (morning's derivation) | `off` tape p50 at 200 and 10,000 rows, day 1, median of two runs: (p50 at 10,000 - p50 at 200) / 9,800. The walk is the engine's; the fixed part is the client's reply |
| A Cabin-served equality | **about 87 µs** at 10,000 rows (days 2 and 3) | about 106 µs | - | `on` tape10k p50, flat in n |
| Write statement with its commit (`open`) | **1,175 µs** p50 per autocommit insert | 1,188 µs | 1,161 µs | `open` p50 over arms, days and runs: the durable commit is nearly all of it |
| Lock or conflict wait | none: one connection | none | none | - |

The board's latency tail by arm (run A day by day; run B and the morning for the p95), microseconds:

| day | arm | run A mean | run A p50 | run A p95 | run A p99 | run B p95 | morning run A p95 |
|---|---|---|---|---|---|---|---|
| day 1 | off | 390.3 µs | 386.6 µs | 431.9 µs | 454.8 µs | 431.6 µs | 474.6 µs |
| day 1 | on | 205.1 µs | 149.5 µs | 394.8 µs | 760.6 µs | 395.2 µs | 440.2 µs |
| day 1 | declared | 249.9 µs | 148.9 µs | 786.1 µs | 808.8 µs | 771.9 µs | 779.4 µs |
| day 2 | off | 386.3 µs | 382.4 µs | 427.1 µs | 451.0 µs | 432.5 µs | 467.0 µs |
| day 2 | on | 199.2 µs | 133.4 µs | 397.5 µs | 776.4 µs | 389.2 µs | 444.9 µs |
| day 2 | declared | 226.6 µs | 128.8 µs | 755.3 µs | 782.6 µs | 763.3 µs | 784.8 µs |
| day 3 | off | 395.3 µs | 393.0 µs | 433.4 µs | 447.4 µs | 430.4 µs | 486.3 µs |
| day 3 | on | 172.7 µs | 131.0 µs | 386.9 µs | 791.1 µs | 396.9 µs | 395.9 µs |
| day 3 | declared | 195.1 µs | 128.8 µs | 779.7 µs | 816.3 µs | 773.9 µs | 793.7 µs |

**The declared arm's tail is the open question this scenario keeps asking** and the client change did not touch it: its board p95 is 755 to 786 µs
against the `on` arm's 387 to 398 µs and the walk's 427 to 433 µs, as in the morning.

Server CPU seconds by arm (the driver's `/proc` reading), now and in the morning. **This is the engine's side of the statement and it did not move**:

| arm | board A | board B | board morning A / B | tape A | tape B | tape morning A / B | open A | close A |
|---|---|---|---|---|---|---|---|---|
| off | 2.20 s | 2.20 s | 2.22 s / 2.24 s | 0.50 s | 0.45 s | 0.43 s / 0.47 s | 0.02 s | 0.04 s |
| on | 0.78 s | 0.84 s | 0.78 s / 0.92 s | 0.21 s | 0.20 s | 0.23 s / 0.28 s | 0.06 s | 0.02 s |
| declared | 0.99 s | 1.03 s | 1.03 s / 1.03 s | 0.25 s | 0.21 s | 0.27 s / 0.24 s | 0.05 s | 0.04 s |

## 6. The controller's lifecycle

Snapshots of `SHOW CABIN_OPTIMIZER` on the `on` arm at chosen points:

run A:

| t (s) | day | tag | managed | creates | extends | drops | entries |
|---|---|---|---|---|---|---|---|
| 13.8 s | day 1 | trading-block-3 | 4 Cabins | 4 creates | 32 extends | 0 drops | board_a:ACTIVE/3p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/1p; tape_10k_a:ACTIVE/1p |
| 36.3 s | day 1 | trading-block-9 | 4 Cabins | 4 creates | 105 extends | 0 drops | board_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 47.5 s | day 1 | trading-block-12 | 4 Cabins | 4 creates | 105 extends | 0 drops | board_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 54.1 s | day 1 | overnight | 4 Cabins | 4 creates | 105 extends | 0 drops | board_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 104.7 s | day 2 | trading-block-3 | 5 Cabins | 5 creates | 135 extends | 0 drops | board_a:ACTIVE/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 127.2 s | day 2 | trading-block-9 | 5 Cabins | 5 creates | 142 extends | 0 drops | board_a:DECAYING/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 138.5 s | day 2 | trading-block-12 | 5 Cabins | 5 creates | 142 extends | 0 drops | board_a:DECAYING/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 145.1 s | day 2 | overnight | 5 Cabins | 5 creates | 142 extends | 0 drops | board_a:DECAYING/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 195.7 s | day 3 | trading-block-3 | 5 Cabins | 5 creates | 165 extends | 0 drops | board_a:ACTIVE/18p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 218.2 s | day 3 | trading-block-9 | 5 Cabins | 5 creates | 172 extends | 0 drops | board_a:ACTIVE/18p; board_b_a:DECAYING/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 229.4 s | day 3 | trading-block-12 | 5 Cabins | 5 creates | 172 extends | 0 drops | board_a:ACTIVE/18p; board_b_a:DECAYING/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 236.0 s | day 3 | overnight | 5 Cabins | 5 creates | 172 extends | 0 drops | board_a:ACTIVE/18p; board_b_a:DECAYING/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |

final header: managed=5 pages_committed=32 page_budget=1024 ticks=546 creates=5 extends=172 heals=0 drops=0 deferred=0 failures=0
verify per day: PASS, PASS, PASS

run B:

| t (s) | day | tag | managed | creates | extends | drops | entries |
|---|---|---|---|---|---|---|---|
| 14.3 s | day 1 | trading-block-3 | 4 Cabins | 4 creates | 28 extends | 0 drops | board_a:ACTIVE/3p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/1p; tape_10k_a:ACTIVE/1p |
| 36.8 s | day 1 | trading-block-9 | 4 Cabins | 4 creates | 104 extends | 0 drops | board_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 48.0 s | day 1 | trading-block-12 | 4 Cabins | 4 creates | 104 extends | 0 drops | board_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 54.7 s | day 1 | overnight | 4 Cabins | 4 creates | 104 extends | 0 drops | board_a:ACTIVE/9p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/2p; tape_10k_a:ACTIVE/2p |
| 105.2 s | day 2 | trading-block-3 | 5 Cabins | 5 creates | 134 extends | 0 drops | board_a:ACTIVE/9p; board_b_a:ACTIVE/3p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 127.8 s | day 2 | trading-block-9 | 5 Cabins | 5 creates | 142 extends | 0 drops | board_a:DECAYING/9p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 139.0 s | day 2 | trading-block-12 | 4 Cabins | 5 creates | 142 extends | 1 drops | board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 145.6 s | day 2 | overnight | 4 Cabins | 5 creates | 142 extends | 1 drops | board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/3p |
| 196.2 s | day 3 | trading-block-3 | 5 Cabins | 6 creates | 165 extends | 1 drops | board_a:ACTIVE/3p; board_b_a:ACTIVE/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/5p |
| 218.7 s | day 3 | trading-block-9 | 5 Cabins | 6 creates | 187 extends | 1 drops | board_a:ACTIVE/6p; board_b_a:DECAYING/4p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 229.9 s | day 3 | trading-block-12 | 4 Cabins | 6 creates | 194 extends | 2 drops | board_a:ACTIVE/14p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |
| 236.5 s | day 3 | overnight | 4 Cabins | 6 creates | 194 extends | 2 drops | board_a:ACTIVE/14p; tape_200_a:ACTIVE/1p; tape_1k_a:ACTIVE/3p; tape_10k_a:ACTIVE/6p |

final header: managed=4 pages_committed=24 page_budget=1024 ticks=546 creates=6 extends=194 heals=0 drops=2 deferred=0 failures=0
verify per day: PASS, PASS, PASS

Run A, defaults: four Cabins are created in the first 13 s of day 1, a fifth (`board_b`) on day 2's first block, `board_a` goes DECAYING on day 2 and returns to ACTIVE on day 3 with its pages doubled (9 to 18) when its hot set
comes back, then `board_b` goes DECAYING; the final header reads `creates=5 extends=172 heals=0 drops=0 deferred=0 failures=0`, **equal to the morning's**. Run B (cooldown 2 half-lives): `board_a` is DROPPED on day 2, re-nominated on day 3
(`creates=6`) and `board_b` dropped at day 3's last block; `creates=6 extends=194 drops=2 failures=0` (morning 193 extends). The controller does not see the client: its ticks are the engine's.

## 7. Correctness

The driver ran its 18-statement comparison across the three arms each day: **PASS on all three days in both runs and in all six A/B runs** (`verify` in the JSON, 8 of 8 runs with no mismatch), so a Cabin, declared or controller-built, changed no reply, and the client change changed none either.

## 8. The client, isolated: old against new, interleaved

Three runs of the driver (each three servers, one run A) per client, old, new, old, new, old, new, the same binary copy, derived qps of day 1 (the tables above give days 2 and 3 for the matrix runs):

| group (day 1) | arm | old: 1 / 2 / 3 | old median | spread | new: 1 / 2 / 3 | new median | spread | new over old | read |
|---|---|---|---|---|---|---|---|---|---|
| board | off | 2,353 / 2,303 / 2,364 | 2,353 qps | 2.6 % | 2,530 / 2,561 / 2,514 | 2,530 qps | 1.9 % | 1.07x (+7.5 %) | **finding** |
| board | on | 4,149 / 4,237 / 4,172 | 4,172 qps | 2.1 % | 5,076 / 5,063 / 4,998 | 5,063 qps | 1.6 % | 1.21x (+21.4 %) | **finding** |
| board | declared | 3,613 / 3,564 / 3,559 | 3,564 qps | 1.5 % | 4,175 / 4,132 / 4,225 | 4,175 qps | 2.2 % | 1.17x (+17.2 %) | **finding** |
| tape200 | off | 9,461 / 8,921 / 9,488 | 9,461 qps | 6.0 % | 9,634 / 10,132 / 9,398 | 9,634 qps | 7.6 % | 1.02x (+1.8 %) | inside spread |
| tape200 | on | 8,673 / 9,166 / 9,346 | 9,166 qps | 7.3 % | 10,905 / 10,776 / 10,582 | 10,776 qps | 3.0 % | 1.18x (+17.6 %) | **finding** |
| tape200 | declared | 9,643 / 9,166 / 8,945 | 9,166 qps | 7.6 % | 10,846 / 10,823 / 11,261 | 10,846 qps | 4.0 % | 1.18x (+18.3 %) | **finding** |
| tape1k | off | 7,962 / 7,553 / 7,680 | 7,680 qps | 5.3 % | 8,197 / 8,425 / 7,930 | 8,197 qps | 6.0 % | 1.07x (+6.7 %) | **finding** |
| tape1k | on | 8,584 / 8,636 / 8,811 | 8,636 qps | 2.6 % | 10,288 / 10,194 / 9,718 | 10,194 qps | 5.6 % | 1.18x (+18.0 %) | **finding** |
| tape1k | declared | 8,993 / 8,613 / 8,518 | 8,613 qps | 5.5 % | 10,471 / 9,785 / 10,406 | 10,406 qps | 6.6 % | 1.21x (+20.8 %) | **finding** |
| tape10k | off | 2,731 / 2,676 / 2,731 | 2,731 qps | 2.0 % | 2,795 / 2,796 / 2,702 | 2,795 qps | 3.4 % | 1.02x (+2.3 %) | inside spread |
| tape10k | on | 5,155 / 5,408 / 5,459 | 5,408 qps | 5.6 % | 6,042 / 5,848 / 5,811 | 5,848 qps | 4.0 % | 1.08x (+8.1 %) | **finding** |
| tape10k | declared | 4,585 / 4,462 / 4,472 | 4,472 qps | 2.7 % | 4,943 / 4,658 / 4,897 | 4,897 qps | 5.8 % | 1.10x (+9.5 %) | **finding** |
| pk | off | 14,025 / 13,624 / 14,728 | 14,025 qps | 7.9 % | 12,484 / 12,594 / 12,469 | 12,484 qps | 1.0 % | 0.89x (-11.0 %) | **finding** |
| pk | on | 13,369 / 13,514 / 14,085 | 13,514 qps | 5.3 % | 14,144 / 13,736 / 13,089 | 13,736 qps | 7.7 % | 1.02x (+1.6 %) | inside spread |
| pk | declared | 14,684 / 13,643 / 13,495 | 13,643 qps | 8.7 % | 14,684 / 13,333 / 14,577 | 14,577 qps | 9.3 % | 1.07x (+6.9 %) | inside spread |
| open | off | 817 / 823 / 778 | 817 qps | 5.5 % | 735 / 805 / 865 | 805 qps | 16.1 % | 0.99x (-1.5 %) | inside spread |
| open | on | 801 / 804 / 848 | 804 qps | 5.9 % | 805 / 813 / 848 | 813 qps | 5.4 % | 1.01x (+1.0 %) | inside spread |
| open | declared | 796 / 810 / 831 | 810 qps | 4.2 % | 837 / 783 / 795 | 795 qps | 6.9 % | 0.98x (-1.8 %) | inside spread |
| close | off | 679 / 685 / 667 | 679 qps | 2.7 % | 792 / 788 / 822 | 792 qps | 4.2 % | 1.17x (+16.7 %) | **finding** |
| close | on | 669 / 683 / 659 | 669 qps | 3.6 % | 812 / 796 / 809 | 809 qps | 2.0 % | 1.21x (+20.9 %) | **finding** |
| close | declared | 676 / 684 / 671 | 676 qps | 1.9 % | 812 / 795 / 792 | 795 qps | 2.5 % | 1.18x (+17.6 %) | **finding** |

**The client moves what it can move, arm by arm.** `board` 1.07x (`off`), 1.21x (`on`), 1.17x (`declared`); `tape200`/`tape1k` served arms 1.18x to 1.21x; `close` 1.17x to 1.21x; `tape10k` `off` 1.02x
(the walk dominates). `open` is flat (0.98x to 1.01x). **The `pk` control is not flat in the `off` arm: 0.89x** (14,025 against 12,484 qps, spreads 7.9 % and 1.0 %),
while its `on` and `declared` arms read +1.6 % and +6.9 % inside their spreads. A -11 % in a one-row statement that the change cannot reach is this scenario's noise floor, so the `board` `off` figure of +7.5 % (spreads 2.6 % and 1.9 %), smaller than the control's own excursion, is read as
"in the direction the other arms show", not as a separate result. The matrix runs (section 3) have no such excursion in their `pk` rows (`off` 1.00x, six days).

## 9. Against PostgreSQL 18.6 (the morning's run, reused)

PostgreSQL is the floor. The twin runs **day 1 only, unpaced, one server, no index**, by its own design: at defaults a PostgreSQL probe seq-scans on day 1 as on day 300. Its numbers are the morning's three runs. KDS side: run A and run B day 1,
derived qps, the median of the two; ratio above 1.00x = KDS faster; the morning's KDS ratio after the table:

| group | PG18 day 1 (no index): p50 / median qps (spread) | KDS off | KDS on | KDS declared | off / PG | on / PG | declared / PG | morning: off / PG | on / PG | declared / PG |
|---|---|---|---|---|---|---|---|---|---|---|
| open | 1,160.8 µs / 831 qps (10.9 %) | 778 qps | 832 qps | 802 qps | 0.94x | 1.00x | 0.96x | 0.93x | 1.00x | 0.98x |
| board | 504.4 µs / 1,976 qps (2.7 %) | 2,565 qps | 4,911 qps | 4,079 qps | 1.30x | 2.49x | 2.06x | 1.19x | 2.11x | 1.84x |
| tape200 | 75.9 µs / 12,484 qps (6.7 %) | 10,275 qps | 10,231 qps | 10,401 qps | 0.82x | 0.82x | 0.83x | 0.73x | 0.74x | 0.75x |
| tape1k | 108.8 µs / 8,726 qps (2.5 %) | 8,526 qps | 9,889 qps | 9,715 qps | 0.98x | 1.13x | 1.11x | 0.88x | 1.01x | 1.02x |
| tape10k | 472.7 µs / 2,100 qps (1.4 %) | 2,848 qps | 5,910 qps | 4,751 qps | 1.36x | 2.81x | 2.26x | 1.29x | 2.54x | 2.17x |
| pk | 61.9 µs / 14,409 qps (4.0 %) | 13,307 qps | 13,658 qps | 13,375 qps | 0.92x | 0.95x | 0.93x | 0.92x | 0.96x | 1.00x |
| close | 442.7 µs / 1,185 qps (0.2 %) | 795 qps | 801 qps | 802 qps | 0.67x | 0.68x | 0.68x | 0.57x | 0.58x | 0.57x |

The comparison with a Cabin is not a like-for-like with PostgreSQL's *indexed* case, which this twin does not run (no `symbol` index on PostgreSQL); "KDS on / PG" says what a Cabin buys over a PostgreSQL seq scan, not over an index.
`tape200` is 0.82x to 0.83x on every arm (from 0.73x to 0.75x): the 200-row probe is still about 13 µs dearer than PostgreSQL's (89 against 76 µs p50) while the `pk` round trip differs by 2 µs (71.4 against 69.4 µs mean), so the
gap is not the round trip; whether it is the scan statement's setup or the 200-row walk, this run does not separate. `close` (three full-relation folds) is 0.67x to 0.68x on every arm (from 0.57x): the fold returns grouped rows and gained from the decoder, and the rest is the whole-relation fold cost that scenario 1 measures as 0.68x on `agg-global` and `agg-distinct`.

## 10. Noise floor, contamination and host

The floor is the `pk` control: on-over-off 0.98x to 1.09x in the matrix runs, and the A/B spreads of the group cells are 1.0 % to 16.1 % (the widest is `open`, the durable group). PostgreSQL's three runs spread 0.2 % to 10.9 %
(`open` 10.9 %, the durable group). The two KDS runs give the repeat: run A and run B differ by up to 11 % per group and arm on days 1 and 2 (day 3 is excluded: run B rebuilds a Cabin there). No KDS cell was contaminated in this scenario (`logs/validity.log`).

Per-cell host stamps (run A and B hold three servers each; the precheck of the `-off` server stands for the cell, the monitor covered the whole window):

| cell | start (UTC) | end (UTC) | load average before (1 / 5 / 15 min) | gate wait | build or test process | foreign samples >= 10 % CPU | exit |
|---|---|---|---|---|---|---|---|
| `s4` | 07:16:19 | 07:21:05 | 0.89 / 0.81 / 0.85 | 0 s | none | 0 of 131 samples | 0 |
| `cd2-s4` | 07:21:13 | 07:26:00 | 0.24 / 0.52 / 0.73 | 0 s | none | 3 of 131 samples | 0 |
| `ab-old1-s4` | 07:31:14 | 07:36:01 | 0.97 / 0.76 / 0.70 | 0 s | none | 0 of 131 samples | 0 |
| `ab-new1-s4` | 07:37:27 | 07:42:14 | 0.69 / 0.53 / 0.60 | 0 s | none | 1 of 132 samples | 0 |
| `ab-old2-s4` | 07:43:53 | 07:48:39 | 0.80 / 0.58 / 0.58 | 0 s | none | 0 of 130 samples | 0 |
| `ab-new2-s4` | 07:50:05 | 07:54:52 | 0.77 / 0.51 / 0.53 | 0 s | none | 0 of 131 samples | 0 |
| `ab-old3-s4` | 07:56:28 | 08:01:15 | 0.76 / 0.58 / 0.56 | 0 s | none | 1 of 131 samples | 0 |
| `ab-new3-s4` | 08:02:41 | 08:07:27 | 0.80 / 0.55 / 0.53 | 0 s | none | 0 of 131 samples | 0 |


## 11. What the run says about the engine

1. **Only the client's half of a served probe changed, and a Cabin-served probe is mostly client.** The engine's side did not move (walk 26.1 ns per row, server CPU 2.20 s for the `off` boards), while a served `board` probe is 150 µs
   against 182 µs: the Cabin's multiple over a walk rose from 1.79x to 1.90x on `board` without the Cabin doing anything differently. A cache that answers in a round trip is measured by its client.
2. **The controller is independent of the client**: the lifecycle, the page counts and the extends are the morning's within one extend, in both runs.
3. **The declared arm's tail** (p95 1.8x the walk's on day 1) is untouched by the decoder, which places it in the engine's miss path of a declared Cabin; this run, like the morning's, does not explain it.

## 12. Archive

`bench/v3.3.0/archive/scenario4-v3.3.0-38-ga40882af/`: both matrix runs' JSON (including the full controller evidence and the 18-statement comparison), the six A/B runs' JSON, driver stdout, the three servers' configs per run. What the four scenarios share is in
`bench/v3.3.0/archive/run-v3.3.0-38-ga40882af/`. The PostgreSQL twin's JSON is in the morning's archive, `bench/v3.3.0/archive/scenario4-v3.3.0-33-g9682b725/json/`.
