Raw output and table scripts for
`bench/v3.0.0/results-bq-s2-rung1-v2.7.0-909-g4cf30270.md`.

- `bq/<durability>/<cell>/<arm>/r<1..3>/` - per run: the driver's JSON
  (`bl-mixed-*.json.gz`, gzip of the driver's output unchanged), the server log
  with every `[wal] roll` line (`*.s.log`) and the console log. Arms: `base`
  (`4cf30270`, 64 MiB), `opt3` (H4 option 3), `s16`, `s256`, `s1024` (the segment
  size in MiB). Cells: `c3`, `c2`, `c1-<placement>` for every arm at `relaxed`;
  `c2-iso`, `c1-iso-<placement>` for `base` only, one run each, at `relaxed` and
  at `group` (the `group` ones are not used by any verdict, see the results file
  section 8). No mixed `group` cell exists.
- `bq/mount/` - mount probe v2 (`<arm>-r<1,2>.json`), the probe the results file
  uses. `bq/mount-v1-superseded/` - probe v1 (SIGKILL, whole restart timed), kept
  only to show why it was discarded (results file section 4.5).
- `hoststamps.log` (one line per gate and per cell end), `watchdog.log`,
  `matrix.console`.
- `driver/` - `s2_matrix.py` (the matrix that ran; `s2_matrix.py.v1` is its first
  version, stopped and edited twice, results file section 0.3), `variants.py` (the
  one-file patch that makes each measurement arm from an export of `4cf30270`),
  `build_variants.sh`, `mount_probe2.py` (and `mount_probe.py`, v1), the
  restart/relay scripts, and `bl_mixed.py.at-1fb71d1a` / `ba_census.py.at-1fb71d1a`
  (the per-cell driver and its base module as at the worktree commit this file was
  written at; `bl_mixed.py` differs from `4cf30270`'s only in `meta_delta`,
  `ba_census.py` in `meta_delta` and `KDS_CLIENT_TIMEOUT`).
- `tables/` - the scripts that produced the tables in the results file. `lib.py`
  reads the original run directory (`S2`, `/home/cdkbs/bench-runs/bq/s2/out`) and
  BQ-S1's archive (`S1`); to re-run from this archive, `gunzip` every
  `bl-mixed-*.json.gz` in place in a copy of `bq/` and point `S2` at that copy
  (the layout `<durability>/<cell>/<arm>/r<n>` is the same). `gen_lat.py` (latency
  and the bar), `gen_rolls.py` (hold, steps, body, modes, recycling), `gen_c3.py`,
  `gen_misc.py` (delta against BQ-S1, waits, mount, isolated cells), `gen_h2.py`,
  `gen_ceiling.py` (the roll-aligned ceiling), `gen_runs.py` (appendices),
  `t2.py`-`t7.py` (the numbers quoted in prose). Run them as `nice python3 -I`.
  The prose of the results file is hand-written; each table is a script's output.
- `verdicts/` - the checked analyses (analyst plus two adversarial rechecks per
  verdict) that the results file's section 1 starts from:
  `final-verdicts-h4opt3-h2-bar.json`, their scripts and outputs (`h4opt3/`,
  `h2/`, `bar_s2.py`, `steps_s2.py`, and the `*-r1/` recomputations). Section 1.1 of
  the results file lists where this file departs from them.

Not archived on purpose: data files and WAL segments, the build trees and the
export (`4cf30270.tar` is byte-identical to `git archive 4cf30270`, sha256
`0190fe9ec929b589f4e081657065ac00a888d01952789829b2f74568dd0b1e99`), and the
binaries (hashes in the results file section 0.1).

`*.log` is ignored by `.gitignore`; the `*.s.log` and `console.log` files here need
`git add -f`, as BQ-S1's archive did.
