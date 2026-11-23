# DQ-S0 - the premise: a join on a cabined inner column before its keys converge

Measured 2026-11-23 between 00:20 and 00:28 UTC and at 00:50 UTC, on worktree
`dq-cabin-miss-build` at `68e5a69fd` (`v3.5.0-96-g68e5a69fd`), the commit DQ
opened from. Order: `instructions/v3.2.0/workorder-dq-cabin-miss-build.md`
§5, DQ-S0 (iii). Driver `tools/dq_s0_premise.py`, tables by
`tools/dq_s0_report.py`, raw output in
`bench/v3.5.0/archive/dq-s0-premise-v3.5.0-96-g68e5a69fd/`. Corrected after
DQ-S0's `critics-developer` review: the order's §7 lists what changed.

## The answer first

**A declared Cabin makes this join 116x to 159x (10,000 inner rows) and
183x to 249x (100,000 inner rows) dearer than the same join with the
Cabin dropped, statement by statement, for as long as its keys are
unobserved. Where the banking gate declines, that lasts for every
statement. At the engine's own defaults the cabined join is refused
outright while the uncabined one answers.** The defaults are
`max_rows_touched` 100,000,000 and `join_build_max_rows` 65,536. At
60,000 inner rows and 2,000 outer rows the cabined join was refused at
both statements and the build answered in 10.7 to 14.6 ms.

Once the keys converge, the Cabin is 3.9x (10,000 rows) to 16.9x (100,000
rows) cheaper than the build in wall time, so the Cabin is worth having.
The problem is only its miss path. The verdict is **E1** (the miss falls
to the build), **without E2**, with DQ-S3 (sub-chains) in scope.

**The cap bounds what E1 can buy.** The fallback is the build, with the
build's `join_build_max_rows` (DQ-R4). With an inner relation past the
cap the map declines and every miss walks, with or without E1. At the
default 65,536 that is every inner relation above 65,536 rows.

| what | 10,000 inner rows | 100,000 inner rows (cap raised) | 60,000 inner rows, 2,000 outer, engine defaults |
|---|---|---|---|
| B, the build (no Cabin), server CPU a statement, median | 2.79 to 3.86 ms | 18.19 to 24.58 ms | 10.71 ms, 14.58 ms |
| Agate, the gate declining, every statement | 442 to 451 ms (1,000 walks) | 4,480 to 4,620 ms (1,000 walks) | refused, 2 of 2 |
| Agate over B, statement by statement | 116x to 159x | 183x to 249x | - |
| A, unique keys, statements #1 and #2 | 446 ms, 491 ms | 4,505 ms, 4,942 ms | refused, 2 of 2 |
| A once converged (1,000 hits), wall p50 | 0.89 ms | 1.16 ms | - |
| refusals at the default `max_rows_touched` | none | A: `unique` #1 and #2; Agate: 9 of 9; B: 0 of 9 (census, cap raised) | A 2 of 2, Agate 2 of 2, B 0 of 2 |

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
- **The build cap.** The 10,000-row and 100,000-row cells set
  `join_build_max_rows = 2 x inner rows` (at 10,000 rows that is the
  default 65,536). At 100,000 rows the default would decline the build and
  walk per outer row, so B would be refused at the default budget too.
  The 60,000-row cell ran at the engine's own cap
  (`--engine-build-cap`), the only cell at every default.
- The server's threads pinned to CPU 2, the driver to CPUs 0 and 4 (SMT
  pairs (0,4) (1,5) (2,6) (3,7)).
- One fresh server per cell, so each cell starts with an empty Cabin
  store.
- The statement: `ANALYZE SELECT o.id, i.w FROM outr AS o JOIN inn AS i ON
  i.k = o.k`.
  - The inner relation `inn (id, k, w)` holds N rows, its `k` a permutation
    of 1..N, so there is one row per key.
  - The outer relation `outr (id, k)` holds 1,000 rows (2,000 in the
    60,000-row cell). Its keys are:
    - `unique`: distinct keys;
    - `repeat`: 100 keys, 10 times each, shuffled;
    - `skewed`: 20 hot keys 25 times each, plus 500 distinct cold keys,
      shuffled.
