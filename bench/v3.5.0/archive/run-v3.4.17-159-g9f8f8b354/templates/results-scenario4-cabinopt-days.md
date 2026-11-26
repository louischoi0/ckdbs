# Scenario 4 (cabin optimizer over business days) at `v3.4.17-159-g9f8f8b354`

**One line.** @@S4_ONE_LINE@@

Driver: `tools/scenario4_cabinopt_days.py` of the measured commit, unmodified (byte-identical to `07f4552a`'s), `--suffix a`, three servers (`off`, `on`, `declared`) with their PIDs for the CPU readings, both with `decay_half_life = 5` and `cabin_optimizer_snapshot_interval_ms = 500`: **run A** at the server defaults (`s4`, and again as `r2-s4` because the first overlapped a build; section 9) and **run B** with `cabin_optimizer_cooldown_half_lives = 2` (`cd2-s4`). Comparator run: `results-scenario4-cabinopt-days-v3.5.0.md` (2026-11-20, `07f4552a`, **another host**). The engine reading is one run of the `07f4552a` binary on this host (`ab-old1-s4`, section 6).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-11-26; run A 21:15:10 to 21:19:43 UTC; the `07f4552a`-binary control 21:42:01 to 21:46:34 UTC; run B 21:46:55 to 21:51:28 UTC; run A again @@S4R_WINDOW@@ |
| Version, commit, binary, host, device, budget | stamped once in `summary-scenarios-v3.4.17-159-g9f8f8b354.md`, section Stamp |
| Server config | the common config plus `decay_half_life = 5`, `cabin_optimizer_snapshot_interval_ms = 500`, and in run B `cabin_optimizer_cooldown_half_lives = 2`; `cores = 1`, `durability = group`; ports 15751 to 15753 |
| Driver | exit 0 in every run; `client_errors` 0, `slot_overruns` 0; elapsed 272.5 s to 273.1 s |

## 2. What was run

Per day, three days: an open (240 inserts into the day's board), a paced 45 s trading session (2,400 hot-board probes, 396 probes on each of three tapes of 200, 1,000 and 10,000 rows, 240 pk controls, per arm, in 12 blocks), a close (`COUNT` / `SUM` / `GROUP BY`, three rounds) and a 45 s idle night. qps is 1,000,000 / mean latency. Cut, as at `v3.5.0`: the old-and-new interleaved A/B of six runs (one `07f4552a`-binary run instead) and PostgreSQL (no server).

## 3. Throughput and latency by arm

p50 and derived qps per group, day and arm; the ratios are of mean latencies (`on over off` = off mean / on mean, above 1 is faster); the last two columns are the same two ratios in the `v3.5.0` run.

Run A (`s4`; overlapped another session's build, section 9):

{{s4-s4}}

Run A again (`r2-s4`):

{{s4-r2-s4}}

Run B (`cd2-s4`, `cabin_optimizer_cooldown_half_lives = 2`):

{{s4-cd2-s4}}

## 4. Server CPU

Run A (`s4`):

{{s4-cpu-s4}}

Run A again (`r2-s4`):

{{s4-cpu-r2-s4}}

Run B:

{{s4-cpu-cd2-s4}}

## 5. The controller's lifecycle

- Run A, final snapshot of the `on` arm: {{s4-meta-s4}}
- Run A again: {{s4-meta-r2-s4}}
- Run B: {{s4-meta-cd2-s4}}

In all 27 evidence snapshots of each run, every managed Cabin holds one page, `extends` is 0, and `hint_fail_pct` and `coverage_miss_pct` are NULL on every entry (a NULL share is a zero lookup count). This is `cabin-controller.md` PO11 unchanged since `v3.5.0`: the controller creates a Cabin on heat, no statement's step probes it without an administrator's plan naming it, and this driver writes no `CREATE PLAN`.

## 6. The `07f4552a` binary on this host

One run of the driver at the defaults on the `07f4552a` binary (`ab-old1-s4`):

{{s4-ab-old1-s4}}

{{s4-cpu-ab-old1-s4}}

{{s4-meta-ab-old1-s4}}

New over old, mean latency per group, arm and day (old mean / new mean, above 1 is the new engine faster), run A (`s4`) against the control:

{{s4-engine}}

Run A again (`r2-s4`) against the control:

{{s4-engine-r2}}

Each cell is one run against one run, so no cell carries a claim by this directory's spread rule.

## 7. Correctness

The 18-statement byte comparison across the three arms reported 0 mismatches on every day of every run, the `07f4552a`-binary run included (section 5 and 6's lines). No client error and no slot overrun in any run.

## 8. What the run says about the engine

@@S4_ENGINE@@

## 9. Noise floor, contamination and host

- **Run A (`s4`) overlapped another session's full build**: 43 of its 123 monitor samples carry foreign processes at 50 % or more of one CPU, on average 8.3 CPUs' worth (`cc1plus`, `as`, `ld`, `cmake`, then `kds_tests`). The driver is paced, so the run completed with no slot overrun, and its numbers are kept and disclosed; @@S4_RERUN@@
- Run B (`cd2-s4`) carries 19 of 122 samples of another session's `kds_tests` at about one CPU. The `07f4552a`-binary control carries none.
- **On this host `on over off` is not level on the boards on any binary** (1.12x to 1.24x in means in run A, run B and the `07f4552a` control alike), while the two arms' p50 agree to 3 µs in 200 µs on every day. The difference is in the `off` arm's tail, the arm each block probes first; the controller's Cabins record no lookup in either binary (section 5), so the ratio is not a Cabin serving. It is not read as a finding. The `pk` control's `on over off` runs 0.91x to 1.04x in run A, the floor a ratio is read against.
- Gate waits: @@S4_GATE@@

## 10. Archive

`bench/v3.5.0/archive/scenario4-v3.4.17-159-g9f8f8b354/` (per-run JSON with every evidence snapshot, driver stdout, prechecks, gate waits, start and end times, the three servers' configs and logs per run) and `bench/v3.5.0/archive/run-v3.4.17-159-g9f8f8b354/`. No data file is kept.
