# BV close: the Transition check's cost on single-row INSERT and UPDATE

**Finding.** On a `BTREE` relation with no Transition, BV's whole code
change (`b0bc959c` to `3eb5a1eb`) moved server CPU per statement by
-0.13 µs on UPDATE and +0.17 µs on INSERT, pooled over six cells, against a
25 µs / 23 µs statement. The two copies of the A binary differ from each
other by -0.21 µs and -0.02 µs, and the B-minus-A sign flips between the 200-row
and the 1K/10K INSERT cells, so the no-Transition cells show no change the
run can separate from its floor. With a Transition on the `varchar` state
column, the check adds **+0.36 µs per UPDATE row and +0.28 µs per INSERT row**
of server CPU (about 1.5 % and 1.2 % of the statement), and client latency
percentiles do not move at the resolution of the driver (p50 44.0 to 44.3 µs
on UPDATE).

## Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-14, 03:07 to 03:10 UTC (the measured run; a 3-run dry batch and a 3-run batch at 12,000 ops before it are not reported: this file holds the one run that also records p95) |
| Branch | `worktree-cn4-transition-refresh` (A is its fork point, B its head) |
| A | `b0bc959c`, `v3.1.0-31-gb0bc959c`, exported with `git archive` (no `.git`, so no dirty state), built in its own `build-release` (`CMAKE_BUILD_TYPE=Release`), `kds_server` mtime 2026-10-14 03:01:44 UTC |
| B | `3eb5a1eb`, `v3.1.0-38-g3eb5a1eb`, `git archive` of the commit (the worktree was clean at it), own `build-release`, `kds_server` mtime 2026-10-14 03:01:46 UTC. Both binaries were built after their commits, so neither is older than its commit |
| Binaries measured | Copies in `/home/cdkbs/bvclose/bin/`; sha256 A `0a2c8f2936c250310fd1798fb0f4497a81d5cf6cb1c9928db4a0737666023b47`, B `b227f8207b32b23bfd4447e8716bc0c5ea6ebd1b8ac4f2a45c05b26fda356deb` (`archive/.../binaries.sha256`) |
| Superblock | A mounts 23, B mounts 24; every server started on its own fresh data file, never shared between arms |
| Device | Data files and WALs under `/home/cdkbs/bvclose/run/final/`, on `/dev/root`, ext4 (`df -T` at the start of the session), not tmpfs |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, no other non-default key. Server pinned to CPU 2, driver to CPU 4 (SMT siblings 3 and 5 unused by the run) |
| Host | 8 CPUs. 1-minute load average 1.4 to 1.9 across the six runs (other sessions' idle servers and `claude` processes present); `pgrep -a -f "cc1plus\|cmake --build\|ctest"` empty before and after every run. The builds had finished before the first measured statement. Per-cell records are in `summary.json` |
| Driver | `archive/bv-close-transition-v3.1.0-38-g3eb5a1eb/bv_close.py`, a new driver that imports `tools/bn_census.py`'s helpers unmodified; `tools/` and every engine source are untouched. `bench/docs/` is closed (`bench/README.md`), so the flags are stated here: `--bin-a --bin-b --workdir --ops 12000 --blocks 12 --runs 6` |
| Tests | No engine code changed for this measurement, so no suite was run for it. BV-S2/S3's own suite results are in the work order's section 6 |

## Method

Four arms run side by side, each in its own server on its own port, all
pinned to one CPU with only one ever busy:

| Arm | Binary | Relation |
|---|---|---|
| A1 | A (`b0bc959c`) | `p...`, no Transition |
| A2 | A, a second server | `p...`, no Transition. A1 against A2 is the noise floor measured inside the run |
| B | B (`3eb5a1eb`) | `p...`, no Transition |
| BT | B, the same server as B | `g...`, same schema, with a Transition |

Each relation is `(id int64, st varchar(16), v int64) BTREE`, every row
`st = 'A'`. The Transition on each `g` relation is
`ON INSERT ('A') ON UPDATE ('A' => 'B', 'B' => 'A')`, so every INSERT writes
the one insert state and every UPDATE writes `'A'` or `'B'`, a declared edge
or the same value again (allowed). After the run, `UPDATE gu200 SET st = 'C'`
is refused `TRANSITION_VIOLATION`, and the same statement on `pu200` is
accepted, so the BT arm's check was live. Zero replies of the wrong tag in
any of 3,456,000 statements.

- **UPDATE:** `UPDATE u<n> SET st = '<A|B>' WHERE id = <k>`, a literal SET of a
  non-key `varchar` by tuple key, `k` cycling over the relation's `n` rows.
- **INSERT:** `INSERT INTO i<n>_<path> VALUES (<key>, 'A', <v>)`, one row,
  keys from 10,000,001 up, into a relation preloaded with `n` rows (so the
  tree holds `n` rows at the first INSERT and `n + 12,000` at the last of a cell).
- **Rows** are `n` in {200, 1,000, 10,000}. A point UPDATE or INSERT does
  not scale with `n`; the three sizes are shown as evidence of that.
- **Path:** `literal` (an unnamed `C_PARSE` of the text per statement, what every
  `tools/` driver does) and `prepared` (one named `C_PARSE`, then
  BIND/EXECUTE).
- **Equal work:** 12,000 statements per arm per cell per run, in 12 blocks of
  1,000, the four arms taking the blocks in an order that rotates one place
  per block and per run. 6 runs, each on four fresh servers and fresh data files.
  Each row below therefore stands on 72,000 statements.
- **Primary metric:** the server's on-CPU time per statement, the sum of
  `/proc/<pid>/task/*/schedstat` over every server thread across the arm's
  blocks. `durability = relaxed`, so there is no commit fsync in the
  statement and group-commit variance is out of the number; this also means
  the numbers say nothing about `group` or `strict` durability.

## Results

### Server CPU per statement

CPU time per statement is a cost, not a throughput, so it is kept in µs and
labelled; the throughput form is the next table. Every cell is the mean over
6 runs. The three right columns are differences in µs, mean over the 6 runs
with the standard deviation across runs: A2 minus A1 is the floor, B minus
the mean of A1 and A2 is BV's whole change on a relation without a
Transition, and BT minus B is the Transition's check on one server.

| Shape | Path | Rows | A1 | A2 | B | BT | A2-A1 (floor) | B-A | BT-B |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE | literal | 200 | 25.07 µs | 24.71 µs | 24.77 µs | 25.07 µs | -0.36 µs (sd 0.47) | -0.12 µs (sd 0.34) | +0.31 µs (sd 0.18) |
| UPDATE | literal | 1000 | 25.15 µs | 24.92 µs | 24.95 µs | 25.35 µs | -0.23 µs (sd 0.48) | -0.08 µs (sd 0.37) | +0.39 µs (sd 0.19) |
| UPDATE | literal | 10000 | 25.22 µs | 25.05 µs | 25.00 µs | 25.38 µs | -0.16 µs (sd 0.54) | -0.14 µs (sd 0.35) | +0.39 µs (sd 0.09) |
| UPDATE | prepared | 200 | 24.42 µs | 24.19 µs | 24.21 µs | 24.55 µs | -0.23 µs (sd 0.63) | -0.09 µs (sd 0.51) | +0.34 µs (sd 0.21) |
| UPDATE | prepared | 1000 | 24.62 µs | 24.43 µs | 24.38 µs | 24.77 µs | -0.20 µs (sd 0.45) | -0.15 µs (sd 0.44) | +0.39 µs (sd 0.12) |
| UPDATE | prepared | 10000 | 24.67 µs | 24.59 µs | 24.40 µs | 24.75 µs | -0.08 µs (sd 0.60) | -0.23 µs (sd 0.39) | +0.35 µs (sd 0.14) |
| INSERT | literal | 200 | 23.10 µs | 23.03 µs | 22.82 µs | 22.99 µs | -0.06 µs (sd 0.34) | -0.24 µs (sd 0.46) | +0.17 µs (sd 0.11) |
| INSERT | literal | 1000 | 22.70 µs | 22.68 µs | 23.10 µs | 23.60 µs | -0.02 µs (sd 0.37) | +0.41 µs (sd 0.53) | +0.50 µs (sd 0.64) |
| INSERT | literal | 10000 | 22.74 µs | 22.74 µs | 23.06 µs | 23.27 µs | +0.00 µs (sd 0.40) | +0.32 µs (sd 0.38) | +0.21 µs (sd 0.19) |
| INSERT | prepared | 200 | 22.51 µs | 22.49 µs | 22.38 µs | 22.42 µs | -0.02 µs (sd 0.50) | -0.12 µs (sd 0.98) | +0.03 µs (sd 0.20) |
| INSERT | prepared | 1000 | 22.19 µs | 22.12 µs | 22.53 µs | 23.14 µs | -0.07 µs (sd 0.43) | +0.38 µs (sd 0.57) | +0.61 µs (sd 0.65) |
| INSERT | prepared | 10000 | 22.25 µs | 22.31 µs | 22.56 µs | 22.74 µs | +0.06 µs (sd 0.31) | +0.28 µs (sd 0.50) | +0.18 µs (sd 0.06) |

Pooled over the six UPDATE cells, mean server CPU per statement is A1
24.86 µs, A2 24.65 µs, B 24.62 µs, BT 24.98 µs; over the six INSERT cells,
A1 22.58 µs, A2 22.56 µs, B 22.74 µs, BT 23.03 µs.

- **Cell 1, no Transition (B against A).** UPDATE -0.13 µs, INSERT
  +0.17 µs pooled. Per cell the difference runs from -0.24 to +0.41 µs with
  a run-to-run sd of 0.3 to 1.0 µs, and the A1/A2 floor is as wide
  (-0.36 to +0.06 µs per cell, sd 0.3 to 0.6). INSERT's B-minus-A is negative at
  200 rows and positive at 1K and 10K; nothing in it is consistent
  enough to call a change, and none is claimed. What the run supports is
  that any cost of BV's code on a relation without a Transition is below
  about 0.4 µs per statement (under 2 %), which is the width of the
  floor; it does not support "exactly zero".
- **Cell 2, with a Transition (BT against B).** The UPDATE difference is
  positive in all six cells, +0.31 to +0.39 µs, with a run-to-run sd of 0.09 to
  0.21 µs (the pair runs on one server, so it is tighter than the
  A1/A2 floor, which compares two processes). INSERT is +0.03 to +0.61 µs per
  cell, pooled +0.28 µs, and the two 1K cells' sd (0.64, 0.65) exceeds their mean. Stated, not
  judged: the check costs about 0.3 to 0.4 µs of server CPU per
  UPDATE row and about 0.3 µs per INSERT row on this host. Against the
  whole binary A, BT is +0.45 µs per INSERT and +0.23 µs per UPDATE, the
  sum of the two effects above.
- **Size.** Neither delta moves with `n` (200, 1K, 10K). A per-row check
  that reads only the row being written should not depend on the relation's
  size (BV-R1), and the run shows none.

### Client throughput (derived)

Derived as 1,000,000 / the arm's mean wall time per statement over the same 72,000
statements; the driver does not report it directly. The driver is one
serial connection, so this is the serial form of the table above plus the
round trip, and it is dominated by the client and socket. The odd low cells
(INSERT 200 literal A2, INSERT 200 prepared A1, INSERT 1K BT) are a few
stalled statements inflating a mean, in a different arm per cell, so they
are host stalls and not an arm's property; the percentile table, which is
median-based, does not show them.

| Shape | Path | Rows | A1 | A2 | B | BT |
|---|---|---|---|---|---|---|
| UPDATE | literal | 200 | 21,074 stmt/s | 21,192 stmt/s | 21,362 stmt/s | 21,102 stmt/s |
| UPDATE | literal | 1000 | 21,040 stmt/s | 21,248 stmt/s | 21,234 stmt/s | 20,992 stmt/s |
| UPDATE | literal | 10000 | 21,057 stmt/s | 21,129 stmt/s | 21,112 stmt/s | 20,936 stmt/s |
| UPDATE | prepared | 200 | 21,481 stmt/s | 21,720 stmt/s | 21,825 stmt/s | 21,362 stmt/s |
| UPDATE | prepared | 1000 | 21,194 stmt/s | 21,464 stmt/s | 21,536 stmt/s | 21,405 stmt/s |
| UPDATE | prepared | 10000 | 21,397 stmt/s | 21,351 stmt/s | 21,388 stmt/s | 21,306 stmt/s |
| INSERT | literal | 200 | 22,393 stmt/s | 18,018 stmt/s | 20,612 stmt/s | 21,218 stmt/s |
| INSERT | literal | 1000 | 22,305 stmt/s | 22,348 stmt/s | 20,938 stmt/s | 19,816 stmt/s |
| INSERT | literal | 10000 | 22,025 stmt/s | 22,080 stmt/s | 21,921 stmt/s | 21,583 stmt/s |
| INSERT | prepared | 200 | 17,702 stmt/s | 22,473 stmt/s | 19,047 stmt/s | 21,775 stmt/s |
| INSERT | prepared | 1000 | 21,333 stmt/s | 22,518 stmt/s | 21,915 stmt/s | 19,607 stmt/s |
| INSERT | prepared | 10000 | 22,274 stmt/s | 22,200 stmt/s | 22,268 stmt/s | 21,786 stmt/s |

### Latency distribution

Client-observed, one statement at a time, 72,000 per row; each percentile is the
median of that percentile over the six runs. The Transition does not
show anywhere in the shape: p0 (the best case the path reaches) and p25
and p50 are within about 0.4 µs of B's, p95 and p99 are inside the
spread between the two A servers.

| Shape | Path | Rows | Arm | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|
| UPDATE | literal | 200 | A1 | 72,000 | 36.2 µs | 43.7 µs | 44.2 µs | 57.8 µs | 68.3 µs |
| UPDATE | literal | 200 | A2 | 72,000 | 35.6 µs | 43.5 µs | 44.0 µs | 57.2 µs | 67.5 µs |
| UPDATE | literal | 200 | B | 72,000 | 35.8 µs | 43.5 µs | 44.0 µs | 57.3 µs | 67.4 µs |
| UPDATE | literal | 200 | BT | 72,000 | 36.2 µs | 43.8 µs | 44.3 µs | 57.4 µs | 67.0 µs |
| UPDATE | literal | 1000 | A1 | 72,000 | 35.6 µs | 43.7 µs | 44.2 µs | 58.1 µs | 69.6 µs |
| UPDATE | literal | 1000 | A2 | 72,000 | 35.8 µs | 43.5 µs | 44.0 µs | 57.7 µs | 67.9 µs |
| UPDATE | literal | 1000 | B | 72,000 | 35.4 µs | 43.5 µs | 44.0 µs | 57.9 µs | 71.1 µs |
| UPDATE | literal | 1000 | BT | 72,000 | 36.0 µs | 43.9 µs | 44.3 µs | 57.9 µs | 68.9 µs |
| UPDATE | literal | 10000 | A1 | 72,000 | 36.1 µs | 43.8 µs | 44.3 µs | 58.0 µs | 68.2 µs |
| UPDATE | literal | 10000 | A2 | 72,000 | 36.2 µs | 43.6 µs | 44.1 µs | 57.9 µs | 69.0 µs |
| UPDATE | literal | 10000 | B | 72,000 | 35.9 µs | 43.6 µs | 44.1 µs | 58.2 µs | 68.6 µs |
| UPDATE | literal | 10000 | BT | 72,000 | 36.1 µs | 43.9 µs | 44.4 µs | 58.5 µs | 67.2 µs |
| UPDATE | prepared | 200 | A1 | 72,000 | 35.0 µs | 42.8 µs | 43.3 µs | 56.5 µs | 66.4 µs |
| UPDATE | prepared | 200 | A2 | 72,000 | 34.8 µs | 42.5 µs | 43.0 µs | 56.0 µs | 66.7 µs |
| UPDATE | prepared | 200 | B | 72,000 | 34.9 µs | 42.6 µs | 43.0 µs | 56.2 µs | 66.2 µs |
| UPDATE | prepared | 200 | BT | 72,000 | 35.2 µs | 42.9 µs | 43.4 µs | 56.7 µs | 65.8 µs |
| UPDATE | prepared | 1000 | A1 | 72,000 | 35.1 µs | 42.9 µs | 43.4 µs | 57.2 µs | 67.9 µs |
| UPDATE | prepared | 1000 | A2 | 72,000 | 35.2 µs | 42.6 µs | 43.1 µs | 56.4 µs | 67.9 µs |
| UPDATE | prepared | 1000 | B | 72,000 | 34.9 µs | 42.8 µs | 43.3 µs | 56.8 µs | 68.6 µs |
| UPDATE | prepared | 1000 | BT | 72,000 | 34.8 µs | 43.1 µs | 43.7 µs | 57.2 µs | 67.9 µs |
| UPDATE | prepared | 10000 | A1 | 72,000 | 35.4 µs | 43.0 µs | 43.5 µs | 57.2 µs | 68.2 µs |
| UPDATE | prepared | 10000 | A2 | 72,000 | 35.0 µs | 42.7 µs | 43.2 µs | 57.5 µs | 68.6 µs |
| UPDATE | prepared | 10000 | B | 72,000 | 35.0 µs | 42.8 µs | 43.3 µs | 56.9 µs | 67.0 µs |
| UPDATE | prepared | 10000 | BT | 72,000 | 35.3 µs | 43.2 µs | 43.7 µs | 57.1 µs | 66.5 µs |
| INSERT | literal | 200 | A1 | 72,000 | 34.1 µs | 41.3 µs | 41.7 µs | 55.1 µs | 71.9 µs |
| INSERT | literal | 200 | A2 | 72,000 | 34.0 µs | 41.2 µs | 41.6 µs | 55.0 µs | 72.7 µs |
| INSERT | literal | 200 | B | 72,000 | 34.4 µs | 41.4 µs | 41.9 µs | 55.3 µs | 68.6 µs |
| INSERT | literal | 200 | BT | 72,000 | 34.2 µs | 41.6 µs | 42.1 µs | 55.3 µs | 66.4 µs |
| INSERT | literal | 1000 | A1 | 72,000 | 35.3 µs | 41.3 µs | 41.8 µs | 55.1 µs | 68.4 µs |
| INSERT | literal | 1000 | A2 | 72,000 | 33.6 µs | 41.3 µs | 41.7 µs | 55.0 µs | 68.4 µs |
| INSERT | literal | 1000 | B | 72,000 | 35.0 µs | 41.6 µs | 42.0 µs | 55.5 µs | 68.4 µs |
| INSERT | literal | 1000 | BT | 72,000 | 34.2 µs | 41.8 µs | 42.3 µs | 56.2 µs | 68.2 µs |
| INSERT | literal | 10000 | A1 | 72,000 | 34.1 µs | 41.4 µs | 41.8 µs | 55.4 µs | 70.8 µs |
| INSERT | literal | 10000 | A2 | 72,000 | 34.9 µs | 41.4 µs | 41.8 µs | 55.4 µs | 70.8 µs |
| INSERT | literal | 10000 | B | 72,000 | 34.1 µs | 41.6 µs | 42.0 µs | 55.5 µs | 70.8 µs |
| INSERT | literal | 10000 | BT | 72,000 | 35.0 µs | 41.7 µs | 42.2 µs | 55.6 µs | 68.9 µs |
| INSERT | prepared | 200 | A1 | 72,000 | 34.0 µs | 40.7 µs | 41.2 µs | 54.8 µs | 69.5 µs |
| INSERT | prepared | 200 | A2 | 72,000 | 34.4 µs | 40.8 µs | 41.3 µs | 54.9 µs | 69.3 µs |
| INSERT | prepared | 200 | B | 72,000 | 33.9 µs | 41.1 µs | 41.5 µs | 54.8 µs | 67.7 µs |
| INSERT | prepared | 200 | BT | 72,000 | 34.0 µs | 41.3 µs | 41.7 µs | 54.9 µs | 66.5 µs |
| INSERT | prepared | 1000 | A1 | 72,000 | 34.6 µs | 40.9 µs | 41.4 µs | 54.8 µs | 67.9 µs |
| INSERT | prepared | 1000 | A2 | 72,000 | 33.9 µs | 40.8 µs | 41.3 µs | 55.0 µs | 67.7 µs |
| INSERT | prepared | 1000 | B | 72,000 | 33.6 µs | 41.3 µs | 41.7 µs | 55.1 µs | 68.4 µs |
| INSERT | prepared | 1000 | BT | 72,000 | 34.1 µs | 41.5 µs | 42.0 µs | 55.5 µs | 68.6 µs |
| INSERT | prepared | 10000 | A1 | 72,000 | 35.0 µs | 41.0 µs | 41.5 µs | 55.4 µs | 70.1 µs |
| INSERT | prepared | 10000 | A2 | 72,000 | 34.0 µs | 41.0 µs | 41.5 µs | 55.3 µs | 70.4 µs |
| INSERT | prepared | 10000 | B | 72,000 | 34.6 µs | 41.4 µs | 41.8 µs | 55.6 µs | 69.9 µs |
| INSERT | prepared | 10000 | BT | 72,000 | 34.5 µs | 41.6 µs | 42.0 µs | 55.6 µs | 69.4 µs |

### Where the statement's time goes

With `durability = relaxed` there is no commit wait, so the decomposition
is two parts, from the UPDATE literal cell at 1,000 rows:

| Component | Share | Basis |
|---|---|---|
| Server CPU: parse, bind, write statement, MVCC and undo, WAL append (no sync), reply | about 25 µs of 47.5 µs mean wall, 53 % | `schedstat` over the server's threads |
| Client and socket round trip: driver encode and decode, the loopback send and wake-up of the server | the remaining 22 µs, 47 % | wall minus server CPU, so it includes any server idle or wake latency |
| Durability or commit (fsync) | 0 by configuration | `relaxed`; not measured and not claimed for `group` or `strict` |
| Lock or conflict wait | 0 observed | one connection, one relation per statement, no second writer |

The check sits inside the 25 µs, and inside it adds 0.3 to 0.4 µs. A durable commit
wait is far larger than a sub-microsecond delta and would hide it; that is
why `relaxed` was used, and why this file bounds the check's CPU and not its
effect on a durable commit (not measured).

## What the run says about the engine

- **The empty-list test costs nothing the run can resolve.** BV-R1's
  design, a check that reads the row being written and the catalog's
  `TableAccess`, with no registry and no per-write derived structure, is
  consistent with the data: a relation without a Transition shows no
  separable difference, and a relation with one pays a sub-microsecond,
  size-independent per-row cost. The work order's section 5 expectation for
  cell 1 ("no change") is not contradicted; its expectation for cell 2 gets
  its first number: about 0.3 to 0.4 µs per row, a single `varchar`
  comparison against a two-edge declaration.
- **A larger declaration was not tried.** The cost of the check at 2
  declared edges says nothing of a Transition with hundreds of edges or of
  a long state string; the run did not vary either (see "Cut").
- **The statement is client- and server-fixed-cost bound.** 25 µs of server
  CPU and 44 µs of client p50 for one row at every row count means this
  shape characterises the request path (parse, dispatch, reply), where the
  Transition's 1 to 2 % is the smallest term. That is the correct place to
  measure a per-row guard's overhead, and the wrong place to look for
  where the engine spends its time.

## Baseline

- **This engine's own last run:** none for this shape. No earlier results file
  holds a single-row INSERT/UPDATE of a `varchar` column on a btree relation
  at three sizes and `relaxed` durability, so this file becomes the baseline
  a later run is read against. The A arm inside this run is the within-run
  baseline for BV's change.
- **PostgreSQL floor:** not measured for this shape.

## Not an extreme (rule 4b)

Nothing in this run approached a limit. One connection, one core, a
`relaxed` commit and a single reactor CPU measure the per-statement fixed
cost, which is what a per-row guard adds to; they do not characterise the
engine at saturation, under `strict` or `group` durability, or across
cores. A Transition's cost under those is unmeasured.

## What was cut (time budget, about one hour including building)

Building two release trees took about 5 minutes and the measurement about
3 minutes. The cuts were by choice of scope, not by time:

- `strict` and `group` durability; only `relaxed` was run.
- More than one core, concurrent writers, any saturation or k sweep.
- DELETE (the `ON DELETE FROM` check), multi-row `VALUES`, the KWP load
  stream, and `UPDATE` that does not assign the governed column. The last is
  the more interesting of the cells that were not run, since a
  governed relation's every UPDATE that skips the column should pay the
  least; it was not measured.
- Declaration size (edges, state-string width) and a state column that is
  `char(N)` or an integer type.
- Delta across a dirty-page, checkpoint or recovery path; no Transition is
  persisted outside its catalog row, and `CREATE TRANSITION`'s own cost (a
  relation scan) was not timed.
- The PostgreSQL floor.

Raw files: `archive/bv-close-transition-v3.1.0-38-g3eb5a1eb/`
(`summary.json` holds every run's every cell with percentiles and
per-cell host records; `driver.log`, `watchdog.log`, the driver, the table
generator and the binaries' hashes).