- Arms:
  - **A**: `CREATE CABIN ON inn(k)`.
  - **Agate**: as A, plus a second session holding `BEGIN; INSERT` open for
    the whole cell (renewed every 40 s, under the 60 s ceiling; no cell
    needed a renewal), so the banking gate declines every recording.
  - **B**: no Cabin, so the build answers.
- Each cell ran 5 statements in sequence (3 in the census, 2 in the
  60,000-row cell). The arm order was rotated and reversed per round, 3
  rounds (1 in the census and the 60,000-row cell).
- Per statement: server CPU (`/proc/<pid>/task/*/schedstat`, every thread),
  client wall time, and the inner step's `ANALYZE` counters.
- Walks of the inner relation are `examined / N` and `cabin_misses`. The
  order names `correlated_scans`, but that counter counts a sub-chain's
  walks only, and this join is top-level.
- The inner step's stats line confirms each arm's kind:
  - `step 1 CabinProbe inn AS i` in A and Agate;
  - `step 1 Scan inn AS i ... inner_built=1` in B.

  The plan lines (`cabin=1 on=col1 key=0:0.1`, `build on=col1 key=0:0.1`)
  were read in a smoke run and are not archived.
- **The budget.** The 10,000-row cells and one census round of the
  100,000-row cells ran at the default `max_rows_touched` (100,000,000).
  The 100,000-row cost cells ran with `max_rows_touched = 1000000000`,
  since the default refuses what they measure.
- **The driver changed inside the stage**, and every change is recorded:
  - The first run (`s0-10k.json`, `s0-10k.log`) ended in an unhandled
    refusal at the first 100,000-row statement. Its 27 10,000-row cells
    (00:20:07 to 00:20:45) are kept.
  - The driver then learned to record a refusal and to raise
    `max_rows_touched`, for the census and the cost cells.
  - After the review it learned `--engine-build-cap`, used by the
    60,000-row cell.
  - It also learned a settled CPU read (`settled_cpu_ns`). That read waits
    until no server thread is runnable, because `schedstat` folds a running
    thread's time in only at a 4 ms tick or when the thread leaves the CPU.
    The archived cells predate it, so their readings under about 4 ms can
    be low: 5 read 0.0 ms. No conclusion here rests on a cell under 4 ms;
    the converged figures are wall time.
  - None of these changes alters what an earlier cell measured.
- **Host and time.** The 1-minute load was 0.47 to 1.89 at every cell, and
  no `cc1plus` ran at any cell. The quiet gate held 0 s.
  - Time spent: building 5 minutes, running 9 minutes (10,000 rows
    0.6 min, the census 1.1 min, the 100,000-row cost cells 4.6 min, the
    60,000-row cell 0.3 min), inside the 20-minute stage budget.

**No earlier number exists for this shape.** No results file under
`bench/` sets a correlated Cabin probe beside the build (the order's
§1.1). These are the first, so there is no delta against an earlier
engine.

## The table

Medians over 3 rounds. The full per-statement tables are
`archive/.../table.md` (10,000 and 100,000 rows), `table-100k-default-budget.md`
(the census) and `table-60k-engine-defaults.md`.

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
| 60,000 rows, 2,000 outer | unique | A | #1, #2 | refused at 4,503 ms, 4,887 ms | - | - | - | - |
| 60,000 rows, 2,000 outer | unique | Agate | #1, #2 | refused at 4,487 ms, 4,584 ms | - | - | - | - |
| 60,000 rows, 2,000 outer | unique | B | #1, #2 | 14.58 ms, 10.71 ms | - | - | - | 1 map |

Client wall time per statement, every round and statement of the class:

| class | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|
| A converged (all hits), 10,000 rows | 0.73 ms | 0.78 ms | 0.89 ms | 1.21 ms | 1.51 ms |
| B, 10,000 rows | 3.21 ms | 3.39 ms | 3.46 ms | 3.98 ms | 4.17 ms |
| Agate, 10,000 rows | 439.74 ms | 443.59 ms | 447.58 ms | 464.90 ms | 474.38 ms |
| A converged (all hits), 100,000 rows | 0.68 ms | 0.96 ms | 1.18 ms | 2.36 ms | 2.39 ms |
| B, 100,000 rows | 17.63 ms | 18.96 ms | 19.55 ms | 24.62 ms | 25.46 ms |
| Agate, 100,000 rows | 4,447.71 ms | 4,484.55 ms | 4,511.93 ms | 4,633.83 ms | 4,683.64 ms |

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
   (491 ms against 446 ms at 10,000 rows): the recording block's key
   compare and entry appends. E1 leaves this statement as it is
   (DQ-Q2 (a)).
