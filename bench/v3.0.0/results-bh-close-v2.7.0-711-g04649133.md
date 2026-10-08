# BH's close: the cost of `PURGE` to every other statement, and the cost of a purge

**Thesis.** BH adds one statement, `PURGE FROM t WHERE <pk window>`, and puts one
text compare on every other statement's head path. Measured in `build-release`
against the commit BH opened at, **no point statement (INSERT, SELECT, UPDATE,
DELETE by pk) moves by more than its noise at `cores = 1` or `cores = 2`, at 200,
1,000 or 10,000 rows**, and a purge costs one statement round trip plus 6 µs per
key: 17,200-18,100 keys/s one key at a time, 162,000-175,000 keys/s in windows,
and ~710 keys/s one key at a time once the commit has to be durable, where the
fsync is 95 % of the statement. One scan shape (`COUNT/MIN/MAX` over a 10,000-row
relation) measured **13 % slower in B in the first `cores = 1` series**, which a
re-run of the same pair an hour later did not reproduce (8 % faster), and which a
stage binary with no scan-path change also does not show; section 2 states it plainly. The
run also found that **a purged key cannot always be placed again** (section 5).

- **A** = `10593366` (`v2.7.0-706-g10593366`), the commit BH opened at.
- **B** = `04649133` (`v2.7.0-711-g04649133`), BH closed: the code of BH-S4's
  `469e0b92` plus comment-only edits.
- Order: `instructions/v3.0.0/workorder-bh-purge-key.md` §6. Raw output:
  `archive/bh-close-v2.7.0-711-g04649133/`.

