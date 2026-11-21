# Scenario 3 (library) at `v3.5.0`

**One line.** At the tag `v3.5.0` (`07f4552a`) the library's walks, joins and indexed reads at 10,000 loans are where `v3.4.14` left them on the same host (`loans-by-user` unindexed 3,180 qps against 3,186, indexed 12,180 against 12,346; every 10,000-loan unindexed shape -4.3 % to +2.9 %, inside its spread). The A/B against the `v3.4.14` binary reads 0.96x to 1.03x on every shape it resolves, with `count-by-user` at 1.07x the one reading over its spread. The 200- and 1,000-loan cells at one core are too noisy this window to read: spreads of 28 % to 67 %. Verification passed 36 of 36 runs.

Driver: `tools/scenario3_library.py` of the tag, unmodified, `--seed 1 --server-indexes on`, `--loans` and `--index-mode` per cell, `--assert-index-reads` for `all`. Comparator: `results-scenario3-library-v3.4.14.md` (this host).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-11-20; main cells 18:15:44 to 18:18:53 UTC; the eight-core repeats (extras) to about 18:38 UTC; A/B 18:28 to 18:36 UTC |
| Version, commit, binary, host, device, budget | stamped once in `summary-scenarios-v3.5.0.md`, section Stamp |
| Server config | the common config, `durability = group`, `cores` 1 or 8; port 15620 |
| Driver | exit 0 in every cell; `verify_problems` empty in 36 of 36 runs |

## 2. What was run

The `v3.4.14` run's cells, unchanged: 200, 1,000 and 10,000 loans, unindexed (`none`) and indexed (`all`), at `cores = 1` three times and `cores = 8` three times (two of them in the extras), and the A/B (10,000 loans unindexed, three interleaved pairs). Cut: the plan replies (as at `v3.4.14`) and PostgreSQL.

## 3. Throughput, now against the `v3.4.14` run

qps derived as 1,000,000 / mean latency, median of the runs.

#### cores = 1

