# CP-S0 premise: the lock family costs about 0.4 µs of a 39 µs point write, the slice `IS` about 0.1 µs a leaf page, a standing fence about 1 µs a point write, and the contention a client sees on a hot row is the MVCC window, not a lock

**Thesis.** `instructions/v3.2.0/workorder-cp-lock-model-exploration.md` asks which cells of
the lock family earn their price. On this host (4 cores, `build-release`, `/dev/nvme0n1p1`
ext4) the whole family is a ~1 % matter on a single-session point `UPDATE` (M1), 0.03 % of
DB time under scenario 2's eight contended bookers (M3), and 0.01 % under eight sessions on
one row (M4). Every wait the family recorded in those cells was granted: no deadlock, no
`fault_net`, no wait turned into a refusal. The slice `IS` that CP-Q3 (a) deletes costs
88-126 ns per leaf page at one core (M2), roughly thirty times under AO-S7's extrapolated
bound. A standing range fence adds about 1 µs to a point write it does not cover (M5), with
the 64-partition walk running exactly once per statement. The cell that corrects the tree is
M6: under `group`, a second session's `UPDATE` of the row a first session just committed is
answered **before** the first session's `COMMIT` acknowledgement in 300 of 300 iterations -
the hot row's lock is *not* held across the group fsync, so `known-gaps.md`'s CA entry was
wrong, and `strict` (where it is held by design) is the control that shows the harness can
see a held lock. What the client does see on a hot row is `TXN_CONFLICT` from the
first-updater-wins rule, on 7.9 % of M4's hot statements, which no lock cell changes.
Nothing in S0 says "build" for C4 or C5 by itself; C1 is deleted on CP-Q3 (a) whatever M2
shows; C2's reason is a correctness shape no throughput cell exercises; C3 is about 1 µs at
four cores and BP-H10's eight-core figure was not reproduced because eight cores were not run.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-11-08, **pass 1** 07:23:51-07:29:35 UTC (344 s: M1 + proof arm, M3, M2, M5 attempt 1, M6, M4), **pass 2** 07:30:51-07:34:52 UTC (241 s: M5, M1 reps 1-3). A smoke test (`scripts/smoke.sh`) ran immediately before pass 1 |
| Worktree / branch | `/home/louis/ckdbs/.claude/worktrees/cp-lock-model-exploration`, `worktree-cp-lock-model-exploration` |
| Commit | `cc54a296` = `v3.4.7-1-gcc54a296`, committed 2026-11-07 02:36:29 UTC. The commit is documents only; its build inputs are those of `6c4b9a6d` (`v3.4.7`) |
| Tree | Modified, none of it an input of `kds_server`: `docs/inflight/known-gaps.md`, `instructions/v3.2.0/index.md`, `instructions/v3.2.0/workorder-cp-lock-model-exploration.md`, `tests/CMakeLists.txt`. Untracked: `instructions/v3.2.0/raft-marks-2026-11-08.md`, `tests/lock_release_before_sync_rig_test.cpp`, `tools/cp_s0_fence.py`, `tools/cp_s0_release_order.py` (cited by path; copies in the archive). The run stamps record `dirty=0 (build inputs)`; the manifests' `git_describe` carries `-dirty` for the files above |
| Binary **base** | copy `kds_server-base`, sha256 `d78b1147d6196cb08c53cda378105fecbb3dd352ba5c7ec2dd03e6ef3bc9a1d4`, mtime 2026-11-08 06:24:43 UTC (after the commit's 2026-11-07 02:36:29 UTC, built from this tree at `cc54a296`, Release, `CMAKE_BUILD_TYPE=Release`). `build-release/kds_server` was **overwritten** later by the no-slice build (07:22:36 UTC; its sha256 is today `1970e950...`), so the build tree's own file is no longer the base - every server of every cell was started from a copy |
| Binary **nolock** | copy `kds_server-nolock`, sha256 `aed7b9b0303bea6f030f43b13ca7582710f4b4ea4d0604e7a8a0b0dcb93bd706`, mtime 07:20:59 UTC: the base tree plus `patches/patch-null-table.diff` - core 0's `TransactionManager` and `CommandDispatcher` and every peer core are handed a null lock table. It strips the whole family: the relation `IX`/`IS` asks, the tuple `X`, `DeclaredWriteBorrow`, the first-intention memo check, the detector-gated park rule, the read borrow, DDL's relation `X`, the foreign key's `S` and the unique key unit. Not for landing |
| Binary **noslice** | copy `kds_server-noslice`, sha256 `1970e950f0c6460c994621a25422bea7ee92762ba84ee2fb76acd12dbebf106c`, mtime 07:22:36 UTC: the base tree plus `patches/patch-no-slice.diff` - `ReadBorrow::Position` declines the per-leaf-page slice `IS` and keeps the relation `IS`. Not for landing |
| Device | data files and WALs under `/home/louis/bench-runs/cp-s0/<stamp>/` on `/dev/nvme0n1p1`, ext4 (`df -T`, recorded in every manifest). The scratchpad (`/tmp`, tmpfs) held only the binary copies and the outputs |
| Host | 8 CPUs (Intel Xeon Platinum 8581C, SMT pairs 0/4 1/5 2/6 3/7), 31 GB, Linux 6.12.113. Servers pinned to CPUs 2-5, drivers to 6-7 (M1, M4, M6), 0,1,6,7 (M3), 0,1 (M5); M2: server CPU 2, driver CPU 3. `host.log` (every 2 s) shows 0 compiler or test processes for both passes; 1-minute load average 0.16-3.35 in pass 1 (the 3.13-3.35 samples are the end of M3 and the start of M2, whose quiet gate then ran) and 0.57-1.47 in pass 2 |
| Server config | `cores = 4` (M2: `cores = 1`), `buffer_pool_frames = 65536`, `ash_enabled = on`; `durability = relaxed` for M1, M4, M5, M2 and `group` for M3 and M6 (M6's strict arm is the session's rung via `SET DURABILITY`); M3 also `ash_interval_ms = 100`. Port chosen per cell (15520-15529, 25731-25742, 32611-32621, 15561-15562) |
| Drivers | unmodified `tools/lock_contention_benchmark.py` (M1, M4, proof arm), `tools/bm_scan.py census` (M2), `tools/scenario2_freight.py` and `tools/scenario0_stockmarket.py` (M3); new `tools/cp_s0_fence.py` (M5) and `tools/cp_s0_release_order.py` (M6). Flags and how to run them: [Reproduce](#12-reproduce). `bench/docs/` has no entry for the two new drivers; their docstrings are the flag reference |
| Build type | Release |
| Prior number | **None for these shapes** on this engine except as in §11 (different sizes, no delta taken). This file is the baseline CP-S3 is read against. PostgreSQL floor: **not measured for any of these shapes** |
| Raw outputs | `archive/cp-s0-premise-v3.4.7-1-gcc54a296/` (3.1 MB; no data file, WAL or server log) |

## 2. What was measured

| Cell | Question (candidate) | Driver, arms | Size |
|---|---|---|---|
| M1 | the family's whole share of a point write (C4, and C3's relation ask) | `lock_contention_benchmark.py --sessions 1`, base against nolock, interleaved in 4 blocks; the proof arm `--prove` at 8 sessions | 256 rows; 12,000 ops/arm (pass 1), 3 × 40,000 ops/arm (pass 2) |
| M3 | what the family waits on under real scenarios (C2's price, C4/C5 outcomes, CP-Q9) | `scenario2_freight.py --bookers 8`; `scenario0_stockmarket.py --fk --txn`, 8 traders; `sys.lock_waits`, `sys.wait_stats`, `sys.meta`, `sys.ash` read after each | scenario 2: 300 organizations, 30 ships, 300 operations, 24,000 cargos; scenario 0: 2,000 users, 2,000 assets, 3,932 accounts |
| M2 | the slice `IS` per leaf page (C1; CP-Q3 (a) deletes it whatever the number) | `bm_scan.py census`, base against noslice, 5 runs × 12 blocks | 100,000 rows (603 leaf pages) and 10,000 wide rows (304 leaf pages) |
| M5 | a standing range fence's cost to point writers it does not cover (C3, BP-H10's shape) | `cp_s0_fence.py`: `BEGIN; UPDATE t SET v = v + 1 WHERE id > 4080` held, writers on `id <= 4080`, fenced against unfenced in 6 interleaved blocks; KWP and text, 4 and 1 writers | 4,096 rows; 1,000 ops per writer per block |
| M4 | contended against disjoint at 8 sessions (C5's `fault_net` count, C4's wait outcomes) | `lock_contention_benchmark.py --sessions 8`, base only | 256 rows; 4,000 ops/session/arm |
| M6 | does the hot row's lock outlive the group fsync (CP-Q5) | `cp_s0_release_order.py`, two sessions on core 0, `group` arm and `strict` control | 300 iterations per arm |

## 3. M1: the whole family is about 0.4 µs of a 39 µs point write

One session, autocommit `UPDATE ... WHERE id = k` on its own row, `relaxed`, so there is no
fsync in the statement; the nolock build has no family to ask. Of the four reps, two are
clean - both servers' sessions landed on peer cores (3 and 3; 3 and 2). The driver flags
the other two (core 0 carries the scheduler ticks and the statistics steward; a pair with
one label on core 0 is biased), and their own noise floors (3.7-10.2 µs against 0.2 µs)
show it: they are set aside, printed for the record.

| Rep | Cores accepted on (base / nolock) | Reading | Binary | ops | p0 | p25 | p50 | p95 | p99 | mean | QPS |
|---|---|---|---|---|---|---|---|---|---|---|---|
| pass 1 rep (12,000 ops/arm) | 1 / 0 | biased (labels on core 1 / core 0) | base | 12,000 | 37.2 µs | 39.1 µs | 43.7 µs | 56.7 µs | 68.9 µs | 46.5 µs | 21,055 QPS |
| pass 1 rep (12,000 ops/arm) | 1 / 0 | biased (labels on core 1 / core 0) | nolock | 12,000 | 32.9 µs | 38.1 µs | 39.6 µs | 53.6 µs | 62.0 µs | 43.4 µs | 22,542 QPS |
| pass 2 rep 1 (40,000) | 3 / 3 | **clean** (both peer core 3) | base | 40,000 | 37.0 µs | 38.7 µs | 39.4 µs | 49.7 µs | 63.5 µs | 42.3 µs | 23,190 QPS |
| pass 2 rep 1 (40,000) | 3 / 3 | **clean** (both peer core 3) | nolock | 40,000 | 36.3 µs | 38.3 µs | 39.0 µs | 48.4 µs | 60.7 µs | 41.4 µs | 23,738 QPS |
| pass 2 rep 2 (40,000) | 0 / 1 | biased (core 0 / core 1) | base | 40,000 | 32.3 µs | 38.4 µs | 39.7 µs | 54.8 µs | 84.3 µs | 44.3 µs | 22,155 QPS |
| pass 2 rep 2 (40,000) | 0 / 1 | biased (core 0 / core 1) | nolock | 40,000 | 36.4 µs | 39.5 µs | 48.6 µs | 56.0 µs | 101.5 µs | 47.6 µs | 20,573 QPS |
| pass 2 rep 3 (40,000) | 3 / 2 | **clean** (peer cores 3 / 2) | base | 40,000 | 36.8 µs | 38.4 µs | 39.1 µs | 47.8 µs | 57.8 µs | 41.4 µs | 23,712 QPS |
| pass 2 rep 3 (40,000) | 3 / 2 | **clean** (peer cores 3 / 2) | nolock | 40,000 | 34.6 µs | 38.0 µs | 38.7 µs | 47.8 µs | 57.7 µs | 41.0 µs | 23,938 QPS |

The p50 of every arm, nolock against base, with the run's own noise floor (the same row
measured twice):

