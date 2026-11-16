# CS close: `BEGIN READ ONLY` costs nothing off its own path and saves the sync, the id and the log record on it

`BEGIN ... READ ONLY` at `adbea55b` adds no measurable cost to the statements that do not use it (CS-R5 met: server CPU B - A is inside the A2 - A noise floor for all three shapes, +0.08 to +0.27 us on 24 to 140 us), and a read-only transaction writes no WAL, issues no id and, under `strict`, pays no sync: at cores = 1, 1 connection, k = 1 it runs 4.7x the groups per second of a plain `BEGIN` (7,144 against 1,526). It does **not** track autocommit at k = 1: under `relaxed` it is indistinguishable from a read-write `BEGIN` in groups/s (6,838 against 7,390, inside the spread), because what an explicit transaction costs a relaxed client is its two extra statements, not its id (server-CPU supplement below: `BEGIN READ ONLY` + read + `COMMIT` costs 53.2 us of server CPU per group against `BEGIN` + read + `COMMIT` 55.0 us and a bare read 23.1 us). The expectation "R tracks A" holds only at k = 16, where 16 reads dominate the group. The work order's premise (`instructions/v3.2.0/workorder-cs-read-only-transaction.md` section 1: the id, the record and the slot are the cost) is borne out for `strict` and for WAL bytes, and is a 3% effect (1.8 us of 55 us, supplement) for `relaxed` CPU at k = 1.

| field | value |
|---|---|
| date/time executed | 2026-11-15 08:07 to about 08:26 UTC (build 08:05-08:07, Q1 about 08:09-08:12, Q2 08:12-08:22, supplement about 08:23-08:25) |
| branch | `worktree-cs-read-only-transaction` |
| arm A | `32354dbe`, `git describe --tags` = `v3.4.11-2-g32354dbe` (the commit CS opened from) |
| arm B | `adbea55b`, `git describe --tags` = `v3.4.11-6-gadbea55b` (CS-S1/S2 landed; the closing code) |
| tree | both arms built from `git archive` exports under `/home/louis/bench-runs/cs-close/src-A`, `src-B`, outside the worktree; the worktree was clean at the start of the run, so no uncommitted edit can be in either binary |
| binaries | Release (`-DCMAKE_BUILD_TYPE=Release`, GCC 14.2), built 08:07:14 (A) and 08:07:15 (B) UTC, after both commits (A's commit time 07:12, B's 08:04). Measured copies in `run/`: `kds_A` and `kds_A2` sha256 `93bfb49cee75c27de2fb1a54c99a4b7cdd1bc228446390ee4bb5b431bfebeae2` (byte-identical, and identical to the binary CS-S0 measured), `kds_B` sha256 `ce0d1aec37cfd3503d9c45f028f8e5bfd78ea82b7a9c4137e4f3a5129eb86983` (`binaries.sha256`). Q2 and the supplement run on `kds_B` alone |
| device | data files and WAL under `/home/louis/bench-runs/cs-close/data`, ext4 on `/dev/nvme0n1p1` (`df -T`), not tmpfs; fresh server and data file per run (Q1, supplement) or per (cores, durability) pair (Q2) |
| host | Intel Xeon Platinum 8581C, 8 CPUs; load average 6.0-7.3 at start (other sessions), about 3 during Q1 (recorded per run in `q1.json`), 2.0-7.9 (median 4.1) during Q2 (per cell in `q2.json`); `cc1plus` count 0 in every Q2 cell. **Not quiet**, so every delta below is read against an in-run control |
| server config | Q1 and supplement: `cores` 1, `durability` relaxed, server on CPU 2, driver on CPU 4. Q2: `cores` 1 (CPU 4) and 4 (CPUs 4-7), `durability` strict and relaxed, clients on CPUs 0-3. `buffer_pool_frames` = 65536, defaults otherwise |
| drivers | `tools/cs_close_overhead.py` (Q1), `tools/cs_close_matrix.py` (Q2, derived from `tools/cs_s0_premise.py`), `tools/cs_close_cpu.py` (supplement); archived under `archive/cs-close-v3.4.11-6-gadbea55b/scripts/`. `bench/docs/` does not exist at this commit; each driver's docstring carries its flags, and the commands are listed under "Commands" |
| tests | not executed: no engine code was changed by this measurement. No `--verify` mode exists; each read is checked to return one row and each UPDATE to return `UPDATED 1` (`bad` = 0 throughout; Q2 `err` empty in every cell) |

## Question 1 (CS-R5): the overhead off the read-only path is nil

