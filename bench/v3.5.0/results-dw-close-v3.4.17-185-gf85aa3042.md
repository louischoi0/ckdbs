# DW close, second measurement: the remedy removes the fruitless passes (0 in 120 s against 61), the write path with the key on costs nothing this run can resolve, and a mix that flips a key between two values shows the driver buying passes for entries it then makes the row re-append

**Thesis.** On worktree `dw-btree-purge-driver` at `f85aa3042` (`v3.4.17-185-gf85aa3042`), four arms ran on one host: **A**, the binary the milestone opened from (`630ecc359`), run as two copies (A and A2, the control); **B-off**, the closing commit with `index_purge_driver` absent; **B-on**, the closing commit with `index_purge_driver = on`. The closing commit counts `deletes` plus `key_updates` (the rows an `UPDATE` wrote with a new entry appended to some index) instead of every `UPDATE` (DW-Q7 (b), `docs/spec/index.md` section 2.3). The previous number for every cell is the first close, `bench/v3.5.0/results-dw-close-v3.4.17-183-g0c2092df8.md`.

> **Correction, 2026-11-27, at the close's review** (`critics-developer`, on `6bad7d706`; no number below is changed): this file reads `index_purge_queued` being 0 at every sample as the counter not counting queued relations. The counter is the relations waiting for a pass and does not include the one whose pass is running (`docs/spec/sys-views.md` §7), so with one relation it reads 0 except in the 50 ms between an offer and the step that starts its pass. It worked as coded; what it cannot show is whether a pass was running.

