# Scenario 3 (library) at `v2.7.0-531-g9a0525d`

**One line.** A secondary index turns a non-pk equality read from a
per-row cost into a constant, and the constant is one point read: at 10,000
loans `loans-by-user` runs 10,881 qps with the index against 1,277 qps
without (8.52x), while at 200 loans the two are 1.10x apart and the pk
control does not move (13,661 to 14,388 qps across all six cells). An
unindexed equality costs about 84 us fixed plus 70 ns per row of the
relation; an indexed one costs about 90 us at any size.

Driver: `tools/scenario3_library.py`, unmodified. **No v3 predecessor: no
delta.** Nothing under `bench/v3.0.0/` at `9a0525d` measured this driver,
so this file is the baseline the next scenario3 run is read against.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-09-30; run 1 06:49:36 to 06:52:10 UTC, run 2 07:02:15 to 07:04:45, run 3 07:04:45 to 07:07:19 (per-cell times in section 7) |
| Worktree / branch | `bench-rerun-scenarios` / `worktree-bench-rerun-scenarios` |
| HEAD | `fc2d343`, which over `9a0525d` only deletes `bench/v3.0.0/` and edits `bench/README.md`; `src/`, `include/`, `tools/` and `CMakeLists.txt` are identical |
| Engine commit measured | `9a0525d`, `git describe --tags` = `v2.7.0-531-g9a0525d` ("AY-S11: AY closed") |
| Tree cleanliness | Clean at the start; nothing edited or built during the cells |
| Binary provenance | `/home/cdkbs/bench-runs/rebaseline-9a0525d/kds_server`, copy of `build-release/kds_server`, **sha256 `1d07d33386541ec3a13090d37918e229957e29c2c777ff15f2a63db6957f8746`**, source binary mtime 2026-09-30 06:30:43 UTC (after `9a0525d` at 06:19:05 and `fc2d343` at 06:29:21); every server started from the copy |
| Device | data files under `/home/cdkbs/bench-runs/rebaseline-9a0525d/data/`, `/dev/root`, `ext4` (`df -T`). Not tmpfs. Fresh data file per cell, deleted after |
| Build type | Release (`CMakeCache.txt`), built to completion before the first cell |
| Host | 8 logical CPUs, AMD EPYC 9V74, 1 socket x 4 cores x 2 threads, Linux 7.0.0-1014-azure |
| Server config | `log_level = warn`, `durability = group`, `cores` 1 or 8 per cell, `indexes` at its default (on; recorded as `--server-indexes on`), all else default; port 15620, never 15432 |
| PostgreSQL | not installed on this host; the floor was not measured for this shape |

## 2. What was run

Thirty-six cells: `cores` {1, 8} x `--loans` {200, 1000, 10000} x
`--index-mode` {none, all}, **three runs each**. Every cell had a fresh
server and data file and ran `--seed 1 --matches 5 --ops 200 --verify 25`
(the defaults), `--index-when after` (the default: indexes declared on the
loaded relations, which times the backfill), and for `all` also
`--assert-index-reads`, which fails the run if a shape that is indexed did
not compile to `IndexProbe` / `IndexRange`. Command:

    python3 tools/scenario3_library.py --port 15620 --suffix <cell> --loans <n> \
        --index-mode <none|all> --server-indexes on --seed 1 [--assert-index-reads] --json <cell>.json

**Row-set axis (rule 9), stated once.** `--loans N` is the bulk relation; users and
books scale to hold `--matches 5` constant. At `N = 10,000`: loans 10,000,
users 2,000, books 2,000, reservations 5,000; at 200 and 1,000 the same
ratios. All three sizes are columns of every matrix below.

**All 36 cells exited 0, all 36 `verify` runs reported no problems, and all
18 `all` cells passed `--assert-index-reads`.** Durability does not enter a
read shape; the load, which is where `group` acts, is in section 5. Host
load and build check per cell: section 7. No build or test process was
live at any cell. `--cabin` and the other `--index-mode` values
(`single`, `composite`, `covering`) were **not executed**: `all` declares
eight indexes including composite and covering ones, and the cross-product
was left out to keep the matrix at three sizes by two modes by two core
counts by three runs.

## 3. Throughput

