# DD close: `PURGE INDEX` takes a churned index back to a rebuilt one's cost, changes no write path, and stalls its reactor for the whole statement

**Thesis.** On `worktree-dd-index-entry-purge`, with the engine at `b8880f9b` (`v3.4.17-3-gb8880f9b`, which adds `PURGE INDEX <name>`) against the commit DD opened from, `054dbc5e` (`v3.4.17-1-g054dbc5e`, no purge), three things were priced on a quiet host at `cores = 1`, `relaxed`, `build-release`.

1. **DD changes no write path or probe measurably.** Eighteen cells (six shapes at 200, 1,000 and 10,000 rows) are interleaved against an in-run control arm: every cell's median paired difference is inside +/- 1.5 % and inside the control's own range of medians (-0.32 to +0.49 us on a 21-46 us statement). **Cell 1's bar, "no cell slower outside its spread", is met** (section 2).
2. **The purged twin costs what the rebuilt twin costs.** S0's two verdict cells (a low-cardinality probe and a unique insert over a churned run) cost +75 % and +61-66 % server CPU on a churned index; **after `PURGE INDEX` the difference to a `REINDEX`ed twin is +0.2 % / +0.8 % (cell 1) and -0.3 / -0.1 us (cell 5)**, inside the control pair's spread. The point probe (cell 2) never moved, and still does not (section 3).
3. **The purge is not cheap and does not yield the reactor.** At n = 100,000 and surplus equal to the live set it costs **2.4 us of server CPU per entry removed (242 ms, 14 attempts)**, 3.4x what a `REINDEX` of the same relation costs (71 ms), and **every writer on the purge's reactor waits the whole statement: 243 ms (w-writers) and 283 ms (v-writers) at `cores = 1`, against 71 ms for the `REINDEX`**. Where the surplus belongs to rows that were `PURGE`d (no slot left) an entry costs **20.8 us**, an attempt runs **0.30-0.33 s**, and the statement takes **2.07 s** at n = 100,000. The 64-leaf attempt bounds the relation `IX` the order's DD-R10 names (a third of the 1 s fault net on a quiet host); **it does not bound the time the statement holds its reactor** (section 5).

| Item | Value |
|---|---|
| Executed | 2026-11-20 UTC. Build of B 13:54-13:56; measurements 13:56:53-14:07:32 (about 11 minutes of cells); report 14:10 |
| Branch / worktree | `worktree-dd-index-entry-purge` (session worktree `dd-index-entry-purge`); the binaries came from detached checkouts, B's in the session scratchpad (`git worktree add --detach`, removed after the run) |
| Commit measured | **B** = `b8880f9b`, `git describe --tags` = `v3.4.17-3-gb8880f9b`, committed 2026-11-20 13:53:44 UTC, tree clean (detached). **A** = `054dbc5e`, `v3.4.17-1-g054dbc5e`, committed 08:00:46 UTC, tree clean (detached) |
| Binary provenance | Both `CMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`. **B**: built here, source binary mtime 13:56:08 UTC (after its commit), copy `bin/kds_B` sha256 `b3d705f6df8a2ca8ae47289261b1a003e10aeb8266deb95dc7cb13a1f0abe7f0`. **A**: the binary DD-S0 built from the same commit with the same flags (source mtime 13:13:26 UTC, after its commit), **reused, not rebuilt**; copy `bin/kds_A` sha256 `3df8801d61b6cbd8f9d9c6b16693c88b6de429069865bc8f527f2f5350587cbb`, the hash `dd-s0-premise-v3.4.17-1-g054dbc5e.md` records. Every server in this document started from a copy under `/home/louis/bench-runs/dd-close/bin/` |
| Device | data files and WAL under `/home/louis/bench-runs/dd-close/wd/` on `/dev/nvme0n1p1`, ext4 (`df -T`, read this session); the binaries were copied beside them |
| Server config | `cores = 1` except the two marked `cores = 2` runs of section 5, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, cadence default; **a fresh server and data file per configuration** (per run in cell 1, per cell elsewhere); server pinned to CPU 2 (`2,3` at `cores = 2`), driver to CPU 4 |
| Host | 8 vCPU, Linux 6.12.116. **Quiet for the whole of the A/B and the `cores = 1` cells**: 1-minute load 1.1-2.2 (4.0 at the first size of the first A/B pass, 2.9 at the second), `competing` (`pgrep` of `cc1plus`, `cmake --build`, `ctest`) 0-1 per block. One host interval was **not** quiet: the last `cores = 2` run (`p2b`, 14:07:11-14:07:32) saw load 3.3-5.4 and 6 competing processes; it is reported flagged. A different session's build had held the host (load 10-16) until 13:56; nothing was measured in that interval, and the hold is outside the budget |
| Driver | scratch `dd_ab.py` (sha256 prefix `ed93458f6324`) and `dd_close.py` (`886afd3616c7`), not in the tree; both import `tools/kwp.py`, `tools/bn_census.py`, `tools/sys_meta.py` and `tools/bb_overhead_benchmark.py` unmodified. Server CPU is the sum of `/proc/<pid>/task/*/schedstat` over the server's threads (`bn_census.sched_stat`). Raw JSON and logs: `/home/louis/bench-runs/dd-close/` (not archived under `bench/`: a narrower measurement, `bench/README.md` rule 1b) |
| Suite | **Not executed by this measurement**; no engine code changed. The session's own log (`dd-suite.log`, written 13:53 UTC, a minute before B's commit) reads `100% tests passed, 0 tests failed out of 4961`; its provenance beyond that timestamp is not established here |
| Baseline | The engine's own: **A** (same shapes, this run), and DD-S0's file for the verdict cells. No earlier results file measures these write-path shapes against an indexed table with a covering index, so this run is also the baseline the next is read against. PostgreSQL floor: not measured for these shapes |

