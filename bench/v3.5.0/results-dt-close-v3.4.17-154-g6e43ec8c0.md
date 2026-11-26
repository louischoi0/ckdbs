# DT close, after the remedy: a write walk looks before it writes

Measured on `v3.4.17-154-g6e43ec8c0` (Release), the second milestone-close A/B
of `instructions/v3.2.0/workorder-dt-write-path-yield.md`, three arms: **A**
`60dc593af` (the commit DT opened from), **B** `d172085e8` (the yield without
the remedy) and **C** `6e43ec8c0` (HEAD: a write walk looks at each leaf under
a shared hold first and takes for writing only a leaf holding a row it would
write). `cores = 1`, `relaxed`, the default checkpoint cadence. The previous
number for A and B is `bench/v3.5.0/results-dt-close-v3.4.17-153-gd172085e8.md`
(`v3.4.17-153-gd172085e8`, the record of the regression); C has no earlier
number and this file is its baseline.

**Finding, in one paragraph.** *With the remedy the stall is gone in every cell
measured, and the write walk's own tail is shorter than A's.* Beside a walked
`UPDATE` that matches nothing (`ivw`), the four hot sessions see a service **p99 of
320.7 and 365.0 µs, a max of 1,533.7 and 2,849.8 µs, and 1,000.2 reads/s** on C
(two runs), against **253.5 and 263.4 ms, 268 and 265 ms, 34.5 and 26.3
reads/s** on A and **9.0 and 9.7 ms, 3.44 and 4.12 s, 488.6 and 838.2 reads/s**
on B. C's no-match walk logs **0 page images and 0 MB of WAL** in the window (A:
15,872-20,800 images, 130-171 MB; B: 253,756-602,676 images, 2.1-4.9 GB), has
**0 WAL sync-gate waits** (B: 14.8 and 5.9 s over 31 and 74 waits) and **0 holds of
100 ms or more** (A: 161 each, one a walk; B: 8 and 2), and no walk of C's in any
of its nine walker cells is over 1 s (A: 25 walks over 1 s in the eight cells with a write
walk, B: 28 in the same). Beside the walk that writes ~50,000 rows (`ivu`) the hot
p99 is **51.1 and 50.9 ms** on C (A **0.66 and 3.39 s**, B **344 and 687 ms**) and
the max **54.8 and 61.2 ms** (A 0.66 and 3.39 s, B 1.38 and 1.61 s): a ~50 ms
plateau remains on C that was not located. C's writing walk alone finishes **83
and 82 walks** in the window (A 30 and 43, B 4 and 21), p50 **284 and 287 ms** (A 448
and 305, B 14,419 and 295 ms), max **369 and 381 ms** (A 2,039 and 1,586 ms). The writing
walk logs **57,094-64,001 page images** on C where A logs 1,152-3,584, because
the cadence checkpoint now runs between slices and each page it copies owes one
image (images equal the checkpoint's pages written, 57,094 against 57,118);
C's WAL per writing walk is 9 to 21 % above A's (derived, section 4). **DT-R4
holds on C**: the eight point statements are within -0.2 to -3.3 % of A (the
control, a second copy of A, is within -0.3 to +0.8 % of A), and the keyless
walked `UPDATE` of 100 rows over 100,000 costs **+212.5 µs (+6.4 %) on a btree**
against A in 15 of 15 rounds (B: +430.5 µs, +13.1 %) and **-77.3 µs (-2.4 %) on a
heap** (B: +21.1 µs). Not measured: the walk whose every row is written
(section 9), `cores > 1`, `strict`. This file does not rule on whether the
milestone closes.

## 1. Stamp

| | |
|---|---|
| Executed | 2026-11-26, 19:25-20:36 UTC (wall 71 min, of which 13.7 min of hold for another session and the run time of the discarded cells, section 2). First pass 19:28-19:50; reruns 20:11-20:18; cell 5 (`dr_close.py`) kept run 20:32:25-20:35:34 (188.8 s). Written 20:45 UTC |
| Branch / worktree | `worktree-dt-write-path-yield`, worktree `dt-write-path-yield` |
| Commit (arm C) | `6e43ec8c0d54a33f01142b89ff794a4240c7b9c4`, **`git describe --tags` = `v3.4.17-154-g6e43ec8c0`**, committed 2026-11-26 14:24:33 -0500. The version of record is `v3.5.0`; the describe string counts from `v3.4.17` because the tag is not in this clone |
| Commit (arm B) | `d172085e8`, `v3.4.17-153-gd172085e8` |
| Commit (arm A) | `60dc593af`, `v3.4.17-150-g60dc593af`, the commit DT opened from |
| Tree | tracked sources: no build input modified (uncommitted at the time: `CLAUDE.md`, `docs/`, `manual/`, all not build inputs, and untracked `tools/dt_close.py`, `bench/` files) |
| Binary C | `cmake --build build-release -j 12 --target kds_server` (24.6 s), source `build-release/kds_server` mtime 2026-11-26 14:25:40 -0500, **after** the commit (14:24:33). Measured from the copy `~/bench-runs/dt/close/bin/kds_server.C`, **sha256 `4b8041af1ef68679ce9dce31d64390059da9626f36520db117214193aa373b02`** (equal to the source's; every C JSON's `bin_sha256` agrees) |
| Binary B | copy from the first close, `~/bench-runs/dt/close/bin/kds_server.B`, **sha256 `145700aedddebda5169600199782c835c9894332de0a1062b0d0566dd1d31d31`**, verified before use, every B JSON agrees |
| Binary A | copy from the first close, `~/bench-runs/dt/close/bin/kds_server.A`, **sha256 `d9e683d9b312c7e2f3aa4ddf26d81a1b215afc10ce840ae7abe8ac65c22da9cc`**, verified before use; `kds_server.A.control` (a second copy, another inode, the same hash) is cell 5's A2 |
| Host | 24 logical CPUs (Intel Core Ultra 7 270K Plus), 30 GiB, Linux 7.3.4-300.fc45, the same host as DT-S0 and the first close. Reactor pinned to CPU 1, clients round-robin on CPUs 2-7 (cells 1-4); cell 5: server on CPU 2, driver on CPUs 0 and 4 |
| Data device | `/dev/mapper/fedora-root`, **xfs** (`df -T ~/bench-runs/dt`), on the NVMe. Not tmpfs. Data files `~/bench-runs/dt/close/work/cell/dt.db` (cells 1-4, 1,056,440,320 B = 128,960 pages, a fresh copy of the template per cell; the template is the first close's, loaded by A) and `~/bench-runs/dt/close/w4r` (cell 5) |
| Build type | Release (`build-release`, `CMAKE_BUILD_TYPE=Release`), all three arms |
| Server configuration | `durability = relaxed`; `cores = 1`; `buffer_pool_frames` 160,000 (cells 1-4) or 131,072 (cell 5); `log_level = warn`; `checkpoint_interval_ms` default (5,000) in every cell; no other non-default key |
| Ports | 32771 (cells 1-4), 32781 (cell 5); each checked free before use |
| Drivers | `tools/dt_close.py` (the first close's, unchanged) for cells 1-4; `tools/dr_close.py --part ab --rounds 15`, unmodified, for cell 5, its `--bin-b` pointing at C (**the driver's arm label `B` in `dr4-ab.json` is C's binary**). `bench/docs/` stays closed (`bench/README.md`); each driver's docstring documents it |
| Raw output | `bench/v3.5.0/archive/dt-close-v3.4.17-154-g6e43ec8c0/`: `dt-<cell>-<arm>-r<run>.json` and `.s.log`, `summary-*.json`, `dr4-ab.json`, `dr4.beat`, `driver-*.log`, `driver-all.log` (the gate, retry and hold lines), `sampler.log`, `scripts/` (`run.sh`, `run2.sh`, `run3.sh`, `quiet.sh`, `watch.sh`), and `contaminated/` (discarded cells, section 2, kept as evidence and not reported) |

## 2. What was measured, what was cut, and the host

Same relation and settings as the first close: `dt_big (id int64, c0 int64, pad
char(1000))`, 900,000 rows, BTREE, 128,960 pages; window 20 s after a 4 s
warm-up; the pool holds the relation and the page cache is warm (`mincore`
1.00), so `io.page_read` is 0.00 % in every cell. The hot sessions are four,
1,000 point reads/s in all, keys 1..700. The writing walker's statement is
`UPDATE dt_big SET c0 = c0 + 1 WHERE c0 >= 1 AND c0 <= 50000` (49,918-49,998
rows a walk, 5.5 % of the rows and about 7,150 of the pages; one walked write
cannot write more than 65,536 rows).

| cell | hot | walker | runs: A / B / C | what it is |
|---|---|---|---|---|
| `ivw` | 4 sessions, 1,000 reads/s | `UPDATE ... SET c0 = c0 WHERE c0 = -1` (matches nothing) | 2 / 2 / 2 | the first close's cell 1 |
| `ivu` | same | the writing walk above | 2 / 2 / 2 | cell 2 |
| `ivr` | same | `SELECT id FROM dt_big WHERE c0 = -1` | 1 / - / 1 | cell 3, control |
| `i` | same | none | 1 / - / 1 | cell 3, control |
| `aw` | none | the no-match walker alone | 2 / 2 / 2 | cell 4 (B twice, not once) |
| `au` | none | the writing walker alone | 2 / 2 / 2 | cell 4 (B twice) |
| cell 5 | none | `dr_close.py --part ab --rounds 15`: A, A2, C | 15 rounds x 3 arms | DT-R4 and the keyless walked `UPDATE` |
| cell 6 | | a walk whose written rows are spread over every leaf | **not run** | section 9 |

**Interleave unit.** One arm's set of cells (a fresh server per cell) in the
order A r0, B r0, C r0, C r1, B r1, A r1, as the first close did, **but six
cells were then run out of that order**: B r0's `ivw`, `ivu`, `aw`, `au`, C r0's `ivw` and
A r1's `ivw` were discarded (below) and rerun one cell at a time at 20:11-20:18,
**after** the sets they belong to. B r1's `ivw` and `ivu` were also discarded and B
r1 rerun whole (19:54-19:59). Cell 5 rotates A, A2, C in six orders over 15
rounds (`dr_close.py`'s own).

**The host was not quiet, and this file's numbers are the cells that were.**
Another worktree (`du-keyed-write`) ran `cmake --build -j24`, `ctest -j12` and
its own `kds_tests` binary on the same 24 CPUs, in bursts, while this run
was in progress. The first pass's `competing:` count in `driver-all.log` and the
driver's `host_state` used `pgrep -f 'cc1plus|cmake --build|ctest'`, which also
matches the polling shell's own command line, so a count of 1 to 2 was a
self-match and was first read as one; a count of 5 to 10 and a competitor named in
a cell's `host_before` or `host_after` was not. The result:

| what | when (UTC) | consequence |
|---|---|---|
| a competing `ctest` or `cmake --build` named in the `host_before`/`host_after` of B r0's `ivu`, `aw`, `au`, C r0's `ivw` and A r1's `ivw` | B r0 and C r0's first cell, 19:31-19:41; A r1's first cell, 19:59:21 | those five cells (and B r0's `ivw`, which had no named competitor but whose set they were) **discarded and rerun** under a gate, `contaminated/` |
| a build `-j24` of `kds_server` and `kds_tests` in the other worktree, seen after C r1's `au` (ended 19:48:06) and during B r1 | from 19:48 | the chain was killed at 19:52:04; B r1's `ivw` and `ivu` discarded and B r1 rerun after a **hold** of 3.8 min (19:50:30 to 19:54:17) until `quiet.sh` saw no `cc1plus`, `ctest`, `cmake` or `make` process (exact names) for 90 s |
| the same, again, before cell 5 | 20:03:31-20:10:43 | `quiet.sh`'s 90 s window never opened for 7.2 min: **hold 7.2 min**, then replaced by `run3.sh` |
| `run3.sh`: a 1 s sampler (`sampler.log`) writes the last second a competitor existed; a cell runs after 45 s without one and is discarded and retried if one appeared during it | 20:11 onward | the six reruns were each clean on the first try; cell 5's try 1 (a competitor from 20:20:22, seven `cc1plus`, killed at 20:22:36) and try 2 (a competitor at 20:28:22, cells of 8 to 19 s instead of 3.1 s) were discarded, try 3 (20:32:25) was clean; a **gate wait of 162 s** (2.7 min) preceded try 2 |

The cells that **are** in the tables: A r0 (all six), C r0 (`ivr`, `i`, `ivu`, `aw`,
`au`), C r1 (`ivw`, `ivu`, `aw`, `au`), A r1 (`ivu`, `aw`, `au`), B r1 (all four,
rerun) have **no competitor named at either end** (`host_before`/`host_after`,
exact), but the first-pass ones were **not sampled between** their ends (the
sampler started at 20:11), so a short competitor inside a cell would not show;
**C r1's `au`** names a `cmake --build` and seven `cc1plus` in its `host_after`
(the build of 19:48 starting as the cell ended), and its result has no sign of
it (82 walks, none over 1 s). The six reruns and cell 5's try 3 were sampled
(`sampler.log` empty during each). The one-minute load average at a cell's
start was 0.5 to 1.8 (the load gate holds a cell under 1.5, up to 120 s, and
then goes on), at its end 0.7 to 3.0; resident and not part of the run: a podman
SQL Server and PostgreSQL 17 container and other `claude` sessions.
**Total hold for another session: 3.8 + 7.2 + 2.7 = 13.7 min**, outside the
budget and not counted in it; the time spent running and discarding cells is
inside it (the wall time, 71 min, less the hold, is 57 min, inside
the hour the work order names).

**Row-set sweep (rule 9) was not done**, for the reason the first close gave:
the subject is a walk's length, the relation is fixed at 900,000 rows, and a walk
of 200, 1,000 or 10,000 rows is shorter than one 64-leaf slice and takes no
pause. 200/1K/10K were not run.

## 3. The effect: hot sessions beside a walker

Hot sessions' **service time** (from the send), microseconds, 20 s window, 4
sessions. Reads/s is derived (`n / 20 s`).

| cell | arm | run | n | reads/s | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ivw | A | r0 | 689 | 34.5 /s | 65.3 µs | 119,847.3 µs | 121,343.4 µs | 143,590.9 µs | 253,485.7 µs | 268,229.7 µs | 268,229.7 µs |
| ivw | A | r1 | 526 | 26.3 /s | 90.8 µs | 120,648.6 µs | 123,254.5 µs | 252,045.2 µs | 263,379.8 µs | 265,051.1 µs | 265,051.1 µs |
| ivw | B | r0 | 9,772 | 488.6 /s | 60.8 µs | 188.8 µs | 470.6 µs | 979.4 µs | 9,005.5 µs | 1,010,983.2 µs | 3,436,931.1 µs |
| ivw | B | r1 | 16,764 | 838.2 /s | 60.1 µs | 180.0 µs | 244.4 µs | 982.3 µs | 9,745.8 µs | 79,419.4 µs | 4,124,915.1 µs |
| ivw | C | r0 | 20,004 | 1,000.2 /s | 59.4 µs | 157.4 µs | 177.9 µs | 274.4 µs | 320.7 µs | 450.6 µs | 1,533.7 µs |
| ivw | C | r1 | 20,005 | 1,000.2 /s | 58.9 µs | 160.5 µs | 181.2 µs | 291.6 µs | 365.0 µs | 992.8 µs | 2,849.8 µs |
| ivu | A | r0 | 146 | 7.3 /s | 73.9 µs | 567,673.4 µs | 587,992.9 µs | 635,427.7 µs | 661,781.5 µs | 661,810.2 µs | 661,810.2 µs |
| ivu | A | r1 | 125 | 6.2 /s | 82.5 µs | 351,693.2 µs | 615,400.3 µs | 2,424,295.4 µs | 3,390,868.2 µs | 3,390,912.3 µs | 3,390,912.3 µs |
| ivu | B | r0 | 7,862 | 393.1 /s | 85.1 µs | 253.4 µs | 878.3 µs | 2,251.1 µs | 343,554.5 µs | 1,300,766.5 µs | 1,383,781.0 µs |
| ivu | B | r1 | 5,729 | 286.4 /s | 122.5 µs | 860.9 µs | 925.3 µs | 2,993.8 µs | 687,013.9 µs | 1,146,762.4 µs | 1,607,953.8 µs |
| ivu | C | r0 | 20,005 | 1,000.2 /s | 115.6 µs | 244.4 µs | 1,240.0 µs | 2,898.0 µs | 51,063.3 µs | 53,865.2 µs | 54,760.5 µs |
| ivu | C | r1 | 19,991 | 999.5 /s | 113.0 µs | 253.4 µs | 1,245.7 µs | 2,815.7 µs | 50,851.3 µs | 53,613.9 µs | 61,158.9 µs |
| ivr | A | r0 | 20,005 | 1,000.2 /s | 58.2 µs | 123.0 µs | 152.8 µs | 293.0 µs | 352.6 µs | 1,132.7 µs | 2,198.9 µs |
| ivr | C | r0 | 20,005 | 1,000.2 /s | 50.3 µs | 119.4 µs | 134.0 µs | 285.1 µs | 338.1 µs | 644.0 µs | 2,852.4 µs |
| i | A | r0 | 20,005 | 1,000.2 /s | 47.0 µs | 229.1 µs | 350.0 µs | 462.5 µs | 536.2 µs | 1,299.4 µs | 3,330.2 µs |
| i | C | r0 | 20,004 | 1,000.2 /s | 29.1 µs | 223.9 µs | 334.0 µs | 458.2 µs | 524.8 µs | 2,155.5 µs | 3,217.0 µs |

**Reading it.** (1) `ivw`: C answers all 20,004-20,005 reads in both runs with a
p99 of 320.7 and 365.0 µs; A's hot sessions complete 526-689 reads, the whole
distribution (p25 119.8-120.6 ms) sitting behind one 118 ms walk; B's p99 is 9.0
and 9.7 ms but a **max of 3.44 and 4.12 s** and 488.6 and 838.2 reads/s. C's
`ivw` p99 is **under** `i`'s (no walker: 524.8 and 536.2 µs), 320.7 and 365.0 µs
against 524.8-536.2 µs. That is not a speed-up from the walker: in `i` the reactor is on the CPU 4.5 % of the window and blocks between
reads, in `ivw` 99 % and never blocks, a likely cause that was not isolated. The p99 of `ivw` on
C and that of the read-walk control `ivr` (338.1 and 352.6 µs) are within what two runs differ by. (2) `ivu`: the hot sessions
beside the writing walk are 1,000.2 and 999.5 reads/s on C, a p50 of 1,240.0 and
1,245.7 µs, a p95 of 2.9 ms and **p99 50.9 and 51.1 ms, p99.9 53.6 and 53.9
ms, max 54.8 and 61.2 ms**. That is a ~50 ms plateau, a quantum, not a smear: 120
foreground polls of 10 ms or more in `ivu` r0's window, none of 100 ms, and **it is the same value the first close
read on B** (p99 50.3 and 50.2 ms; this run's B reads a different tail, 344 and
687 ms, on top of its stalls). It is 13 to 67 times lower than A's p99 (0.66 and
3.39 s) but is not below a millisecond, and **what the poll waits on was not
located** (section 5). (3) `ivr` and `i` are the same engine on A and C: p99
338.1 against 352.6 µs and 524.8 against 536.2 µs, deltas of -14.5 and -11.4 µs,
inside the 63.7 µs two runs of one engine differed by in `ivr` in DT-S0 (288.0 and 351.7 µs) and the 124.9 µs the first close's A runs differed by in `i`.
Those cells had one run each, so this run holds no spread of its own for them.

**The walker's own time per walk** (its own clock; all walks the walker
finished, so a walk begun in the warm-up is counted and the one running at the
window's end is not; with 4 to 197 walks a cell the p99 is within a walk or two
of the max: read it as the max). "In window" is in parentheses.

| cell | arm | run | walks finished (in window) | walks > 1 s | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|---|---|
| ivw | A | r0 | 189 (166) | 0 | 118 ms | 120 ms | 122 ms | 135 ms | 145 ms | 987 ms |
| ivw | A | r1 | 190 (166) | 0 | 118 ms | 120 ms | 121 ms | 131 ms | 161 ms | 826 ms |
| ivw | B | r0 | 50 (33) | 2 | 130 ms | 131 ms | 133 ms | 909 ms | 17,354 ms | 17,354 ms |
| ivw | B | r1 | 101 (84) | 6 | 129 ms | 131 ms | 133 ms | 1,017 ms | 1,380 ms | 19,037 ms |
| ivw | C | r0 | 194 (165) | 0 | 119 ms | 121 ms | 123 ms | 134 ms | 141 ms | 147 ms |
| ivw | C | r1 | 192 (163) | 0 | 121 ms | 122 ms | 124 ms | 134 ms | 146 ms | 152 ms |
| ivu | A | r0 | 79 (65) | 1 | 264 ms | 277 ms | 294 ms | 331 ms | 1,039 ms | 1,039 ms |
| ivu | A | r1 | 60 (46) | 4 | 266 ms | 287 ms | 317 ms | 1,452 ms | 1,907 ms | 1,907 ms |
| ivu | B | r0 | 10 (3) | 5 | 281 ms | 747 ms | 1,095 ms | 15,440 ms | 15,440 ms | 15,440 ms |
| ivu | B | r1 | 16 (7) | 3 | 283 ms | 288 ms | 306 ms | 18,219 ms | 18,219 ms | 18,219 ms |
| ivu | C | r0 | 83 (70) | 0 | 265 ms | 274 ms | 283 ms | 342 ms | 383 ms | 383 ms |
| ivu | C | r1 | 82 (70) | 0 | 267 ms | 276 ms | 286 ms | 353 ms | 365 ms | 365 ms |
| ivr | A | r0 | 531 (453) | 0 | 45 ms | 45 ms | 45 ms | 46 ms | 48 ms | 68 ms |
| ivr | C | r0 | 532 (454) | 0 | 44 ms | 45 ms | 45 ms | 46 ms | 47 ms | 49 ms |

| cell | arm | run | walks finished (in window) | walks > 1 s | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|---|---|
| aw | A | r0 | 190 (165) | 0 | 119 ms | 121 ms | 122 ms | 132 ms | 145 ms | 858 ms |
| aw | A | r1 | 191 (165) | 0 | 119 ms | 120 ms | 122 ms | 130 ms | 163 ms | 822 ms |
| aw | B | r0 | 17 (9) | 4 | 128 ms | 130 ms | 136 ms | 16,885 ms | 16,885 ms | 16,885 ms |
| aw | B | r1 | 37 (22) | 3 | 129 ms | 130 ms | 132 ms | 6,188 ms | 19,372 ms | 19,372 ms |
| aw | C | r0 | 196 (167) | 0 | 120 ms | 121 ms | 122 ms | 130 ms | 144 ms | 150 ms |
| aw | C | r1 | 197 (167) | 0 | 119 ms | 120 ms | 121 ms | 127 ms | 149 ms | 162 ms |
| au | A | r0 | 30 (20) | 11 | 262 ms | 292 ms | 448 ms | 1,802 ms | 2,039 ms | 2,039 ms |
| au | A | r1 | 43 (29) | 9 | 265 ms | 284 ms | 305 ms | 1,553 ms | 1,586 ms | 1,586 ms |
| au | B | r0 | 4 (0) | 2 | 320 ms | 947 ms | 14,419 ms | 18,931 ms | 18,931 ms | 18,931 ms |
| au | B | r1 | 21 (16) | 3 | 276 ms | 285 ms | 295 ms | 9,304 ms | 23,590 ms | 23,590 ms |
| au | C | r0 | 83 (70) | 0 | 261 ms | 275 ms | 284 ms | 340 ms | 369 ms | 369 ms |
| au | C | r1 | 82 (70) | 0 | 264 ms | 276 ms | 287 ms | 361 ms | 381 ms | 381 ms |

**Reading it.** (1) The no-match walk (`ivw`, `aw`): C's best walk is 119 and
121 ms (`ivw`) and 120 and 119 ms (`aw`) against A's 118 and 118 and 119 and
119 ms, **+0 to +3 ms**; p50 122-124 ms against A's 121-122; the first close
measured B at +10 to +12 ms (+9 to +10 %) and this run's B reads 128-130 ms at
p0 and 132-136 at p50 again. **The extra ~10 ms the first close attributed to
the pause is gone on C** (derived: C p50 minus B p50 is -9.5 ms in `ivw` and
-12.5 ms in `aw`, means of two runs), so it was what each slice did to a leaf it
did not write, not the pause. In `ivw` C's p50 is 2.0 ms above A's (123 and 124
against 122 and 121, +1.6 %) and in `aw` it is -0.5 ms: the 2 ms is the hot
sessions' own 1,000 reads/s on the same core, derived, not a cost of the walk.
(2) **C's tail**: its no-match walks run 119 to 162 ms, none over 1 s, where A's run
118 to 987 ms (one over 800 ms a run), and B's
128 ms to 19.4 s. (3) The writing walk: C's p0 is 265 and 267 ms (`ivu`), 261
and 264 ms (`au`), A's 264 and 266, 262 and 265: **+0 to +1 ms**; B's was
+10 to +19 ms in the first close and is 281 and 283 ms (`ivu`) and 320 and
276 ms (`au`) here. C's p50 is 283-287 ms and its max 365-383 ms, where A's p50
is 294-448 ms and its max 1.04-2.04 s with **9 to 11 walks over a second in A's
`au` runs and 1 and 4 in its `ivu` runs**, and **C has none**. B has 2 to 6. The first close
found A's `au` r1 with p99 1,958 ms and called a walk over 1 s not new on B; this run's A has it
in all four writing-walk cells, and C in none.

## 4. Per-cell comparison: C against A (the milestone) and C against B (the remedy)

Two runs per cell, A, B and C each; the delta is the difference of the two
runs' means (so it is derived and carries no spread). "n/a": the cell was not run for B.

**`ivw`**

| metric | A | B | C | C vs A (mean of runs) | C vs B |
|---|---|---|---|---|---|
| hot reads/s | 34.5 and 26.3 | 488.6 and 838.2 | 1,000 and 1,000 | +969.9 | +336.8 |
| hot p99 (µs) | 253,486 and 263,380 | 9,006 and 9,746 | 320.7 and 365 | -258,089.9 | -9,032.8 |
| hot max (µs) | 268,230 and 265,051 | 3,436,931 and 4,124,915 | 1,534 and 2,850 | -264,448.7 | -3,778,731.4 |
| walks finished | 189 and 190 | 50 and 101 | 194 and 192 | +3.5 | +117.5 |
| walk p50 (ms) | 122 and 121 | 133 and 133 | 123 and 124 | +2.0 | -9.5 |
| walk p99 (ms) | 145 and 161 | 17,354 and 1,380 | 141 and 146 | -9.5 | -9,223.5 |
| walk max (ms) | 987 and 826 | 17,354 and 19,037 | 147 and 152 | -757.0 | -18,046.0 |
| walks over 1 s | 0 and 0 | 2 and 6 | 0 and 0 | +0.0 | -4.0 |
| WAL appended (MB) | 170.5 and 130.1 | 2,081 and 4,943 | 0.0 and 0.0 | -150.3 | -3,511.9 |
| page images | 20,800 and 15,872 | 253,756 and 602,676 | 0 and 0 | -18,336.0 | -428,216.0 |
| sync-gate wait (s) | 0.0 and 0.0 | 14.8 and 5.9 | 0 and 0 | -0.0 | -10.4 |

**`ivu`**

| metric | A | B | C | C vs A (mean of runs) | C vs B |
|---|---|---|---|---|---|
| hot reads/s | 7.3 and 6.2 | 393.1 and 286.4 | 1,000 and 999.5 | +993.1 | +660.1 |
| hot p99 (µs) | 661,782 and 3,390,868 | 343,554 and 687,014 | 51,063 and 50,851 | -1,975,367.6 | -464,326.9 |
| hot max (µs) | 661,810 and 3,390,912 | 1,383,781 and 1,607,954 | 54,760 and 61,159 | -1,968,401.6 | -1,437,907.7 |
| walks finished | 79 and 60 | 10 and 16 | 83 and 82 | +13.0 | +69.5 |
| walk p50 (ms) | 294 and 317 | 1,095 and 306 | 283 and 286 | -21.0 | -416.0 |
| walk p99 (ms) | 1,039 and 1,907 | 15,440 and 18,219 | 383 and 365 | -1,099.0 | -16,455.5 |
| walk max (ms) | 1,039 and 1,907 | 15,440 and 18,219 | 383 and 365 | -1,099.0 | -16,455.5 |
| walks over 1 s | 1 and 4 | 5 and 3 | 0 and 0 | -2.5 | -4.0 |
| WAL appended (MB) | 6,769 and 4,699 | 1,488 and 2,019 | 7,976 and 7,862 | +2,185.1 | +6,165.5 |
| page images | 3,584 and 2,880 | 131,354 and 154,018 | 64,001 and 57,098 | +57,317.5 | -82,136.5 |
| sync-gate wait (s) | 1.5 and 3.5 | 16.9 and 16.6 | 1.0 and 1.0 | -1.5 | -15.8 |

**`ivr`**

| metric | A | B | C | C vs A (mean of runs) | C vs B |
|---|---|---|---|---|---|
| hot reads/s | 1,000 | not run | 1,000 | +0.0 | n/a |
| hot p99 (µs) | 352.6 | not run | 338.1 | -14.5 | n/a |
| hot max (µs) | 2,199 | not run | 2,852 | +653.5 | n/a |
| walks finished | 531 | not run | 532 | +1.0 | n/a |
| walk p50 (ms) | 45 | not run | 45 | +0.0 | n/a |

**`i`**

| metric | A | B | C | C vs A (mean of runs) | C vs B |
|---|---|---|---|---|---|
| hot reads/s | 1,000 | not run | 1,000 | -0.0 | n/a |
| hot p99 (µs) | 536.2 | not run | 524.8 | -11.4 | n/a |
| hot max (µs) | 3,330 | not run | 3,217 | -113.2 | n/a |

**`aw`**

| metric | A | B | C | C vs A (mean of runs) | C vs B |
|---|---|---|---|---|---|
| walks finished | 190 and 191 | 17 and 37 | 196 and 197 | +6.0 | +169.5 |
| walk p50 (ms) | 122 and 122 | 136 and 132 | 122 and 121 | -0.5 | -12.5 |
| walk p99 (ms) | 145 and 163 | 16,885 and 19,372 | 144 and 149 | -7.5 | -17,982.0 |
| walk max (ms) | 858 and 822 | 16,885 and 19,372 | 150 and 162 | -684.0 | -17,972.5 |
| walks over 1 s | 0 and 0 | 4 and 3 | 0 and 0 | +0.0 | -3.5 |
| WAL appended (MB) | 175.7 and 153.2 | 1,276 and 2,166 | 0.0 and 0.0 | -164.4 | -1,721.0 |
| page images | 21,440 and 18,688 | 155,580 and 264,124 | 0 and 0 | -20,064.0 | -209,852.0 |
| sync-gate wait (s) | 0.0 and 0.0 | 17.5 and 15.1 | 0 and 0 | -0.0 | -16.3 |

**`au`**

| metric | A | B | C | C vs A (mean of runs) | C vs B |
|---|---|---|---|---|---|
| walks finished | 30 and 43 | 4 and 21 | 83 and 82 | +46.0 | +70.0 |
| walk p50 (ms) | 448 and 305 | 14,419 and 295 | 284 and 287 | -91.0 | -7,071.5 |
| walk p99 (ms) | 2,039 and 1,586 | 18,931 and 23,590 | 369 and 381 | -1,437.5 | -20,885.5 |
| walk max (ms) | 2,039 and 1,586 | 18,931 and 23,590 | 369 and 381 | -1,437.5 | -20,885.5 |
| walks over 1 s | 11 and 9 | 2 and 3 | 0 and 0 | -10.0 | -2.5 |
| WAL appended (MB) | 1,858 and 2,842 | 1,275 and 2,886 | 7,881 and 7,862 | +5,521.4 | +5,791.4 |
| page images | 1,152 and 1,792 | 142,115 and 126,336 | 57,094 and 57,094 | +55,622.0 | -77,131.5 |
| sync-gate wait (s) | 5.8 and 6.3 | 22.8 and 13.9 | 1.5 and 1.4 | -4.6 | -16.9 |



**The spread.** Two runs of one arm differ in `ivw` A by 8.2 reads/s (34.5
and 26.3, 27 % of their mean), by 4 % in p99; C's two runs by 14 % in p99 (320.7 and 365.0 µs) and
0 % in reads/s; in `ivu` C's p99 by 0.4 % and A's by a factor of 5 (0.66 and 3.39
s). B's two runs differ by a factor of 1.7 and 1.4 in reads/s (`ivw`, `ivu`), so **C against B
in `ivw` is a factor of 27 in p99 (9.4 ms against 0.34 ms, means) and 1,000 to 2,000 in the max, against
a run-to-run spread of a factor of 2**; **C against A in `ivw` is a factor of 750 in p99 and in `ivu`
13 to 67**; C against A in the **walker's p0 and in
`ivr`/`i`** is inside the spread (+0 to +3 ms of 118-121, -11.4 and -14.5 µs of
525-536 and 338-353).

## 5. Where the time goes, the log, and what stalls (rule 3)

Share of the window (`total_us` of the event over the window, derived). `on_cpu`
is the reactor's; `sched.run_queue` is summed over sessions and can pass 100 %.
`io.page_read` is 0.00 % in every cell (no device read). No `lock.*` wait
occurred and there were **0 errors in all 28 cells of the tables** (`first_error` null). `fg polls` is the number of foreground
polls in the window: **A's walk is one poll a walk (21-845 polls a window), C's one a
slice (136,809-343,293)**. A "hold" is a statement's continuous hold of its
core (`sched_hold_in_run_*`), what a hot read waits behind.

| cell | arm | run | window | `on_cpu` | `sched.run_queue` (sum) | `io.page_read` | system polls | fg polls | longest hold | holds > 100 ms |
|---|---|---|---|---|---|---|---|---|---|---|
| ivw | A | r0 | 19.9 s | 100.5 % | 125.8 % | 0.0 % | 332 | 845 | 9.9 ms | 161 |
| ivw | A | r1 | 19.9 s | 100.5 % | 211.3 % | 0.0 % | 259 | 682 | 0.0 ms | 161 |
| ivw | B | r0 | 20.9 s | 99.6 % | 121.7 % | 0.0 % | 5,876 | 72,154 | 3,329.1 ms | 8 |
| ivw | B | r1 | 20.9 s | 99.3 % | 63.8 % | 0.0 % | 11,910 | 179,632 | 4,042.7 ms | 2 |
| ivw | C | r0 | 20.0 s | 99.0 % | 7.3 % | 0.0 % | 113 | 343,293 | 0.0 ms | 0 |
| ivw | C | r1 | 20.0 s | 99.0 % | 7.4 % | 0.0 % | 542 | 340,520 | 0.0 ms | 0 |
| ivu | A | r0 | 19.2 s | 102.8 % | 292.2 % | 0.0 % | 75 | 204 | 40.2 ms | 63 |
| ivu | A | r1 | 20.4 s | 102.7 % | 277.7 % | 0.0 % | 57 | 162 | 1,592.2 ms | 44 |
| ivu | B | r0 | 19.5 s | 99.8 % | 282.8 % | 0.0 % | 6,032 | 15,867 | 1,327.1 ms | 19 |
| ivu | B | r1 | 20.2 s | 99.8 % | 289.6 % | 0.0 % | 2,780 | 18,058 | 1,530.3 ms | 20 |
| ivu | C | r0 | 20.0 s | 99.1 % | 65.9 % | 0.0 % | 1,756 | 158,107 | 50.6 ms | 0 |
| ivu | C | r1 | 20.0 s | 99.1 % | 61.3 % | 0.0 % | 1,440 | 158,402 | 50.6 ms | 0 |
| ivr | A | r0 | 20.0 s | 98.0 % | 4.3 % | 0.0 % | 444 | 908,098 | 0.1 ms | 0 |
| ivr | C | r0 | 20.0 s | 98.0 % | 4.3 % | 0.0 % | 461 | 911,348 | 0.1 ms | 0 |
| i | A | r0 | 20.0 s | 4.5 % | 0.2 % | 0.0 % | 63 | 20,005 | 0.0 ms | 0 |
| i | C | r0 | 20.0 s | 4.5 % | 0.2 % | 0.0 % | 52 | 20,005 | 0.0 ms | 0 |
| aw | A | r0 | 19.8 s | 101.2 % | 101.6 % | 0.0 % | 348 | 164 | 13.7 ms | 160 |
| aw | A | r1 | 19.8 s | 100.5 % | 101.0 % | 0.0 % | 318 | 163 | 28.8 ms | 160 |
| aw | B | r0 | 20.5 s | 99.9 % | 84.9 % | 0.0 % | 2,904 | 20,198 | 1,873.9 ms | 19 |
| aw | B | r1 | 20.1 s | 99.9 % | 39.1 % | 0.0 % | 5,401 | 38,683 | 2,909.2 ms | 9 |
| aw | C | r0 | 20.0 s | 99.4 % | 0.4 % | 0.0 % | 137 | 327,399 | 0.4 ms | 0 |
| aw | C | r1 | 20.0 s | 99.4 % | 0.4 % | 0.0 % | 141 | 328,415 | 0.2 ms | 0 |
| au | A | r0 | 16.1 s | 104.6 % | 108.6 % | 0.0 % | 34 | 21 | 262.0 ms | 18 |
| au | A | r1 | 19.2 s | 103.0 % | 106.0 % | 0.0 % | 44 | 30 | 1,268.1 ms | 27 |
| au | B | r0 | 24.8 s | 100.0 % | 96.5 % | 0.0 % | 2,109 | 2,112 | 5,493.4 ms | 19 |
| au | B | r1 | 19.7 s | 99.9 % | 68.9 % | 0.0 % | 3,840 | 34,262 | 3,988.5 ms | 10 |
| au | C | r0 | 20.0 s | 99.6 % | 2.1 % | 0.0 % | 1,557 | 138,533 | 26.7 ms | 0 |
| au | C | r1 | 20.0 s | 99.6 % | 1.5 % | 0.0 % | 1,657 | 136,809 | 0.0 ms | 0 |

**The reactor is on the CPU ~99-103 % of every window with a walker, on all three
arms** (C does not make the walk cheaper, it makes it interruptible): the
difference is who waits. A's `sched.run_queue` (the sessions' wait for the core)
is 126-211 % in `ivw`, C's **7.3 and 7.4 %**, B's 121.7 and 63.8 %. A's longest hold
in `ivw` is the walk itself (161 holds of 100 ms or more, one a walk) and **C's
is 0.0 ms (`ivw`) and 0.4 and 0.2 ms (`aw`), and no C cell has a hold over 100 ms**; in the writing cells
C's longest hold is 50.6 ms (`ivu`) and 26.7 ms and 0.0 ms (`au`), B's 1.3-5.5 s. The system task is
polled 113-542 times in `ivw` on C (A 259-332, B 5,876-11,910) and **1,440-1,756 times with the writing walk on C**,
where A polls it 34-75 times (its walk does not return the core) and B 2,109-6,032.

