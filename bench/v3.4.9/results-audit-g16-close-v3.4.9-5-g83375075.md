# Audit G16-G20 close measurement: the Brent-guarded leaf walks cost nothing the scan census can find, and nothing else moved

**Thesis.** The milestone (`63fec9d6`, `v3.4.9-2-g63fec9d6`, against `83375075`, `v3.4.9-5-g83375075`) replaced the page counter that bounded
every user-relation and index leaf walk with a Brent cycle check (`storage::LeafChainGuard`, one `Visit` per leaf), cut the assertion snapshot
chunks and the checkpoint and admission visit budgets from about 1 MiB to about 272 KiB, guarded the loser's undo walk at mount, and
changed two creation paths. Measured as an interleaved A/B in Release on one host, **no shape regressed.** The shape the change was
most likely to touch, the scan, costs **no more per leaf page on the closing build, and probably a little less: A minus B reads +15 to +25 ns per page (0.7-2.1 % of the scan's server CPU) in the median of every census path**, with the ranges of the two literal paths crossing zero in one or two of five runs (M2).
A single-session point `UPDATE`/`INSERT`/`SELECT` is equal to 0.3 µs at p50 on the two placement-matched repetitions (M1); eight sessions on one hot row read the same refusals and the same granted waits (M4);
scenario 0 reads -0.3 % (inside a 2 % spread); **scenario 2 reads +1.8 % on the closing build** with both builds' own spreads at 0.2 %, which is outside the spread of two runs per build
and not a cost, but I do not attribute it to a change (the run does not isolate one); a server start-to-ready is equal to the baseline (+1.2 and +1.4 ms on 21 starts, inside a 9-30 ms range).
The assertion cell did not resolve its question: it is confounded by session placement and does not show that the 272 KiB snapshot path was exercised (§8).

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-11-14 (UTC). Builds 07:02-07:04; census 07:04:37-07:07:08; M1 07:07:08-07:11:13, label-swapped M1 07:11-07:14; M4 07:15:06-07:16:01; scenario 2 x4 and scenario 0 x4 07:16:01-07:25:19; mount and assertion cells 07:25:41-07:27:02. About 24 minutes of measurement, 27 from the first command, inside the one-hour budget |
| Worktree / branch | `/home/louis/ckdbs/.claude/worktrees/audit-g1-g15-fixes` on `worktree-audit-g16-g20-fixes`; measured from two detached `git worktree`s of the exact commits (each clean), removed afterwards. The tree's HEAD at writing was `5f7287a9` (documents, not a build input; `tools/` is byte-identical to the measured trees' drivers it ran) |
| **A (baseline, the milestone's open)** | `63fec9d6` = `v3.4.9-2-g63fec9d6`, committed 2026-11-14 04:32:38 UTC. Release (`-DCMAKE_BUILD_TYPE=Release`, `--target kds_server`), binary mtime 07:04:28 UTC (after the commit). Copy `kds_server-A2`, 7,603,928 bytes, sha256 `f7e6c034de352ca65911345befdbb00ca8374e9d861774e5ef59459174aed35a` |
| **B (closing)** | `83375075` = `v3.4.9-5-g83375075`, committed 2026-11-14 07:01:31 UTC. Release, binary mtime 07:04:28 UTC (after the commit). Copy `kds_server-B2`, 7,616,704 bytes, sha256 `5dd26c9ae8b7f0cfcf126f4e0f1dc3ba28340175b7ac41af5667c2ba2e5a1644` |
| Binary discipline | every server of every cell started from the two copies (on `/tmp`, tmpfs: binary only). The spill/mount driver writes a config without the required `buffer_pool_frames` key; it was run unmodified through the same one-line `exec` wrapper as the earlier closes (`archive/.../scripts/wrap-A2.sh`, `wrap-B2.sh`) |
| Device | data files and WALs under `/home/louis/bench-runs/audit-g16-close/` on `/dev/nvme0n1p1`, ext4 |
| Host | 8 vCPU Intel Xeon Platinum 8581C @ 2.10 GHz, Linux 6.12.116. The host sampler counted `procs=0` (no compiler, `cmake --build` or test process) in every sample of the main pass; the census's own quiet gate recorded loads of 1.15-1.91 and `competing: none` per run; one-minute load before the first cell was the decaying average of the builds, and the cells began after the compilers exited |
| Server config | M1, M4: `cores = 4` on CPUs 2-5, drivers on 6-7, `relaxed`; M3: `cores = 4` on 2-5, drivers on 0,1,6,7, `group`, `ash_interval_ms = 100`; M2: `cores = 1`, `relaxed`, server on CPU 2, driver on 3; mount and assertion cells as stated in §7-§8. `buffer_pool_frames = 65536`, `ash_enabled = on` throughout. Fresh server and volume per cell |
| Drivers | unmodified `tools/bm_scan.py census` (M2), `tools/lock_contention_benchmark.py` (M1, M4), `tools/scenario2_freight.py`, `tools/scenario0_stockmarket.py` (M3), `tools/mount_cost_benchmark.py` (mount), `tools/assertion_overhead_benchmark.py`; the scripts are the CP close's (`bench/v3.4.7/archive/cp-close-v3.4.7-11-g7d21468a/scripts/`) with paths set through environment variables, plus this run's `master4.sh` and `master5.sh` in the archive |
| Prior numbers | this engine's own: `bench/v3.4.8/results-audit-g-close-v3.4.8-16-g713a84d0.md` (M1, M3, M4, mount, spill) and `bench/v3.4.8/results-audit-g-close-v3.4.8-20-g4cf04d4c.md` (mount); and the CP close `bench/v3.4.7/results-cp-close-v3.4.7-11-g7d21468a.md` (M2 and the scenarios). Cross-day deltas are context only, as those files say; the A/B inside this run is the comparison. PostgreSQL floor: **not measured for any of these shapes** |
| Raw outputs | `bench/v3.4.9/archive/audit-g16-close-v3.4.9-5-g83375075/` (driver JSON and text, views, census JSON, status and host logs, scripts; no data file, WAL segment or server log) |
| Tests | no engine code was changed by this measurement; **no test suite was run by it** (the coordinator reports 4455 / 4455 green on the closing tree) |

## 2. What was measured

A is `63fec9d6`, B is `83375075`; a delta is B minus A unless a table says otherwise (the census reports A minus B, labelled).

| Cell | Question | Driver and arms | Size |
|---|---|---|---|
| M2 | what the leaf walk's Brent guard costs per page | `bm_scan.py census --arm base=A --arm close=B`, 5 runs x 12 blocks, literal and prepared paths, fresh server per arm and run | `rows-100k` (100,000 rows, 603 leaf pages), `wide-a` (10,000 wide rows, 304 pages); 768 scans (192 on rows-100k) and 3,000 point statements per cell and run |
| M1 | do the ordinary paths regress | `lock_contention_benchmark.py --sessions 1`, A against B over 4 blocks, 6 reps: 3 with A as the driver's primary label, 3 swapped | 256 rows; 40,000 ops/arm/rep |
| M3 | throughput and waits under two scenarios | `scenario2_freight.py --bookers 8` x2 per build (A,B,B,A); `scenario0_stockmarket.py --fk --txn` x2 per build (A,B,B,A) | scenario 2: 300 organizations, 30 ships, 300 operations, 24,000 cargos; scenario 0: 2,000 users, 2,000 assets, 8 traders |
| M4 | eight sessions on one hot row | `lock_contention_benchmark.py --sessions 8` | 256 rows; 4,000 ops/session/arm |
| M5 | mount start-to-ready (only the undo walk changed) | `mount_cost_benchmark.py --mounts 7`, A and B alternated, 3 runs each: empty log, 4,000 rows clean stop, 4,000 rows after SIGKILL | 64 MiB segment |
| M6 | an assertion on a relation | `assertion_overhead_benchmark.py --ops 8000 --blocks 16`, A against B | `--preload` 200 / 1,000 / 10,000 rows (and groups) |

**What was cut.** No wide-row *update* walk or write-statement census exists in `tools/` (`bm_scan.py` has a `writers` mode, which was not used: the dispatcher's whole-relation write walk is
reached by `UPDATE ... WHERE <non-key>` statements, and no driver here times one with a server-CPU split; it is therefore **not measured**). The index leaf walk (`IndexVisitFrom`) was not given its own
cell: no census cell here walks a secondary index. M5 spans no log-fill axis. No PostgreSQL twin was run. No test suite was run.

## 3. M2 - the scan: the Brent guard costs nothing the census can find; B is a little faster per page

`bm_scan.py census` at `cores = 1`, A (label `base`) against B (label `close`), 5 runs of 12 blocks; each statement's **server on-CPU time** (the sum over server threads of schedstat) is the measured quantity.
Same page counts on both builds (603, 304), same row digest on both (`1:d417942a30e5651b`, `1:5fa29e54b8b3a1a4`), 0 bad scans. Medians over runs, [min, max]. **Δ is A minus B** (positive = B cheaper).

| Path | Scan server CPU, A | Scan server CPU, B | Δ per statement | Δ per leaf page | Δ per row | Share of A | Point control Δ per statement |
|---|---|---|---|---|---|---|---|
| rows-100k (603 pages), literal | 2,099.4 [2,075.2, 2,285.9] µs | 2,075.9 [2,064.8, 2,191.1] µs | 15.2 [-87.5, 210.5] µs | 25 [-145, 349] ns | 0.15 [-0.87, 2.10] ns | 0.73 [-4.16, 9.21] % | +0.12 [-0.08, 0.40] µs |
| rows-100k, prepared | 2,090.9 [2,078.1, 2,284.9] µs | 2,068.9 [2,059.1, 2,078.9] µs | 14.8 [9.1, 216.5] µs | 24 [15, 359] ns | 0.15 [0.09, 2.17] ns | 0.71 [0.44, 9.48] % | -0.32 [-2.50, 0.14] µs |
| wide-a (304 pages), literal | 284.0 [281.0, 295.9] µs | 280.6 [277.6, 283.4] µs | 4.6 [-0.6, 14.2] µs | 15 [-2, 47] ns | 0.46 [-0.06, 1.42] ns | 1.61 [-0.20, 4.79] % | +0.05 [-0.28, 0.60] µs |
| wide-a, prepared | 278.4 [276.2, 291.9] µs | 274.7 [270.6, 285.7] µs | 6.1 [1.5, 9.8] µs | 20 [5, 32] ns | 0.61 [0.15, 0.98] ns | 2.11 [0.56, 3.42] % | -0.15 [-0.31, 0.03] µs |

Per run (A / B server CPU in µs, Δ), the raw cells behind the medians:

| Path | Run 1 | Run 2 | Run 3 | Run 4 | Run 5 |
|---|---|---|---|---|---|
| rows-100k literal | 2,103.6 / 2,191.1, -87.5 | 2,285.9 / 2,075.4, +210.5 | 2,079.9 / 2,064.8, +15.2 | 2,099.4 / 2,075.9, +23.6 | 2,075.2 / 2,076.0, -0.8 |
| rows-100k prepared | 2,084.4 / 2,059.1, +25.3 | 2,284.9 / 2,068.4, +216.5 | 2,078.1 / 2,068.9, +9.1 | 2,090.9 / 2,078.9, +12.0 | 2,091.2 / 2,076.4, +14.8 |
| wide-a literal | 285.6 / 280.6, +5.0 | 295.9 / 281.7, +14.2 | 282.8 / 283.4, -0.6 | 281.0 / 277.6, +3.4 | 284.0 / 279.4, +4.6 |
| wide-a prepared | 278.4 / 273.2, +5.2 | 291.9 / 285.7, +6.1 | 276.8 / 270.6, +6.2 | 276.2 / 274.7, +1.5 | 286.3 / 276.5, +9.8 |

| Path | Wall p50 A | Wall p50 B | Wall p90 A / B | Wall p99 A / B |
|---|---|---|---|---|
| rows-100k, literal | 2,180.8 µs | 2,171.2 µs | 2,620.1 / 2,314.5 µs | 3,059.3 / 3,048.1 µs |
| rows-100k, prepared | 2,179.0 µs | 2,165.9 µs | 2,626.5 / 2,289.2 µs | 3,044.6 / 2,749.5 µs |
| wide-a, literal | 370.0 µs | 363.3 µs | 458.8 / 448.1 µs | 642.2 / 628.6 µs |
| wide-a, prepared | 360.3 µs | 353.4 µs | 440.3 / 430.0 µs | 634.0 / 643.6 µs |

(The census reports wall p50/p90/p99 pooled over runs, 192 or 768 scans per path per run, not p0/p25; the p0/p25 of a scan are not in its output.)

**Reading: no regression; a small saving, resolved on some paths and not on others.** Of the 20 per-run comparisons, B is cheaper in 17 and dearer in 3 (rows-100k literal runs 1 and 5, wide-a literal run 3), the three by 87.5, 0.8 and 0.6 µs.
The one large figure on A, run 2 (2,285.9 and 2,284.9 µs, +210 and +216 µs), is a single noisy baseline run: both rows-100k paths of the same run on the same server are high together, and B's own run 1 on the literal path is the high one
(2,191.1 µs). Excluding run 2, the rows-100k medians are 15-25 ns/page on the prepared path, with all four remaining runs between +9 and +25 µs, and the literal path is unresolved (-87.5 to +23.6 µs). The wide-a prepared path has its whole range above zero (+1.5 to +9.8 µs, 5 of 5);
wide-a literal has 4 of 5 above zero. Point controls are inside 0.4 µs of zero on every path, so the host did not shift between the arms. **What is excluded is a cost: a Brent check that added work per page would show as a negative delta, and no path's median is negative**; the guard's per-page cost is
bounded by the run-to-run floor, about 5 µs on the wide-a cells (16 ns per page) and about 20 µs on rows-100k (33 ns per page). Wait breakdown: **does not apply** to this cell (one session scanning, `relaxed`, no waits; the measured quantity is the server's CPU).

**Delta against the CP close** (`v3.4.7-11-g7d21468a`; that close's "close" build is the nearest same-shape number): its scan CPU was 2,096.2 / 2,103.5 µs (rows-100k) and 284.7 / 280.7 µs (wide-a). A here reads 2,099.4 / 2,090.9 and 284.0 / 278.4 µs, i.e. the CP close's level to within 0.7 %, and B reads
2,075.9 / 2,068.9 and 280.6 / 274.7 µs, 0.9-2.0 % under it. The CP close's finding stands: the host's per-core speed on this path has not moved, so the milestone's change is the only difference between A and B, and B is no slower.

## 4. M1 - the ordinary paths: equal where the placement is equal

One session, autocommit statements on its own row, `relaxed`; one-session pools land on whichever core SO_REUSEPORT picks. As in the earlier closes, **a pair is only comparable if both sessions sit on the same kind of core**: core 1 reads 9-20 µs slow on every arm (both servers on core 1 in rep 1.1 read 51-52 µs
instead of 40 µs), and core 0 carries the ticks. In every unmatched rep the slower arm is the one on core 0 or core 1, whichever build it was (1.2: B on core 0, update-hot slow; 1.3: A on core 1 slow; 2.1: A on core 0 slow; 2.3: A on core 0, insert-omitted slow). Reps 1.1 (both on core 1) and 2.2 (A on 3, B on 2) are matched.

| Rep | Core A / B | Placement | Arm | A p50 | B p50 | Δ p50 (B - A) | A QPS | B QPS | QPS, B vs A |
|---|---|---|---|---|---|---|---|---|---|
| **1.1** | 1 / 1 | **matched (both core 1)** | update-disjoint | 51.9 µs | 51.6 µs | -0.3 µs | 19,414 | 19,758 | +1.8 % |
| **1.1** | 1 / 1 | **matched** | update-hot | 51.7 µs | 50.9 µs | -0.8 µs | 19,323 | 19,767 | +2.3 % |
| **1.1** | 1 / 1 | **matched** | insert-omitted | 48.3 µs | 48.0 µs | -0.3 µs | 19,101 | 19,158 | +0.3 % |
| **1.1** | 1 / 1 | **matched** | select-hot | 72.4 µs | 71.6 µs | -0.8 µs | 13,844 | 14,805 | +6.9 % |
| 1.2 | 1 / 0 | unmatched | update-disjoint | 52.2 µs | 42.6 µs | -9.6 µs | 18,781 | 20,978 | +11.7 % |
| 1.2 | 1 / 0 | unmatched | update-hot | 41.9 µs | 52.5 µs | +10.6 µs | 21,277 | 18,139 | -14.7 % |
| 1.2 | 1 / 0 | unmatched | insert-omitted | 49.2 µs | 39.0 µs | -10.2 µs | 17,977 | 21,539 | +19.8 % |
| 1.2 | 1 / 0 | unmatched | select-hot | 72.0 µs | 56.0 µs | -16.0 µs | 14,323 | 15,821 | +10.5 % |
| 1.3 | 1 / 2 | unmatched | update-disjoint | 45.4 µs | 40.6 µs | -4.8 µs | 20,556 | 22,905 | +11.4 % |
| 1.3 | 1 / 2 | unmatched | update-hot | 44.3 µs | 40.7 µs | -3.6 µs | 20,664 | 22,837 | +10.5 % |
| 1.3 | 1 / 2 | unmatched | insert-omitted | 48.4 µs | 38.7 µs | -9.7 µs | 19,066 | 21,141 | +10.9 % |
| 1.3 | 1 / 2 | unmatched | select-hot | 72.0 µs | 51.8 µs | -20.2 µs | 13,757 | 18,378 | +33.6 % |
| 2.1 | 0 / 3 | unmatched | update-disjoint | 51.7 µs | 40.6 µs | -11.1 µs | 19,292 | 22,522 | +16.7 % |
| 2.1 | 0 / 3 | unmatched | update-hot | 51.5 µs | 40.5 µs | -11.0 µs | 19,812 | 22,852 | +15.3 % |
| 2.1 | 0 / 3 | unmatched | insert-omitted | 48.5 µs | 38.7 µs | -9.8 µs | 19,257 | 21,359 | +10.9 % |
| 2.1 | 0 / 3 | unmatched | select-hot | 52.9 µs | 52.0 µs | -0.9 µs | 16,939 | 18,163 | +7.2 % |
| **2.2** | 3 / 2 | **matched {2,3}** | update-disjoint | 40.4 µs | 40.7 µs | +0.3 µs | 22,958 | 22,736 | -1.0 % |
| **2.2** | 3 / 2 | **matched** | update-hot | 40.6 µs | 40.5 µs | -0.1 µs | 22,795 | 22,758 | -0.2 % |
| **2.2** | 3 / 2 | **matched** | insert-omitted | 39.0 µs | 38.7 µs | -0.3 µs | 21,097 | 21,350 | +1.2 % |
| **2.2** | 3 / 2 | **matched** | select-hot | 52.0 µs | 51.9 µs | -0.1 µs | 17,973 | 18,210 | +1.3 % |
| 2.3 | 0 / 2 | unmatched | update-disjoint | 40.6 µs | 40.6 µs | +0.0 µs | 22,526 | 22,745 | +1.0 % |
| 2.3 | 0 / 2 | unmatched | update-hot | 40.6 µs | 40.5 µs | -0.1 µs | 22,735 | 23,012 | +1.2 % |
| 2.3 | 0 / 2 | unmatched | insert-omitted | 43.7 µs | 38.9 µs | -4.8 µs | 19,719 | 21,142 | +7.2 % |
| 2.3 | 0 / 2 | unmatched | select-hot | 52.4 µs | 52.0 µs | -0.4 µs | 17,114 | 17,984 | +5.1 % |

(Reps 1.x have A as the driver's primary label, 2.x have B.) The two matched reps' full distributions:

| Rep | Arm | Build | ops | p0 | p25 | p50 | p95 | p99 | mean | QPS |
|---|---|---|---|---|---|---|---|---|---|---|
| 1.1 | update-disjoint | A | 40,000 | 38.5 µs | 42.6 µs | 51.9 µs | 59.0 µs | 69.4 µs | 50.5 µs | 19,414 QPS |
| 1.1 | update-disjoint | B | 40,000 | 35.1 µs | 41.2 µs | 51.6 µs | 59.5 µs | 70.5 µs | 49.6 µs | 19,758 QPS |
| 1.1 | update-disjoint-again | A | 40,000 | 38.6 µs | 41.2 µs | 51.1 µs | 57.9 µs | 67.8 µs | 49.1 µs | 19,955 QPS |
| 1.1 | update-disjoint-again | B | 40,000 | 36.8 µs | 40.8 µs | 50.5 µs | 58.7 µs | 68.3 µs | 48.6 µs | 20,188 QPS |
| 1.1 | update-hot | A | 40,000 | 38.4 µs | 41.1 µs | 51.7 µs | 60.1 µs | 108.4 µs | 50.8 µs | 19,323 QPS |
| 1.1 | update-hot | B | 40,000 | 38.3 µs | 40.8 µs | 50.9 µs | 59.8 µs | 107.6 µs | 49.7 µs | 19,767 QPS |
| 1.1 | insert-omitted | A | 40,000 | 35.5 µs | 39.0 µs | 48.3 µs | 55.8 µs | 70.7 µs | 51.5 µs | 19,101 QPS |
| 1.1 | insert-omitted | B | 40,000 | 35.3 µs | 38.7 µs | 48.0 µs | 55.0 µs | 68.5 µs | 51.4 µs | 19,158 QPS |
| 1.1 | select-hot | A | 40,000 | 48.5 µs | 71.2 µs | 72.4 µs | 80.7 µs | 95.1 µs | 71.2 µs | 13,844 QPS |
| 1.1 | select-hot | B | 40,000 | 48.9 µs | 52.8 µs | 71.6 µs | 80.2 µs | 97.3 µs | 66.6 µs | 14,805 QPS |
| 2.2 | update-disjoint | A | 40,000 | 35.4 µs | 39.8 µs | 40.4 µs | 49.4 µs | 58.6 µs | 42.8 µs | 22,958 QPS |
| 2.2 | update-disjoint | B | 40,000 | 38.2 µs | 39.9 µs | 40.7 µs | 50.5 µs | 61.8 µs | 43.2 µs | 22,736 QPS |
| 2.2 | update-disjoint-again | A | 40,000 | 37.1 µs | 39.9 µs | 40.6 µs | 50.1 µs | 61.8 µs | 43.1 µs | 22,800 QPS |
| 2.2 | update-disjoint-again | B | 40,000 | 38.1 µs | 39.8 µs | 40.5 µs | 49.9 µs | 61.0 µs | 43.0 µs | 22,837 QPS |
| 2.2 | update-hot | A | 40,000 | 38.3 µs | 39.8 µs | 40.6 µs | 50.0 µs | 63.7 µs | 43.1 µs | 22,795 QPS |
| 2.2 | update-hot | B | 40,000 | 37.3 µs | 39.8 µs | 40.5 µs | 50.1 µs | 65.0 µs | 43.2 µs | 22,758 QPS |
| 2.2 | insert-omitted | A | 40,000 | 35.5 µs | 38.3 µs | 39.0 µs | 48.9 µs | 62.6 µs | 46.8 µs | 21,097 QPS |
| 2.2 | insert-omitted | B | 40,000 | 33.7 µs | 38.0 µs | 38.7 µs | 48.7 µs | 64.1 µs | 46.2 µs | 21,350 QPS |
| 2.2 | select-hot | A | 40,000 | 48.5 µs | 50.9 µs | 52.0 µs | 65.1 µs | 102.1 µs | 54.9 µs | 17,973 QPS |
| 2.2 | select-hot | B | 40,000 | 47.3 µs | 50.9 µs | 51.9 µs | 62.8 µs | 73.9 µs | 54.2 µs | 18,210 QPS |

(Rep 1.1's `select-hot` p25 of A, 71.2 µs against B's 52.8 µs, is a bimodal arm on core 1, whose p50 is equal to 0.8 µs.)

**Reading: inside the spread.** On the matched reps the p50 delta is -0.8 to +0.3 µs on every arm, the p25 delta -1.4 to +0.1 µs (rep 1.1 `update-disjoint` -1.4 µs; the bimodal `select-hot` p25 of rep 1.1 excluded), and the run's noise floor
(`update-disjoint` against its repeat) is 0.2-1.1 µs at p50. Whole-server `cpu.on_cpu` per statement (A minus B, over 240,000 statements) is +1.08, +1.81, +0.44, -0.13, +0.27 and -1.32 µs across the six reps; four of six lie within ±1.1 µs and all six within ±1.9 µs, the sign changing, so no CPU delta is claimed. Wait breakdown: `relaxed`, one session: no commit wait and no one to wait for;
`sys.lock_waits` is empty in every M1 run, so a statement is the client round trip plus the server's CPU, resolved here to about ±1 µs of 40-52 µs.

**Delta against this engine's earlier closes** (`v3.4.8-16-g713a84d0` file, §4): its matched reps read `update-disjoint` p50 40.5-40.9 µs and QPS 22,424-22,951; here the matched core-{2,3} rep reads 40.4 / 40.7 µs and 22,958 / 22,736 QPS, the same level to 0.3 µs; the earlier file's finding (placement, not build) is reproduced, including that core 1 reads slow.

## 5. M3 - the scenarios

### 5.1 Scenario 2 (`--bookers 8`, `group`): +1.8 % on the closing build, outside the within-build spread

Each run ended early (the driver reports that the 24,000-cargo pool ran out before the 60 s allowance, as at the earlier closes); TPS is an average over that window. Order of runs: A, B, B, A.

| Run | Build | committed | TPS | booking p0 / p25 / p50 / p95 / p99 | commit p0 / p25 / p50 / p95 / p99 | `--verify` failures of 400 |
|---|---|---|---|---|---|---|
| a | A | 19,915 | 2,303.5 TPS | 398.4 / 1,267.3 / 2,549.9 / 3,326.0 / 4,567.9 µs | 371.7 / 736.7 / 881.1 / 1,322.0 / 1,836.6 µs | 115 |
| b | A | 19,885 | 2,300.0 TPS | 400.1 / 1,281.0 / 2,560.6 / 3,367.8 / 4,671.4 µs | 368.0 / 742.0 / 886.0 / 1,331.5 / 1,975.8 µs | 120 |
| a | B | 19,924 | 2,341.8 TPS | 390.8 / 1,299.0 / 2,545.4 / 3,275.7 / 4,320.4 µs | 382.4 / 731.6 / 875.8 / 1,305.6 / 1,732.5 µs | 122 |
| b | B | 19,900 | 2,345.5 TPS | 404.6 / 1,283.8 / 2,538.5 / 3,276.5 / 4,522.1 µs | 375.6 / 736.2 / 877.1 / 1,307.9 / 1,756.5 µs | 105 |

Mean TPS **A 2,301.8, B 2,343.7 (+1.8 %)**; the within-build spreads are 0.15 % (A) and 0.16 % (B). The two B runs are the two highest, in both positions of the A,B,B,A order, so it is not drift. Statement phases of run `a`:

| Phase | Build | ops | errors | p0 | p25 | p50 | p95 | p99 | rate |
|---|---|---|---|---|---|---|---|---|---|
| operation-update | A | 19,917 | 1 | 42.3 µs | 85.1 µs | 100.2 µs | 204.6 µs | 308.3 µs | 2,304/s |
| operation-update | B | 19,930 | 5 | 46.2 µs | 84.9 µs | 99.4 µs | 210.6 µs | 316.2 µs | 2,342/s |
| org-update | A | 19,916 | 1 | 40.6 µs | 74.6 µs | 88.3 µs | 216.2 µs | 308.4 µs | 2,304/s |
| org-update | B | 19,925 | 1 | 39.4 µs | 74.4 µs | 87.3 µs | 213.3 µs | 304.2 µs | 2,342/s |
| freight-insert | A | 19,917 | 0 | 45.2 µs | 99.3 µs | 122.0 µs | 256.2 µs | 401.2 µs | 2,304/s |
| freight-insert | B | 19,930 | 0 | 45.8 µs | 99.7 µs | 122.8 µs | 251.8 µs | 382.8 µs | 2,342/s |
| charge-insert | A | 112,016 | 0 | 35.7 µs | 67.6 µs | 83.4 µs | 189.4 µs | 299.2 µs | 12,957/s |
| charge-insert | B | 112,115 | 0 | 34.9 µs | 67.8 µs | 82.7 µs | 184.6 µs | 298.2 µs | 13,178/s |

**Wait breakdown (DB time, run by run).** A statement is a sum; for scenario 2 the groups are the server's CPU, the group commit's `fsync`, the run queue, WAL writes, and lock waits:

| Run | `cpu.on_cpu` | `commit.wal_flush` | `sched.run_queue` | `sysio.wal_write` | `sysio.checkpoint` | `app.lock_wait` | `sys.lock_waits` (tuple `U`, all granted; deadlock / `fault_net` / cancelled 0 / 0 / 0) |
|---|---|---|---|---|---|---|---|
| A a | 51.0 % | 45.6 % | 2.4 % | 0.66 % | 0.26 % | 0.023 % (16.9 ms) | 54 rows, max 10.1 ms |
| A b | 51.4 % | 45.3 % | 2.5 % | 0.47 % | 0.26 % | 0.005 % (3.9 ms) | 43 rows, max 0.4 ms |
| B a | 52.1 % | 45.0 % | 2.1 % | 0.36 % | 0.26 % | 0.054 % (38.8 ms) | 68 rows, max 33.0 ms |
| B b | 51.3 % | 45.8 % | 2.1 % | 0.59 % | 0.28 % | 0.032 % (22.8 ms) | 66 rows, max 14.3 ms |

Lock waits are a thousandth of DB time on both builds; the non-CPU half is the group commit's fsync. The +1.8 % shows up in the `charge-insert` and `booking` rates evenly (13,178 against 12,957 and 3,160 against 3,117 per second) and as a 0.3-0.4 point lower run-queue share (2.1 % against 2.4-2.5 %); **the run does not isolate which change, if any, carries it**, and a two-runs-per-build result over one day is the weakest claim in this file. Whether it is a gain
(a smaller checkpoint and admission visit budget letting commits through sooner is a plausible reading; the cell does not test it) or a host artefact is not decided.

### 5.2 Scenario 0 (`--fk --txn`, 8 traders, `group`): equal

| Run | Build | committed | TPS | txn p0 / p25 / p50 / p95 / p99 | trade-insert p0 / p25 / p50 / p95 / p99 | account-update p0 / p25 / p50 / p95 / p99 |
|---|---|---|---|---|---|---|
| a | A | 313,570 | 5,226.2 TPS | 575.3 / 1,250.4 / 1,433.8 / 1,907.9 / 2,305.9 µs | 38.4 / 85.2 / 106.8 / 200.1 / 320.4 µs | 37.7 / 74.8 / 89.9 / 170.9 / 321.1 µs |
| a | B | 314,160 | 5,235.7 TPS | 602.7 / 1,244.7 / 1,426.6 / 1,906.6 / 2,348.1 µs | 37.6 / 84.6 / 105.8 / 199.5 / 336.4 µs | 37.2 / 74.2 / 89.3 / 170.4 / 332.9 µs |
| b | B | 307,795 | 5,129.9 TPS | 628.1 / 1,270.1 / 1,457.4 / 1,958.5 / 2,437.2 µs | 38.0 / 86.0 / 108.2 / 203.7 / 342.7 µs | 36.9 / 75.1 / 90.7 / 174.1 / 341.1 µs |
| b | A | 310,440 | 5,174.0 TPS | 564.5 / 1,265.0 / 1,447.1 / 1,932.4 / 2,355.7 µs | 38.4 / 85.2 / 107.1 / 201.6 / 341.7 µs | 36.8 / 74.5 / 90.0 / 171.3 / 340.6 µs |

Mean **A 5,200.1, B 5,182.8 TPS (-0.3 %)**; within-build spreads 1.0 % (A) and 2.0 % (B): inside the spread, not a finding. Wait breakdown, runs `a`: `commit.wal_flush` 85.1 % (A) and 85.0 % (B) of DB time, `cpu.on_cpu` 12.9 % and 13.0 %, run queue 1.6 %,
WAL writes 0.3-0.4 %, checkpoint 0.06 %; `app.lock_wait` 0 and `sys.lock_waits` empty on every run (scenario 0 shares no parent between writers). Balance verify matched on all four runs; 0 rolled back, 0 torn.

**Delta against the earlier closes.** Scenario 2: `v3.4.8-16` A 2,284.6, B 2,297.8; CP close 2,312.8 (close build) - now A 2,301.8, B 2,343.7. Scenario 0: `v3.4.8-16` A 5,045, B 5,131 (one run each); CP close 5,199 / 5,222 (means of three) - now A 5,200.1, B 5,182.8. Both scenarios' A reads at the CP close's level;
B reads 1.3 % above the CP close's scenario 2 and equal on scenario 0. Across days the host is not controlled (the earlier files say so), so these are context.

## 6. M4 - eight sessions on one hot row: the same refusals, the same granted waits

`lock_contention_benchmark.py --sessions 8`, 4,000 ops per session per arm (32,000 per arm); both servers accepted on cores 0-3.

| Arm | Build | ops | errors | p0 | p25 | p50 | p95 | p99 | max | QPS |
|---|---|---|---|---|---|---|---|---|---|---|
| update-hot | A | 32,000 | 2,450 | 39.4 µs | 195.4 µs | 308.8 µs | 781.8 µs | 1,073.2 µs | 7,679.2 µs | 21,755 QPS |
| update-hot | B | 32,000 | 2,423 | 38.8 µs | 198.8 µs | 313.3 µs | 783.4 µs | 1,074.9 µs | 6,001.6 µs | 21,571 QPS |
| update-disjoint | A | 32,000 | 0 | 39.1 µs | 187.0 µs | 297.5 µs | 763.9 µs | 1,055.6 µs | 5,390.5 µs | 22,420 QPS |
| update-disjoint | B | 32,000 | 0 | 37.3 µs | 190.2 µs | 302.5 µs | 763.0 µs | 1,034.8 µs | 3,329.3 µs | 22,306 QPS |
| update-disjoint-again | A | 32,000 | 0 | 37.7 µs | 189.6 µs | 297.0 µs | 727.9 µs | 1,003.6 µs | 4,593.7 µs | 22,951 QPS |
| update-disjoint-again | B | 32,000 | 0 | 35.5 µs | 187.9 µs | 294.4 µs | 739.2 µs | 1,011.4 µs | 4,615.9 µs | 22,925 QPS |
| insert-omitted | A | 32,000 | 0 | 35.9 µs | 185.7 µs | 293.0 µs | 716.7 µs | 978.9 µs | 7,836.9 µs | 23,286 QPS |
| insert-omitted | B | 32,000 | 0 | 30.9 µs | 153.1 µs | 254.2 µs | 693.1 µs | 963.2 µs | 325,936.2 µs | 20,185 QPS |
| select-hot | A | 32,000 | 0 | 50.1 µs | 262.2 µs | 406.6 µs | 1,001.3 µs | 1,354.2 µs | 5,185.6 µs | 16,810 QPS |
| select-hot | B | 32,000 | 0 | 51.4 µs | 260.9 µs | 404.6 µs | 986.4 µs | 1,345.6 µs | 4,736.1 µs | 16,925 QPS |

Both builds refuse the hot-row arm the same way (2,450 and 2,423 of 32,000 `TXN_CONFLICT retryable=1 row id=1 was written by transaction N`; the first-updater-wins rule). `sys.lock_waits`: tuple `U`, **all granted** - A 12 rows, 221 µs total, max 73 µs; B 14 rows, 106 µs, max 31 µs; 0 deadlock, 0 `fault_net`, 0 cancelled.
`app.lock_wait` events 1,838 (A, 749 µs) and 1,716 (B, 551 µs); `cpu.on_cpu` 19.41 s and 19.40 s. Hot-minus-disjoint at p50 is +11.3 µs (A) and +10.8 µs (B) against floors (disjoint against its repeat) of 0.5 and 8.1 µs. The `insert-omitted` arm's 325.9 ms maximum on B, and the 253 versus 293 µs p50 and 20,185 versus 23,286 QPS beside it, is one stall in 32,000 statements; the same
arm showed a 269 ms maximum on the second label in the earlier close's M4 (`v3.4.8-16`, there on `713a84d0`), so it follows the second server's `insert-omitted` arm, not a build, and is not attributed. Wait breakdown: lock waits are 0.003-0.004 % of `cpu.on_cpu`; the arm's time is eight sessions queueing on four reactors and the round trip.
**Delta against the earlier closes:** `app.lock_wait` events were 2,298 (A) and 2,404 (B) at `v3.4.8-16` and 1,648 / 1,634 at the CP close; 1,838 / 1,716 here, so the count is in the same range across days and builds.

## 7. M5 - mount start-to-ready: equal to the baseline

`mount_cost_benchmark.py --mounts 7 --durability relaxed`, 3 runs per build and cell, order A B / B A / A B; wall times in ms from `Popen` to the `listening on` line (a one-shot duration; QPS does not apply). Only the loser's undo walk changed at mount in this milestone. p99 equals the maximum at n = 21; at n = 3 p25 equals p0.

| Cell | Build | Mounts | p0 | p25 | p50 | p95 | p99 | mean | `recovery_analysis_us` mean | `recovery_checkpoint_us` mean |
|---|---|---|---|---|---|---|---|---|---|---|
| empty log | A | 21 | 57.7 | 64.1 | 66.7 | 73.1 | 87.9 | 66.4 | 22.5 | 3.4 |
| empty log | B | 21 | 60.1 | 64.3 | 66.3 | 77.4 | 77.5 | 67.6 | 22.7 | 3.5 |
| 4,000 rows, clean stop | A | 21 | 58.2 | 60.1 | 62.2 | 68.9 | 71.3 | 62.5 | 16.8 | 3.4 |
| 4,000 rows, clean stop | B | 21 | 59.6 | 62.1 | 62.4 | 66.7 | 82.2 | 63.9 | 17.0 | 3.5 |
| 4,000 rows, SIGKILL, first mount (about 29k records) | A | 3 | 116.0 | 116.0 | 122.2 | 122.4 | 122.4 | 120.2 | 28.5 | 14.4 |
| 4,000 rows, SIGKILL, first mount | B | 3 | 111.1 | 111.1 | 118.1 | 119.9 | 119.9 | 116.4 | 28.0 | 13.0 |
| 4,000 rows, SIGKILL, mounts 2-7 | A | 18 | 58.3 | 60.2 | 61.2 | 66.6 | 66.6 | 62.0 | 16.7 | 3.4 |
| 4,000 rows, SIGKILL, mounts 2-7 | B | 18 | 58.1 | 60.1 | 61.0 | 62.4 | 62.4 | 60.9 | 16.3 | 3.3 |

B minus A: **+1.2 ms** (empty log), **+1.4 ms** (4,000 rows, clean), **-3.8 ms** (SIGKILL first mount, n = 3), **-1.1 ms** (SIGKILL mounts 2-7); p50 -0.4, +0.2, -4.1, -0.2 ms. Every difference is inside the baseline's own p0-to-max range (30.2, 13.1, 6.4 and 8.3 ms): **equal**. The recovery counters are equal. The undo walk's guard would show first in the first mount after the SIGKILL, which replays about 29k records and rolls back no loser (0 rolled back);
**a rollback of a long loser chain was not exercised**, so the guard's cost on a real loser is not measured. Wait breakdown: does not apply (one-shot start).
**Delta against the earlier mount files:** A here is the build that carries the scrub remedy, and its means (66.4 / 62.5 / 120.2 / 62.0 ms) are the remedy build's of `v3.4.8-20-g4cf04d4c` (67.9 / 64.4 / 120.7 / 62.5 ms) to within 1.9 ms: the host and the mount path reproduce.

## 8. M6 - an assertion on a relation: not resolved

`assertion_overhead_benchmark.py` (one session per server, `--preload` 200 / 1,000 / 10,000 groups, 8,000 inserts per arm, 16 blocks, `relaxed`) prices admission against a control relation on the same server. The driver was run once per preload with the servers started by the same launcher; `create_assertion_ms` was 0.3-0.4 (200), 1.3-1.4 (1,000), 11.8-12.5 (10,000) on both builds.
**The comparison is confounded by session placement** and I report it as unresolved: the per-server assertion-minus-control p50 of `insert-assert-existing` is A +10.7 / -5.7 / +11.1 µs and B +11.1 / +1.7 / +2.0 µs at 200 / 1,000 / 10,000 rows, where A's control arm at 1,000 rows (51.1 µs) is 11 µs above B's and A's p50 of the same insert on the control is 42.0 µs at 200 rows: the 10 µs swings are the size of M1's placement effect and are not attributable to the build (the accepted cores were not recorded for this cell). Equal where both look settled: at 200 rows `insert-assert-existing` p50 is 52.5 µs (B) and 52.7 µs (A), `insert-assert-newgroup` 53.6 and 52.9 µs, and no arm of any size had an error.
**The cell does not show that the snapshot path ran:** it forces no checkpoint, the run is a few seconds long, and I read no checkpoint or snapshot counter, so the 272 KiB chunk cut (G17) is **not measured**. A cell for it needs a long, checkpointed assertion build under a pinned session and was cut for the budget.

## 9. Insights

1. **A cycle guard per leaf is free at this resolution.** The walk's per-page work is a latch, a header read and a slot loop; a `Visit` is a pointer compare and a counter increment, and the census, which resolves 5 µs on a 300 µs scan (16 ns per page), found B no dearer on any path and cheaper on most. The cheaper figure should not be over-read: the milestone replaced a page counter with a check, which can drop a load and a compare per page, and the 0.4-2 % saving is the size of such a change, but
   this run cannot show *why* B is lower.
2. **The one-session driver's placement artefact is now the dominant uncertainty in this suite's A/B on single-session shapes**: three closes in a row have shown core 0 and core 1 reading 4-20 µs slow on whichever build landed there, and this file's assertion cell fell into it. The scan census (`cores = 1`, server pinned) and the scenarios do not have it, which is why they carry this file's verdicts. A driver change to pin the placement is the remedy; no driver was changed here.
3. **Scenario 2's +1.8 % is the one number here that reads as a gain with a spread under it**, two runs per build, 0.15 % within. It does not point at G19 (the scenario does not scan a whole relation) and may be G17's smaller budgets or the host; it is a lead, not a result, and the next close of scenario 2 is where to look.
4. **Unmeasured and worth a driver:** the whole-relation write walk, the index leaf walk, the loser's undo walk on a long chain, and the 272 KiB snapshot chunks. The first two are what G19 touched on the write side; no cell here times them.

## 10. Limits

| Not measured | Why |
|---|---|
| The dispatcher's whole-relation write walk and the index leaf walks | no driver with a server-CPU split for them in `tools/` |
| The 272 KiB assertion snapshot and checkpoint budgets (G17) | the one assertion cell does not force or read a checkpoint (§8) |
| Mount undo of a long loser chain (G16) | the mount cells roll back nothing |
| G18 (region creation reads one device page), G20 (`CREATE TABLE ... REFERENCES` trail) | no cell creates relations or free-map regions in a measured loop |
| PostgreSQL floor | no twin for these shapes (rule 4: not a hole) |

## 11. Reproduce

Archive `scripts/`: `master4.sh` (M2, M1, M1 swapped, M4, M3 in order), `master5.sh` (mount and assertion cells), `wrap-A2.sh` / `wrap-B2.sh`, the CP close's `run_m1m4.sh`, `run_m3.sh`, `m2_run_m2.sh`, `m2_summary.py`, `m2_sanity.py`, and the analysis scripts `an_m14.py`, `an_m3.py`, `gen_m1b.py`, `gen_mount5.py`.

```
# M2: bm_scan.py census --arm base=A --arm close=B --runs 5 --blocks 12 --scan-ops 768 --point-ops 3000 --cells rows-100k,wide-a --pin-server 2 --pin-driver 3
# M1 (3 reps; the swapped batch exchanges the two binaries): lock_contention_benchmark.py --port P --label base --ab-port Q --ab-label close --sessions 1 --rows 256 --ops 40000 --blocks 4
# M4: the same with --sessions 8 --ops 4000
# M3: scenario2_freight.py --suffix m3s2 --organizations 300 --ships 30 --operations 300 --cargos 24000 --verify 100 --seed 1 --sync --bookers 8 --seconds 60 --bookings 0
#     scenario0_stockmarket.py --suffix m3s0 --users 2000 --assets 2000 --traders 8 --seconds 60 --fk --txn --verify 200 --seed 1 --sync
# M5: mount_cost_benchmark.py --binary <wrapper> --label X --rows {0,4000} --mounts 7 --durability relaxed [--crash] --port 15481 --scratch <ext4 dir> --json X.json --force
# M6: assertion_overhead_benchmark.py --port P --label B2 --ab-port Q --ab-label A2 --preload {200,1000,10000} --ops 8000 --blocks 16
```

Row-count mapping: M2 `rows-100k` is 100,000 rows in 603 leaf pages and `wide-a` 10,000 wide rows in 304; M1/M4 `--rows` is the table size (256, one leaf or a few, not a scan shape); M5 `--rows` is the spilled-row count loaded before the mounts; M6 `--preload` is the row and group count present when the measured inserts start. `--force` on the mount driver overrides its load-average guard, which read the decaying average of the builds; the host sampler's log shows no competing process.
