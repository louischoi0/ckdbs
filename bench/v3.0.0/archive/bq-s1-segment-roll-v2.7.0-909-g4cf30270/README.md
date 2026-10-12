Raw output and table scripts for
`bench/v3.0.0/results-bq-s1-segment-roll-v2.7.0-909-g4cf30270.md`.

- `bq/<durability>/<cell>/r<1..3>/` - per run: the driver's JSON
  (`bl-mixed-*.json.gz`, gzip of the driver's output unchanged), the server
  log with every `[wal] roll` line (`*.s.log`) and the console log. Cells:
  `c3`, `c2`, `c2-iso`, `c1-<placement>`, `c1-iso-<placement>`.
- `hoststamps.log`, `bq_s1_matrix.py` - the matrix driver that ran the cells
  (phases `smoke`, `bl`, `bq`); `tables/bl_mixed.py.at-4cf30270` is the
  per-cell driver it called (`tools/bl_mixed.py` at `4cf30270`).
- `tables/` - the scripts that produced the tables in the results file. They
  read the original output directory; to re-run them from this archive,
  `gunzip` every `bl-mixed-*.json.gz` in place in a copy of `bq/`, place that
  copy under a directory laid out like `/home/cdkbs/bench-runs/bq/out-4cf30270/`
  (`bq/`, `hoststamps.log`) and set `BQ_OUT` to it. `build.py` fills the
  results file's table placeholders; the prose is hand-written.
- `verdicts/` - the checked analyses' scripts and outputs (H0, H1, H3, H4, H5,
  the bar, the recycling cause, `h3_bound.py`, `sync_cadence.py`,
  `recycle.py`), and `final-verdicts-h0-h5-bar.json` / `recycle-cause.json`,
  the workflow results (analyst plus adversarial checks) that the results
  file's verdicts come from.

Not archived on purpose: the driver's `analysis.txt` and `analyze_s1.py`
(two known errors: a hard-coded bulk core, and a `meta_delta` that drops keys
absent at `m0`; see §9 of the results file), the 6 s smoke cells, and data
files and WAL segments.
