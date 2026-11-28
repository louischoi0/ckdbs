# EH-S1: a global fold from the row's bytes, interleaved against the engine it changes

**One line.** With a global fold taking its rows from the page's bytes, a fold of four aggregates over 60,480 rows costs **29.1 ns of server CPU a row against 38.8 ns before** (0.75x) **and PostgreSQL 18.7's 28.0 ns** (1.04x, inside PostgreSQL's 5.1 % spread), and a bare `COUNT(*)` 11.4 ns against 19.2 ns before and 12.1 ns on PostgreSQL. Every global shape moves by 8.1 ns to 9.7 ns a row, the path EH-S0 priced at 8.3 ns; every grouped shape and the filter that rejects every row are inside their spreads, as nothing in them changed. **EH-Q1's bar - a global fold's server CPU a row at or below PostgreSQL's - is met within the spread, not below it.**

Measured by CLA in the session that opened EH (no `ck-tester` agent was spawned). Driver: `tools/eh_s0_premise.py` (EH-S0's), counting clients on both sides.

## Stamp

| Field | Value |
|---|---|
| Version, commit | worktree `eh-native-fold-per-row`. **Old**: the binary EH-S0 measured, Release from `git archive 9830e6bb0`, whose engine source is `01c6c741c`'s (`v3.5.1-35-g01c6c741c`). **New**: Release from `git archive 6142f32f1` (`v3.5.1-41-g6142f32f1`), EH-S1. Both hashes in `binaries.sha256` |
| Date and time | 2026-11-28 03:50:33 to 03:51:32 UTC, **59 s**, inside the 20-minute stage budget |
| Procedure | six rounds, a fresh KDS server and data file each, the binary in the order old, new, new, old, old, new; each round runs the eleven shapes twice on KDS and on one PostgreSQL 18.7 container beside it (`docker.io/library/postgres:18` at its defaults, run by `podman`, host network); so 6 cells a shape on each binary and 12 on PostgreSQL. Servers `taskset -c 0-7`, driver `taskset -c 8-23` |
| KDS | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536` |
| Data | `pb_bars`, 60,480 rows of 8 integer columns, loaded afresh each round |
| Host and device | Intel Core Ultra 7 270K Plus, 24 CPUs (P-cores 0 to 7), `powersave`; `/dev/mapper/fedora-root`, xfs |
| Quiet gate | before each round: no build or test process, at most 8 % busy over 3 s; all six passed at once. **The 1-minute load was 2.0 to 3.7** (`r*.precheck`), above every other run of the day: a `critics-developer` review of this stage was running in another tree on the host; the gate saw no compiler or test process at the six instants it looked. The old binary's figures repeat EH-S0's within 0.5 % on every global shape (19.2 ns against 19.2 ns, 38.8 ns against 39.0 ns), which is the check this run has on itself |
| Rule | spread = (max - min) / median over a binary's six cells (PostgreSQL's twelve); a difference smaller than the larger spread is inside the spread; a spread over 20 % carries no claim. Server CPU: every KDS server thread's `schedstat` less its idle tick, PostgreSQL's backend; "a row" divides by 60,480 |
| Comparator | this engine's own previous number: the old binary, in the same window (EH-S0 measured it too) |
| Suite | 5,431 of 5,432 at `6142f32f1` (the stage log names the one failure) |

## Server CPU a row

| shape | old | new | new / old | read | PostgreSQL | new / PostgreSQL | read |
|---|---|---|---|---|---|---|---|
| `scan-reject` | 10.9 ns (19.8 %) | 11.5 ns (9.1 %) | **1.05x** | inside the spread | 17.8 ns (4.4 %) | **0.64x** | KDS cheaper |
| `count-star` | 19.2 ns (3.8 %) | 11.4 ns (3.1 %) | **0.59x** | new cheaper | 12.1 ns (4.1 %) | **0.94x** | KDS cheaper |
| `count-col` | 27.7 ns (0.7 %) | 19.0 ns (3.3 %) | **0.68x** | new cheaper | 19.0 ns (7.6 %) | **1.00x** | inside the spread |
| `sum` | 27.7 ns (1.8 %) | 19.2 ns (3.2 %) | **0.69x** | new cheaper | 20.4 ns (6.1 %) | **0.94x** | inside the spread |
| `min-max` | 32.3 ns (3.0 %) | 23.6 ns (2.7 %) | **0.73x** | new cheaper | 22.5 ns (5.1 %) | **1.05x** | inside the spread |
| `agg-global` | 38.8 ns (7.7 %) | 29.1 ns (2.1 %) | **0.75x** | new cheaper | 28.0 ns (5.1 %) | **1.04x** | inside the spread |
| `group-8` | 43.4 ns (4.2 %) | 43.8 ns (4.4 %) | **1.01x** | inside the spread | 39.0 ns (11.2 %) | **1.12x** | KDS dearer |
| `group-8-sum` | 49.0 ns (5.5 %) | 50.0 ns (4.1 %) | **1.02x** | inside the spread | 48.4 ns (2.7 %) | **1.03x** | inside the spread |
| `group-7560` | 74.2 ns (6.8 %) | 75.8 ns (17.8 %) | **1.02x** | inside the spread | 59.6 ns (14.6 %) | **1.27x** | KDS dearer |
| `group-7560-sum` | 86.3 ns (11.4 %) | 88.0 ns (10.8 %) | **1.02x** | inside the spread | 76.0 ns (2.6 %) | **1.16x** | KDS dearer |
| `group-60480` | 233.7 ns (5.2 %) | 232.4 ns (9.1 %) | **0.99x** | inside the spread | 199.0 ns (16.2 %) | **1.17x** | KDS dearer |

## Latency a statement

| shape | binary | cells | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| `scan-reject` | old | 6 cells | 746.5 µs | 673.7 µs | 693.2 µs | 701.1 µs | 1,018.0 µs | 1,018.0 µs |
| `scan-reject` | new | 6 cells | 824.1 µs | 699.1 µs | 729.7 µs | 822.3 µs | 1,086.8 µs | 1,086.8 µs |
| `scan-reject` | PostgreSQL | 12 cells | 1,174.3 µs | 1,055.5 µs | 1,070.6 µs | 1,106.2 µs | 1,399.9 µs | 1,399.9 µs |
| `count-star` | old | 6 cells | 1,227.6 µs | 1,188.8 µs | 1,201.3 µs | 1,205.9 µs | 1,415.2 µs | 1,415.2 µs |
| `count-star` | new | 6 cells | 735.1 µs | 703.0 µs | 718.1 µs | 726.8 µs | 854.2 µs | 854.2 µs |
| `count-star` | PostgreSQL | 12 cells | 772.2 µs | 713.7 µs | 731.6 µs | 742.1 µs | 985.8 µs | 985.8 µs |
| `count-col` | old | 6 cells | 1,780.5 µs | 1,710.4 µs | 1,728.8 µs | 1,738.1 µs | 1,963.3 µs | 1,963.3 µs |
| `count-col` | new | 6 cells | 1,216.2 µs | 1,167.4 µs | 1,187.3 µs | 1,198.4 µs | 1,398.7 µs | 1,398.7 µs |
| `count-col` | PostgreSQL | 12 cells | 1,205.8 µs | 1,146.0 µs | 1,161.4 µs | 1,181.9 µs | 1,404.4 µs | 1,404.4 µs |
| `sum` | old | 6 cells | 1,746.4 µs | 1,703.7 µs | 1,723.3 µs | 1,732.2 µs | 1,917.4 µs | 1,917.4 µs |
| `sum` | new | 6 cells | 1,228.7 µs | 1,184.2 µs | 1,204.4 µs | 1,213.8 µs | 1,393.6 µs | 1,393.6 µs |
| `sum` | PostgreSQL | 12 cells | 1,275.3 µs | 1,234.4 µs | 1,246.9 µs | 1,259.5 µs | 1,492.6 µs | 1,492.6 µs |
| `min-max` | old | 6 cells | 2,043.8 µs | 1,995.2 µs | 2,006.4 µs | 2,022.5 µs | 2,234.1 µs | 2,234.1 µs |
| `min-max` | new | 6 cells | 1,498.7 µs | 1,446.9 µs | 1,459.2 µs | 1,476.6 µs | 1,694.8 µs | 1,694.8 µs |
| `min-max` | PostgreSQL | 12 cells | 1,405.9 µs | 1,354.6 µs | 1,366.5 µs | 1,381.3 µs | 1,578.0 µs | 1,578.0 µs |
| `agg-global` | old | 6 cells | 2,438.6 µs | 2,399.2 µs | 2,422.6 µs | 2,431.9 µs | 2,595.7 µs | 2,595.7 µs |
| `agg-global` | new | 6 cells | 1,856.1 µs | 1,789.9 µs | 1,806.8 µs | 1,827.3 µs | 2,018.9 µs | 2,018.9 µs |
| `agg-global` | PostgreSQL | 12 cells | 1,770.7 µs | 1,696.4 µs | 1,714.3 µs | 1,735.8 µs | 1,971.7 µs | 1,971.7 µs |
| `group-8` | old | 6 cells | 2,762.2 µs | 2,662.4 µs | 2,693.6 µs | 2,720.6 µs | 2,939.1 µs | 2,939.1 µs |
| `group-8` | new | 6 cells | 2,805.0 µs | 2,705.6 µs | 2,735.3 µs | 2,748.4 µs | 3,006.9 µs | 3,006.9 µs |
| `group-8` | PostgreSQL | 12 cells | 2,504.0 µs | 2,334.8 µs | 2,434.1 µs | 2,542.1 µs | 2,716.4 µs | 2,716.4 µs |
| `group-8-sum` | old | 6 cells | 3,220.4 µs | 3,048.5 µs | 3,098.7 µs | 3,171.7 µs | 3,475.6 µs | 3,475.6 µs |
| `group-8-sum` | new | 6 cells | 3,209.2 µs | 3,104.8 µs | 3,129.4 µs | 3,195.6 µs | 3,383.1 µs | 3,383.1 µs |
| `group-8-sum` | PostgreSQL | 12 cells | 3,135.2 µs | 2,901.5 µs | 3,068.6 µs | 3,141.9 µs | 3,413.5 µs | 3,413.5 µs |
| `group-7560` | old | 6 cells | 6,008.7 µs | 5,816.0 µs | 5,900.3 µs | 6,021.3 µs | 6,186.5 µs | 6,186.5 µs |
| `group-7560` | new | 6 cells | 6,113.5 µs | 5,990.0 µs | 6,072.2 µs | 6,100.2 µs | 6,255.1 µs | 6,255.1 µs |
| `group-7560` | PostgreSQL | 12 cells | 7,645.2 µs | 7,485.5 µs | 7,606.9 µs | 7,657.1 µs | 7,816.7 µs | 7,816.7 µs |
| `group-7560-sum` | old | 6 cells | 7,375.0 µs | 7,192.6 µs | 7,304.0 µs | 7,407.8 µs | 7,538.8 µs | 7,538.8 µs |
| `group-7560-sum` | new | 6 cells | 7,491.6 µs | 7,283.1 µs | 7,372.4 µs | 7,474.9 µs | 7,805.7 µs | 7,805.7 µs |
| `group-7560-sum` | PostgreSQL | 12 cells | 9,033.1 µs | 8,881.8 µs | 8,933.2 µs | 9,066.0 µs | 9,192.8 µs | 9,192.8 µs |
| `group-60480` | old | 6 cells | 24,538.3 µs | 23,573.5 µs | 23,742.6 µs | 24,068.0 µs | 26,262.8 µs | 26,262.8 µs |
| `group-60480` | new | 6 cells | 24,549.3 µs | 23,840.1 µs | 24,318.6 µs | 24,361.1 µs | 26,353.7 µs | 26,353.7 µs |
| `group-60480` | PostgreSQL | 12 cells | 42,478.9 µs | 41,393.9 µs | 41,878.1 µs | 42,312.9 µs | 43,449.0 µs | 43,449.0 µs |

## What the run teaches

1. **The path from a visible row to the fold was the whole of a global fold's deficit.** EH-S0 put 8.3 ns a row on it; every global shape fell by 8.1 ns to 9.7 ns, and each now sits within its spread of PostgreSQL's, `COUNT(*)` below it.
2. **Nothing that keeps the frame's path moved**: the eight grouped and filtered shapes read 0.99x to 1.05x, inside their spreads.
3. **What is left behind PostgreSQL is the grouped fold**: 1.12x at 8 groups, 1.16x to 1.27x at 7,560, 1.17x at 60,480, the founding of a group (EH-S0: about 230 ns against 160 ns) and the probe's path through the frame.

## What is cut, absent or unverified

- **The host's load was higher than in any other run of the day** (see the stamp); the old binary reproducing EH-S0 is the evidence the run was not disturbed, not a proof.
- **Two reps a round, six rounds**: a difference under about 5 % is not resolved on most shapes.
- **One relation shape**: integer cells. A `char`, a decimal, a nullable column and a `uint64` are pinned for correctness by `tests/bytes_fold_equivalence_test.cpp` and not measured here.
- **Scenario 4's `close`** is not run here; it is the close's.

## Archive

`bench/v3.5.1/archive/eh-s1-v3.5.1-41-g6142f32f1/`: each round's JSON and stdout, return codes, prechecks, `run.sh`, the watchdog and its log, the timeline, the configuration, the device, both binaries' hashes and the tool copies' hashes.
