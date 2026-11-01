# CK-S0 part 4 (H3): what `AssertionEnforcer::CommitTxn`'s settle costs per commit

The settle costs a few microseconds per commit at most, and only when a transaction makes hundreds of reservations. On a Release build with the driver on separate CPUs, removing it shortened the `COMMIT` statement by 0.3 to 0.9 µs at one INSERT per transaction and by 4 to 5 µs with one assertion at 100 INSERTs (14 to 16 µs with four assertions, 400 reservations), on one and two cores with `durability = relaxed`. The control relation, which has no assertion and so no settle, moved by 0.3 µs or less in the same cells. Under `group` the fsync is about 500 µs and the run's own noise is about ±25 µs, so the settle is not resolved there. The server-CPU-per-commit figure the work order names could not resolve any cell: every delta falls inside the spread, and the spread is set by a host effect, described under "What made the numbers unreliable". PostgreSQL is not part of this comparison: it parses `CREATE ASSERTION` in no release, so there is no twin and no floor for this shape.

## Run stamp

| Item | Value |
|---|---|
| Executed | 2026-11-01, 05:11 to 05:22 UTC (four configurations, back to back) |
| Version | `v3.4.6-12-gb66a0dbe` (`git describe --tags` in the BASE tree) |
| Branch / worktree | `worktree-ck-assertion-commit-settle`, worktree `ck-assertion-commit-settle` |
| BASE tree | `/home/louis/ckdbs/.claude/worktrees/ck-assertion-commit-settle` at `b66a0dbe`, `src/` clean (the only untracked file is the new driver `tools/assertion_settle_benchmark.py`, written after the build) |
| SPIKE tree | `/home/louis/ck-tmp/ck-spike`, detached at `b66a0dbe`, dirty by design: one uncommitted edit of `src/exec/assertion_check.cpp` in which `AssertionEnforcer::CommitTxn` takes the directory latch and erases `pending_[txn]` and does nothing else (no page fetch, no `ASSERT_COMMIT`, no bit clear, no stamp). The spike is a measurement instrument and is not engine state |
| Binary provenance | Both built 05:10:04 UTC on 2026-11-01, after the commit time of `b66a0dbe` (05:07:27 UTC), so neither is older than HEAD. Measured from copies in `/home/louis/ck-tmp/run-ck-s0/`: `kds_server.base` sha256 `ea55821a371c45c99ace84effdac0e3e4ced0dfe314278e486e0379ba14b839b`, `kds_server.spike` sha256 `e0873ed3c96b442b417899d61618b5b55451a7e1883e016a3651c056b9cb980c`. Source binaries' mtime 05:10:04.16 (base) and 05:10:04.55 (spike) |
| Build type | Release (`CMAKE_BUILD_TYPE:STRING=Release` in both caches) |
| Device | data files and logs under `/home/louis/ck-tmp/run-ck-s0/w-*` on `/dev/nvme0n1p1`, ext4 (`df -T`); not tmpfs |
| Host | Intel Xeon Platinum 8581C, 8 logical CPUs (4 cores x 2 threads, siblings 0/4, 1/5, 2/6, 3/7) |
| CPU placement | server pinned with `taskset -c 0,1`, `reactor_cpus = 0` (cores 1) or `0,1` (cores 2); driver pinned with `taskset -c 2,3`. The driver shares no CPU with a server, and no server shares an SMT sibling with the driver. Both servers of a configuration are up together, only one is driven at a time |
| Server configuration | `cores` 1 or 2, `durability` `relaxed` or `group`, `buffer_pool_frames = 65536`, `log_level = warn`, every other key default; port 17301 (base) and 17302 (spike), chosen, not defaulted; fresh server and fresh data file per (cores, durability) configuration |
| Host state | no `cc1plus`, `cmake --build` or `ctest` at the start of any configuration (`pgrep -a -f`, recorded by the driver in its JSON); load averages in the last table. One unrelated `kds_server` (sandbox, pid 12386) was up on the host and near idle |
| Baseline | None for this shape: no earlier results file measures a transaction-size or assertion-count sweep of the commit. This file becomes the baseline the next one is read against. PostgreSQL is not part of this comparison |

## What was measured

The unit is one explicit transaction, `BEGIN; K x INSERT; COMMIT`, on one connection. The driver is `tools/assertion_settle_benchmark.py` (its docstring is its manual; `bench/docs/` stays closed per `bench/README.md`). It is a sibling of `tools/assertion_benchmark.py`, which is unchanged and has no transaction-size or assertion-count knob.

| Axis | Values | Mapping to the driver |
|---|---|---|
| Assertions on the relation | 0 (control), 1, 4 | relations `stl_a0`, `stl_a1`, `stl_a4`; one assertion is `CHECK COUNT(*) <= 10^9 GROUP BY (grp)`, four are two COUNT and two SUM ceilings, all loose, so one INSERT makes 1 or 4 reservations |
| INSERTs per transaction K | 1, 10, 100 | every INSERT names a group no earlier row has, so each is one fresh reservation per assertion (K, K, 100 x 4 = 400 reservations at the extremes) |
| `durability` | `relaxed`, `group` | `--durability` |
| `cores` | 1, 2 | `--cores`, `--reactor-cpus` |
| Preload rows | 1,000 | `--rows 1000`, grp = i % 64. **200 and 10,000 were not run** (see "What was cut") |

Windows. One window is one (side, relation, K) run of `window-stmts // (K+2)` transactions: 30,000 statements for `relaxed` (10,000 / 2,500 / 294 transactions at K = 1 / 10 / 100) and 12,000 for `group` (4,000 / 1,000 / 117 transactions), because a `group` commit waits for an fsync. Server CPU is `utime + stime` of the server pid read around the whole window. Every statement's wall latency is also recorded. Windows are interleaved across sides and relations, the side order alternates every round (base, spike / spike, base), and there is one discarded warm-up round then 5 measured rounds. The spread of a cell is the range of the five per-round spike-minus-base CPU deltas. The commit-latency percentiles pool the five rounds (only the pooled distribution was kept), so their spread is read from the control relation and from the two core counts agreeing, not from per-round values. Equal work: both sides ran the same transaction counts in the same order. After the run the driver checked `COUNT(*)` of every relation against rows inserted and that no statement was refused; both held in all four configurations (`verify_problems: []`), and `sys.assertions` showed `enforcing=1` for every assertion before the first window.

## Finding 1: the settle's cost is visible in the commit statement, and scales with reservations