**The log and the images** (`pool_fetch_images` is the count of page images a
fetch for write logged, `wal.md` §10: a frame owes an image from its fault-in
and each writeback copy; "ckpt pages" is `contention_checkpoint_pages_written`,
checkpoint runs include the cadence's idle runs, 4 in an idle window):

| cell | arm | run | WAL appended | page images (MB) | WAL syncs | sync-gate waits (count / total) | checkpoint runs | ckpt pages written | ring full |
|---|---|---|---|---|---|---|---|---|---|
| ivw | A | r0 | 170.5 MB | 20,800 (170) | 161 | 2 / 0.04 s | 0 | 21,248 | 106 |
| ivw | A | r1 | 130.1 MB | 15,872 (130) | 160 | 1 / 0.02 s | 0 | 16,576 | 52 |
| ivw | B | r0 | 2,081.1 MB | 253,756 (2,080) | 174 | 31 / 14.81 s | 2 | 308,218 | 1,918 |
| ivw | B | r1 | 4,942.6 MB | 602,676 (4,941) | 549 | 74 / 5.94 s | 3 | 712,503 | 4,571 |
| ivw | C | r0 | 0.0 MB | 0 (0) | 330 | 0 / 0.00 s | 4 | 4 | 0 |
| ivw | C | r1 | 0.0 MB | 0 (0) | 324 | 0 / 0.00 s | 4 | 4 | 0 |
| ivu | A | r0 | 6,769.1 MB | 3,584 (29) | 62 | 59 / 1.49 s | 0 | 4,288 | 6,396 |
| ivu | A | r1 | 4,698.8 MB | 2,880 (24) | 43 | 35 / 3.46 s | 0 | 3,264 | 4,439 |
| ivu | B | r0 | 1,487.5 MB | 131,354 (1,077) | 68 | 22 / 16.91 s | 0 | 134,592 | 1,368 |
| ivu | B | r1 | 2,019.4 MB | 154,018 (1,263) | 120 | 30 / 16.62 s | 0 | 173,092 | 1,853 |
| ivu | C | r0 | 7,975.9 MB | 64,001 (526) | 741 | 101 / 0.96 s | 4 | 64,025 | 7,306 |
| ivu | C | r1 | 7,862.2 MB | 57,098 (470) | 731 | 103 / 0.99 s | 4 | 57,122 | 7,211 |
| ivr | A | r0 | 0.0 MB | 0 (0) | 8 | 0 / 0.00 s | 4 | 4 | 0 |
| ivr | C | r0 | 0.0 MB | 0 (0) | 8 | 0 / 0.00 s | 4 | 4 | 0 |
| i | A | r0 | 0.0 MB | 0 (0) | 8 | 0 / 0.00 s | 4 | 4 | 0 |
| i | C | r0 | 0.0 MB | 0 (0) | 8 | 0 / 0.00 s | 4 | 4 | 0 |
| aw | A | r0 | 175.7 MB | 21,440 (176) | 160 | 2 / 0.01 s | 0 | 22,272 | 104 |
| aw | A | r1 | 153.2 MB | 18,688 (153) | 160 | 1 / 0.00 s | 0 | 20,352 | 88 |
| aw | B | r0 | 1,275.8 MB | 155,580 (1,275) | 61 | 19 / 17.54 s | 0 | 178,364 | 1,180 |
| aw | B | r1 | 2,166.2 MB | 264,124 (2,165) | 172 | 33 / 15.11 s | 2 | 323,642 | 1,995 |
| aw | C | r0 | 0.0 MB | 0 (0) | 334 | 0 / 0.00 s | 4 | 4 | 0 |
| aw | C | r1 | 0.0 MB | 0 (0) | 333 | 0 / 0.00 s | 4 | 4 | 0 |
| au | A | r0 | 1,858.4 MB | 1,152 (9) | 17 | 17 / 5.77 s | 0 | 2,176 | 1,755 |
| au | A | r1 | 2,842.1 MB | 1,792 (15) | 26 | 23 / 6.28 s | 0 | 2,816 | 2,685 |
| au | B | r0 | 1,274.6 MB | 142,115 (1,166) | 53 | 19 / 22.81 s | 0 | 131,556 | 1,180 |
| au | B | r1 | 2,885.8 MB | 126,336 (1,036) | 230 | 42 / 13.85 s | 1 | 234,788 | 2,638 |
| au | C | r0 | 7,881.1 MB | 57,094 (470) | 678 | 112 / 1.51 s | 4 | 57,118 | 7,188 |
| au | C | r1 | 7,862.2 MB | 57,094 (470) | 695 | 110 / 1.40 s | 4 | 57,118 | 7,198 |

**C's images are the checkpoint's copies and nothing else.** In `ivw` and `aw`
C's window has 0 images, 0 MB of log and 4 pages written by the checkpoint (the same
4 as an idle `i` window): a walk that matches nothing fetches no leaf for
write, so no page is dirty for the checkpoint to copy and none owes an image.
B's same cells have 253,756-602,676 images (`ivw`) and 155,580-264,124 (`aw`), up
to 4.7 images a page in the relation's 128,960 pages (602,676 / 128,960,
derived), 14.8 and 5.9 s (`ivw`) and 17.5 and 15.1 s (`aw`) of **WAL sync-gate
wait**, and holds of 1.3-5.5 s (B's cells with a walker); A logs 15,872-21,440 images (one for each page the
walk fetched for write and that was not yet imaged in the window: about a sixth
of the relation, as the first close found) and waits 0.00-0.04 s on the gate.
In the writing cells (`ivu`, `au`) C logs **57,094-64,001 images and its
checkpoint writes 57,118-64,025 pages**, 24 pages more than the images in each of
four windows (derived): every page the checkpoint copies owes its next writer
one image, which is the DH rule working as `wal.md` §10 states it. The writing
walk writes the same ~7,150 leaves every walk (c0 in 1..50,000), so the 57,094-64,001
images are 8 to 9 images a leaf over the window (derived), against 70 walks writing each leaf. A's writing cells
have 1,152-3,584 images because the walk never returns the core, **A's
checkpoint ran 0 times in all of its walker cells in the window** (its cadence cannot
run while one poll holds the core for a walk, with 0 `contention_checkpoint_runs` and 34-75 system
polls), and its log is recycled only at a roll that waits: **sync-gate waits of 17-59, 1.5-6.3 s in total**, against C's 101-112 waits, **0.96-1.51 s**.

