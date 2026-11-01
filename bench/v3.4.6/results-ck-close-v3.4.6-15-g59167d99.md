# CK close: what milestone CK's commit costs, against the commit it opened from

Removing the commit-time settle (CK-S1) delivered what the S0 spike predicted and nothing got slower. On a Release build with the driver on separate CPUs and `durability = relaxed`, the `COMMIT` statement at the closing commit is shorter than at the base by 0.6 to 1.8 µs at one INSERT per transaction, by up to 3.6 µs at ten, and by 4.2 to 5.9 µs (one assertion) and 14.7 to 17.8 µs (four assertions) at one hundred, on one and two cores. Every one of those p50 deltas agrees with the S0 spike's delta for the same cell to within 1.8 µs. The control relation (no assertion, so nothing to settle) moved by 0.2 µs or less on one core and by up to 1.5 µs on two. Under `group` the commit waits about 500 µs for the log sync and nothing in this run resolves the settle there (the control alone moves by up to 48 µs at p50 between the two servers); no `group` cell is slower or faster beyond that spread. Server CPU per commit again resolves nothing, for the reasons S0 gave (a 10 ms tick, and one window in five at twice the cost on both binaries). PostgreSQL is not part of this comparison: it parses `CREATE ASSERTION` in no release, so there is no twin and no floor for this shape.

## Run stamp

| Item | Value |
|---|---|
| Executed | 2026-11-01. Retained cells: `c1-relaxed` 07:24:41 to 07:26:58, `c2-relaxed` 07:39:54 to 07:42:45, `c1-group` 07:43:16 to 07:46:37, `c2-group` 07:47:07 to 07:50:44 UTC (see "What made the numbers unreliable" for why they are not contiguous) |
| Version (closing side) | `v3.4.6-15-g59167d99`, branch `worktree-ck-assertion-commit-settle`, worktree `ck-assertion-commit-settle`; tree clean (`git status --short` empty before and after the run) |
| Version (base side) | `v3.4.6-9-g8d183e9b`, the commit CK opened from, built in the detached worktree `/home/louis/ck-tmp/ck-spike` (its one uncommitted spike edit discarded, checked out at `8d183e9b`, `git status --short` empty) |
| What differs between the two sides | `git diff --stat 8d183e9b 59167d99 -- src include`: 7 files, 69 insertions, 129 deletions (`include/kds/exec/assertion_check.hpp`, `include/kds/storage/cabin_bound_page.hpp`, `include/kds/wal/record.hpp`, `src/exec/assertion_check.cpp`, `src/exec/assertion_replay.cpp`, `src/server/command_dispatcher.cpp`, `src/storage/device_page_store.cpp`). The side named `close` in this file is `59167d99`; the driver's own label for the second side is `spike`, kept in the raw JSON only |
| Binary provenance | Both rebuilt this session after their commit's time (`59167d99` committed 07:11:43 UTC; `8d183e9b` committed 2026-10-31 13:40:38 UTC). Measured from copies in `/home/louis/ck-tmp/run-ck-close/`: `kds_server.base` sha256 `54ea61305f8fd37328652d4b41f93a2aadc70801cc0123fa865b92e2170fbaed`, source mtime 07:12:36 UTC (built in `ck-spike/build-release`); `kds_server.close` sha256 `629f836a86e0279a1726176f3e2939cac956c01a9dfc6e1b7063e87068d23d3a`, source mtime 07:13:30 UTC (built in the milestone worktree's `build-release`, an incremental build of `kds_server`). Neither binary is older than its commit |
| Build type | Release (`CMAKE_BUILD_TYPE:STRING=Release` in both caches) |
| Device | data files and logs under `/home/louis/ck-tmp/run-ck-close/w-*` on `/dev/nvme0n1p1`, ext4 (`df -T`); not tmpfs |
| Host | Intel Xeon Platinum 8581C, 8 logical CPUs (4 cores x 2 threads, siblings 0/4, 1/5, 2/6, 3/7), a virtual machine |
| CPU placement | server pinned `taskset -c 0,1` with `reactor_cpus = 0` (1 core) or `0,1` (2 cores); driver `taskset -c 2,3`. Both servers of a configuration are up together, one driven at a time |
| Server configuration | `cores` 1 or 2, `durability` `relaxed` or `group`, `buffer_pool_frames = 65536`, `log_level = warn`, every other key default; ports 17301 (base) and 17302 (close); fresh server and fresh data file per configuration |
| Host state | A watcher logged every poll (3 to 5 s) at which a `ctest`, `cc1plus`, `make`, `cmake` or `kds_tests` process existed. Every retained cell has none. The load averages at each cell's start (1.7 to 4.4) are the tails of earlier work; each cell's JSON records `competing: []` before and after. One unrelated, idle `kds_server` (sandbox, pid 12386) was up on the host |
| Baseline | **The S0 spike result**, `bench/v3.4.6/results-ck-s0-settle-v3.4.6-12-gb66a0dbe.md`: the same driver, grid and host, measuring base against a binary whose `AssertionEnforcer::CommitTxn` only erased the pending list. It is this shape's previous measurement, so each cell reads as close-minus-base beside the S0 spike-minus-base. PostgreSQL: not measured for this shape (no twin exists) |

## What was measured

The unit is one explicit transaction, `BEGIN; K x INSERT; COMMIT`, on one connection, by `tools/assertion_settle_benchmark.py` unchanged from S0 (its docstring is its manual; `bench/docs/` stays closed per `bench/README.md`). One side is `8d183e9b`, the other `59167d99`; they are driven in interleaved windows with the side order alternating each round, one discarded warm-up round and five measured rounds, the same transaction counts on both sides (equal work).

| Axis | Values | Mapping to the driver |
|---|---|---|
| Assertions on the relation | 0 (control), 1, 4 | relations `stl_a0`, `stl_a1`, `stl_a4`; one assertion is `CHECK COUNT(*) <= 10^9 GROUP BY (grp)`, four are two COUNT and two SUM ceilings, all loose, so one INSERT makes 1 or 4 reservations |
| INSERTs per transaction K | 1, 10, 100 | every INSERT names a group no earlier row has, so each is one fresh reservation per assertion (1 to 400 reservations per commit) |
| `durability` | `relaxed`, `group` | `--durability` |
| `cores` | 1, 2 | `--cores`, `--reactor-cpus` |
| Preload rows | 1,000 | `--rows 1000`, grp = i % 64. **200 and 10,000 were not run** (see "What was cut") |

A window is `window-stmts // (K+2)` transactions of one (side, relation, K): 30,000 statements under `relaxed` (10,000 / 2,500 / 294 transactions at K = 1 / 10 / 100) and 12,000 under `group` (4,000 / 1,000 / 117). After each configuration the driver checked `COUNT(*)` of every relation against the rows inserted and that no statement was refused: `verify_problems` is `[]` in all four retained JSONs, and `sys.assertions` showed `enforcing=1` before the first window. The commit percentiles pool the five rounds (the driver keeps only the pooled distribution), so the noise floor is read from the control relation and from the two core counts, not from per-round values.

