# Scenario 3 (library) at `v3.4.17-159-g9f8f8b354`

**One line.** On scenario 3 the engine at `9f8f8b354` is the engine of `07f4552a` (`v3.5.0`): in the pinned same-host A/B (five interleaved pairs, 10,000 loans) the twelve read shapes read 0.97x to 1.03x unindexed and 0.96x to 1.01x indexed, 22 of the 24 inside their spreads, one over 20 % (`resv-by-user` indexed, 1.00x) and one marked by the rule at the floor: `count-by-user` unindexed, -1.1 % against spreads of 1.0 % and 0.6 %. The unpinned A/B reads 0.58x to 0.80x on the indexed cell's twelve read shapes, which the pinned series does not confirm (section 5). The cells read 1.4x to 3.8x the `v3.5.0` run's, which is the host. Verification passed in every run and `--assert-index-reads` in every indexed cell.

Driver: `tools/scenario3_library.py` of the measured commit, unmodified (byte-identical to `07f4552a`'s), `--seed 1 --server-indexes on`, `--loans` and `--index-mode` per cell, `--assert-index-reads` for `all`. Comparator run: `results-scenario3-library-v3.5.0.md` (2026-11-20, `07f4552a`, **another host**), so a ratio against it is host plus engine. The engine reading is the interleaved A/B on this host (sections 5 and 6).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-11-26; main cells 21:11:57 to 21:15:08 UTC; unpinned A/B cells inside 21:19:44 to 21:41:59 UTC; pinned A/B cells inside 21:52:11 to 22:07:44 UTC |
| Version, commit, binary, host, device, budget | stamped once in `summary-scenarios-v3.4.17-159-g9f8f8b354.md`, section Stamp |
| Server config | the common config, `durability = group`, `cores` 1 or 8; port 15720 |
| Driver | exit 0 in every cell, `--assert-index-reads` included (every indexed shape compiled to an index step); `verify_problems` empty in 68 of 68 (main and A/B) |

## 2. What was run

The `v3.5.0` run's cells, unchanged in arguments and suffixes: 200, 1,000 and 10,000 loans, unindexed (`none`) and indexed (`all`), at `cores = 1` three times and `cores = 8` three times, and the A/B (10,000 loans unindexed, three interleaved pairs). **Added**: an indexed A/B cell (10,000 loans, `all`), and the whole A/B again with the server pinned to the P-cores (five pairs). Cut: the plan replies (as at `v3.5.0`) and PostgreSQL.

**A cell here is short.** A shape is 200 statements, 7 ms to 124 ms of work on this host at 10,000 loans unindexed, so one run's mean is set by where the server's thread ran for those milliseconds.

## 3. Throughput, now against the `v3.5.0` run

qps derived as 1,000,000 / mean latency, median of the runs. **The last column is this host over another host**: no cell of it is a finding about the engine.

#### cores = 1

| loans | index | shape | now | spread | runs | v3.5.0 run (other host) | spread | now over v3.5.0 run (host + engine) |
|---|---|---|---|---|---|---|---|---|
| 200 | none | pk-user | 39,216 qps | 52.6 % | 3 | 16,000 qps | 5.0 % | 2.45x |
| 200 | none | loans-by-user | 30,211 qps | 49.1 % | 3 | 12,422 qps | 8.2 % | 2.43x |
| 200 | none | loans-by-book | 31,746 qps | 51.5 % | 3 | 10,799 qps | 49.1 % | 2.94x |
| 200 | none | resv-by-user | 35,971 qps | 55.4 % | 3 | 9,443 qps | 67.1 % | 3.81x |
| 200 | none | books-by-author | 34,014 qps | 55.3 % | 3 | 12,438 qps | 48.1 % | 2.73x |
| 200 | none | books-by-genre | 35,971 qps | 57.4 % | 3 | 11,587 qps | 39.0 % | 3.10x |
| 200 | none | loans-by-daterange | 27,624 qps | 48.1 % | 3 | 12,195 qps | 3.0 % | 2.27x |
| 200 | none | overdue | 29,412 qps | 53.2 % | 3 | 13,245 qps | 0.7 % | 2.22x |
| 200 | none | join-loan-user | 25,000 qps | 57.5 % | 3 | 8,019 qps | 55.7 % | 3.12x |
| 200 | none | join-no-literal | 10,787 qps | 36.3 % | 3 | 4,621 qps | 32.7 % | 2.33x |
| 200 | none | exists-correlated | 19,841 qps | 48.6 % | 3 | 8,104 qps | 40.3 % | 2.45x |
| 200 | none | count-by-user | 43,290 qps | 52.8 % | 3 | 13,405 qps | 41.0 % | 3.23x |
| 200 | all | pk-user | 23,041 qps | 18.3 % | 3 | 15,974 qps | 2.0 % | 1.44x |
| 200 | all | loans-by-user | 18,657 qps | 17.4 % | 3 | 12,594 qps | 5.5 % | 1.48x |
| 200 | all | loans-by-book | 19,646 qps | 18.5 % | 3 | 13,298 qps | 6.7 % | 1.48x |
| 200 | all | resv-by-user | 21,505 qps | 18.9 % | 3 | 14,388 qps | 32.2 % | 1.49x |
| 200 | all | books-by-author | 20,450 qps | 18.8 % | 3 | 13,928 qps | 7.3 % | 1.47x |
| 200 | all | books-by-genre | 21,413 qps | 18.7 % | 3 | 14,749 qps | 3.2 % | 1.45x |
| 200 | all | loans-by-daterange | 18,182 qps | 15.0 % | 3 | 12,019 qps | 2.2 % | 1.51x |
| 200 | all | overdue | 19,048 qps | 16.9 % | 3 | 13,123 qps | 0.9 % | 1.45x |
| 200 | all | join-loan-user | 16,779 qps | 18.1 % | 3 | 11,351 qps | 4.5 % | 1.48x |
| 200 | all | join-no-literal | 7,143 qps | 7.6 % | 3 | 4,367 qps | 1.7 % | 1.64x |
| 200 | all | exists-correlated | 11,834 qps | 13.2 % | 3 | 7,342 qps | 8.3 % | 1.61x |
| 200 | all | count-by-user | 23,529 qps | 21.3 % | 3 | 16,807 qps | 7.0 % | 1.40x |
| 1,000 | none | pk-user | 25,773 qps | 13.3 % | 3 | 15,504 qps | 19.3 % | 1.66x |
| 1,000 | none | loans-by-user | 16,611 qps | 16.7 % | 3 | 10,194 qps | 18.7 % | 1.63x |
| 1,000 | none | loans-by-book | 16,260 qps | 18.7 % | 3 | 10,965 qps | 2.4 % | 1.48x |
| 1,000 | none | resv-by-user | 19,569 qps | 21.1 % | 3 | 13,369 qps | 46.4 % | 1.46x |
| 1,000 | none | books-by-author | 20,202 qps | 21.3 % | 3 | 12,658 qps | 49.1 % | 1.60x |
| 1,000 | none | books-by-genre | 17,422 qps | 17.8 % | 3 | 11,274 qps | 43.6 % | 1.55x |
| 1,000 | none | loans-by-daterange | 9,930 qps | 10.0 % | 3 | 5,851 qps | 31.7 % | 1.70x |
| 1,000 | none | overdue | 11,325 qps | 11.5 % | 3 | 6,978 qps | 33.8 % | 1.62x |
| 1,000 | none | join-loan-user | 14,327 qps | 17.6 % | 3 | 9,355 qps | 44.3 % | 1.53x |
| 1,000 | none | join-no-literal | 6,978 qps | 8.6 % | 3 | 4,000 qps | 29.3 % | 1.74x |
| 1,000 | none | exists-correlated | 10,111 qps | 12.5 % | 3 | 5,688 qps | 28.0 % | 1.78x |
| 1,000 | none | count-by-user | 19,048 qps | 20.3 % | 3 | 10,111 qps | 41.4 % | 1.88x |
| 1,000 | all | pk-user | 37,594 qps | 50.2 % | 3 | 16,026 qps | 2.1 % | 2.35x |
| 1,000 | all | loans-by-user | 28,329 qps | 52.4 % | 3 | 12,739 qps | 8.6 % | 2.22x |
| 1,000 | all | loans-by-book | 30,303 qps | 55.7 % | 3 | 13,441 qps | 2.1 % | 2.25x |
| 1,000 | all | resv-by-user | 33,784 qps | 58.8 % | 3 | 14,286 qps | 2.6 % | 2.36x |
| 1,000 | all | books-by-author | 31,546 qps | 58.3 % | 3 | 13,966 qps | 7.2 % | 2.26x |
| 1,000 | all | books-by-genre | 24,752 qps | 49.6 % | 3 | 10,905 qps | 2.1 % | 2.27x |
| 1,000 | all | loans-by-daterange | 13,333 qps | 30.3 % | 3 | 6,124 qps | 0.2 % | 2.18x |
| 1,000 | all | overdue | 17,241 qps | 28.8 % | 3 | 7,519 qps | 23.4 % | 2.29x |
| 1,000 | all | join-loan-user | 24,631 qps | 33.8 % | 3 | 11,574 qps | 15.0 % | 2.13x |
| 1,000 | all | join-no-literal | 9,042 qps | 25.8 % | 3 | 4,284 qps | 2.0 % | 2.11x |
| 1,000 | all | exists-correlated | 16,694 qps | 32.3 % | 3 | 7,474 qps | 5.5 % | 2.23x |
| 1,000 | all | count-by-user | 41,494 qps | 45.1 % | 3 | 17,301 qps | 1.0 % | 2.40x |
| 10,000 | none | pk-user | 26,810 qps | 69.0 % | 3 | 15,152 qps | 11.2 % | 1.77x |
| 10,000 | none | loans-by-user | 5,388 qps | 28.1 % | 3 | 3,180 qps | 4.8 % | 1.69x |
| 10,000 | none | loans-by-book | 5,491 qps | 25.2 % | 3 | 3,204 qps | 4.1 % | 1.71x |
| 10,000 | none | resv-by-user | 9,050 qps | 37.3 % | 3 | 5,831 qps | 7.7 % | 1.55x |
| 10,000 | none | books-by-author | 13,316 qps | 52.2 % | 3 | 8,628 qps | 2.9 % | 1.54x |
| 10,000 | none | books-by-genre | 5,299 qps | 31.9 % | 3 | 2,821 qps | 5.6 % | 1.88x |
| 10,000 | none | loans-by-daterange | 1,610 qps | 18.6 % | 3 | 907 qps | 3.0 % | 1.78x |
| 10,000 | none | overdue | 2,026 qps | 23.7 % | 3 | 1,121 qps | 3.6 % | 1.81x |
| 10,000 | none | join-loan-user | 5,348 qps | 27.6 % | 3 | 3,040 qps | 10.9 % | 1.76x |
| 10,000 | none | join-no-literal | 3,396 qps | 24.8 % | 3 | 1,954 qps | 4.0 % | 1.74x |
| 10,000 | none | exists-correlated | 2,945 qps | 40.0 % | 3 | 1,520 qps | 2.9 % | 1.94x |
| 10,000 | none | count-by-user | 5,770 qps | 35.3 % | 3 | 3,362 qps | 4.5 % | 1.72x |
| 10,000 | all | pk-user | 22,831 qps | 9.4 % | 3 | 14,065 qps | 4.3 % | 1.62x |
| 10,000 | all | loans-by-user | 18,083 qps | 37.3 % | 3 | 12,180 qps | 8.0 % | 1.48x |
| 10,000 | all | loans-by-book | 19,380 qps | 40.0 % | 3 | 13,193 qps | 2.0 % | 1.47x |
| 10,000 | all | resv-by-user | 20,921 qps | 42.0 % | 3 | 14,006 qps | 6.9 % | 1.49x |
| 10,000 | all | books-by-author | 19,724 qps | 40.3 % | 3 | 13,459 qps | 6.7 % | 1.47x |
| 10,000 | all | books-by-genre | 5,066 qps | 7.7 % | 3 | 2,616 qps | 2.1 % | 1.94x |
| 10,000 | all | loans-by-daterange | 1,608 qps | 23.4 % | 3 | 881 qps | 0.6 % | 1.82x |
| 10,000 | all | overdue | 2,514 qps | 25.0 % | 3 | 1,308 qps | 5.0 % | 1.92x |
| 10,000 | all | join-loan-user | 16,502 qps | 45.8 % | 3 | 11,223 qps | 5.6 % | 1.47x |
| 10,000 | all | join-no-literal | 6,978 qps | 32.4 % | 3 | 4,401 qps | 2.4 % | 1.59x |
| 10,000 | all | exists-correlated | 11,682 qps | 43.4 % | 3 | 7,541 qps | 1.5 % | 1.55x |
| 10,000 | all | count-by-user | 23,256 qps | 78.1 % | 3 | 16,207 qps | 9.8 % | 1.43x |

#### cores = 8

| loans | index | shape | now | spread | runs | v3.5.0 run (other host) | spread | now over v3.5.0 run (host + engine) |
|---|---|---|---|---|---|---|---|---|
| 200 | none | pk-user | 32,362 qps | 24.1 % | 3 | 14,903 qps | 29.8 % | 2.17x |
| 200 | none | loans-by-user | 24,814 qps | 15.5 % | 3 | 11,377 qps | 8.0 % | 2.18x |
| 200 | none | loans-by-book | 26,178 qps | 12.7 % | 3 | 12,392 qps | 3.7 % | 2.11x |
| 200 | none | resv-by-user | 30,395 qps | 14.2 % | 3 | 14,347 qps | 5.6 % | 2.12x |
| 200 | none | books-by-author | 29,240 qps | 12.9 % | 3 | 13,966 qps | 13.6 % | 2.09x |
| 200 | none | books-by-genre | 30,864 qps | 13.9 % | 3 | 14,599 qps | 8.1 % | 2.11x |
| 200 | none | loans-by-daterange | 22,779 qps | 15.8 % | 3 | 10,684 qps | 8.9 % | 2.13x |
| 200 | none | overdue | 25,575 qps | 7.4 % | 3 | 12,034 qps | 11.9 % | 2.13x |
| 200 | none | join-loan-user | 23,310 qps | 0.9 % | 3 | 10,331 qps | 8.0 % | 2.26x |
| 200 | none | join-no-literal | 10,040 qps | 5.4 % | 3 | 4,346 qps | 1.8 % | 2.31x |
| 200 | none | exists-correlated | 18,116 qps | 0.9 % | 3 | 7,669 qps | 1.2 % | 2.36x |
| 200 | none | count-by-user | 37,879 qps | 15.4 % | 3 | 15,408 qps | 3.0 % | 2.46x |
| 200 | all | pk-user | 23,419 qps | 99.9 % | 3 | 14,514 qps | 8.5 % | 1.61x |
| 200 | all | loans-by-user | 25,381 qps | 23.2 % | 3 | 11,614 qps | 16.9 % | 2.19x |
| 200 | all | loans-by-book | 28,169 qps | 23.3 % | 3 | 12,804 qps | 3.6 % | 2.20x |
| 200 | all | resv-by-user | 31,847 qps | 26.4 % | 3 | 14,493 qps | 4.8 % | 2.20x |
| 200 | all | books-by-author | 29,499 qps | 23.4 % | 3 | 13,831 qps | 3.5 % | 2.13x |
| 200 | all | books-by-genre | 31,153 qps | 25.1 % | 3 | 14,430 qps | 4.4 % | 2.16x |
| 200 | all | loans-by-daterange | 23,866 qps | 21.6 % | 3 | 11,111 qps | 7.6 % | 2.15x |
| 200 | all | overdue | 27,473 qps | 20.5 % | 3 | 12,453 qps | 8.8 % | 2.21x |
| 200 | all | join-loan-user | 24,155 qps | 19.9 % | 3 | 11,123 qps | 6.2 % | 2.17x |
| 200 | all | join-no-literal | 8,591 qps | 22.1 % | 3 | 3,774 qps | 4.9 % | 2.28x |
| 200 | all | exists-correlated | 16,051 qps | 29.4 % | 3 | 6,583 qps | 0.9 % | 2.44x |
| 200 | all | count-by-user | 35,714 qps | 49.3 % | 3 | 16,260 qps | 2.7 % | 2.20x |
| 1,000 | none | pk-user | 37,175 qps | 1.5 % | 3 | 14,535 qps | 24.4 % | 2.56x |
| 1,000 | none | loans-by-user | 18,116 qps | 7.5 % | 3 | 8,084 qps | 15.8 % | 2.24x |
| 1,000 | none | loans-by-book | 18,692 qps | 2.6 % | 3 | 7,225 qps | 25.0 % | 2.59x |
| 1,000 | none | resv-by-user | 25,575 qps | 1.5 % | 3 | 8,271 qps | 46.3 % | 3.09x |
| 1,000 | none | books-by-author | 29,155 qps | 2.3 % | 3 | 12,422 qps | 41.3 % | 2.35x |
| 1,000 | none | books-by-genre | 24,096 qps | 2.2 % | 3 | 10,050 qps | 23.2 % | 2.40x |
| 1,000 | none | loans-by-daterange | 11,820 qps | 0.7 % | 3 | 5,084 qps | 9.5 % | 2.33x |
| 1,000 | none | overdue | 13,423 qps | 0.8 % | 3 | 5,896 qps | 8.3 % | 2.28x |
| 1,000 | none | join-loan-user | 16,447 qps | 4.3 % | 3 | 7,530 qps | 8.2 % | 2.18x |
| 1,000 | none | join-no-literal | 8,163 qps | 0.6 % | 3 | 3,500 qps | 3.5 % | 2.33x |
| 1,000 | none | exists-correlated | 12,610 qps | 1.0 % | 3 | 5,074 qps | 8.3 % | 2.49x |
| 1,000 | none | count-by-user | 22,173 qps | 5.4 % | 3 | 9,615 qps | 6.9 % | 2.31x |
| 1,000 | all | pk-user | 37,313 qps | 39.8 % | 3 | 14,881 qps | 28.4 % | 2.51x |
| 1,000 | all | loans-by-user | 27,473 qps | 35.1 % | 3 | 11,737 qps | 23.7 % | 2.34x |
| 1,000 | all | loans-by-book | 29,412 qps | 36.0 % | 3 | 11,547 qps | 35.6 % | 2.55x |
| 1,000 | all | resv-by-user | 33,003 qps | 37.8 % | 3 | 12,723 qps | 38.7 % | 2.59x |
| 1,000 | all | books-by-author | 30,769 qps | 36.4 % | 3 | 12,500 qps | 44.2 % | 2.46x |
| 1,000 | all | books-by-genre | 23,923 qps | 32.6 % | 3 | 9,643 qps | 42.7 % | 2.48x |
| 1,000 | all | loans-by-daterange | 11,834 qps | 23.8 % | 3 | 5,136 qps | 27.0 % | 2.30x |
| 1,000 | all | overdue | 16,611 qps | 28.1 % | 3 | 6,720 qps | 35.3 % | 2.47x |
| 1,000 | all | join-loan-user | 23,866 qps | 32.5 % | 3 | 10,225 qps | 48.2 % | 2.33x |
| 1,000 | all | join-no-literal | 8,540 qps | 24.7 % | 3 | 3,428 qps | 18.2 % | 2.49x |
| 1,000 | all | exists-correlated | 15,748 qps | 30.7 % | 3 | 6,219 qps | 10.2 % | 2.53x |
| 1,000 | all | count-by-user | 39,841 qps | 43.6 % | 3 | 15,924 qps | 2.4 % | 2.50x |
| 10,000 | none | pk-user | 23,148 qps | 63.9 % | 3 | 14,577 qps | 0.6 % | 1.59x |
| 10,000 | none | loans-by-user | 4,695 qps | 31.4 % | 3 | 2,453 qps | 6.4 % | 1.91x |
| 10,000 | none | loans-by-book | 4,958 qps | 21.6 % | 3 | 2,474 qps | 0.9 % | 2.00x |
| 10,000 | none | resv-by-user | 9,488 qps | 34.5 % | 3 | 5,376 qps | 4.5 % | 1.76x |
| 10,000 | none | books-by-author | 14,124 qps | 37.2 % | 3 | 7,728 qps | 2.2 % | 1.83x |
| 10,000 | none | books-by-genre | 5,423 qps | 28.2 % | 3 | 2,663 qps | 7.5 % | 2.04x |
| 10,000 | none | loans-by-daterange | 1,545 qps | 28.3 % | 3 | 813 qps | 1.6 % | 1.90x |
| 10,000 | none | overdue | 1,918 qps | 23.8 % | 3 | 995 qps | 3.2 % | 1.93x |
| 10,000 | none | join-loan-user | 4,647 qps | 33.6 % | 3 | 2,380 qps | 0.1 % | 1.95x |
| 10,000 | none | join-no-literal | 3,150 qps | 21.8 % | 3 | 1,601 qps | 7.3 % | 1.97x |
| 10,000 | none | exists-correlated | 2,957 qps | 41.3 % | 3 | 1,421 qps | 4.4 % | 2.08x |
| 10,000 | none | count-by-user | 5,184 qps | 23.1 % | 3 | 2,637 qps | 1.6 % | 1.97x |
| 10,000 | all | pk-user | 36,765 qps | 1.8 % | 3 | 11,364 qps | 34.5 % | 3.24x |
| 10,000 | all | loans-by-user | 27,027 qps | 1.6 % | 3 | 7,241 qps | 73.1 % | 3.73x |
| 10,000 | all | loans-by-book | 29,762 qps | 1.2 % | 3 | 9,099 qps | 40.0 % | 3.27x |
| 10,000 | all | resv-by-user | 33,113 qps | 2.6 % | 3 | 9,950 qps | 38.6 % | 3.33x |
| 10,000 | all | books-by-author | 30,303 qps | 2.1 % | 3 | 9,814 qps | 25.0 % | 3.09x |
| 10,000 | all | books-by-genre | 6,231 qps | 0.9 % | 3 | 2,362 qps | 2.1 % | 2.64x |
| 10,000 | all | loans-by-daterange | 1,900 qps | 0.4 % | 3 | 815 qps | 2.1 % | 2.33x |
| 10,000 | all | overdue | 3,021 qps | 0.2 % | 3 | 1,189 qps | 3.0 % | 2.54x |
| 10,000 | all | join-loan-user | 23,529 qps | 0.5 % | 3 | 8,562 qps | 46.0 % | 2.75x |
| 10,000 | all | join-no-literal | 8,850 qps | 0.4 % | 3 | 3,442 qps | 17.6 % | 2.57x |
| 10,000 | all | exists-correlated | 15,674 qps | 1.1 % | 3 | 6,402 qps | 10.7 % | 2.45x |
| 10,000 | all | count-by-user | 39,370 qps | 0.8 % | 3 | 15,898 qps | 23.0 % | 2.48x |

## 4. Percentiles

Run 1 of `s3-c1-n10000-none`:

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 4 | 175.5 µs | 175.5 µs | 371.8 µs | 479.5 µs | 479.5 µs | 479.5 µs | 0 |
| load | 19,000 | 122.7 µs | 145.1 µs | 147.3 µs | 178.5 µs | 298.2 µs | 5,158.5 µs | 0 |
| pk-user | 200 | 35.4 µs | 35.8 µs | 36.1 µs | 37.7 µs | 42.7 µs | 229.6 µs | 0 |
| loans-by-user | 200 | 170.5 µs | 181.0 µs | 183.9 µs | 199.3 µs | 212.6 µs | 239.6 µs | 0 |
| loans-by-book | 200 | 172.8 µs | 179.2 µs | 180.8 µs | 192.1 µs | 197.1 µs | 198.6 µs | 0 |
| resv-by-user | 200 | 104.5 µs | 109.0 µs | 110.3 µs | 115.2 µs | 119.7 µs | 124.5 µs | 0 |
| books-by-author | 200 | 67.2 µs | 73.1 µs | 74.8 µs | 79.6 µs | 82.8 µs | 109.0 µs | 0 |
| books-by-genre | 200 | 175.0 µs | 184.2 µs | 189.5 µs | 201.5 µs | 209.7 µs | 216.4 µs | 0 |
| loans-by-daterange | 200 | 211.1 µs | 609.8 µs | 631.1 µs | 672.4 µs | 695.3 µs | 747.2 µs | 0 |
| overdue | 200 | 210.1 µs | 491.0 µs | 506.0 µs | 545.4 µs | 553.2 µs | 561.2 µs | 0 |
| join-loan-user | 200 | 180.4 µs | 184.2 µs | 186.4 µs | 192.6 µs | 199.7 µs | 240.5 µs | 0 |
| join-no-literal | 200 | 290.9 µs | 293.5 µs | 294.4 µs | 298.5 µs | 306.9 µs | 325.3 µs | 0 |
| exists-correlated | 200 | 331.5 µs | 337.1 µs | 339.5 µs | 347.8 µs | 377.6 µs | 472.6 µs | 0 |
| count-by-user | 200 | 166.9 µs | 170.8 µs | 171.3 µs | 184.1 µs | 188.0 µs | 197.0 µs | 0 |

Run 1 of `s3-c1-n10000-all`:

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 4 | 206.6 µs | 206.6 µs | 321.3 µs | 337.3 µs | 337.3 µs | 337.3 µs | 0 |
| load | 19,000 | 122.7 µs | 135.5 µs | 150.4 µs | 181.9 µs | 357.6 µs | 5,981.5 µs | 0 |
| create-index | 8 | 1,842.0 µs | 2,020.8 µs | 4,521.1 µs | 9,196.7 µs | 9,196.7 µs | 9,196.7 µs | 0 |
| pk-user | 200 | 41.5 µs | 42.3 µs | 42.5 µs | 45.4 µs | 52.8 µs | 257.1 µs | 0 |
| loans-by-user | 200 | 46.6 µs | 51.9 µs | 54.5 µs | 61.3 µs | 66.0 µs | 180.7 µs | 0 |
| loans-by-book | 200 | 41.6 µs | 49.0 µs | 51.4 µs | 57.3 µs | 60.0 µs | 61.8 µs | 0 |
| resv-by-user | 200 | 39.0 µs | 46.3 µs | 47.8 µs | 52.4 µs | 54.9 µs | 57.4 µs | 0 |
| books-by-author | 200 | 40.8 µs | 48.7 µs | 50.5 µs | 56.8 µs | 58.7 µs | 67.3 µs | 0 |
| books-by-genre | 200 | 179.1 µs | 190.5 µs | 196.2 µs | 208.8 µs | 213.3 µs | 224.1 µs | 0 |
| loans-by-daterange | 200 | 208.0 µs | 612.4 µs | 630.5 µs | 674.8 µs | 692.7 µs | 752.5 µs | 0 |
| overdue | 200 | 47.7 µs | 393.0 µs | 412.2 µs | 455.7 µs | 464.6 µs | 471.4 µs | 0 |
| join-loan-user | 200 | 54.8 µs | 58.4 µs | 60.6 µs | 66.5 µs | 70.9 µs | 88.3 µs | 0 |
| join-no-literal | 200 | 140.9 µs | 142.1 µs | 142.8 µs | 146.2 µs | 149.5 µs | 176.6 µs | 0 |
| exists-correlated | 200 | 84.3 µs | 84.8 µs | 85.2 µs | 88.0 µs | 89.6 µs | 108.0 µs | 0 |
| count-by-user | 200 | 40.2 µs | 42.2 µs | 42.8 µs | 45.1 µs | 46.3 µs | 54.1 µs | 0 |

## 5. The engine, isolated, unpinned: `07f4552a` against `9f8f8b354`, interleaved on this host

Three interleaved pairs, old first, each on a fresh server and data file.

10,000 loans, unindexed:

| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s3 ddl (10,000 loans, unindexed) | 2,605.5 qps | 74.6 % | 3,342.2 qps | 78.2 % | 1.28x | spread > 20 %, no claim |
| s3 load (10,000 loans, unindexed) | 5,168.0 qps | 71.0 % | 6,101.3 qps | 3.6 % | 1.18x | spread > 20 %, no claim |
| s3 pk-user (10,000 loans, unindexed) | 22,675.7 qps | 19.3 % | 22,624.4 qps | 61.4 % | 1.00x | spread > 20 %, no claim |
| s3 loans-by-user (10,000 loans, unindexed) | 5,750.4 qps | 4.9 % | 5,405.4 qps | 28.7 % | 0.94x | spread > 20 %, no claim |
| s3 loans-by-book (10,000 loans, unindexed) | 5,717.6 qps | 3.9 % | 5,515.7 qps | 28.3 % | 0.96x | spread > 20 %, no claim |
| s3 resv-by-user (10,000 loans, unindexed) | 9,765.6 qps | 7.3 % | 9,049.8 qps | 34.0 % | 0.93x | spread > 20 %, no claim |
| s3 books-by-author (10,000 loans, unindexed) | 14,471.8 qps | 10.7 % | 13,262.6 qps | 38.0 % | 0.92x | spread > 20 %, no claim |
| s3 books-by-genre (10,000 loans, unindexed) | 5,512.7 qps | 4.0 % | 5,285.4 qps | 22.5 % | 0.96x | spread > 20 %, no claim |
| s3 loans-by-daterange (10,000 loans, unindexed) | 1,629.7 qps | 0.3 % | 1,609.8 qps | 20.4 % | 0.99x | spread > 20 %, no claim |
| s3 overdue (10,000 loans, unindexed) | 2,067.4 qps | 1.3 % | 2,018.6 qps | 20.4 % | 0.98x | spread > 20 %, no claim |
| s3 join-loan-user (10,000 loans, unindexed) | 5,694.8 qps | 5.8 % | 5,316.3 qps | 27.1 % | 0.93x | spread > 20 %, no claim |
| s3 join-no-literal (10,000 loans, unindexed) | 3,521.1 qps | 3.7 % | 3,388.7 qps | 22.4 % | 0.96x | spread > 20 %, no claim |
| s3 exists-correlated (10,000 loans, unindexed) | 3,027.6 qps | 3.3 % | 2,934.3 qps | 39.9 % | 0.97x | spread > 20 %, no claim |
| s3 count-by-user (10,000 loans, unindexed) | 6,257.8 qps | 4.6 % | 5,773.7 qps | 31.3 % | 0.92x | spread > 20 %, no claim |

10,000 loans, indexed:

| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s3 ddl (10,000 loans, indexed) | 1,518.1 qps | 14.3 % | 2,045.4 qps | 64.7 % | 1.35x | spread > 20 %, no claim |
| s3 load (10,000 loans, indexed) | 5,005.0 qps | 39.2 % | 2,856.3 qps | 121.0 % | 0.57x | spread > 20 %, no claim |
| s3 create-index (10,000 loans, indexed) | 181.4 qps | 82.5 % | 166.1 qps | 49.3 % | 0.92x | spread > 20 %, no claim |
| s3 pk-user (10,000 loans, indexed) | 36,764.7 qps | 38.4 % | 22,779.0 qps | 5.3 % | 0.62x | spread > 20 %, no claim |
| s3 loans-by-user (10,000 loans, indexed) | 27,472.5 qps | 34.0 % | 17,985.6 qps | 16.0 % | 0.65x | spread > 20 %, no claim |
| s3 loans-by-book (10,000 loans, indexed) | 29,585.8 qps | 35.9 % | 19,379.8 qps | 16.9 % | 0.66x | spread > 20 %, no claim |
| s3 resv-by-user (10,000 loans, indexed) | 33,112.6 qps | 36.9 % | 21,052.6 qps | 17.3 % | 0.64x | spread > 20 %, no claim |
| s3 books-by-author (10,000 loans, indexed) | 30,303.0 qps | 35.3 % | 19,762.8 qps | 17.0 % | 0.65x | spread > 20 %, no claim |
| s3 books-by-genre (10,000 loans, indexed) | 6,410.3 qps | 21.2 % | 5,076.1 qps | 4.0 % | 0.79x | spread > 20 %, no claim |
| s3 loans-by-daterange (10,000 loans, indexed) | 2,014.1 qps | 19.2 % | 1,608.8 qps | 5.1 % | 0.80x | **finding** |
| s3 overdue (10,000 loans, indexed) | 3,151.6 qps | 20.2 % | 2,509.4 qps | 3.5 % | 0.80x | spread > 20 %, no claim |
| s3 join-loan-user (10,000 loans, indexed) | 23,753.0 qps | 31.3 % | 16,420.4 qps | 17.6 % | 0.69x | spread > 20 %, no claim |
| s3 join-no-literal (10,000 loans, indexed) | 9,363.3 qps | 25.6 % | 6,896.6 qps | 8.2 % | 0.74x | spread > 20 %, no claim |
| s3 exists-correlated (10,000 loans, indexed) | 17,241.4 qps | 29.9 % | 11,655.0 qps | 12.2 % | 0.68x | spread > 20 %, no claim |
| s3 count-by-user (10,000 loans, indexed) | 39,682.5 qps | 42.5 % | 23,201.9 qps | 19.2 % | 0.58x | spread > 20 %, no claim |

**The indexed table is not an engine reading.** Its new-over-old ratios sit at 0.58x to 0.80x on all twelve read shapes at once, the primary-key lookup (`pk-user`, 0.62x) among them, and every row but one fails the spread rule (`loans-by-daterange`, 0.80x, is marked). A ratio that is the same on a tuple key descent, an index probe, a join and a walk is not a property of any one access path. Section 6, which fixes where the server runs, reads the same cells 0.96x to 1.01x. The likeliest cause is the class of core the unpinned servers' threads ran on; it is unverified, since no per-run placement was recorded.

## 6. The engine, isolated, pinned: the same A/B with the server on the P-cores

Server under `taskset -c 0-7`, driver under `taskset -c 8-23`, five interleaved pairs, old first.

10,000 loans, unindexed:

| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s3 ddl (10,000 loans, unindexed) | 1,388.9 qps | 75.9 % | 1,477.3 qps | 51.3 % | 1.06x | spread > 20 %, no claim |
| s3 load (10,000 loans, unindexed) | 2,890.2 qps | 167.9 % | 6,775.1 qps | 38.5 % | 2.34x | spread > 20 %, no claim |
| s3 pk-user (10,000 loans, unindexed) | 30,303.0 qps | 15.4 % | 30,864.2 qps | 4.2 % | 1.02x | inside the spread |
| s3 loans-by-user (10,000 loans, unindexed) | 6,887.1 qps | 5.5 % | 6,839.9 qps | 1.0 % | 0.99x | inside the spread |
| s3 loans-by-book (10,000 loans, unindexed) | 7,032.3 qps | 9.8 % | 6,839.9 qps | 6.9 % | 0.97x | inside the spread |
| s3 resv-by-user (10,000 loans, unindexed) | 11,723.3 qps | 9.8 % | 11,560.7 qps | 3.1 % | 0.99x | inside the spread |
| s3 books-by-author (10,000 loans, unindexed) | 17,064.8 qps | 9.5 % | 17,241.4 qps | 11.9 % | 1.01x | inside the spread |
| s3 books-by-genre (10,000 loans, unindexed) | 5,948.8 qps | 6.4 % | 5,945.3 qps | 11.0 % | 1.00x | inside the spread |
| s3 loans-by-daterange (10,000 loans, unindexed) | 1,476.2 qps | 5.5 % | 1,521.4 qps | 3.4 % | 1.03x | inside the spread |
| s3 overdue (10,000 loans, unindexed) | 1,944.0 qps | 1.8 % | 1,963.9 qps | 3.1 % | 1.01x | inside the spread |
| s3 join-loan-user (10,000 loans, unindexed) | 6,775.1 qps | 1.8 % | 6,702.4 qps | 4.0 % | 0.99x | inside the spread |
| s3 join-no-literal (10,000 loans, unindexed) | 4,147.7 qps | 2.9 % | 4,116.9 qps | 5.4 % | 0.99x | inside the spread |
| s3 exists-correlated (10,000 loans, unindexed) | 4,095.0 qps | 2.1 % | 4,122.0 qps | 4.6 % | 1.01x | inside the spread |
| s3 count-by-user (10,000 loans, unindexed) | 7,501.9 qps | 0.6 % | 7,418.4 qps | 1.0 % | 0.99x | **finding** |

10,000 loans, indexed:

| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s3 ddl (10,000 loans, indexed) | 1,711.7 qps | 37.9 % | 1,736.4 qps | 89.0 % | 1.01x | spread > 20 %, no claim |
| s3 load (10,000 loans, indexed) | 6,729.5 qps | 66.7 % | 6,626.9 qps | 64.2 % | 0.98x | spread > 20 %, no claim |
| s3 create-index (10,000 loans, indexed) | 215.1 qps | 18.6 % | 218.8 qps | 6.1 % | 1.02x | inside the spread |
| s3 pk-user (10,000 loans, indexed) | 18,975.3 qps | 10.1 % | 18,181.8 qps | 13.8 % | 0.96x | inside the spread |
| s3 loans-by-user (10,000 loans, indexed) | 25,062.7 qps | 6.4 % | 24,813.9 qps | 2.2 % | 0.99x | inside the spread |
| s3 loans-by-book (10,000 loans, indexed) | 27,027.0 qps | 19.1 % | 27,173.9 qps | 10.0 % | 1.01x | inside the spread |
| s3 resv-by-user (10,000 loans, indexed) | 29,761.9 qps | 1.5 % | 29,761.9 qps | 22.8 % | 1.00x | spread > 20 %, no claim |
| s3 books-by-author (10,000 loans, indexed) | 27,624.3 qps | 12.0 % | 27,777.8 qps | 6.6 % | 1.01x | inside the spread |
| s3 books-by-genre (10,000 loans, indexed) | 5,361.9 qps | 4.7 % | 5,382.1 qps | 13.2 % | 1.00x | inside the spread |
| s3 loans-by-daterange (10,000 loans, indexed) | 1,507.4 qps | 1.2 % | 1,524.9 qps | 2.7 % | 1.01x | inside the spread |
| s3 overdue (10,000 loans, indexed) | 2,371.9 qps | 2.8 % | 2,388.3 qps | 1.6 % | 1.01x | inside the spread |
| s3 join-loan-user (10,000 loans, indexed) | 24,038.5 qps | 6.2 % | 23,696.7 qps | 4.2 % | 0.99x | inside the spread |
| s3 join-no-literal (10,000 loans, indexed) | 8,888.9 qps | 3.1 % | 8,635.6 qps | 7.5 % | 0.97x | inside the spread |
| s3 exists-correlated (10,000 loans, indexed) | 17,064.8 qps | 5.4 % | 16,420.4 qps | 2.9 % | 0.96x | inside the spread |
| s3 count-by-user (10,000 loans, indexed) | 34,722.2 qps | 2.1 % | 34,482.8 qps | 3.1 % | 0.99x | inside the spread |

## 7. What the run says about the engine

- **No change between `07f4552a` and `9f8f8b354` on the library's reads**: the tuple key lookup, the unindexed walks, the indexed probes and ranges, the two joins, the correlated `EXISTS` and the grouped count read 0.96x to 1.03x pinned, and the online index build (`create-index`, 1.02x) is inside its spread.
- **One row is marked, at 1.1 %**: `count-by-user` unindexed, 7,418.4 qps against 7,501.9 qps, with five-run spreads of 1.0 % and 0.6 %. The same shape indexed reads 0.99x inside its spread (3.1 % and 2.1 %). The rule marks it and it is reported as marked; a 1.1 % reading on a 135 µs statement is not attributed to a milestone here.
- `ddl` and `load` do not resolve (spreads 38 % to 168 %): four and 19,000 statements whose mean is set by a few slow ones.
- The cross-host ratios (1.40x to 3.81x at one core, 1.59x to 3.73x at eight) are the host.
- The unpinned series marks one row, `loans-by-daterange` indexed (0.80x), inside a table that reads 0.58x to 0.80x on every read shape; pinned, that row reads 1.01x inside its spread. It is not read as an engine change.

## 8. Noise floor, contamination and host

- **The unpinned cells are bimodal on this host** (hybrid CPU, governor `powersave`; summary, Stamp, the Host row). At one core the median spread over the 72 rows of section 3 is 29.6 % (the `v3.5.0` run's 6.2 %), and at eight cores 20.2 % (8.3 %).
- Three main cells carry foreign samples of 50 % or more of one CPU, all another session's `cc1plus`, kept and disclosed: `r2-s3-c1-n10000-none` (2 of 2 samples), `r2-s3-c8-n10000-all` (2 of 3) and `r3-s3-c8-n10000-none` (3 of 3). No A/B cell, pinned or not, carries one.
- Gate waits on scenario 3 cells: `s3-c1-n1000-all` 6 s, `r2-s3-c1-n10000-all` 25 s, `r3-s3-c1-n200-none` 39 s, `r3-s3-c8-n10000-all` 21 s; every other scenario 3 gate of the main run and the unpinned A/B 0 s; pinned A/B: `pin-ab-new1-s3-c1-n10000-none` 406 s in two holds (another session's test suite), `pin-ab-new1-s3-c1-n10000-all` 24 s, the rest 0 s.

## 9. Archive

`bench/v3.5.0/archive/scenario3-v3.4.17-159-g9f8f8b354/` (per-cell JSON, driver stdout, prechecks, gate waits, start and end times, server configs and logs) and `bench/v3.5.0/archive/run-v3.4.17-159-g9f8f8b354/`. No data file is kept.