| Rep | update-hot | update-disjoint | update-disjoint-again | insert-omitted | select-hot | noise floor base / nolock (disjoint vs again, p50) |
|---|---|---|---|---|---|---|
| pass 1 rep (12,000 ops/arm) (biased) | 49.8 → 49.0 (−0.8 µs) | 43.7 → 39.6 (−4.1 µs) | 40.0 → 49.0 (+9.0 µs) | 42.2 → 45.9 (+3.7 µs) | 71.3 → 51.0 (−20.3 µs) | 3.7 µs / 9.4 µs |
| pass 2 rep 1 (40,000) (clean) | 39.2 → 38.7 (−0.5 µs) | 39.4 → 39.0 (−0.4 µs) | 39.6 → 39.0 (−0.6 µs) | 37.4 → 36.9 (−0.5 µs) | 51.3 → 50.9 (−0.4 µs) | 0.2 µs / 0.0 µs |
| pass 2 rep 2 (40,000) (biased) | 45.1 → 48.9 (+3.8 µs) | 39.7 → 48.6 (+8.9 µs) | 49.9 → 42.6 (−7.3 µs) | 46.8 → 37.5 (−9.3 µs) | 69.0 → 69.6 (+0.6 µs) | 10.2 µs / 6.0 µs |
| pass 2 rep 3 (40,000) (clean) | 39.1 → 38.8 (−0.3 µs) | 39.1 → 38.7 (−0.4 µs) | 38.9 → 38.6 (−0.3 µs) | 37.1 → 37.1 (+0.0 µs) | 50.6 → 50.4 (−0.2 µs) | 0.2 µs / 0.1 µs |