| loans | index | shape | now | spread | runs | v3.4.14 | spread | delta | read |
|---|---|---|---|---|---|---|---|---|---|
| 200 | none | pk-user | 16,000 qps | 5.0 % | 3 | 16,313 qps | 3.4 % | -1.9 % | inside the spread |
| 200 | none | loans-by-user | 12,422 qps | 8.2 % | 3 | 13,089 qps | 8.3 % | -5.1 % | inside the spread |
| 200 | none | loans-by-book | 10,799 qps | 49.1 % | 3 | 13,831 qps | 7.9 % | -21.9 % | spread > 20 %, no claim |
| 200 | none | resv-by-user | 9,443 qps | 67.1 % | 3 | 15,314 qps | 8.0 % | -38.3 % | spread > 20 %, no claim |
| 200 | none | books-by-author | 12,438 qps | 48.1 % | 3 | 14,925 qps | 6.6 % | -16.7 % | spread > 20 %, no claim |
| 200 | none | books-by-genre | 11,587 qps | 39.0 % | 3 | 15,083 qps | 6.2 % | -23.2 % | spread > 20 %, no claim |
| 200 | none | loans-by-daterange | 12,195 qps | 3.0 % | 3 | 12,180 qps | 1.2 % | +0.1 % | inside the spread |
| 200 | none | overdue | 13,245 qps | 0.7 % | 3 | 8,621 qps | 64.3 % | +53.6 % | spread > 20 %, no claim |
| 200 | none | join-loan-user | 8,019 qps | 55.7 % | 3 | 6,523 qps | 87.5 % | +22.9 % | spread > 20 %, no claim |
| 200 | none | join-no-literal | 4,621 qps | 32.7 % | 3 | 4,225 qps | 39.9 % | +9.4 % | spread > 20 %, no claim |
| 200 | none | exists-correlated | 8,104 qps | 40.3 % | 3 | 8,688 qps | 39.6 % | -6.7 % | spread > 20 %, no claim |
| 200 | none | count-by-user | 13,405 qps | 41.0 % | 3 | 17,301 qps | 30.1 % | -22.5 % | spread > 20 %, no claim |
| 200 | all | pk-user | 15,974 qps | 2.0 % | 3 | 16,155 qps | 2.6 % | -1.1 % | inside the spread |
| 200 | all | loans-by-user | 12,594 qps | 5.5 % | 3 | 12,937 qps | 4.2 % | -2.6 % | inside the spread |
| 200 | all | loans-by-book | 13,298 qps | 6.7 % | 3 | 13,423 qps | 6.6 % | -0.9 % | inside the spread |
| 200 | all | resv-by-user | 14,388 qps | 32.2 % | 3 | 15,221 qps | 4.7 % | -5.5 % | spread > 20 %, no claim |
| 200 | all | books-by-author | 13,928 qps | 7.3 % | 3 | 14,065 qps | 10.6 % | -1.0 % | inside the spread |
| 200 | all | books-by-genre | 14,749 qps | 3.2 % | 3 | 15,152 qps | 9.8 % | -2.7 % | inside the spread |
| 200 | all | loans-by-daterange | 12,019 qps | 2.2 % | 3 | 11,834 qps | 7.0 % | +1.6 % | inside the spread |
| 200 | all | overdue | 13,123 qps | 0.9 % | 3 | 13,316 qps | 10.1 % | -1.4 % | inside the spread |
| 200 | all | join-loan-user | 11,351 qps | 4.5 % | 3 | 11,933 qps | 3.4 % | -4.9 % | **finding** |
| 200 | all | join-no-literal | 4,367 qps | 1.7 % | 3 | 4,484 qps | 2.8 % | -2.6 % | inside the spread |
| 200 | all | exists-correlated | 7,342 qps | 8.3 % | 3 | 7,831 qps | 2.4 % | -6.2 % | inside the spread |
| 200 | all | count-by-user | 16,807 qps | 7.0 % | 3 | 17,544 qps | 3.1 % | -4.2 % | inside the spread |
| 1,000 | none | pk-user | 15,504 qps | 19.3 % | 3 | 16,207 qps | 5.6 % | -4.3 % | inside the spread |
| 1,000 | none | loans-by-user | 10,194 qps | 18.7 % | 3 | 10,593 qps | 3.3 % | -3.8 % | inside the spread |
| 1,000 | none | loans-by-book | 10,965 qps | 2.4 % | 3 | 11,111 qps | 1.1 % | -1.3 % | inside the spread |
| 1,000 | none | resv-by-user | 13,369 qps | 46.4 % | 3 | 13,495 qps | 1.9 % | -0.9 % | spread > 20 %, no claim |
| 1,000 | none | books-by-author | 12,658 qps | 49.1 % | 3 | 13,986 qps | 1.2 % | -9.5 % | spread > 20 %, no claim |
| 1,000 | none | books-by-genre | 11,274 qps | 43.6 % | 3 | 11,507 qps | 1.5 % | -2.0 % | spread > 20 %, no claim |
| 1,000 | none | loans-by-daterange | 5,851 qps | 31.7 % | 3 | 6,274 qps | 0.6 % | -6.7 % | spread > 20 %, no claim |
| 1,000 | none | overdue | 6,978 qps | 33.8 % | 3 | 6,954 qps | 8.6 % | +0.3 % | spread > 20 %, no claim |
| 1,000 | none | join-loan-user | 9,355 qps | 44.3 % | 3 | 9,542 qps | 27.7 % | -2.0 % | spread > 20 %, no claim |
| 1,000 | none | join-no-literal | 4,000 qps | 29.3 % | 3 | 4,417 qps | 2.3 % | -9.4 % | spread > 20 %, no claim |
| 1,000 | none | exists-correlated | 5,688 qps | 28.0 % | 3 | 6,297 qps | 8.1 % | -9.7 % | spread > 20 %, no claim |
| 1,000 | none | count-by-user | 10,111 qps | 41.4 % | 3 | 13,569 qps | 4.2 % | -25.5 % | spread > 20 %, no claim |
| 1,000 | all | pk-user | 16,026 qps | 2.1 % | 3 | 15,625 qps | 7.4 % | +2.6 % | inside the spread |
| 1,000 | all | loans-by-user | 12,739 qps | 8.6 % | 3 | 12,690 qps | 11.9 % | +0.4 % | inside the spread |
| 1,000 | all | loans-by-book | 13,441 qps | 2.1 % | 3 | 13,423 qps | 1.8 % | +0.1 % | inside the spread |
| 1,000 | all | resv-by-user | 14,286 qps | 2.6 % | 3 | 14,556 qps | 4.2 % | -1.9 % | inside the spread |
| 1,000 | all | books-by-author | 13,966 qps | 7.2 % | 3 | 14,184 qps | 11.3 % | -1.5 % | inside the spread |
| 1,000 | all | books-by-genre | 10,905 qps | 2.1 % | 3 | 10,204 qps | 25.5 % | +6.9 % | spread > 20 %, no claim |
| 1,000 | all | loans-by-daterange | 6,124 qps | 0.2 % | 3 | 6,050 qps | 2.4 % | +1.2 % | inside the spread |
| 1,000 | all | overdue | 7,519 qps | 23.4 % | 3 | 7,728 qps | 18.2 % | -2.7 % | spread > 20 %, no claim |
| 1,000 | all | join-loan-user | 11,574 qps | 15.0 % | 3 | 11,905 qps | 2.1 % | -2.8 % | inside the spread |
| 1,000 | all | join-no-literal | 4,284 qps | 2.0 % | 3 | 4,355 qps | 8.8 % | -1.6 % | inside the spread |
| 1,000 | all | exists-correlated | 7,474 qps | 5.5 % | 3 | 7,616 qps | 1.5 % | -1.9 % | inside the spread |
| 1,000 | all | count-by-user | 17,301 qps | 1.0 % | 3 | 17,361 qps | 1.4 % | -0.3 % | inside the spread |
| 10,000 | none | pk-user | 15,152 qps | 11.2 % | 3 | 16,129 qps | 3.8 % | -6.1 % | inside the spread |
| 10,000 | none | loans-by-user | 3,180 qps | 4.8 % | 3 | 3,186 qps | 6.3 % | -0.2 % | inside the spread |
| 10,000 | none | loans-by-book | 3,204 qps | 4.1 % | 3 | 3,278 qps | 2.4 % | -2.2 % | inside the spread |
| 10,000 | none | resv-by-user | 5,831 qps | 7.7 % | 3 | 5,872 qps | 4.1 % | -0.7 % | inside the spread |
| 10,000 | none | books-by-author | 8,628 qps | 2.9 % | 3 | 8,873 qps | 6.2 % | -2.8 % | inside the spread |
| 10,000 | none | books-by-genre | 2,821 qps | 5.6 % | 3 | 2,742 qps | 3.2 % | +2.9 % | inside the spread |
| 10,000 | none | loans-by-daterange | 907 qps | 3.0 % | 3 | 900 qps | 1.9 % | +0.8 % | inside the spread |
| 10,000 | none | overdue | 1,121 qps | 3.6 % | 3 | 1,126 qps | 2.6 % | -0.5 % | inside the spread |
| 10,000 | none | join-loan-user | 3,040 qps | 10.9 % | 3 | 3,145 qps | 3.7 % | -3.3 % | inside the spread |
| 10,000 | none | join-no-literal | 1,954 qps | 4.0 % | 3 | 1,934 qps | 3.3 % | +1.0 % | inside the spread |
| 10,000 | none | exists-correlated | 1,520 qps | 2.9 % | 3 | 1,588 qps | 6.2 % | -4.3 % | inside the spread |
| 10,000 | none | count-by-user | 3,362 qps | 4.5 % | 3 | 3,470 qps | 2.4 % | -3.1 % | inside the spread |
| 10,000 | all | pk-user | 14,065 qps | 4.3 % | 3 | 15,848 qps | 6.4 % | -11.3 % | **finding** |
| 10,000 | all | loans-by-user | 12,180 qps | 8.0 % | 3 | 12,346 qps | 1.4 % | -1.3 % | inside the spread |
| 10,000 | all | loans-by-book | 13,193 qps | 2.0 % | 3 | 13,175 qps | 4.8 % | +0.1 % | inside the spread |
| 10,000 | all | resv-by-user | 14,006 qps | 6.9 % | 3 | 14,327 qps | 38.1 % | -2.2 % | spread > 20 %, no claim |
| 10,000 | all | books-by-author | 13,459 qps | 6.7 % | 3 | 13,477 qps | 3.8 % | -0.1 % | inside the spread |
| 10,000 | all | books-by-genre | 2,616 qps | 2.1 % | 3 | 2,662 qps | 1.9 % | -1.7 % | inside the spread |
| 10,000 | all | loans-by-daterange | 881 qps | 0.6 % | 3 | 891 qps | 1.9 % | -1.1 % | inside the spread |
| 10,000 | all | overdue | 1,308 qps | 5.0 % | 3 | 1,329 qps | 3.0 % | -1.6 % | inside the spread |
| 10,000 | all | join-loan-user | 11,223 qps | 5.6 % | 3 | 11,390 qps | 2.3 % | -1.5 % | inside the spread |
| 10,000 | all | join-no-literal | 4,401 qps | 2.4 % | 3 | 4,301 qps | 8.5 % | +2.3 % | inside the spread |
| 10,000 | all | exists-correlated | 7,541 qps | 1.5 % | 3 | 7,541 qps | 1.4 % | +0.0 % | inside the spread |
| 10,000 | all | count-by-user | 16,207 qps | 9.8 % | 3 | 17,123 qps | 0.5 % | -5.3 % | inside the spread |

