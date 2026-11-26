# Scenario 1 (backtest) at `v3.4.17-159-g9f8f8b354`

**One line.** @@S1_ONE_LINE@@

Driver: `tools/scenario1_backtest.py` of the measured commit, unmodified (byte-identical to `07f4552a`'s), with `--seed 1 --bars-clustered btree --verify --sync --analyze`. Measured directly by CLA from a scheduled session (no `ck-tester` agent was spawned; the tester's rules in `.claude/agents/ck-tester.md` were followed through the run scripts). Comparator run: `results-scenario1-backtest-v3.5.0.md` (2026-11-20, `07f4552a`, **another host**: an 8-CPU Xeon Platinum 8581C VM), so a ratio against it is host plus engine and is not read as an engine change. The engine reading is the interleaved A/B on this host against a `07f4552a` binary built here (sections 6 and 7).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-11-26; main cells 21:05:49 to 21:09:38 UTC; unpinned A/B cells inside 21:19:44 to 21:41:59 UTC; pinned A/B cells inside @@PIN_WINDOW@@ |
| Version, commit, binary, host, device, budget | stamped once in `summary-scenarios-v3.4.17-159-g9f8f8b354.md`, section Stamp |
| Server config | the common config; `cores` 1 or 8, `durability` group or strict per cell; port 15710 |
| Driver | exit 0 in every cell; `verify_problems` empty: {{s1-verify}} |

## 2. What was run

The `v3.5.0` run's cells, unchanged in arguments and suffixes: three cells (`c1-g`, `c8-g`, `c1-s`: cores and durability) three times each, the size sweep (252, 1,008 and 10,080 bars at `c1-g`) three times each, and the engine A/B (`c1-g`, three interleaved pairs, old first). **Added**: the same A/B again with the server pinned to the P-cores and the driver to the E-cores (@@PIN_PAIRS@@ interleaved pairs), because this host is a hybrid CPU and the unpinned cells are bimodal across runs (section 8). Cut: the join-order probe (as at `v3.5.0`) and PostgreSQL (no server on this host).

## 3. Results

Warm and cold qps from the driver's sweep, median of three runs; spread = (max - min) / median. **The last column is this host over another host**: no cell of it is a finding about the engine.

#### cell c1-g: cores = 1, group

{{s1-mat-c1-g}}

#### cell c8-g: cores = 8, group

{{s1-mat-c8-g}}

#### cell c1-s: cores = 1, strict

{{s1-mat-c1-s}}

#### phases of `s1-c1-g`

{{s1-phases}}

## 4. Percentiles, run 1 of `s1-c1-g`

{{s1-pct-c1-g}}

## 5. The size sweep

{{s1-size}}

## 6. The engine, isolated, unpinned: `07f4552a` against `9f8f8b354`, interleaved on this host

The `07f4552a` binary built on this host for this run (Release, sha256 `03c8c2e9...`) and the measured binary (sha256 `a333ee48...`), one driver, interleaved old, new, old, new, old, new, each on a fresh server and data file. A row reads `spread > 20 %, no claim` when either arm's three runs spread over 20 %, and on this host most rows do: the server's thread lands on a P-core or an E-core per run, and the two differ by about 1.5x.

{{ab-s1-sweep}}

{{ab-s1-phases}}

## 7. The engine, isolated, pinned: the same A/B with the server on the P-cores

Server under `taskset -c 0-7` (the eight P-cores), driver under `taskset -c 8-23` (the sixteen E-cores), @@PIN_PAIRS@@ interleaved pairs, old first; everything else as section 6.

{{pin-ab-s1-sweep}}

{{pin-ab-s1-phases}}

## 8. What the run says about the engine

@@S1_ENGINE@@

## 9. Noise floor, contamination and host

- **The host is a hybrid CPU** (Intel Core Ultra 7 270K Plus: 8 P-cores at up to 5.4 to 5.5 GHz, 16 E-cores at up to 4.7 GHz, `intel_pstate` active, governor `powersave`), and nothing in the unpinned cells pins a thread. The unpinned spreads here are 3 to 15 times the `v3.5.0` run's on the same cells (median spread of the `c1-g` warm shapes 22.7 % here against 4.9 % there), and `point-join` warm spreads 79.1 %.
- **Another session was building and testing in this repository throughout** (worktree `dt-write-path-yield`: `cmake`, `cc1plus`, `ld`, `ctest`, `kds_tests`). The quiet gate held a cell's start while such a process ran; a process that started inside a cell is in the monitor log. Two scenario 1 cells carry foreign samples of 50 % or more of one CPU and are kept and disclosed: `s1-c8-g` run 1 (`cc1plus` and `ld`, 5 of its 8 samples, 21:06:08 to 21:06:26 UTC) and `ab-new1-s1-c1-g` (`cc1plus`, 2 of its 9 samples, 21:20:34 to 21:20:53 UTC). @@S1_PIN_CONTAM@@
- Gate waits on scenario 1 cells: `s1-c1-g` 9 s, `s1-c1-s` 34 s, `ab-old2-s1-c1-g` 315 s (the hold that prompted the gate's change, summary section Quiet gate); every other scenario 1 gate 0 s@@S1_PIN_GATE@@.

## 10. Archive

`bench/v3.5.0/archive/scenario1-v3.4.17-159-g9f8f8b354/` (per-cell JSON, driver stdout, prechecks, gate waits, start and end times, server configs and logs; the A/B cells `ab-*` and the pinned ones `pin-ab-*` among them) and `bench/v3.5.0/archive/run-v3.4.17-159-g9f8f8b354/` (the run scripts, watchdogs, monitor log, status timeline, binary hashes, the table generator `gen.py` and its output under `tables/`). No data file is kept.