On the two clean reps the nolock build is faster at p50 by 0.3-0.6 µs on all six update
comparisons (mean of the six: −0.4 µs) against a noise floor of 0.0-0.2 µs. **The family's
whole share of a point `UPDATE` at the client is therefore about 0.4 µs, about 1 % of a
~39 µs round trip**, and it is at the edge of what this harness resolves: two reps, floor
0.2 µs, the means (42.3 vs 41.4 µs and 41.4 vs 41.0 µs) differ by 0.4-0.9 µs against a
mean-level repeat spread of 0.1-1.1 µs. The `update-hot` arm (one session, so the same
statement as disjoint) does not differ from `update-disjoint` in either binary, as it should
not. The client round trip carries the Python driver and the text-protocol hop; the
server-side share is a larger fraction of a smaller number, and it was **not separated**:
M1's view readers did not read `sys.statement_stats`. The whole-server cross-check below
(`sys.wait_stats` `cpu.on_cpu` summed over cores, whole run, six arms including the `SHOW
META` ping) puts the CPU difference at 0.5-0.9 µs per statement in the clean reps; it is a
mixed stream, not a price.

| Rep | statements in the run (6 arms × ops) | server `cpu.on_cpu`, base | server `cpu.on_cpu`, nolock | base − nolock | per statement (derived) |
|---|---|---|---|---|---|
| pass 1 rep (biased) | 72,000 | 2,565,658 µs | 2,522,300 µs | 43,358 µs | 0.60 µs |
| pass 2 rep 1 (clean) | 240,000 | 8,440,745 µs | 8,228,055 µs | 212,690 µs | 0.89 µs |
| pass 2 rep 2 (biased) | 240,000 | 8,742,843 µs | 8,449,296 µs | 293,547 µs | 1.22 µs |
| pass 2 rep 3 (clean) | 240,000 | 8,313,799 µs | 8,191,783 µs | 122,016 µs | 0.51 µs |

The lock family's decomposition (A1, the relation `IX`, from A5, the tuple `X` and its
header check) needs a second build and was cut; no tally counts asks.

**The proof arm** (`--prove`, 8 sessions, 300 ops/arm, one pass, run before the reps): on the
nolock binary `update-hot` and `update-disjoint` should differ only by what
first-updater-wins already costs; the driver reports a family cost there if it measures one.

| Arm (8 sessions, 300 ops each, relaxed) | Binary | ops | errors | p0 | p25 | p50 | p95 | p99 | QPS |
|---|---|---|---|---|---|---|---|---|---|
| update-hot | base | 2,400 | 149 | 40.0 µs | 185.5 µs | 295.6 µs | 747.8 µs | 971.9 µs | 21,735 QPS |
| update-hot | nolock | 2,400 | 141 | 38.9 µs | 168.1 µs | 269.3 µs | 685.1 µs | 942.0 µs | 23,743 QPS |
| update-disjoint | base | 2,400 | 0 | 38.8 µs | 181.3 µs | 283.6 µs | 730.5 µs | 985.0 µs | 22,194 QPS |
| update-disjoint | nolock | 2,400 | 0 | 38.2 µs | 166.5 µs | 266.7 µs | 687.1 µs | 904.8 µs | 23,988 QPS |
| update-disjoint-again | base | 2,400 | 0 | 39.0 µs | 145.4 µs | 221.9 µs | 678.5 µs | 1,018.1 µs | 25,287 QPS |
| update-disjoint-again | nolock | 2,400 | 0 | 37.4 µs | 135.2 µs | 211.2 µs | 621.0 µs | 850.0 µs | 27,095 QPS |

Hot − disjoint at p50 on nolock is 2.6 µs against a 55.5 µs noise floor (base: 12.0 µs
against 61.7 µs): the harness reports no family where there is none. Both binaries refuse
about 6 % of the hot statements (149 and 141 of 2,400) with `ERR TXN_CONFLICT retryable=1
row id=1 was written by transaction N`, the first-updater-wins refusal of `txn.md` §5: it is not the
family's, and it is the same on the binary with no family.

## 4. M3: the family under scenarios - every wait granted, almost none of them

### 4.1 Scenario 2 (`--bookers 8`, `group`): 84 parked asks in 19,909 committed bookings