WAL appended per **walk begun in the window** (derived from the table and the
walks "in window"; windows differ in length by up to 20 % between arms, so this is
an average, and a partial walk at the edge is in the divisor): `ivu` A
104 and 102 MB (6,769 / 65, 4,699 / 46), C 114 and 112 MB (7,976 / 70, 7,862 / 70), `au` A 93
and 98 MB (1,858 / 20, 2,842 / 29), C 113 and 112 MB (7,881 / 70,
7,862 / 70): **C logs 9 to 21 % more WAL per writing walk than A**, which is the
images (470-526 MB over 70 walks, about 7 MB a walk). The cost of the look-first
rule on the write side is that the cadence checkpoint can now run during the walk,
and the pages it copies are the pages the walk then writes again.

**Hot sessions' tail with the writing walk, located only so far.** The ~50 ms
plateau of section 3 is a poll, not a lock or device wait the instance counts:
`sys.wait_stats`'s events in the window are `sysio.wal_write` (9 waits, 84.8 ms
in all), `sysio.checkpoint` (8, 13.9 ms) and `sched.run_queue`, **none long
enough to account for 120 polls of 10 ms or more**; the WAL sync gate (101 and
103 waits, 0.96 and 0.99 s) and `wal_ring_full` (7,306 and 7,211) are the
candidates, and neither adds up to 120 x 50 ms = 6 s. The source of the 50 ms
poll was not isolated here.

