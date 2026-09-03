# Archive — AM-S1 `cores = 1` A/B (`v2.7.0-183-gc985d37`)

Raw output of the run documented in
`../../results-am-s1-page-latch-v2.7.0-183-gc985d37.md`. Twelve cells in
three passes, run from `/home/cdkbs/bench-runs/am-s1-page-latch/` on this
host; the twelve `.db` data files (and their `.db.wal/` segment
directories), the two hashed `kds_server` binary copies (arm A and arm
B), each cell's `<cell>.server.txt`, and `logs/<cell>/kdb.log` stayed
there or were never worth copying (below) and are not archived here.
Read the results file first — this directory is the evidence behind it,
not a second copy of its narrative.

## Cells (in run order)

Pass 1 (13:15:25–13:18:38 UTC): `g1-A`, `g1-B`, `s1-A`, `s1-B`, `g2-B`,
`g2-A`, `s2-B`, `s2-A`.

Pass 2 (13:20:47–13:21:24 UTC): `g3-A`, `g3-B` — added because `g1-B`
came back with a 434 ms mount-time completion checkpoint (against
5.2–7.4 ms in the nine clean cells), leaving group durability with only
one clean `B` cell.

Pass 3 (13:22:45–13:23:08 UTC): `g4-B`, `g4-A` — added because both `g3`
cells also came back degraded (168 ms and 61 ms mount checkpoints); run
after an off-orchestrator device probe at 13:22:27 UTC (40×4 KiB
`pwrite`+`fdatasync`, p50 1.50 ms / p90 3.29 ms / max 9.70 ms / min
1.32 ms, loadavg 0.27) that is not itself archived as a file.

`g1-B`, `g3-A` and `g3-B` are the three degraded cells; the results file
excludes them from every comparison but keeps their own rows.

## What each file is

- **`<cell>.json`** — the driver's own JSON summary (`--json` output):
  `meta` (scenario config, outcome counts, TPS) and `phases` (one
  `bench_common.Phase.summary()` per measured phase — ops, qps, mean,
  p0/p25/p50/p95/p99/max, errors). The source of every number in the
  results file's percentile tables.
- **`<cell>.cell.json`** — this run's own per-cell record: the precheck
  (`/proc/loadavg`, `pgrep -a -f "cc1plus|cmake --build|ctest|kds_server"`,
  `df -T` of the run directory), the exact config text written for the
  server, `server_start`, the driver invocation (`argv`, exit code, wall
  seconds), the `SHOW META` reply text (`meta`), `server_stop` (signal,
  exit code, stop wait), and `postcheck_loadavg`.
- **`<cell>.conf`** — the exact config file the server was started with
  (`data_file`, `port`, `cores`, `durability`, `peer_listeners`,
  `placement`, `checkpoint_interval_ms`, `auth`, `tls`, `log_dir`,
  `log_level`).
- **`<cell>.run.stdout.txt`** — the driver's full stdout for its measured
  run: the phase table (`ops qps mean p0 p25 p50 p95 p99 max err`), the
  per-phase description block, the "business scenario" block (`TPS`,
  `statements/sec`, `committed`, `torn`, `underfunded`, per-trader
  counts, the reporting-process summary, the `--verify` result). The
  human-readable twin of `<cell>.json`, and the source of the results
  file's `committed`/`torn`/`underfunded`/`--verify` claims. **Where the
  two differ, `<cell>.json` is authoritative**: this stdout truncates TPS
  to one decimal rather than rounding it, so `g2-B` prints 711.8 and
  `g4-B` 723.8 where `meta.tps` holds 711.87 and 723.85 and the results
  file's §3 carries 711.9 and 723.9.
- **`run.json`** — the orchestrator's own run-wide index: `describe_tree`,
  both arms' binary paths and `sha256`, `started`/`finished`, and the
  `cells` list (one record per cell, matching that cell's own
  `<cell>.cell.json`). **Do not read `passes[]` as a pass list.**
  `run_ab.py` was invoked three times (once per pass) and reloads the
  previous `run.json` to append rather than overwrite (`run_ab.py:168`),
  and the entry it appends is a snapshot of *everything before this
  invocation*, not of the previous pass. Three consequences, all visible
  in the archived file: there are **two** entries for three passes (the
  pass in progress writes no entry for itself); each entry's `cells` is
  **cumulative**, so `passes[1].cells` lists all ten cells run to that
  point rather than pass 2's two; and each entry's `started` is the
  *run's* start carried forward (`run_ab.py:173` passes `prev["started"]`),
  so `passes[1].started` reads `2026-09-03 13:15:25 UTC` — pass 1's
  start, not pass 2's. Only `passes[N].finished` is a true pass boundary.
  **The authority is `run-log.txt`'s per-cell timestamps** (and the
  top-level `pass_started`, correct for the pass it was written during).
  The results file's §2 cells table uses `run-log.txt` and each cell's own
  `<cell>.cell.json:server_start`, never `passes[]`.
- **`run-log.txt`** — the orchestrator's own stdout: one `starting` line
  and one `done` line per cell, each timestamped, giving the authoritative
  pass boundaries the `run.json` quirk above does not.
- **`run_ab.py`**, **`ab_summary.py`** — the orchestrator and the
  summarizing script, archived as run, for reproducibility of how these
  files were produced and how the results file's tables were recomputed.
  `run_ab.py`'s own docstring names the invocation
  (`run_ab.py <run dir> <binary A> <binary B> [all | <cell> ...]`);
  `ab_summary.py <run dir>` reads `run.json` and every `<cell>.json` in
  it and prints the per-cell TPS/percentile/META table this archive's
  numbers were checked against.

## Not archived

The twelve `.db` data files and their `.db.wal/` directories (rule: never
archive data files or WAL segments); the two hashed `kds_server` binary
copies (`kds_server_A`, `kds_server_B` — their `sha256` and size are in
the results file's stamp, §1, which is what a re-run needs to confirm it
is measuring the same engine); `<cell>.server.txt` (the server process's
own stdout/stderr capture — not one of the files any results-file number
was read from); and `logs/<cell>/kdb.log` for every cell — each one is
0 bytes (`log_level = warn` and nothing in any cell's run raised a
warning-or-above server-side log line) and `*.log` is repository-wide
gitignored (`.gitignore`).
