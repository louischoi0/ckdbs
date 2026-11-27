# DX close: a pool that is not full pays nothing - the DX binary is no costlier than its base on any of 24 resident cells, and reads 1.5 % cheaper

**Thesis.** DX-R5 (`instructions/v3.2.0/workorder-dx-pool-under-pressure.md` section 3) says a pool that is not full executes no new instruction on the hit path or the free-list path. Measured on worktree `dx-pool-under-pressure` with DX's closing code `5e25f1d26` (`v3.4.17-205-g5e25f1d26`; HEAD `8765d2d34`, `v3.4.17-206-g8765d2d34`, adds a results file only, so its binary is the same) against the commit DX opened from, `481d101aa` (`v3.4.17-202-g481d101aa`), interleaved, with a pool roomy enough that nothing was evicted or refused:

- **No cost was found.** Server CPU per unit, DX / base, median over 24 cells (4 shapes x BTREE/HEAP x 200/1,000/10,000 rows): **0.985**, cells 0.973-0.994, none at or above 1. Per-round totals over the 24 cells, 24 rounds: median 0.985, range 0.969-1.000. The control (a second copy of the base binary) reads 0.999 over cells (0.993-1.005) and 0.999 per round (0.986-1.019).
- **The sign is the opposite of a cost, and it is reproduced.** Run 1 (12 rounds) read 0.984, run 2 (12 rounds, a fresh start) 0.982. The DX binary reads about 1.5 % cheaper on every shape (select by key 0.986, insert 0.984, update by key 0.976, explicit-transaction rollback 0.986). The saving is **not attributed**: nothing in DX's diff removes work from these paths (the diff only adds, section 5), so it is read as a property of the binary pair (build, layout), not as a DX effect. The DW close measured the same sign for another pair of binaries (`results-dw-close-v3.4.17-185-gf85aa3042.md`, B-off / A 0.981, there against a change that added work), where the same reading applied.
- **The residency premise held in every one of 72 arm runs**: `pool_refused` 0, inline and background reclaim 0, inline and background reclaim batches 0, resident frames 534 of 262,144, misses 534 (every first touch), and **the same pool-hit count in all three arms** (529,982 or 529,983). DX did not change how many pool operations a statement performs, either.
- **DX-R5 holds on this reading**, to the resolution of this harness: a 1-2 % CPU difference a cell is not separable from the control's spread, and nothing above the control's spread was seen. It is **not** a proof of "no new instruction"; section 5 says which new instructions the diff does add to these paths.

## 1. Stamp

