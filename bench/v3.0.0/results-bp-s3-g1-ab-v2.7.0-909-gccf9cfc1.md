# BP-S3's own A/B at `cores = 1`: what H1-H4 removed is below this instrument's resolution, and the one movement it does show is a tail the CPU does not explain

> **Correction, 2026-10-12** (`results-bp-bq-close-v2.7.0-924-g3e77e7d1.md`): the `update-range` tail this file attributes to B belongs to `bn_census`'s **second server**, whichever binary it runs - in the close A/B the second server carries a 0.59-1.22 s statement in 60 of 60 arms and the first one never more than 270 ms. B was the second server here. Sections 3 and 7 read with that in mind; the CPU figures are unaffected.

**Thesis.** BP-S3 (`workorder-bp-per-core-residue.md`, BP-G1) cut four things a single-core instance had been paying
for: `StatementEpochs`' fences (H1), the window latch (H2), the Cabin store's latches (H3) and the mint's `live_` walk
(H4), and with them H5's page-stream stamp. BP-R6 asks the stage for its own A/B and BP-R1 says it **gates nothing**.
Measured with A = `1592007e` (`v2.7.0-904-g1592007e`, BP-S1's instrumented commit) against B = `ccf9cfc1`
(`v2.7.0-909-gccf9cfc1`, BP-S3's head), both Release builds from exports, at `cores = 1` and `relaxed`:

- **The server's CPU per point statement did not move by more than the instrument can see.** Median B - A is
  **-0.15 / -0.56 µs** for the point `SELECT` (literal / prepared) and **+0.67 / +0.54 µs** for the point `UPDATE`
  (-0.6 / -2.2 / +2.9 / +2.3 % of A's 23.5-26.1 µs), against a spread of B - A over three runs of 0.95-2.16 µs and an A that
  differs from itself by 1.5-2.5 µs from run to run. **The sign is the run's, not the binary's**: in r1 B is lower
  than A in 9-11 of 12 blocks in all four point shapes, in r2 it is higher in 10-12 of 12 in all four, in r3 it is
  mixed. Over the three runs no shape's 95 % interval excludes zero, and the half-width is 1.4-2.7 µs (5.6-10.6 % of
  a statement). BN's close measured the same floor at about 1 µs.
- **The every-row `UPDATE` over 10,000 rows is 90 / 131 µs lower (-1.3 / -1.8 %)**, 9-13 ns of a row, with the same
  sign in all six run-and-path means. That is the one shape in which a per-row item (H5's stamp write per logged
  mutation) could show; the movement is smaller than A's own run-to-run spread (194-209 µs) and its interval over runs
  contains zero, so it is reported as a direction, not as a result.
- **The commit loops are fsync-bound and say nothing about the change**: `group` 839.8 to 797.1 stmt/s (-5.1 %) inside
  an A spread of 17.7 %, `strict` 845.3 to 835.8 stmt/s (-1.1 %) inside 4.0 %; one `fsync` per statement on the
  reactor's thread, a p50 of 995 µs-1.03 ms on both sides.
- **What the CPU does not explain is B's tail.** Every one of B's six `update-range` arms contains one statement of
  **0.91-1.09 s** among its 60 (A's longest: 17-196 ms), so the rate at the mean falls 62-70 % while the rate at the
  median and per server-CPU second rise 1.5-1.9 %; and B's three `group` commit loops each hold one reactor-out-of-run pause of
  **142 / 217 / 201 ms** (A's: 26 / 24 / 23 ms). Neither is in a p99, neither is CPU, and these files cannot say
  whether they are B's or the device's. They are the item this report hands on (§3).

The Cabin-served `SELECT`, the cell BP-H3 names, **was not run**, so H3 is not exercised by anything below.
PostgreSQL is not part of this file: the floor was not measured for this shape.

| Item | Value |
|---|---|
| Executed | 2026-10-12 UTC. `bn_census` r1: 14:21:13-14:21:25; the twelve commit loops: 14:21:46-14:31:26; `bn_census` r2 and r3: **19:10:04-19:10:28**. Their first two tries failed at 14:21:45 and again at 16:59:10 (`rc = 1`: the driver reused a WAL directory whose early segments had been recycled, "segment 0 is missing"); `tools/bn_census.py` was fixed at 17:00:58 (`shutil.rmtree(<data>.wal)`, committed `0f10a751`) and the two runs went after BQ-S2's matrix released the host. **This file was written 2026-10-12 19:56 UTC from the existing data; no run, server, build or test was started in the session that wrote it** |
| Branch / worktree | `worktree-bp-per-core-residue`, worktree `bp-per-core-residue`; this file is written there at `1fb71d1a` (`v2.7.0-925-g1fb71d1a`) |
| A | `1592007e`, `git describe --tags` = **`v2.7.0-904-g1592007e`**, committed 2026-10-12 08:42:17 UTC. Binary `/home/cdkbs/bench-runs/bp-s1/v2/bin/kds_server.a`, **sha256 `44b7c2e5e2bcd4093b62c83201e65d17bc17afb8add9264e6ed0d5abc22dfd8a`** (built 14:11:04 from `v2/1592007e.tar`, byte-identical to `git archive 1592007e`, sha256 `fb673f34...0939`; the same hash as BP-S1's pass-1 binary) |
| B | `ccf9cfc1`, **`v2.7.0-909-gccf9cfc1`**, committed 13:46:17 UTC. Binary `kds_server.b`, **sha256 `a6fc3c04ef2bf2e19e3715ca1943c689959c97e9aa59a57a205029da1aea111f`** (built 14:09:30 from `v2/ccf9cfc1.tar`, byte-identical to `git archive ccf9cfc1`, sha256 `1eb04e67...352b`). The binary is newer than its commit |
| What lies between A and B | Five commits: `af9b77c9` (documents only), `676d1d7d` (BP-S1's counter fixes: the in-flight tally, the checkpoint's write counters, data syncs counted at the syncer, a counter on the fence walk deleted; none on the point-statement path), `230c4453` (**H1-H3**), `998903ab` (**H4**), `ccf9cfc1` (**H5**, the stamp and the envelope's core byte). **A to B is therefore H1-H5 plus the counter commit, and no cell separates them**: all land in B together |
| Tree cleanliness | Both binaries were built from exports (`v2/src-1592007e`, `v2/src-ccf9cfc1`), so the measured tree is clean by construction. Tool: `tools/bn_census.py` ran at its pre-fix state for r1 (14:21) and at the fixed state for r2 and r3 (19:10); the fix concerns only the mount of a fresh volume, not what is timed. `tools/ba_census.py` is as committed before `ffe7d4b8`'s `meta_delta` fix and `1fb71d1a`'s timeout variable (the working copy of 14:21 was not compared); neither touches the figures used here |
| Build | Release (`CMAKE_BUILD_TYPE:STRING=Release` in both `v2/build-*/CMakeCache.txt`) |
| Device | `/dev/root`, **ext4** on `nvme0n1p1` (`df -T` recorded as `ext4` at every cell's gate); data files and WAL under `/home/cdkbs/bench-runs/bp-s1/v2/work/`; not tmpfs. 8 vCPU AMD EPYC 9V74 = 4 physical cores x 2 SMT (`thread_siblings_list` 0-1, 2-3, 4-5, 6-7), Linux 7.0.0-1014-azure |
| Server configuration | `cores = 1`, `durability = relaxed` (`bn_census`) or the cell's (`group`, `strict` for the commit loops), `buffer_pool_frames = 65536`, `log_level = warn` (`bn_census`); every other key at its default (64 MiB segments, 1 MiB ring, checkpoint cadence 5000 ms). `bn_census`: **both servers running side by side**, both pinned to CPU 2, the client on CPU 4, only one ever busy; a fresh server and data file per binary per run. Commit loops: server CPU 2, client CPU 4, a fresh server per cell |
| Cells | `bn_census` (`--only select-pk,update-lit,update-range --rows 10000 --ops 3000 --range-ops 60 --blocks 12 --runs 1`), three runs, each over both driver paths (literal, prepared) and both binaries: 12 arms of 3,000 or 60 statements per run. `ba_census --shape insert1 --cores 1 --sessions 1 --seconds 20 --warmup 2 --runs 1`, at `group` and `strict`, A and B, three runs: 12 cells |
| Baseline | A of this file is the baseline (BP-R6: A at the stage's opening commit). The most recent file for the same driver and shapes is `results-bn-close-v2.7.0-787-g8679367e.md`, 117 commits earlier, on 1,000-row relations: its B read 28.1 µs of server CPU for the literal point `SELECT` and 24.5 µs for `UPDATE t SET v = ?`; this file's A reads 25.2-27.0 and 23.5-24.9 µs on 10,000-row relations. The two are not the same shape and are cited as history, not as a delta |
| Suite / overhead | Not executed (analysis of existing files only). The stage's suite claim is in `workorder-bp-per-core-residue.md` §7 (3603/3603, Debug). **The close measures the whole change once (BP-R6); this A/B is not that measurement** |

## 0. What was measured, and how to read the tables

**The A/B.** `tools/bn_census.py` starts one server per binary, loads the same relations into both, and for each shape
and driver path cuts the arm into 12 blocks taken alternately, the first server of a block alternating, so that host
drift lands on both sides alike. Per arm it records the client's wall time per statement (percentiles and mean), **the
server's on-CPU time** (the sum over every server thread of `/proc/<pid>/task/*/schedstat`, per statement, over the
arm and per block) and the client thread's own. The shapes are `select-pk` (`SELECT * FROM t WHERE id = ?`),
`update-lit` (`UPDATE t SET v = ? WHERE id = ?`) and `update-range` (`UPDATE r SET v = ? WHERE id >= 1`, every row of
a 10,000-row relation); **literal** is an unnamed `C_PARSE` per execution, **prepared** a named parse and a bind per
execution. Every reply is checked against the tag its shape must produce (`bad = 0` in all 36 arms), and the end
states are read back (`range_v_ok` and `spill_txt_ok` true on both binaries; 10,000 further updates allocate 0 pages
on either).

**Rows.** The relations hold 10,000 rows (`--rows 10000`). A pk point statement does not scale with rows, and the
every-row `UPDATE` does (one row costs 0.70 µs of server CPU on A: 7.0 ms over 10,000). **The 200 and 1,000-row
sizes were not run**, so the fixed and per-row parts of the every-row figure are not separated here; the point shapes
are the sizes' evidence of flatness and were not repeated at the other two.

**Units.** A table cell that is a rate says so: QPS and stmt/s are derived where marked (`1,000,000 / mean µs`).
The CPU cost per statement is a cost and stays in µs, with its throughput form (statements per server-CPU second) in
the next table. The driver records **p90** and not p95; the tables say p90. The `update-range` arms are 60
statements, so p90 is the 54th and p99 the maximum, and only p0-p50 describe the typical statement there.

**Noise floor, from inside the run.** Block `i` of A and of B share a host moment; A's block-to-block spread inside a
run, A's spread between runs, and the spread of B - A between runs are the three floors, and they are in the table
that carries each delta. **The unit of independence is the run, not the block**: the two servers are separate
processes, and a run's offset between them (placement, memory layout, which one was started first) is shared by its
12 blocks, which is why a sign test over 36 blocks is shown but not used.

## 1. The verdicts

BP-R1 gates nothing and BP-R6 reports; this table reads each cell against the floors above, not against a bar.

| Cell | B - A, median of 3 runs | Spread of B - A over runs; A's own run-to-run spread | Within noise? |
|---|---|---|---|
| point `SELECT`, literal | -0.15 µs (-0.6 %) of 26.08 µs server CPU | 1.86 µs; 1.81 µs | **Yes** |
| point `SELECT`, prepared | -0.56 µs (-2.2 %) of 25.78 µs | 2.16 µs; 2.47 µs | **Yes** |
| point `UPDATE`, literal | +0.67 µs (+2.9 %) of 23.97 µs | 0.95 µs; 1.47 µs | **Yes** |
| point `UPDATE`, prepared | +0.54 µs (+2.3 %) of 23.51 µs | 1.60 µs; 1.73 µs | **Yes** |
| every-row `UPDATE`, literal | -90 µs (-1.3 %) of 6,977 µs | 176 µs; 194 µs | **Yes**, same sign in 3 of 3 runs |
| every-row `UPDATE`, prepared | -131 µs (-1.8 %) of 7,075 µs | 129 µs; 209 µs | **Yes**, same sign in 3 of 3 runs; interval over runs [-320, +14] µs |
| wall-clock throughput, four point shapes | -0.4 / +0.5 / -0.8 / -1.2 % (derived QPS) | A's run-to-run spread 3.4 / 7.4 / 2.9 / 4.3 % | **Yes** |
| `group` commit loop | -5.1 % stmt/s | A's spread 17.7 % (r1 713.8, r2 839.8, r3 862.2) | **Yes** |
| `strict` commit loop | -1.1 % stmt/s | A's spread 4.0 % | **Yes** |
| every-row `UPDATE`, longest statement | 0.91-1.09 s in B's six arms against 17-196 ms in A's six | 6 of 6 against 0 of 6 | **No: not explained by the CPU** (§3) |
| `group` commit loop, longest pause | 142 / 217 / 201 ms in B against 26 / 24 / 23 ms in A | 3 of 3 against 0 of 3 | **No: not explained by the CPU** (§4) |

## 2. Server CPU per statement

A point statement costs the server 22.5-27.1 µs here, and the client 22-33 µs; the wall time of a serial driver is
the two processes' CPU end to end (below), so a change in the server's share is the whole of what a statement can
show.

**Server on-CPU time per statement (`/proc/<pid>/task/*/schedstat`, every server thread, over the arm), `cores = 1`, `relaxed`, 10,000 rows**

| shape (driver path) | statements per arm | A = `1592007e` r1 / r2 / r3 | B = `ccf9cfc1` r1 / r2 / r3 | B - A r1 / r2 / r3 | B - A, % of A, r1 / r2 / r3 | median B - A | spread of B - A (max - min) | A, run to run (max - min) | A, within a run: 12 blocks (max - min), median of runs |
|---|---|---|---|---|---|---|---|---|---|
| select-pk (literal) | 3,000 stmt | 26.08 µs / 25.18 µs / 26.98 µs | 25.20 µs / 26.16 µs / 26.83 µs | -0.88 µs / +0.98 µs / -0.15 µs | -3.4 % / +3.9 % / -0.6 % | -0.15 µs (-0.6 %) | 1.86 µs | 1.81 µs | 1.93 µs |
| select-pk (prepared) | 3,000 stmt | 25.78 µs / 24.58 µs / 27.05 µs | 25.22 µs / 25.43 µs / 25.74 µs | -0.56 µs / +0.84 µs / -1.31 µs | -2.2 % / +3.4 % / -4.9 % | -0.56 µs (-2.2 %) | 2.16 µs | 2.47 µs | 2.08 µs |
| update-lit (literal) | 3,000 stmt | 24.93 µs / 23.46 µs / 23.97 µs | 24.67 µs / 24.13 µs / 24.66 µs | -0.26 µs / +0.67 µs / +0.69 µs | -1.1 % / +2.9 % / +2.9 % | +0.67 µs (+2.9 %) | 0.95 µs | 1.47 µs | 2.53 µs |
| update-lit (prepared) | 3,000 stmt | 24.22 µs / 22.49 µs / 23.51 µs | 23.46 µs / 23.34 µs / 24.05 µs | -0.75 µs / +0.85 µs / +0.54 µs | -3.1 % / +3.8 % / +2.3 % | +0.54 µs (+2.3 %) | 1.60 µs | 1.73 µs | 1.42 µs |
| update-range (literal) | 60 stmt | 7,055.4 µs / 6,861.4 µs / 6,977.0 µs | 6,865.6 µs / 6,771.0 µs / 6,963.3 µs | -189.8 µs / -90.4 µs / -13.7 µs | -2.7 % / -1.3 % / -0.2 % | -90.4 µs (-1.3 %) | 176.2 µs | 194.0 µs | 2,390.4 µs |
| update-range (prepared) | 60 stmt | 7,203.6 µs / 6,994.7 µs / 7,074.5 µs | 6,974.7 µs / 6,894.6 µs / 6,943.8 µs | -228.8 µs / -100.0 µs / -130.7 µs | -3.2 % / -1.4 % / -1.8 % | -130.7 µs (-1.8 %) | 128.8 µs | 208.9 µs | 2,523.6 µs |

**The same, as statements per server-CPU second (derived: 1,000,000 / CPU µs per statement; median of the three runs)**

| shape (driver path) | A | B | B vs A |
|---|---|---|---|
| select-pk (literal) | 38,347 stmt/CPU-s | 38,225 stmt/CPU-s | -0.3 % |
| select-pk (prepared) | 38,791 stmt/CPU-s | 39,327 stmt/CPU-s | +1.4 % |
| update-lit (literal) | 41,720 stmt/CPU-s | 40,558 stmt/CPU-s | -2.8 % |
| update-lit (prepared) | 42,541 stmt/CPU-s | 42,620 stmt/CPU-s | +0.2 % |
| update-range (literal) | 143 stmt/CPU-s | 146 stmt/CPU-s | +1.6 % |
| update-range (prepared) | 141 stmt/CPU-s | 144 stmt/CPU-s | +1.9 % |

**The sign of B - A belongs to the run.** The four point shapes agree within a run and disagree between runs: r1 -0.26
to -0.88 µs (B lower in 9-11 of 12 blocks), r2 +0.67 to +0.98 µs (B lower in 0-2 of 12), r3 -1.31 to +0.68 µs. Runs r2 and
r3 started 12 seconds apart on a quiet host and disagree on `select-pk` (literal: +0.98 against -0.15 µs), so the offset
is not the host's moment. The per-block table makes the point; the run-as-unit table below is the one to read.

**Paired blocks: the arm is cut into 12 blocks taken alternately, so block `i` of A and of B share a host moment; B - A per block, in µs of server CPU per statement**

| shape (driver path) | run | mean of 12 paired B - A | standard error | blocks with B < A | sign test, one-sided p |
|---|---|---|---|---|---|
| select-pk (literal) | r1 | -0.87 µs | 0.28 µs | 11 of 12 | 0.003 |
| select-pk (literal) | r2 | +0.98 µs | 0.16 µs | 0 of 12 | 1.000 |
| select-pk (literal) | r3 | -0.15 µs | 0.77 µs | 3 of 12 | 0.981 |
| select-pk (literal) | **all 36** | **-0.01 µs** | 0.30 µs | **14 of 36** | 0.934 |
| select-pk (prepared) | r1 | -0.56 µs | 0.48 µs | 9 of 12 | 0.073 |
| select-pk (prepared) | r2 | +0.84 µs | 0.14 µs | 0 of 12 | 1.000 |
| select-pk (prepared) | r3 | -1.31 µs | 0.92 µs | 7 of 12 | 0.387 |
| select-pk (prepared) | **all 36** | **-0.34 µs** | 0.37 µs | **16 of 36** | 0.797 |
| update-lit (literal) | r1 | -0.26 µs | 0.68 µs | 9 of 12 | 0.073 |
| update-lit (literal) | r2 | +0.67 µs | 0.22 µs | 2 of 12 | 0.997 |
| update-lit (literal) | r3 | +0.68 µs | 0.35 µs | 4 of 12 | 0.927 |
| update-lit (literal) | **all 36** | **+0.36 µs** | 0.27 µs | **15 of 36** | 0.879 |
| update-lit (prepared) | r1 | -0.75 µs | 0.23 µs | 9 of 12 | 0.073 |
| update-lit (prepared) | r2 | +0.85 µs | 0.15 µs | 0 of 12 | 1.000 |
| update-lit (prepared) | r3 | +0.54 µs | 0.71 µs | 3 of 12 | 0.981 |
| update-lit (prepared) | **all 36** | **+0.21 µs** | 0.27 µs | **12 of 36** | 0.986 |
| update-range (literal) | r1 | -189.8 µs | 86.7 µs | 11 of 12 | 0.003 |
| update-range (literal) | r2 | -90.4 µs | 88.1 µs | 11 of 12 | 0.003 |
| update-range (literal) | r3 | -13.7 µs | 83.2 µs | 6 of 12 | 0.613 |
| update-range (literal) | **all 36** | **-98.0 µs** | 49.7 µs | **28 of 36** | 0.001 |
| update-range (prepared) | r1 | -228.8 µs | 73.3 µs | 10 of 12 | 0.019 |
| update-range (prepared) | r2 | -100.0 µs | 76.0 µs | 10 of 12 | 0.019 |
| update-range (prepared) | r3 | -130.7 µs | 82.2 µs | 9 of 12 | 0.073 |
| update-range (prepared) | **all 36** | **-153.2 µs** | 44.3 µs | **29 of 36** | 0.000 |

**The run as the unit: mean of the three per-run B - A server-CPU deltas, its standard error (n = 3) and the 95 % interval (t, 2 degrees of freedom: 4.30)**

| shape (driver path) | mean of 3 run deltas | standard error | 95 % interval | contains 0 | detectable change at this resolution (half-width) | half-width as % of A |
|---|---|---|---|---|---|---|
| select-pk (literal) | -0.01 µs | 0.54 µs | [-2.34, +2.32] µs | yes | 2.33 µs | 8.9 % |
| select-pk (prepared) | -0.34 µs | 0.63 µs | [-3.07, +2.38] µs | yes | 2.72 µs | 10.6 % |
| update-lit (literal) | +0.37 µs | 0.31 µs | [-0.98, +1.72] µs | yes | 1.35 µs | 5.6 % |
| update-lit (prepared) | +0.21 µs | 0.49 µs | [-1.90, +2.33] µs | yes | 2.11 µs | 9.0 % |
| update-range (literal) | -98 µs | 51 µs | [-317, +121] µs | yes | 219 µs | 3.1 % |
| update-range (prepared) | -153 µs | 39 µs | [-320, +14] µs | yes | 167 µs | 2.4 % |

**Reading it.** With the run as the unit, no shape's interval excludes zero, and the resolution (the interval's
half-width) is 1.35-2.72 µs for a point statement (5.6-10.6 % of it) and 2.4-3.1 % of the every-row one. The every-row
`UPDATE` is the only cell whose six means all point the same way (B lower by 14-229 µs); that is 9-13 ns of a row
(-90 / -131 µs over 10,000 rows) and inside A's own run-to-run spread, so it is a direction. An item of tens of
nanoseconds *per statement* (§5; none was timed here) is about 1e-5 of a 7 ms statement and cannot show in it, so
**if the every-row figure is real it is probably not H1-H4's**: of the five items in B the one that scales with rows is H5
(a thread-local read and a two-byte store per logged mutation, and an every-row `UPDATE` logs one per row) `[inferred]`.

## 3. Wall-clock time, and the tail the CPU does not explain

**Throughput of the serial driver (derived: QPS = 1,000,000 / mean wall µs per statement, from the arm's mean; median of the three runs)**

| shape (driver path) | A | B | B vs A | A r1 / r2 / r3 | B r1 / r2 / r3 |
|---|---|---|---|---|---|
| select-pk (literal) | 17,232 QPS | 17,165 QPS | -0.4 % | 17,232 / 17,435 / 16,856 QPS | 17,471 / 17,165 / 16,930 QPS |
| select-pk (prepared) | 18,159 QPS | 18,246 QPS | +0.5 % | 18,159 / 18,635 / 17,294 QPS | 18,246 / 18,352 / 17,723 QPS |
| update-lit (literal) | 20,836 QPS | 20,678 QPS | -0.8 % | 20,397 / 21,011 / 20,836 QPS | 20,448 / 20,781 / 20,678 QPS |
| update-lit (prepared) | 22,237 QPS | 21,977 QPS | -1.2 % | 21,729 / 22,675 / 22,237 QPS | 21,713 / 22,273 / 21,977 QPS |
| update-range (literal) | 106.7 QPS | 40.0 QPS | -62.5 % | 104.5 / 112.6 / 106.7 QPS | 38.6 / 40.0 / 41.4 QPS |
| update-range (prepared) | 136.6 QPS | 41.1 QPS | -69.9 % | 76.6 / 137.6 / 136.6 QPS | 40.2 / 42.6 / 41.1 QPS |

For `update-range` the mean is not a typical statement: every one of B's six arms (3 runs x 2 paths) has one statement of 0.91-1.09 s among its 60, which puts the mean at 23.5-25.9 ms against medians of 6.54-6.72 ms; A's longest statements are 17-196 ms. The rate at the median is in the next table and agrees with the CPU (section 3).

**Wall-clock latency per statement (client side), median of the three runs per percentile; `p90` stands where the rule asks for `p95` (the driver records p90)**

| shape (driver path) | binary | statements per run | p0 | p25 | p50 | p90 | p99 | 1,000,000 / p50 |
|---|---|---|---|---|---|---|---|---|
| select-pk (literal) | A | 3,000 | 52.84 µs | 54.46 µs | 55.24 µs | 62.25 µs | 73.76 µs | 18,101.8 QPS |
| select-pk (literal) | B | 3,000 | 52.96 µs | 55.04 µs | 55.80 µs | 62.44 µs | 76.16 µs | 17,919.9 QPS |
| select-pk (prepared) | A | 3,000 | 49.70 µs | 51.28 µs | 52.04 µs | 60.30 µs | 73.78 µs | 19,216.4 QPS |
| select-pk (prepared) | B | 3,000 | 49.61 µs | 51.71 µs | 52.45 µs | 60.72 µs | 80.28 µs | 19,066.2 QPS |
| update-lit (literal) | A | 3,000 | 37.67 µs | 44.70 µs | 45.17 µs | 53.14 µs | 71.12 µs | 22,139.6 QPS |
| update-lit (literal) | B | 3,000 | 38.81 µs | 45.31 µs | 45.81 µs | 52.88 µs | 70.08 µs | 21,829.8 QPS |
| update-lit (prepared) | A | 3,000 | 35.12 µs | 41.70 µs | 42.16 µs | 52.27 µs | 68.10 µs | 23,716.9 QPS |
| update-lit (prepared) | B | 3,000 | 34.51 µs | 42.33 µs | 42.74 µs | 50.28 µs | 68.14 µs | 23,395.1 QPS |
| update-range (literal) | A | 60 | 6.56 ms | 6.65 ms | 6.72 ms | 7.99 ms | 109 ms | 148.8 QPS |
| update-range (literal) | B | 60 | 6.38 ms | 6.51 ms | 6.62 ms | 7.43 ms | 1.02 s | 151.1 QPS |
| update-range (prepared) | A | 60 | 6.50 ms | 6.61 ms | 6.67 ms | 7.75 ms | 18.6 ms | 150.0 QPS |
| update-range (prepared) | B | 60 | 6.37 ms | 6.47 ms | 6.54 ms | 7.50 ms | 1.05 s | 152.9 QPS |

The `update-range` percentiles are over 60 statements: p90 is the 54th and p99 the 60th, i.e. the maximum, so only p0-p50 describe the typical statement there.

**The point shapes' wall time is where the CPU says**: p50 within 0.4-0.7 µs on every side (+0.56, +0.41, +0.64, +0.58
µs for the four shapes, B higher), p99 inside the spread of the runs. A point statement is the two processes' CPU and
nothing else:

**What a point statement is made of (wall mean per statement, the server's and the client's CPU, median of the three runs)**

| shape (driver path) | binary | wall, mean | server CPU | client CPU | server + client CPU - wall | share of the wall that is the two processes' CPU |
|---|---|---|---|---|---|---|
| select-pk (literal) | A | 58.03 µs | 26.08 µs | 32.37 µs | +0.42 µs | 101 % |
| select-pk (literal) | B | 58.26 µs | 26.16 µs | 32.41 µs | +0.31 µs | 101 % |
| select-pk (prepared) | A | 55.07 µs | 25.78 µs | 29.96 µs | +0.67 µs | 101 % |
| select-pk (prepared) | B | 54.81 µs | 25.43 µs | 30.13 µs | +0.76 µs | 101 % |
| update-lit (literal) | A | 47.99 µs | 23.97 µs | 24.79 µs | +0.76 µs | 102 % |
| update-lit (literal) | B | 48.36 µs | 24.66 µs | 24.76 µs | +1.05 µs | 102 % |
| update-lit (prepared) | A | 44.97 µs | 23.51 µs | 22.28 µs | +0.82 µs | 102 % |
| update-lit (prepared) | B | 45.50 µs | 23.46 µs | 22.39 µs | +0.35 µs | 101 % |

The wall mean is within 0.3-1.1 µs of the sum of the two CPUs on both binaries: there is **no idle wait in a point
statement** at `relaxed` (no fsync, no lock wait with one session), and no wait type to decompose beyond the server's
CPU and the client's.

**The tail is a different matter.** B's six `update-range` arms each contain one statement of 0.91-1.09 s (907, 1,089,
1,025, 1,080, 1,000 and 1,050 ms); A's six longest are 111, 109, 84, 196, 18.6 and 17.2 ms. The server's CPU for those
arms is 0.2-3.2 % lower in all six run-and-path means and the p50 1.5-1.9 % lower (median of three); the 60-statement
mean is 2 to 3 times higher on B because one second is 17 ms of a 60-statement mean. Four things limit what can be said:
the driver keeps percentiles, not samples, so the position of the event is not known; the server ran at `log_level =
warn`, so a segment roll's line (BQ-S1's `[wal] roll`) is not in any log; `bn_census` does not read `SHOW META` around
the arm, so no hold counter or checkpoint counter exists for it; and every arm of one binary is the same code, so six of
six is not six independent draws. **Whether this is B or the device is not decided by these files.** What can be said
is that a 0.9-1.1 s wait is the size of a segment-roll hold in BQ's results (hold p99 788 ms, maximum 1.21 s at
`relaxed`), that an arm of 60 every-row `UPDATE`s (600,000 row updates) writes at least one 64 MiB segment of log and
probably several (`[inferred]` from the row count and a record of 100 bytes or more; the log volume was not recorded),
and that B's change set contains a reclaim-ordering item (H1) and a commit-publication item (H2), the only items on
paths a wait of that kind could cross `[inferred]`, untested.

## 4. The commit loops

One session, `cores = 1`, a pk `INSERT` per statement committed under `group` or `strict`, 20 s after a 2 s warm-up.
The cell is **device-bound**: one `fsync` per statement, run on the reactor's thread, at about a millisecond.

**Commit loops (`ba_census.py --shape insert1`, one session, `cores = 1`, 20 s after a 2 s warm-up, one fresh server per cell); A and B adjacent in time, A first**

| durability | binary | statements per run | stmt/s r1 / r2 / r3 | median stmt/s | spread | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|---|---|---|
| group | A | 14,272 / 16,797 / 17,244 | 713.8 / 839.8 / 862.2 | 839.8 stmt/s | 148.4 stmt/s (17.7 %) | 857 µs | 982 µs | 1.02 ms | 1.94 ms | 4.95 ms | 24.2 ms |
| group | B | 16,047 / 16,061 / 14,599 | 797.1 / 803.0 / 729.9 | 797.1 stmt/s | 73.1 stmt/s (9.2 %) | 855 µs | 974 µs | 1.03 ms | 1.92 ms | 5.17 ms | 201 ms |
| strict | A | 16,587 / 16,906 / 17,268 | 829.3 / 845.3 / 863.4 | 845.3 stmt/s | 34.1 stmt/s (4.0 %) | 846 µs | 958 µs | 995 µs | 1.75 ms | 4.94 ms | 136 ms |
| strict | B | 16,570 / 16,794 / 16,717 | 828.5 / 839.8 / 835.8 | 835.8 stmt/s | 11.3 stmt/s (1.4 %) | 862 µs | 970 µs | 1.01 ms | 1.95 ms | 5.16 ms | 99.6 ms |

**B against A, commit loops (median of three runs; paired by run in the last column)**

| durability | stmt/s A | stmt/s B | B vs A | paired by run: B vs A, r1 / r2 / r3 | p50 A | p50 B |
|---|---|---|---|---|---|---|
| group | 839.8 stmt/s | 797.1 stmt/s | -5.1 % | +11.7 % / -4.4 % / -15.3 % | 1.02 ms | 1.03 ms |
| strict | 845.3 stmt/s | 835.8 stmt/s | -1.1 % | -0.1 % / -0.7 % / -3.2 % | 995 µs | 1.01 ms |

The runs are noisy before they are different: A's three `group` runs are 713.8, 839.8 and 862.2 stmt/s (r1 ran at a
15-minute load of 8.9, 14:21), and the paired deltas change sign (+11.7 / -4.4 / -15.3 %). `strict` is tighter
(4.0 % and 1.4 % spread) and its paired deltas are all negative and small (-0.1 / -0.7 / -3.2 %), smaller than A's own
spread of 4.0 %. **Neither durability's throughput is a finding.**

The statement is its fsync:

**What a commit statement is made of (per statement, mean of the three runs of each cell)**

| durability | binary | syncs per statement | of which on the reactor thread (inline) | time inside the reactor's foreground polls per statement | `SHOW META` foreground CPU delta per statement |
|---|---|---|---|---|---|
| group | A | 1.001 | 1.000 | 28 µs | +27.3 µs / +29.5 µs / +25.9 µs (r1 / r2 / r3) |
| group | B | 1.000 | 1.000 | 29 µs | +31.7 µs / +29.7 µs / +27.1 µs (r1 / r2 / r3) |
| strict | A | 1.000 | 1.000 | 1,130 µs | -0.1 µs / +4.7 µs / -17.6 µs (r1 / r2 / r3) |
| strict | B | 1.000 | 1.000 | 1,149 µs | -18.5 µs / +1.8 µs / -14.7 µs (r1 / r2 / r3) |

Each statement does one sync (`wal_syncs` per statement 1.000-1.001), and it runs inline on the reactor thread
(`contention_syncs_inline` 1.000). A `strict` statement spends 1,130-1,149 µs inside the reactor's foreground poll, the
whole statement; at `group` the poll holds 28-29 µs and the sync runs outside it. The statement's p0 is 826-983 µs on
both sides, so at least about 0.83 ms of the 1.0 ms p50 is the device `[inferred]`, with a server share of the order of
the point `UPDATE`'s 24 µs plus a client's 22 µs round trip. **The `SHOW META` foreground-CPU delta in the last
column is unusable**: it is negative or near zero at `strict` (-18.5 to +4.7 µs per statement, the cause not traced),
and at `group` it reads 26-32 µs, which cannot be compared with the `strict` value for the same work; no server CPU
per commit is taken from it. The commit loops therefore have no server-CPU figure, only throughput
and latency.

**The `group` pause.** B's three `group` runs each carry one pause of 142, 217 and 201 ms in the reactor's
`sched_hold_out_run_longest_us` (the reactor was outside its run loop that long), and 1, 2 and 16 polls the reactor
did not make for over 100 ms (`sched_unpolled_over_100ms`); A's three: 26, 24 and 23 ms and none. `strict`'s
out-of-run holds are 26-244 ms on both sides. A `group` sync runs on the calling thread at one core
(the work order's H2 card, citing `src/wal/manager.cpp:186-188`), so a pause of this size is what a slow `fdatasync` looks like from the reactor, and
the same shape of event as BQ's slow writer sync `[inferred]`. A is always the first cell of a pair and B the second, so
B follows A's shutdown; whether that order produces it was not tested (the `strict` pair, in the same order, does not
show it).

## 5. What H1-H4 removed at `cores = 1`, and what this A/B could have seen of it

| Item | What a one-core instance stopped paying | Reached by | Reached by this A/B? |
|---|---|---|---|
| **H1** `StatementEpochs` | the full barrier that `Enter` issued per statement, and the one in `AllAtLeast` that the reclaim tick issued (they are taken only when `shared()`, i.e. `cores > 1`) | every statement's scope | every shape, once per statement |
| **H2** the window latch | a `std::mutex` taken per above-floor visibility decision (`LookupCommit`) and per commit (`BeginCommit`, `PublishCommit`, and the `Reclaim` inside it); armed only above one core | an autocommit write takes it per commit; a point read only for a version above the floor | the `UPDATE` shapes per statement; the `SELECT` shape rarely |
| **H3** the Cabin store's latches | the partition and stats latches of `CabinStore` (null when unshared) | a statement the Cabin observes or serves, the FK check's serve, the write hook, `CABIN AUTO` | **no cell**: the Cabin-served `SELECT` was not run, and nothing here shows a Cabin being consulted |
| **H4** `MintCheckView`'s walk of `live_` | a scan of this core's active transactions at each mint, redundant with `AnyUnresolved()` | one mint per statement | every shape, once per statement; the scan is over zero or one entry here |
| **H5** the page stream stamp (in B) | a thread-local read and a two-byte store per logged mutation; the envelope's core byte | each logged mutation | the `UPDATE` shapes (a few records) and above all the every-row one (about 10,000) |

Each of these is a per-statement operation of the order of tens of nanoseconds (one uncontended lock/unlock pair, one
fence, a short scan); none was timed in isolation, and **this A/B did not time any of them**. The statement it measures
is 23-27 µs with a floor of 1-2 µs between runs. What the data support is a bound, not an estimate: the five items
and the counter commit together moved a point statement's server CPU by between -0.56 and +0.67 µs at the median
(-2.2 to +2.9 %), with an interval over runs of +/- 1.35-2.72 µs. H1-H4 are cut on proof (BP-R1), and **this A/B
neither confirms nor refutes that they bought anything measurable at one core**.

## 6. Host, noise floor and defects of this run

**Host per cell** (`bn` cells: the driver's `host_before` / `host_after`; commit cells: the same, from `ba_census`; both list competing `cc1plus|cmake --build|ctest` processes, empty in every cell)

| cell | start (UTC) | load before (1 / 5 / 15 min) | load after | competing processes | quiet gate waited | `df -T` |
|---|---|---|---|---|---|---|
| bn r1 (12 blocks x 6 arms x 2 binaries) | 14:21:13 | 1.52 / 6.43 / 9.16 | 1.92 / 6.35 / 9.11 | none | 0 s | ext4 |
| bn r2 (12 blocks x 6 arms x 2 binaries) | 19:10:04 | 0.93 / 1.93 / 2.45 | 1.10 / 1.94 / 2.45 | none | 0 s | ext4 |
| bn r3 (12 blocks x 6 arms x 2 binaries) | 19:10:16 | 1.10 / 1.94 / 2.45 | 1.22 / 1.92 / 2.44 | none | 0 s | ext4 |
| commit group A r1 | 14:21:46 | 1.37 / 5.94 / 8.91 | 1.33 / 5.56 / 8.70 | none | 0 s | ext4 |
| commit group B r1 | 14:22:11 | 1.33 / 5.56 / 8.70 | 2.06 / 5.37 / 8.56 | none | 0 s | ext4 |
| commit strict A r1 | 14:26:38 | 1.57 / 4.17 / 7.39 | 1.51 / 3.94 / 7.23 | none | 0 s | ext4 |
| commit strict B r1 | 14:27:04 | 1.51 / 3.94 / 7.23 | 1.41 / 3.72 / 7.07 | none | 0 s | ext4 |
| commit group A r2 | 14:27:30 | 1.41 / 3.72 / 7.07 | 1.58 / 3.57 / 6.93 | none | 0 s | ext4 |
| commit group B r2 | 14:28:05 | 1.48 / 3.48 / 6.86 | 1.42 / 3.30 / 6.71 | none | 0 s | ext4 |
| commit strict A r2 | 14:28:31 | 1.42 / 3.30 / 6.71 | 1.30 / 3.11 / 6.56 | none | 0 s | ext4 |
| commit strict B r2 | 14:28:58 | 1.30 / 3.11 / 6.56 | 1.32 / 2.94 / 6.39 | none | 0 s | ext4 |
| commit group A r3 | 14:29:23 | 1.32 / 2.94 / 6.39 | 1.45 / 2.84 / 6.26 | none | 0 s | ext4 |
| commit group B r3 | 14:30:09 | 1.29 / 2.72 / 6.15 | 1.45 / 2.64 / 6.03 | none | 0 s | ext4 |
| commit strict A r3 | 14:30:34 | 1.45 / 2.64 / 6.03 | 1.31 / 2.51 / 5.90 | none | 0 s | ext4 |
| commit strict B r3 | 14:31:00 | 1.31 / 2.51 / 5.90 | 1.27 / 2.40 / 5.77 | none | 0 s | ext4 |

**Controls: the client's own on-CPU time per statement (the driver thread, identical work on both sides) and the server's idle burn**

| shape (driver path) | driver CPU, A r1 / r2 / r3 | driver CPU, B r1 / r2 / r3 | B - A, median |
|---|---|---|---|
| select-pk (literal) | 32.28 µs / 32.37 µs / 32.87 µs | 32.37 µs / 32.41 µs / 32.76 µs | +0.04 µs |
| select-pk (prepared) | 29.96 µs / 29.77 µs / 30.10 µs | 30.13 µs / 29.75 µs / 31.33 µs | +0.18 µs |
| update-lit (literal) | 25.17 µs / 24.79 µs / 24.69 µs | 25.14 µs / 24.76 µs / 24.60 µs | -0.03 µs |
| update-lit (prepared) | 22.70 µs / 22.28 µs / 22.27 µs | 22.87 µs / 22.39 µs / 22.19 µs | +0.11 µs |
| update-range (literal) | 102.86 µs / 112.64 µs / 103.99 µs | 106.49 µs / 118.65 µs / 115.87 µs | +6.01 µs |
| update-range (prepared) | 95.18 µs / 107.06 µs / 97.64 µs | 98.43 µs / 102.25 µs / 93.64 µs | -3.99 µs |

| run | idle CPU of the server per second, A | B | B - A |
|---|---|---|---|
| r1 | 4,494 µs/s | 4,859 µs/s | +365 µs/s (+8.1 %) |
| r2 | 4,879 µs/s | 4,649 µs/s | -230 µs/s (-4.7 %) |
| r3 | 4,824 µs/s | 4,917 µs/s | +94 µs/s (+1.9 %) |

- **Two periods.** r1 ran at 14:21 with a one-minute load of 1.52 and **five- and fifteen-minute loads of 6.4 and 9.2**
  (the builds and BP's earlier cells had just ended); the quiet gate reads the one-minute average only (limit 1.6).
  r2 and r3 ran at 19:10 at 1.9 / 2.5. r1 is the run with B below A in all four point shapes. Whether the host period
  caused that is not separable: r3, in the quiet period, is mixed.
- **The commit loops** ran 14:21-14:31 with fifteen-minute loads falling from 8.9 to 5.8; the competing-process lists
  (`cc1plus`, `cmake --build`, `ctest`) are empty in every cell. The gate does not see I/O pressure
  (`bench/README.md`'s fourth rule asks for the load and the process list, and has no I/O term).
- **Three runs, not eight** (§6.1 of the work order); `bn_census` interleaves in 12 blocks, the commit loops are
  adjacent pairs with A first. The block interleave is the control for the host's drift; the run-to-run offset it does
  not control is the finding of §2.
- **The client's CPU is the control that cannot be affected**: it is identical work on both sides and moves by +0.04,
  +0.18, -0.03 and +0.11 µs at the median in the four point shapes (against server deltas of -0.56 to +0.67 µs), which
  places the harness's own noise at about 0.1-0.2 µs and the server-process offset between runs, not the harness, at
  the 1 µs level. The server's idle burn is 4.5-4.9 ms of CPU per second on both sides (+8.1, -4.7, +1.9 %): at 55 µs a
  statement that is 0.25 µs of it, below every delta here.
- **The `update-range` and `group` tails** are unexplained (§3, §4).
- **No driver change inside the measurement affects a figure** (see the stamp table).

## 7. What the run teaches about the engine

1. **At one core the engine's point statement is its request path.** Server CPU 22.5-27.1 µs and client CPU 22-33
   µs add up to the whole of the 45-58 µs wall time (101-102 %); the per-statement fences, latches and scans that BP-G1
   cut were never a visible share of it, and cutting them did not move it past the 1-2 µs resolution. That is
   consistent with BN's close, where a point `UPDATE`'s own storage work was 2.3 µs of 24.2 µs. It does not argue for or
   against BP-R1's proof-based rule, which was never a cost argument.
2. **The instrument's limit, not the engine's, is the finding of the CPU rows.** Two servers on one CPU, only one
   busy, have a run-level offset of up to 1 µs a statement between them, with a sign that changes between runs 12
   seconds apart. A change of a few tens of nanoseconds a statement needs either many more runs (the interval shrinks
   as 1/sqrt(n) over runs, not blocks) or a profile (`tools/kds_sampler.c`) at the item rather than a delta over the
   statement. BN reached the same floor (about 1 µs) and the same remedy (a control).
3. **A per-row item is the only one this shape could see.** Whether the every-row `UPDATE`'s -1.3 to -1.8 % is H5's is
   `[inferred]` from the shape of the item (it is the only one of five that scales with rows), not shown. A profile of
   the every-row `UPDATE` on B against A is the cell that would show it.
4. **B's tails need a decision, not an interpretation.** A 1.0 s statement in six of six `update-range` arms and a
   0.14-0.22 s reactor pause in three of three `group` loops, with A at 0 of 6 and 0 of 3, are the only places where
   B's raw numbers are worse than A's in a consistent direction. The re-run that would decide it is named in
   §8; until then the BP-S3 A/B cannot be read as "nothing moved".

## 8. What this file does not establish

- **H3.** The Cabin-served `SELECT` was not run; nothing here exercises the Cabin store's latches.
- **The cause of B's `update-range` and `group` tails.** The cells that would decide it: `update-range` with
  `log_level = info` (the `[wal] roll` lines with timestamps) and per-statement samples, A and B, enough runs to tell
  six of six from chance; the `group` loop with the order of A and B swapped.
- **The 200 and 1,000-row sizes**, eight runs, and any server CPU for a commit.
- **Which of H1-H5 any movement belongs to.** No cell isolates one; they landed together (`230c4453`, `998903ab`,
  `ccf9cfc1`).
- **The close's A/B** (BP-R6: A at BP's opening commit against B at its close, interleaved, over §6.1's cells). This is
  the BP-G1 stage's own report.

## Appendix A. Every run

**Appendix A1. Every `bn_census` arm: wall-clock latency, QPS (derived), server and client CPU**

| run | shape (driver path) | bin | statements | p0 | p25 | p50 | p90 | p99 | mean | QPS from mean (derived) | server CPU | client CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| r1 | select-pk (literal) | A | 3,000 | 53.88 µs | 55.20 µs | 55.79 µs | 62.25 µs | 73.76 µs | 58.03 µs | 17,231.9 QPS | 26.08 µs | 32.28 µs |
| r1 | select-pk (literal) | B | 3,000 | 51.97 µs | 54.19 µs | 54.79 µs | 61.73 µs | 76.16 µs | 57.24 µs | 17,470.7 QPS | 25.20 µs | 32.37 µs |
| r1 | select-pk (prepared) | A | 3,000 | 50.69 µs | 52.07 µs | 52.70 µs | 60.30 µs | 73.78 µs | 55.07 µs | 18,159.1 QPS | 25.78 µs | 29.96 µs |
| r1 | select-pk (prepared) | B | 3,000 | 49.12 µs | 51.10 µs | 51.72 µs | 60.72 µs | 102 µs | 54.81 µs | 18,245.8 QPS | 25.22 µs | 30.13 µs |
| r1 | update-lit (literal) | A | 3,000 | 37.59 µs | 45.64 µs | 46.18 µs | 55.32 µs | 75.84 µs | 49.03 µs | 20,397.5 QPS | 24.93 µs | 25.17 µs |
| r1 | update-lit (literal) | B | 3,000 | 36.83 µs | 44.89 µs | 45.46 µs | 56.63 µs | 90.09 µs | 48.90 µs | 20,448.3 QPS | 24.67 µs | 25.14 µs |
| r1 | update-lit (prepared) | A | 3,000 | 36.70 µs | 42.77 µs | 43.24 µs | 52.34 µs | 68.10 µs | 46.02 µs | 21,729.3 QPS | 24.22 µs | 22.70 µs |
| r1 | update-lit (prepared) | B | 3,000 | 34.17 µs | 42.16 µs | 42.61 µs | 50.28 µs | 75.99 µs | 46.06 µs | 21,713.0 QPS | 23.46 µs | 22.87 µs |
| r1 | update-range (literal) | A | 60 | 6.61 ms | 6.73 ms | 6.84 ms | 7.99 ms | 111 ms | 9.57 ms | 104.5 QPS | 7,055.43 µs | 102.86 µs |
| r1 | update-range (literal) | B | 60 | 6.37 ms | 6.51 ms | 6.62 ms | 7.43 ms | 907 ms | 25.9 ms | 38.6 QPS | 6,865.59 µs | 106.49 µs |
| r1 | update-range (prepared) | A | 60 | 6.61 ms | 6.74 ms | 6.79 ms | 10.8 ms | 196 ms | 13.0 ms | 76.6 QPS | 7,203.56 µs | 95.18 µs |
| r1 | update-range (prepared) | B | 60 | 6.34 ms | 6.47 ms | 6.54 ms | 7.50 ms | 1.08 s | 24.9 ms | 40.2 QPS | 6,974.74 µs | 98.43 µs |
| r2 | select-pk (literal) | A | 3,000 | 52.84 µs | 54.46 µs | 55.05 µs | 60.97 µs | 72.41 µs | 57.36 µs | 17,435.1 QPS | 25.18 µs | 32.37 µs |
| r2 | select-pk (literal) | B | 3,000 | 53.98 µs | 55.29 µs | 55.86 µs | 62.44 µs | 73.63 µs | 58.26 µs | 17,165.5 QPS | 26.16 µs | 32.41 µs |
| r2 | select-pk (prepared) | A | 3,000 | 47.89 µs | 51.16 µs | 51.64 µs | 56.71 µs | 68.63 µs | 53.66 µs | 18,634.5 QPS | 24.58 µs | 29.77 µs |
| r2 | select-pk (prepared) | B | 3,000 | 50.42 µs | 51.88 µs | 52.45 µs | 57.64 µs | 68.85 µs | 54.49 µs | 18,351.6 QPS | 25.43 µs | 29.75 µs |
| r2 | update-lit (literal) | A | 3,000 | 37.95 µs | 44.70 µs | 45.17 µs | 50.60 µs | 67.74 µs | 47.59 µs | 21,010.7 QPS | 23.46 µs | 24.79 µs |
| r2 | update-lit (literal) | B | 3,000 | 38.81 µs | 45.39 µs | 45.85 µs | 51.86 µs | 65.05 µs | 48.12 µs | 20,781.4 QPS | 24.13 µs | 24.76 µs |
| r2 | update-lit (prepared) | A | 3,000 | 35.12 µs | 41.40 µs | 41.83 µs | 46.96 µs | 64.79 µs | 44.10 µs | 22,674.8 QPS | 22.49 µs | 22.28 µs |
| r2 | update-lit (prepared) | B | 3,000 | 34.51 µs | 42.33 µs | 42.74 µs | 47.53 µs | 64.31 µs | 44.90 µs | 22,273.5 QPS | 23.34 µs | 22.39 µs |
| r2 | update-range (literal) | A | 60 | 6.54 ms | 6.65 ms | 6.70 ms | 7.42 ms | 109 ms | 8.88 ms | 112.6 QPS | 6,861.39 µs | 112.64 µs |
| r2 | update-range (literal) | B | 60 | 6.41 ms | 6.50 ms | 6.56 ms | 6.95 ms | 1.09 s | 25.0 ms | 40.0 QPS | 6,770.97 µs | 118.65 µs |
| r2 | update-range (prepared) | A | 60 | 6.50 ms | 6.60 ms | 6.66 ms | 7.20 ms | 18.6 ms | 7.27 ms | 137.6 QPS | 6,994.66 µs | 107.06 µs |
| r2 | update-range (prepared) | B | 60 | 6.37 ms | 6.47 ms | 6.54 ms | 7.11 ms | 1000 ms | 23.5 ms | 42.6 QPS | 6,894.64 µs | 102.25 µs |
| r3 | select-pk (literal) | A | 3,000 | 52.41 µs | 54.46 µs | 55.24 µs | 68.29 µs | 89.83 µs | 59.33 µs | 16,855.9 QPS | 26.98 µs | 32.87 µs |
| r3 | select-pk (literal) | B | 3,000 | 52.96 µs | 55.04 µs | 55.80 µs | 65.51 µs | 80.66 µs | 59.07 µs | 16,930.5 QPS | 26.83 µs | 32.76 µs |
| r3 | select-pk (prepared) | A | 3,000 | 49.70 µs | 51.28 µs | 52.04 µs | 65.73 µs | 92.70 µs | 57.82 µs | 17,293.7 QPS | 27.05 µs | 30.10 µs |
| r3 | select-pk (prepared) | B | 3,000 | 49.61 µs | 51.71 µs | 52.45 µs | 64.97 µs | 80.28 µs | 56.42 µs | 17,722.7 QPS | 25.74 µs | 31.33 µs |
| r3 | update-lit (literal) | A | 3,000 | 37.67 µs | 44.63 µs | 45.07 µs | 53.14 µs | 71.12 µs | 47.99 µs | 20,835.8 QPS | 23.97 µs | 24.69 µs |
| r3 | update-lit (literal) | B | 3,000 | 39.11 µs | 45.31 µs | 45.81 µs | 52.88 µs | 70.08 µs | 48.36 µs | 20,677.9 QPS | 24.66 µs | 24.60 µs |
| r3 | update-lit (prepared) | A | 3,000 | 34.24 µs | 41.70 µs | 42.16 µs | 52.27 µs | 68.65 µs | 44.97 µs | 22,236.8 QPS | 23.51 µs | 22.27 µs |
| r3 | update-lit (prepared) | B | 3,000 | 37.57 µs | 42.35 µs | 42.85 µs | 51.17 µs | 68.14 µs | 45.50 µs | 21,977.2 QPS | 24.05 µs | 22.19 µs |
| r3 | update-range (literal) | A | 60 | 6.56 ms | 6.64 ms | 6.72 ms | 8.03 ms | 84.3 ms | 9.37 ms | 106.7 QPS | 6,976.96 µs | 103.99 µs |
| r3 | update-range (literal) | B | 60 | 6.38 ms | 6.56 ms | 6.72 ms | 7.52 ms | 1.02 s | 24.1 ms | 41.4 QPS | 6,963.29 µs | 115.87 µs |
| r3 | update-range (prepared) | A | 60 | 6.48 ms | 6.61 ms | 6.67 ms | 7.75 ms | 17.2 ms | 7.32 ms | 136.6 QPS | 7,074.48 µs | 97.64 µs |
| r3 | update-range (prepared) | B | 60 | 6.38 ms | 6.47 ms | 6.54 ms | 7.69 ms | 1.05 s | 24.4 ms | 41.1 QPS | 6,943.81 µs | 93.64 µs |

**Appendix A2. Every commit-loop run**

| durability | binary | run | statements | stmt/s | p0 | p25 | p50 | p95 | p99 | max | checkpoint runs in the window |
|---|---|---|---|---|---|---|---|---|---|---|---|
| group | A | r1 | 14,272 | 713.8 stmt/s | 983 µs | 1.16 ms | 1.22 ms | 2.14 ms | 5.38 ms | 26.1 ms | 4 |
| group | A | r2 | 16,797 | 839.8 stmt/s | 857 µs | 982 µs | 1.02 ms | 1.94 ms | 4.93 ms | 24.2 ms | 4 |
| group | A | r3 | 17,244 | 862.2 stmt/s | 847 µs | 957 µs | 992 µs | 1.78 ms | 4.95 ms | 23.3 ms | 4 |
| group | B | r1 | 16,047 | 797.1 stmt/s | 855 µs | 974 µs | 1.03 ms | 2.10 ms | 5.11 ms | 143 ms | 4 |
| group | B | r2 | 16,061 | 803.0 stmt/s | 869 µs | 990 µs | 1.03 ms | 1.92 ms | 5.17 ms | 218 ms | 3 |
| group | B | r3 | 14,599 | 729.9 stmt/s | 832 µs | 955 µs | 993 µs | 1.92 ms | 5.60 ms | 201 ms | 4 |
| strict | A | r1 | 16,587 | 829.3 stmt/s | 858 µs | 980 µs | 1.02 ms | 1.87 ms | 5.04 ms | 98.1 ms | 4 |
| strict | A | r2 | 16,906 | 845.3 stmt/s | 846 µs | 958 µs | 995 µs | 1.75 ms | 4.94 ms | 136 ms | 4 |
| strict | A | r3 | 17,268 | 863.4 stmt/s | 826 µs | 948 µs | 983 µs | 1.66 ms | 4.80 ms | 158 ms | 4 |
| strict | B | r1 | 16,570 | 828.5 stmt/s | 862 µs | 980 µs | 1.02 ms | 1.89 ms | 5.17 ms | 99.6 ms | 4 |
| strict | B | r2 | 16,794 | 839.8 stmt/s | 844 µs | 961 µs | 998 µs | 1.95 ms | 4.97 ms | 125 ms | 5 |
| strict | B | r3 | 16,717 | 835.8 stmt/s | 868 µs | 970 µs | 1.01 ms | 2.00 ms | 5.16 ms | 26.5 ms | 4 |
