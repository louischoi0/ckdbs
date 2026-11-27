# DW-S0 premise: a btree index's dead entries grow one per delete or key move with no steady state, and a point read pays for them only where its key run holds them

**Thesis.** On worktree `dw-btree-purge-driver` at `630ecc359` (`v3.4.17-178-g630ecc359`), four mixes ran for 480 s each over a btree relation with a plain secondary index on `k`, nothing purging. **The index's dead entries grow without bound at one per delete (or per key move) and the series shows no steady state; whether a point read by that index pays for them depends on whether the probed key value's run holds the stale entries, which is a property of the key's distribution and not of the churn.**

1. **Dead entries grow linearly in statements: 1.0000 per `DELETE` (D cells) and 0.9989-1.0000 per key-moving `UPDATE` (M cells)**, at a fitted 6,503/s (D-rec), 6,690/s (D-fresh), 11,725/s (M-rec) and 12,788/s (M-fresh) per index, at 34,800-39,600 statements/s over the three twins. After 480 s an index held 3.1-6.1 million entries for 50,000 live rows (63-122 x). The growth rate in the second half of every cell is 14-18 % below the first half's, and the series says nothing about a plateau.
2. **Recurring keys: the point read pays, by a factor of 32.5 (D-rec) and 4.9 (M-rec) in server CPU** (D-rec 470 us churned against 14.5 us purged; M-rec 39.9 us churned against 8.1 us rebuilt, controls +-1.8 % and +-0.2 us). Per stale entry in the key run: **739 ns (D-rec, 617 stale per run)** and **286 ns (M-rec, 111 stale per run)**. Both cells were already over a 5 % read cost at their first 30 s sample (D-rec +96 %, M-rec +24 %); the extrapolation from the per-entry cost puts the 5 % crossing at about one stale entry per key run: **4,896 deletes (0.75 s of the mix) in D-rec, 71,185 key moves (6.1 s) in M-rec**. These two times are derived, not observed.
3. **Fresh keys: the point read does not pay.** D-fresh churned / purged 1.002 (control 1.2 %); M-fresh 0.967 against the rebuilt twin and 1.045 against the purged one (7.53 us against 7.21 us, a 0.32 us difference; the rebuilt twin is slower than both). 3.2 and 6.1 million dead entries sat in those indexes and no probe read one: the probed key run holds one live entry and nothing else. **M-fresh sits at the 5 % edge against the purged reference and the data do not settle it** (section 4).
4. **`REINDEX INDEX` does not take a deleted row's entry out**: in both D cells it left the entry count at 3,134,200 and 3,215,400 (unchanged) and cost 1.97 s and 1.05 s; `PURGE INDEX` removed 3,073,929 and 3,152,155 of them in 3.28 s and 1.63 s. A "rebuilt" reference exists only for the M cells; the D cells' reference is the purged index. This follows `manual/sql/sql.md`'s statement of the two statements' difference.
5. **What an unasked driver would have removed, taken once at the end of each cell:** `PURGE INDEX` on `x_b`'s index removed 98.0-98.8 % of the entries (3.07-6.03 million) in 1.6-4.4 s of one core with `skipped_rows=0`, and left 60,271-75,336 entries for 50,000 live rows (the leaf-first entries `index.md` IX2c keeps, plus the tree's own rows).

