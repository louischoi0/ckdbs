# CY close - managed statistics: the statement path unchanged, a looping GATHER not behind OLTP

Measured on `cy-managed-statistics` at `v3.4.12-113-g61f5dddd`, 2026-11-18.
Two findings. **The statement path of CY is free**: an engine at CY's close
that never gathers (B), and one that has gathered every relation (B'), run
each cell of a point read, a tuple-key chain, a single-row INSERT and a
single-row UPDATE inside the run's noise of the engine CY opened from (A).
**CY-Q14's bar is not met**: a `GATHER STATISTICS` looping beside the OLTP
cells moves their p50 and p99 by up to +13 % at `cores = 1` (bar: 5 %), and at
`cores = 2` and `4`, with the gatherer on a batch core, the point read and the
tuple-key chain by +21 % to +43 % at p50 and up to +51 % at p99 (the writes by
+3 % to +7 % at p50, up to +14 % at p99; bar: the noise floor, 2-3 %). The second finding is the one to act on.

## Stamp

| item | value |
|---|---|
| date and time executed | 2026-11-18 20:23 - 20:58 UTC (A/B run 20:23-20:28, Q14 runs 20:29-20:43, diagnostics 20:46-20:58) |
| branch | `worktree-cy-managed-statistics` |
| B, the close | `61f5dddd`, `git describe --tags` = `v3.4.12-113-g61f5dddd`, committed 2026-11-18 20:19:29 UTC |
| A, CY-S1b's base on main | `9712f5f0` = `v3.4.12-112-g9712f5f0`, built from a detached worktree of its own |
| why A is not CY's first commit | CY-S1a (below A on main) put nothing on a statement's path: it writes at shutdown and reads at mount only (`docs/spec/plan-stats.md` PS15). A against B is CY's whole statement-path delta |
| tree | clean at `61f5dddd` for everything under `src/`, `include/`, `tests/`, `CMakeLists.txt` (the only modified tracked file, `CLAUDE.md`, is not a build input; the untracked harness directory is this run's archive) |
| binaries | `build-release` of each tree (`-DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, gcc 14.2.0), both built 2026-11-18 20:22:48 UTC, after their commits (binary newer than HEAD, so it is the engine at HEAD) |
| measured copies | `kds_server_A` sha256 `adaec037b95e83f147a4c9575c51945f5ab8643cad95dabfdf593434450f54d0`; `kds_server_B` sha256 `5a13b6eaf4381e152fa9a0ade9d917da935c4bca70e153a448264bf4e9285e0c`; every server started from a copy in `/home/louis/cyclose/run/` |
| device | data files and WAL under `/home/louis/cyclose/run/` on `/dev/nvme0n1p1`, ext4 (`df -T`), never tmpfs |
| host | Intel Xeon Platinum 8581C, 8 vCPUs = 4 physical cores x 2 threads (siblings 0/4, 1/5, 2/6, 3/7). Server pinned to CPU 2 (`cores = 1`), 2,3 (`cores = 2`), 2,3,6,7 (`cores = 4`); the driver to CPU 4, the gatherer to CPU 5. `cores = 4` therefore puts two server cores on each of two physical cores |
| server configuration | `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`, `join_order` default (`fixed`), every other key default; cores as per table |
| quiet machine | no `cc1plus`, `cmake --build` or `ctest` in any cell (`host_state()` recorded per cell in the archive's JSON). The 1-minute load average was still decaying from the builds in the A/B run's first three rounds (5.7 falling to 2.1); the later runs' load stayed under 2. The 4 servers of the A/B run are idle but pinned to one CPU, one measured at a time |
| driver | `bench/v3.4.12/archive/cy-close-v3.4.12-113-g61f5dddd/cy_close.py` (derived from CX's close driver), `gatherer.py`, `diag.py`, `go.sh`, `wd.sh` (60 s watchdog; every run ended `DONE`, none orphaned) |

## What was measured

**Workload.** Eight relations on one server (`BTREE`): `pb_bars` 60,480 rows
(the largest, 637 leaves), `pc_loans` 10,000, `pc_results` 2,872, `pc_oltp`
2,000, `pc_users` 200, `pb_symbols` 8, `pc_models` 8, `pb_exchanges` 2. Each
cell is 4,000 statements on one serial connection after a 400-statement warm
(random literals from a seeded stream, the same stream on every engine), 8
interleaved rounds, the starting engine rotated and the order reversed on even
rounds. Tables show the median over the 8 rounds of each percentile (the
driver's per-round percentile is a nearest-rank over the 4,000 latencies).
Latency resolution is 1 us.

| cell | statement | OLTP-set member |
|---|---|---|
| `pk-point-read` | `SELECT * FROM pb_bars WHERE id = <k>` | yes |
| `insert-1row` | `INSERT INTO pc_oltp (v) VALUES (<x>)` (issued tuple key) | yes |
| `update-1row` | `UPDATE pc_oltp SET v = <x> WHERE id = <k>` (2,000-row table) | yes |
| `point-join-3rel` | `pb_bars JOIN pb_symbols JOIN pb_exchanges WHERE b.id = <k>` (tuple-key chain) | yes |
| `point-join-2rel` | the same without `pb_exchanges` | no, shown as a control |
| `join-no-literal` | `pc_users JOIN pc_loans ... WHERE u.id BETWEEN 1 AND 16` (planner-sensitive, a statement-local inner build) | no |
| `model-join` | `pc_results JOIN pc_models WHERE r.model_id = 3` (planner-sensitive, reordered) | no |

**Row-count sweep (rule 9): not done.** The relations above are fixed
(2 to 60,480 rows); the cells that touch one row (point read, insert, update,
chain) do not scale with rows by construction, and the sweep of 200 / 1K / 10K
was cut for the one-hour budget. The relation sizes in the table are what a
reader can reproduce; a `GATHER`'s own cost is per leaf (637 leaves in
`pb_bars`) and was measured at that one size.

**Baseline (rule 4).** In-run: A is the baseline of the A/B run and OFF of
the Q14 runs. No earlier results file carries this exact cell set (the insert
and update cells are new), so these tables are what the next CY measurement is
read against. PostgreSQL's floor was not measured for this shape.

## Gates

| gate (work order §6) | verdict | evidence |
|---|---|---|
| B equals A within noise on every cell (CY-R4) | **met** | every cell's p50 within 1.4 % of A and its p99 within 3.4 % (table A/B), against a control A2 (same binary as A) at 0.9 % p50 / 4.6 % p99; server CPU per statement equal to the decimal (33.1 against 32.8 us on the chain) |
| a point read, a tuple-key chain, a single-row INSERT and UPDATE do not move under B' (nothing on the write path, nothing before the chain's skip) | **met** | Bp against A, p50: point read -0.6 %, insert +0.6 %, update -1.2 %, chain +0.3 %; p99 +0.4 %, +2.4 %, -1.9 %, -0.1 %; all inside the A2 control's spread, and the per-round p50 ranges overlap |
| the statements S0 named run their flipped order under B' with S0's predicted gain | **moot** | CY-S0 found no flipped statement (`bench/v3.4.12/results-cy-s0-premise-v3.4.12-18-ge0cd5a93.md`); B' kept the written / planned order of both planner-sensitive cells (below) |
| CY-Q14 / CY-R10 bar at `cores = 1`: p50 and p99 within 5 % | **not met** | with a looping GATHER, p50 +3.1 % to +5.6 % (insert +5.6 %, over the bar), p99 +2.5 % to +13.2 % (point read +13.2 %, chain +9.9 %) on the four OLTP cells; the noise floor (OFF2) is 2.1 % p50 / 3.2 % p99 |
| CY-Q14 / CY-R10 bar at `cores > 1` with the gatherer at `SET PRIORITY background`: inside the noise floor | **not met** | `cores = 2`: p50 +2.9 % to +42.5 % (read +22.5 %, chain +42.5 %), p99 +9.7 % to +48.2 %; `cores = 4`: p50 +5.4 % to +40.9 % (read +21.1 %), p99 +3.4 % to +50.8 %; floor on the OLTP cells 2.1 % p50 / 2.9 % p99. In the cores = 2 diagnostic the OLTP session sat on core 1 and the gatherer's on core 0 |
| tests before/after | **not applicable** | no engine code was changed; the suite was not run for this measurement |
| `--verify` | **no such mode** | the join cells' result sets were compared across A, B, B' (identical multisets on all five read cells, `plans1.json`) |

## 1. The statement path: A, B, B' (cores = 1)

CY's statement-path cost is zero within what this rig resolves. Rows A and A2
are one binary measured twice; the difference between them is the floor, and B
and B' differ from A by no more than A2 does (p50 +/-1.4 % worst case at 40 us,
where one microsecond of resolution is 2.5 %). Percentiles are in us, QPS is
`ops / wall` of the serial connection, server CPU is the server process's
scheduled CPU per statement (`/proc/<pid>/task/*/schedstat`, every thread
including the WAL writer; the statement's delta over the cell).

| cell | engine | rounds | ops/round | p0 us | p25 us | p50 us | p75 us | p90 us | p99 us | QPS | server CPU us/stmt | p50 min..max | p50 vs A | p99 vs A |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| pk-point-read | A | 8 | 4000 | 38 | 39 | 40 | 41 | 45 | 56 | 24163 | 21.5 | 39..40 | - | - |
| pk-point-read | B | 8 | 4000 | 38 | 39 | 39 | 40 | 44 | 55 | 24603 | 21.0 | 39..39 | -1.4% | -1.1% |
| pk-point-read | A2 | 8 | 4000 | 38 | 39 | 39 | 41 | 44 | 54 | 24486 | 21.3 | 39..40 | -0.7% | -2.5% |
| pk-point-read | Bp | 8 | 4000 | 38 | 39 | 39 | 41 | 44 | 56 | 24383 | 21.3 | 39..40 | -0.6% | +0.4% |
| insert-1row | A | 8 | 4000 | 305 | 421 | 472 | 534 | 608 | 785 | 2047 | 66.4 | 464..514 | - | - |
| insert-1row | B | 8 | 4000 | 302 | 423 | 474 | 537 | 607 | 779 | 2032 | 66.7 | 453..498 | +0.6% | -0.8% |
| insert-1row | A2 | 8 | 4000 | 301 | 422 | 475 | 539 | 611 | 793 | 2037 | 65.9 | 453..505 | +0.6% | +1.1% |
| insert-1row | Bp | 8 | 4000 | 300 | 422 | 475 | 540 | 610 | 804 | 2036 | 66.5 | 459..512 | +0.6% | +2.4% |
| update-1row | A | 8 | 4000 | 308 | 423 | 476 | 539 | 616 | 791 | 2032 | 69.7 | 462..505 | - | - |
| update-1row | B | 8 | 4000 | 306 | 428 | 482 | 545 | 617 | 777 | 2007 | 71.6 | 461..515 | +1.2% | -1.7% |
| update-1row | A2 | 8 | 4000 | 305 | 422 | 473 | 538 | 605 | 770 | 2041 | 70.4 | 461..506 | -0.7% | -2.6% |
| update-1row | Bp | 8 | 4000 | 304 | 421 | 471 | 534 | 601 | 776 | 2052 | 69.7 | 465..497 | -1.2% | -1.9% |
| point-join-3rel | A | 8 | 4000 | 48 | 50 | 50 | 52 | 58 | 70 | 19187 | 32.8 | 50..51 | - | - |
| point-join-3rel | B | 8 | 4000 | 48 | 50 | 50 | 53 | 59 | 71 | 18967 | 33.1 | 50..51 | +0.0% | +2.0% |
| point-join-3rel | A2 | 8 | 4000 | 48 | 49 | 50 | 52 | 56 | 69 | 19247 | 32.6 | 50..51 | -0.5% | -1.4% |
| point-join-3rel | Bp | 8 | 4000 | 48 | 50 | 50 | 53 | 58 | 70 | 19016 | 33.1 | 50..52 | +0.3% | -0.1% |
| point-join-2rel | A | 8 | 4000 | 43 | 45 | 45 | 47 | 51 | 65 | 21137 | 27.9 | 45..46 | - | - |
| point-join-2rel | B | 8 | 4000 | 43 | 44 | 45 | 47 | 50 | 63 | 21441 | 27.5 | 45..46 | -0.6% | -3.4% |
| point-join-2rel | A2 | 8 | 4000 | 43 | 44 | 45 | 47 | 50 | 62 | 21348 | 27.6 | 45..45 | -0.5% | -4.6% |
| point-join-2rel | Bp | 8 | 4000 | 43 | 45 | 45 | 47 | 51 | 63 | 21268 | 27.8 | 45..46 | -0.1% | -4.0% |
| join-no-literal | A | 8 | 4000 | 844 | 902 | 931 | 975 | 1041 | 1157 | 1053 | 671.0 | 919..969 | - | - |
| join-no-literal | B | 8 | 4000 | 837 | 901 | 931 | 978 | 1043 | 1177 | 1053 | 670.4 | 922..939 | -0.0% | +1.7% |
| join-no-literal | A2 | 8 | 4000 | 841 | 908 | 940 | 985 | 1049 | 1201 | 1044 | 676.3 | 927..955 | +0.9% | +3.8% |
| join-no-literal | Bp | 8 | 4000 | 837 | 898 | 926 | 969 | 1034 | 1159 | 1060 | 665.7 | 918..950 | -0.6% | +0.2% |
| model-join | A | 8 | 4000 | 302 | 313 | 319 | 329 | 344 | 469 | 3070 | 168.4 | 314..327 | - | - |
| model-join | B | 8 | 4000 | 301 | 312 | 321 | 336 | 361 | 474 | 3015 | 171.7 | 313..324 | +0.5% | +1.0% |
| model-join | A2 | 8 | 4000 | 301 | 311 | 316 | 327 | 342 | 451 | 3081 | 166.8 | 314..329 | -0.8% | -3.8% |
| model-join | Bp | 8 | 4000 | 300 | 310 | 316 | 325 | 339 | 454 | 3100 | 165.2 | 314..331 | -1.1% | -3.4% |

What the gathered model did to the plans: B' is at plan-statistics generation
8 (eight publishes, one per relation) where A and B are at 0, and the two
planner-sensitive joins are priced from the managed rows - `join-no-literal`'s
`est` moved from `0:16,1:1` to `0:16,1:92` and `model-join`'s from `1:1,0:1000`
to `1:1,0:29` - without changing the order either takes (`order=written` and
`order=planned` on all three engines) or any result (identical multisets).
That is CY-R4 and CY-Q16 seen from the plan side: a gathered count prices, and
under `join_order = fixed` nothing flips unless a better count changes the
cheapest order, which on this workload it does not.

Waits (rule 3). A serial single connection: the client round trip is what the
server CPU does not cover. A point read is 21 us of server CPU in a 41 us
statement (about half the latency is client and socket); the INSERT and UPDATE
spend 66-70 us of server CPU in 490 us, and the rest is the `group` durability
wait for the log sync, which this run did not decompose further. No lock or
conflict wait occurs (one writer).

## 2. CY-Q14: a looping GATHER of `pb_bars` beside the OLTP cells

Method. One server (B). A second connection runs `SET PRIORITY background`
and then, while told to, loops `GATHER STATISTICS ON pb_bars` (published at
once) as fast as it is answered, from its own process pinned to its own CPU.
Conditions per cell, interleaved with the order rotated: **OFF** (that session
connected, background level set, idle), **ON** (it loops during the cell,
including the warm-up), **OFF2** (a second OFF, the noise floor). The
idle-but-connected session in OFF keeps the batch partition active, so OLTP
session placement is the same in all three conditions. The gather averaged
0.4-0.6 ms (637 leaves, 16 a slice) on an idle core and up to 2.7 ms when it
shared the core with an INSERT or UPDATE cell, and ran back to back: about 2,000
to 2,400 gathers per second beside the read cells and 400 beside the write
cells at `cores = 1` (2,750 per second at `cores = 2` and `4` on the write
cells), a continuous duty.
This is the heaviest reading of "in a loop"; it is not a paced gather.

Server CPU in ON includes the gatherer's own work (the process total), so its
rise is the gather's cost, not the statement's.

### cores = 1 (gather on the OLTP session's own core)

At one core the gather slices interleave with statements. The reads gain
+2 % to +4 % at p50 but their p99 gains +10 % to +13 %: consistent with a statement
landing behind a 16-leaf slice and waiting for it (not isolated). The write cells, 12 times slower
statements, gain +4 % to +6 % at p50 and +3 % at p99.

| cell | engine | rounds | ops/round | p0 us | p25 us | p50 us | p75 us | p90 us | p99 us | QPS | server CPU us/stmt | p50 min..max | p50 vs OFF | p99 vs OFF |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| pk-point-read | OFF | 8 | 4000 | 38 | 40 | 40 | 41 | 44 | 52 | 24215 | 23.0 | 40..41 | - | - |
| pk-point-read | ON | 8 | 4000 | 33 | 39 | 42 | 44 | 48 | 59 | 23380 | 41.0 | 40..43 | +3.1% | +13.2% |
| pk-point-read | OFF2 | 8 | 4000 | 38 | 40 | 40 | 41 | 44 | 53 | 24175 | 22.9 | 40..41 | +0.0% | +1.4% |
| insert-1row | OFF | 8 | 4000 | 298 | 421 | 472 | 538 | 611 | 793 | 2035 | 68.2 | 458..493 | - | - |
| insert-1row | ON | 8 | 4000 | 315 | 448 | 499 | 560 | 627 | 812 | 1943 | 130.5 | 492..570 | +5.6% | +2.5% |
| insert-1row | OFF2 | 8 | 4000 | 300 | 428 | 479 | 542 | 613 | 799 | 2019 | 68.1 | 456..496 | +1.4% | +0.8% |
| update-1row | OFF | 8 | 4000 | 306 | 428 | 483 | 551 | 623 | 794 | 1968 | 73.2 | 472..496 | - | - |
| update-1row | ON | 8 | 4000 | 320 | 451 | 503 | 569 | 635 | 816 | 1916 | 136.8 | 495..543 | +4.2% | +2.8% |
| update-1row | OFF2 | 8 | 4000 | 305 | 423 | 473 | 536 | 607 | 772 | 2041 | 72.1 | 465..570 | -2.1% | -2.8% |
| point-join-3rel | OFF | 8 | 4000 | 49 | 51 | 51 | 53 | 56 | 69 | 18898 | 35.0 | 51..52 | - | - |
| point-join-3rel | ON | 8 | 4000 | 44 | 50 | 53 | 56 | 61 | 76 | 18326 | 53.0 | 51..54 | +3.8% | +9.9% |
| point-join-3rel | OFF2 | 8 | 4000 | 49 | 51 | 51 | 52 | 55 | 68 | 19036 | 34.4 | 51..52 | -0.7% | -2.4% |
| point-join-2rel | OFF | 8 | 4000 | 44 | 46 | 46 | 47 | 50 | 62 | 21027 | 29.4 | 46..47 | - | - |
| point-join-2rel | ON | 8 | 4000 | 40 | 45 | 47 | 50 | 55 | 69 | 20659 | 47.0 | 45..49 | +2.1% | +10.7% |
| point-join-2rel | OFF2 | 8 | 4000 | 44 | 46 | 46 | 47 | 49 | 60 | 21255 | 29.3 | 46..47 | -0.2% | -3.2% |
| join-no-literal | OFF | 8 | 4000 | 834 | 877 | 906 | 948 | 1011 | 1127 | 1082 | 655.0 | 898..927 | - | - |
| join-no-literal | ON | 8 | 4000 | 832 | 874 | 893 | 919 | 949 | 1068 | 1107 | 874.1 | 878..931 | -1.5% | -5.2% |
| join-no-literal | OFF2 | 8 | 4000 | 833 | 872 | 899 | 939 | 1005 | 1120 | 1091 | 651.1 | 896..914 | -0.8% | -0.6% |
| model-join | OFF | 8 | 4000 | 300 | 308 | 313 | 322 | 336 | 465 | 3125 | 166.5 | 311..324 | - | - |
| model-join | ON | 8 | 4000 | 311 | 329 | 336 | 347 | 371 | 456 | 2905 | 322.5 | 333..339 | +7.3% | -1.8% |
| model-join | OFF2 | 8 | 4000 | 300 | 309 | 314 | 322 | 337 | 477 | 3101 | 167.5 | 312..323 | +0.2% | +2.7% |

### cores = 2 (CPUs 2,3; the gatherer's session on core 0, OLTP on core 1)

| cell | engine | rounds | ops/round | p0 us | p25 us | p50 us | p75 us | p90 us | p99 us | QPS | server CPU us/stmt | p50 min..max | p50 vs OFF | p99 vs OFF |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| pk-point-read | OFF | 8 | 4000 | 38 | 40 | 41 | 42 | 45 | 56 | 23740 | 24.5 | 41..41 | - | - |
| pk-point-read | ON | 8 | 4000 | 41 | 48 | 50 | 53 | 57 | 69 | 19540 | 78.4 | 49..51 | +22.5% | +23.7% |
| pk-point-read | OFF2 | 8 | 4000 | 38 | 40 | 41 | 42 | 44 | 54 | 23865 | 24.3 | 41..41 | -0.6% | -2.9% |
| insert-1row | OFF | 8 | 4000 | 334 | 468 | 527 | 595 | 668 | 850 | 1847 | 98.7 | 512..563 | - | - |
| insert-1row | ON | 8 | 4000 | 347 | 488 | 544 | 613 | 694 | 932 | 1773 | 597.9 | 537..592 | +3.2% | +9.7% |
| insert-1row | OFF2 | 8 | 4000 | 331 | 461 | 521 | 596 | 673 | 841 | 1847 | 96.6 | 500..545 | -1.1% | -1.0% |
| update-1row | OFF | 8 | 4000 | 343 | 482 | 538 | 608 | 684 | 867 | 1767 | 107.2 | 509..603 | - | - |
| update-1row | ON | 8 | 4000 | 353 | 497 | 554 | 622 | 696 | 957 | 1752 | 608.7 | 535..624 | +2.9% | +10.4% |
| update-1row | OFF2 | 8 | 4000 | 339 | 473 | 531 | 601 | 676 | 873 | 1824 | 103.9 | 517..596 | -1.4% | +0.7% |
| point-join-3rel | OFF | 8 | 4000 | 50 | 52 | 52 | 54 | 56 | 71 | 18592 | 36.9 | 52..53 | - | - |
| point-join-3rel | ON | 8 | 4000 | 54 | 70 | 75 | 80 | 86 | 105 | 13157 | 125.3 | 74..76 | +42.5% | +48.2% |
| point-join-3rel | OFF2 | 8 | 4000 | 50 | 52 | 53 | 54 | 56 | 71 | 18580 | 37.1 | 52..53 | +0.2% | +0.0% |
| point-join-2rel | OFF | 8 | 4000 | 44 | 47 | 47 | 48 | 51 | 63 | 20642 | 31.6 | 46..47 | - | - |
| point-join-2rel | ON | 8 | 4000 | 48 | 59 | 63 | 67 | 72 | 85 | 15624 | 103.0 | 62..65 | +33.3% | +33.8% |
| point-join-2rel | OFF2 | 8 | 4000 | 45 | 47 | 47 | 48 | 51 | 63 | 20764 | 31.4 | 47..48 | -0.3% | -1.0% |
| join-no-literal | OFF | 8 | 4000 | 902 | 950 | 979 | 1019 | 1086 | 1215 | 1004 | 740.4 | 967..982 | - | - |
| join-no-literal | ON | 8 | 4000 | 1456 | 1731 | 1789 | 1851 | 1912 | 2066 | 556 | 3072.6 | 1768..1820 | +82.8% | +70.1% |
| join-no-literal | OFF2 | 8 | 4000 | 901 | 949 | 976 | 1016 | 1084 | 1236 | 1004 | 739.2 | 967..983 | -0.3% | +1.7% |
| model-join | OFF | 8 | 4000 | 303 | 311 | 316 | 325 | 341 | 453 | 3095 | 172.5 | 315..320 | - | - |
| model-join | ON | 8 | 4000 | 358 | 403 | 420 | 449 | 492 | 602 | 2300 | 629.7 | 413..429 | +33.1% | +32.9% |
| model-join | OFF2 | 8 | 4000 | 303 | 311 | 316 | 325 | 343 | 482 | 3078 | 173.4 | 314..318 | +0.2% | +6.3% |

### cores = 4 (CPUs 2,3,6,7; same placement, two server cores per physical core)

| cell | engine | rounds | ops/round | p0 us | p25 us | p50 us | p75 us | p90 us | p99 us | QPS | server CPU us/stmt | p50 min..max | p50 vs OFF | p99 vs OFF |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| pk-point-read | OFF | 8 | 4000 | 39 | 41 | 41 | 42 | 45 | 55 | 23699 | 25.5 | 41..41 | - | - |
| pk-point-read | ON | 8 | 4000 | 41 | 47 | 50 | 52 | 56 | 68 | 19680 | 78.9 | 49..51 | +21.1% | +23.2% |
| pk-point-read | OFF2 | 8 | 4000 | 39 | 41 | 41 | 42 | 45 | 56 | 23700 | 25.5 | 41..42 | +0.1% | +0.8% |
| insert-1row | OFF | 8 | 4000 | 307 | 446 | 506 | 573 | 647 | 850 | 1917 | 108.7 | 494..556 | - | - |
| insert-1row | ON | 8 | 4000 | 317 | 474 | 534 | 604 | 678 | 879 | 1813 | 603.6 | 520..580 | +5.4% | +3.4% |
| insert-1row | OFF2 | 8 | 4000 | 297 | 443 | 500 | 569 | 645 | 847 | 1935 | 107.9 | 475..520 | -1.2% | -0.4% |
| update-1row | OFF | 8 | 4000 | 303 | 446 | 503 | 571 | 641 | 801 | 1903 | 113.6 | 490..568 | - | - |
| update-1row | ON | 8 | 4000 | 336 | 476 | 536 | 608 | 679 | 914 | 1803 | 612.3 | 520..585 | +6.6% | +14.2% |
| update-1row | OFF2 | 8 | 4000 | 309 | 440 | 497 | 568 | 640 | 830 | 1937 | 113.3 | 483..561 | -1.1% | +3.6% |
| point-join-3rel | OFF | 8 | 4000 | 50 | 52 | 53 | 54 | 57 | 71 | 18430 | 38.2 | 52..53 | - | - |
| point-join-3rel | ON | 8 | 4000 | 53 | 69 | 74 | 80 | 87 | 107 | 13159 | 125.6 | 73..77 | +40.9% | +50.8% |
| point-join-3rel | OFF2 | 8 | 4000 | 50 | 52 | 53 | 54 | 57 | 71 | 18442 | 38.4 | 52..53 | -0.0% | -0.1% |
| point-join-2rel | OFF | 8 | 4000 | 45 | 47 | 47 | 48 | 51 | 63 | 20625 | 32.7 | 47..48 | - | - |
| point-join-2rel | ON | 8 | 4000 | 48 | 60 | 63 | 68 | 73 | 86 | 15541 | 104.8 | 62..64 | +33.5% | +35.9% |
| point-join-2rel | OFF2 | 8 | 4000 | 45 | 47 | 47 | 49 | 52 | 65 | 20456 | 33.0 | 47..48 | +0.2% | +1.8% |
| join-no-literal | OFF | 8 | 4000 | 905 | 952 | 980 | 1020 | 1088 | 1211 | 1001 | 762.0 | 975..986 | - | - |
| join-no-literal | ON | 8 | 4000 | 1395 | 1751 | 1821 | 1895 | 1961 | 2098 | 546 | 3157.3 | 1784..1840 | +85.8% | +73.2% |
| join-no-literal | OFF2 | 8 | 4000 | 905 | 953 | 980 | 1021 | 1089 | 1223 | 1003 | 762.8 | 978..989 | -0.0% | +1.0% |
| model-join | OFF | 8 | 4000 | 303 | 313 | 318 | 330 | 349 | 486 | 3050 | 182.2 | 317..319 | - | - |
| model-join | ON | 8 | 4000 | 355 | 401 | 419 | 447 | 489 | 601 | 2309 | 636.5 | 414..420 | +31.6% | +23.6% |
| model-join | OFF2 | 8 | 4000 | 303 | 312 | 317 | 327 | 344 | 483 | 3067 | 180.7 | 315..319 | -0.4% | -0.7% |

The two multi-core runs agree to a few percent cell by cell (a second,
independent replication of one another), and the effect is not what the bar
expected: the OLTP statements are slowed with the gatherer on another core.
The gap is widest where the statement does the most small steps - the chain
+41 %, the two-relation join +33 %, `join-no-literal` +83 % to +86 % (p0 moves
from 905 to 1,395 us: every statement pays, not a tail) - and narrowest on the
writes, whose 500 us is mostly log wait. `join-no-literal` and `model-join` do
not read `pb_bars` at all.

### Diagnostics - what it is not (single runs, console output not archived)

`diag.py`, 2 cores, B, a 60,000-row `big` and a 100-row `small`, 3,000 reads of
a three-way join of `small` on the OLTP session per phase (idle, ON, OFF);
p50 / p99 in us:

| gatherer's loop | idle-first | ON | OFF after | note |
|---|---|---|---|---|
| `GATHER STATISTICS ON big`, tight loop (2 runs) | 115 / 167, 123 / 217 | 186 / 334, 218 / 368 | 103 / 152, 105 / 175 | OLTP session on core 1, gatherer's session on core 0 (`sys.meta` `core`, read from each session, 2nd run); +80 % to +108 % at p50 over OFF |
| `GATHER ... PENDING`, tight loop | 118 / 171 | 193 / 339 | 104 / 151 | `plan_stats_generation` stays 1: no publish, same slowdown |
| plain `SELECT * FROM big WHERE v = 0` at background, tight loop (264 scans of 1.5 ms) | 121 / 204 | 129 / 211 | 105 / 201 | a full walk of the same relation costs +23 % at p50, a quarter of what a GATHER does |
| `GATHER`, one every 10 ms (33 gathers in the window) | 117 / 168 | 111 / 209 | 104 / 167 | p50 +7 %, p99 +25 % over OFF |
| `GATHER`, one every 1 ms (261 gathers in the window) | 122 / 185 | 120 / 286 | 104 / 167 | p50 +15 %, p99 +71 % over OFF |

Three things follow. (1) **Each GATHER publishes a new plan-statistics
generation whether or not the count moved**: 1,633 generations for 1,636
gathers of an unchanged table, and the order memo's key carries the
generation (`plan-stats.md` PS19), so a looping gather invalidates every core's
memo continuously. That is not what slows the OLTP session: a `PENDING` gather,
which publishes nothing, slows it by the same amount. (2) The slowdown is not
generic batch-session interference either: a background full scan at 100 % duty
costs a quarter of it. (3) It scales with the gather rate. What is left of
the gather is its per-slice work (a relation `IS` borrow taken and given back,
the statement-epoch hold, a yield and its wake) done 20+ times per gather and
2,500+ gathers a second; which of them reaches the OLTP core was not isolated
in this budget. **Unresolved**; a profile of the OLTP core with the loop on
would name it.

## What was cut (the one-hour budget)

- The 200 / 1K / 10K row-count sweep (rule 9); one relation size set, one
  gathered relation.
- A paced gather in the formal matrix (only the single-run diagnostics above
  paced it); a gather of a relation larger than 60,480 rows.
- Cores 8; `strict` durability; the CY-S1 gate's "first planned statement
  after a restart" (a restart is not part of this run).
- `GATHER ... PENDING`, `PUBLISH` and the other relation-object statements'
  statement cost: only `GATHER` was looped.
- The PostgreSQL floor (rule 4: not measured for this shape); no suite run (no
  code change); the diagnostics' console output was not archived (the
  numbers above are transcribed from it, the drivers are in the archive).

## What the run teaches

CY's design promise held on the side it could be tested without a gatherer: a
managed model is read at the plan-statistics snapshot and costs nothing on a
statement that never touches the planner's size input, and nothing measurable
on one that does. The promise it made about the gatherer, CY-R10's "behind
OLTP", did not: at every core count, a gather running as fast as it is
answered slows the whole instance's reads, joins most. The mechanism has two
visible parts, a publish per gather that moves the generation with an
unchanged count (a gather of an unchanged table could leave the version where
it was, as PS19's hysteresis does for the band) and a per-slice cost that
`PENDING` does not remove. Neither is established as the cause; the paced runs
say the effect is rate-proportional, so an operator who gathers once a minute
sees none of it, and the bar's wording "in a loop" is what fails.
