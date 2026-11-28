# EH-S0: what the native fold pays for a row, shape by shape

**One line.** On `main`'s engine, through counting clients, a row the scan kernel rejects costs KDS 10.9 ns of server CPU against PostgreSQL 18.7's 17.5 ns, and **a row that reaches the fold costs 19.2 ns against 12.1 ns before any cell is read** (`COUNT(*)`, 1.59x): the fold's deficit starts with the 8.3 ns between rejecting a row and handing it over. A global fold of four aggregates is 39.0 ns a row against 28.0 ns (1.39x, 0.71x of PostgreSQL's rate); a `GROUP BY`'s probe adds no more than PostgreSQL's (23.5 ns against 26.2 ns a row over a count, at 8 groups), and founding a group costs about 230 ns against about 160 ns. The stack samples put a global fold's time in three near-equal parts - the fold's own loop, the decode of the cells it reads, and the row's path through the walk - and a many-group fold's in the hash table and the allocator.

Measured by CLA in the session that opened EH (no `ck-tester` agent was spawned). Driver: `tools/eh_s0_premise.py`, which is `tools/parity_bench.py` with its shapes replaced; both sides are timed through clients that count rows (`kwp.Connection.query`, `pg_wire`'s `query`).

## Stamp

| Field | Value |
|---|---|
| Version, commit | worktree `eh-native-fold-per-row` at `01c6c741c` (`v3.5.1-35-g01c6c741c`). **The measured binary is EG's closing one**, built Release from `git archive 9830e6bb0`: `git diff 9830e6bb0 01c6c741c -- src include CMakeLists.txt` is empty; its sha256 is in the archive |
| Date and time | 2026-11-28 03:34:41 to 03:35:31 UTC, **50 s**, inside the 20-minute stage budget: the eleven shapes, then 150 stack samples of the server inside each of two folds |
| KDS | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, a fresh data file, `taskset -c 0-7` |
| PostgreSQL | 18.7, a fresh `docker.io/library/postgres:18` container (run by `podman`) at its defaults, host network, on CPUs 0 to 7; `max_parallel_workers_per_gather = 0` for the session |
| Data | `pb_bars`: 60,480 rows of 8 columns (seven `int64`, one `int32`), no secondary index on either engine; 8 values of `symbol_id`, 7,560 of `session_no`, 60,480 of `id` |
| Procedure | 5 reps; each rep runs every shape on both engines, the engine order alternating by rep, a warm-up of a tenth of the ops before each cell; both engines must return the same row counts. Driver `taskset -c 8-23` |
| Host and device | Intel Core Ultra 7 270K Plus, 24 CPUs (P-cores 0 to 7), `powersave`; `/dev/mapper/fedora-root`, xfs (`df -T` at the run's start) |
| Quiet gate | before the shapes: no build or test process, at most 8 % busy over 3 s; passed at once, 1-minute load 0.30 |
| Rule | spread = (max - min) / median over the five reps; a difference smaller than the larger spread is inside the spread; a spread over 20 % carries no claim. Server CPU is `schedstat` on-CPU time: every KDS server thread less its idle tick (23,323 µs a second in this run), PostgreSQL's one backend; "a row" divides by the relation's 60,480 rows |
| Comparator | this engine's last figures for two of the shapes, same host and method, measured 108 minutes earlier on an engine whose only source difference is EG's booking of a `strict` commit's sync: `results-ef-s0-premise-v3.5.1-6-g72580aa04.md`, `agg-global` 2,386.2 µs and `agg-by-session` 5,471.1 µs of server CPU; here 2,358.3 µs and 5,065.5 µs (`group-7560-sum` is that shape) |
| Suite | not executed: no build input changed |

## The shapes

Each shape adds one thing to the one above it.

| shape | rows returned | KDS server CPU a statement (spread) | a row | PostgreSQL backend CPU (spread) | a row | CPU, KDS / PG | KDS rate (spread) | PostgreSQL rate (spread) | rate, KDS / PG | read |
|---|---|---|---|---|---|---|---|---|---|---|
| `scan-reject` | 0 rows | 658.0 µs (5.7 %) | **10.9 ns** | 1,059.4 µs (5.3 %) | **17.5 ns** | 0.62x | 1,353.7 qps (15.5 %) | 910.8 qps (10.0 %) | 1.49x | KDS ahead |
| `count-star` | 1 rows | 1,163.5 µs (0.5 %) | **19.2 ns** | 733.3 µs (2.1 %) | **12.1 ns** | 1.59x | 812.6 qps (3.2 %) | 1,291.8 qps (7.9 %) | 0.63x | PostgreSQL ahead |
| `count-col` | 1 rows | 1,671.6 µs (1.0 %) | **27.6 ns** | 1,152.2 µs (2.3 %) | **19.1 ns** | 1.45x | 567.7 qps (0.9 %) | 842.3 qps (2.0 %) | 0.67x | PostgreSQL ahead |
| `sum` | 1 rows | 1,672.9 µs (2.3 %) | **27.7 ns** | 1,236.3 µs (2.4 %) | **20.4 ns** | 1.35x | 563.4 qps (15.9 %) | 785.1 qps (4.7 %) | 0.72x | PostgreSQL ahead |
| `min-max` | 1 rows | 1,954.5 µs (3.6 %) | **32.3 ns** | 1,351.8 µs (4.1 %) | **22.4 ns** | 1.45x | 489.2 qps (13.2 %) | 712.2 qps (7.8 %) | 0.69x | PostgreSQL ahead |
| `agg-global` | 1 rows | 2,358.3 µs (2.8 %) | **39.0 ns** | 1,693.9 µs (2.4 %) | **28.0 ns** | 1.39x | 406.5 qps (3.8 %) | 568.6 qps (5.2 %) | 0.71x | PostgreSQL ahead |
| `group-8` | 8 rows | 2,581.9 µs (1.3 %) | **42.7 ns** | 2,316.8 µs (2.4 %) | **38.3 ns** | 1.11x | 367.2 qps (3.2 %) | 419.7 qps (7.9 %) | 0.88x | PostgreSQL ahead |
| `group-8-sum` | 8 rows | 2,956.5 µs (1.5 %) | **48.9 ns** | 2,885.6 µs (0.9 %) | **47.7 ns** | 1.02x | 320.7 qps (8.9 %) | 327.6 qps (6.2 %) | 0.98x | inside the spread |
| `group-7560` | 7,560 rows | 4,324.1 µs (8.3 %) | **71.5 ns** | 3,529.2 µs (2.7 %) | **58.4 ns** | 1.23x | 169.8 qps (6.7 %) | 132.9 qps (2.7 %) | 1.28x | KDS ahead |
| `group-7560-sum` | 7,560 rows | 5,065.5 µs (8.5 %) | **83.8 ns** | 4,556.0 µs (4.0 %) | **75.3 ns** | 1.11x | 138.2 qps (5.9 %) | 111.1 qps (2.2 %) | 1.24x | KDS ahead |
| `group-60480` | 60,480 rows | 13,759.1 µs (3.1 %) | **227.5 ns** | 11,502.3 µs (3.9 %) | **190.2 ns** | 1.20x | 41.1 qps (2.7 %) | 23.6 qps (4.0 %) | 1.74x | KDS ahead |

| shape | engine | ops a rep | mean | p0 | p25 | p50 | p95 | p99 | client CPU a statement |
|---|---|---|---|---|---|---|---|---|---|
| `scan-reject` | KDS | 20 ops | 738.5 µs | 666.4 µs | 675.2 µs | 685.3 µs | 959.7 µs | 959.7 µs | 16.0 µs |
| `scan-reject` | PostgreSQL | 20 ops | 1,097.6 µs | 1,040.9 µs | 1,063.0 µs | 1,076.1 µs | 1,334.3 µs | 1,334.3 µs | 19.8 µs |
| `count-star` | KDS | 20 ops | 1,230.4 µs | 1,199.3 µs | 1,208.3 µs | 1,214.1 µs | 1,436.9 µs | 1,436.9 µs | 19.4 µs |
| `count-star` | PostgreSQL | 20 ops | 773.8 µs | 714.7 µs | 730.1 µs | 734.2 µs | 995.7 µs | 995.7 µs | 11.5 µs |
| `count-col` | KDS | 20 ops | 1,761.1 µs | 1,717.8 µs | 1,728.8 µs | 1,738.8 µs | 1,951.9 µs | 1,951.9 µs | 21.9 µs |
| `count-col` | PostgreSQL | 20 ops | 1,186.9 µs | 1,149.3 µs | 1,166.0 µs | 1,175.6 µs | 1,403.0 µs | 1,403.0 µs | 17.9 µs |
| `sum` | KDS | 20 ops | 1,774.5 µs | 1,705.9 µs | 1,726.4 µs | 1,741.6 µs | 1,993.1 µs | 1,993.1 µs | 27.7 µs |
| `sum` | PostgreSQL | 20 ops | 1,273.5 µs | 1,220.3 µs | 1,244.6 µs | 1,256.1 µs | 1,495.0 µs | 1,495.0 µs | 18.8 µs |
| `min-max` | KDS | 20 ops | 2,043.6 µs | 2,006.0 µs | 2,013.7 µs | 2,026.0 µs | 2,226.6 µs | 2,226.6 µs | 28.1 µs |
| `min-max` | PostgreSQL | 20 ops | 1,403.8 µs | 1,348.1 µs | 1,362.9 µs | 1,371.5 µs | 1,619.5 µs | 1,619.5 µs | 21.9 µs |
| `agg-global` | KDS | 20 ops | 2,459.8 µs | 2,416.8 µs | 2,430.5 µs | 2,442.1 µs | 2,660.8 µs | 2,660.8 µs | 29.2 µs |
| `agg-global` | PostgreSQL | 20 ops | 1,758.4 µs | 1,698.3 µs | 1,710.7 µs | 1,714.0 µs | 1,980.4 µs | 1,980.4 µs | 23.3 µs |
| `group-8` | KDS | 20 ops | 2,722.7 µs | 2,643.6 µs | 2,668.8 µs | 2,692.0 µs | 2,907.3 µs | 2,907.3 µs | 32.7 µs |
| `group-8` | PostgreSQL | 20 ops | 2,382.3 µs | 2,284.8 µs | 2,321.9 µs | 2,356.8 µs | 2,613.7 µs | 2,613.7 µs | 35.7 µs |
| `group-8-sum` | KDS | 20 ops | 3,118.1 µs | 3,031.0 µs | 3,058.3 µs | 3,076.2 µs | 3,339.3 µs | 3,339.3 µs | 40.4 µs |
| `group-8-sum` | PostgreSQL | 20 ops | 3,052.5 µs | 2,878.2 µs | 2,949.3 µs | 3,076.8 µs | 3,349.4 µs | 3,349.4 µs | 47.4 µs |
| `group-7560` | KDS | 10 ops | 5,887.7 µs | 5,751.9 µs | 5,868.0 µs | 5,890.5 µs | 6,001.8 µs | 6,001.8 µs | 1,265.2 µs |
| `group-7560` | PostgreSQL | 10 ops | 7,521.5 µs | 7,332.6 µs | 7,409.8 µs | 7,481.0 µs | 7,730.8 µs | 7,730.8 µs | 4,310.2 µs |
| `group-7560-sum` | KDS | 10 ops | 7,233.8 µs | 7,134.8 µs | 7,159.2 µs | 7,212.9 µs | 7,466.1 µs | 7,466.1 µs | 1,866.8 µs |
| `group-7560-sum` | PostgreSQL | 10 ops | 8,999.9 µs | 8,814.8 µs | 8,920.1 µs | 9,011.0 µs | 9,159.0 µs | 9,159.0 µs | 5,106.4 µs |
| `group-60480` | KDS | 5 ops | 24,357.4 µs | 23,516.8 µs | 23,781.0 µs | 23,849.8 µs | 25,807.6 µs | 25,807.6 µs | 9,874.2 µs |
| `group-60480` | PostgreSQL | 5 ops | 42,350.7 µs | 41,719.6 µs | 41,742.9 µs | 42,261.4 µs | 43,062.8 µs | 43,062.8 µs | 35,540.2 µs |

## What each step costs, by subtraction (server CPU a row)

| step | KDS | PostgreSQL |
|---|---|---|
| a row walked and rejected on its bytes (`scan-reject`) | 10.9 ns | 17.5 ns |
| a row handed to the fold and counted (`count-star` less `scan-reject`) | **+8.3 ns** | **-5.4 ns** (PostgreSQL's count is cheaper than its filter) |
| one `int64` cell decoded and tested (`count-col` less `count-star`) | +8.4 ns | +7.0 ns |
| adding it (`sum` less `count-col`) | +0.1 ns | +1.3 ns |
| a second fold over one decoded cell (`min-max` less `count-col`) | +4.7 ns | +3.3 ns |
| four aggregates over two cells (`agg-global` less `count-star`) | +19.8 ns | +15.9 ns |
| a `GROUP BY` probe a row, 8 groups (`group-8` less `count-star`) | +23.5 ns | +26.2 ns |
| founding 7,560 groups (`group-7560` less `group-8`, a group) | about 230 ns a group | about 160 ns a group |
| founding a group for every row (`group-60480` less `group-8`, a row; the 60,480 returned rows' encode is in it) | +184.8 ns | +151.9 ns |

## The stack samples

`gdb -batch -ex "thread apply all bt 14"` on the Release server, 150 times, while one client looped the fold; the sampled thread is the one inside the statement. Release frames are inlined, so an anonymous `exec::` frame is the walk and the row's path through it (`AcceptTupleAt` and what it inlines). Percentages are of the samples that caught the statement's thread.

| fold | samples | where they stand (inclusive) |
|---|---|---|
| `agg-global` | 142 of 150 | `Aggregator::FoldInto` 34.5 %; `DecodeAndResolve` 30.3 % (`DecodeColumnsInto` 25.4 %, `DecodeOneValueInto` 14.1 %); the walk's own frame innermost in 27.5 %; `CompareValues` 2.8 % |
| `group-7560-sum` | 102 of 150 | `Aggregator::Accumulate` 56.9 %, of which the `std::unordered_map` lookup 35.3 % (`_Hash_bytes` 10.8 %, `memcmp` 3.9 %) and `FoldInto` 9.8 %; `malloc` and `free` innermost in 16.7 %; `DecodeAndResolve` 13.7 % |

## What the run ranks

1. **The path from a visible row to the fold** (8.3 ns a row, where PostgreSQL pays less than nothing): the payload's copy, the general row's checks, a `std::function` call, and the fold's loop reaching a counter. It is in every aggregate shape and is the whole of `COUNT(*)`'s 1.59x.
2. **Decode and fold of a fixed-width cell** (8.4 ns for the first, 19.8 ns for four aggregates over two): a cell is decoded into an 80-byte value and the fold interprets it per row. With item 1 it is the 11.0 ns a row between `agg-global` and PostgreSQL's.
3. **Founding a group** (about 230 ns against about 160 ns): a `std::string`-keyed node map and three allocations a group. It decides a fold with thousands of groups and nothing with eight.
4. **The probe is not a deficit** (23.5 ns against 26.2 ns).

## What is cut, absent or unverified

- **No instruction-level profile**: the host has neither `perf` nor `valgrind`. The subtraction table and 150 stack samples a fold are what ranks.
- **One relation shape**: integer cells, no `WHERE`, no NULL, no `DISTINCT`, no decimal. A fold behind a predicate, over a `varchar` key or over a wide decimal is not measured.
- **`group-60480`'s step holds the cost of encoding 60,480 returned rows** on both engines; it is not separated.
- **Scenario 4's `close` is not split per statement here** (EH-Q6); its three global folds are `count-star`'s and `sum`'s shapes and its `GROUP BY` has 1,000 groups.
- **The sampled percentages are of 102 to 142 samples**: a share under about 5 % is not resolved.

## Archive

`bench/v3.5.1/archive/eh-s0-v3.5.1-35-g01c6c741c/`: `shapes.json` (every rep's cell), the two stack-sample files (gzip), `run.sh`, the watchdog and its log, the timeline, the server configuration, the device, the precheck, the binary's hash and the tool copies' hashes.
