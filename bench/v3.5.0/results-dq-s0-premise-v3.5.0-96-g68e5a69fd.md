# DQ-S0 - the premise: a join on a cabined inner column before its keys converge

Measured 2026-11-23 between 00:20 and 00:28 UTC, on worktree `dq-cabin-miss-build`
at `68e5a69fd` (`v3.5.0-96-g68e5a69fd`), the commit DQ opened from. Order:
`instructions/v3.2.0/workorder-dq-cabin-miss-build.md` §5, DQ-S0 (iii).
Driver `tools/dq_s0_premise.py`, tables by `tools/dq_s0_report.py`, raw output in
`bench/v3.5.0/archive/dq-s0-premise-v3.5.0-96-g68e5a69fd/`.

## The answer first

**A declared Cabin makes this join 120x (10,000 inner rows) to 190x
(100,000 inner rows) dearer than the same join without it, for as long as
its keys are unobserved. Where the banking gate declines, that lasts for
every statement. At the default row-touch budget, the cabined join is
refused outright: at 100,000 inner rows the build answers it in 25 ms.**
Once the keys converge, the Cabin is 4.6x (10,000 rows) to 25x
(100,000 rows) cheaper than the build, so the Cabin is worth having. The
problem is only its miss path. The verdict is **E1** (the miss falls to
the build), **without E2**, with DQ-S3 (sub-chains) in scope.

| what | 10,000 inner rows | 100,000 inner rows |
|---|---|---|
| B, the build (no Cabin), server CPU a statement, median | 3.3 to 3.7 ms | 18.2 to 24.6 ms |
| Agate, the gate declining, every statement | 442 to 451 ms (1,000 walks) | 4,480 to 4,620 ms (1,000 walks) |
| Agate over B | 120x | 183x to 190x |
| A, unique keys, statements #1 and #2 | 446 ms, 491 ms | 4,505 ms, 4,942 ms |
| A once converged (1,000 hits) | 0.65 to 0.99 ms | under 2 ms (see the resolution note) |
| at the default `max_rows_touched` (100,000,000) | no refusal | **Agate refused 9 of 9 statements; A refused statements #1 and #2 of `unique`; B refused 0 of 9** |

## What was run

- `kds_server` copied to `/home/louis/dqbench/kds_server_A_68e5a69f`
  (sha256 prefix `45eb735a8626c211`). It was built `Release` in the
  worktree's `build-release` at `68e5a69fd`.
- Data file on `/dev/nvme0n1p1` (ext4, a block device). Port 15991/15992,
  chosen. `cores = 1`, `relaxed`, `buffer_pool_frames = 65536`: every page
  is resident, since the relations are made in the pool and never leave
  it.
- `join_order = written`: the outer relation drives. Under `fixed` the
  planner could choose either side.
- `join_build_max_rows = 2 x inner rows`. The default 65,536 would decline a
  100,000-row build and walk per outer row.
- The server's threads pinned to CPU 2, the driver to CPUs 0 and 4 (SMT
  pairs (0,4) (1,5) (2,6) (3,7)).
- One fresh server per cell, so each cell starts with an empty Cabin
  store.
- The statement: `ANALYZE SELECT o.id, i.w FROM outr AS o JOIN inn AS i ON
  i.k = o.k`.
  - The inner relation `inn (id, k, w)` holds N rows, its `k` a permutation
    of 1..N, so there is one row per key.
  - The outer relation `outr (id, k)` holds 1,000 rows. Its keys are:
    - `unique`: 1,000 distinct keys;
    - `repeat`: 100 keys, 10 times each, shuffled;
    - `skewed`: 20 hot keys 25 times each, plus 500 distinct cold keys,
      shuffled.
- Arms:
  - **A**: `CREATE CABIN ON inn(k)`.
  - **Agate**: as A, plus a second session holding `BEGIN; INSERT` open for
    the whole cell (renewed every 40 s, under the 60 s ceiling), so the
    banking gate declines every recording.
  - **B**: no Cabin, so the build answers.
- Each cell ran 5 statements in sequence (3 in the default-budget census).
  The arm order was rotated and reversed per round, 3 rounds.
- Per statement: server CPU (`/proc/<pid>/task/*/schedstat`, every thread),
  client wall time, and the inner step's `ANALYZE` counters.
- The plan lines confirm the arms: A and Agate are `step 1 CabinProbe inn AS
  i cabin=1 on=col1 key=0:0.1`; B is `step 1 Scan inn AS i build on=col1
  key=0:0.1`.