Removing the settle shortens the COMMIT statement, in the median and at p0 together, in every `relaxed` cell with an assertion at K = 100, on both core counts. The control's delta in the same cells is 0.3 µs or less in magnitude, so the shift is not drift between the two servers. The table is the spike's percentile minus the base's, over the pooled five rounds (negative: the spike is faster).

| cores | durability | assertions | K | delta p0 | delta p25 | delta p50 | delta p95 | control delta p50, same K |
|---|---|---|---|---|---|---|---|---|
| 1 | relaxed | 0 | 1 | -0.9 µs | -0.1 µs | -0.1 µs | +0.0 µs | -0.1 µs |
| 1 | relaxed | 1 | 1 | +1.4 µs | -0.3 µs | -0.3 µs | -0.3 µs | -0.1 µs |
| 1 | relaxed | 4 | 1 | -1.3 µs | -0.9 µs | -0.9 µs | -1.1 µs | -0.1 µs |
| 1 | relaxed | 0 | 10 | +0.0 µs | -0.1 µs | -0.1 µs | -0.9 µs | -0.1 µs |
| 1 | relaxed | 1 | 10 | +0.6 µs | -0.6 µs | -0.7 µs | -0.3 µs | -0.1 µs |
| 1 | relaxed | 4 | 10 | -1.5 µs | -2.5 µs | -2.7 µs | -4.5 µs | -0.1 µs |
| 1 | relaxed | 0 | 100 | +0.5 µs | +0.1 µs | +0.3 µs | +1.3 µs | +0.3 µs |
| 1 | relaxed | 1 | 100 | -4.6 µs | -4.2 µs | -4.8 µs | -9.5 µs | +0.3 µs |
| 1 | relaxed | 4 | 100 | -13.7 µs | -13.7 µs | -14.8 µs | -16.6 µs | +0.3 µs |
| 2 | relaxed | 0 | 1 | +0.1 µs | -0.1 µs | -0.1 µs | -0.4 µs | -0.1 µs |
| 2 | relaxed | 1 | 1 | -1.3 µs | -0.5 µs | -0.5 µs | -0.7 µs | -0.1 µs |
| 2 | relaxed | 4 | 1 | -2.6 µs | -1.5 µs | -1.5 µs | -1.7 µs | -0.1 µs |
| 2 | relaxed | 0 | 10 | +0.7 µs | +0.0 µs | +0.1 µs | +0.6 µs | +0.1 µs |
| 2 | relaxed | 1 | 10 | -1.1 µs | -0.8 µs | -0.8 µs | -1.3 µs | +0.1 µs |
| 2 | relaxed | 4 | 10 | -1.6 µs | -2.7 µs | -2.7 µs | -3.6 µs | +0.1 µs |
| 2 | relaxed | 0 | 100 | +0.0 µs | -0.1 µs | -0.1 µs | +1.9 µs | -0.1 µs |
| 2 | relaxed | 1 | 100 | -3.6 µs | -4.0 µs | -4.2 µs | -4.6 µs | -0.1 µs |
| 2 | relaxed | 4 | 100 | -11.5 µs | -14.4 µs | -16.0 µs | -20.2 µs | -0.1 µs |
| 1 | group | 0 | 1 | +7.1 µs | +0.8 µs | -1.8 µs | +4.0 µs | -1.8 µs |
| 1 | group | 1 | 1 | -0.5 µs | -17.3 µs | -22.6 µs | -32.7 µs | -1.8 µs |
| 1 | group | 4 | 1 | +3.4 µs | +1.4 µs | +3.6 µs | +4.7 µs | -1.8 µs |
| 1 | group | 0 | 10 | +6.7 µs | +18.0 µs | +23.4 µs | +37.6 µs | +23.4 µs |
| 1 | group | 1 | 10 | +11.1 µs | -8.2 µs | -13.0 µs | -31.1 µs | +23.4 µs |
| 1 | group | 4 | 10 | -26.6 µs | -21.1 µs | -23.8 µs | -13.6 µs | +23.4 µs |
| 1 | group | 0 | 100 | -37.0 µs | -2.1 µs | -0.2 µs | -13.0 µs | -0.2 µs |
| 1 | group | 1 | 100 | +42.5 µs | -6.9 µs | +0.8 µs | -32.2 µs | -0.2 µs |
| 1 | group | 4 | 100 | +184.8 µs | -16.3 µs | -20.6 µs | +27.1 µs | -0.2 µs |
| 2 | group | 0 | 1 | -23.7 µs | +6.0 µs | +9.1 µs | +27.7 µs | +9.1 µs |
| 2 | group | 1 | 1 | -1.0 µs | +19.3 µs | +23.3 µs | +30.2 µs | +9.1 µs |
| 2 | group | 4 | 1 | -12.2 µs | -22.6 µs | -26.5 µs | -28.6 µs | +9.1 µs |
| 2 | group | 0 | 10 | -26.9 µs | +10.1 µs | +10.2 µs | +28.0 µs | +10.2 µs |
| 2 | group | 1 | 10 | +21.0 µs | +16.8 µs | +23.5 µs | +77.3 µs | +10.2 µs |
| 2 | group | 4 | 10 | -12.0 µs | -27.1 µs | -28.8 µs | -15.3 µs | +10.2 µs |
| 2 | group | 0 | 100 | -174.2 µs | -36.3 µs | -25.8 µs | +29.8 µs | -25.8 µs |
| 2 | group | 1 | 100 | -272.2 µs | -79.0 µs | -60.4 µs | -78.9 µs | -25.8 µs |
| 2 | group | 4 | 100 | -17.6 µs | -109.3 µs | -64.9 µs | -65.5 µs | -25.8 µs |

