# EH-S2: the group table, interleaved against EH-S1

**One line.** With the group table an open-addressing table over one key arena, a fold founding 7,560 groups costs **63.3 ns of server CPU a row against 76.5 ns at EH-S1** (0.83x) and 59.0 ns on PostgreSQL 18.7 (1.07x); with a `SUM` beside the count it is 74.7 ns against 88.1 ns and PostgreSQL's 76.5 ns, inside the spread. A fold founding a group for every row reads 173.7 ns against PostgreSQL's 193.0 ns. The eight-group probe moved by 4 % and is still 1.08x of PostgreSQL's. **The global shapes moved too, by 4 % to 9 %**: the S2 binary also carries EH-S1's review commits (the fold arm's early return, the narrowed shape), which the S1 binary does not.

Measured by CLA in the session that opened EH (no `ck-tester` agent was spawned). Driver: `tools/eh_s0_premise.py`, counting clients on both sides.

## Stamp

| Field | Value |
|---|---|
| Version, commit | worktree `eh-native-fold-per-row`. **Old (S1)**: Release from `git archive 6142f32f1` (`v3.5.1-41-g6142f32f1`). **New (S2)**: Release from `git archive b36deb576` with the two files `70e0cb368` commits (`measured-source.diff`; the measured `src/exec/aggregate.cpp` equals the committed one, checked by hash), that is the source of `v3.5.1-46-g70e0cb368` |
| Date and time | 2026-11-28 04:43:17 to 04:44:13 UTC, **56 s** |
| Procedure | as EH-S1's: six rounds, old, new, new, old, old, new, a fresh KDS server and data file each, two reps a round of the eleven shapes on KDS and on one PostgreSQL 18.7 container beside it; servers `taskset -c 0-7`, driver `taskset -c 8-23` |
| KDS | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536` |
| Host and device | Intel Core Ultra 7 270K Plus, 24 CPUs (P-cores 0 to 7), `powersave`; `/dev/mapper/fedora-root`, xfs |
| Quiet gate | all six rounds passed at once; 1-minute load 0.47 to 0.90 |
| Rule | spread = (max - min) / median over a binary's six cells (PostgreSQL's twelve); a difference smaller than the larger spread is inside the spread; a spread over 20 % carries no claim |
| Comparator | this engine's own previous number: EH-S1's binary in the same window |
| Suite | 5,433 of 5,435 at `70e0cb368`; the stage log names the two failures |

## Server CPU a row

| shape | S1 | S2 | S2 / S1 | read | PostgreSQL | S2 / PostgreSQL | read |
|---|---|---|---|---|---|---|---|
| `scan-reject` | 11.7 ns (16.7 %) | 11.7 ns (33.3 %) | **1.00x** | no claim | 17.9 ns (22.0 %) | **0.65x** | no claim |
| `count-star` | 11.4 ns (2.8 %) | 10.4 ns (2.9 %) | **0.91x** | S2 cheaper | 12.2 ns (6.5 %) | **0.85x** | KDS cheaper |
| `count-col` | 18.9 ns (2.0 %) | 17.8 ns (3.3 %) | **0.94x** | S2 cheaper | 19.0 ns (4.4 %) | **0.94x** | KDS cheaper |
| `sum` | 19.2 ns (2.0 %) | 18.1 ns (1.4 %) | **0.94x** | S2 cheaper | 20.4 ns (2.0 %) | **0.89x** | KDS cheaper |
| `min-max` | 23.6 ns (2.1 %) | 22.2 ns (1.7 %) | **0.94x** | S2 cheaper | 22.3 ns (2.5 %) | **0.99x** | inside the spread |
| `agg-global` | 28.9 ns (2.5 %) | 27.8 ns (0.5 %) | **0.96x** | S2 cheaper | 28.0 ns (5.9 %) | **0.99x** | inside the spread |
| `group-8` | 44.0 ns (3.0 %) | 42.1 ns (3.8 %) | **0.96x** | S2 cheaper | 39.0 ns (4.8 %) | **1.08x** | KDS dearer |
| `group-8-sum` | 50.0 ns (3.4 %) | 49.2 ns (6.5 %) | **0.98x** | inside the spread | 48.3 ns (2.3 %) | **1.02x** | inside the spread |
| `group-7560` | 76.5 ns (9.1 %) | 63.3 ns (3.0 %) | **0.83x** | S2 cheaper | 59.0 ns (6.2 %) | **1.07x** | KDS dearer |
| `group-7560-sum` | 88.1 ns (12.2 %) | 74.7 ns (6.6 %) | **0.85x** | S2 cheaper | 76.5 ns (6.4 %) | **0.98x** | inside the spread |
| `group-60480` | 243.7 ns (27.0 %) | 173.7 ns (6.7 %) | **0.71x** | no claim | 193.0 ns (8.4 %) | **0.90x** | KDS cheaper |

## Latency a statement

| shape | binary | cells | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| `scan-reject` | S1 | 6 cells | 779.9 µs | 720.4 µs | 727.3 µs | 733.6 µs | 1,239.1 µs | 1,239.1 µs |
| `scan-reject` | S2 | 6 cells | 814.0 µs | 694.4 µs | 713.8 µs | 811.9 µs | 1,360.1 µs | 1,360.1 µs |
| `scan-reject` | PostgreSQL | 12 cells | 1,156.3 µs | 1,063.7 µs | 1,081.9 µs | 1,094.6 µs | 1,439.4 µs | 1,439.4 µs |
| `count-star` | S1 | 6 cells | 732.4 µs | 701.5 µs | 716.1 µs | 727.8 µs | 849.4 µs | 849.4 µs |
| `count-star` | S2 | 6 cells | 671.2 µs | 634.7 µs | 646.5 µs | 654.7 µs | 843.3 µs | 843.3 µs |
| `count-star` | PostgreSQL | 12 cells | 777.1 µs | 716.3 µs | 729.6 µs | 747.3 µs | 1,011.9 µs | 1,011.9 µs |
| `count-col` | S1 | 6 cells | 1,209.7 µs | 1,172.8 µs | 1,187.2 µs | 1,194.9 µs | 1,372.7 µs | 1,372.7 µs |
| `count-col` | S2 | 6 cells | 1,146.5 µs | 1,103.1 µs | 1,117.5 µs | 1,128.8 µs | 1,360.3 µs | 1,360.3 µs |
| `count-col` | PostgreSQL | 12 cells | 1,183.0 µs | 1,141.2 µs | 1,158.3 µs | 1,169.9 µs | 1,375.0 µs | 1,375.0 µs |
| `sum` | S1 | 6 cells | 1,236.7 µs | 1,189.7 µs | 1,208.5 µs | 1,214.4 µs | 1,412.2 µs | 1,412.2 µs |
| `sum` | S2 | 6 cells | 1,162.2 µs | 1,116.4 µs | 1,129.8 µs | 1,138.4 µs | 1,356.6 µs | 1,356.6 µs |
| `sum` | PostgreSQL | 12 cells | 1,266.9 µs | 1,225.8 µs | 1,243.7 µs | 1,253.6 µs | 1,463.3 µs | 1,463.3 µs |
| `min-max` | S1 | 6 cells | 1,525.3 µs | 1,458.2 µs | 1,478.4 µs | 1,488.3 µs | 1,806.8 µs | 1,806.8 µs |
| `min-max` | S2 | 6 cells | 1,419.7 µs | 1,377.0 µs | 1,388.5 µs | 1,392.5 µs | 1,615.5 µs | 1,615.5 µs |
| `min-max` | PostgreSQL | 12 cells | 1,384.1 µs | 1,348.4 µs | 1,361.3 µs | 1,370.7 µs | 1,539.2 µs | 1,539.2 µs |
| `agg-global` | S1 | 6 cells | 1,847.1 µs | 1,781.3 µs | 1,804.8 µs | 1,820.1 µs | 2,013.8 µs | 2,013.8 µs |
| `agg-global` | S2 | 6 cells | 1,773.6 µs | 1,706.3 µs | 1,728.8 µs | 1,736.3 µs | 1,995.5 µs | 1,995.5 µs |
| `agg-global` | PostgreSQL | 12 cells | 1,742.8 µs | 1,694.7 µs | 1,708.4 µs | 1,719.1 µs | 1,951.2 µs | 1,951.2 µs |
| `group-8` | S1 | 6 cells | 2,814.4 µs | 2,732.1 µs | 2,753.5 µs | 2,769.1 µs | 3,008.5 µs | 3,008.5 µs |
| `group-8` | S2 | 6 cells | 2,679.9 µs | 2,582.4 µs | 2,606.1 µs | 2,636.2 µs | 2,867.4 µs | 2,867.4 µs |
| `group-8` | PostgreSQL | 12 cells | 2,502.1 µs | 2,346.0 µs | 2,387.7 µs | 2,524.7 µs | 2,685.4 µs | 2,685.4 µs |
| `group-8-sum` | S1 | 6 cells | 3,285.6 µs | 3,113.3 µs | 3,149.2 µs | 3,296.2 µs | 3,588.4 µs | 3,588.4 µs |
| `group-8-sum` | S2 | 6 cells | 3,150.2 µs | 3,037.0 µs | 3,061.2 µs | 3,114.1 µs | 3,346.4 µs | 3,346.4 µs |
| `group-8-sum` | PostgreSQL | 12 cells | 3,119.1 µs | 2,923.6 µs | 3,051.9 µs | 3,135.0 µs | 3,400.0 µs | 3,400.0 µs |
| `group-7560` | S1 | 6 cells | 6,174.4 µs | 6,064.2 µs | 6,133.8 µs | 6,170.5 µs | 6,353.1 µs | 6,353.1 µs |
| `group-7560` | S2 | 6 cells | 5,373.4 µs | 5,244.3 µs | 5,286.5 µs | 5,380.7 µs | 5,506.8 µs | 5,506.8 µs |
| `group-7560` | PostgreSQL | 12 cells | 7,595.1 µs | 7,462.8 µs | 7,557.8 µs | 7,614.2 µs | 7,705.3 µs | 7,705.3 µs |
| `group-7560-sum` | S1 | 6 cells | 7,496.3 µs | 7,371.3 µs | 7,443.4 µs | 7,543.4 µs | 7,657.7 µs | 7,657.7 µs |
| `group-7560-sum` | S2 | 6 cells | 6,686.9 µs | 6,534.4 µs | 6,615.1 µs | 6,706.3 µs | 6,853.3 µs | 6,853.3 µs |
| `group-7560-sum` | PostgreSQL | 12 cells | 9,024.4 µs | 8,854.4 µs | 8,931.7 µs | 9,029.2 µs | 9,268.2 µs | 9,268.2 µs |
| `group-60480` | S1 | 6 cells | 25,065.4 µs | 24,188.5 µs | 24,458.9 µs | 24,859.1 µs | 25,997.7 µs | 25,997.7 µs |
| `group-60480` | S2 | 6 cells | 20,887.8 µs | 20,416.9 µs | 20,529.9 µs | 20,704.7 µs | 21,272.6 µs | 21,272.6 µs |
| `group-60480` | PostgreSQL | 12 cells | 41,836.0 µs | 41,024.5 µs | 41,494.6 µs | 41,898.7 µs | 43,207.1 µs | 43,207.1 µs |

## What the run teaches

1. **The table was the founding cost**: 13.2 ns a row at 7,560 groups (8 rows a group, so about 105 ns a group) and, where every row founds a group, about 70 ns a row. Founding is no longer behind PostgreSQL's.
2. **What is left at 7,560 groups and 8 groups is the probe's path**, the key encoded from the frame and the frame itself: 1.07x and 1.08x of PostgreSQL's. A grouped fold still takes every row through the frame.
3. **The global shapes' 4 % to 9 % is not S2's**, as the stamp says.

## What is cut, absent or unverified

- **`scan-reject` carries no claim** in this run (spreads 16.7 % to 33.3 %); it is a filter, untouched by both stages.
- **`group-60480` carries no claim against S1** (27.0 %); against PostgreSQL it reads ahead.
- **The global shapes' movement is not split** between the two review commits the S2 binary also carries.
- **One key type**: integer keys of one column. A `varchar` key or a key of several columns is pinned for correctness by the aggregate suite and not measured here.

## Archive

`bench/v3.5.1/archive/eh-s2-v3.5.1-46-g70e0cb368/`: each round's JSON and stdout, return codes, prechecks, `run.sh`, the watchdog and its log, the timeline, the configuration, the device, both binaries' hashes, the measured source's diff and the tool copies' hashes.