| Item | Value |
|---|---|
| Executed | 2026-10-08, 10:13-11:33 UTC |
| Branch / worktree | `worktree-bh-purge-key`, worktree `bh-purge-key` |
| B | `04649133`, `v2.7.0-711-g04649133`, committed 2026-10-08 10:09:00 UTC. The tree was clean when B was built (`git status --short` empty at 10:11 UTC, after the build). It has since acquired uncommitted edits by another actor (12 files, among them `src/server/command_dispatcher.cpp`); **those are not in B and were not measured** |
| A | `10593366`, `v2.7.0-706-g10593366`, built from a `git archive` export under the session scratchpad |
| Binaries | Release (`CMAKE_BUILD_TYPE` read back from both `CMakeCache.txt`), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`. B: `build-release/kds_server`, mtime 2026-10-08 10:11:28 UTC, i.e. **newer than HEAD** (10:09:00). A: mtime 10:11:28 UTC |
| Measured copies (`archive/.../binaries.txt`) | A `d136d1e8dee6f8c18ec19abe250130d78c04f8a9e7b4cfd932bb8685f6203cc8`, B `465dc191e229db095ae5bcf6ce7d411da1fe365bb498191dc6998b50122ea170`. Every server ran from a copy under `/home/cdkbs/bench-runs/bh-close/bin/` |
| Stage binaries (section 2 only) | BH-S1 `80a0c223` `701bdce2...c5c7`, BH-S2 `bbc7aa3e` `26f5a1cb...f712`, BH-S3 `26766699` `2e4c7ac7...2d1f`, built the same way. BH-S4 was not built: B's code is S4's |
| Host | 8 vCPU (AMD EPYC 9V74, 2 threads per core, 32 MiB L3), Linux 7.0.0-1014-azure. Server pinned to CPU 2 (`cores = 1`) or CPUs 2,3 (`cores = 2`); the driver to CPU 4 (the latency driver to CPU 5, its readers to CPUs 4, 6, 7) |
| Device | data files and WAL under `/home/cdkbs/bench-runs/bh-close/` on `/dev/root`, **ext4** (`df -T`), not tmpfs |
| Server config | `buffer_pool_frames = 65536` (both binaries refuse to boot without it), `log_level = warn`, roles off (default), `cores = 1` or `2`, `durability = relaxed` for the overhead A/B; the purge cells add `strict` and `group`. Fresh server and fresh data file per (cell, run) |
| Ports | 31711/31712 (A/B), 31721 (purge throughput), 31741 (purge latency). Never the default; 15432 (autotrade) and 25432 (xrock) are held by other sessions' servers, which were left running and idle |
| Host load | `/proc/loadavg` and `pgrep -a -f "cc1plus\|cmake --build\|ctest"` before and after every run, in each run's JSON. Loadavg (1 min) in the 340 archived run records: 0.72-2.01, of which 0.94-1.0 at `cores = 1` and 1.2-1.7 at `cores = 2` is the driver's own pair. **Two purge passes were discarded because the list showed another session's build or test run beside them** (below). In the archived records the only match is another session's idle shell (`until [ -f build/tests/kds_tests ] ...`, pid 1278524), whose command line contains the pattern and which is not a build |
| Suite | **Not executed.** This stage changes no code, so there was no before/after to run it over |

## How it was run

**Rule 5 of `bench/README.md` is broken, and why.** `tools/` has no driver for
`PURGE` and none that sets `buffer_pool_frames` or measures `DELETE` by pk, so
the close's agent wrote scratch drivers beside the archive
(`archive/.../drivers/`): `bh_overhead_ab.py` is
`archive/be-close-.../drivers/be_overhead_ab.py` with a `DELETE`-by-pk arm and a
`--cores` flag; `bh_purge_tp.py` and `bh_purge_latency.py` are new. `bench/docs/`
is closed (`bench/README.md`), so the exact commands are in the archive's
`drivers/run_*.sh` (`run_purge_all.sh`'s first half is the discarded second purge pass; its
latency cells are the ones reported, and `run_purge_final.sh` is the reported throughput pass).

- **Overhead A/B** (`bh_overhead_ab.py --rows 200,1000,10000 --runs 16 --cores N`).
  One client connection, serial. Per (rows, run): a fresh A server and a fresh
  B server, five relations `(id int64, v int64, w int64) BTREE` loaded with
  `--rows` named ascending keys in 500-row INSERTs, then seven arms of 3,000
  statements each, run in 12 blocks of 250 that alternate which server goes first.
  The first server in a run alternates too. `--rows` is the row count the
  relation holds when an arm starts: 200, 1,000 or 10,000. Equal work: both
  servers run the same 3,000 statements per arm.
  - `insert` and `insert-again` (a second relation, same shape: the replicate
    that sets the noise floor), `select-pk` and `update-pk` (key
    `1 + q * 7919 mod rows`), `delete-pk` (each block first INSERTs its 250 fresh
    keys, untimed, then times 250 `DELETE ... WHERE id = k`), `range100`
    (`SELECT COUNT(*), MIN(id), MAX(id) ... WHERE id >= lo AND id < lo + 100`,
    which **walks the whole relation**: 87 µs at 200 rows, 810-870 µs at 10,000)
    and `ping` (`SHOW META`, the control no BH path can touch).
  - 16 runs per size and cores, so 48,000 statements per arm per shape pooled.
  - The delta is the **median of the per-run p50 deltas B - A**, with a
    bootstrap 95 % CI (4,000 resamples of the 16 runs); the mean delta is the
    mean of per-run mean deltas. Throughput is the driver's `ops / elapsed` per
    run; its delta is the median per-run ratio.
  - The `cores = 2` series: the n=200 runs are from the first invocation, whose
    `--max-load 1.0` gate then waited 300 s per run at `cores = 2` (the driver's
    own load average is ~1.3 there). It was stopped and n=1000 and n=10000 were
    redone with `--max-load 1.6`. The two invocations use the same binaries and
    flags.
- **Purge throughput** (`bh_purge_tp.py --cells window:keys`, B only). Per run a
  fresh server; the relation loaded with `keys` rows; then every key is
  `DELETE`d (timed per statement) and every key `PURGE`d (timed per
  statement), one key per statement (`window = 1`) or `window` keys per
  statement (`BETWEEN a AND a+window-1`). Row-set size: the one-key cells run at
  200, 1,000 and 10,000 keys; the window cells at windows of 200, 1,000 and
  10,000 over a 30,000-row relation.
  **Verification is on in every run:** the key is refused while bound
  (`INSERT` of key 1 after the DELETE), every `PURGE` must reply `PURGED <window>`,
  `SELECT COUNT(*)` must read 0 after, and every key is then `INSERT`ed again.
- **DELETE-to-PURGE latency** (`bh_purge_latency.py`, B only): a 60,000-row relation, 20,000
  samples per rep, 3 reps per cell. Background sessions are separate processes:
  readers run autocommit `SELECT * ... WHERE id = <uniform 1..60000>` back to back
  (so up to 1 in 3 reads, 1 in 6 on average over a rep, names a key already deleted or purged), the writer runs
  autocommit `UPDATE ... WHERE id = <upper half>`. The main session, for k = 1..20,000, sends
  `DELETE ... WHERE id = k` and then `PURGE ... WHERE id = k`, retried on
  `TXN_CONFLICT`; the sample is the time from the `DELETE`'s reply to the
  successful `PURGE`'s reply. The snapshot probe is the same without background
  load, with a second session `BEGIN`, one read, and a `COMMIT` H ms after the DELETE.
- **Stage bisection** (section 2): the same overhead driver, A against the BH-S1, S2
  and S3 binaries and against B again, `cores = 1` at 1,000 and 10,000 rows and
  `cores = 2` at 10,000 rows, 10 runs each.

Three purge-throughput passes were run and **the tables below are the last**.
The first checked the re-insert with 500-row INSERT statements; the check failed on
a defect (section 5), and two of its runs had another session's `cmake --build -j8`
beside them. The second (loadavg 1.4-1.6) changed the check, and **every one of its
run records lists another session's `ctest` (pid 2028528, worktree `bb-s5-close`)**;
it measured the one-key cells at `cores = 1` 11-13 % lower (15,500-15,700 keys/s
against 17,400-18,100), which is what a test suite on the other CPUs costs a pinned
pair here. Both were discarded and are not archived. The third ran at loadavg
~1.0 with the list clear, and a 10-run repeat of two cells (`purge-tp-repeat`)
agrees with it to 0.2 % at `cores = 1` and to 2 % at `cores = 2` on the one-key
cell (the `cores = 2` 1,000-key window is bimodal, below).

## 1. Overhead: no point statement moves

At `cores = 1` and `cores = 2`, at every size, `INSERT`, `SELECT`, `UPDATE` and
`DELETE` by pk and the `SHOW META` control have a p50 delta B - A of **-0.6 to
+0.3 µs (at most 1 % of the statement)**. Most intervals straddle zero. The
ones that do not are all B faster: `SELECT` by pk at 200 rows at `cores = 1`
(-0.55 µs [-0.75, -0.20]) and the control itself at `cores = 1`, 1,000 and 10,000
rows (-0.25 and -0.20 µs). The noise floor measured inside the run is the same
size: the control, which no BH path touches, moves by -0.25 to +0.10 µs, and the
replicate arm (`insert-again` against `insert` on one binary) differs by -0.10
to +0.15 µs at p50 with a per-run sd of 0.1-1.5 µs. **A delta under ~0.6 µs at
p50 is not a finding.** The one other resolved delta is
`range100` at 200 rows: **B +1.00 µs [+0.70, +1.20] (+1.1 %) at `cores = 1` and
+0.75 µs [+0.20, +1.20] (+0.9 %) at `cores = 2`**, a 1 % cost on an 87 µs
relation walk, in the same direction at both core counts and just over the
floor.

Headline matrix: median per-run p50 delta B - A with its 95 % CI, and the median
per-run throughput delta. The second block gives A's p50 in each cell (median of the 16 per-run p50s), which is what the delta is of.

| shape | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| INSERT, pk omitted | -0.15 µs [-0.30, +0.00], +1.4 % qps | -0.10 µs [-0.20, +0.00], +0.8 % qps | -0.20 µs [-0.20, +0.00], +1.0 % qps | -0.05 µs [-0.20, +0.20], +0.1 % qps | +0.05 µs [-0.30, +0.15], +0.4 % qps | -0.10 µs [-0.30, +0.10], +0.5 % qps |
| INSERT again (replicate of the line above) | -0.10 µs [-0.25, +0.05], +0.3 % qps | -0.05 µs [-0.20, +0.00], +0.3 % qps | -0.05 µs [-0.20, +0.10], -0.2 % qps | -0.10 µs [-0.25, +0.10], -0.6 % qps | -0.10 µs [-0.25, +0.20], -1.4 % qps | -0.10 µs [-0.30, +0.10], +0.9 % qps |
| SELECT by pk | -0.55 µs [-0.75, -0.20], +0.9 % qps | -0.60 µs [-0.95, +0.05], +0.5 % qps | -0.25 µs [-0.60, +0.00], +0.2 % qps | -0.50 µs [-0.70, +0.50], +0.7 % qps | -0.25 µs [-0.90, +0.50], +0.4 % qps | +0.20 µs [-0.20, +0.40], -0.1 % qps |
| UPDATE by pk | -0.10 µs [-0.70, +0.10], +0.5 % qps | +0.00 µs [-0.60, +0.40], -0.1 % qps | -0.15 µs [-0.50, +0.10], +1.4 % qps | +0.05 µs [-0.20, +0.20], +0.8 % qps | +0.05 µs [-0.10, +0.30], -0.7 % qps | +0.30 µs [-0.20, +0.90], -0.5 % qps |
| DELETE by pk | +0.00 µs [-0.20, +0.10], +0.6 % qps | -0.25 µs [-0.40, +0.00], +1.1 % qps | -0.10 µs [-0.20, +0.00], +0.5 % qps | +0.05 µs [-0.40, +0.25], +0.1 % qps | -0.05 µs [-0.25, +0.15], -0.4 % qps | +0.00 µs [-0.10, +0.30], +0.0 % qps |
| COUNT/MIN/MAX over 100 ids (scans the relation) | +1.00 µs [+0.70, +1.20], -0.9 % qps | +10.45 µs [+9.80, +11.30], -6.0 % qps | +107.85 µs [+101.55, +111.90], -10.4 % qps | +0.75 µs [+0.20, +1.20], -0.2 % qps | -6.05 µs [-7.20, -3.95], +2.9 % qps | -69.35 µs [-73.40, -65.10], +8.9 % qps |
| SHOW META (control) | -0.10 µs [-0.20, +0.00], +0.4 % qps | -0.25 µs [-0.30, -0.10], +0.5 % qps | -0.20 µs [-0.30, -0.10], +1.0 % qps | -0.10 µs [-0.20, +0.05], +0.4 % qps | +0.10 µs [-0.40, +0.40], -0.6 % qps | -0.10 µs [-0.50, +0.80], -0.1 % qps |


A's p50 in each cell (median of the 16 per-run p50s):

| shape | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.7 µs | 49.7 µs | 49.8 µs | 49.7 µs | 49.8 µs | 49.7 µs |
| INSERT again (replicate of the line above) | 49.7 µs | 49.7 µs | 49.7 µs | 49.8 µs | 49.6 µs | 49.6 µs |
| SELECT by pk | 66.0 µs | 69.2 µs | 65.2 µs | 67.0 µs | 69.2 µs | 65.5 µs |
| UPDATE by pk | 51.8 µs | 52.0 µs | 52.3 µs | 52.8 µs | 52.7 µs | 53.1 µs |
| DELETE by pk | 49.6 µs | 49.7 µs | 49.6 µs | 49.7 µs | 49.7 µs | 49.5 µs |
| COUNT/MIN/MAX over 100 ids (scans the relation) | 87.3 µs | 148.1 µs | 808.7 µs | 88.0 µs | 153.2 µs | 868.0 µs |
| SHOW META (control) | 45.6 µs | 45.6 µs | 45.7 µs | 45.8 µs | 45.5 µs | 45.5 µs |

Waits in the overhead cells. Every cell is `durability = relaxed`, so
**there is no commit/fsync wait in any overhead cell**. `SHOW META`, which no
engine path adds to, costs 45.5 µs at p50: the client, the socket, the frame
codec and the dispatch, which this engine cannot separate without server-side
timestamps. INSERT (49.7 µs) and DELETE (49.6 µs) are 4 µs above that floor,
UPDATE (52 µs) 6 µs, SELECT by pk (65-69 µs) 20-24 µs. No lock or conflict wait
occurred: 0 errors in 96 runs (2 core counts x 3 sizes x 16 runs), 4.0 million
timed statements. The BH-added compare on a statement's head (the parser's head
chain plus `DispatchInner`) is inside the 4 µs above the floor, and the data
cannot say more than "under the 0.6 µs floor".

Full percentile tables follow. `n` is 48,000 statements per arm and shape,
pooled over 16 runs; the mean column carries the sd of the 16 run means, which
is what the CI on the mean delta is made from. Throughput is the median over
runs of `ops / elapsed` of that arm (derived by the driver, not inverted from a
mean).

### `cores = 1`

**cores = 1, 200 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 0.94-0.97, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 41.4 µs | 48.7 µs | 49.7 µs | 52.6 µs | 69.8 µs | 93.1 µs | 54.7 µs (sd 5.1) | 18,803 qps |
| INSERT, pk omitted | B | 35.7 µs | 48.7 µs | 49.6 µs | 51.7 µs | 68.4 µs | 85.6 µs | 52.5 µs (sd 2.5) | 19,018 qps |
| INSERT again (replicate) | A | 39.4 µs | 48.7 µs | 49.7 µs | 52.1 µs | 68.2 µs | 84.4 µs | 55.2 µs (sd 3.4) | 18,202 qps |
| INSERT again (replicate) | B | 40.9 µs | 48.7 µs | 49.6 µs | 51.6 µs | 67.5 µs | 81.6 µs | 54.6 µs (sd 1.5) | 18,314 qps |
| SELECT by pk | A | 57.7 µs | 64.6 µs | 66.1 µs | 72.3 µs | 87.0 µs | 110.3 µs | 71.7 µs (sd 5.1) | 14,195 qps |
| SELECT by pk | B | 58.1 µs | 64.0 µs | 65.6 µs | 71.6 µs | 86.0 µs | 103.5 µs | 70.3 µs (sd 4.1) | 14,429 qps |
| UPDATE by pk | A | 39.7 µs | 50.7 µs | 51.9 µs | 55.8 µs | 70.9 µs | 85.5 µs | 57.6 µs (sd 1.1) | 17,313 qps |
| UPDATE by pk | B | 39.0 µs | 50.2 µs | 51.7 µs | 55.0 µs | 70.2 µs | 83.5 µs | 57.3 µs (sd 1.2) | 17,443 qps |
| DELETE by pk | A | 40.3 µs | 48.7 µs | 49.6 µs | 52.0 µs | 66.9 µs | 77.9 µs | 54.7 µs (sd 1.3) | 18,317 qps |
| DELETE by pk | B | 40.4 µs | 48.6 µs | 49.6 µs | 51.9 µs | 67.5 µs | 82.2 µs | 55.9 µs (sd 5.6) | 18,462 qps |
| COUNT/MIN/MAX over 100 ids | A | 79.6 µs | 85.8 µs | 87.7 µs | 94.2 µs | 110.3 µs | 128.3 µs | 93.3 µs (sd 4.4) | 10,796 qps |
| COUNT/MIN/MAX over 100 ids | B | 81.0 µs | 86.6 µs | 88.5 µs | 95.4 µs | 111.0 µs | 124.4 µs | 93.0 µs (sd 1.7) | 10,736 qps |
| SHOW META (control) | A | 33.1 µs | 44.9 µs | 45.6 µs | 47.4 µs | 60.6 µs | 72.1 µs | 48.0 µs (sd 2.9) | 21,090 qps |
| SHOW META (control) | B | 31.7 µs | 44.8 µs | 45.5 µs | 47.4 µs | 59.7 µs | 69.2 µs | 47.7 µs (sd 1.9) | 21,142 qps |

| shape (cores = 1, 200 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.15 µs [-0.30, +0.00] | -2.16 µs [-4.72, -0.47] | +1.35 % [+0.46, +1.80] | no cost resolved |
| INSERT again (replicate) | -0.10 µs [-0.25, +0.05] | -0.62 µs [-2.04, +0.38] | +0.34 % [-1.08, +1.61] | no cost resolved |
| SELECT by pk | -0.55 µs [-0.75, -0.20] | -1.46 µs [-4.54, +1.47] | +0.94 % [+0.59, +1.96] | no cost resolved |
| UPDATE by pk | -0.10 µs [-0.70, +0.10] | -0.36 µs [-0.89, +0.26] | +0.47 % [+0.00, +1.89] | no cost resolved |
| DELETE by pk | +0.00 µs [-0.20, +0.10] | +1.23 µs [-1.02, +4.56] | +0.63 % [-0.67, +1.90] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | +1.00 µs [+0.70, +1.20] | -0.26 µs [-2.27, +0.99] | -0.90 % [-1.08, +0.08] | **B slower** (+1.1 % at p50) |
| SHOW META (control) | -0.10 µs [-0.20, +0.00] | -0.25 µs [-0.96, +0.40] | +0.41 % [-0.24, +1.01] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -0.10 µs, sd 1.12 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -0.05 µs, sd 0.32 µs.

**cores = 1, 1,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 0.93-0.96, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 40.5 µs | 48.8 µs | 49.7 µs | 52.4 µs | 68.1 µs | 83.1 µs | 52.6 µs (sd 0.7) | 18,814 qps |
| INSERT, pk omitted | B | 35.9 µs | 48.7 µs | 49.6 µs | 52.1 µs | 67.6 µs | 82.7 µs | 53.1 µs (sd 4.1) | 18,909 qps |
| INSERT again (replicate) | A | 41.3 µs | 48.7 µs | 49.6 µs | 52.0 µs | 67.8 µs | 84.0 µs | 55.6 µs (sd 4.1) | 18,193 qps |
| INSERT again (replicate) | B | 40.7 µs | 48.7 µs | 49.5 µs | 51.6 µs | 67.6 µs | 81.4 µs | 54.9 µs (sd 1.4) | 18,190 qps |
| SELECT by pk | A | 58.6 µs | 65.3 µs | 69.4 µs | 75.1 µs | 92.8 µs | 116.8 µs | 73.9 µs (sd 4.3) | 13,692 qps |
| SELECT by pk | B | 54.3 µs | 64.8 µs | 68.8 µs | 74.7 µs | 91.7 µs | 113.3 µs | 73.2 µs (sd 3.5) | 13,849 qps |
| UPDATE by pk | A | 41.9 µs | 50.7 µs | 52.0 µs | 56.1 µs | 71.8 µs | 89.2 µs | 61.3 µs (sd 4.5) | 16,516 qps |
| UPDATE by pk | B | 39.4 µs | 50.9 µs | 52.0 µs | 55.8 µs | 71.1 µs | 85.7 µs | 60.6 µs (sd 2.1) | 16,432 qps |
| DELETE by pk | A | 40.7 µs | 48.8 µs | 49.7 µs | 53.1 µs | 68.2 µs | 82.2 µs | 56.7 µs (sd 4.7) | 18,036 qps |
| DELETE by pk | B | 41.1 µs | 48.6 µs | 49.5 µs | 52.0 µs | 67.1 µs | 78.8 µs | 54.3 µs (sd 0.9) | 18,319 qps |
| COUNT/MIN/MAX over 100 ids | A | 134.5 µs | 143.4 µs | 148.3 µs | 160.9 µs | 183.1 µs | 224.4 µs | 156.4 µs (sd 7.2) | 6,480 qps |
| COUNT/MIN/MAX over 100 ids | B | 141.8 µs | 152.7 µs | 158.7 µs | 170.8 µs | 189.9 µs | 229.4 µs | 165.8 µs (sd 5.2) | 6,072 qps |
| SHOW META (control) | A | 31.1 µs | 45.0 µs | 45.6 µs | 47.1 µs | 60.0 µs | 69.1 µs | 47.6 µs (sd 1.0) | 20,983 qps |
| SHOW META (control) | B | 31.8 µs | 44.8 µs | 45.4 µs | 46.8 µs | 59.6 µs | 69.8 µs | 48.1 µs (sd 4.3) | 21,176 qps |

| shape (cores = 1, 1,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.20, +0.00] | +0.49 µs [-0.76, +2.68] | +0.76 % [+0.09, +1.15] | no cost resolved |
| INSERT again (replicate) | -0.05 µs [-0.20, +0.00] | -0.78 µs [-2.82, +0.61] | +0.33 % [-0.95, +1.02] | no cost resolved |
| SELECT by pk | -0.60 µs [-0.95, +0.05] | -0.73 µs [-3.06, +1.10] | +0.48 % [-0.25, +1.52] | no cost resolved |
| UPDATE by pk | +0.00 µs [-0.60, +0.40] | -0.68 µs [-2.48, +0.66] | -0.09 % [-1.24, +1.19] | no cost resolved |
| DELETE by pk | -0.25 µs [-0.40, +0.00] | -2.34 µs [-4.86, -0.48] | +1.11 % [-0.15, +4.89] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | +10.45 µs [+9.80, +11.30] | +9.44 µs [+4.22, +14.26] | -5.96 % [-7.07, -5.30] | **B slower** (+7.1 % at p50) |
| SHOW META (control) | -0.25 µs [-0.30, -0.10] | +0.47 µs [-0.86, +2.73] | +0.51 % [-0.36, +2.26] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.23 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -0.05 µs, sd 0.25 µs.

**cores = 1, 10,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 0.85-1.00, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 41.2 µs | 48.8 µs | 49.7 µs | 52.7 µs | 68.8 µs | 86.9 µs | 53.7 µs (sd 3.7) | 18,772 qps |
| INSERT, pk omitted | B | 41.0 µs | 48.7 µs | 49.6 µs | 51.9 µs | 67.1 µs | 81.9 µs | 52.2 µs (sd 0.6) | 18,985 qps |
| INSERT again (replicate) | A | 41.0 µs | 48.6 µs | 49.7 µs | 52.7 µs | 68.8 µs | 87.0 µs | 57.5 µs (sd 3.3) | 17,689 qps |
| INSERT again (replicate) | B | 40.6 µs | 48.7 µs | 49.6 µs | 52.0 µs | 67.8 µs | 84.5 µs | 57.0 µs (sd 2.6) | 17,662 qps |
| SELECT by pk | A | 57.9 µs | 64.0 µs | 65.3 µs | 70.5 µs | 86.1 µs | 102.7 µs | 69.6 µs (sd 3.0) | 14,483 qps |
| SELECT by pk | B | 57.8 µs | 63.9 µs | 65.2 µs | 69.7 µs | 84.7 µs | 102.4 µs | 69.3 µs (sd 3.2) | 14,549 qps |
| UPDATE by pk | A | 40.1 µs | 51.3 µs | 52.3 µs | 56.3 µs | 72.9 µs | 90.5 µs | 58.3 µs (sd 1.8) | 17,198 qps |
| UPDATE by pk | B | 38.9 µs | 51.3 µs | 52.2 µs | 56.0 µs | 71.9 µs | 89.3 µs | 58.9 µs (sd 4.0) | 17,168 qps |
| DELETE by pk | A | 40.8 µs | 48.6 µs | 49.7 µs | 52.7 µs | 68.9 µs | 81.1 µs | 52.8 µs (sd 2.1) | 19,007 qps |
| DELETE by pk | B | 40.9 µs | 48.6 µs | 49.6 µs | 52.0 µs | 67.4 µs | 80.4 µs | 53.2 µs (sd 2.6) | 19,150 qps |
| COUNT/MIN/MAX over 100 ids | A | 712.9 µs | 786.9 µs | 814.0 µs | 843.2 µs | 1,026.1 µs | 1,132.9 µs | 839.4 µs (sd 49.7) | 1,205 qps |
| COUNT/MIN/MAX over 100 ids | B | 798.1 µs | 886.2 µs | 918.2 µs | 951.1 µs | 1,005.4 µs | 1,135.4 µs | 929.4 µs (sd 8.6) | 1,075 qps |
| SHOW META (control) | A | 31.8 µs | 45.0 µs | 45.7 µs | 47.0 µs | 59.9 µs | 69.5 µs | 48.5 µs (sd 4.1) | 21,001 qps |
| SHOW META (control) | B | 31.3 µs | 44.8 µs | 45.5 µs | 46.6 µs | 58.8 µs | 67.3 µs | 46.9 µs (sd 0.6) | 21,184 qps |

| shape (cores = 1, 10,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.20 µs [-0.20, +0.00] | -1.57 µs [-3.54, -0.47] | +0.99 % [+0.75, +1.28] | no cost resolved |
| INSERT again (replicate) | -0.05 µs [-0.20, +0.10] | -0.48 µs [-1.89, +0.86] | -0.18 % [-0.49, +1.49] | no cost resolved |
| SELECT by pk | -0.25 µs [-0.60, +0.00] | -0.34 µs [-1.34, +0.64] | +0.23 % [-0.55, +1.62] | no cost resolved |
| UPDATE by pk | -0.15 µs [-0.50, +0.10] | +0.59 µs [-0.92, +2.96] | +1.37 % [-0.10, +1.91] | no cost resolved |
| DELETE by pk | -0.10 µs [-0.20, +0.00] | +0.32 µs [-0.82, +1.73] | +0.53 % [-0.02, +0.71] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | +107.85 µs [+101.55, +111.90] | +90.06 µs [+62.82, +106.89] | -10.43 % [-11.54, -10.14] | **B slower** (+13.3 % at p50) |
| SHOW META (control) | -0.20 µs [-0.30, -0.10] | -1.56 µs [-3.67, -0.32] | +0.95 % [+0.54, +1.64] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -0.10 µs, sd 0.44 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.28 µs.

### `cores = 2`

**cores = 2, 200 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 0.94-1.00, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 42.2 µs | 48.8 µs | 49.8 µs | 53.9 µs | 69.7 µs | 82.9 µs | 53.2 µs (sd 1.7) | 18,657 qps |
| INSERT, pk omitted | B | 42.2 µs | 48.7 µs | 49.8 µs | 54.5 µs | 71.4 µs | 93.2 µs | 55.2 µs (sd 5.6) | 18,725 qps |
| INSERT again (replicate) | A | 42.3 µs | 48.7 µs | 49.7 µs | 52.8 µs | 68.4 µs | 80.9 µs | 55.4 µs (sd 1.4) | 17,883 qps |
| INSERT again (replicate) | B | 41.7 µs | 48.6 µs | 49.6 µs | 53.2 µs | 68.5 µs | 82.2 µs | 56.2 µs (sd 2.4) | 17,822 qps |
| SELECT by pk | A | 58.6 µs | 65.5 µs | 66.9 µs | 73.4 µs | 87.9 µs | 101.2 µs | 71.0 µs (sd 1.5) | 14,018 qps |
| SELECT by pk | B | 59.3 µs | 65.2 µs | 66.6 µs | 73.2 µs | 87.6 µs | 102.3 µs | 71.4 µs (sd 3.6) | 14,024 qps |
| UPDATE by pk | A | 43.8 µs | 51.8 µs | 52.7 µs | 57.8 µs | 75.4 µs | 241.7 µs | 63.2 µs (sd 14.5) | 16,775 qps |
| UPDATE by pk | B | 40.9 µs | 51.8 µs | 52.7 µs | 57.0 µs | 72.4 µs | 84.4 µs | 59.3 µs (sd 1.6) | 16,734 qps |
| DELETE by pk | A | 38.2 µs | 48.9 µs | 49.7 µs | 53.1 µs | 69.9 µs | 234.9 µs | 58.1 µs (sd 13.2) | 18,138 qps |
| DELETE by pk | B | 42.1 µs | 48.8 µs | 49.7 µs | 52.8 µs | 67.6 µs | 76.3 µs | 55.2 µs (sd 1.9) | 18,156 qps |
| COUNT/MIN/MAX over 100 ids | A | 78.7 µs | 86.6 µs | 88.7 µs | 96.2 µs | 114.3 µs | 178.9 µs | 97.5 µs (sd 10.5) | 10,595 qps |
| COUNT/MIN/MAX over 100 ids | B | 83.5 µs | 87.2 µs | 89.1 µs | 97.2 µs | 113.7 µs | 135.3 µs | 94.7 µs (sd 3.1) | 10,619 qps |
| SHOW META (control) | A | 31.6 µs | 45.1 µs | 45.8 µs | 47.1 µs | 60.1 µs | 70.1 µs | 48.4 µs (sd 3.9) | 21,028 qps |
| SHOW META (control) | B | 33.8 µs | 45.0 µs | 45.7 µs | 47.0 µs | 59.2 µs | 67.1 µs | 47.2 µs (sd 0.4) | 21,031 qps |

| shape (cores = 2, 200 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.05 µs [-0.20, +0.20] | +1.95 µs [-0.04, +4.43] | +0.07 % [-1.61, +0.83] | no cost resolved |
| INSERT again (replicate) | -0.10 µs [-0.25, +0.10] | +0.73 µs [-0.39, +1.94] | -0.58 % [-2.85, +1.06] | no cost resolved |
| SELECT by pk | -0.50 µs [-0.70, +0.50] | +0.48 µs [-0.97, +2.58] | +0.69 % [-1.21, +1.00] | no cost resolved |
| UPDATE by pk | +0.05 µs [-0.20, +0.20] | -3.93 µs [-11.04, +0.26] | +0.75 % [-1.15, +3.45] | no cost resolved |
| DELETE by pk | +0.05 µs [-0.40, +0.25] | -2.90 µs [-9.24, +0.64] | +0.09 % [-0.87, +1.03] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | +0.75 µs [+0.20, +1.20] | -2.85 µs [-8.17, +1.36] | -0.17 % [-1.34, +2.53] | **B slower** (+0.9 % at p50) |
| SHOW META (control) | -0.10 µs [-0.20, +0.05] | -1.23 µs [-3.29, -0.06] | +0.35 % [-0.20, +1.49] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.00 µs, sd 1.38 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median -0.10 µs, sd 1.51 µs.

**cores = 2, 1,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.16-1.60, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 42.2 µs | 48.9 µs | 49.8 µs | 52.4 µs | 68.7 µs | 85.6 µs | 52.9 µs (sd 1.8) | 18,970 qps |
| INSERT, pk omitted | B | 40.6 µs | 48.8 µs | 49.7 µs | 51.6 µs | 68.1 µs | 85.1 µs | 54.5 µs (sd 9.2) | 19,052 qps |
| INSERT again (replicate) | A | 42.7 µs | 48.8 µs | 49.8 µs | 53.0 µs | 70.5 µs | 90.9 µs | 56.1 µs (sd 2.6) | 17,942 qps |
| INSERT again (replicate) | B | 41.9 µs | 48.7 µs | 49.8 µs | 53.4 µs | 71.9 µs | 107.2 µs | 60.9 µs (sd 14.2) | 18,022 qps |
| SELECT by pk | A | 59.5 µs | 66.1 µs | 70.1 µs | 75.9 µs | 95.0 µs | 131.8 µs | 76.2 µs (sd 8.9) | 13,588 qps |
| SELECT by pk | B | 59.0 µs | 65.9 µs | 69.5 µs | 75.6 µs | 94.0 µs | 151.0 µs | 76.8 µs (sd 11.7) | 13,788 qps |
| UPDATE by pk | A | 45.5 µs | 52.0 µs | 53.0 µs | 56.5 µs | 72.4 µs | 87.6 µs | 62.2 µs (sd 6.3) | 16,520 qps |
| UPDATE by pk | B | 45.2 µs | 52.0 µs | 53.0 µs | 56.3 µs | 72.9 µs | 89.0 µs | 64.3 µs (sd 10.5) | 16,276 qps |
| DELETE by pk | A | 40.2 µs | 48.9 µs | 49.8 µs | 52.0 µs | 67.5 µs | 77.6 µs | 55.4 µs (sd 2.0) | 18,145 qps |
| DELETE by pk | B | 43.1 µs | 48.9 µs | 49.8 µs | 52.2 µs | 68.5 µs | 87.8 µs | 60.2 µs (sd 19.2) | 18,170 qps |
| COUNT/MIN/MAX over 100 ids | A | 137.0 µs | 148.1 µs | 153.5 µs | 164.1 µs | 188.2 µs | 257.0 µs | 162.1 µs (sd 9.7) | 6,321 qps |
| COUNT/MIN/MAX over 100 ids | B | 136.2 µs | 144.0 µs | 147.9 µs | 159.1 µs | 183.7 µs | 252.4 µs | 159.6 µs (sd 14.3) | 6,551 qps |
| SHOW META (control) | A | 31.4 µs | 43.0 µs | 45.5 µs | 46.5 µs | 58.3 µs | 66.8 µs | 46.4 µs (sd 1.3) | 21,412 qps |
| SHOW META (control) | B | 32.4 µs | 43.0 µs | 45.5 µs | 46.5 µs | 58.6 µs | 67.2 µs | 46.2 µs (sd 1.4) | 21,547 qps |

| shape (cores = 2, 1,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | +0.05 µs [-0.30, +0.15] | +1.66 µs [-0.69, +5.94] | +0.43 % [+0.04, +0.82] | no cost resolved |
| INSERT again (replicate) | -0.10 µs [-0.25, +0.20] | +4.83 µs [-0.22, +11.76] | -1.36 % [-3.01, +1.00] | no cost resolved |
| SELECT by pk | -0.25 µs [-0.90, +0.50] | +0.59 µs [-5.31, +7.03] | +0.44 % [-0.34, +3.13] | no cost resolved |
| UPDATE by pk | +0.05 µs [-0.10, +0.30] | +2.12 µs [-0.72, +6.88] | -0.67 % [-1.81, +0.52] | no cost resolved |
| DELETE by pk | -0.05 µs [-0.25, +0.15] | +4.88 µs [-0.54, +14.72] | -0.35 % [-1.78, +0.88] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | -6.05 µs [-7.20, -3.95] | -2.46 µs [-5.53, +2.14] | +2.86 % [+1.57, +4.39] | B faster (-3.9 % at p50) |
| SHOW META (control) | +0.10 µs [-0.40, +0.40] | -0.24 µs [-1.11, +0.54] | -0.63 % [-1.84, +2.01] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.62 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.15 µs, sd 1.03 µs.

**cores = 2, 10,000 rows** - 16 runs, 48,000 statements per arm per shape pooled (loadavg before each run 1.41-1.56, 0 errors)

| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |
|---|---|---|---|---|---|---|---|---|---|
| INSERT, pk omitted | A | 42.6 µs | 48.9 µs | 49.7 µs | 51.7 µs | 69.0 µs | 85.2 µs | 52.7 µs (sd 1.3) | 18,842 qps |
| INSERT, pk omitted | B | 42.2 µs | 48.7 µs | 49.6 µs | 51.3 µs | 67.9 µs | 82.8 µs | 52.8 µs (sd 1.7) | 18,872 qps |
| INSERT again (replicate) | A | 43.1 µs | 48.9 µs | 49.6 µs | 51.3 µs | 67.4 µs | 79.3 µs | 57.1 µs (sd 1.1) | 17,369 qps |
| INSERT again (replicate) | B | 42.6 µs | 48.7 µs | 49.5 µs | 51.1 µs | 66.8 µs | 78.9 µs | 57.1 µs (sd 1.2) | 17,326 qps |
| SELECT by pk | A | 59.7 µs | 64.8 µs | 65.7 µs | 67.9 µs | 83.0 µs | 93.8 µs | 68.2 µs (sd 0.9) | 14,565 qps |
| SELECT by pk | B | 58.8 µs | 64.9 µs | 65.7 µs | 68.1 µs | 83.4 µs | 94.5 µs | 68.4 µs (sd 1.0) | 14,522 qps |
| UPDATE by pk | A | 46.2 µs | 52.2 µs | 53.1 µs | 55.5 µs | 70.8 µs | 81.9 µs | 58.3 µs (sd 1.1) | 17,094 qps |
| UPDATE by pk | B | 47.4 µs | 52.5 µs | 53.5 µs | 55.8 µs | 71.3 µs | 82.5 µs | 58.4 µs (sd 1.3) | 17,019 qps |
| DELETE by pk | A | 42.2 µs | 48.9 µs | 49.6 µs | 51.0 µs | 65.6 µs | 73.5 µs | 51.6 µs (sd 0.4) | 19,220 qps |
| DELETE by pk | B | 42.9 µs | 48.9 µs | 49.6 µs | 51.2 µs | 66.2 µs | 74.9 µs | 51.8 µs (sd 0.5) | 19,203 qps |
| COUNT/MIN/MAX over 100 ids | A | 750.8 µs | 831.5 µs | 870.3 µs | 907.6 µs | 954.8 µs | 1,121.3 µs | 878.4 µs (sd 16.2) | 1,140 qps |
| COUNT/MIN/MAX over 100 ids | B | 715.3 µs | 772.7 µs | 798.2 µs | 823.0 µs | 859.7 µs | 1,017.5 µs | 805.1 µs (sd 4.0) | 1,242 qps |
| SHOW META (control) | A | 33.5 µs | 43.0 µs | 45.4 µs | 46.3 µs | 57.4 µs | 66.1 µs | 46.4 µs (sd 1.2) | 21,432 qps |
| SHOW META (control) | B | 33.4 µs | 43.0 µs | 45.5 µs | 46.4 µs | 57.6 µs | 66.0 µs | 46.5 µs (sd 1.4) | 21,484 qps |

| shape (cores = 2, 10,000 rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |
|---|---|---|---|---|
| INSERT, pk omitted | -0.10 µs [-0.30, +0.10] | +0.04 µs [-0.68, +0.88] | +0.50 % [-0.92, +1.40] | no cost resolved |
| INSERT again (replicate) | -0.10 µs [-0.30, +0.10] | -0.08 µs [-0.66, +0.50] | +0.92 % [-1.75, +1.38] | no cost resolved |
| SELECT by pk | +0.20 µs [-0.20, +0.40] | +0.18 µs [-0.26, +0.66] | -0.13 % [-0.86, +0.79] | no cost resolved |
| UPDATE by pk | +0.30 µs [-0.20, +0.90] | +0.16 µs [-0.72, +1.01] | -0.47 % [-1.65, +1.25] | no cost resolved |
| DELETE by pk | +0.00 µs [-0.10, +0.30] | +0.16 µs [-0.19, +0.56] | +0.00 % [-0.60, +0.57] | no cost resolved |
| COUNT/MIN/MAX over 100 ids | -69.35 µs [-73.40, -65.10] | -73.35 µs [-81.63, -66.93] | +8.94 % [+8.01, +9.61] | B faster (-8.0 % at p50) |
| SHOW META (control) | -0.10 µs [-0.50, +0.80] | +0.05 µs [-0.84, +0.91] | -0.15 % [-2.46, +2.61] | no cost resolved |

Replicate floor, arm A: INSERT again - INSERT p50 per run, median -0.10 µs, sd 0.36 µs.

Replicate floor, arm B: INSERT again - INSERT p50 per run, median +0.00 µs, sd 0.17 µs.

## 2. The scan shape: a sign that follows the build, not the code

`range100` is the one shape whose delta is large, and its sign is not stable. In
the first series at `cores = 1` B was **slower**: +10.45 µs (+7 %) at 1,000 rows
and **+107.85 µs [+101.8, +111.9] (+13 %)** at 10,000 rows, 16 runs, a tight
interval. At `cores = 2` the same pair was **faster**: -6.05 µs and -69.35 µs
(-8 %). The scan walk itself has not changed: `git diff 10593366 04649133 --
src/exec/step_vm.cpp` is three comment edits, and the diff of `btree.cpp` is comments, one error string and the new
`BtreeHoldTombstone`; the Cabin headers differ in comments only. So the pair was
re-run, and the three BH stage binaries added:

| comparison, 10,000 rows | cores | A p50 | other p50 | p50 delta (95 % CI) | runs |
|---|---|---|---|---|---|
| A vs B, the first series (above) | 1 | 814.0 µs | 918.2 µs | **+107.85 µs [+101.55, +111.90]** | 16 |
| A vs B, re-run about an hour later, same binaries | 1 | 851.6 µs | 781.4 µs | -69.35 µs [-72.40, -68.80] | 10 |
| A vs BH-S1 `80a0c223` (the test seam; no engine code) | 1 | 855.7 µs | 784.0 µs | -72.20 µs [-77.70, -63.90] | 10 |
| A vs BH-S2 `bbc7aa3e` (the retire primitive) | 1 | 849.3 µs | 777.8 µs | -73.95 µs [-80.95, -65.20] | 10 |
| A vs BH-S3 `26766699` (the statement) | 1 | 857.6 µs | 824.2 µs | -27.25 µs [-41.30, -22.85] | 10 |
| A vs BH-S1 | 2 | 865.2 µs | 797.1 µs | -69.25 µs [-73.30, -59.00] | 10 |
| A vs B, the first series | 2 | 870.3 µs | 798.2 µs | -69.35 µs [-73.40, -65.10] | 16 |
| A vs B, re-run | 2 | 862.2 µs | 799.0 µs | -67.35 µs [-69.95, -54.05] | 10 |

At 1,000 rows the same: +10.45 µs [+9.80, +11.30] (first series) against -6.70
[-7.05, -5.70] (re-run), BH-S1 -6.70, BH-S2 -7.05, BH-S3 -4.10, and at `cores = 2`
-6.05 [-7.20, -3.95]. The p50s in the table are pooled over the runs.

- **B is not slower on this shape on the evidence of three of the four A/B
  pairings at 10,000 rows.** A binary with no engine change at all (BH-S1) is as much faster
  than A as B is, and BH-S3, which adds a statement but no scan code, sits between.
  That puts the scan's speed in the build (code placement of the walk, or where
  the buffer pool's 10,000 pages land in the cache), not in a BH edit.
  `bench/v3.0.0/results-be-close-v2.7.0-665-geef442cf.md` found the same
  sensitivity in the same walk: a slab's page stride moved a resident scan 2.5-3.7 %.
- **The first `cores = 1` series is not explained.** Its 16 runs agree with each
  other, B's 918 µs is 17 % above B's own 781 µs an hour later with the same
  binary, and A's moved only 5 % (814 to 852 µs). The nearest candidate is host
  state — that series started two minutes after two parallel `-j4` builds
  finished, and memory placement was not recorded; it is a candidate, not a finding. **If it is real, it is +13 % on a full 10,000-row walk in one of four
  paired series; the other three, and the stage binaries, say -8 %.**
- **A's p50 ranged 814-870 µs over the eight series above (7 %); B's ranged
  781-918 µs over the four (17 %).** A scan benchmark at this level needs the
  A/B repeated before a sign is read from it.

The re-run's other arms (all shapes, `cores = 1`, 1,000 and 10,000 rows;
`cores = 2`, 10,000 rows) are in `archive/.../tables-c1-repeat.md` and
`tables-c2-repeat.md`: every point shape sits within 0.5 µs of A there too.

## 3. What a purge costs

A purge is one statement round trip plus 6 µs per key. Median over runs,
`min`/`max` of the per-run rates in brackets; the statement percentiles are the
median over runs of each run's percentile (a run's `n` is in the table).
**PURGE per key is the reciprocal of the throughput, derived.**

**durability = relaxed**

| cores | statement | relation rows | runs x statements | PURGE keys freed per second, median [min, max] | PURGE per key | PURGE statement p0 / p25 / p50 / p95 / p99 | DELETE keys marked per second, median | DELETE statement p50 |
|---|---|---|---|---|---|---|---|---|
| 1 | `PURGE ... WHERE id = k` | 200 | 5 x 200 | 18,114 keys/s [18,039, 18,194] | 55.21 µs | 41.2 µs / 49.7 µs / 54.9 µs / 64.3 µs / 72.0 µs | 20,201 keys/s | 48.2 µs |
| 1 | `PURGE ... WHERE id = k` | 1,000 | 5 x 1,000 | 17,746 keys/s [17,678, 17,939] | 56.35 µs | 40.8 µs / 51.8 µs / 55.7 µs / 64.2 µs / 73.5 µs | 20,255 keys/s | 48.2 µs |
| 1 | `PURGE ... WHERE id = k` | 10,000 | 5 x 10,000 | 17,430 keys/s [17,165, 17,515] | 57.37 µs | 40.5 µs / 51.7 µs / 55.9 µs / 65.4 µs / 73.6 µs | 19,722 keys/s | 48.3 µs |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+199` | 30,000 | 5 x 150 | 164,824 keys/s [151,236, 166,488] | 6.07 µs | 1,050.5 µs / 1,134.6 µs / 1,209.9 µs / 1,340.1 µs / 1,378.0 µs | 116,330 keys/s | 1,705.8 µs |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+999` | 30,000 | 5 x 30 | 172,196 keys/s [171,606, 172,312] | 5.81 µs | 5.72 ms / 5.78 ms / 5.80 ms / 5.86 ms / 5.87 ms | 489,982 keys/s | 2.03 ms |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+9999` | 30,000 | 5 x 3 | 175,326 keys/s [172,920, 177,206] | 5.70 µs | 56.96 ms / 56.96 ms / 57.02 ms / 57.08 ms / 57.08 ms | 1,509,577 keys/s | 6.63 ms |
| 2 | `PURGE ... WHERE id = k` | 200 | 5 x 200 | 17,665 keys/s [17,571, 17,940] | 56.61 µs | 41.8 µs / 50.7 µs / 55.8 µs / 65.8 µs / 74.3 µs | 19,780 keys/s | 48.9 µs |
| 2 | `PURGE ... WHERE id = k` | 1,000 | 5 x 1,000 | 17,610 keys/s [17,341, 17,670] | 56.79 µs | 41.7 µs / 52.3 µs / 56.1 µs / 65.9 µs / 73.4 µs | 19,893 keys/s | 48.7 µs |
| 2 | `PURGE ... WHERE id = k` | 10,000 | 5 x 10,000 | 17,216 keys/s [16,073, 17,391] | 58.09 µs | 40.6 µs / 52.3 µs / 56.3 µs / 66.8 µs / 78.0 µs | 19,596 keys/s | 48.3 µs |
| 2 | `PURGE ... WHERE id BETWEEN a AND a+199` | 30,000 | 5 x 150 | 161,956 keys/s [143,354, 163,078] | 6.17 µs | 1,077.3 µs / 1,161.0 µs / 1,236.0 µs / 1,365.8 µs / 1,397.9 µs | 99,539 keys/s | 2.02 ms |
| 2 | `PURGE ... WHERE id BETWEEN a AND a+999` | 30,000 | 5 x 30 | 166,689 keys/s [164,568, 169,080] | 6.00 µs | 5.88 ms / 5.97 ms / 6.00 ms / 6.09 ms / 6.10 ms | 402,289 keys/s | 2.47 ms |
| 2 | `PURGE ... WHERE id BETWEEN a AND a+9999` | 30,000 | 5 x 3 | 170,958 keys/s [149,787, 172,047] | 5.85 µs | 58.44 ms / 58.44 ms / 58.46 ms / 58.56 ms / 58.56 ms | 1,180,141 keys/s | 8.51 ms |