- **The budget.** The 10,000-row cells and one census round of the
  100,000-row cells ran at the default `max_rows_touched` (100,000,000).
  The 100,000-row cost cells ran with `max_rows_touched = 1000000000`,
  since the default refuses what they measure.
- **Host and time.** Load average 0.73 to 1.89 at every cell, and no
  `cc1plus` at any cell. The quiet gate held 0 s.
  - Time spent: building 5 minutes, running 8 minutes (10,000 rows 2.6 min,
    the census 1.1 min, the 100,000-row cost cells 4.6 min), inside the
    20-minute stage budget.
  - Nothing was cut.

**No earlier number exists for this shape.** No results file under
`bench/` sets a correlated Cabin probe beside the build (the order's
§1.1). These are the first, so there is no delta against an earlier
engine.

## The table

Medians over 3 rounds. The full per-statement table is
`archive/.../table.md`, and the default-budget census is
`archive/.../table-100k-default-budget.md`.

| inner rows | outer keys | arm | statement | server CPU | misses | hits | sets recorded | map built |
|---|---|---|---|---|---|---|---|---|
| 10,000 rows | unique | A | #1 | 445.55 ms | 1000 misses | 0 hits | 0 sets | 0 maps |
| 10,000 rows | unique | A | #2 | 491.24 ms | 1000 misses | 0 hits | 1000 sets | 0 maps |
| 10,000 rows | unique | A | #3 | 0.97 ms | 0 misses | 1000 hits | 0 sets | 0 maps |
| 10,000 rows | unique | Agate | #1 to #5 | 444.04 to 451.31 ms | 1000 misses | 0 hits | 0 sets | 0 maps |
| 10,000 rows | unique | B | #1 to #5 | 2.79 to 3.74 ms | - | - | - | 1 map |
| 10,000 rows | repeat | A | #1 | 95.83 ms | 200 misses | 800 hits | 100 sets | 0 maps |
| 10,000 rows | repeat | A | #2 | 0.77 ms | 0 misses | 1000 hits | 0 sets | 0 maps |
| 10,000 rows | skewed | A | #1 | 243.87 ms | 540 misses | 460 hits | 20 sets | 0 maps |
| 10,000 rows | skewed | A | #2 | 248.98 ms | 500 misses | 500 hits | 500 sets | 0 maps |
| 10,000 rows | skewed | A | #3 | 0.87 ms | 0 misses | 1000 hits | 0 sets | 0 maps |
| 100,000 rows | unique | A | #1 | 4,504.55 ms | 1000 misses | 0 hits | 0 sets | 0 maps |
| 100,000 rows | unique | A | #2 | 4,942.09 ms | 1000 misses | 0 hits | 1000 sets | 0 maps |
| 100,000 rows | unique | Agate | #1 to #5 | 4,498.49 to 4,525.26 ms | 1000 misses | 0 hits | 0 sets | 0 maps |
| 100,000 rows | unique | B | #1 to #5 | 18.19 to 24.58 ms | - | - | - | 1 map |
| 100,000 rows | repeat | A | #1 | 970.61 ms | 200 misses | 800 hits | 100 sets | 0 maps |
| 100,000 rows | skewed | A | #1 | 2,444.43 ms | 540 misses | 460 hits | 20 sets | 0 maps |
| 100,000 rows | skewed | A | #2 | 2,470.53 ms | 500 misses | 500 hits | 500 sets | 0 maps |

Client wall time per statement, every round and statement of the class
(10,000 rows / 100,000 rows):

| class | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|
| A converged (all hits), 10,000 rows | 0.73 ms | 0.78 ms | 0.89 ms | 1.21 ms | 1.51 ms |
| B, 10,000 rows | 3.21 ms | 3.39 ms | 3.46 ms | 3.98 ms | 4.17 ms |
| Agate, 10,000 rows | 439.74 ms | 443.59 ms | 447.58 ms | 464.90 ms | 474.38 ms |
| A converged (all hits), 100,000 rows | 0.68 ms | 0.96 ms | 1.18 ms | 2.36 ms | 2.39 ms |
| B, 100,000 rows | 17.63 ms | 18.96 ms | 19.55 ms | 24.62 ms | 25.46 ms |
| Agate, 100,000 rows | 4,447.71 ms | 4,484.55 ms | 4,511.93 ms | 4,633.83 ms | 4,683.64 ms |

**Resolution note.** `schedstat` advances at scheduler events, so a
statement under about 2 ms can read 0.00 ms of server CPU (seven readings
did). The converged rows are read from wall time, which is the client's
whole round trip, and the conclusions here never rest on a sub-2-ms CPU
cell.