The run **ended early**: the 24,000-cargo pool was exhausted before `--seconds 60`
(the driver says so: "the cargo pool ran out before --seconds did: every cargo was
booked"). 19,909 bookings committed at 2,374.1 TPS; the 26,990 booking attempts at 3,218/s
imply a ~8.4 s booking window (derived: 26,990 / 3,218). The TPS is an average over that
window.

| Phase | ops | errors | p0 | p25 | p50 | p95 | p99 | max | rate |
|---|---|---|---|---|---|---|---|---|---|
| booking | 26,990 | 0 | 386.4 µs | 1,270.6 µs | 2,473.5 µs | 3,257.2 µs | 4,773.5 µs | 276,666.1 µs | 3,218/s |
| commit | 19,909 | 0 | 380.0 µs | 710.4 µs | 846.5 µs | 1,276.7 µs | 1,946.3 µs | 275,436.3 µs | 2,374/s |
| operation-update | 19,913 | 2 | 40.3 µs | 82.0 µs | 96.0 µs | 201.0 µs | 310.7 µs | 19,280.5 µs | 2,375/s |
| org-update | 19,911 | 2 | 38.2 µs | 71.8 µs | 84.7 µs | 211.3 µs | 306.8 µs | 20,026.2 µs | 2,374/s |
| freight-insert | 19,913 | 0 | 46.7 µs | 95.5 µs | 117.5 µs | 249.4 µs | 417.0 µs | 18,614.9 µs | 2,375/s |
| charge-insert | 112,030 | 0 | 34.6 µs | 65.0 µs | 79.9 µs | 187.0 µs | 299.1 µs | 20,041.4 µs | 13,359/s |

Latencies are the driver's, over the window above. `sys.lock_waits`, read after the run:

| Relation | Unit | Mode | Outcome | Rows | Σ waited | max | p50 over all rows | mean over all rows |
|---|---|---|---|---|---|---|---|---|
| `operations_m3s2` | tuple | `X` | granted | 59 | 8,715 µs | 2,934 µs | | |
| `organizations_m3s2` | tuple | `X` | granted | 25 | 14,838 µs | 10,271 µs | | |
| **all** | tuple | `X` | **granted: 84; deadlock 0; `fault_net` 0; cancelled 0** | 84 | 23,553 µs | 10,271 µs | 76.0 µs | 280.4 µs |

The two relations are the two a booking's `UPDATE`s of a shared voyage (`operations`) and a
shared customer (`organizations`) write; 84 waits in 19,909 commits is 0.42 % of bookings.
Four `UPDATE`s were refused `TXN_CONFLICT` ("row id=... was written by transaction ...") -
2 on `operations`, 2 on `organizations` - and retried by the driver: these are the MVCC
window's refusals again, not lock refusals (the table granted every ask it parked).

| Wait accounting, scenario 2 (`sys.wait_stats`) | Since baseline (the run) | Share of DB time | Since boot (load included) |
|---|---|---|---|
| DB time | 72,054,473 µs | 100 % | 84,415,602 µs |
| `cpu.on_cpu` | 37,904,789 µs | 52.6 % | 38,623,811 µs |
| `commit.wal_flush` (durability) | 31,818,090 µs | 44.2 % | 43,373,341 µs |
| `sched.run_queue` | 1,742,031 µs | 2.4 % | 1,768,105 µs |
| `sysio.wal_write` | 383,792 µs | 0.53 % | 401,646 µs |
| `sysio.checkpoint` | 182,210 µs | 0.25 % | 225,138 µs |
| **`app.lock_wait`** (92 events, max 10,270 µs) | **23,561 µs** | **0.033 %** (0.028 % since boot) | 23,561 µs |
| `app.lock_conflict` | 92 events (equals the `lock_wait` count) | | 92 |

Of the 92 `app.lock_wait` events, 84 have a `sys.lock_waits` row; the other 8 are not in the
ring (`lock_waits_dropped = 0`, `lock_waits_overwritten = 0`) and are not explained. The
events by core: 29, 17, 18, 28 (cores 0-3), Σ 5,298 / 1,480 / 1,516 / 15,266 µs. The ASH
sampler (100 ms grid) caught **0** `app.lock_wait` samples: its median wait is 76 µs. The
family's latches: `contention_lock_partition_waits` 7,716 waits / 3,944 µs;
`contention_lock_wait_for_waits` 2 / 0 µs; `contention_inflight_asks` 4, misses 4.
Client round trip, durability and write statement are the same decomposition the table gives:
the booking's p50 is 2,473.5 µs of which `COMMIT` is 846.5 µs (p50); the parked asks are the
0.033 % row.

**Verify.** `--verify 100` ran 400 checks: **106 failed** (26.5 %); the first: "I1 operation
475: booked_cbm=14154000, SUM(freights.cbm)=14364000". This is the driver's client-side lost
update under READ COMMITTED (`SET booked_cbm = <literal>` from a value read earlier in the
transaction), the known question in `known-gaps.md`'s CA section and
`bench/v3.3.0/results-scenario2-freight-v3.3.0-38-ga40882af.md` §6, which saw 15-37 of 400
at 4,000 cargos and 1/8 cores. 106 of 400 is above that range and above the 8-12 % in
`known-gaps.md`; this run did not re-run under `--isolation repeatable-read` and does not
explain the higher rate (the shape differs: 24,000 cargos, 4 cores, early end). It is not
evidence about the family and is reported so the throughput above is not read as a clean run.

### 4.2 Scenario 0 (`--fk --txn`, 8 traders, `group`): zero waits, the family is a latch

Each business transaction is 2 trade `INSERT`s (the foreign key child, `trades.account_id` →
`accounts`) and 2 account `UPDATE`s; each trader owns a disjoint account partition, so **two
writers on one parent cannot occur** (the order's §1.3). 641,218 of the server's 650,200
`INSERT`s are the FK children, each of which takes the parent's `IS` and the row `S` per
`foreign-keys.md` (D9(a)) (the other 8,982 are loads and the reporter's profit rows; asks are not
tallied, so the count is the statement count, not an ask count).

| Phase | ops | errors | p0 | p25 | p50 | p95 | p99 | max | rate |
|---|---|---|---|---|---|---|---|---|---|
| txn | 320,609 | 0 | 595.1 µs | 1,215.4 µs | 1,397.0 µs | 1,885.7 µs | 2,346.8 µs | 286,770.0 µs | 5,344/s |
| trade-insert | 641,218 | 0 | 36.5 µs | 82.1 µs | 103.1 µs | 197.5 µs | 346.5 µs | 10,682.5 µs | 10,687/s |
| account-update | 641,218 | 0 | 35.7 µs | 72.0 µs | 87.1 µs | 168.6 µs | 345.9 µs | 7,916.9 µs | 10,687/s |

320,609 transactions committed in the 60 s window: 5,343.7 TPS, 21,374.6 statements/s,
0 rolled back, 0 torn, 20,471 skipped underfunded, balance verify 200 of 200 match.

| Wait accounting, scenario 0 (`sys.wait_stats`; baseline = boot, delta identical) | Value | Share of DB time |
|---|---|---|
| DB time | 337,694,054 µs | 100 % |
| `commit.wal_flush` (durability) | 285,934,600 µs | 84.7 % |
| `cpu.on_cpu` | 44,454,486 µs | 13.2 % |
| `sched.run_queue` | 5,678,405 µs | 1.7 % |
| `sysio.wal_write` | 1,449,961 µs | 0.43 % |
| `sysio.checkpoint` | 176,602 µs | 0.05 % |
| **`app.lock_wait`** | **0 events** | **0 %** |
| `app.lock_conflict`, `sys.lock_waits` rows, deadlock, `fault_net`, fence walks, in-flight asks | 0, 0, 0, 0, 0, 0 | |
| `contention_lock_partition_waits` | 149,134 waits, 219,546 µs (per core 56,619 / 52,732 / 53,990 / 56,204 µs) | 0.091 % of 240 core-seconds (4 cores × ~60 s); 0.065 % of DB time; 0.68 µs per committed transaction (derived: 219,546 / 320,609) |

The family leaves one trace on an uncontended workload: the 64-partition latch, hit by
unrelated keys that hash to one partition (149,134 waits at ~1.5 µs each). **C2's uncontended
price is not separable here** (no wait of any kind to attribute), and **no cell in S0
exercised the conversion deadlock**: its contender is the rig cell
`tests/fk_cross_core_rig_test.cpp:471`, which is a correctness shape and not a throughput
number.

## 5. M4: eight sessions, one hot row - the client sees the MVCC window, not a lock

Base only, `relaxed`, 8 sessions each issuing 4,000 ops per arm, sessions accepted on cores
0-3.

| Arm (8 sessions, base) | ops | errors | p0 | p25 | p50 | p95 | p99 | max | QPS |
|---|---|---|---|---|---|---|---|---|---|
| update-hot | 32,000 | 2,519 (7.9 %) | 36.5 µs | 178.4 µs | 282.9 µs | 727.9 µs | 1,019.5 µs | 9,915.2 µs | 23,170 QPS |
| update-disjoint | 32,000 | 0 (0.0 %) | 30.9 µs | 176.8 µs | 277.9 µs | 698.7 µs | 944.2 µs | 4,954.2 µs | 24,429 QPS |
| update-disjoint-again | 32,000 | 0 (0.0 %) | 37.1 µs | 175.4 µs | 280.6 µs | 726.1 µs | 1,015.2 µs | 5,238.8 µs | 23,813 QPS |
| insert-omitted | 32,000 | 0 (0.0 %) | 33.7 µs | 174.9 µs | 279.2 µs | 700.9 µs | 961.2 µs | 276,929.8 µs | 19,979 QPS |
| select-hot | 32,000 | 0 (0.0 %) | 49.1 µs | 244.8 µs | 383.2 µs | 936.2 µs | 1,278.8 µs | 3,816.8 µs | 17,876 QPS |

| Quantity | Value |
|---|---|
| hot − disjoint, p50 | +5.0 µs (277.9 → 282.9 µs) |
| noise floor (disjoint vs disjoint-again, p50) | 2.7 µs |
| `update-hot` refusals | 2,519 of 32,000 (7.9 %), first: `ERR TXN_CONFLICT retryable=1 row id=1 was written by transaction 35` |
| `app.lock_conflict` (all arms of the server) | 1,026 events (3.2 % of 32,000 hot statements if all are the hot arm's) |
| `sys.lock_waits` | 17 rows, all tuple `X`, all **granted**, Σ 688 µs, mean 40.5 µs, max 151 µs (cores 1, 2, 3: 2, 10, 5): 0.053 % of the hot statements parked |
| `app.lock_wait` | 1,026 events, 830 µs total, max 151 µs, 0.0083 % of 10,018,415 µs DB time |
| deadlock / `fault_net` / cancelled | 0 / 0 / 0 |

The +5.0 µs is 1.9 × the floor and its sample includes the refusal replies (7.9 % of the
hot arm's statements are error replies), so it is not a clean lock price. The 2,519 refusals
are `txn.md` §5's first-updater-wins over a version committed after the statement's
snapshot - READ COMMITTED's deliberate no-re-read window (`txn.md:769-773`) - **not a lock
refusal**: the family refused nothing, it parked 17 asks and granted them. The two counts
(2,519 client-visible refusals, 17 parks) are the finding: on a hot row the family's wait
is 100× rarer than the MVCC refusal in front of it.

## 6. M2: the slice `IS` costs 88-126 ns per leaf page (CP-Q3 (a) deletes it)

`bm_scan.py census` at `cores = 1` (so there is no partition latch: `lock_table.hpp:511-512`),
base against noslice, 5 runs of 12 blocks, 768 scans and 3,000 point statements per cell;
values are the median over runs with [min, max]. Same pages (603 and 304) and the same row
digest on both builds.

| Path | Scan (server CPU), base | Scan (server CPU), noslice | Δ per statement | Δ per leaf page | Δ per row | Share of base | Point control Δ per stmt |
|---|---|---|---|---|---|---|---|
| rows-100k (603 pages), literal | 2,155.2 [2,139.0, 2,195.3] µs | 2,105.0 [2,082.4, 2,157.2] µs | 52.8 [38.1, 68.4] µs | **88 [63, 113] ns** | 0.53 [0.38, 0.68] ns | 2.47 [1.73, 3.12] % | 0.35 [−0.07, 0.98] µs |
| rows-100k, prepared | 2,168.3 [2,148.2, 2,207.8] µs | 2,115.2 [2,080.6, 2,195.5] µs | 59.6 [−27.3, 73.4] µs | **99 [−45, 122] ns** | 0.60 [−0.27, 0.73] ns | 2.76 [−1.26, 3.36] % | 0.08 [−3.07, 0.16] µs |
| wide-a (304 pages), literal | 326.7 [309.5, 342.3] µs | 285.8 [280.8, 299.5] µs | 38.4 [27.2, 53.3] µs | **126 [90, 175] ns** | 3.84 [2.72, 5.33] ns | 12.03 [8.33, 15.58] % | −0.09 [−0.59, 0.44] µs |
| wide-a, prepared | 314.9 [298.5, 321.0] µs | 279.7 [271.9, 294.4] µs | 28.5 [22.8, 40.2] µs | **94 [75, 132] ns** | 2.85 [2.28, 4.02] ns | 9.04 [7.64, 12.87] % | 0.13 [−0.68, 0.26] µs |

| Path | Wall p50 base | Wall p50 noslice | Δ (derived) | Block spread base / noslice (server µs) |
|---|---|---|---|---|
| rows-100k, literal | 2,257.4 µs | 2,198.1 µs | −59.3 µs | 133.9 / 176.5 µs |
| rows-100k, prepared | 2,255.4 µs | 2,193.5 µs | −61.9 µs | 215.1 / 183.2 µs |
| wide-a, literal | 408.5 µs | 376.0 µs | −32.5 µs | 57.2 / 27.7 µs |
| wide-a, prepared | 393.1 µs | 360.2 µs | −32.9 µs | 35.0 / 38.9 µs |

The price is **per page, not per row**: 88-126 ns a page in both layouts while the per-row
figure moves seven-fold (0.53 vs 3.84 ns) with the rows per page (165.8 vs 32.9), so the
cost is the ask-and-release at each leaf page, and a wide-row scan, which has fewer rows per
page, pays 9-12 % of its CPU where a 166-row page pays 2.5-2.8 %. The four point controls
(no slice taken in a pk lookup) have medians within 0.35 µs of zero, which bounds the build-to-build
difference outside the slice. The one prepared-literal interval that crosses zero
(rows-100k prepared, min −27.3 µs) is one run in five. **AO-S7's extrapolation was "≲ 3 µs
× pages"; the measured unlatched price is 0.088-0.126 µs a page.** Not measured: the
latched price above one core (`bm_scan.py` has no `cores` flag, and a driver change inside
a measurement stage measures the driver). CP-Q3 (a) deletes the slice whatever this table
says; the table is the gain.

## 7. M5: a standing fence costs about 1 µs per uncovered point write

One session holds `id > 4080` in an open transaction (a range `X`, a fence); the writers
update rows `<= 4080` in autocommit, so none is ever covered, refused or parked
(`sys.lock_waits`: 0 rows in all four cells). Fenced and unfenced arms alternate in six
blocks. Each fenced statement walks the 64 partitions once (`contention_fence_walks`
24,000 per 24,000-statement arm, 6,000 per 6,000: 1.000 walks per statement; 0.000 unfenced).

| Cell | Arm | ops | p0 | p25 | p50 | p95 | p99 | max | mean | QPS |
|---|---|---|---|---|---|---|---|---|---|---|
| kwp-w4 | update-fenced | 24,000 | 31.9 µs | 98.3 µs | 133.8 µs | 259.8 µs | 349.3 µs | 2,026.7 µs | 143.4 µs | 27,027 QPS |
| kwp-w4 | update-unfenced | 24,000 | 40.0 µs | 99.2 µs | 136.4 µs | 265.4 µs | 364.1 µs | 3,673.2 µs | 146.4 µs | 26,378 QPS |
| kwp-w1 | update-fenced | 6,000 | 37.3 µs | 42.4 µs | 43.3 µs | 51.1 µs | 65.8 µs | 1,108.3 µs | 45.2 µs | 21,668 QPS |
| kwp-w1 | update-unfenced | 6,000 | 36.1 µs | 41.3 µs | 42.1 µs | 50.7 µs | 67.6 µs | 1,379.4 µs | 44.5 µs | 22,009 QPS |
| text-w4 | update-fenced | 24,000 | 24.3 µs | 44.4 µs | 56.3 µs | 92.0 µs | 126.7 µs | 2,497.0 µs | 61.4 µs | 61,351 QPS |
| text-w4 | update-unfenced | 24,000 | 21.6 µs | 46.6 µs | 54.9 µs | 99.7 µs | 152.0 µs | 4,400.6 µs | 62.2 µs | 61,797 QPS |
| text-w1 | update-fenced | 6,000 | 29.7 µs | 31.8 µs | 32.4 µs | 39.3 µs | 52.9 µs | 1,647.7 µs | 34.3 µs | 28,251 QPS |
| text-w1 | update-unfenced | 6,000 | 27.4 µs | 30.7 µs | 31.3 µs | 39.3 µs | 70.1 µs | 255,911.0 µs | 79.7 µs | 12,385 QPS |

QPS is statements over the arms' summed time and, for 4 writers, the aggregate of the four.
Client wall clock around one send and one receive.

| Cell | Δ p50 (fenced − unfenced) | Δ mean | Block p50 spread, fenced / unfenced | Partition latch waits, fenced / unfenced arm | Server CPU Δ per stmt (10 ms tick) |
|---|---|---|---|---|---|
| kwp, 4 writers | −2.6 µs | −3.0 µs | 19.3 / 6.9 µs | 927 / 530 | +0.83 µs (spread 15.00 µs) |
| kwp, 1 writer | +1.2 µs | +0.7 µs | 1.0 / 1.0 µs | 0 / 0 | +1.67 µs (spread 10.00 µs) |
| text, 4 writers | +1.4 µs | −0.8 µs | 1.0 / 3.5 µs | 0 / 0 | +1.25 µs (spread 10.00 µs) |
| text, 1 writer | +1.1 µs | −45.4 µs | 0.4 / 0.9 µs | 0 / 0 | −5.00 µs (spread 50.00 µs) |

The 4-writer kwp delta (−2.6 µs) is inside a 19.3 µs block spread; of the three small positive
deltas, +1.1 and +1.2 µs sit 0.2 µs above their spreads (0.9 and 1.0 µs) and +1.4 µs sits inside
its 3.5 µs spread. Reading
them together, **the walk costs about 1 µs per point statement here**, ~3 % of the 31-43 µs
single-writer statement. The text 1-writer unfenced arm's mean (79.7 µs, 12,385 QPS) is one
255.9 ms stall in 6,000 statements (255,911 µs / 6,000 = 42.7 µs of the mean), not an
effect of the fence; its p50 is unaffected. The server-CPU deltas come from the process's
`/proc` ticks at 100 Hz and their block spreads (10-50 µs) are far above the deltas: the
CPU column resolves nothing, and is shown so that it is not mistaken for evidence. The
latch waits for the 4-writer kwp cell (1,457 over the run) are the sum over **both** arms:
927 in the fenced blocks and 530 in the unfenced, 649 µs in all.

## 8. M6: under `group` the hot row's lock is released before the fsync

`tools/cp_s0_release_order.py`, text protocol, both sessions on core 0. Session A commits an
`UPDATE` of a row (text protocol, the newline debug surface); session B, released the instant
A's `COMMIT` has been sent (a median 46.9 µs after A's send in the group arm), updates the same
row. If A's lock were held across the group fsync, B would park and answer after A's
acknowledgement.

| Arm | Iterations | B replied before A's `COMMIT` ack | Margin A_ack − B_reply p50 / p90 / p99 / min | A `COMMIT` ack p50 / p90 / p99 | B `UPDATE` latency p50 / p90 / p99 | Margin ÷ ack (p50) | B parked (`sys.lock_waits`) | `app.lock_wait` |
|---|---|---|---|---|---|---|---|---|
| **group** | 300 of 300 usable | **300 of 300 (1.0)** | 423.0 / 643.0 / 933.3 / 55.6 µs | 539.0 / 736.4 / 1,039.9 µs | 52.1 / 89.8 / 118.9 µs | 0.785 | 0 rows | 0 events |
| strict (control) | 300 of 300 usable | 4 of 300 (0.0133) | −31.4 / −12.7 / 24.6 / −170.0 µs | 536.8 / 680.5 / 1,214.9 µs | 524.4 / 686.6 / 1,105.1 µs | −0.058 | 305 rows (tuple `X`, granted, p50 440.0 / p90 570.0 / p99 972.0 µs) | 305 events, 142,202 µs |

Final value of the row in each arm: 305 (expected 305). Under `group`, B's latency (52.1 µs)
is a bare update: it arrives after the release and parks on nothing, while A's
acknowledgement is still a median 423 µs away (A's `commit.wal_flush` wait: 154,167 µs over
305 commits, session 2). In none of the 300 iterations did B arrive before A's release: the
relation has no `sys.lock_waits` row. Under `strict` the borrows are held across the sync by design
(BA-S7) and B waits it out: B's latency is 0.977 of A's acknowledgement (524.4 vs 536.8 µs).
The strict arm is the check that the harness can see a held lock. **`known-gaps.md`'s CA
entry ("the hot row's lock held across the group fsync") is contradicted by this cell and
was corrected on this evidence.** The two-core rig cell
`tests/lock_release_before_sync_rig_test.cpp` pins the same order; its run is the suite's,
reported in the order's §7, not here.

## 9. Against the order's rules

| Candidate | S0 cell | Measured | The order's rule (§3) | Reading |
|---|---|---|---|---|
| **C1** the slice `IS` | M2 | 88-126 ns per leaf page; 2.5-12 % of a scan's server CPU (603-page and 304-page scans) | deleted on CP-Q3 (a) whatever M2 shows | deleted; the gain is the table of §6, unlatched, at one core |
| **C2** the `U` mode | M3 | scenario 0: 0 lock waits, 0 conflicts, the family a latch (0.09 % of core-seconds); scenario 2: 84 granted waits | built if the rig cell's deadlock or §2c's wait has a client-reachable contender; recorded otherwise | the uncontended price is not separable; no S0 cell exercised the contender (the rig cell `fk_cross_core_rig_test.cpp:471` is the contender); decided in S1 on the rig cell and §2c, not on a throughput number |
| **C3** the per-relation lock header | M5, M1 | ≈ 1 µs per fenced point write at 4 cores (+1.1, +1.2, +1.4 µs; −2.6 µs in a 19.3 µs spread); the relation ask is inside M1's ~0.4 µs | built only if M1 puts the relation ask, or M5 the walk, "at or above a point write's floor at this box's core count" | the walk is ~3 % of a 31-43 µs statement; two of the three small cells sit 0.2 µs above their block spreads, the third (text, 4 writers) inside its 3.5 µs spread; read against a point write's p0 (~36 µs) it is not at the floor; read against M1's 0.2 µs noise floor it is above it - **S1 decides which "floor" the rule means**; BP-H10's +12 % at eight cores was not reproduced because eight cores were not run |
| **C4** the row entry made lazy | M1, M3, M4 | the family's whole share 0.4 µs (~1 %) of a point write; 17 parks in 32,000 hot statements, 84 in 19,909 bookings, all granted | built if M1 puts A5's ask-and-release at or above a point write's floor, or M3 shows header-named waits ending at the net | M1 and M3 do not say "build" by themselves; A5 is inside the 0.4 µs and was not separated from A1; the hot row's client-visible refusal is `txn.md` §5's, which C4 (α) does not move |
| **C5** fairness | M3, M4 | `fault_net` rows: 0 in scenario 2, scenario 0, M4 | nothing built while the count is zero (CP-Q9 (a)) | nothing built |
| **CP-Q5** release before durability | M6 | 300 of 300 (group); 4 of 300 (strict) | confirm with a cell, correct `known-gaps.md` | confirmed; corrected |

## 10. What the run teaches about the engine

1. **The family is cheap where it is uncontended and cheap where it is contended.** 0.4 µs
   of a 39 µs write; 0.033 % of a contended scenario's DB time; 0.0083 % under eight sessions
   on one row. The costs the older notes feared (AR2 E1/E12's per-statement tax) are not on
   these shapes. The structure CP proposes to reshape is not where the time goes: durability
   is (`commit.wal_flush` 44 % of scenario 2's DB time, 84.7 % of scenario 0's).
2. **The slice `IS` is the family's one cost that scales with something the client chooses**
   (pages walked), and it is per page: 88-126 ns whatever the rows per page. AO-S7's
   "≲ 3 µs × pages" was a bound thirty times too high at one core; whether the latched figure
   above one core is closer was not measured.
3. **The hot row's wait is not the hot row's problem.** The lock family granted every ask it
   parked (101 parks across M3 and M4, none refused); what a client sees on a hot row is the
   first-updater-wins refusal of a version committed after its statement's snapshot, 7.9 % of
   M4's hot statements. A wait-on-the-decide structure (C4) cannot relieve that.
4. **`known-gaps.md`'s CA explanation of scenario 2's fsync-bound throughput was wrong.** The
   row's lock is released before the group sync; what each booker waits for is its own
   acknowledgement in a closed loop (M6's B answers 423 µs before A's ack).
5. **A standing fence is a ~1 µs tax on every point statement of the relation**, uniform
   (one walk per statement) and not contended at four cores (a latch wait rate of 927 in
   24,000 fenced statements against 530 unfenced). It is small here, and the order's
   unmeasured question is whether 64 partitions × eight cores moves it.
6. **The wait telemetry works:** `app.lock_wait` and `sys.lock_waits` agree on 84 of 92
   events in scenario 2 (the other 8 unexplained), the strict control produced exactly 305
   rows for 300 iterations plus 5 warm-up, and the ASH sampler at 100 ms never saw a 76 µs median wait - it
   cannot price this family, `sys.wait_stats` can.

## 11. Baselines

This engine's own last number for these shapes: **none**. The older numbers are cited for
their definitions and no delta is taken, because the sizes or definitions differ.

| Source | What it says | Why no delta |
|---|---|---|
| AO-S7, `v2.7.0-304-g5e94dc8` (the results file left the tree; the order's §1.4 keeps the numbers) | the family under 3 % of a statement, zero refusals at 8 and 32 sessions; the slice `IS` extrapolated "≲ 3 µs × pages", never measured | measured on an engine where two writers could not meet; contended minus disjoint inside floors is a different definition from M1's nolock build. M2 is the first measurement of the slice |
| BP-H10 | `64 × cores` partitions: +12 % of a statement's CPU at eight cores under a held fence | eight cores, CPU; M5 is four cores, client p50, wall. Not reproduced at four; not tested at eight |
| CA-R1, `bench/v3.2.0/results-ca-r1-visibility-v3.2.0-154-g7112d4a1.md` | scenario 2, 8 bookers, `group`: 2,108 booking ops/s, 1,537 committed TPS | `--cargos 10000 --bookings 9000` there, `--cargos 24000` (run ended at pool exhaustion) here; different commit |
| `bench/v3.3.0/results-scenario2-freight-v3.3.0-38-ga40882af.md` | scenario 2 at 4,000 cargos, 1 and 8 cores | a different size and core count |
| PostgreSQL | - | the floor was not measured for any of these shapes |

## 12. Reproduce

Binary copies and patches: `archive/.../patches/`; the run scripts: `archive/.../scripts/`
(`run_all.sh` and `run_all2.sh` are the two passes; each cell's script is named for its
cell). The measurement builds are the base tree with a patch applied, built with
`cmake --build build-release`, then copied.

```
# M1 and the proof arm (lock_contention_benchmark.py; flags in scripts/m1m4_run_m1m4.sh)
lock_contention_benchmark.py --port P --label base --ab-port Q --ab-label nolock \
    --sessions 1 --rows 256 --ops 40000 --blocks 4 --json m1-rep1.json
lock_contention_benchmark.py ... --sessions 8 --rows 256 --ops 300 --blocks 4 --prove
# M4: --sessions 8 --rows 256 --ops 4000 (base only)
# M3
scenario2_freight.py --suffix m3s2 --organizations 300 --ships 30 --operations 300 \
    --cargos 24000 --verify 100 --seed 1 --sync --bookers 8 --seconds 60 --bookings 0
scenario0_stockmarket.py --suffix m3s0 --users 2000 --assets 2000 --traders 8 --seconds 60 \
    --fk --txn --verify 200 --seed 1 --sync
# M2
bm_scan.py census --arm base=BIN --arm noslice=BIN --runs 5 --blocks 12 --scan-ops 768 \
    --point-ops 3000 --cells rows-100k,wide-a --pin-server 2 --pin-driver 3
# M5, M6: tools/cp_s0_fence.py and tools/cp_s0_release_order.py (--ops 1000 --blocks 6;
#   --iterations 300 over the text protocol); docstrings are the flag reference
```

Row counts: M1/M4's `--rows 256` is the table size (each session owns one row, `update-hot`
shares one); M5's 4,096 rows with the fence at `id > 4080` covers 16 rows; M2's cells are
`rows-100k` (100,000 rows, 603 leaf pages) and `wide-a` (10,000 rows, 304 leaf pages).

## 13. Limits and what was cut

- **Time budget** (CP-Q8 (a), 20 minutes for the stage): pass 1 344 s + pass 2 241 s =
  585 s = 9.75 minutes of measurement, plus the two measurement builds and a 10 s smoke test
  before pass 1. Cut: M1's first pass ran one rep (12,000 ops) and was re-run as three
  (40,000); **M5's first attempt (07:29:06 UTC) lost all four cells** to a relation name
  with a hyphen refused at `CREATE TABLE` ("expected '(', got '-' at byte 25"; only its
  `run.txt` is kept) and was re-run in pass 2; scenario 2 at 24,000 cargos ended at 8.4 s.
- **Row-set sweep (rule 9) not done** at 200 / 1K / 10K. A pk point `UPDATE` does not scale
  with rows, but M1 ran one size (256 rows) and M5 one (4,096); M2 is the only cell with two
  sizes (10,000 and 100,000 rows) and they are the point of that cell.
- **M1 is two clean reps.** The two biased reps (labels on core 0 and a peer) are shown, not
  used. A1 (the relation ask) was not separated from A5 (the tuple and its header check).
- **Not measured:** the latched slice `IS` above one core (M2 is `cores = 1`); the
  eight-core fence walk (BP-H10's shape); `strict` durability for M1-M5 (all `relaxed` or
  `group`; `strict` only as M6's control); a shared-parent arm for C2 (a driver flag, not
  admitted inside a measurement stage); the conversion deadlock.
- The M1 `cpu.on_cpu` cross-check is a whole-server counter over a mixed stream, not a
  per-statement price.
- Scenario 2's `--verify` fails 106 of 400 checks (§4.1); the throughput there is not a
  clean-run figure.
- **Tests:** no engine code changed; the two measurement patches are not landed. This
  document ran no test suite (a Debug build and the full suite were running on the host
  when it was written). The new rig cell's run is the suite's, reported in the order's §7.
