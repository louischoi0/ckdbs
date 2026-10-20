# Scenario 3 (library) at `v3.3.0-33-g9682b725`

**One line.** Twelve read shapes over four relations at 200, 1,000 and 10,000 loans, with and without server indexes, at `cores = 1` and `cores = 8`, three runs each: at 10,000 loans the engine is
where it was at v3.1.0 on 11 of 12 shapes (geometric means 1.07x unindexed and 0.98x indexed; the four smaller columns, 0.84x to 0.96x, sit inside spreads of 20 % to 40 % and are not read), and on one it is
**1.98x faster: the unindexed two-relation join `join-no-literal` at 10,000 loans, 1,683 qps against 851 qps** (spread 2.7 % now, 2.4 % at v3.1.0), with the plan
reporting `inner_built=1` (a statement-local hash build of `loans`, 79 rows built from one 10,074-row scan instead of one scan per outer row). That is **not the join planner's order choice** (CB-S2): the same
cell with `join_order = written` is 1,719 qps, 5 % faster than the planned arm (section 6). Both arms carry CB-S1's derived range bounds on `loans`, so the A/B does not
exclude CB-S1 as the cause. Against PostgreSQL 18.6, **re-measured in the same window**, the unindexed 10,000-loan columns are
1.06x (geometric mean over 12 shapes) and the indexed ones 0.98x; the unindexed equality (`loans-by-user`) is 1.15x of PostgreSQL's seqscan. At 200 loans the
`cores = 1` cells are too noisy to read (section 10): 0.79x unindexed and 1.16x indexed.