## Finding 1: the commit statement is shorter by what the spike said, and in no cell longer

The table is the closing commit's `COMMIT` latency minus the base's, over the pooled five rounds (negative: `59167d99` is faster), beside the S0 spike's p50 delta and this run's control at the same K. The last column reads the p50 delta against the control: "better than control" is a delta more than 0.5 µs below the control's; "inside floor" is a `relaxed` cell within 0.5 µs of it; "inside noise" a `group` cell within the control's own range.

| cores | durability | assertions | K | close-base p0 | close-base p25 | close-base p50 | close-base p95 | S0 spike-base p50 | control close-base p50 (same K) | verdict |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | relaxed | 0 | 1 | +0.1 µs | -0.1 µs | -0.1 µs | -0.3 µs | -0.1 µs | -0.1 µs | control |
| 1 | relaxed | 1 | 1 | -0.5 µs | -0.6 µs | -0.6 µs | -0.3 µs | -0.3 µs | -0.1 µs | inside floor |
| 1 | relaxed | 4 | 1 | -1.4 µs | -1.4 µs | -1.5 µs | -1.5 µs | -0.9 µs | -0.1 µs | better than control |
| 1 | relaxed | 0 | 10 | -0.4 µs | -0.1 µs | -0.2 µs | -1.4 µs | -0.1 µs | -0.2 µs | control |
| 1 | relaxed | 1 | 10 | -1.3 µs | -0.7 µs | -0.8 µs | -0.6 µs | -0.7 µs | -0.2 µs | better than control |
| 1 | relaxed | 4 | 10 | -1.4 µs | -2.6 µs | -2.7 µs | -3.6 µs | -2.7 µs | -0.2 µs | better than control |
| 1 | relaxed | 0 | 100 | -0.1 µs | -0.2 µs | -0.1 µs | +0.0 µs | +0.3 µs | -0.1 µs | control |
| 1 | relaxed | 1 | 100 | -2.8 µs | -3.8 µs | -4.2 µs | -7.1 µs | -4.8 µs | -0.1 µs | better than control |
| 1 | relaxed | 4 | 100 | -11.4 µs | -13.7 µs | -14.7 µs | -18.6 µs | -14.8 µs | -0.1 µs | better than control |
| 2 | relaxed | 0 | 1 | -0.3 µs | +0.1 µs | +1.5 µs | +4.2 µs | -0.1 µs | +1.5 µs | control |
| 2 | relaxed | 1 | 1 | -0.7 µs | -1.1 µs | -0.9 µs | -1.8 µs | -0.5 µs | +1.5 µs | better than control |
| 2 | relaxed | 4 | 1 | -1.8 µs | -1.9 µs | -1.8 µs | -3.3 µs | -1.5 µs | +1.5 µs | better than control |
| 2 | relaxed | 0 | 10 | -0.5 µs | +0.1 µs | +1.1 µs | -2.5 µs | +0.1 µs | +1.1 µs | control |
| 2 | relaxed | 1 | 10 | -0.9 µs | -0.6 µs | +0.1 µs | +3.4 µs | -0.8 µs | +1.1 µs | better than control |
| 2 | relaxed | 4 | 10 | -4.0 µs | -3.2 µs | -3.6 µs | -7.0 µs | -2.7 µs | +1.1 µs | better than control |
| 2 | relaxed | 0 | 100 | +1.3 µs | -0.6 µs | -1.2 µs | -2.4 µs | -0.1 µs | -1.2 µs | control |
| 2 | relaxed | 1 | 100 | -1.9 µs | -6.2 µs | -5.9 µs | -9.1 µs | -4.2 µs | -1.2 µs | better than control |
| 2 | relaxed | 4 | 100 | -11.5 µs | -16.2 µs | -17.8 µs | -38.8 µs | -16.0 µs | -1.2 µs | better than control |
| 1 | group | 0 | 1 | +24.8 µs | -6.2 µs | -13.2 µs | -32.0 µs | -1.8 µs | -13.2 µs | control |
| 1 | group | 1 | 1 | +3.6 µs | +0.3 µs | -0.8 µs | +14.3 µs | -22.6 µs | -13.2 µs | inside noise |
| 1 | group | 4 | 1 | -4.8 µs | -10.7 µs | -10.6 µs | -13.1 µs | +3.6 µs | -13.2 µs | inside noise |
| 1 | group | 0 | 10 | +5.1 µs | +1.2 µs | -0.9 µs | +5.3 µs | +23.4 µs | -0.9 µs | control |
| 1 | group | 1 | 10 | -26.1 µs | -2.1 µs | -0.4 µs | +9.7 µs | -13.0 µs | -0.9 µs | inside noise |
| 1 | group | 4 | 10 | +16.8 µs | +11.1 µs | +9.6 µs | -5.6 µs | -23.8 µs | -0.9 µs | inside noise |
| 1 | group | 0 | 100 | +28.3 µs | -0.8 µs | -11.7 µs | -8.4 µs | -0.2 µs | -11.7 µs | control |
| 1 | group | 1 | 100 | -70.3 µs | -17.4 µs | -14.1 µs | +11.6 µs | +0.8 µs | -11.7 µs | inside noise |
| 1 | group | 4 | 100 | -154.2 µs | -8.1 µs | -14.2 µs | -51.4 µs | -20.6 µs | -11.7 µs | inside noise |
| 2 | group | 0 | 1 | +25.1 µs | +42.8 µs | +48.4 µs | +57.9 µs | +9.1 µs | +48.4 µs | control |
| 2 | group | 1 | 1 | +10.0 µs | -4.4 µs | -4.7 µs | +3.7 µs | +23.3 µs | +48.4 µs | inside noise (2-core control spread) |
| 2 | group | 4 | 1 | +11.4 µs | +7.3 µs | +3.1 µs | -18.2 µs | -26.5 µs | +48.4 µs | inside noise (2-core control spread) |
| 2 | group | 0 | 10 | +4.9 µs | +17.4 µs | +21.0 µs | +36.8 µs | +10.2 µs | +21.0 µs | control |
| 2 | group | 1 | 10 | +0.3 µs | +11.9 µs | +11.2 µs | -28.9 µs | +23.5 µs | +21.0 µs | inside noise |
| 2 | group | 4 | 10 | -19.5 µs | +13.9 µs | +8.1 µs | +1.2 µs | -28.8 µs | +21.0 µs | inside noise |
| 2 | group | 0 | 100 | +83.7 µs | +59.9 µs | +39.0 µs | -45.2 µs | -25.8 µs | +39.0 µs | control |
| 2 | group | 1 | 100 | +212.7 µs | +27.6 µs | +3.1 µs | -91.4 µs | -60.4 µs | +39.0 µs | inside noise (2-core control spread) |
| 2 | group | 4 | 100 | +105.2 µs | +79.9 µs | +51.1 µs | +118.1 µs | -64.9 µs | +39.0 µs | inside noise |