## 6. DT-R4: a write that takes no pause pays nothing

`tools/dr_close.py --part ab --rounds 15`, unmodified: `cores = 1`, `relaxed`, a
fresh server per cell on CPU 2, 100,000 rows in a btree (`id int64`) and a heap (`id
int64 PRIMARY KEY`), pool 131,072 frames, 15 rounds, arms A, **A2 (a second copy
of A, the control)** and **C** in six rotating orders (45 cells, 188.8 s, the
driver's own `cc1plus` quiet-gate hold 0.0 s; a sampler saw no competitor
during it). Hashes (first 16 hex): A and A2 `d9e683d9b312c7e2`, C
`4b8041af1ef68679`. **Server CPU a statement** is the median over a phase's 20
batches, and each row's number is the median over the 15 rounds with the range
over rounds beside it (ns/statement, derived from `cpu_us` x 1,000). Statement
counts: reads 20,000, `UPDATE`/`INSERT`/`DELETE` 4,000, range 1,000, walked 40.

| relation | statement | A | A2 (control) | C | C - A | A2 - A (range over rounds) | C - A2 | per-round C - A | rounds C > A |
|---|---|---|---|---|---|---|---|---|---|
| btree | point `SELECT` by key | 6,330 (6,190-6,530) | 6,310 | 6,140 (6,050-6,200) | -190 ns (-3.0 %) | -20 (-200 .. +120) | -170 ns (-2.7 %) | -340 .. -10 | 0 of 15 |
| btree | point `UPDATE` by key | 7,010 (6,860-7,160) | 7,030 | 6,800 (6,760-6,940) | -210 ns (-3.0 %) | +20 (-210 .. +100) | -230 ns (-3.3 %) | -390 .. -40 | 0 of 15 |
| btree | single-row `INSERT` | 6,530 (6,370-6,600) | 6,560 | 6,520 (6,360-6,650) | -10 ns (-0.2 %) | +30 (-90 .. +220) | -40 ns (-0.6 %) | -150 .. +130 | 6 of 15 |
| btree | `DELETE` by key | 6,130 (5,990-6,320) | 6,180 | 6,030 (5,890-6,350) | -100 ns (-1.6 %) | +50 (-190 .. +280) | -150 ns (-2.4 %) | -220 .. +50 | 2 of 15 |
| btree | range, 100 rows | 15,850 (15,620-16,090) | 15,830 | 15,730 (15,560-15,860) | -120 ns (-0.8 %) | -20 (-260 .. +400) | -100 ns (-0.6 %) | -360 .. +120 | 3 of 15 |
| heap | point `SELECT` by key | 6,910 (6,740-6,960) | 6,910 | 6,680 (6,630-8,130) | -230 ns (-3.3 %) | 0 (-120 .. +170) | -230 ns (-3.3 %) | -300 .. +1,170 | 1 of 15 |
| heap | point `UPDATE` by key | 7,500 (7,340-8,020) | 7,510 | 7,260 (7,150-9,270) | -240 ns (-3.2 %) | +10 (-540 .. +210) | -250 ns (-3.3 %) | -790 .. +1,670 | 1 of 15 |
| heap | single-row `INSERT` | 8,330 (8,170-8,440) | 8,380 | 8,260 (8,160-10,580) | -70 ns (-0.8 %) | +50 (-90 .. +260) | -120 ns (-1.4 %) | -230 .. +2,280 | 5 of 15 |
| heap | `DELETE` by key | 6,580 (6,430-6,810) | 6,590 | 6,470 (6,270-8,230) | -110 ns (-1.7 %) | +10 (-240 .. +250) | -120 ns (-1.8 %) | -540 .. +1,660 | 3 of 15 |
| heap | range, 100 rows | 25,290 (25,030-25,770) | 25,280 | 24,970 (24,780-28,380) | -320 ns (-1.3 %) | -10 (-420 .. +800) | -310 ns (-1.2 %) | -800 .. +2,740 | 4 of 15 |

