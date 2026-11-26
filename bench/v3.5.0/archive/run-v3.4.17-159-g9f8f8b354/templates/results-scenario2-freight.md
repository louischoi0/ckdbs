# Scenario 2 (freight) at `v3.4.17-159-g9f8f8b354`

**One line.** @@S2_ONE_LINE@@

Driver: `tools/scenario2_freight.py` of the measured commit, unmodified (byte-identical to `07f4552a`'s), `--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync`, schema, load and run as three invocations on one server, as the `v3.5.0` run. Comparator run: `results-scenario2-freight-v3.5.0.md` (2026-11-20, `07f4552a`, **another host**), so a ratio against it is host plus engine. The engine reading is the interleaved A/B on this host (sections 7 and 8).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-11-26; main cells 21:09:39 to 21:11:56 UTC; unpinned A/B cells inside 21:19:44 to 21:41:59 UTC; extras (repeatable-read, 16 bookers) 21:46:35 to 21:46:54 UTC; pinned A/B cells inside @@PIN_WINDOW@@ |
| Version, commit, binary, host, device, budget | stamped once in `summary-scenarios-v3.4.17-159-g9f8f8b354.md`, section Stamp |
| Server config | the common config; `cores` and `durability` per cell; ports 15704 to 15707 |
| Driver | exit 0 in every cell, and in every `--schema-only` and `--load-only` invocation |

## 2. What was run

The `v3.5.0` run's cells, unchanged in arguments, suffixes and order: four matrix cells (`cores` 1 or 8, `group` or `strict`) and two single-booker cells, three times each; the booker sweep (1, 2, 4 and 16 bookers at eight cores, once each); the 32-booker saturation cells (once each, with a 1 s host CPU sampler); repeatable-read at one and eight cores and 16 bookers again (extras, once each); and the A/B (8 bookers `c1-g` and 1 booker `c1-s`, three interleaved pairs). **Added**: an eight-core A/B cell (8 bookers `c8-g`), and the whole A/B again with the server pinned to the P-cores (@@PIN_PAIRS@@ pairs). **Cut**, as at `v3.5.0`: the extreme cells (20,000 bookings, 32 bookers, one and two client processes) and PostgreSQL (no server on this host).

**A cell here is short.** 3,000 bookings take 0.4 s at eight cores and 0.9 s at one on this host (they took 1.1 s and 2.6 s on the `v3.5.0` run's), so one cell is a sample of under a second and its run-to-run spread is wide.

## 3. Throughput

**The last column is this host over another host**: no cell of it is a finding about the engine.

{{s2-tp}}

## 4. The booker sweep and saturation

Eight bookers are the matrix cells (three runs); every other row is one run.

{{s2-sweep}}

The extras:

{{s2-extras}}

## 5. Where the time goes

Mean booking latency and the commit's share of it, run 1:

{{s2-commit}}

Every statement of the booking transaction, run 1 of `s2-c8-g` (the share is of the summed statement time, the reporter's three reads included):

{{s2-time-s2-c8-g}}

Run 1 of `s2-c1-g`:

{{s2-time-s2-c1-g}}

Percentiles, run 1 of `s2-c8-g`:

{{s2-pct-s2-c8-g}}

## 6. Correctness: the invariant failures

{{s2-outcomes}}

With one booker every run passes 400 of 400, and under `--isolation repeatable-read` both cells pass 400 of 400 (section 4's extras). Under the default isolation with more bookers the `--verify 100` pass fails as at `v3.5.0` (`I1 operation <n>: booked_cbm=..., SUM(freights.cbm)=...`): the driver writes `SET booked_cbm = <literal>` from a value it read earlier in the transaction, the client-side lost update its source names. It is not engine corruption. **One booking was abandoned**: `r2-s2-c8-s` (eight cores, `strict`, run 2) lost one booking to `--max-retries 5` after 55 conflicts; no `v3.5.0` cell abandoned one. One event in 28 main cells is not a rate, and @@S2_ABANDON@@

## 7. The engine, isolated, unpinned: `07f4552a` against `9f8f8b354`, interleaved on this host

The `07f4552a` binary built on this host (sha256 `03c8c2e9...`) and the measured binary (sha256 `a333ee48...`), one driver, interleaved old, new, old, new, old, new, each on a fresh server and data file.

{{ab-s2-tps}}

Outcomes per run:

{{ab-s2-outcomes}}

Statements, 8 bookers at one core, `group`:

{{ab-s2-phases-s2-c1-g}}

## 8. The engine, isolated, pinned: the same A/B with the server on the P-cores

Server under `taskset -c 0-7`, driver (and its booker processes) under `taskset -c 8-23`, @@PIN_PAIRS@@ interleaved pairs, old first.

{{pin-ab-s2-tps}}

Outcomes per run:

{{pin-ab-s2-outcomes}}

Statements, 8 bookers at one core, `group`:

{{pin-ab-s2-phases-s2-c1-g}}

Statements, 1 booker at one core, `strict`:

{{pin-ab-s2-phases-b1-s2-c1-s}}

Statements, 8 bookers at eight cores, `group`:

{{pin-ab-s2-phases-s2-c8-g}}

## 9. What the run says about the engine

@@S2_ENGINE@@

## 10. Noise floor, contamination and host

- **The unpinned cells are bimodal on this host** (hybrid CPU, governor `powersave`; summary, Stamp, the Host row): `s2-c8-s` ran 8,175, 4,031 and 7,521 tps and `b1-s2-c1-g` 1,821, 1,098 and 1,271 tps, with no foreign sample of 50 % or more of one CPU inside those cells. Four of the six matrix cells spread over 20 % (the `v3.5.0` run's spread 2.1 % to 5.1 %).
- One main cell carries a foreign sample of 50 % or more of one CPU: `s2-c1-g` run 1 (another session's `python3`, 65 %, one sample of two, 21:09:48 to 21:09:51 UTC); kept and disclosed. @@S2_PIN_CONTAM@@
- Gate waits on scenario 2 cells: `ab-new1-s2-c1-g` 720 s (another session's build and test cycle); every other scenario 2 gate of the main run and the unpinned A/B 0 s@@S2_PIN_GATE@@.

## 11. Archive

`bench/v3.5.0/archive/scenario2-v3.4.17-159-g9f8f8b354/` (per-cell JSON, driver stdout, the schema and load invocations' output, prechecks, gate waits, start and end times, the saturation cells' CPU samples, server configs and logs) and `bench/v3.5.0/archive/run-v3.4.17-159-g9f8f8b354/`. No data file is kept.