Reading it. In `relaxed`, one assertion at K = 100 shortens the commit by 4.2 µs (1 core) and 5.9 µs (2 cores), four assertions by 14.7 and 17.8 µs; the S0 spike's figures for the same cells were 4.8 / 4.2 and 14.8 / 16.0 µs. At K = 10 with four assertions it is 2.7 and 3.6 µs (S0: 2.7 and 2.7), at K = 1 it is 0.6 to 1.8 µs. The p0 column agrees (K = 100, four assertions: -11.4 and -11.5 µs against S0's p0 of -13.7 and -11.5), so the whole distribution moved, not its tail. The cost removed scales with reservations (one assertion at K = 100 is about a third of four), as S0 found. Three `relaxed` asserted cells are at or inside the floor and are not findings: 1 core, one assertion, K = 1 (-0.6 µs against a control of -0.1, at the 0.5 µs threshold), and 2 cores, one assertion, K = 1 (-0.9 µs against a control that moved +1.5 µs) and K = 10 (+0.1 µs against a control of +1.1 µs). The 2-core control moved by 1.1 to 1.5 µs at K = 1 and K = 10 in this run (S0: 0.1 µs), so 1.5 µs is the floor to read the 2-core `relaxed` cells against. No `relaxed` cell is slower than base by more than its control's own movement.

Under `group` the p50 of every cell sits inside the control's range. The control (no assertion) moved by -13.2 / -0.9 / -11.7 µs at 1 core and +48.4 / +21.0 / +39.0 µs at 2 cores, against a sync of about 500 µs; the asserted cells moved by -14.2 to +9.6 µs at 1 core and -4.7 to +51.1 µs at 2 cores. The settle is below what this run resolves under `group`, as it was in S0, and no `group` cell is resolvably worse or better. The 2-core `group` control's +48 µs is a difference between the two servers that has nothing to do with assertions (no assertion exists on that relation), so it measures how far two servers of the same purpose drift apart on this host under a sync-bound path, and the 2-core `group` verdicts carry that width.

## Finding 2: server CPU per commit resolved nothing, and neither did throughput

