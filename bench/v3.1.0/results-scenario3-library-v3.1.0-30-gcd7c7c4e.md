# Scenario 3 (library) at `v3.1.0-30-gcd7c7c4e`

**One line.** Reading a row costs 2.4x to 2.8x less than it did at `9a0525d`:
an unindexed non-pk equality now costs about 27 ns per row walked (it cost
70) plus an unchanged fixed 58 to 87 µs, so at 10,000 loans the unindexed
`loans-by-user` runs 2,779 qps against 1,277 qps
(2.18x, cross-version) while the
indexed read is where it was (11,223 against 10,881 qps, 1.03x), so
**the index's gain at 10,000 rows shrank from 8.5x to 4.0x** because the thing it
saves became cheaper. Shapes that are not a plain equality (range
predicates, the join without a literal, the correlated `EXISTS`) gain only 1.0x
to 1.5x per row.

Driver: `tools/scenario3_library.py`, unmodified, 36 cells, three runs each,
the 2026-09-30 rebaseline's arguments. Comparator (cross-version):
`bench/v3.0.0/results-scenario3-library-v2.7.0-531-g9a0525d.md`, held at
`36449d28`. PostgreSQL 18.6 figures are **reused, not re-measured**, from
`results-scenario3-library-pg18-v2.7.0-545-gf2f1ee7.md` (held at `36449d28`,
measured 2026-09-30, **14 days before this run**). The time budget was lifted
by the operator for this run.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-13 23:53 UTC to 2026-10-14 00:25 UTC (run 1 23:53 to 00:18, run 2 00:18 to 00:21, run 3 00:21 to 00:25; the day changes at midnight; gaps are waits for another session's builds; per-cell windows in section 10) |
| Worktree / branch | `bench-v3.1.0-scenarios` / `worktree-bench-v3.1.0-scenarios` |
| HEAD | `cd7c7c4e`, `git describe --tags` = `v3.1.0-30-gcd7c7c4e` ("Merge sysview-strict-compare: a catalog view's WHERE takes the relation's literal rule, compared under each column's declared type"), committed 2026-10-13 12:24:08 UTC |
| Tree cleanliness | Clean at the start (`git status` empty); nothing under `src/`, `include/`, `tools/` or `CMakeLists.txt` was edited, and the drivers are the unmodified `tools/` scripts (the one exception is stated where it applies). The results files under `bench/v3.1.0/` are uncommitted additions |
| Binary provenance | `build-release/kds_server` built from this tree at HEAD (`cmake -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, `-O3 -DNDEBUG`), finished 2026-10-13 23:26:41 UTC, 14 hours after the HEAD commit, so the binary is not older than the engine. Copied to `/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/kds_server` before the first cell; **copy sha256 `4752f56285aba5e7b1adbd881facc8a7a7dadfab6269b468d30f0b29ce53befa`** (equal to the source binary's), source binary mtime 2026-10-13 23:26:41.5 UTC. Every server started from the copy; no build ran during any reported cell |
| Device | data files under `/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/data/`, `/dev/root`, `ext4` (`df -T`); never tmpfs. A fresh data file per cell (per server, in scenario 4), deleted after |
| Build type | Release (`CMakeCache.txt`: `CMAKE_BUILD_TYPE=Release`) |
| Host | 8 logical CPUs, AMD EPYC 9V74 (1 socket x 4 cores x 2 threads), Linux 7.0.0-1014-azure, a shared development VM: other sessions build and run `ctest` on it |
| Time budget | **Lifted by the operator for this run** (order of 2026-10-13: "this work is an exception to the time-limit rule"); the rebaseline's arguments and repeat counts ran in full, no cell was cut |
| Prior run of this shape | the 2026-09-30 rebaseline at `9a0525d` (`v2.7.0-531-g9a0525d`, "AY closed"), file held at `36449d28` (`git show 36449d28:bench/v3.0.0/results-scenario*-v2.7.0-531-g9a0525d.md`). **495 commits and 176 first-parent merges apart** (`git rev-list --count 9a0525d..HEAD`), with 256 files changed under `src/`, `include/` and `CMakeLists.txt`, so **no delta below is attributable to one change** |
| Server config | `log_level = warn`, `durability = group`, **`buffer_pool_frames = 65536`** (required since BE; the comparator's config predates the key), `cores` 1 or 8 per cell, `indexes` at its default (on; recorded as `--server-indexes on`), all else default; port 15620, never 15432 |
| Quiet host | before each cell a gate waited for no `cc1plus` / `cmake --build` / `ctest` / `kds_tests` process and at most 8 % busy CPU over 3 s (`/proc/loadavg` and the process list are in each cell's precheck, tables below). **A 2-second monitor ran through the whole run** (`monitor.log`) and a cell with a build/test process or a foreign process at 50 % CPU or more inside its window was **discarded and re-run** (up to 8 attempts, 8 happened, `contaminated/` in the archive). Foreign `claude` processes at 10 to 30 % of one CPU were present in many windows and are counted as minor, not excluded. The monitor's limit: it samples every 2 to 3 s, so a shorter burst is not seen |
| PostgreSQL | **reused, not re-measured**: PostgreSQL 18.6 at its defaults, `results-scenario3-library-pg18-v2.7.0-545-gf2f1ee7.md` (held at `36449d28`), measured 2026-09-30 at `f2f1ee7`. Section 8 |

## 2. What was run

Thirty-six cells: `cores` {1, 8} x `--loans` {200, 1000, 10000} x
`--index-mode` {none, all}, **three runs each**; fresh server and data file per
cell; `--seed 1 --matches 5 --ops 200 --verify 25` (the defaults),
`--index-when after` (the default: indexes declared on the loaded relations,
which times the backfill), and for `all` also `--assert-index-reads`. Command:

    python3 tools/scenario3_library.py --port 15620 --suffix <cell> --loans <n> \
        --index-mode <none|all> --server-indexes on --seed 1 [--assert-index-reads] --json <cell>.json

**Row-set axis (rule 9), stated once.** `--loans N` is the bulk relation; users
and books scale to hold `--matches 5` constant. At `N = 10,000`: loans 10,000,
users 2,000, books 2,000, reservations 5,000; at 200 and 1,000 the same ratios.
Every matrix below carries the three sizes as columns.

**All 36 cells exited 0, all 36 `verify` runs reported no problems, and all 18
`all` cells passed `--assert-index-reads`** (section 10). Durability does not enter
a read shape; the load, which is where `group` acts, is in section 6. `--cabin` and the
other `--index-mode` values (`single`, `composite`, `covering`) were **not executed**,
as at the comparator. Host load and build check per cell: section 10.

## 3. Throughput

A serial single-connection driver reports no qps; each cell is **derived**:
1,000,000 / mean µs, then the median of three runs. `cores = 1`:

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 13,089 qps | 15,038 qps | 15,015 qps | 14,970 qps | 15,432 qps | 14,749 qps |
| loans-by-user | 10,132 qps | 11,976 qps | 9,311 qps | 11,364 qps | 2,779 qps | 11,223 qps |
| loans-by-book | 12,136 qps | 12,610 qps | 9,728 qps | 12,121 qps | 2,723 qps | 12,151 qps |
| resv-by-user | 10,953 qps | 14,347 qps | 12,407 qps | 13,569 qps | 4,876 qps | 13,605 qps |
| books-by-author | 9,843 qps | 13,316 qps | 12,821 qps | 12,594 qps | 7,027 qps | 12,346 qps |
| books-by-genre | 10,277 qps | 14,205 qps | 9,814 qps | 9,390 qps | 2,113 qps | 2,111 qps |
| loans-by-daterange | 8,203 qps | 11,086 qps | 4,764 qps | 4,655 qps | 658 qps | 646 qps |
| overdue | 9,960 qps | 12,180 qps | 5,845 qps | 6,227 qps | 877 qps | 1,043 qps |
| join-loan-user | 7,752 qps | 10,730 qps | 8,382 qps | 10,549 qps | 2,541 qps | 10,256 qps |
| join-no-literal | 3,563 qps | 3,664 qps | 2,952 qps | 3,449 qps | 851 qps | 3,506 qps |
| exists-correlated | 7,734 qps | 7,123 qps | 5,714 qps | 5,627 qps | 1,586 qps | 6,954 qps |
| count-by-user | 15,601 qps | 15,974 qps | 11,561 qps | 11,848 qps | 2,906 qps | 15,314 qps |

`cores = 8`:

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 14,286 qps | 14,164 qps | 14,144 qps | 13,831 qps | 14,663 qps | 14,535 qps |
| loans-by-user | 10,953 qps | 11,161 qps | 8,278 qps | 10,718 qps | 2,501 qps | 10,941 qps |
| loans-by-book | 11,723 qps | 11,723 qps | 8,467 qps | 11,377 qps | 2,525 qps | 11,792 qps |
| resv-by-user | 13,643 qps | 13,624 qps | 11,123 qps | 12,469 qps | 4,585 qps | 13,298 qps |
| books-by-author | 13,123 qps | 12,531 qps | 11,947 qps | 11,876 qps | 6,845 qps | 12,092 qps |
| books-by-genre | 13,793 qps | 13,441 qps | 9,083 qps | 8,711 qps | 2,027 qps | 2,023 qps |
| loans-by-daterange | 10,331 qps | 10,267 qps | 4,421 qps | 4,234 qps | 624 qps | 629 qps |
| overdue | 11,442 qps | 11,416 qps | 5,394 qps | 5,814 qps | 828 qps | 1,007 qps |
| join-loan-user | 9,940 qps | 10,091 qps | 7,559 qps | 9,911 qps | 2,408 qps | 9,823 qps |
| join-no-literal | 3,573 qps | 3,392 qps | 2,670 qps | 3,188 qps | 819 qps | 3,319 qps |
| exists-correlated | 7,123 qps | 6,515 qps | 5,181 qps | 6,223 qps | 1,558 qps | 6,502 qps |
| count-by-user | 14,815 qps | 14,948 qps | 10,225 qps | 14,368 qps | 2,664 qps | 14,620 qps |

`cores = 8` reads 0.87x to 1.34x of `cores = 1` over the 72 cells (median 0.94x) and is the
slower of the two in 61 of them (comparator: 0.86x to 1.18x, median 0.95x, slower in 65): one serial connection lands on one core and the
other seven have nothing to do, so no gain is expected; the 5 to 6 % that is lost is inside the cell-to-cell spread of the
`pk-user` control (section 10) and is not a finding.

**Row-count independence of the control.** `pk-user`, a `Lookup`, does not scale with
rows or index mode: 13,089 to 15,432 qps across the six `cores = 1` cells; five of the six lie within 5 % of each other (14,749 to 15,432 qps), and the low one (`n=200 none`) is the
cell whose median was pulled by the host's slow mode (section 10), not by the row count.

## 4. Delta against the previous run of this shape (cross-version)

The comparator is `bench/v3.0.0/results-scenario3-library-v2.7.0-531-g9a0525d.md`, three runs
per cell, held at `36449d28`. The comparator's describe string says `v2.7.0` because that was the version of record on 2026-09-30; it is the same architecture series (the AR0 single-WAL engine) and `bench/README.md` names it as the comparator for every later delta of the same driver and shape. The version label changed between the two runs, so every delta in this file is **cross-version** and is labelled so.
**495 commits apart**, so no difference is attributable to one change. Ratio of median qps,
now over the comparator (above 1.00x = faster now), `cores = 1`:

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 0.94x | 1.09x | 1.10x | 1.08x | 1.10x | 1.03x |
| loans-by-user | 1.00x | 1.07x | 1.45x | 1.03x | 2.18x | 1.03x |
| loans-by-book | 1.13x | 1.09x | 1.44x | 1.04x | 2.14x | 1.04x |
| resv-by-user | 0.86x | 1.09x | 1.34x | 1.04x | 2.08x | 1.05x |
| books-by-author | 0.78x | 1.12x | 1.55x | 1.03x | 1.65x | 1.05x |
| books-by-genre | 0.77x | 1.07x | 1.30x | 1.01x | 1.28x | 1.00x |
| loans-by-daterange | 0.87x | 1.19x | 1.25x | 1.21x | 1.34x | 1.26x |
| overdue | 0.97x | 1.08x | 1.33x | 0.97x | 1.45x | 0.97x |
| join-loan-user | 0.84x | 1.06x | 1.37x | 1.05x | 2.02x | 1.01x |
| join-no-literal | 0.94x | 0.99x | 1.04x | 1.00x | 1.09x | 0.97x |
| exists-correlated | 0.99x | 1.01x | 1.01x | 0.91x | 1.01x | 0.98x |
| count-by-user | 1.20x | 1.09x | 1.57x | 0.82x | 2.20x | 1.07x |
| geometric mean | 0.933x | 1.078x | 1.299x | 1.012x | 1.565x | 1.035x |

And `cores = 8`:

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 1.10x | 1.08x | 1.09x | 1.06x | 1.08x | 1.07x |
| loans-by-user | 1.15x | 1.03x | 1.49x | 1.01x | 2.07x | 1.06x |
| loans-by-book | 1.16x | 1.03x | 1.43x | 1.01x | 2.08x | 1.06x |
| resv-by-user | 1.13x | 1.05x | 1.33x | 1.00x | 1.95x | 1.07x |
| books-by-author | 1.09x | 1.05x | 1.22x | 1.00x | 1.70x | 1.06x |
| books-by-genre | 1.09x | 1.06x | 1.13x | 0.98x | 1.20x | 0.99x |
| loans-by-daterange | 1.17x | 1.17x | 1.24x | 1.18x | 1.27x | 1.29x |
| overdue | 1.19x | 1.02x | 1.34x | 1.00x | 1.43x | 0.99x |
| join-loan-user | 1.18x | 1.05x | 1.40x | 1.03x | 2.02x | 1.00x |
| join-no-literal | 1.01x | 1.00x | 1.03x | 0.97x | 1.11x | 0.99x |
| exists-correlated | 0.99x | 0.99x | 1.01x | 0.94x | 1.04x | 0.98x |
| count-by-user | 1.23x | 1.07x | 1.54x | 1.02x | 2.15x | 1.08x |
| geometric mean | 1.121x | 1.050x | 1.261x | 1.015x | 1.532x | 1.050x |

The comparator's `cores = 1` matrix, for side-by-side reading (qps):

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

**Reading it.** The floor of this table is the `pk-user` row (0.94x to 1.10x): a ratio inside about
0.90x to 1.10x is not a finding, and the `n=200 none` column (geometric mean 0.933x) is the one the
host's slow mode touched (run 3 of that cell has a `pk-user` mean of 95.9 µs against 66 to 76 µs, section 10),
so its sub-1.0 ratios are noise. Outside the floor: the unindexed equality shapes at 1,000 rows (1.3x to 1.6x)
and at 10,000 rows (**1.65x to 2.20x**, `loans-by-user` 2.18x, `count-by-user` 2.20x), and
`loans-by-daterange` (1.19x to 1.34x in the five columns other than `n=200 none`, a full scan with a range predicate). The indexed
columns are flat (geometric mean 1.035x at 10,000 rows). The eight-core cells show the same
(unindexed 10,000 rows 1.53x geometric mean).

## 5. What the index buys now

The ratio none / all of mean latency (`cores = 1`, medians of three), now and at the comparator. Above 1 the
index is faster:

| shape | n=200 | n=1,000 | n=10,000 |
|---|---|---|---|
| pk-user | 1.15x / 0.99x | 1.00x / 1.01x | 0.96x / 1.03x |
| loans-by-user | 1.18x / 1.10x | 1.22x / 1.72x | 4.04x / 8.52x |
| loans-by-book | 1.04x / 1.08x | 1.25x / 1.72x | 4.46x / 9.24x |
| resv-by-user | 1.31x / 1.04x | 1.09x / 1.41x | 2.79x / 5.54x |
| books-by-author | 1.35x / 0.94x | 0.98x / 1.47x | 1.76x / 2.77x |
| books-by-genre | 1.38x / 1.00x | 0.96x / 1.23x | 1.00x / 1.28x |
| loans-by-daterange | 1.35x / 1.00x | 0.98x / 1.01x | 0.98x / 1.05x |
| overdue | 1.22x / 1.10x | 1.07x / 1.46x | 1.19x / 1.77x |
| join-loan-user | 1.38x / 1.09x | 1.26x / 1.64x | 4.04x / 8.06x |
| join-no-literal | 1.03x / 0.97x | 1.17x / 1.21x | 4.12x / 4.63x |
| exists-correlated | 0.92x / 0.91x | 0.98x / 1.10x | 4.39x / 4.56x |
| count-by-user | 1.02x / 1.12x | 1.02x / 1.96x | 5.27x / 10.81x |

The pk control (`pk-user`) reads 0.96x to 1.15x: **a ratio inside about 1.00 +/- 0.15 is not a finding**
here. The gain at 10,000 rows roughly halved (8.5x to 4.0x for `loans-by-user`) and at 1,000 rows
fell from 1.7x to 1.2x, because the unindexed side got cheaper while the indexed side (one round trip) did not;
at 200 rows the index buys 0.9x to 1.4x, at the floor.

## 6. Percentiles

`cores = 1`, run 1, in full: 200 operations per shape. Microseconds; `plan` is what ANALYZE
reported the shape compiled to. The `pk-user` p50 in `n=200 none` of run 1 is 68.8 µs against about
66 µs in clean cells (the slow mode, section 10).

| n | mode | shape | p0 | p25 | p50 | p95 | p99 | plan |
|---|---|---|---|---|---|---|---|---|
| 200 | none | pk-user | 58.2 µs | 61.8 µs | 68.8 µs | 108.7 µs | 130.7 µs | - |
| 200 | none | loans-by-user | 63.7 µs | 79.7 µs | 97.5 µs | 130.1 µs | 144.3 µs | FilterScan |
| 200 | none | loans-by-book | 64.6 µs | 71.8 µs | 78.5 µs | 90.0 µs | 95.9 µs | FilterScan |
| 200 | none | resv-by-user | 60.2 µs | 83.8 µs | 94.4 µs | 113.7 µs | 124.9 µs | FilterScan |
| 200 | none | books-by-author | 88.2 µs | 93.6 µs | 100.2 µs | 120.6 µs | 130.5 µs | FilterScan |
| 200 | none | books-by-genre | 88.1 µs | 92.5 µs | 95.5 µs | 114.4 µs | 121.4 µs | FilterScan |
| 200 | none | loans-by-daterange | 100.3 µs | 111.8 µs | 118.3 µs | 145.6 µs | 153.5 µs | Scan |
| 200 | none | overdue | 66.9 µs | 83.4 µs | 102.0 µs | 129.3 µs | 143.5 µs | FilterScan |
| 200 | none | join-loan-user | 99.4 µs | 120.1 µs | 126.6 µs | 155.2 µs | 168.8 µs | - |
| 200 | none | join-no-literal | 245.8 µs | 256.0 µs | 285.9 µs | 313.7 µs | 372.1 µs | Range,Scan |
| 200 | none | exists-correlated | 120.1 µs | 121.5 µs | 122.3 µs | 141.1 µs | 155.6 µs | Range,Scan |
| 200 | none | count-by-user | 58.5 µs | 59.6 µs | 60.1 µs | 73.3 µs | 77.4 µs | - |
| 200 | all | pk-user | 59.1 µs | 60.7 µs | 62.6 µs | 82.1 µs | 114.9 µs | - |
| 200 | all | loans-by-user | 60.4 µs | 75.6 µs | 82.2 µs | 102.3 µs | 112.7 µs | IndexProbe |
| 200 | all | loans-by-book | 62.7 µs | 71.8 µs | 78.9 µs | 93.2 µs | 101.1 µs | IndexProbe |
| 200 | all | resv-by-user | 56.9 µs | 65.7 µs | 68.7 µs | 82.7 µs | 91.3 µs | IndexProbe |
| 200 | all | books-by-author | 62.8 µs | 67.1 µs | 73.2 µs | 94.1 µs | 104.6 µs | IndexProbe |
| 200 | all | books-by-genre | 62.1 µs | 65.8 µs | 69.3 µs | 81.2 µs | 92.1 µs | IndexProbe |
| 200 | all | loans-by-daterange | 70.4 µs | 82.4 µs | 88.6 µs | 115.6 µs | 127.3 µs | Scan |
| 200 | all | overdue | 61.5 µs | 77.3 µs | 81.8 µs | 96.6 µs | 105.1 µs | IndexRange |
| 200 | all | join-loan-user | 66.0 µs | 85.4 µs | 92.3 µs | 112.8 µs | 117.8 µs | - |
| 200 | all | join-no-literal | 259.1 µs | 264.7 µs | 269.2 µs | 289.7 µs | 358.1 µs | Range,IndexProbe |
| 200 | all | exists-correlated | 132.8 µs | 134.7 µs | 136.2 µs | 158.6 µs | 180.6 µs | Range,IndexProbe |
| 200 | all | count-by-user | 56.7 µs | 60.1 µs | 61.0 µs | 74.9 µs | 78.9 µs | - |
| 1,000 | none | pk-user | 58.7 µs | 61.5 µs | 65.4 µs | 86.6 µs | 103.5 µs | - |
| 1,000 | none | loans-by-user | 86.8 µs | 102.0 µs | 108.1 µs | 133.4 µs | 144.1 µs | FilterScan |
| 1,000 | none | loans-by-book | 87.4 µs | 97.6 µs | 104.8 µs | 128.5 µs | 139.0 µs | FilterScan |
| 1,000 | none | resv-by-user | 69.7 µs | 76.6 µs | 81.1 µs | 102.3 µs | 115.4 µs | FilterScan |
| 1,000 | none | books-by-author | 63.1 µs | 74.2 µs | 80.0 µs | 102.7 µs | 133.0 µs | FilterScan |
| 1,000 | none | books-by-genre | 81.3 µs | 95.8 µs | 101.0 µs | 126.4 µs | 141.8 µs | FilterScan |
| 1,000 | none | loans-by-daterange | 119.7 µs | 199.9 µs | 211.6 µs | 256.9 µs | 266.9 µs | Scan |
| 1,000 | none | overdue | 95.5 µs | 164.5 µs | 174.4 µs | 202.6 µs | 214.5 µs | FilterScan |
| 1,000 | none | join-loan-user | 92.2 µs | 109.8 µs | 115.7 µs | 144.7 µs | 165.2 µs | - |
| 1,000 | none | join-no-literal | 326.0 µs | 335.1 µs | 342.6 µs | 360.7 µs | 374.3 µs | Range,Scan |
| 1,000 | none | exists-correlated | 163.9 µs | 167.8 µs | 171.5 µs | 192.8 µs | 202.1 µs | Range,Scan |
| 1,000 | none | count-by-user | 81.0 µs | 82.5 µs | 84.5 µs | 132.9 µs | 143.9 µs | - |
| 1,000 | all | pk-user | 59.4 µs | 61.0 µs | 63.0 µs | 77.6 µs | 90.9 µs | - |
| 1,000 | all | loans-by-user | 63.3 µs | 79.0 µs | 86.2 µs | 114.7 µs | 128.7 µs | IndexProbe |
| 1,000 | all | loans-by-book | 63.8 µs | 75.6 µs | 81.0 µs | 99.9 µs | 111.5 µs | IndexProbe |
| 1,000 | all | resv-by-user | 57.9 µs | 68.7 µs | 72.7 µs | 88.1 µs | 93.4 µs | IndexProbe |
| 1,000 | all | books-by-author | 60.8 µs | 72.7 µs | 78.0 µs | 98.5 µs | 104.3 µs | IndexProbe |
| 1,000 | all | books-by-genre | 83.7 µs | 99.4 µs | 105.0 µs | 129.5 µs | 143.9 µs | IndexProbe |
| 1,000 | all | loans-by-daterange | 107.6 µs | 202.9 µs | 213.7 µs | 249.7 µs | 264.1 µs | Scan |
| 1,000 | all | overdue | 76.9 µs | 146.8 µs | 159.8 µs | 191.0 µs | 216.2 µs | IndexRange |
| 1,000 | all | join-loan-user | 68.7 µs | 87.1 µs | 93.3 µs | 114.9 µs | 129.0 µs | - |
| 1,000 | all | join-no-literal | 271.9 µs | 280.6 µs | 288.1 µs | 329.2 µs | 368.8 µs | Range,IndexProbe |
| 1,000 | all | exists-correlated | 135.4 µs | 137.6 µs | 141.2 µs | 182.4 µs | 194.6 µs | Range,IndexProbe |
| 1,000 | all | count-by-user | 58.5 µs | 61.9 µs | 63.3 µs | 81.6 µs | 89.4 µs | - |
| 10,000 | none | pk-user | 59.4 µs | 60.8 µs | 61.6 µs | 76.2 µs | 103.5 µs | - |
| 10,000 | none | loans-by-user | 335.2 µs | 349.2 µs | 357.3 µs | 378.0 µs | 390.5 µs | FilterScan |
| 10,000 | none | loans-by-book | 328.7 µs | 349.6 µs | 360.1 µs | 424.4 µs | 494.1 µs | FilterScan |
| 10,000 | none | resv-by-user | 190.1 µs | 199.6 µs | 203.8 µs | 220.4 µs | 227.6 µs | FilterScan |
| 10,000 | none | books-by-author | 120.0 µs | 131.8 µs | 136.7 µs | 171.3 µs | 246.8 µs | FilterScan |
| 10,000 | none | books-by-genre | 429.8 µs | 453.8 µs | 470.3 µs | 509.2 µs | 558.2 µs | FilterScan |
| 10,000 | none | loans-by-daterange | 444.9 µs | 1,491.9 µs | 1,538.1 µs | 1,636.9 µs | 1,790.9 µs | Scan |
| 10,000 | none | overdue | 384.6 µs | 1,134.4 µs | 1,169.6 µs | 1,277.9 µs | 1,334.7 µs | FilterScan |
| 10,000 | none | join-loan-user | 347.0 µs | 365.3 µs | 375.1 µs | 404.4 µs | 436.3 µs | - |
| 10,000 | none | join-no-literal | 1,104.7 µs | 1,116.2 µs | 1,123.5 µs | 1,285.2 µs | 1,332.7 µs | Range,Scan |
| 10,000 | none | exists-correlated | 595.8 µs | 609.3 µs | 617.1 µs | 641.5 µs | 666.8 µs | Range,Scan |
| 10,000 | none | count-by-user | 324.2 µs | 328.5 µs | 333.1 µs | 365.6 µs | 389.8 µs | - |
| 10,000 | all | pk-user | 59.8 µs | 61.3 µs | 62.9 µs | 89.1 µs | 109.3 µs | - |
| 10,000 | all | loans-by-user | 67.3 µs | 79.5 µs | 86.9 µs | 106.4 µs | 119.2 µs | IndexProbe |
| 10,000 | all | loans-by-book | 59.2 µs | 74.7 µs | 79.7 µs | 99.4 µs | 108.7 µs | IndexProbe |
| 10,000 | all | resv-by-user | 56.5 µs | 67.0 µs | 70.7 µs | 87.6 µs | 99.4 µs | IndexProbe |
| 10,000 | all | books-by-author | 58.3 µs | 73.3 µs | 78.5 µs | 106.2 µs | 118.1 µs | IndexProbe |
| 10,000 | all | books-by-genre | 417.2 µs | 453.3 µs | 471.9 µs | 512.5 µs | 633.5 µs | IndexProbe |
| 10,000 | all | loans-by-daterange | 464.2 µs | 1,535.3 µs | 1,581.5 µs | 1,690.0 µs | 2,019.9 µs | Scan |
| 10,000 | all | overdue | 112.0 µs | 973.6 µs | 1,020.2 µs | 1,131.1 µs | 1,168.8 µs | IndexRange |
| 10,000 | all | join-loan-user | 107.5 µs | 123.0 µs | 129.6 µs | 156.8 µs | 170.4 µs | - |
| 10,000 | all | join-no-literal | 308.7 µs | 314.6 µs | 320.3 µs | 340.0 µs | 358.0 µs | Range,IndexProbe |
| 10,000 | all | exists-correlated | 169.5 µs | 171.8 µs | 173.6 µs | 195.8 µs | 203.2 µs | Range,IndexProbe |
| 10,000 | all | count-by-user | 87.0 µs | 91.8 µs | 93.5 µs | 112.3 µs | 120.6 µs | - |

`cores = 8`, run 1, the same:

| n | mode | shape | p0 | p25 | p50 | p95 | p99 | plan |
|---|---|---|---|---|---|---|---|---|
| 200 | none | pk-user | 59.7 µs | 62.3 µs | 66.6 µs | 86.8 µs | 98.4 µs | - |
| 200 | none | loans-by-user | 65.3 µs | 80.8 µs | 88.2 µs | 112.1 µs | 122.6 µs | FilterScan |
| 200 | none | loans-by-book | 68.2 µs | 78.1 µs | 83.9 µs | 104.2 µs | 114.7 µs | FilterScan |
| 200 | none | resv-by-user | 59.4 µs | 67.4 µs | 70.6 µs | 89.1 µs | 96.1 µs | FilterScan |
| 200 | none | books-by-author | 62.7 µs | 68.0 µs | 76.6 µs | 92.7 µs | 99.6 µs | FilterScan |
| 200 | none | books-by-genre | 61.5 µs | 67.0 µs | 70.5 µs | 91.1 µs | 100.2 µs | FilterScan |
| 200 | none | loans-by-daterange | 75.1 µs | 87.3 µs | 95.1 µs | 122.9 µs | 136.0 µs | Scan |
| 200 | none | overdue | 66.1 µs | 80.9 µs | 85.0 µs | 104.7 µs | 111.5 µs | FilterScan |
| 200 | none | join-loan-user | 77.9 µs | 90.8 µs | 99.0 µs | 125.0 µs | 152.0 µs | - |
| 200 | none | join-no-literal | 258.2 µs | 270.7 µs | 280.5 µs | 313.3 µs | 401.6 µs | Range,Scan |
| 200 | none | exists-correlated | 127.7 µs | 131.8 µs | 136.6 µs | 158.9 µs | 171.6 µs | Range,Scan |
| 200 | none | count-by-user | 61.6 µs | 63.2 µs | 65.0 µs | 85.5 µs | 92.3 µs | - |
| 200 | all | pk-user | 59.8 µs | 63.4 µs | 67.9 µs | 92.5 µs | 105.3 µs | - |
| 200 | all | loans-by-user | 62.4 µs | 81.2 µs | 88.5 µs | 113.0 µs | 123.3 µs | IndexProbe |
| 200 | all | loans-by-book | 64.8 µs | 78.3 µs | 84.3 µs | 107.2 µs | 114.7 µs | IndexProbe |
| 200 | all | resv-by-user | 59.3 µs | 70.4 µs | 74.2 µs | 97.1 µs | 111.5 µs | IndexProbe |
| 200 | all | books-by-author | 64.0 µs | 71.2 µs | 80.2 µs | 99.6 µs | 111.1 µs | IndexProbe |
| 200 | all | books-by-genre | 63.3 µs | 69.3 µs | 72.7 µs | 89.9 µs | 97.1 µs | IndexProbe |
| 200 | all | loans-by-daterange | 72.5 µs | 88.0 µs | 96.8 µs | 117.2 µs | 130.8 µs | Scan |
| 200 | all | overdue | 63.5 µs | 80.7 µs | 86.2 µs | 106.7 µs | 112.0 µs | IndexRange |
| 200 | all | join-loan-user | 66.4 µs | 89.9 µs | 97.4 µs | 121.6 µs | 139.4 µs | - |
| 200 | all | join-no-literal | 274.8 µs | 283.3 µs | 293.4 µs | 315.8 µs | 325.2 µs | Range,IndexProbe |
| 200 | all | exists-correlated | 138.5 µs | 144.8 µs | 156.4 µs | 197.8 µs | 207.6 µs | Range,IndexProbe |
| 200 | all | count-by-user | 62.4 µs | 69.1 µs | 71.7 µs | 89.5 µs | 97.6 µs | - |
| 1,000 | none | pk-user | 58.9 µs | 62.3 µs | 66.5 µs | 88.0 µs | 108.2 µs | - |
| 1,000 | none | loans-by-user | 95.2 µs | 110.3 µs | 118.7 µs | 146.9 µs | 156.8 µs | FilterScan |
| 1,000 | none | loans-by-book | 96.8 µs | 107.6 µs | 114.5 µs | 138.5 µs | 147.8 µs | FilterScan |
| 1,000 | none | resv-by-user | 73.8 µs | 82.6 µs | 86.5 µs | 108.7 µs | 119.1 µs | FilterScan |
| 1,000 | none | books-by-author | 66.5 µs | 75.3 µs | 81.8 µs | 102.2 µs | 113.3 µs | FilterScan |
| 1,000 | none | books-by-genre | 84.4 µs | 100.7 µs | 108.1 µs | 134.2 µs | 151.3 µs | FilterScan |
| 1,000 | none | loans-by-daterange | 107.5 µs | 213.8 µs | 225.9 µs | 259.9 µs | 295.6 µs | Scan |
| 1,000 | none | overdue | 111.7 µs | 174.5 µs | 182.6 µs | 211.8 µs | 225.7 µs | FilterScan |
| 1,000 | none | join-loan-user | 101.3 µs | 119.7 µs | 127.3 µs | 161.0 µs | 171.5 µs | - |
| 1,000 | none | join-no-literal | 351.0 µs | 362.7 µs | 372.6 µs | 397.2 µs | 453.7 µs | Range,Scan |
| 1,000 | none | exists-correlated | 176.5 µs | 181.2 µs | 187.6 µs | 217.7 µs | 233.3 µs | Range,Scan |
| 1,000 | none | count-by-user | 90.4 µs | 91.7 µs | 92.5 µs | 117.8 µs | 132.9 µs | - |
| 1,000 | all | pk-user | 60.2 µs | 67.4 µs | 70.3 µs | 97.2 µs | 114.8 µs | - |
| 1,000 | all | loans-by-user | 64.1 µs | 81.7 µs | 89.6 µs | 117.4 µs | 131.2 µs | IndexProbe |
| 1,000 | all | loans-by-book | 64.0 µs | 76.4 µs | 83.5 µs | 104.5 µs | 116.5 µs | IndexProbe |
| 1,000 | all | resv-by-user | 59.2 µs | 69.9 µs | 73.7 µs | 97.2 µs | 107.6 µs | IndexProbe |
| 1,000 | all | books-by-author | 60.6 µs | 73.9 µs | 80.5 µs | 105.1 µs | 118.7 µs | IndexProbe |
| 1,000 | all | books-by-genre | 84.1 µs | 101.2 µs | 108.7 µs | 136.8 µs | 148.9 µs | IndexProbe |
| 1,000 | all | loans-by-daterange | 114.7 µs | 218.9 µs | 231.9 µs | 284.3 µs | 332.3 µs | Scan |
| 1,000 | all | overdue | 78.7 µs | 152.2 µs | 166.4 µs | 197.9 µs | 207.9 µs | IndexRange |
| 1,000 | all | join-loan-user | 72.2 µs | 90.3 µs | 98.9 µs | 129.4 µs | 148.8 µs | - |
| 1,000 | all | join-no-literal | 288.2 µs | 302.6 µs | 311.5 µs | 335.6 µs | 347.4 µs | Range,IndexProbe |
| 1,000 | all | exists-correlated | 146.6 µs | 150.4 µs | 157.5 µs | 181.8 µs | 192.3 µs | Range,IndexProbe |
| 1,000 | all | count-by-user | 58.8 µs | 64.1 µs | 66.9 µs | 87.8 µs | 93.8 µs | - |
| 10,000 | none | pk-user | 60.0 µs | 61.8 µs | 64.0 µs | 90.9 µs | 214.1 µs | - |
| 10,000 | none | loans-by-user | 367.2 µs | 389.4 µs | 399.6 µs | 423.2 µs | 434.4 µs | FilterScan |
| 10,000 | none | loans-by-book | 364.3 µs | 381.8 µs | 394.1 µs | 518.2 µs | 545.3 µs | FilterScan |
| 10,000 | none | resv-by-user | 196.3 µs | 237.9 µs | 269.9 µs | 290.5 µs | 297.3 µs | FilterScan |
| 10,000 | none | books-by-author | 125.1 µs | 137.4 µs | 144.6 µs | 167.5 µs | 181.4 µs | FilterScan |
| 10,000 | none | books-by-genre | 436.0 µs | 479.8 µs | 496.3 µs | 528.7 µs | 552.0 µs | FilterScan |
| 10,000 | none | loans-by-daterange | 503.5 µs | 1,598.8 µs | 1,648.0 µs | 1,753.2 µs | 2,043.3 µs | Scan |
| 10,000 | none | overdue | 446.6 µs | 1,220.1 µs | 1,262.5 µs | 1,366.9 µs | 1,414.1 µs | FilterScan |
| 10,000 | none | join-loan-user | 374.8 µs | 403.7 µs | 418.7 µs | 452.8 µs | 464.4 µs | - |
| 10,000 | none | join-no-literal | 1,189.6 µs | 1,209.2 µs | 1,217.5 µs | 1,242.0 µs | 1,258.2 µs | Range,Scan |
| 10,000 | none | exists-correlated | 611.0 µs | 635.9 µs | 643.4 µs | 667.7 µs | 703.2 µs | Range,Scan |
| 10,000 | none | count-by-user | 352.6 µs | 363.6 µs | 378.4 µs | 410.4 µs | 418.6 µs | - |
| 10,000 | all | pk-user | 59.4 µs | 61.2 µs | 63.3 µs | 90.2 µs | 103.8 µs | - |
| 10,000 | all | loans-by-user | 70.6 µs | 81.7 µs | 89.5 µs | 114.9 µs | 123.7 µs | IndexProbe |
| 10,000 | all | loans-by-book | 64.4 µs | 76.3 µs | 82.7 µs | 105.6 µs | 116.2 µs | IndexProbe |
| 10,000 | all | resv-by-user | 55.6 µs | 68.7 µs | 72.7 µs | 96.9 µs | 104.7 µs | IndexProbe |
| 10,000 | all | books-by-author | 60.4 µs | 74.3 µs | 79.5 µs | 102.1 µs | 111.5 µs | IndexProbe |
| 10,000 | all | books-by-genre | 424.1 µs | 470.3 µs | 485.1 µs | 527.6 µs | 562.7 µs | IndexProbe |
| 10,000 | all | loans-by-daterange | 508.4 µs | 1,553.0 µs | 1,595.8 µs | 1,709.3 µs | 1,877.9 µs | Scan |
| 10,000 | all | overdue | 89.6 µs | 976.3 µs | 1,024.8 µs | 1,143.5 µs | 1,400.3 µs | IndexRange |
| 10,000 | all | join-loan-user | 76.8 µs | 91.0 µs | 97.5 µs | 134.7 µs | 157.9 µs | - |
| 10,000 | all | join-no-literal | 277.2 µs | 287.5 µs | 296.6 µs | 322.8 µs | 380.6 µs | Range,IndexProbe |
| 10,000 | all | exists-correlated | 142.8 µs | 145.8 µs | 149.6 µs | 176.2 µs | 189.7 µs | Range,IndexProbe |
| 10,000 | all | count-by-user | 58.7 µs | 63.2 µs | 65.5 µs | 90.4 µs | 97.6 µs | - |

## 7. Where the time goes

A read is one round trip plus the work. The decomposition is fixed cost and rows
(least squares over the three sizes, medians of three runs, `cores = 1`, **unindexed**;
`rows walked` is the relation the statement walks, as a multiple of `--loans`):

| shape | rows walked per n | fixed a now | slope b now | fixed a 9a0525d | slope b 9a0525d | b ratio then/now |
|---|---|---|---|---|---|---|
| loans-by-user | 1 x n | 87.0 µs | 27 ns per row | 84.8 µs | 70 ns per row | 2.56x |
| loans-by-book | 1 x n | 75.1 µs | 29 ns per row | 78.3 µs | 71 ns per row | 2.43x |
| resv-by-user | 0.5 x n | 78.9 µs | 25 ns per row | 72.2 µs | 71 ns per row | 2.83x |
| books-by-author | 0.2 x n | 87.2 µs | 27 ns per row | 90.5 µs | 73 ns per row | 2.70x |
| books-by-genre | 0.2 x n | 76.4 µs | 198 ns per row | 71.1 µs | 268 ns per row | 1.36x |
| loans-by-daterange | 1 x n | 80.2 µs | 144 ns per row | 66.3 µs | 197 ns per row | 1.37x |
| overdue | 1 x n | 72.0 µs | 107 ns per row | 66.9 µs | 158 ns per row | 1.48x |
| join-loan-user | 1 x n | 107.8 µs | 28 ns per row | 93.5 µs | 70 ns per row | 2.47x |
| join-no-literal | 1 x n | 254.9 µs | 92 ns per row | 244.8 µs | 104 ns per row | 1.13x |
| exists-correlated | 1 x n | 121.5 µs | 51 ns per row | 121.8 µs | 52 ns per row | 1.02x |
| count-by-user | 1 x n | 58.2 µs | 29 ns per row | 64.8 µs | 69 ns per row | 2.42x |

| Component | Now | How derived |
|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | **69.6 µs** (`pk-user` mean, median of 36 cells; 64.3 to 95.9 µs) | the control |
| Fixed cost of a FilterScan statement | 58 to 87 µs for the single-relation equality statements, 108 to 255 µs for the joins (`a` above); comparator 65 to 91 and 94 to 245 µs | intercept of the fit |
| Rows walked by an equality FilterScan | **25 to 29 ns per row** (comparator 69 to 73) | slope of the fit: `loans-by-user`, `loans-by-book`, `resv-by-user`, `books-by-author`, `count-by-user`, `join-loan-user` |
| Rows walked by a range or multi-column predicate | 107 to 198 ns per row (comparator 158 to 268) | `loans-by-daterange`, `overdue`, `books-by-genre` (the last returns many rows) |
| Nested range + scan or probe | 51 to 92 ns per row (comparator 52 to 104) | `exists-correlated`, `join-no-literal` |
| Durability / commit | not applicable to a read. The load is the only write path: **1,028 to 1,133 µs p50 per 500-row batch** (a batch is one transaction and one commit), mean 1,198 to 1,369 µs; the same at both core counts and all row counts, so the commit is fixed per batch and ~2.5 µs per row | `load` phase, 36 cells |
| Index backfill | `create-index` (8 indexes after load): 1.2 ms at 200 rows, 1.8 to 3.9 ms at 1,000, 7.2 to 8.0 ms at 10,000 (`create-index` mean per index) | one-shot phases below |
| Lock or conflict wait | none: one reader, no concurrent writer | |
| Client / socket | in the fixed cost; the client is the same Python driver both runs | |

One-shot phases (not matrix rows; units stay in µs; `cores = 1`, run 1):

| n | mode | load ops | load p50 | load mean | create-index ops | create-index p50 | create-index mean | now/9a0525d load mean |
|---|---|---|---|---|---|---|---|---|
| 200 | none | 380 ops | 1,050 µs | 1,198 µs | not run | - | - | 0.50x |
| 200 | all | 380 ops | 1,028 µs | 1,198 µs | 8 ops | 1,218 µs | 1,232 µs | 0.67x; create-index 0.80x |
| 1,000 | none | 1,900 ops | 1,044 µs | 1,198 µs | not run | - | - | 0.78x |
| 1,000 | all | 1,900 ops | 1,040 µs | 1,222 µs | 8 ops | 1,601 µs | 1,790 µs | 0.80x; create-index 0.70x |
| 10,000 | none | 19,000 ops | 1,077 µs | 1,283 µs | not run | - | - | 0.85x |
| 10,000 | all | 19,000 ops | 1,067 µs | 1,266 µs | 8 ops | 5,948 µs | 7,607 µs | 0.81x; create-index 0.89x |

**The per-row cost of a walked row fell from about 70 ns to 27 ns, but only for the shapes whose
predicate is an equality the scan can reject on the cell's bytes.** Those are the six shapes
with 2.4x to 2.8x lower slope; the range-predicate and join shapes are 1.0x to 1.5x. The fixed
cost did not move (58 to 87 µs against 65 to 91 for the single-relation equality statements). The rows-per-statement arithmetic is why the
index lost half its multiple: a 10,000-row equality scan was 700 µs of which 84 was fixed; it is now 360 µs
of which 87 is fixed.

## 8. Against PostgreSQL 18.6 (reused, not re-measured)

PostgreSQL is the floor: it answers "is the engine in the right league" and nothing finer. The twin
(`tools/pg_scenario3_library.py`) was run at `f2f1ee7` on 2026-09-30, 14 days before these cells,
three runs per cell, with PostgreSQL's defaults (`shared_buffers = 128MB`, `synchronous_commit = on`); its per-shape
medians are in its own file and are not re-measured here. Ratio of KDS `cores = 1` median qps (this run)
to the PG18 median qps, per shape (above 1.00x = KDS faster):

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 0.83x | 0.94x | 1.01x | 1.03x | 0.99x | 0.95x |
| loans-by-user | 0.75x | 0.99x | 0.96x | 0.98x | 1.12x | 1.03x |
| loans-by-book | 0.88x | 0.99x | 0.99x | 0.99x | 1.15x | 1.01x |
| resv-by-user | 0.70x | 0.96x | 0.99x | 0.99x | 1.12x | 1.01x |
| books-by-author | 0.64x | 0.93x | 0.91x | 0.98x | 1.00x | 0.99x |
| books-by-genre | 0.64x | 0.97x | 0.82x | 0.85x | 0.68x | 0.54x |
| loans-by-daterange | 0.69x | 0.95x | 0.83x | 0.78x | 0.62x | 0.61x |
| overdue | 0.80x | 1.10x | 0.88x | 0.82x | 0.77x | 0.54x |
| join-loan-user | 0.77x | 1.14x | 1.14x | 1.25x | 1.09x | 1.14x |
| join-no-literal | 0.79x | 0.88x | 0.86x | 1.07x | 0.57x | 2.43x |
| exists-correlated | 1.35x | 1.32x | 1.71x | 1.13x | 2.09x | 1.35x |
| count-by-user | 1.15x | 1.31x | 1.18x | 0.99x | 1.18x | 1.32x |
| geometric mean (shapes ahead by >1.05x) | 0.81x (2 of 12) | 1.03x (4 of 12) | 1.00x (3 of 12) | 0.98x (3 of 12) | 0.97x (6 of 12) | 0.99x (4 of 12) |

Against the comparator-era floor (`results-kds-vs-pg18-summary-v2.7.0-545-gf2f1ee7.md`: geometric mean
0.87x / 0.96x / 0.77x / 0.97x / 0.62x / 0.95x for the same six columns), **the unindexed gap closed**: at 10,000 rows
the unindexed equality shapes are now 1.12x to 1.15x of PostgreSQL's seqscan (they were 0.52x to 0.54x), a KDS walked row
(27 ns) is faster than PostgreSQL's (34 to 36 ns by the same fit on its reused medians: `loans-by-user` 34, `loans-by-book` 36,
`resv-by-user` 34, `books-by-author` 40, `count-by-user` 34), and the geometric mean over twelve shapes at 10,000 rows
unindexed went from 0.62x to 0.97x (the table's last row). **Still behind**: the shapes
that are not a plain equality or that return many rows (`books-by-genre`, `loans-by-daterange`, `overdue`: 0.54x to 0.77x at
10,000 rows), and the fixed round trip (`pk-user` 0.83x to 1.03x; the KDS statement costs 4 to 10 µs more than
PostgreSQL's 62 to 69 µs). **Ahead**: `exists-correlated` in every column (1.13x to 2.09x), `join-loan-user` at 1,000
and 10,000 rows with indexes (1.25x, 1.14x), `join-no-literal` indexed at 10,000 rows (2.43x). The
PostgreSQL side was one set of runs 14 days earlier on a host that has since run other work; the KDS cells carry the
slow-mode noise of section 10, so ratios inside 0.90x to 1.10x are not findings.

## 9. What the run says about the engine

1. **The row scan is now at or below PostgreSQL's per-row cost for an equality**, 27 against 34 ns, after being
   at twice it. The mechanism the specs name is BM (a scan kernel rejects on the cell's bytes before any
   decode) and BT (a rejected row reads its tuple without a `Status`; `docs/spec/parser-v2.md` §7); this run measures the
   outcome, not the milestone, and BM's and BT's own close files at `bench/v3.0.0/` carry their A/B.
2. **The win does not reach shapes whose predicate is not that equality.** Range and multi-column predicates walk at 107 to
   198 ns per row and the join shapes at 51 to 92 ns; those are where the next row-walk cost lives.
3. **An index is worth less than it was, and still decisive at 10,000 rows**: 4.0x for `loans-by-user`,
   against a fixed cost of about 87 µs that no index removes. At 200 rows the index buys nothing outside the floor.
4. **The fixed round trip did not change** (69.6 µs median `pk-user`; comparator 69 to 73 µs by its means): the
   per-statement cost floor that sets the indexed ceiling of about 15,314 qps
   per connection is client, socket and the statement's own setup.
5. **One connection does not use eight cores**: `cores = 8` is 0.94x of `cores = 1` (median), as at the comparator
   (0.95x). Nothing was tried above one connection in this scenario.

## 10. Noise floor, contamination and host stamps

This host has a **slow mode**: a run in which every statement is about 40 % slower for the whole run
(`pk-user` 94 to 96 µs against 64 to 68). It appears here in three of 36 cells, one run each
(`r3-s3-c1-n200-none`, `r2-s3-c1-n10000-none`, `r2-s3-c1-n10000-all`), with no foreign process in
the monitor's samples for those windows; it is the same effect the comparator named. A median of three votes
a single slow run out; for `n=200 none` it picked a run 12 % above the clean value, which is why that
column is the least reliable. `pk-user` mean (µs) per run, the thermometer:

| cores | n | mode | run 1 | run 2 | run 3 | max/min-1 |
|---|---|---|---|---|---|---|
| 1 | 200 | none | 76.4 µs | 68.0 µs | 95.9 µs | 41.0 % |
| 1 | 200 | all | 66.5 µs | 65.3 µs | 71.4 µs | 9.3 % |
| 1 | 1,000 | none | 68.1 µs | 66.6 µs | 66.6 µs | 2.3 % |
| 1 | 1,000 | all | 66.1 µs | 67.6 µs | 66.8 µs | 2.3 % |
| 1 | 10,000 | none | 64.8 µs | 94.0 µs | 64.3 µs | 46.2 % |
| 1 | 10,000 | all | 67.8 µs | 94.1 µs | 65.9 µs | 42.8 % |
| 8 | 200 | none | 69.6 µs | 70.0 µs | 71.0 µs | 2.0 % |
| 8 | 200 | all | 71.1 µs | 70.6 µs | 69.9 µs | 1.7 % |
| 8 | 1,000 | none | 70.0 µs | 77.7 µs | 70.7 µs | 11.0 % |
| 8 | 1,000 | all | 74.5 µs | 71.4 µs | 72.3 µs | 4.3 % |
| 8 | 10,000 | none | 71.0 µs | 68.2 µs | 67.5 µs | 5.2 % |
| 8 | 10,000 | all | 68.3 µs | 69.6 µs | 68.8 µs | 1.9 % |

pk-user mean over 36 cells: min 64.3 median 69.6 max 95.9
cells above 85 µs: 3

Run-to-run spread (max/min - 1) of each cell's per-shape mean, median and worst over the twelve shapes:

| cores | n | mode | median spread over shapes | worst shape | worst spread |
|---|---|---|---|---|---|
| 1 | 200 | none | 38.0 % | count-by-user | 47.6 % |
| 1 | 200 | all | 6.0 % | exists-correlated | 25.9 % |
| 1 | 1,000 | none | 2.9 % | count-by-user | 7.2 % |
| 1 | 1,000 | all | 2.2 % | count-by-user | 31.1 % |
| 1 | 10,000 | none | 6.1 % | pk-user | 46.2 % |
| 1 | 10,000 | all | 37.1 % | count-by-user | 47.5 % |
| 8 | 200 | none | 2.0 % | books-by-genre | 12.3 % |
| 8 | 200 | all | 2.0 % | count-by-user | 10.8 % |
| 8 | 1,000 | none | 9.6 % | books-by-genre | 11.9 % |
| 8 | 1,000 | all | 15.1 % | books-by-author | 40.0 % |
| 8 | 10,000 | none | 1.8 % | resv-by-user | 19.7 % |
| 8 | 10,000 | all | 2.1 % | loans-by-user | 4.7 % |

The medians are the spreads of a cell where no run was slow (2 to 15 %); the 37 to 38 % medians are the
three slow-mode cells. **A difference under about 15 % in a single cell, or under 10 % in the geometric mean of
a column, is not a result.** The comparator's own noise was 1.6 to 8.2 % per shape with four groups at
21 to 33 %.

**Contamination.** Another session's builds and test suite ran on this host during the measurement. Two cells were
discarded and re-run because the monitor saw a build or test process in their window: `s3-c1-n1000-all` (one
attempt) and `s3-c8-n200-none` (one attempt); their discarded outputs are in `contaminated/` in the archive.
Every reported cell has no build or test sample. The device thermometer, the `load` phase p50
(1,028 to 1,133 µs over all 36 cells, one commit each), shows no stall; `recovery_checkpoint_us` was not sampled.

| cell | window UTC | loadavg 1/5/15 | gate wait | build procs | driver exit | verify problems | samples | build/test | foreign >= 50 % | minor |
|---|---|---|---|---|---|---|---|---|---|---|
| `s3-c1-n200-none` | 10-13 23:53:32 to 23:53:33 | 4.66 / 7.30 / 5.77 | 0 s | none | exit 0 | 0 problems | 1 | 0 | 0 | 0 |
| `s3-c1-n200-all` | 10-13 23:53:36 to 23:53:38 | 4.37 / 7.19 / 5.75 | 0 s | none | exit 0 | 0 problems | 1 | 0 | 0 | 0 |
| `s3-c1-n1000-none` | 10-13 23:53:41 to 23:53:44 | 4.10 / 7.09 / 5.72 | 0 s | none | exit 0 | 0 problems | 2 | 0 | 0 | 2 |
| `s3-c1-n1000-all` | 10-14 00:07:51 to 00:07:54 | 4.90 / 6.96 / 6.17 | 0 s | none | exit 0 | 0 problems | 2 | 0 | 0 | 0 |
| `s3-c1-n10000-none` | 10-14 00:08:01 to 00:08:28 | 4.22 / 6.74 / 6.11 | 0 s | none | exit 0 | 0 problems | 13 | 0 | 0 | 3 |
| `s3-c1-n10000-all` | 10-14 00:08:35 to 00:09:01 | 2.64 / 6.07 / 5.91 | 0 s | none | exit 0 | 0 problems | 12 | 0 | 0 | 4 |
| `s3-c8-n200-none` | 10-14 00:16:41 to 00:16:43 | 8.79 / 8.02 / 6.79 | 280 s | none | exit 0 | 0 problems | 1 | 0 | 0 | 0 |
| `s3-c8-n200-all` | 10-14 00:16:50 to 00:16:51 | 7.52 / 7.77 / 6.73 | 0 s | none | exit 0 | 0 problems | 1 | 0 | 0 | 0 |
| `s3-c8-n1000-none` | 10-14 00:16:59 to 00:17:03 | 6.59 / 7.57 / 6.67 | 0 s | none | exit 0 | 0 problems | 2 | 0 | 0 | 1 |
| `s3-c8-n1000-all` | 10-14 00:17:10 to 00:17:13 | 5.65 / 7.33 / 6.60 | 0 s | none | exit 0 | 0 problems | 2 | 0 | 0 | 1 |
| `s3-c8-n10000-none` | 10-14 00:17:20 to 00:17:48 | 4.86 / 7.11 / 6.54 | 0 s | none | exit 0 | 0 problems | 13 | 0 | 0 | 2 |
| `s3-c8-n10000-all` | 10-14 00:17:55 to 00:18:22 | 3.07 / 6.41 / 6.33 | 0 s | none | exit 0 | 0 problems | 13 | 0 | 0 | 0 |
| `r2-s3-c1-n200-none` | 10-14 00:18:29 to 00:18:31 | 2.20 / 5.83 / 6.13 | 0 s | none | exit 0 | 0 problems | 1 | 0 | 0 | 0 |
| `r2-s3-c1-n200-all` | 10-14 00:18:38 to 00:18:39 | 1.86 / 5.64 / 6.07 | 0 s | none | exit 0 | 0 problems | 0 | 0 | 0 | 0 |
| `r2-s3-c1-n1000-none` | 10-14 00:18:46 to 00:18:50 | 1.71 / 5.54 / 6.03 | 0 s | none | exit 0 | 0 problems | 2 | 0 | 0 | 0 |
| `r2-s3-c1-n1000-all` | 10-14 00:18:57 to 00:19:00 | 1.52 / 5.38 / 5.97 | 0 s | none | exit 0 | 0 problems | 2 | 0 | 0 | 0 |
| `r2-s3-c1-n10000-none` | 10-14 00:19:07 to 00:19:33 | 1.25 / 5.13 / 5.88 | 0 s | none | exit 0 | 0 problems | 12 | 0 | 0 | 1 |
| `r2-s3-c1-n10000-all` | 10-14 00:19:40 to 00:20:06 | 1.07 / 4.71 / 5.72 | 0 s | none | exit 0 | 0 problems | 13 | 0 | 0 | 0 |
| `r2-s3-c8-n200-none` | 10-14 00:20:13 to 00:20:14 | 1.04 / 4.30 / 5.54 | 0 s | none | exit 0 | 0 problems | 1 | 0 | 0 | 0 |
| `r2-s3-c8-n200-all` | 10-14 00:20:21 to 00:20:23 | 0.96 / 4.23 / 5.51 | 0 s | none | exit 0 | 0 problems | 1 | 0 | 0 | 0 |
| `r2-s3-c8-n1000-none` | 10-14 00:20:30 to 00:20:33 | 0.88 / 4.11 / 5.46 | 0 s | none | exit 0 | 0 problems | 2 | 0 | 0 | 0 |
| `r2-s3-c8-n1000-all` | 10-14 00:20:40 to 00:20:44 | 0.82 / 3.99 / 5.41 | 0 s | none | exit 0 | 0 problems | 3 | 0 | 0 | 0 |
| `r2-s3-c8-n10000-none` | 10-14 00:20:51 to 00:21:17 | 0.77 / 3.87 / 5.35 | 0 s | none | exit 0 | 0 problems | 12 | 0 | 0 | 0 |
| `r2-s3-c8-n10000-all` | 10-14 00:21:24 to 00:21:50 | 0.85 / 3.55 / 5.19 | 0 s | none | exit 0 | 0 problems | 12 | 0 | 0 | 0 |
| `r3-s3-c1-n200-none` | 10-14 00:21:57 to 00:21:59 | 0.96 / 3.32 / 5.06 | 0 s | none | exit 0 | 0 problems | 1 | 0 | 0 | 0 |
| `r3-s3-c1-n200-all` | 10-14 00:22:06 to 00:22:07 | 0.89 / 3.23 / 5.01 | 0 s | none | exit 0 | 0 problems | 1 | 0 | 0 | 0 |
| `r3-s3-c1-n1000-none` | 10-14 00:22:15 to 00:22:18 | 0.98 / 3.17 / 4.97 | 0 s | none | exit 0 | 0 problems | 2 | 0 | 0 | 0 |
| `r3-s3-c1-n1000-all` | 10-14 00:22:25 to 00:22:28 | 0.98 / 3.10 / 4.93 | 0 s | none | exit 0 | 0 problems | 1 | 0 | 0 | 0 |
| `r3-s3-c1-n10000-none` | 10-14 00:22:36 to 00:23:01 | 0.90 / 3.01 / 4.88 | 0 s | none | exit 0 | 0 problems | 12 | 0 | 0 | 0 |
| `r3-s3-c1-n10000-all` | 10-14 00:23:08 to 00:23:34 | 0.86 / 2.77 / 4.73 | 0 s | none | exit 0 | 0 problems | 12 | 0 | 0 | 0 |
| `r3-s3-c8-n200-none` | 10-14 00:23:41 to 00:23:43 | 0.84 / 2.58 / 4.61 | 0 s | none | exit 0 | 0 problems | 1 | 0 | 0 | 0 |
| `r3-s3-c8-n200-all` | 10-14 00:23:50 to 00:23:52 | 0.86 / 2.53 / 4.57 | 0 s | none | exit 0 | 0 problems | 1 | 0 | 0 | 0 |
| `r3-s3-c8-n1000-none` | 10-14 00:23:59 to 00:24:02 | 0.73 / 2.45 / 4.52 | 0 s | none | exit 0 | 0 problems | 2 | 0 | 0 | 0 |
| `r3-s3-c8-n1000-all` | 10-14 00:24:09 to 00:24:13 | 0.77 / 2.40 / 4.48 | 0 s | none | exit 0 | 0 problems | 3 | 0 | 0 | 0 |
| `r3-s3-c8-n10000-none` | 10-14 00:24:20 to 00:24:47 | 0.80 / 2.35 / 4.44 | 0 s | none | exit 0 | 0 problems | 13 | 0 | 0 | 0 |
| `r3-s3-c8-n10000-all` | 10-14 00:24:54 to 00:25:20 | 0.74 / 2.17 / 4.30 | 0 s | none | exit 0 | 0 problems | 13 | 0 | 0 | 0 |

Archive: `bench/v3.1.0/archive/scenario3-v3.1.0-30-gcd7c7c4e/`.