A serial single-connection driver reports no qps; each cell below is
**derived**: 1,000,000 / mean us, then the median of three runs.

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 13,928 qps | 13,774 qps | 13,661 qps | 13,831 qps | 14,025 qps | 14,388 qps |
| loans-by-user | 10,173 qps | 11,186 qps | 6,443 qps | 11,074 qps | 1,277 qps | 10,881 qps |
| loans-by-book | 10,730 qps | 11,614 qps | 6,739 qps | 11,601 qps | 1,270 qps | 11,737 qps |
| resv-by-user | 12,706 qps | 13,175 qps | 9,225 qps | 13,004 qps | 2,341 qps | 12,970 qps |
| books-by-author | 12,674 qps | 11,919 qps | 8,278 qps | 12,210 qps | 4,257 qps | 11,792 qps |
| books-by-genre | 13,298 qps | 13,316 qps | 7,570 qps | 9,285 qps | 1,646 qps | 2,101 qps |
| loans-by-daterange | 9,398 qps | 9,355 qps | 3,814 qps | 3,855 qps | 492 qps | 514 qps |
| overdue | 10,288 qps | 11,325 qps | 4,407 qps | 6,431 qps | 606 qps | 1,070 qps |
| join-loan-user | 9,242 qps | 10,091 qps | 6,135 qps | 10,070 qps | 1,257 qps | 10,132 qps |
| join-no-literal | 3,798 qps | 3,686 qps | 2,847 qps | 3,457 qps | 779 qps | 3,608 qps |
| exists-correlated | 7,788 qps | 7,057 qps | 5,631 qps | 6,215 qps | 1,564 qps | 7,128 qps |
| count-by-user | 13,038 qps | 14,599 qps | 7,358 qps | 14,430 qps | 1,323 qps | 14,306 qps |

The same at `cores = 8` (derived, median of three):

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 13,038 qps | 13,123 qps | 12,987 qps | 13,089 qps | 13,569 qps | 13,643 qps |
| loans-by-user | 9,560 qps | 10,787 qps | 5,565 qps | 10,638 qps | 1,206 qps | 10,299 qps |
| loans-by-book | 10,070 qps | 11,364 qps | 5,914 qps | 11,287 qps | 1,212 qps | 11,111 qps |
| resv-by-user | 12,034 qps | 12,987 qps | 8,354 qps | 12,516 qps | 2,353 qps | 12,392 qps |
| books-by-author | 12,048 qps | 11,976 qps | 9,766 qps | 11,848 qps | 4,037 qps | 11,429 qps |
| books-by-genre | 12,706 qps | 12,626 qps | 8,032 qps | 8,850 qps | 1,686 qps | 2,050 qps |
| loans-by-daterange | 8,850 qps | 8,764 qps | 3,551 qps | 3,577 qps | 493 qps | 488 qps |
| overdue | 9,606 qps | 11,148 qps | 4,034 qps | 5,831 qps | 581 qps | 1,019 qps |
| join-loan-user | 8,432 qps | 9,569 qps | 5,391 qps | 9,606 qps | 1,194 qps | 9,823 qps |
| join-no-literal | 3,530 qps | 3,407 qps | 2,587 qps | 3,289 qps | 738 qps | 3,338 qps |
| exists-correlated | 7,220 qps | 6,583 qps | 5,118 qps | 6,588 qps | 1,499 qps | 6,653 qps |
| count-by-user | 12,077 qps | 14,006 qps | 6,623 qps | 14,144 qps | 1,238 qps | 13,532 qps |

`cores = 8` reads 0.86x to 1.18x of `cores = 1` (median 0.95x over the 72
cells) and is the slower of the two in 65 of them; a single serial
connection lands on one core and the other seven have nothing to do, so no
gain is expected. Each cell's difference (9,560 against 10,173 qps for
`loans-by-user` at 200 rows, 1,206 against 1,277 at 10,000) is inside the
noise below, but the sign is consistent: about 5 % is lost at eight
cores, and this run does not say where.

**The index's effect** (none over all, ratio of mean latency, `cores = 1`;
above 1 means the index is faster):

| shape | n=200 | n=1,000 | n=10,000 |
|---|---|---|---|
| pk-user | 0.99x | 1.01x | 1.03x |
| loans-by-user | 1.10x | 1.72x | 8.52x |
| loans-by-book | 1.08x | 1.72x | 9.24x |
| resv-by-user | 1.04x | 1.41x | 5.54x |
| books-by-author | 0.94x | 1.47x | 2.77x |
| books-by-genre | 1.00x | 1.23x | 1.28x |
| loans-by-daterange | 1.00x | 1.01x | 1.05x |
| overdue | 1.10x | 1.46x | 1.77x |
| join-loan-user | 1.09x | 1.64x | 8.06x |
| join-no-literal | 0.97x | 1.21x | 4.63x |
| exists-correlated | 0.91x | 1.10x | 4.56x |
| count-by-user | 1.12x | 1.96x | 10.81x |