| Item | Value |
|---|---|
| Executed (UTC, `date -u`) | 2026-11-27. Run 1 14:37:25-14:40:25 (12 rounds), run 2 14:40:44-14:43:44 (12 rounds). **Timed measurement 6 min** wall; with the builds already done, the driver written and a smoke run, about 25 min in all |
| Branch / worktree | `worktree-dx-pool-under-pressure`, worktree `dx-pool-under-pressure`. Tree at HEAD `8765d2d34` had uncommitted `CLAUDE.md` and the work order (not this run's); this run added `tools/dx_close.py`, `tools/dx_close_report.py` (untracked) |
| Commits measured | **A (base)**: `481d101aa`, `git describe --tags` = `v3.4.17-202-g481d101aa`, committed 2026-11-27 06:55:30 -0500. **B (DX)**: `5e25f1d26`, `v3.4.17-205-g5e25f1d26`, committed 2026-11-27 09:32:25 -0500. `git diff --shortstat 481d101aa 5e25f1d26 -- src include`: 10 files, 236 insertions, 60 deletions |
| Binary provenance | Each built from `git archive` of its commit, Release, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`. A: `build-dx-base/rel/kds_server`, mtime 2026-11-27 07:07:59 -0500 (after its commit, by 12 min). B: `build-dx-s2/rel/kds_server`, mtime 09:33:06 -0500 (after its commit, by 41 s). Both hashes re-read at run time and equal the brief's. Run from copies under `/home/ukjin/bench-runs/dx-close/bin/` (`A`, `A2` = a second copy of A, `B`), sha256 A and A2 `17cda4082ac1714e5ba146bf72b212f50593de24aead4a4a87b61fb736cce0c7`, B `926719225d557275ae56ddbfa83385eb5dd5b7a6634388cfa71f3a9bb35ec25e`; the driver re-hashed each copy at each arm start (`meta.json`, per-arm `bin_sha`) |
| Device | data files, WALs and binary copies under `/home/ukjin/bench-runs/dx-close/` on `/dev/mapper/fedora-root`, xfs (`df -T`), not tmpfs. Data files and WAL segments are not archived |
| Build type | Release |
| Server config | `cores = 1`, `durability = relaxed`, **`buffer_pool_frames = 262144`** (2 GiB; the working set is 534 frames), `log_level = warn`, everything else default; fresh server and data file per arm and round |
| Host | Intel Core Ultra 7 270K Plus, 24 CPUs, P-cores 0-7 (hybrid), Linux 7.3.6. **Server pinned to CPUs 0-7** (`taskset`), driver pinned to E-core 8. Ports 15611-15639 and 15651-15679 (not 15432, not 15781) |
| Host load | `/proc/loadavg` before every arm, in `dx.json`: 1-minute load 0.61-1.55 across the 72 arm runs, `competing` (a `cc1plus`, `cmake --build` or `ctest`) 0 in every record; quiet-gate hold 0 s. **Two other `kds_server` processes were up** and not touched: firestrike's instance (`/home/ukjin/.local/state/firestrike/kdbs`) and one under `/sts/kds.conf` (2.4 % CPU lifetime mean). Neither is separable from the load figures |
| Watchdog | a 60 s poll of `hb.json`'s age, written by the driver (`watch.log`, `watch2.log`): age 0-3 s at every poll, no orphan, no resume needed |
| Driver | `tools/dx_close.py` (new, uncommitted; imports `tools/dw_close.py`, `tools/dq_s0_premise.py`, `tools/kwp.py` unmodified, overrides only the server pin to `0-7`), tables by `tools/dx_close_report.py`; as-run copies in the archive. Server CPU is the sum of `/proc/<pid>/task/*/schedstat` over the server's threads, read once no thread is runnable |
| Suite | Not executed (no engine code changed in this task) |
| Prior number | **The DW close's B-off arm**, `v3.4.17-185-gf85aa3042`, for the shapes `sel_pk`, `ins`, `upd_nk` (same driver shape, relations and statements; a different commit, a different CPU pin (0-1 there, 0-7 here) and a different hour). The rollback shape has none and becomes the baseline of the next. PostgreSQL floor: not measured for these shapes |

Raw output: `bench/v3.5.0/archive/dx-close-v3.4.17-206-g8765d2d34/` (`run1/`, `run2/`, `pooled/` with `dx.json` and `report.md` each, logs, watchdog, as-run drivers). Each cell is a driver flag combination documented in `tools/dx_close.py`'s docstring; `bench/docs/` carries no entry for it (README says the directory stays closed until a v3 driver needs it).

## 2. Method

Three arms: **A** the base, **A2** a second copy of A (the control: what two runs of one engine differ by), **B** DX. Per round the three arms run once each in one of the six orders of three, so over 6 rounds each arm sits in every position equally; 24 rounds (two runs of 12) gave each order four times. On each arm, per BTREE and HEAP relation of 200, 1,000 and 10,000 rows (the **row count when the phases start**; `(id int64 PRIMARY KEY, k int64, w int64)` with `CREATE INDEX ix ON t (k)`, as in the DW close), **2,000 prepared units a shape**, in order:

| shape | statement | which DX code it reaches |
|---|---|---|
| `sel_pk` | `SELECT * FROM t WHERE id = ?`, ids cycled over the rows | the hit path |
| `ins` | `INSERT INTO t VALUES (?, ?, 0)`, new ids, autocommit | the free-list path, a write's `EndWrite` and statement staging, writeback of new pages |
| `upd_nk` | `UPDATE t SET w = ? WHERE id = ?`, autocommit | `EndWrite`'s thread-local read, `StageStatement`'s clear |
| `rb` | `BEGIN`, the same `UPDATE`, `ROLLBACK` (one unit is three frames) | every abort entry's `RequireNoPinsHeld` |

Server CPU per unit is the **median of 20 batches of 100 units** per phase. Client latency is per unit. Throughput columns are derived (units / s = 1,000,000 / mean client latency, not separately controlled) and are in section 3.3. The pool counters are read from `SHOW META` after the loads and after the phases, outside the batches. The comparison is **B / A per cell, median over rounds with min-max**, against **A2 / A** as the noise floor. The sweep of 200 / 1,000 / 10,000 rows is a row of the table; none of the four shapes scales with the row count (section 4).

## 3. Results

### 3.1 CPU per unit, DX against base, with the control and the previous number

24 rounds a cell. B / A is below 1 in every cell (0.973-0.994 median) and the control A2 / A is within 0.993-1.005 in every cell. The per-cell min-max brackets (0.875-1.160) are single-round batch noise on a loaded shared host and are listed, not hidden; the 24-round median is the number read. The **previous number** column is the DW close's B-off at `f85aa3042`, a different engine commit and a different pin; it places these absolute costs in this engine's own history and is not a controlled delta (B vs prior -1.9 % to +1.8 %, mean +0.4 %, which is the size of the host and pin difference).

| relation | rows | shape | A cpu (base) | A2 cpu (control) | B cpu (DX) | B / A, median [min-max over 24 rounds] | A2 / A, median [min-max] | prior B-off cpu (DW close, `f85aa3042`) | B vs prior |
|---|---|---|---|---|---|---|---|---|---|
| BTREE | 200 | sel_pk | 6.061 us | 6.070 us | 5.986 us | 0.986 [0.954-1.160] | 1.003 [0.982-1.126] | 5.93 us | +0.9 % |
| BTREE | 200 | ins | 6.438 us | 6.420 us | 6.306 us | 0.979 [0.940-1.043] | 0.996 [0.972-1.050] | 6.22 us | +1.4 % |
| BTREE | 200 | upd_nk | 6.456 us | 6.453 us | 6.316 us | 0.976 [0.875-1.103] | 0.993 [0.920-1.070] | 6.29 us | +0.4 % |
| BTREE | 200 | rb | 13.013 us | 13.011 us | 12.822 us | 0.992 [0.936-1.103] | 1.002 [0.936-1.099] | none (shape new here) | - |
| BTREE | 1000 | sel_pk | 6.081 us | 6.067 us | 6.005 us | 0.987 [0.965-1.000] | 1.000 [0.978-1.014] | 5.96 us | +0.8 % |
| BTREE | 1000 | ins | 6.419 us | 6.389 us | 6.264 us | 0.976 [0.949-1.030] | 0.997 [0.973-1.015] | 6.24 us | +0.4 % |
| BTREE | 1000 | upd_nk | 6.448 us | 6.430 us | 6.307 us | 0.978 [0.893-1.005] | 1.000 [0.924-1.144] | 6.28 us | +0.4 % |
| BTREE | 1000 | rb | 13.088 us | 13.117 us | 12.908 us | 0.986 [0.951-1.036] | 1.001 [0.962-1.047] | none (shape new here) | - |
| BTREE | 10000 | sel_pk | 6.042 us | 6.075 us | 5.976 us | 0.987 [0.966-1.103] | 1.002 [0.975-1.037] | 5.97 us | +0.1 % |
| BTREE | 10000 | ins | 6.412 us | 6.423 us | 6.252 us | 0.974 [0.944-1.014] | 1.003 [0.964-1.030] | 6.31 us | -0.9 % |
| BTREE | 10000 | upd_nk | 6.435 us | 6.409 us | 6.305 us | 0.973 [0.950-1.017] | 0.998 [0.973-1.022] | 6.30 us | +0.1 % |
| BTREE | 10000 | rb | 12.938 us | 13.012 us | 12.815 us | 0.982 [0.929-1.017] | 1.005 [0.945-1.039] | none (shape new here) | - |
| HEAP | 200 | sel_pk | 6.794 us | 6.783 us | 6.684 us | 0.985 [0.962-1.015] | 1.000 [0.961-1.027] | 6.67 us | +0.2 % |
| HEAP | 200 | ins | 8.178 us | 8.145 us | 8.098 us | 0.991 [0.973-1.013] | 0.996 [0.975-1.019] | 8.00 us | +1.2 % |
| HEAP | 200 | upd_nk | 6.970 us | 6.954 us | 6.806 us | 0.976 [0.933-0.995] | 0.996 [0.942-1.029] | 6.78 us | +0.4 % |
| HEAP | 200 | rb | 13.497 us | 13.446 us | 13.287 us | 0.991 [0.927-1.055] | 0.997 [0.954-1.054] | none (shape new here) | - |
| HEAP | 1000 | sel_pk | 6.605 us | 6.617 us | 6.475 us | 0.983 [0.953-1.016] | 0.999 [0.973-1.028] | 6.60 us | -1.9 % |
| HEAP | 1000 | ins | 8.133 us | 8.108 us | 8.043 us | 0.989 [0.968-1.063] | 0.999 [0.977-1.024] | 7.94 us | +1.3 % |
| HEAP | 1000 | upd_nk | 6.997 us | 6.979 us | 6.821 us | 0.978 [0.948-1.158] | 0.996 [0.964-1.021] | 6.79 us | +0.5 % |
| HEAP | 1000 | rb | 13.575 us | 13.538 us | 13.431 us | 0.987 [0.954-1.160] | 0.999 [0.968-1.046] | none (shape new here) | - |
| HEAP | 10000 | sel_pk | 6.610 us | 6.615 us | 6.514 us | 0.989 [0.960-1.012] | 1.002 [0.979-1.037] | 6.58 us | -1.0 % |
| HEAP | 10000 | ins | 8.173 us | 8.159 us | 8.116 us | 0.994 [0.956-1.019] | 0.997 [0.960-1.040] | 8.05 us | +0.8 % |
| HEAP | 10000 | upd_nk | 7.005 us | 7.000 us | 6.841 us | 0.973 [0.956-1.002] | 0.998 [0.970-1.029] | 6.72 us | +1.8 % |
| HEAP | 10000 | rb | 13.549 us | 13.534 us | 13.341 us | 0.984 [0.947-1.057] | 1.001 [0.962-1.033] | none (shape new here) | - |

Medians by shape over its 6 cells (B / A against the control A2 / A): select by key **0.986** (control 1.001), insert **0.984** (0.997), update by key **0.976** (0.997), rollback **0.986** (1.001). Totals of the 24 cell medians by position in the round, A: 202.1 / 201.9 / 202.1 us, A2: 201.0 / 201.9 / 202.1 us, B: 200.1 / 198.3 / 199.0 us, so the order the arms ran in does not explain the sign. The statements/s of the same units, derived: select 40,816 / 40,650 / 40,816 per s (A / A2 / B), insert 48,310 / 48,077 / 48,309, update 50,251 / 50,251 / 50,378, rollback unit 23,041 / 23,148 / 23,256; B / A 1.000, 1.000, 1.003, 1.009 against a control 0.996-1.005, **inside the control, so not a finding**: the client is 67-75 % of the wall and swamps a 1.5 % server delta.

### 3.2 Client latency, base against DX

Median over 24 rounds of each phase's percentile (2,000 operations per phase per round). A2 is in `pooled/dx.json`. The `sel_pk` p95 and p99 sit well above its p50 (the 200-row BTREE p99 is about 70 us against a 24 us p50) on both arms alike; that tail is not attributed here.

| relation | rows | shape | arm | ops per round | p0 | p25 | p50 | p95 | p99 | server CPU / unit | server share of p50 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| BTREE | 200 | sel_pk | A | 2000 ops | 22.4 us | 23.8 us | 24.0 us | 39.9 us | 69.6 us | 6.06 us | 25 % |
| BTREE | 200 | sel_pk | B | 2000 ops | 22.2 us | 23.7 us | 23.9 us | 40.8 us | 70.9 us | 5.99 us | 25 % |
| BTREE | 200 | ins | A | 2000 ops | 17.7 us | 19.1 us | 19.3 us | 20.2 us | 27.2 us | 6.44 us | 33 % |
| BTREE | 200 | ins | B | 2000 ops | 17.5 us | 18.9 us | 19.1 us | 20.1 us | 27.4 us | 6.31 us | 33 % |
| BTREE | 200 | upd_nk | A | 2000 ops | 17.7 us | 19.3 us | 19.4 us | 20.1 us | 22.0 us | 6.46 us | 33 % |
| BTREE | 200 | upd_nk | B | 2000 ops | 17.7 us | 19.1 us | 19.3 us | 20.1 us | 21.9 us | 6.32 us | 33 % |
| BTREE | 200 | rb | A | 2000 ops | 39.5 us | 41.7 us | 42.2 us | 44.0 us | 46.0 us | 13.01 us | 31 % |
| BTREE | 200 | rb | B | 2000 ops | 39.2 us | 41.4 us | 42.0 us | 44.0 us | 46.5 us | 12.82 us | 31 % |
| BTREE | 1000 | sel_pk | A | 2000 ops | 22.3 us | 23.8 us | 23.9 us | 24.6 us | 26.9 us | 6.08 us | 25 % |
| BTREE | 1000 | sel_pk | B | 2000 ops | 22.2 us | 23.6 us | 23.8 us | 24.7 us | 27.0 us | 6.01 us | 25 % |
| BTREE | 1000 | ins | A | 2000 ops | 17.6 us | 19.1 us | 19.3 us | 20.1 us | 26.9 us | 6.42 us | 33 % |
| BTREE | 1000 | ins | B | 2000 ops | 17.4 us | 18.9 us | 19.1 us | 20.0 us | 27.1 us | 6.26 us | 33 % |
| BTREE | 1000 | upd_nk | A | 2000 ops | 17.6 us | 19.1 us | 19.3 us | 20.1 us | 22.2 us | 6.45 us | 33 % |
| BTREE | 1000 | upd_nk | B | 2000 ops | 17.5 us | 19.0 us | 19.2 us | 20.0 us | 21.9 us | 6.31 us | 33 % |
| BTREE | 1000 | rb | A | 2000 ops | 39.4 us | 41.7 us | 42.3 us | 44.2 us | 46.2 us | 13.09 us | 31 % |
| BTREE | 1000 | rb | B | 2000 ops | 39.0 us | 41.2 us | 41.8 us | 43.9 us | 45.5 us | 12.91 us | 31 % |
| BTREE | 10000 | sel_pk | A | 2000 ops | 22.2 us | 23.7 us | 23.9 us | 24.6 us | 27.0 us | 6.04 us | 25 % |
| BTREE | 10000 | sel_pk | B | 2000 ops | 22.2 us | 23.6 us | 23.8 us | 24.6 us | 27.4 us | 5.98 us | 25 % |
| BTREE | 10000 | ins | A | 2000 ops | 17.6 us | 19.1 us | 19.2 us | 20.1 us | 26.9 us | 6.41 us | 33 % |
| BTREE | 10000 | ins | B | 2000 ops | 17.4 us | 18.8 us | 18.9 us | 19.9 us | 26.9 us | 6.25 us | 33 % |
| BTREE | 10000 | upd_nk | A | 2000 ops | 17.7 us | 19.1 us | 19.3 us | 20.0 us | 21.9 us | 6.44 us | 33 % |
| BTREE | 10000 | upd_nk | B | 2000 ops | 17.4 us | 18.9 us | 19.1 us | 19.9 us | 21.9 us | 6.30 us | 33 % |
| BTREE | 10000 | rb | A | 2000 ops | 39.3 us | 41.4 us | 42.0 us | 44.0 us | 46.1 us | 12.94 us | 31 % |
| BTREE | 10000 | rb | B | 2000 ops | 39.1 us | 41.0 us | 41.5 us | 43.2 us | 45.3 us | 12.81 us | 31 % |
| HEAP | 200 | sel_pk | A | 2000 ops | 22.5 us | 24.1 us | 24.4 us | 25.2 us | 27.4 us | 6.79 us | 28 % |
| HEAP | 200 | sel_pk | B | 2000 ops | 22.5 us | 24.0 us | 24.3 us | 25.1 us | 27.2 us | 6.68 us | 28 % |
| HEAP | 200 | ins | A | 2000 ops | 19.1 us | 20.6 us | 20.9 us | 21.9 us | 28.1 us | 8.18 us | 39 % |
| HEAP | 200 | ins | B | 2000 ops | 18.9 us | 20.5 us | 20.8 us | 21.8 us | 28.5 us | 8.10 us | 39 % |
| HEAP | 200 | upd_nk | A | 2000 ops | 18.0 us | 19.5 us | 19.8 us | 20.6 us | 22.5 us | 6.97 us | 35 % |
| HEAP | 200 | upd_nk | B | 2000 ops | 17.7 us | 19.3 us | 19.6 us | 20.6 us | 23.4 us | 6.81 us | 35 % |
| HEAP | 200 | rb | A | 2000 ops | 39.7 us | 42.0 us | 42.8 us | 44.3 us | 46.2 us | 13.50 us | 32 % |
| HEAP | 200 | rb | B | 2000 ops | 39.2 us | 41.5 us | 42.0 us | 44.2 us | 47.2 us | 13.29 us | 32 % |
| HEAP | 1000 | sel_pk | A | 2000 ops | 22.6 us | 24.1 us | 24.4 us | 25.3 us | 27.8 us | 6.61 us | 27 % |
| HEAP | 1000 | sel_pk | B | 2000 ops | 22.5 us | 24.0 us | 24.3 us | 25.1 us | 27.6 us | 6.47 us | 27 % |
| HEAP | 1000 | ins | A | 2000 ops | 19.1 us | 20.5 us | 20.8 us | 21.9 us | 28.1 us | 8.13 us | 39 % |
| HEAP | 1000 | ins | B | 2000 ops | 18.9 us | 20.4 us | 20.7 us | 21.8 us | 27.6 us | 8.04 us | 39 % |
| HEAP | 1000 | upd_nk | A | 2000 ops | 17.9 us | 19.5 us | 19.8 us | 20.6 us | 22.6 us | 7.00 us | 35 % |
| HEAP | 1000 | upd_nk | B | 2000 ops | 17.9 us | 19.4 us | 19.7 us | 20.5 us | 22.4 us | 6.82 us | 35 % |
| HEAP | 1000 | rb | A | 2000 ops | 39.7 us | 42.0 us | 42.8 us | 44.5 us | 46.4 us | 13.57 us | 32 % |
| HEAP | 1000 | rb | B | 2000 ops | 39.6 us | 41.9 us | 42.7 us | 44.5 us | 46.5 us | 13.43 us | 31 % |
| HEAP | 10000 | sel_pk | A | 2000 ops | 22.6 us | 24.1 us | 24.4 us | 25.1 us | 27.1 us | 6.61 us | 27 % |
| HEAP | 10000 | sel_pk | B | 2000 ops | 22.5 us | 24.1 us | 24.3 us | 25.1 us | 27.3 us | 6.51 us | 27 % |
| HEAP | 10000 | ins | A | 2000 ops | 19.1 us | 20.6 us | 20.9 us | 22.0 us | 28.4 us | 8.17 us | 39 % |
| HEAP | 10000 | ins | B | 2000 ops | 19.0 us | 20.5 us | 20.8 us | 22.1 us | 33.1 us | 8.12 us | 39 % |
| HEAP | 10000 | upd_nk | A | 2000 ops | 17.9 us | 19.5 us | 19.8 us | 20.7 us | 22.6 us | 7.00 us | 35 % |
| HEAP | 10000 | upd_nk | B | 2000 ops | 17.9 us | 19.4 us | 19.7 us | 20.6 us | 22.5 us | 6.84 us | 35 % |
| HEAP | 10000 | rb | A | 2000 ops | 39.8 us | 42.2 us | 43.0 us | 44.5 us | 46.2 us | 13.55 us | 32 % |
| HEAP | 10000 | rb | B | 2000 ops | 39.8 us | 41.8 us | 42.2 us | 44.3 us | 46.2 us | 13.34 us | 32 % |

### 3.3 Waits (rule 3)

A unit here is client work plus the loopback round trip plus the server's CPU. The server's measured share of the client p50 is in the last column: **25 % of a select by key, 33 % of an insert or an update by key, about 31 % of the rollback unit** (three frames, 13 us of 42 us). The remainder, 67-75 %, is the Python client and the loopback socket and is not decomposed further here. Durability is `relaxed`, so **no commit (fsync) wait is measured** (`strict` was cut), a single connection has **no lock or conflict wait**, and with the pool resident there is **no read I/O wait** (misses = first touches only). DX's own waits (a refused statement's drain, a re-run) are not on this path by design: that is what the zero in `pool_refused` is evidence of.

## 4. What the run says about the engine

1. **The non-refused path is flat.** The pool counters are the evidence that it was a resident control and not a harness artefact: every arm in every round ended with `pool_refused` 0, no reclaim batch, 534 resident frames, and a **hit count that is identical across the three arms** (529,982-529,983). The same number of page operations ran on the base and on DX; the cost difference, if any, is per operation, not in the count. The DX code on these paths is a thread-local read and clear and a branch that is taken only on a refusal; this run resolves it as below the control's 1-2 % spread, in the direction of cheaper.
2. **The rollback unit has no prior and now has a baseline**: 12.7-13.5 us of server CPU for `BEGIN`, one-row `UPDATE`, `ROLLBACK` (BTREE 12.8-13.1, HEAP 13.3-13.6 across the arms), about two single autocommit updates, and no row-count dependence. DX-S1 put `RequireNoPinsHeld` at each abort entry; B is 1.4 % under A on this unit (0.982-0.992 per cell medians), so that check costs nothing this run resolves.
3. **No shape scales with the row count**, as a point read, an insert and an update by key should not: across 200, 1,000 and 10,000 rows each shape's CPU (max / min over the three sizes) varies by 0.2-1.2 % on BTREE and 0.5-3.2 % on HEAP, the widest being the HEAP select by key (3 %, with the 200-row relation the dearest), on both arms alike. The HEAP insert is the dearest shape at 8.1 us. The flatness is the evidence of it (rule 9); the HEAP select's 3 % is recorded and not explained.
4. **The base-to-DX saving is unexplained and is not claimed.** Both the DW close and this one read the later binary of a pair cheaper by 1.5-2 % on every shape with the same control at 1.000. A third binary built at one commit by two build directories would separate "binary layout" from "code"; it was not run.

## 5. What was not measured, and the budget

- **What the diff adds on these paths**: `git diff 481d101aa 5e25f1d26 -- src include` adds a thread-local pin-count check at each abort entry (DX-S1), a thread-local refusal note cleared per statement and read in `EndWrite` only on failure, a re-run branch in `DispatchAsync` taken only on a refusal, and moves writeback's checksum stamp from the live frame to the scratch copy (DX-S2). None is "no new instruction": DX-R5 as written ("executes no new instruction") is **not** what was measured; what holds is "no cost this harness resolves", stated in the thesis. A cost under 1-2 % a unit (about 0.1 us of 6-8 us) would not be separated from the control by this design.
- **Cut for the budget or not applicable to the claim**: `strict` durability (the writeback checksum move sits beside fsync's wait and the fsync wait would hide a 0.1 us cost); `cores > 1` (the drain and the thread-local note are per reactor and the claim is about one path); an explicit transaction that commits; a heap and BTREE of more than 10,000 rows; the PostgreSQL floor (rule 4: not measured for these shapes). The pressured cell (a dirty pool) is DX-S2's own file, `results-dx-s2-dirty-pool-v3.4.17-205-g5e25f1d26.md`, and is not repeated.
- **Controls**: the control A2 is one more copy of the base binary, so it measures run-to-run and slot-to-slot differences of one engine; it does not separate a build's layout from its code (section 4, item 4).
- **Time**: 6 min of timed measurement, two runs of 12 rounds; the run was cheap enough to repeat, so it was repeated rather than cut.
