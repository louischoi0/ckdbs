Raw output and table scripts for
`bench/v3.0.0/results-bp-s3-g1-ab-v2.7.0-909-gccf9cfc1.md`.

- `ab/bn-r<1..3>.json` and `.log` - `tools/bn_census.py` (A = `1592007e`, B =
  `ccf9cfc1`, both servers side by side, 12 blocks per arm); the `.json` carries
  every arm's wall percentiles, server and client CPU, per-block server CPU and
  the end-state checks, and each `.log` is the driver's one-line-per-arm output.
  r1 ran at 14:21 UTC, r2 and r3 at 19:10 UTC (their first tries, 14:21 and 16:59,
  failed on a stale WAL directory and left nothing).
- `ab/commit-<group|strict>-<a|b>-r<1..3>/` - `tools/ba_census.py --shape insert1`
  commit loops (`*.json.gz`, gzip of the driver's JSON unchanged) and the
  one-line `.log` beside each.
- `hoststamps.ab.log` - the chain's gate lines for these cells (load average and
  filesystem per cell); `driver/` - `chain.sh` (the chain that ran the cells; its
  H7/H9/H10 part belongs to the BP-S1 census file), `after_s2.sh` (the two
  `bn_census` re-runs), `start_after_s2.sh`, `watchdog.sh`, `ab_table.py` (the
  first table sketch), and `bn_census.py.at-1fb71d1a`, `ba_census.py.at-1fb71d1a`
  (the drivers as at the worktree commit the results file was written at;
  `bn_census.py` gained the stale-WAL fix after r1 was taken).
- `binaries.txt` - the two binaries' hashes and the exports they were built from.
- `tables/` - the scripts that produced the results file's tables and quoted
  numbers (`bp_lib.py` reads the original directory, `AB` at its top; point it at
  a copy of `ab/` with the `commit-*` JSONs gunzipped to re-run). `gen_bp.py` is
  every table; `t8.py` (the `update-range` stall per arm), `t9.py` (the commit
  loops' reactor counters) and `t10.py` (the checks) are the numbers quoted in
  prose. Run them as `nice python3 -I`.

Not archived: data files and WAL segments, the build trees, the two exports
(byte-identical to `git archive` of their commits) and the binaries.