The pk control (`pk-user`, a `Lookup`) reads 0.99x to 1.03x and is the floor
of this table: **a ratio inside about 1.00 +/- 0.05 is not a finding**, so
`books-by-author` 0.94x, `join-no-literal` 0.97x and `exists-correlated`
0.91x at 200 rows are the index costing nothing that can be seen.

## 4. Percentiles

Run 1 of the `cores = 1` cells, in full: 200 operations per shape.
Microseconds; `plan` is what ANALYZE reported the shape compiled to. Run 1
of `n=1,000 all` met the host's slow mode (section 7): its `pk-user` p50 is
102.3 us against about 70 us elsewhere, and runs 2 and 3 of that cell read
72.3 us mean; section 3's medians vote it out, this table does not.

| n | mode | shape | p0 | p25 | p50 | p95 | p99 | plan |
|---|---|---|---|---|---|---|---|---|
| 200 | none | pk-user | 64.6 µs | 66.4 µs | 68.3 µs | 85.9 µs | 101.9 µs | - |
| 200 | none | loans-by-user | 77.1 µs | 89.7 µs | 97.1 µs | 117.7 µs | 122.4 µs | FilterScan |
| 200 | none | loans-by-book | 78.8 µs | 86.8 µs | 92.7 µs | 108.7 µs | 117.3 µs | FilterScan |
| 200 | none | resv-by-user | 67.4 µs | 74.3 µs | 77.0 µs | 91.3 µs | 96.8 µs | FilterScan |
| 200 | none | books-by-author | 67.1 µs | 71.5 µs | 77.3 µs | 97.4 µs | 101.8 µs | FilterScan |
| 200 | none | books-by-genre | 67.0 µs | 70.6 µs | 73.5 µs | 88.3 µs | 102.2 µs | FilterScan |
| 200 | none | loans-by-daterange | 87.7 µs | 97.9 µs | 104.6 µs | 127.2 µs | 132.9 µs | Scan |
| 200 | none | overdue | 79.6 µs | 92.3 µs | 96.2 µs | 114.5 µs | 118.7 µs | FilterScan |
| 200 | none | join-loan-user | 83.2 µs | 99.3 µs | 105.3 µs | 127.8 µs | 140.4 µs | - |
| 200 | none | join-no-literal | 249.7 µs | 255.4 µs | 261.9 µs | 282.3 µs | 294.5 µs | Range,Scan |
| 200 | none | exists-correlated | 122.8 µs | 124.9 µs | 126.3 µs | 142.5 µs | 149.6 µs | Range,Scan |
| 200 | none | count-by-user | 73.6 µs | 74.7 µs | 75.6 µs | 90.5 µs | 94.9 µs | - |
| 200 | all | pk-user | 65.5 µs | 67.6 µs | 69.0 µs | 88.3 µs | 98.9 µs | - |
| 200 | all | loans-by-user | 66.7 µs | 81.3 µs | 88.1 µs | 107.5 µs | 119.8 µs | IndexProbe |
| 200 | all | loans-by-book | 68.5 µs | 77.8 µs | 84.2 µs | 104.8 µs | 115.6 µs | IndexProbe |
| 200 | all | resv-by-user | 62.4 µs | 70.6 µs | 73.7 µs | 86.3 µs | 105.0 µs | IndexProbe |
| 200 | all | books-by-author | 67.8 µs | 72.6 µs | 82.3 µs | 226.1 µs | 356.5 µs | IndexProbe |
| 200 | all | books-by-genre | 67.7 µs | 102.0 µs | 108.2 µs | 128.8 µs | 135.8 µs | IndexProbe |
| 200 | all | loans-by-daterange | 119.9 µs | 132.3 µs | 138.5 µs | 168.9 µs | 176.8 µs | Scan |
| 200 | all | overdue | 97.7 µs | 116.6 µs | 121.3 µs | 144.2 µs | 149.7 µs | IndexRange |
| 200 | all | join-loan-user | 70.9 µs | 106.6 µs | 133.2 µs | 162.2 µs | 178.8 µs | - |
| 200 | all | join-no-literal | 259.6 µs | 264.2 µs | 269.3 µs | 287.8 µs | 298.1 µs | Range,IndexProbe |
| 200 | all | exists-correlated | 133.2 µs | 134.6 µs | 135.7 µs | 153.2 µs | 157.5 µs | Range,IndexProbe |
| 200 | all | count-by-user | 62.1 µs | 65.1 µs | 66.2 µs | 80.2 µs | 83.6 µs | - |
| 1,000 | none | pk-user | 65.2 µs | 67.1 µs | 69.9 µs | 84.9 µs | 96.7 µs | - |
| 1,000 | none | loans-by-user | 132.1 µs | 145.9 µs | 153.3 µs | 170.3 µs | 178.8 µs | FilterScan |
| 1,000 | none | loans-by-book | 133.4 µs | 142.1 µs | 147.1 µs | 161.7 µs | 165.0 µs | FilterScan |
| 1,000 | none | resv-by-user | 95.0 µs | 104.1 µs | 113.7 µs | 154.6 µs | 169.2 µs | FilterScan |
| 1,000 | none | books-by-author | 111.9 µs | 123.3 µs | 128.1 µs | 147.6 µs | 154.6 µs | FilterScan |
| 1,000 | none | books-by-genre | 132.8 µs | 147.7 µs | 152.3 µs | 176.7 µs | 184.3 µs | FilterScan |
| 1,000 | none | loans-by-daterange | 147.9 µs | 248.4 µs | 263.4 µs | 321.5 µs | 367.9 µs | Scan |
| 1,000 | none | overdue | 151.5 µs | 215.0 µs | 223.2 µs | 244.4 µs | 256.2 µs | FilterScan |
| 1,000 | none | join-loan-user | 137.0 µs | 155.4 µs | 161.0 µs | 183.5 µs | 196.0 µs | - |
| 1,000 | none | join-no-literal | 334.6 µs | 340.6 µs | 345.1 µs | 358.8 µs | 365.2 µs | Range,Scan |
| 1,000 | none | exists-correlated | 167.7 µs | 170.5 µs | 172.2 µs | 194.3 µs | 207.6 µs | Range,Scan |
| 1,000 | none | count-by-user | 126.2 µs | 127.5 µs | 128.4 µs | 141.7 µs | 144.5 µs | - |
| 1,000 | all | pk-user | 96.3 µs | 99.1 µs | 102.3 µs | 126.5 µs | 132.5 µs | - |
| 1,000 | all | loans-by-user | 99.9 µs | 119.2 µs | 126.7 µs | 150.5 µs | 160.2 µs | IndexProbe |
| 1,000 | all | loans-by-book | 102.4 µs | 114.6 µs | 120.0 µs | 140.8 µs | 150.0 µs | IndexProbe |
| 1,000 | all | resv-by-user | 63.6 µs | 73.2 µs | 78.6 µs | 120.8 µs | 134.8 µs | IndexProbe |
| 1,000 | all | books-by-author | 63.9 µs | 75.6 µs | 80.6 µs | 94.1 µs | 104.0 µs | IndexProbe |
| 1,000 | all | books-by-genre | 87.5 µs | 101.6 µs | 106.9 µs | 130.9 µs | 136.3 µs | IndexProbe |
| 1,000 | all | loans-by-daterange | 147.3 µs | 247.4 µs | 257.7 µs | 290.1 µs | 319.6 µs | Scan |
| 1,000 | all | overdue | 69.1 µs | 145.2 µs | 154.8 µs | 179.7 µs | 186.0 µs | IndexRange |
| 1,000 | all | join-loan-user | 72.4 µs | 89.1 µs | 95.3 µs | 116.9 µs | 131.0 µs | - |
| 1,000 | all | join-no-literal | 269.4 µs | 273.2 µs | 277.8 µs | 289.8 µs | 299.7 µs | Range,IndexProbe |
| 1,000 | all | exists-correlated | 137.7 µs | 153.5 µs | 178.6 µs | 201.2 µs | 209.8 µs | Range,IndexProbe |
| 1,000 | all | count-by-user | 95.3 µs | 99.5 µs | 101.1 µs | 121.2 µs | 127.6 µs | - |
| 10,000 | none | pk-user | 65.4 µs | 66.8 µs | 67.9 µs | 86.5 µs | 109.2 µs | - |
| 10,000 | none | loans-by-user | 751.8 µs | 768.5 µs | 775.4 µs | 793.6 µs | 805.7 µs | FilterScan |
| 10,000 | none | loans-by-book | 726.0 µs | 747.3 µs | 757.0 µs | 787.0 µs | 815.6 µs | FilterScan |
| 10,000 | none | resv-by-user | 395.4 µs | 437.0 µs | 445.5 µs | 469.1 µs | 478.4 µs | FilterScan |
| 10,000 | none | books-by-author | 219.8 µs | 259.6 µs | 268.1 µs | 294.0 µs | 306.2 µs | FilterScan |
| 10,000 | none | books-by-genre | 560.8 µs | 593.3 µs | 605.0 µs | 640.3 µs | 652.2 µs | FilterScan |
| 10,000 | none | loans-by-daterange | 889.8 µs | 1,933.1 µs | 1,981.7 µs | 2,086.8 µs | 2,429.9 µs | Scan |
| 10,000 | none | overdue | 902.3 µs | 1,643.3 µs | 1,679.3 µs | 1,778.1 µs | 1,799.2 µs | FilterScan |
| 10,000 | none | join-loan-user | 748.3 µs | 785.0 µs | 794.9 µs | 821.1 µs | 845.4 µs | - |
| 10,000 | none | join-no-literal | 1,240.3 µs | 1,271.8 µs | 1,281.3 µs | 1,310.5 µs | 1,398.5 µs | Range,Scan |
| 10,000 | none | exists-correlated | 603.8 µs | 628.7 µs | 638.0 µs | 661.4 µs | 705.0 µs | Range,Scan |
| 10,000 | none | count-by-user | 721.5 µs | 742.7 µs | 752.6 µs | 766.2 µs | 777.2 µs | - |
| 10,000 | all | pk-user | 64.2 µs | 65.4 µs | 66.5 µs | 80.9 µs | 90.0 µs | - |
| 10,000 | all | loans-by-user | 70.5 µs | 83.6 µs | 90.2 µs | 111.3 µs | 118.7 µs | IndexProbe |
| 10,000 | all | loans-by-book | 64.3 µs | 78.5 µs | 83.6 µs | 100.7 µs | 115.2 µs | IndexProbe |
| 10,000 | all | resv-by-user | 61.3 µs | 71.5 µs | 75.3 µs | 88.5 µs | 96.8 µs | IndexProbe |
| 10,000 | all | books-by-author | 63.2 µs | 76.5 µs | 81.4 µs | 100.9 µs | 113.7 µs | IndexProbe |
| 10,000 | all | books-by-genre | 409.7 µs | 444.2 µs | 457.3 µs | 492.7 µs | 499.3 µs | IndexProbe |
| 10,000 | all | loans-by-daterange | 867.5 µs | 1,910.4 µs | 1,964.5 µs | 2,062.7 µs | 2,102.2 µs | Scan |
| 10,000 | all | overdue | 82.8 µs | 918.9 µs | 972.8 µs | 1,077.5 µs | 1,123.7 µs | IndexRange |
| 10,000 | all | join-loan-user | 79.2 µs | 106.2 µs | 131.0 µs | 158.6 µs | 163.9 µs | - |
| 10,000 | all | join-no-literal | 267.3 µs | 271.9 µs | 275.9 µs | 288.2 µs | 291.6 µs | Range,IndexProbe |
| 10,000 | all | exists-correlated | 135.3 µs | 136.6 µs | 137.5 µs | 152.6 µs | 154.4 µs | Range,IndexProbe |
| 10,000 | all | count-by-user | 63.2 µs | 67.1 µs | 68.7 µs | 79.4 µs | 87.2 µs | - |

