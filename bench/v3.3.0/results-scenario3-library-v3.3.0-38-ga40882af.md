# Scenario 3 (library) at `v3.3.0-38-ga40882af`

**One line.** Twelve read shapes over four relations at 200, 1,000 and 10,000 loans, with and without server indexes, at `cores = 1` and `cores = 8`, three runs each, the morning's binary and the new client decoder.
The decoder moves the shapes that return many rows and nothing else: at 10,000 loans unindexed, `books-by-genre` (about 125 rows) 1.50x, `loans-by-daterange` 1.51x and `overdue`
1.41x in three interleaved old/new pairs (section 8); the one-row and few-row shapes sit at 0.99x to 1.05x. Against the morning's run the same three are +40 %, +40 % and +29 %, and the unindexed
equality walks (`loans-by-user`, `loans-by-book`, `resv-by-user`, `count-by-user`, `join-loan-user`) are **-12 % to -16 %**, which the A/B places on the host (the old client in this window reads 2,475 qps on `loans-by-user`, the morning's cell 2,779 qps).
Against PostgreSQL 18.6, **reused from the morning**, the 10,000-loan columns are 1.07x (unindexed) and 1.14x (indexed) as geometric means over 12 shapes, from 1.06x and 0.98x; the three many-row shapes, 0.61x to 0.77x of PostgreSQL unindexed,
are now 0.96x, 0.86x and 0.99x. At `cores = 1` and 200 or 1,000 loans, 12 of the 48 cells carry a spread above 20 % in this window against 29 of 48 in the morning
(the median spread of the 200-loan cells is 4.5 % unindexed and 3.8 % indexed, from 27.7 % and 23.4 %), so more of the small-relation columns are readable: KDS is at 1.02x of PostgreSQL unindexed at 200 loans (four of its twelve shapes above the 20 % line).
At `cores = 8` the order reverses (10 of 48 above 20 % now, 6 of 48 in the morning; 7 of 12 at 200 loans unindexed).

Driver: `tools/scenario3_library.py`, unmodified, `--index-mode none|all --server-indexes on --seed 1` (`--assert-index-reads` on the indexed cells). Measured by `ck-tester`.
Comparator (this engine's own last run, same binary, old client): `bench/v3.3.0/results-scenario3-library-v3.3.0-33-g9682b725.md`. PostgreSQL 18.6: that file's twin cells (`tools/pg_scenario3_library.py --user cdkbs`, three runs per cell), **not re-run**:
budget lifted by the operator for this measurement; PostgreSQL reused from the same-day run at v3.3.0-33-g9682b725.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; matrix cells (36) 07:05:52 to 07:16:12 UTC; A/B pairs (six cells, interleaved with the scenario 1 and 4 A/B cells) 07:30:38 to 08:02:33 UTC; join-order probe 08:07:47 to 08:17:53 UTC. Per-cell windows: section 10 |
| Worktree, commit, engine binary, client, comparator, budget and everything else common to the run | stamped once in `summary-scenarios-v3.3.0-38-ga40882af.md`, section Stamp |
| Driver | **unmodified** `tools/scenario3_library.py`; exit code 0 and no verify problem in every KDS cell (`verify_problems` empty in 50 of 50); `--assert-index-reads` passed on every indexed cell |

## 2. What was run

| axis | values |
|---|---|
| cores | 1, 8 (`group` durability; reads only, so durability does not enter) |
| index mode | `none` (the driver creates no index) and `all` (it creates the indexes each shape can use, after the load); `--server-indexes on` |
| rows | `--loans 200`, `1000`, `10000`; users, books and reservations scale with it (`sizes` in each JSON: at 200 loans 40 / 40 / 100 relation rows, at 10,000 loans 2,000 / 2,000 / 5,000) |
| runs | 3 per cell (`r2-`, `r3-` prefixes); a fresh server and data file per cell |
| shapes | 12, 200 operations each, serial on one connection, so the **qps is derived**: 1,000,000 / mean µs |
| A/B | `ab-old{1,2,3}-s3-c1-n10000-none` against `ab-new{1,2,3}-s3-c1-n10000-none`, interleaved old, new, old, new, old, new |
| join-order probe | `join_order = planned` (default) and `written`, 10,000 loans, `none` x 3 runs and `all` x 1 run, with the full ANALYZE replies dumped |
| PostgreSQL | not run; the morning's `pg_scenario3_library.py` cells at the same `--loans` and `--index-mode` (three runs each) |

Cut: nothing from the morning's KDS plan. Throughput below is derived qps (1,000,000 / mean µs), the median of three runs.

## 3. Throughput, now against the morning

Findings are read by the rule of `summary-scenarios-v3.3.0-38-ga40882af.md` ("What counts as a finding"): a delta below the larger spread is not a finding; a spread above 20 % carries no claim. A delta against the morning is client plus host (section 8 separates them).

`cores = 1`, derived qps (1,000,000 / mean µs), median of three runs; delta = now over the morning.

**n = 200 loans**

| shape | none: now | spread | none: morning | spread | delta | read | all: now | spread | all: morning | spread | delta | read |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| pk-user | 14,430 qps | 5.3 % | 11,099 qps | 42.8 % | +30.0 % | no claim (spread > 20 %) | 14,535 qps | 2.5 % | 14,706 qps | 0.7 % | -1.2 % | inside spread |
| loans-by-user | 12,210 qps | 1.5 % | 8,496 qps | 36.2 % | +43.7 % | no claim (spread > 20 %) | 12,407 qps | 2.1 % | 11,521 qps | 1.9 % | +7.7 % | **finding** |
| loans-by-book | 12,870 qps | 1.8 % | 8,787 qps | 37.0 % | +46.5 % | no claim (spread > 20 %) | 12,987 qps | 3.9 % | 12,121 qps | 13.0 % | +7.1 % | inside spread |
| resv-by-user | 14,347 qps | 2.2 % | 9,891 qps | 13.0 % | +45.1 % | **finding** | 14,225 qps | 2.9 % | 13,908 qps | 31.3 % | +2.3 % | no claim (spread > 20 %) |
| books-by-author | 14,124 qps | 2.3 % | 9,443 qps | 24.5 % | +49.6 % | no claim (spread > 20 %) | 13,550 qps | 1.5 % | 13,038 qps | 30.1 % | +3.9 % | no claim (spread > 20 %) |
| books-by-genre | 14,306 qps | 3.7 % | 10,204 qps | 39.6 % | +40.2 % | no claim (spread > 20 %) | 14,225 qps | 22.4 % | 13,755 qps | 30.2 % | +3.4 % | no claim (spread > 20 %) |
| loans-by-daterange | 11,669 qps | 3.0 % | 7,930 qps | 33.2 % | +47.1 % | no claim (spread > 20 %) | 11,655 qps | 27.2 % | 10,638 qps | 25.7 % | +9.6 % | no claim (spread > 20 %) |
| overdue | 12,121 qps | 29.9 % | 11,086 qps | 28.5 % | +9.3 % | no claim (spread > 20 %) | 12,300 qps | 3.8 % | 10,929 qps | 21.1 % | +12.5 % | no claim (spread > 20 %) |
| join-loan-user | 10,060 qps | 28.1 % | 9,728 qps | 27.0 % | +3.4 % | no claim (spread > 20 %) | 10,320 qps | 2.7 % | 9,625 qps | 28.1 % | +7.2 % | no claim (spread > 20 %) |
| join-no-literal | 4,902 qps | 15.5 % | 3,709 qps | 2.2 % | +32.2 % | **finding** | 4,602 qps | 6.3 % | 3,432 qps | 14.9 % | +34.1 % | **finding** |
| exists-correlated | 8,097 qps | 23.0 % | 7,651 qps | 0.1 % | +5.8 % | no claim (spread > 20 %) | 7,593 qps | 4.4 % | 6,974 qps | 19.6 % | +8.9 % | inside spread |
| count-by-user | 14,993 qps | 31.8 % | 15,244 qps | 0.6 % | -1.6 % | no claim (spread > 20 %) | 15,408 qps | 3.9 % | 15,198 qps | 30.5 % | +1.4 % | no claim (spread > 20 %) |

**n = 1,000 loans**

| shape | none: now | spread | none: morning | spread | delta | read | all: now | spread | all: morning | spread | delta | read |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| pk-user | 14,493 qps | 24.6 % | 14,184 qps | 4.7 % | +2.2 % | no claim (spread > 20 %) | 14,430 qps | 1.2 % | 10,060 qps | 42.6 % | +43.4 % | no claim (spread > 20 %) |
| loans-by-user | 9,217 qps | 25.2 % | 8,251 qps | 21.0 % | +11.7 % | no claim (spread > 20 %) | 12,048 qps | 0.6 % | 8,210 qps | 32.8 % | +46.7 % | no claim (spread > 20 %) |
| loans-by-book | 9,662 qps | 16.4 % | 7,348 qps | 31.0 % | +31.5 % | no claim (spread > 20 %) | 12,594 qps | 1.0 % | 10,965 qps | 26.7 % | +14.9 % | no claim (spread > 20 %) |
| resv-by-user | 11,947 qps | 1.6 % | 10,965 qps | 29.0 % | +9.0 % | no claim (spread > 20 %) | 13,587 qps | 1.8 % | 12,987 qps | 29.0 % | +4.6 % | no claim (spread > 20 %) |
| books-by-author | 12,903 qps | 0.8 % | 12,315 qps | 28.6 % | +4.8 % | no claim (spread > 20 %) | 13,055 qps | 1.7 % | 8,921 qps | 38.0 % | +46.3 % | no claim (spread > 20 %) |
| books-by-genre | 11,038 qps | 1.1 % | 9,452 qps | 13.8 % | +16.8 % | **finding** | 10,753 qps | 2.4 % | 7,062 qps | 28.7 % | +52.3 % | no claim (spread > 20 %) |
| loans-by-daterange | 5,921 qps | 0.4 % | 4,509 qps | 6.8 % | +31.3 % | **finding** | 5,865 qps | 2.1 % | 4,036 qps | 11.6 % | +45.3 % | **finding** |
| overdue | 6,766 qps | 20.4 % | 5,675 qps | 18.4 % | +19.2 % | no claim (spread > 20 %) | 7,669 qps | 2.0 % | 5,079 qps | 25.5 % | +51.0 % | no claim (spread > 20 %) |
| join-loan-user | 7,880 qps | 24.2 % | 8,130 qps | 2.3 % | -3.1 % | no claim (spread > 20 %) | 10,040 qps | 1.2 % | 7,536 qps | 30.6 % | +33.2 % | no claim (spread > 20 %) |
| join-no-literal | 4,250 qps | 14.5 % | 3,311 qps | 3.9 % | +28.3 % | **finding** | 4,390 qps | 5.0 % | 2,946 qps | 7.0 % | +49.0 % | **finding** |
| exists-correlated | 5,942 qps | 19.7 % | 5,754 qps | 1.1 % | +3.3 % | inside spread | 7,380 qps | 2.2 % | 6,072 qps | 24.9 % | +21.5 % | no claim (spread > 20 %) |
| count-by-user | 10,870 qps | 21.9 % | 11,429 qps | 1.9 % | -4.9 % | no claim (spread > 20 %) | 15,129 qps | 21.7 % | 14,535 qps | 33.2 % | +4.1 % | no claim (spread > 20 %) |

**n = 10,000 loans**

| shape | none: now | spread | none: morning | spread | delta | read | all: now | spread | all: morning | spread | delta | read |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| pk-user | 14,859 qps | 3.6 % | 14,641 qps | 26.8 % | +1.5 % | no claim (spread > 20 %) | 14,599 qps | 9.3 % | 14,859 qps | 1.6 % | -1.8 % | inside spread |
| loans-by-user | 2,449 qps | 1.4 % | 2,779 qps | 9.8 % | -11.9 % | **finding** | 11,682 qps | 4.2 % | 11,001 qps | 1.4 % | +6.2 % | **finding** |
| loans-by-book | 2,431 qps | 5.9 % | 2,828 qps | 3.1 % | -14.0 % | **finding** | 12,438 qps | 3.1 % | 11,507 qps | 18.4 % | +8.1 % | inside spread |
| resv-by-user | 4,255 qps | 11.8 % | 4,950 qps | 11.0 % | -14.0 % | **finding** | 13,477 qps | 3.0 % | 13,245 qps | 1.3 % | +1.8 % | inside spread |
| books-by-author | 6,930 qps | 20.7 % | 7,267 qps | 21.1 % | -4.6 % | no claim (spread > 20 %) | 12,547 qps | 4.2 % | 11,990 qps | 4.1 % | +4.6 % | **finding** |
| books-by-genre | 2,925 qps | 12.2 % | 2,092 qps | 6.2 % | +39.8 % | **finding** | 3,158 qps | 5.5 % | 2,093 qps | 2.1 % | +50.8 % | **finding** |
| loans-by-daterange | 913 qps | 2.0 % | 652 qps | 1.4 % | +40.0 % | **finding** | 933 qps | 2.0 % | 657 qps | 1.0 % | +42.1 % | **finding** |
| overdue | 1,126 qps | 4.4 % | 873 qps | 2.1 % | +29.0 % | **finding** | 1,591 qps | 4.3 % | 1,052 qps | 0.4 % | +51.1 % | **finding** |
| join-loan-user | 2,297 qps | 5.7 % | 2,728 qps | 9.8 % | -15.8 % | **finding** | 10,091 qps | 9.1 % | 9,302 qps | 7.7 % | +8.5 % | inside spread |
| join-no-literal | 1,666 qps | 8.0 % | 1,683 qps | 2.7 % | -1.0 % | inside spread | 4,374 qps | 8.7 % | 3,408 qps | 3.0 % | +28.3 % | **finding** |
| exists-correlated | 1,608 qps | 8.0 % | 1,596 qps | 2.8 % | +0.7 % | inside spread | 7,468 qps | 0.7 % | 6,863 qps | 5.1 % | +8.8 % | **finding** |
| count-by-user | 2,665 qps | 8.7 % | 3,031 qps | 3.7 % | -12.1 % | **finding** | 14,859 qps | 3.2 % | 14,641 qps | 3.7 % | +1.5 % | inside spread |

`cores = 8`, derived qps (1,000,000 / mean µs), median of three runs; delta = now over the morning.

**n = 200 loans**

| shape | none: now | spread | none: morning | spread | delta | read | all: now | spread | all: morning | spread | delta | read |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| pk-user | 11,614 qps | 36.2 % | 13,870 qps | 5.9 % | -16.3 % | no claim (spread > 20 %) | 13,812 qps | 33.1 % | 13,793 qps | 4.2 % | +0.1 % | no claim (spread > 20 %) |
| loans-by-user | 11,364 qps | 20.8 % | 10,929 qps | 6.4 % | +4.0 % | no claim (spread > 20 %) | 10,152 qps | 19.8 % | 10,811 qps | 4.0 % | -6.1 % | inside spread |
| loans-by-book | 12,048 qps | 10.9 % | 11,561 qps | 6.9 % | +4.2 % | inside spread | 12,077 qps | 2.8 % | 11,287 qps | 3.8 % | +7.0 % | **finding** |
| resv-by-user | 11,481 qps | 25.6 % | 13,459 qps | 4.4 % | -14.7 % | no claim (spread > 20 %) | 13,405 qps | 4.0 % | 12,987 qps | 2.7 % | +3.2 % | inside spread |
| books-by-author | 12,594 qps | 22.8 % | 12,937 qps | 4.8 % | -2.6 % | no claim (spread > 20 %) | 12,804 qps | 30.4 % | 11,947 qps | 4.6 % | +7.2 % | no claim (spread > 20 %) |
| books-by-genre | 13,298 qps | 9.1 % | 13,587 qps | 4.8 % | -2.1 % | inside spread | 13,175 qps | 6.3 % | 12,821 qps | 4.4 % | +2.8 % | inside spread |
| loans-by-daterange | 10,246 qps | 12.4 % | 10,194 qps | 5.3 % | +0.5 % | inside spread | 11,074 qps | 2.5 % | 9,785 qps | 4.2 % | +13.2 % | **finding** |
| overdue | 10,060 qps | 22.6 % | 10,893 qps | 2.8 % | -7.6 % | no claim (spread > 20 %) | 11,614 qps | 4.1 % | 10,965 qps | 3.7 % | +5.9 % | **finding** |
| join-loan-user | 8,929 qps | 20.3 % | 9,311 qps | 5.0 % | -4.1 % | no claim (spread > 20 %) | 9,756 qps | 4.3 % | 9,268 qps | 1.7 % | +5.3 % | **finding** |
| join-no-literal | 4,537 qps | 3.2 % | 3,435 qps | 4.4 % | +32.1 % | **finding** | 4,268 qps | 2.1 % | 3,213 qps | 4.0 % | +32.8 % | **finding** |
| exists-correlated | 6,793 qps | 11.0 % | 7,138 qps | 3.7 % | -4.8 % | inside spread | 7,032 qps | 2.5 % | 6,553 qps | 2.0 % | +7.3 % | **finding** |
| count-by-user | 10,593 qps | 36.4 % | 14,409 qps | 5.4 % | -26.5 % | no claim (spread > 20 %) | 14,771 qps | 5.1 % | 14,556 qps | 1.6 % | +1.5 % | inside spread |

**n = 1,000 loans**

| shape | none: now | spread | none: morning | spread | delta | read | all: now | spread | all: morning | spread | delta | read |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| pk-user | 13,699 qps | 0.8 % | 13,643 qps | 2.8 % | +0.4 % | inside spread | 13,495 qps | 1.8 % | 13,387 qps | 27.6 % | +0.8 % | no claim (spread > 20 %) |
| loans-by-user | 8,203 qps | 1.5 % | 8,084 qps | 1.4 % | +1.5 % | **finding** | 11,136 qps | 11.3 % | 10,309 qps | 8.9 % | +8.0 % | inside spread |
| loans-by-book | 8,532 qps | 0.8 % | 8,460 qps | 0.2 % | +0.9 % | **finding** | 11,614 qps | 11.7 % | 9,960 qps | 13.5 % | +16.6 % | **finding** |
| resv-by-user | 11,013 qps | 1.4 % | 10,834 qps | 2.1 % | +1.7 % | inside spread | 12,642 qps | 25.8 % | 10,121 qps | 26.2 % | +24.9 % | no claim (spread > 20 %) |
| books-by-author | 12,121 qps | 7.0 % | 11,223 qps | 27.0 % | +8.0 % | no claim (spread > 20 %) | 12,165 qps | 7.8 % | 10,471 qps | 20.0 % | +16.2 % | no claim (spread > 20 %) |
| books-by-genre | 10,331 qps | 3.8 % | 7,138 qps | 26.7 % | +44.7 % | no claim (spread > 20 %) | 10,163 qps | 2.7 % | 7,994 qps | 17.8 % | +27.1 % | **finding** |
| loans-by-daterange | 5,470 qps | 3.0 % | 3,954 qps | 12.9 % | +38.3 % | **finding** | 5,414 qps | 4.5 % | 4,139 qps | 15.5 % | +30.8 % | **finding** |
| overdue | 6,161 qps | 2.2 % | 4,776 qps | 15.2 % | +29.0 % | **finding** | 7,077 qps | 9.7 % | 5,841 qps | 11.1 % | +21.2 % | **finding** |
| join-loan-user | 7,315 qps | 5.9 % | 6,693 qps | 10.1 % | +9.3 % | inside spread | 9,470 qps | 18.0 % | 9,083 qps | 24.5 % | +4.3 % | no claim (spread > 20 %) |
| join-no-literal | 3,867 qps | 1.5 % | 2,915 qps | 2.8 % | +32.7 % | **finding** | 4,032 qps | 0.4 % | 3,068 qps | 3.8 % | +31.4 % | **finding** |
| exists-correlated | 5,447 qps | 0.9 % | 4,636 qps | 9.6 % | +17.5 % | **finding** | 6,752 qps | 4.4 % | 6,418 qps | 3.5 % | +5.2 % | **finding** |
| count-by-user | 9,560 qps | 1.8 % | 9,709 qps | 10.0 % | -1.5 % | inside spread | 13,831 qps | 12.0 % | 13,532 qps | 15.4 % | +2.2 % | inside spread |

**n = 10,000 loans**

| shape | none: now | spread | none: morning | spread | delta | read | all: now | spread | all: morning | spread | delta | read |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| pk-user | 14,045 qps | 1.5 % | 13,928 qps | 6.2 % | +0.8 % | inside spread | 14,184 qps | 4.6 % | 14,306 qps | 1.1 % | -0.9 % | inside spread |
| loans-by-user | 2,301 qps | 0.7 % | 2,547 qps | 1.9 % | -9.6 % | **finding** | 11,416 qps | 2.0 % | 10,352 qps | 2.9 % | +10.3 % | **finding** |
| loans-by-book | 2,317 qps | 0.5 % | 2,598 qps | 1.0 % | -10.8 % | **finding** | 12,019 qps | 1.7 % | 11,274 qps | 1.0 % | +6.6 % | **finding** |
| resv-by-user | 4,203 qps | 0.7 % | 4,693 qps | 1.6 % | -10.4 % | **finding** | 13,089 qps | 1.4 % | 12,531 qps | 10.7 % | +4.5 % | inside spread |
| books-by-author | 6,601 qps | 0.7 % | 6,845 qps | 1.8 % | -3.6 % | **finding** | 12,180 qps | 2.6 % | 11,561 qps | 29.0 % | +5.4 % | no claim (spread > 20 %) |
| books-by-genre | 2,908 qps | 1.5 % | 2,014 qps | 1.9 % | +44.4 % | **finding** | 2,994 qps | 0.4 % | 2,017 qps | 7.8 % | +48.5 % | **finding** |
| loans-by-daterange | 889 qps | 1.7 % | 621 qps | 0.8 % | +43.0 % | **finding** | 888 qps | 0.6 % | 623 qps | 2.6 % | +42.4 % | **finding** |
| overdue | 1,095 qps | 1.0 % | 829 qps | 1.4 % | +32.0 % | **finding** | 1,518 qps | 2.8 % | 997 qps | 0.0 % | +52.2 % | **finding** |
| join-loan-user | 2,290 qps | 6.8 % | 2,540 qps | 2.4 % | -9.8 % | **finding** | 9,756 qps | 1.3 % | 9,183 qps | 12.4 % | +6.2 % | inside spread |
| join-no-literal | 1,649 qps | 2.7 % | 1,554 qps | 2.5 % | +6.1 % | **finding** | 4,115 qps | 2.7 % | 3,110 qps | 9.3 % | +32.3 % | **finding** |
| exists-correlated | 1,607 qps | 0.2 % | 1,574 qps | 3.9 % | +2.1 % | inside spread | 6,831 qps | 1.0 % | 6,390 qps | 2.4 % | +6.9 % | **finding** |
| count-by-user | 2,417 qps | 1.9 % | 2,695 qps | 4.4 % | -10.3 % | **finding** | 14,245 qps | 1.3 % | 14,006 qps | 3.8 % | +1.7 % | inside spread |


Geometric mean over the 12 shapes of the ratio now over the morning, per cell, and how many shapes lie outside both spreads:

| cell | geometric mean now over morning | min | max | shapes outside spread |
|---|---|---|---|---|
| cores 1, n=200 none | 1.278x | 0.98x | 1.50x | 2 of 12 |
| cores 1, n=200 all | 1.078x | 0.99x | 1.34x | 2 of 12 |
| cores 1, n=1,000 none | 1.118x | 0.95x | 1.31x | 3 of 12 |
| cores 1, n=1,000 all | 1.332x | 1.04x | 1.52x | 2 of 12 |
| cores 1, n=10,000 none | 1.013x | 0.84x | 1.40x | 8 of 12 |
| cores 1, n=10,000 all | 1.161x | 0.98x | 1.51x | 7 of 12 |
| cores 8, n=200 none | 0.959x | 0.74x | 1.32x | 1 of 12 |
| cores 8, n=200 all | 1.063x | 0.94x | 1.33x | 6 of 12 |
| cores 8, n=1,000 none | 1.141x | 0.98x | 1.45x | 6 of 12 |
| cores 8, n=1,000 all | 1.152x | 1.01x | 1.31x | 6 of 12 |
| cores 8, n=10,000 none | 1.044x | 0.89x | 1.44x | 10 of 12 |
| cores 8, n=10,000 all | 1.166x | 0.99x | 1.52x | 7 of 12 |

**Row-returning shapes up, scans down, the rest flat.** At 10,000 loans unindexed `books-by-genre` +39.8 %, `loans-by-daterange` +40.0 %,
`overdue` +29.0 % (every one a finding by its spreads), the five integer-column equalities and joins -12 % to -16 %, `join-no-literal` and `exists-correlated` inside their spreads, `pk-user` flat.
Indexed, the same row-returning shapes move +51 %, +42 % and +51 %, the equalities +2 % to +8 %. At `cores = 1`, 200 and 1,000 loans, 29 of the morning's 48 cells carried a spread above 20 % (a slow mode, one run in three) and cannot be
read against; 12 of 48 do here (200 loans unindexed: 4 of 12 shapes, indexed 2; 1,000 unindexed 5, indexed 1) and are not read. At `cores = 8` it is the other way round
(10 of 48 now, 6 of 48 then), concentrated in the 200-loan unindexed column (7 of 12). A 20 ms cell falling into a slow mode one run in three is a property of the host and the cell length, not of the client; whether it has gone at `cores = 1` or did not fall
in this window is not separated by one run of three.

## 4. What the index buys

Throughput of the indexed cell over the unindexed one, `cores = 1`, median of three, the morning's ratio and PostgreSQL's beside it:

| shape | n=200 all/none | n=1,000 all/none | n=10,000 all/none (now) | n=10,000 all/none (morning) | PG n=10,000 all/none |
|---|---|---|---|---|---|
| pk-user | 1.01x | 1.00x | 0.98x | 1.01x | 1.00x |
| loans-by-user | 1.02x | 1.31x | 4.77x | 3.96x | 4.47x |
| loans-by-book | 1.01x | 1.30x | 5.12x | 4.07x | 5.09x |
| resv-by-user | 0.99x | 1.14x | 3.17x | 2.68x | 3.04x |
| books-by-author | 0.96x | 1.01x | 1.81x | 1.65x | 1.80x |
| books-by-genre | 0.99x | 0.97x | 1.08x | 1.00x | 1.23x |
| loans-by-daterange | 1.00x | 0.99x | 1.02x | 1.01x | 1.00x |
| overdue | 1.01x | 1.13x | 1.41x | 1.21x | 1.68x |
| join-loan-user | 1.03x | 1.27x | 4.39x | 3.41x | 3.84x |
| join-no-literal | 0.94x | 1.03x | 2.63x | 2.02x | 0.98x |
| exists-correlated | 0.94x | 1.24x | 4.65x | 4.30x | 6.68x |
| count-by-user | 1.03x | 1.39x | 5.58x | 4.83x | 4.73x |

An index is worth nothing at 200 loans (the walk is two pages and the statement is a round trip: 0.94x to 1.03x on every shape), 0.97x to 1.39x at 1,000, and 4.4x to 5.6x on four of the equality shapes at 10,000
(`loans-by-user` 4.77x, `loans-by-book`, `join-loan-user`, `count-by-user`; PostgreSQL 4.47x on `loans-by-user`) and 3.2x on `resv-by-user`.
The morning's 3.96x on `loans-by-user` is lower than this run's because the morning's unindexed walk read 12 % faster (section 8) while its indexed cell read 6 % slower; the index did not change. `loans-by-daterange` and `books-by-genre` gain nothing on either side: their cost is the rows returned
(`books-by-genre` about 125 rows) or the range's whole-relation walk.

## 5. The join planner (CB/CC) on the library's joins

The client has no part in a plan. ANALYZE of the join shapes at 10,000 loans (`pl-s3-planned-none-r1`; the replies in full are in the archive) is the morning's plan, line for line: `join-no-literal` is
`Range users` [1, 16] -> `Scan loans`, **`build on=col1`**, `inner_built=1`, `build_rows=79`, `build_probes=15`, `examined=10,106` rows (159 pages), `planned order=memo ... switch=1` (order = written);
`exists-correlated` is `Range users` [1, 20] with a correlated `EXISTS` sub-chain `Scan loans` (`build on=col1`, `build_rows=4,603`, `corr_scans=3`), `examined=4,652`. The planned default against `join_order = written`, same binary, same window:

Scenario 3, `cores = 1`, `group`, 10,000 loans: planned against `join_order = written`.

| cell | shape | planned | spread | written | spread | planned over written |
|---|---|---|---|---|---|---|
| n=10,000 none (3 runs) | join-loan-user | 2,501 qps | 8.8 % | 2,459 qps | 0.6 % | 1.02x |
| n=10,000 none (3 runs) | join-no-literal | 1,782 qps | 6.4 % | 1,796 qps | 2.3 % | 0.99x |
| n=10,000 none (3 runs) | exists-correlated | 1,637 qps | 7.6 % | 1,671 qps | 3.1 % | 0.98x |
| n=10,000 all (1 runs) | join-loan-user | 10,406 qps | n/a | 10,593 qps | n/a | 0.98x |
| n=10,000 all (1 runs) | join-no-literal | 4,604 qps | n/a | 4,507 qps | n/a | 1.02x |
| n=10,000 all (1 runs) | exists-correlated | 7,273 qps | n/a | 7,380 qps | n/a | 0.99x |


Both arms have the same plan (`inner_built=1` in each). Planned over written is the number to read; the decoder cost that hid it is smaller now, and the arms are within their spreads of each other wherever a spread is below 20 %, as in the morning.

## 6. Percentiles

Run 1 of each `cores = 1` cell, every phase (microseconds; the five percentiles are the driver's). The `cores = 8` cells' JSON is in the archive.

#### `s3-c1-n200-none` run 1 (cores = 1)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load | 380 ops | 1,008.2 µs | 1,122.0 µs | 1,168.3 µs | 1,301.4 µs | 1,662.2 µs | 6,454.2 µs | 0 errors |
| pk-user | 200 ops | 60.6 µs | 62.2 µs | 64.3 µs | 82.6 µs | 91.6 µs | 204.6 µs | 0 errors |
| loans-by-user | 200 ops | 67.7 µs | 76.2 µs | 80.7 µs | 97.5 µs | 107.9 µs | 122.0 µs | 0 errors |
| loans-by-book | 200 ops | 67.4 µs | 72.8 µs | 76.2 µs | 92.7 µs | 98.3 µs | 100.4 µs | 0 errors |
| resv-by-user | 200 ops | 62.0 µs | 66.4 µs | 68.0 µs | 83.1 µs | 92.5 µs | 100.1 µs | 0 errors |
| books-by-author | 200 ops | 62.4 µs | 65.5 µs | 70.1 µs | 85.3 µs | 97.1 µs | 109.7 µs | 0 errors |
| books-by-genre | 200 ops | 61.3 µs | 64.3 µs | 66.0 µs | 81.9 µs | 86.0 µs | 90.3 µs | 0 errors |
| loans-by-daterange | 200 ops | 72.5 µs | 79.1 µs | 83.2 µs | 102.0 µs | 128.9 µs | 140.3 µs | 0 errors |
| overdue | 200 ops | 100.1 µs | 112.5 µs | 115.5 µs | 136.1 µs | 144.4 µs | 147.5 µs | 0 errors |
| join-loan-user | 200 ops | 116.7 µs | 128.2 µs | 134.1 µs | 155.9 µs | 167.0 µs | 212.3 µs | 0 errors |
| join-no-literal | 200 ops | 226.2 µs | 230.0 µs | 232.8 µs | 254.9 µs | 258.9 µs | 296.1 µs | 0 errors |
| exists-correlated | 200 ops | 148.6 µs | 150.9 µs | 152.7 µs | 176.9 µs | 181.2 µs | 224.7 µs | 0 errors |
| count-by-user | 200 ops | 91.3 µs | 92.8 µs | 93.6 µs | 113.7 µs | 118.6 µs | 123.7 µs | 0 errors |

#### `s3-c1-n200-all` run 1 (cores = 1)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load | 380 ops | 1,014.3 µs | 1,134.4 µs | 1,193.0 µs | 1,456.5 µs | 8,926.7 µs | 27,233.3 µs | 0 errors |
| create-index | 8 ops | 1,203.5 µs | 1,379.5 µs | 1,385.0 µs | 1,645.0 µs | 1,645.0 µs | 1,645.0 µs | 0 errors |
| pk-user | 200 ops | 60.2 µs | 62.5 µs | 66.1 µs | 86.4 µs | 106.0 µs | 185.8 µs | 0 errors |
| loans-by-user | 200 ops | 62.7 µs | 74.9 µs | 79.3 µs | 99.0 µs | 107.9 µs | 111.1 µs | 0 errors |
| loans-by-book | 200 ops | 64.6 µs | 71.7 µs | 76.1 µs | 91.8 µs | 103.4 µs | 107.0 µs | 0 errors |
| resv-by-user | 200 ops | 59.4 µs | 66.2 µs | 68.4 µs | 84.8 µs | 90.5 µs | 103.8 µs | 0 errors |
| books-by-author | 200 ops | 63.6 µs | 67.6 µs | 72.6 µs | 90.7 µs | 101.8 µs | 112.4 µs | 0 errors |
| books-by-genre | 200 ops | 62.4 µs | 65.8 µs | 68.7 µs | 84.9 µs | 97.8 µs | 115.6 µs | 0 errors |
| loans-by-daterange | 200 ops | 71.5 µs | 79.2 µs | 83.5 µs | 102.5 µs | 111.6 µs | 129.5 µs | 0 errors |
| overdue | 200 ops | 65.1 µs | 76.6 µs | 80.4 µs | 97.1 µs | 105.1 µs | 111.6 µs | 0 errors |
| join-loan-user | 200 ops | 80.4 µs | 90.6 µs | 94.9 µs | 113.2 µs | 124.2 µs | 151.2 µs | 0 errors |
| join-no-literal | 200 ops | 202.6 µs | 207.7 µs | 215.5 µs | 237.0 µs | 251.7 µs | 281.8 µs | 0 errors |
| exists-correlated | 200 ops | 123.3 µs | 125.8 µs | 127.5 µs | 146.3 µs | 155.6 µs | 168.4 µs | 0 errors |
| count-by-user | 200 ops | 57.6 µs | 60.4 µs | 62.0 µs | 83.3 µs | 93.2 µs | 134.9 µs | 0 errors |

#### `s3-c1-n1000-none` run 1 (cores = 1)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load | 1,900 ops | 960.5 µs | 1,142.4 µs | 1,202.7 µs | 1,478.9 µs | 2,133.5 µs | 6,888.2 µs | 0 errors |
| pk-user | 200 ops | 60.3 µs | 62.2 µs | 64.7 µs | 87.0 µs | 106.0 µs | 211.3 µs | 0 errors |
| loans-by-user | 200 ops | 94.7 µs | 102.6 µs | 106.9 µs | 123.9 µs | 129.4 µs | 132.7 µs | 0 errors |
| loans-by-book | 200 ops | 93.1 µs | 98.5 µs | 101.6 µs | 118.4 µs | 125.9 µs | 134.5 µs | 0 errors |
| resv-by-user | 200 ops | 74.0 µs | 79.4 µs | 81.8 µs | 100.9 µs | 106.0 µs | 116.1 µs | 0 errors |
| books-by-author | 200 ops | 66.0 µs | 73.5 µs | 76.0 µs | 91.9 µs | 97.5 µs | 104.1 µs | 0 errors |
| books-by-genre | 200 ops | 77.9 µs | 85.9 µs | 89.0 µs | 105.6 µs | 115.4 µs | 121.4 µs | 0 errors |
| loans-by-daterange | 200 ops | 100.9 µs | 160.7 µs | 167.3 µs | 194.9 µs | 204.6 µs | 225.5 µs | 0 errors |
| overdue | 200 ops | 100.4 µs | 140.5 µs | 146.2 µs | 164.7 µs | 171.3 µs | 176.1 µs | 0 errors |
| join-loan-user | 200 ops | 110.2 µs | 116.4 µs | 120.7 µs | 141.4 µs | 153.1 µs | 180.6 µs | 0 errors |
| join-no-literal | 200 ops | 222.3 µs | 227.2 µs | 231.5 µs | 248.9 µs | 257.2 µs | 293.4 µs | 0 errors |
| exists-correlated | 200 ops | 156.1 µs | 159.1 µs | 161.4 µs | 181.1 µs | 189.6 µs | 242.9 µs | 0 errors |
| count-by-user | 200 ops | 86.8 µs | 88.0 µs | 89.0 µs | 105.6 µs | 110.1 µs | 141.8 µs | 0 errors |

#### `s3-c1-n1000-all` run 1 (cores = 1)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load | 1,900 ops | 979.8 µs | 1,117.6 µs | 1,174.9 µs | 1,462.6 µs | 2,649.1 µs | 25,386.9 µs | 0 errors |
| create-index | 8 ops | 1,284.1 µs | 1,374.3 µs | 1,649.8 µs | 2,851.0 µs | 2,851.0 µs | 2,851.0 µs | 0 errors |
| pk-user | 200 ops | 61.6 µs | 63.7 µs | 65.8 µs | 82.8 µs | 103.5 µs | 226.8 µs | 0 errors |
| loans-by-user | 200 ops | 67.4 µs | 76.8 µs | 80.8 µs | 96.7 µs | 116.1 µs | 136.3 µs | 0 errors |
| loans-by-book | 200 ops | 66.2 µs | 73.7 µs | 78.1 µs | 92.0 µs | 96.5 µs | 97.4 µs | 0 errors |
| resv-by-user | 200 ops | 61.1 µs | 68.7 µs | 71.4 µs | 83.9 µs | 92.8 µs | 99.1 µs | 0 errors |
| books-by-author | 200 ops | 62.0 µs | 71.4 µs | 74.1 µs | 87.8 µs | 97.0 µs | 127.8 µs | 0 errors |
| books-by-genre | 200 ops | 78.5 µs | 87.0 µs | 91.0 µs | 105.9 µs | 121.6 µs | 128.8 µs | 0 errors |
| loans-by-daterange | 200 ops | 104.5 µs | 160.8 µs | 167.0 µs | 186.9 µs | 197.0 µs | 226.2 µs | 0 errors |
| overdue | 200 ops | 73.7 µs | 120.6 µs | 127.5 µs | 153.6 µs | 161.7 µs | 166.4 µs | 0 errors |
| join-loan-user | 200 ops | 83.5 µs | 91.6 µs | 97.1 µs | 114.3 µs | 137.5 µs | 141.4 µs | 0 errors |
| join-no-literal | 200 ops | 213.6 µs | 217.4 µs | 221.5 µs | 240.2 µs | 251.8 µs | 299.4 µs | 0 errors |
| exists-correlated | 200 ops | 127.7 µs | 129.5 µs | 130.7 µs | 148.6 µs | 156.5 µs | 158.7 µs | 0 errors |
| count-by-user | 200 ops | 60.1 µs | 63.4 µs | 64.7 µs | 78.8 µs | 87.3 µs | 93.8 µs | 0 errors |

#### `s3-c1-n10000-none` run 1 (cores = 1)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load | 19,000 ops | 954.6 µs | 1,116.5 µs | 1,173.1 µs | 1,400.1 µs | 2,350.3 µs | 16,798.3 µs | 0 errors |
| pk-user | 200 ops | 60.0 µs | 61.3 µs | 62.4 µs | 81.4 µs | 107.3 µs | 210.1 µs | 0 errors |
| loans-by-user | 200 ops | 376.3 µs | 392.4 µs | 401.5 µs | 445.0 µs | 455.1 µs | 463.2 µs | 0 errors |
| loans-by-book | 200 ops | 383.3 µs | 416.7 µs | 423.7 µs | 445.3 µs | 464.1 µs | 533.5 µs | 0 errors |
| resv-by-user | 200 ops | 225.2 µs | 253.9 µs | 257.5 µs | 280.7 µs | 285.9 µs | 304.6 µs | 0 errors |
| books-by-author | 200 ops | 161.9 µs | 170.9 µs | 174.3 µs | 196.7 µs | 200.0 µs | 216.2 µs | 0 errors |
| books-by-genre | 200 ops | 332.8 µs | 354.9 µs | 362.9 µs | 388.6 µs | 393.8 µs | 398.6 µs | 0 errors |
| loans-by-daterange | 200 ops | 495.4 µs | 1,079.1 µs | 1,108.4 µs | 1,152.8 µs | 1,175.0 µs | 1,561.1 µs | 0 errors |
| overdue | 200 ops | 427.9 µs | 884.2 µs | 908.4 µs | 970.0 µs | 980.8 µs | 1,059.2 µs | 0 errors |
| join-loan-user | 200 ops | 379.0 µs | 391.8 µs | 405.0 µs | 451.8 µs | 487.9 µs | 566.4 µs | 0 errors |
| join-no-literal | 200 ops | 538.6 µs | 549.6 µs | 554.8 µs | 573.4 µs | 582.0 µs | 613.1 µs | 0 errors |
| exists-correlated | 200 ops | 574.8 µs | 588.8 µs | 596.5 µs | 618.4 µs | 640.0 µs | 860.2 µs | 0 errors |
| count-by-user | 200 ops | 360.9 µs | 367.4 µs | 371.7 µs | 391.8 µs | 435.1 µs | 488.0 µs | 0 errors |

#### `s3-c1-n10000-all` run 1 (cores = 1)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load | 19,000 ops | 966.5 µs | 1,125.5 µs | 1,183.4 µs | 1,422.4 µs | 2,405.2 µs | 22,680.1 µs | 0 errors |
| create-index | 8 ops | 3,121.8 µs | 3,554.8 µs | 6,238.5 µs | 11,066.3 µs | 11,066.3 µs | 11,066.3 µs | 0 errors |
| pk-user | 200 ops | 62.0 µs | 63.7 µs | 64.7 µs | 86.6 µs | 99.0 µs | 209.2 µs | 0 errors |
| loans-by-user | 200 ops | 69.4 µs | 79.0 µs | 83.4 µs | 104.7 µs | 118.3 µs | 134.5 µs | 0 errors |
| loans-by-book | 200 ops | 63.6 µs | 74.2 µs | 79.1 µs | 96.0 µs | 108.7 µs | 113.9 µs | 0 errors |
| resv-by-user | 200 ops | 62.5 µs | 69.5 µs | 72.4 µs | 89.5 µs | 96.5 µs | 106.2 µs | 0 errors |
| books-by-author | 200 ops | 62.4 µs | 74.3 µs | 77.6 µs | 96.6 µs | 106.7 µs | 121.1 µs | 0 errors |
| books-by-genre | 200 ops | 280.2 µs | 305.1 µs | 317.7 µs | 340.6 µs | 348.2 µs | 367.2 µs | 0 errors |
| loans-by-daterange | 200 ops | 456.0 µs | 1,048.6 µs | 1,078.3 µs | 1,140.0 µs | 1,167.9 µs | 1,532.2 µs | 0 errors |
| overdue | 200 ops | 88.4 µs | 630.2 µs | 661.7 µs | 732.6 µs | 765.9 µs | 783.9 µs | 0 errors |
| join-loan-user | 200 ops | 85.7 µs | 93.8 µs | 97.5 µs | 142.2 µs | 164.4 µs | 182.2 µs | 0 errors |
| join-no-literal | 200 ops | 211.2 µs | 218.9 µs | 226.0 µs | 285.4 µs | 350.9 µs | 401.4 µs | 0 errors |
| exists-correlated | 200 ops | 127.8 µs | 129.9 µs | 131.0 µs | 147.2 µs | 155.1 µs | 162.0 µs | 0 errors |
| count-by-user | 200 ops | 60.3 µs | 64.2 µs | 66.0 µs | 81.9 µs | 97.3 µs | 111.1 µs | 0 errors |


The three many-row shapes at 10,000 loans unindexed, run 1 now against run 1 of the morning and of PostgreSQL (the morning's twin run):

| phase | run | ops | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| books-by-genre | now | 200 ops | 332.8 µs | 354.9 µs | 362.9 µs | 388.6 µs | 393.8 µs |
| books-by-genre | morning (old client) | 200 ops | 427.8 µs | 459.6 µs | 475.9 µs | 511.1 µs | 543.9 µs |
| books-by-genre | PostgreSQL 18.6 (morning's run) | 200 ops | 284.6 µs | 322.2 µs | 332.2 µs | 362.5 µs | 385.3 µs |
| loans-by-daterange | now | 200 ops | 495.4 µs | 1,079.1 µs | 1,108.4 µs | 1,152.8 µs | 1,175.0 µs |
| loans-by-daterange | morning (old client) | 200 ops | 440.8 µs | 1,511.8 µs | 1,551.4 µs | 1,667.6 µs | 1,742.5 µs |
| loans-by-daterange | PostgreSQL 18.6 (morning's run) | 200 ops | 514.4 µs | 917.4 µs | 933.3 µs | 974.2 µs | 1,281.9 µs |
| overdue | now | 200 ops | 427.9 µs | 884.2 µs | 908.4 µs | 970.0 µs | 980.8 µs |
| overdue | morning (old client) | 200 ops | 378.9 µs | 1,131.6 µs | 1,173.5 µs | 1,296.1 µs | 1,419.8 µs |
| overdue | PostgreSQL 18.6 (morning's run) | 200 ops | 475.2 µs | 893.2 µs | 907.5 µs | 1,109.0 µs | 1,124.8 µs |

The whole distribution moved, not the median alone: `books-by-genre` p0 0.78x, p50 0.76x, p99 0.72x of the morning's; `loans-by-daterange` p50 0.71x
and `overdue` p50 0.77x (the scan beneath them is the same engine work, so the saving is the rows' share of a statement). Every one of the three is tight: p99 is within 9 % of p50.

PostgreSQL 18.6 at 10,000 loans, run 1 of the morning's cells, for every phase:

#### PostgreSQL 18.6, `pg-s3-n10000-none` run 1 (the morning's run)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load | 19,000 ops | 958.3 µs | 1,122.9 µs | 1,191.5 µs | 1,558.5 µs | 2,383.7 µs | 25,375.9 µs | 0 errors |
| pk-user | 200 ops | 61.4 µs | 84.9 µs | 85.9 µs | 102.3 µs | 114.5 µs | 222.7 µs | 0 errors |
| loans-by-user | 200 ops | 385.6 µs | 398.0 µs | 410.8 µs | 444.7 µs | 498.0 µs | 560.2 µs | 0 errors |
| loans-by-book | 200 ops | 409.5 µs | 426.5 µs | 440.8 µs | 466.7 µs | 471.3 µs | 478.2 µs | 0 errors |
| resv-by-user | 200 ops | 200.6 µs | 207.1 µs | 214.5 µs | 251.3 µs | 269.0 µs | 406.5 µs | 0 errors |
| books-by-author | 200 ops | 143.6 µs | 152.2 µs | 155.2 µs | 177.9 µs | 185.7 µs | 245.6 µs | 0 errors |
| books-by-genre | 200 ops | 284.6 µs | 322.2 µs | 332.2 µs | 362.5 µs | 385.3 µs | 431.6 µs | 0 errors |
| loans-by-daterange | 200 ops | 514.4 µs | 917.4 µs | 933.3 µs | 974.2 µs | 1,281.9 µs | 2,657.5 µs | 0 errors |
| overdue | 200 ops | 475.2 µs | 893.2 µs | 907.5 µs | 1,109.0 µs | 1,124.8 µs | 1,152.2 µs | 0 errors |
| join-loan-user | 200 ops | 408.8 µs | 420.5 µs | 428.6 µs | 449.2 µs | 460.7 µs | 550.8 µs | 0 errors |
| join-no-literal | 200 ops | 635.3 µs | 647.1 µs | 655.8 µs | 676.3 µs | 704.6 µs | 821.0 µs | 0 errors |
| exists-correlated | 200 ops | 1,257.3 µs | 1,290.7 µs | 1,303.1 µs | 1,329.8 µs | 1,353.7 µs | 1,444.2 µs | 0 errors |
| count-by-user | 200 ops | 386.2 µs | 392.4 µs | 410.8 µs | 440.1 µs | 502.2 µs | 547.9 µs | 0 errors |

#### PostgreSQL 18.6, `pg-s3-n10000-all` run 1 (the morning's run)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| load | 19,000 ops | 973.4 µs | 1,139.1 µs | 1,204.1 µs | 1,570.9 µs | 2,396.4 µs | 16,208.5 µs | 0 errors |
| create-index | 12 ops | 2,232.2 µs | 2,417.0 µs | 3,832.1 µs | 12,161.1 µs | 12,161.1 µs | 12,161.1 µs | 0 errors |
| pk-user | 200 ops | 83.9 µs | 85.4 µs | 86.1 µs | 102.9 µs | 109.0 µs | 282.1 µs | 0 errors |
| loans-by-user | 200 ops | 99.0 µs | 108.4 µs | 113.4 µs | 137.0 µs | 160.3 µs | 250.5 µs | 0 errors |
| loans-by-book | 200 ops | 91.9 µs | 104.1 µs | 107.4 µs | 129.3 µs | 141.2 µs | 145.8 µs | 0 errors |
| resv-by-user | 200 ops | 86.6 µs | 95.5 µs | 97.8 µs | 118.9 µs | 125.1 µs | 196.6 µs | 0 errors |
| books-by-author | 200 ops | 90.1 µs | 101.0 µs | 104.1 µs | 123.7 µs | 132.2 µs | 244.8 µs | 0 errors |
| books-by-genre | 200 ops | 261.0 µs | 278.4 µs | 289.6 µs | 314.6 µs | 324.6 µs | 390.8 µs | 0 errors |
| loans-by-daterange | 200 ops | 529.6 µs | 945.9 µs | 1,087.5 µs | 1,156.6 µs | 1,473.2 µs | 2,827.9 µs | 0 errors |
| overdue | 200 ops | 88.5 µs | 526.6 µs | 550.5 µs | 621.4 µs | 644.7 µs | 1,579.8 µs | 0 errors |
| join-loan-user | 200 ops | 99.4 µs | 107.1 µs | 111.3 µs | 137.3 µs | 157.7 µs | 220.0 µs | 0 errors |
| join-no-literal | 200 ops | 665.3 µs | 693.7 µs | 707.3 µs | 722.4 µs | 737.6 µs | 840.2 µs | 0 errors |
| exists-correlated | 200 ops | 214.9 µs | 217.9 µs | 220.1 µs | 243.8 µs | 248.4 µs | 284.5 µs | 0 errors |
| count-by-user | 200 ops | 105.2 µs | 108.1 µs | 109.5 µs | 128.8 µs | 134.1 µs | 186.6 µs | 0 errors |



## 7. Where the time goes

Reads on one connection, no writes after the load, no commit, no lock: the latency is a fixed round trip plus rows walked plus rows returned, with no durability or conflict wait to separate.

| Component | KDS now | PostgreSQL (morning's run) | How derived |
|---|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | **65.7 µs** | 84.5 µs | `pk-user` mean at 10,000 loans, run 1 |
| Durability / commit | not applicable | not applicable | read-only shapes |
| Lock or conflict wait | none | none | one connection |

Cost per row walked by an unindexed equality (mean at 10,000 loans minus mean at 200, over the 9,800 extra rows of `loans`), and the cost of a returned row (`books-by-genre` returns about 125 rows):

| shape | KDS mean n=200 | KDS mean n=10,000 | KDS per extra row now | KDS per extra row morning | PG per extra row | KDS now / PG |
|---|---|---|---|---|---|---|
| pk-user | 69.3 µs | 67.3 µs | -0.2 ns | -2.2 ns | 0.2 ns | - (a flat shape) |
| loans-by-user | 81.9 µs | 408.4 µs | 33.3 ns | 24.7 ns | 34.4 ns | 0.97x |
| loans-by-book | 77.7 µs | 411.4 µs | 34.1 ns | 24.5 ns | 36.7 ns | 0.93x |
| books-by-genre | 69.9 µs | 341.9 µs | 27.8 ns | 38.8 ns | 27.0 ns | 1.03x |

This table carries the host drift: the walk costs 33.3 ns per extra row here against 24.7 ns in the morning for `loans-by-user` and PostgreSQL's 34.4 ns, so **the morning's reading that KDS walks cheaper per examined row than PostgreSQL
(24.5 and 24.7 ns against 34.4 and 36.7 ns) is not reproduced in this window**: the old client reads the same walk as slowly (section 8). The returned-row term moved the other way: `books-by-genre` costs 27.8 ns per extra relation row against 38.8 ns in the morning (PostgreSQL 27.0 ns),
which is the decoder. Which of the two windows has the engine's true walk cost is not separated by this run; the morning's PostgreSQL cells ran at 04:23 to 04:31 UTC, closer in time to the morning's KDS cells (03:58 to 04:07) than to these.

## 8. The client, isolated: old against new, interleaved

Three pairs on the same host, `s3-c1-n10000-none` each (a fresh server and data file per cell, the same binary copy; old = `a40882af^`'s client from `oldtools/`).

| shape | old: 1 / 2 / 3 | old median | spread | new: 1 / 2 / 3 | new median | spread | new over old | read |
|---|---|---|---|---|---|---|---|---|
| pk-user | 14,793 / 14,815 / 14,948 qps | 14,815 qps | 1.0 % | 14,749 / 14,793 / 14,881 qps | 14,793 qps | 0.9 % | 1.00x (-0.1 %) | inside spread |
| loans-by-user | 2,439 / 2,496 / 2,475 qps | 2,475 qps | 2.3 % | 2,510 / 2,527 / 2,525 qps | 2,525 qps | 0.7 % | 1.02x (+2.0 %) | inside spread |
| loans-by-book | 2,341 / 2,534 / 2,503 qps | 2,503 qps | 7.7 % | 2,539 / 2,445 / 2,537 qps | 2,537 qps | 3.7 % | 1.01x (+1.4 %) | inside spread |
| resv-by-user | 3,810 / 4,371 / 4,350 qps | 4,350 qps | 12.9 % | 4,344 / 3,871 / 4,431 qps | 4,344 qps | 12.9 % | 1.00x (-0.1 %) | inside spread |
| books-by-author | 5,543 / 6,812 / 6,676 qps | 6,676 qps | 19.0 % | 6,935 / 6,978 / 6,998 qps | 6,978 qps | 0.9 % | 1.05x (+4.5 %) | inside spread |
| books-by-genre | 1,908 / 2,048 / 2,035 qps | 2,035 qps | 6.9 % | 3,051 / 3,080 / 3,053 qps | 3,053 qps | 1.0 % | 1.50x (+50.0 %) | **finding** |
| loans-by-daterange | 615 / 621 / 629 qps | 621 qps | 2.3 % | 938 / 954 / 934 qps | 938 qps | 2.2 % | 1.51x (+51.0 %) | **finding** |
| overdue | 813 / 833 / 820 qps | 820 qps | 2.5 % | 1,152 / 1,172 / 1,121 qps | 1,152 qps | 4.5 % | 1.41x (+40.6 %) | **finding** |
| join-loan-user | 2,233 / 2,473 / 2,425 qps | 2,425 qps | 9.9 % | 2,499 / 2,473 / 2,294 qps | 2,473 qps | 8.3 % | 1.02x (+2.0 %) | inside spread |
| join-no-literal | 1,539 / 1,558 / 1,578 qps | 1,558 qps | 2.5 % | 1,766 / 1,783 / 1,657 qps | 1,766 qps | 7.1 % | 1.13x (+13.3 %) | **finding** |
| exists-correlated | 1,635 / 1,643 / 1,644 qps | 1,643 qps | 0.5 % | 1,642 / 1,671 / 1,548 qps | 1,642 qps | 7.5 % | 1.00x (-0.0 %) | inside spread |
| count-by-user | 2,638 / 2,658 / 2,656 qps | 2,656 qps | 0.8 % | 2,637 / 2,655 / 2,461 qps | 2,637 qps | 7.4 % | 0.99x (-0.7 %) | inside spread |


**The decoder moves exactly the shapes that return rows.** `books-by-genre` 1.50x, `loans-by-daterange` 1.51x, `overdue` 1.41x,
`join-no-literal` 1.13x (the build keeps 79 rows, and the join returns them), against 0.99x to 1.05x for the shapes that return 5 rows or fewer. **The -12 % to -16 % on the equality walks is not the client**: the old client
reads `loans-by-user` 2,475 qps, `loans-by-book` 2,503 qps, `resv-by-user` 4,350 qps and `count-by-user` 2,656 qps in this window, against the morning's 2,779 qps, 2,828 qps,
4,950 qps and 3,031 qps, with the same binary. The same host effect as scenario 1's scans (section 9 of its file); not separated into frequency, neighbour or placement.

## 9. Against PostgreSQL 18.6 (the morning's run, reused)

PostgreSQL is the floor: it answers "is the engine in the right league" and nothing finer. Its twin was not re-run; ratios are KDS `cores = 1` median qps (this run) over the morning's PostgreSQL median qps (above 1.00x = KDS faster), the morning's KDS ratio after the slash:

| shape | n=200 none: now / morning | n=200 all: now / morning | n=1,000 none: now / morning | n=1,000 all: now / morning | n=10,000 none: now / morning | n=10,000 all: now / morning |
|---|---|---|---|---|---|---|
| pk-user | 0.93x / 0.72x | 0.92x / 0.93x | 1.01x / 0.99x | 1.01x / 0.70x | 1.00x / 0.98x | 0.98x / 1.00x |
| loans-by-user | 0.92x / 0.64x | 1.22x / 1.14x | 0.96x / 0.86x | 1.07x / 0.73x | 1.01x / 1.15x | 1.08x / 1.02x |
| loans-by-book | 0.95x / 0.65x | 1.36x / 1.27x | 1.01x / 0.76x | 1.08x / 0.94x | 1.05x / 1.23x | 1.06x / 0.98x |
| resv-by-user | 0.94x / 0.65x | 1.33x / 1.30x | 0.97x / 0.89x | 1.03x / 0.98x | 1.00x / 1.16x | 1.04x / 1.02x |
| books-by-author | 0.93x / 0.62x | 1.31x / 1.26x | 0.96x / 0.91x | 1.12x / 0.76x | 1.03x / 1.08x | 1.04x / 0.99x |
| books-by-genre | 0.92x / 0.65x | 1.34x / 1.29x | 0.94x / 0.81x | 1.04x / 0.69x | 0.96x / 0.69x | 0.84x / 0.56x |
| loans-by-daterange | 1.00x / 0.68x | 1.04x / 0.95x | 0.99x / 0.75x | 1.03x / 0.71x | 0.86x / 0.61x | 0.88x / 0.62x |
| overdue | 0.99x / 0.91x | 1.15x / 1.02x | 1.03x / 0.87x | 1.05x / 0.70x | 0.99x / 0.77x | 0.83x / 0.55x |
| join-loan-user | 1.02x / 0.99x | 1.18x / 1.10x | 1.07x / 1.10x | 1.20x / 0.90x | 0.99x / 1.17x | 1.13x / 1.04x |
| join-no-literal | 1.12x / 0.85x | 1.13x / 0.84x | 1.23x / 0.96x | 1.40x / 0.94x | 1.11x / 1.12x | 2.96x / 2.31x |
| exists-correlated | 1.43x / 1.35x | 1.64x / 1.51x | 1.79x / 1.73x | 1.53x / 1.26x | 2.09x / 2.08x | 1.46x / 1.34x |
| count-by-user | 1.12x / 1.14x | 1.62x / 1.60x | 1.12x / 1.18x | 1.28x / 1.23x | 1.10x / 1.25x | 1.29x / 1.27x |

| cells | geometric mean KDS now / PG | geometric mean KDS morning / PG | min now | max now |
|---|---|---|---|---|
| n=200 none | 1.02x | 0.79x | 0.92x | 1.43x |
| n=200 all | 1.25x | 1.16x | 0.92x | 1.64x |
| n=1,000 none | 1.07x | 0.96x | 0.94x | 1.79x |
| n=1,000 all | 1.14x | 0.86x | 1.01x | 1.53x |
| n=10,000 none | 1.07x | 1.06x | 0.86x | 2.09x |
| n=10,000 all | 1.14x | 0.98x | 0.83x | 2.96x |

PostgreSQL 18.6 median qps (the morning's run):

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 15,480 qps | 15,748 qps | 14,388 qps | 14,347 qps | 14,925 qps | 14,925 qps |
| loans-by-user | 13,263 qps | 10,142 qps | 9,643 qps | 11,261 qps | 2,421 qps | 10,834 qps |
| loans-by-book | 13,532 qps | 9,542 qps | 9,606 qps | 11,669 qps | 2,307 qps | 11,751 qps |
| resv-by-user | 15,198 qps | 10,707 qps | 12,330 qps | 13,228 qps | 4,259 qps | 12,953 qps |
| books-by-author | 15,129 qps | 10,341 qps | 13,495 qps | 11,682 qps | 6,734 qps | 12,107 qps |
| books-by-genre | 15,601 qps | 10,638 qps | 11,696 qps | 10,309 qps | 3,041 qps | 3,755 qps |
| loans-by-daterange | 11,710 qps | 11,223 qps | 5,984 qps | 5,669 qps | 1,063 qps | 1,065 qps |
| overdue | 12,225 qps | 10,741 qps | 6,540 qps | 7,273 qps | 1,135 qps | 1,911 qps |
| join-loan-user | 9,833 qps | 8,780 qps | 7,375 qps | 8,347 qps | 2,327 qps | 8,929 qps |
| join-no-literal | 4,380 qps | 4,062 qps | 3,452 qps | 3,135 qps | 1,504 qps | 1,478 qps |
| exists-correlated | 5,653 qps | 4,632 qps | 3,322 qps | 4,836 qps | 768 qps | 5,128 qps |
| count-by-user | 13,351 qps | 9,524 qps | 9,662 qps | 11,820 qps | 2,434 qps | 11,521 qps |

**Reading.** At 10,000 loans the engine is in the PostgreSQL league and the many-row shapes no longer set it apart: `books-by-genre` 0.96x (from 0.69x), `loans-by-daterange` 0.86x (from 0.61x),
`overdue` 0.99x (from 0.77x); indexed, 0.84x, 0.88x and 0.83x. The shapes that gained nothing from the decoder read lower against a PostgreSQL that was measured before the drift (unindexed equalities 1.00x to 1.10x, from 1.08x to 1.25x). At 200 loans KDS is at
1.02x unindexed and 1.25x indexed (geometric means); the morning's 0.79x came with spreads of 20 % to 43 % on most of those cells (section 3), and this window's figure has four of its twelve shapes above the 20 % line, so neither is a settled number.

## 10. Noise floor, contamination and host

The noise floor is the 3-run spread of one cell: at 10,000 loans unindexed it is 0.7 % to 20.7 % across the 12 shapes, median 7.0 %; the A/B spreads are 0.5 % to 19 %. `books-by-author` (20.7 %, 19.0 %) and `resv-by-user` (11.8 %, 12.9 %)
carry the widest. Contamination: none in this scenario (`logs/validity.log`; summary, "Contamination").

Per-cell host stamps (load average before the cell, gate wait, and what the 1-second monitor saw inside the window):

| cell | start (UTC) | end (UTC) | load average before (1 / 5 / 15 min) | gate wait | build or test process | foreign samples >= 10 % CPU | exit |
|---|---|---|---|---|---|---|---|
| `s3-c1-n200-none` | 07:05:55 | 07:05:57 | 1.04 / 1.08 / 0.94 | 0 s | none | 0 of 3 samples | 0 |
| `s3-c1-n200-all` | 07:06:04 | 07:06:05 | 1.04 / 1.08 / 0.94 | 0 s | none | 0 of 3 samples | 0 |
| `s3-c1-n1000-none` | 07:06:13 | 07:06:16 | 0.88 / 1.04 / 0.93 | 0 s | none | 0 of 4 samples | 0 |
| `s3-c1-n1000-all` | 07:06:23 | 07:06:27 | 0.81 / 1.03 / 0.93 | 0 s | none | 0 of 4 samples | 0 |
| `s3-c1-n10000-none` | 07:06:34 | 07:06:59 | 0.76 / 1.01 / 0.92 | 0 s | none | 0 of 13 samples | 0 |
| `s3-c1-n10000-all` | 07:07:06 | 07:07:32 | 0.78 / 0.99 / 0.92 | 0 s | none | 0 of 14 samples | 0 |
| `s3-c8-n200-none` | 07:07:39 | 07:07:40 | 0.94 / 1.01 / 0.93 | 0 s | none | 0 of 2 samples | 0 |
| `s3-c8-n200-all` | 07:07:47 | 07:07:49 | 0.87 / 0.99 / 0.92 | 0 s | none | 0 of 4 samples | 0 |
| `s3-c8-n1000-none` | 07:07:56 | 07:08:00 | 0.73 / 0.96 / 0.91 | 0 s | none | 0 of 4 samples | 0 |
| `s3-c8-n1000-all` | 07:08:07 | 07:08:10 | 0.62 / 0.92 / 0.90 | 0 s | none | 0 of 4 samples | 0 |
| `s3-c8-n10000-none` | 07:08:17 | 07:08:43 | 0.53 / 0.89 / 0.89 | 0 s | none | 0 of 14 samples | 0 |
| `s3-c8-n10000-all` | 07:08:51 | 07:09:16 | 0.63 / 0.89 / 0.89 | 0 s | none | 0 of 14 samples | 0 |
| `r2-s3-c1-n200-none` | 07:09:23 | 07:09:25 | 0.76 / 0.90 / 0.90 | 0 s | none | 0 of 4 samples | 0 |
| `r2-s3-c1-n200-all` | 07:09:32 | 07:09:34 | 0.80 / 0.90 / 0.90 | 0 s | none | 0 of 3 samples | 0 |
| `r2-s3-c1-n1000-none` | 07:09:41 | 07:09:44 | 0.82 / 0.91 / 0.90 | 0 s | none | 0 of 3 samples | 0 |
| `r2-s3-c1-n1000-all` | 07:09:51 | 07:09:55 | 0.77 / 0.89 / 0.90 | 0 s | none | 0 of 4 samples | 0 |
| `r2-s3-c1-n10000-none` | 07:10:02 | 07:10:27 | 0.65 / 0.86 / 0.89 | 0 s | none | 0 of 13 samples | 0 |
| `r2-s3-c1-n10000-all` | 07:10:34 | 07:10:59 | 0.71 / 0.86 / 0.89 | 0 s | none | 0 of 14 samples | 0 |
| `r2-s3-c8-n200-none` | 07:11:06 | 07:11:08 | 0.68 / 0.84 / 0.88 | 0 s | none | 0 of 3 samples | 0 |
| `r2-s3-c8-n200-all` | 07:11:15 | 07:11:17 | 0.63 / 0.83 / 0.88 | 0 s | none | 0 of 3 samples | 0 |
| `r2-s3-c8-n1000-none` | 07:11:24 | 07:11:27 | 0.69 / 0.84 / 0.88 | 0 s | none | 0 of 3 samples | 0 |
| `r2-s3-c8-n1000-all` | 07:11:35 | 07:11:38 | 0.66 / 0.82 / 0.88 | 0 s | none | 0 of 3 samples | 0 |
| `r2-s3-c8-n10000-none` | 07:11:45 | 07:12:11 | 0.63 / 0.81 / 0.87 | 0 s | none | 0 of 14 samples | 0 |
| `r2-s3-c8-n10000-all` | 07:12:19 | 07:12:44 | 0.71 / 0.82 / 0.87 | 0 s | none | 0 of 14 samples | 0 |
| `r3-s3-c1-n200-none` | 07:12:51 | 07:12:53 | 0.69 / 0.81 / 0.87 | 0 s | none | 0 of 3 samples | 0 |
| `r3-s3-c1-n200-all` | 07:13:00 | 07:13:02 | 0.63 / 0.79 / 0.86 | 0 s | none | 1 of 3 samples | 0 |
| `r3-s3-c1-n1000-none` | 07:13:09 | 07:13:12 | 0.61 / 0.78 / 0.86 | 0 s | none | 0 of 4 samples | 0 |
| `r3-s3-c1-n1000-all` | 07:13:19 | 07:13:23 | 0.67 / 0.79 / 0.86 | 0 s | none | 0 of 4 samples | 0 |
| `r3-s3-c1-n10000-none` | 07:13:30 | 07:13:55 | 0.71 / 0.80 / 0.86 | 0 s | none | 1 of 14 samples | 0 |
| `r3-s3-c1-n10000-all` | 07:14:02 | 07:14:27 | 0.69 / 0.79 / 0.86 | 0 s | none | 0 of 13 samples | 0 |
| `r3-s3-c8-n200-none` | 07:14:34 | 07:14:36 | 0.73 / 0.79 / 0.86 | 0 s | none | 0 of 3 samples | 0 |
| `r3-s3-c8-n200-all` | 07:14:43 | 07:14:45 | 0.69 / 0.78 / 0.86 | 0 s | none | 0 of 3 samples | 0 |
| `r3-s3-c8-n1000-none` | 07:14:52 | 07:14:55 | 0.59 / 0.76 / 0.85 | 0 s | none | 0 of 4 samples | 0 |
| `r3-s3-c8-n1000-all` | 07:15:02 | 07:15:06 | 0.49 / 0.73 / 0.84 | 0 s | none | 0 of 4 samples | 0 |
| `r3-s3-c8-n10000-none` | 07:15:13 | 07:15:39 | 0.49 / 0.72 / 0.83 | 0 s | none | 0 of 14 samples | 0 |
| `r3-s3-c8-n10000-all` | 07:15:46 | 07:16:12 | 0.72 / 0.77 / 0.84 | 0 s | none | 0 of 14 samples | 0 |
| `ab-old1-s3-c1-n10000-none` | 07:30:41 | 07:31:07 | 1.00 / 0.73 / 0.69 | 0 s | none | 1 of 14 samples | 0 |
| `ab-new1-s3-c1-n10000-none` | 07:36:54 | 07:37:20 | 0.72 / 0.51 / 0.59 | 0 s | none | 1 of 14 samples | 0 |
| `ab-old2-s3-c1-n10000-none` | 07:43:19 | 07:43:46 | 0.91 / 0.57 / 0.58 | 0 s | none | 0 of 15 samples | 0 |
| `ab-new2-s3-c1-n10000-none` | 07:49:32 | 07:49:58 | 0.50 / 0.44 / 0.51 | 0 s | none | 0 of 14 samples | 0 |
| `ab-old3-s3-c1-n10000-none` | 07:55:56 | 07:56:21 | 0.76 / 0.55 / 0.54 | 0 s | none | 0 of 14 samples | 0 |
| `ab-new3-s3-c1-n10000-none` | 08:02:08 | 08:02:33 | 0.80 / 0.52 / 0.51 | 0 s | none | 0 of 14 samples | 0 |
| `pl-s3-planned-none-r1` | 08:07:50 | 08:08:15 | 0.25 / 0.37 / 0.45 | 0 s | none | 0 of 13 samples | 0 |
| `pl-s3-planned-none-r2` | 08:10:27 | 08:10:53 | 0.96 / 0.65 / 0.55 | 0 s | none | 0 of 14 samples | 0 |
| `pl-s3-planned-none-r3` | 08:13:04 | 08:13:29 | 0.90 / 0.78 / 0.61 | 0 s | none | 0 of 13 samples | 0 |
| `pl-s3-planned-all-r1` | 08:16:56 | 08:17:21 | 1.15 / 0.99 / 0.74 | 0 s | none | 0 of 13 samples | 0 |
| `pl-s3-written-none-r1` | 08:09:08 | 08:09:34 | 0.69 / 0.49 / 0.48 | 0 s | none | 0 of 14 samples | 0 |
| `pl-s3-written-none-r2` | 08:11:46 | 08:12:11 | 1.02 / 0.75 / 0.59 | 0 s | none | 0 of 13 samples | 0 |
| `pl-s3-written-none-r3` | 08:15:37 | 08:16:03 | 1.02 / 0.94 / 0.71 | 0 s | none | 0 of 14 samples | 0 |
| `pl-s3-written-all-r1` | 08:17:28 | 08:17:53 | 1.53 / 1.11 / 0.80 | 0 s | none | 0 of 14 samples | 0 |


## 11. What the run says about the engine

1. **A returned row costs the engine little; the Python decoder was most of what a client saw.** `books-by-genre` is one 10,000-loan scan returning about 125 rows, and its statement time falls by a third with the decoder fixed and nothing else touched.
   Scenario 3's shapes return 1 to 125 rows, so only three of its twelve move; the fixed round trip is untouched (65.7 µs for `pk-user`).
2. **An index is worth about the same factor on both engines** (4.4x to 5.6x on KDS against 3.8x to 5.1x on PostgreSQL for `loans-by-user`, `loans-by-book`, `join-loan-user` and `count-by-user` at 10,000 loans), and nothing at 200 loans on either. The structure that pays for itself in this scenario is the same one that pays in PostgreSQL.
3. **Host drift is workload-specific and sizeable**: -11 % to -16 % on integer-column scans of a 10,000-row relation between a 03:58 and a 07:06 window, none on scenario 4's varchar walk (server CPU 2.20 s against 2.22 s). A comparison across hours that does not interleave the arms cannot read a 10 % difference on a scan shape in this environment.

## 12. Archive

`bench/v3.3.0/archive/scenario3-v3.3.0-38-ga40882af/`: per-cell JSON, driver stdout, server configs, the join-order probe's ANALYZE replies (`plans/`). What the four scenarios share is in `bench/v3.3.0/archive/run-v3.3.0-38-ga40882af/`. No data file is kept.