Driver: `tools/scenario3_library.py`, unmodified, `--index-mode none|all --server-indexes on --seed 1` (`--assert-index-reads` on the indexed cells), the v3.1.0 run's arguments. Measured by `ck-tester`.
Comparator (cross-version): `bench/v3.1.0/results-scenario3-library-v3.1.0-30-gcd7c7c4e.md`, re-derived from its archived JSON (three runs per cell). PostgreSQL 18.6:
`tools/pg_scenario3_library.py --user cdkbs --index-mode none|all`, **fresh, three runs per cell**, at its defaults.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; KDS cells 03:58:09 to 04:08:43 UTC (72 cells: 2 core counts x 3 sizes x 2 index modes x 3 runs); PostgreSQL cells 04:23:14 to 04:32:06 UTC (18 cells); join-order probe 04:34:04 to 04:44:03 UTC (8 cells) |
| Worktree / branch | `bench-scenarios-1-4` / `bench-scenarios-1-4` (the main checkout is not used) |
| HEAD | `9682b725`, `git describe --tags` = `v3.3.0-33-g9682b725` ("Merge fix-fk-self-reference-refused: a self-referencing foreign key is refused NotImplemented at both doors"), committed 2026-10-20 03:33:40 UTC; equal to `origin/main` when the run began |
| Tree cleanliness | Clean (`git status` empty) at the start and at the end of the measurement; nothing under `src/`, `include/`, `tools/` or `CMakeLists.txt` was edited, and the drivers are the unmodified `tools/` scripts. `build-release/` is ignored by git. The results files under `bench/v3.3.0/` are uncommitted additions until the commit that carries them |
| Binary provenance | `build-release/kds_server`, built in this worktree at HEAD with `cmake -DCMAKE_BUILD_TYPE=Release` (`CMakeCache.txt`: `Release`), target `kds_server`, `-j4`, finished 2026-10-20 03:36:57 UTC, 3 min 17 s **after** the HEAD commit, so the binary is not older than the engine. Copied to `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/kds_server` before the first cell; **copy sha256 `7532bd14afb2acd4f9e48cac00732f899cb8e59b26fa1d9b982047fc89272f2a`** (equal to the source binary's, checked again after the run); source binary mtime 2026-10-20 03:36:57.38 UTC. Every server started from the copy; no build ran during any reported cell (the only build of the session finished before the first cell) |
| Device | data files under `/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/data/`, `/dev/root`, `ext4` (`df -T`); never tmpfs. A fresh data file per cell (per server in scenario 4), deleted when the cell finished. The PostgreSQL cluster (`/home/cdkbs/pg-bench-ba`) is on the same device |
| Build type | Release |
| Host | 8 logical CPUs, AMD EPYC 9V74 (1 socket x 4 cores x 2 threads), Linux 7.0.0-1014-azure, a shared development VM; `/` was 95 % full (14 GB free) |
| Time budget | **Budget lifted by the operator for this run** (order of 2026-10-20: "lift the budget, re-measure PG too"). Every cell of the v3.1.0 rebaseline's plan ran, with its repeat counts, and PostgreSQL was re-measured fresh in the same window. Nothing was cut |
| Prior run of this shape | `bench/v3.1.0/results-scenario3-library-v3.1.0-30-gcd7c7c4e.md` (KDS binary `cd7c7c4e`, sha256 `4752f562...`, measured 2026-10-13 23:38 UTC to 2026-10-14 01:39 UTC). The two commits are 240 commits apart (`git rev-list --count cd7c7c4e..HEAD`), 101 of them first-parent and 23 of those merges, with 178 files changed under `src/`, `include/` and `CMakeLists.txt` (milestones BV to CD among them), so **every KDS delta below is cross-version and attributable to no single change**. The host and its device state are also a week-old comparison: v3.1.0's cells ran on the same VM, about 6.2 days earlier |
| Server config | `log_level = warn`, `buffer_pool_frames = 65536` (required since BE), `cores` and `durability` per cell, all else default. Ports 15604 to 15607, 15610, 15620, 15651 to 15653, never 15432 (held by an unrelated `kds_server`) |
| Quiet host | before each cell a gate waited for no `cc1plus` / `cmake --build` / `ctest` / `kds_tests` process and at most 8 % busy CPU over 3 s (`lib.sh` in the archive); `/proc/loadavg` and the process list are in each cell's precheck. **A 1-second monitor ran through the whole run** (`monitor.log`: 1,500+ samples of foreign processes above 10 % CPU) and every cell was checked against it after it ended (`cellvalid.py`): a build/test process or a foreign process at 50 % CPU or more inside the window discards and re-runs the cell. **No cell was discarded**: the monitor saw only a `claude` process at 11 to 23 % of one CPU in 23 samples of the 1,518, all outside the 50 % line. One batch of PostgreSQL cells failed on a connection error and is listed under contamination below |
| PostgreSQL | PostgreSQL 18.6 (`/home/cdkbs/pg-bench-ba`, port 15434), defaults (`shared_buffers = 128MB`, `synchronous_commit = on`, `fsync = on`, `max_wal_size = 1GB`), not started or stopped by this run. **Re-measured fresh in this window** (the twin runs follow the KDS cells, never concurrent with them) |
| Driver | **unmodified** `tools/scenario3_library.py`; exit 0 in every cell; `verify_problems` empty in 36 of 36 KDS cells |

## 2. What was run

| axis | values |
|---|---|
| cores | 1, 8 (`group` durability; reads only, so durability does not enter) |
| index mode | `none` (the driver creates no index) and `all` (it creates the indexes each shape can use, after the load); `--server-indexes on` |
| rows (rule 9) | `--loans 200`, `1000`, `10000`; users, books and reservations scale with it (`sizes` in each JSON: at 200 loans 40 / 40 / 100 relation rows, at 10,000 loans 2,000 / 2,000 / 5,000) |
| runs | 3 per cell (`r2-`, `r3-` prefixes); a fresh server and data file per cell |
| shapes | 12, 200 operations each, serial on one connection, so the **qps is derived**: 1,000,000 / mean µs |
| PostgreSQL | `pg_scenario3_library.py` at the same `--loans` and `--index-mode`, `--seed 1`, 3 runs per cell, no `cores` axis |
| join-order probe | `join_order = planned` (default) and `written`, 10,000 loans, `none` x 3 runs and `all` x 1 run, with the full ANALYZE replies dumped |

Cut: nothing. Throughput below is derived qps (1,000,000 / mean µs), the median of three runs.

## 3. Throughput

**What counts as a finding.** Each KDS cell was run three times; the spread (max minus min over the median, shown beside every median) is this run's own noise floor. A delta between two medians that is smaller than the larger of the two spreads is not a finding, and the cross-version columns are held to the same rule with v3.1.0's spread. Cells whose spread is above 20 % (most of the 200- and 1,000-loan `cores = 1` cells, where one run in three falls into a slow mode, section 10) are reported but carry no delta claim.

`cores = 1` and `cores = 8`:

`cores = 1`:

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 11,099 qps | 14,706 qps | 14,184 qps | 10,060 qps | 14,641 qps | 14,859 qps |
| loans-by-user | 8,496 qps | 11,521 qps | 8,251 qps | 8,210 qps | 2,779 qps | 11,001 qps |
| loans-by-book | 8,787 qps | 12,121 qps | 7,348 qps | 10,965 qps | 2,828 qps | 11,507 qps |
| resv-by-user | 9,891 qps | 13,908 qps | 10,965 qps | 12,987 qps | 4,950 qps | 13,245 qps |
| books-by-author | 9,443 qps | 13,038 qps | 12,315 qps | 8,921 qps | 7,267 qps | 11,990 qps |
| books-by-genre | 10,204 qps | 13,755 qps | 9,452 qps | 7,062 qps | 2,092 qps | 2,093 qps |
| loans-by-daterange | 7,930 qps | 10,638 qps | 4,509 qps | 4,036 qps | 652 qps | 657 qps |
| overdue | 11,086 qps | 10,929 qps | 5,675 qps | 5,079 qps | 873 qps | 1,052 qps |
| join-loan-user | 9,728 qps | 9,625 qps | 8,130 qps | 7,536 qps | 2,728 qps | 9,302 qps |
| join-no-literal | 3,709 qps | 3,432 qps | 3,311 qps | 2,946 qps | 1,683 qps | 3,408 qps |
| exists-correlated | 7,651 qps | 6,974 qps | 5,754 qps | 6,072 qps | 1,596 qps | 6,863 qps |
| count-by-user | 15,244 qps | 15,198 qps | 11,429 qps | 14,535 qps | 3,031 qps | 14,641 qps |

`cores = 8`:

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 13,870 qps | 13,793 qps | 13,643 qps | 13,387 qps | 13,928 qps | 14,306 qps |
| loans-by-user | 10,929 qps | 10,811 qps | 8,084 qps | 10,309 qps | 2,547 qps | 10,352 qps |
| loans-by-book | 11,561 qps | 11,287 qps | 8,460 qps | 9,960 qps | 2,598 qps | 11,274 qps |
| resv-by-user | 13,459 qps | 12,987 qps | 10,834 qps | 10,121 qps | 4,693 qps | 12,531 qps |
| books-by-author | 12,937 qps | 11,947 qps | 11,223 qps | 10,471 qps | 6,845 qps | 11,561 qps |
| books-by-genre | 13,587 qps | 12,821 qps | 7,138 qps | 7,994 qps | 2,014 qps | 2,017 qps |
| loans-by-daterange | 10,194 qps | 9,785 qps | 3,954 qps | 4,139 qps | 621 qps | 623 qps |
| overdue | 10,893 qps | 10,965 qps | 4,776 qps | 5,841 qps | 829 qps | 997 qps |
| join-loan-user | 9,311 qps | 9,268 qps | 6,693 qps | 9,083 qps | 2,540 qps | 9,183 qps |
| join-no-literal | 3,435 qps | 3,213 qps | 2,915 qps | 3,068 qps | 1,554 qps | 3,110 qps |
| exists-correlated | 7,138 qps | 6,553 qps | 4,636 qps | 6,418 qps | 1,574 qps | 6,390 qps |
| count-by-user | 14,409 qps | 14,556 qps | 9,709 qps | 13,532 qps | 2,695 qps | 14,006 qps |

`cores = 8` reads 0.76x to 1.37x of `cores = 1` over the 72 cells (median 0.95x) and is the slower of the two in 54 of 72: one serial connection lands on one core and the other seven have nothing to do, so no gain is expected; the
loss is inside the cell-to-cell spread of the `pk-user` control and is not a finding.

**Row-count independence of the control.** `pk-user`, a `Lookup`, does not scale with rows or index mode: 11,099 to 14,859 qps across the six `cores = 1` cells, 14,184 to 14,859 qps in four of them. The two low
cells (`n=200 none` 11,099 and `n=1,000 all` 10,060 qps) are the slow-mode cells of section 10, with spreads of 43 % and 43 %.

Spread of the three runs per cell (the noise floor of each cell), `cores = 1` then `cores = 8`:

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 42.8 % | 0.7 % | 4.7 % | 42.6 % | 26.8 % | 1.6 % |
| loans-by-user | 36.2 % | 1.9 % | 21.0 % | 32.8 % | 9.8 % | 1.4 % |
| loans-by-book | 37.0 % | 13.0 % | 31.0 % | 26.7 % | 3.1 % | 18.4 % |
| resv-by-user | 13.0 % | 31.3 % | 29.0 % | 29.0 % | 11.0 % | 1.3 % |
| books-by-author | 24.5 % | 30.1 % | 28.6 % | 38.0 % | 21.1 % | 4.1 % |
| books-by-genre | 39.6 % | 30.2 % | 13.8 % | 28.7 % | 6.2 % | 2.1 % |
| loans-by-daterange | 33.2 % | 25.7 % | 6.8 % | 11.6 % | 1.4 % | 1.0 % |
| overdue | 28.5 % | 21.1 % | 18.4 % | 25.5 % | 2.1 % | 0.4 % |
| join-loan-user | 27.0 % | 28.1 % | 2.3 % | 30.6 % | 9.8 % | 7.7 % |
| join-no-literal | 2.2 % | 14.9 % | 3.9 % | 7.0 % | 2.7 % | 3.0 % |
| exists-correlated | 0.1 % | 19.6 % | 1.1 % | 24.9 % | 2.8 % | 5.1 % |
| count-by-user | 0.6 % | 30.5 % | 1.9 % | 33.2 % | 3.7 % | 3.7 % |

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 5.9 % | 4.2 % | 2.8 % | 27.6 % | 6.2 % | 1.1 % |
| loans-by-user | 6.4 % | 4.0 % | 1.4 % | 8.9 % | 1.9 % | 2.9 % |
| loans-by-book | 6.9 % | 3.8 % | 0.2 % | 13.5 % | 1.0 % | 1.0 % |
| resv-by-user | 4.4 % | 2.7 % | 2.1 % | 26.2 % | 1.6 % | 10.7 % |
| books-by-author | 4.8 % | 4.6 % | 27.0 % | 20.0 % | 1.8 % | 29.0 % |
| books-by-genre | 4.8 % | 4.4 % | 26.7 % | 17.8 % | 1.9 % | 7.8 % |
| loans-by-daterange | 5.3 % | 4.2 % | 12.9 % | 15.5 % | 0.8 % | 2.6 % |
| overdue | 2.8 % | 3.7 % | 15.2 % | 11.1 % | 1.4 % | 0.0 % |
| join-loan-user | 5.0 % | 1.7 % | 10.1 % | 24.5 % | 2.4 % | 12.4 % |
| join-no-literal | 4.4 % | 4.0 % | 2.8 % | 3.8 % | 2.5 % | 9.3 % |
| exists-correlated | 3.7 % | 2.0 % | 9.6 % | 3.5 % | 3.9 % | 2.4 % |
| count-by-user | 5.4 % | 1.6 % | 10.0 % | 15.4 % | 4.4 % | 3.8 % |

## 4. Delta against the previous run of this shape (cross-version)

Ratio of median qps, now over v3.1.0 (above 1.00x = faster now), 240 commits apart, so no difference is attributable to one change. **Read it with the spreads above**: at 200 and 1,000
loans most cells carry 20 % to 40 % spreads (a cell is 200 statements of 100 µs, 20 ms, and one scheduler stall is a third of it), so the ratios there are not findings. At 10,000 loans the
spreads of the unindexed cells are 1 % to 10 % and the ratios are readable.

`cores = 1`:

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 0.85x | 0.98x | 0.94x | 0.67x | 0.95x | 1.01x |
| loans-by-user | 0.84x | 0.96x | 0.89x | 0.72x | 1.00x | 0.98x |
| loans-by-book | 0.72x | 0.96x | 0.76x | 0.90x | 1.04x | 0.95x |
| resv-by-user | 0.90x | 0.97x | 0.88x | 0.96x | 1.02x | 0.97x |
| books-by-author | 0.96x | 0.98x | 0.96x | 0.71x | 1.03x | 0.97x |
| books-by-genre | 0.99x | 0.97x | 0.96x | 0.75x | 0.99x | 0.99x |
| loans-by-daterange | 0.97x | 0.96x | 0.95x | 0.87x | 0.99x | 1.02x |
| overdue | 1.11x | 0.90x | 0.97x | 0.82x | 1.00x | 1.01x |
| join-loan-user | 1.25x | 0.90x | 0.97x | 0.71x | 1.07x | 0.91x |
| join-no-literal | 1.04x | 0.94x | 1.12x | 0.85x | 1.98x | 0.97x |
| exists-correlated | 0.99x | 0.98x | 1.01x | 1.08x | 1.01x | 0.99x |
| count-by-user | 0.98x | 0.95x | 0.99x | 1.23x | 1.04x | 0.96x |

`cores = 8`:

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 0.97x | 0.97x | 0.96x | 0.97x | 0.95x | 0.98x |
| loans-by-user | 1.00x | 0.97x | 0.98x | 0.96x | 1.02x | 0.95x |
| loans-by-book | 0.99x | 0.96x | 1.00x | 0.88x | 1.03x | 0.96x |
| resv-by-user | 0.99x | 0.95x | 0.97x | 0.81x | 1.02x | 0.94x |
| books-by-author | 0.99x | 0.95x | 0.94x | 0.88x | 1.00x | 0.96x |
| books-by-genre | 0.99x | 0.95x | 0.79x | 0.92x | 0.99x | 1.00x |
| loans-by-daterange | 0.99x | 0.95x | 0.89x | 0.98x | 1.00x | 0.99x |
| overdue | 0.95x | 0.96x | 0.89x | 1.00x | 1.00x | 0.99x |
| join-loan-user | 0.94x | 0.92x | 0.89x | 0.92x | 1.05x | 0.93x |
| join-no-literal | 0.96x | 0.95x | 1.09x | 0.96x | 1.90x | 0.94x |
| exists-correlated | 1.00x | 1.01x | 0.89x | 1.03x | 1.01x | 0.98x |
| count-by-user | 0.97x | 0.97x | 0.95x | 0.94x | 1.01x | 0.96x |

At 10,000 loans, unindexed, `cores = 1`: eleven of the twelve shapes are within 0.95x to 1.07x (`exists-correlated` 1.01x, `join-loan-user` 1.07x); `join-no-literal` is **1.98x** (851 qps to 1,683 qps, spreads 2.7 % and 2.4 %) and `cores = 8` shows the same
step (1,554 qps).
Geometric mean over the 12 shapes, per column, `cores = 1` (new over old): 0.96x, 0.95x, 0.95x, 0.84x, 1.07x, 0.98x for n=200 none, n=200 all, n=1,000 none, n=1,000 all, n=10,000 none, n=10,000 all.

## 5. What the index buys now

Throughput of the indexed cell over the unindexed one, `cores = 1`, median of three, with PostgreSQL's ratio at 10,000 loans beside it (a B-tree on the same columns):

| shape | n=200: all over none | n=1,000: all over none | n=10,000: all over none | n=10,000 none (qps) | n=10,000 all (qps) | PG18 n=10,000: all over none |
|---|---|---|---|---|---|---|
| pk-user | 1.32x | 0.71x | 1.01x | 14,641 qps | 14,859 qps | 1.00x |
| loans-by-user | 1.36x | 1.00x | 3.96x | 2,779 qps | 11,001 qps | 4.47x |
| loans-by-book | 1.38x | 1.49x | 4.07x | 2,828 qps | 11,507 qps | 5.09x |
| resv-by-user | 1.41x | 1.18x | 2.68x | 4,950 qps | 13,245 qps | 3.04x |
| books-by-author | 1.38x | 0.72x | 1.65x | 7,267 qps | 11,990 qps | 1.80x |
| books-by-genre | 1.35x | 0.75x | 1.00x | 2,092 qps | 2,093 qps | 1.23x |
| loans-by-daterange | 1.34x | 0.90x | 1.01x | 652 qps | 657 qps | 1.00x |
| overdue | 0.99x | 0.89x | 1.21x | 873 qps | 1,052 qps | 1.68x |
| join-loan-user | 0.99x | 0.93x | 3.41x | 2,728 qps | 9,302 qps | 3.84x |
| join-no-literal | 0.93x | 0.89x | 2.02x | 1,683 qps | 3,408 qps | 0.98x |
| exists-correlated | 0.91x | 1.06x | 4.30x | 1,596 qps | 6,863 qps | 6.68x |
| count-by-user | 1.00x | 1.27x | 4.83x | 3,031 qps | 14,641 qps | 4.73x |

An index is worth nothing at 200 loans (the walk is two pages and the statement is a round trip) and about 4x at 10,000 on the equality shapes (`loans-by-user` 3.96x,
PostgreSQL 4.47x). Two shapes gain nothing on KDS: `books-by-genre` (1/16 of the relation, about 125 rows returned, plan stays `IndexProbe` but the cost is the rows) and `loans-by-daterange` (plan stays `Scan`); PostgreSQL gains 1.2x on `books-by-genre` and 1.0x on `loans-by-daterange`. `overdue` gains 1.2x on KDS and 1.7x on PostgreSQL.

## 6. The join planner (CB/CC) on the library's joins

ANALYZE of the three join shapes at 10,000 loans, unindexed and indexed (`pl-s3-planned-none-r1`, `pl-s3-planned-all-r1`; the replies in full are in the archive):

| shape | unindexed plan | `reordered` | examined | indexed plan |
|---|---|---|---|---|
| `join-no-literal` | `Range users` [1, 16] -> `Scan loans`, **`build on=col1`**, `inner_built=1`, `build_rows=79`, `build_probes=15` (order = written, est 0:16 1:91, gen 1) | no | 10,106 (pages 159) | `Range users` -> `IndexProbe loans` |
| `exists-correlated` | `Range users` [1, 20] with a correlated `EXISTS` sub-chain `Scan loans` (`build on=col1`, `inner_built=0`, `build_rows=4,603`, `corr_scans=3`) | n/a (one outer relation) | 4,652 (pages 61) | `Range users` -> `IndexProbe loans` |
| `join-loan-user` | not ANALYZEd by the driver (its plan is not recorded) | - | - | - |

The planner priced both orders of `join-no-literal`, kept the written one (`switch=1`), and the build on the inner `Scan` is what removed the per-outer-row scan: 10,074 rows examined for a 16-row outer
instead of 16 x 10,000. A and B, the planned default against `join_order = written` on the same binary:

Scenario 3, `cores = 1`, `group`, 10,000 loans: planned against `join_order = written`.

| cell | shape | planned | spread | written | spread | planned over written |
|---|---|---|---|---|---|---|
| n=10,000 none (3 runs) | join-loan-user | 2,771 qps | 8.2 % | 2,786 qps | 1.0 % | 0.99x |
| n=10,000 none (3 runs) | join-no-literal | 1,636 qps | 4.7 % | 1,719 qps | 1.4 % | 0.95x |
| n=10,000 none (3 runs) | exists-correlated | 1,621 qps | 3.0 % | 1,645 qps | 1.4 % | 0.99x |
| n=10,000 all (1 runs) | join-loan-user | 9,533 qps | n/a | 10,132 qps | n/a | 0.94x |
| n=10,000 all (1 runs) | join-no-literal | 3,043 qps | n/a | 3,291 qps | n/a | 0.92x |
| n=10,000 all (1 runs) | exists-correlated | 5,841 qps | n/a | 6,821 qps | n/a | 0.86x |

**Both arms have the same plan** (`inner_built=1` in each; the `written` arm shows no `planned order=` line because it compiles no planned block), and the planned arm is 1 % to 5 % *slower* on the three join shapes unindexed (three runs; spreads 1 % to 8 %) and 6 % to 14 % slower indexed (one run, so no claim). The planner chose what the written order already was. So the 1.98x that this scenario shows against v3.1.0 is not CB-S2's order choice. Both arms also carry the
range bounds CB-S1 derives across the join-key equality (`filter 0:1.1 >= 1 derived`, `<= 16 derived` on `loans`), which hold the build to 79 rows, so the A/B does
**not** exclude CB-S1. The v3.1.0 JSON records only the step kinds (`Range`, `Scan`), not `inner_built`, so this run cannot say whether v3.1.0 built. One bound it does give: 16 full
scans of `loans` at the walk cost of section 8 (about 24 ns per row) would be about 3.9 ms a statement, and v3.1.0's cell ran 851 qps (1,175 µs), so v3.1.0 was most likely
not scanning once per outer row either. Candidate causes, none separated: CB-S1's derived bounds, the inner build's reach (`join-inner-build.md`), or the walk beneath it, 240 commits apart.

## 7. Percentiles

Run 1 of each `cores = 1` cell, every phase (microseconds; the five percentiles are the driver's). The `cores = 8` cells' JSON is in the archive.

#### `s3-c1-n200-none` run 1 (cores = 1)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 4 ops | 1,328.0 µs | 1,328.0 µs | 1,425.2 µs | 2,095.7 µs | 2,095.7 µs | 2,095.7 µs | 0 errors |
| load | 380 ops | 975.8 µs | 1,086.6 µs | 1,148.3 µs | 1,559.7 µs | 1,888.7 µs | 9,105.4 µs | 0 errors |
| pk-user | 200 ops | 61.7 µs | 73.6 µs | 91.1 µs | 118.5 µs | 141.5 µs | 203.3 µs | 0 errors |
| loans-by-user | 200 ops | 93.1 µs | 109.0 µs | 115.9 µs | 137.1 µs | 149.0 µs | 150.1 µs | 0 errors |
| loans-by-book | 200 ops | 97.2 µs | 105.3 µs | 113.2 µs | 131.9 µs | 138.7 µs | 188.3 µs | 0 errors |
| resv-by-user | 200 ops | 86.8 µs | 96.6 µs | 99.6 µs | 119.8 µs | 126.1 µs | 131.6 µs | 0 errors |
| books-by-author | 200 ops | 92.3 µs | 98.2 µs | 106.7 µs | 121.0 µs | 134.0 µs | 138.0 µs | 0 errors |
| books-by-genre | 200 ops | 64.2 µs | 93.4 µs | 98.5 µs | 117.5 µs | 125.1 µs | 131.1 µs | 0 errors |
| loans-by-daterange | 200 ops | 105.0 µs | 116.6 µs | 123.9 µs | 147.9 µs | 158.5 µs | 179.2 µs | 0 errors |
| overdue | 200 ops | 65.9 µs | 80.0 µs | 84.7 µs | 125.8 µs | 131.2 µs | 136.5 µs | 0 errors |
| join-loan-user | 200 ops | 79.4 µs | 94.2 µs | 100.4 µs | 123.7 µs | 138.6 µs | 148.9 µs | 0 errors |
| join-no-literal | 200 ops | 256.2 µs | 263.1 µs | 271.4 µs | 292.4 µs | 302.3 µs | 350.3 µs | 0 errors |
| exists-correlated | 200 ops | 122.6 µs | 124.5 µs | 126.0 µs | 152.7 µs | 158.1 µs | 167.3 µs | 0 errors |
| count-by-user | 200 ops | 61.0 µs | 62.2 µs | 63.0 µs | 79.2 µs | 92.7 µs | 93.4 µs | 0 errors |

#### `s3-c1-n200-all` run 1 (cores = 1)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 4 ops | 1,164.8 µs | 1,164.8 µs | 1,318.8 µs | 1,899.9 µs | 1,899.9 µs | 1,899.9 µs | 0 errors |
| load | 380 ops | 1,015.0 µs | 1,167.3 µs | 1,216.3 µs | 1,562.4 µs | 2,861.8 µs | 6,300.1 µs | 0 errors |
| create-index | 8 ops | 1,161.5 µs | 1,173.9 µs | 1,340.4 µs | 1,565.8 µs | 1,565.8 µs | 1,565.8 µs | 0 errors |
| pk-user | 200 ops | 61.0 µs | 62.8 µs | 64.5 µs | 82.4 µs | 104.8 µs | 188.9 µs | 0 errors |
| loans-by-user | 200 ops | 62.2 µs | 78.4 µs | 85.9 µs | 107.0 µs | 120.7 µs | 130.0 µs | 0 errors |
| loans-by-book | 200 ops | 64.3 µs | 76.2 µs | 81.4 µs | 98.1 µs | 106.8 µs | 117.5 µs | 0 errors |
| resv-by-user | 200 ops | 58.9 µs | 67.4 µs | 71.1 µs | 86.6 µs | 96.2 µs | 99.3 µs | 0 errors |
| books-by-author | 200 ops | 64.1 µs | 68.9 µs | 77.4 µs | 91.5 µs | 102.5 µs | 105.6 µs | 0 errors |
| books-by-genre | 200 ops | 63.6 µs | 67.5 µs | 71.0 µs | 88.1 µs | 97.1 µs | 103.0 µs | 0 errors |
| loans-by-daterange | 200 ops | 73.1 µs | 85.9 µs | 91.7 µs | 115.7 µs | 122.7 µs | 133.2 µs | 0 errors |
| overdue | 200 ops | 64.6 µs | 81.6 µs | 86.9 µs | 139.9 µs | 159.1 µs | 167.3 µs | 0 errors |
| join-loan-user | 200 ops | 81.0 µs | 94.7 µs | 101.9 µs | 126.7 µs | 154.5 µs | 184.1 µs | 0 errors |
| join-no-literal | 200 ops | 272.3 µs | 282.3 µs | 289.6 µs | 311.0 µs | 319.4 µs | 356.4 µs | 0 errors |
| exists-correlated | 200 ops | 135.1 µs | 137.3 µs | 139.6 µs | 163.5 µs | 166.3 µs | 176.2 µs | 0 errors |
| count-by-user | 200 ops | 59.5 µs | 62.2 µs | 63.3 µs | 80.6 µs | 95.0 µs | 101.0 µs | 0 errors |

#### `s3-c1-n1000-none` run 1 (cores = 1)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 4 ops | 1,372.2 µs | 1,372.2 µs | 1,385.9 µs | 3,674.3 µs | 3,674.3 µs | 3,674.3 µs | 0 errors |
| load | 1,900 ops | 985.6 µs | 1,157.1 µs | 1,232.0 µs | 1,600.9 µs | 3,006.3 µs | 12,144.0 µs | 0 errors |
| pk-user | 200 ops | 61.0 µs | 63.0 µs | 65.0 µs | 82.7 µs | 117.7 µs | 209.7 µs | 0 errors |
| loans-by-user | 200 ops | 89.9 µs | 102.7 µs | 109.8 µs | 150.9 µs | 168.6 µs | 199.4 µs | 0 errors |
| loans-by-book | 200 ops | 89.0 µs | 98.4 µs | 103.5 µs | 122.3 µs | 131.2 µs | 142.0 µs | 0 errors |
| resv-by-user | 200 ops | 71.8 µs | 78.8 µs | 82.3 µs | 100.5 µs | 110.7 µs | 119.7 µs | 0 errors |
| books-by-author | 200 ops | 66.3 µs | 75.6 µs | 80.2 µs | 96.2 µs | 110.4 µs | 115.5 µs | 0 errors |
| books-by-genre | 200 ops | 85.1 µs | 99.5 µs | 104.3 µs | 127.2 µs | 140.3 µs | 140.5 µs | 0 errors |
| loans-by-daterange | 200 ops | 109.0 µs | 202.8 µs | 214.2 µs | 249.1 µs | 256.4 µs | 299.7 µs | 0 errors |
| overdue | 200 ops | 96.2 µs | 166.1 µs | 175.4 µs | 204.8 µs | 221.3 µs | 272.3 µs | 0 errors |
| join-loan-user | 200 ops | 101.1 µs | 114.7 µs | 121.4 µs | 144.4 µs | 166.7 µs | 180.6 µs | 0 errors |
| join-no-literal | 200 ops | 290.9 µs | 295.9 µs | 300.9 µs | 316.2 µs | 327.3 µs | 345.9 µs | 0 errors |
| exists-correlated | 200 ops | 166.3 µs | 169.2 µs | 171.3 µs | 190.4 µs | 219.7 µs | 251.7 µs | 0 errors |
| count-by-user | 200 ops | 82.3 µs | 83.9 µs | 84.8 µs | 101.2 µs | 107.6 µs | 138.0 µs | 0 errors |

#### `s3-c1-n1000-all` run 1 (cores = 1)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 4 ops | 1,277.3 µs | 1,277.3 µs | 1,376.0 µs | 1,813.9 µs | 1,813.9 µs | 1,813.9 µs | 0 errors |
| load | 1,900 ops | 998.7 µs | 1,156.0 µs | 1,222.4 µs | 1,584.9 µs | 2,089.5 µs | 10,451.9 µs | 0 errors |
| create-index | 8 ops | 1,523.7 µs | 1,581.3 µs | 1,826.3 µs | 2,562.9 µs | 2,562.9 µs | 2,562.9 µs | 0 errors |
| pk-user | 200 ops | 89.3 µs | 92.9 µs | 96.1 µs | 116.8 µs | 141.2 µs | 208.3 µs | 0 errors |
| loans-by-user | 200 ops | 90.2 µs | 112.3 µs | 120.3 µs | 147.1 µs | 158.8 µs | 164.2 µs | 0 errors |
| loans-by-book | 200 ops | 65.3 µs | 79.5 µs | 86.0 µs | 125.0 µs | 131.4 µs | 146.7 µs | 0 errors |
| resv-by-user | 200 ops | 59.5 µs | 70.2 µs | 74.7 µs | 93.1 µs | 101.3 µs | 118.3 µs | 0 errors |
| books-by-author | 200 ops | 87.9 µs | 105.1 µs | 110.3 µs | 130.8 µs | 150.2 µs | 160.0 µs | 0 errors |
| books-by-genre | 200 ops | 117.1 µs | 133.6 µs | 141.1 µs | 166.9 µs | 177.4 µs | 179.5 µs | 0 errors |
| loans-by-daterange | 200 ops | 106.7 µs | 202.3 µs | 216.6 µs | 278.3 µs | 316.3 µs | 347.9 µs | 0 errors |
| overdue | 200 ops | 67.3 µs | 146.7 µs | 157.3 µs | 188.9 µs | 195.8 µs | 201.2 µs | 0 errors |
| join-loan-user | 200 ops | 79.0 µs | 94.4 µs | 100.4 µs | 141.1 µs | 161.8 µs | 174.3 µs | 0 errors |
| join-no-literal | 200 ops | 282.5 µs | 294.5 µs | 310.1 µs | 362.4 µs | 436.8 µs | 450.9 µs | 0 errors |
| exists-correlated | 200 ops | 135.5 µs | 137.1 µs | 138.5 µs | 156.6 µs | 163.2 µs | 180.7 µs | 0 errors |
| count-by-user | 200 ops | 59.6 µs | 62.6 µs | 63.8 µs | 79.5 µs | 87.2 µs | 92.5 µs | 0 errors |

#### `s3-c1-n10000-none` run 1 (cores = 1)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 4 ops | 1,211.8 µs | 1,211.8 µs | 1,304.9 µs | 2,024.9 µs | 2,024.9 µs | 2,024.9 µs | 0 errors |
| load | 19,000 ops | 955.7 µs | 1,145.5 µs | 1,211.9 µs | 1,580.1 µs | 2,609.0 µs | 24,494.2 µs | 0 errors |
| pk-user | 200 ops | 62.1 µs | 63.5 µs | 64.7 µs | 80.0 µs | 110.4 µs | 194.7 µs | 0 errors |
| loans-by-user | 200 ops | 332.3 µs | 347.3 µs | 355.2 µs | 396.5 µs | 416.8 µs | 436.7 µs | 0 errors |
| loans-by-book | 200 ops | 317.1 µs | 334.5 µs | 341.6 µs | 366.2 µs | 386.7 µs | 497.1 µs | 0 errors |
| resv-by-user | 200 ops | 185.9 µs | 194.7 µs | 199.5 µs | 217.8 µs | 245.9 µs | 265.6 µs | 0 errors |
| books-by-author | 200 ops | 118.2 µs | 130.0 µs | 135.6 µs | 158.7 µs | 180.0 µs | 186.0 µs | 0 errors |
| books-by-genre | 200 ops | 427.8 µs | 459.6 µs | 475.9 µs | 511.1 µs | 543.9 µs | 656.8 µs | 0 errors |
| loans-by-daterange | 200 ops | 440.8 µs | 1,511.8 µs | 1,551.4 µs | 1,667.6 µs | 1,742.5 µs | 2,008.1 µs | 0 errors |
| overdue | 200 ops | 378.9 µs | 1,131.6 µs | 1,173.5 µs | 1,296.1 µs | 1,419.8 µs | 1,442.0 µs | 0 errors |
| join-loan-user | 200 ops | 333.6 µs | 350.2 µs | 358.5 µs | 423.7 µs | 505.1 µs | 544.0 µs | 0 errors |
| join-no-literal | 200 ops | 574.7 µs | 586.7 µs | 595.6 µs | 622.8 µs | 678.5 µs | 858.5 µs | 0 errors |
| exists-correlated | 200 ops | 585.4 µs | 605.9 µs | 626.1 µs | 658.9 µs | 720.7 µs | 873.0 µs | 0 errors |
| count-by-user | 200 ops | 318.3 µs | 323.7 µs | 328.5 µs | 370.1 µs | 402.0 µs | 502.3 µs | 0 errors |

#### `s3-c1-n10000-all` run 1 (cores = 1)

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 4 ops | 1,136.1 µs | 1,136.1 µs | 1,315.3 µs | 1,809.6 µs | 1,809.6 µs | 1,809.6 µs | 0 errors |
| load | 19,000 ops | 955.9 µs | 1,148.7 µs | 1,217.9 µs | 1,590.0 µs | 2,877.3 µs | 30,721.4 µs | 0 errors |
| create-index | 8 ops | 3,179.2 µs | 3,441.0 µs | 6,079.5 µs | 12,232.3 µs | 12,232.3 µs | 12,232.3 µs | 0 errors |
| pk-user | 200 ops | 60.7 µs | 62.3 µs | 63.3 µs | 83.6 µs | 100.4 µs | 213.1 µs | 0 errors |
| loans-by-user | 200 ops | 70.3 µs | 82.2 µs | 88.9 µs | 108.9 µs | 124.9 µs | 134.5 µs | 0 errors |
| loans-by-book | 200 ops | 67.4 µs | 80.5 µs | 94.5 µs | 144.9 µs | 229.9 µs | 348.6 µs | 0 errors |
| resv-by-user | 200 ops | 58.2 µs | 69.8 µs | 73.9 µs | 91.8 µs | 101.6 µs | 111.8 µs | 0 errors |
| books-by-author | 200 ops | 60.4 µs | 74.7 µs | 80.0 µs | 98.3 µs | 104.7 µs | 125.8 µs | 0 errors |
| books-by-genre | 200 ops | 420.5 µs | 450.7 µs | 472.4 µs | 559.2 µs | 738.3 µs | 799.0 µs | 0 errors |
| loans-by-daterange | 200 ops | 441.3 µs | 1,486.0 µs | 1,529.8 µs | 1,717.5 µs | 2,186.8 µs | 2,363.9 µs | 0 errors |
| overdue | 200 ops | 84.3 µs | 924.1 µs | 969.2 µs | 1,110.7 µs | 1,517.1 µs | 1,602.8 µs | 0 errors |
| join-loan-user | 200 ops | 84.7 µs | 96.4 µs | 103.0 µs | 130.0 µs | 159.8 µs | 329.4 µs | 0 errors |
| join-no-literal | 200 ops | 277.3 µs | 283.4 µs | 288.4 µs | 323.3 µs | 378.7 µs | 483.8 µs | 0 errors |
| exists-correlated | 200 ops | 135.4 µs | 137.0 µs | 138.0 µs | 153.6 µs | 159.1 µs | 164.3 µs | 0 errors |
| count-by-user | 200 ops | 60.6 µs | 64.1 µs | 66.5 µs | 86.2 µs | 97.4 µs | 99.8 µs | 0 errors |

## 8. Where the time goes

Reads on one connection, no writes after the load, no commit, no lock: the latency is a fixed round trip plus rows walked plus rows returned, with no durability or conflict wait to separate.

| Component | KDS | PostgreSQL | How derived |
|---|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | **68.3 µs** | 84.5 µs | `pk-user` mean at 10,000 loans, run 1 |
| Durability / commit | not applicable | not applicable | read-only shapes |
| Lock or conflict wait | none | none | one connection |

Cost per row walked by an unindexed equality (mean at 10,000 loans minus mean at 200, over the 9,800 extra rows of `loans`), and the cost of a returned row (`books-by-genre` returns about 125 rows):

| shape | KDS mean n=200 | KDS mean n=10,000 | KDS per extra row | PG mean n=200 | PG mean n=10,000 | PG per extra row |
|---|---|---|---|---|---|---|
| pk-user | 90.1 µs | 68.3 µs | -2.2 ns | 64.6 µs | 84.5 µs | 2.0 ns |
| loans-by-user | 117.7 µs | 359.8 µs | 24.7 ns | 83.7 µs | 415.3 µs | 33.8 ns |
| loans-by-book | 113.8 µs | 344.4 µs | 23.5 ns | 97.2 µs | 441.3 µs | 35.1 ns |
| books-by-genre | 98.0 µs | 477.9 µs | 38.8 ns | 63.5 µs | 333.9 µs | 27.6 ns |

KDS's walk is cheaper per examined row (23.5 and 24.7 ns against PostgreSQL's 33.8 and 35.1 ns on the two `loans` equalities), which is the whole reason the unindexed equality is ahead of PostgreSQL at 10,000 loans. Where rows must be
returned it is the opposite way (`books-by-genre`, 125 rows: 38.8 ns per extra relation row against 27.6 ns), the per-row result cost that scenario 1 isolates.

## 9. Against PostgreSQL 18.6 (fresh, same window)

PostgreSQL is the floor: it answers "is the engine in the right league" and nothing finer. The twin ran after the KDS cells, three runs per cell, at its defaults (`shared_buffers = 128MB`, `synchronous_commit = on`); no
`cores` axis. Ratio of KDS `cores = 1` median qps to PostgreSQL's median qps (above 1.00x = KDS faster), then PostgreSQL's own medians:

Ratio of KDS `cores = 1` median qps over PostgreSQL 18.6 median qps (fresh, same window):

| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| pk-user | 0.72x | 0.93x | 0.99x | 0.70x | 0.98x | 1.00x |
| loans-by-user | 0.64x | 1.14x | 0.86x | 0.73x | 1.15x | 1.02x |
| loans-by-book | 0.65x | 1.27x | 0.76x | 0.94x | 1.23x | 0.98x |
| resv-by-user | 0.65x | 1.30x | 0.89x | 0.98x | 1.16x | 1.02x |
| books-by-author | 0.62x | 1.26x | 0.91x | 0.76x | 1.08x | 0.99x |
| books-by-genre | 0.65x | 1.29x | 0.81x | 0.69x | 0.69x | 0.56x |
| loans-by-daterange | 0.68x | 0.95x | 0.75x | 0.71x | 0.61x | 0.62x |
| overdue | 0.91x | 1.02x | 0.87x | 0.70x | 0.77x | 0.55x |
| join-loan-user | 0.99x | 1.10x | 1.10x | 0.90x | 1.17x | 1.04x |
| join-no-literal | 0.85x | 0.84x | 0.96x | 0.94x | 1.12x | 2.31x |
| exists-correlated | 1.35x | 1.51x | 1.73x | 1.26x | 2.08x | 1.34x |
| count-by-user | 1.14x | 1.60x | 1.18x | 1.23x | 1.25x | 1.27x |

PostgreSQL 18.6 median qps:

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

| cells | geometric mean | min | max |
|---|---|---|---|
| n=200 none | 0.79x | 0.62x | 1.35x |
| n=200 all | 1.16x | 0.84x | 1.60x |
| n=1,000 none | 0.96x | 0.75x | 1.73x |
| n=1,000 all | 0.86x | 0.69x | 1.26x |
| n=10,000 none | 1.06x | 0.61x | 2.08x |
| n=10,000 all | 0.98x | 0.55x | 2.31x |

**Reading.** At 10,000 loans the engine is in the PostgreSQL league: the unindexed equalities are 1.08x to 1.23x, the correlated `EXISTS` 2.08x unindexed, `join-no-literal` 1.12x unindexed and 2.31x
indexed. It is behind where rows must be returned or a range filter walks the whole relation (`books-by-genre` 0.69x, `loans-by-daterange` 0.61x, `overdue` 0.77x unindexed). At 200 loans it is behind on 10 of 12 shapes unindexed (0.62x to 0.99x) and ahead on `exists-correlated` and `count-by-user`; the round trip alone does not explain it (PostgreSQL's `pk-user` is 64.6 µs at 200 loans; KDS's 90.1 µs is run 1 of the slow-mode `n=200 none` cell, and its steady figure is 68 µs in `n=200 all`), and the `cores = 1` cells there are not read (section 10).

## 10. Noise floor, contamination and host

The noise floor is the spread table of section 3: 0.1 % to 4 % on the large unindexed cells and the join shapes, **20 % to 43 % on a third of the cells at 200 and 1,000 loans**. The cause is the cell length: a shape is 200
statements of 70 to 130 µs, 14 to 26 ms, and one slow run in three (a stall of 5 to 10 ms of the host) is a spread of that size. The pattern repeats at `pk-user`, the control, whose spread is 43 % in `n=200 none` and 43 % in `n=1,000 all`; a
cell where the control moves is a cell where the host moved, and **no comparison in the small columns, cross-version or against PostgreSQL, is read as a finding**. This is the same behaviour section 10 of the v3.1.0 file describes (its
"slow mode").

**Contamination.** No KDS cell was discarded. The PostgreSQL batch of this scenario failed on its first attempt, six cells in 3 to 4 seconds each, on `role "ec2-user" does not exist` (the twin's `--user` default); the master and the watchdog were
stopped by recorded PID, the six attempts are in `contaminated/pg-s3-no-user-attempt/` of the run archive, the command line got `--user cdkbs` and the cells were re-run in the same window. All 18 PostgreSQL cells in this file are the re-run.

Per-cell host stamps:

| cell | start (UTC) | end (UTC) | load average before (1 / 5 / 15 min) | gate wait | build or test process | foreign samples >= 10 % CPU | exit |
|---|---|---|---|---|---|---|---|
| `s3-c1-n200-none` | 03:58:12 | 03:58:13 | 0.88 / 0.98 / 1.48 | 0 s | none | 0 of 3 samples | 0 |
| `s3-c1-n200-all` | 03:58:20 | 03:58:22 | 0.75 / 0.95 / 1.46 | 0 s | none | 0 of 3 samples | 0 |
| `s3-c1-n1000-none` | 03:58:29 | 03:58:33 | 0.71 / 0.94 / 1.45 | 0 s | none | 0 of 4 samples | 0 |
| `s3-c1-n1000-all` | 03:58:40 | 03:58:44 | 0.60 / 0.91 / 1.44 | 0 s | none | 0 of 4 samples | 0 |
| `s3-c1-n10000-none` | 03:58:51 | 03:59:17 | 0.66 / 0.91 / 1.43 | 0 s | none | 1 of 14 samples | 0 |
| `s3-c1-n10000-all` | 03:59:24 | 03:59:51 | 0.71 / 0.90 / 1.41 | 0 s | none | 3 of 15 samples | 0 |
| `s3-c8-n200-none` | 03:59:58 | 04:00:00 | 1.31 / 1.05 / 1.44 | 0 s | none | 0 of 3 samples | 0 |
| `s3-c8-n200-all` | 04:00:07 | 04:00:08 | 1.25 / 1.05 / 1.44 | 0 s | none | 0 of 2 samples | 0 |
| `s3-c8-n1000-none` | 04:00:15 | 04:00:19 | 1.06 / 1.01 / 1.42 | 0 s | none | 0 of 4 samples | 0 |
| `s3-c8-n1000-all` | 04:00:26 | 04:00:30 | 0.97 / 1.00 / 1.41 | 0 s | none | 0 of 4 samples | 0 |
| `s3-c8-n10000-none` | 04:00:37 | 04:01:04 | 0.98 / 1.00 / 1.41 | 0 s | none | 0 of 14 samples | 0 |
| `s3-c8-n10000-all` | 04:01:11 | 04:01:38 | 1.06 / 1.03 / 1.40 | 0 s | none | 0 of 15 samples | 0 |
| `r2-s3-c1-n200-none` | 04:01:45 | 04:01:47 | 1.02 / 1.02 / 1.39 | 0 s | none | 0 of 3 samples | 0 |
| `r2-s3-c1-n200-all` | 04:01:54 | 04:01:56 | 0.86 / 0.99 / 1.37 | 0 s | none | 0 of 3 samples | 0 |
| `r2-s3-c1-n1000-none` | 04:02:03 | 04:02:06 | 0.79 / 0.97 / 1.36 | 0 s | none | 0 of 3 samples | 0 |
| `r2-s3-c1-n1000-all` | 04:02:13 | 04:02:17 | 0.82 / 0.97 / 1.36 | 0 s | none | 0 of 4 samples | 0 |
| `r2-s3-c1-n10000-none` | 04:02:24 | 04:02:51 | 0.77 / 0.95 / 1.35 | 0 s | none | 0 of 14 samples | 0 |
| `r2-s3-c1-n10000-all` | 04:02:58 | 04:03:24 | 1.04 / 1.01 / 1.35 | 0 s | none | 0 of 14 samples | 0 |
| `r2-s3-c8-n200-none` | 04:03:31 | 04:03:32 | 0.93 / 0.99 / 1.34 | 0 s | none | 0 of 3 samples | 0 |
| `r2-s3-c8-n200-all` | 04:03:39 | 04:03:41 | 0.79 / 0.95 / 1.32 | 0 s | none | 0 of 3 samples | 0 |
| `r2-s3-c8-n1000-none` | 04:03:48 | 04:03:52 | 0.72 / 0.94 / 1.31 | 0 s | none | 0 of 4 samples | 0 |
| `r2-s3-c8-n1000-all` | 04:03:59 | 04:04:03 | 0.63 / 0.91 / 1.30 | 0 s | none | 0 of 4 samples | 0 |
| `r2-s3-c8-n10000-none` | 04:04:10 | 04:04:37 | 0.53 / 0.88 / 1.28 | 0 s | none | 0 of 14 samples | 0 |
| `r2-s3-c8-n10000-all` | 04:04:44 | 04:05:10 | 0.81 / 0.91 / 1.28 | 0 s | none | 0 of 14 samples | 0 |
| `r3-s3-c1-n200-none` | 04:05:17 | 04:05:19 | 0.81 / 0.90 / 1.26 | 0 s | none | 0 of 3 samples | 0 |
| `r3-s3-c1-n200-all` | 04:05:26 | 04:05:28 | 0.76 / 0.89 / 1.25 | 0 s | none | 0 of 3 samples | 0 |
| `r3-s3-c1-n1000-none` | 04:05:35 | 04:05:39 | 0.64 / 0.86 / 1.24 | 0 s | none | 0 of 4 samples | 0 |
| `r3-s3-c1-n1000-all` | 04:05:46 | 04:05:50 | 0.54 / 0.83 / 1.22 | 0 s | none | 0 of 4 samples | 0 |
| `r3-s3-c1-n10000-none` | 04:05:57 | 04:06:23 | 0.69 / 0.85 / 1.23 | 0 s | none | 0 of 14 samples | 0 |
| `r3-s3-c1-n10000-all` | 04:06:30 | 04:06:56 | 0.75 / 0.85 / 1.21 | 0 s | none | 0 of 13 samples | 0 |
| `r3-s3-c8-n200-none` | 04:07:03 | 04:07:05 | 0.98 / 0.90 / 1.21 | 0 s | none | 0 of 4 samples | 0 |
| `r3-s3-c8-n200-all` | 04:07:12 | 04:07:13 | 0.83 / 0.87 / 1.20 | 0 s | none | 0 of 3 samples | 0 |
| `r3-s3-c8-n1000-none` | 04:07:21 | 04:07:24 | 0.78 / 0.86 / 1.19 | 0 s | none | 0 of 4 samples | 0 |
| `r3-s3-c8-n1000-all` | 04:07:31 | 04:07:35 | 0.73 / 0.85 / 1.18 | 0 s | none | 0 of 4 samples | 0 |
| `r3-s3-c8-n10000-none` | 04:07:42 | 04:08:09 | 0.69 / 0.84 / 1.18 | 0 s | none | 0 of 14 samples | 0 |
| `r3-s3-c8-n10000-all` | 04:08:16 | 04:08:43 | 0.85 / 0.87 / 1.17 | 0 s | none | 0 of 15 samples | 0 |
| `pg-s3-n200-none` | 04:23:17 | 04:23:17 | 0.64 / 0.64 / 0.80 | 0 s | none | 0 of 2 samples | 0 |
| `pg-s3-n200-all` | 04:23:24 | 04:23:25 | 0.54 / 0.61 / 0.79 | 0 s | none | 0 of 3 samples | 0 |
| `pg-s3-n1000-none` | 04:23:32 | 04:23:35 | 0.50 / 0.60 / 0.79 | 0 s | none | 0 of 4 samples | 0 |
| `pg-s3-n1000-all` | 04:23:42 | 04:23:45 | 0.49 / 0.60 / 0.78 | 0 s | none | 0 of 3 samples | 0 |
| `pg-s3-n10000-none` | 04:23:52 | 04:24:18 | 0.57 / 0.61 / 0.79 | 0 s | none | 1 of 14 samples | 0 |
| `pg-s3-n10000-all` | 04:24:25 | 04:24:50 | 0.71 / 0.66 / 0.79 | 0 s | none | 0 of 14 samples | 0 |
| `r2-pg-s3-n200-none` | 04:26:57 | 04:26:58 | 0.86 / 0.75 / 0.82 | 0 s | none | 0 of 3 samples | 0 |
| `r2-pg-s3-n200-all` | 04:27:05 | 04:27:05 | 0.72 / 0.73 / 0.81 | 0 s | none | 0 of 2 samples | 0 |
| `r2-pg-s3-n1000-none` | 04:27:13 | 04:27:15 | 0.67 / 0.71 / 0.80 | 0 s | none | 0 of 3 samples | 0 |
| `r2-pg-s3-n1000-all` | 04:27:22 | 04:27:25 | 0.64 / 0.71 / 0.80 | 0 s | none | 0 of 3 samples | 0 |
| `r2-pg-s3-n10000-none` | 04:27:33 | 04:27:58 | 0.61 / 0.70 / 0.80 | 0 s | none | 0 of 13 samples | 0 |
| `r2-pg-s3-n10000-all` | 04:28:05 | 04:28:30 | 0.80 / 0.75 / 0.81 | 0 s | none | 0 of 13 samples | 0 |
| `r3-pg-s3-n200-none` | 04:30:35 | 04:30:36 | 0.85 / 0.83 / 0.83 | 0 s | none | 0 of 3 samples | 0 |
| `r3-pg-s3-n200-all` | 04:30:43 | 04:30:44 | 0.78 / 0.81 / 0.83 | 0 s | none | 0 of 3 samples | 0 |
| `r3-pg-s3-n1000-none` | 04:30:51 | 04:30:54 | 0.81 / 0.82 / 0.83 | 0 s | none | 0 of 3 samples | 0 |
| `r3-pg-s3-n1000-all` | 04:31:01 | 04:31:04 | 1.13 / 0.89 / 0.85 | 0 s | none | 0 of 4 samples | 0 |
| `r3-pg-s3-n10000-none` | 04:31:11 | 04:31:35 | 0.96 / 0.86 / 0.84 | 0 s | none | 0 of 13 samples | 0 |
| `r3-pg-s3-n10000-all` | 04:31:42 | 04:32:06 | 1.47 / 1.00 / 0.89 | 0 s | none | 1 of 13 samples | 0 |
| `pl-s3-planned-none-r1` | 04:34:07 | 04:34:33 | 0.66 / 0.88 / 0.87 | 0 s | none | 0 of 14 samples | 0 |
| `pl-s3-planned-none-r2` | 04:37:07 | 04:37:33 | 0.91 / 0.94 / 0.91 | 0 s | none | 0 of 14 samples | 0 |
| `pl-s3-planned-none-r3` | 04:40:07 | 04:40:32 | 1.13 / 1.02 / 0.94 | 0 s | none | 0 of 14 samples | 0 |
| `pl-s3-planned-all-r1` | 04:43:06 | 04:43:31 | 1.11 / 1.07 / 0.99 | 0 s | none | 0 of 13 samples | 0 |
| `pl-s3-written-none-r1` | 04:35:37 | 04:36:03 | 1.21 / 0.98 / 0.91 | 0 s | none | 0 of 14 samples | 0 |
| `pl-s3-written-none-r2` | 04:38:37 | 04:39:02 | 0.98 / 0.98 / 0.92 | 0 s | none | 0 of 13 samples | 0 |
| `pl-s3-written-none-r3` | 04:41:36 | 04:42:02 | 1.14 / 1.04 / 0.96 | 0 s | none | 0 of 14 samples | 0 |
| `pl-s3-written-all-r1` | 04:43:38 | 04:44:03 | 1.04 / 1.06 / 0.99 | 0 s | none | 0 of 14 samples | 0 |

## 11. What the run says about the engine

1. **Nothing in the 11 read shapes moved beyond noise since v3.1.0**: the large unindexed cells (readable) are within 0.95x to 1.07x and the indexed ones within 0.91x to 1.02x. The engine's read path is where v3.1.0 left it in this scenario.
2. **The one real mover is a join without a literal, and the planner's order choice is not the reason**: `join-no-literal` is 1.98x v3.1.0, and the same cell with `join_order = written` is as fast. The statement-local inner build and CB-S1's derived range bounds are in both arms, so neither is excluded (section 6). The planned arm
   is *not* faster than written on any of the three join shapes. This is the first data point on CB/CC in this scenario: the planner is conservative (it keeps the written order unless it wins by the margin) and costs nothing measurable here.
3. **KDS walks a row cheaper than PostgreSQL (about 24 ns against 34 ns) and returns a row dearer** (the `books-by-genre` figure above). The crossover is why the equality shapes are ahead at 10,000 loans and the
   range and wide-result shapes are behind.
4. **An index pays about 4x for an equality at 10,000 loans and nothing at 200** on both engines. A Cabin or index on a relation of a few pages is no tool.
5. **This scenario's small cells cannot support a finding**; a longer `--ops` would, and is the next change to the measurement, not to the engine.

## 12. Archive

`bench/v3.3.0/archive/scenario3-v3.3.0-33-g9682b725/`: per-cell JSON (KDS and PostgreSQL), driver stdout, server configs, and `plans/` (the full ANALYZE replies of the join-order probe). What the four scenarios share is in
`bench/v3.3.0/archive/run-v3.3.0-33-g9682b725/`.
