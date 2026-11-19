# CY close re-measure - CY-Q31's remedy: a paced GATHER is behind OLTP

Measured on `cy-managed-statistics` at `v3.4.12-114-g127e5c34` (committed
2026-11-18 20:49:51 UTC), 2026-11-18 20:56 - 21:07 UTC. **The CY-Q14 bar is met
at `cores = 1` and at `cores = 2`**: with the same looping `GATHER STATISTICS ON
pb_bars` as `results-cy-close-v3.4.12-113-g61f5dddd.md` measured, every OLTP
cell's p50 and p99 are inside the run's own noise floor, and the loop now
achieves 13 to 18 gathers a second where it achieved 2,000 to 6,000. A second
file rather than a section, because that file's tables describe another engine
state (rule 1). The workload, cells, drivers, conditions (OFF / ON / OFF2) and
method are those file's; only what differs is stated here.

## Stamp

| item | value |
|---|---|
| branch / commit | `worktree-cy-managed-statistics`, `127e5c34` = `v3.4.12-114-g127e5c34`; built from a detached worktree of that commit (the coordinator's uncommitted edits in the working tree are not in the binary) |
| binary | `build-release`, Release, gcc 14.2.0, built 2026-11-18 20:55:33 UTC (after the commit); measured copy `kds_server_C` sha256 `7b04400f8eff1c64fba190d3adb01e7f1da8067e95e28ae3c71e5607f8d683b5` |
| A (for the B'' check) | `9712f5f0` = `v3.4.12-112-g9712f5f0`, the copy of the earlier run (sha256 `adaec037b95e83f147a4c9575c51945f5ab8643cad95dabfdf593434450f54d0`) |
| device / host | `/dev/nvme0n1p1` ext4 under `/home/louis/cyclose/run/`; 8 vCPUs (4 cores x 2 threads); server on CPU 2 (`cores = 1`) or 2,3 (`cores = 2`), driver CPU 4, gatherer CPU 5 |
| configuration | `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`, `join_order` default; every other key default |
| quiet machine | no competing `cc1plus` / `cmake --build` / `ctest` in any cell; the 1-minute load average was decaying from the build in the first rounds of the `cores = 1` run (max 4.9) and under 1.2 in the other two. The long-running sandbox `kds_server` was idle |
| driver | as before; `cy_close.py` now also records the plan-statistics generation before and after each ON cell (`sys.meta` `plan_stats_generation`) and the ON window's wall time; archived in `archive/cy-close-v3.4.12-113-g61f5dddd/q31/` |

## 1. CY-Q14 with the paced gather

Percentiles in us, medians over 8 rounds of 4,000 statements; server CPU
omitted here (ON includes the gatherer's process CPU, as before).

### cores = 1

| cell | engine | rounds | ops/round | p0 us | p25 us | p50 us | p75 us | p90 us | p99 us | QPS | server CPU us/stmt | p50 min..max | p50 vs OFF | p99 vs OFF |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| pk-point-read | OFF | 8 | 4000 | 38 | 40 | 40 | 41 | 44 | 54 | 24034 | 23.0 | 40..41 | - | - |
| pk-point-read | ON | 8 | 4000 | 37 | 40 | 40 | 42 | 44 | 56 | 23941 | 23.8 | 40..41 | -0.0% | +3.2% |
| pk-point-read | OFF2 | 8 | 4000 | 38 | 40 | 40 | 41 | 44 | 53 | 24169 | 22.8 | 40..41 | -0.2% | -1.6% |
| insert-1row | OFF | 8 | 4000 | 297 | 428 | 479 | 546 | 622 | 806 | 2002 | 69.1 | 462..492 | - | - |
| insert-1row | ON | 8 | 4000 | 291 | 430 | 484 | 553 | 624 | 812 | 2001 | 73.6 | 469..529 | +1.1% | +0.7% |
| insert-1row | OFF2 | 8 | 4000 | 296 | 421 | 473 | 540 | 612 | 790 | 2033 | 68.5 | 457..496 | -1.3% | -2.0% |
| update-1row | OFF | 8 | 4000 | 303 | 433 | 485 | 551 | 621 | 797 | 1987 | 72.8 | 466..495 | - | - |
| update-1row | ON | 8 | 4000 | 307 | 432 | 484 | 552 | 620 | 790 | 1991 | 78.5 | 473..513 | -0.2% | -0.9% |
| update-1row | OFF2 | 8 | 4000 | 304 | 426 | 479 | 544 | 611 | 776 | 2025 | 72.7 | 472..514 | -1.3% | -2.7% |
| point-join-3rel | OFF | 8 | 4000 | 48 | 50 | 51 | 52 | 55 | 67 | 19144 | 34.2 | 51..52 | - | - |
| point-join-3rel | ON | 8 | 4000 | 48 | 51 | 51 | 53 | 56 | 68 | 18983 | 35.3 | 51..52 | +0.4% | +2.6% |
| point-join-3rel | OFF2 | 8 | 4000 | 48 | 51 | 51 | 53 | 55 | 70 | 18984 | 34.7 | 51..52 | +0.4% | +4.3% |
| point-join-2rel | OFF | 8 | 4000 | 44 | 45 | 46 | 47 | 49 | 60 | 21269 | 29.2 | 46..47 | - | - |
| point-join-2rel | ON | 8 | 4000 | 42 | 46 | 46 | 48 | 51 | 65 | 21008 | 30.5 | 46..47 | +0.4% | +7.8% |
| point-join-2rel | OFF2 | 8 | 4000 | 43 | 45 | 46 | 47 | 50 | 62 | 21178 | 29.3 | 46..46 | +0.1% | +2.9% |
| join-no-literal | OFF | 8 | 4000 | 823 | 864 | 890 | 931 | 998 | 1120 | 1100 | 641.0 | 883..898 | - | - |
| join-no-literal | ON | 8 | 4000 | 828 | 876 | 902 | 944 | 1006 | 1160 | 1085 | 656.2 | 887..920 | +1.4% | +3.6% |
| join-no-literal | OFF2 | 8 | 4000 | 823 | 870 | 902 | 946 | 1010 | 1114 | 1090 | 648.7 | 885..929 | +1.3% | -0.5% |
| model-join | OFF | 8 | 4000 | 299 | 307 | 312 | 320 | 337 | 468 | 3116 | 165.6 | 310..315 | - | - |
| model-join | ON | 8 | 4000 | 299 | 308 | 313 | 323 | 337 | 450 | 3123 | 168.0 | 311..328 | +0.4% | -3.9% |
| model-join | OFF2 | 8 | 4000 | 299 | 308 | 312 | 321 | 338 | 468 | 3116 | 165.2 | 310..314 | +0.2% | +0.0% |

### cores = 2 (gatherer's session on the batch core, `SET PRIORITY background`)

| cell | engine | rounds | ops/round | p0 us | p25 us | p50 us | p75 us | p90 us | p99 us | QPS | server CPU us/stmt | p50 min..max | p50 vs OFF | p99 vs OFF |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| pk-point-read | OFF | 8 | 4000 | 39 | 40 | 41 | 42 | 44 | 53 | 23774 | 24.6 | 41..41 | - | - |
| pk-point-read | ON | 8 | 4000 | 39 | 40 | 41 | 42 | 45 | 54 | 23741 | 25.0 | 41..41 | +0.1% | +2.1% |
| pk-point-read | OFF2 | 8 | 4000 | 39 | 41 | 41 | 42 | 45 | 55 | 23711 | 24.5 | 41..41 | +0.2% | +3.1% |
| insert-1row | OFF | 8 | 4000 | 337 | 469 | 526 | 596 | 668 | 863 | 1838 | 101.2 | 511..547 | - | - |
| insert-1row | ON | 8 | 4000 | 335 | 474 | 532 | 608 | 684 | 894 | 1807 | 104.9 | 524..593 | +1.1% | +3.6% |
| insert-1row | OFF2 | 8 | 4000 | 339 | 477 | 535 | 604 | 678 | 846 | 1817 | 101.4 | 509..553 | +1.6% | -2.0% |
| update-1row | OFF | 8 | 4000 | 344 | 485 | 543 | 612 | 685 | 859 | 1776 | 107.5 | 521..556 | - | - |
| update-1row | ON | 8 | 4000 | 340 | 475 | 532 | 601 | 674 | 850 | 1826 | 110.2 | 519..549 | -2.1% | -1.0% |
| update-1row | OFF2 | 8 | 4000 | 335 | 482 | 540 | 613 | 690 | 879 | 1792 | 106.6 | 520..608 | -0.6% | +2.3% |
| point-join-3rel | OFF | 8 | 4000 | 50 | 52 | 52 | 54 | 57 | 71 | 18526 | 36.9 | 52..53 | - | - |
| point-join-3rel | ON | 8 | 4000 | 50 | 52 | 53 | 54 | 57 | 71 | 18444 | 37.6 | 52..54 | +0.4% | -0.7% |
| point-join-3rel | OFF2 | 8 | 4000 | 50 | 52 | 53 | 54 | 57 | 71 | 18537 | 36.9 | 52..53 | +0.4% | -0.7% |
| point-join-2rel | OFF | 8 | 4000 | 45 | 47 | 47 | 49 | 51 | 65 | 20549 | 31.5 | 47..48 | - | - |
| point-join-2rel | ON | 8 | 4000 | 44 | 46 | 47 | 48 | 51 | 66 | 20699 | 31.9 | 47..48 | -0.6% | +1.3% |
| point-join-2rel | OFF2 | 8 | 4000 | 44 | 46 | 47 | 48 | 50 | 62 | 20800 | 31.3 | 47..47 | -0.6% | -3.9% |
| join-no-literal | OFF | 8 | 4000 | 907 | 952 | 980 | 1020 | 1091 | 1208 | 1000 | 743.3 | 970..983 | - | - |
| join-no-literal | ON | 8 | 4000 | 906 | 954 | 984 | 1024 | 1092 | 1202 | 999 | 753.9 | 978..992 | +0.4% | -0.5% |
| join-no-literal | OFF2 | 8 | 4000 | 906 | 952 | 981 | 1021 | 1089 | 1205 | 1002 | 741.2 | 976..985 | +0.0% | -0.2% |
| model-join | OFF | 8 | 4000 | 301 | 310 | 315 | 324 | 341 | 466 | 3101 | 171.9 | 314..316 | - | - |
| model-join | ON | 8 | 4000 | 301 | 310 | 316 | 327 | 345 | 485 | 3067 | 176.9 | 314..319 | +0.5% | +3.9% |
| model-join | OFF2 | 8 | 4000 | 301 | 310 | 315 | 326 | 345 | 474 | 3083 | 171.7 | 314..317 | +0.2% | +1.6% |

### The loop's rate and what it published

| item | cores = 1 | cores = 2 |
|---|---|---|
| gathers per second while ON, beside the read cells | 15.6 to 17.9 | 15.5 to 17.8 |
| gathers per second while ON, beside the write and long-join cells | 12.3 to 14.4 (13.0 on INSERT) | 13.0 to 13.3 |
| mean duration of one gather of `pb_bars` (637 leaves, 40 slices) | 79 to 90 ms | 85 ms |
| leaves per second | about 7,800 to 11,400 | about 8,300 to 11,300 |
| plan-statistics generations published by the ON windows (every cell) | 0 | 0 |
| (previous engine state, for the rate only) | 2,000 to 2,400 per read cell | 2,750 per write cell |

0 generations is the count of the table being gathered, which did not move
(no write touches `pb_bars` in any cell); the remedy installs a generation only
when a published band moved. The leaves-per-second figure is `637 x gathers/s`;
it sits at the 8,000 ceiling of CY-Q31 (16 leaves per 2 ms) plus the cost of the
slice itself, so the pacing is what bounds the loop.

### Verdict against CY-Q14

| bar | verdict | numbers |
|---|---|---|
| `cores = 1`: p50 and p99 within 5 % | **met** | OLTP cells, ON against OFF, p50 / p99: point read -0.0 % / +3.2 %, INSERT +1.1 % / +0.7 %, UPDATE -0.2 % / -0.9 %, chain +0.4 % / +2.6 %. The noise floor (OFF2 against OFF) is up to 1.3 % at p50 and 4.3 % at p99 on the same cells, so the +3.2 % and +2.6 % p99 are inside it |
| `cores = 2`: inside the noise floor | **met** | point read +0.1 % / +2.1 %, INSERT +1.1 % / +3.6 %, UPDATE -2.1 % / -1.0 %, chain +0.4 % / -0.7 %; floor up to 1.6 % at p50 and 3.1 % at p99 (INSERT p99 +3.6 % against a floor of -2.0 % on that cell and +3.1 % on the point read: inside the largest p99 floor of the run, not above it by a margin this sample resolves) |
| non-OLTP controls (the two-relation join, `join-no-literal`, `model-join`) | inside the floor | `cores = 1`: p50 +0.4 / +1.4 / +0.4 %; `cores = 2`: -0.6 / +0.4 / +0.5 % |

Rule 9 (the 200 / 1K / 10K sweep) was not done, as before; the gathered relation
is one size, 60,480 rows.

## 2. B'' against A (short pair)

B'' is `127e5c34` with an empty model (`B`), `A` is `9712f5f0`, `A2` the control
(same binary as A), `Bp` B'' after a `GATHER` of every relation; 6 interleaved
rounds of 4,000 statements, `cores = 1`, CPU 2.

| cell | engine | rounds | ops/round | p0 us | p25 us | p50 us | p75 us | p90 us | p99 us | QPS | server CPU us/stmt | p50 min..max | p50 vs A | p99 vs A |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| pk-point-read | A | 6 | 4000 | 38 | 39 | 39 | 41 | 45 | 58 | 24216 | 21.4 | 39..40 | - | - |
| pk-point-read | B | 6 | 4000 | 38 | 39 | 40 | 41 | 45 | 56 | 24265 | 21.3 | 39..40 | +0.2% | -3.2% |
| pk-point-read | A2 | 6 | 4000 | 38 | 39 | 40 | 42 | 44 | 55 | 24327 | 21.4 | 39..40 | +0.2% | -5.2% |
| pk-point-read | Bp | 6 | 4000 | 38 | 39 | 40 | 41 | 46 | 56 | 24207 | 21.4 | 39..40 | +0.3% | -3.8% |
| insert-1row | A | 6 | 4000 | 307 | 425 | 479 | 544 | 617 | 807 | 2016 | 67.5 | 473..485 | - | - |
| insert-1row | B | 6 | 4000 | 302 | 424 | 477 | 542 | 620 | 783 | 2026 | 66.7 | 463..505 | -0.4% | -3.0% |
| insert-1row | A2 | 6 | 4000 | 303 | 427 | 478 | 543 | 613 | 783 | 2023 | 66.5 | 465..482 | -0.3% | -3.1% |
| insert-1row | Bp | 6 | 4000 | 297 | 426 | 481 | 551 | 625 | 802 | 2000 | 66.5 | 463..495 | +0.4% | -0.7% |
| update-1row | A | 6 | 4000 | 304 | 427 | 479 | 543 | 616 | 794 | 2015 | 70.4 | 467..499 | - | - |
| update-1row | B | 6 | 4000 | 299 | 426 | 479 | 544 | 616 | 787 | 2019 | 70.9 | 464..502 | +0.1% | -0.9% |
| update-1row | A2 | 6 | 4000 | 304 | 423 | 475 | 541 | 613 | 791 | 2037 | 70.3 | 469..498 | -0.8% | -0.3% |
| update-1row | Bp | 6 | 4000 | 303 | 421 | 473 | 533 | 600 | 760 | 2055 | 70.1 | 460..486 | -1.1% | -4.2% |
| point-join-3rel | A | 6 | 4000 | 47 | 50 | 50 | 52 | 56 | 74 | 19080 | 32.8 | 50..51 | - | - |
| point-join-3rel | B | 6 | 4000 | 48 | 49 | 50 | 52 | 57 | 71 | 19261 | 32.7 | 50..51 | -0.6% | -4.2% |
| point-join-3rel | A2 | 6 | 4000 | 48 | 50 | 50 | 53 | 58 | 73 | 18992 | 33.0 | 50..51 | +0.5% | -2.0% |
| point-join-3rel | Bp | 6 | 4000 | 49 | 50 | 50 | 53 | 58 | 72 | 19013 | 33.0 | 50..51 | +0.3% | -3.2% |

**Met.** B'' against A: p50 +0.2 % (point read), -0.4 % (INSERT), +0.1 %
(UPDATE), -0.6 % (chain); p99 -3.2 / -3.0 / -0.9 / -4.2 %. The control A2 differs
from A by up to 0.8 % at p50 and 5.2 % at p99 on these cells, so every delta is
inside the floor; server CPU per statement is equal to the decimal (21.3 against
21.4 us on the point read, 66.7 against 67.5 us on the INSERT). The pacing
touched no statement path, and the measurement agrees. (The p99 deltas are all
negative and in the order of the control's own: noise, not a gain.)

## What the run teaches

The cost of the first measurement was the gather's rate. A gather bounded at
about 8,000 leaves a second costs the OLTP cells nothing this rig resolves, at
both core counts, where the unbounded loop's 2,000+ gathers a second moved them
by 20 to 85 %; the earlier diagnostic's rate-proportionality is thereby
confirmed from the other side. The republish rule and the pacing were changed
together, so this run cannot say which of the two carries the improvement; the
earlier diagnostic argued the republish was not the cause of the slowdown, which
leaves the pacing as the carrier. What this measures is the bar's own wording
("in a loop"): an operator's gather is now a bounded background walk of about
80 ms per 60,000-row relation, 637 leaves, and a relation's gather time grows
linearly with its leaf count at the ceiling. Not measured: a relation larger
than `pb_bars`, a gather loop on more than one relation at once, and `cores = 4`.

## What was cut

- `cores = 4` and `8`, `strict` durability, the row-count sweep, the CY-S1
  restart gate, a gather of a larger relation.
- The B'' pair is 6 rounds and 4 cells (the point read, INSERT, UPDATE and the
  chain), as the request allowed; the PostgreSQL floor was not measured.
- No engine code changed in this measurement; the suite was not run here (the
  coordinator reports the Debug suite at `127e5c34`: 0 failed of 4728).