## 5. Where the time goes

A read has no durability wait and no commit; the decomposition is fixed
round trip plus engine work that scales with the rows walked.

| Component | Estimate | How derived |
|---|---|---|
| Fixed cost: client, socket, parse, one pk descent | about 84 us for a scan's intercept, 64 us at p0 for a pk lookup, 76 to 92 us (mean, median of three) for an indexed equality shape | `pk-user` p0 (64.6 us at `n=200 none`); the two-point fit below; indexed `loans-by-user` p50 (88.1 us at `n=200 all`) |
| Rows walked | about **70 ns per row** of the relation for an unindexed equality | `loans-by-user none` mean 98.3 us at 200 loans (10,173 qps) and 783 us at 10,000 (1,277 qps): (783 - 98) / 9,800 = 70 ns per row, intercept 84 us; the 1,000-loan cell then predicts 154 us and measures 155 us (6,443 qps) |
| Index probe | about 20 us over a pk lookup, flat in n | indexed `loans-by-user` at 10,881 qps (92 us) against `pk-user` at 14,388 qps (70 us) |
| Lock / commit / durability | not applicable: read-only statements on one connection | |
| Backfill (`create-index`, one-shot, not a matrix row) | see the table below | |

| n | mode | load ops | load p50 | load mean | create-index ops | create-index p50 | create-index mean |
|---|---|---|---|---|---|---|---|
| 200 | none | 380 ops | 1,415 µs | 2,415 µs | not run | - | - |
| 200 | all | 380 ops | 1,388 µs | 1,782 µs | 8 ops | 1,397 µs | 1,537 µs |
| 1,000 | none | 1,900 ops | 1,334 µs | 1,535 µs | not run | - | - |
| 1,000 | all | 1,900 ops | 1,352 µs | 1,520 µs | 8 ops | 2,344 µs | 2,566 µs |
| 10,000 | none | 19,000 ops | 1,347 µs | 1,517 µs | not run | - | - |
| 10,000 | all | 19,000 ops | 1,364 µs | 1,569 µs | 8 ops | 10,431 µs | 8,573 µs |

