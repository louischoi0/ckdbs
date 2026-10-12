Raw output and table scripts for
`bench/v3.0.0/results-bl-ruling1-remeasure-v2.7.0-909-g4cf30270.md`.

- `bl/<commit>/<placement>/r<1..3>/` - per run: the driver's JSON
  (`bl-mixed-*.json.gz`, gzip of the driver's output unchanged), the server
  log and the console log. `<commit>` is `3338cf0c` or `05cd011f`: BL's own
  builds, `/home/cdkbs/bench-runs/bl/bin/kds_server.<sha>`, started by the BQ
  matrix driver (phase `bl`).
- `hoststamps.log`, `bq_s1_matrix.py` - the matrix driver and its host stamps
  (the file also holds BQ-S1's cells); `tables/bl_mixed.py.at-4cf30270` is the
  per-cell driver it called.
- `tables/` - the scripts behind the tables; set `BQ_OUT` to a directory laid
  out like `/home/cdkbs/bench-runs/bq/out-4cf30270/` with the JSONs gunzipped
  in place.