3. **At the engine's defaults the cabined join is a refusal.**
   - At 60,000 inner rows the build fits the default cap. With 2,000
     outer rows, the per-row walk is 120,000,000 row touches, over the
     default `max_rows_touched`. A and Agate were refused at both
     statements, and B answered in 10.7 to 14.6 ms.
   - `cabin.md` §4a calls an accelerator that turns a rows-returning
     statement into a budget refusal "what §1 forbids". The tree does that
     today, on a top-level join, and dropping the Cabin brings the rows
     back.
   - The window at defaults: N up to 65,536 (the cap) and k x N above
     100,000,000.
   - E1 removes the refusal for every non-recording miss and every declined
     recording: every Agate statement, and `unique` #1. It does not remove
     it for a statement whose misses are all recording touches, like
     `unique` #2 (DQ-Q11, `known-gaps.md`).
   - Past the cap, E1 removes nothing.
   - The 100,000-row census shows the same refusals, with the cap raised
     so that B could build.
4. **A banking gate that refuses never converges.** Agate reads 1,000
   misses and 0 sets in all 90 statements at both sizes. One open
   write transaction anywhere on the instance is enough. That is the
   known-gaps entry ("A Cabin builds rarely on a busy instance") turned
   into a number: 116x to 249x the uncabined cost, for good.
5. **Converged, the Cabin beats the build.**
   - 1,000 hits cost 0.89 ms (10,000 rows) and 1.16 ms (100,000 rows) of
     wall time at the median.
   - B costs 3.46 ms and 19.55 ms, since every statement builds its own
     map.
   - So the ladder's order (banked structure first, build last) is right
     on a converged join, and DQ keeps it.
   - B's statement #1 is 5 to 6 ms dearer than #2 to #5 at 100,000 rows in
     every distribution and round. The cause is unverified (the load's
     background work finishing, the planner's first sample, first-use
     allocation). A per-thread CPU read would tell them apart, and the
     direction is conservative for B.

## The verdict, and what it does to the order's items

- **E1, not E0.** The miss walks are the whole cost of every unconverged
  cell, and at the defaults they pass the budget where the build does not.
  A B statement costs 2.8 to 3.9 ms (10,000 rows) or 18.2 to 24.6 ms
  (100,000 rows) against the 446 ms or 4.5 s it would replace.
- **E2 is not built.** DQ-Q3 (a)'s condition was read as whether E2
  saves a walk, not as written, and the order's §7 records that.
  - As written ("the first miss is often a recording one"), the condition
    is met in `unique` #2 and `skewed` #2, where every miss is a recording
    touch.
  - But E2 saves a walk only in a statement whose first miss is a recording
    touch and which later has a miss the map would serve. That is at most
    one walk a statement, by construction, because under DQ-Q2 (a) each
    later recording touch walks anyway.
  - In the measured cells it saves 0, since each statement re-runs the same
    outer relation. The rejection rests on the bound, not on the cells.
  - DQ-S4 does not run.
- **DQ-Q8's bar stays 1.5x, provisional until DQ-S5 measures it.** Under
  E1, the gate-refusing cell is predicted at B's cost plus the bookkeeping
  of 1,000 misses: a set lookup, the sighting, and the gate's decline.
  - No cell measured that bookkeeping. The converged cells bound a set's
    serve at about 0.9 to 1.2 us a probe, and that bounds a miss's
    bookkeeping only if a miss costs no more than a serve.
  - On that assumption the cell is about 1.3x of B at 10,000 rows and
    1.05x at 100,000 rows. The 100,000-row figure needs the raised cap.
- **DQ-Q4 (a) holds: sub-chains are DQ-S3.** DQ-S0 (ii) found that E1
  composes with both stop rules. The reading is in the order's §7.