DW-Q1's bar ("a point read by index pays under 5 % at steady state") is not read here; section 4 lists, per cell, the figures that bear on it.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-11-27 UTC. Mixes and read phases 07:12:37-07:21:53 (9 min 16 s, four servers concurrently, then four read phases serially); M-fresh recheck 07:22-07:23:35 (about 1.5 min). Total timed measurement about 10.8 min |
| Branch / worktree | `worktree-dw-btree-purge-driver`, worktree `dw-btree-purge-driver` |
| Commit measured | `630ecc359`, `git describe --tags` = `v3.4.17-178-g630ecc359`, committed 2026-11-26 20:12:53 -0500. **HEAD moved to `f6491ff81` (`v3.4.17-179-gf6491ff81`) during the session**; that commit changes `instructions/` only (four files), so the engine measured is `f6491ff81`'s too. Tree otherwise clean; `tools/dw_s0_premise.py` and `tools/dw_s0_report.py` are the only untracked files |
| Binary provenance | `build-release/kds_server`, `CMAKE_BUILD_TYPE=Release`, mtime 2026-11-27 02:07:04 -0500 (07:07:04 UTC), 87 s before `f6491ff81` and 5 h 54 min after `630ecc359`, so it is `630ecc359`'s source (the session's task statement says it was built at that commit; not rebuilt here). Measured from a copy `/home/ukjin/bench-runs/dw-s0/bin/kds_server`, sha256 `7d27de1c82964aa36c0d42215af5f033dabac6fa4647e5b30f6bc581b5d2322f` (the source binary's hash is the same) |
| Device | data files and logs under `/home/ukjin/bench-runs/dw-s0/run/<cell>/` on `/dev/mapper/fedora-root` (LVM on `/dev/nvme0n1p3`, Sandisk Optimus 5100 NVMe), xfs (`df -T`). `/tmp` is tmpfs on this host and held scripts only |
| Build type | Release |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 262144` (2 GiB; every cell's working set stayed resident: the largest cell directory, data file and log together, is 1.0 GiB), `log_level = warn`, cadence default, `reactor_cpus` unset. A fresh server and data file per cell; four servers at once during the mix on ports 15521-15524 (not 15432; the unrelated `kds_server` on this host holds 15470 and was left alone) |
| Host | Intel Core Ultra 7 270K Plus, 24 CPUs, P-cores 0-7, E-cores 8-23 (hybrid), `powersave` governor, Linux 7.3.4-300.fc45, 31 GiB. **Pinned**: each server with `taskset` to two P-cores (D-rec 0-1, D-fresh 2-3, M-rec 4-5, M-fresh 6-7), each cell's driver to one E-core (8, 9, 10, 11), the recheck driver to CPU 8 |
| Quiet gate | `pgrep -x cc1plus` and `pgrep -f "^(cmake --build\|ctest)"` both 0 before the run, in each cell's read phase (`host_before`, `host_after_timed`) and before the recheck. Load average 1.43 (1-minute) at the start, 5.9 at the first read phase (the four mixes' own tail), 2.8-3.5 at the later ones. The session's own Debug build (`cmake --build build`) was running at 07:07 UTC and had exited by 07:08:29 (its log's mtime), **a hold of about 1.5 min, outside the time budget**; the driver was written during it, and the first timed cell started at 07:12:37 after the gate read clear |
| Driver | `tools/dw_s0_premise.py`, tables by `tools/dw_s0_report.py`; imports `tools/kwp.py` and `tools/bn_census.py` (`sched_stat`) unmodified. The run used the driver as archived in `bench/v3.5.0/archive/dw-s0-premise-v3.4.17-178-g630ecc359/dw_s0_premise.as-run.py` (sha256 prefix `f8a61af4dc415245`); the tree's copy has the read phase's nested `block`/`timed` functions moved into a `Timer` class and the `recheck` mode added, with no change to what they do. **Server CPU** is the sum of `/proc/<pid>/task/*/schedstat` over the server's threads |
| Suite | Not executed: no engine code changed |
| Prior number | **None for this shape**: this run is the baseline the next one is read against. DD-S0 (`bench/v3.4.17/dd-s0-premise-v3.4.17-1-g054dbc5e.md`) priced a stale entry at about 0.76 us in a probe whose key run held them; D-rec's 0.74 us is the same order. PostgreSQL floor: not measured for these shapes |

## 2. Method

Per cell, one server holding three twins `x_a`, `x_b`, `x_c`, each `(id int64 PRIMARY KEY, k int64, w int64) BTREE` with `CREATE INDEX ix_<a|b|c> ON x_<a|b|c> (k)` (the primary key is the Keystone, so the index on `k` is the only secondary tree). The twins receive one history: each iteration issues the same statement to `x_a`, then `x_b`, then `x_c`. All statements are prepared (`kwp.Connection.prepare`), ids are named by the driver, and every reply is checked: **0 refusals in every mix and in every read phase** (counted by error code; the table of codes is empty).

| cell | `--n` mapping | iteration (per twin) | `k` | live rows | what recurs |
|---|---|---|---|---|---|
| D-rec | n = 50,000 live rows | `INSERT (id, k, 0)`, then `DELETE` the oldest live id | uniform in 1..n/10 = 5,000 | 50,000, 10 per value | every key value, every few iterations |
| D-fresh | n = 50,000 | as D-rec | a counter above 10^9, never reused | 50,000, 1 per value | no value |
| M-rec | n = 50,000 rows, never deleted | `UPDATE x SET k = ? WHERE id = ?`, a uniform random id | uniform in 1..n = 50,000 | 50,000, 1.0-1.6 per value | every value |
| M-fresh | n = 50,000, initial `k = id` | as M-rec | a counter above 10^9, never reused | 50,000, 1 per value | no value |

**The mix**: 480 s (the order says 10 minutes; cut for the budget). **Every 30 s** a sampler session reads `SELECT * FROM sys.indexes` (entries, height) and `SHOW META` (`pages_allocated`), the mix thread counts its own statements, and **300 prepared point reads on `x_c` over live keys** are timed serially (server CPU from schedstat, client wall) with no mix statement inside the window: the series of probe cost.

**The read phase**, one cell at a time with the other three servers idle (alive, nothing running): `ANALYZE SELECT * FROM x_<r> WHERE k = <a live key>` on each twin; `REINDEX INDEX ix_a`; a 500-key pool drawn uniformly from the live key values (every twin holds the same rows, so the keys are live in all three); one untimed warm block per relation; then **3 repetitions x 4 blocks of 1,000 prepared `SELECT * FROM x_<r> WHERE k = ?` per relation**, the three relations taken in a rotating order inside each block (12 blocks, 12,000 statements per relation), server CPU per statement from the schedstat delta over each block. Each statement's row count is compared with the driver's own count for that key: **0 wrong-row counts** in every phase. Then `PURGE INDEX ix_b`, its reply, wall time and the entry counts; one warm block per relation and the same 3 x 4 blocks again. **The control is the two twins that are churned in the phase against each other** (`x_b` against `x_c` after the `REINDEX`; `x_a` against `x_c` after the purge in a D cell, where `REINDEX` left `x_a` churned); its spread is the range of the per-repetition difference. An M cell has no control after the purge: `x_a` is rebuilt, `x_b` purged and only `x_c` churned.

Run: `tools/dw_s0_premise.py --bin <copy> --run-dir <dir on a block device> --base-port 15521 --mix-seconds 480 --purge` (defaults `--n 50000 --frames 262144 --stmts 1000 --key-pool 500 --reps 3 --blocks 4 --sample-s 30`), tables by `tools/dw_s0_report.py <dir>`. The recheck in section 4 is `tools/dw_s0_premise.py recheck --cell M-fresh --purge-rel c ...`.

## 3. Dead entries over the mix

**An index's dead entries equal its entries less the live rows, and they grow by one per delete or key move.** Every twin of a cell held the same entry count at every sample.

| cell | statements (3 twins), mean rate | deletes / moves per relation | entries at 480 s | dead entries | fitted growth (first half / second half) | dead per delete or move | dead per live row | stale per key value |
|---|---|---|---|---|---|---|---|---|
| D-rec | 18,510,642; 38,540 stmts/s | 3,084,200 deletes | 3,134,200 | 3,084,200 | 6,503 /s (7,094 / 5,930) | 1.0000 | 61.7 | 616.8 (5,000 values) |
| D-fresh | 18,997,842; 39,571 stmts/s | 3,165,400 deletes | 3,215,400 | 3,165,400 | 6,690 /s (7,198 / 6,156) | 1.0000 | 63.3 | 0 in any live key's run |
| M-rec | 16,691,442; 34,767 stmts/s | 5,562,000 moves (6,341 set a value it already held) | 5,605,659 | 5,555,659 | 11,725 /s (12,903 / 10,526) | 0.9989 | 111.1 | 111.1 (50,000 values) |
| M-fresh | 18,178,242; 37,848 stmts/s | 6,057,600 moves | 6,107,600 | 6,057,600 | 12,788 /s (13,800 / 11,762) | 1.0000 | 121.2 | 0 in any live key's run |

Statement rates are the driver's count (three twins, the sampler's and the probe's statements included, divided by the mix's wall time); they are throughput of the mix, not a limit: the driver is one serial session. The rate was 27,000-30,000 stmts/s in the first 30 s, 38,000-49,000 at 60-150 s and 28,000-40,000 after 270 s. It is not decomposed here: the data cannot separate the dead entries' effect on the writes from the tree's growth and the host's.

Selected samples (index `ix_a`; `pages_allocated` is the instance's counter over all three relations):

| t | D-rec dead | D-fresh dead | M-rec dead | M-fresh dead | D-rec `pages_allocated` | M-rec `pages_allocated` |
|---|---|---|---|---|---|---|
| 0 s | 0 | 0 | 0 | 0 | 1,365 | 1,332 |
| 30 s | 146,200 | 147,600 | 273,976 | 280,000 | 5,271 | 4,011 |
| 120 s | 805,000 | 811,800 | 1,476,256 | 1,558,000 | 23,088 | 13,731 |
| 240 s | 1,656,900 | 1,682,700 | 3,016,636 | 3,223,200 | 46,017 | 26,679 |
| 360 s | 2,399,500 | 2,452,900 | 4,347,669 | 4,692,700 | 68,083 | 43,893 |
| 480 s | 3,084,200 | 3,165,400 | 5,555,659 | 6,057,600 | 87,670 | 50,772 |

The full 17-sample series per cell is in `bench/v3.5.0/archive/dw-s0-premise-v3.4.17-178-g630ecc359/report.txt` and `series-<cell>.jsonl`. Index height was 3 for every churned index at the end (2 for a rebuilt one); the heap's data pages grow with the delete-marked rows in D cells (`sys.relation_stats` `leaves`: 18,881 in D-rec, 19,370 in D-fresh) and not in M cells (302 in both, the rows never leave their leaves).

**The probe cost series on `x_c`** (300 serial prepared reads, server CPU per statement; one sample each, noisy):

| t | D-rec | D-fresh | M-rec | M-fresh |
|---|---|---|---|---|
| 0 s | 14.2 us | 9.5 us | 10.2 us | 9.6 us |
| 30 s | 27.9 us | 8.5 us | 12.7 us | 8.8 us |
| 120 s | 105.7 us | 8.6 us | 21.4 us | 8.2 us |
| 240 s | 240.2 us | 10.3 us | 46.3 us | 11.3 us |
| 360 s | 358.9 us | 8.3 us | 35.5 us | 8.2 us |
| 450 s | 456.1 us | 8.5 us | 41.2 us | 8.1 us |
| 480 s | 664.6 us | 8.8 us | 62.4 us | 8.5 us |

D-rec's series is close to linear in time (fitted 1.12 us/s, about 0.86 us per stale entry on the cell's growth). **The 480 s samples (664.6 us in D-rec, 62.4 us in M-rec) are outliers against the read phase's 458 us and 39.9 us over the same state** a few seconds later; a single 300-statement sample catches a checkpoint or a scheduler hiccup (D-fresh's 240 s sample at 10.3 us and M-rec's at 46.3 us are others). The fresh cells' series is flat at 8.1-10.3 us for the whole mix.

## 4. The read cost at the end, per cell

All figures are server CPU per statement, the mean of the 12 blocks of a relation (3 repetitions x 4); the percentile and QPS forms are below. `ratio` is churned over reference; `delta` its difference. **The 5 % column asks whether the delta exceeds 5 % of the reference and also the control's spread.**

| cell | reference | reference CPU | churned CPU | ratio | delta | control (churned twins against each other) | over 5 % beyond the control |
|---|---|---|---|---|---|---|---|
| D-rec | `x_b` after `PURGE INDEX` (a `REINDEX` leaves the entries) | 14.47 us | `x_a` 471.22 us, `x_c` 469.08 us | **32.49** | +455.7 us | `x_a` - `x_c` -6.09 to +8.61 us (1.8 % of the churned cost) | **over** |
| D-rec | `x_a` after `REINDEX INDEX` (entries unchanged) | 458.45 us | `x_b` 459.27 us, `x_c` 457.97 us | 1.000 | +0.17 us | `x_b` - `x_c` -2.17 to +7.81 us | not applicable: the `REINDEX` is not a rebuild of the entries |
| D-fresh | `x_b` after `PURGE INDEX` | 7.47 us | `x_a` 7.48 us, `x_c` 7.49 us | **1.002** | +0.02 us | -0.07 to +0.02 us (1.2 %) | **under** |
| M-rec | `x_a` after `REINDEX INDEX` | 8.13 us | `x_b` 39.90 us, `x_c` 39.88 us | **4.907** | +31.76 us | `x_b` - `x_c` -0.23 to +0.16 us (0.5 % of the churned cost) | **over** |
| M-rec | `x_b` after `PURGE INDEX` | 7.48 us | `x_c` 39.94 us | 5.336 | +32.45 us | none (one churned twin) | **over** |
| M-fresh | `x_a` after `REINDEX INDEX` | 7.75 us | `x_b` 7.48 us, `x_c` 7.50 us | **0.967** | -0.26 us | -0.05 to +0.01 us (0.7 %) | **under** (the churned twins are cheaper) |
| M-fresh | `x_b` after `PURGE INDEX` | 7.21 us | `x_c` 7.53 us | **1.045** | +0.32 us | none (one churned twin) | **at the edge**: 4.5 % |

**M-fresh at the bar's edge.** Against the purged twin the churned one costs 4.4-4.5 % more (7.53 against 7.21 us by mean, 7.45 against 7.13 us by the median of 12 blocks); against the rebuilt twin it costs 3.3 % less, and the rebuilt twin (height 2, 50,000 entries) is the *slowest* of the three, 0.5 us (7 %) above the purged one (height 3, 75,336 entries). So the three twins differ from each other by more than the churned-to-purged delta and not in the churned direction. A recheck on the same data file (the server mounted again, `PURGE INDEX ix_c`, the three timed again, `recheck-M-fresh.json`) read: before the purge `x_a` 7.88, `x_b` 7.23, `x_c` 7.73 us (`x_c`'s repetitions 7.47, 8.25, 7.47: one noisy repetition); after the purge removed 6,032,264 entries from `ix_c` in 4.42 s, `x_a` 7.81, `x_b` 7.38, `x_c` 7.31 us. Churned over purged on `x_c` is 1.058 by the mean and 1.02 by the median repetition. **The data put M-fresh's churned-to-purged cost at 2-6 %, bracketing the bar, and cannot place it on either side; against the rebuilt twin (the order's reference) it is not a cost.**

D-fresh's first phase had one outlier block (`x_b`, block 5: 41.49 us against 7.4-8.0 elsewhere; its p99 147 us), which is why the table's `REINDEX` phase is not quoted for D-fresh: the medians of the 12 blocks are 7.48 (`x_a`), 7.46 (`x_b`), 7.46 (`x_c`) us, and the purge phase (clean, 1.2 % control) is the figure above.

**The stale entries per key run, and the cost of each** (the probed key's run; the recurring cells only):

| cell | stale entries per key value | live rows per probe | pages touched per probe (`ANALYZE`, a churned twin) | delta per probe | per stale entry (derived) |
|---|---|---|---|---|---|
| D-rec | 616.8 (3,084,200 / 5,000) | 10.1 | 615 | 455.7 us | **739 ns** |
| M-rec | 111.1 (5,555,659 / 50,000) | 1.64 | 114 (113 entries examined, 1 row) | 31.76 us | **286 ns** |
| D-fresh, M-fresh | 0 (a live key's run holds its live entry) | 1.0 | 2 | about 0 | not defined |

`ANALYZE` confirmed an `IndexProbe` of `x_<r>` on every relation of every cell. **The pages touched per probe equal the stale entries in the run**, to within the live rows (615 against 617 in D-rec; 114 against 112 in M-rec): each stale entry costs one row-page fetch. The per-entry price differs 2.6 x between the cells (739 against 286 ns) with the same mechanism; a delete-marked row's resolve (D-rec) is dearer than a row whose newer version carries another key (M-rec). The stale entries per key value are the cell's average, not a per-key measurement: the index exposes no per-key count, and an M-rec run's length varies with the key (live keys are size-biased).

**When the cost crosses 5 %, in the recurring cells (derived).** The reference is the cost with no stale entry (D-rec 14.47 us, M-rec 8.13 us); the crossing is the stale entries per run at which the per-entry cost times that count reaches 5 % of it, divided into the measured growth.

| cell | stale per key run for +5 % | = deletes or moves per relation | at the fitted growth | observed |
|---|---|---|---|---|
| D-rec | 0.98 (0.05 x 14.47 us / 0.739 us) | 4,896 deletes (and as many inserts) | **0.75 s** | first sample (30 s): +96 % over t = 0 |
| M-rec | 1.43 (0.05 x 8.13 us / 0.286 us) | 71,185 moves | **6.1 s** | first sample (30 s): +24 % over t = 0 |

The crossing precedes the first sample in both cells; **it is extrapolated from the end state's per-entry cost, linear in the stale entries per run, and not observed**: the series' per-entry cost at 30 s (D-rec 0.47 us, M-rec 0.46 us, each over its own t = 0 probe) is not the end state's, so the shape between 0 and 30 s is not known to be linear. What is observed: at the first 30 s sample, with 29 (D-rec) and 5.5 (M-rec) stale entries per key value, both cells were already past 5 %. Nothing was run between t = 0 and 30 s.

## 5. Latency and wait accounting

Distribution of the client-observed statement time, us, 12,000 statements per row (3 repetitions x 4 blocks x 1,000).

After `REINDEX INDEX ix_a` (the first phase):

| cell | relation | p0 | p25 | p50 | p95 | p99 | derived throughput |
|---|---|---|---|---|---|---|---|
| D-rec | `x_a` (not rebuilt) | 389.0 | 501.9 | 517.0 | 558.5 | 601.5 | 1,928 /s |
| D-rec | `x_b` | 387.1 | 504.4 | 518.6 | 560.7 | 608.3 | 1,921 /s |
| D-rec | `x_c` | 358.0 | 502.7 | 517.5 | 556.3 | 583.4 | 1,932 /s |
| D-fresh | `x_a` | 23.6 | 25.5 | 25.8 | 26.6 | 27.4 | 38,760 /s |
| D-fresh | `x_b` | 23.8 | 25.5 | 25.8 | 26.9 | 147.2 | 35,336 /s (one outlier block) |
| D-fresh | `x_c` | 23.7 | 25.5 | 25.8 | 26.6 | 27.4 | 38,610 /s |
| M-rec | `x_a` rebuilt | 24.1 | 26.3 | 26.9 | 29.1 | 30.8 | 36,765 /s |
| M-rec | `x_b` | 50.4 | 56.8 | 58.8 | 65.0 | 68.7 | 16,807 /s |
| M-rec | `x_c` | 50.3 | 56.8 | 58.9 | 65.1 | 69.0 | 16,835 /s |
| M-fresh | `x_a` rebuilt | 23.8 | 25.5 | 26.0 | 27.4 | 28.5 | 38,168 /s |
| M-fresh | `x_b` | 23.9 | 25.2 | 25.8 | 27.0 | 28.0 | 38,610 /s |
| M-fresh | `x_c` | 23.9 | 25.2 | 25.8 | 27.0 | 28.0 | 38,610 /s |

After `PURGE INDEX ix_b` (the second phase):

| cell | relation | p0 | p25 | p50 | p95 | p99 | derived throughput |
|---|---|---|---|---|---|---|---|
| D-rec | `x_a` | 402.3 | 518.0 | 532.2 | 570.6 | 610.4 | 1,873 /s |
| D-rec | `x_b` purged | 27.4 | 33.8 | 35.9 | 74.1 | 87.5 | 25,189 /s |
| D-rec | `x_c` | 363.7 | 509.1 | 524.5 | 566.3 | 641.8 | 1,900 /s |
| D-fresh | `x_a` | 23.8 | 25.4 | 25.7 | 26.5 | 27.2 | 38,760 /s |
| D-fresh | `x_b` purged | 23.4 | 25.4 | 25.7 | 26.5 | 27.2 | 38,760 /s |
| D-fresh | `x_c` | 23.5 | 25.4 | 25.7 | 26.4 | 27.1 | 38,760 /s |
| M-rec | `x_a` rebuilt | 24.0 | 26.2 | 26.8 | 28.9 | 30.4 | 36,900 /s |
| M-rec | `x_b` purged | 23.7 | 25.5 | 26.1 | 28.2 | 29.8 | 37,879 /s |
| M-rec | `x_c` | 49.6 | 56.7 | 58.8 | 65.2 | 70.3 | 16,807 /s |
| M-fresh | `x_a` rebuilt | 23.6 | 25.5 | 26.0 | 27.4 | 28.3 | 38,023 /s |
| M-fresh | `x_b` purged | 23.8 | 24.9 | 25.7 | 26.9 | 27.9 | 38,911 /s |
| M-fresh | `x_c` | 23.9 | 25.3 | 25.9 | 27.1 | 28.4 | 38,462 /s |

(Throughput is derived, `1,000,000 / mean us` of the wall time; the mean is in the archived JSON.) **The p0 and p25 columns carry the finding**: in D-rec and M-rec the whole distribution moves, floor included (D-rec p0 358-402 us against 27.4 us; M-rec 50 against 24 us), so the stale entries are a fixed per-statement cost for a key whose run holds them, not a tail. In the fresh cells the three twins agree to 0.1-0.6 us at every percentile but p99 on the outlier block.

**Where a statement's time went** (mean over the 12 blocks; the reference and churned relation of each phase):

| cell, relation | wall | server CPU | run-queue wait | client, socket and reply decode (remainder) |
|---|---|---|---|---|
| D-rec `x_b` purged | 39.7 us | 14.5 us | 0.0 us | 25.2 us |
| D-rec `x_a` churned | 534.0 us | 471.2 us | 0.0 us | 62.8 us (10.1 rows returned) |
| M-rec `x_a` rebuilt | 27.1 us | 8.1 us | 0.0 us | 19.0 us |
| M-rec `x_c` churned | 59.5 us | 39.9 us | 0.0 us | 19.6 us |
| D-fresh `x_a` | 25.8 us | 7.5 us | 0.0 us | 18.3 us |
| M-fresh `x_c` | 26.0 us | 7.5 us | 0.0 us | 18.5 us |

**Server CPU is the whole of the stale entries' cost**: run-queue wait was 0.0-0.1 us a statement in every cell, the remainder (client, loopback round trip, reply decode) is 18-25 us and does not move with the churn (it grows with rows returned: 62.8 us at 10.1 rows). There is no durability wait (these are reads), no lock or conflict wait (one session on an otherwise idle server) and no write-statement wait in the timed phase; a split of the mix's writes into waits is not measured.

## 6. What an unasked driver would have removed

`PURGE INDEX` was run once per cell on `ix_b`, with the other servers idle, nothing else on the relation. Its reply, wall time and the entry count after:

| cell | reply | wall | entries removed | entries after (live rows 50,000) | wall per removed entry | removal rate | pages_allocated delta |
|---|---|---|---|---|---|---|---|
| D-rec | `entries=3073929 skipped_rows=0 attempts=642` | 3.28 s | 3,073,929 | 60,271 | 1.07 us | 0.94 M entries/s | -2,753 |
| D-fresh | `entries=3152155 skipped_rows=0 attempts=841` | 1.63 s | 3,152,155 | 63,245 | 0.52 us | 1.93 M entries/s | 0 |
| M-rec | `entries=5539114 skipped_rows=0 attempts=1035` | 3.98 s | 5,539,114 | 66,545 | 0.72 us | 1.39 M entries/s | 0 |
| M-fresh | `entries=6032264 skipped_rows=0 attempts=1598` | 4.40 s | 6,032,264 | 75,336 | 0.73 us | 1.37 M entries/s | 0 |

(M-fresh's recheck purge of `ix_c` removed the same 6,032,264 entries in 4.42 s.) At the fitted growth, the purge's wall time per dead entry times the creation rate is **0.7 % of a core in D-rec (1.07 us x 6,503/s), 0.8 % in M-rec (0.72 us x 11,725/s)**, derived arithmetic on one end-state purge of an idle instance: a purge run during a mix is not measured, nor what it costs the foreground. The tree keeps its size: `PURGE INDEX` does not shrink the height (3 in every purged index) and the surviving entries are 10,271-25,336 above the live rows. `REINDEX INDEX` on the M cells' `x_a` took 25.0 and 23.9 ms (50,000 entries, height 3 to 2) and on the D cells' 1.97 s and 1.05 s (3.1-3.2 million entries kept, 10,304 and 13,523 pages allocated); `REINDEX` waits for writers and holds new ones off (`manual/sql/sql.md`), which an unasked driver would not.

## 7. What the run teaches

- **The cost is a function of the key's run, not of the index's size.** The two fresh cells' indexes held 3.2 and 6.1 million dead entries and a probe paid nothing, while D-rec's 616 stale entries per key value multiplied a point read by 32.5. A rule that triggers a purge on index size or on total dead entries cannot tell these apart; the signal that separates them is whether the indexed value recurs, which no counter in `sys.relation_stats` or `sys.indexes` carries (DW-Q2). `sys.relation_stats` `deletes` and `updates` were exact (D-rec 3,084,200 and 5,562,000 for M-rec, equal to the driver's counts), but `est_delete_marked` and `est_rows` were NULL on all twins of all four cells after the mix: the steward had not sampled them. Whether `updates` counts an `UPDATE` that does not move an indexed column was not tested here (every M-cell update moved `k`).
- **Recurring keys make the cost arrive almost at once**: the crossing is at about one stale entry per key run (0.75 s and 6.1 s of mix, derived), so a driver for such an index has no grace period measured in minutes, and the growth is steady (6.5-12.8 thousand a second per index) with no plateau.
- **`REINDEX` is not the remedy for a delete-heavy btree**: it kept every deleted row's entry (3,134,200 before and after) and took 1.97 s. Only `PURGE INDEX` takes them, so the D cells' only reference is a purged index and its cost, 14.5 us, is the point read with nothing stale in it.
- **An entry's price differs by what it names**: 739 ns where it names a delete-marked row (D-rec), 286 ns where it names a live row that moved (M-rec). The same pages-per-probe = stale-per-run identity holds in both.
- **Purged and rebuilt trees are not ranked by size**: M-fresh's rebuilt tree (50,000 entries, height 2) was 0.5 us slower than the purged one (75,336 entries, height 3) and than the churned one. The data do not explain it (layout, leaf fill and placement are candidates; not profiled), and it is the reason M-fresh cannot be placed against 5 %.
- **A DW expectation this contradicts or supports:** the order's section 1 expectation that a moved key's old entry is purged only beside a retired slot (`known-gaps.md`, DR-Q18) is borne out by the M cells' growth: 5.6 and 6.1 million dead entries from 5.6 and 6.1 million key moves, one per move, with no row deleted and nothing reclaimed.

## 8. What was cut and what was not measured

- **Mix length 480 s, not the order's 10 minutes** (the budget). One run per cell, no repeat of a mix; the noise floor is the control pair inside the read phase and the recheck.
- **One row count: n = 50,000 live rows per relation. Rule 9's sweep (200, 1,000, 10,000) is not run.** The dead entries per delete or move are a count and do not depend on n; the cost of a probe depends on the stale entries per key run, i.e. on n / domain and the mix length, which this run fixes (D-rec domain 5,000, M-rec 50,000). A point read by a unique key does not scale with the rows, and these shapes scale with the key run only; the per-entry cost at other n is not measured.
- **`cores = 1`, `durability = relaxed` only**: no `strict`, no `cores > 1`, no `group`. No PostgreSQL floor.
- **No concurrent reader during the mix and no long-held view**: DW-R5's horizon case (an old view that stops the purge) is not exercised; the end-state `PURGE INDEX` ran with no other transaction open and skipped 0 rows. No competing foreground load during the purge.
- **The four mixes ran concurrently on four servers** (each pinned to two P-cores, each driver to one E-core), so shared memory bandwidth and the last-level cache were shared; the growth is a count and not sensitive to it, the statement rates are. The read phases ran serially with the other three servers idle, not stopped.
- **The probe-cost series is one 300-statement sample every 30 s** on `x_c` only, with the noise stated in section 3; no probe was taken between t = 0 and 30 s, so the 5 % crossing times are not observed.
- **The cost of the dead entries to the mix's own writes** (insert, delete and update statement cost over time) is not isolated: no run with a purge as comparison, so the rate fall in the second half is not attributable.
- **The census of the heap release's scheduling** (the second item in DW-S0's order) was not part of this task and was not run.
- **Index size in pages** is not exposed per index; `pages_allocated` is instance-wide over three relations and is reported as such.
- **Covered or spilled-column churn, unique indexes, the primary-key index of a non-Keystone key, and a heap relation** are outside the cells.
- **Peak process memory** was not recorded; the buffer pool (2 GiB of frames) held every working set, and the data files at the end were 464 MiB-1.0 GiB.
- The raw output (series, mix and read JSON, the recheck, worker logs, configs, the driver as run, the report) is archived under `bench/v3.5.0/archive/dw-s0-premise-v3.4.17-178-g630ecc359/`; no data file or WAL segment is archived.
