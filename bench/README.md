# `bench/` — emptied 2026-09-02

Everything that lived here left ahead of the big-bang change to the
architecture, rules and constraints: the five C++ microbenchmarks
(`bench_main.cpp`, `keystone_alloc_bench.cpp`, `txn_layers_bench.cpp`,
`crosscore_pipeline_bench.cpp`, `session_step_state_bench.cpp`), the probe
drivers, `bench/docs/`, the three scenario documents, and every results
file and archive from `v2.2.0` through `v2.8.0`. Each number in them priced
the engine that change replaces, and a results file kept beside a different
engine reads as that engine's.

The last commit holding the tree is `1769487`:

    git ls-tree -r --name-only 1769487 bench     # what was here
    git show 1769487:bench/<path>                # any one file

A citation to `bench/...` in a spec, the manual, a test or a source comment
points at that commit. Nothing is written here until the big-bang change
says what a measurement of the new engine is and where it goes.
