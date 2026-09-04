# Archive — AR2 §9 step 1, cells C1 and C2 (`v2.7.0-178-g92cb654`)

Raw output of the run documented in
`../../results-ar2-c1-colocation-v2.7.0-178-g92cb654.md` (scenario 2, C1,
co-location ceiling) and
`../../results-ar2-c2-spreading-v2.7.0-178-g92cb654.md` (scenario 0, C2,
local parallel inserts). Thirteen cells, run from
`/home/cdkbs/bench-runs/ar2-c1c2-92cb654/` on this host; the `.db` data
files and the hashed `kds_server` binary copy stayed there and are not
archived here (rule: never archive data files or the binary). Read the
two results files first — this directory is the evidence, not a second
copy of the narrative.

## Cells (in run order)

`s2-c8-plon`, `s0-c8-sp0`, `s2-c8-ploff`, `s0-c8-sp65536`, `s2-c1`,
`s0-c1`, `s0-c8-sp4096`, `s2-c8-plon-r2`, `s2-c8-ploff-r2`,
`s0-c8-sp0-r2`, `s0-c8-sp65536-r2`, `s2-c1-r2`, `s0-c1-r2`. A fourteenth
cell, `smoke-c1`, preceded all of these as a port/META sanity check with
no driver run; it produced no `.json`/`.meta.json`/`.cell.json` and is
excluded here, per the two results files' own §2.

## What each file is

- **`<cell>.json`** — the driver's own JSON summary (`--json` output):
  `meta` (scenario config, outcome counts, TPS) and `phases` (one
  `bench_common.Phase.summary()` per measured phase — ops, qps, mean,
  p0/p25/p50/p95/p99/max, errors). The source of every number in the
  results files' percentile tables.
- **`<cell>.meta.json`** — `SHOW META` from every reachable core, captured
  once after the driver's last phase and before `SIGTERM`
  (`run_cells.py`'s `dump_meta`). `attempts`/`cores_reached` record how
  many probe connections it took to see every core; for a `peer_listeners
  = off` cell only core 0 is ever reachable (`s2-c8-ploff*`:
  `attempts: 500`, `cores_reached: [0]` — the probe exhausted its retry
  budget because no other core has a listening socket, which is the
  mechanical proof behind C1's "every session lands on core 0").
- **`<cell>.cell.json`** — this run's own per-cell record: the precheck
  (`/proc/loadavg`, `pgrep -a -f 'cc1plus|cmake --build|ctest|kds_server'`,
  `df -T` of the run directory), the config text written for the server,
  server-start/stop timestamps, the driver phase list with exit codes and
  wall seconds, and the postcheck loadavg.
- **`<cell>.conf`** — the exact config file the server was started with
  (`data_file`, `port`, `cores`, `durability`, `peer_listeners`,
  `placement`, `range_size_ids` where armed, `log_dir`, `log_level`).
- **`<cell>.run.stdout.txt`** — the driver's full stdout for its measured
  run: the phase table (`ops qps mean p0 p25 p50 p95 p99 max err`), the
  scenario-specific outcome block (scenario 2: `committed`,
  `rejected-capacity`, `rejected-credit`, `conflicted`; scenario 0:
  `TPS`, `committed`, `torn`, `underfunded`, per-trader counts), and the
  `--verify` result. This is what `summarize.py` and both results files
  parse their tables from — nothing in the results files is recomputed
  from `.json` that isn't also visible here.
- **`<cell>.schema-only.stdout.txt`**, **`<cell>.load-only.stdout.txt`**
  (scenario 2 cells only) — `scenario2_freight.py --schema-only` and
  `--load-only`, run before the measured phase so the same `--suffix`'s
  data file is built once and then measured once, per
  `bench/docs/README.md`'s documented shape (`git show
  1769487:bench/docs/README.md`).
- **`run.json`** — `run_cells.py`'s own run-wide index: `describe`,
  `binary_sha256`, `started`/`finished`, and a `cells` list. **Only a
  partial index of this run**: `run_cells.py` was invoked twice (the
  first six cells, then the remaining seven), and each invocation
  overwrites `run.json` from scratch, so the file in this archive holds
  only the second invocation's seven cells (`s0-c8-sp4096` through
  `s0-c1-r2`). The `describe` and `binary_sha256` fields are run-wide
  facts and are correct for every cell (same binary, same worktree
  state, both invocations); per-cell precheck/timing/phase data for the
  first six cells (`s2-c8-plon` through `s0-c1`) is only in their own
  `<cell>.cell.json`, which is complete for all thirteen cells regardless
  of which invocation produced them.
- **`run_cells.py`**, **`summarize.py`** — the orchestrator and the
  headline-table script, copied as run, for reproducibility of how these
  files were produced. Both are also linked from the run directory in the
  results files' stamps; kept here too since the run directory itself
  (`/home/cdkbs/bench-runs/ar2-c1c2-92cb654/`) is outside the repository
  and not guaranteed to persist.

## Not archived

The thirteen `.db` data files and their `.db.wal/` directories, the
hashed `kds_server` binary copy, and `smoke-c1`'s own `.conf`/
`.server.txt` (no measurement). `<cell>.server.txt` (the server process's
own stdout/stderr capture) is also not archived — it is not one of the
files either results file's numbers were read from.

**`logs/<cell>/kdb.log` is also not archived, though `bench/README.md`'s
rule calls for a scenario run to archive its logs alongside the JSON
summaries.** *Corrected on `ar2-borrow-model` after `c40b3cc` (archive
re-read 2026-09-04): this file previously described a `logs/<cell>/kdb.log`
entry, quoting the `TXN_CONFLICT` lines the C2 results file's §5 reads,
as one of the files present here — no `logs/` directory exists in this
archive directory (`ls` at `c40b3cc` shows only `README.md`,
`device-probes.txt`, `run.json`, `run_cells.py`, `summarize.py` and the
thirteen cells' `.cell.json`/`.conf`/`.json`/`.meta.json`/
`.run.stdout.txt`/`.schema-only.stdout.txt`/`.load-only.stdout.txt`).
The C2 results file's own kdb.log quotation is therefore not
independently reproducible from this archive; it is carried over from
the run directory's own log file at measurement time, and is consistent
with the archived `.meta.json` counters the results file cites beside
it (`cross_core_write_refusals`, `shipped_refusals`, `trade-insert`
`errors`, all matching 7/7/6 across the three spreading cells).*
