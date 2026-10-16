# BY close: an ordinary index costs the same to write, REINDEX shrinks a churned index to its live share without moving a probe, and a dropped index's pages come back after two checkpoints

**Thesis.** On `worktree-reindex-drop-index-reclaim`, with the engine at `9da66f0e` (`v3.2.0-119-g9da66f0e`, **B**, BY-S3, the milestone's code close) against `5f610082` (`v3.2.0-115-g5f610082`, **A**, the commit BY opened from), four findings:

1. **An ordinary single-row `INSERT` or `UPDATE` on a btree relation with one ordinary secondary index costs B - A = +0.33 us of server CPU per statement (+1.2 %)**, pooled over three row-set sizes (540 paired runs; per cell +0.18 to +0.50 us, every one inside the scatter of single runs). The in-run control, A' (A's bytes started as a second server), sits +0.10 us above A, so the part of the difference that is not a start-position offset is about **+0.2 us (+0.8 %)**: +0.4 us (+1.6 %) for the `INSERT`, +0.2 us (+0.7 %) for an `UPDATE` of the indexed column, +0.1 us (+0.3 %) for an `UPDATE` of another column. The claim "unchanged" holds to within about 1 % to 2 %; CLA does not report it as exactly zero.
2. **`REINDEX INDEX` leaves exactly the live set in the index and frees the rest**: after 3x surplus from rolled-back inserts, or from `DELETE` + `PURGE`, the entry count falls from 4n to n and the tree from 1,682 pages to 421 at n = 100,000 (4.0x), at **0.64 us of build per row** (63-65 ms for 100,000 rows, the relation's `X` held throughout). **A probe through the rebuilt index costs the same**: the rebuilt twin minus the untouched twin is -0.48 to +0.14 us across 10 cells, the same spread as the untouched control, including the cell where the height falls from 3 to 2.
3. **`DROP INDEX` returns every page of the tree and nothing at the statement**: 0 pages at the `DROP`, the whole tree (1, 5, 42, 421 pages at 200 / 1,000 / 10,000 / 100,000 rows) after **9.5 to 10 s, two checkpoint runs**, and a `CREATE INDEX` of the same size afterwards takes exactly those pages back (`pages_reused` = `pages_allocated`, the same root page id, the data file unchanged).
4. **One engine defect, not in BY, that stops half the REINDEX workload: a relation whose rows were `UPDATE`d cannot be indexed or reindexed.** `CREATE INDEX` / `REINDEX INDEX` is refused `Corruption` (`undo record at N has type 0`) once rows carry an undo chain two deep (two `UPDATE`s of 200 rows), on A as well as B. The `UPDATE`-churn row of the REINDEX matrix therefore has no after-state. A fix is on `worktree-fix-backfill-walk-horizon` (`796ecf49`), in neither binary measured here.

Server CPU is the sum of `/proc/<pid>/task/*/schedstat` over the server's threads (`tools/bn_census.py`'s accounting, imported unmodified); wall is the client's. The A/B driver is `tools/bx_ab.py` (committed at `ec87ae1f`, run unmodified; its `UNIQUE` arm is not used). The REINDEX and DROP INDEX cells use a scratch driver, `by_reclaim.py`, that imports `bn_census` and `kwp` and changes neither; it is not in the tree (section 1). `bench/docs/` stays closed (`bench/README.md`).

