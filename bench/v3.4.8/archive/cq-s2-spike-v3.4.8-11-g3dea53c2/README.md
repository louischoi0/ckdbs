# Raw outputs of CQ-S2 (v3.4.8-11-g3dea53c2)

Results file: `../../results-cq-s2-spike-v3.4.8-11-g3dea53c2.md`. It
quotes **pass 2**; pass 1 is kept because it ran first, agrees, and is
where the spike's false refusal was found.

- `pass2/`: the quoted pass, at `3dea53c2`. Per server kind and
  repetition, the driver's per-cell summaries (`tools/cq_s0_premise.py`):
  `m1-plain-N.json` (the spike's binary, no mark: hot, hot-last piped,
  disjoint at `k` in 2, 8), `m1-marked-N.json` (the same binary with
  `cq_acct.c` marked: the hot arm, which is the reserved arm),
  `ab-base-N.json` (the base binary, the three arms), `m4p-plain.json`
  and `m4p-marked.json` (the 1.2 s pause), `m2-plain.json` and
  `m2-marked.json` (`strict`). The `.txt` beside each is the driver's
  stdout; `<label>.server.json` is each server's sidecar (binary hash,
  config, `df -T`, load at start and stop); `status.txt` is the
  schedule's timeline, `watchdog.txt` the 60 s watch, `wait_quiet.txt`
  the wait before the start.
- `pass1/`: the first pass, at `f72b8232`. Its base cells ran `k = 8` and
  two arms only, and its watchdog log is missing: the first watchdog
  could not read the driver's fractional heartbeat and stopped, and the
  corrected one started mid-pass.
- `scripts/`: `run_s2.sh` (the schedule as pass 2 ran it), `launch.sh`,
  `wait_quiet.sh`, `watchdog3.sh`, and `gen_tables.py` with `cells.py`,
  which printed the results file's tables from a pass directory. The
  server launcher is CP-S0's, unmodified:
  `../../../v3.4.7/archive/cp-s0-premise-v3.4.7-1-gcc54a296/scripts/m1m4_srv.py`.
- Not archived: data files, WAL segments, server logs, the base binary's
  build tree (a release build of `git archive e698fce8`), and the smoke
  pass that proved the schedule (none of its numbers is quoted).