The work order's meter again straddles zero. Every asserted cell's per-round CPU delta range includes zero except two `relaxed` K = 1, four-assertion cells (1 core: -2.6 µs, range -4.0 to 0.0; 2 cores: -30.0 µs, range -73 to -2, driven by one base round at about 130 µs, which the table's verdict column marks "see text"). Those are the sign the settle's removal predicts, but they sit at a few ticks of a 10 ms-tick meter at that window size (1 µs per commit at K = 1) and are not claimed. The control has per-round ranges as wide as the asserted cells (for example 1 core, K = 100: -2,245 to +1,531 µs). The `txn/s` columns are medians of the five per-round values (`txns / window wall`, computed by the driver, not a separate run) and share the spread; the per-cell ratio between sides runs from -43 % to +102 % and the control's runs the same width, so no throughput difference is claimed. The commit-latency percentiles in Finding 1 are the instrument that resolved anything.

| cores | durability | assertions | K | base CPU/commit median [min] | close CPU/commit median [min] | delta of medians | per-round delta range | control (0 assertions) per-round delta range, same K | txn/s median base / close | outside spread? |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | relaxed | 0 | 1 | 34.0 µs [34.0] | 34.0 µs [33.0] | +0.0 µs | -2.0 .. +3.0 µs | -2.0 .. +3.0 µs | 13084 / 12960 txn/s | control |
| 1 | relaxed | 1 | 1 | 74.0 µs [36.0] | 38.0 µs [34.0] | -36.0 µs | -59.0 .. +25.0 µs | -2.0 .. +3.0 µs | 5893 / 11927 txn/s | no (range straddles 0) |
| 1 | relaxed | 4 | 1 | 44.0 µs [40.0] | 40.0 µs [38.0] | -4.0 µs | -4.0 .. -0.0 µs | -2.0 .. +3.0 µs | 11407 / 11918 txn/s | see text |
| 1 | relaxed | 0 | 10 | 312.0 µs [180.0] | 172.0 µs [168.0] | -140.0 µs | -264.0 .. +0.0 µs | -264.0 .. +0.0 µs | 1931 / 2689 txn/s | control |
| 1 | relaxed | 1 | 10 | 192.0 µs [184.0] | 220.0 µs [188.0] | +28.0 µs | -108.0 .. +144.0 µs | -264.0 .. +0.0 µs | 2499 / 2358 txn/s | no (range straddles 0) |
| 1 | relaxed | 4 | 10 | 256.0 µs [236.0] | 244.0 µs [216.0] | -12.0 µs | -216.0 .. +4.0 µs | -264.0 .. +0.0 µs | 2134 / 2134 txn/s | no (range straddles 0) |
| 1 | relaxed | 0 | 100 | 1530.6 µs [1496.6] | 1530.6 µs [1530.6] | +0.0 µs | -2244.9 .. +1530.6 µs | -2244.9 .. +1530.6 µs | 299 / 294 txn/s | control |
| 1 | relaxed | 1 | 100 | 1802.7 µs [1768.7] | 1870.7 µs [1768.7] | +68.0 µs | -1938.8 .. +102.0 µs | -2244.9 .. +1530.6 µs | 221 / 277 txn/s | no (range straddles 0) |
| 1 | relaxed | 4 | 100 | 2142.9 µs [2006.8] | 3333.3 µs [2449.0] | +1190.5 µs | -1088.4 .. +2381.0 µs | -2244.9 .. +1530.6 µs | 250 / 154 txn/s | no (range straddles 0) |
| 2 | relaxed | 0 | 1 | 41.0 µs [37.0] | 42.0 µs [38.0] | +1.0 µs | -25.0 .. +6.0 µs | -25.0 .. +6.0 µs | 10636 / 10388 txn/s | control |
| 2 | relaxed | 1 | 1 | 44.0 µs [40.0] | 40.0 µs [40.0] | -4.0 µs | -5.0 .. +54.0 µs | -25.0 .. +6.0 µs | 9249 / 9571 txn/s | no (range straddles 0) |
| 2 | relaxed | 4 | 1 | 65.0 µs [49.0] | 46.0 µs [44.0] | -19.0 µs | -73.0 .. -2.0 µs | -25.0 .. +6.0 µs | 6730 / 9291 txn/s | see text |
| 2 | relaxed | 0 | 10 | 208.0 µs [204.0] | 200.0 µs [192.0] | -8.0 µs | -132.0 .. +192.0 µs | -132.0 .. +192.0 µs | 1690 / 2075 txn/s | control |
| 2 | relaxed | 1 | 10 | 248.0 µs [220.0] | 216.0 µs [212.0] | -32.0 µs | -68.0 .. +224.0 µs | -132.0 .. +192.0 µs | 1737 / 1872 txn/s | no (range straddles 0) |
| 2 | relaxed | 4 | 10 | 344.0 µs [276.0] | 264.0 µs [260.0] | -80.0 µs | -276.0 .. +268.0 µs | -132.0 .. +192.0 µs | 1391 / 1853 txn/s | no (range straddles 0) |
| 2 | relaxed | 0 | 100 | 1938.8 µs [1734.7] | 2483.0 µs [1768.7] | +544.2 µs | -2517.0 .. +884.4 µs | -2517.0 .. +884.4 µs | 227 / 204 txn/s | control |
| 2 | relaxed | 1 | 100 | 2244.9 µs [2040.8] | 2074.8 µs [1870.7] | -170.1 µs | -2653.1 .. +1870.7 µs | -2517.0 .. +884.4 µs | 195 / 207 txn/s | no (range straddles 0) |
| 2 | relaxed | 4 | 100 | 2789.1 µs [2312.9] | 2721.1 µs [2415.0] | -68.0 µs | -1224.5 .. +1598.6 µs | -2517.0 .. +884.4 µs | 167 / 157 txn/s | no (range straddles 0) |
| 1 | group | 0 | 1 | 107.5 µs [85.0] | 97.5 µs [82.5] | -10.0 µs | -62.5 .. +32.5 µs | -62.5 .. +32.5 µs | 1081 / 1669 txn/s | control |
| 1 | group | 1 | 1 | 117.5 µs [90.0] | 127.5 µs [102.5] | +10.0 µs | -7.5 .. +50.0 µs | -62.5 .. +32.5 µs | 1521 / 1029 txn/s | no (range straddles 0) |
| 1 | group | 4 | 1 | 120.0 µs [102.5] | 120.0 µs [92.5] | +0.0 µs | -27.5 .. +52.5 µs | -62.5 .. +32.5 µs | 1112 / 1482 txn/s | no (range straddles 0) |
| 1 | group | 0 | 10 | 250.0 µs [240.0] | 250.0 µs [240.0] | +0.0 µs | -140.0 .. +10.0 µs | -140.0 .. +10.0 µs | 781 / 922 txn/s | control |
| 1 | group | 1 | 10 | 270.0 µs [270.0] | 310.0 µs [260.0] | +40.0 µs | -10.0 .. +230.0 µs | -140.0 .. +10.0 µs | 859 / 857 txn/s | no (range straddles 0) |
| 1 | group | 4 | 10 | 410.0 µs [320.0] | 340.0 µs [310.0] | -70.0 µs | -260.0 .. +190.0 µs | -140.0 .. +10.0 µs | 437 / 794 txn/s | no (range straddles 0) |
| 1 | group | 0 | 100 | 1794.9 µs [1709.4] | 1794.9 µs [1709.4] | +0.0 µs | -85.5 .. +427.4 µs | -85.5 .. +427.4 µs | 178 / 177 txn/s | control |
| 1 | group | 1 | 100 | 3333.3 µs [2051.3] | 2136.8 µs [1965.8] | -1196.6 µs | -2393.2 .. +256.4 µs | -85.5 .. +427.4 µs | 118 / 124 txn/s | no (range straddles 0) |
| 1 | group | 4 | 100 | 2478.6 µs [2393.2] | 2564.1 µs [2307.7] | +85.5 µs | -256.4 .. +1453.0 µs | -85.5 .. +427.4 µs | 141 / 141 txn/s | no (range straddles 0) |
| 2 | group | 0 | 1 | 132.5 µs [117.5] | 150.0 µs [127.5] | +17.5 µs | -42.5 .. +32.5 µs | -42.5 .. +32.5 µs | 1498 / 986 txn/s | control |
| 2 | group | 1 | 1 | 142.5 µs [137.5] | 157.5 µs [135.0] | +15.0 µs | -70.0 .. +40.0 µs | -42.5 .. +32.5 µs | 1396 / 1028 txn/s | no (range straddles 0) |
| 2 | group | 4 | 1 | 172.5 µs [122.5] | 170.0 µs [135.0] | -2.5 µs | -72.5 .. +55.0 µs | -42.5 .. +32.5 µs | 1299 / 985 txn/s | no (range straddles 0) |
| 2 | group | 0 | 10 | 300.0 µs [290.0] | 340.0 µs [300.0] | +40.0 µs | -120.0 .. +260.0 µs | -120.0 .. +260.0 µs | 829 / 471 txn/s | control |
| 2 | group | 1 | 10 | 440.0 µs [320.0] | 360.0 µs [330.0] | -80.0 µs | -160.0 .. +230.0 µs | -120.0 .. +260.0 µs | 554 / 724 txn/s | no (range straddles 0) |
| 2 | group | 4 | 10 | 430.0 µs [380.0] | 420.0 µs [370.0] | -10.0 µs | -280.0 .. +210.0 µs | -120.0 .. +260.0 µs | 729 / 453 txn/s | no (range straddles 0) |
| 2 | group | 0 | 100 | 1965.8 µs [1880.3] | 2051.3 µs [2051.3] | +85.5 µs | -170.9 .. +512.8 µs | -170.9 .. +512.8 µs | 204 / 188 txn/s | control |
| 2 | group | 1 | 100 | 2393.2 µs [2136.8] | 2393.2 µs [2307.7] | +0.0 µs | -1453.0 .. +512.8 µs | -170.9 .. +512.8 µs | 134 / 135 txn/s | no (range straddles 0) |
| 2 | group | 4 | 100 | 3076.9 µs [2649.6] | 3076.9 µs [2649.6] | +0.0 µs | -598.3 .. +512.8 µs | -170.9 .. +512.8 µs | 126 / 131 txn/s | no (range straddles 0) |

Two limits bound this meter, both inherited from S0: `/proc/<pid>/stat` advances in 10 ms ticks (a window of N transactions resolves 10 ms / N per commit: 1 / 4 / 34 µs for `relaxed`, 2.5 / 10 / 85.5 µs for `group`), and about one window in five ran at roughly twice the CPU and wall time of its neighbours, in both binaries and in the control (for example `c1-relaxed`, 0 assertions, K = 100, base 2,218 ± 914 µs per commit). The cause was not identified.

## Where the time goes

A commit's wall time is a sum. The table splits the mean transaction into its statements and the server CPU, for the control and for four assertions, both sides. Means are used so the pieces add to the transaction (they include the slow windows, so the sums differ from the medians in Finding 1).

| Wait type | Measured here? |
|---|---|
| Client and socket round trip | Yes, indirectly: a statement's p0 to p50 on a nearly empty path is 15 to 25 µs and is nearly all round trip (`COMMIT` p50 of the control under `relaxed` is 20 to 27 µs on one core) |
| Write statement (INSERT) | Yes, the K INSERTs column |
| Durability/commit (fsync) | Yes, as the difference between a `group` and a `relaxed` COMMIT: about 450 to 550 µs at p50 (one INSERT) against 20 µs |
| Settle | Yes, as the base-minus-close delta in Finding 1 (the engine publishes no counter, and at `59167d99` there is no settle to count) |
| Read | None, no SELECT is in the unit |
| Lock or conflict wait | None: one connection, and the driver counted zero refused statements |

| cores | durability | assertions | K | side | BEGIN (mean) | K INSERTs (mean, summed) | COMMIT (mean) | transaction wall (mean) | server CPU per transaction (median of rounds) | wall minus server CPU (client, socket, sync wait) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | relaxed | 0 | 1 | base | 21.3 µs | 33.4 µs | 21.1 µs | 76.3 µs | 34.0 µs | 42.3 µs |
| 1 | relaxed | 0 | 1 | close | 25.7 µs | 33.7 µs | 21.2 µs | 81.1 µs | 34.0 µs | 47.1 µs |
| 1 | relaxed | 4 | 1 | base | 21.8 µs | 38.9 µs | 27.9 µs | 89.1 µs | 44.0 µs | 45.1 µs |
| 1 | relaxed | 4 | 1 | close | 21.6 µs | 49.0 µs | 25.4 µs | 96.5 µs | 40.0 µs | 56.5 µs |
| 1 | relaxed | 0 | 10 | base | 23.4 µs | 551.0 µs | 27.2 µs | 603.7 µs | 312.0 µs | 291.7 µs |
| 1 | relaxed | 0 | 10 | close | 22.8 µs | 344.0 µs | 23.4 µs | 392.6 µs | 172.0 µs | 220.6 µs |
| 1 | relaxed | 4 | 10 | base | 66.3 µs | 556.0 µs | 29.1 µs | 653.6 µs | 256.0 µs | 397.6 µs |
| 1 | relaxed | 4 | 10 | close | 26.1 µs | 457.0 µs | 24.5 µs | 509.9 µs | 244.0 µs | 265.9 µs |
| 1 | relaxed | 0 | 100 | base | 23.0 µs | 4420.0 µs | 30.7 µs | 4488.0 µs | 1530.6 µs | 2957.4 µs |
| 1 | relaxed | 0 | 100 | close | 34.6 µs | 4190.0 µs | 30.2 µs | 4268.8 µs | 1530.6 µs | 2738.2 µs |
| 1 | relaxed | 4 | 100 | base | 24.2 µs | 4460.0 µs | 53.9 µs | 4553.0 µs | 2142.9 µs | 2410.1 µs |
| 1 | relaxed | 4 | 100 | close | 292.2 µs | 6600.0 µs | 40.2 µs | 6953.2 µs | 3333.3 µs | 3619.9 µs |
| 2 | relaxed | 0 | 1 | base | 41.2 µs | 38.8 µs | 41.9 µs | 122.5 µs | 41.0 µs | 81.5 µs |
| 2 | relaxed | 0 | 1 | close | 26.3 µs | 63.4 µs | 30.2 µs | 120.6 µs | 42.0 µs | 78.6 µs |
| 2 | relaxed | 4 | 1 | base | 29.3 µs | 92.6 µs | 41.8 µs | 164.3 µs | 65.0 µs | 99.3 µs |
| 2 | relaxed | 4 | 1 | close | 25.0 µs | 50.1 µs | 29.5 µs | 105.2 µs | 46.0 µs | 59.2 µs |
| 2 | relaxed | 0 | 10 | base | 26.0 µs | 527.0 µs | 27.7 µs | 583.0 µs | 208.0 µs | 375.0 µs |
| 2 | relaxed | 0 | 10 | close | 24.7 µs | 524.0 µs | 25.4 µs | 577.1 µs | 200.0 µs | 377.1 µs |
| 2 | relaxed | 4 | 10 | base | 33.8 µs | 717.0 µs | 37.1 µs | 791.4 µs | 344.0 µs | 447.4 µs |
| 2 | relaxed | 4 | 10 | close | 25.4 µs | 578.0 µs | 25.7 µs | 632.3 µs | 264.0 µs | 368.3 µs |
| 2 | relaxed | 0 | 100 | base | 42.2 µs | 6090.0 µs | 34.6 µs | 6197.8 µs | 1938.8 µs | 4259.0 µs |
| 2 | relaxed | 0 | 100 | close | 27.3 µs | 5020.0 µs | 33.3 µs | 5101.4 µs | 2483.0 µs | 2618.4 µs |
| 2 | relaxed | 4 | 100 | base | 36.3 µs | 5770.0 µs | 59.9 µs | 5892.4 µs | 2789.1 µs | 3103.3 µs |
| 2 | relaxed | 4 | 100 | close | 200.2 µs | 6310.0 µs | 36.2 µs | 6573.1 µs | 2721.1 µs | 3852.0 µs |
| 1 | group | 0 | 1 | base | 65.9 µs | 71.4 µs | 777.3 µs | 915.6 µs | 107.5 µs | 808.1 µs |
| 1 | group | 0 | 1 | close | 47.7 µs | 69.9 µs | 481.3 µs | 599.9 µs | 97.5 µs | 502.4 µs |
| 1 | group | 4 | 1 | base | 57.6 µs | 81.5 µs | 693.1 µs | 833.1 µs | 120.0 µs | 713.1 µs |
| 1 | group | 4 | 1 | close | 49.7 µs | 80.8 µs | 685.3 µs | 816.7 µs | 120.0 µs | 696.7 µs |
| 1 | group | 0 | 10 | base | 53.1 µs | 526.0 µs | 670.7 µs | 1253.6 µs | 250.0 µs | 1003.6 µs |
| 1 | group | 0 | 10 | close | 36.6 µs | 505.0 µs | 882.0 µs | 1427.6 µs | 250.0 µs | 1177.6 µs |
| 1 | group | 4 | 10 | base | 53.0 µs | 640.0 µs | 1354.3 µs | 2051.5 µs | 410.0 µs | 1641.5 µs |
| 1 | group | 4 | 10 | close | 71.5 µs | 582.0 µs | 1050.9 µs | 1708.6 µs | 340.0 µs | 1368.6 µs |
| 1 | group | 0 | 100 | base | 39.1 µs | 4450.0 µs | 1984.2 µs | 6506.1 µs | 1794.9 µs | 4711.2 µs |
| 1 | group | 0 | 100 | close | 37.1 µs | 4630.0 µs | 1378.8 µs | 6080.5 µs | 1794.9 µs | 4285.6 µs |
| 1 | group | 4 | 100 | base | 40.9 µs | 5430.0 µs | 2156.3 µs | 7661.4 µs | 2478.6 µs | 5182.8 µs |
| 1 | group | 4 | 100 | close | 533.4 µs | 5400.0 µs | 1798.9 µs | 7770.3 µs | 2564.1 µs | 5206.2 µs |
| 2 | group | 0 | 1 | base | 44.5 µs | 98.8 µs | 525.1 µs | 669.4 µs | 132.5 µs | 536.9 µs |
| 2 | group | 0 | 1 | close | 49.2 µs | 79.0 µs | 857.2 µs | 986.4 µs | 150.0 µs | 836.4 µs |
| 2 | group | 4 | 1 | base | 42.5 µs | 112.0 µs | 683.3 µs | 838.7 µs | 172.5 µs | 666.2 µs |
| 2 | group | 4 | 1 | close | 50.6 µs | 89.4 µs | 768.0 µs | 909.0 µs | 170.0 µs | 739.0 µs |
| 2 | group | 0 | 10 | base | 42.9 µs | 504.0 µs | 695.1 µs | 1245.1 µs | 300.0 µs | 945.1 µs |
| 2 | group | 0 | 10 | close | 47.5 µs | 444.0 µs | 1499.4 µs | 1994.2 µs | 340.0 µs | 1654.2 µs |
| 2 | group | 4 | 10 | base | 44.2 µs | 584.0 µs | 1010.4 µs | 1642.3 µs | 430.0 µs | 1212.3 µs |
| 2 | group | 4 | 10 | close | 44.1 µs | 567.0 µs | 1318.0 µs | 1932.5 µs | 420.0 µs | 1512.5 µs |
| 2 | group | 0 | 100 | base | 46.1 µs | 4020.0 µs | 3026.8 µs | 7120.5 µs | 1965.8 µs | 5154.7 µs |
| 2 | group | 0 | 100 | close | 48.6 µs | 4120.0 µs | 1540.0 µs | 5735.4 µs | 2051.3 µs | 3684.1 µs |
| 2 | group | 4 | 100 | base | 47.6 µs | 5170.0 µs | 2102.8 µs | 7345.3 µs | 3076.9 µs | 4268.4 µs |
| 2 | group | 4 | 100 | close | 52.0 µs | 5270.0 µs | 3800.5 µs | 9148.2 µs | 3076.9 µs | 6071.3 µs |

The `wall minus server CPU` column is not a wait measurement: it is whatever the server's CPU does not cover, the client's Python socket work and the server's sleeping, and under `group` it holds the fsync. The settle's share of a `relaxed` transaction's wall time at K = 100 with four assertions is 15 to 18 µs of about 4,500 to 6,000 µs of wall time, under half a percent.

## Full COMMIT percentile table

The `COMMIT` statement's wall latency as the client saw it, all five measured rounds pooled, warm-up excluded. `p0` is the best case, `p25` the body. The `commits` column is the operation count. The `group` p95 and p99 carry single large outliers (a p99 of 195,515 µs at one `c1-group` cell), which is why p99 is read for no delta.

| cores | durability | assertions | K | side | commits | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | relaxed | 0 | 1 | base | 50000 | 16.1 µs | 19.9 µs | 20.2 µs | 24.0 µs | 28.8 µs |
| 1 | relaxed | 0 | 1 | close | 50000 | 16.2 µs | 19.8 µs | 20.1 µs | 23.7 µs | 28.9 µs |
| 1 | relaxed | 1 | 1 | base | 50000 | 16.0 µs | 20.4 µs | 20.7 µs | 24.4 µs | 31.3 µs |
| 1 | relaxed | 1 | 1 | close | 50000 | 15.5 µs | 19.8 µs | 20.1 µs | 24.1 µs | 30.6 µs |
| 1 | relaxed | 4 | 1 | base | 50000 | 16.8 µs | 21.1 µs | 21.5 µs | 25.3 µs | 32.3 µs |
| 1 | relaxed | 4 | 1 | close | 50000 | 15.4 µs | 19.7 µs | 20.0 µs | 23.8 µs | 31.3 µs |
| 1 | relaxed | 0 | 10 | base | 12500 | 17.6 µs | 20.5 µs | 20.9 µs | 25.7 µs | 32.9 µs |
| 1 | relaxed | 0 | 10 | close | 12500 | 17.2 µs | 20.4 µs | 20.7 µs | 24.3 µs | 29.9 µs |
| 1 | relaxed | 1 | 10 | base | 12500 | 18.4 µs | 21.3 µs | 21.7 µs | 26.0 µs | 31.9 µs |
| 1 | relaxed | 1 | 10 | close | 12500 | 17.1 µs | 20.6 µs | 20.9 µs | 25.4 µs | 32.2 µs |
| 1 | relaxed | 4 | 10 | base | 12500 | 19.2 µs | 23.3 µs | 23.8 µs | 30.0 µs | 44.7 µs |
| 1 | relaxed | 4 | 10 | close | 12500 | 17.8 µs | 20.7 µs | 21.1 µs | 26.4 µs | 35.0 µs |
| 1 | relaxed | 0 | 100 | base | 1470 | 22.6 µs | 26.2 µs | 26.9 µs | 34.2 µs | 41.9 µs |
| 1 | relaxed | 0 | 100 | close | 1470 | 22.5 µs | 26.0 µs | 26.8 µs | 34.2 µs | 47.9 µs |
| 1 | relaxed | 1 | 100 | base | 1470 | 25.4 µs | 30.4 µs | 31.6 µs | 42.5 µs | 58.8 µs |
| 1 | relaxed | 1 | 100 | close | 1470 | 22.6 µs | 26.6 µs | 27.4 µs | 35.4 µs | 43.7 µs |
| 1 | relaxed | 4 | 100 | base | 1470 | 34.0 µs | 41.0 µs | 43.5 µs | 56.9 µs | 80.9 µs |
| 1 | relaxed | 4 | 100 | close | 1470 | 22.6 µs | 27.3 µs | 28.8 µs | 38.3 µs | 52.5 µs |
| 2 | relaxed | 0 | 1 | base | 50000 | 15.7 µs | 20.1 µs | 20.6 µs | 28.4 µs | 42.4 µs |
| 2 | relaxed | 0 | 1 | close | 50000 | 15.4 µs | 20.2 µs | 22.1 µs | 32.6 µs | 45.3 µs |
| 2 | relaxed | 1 | 1 | base | 50000 | 16.3 µs | 21.8 µs | 25.0 µs | 36.6 µs | 49.7 µs |
| 2 | relaxed | 1 | 1 | close | 50000 | 15.6 µs | 20.7 µs | 24.1 µs | 34.8 µs | 44.5 µs |
| 2 | relaxed | 4 | 1 | base | 50000 | 17.3 µs | 22.6 µs | 25.9 µs | 38.2 µs | 61.4 µs |
| 2 | relaxed | 4 | 1 | close | 50000 | 15.5 µs | 20.7 µs | 24.1 µs | 34.9 µs | 46.7 µs |
| 2 | relaxed | 0 | 10 | base | 12500 | 17.3 µs | 21.2 µs | 22.2 µs | 35.7 µs | 62.3 µs |
| 2 | relaxed | 0 | 10 | close | 12500 | 16.8 µs | 21.3 µs | 23.3 µs | 33.2 µs | 50.7 µs |
| 2 | relaxed | 1 | 10 | base | 12500 | 18.8 µs | 22.5 µs | 25.0 µs | 36.4 µs | 57.2 µs |
| 2 | relaxed | 1 | 10 | close | 12500 | 17.9 µs | 21.9 µs | 25.1 µs | 39.8 µs | 50.8 µs |
| 2 | relaxed | 4 | 10 | base | 12500 | 21.1 µs | 25.0 µs | 28.4 µs | 42.2 µs | 97.7 µs |
| 2 | relaxed | 4 | 10 | close | 12500 | 17.1 µs | 21.8 µs | 24.8 µs | 35.2 µs | 48.0 µs |
| 2 | relaxed | 0 | 100 | base | 1470 | 25.3 µs | 29.7 µs | 33.2 µs | 48.0 µs | 62.8 µs |
| 2 | relaxed | 0 | 100 | close | 1470 | 26.6 µs | 29.1 µs | 32.0 µs | 45.6 µs | 60.3 µs |
| 2 | relaxed | 1 | 100 | base | 1470 | 28.9 µs | 36.0 µs | 39.4 µs | 58.6 µs | 97.7 µs |
| 2 | relaxed | 1 | 100 | close | 1470 | 27.0 µs | 29.8 µs | 33.5 µs | 49.5 µs | 63.3 µs |
| 2 | relaxed | 4 | 100 | base | 1470 | 37.9 µs | 46.9 µs | 51.5 µs | 86.4 µs | 136.4 µs |
| 2 | relaxed | 4 | 100 | close | 1470 | 26.4 µs | 30.7 µs | 33.7 µs | 47.6 µs | 60.6 µs |
| 1 | group | 0 | 1 | base | 20000 | 262.8 µs | 421.4 µs | 476.2 µs | 674.0 µs | 895.0 µs |
| 1 | group | 0 | 1 | close | 20000 | 287.6 µs | 415.2 µs | 463.0 µs | 642.0 µs | 768.6 µs |
| 1 | group | 1 | 1 | base | 20000 | 273.1 µs | 424.1 µs | 477.4 µs | 670.5 µs | 819.3 µs |
| 1 | group | 1 | 1 | close | 20000 | 276.7 µs | 424.4 µs | 476.6 µs | 684.8 µs | 1025.3 µs |
| 1 | group | 4 | 1 | base | 20000 | 271.8 µs | 416.1 µs | 470.4 µs | 678.2 µs | 873.2 µs |
| 1 | group | 4 | 1 | close | 20000 | 267.0 µs | 405.4 µs | 459.8 µs | 665.1 µs | 867.6 µs |
| 1 | group | 0 | 10 | base | 5000 | 328.4 µs | 479.2 µs | 538.7 µs | 741.1 µs | 898.5 µs |
| 1 | group | 0 | 10 | close | 5000 | 333.5 µs | 480.4 µs | 537.8 µs | 746.4 µs | 1304.5 µs |
| 1 | group | 1 | 10 | base | 5000 | 363.6 µs | 509.5 µs | 571.9 µs | 791.2 µs | 961.6 µs |
| 1 | group | 1 | 10 | close | 5000 | 337.5 µs | 507.4 µs | 571.5 µs | 800.9 µs | 1059.4 µs |
| 1 | group | 4 | 10 | base | 5000 | 377.7 µs | 554.5 µs | 620.5 µs | 882.4 µs | 2144.2 µs |
| 1 | group | 4 | 10 | close | 5000 | 394.5 µs | 565.6 µs | 630.1 µs | 876.8 µs | 1394.7 µs |
| 1 | group | 0 | 100 | base | 585 | 650.8 µs | 836.9 µs | 911.8 µs | 1103.3 µs | 1454.7 µs |
| 1 | group | 0 | 100 | close | 585 | 679.1 µs | 836.1 µs | 900.1 µs | 1094.9 µs | 1231.9 µs |
| 1 | group | 1 | 100 | base | 585 | 685.5 µs | 949.3 µs | 1016.7 µs | 1278.3 µs | 195515.0 µs |
| 1 | group | 1 | 100 | close | 585 | 615.2 µs | 931.9 µs | 1002.6 µs | 1289.9 µs | 52228.3 µs |
| 1 | group | 4 | 100 | base | 585 | 825.3 µs | 1073.5 µs | 1161.0 µs | 1522.3 µs | 45848.0 µs |
| 1 | group | 4 | 100 | close | 585 | 671.1 µs | 1065.4 µs | 1146.8 µs | 1470.9 µs | 6024.5 µs |
| 2 | group | 0 | 1 | base | 20000 | 271.6 µs | 440.8 µs | 502.3 µs | 717.4 µs | 907.3 µs |
| 2 | group | 0 | 1 | close | 20000 | 296.7 µs | 483.6 µs | 550.7 µs | 775.3 µs | 1140.3 µs |
| 2 | group | 1 | 1 | base | 20000 | 279.1 µs | 463.2 µs | 528.0 µs | 747.3 µs | 924.8 µs |
| 2 | group | 1 | 1 | close | 20000 | 289.1 µs | 458.8 µs | 523.3 µs | 751.0 µs | 964.1 µs |
| 2 | group | 4 | 1 | base | 20000 | 265.7 µs | 442.3 µs | 504.7 µs | 732.5 µs | 961.4 µs |
| 2 | group | 4 | 1 | close | 20000 | 277.1 µs | 449.6 µs | 507.8 µs | 714.3 µs | 928.4 µs |
| 2 | group | 0 | 10 | base | 5000 | 330.4 µs | 547.7 µs | 616.4 µs | 848.8 µs | 1042.9 µs |
| 2 | group | 0 | 10 | close | 5000 | 335.3 µs | 565.1 µs | 637.4 µs | 885.6 µs | 1767.3 µs |
| 2 | group | 1 | 10 | base | 5000 | 372.1 µs | 565.7 µs | 636.0 µs | 894.3 µs | 1631.0 µs |
| 2 | group | 1 | 10 | close | 5000 | 372.4 µs | 577.6 µs | 647.2 µs | 865.4 µs | 1029.2 µs |
| 2 | group | 4 | 10 | base | 5000 | 383.6 µs | 585.9 µs | 663.9 µs | 937.9 µs | 1269.9 µs |
| 2 | group | 4 | 10 | close | 5000 | 364.1 µs | 599.8 µs | 672.0 µs | 939.1 µs | 1705.1 µs |
| 2 | group | 0 | 100 | base | 585 | 510.9 µs | 791.1 µs | 902.4 µs | 1234.0 µs | 98139.0 µs |
| 2 | group | 0 | 100 | close | 585 | 594.6 µs | 851.0 µs | 941.4 µs | 1188.8 µs | 2909.7 µs |
| 2 | group | 1 | 100 | base | 585 | 489.5 µs | 875.5 µs | 974.6 µs | 1332.3 µs | 188195.6 µs |
| 2 | group | 1 | 100 | close | 585 | 702.2 µs | 903.1 µs | 977.7 µs | 1240.9 µs | 9114.8 µs |
| 2 | group | 4 | 100 | base | 585 | 553.5 µs | 952.7 µs | 1083.0 µs | 1425.3 µs | 16017.9 µs |
| 2 | group | 4 | 100 | close | 585 | 658.7 µs | 1032.6 µs | 1134.1 µs | 1543.4 µs | 88202.4 µs |

The round-trip floor and the host load before and after each configuration, with the `SHOW META` ping (a heavy `sys.meta` read of about 0.5 to 0.8 ms, not a bare round trip):

| cores | durability | side | `SHOW META` round trip p50 | p99 | load average before / after |
|---|---|---|---|---|---|
| 1 | relaxed | base | 485.0 µs | 708.2 µs | ['4.02', '5.98', '6.44'] / ['4.24', '5.01', '5.99'] |
| 1 | relaxed | close | 472.4 µs | 713.8 µs | ['4.02', '5.98', '6.44'] / ['4.24', '5.01', '5.99'] |
| 2 | relaxed | base | 815.3 µs | 1878.8 µs | ['4.42', '6.69', '6.62'] / ['3.10', '5.06', '6.00'] |
| 2 | relaxed | close | 814.7 µs | 1100.5 µs | ['4.42', '6.69', '6.62'] / ['3.10', '5.06', '6.00'] |
| 1 | group | base | 767.1 µs | 922.5 µs | ['2.41', '4.64', '5.82'] / ['2.26', '3.49', '5.13'] |
| 1 | group | close | 719.8 µs | 839.0 µs | ['2.41', '4.64', '5.82'] / ['2.26', '3.49', '5.13'] |
| 2 | group | base | 809.1 µs | 1038.9 µs | ['1.74', '3.23', '4.98'] / ['2.11', '2.65', '4.39'] |
| 2 | group | close | 773.0 µs | 1002.3 µs | ['1.74', '3.23', '4.98'] / ['2.11', '2.65', '4.39'] |

## What this teaches about the engine

- **CK-S1 buys what S0 priced, and S0's instrument predicted it.** The spike removed only the settle from `CommitTxn`; the closing commit removes the settle and makes `CommitTxn` void. Their commit-latency deltas against the same base agree to within 1.8 µs in every `relaxed` asserted cell. The spike was a faithful stand-in for the change, so S0's reading carries over: what S1 earned in commit time is up to 18 µs at 400 reservations and under 2 µs at one INSERT, a fraction of one percent of a transaction.
- **The commit path no longer scales with the reservations a transaction made.** The settle was per reservation (about 40 to 70 ns each, S0's fit) and its removal shows exactly where reservations are many: at K = 100 with four assertions the base's COMMIT p50 is 43.5 µs on one core against 28.8 µs at the closing commit, and the control, which makes no reservations, is 26.9 against 26.8 µs. The residual 2 µs between the assertion-free and the four-assertion commit at the closing commit (26.8 against 28.8 µs at p50) is not attributed here; the run cannot say whether it is the pending list's bookkeeping or the dispatcher's.
- **Under `group` the settle never showed and still does not.** A commit waits about 500 µs for the log sync; the settle's record rode that sync, and nothing here shows a sync added or removed. This is a non-result bounded by the control's drift of up to 48 µs, not evidence that the cost is zero there.
- **The case for CK-S1 is not made by speed.** As S0 said, whatever case exists for removing the settle is one failure point fewer (H2), not H3's bill; this run confirms the bill was small and is now gone, with no regression in any cell the control can read.

## What was cut, and what could not be measured

| Item | Status |
|---|---|
| Preload sizes 200 and 10,000 | Not run. Only 1,000 rows were preloaded (rule 9 asks for three sizes). The commit works on the pending list and the entry-chain tail, not the base rows, so a row-count dependence is not expected, but that is an expectation and was not measured |
| Rounds | 5 measured plus 1 warm-up per configuration, as in S0; more rounds would tighten the CPU spread and were cut for the one-hour budget |
| Per-round commit percentiles | Not kept; the pooled distribution only |
| Test suite | Not executed by this measurement; this file reports no pass |
| Counter of `ASSERT_COMMIT` records on the base side | Not read (as in S0) |
| `strict` durability | Not run: the work order's grid is `relaxed` and `group` |
| PostgreSQL | Not part of this comparison; no twin exists |

## What made the numbers unreliable

- **A concurrent session used the machine.** Another worktree (`cj-wait-events-ash`) built `kds_tests` and ran `ctest -j8` on the host while this run was in progress. The first run was aborted at 07:33 (its `c1-group` cell began under that `ctest`); its `c2-relaxed` cell (07:26:59 to 07:31:18, 4 min 19 s against 2 min 17 s for the clean `c1-relaxed`, with `make -j8 kds_tests` visible in the watcher from 07:27:51) was discarded. The retained `c1-relaxed` cell (07:24:41 to 07:26:58) finished before the first competing process appeared in the watcher (07:27:51). A second driver then ran the remaining three cells, waiting for 30 s of quiet before each; its first `c2-relaxed` attempt overlapped a `cmake --build` of another session's tree (62 watcher samples, 07:35:47 to 07:38:54), so its JSON was set aside and the cell run again. The retained `c2-relaxed`, `c1-group` and `c2-group` cells ran with no competing process in the watcher. The discarded files are in `/home/louis/ck-tmp/run-ck-close/keep/`. The watcher sees process names only; a competitor under another name would not show.
- The one-window-in-five slow rounds, and the 10 ms CPU tick, as in S0.
- The two-core control moved by up to 1.5 µs at p50 under `relaxed` and up to 48 µs under `group`, which sets how far a two-core delta can be read.
- Group-durability p95 and p99 carry single large outliers.

## Commands

```
export TMPDIR=/home/louis/ck-tmp            # /tmp is a nearly-full tmpfs
# base: detached worktree at 8d183e9b; close: the milestone worktree at 59167d99
cmake --build build-release --target kds_server -j8       # in each tree
cp build-release/kds_server /home/louis/ck-tmp/run-ck-close/kds_server.{base,close}
# per configuration, e.g. cores 1, relaxed (group used --window-stmts 12000; cores 2 used --reactor-cpus 0,1):
taskset -c 2,3 python3 -I tools/assertion_settle_benchmark.py \
    --base-bin /home/louis/ck-tmp/run-ck-close/kds_server.base \
    --spike-bin /home/louis/ck-tmp/run-ck-close/kds_server.close \
    --workdir /home/louis/ck-tmp/run-ck-close/w-c1-relaxed --cores 1 --reactor-cpus 0 \
    --durability relaxed --window-stmts 30000 --rounds 5 --json /home/louis/ck-tmp/run-ck-close/c1-relaxed.json
```

The driver's flag `--spike-bin` names the second side, which in this file is the closing commit's binary. The raw JSON of the four retained configurations is left in `/home/louis/ck-tmp/run-ck-close/` and is not archived under `bench/` (a narrower measurement archives nothing, `bench/README.md`).
