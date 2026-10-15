# BX close overhead: an ordinary secondary index costs no more than the A/A' spread, a UNIQUE index adds about 5 us to an INSERT and 7 us to an indexed-column UPDATE

**Thesis.** On `worktree-bx-unique-index`, with the engine at `9bf559e2` (`v3.2.0-62-g9bf559e2`, **B**, BX-S3, the milestone's code close) against `87d2893c` (`v3.2.0-58-g87d2893c`, **A**, the commit BX opened from), a single-row `INSERT` or `UPDATE` on a btree relation whose only index is **ordinary** costs B - A = **+0.09 to +0.30 us of server CPU per statement (+0.3 % to +1.1 %)**, pooled over three row-set sizes (30 runs per shape and path). That is smaller than the per-run scatter of the in-run control (A' - A, sd 0.4-0.6 us), but it is not centred on zero: B is above A in 124 of 180 paired runs while A' is above A in 79 of 180. So the claim "no cost" holds to within about 0.3 us (1 %), and CLA does not report it as exactly zero. Declaring the same index **`UNIQUE`** (B only) adds **+5.0 to +5.5 us per `INSERT` (+19 % to +22 %)** and **+6.6 to +7.6 us per `UPDATE` of the indexed column (+23 % to +27 %)**, in every one of 30 paired runs per cell; an `UPDATE` that does not assign the indexed column pays **+0.1 to +0.5 us (+0.4 % to +1.9 %), inside the floor**.

Server CPU is the sum of `/proc/<pid>/task/*/schedstat` over the server's threads (`tools/bn_census.py`'s accounting, imported unmodified); wall is the client's. The driver is new: `tools/bx_ab.py` (untracked at the time of writing; it imports `bn_census`, `kwp`, `bb_overhead_benchmark` and changes none of them). `bench/docs/` stays closed (`bench/README.md`).

