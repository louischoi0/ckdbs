# `bench/` — reopened 2026-09-03 for v3.0.0

Emptied on 2026-09-02 ahead of the big-bang change to the architecture,
rules and constraints; reopened by AL-R8 of
`instructions/v3.0.0/workorder-al-m0-single-wal.md` once there was a v3
engine to price. **`bench/v3.0.0/` is a fresh series and carries no delta
against any v2.x number** (AR0 D15): every number under `v2.2.0`..`v2.8.0`
measured the engine AR0 replaces, and a per-core-WAL number set beside a
single-stream one reads as a regression or a win that neither ran.

What left, and where it is: the five C++ microbenchmarks
(`bench_main.cpp`, `keystone_alloc_bench.cpp`, `txn_layers_bench.cpp`,
`crosscore_pipeline_bench.cpp`, `session_step_state_bench.cpp`), the probe
drivers, `bench/docs/`, the three scenario documents, and every results
file from `v2.2.0` through `v2.8.0`. The last commit holding them is
`1769487`:

    git ls-tree -r --name-only 1769487 bench     # what was here
    git show 1769487:bench/<path>                # any one file

A citation to `bench/...` in a spec, the manual, a test or a source comment
points at that commit unless it names a `v3.` path.

## Where a result goes

`bench/<version>/<benchmark>-<git describe --tags>.md` under the version of
record, with a scenario run's raw driver output (JSON summaries and logs,
never data files) beside it under
`bench/<version>/archive/<scenario>-<describe>/`.

## The five rules a run is invalid without

Four carry over from `bench/docs/README.md` at `1769487` — each has already
invalidated a run on this box, and none is implied by "measure in release".
The fifth is new to v3.

1. **Release, rebuilt at the measured commit.** `CMakeLists.txt` defaults to
   Debug, which has reported the wrong *sign*; a stale `build-release`
   silently prices an older engine than `HEAD`.
2. **A block device, never tmpfs**, for the *data* file. Check with `df -T`
   at run time and **name the device in the results file**: the answer has
   differed between hosts this suite runs on. (On this box, 2026-09-03: `/`
   and `/tmp` are both `/dev/root`, ext4.)
3. **Measure a copy of the binary.** `cp` it into the run's own directory,
   hash the copy, start every server from the copy. The build tree is shared
   with every other session in this repository, and a `cmake --build`
   landing mid-matrix swaps the engine under a run that starts a fresh
   server per configuration — with nothing in the driver output to show it.
4. **Record the host's load, per cell.** `/proc/loadavg` and
   `pgrep -a -f "cc1plus|cmake --build|ctest"` before each cell, both
   written into the file. Three session scratchpads were live here on
   2026-09-02 and a competing build moved a colocated p99 by 12×.
5. **The port is chosen, never defaulted.** 15432 on this box is held by an
   unrelated `kds_server` under `/home/cdkbs/autotrade`; a cell that binds
   the default either fails or, worse, talks to that instance.

The drivers themselves are unmodified `tools/` scripts — a driver change
inside a measurement stage measures the driver. `bench/docs/` stays closed
until there is a v3 driver whose behaviour is not already documented in the
tool's own `--help`.

## Every cell carries its unit — since 2026-09-09

The operator's working rule of 2026-09-09: **in a results document's matrix,
the unit belongs in the cell** — `437 µs`, `15,414 tps`, `3 ns` — and not
only in the column heading.

The reason is what a table is for. A results file is read years after the
run, in fragments, and quoted one row at a time into a work order or a
spec; a cell lifted out of a table whose heading carried the unit is a bare
number, and a bare number in a durability discussion has been read as
milliseconds when it was microseconds. A heading is not carried by the
sentence that quotes the row.

Where a column is genuinely unitless — a count, a ratio, an error tally —
the cell says so the same way: `0 errors`, `1.3 %`, `4 cells`.

**It applies to every results file written after this rule was recorded,
and earlier files stay as they are** — the last one before it is
`results-ao-s7-c3-v2.7.0-304-g5e94dc8.md`, whose run matrix carries its
units in the headings. The boundary is this rule's own commit and not a
date, because a date is ambiguous on the day it is written and this rule
was written on a day that already had a results file in it. Rewriting them would restate numbers nobody re-measured,
which is the one thing this directory's rules exist to prevent; a reader who
meets an older table reads its heading, as that table's own commit intended.

## What a v3 number is measured on — BTREE only, since 2026-09-08

The operator's mark on AS-Q6 (`instructions/v3.0.0/raft-marks-2026-09-08-as-q6.md`):
**every relation a driver creates is `BTREE`, and a delta is taken only
against a BTREE baseline of the same driver** — a driver still emitting
`HEAP` is refused at `CREATE TABLE` and produces no v3 number until AS-S3
changes it. Heap relations are suspended
(SUS-1), so a post-2026-09-05 engine refuses the shape the AL-S8 files at
`f6ed10c` measured — `trades`/`user_periodic_profit` and
`freights`/`charges` were `HEAP` there. Those files stay as history and are
compared against nothing: a heap number beside a btree number is two
workloads. The comparator for every later delta is `f6ed10c` re-measured
with the changed drivers, on the same host and from the archived binary
AL-S8's stamp names — **measured 2026-09-08**, eight cells, all of
AL-S8's own arguments and its cell order:

- `results-scenario0-stockmarket-btree-v2.7.0-157-gf6ed10c.md`
- `results-scenario2-freight-btree-v2.7.0-157-gf6ed10c.md`

The `-btree-` in the benchmark name is what keeps the two series apart,
and it is load-bearing: the same commit now has two results files per
scenario, measuring two workloads. **A delta is read against the
`-btree-` pair and never against the other.** Each carries, once and
marked not to be reused, the difference between the two — which prices
the driver change and the five days between the runs together, and
separates neither, because a statement whose storage class did not change
moved as well. The driver change itself was a tools stage outside any
measurement stage, which is what the rule above requires and why the
baseline was re-measured rather than reused.