| Item | Value |
|---|---|
| Executed | 2026-10-16 UTC. Builds about 05:43:50-05:45:36. Quiet A/B matrix 06:04:12-06:11:24. REINDEX cells 05:59:05-06:03:00 (200, 1,000, 10,000, 100,000 rows; the 25,000-row cell a minute later). DROP INDEX cells 06:01:40-06:02:22 |
| Branch / worktree | `worktree-reindex-drop-index-reclaim` (the session worktree `reindex-drop-index-reclaim`, HEAD `9da66f0e`). The binaries were built from detached checkouts of the two commits, so the session worktree's uncommitted files (docs, `tests/index_reindex_test.cpp`) are in neither; the checkouts were removed afterwards |
| Commits measured | **A** = `5f610082` (`v3.2.0-115-g5f610082`, committed 00:49:36 UTC), **B** = `9da66f0e` (`v3.2.0-119-g9da66f0e`, committed 05:42:38 UTC). A' = A's bytes started as a second server (in-run control) |
| Binary provenance | `git worktree add --detach` of each commit, `build-rel/`, `CMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server -j4`, both built together, finished before any measurement. Source binary mtimes: A 05:45:36.65, B 05:45:34.53 UTC (both newer than their commits, so both are the commits' engines). **A and A'** sha256 `3432e56b34a489a813ecc81ea9b2c183660550dd4f5cf18473165c7a369d43c5`; **B** `e2f53f5e0f0be41acdae526789afd065f075429deae61524869fdc7d870999b3`. Every server started from the copies in `/home/cdkbs/bench-runs/by-close/bin/`. The REINDEX and DROP INDEX cells ran on B's copy only |
| Device | Data files and WAL under `/home/cdkbs/bench-runs/by-close/wd/` on `/dev/root` (`/dev/nvme0n1p1`), ext4 (`df -T`, `mount`), not tmpfs |
| Build type | Release (above) |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536` (512 MiB, larger than every relation here, so every page is resident), `log_level = warn`, the checkpoint cadence at its default (5 s); fresh server and data file per arm per run (A/B) and per cell group (REINDEX, DROP INDEX). Server pinned to CPU 2, driver to CPU 4, one server busy at a time. REINDEX and DROP INDEX cells: one session |
| Host load | 8 vCPU (4 cores x 2 threads), Linux 7.0.0-1014-azure. The reported A/B matrix ran on a quiet host: 1-minute load average 0.35 at the start, maximum 1.56 across the matrix (the run's own server), `pgrep -f "cc1plus\|cmake --build\|ctest"` found no process before any of the 1,620 cells (`competing` = 0 in every record), the one-minute watchdog (`watch4.txt`) never saw a stall or an orphan. The REINDEX / DROP INDEX cells have no per-cell host record; the watchdog's load average read 0.64-1.40 during them (`watch3.txt`) |
| Drivers | `tools/bx_ab.py` (git blob `510b3f8f5b06...`), `tools/bn_census.py` `35693dc24f17...`, `tools/kwp.py` `99341280c37f...`, `tools/bb_overhead_benchmark.py` `ba9100efeada...` (SHA-256 prefixes of the files in the session worktree) |
| Suite | Not executed in this task; no engine code was changed by this measurement |
| Prior number | **A/B shape:** `results-bx-close-v3.2.0-62-g9bf559e2.md` is the last result for this shape (ordinary index, `INSERT` and the two `UPDATE`s); section 6 sets this run's A beside its B arm. **REINDEX and DROP INDEX cells:** none before BY; this file is the baseline the next run is read against. PostgreSQL floor: not measured for these shapes |

## 1. What was run and what was cut

### 1.1 The A/B (cell 1)

Three servers per run (A, A', B), a fresh server and data file for each, side by side; every cell is cut into 6 blocks of 333 statements taken round-robin, the first server of a block rotating by one per block and per run. Each server holds `t` (`id, v, w`, `--rows` rows, ordinary index `ix_t_v` on `v`) and an empty `i` (`id, v, w`, ordinary index `ix_i_w` on `w`), both `BTREE`, both indexes created with `CREATE INDEX` (not `UNIQUE`). Each statement path is run as a literal (unnamed `C_PARSE` per execution) and as a prepared statement.

| shape | statement | indexed column touched | count per cell |
|---|---|---|---|
| INSERT | `INSERT INTO i VALUES (?, ?, ?)` | yes (`w` = the key, distinct) | 1,998 per arm per run |
| UPDATE indexed col | `UPDATE t SET v = ? WHERE id = ?` | yes (`v` assigned a distinct value) | 1,998 |
| UPDATE other col | `UPDATE t SET w = ? WHERE id = ?` | no (`v` is the indexed column) | 1,998 |

`--rows` is the row count of `t`: **200, 1,000 and 10,000** (rule 9; the keys of the `UPDATE`s are `1 + (q * 7919) mod rows`). The `INSERT` does not touch `t`, so its three sizes are three repeats of one shape. Runs: **30 per size**, 540 paired runs per arm in all; every reply was checked against its expected tag and **0 statements were refused** (`bad` = 0 in all 1,620 records).

**One matrix was thrown away and redone.** The first matrix (10 + 20 runs per size, 05:46-05:54) ran while another session's `ctest` was alive on this host (`competing` = 1 in every cell); the host was not quiet by the standard that the benchmark rules set, so it was repeated at 06:04 on a host with no competing process and only that repeat is reported. The discarded matrix gave the same picture (B - A +0.52 us, A' - A +0.35 us, B - A' +0.17 us pooled), so nothing in section 2 depends on the choice.

### 1.2 The REINDEX cells (cell 2)

One fresh server per row-set size, B's binary. Per size, three kinds of churn, each on a **pair of identical relations** `x_a` and `x_b` (`id, v, w`, `n` live rows with `v = id`, an ordinary index on `v`):

| churn | how the surplus is made | surplus entries |
|---|---|---|
| rolled-back INSERTs (`rb`) | `BEGIN`, `INSERT` 1,000 rows of fresh `v`, `ROLLBACK`, repeated until 3n rows | 3n |
| DELETE + PURGE (`pg`) | 3n rows committed, then `DELETE ... WHERE id > n` and `PURGE FROM ... WHERE id > n` | 3n |
| one every-row UPDATE (`up`) | `UPDATE x SET v = v + 10000000 WHERE id >= 1`, once | n |

Twin `a` takes `REINDEX INDEX`; twin `b` is the control and is never touched. Probes are `SELECT * FROM x WHERE v = ?` (prepared; `ANALYZE` confirmed `step 0 IndexProbe` on twin `a` of every churn before and, for the rebuilt ones, after the rebuild; 60 probes returned the same row from `a` and `b` before the rebuild in every cell and after it in every rebuilt cell), 6,000 per relation per repetition in 6 interleaved blocks, 3 repetitions before and 3 after: 18,000 probes per relation per phase. The entry count and height come from `SELECT * FROM sys.indexes` (`entries`, `height`); the **page count** is not exposed per index (`SHOW PAGE` misreads an index page as a heap page), so it is derived two ways that agree: the `pages_allocated` delta across the `REINDEX` statement is the new tree, the `pages_freed` delta across the reclaim that follows is the old one (`SHOW META`, `allocation` section). Sizes: `n` = **200, 1,000, 10,000** (rule 9), plus **25,000** (found by looking for the row count at which 4n entries need one more tree level than n) and **100,000** (the extreme). The 25,000-row cell is the one where a rebuild changes the height, 3 to 2.

### 1.3 The DROP INDEX cells (cell 3)

One server, relations `d200`, `d1000`, `d10000`, `d100000` (`id, v, w`, `v = id`), taken in order. For each: `SHOW META` counters and the data file's size, `CREATE INDEX` on `v`, `DROP INDEX`, poll `SHOW META` every 0.5 s until `reclaim_pending` is 0 and `pages_freed` has risen (limit 90 s), then `CREATE INDEX` of the same name again. No other session, no open transaction: the reclaimer's "every older statement ended" condition is met at once, so what is measured is the checkpoint gate alone.

### 1.4 What was cut

Cut for the one-hour budget, or never in scope of a single-session relaxed run: **two-core and `strict`/`group` durability** (the cost asked about is per-statement CPU and tree shape; under `group` the log wait is 82-85 % of an update and would hide a 0.3 us difference); **a cold buffer pool** (the pool holds every page, so a smaller tree could not save a read; the page savings below are space, and the I/O saving is unmeasured); **concurrent writers during a REINDEX** (the relation's `X` is held for the build, 65 ms at 100,000 rows, and what a writer waiting behind it sees was not measured; nor was a larger relation); **a `UNIQUE`, covering or composite index rebuild**; **a long-lived reader or open transaction holding the reclaim back**; **a crash between the swap's records or during the frees** (correctness cells of BY-S2 and BY-S3, not overhead cells); **repeats of the REINDEX and DROP cells** (one run each, so a single value is a single sample: the probe CPU cells carry their own in-run control, the page and entry counts are deterministic, the wait times are one sample each of a 5 s cadence); **UPDATE-churn REINDEX** (section 5). No PostgreSQL twin: the floor was not measured for these shapes.

## 2. Cell 1: an ordinary index, A against B

Pooled over the three sizes, in us of server CPU per statement, paired by run (540 runs per row of the last line):

| statement | paths pooled | paired runs | A (median) | B - A median | % of A | A' - A median | B - A' median | % of A | B above A | A' above A |
|---|---|---|---|---|---|---|---|---|---|---|
| INSERT | literal + prepared | 180 | 25.88 us | +0.31 us | +1.2 % | -0.06 us | +0.42 us | +1.6 % | 129 | 81 |
| UPDATE indexed col | literal + prepared | 180 | 28.97 us | +0.38 us | +1.3 % | +0.21 us | +0.20 us | +0.7 % | 135 | 116 |
| UPDATE other col | literal + prepared | 180 | 26.52 us | +0.27 us | +1.0 % | +0.17 us | +0.08 us | +0.3 % | 125 | 113 |
| all | both | 540 | 26.67 us | +0.33 us | +1.2 % | +0.10 us | +0.20 us | +0.8 % | 389 | 310 |

The standard error of each pooled mean difference is 0.03 to 0.05 us, so the pooled B - A offset is not zero: B sits a fraction of a microsecond above A in 72 % of paired runs (the control A' in 57 %). But the offset is a third of a microsecond on a 26-29 us statement, and none of it is carried by a single cell, as the full matrix shows (`B - A median [min, max]` is the spread of the 30 per-run differences of one cell; the per-cell medians are all positive and between +0.18 and +0.50 us):

| statement | path | rows | A | A' | B | B - A median [min, max] | % of A | A' - A median | B - A' median | B above A |
|---|---|---|---|---|---|---|---|---|---|---|
| INSERT | literal | 200 | 26.22 us | 26.01 us | 26.47 us | +0.26 us [-0.58, +1.62] | +1.0 % | -0.06 us | +0.45 us | 22 of 30 |
| INSERT | literal | 1,000 | 26.11 us | 26.14 us | 26.39 us | +0.48 us [-1.19, +1.49] | +1.8 % | -0.04 us | +0.31 us | 22 of 30 |
| INSERT | literal | 10,000 | 26.19 us | 26.30 us | 26.61 us | +0.28 us [-1.39, +1.30] | +1.1 % | -0.05 us | +0.45 us | 19 of 30 |
| INSERT | prepared | 200 | 25.48 us | 25.35 us | 25.74 us | +0.28 us [-0.24, +1.58] | +1.1 % | -0.10 us | +0.61 us | 24 of 30 |
| INSERT | prepared | 1,000 | 25.57 us | 25.66 us | 25.80 us | +0.50 us [-0.96, +1.84] | +2.0 % | +0.03 us | +0.32 us | 23 of 30 |
| INSERT | prepared | 10,000 | 25.61 us | 25.58 us | 25.88 us | +0.27 us [-1.39, +1.29] | +1.1 % | -0.10 us | +0.42 us | 19 of 30 |
| UPDATE indexed col | literal | 200 | 29.21 us | 29.30 us | 29.55 us | +0.42 us [-0.60, +1.76] | +1.4 % | +0.11 us | +0.23 us | 23 of 30 |
| UPDATE indexed col | literal | 1,000 | 29.07 us | 29.38 us | 29.52 us | +0.43 us [-0.88, +1.63] | +1.5 % | +0.33 us | +0.20 us | 23 of 30 |
| UPDATE indexed col | literal | 10,000 | 29.57 us | 29.84 us | 29.90 us | +0.40 us [-0.89, +1.99] | +1.4 % | +0.31 us | +0.12 us | 23 of 30 |
| UPDATE indexed col | prepared | 200 | 28.47 us | 28.62 us | 28.88 us | +0.37 us [-0.79, +1.79] | +1.3 % | +0.08 us | +0.23 us | 23 of 30 |
| UPDATE indexed col | prepared | 1,000 | 28.29 us | 28.47 us | 28.82 us | +0.35 us [-0.98, +1.85] | +1.3 % | +0.23 us | +0.16 us | 21 of 30 |
| UPDATE indexed col | prepared | 10,000 | 29.06 us | 29.11 us | 29.28 us | +0.32 us [-0.65, +1.74] | +1.1 % | +0.21 us | +0.19 us | 22 of 30 |
| UPDATE other col | literal | 200 | 26.58 us | 26.73 us | 26.95 us | +0.26 us [-0.80, +1.51] | +1.0 % | +0.24 us | +0.18 us | 21 of 30 |
| UPDATE other col | literal | 1,000 | 26.63 us | 26.78 us | 26.93 us | +0.29 us [-0.97, +1.18] | +1.1 % | +0.20 us | -0.01 us | 23 of 30 |
| UPDATE other col | literal | 10,000 | 27.04 us | 27.26 us | 27.41 us | +0.39 us [-0.65, +2.17] | +1.4 % | +0.16 us | +0.13 us | 22 of 30 |
| UPDATE other col | prepared | 200 | 25.99 us | 25.96 us | 26.30 us | +0.18 us [-1.26, +1.58] | +0.7 % | +0.10 us | +0.11 us | 20 of 30 |
| UPDATE other col | prepared | 1,000 | 25.87 us | 26.20 us | 26.15 us | +0.22 us [-0.98, +2.65] | +0.8 % | +0.15 us | +0.00 us | 19 of 30 |
| UPDATE other col | prepared | 10,000 | 26.53 us | 26.74 us | 26.65 us | +0.31 us [-0.74, +1.69] | +1.2 % | +0.15 us | +0.09 us | 20 of 30 |

Reading: no single cell resolves a cost (every cell's 30 differences straddle zero; the A'-versus-A scatter of identical bytes is as wide), and there is no row-count trend (the `INSERT`, independent of the table size, moves +0.26 / +0.48 / +0.28 us across the three sizes, which is the noise between repeats of one cell). The pooled offset is positive and small. How much of it is B and how much is that the arms start in a fixed order within a run (A first, A' second, B third; the engine and the first server of a block both rotate, the start order does not) cannot be separated by this design: the A' - A offset of +0.10 us is the size of the start-order effect, which would put B's own share at +0.2 us. The `INSERT` carries the largest residual over the control (+0.42 us, +1.6 %); the engine's insert path is where BY's code sits closest (an owed-index-tree queue behind a rolled-back create, a slot table that can lose entries), but CLA did not look for the source. It is paid by the whole distribution:

| statement | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s (derived) | server CPU | driver CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| INSERT | literal | A | 59,940 | 42.8 us | 46.3 us | 46.8 us | 56.6 us | 81.6 us | 19,916 | 26.11 us | 24.46 us |
| INSERT | literal | A' | 59,940 | 42.7 us | 46.3 us | 46.9 us | 57.2 us | 80.8 us | 19,916 | 26.14 us | 24.48 us |
| INSERT | literal | B | 59,940 | 42.6 us | 46.5 us | 47.2 us | 57.5 us | 81.5 us | 19,793 | 26.39 us | 24.49 us |
| INSERT | prepared | A | 59,940 | 39.3 us | 43.6 us | 44.1 us | 54.4 us | 77.6 us | 20,713 | 25.57 us | 22.36 us |
| INSERT | prepared | A' | 59,940 | 39.9 us | 43.7 us | 44.3 us | 54.4 us | 77.5 us | 20,614 | 25.66 us | 22.39 us |
| INSERT | prepared | B | 59,940 | 39.4 us | 44.0 us | 44.6 us | 54.6 us | 77.9 us | 20,509 | 25.80 us | 22.41 us |
| UPDATE indexed col | literal | A | 59,940 | 42.1 us | 48.9 us | 49.5 us | 60.3 us | 80.9 us | 18,885 | 29.07 us | 24.59 us |
| UPDATE indexed col | literal | A' | 59,940 | 42.1 us | 49.1 us | 49.8 us | 61.1 us | 81.6 us | 18,786 | 29.38 us | 24.58 us |
| UPDATE indexed col | literal | B | 59,940 | 42.7 us | 49.3 us | 49.9 us | 61.5 us | 81.8 us | 18,710 | 29.52 us | 24.62 us |
| UPDATE indexed col | prepared | A | 59,940 | 41.2 us | 45.9 us | 46.5 us | 56.3 us | 78.7 us | 19,914 | 28.29 us | 22.10 us |
| UPDATE indexed col | prepared | A' | 59,940 | 40.7 us | 46.1 us | 46.7 us | 56.1 us | 78.5 us | 19,654 | 28.47 us | 22.11 us |
| UPDATE indexed col | prepared | B | 59,940 | 41.1 us | 46.2 us | 46.8 us | 57.2 us | 78.8 us | 19,762 | 28.82 us | 22.10 us |
| UPDATE other col | literal | A | 59,940 | 40.2 us | 46.8 us | 47.4 us | 57.0 us | 76.9 us | 19,787 | 26.63 us | 24.57 us |
| UPDATE other col | literal | A' | 59,940 | 40.8 us | 46.9 us | 47.5 us | 57.0 us | 78.5 us | 19,730 | 26.78 us | 24.60 us |
| UPDATE other col | literal | B | 59,940 | 39.7 us | 47.0 us | 47.6 us | 57.1 us | 77.2 us | 19,690 | 26.93 us | 24.56 us |
| UPDATE other col | prepared | A | 59,940 | 38.8 us | 44.0 us | 44.5 us | 53.3 us | 74.4 us | 20,800 | 25.87 us | 22.14 us |
| UPDATE other col | prepared | A' | 59,940 | 38.4 us | 44.0 us | 44.6 us | 53.5 us | 75.1 us | 20,748 | 26.20 us | 22.11 us |
| UPDATE other col | prepared | B | 59,940 | 39.0 us | 44.1 us | 44.6 us | 53.8 us | 75.3 us | 20,759 | 26.15 us | 22.12 us |

(1,000-row group, 30 runs each, medians over runs of each run's percentile; `bn_census.py` emits p90, not p95, so the five percentiles of rule 6 are p0, p25, p50, p90 and p99 here. Throughput is derived, `1e6 / mean wall us`.) A, A' and B are the same distribution at every percentile of every shape (p0 to p99 within 1.6 us, mixed sign) and B's derived throughput is 0.2 % to 1.0 % below A's.

## 3. Cell 2: REINDEX on a churned index

### 3.1 What the rebuild leaves, and what it frees

The rebuild drops every entry that belongs to no version the relation still holds, in every row-set size, and the tree shrinks in proportion (old : new = 4.0 : 1 at 3x surplus). The `up` rows are the cells that could not be rebuilt (section 5): the index still carries its surplus and the statement was refused with the old index untouched, which is BY-R7 holding (entries, height and root page id unchanged after the refusal; the one page the failed build had taken freed, `pages_freed` +1).

| live rows n | churn | entries before | entries after | height before -> after | old tree (pages freed) | new tree (pages allocated) | REINDEX wall | REINDEX to pages freed |
|---|---|---|---|---|---|---|---|---|
| 200 | rolled-back INSERTs (3n) | 800 | 200 | 2 -> 1 | 4 pages | 1 page | 0.2 ms | 7.5 s |
| 200 | DELETE + PURGE (3n) | 800 | 200 | 2 -> 1 | 4 pages | 1 page | 0.2 ms | 10.0 s |
| 200 | one every-row UPDATE (n) | 400 | 400 (refused) | 1 -> 1 | n/a | n/a | refused, section 5 | n/a |
| 1,000 | rolled-back INSERTs (3n) | 4,000 | 1,000 | 2 -> 2 | 17 pages | 5 pages | 0.7 ms | 7.5 s |
| 1,000 | DELETE + PURGE (3n) | 4,000 | 1,000 | 2 -> 2 | 17 pages | 5 pages | 0.7 ms | 10.0 s |
| 1,000 | one every-row UPDATE (n) | 2,000 | 2,000 (refused) | 2 -> 2 | n/a | n/a | refused, section 5 | n/a |
| 10,000 | rolled-back INSERTs (3n) | 40,000 | 10,000 | 2 -> 2 | 168 pages | 42 pages | 6.3 ms | 6.5 s |
| 10,000 | DELETE + PURGE (3n) | 40,000 | 10,000 | 2 -> 2 | 168 pages | 42 pages | 6.3 ms | 10.0 s |
| 10,000 | one every-row UPDATE (n) | 20,000 | 20,000 (refused) | 2 -> 2 | n/a | n/a | refused, section 5 | n/a |
| 25,000 | rolled-back INSERTs (3n) | 100,000 | 25,000 | 3 -> 2 | 421 pages | 105 pages | 16.2 ms | 9.5 s |
| 25,000 | DELETE + PURGE (3n) | 100,000 | 25,000 | 3 -> 2 | 421 pages | 105 pages | 15.5 ms | 10.0 s |
| 25,000 | one every-row UPDATE (n) | 50,000 | 50,000 (refused) | 2 -> 2 | n/a | n/a | refused, section 5 | n/a |
| 100,000 | rolled-back INSERTs (3n) | 400,000 | 100,000 | 3 -> 3 | 1,682 pages | 421 pages | 64.8 ms | 12.0 s |
| 100,000 | DELETE + PURGE (3n) | 400,000 | 100,000 | 3 -> 3 | 1,682 pages | 421 pages | 63.4 ms | 9.5 s |
| 100,000 | one every-row UPDATE (n) | 200,000 | 200,000 (refused) | 3 -> 3 | n/a | n/a | refused, section 5 | n/a |

The build is linear in the live rows (0.64 us per row at 100,000, 0.63 at 10,000, 0.65 at 25,000, 0.7 and 1.0 us per row at the two smallest sizes, where the statement's fixed cost shows) and does not depend on how much surplus there was: the build reads the relation, not the old tree. A rolled-back-insert surplus and a purged-row surplus give the same entry count, page count and build time at every size, so the two ways of making a surplus are the same thing to the rebuild. "REINDEX to pages freed" is the poll of `SHOW META` from the end of the `REINDEX` statement until `reclaim_pending` is 0 and `pages_freed` has risen, at 0.5 s granularity: 6.5 to 12 s, one sample per cell (section 4 shows why).

### 3.2 A probe costs the same before and after

A probe is `SELECT * FROM x WHERE v = ?` through the index, server CPU per statement, medians over 3 repetitions of 6,000 probes; "a" is the rebuilt twin, "b" the untouched control, and "a - b" is the effect of the rebuild net of whatever moved both (the "after" phase runs 1-2 s after the "before" phase on the same server, and the whole relation set drifted by -0.5 to +0.8 us between the two). The `up` rows are the in-run control for the whole table: neither twin was rebuilt there, and their "a - b" spans -0.14 to +0.38 us.

| live rows n | churn | twin a (rebuilt) before | twin a after | a: after - before | twin b (control) before | twin b after | b: after - before | a - b | probes per relation per phase | refused/odd replies |
|---|---|---|---|---|---|---|---|---|---|---|
| 200 | rolled-back INSERTs (3n) | 30.60 us | 30.74 us | +0.13 us | 30.53 us | 31.15 us | +0.61 us | -0.48 us | 18,000 | 0 |
| 200 | DELETE + PURGE (3n) | 30.59 us | 30.71 us | +0.12 us | 30.57 us | 30.86 us | +0.29 us | -0.17 us | 18,000 | 0 |
| 200 | one every-row UPDATE (n), not rebuilt | 31.55 us | 31.93 us | +0.38 us | 31.46 us | 31.93 us | +0.47 us | -0.09 us | 18,000 | 0 |
| 1,000 | rolled-back INSERTs (3n) | 30.45 us | 30.99 us | +0.55 us | 30.33 us | 30.78 us | +0.44 us | +0.10 us | 18,000 | 0 |
| 1,000 | DELETE + PURGE (3n) | 30.34 us | 30.93 us | +0.59 us | 30.29 us | 30.83 us | +0.54 us | +0.05 us | 18,000 | 0 |
| 1,000 | one every-row UPDATE (n), not rebuilt | 30.53 us | 31.22 us | +0.69 us | 30.55 us | 31.37 us | +0.82 us | -0.14 us | 18,000 | 0 |
| 10,000 | rolled-back INSERTs (3n) | 30.46 us | 30.17 us | -0.29 us | 30.52 us | 30.09 us | -0.43 us | +0.14 us | 18,000 | 0 |
| 10,000 | DELETE + PURGE (3n) | 30.60 us | 30.16 us | -0.44 us | 30.50 us | 30.09 us | -0.41 us | -0.03 us | 18,000 | 0 |
| 10,000 | one every-row UPDATE (n), not rebuilt | 30.57 us | 30.03 us | -0.54 us | 30.59 us | 30.12 us | -0.47 us | -0.07 us | 18,000 | 0 |
| 25,000 | rolled-back INSERTs (3n) | 30.82 us | 30.80 us | -0.01 us | 30.71 us | 30.79 us | +0.08 us | -0.09 us | 18,000 | 0 |
| 25,000 | DELETE + PURGE (3n) | 30.77 us | 30.76 us | -0.01 us | 30.68 us | 30.61 us | -0.07 us | +0.06 us | 18,000 | 0 |
| 25,000 | one every-row UPDATE (n), not rebuilt | 30.50 us | 30.67 us | +0.18 us | 30.67 us | 30.47 us | -0.20 us | +0.38 us | 18,000 | 0 |
| 100,000 | rolled-back INSERTs (3n) | 31.31 us | 31.18 us | -0.13 us | 31.40 us | 31.41 us | +0.00 us | -0.13 us | 18,000 | 0 |
| 100,000 | DELETE + PURGE (3n) | 31.27 us | 31.33 us | +0.06 us | 31.31 us | 31.34 us | +0.03 us | +0.03 us | 18,000 | 0 |
| 100,000 | one every-row UPDATE (n), not rebuilt | 31.44 us | 31.31 us | -0.14 us | 31.30 us | 31.07 us | -0.23 us | +0.09 us | 18,000 | 0 |

The rebuild moves a probe by **-0.48 to +0.14 us** over the ten rebuilt cells, inside the control's own spread: it is not a finding in either direction, including at 25,000 rows where the height falls from 3 to 2 (a - b of -0.09 us and +0.06 us). The latency distribution at that cell:

| relation | phase | ops | p0 | p25 | p50 | p90 | p99 | probes/s (derived) | server CPU |
|---|---|---|---|---|---|---|---|---|---|
| a (rebuilt, height 3 -> 2) | before | 18,000 | 54.9 us | 57.2 us | 58.0 us | 62.4 us | 74.7 us | 16,921 | 30.82 us |
| a (rebuilt, height 3 -> 2) | after | 18,000 | 54.5 us | 57.0 us | 57.8 us | 63.6 us | 75.9 us | 16,936 | 30.80 us |
| b (control, height 3) | before | 18,000 | 55.0 us | 57.1 us | 57.9 us | 62.0 us | 74.2 us | 16,972 | 30.71 us |
| b (control, height 3) | after | 18,000 | 54.7 us | 57.0 us | 57.8 us | 62.8 us | 75.1 us | 16,963 | 30.79 us |

## 4. Cell 3: what DROP INDEX returns

`DROP INDEX` frees nothing itself, and its tree comes back in full after two checkpoint runs; the same-size `CREATE INDEX` that follows takes the same pages (same root page id), and the data file does not grow.

| live rows n | index height | tree pages (first CREATE allocated) | pages freed at the DROP statement | reclaim pending right after DROP | pages freed by the job | wait from DROP to freed | checkpoint runs in the wait | second CREATE: allocated / reused | root page, first -> second | data file bytes: before / after first CREATE / after reclaim / after second CREATE |
|---|---|---|---|---|---|---|---|---|---|---|
| 200 | 1 | 1 | 0 | 1 | 1 | 9.5 s | 2 | 1 / 1 | 137 -> 137 | 1,572,864 / 1,572,864 / 1,572,864 / 1,572,864 |
| 1,000 | 2 | 5 | 0 | 1 | 5 | 9.5 s | 2 | 5 / 5 | 151 -> 151 | 1,572,864 / 1,572,864 / 1,572,864 / 1,572,864 |
| 10,000 | 2 | 42 | 0 | 1 | 42 | 9.5 s | 2 | 42 / 42 | 219 -> 219 | 2,097,152 / 2,621,440 / 2,621,440 / 2,621,440 |
| 100,000 | 3 | 421 | 0 | 1 | 421 | 10.0 s | 2 | 421 / 421 | 1255 -> 1255 | 7,340,032 / 11,010,048 / 11,010,048 / 11,010,048 |

The pages returned equal the pages the `CREATE INDEX` allocated at every size (1 / 5 / 42 / 421), `reclaim_refused`, `reclaim_skipped` and `reclaim_deferred` stayed 0, and `reclaim_pending` was 1 from the `DROP` until the job ran. The data file grows in chunks (1.5 MiB at the small sizes; 7.3 MiB to 11.0 MiB over the 100,000-row `CREATE INDEX`, which is 421 pages = 3.3 MiB, the file growing 3.5 MiB) and does not move across the drop, the reclaim or the second create. Build cost of the `CREATE INDEX` is the same both times (0.26 / 0.73 / 5.8 / 58 ms against 0.27 / 0.69 / 5.7 / 56 ms): a reused page costs no more than a fresh one here.

## 5. The defect: a relation with updated rows cannot be indexed

`CREATE INDEX` and `REINDEX INDEX` read the relation through the same backfill, and it is refused `Corruption` (wire category 5), `building index 'ix_up_a': undo record at 5932 has type 0`, whenever the relation's rows carry undo chains it cannot follow. Isolated on a fresh server, with the index created after the churn so that REINDEX is not involved:

| binary | rows | `UPDATE x SET v = v + 10000000` rounds, then `CREATE INDEX` | result |
|---|---|---|---|
| A (`5f610082`) | 200 | 1 | created |
| A (`5f610082`) | 200 | 2, 3 | refused, `undo record at 980 has type 0` (2); `... at 6448 ...` (3) |
| B (`9da66f0e`) | 200 | 1 | created |
| B (`9da66f0e`) | 200 | 2, 3 | refused, same messages |
| B | 1, 5, 20, 50 | 2 | created |
| B | 100, 150, 200 | 2 | refused |
| B | 1,000 and 10,000 | 1 | created |
| B | 200 | 2, `durability` = `relaxed`, `group`, `strict` | refused in each; waiting 8 s (a checkpoint cadence and more) before the `CREATE INDEX` changes nothing |

In the REINDEX matrix a single `UPDATE` round was refused at every size (type 0 at 200, 1,000, 10,000, 25,000 and 100,000 rows), where the isolated cell above creates the index at 200, 1,000 and 10,000 rows: the `up` relations were churned first, the rolled-back and purged churns ran after them on the same server, and the `REINDEX` attempts came last, so whatever recycles the undo page was reached in this order and not in the isolated one. The order was not varied. A refused `REINDEX` leaves the old index serving (entries, height, root page id unchanged; checked) and gives back the tree it started, so the failure is clean, but **the cases REINDEX exists for, an index grown by `UPDATE`s, are the ones it cannot be run on until this is fixed**. The error text is `UndoPageRead` (`src/txn/undo_page.cpp:321`) finding an `undo_ptr` that points into a page which no longer holds a record; `worktree-fix-backfill-walk-horizon` (`796ecf49`) states the cause as the backfill following a row's undo chain past the purge horizon into a recycled page, which matches. This file does not rest on that commit; it was not built or measured.

Consequence for the measurement: the "one every-row UPDATE" rows of sections 3.1 and 3.2 carry the before state only, and the question the BY work order poses for update churn ("a version still on an undo chain keeps its entry") is unmeasured: how many of an `UPDATE`-grown index's entries a rebuild removes is not known from this run.

## 6. Against the last number for the shape

`results-bx-close-v3.2.0-62-g9bf559e2.md` is the last result for these statements (its B arm, `9bf559e2`, is an engine with an ordinary index and the BX code; 1,000-row group, medians of 10 runs, server CPU per statement). This run's A is `5f610082`, 53 commits later; its B is `9da66f0e`, 4 commits after that. The builds, the host's state and the day differ, so the columns are a series, not a paired comparison:

| statement | path | `v3.2.0-62-g9bf559e2` (BX close, B) | `v3.2.0-115-g5f610082` (A) | `v3.2.0-119-g9da66f0e` (B) |
|---|---|---|---|---|
| INSERT | literal | 25.62 us | 26.11 us | 26.39 us |
| INSERT | prepared | 24.91 us | 25.57 us | 25.80 us |
| UPDATE indexed col | literal | 28.92 us | 29.07 us | 29.52 us |
| UPDATE indexed col | prepared | 27.96 us | 28.29 us | 28.82 us |
| UPDATE other col | literal | 26.51 us | 26.63 us | 26.93 us |
| UPDATE other col | prepared | 26.07 us | 25.87 us | 26.15 us |

Across the 57 commits between the first and the last column the six statements moved by +0.1 to +0.9 us; the 53-commit span before BY opened accounts for -0.2 to +0.7 us of it and BY's four commits for +0.2 to +0.5 us (the latter pooled and controlled in section 2), and CLA has no host-state record of the BX close run beyond its own file. Inside this run, with the controls above, BY's share is the +0.2 us of section 2.

## 7. Where the time goes

A statement in the A/B is about 44-50 us of client-observed wall, of which the **server's CPU is 25-30 us** (relaxed durability: no fsync wait, none of the WAL waits of a `group` or `strict` commit), the **driver's own CPU is 22-25 us**, and the remainder is the socket round trip and the hand-off between the two pinned CPUs. The +0.2 to +0.3 us this run resolves is about 1 % of the server's share. A probe in cell 2 is about 31 us of server CPU and 55-58 us of wall (p50), of which a tree level is not resolvable. A `REINDEX` is one autocommit statement holding the relation's `X`: its wall is the build itself (0.2 ms to 65 ms), with no fsync wait under `relaxed` and no lock wait with a single session; what follows it, the reclaim, is not on the statement's path but is a wait for the **checkpoint cadence** (9.5 to 10 s in cell 3, two runs of the checkpoint at its 5 s default; 6.5 to 12 s after a `REINDEX`, where the statement's end falls at a random phase of the cadence). The split of the 0.64 us per row of the build between the relation walk, the undo-chain walk and the tree inserts is not measured (no sampler run, the stage budget).

## 8. What it says about the engine

1. **BY adds no resolvable cost to the ordinary write path.** Pooled, a statement on A is +0.1 us from its own identical twin and B is +0.3 us from A, i.e. 0.2 us (0.8 %) over the control; the result is a bound ("about 1 to 2 %"), not a zero, and the `INSERT` is the shape that carries most of it. This agrees with the BY work order's header, which says BY "changes nothing on the read or write path of a live index".
2. **A rebuild is a space reclaim and nothing else, here.** The tree falls in proportion to the surplus (4.0x for 3x surplus) and the probe cost does not move with it, not at 25,000 rows where a tree level goes away. With a 512 MiB pool holding every page, the engine's probe is protocol- and parse-bound (about 31 us of 55-58 us wall) and not tree-bound; the benefit that a smaller tree should bring to a cold pool or a smaller pool is unmeasured and is the cell to run next. The append-only maintenance the work order calls IX2 therefore costs memory and file space in this run, not probe time.
3. **A REINDEX temporarily needs the old and the new tree, and the old one is not free for 6 to 12 s.** At n = 100,000 and 3x surplus the build allocated 421 pages while 1,682 more stayed held until two checkpoint runs had passed, so the peak is old plus new, a quarter again as large as the old tree here (a volume too full to hold both was not tried).
4. **`DROP INDEX` reclaim is checkpoint-gated and exact.** Zero pages at the statement, all of them (and only them: the pages returned equal the pages the create took) two checkpoint runs later, reused by the next create at the same root page id. That is BY-R3/R4 doing what the work order says; the 9.5 to 10 s is the redo-start gate of BF-R4 at the default cadence, so an operator who reads `allocated_pages` straight after a `DROP INDEX` will see nothing and should wait for `reclaim_pending` to reach 0.
5. **The workload REINDEX is for does not yet work.** Section 5. This is not BY's defect (it is on A), but it makes the "grown by `UPDATE`s" half of BY-Q6's measurement impossible today, and `CREATE INDEX` on any relation whose rows were updated twice is affected on both binaries. No spec was read for this file, so no stated expectation is confirmed or contradicted here.

## Reproduce

    # binaries: cp <detached checkout>/build-rel/kds_server $RUN/bin/kds_A ; kds_A2 (same bytes) ; kds_B
    python3 tools/bx_ab.py --arm A=$RUN/bin/kds_A --arm A2=$RUN/bin/kds_A2 --arm B=$RUN/bin/kds_B \
        --workdir $RUN/wd --runs 30 --rows {200|1000|10000} --port 31960 --out q<rows>.json --progress pq<rows>.txt
    python3 by_reclaim.py part2 <n> 6000 6 3     # n = 200 1000 10000 25000 100000; REINDEX cells
    python3 by_reclaim.py part3 200,1000,10000,100000   # DROP INDEX cells
    python3 repro_undo.py $RUN/bin/kds_B 200 2 noidx    # section 5

The raw per-run JSON (`q200.json`, `q1000.json`, `q10000.json`, `part2_<n>.json`, `part3.json`), the watchdog lines (`watch3.txt`, `watch4.txt`, `pq*.txt`, `p3.txt`), the scratch drivers (`by_reclaim.py`, `repro_undo.py`, `repro_undo2.py`) and the analysis scripts are under `/home/cdkbs/bench-runs/by-close/`; a narrower measurement archives nothing in the tree (rule 1b). The discarded busy-host matrix is under `busy/` there.
