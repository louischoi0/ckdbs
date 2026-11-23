# DQ close - a correlated Cabin probe's miss answered from the build

Measured 2026-11-23 between 01:54 and 02:51 UTC on worktree
`dq-cabin-miss-build` at `7e31d272b` (`v3.5.0-101-g7e31d272b`), against the
commit DQ opened from, `68e5a69fd` (`v3.5.0-96-g68e5a69fd`). Order:
`instructions/v3.2.0/workorder-dq-cabin-miss-build.md` §3 (DQ-R6), §4
(DQ-Q8), §5 (DQ-S5). Spec: `docs/spec/join-inner-build.md` §5a. Drivers:
`tools/dq_close.py` (M1, new), `tools/dq_s0_premise.py` and
`tools/dq_s0_report.py` (M2; `dq_s0_premise.py` ran M2 unchanged and gained
`--build-cap` and `--gap-s` for the diagnostics below). Raw output, scripts,
heartbeat and watchdog files:
`bench/v3.5.0/archive/dq-close-v3.5.0-101-g7e31d272b/`.

| | |
|---|---|
| date and time executed | 2026-11-23 01:54 to 02:51 UTC (M1 01:54-01:56 and 02:02-02:05, M2 01:56-02:01, M2 floor and follow-up 02:06-02:10, the cap-0 diagnostic 02:14-02:22, B2's M1 and cell 02:24-02:37, the gap diagnostic 02:39-02:51; heartbeat files) |
| branch, commit | `worktree-dq-cabin-miss-build`, B = `7e31d272b` (`v3.5.0-101-g7e31d272b`); A = `68e5a69fd` (`v3.5.0-96-g68e5a69fd`) |
| tree | B's source tree is clean: `git status` shows `CLAUDE.md` and `docs/spec/join-inner-build.md` modified (documents) and `tools/dq_close.py` untracked; nothing under `src/`, `include/`, `tests/` |
| binary A | copy `/home/louis/dqbench/kds_server_A_68e5a69f`, sha256 `45eb735a8626c211f9e87e0ec65afd8e858d3373de63fc879f3184809b1b325b`; the copy's mtime is 2026-11-23 00:18:38 UTC (the copy's time; A's commit is 00:08:03). The source binary was built at `68e5a69fd` per the order and has since been overwritten by B's build, so its own mtime cannot be read |
| binary B | copy `/home/louis/dqbench/kds_server_B_7e31d272`, sha256 `9a5ac8d83af20781c65ba035aa8ac5a3aeb130d90ab7d10ad7ea97f4a0af00d3`, copy's mtime 01:52:20 UTC; the source `build-release/kds_server` has mtime 01:52:01 UTC, after HEAD's commit time 01:51:07 UTC, so the binary is not older than HEAD |
| build type | Release (`build-release` of this worktree) |
| device | data files on `/dev/nvme0n1p1`, ext4 (`df -T`), not tmpfs; ports 15781-15783 (M1), 15991-15997 (M2), chosen |
| server configuration | `cores = 1`, `durability = relaxed`, `join_order = written`, `buffer_pool_frames = 65536` (every page resident), all else default; M2 raises `join_build_max_rows` itself except in the 60,000-row cell |
| CPUs | M1: servers on CPUs 1, 2, 3 (distinct physical cores; map rotated per round), driver on CPU 0, SMT siblings 4-7 idle. M2: server on CPU 2, driver on CPUs 0 and 4 (`dq_s0_premise.py`'s own placement) |
| host | no `cc1plus`, `ctest`, `cmake`, `ld` or `kds_tests` process at any cell (the `/proc/*/comm` gate, polled before each M1 shape and M2 cell); loadavg 0.5 to 2.2, which is this run's own server and driver. Quiet gate held 0 s. |
| budget | about 46 minutes of runs inside 57 minutes of wall time (01:54:35 to 02:51:17), B2's build inside that window; B's build, before 01:52, not timed. What was cut: M2's 100,000-row A cell ran 1 round as the order allows, and the per-thread split was not taken |

## The answer first

**DQ does what its order says; where it adds nothing it costs nothing
measurable on a point read and +0.2 to +0.5 us (0.5 % to 1.2 %) on a join
statement.** The Agate arm (the banking gate declining every recording) now
costs 0.79x to 1.15x of the same join with the Cabin dropped, statement by
statement, at every size measured; the bar was 1.5x. Against the engine DQ
opened from, the same cell is 108x to 133x cheaper (10,000 inner rows) and
186x to 251x cheaper (100,000 rows), and the correlated EXISTS 48x to 61x.
At the engine's defaults the cabined join at 60,000 inner rows is answered
at statement #1 (14.2 ms) where A refused it at both.

Two things the order's text should not be read past:

1. **At the defaults the cabined join is still refused at its second
   statement.** The recording statement, in which every miss is a recording
   touch, still walks once a key and is refused for the row-touch budget
   (4.87 s of server CPU, then refused). This is DQ-Q11 (`known-gaps.md`),
   not a DQ defect, and it is the only refusal left in the cells.
2. **A converged statement is not uniformly as cheap as before.** On the
   statement right after the recording statement (`unique`, 10,000 rows) B
   reads about 20 % dearer than A (19 % to 21 % in a follow-up, 26 % to 35 % in the first pair), four of four observations, outside
   the control's range. Statement #4 reads dearer in some runs (the cap-0
   diagnostic: B +123 us over A) and level in others; #5 and later are
   level back to back.
   - **Cause not isolated.** The three diagnostics below find:
     - with the build taken away (`join_build_max_rows = 0`), about two
       thirds of it follows B's statement #1 and one third the binary;
     - B2 (the helpers kept out of line) removes the binary third but adds
       1 % to 2 % to a whole-relation walk, and is withdrawn, so B stays the
       closing binary;
     - with a 1.5 s idle gap before every statement, B's converged
       statements read +7 % to +10 % dearer than A's (#3, #4 and #5), so it
       is not a periodic task landing in another statement's window.
   - It is largest when the state is coldest, which reads as memory or
     cache state; the mechanism is not isolated.
   - The close's measurement budget was spent here. The order's DQ-Q13
     records what was decided.

## M1 - DQ-R6: what DQ costs where it adds nothing

Method: three servers up at once (A, A2 = a second server of A's binary,
B), 15 server lifetimes (a first batch of 5, then a second of 10 run the
same way), each lifetime a fresh start of all three, a discarded first round
per shape, then 24 timed rounds with the arm order rotated and reversed, the
same statements in every arm of a round (equal work). Server CPU per
statement: `/proc/<pid>/task/*/schedstat` across every thread over the
block, read once no server thread is runnable (`settled_cpu_ns`). Shapes
(`tools/dq_close.py` docstring):

| shape | statement | block |
|---|---|---|
| a_point | `SELECT * FROM pt WHERE id = <k>`, 10,000 rows | 2,000 stmts |
| b_build | `SELECT o.id, i.w FROM outb o JOIN innb i ON i.k = o.k`, 16 outer rows, 1,000 inner rows, no Cabin (the build answers) | 300 stmts |
| c_cabin | the same join with `CREATE CABIN ON innc(k)`, timed only after `ANALYZE` showed `cabin_hits=16`, no misses (3 statements to get there; re-checked after every shape block, all 15 lifetimes and 3 arms) | 1,000 stmts |
| d_tuplekey | `SELECT o.id, i.w FROM outd o JOIN innd i ON i.id = o.k`, inner probed by its tuple key, 16 outer rows | 600 stmts |

Row counts agreed across the three arms in every block (1, 16, 16, 16 rows).
Sizes: M1's relations are fixed (10,000 / 16 x 1,000); the order sweeps
inner size in M2.

### Server CPU a statement, B minus A against the A2 minus A floor

All 15 lifetimes pooled: 24 rounds x 15 lifetimes = 360 round-pairs a shape.
Cells are server CPU per statement (microseconds; QPS = 1,000,000 / CPU for
a serial driver, so a_point is about 44,200 stmts/s of server time).

| shape | A | A2 | B | B-A | A2-A (the floor) | B-A as % of A | lifetimes with B-A > 0 | lifetimes with B-A inside the floor's lifetime range |
|---|---|---|---|---|---|---|---|---|
| a_point | 22.63 us | 22.59 us | 22.61 us | -0.06 us | -0.07 us (lifetime medians -0.38 to +0.58 us) | -0.24 % | 7 of 15 | 15 of 15 |
| b_build | 147.45 us | 147.23 us | 145.34 us | -1.95 us | -0.01 us (lifetime medians -2.52 to +1.46 us) | -1.32 % | 0 of 15 | 10 of 15 |
| c_cabin | 37.91 us | 37.54 us | 38.23 us | +0.33 us | -0.34 us (lifetime medians -1.27 to +0.69 us) | +0.87 % | 13 of 15 | 12 of 15 |
| d_tuplekey | 37.76 us | 37.65 us | 38.04 us | +0.26 us | -0.21 us (lifetime medians -1.04 to +0.85 us) | +0.69 % | 11 of 15 | 15 of 15 |

Per lifetime (the median over the lifetime's 24 rounds):

| lifetime | a_point B-A | a_point A2-A | b_build B-A | b_build A2-A | c_cabin B-A | c_cabin A2-A | d_tuplekey B-A | d_tuplekey A2-A |
|---|---|---|---|---|---|---|---|---|
| 1 | -0.21 us | -0.38 us | -3.61 us | -1.25 us | -0.55 us | -0.82 us | -0.16 us | -0.78 us |
| 2 | +0.03 us | -0.01 us | -2.59 us | +0.50 us | +0.24 us | -0.27 us | -0.08 us | -0.42 us |
| 3 | -0.22 us | -0.09 us | -2.25 us | -0.79 us | +0.72 us | +0.30 us | +0.40 us | +0.19 us |
| 4 | -0.10 us | -0.11 us | -1.73 us | -0.65 us | +0.76 us | -0.30 us | +0.51 us | -0.41 us |
| 5 | +0.29 us | -0.20 us | -1.38 us | +0.58 us | +0.61 us | +0.03 us | +0.45 us | +0.20 us |
| 6 | -0.05 us | +0.14 us | -3.18 us | +0.01 us | +0.68 us | +0.69 us | +0.68 us | +0.36 us |
| 7 | -0.13 us | -0.23 us | -0.78 us | -0.32 us | +0.48 us | -0.29 us | +0.51 us | +0.25 us |
| 8 | +0.27 us | +0.01 us | -2.42 us | +0.55 us | +0.10 us | -1.27 us | +0.49 us | -0.12 us |
| 9 | -0.30 us | -0.21 us | -0.87 us | +1.46 us | +0.30 us | -0.28 us | +0.60 us | +0.85 us |
| 10 | +0.26 us | +0.58 us | -0.01 us | +0.49 us | +0.83 us | -0.74 us | +0.26 us | -1.04 us |
| 11 | +0.27 us | +0.11 us | -1.76 us | -2.52 us | +0.21 us | -0.64 us | -0.71 us | -0.73 us |
| 12 | -0.13 us | -0.19 us | -3.27 us | -0.77 us | -0.02 us | -0.73 us | -0.02 us | -0.11 us |
| 13 | +0.06 us | +0.12 us | -1.76 us | +0.30 us | +0.00 us | -1.01 us | +0.13 us | -0.49 us |
| 14 | +0.02 us | -0.05 us | -4.75 us | -0.04 us | +0.18 us | -0.53 us | +0.44 us | -0.00 us |
| 15 | -0.06 us | +0.03 us | -1.19 us | -0.57 us | +0.52 us | +0.12 us | +0.22 us | -0.65 us |

How the bar reads (DQ-R6: B minus A inside the A2 minus A floor):

- **(a) point read, (d) tuple-key join: met.** B-A is -0.06 us and +0.26 us
  against floor lifetime ranges of about +-1 us; a_point's B-A is 0.0 within
  noise, d_tuplekey's is inside the range in 15 of 15 lifetimes.
- **(b) build join: met, and B is faster.** B-A is -1.95 us (-1.3 %) with no
  positive lifetime in 15, and 5 of 15 lifetimes below the floor's range.
  A faster `BuiltWalk` dispatch is a candidate cause; not isolated, so not
  asserted.
- **(c) the all-served Cabin join: inside the floor's range by the pooled
  figure, not shown to be zero.** B-A is +0.33 us (+0.9 %); 13 of 15
  lifetimes read positive, 3 (0.72, 0.76, 0.83 us) above the top of the
  floor's lifetime range (+0.69 us) while the floor itself reads -0.34 us
  (A2 is a touch faster than A, a measured asymmetry of this arm/CPU
  design). Against A2 rather than A, B is +0.63 us (14 of 15 positive).
  Reading it conservatively, DQ's cost where every key is served is up to
  about +0.3 to +0.7 us a statement (16 probes), 0.9 % to 1.7 %. The same
  +0.26 us (0.7 %) appears on d_tuplekey, which has no Cabin step, so the
  cost is in the join statement's compile or dispatch path, not in the
  Cabin hit path. What DQ changed that both statements run (read at
  `7e31d272`): `RunStep`'s coroutine, into which `BuiltWalk` is inlined, and
  at compile `BuildAnnotationOf` on every walked step, which asks
  `HasUnindexedEqualityFilter` a second time (`step_compiler.cpp:3602`,
  `:2003`); c_cabin's Cabin step also runs `BuildKeyOf` at every compile.
  Neither runs `BuildIsArmable`, and `Step`'s layout did not move. A
  one-test annotation cost per miss cannot be it, since the hit path has no
  miss.

### Client wall latency per statement, with the server CPU's share

Pooled over 15 lifetimes, every 7th statement sampled (`lat_sample_us`);
24 rounds x block size x 15 lifetimes timed statements a cell. The three
arms are level in every percentile.

| shape | arm | statements timed | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| a_point | A | 720,000 | 46 us | 48 us | 49 us | 61 us | 70 us |
| a_point | A2 | 720,000 | 46 us | 48 us | 49 us | 61 us | 70 us |
| a_point | B | 720,000 | 45 us | 48 us | 49 us | 61 us | 70 us |
| b_build | A | 108,000 | 167 us | 174 us | 178 us | 323 us | 428 us |
| b_build | A2 | 108,000 | 168 us | 174 us | 177 us | 317 us | 422 us |
| b_build | B | 108,000 | 166 us | 172 us | 175 us | 312 us | 419 us |
| c_cabin | A | 360,000 | 65 us | 68 us | 70 us | 90 us | 108 us |
| c_cabin | A2 | 360,000 | 64 us | 68 us | 69 us | 89 us | 106 us |
| c_cabin | B | 360,000 | 65 us | 68 us | 70 us | 90 us | 110 us |
| d_tuplekey | A | 216,000 | 65 us | 68 us | 69 us | 88 us | 106 us |
| d_tuplekey | A2 | 216,000 | 64 us | 68 us | 69 us | 88 us | 105 us |
| d_tuplekey | B | 216,000 | 64 us | 68 us | 69 us | 88 us | 106 us |

**Waits.** Each statement is a read on an idle, all-resident `cores = 1`
server from one session: no fsync (reads, `relaxed`), no lock wait, no page
read wait. The only decomposition available is server CPU against the rest
(client, socket round trip, wakeup), from this table and the one above, at
B: a_point 22.6 of 49 us p50 (46 % server CPU), b_build 145.3 of 175 us
(83 %), c_cabin 38.2 of 70 us (55 %), d_tuplekey 38.0 of 69 us (55 %). The
remainder is not split further; `sys.wait_stats` was not read.

## M2 - DQ-S0's cells run again

`tools/dq_s0_premise.py --bin <A or B>`, the order's flags. Medians over
rounds, each cell a fresh server. Join: `ANALYZE SELECT o.id, i.w FROM outr
AS o JOIN inn AS i ON i.k = o.k`, 1,000 outer rows (2,000 in the 60,000-row
cell), one inner row a key. The inner size is `--sizes`, the outer
distribution `--dists` (`unique`, `repeat`, `skewed`). Rounds: 10,000 rows
3 rounds x 5 statements, both binaries; 100,000 rows with
`--max-rows-touched 1000000000`, A 1 round (cells take 4.5 s a statement),
B 3 rounds, 5 statements; 60,000 rows with `--outer 2000
--engine-build-cap --dists unique`, 1 round, 2 statements, the engine's
defaults; `--shape exists` at 10,000 rows, 3 rounds, 5 statements. Full
per-statement tables: `archive/.../table-{A,B}-{10k,100k,60k,exists10k}.md`.

### DQ-Q8: the Agate arm against the Cabin-dropped arm (B binary)

**Met at every size, with the margin large: 0.79x to 1.15x per statement,
the 1.5x bar never approached.** Server CPU a statement, Agate over B-arm,
same binary, same statement index. DQ-S0 predicted about 1.3x at 10,000 rows
and 1.05x at 100,000 rows; the measurement is 1.03x to 1.06x pooled at
10,000 rows and 0.95x to 1.04x at 100,000 rows. The bookkeeping of 1,000
declined misses is therefore not measurable above the build's own cost
(3.2 ms and 18 ms): the prediction of 1.3x at 10,000 rows was high.

| cell | outer keys | Agate / B-arm, statements #1 to #5 | pooled median ratio | statements |
|---|---|---|---|---|
| 10,000 rows, join | repeat | 1.15x, 1.05x, 1.05x, 1.05x, 1.07x | 1.06x | 15 |
| 10,000 rows, join | skewed | 0.80x, 0.79x, 0.89x, 1.07x, 1.00x | 0.92x | 15 |
| 10,000 rows, join | unique | 1.06x, 1.02x, 1.02x, 1.06x, 1.04x | 1.03x | 15 |
| 100,000 rows, join | repeat | 1.03x, 1.08x, 1.04x, 1.05x, 1.06x | 1.04x | 15 |
| 100,000 rows, join | skewed | 0.99x, 0.97x, 0.97x, 0.98x, 0.98x | 0.98x | 15 |
| 100,000 rows, join | unique | 0.96x, 0.98x, 0.94x, 0.94x, 0.95x | 0.95x | 15 |
| 10,000 rows, EXISTS | repeat | 1.09x, 1.11x, 1.04x, 1.06x, 1.06x | 1.08x | 15 |
| 10,000 rows, EXISTS | skewed | 1.08x, 1.04x, 1.06x, 1.06x, 1.05x | 1.07x | 15 |
| 10,000 rows, EXISTS | unique | 1.01x, 1.02x, 1.02x, 1.07x, 1.06x | 1.05x | 15 |
| 60,000 rows, join, defaults | unique | 1.03x, 1.06x | 1.04x | 2 |

The 0.79x to 0.89x readings on `skewed` at 10,000 rows come from a noisy B arm
(its statements #1 to #3 read 5.11, 4.60, 4.26 ms against 3.3 ms later, max
5.98 ms), not from Agate being cheaper.

### The before/after: Agate on A against Agate on B

Server CPU a statement, medians, A binary to B binary. A's Agate walks the
inner relation once per outer row (1,000 walks); B's answers from the map.

| cell | outer keys | A, statements #1 to #5 | B, statements #1 to #5 | speedup range |
|---|---|---|---|---|
| 10,000 rows, join | unique | 448.93, 447.92, 442.64, 442.80, 441.74 ms | 4.11, 3.57, 3.64, 3.58, 3.51 ms | 109x to 126x |
| 10,000 rows, join | repeat | 448.15, 448.47, 456.11, 448.85, 453.58 ms | 4.16, 3.46, 3.54, 3.38, 3.41 ms | 108x to 133x |
| 10,000 rows, join | skewed | 447.72, 443.56, 442.07, 444.44, 445.63 ms | 4.10, 3.62, 3.79, 3.54, 3.52 ms | 109x to 127x |
| 100,000 rows, join | unique | 4,501.24, 4,509.30, 4,629.56, 4,499.64, 4,472.37 ms | 23.94, 18.46, 19.22, 18.85, 18.92 ms | 188x to 244x |
| 100,000 rows, join | repeat | 4,539.33, 4,539.82, 4,466.78, 4,575.61, 4,536.24 ms | 24.41, 19.07, 19.34, 19.06, 19.72 ms | 186x to 240x |
| 100,000 rows, join | skewed | 4,498.63, 4,557.43, 4,628.38, 4,559.86, 4,508.26 ms | 23.89, 18.12, 19.01, 19.10, 18.44 ms | 188x to 251x |
| 10,000 rows, EXISTS | unique | 228.23, 229.42, 225.99, 229.99, 228.93 ms | 4.39, 3.94, 4.08, 4.02, 3.97 ms | 52x to 58x |
| 10,000 rows, EXISTS | repeat | 240.95, 238.84, 239.45, 238.32, 237.22 ms | 4.52, 4.02, 4.00, 3.91, 3.95 ms | 53x to 61x |
| 10,000 rows, EXISTS | skewed | 220.67, 217.43, 215.80, 219.63, 219.22 ms | 4.63, 3.96, 4.08, 4.00, 3.88 ms | 48x to 57x |

DQ-S0's reading of A was 442 to 456 ms and 4,498 to 4,525 ms for the join's
Agate arm; A reads 441.7 to 456.1 ms and 4,466.8 to 4,629.6 ms now. **A reads
the same.** Its other DQ-S0 figures reproduce too: `unique` #1 and #2 at
10,000 rows 444.2 and 495.4 ms (DQ-S0: 445.6, 491.2 ms), at 100,000 rows
4,495.8 and 4,920.5 ms (4,504.6, 4,942.1 ms); 100,000-row `repeat` #1
989 ms (970.6), `skewed` #1 and #2 2,420 and 2,429 ms (2,444 and 2,471);
converged wall p50 0.82 ms at 10,000 rows (0.89 ms) and 0.84 ms at 100,000
rows (1.16 ms; 10 statements only, so this is the thinner figure).

### At the engine's defaults (60,000 inner rows, 2,000 outer, 1 round)

**B answers the cabined join at statement #1, not at statement #2.** The
second statement of the A arm (every miss a recording touch) is refused
for the row-touch budget on both binaries.

| arm | A binary, statement #1 | A binary, statement #2 | B binary, statement #1 | B binary, statement #2 |
|---|---|---|---|---|
| Cabin (A arm) | refused after 4,659.5 ms | refused after 4,869.5 ms | answered, 14.19 ms | refused after 4,871.2 ms |
| Cabin, gate declining (Agate) | refused after 4,509.0 ms | refused after 4,492.9 ms | answered, 14.26 ms | answered, 11.19 ms |
| no Cabin (B arm) | answered, 14.68 ms | answered, 11.21 ms | answered, 13.83 ms | answered, 10.54 ms |

The refusal that remains is DQ-Q11: each recording touch walks the inner
relation (2,000 x 60,000 = 120,000,000 row touches against the default
100,000,000). It is an unchanged part of the recording path; DQ left it by
design (DQ-Q2 (a)). The 10,000-row recording statement reads 501.15 ms on B
against 495.4 ms on A (+1.2 %, 3 rounds against 3), and at 100,000 rows
5,011 ms against 4,920 ms (+1.9 %, 3 rounds against 1).

### A converged join: B against A, with a control

A converged statement is one with `cabin_hits` = 1,000, no misses and no
recordings, in the A arm, at 10,000 rows. The in-run control is a second run
of each binary on the same cell (the driver's fresh servers, the same
distribution seeds): A against A again, B against B again. The median
server CPU over the 30 converged statements of each run (10
distribution-statement groups x 3 rounds); wall percentiles over the 30.

| run | server CPU median | wall p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|
| join, A, run 1 | 707 us | 727 us | 770 us | 820 us | 1,080 us | 1,085 us |
| join, A, run 2 | 665 us | 688 us | 727 us | 758 us | 1,064 us | 1,103 us |
| join, B, run 1 | 827 us | 709 us | 835 us | 967 us | 1,369 us | 1,407 us |
| join, B, run 2 | 711 us | 698 us | 743 us | 798 us | 1,202 us | 1,233 us |
| EXISTS, A, run 1 | 1,116 us | 1,094 us | 1,167 us | 1,216 us | 1,494 us | 1,581 us |
| EXISTS, A, run 2 | 1,086 us | 1,087 us | 1,137 us | 1,184 us | 1,473 us | 1,494 us |
| EXISTS, B, run 1 | 1,101 us | 1,095 us | 1,138 us | 1,215 us | 1,583 us | 1,591 us |
| EXISTS, B, run 2 | 1,118 us | 1,115 us | 1,166 us | 1,216 us | 1,494 us | 1,530 us |

The control's range at this cell is 665 to 707 us (6 %) on the join and
1,086 to 1,116 us (3 %) on EXISTS. **The sub-chain (EXISTS) form is level.**
On the join, B reads 711 to 827 us: both runs lie above the control's
range, run 2 by 4 us and run 1 by 120 us. The spread is in the `unique` distribution, so a
follow-up ran `unique` alone at 10,000 rows, 8 statements and 8 rounds, A B
A B (`archive/.../m2u-*`), server CPU median per statement, A arm:

| statement | A run 1 | A run 2 | B run 1 | B run 2 |
|---|---|---|---|---|
| #3 (the first all-hit one) | 895 us | 936 us | 1,065 us | 1,135 us |
| #4 | 648 us | 850 us | 760 us | 811 us |
| #5 | 676 us | 721 us | 694 us | 734 us |
| #6 | 660 us | 846 us | 691 us | 707 us |
| #7 | 669 us | 719 us | 690 us | 706 us |
| #8 | 658 us | 665 us | 672 us | 684 us |
| #4 to #8, median | 657 us | 742 us | 695 us | 734 us |

- **From the second all-hit statement on, B is level with A**: B's 695 and
  734 us lie inside A's 657 to 742 us control range.
- **The first all-hit statement, #3, reads 19 % to 21 % dearer on B** (B
  1,065 and 1,135 us against A 895 and 936 us; in the first M2 run 1,257 us
  against 930 us, and in the floor run 1,116 us against 888 us, 26 % to 35 %), outside the
  control (A's two runs differ by 41 us). Four of four B readings above
  four of four A readings. `repeat` and `skewed` #3 do not show it.
  The cause is split by the diagnostic section below. The one difference in what the two
  binaries did before statement #3 in `unique` is that B's statement #1
  built the map and walked nothing, where A's walked the inner relation
  1,000 times; that would change what is warm, not what the hit path
  executes, but it was a hypothesis; the diagnostic below tests it.
- At 100,000 rows one statement #3 reads 4.5 to 4.8 ms in each binary
  (A `skewed` #3 4.50 ms; B `unique` #3 4.84 ms) where its neighbours read
  about 1 ms. It appears on both binaries, so it is not DQ's; cause
  unverified.

## Diagnostic - where statement #3's extra cost comes from

Question: on `unique` at 10,000 rows, B's first all-hit statement (#3) reads
about 20 % dearer than A's. Hypothesis: the difference is what B's statement
#1 does (builds a 10,000-row map and frees it, where A walks the inner
relation 1,000 times). Control: the B binary with `join_build_max_rows = 0`,
which takes the fallback away, so B's statement #1 runs A's executor path
(1,000 walks).

Driver change, recorded here: `tools/dq_s0_premise.py` gained `--build-cap N`,
which writes `join_build_max_rows = N` (0 takes the build away) and
overrides `--engine-build-cap` and the raised default. Nothing else changed.

Run (`m2d.sh`, archive `m2d-*`): four configurations, `--sizes 10000 --dists
unique --rounds 8 --stmts 5`, each a fresh process per cell, in three passes
with the configuration order rotated per pass (A B B0 Ar / B0 Ar A B / Ar A B
B0): **A**, **Ar** (A's binary again, the in-run floor), **B**, **B0** (B's
binary, `--build-cap 0`). The statistics below are the A arm (the declared
Cabin), 24 cells (3 passes x 8 rounds) a configuration. Counters agree with
the intent: statement #1 reads `inner_built=1` on B only, 1,000 misses and
no build on A, Ar and B0; #2 records 1,000 sets on all four; #3 to #5 are
1,000 hits on all four.

Statement #3 and #4, server CPU and client wall per statement; spread is the
p25 to p75 over the 24 cells, with the three pass medians beside it:

| statement | configuration | server CPU median | p25 to p75 | min to max | pass medians (CPU) | wall median | wall p25 to p75 |
|---|---|---|---|---|---|---|---|
| #3 | A | 921 us | 874 to 965 us | 851 to 1,067 us | 917, 913, 952 us | 1,050 us | 1,009 to 1,103 us |
| #3 | Ar (A again, floor) | 927 us | 885 to 981 us | 859 to 1,155 us | 920, 932, 933 us | 1,062 us | 1,033 to 1,129 us |
| #3 | B0 (B, cap 0) | 999 us | 978 to 1,057 us | 915 to 1,326 us | 1,018, 993, 983 us | 1,145 us | 1,105 to 1,188 us |
| #3 | B | 1,138 us | 1,094 to 1,185 us | 1,024 to 1,277 us | 1,129, 1,166, 1,131 us | 1,277 us | 1,214 to 1,349 us |
| #4 | A | 682 us | 661 to 723 us | 642 to 912 us | 686, 687, 680 us | 787 us | 760 to 817 us |
| #4 | Ar | 707 us | 668 to 766 us | 630 to 992 us | 736, 694, 698 us | 795 us | 762 to 862 us |
| #4 | B0 | 728 us | 682 to 750 us | 657 to 1,103 us | 731, 732, 709 us | 818 us | 790 to 851 us |
| #4 | B | 805 us | 734 to 935 us | 692 to 1,140 us | 784, 832, 880 us | 902 us | 837 to 1,063 us |

For reference, statement #5: A 654 us, Ar 688 us, B0 678 us, B 714 us (CPU
medians); statement #1: A 445.9 ms, Ar 448.0 ms, B0 449.8 ms, B 4.09 ms;
statement #2: A 490.8 ms, Ar 491.3 ms, B0 485.2 ms, B 486.5 ms.

Reading, against the order's two outcomes:

- **Neither outcome holds whole; the excess splits.** On #3, B is +214 us
  over A (+23 %; the floor A against Ar is -6 us). Taking the fallback away
  removes 139 us of it (B0 999 us against B 1,138 us, 65 % of the excess);
  **75 us remains** (B0 against A, +8 %), with B0's p25 (978 us) above A's
  p75 (965 us) and B0's three pass medians (983 to 1,018 us) above A's and
  Ar's (913 to 952 us). So about two thirds of the #3 cost follows the
  fallback's statement #1 (a run-time consequence of the feature: the state
  a 10,000-row map build leaves, not what the hit path executes), and about
  one third is carried by the B binary whatever statement #1 did (its code or
  layout). The mechanism of either part was not isolated: this run cannot
  tell the allocator or cache state of the freed map from where #2's sets
  landed.