#### cores = 8

| loans | index | shape | now | spread | runs | v3.4.14 | spread | delta | read |
|---|---|---|---|---|---|---|---|---|---|
| 200 | none | pk-user | 14,903 qps | 29.8 % | 3 | 15,432 qps | 6.1 % | -3.4 % | spread > 20 %, no claim |
| 200 | none | loans-by-user | 11,377 qps | 8.0 % | 3 | 11,601 qps | 2.6 % | -1.9 % | inside the spread |
| 200 | none | loans-by-book | 12,392 qps | 3.7 % | 3 | 12,300 qps | 6.8 % | +0.7 % | inside the spread |
| 200 | none | resv-by-user | 14,347 qps | 5.6 % | 3 | 14,205 qps | 6.4 % | +1.0 % | inside the spread |
| 200 | none | books-by-author | 13,966 qps | 13.6 % | 3 | 13,966 qps | 3.3 % | +0.0 % | inside the spread |
| 200 | none | books-by-genre | 14,599 qps | 8.1 % | 3 | 14,245 qps | 5.2 % | +2.5 % | inside the spread |
| 200 | none | loans-by-daterange | 10,684 qps | 8.9 % | 3 | 11,086 qps | 2.7 % | -3.6 % | inside the spread |
| 200 | none | overdue | 12,034 qps | 11.9 % | 3 | 11,990 qps | 5.9 % | +0.4 % | inside the spread |
| 200 | none | join-loan-user | 10,331 qps | 8.0 % | 3 | 10,537 qps | 4.0 % | -2.0 % | inside the spread |
| 200 | none | join-no-literal | 4,346 qps | 1.8 % | 3 | 4,464 qps | 5.6 % | -2.7 % | inside the spread |
| 200 | none | exists-correlated | 7,669 qps | 1.2 % | 3 | 7,710 qps | 11.8 % | -0.5 % | inside the spread |
| 200 | none | count-by-user | 15,408 qps | 3.0 % | 3 | 15,480 qps | 20.1 % | -0.5 % | spread > 20 %, no claim |
| 200 | all | pk-user | 14,514 qps | 8.5 % | 3 | 15,106 qps | 5.7 % | -3.9 % | inside the spread |
| 200 | all | loans-by-user | 11,614 qps | 16.9 % | 3 | 12,500 qps | 1.6 % | -7.1 % | inside the spread |
| 200 | all | loans-by-book | 12,804 qps | 3.6 % | 3 | 12,285 qps | 30.7 % | +4.2 % | spread > 20 %, no claim |
| 200 | all | resv-by-user | 14,493 qps | 4.8 % | 3 | 14,347 qps | 14.1 % | +1.0 % | inside the spread |
| 200 | all | books-by-author | 13,831 qps | 3.5 % | 3 | 13,351 qps | 12.1 % | +3.6 % | inside the spread |
| 200 | all | books-by-genre | 14,430 qps | 4.4 % | 3 | 12,438 qps | 19.2 % | +16.0 % | inside the spread |
| 200 | all | loans-by-daterange | 11,111 qps | 7.6 % | 3 | 11,223 qps | 7.7 % | -1.0 % | inside the spread |
| 200 | all | overdue | 12,453 qps | 8.8 % | 3 | 12,531 qps | 30.8 % | -0.6 % | spread > 20 %, no claim |
| 200 | all | join-loan-user | 11,123 qps | 6.2 % | 3 | 10,905 qps | 43.3 % | +2.0 % | spread > 20 %, no claim |
| 200 | all | join-no-literal | 3,774 qps | 4.9 % | 3 | 3,752 qps | 7.7 % | +0.6 % | inside the spread |
| 200 | all | exists-correlated | 6,583 qps | 0.9 % | 3 | 6,579 qps | 25.2 % | +0.1 % | spread > 20 %, no claim |
| 200 | all | count-by-user | 16,260 qps | 2.7 % | 3 | 16,181 qps | 17.9 % | +0.5 % | inside the spread |
| 1,000 | none | pk-user | 14,535 qps | 24.4 % | 3 | 14,749 qps | 11.0 % | -1.5 % | spread > 20 %, no claim |
| 1,000 | none | loans-by-user | 8,084 qps | 15.8 % | 3 | 7,862 qps | 18.1 % | +2.8 % | inside the spread |
| 1,000 | none | loans-by-book | 7,225 qps | 25.0 % | 3 | 7,849 qps | 17.4 % | -7.9 % | spread > 20 %, no claim |
| 1,000 | none | resv-by-user | 8,271 qps | 46.3 % | 3 | 10,616 qps | 22.1 % | -22.1 % | spread > 20 %, no claim |
| 1,000 | none | books-by-author | 12,422 qps | 41.3 % | 3 | 12,315 qps | 5.6 % | +0.9 % | spread > 20 %, no claim |
| 1,000 | none | books-by-genre | 10,050 qps | 23.2 % | 3 | 10,183 qps | 11.6 % | -1.3 % | spread > 20 %, no claim |
| 1,000 | none | loans-by-daterange | 5,084 qps | 9.5 % | 3 | 5,230 qps | 4.3 % | -2.8 % | inside the spread |
| 1,000 | none | overdue | 5,896 qps | 8.3 % | 3 | 5,889 qps | 3.0 % | +0.1 % | inside the spread |
| 1,000 | none | join-loan-user | 7,530 qps | 8.2 % | 3 | 7,513 qps | 6.7 % | +0.2 % | inside the spread |
| 1,000 | none | join-no-literal | 3,500 qps | 3.5 % | 3 | 3,493 qps | 5.4 % | +0.2 % | inside the spread |
| 1,000 | none | exists-correlated | 5,074 qps | 8.3 % | 3 | 5,373 qps | 3.2 % | -5.6 % | inside the spread |
| 1,000 | none | count-by-user | 9,615 qps | 6.9 % | 3 | 9,681 qps | 1.9 % | -0.7 % | inside the spread |
| 1,000 | all | pk-user | 14,881 qps | 28.4 % | 3 | 15,480 qps | 2.0 % | -3.9 % | spread > 20 %, no claim |
| 1,000 | all | loans-by-user | 11,737 qps | 23.7 % | 3 | 12,121 qps | 3.2 % | -3.2 % | spread > 20 %, no claim |
| 1,000 | all | loans-by-book | 11,547 qps | 35.6 % | 3 | 12,706 qps | 2.5 % | -9.1 % | spread > 20 %, no claim |
| 1,000 | all | resv-by-user | 12,723 qps | 38.7 % | 3 | 13,774 qps | 3.5 % | -7.6 % | spread > 20 %, no claim |
| 1,000 | all | books-by-author | 12,500 qps | 44.2 % | 3 | 13,387 qps | 5.1 % | -6.6 % | spread > 20 %, no claim |
| 1,000 | all | books-by-genre | 9,643 qps | 42.7 % | 3 | 10,277 qps | 6.7 % | -6.2 % | spread > 20 %, no claim |
| 1,000 | all | loans-by-daterange | 5,136 qps | 27.0 % | 3 | 5,097 qps | 6.5 % | +0.8 % | spread > 20 %, no claim |
| 1,000 | all | overdue | 6,720 qps | 35.3 % | 3 | 7,321 qps | 1.5 % | -8.2 % | spread > 20 %, no claim |
| 1,000 | all | join-loan-user | 10,225 qps | 48.2 % | 3 | 11,148 qps | 4.0 % | -8.3 % | spread > 20 %, no claim |
| 1,000 | all | join-no-literal | 3,428 qps | 18.2 % | 3 | 3,682 qps | 2.4 % | -6.9 % | inside the spread |
| 1,000 | all | exists-correlated | 6,219 qps | 10.2 % | 3 | 6,489 qps | 1.4 % | -4.2 % | inside the spread |
| 1,000 | all | count-by-user | 15,924 qps | 2.4 % | 3 | 16,026 qps | 2.3 % | -0.6 % | inside the spread |
| 10,000 | none | pk-user | 14,577 qps | 0.6 % | 3 | 14,970 qps | 5.2 % | -2.6 % | inside the spread |
| 10,000 | none | loans-by-user | 2,453 qps | 6.4 % | 3 | 2,509 qps | 6.1 % | -2.3 % | inside the spread |
| 10,000 | none | loans-by-book | 2,474 qps | 0.9 % | 3 | 2,528 qps | 5.0 % | -2.2 % | inside the spread |
| 10,000 | none | resv-by-user | 5,376 qps | 4.5 % | 3 | 5,482 qps | 5.1 % | -1.9 % | inside the spread |
| 10,000 | none | books-by-author | 7,728 qps | 2.2 % | 3 | 8,097 qps | 4.8 % | -4.6 % | inside the spread |
| 10,000 | none | books-by-genre | 2,663 qps | 7.5 % | 3 | 2,760 qps | 2.1 % | -3.5 % | inside the spread |
| 10,000 | none | loans-by-daterange | 813 qps | 1.6 % | 3 | 821 qps | 1.5 % | -1.0 % | inside the spread |
| 10,000 | none | overdue | 995 qps | 3.2 % | 3 | 1,012 qps | 1.4 % | -1.7 % | inside the spread |
| 10,000 | none | join-loan-user | 2,380 qps | 0.1 % | 3 | 2,362 qps | 9.4 % | +0.8 % | inside the spread |
| 10,000 | none | join-no-literal | 1,601 qps | 7.3 % | 3 | 1,593 qps | 3.2 % | +0.5 % | inside the spread |
| 10,000 | none | exists-correlated | 1,421 qps | 4.4 % | 3 | 1,489 qps | 3.2 % | -4.6 % | **finding** |
| 10,000 | none | count-by-user | 2,637 qps | 1.6 % | 3 | 2,621 qps | 3.5 % | +0.6 % | inside the spread |
| 10,000 | all | pk-user | 11,364 qps | 34.5 % | 3 | 15,175 qps | 9.0 % | -25.1 % | spread > 20 %, no claim |
| 10,000 | all | loans-by-user | 7,241 qps | 73.1 % | 3 | 11,338 qps | 43.4 % | -36.1 % | spread > 20 %, no claim |
| 10,000 | all | loans-by-book | 9,099 qps | 40.0 % | 3 | 12,048 qps | 5.1 % | -24.5 % | spread > 20 %, no claim |
| 10,000 | all | resv-by-user | 9,950 qps | 38.6 % | 3 | 13,495 qps | 4.4 % | -26.3 % | spread > 20 %, no claim |
| 10,000 | all | books-by-author | 9,814 qps | 25.0 % | 3 | 12,422 qps | 6.8 % | -21.0 % | spread > 20 %, no claim |
| 10,000 | all | books-by-genre | 2,362 qps | 2.1 % | 3 | 2,408 qps | 3.2 % | -1.9 % | inside the spread |
| 10,000 | all | loans-by-daterange | 815 qps | 2.1 % | 3 | 824 qps | 0.5 % | -1.1 % | inside the spread |
| 10,000 | all | overdue | 1,189 qps | 3.0 % | 3 | 1,199 qps | 4.0 % | -0.8 % | inside the spread |
| 10,000 | all | join-loan-user | 8,562 qps | 46.0 % | 3 | 9,390 qps | 24.6 % | -8.8 % | spread > 20 %, no claim |
| 10,000 | all | join-no-literal | 3,442 qps | 17.6 % | 3 | 3,727 qps | 17.6 % | -7.6 % | inside the spread |
| 10,000 | all | exists-correlated | 6,402 qps | 10.7 % | 3 | 6,042 qps | 21.0 % | +6.0 % | spread > 20 %, no claim |
| 10,000 | all | count-by-user | 15,898 qps | 23.0 % | 3 | 15,267 qps | 5.0 % | +4.1 % | spread > 20 %, no claim |