The load is 380 / 1,900 / 19,000 individual inserts at a p50 of about 1.4
ms each: one durability point per statement under `group`, the same
autocommit wait that scenario0 measures.

## 6. Correctness and shape

`--verify 25` checks the five invariants over a sample of 25 and found no
problem in any of the 36 cells. `--assert-index-reads` passed in the 18
indexed cells: `loans-by-user`, `loans-by-book`, `resv-by-user`,
`books-by-author` and `books-by-genre` compile to `IndexProbe`, `overdue` to
`IndexRange`, and `join-no-literal` and `exists-correlated` to
`Range, IndexProbe` (ANALYZE reports no plan for `join-loan-user`).

Three shapes the indexes do not fix, and why:

- **`loans-by-daterange`** compiles to `Scan` even with all eight indexes
  declared: no index covers `loaned_day`. 492 to 514 qps at 10,000 rows in
  both modes (1.05x).
- **`books-by-genre`** is indexed (`IndexProbe`) but returns a sixteenth of
  the relation: at 10,000 loans 125 of 2,000 books qualify, so the walk of
  matches, not the search, is the cost. 1.28x.
- **`overdue`** (`IndexRange` over `(status, due_day)`) is 1.77x at 10,000: a
  range over a composite key returns many rows.

## 7. Noise floor and host stamps