**DT-R4 holds on C, by the numbers**: no point statement costs more on C than on A
(the largest per-round C - A in the point rows is +1,670 ns on a heap `UPDATE`,
one round of 15; the medians are -10 to -240 ns). The `SELECT`, which
**no DT code path touches**, is 190 and 230 ns (3.0 and 3.3 %) *faster* on C, while the control
A2 differs from A by -20 and 0 ns: so the two binaries differ by about 3 % on a
statement the change cannot have affected, and the same 3 % is in the write
rows (-210, -240 ns). This file does not assign it (a code-layout effect between
two builds is the usual cause and was not examined), and the first close's B
read +30 and +90 ns on the same `SELECT`; the reading for DT-R4 is that **the
write statements are no slower on C than the control statement is, and none is
slower than A**.

**A statement that does pause, from the same driver (not DT-R4).** The
driver's `walked` phase is `UPDATE t SET w = w + 1 WHERE v = <k>` with no key,
walking the whole relation (100,000 rows, 40 statements, each writing 100 rows;
the relation's leaves number past 64, so it pauses). The first close's B number
for the same cell, `v3.4.17-153-gd172085e8`, is beside it.

| relation | A (ns/stmt, median; range of 15) | A2 (control) | C | C - A | per-round C - A | rounds C > A | B - A (first close) |
|---|---|---|---|---|---|---|---|
| btree | 3,298,450 (3,286,860-3,332,680) | 3,305,390 | 3,510,930 (3,485,760-3,541,350) | **+212,480 ns (+6.4 %)** | +178,290 .. +239,920 | 15 of 15 | +430,540 ns (+13.1 %) |
| heap | 3,280,060 (3,268,470-3,312,430) | 3,282,700 | 3,202,740 (3,182,010-3,227,350) | **-77,320 ns (-2.4 %)** | -106,160 .. -58,010 | 0 of 15 | +21,080 ns (+0.6 %) |

The control moves +6,940 and +2,640 ns, so neither row is noise. **On a btree
the keyless walked `UPDATE` of 100 rows costs +212.5 µs of server CPU (+6.4 %) on C in 15 of
15 rounds, half of B's +430.5 µs (-218.1 µs, derived), and on a heap it costs
77.3 µs less than on A in 15 of 15 rounds** (A's walk fetches every page for write; C's
looks first and takes for writing only the pages holding the 100 rows).
C's remaining +212.5 µs on the btree **is the price of a statement that
yields on a relation of ~500 small leaves** (about 8 pauses by a back-of-envelope
count, ~27 µs a pause, unmeasured: the pause count was not read) **plus the shared look
before the write of each leaf that has a row**; this file does not separate the two. An
earlier try of this cell (try 2, with competitors present, discarded) read +225.6 µs
(+6.8 %) and -73.7 µs (-2.2 %), the same.

## 7. Against this engine's own earlier number

The first close (`v3.4.17-153-gd172085e8`) measured A and B on the same host the
same day; A reproduces it, so C is compared to the same A.

| shape | first close, A | this run, A | first close, B | this run, B | this run, C |
|---|---|---|---|---|---|
| `ivw` hot p99 | 245,226 and 265,067 µs | 253,486 and 263,380 µs | 1,622 and 10,261 µs | 9,006 and 9,746 µs | **320.7 and 365.0 µs** |
| `ivw` hot max | 254,369 and 267,142 µs | 268,230 and 265,051 µs | 1.39 and 7.66 s | 3.44 and 4.12 s | **1.53 and 2.85 ms** |
| `ivw` hot reads/s | 34.6 and 33.8 | 34.5 and 26.3 | 555.6 and 1,000.2 | 488.6 and 838.2 | **1,000.2 and 1,000.2** |
| `ivw` walker p50 | 121 and 121 ms | 122 and 121 ms | 131 and 133 ms | 133 and 133 ms | **123 and 124 ms** |
| `ivw` WAL / page images | 169 MB / 20,672, 165 MB / 20,160 | 171 / 20,800, 130 / 15,872 | 3,020 / 368,312, 1,909 / 232,764 | 2,081 / 253,756, 4,943 / 602,676 | **0 / 0, 0 / 0** |
| `ivu` hot p99 | 658.9 and 640.0 ms | 0.66 and 3.39 s | 50.3 and 50.2 ms | 343.6 and 687.0 ms | **50.9 and 51.1 ms** |
| `au` walks finished | 80 and 63 | 30 and 43 | 3 and 49 | 4 and 21 | **83 and 82** |
| DT-R4 walked `UPDATE`, btree, vs A | | | +430.5 µs (+13.1 %) | | **+212.5 µs (+6.4 %)** |
| DT-R4 walked `UPDATE`, heap, vs A | | | +21.1 µs (+0.6 %) | | **-77.3 µs (-2.4 %)** |

A reproduces the first close in `ivw` (p99 within 1.5 %, reads/s 34.5 against 34.6
and one run at 26.3, a spread this cell has shown before), in `ivw`'s walker
(121-122 ms) and in WAL and images (in range); **A's `ivu` and `au` tails are heavier here (a p99 of 3.39 s against 0.66 s in
one run; 30 and 43 walks against 80 and 63)**, which this file reports and does
not explain: the writing walks of A depend on a log roll that waits (section 5), and
A's two runs in `au` differ by 43 % in walks finished. B reproduces the pattern
of the first close, not its numbers: the stall (a max of seconds, 100x images,
sync-gate waits of 5.9-22.8 s) in every B cell, with p99s that move from run to
run by an order of magnitude.

