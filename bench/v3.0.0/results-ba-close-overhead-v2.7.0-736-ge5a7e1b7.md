# BA's close: the milestone's interleaved A/B overhead (`v2.7.0-736-ge5a7e1b7`)

**Thesis.** Over BA's whole code change, one connection pays **nothing resolvable on
an unsynced pk `INSERT`** (+0.00 to +0.15 µs against a floor of 0.2 µs), **0.1-1.25 µs
(0.2-2.2 %) on `UPDATE`, `DELETE`, the 20-row `INSERT` and a pk `SELECT`**, and a
**per-row cost on the whole-relation walk**: +1.35 to +2.0 µs at 200 rows, +2.0 to +5.2 µs
at 1,000 and +19.7 to +46.3 µs at 10,000 across six passes (about 2-4.5 ns on a 71 ns row,
2.5-6 %). A synced single-row `INSERT` costs **nothing resolvable at `cores = 1`** and
**+13 to +32 µs (1.0-3.1 %) at `cores = 2`**, the price of BA-S7's hand-off of the sync to
the writer thread; at 16 sessions the census file reads what that hand-off buys. At
10,000 rows B shows **a statement of 62-226 ms in 31 of 32 runs** where A's longest has a median of 18-22 ms (range
9-88 ms; 3 of 32 runs over 40 ms for A, 31 of 32 for B), which the stall probe attributes to the
checkpoint; at 200 and 1,000 rows B's longest statement is 2-19 ms where A's is 7-46 ms.
Scenario 0's `c8-s` cell, which refused one trade insert in each of three runs at
`v2.7.0-531-g9a0525d`, refuses none in three runs here. **One cell of this measurement is
invalid and one reproduction of it is pending: section 9.**

- **A** = `d43845a0` (`v2.7.0-720-gd43845a0`), the commit BA opened from.
- **B** = `e5a7e1b7` (`v2.7.0-736-ge5a7e1b7`), BA closed in code. **The worktree branch has
  moved since**: `worktree-ba-open-marks` is at `1a5176ae` (`v2.7.0-746-g1a5176ae`), ten
  commits on, and `git diff --stat e5a7e1b7 HEAD -- src include tools CMakeLists.txt`
  reports 15 changed files (BJ-S1 to BJ-S3: the lexer, the SET-expression parser and typer,
  `command_dispatcher.cpp`, `kwp_session.cpp`). **The engine measured here is `e5a7e1b7`,
  not the branch tip.** `build-release/kds_server` still carries mtime 2026-10-10 02:09:03 UTC,
  the build of `e5a7e1b7`, and its hash is B's.
- Every statement of this file was measured by an earlier ck-tester run that the operator
  stopped before it wrote its files; this file was written from that run's records, with
  **no new measurement**. Section 9 lists where the records could not support a claim.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-10, 02:11 to 08:57 UTC, plus an incomplete cell at 14:36-14:37 UTC (section 9). Relaxed first pass 02:11-02:23; synced first pass 02:23-03:49; quiet relaxed passes 03:49-04:14 (`cores = 1`) and 08:21-08:27 (`cores = 2`); synced repeats 04:15-05:01 (`cores = 1`) and 08:28-08:43 (`group`, `cores = 2`); stall probes 04:09-04:10; scenario 0 08:54-08:57. The census re-run (05:01-08:19) is its own file. Per-pass windows and loads: section 2 |