Reading it. In `relaxed`, one assertion at K = 100 costs about 4 to 5 µs per commit (p50 -4.8 µs on one core, -4.2 µs on two); four assertions at K = 100 cost about 15 µs (-14.8 and -16.0 µs); K = 10 with four assertions costs 2.7 µs on both core counts; K = 1 costs 0.3 to 0.9 µs with one and four assertions on one core and 0.5 to 1.5 µs on two. Dividing by reservations gives roughly 40 to 70 ns per reservation (15 µs / 400, 4.5 µs / 100, 2.7 µs / 40) plus a fixed few tenths of a microsecond. A per-page cost that did not scale with reservations would have made the K = 1 and K = 100 cells with one assertion equal, and they are not. That is consistent with a settle whose bulk is per-reservation work (the pending-list grouping, the record's index list, the bit clears) and a small per-(assertion, page) part (one fetch, one record, one stamp); this run cannot separate the two, because the page count per assertion was not varied. Do not read the fit as a measured per-reservation constant. Under `group` every difference is inside the control's own movement (the control moves by up to 26 µs at p50 between the two servers, against a sync of about 500 µs); the settle is below what this run resolves there, not shown to be zero.

## Finding 2: server CPU per commit resolved nothing

The figure the work order asks for. Every cell's per-round delta range straddles zero (or touches it) except one: the `group`, two-core, four-assertion, K = 100 cell, whose five deltas are all positive (+85.5 to +1,709 µs, the spike spending more CPU, which is the wrong sign for a removed cost and so is not the settle); and the control, which cannot be affected, has per-round ranges as wide as the asserted cells'. The `CPU/commit` columns are the median of five per-round values, with the smallest round in brackets.

| cores | durability | assertions | K | base CPU/commit median [min] | spike CPU/commit median [min] | delta of medians | per-round delta range | control (0 assertions) per-round delta range, same K | txn/s median base / spike | outside spread? |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | relaxed | 0 | 1 | 34.0 µs [33.0] | 34.0 µs [33.0] | -0.0 µs | -1.0 .. +62.0 µs | -1.0 .. +62.0 µs | 13081 / 13204 txn/s | control |
| 1 | relaxed | 1 | 1 | 72.0 µs [35.0] | 37.0 µs [35.0] | -35.0 µs | -60.0 .. +15.0 µs | -1.0 .. +62.0 µs | 6675 / 11593 txn/s | no (range straddles 0) |
| 1 | relaxed | 4 | 1 | 44.0 µs [40.0] | 43.0 µs [40.0] | -1.0 µs | -18.0 .. +3.0 µs | -1.0 .. +62.0 µs | 11511 / 9839 txn/s | no (range straddles 0) |
| 1 | relaxed | 0 | 10 | 224.0 µs [168.0] | 176.0 µs [168.0] | -48.0 µs | -264.0 .. +16.0 µs | -264.0 .. +16.0 µs | 2347 / 2666 txn/s | control |
| 1 | relaxed | 1 | 10 | 188.0 µs [184.0] | 240.0 µs [188.0] | +52.0 µs | +0.0 .. +172.0 µs | -264.0 .. +16.0 µs | 2544 / 2260 txn/s | no (range straddles 0) |
| 1 | relaxed | 4 | 10 | 400.0 µs [236.0] | 236.0 µs [224.0] | -164.0 µs | -248.0 .. +4.0 µs | -264.0 .. +16.0 µs | 1437 / 2204 txn/s | no (range straddles 0) |
| 1 | relaxed | 0 | 100 | 1530.6 µs [1496.6] | 2142.9 µs [1530.6] | +612.2 µs | -2278.9 .. +1224.5 µs | -2278.9 .. +1224.5 µs | 301 / 238 txn/s | control |
| 1 | relaxed | 1 | 100 | 1802.7 µs [1666.7] | 1802.7 µs [1700.7] | +0.0 µs | -1972.8 .. +884.4 µs | -2278.9 .. +1224.5 µs | 217 / 276 txn/s | no (range straddles 0) |
| 1 | relaxed | 4 | 100 | 2142.9 µs [2040.8] | 2244.9 µs [2108.8] | +102.0 µs | -204.1 .. +2551.0 µs | -2278.9 .. +1224.5 µs | 250 / 234 txn/s | no (range straddles 0) |
| 2 | relaxed | 0 | 1 | 38.0 µs [36.0] | 37.0 µs [37.0] | -1.0 µs | -53.0 .. +1.0 µs | -53.0 .. +1.0 µs | 12742 / 12947 txn/s | control |
| 2 | relaxed | 1 | 1 | 40.0 µs [38.0] | 40.0 µs [38.0] | +0.0 µs | -35.0 .. +20.0 µs | -53.0 .. +1.0 µs | 12428 / 12426 txn/s | no (range straddles 0) |
| 2 | relaxed | 4 | 1 | 46.0 µs [44.0] | 44.0 µs [42.0] | -2.0 µs | -56.0 .. +0.0 µs | -53.0 .. +1.0 µs | 10893 / 11773 txn/s | no (range straddles 0) |
| 2 | relaxed | 0 | 10 | 184.0 µs [184.0] | 192.0 µs [180.0] | +8.0 µs | -48.0 .. +20.0 µs | -48.0 .. +20.0 µs | 2606 / 2571 txn/s | control |
| 2 | relaxed | 1 | 10 | 296.0 µs [208.0] | 228.0 µs [204.0] | -68.0 µs | -240.0 .. +128.0 µs | -48.0 .. +20.0 µs | 2048 / 2379 txn/s | no (range straddles 0) |
| 2 | relaxed | 4 | 10 | 264.0 µs [256.0] | 264.0 µs [252.0] | +0.0 µs | -4.0 .. +200.0 µs | -48.0 .. +20.0 µs | 2162 / 2171 txn/s | no (range straddles 0) |
| 2 | relaxed | 0 | 100 | 1870.7 µs [1666.7] | 2551.0 µs [1972.8] | +680.3 µs | -1496.6 .. +1904.8 µs | -1496.6 .. +1904.8 µs | 284 / 214 txn/s | control |
| 2 | relaxed | 1 | 100 | 1938.8 µs [1836.7] | 2040.8 µs [1972.8] | +102.0 µs | -782.3 .. +1734.7 µs | -1496.6 .. +1904.8 µs | 274 / 273 txn/s | no (range straddles 0) |
| 2 | relaxed | 4 | 100 | 2517.0 µs [2346.9] | 2721.1 µs [2244.9] | +204.1 µs | -2108.8 .. +2244.9 µs | -1496.6 .. +1904.8 µs | 181 / 237 txn/s | no (range straddles 0) |
| 1 | group | 0 | 1 | 92.5 µs [87.5] | 95.0 µs [90.0] | +2.5 µs | -15.0 .. +40.0 µs | -15.0 .. +40.0 µs | 1552 / 1557 txn/s | control |
| 1 | group | 1 | 1 | 100.0 µs [95.0] | 122.5 µs [95.0] | +22.5 µs | -12.5 .. +47.5 µs | -15.0 .. +40.0 µs | 1482 / 1505 txn/s | no (range straddles 0) |
| 1 | group | 4 | 1 | 102.5 µs [100.0] | 132.5 µs [107.5] | +30.0 µs | -15.0 .. +50.0 µs | -15.0 .. +40.0 µs | 1490 / 1421 txn/s | no (range straddles 0) |
| 1 | group | 0 | 10 | 230.0 µs [230.0] | 240.0 µs [230.0] | +10.0 µs | -60.0 .. +260.0 µs | -60.0 .. +260.0 µs | 1015 / 1010 txn/s | control |
| 1 | group | 1 | 10 | 300.0 µs [250.0] | 260.0 µs [250.0] | -40.0 µs | -210.0 .. +180.0 µs | -60.0 .. +260.0 µs | 929 / 977 txn/s | no (range straddles 0) |
| 1 | group | 4 | 10 | 330.0 µs [310.0] | 350.0 µs [300.0] | +20.0 µs | -10.0 .. +200.0 µs | -60.0 .. +260.0 µs | 801 / 838 txn/s | no (range straddles 0) |
| 1 | group | 0 | 100 | 1538.5 µs [1538.5] | 1538.5 µs [1538.5] | +0.0 µs | -85.5 .. +170.9 µs | -85.5 .. +170.9 µs | 231 / 229 txn/s | control |
| 1 | group | 1 | 100 | 1880.3 µs [1794.9] | 1794.9 µs [1709.4] | -85.5 µs | -1794.9 .. +85.5 µs | -85.5 .. +170.9 µs | 195 / 214 txn/s | no (range straddles 0) |
| 1 | group | 4 | 100 | 2222.2 µs [2222.2] | 2393.2 µs [2136.8] | +170.9 µs | -1367.5 .. +1025.6 µs | -85.5 .. +170.9 µs | 190 / 132 txn/s | no (range straddles 0) |
| 2 | group | 0 | 1 | 155.0 µs [127.5] | 142.5 µs [135.0] | -12.5 µs | -45.0 .. +15.0 µs | -45.0 .. +15.0 µs | 1459 / 1378 txn/s | control |
| 2 | group | 1 | 1 | 147.5 µs [127.5] | 152.5 µs [137.5] | +5.0 µs | -62.5 .. +35.0 µs | -45.0 .. +15.0 µs | 1331 / 1373 txn/s | no (range straddles 0) |
| 2 | group | 4 | 1 | 167.5 µs [142.5] | 160.0 µs [145.0] | -7.5 µs | -45.0 .. +40.0 µs | -45.0 .. +15.0 µs | 1317 / 1347 txn/s | no (range straddles 0) |
| 2 | group | 0 | 10 | 290.0 µs [290.0] | 340.0 µs [290.0] | +50.0 µs | -110.0 .. +180.0 µs | -110.0 .. +180.0 µs | 861 / 850 txn/s | control |
| 2 | group | 1 | 10 | 360.0 µs [320.0] | 370.0 µs [320.0] | +10.0 µs | -160.0 .. +120.0 µs | -110.0 .. +180.0 µs | 730 / 776 txn/s | no (range straddles 0) |
| 2 | group | 4 | 10 | 380.0 µs [370.0] | 430.0 µs [380.0] | +50.0 µs | -140.0 .. +230.0 µs | -110.0 .. +180.0 µs | 769 / 754 txn/s | no (range straddles 0) |
| 2 | group | 0 | 100 | 1880.3 µs [1794.9] | 1880.3 µs [1880.3] | +0.0 µs | -0.0 .. +769.2 µs | -0.0 .. +769.2 µs | 214 / 219 txn/s | control |
| 2 | group | 1 | 100 | 2051.3 µs [1880.3] | 2136.8 µs [1965.8] | +85.5 µs | -2478.6 .. +170.9 µs | -0.0 .. +769.2 µs | 202 / 209 txn/s | no (range straddles 0) |
| 2 | group | 4 | 100 | 2564.1 µs [2478.6] | 2906.0 µs [2649.6] | +341.9 µs | +85.5 .. +1709.4 µs | -0.0 .. +769.2 µs | 165 / 127 txn/s | see text |

Two things bound this meter. First, `/proc/<pid>/stat` advances in 10 ms ticks, so a window of N transactions has a resolution of 10 ms / N per commit: 1 µs (K = 1) / 4 µs (K = 10) / 34 µs (K = 100) for `relaxed`, and 2.5 / 10 / 85.5 µs for `group`. The settle at K = 100 (5 to 16 µs) is below one tick of its window, and the observed deltas of the smallest rounds, +34 and +68 µs, are one and two ticks. Second, roughly one window in five ran at about twice the CPU and twice the wall time of its neighbours, in both binaries and in the control (for example `c1-relaxed`, 0 assertions, K = 100, base: five rounds of 1,633, 1,497, 1,531, 1,531 and 3,810 µs/commit). The cause was not identified. It makes a mean or a median over five rounds unreliable and is the reason this table's spread is as wide as it is. No cell's CPU delta is outside the spread, and no claim about CPU share follows from it.

## Where the time goes

A commit's wall time is a sum. The table splits the mean transaction into its statements and then subtracts the server CPU, for the control and for four assertions, base and spike. Means are used so that the pieces add to the transaction (they include the slow windows above, so the sums differ from the medians in Finding 1).

| Wait type | Measured here? |
|---|---|
| Client and socket round trip | Yes, indirectly: a statement's p0 to p50 on a nearly empty path is about 15 to 21 µs and is nearly all round trip (`COMMIT` p50 of the control under `relaxed` is 20 to 28 µs) |
| Write statement (INSERT) | Yes, the K INSERTs column |
| Durability/commit (fsync) | Yes, as the difference between a `group` and a `relaxed` COMMIT: about 500 µs at p50 (one INSERT) against 20 µs |
| Settle | Yes, as the base-minus-spike delta in Finding 1 (never a separate counter: the engine publishes none) |
| Read | None, no SELECT is in the unit |
| Lock or conflict wait | None: one connection, and the driver counted zero refused statements |

| cores | durability | assertions | K | side | BEGIN (mean) | K INSERTs (mean, summed) | COMMIT (mean) | transaction wall (mean) | server CPU per transaction (median of rounds) | wall minus server CPU (client, socket, sync wait) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | relaxed | 0 | 1 | base | 21.2 µs | 33.3 µs | 21.2 µs | 76.2 µs | 34.0 µs | 42.2 µs |
| 1 | relaxed | 0 | 1 | spike | 22.3 µs | 33.5 µs | 58.3 µs | 114.6 µs | 34.0 µs | 80.6 µs |
| 1 | relaxed | 4 | 1 | base | 27.9 µs | 39.3 µs | 25.1 µs | 92.8 µs | 44.0 µs | 48.8 µs |
| 1 | relaxed | 4 | 1 | spike | 31.3 µs | 45.2 µs | 25.5 µs | 102.4 µs | 43.0 µs | 59.4 µs |
| 1 | relaxed | 0 | 10 | base | 30.3 µs | 495.0 µs | 26.3 µs | 553.8 µs | 224.0 µs | 329.8 µs |
| 1 | relaxed | 0 | 10 | spike | 22.1 µs | 356.0 µs | 22.8 µs | 402.8 µs | 176.0 µs | 226.8 µs |
| 1 | relaxed | 4 | 10 | base | 115.4 µs | 496.0 µs | 106.2 µs | 720.0 µs | 400.0 µs | 320.0 µs |
| 1 | relaxed | 4 | 10 | spike | 22.4 µs | 445.0 µs | 23.2 µs | 492.5 µs | 236.0 µs | 256.5 µs |
| 1 | relaxed | 0 | 100 | base | 25.9 µs | 3880.0 µs | 36.1 µs | 3960.2 µs | 1530.6 µs | 2429.6 µs |
| 1 | relaxed | 0 | 100 | spike | 158.5 µs | 4130.0 µs | 28.9 µs | 4332.5 µs | 2142.9 µs | 2189.6 µs |
| 1 | relaxed | 4 | 100 | base | 23.6 µs | 4490.0 µs | 47.2 µs | 4576.3 µs | 2142.9 µs | 2433.4 µs |
| 1 | relaxed | 4 | 100 | spike | 41.3 µs | 5770.0 µs | 35.1 µs | 5869.6 µs | 2244.9 µs | 3624.7 µs |
| 2 | relaxed | 0 | 1 | base | 29.3 µs | 47.2 µs | 21.4 µs | 98.3 µs | 38.0 µs | 60.3 µs |
| 2 | relaxed | 0 | 1 | spike | 21.4 µs | 34.4 µs | 21.0 µs | 77.3 µs | 37.0 µs | 40.3 µs |
| 2 | relaxed | 4 | 1 | base | 22.4 µs | 73.7 µs | 38.5 µs | 135.0 µs | 46.0 µs | 89.0 µs |
| 2 | relaxed | 4 | 1 | spike | 27.0 µs | 40.8 µs | 21.3 µs | 89.6 µs | 44.0 µs | 45.6 µs |
| 2 | relaxed | 0 | 10 | base | 22.4 µs | 347.0 µs | 21.9 µs | 393.4 µs | 184.0 µs | 209.4 µs |
| 2 | relaxed | 0 | 10 | spike | 21.6 µs | 341.0 µs | 22.0 µs | 387.2 µs | 192.0 µs | 195.2 µs |
| 2 | relaxed | 4 | 10 | base | 22.0 µs | 426.0 µs | 25.8 µs | 476.4 µs | 264.0 µs | 212.4 µs |
| 2 | relaxed | 4 | 10 | spike | 21.9 µs | 472.0 µs | 22.5 µs | 518.7 µs | 264.0 µs | 254.7 µs |
| 2 | relaxed | 0 | 100 | base | 23.4 µs | 4160.0 µs | 29.9 µs | 4232.9 µs | 1870.7 µs | 2362.2 µs |
| 2 | relaxed | 0 | 100 | spike | 23.1 µs | 4830.0 µs | 29.7 µs | 4900.7 µs | 2551.0 µs | 2349.7 µs |
| 2 | relaxed | 4 | 100 | base | 25.4 µs | 5290.0 µs | 48.0 µs | 5378.6 µs | 2517.0 µs | 2861.6 µs |
| 2 | relaxed | 4 | 100 | spike | 23.5 µs | 5260.0 µs | 31.5 µs | 5329.9 µs | 2721.1 µs | 2608.8 µs |
| 1 | group | 0 | 1 | base | 54.0 µs | 70.2 µs | 555.2 µs | 680.5 µs | 92.5 µs | 588.0 µs |
| 1 | group | 0 | 1 | spike | 44.2 µs | 70.5 µs | 525.6 µs | 641.3 µs | 95.0 µs | 546.3 µs |
| 1 | group | 4 | 1 | base | 41.5 µs | 81.2 µs | 530.3 µs | 654.0 µs | 102.5 µs | 551.5 µs |
| 1 | group | 4 | 1 | spike | 63.7 µs | 81.6 µs | 558.6 µs | 704.9 µs | 132.5 µs | 572.4 µs |
| 1 | group | 0 | 10 | base | 42.5 µs | 388.0 µs | 702.3 µs | 1135.6 µs | 230.0 µs | 905.6 µs |
| 1 | group | 0 | 10 | spike | 83.1 µs | 388.0 µs | 761.4 µs | 1235.9 µs | 240.0 µs | 995.9 µs |
| 1 | group | 4 | 10 | base | 37.4 µs | 465.0 µs | 833.1 µs | 1338.9 µs | 330.0 µs | 1008.9 µs |
| 1 | group | 4 | 10 | spike | 37.8 µs | 517.0 µs | 705.8 µs | 1263.8 µs | 350.0 µs | 913.8 µs |
| 1 | group | 0 | 100 | base | 39.6 µs | 3330.0 µs | 958.2 µs | 4346.9 µs | 1538.5 µs | 2808.4 µs |
| 1 | group | 0 | 100 | spike | 39.1 µs | 3430.0 µs | 952.5 µs | 4442.2 µs | 1538.5 µs | 2903.7 µs |
| 1 | group | 4 | 100 | base | 293.0 µs | 3950.0 µs | 2160.5 µs | 6422.2 µs | 2222.2 µs | 4200.0 µs |
| 1 | group | 4 | 100 | spike | 168.2 µs | 3980.0 µs | 3492.5 µs | 7655.9 µs | 2393.2 µs | 5262.7 µs |
| 2 | group | 0 | 1 | base | 60.4 µs | 85.2 µs | 565.4 µs | 712.1 µs | 155.0 µs | 557.1 µs |
| 2 | group | 0 | 1 | spike | 46.7 µs | 96.1 µs | 575.5 µs | 719.4 µs | 142.5 µs | 576.9 µs |
| 2 | group | 4 | 1 | base | 46.8 µs | 92.7 µs | 646.6 µs | 787.1 µs | 167.5 µs | 619.6 µs |
| 2 | group | 4 | 1 | spike | 45.3 µs | 112.6 µs | 598.1 µs | 757.2 µs | 160.0 µs | 597.2 µs |
| 2 | group | 0 | 10 | base | 70.4 µs | 398.0 µs | 719.7 µs | 1191.4 µs | 290.0 µs | 901.4 µs |
| 2 | group | 0 | 10 | spike | 45.9 µs | 485.0 µs | 682.9 µs | 1216.6 µs | 340.0 µs | 876.6 µs |
| 2 | group | 4 | 10 | base | 73.3 µs | 523.0 µs | 732.1 µs | 1331.7 µs | 380.0 µs | 951.7 µs |
| 2 | group | 4 | 10 | spike | 44.0 µs | 577.0 µs | 713.7 µs | 1337.2 µs | 430.0 µs | 907.2 µs |
| 2 | group | 0 | 100 | base | 51.5 µs | 3570.0 µs | 1541.3 µs | 5185.8 µs | 1880.3 µs | 3305.5 µs |
| 2 | group | 0 | 100 | spike | 51.0 µs | 3670.0 µs | 1047.7 µs | 4788.6 µs | 1880.3 µs | 2908.3 µs |
| 2 | group | 4 | 100 | base | 50.3 µs | 4270.0 µs | 2235.4 µs | 6575.3 µs | 2564.1 µs | 4011.2 µs |
| 2 | group | 4 | 100 | spike | 49.1 µs | 4250.0 µs | 3483.4 µs | 7801.7 µs | 2906.0 µs | 4895.7 µs |

The `wall minus server CPU` column is not a wait measurement: it is whatever the server's CPU does not cover, which is the client's Python socket work and the server's sleeping, and under `group` it holds the fsync. Under `relaxed` the settle's share of the transaction's wall time at K = 100 is 15 / 4,500 µs, under half a percent; at K = 1 it is under 1 %.

## Full COMMIT percentile table

The COMMIT statement's wall latency as the client saw it, over all five measured rounds (warm-up excluded). `p0` is the best case, `p25` the body. The `commits` column is the operation count.

| cores | durability | assertions | K | side | commits | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | relaxed | 0 | 1 | base | 50000 | 15.8 µs | 19.9 µs | 20.2 µs | 23.6 µs | 28.8 µs |
| 1 | relaxed | 0 | 1 | spike | 50000 | 14.9 µs | 19.8 µs | 20.1 µs | 23.6 µs | 29.1 µs |
| 1 | relaxed | 1 | 1 | base | 50000 | 15.9 µs | 20.3 µs | 20.7 µs | 24.4 µs | 30.7 µs |
| 1 | relaxed | 1 | 1 | spike | 50000 | 17.3 µs | 20.0 µs | 20.4 µs | 24.1 µs | 30.5 µs |
| 1 | relaxed | 4 | 1 | base | 50000 | 16.9 µs | 21.0 µs | 21.3 µs | 25.1 µs | 32.8 µs |
| 1 | relaxed | 4 | 1 | spike | 50000 | 15.6 µs | 20.1 µs | 20.4 µs | 24.0 µs | 29.9 µs |
| 1 | relaxed | 0 | 10 | base | 12500 | 17.7 µs | 20.6 µs | 21.0 µs | 25.5 µs | 31.4 µs |
| 1 | relaxed | 0 | 10 | spike | 12500 | 17.7 µs | 20.5 µs | 20.9 µs | 24.6 µs | 30.5 µs |
| 1 | relaxed | 1 | 10 | base | 12500 | 16.8 µs | 21.4 µs | 21.8 µs | 25.6 µs | 31.2 µs |
| 1 | relaxed | 1 | 10 | spike | 12500 | 17.4 µs | 20.8 µs | 21.1 µs | 25.3 µs | 32.3 µs |
| 1 | relaxed | 4 | 10 | base | 12500 | 19.8 µs | 23.3 µs | 23.8 µs | 29.7 µs | 41.1 µs |
| 1 | relaxed | 4 | 10 | spike | 12500 | 18.3 µs | 20.8 µs | 21.1 µs | 25.2 µs | 31.5 µs |
| 1 | relaxed | 0 | 100 | base | 1470 | 22.4 µs | 26.0 µs | 26.6 µs | 33.2 µs | 42.5 µs |
| 1 | relaxed | 0 | 100 | spike | 1470 | 22.9 µs | 26.1 µs | 26.9 µs | 34.5 µs | 44.4 µs |
| 1 | relaxed | 1 | 100 | base | 1470 | 26.4 µs | 30.6 µs | 32.0 µs | 44.0 µs | 65.8 µs |
| 1 | relaxed | 1 | 100 | spike | 1470 | 21.8 µs | 26.4 µs | 27.2 µs | 34.5 µs | 43.2 µs |
| 1 | relaxed | 4 | 100 | base | 1470 | 36.2 µs | 40.8 µs | 43.1 µs | 54.8 µs | 72.3 µs |
| 1 | relaxed | 4 | 100 | spike | 1470 | 22.5 µs | 27.1 µs | 28.3 µs | 38.2 µs | 54.1 µs |
| 2 | relaxed | 0 | 1 | base | 50000 | 17.0 µs | 20.0 µs | 20.3 µs | 24.0 µs | 29.4 µs |
| 2 | relaxed | 0 | 1 | spike | 50000 | 17.1 µs | 19.9 µs | 20.2 µs | 23.6 µs | 28.3 µs |
| 2 | relaxed | 1 | 1 | base | 50000 | 17.3 µs | 20.4 µs | 20.7 µs | 24.4 µs | 29.6 µs |
| 2 | relaxed | 1 | 1 | spike | 50000 | 16.0 µs | 19.9 µs | 20.2 µs | 23.7 µs | 29.0 µs |
| 2 | relaxed | 4 | 1 | base | 50000 | 18.0 µs | 21.5 µs | 21.8 µs | 26.0 µs | 31.6 µs |
| 2 | relaxed | 4 | 1 | spike | 50000 | 15.4 µs | 20.0 µs | 20.3 µs | 24.3 µs | 29.3 µs |
| 2 | relaxed | 0 | 10 | base | 12500 | 17.1 µs | 20.7 µs | 21.0 µs | 24.9 µs | 29.8 µs |
| 2 | relaxed | 0 | 10 | spike | 12500 | 17.8 µs | 20.7 µs | 21.1 µs | 25.5 µs | 30.4 µs |
| 2 | relaxed | 1 | 10 | base | 12500 | 18.3 µs | 21.6 µs | 21.9 µs | 26.3 µs | 32.2 µs |
| 2 | relaxed | 1 | 10 | spike | 12500 | 17.2 µs | 20.8 µs | 21.1 µs | 25.0 µs | 30.3 µs |
| 2 | relaxed | 4 | 10 | base | 12500 | 19.6 µs | 23.6 µs | 24.0 µs | 28.9 µs | 35.6 µs |
| 2 | relaxed | 4 | 10 | spike | 12500 | 18.0 µs | 20.9 µs | 21.3 µs | 25.3 µs | 31.5 µs |
| 2 | relaxed | 0 | 100 | base | 1470 | 23.5 µs | 27.7 µs | 28.4 µs | 35.8 µs | 48.3 µs |
| 2 | relaxed | 0 | 100 | spike | 1470 | 23.5 µs | 27.6 µs | 28.3 µs | 37.7 µs | 45.2 µs |
| 2 | relaxed | 1 | 100 | base | 1470 | 27.4 µs | 32.1 µs | 33.1 µs | 41.5 µs | 53.2 µs |
| 2 | relaxed | 1 | 100 | spike | 1470 | 23.8 µs | 28.1 µs | 28.9 µs | 36.9 µs | 44.6 µs |
| 2 | relaxed | 4 | 100 | base | 1470 | 36.3 µs | 43.7 µs | 46.4 µs | 58.8 µs | 75.3 µs |
| 2 | relaxed | 4 | 100 | spike | 1470 | 24.8 µs | 29.3 µs | 30.4 µs | 38.6 µs | 49.3 µs |
| 1 | group | 0 | 1 | base | 20000 | 281.9 µs | 442.3 µs | 499.6 µs | 687.2 µs | 831.2 µs |
| 1 | group | 0 | 1 | spike | 20000 | 289.0 µs | 443.1 µs | 497.8 µs | 691.2 µs | 834.3 µs |
| 1 | group | 1 | 1 | base | 20000 | 278.0 µs | 450.0 µs | 513.6 µs | 724.1 µs | 871.2 µs |
| 1 | group | 1 | 1 | spike | 20000 | 277.5 µs | 432.7 µs | 491.0 µs | 691.4 µs | 835.4 µs |
| 1 | group | 4 | 1 | base | 20000 | 274.9 µs | 440.3 µs | 499.5 µs | 701.5 µs | 829.1 µs |
| 1 | group | 4 | 1 | spike | 20000 | 278.3 µs | 441.7 µs | 503.1 µs | 706.2 µs | 868.5 µs |
| 1 | group | 0 | 10 | base | 5000 | 333.0 µs | 485.1 µs | 542.3 µs | 741.0 µs | 872.4 µs |
| 1 | group | 0 | 10 | spike | 5000 | 339.7 µs | 503.1 µs | 565.7 µs | 778.6 µs | 957.8 µs |
| 1 | group | 1 | 10 | base | 5000 | 348.1 µs | 510.1 µs | 576.9 µs | 792.5 µs | 921.6 µs |
| 1 | group | 1 | 10 | spike | 5000 | 359.2 µs | 501.9 µs | 563.9 µs | 761.4 µs | 892.1 µs |
| 1 | group | 4 | 10 | base | 5000 | 398.0 µs | 576.7 µs | 644.3 µs | 869.7 µs | 1018.5 µs |
| 1 | group | 4 | 10 | spike | 5000 | 371.4 µs | 555.6 µs | 620.5 µs | 856.1 µs | 1037.7 µs |
| 1 | group | 0 | 100 | base | 585 | 676.7 µs | 869.2 µs | 938.7 µs | 1151.4 µs | 1330.1 µs |
| 1 | group | 0 | 100 | spike | 585 | 639.7 µs | 867.1 µs | 938.5 µs | 1138.4 µs | 1313.3 µs |
| 1 | group | 1 | 100 | base | 585 | 741.7 µs | 949.6 µs | 1009.0 µs | 1253.8 µs | 5076.1 µs |
| 1 | group | 1 | 100 | spike | 585 | 784.2 µs | 942.7 µs | 1009.8 µs | 1221.6 µs | 1960.3 µs |
| 1 | group | 4 | 100 | base | 585 | 687.9 µs | 1099.3 µs | 1187.3 µs | 1441.6 µs | 2375.8 µs |
| 1 | group | 4 | 100 | spike | 585 | 872.7 µs | 1083.0 µs | 1166.7 µs | 1468.7 µs | 33994.4 µs |
| 2 | group | 0 | 1 | base | 20000 | 319.1 µs | 486.5 µs | 547.9 µs | 741.4 µs | 866.0 µs |
| 2 | group | 0 | 1 | spike | 20000 | 295.4 µs | 492.5 µs | 557.0 µs | 769.1 µs | 902.1 µs |
| 2 | group | 1 | 1 | base | 20000 | 306.1 µs | 467.6 µs | 530.1 µs | 744.1 µs | 887.8 µs |
| 2 | group | 1 | 1 | spike | 20000 | 305.1 µs | 486.9 µs | 553.4 µs | 774.3 µs | 924.6 µs |
| 2 | group | 4 | 1 | base | 20000 | 319.9 µs | 506.3 µs | 574.7 µs | 799.0 µs | 961.2 µs |
| 2 | group | 4 | 1 | spike | 20000 | 307.7 µs | 483.7 µs | 548.2 µs | 770.4 µs | 939.1 µs |
| 2 | group | 0 | 10 | base | 5000 | 374.8 µs | 573.4 µs | 644.1 µs | 870.1 µs | 1008.3 µs |
| 2 | group | 0 | 10 | spike | 5000 | 347.9 µs | 583.5 µs | 654.3 µs | 898.1 µs | 1121.7 µs |
| 2 | group | 1 | 10 | base | 5000 | 399.7 µs | 574.3 µs | 637.2 µs | 844.7 µs | 982.5 µs |
| 2 | group | 1 | 10 | spike | 5000 | 420.7 µs | 591.1 µs | 660.7 µs | 922.0 µs | 1134.5 µs |
| 2 | group | 4 | 10 | base | 5000 | 437.2 µs | 635.8 µs | 713.2 µs | 953.8 µs | 1092.8 µs |
| 2 | group | 4 | 10 | spike | 5000 | 425.2 µs | 608.7 µs | 684.4 µs | 938.5 µs | 1178.2 µs |
| 2 | group | 0 | 100 | base | 585 | 768.4 µs | 984.2 µs | 1055.8 µs | 1247.4 µs | 1388.2 µs |
| 2 | group | 0 | 100 | spike | 585 | 594.2 µs | 947.9 µs | 1030.0 µs | 1277.2 µs | 1521.0 µs |
| 2 | group | 1 | 100 | base | 585 | 906.3 µs | 1064.7 µs | 1140.4 µs | 1374.4 µs | 15031.4 µs |
| 2 | group | 1 | 100 | spike | 585 | 634.1 µs | 985.7 µs | 1080.0 µs | 1295.5 µs | 1476.9 µs |
| 2 | group | 4 | 100 | base | 585 | 655.8 µs | 1174.4 µs | 1260.6 µs | 1585.1 µs | 4475.9 µs |
| 2 | group | 4 | 100 | spike | 585 | 638.2 µs | 1065.1 µs | 1195.7 µs | 1519.6 µs | 117523.1 µs |

The round-trip floor, the host load before and after each configuration, and the `SHOW META` ping (a heavy `sys.meta` read of about 0.5 ms, not a bare round trip):

| cores | durability | side | `SHOW META` round trip p50 | p99 | load average before / after |
|---|---|---|---|---|---|
| 1 | relaxed | base | 484.5 µs | 723.9 µs | ['1.91', '3.33', '4.79'] / ['1.18', '2.61', '4.34'] |
| 1 | relaxed | spike | 501.7 µs | 707.0 µs | ['1.91', '3.33', '4.79'] / ['1.18', '2.61', '4.34'] |
| 2 | relaxed | base | 542.4 µs | 743.4 µs | ['1.15', '2.55', '4.31'] / ['4.74', '3.21', '4.31'] |
| 2 | relaxed | spike | 566.2 µs | 759.9 µs | ['1.15', '2.55', '4.31'] / ['4.74', '3.21', '4.31'] |
| 1 | group | base | 478.6 µs | 709.8 µs | ['4.60', '3.20', '4.30'] / ['1.42', '2.32', '3.78'] |
| 1 | group | spike | 546.0 µs | 775.4 µs | ['4.60', '3.20', '4.30'] / ['1.42', '2.32', '3.78'] |
| 2 | group | base | 559.7 µs | 750.8 µs | ['1.47', '2.31', '3.77'] / ['1.03', '1.75', '3.29'] |
| 2 | group | spike | 555.5 µs | 758.0 µs | ['1.47', '2.31', '3.77'] / ['1.03', '1.75', '3.29'] |

## What this teaches about the engine

- **H3 holds in sign and is small.** The settle costs something measurable in the statement latency of a commit (up to 16 µs at 400 reservations, 0.3 to 1.5 µs at one INSERT), and it is a fraction of one percent of the transaction's wall time on this host. It is not a cost a single-row commit pays visibly. The operator's CK-S0 verdict is therefore not "the settle is expensive": whatever case exists for removing it is H2's (one failure point fewer), not H3's.
- **The bill is per reservation, not per commit.** One assertion at 100 INSERTs costs about a third of four assertions at 100 INSERTs, which is the ratio of reservations, not of pages. Entries appended at the chain's tail share a page (`assertion.md` AS6b, work order §1.7), so a page-count term should have stayed flat across the assertion count; the data is consistent with that and with per-reservation work dominating, though not a proof.
- **Under `group` the settle is hidden behind the sync.** A commit waits about 500 µs for the log sync, and the settle's record is appended before the commit record, so it rides the same sync. Nothing in this run shows it adding a sync; it adds at most the tens of microseconds the noise already covers.
- **The CPU meter at the work order's chosen granularity cannot see this.** A settle of 15 µs inside a transaction of 2,000 to 3,000 µs of CPU is well under the 10 ms tick resolution at K = 100, and the host's slow windows swamp the rest. Pricing a per-commit step of a few microseconds by server CPU needs either many more windows or a counter inside the engine; the commit-latency percentiles did it here only because the round trip is short and tight (p25 within a few percent of p50).

## What was cut, and what could not be measured

| Item | Status |
|---|---|
| Preload sizes 200 and 10,000 | Not run. Only 1,000 rows were preloaded (rule 9 asks for three sizes). The settle works on the pending list and the entry-chain tail, not on the base rows, so a row-count dependence is not expected, but that is an expectation and was not measured |
| Rounds | 5 measured plus 1 warm-up per configuration; more rounds would tighten the CPU spread and were cut for the 20-minute stage budget |
| Per-round commit percentiles | Not kept; the pooled distribution only |
| A count of `ASSERT_COMMIT` records or pages touched on the base binary | Not read. The base's settle was not confirmed by a record counter; the deltas above are the evidence that it does something |
| Spike correctness | Not part of this file. The spike was not run against the suite here (the BASE tree's `src/` is unchanged, so no suite was owed for it); CK-S0 part 2 owns that |
| PostgreSQL | Not part of this comparison |

## What made the numbers unreliable

- About one window in five ran at roughly twice the CPU and wall time, in both binaries and in the control. Cause unidentified (the host is a virtual machine; the driver and the servers were pinned to disjoint CPUs and no build was running).
- The 10 ms CPU tick, above.
- The load average at the start of `c1-group` was 4.6, the tail of the previous configuration's own work (two servers and a driver), not a competing build; `pgrep` found no `cc1plus`, `cmake --build` or `ctest` at the start of any configuration. No orphaned run: the status file was polled through the run and each configuration ended with return code 0.
- Group-durability percentiles at p95 and p99 carry single large outliers (for example a 117,523 µs p99 for one `group` cell at K = 100 on the spike), which are why p99 is not used to read any delta.

## Commands

```
export TMPDIR=/home/louis/ck-tmp            # /tmp is a nearly-full tmpfs
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release      # in each tree
cmake --build build-release --target kds_server -j4         # in each tree
cp build-release/kds_server /home/louis/ck-tmp/run-ck-s0/kds_server.{base,spike}
# per configuration, e.g. cores 1, relaxed (group used --window-stmts 12000; cores 2 used --reactor-cpus 0,1):
taskset -c 2,3 python3 -I tools/assertion_settle_benchmark.py \
    --base-bin /home/louis/ck-tmp/run-ck-s0/kds_server.base \
    --spike-bin /home/louis/ck-tmp/run-ck-s0/kds_server.spike \
    --workdir /home/louis/ck-tmp/run-ck-s0/w-c1-relaxed --cores 1 --reactor-cpus 0 \
    --durability relaxed --window-stmts 30000 --rounds 5 --json /home/louis/ck-tmp/run-ck-s0/c1-relaxed.json
```

The raw JSON of the four configurations is left in `/home/louis/ck-tmp/run-ck-s0/` and is not archived under `bench/` (a narrower measurement archives nothing, `bench/README.md`).