Three runs per cell: run-to-run spread of each shape's mean (max / min - 1),
summarised over the twelve read shapes of a cell:

| cores | n | mode | median spread over shapes | worst shape | worst spread |
|---|---|---|---|---|---|
| 1 | 200 | none | 5.8 % | books-by-genre | 45.4 % |
| 1 | 200 | all | 21.1 % | overdue | 40.9 % |
| 1 | 1,000 | none | 4.5 % | books-by-author | 37.0 % |
| 1 | 1,000 | all | 31.4 % | count-by-user | 52.2 % |
| 1 | 10,000 | none | 6.4 % | pk-user | 46.1 % |
| 1 | 10,000 | all | 4.9 % | exists-correlated | 43.5 % |
| 8 | 200 | none | 32.7 % | pk-user | 40.8 % |
| 8 | 200 | all | 22.4 % | books-by-author | 40.3 % |
| 8 | 1,000 | none | 8.2 % | resv-by-user | 24.3 % |
| 8 | 1,000 | all | 1.7 % | overdue | 4.9 % |
| 8 | 10,000 | none | 1.6 % | exists-correlated | 4.2 % |
| 8 | 10,000 | all | 3.2 % | loans-by-user | 89.2 % |

**The floor is large and heavy-tailed.** The median shape moves 1.6 to 8.2
% between runs in eight of the twelve (cores, n, mode) groups, but four
groups sit at 21 to 33 %, and in every group the worst single shape is 4 to
89 % (`pk-user`, the control, reaches 46 % at 10,000 rows, `cores = 1`, and 41 %
at 200 rows, `cores = 8`, both without an index). The host has a slow mode (the same one AY-S11 recorded as
51 us against 75 us for identical statements), and a run that meets it
moves a whole cell. Consequences: the matrices above are medians of three
so a slow run is voted out; **a difference under about 10 % in one cell is
not a result**; the 8.5x, 9.2x and 10.8x index effects at 10,000 rows and
the 1.4x to 2.0x at 1,000 rows on the same shapes are far outside it.

