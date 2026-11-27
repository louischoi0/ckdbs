# DW close: with the key absent the change costs no statement anything, and with it on the unasked purge holds a btree index to 1.1-2.0 times its live rows for 1.5-7 % of one core

**Thesis.** On worktree `dw-btree-purge-driver` at `0c2092df8` (`v3.4.17-183-g0c2092df8`), three arms ran on the same host: **A**, the binary the milestone opened from (`630ecc359`); **B-off**, the closing commit with `index_purge_driver` absent; **B-on**, the closing commit with `index_purge_driver = on`. **The data support three statements and leave two questions open.**

1. **B-off is not slower than A.** Over 39 cells (6 shapes x 3 sizes x BTREE/HEAP, plus the heap release's idle tail), the server CPU per statement of B-off / A has a median of **0.984** (cell medians 0.971-1.001), against a control (a second copy of A, A2) whose median is **1.0006**; client statements/s B-off / A has a median of 1.005 (0.988-1.018) against a control spread of 0.961-1.021. 26 of 39 cells lie outside the control's per-cell range, **every one of them on the cheaper side**; no cell is above 1.001. No regression; the 1.6 % saving is real against the control and **its cause was not investigated** (it is on btree point reads, which the heap refactor does not touch, so it is not that refactor).
2. **With the driver on, the dead entries plateau and the point read stays near a clean index's cost.** After 120 s an index held **57,515-95,974 entries for 50,000 live rows with the driver on, against 2.58-5.31 million with it off** (2,531,600 dead entries after 2,531,600 deletes in D-rec: 1.0000 a delete; 0.9989 a key move in M-rec, the premise's figures to four digits). A point read by the indexed key at the end of the mix cost **1.06-1.08 times a purged index's (one core) and 1.83 times (two cores), against 26.0-29.5 (D-rec), 4.9 (M-rec) and 48-54 (two cores) with it off** (server CPU of 200-statement batches).
3. **The driver's price is 1.5-4.0 % of one core by the CPU delta (up to about 7 % by entries read times the final purge's per-entry cost), a p99.9 of about 143-160 us against 72-113 us, and a throughput change that this run cannot resolve.** Server CPU per statement on / off: D-rec 1.094 and 1.112 (two runs), M-rec 1.125, U-nokey 1.048, two-core D-rec 1.051 and 1.059. Foreground statements/s on / off: D-rec 1.072 and **0.937** (the same cell, the arms in opposite order), M-rec 0.941, U-nokey 1.011, two-core D-rec 1.040 and **0.941**. The run-to-run swing of the ratio (13.5 points in D-rec) is larger than any effect it could show; the CPU per statement is the stable measure.

Open: **(a)** DW-Q1's bar ("a point read by index pays under 5 % at steady state") is not settled by this run: the probe's own resolution is wider than 5 % (the same clean index read 11.5 and 13.9 us at two runs' first samples) and the on-arm samples sit 6-23 % over the clean reference (section 4.3). **(b)** The server never saturated (29-36 % of one core busy; the one Python client bound every cell), so **the driver's cost at a saturated core 0 was not measured**; what is reported is cost per statement and share of a core at 42-48k statements/s a connection.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-11-27 UTC. Part 1 08:56:33-09:00:27 (3 min 54 s); Part 2, eight cells, 09:00:28-09:17:40 (17 min 12 s); D-rec repeat (two cells) 09:17:40-09:22; two-core repeat (two cells) 09:22-09:26:26. Timed measurement 29 min 53 s wall, 08:56:33-09:26:26 |
| Branch / worktree | `worktree-dw-btree-purge-driver`, worktree `dw-btree-purge-driver` |
| Commit measured | B: `0c2092df8`, `git describe --tags` = `v3.4.17-183-g0c2092df8`, committed 2026-11-27 03:48:42 -0500. A: `630ecc359` (`v3.4.17-178-g630ecc359`), committed 2026-11-26 20:12:53 -0500. Tree clean at the start; `tools/dw_close.py` and `tools/dw_close_report.py` are the new files |
| Binary provenance | **B**: `build-release/kds_server`, Release, mtime 2026-11-27 03:49:15 -0500, 33 s after `0c2092df8` was committed, so HEAD's engine. Run from a copy, `/home/ukjin/bench-runs/dw-close/bin/kds_B`, sha256 `3f22bf30d82c62039584aabedd694ad58d7500c0dfbbac4ebf7b9334eb1adf9d` (the source's hash matches). **A**: the copy the premise run measured, `/home/ukjin/bench-runs/dw-s0/bin/kds_server`, sha256 `7d27de1c82964aa36c0d42215af5f033dabac6fa4647e5b30f6bc581b5d2322f` (verified equal to the premise file's). It is `630ecc359`'s source (the premise file's provenance row: built 02:07:04 -0500 on 2026-11-27, 5 h 54 min after that commit; the copy's own mtime is its copy time, 02:09:01). Run from two further copies, `kds_A` and `kds_A2`, both carrying that hash (A2 is the control: one engine, a second path). Every server of every cell started from its arm's copy. `git diff --stat 630ecc359 HEAD -- src include`: 11 files, 463 insertions, 58 deletions |
| Device | data files, logs and the three binaries' copies under `/home/ukjin/bench-runs/dw-close/` on `/dev/mapper/fedora-root`, xfs (`df -T`), LVM on `/dev/nvme0n1p3`, Sandisk Optimus 5100 NVMe. `/tmp` is tmpfs on this host and held scripts only |
| Build type | Release |
| Server config | `durability = relaxed`; `cores = 1` except the two-core cells (`cores = 2`); `buffer_pool_frames = 262144` (2 GiB; the largest cell directory, data file and log together, is 613 MB, so every working set stayed resident); `log_level = warn`; every other key default; B-on adds `index_purge_driver = on` and nothing else. A fresh server and data file per arm and cell (Part 1: per arm and round). Ports 15601-15709 (the unrelated `kds_server` of another project on this host holds 15470 and was left alone) |
| Host | Intel Core Ultra 7 270K Plus, 24 CPUs, P-cores 0-7, E-cores 8-23 (hybrid), `powersave` governor, Linux 7.3.4-300.fc45. **Pinned**: servers to P-cores (`cores = 1`: CPUs 0-1; `cores = 2`: CPUs 0-3), the Part 1 driver to E-core 8, Part 2's writers to E-cores 9 and 10 and its orchestrator/sampler to E-core 12 |
| Quiet gate | `pgrep -x cc1plus` and `pgrep -f "^(cmake --build\|ctest)"` read 0 before the run and before every arm and cell (`quiet_gate`, `competing = 0` in every recorded `host`); load average 0.55-2.3. **Hold: 0 s.** The driver was written and smoke-tested (two 8-14 s cells on throw-away data, outside the timed run) before the gate read |
| Driver | `tools/dw_close.py`, tables by `tools/dw_close_report.py`; they import `tools/kwp.py`, `tools/bn_census.py` and `tools/dq_s0_premise.py` unmodified. The first Part 2 run used the driver before two additions: `--flip`/`--seq-base` (the repeat runs) and a re-open of the sampler's session on core 0 when `index_purge_*` is absent from its `SHOW META` (the second two-core run; the first two-core on-arm run lost 4 of its 8 samples' counters to it, section 4.4). No measured path changed between them. **Server CPU** is the sum of `/proc/<pid>/task/*/schedstat` over the server's threads, read once no thread is runnable (`settled_cpu_ns`) |
| Suite | Not executed (no engine code changed in this task) |
| Prior number | **Part 1: none** - this run is the A-side baseline of the next. **Part 2: the premise run**, `bench/v3.5.0/results-dw-s0-premise-v3.4.17-178-g630ecc359.md`, whose dead-entry growth and point-read cost the B-off arm reproduces (section 4.1). PostgreSQL floor: not measured for these shapes |

The raw JSON, logs and server configs are archived under `bench/v3.5.0/archive/dw-close-v3.4.17-183-g0c2092df8/` (`run/` Part 1 and Part 2, `run-rep/` the D-rec repeat, `run-rep2/` the two-core repeat); no data file or WAL segment is archived.

## 2. Method