- **The wall time reads the same split**: #3 wall is 1,050 us (A), 1,145 us
  (B0), 1,277 us (B).
- **Statement #4 shows the same ordering with smaller steps**: A 682, B0
  728, B 805 us. B0 is +46 us over A (floor +25 us), B +123 us; the
  spread of B (734 to 935 us) is the widest. By statement #5 the four
  configurations lie within 654 to 714 us (A against Ar 34 us), consistent
  with the earlier follow-up that found #4 onward level.
- Size: the whole effect is 214 us on one statement of a join whose build-less
  walk costs 445 ms and whose build costs 4.1 ms; it is a few tenths of a
  millisecond on the statement after a recording statement and gone by #5.

## Remedy measurement - B2, `BuiltWalk` and `MissWalk` out of line

**B2 is withdrawn.** Its `[[gnu::noinline]]` is reverted in the source; B
(`7e31d272`) is the closing binary, and every other section of this file
measures B. B2 appears only here.

B2 is B's source plus `[[gnu::noinline]]` on `BuiltWalk` and `MissWalk` in
`src/exec/step_vm.cpp` (uncommitted when measured). Copy
`/home/louis/dqbench/kds_server_B2_noinline`, sha256
`47ccf2350593805408a6ba73f144cb16c0da6bcccae3efea1468d24d3befb93f`
(`m1c.json`, hashed by the driver at the run's start), Release. `nm` shows `BuiltWalk` inlined into `RunStep` in B and out
of line in B2. Hypothesis: the inlined guard and three coroutine creations
grew a hot coroutine, and this is the binary component seen in M1 (c) and (d)
and in statement #3's B0-against-A part.

Driver change, recorded here: `tools/dq_close.py` gained `--binB2`, a fourth
arm B2 started from that binary; with it `--cpus` names four CPUs and the
rotation runs over four. The runs below used servers on CPUs 0, 1, 2, 3 and
the driver on CPU 4 (SMT sibling of CPU 0: the driver blocks while a server
executes, and the rotation puts every arm on CPU 0 equally often). M1 above
used three servers on CPUs 1 to 3 and the driver on CPU 0, so the A, A2 and B
figures here are a second reading of them under a different placement.

**B2 does not remove the binary component in M1, and the diagnostic reading
says it removes the part of statement #3 the fallback does not own.**

### M1 shapes (a) to (d) with A, A2, B and B2

10 lifetimes (a cut from the ordered "about 10"), 24 rounds each, 240
round-pairs a shape, the arm order rotated and reversed; blocks and shapes as
above. Server CPU per statement; every delta is against A, the A2 column is
the floor. Cells carry microseconds; "positive" counts lifetimes (of 10)
whose median delta against A is above zero; "in floor" counts lifetimes whose
median delta lies in the A2-A lifetime range.

| shape | A | A2 - A (floor) | B - A | B2 - A | B2 - B (lifetime median) |
|---|---|---|---|---|---|
| a_point | 22.80 us | -0.10 us (-0.45 %), positive 5 of 10 | -0.09 us (-0.37 %), positive 3, in floor 8 | -0.08 us (-0.37 %), positive 3, in floor 8 | +0.07 us, positive 7 of 10 |
| b_build | 148.09 us | -0.39 us (-0.26 %), positive 4 of 10 | -2.30 us (-1.55 %), positive 1, in floor 7 | -3.87 us (-2.61 %), positive 0, in floor 1 | -0.85 us, positive 1 of 10 |
| c_cabin | 38.50 us | -0.74 us (-1.92 %), positive 1 of 10 | +0.20 us (+0.51 %), positive 6, in floor 5 | +0.52 us (+1.36 %), positive 9, in floor 3 | +0.10 us, positive 6 of 10 |
| d_tuplekey | 38.29 us | -0.32 us (-0.84 %), positive 3 of 10 | +0.46 us (+1.19 %), positive 8, in floor 5 | +0.36 us (+0.95 %), positive 8, in floor 4 | +0.21 us, positive 6 of 10 |

Reading:

- **c_cabin and d_tuplekey: B2 is no better than B.** B2 against A is +0.52
  us and +0.36 us (9 and 8 lifetimes positive), B2 against B is +0.10 us and
  +0.21 us. The out-of-line `BuiltWalk` leaves the residue in place, so
  the inlining hypothesis is **not confirmed for M1**: the +0.3 to +0.5 us
  on a join statement without a build is elsewhere (the compile or dispatch
  path, not `BuiltWalk`'s placement). In this placement B alone reads +0.20 us
  on c_cabin (earlier 15-lifetime reading +0.33 us) and +0.46 us on
  d_tuplekey (+0.26 us); the floor itself reads -0.74 and -0.32 us, so these
  deltas are within about 1 us of noise either way.
- **b_build: B2 is faster again** (-3.87 us, -2.6 %, no positive
  lifetime), B -2.30 us. Out of line is cheaper for the build join, a gain
  of 0.85 us over B (1 of 10 lifetimes positive).
- **a_point: nothing** (all three within -0.1 us, the floor's -0.10 us).
- Client wall (72,000 to 480,000 statements timed a cell, every 7th
  sampled, 10,290 to 68,580 a cell): p0 to p99 are level across the four arms, for example c_cabin A 65,
  69, 72, 138, 145 us; B2 66, 69, 72, 138, 145 us; d_tuplekey A 65, 69, 71,
  136, 144 us; B2 66, 69, 72, 137, 144 us; b_build A 169, 176, 183, 257,
  324 us against B2 165, 172, 179, 251, 319 us. Row counts agreed across
  the four arms in every block.

### The #3 diagnostic cell with A, B2 and B2 at `--build-cap 0`

`--sizes 10000 --dists unique --rounds 8 --stmts 5`, a fresh process per cell,
3 passes with the order rotated (A B2 B2c0 Ar / B2c0 Ar A B2 / Ar A B2 B2c0),
24 cells a configuration; Ar is A's binary again (the floor), B2c0 is B2 with
`join_build_max_rows = 0`. Counters as intended (`inner_built=1` at #1 on B2
only; 1,000 misses at #1 and 1,000 recordings at #2 on all four; #3 to #5
all hits). Server CPU and client wall; spread is p25 to p75 over 24 cells,
pass medians beside it (server CPU).

| statement | configuration | server CPU median | p25 to p75 | min to max | pass medians (CPU) | wall median | wall p25 to p75 |
|---|---|---|---|---|---|---|---|
| #3 | A | 938 us | 899 to 986 us | 857 to 1,083 us | 961, 926, 924 us | 1,067 us | 1,024 to 1,137 us |
| #3 | Ar (floor) | 892 us | 869 to 921 us | 838 to 1,472 us | 892, 916, 883 us | 1,046 us | 991 to 1,085 us |
| #3 | B2c0 (B2, cap 0) | 912 us | 879 to 966 us | 859 to 1,164 us | 913, 923, 893 us | 1,050 us | 1,024 to 1,104 us |
| #3 | B2 | 1,068 us | 1,022 to 1,131 us | 893 to 1,570 us | 1,088, 1,073, 1,043 us | 1,221 us | 1,157 to 1,287 us |
| #4 | A | 688 us | 665 to 838 us | 646 to 1,046 us | 802, 686, 671 us | 785 us | 758 to 935 us |
| #4 | Ar (floor) | 668 us | 657 to 746 us | 642 to 1,016 us | 663, 663, 740 us | 782 us | 750 to 841 us |
| #4 | B2c0 | 697 us | 662 to 823 us | 640 to 1,051 us | 738, 688, 750 us | 808 us | 751 to 929 us |
| #4 | B2 | 782 us | 731 to 910 us | 700 to 1,104 us | 800, 803, 743 us | 894 us | 834 to 1,039 us |
| #5 | A | 674 us | 641 to 709 us | 614 to 957 us | 669, 687, 676 us | 786 us | 772 to 833 us |
| #5 | Ar (floor) | 680 us | 631 to 728 us | 615 to 1,007 us | 747, 658, 675 us | 773 us | 724 to 812 us |
| #5 | B2c0 | 682 us | 648 to 711 us | 618 to 1,039 us | 669, 667, 701 us | 774 us | 742 to 804 us |
| #5 | B2 | 698 us | 670 to 771 us | 644 to 987 us | 675, 715, 702 us | 834 us | 776 to 859 us |

For reference, the walking statements: #1 A 446.8 ms, Ar 445.9 ms, B2c0
452.3 ms, B2 4.09 ms; #2 A 491.3 ms, Ar 492.3 ms, B2c0 500.4 ms, B2 500.1 ms.

**Does B2 remove the binary component? Yes for statement #3, no for M1's
join residue.**

- **#3.** B2 with the fallback off (B2c0, 912 us) reads level with A (938 us)
  and Ar (892 us): its three pass medians (893 to 923 us) lie in A's and Ar's
  range (883 to 961 us) and its spread overlaps A's. The binary component the
  earlier diagnostic left (B0 +75 us over A) is not present in B2. What stays
  is the fallback's: B2 over B2c0 is +156 us (+17 %), B2 over A +130 us
  (+14 %; floor A against Ar 46 us), with B2's p25 (1,022 us) above A's p75
  (986 us) and all B2 pass medians (1,043 to 1,088 us) above A's (924 to 961
  us). The earlier reading of B over A was +214 us, so B2 recovers about 84 us
  of #3; the component owned by the fallback's statement #1 is of the same
  size as before (156 us here, 139 us there).
- **#4.** A 688, Ar 668, B2c0 697, B2 782 us: the same ordering at smaller
  steps; B2 (+94 us over A, 731 to 910 us) is the only one clear of the A to
  Ar band. #5 is level: 674, 680, 682, 698 us.
- **The cost moves to the walking statements.** With the build off, B2c0
  reads +5.5 ms (+1.2 %) on #1 and +9.1 ms (+1.9 %) on #2 over A, where the
  floor (A against Ar) is 0.9 ms and 1.1 ms; the earlier B0 read level on
  both (449.8 and 485.2 ms against 445.9 and 490.8 ms). B2 on #2 reads the
  same, 500.1 ms. So out-of-line `MissWalk` or `BuiltWalk` costs about 1 % to
  2 % on a whole-relation walk in this cell. It is outside the floor but small
  against a 445 to 500 ms statement that the fallback replaces with 4 ms.
- **What B2 does not touch.** The fallback's own part of #3 (about 130 to
  156 us, 14 % to 17 %), and M1's +0.3 to +0.5 us on a join statement
  without a build. The mechanism of the first (the freed map's allocator or
  cache state, or where #2's sets land) is still not isolated; the second is
  not `BuiltWalk`'s inlining.

## Diagnostic - is statement #3's fallback-owned cost a timing artifact of the window?

Hypothesis: under the fallback B's statement #1 takes 4 ms where A's takes
445 ms, so B reaches #3 about 0.45 s earlier in the server's life, and
periodic background work (checkpoint cadence, statistics steward's tick,
Cabin controller's tick, publishes) lands in a different statement's
window. The driver sums CPU over every server thread, so such work would be
charged to B's #3 and to A's #2. With a gap before every statement, outside
the timed window, each statement starts after any such tick has had its
chance, on both binaries alike.

Driver change, recorded here: `tools/dq_s0_premise.py` gained `--gap-s S`
(sleep S seconds before each statement, after the holder's renewal and before
the CPU baseline is read, so outside the timed window) and records per-
statement CPU by thread `comm` name as `threads_us`.

Run (`m2g.sh`, archive `m2g-*`): `--sizes 10000 --dists unique --rounds 6
--stmts 5`, A and B, each at `--gap-s 0` and `--gap-s 1.5`, 2 passes, order
rotated per pass (A1.5 B1.5 A0 B0 / B0 A0 B1.5 A1.5), 12 cells a
configuration; fresh process per cell; the A arm (declared Cabin). B is the
closing binary 7e31d272 (B2 is withdrawn, see above). Server CPU and client
wall per statement, spread p25 to p75 over 12 cells, pass medians beside:

| statement | configuration | server CPU median | p25 to p75 | min to max | pass medians (CPU) | wall median | wall p25 to p75 |
|---|---|---|---|---|---|---|---|
| #3 | A, gap 0 | 982 us | 900 to 1,034 us | 879 to 1,147 us | 965, 1,020 us | 1,135 us | 1,066 to 1,182 us |
| #3 | B, gap 0 | 1,085 us | 1,021 to 1,153 us | 972 to 1,229 us | 1,085, 1,102 us | 1,238 us | 1,149 to 1,299 us |
| #3 | A, gap 1.5 s | 1,150 us | 1,120 to 1,166 us | 1,108 to 1,195 us | 1,143, 1,162 us | 1,305 us | 1,272 to 1,314 us |
| #3 | B, gap 1.5 s | 1,229 us | 1,217 to 1,256 us | 1,203 to 1,390 us | 1,241, 1,229 us | 1,390 us | 1,368 to 1,418 us |
| #4 | A, gap 0 | 787 us | 663 to 1,005 us | 650 to 1,060 us | 787, 868 us | 893 us | 772 to 1,137 us |
| #4 | B, gap 0 | 783 us | 766 to 843 us | 701 to 1,063 us | 783, 812 us | 914 us | 864 to 958 us |
| #4 | A, gap 1.5 s | 1,151 us | 1,134 to 1,161 us | 1,121 to 1,228 us | 1,153, 1,145 us | 1,299 us | 1,278 to 1,337 us |
| #4 | B, gap 1.5 s | 1,265 us | 1,241 to 1,278 us | 1,228 to 1,309 us | 1,280, 1,258 us | 1,415 us | 1,382 to 1,451 us |
| #5 | A, gap 0 | 706 us | 641 to 774 us | 620 to 982 us | 706, 729 us | 836 us | 718 to 894 us |
| #5 | B, gap 0 | 746 us | 684 to 770 us | 668 to 1,060 us | 708, 803 us | 852 us | 805 to 873 us |
| #5 | A, gap 1.5 s | 1,161 us | 1,152 to 1,189 us | 1,128 to 1,232 us | 1,174, 1,158 us | 1,301 us | 1,289 to 1,353 us |
| #5 | B, gap 1.5 s | 1,254 us | 1,239 to 1,268 us | 1,218 to 1,313 us | 1,244, 1,260 us | 1,410 us | 1,382 to 1,457 us |

For reference: #1 A 445.1 ms (gap 0) and 445.2 ms (gap 1.5), B 4.08 ms and
4.13 ms; #2 A 492.0 and 489.7 ms, B 484.7 and 485.1 ms.

Reading:

- **With the gap, B's #3 is still dearer: the cost belongs to the statement,
  not to the window.** B at gap 1.5 s reads 1,229 us against A's 1,150 us
  (+79 us, +7 %); B's p25 (1,217 us) is above A's p75 (1,166 us) and both
  pass medians are above. A timing artifact would have made the two level.
  At gap 0 this run reads +103 us (+10 %: B 1,085, A 982 us), smaller than
  the +130 to +214 us of the earlier cells; the spreads touch (A p75 1,034 us,
  B p25 1,021 us), so the gap-0 reading is a weaker one than the gap-1.5 s
  one, whose spreads do not overlap.
- **The gap raises every statement on both binaries** (A: #3 982 to 1,150 us,
  #4 787 to 1,151 us, #5 706 to 1,161 us): after 1.5 s idle a hit statement
  starts cold and costs about 1,150 us, where a back-to-back one costs about
  700 us. At gap 1.5 s B is dearer than A on **all three** statements
  (+79, +114, +93 us, +7 % to +10 %), where at gap 0 it is level from #4
  (783 against 787 us, 746 against 706 us). So the extra cost B pays is
  largest where the starting state is coldest, which fits a memory or
  cache-state explanation (the freed map's allocator or cache state, or
  where #2's sets land) and not a periodic tick that either binary meets in
  some window. It is a reading, not an isolation of the mechanism.
- **Thread split: not obtained.** Every server thread has the same
  `/proc/<pid>/task/<tid>/comm` (the binary's name, truncated), so `threads_us`
  holds one entry and the reactor's CPU cannot be told from the others by
  comm. Taking thread ids from `/proc/<pid>/task` at server start would
  separate them; not done inside the budget.
- **The earlier B-against-A #3 figures stand as measured with their spread**
  (+214, +130, +103 us over A in three independent cells at gap 0, B2's
  +130 us with the fallback on); the hit path costs B about 8 % to 10 % more
  than A in a cold state whatever the gap.

## What the run teaches about the engine

- **A declined miss costs nothing next to the build.** Agate over B-arm is
  0.79x to 1.15x a statement, 0.92x to 1.08x pooled. The set lookup, sighting and gate decline for 1,000 misses
  are lost inside a 3.2 ms (10,000 rows) or 18 ms (100,000 rows) build. DQ-S0
  expected 1.3x at 10,000 rows; the measured 1.03x to 1.06x says the
  bookkeeping is cheaper than a serve is, not equal to it.
- **E1's remaining gap is the recording statement, as DQ-S0 reasoned.** The
  statement whose misses are all recording touches (`unique` #2) still walks
  once per key: 501 ms at 10,000 rows, 5.0 s at 100,000 rows, refused at the
  defaults at 60,000. After DQ, the only budget refusal reachable in these
  cells is this one.
- **The cap still bounds E1.** The 60,000-row cell fits the default build cap
  of 65,536; DQ-S0 already noted that past the cap the map declines and every
  miss walks. The 100,000-row cells here ran with the cap raised, as in
  DQ-S0, and say nothing for the engine's defaults there.
- **Nothing in DQ is a measurable cost on a point read.** a_point B-A is
  -0.06 us against a floor of -0.07 us. A join statement is where the
  compile and dispatch change lands: +0.26 us to +0.33 us (0.7 % to 0.9 %)
  on the 16-row joins without a build, and -1.95 us (-1.3 %) on the join
  with one.

## Reproducing

`tools/dq_close.py` documents its flags and the command; M1 and M2 were
```
dq_close.py --binA A --binB B --workdir D --out m1.json  --lifetimes 5  --rounds 24
dq_close.py --binA A --binB B --workdir D --out m1b.json --lifetimes 10 --rounds 24
dq_s0_premise.py --bin X --sizes 10000                                   # 3 rounds, 5 statements
dq_s0_premise.py --bin X --sizes 100000 --max-rows-touched 1000000000 --rounds 3   # A: --rounds 1
dq_s0_premise.py --bin X --sizes 60000 --outer 2000 --engine-build-cap --rounds 1 --stmts 2 --dists unique
dq_s0_premise.py --bin X --sizes 10000 --shape exists
```
and the diagnostics `m2d.sh`, `m1c.sh` (B2's M1, then its cell `m2e-*`) and
`m2g.sh`. Every script in the archive (`m1.sh`, `m1b.sh`, `m1c.sh`, `m2.sh`,
`m2rep.sh`, `m2u.sh`, `m2d.sh`, `m2g.sh`) carries its exact invocations.
**Watchdogs:** `watch.sh` polled every 60 s for m1, m1b, m2, m2d, m1c and
m2g (`*.watch`). m2rep, m2u and m2e had none of their own: m1c.watch kept
polling m1c's heartbeat while m2e ran, which is why it reads up to 440 s.
Ages at the other polls were 0 to 12 s while a driver ran, up to 25 s at
its exit, and m2g's first poll ran before its heartbeat file existed. No
run stalled: every heartbeat file runs to its `end` or `done` line.

## Not measured

- Server CPU of the 100,000-row cells at the engine's default build cap
  (the cap makes the map decline there; DQ-S0 noted the same).
- Which mechanism (the freed map's allocator or cache state, or where #2's sets land) carries the #3 excess; a per-thread CPU split for the 4.5 ms statements (cause unverified).
- Wait events: `sys.wait_stats` and `ANALYZE`'s DB time were not read; the
  cells are all CPU on one session.
- The test suite was not run by this measurement (nothing under `src/`,
  `include/` or `tests/` changed here).
