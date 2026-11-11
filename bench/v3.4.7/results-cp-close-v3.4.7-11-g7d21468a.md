# CP close measurement: the slice `IS` deletion is a 82-99 ns per leaf page gain, the `U` mode and everything else sit inside the spread, and no wait became a refusal

**Thesis.** Milestone CP changed two things in the lock family: CP-S2a deleted the slice
unit (a positioned reader no longer takes an `IS` at every leaf page), CP-S2b added the `U`
mode (an `UPDATE` asks `U`; `DELETE` and `INSERT` keep `X`; a foreign key child's `S` on a
parent row is admitted under a parent `UPDATE`'s `U`). Measured as an interleaved A/B of
the opening build (`cc54a296`, build inputs of `v3.4.7`) against the closing build
(`7d21468a`) on one host, the deletion is visible exactly where S0 said it would be: a
full scan of 603 leaf pages costs **82-99 ns less per page** on the closing build, 2.5-2.8 %
of that scan's server CPU (8.2-8.8 % of a wide-row scan's), with the point controls inside
0.30 µs of zero (M2). Everything else moved by less than this run can resolve: a single-session point `UPDATE`
reads +1.1 µs at p50 (about 2 %, standard error about 1 µs, and the sign flipped when the
driver's two label roles were swapped, M1); scenario 2's committed TPS is equal to 0.02 %
(M3); scenario 0 reads -0.45 % over three runs per binary (M3: the close runs span 5.5 %, the base runs 2.0 %, so the delta is inside the run-to-run spread); eight sessions on one hot row read the same refusals and the same waits (M4); a
standing range fence costs the same ~1 µs per uncovered point write (M5). The `U` mode
shows up in `sys.lock_waits` exactly as designed - every `UPDATE` wait on the closing
build is mode `U`, every one on the opening build mode `X` - at the same count and the same
~100 µs mean. **No wait was turned into a refusal**: 0 deadlocks, 0 `fault_net`, 0
cancelled in every closing-build run whose views were read (M3 x5, M4, M1 x6), every parked ask granted, and the only
client-visible refusal is the same `TXN_CONFLICT ... was written by transaction N` of the
first-updater-wins rule the opening build gives. The one class CP-Q10 (a) accepts - a
parent updater granted `U` over a child writer's share - is not reachable by any cell here
(neither scenario shares a parent between writers), so the run neither exercised nor
refuted it.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-11-11 (UTC). Smoke 09:48; follow-up scenario-0 pair (base first) 11:42:17-11:44:34; main pass 09:49:19-10:04:41 (M1, M4, M2, M3 x6, M5 on close); second pass 10:05-10:10:09 (M5 on base and repeats, M3 scenario 0 second pair, scenario 2 third pair); M1 label-swapped repeat 10:11:09-10:16:27. About 27 minutes of measurement inside the 40-minute budget |
| Worktree / branch | `/home/louis/ckdbs/.claude/worktrees/worktree-cp-lock-model-exploration`, `worktree-worktree-cp-lock-model-exploration` |
| Commit | `7d21468a` = `v3.4.7-11-g7d21468a`, committed 2026-11-11 09:46:40 UTC. Tree clean (`git status` empty) |
| Opening build (**base**) | `cc54a296` = `v3.4.7-1-gcc54a296` (documents only; build inputs of `v3.4.7`, `6c4b9a6d`). Release, built 2026-11-11 from a `git worktree` of that commit, source `.../scratchpad/cp-base/build-release/kds_server` mtime 09:40:13 UTC. Copy `kds_server-base`, sha256 `d78b1147d6196cb08c53cda378105fecbb3dd352ba5c7ec2dd03e6ef3bc9a1d4`, 7,604,736 bytes (copied 09:47:23 UTC), **byte-identical to CP-S0's base binary** (same sha256 as `results-cp-s0-premise-v3.4.7-1-gcc54a296.md` §1) |
| Closing build (**close**) | `7d21468a`, Release (`CMAKE_BUILD_TYPE=Release`), source `build-release/kds_server` of this worktree, mtime 09:47:23 UTC (after the commit's 09:46:40 UTC; the commit's last delta over `f5d310ec` is documents). Copy `kds_server-close`, sha256 `049f27e66219f684588e0b40543512ef2b31437243aca2cedbb13d257eee4ecb`, 7,604,344 bytes. Both hashes re-computed before the first cell |
| Binary discipline | every server of every cell started from the two copies under `.../scratchpad/cp-close/bin/`; the build trees were not touched during the run |
| Device | data files and WALs under `/home/louis/bench-runs/cp-close/20261111T094919Z/` on `/dev/nvme0n1p1`, ext4 (`df -T`); `/tmp` (tmpfs) held only the binaries and the outputs |
| Host | 8 CPUs (Intel Xeon Platinum 8581C @ 2.10 GHz, SMT pairs 0/4 1/5 2/6 3/7), Linux 6.12.113. Servers `taskset` 2-5, drivers 6-7 (M1, M4), 0,1,6,7 (M3), 0,1 (M5); M2 server CPU 2, driver CPU 3. `host.log` (every 2 s, 456 + 145 + M1-swap lines) shows `procs=0` - no compiler, `cmake --build` or test process - in every sample; 1-minute load 0.87-5.43 (the servers' reactors spin; the 5.15 15-minute figure at 09:49 is the Debug build that finished at 09:46) |
| Server config | `cores = 4` (M2: `cores = 1`), `buffer_pool_frames = 65536`, `ash_enabled = on`; `durability = relaxed` for M1, M2, M4, M5, `group` for M3; M3 `ash_interval_ms = 100`. A fresh server and volume per cell. Ports 15520-15525 (M1, M4), 25731-25742 (M3), 32611-32621 (M2), 15561-15562 (M5) |
| Drivers | unmodified `tools/lock_contention_benchmark.py` (M1, M4), `tools/bm_scan.py census` (M2), `tools/scenario2_freight.py`, `tools/scenario0_stockmarket.py` (M3), `tools/cp_s0_fence.py` (M5); the five existing drivers are byte-unchanged between `cc54a296` and `7d21468a` except the new `cp_s0_*` files. Flags: [Reproduce](#11-reproduce) |
| Build type | Release |
| Prior number | **CP-S0, `bench/v3.4.7/results-cp-s0-premise-v3.4.7-1-gcc54a296.md`** (the same shapes, the same drivers, the same base binary). Every cell below carries a delta against it. PostgreSQL floor: **not measured for any of these shapes** |
| Raw outputs | `archive/cp-close-v3.4.7-11-g7d21468a/` (6.8 MB; no data file, WAL or server log) |
| Tests | no engine code changed by this measurement; **no test suite was run by it** (the suite is the milestone's step, not this cell's) |

## 2. What was measured

| Cell | Question | Driver, arms | Size |
|---|---|---|---|
| M1 | what the `U` mode and the slice deletion cost a single-session point `UPDATE` | `lock_contention_benchmark.py --sessions 1`, base against close, interleaved over 4 blocks; **6 reps**: 3 with base the driver's primary label, 3 with the roles swapped | 256 rows; 40,000 ops/arm/rep |
| M2 | the gain: the slice `IS` per leaf page | `bm_scan.py census --arm base= --arm close=`, 5 runs x 12 blocks | 100,000 rows (603 leaf pages), 10,000 wide rows (304); 768 scans, 3,000 point statements per cell |
| M3 | waits and throughput under the scenarios; `U` rows | `scenario2_freight.py --bookers 8` (3 runs per binary), `scenario0_stockmarket.py --fk --txn` (3 runs per binary) | scenario 2: 300 organizations, 30 ships, 300 operations, 24,000 cargos; scenario 0: 2,000 users, 2,000 assets, 8 traders |
| M4 | eight sessions on one hot row: refusals, waits | `lock_contention_benchmark.py --sessions 8`, base against close | 256 rows; 4,000 ops/session/arm |
| M5 | a standing range fence's cost to uncovered point writers (the held `UPDATE ... WHERE id > 4080` is a range `U` on close) | `cp_s0_fence.py`, fenced against unfenced, 6 blocks; base and close, 2 runs each | 4,096 rows; 1,000 ops per writer per block |
| M6 | not re-run | | |

## 3. M2 - the gain: the slice `IS` is 82-99 ns per leaf page, 2.5-8.8 % of a scan

`bm_scan.py census` at `cores = 1` (no partition latch exists at one core), base against
close, 5 runs of 12 blocks; values are the median over runs with [min, max]. Same page
counts (603 and 304), same row digest on both builds (the sanity arm and the census agree;
0 bad scans). The closing binary is the build in which the slice is deleted; the opening
binary is S0's, and S0's "no-slice" build was the same deletion done by a patch.

| Path | Scan server CPU, base | Scan server CPU, close | Δ per statement | Δ per leaf page | Δ per row | Share of base | Point control Δ per stmt |
|---|---|---|---|---|---|---|---|
| rows-100k (603 pages), literal | 2,155.7 [2,150.3, 2,189.8] µs | 2,096.2 [2,071.4, 2,227.3] µs | 59.8 [-37.5, 82.9] µs | **99 [-62, 138] ns** | 0.60 ns | 2.78 [-1.71, 3.85] % | -0.08 [-3.66, 0.52] µs |
| rows-100k, prepared | 2,151.2 [2,132.5, 2,190.2] µs | 2,103.5 [2,077.2, 2,114.5] µs | 54.0 [41.6, 75.6] µs | **90 [69, 125] ns** | 0.54 ns | 2.50 [1.94, 3.45] % | -0.29 [-3.64, 0.46] µs |
| wide-a (304 pages), literal | 310.7 [306.4, 314.5] µs | 284.7 [281.2, 288.4] µs | 27.6 [21.7, 29.3] µs | **91 [71, 96] ns** | 2.76 ns | 8.77 [7.08, 9.42] % | -0.30 [-0.68, 0.33] µs |
| wide-a, prepared | 305.5 [302.1, 311.9] µs | 280.7 [279.6, 284.1] µs | 24.9 [21.4, 29.8] µs | **82 [70, 98] ns** | 2.49 ns | 8.16 [7.04, 9.63] % | -0.12 [-0.37, 0.16] µs |

| Path | Wall p50 base | Wall p50 close | Wall p90 base / close | Wall p99 base / close |
|---|---|---|---|---|
| rows-100k, literal | 2,226.4 µs | 2,181.3 µs | 2,461.0 / 2,334.6 µs | 3,599.5 / 3,702.5 µs |
| rows-100k, prepared | 2,221.6 µs | 2,169.6 µs | 2,402.4 / 2,358.8 µs | 3,111.2 / 3,109.8 µs |
| wide-a, literal | 382.3 µs | 355.1 µs | 470.8 / 441.5 µs | 664.9 / 652.3 µs |
| wide-a, prepared | 373.1 µs | 348.4 µs | 448.4 / 430.1 µs | 619.9 / 642.9 µs |

(The census reports wall p50/p90/p99 over the pooled scans, not p0/p25; 192 scans per path per run for rows-100k and 768 for wide-a, 5 runs.) On the rows-100k literal path the lowest run (-37.5 µs) crosses zero (S0 had the same on its prepared path); the prepared 100k cell and both wide-row cells
have their whole [min, max] above zero.

**Delta against S0.** The same cell, the same host arrangement, the no-slice build against
the closing build:

| Path | S0 base scan CPU | this run base | S0 Δ per page (base - noslice) | this run Δ per page (base - close) | S0 point control | this run point control |
|---|---|---|---|---|---|---|
| rows-100k, literal | 2,155.2 µs | 2,155.7 µs | 88 [63, 113] ns | 99 [-62, 138] ns | +0.35 µs | -0.08 µs |
| rows-100k, prepared | 2,168.3 µs | 2,151.2 µs | 99 [-45, 122] ns | 90 [69, 125] ns | +0.08 µs | -0.29 µs |
| wide-a, literal | 326.7 µs | 310.7 µs | 126 [90, 175] ns | 91 [71, 96] ns | -0.09 µs | -0.30 µs |
| wide-a, prepared | 314.9 µs | 305.5 µs | 94 [75, 132] ns | 82 [70, 98] ns | +0.13 µs | -0.12 µs |

The base binary is the same bytes and its scan CPU repeats to 0.02 % on rows-100k literal
(2,155.2 → 2,155.7 µs), to 0.8 % on rows-100k prepared and runs 3-5 % lower on the two wide-a paths, so the host's per-core speed on the scan
path did not move between S0 and now. The closing build's scan CPU (2,096.2, 2,103.5,
284.7, 280.7 µs) lands within 0.6 % of the S0 no-slice patch (2,105.0, 2,115.2, 285.8,
279.7 µs): the deletion CP-S2a shipped costs the scan what the patch cost, and gains the
same 82-99 ns per page (S0: 88-126). Not measured: the latched slice above one core
(`bm_scan.py` has no `cores` flag; a driver change inside a measurement is not allowed).

## 4. M1 - the `U` mode and the slice deletion cost a point `UPDATE` nothing this host can resolve

One session, autocommit `UPDATE ... WHERE id = k` on its own row, `relaxed`. Of the six
reps all but one have both labels on peer cores (rep 2 of the swapped batch has base on core 0,
which carries the scheduler ticks: flagged by the driver's own check, shown, not used). With one session,
`update-hot`, `update-disjoint` and `update-disjoint-again` are the same statement; they
are nine samples per rep-batch of one comparison.

Batch 1 (base the driver's primary label, run first in even blocks), the three update arms and the
`insert-omitted`, `select-hot` controls, 40,000 ops per arm:

| Rep (cores base / close) | Arm | Binary | ops | p0 | p25 | p50 | p95 | p99 | mean | QPS |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 (3 / 1) clean | update-disjoint | base | 40,000 | 35.0 µs | 47.7 µs | 52.4 µs | 98.4 µs | 141.4 µs | 58.7 µs | 16,734 QPS |
| 1 | update-disjoint | close | 40,000 | 35.6 µs | 52.5 µs | 55.0 µs | 118.8 µs | 147.3 µs | 63.7 µs | 15,382 QPS |
| 1 | update-hot | base | 40,000 | 37.2 µs | 45.5 µs | 53.6 µs | 114.1 µs | 170.3 µs | 61.7 µs | 15,937 QPS |
| 1 | update-hot | close | 40,000 | 36.8 µs | 49.1 µs | 53.6 µs | 115.2 µs | 171.8 µs | 62.2 µs | 15,788 QPS |
| 1 | insert-omitted | base | 40,000 | 35.1 µs | 54.2 µs | 64.0 µs | 133.4 µs | 192.0 µs | 78.9 µs | 12,492 QPS |
| 1 | insert-omitted | close | 40,000 | 35.1 µs | 48.8 µs | 55.3 µs | 124.2 µs | 177.6 µs | 71.5 µs | 13,765 QPS |
| 1 | select-hot | base | 40,000 | 48.1 µs | 67.7 µs | 86.9 µs | 179.9 µs | 242.8 µs | 98.6 µs | 10,000 QPS |
| 1 | select-hot | close | 40,000 | 48.7 µs | 72.5 µs | 85.0 µs | 181.0 µs | 243.4 µs | 99.9 µs | 9,863 QPS |
| 2 (3 / 1) clean | update-disjoint | base | 40,000 | 36.0 µs | 43.8 µs | 51.6 µs | 101.5 µs | 147.6 µs | 59.9 µs | 16,372 QPS |
| 2 | update-disjoint | close | 40,000 | 38.6 µs | 53.1 µs | 58.0 µs | 118.1 µs | 161.4 µs | 67.5 µs | 14,501 QPS |
| 2 | update-hot | base | 40,000 | 36.9 µs | 45.2 µs | 55.4 µs | 121.0 µs | 178.6 µs | 63.7 µs | 15,426 QPS |
| 2 | update-hot | close | 40,000 | 38.5 µs | 52.4 µs | 56.9 µs | 123.5 µs | 174.1 µs | 67.2 µs | 14,604 QPS |
| 3 (3 / 3) clean | update-disjoint | base | 40,000 | 36.7 µs | 42.6 µs | 51.3 µs | 129.5 µs | 183.2 µs | 64.9 µs | 15,121 QPS |
| 3 | update-disjoint | close | 40,000 | 36.8 µs | 47.4 µs | 59.3 µs | 137.4 µs | 197.3 µs | 78.8 µs | 12,477 QPS |
| 3 | update-hot | base | 40,000 | 36.9 µs | 43.9 µs | 50.5 µs | 107.6 µs | 153.5 µs | 59.4 µs | 16,546 QPS |
| 3 | update-hot | close | 40,000 | 35.0 µs | 40.6 µs | 46.3 µs | 80.4 µs | 130.4 µs | 51.5 µs | 19,091 QPS |

(`update-disjoint-again`, `select-hot`, `insert-omitted` and `ping` of every rep, and the swapped batch's tables, are in
`archive/.../pass/m1*/m1-rep*.txt` and `.json`.) The comparison over the three
update arms, every rep, close minus base at p50:

| Batch | Rep (cores base / close) | Reading | Δ p50 hot / disjoint / again | QPS over the three update arms, base → close | Noise floor, disjoint vs its repeat (p50), base / close | Whole-server `cpu.on_cpu` Δ per statement (base - close, derived, 240,000 statements) |
|---|---|---|---|---|---|---|
| 1 | 1 (3 / 1) | clean | 0.0 / +2.6 / -0.7 µs | 15,186 → 14,266 QPS | 12.1 / 8.8 µs | -2.72 µs |
| 1 | 2 (3 / 1) | clean | +1.5 / +6.4 / +2.4 µs | 15,772 → 14,561 QPS | 3.5 / 0.5 µs | +0.03 µs |
| 1 | 3 (3 / 3) | clean | -4.2 / +8.0 / +5.5 µs | 15,368 → 14,643 QPS | 1.7 / 0.8 µs | +2.66 µs |
| 2 (swapped) | 1 (2 / 2) | clean | +3.7 / -7.9 / +2.8 µs | 14,850 → 14,424 QPS | 1.0 / 9.7 µs | -1.30 µs |
| 2 (swapped) | 2 (0 / 1) | **biased** (base on core 0) | -0.8 / +2.5 / +2.8 µs | not used | 0.9 / 1.2 µs | +1.61 µs |
| 2 (swapped) | 3 (1 / 3) | clean | -0.5 / +2.4 / -5.4 µs | 13,986 → 14,141 QPS | 5.9 / 1.9 µs | +8.86 µs |

Pooled over the clean reps (15 comparisons): close minus base at p50 = **+1.1 µs (+2 %)**, standard error about
1 µs; batch 1 alone +2.4 µs at p50 (+4.6 %; QPS -6.2 %: close slower in all three reps), the swapped batch alone -0.8 µs on its two clean reps
(QPS -2.9 % and +1.1 %): **swapping which binary the driver treats as primary moved the apparent
effect by about 3 µs**, as large as the effect, so it is a role / port / core-placement effect of the harness, not a
price of either build. Whole-server CPU per statement (base - close) is -2.72 to +8.86 µs across the six
reps with median +0.8 µs: inside its own spread. The controls that the change cannot reach move the same way
(`insert-omitted` -8.7 to +5.9 µs, `select-hot` -12.8 to +9.2 µs). The noise floors (same statement
measured twice) run 0.5-12.1 µs.

**Reading: no cost outside the spread; resolution about ±1-2 µs of a ~52 µs round trip.**
`app.lock_wait` and `sys.lock_waits` are 0 in every M1 run (one session: nothing to wait for), so there is no
wait to decompose beyond the client round trip and the server's CPU; `relaxed` has no fsync in the statement.

**Delta against S0.** S0's clean reps measured p50 39.1-39.7 µs on the same arms (base, byte-identical
binary) and 42.3 / 41.4 µs mean; this run's base measures **50.5-59.5 µs p50** (64.8 µs mean pooled over batch 1, 15,438 QPS).
The same binary reads 11-20 µs (28-50 %) slower on this shape today, with no competing process in `host.log`. The cause was not found (M5's one-session
KWP point `UPDATE`, the same statement with the driver on CPUs 0,1 instead of 6,7, reads 42.7-44.2 µs on base here against S0's 42.1-43.3 µs, i.e. unchanged;
M1's driver sits on CPUs 6-7, the SMT siblings of reactor CPUs 2-3). It is why this file takes every close-versus-base delta inside the run and none across days.

## 5. M3 - the scenarios: waits are rare and granted, `U` is what an `UPDATE` waits in, throughput is equal

### 5.1 Scenario 2 (`--bookers 8`, `group`)

Each run ended early (the 24,000-cargo pool is booked in about 8.6 s of the 60 s allowance, as in S0; the
driver says so); the TPS is an average over that window. Three runs per binary, order base, close | close, base | base, close
(`a`, `b`, `c`). Booking and commit are the driver's latency over the booking window:

| Run | Binary | committed | TPS | booking p0 / p25 / p50 / p95 / p99 | commit p0 / p25 / p50 / p95 / p99 | `--verify 100` failures of 400 | `TXN_CONFLICT` retried |
|---|---|---|---|---|---|---|---|
| a | base | 19,894 | 2,309.6 TPS | 403.2 / 1,263.0 / 2,544.5 / 3,361.7 / 5,112.8 µs | 396.3 / 735.0 / 879.1 / 1,337.0 / 2,024.8 µs | 97 | 3 (operations) |
| b | base | 19,921 | 2,327.5 TPS | 390.4 / 1,298.0 / 2,548.4 / 3,338.0 / 4,731.8 µs | 418.0 / 747.8 / 897.7 / 1,357.7 / 2,028.1 µs | 91 | 4 (3 operations, 1 organizations) |
| c | base | 19,907 | 2,299.8 TPS | 399.3 / 1,318.1 / 2,558.5 / 3,391.1 / 5,381.6 µs | 370.1 / 736.9 / 880.7 / 1,368.1 / 2,228.1 µs | 121 | 4 (organizations) |
| a | close | 19,890 | 2,277.0 TPS | 386.9 / 1,287.3 / 2,542.0 / 3,393.3 / 5,363.9 µs | 402.9 / 730.6 / 876.4 / 1,334.1 / 1,955.9 µs | 109 | 5 (1 operations, 4 organizations) |
| b | close | 19,914 | 2,323.9 TPS | 411.2 / 1,259.2 / 2,545.8 / 3,343.9 / 4,756.8 µs | 368.6 / 746.3 / 891.9 / 1,335.9 / 1,980.8 µs | 110 | 4 (1 operations, 3 organizations) |
| c | close | 19,915 | 2,337.4 TPS | 393.5 / 1,280.7 / 2,524.7 / 3,288.5 / 4,528.2 µs | 336.3 / 725.2 / 864.2 / 1,282.9 / 1,663.4 µs | 111 | 1 (operations) |

Mean TPS **base 2,312.3, close 2,312.8 (+0.02 %)**; the within-binary spread is 1.2 % (base) and 2.6 % (close).
The statement phases of runs `a` (each binary; the other runs are in the archive):

| Phase | Binary | ops | errors | p0 | p25 | p50 | p95 | p99 | rate |
|---|---|---|---|---|---|---|---|---|---|
| operation-update | base | 19,897 | 3 | 43.8 µs | 83.5 µs | 97.8 µs | 215.5 µs | 362.5 µs | 2,310/s |
| operation-update | close | 19,895 | 1 | 39.4 µs | 83.6 µs | 98.9 µs | 220.1 µs | 370.3 µs | 2,278/s |
| org-update | base | 19,894 | 0 | 39.5 µs | 73.4 µs | 86.9 µs | 230.5 µs | 384.3 µs | 2,310/s |
| org-update | close | 19,894 | 4 | 41.5 µs | 73.4 µs | 86.9 µs | 232.7 µs | 392.6 µs | 2,278/s |
| freight-insert | base | 19,897 | 0 | 46.3 µs | 98.0 µs | 121.4 µs | 256.1 µs | 430.8 µs | 2,310/s |
| freight-insert | close | 19,895 | 0 | 48.0 µs | 97.2 µs | 121.1 µs | 267.1 µs | 458.5 µs | 2,278/s |
| charge-insert | base | 111,924 | 0 | 33.3 µs | 66.0 µs | 81.0 µs | 189.5 µs | 330.0 µs | 12,994/s |
| charge-insert | close | 111,903 | 0 | 33.9 µs | 65.9 µs | 81.1 µs | 192.0 µs | 336.9 µs | 12,811/s |

`sys.lock_waits` (the whole ring, the run since baseline), by mode:

| Run | Binary | Unit | Mode | Outcome | Rows | Σ waited | max | p50 | deadlock / `fault_net` / cancelled |
|---|---|---|---|---|---|---|---|---|---|
| a | base | tuple | `X` | granted | 49 | 6,879 µs | 1,975 µs | 91 µs | 0 / 0 / 0 |
| b | base | tuple | `X` | granted | 59 | 6,300 µs | 432 µs | 81 µs | 0 / 0 / 0 |
| c | base | tuple | `X` | granted | 63 | 31,672 µs | 12,311 µs | 71 µs | 0 / 0 / 0 |
| a | close | tuple | **`U`** | granted | 67 | 6,195 µs | 1,013 µs | 56 µs | 0 / 0 / 0 |
| b | close | tuple | **`U`** | granted | 45 | 5,238 µs | 1,276 µs | 76 µs | 0 / 0 / 0 |
| c | close | tuple | **`U`** | granted | 62 | 6,750 µs | 1,750 µs | 70.5 µs | 0 / 0 / 0 |

Per relation in run `a`: base `operations` 34 (5,709 µs) and `organizations` 15 (1,170 µs) rows, all `X`; close `operations` 39 (4,845 µs)
and `organizations` 28 (1,350 µs), all `U`. **Not one `X` row exists on the closing build and not one `U` row on the opening build**: an
`UPDATE`'s wait is now in `U`. Parked asks are 0.23-0.34 % of bookings (45-67 of ~19,900) on close and 0.25-0.32 % (49-63) on base (S0: 0.42 %, 84).

Wait accounting (`sys.wait_stats` delta over the run since the baseline, which includes the 30 s load; run `a`):

| Wait type | base | share of DB time | close | share of DB time |
|---|---|---|---|---|
| DB time | 71,978,179 µs | 100 % | 71,804,396 µs | 100 % |
| `cpu.on_cpu` (read, write statements, client round trip excluded) | 37,802,636 µs | 52.5 % | 37,153,136 µs | 51.7 % |
| `commit.wal_flush` (durability) | 32,252,003 µs | 44.8 % | 32,501,565 µs | 45.3 % |
| `sched.run_queue` | 1,482,160 µs | 2.06 % | 1,700,157 µs | 2.37 % |
| `sysio.wal_write` | 242,740 µs | 0.34 % | 254,628 µs | 0.35 % |
| `sysio.checkpoint` | 191,758 µs | 0.27 % | 188,710 µs | 0.26 % |
| **`app.lock_wait`** | **6,882 µs (52 events)** | **0.010 %** | **6,200 µs (75 events)** | **0.009 %** |

`app.lock_wait` over the six runs: base 0.010 / 0.009 / 0.043 %, close 0.009 / 0.007 / 0.009 % of DB time (base `c`'s 0.043 % is one 12.3 ms wait).
The partition latch: base 7,437 / 7,824 / 7,760 waits (3,949 / 3,893 / 5,258 µs), close 7,439 / 7,129 / 7,323 waits (4,334 / 3,870 / 3,875 µs); S0: 7,716 waits / 3,944 µs.
The ASH sampler (100 ms grid) caught `app.lock_wait` in 1 sample in total over the 10 M3 runs (a close run): its waits are about 80 µs long.

**Verify.** `--verify 100` fails 91-121 of 400 on base and 109-111 on close, the same known client-side lost-update under READ COMMITTED
that S0 reported (106 of 400) and `known-gaps.md`'s CA section describes; it is not a property of either build and the throughput above is not a clean-run figure for either.

### 5.2 Scenario 0 (`--fk --txn`, 8 traders, `group`)

Disjoint account partitions: no two writers ever share a parent row, so the foreign-key `S` and `IS` are the uncontended path and the `U`-admits-`S` rule cannot fire. Order of runs: close, base, base, close, then a third pair, base first, run later as a follow-up (11:42:17-11:44:34 UTC, host load 0.1 at start, `procs=0` in all 68 samples, binaries re-hashed).

| Run | Binary | txn ops | TPS | txn p0 / p25 / p50 / p95 / p99 | trade-insert p0 / p25 / p50 / p95 / p99 | account-update p0 / p25 / p50 / p95 / p99 | rolled back / torn / verify |
|---|---|---|---|---|---|---|---|
| a | close | 311,304 | 5,188 TPS | 627.6 / 1,237.6 / 1,427.5 / 1,935.4 / 2,549.3 µs | 38.0 / 83.6 / 104.8 / 196.9 / 327.4 µs | 36.9 / 73.1 / 88.5 / 168.8 / 322.7 µs | 0 / 0 / 200 of 200 match |
| a | base | 312,561 | 5,210 TPS | 608.5 / 1,243.5 / 1,434.6 / 1,930.0 / 2,434.0 µs | 37.7 / 83.4 / 104.2 / 195.6 / 329.7 µs | 36.8 / 73.1 / 88.1 / 167.8 / 327.7 µs | 0 / 0 / 200 of 200 |
| b | base | 310,484 | 5,175 TPS | 593.7 / 1,245.9 / 1,440.7 / 1,955.6 / 2,600.8 µs | 37.8 / 83.4 / 103.9 / 196.3 / 323.6 µs | 36.7 / 73.1 / 87.9 / 167.9 / 315.2 µs | 0 / 0 / 200 of 200 |
| b | close | 303,719 | 5,062 TPS | 618.4 / 1,269.2 / 1,458.6 / 2,006.7 / 2,752.8 µs | 37.8 / 85.7 / 108.2 / 221.4 / 358.0 µs | 36.7 / 75.3 / 92.0 / 204.3 / 338.8 µs | 0 / 0 / 200 of 200 |
| c (11:42, base first) | base | 316,836 | 5,281 TPS | 607.4 / 1,223.6 / 1,404.2 / 1,895.4 / 2,705.8 µs | 37.9 / 83.3 / 104.1 / 195.3 / 335.7 µs | 37.3 / 73.3 / 88.3 / 168.0 / 324.5 µs | 0 / 0 / 200 of 200 |
| c | close | 320,712 | 5,346 TPS | 603.4 / 1,220.2 / 1,396.8 / 1,856.5 / 2,350.3 µs | 37.2 / 83.3 / 103.8 / 193.2 / 323.1 µs | 36.8 / 73.2 / 88.3 / 167.7 / 318.8 µs | 0 / 0 / 200 of 200 |

Three runs per binary: base 5,210 / 5,175 / 5,281 (mean **5,222**, spread 106 TPS = 2.0 %); close 5,188 / 5,062 / 5,346 (mean **5,199**, spread 284 TPS = 5.5 %).
Close minus base: **-23 TPS (-0.45 %)**, down from -1.3 % at two runs. **Three runs per binary do not resolve a difference: the delta is a twelfth of the close runs' own
spread, and the third pair put close above base (5,346 against 5,281 TPS, trade-insert p95 193.2 against 195.3 µs).** The low run `b` (5,062 TPS) was the
build-independent kind of low point: the same binary ran 5,346 TPS next. The honest reading is "no difference resolvable above about 3-5 % at three runs", not "equal to 0.4 %".
No `sys.lock_waits` row, deadlock, `fault_net` or cancelled in run `c` of either binary (`app.lock_wait` 0 events, partition latch 146,676 waits / 205,741 µs on base and 147,539 / 206,468 µs on close).

| Wait accounting, scenario 0 (`sys.wait_stats`, boot to end; run `a` of each) | base | share | close | share |
|---|---|---|---|---|
| DB time | 340,127,476 µs | 100 % | 339,736,133 µs | 100 % |
| `commit.wal_flush` | 291,019,660 µs | 85.6 % | 290,400,618 µs | 85.5 % |
| `cpu.on_cpu` | 42,405,524 µs | 12.5 % | 42,643,527 µs | 12.6 % |
| `sched.run_queue` | 5,407,531 µs | 1.59 % | 5,355,723 µs | 1.58 % |
| `sysio.wal_write` / `sysio.checkpoint` | 1,100,738 / 194,023 µs | 0.32 / 0.06 % | 1,125,930 / 210,335 µs | 0.33 / 0.06 % |
| **`app.lock_wait`** | **0 events** | **0 %** | **0 events** | **0 %** |
| `sys.lock_waits` rows, deadlock, `fault_net`, fence walks | 0, 0, 0, 0 | | 0, 0, 0, 0 | |

Partition latch waits: base 149,894 / 148,510 (201,300 / 205,636 µs), close 142,876 / 130,483 (205,664 / 207,880 µs); S0 149,134 / 219,546 µs.
The durable commit is 85 % of DB time on both builds; the lock family is a latch with no wait.

**Delta against S0** (base, byte-identical binary, one run each there): scenario 2 TPS 2,374.1 → 2,309.6 / 2,327.5 / 2,299.8 (mean -2.6 %), scenario 0 TPS 5,343.7 → 5,210 / 5,175 / 5,281 (mean -2.3 %);
scenario 0 txn p50 1,397.0 → 1,434.6 / 1,440.7 / 1,404.2 µs (+0.5 to +3.1 %). So the same binary is 0.5-3.1 % slower today than at S0, which is
larger than the close-versus-base delta in scenario 2 (+0.02 %) and in scenario 0 (-0.45 %). The lock-wait picture matches S0: 84 granted waits and `app.lock_wait` 0.033 % of DB time there, 49-63 and 0.009-0.043 % here on base.

## 6. M4 - eight sessions on one hot row: same refusals, same waits, `U` instead of `X`

Base and close side by side (the driver's `--ab-port`), `relaxed`, 8 sessions x 4,000 ops per arm, sessions accepted on cores 0, 2, 3 (base) and 0-3 (close).

| Arm (8 sessions) | Binary | ops | errors | p0 | p25 | p50 | p95 | p99 | max | QPS |
|---|---|---|---|---|---|---|---|---|---|---|
| update-hot | base | 32,000 | 2,318 (7.2 %) | 37.8 µs | 189.6 µs | 303.2 µs | 765.9 µs | 1,059.7 µs | 5,245.2 µs | 22,270 QPS |
| update-hot | close | 32,000 | 2,065 (6.5 %) | 35.9 µs | 182.3 µs | 288.1 µs | 719.3 µs | 997.2 µs | 4,158.2 µs | 23,575 QPS |
| update-disjoint | base | 32,000 | 0 | 38.9 µs | 185.6 µs | 292.9 µs | 716.8 µs | 985.3 µs | 4,203.8 µs | 23,378 QPS |
| update-disjoint | close | 32,000 | 0 | 38.7 µs | 175.7 µs | 276.7 µs | 688.7 µs | 942.2 µs | 6,533.3 µs | 24,584 QPS |
| update-disjoint-again | base | 32,000 | 0 | 36.2 µs | 178.1 µs | 280.7 µs | 713.4 µs | 986.0 µs | 5,472.6 µs | 23,779 QPS |
| update-disjoint-again | close | 32,000 | 0 | 36.4 µs | 174.7 µs | 272.1 µs | 681.9 µs | 940.6 µs | 6,732.0 µs | 24,690 QPS |
| insert-omitted | base | 32,000 | 0 | 37.1 µs | 179.3 µs | 280.2 µs | 687.4 µs | 938.4 µs | 5,015.3 µs | 24,289 QPS |
| insert-omitted | close | 32,000 | 0 | 33.9 µs | 120.1 µs | 194.7 µs | 596.1 µs | 845.9 µs | 277,308.2 µs | 21,963 QPS |
| select-hot | base | 32,000 | 0 | 47.8 µs | 253.1 µs | 397.6 µs | 984.5 µs | 1,371.9 µs | 6,606.1 µs | 17,016 QPS |
| select-hot | close | 32,000 | 0 | 51.3 µs | 257.0 µs | 401.8 µs | 982.0 µs | 1,339.8 µs | 3,673.6 µs | 17,109 QPS |

| Quantity | S0 (base) | this run base | this run close |
|---|---|---|---|
| hot - disjoint, p50 | +5.0 µs | +10.3 µs | +11.4 µs |
| noise floor (disjoint vs again, p50) | 2.7 µs | 12.2 µs | 4.6 µs |
| `update-hot` refusals | 2,519 of 32,000 (7.9 %) | 2,318 (7.2 %) | 2,065 (6.5 %) |
| first refusal text | `ERR TXN_CONFLICT retryable=1 row id=1 was written by transaction 35` | `... row id=1 was written by transaction 12319` | `... row id=1 was written by transaction 4357` |
| `sys.lock_waits` | 17 rows, tuple `X`, granted, Σ 688 µs | 9 rows, tuple `X`, granted, Σ 137 µs, max 38 µs | 18 rows, tuple **`U`**, granted, Σ 303 µs, max 166 µs |
| `app.lock_wait` / `app.lock_conflict` | 1,026 events, 830 µs | 1,634 events, 553 µs | 1,648 events, 849 µs |
| deadlock / `fault_net` / cancelled | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |

The hot-minus-disjoint of both builds (+10.3, +11.4 µs) is inside the 4.6-12.2 µs noise floors and includes the refusal replies; the closing build refuses
**fewer** statements (6.5 % against 7.2 %), the same `TXN_CONFLICT` of first-updater-wins, from the MVCC window, never a lock. `insert-omitted` on
close is 85 µs lower at p50 (194.7 against 280.2 µs): one sample in a control the change does not reach, not a gain; and the 277 ms max in the same
row is the one transaction-id carve stall (`contention_carve_longest_us` 233-236 ms in the views) that appears once per run in
whichever arm it lands. The base-versus-S0 spread of the refusal counts (7.9, 7.2 %) shows run-to-run variation of about 0.7 points; the close figure is
0.7 points lower than this run's base. S0's lock-wait count (17 rows) and this run's (9, 18) say the same: ~0.03-0.06 % of the hot statements parked.

## 7. M5 - a standing fence: the same ~1 µs on both builds

`cp_s0_fence.py`: one session holds `UPDATE ... WHERE id > 4080` in an open transaction (a range `X` on base; a range `U` on close - the held
statement is an `UPDATE`); writers update rows `<= 4080` in autocommit, so none is covered, refused or parked (`sys.lock_waits`: 0 rows in every cell). Fenced and
unfenced arms alternate in 6 blocks. Every fenced statement walks the 64 partitions exactly once (`walks_per_statement` 1.000; 0.000 unfenced) on both builds. The lock-table mode of the held
fence was not read (no wait ever produced a `sys.lock_waits` row to name it).

| Cell | Arm | Binary (run) | ops | p0 | p25 | p50 | p95 | p99 | mean | QPS |
|---|---|---|---|---|---|---|---|---|---|---|
| kwp-w4 | update-fenced | base (a) | 24,000 | 33.1 µs | 97.2 µs | 133.6 µs | 252.1 µs | 331.5 µs | 141.5 µs | 27,188 QPS |
| kwp-w4 | update-unfenced | base (a) | 24,000 | 38.9 µs | 99.5 µs | 134.5 µs | 250.3 µs | 334.2 µs | 143.2 µs | 26,977 QPS |
| kwp-w4 | update-fenced | close (a) | 24,000 | 42.9 µs | 97.9 µs | 132.9 µs | 253.7 µs | 336.6 µs | 142.3 µs | 27,086 QPS |
| kwp-w4 | update-unfenced | close (a) | 24,000 | 40.8 µs | 96.1 µs | 133.0 µs | 254.8 µs | 341.5 µs | 141.4 µs | 27,100 QPS |
| kwp-w1 | update-fenced | base (a) | 6,000 | 35.8 µs | 43.4 µs | 44.2 µs | 57.2 µs | 96.1 µs | 48.2 µs | 20,377 QPS |
| kwp-w1 | update-unfenced | base (a) | 6,000 | 35.7 µs | 42.1 µs | 42.7 µs | 50.5 µs | 61.4 µs | 44.3 µs | 22,111 QPS |
| kwp-w1 | update-fenced | close (a) | 6,000 | 40.9 µs | 43.5 µs | 44.8 µs | 61.3 µs | 76.3 µs | 49.7 µs | 19,721 QPS |
| kwp-w1 | update-unfenced | close (a) | 6,000 | 40.1 µs | 42.5 µs | 44.6 µs | 60.1 µs | 77.1 µs | 50.7 µs | 19,336 QPS |
| text-w4 | update-fenced | base (a) | 24,000 | 24.4 µs | 45.1 µs | 58.0 µs | 98.7 µs | 137.1 µs | 65.2 µs | 57,684 QPS |
| text-w4 | update-unfenced | base (a) | 24,000 | 21.6 µs | 46.9 µs | 56.6 µs | 100.5 µs | 139.8 µs | 63.0 µs | 60,763 QPS |
| text-w4 | update-fenced | close (a) | 24,000 | 24.7 µs | 45.8 µs | 58.5 µs | 96.6 µs | 122.7 µs | 63.7 µs | 58,837 QPS |
| text-w4 | update-unfenced | close (a) | 24,000 | 22.6 µs | 47.3 µs | 57.5 µs | 99.0 µs | 142.9 µs | 63.1 µs | 61,007 QPS |
| text-w1 | update-fenced | base (a) | 6,000 | 30.9 µs | 32.6 µs | 33.1 µs | 40.4 µs | 52.8 µs | 34.5 µs | 28,126 QPS |
| text-w1 | update-unfenced | base (a) | 6,000 | 28.3 µs | 31.5 µs | 32.1 µs | 40.6 µs | 81.3 µs | 67.1 µs | 14,673 QPS |
| text-w1 | update-fenced | close (a) | 6,000 | 31.0 µs | 32.8 µs | 33.2 µs | 38.5 µs | 52.0 µs | 64.4 µs | 15,282 QPS |
| text-w1 | update-unfenced | close (a) | 6,000 | 27.9 µs | 31.7 µs | 32.2 µs | 39.3 µs | 66.7 µs | 38.3 µs | 25,447 QPS |

(Runs `b` of both binaries in the archive.) Fenced minus unfenced at p50 in the two runs of each binary, and S0's base:

| Cell | S0 base | base a / b | close a / b | block p50 spread fenced / unfenced, close a (µs) |
|---|---|---|---|---|
| kwp, 4 writers | -2.6 µs | -0.9 / -1.2 µs | -0.1 / -1.8 µs | 11.3 / 15.9 |
| kwp, 1 writer | +1.2 µs | +1.5 / +1.2 µs | +0.2 / +0.9 µs | 13.7 / 12.9 |
| text, 4 writers | +1.4 µs | +1.4 / +1.1 µs | +1.0 / +1.0 µs | 1.8 / 3.1 |
| text, 1 writer | +1.1 µs | +1.0 / +1.2 µs | +1.0 / +1.1 µs | 0.3 / 1.0 |

The fence's price (~1 µs at p50 on the single-writer cells, 0-1.4 µs on four) is the same on both builds and the same as S0's; the close-minus-base
difference of that delta is -1.3 to +0.8 µs per cell, inside the block spreads where those exceed it. The text 1-writer means (34-73 µs) are
one stall of about 190-250 ms in 6,000 statements (the carve above) landing in whichever arm: the fenced arm in both close runs, the unfenced arm in one of two base runs: a mean-level artefact; p50 is unaffected. The "latch waits" in S0's
table are cumulative over the server's life and are not re-tabulated here (1,340-1,607 over the four cells of each run).

## 8. Against the bar, CP-Q7 (a)

| Clause | Reading | Evidence |
|---|---|---|
| **(i) a measured gain on the shape each candidate names** (C1: scan CPU, M2) | **Met for C1.** Not resolved for the `U` mode (C2) | M2: base minus close 82-99 ns per leaf page, 2.50-2.78 % of the 603-page scan's server CPU and 8.16-8.77 % of the 304-page wide scan's, three of four cells with the whole [min, max] above zero, point controls -0.08 to -0.30 µs; the closing build's scan CPU is within 0.6 % of S0's no-slice patch. The gain is at one core, unlatched. The shape C2 names (a parent updater and a child writer on one parent) is not in any throughput cell: scenario 0 has no shared parents and scenario 2 no `--fk`; the evidence for it is the suite's rig cell, which this run did not run |
| **(ii) no cost outside the spread elsewhere** (M1, M3, M4) | **Met, at the resolution stated** (scenario 0's delta is not distinguishable from the run-to-run spread) | M1: +1.1 µs p50 (+2 %, SE about 1 µs) over 15 clean comparisons, +2.4 / -0.8 µs in the two label-role batches, whole-server CPU median +0.8 µs per statement in a -2.7..+8.9 spread. M3 scenario 2: 2,312.8 against 2,312.3 TPS (+0.02 %), `app.lock_wait` 0.007-0.009 % of DB time on close against 0.009-0.043 % on base, partition latch 7,129-7,439 against 7,437-7,824 waits. M3 scenario 0: three runs per binary, base 5,222 / close 5,199 mean TPS (-0.45 %), spreads 2.0 % (base) and 5.5 % (close); the delta is inside the spread, so it is neither a cost nor a demonstrated equality. M4: hot-minus-disjoint +11.4 against +10.3 µs inside floors of 4.6-12.2 µs, 1,648 against 1,634 `app.lock_wait` events. M5: fence price unchanged (±1 µs) |
| **(iii) no wait turned into a refusal, except the accepted CP-Q10 (a) class** | **Met on every shape run; the accepted class was not reachable** | 0 deadlocks, 0 `fault_net`, 0 cancelled in `sys.lock_waits` of every closing-build run whose views were read (M3 x5, M4; M1 x6 had no waits at all); every parked ask granted; `U` rows (45-67 per scenario-2 run, 18 in M4) all granted, none refused. The only refusal text on either build is `ERR TXN_CONFLICT retryable=1 row id=N was written by transaction T` (M3: 1-5 per run on close, 3-4 on base; M4: 2,065 on close, 2,318 on base). No cell runs a parent updater over a child writer's share, so CP-Q10 (a)'s deadlock class is neither exercised nor excluded |

## 9. What the run teaches about the engine

1. **CP-S2a's deletion is real and about as large as S0 said.** The measured 82-99 ns per leaf page (S0's 88-126 ns by patch) is
   AO-S7's "up to 3 µs a page" bound off by a factor of thirty. It is the only change of the milestone that
   shows in a throughput-shaped number, and it shows on the shape that walks pages: a full scan of a 603-page relation saves about
   55-60 µs of 2,156 µs.
2. **The `U` mode is free at the lock table and free to the client.** An `UPDATE`'s contended waits are now mode `U`,
   45-67 of them per scenario-2 run against 49-63 `X` on the opening build, at a 56-91 µs median and ~100 µs mean either way; the
   booking's `UPDATE` latency (`operation-update` p50 97.8 → 98.9 µs, `org-update` 86.9 → 86.9 µs) does not move. What it buys - the pair of
   a child's `S` and a parent updater's `U` that no longer deadlocks - needs a shape this suite of drivers does not generate.
3. **This host's cross-day drift is 2.6-3.1 % on scenarios and 28-50 % on M1's round trip, for the identical binary**, and the M1
   harness's label-role swap moved its own apparent effect by ~3 µs. Both are why the opening-versus-closing delta is taken inside each run, and
   why a delta below ~2-3 % on the scenarios and ~±2 µs on M1 is reported as unresolved. The sum is that
   the lock family's cost, which S0 priced at ~0.4 µs of a 39 µs write, is below what this harness resolves on this host today.
4. **Durability still dominates**: `commit.wal_flush` is 45 % of scenario 2's and 85 % of scenario 0's DB time on both builds; `app.lock_wait` is
   0.007-0.043 % and 0 %. Nothing in CP changes which structure the time goes to.
5. **A once-per-run 190-290 ms stall** (the transaction-id carve, `contention_carve_longest_us` 233-236 ms) falls in whichever arm or phase is running (the 277-290 ms `max` of
   M3's commit and booking tables, M4's `insert-omitted`, M5's text 1-writer means). It is the reason means are not used for any claim in this file.

## 10. Limits and what was cut

- **Budget:** about 27 minutes of measurement (09:49:19 to 10:16:27 UTC) inside the 40-minute allowance; nothing was cut for time. M6 was not re-run by instruction. A follow-up scenario-0 pair ran later at 11:42:17-11:44:34 UTC (total measurement about 29 minutes). The second pass (M5 on base and repeats, a second scenario-0 pair, a third scenario-2 pair; the third scenario-0 pair came as a follow-up) and the M1 label-swapped batch were added because the main pass finished in 15 minutes.
- **Row-set sweep (rule 9) not done** at 200 / 1K / 10K: M1 and M4 one size (256 rows), M5 4,096 rows, M3 two scenario sizes; M2 has two sizes (10,000 and 100,000 rows). A point `UPDATE` does not scale with rows. The sweep was not run in S0 either.
- **Scenario 0 is three runs per binary** (a follow-up pair, base first, 11:42:17-11:44:34 UTC). Three runs do not resolve the -0.45 % against a 5.5 % close spread; a difference below roughly 3-5 % is not detectable here.
- **M1 resolution** is about ±1-2 µs of ~52 µs; it cannot see the ~0.4 µs S0 attributed to the whole family. The M1 base is 11-20 µs slower than S0's for the same binary and the cause is not known.
- **Not measured:** the latched slice `IS` above one core (M2 is `cores = 1`; the deletion's gain with the 64-partition latch is not priced); eight cores; `strict` durability for any cell (all `relaxed` or `group`); a shared-parent arm for C2 (no driver has one; not admitted inside a measurement stage); the CP-Q10 (a) deadlock class; the lock-table mode of M5's held range fence. The scenario-2 `--verify` failures (91-121 of 400 on both builds) are the driver's own client-side lost update, not evidence about the family.
- **Prior-day comparisons** (the S0 columns) are one run each there and are given for repeatability, not as a delta of the change.
- **PostgreSQL floor:** not measured for any of these shapes.
- **Tests:** none run by this measurement; no engine code was changed.

## 11. Reproduce

Scripts: `archive/cp-close-v3.4.7-11-g7d21468a/scripts/` (`run_all.sh`, `run_extra.sh`, `run_m1swap.sh` are the three passes; each cell's script is named for its cell; `archive.sh` copied the outputs). Both binaries are Release builds, copied before the first cell:

```
# M1 (three reps; the swapped batch uses --port Q --label close --ab-port P --ab-label base)
lock_contention_benchmark.py --port P --label base --ab-port Q --ab-label close \
    --sessions 1 --rows 256 --ops 40000 --blocks 4 --json m1-repN.json
# M4
lock_contention_benchmark.py --port P --label base --ab-port Q --ab-label close \
    --sessions 8 --rows 256 --ops 4000 --blocks 4 --json m4.json
# M2
bm_scan.py census --arm base=BIN_BASE --arm close=BIN_CLOSE --runs 5 --blocks 12 --scan-ops 768 \
    --point-ops 3000 --cells rows-100k,wide-a --pin-server 2 --pin-driver 3
# M3 (one server per run, group, cores 4 on CPUs 2-5)
scenario2_freight.py --suffix m3s2 --organizations 300 --ships 30 --operations 300 --cargos 24000 \
    --verify 100 --seed 1 --sync --bookers 8 --seconds 60 --bookings 0   # --schema-only, --load-only, then the run
scenario0_stockmarket.py --suffix m3s0 --users 2000 --assets 2000 --traders 8 --seconds 60 \
    --fk --txn --verify 200 --seed 1 --sync
# M5
cp_s0_fence.py --rows 4096 --writers {4,1} --ops 1000 --blocks 6 --protocol {kwp,text}
```

Row counts: M1/M4 `--rows 256` is the table size; M5's 4,096 rows with the fence at `id > 4080` covers 16 rows; M2's `rows-100k` has 100,000 rows (603 leaf pages) and `wide-a` 10,000 rows (304).