## 8. What the run says about the engine

1. **The stall was the write walk dirtying leaves it did not change, and the
   remedy removes it where it was found.** Same cell, same walk, same cadence:
   B logs 253,756-602,676 images and waits 5.9-14.8 s on the log's sync gate,
   C logs none and waits none, and the hot sessions' max falls from 3.4-4.1 s to
   1.5-2.8 ms. The first close named the cadence checkpoint as a candidate; this run
   adds the contrast that **a walk that matches nothing now dirties no page, so the checkpoint
   that runs between slices copies 4 pages (an idle window's) instead of 308,218-712,503.**
   What this does not show is which of the checkpoint's writeback and the log's
   sync gate is the bound on B (the two fall away together).
2. **The yield's price on a write walk was the exclusive fetch, not the pause.** B's
   no-match walk was 9-10 % slower than A's at p0 (128-130 against 118-119 ms);
   C's is within 0 to 3 ms of A's (119-121 ms) with the same ~2,000 slices a walk
   (C: 136,809-343,293 foreground polls in a window). The first close derived
   ~5 µs a pause from that +10 ms; the remedy shows the 10 ms was the leaf
   fetched for write, not the poll's re-parse and re-descent.
3. **The checkpoint now runs where a long write walk has pages to give it, and
   every page it copies is imaged once more** (images = checkpoint pages, within 24
   in four windows). With a walk that writes the same leaves 70 times in a
   window that is a log 9 to 21 % above A's per walk; and in return the **log is
   recycled between slices**: C's writing walks wait on the sync gate for 1.4-1.5
   s a window against A's 5.8-6.3 s (`au`), finish 83 walks to A's 30-43, and have no walk
   over 1 s where A has 9-11. That the checkpoint's run is what lets the log
   recycle, so the walk does not stall at a roll, is a reading the data are
   consistent with (A's checkpoint ran 0 times in these cells) and **was not isolated**.
4. **The tail that remains is a ~50 ms poll beside the writing walk** (hot
   p99.9 53.6-53.9 ms), and neither the wait events nor the sync-gate total
   account for it. In numbers against 1 ms (no p99 bar is applied here: DT-Q1's 1 ms was written for the cold-write stall and revised per mechanism in `workorder-dt-write-path-yield.md`), **the no-match walk's hot p99 is 0.32-0.37 ms and the writing walk's 51 ms**; the reading is the operator's.
5. **A write that pauses on a btree still costs +6.4 % of server CPU in a cell
   where the pause is the only difference** (`dr_close.py`'s keyless walked
   `UPDATE`), and costs -2.4 % on a heap, where the look-first spares the same walk
   99 % of its page dirtying. Both are measured; the split of the btree's +212.5 µs
   into the yield and the look was not.
6. **Read walks and point statements are unaffected** (`ivr`'s walker
   p0-p99 equal to A's, DT-R4, section 6).

## 9. What was not measured

- **Cell 6, a writing walk whose rows are spread over all leaves** (the remedy's worst
  case, which prices the shared look per leaf against the write it precedes), **was not run**:
  the data do not admit it without a reload. `c0` equals `id` for every row, so
  a predicate matching "1 row in 20" needs `c0 % 20 = 0` (an UPDATE's WHERE refuses
  an expression: `NOT_IMPLEMENTED ... got '%' (byte 39)`, tried on C) or a value
  list: an `IN` list above 4,096 values is refused (`an OR/NOT group of more than
  4096 conditions`), and one of 4,000 values (every 225th `c0`, 4,000 rows in 4,000 leaves)
  was admitted but the one walk it finished took **20.0 s** on C, the list being checked
  row by row, so it measures the list, not the leaf. These three trials ran on C only, not
  archived. The nearest cell that was measured is `au`, whose rows are in the
  first ~7,150 leaves, 5.5 % of the relation's.
- B's `ivr` and `i` (the controls were A and C only, one run each), a second run of
  either control, so no spread of this run's own for them.
- A spread for cell 5 beyond the 15 rounds; `cores > 1`; `strict`; a `SET
  PRIORITY analytic` session (the 16-leaf slice); a write driven by a list on
  the tuple key (DT's third path); a cancel's latency at the pause (DT-S3); a
  pool smaller than the relation; a heap relation as the walk's target (only
  `dr_close.py`'s heap relation); `INSERT`/`DELETE`/`PURGE` as walkers; a write
  set above 65,536 rows; the row-set sweep of rule 9 (section 2); the cadence at
  any other value than 5,000 ms (the first close's `checkpoint_interval_ms = 0`
  probe was not repeated: the remedy changes what the checkpoint finds, not
  whether it runs); the source of the ~50 ms poll (section 5) and the split of the
  btree walked-`UPDATE`'s +212.5 µs (section 6).
- **Cells that ran while another session built** are not in the tables (section
  2); those whose ends were clean but which were not sampled between are, and
  **a short competitor inside one of them would not show**.
- Interleave was by arm set, then broken for six cells (section 2); a drift
  between the first and the last cell of a set is not cancelled inside a set,
  and the six reruns ran 20-45 minutes after their sets.

**The milestone's close is not ruled on here.**
