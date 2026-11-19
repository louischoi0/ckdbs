# Scenario 3 (library) at `v3.4.14`

**One line.** Twelve read shapes over four relations at 200, 1,000 and 10,000 loans, with and without server indexes, at `cores = 1` and `cores = 8`, three runs each: **the engine is the same as `v3.3.0-38-ga40882af`'s inside the spread on all twelve shapes (an interleaved same-host A/B, 0.98x to 1.04x, the one outlier `resv-by-user` 1.14x with spreads of 17 % and 2 %)**, while against the recorded numbers the integer-column equality walks read 28 % to 38 % faster and the two scans that return many rows read 6 % to 16 % slower, which is the host (a faster core for scanning, a slower one for the Python client decoding) and not the engine. An index is worth 3.6x to 4.9x on four equality shapes at 10,000 loans and nothing at 200, and the join, the correlated `EXISTS` and the aggregate each keep the plan they had.

Driver: `tools/scenario3_library.py` from the `35086008` extraction, unmodified, `--index-mode none|all --server-indexes on --seed 1` (`--assert-index-reads` on the indexed cells). Run by `ck-tester`.
Version measured: **`v3.4.14` = `35086008`** (`git describe --tags 35086008`). **`origin/main` is ahead of the tag** (`12887a14`, `v3.4.14-22-g12887a14`); nothing after the tag is measured.
Comparator: `bench/v3.3.0/results-scenario3-library-v3.3.0-38-ga40882af.md`, another host (see `results-scenario1-backtest-v3.4.14.md` section 1), so section 8's same-host A/B is the reading of the engine. PostgreSQL (the floor): **not measured** on this host (no server); the columns carry the other-host PostgreSQL 18.6 of the v3.3.0-33 run, orientation only.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-11-19; `cores = 1` cells x 3 runs and the `cores = 8` first run 19:10:34 to 19:13:43 UTC; `cores = 8` runs 2 and 3 19:22:28 to 19:24:11; engine A/B (6 cells of this scenario) inside 19:14:53 to 19:22:28 |
| Branch / commit | extraction of tag `v3.4.14` = `35086008` under `/home/louis/bench-runs/scenarios-v3.4.14/src/`; **`origin/main` (`12887a14`) is ahead of the tag**; files written in worktree `scenarios-v3-4-14` |
| Tree cleanliness | an extraction has no working tree; drivers unmodified |
| Binary provenance | Release build of the extraction, source binary mtime 2026-11-19 18:56:53.09 UTC; measured a copy, **sha256 `78b3aede97192d165094e5051a80958b026d36e2bb92d7a13a394412399d3fb3`**. A/B arm: `a40882af` Release, sha256 `42041f0b8d681dcffeb271495b1626e06b9b4feb39d5561a7615619845d1e2a8` |
| Device | `/dev/nvme0n1p1`, ext4; data files under `/home/louis/bench-runs/scenarios-v3.4.14/data/`, never tmpfs; fresh server and data file per cell |
| Build type | Release |
| Host | 8 logical CPUs, Intel Xeon Platinum 8581C @ 2.10 GHz, Linux 6.12.116, shared VM |
| Server config | `cores` 1 or 8, `durability = group` (reads only, so durability does not enter), `log_level = warn`, `buffer_pool_frames = 65536`, all else default; port 15620 |
| Quiet host | gate before each cell: every wait 0 s; 1-second monitor throughout, largest foreign sample 27.7 % of one CPU (this session's agent); no cell discarded |
| 60 s watchdog | status-file and PID poll; no orphan, no stall |

## 2. What was run

| axis | values |
|---|---|
| cores | 1, 8 |
| index mode | `none` (the driver creates no index) and `all` (it creates the indexes each shape can use, after the load); `--server-indexes on` |
| rows | `--loans 200`, `1000`, `10000`; users, books and reservations scale with it (at 200 loans 40 / 40 / 100 relation rows, at 10,000 loans 2,000 / 2,000 / 5,000) |
| runs | 3 per cell (`r2-`, `r3-` prefixes), 72 cells; a fresh server and data file per cell |
| shapes | 12, 200 operations each, serial on one connection, so the **qps is derived**: 1,000,000 / mean µs |
| A/B | `ab-old{1,2,3}-s3-c1-n10000-none` against `ab-new{1,2,3}-s3-c1-n10000-none`, interleaved old, new, old, new, old, new; old = the `a40882af` binary, same driver |
| PostgreSQL | not run (no server on this host) |

Cut: the v3.3.0-38 run's join-order probe (planned against written) and old-client A/B were not repeated (client unchanged, see scenario 1 section 2); the join planner's plans are not re-read here. Everything else of the recorded plan ran, with its repeats.

**Row-count axis (rule 9):** `--loans 200 | 1000 | 10000` is the table's size column; pk shapes (`pk-user`) are flat across it and are shown at all three sizes as the evidence.

## 3. Throughput, now against the recorded run

Derived qps (1,000,000 / mean µs), median of three runs; "read" by the rule of the v3.3.0-38 summary. **Deltas against v3.3.0-38 are engine plus host; section 8 separates them.** `none` = unindexed, `all` = indexed. The PostgreSQL column is the other-host 18.6; the ratio carries the host difference.

#### cores = 1

| loans | index | shape | now | spread | runs | v3.3.0-38 | spread | delta | read | PG 18.6 | KDS/PG now |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 200 | none | pk-user | 16,313 qps | 3.4 % | 3 | 14,430 qps | 5.3 % | +13.1 % | **finding** | 15,480 qps | 1.05x |
| 200 | none | loans-by-user | 13,089 qps | 8.3 % | 3 | 12,210 qps | 1.5 % | +7.2 % | inside the spread | 13,263 qps | 0.99x |
| 200 | none | loans-by-book | 13,831 qps | 7.9 % | 3 | 12,870 qps | 1.8 % | +7.5 % | inside the spread | 13,532 qps | 1.02x |
| 200 | none | resv-by-user | 15,314 qps | 8.0 % | 3 | 14,347 qps | 2.2 % | +6.7 % | inside the spread | 15,198 qps | 1.01x |
| 200 | none | books-by-author | 14,925 qps | 6.6 % | 3 | 14,124 qps | 2.3 % | +5.7 % | inside the spread | 15,129 qps | 0.99x |
| 200 | none | books-by-genre | 15,083 qps | 6.2 % | 3 | 14,306 qps | 3.7 % | +5.4 % | inside the spread | 15,601 qps | 0.97x |
| 200 | none | loans-by-daterange | 12,180 qps | 1.2 % | 3 | 11,669 qps | 3.0 % | +4.4 % | **finding** | 11,710 qps | 1.04x |
| 200 | none | overdue | 8,621 qps | 64.3 % | 3 | 12,121 qps | 29.9 % | -28.9 % | spread > 20 %, no claim | 12,225 qps | 0.71x |
| 200 | none | join-loan-user | 6,523 qps | 87.5 % | 3 | 10,060 qps | 28.1 % | -35.2 % | spread > 20 %, no claim | 9,833 qps | 0.66x |
| 200 | none | join-no-literal | 4,225 qps | 39.9 % | 3 | 4,902 qps | 15.5 % | -13.8 % | spread > 20 %, no claim | 4,380 qps | 0.96x |
| 200 | none | exists-correlated | 8,688 qps | 39.6 % | 3 | 8,097 qps | 23.0 % | +7.3 % | spread > 20 %, no claim | 5,653 qps | 1.54x |
| 200 | none | count-by-user | 17,301 qps | 30.1 % | 3 | 14,993 qps | 31.8 % | +15.4 % | spread > 20 %, no claim | 13,351 qps | 1.30x |
| 200 | all | pk-user | 16,155 qps | 2.6 % | 3 | 14,535 qps | 2.5 % | +11.1 % | **finding** | 15,748 qps | 1.03x |
| 200 | all | loans-by-user | 12,937 qps | 4.2 % | 3 | 12,407 qps | 2.1 % | +4.3 % | **finding** | 10,142 qps | 1.28x |
| 200 | all | loans-by-book | 13,423 qps | 6.6 % | 3 | 12,987 qps | 3.9 % | +3.4 % | inside the spread | 9,542 qps | 1.41x |
| 200 | all | resv-by-user | 15,221 qps | 4.7 % | 3 | 14,225 qps | 2.9 % | +7.0 % | **finding** | 10,707 qps | 1.42x |
| 200 | all | books-by-author | 14,065 qps | 10.6 % | 3 | 13,550 qps | 1.5 % | +3.8 % | inside the spread | 10,341 qps | 1.36x |
| 200 | all | books-by-genre | 15,152 qps | 9.8 % | 3 | 14,225 qps | 22.4 % | +6.5 % | spread > 20 %, no claim | 10,638 qps | 1.42x |
| 200 | all | loans-by-daterange | 11,834 qps | 7.0 % | 3 | 11,655 qps | 27.2 % | +1.5 % | spread > 20 %, no claim | 11,223 qps | 1.05x |
| 200 | all | overdue | 13,316 qps | 10.1 % | 3 | 12,300 qps | 3.8 % | +8.3 % | inside the spread | 10,741 qps | 1.24x |
| 200 | all | join-loan-user | 11,933 qps | 3.4 % | 3 | 10,320 qps | 2.7 % | +15.6 % | **finding** | 8,780 qps | 1.36x |
| 200 | all | join-no-literal | 4,484 qps | 2.8 % | 3 | 4,602 qps | 6.3 % | -2.6 % | inside the spread | 4,062 qps | 1.10x |
| 200 | all | exists-correlated | 7,831 qps | 2.4 % | 3 | 7,593 qps | 4.4 % | +3.1 % | inside the spread | 4,632 qps | 1.69x |
| 200 | all | count-by-user | 17,544 qps | 3.1 % | 3 | 15,408 qps | 3.9 % | +13.9 % | **finding** | 9,524 qps | 1.84x |
| 1,000 | none | pk-user | 16,207 qps | 5.6 % | 3 | 14,493 qps | 24.6 % | +11.8 % | spread > 20 %, no claim | 14,388 qps | 1.13x |
| 1,000 | none | loans-by-user | 10,593 qps | 3.3 % | 3 | 9,217 qps | 25.2 % | +14.9 % | spread > 20 %, no claim | 9,643 qps | 1.10x |
| 1,000 | none | loans-by-book | 11,111 qps | 1.1 % | 3 | 9,662 qps | 16.4 % | +15.0 % | inside the spread | 9,606 qps | 1.16x |
| 1,000 | none | resv-by-user | 13,495 qps | 1.9 % | 3 | 11,947 qps | 1.6 % | +13.0 % | **finding** | 12,330 qps | 1.09x |
| 1,000 | none | books-by-author | 13,986 qps | 1.2 % | 3 | 12,903 qps | 0.8 % | +8.4 % | **finding** | 13,495 qps | 1.04x |
| 1,000 | none | books-by-genre | 11,507 qps | 1.5 % | 3 | 11,038 qps | 1.1 % | +4.3 % | **finding** | 11,696 qps | 0.98x |
| 1,000 | none | loans-by-daterange | 6,274 qps | 0.6 % | 3 | 5,921 qps | 0.4 % | +6.0 % | **finding** | 5,984 qps | 1.05x |
| 1,000 | none | overdue | 6,954 qps | 8.6 % | 3 | 6,766 qps | 20.4 % | +2.8 % | spread > 20 %, no claim | 6,540 qps | 1.06x |
| 1,000 | none | join-loan-user | 9,542 qps | 27.7 % | 3 | 7,880 qps | 24.2 % | +21.1 % | spread > 20 %, no claim | 7,375 qps | 1.29x |
| 1,000 | none | join-no-literal | 4,417 qps | 2.3 % | 3 | 4,250 qps | 14.5 % | +3.9 % | inside the spread | 3,452 qps | 1.28x |
| 1,000 | none | exists-correlated | 6,297 qps | 8.1 % | 3 | 5,942 qps | 19.7 % | +6.0 % | inside the spread | 3,322 qps | 1.90x |
| 1,000 | none | count-by-user | 13,569 qps | 4.2 % | 3 | 10,870 qps | 21.9 % | +24.8 % | spread > 20 %, no claim | 9,662 qps | 1.40x |
| 1,000 | all | pk-user | 15,625 qps | 7.4 % | 3 | 14,430 qps | 1.2 % | +8.3 % | **finding** | 14,347 qps | 1.09x |
| 1,000 | all | loans-by-user | 12,690 qps | 11.9 % | 3 | 12,048 qps | 0.6 % | +5.3 % | inside the spread | 11,261 qps | 1.13x |
| 1,000 | all | loans-by-book | 13,423 qps | 1.8 % | 3 | 12,594 qps | 1.0 % | +6.6 % | **finding** | 11,669 qps | 1.15x |
| 1,000 | all | resv-by-user | 14,556 qps | 4.2 % | 3 | 13,587 qps | 1.8 % | +7.1 % | **finding** | 13,228 qps | 1.10x |
| 1,000 | all | books-by-author | 14,184 qps | 11.3 % | 3 | 13,055 qps | 1.7 % | +8.7 % | inside the spread | 11,682 qps | 1.21x |
| 1,000 | all | books-by-genre | 10,204 qps | 25.5 % | 3 | 10,753 qps | 2.4 % | -5.1 % | spread > 20 %, no claim | 10,309 qps | 0.99x |
| 1,000 | all | loans-by-daterange | 6,050 qps | 2.4 % | 3 | 5,865 qps | 2.1 % | +3.1 % | **finding** | 5,669 qps | 1.07x |
| 1,000 | all | overdue | 7,728 qps | 18.2 % | 3 | 7,669 qps | 2.0 % | +0.8 % | inside the spread | 7,273 qps | 1.06x |
| 1,000 | all | join-loan-user | 11,905 qps | 2.1 % | 3 | 10,040 qps | 1.2 % | +18.6 % | **finding** | 8,347 qps | 1.43x |
| 1,000 | all | join-no-literal | 4,355 qps | 8.8 % | 3 | 4,390 qps | 5.0 % | -0.8 % | inside the spread | 3,135 qps | 1.39x |
| 1,000 | all | exists-correlated | 7,616 qps | 1.5 % | 3 | 7,380 qps | 2.2 % | +3.2 % | **finding** | 4,836 qps | 1.58x |
| 1,000 | all | count-by-user | 17,361 qps | 1.4 % | 3 | 15,129 qps | 21.7 % | +14.8 % | spread > 20 %, no claim | 11,820 qps | 1.47x |
| 10,000 | none | pk-user | 16,129 qps | 3.8 % | 3 | 14,859 qps | 3.6 % | +8.5 % | **finding** | 14,925 qps | 1.08x |
| 10,000 | none | loans-by-user | 3,186 qps | 6.3 % | 3 | 2,449 qps | 1.4 % | +30.1 % | **finding** | 2,421 qps | 1.32x |
| 10,000 | none | loans-by-book | 3,278 qps | 2.4 % | 3 | 2,431 qps | 5.9 % | +34.8 % | **finding** | 2,307 qps | 1.42x |
| 10,000 | none | resv-by-user | 5,872 qps | 4.1 % | 3 | 4,255 qps | 11.8 % | +38.0 % | **finding** | 4,259 qps | 1.38x |
| 10,000 | none | books-by-author | 8,873 qps | 6.2 % | 3 | 6,930 qps | 20.7 % | +28.0 % | spread > 20 %, no claim | 6,734 qps | 1.32x |
| 10,000 | none | books-by-genre | 2,742 qps | 3.2 % | 3 | 2,925 qps | 12.2 % | -6.3 % | inside the spread | 3,041 qps | 0.90x |
| 10,000 | none | loans-by-daterange | 900 qps | 1.9 % | 3 | 913 qps | 2.0 % | -1.4 % | inside the spread | 1,063 qps | 0.85x |
| 10,000 | none | overdue | 1,126 qps | 2.6 % | 3 | 1,126 qps | 4.4 % | +0.1 % | inside the spread | 1,135 qps | 0.99x |
| 10,000 | none | join-loan-user | 3,145 qps | 3.7 % | 3 | 2,297 qps | 5.7 % | +36.9 % | **finding** | 2,327 qps | 1.35x |
| 10,000 | none | join-no-literal | 1,934 qps | 3.3 % | 3 | 1,666 qps | 8.0 % | +16.1 % | **finding** | 1,504 qps | 1.29x |
| 10,000 | none | exists-correlated | 1,588 qps | 6.2 % | 3 | 1,608 qps | 8.0 % | -1.2 % | inside the spread | 768 qps | 2.07x |
| 10,000 | none | count-by-user | 3,470 qps | 2.4 % | 3 | 2,665 qps | 8.7 % | +30.2 % | **finding** | 2,434 qps | 1.43x |
| 10,000 | all | pk-user | 15,848 qps | 6.4 % | 3 | 14,599 qps | 9.3 % | +8.6 % | inside the spread | 14,925 qps | 1.06x |
| 10,000 | all | loans-by-user | 12,346 qps | 1.4 % | 3 | 11,682 qps | 4.2 % | +5.7 % | **finding** | 10,834 qps | 1.14x |
| 10,000 | all | loans-by-book | 13,175 qps | 4.8 % | 3 | 12,438 qps | 3.1 % | +5.9 % | **finding** | 11,751 qps | 1.12x |
| 10,000 | all | resv-by-user | 14,327 qps | 38.1 % | 3 | 13,477 qps | 3.0 % | +6.3 % | spread > 20 %, no claim | 12,953 qps | 1.11x |
| 10,000 | all | books-by-author | 13,477 qps | 3.8 % | 3 | 12,547 qps | 4.2 % | +7.4 % | **finding** | 12,107 qps | 1.11x |
| 10,000 | all | books-by-genre | 2,662 qps | 1.9 % | 3 | 3,158 qps | 5.5 % | -15.7 % | **finding** | 3,755 qps | 0.71x |
| 10,000 | all | loans-by-daterange | 891 qps | 1.9 % | 3 | 933 qps | 2.0 % | -4.5 % | **finding** | 1,065 qps | 0.84x |
| 10,000 | all | overdue | 1,329 qps | 3.0 % | 3 | 1,591 qps | 4.3 % | -16.4 % | **finding** | 1,911 qps | 0.70x |
| 10,000 | all | join-loan-user | 11,390 qps | 2.3 % | 3 | 10,091 qps | 9.1 % | +12.9 % | **finding** | 8,929 qps | 1.28x |
| 10,000 | all | join-no-literal | 4,301 qps | 8.5 % | 3 | 4,374 qps | 8.7 % | -1.7 % | inside the spread | 1,478 qps | 2.91x |
| 10,000 | all | exists-correlated | 7,541 qps | 1.4 % | 3 | 7,468 qps | 0.7 % | +1.0 % | inside the spread | 5,128 qps | 1.47x |
| 10,000 | all | count-by-user | 17,123 qps | 0.5 % | 3 | 14,859 qps | 3.2 % | +15.2 % | **finding** | 11,521 qps | 1.49x |

#### cores = 8

| loans | index | shape | now | spread | runs | v3.3.0-38 | spread | delta | read | PG 18.6 | KDS/PG now |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 200 | none | pk-user | 15,432 qps | 6.1 % | 3 | 11,614 qps | 36.2 % | +32.9 % | spread > 20 %, no claim | 15,480 qps | 1.00x |
| 200 | none | loans-by-user | 11,601 qps | 2.6 % | 3 | 11,364 qps | 20.8 % | +2.1 % | spread > 20 %, no claim | 13,263 qps | 0.87x |
| 200 | none | loans-by-book | 12,300 qps | 6.8 % | 3 | 12,048 qps | 10.9 % | +2.1 % | inside the spread | 13,532 qps | 0.91x |
| 200 | none | resv-by-user | 14,205 qps | 6.4 % | 3 | 11,481 qps | 25.6 % | +23.7 % | spread > 20 %, no claim | 15,198 qps | 0.93x |
| 200 | none | books-by-author | 13,966 qps | 3.3 % | 3 | 12,594 qps | 22.8 % | +10.9 % | spread > 20 %, no claim | 15,129 qps | 0.92x |
| 200 | none | books-by-genre | 14,245 qps | 5.2 % | 3 | 13,298 qps | 9.1 % | +7.1 % | inside the spread | 15,601 qps | 0.91x |
| 200 | none | loans-by-daterange | 11,086 qps | 2.7 % | 3 | 10,246 qps | 12.4 % | +8.2 % | inside the spread | 11,710 qps | 0.95x |
| 200 | none | overdue | 11,990 qps | 5.9 % | 3 | 10,060 qps | 22.6 % | +19.2 % | spread > 20 %, no claim | 12,225 qps | 0.98x |
| 200 | none | join-loan-user | 10,537 qps | 4.0 % | 3 | 8,929 qps | 20.3 % | +18.0 % | spread > 20 %, no claim | 9,833 qps | 1.07x |
| 200 | none | join-no-literal | 4,464 qps | 5.6 % | 3 | 4,537 qps | 3.2 % | -1.6 % | inside the spread | 4,380 qps | 1.02x |
| 200 | none | exists-correlated | 7,710 qps | 11.8 % | 3 | 6,793 qps | 11.0 % | +13.5 % | **finding** | 5,653 qps | 1.36x |
| 200 | none | count-by-user | 15,480 qps | 20.1 % | 3 | 10,593 qps | 36.4 % | +46.1 % | spread > 20 %, no claim | 13,351 qps | 1.16x |
| 200 | all | pk-user | 15,106 qps | 5.7 % | 3 | 13,812 qps | 33.1 % | +9.4 % | spread > 20 %, no claim | 15,748 qps | 0.96x |
| 200 | all | loans-by-user | 12,500 qps | 1.6 % | 3 | 10,152 qps | 19.8 % | +23.1 % | **finding** | 10,142 qps | 1.23x |
| 200 | all | loans-by-book | 12,285 qps | 30.7 % | 3 | 12,077 qps | 2.8 % | +1.7 % | spread > 20 %, no claim | 9,542 qps | 1.29x |
| 200 | all | resv-by-user | 14,347 qps | 14.1 % | 3 | 13,405 qps | 4.0 % | +7.0 % | inside the spread | 10,707 qps | 1.34x |
| 200 | all | books-by-author | 13,351 qps | 12.1 % | 3 | 12,804 qps | 30.4 % | +4.3 % | spread > 20 %, no claim | 10,341 qps | 1.29x |
| 200 | all | books-by-genre | 12,438 qps | 19.2 % | 3 | 13,175 qps | 6.3 % | -5.6 % | inside the spread | 10,638 qps | 1.17x |
| 200 | all | loans-by-daterange | 11,223 qps | 7.7 % | 3 | 11,074 qps | 2.5 % | +1.3 % | inside the spread | 11,223 qps | 1.00x |
| 200 | all | overdue | 12,531 qps | 30.8 % | 3 | 11,614 qps | 4.1 % | +7.9 % | spread > 20 %, no claim | 10,741 qps | 1.17x |
| 200 | all | join-loan-user | 10,905 qps | 43.3 % | 3 | 9,756 qps | 4.3 % | +11.8 % | spread > 20 %, no claim | 8,780 qps | 1.24x |
| 200 | all | join-no-literal | 3,752 qps | 7.7 % | 3 | 4,268 qps | 2.1 % | -12.1 % | **finding** | 4,062 qps | 0.92x |
| 200 | all | exists-correlated | 6,579 qps | 25.2 % | 3 | 7,032 qps | 2.5 % | -6.4 % | spread > 20 %, no claim | 4,632 qps | 1.42x |
| 200 | all | count-by-user | 16,181 qps | 17.9 % | 3 | 14,771 qps | 5.1 % | +9.5 % | inside the spread | 9,524 qps | 1.70x |
| 1,000 | none | pk-user | 14,749 qps | 11.0 % | 3 | 13,699 qps | 0.8 % | +7.7 % | inside the spread | 14,388 qps | 1.03x |
| 1,000 | none | loans-by-user | 7,862 qps | 18.1 % | 3 | 8,203 qps | 1.5 % | -4.2 % | inside the spread | 9,643 qps | 0.82x |
| 1,000 | none | loans-by-book | 7,849 qps | 17.4 % | 3 | 8,532 qps | 0.8 % | -8.0 % | inside the spread | 9,606 qps | 0.82x |
| 1,000 | none | resv-by-user | 10,616 qps | 22.1 % | 3 | 11,013 qps | 1.4 % | -3.6 % | spread > 20 %, no claim | 12,330 qps | 0.86x |
| 1,000 | none | books-by-author | 12,315 qps | 5.6 % | 3 | 12,121 qps | 7.0 % | +1.6 % | inside the spread | 13,495 qps | 0.91x |
| 1,000 | none | books-by-genre | 10,183 qps | 11.6 % | 3 | 10,331 qps | 3.8 % | -1.4 % | inside the spread | 11,696 qps | 0.87x |
| 1,000 | none | loans-by-daterange | 5,230 qps | 4.3 % | 3 | 5,470 qps | 3.0 % | -4.4 % | **finding** | 5,984 qps | 0.87x |
| 1,000 | none | overdue | 5,889 qps | 3.0 % | 3 | 6,161 qps | 2.2 % | -4.4 % | **finding** | 6,540 qps | 0.90x |
| 1,000 | none | join-loan-user | 7,513 qps | 6.7 % | 3 | 7,315 qps | 5.9 % | +2.7 % | inside the spread | 7,375 qps | 1.02x |
| 1,000 | none | join-no-literal | 3,493 qps | 5.4 % | 3 | 3,867 qps | 1.5 % | -9.7 % | **finding** | 3,452 qps | 1.01x |
| 1,000 | none | exists-correlated | 5,373 qps | 3.2 % | 3 | 5,447 qps | 0.9 % | -1.3 % | inside the spread | 3,322 qps | 1.62x |
| 1,000 | none | count-by-user | 9,681 qps | 1.9 % | 3 | 9,560 qps | 1.8 % | +1.3 % | inside the spread | 9,662 qps | 1.00x |
| 1,000 | all | pk-user | 15,480 qps | 2.0 % | 3 | 13,495 qps | 1.8 % | +14.7 % | **finding** | 14,347 qps | 1.08x |
| 1,000 | all | loans-by-user | 12,121 qps | 3.2 % | 3 | 11,136 qps | 11.3 % | +8.8 % | inside the spread | 11,261 qps | 1.08x |
| 1,000 | all | loans-by-book | 12,706 qps | 2.5 % | 3 | 11,614 qps | 11.7 % | +9.4 % | inside the spread | 11,669 qps | 1.09x |
| 1,000 | all | resv-by-user | 13,774 qps | 3.5 % | 3 | 12,642 qps | 25.8 % | +9.0 % | spread > 20 %, no claim | 13,228 qps | 1.04x |
| 1,000 | all | books-by-author | 13,387 qps | 5.1 % | 3 | 12,165 qps | 7.8 % | +10.0 % | **finding** | 11,682 qps | 1.15x |
| 1,000 | all | books-by-genre | 10,277 qps | 6.7 % | 3 | 10,163 qps | 2.7 % | +1.1 % | inside the spread | 10,309 qps | 1.00x |
| 1,000 | all | loans-by-daterange | 5,097 qps | 6.5 % | 3 | 5,414 qps | 4.5 % | -5.9 % | inside the spread | 5,669 qps | 0.90x |
| 1,000 | all | overdue | 7,321 qps | 1.5 % | 3 | 7,077 qps | 9.7 % | +3.4 % | inside the spread | 7,273 qps | 1.01x |
| 1,000 | all | join-loan-user | 11,148 qps | 4.0 % | 3 | 9,470 qps | 18.0 % | +17.7 % | inside the spread | 8,347 qps | 1.34x |
| 1,000 | all | join-no-literal | 3,682 qps | 2.4 % | 3 | 4,032 qps | 0.4 % | -8.7 % | **finding** | 3,135 qps | 1.17x |
| 1,000 | all | exists-correlated | 6,489 qps | 1.4 % | 3 | 6,752 qps | 4.4 % | -3.9 % | inside the spread | 4,836 qps | 1.34x |
| 1,000 | all | count-by-user | 16,026 qps | 2.3 % | 3 | 13,831 qps | 12.0 % | +15.9 % | **finding** | 11,820 qps | 1.36x |
| 10,000 | none | pk-user | 14,970 qps | 5.2 % | 3 | 14,045 qps | 1.5 % | +6.6 % | **finding** | 14,925 qps | 1.00x |
| 10,000 | none | loans-by-user | 2,509 qps | 6.1 % | 3 | 2,301 qps | 0.7 % | +9.0 % | **finding** | 2,421 qps | 1.04x |
| 10,000 | none | loans-by-book | 2,528 qps | 5.0 % | 3 | 2,317 qps | 0.5 % | +9.1 % | **finding** | 2,307 qps | 1.10x |
| 10,000 | none | resv-by-user | 5,482 qps | 5.1 % | 3 | 4,203 qps | 0.7 % | +30.4 % | **finding** | 4,259 qps | 1.29x |
| 10,000 | none | books-by-author | 8,097 qps | 4.8 % | 3 | 6,601 qps | 0.7 % | +22.7 % | **finding** | 6,734 qps | 1.20x |
| 10,000 | none | books-by-genre | 2,760 qps | 2.1 % | 3 | 2,908 qps | 1.5 % | -5.1 % | **finding** | 3,041 qps | 0.91x |
| 10,000 | none | loans-by-daterange | 821 qps | 1.5 % | 3 | 889 qps | 1.7 % | -7.6 % | **finding** | 1,063 qps | 0.77x |
| 10,000 | none | overdue | 1,012 qps | 1.4 % | 3 | 1,095 qps | 1.0 % | -7.6 % | **finding** | 1,135 qps | 0.89x |
| 10,000 | none | join-loan-user | 2,362 qps | 9.4 % | 3 | 2,290 qps | 6.8 % | +3.1 % | inside the spread | 2,327 qps | 1.02x |
| 10,000 | none | join-no-literal | 1,593 qps | 3.2 % | 3 | 1,649 qps | 2.7 % | -3.4 % | **finding** | 1,504 qps | 1.06x |
| 10,000 | none | exists-correlated | 1,489 qps | 3.2 % | 3 | 1,607 qps | 0.2 % | -7.3 % | **finding** | 768 qps | 1.94x |
| 10,000 | none | count-by-user | 2,621 qps | 3.5 % | 3 | 2,417 qps | 1.9 % | +8.4 % | **finding** | 2,434 qps | 1.08x |
| 10,000 | all | pk-user | 15,175 qps | 9.0 % | 3 | 14,184 qps | 4.6 % | +7.0 % | inside the spread | 14,925 qps | 1.02x |
| 10,000 | all | loans-by-user | 11,338 qps | 43.4 % | 3 | 11,416 qps | 2.0 % | -0.7 % | spread > 20 %, no claim | 10,834 qps | 1.05x |
| 10,000 | all | loans-by-book | 12,048 qps | 5.1 % | 3 | 12,019 qps | 1.7 % | +0.2 % | inside the spread | 11,751 qps | 1.03x |
| 10,000 | all | resv-by-user | 13,495 qps | 4.4 % | 3 | 13,089 qps | 1.4 % | +3.1 % | inside the spread | 12,953 qps | 1.04x |
| 10,000 | all | books-by-author | 12,422 qps | 6.8 % | 3 | 12,180 qps | 2.6 % | +2.0 % | inside the spread | 12,107 qps | 1.03x |
| 10,000 | all | books-by-genre | 2,408 qps | 3.2 % | 3 | 2,994 qps | 0.4 % | -19.6 % | **finding** | 3,755 qps | 0.64x |
| 10,000 | all | loans-by-daterange | 824 qps | 0.5 % | 3 | 888 qps | 0.6 % | -7.1 % | **finding** | 1,065 qps | 0.77x |
| 10,000 | all | overdue | 1,199 qps | 4.0 % | 3 | 1,518 qps | 2.8 % | -21.0 % | **finding** | 1,911 qps | 0.63x |
| 10,000 | all | join-loan-user | 9,390 qps | 24.6 % | 3 | 9,756 qps | 1.3 % | -3.8 % | spread > 20 %, no claim | 8,929 qps | 1.05x |
| 10,000 | all | join-no-literal | 3,727 qps | 17.6 % | 3 | 4,115 qps | 2.7 % | -9.4 % | inside the spread | 1,478 qps | 2.52x |
| 10,000 | all | exists-correlated | 6,042 qps | 21.0 % | 3 | 6,831 qps | 1.0 % | -11.5 % | spread > 20 %, no claim | 5,128 qps | 1.18x |
| 10,000 | all | count-by-user | 15,267 qps | 5.0 % | 3 | 14,245 qps | 1.3 % | +7.2 % | **finding** | 11,521 qps | 1.33x |

Geometric mean of KDS over PostgreSQL across the 12 shapes, `cores = 1` (other-host PostgreSQL; the host difference is in both columns' ratios):

| loans | index | geometric mean KDS/PG over 12 shapes, now | v3.3.0-38 |
|---|---|---|---|
| 200 | none | 1.00x | 1.02x |
| 200 | all | 1.33x | 1.25x |
| 1,000 | none | 1.19x | 1.07x |
| 1,000 | all | 1.21x | 1.14x |
| 10,000 | none | 1.25x | 1.07x |
| 10,000 | all | 1.15x | 1.14x |

**Two groups of shapes, with opposite signs against the recorded run.** At 10,000 loans unindexed the integer-column equality walks (`loans-by-user` +30.1 %, `loans-by-book` +34.8 %, `resv-by-user` +38.0 %, `join-loan-user` +36.9 %, `count-by-user` +30.2 %) read about a third faster and the shapes that return many rows (`books-by-genre` -6.3 %, `loans-by-daterange` -1.4 %, `overdue` +0.1 %) are flat to slower; the indexed equality shapes gain 5 % to 15 % and the indexed `books-by-genre` and `overdue` read -16 %. The A/B of section 8 reads the engine at 0.98x to 1.04x on all twelve, so the gain on the walks is the host's faster scan (the examined-row cost is 24 ns here, section 7, against 33 ns recorded) and not a change in the engine. At `cores = 1` 8 of the 72 cell figures carry a spread above 20 % (5 of them at 200 loans unindexed), at `cores = 8` 9 of 72 (4 at 200 loans indexed): the 200-loan statements are 60 to 70 µs round trips and one slow run of three decides the spread.

## 4. What the index buys

Throughput of the indexed cell over the unindexed one, `cores = 1`, median of three, the recorded ratio and PostgreSQL's beside it:

| shape | n=200 all/none | n=1,000 all/none | n=10,000 all/none (now) | n=10,000 all/none (v3.3.0-38) | PG n=10,000 all/none |
|---|---|---|---|---|---|
| pk-user | 0.99x | 0.96x | 0.98x | 0.98x | 1.00x |
| loans-by-user | 0.99x | 1.20x | 3.88x | 4.77x | 4.47x |
| loans-by-book | 0.97x | 1.21x | 4.02x | 5.12x | 5.09x |
| resv-by-user | 0.99x | 1.08x | 2.44x | 3.17x | 3.04x |
| books-by-author | 0.94x | 1.01x | 1.52x | 1.81x | 1.80x |
| books-by-genre | 1.00x | 0.89x | 0.97x | 1.08x | 1.23x |
| loans-by-daterange | 0.97x | 0.96x | 0.99x | 1.02x | 1.00x |
| overdue | 1.54x | 1.11x | 1.18x | 1.41x | 1.68x |
| join-loan-user | 1.83x | 1.25x | 3.62x | 4.39x | 3.84x |
| join-no-literal | 1.06x | 0.99x | 2.22x | 2.63x | 0.98x |
| exists-correlated | 0.90x | 1.21x | 4.75x | 4.65x | 6.68x |
| count-by-user | 1.01x | 1.28x | 4.93x | 5.58x | 4.73x |

An index is worth nothing at 200 loans (0.90x to 1.06x on 10 of 12; `overdue` 1.54x and `join-loan-user` 1.83x come from unindexed cells whose spreads are 64.3 % and 87.5 %, one slow run of three, so no ratio is claimed), 0.89x to 1.28x at 1,000 and **3.6x to 4.9x on the equality shapes at 10,000** (`loans-by-user` 3.88x, `loans-by-book` 4.02x, `join-loan-user` 3.62x, `count-by-user` 4.93x, `exists-correlated` 4.75x) and 2.4x on `resv-by-user`. The factors are lower than the recorded 4.8x to 5.6x because **this host's unindexed walk is faster** (the numerator, the indexed read, moved +5.7 % and the denominator +30 %), not because the index changed; the structure pays for itself in the same range on both engines (PostgreSQL's recorded 4.47x on `loans-by-user`, 5.09x on `loans-by-book`). `loans-by-daterange` and `books-by-genre` gain nothing (0.99x, 0.97x): their cost is the rows returned or the range's whole-relation walk. `join-no-literal` gains 2.22x on KDS where the recorded PostgreSQL gained 0.98x; the other-host figure says only that the two engines' plans for that join differ, not which is better.

## 5. The join planner on the library's joins

Not re-measured (the probe was cut, section 2). The A/B's `join-loan-user` 0.98x, `join-no-literal` 1.04x and `exists-correlated` 0.98x say the joins' cost did not move with the engine; the replies of the plan statements are not captured in this run.

## 6. Percentiles

Run 1 of each `cores = 1` cell, every phase (microseconds; 200 operations per shape; `load` is the one-shot relation load; `ddl` is in the JSON):

#### `s3-c1-n200-none` run 1

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 4 ops | 612.1 µs | 612.1 µs | 640.0 µs | 971.0 µs | 971.0 µs | 971.0 µs | 0 errors |
| load | 380 ops | 296.1 µs | 390.2 µs | 438.8 µs | 639.6 µs | 769.0 µs | 1,532.0 µs | 0 errors |
| pk-user | 200 ops | 55.2 µs | 57.4 µs | 58.2 µs | 69.4 µs | 74.4 µs | 255.1 µs | 0 errors |
| loans-by-user | 200 ops | 61.0 µs | 71.0 µs | 74.5 µs | 86.6 µs | 95.8 µs | 127.5 µs | 0 errors |
| loans-by-book | 200 ops | 62.5 µs | 68.3 µs | 71.9 µs | 80.2 µs | 85.1 µs | 87.3 µs | 0 errors |
| resv-by-user | 200 ops | 55.5 µs | 61.3 µs | 63.6 µs | 71.1 µs | 74.6 µs | 76.1 µs | 0 errors |
| books-by-author | 200 ops | 59.1 µs | 62.8 µs | 65.9 µs | 74.8 µs | 81.3 µs | 83.3 µs | 0 errors |
| books-by-genre | 200 ops | 57.6 µs | 60.9 µs | 62.9 µs | 69.9 µs | 72.1 µs | 78.8 µs | 0 errors |
| loans-by-daterange | 200 ops | 67.5 µs | 75.8 µs | 79.8 µs | 93.6 µs | 112.3 µs | 126.8 µs | 0 errors |
| overdue | 200 ops | 61.4 µs | 72.1 µs | 74.5 µs | 85.1 µs | 89.6 µs | 100.0 µs | 0 errors |
| join-loan-user | 200 ops | 69.3 µs | 78.1 µs | 81.7 µs | 91.5 µs | 99.0 µs | 131.5 µs | 0 errors |
| join-no-literal | 200 ops | 185.2 µs | 188.1 µs | 190.1 µs | 201.7 µs | 214.7 µs | 270.3 µs | 0 errors |
| exists-correlated | 200 ops | 106.1 µs | 108.6 µs | 109.8 µs | 145.4 µs | 164.9 µs | 278.6 µs | 0 errors |
| count-by-user | 200 ops | 53.4 µs | 55.4 µs | 56.2 µs | 66.4 µs | 76.1 µs | 90.7 µs | 0 errors |

#### `s3-c1-n200-all` run 1

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 4 ops | 564.9 µs | 564.9 µs | 597.8 µs | 883.8 µs | 883.8 µs | 883.8 µs | 0 errors |
| load | 380 ops | 321.2 µs | 446.0 µs | 497.0 µs | 684.2 µs | 787.1 µs | 866.5 µs | 0 errors |
| create-index | 8 ops | 662.2 µs | 668.8 µs | 724.6 µs | 1,033.0 µs | 1,033.0 µs | 1,033.0 µs | 0 errors |
| pk-user | 200 ops | 55.2 µs | 57.2 µs | 58.1 µs | 66.0 µs | 88.0 µs | 274.5 µs | 0 errors |
| loans-by-user | 200 ops | 58.2 µs | 70.9 µs | 74.8 µs | 89.7 µs | 115.6 µs | 249.8 µs | 0 errors |
| loans-by-book | 200 ops | 59.8 µs | 67.8 µs | 72.6 µs | 84.7 µs | 92.5 µs | 101.0 µs | 0 errors |
| resv-by-user | 200 ops | 54.7 µs | 61.9 µs | 64.8 µs | 86.4 µs | 110.3 µs | 179.5 µs | 0 errors |
| books-by-author | 200 ops | 57.5 µs | 63.4 µs | 69.2 µs | 84.1 µs | 149.7 µs | 205.4 µs | 0 errors |
| books-by-genre | 200 ops | 58.0 µs | 64.0 µs | 66.9 µs | 93.8 µs | 194.4 µs | 241.3 µs | 0 errors |
| loans-by-daterange | 200 ops | 69.7 µs | 77.7 µs | 81.8 µs | 111.3 µs | 191.9 µs | 356.4 µs | 0 errors |
| overdue | 200 ops | 59.6 µs | 72.3 µs | 76.5 µs | 126.5 µs | 188.2 µs | 371.1 µs | 0 errors |
| join-loan-user | 200 ops | 68.9 µs | 78.6 µs | 82.4 µs | 96.3 µs | 105.6 µs | 131.2 µs | 0 errors |
| join-no-literal | 200 ops | 209.6 µs | 213.7 µs | 216.8 µs | 241.9 µs | 294.9 µs | 407.9 µs | 0 errors |
| exists-correlated | 200 ops | 120.8 µs | 124.3 µs | 125.6 µs | 137.0 µs | 148.5 µs | 172.7 µs | 0 errors |
| count-by-user | 200 ops | 51.7 µs | 55.0 µs | 56.3 µs | 61.0 µs | 74.3 µs | 86.0 µs | 0 errors |

#### `s3-c1-n1000-none` run 1

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 4 ops | 475.6 µs | 475.6 µs | 489.4 µs | 878.4 µs | 878.4 µs | 878.4 µs | 0 errors |
| load | 1,900 ops | 295.1 µs | 414.7 µs | 469.7 µs | 657.6 µs | 758.6 µs | 2,090.7 µs | 0 errors |
| pk-user | 200 ops | 56.1 µs | 58.6 µs | 59.3 µs | 79.2 µs | 206.0 µs | 238.3 µs | 0 errors |
| loans-by-user | 200 ops | 81.0 µs | 90.1 µs | 93.7 µs | 106.3 µs | 112.0 µs | 116.7 µs | 0 errors |
| loans-by-book | 200 ops | 80.5 µs | 86.4 µs | 88.9 µs | 99.8 µs | 105.4 µs | 111.7 µs | 0 errors |
| resv-by-user | 200 ops | 64.8 µs | 71.4 µs | 74.3 µs | 83.2 µs | 92.6 µs | 98.4 µs | 0 errors |
| books-by-author | 200 ops | 60.8 µs | 68.8 µs | 71.8 µs | 81.1 µs | 85.3 µs | 93.1 µs | 0 errors |
| books-by-genre | 200 ops | 74.1 µs | 82.8 µs | 86.3 µs | 99.2 µs | 107.1 µs | 117.8 µs | 0 errors |
| loans-by-daterange | 200 ops | 89.4 µs | 153.0 µs | 158.1 µs | 180.1 µs | 196.0 µs | 218.3 µs | 0 errors |
| overdue | 200 ops | 92.7 µs | 132.7 µs | 139.5 µs | 175.7 µs | 223.2 µs | 341.3 µs | 0 errors |
| join-loan-user | 200 ops | 89.2 µs | 96.9 µs | 101.2 µs | 129.0 µs | 163.8 µs | 234.6 µs | 0 errors |
| join-no-literal | 200 ops | 215.4 µs | 219.2 µs | 223.1 µs | 243.2 µs | 275.5 µs | 297.5 µs | 0 errors |
| exists-correlated | 200 ops | 150.3 µs | 154.1 µs | 156.7 µs | 239.1 µs | 397.9 µs | 470.8 µs | 0 errors |
| count-by-user | 200 ops | 70.3 µs | 72.7 µs | 74.1 µs | 87.1 µs | 115.4 µs | 160.4 µs | 0 errors |

#### `s3-c1-n1000-all` run 1

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 4 ops | 442.3 µs | 442.3 µs | 660.7 µs | 923.4 µs | 923.4 µs | 923.4 µs | 0 errors |
| load | 1,900 ops | 287.9 µs | 405.0 µs | 455.4 µs | 645.6 µs | 749.1 µs | 1,790.8 µs | 0 errors |
| create-index | 8 ops | 943.4 µs | 1,056.5 µs | 1,641.0 µs | 2,876.5 µs | 2,876.5 µs | 2,876.5 µs | 0 errors |
| pk-user | 200 ops | 54.9 µs | 57.2 µs | 57.9 µs | 65.3 µs | 87.0 µs | 350.8 µs | 0 errors |
| loans-by-user | 200 ops | 60.6 µs | 72.4 µs | 76.4 µs | 90.5 µs | 104.7 µs | 211.3 µs | 0 errors |
| loans-by-book | 200 ops | 60.6 µs | 69.0 µs | 73.2 µs | 87.5 µs | 99.6 µs | 152.4 µs | 0 errors |
| resv-by-user | 200 ops | 54.9 µs | 63.9 µs | 67.7 µs | 94.6 µs | 143.0 µs | 199.6 µs | 0 errors |
| books-by-author | 200 ops | 55.4 µs | 66.2 µs | 69.4 µs | 81.1 µs | 89.5 µs | 96.0 µs | 0 errors |
| books-by-genre | 200 ops | 76.3 µs | 101.4 µs | 117.8 µs | 182.7 µs | 261.6 µs | 291.0 µs | 0 errors |
| loans-by-daterange | 200 ops | 89.6 µs | 152.0 µs | 159.6 µs | 218.9 µs | 290.6 µs | 296.7 µs | 0 errors |
| overdue | 200 ops | 68.7 µs | 122.0 µs | 138.1 µs | 237.9 µs | 317.4 µs | 339.6 µs | 0 errors |
| join-loan-user | 200 ops | 69.1 µs | 78.4 µs | 81.4 µs | 97.5 µs | 106.7 µs | 131.3 µs | 0 errors |
| join-no-literal | 200 ops | 217.3 µs | 219.9 µs | 223.8 µs | 248.5 µs | 304.6 µs | 396.7 µs | 0 errors |
| exists-correlated | 200 ops | 123.0 µs | 126.6 µs | 129.3 µs | 160.7 µs | 184.0 µs | 242.3 µs | 0 errors |
| count-by-user | 200 ops | 51.0 µs | 55.1 µs | 56.4 µs | 63.3 µs | 72.3 µs | 91.3 µs | 0 errors |

#### `s3-c1-n10000-none` run 1

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 4 ops | 435.8 µs | 435.8 µs | 586.3 µs | 801.6 µs | 801.6 µs | 801.6 µs | 0 errors |
| load | 19,000 ops | 268.3 µs | 426.8 µs | 483.8 µs | 678.8 µs | 808.8 µs | 2,538.7 µs | 0 errors |
| pk-user | 200 ops | 56.5 µs | 58.7 µs | 59.6 µs | 69.4 µs | 90.4 µs | 240.7 µs | 0 errors |
| loans-by-user | 200 ops | 272.8 µs | 288.9 µs | 299.2 µs | 387.8 µs | 413.3 µs | 450.8 µs | 0 errors |
| loans-by-book | 200 ops | 269.1 µs | 284.3 µs | 293.5 µs | 367.5 µs | 401.0 µs | 407.3 µs | 0 errors |
| resv-by-user | 200 ops | 153.9 µs | 162.3 µs | 165.8 µs | 190.8 µs | 259.2 µs | 302.2 µs | 0 errors |
| books-by-author | 200 ops | 98.9 µs | 107.4 µs | 110.7 µs | 125.6 µs | 138.8 µs | 172.3 µs | 0 errors |
| books-by-genre | 200 ops | 301.0 µs | 347.8 µs | 367.8 µs | 455.4 µs | 535.8 µs | 587.4 µs | 0 errors |
| loans-by-daterange | 200 ops | 373.5 µs | 1,083.1 µs | 1,128.9 µs | 1,275.3 µs | 1,374.1 µs | 1,423.0 µs | 0 errors |
| overdue | 200 ops | 408.9 µs | 876.4 µs | 905.0 µs | 979.9 µs | 1,008.3 µs | 1,067.4 µs | 0 errors |
| join-loan-user | 200 ops | 283.2 µs | 293.8 µs | 304.3 µs | 397.1 µs | 453.2 µs | 516.8 µs | 0 errors |
| join-no-literal | 200 ops | 459.4 µs | 480.4 µs | 497.4 µs | 606.0 µs | 701.8 µs | 774.7 µs | 0 errors |
| exists-correlated | 200 ops | 574.5 µs | 598.5 µs | 616.1 µs | 708.2 µs | 756.7 µs | 923.6 µs | 0 errors |
| count-by-user | 200 ops | 262.0 µs | 269.0 µs | 278.4 µs | 345.9 µs | 379.7 µs | 405.6 µs | 0 errors |

#### `s3-c1-n10000-all` run 1

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 4 ops | 561.8 µs | 561.8 µs | 668.3 µs | 923.6 µs | 923.6 µs | 923.6 µs | 0 errors |
| load | 19,000 ops | 272.4 µs | 421.7 µs | 477.7 µs | 672.3 µs | 802.4 µs | 3,427.7 µs | 0 errors |
| create-index | 8 ops | 4,279.6 µs | 4,607.6 µs | 8,673.9 µs | 16,719.1 µs | 16,719.1 µs | 16,719.1 µs | 0 errors |
| pk-user | 200 ops | 55.4 µs | 57.9 µs | 58.9 µs | 67.5 µs | 87.2 µs | 325.9 µs | 0 errors |
| loans-by-user | 200 ops | 64.7 µs | 74.0 µs | 78.5 µs | 99.6 µs | 109.9 µs | 146.4 µs | 0 errors |
| loans-by-book | 200 ops | 57.9 µs | 68.7 µs | 73.6 µs | 88.6 µs | 96.5 µs | 106.7 µs | 0 errors |
| resv-by-user | 200 ops | 54.9 µs | 64.3 µs | 67.1 µs | 83.3 µs | 105.2 µs | 208.0 µs | 0 errors |
| books-by-author | 200 ops | 57.9 µs | 68.6 µs | 72.5 µs | 89.8 µs | 113.3 µs | 184.2 µs | 0 errors |
| books-by-genre | 200 ops | 300.9 µs | 337.1 µs | 360.2 µs | 500.2 µs | 531.3 µs | 590.3 µs | 0 errors |
| loans-by-daterange | 200 ops | 492.3 µs | 1,054.3 µs | 1,117.4 µs | 1,250.2 µs | 1,338.4 µs | 1,547.7 µs | 0 errors |
| overdue | 200 ops | 96.3 µs | 706.2 µs | 761.0 µs | 896.8 µs | 957.2 µs | 987.3 µs | 0 errors |
| join-loan-user | 200 ops | 74.0 µs | 82.3 µs | 85.8 µs | 101.4 µs | 134.6 µs | 185.2 µs | 0 errors |
| join-no-literal | 200 ops | 214.6 µs | 223.4 µs | 231.0 µs | 368.4 µs | 529.4 µs | 631.3 µs | 0 errors |
| exists-correlated | 200 ops | 125.6 µs | 128.4 µs | 130.0 µs | 155.9 µs | 169.1 µs | 223.5 µs | 0 errors |
| count-by-user | 200 ops | 52.8 µs | 56.1 µs | 57.6 µs | 64.8 µs | 73.5 µs | 97.3 µs | 0 errors |

## 7. Where the time goes

Reads on one connection, no writes after the load, no commit, no lock: the latency is a fixed round trip plus rows walked plus rows returned, with no durability or conflict wait to separate.

| Component | KDS now | Reading |
|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | **61.3 µs** (`pk-user` mean at 200 loans, median of three; 62.0 µs at 10,000) | recorded 65.7 µs; PostgreSQL recorded 84.5 µs |
| Durability / commit | not applicable | read-only shapes |
| Lock or conflict wait | none | one connection |

Cost per row walked by an unindexed equality (mean at 10,000 loans minus mean at 200, over the 9,800 extra rows of `loans`), and the cost of a returned row (`books-by-genre` returns about 125 rows):

| shape | mean n=200 | mean n=10,000 | per extra relation row now | v3.3.0-38 | PG |
|---|---|---|---|---|---|
| pk-user | 61.3 µs | 62.0 µs | 0.1 ns | -0.2 ns | 0.2 ns |
| loans-by-user | 76.4 µs | 313.9 µs | 24.2 ns | 33.3 ns | 34.4 ns |
| loans-by-book | 72.3 µs | 305.1 µs | 23.8 ns | 34.1 ns | 36.7 ns |
| books-by-genre | 66.3 µs | 364.7 µs | 30.4 ns | 27.8 ns | 27.0 ns |

**The walk costs 24 ns per examined row here, against 33 to 34 ns in the recorded run, and the A/B says the engine is the same: this host examines rows faster.** The recorded run's own reading was that the walk cost was at 24.7 ns in an earlier window and 33.3 ns in a later one on the same binary, a host effect of the same size; this run's 24.2 ns lands on the earlier window's figure, so the examined-row cost of the engine is about 24 ns when the host is the faster of the two and 33 ns when it is not. The returned-row term (`books-by-genre`, 30.4 ns per extra relation row against 27.8 ns) went the other way, which is the Python decoder on this CPU.

## 8. The engine, isolated: v3.3.0-38 against v3.4.14, interleaved on this host

The same driver against the `a40882af` binary and the measured binary, 10,000 loans unindexed (the cell where the walks are dearest), alternating old, new, old, new, old, new, medians and spreads over three runs:

| measure | v3.3.0-38 engine | spread | v3.4.14 engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s3 pk-user (10,000 loans, unindexed) | 16,103.1 qps | 8.1 % | 16,025.6 qps | 3.0 % | 1.00x | inside the spread |
| s3 loans-by-user (10,000 loans, unindexed) | 3,183.7 qps | 3.8 % | 3,230.0 qps | 3.7 % | 1.01x | inside the spread |
| s3 loans-by-book (10,000 loans, unindexed) | 3,193.9 qps | 13.9 % | 3,254.1 qps | 3.1 % | 1.02x | inside the spread |
| s3 resv-by-user (10,000 loans, unindexed) | 5,241.1 qps | 17.4 % | 5,984.4 qps | 2.3 % | 1.14x | inside the spread |
| s3 books-by-author (10,000 loans, unindexed) | 8,445.9 qps | 38.4 % | 8,873.1 qps | 2.7 % | 1.05x | spread > 20 %, no claim |
| s3 books-by-genre (10,000 loans, unindexed) | 2,791.0 qps | 5.2 % | 2,827.3 qps | 1.0 % | 1.01x | inside the spread |
| s3 loans-by-daterange (10,000 loans, unindexed) | 892.0 qps | 0.8 % | 893.2 qps | 2.2 % | 1.00x | inside the spread |
| s3 overdue (10,000 loans, unindexed) | 1,094.2 qps | 2.6 % | 1,113.0 qps | 1.3 % | 1.02x | inside the spread |
| s3 join-loan-user (10,000 loans, unindexed) | 3,140.7 qps | 4.2 % | 3,092.1 qps | 2.9 % | 0.98x | inside the spread |
| s3 join-no-literal (10,000 loans, unindexed) | 1,842.6 qps | 8.6 % | 1,919.0 qps | 1.5 % | 1.04x | inside the spread |
| s3 exists-correlated (10,000 loans, unindexed) | 1,600.5 qps | 5.9 % | 1,573.3 qps | 2.0 % | 0.98x | inside the spread |
| s3 count-by-user (10,000 loans, unindexed) | 3,379.5 qps | 3.8 % | 3,448.3 qps | 0.7 % | 1.02x | inside the spread |

**No shape moved outside its spread.** All twelve read 0.98x to 1.04x, except `resv-by-user` (1.14x with the old binary's spread at 17.4 %) and `books-by-author` (1.05x, old spread 38.4 %, no claim). The ratios disperse within about 4 %, while the rule's spreads reach 8.6 % on the old binary's `join-no-literal` and 8.1 % on its `pk-user`; by the rule only a ratio beyond those would be a finding, and none is. 432 commits and 214 source and header files lie between the two binaries; the read-side statements of this scenario cost the same.

## 9. The floor (PostgreSQL), not measured

No PostgreSQL server exists on this host (client tools only; the twin was attempted and every cell exited 1 on "connection refused"). The geometric means in section 3 divide KDS here by PostgreSQL 18.6 on the other host and read 1.00x to 1.33x at `cores = 1`; the recorded KDS/PG at 10,000 loans unindexed was 1.07x and is 1.25x now, a 17 % rise of which the scan-speed of the host accounts for most (the walks are +28 % to +38%). It supports "same league" and nothing finer; a result is neither good for beating the floor nor bad for not.

## 10. Noise floor, contamination and host

* Spread over three runs at `cores = 1`: median per cell 2.6 % to 8.2 % across the six cells of 12 shapes; 8 of 72 figures above 20 %. At `cores = 8`: median 2.8 % to 16.0 %; 9 of 72 above 20 %. The A/B's floor is about 4 %.
* **Contamination:** none; no cell was discarded. **One disclosure:** the table generator (one thread, 1 to 3 s) ran while `s3-c1-n1000-all` (19:10:52) and `ab-new1-s3-c1-n10000-none` (19:15 to 19:17) were live; below the monitor's 10 % line; neither is an outlier.
* No limit of the engine was approached by this scenario (serial reads on one connection; the client's round trip bounds the shapes under 100 µs).

## 11. What the run says about the engine

1. **Across 432 commits the read side of this scenario did not move.** Twelve shapes, 10,000 loans, the same A/B floor of about 4 %: nothing in KDS's walk, descent, join or returned-row path got dearer.
2. **The scan cost of a row is a host-weather figure on this suite**: 24 ns here, 33 ns recorded on the same binary, both within the same 1.4x as the earlier window of the recorded run; any claim about "KDS's examined-row cost against PostgreSQL's" needs the two engines in one window on one host, which this run could not give (no PostgreSQL server).
3. **An index buys 3.6x to 4.9x on an equality at 10,000 loans and nothing at 200**, on this engine as on the recorded PostgreSQL; the cost of a short statement is the round trip (61 µs of fixed cost), so the structure's value arrives only when the walk is longer than the round trip.

## 12. Archive

`bench/v3.4.14/archive/scenario3-v3.4.14/`: per-cell JSON, driver stdout, server configs. What the scenarios share is in `bench/v3.4.14/archive/run-v3.4.14/`. No data file is kept.