**durability = group**

| cores | statement | relation rows | runs x statements | PURGE keys freed per second, median [min, max] | PURGE per key | PURGE statement p0 / p25 / p50 / p95 / p99 | DELETE keys marked per second, median | DELETE statement p50 |
|---|---|---|---|---|---|---|---|---|
| 1 | `PURGE ... WHERE id = k` | 1,000 | 3 x 1,000 | 706 keys/s [691, 733] | 1,416.83 µs | 1,042.2 µs / 1,187.9 µs / 1,246.2 µs / 1,918.6 µs / 4.87 ms | 723 keys/s | 1,229.4 µs |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+999` | 10,000 | 3 x 10 | 126,942 keys/s [116,130, 129,136] | 7.88 µs | 7.45 ms / 7.55 ms / 7.64 ms / 10.54 ms / 10.54 ms | 264,679 keys/s | 3.21 ms |
| 2 | `PURGE ... WHERE id = k` | 1,000 | 3 x 1,000 | 719 keys/s [718, 744] | 1,390.43 µs | 1,031.7 µs / 1,181.4 µs / 1,224.6 µs / 1,725.8 µs / 4.74 ms | 711 keys/s | 1,216.5 µs |
| 2 | `PURGE ... WHERE id BETWEEN a AND a+999` | 10,000 | 3 x 10 | 128,668 keys/s [117,994, 129,430] | 7.77 µs | 7.60 ms / 7.64 ms / 7.65 ms / 8.39 ms / 8.39 ms | 269,830 keys/s | 3.43 ms |

**durability = strict**

| cores | statement | relation rows | runs x statements | PURGE keys freed per second, median [min, max] | PURGE per key | PURGE statement p0 / p25 / p50 / p95 / p99 | DELETE keys marked per second, median | DELETE statement p50 |
|---|---|---|---|---|---|---|---|---|
| 1 | `PURGE ... WHERE id = k` | 1,000 | 3 x 1,000 | 726 keys/s [719, 738] | 1,377.79 µs | 1,040.2 µs / 1,169.2 µs / 1,229.2 µs / 1,895.4 µs / 5.08 ms | 708 keys/s | 1,241.3 µs |
| 1 | `PURGE ... WHERE id BETWEEN a AND a+999` | 10,000 | 3 x 10 | 122,029 keys/s [116,480, 133,790] | 8.19 µs | 7.41 ms / 7.57 ms / 7.64 ms / 10.23 ms / 10.23 ms | 277,409 keys/s | 3.22 ms |
| 2 | `PURGE ... WHERE id = k` | 1,000 | 3 x 1,000 | 704 keys/s [694, 735] | 1,420.66 µs | 1,057.4 µs / 1,202.3 µs / 1,249.3 µs / 1,906.3 µs / 4.83 ms | 728 keys/s | 1,232.1 µs |
| 2 | `PURGE ... WHERE id BETWEEN a AND a+999` | 10,000 | 3 x 10 | 123,342 keys/s [107,225, 128,291] | 8.11 µs | 7.47 ms / 7.57 ms / 7.63 ms / 11.74 ms / 11.74 ms | 268,869 keys/s | 3.44 ms |

- **One key per statement** (200, 1,000, 10,000 keys): **17,400-18,100 keys/s at
  `cores = 1`, 17,200-17,700 keys/s at `cores = 2`**, a p50 of 55-56 µs, against 48 µs for
  the `DELETE` of the same key in the same run. The `SHOW META` floor is 45.5 µs,
  so a purge is 10-12 µs of work over the round trip, and about 7-8 µs more
  than the `DELETE` it follows. There is almost no row-count dependence: 55.2, 56.4 and
  57.4 µs per key at 200, 1,000 and 10,000 keys at `cores = 1` is a 4 % drift
  (56.6, 56.8, 58.1 µs at `cores = 2`), and `cores = 2` is 1-3 % dearer.
- **A window of 1,000 keys: 172,000 keys/s at `cores = 1` (5.8 ms a statement),
  167,000 keys/s at `cores = 2` (6.0 ms)**, and the per-key cost is flat across
  windows of 200, 1,000 and 10,000 keys (6.07, 5.81, 5.70 µs): **the statement's
  fixed cost is small and a key costs ~5.7 µs**, a tenth of a one-key statement.
  The `cores = 2` 1,000-key window ran bimodally across the 10 runs of a repeat
  (147,300 or 167,600 keys/s; `archive/.../purge-tp-repeat`, 6 of 10 runs in the
  lower mode; the five runs of the table are all in the upper one). It is
  probably which of the two cores the session was accepted on; the `cores = 1`
  cell has no such split.
- **Durable commit dominates the one-key purge.** At `strict` or `group` (one
  client, so `group` has nothing to batch) a one-key `PURGE` is **1.22-1.25 ms
  at p50, 704-726 keys/s**, and so is its `DELETE`: ~1.17 ms of that is the
  commit wait, 95 % of the statement. A window amortises it: the 1,000-key
  window costs 7.6 ms durable against 5.8-6.0 ms relaxed, **+1.6-1.8 ms for one commit
  wait per statement** (the two cells are on relations of 10,000 and 30,000 rows,
  and the relaxed per-key cost does not depend on that).
- **`DELETE` by key window has a fixed cost this stage did not touch.** A
  200-key `DELETE ... BETWEEN` is 1.7 ms and a 10,000-key one 6.6 ms: ~1.6 ms of
  fixed cost on a 30,000-row relation (a walk of the relation is the obvious
  candidate; not looked into). It is `DELETE`'s, outside BH.
- **No cell approached a limit of `PURGE`.** What bounds it is the round trip at
  one key (the engine does 10-12 µs of work in a 55 µs statement), the fsync when
  durable, and per-key work (~5.7 µs: the find, the retire and its log record)
  in a window. A window larger than 10,000 keys was not run; `known-gaps.md`
  says a wide window collects every target before it writes.

## 4. DELETE to PURGE under concurrent readers

**A purge waits for nothing in the ordinary case.** Its first attempt costs the
same as the `DELETE` before it, under idle and loaded servers alike; across 360,000
samples there was **not one `TXN_CONFLICT`**, and 1.64 million background reads
and 0.36 million background updates ran beside them with 0 errors. The latency
percentiles are the median over the 3 reps of each rep's percentile.

**DELETE commit to PURGE success**

| cores | background load | reps x samples | p0 | p25 | p50 | p95 | p99 | worst single sample (any rep) | PURGE first attempt p50 / p99 | DELETE p50 / p99 | TXN_CONFLICT refusals | each reader: throughput, p50 | writer throughput |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | none (idle server) | 3 x 20,000 | 41.9 µs | 53.9 µs | 57.8 µs | 68.4 µs | 77.8 µs | 22.6 ms | 57.6 / 77.6 µs | 56.7 / 77.0 µs | 0 of 60,000 | - | - |
| 1 | 2 readers + 1 writer | 3 x 20,000 | 48.4 µs | 93.2 µs | 99.7 µs | 125.9 µs | 165.5 µs | 33.7 ms | 99.3 / 165.1 µs | 97.9 / 164.8 µs | 0 of 60,000 | 9,557 qps, 96 µs | 9,984 qps |
| 1 | 3 readers | 3 x 20,000 | 44.5 µs | 87.2 µs | 98.3 µs | 125.5 µs | 156.0 µs | 32.2 ms | 97.9 / 155.6 µs | 95.7 / 151.8 µs | 0 of 60,000 | 9,673 qps, 96 µs | - |
| 2 | none (idle server) | 3 x 20,000 | 44.1 µs | 54.6 µs | 58.5 µs | 69.6 µs | 78.9 µs | 17.5 ms | 58.3 / 78.6 µs | 57.4 / 78.5 µs | 0 of 60,000 | - | - |
| 2 | 2 readers + 1 writer | 3 x 20,000 | 41.5 µs | 75.1 µs | 82.8 µs | 112.2 µs | 131.1 µs | 105.3 ms | 82.4 / 130.6 µs | 80.0 / 127.0 µs | 0 of 60,000 | 10,326 qps, 83 µs | 12,591 qps |
| 2 | 3 readers | 3 x 20,000 | 41.5 µs | 76.0 µs | 83.9 µs | 112.3 µs | 126.7 µs | 41.3 ms | 83.5 / 126.3 µs | 81.5 / 126.1 µs | 0 of 60,000 | 11,075 qps, 83 µs | - |


**Older snapshot held H ms after the DELETE**

| cores | H | keys | PURGE total (DELETE reply to PURGED), min / median / max | first attempt, median | TXN_CONFLICT refusals | total minus H, median |
|---|---|---|---|---|---|---|
| 1 | 0 ms | 5 | 0.05 / 0.07 / 0.07 ms | 0.06 ms | 0 of 5 | 0.07 ms |
| 1 | 50 ms | 5 | 50.25 / 50.28 / 50.30 ms | 50.28 ms | 0 of 5 | 0.28 ms |
| 1 | 200 ms | 5 | 200.37 / 200.41 / 200.46 ms | 200.41 ms | 0 of 5 | 0.41 ms |
| 1 | 500 ms | 5 | 500.39 / 500.42 / 500.45 ms | 500.42 ms | 0 of 5 | 0.42 ms |
| 1 | 900 ms | 5 | 900.42 / 900.45 / 900.46 ms | 900.45 ms | 0 of 5 | 0.45 ms |
| 1 | 1100 ms | 5 | 1100.36 / 1100.37 / 1100.39 ms | 1000.89 ms | 5 of 5 | 0.37 ms |
| 2 | 0 ms | 5 | 0.05 / 0.07 / 0.11 ms | 0.07 ms | 0 of 5 | 0.07 ms |
| 2 | 50 ms | 5 | 50.32 / 50.33 / 50.34 ms | 50.33 ms | 0 of 5 | 0.33 ms |
| 2 | 200 ms | 5 | 200.41 / 200.44 / 200.46 ms | 200.44 ms | 0 of 5 | 0.44 ms |
| 2 | 500 ms | 5 | 500.42 / 500.43 / 500.45 ms | 500.43 ms | 0 of 5 | 0.43 ms |
| 2 | 900 ms | 5 | 900.43 / 900.47 / 900.48 ms | 900.47 ms | 0 of 5 | 0.47 ms |
| 2 | 1100 ms | 5 | 1100.37 / 1100.40 / 1100.42 ms | 1000.92 ms | 5 of 5 | 0.40 ms |

- **Idle server: 58 µs at p50, 78-79 µs at p99.** The PURGE and the DELETE
  are the same cost (57.6 and 56.7 µs).
- **With three autocommit readers on the one core the latency rises to 98 µs
  (`cores = 1`), 84 µs (`cores = 2`) — and so does the DELETE's, to 96 and 82 µs.**
  The rise is the queueing behind the readers' own statements on the reactor
  (each reader's p50 is itself 96 µs); the purge adds none. Mixed load (two
  readers and a writer) is the same, 99.7 and 82.8 µs, and the writer is not
  blocked: its 10,000-12,600 `UPDATE`s per second ran throughout without a
  refusal, which is what `workorder-bh-purge-key.md` §6 ("a running `PURGE` holds the
  relation's `IX` and no row unit, so it blocks no DML") predicts. (No PURGE-free baseline for the writer was taken, so the
  writer's rate is a lower bound on "not blocked", not a delta.)
- **The `cores = 2` p50 moved from 72 to 102 µs across the three reps of one cell**:
  which core the session lands on, and whether the readers share it, is
  chosen by the accept and not by the driver. Percentiles are therefore reported
  per cell with that spread, and nothing finer is claimed.
- **The worst single sample of a cell is 17-105 ms, idle cells included**
  (22.6 ms at `cores = 1` idle, 17.5 ms at `cores = 2`): periodic stalls of the
  reactor or the device, which p99 does not see (a 20,000-sample rep has 200
  samples above p99). They are not purge waits: the PURGE first attempt is
  the whole latency in every rep.
- **Where the time of one purge goes** (p50, `cores = 1`, derived from the
  tables above):

  | wait type | idle, relaxed | three readers, relaxed | idle, `strict` or `group` |
  |---|---|---|---|
  | client, socket, frame and dispatch (the `SHOW META` floor) | 45.5 µs (79 %) | 45.5 µs (46 %) | 45.5 µs (4 %) |
  | the purge's own work (find, retire, log record) | ~12 µs (21 %) | ~12 µs (12 %) | ~12 µs (1 %) |
  | queueing behind the readers' statements on the reactor | 0 | ~40 µs (41 %) | 0 |
  | durability / commit (fsync) | 0 (relaxed) | 0 (relaxed) | ~1,170 µs (95 %) |
  | horizon wait, lock or conflict wait | 0 (first attempt == DELETE's cost) | 0 (0 of 60,000 refused) | not measured |

  The 45.5 µs floor includes the engine's own dispatch, which cannot be separated from
  the socket without server-side timestamps; the "own work" and "queueing" rows are
  differences of medians, not timed spans.
- **Older snapshots.** A purge behind a `REPEATABLE READ` snapshot older than its
  `DELETE` finishes **0.28-0.47 ms after the snapshot ends** (holds of 50-900 ms, 20 of 20
  samples at each core count; the figure includes the timer thread that issues the
  `COMMIT`; with no hold it is 0.07 ms). A hold longer than the bound is
  **refused `TXN_CONFLICT retryable=1` at 1.0009 s** (5 of 5, both core counts)
  and the retry succeeds 0.1 ms after the commit. So `kPurgeHorizonWait` (1 s) is
  the bound and the wait resolves within a half millisecond of the horizon
  moving, on an idle server; the poll was not measured under load.

## 5. A finding: a purged key can be unplaceable

The re-insert check failed for about 1 key in 168 in every cell with more than one
leaf (one per leaf of 166 keys), deterministically (5 of 1,000, 59 of 10,000, 119 of 20,000, 179 of
30,000), and **not at all in a one-leaf relation** (200 of 200). A key is
refused `ERR separator key <k> is already present in this internal node` when it
**equals the separator of a leaf, and every slot of that leaf has been
retired**. Reproduction on B (scripts in `archive/.../drivers/repro_reinsert*.py`):

    1,000 rows loaded, DELETE ... BETWEEN 167 AND 332, PURGE ... BETWEEN 167 AND 332  -> PURGED 166
    INSERT (167,..)  -> ERR separator key 167 is already present in this internal node (byte 22)
    INSERT (168,..)  -> INSERTED  page=143 slot=0       (a different page)
    INSERT (167,..)  -> ERR again, and again after 250 and 332 were placed

- **It does not occur when the purged window leaves a keyed slot in the leaf**
  (`PURGE` of 333..400, then 333: placed), nor for a `DELETE`, `PURGE`, `INSERT`
  of one key at a time over a whole 1,000-row relation (1,000 of 1,000 placed).
  It is a leaf with *no* keyed slot left.
- **The manual's claim is that naming a purged key places it**
  (`manual/sql/sql.md`, PURGE: "an `INSERT` that names one is placed as if the key
  had never been used"). For this key it does not, and the refusal text
  describes the tree, not the key. `known-gaps.md` (BH: "the leaf space a purge
  frees comes back only at a dividing split, never at an append split") states the
  space half; it does not say that the split's separator can be the key the
  insert is placing, and a grep of `docs/`, `instructions/` and `tests/` finds
  no entry for the refusal (the string occurs only in
  `src/storage/btree/btree_page.cpp`).
- Seen once, not followed up: after a purge of a whole 1,000-row relation, a
  500-row `INSERT` of the keys 1..500 was refused at row 167, and a following
  single `INSERT` of key 1 was then refused with the same error.
- This is the engine's behaviour at `04649133` and was not investigated further; no
  code was changed. The verification phase of `bh_purge_tp.py` counts such refusals apart
  (`reinsert_refused_separator`) and any other refusal separately (0 in every cell).

## What this says about the engine

- **A statement-class addition on the dispatch path is free at this resolution.** One text
  compare in the parser and one in `DispatchInner` sit under the 0.6 µs floor of a
  45 µs round trip, at both core counts and all three sizes, and the writes that
  `PURGE`'s extraction (`FoldPkWindow`) and the resume flag touch show no delta.
- **The scan loop is the engine's most build-sensitive path**, ±8-13 % between
  builds that differ by nothing it executes, and a scan A/B at this level needs the
  pair repeated on a quiet host before a sign is trusted. The point paths show no
  such sensitivity: they are dominated by the 45 µs round trip.
- **A purge is about as cheap as the delete it follows and does not wait, in the
  ordinary case.** Autocommit readers never held the horizon past the purge's first
  attempt (0 refusals in 360,000), so the 1 s poll is a bound that the OLTP shape
  does not reach; only an idle `BEGIN` does, and then the wait ends within half a
  millisecond of the snapshot. That is the design expectation in `workorder-bh-purge-key.md`
  §6 and `known-gaps.md` ("one open older snapshot blocks every PURGE of a key deleted
  after it"), and now has numbers.
- **Per key a purge is 5.7 µs, so freeing a million deleted keys in 1,000-key windows
  takes ~6 s relaxed** (derived, not run).
- **The first real data point on BH's re-placement claim is a miss** (section 5).

## Not run, and not measured

- **Overhead A/B at `strict` or `group`:** not run; only `relaxed`. Both are
  the engine's durable modes. BH's added work is before the commit, so a delta
  there would sit under a 1.2 ms commit wait (95 % of a one-key statement,
  section 3) and be harder to resolve, not easier.
- **`cores` above 2**, the multi-client write mix (`bb_concurrent_benchmark.py`'s shape) and `HEAP`
  relations (suspended, SUS-1): not run.
- **A BH-S4 binary:** not built; B has its code.
- **A purge under a concurrent *named* INSERT of the same key, a crash mid-window, or a
  window larger than 10,000 keys:** not run (BH-S4's sim and rig cells cover the
  two-core and crash shapes; they were not re-run).
- **A PostgreSQL floor:** not measured for this shape; there is no twin of `PURGE`.
- **The test suite:** not executed in this session.
- **No attribution of the cost inside a purge** (find, retire, log) was taken: `perf` is
  locked out on this host (`perf_event_paranoid` = 4, read this session), and the per-key figure is the
  difference of whole-statement times.

## Baseline for the next run

There is no earlier `PURGE` number; **this file is the baseline** for the shapes
in section 3 and 4, read against `v2.7.0-711-g04649133`. The overhead A/B's
baseline is `v2.7.0-706-g10593366` (A above).