| Item | Value |
|---|---|
| Executed | 2026-10-15 UTC. Builds 10:25:02-10:26:42; measurement 10:29:42-10:36:51 (smoke run, 1,000-row group, two 200 / 10,000-row groups; see section 1) |
| Branch / worktree | `worktree-bx-unique-index` (the session's worktree `bx-unique-index`, HEAD `9bf559e2`). The binaries were built from detached checkouts of the two commits, so the dirty tracked files of the session worktree (docs only) are not in them; the checkouts were removed afterwards |
| Commits measured | **A** = `87d2893c` (`v3.2.0-58-g87d2893c`, committed 02:16:00 UTC), **B** = `9bf559e2` (`v3.2.0-62-g9bf559e2`, committed 10:24:20 UTC). A' = A's bytes started as a second server (in-run control). **BU** = B's binary with the index declared `UNIQUE` |
| Binary provenance | `git worktree add --detach` of each commit, `build-rel/`, `CMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server -j4`, both built together, finished before any measurement. Source binary mtimes: A 10:26:42.677, B 10:26:42.882 UTC (both newer than their commits, so both are the commits' engines). **A and A'** sha256 `417497cec4a53ac743dbbe2d8d86d430242edc3e99ee9342c080202a845f3014`; **B and BU** `009091ce9235fdb86b55e81969bf5e4c9f0e3625898671c9b3805a730e7c68d3`. Every server started from the copies in `/home/cdkbs/bench-runs/bx-close/bin/` |
| Device | Data files and WAL under `/home/cdkbs/bench-runs/bx-close/wd/` on `/dev/root`, ext4 (`df -T`), not tmpfs |
| Build type | Release (above) |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, `scan_kernel` default; fresh server and data file per arm per run; server pinned to CPU 2, driver to CPU 4, one server busy at a time |
| Host load | 8 vCPU, Linux 7.0.0-1014-azure. `uptime` load average 4.78 at 10:24 (the builds' own and other sessions' tail), 0.45 at 10:29:42 before the first cell. Load average read before each cell: maximum 1.45; `pgrep -f "cc1plus|cmake --build|ctest"` found no competing process before any of the 720 cells (`competing` = 0 in every record); the one-minute watchdog (`watch.txt`) never saw a stall or an orphan. No `--cell-quiet-pct` gating was used (the host was already quiet) |
| Drivers | `tools/bx_ab.py` as committed to the tree is the final revision (key space taken from `--rows`); the 1,000-row group ran on the first revision, which fixed it at 1,000, and is unaffected by the change (see section 1). `tools/bn_census.py` `35693dc24f17...`, `tools/kwp.py` `99341280c37f...` |
| Suite | Not executed in this task; no engine code was changed by this measurement |
| Prior number | None for this shape (a unique-index cell cannot exist before BX; the ordinary-index INSERT/UPDATE cells have no earlier results file). **A is this milestone's baseline and this file becomes the one the next run is read against.** PostgreSQL floor: not measured for this shape |

## 1. What was run and what was cut

Four servers per run (A, A', B, BU), a fresh server and data file for each, side by side; every cell is cut into 6 blocks of 333 statements taken round-robin, the first server of a block rotating by one per block and per run, so each arm takes each position equally often. Each server holds `t` (`id, v, w`, `--rows` rows, ordinary index `ix_t_v` on `v`) and an empty `i` (`id, v, w`, index `ix_i_w` on `w`), both `BTREE`. On BU both indexes are `CREATE UNIQUE INDEX`; on the others `CREATE INDEX`. Each statement path is run as a literal (unnamed `C_PARSE` per execution) and as a prepared statement.

| shape | statement | indexed column touched | count per cell | notes |
|---|---|---|---|---|
| insert | `INSERT INTO i VALUES (?, ?, ?)` | yes (`w` = the key, distinct) | 1,998 per arm per run | one index entry written per statement; independent of `--rows` |
| update-idx | `UPDATE t SET v = ? WHERE id = ?` | yes (`v` assigned a distinct value) | 1,998 | the unique check runs on BU |
| update-w | `UPDATE t SET w = ? WHERE id = ?` | no (`v` is the indexed column) | 1,998 | on B and BU the statement still reaches the unique-keys collector; on BU the index is not written |

`--rows` is the row count of `t`: **200, 1,000 and 10,000** (rule 9; the keys of the `UPDATE`s are `1 + (q * 7919) mod rows`, so the whole table is the working set). The `INSERT` does not touch `t`, so its three sizes are three repeats of one shape and say whether the table size leaks into it (it does not: section 2). Runs: 10 per size, 120 timed cells per size and 240 records per size file (4 arms x 3 shapes x 2 paths x 10 runs), 720 in all; every reply was checked against its expected tag and **0 statements were refused** in the three final groups.

**One run was thrown away and redone.** The first revision of the driver hard-coded 1,000 as the `UPDATE` key space; at 200 rows 80 % of the keys matched nothing (the driver counted 256,000 mismatching replies and the cells were visibly cheaper). The 200- and 10,000-row groups were re-run after the fix with the same 10 runs each; only the re-run is reported. The 1,000-row group, which the first revision measured correctly, was not repeated.

**Cut for the one-hour budget:** a multi-row `INSERT` (the collector runs once per statement either way, and the driver has no shape for it); two-core or `strict`/`group` durability (the question is the per-statement CPU of the unique-keys path, and `group` durability's wait is 82-85 % of an update and would hide it; the cost here is therefore the upper bound as a share of a relaxed statement, not a share of a durable one); a covering index; a composite unique key; contention on a unique key from a second session (BX-S3's cross-core case is a correctness cell, not an overhead cell); the cost of `CREATE UNIQUE INDEX` itself on a populated table. No PostgreSQL twin: the floor was not measured for this shape.

## 2. Cell 1: an ordinary index, A against B

B - A paired by run, in us of server CPU per statement; 10 runs per size, three sizes. "A' - A" is the in-run control (the same bytes as A, a second server).

| shape | path | rows | A | A' | B | B - A median [min, max] | % of A | largest \|A' - A\| | verdict |
|---|---|---|---|---|---|---|---|---|---|
| insert | literal | 200 | 25.47 us | 25.19 us | 25.77 us | +0.08 [-0.95, +0.66] | +0.3 % | 1.17 us | inside |
| insert | literal | 1,000 | 25.77 us | 25.97 us | 25.62 us | -0.05 [-0.94, +1.26] | -0.2 % | 1.44 us | inside |
| insert | literal | 10,000 | 25.55 us | 25.60 us | 25.99 us | +0.19 [-0.39, +0.82] | +0.8 % | 0.96 us | inside |
| insert | prepared | 200 | 24.83 us | 24.79 us | 25.05 us | +0.23 [-0.62, +0.71] | +0.9 % | 0.68 us | inside |
| insert | prepared | 1,000 | 25.00 us | 24.93 us | 24.91 us | -0.16 [-0.57, +3.52] | -0.6 % | 1.19 us | inside |
| insert | prepared | 10,000 | 25.07 us | 24.89 us | 25.32 us | +0.23 [-0.34, +0.99] | +0.9 % | 1.19 us | inside |
| update-idx | literal | 200 | 28.46 us | 28.40 us | 28.82 us | +0.58 [-0.92, +1.29] | +2.0 % | 1.23 us | inside |
| update-idx | literal | 1,000 | 28.71 us | 29.09 us | 28.92 us | +0.26 [-1.11, +1.02] | +0.9 % | 1.08 us | inside |
| update-idx | literal | 10,000 | 29.68 us | 29.30 us | 30.01 us | +0.29 [-0.13, +0.95] | +1.0 % | 1.00 us | inside |
| update-idx | prepared | 200 | 27.72 us | 27.76 us | 28.16 us | +0.49 [-0.79, +1.21] | +1.8 % | 0.85 us | inside |
| update-idx | prepared | 1,000 | 27.83 us | 28.16 us | 27.96 us | +0.17 [-0.69, +1.19] | +0.6 % | 0.98 us | inside |
| update-idx | prepared | 10,000 | 29.11 us | 28.78 us | 29.17 us | +0.13 [-2.10, +0.81] | +0.5 % | 1.18 us | inside |
| update-w | literal | 200 | 26.64 us | 26.22 us | 26.58 us | +0.14 [-1.31, +1.01] | +0.5 % | 1.55 us | inside |
| update-w | literal | 1,000 | 26.35 us | 26.82 us | 26.51 us | +0.13 [-0.93, +1.16] | +0.5 % | 1.45 us | inside |
| update-w | literal | 10,000 | 27.35 us | 26.79 us | 27.48 us | +0.16 [-0.62, +0.63] | +0.6 % | 1.27 us | inside |
| update-w | prepared | 200 | 25.44 us | 25.41 us | 26.04 us | +0.73 [-1.06, +1.06] | +2.9 % | 1.20 us | inside |
| update-w | prepared | 1,000 | 25.83 us | 26.24 us | 26.07 us | +0.30 [-0.82, +1.30] | +1.2 % | 1.07 us | inside |
| update-w | prepared | 10,000 | 26.56 us | 26.28 us | 26.74 us | +0.16 [-0.46, +0.66] | +0.6 % | 0.83 us | inside |

Every one of the 18 cells is inside the A' - A scatter, and none shows a size trend (the `INSERT`, which does not depend on the table size, moves -0.16 to +0.23 across the three sizes, which is the noise between repeats of one cell). Pooling the three sizes, because the cells are the same code path at 10 runs each:

| shape | path | paired runs | B - A median | B above A | A' - A median | A' above A | sd of A' - A |
|---|---|---|---|---|---|---|---|
| insert | literal | 30 | +0.09 us | 18 of 30 | -0.02 us | 15 of 30 | 0.56 us |
| insert | prepared | 30 | +0.18 us | 19 of 30 | -0.15 us | 10 of 30 | 0.43 us |
| update-idx | literal | 30 | +0.30 us | 22 of 30 | -0.26 us | 13 of 30 | 0.62 us |
| update-idx | prepared | 30 | +0.18 us | 23 of 30 | -0.14 us | 13 of 30 | 0.56 us |
| update-w | literal | 30 | +0.16 us | 20 of 30 | -0.13 us | 12 of 30 | 0.63 us |
| update-w | prepared | 30 | +0.21 us | 22 of 30 | +0.07 us | 16 of 30 | 0.54 us |
| all | both | 180 | +0.18 us (+0.7 %) | 124 of 180 | -0.12 us | 79 of 180 | |

Reading: no cell resolves a cost, and no cell shows a gain. But the pooled sign is consistent: B sits a fraction of a microsecond above A (124 of 180 against 79 of 180 for the control) and the control sits slightly below. The two offsets together put the B-versus-A difference at about **+0.3 us, +1 % of a 26 us statement**, which is the size of what the new per-statement collector loop is expected to cost (a walk over the relation's one index, finding it ordinary). A run-to-run swing of 1 us in one cell is larger than it, so a single cell of this matrix could not have shown it. CLA states "no cost beyond about 1 %", not "no cost".

## 3. Cell 2: the same workload with the index `UNIQUE`, B only

BU - B paired by run (the same binary; the only difference is `UNIQUE` on the index). Throughput is derived (`1e6 / mean wall us`). The `UPDATE` rows are the 1,000-row group, the sizes are in the second table.

| shape | path | B server CPU | BU server CPU | BU - B median [min, max] | % of B | B statements/s | BU statements/s | BU - B above 0 |
|---|---|---|---|---|---|---|---|---|
| insert | literal | 25.62 us | 31.10 us | +5.32 us [+3.89, +7.44] | +20.8 % | 20,029 | 17,984 | 10 of 10 |
| insert | prepared | 24.91 us | 30.54 us | +5.53 us [+2.83, +6.46] | +22.2 % | 21,140 | 18,975 | 10 of 10 |
| update-idx | literal | 28.92 us | 36.51 us | +7.36 us [+6.56, +8.11] | +25.4 % | 18,748 | 16,264 | 10 of 10 |
| update-idx | prepared | 27.96 us | 35.55 us | +7.61 us [+6.55, +8.18] | +27.2 % | 19,935 | 17,297 | 10 of 10 |
| update-w | literal | 26.51 us | 27.03 us | +0.50 us [-1.05, +0.82] | +1.9 % | 19,638 | 19,595 | 5 of 10 (inside the floor) |
| update-w | prepared | 26.07 us | 26.39 us | +0.27 us [-0.78, +1.16] | +1.0 % | 20,305 | 20,774 | 3 of 10 (inside the floor) |

The same difference at the other two sizes (BU - B median, us of server CPU, 10 runs each):

| shape | path | 200 rows | 1,000 rows | 10,000 rows |
|---|---|---|---|---|
| insert | literal | +5.00 us (+19.4 %) | +5.32 us (+20.8 %) | +5.15 us (+19.8 %) |
| insert | prepared | +5.28 us (+21.1 %) | +5.53 us (+22.2 %) | +5.27 us (+20.8 %) |
| update-idx | literal | +6.65 us (+23.1 %) | +7.36 us (+25.4 %) | +7.39 us (+24.6 %) |
| update-idx | prepared | +6.78 us (+24.1 %) | +7.61 us (+27.2 %) | +7.26 us (+24.9 %) |
| update-w | literal | +0.29 us (+1.1 %) | +0.50 us (+1.9 %) | +0.38 us (+1.4 %) |
| update-w | prepared | +0.28 us (+1.1 %) | +0.27 us (+1.0 %) | +0.34 us (+1.3 %) |

The unique check is a fixed cost per written key: it moves by at most 0.8 us across a 50x change in table size. In the `UPDATE`s that assign the indexed column it is about 1.4 times the `INSERT`'s, which is what the engine does: an `UPDATE` of an indexed column writes a new index entry and has to check the new key against the live rows, while an `INSERT` checks one key. The `update-w` rows, where the indexed column is not assigned, cost +0.3 to +0.5 us on BU against B: the unique-keys collector is entered, finds that no unique key changed, and does nothing further. That cost is of the order of the B - A offset in section 2 and not separable from it.

## 4. Latency, 1,000-row group (wall per statement, median over runs of each run's percentile)

`bn_census.py` emits p90, not p95 (the five percentiles of rule 6 are p0, p25, p50, p90 and p99 here). ops = statements over 10 runs. Throughput is derived from the mean wall.

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s (derived) | server CPU | driver CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | literal | A | 19,980 | 40.9 us | 45.5 us | 46.2 us | 58.1 us | 83.0 us | 20,024 | 25.77 us | 24.56 us |
| insert | literal | B | 19,980 | 41.4 us | 45.5 us | 46.2 us | 57.5 us | 82.8 us | 20,029 | 25.62 us | 24.58 us |
| insert | literal | BU | 19,980 | 44.8 us | 50.5 us | 51.6 us | 65.5 us | 87.6 us | 17,984 | 31.10 us | 24.60 us |
| insert | prepared | A | 19,980 | 38.1 us | 42.9 us | 43.7 us | 54.3 us | 79.7 us | 21,159 | 25.00 us | 22.55 us |
| insert | prepared | B | 19,980 | 37.0 us | 42.9 us | 43.6 us | 54.9 us | 80.9 us | 21,140 | 24.91 us | 22.64 us |
| insert | prepared | BU | 19,980 | 44.1 us | 48.0 us | 49.1 us | 62.4 us | 85.6 us | 18,975 | 30.54 us | 22.61 us |
| update-idx | literal | A | 19,980 | 41.6 us | 48.4 us | 49.2 us | 61.9 us | 85.6 us | 18,729 | 28.71 us | 24.75 us |
| update-idx | literal | B | 19,980 | 42.0 us | 48.7 us | 49.5 us | 62.1 us | 82.7 us | 18,748 | 28.92 us | 24.74 us |
| update-idx | literal | BU | 19,980 | 47.5 us | 55.6 us | 56.7 us | 70.5 us | 96.2 us | 16,264 | 36.51 us | 24.75 us |
| update-idx | prepared | A | 19,980 | 39.6 us | 45.3 us | 46.0 us | 57.7 us | 80.3 us | 19,735 | 27.83 us | 22.24 us |
| update-idx | prepared | B | 19,980 | 39.5 us | 45.5 us | 46.3 us | 58.4 us | 81.8 us | 19,935 | 27.96 us | 22.13 us |
| update-idx | prepared | BU | 19,980 | 45.9 us | 52.4 us | 53.5 us | 67.5 us | 92.4 us | 17,297 | 35.55 us | 22.28 us |
| update-w | literal | A | 19,980 | 38.7 us | 46.5 us | 47.1 us | 58.7 us | 79.7 us | 19,697 | 26.35 us | 24.66 us |
| update-w | literal | B | 19,980 | 40.4 us | 46.5 us | 47.1 us | 59.0 us | 83.4 us | 19,638 | 26.51 us | 24.72 us |
| update-w | literal | BU | 19,980 | 40.1 us | 46.9 us | 47.7 us | 59.5 us | 82.2 us | 19,595 | 27.03 us | 24.62 us |
| update-w | prepared | A | 19,980 | 36.2 us | 43.5 us | 44.2 us | 55.3 us | 76.6 us | 20,820 | 25.83 us | 22.20 us |
| update-w | prepared | B | 19,980 | 36.8 us | 43.4 us | 44.1 us | 56.1 us | 80.0 us | 20,305 | 26.07 us | 22.23 us |
| update-w | prepared | BU | 19,980 | 36.8 us | 43.9 us | 44.5 us | 55.4 us | 77.0 us | 20,774 | 26.39 us | 22.11 us |

The BU shift is a shift of the whole distribution, not a tail: p0 moves by +3.4 to +6.1 us, p50 by +5.4 to +7.5 us and p99 by +3.3 to +10.6 us, so the added cost is paid by every statement, which is what a fixed per-key check does. A and B are the same distribution at every percentile of every shape (p0 to p99 within 1.6 us, mixed sign), consistent with section 2. The A' arm's rows are in `g1000.json`; they are not repeated here.

## 5. Where the time goes

A statement here is about 46-50 us of client-observed wall, of which the **server's CPU is 25-29 us** (relaxed durability: no fsync wait, none of the WAL waits of a `group` or `strict` commit), the **driver's own CPU is 22-25 us**, and the remainder is the socket round trip and the scheduling hand-off between the two pinned CPUs (the client's wall minus the larger of the two). Within the server's share the unique check is the only component these cells isolate: **+5 us per `INSERT` and +7 us per indexed-column `UPDATE`**, against a 25-29 us statement. Lock-unit acquire/release, the index run walk and the clustered descent are one cost here, and CLA did not decompose it further: no sampler run was done (the stage budget), so the split between the lock unit and the two walks is unmeasured. Fsync / commit wait, lock or conflict wait and the second-session wait do not apply to this measurement: `relaxed` durability on one session never waits on any of them, and the cut list in section 1 names the cells where they would.

## 6. What it says about the engine

1. **An ordinary index is free to within about 1 %; the milestone's claim stands at that resolution.** The unique-keys collector's loop over the relation's indexes costs about 0.3 us in the 26 us statement once the control's offset is removed. The sign is positive across the pooled runs, so the loop is present and cheap, not absent.
2. **A unique index costs a fixed ~5 us per `INSERT` and ~7 us per indexed-column `UPDATE`, independent of the table size over 200 to 10,000 rows**, and a fifth to a quarter of a relaxed statement's server CPU. The size independence says the check does not scan; the tree depth was not recorded and the split of the cost between the lock unit and the walks was not measured. Whether the same holds at tens of millions of rows is not measured (the cut list names no such cell, and the ceiling of the descent is not reached here).
3. **The `UPDATE` that does not assign the unique column pays about 0.3-0.5 us (1-2 %), and CLA cannot separate that from the ordinary-index offset.** This run shows the cost of a statement that reaches the collector and writes no unique key is small; it does not show whether the check is skipped or merely cheap, and `docs/spec/index.md` was not read for this file.
4. **This is the cost on the cheapest durability rung.** Under `group` durability a statement spends 82-85 % of its time waiting for the log (project memory, "KDS benchmark host"); the same +5 to +7 us would be 1-2 % there. The 20-27 % shares in section 3 are the worst case, the right figure for a CPU-bound `relaxed` or in-memory deployment, and an upper bound for a durable one. The `strict` rung and a second core (where BX-S3's cross-core key lock is exercised) are not measured here.
5. **No stated expectation was contradicted.** The operator's brief for this close claimed no cost for an ordinary index; the data supports it to about 1 %. The unique check has its first overhead data point here; the BX work order was not re-read for this file.

## Reproduce

    cp <build-release>/kds_server $RUN/bin/kds_A ; ... kds_A2 (same bytes) ; kds_B
    python3 tools/bx_ab.py --arm A=$RUN/bin/kds_A --arm A2=$RUN/bin/kds_A2 --arm B=$RUN/bin/kds_B \
        --arm BU=$RUN/bin/kds_B,unique --workdir $RUN/wd --runs 10 --rows {200|1000|10000} \
        --port 31960 --out g<rows>.json --progress p<rows>.txt

The raw per-run, per-cell JSON (`g200.json`, `g1000.json`, `g10000.json`), the watchdog lines (`watch*.txt`, `p*.txt`) and the analysis script are under `/home/cdkbs/bench-runs/bx-close/`; a narrower measurement archives nothing in the tree (rule 1b).