1. **The remedy works on the cell it was made for.** U-nokey (`UPDATE ... SET w`, no entry left) with the key on ran **0 passes in 120 s (61 in the first close), 0 entries read**; server CPU per statement on / off is **1.006** (6.650 / 6.608 us) against **1.048** in the first close, statements over 1 ms **169 on / 167 off** (578 / 103 in the first close), and the index held 50,000 entries in both arms at every sample. The 0.6 % residue is inside what this run can resolve (Part 1's control spread on one shape is 1-2 %).
2. **The key's write-path cost is not resolvable.** Part 1, eight rounds of the four arms: B-on / B-off server CPU per statement has a median of **0.9993 over 36 cells** (4 of 36 outside the control's per-cell range, A2 / A), and for the two shapes that carry the counter, `UPDATE` of an indexed column and of a non-indexed one, the per-relation medians are 0.994-1.004 and 0.997-1.014, inside the control's 0.996-1.004. The counter is one relaxed add a row that appended an entry; if it costs anything it is under the 1-2 % this harness resolves. No driver pass ran inside any timed block (`index_purge_passes` = 0 after every one of 288 phases, 8 rounds x 36 cells).
3. **B-off is not slower than A, again.** B-off / A server CPU per statement: median over 39 cells **0.9808** (cell medians 0.968-1.002; per-round totals 0.974-0.988) against a control of **1.0007**; the first close read 0.9843 / 1.0006. The saving is not explained here: it is on every shape, largest on point reads (0.964-0.978), which neither the heap release nor the driver touches; it is a property of the binary pair, and nothing in B-off is slower.
4. **The plateau and the cost with the key on are as the first close measured, within its repeat spread.** After 120 s the index held 59,001 (D-rec), 64,366 (M-rec) and 88,856 (two cores) entries with the key on for 50,000 live rows, against 2.82, 5.28 and 4.94 million with it off. Server CPU per statement on / off: D-rec **1.102** (first close 1.094, 1.112), M-rec **1.117** (1.125), two cores **1.045** (1.051, 1.059). The driver's share of one core by that difference: 2.3-3.9 % (3.2 % at two cores, summed).
5. **M-flip, the cell added here, contradicts its own premise and says something about the design.** A row that alternates `k` between two values has, with the driver off, a bounded index (exactly 100,000 entries for 50,000 rows at every sample, 1.06 times a clean index's read cost); the premise was that the driver would buy almost no pass there. It bought **60 passes, read 5.48 million entries and removed 2,466,708**, +7.2 % server CPU per statement (7.546 / 7.037 us), and left the read at 1.06 times clean in both arms. **The driver removes the dead entry the next flip would have revived, so the next flip appends again, and counts it** (0.466 removals a statement). It does not make the index worse, and it buys nothing; section 4.4.

Open: **(a)** the throughput ratio is again not a finding (0.965-1.034 across the five cells; the first close's identical-cell pairs swung 0.937-1.072), and this run made no repeat of a Part 2 cell, so **it has no inside-the-run noise floor for Part 2**; the CPU per statement, whose two first-close repeat pairs agreed to 1.8 and 0.8 points, is the measure used. **(b)** The server never saturated (29-37 % of one core per core in every cell), so the driver's cost at a saturated core 0 is not measured. **(c)** The ~0.3-1 s whole-process stalls of the first close recur in cells of both arms (section 4.3) and are not attributed.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-11-27 UTC. Part 1 (4 rounds) 10:53:33-10:57:01; Part 2 (five mixes, ten cells) 10:57:01-11:18:27 (21 min 26 s); Part 1 repeat (rounds 4-7, the same four rotations) 11:18:41-11:22:10. **Timed measurement 28 min 37 s wall** (10:53:33-11:22:10). `date -u` read 10:52 at the start of the session and 11:23 when the files were assembled |
| Branch / worktree | `worktree-dw-btree-purge-driver`, worktree `dw-btree-purge-driver` |
| Commit measured | B: `f85aa3042`, `git describe --tags` = `v3.4.17-185-gf85aa3042`, committed 2026-11-27 05:51:40 -0500. A: `630ecc359` (`v3.4.17-178-g630ecc359`), committed 2026-11-26 20:12:53 -0500. **Tree at HEAD clean** at the start; the run then modified `tools/dw_close.py` and `tools/dw_close_report.py` (uncommitted: the `M-flip` mix, `--with-on` for Part 1, B-on tables); their as-run copies are in the archive. `git diff --shortstat 630ecc359 HEAD -- src include`: 14 files, 473 insertions, 75 deletions |
| Binary provenance | **B**: `build-release/kds_server`, Release, mtime 2026-11-27 05:52:05 -0500, **25 s after `f85aa3042` was committed**, so HEAD's engine. Run from a copy, `/home/ukjin/bench-runs/dw-close2/bin/B`, sha256 `644ef9b0a8b53e8dce54ab5adf71e6928578f1153aba0bab0ee770baa792ad8d` (the source's hash matches the brief's). **A**: the copy the premise and first-close runs measured, `/home/ukjin/bench-runs/dw-s0/bin/kds_server` (mtime 2026-11-27 02:09:01 -0500, its copy time; built from `630ecc359`), sha256 `7d27de1c82964aa36c0d42215af5f033dabac6fa4647e5b30f6bc581b5d2322f`; run from two copies, `bin/A` and `bin/A2`, both with that hash. Every server of every cell started from its arm's copy; the driver re-hashed each arm's copy at its start (`part1-meta.json`, `part2-meta.json`) |
| Device | data files, logs and binary copies under `/home/ukjin/bench-runs/dw-close2/` on `/dev/mapper/fedora-root`, xfs (`df -T`), not `/tmp` (tmpfs here). The data files and WAL segments are not archived |
| Build type | Release |
| Server config | `durability = relaxed`; `cores = 1` except D-rec-2c (`cores = 2`); `buffer_pool_frames = 262144` (2 GiB, working sets resident); `log_level = warn`; every other key default; B-on adds `index_purge_driver = on` and nothing else. Fresh server and data file per arm and cell (Part 1: per arm and round). Ports 15601-15609 (Part 1), 15701-15719 (Part 2), 15801-15809 (Part 1 repeat); 15470, an unrelated server's, left alone |
| Host | Intel Core Ultra 7 270K Plus, 24 CPUs, P-cores 0-7, E-cores 8-23 (hybrid), Linux 7.3.4-300.fc45. **Pinned**: servers to P-cores (`cores = 1`: CPUs 0-1; `cores = 2`: CPUs 0-3), Part 1's driver to E-core 8, Part 2's writers to E-cores 9 and 10, its orchestrator/sampler unpinned (it was pinned to E-core 12 in the first close; the shell launching it was not `taskset`) |
| Host load | **Not a quiet host by assignment, and the records show a light one at these instants.** `/proc/loadavg` before every cell and arm: 1-minute load 0.28-1.85 (Part 1: 0.28-0.34 at its first arms), 5-minute 0.99-2.81, `competing` (a compile or `ctest` of this session) 0 in every record. A 10 s sampler (`hostlog.txt`, 150 lines) read 1-minute load 0.31-2.10 and 5-minute 0.99-2.81; those include this run's own 1-3 runnable threads. One `top` at 11:19 read 99.2 % idle. The sampler's per-process column is `ps`'s **lifetime-average** `pcpu`, so it cannot show a burst: it listed `llama-server` at a constant 22.1 % (its lifetime mean), and it cannot be used to say what the other tenants did during a cell. A host stall by the other users' processes **cannot be ruled out** and the 0.3-1 s stalls below may be one. The first close's records (load 0.55-2.3) are not different from these ones, so the two runs are not distinguishable by background load |
| Quiet gate | `pgrep -x cc1plus` and `pgrep -f "^(cmake --build\|ctest)"` read 0 before the run and before every arm and cell (`quiet_gate`). **Hold: 0 s.** Nothing was built or tested in this session |
| Driver | `tools/dw_close.py` (as run, with the uncommitted additions above), tables by `tools/dw_close_report.py`; they import `tools/kwp.py`, `tools/bn_census.py` and `tools/dq_s0_premise.py` unmodified. **Server CPU** is the sum of `/proc/<pid>/task/*/schedstat` over the server's threads, read once no thread is runnable. The first close's archive still reports with this report script (`part1`/`part2` accept one directory; `part1` accepts a comma-separated list) |
| Suite | Not executed (no engine code changed in this task; `tools/` only) |
| Prior number | **The first close**, `v3.4.17-183-g0c2092df8`, for every cell that exists there; M-flip and Part 1's B-on arm have none and become the baseline of the next. PostgreSQL floor: not measured for these shapes |

The raw JSON, per-cell server configs, stderr and writer logs, the heartbeat and the host sampler are archived under `bench/v3.5.0/archive/dw-close-v3.4.17-185-gf85aa3042/` (`run/` Part 1 rounds 0-3 and Part 2, `run-rep/` Part 1 rounds 4-7, `dw_close.py.as-run`, `dw_close_report.py.as-run`); no data file or WAL segment is archived.

## 2. Method

**Part 1** (`tools/dw_close.py part1 --with-on --rounds 4`, run twice): per round, the four arms A, B-off, A2, B-on once each in one of the four cyclic rotations (so each arm sat in every position once per four rounds; two runs: eight rounds). `cores = 1`, `relaxed`. On each arm a BTREE and a HEAP relation of 200, 1,000 and 10,000 rows (`id int64 PRIMARY KEY, k int64, w int64`, `CREATE INDEX ix ON t (k)`, one unique `k` a row; **the size is the row count when the phases start**), and on each, 2,000 prepared statements a shape: `sel_pk`, `sel_ix`, `ins` (new ids), `upd_nk` (`SET w`), `upd_ix` (`SET k`, a new unique value), `del` (the ids `ins` placed). Server CPU a statement is the **median of 20 batches of 100**. The B-on arm reads `SHOW META`'s `index_purge` counters after every phase, outside the batches, so a pass bought inside a block would show (none did). The 2,000 key-moving updates plus 2,000 deletes on a relation of 10,000 rows stay under the driver's threshold (4,096 writes **and** one fifth of the entries, `index.md` section 2.3), which is why no pass ran. Controls: **A2 / A** per cell, its min-max over the eight rounds; "beyond control" is a median ratio outside that range. The statements/s columns of the first close's table are not repeated; Part 1 here is stated in CPU per statement (the unit the ratios are made in); client statements/s medians over cells are 1.0073 (B-off / A), 0.9999 (A2 / A), 1.0013 (B-on / B-off).

**Part 2** (`tools/dw_close.py part2`): one relation `x (id int64 PRIMARY KEY, k int64, w int64) BTREE`, `CREATE INDEX ix ON x (k)`, **n = 50,000 live rows**, a fixed **120 s** mix, a fresh server per cell and arm, **one run of each cell and arm** (no repeat).

| cell | statement mix (prepared) | `k` | cores / writers | what it tests |
|---|---|---|---|---|
| U-nokey | `UPDATE x SET w = ? WHERE id = ?` | initial uniform in 50,000, never changed | 1 / 1 | the remedy: nothing moves a key, so no pass is owed |
| D-rec | per iteration `INSERT` a new id, `DELETE` the oldest live id | uniform in 5,000 values | 1 / 1 | delete-heavy, recurring keys |
| M-rec | `UPDATE x SET k = ? WHERE id = ?`, random id | uniform in 50,000 values | 1 / 1 | key-moving updates, recurring keys |
| M-flip | the same statement, on a random live row, alternating that row's `k` between `id` and `id + 1,000,000` (rows load with `k = id`) | two values a row | 1 / 1 | a key that moves and comes back: the premise says no new entry after a row's first two updates |
| D-rec-2c | D-rec, ids split between two writers | as D-rec | 2 / 2 (one connection re-opened until it lands on each core) | the driver runs on core 0 only |

Order, arms alternating in time, never concurrent: U-nokey off, on; D-rec on, off; M-rec off, on; M-flip on, off; D-rec-2c off, on. Writers are subprocesses; every 20 s the orchestrator pauses them, reads `sys.indexes` and `SHOW META` (from a core-0 session) and times 3 x 200 prepared `SELECT * FROM x WHERE k = ?` over live keys; at the end the same probe, then `PURGE INDEX ix`, then the probe again (**the purged index is the clean reference**). Throughput counts a writer's active time; server CPU per statement is the schedstat delta of the whole mix less the CPU inside the pauses, over all statements. Every statement's affected count was checked (0 wrong in all ten cells) and refusals counted (0 in all ten).

**Waits (rule 3).** A statement here is a sum of client time, socket round trip and the server's own CPU; the server's share is measured, the rest is not decomposed. Client p50 is 20-21 us a statement (p0 16-19 us) against 6.6-8.9 us of server CPU a statement, so **the server was busy 29-34 % of the wall in the one-core cells** (server CPU over active time: U-nokey 28.6 % / 29.8 %, D-rec 31.5 / 34.1, M-rec 34.1 / 36.8, M-flip 31.2 / 33.4 off / on; two cores, summed over two cores, 71.0 / 75.5 %) and the remaining ~66 % is the Python client and the loopback round trip. Durability is `relaxed`: no commit wait is measured (`strict` was not run). Lock and conflict waits: no refusal in any cell, and no lock-wait statistic was read. A driver slice is the only wait the mixes add, and it is read as the 1-5 ms events in section 4.3.

**Sizes (rule 9).** Part 1 sweeps 200, 1,000 and 10,000 rows; Part 2 ran n = 50,000 only (cut, section 6). Which shapes scale with rows: none of Part 1's does (a point lookup, an insert and an update by key cost the same at 200, 1,000 and 10,000 rows to within about 1 %).

## 3. Part 1: B-off against A, and B-on against B-off

**B-off's server CPU per statement is 0.9808 of A's (median over 39 cells, per-cell medians 0.968-1.002) against a control of 1.0007; B-on is indistinguishable from B-off (median over 36 statement cells 0.9993).** Per-round totals over the 36 statement cells, eight rounds: A2 / A 0.994-1.005, B-off / A 0.974-0.988 (below the control in every round and in every one of the four positions), B-on / B-off 0.990-1.006. 24 of 39 B-off / A cells lie outside the control's per-cell range, all on the cheaper side, none above 1.002; the first close flagged 26. Absolute costs are 5.8-8.2 us a statement, so the 1.9 % saving is about 0.12 us.

**What the key adds to the write path.** The new counter is touched only by an `UPDATE` row that appended an entry (`upd_ix`) and costs a relaxed add; the other shapes are untouched. B-on / B-off, median [min-max] over eight rounds against the same cell's control A2 / A:

| shape (relation, rows) | B-on / B-off | control A2 / A |
|---|---|---|
| `upd_ix` BTREE 200 / 1,000 / 10,000 | 1.000 [0.980-1.015] / 0.997 [0.974-1.019] / 1.003 [0.974-1.020] | 1.002 / 1.000 / 1.004 |
| `upd_ix` HEAP 200 / 1,000 / 10,000 | 0.994 [0.985-1.013] / 1.004 [0.989-1.055] / 1.004 [0.995-1.020] | 0.996 / 1.003 / 1.001 |
| `upd_nk` BTREE 200 / 1,000 / 10,000 (counts nothing) | 1.001 / 0.997 / 1.001 | 1.001 / 1.001 / 0.997 |
| `upd_nk` HEAP 200 / 1,000 / 10,000 (counts nothing) | 0.999 / 1.001 / 1.014 [0.985-1.020] | 1.001 / 1.002 / 1.004 |

By shape over all relations and rounds, B-on / B-off: sel_pk 0.994, sel_ix 1.001, ins 0.998, upd_nk 1.001, upd_ix 1.002, del 1.003; the control's: 0.999, 1.000, 0.998, 1.001, 1.001, 1.002. **The `upd_ix` difference, 0.1 % beyond the control's, is not a finding**; the resolution of the batch median is about 1-2 % a cell (the control's widest per-cell range is 0.968-1.022). The write path's cost with the key on is therefore **below the harness's resolution, not zero**: a cost of 0.1 us a statement (1.5 %) would not be separated from the control by this design. The heap release's idle tail (the last row of each HEAP size) is inside its control in both comparisons.

**Table 1 and Table 2 (generated, `tools/dw_close_report.py <run> part1`; the two run directories pooled, eight rounds).** The cpu columns are server CPU per statement in us; the ratio columns are the comparison, bracket = min-max of the eight per-round ratios. Table 1's `stmts/s` columns are derived (a phase's statements over the sum of its client latencies, the median over rounds, not separately controlled); Table 2 carries CPU only, its throughput being the 1.0013 median over cells stated in the method.

rounds: 8; arms per round: ['A', 'A2', 'B-off', 'B-on']

| relation | rows | shape | A stmts/s (client) | B-off stmts/s | A cpu us/stmt | B-off cpu us/stmt | B-off / A cpu, median [min-max over rounds] | control A2 / A cpu, median [min-max] | B-off beyond control |
|---|---|---|---|---|---|---|---|---|---|
| BTREE | 200 | sel_pk | 33694 | 33721 | 6.09 | 5.93 | 0.975 [0.955-0.980] | 1.003 [0.977-1.009] | YES |
| BTREE | 200 | sel_ix | 39928 | 40482 | 7.33 | 7.13 | 0.976 [0.959-0.980] | 1.000 [0.983-1.008] | YES |
| BTREE | 200 | ins | 51790 | 52270 | 6.38 | 6.22 | 0.973 [0.953-0.996] | 0.995 [0.979-1.004] | YES |
| BTREE | 200 | upd_nk | 51593 | 52283 | 6.36 | 6.29 | 0.988 [0.975-1.009] | 1.001 [0.996-1.014] | YES |
| BTREE | 200 | upd_ix | 49689 | 49604 | 6.86 | 6.81 | 0.993 [0.980-0.998] | 1.002 [0.991-1.018] | no |
| BTREE | 200 | del | 54256 | 54260 | 5.91 | 5.88 | 0.995 [0.983-1.008] | 0.999 [0.994-1.016] | no |
| BTREE | 1000 | sel_pk | 33407 | 34228 | 6.10 | 5.96 | 0.980 [0.965-0.989] | 0.998 [0.988-1.014] | YES |
| BTREE | 1000 | sel_ix | 38834 | 39267 | 7.34 | 7.09 | 0.968 [0.949-0.990] | 0.998 [0.981-1.009] | YES |
| BTREE | 1000 | ins | 51898 | 51853 | 6.36 | 6.24 | 0.983 [0.927-0.992] | 1.003 [0.945-1.010] | no |
| BTREE | 1000 | upd_nk | 51843 | 51996 | 6.36 | 6.28 | 0.989 [0.980-0.997] | 1.001 [0.989-1.010] | YES |
| BTREE | 1000 | upd_ix | 49719 | 49720 | 6.86 | 6.78 | 0.987 [0.948-1.010] | 1.000 [0.968-1.012] | no |
| BTREE | 1000 | del | 54232 | 54415 | 5.92 | 5.87 | 0.993 [0.982-1.003] | 0.999 [0.991-1.016] | no |
| BTREE | 10000 | sel_pk | 34390 | 34003 | 6.13 | 5.97 | 0.972 [0.963-0.984] | 0.998 [0.986-1.010] | YES |
| BTREE | 10000 | sel_ix | 39854 | 40197 | 7.27 | 7.08 | 0.974 [0.963-0.989] | 1.003 [0.987-1.015] | YES |
| BTREE | 10000 | ins | 51664 | 51882 | 6.37 | 6.31 | 0.991 [0.977-0.998] | 0.999 [0.988-1.012] | no |
| BTREE | 10000 | upd_nk | 52050 | 52286 | 6.37 | 6.30 | 0.989 [0.979-1.011] | 0.997 [0.986-1.010] | no |
| BTREE | 10000 | upd_ix | 49629 | 50020 | 6.82 | 6.77 | 0.993 [0.977-1.012] | 1.004 [0.992-1.022] | no |
| BTREE | 10000 | del | 54478 | 54037 | 5.89 | 5.89 | 1.002 [0.978-1.009] | 1.009 [0.996-1.015] | no |
| HEAP | 200 | sel_pk | 34093 | 34035 | 6.89 | 6.67 | 0.970 [0.953-0.983] | 1.002 [0.984-1.009] | YES |
| HEAP | 200 | sel_ix | 39871 | 40707 | 7.12 | 6.85 | 0.968 [0.952-0.979] | 1.000 [0.967-1.021] | no |
| HEAP | 200 | ins | 47148 | 47710 | 8.17 | 8.00 | 0.981 [0.967-0.989] | 1.002 [0.995-1.011] | YES |
| HEAP | 200 | upd_nk | 50817 | 50571 | 6.88 | 6.78 | 0.988 [0.963-1.012] | 1.001 [0.985-1.024] | no |
| HEAP | 200 | upd_ix | 48797 | 49015 | 7.36 | 7.20 | 0.984 [0.971-0.999] | 0.996 [0.990-1.014] | YES |
| HEAP | 200 | del | 52324 | 52650 | 6.52 | 6.38 | 0.977 [0.972-0.992] | 0.995 [0.975-1.015] | no |
| HEAP | 200 | heap release tail (cpu us per deleted row, idle 1.5 s) | - | - | 13.86 | 12.71 | 0.930 [0.828-1.067] | 0.952 [0.800-1.117] | no |
| HEAP | 1000 | sel_pk | 33132 | 33752 | 6.75 | 6.60 | 0.979 [0.958-0.993] | 1.001 [0.992-1.021] | YES |
| HEAP | 1000 | sel_ix | 40167 | 40581 | 7.06 | 6.83 | 0.972 [0.955-0.981] | 0.997 [0.992-1.012] | YES |
| HEAP | 1000 | ins | 47307 | 47678 | 8.12 | 7.94 | 0.978 [0.969-0.996] | 0.996 [0.985-1.012] | YES |
| HEAP | 1000 | upd_nk | 50458 | 51082 | 6.95 | 6.79 | 0.977 [0.965-0.995] | 1.002 [0.991-1.009] | YES |
| HEAP | 1000 | upd_ix | 48384 | 48574 | 7.33 | 7.19 | 0.981 [0.974-0.991] | 1.003 [0.993-1.014] | YES |
| HEAP | 1000 | del | 52912 | 53103 | 6.47 | 6.38 | 0.987 [0.964-1.014] | 1.001 [0.990-1.015] | YES |
| HEAP | 1000 | heap release tail (cpu us per deleted row, idle 1.5 s) | - | - | 14.27 | 14.19 | 1.007 [0.905-1.172] | 1.015 [0.813-1.233] | no |
| HEAP | 10000 | sel_pk | 33337 | 33313 | 6.79 | 6.58 | 0.970 [0.955-0.986] | 0.995 [0.978-1.009] | YES |
| HEAP | 10000 | sel_ix | 40353 | 40586 | 7.04 | 6.86 | 0.973 [0.965-0.991] | 1.002 [0.992-1.006] | YES |
| HEAP | 10000 | ins | 47001 | 47313 | 8.20 | 8.05 | 0.982 [0.968-0.990] | 0.994 [0.985-1.001] | YES |
| HEAP | 10000 | upd_nk | 50563 | 51094 | 6.91 | 6.72 | 0.975 [0.968-0.991] | 1.004 [0.991-1.023] | YES |
| HEAP | 10000 | upd_ix | 48189 | 48659 | 7.42 | 7.24 | 0.979 [0.967-0.988] | 1.001 [0.993-1.012] | YES |
| HEAP | 10000 | del | 52805 | 53255 | 6.46 | 6.36 | 0.984 [0.970-1.008] | 1.006 [0.994-1.021] | YES |
| HEAP | 10000 | heap release tail (cpu us per deleted row, idle 1.5 s) | - | - | 13.28 | 13.42 | 1.034 [0.951-1.123] | 1.023 [0.834-1.157] | no |

### B-on against B-off (same binary), per-statement server CPU

| relation | rows | shape | B-off cpu us/stmt | B-on cpu us/stmt | B-on / B-off, median [min-max over rounds] | control A2 / A, median [min-max] | B-on beyond control | passes seen after the phase (max over rounds) |
|---|---|---|---|---|---|---|---|---|
| BTREE | 200 | sel_pk | 5.93 | 5.93 | 0.999 [0.988-1.013] | 1.003 [0.977-1.009] | no | 0 |
| BTREE | 200 | sel_ix | 7.13 | 7.13 | 1.001 [0.985-1.018] | 1.000 [0.983-1.008] | no | 0 |
| BTREE | 200 | ins | 6.22 | 6.23 | 0.999 [0.995-1.006] | 0.995 [0.979-1.004] | no | 0 |
| BTREE | 200 | upd_nk | 6.29 | 6.31 | 1.001 [0.986-1.016] | 1.001 [0.996-1.014] | no | 0 |
| BTREE | 200 | upd_ix | 6.81 | 6.83 | 1.000 [0.980-1.015] | 1.002 [0.991-1.018] | no | 0 |
| BTREE | 200 | del | 5.88 | 5.84 | 0.992 [0.981-1.016] | 0.999 [0.994-1.016] | YES | 0 |
| BTREE | 1000 | sel_pk | 5.96 | 5.95 | 0.993 [0.988-1.009] | 0.998 [0.988-1.014] | no | 0 |
| BTREE | 1000 | sel_ix | 7.09 | 7.16 | 1.007 [0.990-1.021] | 0.998 [0.981-1.009] | no | 0 |
| BTREE | 1000 | ins | 6.24 | 6.26 | 1.003 [0.987-1.017] | 1.003 [0.945-1.010] | no | 0 |
| BTREE | 1000 | upd_nk | 6.28 | 6.29 | 0.997 [0.992-1.010] | 1.001 [0.989-1.010] | no | 0 |
| BTREE | 1000 | upd_ix | 6.78 | 6.76 | 0.997 [0.974-1.019] | 1.000 [0.968-1.012] | no | 0 |
| BTREE | 1000 | del | 5.87 | 5.87 | 0.996 [0.978-1.015] | 0.999 [0.991-1.016] | no | 0 |
| BTREE | 10000 | sel_pk | 5.97 | 5.94 | 0.997 [0.991-1.009] | 0.998 [0.986-1.010] | no | 0 |
| BTREE | 10000 | sel_ix | 7.08 | 7.09 | 1.002 [0.990-1.025] | 1.003 [0.987-1.015] | no | 0 |
| BTREE | 10000 | ins | 6.31 | 6.25 | 0.992 [0.978-1.006] | 0.999 [0.988-1.012] | no | 0 |
| BTREE | 10000 | upd_nk | 6.30 | 6.31 | 1.001 [0.987-1.012] | 0.997 [0.986-1.010] | no | 0 |
| BTREE | 10000 | upd_ix | 6.77 | 6.80 | 1.003 [0.974-1.020] | 1.004 [0.992-1.022] | no | 0 |
| BTREE | 10000 | del | 5.89 | 5.90 | 1.006 [0.982-1.018] | 1.009 [0.996-1.015] | no | 0 |
| HEAP | 200 | sel_pk | 6.67 | 6.69 | 0.998 [0.990-1.014] | 1.002 [0.984-1.009] | no | 0 |
| HEAP | 200 | sel_ix | 6.85 | 6.93 | 1.009 [0.976-1.028] | 1.000 [0.967-1.021] | no | 0 |
| HEAP | 200 | ins | 8.00 | 7.97 | 0.999 [0.980-1.013] | 1.002 [0.995-1.011] | no | 0 |
| HEAP | 200 | upd_nk | 6.78 | 6.76 | 0.999 [0.963-1.013] | 1.001 [0.985-1.024] | no | 0 |
| HEAP | 200 | upd_ix | 7.20 | 7.20 | 0.994 [0.985-1.013] | 0.996 [0.990-1.014] | no | 0 |
| HEAP | 200 | del | 6.38 | 6.38 | 1.004 [0.985-1.014] | 0.995 [0.975-1.015] | no | 0 |
| HEAP | 1000 | sel_pk | 6.60 | 6.51 | 0.983 [0.969-1.004] | 1.001 [0.992-1.021] | YES | 0 |
| HEAP | 1000 | sel_ix | 6.83 | 6.79 | 0.989 [0.977-1.018] | 0.997 [0.992-1.012] | YES | 0 |
| HEAP | 1000 | ins | 7.94 | 7.91 | 0.995 [0.991-1.014] | 0.996 [0.985-1.012] | no | 0 |
| HEAP | 1000 | upd_nk | 6.79 | 6.81 | 1.001 [0.986-1.017] | 1.002 [0.991-1.009] | no | 0 |
| HEAP | 1000 | upd_ix | 7.19 | 7.22 | 1.004 [0.989-1.055] | 1.003 [0.993-1.014] | no | 0 |
| HEAP | 1000 | del | 6.38 | 6.41 | 1.002 [0.976-1.025] | 1.001 [0.990-1.015] | no | 0 |
| HEAP | 10000 | sel_pk | 6.58 | 6.50 | 0.989 [0.969-1.003] | 0.995 [0.978-1.009] | no | 0 |
| HEAP | 10000 | sel_ix | 6.86 | 6.80 | 0.992 [0.978-1.006] | 1.002 [0.992-1.006] | YES | 0 |
| HEAP | 10000 | ins | 8.05 | 7.99 | 0.996 [0.978-1.037] | 0.994 [0.985-1.001] | no | 0 |
| HEAP | 10000 | upd_nk | 6.72 | 6.81 | 1.014 [0.985-1.020] | 1.004 [0.991-1.023] | no | 0 |
| HEAP | 10000 | upd_ix | 7.24 | 7.30 | 1.004 [0.995-1.020] | 1.001 [0.993-1.012] | no | 0 |
| HEAP | 10000 | del | 6.36 | 6.41 | 1.009 [0.988-1.019] | 1.006 [0.994-1.021] | no | 0 |

B-on/B-off median over cells: 0.9993; cells outside the control's range: 4 of 36
B-on/B-off median per shape over all relations and rounds: sel_pk 0.994, sel_ix 1.001, ins 0.998, upd_nk 1.001, upd_ix 1.002, del 1.003
control A2/A median per shape over all relations and rounds: sel_pk 0.999, sel_ix 1.000, ins 0.998, upd_nk 1.001, upd_ix 1.001, del 1.002

cells: 39; B-off/A median over cells: 0.9808; A2/A median over cells: 1.0007; cells whose median ratio lies outside the control's range: 24


## 4. Part 2: the mixes, key on against key off

Five cells, one run each, `cores = 1` except the last. The summary (statements/s, server CPU, entries, probe multiple, counters) is the first table; the latency distribution follows in 4.3.

| mix | arm | stmts/s | on / off | server CPU us/stmt | on / off | first close's on / off (CPU) | entries at end | probe at end, multiple of clean | passes | entries read | entries removed | rows skipped |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| U-nokey | off | 43,251 | | 6.608 | | | 50,000 | 1.33 | - | - | - | - |
| U-nokey | **on** | 44,738 | 1.034 | 6.650 | **1.006** | 1.048 | 50,000 | 1.40 | **0** | **0** | 0 | 0 |
| D-rec | off | 46,565 | | 6.757 | | | 2,823,200 | 27.98 | - | - | - | - |
| D-rec | on | 45,754 | 0.983 | 7.447 | 1.102 | 1.094; 1.112 | 59,001 | 1.05 | 120 | 8,760,615 | 2,729,999 | 0 |
| M-rec | off | 43,714 | | 7.809 | | | 5,280,388 | 5.16 | - | - | - | - |
| M-rec | on | 42,188 | 0.965 | 8.725 | 1.117 | 1.125 | 64,366 | 1.21 | 120 | 11,074,989 | 5,041,695 | 0 |
| M-flip | off | 44,392 | | 7.037 | | (none) | 100,000 | 1.06 | - | - | - | - |
| M-flip | on | 44,195 | 0.996 | 7.546 | 1.072 | (none) | 81,634 | 1.06 | 60 | 5,480,007 | 2,466,708 | 0 |
| D-rec-2c (summed over two writers) | off | 82,729 | | 8.582 | | | 4,941,600 | 52.34 | - | - | - | - |
| D-rec-2c | on | 84,263 | 1.019 | 8.965 | 1.045 | 1.051; 1.059 | 88,856 | 1.50 | 69 | 8,507,986 | 4,997,944 | 0 |

(Throughput is statements over the writers' active time; the stmts/s ratio is not a finding, below. "Multiple of clean" is the probe's server CPU at the end over the probe after the final `PURGE INDEX`; **the probe's own floor is wide**: U-nokey, whose index never holds a dead entry, reads 1.33 and 1.40, so an end-sample multiple below about 1.4 is not distinguishable from a clean index. M-rec on reads 1.03-1.06 at its mid-run samples and 1.21 at the end.)

### 4.1 U-nokey: the remedy

**The driver ran zero passes: no pass was owed, none was bought.** `index_purge_passes`, `entries_read`, `entries_removed` and `rows_skipped` were 0 at every one of the eight samples (the first close: 61 passes, 3,050,000 entries read, 0 removed). The cost the first close attributed to the fruitless passes is gone: **server CPU 1.006 (first close 1.048), statements over 1 ms 169 against 167 off (578 / 103), p99.9 63.4 against 64.9 us, probe unchanged**. The remaining 0.6 % is below this run's resolution. This is the arm of the change the milestone's remedy targeted, and its figure is measured here; **the cost is not "removed", it is "undetectable at 1-2 %"**. Throughput on / off reads 1.034, which this run does not call a result (4.3).

### 4.2 The key on holds the index; the key off lets it grow as the premise measured

**With the key on the index held 59,001-63,354 (D-rec), 64,366-73,378 (M-rec) and 71,889-88,856 entries (two cores) at every sample, for 50,000 live rows; with it off it grew linearly to 2.82, 5.28 and 4.94 million.** Dead entries at the end of the off arm equal the deletes (2,773,200 of 2,773,200 deletes in D-rec) and 0.9989 a key move in M-rec (5,230,388 dead entries after 5,236,000 moves), the premise's figures. The probe's server CPU, at the end, with the key off: D-rec 420.3 us against 15.0 clean (**28.0 times**; first close 26.0 and 29.5), M-rec 38.6 against 7.5 (**5.2**; 4.94), two cores 981.9 against 18.8 (**52.3**; 48.0 and 54.1); with it on, 13.9, 9.0 and 22.3 us (1.05, 1.21, 1.50 times clean; the first close's 1.08/1.07, 1.06 and 1.82/1.83, and the probe's floor of about 1.4 above applies to all of these). **The off arm reproduces the first close; the on arm differs only where the probe's own floor does.**

| cell | passes in 120 s | entries read a pass | removed / deletes or moves | rows skipped | first close: passes |
|---|---|---|---|---|---|
| U-nokey | 0 | - | - | 0 | 61 |
| D-rec | 120 | 73,005 | 0.997 | 0 | 120; 113 |
| M-rec | 120 | 92,292 | 0.997 | 0 | 120 |
| M-flip | 60 | 91,333 | 0.466 (of statements) | 0 | none |
| D-rec-2c | 69 | 123,304 | 0.992 | 0 | 83 |

The two-core pass rate again halves (19 passes by 20 s, then 10 every 20 s, against 20 every 20 s in the one-core cells); **why a writer sharing core 0 halves it is still not established**, and this run's counters did not record `queued` usefully (`index_purge_queued` was 0 at every sample in every cell, including the ones that ran 120 passes, so it is not the count of relations queued).

### 4.3 The foreground cost, and what the run cannot separate

**The driver's CPU cost with the key on is 2.3-3.9 % of one core in the churn cells and 0.2 % in U-nokey; statements over 1 ms rise by 1.2-1.9 thousand (one-core), the tail the first close described.** Share of one core = (CPU per statement on - off) x on-arm statements/s: D-rec 3.2 %, M-rec 3.9 %, M-flip 2.25 %, U-nokey 0.19 %, two cores 3.2 % summed (1.6 % a core). The first close read 2.9 / 3.4, 4.0, 1.45 and 3.65 / 4.0 % for the same cells.

| cell | p99 us off / on | p99.9 us off / on | max ms off / on | statements over 1 ms off / on | over 5 ms off / on | over 50 ms off / on |
|---|---|---|---|---|---|---|
| U-nokey | 23.3 / 23.3 | 64.9 / 63.4 | 1,026.5 / 471.1 | 167 / 169 | 113 / 75 | 17 / 19 |
| D-rec | 28.6 / 25.6 | 87.5 / 137.7 | 394.3 / 54.3 | 129 / 1,973 | 53 / 11 | 10 / 1 |
| M-rec | 27.6 / 24.2 | 114.2 / 154.0 | 83.9 / 76.7 | 72 / 2,194 | 11 / 9 | 2 / 1 |
| M-flip | 23.9 / 23.9 | 63.5 / 75.6 | 401.2 / 98.2 | 84 / 1,330 | 37 / 13 | 9 / 1 |
| D-rec-2c, two writers summed (p99 / p99.9 / max of writers 0, 1) | 40.7, 42.3 / 42.2, 35.8 | 140.3, 160.8 / 157.3, 103.0 | 625.7, 555.5 / 464.4, 398.6 | 783 / 2,628 | 223 / 494 | 55 / 27 |

(The p0, p25, p50, p95 tables are generated, section 7: p0 16.3-18.8 us, p25 19.3-21.1, p50 20.1-21.5 and p95 21.1-26.4 in every row, equal between arms to within 1 us.) The statements-over-1-ms excess on the one-core on arms, 1,844 (D-rec), 2,122 (M-rec), 1,246 (M-flip), is 15.4, 17.7 and 20.8 per pass (D-rec 1,973 - 129 over 120 passes; M-flip 1,330 - 84 over 60 passes), the slice-of-16-leaves signature of the first close (16.2-17.7 a pass); M-flip's 20.8 a pass over 91 thousand entries a pass is the same kind. The p99.9 is again higher with the key on in D-rec and M-rec (137.7 against 87.5; 154.0 against 114.2) and again its source is not identified; M-flip's 75.6 against 63.5 is smaller. The p99 is lower with the key on in D-rec and M-rec (25.6 against 28.6; 24.2 against 27.6), the bloated index's cost, as the first close read.

**Throughput on / off, 0.965-1.034, is not a finding.** D-rec 0.983, M-rec 0.965, M-flip 0.996, U-nokey 1.034, two cores 1.019; with no repeated cell this run measured no noise floor for it, and the first close's identical-cell repeats read 1.072 / 0.937 and 1.040 / 0.941. **Stalls again**: statements over 0.29 s occur in the **U-nokey off arm (14 of the 20 largest statements, up to 1.03 s)**, **U-nokey on (8)**, D-rec off (5), M-flip off (5), both two-core arms (22 and 14, the same sizes in the two writers of one cell), and in no D-rec on, M-rec or M-flip on cell. U-nokey off and on have no driver activity and no growing index, so **these stalls are not the driver's and are not an index's**; they are 6 of the 10 cells, 4 of the 5 off arms and 2 of the 5 on arms. Their cause is not attributed here (a checkpoint, the data-file device, the host's other tenants or the single client are candidates; `sys.checkpoints` was not read at the dips). The effect on the throughput ratios is visible in the 10 s buckets (section 7): D-rec off falls from 475 thousand to 367-403 thousand in its last two buckets, U-nokey off dips to 379-423 thousand.

### 4.4 M-flip: the driver counts appends it causes

**M-flip off held exactly 100,000 entries at every sample after the first, as the premise reads: a row that alternates between two values adds two entries and then re-uses them.** The final `PURGE INDEX` of the off arm removed 49,823 entries, leaving 50,177 (the live rows' and 177 more), and the off arm's point read cost 7.46 us against 7.05 clean (1.06; the on arm 1.06). **With the key on, 60 passes read 5,480,007 entries and removed 2,466,708**, 0.466 for each of 5,297,000 statements, and the index ended at 81,634 entries (83,331-85,300 at the samples). The mechanism that fits these numbers: removing the dead entry of a row that is currently at the other value kills the entry the next flip would have revived, so that flip appends a new entry, the write path counts it (`key_updates`), and a later pass removes it. **This is inferred**: `key_updates` is not exposed in `SHOW META`, so the appends were not counted directly (a check would be the on-arm's `sys.indexes` entry count growth per pass, or a counter exposed for the run). The driver therefore **does work in proportion to the statements, not to the dead entries that would otherwise exist** (the premise's "almost no pass" applied to the driver-off engine). Its result: server CPU per statement +7.2 % (7.546 / 7.037), 2.25 % of one core, 1,330 statements over 1 ms against 84, a point read at 1.06 times clean in both arms, i.e. **the work bought no read-side benefit in this shape, because the dead entry it removes costs a read nothing: the index off is bounded at two entries a row**.

*Insight, a design observation rather than a result of this run alone*: the remedy counts exactly the writes that leave an entry (`key_updates`) and so fixes U-nokey, but a count of appends cannot tell an append that adds a dead entry to a tree that will keep growing from one that re-creates an entry the driver itself removed. **A mix with a bounded dead-entry population, in this case two entries a row, is not bounded when the driver runs.** The recommendation and its trade-off are in the report, not here; this file states what the engine does at this commit.

### 4.5 Two cores

**The driver runs where core 0 runs and the index plateaus at a higher level (71.9-88.9 thousand entries, 1.50 times clean; first close 70-100 thousand, 1.82-1.83).** Off, the placer again swapped the writers' cores in the run (`0->1` and `1->0`); with the key on neither moved. The 4,998,000 removals are 0.992 a delete; the skipped-rows counter was 0. The two-core on / off CPU ratio is 1.045 (first close 1.051, 1.059), the share 1.6 % of each core (3.2 % of one).

## 5. What the run teaches about the engine

1. **The remedy fixed what it was made for and moved nothing else.** Same per-pass cost (the on-arm CPU ratios in D-rec, M-rec and the two-core cell sit inside the first close's), and the bug cell's passes fell from 61 to 0.
2. **The write path's price for the key is below this harness's resolution.** Eight rounds, a control, 36 cells: 0.9993 with a control of ~1.000 and a resolution of 1-2 %. That does not show no cost; it shows less than about 0.1 us a statement.
3. **A count of key-moving writes is the wrong price signal when the pass itself creates the next append (M-flip).** `docs/spec/index.md` section 2.3 lists among the writes not counted one "back to a value whose entry is still in the tree, which kills one entry and revives another". That holds while the entry is in the tree; once a pass has removed it the same statement appends and is counted, and 0.466 removals a statement shows the entry is often gone. This is measured behaviour at `f85aa3042`; it **contradicts the expectation in this run's brief** ("the driver should buy almost no pass"), not a spec claim.
4. **The plateau is real and cheap for churn, and the probe's cost with the key on is within its own noise of clean** (1.05, 1.21, 1.50 against off arms of 28.0, 5.2 and 52.3).

## 6. Cuts and what did not run

- **Part 2 ran n = 50,000 only** (rule 9's 200 / 1K / 10K sweep cut for the budget; Part 1 sweeps all three).
- **No repeat of any Part 2 cell**, so no noise floor inside the run for throughput, p99.9 or the probe multiple. The first close's repeats are the only spread cited, and they are of another run of the same engine family.
- **`strict` durability was not run**, and neither was a saturated core 0 (rule 4b): no limit was approached. What bound the cells was the single Python client at 42-47 thousand statements/s a connection, the server at 29-37 % of a core.
- **No cell with an older read view held** (a long `READ ONLY` transaction under which a pass is fruitless on purpose): the `rows_skipped` counter was 0 everywhere, so the skip path was not exercised.
- Part 1 rounds were run as two runs of four rotations (eight rounds); the first close ran six. The Part 1 repeat directory is `run-rep/`.
- The orchestrator of Part 2 was not pinned to E-core 12 as in the first close; the writers (E-cores 9, 10) and servers (P-cores) were.
- The suite was not executed.

## 7. Generated tables

Part 1 is Tables 1 and 2 below (eight rounds pooled); Part 2's generated tables follow.

### Part 2 summary (generated)

| mix | arm | order | stmts/s | on / off | server cpu us/stmt | entries at end | entries after final PURGE | probe cpu us at end (3x200 median) | probe cpu us clean | multiple of clean | passes | entries read | entries removed | rows skipped |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| U-nokey | B-off | 00 | 43251 | 1.000 | 6.608 | 50000 | 50000 | 10.62 | 7.98 | 1.33 | - | - | - | - |
| U-nokey | B-on | 01 | 44738 | 1.034 | 6.650 | 50000 | 50000 | 11.38 | 8.13 | 1.40 | 0 | 0 | 0 | 0 |
| D-rec | B-off | 03 | 46565 | 1.000 | 6.757 | 2823200 | 59850 | 420.33 | 15.02 | 27.98 | - | - | - | - |
| D-rec | B-on | 02 | 45754 | 0.983 | 7.447 | 59001 | 50258 | 13.90 | 13.28 | 1.05 | 120 | 8760615 | 2729999 | 0 |
| M-rec | B-off | 04 | 43714 | 1.000 | 7.809 | 5280388 | 66322 | 38.58 | 7.47 | 5.16 | - | - | - | - |
| M-rec | B-on | 05 | 42188 | 0.965 | 8.725 | 64366 | 50290 | 9.01 | 7.46 | 1.21 | 120 | 11074989 | 5041695 | 0 |
| M-flip | B-off | 07 | 44392 | 1.000 | 7.037 | 100000 | 50177 | 7.46 | 7.05 | 1.06 | - | - | - | - |
| M-flip | B-on | 06 | 44195 | 0.996 | 7.546 | 81634 | 50178 | 7.38 | 6.93 | 1.06 | 60 | 5480007 | 2466708 | 0 |
| D-rec-2c | B-off | 08 | 82729 | 1.000 | 8.582 | 4941600 | 68446 | 981.93 | 18.76 | 52.34 | - | - | - | - |
| D-rec-2c | B-on | 09 | 84263 | 1.019 | 8.965 | 88856 | 50494 | 22.31 | 14.90 | 1.50 | 69 | 8507986 | 4997944 | 0 |

### latency of the mix statements, client, us

| mix | arm | writer (core) | statements | p0 | p25 | p50 | p95 | p99 | p99.9 | max | over 1 ms | over 5 ms | over 50 ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| U-nokey | B-off | 0 (0->0) | 5184000 | 17.5 | 20.0 | 20.2 | 21.1 | 23.3 | 64.9 | 1026470.9 | 167 | 113 | 17 |
| U-nokey | B-on | 0 (0->0) | 5362200 | 17.6 | 20.1 | 20.3 | 21.1 | 23.3 | 63.4 | 471128.7 | 169 | 75 | 19 |
| D-rec | B-off | 0 (0->0) | 5546400 | 17.0 | 19.3 | 20.3 | 22.2 | 28.6 | 87.5 | 394296.7 | 129 | 53 | 10 |
| D-rec | B-on | 0 (0->0) | 5478000 | 16.5 | 19.5 | 20.1 | 21.5 | 25.6 | 137.7 | 54255.8 | 1973 | 11 | 1 |
| M-rec | B-off | 0 (0->0) | 5236000 | 18.8 | 21.1 | 21.3 | 23.8 | 27.6 | 114.2 | 83928.6 | 72 | 11 | 2 |
| M-rec | B-on | 0 (0->0) | 5056200 | 18.1 | 20.6 | 20.9 | 21.8 | 24.2 | 154.0 | 76704.2 | 2194 | 9 | 1 |
| M-flip | B-off | 0 (0->0) | 5320600 | 18.2 | 20.6 | 20.8 | 21.7 | 23.9 | 63.5 | 401207.4 | 84 | 37 | 9 |
| M-flip | B-on | 0 (0->0) | 5297000 | 18.1 | 20.5 | 20.8 | 21.7 | 23.9 | 75.6 | 98206.3 | 1330 | 13 | 1 |
| D-rec-2c | B-off | 0 (0->1) | 4904800 | 16.3 | 20.0 | 21.4 | 26.1 | 40.7 | 140.3 | 625715.6 | 389 | 108 | 28 |
| D-rec-2c | B-off | 1 (1->0) | 4878400 | 16.7 | 20.2 | 21.5 | 26.4 | 42.3 | 160.8 | 555532.7 | 394 | 115 | 27 |
| D-rec-2c | B-on | 0 (0->1) | 4946400 | 16.5 | 19.9 | 21.2 | 25.6 | 42.2 | 157.3 | 464372.3 | 1611 | 335 | 14 |
| D-rec-2c | B-on | 1 (1->0) | 5127200 | 16.3 | 19.8 | 21.2 | 25.5 | 35.8 | 103.0 | 398631.9 | 1017 | 159 | 13 |

### per-writer throughput in 10 s buckets (statements per bucket)

- U-nokey B-off w0: [465908, 464986, 423417, 441794, 420520, 415883, 410348, 379258, 466342, 436972, 407745, 428410]; refused={}; wrong_affected=0
- U-nokey B-on w0: [427054, 430552, 459624, 460899, 455584, 431241, 444431, 426210, 434091, 453739, 459833, 458842]; refused={}; wrong_affected=0
- D-rec B-off w0: [479456, 476708, 475620, 474162, 476382, 476088, 474482, 475698, 474316, 469870, 403346, 367306]; refused={}; wrong_affected=0
- D-rec B-on w0: [454440, 456814, 452178, 454266, 453348, 451960, 454282, 457692, 457598, 456086, 458860, 447288]; refused={}; wrong_affected=0
- M-rec B-off w0: [442250, 441626, 437291, 439838, 436939, 436650, 434596, 435471, 429368, 431504, 431652, 417602]; refused={}; wrong_affected=0
- M-rec B-on w0: [417109, 418733, 416354, 415866, 421634, 422040, 422339, 420014, 422214, 421364, 421523, 415607]; refused={}; wrong_affected=0
- M-flip B-off w0: [450279, 452567, 453169, 451539, 446661, 451252, 451852, 449731, 442788, 424559, 415470, 409249]; refused={}; wrong_affected=0
- M-flip B-on w0: [441936, 440379, 441448, 438680, 439723, 440899, 438939, 436837, 440772, 440605, 440486, 434277]; refused={}; wrong_affected=0
- D-rec-2c B-off w0: [442600, 437808, 444116, 445746, 345932, 356676, 440374, 417648, 312518, 427670, 443686, 368348]; refused={}; wrong_affected=0
- D-rec-2c B-off w1: [449886, 442020, 431960, 430214, 355004, 364992, 424068, 403786, 331072, 437542, 428268, 357820]; refused={}; wrong_affected=0
- D-rec-2c B-on w0: [410928, 406082, 415006, 413500, 406900, 367592, 367910, 428614, 415872, 416216, 445308, 430788]; refused={}; wrong_affected=0
- D-rec-2c B-on w1: [456196, 451122, 450868, 451038, 445630, 404670, 342492, 393436, 451256, 452200, 410940, 395352]; refused={}; wrong_affected=0

### series


#### U-nokey

| t (s) | off entries | on entries | off probe cpu us | on probe cpu us | off probe client p50 us | on probe client p50 us | on passes | on entries read | on entries removed |
|---|---|---|---|---|---|---|---|---|---|
| 0 (start) | 50000 | 50000 | 10.89 | 8.08 | 27.7 | 20.7 | 0 | 0 | 0 |
| 20.1 (mix) | 50000 | 50000 | 8.50 | 8.57 | 21.4 | 21.5 | 0 | 0 | 0 |
| 40.2 (mix) | 50000 | 50000 | 8.00 | 7.93 | 26.8 | 20.3 | 0 | 0 | 0 |
| 60.2 (mix) | 50000 | 50000 | 8.83 | 7.86 | 20.8 | 20.4 | 0 | 0 | 0 |
| 80.3 (mix) | 50000 | 50000 | 7.97 | 7.87 | 26.6 | 20.5 | 0 | 0 | 0 |
| 100.3 (mix) | 50000 | 50000 | 7.99 | 8.00 | 26.8 | 27.2 | 0 | 0 | 0 |
| 120 (end) | 50000 | 50000 | 10.62 | 11.38 | 29.7 | 30.6 | 0 | 0 | 0 |
| 121 (after-purge) | 50000 | 50000 | 7.98 | 8.13 | 27.3 | 28.2 | 0 | 0 | 0 |

#### D-rec

| t (s) | off entries | on entries | off probe cpu us | on probe cpu us | off probe client p50 us | on probe client p50 us | on passes | on entries read | on entries removed |
|---|---|---|---|---|---|---|---|---|---|
| 0 (start) | 50000 | 50000 | 11.63 | 11.51 | 35.0 | 27.5 | 0 | 0 | 0 |
| 20.1 (mix) | 542600 | 61922 | 54.19 | 13.46 | 69.9 | 27.8 | 20 | 1463212 | 458278 |
| 40.2 (mix) | 1018000 | 63354 | 110.44 | 12.56 | 126.3 | 27.8 | 40 | 2920354 | 910646 |
| 60.3 (mix) | 1494800 | 63234 | 175.53 | 12.71 | 212.7 | 35.5 | 60 | 4381438 | 1363766 |
| 80.5 (mix) | 1970400 | 62571 | 245.78 | 17.41 | 362.2 | 36.6 | 80 | 5849782 | 1820829 |
| 100.8 (mix) | 2443000 | 63106 | 316.88 | 14.06 | 438.3 | 29.8 | 100 | 7314613 | 2277494 |
| 120 (end) | 2823200 | 59001 | 420.33 | 13.90 | 451.4 | 29.1 | 120 | 8760615 | 2729999 |
| 121 (after-purge) | 59850 | 50258 | 15.02 | 13.28 | 30.8 | 28.7 | 121 | 8819616 | 2738742 |

#### M-rec

| t (s) | off entries | on entries | off probe cpu us | on probe cpu us | off probe client p50 us | on probe client p50 us | on passes | on entries read | on entries removed |
|---|---|---|---|---|---|---|---|---|---|
| 0 (start) | 50000 | 50000 | 7.80 | 8.19 | 20.3 | 20.8 | 0 | 0 | 0 |
| 20.1 (mix) | 960215 | 68903 | 14.20 | 7.71 | 33.1 | 20.1 | 19 | 1848341 | 843272 |
| 40.2 (mix) | 1837689 | 71099 | 19.00 | 7.85 | 31.8 | 26.9 | 40 | 3685002 | 1674256 |
| 60.2 (mix) | 2711322 | 73378 | 25.49 | 7.91 | 39.7 | 27.0 | 60 | 5532494 | 2516558 |
| 80.3 (mix) | 3581000 | 72972 | 29.22 | 7.91 | 41.9 | 27.2 | 80 | 7385125 | 3359936 |
| 100.4 (mix) | 4441405 | 72951 | 33.95 | 7.88 | 46.5 | 26.6 | 100 | 9237542 | 4204334 |
| 120 (end) | 5280388 | 64366 | 38.58 | 9.01 | 58.5 | 20.5 | 120 | 11074989 | 5041695 |
| 121 (after-purge) | 66322 | 50290 | 7.47 | 7.46 | 26.9 | 19.9 | 121 | 11139355 | 5055771 |

#### M-flip

| t (s) | off entries | on entries | off probe cpu us | on probe cpu us | off probe client p50 us | on probe client p50 us | on passes | on entries read | on entries removed |
|---|---|---|---|---|---|---|---|---|---|
| 0 (start) | 50000 | 50000 | 7.06 | 6.90 | 19.4 | 19.1 | 0 | 0 | 0 |
| 20.1 (mix) | 100000 | 83331 | 7.35 | 6.99 | 19.0 | 19.1 | 10 | 910783 | 406426 |
| 40.2 (mix) | 100000 | 83748 | 7.25 | 7.42 | 26.0 | 19.1 | 20 | 1824965 | 818147 |
| 60.2 (mix) | 100000 | 84754 | 7.44 | 7.00 | 26.1 | 19.1 | 30 | 2738826 | 1230474 |
| 80.3 (mix) | 100000 | 85300 | 7.08 | 6.98 | 26.0 | 19.0 | 40 | 3651617 | 1641841 |
| 100.3 (mix) | 100000 | 84325 | 7.29 | 7.97 | 26.0 | 26.2 | 50 | 4569281 | 2055921 |
| 120 (end) | 100000 | 81634 | 7.46 | 7.38 | 26.0 | 19.6 | 60 | 5480007 | 2466708 |
| 121 (after-purge) | 50177 | 50178 | 7.05 | 6.93 | 25.8 | 19.3 | 61 | 5561641 | 2498164 |

#### D-rec-2c

| t (s) | off entries | on entries | off probe cpu us | on probe cpu us | off probe client p50 us | on probe client p50 us | on passes | on entries read | on entries removed |
|---|---|---|---|---|---|---|---|---|---|
| 0 (start) | 50000 | 50000 | 13.85 | 13.98 | 29.0 | 29.5 | 0 | 0 | 0 |
| 20.1 (mix) | 963400 | 71889 | 114.05 | 16.85 | 131.2 | 37.6 | 19 | 1868598 | 867711 |
| 40.3 (mix) | 1840200 | 87766 | 253.65 | 16.20 | 364.8 | 31.2 | 29 | 3212883 | 1717834 |
| 60.5 (mix) | 2552200 | 86151 | 378.82 | 21.47 | 494.9 | 39.6 | 39 | 4530665 | 2532849 |
| 80.9 (mix) | 3388400 | 84824 | 530.26 | 15.87 | 641.9 | 39.9 | 49 | 5811371 | 3301376 |
| 101.4 (mix) | 4150200 | 84880 | 660.30 | 16.38 | 689.3 | 30.7 | 59 | 7190653 | 4169920 |
| 120 (end) | 4941600 | 88856 | 981.93 | 22.31 | 1096.8 | 36.3 | 69 | 8507986 | 4997944 |
| 121 (after-purge) | 68446 | 50494 | 18.76 | 14.90 | 32.9 | 29.8 | 71 | 8618639 | 5036306 |

### final PURGE INDEX and top latencies

- 00-U-nokey-B-off: {'tag': 'PURGED INDEX name=ix entries=0 skipped_rows=0 attempts=9', 'wall_ms': 32.8}; top-20 latencies per writer (us): [496876.5, 513218.2, 566335.3, 1007930.8, 1026470.9]
- 01-U-nokey-B-on: {'tag': 'PURGED INDEX name=ix entries=0 skipped_rows=0 attempts=9', 'wall_ms': 33.3}; top-20 latencies per writer (us): [342127.6, 356000.8, 367755.8, 415144.7, 471128.7]
- 02-D-rec-B-on: {'tag': 'PURGED INDEX name=ix entries=0 skipped_rows=0 attempts=17', 'wall_ms': 28.9}; top-20 latencies per writer (us): [11877.0, 13460.3, 32239.0, 36720.9, 54255.8]
- 03-D-rec-B-off: {'tag': 'PURGED INDEX name=ix entries=2763350 skipped_rows=0 attempts=616', 'wall_ms': 2650.9}; top-20 latencies per writer (us): [298165.5, 328609.9, 362730.2, 371753.3, 394296.7]
- 04-M-rec-B-off: {'tag': 'PURGED INDEX name=ix entries=5214066 skipped_rows=0 attempts=1021', 'wall_ms': 3746.3}; top-20 latencies per writer (us): [27348.3, 32423.0, 45343.1, 68067.5, 83928.6]
- 05-M-rec-B-on: {'tag': 'PURGED INDEX name=ix entries=0 skipped_rows=0 attempts=19', 'wall_ms': 34.8}; top-20 latencies per writer (us): [7042.1, 9389.3, 16795.7, 38635.0, 76704.2]
- 06-M-flip-B-on: {'tag': 'PURGED INDEX name=ix entries=0 skipped_rows=0 attempts=22', 'wall_ms': 29.2}; top-20 latencies per writer (us): [13494.7, 14086.5, 18432.4, 19874.5, 98206.3]
- 07-M-flip-B-off: {'tag': 'PURGED INDEX name=ix entries=49823 skipped_rows=0 attempts=22', 'wall_ms': 60.8}; top-20 latencies per writer (us): [294201.6, 333180.4, 346890.1, 373679.7, 401207.4]
- 08-D-rec-2c-B-off: {'tag': 'PURGED INDEX name=ix entries=4873154 skipped_rows=0 attempts=1153', 'wall_ms': 5565.4}; top-20 latencies per writer (us): [408858.0, 464328.5, 500895.6, 510900.4, 625715.6]; [419852.7, 454171.8, 461292.0, 478291.6, 555532.7]
- 09-D-rec-2c-B-on: {'tag': 'PURGED INDEX name=ix entries=0 skipped_rows=0 attempts=31', 'wall_ms': 40.1}; top-20 latencies per writer (us): [320051.9, 324634.5, 327416.5, 336734.0, 464372.3]; [310841.7, 324546.0, 349971.2, 366182.1, 398631.9]