**The small cells.** At 200 and 1,000 loans unindexed one of the three runs is 30 % to 50 % slower on most shapes, a different run in each cell (`r3-` at 1,000 loans, runs 1 and 2 at 200), with no foreign process seen; these shapes take 60 to 150 µs and are bound by the round trip, and no claim is made on them. Two cells cross their spread: `join-loan-user` at 200 loans indexed (-4.9 % against 4.5 %) and `pk-user` at 10,000 loans indexed (-11.3 % against 6.4 %). The first is at the floor. The second was the cell live while an `apt-get` process took 63 % of one CPU (18:16:24 UTC; see section 7), and `pk-user` unindexed and in the A/B does not move, so neither is read as an engine change.

## 4. Percentiles, run 1 of `s3-c1-n10000-none`

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 4 | 676.8 µs | 676.8 µs | 690.8 µs | 905.3 µs | 905.3 µs | 905.3 µs | 0 |
| load | 19,000 | 270.2 µs | 407.3 µs | 461.4 µs | 649.9 µs | 774.7 µs | 2,987.5 µs | 0 |
| pk-user | 200 | 56.9 µs | 59.8 µs | 68.4 µs | 88.9 µs | 203.6 µs | 503.4 µs | 0 |
| loans-by-user | 200 | 281.9 µs | 299.3 µs | 311.2 µs | 392.1 µs | 417.2 µs | 458.9 µs | 0 |
| loans-by-book | 200 | 275.2 µs | 290.0 µs | 298.5 µs | 382.2 µs | 404.3 µs | 409.1 µs | 0 |
| resv-by-user | 200 | 153.8 µs | 163.5 µs | 166.9 µs | 186.4 µs | 204.7 µs | 245.7 µs | 0 |
| books-by-author | 200 | 100.5 µs | 108.5 µs | 112.1 µs | 144.9 µs | 184.6 µs | 193.2 µs | 0 |
| books-by-genre | 200 | 292.0 µs | 314.6 µs | 330.8 µs | 444.8 µs | 490.2 µs | 539.9 µs | 0 |
| loans-by-daterange | 200 | 429.9 µs | 1,066.1 µs | 1,101.4 µs | 1,240.3 µs | 1,343.6 µs | 1,375.2 µs | 0 |
| overdue | 200 | 401.0 µs | 883.2 µs | 912.3 µs | 985.8 µs | 1,032.5 µs | 1,079.3 µs | 0 |
| join-loan-user | 200 | 285.0 µs | 321.7 µs | 355.6 µs | 419.5 µs | 536.7 µs | 716.1 µs | 0 |
| join-no-literal | 200 | 459.8 µs | 471.6 µs | 489.1 µs | 591.7 µs | 688.2 µs | 765.9 µs | 0 |
| exists-correlated | 200 | 589.3 µs | 622.1 µs | 644.4 µs | 752.2 µs | 837.1 µs | 891.0 µs | 0 |
| count-by-user | 200 | 261.2 µs | 271.3 µs | 286.4 µs | 363.4 µs | 415.0 µs | 476.5 µs | 0 |