Three servers on one host, one connection each, blocks of 500 units (166 for the transaction shape) rotated in arm order, 14 runs (a fresh server per run). Units: autocommit `SELECT * FROM t<n> WHERE id = k`; autocommit `UPDATE t<n> SET v = v + 1 WHERE id = k`; `BEGIN; 4 x SELECT; COMMIT` with a plain `BEGIN` (READ WRITE). The row count 200 / 1,000 / 10,000 selects the relation (`t200`, `t1000`, `t10000`, all three loaded in every server); a tuple-key lookup does not scale with rows and the three sizes are the evidence (A's server CPU on the SELECT 24.3, 24.6, 24.6 us). Units per cell per run: 3,000 (SELECT, UPDATE) and 996 (transaction), over 14 runs: 42,000 and 13,944 per arm per cell. A2 is a second server on a byte-identical copy of A. Server CPU is the sum of `/proc/<pid>/task/*/schedstat` on-CPU ns over the block, divided by the units.

Reading: B - A against A2 - A. A delta smaller than A2 - A's standard error is not a finding. Pooled over the three sizes (42 paired run-cells per shape; us of server CPU per unit):

| shape | A server CPU us | B - A (SE) | A2 - A (SE) | verdict |
|---|---|---|---|---|
| autocommit tuple-key SELECT | 24.52 | +0.08 (0.15) | -0.04 (0.13) | nil: B - A inside the noise floor |
| autocommit point UPDATE | 26.50 | +0.27 (0.14) | -0.02 (0.15) | nil to at most +1%: B - A is positive at all three sizes (+0.23 to +0.34) and pooled 1.9 SE, while A2 - A straddles zero; per size the run-to-run sd (0.7 to 1.2 us) cannot resolve it, so it is neither reported as a regression nor ruled out below 1% |
| BEGIN; 4 SELECT; COMMIT (READ WRITE) | 139.46 | -0.81 (0.86) | -0.29 (0.98) | nil: the noise exceeds the delta |

Per size, with the client latency distribution (microseconds; mean of the 14 run-level percentiles; the server CPU column is us per unit):

| shape | rows | arm | ops/run | runs | server CPU us/unit (mean of run means) | min..max | client mean us | p0 | p25 | p50 | p75 | p99 | bad |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| select-pk | 200 | A | 3000 | 14 | 24.32 | 20.58..30.86 | 61.0 | 45.9 | 50.7 | 53.7 | 61.8 | 144.1 | 0 |
| select-pk | 200 | B | 3000 | 14 | 24.00 | 20.56..30.72 | 60.5 | 45.5 | 50.6 | 53.4 | 61.3 | 141.2 | 0 |
| select-pk | 200 | A2 | 3000 | 14 | 24.20 | 20.36..30.72 | 60.9 | 45.7 | 50.6 | 54.0 | 61.7 | 142.5 | 0 |
| select-pk | 1000 | A | 3000 | 14 | 24.63 | 20.44..37.89 | 62.0 | 46.3 | 52.9 | 55.8 | 62.3 | 148.5 | 0 |
| select-pk | 1000 | B | 3000 | 14 | 25.09 | 20.76..38.35 | 62.7 | 46.7 | 53.2 | 55.8 | 62.5 | 152.1 | 0 |
| select-pk | 1000 | A2 | 3000 | 14 | 24.62 | 20.45..38.38 | 61.6 | 46.2 | 52.8 | 55.6 | 62.0 | 146.0 | 0 |
| select-pk | 10000 | A | 3000 | 14 | 24.62 | 20.86..40.05 | 62.3 | 46.6 | 52.9 | 55.9 | 62.5 | 138.5 | 0 |
| select-pk | 10000 | B | 3000 | 14 | 24.73 | 20.74..42.32 | 62.4 | 46.8 | 53.0 | 55.9 | 62.1 | 142.5 | 0 |
| select-pk | 10000 | A2 | 3000 | 14 | 24.64 | 20.97..40.13 | 61.8 | 46.6 | 52.5 | 55.6 | 62.2 | 140.7 | 0 |
| update-pk | 200 | A | 3000 | 14 | 26.48 | 21.94..41.98 | 54.7 | 37.8 | 45.2 | 47.7 | 54.3 | 146.3 | 0 |
| update-pk | 200 | B | 3000 | 14 | 26.82 | 22.00..41.46 | 55.0 | 37.7 | 45.3 | 48.0 | 55.0 | 149.0 | 0 |
| update-pk | 200 | A2 | 3000 | 14 | 26.67 | 22.03..41.08 | 54.9 | 37.8 | 45.2 | 47.9 | 54.1 | 153.6 | 0 |
| update-pk | 1000 | A | 3000 | 14 | 26.79 | 21.83..40.39 | 54.5 | 37.9 | 45.4 | 48.0 | 53.8 | 131.5 | 0 |
| update-pk | 1000 | B | 3000 | 14 | 27.02 | 22.06..40.78 | 55.0 | 37.7 | 45.8 | 48.3 | 54.4 | 129.9 | 0 |
| update-pk | 1000 | A2 | 3000 | 14 | 26.55 | 22.12..39.85 | 54.1 | 37.2 | 45.5 | 47.6 | 54.0 | 130.3 | 0 |
| update-pk | 10000 | A | 3000 | 14 | 26.23 | 22.62..39.49 | 53.3 | 38.0 | 44.8 | 47.5 | 53.4 | 124.7 | 0 |
| update-pk | 10000 | B | 3000 | 14 | 26.46 | 22.87..39.87 | 53.8 | 38.2 | 44.9 | 47.7 | 54.2 | 124.8 | 0 |
| update-pk | 10000 | A2 | 3000 | 14 | 26.21 | 22.76..39.98 | 53.5 | 38.0 | 44.8 | 47.5 | 53.6 | 119.1 | 0 |
| txn4-rw | 200 | A | 996 | 14 | 137.40 | 112.83..231.27 | 333.2 | 259.2 | 298.8 | 316.1 | 347.5 | 607.0 | 0 |
| txn4-rw | 200 | B | 996 | 14 | 135.47 | 113.50..232.60 | 332.6 | 259.5 | 298.2 | 315.1 | 347.4 | 600.7 | 0 |
| txn4-rw | 200 | A2 | 996 | 14 | 136.68 | 112.91..233.84 | 333.0 | 258.9 | 299.0 | 316.4 | 347.6 | 597.6 | 0 |
| txn4-rw | 1000 | A | 996 | 14 | 135.74 | 113.40..189.48 | 331.7 | 259.6 | 288.0 | 316.7 | 349.3 | 637.2 | 0 |
| txn4-rw | 1000 | B | 996 | 14 | 136.31 | 115.04..194.82 | 332.6 | 260.1 | 289.3 | 319.0 | 349.5 | 598.2 | 0 |
| txn4-rw | 1000 | A2 | 996 | 14 | 137.27 | 114.65..209.44 | 333.9 | 259.9 | 296.3 | 320.2 | 351.0 | 589.4 | 0 |
| txn4-rw | 10000 | A | 996 | 14 | 145.24 | 115.99..240.29 | 350.0 | 264.1 | 310.5 | 330.8 | 365.0 | 643.1 | 0 |
| txn4-rw | 10000 | B | 996 | 14 | 144.17 | 114.88..238.12 | 347.5 | 261.9 | 308.8 | 330.5 | 365.5 | 623.4 | 0 |
| txn4-rw | 10000 | A2 | 996 | 14 | 143.57 | 116.29..240.65 | 349.5 | 269.8 | 309.4 | 330.2 | 365.6 | 621.1 | 0 |

Deltas per size (the paired columns are mean +- sd over the 14 runs):

| shape | rows | B-A cpu us | A2-A cpu us | B-A client mean us | A2-A client mean us | paired B-A cpu (per-run) mean +- sd | paired A2-A cpu mean +- sd |
|---|---|---|---|---|---|---|---|
| select-pk | 200 | -0.32 | -0.12 | -0.5 | -0.1 | -0.32 +- 0.39 | -0.12 +- 1.06 |
| select-pk | 1000 | +0.46 | -0.01 | +0.7 | -0.3 | +0.46 +- 0.70 | -0.01 +- 0.54 |
| select-pk | 10000 | +0.11 | +0.02 | +0.0 | -0.5 | +0.11 +- 1.42 | +0.02 +- 0.85 |
| update-pk | 200 | +0.34 | +0.19 | +0.3 | +0.2 | +0.34 +- 1.08 | +0.19 +- 0.78 |
| update-pk | 1000 | +0.23 | -0.24 | +0.5 | -0.3 | +0.23 +- 1.03 | -0.24 +- 1.23 |
| update-pk | 10000 | +0.23 | -0.02 | +0.5 | +0.1 | +0.23 +- 0.67 | -0.02 +- 0.76 |
| txn4-rw | 200 | -1.93 | -0.72 | -0.6 | -0.1 | -1.93 +- 5.87 | -0.72 +- 5.20 |
| txn4-rw | 1000 | +0.57 | +1.53 | +1.0 | +2.3 | +0.57 +- 6.02 | +1.53 +- 6.98 |
| txn4-rw | 10000 | -1.07 | -1.66 | -2.5 | -0.4 | -1.07 +- 4.90 | -1.66 +- 6.75 |

The client-side deltas B - A are -2.5 to +1.0 us on a 48 to 350 us unit at every size, the same size as A2 - A (-0.5 to +2.3). The three branches CS adds (`SnapshotFor`, `BeginWrite`, the statement head's read-only gate) are below what a 14-run interleaved A/B on a shared host resolves: about 0.3 us on a 25 us statement (1%) for the point shapes and about 2 us on a 139 us group.

**Rule 3, waits.** Not decomposed by wait type: Q1 reads server on-CPU time and client latency only; `sys.wait_stats` was not read. At cores = 1 relaxed, one connection, the client mean exceeds the server CPU by 2.5x (SELECT: 61 against 24.5 us); the rest is the socket round trip and the client's own time, which this run does not split further.

## Question 2: the win

Relation `cs_t (id int64, a int64, b int64) BTREE`, 10,000 rows, reads `SELECT * FROM cs_t WHERE id = <random>`. A **group** is one unit: **R** = `BEGIN READ ONLY`, k reads, `COMMIT` (k + 2 round trips); **W** = plain `BEGIN`, k reads, `COMMIT`; **A** = k reads in autocommit (k round trips); **A2** = a second A arm, the noise control. All four arms run on binary B (R needs it; A and W on B so the arms differ by the statement stream alone). 4 repeats of 2.0 s per arm per cell, arm order rotated and alternately reversed per repeat, one client process per connection. The row-count sweep was cut (below).

Reading rule: spread = (max - min) / mean over the eight A and A2 runs of the cell; R "tracks" A when |R - A| / A is within it. Groups/s is the mean of the 4 repeats; A in the (R - A) / A column is the A + A2 mean.

| cores | dur | conns | k | R groups/s | W groups/s | A groups/s | A2 groups/s | (R-A)/A | (W-A)/A | spread (A,A2) | R reading | R read_only_begins per group | W read_only_begins |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | strict | 1 | 1 | 7144 | 1526 | 15895 | 15519 | -55% | -90% | 32% | R below A | 1.000 | 0 |
| 1 | strict | 1 | 16 | 847 | 545 | 885 | 1066 | -13% | -44% | 53% | R tracks A (inside spread) | 1.000 | 0 |
| 1 | strict | 8 | 1 | 22388 | 2153 | 48094 | 48084 | -53% | -96% | 4% | R below A | 1.000 | 0 |
| 1 | strict | 8 | 16 | 2492 | 1095 | 2707 | 2737 | -8% | -60% | 43% | R tracks A (inside spread) | 1.000 | 0 |
| 1 | relaxed | 1 | 1 | 6838 | 7390 | 15276 | 16504 | -57% | -53% | 20% | R below A | 1.000 | 0 |
| 1 | relaxed | 1 | 16 | 666 | 639 | 663 | 591 | +6% | +2% | 69% | R tracks A (inside spread) | 1.000 | 0 |
| 1 | relaxed | 8 | 1 | 17945 | 14551 | 33214 | 33520 | -46% | -56% | 97% | R tracks A (inside spread) | 1.000 | 0 |
| 1 | relaxed | 8 | 16 | 2337 | 2232 | 2491 | 2515 | -7% | -11% | 19% | R tracks A (inside spread) | 1.000 | 0 |
| 4 | strict | 1 | 1 | 6956 | 1212 | 14519 | 16085 | -55% | -92% | 36% | R below A | 0.000 | 0 |
| 4 | strict | 1 | 16 | 912 | 374 | 1056 | 1011 | -12% | -64% | 13% | R tracks A (inside spread) | 0.492 | 0 |
| 4 | strict | 8 | 1 | 26122 | 4201 | 60860 | 55143 | -55% | -93% | 39% | R below A | 0.258 | 0 |
| 4 | strict | 8 | 16 | 2786 | 2311 | 3186 | 3153 | -12% | -27% | 25% | R tracks A (inside spread) | 0.232 | 0 |
| 4 | relaxed | 1 | 1 | 7475 | 6850 | 17299 | 17213 | -57% | -60% | 14% | R below A | 0.505 | 0 |
| 4 | relaxed | 1 | 16 | 1103 | 1026 | 1169 | 1115 | -3% | -10% | 13% | R tracks A (inside spread) | 0.000 | 0 |
| 4 | relaxed | 8 | 1 | 23512 | 27098 | 56749 | 61450 | -60% | -54% | 28% | R below A | 0.229 | 0 |
| 4 | relaxed | 8 | 16 | 4047 | 3825 | 4378 | 4358 | -7% | -12% | 13% | R tracks A (inside spread) | 0.266 | 0 |

R is read-only in every cell. Evidence that does not depend on a counter: **WAL bytes per group are 0.00 to 0.10 for R, against 64.00 to 64.27 for W in every cell**, and at cores = 1 strict R's `wal_syncs` delta is 0.0000 to 0.0012 per group against W's 1.00. `read_only_begins` (`sys.meta`, section `txn`) is the direct counter and reads 1.000 per R group at cores = 1 and 0 for every W group in every cell. **At cores = 4 it reads 0.00 to 0.51 per R group, which is the counter's scope, not a failure of the path**: `sys.meta` carries the session's own core's fields, the driver's admin connection sits on one core, and R's sessions are spread across four, so the delta is the share of R sessions the admin's core served (I infer this from the cores = 1 result and from the WAL bytes; I did not read a counter per core). The same scope makes the `syncs/group` column valid at cores = 1 only. A cores = 4 proof by counter would need one read per core; this run does not have it, and the WAL bytes carry the claim there.

Latency per group, microseconds, mean of the four repeats' percentiles (A pools A and A2); `groups` is the cell's total over its four repeats; `WAL B/group` is the delta of `wal_append_lsn` over the groups:

| cores | dur | conns | k | arm | p0 | p25 | p50 | p75 | p99 (us) | groups (4 reps) | WAL B/group | syncs/group |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | strict | 1 | 1 | R | 110 | 128 | 138 | 145 | 186 | 57150 | 0.02 | 0.0001 |
| 1 | strict | 1 | 1 | W | 371 | 558 | 629 | 715 | 1047 | 12212 | 64.00 | 1.0003 |
| 1 | strict | 1 | 1 | A (A+A2) | 46 | 52 | 59 | 69 | 107 | 251316 | 0.00 | 0.0000 |
| 1 | strict | 1 | 16 | R | 918 | 1066 | 1253 | 1373 | 1655 | 6776 | 0.06 | 0.0012 |
| 1 | strict | 1 | 16 | W | 1255 | 1702 | 1869 | 2006 | 2409 | 4360 | 64.00 | 1.0009 |
| 1 | strict | 1 | 16 | A (A+A2) | 760 | 924 | 1047 | 1178 | 1466 | 15602 | 0.02 | 0.0005 |
| 1 | strict | 8 | 1 | R | 142 | 292 | 370 | 412 | 503 | 179100 | 0.00 | 0.0000 |
| 1 | strict | 8 | 1 | W | 687 | 2982 | 3572 | 4387 | 6473 | 17223 | 64.00 | 1.0002 |
| 1 | strict | 8 | 1 | A (A+A2) | 58 | 116 | 162 | 208 | 268 | 769427 | 0.00 | 0.0000 |
| 1 | strict | 8 | 16 | R | 1526 | 2515 | 3423 | 4061 | 5447 | 19936 | 0.02 | 0.0004 |
| 1 | strict | 8 | 16 | W | 3050 | 6238 | 7077 | 7839 | 10476 | 8760 | 64.00 | 1.0005 |
| 1 | strict | 8 | 16 | A (A+A2) | 1400 | 2256 | 3078 | 3665 | 4797 | 43548 | 0.01 | 0.0002 |
| 1 | relaxed | 1 | 1 | R | 111 | 124 | 140 | 156 | 223 | 54701 | 0.02 | 0.0001 |
| 1 | relaxed | 1 | 1 | W | 111 | 119 | 125 | 139 | 216 | 59121 | 64.00 | 0.0135 |
| 1 | relaxed | 1 | 1 | A (A+A2) | 46 | 51 | 59 | 67 | 112 | 254240 | 0.00 | 0.0000 |
| 1 | relaxed | 1 | 16 | R | 837 | 916 | 995 | 1225 | 6375 | 5326 | 0.07 | 0.0015 |
| 1 | relaxed | 1 | 16 | W | 904 | 1127 | 1245 | 1528 | 5382 | 5109 | 64.00 | 0.1509 |
| 1 | relaxed | 1 | 16 | A (A+A2) | 771 | 1084 | 1263 | 1541 | 5104 | 10032 | 0.04 | 0.0008 |
| 1 | relaxed | 8 | 1 | R | 151 | 287 | 375 | 423 | 3754 | 143559 | 0.00 | 0.0001 |
| 1 | relaxed | 8 | 1 | W | 155 | 301 | 398 | 461 | 4611 | 116405 | 64.00 | 0.0062 |
| 1 | relaxed | 8 | 1 | A (A+A2) | 55 | 117 | 152 | 213 | 4130 | 533872 | 0.00 | 0.0000 |
| 1 | relaxed | 8 | 16 | R | 1658 | 3006 | 3470 | 3854 | 5086 | 18698 | 0.02 | 0.0004 |
| 1 | relaxed | 8 | 16 | W | 1777 | 3167 | 3611 | 3992 | 5285 | 17854 | 64.00 | 0.0441 |
| 1 | relaxed | 8 | 16 | A (A+A2) | 1561 | 2789 | 3239 | 3605 | 4652 | 40046 | 0.01 | 0.0002 |
| 4 | strict | 1 | 1 | R | 95 | 126 | 135 | 145 | 334 | 55649 | 0.04 | 0.0000 |
| 4 | strict | 1 | 1 | W | 464 | 705 | 795 | 903 | 1385 | 9695 | 64.06 | 0.0000 |
| 4 | strict | 1 | 1 | A (A+A2) | 44 | 53 | 59 | 65 | 187 | 244833 | 0.01 | 0.0000 |
| 4 | strict | 1 | 16 | R | 815 | 916 | 1064 | 1186 | 1775 | 7300 | 0.10 | 0.0016 |
| 4 | strict | 1 | 16 | W | 1311 | 1595 | 1731 | 1913 | 72617 | 2995 | 64.27 | 0.6962 |
| 4 | strict | 1 | 16 | A (A+A2) | 685 | 832 | 913 | 1029 | 1576 | 16538 | 0.09 | 0.0012 |
| 4 | strict | 8 | 1 | R | 95 | 240 | 292 | 354 | 564 | 208977 | 0.00 | 0.0000 |
| 4 | strict | 8 | 1 | W | 528 | 1040 | 1182 | 1369 | 31525 | 33609 | 64.02 | 0.0000 |
| 4 | strict | 8 | 1 | A (A+A2) | 41 | 91 | 123 | 166 | 344 | 928024 | 0.00 | 0.0000 |
| 4 | strict | 8 | 16 | R | 801 | 1993 | 2607 | 3463 | 6915 | 22287 | 0.03 | 0.0000 |
| 4 | strict | 8 | 16 | W | 1431 | 2754 | 3281 | 3960 | 6578 | 18489 | 64.05 | 0.0000 |
| 4 | strict | 8 | 16 | A (A+A2) | 717 | 1719 | 2248 | 2997 | 6846 | 50710 | 0.03 | 0.0000 |
| 4 | relaxed | 1 | 1 | R | 94 | 117 | 122 | 131 | 279 | 59803 | 0.03 | 0.0001 |
| 4 | relaxed | 1 | 1 | W | 94 | 120 | 126 | 135 | 357 | 54801 | 64.01 | 0.0069 |
| 4 | relaxed | 1 | 1 | A (A+A2) | 41 | 50 | 52 | 57 | 138 | 276096 | 0.01 | 0.0000 |
| 4 | relaxed | 1 | 16 | R | 707 | 871 | 884 | 901 | 1243 | 8821 | 0.09 | 0.0005 |
| 4 | relaxed | 1 | 16 | W | 778 | 885 | 944 | 1015 | 1372 | 8211 | 64.09 | 0.0247 |
| 4 | relaxed | 1 | 16 | A (A+A2) | 699 | 812 | 828 | 875 | 1367 | 18274 | 0.08 | 0.0004 |
| 4 | relaxed | 8 | 1 | R | 92 | 210 | 264 | 343 | 1470 | 188098 | 0.00 | 0.0000 |
| 4 | relaxed | 8 | 1 | W | 94 | 223 | 277 | 346 | 617 | 216788 | 64.00 | 0.0009 |
| 4 | relaxed | 8 | 1 | A (A+A2) | 41 | 90 | 120 | 160 | 357 | 945596 | 0.00 | 0.0000 |
| 4 | relaxed | 8 | 16 | R | 782 | 1572 | 1954 | 2339 | 3349 | 32374 | 0.02 | 0.0000 |
| 4 | relaxed | 8 | 16 | W | 851 | 1764 | 2056 | 2381 | 3352 | 30600 | 64.03 | 0.0000 |
| 4 | relaxed | 8 | 16 | A (A+A2) | 748 | 1498 | 1776 | 2122 | 3123 | 69885 | 0.02 | 0.0000 |

What the matrix shows:

- **Strict: R is the win, and it is the sync.** At cores = 1, 1 connection, k = 1: R 7,144 groups/s against W 1,526 (4.7x), p50 138 against 629 us; 8 connections 22,388 against 2,153 (10x); k = 16 847 against 545 (1.6x) and 2,492 against 1,095 (2.3x). At cores = 4: 6,956 against 1,212 (5.7x), 26,122 against 4,201 (6.2x), k = 16 912 against 374 and 2,786 against 2,311. W's strict cost is the commit's device sync (1.00 per group at cores = 1); R has none.
- **R against A at k = 1 is the round trips, not the engine.** R is 46% to 60% below A in all eight k = 1 cells and outside the spread in seven (A's p50 is 52 to 59 us at one connection, R's 122 to 140: three round trips against one; with reads under 100 us the two extra statements are the whole difference). The expectation that R tracks A does not hold at k = 1 and cannot while `BEGIN` and `COMMIT` are statements of their own.
- **At k = 16 R tracks A in all eight cells** (-13% to +6%, each inside its spread of 13% to 69%): 16 reads of about 50 us dominate the group and the two extra statements are 1 to 6% of it.
- **Relaxed: R and W are indistinguishable in groups/s.** R against W at relaxed is -13% to +23% across the eight cells, every one inside the spread; in the one cell where R's p50 is the larger (cores = 1, 1 connection, k = 1: 140 against 125 us) the 15 us is inside the cell's p75 - p25 width. With no sync to save, a read-write group's id, `TXN_BEGIN`/`TXN_COMMIT` and slot are not a cost this matrix can see; the supplement sizes it.

## Supplement: where the relaxed group's time goes

One server (binary B, cores = 1, relaxed, CPU 2), 10 runs each on a fresh server, A / W / R rotated in blocks on one connection; server CPU from `schedstat`. k = 1: 3,000 groups per arm per run; k = 16: 300.

| k | arm | runs | groups/run | server CPU us/group (mean) | min..max | client mean us | p0 | p25 | p50 | p75 | p99 | bad |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | A | 10 | 3000 | 23.13 | 22.0..24.7 | 54.4 | 47 | 50 | 51 | 53 | 103 | 0 |
| 1 | W | 10 | 3000 | 55.00 | 53.2..61.4 | 131.2 | 111 | 118 | 122 | 129 | 220 | 0 |
| 1 | R | 10 | 3000 | 53.18 | 51.8..57.7 | 130.1 | 111 | 118 | 120 | 127 | 216 | 0 |
|  | W-A paired server CPU | | | +31.87 us | sd 2.01 | | | | | | | |
|  | R-A paired server CPU | | | +30.05 us | sd 1.43 | | | | | | | |
|  | R-W paired server CPU | | | -1.82 us | sd 0.79 | | | | | | | |
| 16 | A | 10 | 300 | 362.67 | 356.5..373.2 | 843.8 | 767 | 799 | 812 | 855 | 1244 | 0 |
| 16 | W | 10 | 300 | 403.15 | 395.7..421.8 | 918.6 | 835 | 873 | 887 | 916 | 1324 | 0 |
| 16 | R | 10 | 300 | 393.79 | 382.5..406.9 | 925.2 | 837 | 870 | 885 | 938 | 1498 | 0 |
|  | W-A paired server CPU | | | +40.48 us | sd 6.04 | | | | | | | |
|  | R-A paired server CPU | | | +31.12 us | sd 5.45 | | | | | | | |
|  | R-W paired server CPU | | | -9.36 us | sd 6.56 | | | | | | | |

Two statements (`BEGIN`, `COMMIT`) cost the server about 30 us of CPU per group (R - A = +30.05 us at k = 1, +31.1 at k = 16), about 15 us a statement of parse, dispatch and reply. What a read-write transaction adds over a read-only one is 1.8 us at k = 1 (R - W = -1.82 us, sd 0.79 over 10 paired runs: resolvable, 3% of W's 55 us group) and 9.4 us at k = 16 (sd 6.56, SE 2.1: resolvable, 2.3% of 403 us). That W's extra grows with k, while R - A stays at +30 to +31 whatever k is, says that R's reads cost the server what autocommit reads do and W's reads cost about 0.6 us each more; the run does not isolate why (a per-statement view or id check a held read-only view skips is the candidate, not measured). **The engine's id, record and slot are about 2 us of 55 at k = 1; the statements around them are 30.** Client p50 (122 against 120 us at k = 1) moves with it by under a microsecond.

## Against CS-S0's file

`bench/v3.4.11/results-cs-s0-premise-v3.4.11-3-gbbaff33a.md` measured W against A on binary A (`32354dbe`, the same binary as here) under host load 11 to 21 with 0 to 16 `cc1plus` processes, 2 repeats of 2.0 s. The same cells here run W and A on binary B under load 2 to 8 and no concurrent build. Absolute groups/s are 2 to 5x higher here (A at cores = 1 strict one connection k = 1: 2,924 there, 15,707 here) and are not comparable; the ratio (W - A) / A is the quantity that carries over:

| cores | dur | conns | k | S0 W | now W (B) | S0 A | now A (B) | S0 (W-A)/A | now (W-A)/A |
|---|---|---|---|---|---|---|---|---|---|
| 1 | strict | 1 | 1 | 562 | 1526 | 2924 | 15707 | -81% | -90% |
| 1 | strict | 1 | 16 | 156 | 545 | 212 | 975 | -26% | -44% |
| 1 | strict | 8 | 1 | 1557 | 2153 | 10918 | 48089 | -86% | -96% |
| 1 | strict | 8 | 16 | 475 | 1095 | 673 | 2722 | -29% | -60% |
| 1 | relaxed | 1 | 1 | 3122 | 7390 | 9089 | 15890 | -66% | -53% |
| 1 | relaxed | 1 | 16 | 229 | 639 | 291 | 627 | -21% | +2% |
| 1 | relaxed | 8 | 1 | 6831 | 14551 | 15652 | 33367 | -56% | -56% |
| 1 | relaxed | 8 | 16 | 1621 | 2232 | 2413 | 2503 | -33% | -11% |
| 4 | strict | 1 | 1 | 849 | 1212 | 9588 | 15302 | -91% | -92% |
| 4 | strict | 1 | 16 | 191 | 374 | 540 | 1034 | -65% | -64% |
| 4 | strict | 8 | 1 | 1872 | 4201 | 18971 | 58002 | -90% | -93% |
| 4 | strict | 8 | 16 | 484 | 2311 | 1160 | 3169 | -58% | -27% |
| 4 | relaxed | 1 | 1 | 3628 | 6850 | 5661 | 17256 | -36% | -60% |
| 4 | relaxed | 1 | 16 | 407 | 1026 | 374 | 1142 | +9% | -10% |
| 4 | relaxed | 8 | 1 | 7802 | 27098 | 17692 | 59100 | -56% | -54% |
| 4 | relaxed | 8 | 16 | 1146 | 3825 | 1350 | 4368 | -15% | -12% |

The sign agrees in 14 of 16 cells; the two that differ are k = 16 relaxed one connection (cores = 1: S0 -21%, here +2%; cores = 4: S0 +9%, here -10%), each inside its spread in both runs. W's strict penalty at k = 1 is larger here (-90% to -96% against -81% to -91%), plausibly because A got faster on the quiet host while a device sync did not; I did not test that.

## What the run teaches about the engine

- CS adds nothing the off-path statements can feel: pooled B - A for a tuple-key SELECT is +0.08 us on 24.5 (SE 0.15).
- A read-only transaction removes exactly what the work order said it would: 64 WAL bytes, one id and one sync per group. Under `strict` that is the dominant term (W p50 629 us against 125 us under relaxed on this device, so a sync is about 500 us against a 60 us read).
- A transaction's fixed cost on a `relaxed` server is its statements, not its bookkeeping: about 30 us for `BEGIN` + `COMMIT` against 1.8 us for what the read-write path adds at k = 1. That is the number behind "R does not track A at k = 1". Reducing the per-statement cost of `BEGIN`/`COMMIT`, or a form of transaction that does not spend two statements, is the lever for an explicit relaxed read transaction; the read-only flag is not.
- Nothing in this run approached a limit of the read-only path itself: Q2's cells are small-k reads on one relation at up to 8 connections, bound by the clients' round trips and, under `strict`, by the sync. The view and lease R holds for the life of the transaction were not stressed (no long-held R against a writer's purge or a checkpoint); this measurement says nothing about that.

## What was cut

- **Row sweep 200 / 1K / 10K** in Q2 and the supplement: 10,000 rows only (a pk lookup does not scale with rows; Q1 shows the three sizes and A's cost is flat across them).
- **k = 4** (S0 had 1, 4, 16), **`group` durability** (S0 had it) and S0's `--id-probe`: Q2 runs k in {1, 16} and `strict` / `relaxed` only, as asked.
- **Repeats**: Q2 4 x 2.0 s per arm per cell (S0 2 x 2.0), so a spread over 8 A and A2 runs; the 8-connection cells' spreads of 4% to 97% are the host's noise.
- **A2 for the supplement**: it has no byte-identical control, so R - W = -1.82 us carries its paired sd only. It measures server CPU, not client throughput.
- **Wait-type decomposition** (rule 3): not done beyond the sync / statements split above; no `sys.wait_stats`.
- **PostgreSQL floor** not measured for these shapes. **Baseline**: for the Q1 shapes the previous kdbs number is arm A on `32354dbe` here (no earlier results file exists for them); for the W / A cells it is CS-S0's file above.
- **Per-core `read_only_begins`** at cores = 4 not read; WAL bytes carry the claim.
- A 6-run shake-down of Q1 (`q1-shakedown.json`, same driver) ran first to check the harness; it is not used above, and its B - A server-CPU deltas (-0.14 to +1.19 us) show the same picture.
- Not run: the `tests/` suite (no engine code changed).

## Commands

```
cmake -S src-B -B build-B -DCMAKE_BUILD_TYPE=Release && cmake --build build-B --target kds_server -j4   # likewise src-A
python3 tools/cs_close_overhead.py --arm A=run/kds_A --arm B=run/kds_B --arm A2=run/kds_A2 --workdir data --out q1.json --progress q1.hb --runs 14
python3 tools/cs_close_matrix.py --bin run/kds_B --workdir data --out q2.json --seconds 2 --reps 4 --heartbeat q2.hb --status q2.status
python3 tools/cs_close_cpu.py --bin run/kds_B --workdir data --out cpu.json --runs 10
```

Raw files: `bench/v3.4.11/archive/cs-close-v3.4.11-6-gadbea55b/` (`q1.json`, `q2.json`, `cpu.json`, logs, analysis scripts, `binaries.sha256`; no data files). A 60 s watchdog polled `q2.hb` (a file the driver rewrites at every cell) and found no stall.
