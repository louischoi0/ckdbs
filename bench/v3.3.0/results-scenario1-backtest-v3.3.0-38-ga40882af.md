# Scenario 1 (backtest) at `v3.3.0-38-ga40882af`

**One line.** Same engine binary as the morning's run, new client decoder: every shape that returns many rows runs 1.7x to 1.9x faster, and nothing else moved
by more than 2 % to 6 % (A/B, section 9). On 60,480 bars a 200-row key range (`bar-range`) runs 1,260.3 qps against 684.4 qps in the morning,
a 7,560-row symbol history 27.3 qps against 13.8 qps, and the 359-row `model-join` 1,030.2 qps against 614.8 qps.
Interleaved on the same host with the old client (three A/B pairs) the same three shapes are 1.83x, 1.92x and 1.75x,
and the cost of a returned row falls from 6.9 to 3.6 µs (`bar-range`) and from 9.5 to 4.8 µs (`symbol-history`) against PostgreSQL 18.6's 3.2 and 4.1 µs. That puts the many-row shapes at
0.90x (`bar-range`), 0.85x (`symbol-history`) and 0.94x (`model-join`) of PostgreSQL's, from 0.49x, 0.43x and 0.56x.
**Two caveats govern the rest of the file.** First, the one-core scan shapes (`day-slice`, `cross-join`, `backtest-read`) read 13 % to 16 % *below* the morning, and the A/B shows the cause is not the client:
the old client reads 493 qps on `day-slice` in this window, the new 504 qps, the morning 591 qps (section 9). Host or placement, not separated.
Second, PostgreSQL is reused from the morning's run, so a KDS/PG ratio on a scan shape carries that drift; the ratios on the many-row shapes (where the client moved the number by a factor of two) do not depend on it in any way that changes a reading.

Driver: `tools/scenario1_backtest.py`, unmodified, `--seed 1 --bars-clustered btree --verify --sync --analyze` (flags in `bench/docs/`). Measured by `ck-tester`.
Comparator (this engine's own last run, **same binary, old client**): `bench/v3.3.0/results-scenario1-backtest-v3.3.0-33-g9682b725.md`. PostgreSQL 18.6: the same file's twin runs
(`tools/pg_scenario1_backtest.py`, three runs per cell), **not re-run**: budget lifted by the operator for this measurement; PostgreSQL reused from the same-day run at v3.3.0-33-g9682b725.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; matrix cells 06:46:48 to 06:53:37 UTC (nine cells); size sweep 07:26:04 to 07:28:05 UTC (nine cells); A/B pairs 07:29:41 to 08:02:01 UTC (six cells); join-order probe 08:08:19 to 08:16:49 UTC (six cells). Per-cell windows: section 11 |
| Worktree, commit, engine binary, client, comparator, budget and everything else common to the run | stamped once in `summary-scenarios-v3.3.0-38-ga40882af.md`, section Stamp |
| Driver | **unmodified** `tools/scenario1_backtest.py`; exit code 0 and no verify problem in every KDS cell (`verify_problems` empty in 30 of 30) |

## 2. What was run

The morning's plan, every cell, with the same repeats (`run_all.sh` in the archive is the morning's, with its PostgreSQL stage removed and the A/B stage added):

| cell | cores | durability | runs | arguments |
|---|---|---|---|---|
| `s1-c1-g`, `r2-`, `r3-` | 1 | group | 3 | `--seed 1 --bars-clustered btree --verify --sync --analyze` (60,480 bars) |
| `s1-c8-g`, `r2-`, `r3-` | 8 | group | 3 | same |
| `s1-c1-s`, `r2-`, `r3-` | 1 | strict | 3 | same |
| `s1s-n252-c1-g`, `s1s-n1008-c1-g`, `s1s-n10080-c1-g` (+ `r2-`, `r3-`) | 1 | group | 3 each | same plus `--years 1 --symbols 1 --exchanges 1` (252 bars), `--years 1 --symbols 4 --exchanges 2` (1,008), `--years 5 --symbols 8 --exchanges 2` (10,080) |
| `ab-old{1,2,3}-s1-c1-g`, `ab-new{1,2,3}-s1-c1-g` | 1 | group | 3 pairs, interleaved old, new, old, new, old, new | the `s1-c1-g` arguments; `old` = `a40882af^`'s `tools/kwp.py` and `tools/ckdbs_cli.py` from `oldtools/`, `new` = this worktree's |
| `pl-s1-planned-r1..3`, `pl-s1-written-r1..3` | 1 | group | 3 each | the `s1-c1-g` arguments, the second arm's server started with `join_order = written`, ANALYZE replies dumped |
| PostgreSQL | - | - | not run | taken from `bench/v3.3.0/archive/scenario1-v3.3.0-33-g9682b725/json/pg-s1*.json` (morning, 04:25 to 04:33 UTC) |

**Row-count axis (rule 9).** Bars = 252 sessions per year x years x symbols; `daily_stats` has one row per bar. The driver's flags map to 252, 1,008, 10,080 and (default `--years 30 --symbols 8`) 60,480 bars.
A returned-row count is fixed per shape: `bar-range` 200 rows, `symbol-history` one symbol's sessions (252, 252, 1,260 and 7,560 rows at the four sizes), `model-join` 359, `day-slice` 8.
A cell is one fresh server and a fresh data file, deleted afterwards.

