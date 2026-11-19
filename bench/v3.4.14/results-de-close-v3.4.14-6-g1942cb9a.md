# DE close: a page read that leaves the reactor gives the never-miss sessions their resident latency back, and costs a resident point `SELECT` about 0.4 µs of server CPU

Measured at `v3.4.14-6-g1942cb9a` (arm B, DE-S2, the milestone's closing code
commit) against `v3.4.14` = `35086008` (arm A, the commit DE opened from),
interleaved. This is the milestone-close measurement of
`instructions/v3.2.0/workorder-de-page-read-off-the-reactor.md` (§3 DE-R1,
DE-R4, DE-R9; §4 DE-Q1's bar at the close and DE-Q14; §7's DE-S0, DE-S1 and
DE-S2 entries) and the "after" of
`bench/v3.4.13/results-de-s0-premise-v3.4.13-19-g35086008.md`.

**Findings.** (1) With the page cache kept from answering, a quarter-size pool
and four sessions missing on 77 % of their reads (cell iii), the never-miss
sessions' p95 from the schedule is 358.1 to 386.2 µs in B against 747.9 to
819.9 µs in A over four rounds, and their p99 is 411.5 to 448.2 µs against
1,009.0 to 1,022.1 µs (148,869.0 µs in one round, a 188 ms device read that
held A's core). B's (iii) is 1.01 to 1.03 times the same round's (ii) at p50,
p95 and p99; A's is 2.12 to 2.28 times at p95. (2) Beside a scanner (cell iv)
A's never-miss sessions complete 10,892 to 10,908 of the 20,004 reads due,
with a schedule p50 of 5.75 to 5.76 s; B's complete 20,003 or 20,004 with a
p50 of 282.8 to 309.0 µs, and the scanner reads 19,774 to 19,796 pages/s in
both arms bar one B round at 19,489. (3) No resident cell's p50 or p95 is slower
outside its two-round spread (vscan's are 1 to 2 % lower). **(4) The price is not zero, and it is on the
resident path:** a resident point `SELECT` costs 0.34 to 0.53 µs more server
CPU a statement in B at `cores = 1`, pooled over six server lifetimes, against
a same-binary floor of -0.06 to +0.02 µs (pooled over blocks); a resident whole-relation walk
(vscan) runs 7 to 9 % fewer slices per second at the same CPU; a cold read
costs 41 to 49 µs more CPU and its session 2 to 3.4 % more at p50.
The point write measured (`update-lit`) is 0.13 to 0.18 µs cheaper, inside
the floor of any one lifetime. The wait it removes is the core's; the work it
adds is a second run of the statement.

**B is not the closing code, and what the closing code measured is in section
13.** After this close was written, a knob spike on B (section 13) placed the
two resident costs in (4) and a remedy was applied: variant K5 (B plus three
always-on trims) runs `vscan` within about 1 % of A (37.30 to 37.64 M pool
hits in 20 s against A's 37.56 to 38.02 M, two runs each) and its resident
point `SELECT` costs +0.21 to +0.41 µs of server CPU over A, against B's +0.33
to +0.66 µs in the same spike. **K5 is not exactly the closing code** (the
review put one thread-local store a statement back after the spike, and that
tree was not re-measured), the point cost is not removed and is not separated
from DE's per-statement code or from binary layout, and what B's 8 % `vscan`
loss was is not isolated. Sections 1 to 12 are B's findings and stay as they
were measured.

## 1. Stamp

| | |
|---|---|
| Executed (UTC) | 2026-11-19. Part 1 (28 cells, rounds 0 and 1): 10:23:49 to 10:40:56. Part 2 (six server lifetimes): 10:42:16 to 10:45:44. Extra rounds 2 and 3 of cells iii and iv (8 runs): 10:46:19 to 10:51:03. About 28 minutes of measurement inside the milestone's hour; the builds (1 min 46 s, A 77 s and B 29 s incremental) were done before and are not counted. The quiet gate held nothing worth stating: the driver's `wait_s` is 0.0 s in every cell but one (iii4 B r1, 5.0 s), part 2's gate waited 12 s a lifetime over six cells |
| Branch / worktree | `worktree-de-page-read-off-the-reactor`, worktree `de-page-read-off-the-reactor` |
| Arm A | `35086008c6b35a1251f68256b96c67a569fd7dcd`, `git describe --tags` = `v3.4.14` (the commit DE opened from; it described as `v3.4.13-19-g35086008` when DE-S0 ran). Commit time 2026-11-19 07:59:41 UTC. Release build, copy `~/bench-runs/de/close/kds_server_A`, **sha256 `78b3aede97192d165094e5051a80958b026d36e2bb92d7a13a394412399d3fb3`** (equal in all 18 of A's cell JSONs) |
| Arm B | `1942cb9a58614502f12347f1a8631ba2f807e14e`, `git describe --tags` = **`v3.4.14-6-g1942cb9a`** (DE-S0's census and DE-S1, DE-S2 and their reviews; the diff from A under `src` and `include` is 27 files, +948 -26). Commit time 2026-11-19 10:20:10 UTC. Release build, copy `~/bench-runs/de/close/kds_server_B`, **sha256 `ca767bfb184298f219c3dca2cf8ed84c3ebeb7021535e5638a56e9a238d577c8`** (equal in all 18 of B's cell JSONs, and equal to `build-release/kds_server`'s hash, read at the end of the run; its mtime is 10:22:39 UTC, after B's commit time, so the binary is not older than HEAD) |
| Tree | the worktree's tracked state at the run: modified, not committed, `docs/rules/rules.md`, `docs/spec/client-manual.md`, `docs/spec/page.md`, `docs/spec/sched.md`, `docs/spec/wait-events.md`, `include/kds/sched/io_backend.hpp`, `tools/de_premise.py`. The two binaries were built from clean trees by the caller (stated to me; I did not rebuild or verify), so the modifications are the driver's and documents', and the run measured the hashes above. The binary copies' mtimes are the copy time (10:22:39 UTC), not the build time |
| Host | 8 logical CPUs = 4 cores x 2 threads (siblings 0/4, 1/5, 2/6, 3/7), Intel Xeon Platinum 8581C @ 2.10 GHz, Linux 6.12.116+deb13-cloud-amd64 |
| Data device | `/dev/nvme0n1p1`, ext4 (`data_device` in all 36 cell JSONs; `df -T` of `~/bench-runs/de/work` and of `~/bench-runs/de/p2` at the run). Not tmpfs. Data file `~/bench-runs/de/work/de.db`, 1,056,440,320 B = 128,960 pages |
| Ports | 32750 (part 1 and the extra rounds), 32760 to 32762 (part 2) |
| Build type | Release, both arms |
| Server configuration | part 1: `durability = relaxed`, `buffer_pool_frames` 32,768 (i, ii, iii, iv, iii4) or 160,000 (v, vscan), `cores = 1` with `reactor_cpus = 1` except iii4 (`cores = 4`, `reactor_cpus = 0,1,2,3`), `log_level = warn`, nothing else non-default. Part 2: `cores = 1`, `durability = relaxed`, bn_census's own configuration (below) |
| Drivers | `tools/de_premise.py` (the working-tree version, adjusted for this run) through the runner `archive/.../part2-and-runners/de_close.sh` (`--runs 1 --run-base <round> --cells <cell> --cold-rate 650 --probe-reads 200 --keep`, arms interleaved, order swapped every round); part 2 `tools/bn_census.py` imported unmodified through the wrapper `p2.py`, which only adds relation `x` with a secondary index and the shape `select-index`. Driver documentation is `bench/docs/` and the drivers' own docstrings; not repeated here |
| Raw output | `bench/v3.4.14/archive/de-close-v3.4.14-6-g1942cb9a/`: `de-<cell>-<arm>-r<round>.json` (36) and `.s.log` (36), `summary-<arm>-r<round>.json`, and `part2-and-runners/` (part 2's `p2.json` and `p2.log`, the runner scripts, the status and driver logs, the analysis scripts that generated every table below) |
| Baseline | **arm A measured again in this run, interleaved with B** (this engine's own last state for the same shapes). DE-S0's file measured A earlier (section 11 checks it as a consistency figure, not a second delta). PostgreSQL floor: not measured for this shape |

## 2. What was measured, and how it maps to the order

One relation `de_big` of 900,000 rows `(id int64, c0 int64, pad char(1000))`,
128,960 data-file pages, **loaded once at DE-S0 by arm A's binary and never
reloaded** (the data file's last write in this run is the extra rounds' cell
iv at 10:51); both arms mount it, every cell is read-only. The cells,
sessions, rates, pools and the page-cache control (`posix_fadvise(DONTNEED)`
plus a memory-limited systemd scope) are those of the DE-S0 file's section 2
and are not re-explained: hot = 4 never-miss sessions at 1,000 point reads/s
open-loop over keys 1..700; cold = 4 sessions uniform over the relation at
**650 reads/s in total, fixed for both arms** (DE-S0 used 648.1); scanner =
`SELECT id FROM de_big WHERE c0 = -1`; window 20 s after a 4 s warm-up;
pool 32,768 frames (25.4 % of the relation) or 160,000.

| cell | hot | cold or scanner | pool | OS page cache | cores |
|---|---|---|---|---|---|
| i | 4 sessions | none | 32,768 frames | warm | 1 |
| ii | 4 sessions | 4 cold | 32,768 frames | warm | 1 |
| iii | 4 sessions | 4 cold | 32,768 frames | kept from answering | 1 |
| iv | 4 sessions | 1 scanner | 32,768 frames | kept from answering | 1 |
| v | 4 sessions | 4 cold | 160,000 frames | warm | 1 |
| vscan | 4 sessions | 1 scanner | 160,000 frames | warm | 1 |
| iii4 | 4 sessions | 4 cold, `SET PRIORITY analytic` | 32,768 frames | kept from answering | 4 |

Clients are pinned: at `cores = 1` the server is on CPU 1 and the clients on
CPUs 2, 3, 6, 7; **in iii4 the clients are on CPUs 4 to 7 (the reactors'
siblings), where DE-S0's iii4 had them on 2, 3, 6, 7, so this iii4 is not
comparable with DE-S0's** (only arm against arm within this run is read). iii4
has no `cores = 4` resident baseline.

**Rounds.** Rounds 0 and 1 cover all seven cells for both arms, A first in
round 0 and B first in round 1. **Cells iii and iv have two further rounds
(2 and 3)**, added after round 0's A iii run met a 188 ms device read (section
4) and kept in the file for both arms; rounds 2 and 3 have no (ii) beside them,
so every difference against (ii) uses rounds 0 and 1. Rule 9's row-count sweep
(200, 1K, 10K) does not apply to part 1: the relation is one 900,000-row size
by design, and the cells' sessions read by tuple key, which does not scale
with rows. Part 2 ran at `--rows 1000` only (not swept; a point statement by
key or index is a fixed cost, and its cost per statement is the quantity).

## 3. The host while it ran

Every cell's `host_before`/`host_after`: **`competing` (the driver's
`pgrep -a -f "cc1plus|cmake --build|ctest"`) was empty before and after all 36
cells; the 1-minute load average before a cell was 0.40 to 1.43 and after one
0.44 to 1.51** (the highest, 1.51 after vscan A r1, is a post-cell figure that
includes that cell's own server and clients; the driver's gate is on the
before figure, 1.5). No cell was tainted by the criterion given (a competing
process or a load above 1.5 before it), so none was re-run on that account.
**One cell was disturbed by the device, not the host**: A iii r0 (section 4).

| cell | round | arm | 1-min load before | 1-min load after | `wait_s` | competing before | competing after |
|---|---|---|---|---|---|---|---|
| i | r0 | A | 1.43 | 0.97 | 0.0 s | none | none |
| i | r0 | B | 0.97 | 0.80 | 0.0 s | none | none |
| i | r1 | A | 1.07 | 0.75 | 0.0 s | none | none |
| i | r1 | B | 1.00 | 1.07 | 0.0 s | none | none |
| ii | r0 | A | 0.80 | 0.55 | 0.0 s | none | none |
| ii | r0 | B | 0.55 | 0.61 | 0.0 s | none | none |
| ii | r1 | A | 0.56 | 0.44 | 0.0 s | none | none |
| ii | r1 | B | 0.75 | 0.52 | 0.0 s | none | none |
| iii | r0 | A | 1.12 | 1.00 | 0.0 s | none | none |
| iii | r0 | B | 1.00 | 0.66 | 0.0 s | none | none |
| iii | r1 | A | 0.77 | 0.91 | 0.0 s | none | none |
| iii | r1 | B | 0.44 | 0.77 | 0.0 s | none | none |
| iii | r2 | A | 0.40 | 1.04 | 0.0 s | none | none |
| iii | r2 | B | 1.04 | 0.94 | 0.0 s | none | none |
| iii | r3 | A | 0.83 | 0.81 | 0.0 s | none | none |
| iii | r3 | B | 0.98 | 0.83 | 0.0 s | none | none |
| iv | r0 | A | 0.66 | 0.83 | 0.0 s | none | none |
| iv | r0 | B | 0.83 | 0.97 | 0.0 s | none | none |
| iv | r1 | A | 1.43 | 1.24 | 0.0 s | none | none |
| iv | r1 | B | 0.91 | 1.43 | 0.0 s | none | none |
| iv | r2 | A | 0.94 | 0.97 | 0.0 s | none | none |
| iv | r2 | B | 0.97 | 0.98 | 0.0 s | none | none |
| iv | r3 | A | 0.84 | 0.99 | 0.0 s | none | none |
| iv | r3 | B | 0.81 | 0.84 | 0.0 s | none | none |
| v | r0 | A | 0.97 | 0.95 | 0.0 s | none | none |
| v | r0 | B | 0.95 | 0.72 | 0.0 s | none | none |
| v | r1 | A | 0.87 | 1.01 | 0.0 s | none | none |
| v | r1 | B | 1.24 | 0.85 | 0.0 s | none | none |
| vscan | r0 | A | 0.72 | 1.04 | 0.0 s | none | none |
| vscan | r0 | B | 1.04 | 1.18 | 0.0 s | none | none |
| vscan | r1 | A | 1.15 | 1.51 | 0.0 s | none | none |
| vscan | r1 | B | 1.01 | 1.15 | 0.0 s | none | none |
| iii4 | r0 | A | 1.18 | 0.83 | 0.0 s | none | none |
| iii4 | r0 | B | 0.83 | 1.00 | 0.0 s | none | none |
| iii4 | r1 | A | 1.17 | 1.07 | 0.0 s | none | none |
| iii4 | r1 | B | 1.39 | 1.17 | 5.0 s | none | none |


## 4. The finding: the never-miss sessions in iii and iv

Hot sessions, from the schedule's due instant (what an open-loop client sees,
queueing included) and from the send (service time). A's iv schedule values
are in seconds, written in ms. `n` is the reads completed in the 20 s window of
the 20,004 due.

| cell | round | arm | p50 (schedule) | p95 (schedule) | p99 (schedule) | p50 (service) | p95 (service) | p99 (service) | n completed of 20,004 due |
|---|---|---|---|---|---|---|---|---|---|
| iii | r0 | A | 272.5 µs | 819.9 µs | 148,869.0 µs | 184.1 µs | 699.3 µs | 1,408.8 µs | 20,004 ops |
| iii | r0 | B | 247.7 µs | 360.3 µs | 416.3 µs | 159.0 µs | 262.3 µs | 308.0 µs | 20,004 ops |
| iii | r1 | A | 272.5 µs | 747.9 µs | 1,022.1 µs | 181.2 µs | 655.2 µs | 900.3 µs | 20,004 ops |
| iii | r1 | B | 244.4 µs | 358.1 µs | 411.5 µs | 157.1 µs | 260.6 µs | 303.7 µs | 20,004 ops |
| iii | r2 | A | 296.6 µs | 775.6 µs | 1,009.0 µs | 205.8 µs | 682.1 µs | 909.6 µs | 20,004 ops |
| iii | r2 | B | 270.8 µs | 386.2 µs | 448.2 µs | 178.6 µs | 286.1 µs | 328.8 µs | 20,003 ops |
| iii | r3 | A | 299.7 µs | 782.0 µs | 1,011.1 µs | 208.6 µs | 686.6 µs | 904.8 µs | 20,004 ops |
| iii | r3 | B | 270.6 µs | 382.4 µs | 432.7 µs | 178.1 µs | 283.7 µs | 317.8 µs | 20,003 ops |
| iv | r0 | A | 5,759.16 ms | 8,772.27 ms | 9,042.31 ms | 6.44 ms | 7.53 ms | 7.82 ms | 10,895 ops |
| iv | r0 | B | 282.8 µs | 455.9 µs | 587.7 µs | 193.4 µs | 363.1 µs | 479.9 µs | 20,003 ops |
| iv | r1 | A | 5,745.94 ms | 8,763.26 ms | 9,032.59 ms | 6.43 ms | 7.55 ms | 7.99 ms | 10,908 ops |
| iv | r1 | B | 284.2 µs | 459.9 µs | 605.8 µs | 194.3 µs | 360.3 µs | 473.5 µs | 20,004 ops |
| iv | r2 | A | 5,751.17 ms | 8,769.88 ms | 9,038.98 ms | 6.43 ms | 7.54 ms | 7.82 ms | 10,899 ops |
| iv | r2 | B | 302.3 µs | 469.0 µs | 587.0 µs | 211.7 µs | 371.9 µs | 475.6 µs | 20,004 ops |
| iv | r3 | A | 5,763.44 ms | 8,777.70 ms | 9,047.03 ms | 6.44 ms | 7.54 ms | 7.81 ms | 10,892 ops |
| iv | r3 | B | 309.0 µs | 484.0 µs | 622.5 µs | 216.0 µs | 383.9 µs | 499.5 µs | 20,003 ops |


**What it says.** In (iii) B's never-miss sessions sit where the resident
cells put them (section 5): p95 of 358.1 to 386.2 µs from the schedule
against A's 747.9 to 819.9 µs, p99 of 411.5 to 448.2 µs against 1,009.0 to
1,022.1 µs, and in service p95 260.6 to 286.1 µs against 655.2 to 699.3 µs.
Both arms' rounds 2 and 3 run 7 to 11 % higher at p50 than rounds 0 and
1 (A 296.6 and 299.7 µs against 272.5, B 270.8 and 270.6 against 247.7 and
244.4; iv's B 302.3 and 309.0 against 282.8 and 284.2), a drift of the host
or the device between 10:30 and 10:50 that the load average does not show, so
**arms are compared inside a round, never across rounds**; inside every round
B's p50 is 8.7 to 10.3 % below A's in (iii) (derived: 9.1, 10.3, 8.7, 9.7 %), where the resident cells' p50 does not move between arms (section 5.1), so it is the stalled core's queueing and not a faster engine. In (iv), where A's schedule runs 5.75 s
behind and 45 % of the due reads are never sent, B's sessions complete every
read due at 283 to 309 µs p50, 456 to 484 µs p95 and 587.0 to 622.5 µs p99.
The (iv) hot sessions against the resident ceiling (vscan, B: p50 275.6 and
278.0 µs, p95 346.7 and 347.1 µs, p99 381.7 and 386.2 µs) are, in rounds 0 and 1,
1.7 and 3.1 % higher at p50, 31.5 and 32.5 % at p95 and 54.0 and 56.9 % at p99
(derived): the stall is gone, and a tail of about 0.1 to 0.2 ms is left, which
the scanner's 64-page slices between its parks are the candidate for (not
isolated).

**The disturbed cell.** A's iii r0 has p99 of 148,869.0 µs from the schedule
and 1,408.8 µs in service: one `io.page_read` of 188,087 µs (`max_us_since_start`
in the window's wait statistics) held A's single core for 188 ms, the roughly
190 reads due in that time queued behind it, and the schedule's p99 carries them.
The host's load was 1.12 and no competing process ran. The device produces
such reads in both arms (B iii r1: 312,003 µs, r2: 281,485 µs; and in iv
74,098 to 290,287 µs), and what differs is what they hold: B's hot sessions
never queue behind one. (The longest `sched.run_queue` wait since the server
started, which includes the pre-warm, is 368 to 1,369 µs in B's four iii
windows and 3,117 to 144,314 µs in A's; indicative only.) Rounds 1 to 3 of A
are the ordinary A.
The row stays in every table; A's r0 p99 is not used in any ratio below.

## 5. DE-Q1 (a)'s close bar, applied literally

### 5.1 Bar (1): the resident cells no slower in B outside their spread (DE-R4)

Two rounds per arm give a two-point range per arm, not an interval. Table: for
every resident cell (i, ii where the page cache answers, v, vscan) and iii4,
the never-miss sessions' p50, p95 and p99 in both arms and both rounds. "Ranges
overlap" is true when the two arms' two-point ranges share a value. The
percentiles that carry a claim here are p50 and p95: they move 0.4 to 7 µs
between two rounds of one arm. **p99 and above do not**: cell i's p99 from the
schedule moves 108.0 µs in A (517.1 and 409.1 µs) and 122.3 µs in B between
two runs, and DE-S0 saw the same cell's p99 move up to 2.1x.

| cell | basis | percentile | A r0 | A r1 | B r0 | B r1 | mean B vs mean A | ranges overlap |
|---|---|---|---|---|---|---|---|---|
| i | schedule | p50 | 262.0 µs | 257.0 µs | 258.9 µs | 259.6 µs | -0.1 % (-0.2 µs) | yes |
| i | schedule | p95 | 368.4 µs | 361.7 µs | 365.3 µs | 361.9 µs | -0.4 % (-1.4 µs) | yes |
| i | schedule | p99 | 517.1 µs | 409.1 µs | 525.5 µs | 403.2 µs | +0.3 % (+1.2 µs) | yes |
| i | service | p50 | 171.7 µs | 167.4 µs | 170.7 µs | 169.8 µs | +0.4 % (+0.7 µs) | yes |
| i | service | p95 | 266.1 µs | 262.5 µs | 264.9 µs | 263.0 µs | -0.1 % (-0.4 µs) | yes |
| i | service | p99 | 302.8 µs | 301.1 µs | 309.5 µs | 299.0 µs | +0.8 % (+2.3 µs) | yes |
| ii | schedule | p50 | 245.1 µs | 238.9 µs | 243.9 µs | 240.3 µs | +0.0 % (+0.1 µs) | yes |
| ii | schedule | p95 | 359.3 µs | 352.9 µs | 355.7 µs | 353.3 µs | -0.4 % (-1.6 µs) | yes |
| ii | schedule | p99 | 413.2 µs | 404.8 µs | 412.0 µs | 400.8 µs | -0.6 % (-2.6 µs) | yes |
| ii | service | p50 | 155.7 µs | 150.2 µs | 155.4 µs | 152.3 µs | +0.6 % (+0.9 µs) | yes |
| ii | service | p95 | 260.1 µs | 254.6 µs | 257.5 µs | 256.1 µs | -0.2 % (-0.6 µs) | yes |
| ii | service | p99 | 303.0 µs | 296.8 µs | 301.6 µs | 296.6 µs | -0.3 % (-0.8 µs) | yes |
| v | schedule | p50 | 239.1 µs | 234.8 µs | 237.9 µs | 241.3 µs | +1.1 % (+2.7 µs) | yes |
| v | schedule | p95 | 351.2 µs | 348.6 µs | 348.8 µs | 352.4 µs | +0.2 % (+0.7 µs) | yes |
| v | schedule | p99 | 397.7 µs | 397.2 µs | 399.6 µs | 400.1 µs | +0.6 % (+2.4 µs) | **no** |
| v | service | p50 | 152.1 µs | 147.6 µs | 151.2 µs | 152.6 µs | +1.4 % (+2.0 µs) | yes |
| v | service | p95 | 254.9 µs | 252.5 µs | 253.9 µs | 255.6 µs | +0.4 % (+1.1 µs) | yes |
| v | service | p99 | 294.4 µs | 293.7 µs | 294.4 µs | 296.6 µs | +0.5 % (+1.5 µs) | yes |
| vscan | schedule | p50 | 280.4 µs | 282.8 µs | 278.0 µs | 275.6 µs | -1.7 % (-4.8 µs) | **no** |
| vscan | schedule | p95 | 351.7 µs | 354.1 µs | 346.7 µs | 347.1 µs | -1.7 % (-6.0 µs) | **no** |
| vscan | schedule | p99 | 395.5 µs | 401.7 µs | 381.7 µs | 386.2 µs | -3.7 % (-14.7 µs) | **no** |
| vscan | service | p50 | 191.5 µs | 192.2 µs | 191.2 µs | 189.8 µs | -0.7 % (-1.3 µs) | **no** |
| vscan | service | p95 | 251.1 µs | 252.2 µs | 249.6 µs | 248.7 µs | -1.0 % (-2.5 µs) | **no** |
| vscan | service | p99 | 280.6 µs | 285.1 µs | 275.1 µs | 273.2 µs | -3.1 % (-8.7 µs) | **no** |
| iii4 | schedule | p50 | 294.9 µs | 319.0 µs | 298.5 µs | 315.7 µs | +0.0 % (+0.2 µs) | yes |
| iii4 | schedule | p95 | 380.0 µs | 402.9 µs | 384.3 µs | 399.8 µs | +0.2 % (+0.6 µs) | yes |
| iii4 | schedule | p99 | 435.1 µs | 452.8 µs | 431.5 µs | 462.1 µs | +0.6 % (+2.8 µs) | yes |
| iii4 | service | p50 | 207.9 µs | 226.5 µs | 211.5 µs | 227.0 µs | +0.9 % (+2.1 µs) | yes |
| iii4 | service | p95 | 280.1 µs | 299.0 µs | 284.4 µs | 298.5 µs | +0.7 % (+1.9 µs) | yes |
| iii4 | service | p99 | 318.1 µs | 328.3 µs | 318.3 µs | 334.7 µs | +1.0 % (+3.3 µs) | yes |


Reading. **p50 and p95: every difference between the arms' means is 1.4 % or
less in absolute value, in i, ii, v and iii4, and the ranges overlap**; the
widest is v's p50 from the schedule (+1.1 %, +2.7 µs) and service (+1.4 %,
+2.0 µs, ranges overlapping). **Literally, three cells have non-overlapping
ranges**: vscan (all of p50, p95 and p99 in both bases, B *lower* by 0.7 to
3.7 %) and v's p99 from the schedule (B 399.6 and 400.1 µs against A's 397.7
and 397.2 µs, +0.6 %, +2.4 µs: against cell i's two-round p99 spread of 108 µs
in A). v's p99 in service overlaps. **The data support "no resident cell is
slower outside its spread" at p50 and p95, with one 2.4 µs p99 exceedance at v
that is about 0.6 %; they cannot support a claim at p99 or above from two
points.** The p0 differences (i: B 14 % lower from the schedule, 20 % lower in
service, both rounds outside A's range) carry no claim: a p0 is one sample,
and ii's and v's p0 move the other way or not at all. What the client's
percentiles cannot see is the server CPU of section 9: +0.34 to +0.53 µs on a
~150 µs service time is under 0.4 % and below this table's resolution.

Cell ii is the page-cache-answers cell: the pool misses 10,010 to 10,023 times,
`preadv2` with `RWF_NOWAIT` hands every page over at once (`pool_would_wait`
0 in B, the `io.page_read` histogram in buckets 1 to 4, [2, 32) µs, mean 4.7 to
5.1 µs in both arms), so B runs no park there and its (ii) equals A's, as the
table shows.

### 5.2 Bar (2): "the same three numbers after"

**(1) The excess of (iii) over (ii), never-miss sessions**, same round (rounds 0
and 1 only):

| arm | round | basis | p50: (iii) - (ii) | p95: (iii) - (ii) | p99: (iii) - (ii) |
|---|---|---|---|---|---|
| A | r0 | schedule | +27.4 µs (1.11x) | +460.6 µs (2.28x) | +148,455.8 µs (360.28x) |
| A | r0 | service | +28.4 µs (1.18x) | +439.2 µs (2.69x) | +1,105.8 µs (4.65x) |
| A | r1 | schedule | +33.6 µs (1.14x) | +395.0 µs (2.12x) | +617.3 µs (2.52x) |
| A | r1 | service | +31.0 µs (1.21x) | +400.6 µs (2.57x) | +603.5 µs (3.03x) |
| B | r0 | schedule | +3.8 µs (1.02x) | +4.6 µs (1.01x) | +4.3 µs (1.01x) |
| B | r0 | service | +3.6 µs (1.02x) | +4.8 µs (1.02x) | +6.4 µs (1.02x) |
| B | r1 | schedule | +4.1 µs (1.02x) | +4.8 µs (1.01x) | +10.7 µs (1.03x) |
| B | r1 | service | +4.8 µs (1.03x) | +4.5 µs (1.02x) | +7.1 µs (1.02x) |


A: +395 to +461 µs at p95 (2.12 to 2.28x), +603 to +1,106 µs at p99 in service
and +617 µs from the schedule in round 1 (round 0's schedule p99 is the 188 ms
read, not claimed). B: +4.5 to +4.8 µs at p95 (1.01 to 1.02x) and +4.3 to
+10.7 µs at p99 (1.01 to 1.03x); p50 +3.6 to +4.8 µs (1.02 to 1.03x) in B and
+27.4 to +33.6 µs (1.11 to 1.21x) in A. **B's (iii) is 1.01 to 1.03 times its (ii) at p95 and p99 where A's is 2.1 to
2.7 times**; at p50 it did not go to zero (+3.6 to +4.8 µs in both rounds and
both bases, 1.02 to 1.03x), about the 5 µs between two rounds of A's cell i p50.

**(2) The share of the core's time inside `io.page_read`.** In B the event is
a **park, time the statement was suspended and the core was not stalled**; the
core's ON_CPU share and its idle block go beside it:

| cell | arm | `read_share_of_one_core`, `io.page_read` total over 20 s (rounds 0 to 3 where run) | what the event is | `on_cpu_share_of_one_core` | core idle block (of 20 s) |
|---|---|---|---|---|---|
| iii | A | 26.07 %, 24.90 %, 24.96 %, 25.28 % | the core inside a blocking `pread` | 4.87 %, 4.81 %, 5.58 %, 5.65 % | 12.97 s and 13.28 s (r0, r1) |
| iii | B | 24.18 %, 23.87 %, 23.73 %, 23.91 % | statements parked, core free | 7.22 %, 6.88 %, 8.05 %, 8.04 % | 17.71 s and 17.80 s (r0, r1) |
| iv | A | 92.72 %, 92.71 %, 92.55 %, 92.62 % | the core inside a blocking `pread` | 5.55 %, 5.49 %, 5.68 %, 5.63 % | 0.00 s (r0, r1) |
| iv | B | 90.53 %, 90.50 %, 90.26 %, 90.28 % | statements parked, core free (about 5 % of the reads; the rest return in under 4 µs) | 13.14 %, 13.18 %, 13.80 %, 13.86 % | 15.42 s and 15.43 s (r0, r1) |
| ii | A, B | 0.24 to 0.25 % in both | `pread` answered by the page cache | 4.22 to 4.51 % in both | 18.30 to 18.40 s in both |

Reading. **The two arms spend about the same time inside the read (24 to 26 %
in iii, 90 to 93 % in iv) and the two numbers mean different things**: in A
the core did nothing else in that time; in B the core's idle block is 17.7 to
17.8 s (rounds 0 and 1), 4.5 to 4.7 s more than A's 13.0 to 13.3 s, and its
ON_CPU share is 2.1 to 2.5 points higher. The
extra ON_CPU is **41 to 49 µs of core per cold read** (derived: (B - A)
on_cpu per round, 0.475, 0.414, 0.494 and 0.478 s, over 10,001 to 10,026
reads): the discarded first run of the statement, its parse and compile again,
the reader's queue and the wake. The mean `io.page_read` per read is 473.3 to
482.4 µs in B and 496.7 to 514.0 µs in A (the A mean includes A's stalls; not
attributed further). In iv the read share stays 90 % in B because the event is
timed from hand-off to wake and the scanner's 19.5 to 19.7 thousand parks a
window are exactly its slow reads (below).

**(3) The device's own read time.** Each cell run ran a **200-read** `O_DIRECT`
8 KiB queue-depth-1 probe before the mount (the order of this run, set by
`--probe-reads 200`); the raw samples are not kept, so the probes are pooled
here as the **median of 36 per-probe percentiles** (28 runs of part 1's rounds
0 and 1 plus the 8 extra runs): p50 438.9 µs (range of the 36 per-probe p50s
329.0 to 497.9 µs), p95 624.0 µs (480.9 to 958.4 µs), p99 828.9 µs (592.8 to
2,173.0 µs), p0 252.1 µs. The iii runs' own probes had p50s of 365.2 to
497.0 µs. **DE-S0's 2,000-read probe is the reference for the shape of the
distribution** (p50 462.9 µs, p95 712.7 µs, p99 989.6 µs, p0 5.3 µs, 2,000
reads; its p0 was a page-cache hit, which these 200-read probes did not
meet). The server's mean per read, 473 to 514 µs, sits at the probe's p50
(within 15 %). Kernel `read_bytes` per counted read is 8,254 to 8,267 B in iii
in both arms and 8,203 to 8,204 B in iv: **B's reader does not read the device
twice** (its warm and the re-run's fetch share one device read).

### 5.3 What cannot be claimed

Two rounds per cell (four in iii and iv). The (iii) minus (ii) table has two
matched pairs per arm. The resident verdict at p99 and above is not
supported by this design. The close bar's wording, "the resident cells no
slower outside their spread", is met at p50 and p95 and has one 2.4 µs
exceedance at v's p99 from the schedule; I do not rule "pass" on a bar
whose spread is two points, and part 2 (section 9) is a separate finding that
the resident path pays CPU.

## 6. What the cold sessions saw: did the read that left the reactor make them slower?

Yes, a little. Cold sessions' service time, from the send:

| cell | round | arm | p0 | p25 | p50 | p95 | p99 | p99.9 | max | n |
|---|---|---|---|---|---|---|---|---|---|---|
| ii | r0 | A | 76.3 µs | 133.0 µs | 160.7 µs | 268.9 µs | 305.7 µs | 738.6 µs | 3,730.1 µs | 13,003 ops |
| ii | r0 | B | 78.2 µs | 132.6 µs | 159.7 µs | 268.5 µs | 312.3 µs | 1,193.8 µs | 5,133.6 µs | 13,003 ops |
| ii | r1 | A | 78.2 µs | 129.0 µs | 154.5 µs | 262.3 µs | 306.8 µs | 879.0 µs | 2,387.0 µs | 13,003 ops |
| ii | r1 | B | 79.4 µs | 129.5 µs | 155.7 µs | 262.7 µs | 305.2 µs | 572.7 µs | 2,992.9 µs | 13,003 ops |
| iii | r0 | A | 83.7 µs | 494.5 µs | 621.8 µs | 929.1 µs | 1,888.0 µs | 12,665.3 µs | 39,583.7 µs | 13,003 ops |
| iii | r0 | B | 79.4 µs | 507.1 µs | 643.0 µs | 922.2 µs | 1,371.6 µs | 2,618.6 µs | 4,561.4 µs | 13,003 ops |
| iii | r1 | A | 77.0 µs | 484.9 µs | 616.8 µs | 893.1 µs | 1,284.4 µs | 2,970.5 µs | 7,470.6 µs | 13,003 ops |
| iii | r1 | B | 80.8 µs | 500.9 µs | 634.0 µs | 914.6 µs | 1,324.7 µs | 2,831.5 µs | 7,945.8 µs | 13,003 ops |
| v | r0 | A | 76.8 µs | 123.7 µs | 149.3 µs | 252.0 µs | 294.2 µs | 387.7 µs | 1,525.2 µs | 13,003 ops |
| v | r0 | B | 71.8 µs | 125.9 µs | 151.2 µs | 256.8 µs | 296.1 µs | 422.7 µs | 1,382.8 µs | 13,003 ops |
| v | r1 | A | 69.4 µs | 122.8 µs | 147.8 µs | 255.6 µs | 298.5 µs | 864.0 µs | 2,049.7 µs | 13,003 ops |
| v | r1 | B | 74.9 µs | 126.6 µs | 152.3 µs | 254.9 µs | 298.3 µs | 511.2 µs | 2,113.8 µs | 13,003 ops |
| iii4 | r0 | A | 112.3 µs | 534.8 µs | 668.8 µs | 939.8 µs | 1,319.6 µs | 2,594.9 µs | 4,555.9 µs | 13,003 ops |
| iii4 | r0 | B | 108.5 µs | 550.0 µs | 691.2 µs | 954.9 µs | 1,239.5 µs | 2,557.3 µs | 8,431.9 µs | 13,003 ops |
| iii4 | r1 | A | 112.3 µs | 559.1 µs | 697.1 µs | 970.4 µs | 1,434.6 µs | 2,783.1 µs | 4,706.6 µs | 13,003 ops |
| iii4 | r1 | B | 111.1 µs | 579.6 µs | 728.8 µs | 1,026.6 µs | 1,543.3 µs | 3,525.3 µs | 6,079.2 µs | 13,003 ops |


In (iii) the cold sessions' p50 is 643.0 and 634.0 µs in B against 621.8 and
616.8 µs in A (rounds 0 and 1; round 2 and 3: 664.5 and 663.3 against 648.5 and
650.4 µs): **+21.2, +17.2, +16.0 and +12.9 µs (+3.4, +2.8, +2.5 and +2.0 %)**,
the same sign in all four rounds. p95 is -6.9, +21.5, +31.7 and +13.3 µs
(-0.7 % to +3.5 %), p99 inside the rounds' scatter (A 1,227.6 to 1,888.0 µs,
B 1,198.8 to 1,371.6 µs). In iii4 (clients on CPUs 4 to 7) the cold p50 is
+22.4 and +31.7 µs (+3.3 and +4.5 %). A cold read in B pays the round trip
that A pays in its `pread`, plus the statement's first run, the reader's
queue and the wake: about 40 to 50 µs of core, 13 to 32 µs of the session's
own latency. In (ii) and (v) (no park) the cold sessions' p50 differ by -1.0 and
+1.2 µs (ii) and +1.9 and +4.5 µs (v), B against A.

| round | arm | core | p0 | p25 | p50 | p95 | p99 | p99.9 | max | n |
|---|---|---|---|---|---|---|---|---|---|---|
| r0 | A | 1 | 148.3 µs | 258.0 µs | 289.4 µs | 376.7 µs | 432.5 µs | 2,023.5 µs | 4,889.2 µs | 10,002 ops |
| r0 | A | 2 | 196.5 µs | 275.4 µs | 299.9 µs | 379.1 µs | 425.3 µs | 1,698.7 µs | 3,393.4 µs | 5,001 ops |
| r0 | A | 3 | 186.4 µs | 271.1 µs | 298.3 µs | 387.2 µs | 441.6 µs | 1,027.1 µs | 2,035.4 µs | 5,001 ops |
| r0 | B | 1 | 168.6 µs | 262.5 µs | 293.5 µs | 384.6 µs | 428.4 µs | 1,787.4 µs | 4,465.6 µs | 10,002 ops |
| r0 | B | 2 | 197.6 µs | 281.8 µs | 305.9 µs | 383.4 µs | 425.3 µs | 1,030.9 µs | 2,007.0 µs | 5,001 ops |
| r0 | B | 3 | 188.1 µs | 273.9 µs | 299.2 µs | 386.2 µs | 440.8 µs | 2,441.9 µs | 6,490.5 µs | 5,001 ops |
| r1 | A | 1 | 155.4 µs | 280.1 µs | 310.7 µs | 397.9 µs | 445.4 µs | 1,822.7 µs | 4,071.5 µs | 10,002 ops |
| r1 | A | 2 | 198.6 µs | 302.1 µs | 329.3 µs | 401.7 µs | 448.0 µs | 1,926.9 µs | 5,916.8 µs | 5,001 ops |
| r1 | A | 3 | 205.3 µs | 295.6 µs | 323.8 µs | 411.3 µs | 477.1 µs | 1,237.9 µs | 5,012.5 µs | 5,001 ops |
| r1 | B | 1 | 165.2 µs | 280.1 µs | 313.3 µs | 398.9 µs | 451.3 µs | 1,751.9 µs | 6,103.8 µs | 10,001 ops |
| r1 | B | 2 | 205.5 µs | 296.1 µs | 322.6 µs | 397.7 µs | 434.2 µs | 1,304.9 µs | 5,293.4 µs | 5,001 ops |
| r1 | B | 3 | 177.1 µs | 284.7 µs | 313.8 µs | 403.4 µs | 575.5 µs | 2,329.1 µs | 4,394.1 µs | 5,001 ops |


(That table is iii4's never-miss sessions by core; at `cores = 4` neither
arm shows the stall, and the arms are equal: p95 of 376.7 to 411.3 µs and p99 of
425.3 to 575.5 µs across cores and rounds in both arms, the 575.5 µs a
single core-3 reading in B r1, 1.21x A's 477.1 µs there. iii4 has no `cores = 4` resident
baseline, so nothing in it is a "no slower" claim; the stall is a same-core
effect in A as in DE-S0, and B's change is not visible where the stall was
already absent.)

## 7. The accounting of the waits (rule 3) and DE-R9

`sys.wait_stats` decomposes the **reactor's time over the window, not one
hot request**: the wait a hot request spends behind another session's read is
not an event of its own on this engine (DE-S0's file, section 6.1), so a
per-request breakdown is not measurable. The reactor's window:

| cell | round | arm | `cpu.on_cpu` | `io.page_read` count | `io.page_read` total | mean per read | `read_share_of_one_core` | `on_cpu_share_of_one_core` | `sched.run_queue` count / total | core idle block (`meta_delta`) | `pool_would_wait` | `miss_reruns` |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| i | r0 | A | 0.617 s | 0 reads | 0.000 s | - | 0.00 % | 3.09 % | 20,020 / 0.025 s | 18.85 s | absent (0) | absent (0) |
| i | r0 | B | 0.618 s | 0 reads | 0.000 s | - | 0.00 % | 3.09 % | 20,019 / 0.027 s | 18.85 s | 0.0 | 0.0 |
| i | r1 | A | 0.599 s | 0 reads | 0.000 s | - | 0.00 % | 2.99 % | 20,019 / 0.023 s | 18.89 s | absent (0) | absent (0) |
| i | r1 | B | 0.602 s | 0 reads | 0.000 s | - | 0.00 % | 3.01 % | 20,019 / 0.025 s | 18.88 s | 0.0 | 0.0 |
| ii | r0 | A | 0.901 s | 10,020 reads | 0.050 s | 5.0 µs | 0.25 % | 4.51 % | 33,019 / 0.067 s | 18.30 s | absent (0) | absent (0) |
| ii | r0 | B | 0.898 s | 10,023 reads | 0.051 s | 5.1 µs | 0.25 % | 4.49 % | 33,020 / 0.068 s | 18.31 s | 0.0 | 0.0 |
| ii | r1 | A | 0.845 s | 10,010 reads | 0.047 s | 4.7 µs | 0.24 % | 4.22 % | 33,020 / 0.064 s | 18.40 s | absent (0) | absent (0) |
| ii | r1 | B | 0.864 s | 10,020 reads | 0.050 s | 5.0 µs | 0.25 % | 4.32 % | 33,020 / 0.064 s | 18.37 s | 0.0 | 0.0 |
| iii | r0 | A | 0.968 s | 10,073 reads | 5.178 s | 514.0 µs | 26.07 % | 4.87 % | 33,141 / 0.965 s | 12.97 s | absent (0) | absent (0) |
| iii | r0 | B | 1.443 s | 10,026 reads | 4.837 s | 482.4 µs | 24.18 % | 7.22 % | 33,019 / 0.103 s | 17.71 s | 10025.0 | 10025.0 |
| iii | r1 | A | 0.962 s | 10,025 reads | 4.979 s | 496.7 µs | 24.90 % | 4.81 % | 33,019 / 0.189 s | 13.28 s | absent (0) | absent (0) |
| iii | r1 | B | 1.376 s | 10,001 reads | 4.773 s | 477.3 µs | 23.87 % | 6.88 % | 33,019 / 0.098 s | 17.80 s | 10000.0 | 10000.0 |
| iii | r2 | A | 1.117 s | 10,000 reads | 4.992 s | 499.2 µs | 24.96 % | 5.58 % | 33,019 / 0.223 s | 12.99 s | absent (0) | absent (0) |
| iii | r2 | B | 1.610 s | 10,025 reads | 4.745 s | 473.3 µs | 23.73 % | 8.05 % | 33,020 / 0.132 s | 17.45 s | 10024.0 | 10024.0 |
| iii | r3 | A | 1.130 s | 10,006 reads | 5.056 s | 505.3 µs | 25.28 % | 5.65 % | 33,019 / 0.217 s | 12.93 s | absent (0) | absent (0) |
| iii | r3 | B | 1.607 s | 10,026 reads | 4.782 s | 477.0 µs | 23.91 % | 8.04 % | 33,020 / 0.121 s | 17.46 s | 10025.0 | 10025.0 |
| iv | r0 | A | 1.109 s | 395,848 reads | 18.545 s | 46.8 µs | 92.72 % | 5.55 % | 18,592 / 40.079 s | 0.00 s | absent (0) | absent (0) |
| iv | r0 | B | 2.628 s | 395,924 reads | 18.106 s | 45.7 µs | 90.53 % | 13.14 % | 26,210 / 0.309 s | 15.42 s | 19484.0 | 0.0 |
| iv | r1 | A | 1.098 s | 395,912 reads | 18.544 s | 46.8 µs | 92.71 % | 5.49 % | 18,603 / 40.155 s | 0.00 s | absent (0) | absent (0) |
| iv | r1 | B | 2.636 s | 389,789 reads | 18.100 s | 46.4 µs | 90.50 % | 13.18 % | 26,115 / 0.296 s | 15.43 s | 19398.0 | 0.0 |
| iv | r2 | A | 1.136 s | 395,912 reads | 18.511 s | 46.8 µs | 92.55 % | 5.68 % | 18,594 / 40.105 s | 0.00 s | absent (0) | absent (0) |
| iv | r2 | B | 2.761 s | 395,908 reads | 18.053 s | 45.6 µs | 90.26 % | 13.80 % | 26,210 / 0.302 s | 15.22 s | 19721.0 | 0.0 |
| iv | r3 | A | 1.126 s | 395,848 reads | 18.525 s | 46.8 µs | 92.62 % | 5.63 % | 18,591 / 40.137 s | 0.00 s | absent (0) | absent (0) |
| iv | r3 | B | 2.773 s | 395,476 reads | 18.055 s | 45.7 µs | 90.28 % | 13.86 % | 26,204 / 0.329 s | 15.20 s | 19677.0 | 0.0 |
| v | r0 | A | 0.840 s | 0 reads | 0.000 s | - | 0.00 % | 4.20 % | 33,020 / 0.057 s | 18.44 s | absent (0) | absent (0) |
| v | r0 | B | 0.853 s | 0 reads | 0.000 s | - | 0.00 % | 4.27 % | 33,020 / 0.060 s | 18.42 s | 0.0 | 0.0 |
| v | r1 | A | 0.808 s | 0 reads | 0.000 s | - | 0.00 % | 4.04 % | 33,020 / 0.060 s | 18.49 s | absent (0) | absent (0) |
| v | r1 | B | 0.849 s | 0 reads | 0.000 s | - | 0.00 % | 4.24 % | 33,021 / 0.066 s | 18.43 s | 0.0 | 0.0 |
| vscan | r0 | A | 19.217 s | 0 reads | 0.000 s | - | 0.00 % | 96.08 % | 607,833 / 1.929 s | 0.04 s | absent (0) | absent (0) |
| vscan | r0 | B | 19.262 s | 0 reads | 0.000 s | - | 0.00 % | 96.31 % | 553,896 / 1.880 s | 0.04 s | 0.0 | 0.0 |
| vscan | r1 | A | 19.218 s | 0 reads | 0.000 s | - | 0.00 % | 96.09 % | 599,113 / 1.918 s | 0.04 s | absent (0) | absent (0) |
| vscan | r1 | B | 19.260 s | 0 reads | 0.000 s | - | 0.00 % | 96.30 % | 556,687 / 1.891 s | 0.04 s | 0.0 | 0.0 |
| iii4 | r0 | A | 1.687 s | 10,025 reads | 5.147 s | 513.4 µs | 25.73 % | 8.44 % | 33,062 / 0.048 s | 19.47 s | absent (0) | absent (0) |
| iii4 | r0 | B | 2.244 s | 10,027 reads | 4.780 s | 476.7 µs | 23.90 % | 11.22 % | 33,064 / 0.051 s | 19.45 s | 10025.0 | 10025.0 |
| iii4 | r1 | A | 1.798 s | 10,005 reads | 5.202 s | 519.9 µs | 26.01 % | 8.99 % | 33,064 / 0.059 s | 19.45 s | absent (0) | absent (0) |
| iii4 | r1 | B | 2.406 s | 10,003 reads | 5.030 s | 502.8 µs | 25.15 % | 12.03 % | 33,063 / 0.059 s | 19.07 s | 10001.0 | 10000.0 |


`cpu.on_cpu`'s count is 0 in every JSON (time only) and its total sums every
session. In B `io.page_read` for a parked read is the statement's suspension,
not the core's stall. `pool_would_wait` equals `miss_reruns` to within 1 in
every parking cell (iii: 10,000 to 10,025 against 10,001 to 10,026 reads; iii4
the same; iv: 19,398 to 19,721 against 389,789 to 395,924), so **each cold
statement is refused once and run again once** (refusals about equal the
cold statements, each of which misses one leaf), so none reached the eight-run cap in iii.

**DE-R9 ("the event keeps its name", `io.page_read` once a read on both
paths).** `io.page_read`'s count stays one per device read in B: 10,001 to
10,026 reads in iii (A: 10,000 to 10,073), 389,789 to 395,924 in iv (A:
395,848 to 395,912), and kernel `read_bytes` per counted read is 8,254 to
8,267 B in both arms. **The pool's miss and hit counters do not: B's
`pool_misses` is 2.00 per counted read in iii and iii4 (20,002 to 20,053
against 10,001 to 10,027 reads) and 1.05 in iv, A's is 1.00, and B's hits per
counted read are 26.0 to 26.1 against A's 22.0 to 22.1.** The refused fetch
counts as a miss and the fetch after the warm counts as another; the re-run of
the statement's descent repeats its four hits on the upper levels (the pool
hits rise by 39,400 to 40,000 a window, about four a cold read). `sys.meta`'s
`pool_misses` can no longer be read as "reads that reached the device" in B;
`io.page_read`'s count can. The sentence of §7 (DE-S1) that a device read is
one `io.page_read` on either path holds on this run's data; I did not find a
statement of the order that the miss counter stays one per read, and say only
that it does not.

| cell | round | arm | `reads_in_window` | pool misses | misses per counted read | pool hits | hits per counted read | `read_bytes` | `device_bytes_per_read` |
|---|---|---|---|---|---|---|---|---|---|
| i | r0 | A | 0 reads | 0 misses | - | 140,011 hits | - | 0 B | no reads counted |
| i | r0 | B | 0 reads | 0 misses | - | 140,004 hits | - | 0 B | no reads counted |
| i | r1 | A | 0 reads | 0 misses | - | 140,011 hits | - | 0 B | no reads counted |
| i | r1 | B | 0 reads | 0 misses | - | 140,004 hits | - | 0 B | no reads counted |
| ii | r0 | A | 10,020 reads | 10,020 misses | 1.000 | 220,984 hits | 22.05 | 0 B | no reads counted |
| ii | r0 | B | 10,023 reads | 10,023 misses | 1.000 | 220,988 hits | 22.05 | 0 B | no reads counted |
| ii | r1 | A | 10,010 reads | 10,010 misses | 1.000 | 221,001 hits | 22.08 | 0 B | no reads counted |
| ii | r1 | B | 10,020 reads | 10,020 misses | 1.000 | 220,991 hits | 22.05 | 0 B | no reads counted |
| iii | r0 | A | 10,073 reads | 10,063 misses | 0.999 | 221,613 hits | 22.00 | 83,181,568 B | 8,257.9 B |
| iii | r0 | B | 10,026 reads | 20,052 misses | 2.000 | 261,084 hits | 26.04 | 82,886,656 B | 8,267.2 B |
| iii | r1 | A | 10,025 reads | 10,025 misses | 1.000 | 220,986 hits | 22.04 | 82,870,272 B | 8,266.4 B |
| iii | r1 | B | 10,001 reads | 20,002 misses | 2.000 | 261,009 hits | 26.10 | 82,657,280 B | 8,264.9 B |
| iii | r2 | A | 10,000 reads | 10,000 misses | 1.000 | 221,011 hits | 22.10 | 82,665,472 B | 8,266.5 B |
| iii | r2 | B | 10,025 reads | 20,050 misses | 2.000 | 261,074 hits | 26.04 | 82,878,464 B | 8,267.2 B |
| iii | r3 | A | 10,006 reads | 10,006 misses | 1.000 | 220,998 hits | 22.09 | 82,714,624 B | 8,266.5 B |
| iii | r3 | B | 10,026 reads | 20,052 misses | 2.000 | 261,084 hits | 26.04 | 82,886,656 B | 8,267.2 B |
| iv | r0 | A | 395,848 reads | 395,848 misses | 1.000 | 86,950 hits | 0.22 | 3,247,308,800 B | 8,203.4 B |
| iv | r0 | B | 395,924 reads | 415,376 misses | 1.049 | 140,325 hits | 0.35 | 3,247,964,160 B | 8,203.5 B |
| iv | r1 | A | 395,912 reads | 395,912 misses | 1.000 | 87,020 hits | 0.22 | 3,248,095,232 B | 8,204.1 B |
| iv | r1 | B | 389,789 reads | 409,187 misses | 1.050 | 140,325 hits | 0.36 | 3,197,894,656 B | 8,204.2 B |
| iv | r2 | A | 395,912 reads | 395,912 misses | 1.000 | 86,957 hits | 0.22 | 3,247,964,160 B | 8,203.8 B |
| iv | r2 | B | 395,908 reads | 415,613 misses | 1.050 | 140,332 hits | 0.35 | 3,247,833,088 B | 8,203.5 B |
| iv | r3 | A | 395,848 reads | 395,848 misses | 1.000 | 86,943 hits | 0.22 | 3,247,702,016 B | 8,204.4 B |
| iv | r3 | B | 395,476 reads | 415,141 misses | 1.050 | 140,332 hits | 0.35 | 3,244,425,216 B | 8,203.8 B |
| v | r0 | A | 0 reads | 0 misses | - | 231,004 hits | - | 0 B | no reads counted |
| v | r0 | B | 0 reads | 0 misses | - | 231,011 hits | - | 0 B | no reads counted |
| v | r1 | A | 0 reads | 0 misses | - | 231,011 hits | - | 0 B | no reads counted |
| v | r1 | B | 0 reads | 0 misses | - | 231,011 hits | - | 0 B | no reads counted |
| vscan | r0 | A | 0 reads | 0 misses | - | 37,761,114 hits | - | 0 B | no reads counted |
| vscan | r0 | B | 0 reads | 0 misses | - | 34,309,254 hits | - | 0 B | no reads counted |
| vscan | r1 | A | 0 reads | 0 misses | - | 37,202,375 hits | - | 0 B | no reads counted |
| vscan | r1 | B | 0 reads | 0 misses | - | 34,487,564 hits | - | 0 B | no reads counted |
| iii4 | r0 | A | 10,025 reads | 10,025 misses | 1.000 | 220,986 hits | 22.04 | 82,780,160 B | 8,257.4 B |
| iii4 | r0 | B | 10,027 reads | 20,053 misses | 2.000 | 261,088 hits | 26.04 | 82,845,696 B | 8,262.3 B |
| iii4 | r1 | A | 10,005 reads | 10,005 misses | 1.000 | 221,006 hits | 22.09 | 82,583,552 B | 8,254.2 B |
| iii4 | r1 | B | 10,003 reads | 20,005 misses | 2.000 | 261,012 hits | 26.09 | 82,599,936 B | 8,257.5 B |


`io.page_read` histogram (bucket k counts reads of [2^k, 2^(k+1)) µs):

| cell | round | arm | reads by bucket k (bucket k = [2^k, 2^(k+1)) µs): count | total |
|---|---|---|---|---|
| ii | r0 | A | 1: 2,208; 2: 7,630; 3: 152; 4: 29; 8: 1 | 10,020 reads |
| ii | r0 | B | 1: 1,988; 2: 7,819; 3: 185; 4: 31 | 10,023 reads |
| ii | r1 | A | 1: 2,744; 2: 7,137; 3: 127; 4: 2 | 10,010 reads |
| ii | r1 | B | 1: 2,014; 2: 7,849; 3: 156; 4: 1 | 10,020 reads |
| iii | r0 | A | 2: 2; 8: 6,743; 9: 3,162; 10: 98; 11: 51; 12: 11; 13: 1 | 10,068 reads |
| iii | r0 | B | 1: 1; 2: 1; 7: 2; 8: 7,209; 9: 2,673; 10: 120; 11: 19; 12: 2 | 10,027 reads |
| iii | r1 | A | 2: 2; 8: 6,641; 9: 3,261; 10: 107; 11: 12; 12: 2 | 10,025 reads |
| iii | r1 | B | 1: 2; 7: 1; 8: 7,305; 9: 2,566; 10: 109; 11: 16; 12: 2 | 10,001 reads |
| iii | r2 | A | 2: 2; 8: 6,445; 9: 3,463; 10: 82; 11: 8 | 10,000 reads |
| iii | r2 | B | 2: 2; 7: 2; 8: 7,306; 9: 2,639; 10: 70; 11: 6; 12: 1 | 10,026 reads |
| iii | r3 | A | 1: 1; 2: 1; 8: 6,302; 9: 3,591; 10: 101; 11: 10 | 10,006 reads |
| iii | r3 | B | 1: 1; 2: 1; 6: 1; 7: 1; 8: 7,364; 9: 2,564; 10: 77; 11: 16; 12: 1; 13: 1 | 10,027 reads |
| iv | r0 | A | 0: 272,952; 1: 93,578; 2: 2,179; 3: 427; 4: 109; 5: 3,389; 6: 3,705; 7: 564; 8: 6,288; 9: 6,523; 10: 5,932; 11: 199; 12: 2; 13: 1 | 395,848 reads |
| iv | r0 | B | 0: 253,309; 1: 113,332; 2: 2,091; 3: 428; 4: 110; 5: 3,498; 6: 4,012; 7: 487; 8: 7,959; 9: 4,248; 10: 6,277; 11: 170; 12: 3 | 395,924 reads |
| iv | r1 | A | 0: 262,782; 1: 103,742; 2: 2,244; 3: 485; 4: 81; 5: 3,541; 6: 3,736; 7: 690; 8: 6,568; 9: 5,754; 10: 6,101; 11: 183; 12: 5 | 395,912 reads |
| iv | r1 | B | 0: 242,195; 1: 118,788; 2: 2,070; 3: 468; 4: 75; 5: 3,301; 6: 3,810; 7: 471; 8: 7,816; 9: 4,692; 10: 5,882; 11: 203; 12: 14; 13: 1; 15: 2; 17: 1 | 389,789 reads |
| iv | r2 | A | 0: 277,155; 1: 89,055; 2: 2,528; 3: 479; 4: 114; 5: 3,535; 6: 3,471; 7: 597; 8: 6,252; 9: 6,709; 10: 5,804; 11: 210; 12: 2; 13: 1 | 395,912 reads |
| iv | r2 | B | 0: 263,359; 1: 103,111; 2: 2,262; 3: 522; 4: 67; 5: 3,073; 6: 4,084; 7: 408; 8: 7,904; 9: 4,932; 10: 6,058; 11: 125; 12: 3 | 395,908 reads |
| iv | r3 | A | 0: 278,879; 1: 87,244; 2: 2,502; 3: 544; 4: 90; 5: 3,565; 6: 3,444; 7: 596; 8: 6,217; 9: 6,740; 10: 5,837; 11: 186; 12: 4 | 395,848 reads |
| iv | r3 | B | 0: 262,124; 1: 103,759; 2: 2,383; 3: 506; 4: 91; 5: 3,244; 6: 3,986; 7: 418; 8: 7,922; 9: 4,820; 10: 5,987; 11: 197; 12: 6; 13: 1 | 395,444 reads |
| iii4 | r0 | A | 2: 2; 8: 5,950; 9: 3,949; 10: 108; 11: 17 | 10,026 reads |
| iii4 | r0 | B | 2: 2; 5: 2; 7: 5; 8: 7,202; 9: 2,732; 10: 72; 11: 11; 12: 2 | 10,028 reads |
| iii4 | r1 | A | 2: 2; 8: 5,810; 9: 4,053; 10: 123; 11: 17; 12: 1 | 10,006 reads |
| iii4 | r1 | B | 2: 2; 5: 1; 7: 2; 8: 6,564; 9: 3,283; 10: 124; 11: 25; 12: 3 | 10,004 reads |


In (iii) 98 % of the reads in both arms fall in buckets 8 and 9, [256, 1,024)
µs: B's reads are as long as A's, and the park, not the read, changed. In (iv)
the reads under 4 µs (buckets 0 and 1) are 91.4 to 92.6 % of both arms'
reads (A 366,530 and 366,524 in rounds 0 and 1 against B 366,641 and 360,983: 92.6 % in
all four), the reads of 256 µs and over (bucket 8 and up) are 4.7 to 4.8 % of
both arms' reads, and **B parks on 4.9 to 5.0 % of the reads** (19,398 to 19,721
of 389,789 to 395,924): the walk parks exactly where the
kernel would have blocked it, and not where read-ahead has the page.

## 8. The scan: iv against vscan, and what a park does to the scanner

**Does the never-miss sessions' schedule still fall behind in B?** No: 20,003
or 20,004 of the 20,004 due are sent and completed in all four rounds, a
schedule p50 of 282.8 to 309.0 µs (section 4's table); A completes 10,892 to
10,908 (54.5 %) at 5.75 s.

**Did parking slow the scan itself?** Cell iv's scanner:

| arm | round | scanner pages/s (`reads_in_window` / 20 s) | derived MB/s at 8,192 B | kernel `read_bytes` MB/s | hot reads completed | hot reads/s |
|---|---|---|---|---|---|---|
| A | r0 | 19,792 pages/s | 162.1 MB/s | 162.4 MB/s | 10,895 of 20,004 | 544.8 reads/s |
| B | r0 | 19,796 pages/s | 162.2 MB/s | 162.4 MB/s | 20,003 of 20,004 | 1,000.1 reads/s |
| A | r1 | 19,795 pages/s | 162.2 MB/s | 162.4 MB/s | 10,908 of 20,004 | 545.4 reads/s |
| B | r1 | 19,489 pages/s | 159.7 MB/s | 159.9 MB/s | 20,004 of 20,004 | 1,000.2 reads/s |
| A | r2 | 19,795 pages/s | 162.2 MB/s | 162.4 MB/s | 10,899 of 20,004 | 544.9 reads/s |
| B | r2 | 19,795 pages/s | 162.2 MB/s | 162.4 MB/s | 20,004 of 20,004 | 1,000.2 reads/s |
| A | r3 | 19,792 pages/s | 162.1 MB/s | 162.4 MB/s | 10,892 of 20,004 | 544.6 reads/s |
| B | r3 | 19,774 pages/s | 162.0 MB/s | 162.2 MB/s | 20,003 of 20,004 | 1,000.1 reads/s |


The scanner reads **19,774 to 19,796 pages/s in B in three of four rounds
against 19,792 to 19,795 in A in all four, and 19,489 in B's round 1 (-1.5 %)**.
The equal rates say the scan is bound by the device at queue depth 1 in both arms (one leaf at a
time, read then park then read; inferred, not instrumented), at 162 MB/s, and parking neither speeds it
nor slows it: the core it no longer holds is idle (B iv's idle block is 15.4
s of 20 s, A's 0.00 s), not given to the scanner. The scanner's cost for the
hot sessions in B is the 64-page slices between its parks (not isolated).

**The resident scan, vscan, does not come out the same.** The scanner with the
relation resident (no park, no refusal) runs in a window:

| arm | round | pool hits in window | 64-page slices/s (derived: hits / 64 / 20 s) | `cpu.on_cpu` | `sched.run_queue` count |
|---|---|---|---|---|---|
| A | r0 | 37,761,114 hits | 29,501 slices/s | 19.217 s | 607,833 |
| B | r0 | 34,309,254 hits | 26,804 slices/s | 19.262 s | 553,896 |
| A | r1 | 37,202,375 hits | 29,064 slices/s | 19.218 s | 599,113 |
| B | r1 | 34,487,564 hits | 26,943 slices/s | 19.260 s | 556,687 |


B's walk completes 7 to 9 % fewer slices per second (26,804 and 26,943 against
29,501 and 29,064: -9.1 % and -7.3 %, rounds 0 and 1; A's two rounds differ by
1.5 %, B's by 0.5 %) **at the same CPU** (`cpu.on_cpu` 19.26 s against 19.22
s), i.e. 34.8 against 31.6 µs of core a 64-page slice (derived: `on_cpu` over
the window's `sched.run_queue` count, which assumes one wait per slice), +3.2 µs
or about 50 ns a page. The hot sessions' own latencies in vscan are *lower* in B
(section 5.1), so the scanner does not lose its CPU to them. This is the
resident walk's cost of DE-S2's per-leaf fetch with `kScanNoWait` and its
refusal check, at a ~10 % higher per-page cost than A's walk; **I did not
isolate the cause** (a hit "reads and writes no word it did not before" in the
pool, DE-S1's entry, so what remains is the walk's per-leaf code, not the
pool's hit path), and it is two rounds. It does not show in the never-miss
sessions' percentiles, and it is a cost a resident full scan pays and a point
read does not. (This is B's finding; section 13's spike re-measured it over
three rounds and a remedy variant, and still does not isolate the cause.)

## 9. Part 2: the resident point statements at `cores = 1` (DE-R4, DE-Q14)

**What the order says it adds on every path**: one thread-local read per pool
miss (nothing on a hit), four idle threads, and at `cores = 1` a wake
descriptor in core 0's readiness set plus a sleeping-flag store around each
idle block (DE-Q14), and DE-S1's arming of a top-level `SELECT` body.
Priced the way `bench/v3.4.13/results-cx-close-bisect-v3.4.13-1-g11f72ce9.md`
prices sub-microsecond deltas: server CPU per statement from the server
threads' `schedstat`, **one client connection a server**, three servers side by
side on CPU 3 and queried one at a time, the client on CPU 5, arms **A, A2
(A's binary again, a second server of it, started from its own copy: the floor
between two servers of one binary) and B**, 24 blocks of 500 statements a
cell, the order of the servers rotating one place per block and per lifetime,
**six server lifetimes** (`--runs 6`: fresh servers and data files each, all
three binaries' sha256 as in the stamp). `tools/bn_census.py --arm A=... --arm
A2=... --arm B=... --workdir ~/bench-runs/de/p2/wd --port 32760 --rows 1000
--ops 12000 --blocks 24 --runs 6 --pin-server 3 --pin-driver 5
--cell-quiet-pct 15 --cell-quiet-wait 60 --cell-retries 2 --cell-clean-pct 25
--only select-pk,select-index,update-lit`, through `p2.py` (the shape
`select-index`: `SELECT * FROM x WHERE w = ?` over 1,000 rows with a
secondary index on `w`; `ANALYZE` reads `step 0 IndexProbe x`, so the index is
what was measured; `select-pk`: `SELECT * FROM t WHERE id = ?`; the write shape
`update-lit`: `UPDATE t SET v = ? WHERE id = ?`, a point UPDATE; each over both
driver paths, literal and prepared). 12,000 statements a cell and arm a
lifetime, 72,000 over six; every reply was checked (`bad` 0 everywhere); **all
108 cells were clean** (sibling-CPU busy under 25 %, foreign busy under 5 %, no
retries). Table of server CPU per statement (a cost, not a delay matrix):
per lifetime, and the deltas as the median of the 24 round-paired block
differences within the lifetime.

| shape | path | lifetime | A server CPU (µs) | A2 server CPU (µs) | B server CPU (µs) | A2 - A (µs) | B - A (µs) | B - A2 (µs) | bad replies | clean cell |
|---|---|---|---|---|---|---|---|---|---|---|
| select-index | literal | 0 | 23.80 | 23.48 | 24.29 | -0.28 | +0.47 | +0.66 | 0 | True |
| select-index | literal | 1 | 24.04 | 23.90 | 24.53 | -0.09 | +0.43 | +0.59 | 0 | True |
| select-index | literal | 2 | 23.75 | 23.79 | 24.31 | +0.06 | +0.54 | +0.48 | 0 | True |
| select-index | literal | 3 | 23.95 | 23.93 | 24.41 | +0.00 | +0.43 | +0.38 | 0 | True |
| select-index | literal | 4 | 23.93 | 23.67 | 24.49 | -0.10 | +0.60 | +0.55 | 0 | True |
| select-index | literal | 5 | 23.97 | 23.87 | 24.48 | -0.07 | +0.55 | +0.54 | 0 | True |
| select-index | prepared | 0 | 23.15 | 23.11 | 23.65 | +0.05 | +0.48 | +0.53 | 0 | True |
| select-index | prepared | 1 | 23.28 | 23.48 | 23.84 | +0.27 | +0.52 | +0.37 | 0 | True |
| select-index | prepared | 2 | 22.75 | 22.81 | 23.52 | +0.15 | +0.62 | +0.56 | 0 | True |
| select-index | prepared | 3 | 23.43 | 23.21 | 23.75 | -0.15 | +0.45 | +0.58 | 0 | True |
| select-index | prepared | 4 | 23.51 | 23.33 | 24.03 | -0.18 | +0.46 | +0.69 | 0 | True |
| select-index | prepared | 5 | 23.24 | 23.21 | 23.79 | -0.06 | +0.62 | +0.61 | 0 | True |
| select-pk | literal | 0 | 20.82 | 20.53 | 20.91 | -0.14 | +0.25 | +0.40 | 0 | True |
| select-pk | literal | 1 | 20.69 | 20.77 | 21.23 | +0.04 | +0.43 | +0.46 | 0 | True |
| select-pk | literal | 2 | 20.79 | 20.90 | 21.22 | +0.18 | +0.43 | +0.34 | 0 | True |
| select-pk | literal | 3 | 20.68 | 20.72 | 21.02 | -0.02 | +0.26 | +0.38 | 0 | True |
| select-pk | literal | 4 | 20.86 | 20.83 | 21.24 | -0.05 | +0.38 | +0.36 | 0 | True |
| select-pk | literal | 5 | 20.98 | 20.93 | 21.32 | -0.04 | +0.42 | +0.39 | 0 | True |
| select-pk | prepared | 0 | 19.86 | 19.89 | 20.21 | +0.00 | +0.23 | +0.29 | 0 | True |
| select-pk | prepared | 1 | 20.22 | 20.33 | 20.60 | +0.07 | +0.47 | +0.17 | 0 | True |
| select-pk | prepared | 2 | 20.01 | 20.23 | 20.44 | +0.28 | +0.51 | +0.37 | 0 | True |
| select-pk | prepared | 3 | 20.01 | 19.95 | 20.18 | -0.14 | +0.22 | +0.22 | 0 | True |
| select-pk | prepared | 4 | 19.93 | 19.97 | 20.42 | +0.00 | +0.56 | +0.51 | 0 | True |
| select-pk | prepared | 5 | 20.39 | 20.24 | 20.79 | -0.07 | +0.40 | +0.49 | 0 | True |
| update-lit | literal | 0 | 21.12 | 21.09 | 20.93 | +0.06 | -0.17 | -0.13 | 0 | True |
| update-lit | literal | 1 | 20.95 | 20.93 | 20.73 | -0.03 | -0.20 | -0.14 | 0 | True |
| update-lit | literal | 2 | 20.76 | 20.90 | 20.67 | +0.12 | -0.10 | -0.17 | 0 | True |
| update-lit | literal | 3 | 20.98 | 20.81 | 20.84 | -0.01 | -0.09 | -0.00 | 0 | True |
| update-lit | literal | 4 | 21.05 | 20.89 | 20.89 | -0.12 | -0.13 | -0.06 | 0 | True |
| update-lit | literal | 5 | 21.20 | 20.80 | 20.59 | -0.18 | -0.27 | -0.18 | 0 | True |
| update-lit | prepared | 0 | 20.21 | 20.15 | 19.86 | -0.09 | -0.33 | -0.26 | 0 | True |
| update-lit | prepared | 1 | 20.04 | 20.31 | 19.96 | +0.28 | -0.14 | -0.27 | 0 | True |
| update-lit | prepared | 2 | 20.25 | 20.31 | 20.18 | +0.04 | -0.11 | -0.16 | 0 | True |
| update-lit | prepared | 3 | 20.09 | 20.03 | 20.06 | -0.03 | -0.11 | +0.07 | 0 | True |
| update-lit | prepared | 4 | 20.13 | 20.10 | 20.03 | -0.05 | -0.10 | +0.02 | 0 | True |
| update-lit | prepared | 5 | 20.32 | 20.11 | 20.21 | -0.25 | -0.24 | +0.03 | 0 | True |

Pooled, as the median over six lifetimes of each lifetime's median (range over
lifetimes beside it):

| shape | path | n lifetimes | mean A server CPU (µs) | A2-A median (range), µs | B-A median (range), µs | B-A2 median (range), µs |
|---|---|---|---|---|---|---|
| select-index | literal | 6 | 23.91 | -0.08 (-0.28..+0.06) | +0.50 (+0.43..+0.60) | +0.55 (+0.38..+0.66) |
| select-index | prepared | 6 | 23.23 | -0.01 (-0.18..+0.27) | +0.50 (+0.45..+0.62) | +0.57 (+0.37..+0.69) |
| select-pk | literal | 6 | 20.80 | -0.03 (-0.14..+0.18) | +0.40 (+0.25..+0.43) | +0.39 (+0.34..+0.46) |
| select-pk | prepared | 6 | 20.07 | +0.00 (-0.14..+0.28) | +0.44 (+0.22..+0.56) | +0.33 (+0.17..+0.51) |
| update-lit | literal | 6 | 21.01 | -0.02 (-0.18..+0.12) | -0.15 (-0.27..-0.09) | -0.14 (-0.18..-0.00) |
| update-lit | prepared | 6 | 20.17 | -0.04 (-0.25..+0.28) | -0.13 (-0.33..-0.10) | -0.07 (-0.27..+0.07) |

And the same pooled over every block of every lifetime (144 paired blocks a
cell, median [bootstrap 95 % interval of the median, 2,000 resamples, seeded]):

| shape | path | pair | median over 144 blocks | bootstrap 95 % interval |
|---|---|---|---|---|
| select-index | literal | A2-A | -0.055 µs | [-0.110 µs, +0.000 µs] |
| select-index | literal | B-A | +0.530 µs | [+0.430 µs, +0.590 µs] |
| select-index | literal | B-A2 | +0.540 µs | [+0.480 µs, +0.630 µs] |
| select-index | prepared | A2-A | -0.010 µs | [-0.080 µs, +0.100 µs] |
| select-index | prepared | B-A | +0.500 µs | [+0.470 µs, +0.580 µs] |
| select-index | prepared | B-A2 | +0.550 µs | [+0.475 µs, +0.620 µs] |
| select-pk | literal | A2-A | -0.020 µs | [-0.070 µs, +0.030 µs] |
| select-pk | literal | B-A | +0.340 µs | [+0.280 µs, +0.405 µs] |
| select-pk | literal | B-A2 | +0.380 µs | [+0.350 µs, +0.420 µs] |
| select-pk | prepared | A2-A | +0.015 µs | [-0.045 µs, +0.070 µs] |
| select-pk | prepared | B-A | +0.375 µs | [+0.290 µs, +0.450 µs] |
| select-pk | prepared | B-A2 | +0.390 µs | [+0.265 µs, +0.465 µs] |
| update-lit | literal | A2-A | -0.020 µs | [-0.085 µs, +0.030 µs] |
| update-lit | literal | B-A | -0.170 µs | [-0.245 µs, -0.095 µs] |
| update-lit | literal | B-A2 | -0.120 µs | [-0.180 µs, -0.050 µs] |
| update-lit | prepared | A2-A | -0.030 µs | [-0.090 µs, +0.045 µs] |
| update-lit | prepared | B-A | -0.180 µs | [-0.220 µs, -0.120 µs] |
| update-lit | prepared | B-A2 | -0.130 µs | [-0.170 µs, -0.020 µs] |


Throughput and latency per arm (QPS derived as 1,000,000 / the mean wall time
a statement over all six lifetimes, the client's included; percentiles are the
medians of the six lifetimes' values, 72,000 statements an arm and cell):

| shape | path | arm | mean wall time (µs) | QPS (1e6/mean wall) | p0 (µs) | p25 (µs) | p50 (µs) | p99 (µs, median of lifetimes' values) |
|---|---|---|---|---|---|---|---|---|
| select-index | literal | A | 56.45 | 17,715 | 49.2 | 52.0 | 53.3 | 74.4 |
| select-index | literal | A2 | 56.13 | 17,817 | 49.3 | 52.0 | 53.2 | 73.1 |
| select-index | literal | B | 56.92 | 17,569 | 49.8 | 52.6 | 53.8 | 74.9 |
| select-index | prepared | A | 52.67 | 18,986 | 46.2 | 48.8 | 49.9 | 68.9 |
| select-index | prepared | A2 | 52.52 | 19,039 | 46.1 | 48.7 | 49.8 | 68.8 |
| select-index | prepared | B | 53.20 | 18,795 | 46.7 | 49.3 | 50.4 | 69.8 |
| select-pk | literal | A | 53.34 | 18,749 | 47.5 | 49.5 | 50.4 | 69.3 |
| select-pk | literal | A2 | 53.29 | 18,765 | 46.7 | 49.5 | 50.3 | 69.1 |
| select-pk | literal | B | 53.77 | 18,599 | 47.8 | 49.8 | 50.7 | 69.8 |
| select-pk | prepared | A | 49.37 | 20,256 | 44.1 | 45.9 | 46.6 | 64.2 |
| select-pk | prepared | A2 | 49.48 | 20,212 | 44.3 | 45.9 | 46.6 | 65.3 |
| select-pk | prepared | B | 49.77 | 20,091 | 44.6 | 46.3 | 47.1 | 65.4 |
| update-lit | literal | A | 45.09 | 22,176 | 36.0 | 41.3 | 41.9 | 64.4 |
| update-lit | literal | A2 | 45.01 | 22,218 | 35.9 | 41.3 | 42.0 | 63.3 |
| update-lit | literal | B | 44.81 | 22,317 | 35.3 | 41.2 | 41.8 | 62.6 |
| update-lit | prepared | A | 41.59 | 24,042 | 36.7 | 38.1 | 38.7 | 60.2 |
| update-lit | prepared | A2 | 41.64 | 24,013 | 35.5 | 38.1 | 38.6 | 59.3 |
| update-lit | prepared | B | 41.57 | 24,053 | 35.1 | 38.0 | 38.6 | 59.2 |

**Reading.** On the two read shapes **B - A is positive in every one of the 24
lifetime-cells, +0.22 to +0.62 µs, pooled +0.34 to +0.53 µs; the
same-binary floor A2 - A is -0.28 to +0.28 µs in any one lifetime and
pooled -0.08 to +0.00 µs (median of lifetimes) and -0.06 to +0.02 µs (over blocks, intervals within [-0.11, +0.10] µs)**. B - A is outside the floor on
`select-pk` and `select-index` on both paths: 1.6 to 2.2 % of a statement's 20 to
24 µs of server CPU (derived), and 0.8 to 1.0 % of its wall time (QPS
18,749 to 18,599, 20,256 to 20,091, 17,715 to 17,569, 18,986 to 18,795, A against
B). The smallest single B - A (+0.22 µs, `select-pk` prepared, lifetime 3) is
inside the largest single control offset (0.28 µs), so the claim rests on the
sign in all 24 lifetime-cells and on the pooled intervals, not on one lifetime. **The point write, `update-lit`, is -0.09 to -0.33 µs, pooled
-0.13 to -0.18 µs (B cheaper)**, which is outside the *pooled* control's
interval but inside the +-0.25 to 0.28 µs that A2 - A reaches in a single
lifetime: not claimed as a speed-up, and it is the write path that DE does not
arm, so it is consistent with the read-side delta being DE's arming of a `SELECT`
body (a thread-local note set and dropped, DE-S1's `SelectRun::Execute`), not
the idle threads or the wake descriptor (which a write would pay too).
**Binary layout** moves a statement by 0.2 to 0.3 µs on this host (the bisect
file's finding), so +0.34 to +0.53 µs is not shown to be DE's own cost; it is
the size of what a layout change and DE's code could each be.

DE-Q14's two departures from G2 (the registry with core 0's descriptor,
DE-R8's read threads), priced on the resident `cores = 1` cell i: ON_CPU of
the window is 0.618 and 0.602 s in B against 0.617 and 0.599 s in A
(rounds 0 and 1; 3.09 and 3.01 % of the core against 3.09 and 2.99 %), the
core's idle block 18.85 and 18.88 s against 18.85 and 18.89 s, the hot
sessions' p50 and p95 equal (section 5.1). **At 1,000 reads/s the idle
machinery costs nothing visible** (a difference of 0.0 to 0.003 s in 20 s of
CPU, 0 to 0.5 %, the cell's own two-round A spread being 0.018 s). It is the
per-statement figure above, not the idle one, that has a price. (Section 13
re-measures this figure for B and for variants of it; its conclusion, that
the shift is not the idle machinery and is not separated from DE's
per-statement code or from layout, supersedes nothing here.)

## 10. Does the data contradict the order?

1. **DE-R4, "a hit pays nothing new", read at the milestone level, is not
   what the resident cells show.** The pool's hit path is untouched by
   design, but a resident point `SELECT` is +0.34 to +0.53 µs of server CPU
   in B (section 9), a resident full-relation walk is 7 to 9 % slower per slice
   at equal CPU (section 8), and the cell-level percentiles hide both. The
   order's close bar, worded on percentiles, is met at p50 and p95; the CPU is
   where the cost is.
2. **DE-S1's (and DE-R9's) "one `io.page_read` a device read" holds; the pool's
   miss counter is 2.00 a cold read in B**, and its hit counter four above A's
   (section 7). Nothing in §7 claims otherwise; anything that reads
   `pool_misses` as device reads (an operator's dashboard, `sys.meta`) is
   wrong in B.
3. **DE-S1's smoke run "showed the park, the thread's read and the wake working"
   is confirmed and sized**: one park and one re-run per cold read in iii,
   about 5 % of a scan's reads in iv, 41 to 49 µs of core a cold read.
4. **DE-S2's claim that the walk "parks at a leaf whose read would wait" is
   confirmed, and the scan is no faster or slower for it** (section 8),
   which §7 does not claim either way; the order's own premise that the
   scanner's problem was the stall (DE-S0: 93 % of the core in reads, a
   3.0 ms slice) is resolved on the hot sessions and not on the scan.
5. Nothing found contradicts DE-R1 (`errors` is 0 in all 36 cell JSONs and
   every part-2 reply tag was right) or DE-R7 in iii (refusals about equal the
   cold statements, so none went past its first re-run); no answer was
   compared byte for byte here, the suite is DE's check on DE-R1.

## 11. Against this engine's own previous number, and the DE-S0 consistency check

The baseline is A measured again, interleaved (sections 4 to 9). DE-S0's A
(`bench/v3.4.13/results-de-s0-premise-v3.4.13-19-g35086008.md`, the same
commit's binary, earlier relation state, a cold rate of 648.1 rather than
650) is a check on the baseline: iii hot p95 from the schedule 792.5 and 796.6
µs there, 747.9 to 819.9 µs here; iii hot p99 1,087.7 and 1,263.1 µs, 1,009.0
to 1,022.1 µs (the 188 ms round aside); read share 27.52 and 27.01 %, 24.90
to 26.07 %; mean per read 550.9 and 540.2 µs, 496.7 to 514.0 µs (8 % lower
now); iv hot completed 9,777 and 10,887, here 10,892 to 10,908; iv scanner
395,144 and 395,720 reads, here 395,848 and 395,912; vscan p50 268.0 and 276.6
µs, here 280.4 and 282.8 µs; i p50 251.1 and 247.7, here 262.0 and 257.0 µs
(+4 %). The baseline reproduces to within the spread DE-S0 recorded, with the
device's per-read time about 8 % shorter and the resident cells about 4 %
slower than DE-S0's session, which the DE-S0 file could not have predicted and
this one does not attribute. **No second delta is built from DE-S0.**

## 12. What this teaches about the engine

The never-miss sessions were being held by *a core*, and DE-S1 and DE-S2 gave
the core back: a read that the kernel would block on is handed to a thread and
the statement parks, so the reactor's idle block goes from 13.0 to 17.8 s of 20
in (iii) and from 0.0 to 15.4 s in (iv), and the hot sessions' p95 returns to
the resident cells' (358 to 386 µs against 349 to 368 µs in cells i, ii and v) from A's 748 to 820 µs, their schedule from 5.75 s behind to on
time. What the park does *not* do is make a read faster: the device read is
the same 473 to 514 µs (probe p50 439 µs), the cold session's own read costs
it 2 to 3.4 % more (13 to 32 µs), and the core pays 41 to 49 µs more to run
the statement twice and wake it. A scan stays at the same 162 MB/s in both
arms, which says it is bound by one queue-depth-1 device read at a time (inferred); what changed is whom the
wait is charged to.

The price lands on the path that did not need the change: **resident point
reads cost about 0.4 µs of server CPU more (1.6 to 2.2 %) and a resident walk
about 50 ns a page**, both measured against a floor that excludes them, one
build layout and 0.2 to 0.3 µs apart from being proven DE's own. Neither
changes a percentile a client can see at 1,000 reads/s. At the saturation this
engine is built for, 1.6 to 2.2 % of CPU on the most common statement is the
number to weigh against a 2.1 to 2.3-fold p95 and 2.5-fold p99 stall removed when a
pool misses at all (the order's DE-Q1 (a) and DE-Q14 are where the operator
weighs it).

What is left blocking a core, by this run and the order's own §6: **writes**
(never armed; DE does not move their misses, and this run measured none),
the fetch that follows a park (it is synchronous by design and succeeds from
the page cache), every fetch under a walk that is not its leaf (an index probe
inside a join walk, a spilled value, a version chain, the undo), a descent
that misses more than eight pages (it reads the rest on its core), and a
read the kernel *can* hand over without blocking, which is 95 % of a scan's
reads in iv (92.6 % of them return in under 4 µs). A host with a different
page-cache state, or a device that does not have 439 µs reads, would size
none of this the same way.

## 13. The spike after the close: what the two resident costs are not, and the remedy

**Finding.** Section 12's two prices (a resident point `SELECT` about 0.4 µs
of server CPU, a resident walk about 50 ns a page) were taken to a knob spike
on `de-page-read-off-the-reactor`, and a remedy was applied. The spike
**rules out the page reader, the armed descent and the one-core wake table as
the point cost, and finds that a restructured page loop plus three trims
brings `vscan` to within about 1 % of A. It does not isolate what B's `vscan`
loss was, and it does not remove the point cost.** Everything below is
Release, `cores = 1`, durability and pool as in section 1, interleaved, one
fresh server and data file per cell as in the close; the drivers are the
close's (`tools/de_premise.py` for `vscan`, `part2-and-runners/p2.py` with
`bn_census.py` unmodified for part 2).

### 13.1 The arms

| Arm | What it is | sha256 of the binary copy |
|---|---|---|
| A | `35086008`, `v3.4.14` | as section 1 (`78b3aede...`) |
| B | `1942cb9a`, `v3.4.14-6-g1942cb9a` | as section 1 (`ca767bfb...`) |
| K1 | B plus the walk's page loop restructured so its leaf-visit routine has one call site (`src/exec/step_vm.cpp`) | `23d06ee45d6ba6db4aeed7af18d51e948a6b9b407fb97da78efdf0cf75824f81` |
| K2 | K1 with no descent ever armed (`CommandDispatcher::MissRefusalAdmitted` answers false); throw-away edit (`mut.py K2`) | `73d19c76a3773481095310cfa696a6de1dcc6abe404411bc494f766f1fa86fe0` |
| K3 | K1 with no page reader built at all: no reader threads, no one-core wake table, every dispatcher's reader null; throw-away edit (`mut.py K3`) | `b9c67ca646be05a0b834c37ad7d5ea20ff840dbf5e161be4b849583a5ed503d6` |
| K5 | K1 plus three always-on trims: the walk's park predicate built only for a walk that can park; `PageReader::threaded()` an atomic read instead of a lock; the per-statement note cleared once, where it is read | `3bb495965dc7d5059c69a32233d784283bc7d8bd023deae3deb927926a9d9f3c` |

**K5 is not exactly the closing code.** After the spike, the close's review had
one unconditional thread-local store put back at every statement's head
(`storage::DisarmMissRefusal()` in `StageStatement`) and three comments
changed; that tree was not re-measured. Where this section says "the closing
code" it means K5 plus one thread-local store a statement, not re-measured.
Variants K1 to K3 were built 11:01:12 to 11:03:03 UTC (K1 45 s, K2 29 s, K3
28 s) from the worktree's dirty tree (`include/kds/sched/io_backend.hpp` and
`src/exec/step_vm.cpp` modified, restored after; the binaries are identified
by the hashes above); K5 was built 11:14:15 to 11:14:50 UTC (35 s). Raw output
and the runners are in
`archive/de-close-v3.4.14-6-g1942cb9a/spike/` (`round1/`, `round2/`,
`spike.sh`, `spike2.sh`, `build_k.sh`, `mut.py`, `vs.py`, `p2an.py`, and the
status files).

### 13.2 `vscan`: the resident scan

K5's 20 s window completes within about 1 % of A's pages; K1 and K3 sit 3.6 and
2.7 % under A and B 8.1 % under. The pool hits in the window are pages
fetched, i.e. the scan's speed (derived rate: hits / 20 s):

| Arm | Round | Runs | Pool hits in 20 s window (M pages) | `cpu.on_cpu` (s) | Mean vs A (derived) |
|---|---|---|---|---|---|
| A | 1 | 3 | 37.66, 37.76, 38.01 | 19.216 to 19.234 | base (mean 37.81 M) |
| B | 1 | 3 | 34.31, 34.96, 34.97 | 19.265 to 19.294 | -8.1 % |
| K1 | 1 | 3 | 36.27, 36.50, 36.62 | 19.236 to 19.253 | -3.6 % |
| A | 2 | 2 | 37.56, 38.02 | 19.199, 19.222 | base (mean 37.79 M) |
| K3 | 2 | 2 | 36.68, 36.87 | 19.229, 19.231 | -2.7 % |
| K5 | 2 | 2 | 37.30, 37.64 | 19.213, 19.238 | -0.8 % |

(Ranges per arm, both rounds for A: 37.56 to 38.02 M. Round 1 ran A, B and K1
three times with the arm order rotated, round 2 ran A, K3 and K5 twice; each
arm's means are against the same round's A.) The never-miss sessions'
percentiles from the schedule did not move, which is why they are not where
the cost shows:

| Arm | Round | p50 (µs) | p95 (µs) | p99 (µs) | Reads per run |
|---|---|---|---|---|---|
| A | 1 | 266.3 to 274.7 | 333.5 to 346.9 | 367.2 to 390.5 | 20,003 to 20,004 |
| B | 1 | 270.4 to 273.2 | 336.9 to 343.8 | 367.6 to 380.8 | 20,004 |
| K1 | 1 | 270.6 to 272.8 | 340.0 to 342.1 | 378.4 to 392.9 | 20,003 to 20,004 |
| A | 2 | 272.0 to 275.4 | 340.7 to 344.0 | 376.0 to 379.3 | 20,003 to 20,004 |
| K3 | 2 | 270.6 to 275.6 | 336.6 to 347.9 | 366.7 to 419.9 | 20,003 to 20,004 |
| K5 | 2 | 269.2 to 280.4 | 337.1 to 348.1 | 370.0 to 383.6 | 20,003 to 20,004 |

What the numbers support, and what they do not:

* **K5 is within about 1 % of A with the ranges overlapping, two runs each.**
  That is not "within A's spread": one K5 run (37.30 M) is below A's minimum
  (37.56 M).
* **This is not a lost inlining, remedied.** `nm` on the binaries shows the
  leaf-visit lambda out of line, and the same size, in both A and B, and inlined
  into the walk's coroutine in K1 and K5. B therefore did not lose an inlining
  A had; the one-call-site form gains one A never had.
* K1, the loop change alone, is still 3.6 % under A; K3, the same code with the
  park branch never taken (no reader at all), is 2.7 % under. The step from
  K1 or K3 to K5 is three per-statement changes that cannot move a per-page
  rate over a handful of scans, so that step is code layout or run-to-run
  variance, and the same variance bounds the remedy claim. **What B's 8 % was
  is not isolated.**
* CPU is equal in every arm (19.2 to 19.3 s of 20 s): the arms differ in pages
  done per second of core, not in how much core they were given.

### 13.3 The point `SELECT`: server CPU a statement

Part 2 ran four server lifetimes a round, shapes `select-pk` and
`select-index`, literal and prepared (1,000 rows, as section 9). Round 1 ran A,
A2 (A's binary again, the noise floor), B, K1, K2, K3; round 2 ran A, A2, B, K5.
The table is the median over lifetimes of each arm minus A in the same
lifetime, in µs of server CPU a statement (from `p2an.py`; A's own CPU, in the
last column, is the range over lifetimes):

| Arm | `select-pk` literal | `select-pk` prepared | `select-index` literal | `select-index` prepared | Across the shapes | Round |
|---|---|---|---|---|---|---|
| A (base), µs per statement | 21.0 | 20.1 to 20.5 | 24.0 to 24.3 | 23.1 to 23.5 | | 1 |
| A2 - A | -0.14 | -0.10 | -0.18 | -0.08 | -0.18 to -0.08 | 1 |
| B - A | +0.33 | +0.44 | +0.49 | +0.55 | +0.33 to +0.55 | 1 |
| K1 - A | +0.38 | +0.60 | +0.69 | +0.64 | +0.38 to +0.69 | 1 |
| K2 - A | +0.26 | +0.41 | +0.44 | +0.48 | +0.26 to +0.48 | 1 |
| K3 - A | +0.28 | +0.37 | +0.42 | +0.49 | +0.28 to +0.49 | 1 |
| A (base), µs per statement | 20.7 to 20.8 | 19.9 to 20.3 | 23.8 to 24.1 | 23.1 to 23.4 | | 2 |
| A2 - A | +0.00 | +0.02 | -0.12 | -0.10 | -0.12 to +0.02 | 2 |
| B - A | +0.39 | +0.44 | +0.51 | +0.66 | +0.39 to +0.66 | 2 |
| K5 - A | +0.35 | +0.21 | +0.25 | +0.41 | +0.21 to +0.41 | 2 |

Across both rounds: B - A medians are +0.33 to +0.66 µs, K1 +0.38 to +0.69,
K2 +0.26 to +0.48, K3 +0.28 to +0.49, K5 +0.21 to +0.41, and the same-binary
floor A2 - A is -0.18 to +0.02 µs in medians with single lifetimes down to
-0.59 µs. As a share of a statement's server CPU (derived against A's own
CPU in the table's first rows): B is 1.6 to 2.8 %, K3 1.3 to 2.1 %, K5 1.0 to
1.8 %.

* **K3 carries the same shift as B within noise, so the shift is not the
  page reader, the armed descent, or the one-core wake table.** K3 has no
  reader, no arm and no wake table, and its range (+0.28 to +0.49 µs) lies
  inside B's (+0.33 to +0.66 µs) and K2's.
* **Nothing distinguishes K5 from B on the point statement**: the ranges
  overlap and the difference between them is smaller than the spread of the
  same binary measured twice (A2 - A single lifetimes to -0.59 µs).
* The shift is **not separated from DE's per-statement code that every variant
  still runs** (the refusal's admission test, the note's read, a larger
  `DispatchOutcome` and `SelectRun`) **or from binary layout**, which this host
  with no PMU cannot tell apart (layout alone moves a statement by 0.2 to 0.3
  µs, section 9).

### 13.4 What the spike changes about the close's conclusions

Section 10's item 1 stands as B's: a resident point `SELECT` is dearer in B and
a resident walk 7 to 9 % slower. With the spike's data: the walk runs within about 1 % of A at K5 (two
runs), K1 and K3 are still 2.7 to 3.6 % under, the step from them to K5 is
layout or run-to-run variance, and the cause of B's loss is not isolated. The
point statement's shift is not the page reader, the arm or the wake table, and
the 1.0 to 1.8 % of statement CPU K5 still carries is DE's per-statement code
or layout, to be weighed as section 12 weighs it against the stall removed.
The closing code was not measured: the one thread-local store a statement
that the review put back is a cost nobody has priced here.

### 13.5 What was not done

* No third round of either part, no further `vscan` run of B or K5 beyond the
  counts above.
* No variant isolating struct sizes (`DispatchOutcome`, `SelectRun`) from
  layout.
* Writes are not in the spike (`update-lit` was not re-run); the cold path
  (iii, iv) was not re-measured on any variant.
* The closing code itself was not measured.

### 13.6 Time

Variant builds 11:01:12 to 11:03:03 UTC (1 min 51 s) and K5's 35 s; round 1
about 11 minutes, round 2 about 7 minutes. With the close's own 28 minutes of
measurement and 1 min 46 s of builds, the milestone's close measurement comes
to about 50 minutes, inside the hour's budget. The quiet gate's holds, as in
the close, are outside it.

## 14. Limits and cuts

* **Two rounds per cell** (four for iii and iv; the extras are in the tables).
  The resident verdict at p99 and above is not supported; section 5.1 states
  which percentiles carry a claim.
* One relation (900,000 rows, loaded once at DE-S0 by A's binary and mounted
  by both arms, never reloaded), one pool ratio (a quarter, and the whole),
  one device (`/dev/nvme0n1p1`, a cloud persistent disk, 439 µs p50 reads),
  one cold rate (650 reads/s, 0.3 of a core over the probe), one host.
* iii4 has no `cores = 4` resident baseline and its clients are on different
  CPUs from DE-S0's. `cores > 1` is measured only there; the order's two-core
  behaviour (the park on another core's page, the wake registry) is not
  measured beyond it.
* **Not measured**: writes' misses (DE does not move them), a host without
  the memory-limited scope, `strict` durability, PostgreSQL (the floor, not
  measured for this shape), a row-count sweep (the relation is fixed;
  part 2 is at 1,000 rows), a `cores = 1` run of the re-run's cost against a
  deadline-bound park (a read that takes a fault net's whole second), a
  `tsan` or nightly run (the order's §7 says not executed).
* Part 2's probes: 200 reads per cell run, pooled as a median of
  per-probe percentiles because the raw samples are not kept.
* The binary copies' mtimes are the copy time; the builds' times are not
  recorded here (the caller's).
* The tree was dirty with documents, `io_backend.hpp` and the driver
  (section 1); the binaries are the caller's clean-tree builds, identified by
  hash.
* Cut for the budget: nothing planned was cut. No part-2 write shape beyond
  `update-lit` (no insert, no delete), and no `A2` arm in part 1 (part 1's noise
  floor is the spread between rounds of one arm).
