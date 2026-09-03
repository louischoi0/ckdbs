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