Cut: nothing from the morning's KDS plan. The morning's three `bar-lookup` re-run cells were not repeated (its verdict, a bimodal shape, is read again in section 3 from this run's own spreads).

## 3. Results

The QPS sweep (the driver's controlled matrix, 100 statements per cell, 8 distinct arguments in the warm cases), median of three runs. `cold` = every argument seen for the first time, `warm` = the same few
arguments cycled, `cabin` = warm with a Cabin declared on the filter column. "Read" applies the rule of `summary-scenarios-v3.3.0-38-ga40882af.md` ("What counts as a finding"): a delta below the larger spread is not a finding,
a spread above 20 % carries no claim. **A delta against the morning is client plus host; only section 9 separates them**, and its A/B says where the host part is not zero.

#### cell c1-g: cores = 1, group

| shape | cold now | warm now | spread | warm morning | spread | warm delta | read | cabin now | cabin morning | cabin delta |
|---|---|---|---|---|---|---|---|---|---|---|
| bar-lookup | 12,575.3 qps | 12,619.9 qps | 29.0 % | 11,263.5 qps | 13.0 % | +12.0 % | spread > 20 %, no claim | - | - | - |
| bar-range | 1,237.2 qps | 1,260.3 qps | 4.3 % | 684.4 qps | 5.7 % | +84.1 % | **finding** | - | - | - |
| day-slice | 496.8 qps | 508.7 qps | 18.6 % | 591.3 qps | 6.9 % | -14.0 % | inside the spread | 8,109.3 qps | 6,557.7 qps | +23.7 % |
| symbol-history | 27.5 qps | 27.3 qps | 0.7 % | 13.8 qps | 6.5 % | +97.8 % | **finding** | 26.2 qps | 13.6 qps | +92.6 % |
| cross-join | 484.1 qps | 496.9 qps | 20.6 % | 575.8 qps | 1.8 % | -13.7 % | spread > 20 %, no claim | 6,722.3 qps | 5,570.3 qps | +20.7 % |
| point-join | 12,305.5 qps | 10,589.7 qps | 37.8 % | 12,674.6 qps | 2.4 % | -16.4 % | spread > 20 %, no claim | - | - | - |
| model-join | 1,016.6 qps | 1,030.2 qps | 3.4 % | 614.8 qps | 6.5 % | +67.6 % | **finding** | 1,041.0 qps | 618.0 qps | +68.4 % |

#### cell c8-g: cores = 8, group

| shape | cold now | warm now | spread | warm morning | spread | warm delta | read | cabin now | cabin morning | cabin delta |
|---|---|---|---|---|---|---|---|---|---|---|
| bar-lookup | 12,538.5 qps | 12,463.9 qps | 2.8 % | 12,349.9 qps | 0.2 % | +0.9 % | inside the spread | - | - | - |
| bar-range | 1,234.1 qps | 1,223.2 qps | 2.8 % | 665.2 qps | 1.8 % | +83.9 % | **finding** | - | - | - |
| day-slice | 479.8 qps | 480.6 qps | 18.8 % | 554.4 qps | 2.3 % | -13.3 % | inside the spread | 7,875.8 qps | 6,248.3 qps | +26.0 % |
| symbol-history | 26.9 qps | 26.7 qps | 0.4 % | 13.8 qps | 0.7 % | +93.5 % | **finding** | 25.5 qps | 13.4 qps | +90.3 % |
| cross-join | 462.9 qps | 472.8 qps | 17.6 % | 548.1 qps | 1.2 % | -13.7 % | inside the spread | 6,360.4 qps | 5,271.2 qps | +20.7 % |
| point-join | 11,835.3 qps | 11,908.3 qps | 0.6 % | 11,776.5 qps | 1.1 % | +1.1 % | **finding** | - | - | - |
| model-join | 962.3 qps | 991.4 qps | 2.0 % | 583.3 qps | 0.9 % | +70.0 % | **finding** | 1,001.1 qps | 579.3 qps | +72.8 % |

#### cell c1-s: cores = 1, strict

| shape | cold now | warm now | spread | warm morning | spread | warm delta | read | cabin now | cabin morning | cabin delta |
|---|---|---|---|---|---|---|---|---|---|---|
| bar-lookup | 12,759.3 qps | 13,025.9 qps | 32.7 % | 12,867.1 qps | 28.5 % | +1.2 % | spread > 20 %, no claim | - | - | - |
| bar-range | 1,221.0 qps | 1,224.3 qps | 9.8 % | 678.9 qps | 3.3 % | +80.3 % | **finding** | - | - | - |
| day-slice | 491.5 qps | 504.8 qps | 1.6 % | 599.1 qps | 1.0 % | -15.7 % | **finding** | 8,030.8 qps | 6,512.2 qps | +23.3 % |
| symbol-history | 27.2 qps | 27.1 qps | 8.1 % | 14.2 qps | 4.9 % | +90.8 % | **finding** | 26.2 qps | 13.8 qps | +89.9 % |
| cross-join | 480.0 qps | 486.7 qps | 2.7 % | 578.0 qps | 3.3 % | -15.8 % | **finding** | 6,324.6 qps | 5,453.6 qps | +16.0 % |
| point-join | 8,543.6 qps | 9,020.4 qps | 43.6 % | 12,216.4 qps | 31.2 % | -26.2 % | spread > 20 %, no claim | - | - | - |
| model-join | 975.0 qps | 1,010.9 qps | 7.8 % | 605.8 qps | 4.8 % | +66.9 % | **finding** | 1,035.1 qps | 606.2 qps | +70.8 % |

**The shapes that return rows moved by a factor of two; the shapes that return a handful did not move, or lost 13 % to 16 % to a host effect.** `bar-range`, `symbol-history` and `model-join` are
+84 % / +98 % / +68 % at `c1-g`, +84 % / +94 % / +70 % at `c8-g` and +80 % / +91 % / +67 % at `c1-s`, every one far above its spread. `day-slice` and `cross-join` are -14 % at `c1-g`, -13 % / -14 % at `c8-g` and
-16 % / -16 % at `c1-s`: by the rule, inside the spread or past the 20 % line at `c1-g` (spreads 18.6 % and 20.6 %) and `c8-g` (18.8 % and 17.6 %: one slow run in three) and a finding at `c1-s` (spreads 1.0 % to 2.7 %). Their Cabin arms, which are
statements returning 8 rows by a cache hit, are **+16 % to +26 %** (a round trip of 8 rows decodes cheaper), so the same cells moved in opposite directions: the scan got slower and the reply got cheaper. Section 9's A/B
puts the scan's -14 % on the host.
`bar-lookup` and `point-join` are bimodal at `c1-g` and `c1-s` (spreads 29 % and 38 % at `c1-g`, 33 % and 44 % at `c1-s`) and steady at `c8-g` (2.8 % and 0.6 %); no delta is claimed on the bimodal cells.

Every phase of the driver, median over the three `c1-g` runs. The PostgreSQL column is the morning's run:

| phase | ops | now qps | spread | morning qps | spread | delta | read | PG qps | KDS/PG now | KDS/PG morning |
|---|---|---|---|---|---|---|---|---|---|---|
| load-exchanges | 2 ops | 819.4 qps | 17.2 % | 847.4 qps | 22.3 % | -3.3 % | spread > 20 %, no claim | 700.0 qps | 1.17x | 1.21x |
| load-symbols | 8 ops | 850.0 qps | 4.2 % | 899.1 qps | 58.0 % | -5.5 % | spread > 20 %, no claim | 806.7 qps | 1.05x | 1.11x |
| load-sessions | 7,560 ops | 21,905.7 qps | 2.0 % | 21,542.0 qps | 16.6 % | +1.7 % | inside the spread | 13,292.7 qps | 1.65x | 1.62x |
| load-bars | 60,480 ops | 19,626.3 qps | 1.8 % | 19,575.2 qps | 3.4 % | +0.3 % | inside the spread | 13,336.9 qps | 1.47x | 1.47x |
| load-stats | 60,480 ops | 19,196.6 qps | 1.5 % | 19,921.2 qps | 3.4 % | -3.6 % | **finding** | 12,948.5 qps | 1.48x | 1.54x |
| load-models | 8 ops | 742.4 qps | 23.7 % | 765.9 qps | 12.2 % | -3.1 % | spread > 20 %, no claim | 797.3 qps | 0.93x | 0.96x |
| backtest-read | 360 ops | 464.2 qps | 17.3 % | 531.7 qps | 1.0 % | -12.7 % | inside the spread | 367.8 qps | 1.26x | 1.45x |
| result-insert | 2,872 ops | 749.7 qps | 1.2 % | 742.1 qps | 1.7 % | +1.0 % | inside the spread | 779.8 qps | 0.96x | 0.95x |
| backtest-replay | 360 ops | 491.8 qps | 17.3 % | 575.4 qps | 3.8 % | -14.5 % | inside the spread | 387.8 qps | 1.27x | 1.48x |
| compare-all | 1 ops | 101.0 qps | 1.0 % | 57.8 qps | 6.7 % | +74.7 % | **finding** | 120.0 qps | 0.84x | 0.48x |
| compare-one | 32 ops | 1,026.9 qps | 1.2 % | 603.6 qps | 4.8 % | +70.1 % | **finding** | 1,091.5 qps | 0.94x | 0.55x |
| read-bar-lookup | 200 ops | 11,892.5 qps | 28.3 % | 12,551.5 qps | 22.2 % | -5.3 % | spread > 20 %, no claim | 14,120.8 qps | 0.84x | 0.89x |
| read-bar-range | 200 ops | 1,278.0 qps | 2.2 % | 669.5 qps | 6.8 % | +90.9 % | **finding** | 1,383.6 qps | 0.92x | 0.48x |
| read-symbol-history | 10 ops | 27.1 qps | 3.3 % | 14.2 qps | 5.6 % | +90.8 % | **finding** | 32.7 qps | 0.83x | 0.43x |
| read-day-slice | 200 ops | 507.3 qps | 18.6 % | 589.5 qps | 3.4 % | -13.9 % | inside the spread | 415.1 qps | 1.22x | 1.42x |
| read-join-point | 200 ops | 12,546.8 qps | 31.7 % | 12,339.9 qps | 31.4 % | +1.7 % | spread > 20 %, no claim | 6,924.8 qps | 1.81x | 1.78x |
| read-join-exists | 10 ops | 166.7 qps | 4.6 % | 167.8 qps | 1.7 % | -0.7 % | inside the spread | 123.9 qps | 1.35x | 1.35x |
| agg-global | 10 ops | 165.2 qps | 1.8 % | 166.4 qps | 0.5 % | -0.7 % | inside the spread | 244.4 qps | 0.68x | 0.68x |
| agg-by-symbol | 10 ops | 133.1 qps | 1.7 % | 132.4 qps | 1.6 % | +0.5 % | inside the spread | 145.5 qps | 0.91x | 0.91x |
| agg-by-session | 10 ops | 45.9 qps | 3.1 % | 32.7 qps | 3.7 % | +40.4 % | **finding** | 50.1 qps | 0.92x | 0.65x |
| agg-day-slice | 200 ops | 514.0 qps | 18.6 % | 615.8 qps | 2.0 % | -16.5 % | inside the spread | 413.4 qps | 1.24x | 1.49x |
| agg-distinct | 10 ops | 167.2 qps | 3.7 % | 169.8 qps | 7.4 % | -1.5 % | inside the spread | 244.6 qps | 0.68x | 0.69x |
| sync | 1 ops | 229.1 qps | 74.2 % | 219.8 qps | 30.4 % | +4.2 % | spread > 20 %, no claim | - | - | - |

**Writes: the durability wait is the cost of a row.** Rows per `BEGIN`/`COMMIT`; median of three runs with the spread in brackets (writes return no rows; the delta is the host's, not the client's):

| rows per BEGIN/COMMIT | c1-g now | c8-g now | c1-s now | c1-g morning | c8-g morning | c1-s morning | PG |
|---|---|---|---|---|---|---|---|
| 1 | 768 rows/s (1.9 %) | 751 rows/s (1.8 %) | 764 rows/s (4.3 %) | 744 rows/s (5.8 %) | 730 rows/s (2.8 %) | 755 rows/s (1.1 %) | 739 rows/s |
| 10 | 5,534 rows/s (3.4 %) | 5,325 rows/s (16.3 %) | 5,768 rows/s (0.7 %) | 5,551 rows/s (3.3 %) | 5,396 rows/s (4.9 %) | 5,401 rows/s (9.4 %) | 5,248 rows/s |
| 100 | 15,799 rows/s (1.0 %) | 15,448 rows/s (9.5 %) | 16,044 rows/s (5.3 %) | 15,434 rows/s (6.9 %) | 13,152 rows/s (18.3 %) | 15,933 rows/s (3.5 %) | 12,149 rows/s |
| 1000 | 20,868 rows/s (19.9 %) | 19,074 rows/s (19.9 %) | 20,823 rows/s (8.2 %) | 21,023 rows/s (2.6 %) | 19,758 rows/s (16.0 %) | 21,171 rows/s (11.9 %) | 16,505 rows/s |

**Concurrency.** One join (`cross-join`) issued over 1, 2, 4 and 8 connections:

| connections | c1-g now | c8-g now | c1-s now | c1-g morning | c8-g morning | c1-s morning | PG |
|---|---|---|---|---|---|---|---|
| 1 | 484.8 qps (21.1 %) | 465.7 qps (18.1 %) | 491.6 qps (0.7 %) | 561.3 qps (3.5 %) | 540.6 qps (1.0 %) | 568.9 qps (1.4 %) | 456.4 qps |
| 2 | 524.6 qps (19.4 %) | 886.4 qps (16.0 %) | 519.6 qps (1.5 %) | 631.0 qps (1.4 %) | 1,020.6 qps (1.1 %) | 629.1 qps (4.5 %) | 870.2 qps |
| 4 | 527.4 qps (20.6 %) | 1,195.5 qps (25.8 %) | 521.7 qps (0.9 %) | 634.9 qps (2.3 %) | 1,131.8 qps (41.9 %) | 633.7 qps (6.5 %) | 892.5 qps |
| 8 | 528.0 qps (19.0 %) | 1,515.7 qps (24.9 %) | 522.0 qps (0.6 %) | 633.6 qps (2.0 %) | 1,588.5 qps (22.0 %) | 636.7 qps (0.9 %) | 874.6 qps |

The `cores = 1` cells do not scale with connections (the aggregate is round-trip overlap) and `cores = 8` reaches 3.25x at eight connections
(spreads of 16 % to 26 % at two, four and eight: the sessions are placed on cores as they arrive, BL-S3). The `cross-join` rows are below the morning's by 13 % to 16 % at one connection: the scan effect of section 9, not the concurrency.

## 4. Percentiles

Run 1 of each cell, every phase of the driver (microseconds; the five percentiles are the driver's). The phases that return rows, now against the morning's run 1 and PostgreSQL's run 1, come first:

| phase | run | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| read-bar-range | now | 200 ops | 762.4 µs | 776.4 µs | 781.3 µs | 798.6 µs | 811.7 µs |
| read-bar-range | morning (old client) | 200 ops | 1,424.0 µs | 1,446.8 µs | 1,490.0 µs | 1,535.1 µs | 2,188.0 µs |
| read-bar-range | PostgreSQL 18.6 (morning's run) | 200 ops | 703.4 µs | 716.1 µs | 720.9 µs | 739.5 µs | 750.0 µs |
| read-symbol-history | now | 10 ops | 35,286.7 µs | 35,580.7 µs | 35,903.0 µs | 38,877.2 µs | 38,877.2 µs |
| read-symbol-history | morning (old client) | 10 ops | 68,707.1 µs | 69,294.9 µs | 69,709.7 µs | 72,947.6 µs | 72,947.6 µs |
| read-symbol-history | PostgreSQL 18.6 (morning's run) | 10 ops | 28,871.4 µs | 29,578.2 µs | 30,217.6 µs | 34,674.5 µs | 34,674.5 µs |
| compare-one | now | 32 ops | 940.6 µs | 963.6 µs | 968.0 µs | 1,032.6 µs | 1,034.3 µs |
| compare-one | morning (old client) | 32 ops | 1,610.4 µs | 1,620.7 µs | 1,632.9 µs | 1,777.7 µs | 1,929.0 µs |
| compare-one | PostgreSQL 18.6 (morning's run) | 32 ops | 887.1 µs | 902.3 µs | 911.7 µs | 1,039.8 µs | 1,140.3 µs |
| compare-all | now | 1 ops | 9,839.0 µs | 9,839.0 µs | 9,839.0 µs | 9,839.0 µs | 9,839.0 µs |
| compare-all | morning (old client) | 1 ops | 17,104.1 µs | 17,104.1 µs | 17,104.1 µs | 17,104.1 µs | 17,104.1 µs |
| compare-all | PostgreSQL 18.6 (morning's run) | 1 ops | 8,367.3 µs | 8,367.3 µs | 8,367.3 µs | 8,367.3 µs | 8,367.3 µs |
| agg-by-session | now | 10 ops | 20,592.0 µs | 20,704.0 µs | 20,727.2 µs | 24,170.0 µs | 24,170.0 µs |
| agg-by-session | morning (old client) | 10 ops | 29,904.7 µs | 29,956.7 µs | 30,079.2 µs | 33,071.5 µs | 33,071.5 µs |
| agg-by-session | PostgreSQL 18.6 (morning's run) | 10 ops | 19,407.5 µs | 19,507.5 µs | 19,629.1 µs | 22,410.1 µs | 22,410.1 µs |
| backtest-read | now | 360 ops | 1,763.7 µs | 1,811.0 µs | 1,822.5 µs | 1,923.7 µs | 2,020.5 µs |
| backtest-read | morning (old client) | 360 ops | 1,811.1 µs | 1,851.2 µs | 1,878.0 µs | 2,033.9 µs | 2,544.9 µs |
| backtest-read | PostgreSQL 18.6 (morning's run) | 360 ops | 2,628.0 µs | 2,683.6 µs | 2,705.1 µs | 2,843.3 µs | 2,956.3 µs |

The distributions moved as a whole, not by a tail: `read-bar-range` p0 762 µs against 1,424 µs in the morning and 703 µs for PostgreSQL, p50 781 against 1,490
(PostgreSQL 721) and p99 812 against 2,188 (PostgreSQL 750); `read-symbol-history` p50 35.9 ms against 69.7 ms (PostgreSQL 30.2 ms).
A decoder cost is paid per row on every statement, so the floor (p0) and the body (p25, p50) move with the tail.

#### phases, run 1 of `s1-c1-g` (cores = 1, group)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load-exchanges | 2 ops | 1,135.0 µs | 1,135.0 µs | 1,135.0 µs | 1,265.9 µs | 1,265.9 µs | 1,265.9 µs | 0 errors |
| load-symbols | 8 ops | 1,081.7 µs | 1,097.5 µs | 1,131.5 µs | 1,245.5 µs | 1,245.5 µs | 1,245.5 µs | 0 errors |
| load-sessions | 7,560 ops | 42.5 µs | 43.7 µs | 44.2 µs | 58.0 µs | 68.1 µs | 112.7 µs | 0 errors |
| load-bars | 60,480 ops | 37.2 µs | 45.0 µs | 45.8 µs | 67.3 µs | 83.9 µs | 3,860.3 µs | 0 errors |
| load-stats | 60,480 ops | 38.2 µs | 45.3 µs | 46.2 µs | 67.2 µs | 84.0 µs | 233.3 µs | 0 errors |
| load-models | 8 ops | 1,161.3 µs | 1,167.7 µs | 1,180.5 µs | 1,759.4 µs | 1,759.4 µs | 1,759.4 µs | 0 errors |
| backtest-read | 360 ops | 1,763.7 µs | 1,811.0 µs | 1,822.5 µs | 1,923.7 µs | 2,020.5 µs | 2,571.3 µs | 0 errors |
| result-insert | 2,872 ops | 1,007.3 µs | 1,203.3 µs | 1,254.8 µs | 1,542.9 µs | 3,145.5 µs | 15,134.6 µs | 0 errors |
| backtest-replay | 360 ops | 1,670.6 µs | 1,726.4 µs | 1,741.7 µs | 1,780.5 µs | 1,816.9 µs | 1,857.9 µs | 0 errors |
| compare-all | 1 ops | 9,839.0 µs | 9,839.0 µs | 9,839.0 µs | 9,839.0 µs | 9,839.0 µs | 9,839.0 µs | 0 errors |
| compare-one | 32 ops | 940.6 µs | 963.6 µs | 968.0 µs | 1,032.6 µs | 1,034.3 µs | 1,034.3 µs | 0 errors |
| read-bar-lookup | 200 ops | 73.3 µs | 105.8 µs | 107.9 µs | 131.8 µs | 147.9 µs | 158.4 µs | 0 errors |
| read-bar-range | 200 ops | 762.4 µs | 776.4 µs | 781.3 µs | 798.6 µs | 811.7 µs | 855.5 µs | 0 errors |
| read-symbol-history | 10 ops | 35,286.7 µs | 35,580.7 µs | 35,903.0 µs | 38,877.2 µs | 38,877.2 µs | 38,877.2 µs | 0 errors |
| read-day-slice | 200 ops | 1,629.1 µs | 1,655.7 µs | 1,690.1 µs | 1,712.8 µs | 1,729.2 µs | 2,045.3 µs | 0 errors |
| read-join-point | 200 ops | 108.9 µs | 111.3 µs | 113.0 µs | 133.1 µs | 139.5 µs | 154.0 µs | 0 errors |
| read-join-exists | 10 ops | 5,900.1 µs | 5,914.0 µs | 6,052.2 µs | 6,518.8 µs | 6,518.8 µs | 6,518.8 µs | 0 errors |
| agg-global | 10 ops | 5,983.6 µs | 5,987.1 µs | 5,996.2 µs | 6,553.3 µs | 6,553.3 µs | 6,553.3 µs | 0 errors |
| agg-by-symbol | 10 ops | 7,413.7 µs | 7,429.2 µs | 7,443.9 µs | 7,769.0 µs | 7,769.0 µs | 7,769.0 µs | 0 errors |
| agg-by-session | 10 ops | 20,592.0 µs | 20,704.0 µs | 20,727.2 µs | 24,170.0 µs | 24,170.0 µs | 24,170.0 µs | 0 errors |
| agg-day-slice | 200 ops | 1,622.3 µs | 1,634.9 µs | 1,640.7 µs | 1,659.3 µs | 1,690.7 µs | 2,643.0 µs | 0 errors |
| agg-distinct | 10 ops | 6,007.6 µs | 6,017.4 µs | 6,061.0 µs | 6,754.7 µs | 6,754.7 µs | 6,754.7 µs | 0 errors |
| sync | 1 ops | 4,078.5 µs | 4,078.5 µs | 4,078.5 µs | 4,078.5 µs | 4,078.5 µs | 4,078.5 µs | 0 errors |

#### phases, run 1 of `s1-c8-g` (cores = 8, group)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load-exchanges | 2 ops | 1,252.4 µs | 1,252.4 µs | 1,252.4 µs | 1,334.0 µs | 1,334.0 µs | 1,334.0 µs | 0 errors |
| load-symbols | 8 ops | 1,147.8 µs | 1,171.1 µs | 1,219.7 µs | 1,484.2 µs | 1,484.2 µs | 1,484.2 µs | 0 errors |
| load-sessions | 7,560 ops | 39.3 µs | 43.3 µs | 44.0 µs | 62.9 µs | 71.4 µs | 137.8 µs | 0 errors |
| load-bars | 60,480 ops | 41.1 µs | 45.1 µs | 46.8 µs | 72.8 µs | 87.8 µs | 4,820.3 µs | 0 errors |
| load-stats | 60,480 ops | 39.3 µs | 45.0 µs | 46.1 µs | 69.7 µs | 85.2 µs | 1,813.8 µs | 0 errors |
| load-models | 8 ops | 1,153.2 µs | 1,203.5 µs | 1,229.9 µs | 2,311.4 µs | 2,311.4 µs | 2,311.4 µs | 0 errors |
| backtest-read | 360 ops | 1,862.5 µs | 1,913.5 µs | 1,932.8 µs | 2,021.1 µs | 2,183.0 µs | 2,506.7 µs | 0 errors |
| result-insert | 2,872 ops | 1,010.6 µs | 1,200.0 µs | 1,257.4 µs | 1,551.6 µs | 2,763.3 µs | 18,384.8 µs | 0 errors |
| backtest-replay | 360 ops | 1,761.7 µs | 1,793.5 µs | 1,805.2 µs | 1,852.6 µs | 1,963.3 µs | 3,516.5 µs | 0 errors |
| compare-all | 1 ops | 10,343.1 µs | 10,343.1 µs | 10,343.1 µs | 10,343.1 µs | 10,343.1 µs | 10,343.1 µs | 0 errors |
| compare-one | 32 ops | 972.0 µs | 984.2 µs | 1,005.6 µs | 1,064.6 µs | 1,135.1 µs | 1,135.1 µs | 0 errors |
| read-bar-lookup | 200 ops | 73.6 µs | 75.6 µs | 78.3 µs | 99.3 µs | 107.0 µs | 126.3 µs | 0 errors |
| read-bar-range | 200 ops | 798.4 µs | 821.9 µs | 831.0 µs | 854.4 µs | 885.2 µs | 896.2 µs | 0 errors |
| read-symbol-history | 10 ops | 37,078.0 µs | 37,360.7 µs | 37,617.6 µs | 41,364.1 µs | 41,364.1 µs | 41,364.1 µs | 0 errors |
| read-day-slice | 200 ops | 1,717.4 µs | 1,740.5 µs | 1,749.3 µs | 1,772.8 µs | 1,794.2 µs | 2,390.8 µs | 0 errors |
| read-join-point | 200 ops | 75.0 µs | 77.5 µs | 80.0 µs | 104.3 µs | 112.3 µs | 130.2 µs | 0 errors |
| read-join-exists | 10 ops | 4,913.6 µs | 4,940.8 µs | 4,968.7 µs | 5,969.0 µs | 5,969.0 µs | 5,969.0 µs | 0 errors |
| agg-global | 10 ops | 6,116.4 µs | 6,125.2 µs | 6,129.0 µs | 6,207.8 µs | 6,207.8 µs | 6,207.8 µs | 0 errors |
| agg-by-symbol | 10 ops | 7,533.5 µs | 7,564.6 µs | 7,583.4 µs | 7,713.4 µs | 7,713.4 µs | 7,713.4 µs | 0 errors |
| agg-by-session | 10 ops | 21,375.0 µs | 21,422.0 µs | 21,524.3 µs | 24,419.1 µs | 24,419.1 µs | 24,419.1 µs | 0 errors |
| agg-day-slice | 200 ops | 1,664.5 µs | 1,691.2 µs | 1,700.9 µs | 1,735.9 µs | 1,835.3 µs | 2,855.7 µs | 0 errors |
| agg-distinct | 10 ops | 5,978.4 µs | 5,995.5 µs | 6,015.6 µs | 6,340.2 µs | 6,340.2 µs | 6,340.2 µs | 0 errors |
| sync | 1 ops | 3,140.0 µs | 3,140.0 µs | 3,140.0 µs | 3,140.0 µs | 3,140.0 µs | 3,140.0 µs | 0 errors |

#### phases, run 1 of `s1-c1-s` (cores = 1, strict)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load-exchanges | 2 ops | 1,030.9 µs | 1,030.9 µs | 1,030.9 µs | 1,166.0 µs | 1,166.0 µs | 1,166.0 µs | 0 errors |
| load-symbols | 8 ops | 1,036.9 µs | 1,059.8 µs | 1,089.5 µs | 1,193.2 µs | 1,193.2 µs | 1,193.2 µs | 0 errors |
| load-sessions | 7,560 ops | 41.9 µs | 43.2 µs | 43.6 µs | 52.3 µs | 60.5 µs | 151.4 µs | 0 errors |
| load-bars | 60,480 ops | 36.7 µs | 45.0 µs | 45.7 µs | 67.8 µs | 84.4 µs | 3,787.2 µs | 0 errors |
| load-stats | 60,480 ops | 40.5 µs | 45.4 µs | 46.0 µs | 66.5 µs | 83.7 µs | 992.3 µs | 0 errors |
| load-models | 8 ops | 1,134.3 µs | 1,149.8 µs | 1,181.3 µs | 1,410.2 µs | 1,410.2 µs | 1,410.2 µs | 0 errors |
| backtest-read | 360 ops | 2,090.2 µs | 2,118.6 µs | 2,141.2 µs | 2,232.8 µs | 2,363.5 µs | 2,630.2 µs | 0 errors |
| result-insert | 2,872 ops | 1,000.0 µs | 1,199.2 µs | 1,256.5 µs | 1,470.4 µs | 2,887.6 µs | 10,654.8 µs | 0 errors |
| backtest-replay | 360 ops | 2,002.2 µs | 2,062.9 µs | 2,071.8 µs | 2,117.3 µs | 2,135.3 µs | 2,437.4 µs | 0 errors |
| compare-all | 1 ops | 9,792.6 µs | 9,792.6 µs | 9,792.6 µs | 9,792.6 µs | 9,792.6 µs | 9,792.6 µs | 0 errors |
| compare-one | 32 ops | 970.0 µs | 984.3 µs | 987.7 µs | 1,038.0 µs | 1,055.5 µs | 1,055.5 µs | 0 errors |
| read-bar-lookup | 200 ops | 102.3 µs | 105.3 µs | 106.6 µs | 127.4 µs | 133.5 µs | 143.1 µs | 0 errors |
| read-bar-range | 200 ops | 765.0 µs | 779.3 µs | 784.8 µs | 833.7 µs | 849.9 µs | 974.8 µs | 0 errors |
| read-symbol-history | 10 ops | 36,803.6 µs | 36,922.4 µs | 36,987.2 µs | 39,952.3 µs | 39,952.3 µs | 39,952.3 µs | 0 errors |
| read-day-slice | 200 ops | 2,002.5 µs | 2,010.3 µs | 2,013.7 µs | 2,040.5 µs | 2,083.3 µs | 2,348.3 µs | 0 errors |
| read-join-point | 200 ops | 109.3 µs | 111.6 µs | 113.3 µs | 134.8 µs | 140.0 µs | 154.9 µs | 0 errors |
| read-join-exists | 10 ops | 5,545.0 µs | 6,009.2 µs | 6,019.4 µs | 6,082.2 µs | 6,082.2 µs | 6,082.2 µs | 0 errors |
| agg-global | 10 ops | 5,939.9 µs | 5,943.6 µs | 5,946.6 µs | 6,020.9 µs | 6,020.9 µs | 6,020.9 µs | 0 errors |
| agg-by-symbol | 10 ops | 7,387.4 µs | 7,393.8 µs | 7,408.6 µs | 7,737.9 µs | 7,737.9 µs | 7,737.9 µs | 0 errors |
| agg-by-session | 10 ops | 20,899.9 µs | 20,953.0 µs | 21,090.3 µs | 24,388.5 µs | 24,388.5 µs | 24,388.5 µs | 0 errors |
| agg-day-slice | 200 ops | 1,899.9 µs | 1,914.9 µs | 1,918.5 µs | 1,962.0 µs | 2,101.6 µs | 2,917.5 µs | 0 errors |
| agg-distinct | 10 ops | 5,727.6 µs | 5,740.6 µs | 5,767.4 µs | 6,843.8 µs | 6,843.8 µs | 6,843.8 µs | 0 errors |
| sync | 1 ops | 4,768.6 µs | 4,768.6 µs | 4,768.6 µs | 4,768.6 µs | 4,768.6 µs | 4,768.6 µs | 0 errors |

## 5. Where the time goes

The matrix is serial reads on one connection, so a statement is a fixed cost plus a per-row cost, and a write is a statement plus the durability wait. What the cells measure of each, `c1-g` run 1:

| Component | Now | Morning (old client) | PostgreSQL (morning's run) | How derived |
|---|---|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | **106.4 µs** mean, p0 73.3 µs | 102.2 µs, p0 73.8 µs | 70.9 µs, p0 65.0 µs | `read-bar-lookup`, run 1, 200 ops |
| Per-row cost of a returned row | **3.38 µs per row** (`read-bar-range`) and **4.79 µs per row** (`read-symbol-history`) | 6.96 and 9.25 µs | 3.26 and 4.04 µs | (mean - fixed) / rows returned (200; 7,560), run 1 of each |
| Per-row cost of an examined, unreturned row | about 26 ns | about 27 ns | about 39 ns | `read-day-slice` examines all 60,480 `daily_stats` rows and returns 8: (mean - fixed) / 60,480. Run 1 of each; this is the shape the host drift of section 9 touches |
| Durability / commit | **1,254 µs per commit** | - | 1,292 µs | autocommit insert 1,302 µs per row, a 1,000-row batch 47.9 µs per row; the difference is one durable commit (median of three runs) |
| Lock or conflict wait | none | none | none | one connection (the concurrency sweep reads only) |
| Write statement | 47.9 µs per row at batch 1,000 | - | 60.6 µs | the insert path with the sync amortised away |

The size sweep separates the per-row term from the fixed term: the returned-row cost (mean of the warm sweep minus the cell's own `bar-lookup` mean, over the rows returned), now against the morning's and PostgreSQL's:

| bars | shape | rows returned | fixed round trip (bar-lookup mean) now / PG | KDS now µs/row | KDS morning µs/row | PG µs/row | KDS now / PG | KDS morning / PG |
|---|---|---|---|---|---|---|---|---|
| 252 | bar-range | 200 rows | 74.4 µs / 99.8 µs | 3.54 µs | 6.96 µs | 3.29 µs | 1.07x | 2.12x |
| 252 | symbol-history | 252 rows | 74.4 µs / 99.8 µs | 4.51 µs | 8.90 µs | 3.88 µs | 1.16x | 2.29x |
| 1,008 | bar-range | 200 rows | 75.9 µs / 87.2 µs | 3.54 µs | 7.14 µs | 3.47 µs | 1.02x | 2.06x |
| 1,008 | symbol-history | 252 rows | 75.9 µs / 87.2 µs | 4.57 µs | 9.14 µs | 4.16 µs | 1.10x | 2.20x |
| 10,080 | bar-range | 200 rows | 77.0 µs / 70.4 µs | 3.61 µs | 6.91 µs | 3.34 µs | 1.08x | 2.07x |
| 10,080 | symbol-history | 1,260 rows | 77.0 µs / 70.4 µs | 4.77 µs | 9.29 µs | 4.02 µs | 1.19x | 2.31x |
| 60,480 | bar-range | 200 rows | 79.1 µs / 68.0 µs | 3.57 µs | 6.86 µs | 3.23 µs | 1.11x | 2.12x |
| 60,480 | symbol-history | 7,560 rows | 79.1 µs / 68.0 µs | 4.84 µs | 9.54 µs | 4.08 µs | 1.18x | 2.34x |

**The per-row cost of a returned row is flat across a 240x range of relation size and across two row counts, and it is now within 2 % to 19 % of PostgreSQL's.** KDS spends 3.54 to 3.61 µs per row on a 200-row range at every size
(PostgreSQL 3.23 to 3.47 µs) and 4.51 to 4.84 µs on a symbol history (PostgreSQL 3.88 to 4.16 µs); the morning's figures were 6.9 to 7.1 and 8.9 to 9.5 µs. The cost lived in the Python decoder
(`tools/kwp.py`), not in the engine and not in the relation: the same binary, a different client, a ratio of two. Section 10 puts a number on what is left.
The fixed round trip is the one place PostgreSQL is cheaper per statement at the larger sizes (KDS 79 µs against 68 µs at 60,480 bars); at 252 and 1,008 bars KDS's is the cheaper (74 against 100 µs, 76 against 87 µs),
which makes the pk reads 1.34x and 1.15x of PostgreSQL's there and 0.86x at 60,480 bars (the cell is bimodal; section 3).

## 6. Joins and the plan the engine chose

The engine chooses an inner-join block's order since CB-S2 and prices it from the join planner's statistics since CC. The client has no part in a plan; this section repeats the morning's probe on the same binary
(`pl-s1-planned-r1..3`, `pl-s1-written-r1..3`), three runs each of the default and of `join_order = written`, same window:

| measure | planned | spread | written | spread | planned over written |
|---|---|---|---|---|---|
| phase backtest-read | 462.4 qps | 1.0 % | 467.6 qps | 1.1 % | 0.99x |
| phase backtest-replay | 489.8 qps | 0.9 % | 496.2 qps | 1.7 % | 0.99x |
| phase compare-all | 98.1 qps | 6.4 % | 102.2 qps | 1.7 % | 0.96x |
| phase compare-one | 1,025.5 qps | 1.9 % | 962.2 qps | 1.8 % | 1.07x |
| sweep cross-join cold | 481.6 qps | 2.5 % | 493.6 qps | 0.4 % | 0.98x |
| sweep cross-join warm | 492.8 qps | 2.1 % | 496.5 qps | 0.7 % | 0.99x |
| sweep cross-join cabin | 6,640.3 qps | 1.2 % | 6,963.1 qps | 1.8 % | 0.95x |
| sweep point-join cold | 12,171.5 qps | 33.8 % | 12,488.6 qps | 0.6 % | 0.97x |
| sweep point-join warm | 12,426.8 qps | 32.3 % | 12,599.3 qps | 2.3 % | 0.99x |
| sweep model-join cold | 1,011.8 qps | 6.6 % | 940.4 qps | 1.3 % | 1.08x |
| sweep model-join warm | 1,038.7 qps | 4.0 % | 967.8 qps | 2.0 % | 1.07x |
| sweep model-join cabin | 1,044.3 qps | 4.1 % | 988.0 qps | 2.8 % | 1.06x |


The planned and written arms are compared with each other in one window, so the host drift of section 9 cancels. The plans themselves (the ANALYZE replies under `plans/` in the archive) are those of the morning:
`compare-all` and `compare-one` are reordered (`reordered=1`), `backtest-read` keeps its written order, and `compare-all` carries the statement-local build.
**The order choice buys the same 6 % to 7 % on the two reordered joins as in the morning, and a shorter statement does not enlarge it.** `compare-one` is 1.07x planned over written (spreads 1.9 % and 1.8 %, so a finding by the rule) and the
`model-join` sweep 1.06x to 1.08x (spreads 1.3 % to 6.6 %); `compare-all` is 0.96x (spread 6.4 %), inside its spread. The morning measured 1.06x and 1.02x on the same rows with the old decoder. The statement that was 1.64 ms is 0.98 ms now, so the planner's
saving (3,231 rows examined against 2,873) is 7 % of a statement of either length: its payoff scales with the walk, which the client change did not touch. The `point-join` rows (0.97x and 0.99x) are the bimodal row of section 3 (spread 32 % to 34 % in the
planned arm, 0.6 % to 2.3 % in the written) and carry no claim.

## 7. The size sweep

`c1-g`, 252 / 1,008 / 10,080 bars (60,480 is section 3), three runs each, now against the morning and against PostgreSQL's (morning) warm qps; the last two columns are the ratio of KDS to PostgreSQL, now and in the morning:

| bars | shape | warm now | spread | warm morning | delta | read | PG warm | KDS/PG now | KDS/PG morning |
|---|---|---|---|---|---|---|---|---|---|
| 252 | bar-lookup | 13,408.1 qps | 5.6 % | 13,184.7 qps | +1.7 % | inside the spread | 9,989.7 qps | 1.34x | 1.32x |
| 252 | bar-range | 1,279.3 qps | 1.7 % | 680.9 qps | +87.9 % | **finding** | 1,318.8 qps | 0.97x | 0.52x |
| 252 | day-slice | 11,105.7 qps | 4.5 % | 10,915.6 qps | +1.7 % | inside the spread | 13,721.8 qps | 0.81x | 0.80x |
| 252 | symbol-history | 826.3 qps | 1.0 % | 431.0 qps | +91.7 % | **finding** | 927.4 qps | 0.89x | 0.46x |
| 252 | cross-join | 8,264.1 qps | 14.7 % | 8,601.1 qps | -3.9 % | spread > 20 %, no claim | 5,666.7 qps | 1.46x | 1.52x |
| 252 | point-join | 12,568.8 qps | 14.0 % | 12,630.9 qps | -0.5 % | inside the spread | 6,353.6 qps | 1.98x | 1.99x |
| 252 | model-join | 8,485.1 qps | 23.9 % | 7,288.3 qps | +16.4 % | spread > 20 %, no claim | 6,596.8 qps | 1.29x | 1.10x |
| 1,008 | bar-lookup | 13,149.9 qps | 31.3 % | 12,862.6 qps | +2.2 % | spread > 20 %, no claim | 11,445.5 qps | 1.15x | 1.12x |
| 1,008 | bar-range | 1,276.6 qps | 2.1 % | 664.4 qps | +92.1 % | **finding** | 1,278.9 qps | 1.00x | 0.52x |
| 1,008 | day-slice | 7,661.0 qps | 0.5 % | 7,140.7 qps | +7.3 % | **finding** | 7,652.4 qps | 1.00x | 0.93x |
| 1,008 | symbol-history | 814.8 qps | 1.4 % | 420.1 qps | +94.0 % | **finding** | 879.8 qps | 0.93x | 0.48x |
| 1,008 | cross-join | 6,330.0 qps | 0.3 % | 6,004.4 qps | +5.4 % | **finding** | 3,398.9 qps | 1.86x | 1.77x |
| 1,008 | point-join | 12,521.3 qps | 2.9 % | 12,918.0 qps | -3.1 % | **finding** | 7,119.6 qps | 1.76x | 1.81x |
| 1,008 | model-join | 8,688.0 qps | 5.8 % | 7,323.9 qps | +18.6 % | **finding** | 8,067.8 qps | 1.08x | 0.91x |
| 10,080 | bar-lookup | 12,962.2 qps | 26.2 % | 12,970.4 qps | -0.1 % | spread > 20 %, no claim | 14,170.7 qps | 0.91x | 0.92x |
| 10,080 | bar-range | 1,252.6 qps | 3.4 % | 685.8 qps | +82.6 % | **finding** | 1,355.2 qps | 0.92x | 0.51x |
| 10,080 | day-slice | 2,285.3 qps | 7.4 % | 2,432.1 qps | -6.0 % | inside the spread | 2,058.3 qps | 1.11x | 1.18x |
| 10,080 | symbol-history | 164.4 qps | 2.4 % | 84.9 qps | +93.6 % | **finding** | 194.7 qps | 0.84x | 0.44x |
| 10,080 | cross-join | 2,127.3 qps | 0.6 % | 2,252.0 qps | -5.5 % | **finding** | 1,681.7 qps | 1.26x | 1.34x |
| 10,080 | point-join | 12,855.2 qps | 1.4 % | 12,657.7 qps | +1.6 % | inside the spread | 7,010.1 qps | 1.83x | 1.81x |
| 10,080 | model-join | 4,187.3 qps | 1.1 % | 2,900.2 qps | +44.4 % | **finding** | 4,189.7 qps | 1.00x | 0.69x |
| 60,480 | bar-lookup | 12,619.9 qps | 29.0 % | 11,263.5 qps | +12.0 % | spread > 20 %, no claim | 14,673.3 qps | 0.86x | 0.77x |
| 60,480 | bar-range | 1,260.3 qps | 4.3 % | 684.4 qps | +84.1 % | **finding** | 1,399.8 qps | 0.90x | 0.49x |
| 60,480 | day-slice | 508.7 qps | 18.6 % | 591.3 qps | -14.0 % | inside the spread | 413.5 qps | 1.23x | 1.43x |
| 60,480 | symbol-history | 27.3 qps | 0.7 % | 13.8 qps | +97.8 % | **finding** | 32.3 qps | 0.85x | 0.43x |
| 60,480 | cross-join | 496.9 qps | 20.6 % | 575.8 qps | -13.7 % | spread > 20 %, no claim | 393.1 qps | 1.26x | 1.46x |
| 60,480 | point-join | 10,589.7 qps | 37.8 % | 12,674.6 qps | -16.4 % | spread > 20 %, no claim | 7,071.3 qps | 1.50x | 1.79x |
| 60,480 | model-join | 1,030.2 qps | 3.4 % | 614.8 qps | +67.6 % | **finding** | 1,092.2 qps | 0.94x | 0.56x |

**Fixed and per-row costs separate cleanly at every size.** The pk shapes are flat (`bar-lookup` 12,962 to 13,408 qps across 252, 1,008 and 10,080 bars; `point-join` 12,521 to 12,855 qps at all but the bimodal 60,480-bar cell).
The scan shapes scale with rows (`day-slice` 11,106 -> 7,661 -> 2,285 -> 509 qps). The row-returning shapes are flat in relation size (`bar-range` 1,253 to 1,279 qps at every size) and
scale with the rows returned (`symbol-history` 826 -> 815 -> 164 -> 27 qps as the symbol's sessions go 252 -> 252 -> 1,260 -> 7,560). At every size the morning-to-now ratio of a many-row shape is
1.8x to 2.0x (`bar-range` 1.83x to 1.92x; `symbol-history` 1.92x to 1.98x), so the decoder cost is per row and independent of the relation.

## 8. Against PostgreSQL 18.6 (the morning's run, reused)

PostgreSQL is the floor: it says whether the engine is in the right league and nothing finer. Its twin was **not re-run**; its cells are the morning's `pg-s1*` runs (three runs each; 04:25 to 04:33 UTC). Ratios are KDS `c1-g`
(median of three, this run) over PostgreSQL (median of three, the morning's), above 1.00x = KDS faster; the morning's KDS ratio is beside it:

| shape | KDS warm now | KDS warm morning | PG warm (morning's run) | KDS/PG now | KDS/PG morning | KDS cabin now | PG index | KDS cabin / PG index |
|---|---|---|---|---|---|---|---|---|
| bar-lookup | 12,619.9 qps | 11,263.5 qps | 14,673.3 qps | 0.86x | 0.77x | - | - | - |
| bar-range | 1,260.3 qps | 684.4 qps | 1,399.8 qps | 0.90x | 0.49x | - | - | - |
| day-slice | 508.7 qps | 591.3 qps | 413.5 qps | 1.23x | 1.43x | 8,109.3 qps | 8,722.0 qps | 0.93x |
| symbol-history | 27.3 qps | 13.8 qps | 32.3 qps | 0.85x | 0.43x | 26.2 qps | 33.8 qps | 0.78x |
| cross-join | 496.9 qps | 575.8 qps | 393.1 qps | 1.26x | 1.46x | 6,722.3 qps | 4,594.5 qps | 1.46x |
| point-join | 10,589.7 qps | 12,674.6 qps | 7,071.3 qps | 1.50x | 1.79x | - | - | - |
| model-join | 1,030.2 qps | 614.8 qps | 1,092.2 qps | 0.94x | 0.56x | 1,041.0 qps | 1,124.4 qps | 0.93x |

| rows per BEGIN/COMMIT | KDS c1-g (now) | PG18 (morning's run) | KDS/PG |
|---|---|---|---|
| 1 | 768 rows/s | 739 rows/s | 1.04x |
| 10 | 5,534 rows/s | 5,248 rows/s | 1.05x |
| 100 | 15,799 rows/s | 12,149 rows/s | 1.30x |
| 1000 | 20,868 rows/s | 16,505 rows/s | 1.26x |

**The league reading.** The shapes that returned many rows were at half of PostgreSQL's speed and are at 0.83x to 0.94x of it (`bar-range` 0.90x, `symbol-history` 0.85x,
`model-join` 0.94x, `compare-all` 0.84x, `compare-one` 0.94x, `agg-by-session` 0.92x). KDS is still ahead on the pk-anchored join (1.76x to 1.98x at 252 to 10,080 bars; the 1.50x at 60,480 bars is the bimodal cell of section 3 and carries no claim), the unindexed equality scan
(`day-slice` 1.23x at 60,480 bars, a figure the host drift lowers; 1.00x to 1.11x at 1,008 and 10,080 bars) and the three big load phases (`load-sessions`, `load-bars`, `load-stats`: 1.47x to 1.65x), and behind on whole-relation folds (`agg-global` 0.68x,
`agg-distinct` 0.68x), which return one row and are not the decoder's. Row cost is no longer the thing that sets KDS apart from PostgreSQL on this scenario; the folds and `bar-lookup` are.

## 9. The client, isolated: old against new, interleaved

Three pairs, old, new, old, new, old, new, on the same host, one `s1-c1-g` cell each (a fresh server and data file per cell, the same binary copy; the old client is `a40882af^`'s `tools/kwp.py` and
`tools/ckdbs_cli.py` from `oldtools/`, imported first through `PYTHONPATH`). Spread = (max - min) / median over the three; the rule of the summary applies.

| shape (warm qps) | old client: pair 1 / 2 / 3 | old median | old spread | new client: pair 1 / 2 / 3 | new median | new spread | new over old | read |
|---|---|---|---|---|---|---|---|---|
| bar-lookup | 12,635 / 12,546 / 12,337 qps | 12,546 qps | 2.4 % | 12,741 / 13,382 / 13,251 qps | 13,251 qps | 4.8 % | 1.056x | **finding** |
| bar-range | 685 / 684 / 691 qps | 685 qps | 0.9 % | 1,239 / 1,267 / 1,254 qps | 1,254 qps | 2.2 % | 1.831x | **finding** |
| day-slice | 491 / 493 / 501 qps | 493 qps | 2.0 % | 502 / 508 / 504 qps | 504 qps | 1.0 % | 1.023x | **finding** |
| symbol-history | 14.2 / 14.0 / 14.2 qps | 14.2 qps | 1.4 % | 27.3 / 27.4 / 27.3 qps | 27.3 qps | 0.4 % | 1.923x | **finding** |
| cross-join | 484 / 490 / 480 qps | 484 qps | 2.0 % | 493 / 498 / 493 qps | 493 qps | 1.0 % | 1.019x | inside the spread |
| point-join | 12,581 / 12,544 / 8,751 qps | 12,544 qps | 30.5 % | 12,155 / 12,802 / 12,264 qps | 12,264 qps | 5.3 % | 0.978x | no claim (spread > 20 %) |
| model-join | 598 / 603 / 594 qps | 598 qps | 1.5 % | 1,033 / 1,048 / 1,052 qps | 1,048 qps | 1.9 % | 1.753x | **finding** |
| day-slice (cabin) | 6,502 / 6,498 / 6,490 qps | 6,498 qps | 0.2 % | 8,219 / 8,282 / 8,157 qps | 8,219 qps | 1.5 % | 1.265x | **finding** |
| symbol-history (cabin) | 13.9 / 13.7 / 13.9 qps | 13.9 qps | 1.4 % | 26.1 / 26.4 / 26.1 qps | 26.1 qps | 1.1 % | 1.878x | **finding** |
| cross-join (cabin) | 5,521 / 5,564 / 4,565 qps | 5,521 qps | 18.1 % | 6,632 / 6,885 / 6,675 qps | 6,675 qps | 3.8 % | 1.209x | **finding** |
| model-join (cabin) | 605 / 596 / 598 qps | 598 qps | 1.4 % | 1,076 / 1,072 / 1,077 qps | 1,076 qps | 0.5 % | 1.800x | **finding** |

And every phase of the driver:

| phase (qps) | old client: pair 1 / 2 / 3 | old median | old spread | new client: pair 1 / 2 / 3 | new median | new spread | new over old | read |
|---|---|---|---|---|---|---|---|---|
| load-exchanges | 865 / 879 / 804 qps | 865 qps | 8.7 % | 816 / 873 / 845 qps | 845 qps | 6.8 % | 0.977x | inside the spread |
| load-symbols | 928 / 835 / 845 qps | 845 qps | 11.0 % | 832 / 906 / 905 qps | 905 qps | 8.1 % | 1.071x | inside the spread |
| load-sessions | 21,692 / 21,799 / 21,946 qps | 21,799 qps | 1.2 % | 19,138 / 22,312 / 21,919 qps | 21,919 qps | 14.5 % | 1.005x | inside the spread |
| load-bars | 20,364 / 18,335 / 19,720 qps | 19,720 qps | 10.3 % | 17,849 / 20,422 / 18,377 qps | 18,377 qps | 14.0 % | 0.932x | inside the spread |
| load-stats | 20,662 / 19,142 / 19,705 qps | 19,705 qps | 7.7 % | 18,266 / 19,516 / 18,724 qps | 18,724 qps | 6.7 % | 0.950x | inside the spread |
| load-models | 334 / 729 / 786 qps | 729 qps | 61.9 % | 735 / 694 / 826 qps | 735 qps | 18.0 % | 1.008x | no claim (spread > 20 %) |
| backtest-read | 454 / 454 / 461 qps | 454 qps | 1.5 % | 458 / 465 / 464 qps | 464 qps | 1.6 % | 1.021x | **finding** |
| result-insert | 776 / 726 / 778 qps | 776 qps | 6.6 % | 728 / 783 / 770 qps | 770 qps | 7.1 % | 0.992x | inside the spread |
| backtest-replay | 482 / 480 / 486 qps | 482 qps | 1.2 % | 491 / 483 / 492 qps | 491 qps | 1.9 % | 1.017x | inside the spread |
| compare-all | 57.8 / 56.8 / 57.8 qps | 57.8 qps | 1.7 % | 103.5 / 101.9 / 103.5 qps | 103.5 qps | 1.5 % | 1.791x | **finding** |
| compare-one | 602 / 599 / 609 qps | 602 qps | 1.7 % | 1,036 / 1,028 / 1,024 qps | 1,028 qps | 1.1 % | 1.707x | **finding** |
| read-bar-lookup | 12,654 / 12,681 / 12,567 qps | 12,654 qps | 0.9 % | 10,198 / 12,938 / 12,816 qps | 12,816 qps | 21.4 % | 1.013x | no claim (spread > 20 %) |
| read-bar-range | 686 / 685 / 673 qps | 685 qps | 1.9 % | 1,226 / 1,255 / 1,272 qps | 1,255 qps | 3.7 % | 1.832x | **finding** |
| read-symbol-history | 14.1 / 14.1 / 14.1 qps | 14.1 qps | 0.0 % | 26.8 / 27.2 / 27.0 qps | 27.0 qps | 1.5 % | 1.915x | **finding** |
| read-day-slice | 491 / 495 / 497 qps | 495 qps | 1.3 % | 505 / 505 / 507 qps | 505 qps | 0.3 % | 1.021x | **finding** |
| read-join-point | 12,265 / 12,750 / 12,508 qps | 12,508 qps | 3.9 % | 12,556 / 12,978 / 12,636 qps | 12,636 qps | 3.3 % | 1.010x | inside the spread |
| read-join-exists | 166 / 169 / 172 qps | 169 qps | 3.0 % | 171 / 171 / 171 qps | 171 qps | 0.2 % | 1.008x | inside the spread |
| agg-global | 164 / 165 / 168 qps | 165 qps | 2.5 % | 168 / 169 / 169 qps | 169 qps | 0.5 % | 1.023x | inside the spread |
| agg-by-symbol | 130 / 130 / 130 qps | 130 qps | 0.2 % | 133 / 132 / 132 qps | 132 qps | 0.7 % | 1.018x | **finding** |
| agg-by-session | 32.2 / 32.2 / 32.1 qps | 32.2 qps | 0.3 % | 46.4 / 46.7 / 46.5 qps | 46.5 qps | 0.6 % | 1.444x | **finding** |
| agg-day-slice | 506 / 508 / 506 qps | 506 qps | 0.4 % | 515 / 506 / 518 qps | 515 qps | 2.3 % | 1.017x | inside the spread |
| agg-distinct | 168 / 168 / 168 qps | 168 qps | 0.1 % | 170 / 170 / 172 qps | 170 qps | 1.0 % | 1.012x | **finding** |
| sync | 123 / 366 / 183 qps | 183 qps | 132.7 % | 196 / 208 / 172 qps | 196 qps | 18.4 % | 1.070x | no claim (spread > 20 %) |

**The decoder explains the whole movement of the row-returning shapes and none of the movement of the others.** In one window, with the engine identical, `bar-range` 1.83x,
`symbol-history` 1.92x, `model-join` 1.75x, `compare-all` 1.79x, `compare-one` 1.71x, `agg-by-session`
1.44x (7,560 groups returned), and the Cabin-served 8-row statements 1.2x to 1.3x; everything that returns one row or a handful sits between 0.97x and 1.06x
(`day-slice` 1.023x, `cross-join` 1.019x, `bar-lookup` 1.056x, the folds 1.01x to 1.02x). The loads, the writes and `sync` are
inside their spreads, which fits a client change that touches only row batches.

**The -14 % on the scan shapes is the host.** The old client, the morning's client, reads `day-slice` at 493 qps in this window and the morning's own cells read 591 qps: -17 %
with nothing changed but the time of day. `cross-join` the same (484 qps old now against 576 qps then). Scenario 4's `off` walk, on a 10,000-row varchar equality, did not drift (its server CPU is
2.20 s here and 2.22 s in the morning), and scenario 3's integer-column scans did (the old client reads them 11 % to 12 % below the morning's cells, the same sign), so the effect belongs to the scans of the large integer relations. This run does not say whether it is
frequency, a neighbouring tenant or where the server thread landed; it says the interleaved pairs are the only reading of the client's effect that the drift does not touch.

## 10. What moved, why, and what is left of the gap to PostgreSQL

1. **The decoder was the per-row gap.** Same binary, same data, same statements: the only difference between the arms of section 9 is the client's row-batch decode and the renderer lookup, and a returned row costs 3.5 to 4.8 µs
   instead of 6.9 to 9.5 µs (section 5's size sweep). This answers the question the morning's file left open ("whether the factor of two lives in the engine or in the two Python wire decoders is not separated"): it lived in the decoder, and
   `bench/v3.3.0/results-row-cost-attribution-v3.3.0-37-gf829964c.md`'s verdict (84 % of the per-row wall gap is the decoder) is borne out in end-to-end scenario numbers.
2. **What remains is 0.85x to 0.94x of PostgreSQL on the many-row shapes**, a per-row difference of 0.3 to 0.8 µs (section 5). The row-cost attribution
   (`bench/v3.3.0/results-row-cost-attribution-v3.3.0-37-gf829964c.md`, whose corrected verdict is 84 % decoder and 16 % engine-side) found the engine-side 0.42 µs per row is not CPU but the *absence of overlap*: KDS builds the whole reply and sends it
   at statement completion (`WireResultSink::Emit`, `KwpSession::Deliver`, BN-R7), so the server's 0.43 µs of CPU per row is added to the client's time instead of overlapped with it, as PostgreSQL's streaming backend allows.
   **Where the rest of the gap sits (a KDS-only re-run of the attribution's cells, `rowcost/` in the run archive; PostgreSQL is the attribution file's, not re-measured).** On the 12-column read the new client's wall slope is 3.21 µs per row against 6.31 µs for the old client in the same window (the attribution measured 6.19), and against PostgreSQL's 3.57 µs: a like-for-like decode to Python ints is no longer where KDS is behind. The scenario drivers do not stop at ints: `ServerConnection.send_command` renders every cell to text and joins it, and that path costs 4.70 µs per row on the new client (9.25 on the old), close to the scenario's own figure (`symbol-history` 4.84 µs per row at 7,560 rows). It breaks into the client's decode (2.20 µs of CPU), the render and join (2.05 µs of CPU) and 0.43 µs of client idle, the server's 0.42 µs of CPU per row that is not overlapped because the reply leaves at statement completion. PostgreSQL's matching scenario figure is 4.08 µs per row. The two KDS figures are not one statement: `symbol-history` is a FilterScan that walks all 60,480 rows to return 7,560, 8 examined rows per returned row at about 26 ns each (section 5), so about 0.21 µs per returned row of walk that the key-range cells do not have and that also finishes before the first byte leaves. Of the 0.76 µs per row that remains between KDS and PostgreSQL on `symbol-history` (4.84 against 4.08), **the engine-side non-overlap is at least 0.43 µs (56 %) and up to about 0.64 µs (84 %) with the walk counted; the remaining 0.12 to 0.33 µs is not attributed** (it would hold the render step's excess over PostgreSQL's client path, which was not measured). The split is derived by subtraction across two statements and two windows (this window's key-range idle, PostgreSQL's attribution figures and its scenario cell), not measured. For `bar-range` (nine columns, a key range, 3.57 against 3.23 µs per row, a gap of 0.34 µs) the engine-side term was not measured at nine columns. Before the fix the gap on that symbol history was 5.46 µs per row (9.54 against 4.08); the fix removed 4.70 µs per row of it on the scenario path (86 %), and the non-overlap was and is part of the 0.76 µs that remains.

3. **No limit was found for the many-row shapes**; none was sought: they are serial single-connection reads, bounded by the client's per-row work and now by the engine's non-overlapped reply, not by a resource.
4. **Insights about the engine.** The engine's own cost of a returned row on a key range is 0.42 µs of server CPU (new client and old, this window), flat; everything else a scenario driver pays per row is Python. The decoder fix halves the
   driver's per-row cost and leaves the engine's term exactly where it was (0.42 µs of wall that is not overlapped), which is why that term is now 9 % of a returned row (0.42 of 4.70 µs) instead of 4.5 % of the old client's 9.25 µs.
   Where a result bears on a stated design expectation: `docs/spec/protocol.md`'s 64 KiB batch target and the single-write reply (BN-R7) are chosen for syscall count; this scenario puts the price on them, a reply that cannot overlap
   server and client, at 0.42 µs per row of a 12-column read, and that price is the largest term the engine still owns on a many-row read. The join planner's payoff, which a 6 µs-per-row decoder hid, is re-read in section 6.

#### Row-cost attribution, KDS only (`rowcost/` in the run archive)

The attribution's own cells (`SELECT * FROM daily_stats WHERE id BETWEEN 1 AND N`, N = 1 to 60,480, 12 columns, three repeats per point, windows of about 3 s; the server pinned to CPU 2 and the client to CPU 6) on this run's binary copy,
with the old and the new client in turn on fresh servers (`values`: cells become Python ints; `send`: the scenario drivers' path). PostgreSQL's column is the attribution file's (06:19 to 06:26 UTC), **not re-measured**, so every "new / PG" is a cross-window ratio of 2 to 3 hours.

| component (µs per returned row, 12-column `daily_stats`, slope over 1 to 10,000 rows) | KDS, old client (this window) | KDS, new client | PostgreSQL 18.6 (attribution file, not re-measured) | new / old | new / PG |
|---|---|---|---|---|---|
| Wall slope, `values` (cells as Python ints) | 6.31 µs | 3.21 µs | 3.57 µs | 0.51x | 0.90x |
| Server CPU (`drain`, fixed 3 s windows) | 0.42 µs | 0.42 µs | 0.57 µs | 1.00x | 0.74x |
| Client CPU, total (`values`) | 5.89 µs | 2.79 µs | 3.55 µs | 0.47x | 0.79x |
| - receive (`drain` client CPU) | 0.02 µs | 0.02 µs | 0.27 µs | 1.00x | 0.07x |
| - split cells into `bytes` (`split` minus `drain`) | 5.28 µs | 2.20 µs | 2.59 µs | 0.42x | 0.85x |
| - convert to int (`values` minus `split`) | 0.59 µs | 0.57 µs | 0.69 µs | 0.96x | 0.82x |
| Client idle = wall - client CPU (`values`) | 0.43 µs | 0.42 µs | 0.01 µs | 0.98x | - |
| Wall slope, `drain` (decode removed) | 0.42 µs | 0.42 µs | 0.57 µs | 1.00x | 0.74x |
| **Wall slope, `send` (the scenario drivers' path: decode, render every cell to text, join)** | 9.25 µs | 4.70 µs | - | 0.51x | - |
| - client CPU, `send` | 8.80 µs | 4.27 µs | - | 0.49x | - |
| - of which render and join (`send` client CPU minus `split` client CPU) | 3.51 µs | 2.05 µs | - | 0.59x | - |
| - client idle, `send` (wall - client CPU) | 0.45 µs | 0.43 µs | - | 0.96x | - |

Per statement, median of three repeats (spread in brackets):

| rows returned | mode | KDS old client: wall / stmt (spread) | KDS new client: wall / stmt (spread) | new / old | KDS new client CPU / stmt | server CPU / stmt (new) |
|---|---|---|---|---|---|---|
| 1 row | values | 80.1 µs (0 %) | 76.8 µs (2 %) | 0.96x | 44.1 µs | 32.2 µs |
| 1 row | send | 83.5 µs (1 %) | 83.8 µs (0 %) | 1.00x | 51.9 µs | 31.2 µs |
| 10 rows | values | 137.1 µs (1 %) | 105.1 µs (1 %) | 0.77x | 68.0 µs | 36.7 µs |
| 10 rows | send | 169.7 µs (1 %) | 127.0 µs (0 %) | 0.75x | 90.6 µs | 36.2 µs |
| 100 rows | values | 693.2 µs (1 %) | 388.4 µs (1 %) | 0.56x | 308.7 µs | 79.5 µs |
| 100 rows | send | 985.8 µs (2 %) | 540.9 µs (1 %) | 0.55x | 461.8 µs | 79.5 µs |
| 1,000 rows | values | 6,227.9 µs (1 %) | 3,214.5 µs (0 %) | 0.52x | 2,765.4 µs | 468.8 µs |
| 1,000 rows | send | 9,107.8 µs (0 %) | 4,683.6 µs (0 %) | 0.51x | 4,242.2 µs | 479.7 µs |
| 10,000 rows | values | 63,182.6 µs (1 %) | 32,150.1 µs (1 %) | 0.51x | 27,926.6 µs | 4,562.3 µs |
| 10,000 rows | send | 92,494.6 µs (2 %) | 47,083.8 µs (1 %) | 0.51x | 42,766.9 µs | 4,730.6 µs |
| 60,480 rows | values | 407,250.2 µs (0 %) | 222,400.4 µs (0 %) | 0.55x | 195,466.5 µs | 29,494.9 µs |
| 60,480 rows | send | 590,145.6 µs (0 %) | 318,365.9 µs (0 %) | 0.54x | 291,411.1 µs | 30,101.0 µs |

Latency distribution of one statement, pooled over the three repeats (n = statements):

| mode | rows | client | n statements | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| values | 1 | old | 197,802 | 73 µs | 77 µs | 78 µs | 92 µs | 102 µs |
| values | 1 | new | 197,802 | 71 µs | 73 µs | 74 µs | 89 µs | 102 µs |
| values | 100 | old | 94,734 | 659 µs | 679 µs | 686 µs | 715 µs | 833 µs |
| values | 100 | new | 94,734 | 366 µs | 379 µs | 384 µs | 405 µs | 457 µs |
| values | 1,000 | old | 16,512 | 6,040 µs | 6,133 µs | 6,196 µs | 6,370 µs | 7,401 µs |
| values | 1,000 | new | 16,512 | 3,087 µs | 3,166 µs | 3,189 µs | 3,277 µs | 4,558 µs |
| values | 10,000 | old | 1,782 | 60,704 µs | 62,501 µs | 63,036 µs | 66,025 µs | 67,982 µs |
| values | 10,000 | new | 1,782 | 30,520 µs | 31,730 µs | 32,072 µs | 33,899 µs | 34,721 µs |
| send | 1 | old | 197,802 | 78 µs | 81 µs | 82 µs | 96 µs | 105 µs |
| send | 1 | new | 197,802 | 74 µs | 80 µs | 81 µs | 97 µs | 109 µs |
| send | 100 | old | 94,734 | 946 µs | 974 µs | 984 µs | 1,016 µs | 1,062 µs |
| send | 100 | new | 94,734 | 517 µs | 532 µs | 537 µs | 561 µs | 594 µs |
| send | 1,000 | old | 16,512 | 8,848 µs | 9,009 µs | 9,071 µs | 9,306 µs | 10,223 µs |
| send | 1,000 | new | 16,512 | 4,534 µs | 4,623 µs | 4,666 µs | 4,764 µs | 5,485 µs |
| send | 10,000 | old | 1,782 | 88,956 µs | 91,308 µs | 92,284 µs | 95,134 µs | 101,962 µs |
| send | 10,000 | new | 1,782 | 44,999 µs | 46,542 µs | 47,032 µs | 48,951 µs | 49,798 µs |

The wall time per statement is halved at every size from 100 rows up (0.51x to 0.56x), and a one-row statement is unchanged (`send`: 83.5 against 83.8 µs). The distributions move as a whole (p0 to p99), as in the scenario cells.

Three cells of these sweeps overlapped a sample with a foreign process at 50 % CPU or more and were re-run (`rowcost/contaminated/` in the archive; the summary's Contamination lists them); the tables above are the clean sweeps.

## 11. Noise floor, contamination and host

The noise floor is the 3-run spread of one cell: 4.3 % for `bar-range`, 0.7 % for `symbol-history`, 3.4 % for `model-join`, 18.6 % for `day-slice`
(one slow run in three) and 29 % for `bar-lookup`, which is bimodal as in the morning: a fast mode of 12,300 to 13,400 qps and a slow one of 9,100 to 9,200 qps (2 of the 9 matrix runs, none of the 6 A/B runs); `point-join`
has a slow mode of 8,700 to 10,600 qps (4 of 9 matrix runs, 1 of 6 A/B runs). No delta is claimed on either. The A/B spreads (section 9) are 0.4 % to 4.8 % on the six non-bimodal warm shapes (`cross-join` in its Cabin arm, 18.1 % in the old arm, is the widest).

Contamination: one KDS cell of the whole run (scenario 2's `s2-c1-s`) was discarded and re-run because a `claude` process of another session used more than 50 % of a CPU for one 1-second sample
inside its window; none of scenario 1's cells was contaminated (`logs/validity.log`). The re-run's attempt is under `contaminated/` in the run archive.

Per-cell host stamps (load average before the cell, gate wait, and what the 1-second monitor saw inside the window):

| cell | start (UTC) | end (UTC) | load average before (1 / 5 / 15 min) | gate wait | build or test process | foreign samples >= 10 % CPU | exit |
|---|---|---|---|---|---|---|---|
| `s1-c1-g` | 06:46:51 | 06:47:29 | 0.35 / 0.55 / 0.48 | 0 s | none | 0 of 19 samples | 0 |
| `s1-c8-g` | 06:47:36 | 06:48:15 | 0.54 / 0.58 / 0.49 | 0 s | none | 0 of 20 samples | 0 |
| `s1-c1-s` | 06:48:22 | 06:49:00 | 0.92 / 0.67 / 0.53 | 0 s | none | 1 of 20 samples | 0 |
| `r2-s1-c1-g` | 06:49:07 | 06:49:46 | 0.81 / 0.69 / 0.54 | 0 s | none | 0 of 19 samples | 0 |
| `r2-s1-c8-g` | 06:49:53 | 06:50:32 | 0.76 / 0.70 / 0.55 | 0 s | none | 1 of 20 samples | 0 |
| `r2-s1-c1-s` | 06:50:39 | 06:51:19 | 0.97 / 0.77 / 0.59 | 0 s | none | 0 of 20 samples | 0 |
| `r3-s1-c1-g` | 06:51:26 | 06:52:05 | 0.90 / 0.79 / 0.60 | 0 s | none | 0 of 20 samples | 0 |
| `r3-s1-c8-g` | 06:52:12 | 06:52:51 | 1.24 / 0.89 / 0.65 | 0 s | none | 0 of 19 samples | 0 |
| `r3-s1-c1-s` | 06:52:58 | 06:53:37 | 1.58 / 1.07 / 0.72 | 0 s | none | 0 of 20 samples | 0 |
| `s1s-n252-c1-g` | 07:26:07 | 07:26:12 | 0.27 / 0.33 / 0.59 | 0 s | none | 1 of 5 samples | 0 |
| `s1s-n1008-c1-g` | 07:26:19 | 07:26:24 | 0.30 / 0.34 / 0.59 | 0 s | none | 0 of 4 samples | 0 |
| `s1s-n10080-c1-g` | 07:26:31 | 07:26:42 | 0.30 / 0.34 / 0.58 | 0 s | none | 0 of 7 samples | 0 |
| `r2-s1s-n252-c1-g` | 07:26:49 | 07:26:54 | 0.53 / 0.39 / 0.59 | 0 s | none | 0 of 5 samples | 0 |
| `r2-s1s-n1008-c1-g` | 07:27:01 | 07:27:06 | 0.67 / 0.42 / 0.60 | 0 s | none | 0 of 5 samples | 0 |
| `r2-s1s-n10080-c1-g` | 07:27:13 | 07:27:23 | 0.73 / 0.45 / 0.61 | 0 s | none | 0 of 7 samples | 0 |
| `r3-s1s-n252-c1-g` | 07:27:30 | 07:27:35 | 0.71 / 0.46 / 0.61 | 0 s | none | 0 of 5 samples | 0 |
| `r3-s1s-n1008-c1-g` | 07:27:42 | 07:27:48 | 0.62 / 0.45 / 0.61 | 0 s | none | 0 of 5 samples | 0 |
| `r3-s1s-n10080-c1-g` | 07:27:55 | 07:28:05 | 0.60 / 0.46 / 0.61 | 0 s | none | 1 of 6 samples | 0 |
| `ab-old1-s1-c1-g` | 07:29:44 | 07:30:34 | 0.98 / 0.67 / 0.67 | 0 s | none | 0 of 24 samples | 0 |
| `ab-new1-s1-c1-g` | 07:36:08 | 07:36:47 | 0.07 / 0.38 / 0.56 | 0 s | none | 0 of 20 samples | 0 |
| `ab-old2-s1-c1-g` | 07:42:21 | 07:43:12 | 0.37 / 0.40 / 0.52 | 0 s | none | 0 of 26 samples | 0 |
| `ab-new2-s1-c1-g` | 07:48:47 | 07:49:25 | 0.27 / 0.39 / 0.50 | 0 s | none | 0 of 19 samples | 0 |
| `ab-old3-s1-c1-g` | 07:54:59 | 07:55:49 | 0.15 / 0.42 / 0.50 | 0 s | none | 0 of 24 samples | 0 |
| `ab-new3-s1-c1-g` | 08:01:22 | 08:02:01 | 0.15 / 0.35 / 0.46 | 0 s | none | 0 of 19 samples | 0 |
| `pl-s1-planned-r1` | 08:08:22 | 08:09:01 | 0.49 / 0.43 / 0.46 | 0 s | none | 0 of 20 samples | 0 |
| `pl-s1-written-r1` | 08:09:41 | 08:10:20 | 0.92 / 0.57 / 0.51 | 0 s | none | 0 of 19 samples | 0 |
| `pl-s1-planned-r2` | 08:11:00 | 08:11:39 | 0.89 / 0.67 / 0.55 | 0 s | none | 0 of 20 samples | 0 |
| `pl-s1-written-r2` | 08:12:18 | 08:12:56 | 0.85 / 0.74 / 0.59 | 0 s | none | 1 of 19 samples | 0 |
| `pl-s1-planned-r3` | 08:13:36 | 08:14:15 | 0.96 / 0.81 / 0.63 | 0 s | none | 0 of 20 samples | 0 |
| `pl-s1-written-r3` | 08:16:10 | 08:16:49 | 0.99 / 0.94 / 0.72 | 0 s | none | 0 of 20 samples | 0 |


## 12. Archive

`bench/v3.3.0/archive/scenario1-v3.3.0-38-ga40882af/`: per-cell JSON, the driver stdout of each cell, the server configs, the join-order probe's ANALYZE replies (`plans/`). What the four scenarios share is in
`bench/v3.3.0/archive/run-v3.3.0-38-ga40882af/`: the run scripts, the timeline, the monitor and watchdog logs, the binary hashes, the table generators under `analysis/`, and `rowcost/`. No data file is kept.
