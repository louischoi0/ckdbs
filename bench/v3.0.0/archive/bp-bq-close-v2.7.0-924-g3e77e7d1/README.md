Raw output and table scripts for
`bench/v3.0.0/results-bp-bq-close-v2.7.0-924-g3e77e7d1.md`.

- `out/bn/` - `tools/bn_census.py` pairs at `cores = 1`: `AB-r<1..3>` (A `cf889b63` against B `3e77e7d1`) and
  `AQ-r<1..3>` (A against Q `4cf30270`). Each `.json` carries every arm's wall percentiles, the server's and the
  client's on-CPU time, the per-block server CPU and the end-state checks; each `.log` is the driver's one-line-per-arm
  output.
- `out/bisect/` (`S2S1`: A against `1592007e`; `S4S6BQ`: `ccf9cfc1` against B), `out/bisect2/` (`MERGE`: `ccf9cfc1`
  against `6139c2de`; `S4`: `6139c2de` against `d555cb55`; `S6`: `d555cb55` against B), `out/aligned/`
  (`MERGEALIGNED`: the two aligned builds), `out/rollfix/` (`PREMERGE_FIX`: `ccf9cfc1` against `rollfix`;
  `CLOSE_FIX`: B against `rollfix`) - the same driver, `--only select-pk,update-range`. The first name of each pair
  is `--bin` (the **first server**), the second `--bin-b`.
- `out/ba/<shape>-<A|Q|B>-r<1..3>/` - `tools/ba_census.py` at `cores = 4`, `point` and `insert1`, one JSON per cell
  (driver output unchanged) and the one-line `.log` beside it.
- `out/lock/` - `lockcell.py` at `cores = 8`: `<S6A|B>-locks-relaxed-k8-<plain|fence>-r<0..2>.json` (`SHOW META`
  before and after, the server's CPU, the holder's log), `.bench.json` (the lock benchmark's own per-arm summaries) and
  `.txt` (its console report).
- `out/profile/` - `bn_census.py --sampler` passes of `ccf9cfc1` (`pre`), `6139c2de` (`post`), `rollfix` (`fix`) and B
  (`close`): the driver's `.json`/`.log`, the sampler's raw `dump.0` (literal) and `dump.1` (prepared) per pass, and
  `report.txt`, `report2.txt` (the top-25 lists the first pass of analysis read).
- `out/hoststamps.log`, `out/watchdog.log`, `out/*/stamps.log` - the gate lines (load average per cell) and the chain's
  progress.
- `binaries.txt` - the hashes, sizes and build times of every binary.
- `driver/` - the chain scripts that ran the cells (`chain.sh`: the close chain; `bisect.sh`, `bisect2.sh`, `aligned.sh`,
  `rollfix.sh`: the follow-ups; `profile.sh`, `profile2.sh`; `build_close.sh`, `build_aligned.sh`: the builds from
  exports; `watchdog.sh`, `start.sh`), `lockcell.py` (BP-S1's lock-cell wrapper, from
  `/home/cdkbs/bench-runs/bp-s1/v2/scripts/`), `bn_census.py.at-1fb71d1a` and `ba_census.py.at-1fb71d1a` (the drivers as
  at the worktree commit the results file was written at; `tools/` carried no uncommitted change), and the first table
  sketches (`table*.py`, `lock_table.py`, `profdiff.py`).
- `tables/` - the scripts that produced the results file's tables and quoted numbers. `lib.py` reads the original
  directory (`OUT` at its top; point it at a copy of `out/` to re-run); `gen1.py`-`gen4.py` write every table fragment
  (`gen1`: the `cores = 1` A/B tables; `gen2`: the ladder, the pooled-by-binary table, `cores = 4`, the lock cell;
  `gen3`: the host table and the profile table, which reads `prof2.json`; `gen4`: the comparison with BP-S3's file and
  the fence-premium model). `prof2.py` regenerates `prof2.json` from the sampler dumps through
  `tools/kds_sampler_report.py --top 400`. `tail1.py`-`tail3.py` are the second-server stall numbers of §4,
  `pooled.py` the by-binary means, `ba2.py`/`ba3.py` the `cores = 4` counters, `lock1.py` the lock-cell numbers,
  `wall1.py` the wall-clock numbers and the competing-process check, `chk.py` the end-state and host checks. Run them as
  `nice python3 -I`.

Not archived: data files and WAL segments, the build trees, the exports (byte-identical to `git archive` of their
commits) and the binaries.
