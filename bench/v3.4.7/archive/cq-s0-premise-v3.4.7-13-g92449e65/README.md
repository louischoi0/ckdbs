# Raw outputs of CQ-S0 (v3.4.7-13-g92449e65)

Results file: `../../results-cq-s0-premise-v3.4.7-13-g92449e65.md`. It
quotes **pass 2**; pass 1 is kept because it ran first and agrees.

- `pass2/`: the quoted pass. `m1-group.json` (M1, 45 cells: five arms, `k`
  in 0, 2, 8, three repetitions), `m4p-group.json` (M4', 1 cell) and
  `m2-strict.json` (M2, 15 cells) are the driver's per-cell summaries -
  throughput, latency percentiles, refusals, each session's core at its
  start and end, the server's CPU, the oracle's verdict, the host's load
  and competing builds, the `sys.lock_waits` sample. The `.txt` beside
  each is the driver's stdout; `status.txt` is the schedule's timeline;
  `wait_quiet.txt` is the wait before it; `<label>.server.json` is each
  server's sidecar (binary hash, config, `df -T`, load at start and stop);
  `.start.json` and `.stop.json` are the launcher's replies.
- `pass1/`: the first pass, the driver in `tools-at-run/pass1/`: `k` in 0
  and 8, four arms under `strict`, the oracle on the hot row only, no
  cores, no server CPU.
- `scripts/`: `run_s0.sh` (the schedule as pass 2 ran it; pass 1's differed
  in `k` and in M2's arms), `wait_quiet.sh`, and `gen_tables.py` with
  `fill.py`, which built the results file's tables from `pass2/`. The
  server launcher is CP-S0's, unmodified:
  `../cp-s0-premise-v3.4.7-1-gcc54a296/scripts/m1m4_srv.py`.
- `tools-at-run/cq_s0_premise.py`: the driver as pass 2 ran it,
  byte-identical to the committed `tools/cq_s0_premise.py`.
- Not archived: data files, WAL segments, server logs, and the two smoke
  passes that proved the driver (none of their numbers is quoted).
