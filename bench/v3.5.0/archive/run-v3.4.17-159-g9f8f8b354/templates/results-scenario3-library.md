# Scenario 3 (library) at `v3.4.17-159-g9f8f8b354`

**One line.** @@S3_ONE_LINE@@

Driver: `tools/scenario3_library.py` of the measured commit, unmodified (byte-identical to `07f4552a`'s), `--seed 1 --server-indexes on`, `--loans` and `--index-mode` per cell, `--assert-index-reads` for `all`. Comparator run: `results-scenario3-library-v3.5.0.md` (2026-11-20, `07f4552a`, **another host**), so a ratio against it is host plus engine. The engine reading is the interleaved A/B on this host (sections 5 and 6).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-11-26; main cells 21:11:57 to 21:15:08 UTC; unpinned A/B cells inside 21:19:44 to 21:41:59 UTC; pinned A/B cells inside @@PIN_WINDOW@@ |
| Version, commit, binary, host, device, budget | stamped once in `summary-scenarios-v3.4.17-159-g9f8f8b354.md`, section Stamp |
| Server config | the common config, `durability = group`, `cores` 1 or 8; port 15720 |
| Driver | exit 0 in every cell, `--assert-index-reads` included (every indexed shape compiled to an index step); `verify_problems` empty in {{s3-verify}} |

## 2. What was run

The `v3.5.0` run's cells, unchanged in arguments and suffixes: 200, 1,000 and 10,000 loans, unindexed (`none`) and indexed (`all`), at `cores = 1` three times and `cores = 8` three times, and the A/B (10,000 loans unindexed, three interleaved pairs). **Added**: an indexed A/B cell (10,000 loans, `all`), and the whole A/B again with the server pinned to the P-cores (@@PIN_PAIRS@@ pairs). Cut: the plan replies (as at `v3.5.0`) and PostgreSQL.

**A cell here is short.** A shape is 200 statements, 7 ms to 66 ms of work on this host at 10,000 loans unindexed, so one run's mean is set by where the server's thread ran for those milliseconds.

## 3. Throughput, now against the `v3.5.0` run

qps derived as 1,000,000 / mean latency, median of the runs. **The last column is this host over another host**: no cell of it is a finding about the engine.

#### cores = 1

{{s3-tp-c1}}

#### cores = 8

{{s3-tp-c8}}

## 4. Percentiles

Run 1 of `s3-c1-n10000-none`:

{{s3-pct-10000-none}}

Run 1 of `s3-c1-n10000-all`:

{{s3-pct-10000-all}}

## 5. The engine, isolated, unpinned: `07f4552a` against `9f8f8b354`, interleaved on this host

Three interleaved pairs, old first, each on a fresh server and data file.

10,000 loans, unindexed:

{{ab-s3-none}}

10,000 loans, indexed:

{{ab-s3-all}}

**The indexed table is a placement reading, not an engine reading.** Its new-over-old ratios sit at 0.58x to 0.80x on every shape at once, the primary-key lookup (`pk-user`, 0.62x) and the load among them, and every row but one fails the spread rule. A ratio that is the same on a tuple key descent, an index probe, a join and a walk is the ratio of the cores the two arms' servers ran on (the P-cores against the E-cores, about 1.5x apart), which is what section 6 removes.

## 6. The engine, isolated, pinned: the same A/B with the server on the P-cores

Server under `taskset -c 0-7`, driver under `taskset -c 8-23`, @@PIN_PAIRS@@ interleaved pairs, old first.

10,000 loans, unindexed:

{{pin-ab-s3-none}}

10,000 loans, indexed:

{{pin-ab-s3-all}}

## 7. What the run says about the engine

@@S3_ENGINE@@

## 8. Noise floor, contamination and host

- **The unpinned cells are bimodal on this host** (hybrid CPU, governor `powersave`; summary, Stamp, the Host row). At one core the median spread over the 72 rows of section 3 is 29.6 % (the `v3.5.0` run's 6.2 %), and at eight cores 20.2 % (8.3 %).
- Three main cells carry foreign samples of 50 % or more of one CPU, all another session's `cc1plus`, kept and disclosed: `r2-s3-c1-n10000-none` (2 of 2 samples), `r2-s3-c8-n10000-all` (2 of 3) and `r3-s3-c8-n10000-none` (3 of 3). @@S3_PIN_CONTAM@@
- Gate waits on scenario 3 cells: `s3-c1-n1000-all` 6 s, `r2-s3-c1-n10000-all` 25 s, `r3-s3-c1-n200-none` 39 s, `r3-s3-c8-n10000-all` 21 s; every other scenario 3 gate of the main run and the unpinned A/B 0 s@@S3_PIN_GATE@@.

## 9. Archive

`bench/v3.5.0/archive/scenario3-v3.4.17-159-g9f8f8b354/` (per-cell JSON, driver stdout, prechecks, gate waits, start and end times, server configs and logs) and `bench/v3.5.0/archive/run-v3.4.17-159-g9f8f8b354/`. No data file is kept.