Every cell's precheck, driver exit and verify count:

| cell | precheck UTC | loadavg | build procs | driver exit | verify problems |
|---|---|---|---|---|---|
| `s3-c1-n200-none` | 2026-09-30T06:49:36Z | 1.90 / 1.70 / 1.54 | none | exit 0 | 0 problems |
| `s3-c1-n200-all` | 2026-09-30T06:49:38Z | 1.90 / 1.70 / 1.54 | none | exit 0 | 0 problems |
| `s3-c1-n1000-none` | 2026-09-30T06:49:40Z | 1.90 / 1.70 / 1.54 | none | exit 0 | 0 problems |
| `s3-c1-n1000-all` | 2026-09-30T06:49:44Z | 1.90 / 1.70 / 1.54 | none | exit 0 | 0 problems |
| `s3-c1-n10000-none` | 2026-09-30T06:49:48Z | 1.83 / 1.69 / 1.54 | none | exit 0 | 0 problems |
| `s3-c1-n10000-all` | 2026-09-30T06:50:20Z | 1.56 / 1.64 / 1.52 | none | exit 0 | 0 problems |
| `s3-c8-n200-none` | 2026-09-30T06:50:52Z | 1.47 / 1.62 / 1.52 | none | exit 0 | 0 problems |
| `s3-c8-n200-all` | 2026-09-30T06:50:54Z | 1.47 / 1.62 / 1.52 | none | exit 0 | 0 problems |
| `s3-c8-n1000-none` | 2026-09-30T06:50:55Z | 1.43 / 1.60 / 1.52 | none | exit 0 | 0 problems |
| `s3-c8-n1000-all` | 2026-09-30T06:50:59Z | 1.43 / 1.60 / 1.52 | none | exit 0 | 0 problems |
| `s3-c8-n10000-none` | 2026-09-30T06:51:04Z | 1.32 / 1.58 / 1.51 | none | exit 0 | 0 problems |
| `s3-c8-n10000-all` | 2026-09-30T06:51:37Z | 1.54 / 1.61 / 1.52 | none | exit 0 | 0 problems |
| `r2-s3-c1-n200-none` | 2026-09-30T07:02:15Z | 0.22 / 0.42 / 0.92 | none | exit 0 | 0 problems |
| `r2-s3-c1-n200-all` | 2026-09-30T07:02:17Z | 0.36 / 0.45 / 0.93 | none | exit 0 | 0 problems |
| `r2-s3-c1-n1000-none` | 2026-09-30T07:02:18Z | 0.36 / 0.45 / 0.93 | none | exit 0 | 0 problems |
| `r2-s3-c1-n1000-all` | 2026-09-30T07:02:22Z | 0.41 / 0.46 / 0.93 | none | exit 0 | 0 problems |
| `r2-s3-c1-n10000-none` | 2026-09-30T07:02:26Z | 0.46 / 0.47 / 0.93 | none | exit 0 | 0 problems |
| `r2-s3-c1-n10000-all` | 2026-09-30T07:02:59Z | 0.75 / 0.54 / 0.94 | none | exit 0 | 0 problems |
| `r2-s3-c8-n200-none` | 2026-09-30T07:03:29Z | 0.92 / 0.60 / 0.95 | none | exit 0 | 0 problems |
| `r2-s3-c8-n200-all` | 2026-09-30T07:03:31Z | 1.01 / 0.62 / 0.95 | none | exit 0 | 0 problems |
| `r2-s3-c8-n1000-none` | 2026-09-30T07:03:33Z | 1.01 / 0.62 / 0.95 | none | exit 0 | 0 problems |
| `r2-s3-c8-n1000-all` | 2026-09-30T07:03:37Z | 1.17 / 0.66 / 0.96 | none | exit 0 | 0 problems |
| `r2-s3-c8-n10000-none` | 2026-09-30T07:03:41Z | 1.23 / 0.68 / 0.97 | none | exit 0 | 0 problems |
| `r2-s3-c8-n10000-all` | 2026-09-30T07:04:14Z | 1.68 / 0.84 / 1.02 | none | exit 0 | 0 problems |
| `r3-s3-c1-n200-none` | 2026-09-30T07:04:45Z | 1.64 / 0.94 / 1.04 | none | exit 0 | 0 problems |
| `r3-s3-c1-n200-all` | 2026-09-30T07:04:47Z | 1.64 / 0.94 / 1.04 | none | exit 0 | 0 problems |
| `r3-s3-c1-n1000-none` | 2026-09-30T07:04:49Z | 1.64 / 0.94 / 1.04 | none | exit 0 | 0 problems |
| `r3-s3-c1-n1000-all` | 2026-09-30T07:04:53Z | 1.83 / 0.99 / 1.06 | none | exit 0 | 0 problems |
| `r3-s3-c1-n10000-none` | 2026-09-30T07:04:57Z | 1.77 / 0.99 / 1.06 | none | exit 0 | 0 problems |
| `r3-s3-c1-n10000-all` | 2026-09-30T07:05:29Z | 1.46 / 0.99 / 1.05 | none | exit 0 | 0 problems |
| `r3-s3-c8-n200-none` | 2026-09-30T07:06:01Z | 1.39 / 1.03 / 1.06 | none | exit 0 | 0 problems |
| `r3-s3-c8-n200-all` | 2026-09-30T07:06:03Z | 1.39 / 1.03 / 1.06 | none | exit 0 | 0 problems |
| `r3-s3-c8-n1000-none` | 2026-09-30T07:06:05Z | 1.39 / 1.03 / 1.06 | none | exit 0 | 0 problems |
| `r3-s3-c8-n1000-all` | 2026-09-30T07:06:10Z | 1.44 / 1.04 / 1.07 | none | exit 0 | 0 problems |
| `r3-s3-c8-n10000-none` | 2026-09-30T07:06:15Z | 1.48 / 1.06 / 1.07 | none | exit 0 | 0 problems |
| `r3-s3-c8-n10000-all` | 2026-09-30T07:06:48Z | 1.66 / 1.15 / 1.10 | none | exit 0 | 0 problems |

