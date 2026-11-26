# DT close: the write walk gives its core back, and the checkpoint walks in

Measured on `v3.4.17-153-gd172085e8` (Release), the milestone-close A/B of
`instructions/v3.2.0/workorder-dt-write-path-yield.md`: arm A is the commit DT
opened from (`60dc593af`, `v3.4.17-150-g60dc593af`), arm B the closing code
(`d172085e8`), `cores = 1`, `relaxed`.

**Finding, in one paragraph.** *The yield works, and with the default
checkpoint cadence it does not yet deliver DT-Q1's bar on this host.* A
walked write that matches nothing starves four hot point-read sessions on A
(service p99 **245.2 and 265.1 ms**, **34.6 and 33.8 reads/s** of the 1,000
offered). On B the same cell leaves them **1,000.2 and 555.6 reads/s** and a
service p99 of **1,621.7 µs and 10,260.8 µs** (two runs), but a service **max of
1.39 s and 7.66 s**, because B's walks stall for seconds at a time: B's
no-match walk takes **129-133 ms at p0-p50 and 1.0 to 16.2 s at p95-p99** where
A's takes 118-121 ms and p99 145-161 ms. Beside a walk that writes ~50,000
rows (`ivu`) the hot p99 is **50.3 and 50.2 ms** on B against **658.9 and 640.0
ms** on A, with a max of 72.5 ms and 1,258.8 ms. **A probe with the cadence
checkpoint off (`checkpoint_interval_ms = 0`, one run each, not part of the
A/B) removes the stalls on B**: hot p99 **332.4 µs**, max 2,900.8 µs, 1,000.2
reads/s, 156 walks in the window of 129-137 ms (A with the same key off:
p99 133.5 ms, 734 reads). What the default-cadence runs show beside the
stalls is a **log volume 11 to 18 times A's for a walk that writes no row**
(3,020 MB and 1,909 MB of page images in 20 s against 169 and 165 MB; 368,312
images against 20,672) and **WAL sync-gate waits totalling 12.65,
15.18, 18.70 and 22.14 s** in four of the cells that stall (19-45 waits each),
the order of the stalls; the probe's 0 images
and 0 MB log are consistent with a checkpoint that runs between two slices and
leaves every page owing another image (`wal.md` §10), **but the cause of the
multi-second polls was not isolated here**. DT-R4 holds: a point `UPDATE`,
`INSERT`, `DELETE` and `SELECT` cost on B within **-0.9 to +0.7 %** (-70 to +40
ns a statement) of a second copy of A (the control), medians of 15 rounds, and
+0.5 to +2.9 % of A, where the control itself is +0.5 to +2.9 % of A (section
7). A statement that does pause (a keyless walked `UPDATE` of 100 rows) costs
+13.1 % on a btree and +0.6 % on a heap. Section 3's per-walk times and
section 9's list of what was not measured carry the rest. This file does not
rule on whether the milestone closes.

## 1. Stamp