## 5. The engine, isolated: `v3.4.14` against `v3.5.0`, interleaved on this host

| measure | v3.4.14 engine | spread | v3.5.0 engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s3 pk-user (10,000 loans, unindexed) | 15,105.7 qps | 27.0 % | 14,836.8 qps | 35.7 % | 0.98x | spread > 20 %, no claim |
| s3 loans-by-user (10,000 loans, unindexed) | 2,884.3 qps | 8.4 % | 2,908.7 qps | 13.0 % | 1.01x | inside the spread |
| s3 loans-by-book (10,000 loans, unindexed) | 2,963.0 qps | 12.4 % | 2,973.5 qps | 9.8 % | 1.00x | inside the spread |
| s3 resv-by-user (10,000 loans, unindexed) | 4,299.2 qps | 39.0 % | 5,274.3 qps | 21.4 % | 1.23x | spread > 20 %, no claim |
| s3 books-by-author (10,000 loans, unindexed) | 7,429.4 qps | 27.8 % | 8,084.1 qps | 28.8 % | 1.09x | spread > 20 %, no claim |
| s3 books-by-genre (10,000 loans, unindexed) | 2,767.0 qps | 9.7 % | 2,657.5 qps | 3.5 % | 0.96x | inside the spread |
| s3 loans-by-daterange (10,000 loans, unindexed) | 892.4 qps | 3.6 % | 893.6 qps | 0.6 % | 1.00x | inside the spread |
| s3 overdue (10,000 loans, unindexed) | 1,089.8 qps | 5.6 % | 1,111.2 qps | 0.4 % | 1.02x | inside the spread |
| s3 join-loan-user (10,000 loans, unindexed) | 2,853.1 qps | 11.4 % | 2,773.9 qps | 6.2 % | 0.97x | inside the spread |
| s3 join-no-literal (10,000 loans, unindexed) | 1,968.5 qps | 0.6 % | 1,904.8 qps | 10.6 % | 0.97x | inside the spread |
| s3 exists-correlated (10,000 loans, unindexed) | 1,511.3 qps | 1.9 % | 1,563.7 qps | 5.5 % | 1.03x | inside the spread |
| s3 count-by-user (10,000 loans, unindexed) | 3,207.2 qps | 3.4 % | 3,416.5 qps | 4.7 % | 1.07x | **finding** |

`count-by-user` reads 1.07x, over its spread (3.4 % and 4.7 %); the cross-run reading at 10,000 loans does not confirm it, and it is not attributed.

## 6. What the run says about the engine

- **No change between `v3.4.14` and `v3.5.0` on scenario 3's 10,000-loan shapes** at one or eight cores, indexed or not.
- **The small cells are unresolved this window**: their spreads exceed 20 % and they say nothing either way.

## 7. Noise floor, contamination and host

Every quiet-gate wait was 0 s. **`s3-c1-n10000-all` (run 1) carries a foreign sample above 50 %**: an `apt-get --just-print` process at 63.0 % of one CPU at 18:16:24 UTC (the host's own package check, not a session's). The cell is kept and disclosed: its `pk-user` reads 14,065 qps against the other two runs' 14,556 and 13,947.

## 8. Archive

`bench/v3.5.0/archive/scenario3-v3.5.0/` (per-cell JSON, driver stdout, prechecks, gate waits, server configs and logs) and `bench/v3.5.0/archive/run-v3.5.0/` (the run scripts, watchdogs, monitor log, timeline, binary hashes, the table generator `gen.py` and its output under `tables/`). No data file is kept.