Loadavg (one-minute) at the precheck reads 0.22 to 1.90, the tail of the previous cell's
own client and server; no `cc1plus`, `cmake --build` or `ctest` process was
live at any cell. The first `s3` pass followed the scenario0 and
scenario2 passes with no idle gap.

## 8. What the run says about the engine

1. **The secondary index pays for itself from about 1,000 rows and is
   worth 5x to 11x at 10,000.** At 200 rows a scan costs about 27 us over the
   floor and the index is at most 1.12x faster, inside the noise; at 1,000
   rows the ratio is 1.2x to 2.0x on the indexed equality shapes; at 10,000
   it is 5.5x to 10.8x for `loans-by-user`, `loans-by-book`, `resv-by-user`
   and `count-by-user`, 2.8x for `books-by-author` and 1.3x for
   `books-by-genre`. The two `books` shapes gain less because the walk they
   replace is shorter (`books` holds 2,000 rows against `loans`' 10,000 and
   `reservations`' 5,000), and `books-by-genre` also returns 125 of those
   rows; `books-by-author` returns about four, fewer than `loans-by-user`'s
   five. `join-loan-user` is 8.1x and the two literal-free joins 4.6x.
2. **The index makes a non-pk equality independent of the relation's size.**
   Indexed `loans-by-user` is 11,186 / 11,074 / 10,881 qps at 200 / 1,000 /
   10,000 rows, a 2.8 % spread across a 50x change in size, inside the
   noise; unindexed it is 10,173 / 6,443 / 1,277 qps.
3. **The residual cost of an indexed read is the round trip.** 90 us
   against 70 us for a pk `Lookup`; the extra 20 us is presumably the index
   descent and the main-tree fetch (not separated here). Below that there is nothing left for an index to
   remove; the next gain is not in the index.
4. **Reads do not scale with `cores` from one connection**, as expected,
   and run about 5 % slower at eight (section 3). This scenario does not
   exercise the multi-core read path.
5. **Backfill is cheap and grows with the row count**: the eight
   `create-index` statements take 1.4 ms / 2.3 ms / 10.4 ms at p50 at 200 /
   1,000 / 10,000 loans.

Archive: `bench/v3.0.0/archive/scenario3-v2.7.0-531-g9a0525d/`: per-cell
JSON, driver stdout, prechecks, server logs, configs, run scripts, the
summariser and the timeline.