## 1. Method

**Arms.** Cell 1 runs three servers at once per run: **A**, **A'** (the same A binary, a second server: the in-run control that cannot affect the result) and **B**. Each run builds three fresh servers, loads them identically, then for every shape takes six blocks per arm with the arm order rotated each block, so each arm is "busy" one at a time and every arm leads equally often. A "run" is one such three-server set; the unit of analysis is the **paired per-run difference** B-A (and A'-A for the control) in server CPU per statement.

**Tables.** `t (id, v, w, z)` BTREE with `CREATE INDEX ix_t_v ON t(v) COVERING (w)` loaded with 200, 1,000 or 10,000 rows (`v = w = z = id`); an empty `i` of the same shape and index for the insert. The covering index is deliberate: it is the code path DD touched (`AppendIndexEntry`'s covered copy).

| shape | statement (prepared) | what it exercises | `ANALYZE` plan (checked at 1,000 rows) |
|---|---|---|---|
| probe-point | `SELECT * FROM t WHERE v = ?` | point probe through the index | `IndexProbe` |
| probe-range | `SELECT * FROM t ORDER BY v LIMIT 20` | ordered index walk of 20 entries (**there is no index range for a `WHERE v >= a AND v < b`: `ANALYZE` shows a `Scan`; CV's ordered walk is the engine's range read through an index**) | `IndexRange order=index_asc`, `examined=20` |
| insert | `INSERT INTO i VALUES (?, ?, ?, ?)` | single-row insert, one index entry with a covered copy | n/a |
| update-other | `UPDATE t SET z = ? WHERE id = ?` | row write, index untouched | n/a |
| update-covered | `UPDATE t SET w = ? WHERE id = ?` | covered column: new entry with a new covered copy | n/a |
| update-idx | `UPDATE t SET v = ? WHERE id = ?` | indexed column moves | n/a |

Run counts: pass 1 8 runs x 1,200 statements per arm per shape; **pass 2 (reported in section 2) 12 runs x 3,000 = 36,000 statements per arm per shape per size**, 10 blocks per run. Pass 1 is the replicate and is summarised in a sentence. Row-count mapping: the `--sizes` flag of `dd_ab.py` is the number of rows loaded into `t` (200, 1,000, 10,000); the insert shape's `i` starts empty at every size.

**Cells 2-3 (`dd_close.py`).** One fresh server per cell holding **four** relations of one history, `x_a`, `x_p`, `x_b`, `x_c` (`id, v, w`, BTREE, indexes `ix_a`, `ix_p`, `ix_b`, `ix_c`): one churn statement per relation makes the surplus equal to the live set (2n entries per index); then **`REINDEX INDEX ix_a`**, **`PURGE INDEX ix_p`**, and `x_b`, `x_c` untouched (**the control pair is `x_b` against `x_c`**; this run carries its own, where DD-S0's earlier cell lacked a purged arm). Twelve blocks per relation (3 repetitions x 4 blocks), the four relations rotated within each block; server CPU is the schedstat delta over a block divided by its statements. Entries and height come from `sys.indexes` read before and after. Every reply tag was checked and every probe returned its row count (`bad` = 0 in every cell). Cell 3's relations are separate per measurement (each purge and each `REINDEX` meets a freshly churned relation).

| cell | n | history | measured statement |
|---|---|---|---|
| S0-1 | 100,000 | `v = id mod 4`; `UPDATE SET v = 3 - v` | `SELECT * FROM x WHERE v = 0` (25,000 rows), 6 per block |
| S0-5 | 10,000 | `UNIQUE` on `v`; `DELETE` all, row `PURGE` all, `INSERT` the same `v` under new ids | `INSERT` of a row whose `v` was just deleted, 150 per block |
| S0-2 | 100,000 | `v = id`; `UPDATE SET v = v + 10000000` | prepared `SELECT * WHERE v = ?`, 1 row, 1,000 per block |
| 3 | 100,000 | cell 2's history, one relation per measurement | `PURGE INDEX`, alone and across two writer connections |

Cells S0-1, S0-5 and S0-2 were run twice each (two independent servers, 13:58-13:59 and 14:01); both are shown.

## 2. Cell 1: the write path and the probes, A against B

**Every one of the eighteen cells' median paired B-A difference is between -0.8 % and +1.5 % of the statement's server CPU, and each lies inside the range spanned by the control arm's medians.** Throughput is derived from the client's mean wall time per statement (`QPS = 1,000,000 / mean us`, the loopback and driver included); the comparison the bar reads is server CPU, a cost, in the paired columns. "Control" is A' minus A in the same run; "range" is the minimum to maximum of the 12 per-run paired differences.

| rows in `t` | shape | A CPU us | B CPU us | B-A median us (%) | control A'-A median (range) us | B-A range us | QPS A / B |
|---|---|---|---|---|---|---|---|
| 200 | probe-point | 24.11 | 23.93 | +0.04 (+0.2 %) | +0.15 (-0.70, +0.58) | -0.59, +0.58 | 13,693 / 13,715 |
| 200 | probe-range | 45.36 | 44.97 | +0.05 (+0.1 %) | +0.10 (-1.69, +2.13) | -1.45, +4.04 | 8,273 / 8,272 |
| 200 | insert | 21.60 | 21.58 | +0.05 (+0.2 %) | -0.21 (-1.28, +1.06) | -0.57, +0.54 | 19,746 / 19,823 |
| 200 | update-other | 21.69 | 21.57 | +0.06 (+0.3 %) | -0.00 (-0.67, +1.07) | -1.26, +0.90 | 20,163 / 19,599 |
| 200 | update-covered | 23.51 | 23.44 | +0.04 (+0.2 %) | +0.02 (-0.17, +1.35) | -0.64, +0.75 | 18,904 / 18,860 |
| 200 | update-idx | 23.32 | 23.54 | +0.25 (+1.1 %) | +0.18 (-0.48, +1.00) | -0.32, +1.04 | 17,007 / 17,325 |
| 1,000 | probe-point | 24.13 | 24.44 | +0.36 (+1.5 %) | -0.29 (-0.56, +0.91) | -0.42, +1.00 | 13,499 / 13,476 |
| 1,000 | probe-range | 42.69 | 43.05 | -0.06 (-0.1 %) | -0.32 (-1.14, +1.30) | -0.82, +1.02 | 9,077 / 9,066 |
| 1,000 | insert | 21.69 | 21.83 | +0.14 (+0.6 %) | -0.06 (-1.22, +0.48) | -1.00, +0.66 | 18,695 / 18,715 |
| 1,000 | update-other | 21.81 | 21.59 | -0.15 (-0.7 %) | -0.19 (-0.53, +0.51) | -0.59, +0.34 | 17,475 / 17,792 |
| 1,000 | update-covered | 24.46 | 23.64 | -0.19 (-0.8 %) | -0.20 (-1.35, +0.88) | -1.35, +0.55 | 16,337 / 16,387 |
| 1,000 | update-idx | 23.18 | 23.64 | +0.23 (+1.0 %) | +0.16 (-0.56, +0.79) | -0.90, +1.32 | 17,469 / 17,435 |
| 10,000 | probe-point | 24.41 | 24.47 | +0.21 (+0.9 %) | -0.22 (-0.90, +1.34) | -0.77, +1.33 | 13,166 / 13,286 |
| 10,000 | probe-range | 42.88 | 43.32 | +0.27 (+0.6 %) | +0.49 (-1.20, +1.02) | -1.04, +1.53 | 9,323 / 9,382 |
| 10,000 | insert | 21.59 | 21.59 | -0.11 (-0.5 %) | -0.28 (-1.10, +0.27) | -0.91, +1.23 | 20,459 / 20,571 |
| 10,000 | update-other | 22.66 | 22.50 | -0.08 (-0.3 %) | +0.18 (-1.38, +1.13) | -1.28, +0.58 | 16,276 / 16,256 |
| 10,000 | update-covered | 24.41 | 24.08 | -0.17 (-0.7 %) | +0.03 (-0.96, +0.75) | -0.86, +0.38 | 16,570 / 16,743 |
| 10,000 | update-idx | 23.45 | 24.19 | +0.32 (+1.4 %) | +0.16 (-0.54, +1.29) | -0.54, +2.20 | 17,105 / 17,015 |

Three readings, none a finding.

- **The paired medians are noise-shaped.** Across the fifteen cells that are not update-idx they are positive and negative in near-equal number (-0.8 % to +1.5 %), and the control's medians (-0.32 to +0.49 us) are of the same size. The covered-copy path DD touched (insert, update-covered) moves by -0.8 % to +0.6 %.
- **update-idx is the one row that leans the same way in every measurement**: B-A is +0.25, +0.23, +0.32 us here and +0.01, +0.22, +0.31 us in pass 1 (+1.0 to +1.4 %). **The control leans with it**: A'-A is +0.18, +0.16, +0.16 us in the same three cells of this pass. B minus the control is +0.07, +0.07, +0.16 us (0.3-0.7 %), which is the floor, not a delta. An index-key update is the row an arm-order effect or a small real cost would show in; this run cannot separate them and says so.
- **The per-run extremes are wider for B than for A' in some cells** (200 probe-range +4.04 against +2.13 us; 10,000 update-idx +2.20 against +1.29 us; 10,000 insert +1.23 against +0.27 us). These are single runs out of 12 and the medians do not follow them; they are the largest excursions in the table and are named here so a reader does not find them unannounced. Pass 1's B-A medians (8 runs, 1,200 statements) span -0.8 % to +2.3 % (the +2.3 %, 200 rows probe-range, +1.00 us, is +0.05 us in this pass).

**The bar is met**: no cell's median is outside the control's spread, and none is outside +/- 1.5 %. It is read on a quiet host, at `cores = 1` and `relaxed` only (section 7).

Client-observed latency per statement at 1,000 rows (a distribution, not a matrix), microseconds; each figure is the median over the 12 runs of that run's percentile, 3,000 statements per run, **p90 in the p95 column** (the driver's `bn_census.summarize` emits p90, not p95):

| shape / arm | ops | p0 | p25 | p50 | p90 | p99 |
|---|---|---|---|---|---|---|
| probe-point A | 36,000 | 49.7 | 52.3 | 55.2 | 80.4 | 120.3 |
| probe-point B | 36,000 | 49.4 | 52.5 | 55.4 | 80.6 | 133.9 |
| probe-range A | 36,000 | 82.1 | 85.9 | 88.6 | 147.9 | 188.3 |
| probe-range B | 36,000 | 81.7 | 85.7 | 88.4 | 148.5 | 185.2 |
| insert A | 36,000 | 37.8 | 39.9 | 41.0 | 56.0 | 104.0 |
| insert B | 36,000 | 38.2 | 40.0 | 41.0 | 54.8 | 105.5 |
| update-other A | 36,000 | 35.6 | 39.7 | 42.6 | 70.8 | 106.3 |
| update-other B | 36,000 | 33.4 | 39.4 | 42.4 | 70.1 | 105.4 |
| update-covered A | 36,000 | 38.8 | 47.5 | 55.1 | 79.2 | 106.8 |
| update-covered B | 36,000 | 38.7 | 45.5 | 52.1 | 79.2 | 109.1 |
| update-idx A | 36,000 | 38.7 | 41.1 | 43.7 | 69.6 | 104.2 |
| update-idx B | 36,000 | 38.7 | 40.9 | 43.4 | 68.0 | 104.1 |

The floors (p0) and bodies (p25) match to within 0.5 us except update-other's p0 (35.6 against 33.4 us, a single fast statement). The 200-row and 10,000-row distributions match to the same degree and are not tabulated.

**Wait accounting.** Single session, `relaxed`: there is no durability wait (no `strict` or `group` rung measured), no lock or conflict wait, and no checkpoint wait in the window. Server on-CPU is 33-41 % of the client-observed mean time per statement (probe-point 24.1 of about 74 us; probe-range 42.7 of about 110 us; insert 21.7 of about 53 us; update-idx 23.2 of about 57 us); the rest is the client, its socket round trip and the server's request parsing between statements, which the driver cannot separate, and server run-queue wait of 0.1-5 us. No further decomposition of the in-engine time (write statement against read) is measured by this run.

## 3. Cell 2: S0's verdict cells again, purged against rebuilt against churned

**A purge brings the churned twin to the rebuilt twin's cost.** Server CPU per statement; "churned" is the mean of `x_b` and `x_c`; the two figures per cell are the two independent runs (run 1 / run 2). The delta columns are the per-repetition paired differences (three per run).

| cell | x_a rebuilt | x_p purged | churned (b, c mean) | churned / rebuilt | purged - rebuilt, per repetition | control b - c, per repetition | purged within the control |
|---|---|---|---|---|---|---|---|
| S0-1 low-cardinality probe, us | 16,148 / 15,857 | 16,112 / 15,980 | 28,324 / 27,720 | 1.754 / 1.748 | -118, -53, +64 / -51, +151, +269 | +35, +713, +907 / -656, -130, -4 | **yes** (largest +269 us, 1.7 % of the cost) |
| S0-5 unique insert, us | 28.49 / 30.24 | 28.12 / 30.10 | 47.23 / 48.73 | 1.658 / 1.611 | -0.80, -0.02, -0.29 / +0.06, -0.54, +0.07 | +0.29, -0.20, -0.49 / -0.97, +1.50, -1.89 | **yes** |
| S0-2 point probe, us | 26.95 / 26.64 | 27.20 / 26.79 | 27.23 / 26.65 | 1.010 / 1.000 | +0.12, +0.44, +0.19 / -0.14, +0.13, +0.46 | +0.09, +0.59, -0.74 / -0.28, -0.30, +0.03 | **yes** (nothing to remove) |

Derived wall throughput (`QPS = 1,000,000 / mean us`, run 1 / run 2): S0-1 `x_a` 34.3 / 39.3, `x_p` 35.3 / 38.8, `x_b` 24.4 / 26.8, `x_c` 24.8 / 26.7 probes per second (25,000 rows each); S0-5 `x_a` 18,556 / 12,856, `x_p` 19,305 / 13,388, `x_b` 14,169 / 11,640, `x_c` 13,996 / 11,015 inserts per second; S0-2 `x_a` 16,464 / 13,076, `x_p` 16,346 / 13,630, `x_b` 16,240 / 13,652, `x_c` 16,379 / 13,146 statements per second. Run 2's wall figures are lower in S0-5 and S0-2 at the same server CPU (a longer p50, 80 us against 48 us in S0-2) and carry the driver's and kernel's noise; the server CPU is the comparison.

Client-observed latency, run 1, microseconds (S0-1 has 72 statements per relation, S0-5 1,800, S0-2 12,000):

| cell / relation | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|
| S0-1 a rebuilt | 72 | 25,135 | 25,730 | 27,270 | 35,499 | 41,156 |
| S0-1 p purged | 72 | 25,132 | 25,713 | 26,408 | 35,740 | 40,679 |
| S0-1 b churned | 72 | 36,990 | 38,162 | 39,553 | 46,644 | 50,764 |
| S0-1 c churned | 72 | 36,767 | 37,644 | 38,759 | 47,191 | 48,524 |
| S0-5 a | 1,800 | 41.2 | 45.7 | 48.0 | 77.5 | 105.7 |
| S0-5 p | 1,800 | 41.7 | 45.5 | 47.8 | 76.3 | 103.7 |
| S0-5 b | 1,800 | 59.7 | 64.5 | 66.7 | 89.2 | 116.8 |
| S0-5 c | 1,800 | 60.2 | 64.4 | 66.5 | 88.9 | 115.7 |
| S0-2 a | 12,000 | 49.1 | 53.2 | 55.3 | 89.6 | 118.0 |
| S0-2 p | 12,000 | 49.2 | 53.4 | 55.6 | 91.6 | 121.7 |
| S0-2 b | 12,000 | 49.2 | 53.5 | 55.5 | 90.7 | 120.6 |
| S0-2 c | 12,000 | 48.9 | 53.4 | 55.4 | 88.6 | 120.1 |

In S0-1 and S0-5 the whole distribution of the churned twins is shifted, floor included (S0-5 p0 59.7 / 60.2 us against 41.2 us rebuilt and 41.7 us purged), so the cost is a fixed one per statement, and the purged relation returns to the rebuilt one's p0. S0-2's four relations share p0 and p25 to within 0.3 us. The S0-1 client time is mostly the 25,000-row result (server CPU 16 ms of a 25-27 ms floor); the server's run-queue wait is 5-100 us a statement.

**Structure** (`sys.indexes` entries and height; pages from the allocation counters, as DD-S0 read them: the rebuilt tree is the `pages_allocated` delta across the `REINDEX`, the churned tree the `pages_freed` delta once it is reclaimed; **`PURGE INDEX` allocated and freed 0 pages in every run, so the purged tree has the churned tree's pages by construction, not by a separate count**):

| cell | entries churned | after `REINDEX` | after `PURGE INDEX` | height (churned, rebuilt, purged) | pages rebuilt | pages churned (= purged) |
|---|---|---|---|---|---|---|
| S0-1 | 200,000 | 100,000 | 100,419 | 3, 3, 3 | 420 | 424 |
| S0-5 | 20,000 | 10,000 | 10,042 | 2, 2, 2 | 42 | 44 |
| S0-2 | 200,000 | 100,000 | 100,419 | 3, 3, 3 | 421 | 841 |

The purge leaves 419 / 42 entries more than a rebuild: **the first entry of each leaf** (DD-Q3 (a), `manual/sql/sql.md`'s "Each leaf keeps its first entry"), the order's one-a-leaf residue. At S0-2 the purged tree is **2.0x the rebuilt one's pages and probes at the same cost**, which is S0's finding about the page ratio and the cost: pages were never what a probe paid for.

## 4. The purge itself (cell 3, `cores = 1`, quiet host, no writers)

**A `PURGE INDEX` costs the walk of the relation plus the removal, about 2.4 us a removed entry at surplus equal to the live set, and 3.4x a `REINDEX`.** Server CPU equals wall time to within 0.1 ms in every row here (one thread does the work), so one figure serves for both. Rows are separate measurements, each on a freshly churned relation; n = 100,000 unless stated; the history of the first four rows is S0-2's.

| measured | entries removed | attempts | time ms | us per entry removed | us per row walked |
|---|---|---|---|---|---|
| `PURGE INDEX`, surplus = live set (6 measurements: 232.3, 240.8, 242.0, 242.3, 242.6, 251.8) | 99,581 | 14 (7 for S0-1's 420-leaf tree) | **242** (median; 232-252) | **2.43** (2.33-2.53) | 2.42 |
| the same `PURGE INDEX` run again at once, nothing to remove | 0 | 14 | 108.4, 109.3 | - | **1.08** (the floor: the judge's walk) |
| `REINDEX INDEX` on the same history, no writers | - | - | 70.9, 73.1 | - | 0.71 |
| `PURGE INDEX`, surplus = 2x the live set (a second `UPDATE` first) | 199,163 | 20 | 393.4, 402.2 | 1.97-2.02 | 3.9 |
| unique index, rows `DELETE`d, row-`PURGE`d and re-placed, n = 15,000 | 14,937 | 1 | 306.7 | **20.5** | 20.4 |
| non-unique index, the same history, n = 15,000 | 14,937 | 1 | 324.9 | **21.8** | 21.7 |
| unique index, the same history, n = 100,000 | 99,582 | 7 | **2,089** | **20.8** | 20.9 |
| non-unique index, the same history, n = 100,000 | 99,582 | 7 | 2,065 | 20.7 | 20.7 |
| unique index, the same, n = 10,000 (the `x_p` purge of section 3's S0-5, two runs) | 9,958 | 1 | 207.0, 202.6 | 20.4-20.8 | 20.6 |
| non-unique, rows `DELETE`d and **not** purged, n = 15,000 | 14,938 | 1 | 14.2 | 0.95 | 0.95 |

Four readings.

- **The removal is 1.3 us an entry; the walk is the rest.** A purge with nothing to remove still costs 1.08 us a row (14 attempts of the judge's rebuild walk), so the price is set by the relation's size first and the surplus second: doubling the surplus (99,581 to 199,163 entries) takes the time from 242 to 393-402 ms, not to 484. At n = 100,000 a purge costs 3.4 `REINDEX`es of CPU at one level of surplus (242 against 71 ms).
- **The entries of rows whose slot was `PURGE`d cost 20 us each, about 8x a live-row entry and 21x a delete-marked one** (14.2 ms against 306.7-324.9 ms for the same 14,9xx entries). **Unique or not changes nothing** (306.7 / 324.9 ms; 2,089 / 2,065 ms), so the unique check is not the cause. The mechanism is not established: the row-`PURGE`d case is the one where each entry's tuple key meets a retired slot, and this run did not profile it (no `perf` on this host, and no profiler was run). It is the only cell whose attempt approaches the budget DD-R10 names.
- **The longest attempt.** A 64-leaf attempt in the live-row shape runs about 17 ms (242 ms / 14 attempts, averaged). In the `PURGE`d-row shape a statement of one attempt took **307-325 ms**, and 7 attempts took 2,065-2,089 ms (**295-298 ms mean**). That is **30-33 % of the relation `IX` fault net of 1 s** on a quiet host (load 1.2-2.2), 3x inside it; a host twice as slow, or a tree with fuller leaves, would sit at a half. The order's DD-R10 says the leaf count is cut until the attempt "sits far inside the net"; **a third of it is the measurement, and whether that is "far" is the operator's reading** (item 1 below). The per-attempt time was inferred from `attempts=` and the statement's time; no per-attempt clock exists in the reply.
- **Correctness check.** After every purge in cell 3, every relation answered `SELECT * ... WHERE id >= 1` and `WHERE v >= 0` (the second through the index) with 100,000 rows, including those whose writers updated the indexed column during the purge. `skipped_rows` was **0 in every measurement** (including the writer runs of section 5): the try-only tuple `U` never met a writer that held a row's unit.

## 5. Cell 3: writers across one statement, beside S0's cell 7

**At `cores = 1` a purge keeps every writer off the relation for the whole statement, and for longer than the `REINDEX` does, because the statement is a loop on its reactor that does not yield between attempts.** Two writer connections ran `UPDATE x SET <col> = ? WHERE id = ?` in a loop on the relation (`col = w`: not the indexed column; `col = v`: the indexed column, which also makes new stale entries) from 0.5 s before the statement to 0.5 s after it, on a fresh relation each (n = 100,000, surplus = live set); a third connection issued the statement. The control window issues no statement and waits 0.3 s. "During" is every writer statement that overlapped the statement's window.

`cores = 1` (cells run 14:06:36-14:06:53, load 1.8-2.0, 0 competing):

| statement | writers | statement ms | writer statements overlapping it | longest writer stall ms | refusals | writers before: ops, p0, p25, p50, p95, p99 us |
|---|---|---|---|---|---|---|
| `REINDEX INDEX` | w | 71 | 4 | 70.65 | 0 | 21,075; 31; 39; 43; 78; 95 |
| `PURGE INDEX` | w | **243** | 4 | **243.3** | 0 | 21,191; 30; 38; 43; 77; 94 |
| `PURGE INDEX` | v | **283** | 4 | **283.9** | 0 | 21,741; 32; 38; 43; 60; 101 |
| control, no statement (0.3 s) | w | - | 11,493 | 5.5 (host) | 0 | 18,324; 30; 39; 44; 82; 225 |

The "4 overlapping statements" are the two writers' last statement before the window and first after it: **no writer statement completed inside either statement's window**, and the longest stall equals the statement's whole duration to within 0.3 ms in every row. A writer's p50 is 43 us outside the window. No writer was refused (0 refusals in 33,600-43,200 writer statements per run). Against DD-S0's cell 7 (`REINDEX` of the same relation, `054dbc5e`, a busy host at load 17-20): 156-178 ms there, **71 ms here** with the same statement from a binary whose `REINDEX` DD did not touch, so the difference between the two files is the host, not the engine; **the like-for-like delta on this host is the purge against the `REINDEX`: 243 ms against 71 ms (3.4x)**.

`cores = 2` (`p2`: 14:06:53-14:07:11, load 1.95-3.3; `p2b`: 14:07:11-14:07:32, **a competing build, load 3.3-5.4, flagged and noisy**):

| run | statement | writers | statement ms | writer statements overlapping | their p50, p99 us | longest stall ms | refusals inside the window |
|---|---|---|---|---|---|---|---|
| p2 | `REINDEX INDEX` | w | 99 | 4 | 289, 98,809 (the two writers each stalled) | 98.8 | 0 |
| p2 | `PURGE INDEX` | w | 350 | **6,235** | 53, 131 | **349.9** | 0 |
| p2 | `PURGE INDEX` | v | 369 | **2,366** | 63, 108 | 316.6 | 0 |
| p2 | control, 0.3 s | w | - | 11,824 | 46, 108 | 1.6 | 0 |
| p2b | `REINDEX INDEX` | w | 187 | 4 | 339, 187,689 | 187.7 | 0 |
| p2b | `PURGE INDEX` | w | 518 | 4,385 | 79, 753 | 518.4 | 0 |
| p2b | `PURGE INDEX` | v | 574 | 664 | 100, 1,311 | 547.3 | 0 |
| p2b | control, 0.3 s | w | - | 5,585 | 72, 1,160 | 6.8 | 0 |

With a second core, **writers on the other core go on through the purge** (6,235 statements at a 53 us median in 350 ms in p2, 17.8 per ms, against 11,824 in the statement-free 0.3 s window, 39.4 per ms: about 45 % of the free rate, one of the two writers being the stalled one) **and are held off by the `REINDEX`'s `X` (4 statements: both writers stall the whole 99 ms)**. The writer session that shares the purge's reactor waits the whole statement (349.9 ms against a 350 ms statement; 518 ms against 518 ms). Refusals: none inside any window; the refusals in the whole-run counts (0-13 per run, `row id=... was written by transaction ...`) are the two writer connections colliding on one row at `cores = 2`, and they occur in the 0.3 s control window as well (1 in p2, 13 in p2b), outside the statements. `skipped_rows=0` in all.

**What this contradicts.** The manual (`manual/sql/sql.md`, "PURGE INDEX") says **"Writes go on while it runs"** and the order's DD-R10 bounds the relation `IX` an attempt holds; both are true of the lock: nothing is held between attempts (read in `src/server/command_dispatcher.cpp` at `b8880f9b`, the `do { BeginWrite ... EndWrite } while (!resume.empty())` loop of `PURGE INDEX`'s handler, which has no suspension between attempts), and the writer run shows 0 refusals and 0 `skipped_rows`. They are not true of the reactor: at `cores = 1` **no writer runs while the statement does**, and at `cores = 2` the sessions on the statement's own core wait for all of it. The leaf count per attempt (64) bounds a DDL's wait, not a reactor's occupancy.

## 6. What the run teaches

- **DD is free where it is not used.** The only code DD put on an existing path is the covered copy `AppendIndexEntry` shares with the judge; across six shapes and three sizes against an in-run control its cost is not distinguishable from zero (+/- 1.5 %), and the shape most likely to show it, an indexed-column update, leans +1.0-1.4 % with its own control leaning +0.7-0.8 %.
- **The purge does what the order built it for, and it is a rebuild's walk plus a removal, priced like one.** A churned index costs +61 to +75 % a statement in the two cells where stale entries share a probe's run; after a purge the cost equals a rebuild's to within 1.7 % of the statement; the pages stay (2.0x at S0-2) and the point probe never cared. This is the order's DD-Q0 premise (a stale entry costs a probe only where it is read) now seen from the purge's side: removing entries a statement reads buys the statement's cost back, removing pages is a `REINDEX`'s.
- **A purge is a walk of the relation, not of the surplus.** 1.08 us a row with nothing to remove against 2.43 us an entry at surplus = live set: a purge of a lightly churned 100,000-row index still costs about 108 ms and 14 attempts. An operator who purges on a schedule pays the walk each time.
- **The cost that is `PURGE INDEX`'s own, and the one the order's stall leg wanted to remove, is not removed at one core.** DD-Q0's rationale for a purge beside `REINDEX` included writers not stalling for the build; at `cores = 1` the stall grew from 71 ms (`REINDEX`) to 243-283 ms (purge) at n = 100,000, linear in the relation and 2.07 s where the surplus is row-`PURGE`d rows. The lock family's 1 s net does not govern that occupancy (no lock is awaited during it), so nothing refuses the statement or its neighbours however long it runs; the heavy shape's 2.07 s at n = 100,000 is already past the net's figure.
- **The row-`PURGE`d entry is 20 us and is the thing to profile.** It is about 8x the live-row entry and independent of the index being unique; if it is a descent that meets a retired slot, a cheaper miss would take the heavy shape from 300 ms an attempt to the live-row shape's 17 ms. No data here says why.

## 7. Items for the operator, and what was cut

**Items back to the operator.**
1. **DD-R10's "far inside the net".** The longest attempt measured is **0.30-0.33 s of a 1 s net** on a quiet host, in the shape where every stale entry's row was row-`PURGE`d. Whether the 64-leaf count should be cut (to bound a DDL's wait at, say, 0.1 s) is a ruling; the measurement does not decide it.
2. **A purge's reactor occupancy at `cores = 1`.** The statement does not suspend between attempts, so every other session on its core waits the statement's whole time (243-283 ms at n = 100,000, up to 2.07 s for the heavy shape). Yielding between attempts (the walking `SELECT`'s every-64-pages yield, `docs/spec/sched.md` section 3, is the existing pattern) is the change that would make "writes go on while it runs" true at one core; it is an engine change this run does not make. The manual's sentence is true across cores and across the lock, and untrue of the reactor.
3. **The 20 us row-`PURGE`d entry** (section 4) is not explained; a profile of that one statement would say whether it is a descent per entry.

**Cut to fit, and not run.**

| cut | why |
|---|---|
| no `cores > 1` write-path A/B; no `group` or `strict` rung; no PostgreSQL floor | the order's standing cuts (`workorder-dd-index-entry-purge.md`, DD-Q7's budget); the write-path bar is read at `cores = 1`, `relaxed` only, so any cost DD added to a multi-core or a durable-commit path is not measured |
| S0's cells 3, 4, 6 (a covered-column probe, an indexed-column `UPDATE` on a churned twin, a probe under a smaller pool) not repeated on a purged twin | cell 4's shape is the A/B update-idx row, and cell 6 never separated its twins in S0; cells 1, 5 and 2, S0's verdict cells and the point probe, are the ones run |
| one surplus level (equal to the live set) in the twin cells; 2x in cell 3's CPU row only | the order's budget; the purge's CPU against surplus is two points (1x, 2x), not a fit |
| no cold-device read | as DD-S0: every page was in the host's page cache |
| no concurrent `PURGE INDEX` against a `REINDEX`, a second purge or a DDL | the test suite carries two purges at once; no measurement |
| the A/B write-path numbers are `prepared` only | S0's `bx_ab.py`-style driver also ran the literal path; the shapes' parse cost is not what DD touched |
| `p2b`'s host was busy (a competing build from 14:07:11) | reported flagged; the `cores = 2` rows should be read from `p2` |

**Time spent.** Build of B (about 2 minutes: another session's build had ended; A reused from S0), about 11 minutes of cells, about 15 minutes of writing and analysis; 14:11 UTC for this report, 17 minutes after the session began (13:54) against the order's hour, with no quiet-host hold counted. The 60 s watchdog's records (`/home/louis/bench-runs/dd-close/watch_*.txt`) show the progress files advancing every poll; one watcher was lost to a script error and restarted before its cell finished.
