# Raw outputs of the CP close measurement (v3.4.7-11-g7d21468a)

Results file: `../../results-cp-close-v3.4.7-11-g7d21468a.md`. Base = `cc54a296` (sha256
`d78b1147...`), close = `7d21468a` (sha256 `049f27e6...`), both Release copies.

- `pass/`: the run's per-cell directories (JSON summaries, driver stdout, `sys` view dumps
  `*.views.txt/.json`, manifests, timelines, `host*.log`, console logs, `status.txt`,
  `watch.log`). `m1/` is M1 batch 1, `m1swap/` the label-swapped batch (the driver's primary
  and AB labels exchanged), `m4/`, `m2/` (`census.json.gz`, `summary.md`), `m3-s2-{base,close}-{a,b,c}/`,
  `m3-s0-{base,close}-{a,b,c}/`, `m5-{base,close}-<stamp>/` (M5, two runs per binary).
- `scripts/`: the run scripts as run (`run_all.sh`, `run_extra.sh`, `run_m1swap.sh`; each cell's
  script, the server launcher `m1m4_srv.py`, view readers, `host_sampler.sh`, `watchdog.sh`), and the
  analysis scripts (`an_*.py`) that produced the tables. The scripts are the CP-S0 ones
  (`../cp-s0-premise-v3.4.7-1-gcc54a296/scripts/`) with paths and labels adapted (`nolock` / `noslice`
  became `close`; M4 runs base and close side by side).
- `results-analysis/m3_analysis.txt`: the M3 extraction of every run.
- Not archived: data files, WAL segments, server logs (`*.kdb.log`, `server.out`), M2's per-arm
  `server-files/`.