| | |
|---|---|
| Executed | 2026-11-26, 18:24-18:53 UTC. Cells 1-3: 18:24:47-18:41:13; cell 4 (`dr_close.py`): 18:41:15-18:43:56 (160.8 s); scaling probe 18:47-18:50; checkpoint-off probe 18:50-18:53. Written 18:55 UTC. Start of the session 18:20 UTC (build 18:20-18:21) |
| Branch / worktree | `worktree-dt-write-path-yield`, worktree `dt-write-path-yield` |
| Commit (arm B) | `d172085e86997a66ecf559bb91beef9c618b2747`, **`git describe --tags` = `v3.4.17-153-gd172085e8`**, committed 2026-11-26 13:20:11 -0500. The version of record is `v3.5.0` (`07f4552a`), whose tag is not in this clone, so describe prints against `v3.4.17` |
| Commit (arm A) | `60dc593af`, `v3.4.17-150-g60dc593af`, the commit DT opened from (3 commits before B) |
| Tree | tracked sources clean. Uncommitted at the time: edits under `docs/`, `manual/` and `CLAUDE.md` (not build inputs; another session's), untracked `tools/dt_close.py` (the driver, new) and this file with its archive |
| Binary B | built here: `cmake --build build-release -j 12 --target kds_server` (24 s), source `build-release/kds_server` mtime 2026-11-26 13:21:24 -0500, **after** the commit (13:20:11). Measured from the copy `~/bench-runs/dt/close/bin/kds_server.B`, **sha256 `145700aedddebda5169600199782c835c9894332de0a1062b0d0566dd1d31d31`** (every B JSON's `bin_sha256` agrees) |
| Binary A | `~/bench-runs/dt/work/kds_server.dt` was gone (DT-S0's driver removes its work directory), but `build-release/kds_server` still held A's build (mtime 2026-11-26 12:00:57 -0500, built before B's rebuild), and its sha256 **`d9e683d9b312c7e2f3aa4ddf26d81a1b215afc10ce840ae7abe8ac65c22da9cc` equals the hash DT-S0 recorded**, so it was copied before the rebuild to `~/bench-runs/dt/close/bin/kds_server.A` and no rebuild of A was needed. The hash was verified before use |
| Host | 24 logical CPUs (Intel Core Ultra 7 270K Plus), 30 GiB, Linux 7.3.4-300.fc45; the same host as DT-S0. Reactor pinned to CPU 1, clients round-robin on CPUs 2-7 (cells 1-3, `dt_close.py`); cell 4: server on CPU 2, driver on CPUs 0 and 4 (`dr_close.py` defaults) |
| Data device | `/dev/mapper/fedora-root`, **xfs** (`df -T ~/bench-runs/dt/close`), on the NVMe of DT-S0. Not tmpfs. Data files `~/bench-runs/dt/close/work/cell/dt.db` (cells 1-3, 1,056,440,320 B = 128,960 pages) and `~/bench-runs/dt/close/w4` (cell 4) |
| Build type | Release (`build-release`, `CMAKE_BUILD_TYPE=Release`), both arms |
| Server configuration | `durability = relaxed`; `cores = 1`; `buffer_pool_frames` 160,000 (cells 1-3) or 131,072 (cell 4); `log_level = warn`; no other non-default key, **except the checkpoint-off probe (section 8), which adds `checkpoint_interval_ms = 0`** |
| Ports | 32771 (cells 1-3, scaling, probe), 32781-32782 (cell 4); each checked free before it was used |
| Drivers | `tools/dt_close.py`: a copy of `tools/dt_s0_premise.py` (which stays as it was) with three additions: kind `u` (a walker whose statement is `--walk-sql`), cells `ivu`, `aw`, `au`, `ar` (the `a...` cells have no hot sessions), and `--conf-extra`. The cells `ivw`, `ivr`, `i` run DT-S0's code unchanged. `--conf-extra` was added after cells 1-3 had started (it defaults to empty and cells 1-3 never used it). `tools/dr_close.py --part ab` (DR's close driver) **unmodified** for cell 4. `bench/docs/` stays closed (`bench/README.md`); each driver's docstring is its documentation |
| Raw output | `bench/v3.5.0/archive/dt-close-v3.4.17-153-gd172085e8/`: `dt-<cell>-<arm>-r<run>.json` and `.s.log` (24 cells), `scaling/` and `probe-ckpt-off/` (8 cells), `dr4-ab.json` and `dr4.beat` (cell 4), `driver-*.log`, `summary-*.json`, `scripts/` (the four chain scripts and the build log) |

## 2. What was measured, and what was cut

Relation `dt_big (id int64, c0 int64, pad char(1000))`, 900,000 rows, BTREE,
loaded once into a template by arm A's binary (9.1 s); **B mounted a template
written by A** (the on-disk format did not change between the arms). Every
cell mounts a fresh copy of the template. Window 20 s after a 4 s warm-up; the
pool holds the relation (160,000 frames over 128,960 pages) and the page cache
is warm (`mincore` 1.00 before every cell), so no device read is in any cell
(`io.page_read` is 0.00 % of every window). The control session walks the
relation once before the window (the pre-warm) and reads `sys.wait_stats` and
`sys.meta` at both ends of it.

| cell | hot | co-located load | runs per arm | what the cell is |
|---|---|---|---|---|
| `ivw` | 4 sessions, 1,000 point reads/s in all, keys 1..700 | 1 walker, closed-loop, `UPDATE dt_big SET c0 = c0 WHERE c0 = -1` (writes nothing) | 2 | DT-S0's cell, unchanged |
| `ivr` | same | 1 scanner, closed-loop, `SELECT id FROM dt_big WHERE c0 = -1` | 2 | DT-S0's cell, unchanged |
| `i` | same | none | 2 | DT-S0's cell, unchanged |
| `ivu` | same | 1 walker, closed-loop, `UPDATE dt_big SET c0 = c0 + 1 WHERE c0 >= 1 AND c0 <= 50000` (writes 49,9xx rows a walk) | 2 | new: **the walk that writes rows** |
| `aw` | none | the no-match walker alone | 2 | new: the walk's own cost |
| `au` | none | the writing walker alone | 2 | new: the walk's own cost |
| scaling | none | the writing walker alone, `c0 <= 5000` and `c0 <= 15000` | 1 per arm | rows written per walk |
| probe | `ivw`, `au` | as above, **`checkpoint_interval_ms = 0`** | 1 per arm | whether the cadence checkpoint is what stalls B |
| cell 4 | none | `tools/dr_close.py --part ab --rounds 15` | 15 rounds x 3 arms | DT-R4 |

**Why the writing walker's statement is narrowed.** The whole-relation form
`UPDATE dt_big SET c0 = c0 + 1 WHERE c0 >= 0` (900,000 rows) was tried first on
A in a scratch trial (not archived): the engine refused every attempt with
`transaction reached max_locks_per_txn (65536 borrows)` after 0.35-0.40 s, so a
single walked write cannot write more than 65,536 rows. The statement used,
`c0 >= 1 AND c0 <= 50000`, is a non-key predicate: it walks all 128,960 pages
and writes 49,9xx rows (the count in the reply, 49,913-50,000, rises by a row a
walk as `c0` moves). That is 5.5 % of the relation's rows and about 7,150 of
its pages.

**Interleaving.** The unit of interleave is one arm's whole set of six cells
(`ivw, ivr, i, ivu, aw, au`, in that order, one fresh server each): A r0, B r0,
B r1, A r1. Cell by cell alternation was not done, so a drift in the host
between the first and last cell of a set is not cancelled inside a set. Cell 4
rotates A, A2, B through six orders over 15 rounds (`dr_close.py`'s own).

**What was cut.** Nothing from the order the work order gave: all cells ran
two runs per arm and cell 3 ran both statements. Cell 4 ran 15 rounds, not
five. Two cells were **added** (scaling, probe) after cells 1-3 showed B
stalling, and ran once per arm each. Time: session start to the last cell
18:20-18:53 UTC (33 min), 24 s of which was the build; **hold for competing
builds or suites: none** (`pgrep` empty at every invocation, section 4). The
driver's own load gate (1-minute load below 1.5) waited a total of 200 s over
all 32 cells, the load being the previous cell's own; the file does not count
that as a hold. One chain script (`run5.sh`) was started with a path bug and
once more after being killed by its own `pkill`; neither started a cell.

**Row-set sweep (rule 9) was not done in the form the rule states.** The
subject is a walk's length: the relation is fixed at 900,000 rows (128,960
pages) and the rows written per walk are varied in `ivu`/`au` (0, 5,000,
15,000, ~50,000). A walk of 200, 1,000 or 10,000 rows is shorter than one
64-leaf slice (a pause is taken only past 64 leaves) and takes none; cell 4's
point statements are by key and do not scale with rows. 200/1K/10K were not
run.

## 3. The effect: hot sessions beside a walker

Hot sessions' **service time** (from the send), in microseconds, 20 s window,
4 sessions. Reads/s is derived (`n / 20 s`).

| cell | arm | run | n | reads/s | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ivw | A | r0 | 693 | 34.6 /s | 61.5 µs | 119,492.5 µs | 120,745.4 µs | 131,834.3 µs | 245,226.4 µs | 254,368.5 µs | 254,368.5 µs |
| ivw | A | r1 | 675 | 33.8 /s | 63.2 µs | 119,226.2 µs | 120,824.8 µs | 144,889.6 µs | 265,067.1 µs | 267,141.8 µs | 267,141.8 µs |
| ivw | B | r0 | 11,112 | 555.6 /s | 84.2 µs | 195.3 µs | 560.0 µs | 973.2 µs | 10,260.8 µs | 1,232,408.3 µs | 1,389,095.8 µs |
| ivw | B | r1 | 20,004 | 1,000.2 /s | 21.2 µs | 177.4 µs | 212.9 µs | 910.3 µs | 1,621.7 µs | 691,310.2 µs | 7,655,877.1 µs |
| ivr | A | r0 | 20,004 | 1,000.2 /s | 61.0 µs | 112.5 µs | 123.7 µs | 239.6 µs | 286.6 µs | 541.9 µs | 2,483.1 µs |
| ivr | A | r1 | 20,005 | 1,000.2 /s | 48.6 µs | 109.7 µs | 123.3 µs | 232.7 µs | 274.7 µs | 526.7 µs | 2,851.5 µs |
| ivr | B | r0 | 20,005 | 1,000.2 /s | 58.2 µs | 116.6 µs | 127.1 µs | 277.5 µs | 334.3 µs | 518.1 µs | 2,369.9 µs |
| ivr | B | r1 | 20,005 | 1,000.2 /s | 51.3 µs | 111.8 µs | 125.2 µs | 238.7 µs | 288.7 µs | 476.1 µs | 2,851.0 µs |
| i | A | r0 | 20,004 | 1,000.2 /s | 48.2 µs | 253.2 µs | 277.5 µs | 441.1 µs | 507.6 µs | 586.5 µs | 1,440.8 µs |
| i | A | r1 | 20,004 | 1,000.2 /s | 48.2 µs | 181.2 µs | 190.5 µs | 311.9 µs | 382.7 µs | 1,988.6 µs | 2,900.6 µs |
| i | B | r0 | 20,004 | 1,000.2 /s | 52.9 µs | 331.4 µs | 382.7 µs | 474.5 µs | 548.1 µs | 2,144.3 µs | 3,221.8 µs |
| i | B | r1 | 20,005 | 1,000.2 /s | 42.4 µs | 250.3 µs | 273.5 µs | 428.9 µs | 509.5 µs | 1,586.0 µs | 2,505.5 µs |
| ivu | A | r0 | 140 | 7.0 /s | 299,979.2 µs | 571,903.5 µs | 586,141.3 µs | 637,851.0 µs | 658,915.0 µs | 658,965.6 µs | 658,965.6 µs |
| ivu | A | r1 | 157 | 7.8 /s | 230.3 µs | 553,978.9 µs | 582,866.4 µs | 631,425.6 µs | 639,996.1 µs | 640,037.3 µs | 640,037.3 µs |
| ivu | B | r0 | 20,006 | 1,000.3 /s | 115.2 µs | 299.5 µs | 785.6 µs | 2,896.1 µs | 50,341.4 µs | 54,224.0 µs | 72,503.8 µs |
| ivu | B | r1 | 13,546 | 677.3 /s | 102.5 µs | 344.0 µs | 870.5 µs | 2,923.5 µs | 50,230.3 µs | 993,593.5 µs | 1,258,832.9 µs |

**Reading it.** (1) The read walk (`ivr`) is where B and A are the same: hot
p99 334.3 and 288.7 µs on B, 286.6 and 274.7 µs on A. B is higher by 47.7 and
14.0 µs at p99 and by 3.4 and 1.9 µs at p50, but DT-S0 ran two runs of the
same engine in this cell and they differed by 63.7 µs at p99 (288.0 and 351.7
µs), so that delta is inside a noise floor this host has already shown. `i`
likewise: B p99 548.1 and 509.5 µs against A's 507.6 and 382.7 µs, A's own two
runs 124.9 µs apart. No delta in `i` or `ivr` is a result.
(2) `ivw` and `ivu` are the effect, and they move by orders of magnitude on the
**body** of the distribution: p50 from 120,745 and 120,825 µs on A to 560.0 and
212.9 µs on B (`ivw`), and from 586,141 and 582,866 µs to 785.6 and 870.5 µs
(`ivu`). (3) They do not move the **tail** the same way: B's max is 1.26-7.66 s in
three of the four B runs where the p99 is 1.6-10.3 ms (`ivw`) or 50 ms (`ivu`),
and one B run in each (`ivw` r0, `ivu` r1) completed only 555.6
and 677.3 reads/s of the 1,000 offered. The hot sessions' schedule-based
view, the open-loop user's: `ivw` B p99 8,488,910.2 and 12,931,360.7 µs, max
9,459,833.9 and 13,116,376.9 µs; `ivu` B 54,932.6 and 5,313,516.9 µs, max
82,068.2 and 6,465,091.0 µs (A: p99 23.1-23.9 s in both cells).

**The walker's own time per walk** (its own clock; seconds per walk, all walks
the walker finished, so the walk that was running at the window's end is not
included and a walk begun in the warm-up is). With 24-190 walks a cell the p99
is within one or two walks of the max: read it as the max.

| cell | arm | run | walks finished (in window) | commits | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|---|---|
| ivw | A | r0 | 192 (167) | 162 | 118.0 ms | 120.0 ms | 121.0 ms | 128.0 ms | 161.0 ms | 810.0 ms |
| ivw | A | r1 | 192 (165) | 160 | 118.0 ms | 119.0 ms | 121.0 ms | 126.0 ms | 145.0 ms | 841.0 ms |
| ivw | B | r0 | 59 (42) | 39 | 129.0 ms | 131.0 ms | 131.0 ms | 1,003.0 ms | 13,190.0 ms | 13,190.0 ms |
| ivw | B | r1 | 39 (24) | 20 | 130.0 ms | 131.0 ms | 133.0 ms | 1,039.0 ms | 16,247.0 ms | 16,247.0 ms |
| ivr | A | r0 | 539 (460) | 0 | 44.0 ms | 44.0 ms | 44.0 ms | 45.0 ms | 47.0 ms | 66.0 ms |
| ivr | A | r1 | 539 (460) | 0 | 44.0 ms | 44.0 ms | 44.0 ms | 45.0 ms | 46.0 ms | 48.0 ms |
| ivr | B | r0 | 538 (459) | 0 | 44.0 ms | 44.0 ms | 44.0 ms | 45.0 ms | 47.0 ms | 66.0 ms |
| ivr | B | r1 | 539 (460) | 0 | 44.0 ms | 44.0 ms | 44.0 ms | 45.0 ms | 46.0 ms | 49.0 ms |
| ivu | A | r0 | 79 (65) | 62 | 260.0 ms | 275.0 ms | 300.0 ms | 334.0 ms | 991.0 ms | 991.0 ms |
| ivu | A | r1 | 80 (66) | 63 | 260.0 ms | 274.0 ms | 293.0 ms | 320.0 ms | 1,023.0 ms | 1,023.0 ms |
| ivu | B | r0 | 60 (52) | 51 | 279.0 ms | 287.0 ms | 303.0 ms | 1,084.0 ms | 1,117.0 ms | 1,117.0 ms |
| ivu | B | r1 | 37 (28) | 27 | 277.0 ms | 293.0 ms | 309.0 ms | 1,323.0 ms | 18,199.0 ms | 18,199.0 ms |
| aw | A | r0 | 193 (168) | 163 | 118.0 ms | 119.0 ms | 120.0 ms | 125.0 ms | 161.0 ms | 811.0 ms |
| aw | A | r1 | 194 (169) | 164 | 117.0 ms | 119.0 ms | 119.0 ms | 124.0 ms | 155.0 ms | 806.0 ms |
| aw | B | r0 | 119 (103) | 101 | 128.0 ms | 129.0 ms | 132.0 ms | 749.0 ms | 1,046.0 ms | 3,956.0 ms |
| aw | B | r1 | 8 (1) | 2 | 129.0 ms | 131.0 ms | 372.0 ms | 16,398.0 ms | 16,398.0 ms | 16,398.0 ms |
| au | A | r0 | 80 (66) | 63 | 260.0 ms | 276.0 ms | 292.0 ms | 327.0 ms | 1,016.0 ms | 1,016.0 ms |
| au | A | r1 | 63 (49) | 46 | 262.0 ms | 277.0 ms | 298.0 ms | 1,022.0 ms | 1,958.0 ms | 1,958.0 ms |
| au | B | r0 | 3 (1) | 2 | 4,648.0 ms | 4,648.0 ms | 12,657.0 ms | 17,005.0 ms | 17,005.0 ms | 17,005.0 ms |
| au | B | r1 | 49 (41) | 40 | 272.0 ms | 292.0 ms | 307.0 ms | 1,538.0 ms | 8,548.0 ms | 8,548.0 ms |

**The walk's own cost, alone (cell 3).** With no other session, B's best walk
is **128-129 ms** (no-match) against A's **117-118 ms**, +10.0 to +12.0 ms or +9
to +10 % (`aw`), and **272-279 ms** (writing) against **260-262 ms**
(`ivu`/`au` p0 on A), +10 to +19 ms or +4 to +7 %. Over the no-match walk's
~2,085 slices (`sched_foreground_polls`: 325,213 polls in the probe's 156
walks, section 8) that is about **5 µs a pause**, derived. That is the price of
the pause itself: the statement line's re-parse, the re-compile and the resume's
descent per slice, together. It is a fixed, small price. The p25 and p50 of
the no-match walk on B in `ivw` (131 and 131-133 ms) and `aw` r0 (129 and 132
ms) are 8 to 12 % above A's (119-121 ms); **what B adds is the tail**: `aw` B r0's
walks run 128 ms to 3,956 ms, B r1's first eight walks give a p50 of 372 ms
and one walk of 16.4 s, and `au` B r0 finished three walks of 4.6, 12.7 and
17.0 s where A finished 80 of 260-1,016 ms. A's own `au` r1 has a p99 of
1,958 ms and A's `ivu`/`au` tails sit near 1 s, so a walk over a second is
not new on B; but B has **2 to 7 walks over a second in every B cell** (`ivw`
3, 2; `ivu` 5, 6; `aw` 3, 3; `au` 3, 7) where A has 0 to 4 (A: `ivw` 0, 0;
`ivu` 0, 1; `aw` 0, 0; `au` 1, 4), and a walk over twice its own best on B
13 times in `aw` r0 where A has 1.

## 4. The host while it ran

`pgrep -a -f "cc1plus|cmake --build|ctest"` was **empty at every invocation
start and end** (`driver-all.log`, `competing: 0` in all 8 lines, and the
driver's `host_state` per cell). The one-minute load before a cell was 0.81 to
1.49 (the cells' own previous load; the gate holds a cell until it is under
1.5), 5-minute 0.9-2.9, 15-minute 1.6-3.3 at the start and falling. Resident
and not part of the run: a podman SQL Server and PostgreSQL 17 container, other
`claude` sessions. `loadavg` before/after per cell is in each JSON
(`host_before`, `host_after`); the cell-4 hosts ran at 0.73 to 1.84.

## 5. The wait breakdown (cells 1-3)

Share of the window (`total_us` of the event over the window, derived).
`on_cpu` is the reactor's; `sched.run_queue` is summed over sessions and can
pass 100 %. `io.page_read` is 0.00 % in every cell (no device read). The
`lock.*` waits did not occur and no cell had an error (0 errors in all 32
cells). `holds` are `sys.meta`'s `sched_hold_in_run_*` (a statement's
continuous hold of its core), and `fg polls` the number of foreground polls
in the window: **A's walk is one poll a walk (850 polls for 162 walks); B's is
one per slice (89,940)**.

| cell | arm | run | `on_cpu` | `sched.run_queue` (sum) | `io.page_read` | longest hold | holds over 10 ms | holds over 100 ms | fg polls | WAL appended | WAL syncs |
|---|---|---|---|---|---|---|---|---|---|---|---|
| i | A | r0 | 4.3 % | 0.2 % | 0.00 % | 0.0 ms | 0 | 0 | 20,006 | 0.0 MB | 8 |
| i | B | r0 | 4.7 % | 0.2 % | 0.00 % | 0.0 ms | 0 | 0 | 20,006 | 0.0 MB | 8 |
| ivr | A | r0 | 98.0 % | 4.3 % | 0.00 % | 0.1 ms | 0 | 0 | 921,319 | 0.0 MB | 8 |
| ivr | B | r0 | 97.9 % | 4.3 % | 0.00 % | 0.0 ms | 0 | 0 | 918,562 | 0.0 MB | 8 |
| ivw | A | r0 | 100.5 % | 109.0 % | 0.00 % | 21.2 ms | 162 | 162 | 850 | 169.4 MB | 162 |
| ivw | A | r1 | 100.5 % | 137.0 % | 0.00 % | 0.0 ms | 161 | 161 | 831 | 165.2 MB | 160 |
| ivw | B | r0 | 99.6 % | 176.6 % | 0.00 % | 1,295.0 ms | 24 | 12 | 89,940 | 3,020.7 MB | 283 |
| ivw | B | r1 | 99.5 % | 257.3 % | 0.00 % | 7,543.0 ms | 23 | 11 | 61,330 | 1,908.9 MB | 170 |
| ivu | A | r0 | 102.9 % | 301.1 % | 0.00 % | 63.3 ms | 63 | 63 | 198 | 6,768.1 MB | 62 |
| ivu | A | r1 | 103.0 % | 276.6 % | 0.00 % | 15.7 ms | 64 | 64 | 216 | 6,878.3 MB | 63 |
| ivu | B | r0 | 99.1 % | 81.1 % | 0.00 % | 0.0 ms | 36 | 0 | 122,391 | 10,547.0 MB | 842 |
| ivu | B | r1 | 99.4 % | 177.1 % | 0.00 % | 1,195.0 ms | 45 | 10 | 68,563 | 6,464.4 MB | 497 |
| aw | A | r0 | 100.5 % | 100.4 % | 0.00 % | 28.9 ms | 163 | 163 | 167 | 184.7 MB | 163 |
| aw | B | r0 | 99.6 % | 29.5 % | 0.00 % | 2,305.8 ms | 28 | 2 | 201,725 | 4,636.6 MB | 542 |
| aw | B | r1 | 100.0 % | 83.4 % | 0.00 % | 1,661.4 ms | 16 | 16 | 2,845 | 1,275.5 MB | 53 |
| au | A | r0 | 103.0 % | 92.2 % | 0.00 % | 75.4 ms | 64 | 64 | 67 | 6,877.8 MB | 63 |
| au | A | r1 | 103.0 % | 94.4 % | 0.00 % | 1,645.3 ms | 47 | 47 | 50 | 5,023.8 MB | 46 |
| au | B | r0 | 100.0 % | 70.8 % | 0.00 % | 4,767.6 ms | 21 | 19 | 2,562 | 1,275.4 MB | 58 |
| au | B | r1 | 99.8 % | 20.4 % | 0.00 % | 846.6 ms | 45 | 2 | 80,964 | 8,873.0 MB | 672 |

(`ivr` r1, `i` r1 and the `ivw`/`ivu`/`aw`/`au` rows not listed are in the
JSONs; the table keeps one row for the cells whose two runs agree.) **The
reactor is on the CPU ~100 % of the window in every cell with a walker, on both
arms**: B did not make the walk cheaper, it made it interruptible, so the core's
time is shared between the walker and the hot sessions (`sched.run_queue` in
`ivw` is 176.6 and 257.3 % on B against 109.0 and 137.0 % on A; it was not
decomposed). A hold over 100 ms is one a hot read waits behind: A has 161-162 of
them a window in `ivw` (one per walk) and B 11-12, **none of them a slice** (a
64-leaf slice is about 60 µs, derived from 122 ms over ~2,000 slices) but
the multi-second polls of section 6.

## 6. Where B's stalls are, as far as this run goes

Cells 1-3 on B show, beside the stalls, a different volume of log and of
page images than A for a statement whose rows are the same. `pool_fetch_images`
is the count of page images a fetch-for-write logged (`wal.md` §10: a frame owes
an image from its fault-in and each writeback copy).

| cell | arm | run | page images fetched | image bytes | WAL appended | commits | WAL sync-gate waits (count / total) | checkpoint pages written | checkpoint runs | `wal_ring_full` |
|---|---|---|---|---|---|---|---|---|---|---|
| ivw | A | r0 | 20,672 | 169 MB | 169 MB | 162 | 1 / 0.03 s | 21,312 | 0 | 106 |
| ivw | A | r1 | 20,160 | 165 MB | 165 MB | 160 | 1 / 0.02 s | 21,056 | 0 | 98 |
| ivw | B | r0 | 368,312 | 3,020 MB | 3,021 MB | 39 | 45 / 12.65 s | 407,034 | 2 | 2,781 |
| ivw | B | r1 | 232,764 | 1,908 MB | 1,909 MB | 20 | 30 / 15.18 s | 232,765 | 1 | 1,753 |
| aw | A | r0 | 22,528 | 185 MB | 185 MB | 163 | 2 / 0.02 s | 22,848 | 0 | 107 |
| aw | B | r0 | 565,364 | 4,635 MB | 4,637 MB | 101 | 70 / 2.18 s | 602,743 | 3 | 4,270 |
| aw | B | r1 | 155,516 | 1,275 MB | 1,275 MB | 2 | 19 / 18.70 s | 149,760 | 0 | 1,170 |
| ivu | A | r0 | 3,456 | 28 MB | 6,768 MB | 62 | 55 / 1.47 s | 4,288 | 0 | 6,395 |
| ivu | B | r0 | 617,727 | 5,067 MB | 10,547 MB | 51 | 155 / 1.73 s | 617,739 | 4 | 9,695 |
| ivu | B | r1 | 429,894 | 3,526 MB | 6,464 MB | 27 | 96 / 9.42 s | 433,548 | 2 | 5,940 |
| au | A | r0 | 3,584 | 29 MB | 6,878 MB | 63 | 54 / 1.32 s | 3,520 | 0 | 6,500 |
| au | B | r0 | 128,931 | 1,058 MB | 1,275 MB | 2 | 19 / 22.14 s | 154,213 | 1 | 1,164 |
| au | B | r1 | 547,359 | 4,490 MB | 8,873 MB | 40 | 131 / 5.32 s | 653,958 | 4 | 8,154 |

**What the table says.** A's no-match walk logs 20,160-22,912 images
(165-188 MB; about a sixth of the relation's 128,960 pages: why not every page
was not examined). On B the same statement logs **155,516 to 565,364 images
(1.3-4.6 GB)**, up to 4.4 images a page over the window (565,364 / 128,960,
derived) and, per walk finished in the window, 72 MB on B `ivw` r0 (3,020.7
MB / 42 walks) and 80 MB on r1 (1,908.9 / 24) against A's 1.0 MB (169.4 /
167). The checkpoint-runs column is the window's `contention_checkpoint_runs`
delta (B 0-4, A 0 in these cells, although A's `contention_checkpoint_pages_written`
is 21,056-21,312: what the A counter means in these windows was not examined).
**The system task is polled 8 to 100 times more often on B**
(`sched_system_polls` 2,466-12,877 on B, 68-371 on A, same cells), which is
what a statement that yields would be expected to do. The stalling windows are the ones with the most
**WAL sync-gate wait**: the largest holds in section 5 (7.5 s, 4.8 s, 2.3 s)
are in cells whose sync-gate wait totals 15.18, 22.14 and 2.18 s. This file
reports the association; **it does not isolate which wait the multi-second
polls were inside** (`sys.wait_stats` has no event of that length on the
reactor in these windows; `sysio.wal_write` shows 1.0 s total in `ivw` B r0),
nor whether the checkpoint's writeback or the log's recycling gate is the
bound. Section 8's probe is the one experiment that bears on it.

## 7. DT-R4: a write that takes no pause pays nothing

`tools/dr_close.py --part ab --rounds 15`, unmodified (the driver DR's close
used, `bench/v3.5.0/results-dr-close-v3.5.0-124-ge0d86ec62.md`): `cores = 1`,
`relaxed`, one fresh server per cell on CPU 2, 100,000 rows in a btree
(`id int64`) and a heap (`id int64 PRIMARY KEY`), pool 131,072 frames (every
page resident), 15 rounds, arms A, **A2 (a second copy of A: another inode, the
same engine, the control)** and B in six rotating orders (45 cells, 160.8 s,
quiet-gate hold 0.0 s). Hashes (first 16 hex): A and A2 `d9e683d9b312c7e2`,
B `145700aedddebda5`. **Server CPU a statement** is the median over a phase's 20
batches (`/proc/<pid>/stat` delta over the batch), and each row's number
below is the median over the 15 rounds with the range over the rounds beside
it. Statement counts: reads 20,000, `UPDATE`/`INSERT`/`DELETE` 4,000 each, range
(100 rows) 1,000, walked 40. Resident point `UPDATE` by tuple key is
`UPDATE t SET w = w + 1 WHERE id = k`; `INSERT` is single-row; the `SELECT` by key
is the control.

| relation | statement | A (ns/stmt, median; range of 15) | A2, control (ns/stmt) | B (ns/stmt) | B - A | A2 - A, control (median; range over rounds) | B - A2 | per-round B - A | rounds B > A |
|---|---|---|---|---|---|---|---|---|---|
| btree | point `SELECT` by key | 6,240 ns (6,090-6,300) | 6,270 ns (6,170-6,380) | 6,270 ns (6,060-6,370) | +30 ns (+0.5 %) | +30 ns (-50 .. +270) | 0 ns | -210 .. +220 ns | 9 of 15 |
| btree | point `UPDATE` by key | 6,810 ns (6,720-6,890) | 7,010 ns (6,860-7,090) | 7,010 ns (6,810-7,180) | +200 ns (+2.9 %) | +200 ns (-10 .. +330) | 0 ns | -70 .. +370 ns | 14 of 15 |
| btree | single-row `INSERT` | 6,380 ns (6,250-6,480) | 6,520 ns (6,390-6,680) | 6,550 ns (6,400-6,630) | +170 ns (+2.7 %) | +140 ns (-60 .. +370) | +30 ns | +10 .. +290 ns | 15 of 15 |
| btree | `DELETE` by key | 6,000 ns (5,860-6,080) | 6,120 ns (5,990-6,290) | 6,160 ns (6,020-6,350) | +160 ns (+2.7 %) | +120 ns (-30 .. +360) | +40 ns | -20 .. +320 ns | 13 of 15 |
| btree | range, 100 rows | 15,640 ns (15,510-15,850) | 15,810 ns | 15,770 ns (15,510-16,190) | +130 ns (+0.8 %) | +170 ns (-90 .. +390) | -40 ns | -150 .. +430 ns | 11 of 15 |
| heap | point `SELECT` by key | 6,740 ns (6,640-6,860) | 6,840 ns (6,760-6,940) | 6,830 ns (6,690-6,880) | +90 ns (+1.3 %) | +100 ns (-40 .. +270) | -10 ns | -110 .. +200 ns | 12 of 15 |
| heap | point `UPDATE` by key | 7,280 ns (7,180-7,400) | 7,430 ns (7,340-7,780) | 7,360 ns (7,290-7,500) | +80 ns (+1.1 %) | +150 ns (-60 .. +540) | -70 ns | -40 .. +250 ns | 14 of 15 |
| heap | single-row `INSERT` | 8,150 ns (8,040-8,220) | 8,330 ns (8,200-8,520) | 8,310 ns (8,250-8,460) | +160 ns (+2.0 %) | +180 ns (+10 .. +410) | -20 ns | +60 .. +280 ns | 15 of 15 |
| heap | `DELETE` by key | 6,380 ns (6,220-6,510) | 6,520 ns (6,380-6,810) | 6,480 ns (6,420-6,630) | +100 ns (+1.6 %) | +140 ns (-90 .. +360) | -40 ns | -30 .. +260 ns | 12 of 15 |
| heap | range, 100 rows | 25,040 ns (24,860-25,460) | 25,260 ns | 24,990 ns (24,720-26,180) | -50 ns (-0.2 %) | +220 ns (-200 .. +620) | -270 ns | -470 .. +1,170 ns | 3 of 15 |

(Units: ns of server CPU a statement; the per-statement figure is derived from
`cpu_us` x 1,000 in `dr4-ab.json`.)

**Does DT-R4 hold within the spread: yes, by the numbers.** The `SELECT`
control moves +30 ns (btree) and +90 ns (heap); the write statements move
**+80 to +200 ns (+1.1 to +2.9 %) against A, but the control, a second copy of
A, moves +100 to +200 ns against A in the same rows** (A2 - A), and **B - A2 is
-70 to +40 ns (-0.9 to +0.7 %)** for the eight point statements and -40 and
-270 ns (-0.3 and -1.1 %) for the two ranges. In the cells where B's
sign is positive against A in 14-15 of 15 rounds (`UPDATE`, `INSERT`), A2's is
positive too; the difference between B and A is the difference between
two copies of A. Every point statement's |B - A2| (0 to 70 ns) is under the smallest
round-to-round range of any arm in those rows (170 ns). The resident write statements that take no
pause cost the same on B, within about 40-70 ns of the control.

**A statement that does pause, from the same driver (not DT-R4).** The
driver's `walked` phase is `UPDATE t SET w = w + 1 WHERE v = <k>` with no key,
which walks the relation (100,000 rows, 40 statements, each writing 100 rows).
The relation's leaves number past 64, so B pauses, and the median per
statement is:

| relation | A (ns/stmt) | A2 (ns/stmt) | B (ns/stmt) | B - A | per-round B - A | rounds B > A |
|---|---|---|---|---|---|---|
| btree | 3,295,530 ns (3,288,380-3,319,440) | 3,297,900 ns | 3,726,070 ns (3,707,830-4,405,090) | +430,540 ns (+13.1 %) | +396,940 .. +1,116,190 ns | 15 of 15 |
| heap | 3,277,940 ns (3,266,310-3,290,940) | 3,278,780 ns | 3,299,020 ns (3,293,840-3,627,190) | +21,080 ns (+0.6 %) | +8,080 .. +339,620 ns | 15 of 15 |

The control moves +2,370 and +840 ns, so these are not noise. The btree
statement costs **+430.5 µs of server CPU** more on B in 15 of 15 rounds and the
heap +21 µs. How many pauses each statement takes was not measured here, so no
per-pause figure is derived; the 5 µs of section 3 (a relation of
1,000-byte rows, ~2,085 pauses a walk) and the 430.5 µs over this btree's
leaves (rows of 24 bytes; about 500 leaves by a back-of-envelope count, so
about 8 pauses, or about 50 µs each, an unmeasured guess) are not in
agreement and **this file does not reconcile them**.

## 8. The two probes (one run per arm, not part of the A/B)

**Rows written per walk (scaling).** The writing walker alone, `c0 >= 1 AND c0
<= N`, 22 s window, one run per arm. B's p0 is the walk it can finish between
the checkpoint's runs.

| rows written (N) | arm | walks finished | p0 | p50 | p95 | max | page images | WAL appended | checkpoint runs | sync-gate wait |
|---|---|---|---|---|---|---|---|---|---|---|
| 5,000 | A | 162 | 126.0 ms | 129.0 ms | 154.0 ms | 818.0 ms | 11,456 | 1,698 MB | 0 | 0.25 s |
| 5,000 | B | 101 | 139.0 ms | 143.0 ms | 935.0 ms | 7,254.0 ms | 759,365 | 7,261 MB | 5 | 2.10 s |
| 15,000 | A | 96 | 145.0 ms | 149.0 ms | 740.0 ms | 899.0 ms | 1,728 | 2,779 MB | 0 | 7.29 s |
| 15,000 | B | 6 | 159.0 ms | 417.0 ms | 17,907.0 ms | 17,907.0 ms | 133,915 | 1,135 MB | 2 | 15.78 s |
| ~50,000 (section 3) | A | 80 / 63 | 260.0 / 262.0 ms | 292.0 / 298.0 ms | 327.0 / 1,022.0 ms | 1,016.0 / 1,958.0 ms | 3,584 / 2,752 | 6,878 / 5,024 MB | 0 | 1.32 / 2.54 s |
| ~50,000 (section 3) | B | 3 / 49 | 4,648.0 / 272.0 ms | 12,657.0 / 307.0 ms | 17,005.0 / 1,538.0 ms | 17,005.0 / 8,548.0 ms | 128,931 / 547,359 | 1,275 / 8,873 MB | 1 / 4 | 22.14 / 5.32 s |

The walk's best case on B stays within +10 to +14 ms of A's at 5,000 and
15,000 rows (+11 % and +10 %) and **the stalls are in B at every size
tried, 5,000 rows included** (p95 935 ms, max 7,254 ms), so the stall is not
only a long-write-set effect. One run each: the spread is not known.

**The cadence checkpoint off (`checkpoint_interval_ms = 0`)**, cells `ivw`
and `au`, one run per arm, the server log's first line reading `checkpoint
cadence disabled; durability is SYNC and shutdown only`. A probe of the
mechanism, not a configuration the engine recommends, and not an A/B result:
one run, no spread.

| cell | arm | hot n (reads/s) | hot p0 | p25 | p50 | p95 | p99 | p99.9 | max | walks (in window) | walk p0 / p50 / max | page images | WAL appended |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ivw | A | 734 (36.7 /s) | 20.5 µs | 117,526.1 µs | 118,883.1 µs | 124,286.9 µs | 133,519.9 µs | 133,893.5 µs | 133,893.5 µs | 169 | 117.0 / 119.0 / 862.0 ms | 0 | 0 MB |
| ivw | B | 20,004 (1,000.2 /s) | 58.4 µs | 164.0 µs | 189.1 µs | 288.2 µs | 332.4 µs | 1,031.4 µs | 2,900.8 µs | 156 | 129.0 / 130.0 / 762.0 ms | 0 | 0 MB |

| cell | arm | walks finished | walk p0 | p50 | p95 | max | WAL appended | sync-gate wait | note |
|---|---|---|---|---|---|---|---|---|---|
| au | A | 19 | 275.0 ms | 1,326.0 ms | 2,597.0 ms | 2,597.0 ms | 1,414 MB | 7.83 s | the log is never recycled, no cadence |
| au | B | 27 | 272.0 ms | 938.0 ms | 1,553.0 ms | 1,624.0 ms | 1,712 MB | 15.18 s | same; its first nine walks are 272-300 ms |

**With the cadence off, B's no-match walk beside four hot sessions is what DT
meant it to be**: service p99 **332.4 µs** (max 2,900.8 µs) at 1,000.2 reads/s,
where A is at 133,519.9 µs and 36.7 reads/s, **and the walker's p50 is 130.0
ms against A's 119.0 ms** (+11 ms, +9 %). The slice is about 60 µs
(130 ms / 2,085 polls), derived. Its page images are **0 and its log 0 MB**,
like A's. The `au` rows are **not a comparison of B's pauses**: with no
cadence the log does not recycle, and *both* arms' writing walks slow to
0.9-1.3 s (A p50 1,326 ms), which is a property of this configuration, not
of DT. What they show is that B's first nine walks (272-300 ms) match A's p0
(275 ms) until the log fills. Together with section 6, the probe says the
stalls of the default configuration need the cadence checkpoint, **but it does
not say the checkpoint's work is the cause** (the checkpoint's own cost, its
interaction with the log gate, and the image re-fetch are not separated).

## 9. Against this engine's own earlier number (DT-S0, same host)

DT-S0 (`bench/v3.5.0/results-dt-s0-premise-v3.4.17-150-g60dc593af.md`)
measured A's shapes on this host the same day; the arm-A rows of this file are
the same engine, so they are compared first (does this run reproduce it), then B.

| shape | DT-S0 (A), two runs | this run, arm A, two runs | this run, arm B, two runs |
|---|---|---|---|
| `ivw` hot service p99 | 241,518.5 and 245,024.7 µs | 245,226.4 and 265,067.1 µs | 10,260.8 and 1,621.7 µs |
| `ivw` hot service max | 242,147.0 and 247,432.0 µs | 254,368.5 and 267,141.8 µs | 1,389,095.8 and 7,655,877.1 µs |
| `ivw` hot reads completed | 34.9 and 34.5 /s | 34.6 and 33.8 /s | 555.6 and 1,000.2 /s |
| `ivw` walker, per walk p50 | 122 and 123 ms | 121.0 and 121.0 ms | 131.0 and 133.0 ms |
| `ivr` hot service p99 | 288.0 and 351.7 µs | 286.6 and 274.7 µs | 334.3 and 288.7 µs |
| `ivr` walker, per walk p50 | 45 and 45 ms | 44.0 and 44.0 ms | 44.0 and 44.0 ms |
| `i` hot service p99 | 530.5 and 553.1 µs | 507.6 and 382.7 µs | 548.1 and 509.5 µs |

A reproduces DT-S0: `ivw` p99 245.2 and 265.1 ms against 241.5 and 245.0 ms
(+1.5 % and +8.2 %), reads/s 34.6 and 33.8 against 34.9 and 34.5, so the host
and the method read the same on the same engine. B against DT-S0's A is
therefore a fair delta on the same host: `ivw` p99 -96 % and -99.3 %, hot reads
completed 16x and 29x, **and the max 5.6x and 31x higher**. DT-S0 had no
shape for the walk that writes rows, nor an alone cell, nor a cell-4 number;
this file is the baseline for those (the previous number for DT-R4's statements
on the same engine is `bench/v3.5.0/results-dr-close-v3.5.0-124-ge0d86ec62.md`'s,
on another host and another set of rows, and is not compared).

## 10. What the run says about the engine

1. **The yield does what the design said, and its price is small.** With the
   cadence checkpoint out of the way, the write walk's slice is a poll of
   about 60 µs, the pause costs about 5 µs (11 ms over ~2,085 slices), the hot
   sessions' p99 beside a whole-relation write walk falls from 133.5 ms to 332.4
   µs (probe, one run), and the walk's own time is +9 %. That is under DT-Q1's
   1 ms at p99 (`workorder-dt-write-path-yield.md`); the reading is the
   operator's.
2. **A yield makes the write walk interruptible by the system task, the
   cadence checkpoint included.** This is a candidate mechanism, not a
   finding: on A a walk was one poll and the system task was polled 68-371
   times a window; on B it is polled 2,466-12,877 times, between any two
   slices. The data in
   the file agree with it (B's images 11-18x A's, 0 with the cadence off;
   B's stalls only with the cadence on) and **do not prove it**: the probe is
   one run, the multi-second polls were not located inside any wait event, and
   the checkpoint's writeback and the log's sync gate were not separated. If it
   holds, it is the DH design (`wal.md` §10: every copy owes the next write an
   image) meeting the first statement that writes a page it does not change
   and now has time to be copied under it. The first number DT-Q1 should be
   read beside is the default-cadence one: p99 1.6-10.3 ms on `ivw`, 50 ms on
   `ivu`, max 1.2-7.7 s.
3. **A walked write that writes no row logs a page image for every page it
   fetches for write, and on B the same page can be imaged again and again**
   (232,764-565,364 images in a 20 s window against 20,160-22,912), which
   `ivw`'s `UPDATED 0` reply does not suggest. DT-S0's note that such a walk
   "is not free when it writes nothing" (1.05 MB of log a walk on A) becomes
   72 and 80 MB a walk on B `ivw` with the default cadence (derived from
   3,020.7 and 1,908.9 MB over the 42 and 24 walks finished in the window;
   the walks differ in length, so it is an average).
4. **Read walks are untouched** (`ivr`, section 3, and p0-p99 of the scanner's
   walks equal on both arms), as is every point statement (section 7).

## 11. What was not measured

`cores > 1`; `strict` durability (every cell is `relaxed`); a `SET PRIORITY
analytic` session (the 16-leaf slice); a write driven by a list on the tuple key
(DT's third path); a cancel's latency at the pause (DT-S3); a pool smaller than
the relation (DT-S0's `ivwc`, which died of `buffer pool full` once); a heap
relation; `INSERT`/`DELETE`/`PURGE` as walkers; a write set above 65,536 rows
(refused by the engine on A, tried only there); the row-set sweep of rule 9
(section 2); the cause of the multi-second polls (section 6); a second run of
either probe, so no spread for them; cell-by-cell alternation of cells 1-3
(section 2); the cadence at any other value than 5,000 ms (default) and 0.
Per-pause cost was derived (section 3) from the no-match walk only. **The
milestone's close is not ruled on here.**