**Part 1** (`tools/dw_close.py part1`): six rounds, each round the three arms A, A2, B-off once in one of the six orders of three (so each arm sat in every position twice). `cores = 1`, `relaxed`. On each arm a BTREE and a HEAP relation of 200, 1,000 and 10,000 rows (`id int64 PRIMARY KEY, k int64, w int64`, `CREATE INDEX ix ON t (k)`, one unique `k` a row; the **size is the row count when the phases start**). On each relation, 2,000 prepared statements per shape, in this order: `sel_pk` (`WHERE id = ?`), `sel_ix` (`WHERE k = ?`), `ins` (2,000 new named ids), `upd_nk` (`SET w`), `upd_ix` (`SET k`, a new unique value), `del` (the 2,000 ids `ins` placed, so the relation is its start size again and nothing needs re-inserting). Server CPU a statement is the **median of 20 batches of 100**; the control is **A2 / A**; the verdict compares B-off / A's median over the six rounds with the control's range. After `del` the server idles 1.5 s and its CPU is read per deleted row (the heap's slot release runs on core 0's tick after the statements). The statements-per-second column is `statements / sum(client latency)` of the phase, the median over rounds, and is not separately controlled here.

**Part 2** (`tools/dw_close.py part2`): one relation `x (id int64 PRIMARY KEY, k int64, w int64) BTREE` with `CREATE INDEX ix ON x (k)`, **n = 50,000 live rows** (the premise's size; the 200/1,000/10,000 sweep was cut, section 6), a fixed **120 s** mix (180 s was suggested; cut for the budget), each arm on its own fresh server.

| cell | statement mix (prepared) | `k` | cores / writers | what it tests |
|---|---|---|---|---|
| D-rec | per iteration `INSERT` a new id, `DELETE` the oldest live id | uniform in 5,000 values (10 live a value) | 1 / 1 | delete-heavy, recurring keys |
| M-rec | `UPDATE x SET k = ? WHERE id = ?`, random id | uniform in 50,000 values | 1 / 1 | key-moving updates, recurring keys |
| U-nokey | `UPDATE x SET w = ? WHERE id = ?` | initial uniform in 50,000, never changed | 1 / 1 | no entry is ever left: every pass is fruitless |
| D-rec-2c | D-rec, ids split between the writers | as D-rec | 2 / 2 (one connection re-opened until it lands on each core) | the driver runs on core 0 only |

The writers are subprocesses; **every 20 s the orchestrator pauses them** (a file, acknowledged), reads `sys.indexes` and `SHOW META`, and times 3 x 200 prepared `SELECT * FROM x WHERE k = ?` over live keys (the keys read from a window of 600 live rows beginning at a random id), then releases them. Throughput counts a writer's active time (pauses excluded; they are 0.03-0.75 s a writer); server CPU per statement is the schedstat delta of the whole mix less the CPU spent inside the pauses, over all writers' statements. At the end, the same probe, then `PURGE INDEX ix` (reply, wall), then the probe again: **the purged index is the clean reference**. Each writer records every statement's client latency (p0 to max, counts over 1, 5 and 50 ms, the 20 largest, 10 s throughput buckets). Every statement's affected-row count is checked (0 wrong in every cell) and refusals counted by code (0 in every cell).

**Order**: off, on for D-rec; on, off for M-rec; off, on for U-nokey; on, off for D-rec-2c; then the repeats D-rec on, off and D-rec-2c off, on. The arms alternate in time, never concurrent. The driver's tick is the engine's own (1 s offers, 50 ms slices of 16 leaves); `SHOW META`'s `index_purge` section is read from a core-0 session.

## 3. Part 1: the change with the driver off costs nothing

**B-off's server CPU per statement is 0.984 of A's (median over the 39 cells; per-cell medians 0.971-1.001), the control's 1.0006, and the heap release's tail is inside its control.** Per-round totals over the 36 statement cells: B-off / A 0.9843, 0.9891, 0.9845, 0.9880, 0.9878, 0.9804 (control A2 / A 1.0005, 0.9998, 1.0064, 1.0049, 1.0035, 0.9963); B-off sat in position 0, 1 and 2 of the round twice each and was below 0.99 in each, so the saving is not an order effect. The statement-cell absolute costs are 5.85-8.19 us a statement; a 1.6 % difference is 0.1 us, which the batch median resolves (control spread per cell typically +-1 to +-3 %, the widest 0.97-1.07). A column's "beyond control" is "the median ratio lies outside the control's min-max over the six rounds": it flags 26 cells, all with a ratio below 1; it does not mean a regression.

The rows are not scale-dependent: every shape costs the same at 200, 1,000 and 10,000 rows to within 1 % (a point lookup, an insert, an update by key do not scale with rows; `ins`/`del` run on relations that start at that size).

Heap release tail (`HEAP`, the last row of each size): the server's CPU over 1.5 s of idleness after deleting 2,000 rows, per deleted row, 13.2-14.6 us for A and B-off alike; the control's range is **0.87-1.23**, so B-off / A of 0.983, 0.994, 0.993 is inside it. This is the phase DW refactored (`heap_release.cpp` -45/+) and the data show no cost.

**Table 1: Part 1, A against B-off, the median over six rounds of 20 batches of 100 statements.** `stmts/s` is derived: statements over the sum of client latency (`QPS = 1,000,000 / mean us`), not a separately controlled column. The cpu columns are server CPU per statement in us (schedstat), the unit being the point of the cell and the ratio columns being the comparison. The ratio's bracket is the min-max of the six per-round ratios; "beyond control" means the B-off median lies outside the control's bracket.

| relation | rows | shape | A stmts/s (client) | B-off stmts/s | A cpu us/stmt | B-off cpu us/stmt | B-off / A cpu, median [min-max over rounds] | control A2 / A cpu, median [min-max] | B-off beyond control |
|---|---|---|---|---|---|---|---|---|---|
| BTREE | 200 | sel_pk | 35057 | 34917 | 6.12 | 6.00 | 0.981 [0.975-0.994] | 0.996 [0.982-1.005] | YES |
| BTREE | 200 | sel_ix | 40322 | 40486 | 7.30 | 7.17 | 0.980 [0.973-0.993] | 1.005 [0.990-1.024] | YES |
| BTREE | 200 | ins | 52214 | 52029 | 6.36 | 6.33 | 0.989 [0.983-1.001] | 0.998 [0.992-1.016] | YES |
| BTREE | 200 | upd_nk | 52328 | 52354 | 6.40 | 6.32 | 0.990 [0.974-1.001] | 1.003 [0.998-1.013] | YES |
| BTREE | 200 | upd_ix | 49880 | 49518 | 6.86 | 6.83 | 0.992 [0.982-0.999] | 1.004 [0.987-1.013] | no |
| BTREE | 200 | del | 54493 | 54019 | 5.92 | 5.88 | 0.992 [0.978-0.999] | 1.005 [0.985-1.011] | no |
| BTREE | 1000 | sel_pk | 34989 | 34496 | 6.13 | 6.05 | 0.981 [0.978-0.997] | 0.997 [0.989-1.011] | YES |
| BTREE | 1000 | sel_ix | 38787 | 39320 | 7.34 | 7.19 | 0.979 [0.970-0.991] | 1.002 [0.991-1.022] | YES |
| BTREE | 1000 | ins | 51673 | 52222 | 6.35 | 6.30 | 0.991 [0.979-1.005] | 1.009 [0.995-1.033] | YES |
| BTREE | 1000 | upd_nk | 51996 | 52340 | 6.39 | 6.31 | 0.989 [0.969-1.003] | 1.001 [0.993-1.015] | YES |
| BTREE | 1000 | upd_ix | 49867 | 49980 | 6.83 | 6.82 | 1.001 [0.983-1.006] | 1.005 [0.989-1.015] | no |
| BTREE | 1000 | del | 53960 | 53736 | 5.93 | 5.90 | 0.998 [0.975-1.006] | 1.002 [0.989-1.029] | no |
| BTREE | 10000 | sel_pk | 34714 | 34918 | 6.16 | 6.06 | 0.983 [0.969-0.993] | 1.002 [0.993-1.010] | YES |
| BTREE | 10000 | sel_ix | 39769 | 40110 | 7.31 | 7.16 | 0.983 [0.967-0.991] | 1.003 [0.996-1.022] | YES |
| BTREE | 10000 | ins | 51071 | 51928 | 6.41 | 6.36 | 0.996 [0.974-1.004] | 1.002 [0.987-1.011] | no |
| BTREE | 10000 | upd_nk | 52110 | 52453 | 6.33 | 6.30 | 0.987 [0.974-1.000] | 1.004 [0.985-1.033] | no |
| BTREE | 10000 | upd_ix | 50068 | 50346 | 6.82 | 6.72 | 0.984 [0.980-1.003] | 0.998 [0.989-1.019] | YES |
| BTREE | 10000 | del | 53773 | 53476 | 5.93 | 5.85 | 0.989 [0.985-0.994] | 1.004 [0.989-1.017] | YES |
| HEAP | 200 | sel_pk | 34367 | 33648 | 6.90 | 6.77 | 0.979 [0.971-0.987] | 0.999 [0.992-1.014] | YES |
| HEAP | 200 | sel_ix | 40063 | 40428 | 7.15 | 6.97 | 0.971 [0.956-0.992] | 0.989 [0.983-1.009] | YES |
| HEAP | 200 | ins | 47713 | 47727 | 8.14 | 8.10 | 0.991 [0.987-0.999] | 1.000 [0.991-1.033] | no |
| HEAP | 200 | upd_nk | 50687 | 50868 | 6.92 | 6.79 | 0.980 [0.978-0.986] | 0.998 [0.988-1.014] | YES |
| HEAP | 200 | upd_ix | 49037 | 48702 | 7.36 | 7.27 | 0.987 [0.976-0.998] | 0.998 [0.990-1.002] | YES |
| HEAP | 200 | del | 52786 | 52818 | 6.49 | 6.43 | 0.987 [0.969-0.998] | 1.000 [0.968-1.008] | no |
| HEAP | 200 | heap release tail (cpu us per deleted row, idle 1.5 s) | - | - | 13.49 | 13.17 | 0.983 [0.952-1.074] | 1.050 [0.876-1.229] | no |
| HEAP | 1000 | sel_pk | 34559 | 33709 | 6.77 | 6.71 | 0.982 [0.970-1.004] | 0.990 [0.986-1.009] | YES |
| HEAP | 1000 | sel_ix | 40430 | 40761 | 7.04 | 6.92 | 0.975 [0.969-0.994] | 0.995 [0.992-1.019] | YES |
| HEAP | 1000 | ins | 47434 | 47698 | 8.10 | 8.07 | 0.998 [0.985-1.004] | 1.008 [0.988-1.035] | no |
| HEAP | 1000 | upd_nk | 50714 | 51012 | 6.96 | 6.81 | 0.977 [0.961-0.992] | 1.000 [0.990-1.011] | YES |
| HEAP | 1000 | upd_ix | 48545 | 48866 | 7.38 | 7.23 | 0.979 [0.969-0.998] | 0.996 [0.989-1.008] | YES |
| HEAP | 1000 | del | 52908 | 53419 | 6.46 | 6.38 | 0.984 [0.976-1.003] | 0.997 [0.987-1.012] | YES |
| HEAP | 1000 | heap release tail (cpu us per deleted row, idle 1.5 s) | - | - | 14.62 | 14.40 | 0.994 [0.872-1.074] | 0.999 [0.914-1.116] | no |
| HEAP | 10000 | sel_pk | 33576 | 33017 | 6.79 | 6.71 | 0.984 [0.981-1.026] | 1.000 [0.996-1.015] | YES |
| HEAP | 10000 | sel_ix | 40392 | 40052 | 7.08 | 6.92 | 0.981 [0.951-0.985] | 1.001 [0.982-1.007] | YES |
| HEAP | 10000 | ins | 47286 | 47129 | 8.19 | 8.15 | 0.995 [0.986-1.005] | 1.018 [0.989-1.068] | no |
| HEAP | 10000 | upd_nk | 50674 | 50920 | 6.93 | 6.78 | 0.980 [0.972-0.990] | 1.001 [0.999-1.015] | YES |
| HEAP | 10000 | upd_ix | 48508 | 48542 | 7.41 | 7.28 | 0.985 [0.964-0.991] | 0.998 [0.991-1.015] | YES |
| HEAP | 10000 | del | 52978 | 53342 | 6.46 | 6.35 | 0.983 [0.971-0.997] | 1.001 [0.984-1.018] | YES |
| HEAP | 10000 | heap release tail (cpu us per deleted row, idle 1.5 s) | - | - | 13.50 | 13.43 | 0.993 [0.954-1.044] | 0.996 [0.913-1.078] | no |

cells: 39; B-off/A median over cells: 0.9843; A2/A median over cells: 1.0006; cells whose median ratio lies outside the control's range: 26


## 4. Part 2: what the driver costs the foreground and what it buys

### 4.1 With the key absent, the index grows as the premise measured

**B-off reproduces DW-S0's growth and read cost with one relation and one client.** The dead entries equal the deletes exactly in D-rec (2,531,600 after 2,531,600; 2,820,000 after 2,820,000 in the repeat) and 0.9989 a key move in M-rec (5,257,938 after 5,263,600), the premise's figures. The point read by the indexed key cost, at the end of the mix, 355.8 us (D-rec; repeat 400.6) against 13.7 (13.6) us for the purged index, **26.0 (29.5) times** (premise 32.5); M-rec 37.2 against 7.5 us, **4.94 times** (premise 4.9); the two-core cell 916 and 1,053 against 19.1 and 19.5 us, **48 and 54 times**. Per stale entry in the value's run: 0.68-0.69 us (D-rec, 506 and 564 stale a value; premise 0.739), 0.283 us (M-rec, 105 a value; premise 0.286), 0.94 us (two cores, 956 a value; not explained here). The off arm is the premise's engine, so the on-arm differences below are the driver's.

### 4.2 With it on, the dead entries plateau

**The index held 57.5-76.9 thousand entries (D-rec), 64.2-73.2 thousand (M-rec) and 70.4-99.8 thousand (two cores) at every sample after the first, with 50,000 live rows; with the driver off at the same instants it held 0.50-2.87 million, 0.96-5.31 million and 0.88-5.11 million and grew linearly.** U-nokey held exactly 50,000 entries in both arms at every sample: no `UPDATE` of a non-indexed column leaves an entry, so the driver has nothing to remove (section 4.5). The point read's server CPU, sampled over the mix (series tables below), stays at 11.5-14.2 us (D-rec; the purged index 11.4) and 7.8-9.3 us (M-rec; purged 8.0) with the driver on, and grows by about 57 us per 20 s (D-rec) or 4.9 us per 20 s (M-rec) with it off.

| cell | live rows | entries, driver on, at the 20 s samples through the end (min-max) | entries, driver off, at the end | on / clean read cost at the end | off / clean read cost at the end |
|---|---|---|---|---|---|
| D-rec, 2 runs | 50,000 | 57,515-63,814; 57,895-76,931 | 2,581,600; 2,870,000 | 1.08; 1.07 | 26.0; 29.5 |
| M-rec | 50,000 | 64,164-73,203 | 5,307,938 | 1.06 | 4.94 |
| U-nokey | 50,000 | 50,000 | 50,000 | 1.01 | 1.01 |
| D-rec, two cores, 2 runs | 50,000 | 70,493-96,655; 70,430-99,821 | 4,830,200; 5,107,000 | 1.82; 1.83 | 48.0; 54.1 |

What a pass does, from `SHOW META`'s counters at the end of each on-arm cell:

| cell | passes in 120 s | entries read | entries read a pass | entries removed | read per removed | removed / deletes or moves | rows skipped |
|---|---|---|---|---|---|---|---|
| D-rec | 120; 113 (repeat) | 8,747,752; 8,324,535 | 72,898; 73,668 | 2,718,085; 2,644,715 | 3.2; 3.1 | 0.997 (of 2,725,600 deletes); 0.997 (of 2,653,200) | 0; 0 |
| M-rec | 120 | 10,972,814 | 91,440 | 4,939,504 | 2.2 | 0.997 (of 4,953,800 moves) | 0 |
| U-nokey | 61 | 3,050,000 | 50,000 | **0** | - | - | 0 |
| D-rec, two cores (second run; the first lost counters at 4 of 8 samples, section 4.4) | 83 | 8,948,593 | 107,814 | 4,768,226 | 1.9 | 0.990 (of 4,814,200 deletes) | 4 |

D-rec's and M-rec's one pass a second matches the engine's 1 s offer tick: 120 passes in 120 s (113 in the repeat, whose pass rate slowed from 74 passes at 80 s).

### 4.3 The foreground cost

**The driver costs 1.5-4.0 % of one core by CPU per statement, shows in the latency tail as 1-5 ms events about 16 a pass, and moves foreground statements/s by an amount this run cannot separate from its own noise.** Server CPU per statement is the stable measure (it does not depend on the stalls below); the share of a core is that difference times the on-arm's statement rate. The second estimate multiplies the entries read a second by the per-entry cost of the **off arm's** final `PURGE INDEX` in the same mix (the on arm's has nothing to remove): 0.70 us in M-rec, 0.92-0.93 us in D-rec, 1.18-1.20 us in the two-core cells, wall over entries on an idle server; U-nokey takes the one-core range. It is an estimate by analogy, not a measurement of the driver's slices.

| cell | statements/s off / on | on / off | server CPU us a statement off / on | on / off | driver share of one core, from the CPU difference | share, from entries read x final-purge cost |
|---|---|---|---|---|---|---|
| D-rec | 42,457 / 45,532 | 1.072 | 6.831 / 7.476 | 1.094 | 2.9 % | 5.1-6.8 % |
| D-rec (repeat, on first) | 47,317 / 44,318 | 0.937 | 6.770 / 7.527 | 1.112 | 3.4 % | 4.9-6.5 % |
| M-rec | 43,946 / 41,338 | 0.941 | 7.757 / 8.727 | 1.125 | 4.0 % | 6.4-8.5 % |
| U-nokey | 45,489 / 46,001 | 1.011 | 6.599 / 6.914 | 1.048 | 1.45 % | 1.8-2.4 % |
| D-rec, two cores (sum over both) | 80,718 / 83,948 | 1.040 | 8.515 / 8.950 | 1.051 | 3.65 % | not computed (counters incomplete) |
| D-rec, two cores (repeat, off first) | 85,552 / 80,547 | 0.941 | 8.505 / 9.004 | 1.059 | 4.0 % | 5.2-9.0 % at 0.70-1.2 us (a pass rate that halved after 40 s) |

The two estimates disagree by about a factor of two. The CPU difference is a lower bound on the driver if the off arm's per-statement cost rises with its index (a 2.5-5 million-entry tree is wider than a 60,000-entry one) and the true cost is nearer the entries-read estimate; the final purge's per-entry cost is an upper bound if its wall time includes work a slice does not (the off arm's purge works on dead-dense leaves). The data do not choose. Neither is a saturated-core cost: the server was 29-38 % busy per core in every cell (server CPU over active time: D-rec off 29.0 %, on 34.0 %; M-rec 34.1 %, 36.1 %; U-nokey 30.0 %, 31.8 %; two cores 34.4 %, 37.6 % a core), the one Python client bound the rate.

**The throughput ratio's noise is as large as the effect.** The two D-rec runs, identical but for the arms' order, read 1.072 and 0.937; the two two-core runs 1.040 and 0.941. The 10 s buckets (tables below) show why: bucket counts in the stalled runs fall to 293-340 thousand statements from a steady 430-475 thousand, and **statements over 0.29 s appear in 7 of the 12 cells, in either arm** (off: D-rec 1.2 s, U-nokey 0.52 s, two-core 1.13 s and 0.48 s; on: two-core 0.49 s and 0.51 s, D-rec repeat 0.41 s) with the same size in both writers of a two-core cell (1,129.9 and 1,129.4 ms in one run: a server-wide stall, not one session's). The stalls are not attributed here (a checkpoint, the data-file device or the host are candidates; **unverified**, a check would be `sys.checkpoints` against the time of the dips, not read). **Outside the stalled runs the picture is steady**: M-rec, whose buckets are smooth in both arms, reads on / off 0.941 across 12 buckets (on 401-416 thousand a 10 s bucket, off 422-446 thousand); the D-rec repeat's off arm, clean, is 466-475 thousand a bucket and its on arm 407-453 thousand, 0.937. **A 5-6 % throughput cost at a client-bound 40-47 thousand statements/s is the reading of the two clean pairs**; U-nokey (1.011) and the stalled pairs say it is not larger than their noise.

| cell | p99 us off / on | p99.9 us off / on | max ms off / on | statements over 1 ms off / on | over 5 ms off / on |
|---|---|---|---|---|---|
| D-rec | 29.5 / 25.5 | 113.1 / 143.3 | 1,204.7 / 72.4 | 230 / 1,946 | 150 / 7 |
| D-rec (repeat) | 27.4 / 25.8 | 72.4 / 143.1 | 65.8 / 412.9 | 82 / 1,964 | 15 / 73 |
| M-rec | 27.1 / 24.6 | 113.3 / 160.1 | 67.2 / 70.1 | 46 / 2,168 | 15 / 13 |
| U-nokey | 23.1 / 22.9 | 58.5 / 60.6 | 521.4 / 22.5 | 103 / 578 | 51 / 7 |
| D-rec, two cores, writers 0 / 1 | 42.5, 40.8 / 43.1, 34.5 | 152.7, 149.6 / 175.8, 82.4 | 1,129.9, 1,129.4 / 489.8, 436.4 | 324, 337 / 2,011, 595 | 140, 138 / 504, 138 |
| D-rec, two cores (repeat), writers 0 / 1 | 41.0, 41.8 / 40.7, 39.2 | 156.6, 129.5 / 158.9, 148.0 | 422.5, 482.5 / 512.2, 505.1 | 392, 288 / 1,358, 1,361 | 64, 50 / 272, 238 |

(The full p0-p99.9-max tables are in the generated section below.) **The ones that follow a slice are the 1-5 ms events**: on the on arms of the one-core cells there are 1,946, 1,964 and 2,168 statements over 1 ms, 1,716-2,122 more than off, **16.2-17.7 a pass** (1,964 / 120 passes = 16.4), which is the slice count of a pass (16 leaves a slice, about 4.5 thousand entries a slice at 72.9 thousand entries a pass), and U-nokey's extra 475 over 61 passes (7.8 a pass, 50,000 entries a pass). A statement that arrives while a slice runs waits for it; the slice is 1-5 ms because only 7-13 statements a run exceed 5 ms with the driver on in the clean cells. **The p99.9 does not belong to the slices**: 0.036 % of D-rec's statements are over 1 ms, less than the 0.1 % the percentile reads, yet the on-arm p99.9 is 143 us in both runs against 113 and 72 off, and in M-rec 160 against 113; U-nokey (0.009 % of statements in slices) shows none (60.6 against 58.5), and the two-core cells none that is outside their repeat's spread. The p99.9's source in the one-core cells is not identified here. **The longest statement attributable to a slice cannot be read from the max**: the on-arm clean cells' maxima are 72.4 ms (D-rec), 70.1 ms (M-rec) and 22.5 ms (U-nokey), but M-rec's off arm has 58.0 and 67.2 ms events too (an off arm has no slice), so a 60-70 ms event is not a slice's. The five largest statements of the on arms are 14.3, 21.2, 22.2, 45.6 and 72.4 ms (D-rec), 16.2, 32.7, 39.7, 66.3 and 70.1 ms (M-rec) and 9.5, 10.4, 10.8, 19.2 and 22.5 ms (U-nokey); **a slice of 16 leaves costs 1-5 ms by the counts above, and no statement over 20 ms can be called a slice's** (the driverless M-rec arm has them). The p99 is **lower** with the driver on in the three one-core churn cells (25.5 / 25.8 against 29.5 / 27.4; 24.6 against 27.1; U-nokey 22.9 against 23.1, equal): a bloated index costs the writes in the body of the distribution.

### 4.4 Two cores

**The driver runs where core 0 runs, and the index still plateaus, at a higher level (70-100 thousand entries, 1.82-1.83 times a clean read).** Writers were placed one on each core at the start (a connection is re-opened until `SHOW META`'s `core` says so). Off, the placer swapped the two sessions in both runs (`0->1` and `1->0` in the table); on, neither moved. The first on-arm run's sampler session was moved off core 0 by the same placer, so its `SHOW META` carried no `index_purge` section at 4 of its 8 samples (`None` in the series table; the counters at the other samples are real: 20 passes at 20 s, 60 at 60 s, 88 at 100 s); the second run re-opened it on core 0 and carried every sample. In the second run the pass count was 20 at 20 s, 40 at 40 s, then 53, 63, 73, 83 (a pass every 2 s after 40 s, against one a second in the one-core cells), while the entry level rose from 70 thousand to 94-100 thousand; **why the pass rate halved with a writer sharing core 0 is not established here** (a slice skipped because the relation is held, `rows_skipped` 4, would not account for it; the cell did not record `queued`).

### 4.5 U-nokey: fruitless passes are not rare

**61 passes ran in 120 s, each reading all 50,000 entries and removing none: 3,050,000 entries read for nothing.** The driver's learned weight did lower the rate: a pure update-of-`w` mix at 46 thousand statements/s crosses the 4,096-update threshold in 0.09 s, so at full weight a pass would be offered at every 1 s tick (120 passes in 120 s, as D-rec's); at the floor weight of one eighth the threshold is 10,000 weighted updates, about 80 thousand updates, 1.7 s, and the data show one pass every 1.97 s (11 passes in the first 20 s, then 10 each 20 s: 0.55/s, 0.5/s). So the weight floor halves the fruitless passes, **it does not make them rare**: at this update rate the passes it bounds are still one every two seconds, each walking the whole index. The cost: +0.315 us a statement (1.048 times), 1.45 % of a core, 475 extra statements over 1 ms, nothing at p99 (23.1 / 22.9 us), p99.9 (58.5 / 60.6 us) or in the point read (1.01 of clean in both arms); throughput 1.011. (The weight and threshold arithmetic is derived from `docs/spec/index.md` §2.3's description, not read from the driver's counters; `index_purge_queued` was 0 at every sample, and the weight itself is not exposed.)

### 4.6 Generated tables, first run (`run/`)

The summary's "order" is the cell's position in the run; "multiple of clean" is the probe's server CPU at the end over the probe after the final `PURGE INDEX`. Probe CPU is the median of three batches of 200 point reads by `k`. Each "series" row is one 20 s sample (`label` mix) or the end.

### summary

| mix | arm | order | stmts/s | on / off | server cpu us/stmt | entries at end | entries after final PURGE | probe cpu us at end (3x200 median) | probe cpu us clean | multiple of clean | passes | entries read | entries removed | rows skipped |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| D-rec | B-off | 00 | 42457 | 1.000 | 6.831 | 2581600 | 58645 | 355.79 | 13.69 | 25.98 | - | - | - | - |
| D-rec | B-on | 01 | 45532 | 1.072 | 7.476 | 57515 | 50259 | 12.36 | 11.44 | 1.08 | 120 | 8747752 | 2718085 | 0 |
| M-rec | B-off | 03 | 43946 | 1.000 | 7.757 | 5307938 | 66354 | 37.23 | 7.54 | 4.94 | - | - | - | - |
| M-rec | B-on | 02 | 41338 | 0.941 | 8.727 | 64164 | 50290 | 8.47 | 7.98 | 1.06 | 120 | 10972814 | 4939504 | 0 |
| U-nokey | B-off | 04 | 45489 | 1.000 | 6.599 | 50000 | 50000 | 8.13 | 8.01 | 1.01 | - | - | - | - |
| U-nokey | B-on | 05 | 46001 | 1.011 | 6.914 | 50000 | 50000 | 8.07 | 8.01 | 1.01 | 61 | 3050000 | 0 | 0 |
| D-rec-2c | B-off | 07 | 80718 | 1.000 | 8.515 | 4830200 | 67719 | 916.18 | 19.10 | 47.97 | - | - | - | - |
| D-rec-2c | B-on | 06 | 83948 | 1.040 | 8.950 | 96655 | 50477 | 24.31 | 13.35 | 1.82 | - | - | - | - |

### latency of the mix statements, client, us

| mix | arm | writer (core) | statements | p0 | p25 | p50 | p95 | p99 | p99.9 | max | over 1 ms | over 5 ms | over 50 ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| D-rec | B-off | 0 (0->0) | 5063200 | 16.7 | 19.4 | 20.2 | 22.4 | 29.5 | 113.1 | 1204688.3 | 230 | 150 | 39 |
| D-rec | B-on | 0 (0->0) | 5451200 | 16.8 | 19.5 | 20.2 | 21.6 | 25.5 | 143.3 | 72418.8 | 1946 | 7 | 1 |
| M-rec | B-off | 0 (0->0) | 5263600 | 18.7 | 21.0 | 21.3 | 23.7 | 27.1 | 113.3 | 67184.8 | 46 | 15 | 2 |
| M-rec | B-on | 0 (0->0) | 4953800 | 18.9 | 21.1 | 21.3 | 22.3 | 24.6 | 160.1 | 70148.9 | 2168 | 13 | 2 |
| U-nokey | B-off | 0 (0->0) | 5451800 | 17.8 | 20.1 | 20.3 | 21.1 | 23.1 | 58.5 | 521368.0 | 103 | 51 | 9 |
| U-nokey | B-on | 0 (0->0) | 5512400 | 17.7 | 20.1 | 20.3 | 21.1 | 22.9 | 60.6 | 22505.4 | 578 | 7 | 0 |
| D-rec-2c | B-off | 0 (0->1) | 4779600 | 16.4 | 19.9 | 21.4 | 26.0 | 42.5 | 152.7 | 1129932.4 | 324 | 140 | 29 |
| D-rec-2c | B-off | 1 (1->0) | 4780800 | 16.6 | 20.0 | 21.4 | 26.1 | 40.8 | 149.6 | 1129410.8 | 337 | 138 | 31 |
| D-rec-2c | B-on | 0 (0->0) | 4802000 | 16.7 | 20.2 | 21.5 | 25.1 | 43.1 | 175.8 | 489834.0 | 2011 | 504 | 12 |
| D-rec-2c | B-on | 1 (1->1) | 5233600 | 16.6 | 19.8 | 21.2 | 24.7 | 34.5 | 82.4 | 436384.7 | 595 | 138 | 12 |

### per-writer throughput in 10 s buckets (statements per bucket)

- D-rec B-off w0: [429698, 451158, 451472, 411634, 393990, 293302, 341738, 416988, 470676, 471240, 477670, 441448]; refused={}; wrong_affected=0
- D-rec B-on w0: [453140, 453456, 453550, 453266, 454286, 453170, 450774, 454766, 454504, 454292, 452228, 440742]; refused={}; wrong_affected=0
- M-rec B-off w0: [446167, 442179, 440485, 439574, 440214, 438699, 438557, 435280, 434861, 433641, 429902, 422408]; refused={}; wrong_affected=0
- M-rec B-on w0: [415105, 415602, 409738, 412624, 411297, 410445, 412089, 411508, 409967, 411514, 411774, 400979]; refused={}; wrong_affected=0
- U-nokey B-off w0: [466065, 466974, 464954, 465652, 452540, 432348, 428024, 449816, 419267, 463498, 461107, 458917]; refused={}; wrong_affected=0
- U-nokey B-on w0: [455906, 457733, 457532, 458315, 458886, 459661, 459095, 457222, 459927, 458090, 458572, 449004]; refused={}; wrong_affected=0
- D-rec-2c B-off w0: [440084, 352658, 303660, 358906, 437894, 441206, 446038, 446316, 303058, 408274, 444702, 375424]; refused={}; wrong_affected=0
- D-rec-2c B-off w1: [454158, 361700, 302242, 366984, 429394, 434232, 437284, 435422, 314016, 421684, 437200, 364682]; refused={}; wrong_affected=0
- D-rec-2c B-on w0: [406012, 403792, 406128, 405952, 404442, 404958, 404698, 377834, 347162, 420996, 409932, 388514]; refused={}; wrong_affected=0
- D-rec-2c B-on w1: [455226, 453070, 455304, 455438, 454292, 454358, 455786, 422592, 332090, 391220, 449680, 432458]; refused={}; wrong_affected=0

### series


#### D-rec

| t (s) | off entries | on entries | off probe cpu us | on probe cpu us | off probe client p50 us | on probe client p50 us | on passes | on entries read | on entries removed |
|---|---|---|---|---|---|---|---|---|---|
| 0 (start) | 50000 | 50000 | 12.57 | 11.51 | 35.7 | 33.7 | 0 | 0 | 0 |
| 20.1 (mix) | 499400 | 60084 | 49.64 | 12.36 | 72.8 | 34.5 | 20 | 1462369 | 457516 |
| 40.2 (mix) | 931400 | 61811 | 105.93 | 12.49 | 131.8 | 34.4 | 40 | 2919659 | 909789 |
| 60.3 (mix) | 1272800 | 63334 | 143.72 | 14.18 | 165.4 | 35.5 | 60 | 4377419 | 1362666 |
| 80.8 (mix) | 1663000 | 63814 | 197.32 | 12.54 | 224.6 | 34.7 | 80 | 5838450 | 1815386 |
| 101.0 (mix) | 2134600 | 62953 | 270.59 | 13.16 | 306.3 | 35.6 | 100 | 7304691 | 2270847 |
| 120 (end) | 2581600 | 57515 | 355.79 | 12.36 | 377.8 | 34.4 | 120 | 8747752 | 2718085 |
| 121 (after-purge) | 58645 | 50259 | 13.69 | 11.44 | 36.1 | 34.1 | 121 | 8805267 | 2725341 |

#### M-rec

| t (s) | off entries | on entries | off probe cpu us | on probe cpu us | off probe client p50 us | on probe client p50 us | on passes | on entries read | on entries removed |
|---|---|---|---|---|---|---|---|---|---|
| 0 (start) | 50000 | 50000 | 8.12 | 9.82 | 27.2 | 27.8 | 0 | 0 | 0 |
| 20.1 (mix) | 964815 | 69050 | 13.19 | 7.80 | 32.0 | 26.6 | 20 | 1843125 | 837925 |
| 40.2 (mix) | 1845480 | 70775 | 18.37 | 7.84 | 37.2 | 26.6 | 40 | 3670167 | 1659185 |
| 60.2 (mix) | 2724506 | 73203 | 23.34 | 8.94 | 41.8 | 26.6 | 60 | 5496259 | 2479538 |
| 80.3 (mix) | 3598172 | 72719 | 27.99 | 9.27 | 46.6 | 26.8 | 80 | 7329267 | 3304193 |
| 100.4 (mix) | 4465966 | 72987 | 32.69 | 9.25 | 51.4 | 26.7 | 100 | 9159144 | 4126104 |
| 120 (end) | 5307938 | 64164 | 37.23 | 8.47 | 55.9 | 27.2 | 120 | 10972814 | 4939504 |
| 121 (after-purge) | 66354 | 50290 | 7.54 | 7.98 | 26.6 | 26.9 | 121 | 11036978 | 4953378 |

#### U-nokey

| t (s) | off entries | on entries | off probe cpu us | on probe cpu us | off probe client p50 us | on probe client p50 us | on passes | on entries read | on entries removed |
|---|---|---|---|---|---|---|---|---|---|
| 0 (start) | 50000 | 50000 | 7.98 | 8.16 | 27.0 | 27.2 | 0 | 0 | 0 |
| 20.1 (mix) | 50000 | 50000 | 7.93 | 8.34 | 26.7 | 26.6 | 11 | 550000 | 0 |
| 40.2 (mix) | 50000 | 50000 | 7.88 | 7.95 | 26.7 | 26.6 | 21 | 1050000 | 0 |
| 60.2 (mix) | 50000 | 50000 | 8.03 | 9.34 | 26.6 | 26.7 | 31 | 1550000 | 0 |
| 80.3 (mix) | 50000 | 50000 | 9.90 | 9.07 | 27.2 | 26.6 | 41 | 2055425 | 0 |
| 100.3 (mix) | 50000 | 50000 | 8.02 | 9.06 | 26.7 | 26.6 | 51 | 2561148 | 0 |
| 120 (end) | 50000 | 50000 | 8.13 | 8.07 | 27.6 | 27.3 | 61 | 3050000 | 0 |
| 121 (after-purge) | 50000 | 50000 | 8.01 | 8.01 | 27.4 | 27.0 | 62 | 3100000 | 0 |

#### D-rec-2c

| t (s) | off entries | on entries | off probe cpu us | on probe cpu us | off probe client p50 us | on probe client p50 us | on passes | on entries read | on entries removed |
|---|---|---|---|---|---|---|---|---|---|
| 0 (start) | 50000 | 50000 | 12.84 | 13.25 | 35.0 | 35.0 | 0 | 0 | 0 |
| 20.1 (mix) | 879200 | 70493 | 109.00 | 18.56 | 133.2 | 37.6 | 20 | 1871539 | 865707 |
| 40.3 (mix) | 1548600 | 73939 | 201.07 | 16.85 | 229.0 | 38.0 | None | None | None |
| 60.5 (mix) | 2421000 | 72830 | 371.08 | 22.00 | 408.0 | 40.2 | 60 | 5609472 | 2585570 |
| 80.8 (mix) | 3304400 | 94723 | 494.66 | 17.65 | 546.5 | 40.1 | None | None | None |
| 101.2 (mix) | 4028600 | 96301 | 624.00 | 16.90 | 692.6 | 39.6 | 88 | 8580143 | 4139699 |
| 120 (end) | 4830200 | 96655 | 916.18 | 24.31 | 957.3 | 42.0 | None | None | None |
| 121 (after-purge) | 67719 | 50477 | 19.10 | 13.35 | 41.4 | 35.2 | None | None | None |

### final PURGE INDEX and top latencies

- 00-D-rec-B-off: {'tag': 'PURGED INDEX name=ix entries=2522955 skipped_rows=0 attempts=541', 'wall_ms': 2398.2}; top-20 latencies per writer (us): [534745.9, 626124.0, 626494.6, 1088573.9, 1204688.3]
- 01-D-rec-B-on: {'tag': 'PURGED INDEX name=ix entries=0 skipped_rows=0 attempts=17', 'wall_ms': 33.4}; top-20 latencies per writer (us): [14314.7, 21240.7, 22169.6, 45641.7, 72418.8]
- 02-M-rec-B-on: {'tag': 'PURGED INDEX name=ix entries=0 skipped_rows=0 attempts=19', 'wall_ms': 33.9}; top-20 latencies per writer (us): [16231.3, 32666.0, 39691.9, 66263.1, 70148.9]
- 03-M-rec-B-off: {'tag': 'PURGED INDEX name=ix entries=5241584 skipped_rows=0 attempts=1023', 'wall_ms': 3732.6}; top-20 latencies per writer (us): [16472.9, 16850.7, 21962.7, 58021.1, 67184.8]
- 04-U-nokey-B-off: {'tag': 'PURGED INDEX name=ix entries=0 skipped_rows=0 attempts=9', 'wall_ms': 31.6}; top-20 latencies per writer (us): [299673.8, 299760.5, 325683.6, 340766.3, 521368.0]
- 05-U-nokey-B-on: {'tag': 'PURGED INDEX name=ix entries=0 skipped_rows=0 attempts=9', 'wall_ms': 32.7}; top-20 latencies per writer (us): [9490.6, 10376.7, 10761.7, 19198.6, 22505.4]
- 06-D-rec-2c-B-on: {'tag': 'PURGED INDEX name=ix entries=0 skipped_rows=0 attempts=30', 'wall_ms': 39.3}; top-20 latencies per writer (us): [312523.7, 312690.9, 350890.0, 396041.1, 489834.0]; [310133.8, 313692.4, 314113.1, 375276.5, 436384.7]
- 07-D-rec-2c-B-off: {'tag': 'PURGED INDEX name=ix entries=4762481 skipped_rows=0 attempts=1108', 'wall_ms': 5709.9}; top-20 latencies per writer (us): [484813.8, 496781.0, 497390.1, 1075006.6, 1129932.4]; [461412.6, 497346.4, 517207.5, 1063818.9, 1129410.8]

### 4.7 Generated tables, the D-rec repeat (`run-rep/`, on first)

### summary

| mix | arm | order | stmts/s | on / off | server cpu us/stmt | entries at end | entries after final PURGE | probe cpu us at end (3x200 median) | probe cpu us clean | multiple of clean | passes | entries read | entries removed | rows skipped |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| D-rec | B-off | 01 | 47317 | 1.000 | 6.770 | 2870000 | 59918 | 400.61 | 13.59 | 29.49 | - | - | - | - |
| D-rec | B-on | 00 | 44318 | 0.937 | 7.527 | 58485 | 50272 | 12.65 | 11.82 | 1.07 | 113 | 8324535 | 2644715 | 0 |

### latency of the mix statements, client, us

| mix | arm | writer (core) | statements | p0 | p25 | p50 | p95 | p99 | p99.9 | max | over 1 ms | over 5 ms | over 50 ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| D-rec | B-off | 0 (0->0) | 5640000 | 16.6 | 19.4 | 20.2 | 22.3 | 27.4 | 72.4 | 65769.5 | 82 | 15 | 2 |
| D-rec | B-on | 0 (0->0) | 5306400 | 16.7 | 19.5 | 20.1 | 21.7 | 25.8 | 143.1 | 412908.3 | 1964 | 73 | 9 |

### per-writer throughput in 10 s buckets (statements per bucket)

- D-rec B-off w0: [475238, 474622, 472530, 470600, 472910, 472724, 472022, 466588, 468874, 468908, 470044, 432248]; refused={}; wrong_affected=0
- D-rec B-on w0: [449904, 452766, 450484, 451106, 452688, 435590, 420090, 407256, 424828, 446992, 450780, 440924]; refused={}; wrong_affected=0

### series


#### D-rec

| t (s) | off entries | on entries | off probe cpu us | on probe cpu us | off probe client p50 us | on probe client p50 us | on passes | on entries read | on entries removed |
|---|---|---|---|---|---|---|---|---|---|
| 0 (start) | 50000 | 50000 | 11.51 | 13.90 | 34.2 | 35.0 | 0 | 0 | 0 |
| 20.1 (mix) | 539200 | 61535 | 52.00 | 12.81 | 74.7 | 35.2 | 20 | 1459781 | 454265 |
| 40.2 (mix) | 1011200 | 63217 | 110.05 | 12.92 | 133.2 | 35.6 | 40 | 2914424 | 903983 |
| 60.3 (mix) | 1484600 | 57895 | 166.69 | 12.31 | 192.4 | 36.1 | 60 | 4368187 | 1352705 |
| 80.5 (mix) | 1954400 | 76931 | 230.36 | 14.91 | 259.7 | 36.6 | 74 | 5476239 | 1748669 |
| 100.7 (mix) | 2423800 | 62835 | 328.51 | 17.08 | 355.2 | 35.6 | 93 | 6885021 | 2199565 |
| 120 (end) | 2870000 | 58485 | 400.61 | 12.65 | 423.7 | 35.3 | 113 | 8324535 | 2644715 |
| 121 (after-purge) | 59918 | 50272 | 13.59 | 11.82 | 36.5 | 35.4 | 114 | 8383020 | 2652928 |

### final PURGE INDEX and top latencies

- 00-D-rec-B-on: {'tag': 'PURGED INDEX name=ix entries=0 skipped_rows=0 attempts=17', 'wall_ms': 35.2}; top-20 latencies per writer (us): [288152.3, 304115.8, 339297.3, 341430.8, 412908.3]
- 01-D-rec-B-off: {'tag': 'PURGED INDEX name=ix entries=2810082 skipped_rows=0 attempts=620', 'wall_ms': 2653.5}; top-20 latencies per writer (us): [32576.1, 43987.6, 47563.2, 54703.0, 65769.5]

### 4.8 Generated tables, the two-core repeat (`run-rep2/`, off first)

### summary

| mix | arm | order | stmts/s | on / off | server cpu us/stmt | entries at end | entries after final PURGE | probe cpu us at end (3x200 median) | probe cpu us clean | multiple of clean | passes | entries read | entries removed | rows skipped |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| D-rec-2c | B-off | 00 | 85552 | 1.000 | 8.505 | 5107000 | 69337 | 1053.49 | 19.47 | 54.10 | - | - | - | - |
| D-rec-2c | B-on | 01 | 80547 | 0.941 | 9.004 | 95974 | 50481 | 23.61 | 12.88 | 1.83 | 83 | 8948593 | 4768226 | 4 |

### latency of the mix statements, client, us

| mix | arm | writer (core) | statements | p0 | p25 | p50 | p95 | p99 | p99.9 | max | over 1 ms | over 5 ms | over 50 ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| D-rec-2c | B-off | 0 (0->1) | 5064000 | 16.9 | 20.1 | 21.6 | 25.8 | 41.0 | 156.6 | 422492.9 | 392 | 64 | 12 |
| D-rec-2c | B-off | 1 (1->0) | 5050000 | 16.8 | 20.3 | 21.7 | 26.0 | 41.8 | 129.5 | 482511.8 | 288 | 50 | 10 |
| D-rec-2c | B-on | 0 (0->0) | 4787200 | 16.6 | 20.2 | 21.6 | 25.4 | 40.7 | 158.9 | 512223.7 | 1358 | 272 | 23 |
| D-rec-2c | B-on | 1 (1->1) | 4841200 | 16.6 | 20.1 | 21.5 | 25.3 | 39.2 | 148.0 | 505105.0 | 1361 | 238 | 20 |

### per-writer throughput in 10 s buckets (statements per bucket)

- D-rec-2c B-off w0: [438812, 432834, 444340, 440214, 441630, 439678, 336942, 404572, 429178, 434692, 437708, 362222]; refused={}; wrong_affected=0
- D-rec-2c B-off w1: [449828, 442034, 435758, 434032, 430390, 431192, 327834, 404830, 439178, 444506, 432538, 356622]; refused={}; wrong_affected=0
- D-rec-2c B-on w0: [403996, 404592, 444184, 442684, 410468, 362000, 386074, 389250, 346648, 375638, 408498, 392042]; refused={}; wrong_affected=0
- D-rec-2c B-on w1: [452028, 449108, 404486, 403274, 378282, 338670, 415802, 423328, 333632, 354796, 441670, 424666]; refused={}; wrong_affected=0

### series


#### D-rec-2c

| t (s) | off entries | on entries | off probe cpu us | on probe cpu us | off probe client p50 us | on probe client p50 us | on passes | on entries read | on entries removed |
|---|---|---|---|---|---|---|---|---|---|
| 0 (start) | 50000 | 50000 | 12.81 | 16.18 | 35.2 | 35.0 | 0 | 0 | 0 |
| 20.1 (mix) | 958400 | 70430 | 113.26 | 16.19 | 136.6 | 37.6 | 20 | 1866635 | 860970 |
| 40.3 (mix) | 1836600 | 73603 | 269.63 | 22.02 | 299.0 | 40.1 | 40 | 3717197 | 1705997 |
| 60.5 (mix) | 2708800 | 93172 | 429.30 | 16.43 | 474.5 | 39.3 | 53 | 5102866 | 2431828 |
| 80.9 (mix) | 3446800 | 84990 | 573.13 | 19.43 | 625.2 | 38.1 | 63 | 6427188 | 3248210 |
| 101.4 (mix) | 4320200 | 99821 | 729.56 | 19.84 | 798.0 | 39.9 | 73 | 7627002 | 3939779 |
| 120 (end) | 5107000 | 95974 | 1053.49 | 23.61 | 1074.7 | 41.0 | 83 | 8948593 | 4768226 |
| 121 (after-purge) | 69337 | 50481 | 19.47 | 12.88 | 41.6 | 35.9 | 84 | 9044567 | 4813719 |

### final PURGE INDEX and top latencies

- 00-D-rec-2c-B-off: {'tag': 'PURGED INDEX name=ix entries=5037663 skipped_rows=0 attempts=1209', 'wall_ms': 6108.7}; top-20 latencies per writer (us): [292907.2, 335649.0, 351828.2, 377732.2, 422492.9]; [330055.1, 342922.8, 347112.4, 393562.9, 482511.8]
- 01-D-rec-2c-B-on: {'tag': 'PURGED INDEX name=ix entries=0 skipped_rows=0 attempts=31', 'wall_ms': 37.9}; top-20 latencies per writer (us): [417781.6, 447764.7, 456117.1, 480466.2, 512223.7]; [403061.9, 416472.2, 438068.6, 465098.3, 505105.0]

## 5. What the run teaches about the engine

1. **The driver's work is proportional to the index, not to the dead entries.** A pass walks the whole index: 72.9 thousand entries a pass in D-rec for 50,000 live rows and about 22 thousand dead ones, 3.1-3.2 entries read for each one removed (2.2 in M-rec, 1.9 in the two-core cell), and 50,000 for none in U-nokey. The 1 s offer tick and the "a fifth of the entries" threshold are what hold the dead entries to 7.5-27 thousand (0.15-0.5 of the live rows, 1.1-1.5 times the index) at about 22 thousand deletes a second (45 thousand statements/s); the data agree with `docs/spec/index.md` §2.3's description and give its first number for what a pass costs: about 16 slices, 1-5 ms each, so a statement meets a slice about 16 times a second at that rate.
2. **The point read's cost with the driver on is a few dead entries a key run.** At 5,000 values and 7.5-27 thousand dead entries a D-rec index holds 1.5-5 stale entries a value; at the measured 0.68 us each that is 1.0-3.7 us over the clean 11.4-11.8 us, and the probe reads +8 % at the end and +4 to +45 % across the samples, 1.09 at the median. In M-rec (one live entry a value, 0.3-0.5 stale) the median is 1.09 too, against a derived +1-2 %. **Whether that meets DW-Q1's 5 % bar is not settled by this run, and the run does not decide it**: the median of the six mix samples reads above 1.05 in D-rec (1.09 in both runs) and M-rec (1.09), the probe's noise outliers reach +24 % on an index that never changed (the U-nokey off arm's 9.90 us against 8.0), and the same clean index read 11.5 and 13.9 us at the first sample of two runs.
3. **Fruitless passes are bounded, not rare** (section 4.5): the weight floor of an eighth halves them at 46 thousand updates a second. By the spec's arithmetic (derived, measured only at 46 thousand a second) a pass is offered once 80,000 updates have accrued (10,000 weighted updates at weight 1/8), so at r updates a second one pass every max(1 s, 80,000 / r) seconds.
4. **The foreground's tail shows the slice and nothing else of the driver.** Over-1 ms statements rise by 16.2-17.7 a pass; the p99 falls (a smaller tree); the p99.9 rises 30-71 us in the one-core churn cells and its source is open; the maxima are dominated by stalls that occur with the driver off as well (section 4.3). **The slice is short enough that it is not what the maxima are.**
5. **Part 1's saving (B-off 1.6 % cheaper than A) is uniform across shapes and kinds** (0.971-1.001 per cell, BTREE and HEAP, any size), which says the difference is not in a path the milestone changed for one shape but across the binary; the CPU is per statement and the statement is mostly the client (p50 20.2 us for a statement that costs the server 6.6-8.7 us). The cause was not looked for.
6. **A wait decomposition does not apply to Part 1 and is partial for Part 2.** `durability = relaxed`: no fsync wait is in any statement. A one-statement round trip's p50 of 20.2 us is the server's 6.6-8.7 us of CPU plus the client's and the socket's remainder, 12-14 us (derived, not separately measured). The lock, conflict and read waits are 0 in these single-relation mixes (0 refusals; `sys.wait_stats` was not read).

## 6. Cuts, and what was not measured

| cut or omission | what was done instead | why |
|---|---|---|
| Part 2 mix 120 s, not 180 s | 120 s | the budget (about one hour); the series plateaus within 20 s (on) and grows linearly (off) |
| Part 2 sweep of 200 / 1,000 / 10,000 rows | n = 50,000 only (the premise's size) | the budget; **rule 9's sweep is not met for Part 2**. Part 1 does sweep all three sizes |
| Part 2 `off, on, on, off` per cell | one run per arm per cell, order alternating across cells; **D-rec and the two-core cell repeated in the opposite order** | the budget; M-rec and U-nokey have one run per arm, so their ratios carry the noise D-rec's two runs show (+-7 points of throughput) |
| three twins as in the premise | one relation | simpler and the premise's own suggestion; the twin control is therefore absent; the noise floor of Part 2 is the D-rec / two-core repeat pairs |
| Part 1 with the driver on | not measured | the task asked B-off against A; **the per-write `Offer` cost the driver adds on the write path (`command_dispatcher.cpp`, DW-S1) is therefore in Part 2's CPU difference and cannot be told from the slices' cost** |
| a saturated server | not reached: the one Python client bound every cell, the server 29-38 % busy a core | rule 4b is not met on the driver's cost at saturation. To reach it, more writer connections on `cores = 1` would be the next run |
| a HEAP relation in Part 2 | not run | the work order's heap case (DW-S2's moved-key entry on a heap) is covered by no timed cell here; Part 1's heap cells have the driver off |
| `durability = strict`, `cores > 2`, `SET PRIORITY` levels | not run | the budget |
| DW-R5: a long-lived old view held across a run (a pass that is fruitless because of a reader) | not run | the budget; U-nokey is the only fruitless-pass cell and its fruitlessness is the workload's, not a reader's |
| the stall's cause (7 of 12 cells, both arms) | not investigated | `sys.checkpoints` against the dips' times was not read |
| the second two-core on-arm run's `queued` counter and the pass rate halving | not recorded / not explained | the samples carry `index_purge_queued`, which was 0 at every sample read; no cause was looked for |
| PostgreSQL floor | not measured for these shapes | rule 4: a missing twin is stated, not a hole |
| the suite | not executed | no engine code changed in this task |

**Anything that looked wrong:** the first two-core on-arm run's sampler session was moved off core 0 and read no `index_purge` section at 4 of its 8 samples (driver fixed for the second run; the counters in that run's series are real where present, absent elsewhere, and the cell's final counters are the second run's); the 0.3-1.2 s stalls (section 4.3); the two-core pass rate falling to one in two seconds after 40 s. **A pass count of zero with the key on did not occur** (113-120 passes in the churn cells, 61 in U-nokey, 83-88 in two-core); **no server died, no statement was refused, no affected-row count was wrong** (0 in all 12 cells), and **no rows were skipped but 1 (run 1) and 4 (second two-core run) in the two-core cell**.

## 7. Reproduction

```
cp build-release/kds_server ~/bench-runs/dw-close/bin/kds_B          # the closing commit
cp ~/bench-runs/dw-s0/bin/kds_server ~/bench-runs/dw-close/bin/kds_A  # the opening commit's; and kds_A2, a second copy
taskset -c 8  tools/dw_close.py part1 --run-dir ~/bench-runs/dw-close/run --bin-a .../kds_A --bin-a2 .../kds_A2 --bin-b .../kds_B
taskset -c 12 tools/dw_close.py part2 --run-dir ~/bench-runs/dw-close/run --bin-b .../kds_B
taskset -c 12 tools/dw_close.py part2 --run-dir .../run-rep  --bin-b .../kds_B --mixes D-rec --flip
taskset -c 12 tools/dw_close.py part2 --run-dir .../run-rep2 --bin-b .../kds_B --mixes D-rec-2c
tools/dw_close_report.py <run-dir> part1|part2
```

`--mix-seconds 120 --sample-s 20 --n 50000 --stmts 2000 --batch 100 --rounds 6 --probe-stmts 200 --frames 262144` are the defaults. The flags are documented in `tools/dw_close.py`'s docstring (the tree has no `bench/docs/` directory).