**No wait breakdown.** One session ran each statement on an idle,
all-resident `cores = 1` server, so the statement is all CPU. `ANALYZE`'s
DB time line was not collected, and no `sys.wait_stats` was read.

## What the engine does, read from the numbers

1. **A miss is a whole walk, and the walk does not change with the key
   distribution.** A walk costs 445 us at 10,000 rows and 4.5 ms at
   100,000 rows, 44 to 45 ns a row at both sizes. Every miss pays it.
   - `unique` #1 and `Agate` pay 1,000 walks, `repeat` #1 pays 200, and
     `skewed` #1 pays 540.
   - The counters match the n = 2 rule exactly. A key's first touch sights
     it, its second records it, and its third is served. The driver's
     prediction (`predicted` in the JSON) equals the counters in 90 of 90
     A statements.
2. **The recording statement costs as much as the sighting statement.**
   `unique` #2 records 1,000 sets with 1,000 walks, 10 % dearer than #1
   (491 ms against 446 ms at 10,000 rows): the recording block's key compare
   and entry appends. E1 leaves this statement as it is (DQ-Q2 (a)), and
   at 100,000 rows and the default budget it is refused. See the next
   point.
3. **The default row-touch budget turns the cabined join into a
   refusal.**
   - At 100,000 inner rows, 1,000 walks are 100,000,000 row touches,
     exactly `max_rows_touched`.
   - In the census, A was refused at `unique` #1 and #2, and Agate at every
     statement of every distribution. B was refused at none.
   - B touches 100,000 rows plus 1,000 probes, and answers in 25 ms.
   - `cabin.md` §4a calls an accelerator that turns a rows-returning
     statement into a budget refusal "what §1 forbids". The tree does that
     today, on a top-level join, and dropping the Cabin brings the rows
     back.
   - E1 removes it for every non-recording miss and every declined
     recording, which is every Agate statement and `unique` #1. It does
     not remove it for a statement whose misses are all recording touches,
     like `unique` #2 (DQ-Q11, `known-gaps.md`).
4. **A banking gate that refuses never converges.** Agate reads 1,000
   misses and 0 sets in all 15 statements at both sizes. One open write
   transaction anywhere on the instance is enough. That is the
   known-gaps entry ("A Cabin builds rarely on a busy instance") turned
   into a number: 120x to 190x the uncabined cost, for good.
5. **Converged, the Cabin beats the build.** 1,000 hits cost 0.7 to 1.0 ms
   of wall time at either size. The build costs 3.5 ms at 10,000 rows and
   19.6 ms at 100,000 rows, because it re-walks the relation every
   statement. The ladder's order (banked structure first, build last) is
   right on a converged join. DQ keeps it.

## The verdict, and what it does to the order's items

- **E1, not E0.** The miss walks are the whole cost of every unconverged
  cell, and they meet or pass the budget at 100,000 rows. The build's
  first walk costs 3.5 ms (10,000 rows) or 24.6 ms (100,000 rows) against
  the 446 ms or 4.5 s it would replace.
- **E2 is not built (DQ-Q3 (a)'s condition is not met).**
  - E2 saves a walk only in a statement whose first miss is a recording
    touch and which later has a miss the map would serve. That is at most
    one walk a statement, by construction.
  - In every measured cell it saves 0 walks. The statements whose first
    miss records (`unique` #2, `skewed` #2) have no non-recording miss
    after it: 1,000 and 500 misses, all recorded. Under DQ-Q2 (a) each
    later recording touch walks anyway.
  - In every other statement the first miss is a first touch, which
    E1's build walk already serves.
  - DQ-S4 does not run.
- **DQ-Q8's bar: 1.5x, kept on a measured basis.**
  - Under E1, the gate-refusing cell is predicted at B's cost plus the
    bookkeeping of 1,000 misses: a set lookup, the sighting, and the
    gate's decline.
  - The converged A cells bound one probe of the set at 0.7 to 1.0 us
    (1,000 hits in 0.7 to 1.0 ms of wall time).
  - So Agate under E1 is predicted at about 4.5 ms against B's 3.5 ms at
    10,000 rows (1.3x), and about 20.5 ms against 19.6 ms at 100,000 rows
    (1.05x).
  - The bar stays 1.5x at the gate-refusing cell. It is no longer
    provisional: S0 measured its basis, and DQ-S5 measures it.
- **DQ-Q4 (a) holds: sub-chains are DQ-S3.** DQ-S0 (ii) found that E1
  composes with both stop rules. The reading is in the order's §7.