| Branch / worktree | `worktree-ba-open-marks`, worktree `ba-open-marks` |
| B | `e5a7e1b7`, `v2.7.0-736-ge5a7e1b7`. The earlier run recorded the tree clean (`git status --short` empty) before the build and at its stamp; that cannot be re-checked now. `build-release` was rebuilt at that commit with `-DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`; binary mtime 2026-10-10 02:09:03 UTC, **after** the commit (02:06:37 UTC) |
| A | `d43845a0`, `v2.7.0-720-gd43845a0` (commit date 2026-10-08 12:29:48 UTC). Its tree was exported by `tar` into a scratch directory and built there (Release, the same flags; binary mtime 2026-10-10 02:10:06 UTC). The checkout, the branch and `build/` (Debug) were not touched |
| Measured copies | A `1d41bdd516c7737b320ced94a9d7b755016299d6e4e81f47cb92934ea39cfd08`, B `d2a6977f220a32147b6a2df985aec10b60ee7411fe3fca3381b78e46e525223e` (re-hashed at the time of writing: both match); every server in every run started from a copy under `~/bench-runs/ba-close/bin/` (`archive/.../binaries.txt`). The same B copy's hash is the census file's |
| Device | data files under `~/bench-runs/ba-close/w*`, `/dev/root`, ext4 (`df -T`), not tmpfs; a fresh data file and server per (arm, run), deleted after |
| Host | 8 logical CPUs, AMD EPYC 9V74, 4 physical cores x 2 threads (SMT siblings 0/1, 2/3, 4/5, 6/7), Linux 7.0.0-1014-azure |
| Pinning | server on CPU 4 (`cores = 1`) or 4,5 (`cores = 2`, siblings of one physical core, as the BH close's 2,3 were); driver on CPU 7. **Not the BH close's CPUs 2,3 and 4**, because a training job held CPUs 0-3 early on (section 2) |
| Server config | `cores` 1 or 2; `durability` relaxed, and `group` or `strict` for the synced arms; `buffer_pool_frames = 65536`; `log_level = warn`; everything else default. Ports 31811 (A) and 31812 (B), chosen, never the default |
| Suite | not executed. No code changed in this measurement; the suite belongs to the stages |
| PostgreSQL floor | not measured for this shape. The A/B is B against A; the census file carries the floor |
| Drivers | `tools/` untouched. The BH close's `bh_overhead_ab.py` was copied into the archive as `ba_overhead_ab.py` with three additions (the server's `durability` as a flag, an `--arms` filter, and `ba_overhead_ctrl.py`'s two more controls). `bench/docs/` is closed (`bench/README.md`), so the commands are the archived `drivers/run_*.sh`. `tools/` has no driver for these shapes, which breaks README rule 5 exactly as the BH close did |

## 2. Method, and the host each pass ran on

Method, unchanged from the BH close: one client connection, serial. Per (rows, run) a
fresh A server and a fresh B server, six `(id, v, w) BTREE` relations loaded with `--rows`
named ascending keys in 500-row `INSERT`s, then arms of 3,000 statements each in 12 blocks
of 250 that alternate which server goes first; the first server alternates per run.
**The flag `--rows 200,1000,10000` is the relation's row count when an arm starts.** Equal
work: 3,000 statements per arm per server; 16 runs per cell, so 48,000 statements per arm
pooled. The delta is the median of the per-run p50 differences B - A with a bootstrap 95 %
CI (4,000 resamples of the 16 runs, seed 1). Arms: the pk-omitted `INSERT` and its
replicate, `SELECT` by pk, `UPDATE` by pk, `DELETE` by pk (the block's keys are inserted
untimed first), a 20-row ascending `INSERT`, a whole-relation walk (`SELECT COUNT(*),
MIN(id), MAX(id) ... WHERE id >= lo AND id < lo + 100`, which walks the relation: A's p50
grows from 86 to 784 µs with the rows), and controls.

**The host was shared, and this table is the record.** Every pass is listed, including those
that are not used for a number. "Live program" counts runs whose before- or after-list of
competing processes holds a process that is itself a compiler, a build driver or a test
binary (`cc1plus`, `cmake`, `ctest`, `kds_tests`); the driver's `pgrep` also matches other
sessions' idle wait loops that merely name `ctest`, so every run has a non-empty list and
that is not counted. Derived from the run JSONs by `archive/.../host-summary.txt`.

| pass (archive directory) | cores | durability | UTC window | 1-min load before a run | runs with a live program | role |
|---|---|---|---|---|---|---|
| `overhead-c1` | 1 | relaxed | 02:11-02:16 | 4.77-5.78 | 0 of 48 | replicate; a training job (`train_deep.py`, about 300 % CPU) ran |
| `overhead-c2` | 2 | relaxed | 02:16-02:23 | 2.23-5.83 | 0 of 48 | replicate, same job; **CIs of tens of µs, not used for any number** |
| `overhead-group-c1` | 1 | group | 02:23-02:47 | 1.15-14.08 | 21 of 48 | replicate; another session's `ctest -j8` |
| `overhead-strict-c1` | 1 | strict | 02:47-03:19 | 1.37-9.96 | 25 of 48 | replicate; same |
| `overhead-group-c2` | 2 | group | 03:19-03:36 | 1.26-9.53 | 5 of 48 | replicate (first pass) |
| `overhead-strict-c2` | 2 | strict | 03:36-03:49 | 1.12-1.48 | 0 of 48 | **reported** (`strict`, `cores = 2`) |
| `overheadq-c1` | 1 | relaxed | 03:49-03:54 | 1.00-1.91 | 0 of 48 | **reported** (relaxed, `cores = 1`) |
| `overheadq-c2` | 2 | relaxed | 03:55-04:06 | 1.29-2.00 | 25 of 48 | replicate; a named test process in 411 of 711 sampler seconds, so it is not the reported pass |
| `overheadq2-c1` | 1 | relaxed | 04:10-04:14 | 1.12-1.91 | 0 of 48 | **reported** as the repeat of the pk `SELECT` and the walk (arms `select-pk`, `range100` and the controls) |
| `overheadq-group-c1` | 1 | group | 04:15-04:42 | 1.18-10.43 | 20 of 48 | **reported** (`group`, `cores = 1`); the load widens the CIs |
| `overheadq-strict-c1` | 1 | strict | 04:42-05:01 | 1.23-4.16 | 8 of 48 | **reported** (`strict`, `cores = 1`) |
| `overheadq3-c2` | 2 | relaxed | 08:21-08:27 | 0.59-1.96 | 0 of 48 | **reported** (relaxed, `cores = 2`); no named test or build process in any sampler second |
| `overheadq3-group-c2` | 2 | group | 08:28-08:43 | 1.31-1.96 | 0 of 48 | **reported** (`group`, `cores = 2`) |

- **The first two passes ran under an unrelated training job** and are kept as replicates.
  `ps` at the start of the first shows it at 303 % CPU on CPU 0, at the second on CPU 2 at
  306 %; its affinity is not recorded in the files (the earlier run's notes say CPUs 0-3,
  which is why the servers sit on 4-7).
- **Other sessions' `ctest -j8` (`bj-expression-update`) loaded all eight CPUs** in windows
  between 02:53 and 04:52 and the sampler (1 s, per-CPU busy time from `/proc/stat`, started
  02:52) saw them. Interleaving gives A and B each run's conditions, so load widens the CIs
  rather than moving the centre; the synced `cores = 1` CIs have half-widths of 2-6 µs for that reason.
- **Sampler means over the reported passes**: `overheadq-c1` CPUs 0-3 and 5-6 at 1-2 % busy,
  CPU 4 (the server) 57 %, CPU 7 (the driver) 35 %; `overheadq3-c2` CPUs 0, 2, 3, 6 at 2-3 %
  but **CPU 1 at 24 % from a process the sampler did not name** (physical core 0, a different
  physical core from the server's 4,5 and the driver's 7); `overheadq3-group-c2` the same CPU 1
  reading (23 %). `host-samples.jsonl` is 8.5 MB and is not archived.
- **No build (`cc1plus`, `cmake`) was live at the start or end of any reported run**, except
  the 20 and 8 runs counted above in the reported synced `cores = 1` passes, whose CIs carry it.

**README's five rules**: Release rebuilt at the measured commit (above); data on a block
device, `df -T` `/dev/root` ext4; a copy of each binary, hashed; the host's load and competing
list written per run (the JSONs' `host_before` and `host_after`); the ports chosen.

### The noise floor, from inside the run

**The BH close's control is no longer a control.** `SHOW META` is +4.8 to +5.5 µs (+11 %)
slower in B in every cell, because B's reply carries the `contention_*` block (BA-S2) and A's
source has no such field (`grep contention_ src include` is empty in A's tree). It is
reported as that, in the matrices, and not used as a floor. **`SHOW NAMESPACES` and `SET
ISOLATION LEVEL`** replace it: nothing BA touched is on either path.

| floor | `cores = 1` | `cores = 2` |
|---|---|---|
| control p50 delta B - A (`SHOW NAMESPACES`, `SET ISOLATION LEVEL`), reported passes | +0.00 to +0.10 µs, CI inside [-0.10, +0.20] | +0.00 to +0.05 µs, CI inside [-0.10, +0.20] |
| replicate (`INSERT again` - `INSERT` p50 inside one arm), sd of 16 runs, relaxed | 0.12-0.17 µs | 0.13-0.24 µs |
| replicate throughput delta (`INSERT again`, B over A), relaxed | +2.2 to +7.8 % | +1.7 to +10.4 % |
| replicate p50 sd, `group` / `strict`, sd of 16 runs | `group` 17-18 µs at 1,000 and 10,000 rows (97-141 µs at 200 rows, load to 14); `strict` 12-13 µs at 10,000 rows (50-82 µs at 200 and 1,000 rows, under load) | `group` 13-25 µs, `strict` 12-22 µs (quiet passes) |

**A relaxed p50 delta under 0.2 µs is not a finding.** The replicate's *throughput* delta is
+2 to +10 % because a stall (section 8) lands in one arm of one server per run: **a
throughput delta under about 10 % at 10,000 rows is inside the floor, and the p50, which a
stall does not move, is the measure every reading below uses.** For a synced arm the floor on
a single run is an order larger (12-25 µs), which the 16-run median and its CI average down;
a synced delta is read by its sign across cells as well as by its CI (section 7).

## 3. Where a statement's time goes (rule 3)

A statement's latency is a sum. For one serial connection the parts are measurable from the
controls; **a lock or conflict wait is zero by construction** (one connection, no other writer),
and the `contention_*` counters were not read by the A/B driver. A's p50 at `cores = 1`,
200 rows:

| part | how it is read | A's p50 | share of the unsynced `INSERT` | share of a `group` commit |
|---|---|---|---|---|
| socket round trip, framing, parse, dispatch | `SET ISOLATION LEVEL` / `SHOW NAMESPACES` (nothing else in them) | 37.5-37.7 µs | 77 % | 3.7 % |
| write statement beyond that floor | pk `INSERT`, relaxed, minus the floor | 11.4 µs (49.1 - 37.7) | 23 % | 1.1 % |
| read statement beyond the floor | pk `SELECT` minus the floor | 27.1 µs (64.8 - 37.7) | - | - |
| durability wait (fsync of the commit) | `group` `INSERT` (1,032.8 µs) minus relaxed `INSERT` | 983.7 µs | - | 95.2 % |
| the same, `strict` | `strict` `INSERT` (1,022.0 µs) minus relaxed `INSERT` | 972.9 µs | - | - |

The device, not the engine, is 95 % of a synced single-row commit, so a synced delta is a
small share of a large number: **the +13 to +32 µs at `cores = 2` is 1.0-3.1 % of the commit**.
The commit's own cost wandered with the device during this session: A's `group` `INSERT`
p50 at `cores = 2` reads 1,011-1,023 µs at 03:19-03:36 and 1,258-1,266 µs at 08:28-08:43
(section 7). Client round-trip and socket time cannot be separated from dispatch on this
driver.

## 4. Result: relaxed, a single connection

The p50 delta B - A with its 95 % CI, and the median per-run throughput change, from the
reported passes (`overheadq-c1`, `overheadq3-c2`). Throughput is the driver's `ops / elapsed`;
the second table gives it directly, A then B. Every reading below 0.2 µs is inside the floor.

| shape | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| INSERT, pk omitted | +0.00 µs [-0.10, +0.10]; +0.1 % tps [-0.2, +0.5] | +0.05 µs [+0.00, +0.20]; -0.2 % tps [-0.9, +0.2] | +0.00 µs [-0.10, +0.20]; +0.2 % tps [-0.2, +0.6] | +0.15 µs [+0.00, +0.30]; -0.4 % tps [-1.0, -0.1] | +0.10 µs [-0.10, +0.25]; -0.2 % tps [-0.4, +0.3] | +0.10 µs [+0.00, +0.10]; -0.9 % tps [-1.4, -0.4] |
| INSERT again (replicate) | +0.05 µs [-0.20, +0.15]; +2.2 % tps [+1.9, +2.7] | +0.10 µs [+0.00, +0.20]; +5.3 % tps [+4.4, +6.4] | +0.00 µs [-0.10, +0.10]; +7.8 % tps [+5.6, +9.3] | +0.15 µs [-0.10, +0.30]; +1.7 % tps [+0.8, +3.2] | +0.00 µs [-0.05, +0.20]; +4.0 % tps [+2.4, +5.2] | +0.05 µs [-0.10, +0.20]; +10.4 % tps [+8.0, +10.9] |
| SELECT by pk | +0.50 µs [+0.40, +1.10]; -1.0 % tps [-1.5, -0.4] | +0.60 µs [+0.30, +1.00]; -1.2 % tps [-1.5, -0.6] | +0.50 µs [+0.20, +0.80]; -1.1 % tps [-1.5, -0.4] | +0.95 µs [+0.80, +1.10]; -1.5 % tps [-1.9, -0.9] | +0.85 µs [+0.50, +1.20]; -1.1 % tps [-1.6, -0.6] | +0.85 µs [+0.10, +1.40]; -1.4 % tps [-2.5, -0.5] |
| UPDATE by pk | +0.55 µs [+0.25, +0.90]; +1.8 % tps [-0.3, +4.6] | +0.20 µs [-0.10, +0.40]; +6.0 % tps [+5.0, +7.8] | +0.65 µs [+0.00, +0.80]; +1.2 % tps [+0.5, +2.8] | +0.85 µs [+0.60, +1.40]; +1.9 % tps [+0.3, +3.2] | +0.75 µs [+0.45, +1.20]; +5.3 % tps [+4.5, +7.1] | +1.15 µs [+0.50, +1.45]; +1.7 % tps [+0.6, +2.4] |
| DELETE by pk | +0.10 µs [+0.10, +0.30]; +3.1 % tps [+1.7, +4.1] | +0.20 µs [+0.00, +0.40]; +4.2 % tps [+2.3, +5.5] | +0.10 µs [+0.00, +0.20]; -2.1 % tps [-2.7, -0.8] | +0.30 µs [+0.20, +0.80]; +2.6 % tps [+1.3, +3.5] | +0.25 µs [+0.15, +0.40]; +2.5 % tps [+1.2, +3.3] | +0.20 µs [+0.20, +0.50]; -1.7 % tps [-2.3, -0.9] |
| bulk INSERT, 20 rows | +0.40 µs [-0.10, +0.60]; -1.6 % tps [-2.0, -0.7] | -0.10 µs [-1.00, +0.70]; -0.4 % tps [-1.4, +0.6] | -0.10 µs [-0.90, +0.90]; -19.5 % tps [-22.5, -16.8] | +1.00 µs [+0.10, +2.15]; -0.4 % tps [-1.2, +0.5] | +1.25 µs [+0.90, +2.60]; -0.6 % tps [-1.8, +3.2] | +0.95 µs [+0.40, +2.80]; -25.8 % tps [-34.3, -17.9] |
| whole-relation scan (COUNT/MIN/MAX, 100-id window) | +1.40 µs [+0.75, +1.90]; -1.4 % tps [-1.8, -0.5] | +4.75 µs [+4.40, +5.50]; -2.8 % tps [-3.5, -2.5] | +39.65 µs [+37.90, +41.90]; -7.1 % tps [-7.9, -5.2] | +1.35 µs [+0.30, +2.15]; -1.3 % tps [-2.6, -0.1] | +3.35 µs [+2.80, +4.20]; -2.5 % tps [-3.2, -1.4] | +33.25 µs [+32.30, +36.20]; -4.7 % tps [-5.6, -3.9] |
| SHOW META | +4.90 µs [+4.80, +5.10]; -11.5 % tps [-11.9, -11.1] | +4.95 µs [+4.90, +5.10]; -11.5 % tps [-12.3, -10.8] | +5.20 µs [+4.90, +5.30]; -11.8 % tps [-12.4, -11.6] | +4.90 µs [+4.75, +5.30]; -11.8 % tps [-13.1, -10.9] | +5.50 µs [+4.90, +7.50]; -11.8 % tps [-25.3, -10.2] | +5.25 µs [+4.90, +6.90]; -12.5 % tps [-14.0, -11.5] |
| SHOW NAMESPACES (control) | +0.10 µs [-0.10, +0.20]; -0.2 % tps [-0.7, +0.4] | +0.00 µs [-0.10, +0.10]; +0.1 % tps [-0.2, +0.9] | +0.00 µs [-0.10, +0.10]; +0.2 % tps [-0.1, +0.3] | +0.00 µs [-0.10, +0.05]; -0.1 % tps [-0.3, +0.3] | +0.00 µs [-0.05, +0.15]; +0.2 % tps [-0.7, +0.5] | +0.05 µs [-0.10, +0.10]; +0.1 % tps [-0.7, +0.6] |
| SET ISOLATION LEVEL (control) | +0.00 µs [-0.10, +0.10]; +0.2 % tps [-0.2, +0.6] | +0.00 µs [-0.10, +0.00]; -0.0 % tps [-0.1, +0.4] | +0.00 µs [-0.05, +0.10]; -0.2 % tps [-0.5, +0.1] | +0.00 µs [-0.10, +0.10]; +0.1 % tps [-0.6, +0.4] | +0.00 µs [-0.10, +0.10]; +0.2 % tps [-0.3, +0.6] | +0.00 µs [-0.10, +0.20]; +0.1 % tps [-1.0, +0.5] |

Throughput, A then B (median of the runs' `ops / elapsed`):

| shape | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| INSERT, pk omitted | 19,422 -> 19,461 tps | 19,470 -> 19,436 tps | 19,462 -> 19,452 tps | 19,079 -> 19,037 tps | 19,106 -> 18,990 tps | 19,050 -> 18,907 tps |
| INSERT again (replicate) | 18,551 -> 19,035 tps | 18,213 -> 19,365 tps | 18,017 -> 19,388 tps | 18,412 -> 18,695 tps | 18,188 -> 18,950 tps | 17,225 -> 19,011 tps |
| SELECT by pk | 14,668 -> 14,462 tps | 14,327 -> 14,168 tps | 15,057 -> 14,895 tps | 14,244 -> 14,027 tps | 13,824 -> 13,627 tps | 14,500 -> 14,381 tps |
| UPDATE by pk | 17,924 -> 18,209 tps | 17,243 -> 18,311 tps | 17,751 -> 18,080 tps | 17,303 -> 17,560 tps | 16,720 -> 17,571 tps | 17,368 -> 17,570 tps |
| DELETE by pk | 18,926 -> 19,360 tps | 18,789 -> 19,574 tps | 19,751 -> 19,297 tps | 18,358 -> 18,782 tps | 18,584 -> 18,968 tps | 19,299 -> 18,967 tps |
| bulk INSERT, 20 rows | 9,172 -> 9,038 tps | 9,144 -> 9,115 tps | 9,021 -> 7,227 tps | 8,461 -> 8,442 tps | 8,420 -> 8,456 tps | 8,482 -> 6,326 tps |
| whole-relation scan (COUNT/MIN/MAX, 100-id window) | 11,188 -> 11,050 tps | 6,743 -> 6,541 tps | 1,264 -> 1,175 tps | 10,861 -> 10,698 tps | 6,602 -> 6,475 tps | 1,240 -> 1,183 tps |
| SHOW META | 21,716 -> 19,192 tps | 21,670 -> 19,190 tps | 21,808 -> 19,194 tps | 21,440 -> 18,931 tps | 21,280 -> 18,745 tps | 21,547 -> 18,869 tps |
| SHOW NAMESPACES (control) | 25,630 -> 25,568 tps | 25,565 -> 25,650 tps | 25,583 -> 25,630 tps | 25,072 -> 25,030 tps | 24,930 -> 24,893 tps | 24,981 -> 25,041 tps |
| SET ISOLATION LEVEL (control) | 25,713 -> 25,713 tps | 25,825 -> 25,863 tps | 25,820 -> 25,709 tps | 25,227 -> 25,224 tps | 25,230 -> 25,223 tps | 25,209 -> 25,225 tps |

A's p50 (median of the 16 per-run p50s):

| shape | cores 1, 200 rows | cores 1, 1,000 rows | cores 1, 10,000 rows | cores 2, 200 rows | cores 2, 1,000 rows | cores 2, 10,000 rows |
|---|---|---|---|---|---|---|
| INSERT, pk omitted | 49.1 µs | 49.0 µs | 48.9 µs | 49.5 µs | 49.4 µs | 49.5 µs |
| INSERT again (replicate) | 49.1 µs | 48.9 µs | 49.0 µs | 49.4 µs | 49.3 µs | 49.5 µs |
| SELECT by pk | 64.8 µs | 66.3 µs | 64.0 µs | 66.4 µs | 68.7 µs | 65.7 µs |
| UPDATE by pk | 50.9 µs | 51.3 µs | 51.4 µs | 52.1 µs | 52.3 µs | 52.7 µs |
| DELETE by pk | 48.8 µs | 48.8 µs | 49.0 µs | 49.4 µs | 49.4 µs | 49.4 µs |
| bulk INSERT, 20 rows | 93.9 µs | 94.5 µs | 94.6 µs | 102.1 µs | 101.2 µs | 102.5 µs |
| whole-relation scan (COUNT/MIN/MAX, 100-id window) | 85.7 µs | 143.9 µs | 783.4 µs | 87.7 µs | 146.4 µs | 798.5 µs |
| SHOW META | 45.2 µs | 45.2 µs | 45.0 µs | 45.6 µs | 45.5 µs | 45.5 µs |
| SHOW NAMESPACES (control) | 37.5 µs | 37.6 µs | 37.6 µs | 38.1 µs | 38.2 µs | 38.1 µs |
| SET ISOLATION LEVEL (control) | 37.7 µs | 37.6 µs | 37.6 µs | 38.0 µs | 38.2 µs | 38.1 µs |

The shape of the latency (rule 6): the median over the 16 runs of each run's percentile, 3,000
statements per run (48,000 pooled), A then B.

| cell | statement | server | statements | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| relaxed, cores 1, 1,000 rows | INSERT, pk omitted | A | 48,000 statements | 42.2 µs | 48.2 µs | 49.0 µs | 62.1 µs | 76.1 µs |
| relaxed, cores 1, 1,000 rows | INSERT, pk omitted | B | 48,000 statements | 42.0 µs | 48.2 µs | 49.0 µs | 62.7 µs | 77.2 µs |
| relaxed, cores 1, 1,000 rows | SELECT by pk | A | 48,000 statements | 59.8 µs | 64.2 µs | 66.3 µs | 83.4 µs | 96.1 µs |
| relaxed, cores 1, 1,000 rows | SELECT by pk | B | 48,000 statements | 61.3 µs | 64.7 µs | 67.0 µs | 83.8 µs | 95.9 µs |
| relaxed, cores 1, 1,000 rows | UPDATE by pk | A | 48,000 statements | 43.4 µs | 50.7 µs | 51.3 µs | 65.2 µs | 78.5 µs |
| relaxed, cores 1, 1,000 rows | UPDATE by pk | B | 48,000 statements | 44.5 µs | 50.8 µs | 51.5 µs | 65.4 µs | 77.8 µs |
| relaxed, cores 1, 1,000 rows | DELETE by pk | A | 48,000 statements | 42.3 µs | 48.1 µs | 48.8 µs | 61.5 µs | 69.6 µs |
| relaxed, cores 1, 1,000 rows | DELETE by pk | B | 48,000 statements | 41.7 µs | 48.2 µs | 49.0 µs | 60.9 µs | 70.1 µs |
| relaxed, cores 1, 1,000 rows | INSERT, 20 rows | A | 48,000 statements | 90.5 µs | 92.7 µs | 94.5 µs | 120.4 µs | 165.4 µs |
| relaxed, cores 1, 1,000 rows | INSERT, 20 rows | B | 48,000 statements | 89.7 µs | 92.4 µs | 94.3 µs | 128.0 µs | 212.1 µs |
| relaxed, cores 1, 1,000 rows | walk, COUNT/MIN/MAX | A | 48,000 statements | 134.6 µs | 140.6 µs | 143.9 µs | 163.5 µs | 180.8 µs |
| relaxed, cores 1, 1,000 rows | walk, COUNT/MIN/MAX | B | 48,000 statements | 139.1 µs | 145.4 µs | 148.9 µs | 169.0 µs | 184.1 µs |
| relaxed, cores 1, 1,000 rows | SET ISOLATION LEVEL (control) | A | 48,000 statements | 34.0 µs | 37.3 µs | 37.6 µs | 46.0 µs | 54.2 µs |
| relaxed, cores 1, 1,000 rows | SET ISOLATION LEVEL (control) | B | 48,000 statements | 33.7 µs | 37.3 µs | 37.5 µs | 46.0 µs | 53.9 µs |
| group, cores 1, 1,000 rows | INSERT, pk omitted | A | 48,000 statements | 870.6 µs | 970.2 µs | 1005.2 µs | 1871.4 µs | 4994.4 µs |
| group, cores 1, 1,000 rows | INSERT, pk omitted | B | 48,000 statements | 873.7 µs | 973.0 µs | 1009.6 µs | 1956.4 µs | 5218.5 µs |
| strict, cores 1, 1,000 rows | INSERT, pk omitted | A | 48,000 statements | 883.5 µs | 981.8 µs | 1017.4 µs | 1865.0 µs | 5029.0 µs |
| strict, cores 1, 1,000 rows | INSERT, pk omitted | B | 48,000 statements | 881.1 µs | 982.6 µs | 1018.6 µs | 1959.9 µs | 5118.6 µs |
| group, cores 2, 1,000 rows | INSERT, pk omitted | A | 48,000 statements | 1024.7 µs | 1197.3 µs | 1259.0 µs | 2275.6 µs | 5621.0 µs |
| group, cores 2, 1,000 rows | INSERT, pk omitted | B | 48,000 statements | 1051.3 µs | 1220.2 µs | 1279.9 µs | 2198.3 µs | 5203.4 µs |
| strict, cores 2, 1,000 rows | INSERT, pk omitted | A | 48,000 statements | 888.4 µs | 983.7 µs | 1016.9 µs | 1920.2 µs | 4955.5 µs |
| strict, cores 2, 1,000 rows | INSERT, pk omitted | B | 48,000 statements | 899.8 µs | 1006.1 µs | 1042.0 µs | 2003.8 µs | 5276.2 µs |
| relaxed, cores 1, 10,000 rows | walk, COUNT/MIN/MAX | A | 48,000 statements | 712.2 µs | 758.9 µs | 783.4 µs | 833.0 µs | 892.8 µs |
| relaxed, cores 1, 10,000 rows | walk, COUNT/MIN/MAX | B | 48,000 statements | 744.4 µs | 795.7 µs | 822.2 µs | 875.1 µs | 920.6 µs |
| relaxed, cores 1, 10,000 rows | INSERT, 20 rows | A | 48,000 statements | 90.2 µs | 92.8 µs | 94.6 µs | 120.2 µs | 167.4 µs |
| relaxed, cores 1, 10,000 rows | INSERT, 20 rows | B | 48,000 statements | 90.2 µs | 92.9 µs | 94.7 µs | 120.9 µs | 164.6 µs |

The pk statements shift by about the same amount at every percentile (the pk `SELECT`: +1.5
µs at p0, +0.5 at p25, +0.7 at p50, +0.4 at p95, -0.2 at p99), so the cost is a fixed one. **The
walk at 10,000 rows shifts by +32.2 µs at p0, +36.8 at p25, +38.8 at p50, +42.1 at p95 and
+27.8 at p99: the best case pays as much as the median, which is what a per-row cost looks like
and a stall does not.** The 20-row `INSERT` at 1,000 rows has a heavier B tail (p99 212.1 µs
against 165.4 µs) and an unmoved p50.

What the matrix says:

- **A pk-omitted `INSERT` costs nothing resolvable at either core count**: +0.00 to +0.15 µs,
  every CI within the floor; the replicate agrees (+0.00 to +0.15 µs).
- **`UPDATE` by pk +0.20 to +1.15 µs and `DELETE` by pk +0.10 to +0.30 µs**: at `cores = 2`
  every CI excludes zero (0.4-2.2 % and 0.4-0.6 % of A's p50); at `cores = 1` only some do.
  `DELETE` stays inside 0.3 µs everywhere.
- **A pk `SELECT` costs +0.50 to +0.95 µs (0.8-1.4 %)**, CI excluding zero in all six cells,
  reproduced by the repeat (+0.70 to +1.00 µs, 1.1-1.5 %). This is the one point statement
  with a stable sign. **Its size is two orders above what BA-S1c's own text prices**
  (`workorder-ba-parallelism.md`: "a statement reads its session's bound and finds it
  consumed; the first statement after a commit reads `SnapshotCeiling()` once, three loads at
  one core" is nanoseconds; the measured delta is 500-950 ns). BA-S2's latch timing and the
  walk-boundary changes of S15 are on or near this path; the A/B spans all stages and does not
  say which one the 0.5-0.95 µs is.
- **The 20-row `INSERT` is unresolved at `cores = 1`** (-0.10 to +0.40 µs, CIs spanning zero)
  and **+0.95 to +1.25 µs (0.9-1.2 %) at `cores = 2`**, the three CIs excluding zero (lower
  bounds +0.10, +0.90 and +0.40 µs).
- **`SHOW META` +4.90 to +5.50 µs (10.7-12.1 %)**: the `contention_*` reply block, not the
  engine's hot path.

### Against this engine's last result (rule 4)

The previous measurement of the same shapes and driver is the BH close,
`results-bh-close-v2.7.0-712-g4015736d.md`, at `v2.7.0-712-g4015736d` (24 commits before
B). Its B' column is its A p50 plus its median delta (derived). The two runs differ in pinning
(server on CPU 2, driver on CPU 4 there; CPU 4 and 7 here) and in the day, so a difference
under about 7 µs on the walk (the scatter between BH's A at `v2.7.0-706` and this run's A at
`v2.7.0-720`: -1.1, -0.2 and -6.8 µs at the three sizes) is not a finding.

| shape, `cores = 1` | rows | BH close B' (712), p50 | this run's A (720), p50 | this run's B (736), p50 | B (736) - B' (712) |
|---|---|---|---|---|---|
| `INSERT`, pk omitted | 200 | 49.3 µs | 49.1 µs | 49.1 µs | -0.2 µs |
| `INSERT`, pk omitted | 1,000 | 49.2 µs | 49.0 µs | 49.1 µs | -0.1 µs |
| `INSERT`, pk omitted | 10,000 | 49.2 µs | 48.9 µs | 48.9 µs | -0.3 µs |
| `SELECT` by pk | 200 | 65.3 µs | 64.8 µs | 65.3 µs | 0.0 µs |
| `SELECT` by pk | 1,000 | 66.6 µs | 66.3 µs | 66.9 µs | +0.4 µs |
| `SELECT` by pk | 10,000 | 64.5 µs | 64.0 µs | 64.5 µs | +0.1 µs |
| walk (`COUNT/MIN/MAX`) | 200 | 86.4 µs | 85.7 µs | 87.1 µs | +0.7 µs |
| walk (`COUNT/MIN/MAX`) | 1,000 | 145.4 µs | 143.9 µs | 148.7 µs | +3.3 µs |
| walk (`COUNT/MIN/MAX`) | 10,000 | 804.3 µs | 783.4 µs | 823.1 µs | +18.8 µs |

Against the last published result the pk statements are unchanged and the walk at 10,000
rows is +18.8 µs (+2.3 %). **Against A the same walk reads +39.65 µs, twice as much, because A's
walk (783.4 µs) sits 21 µs below the BH close's B' (804.3 µs).** Two walk p50s per size
bracket B's cost: the within-run delta and this cross-day one disagree by a factor of two at
10,000 rows, and the data cannot say which engine state moved.

## 5. The whole-relation walk: a per-row cost

The walk's cost is per row of the relation, not a fixed one, and it does not reproduce
tightly from pass to pass. Δ p50, B - A, with its CI, every relaxed pass that measured the
walk:

| pass | cores | 200 rows | 1,000 rows | 10,000 rows | per row, 200 to 10,000 rows |
|---|---|---|---|---|---|
| `overhead-c1` (training job live) | 1 | +1.75 µs [+1.20, +2.10] | +2.00 µs [+1.70, +2.70] | +22.90 µs [+12.70, +28.30] | 2.2 ns |
| `overheadq-c1` (**reported**) | 1 | +1.40 µs [+0.75, +1.90] | +4.75 µs [+4.40, +5.50] | +39.65 µs [+37.90, +41.90] | 3.9 ns |
| `overheadq2-c1` (**repeat**) | 1 | +1.65 µs [+1.10, +1.90] | +5.20 µs [+4.30, +6.10] | +42.95 µs [+38.30, +51.30] | 4.2 ns |
| `overhead-c2` (training job live; CIs unusable) | 2 | +1.65 µs [-0.15, +26.20] | +2.25 µs [-53.00, +36.20] | +19.70 µs [+14.80, +22.50] | 1.8 ns |
| `overheadq-c2` (named test process live) | 2 | +2.00 µs [+1.60, +2.25] | +4.55 µs [+4.20, +5.65] | +46.25 µs [+41.25, +52.00] | 4.5 ns |
| `overheadq3-c2` (**reported**) | 2 | +1.35 µs [+0.30, +2.10] | +3.35 µs [+2.80, +4.20] | +33.25 µs [+32.30, +36.20] | 3.3 ns |
| A's p50 (reported passes) | 1 / 2 | 85.7 / 87.7 µs | 143.9 / 146.4 µs | 783.4 / 798.5 µs | 71.2 / 72.5 ns |

- **The fixed part is small and stable**: 0.6-0.8 µs (the intercept of the 200-row point
  against the per-row slope), 1.35-2.0 µs at 200 rows in all six passes.
- **The per-row part is 1.8-4.5 ns on A's 71-72 ns per row, 2.5-6 %**, and **the passes disagree by
  more than any one pass's CI**: the six 10,000-row deltas span +19.7 to +46.3 µs while the
  widest reported-pass CI is 13 µs. The two passes that ran under foreign load give the smaller
  deltas (+22.9, +19.7 µs); the three quiet passes give +33.3 to +43.0 µs, and the pass with a live test process gives +46.3 µs. The data does not say
  whether load shrinks the delta or the quiet passes inflate it. **The walk's cost at 10,000
  rows is therefore stated as +20 to +46 µs (2.5-5.8 %), not as one number.**
- **What in BA causes it is not isolated.** The A/B spans BA-S2 (counters on latches), S7,
  S12, S13, S14 and S15 (the sliced walk, `SelectRun`, the yield every 64 pages), and the walk
  is the path S15 changed most directly. A per-stage bisection would price it and was not run.
- **BA-S15's 64-page slice was not varied** (it is not a flag): BA-Q11 deferred its
  re-measurement to this file, and this file cannot give it. Whether a 10,000-row walk
  crosses a 64-page boundary depends on the leaf capacity, which the run did not read; the
  50,000 and 100,000-row cell that was built to cross it is invalid (section 9).

## 6. The pk statements at three sizes

Row sizes do not move the pk statements: the pk `SELECT`, `UPDATE` and `DELETE` columns of
section 4 differ by under 0.5 µs across the three sizes, which is what a pk point lookup
that does not scale with rows should look like (README rule 9: shown at three sizes as
evidence of it).

## 7. Result: synced, a single connection

`group` and `strict`, 16 runs per cell, `INSERT` and its replicate. `cores = 1`: from
`overheadq-group-c1` and `overheadq-strict-c1`. `cores = 2`: `group` from `overheadq3-group-c2`,
`strict` from `overhead-strict-c2`.

| durability | cores | rows | A p50, INSERT | B - A p50, INSERT (95 % CI); tps change | A -> B, INSERT | B - A p50, INSERT again (95 % CI); tps change | A -> B, INSERT again | B - A as share of A's p50 |
|---|---|---|---|---|---|---|---|---|
| group | 1 | 200 | 1032.8 µs | +0.90 µs [-3.15, +5.35]; -0.5 % tps [-1.8, +1.0] | 814 -> 824 tps | -2.25 µs [-5.20, +3.10]; -0.1 % tps [-3.6, +2.3] | 808 -> 829 tps | +0.1 % and -0.2 % |
| group | 1 | 1,000 | 1005.2 µs | -1.20 µs [-5.50, +3.20]; -0.8 % tps [-2.8, +4.7] | 845 -> 837 tps | -2.55 µs [-5.40, +1.95]; -0.3 % tps [-3.2, +4.2] | 835 -> 836 tps | -0.1 % and -0.3 % |
| group | 1 | 10,000 | 1025.9 µs | -1.00 µs [-3.40, +1.30]; +2.3 % tps [-0.6, +12.5] | 784 -> 811 tps | -0.60 µs [-4.10, +3.80]; -0.8 % tps [-4.3, +6.9] | 753 -> 760 tps | -0.1 % and -0.1 % |
| group | 2 | 200 | 1265.2 µs | +14.05 µs [+8.80, +24.45]; -0.2 % tps [-1.1, +1.5] | 690 -> 688 tps | +20.05 µs [+10.20, +32.00]; -1.6 % tps [-2.3, -0.1] | 689 -> 676 tps | +1.1 % and +1.6 % |
| group | 2 | 1,000 | 1259.0 µs | +12.95 µs [+2.10, +20.05]; -0.1 % tps [-1.6, +1.2] | 689 -> 690 tps | +15.55 µs [+6.40, +22.40]; -1.4 % tps [-2.3, +0.2] | 694 -> 682 tps | +1.0 % and +1.2 % |
| group | 2 | 10,000 | 1258.0 µs | +19.15 µs [+11.10, +32.40]; -1.2 % tps [-2.2, +0.4] | 694 -> 685 tps | +21.30 µs [+12.40, +29.20]; -2.4 % tps [-3.2, -1.9] | 700 -> 679 tps | +1.5 % and +1.7 % |
| strict | 1 | 200 | 1022.0 µs | +4.95 µs [-1.20, +7.30]; +0.3 % tps [-3.2, +2.0] | 818 -> 814 tps | +1.50 µs [-4.50, +4.00]; -0.4 % tps [-1.6, +2.0] | 828 -> 810 tps | +0.5 % and +0.1 % |
| strict | 1 | 1,000 | 1017.4 µs | +0.35 µs [-1.50, +3.90]; -0.7 % tps [-1.9, +0.4] | 836 -> 834 tps | +2.85 µs [-3.90, +7.80]; -0.1 % tps [-3.3, +1.1] | 825 -> 821 tps | +0.0 % and +0.3 % |
| strict | 1 | 10,000 | 1026.3 µs | -2.25 µs [-4.10, -0.10]; +2.9 % tps [+0.2, +5.0] | 807 -> 826 tps | +3.00 µs [-0.80, +7.15]; -0.2 % tps [-1.2, +0.9] | 826 -> 815 tps | -0.2 % and +0.3 % |
| strict | 2 | 200 | 1012.2 µs | +17.50 µs [+11.50, +29.20]; -2.9 % tps [-4.6, +2.4] | 839 -> 825 tps | +13.05 µs [+6.10, +22.40]; -0.1 % tps [-1.5, +0.6] | 832 -> 826 tps | +1.7 % and +1.3 % |
| strict | 2 | 1,000 | 1016.9 µs | +25.45 µs [+14.40, +34.60]; -2.3 % tps [-6.0, -0.7] | 843 -> 813 tps | +26.65 µs [+7.70, +32.80]; -1.7 % tps [-3.8, +0.3] | 836 -> 817 tps | +2.5 % and +2.6 % |
| strict | 2 | 10,000 | 1022.3 µs | +21.80 µs [+6.60, +30.60]; -0.8 % tps [-1.5, +0.6] | 831 -> 821 tps | +23.35 µs [+2.20, +30.00]; -1.6 % tps [-3.6, +1.6] | 839 -> 824 tps | +2.1 % and +2.3 % |

- **At `cores = 1` the delta is zero within CIs of 2-6 µs half-width** (-2.55 to +4.95 µs on
  commits of 1.0 ms, in both arms, at three sizes, in both durabilities): +0.90, -1.20, -1.00
  (`group`) and +4.95, +0.35, -2.25 µs (`strict`) on the `INSERT`. One of the twelve CIs
  excludes zero on the negative side (`strict`, 10,000 rows, -2.25 µs [-4.10, -0.10]), inside the
  12 µs run-to-run sd. This is what BA-S7's own text says it should be
  ("at `cores = 1` nothing changed", both parts), and it is the one place the A/B checks that
  statement. The load during these passes widened the CIs; it did not produce a sign.
- **At `cores = 2` all twelve (`group`, `strict`, three sizes, two arms) deltas are positive,
  +12.95 to +26.65 µs**, all twelve CIs excluding zero (the lowest lower bounds are +2.1 and
  +2.2 µs). The control (`SET ISOLATION LEVEL`) stays within ±0.10 µs in the same passes.
  Taken together they are resolved; taken singly they sit within one run-to-run sd (12-25 µs)
  of the floor, and that is why the sign across cells is the stronger reading.
- **The first-pass `group` at `cores = 2` read higher**, and the two passes overlap in their CIs:

| durability | cores | rows | A p50, INSERT | B - A p50, INSERT (95 % CI); tps change | A -> B, INSERT | B - A p50, INSERT again (95 % CI); tps change | A -> B, INSERT again | B - A as share of A's p50 |
|---|---|---|---|---|---|---|---|---|
| group, first pass | 1 | 200 | 1263.0 µs | +1.60 µs [-10.50, +13.40]; +0.0 % tps [-1.4, +1.2] | 677 -> 675 tps | +2.80 µs [-4.00, +21.30]; +0.2 % tps [-1.7, +2.1] | 678 -> 679 tps | +0.1 % and +0.2 % |
| group, first pass | 1 | 1,000 | 1033.2 µs | -1.15 µs [-4.50, +5.70]; -1.0 % tps [-5.2, +3.8] | 808 -> 798 tps | +1.55 µs [-5.10, +4.40]; -0.9 % tps [-2.1, +1.5] | 773 -> 760 tps | -0.1 % and +0.2 % |
| group, first pass | 1 | 10,000 | 1038.8 µs | +0.90 µs [-5.40, +6.90]; +1.8 % tps [+0.3, +4.9] | 752 -> 742 tps | +0.85 µs [-3.10, +5.50]; -1.2 % tps [-8.1, +0.2] | 807 -> 800 tps | +0.1 % and +0.1 % |
| group, first pass | 2 | 200 | 1014.8 µs | +31.65 µs [+18.00, +36.70]; -3.1 % tps [-4.4, -1.3] | 842 -> 816 tps | +31.80 µs [+18.70, +34.70]; -3.6 % tps [-4.5, -2.0] | 836 -> 814 tps | +3.1 % and +3.1 % |
| group, first pass | 2 | 1,000 | 1023.0 µs | +29.65 µs [+22.35, +35.80]; -2.4 % tps [-4.0, -0.6] | 827 -> 795 tps | +29.70 µs [+25.20, +39.00]; -2.8 % tps [-4.1, -0.9] | 835 -> 814 tps | +2.9 % and +2.9 % |
| group, first pass | 2 | 10,000 | 1019.1 µs | +20.60 µs [+5.50, +24.10]; -1.6 % tps [-6.2, +1.1] | 823 -> 811 tps | +27.05 µs [+9.10, +30.70]; -1.9 % tps [-3.8, +2.1] | 824 -> 810 tps | +2.0 % and +2.7 % |
| strict, first pass | 1 | 200 | 1023.8 µs | +1.10 µs [-2.60, +3.10]; +0.6 % tps [-2.1, +4.3] | 817 -> 819 tps | +1.05 µs [-1.40, +2.80]; +0.3 % tps [-1.6, +1.9] | 818 -> 826 tps | +0.1 % and +0.1 % |
| strict, first pass | 1 | 1,000 | 1015.3 µs | +3.95 µs [-1.90, +8.40]; +0.1 % tps [-3.4, +1.6] | 824 -> 821 tps | -1.15 µs [-2.30, +2.90]; +1.3 % tps [-1.3, +10.0] | 811 -> 796 tps | +0.4 % and -0.1 % |
| strict, first pass | 1 | 10,000 | 1053.8 µs | +1.55 µs [-2.40, +5.10]; -2.2 % tps [-8.8, +0.8] | 640 -> 623 tps | -3.00 µs [-9.50, +3.60]; +0.3 % tps [-10.5, +3.4] | 654 -> 714 tps | +0.1 % and -0.3 % |

  `overhead-group-c2` ran 03:19-03:36 with the host loaded (load to 9.5) and A's commit p50
  at about 1,015 µs; `overheadq3-group-c2` ran 08:28-08:43 quietly with A's commit p50 at about
  1,260 µs. **The same engine's commit was 24 % slower later in the session, on a device with
  nothing else of ours running**, and B's added cost was smaller in the slower pass (+13 to
  +21 µs against +21 to +32 µs). The first-pass `group` at `cores = 1` shows the same wander: A's p50 reads
  1,263 µs at 200 rows (loaded host) and 1,033-1,039 µs at the other two sizes. This is the cross-pass
  scatter of the synced arm, stated, not explained.
- **Reading**: the hand-off of a sync from the reactor to the writer thread (BA-S7) costs one
  connection 13-32 µs per commit at `cores = 2`, 1.0-3.1 %. It does not exist at `cores = 1`,
  where S7 changed nothing, and the data agree.
- **`SHOW META`** has no clean control reading at `strict`, `cores = 2` (it carries the +5 µs
  reply block); the pass has no `SET ISOLATION LEVEL` arm, so its floor is the replicate's.

## 8. The 10,000-row stall

The throughput columns of two 10,000-row shapes read -19.5 % and -25.8 % (the 20-row
`INSERT`, `cores` 1 and 2) while their p50 is flat (-0.10 and +0.95 µs). The cause is one long
statement per run, which the A/B records as each arm's `max_us`. The longest statement of any
timed arm (controls excluded), per run, 16 runs per cell:

| rows | `cores` | A: median, range of the 16 runs | A: runs over 40 ms | B: median, range | B: runs over 40 ms | arms holding B's long statement |
|---|---|---|---|---|---|---|
| 200 | 1 | 10 ms, 7-32 ms | 0 of 16 | 6 ms, 4-19 ms | 0 of 16 | - |
| 1,000 | 1 | 14 ms, 10-26 ms | 0 of 16 | 4 ms, 2-7 ms | 0 of 16 | - |
| 10,000 | 1 | 22 ms, 9-88 ms | 3 of 16 | 101 ms, 3-118 ms | 15 of 16 | 20-row `INSERT` 11, walk 4 |
| 200 | 2 | 12 ms, 7-20 ms | 0 of 16 | 3 ms, 2-14 ms | 0 of 16 | - |
| 1,000 | 2 | 17 ms, 12-46 ms | 1 of 16 | 4 ms, 2-10 ms | 0 of 16 | - |
| 10,000 | 2 | 18 ms, 14-34 ms | 0 of 16 | 122 ms, 62-226 ms | 16 of 16 | 20-row `INSERT` 14, walk 2 |

- **At 200 and 1,000 rows B's longest statement is at most 19 ms and A's at least 7 ms**, over
  all 64 runs (B 2-19 ms, A 7-46 ms): BA took a periodic 10-17 ms stall out of the small cells. This is the
  direction BA-S13 (a carve that persists page 0 alone) was written for; the run does not isolate
  that stage.
- **At 10,000 rows B has a 62-226 ms statement in 31 of 32 runs (medians 101 and 122 ms) and A's
  longest has a median of 22 and 18 ms** (range 9-88 ms). **Section 4's walk and 20-row `INSERT` means carry it**: the walk's mean delta
  is +62 µs against its p50's +39.65 µs, and at `cores = 1` `B - A` elapsed for the 20-row
  `INSERT` arm is a median +80.6 ms with 13 of 16 runs over +40 ms (`insert again`: -12 ms, because A's 10-15 ms
  stall lands in that arm).
- **Attribution.** Three probes of a fresh `cores = 1` relaxed server of each binary
  (`stall_probe.py`; its row count argument is not recorded in the probe files, the 0.06 s
  load matches 10,000 rows) show, for B, one statement of 54, 64 and 73 ms in the second walk
  arm at 1.8, 1.9 and 1.4 s, where `contention_checkpoint_runs` goes from 0 to 1 and
  `contention_checkpoint_longest_us` reads 53.2, 63.5 and 71.9 ms; `contention_carve_longest_us`
  reads 1.9-3.9 ms. A has no checkpoint counter; its probes show no statement over 20 ms in two
  of three and one 24 ms statement in the third. **The stall is the checkpoint**, and n is three.
- **It is not a per-statement cost.** A stall of 60-120 ms in an arm of 0.3-0.5 s moves a
  throughput column by 15-25 % and a mean by 20-60 µs while moving the p50 by nothing. The
  census (its own file) reads the same checkpoint at the relaxed class as a median 123-209 ms
  longest, against BA-S4's 19-121 ms.
- **What is not known**: whether BA made the checkpoint longer (the A/B reads A's median longest of 18-22 ms
  against B's 101-122 ms), or moved work from a carve that used to sync the whole pool into the
  checkpoint (S13's text: before it, "a carve synced the whole pool"). The two readings
  predict the same A/B. The checkpoint half is BI-Q1's and is not built.

## 9. What is invalid, and why

- **The 50,000 and 100,000-row walk cell (`overheadbig-c1`, `overheadbig-c2`) is invalid and
  incomplete.** It was meant to run the walk past one 64-page slice (BA-S15's yield), 12 runs,
  600 statements per arm per server, `cores` 1 and 2. At `cores = 1`, 4 of 12 runs of the
  50,000-row size completed and the driver then died on a client socket timeout in the setup
  `INSERT` (`tools/kwp.py`, `_recv_frame`); the 100,000-row size never ran. `cores = 2` died in
  its first setup with the same error. **The four runs that exist are not evidence**: the third
  carried a live `cmake --build -j8` and four `cc1plus` from another session at its start, the
  load rose to 2.5, and its control arm read 121.9 µs (A) and 121.6 µs (B) against 37.5-37.8 µs
  in the first two. The cause of the timeouts is not yet known; the overlapping build is one
  candidate, and nothing here excludes another.
  The summaries are kept under `archive/.../invalid-walk-cell-50000-rows/` for the record, and
  no number from them appears in this file.

  **The reproduction (2026-10-10, 14:44-14:45 UTC):**
  - The same driver and binaries at `cores = 2` (server on CPUs 4,5), 50,000 and 100,000 rows,
    two runs each. The load before it was 1.84, and it read 1.71-2.09 across the runs.
  - **All four runs completed, with no timeout and no error.**
  - The control arm read 37.9-38.2 µs on both binaries.
  - range100 read B over A by -2 % to +4 % at 50,000 rows, and +3.7 to +4.3 % at 100,000 rows
    (8,308-8,322 µs against 8,618-8,683 µs). That sits beside the per-row walk cost above.
  - Two runs are too few for a CI, and no number from them is used elsewhere in this file.
  - Read: the setup timeouts did not recur on a quiet host. That points to the overlapping
    build, without proving it.
  - The log is `/home/cdkbs/bench-runs/ba-close/repro.log`, and its summaries are under
    `repro-c2/`. Neither is archived.

- **`overhead-c2`** (the first `cores = 2` pass, under the training job): CIs of tens of µs;
  not used for any number. **`overheadq-c2`**: a named test process in 411 of 711 sampler
  seconds; superseded by `overheadq3-c2`.
- **`SHOW META` as a control**: invalid since BA-S2 added the reply block.
- **Throughput columns at 10,000 rows** for the 20-row `INSERT` (-19.5 %, -25.8 %), the
  replicate (+7.8 %, +10.4 %) and the walk are inside the stall's floor (section 8); only
  their p50 is a reading.
- **The synced `cores = 1` passes** ran under other sessions' `ctest -j8`; their CI
  half-widths are 2-6 µs because of it, and they resolve nothing smaller than that.
- **A first run of scenario 0** (08:44-08:49 UTC) was deleted by `run_s0.sh` (`rm -rf` of its own
  directory) and re-run at 08:54-08:57; the files do not record why.
- **The stall probe's row count** and its A/B scale (n = 3) are the weak links of section 8.
- **The tree's cleanliness at the measurement** was recorded by the earlier run and could not
  be re-checked in this write-up. The branch has moved since (the second bullet under the thesis).
- **Raw per-statement files** (`*.raw.json.gz`, 528 files, 28 MB) and `host-samples.jsonl`
  (8.5 MB) are not archived, to keep the archive to 7 MB. The matrices regenerate from the
  summaries with `drivers/ba_matrix.py`; the "Δ mean" columns of the long tables
  (`tables/full-*.md`) came from the raw files and cannot be regenerated from the archive.

## 10. Scenario 0

BA-S14's exit row: "scenario 0's `c8-s` cell with no refusal". `tools/scenario0_stockmarket.py`
(unchanged since `9a0525d`), the arguments and cell order of
`archive/scenario0-v2.7.0-531-g9a0525d/run_s02.sh`: `--users 100 --accounts-per-user 3
--assets 30 --traders 8 --txn-per-user 50 --verify 200 --seed 1 --sync`, three passes of four
cells, the B binary, ports 15700-15703, no CPU pinning (as there), `buffer_pool_frames
= 65536` (required since BE; the `9a0525d` run predates it), `log_level = warn`. Host: load
1.19-1.52 before each cell, no competing process at any cell (`*.competing` empty), the gate
(no `ctest`/`cc1plus`/`kds_tests`) waited 0 s.

| cell | cores | durability | pass 1 | pass 2 | pass 3 | median | spread (max/min - 1) | `9a0525d` median (spread) | change |
|---|---|---|---|---|---|---|---|---|---|
| `s0-c1-g` | 1 | group | 651.1 tps | 650.4 tps | 662.6 tps | **651.1 tps** | 1.9 % | 626.0 tps (3.8 %) | +4.0 % |
| `s0-c8-g` | 8 | group | 698.4 tps | 697.7 tps | 733.0 tps | **698.4 tps** | 5.1 % | 661.0 tps (6.8 %) | +5.7 % |
| `s0-c1-s` | 1 | strict | 173.4 tps | 177.5 tps | 179.3 tps | **177.5 tps** | 3.4 % | 169.4 tps (6.1 %) | +4.8 % |
| `s0-c8-s` | 8 | strict | 659.3 tps | 623.1 tps | 586.3 tps | **623.1 tps** | 12.5 % | 331.7 tps (20.0 %) | +87.9 % |

**`s0-c8-s` completed 5,000 transactions in all three passes with 0 engine errors in every
phase, 0 torn transactions, 0 rolled back, driver exit 0, and `--verify 200` matched on all 200
accounts (it matched in all twelve runs).** At `9a0525d` (`v2.7.0-531-g9a0525d`, 205 commits and several milestones
before B) the same cell refused one trade insert `TXN_CONFLICT retryable=1 btree descent
for key 113 ... gave up after 5 attempts` in each of its three passes (3 of 3, torn 1, exit 1).
Its other nine runs were error-free, and all twelve are here.

The `txn` phase, a business transaction of 4 statements over 5,000 transactions (median of
the three passes of each percentile):

| cell | ops | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| `s0-c1-g` | 5,000 transactions | 9,267 µs | 10,452 µs | 11,046 µs | 19,015 µs | 24,858 µs | 40,587 µs |
| `s0-c8-g` | 5,000 transactions | 7,860 µs | 9,568 µs | 10,174 µs | 18,144 µs | 23,034 µs | 58,727 µs |
| `s0-c1-s` | 5,000 transactions | 4,468 µs | 35,661 µs | 39,135 µs | 76,693 µs | 93,788 µs | 126,814 µs |
| `s0-c8-s` | 5,000 transactions | 6,415 µs | 9,928 µs | 11,104 µs | 21,168 µs | 28,092 µs | 39,774 µs |

- **What the data supports**: no refusal reached the client in 12 of 12 runs. **What it does not
  support**: that the engine took no structural refusal. The driver did not read `SHOW META`, so
  `contention_refused_btree_descend` and `contention_structural_reruns` are not recorded for
  these cells (the census file counts 29 of each in 675 runs). "No refusal" here is "none
  visible".
- **`c8-s`'s three passes fall monotonically** (659.3, 623.1, 586.3 tps, 12.5 %). Three points
  do not make a trend and the data does not say whether it is warm-up of the host, the
  device, or the engine; at `9a0525d` the same cell's passes rose (281.3, 331.7, 337.6 tps).
- **The +87.9 % of `c8-s`** is consistent with BA-S7 part 2 taking the `strict` sync off the
  reactor, but 205 commits lie between the two runs and this file does not isolate the stage.
  The other three cells moved +4.0 to +5.7 %, near the `9a0525d` spreads of 3.8-6.8 %, and are
  not read as findings.

## 11. What this run teaches about the engine

- **The hot single-connection path is not where BA's cost is.** pk statements move by
  under 1.25 µs, an unsynced pk `INSERT` by nothing. BA-S7's statement for `cores = 1`
  ("nothing changed") holds on the sync path to within 2-6 µs on a 1 ms commit. S1c's priced
  cost (nanoseconds) does not account for the 0.5-0.95 µs the pk `SELECT` pays: something else on
  that path moved, and the A/B cannot say what.
- **The one regression-shaped result is per row and in the read path**: the whole-relation
  walk, +2-4.5 ns per row of 71, with cross-pass scatter wider than any CI. It is the stage
  to bisect if the walk matters, and BA-S15 changed it; nothing in this run isolates S15.
- **BA's synced cost is a hand-off, and it is bought back at 16 sessions.** 13-32 µs per
  commit for one connection at `cores = 2`; the census file reads 1.7-4.0x (`group`) and 7.7-8.8x (`strict`) over BA-S4 in the synced
  write shapes at 16 sessions and `cores = 2` on the same binary (a comparison that carries the
  census file's cross-day shift).
- **The periodic stall changed shape rather than size.** The 10-17 ms carve stall of the small
  relations is gone; at 10,000 rows a checkpoint of 62-226 ms is the longest statement of
  nearly every run. The checkpoint half is BI-Q1's and unbuilt; the census file's P12
  reading carries it.
- **The synced commit is 95 % device** (section 3), and the device's share moved by 24 % within
  a session. A synced overhead figure under a few percent is below what a device-dominated
  latency can attribute to the engine without a same-minute control.
