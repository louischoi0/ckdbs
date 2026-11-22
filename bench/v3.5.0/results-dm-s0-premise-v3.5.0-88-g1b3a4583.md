# DM-S0 premise: every write off the rightmost leaf fetches the right sibling, and on a table six times the pool the sibling misses the pool in 49-79 % of write statements

**Thesis.** These counts and timings come from the census spike
`worktree-dm-s0-spike` at `1b3a4583` (`v3.5.0-88-g1b3a4583`). That spike is
the engine at `bf5fce58` plus eleven counters, and it was never landed.

- **Warm pool.** Every write that lands on a leaf other than the rightmost
  fetches that leaf's right sibling once per tree. This covers a secondary
  index entry's insert (0.99 fetches per index per statement) and a named
  key mid-chain (1.00). It also covers a point `UPDATE`, `DELETE` and `PURGE`
  by tuple key (1.00), which the order's table did not list.
- **Warm cost.** The fetch costs 0.12-0.15 µs of a 27-30 µs statement at
  one core, and 0.27-0.29 µs of 47-48 µs at four: 0.43-0.61 % of server CPU.
- **Cold pool.** The table is about six times the pool (a 101 MB file on
  2,048 frames). The sibling is not resident in 49 % of indexed inserts and
  79 % of point updates. Each such statement makes a `pread` of about
  3.5 µs under the leaf's exclusive hold, on the statement's reactor. These
  are 39 % and 45 % of all the pool's misses in those cells.
- **Those reads came from the OS page cache, not the device.** The data
  file is opened without `O_DIRECT` (`file_page_device.hpp:36`), and it was
  resident. 3.5 µs is a page-cache copy, and an NVMe read takes about 100
  µs. DM-S1 adds cells with the page cache capped, where the read is the
  device's.

**V0 (DM-Q2 (a)) passes.** Its first clause carries it alone: at least half
of the indexed inserts fetch, and 99.7 % do. Its second clause asks for a
sibling read from the device in at least 1 % of write statements under pool
pressure. It is met here by pool misses the page cache answered (49.4 % and
79.1 %), and by device reads only in DM-S1's capped cells.

| field | value |
|---|---|
| date executed | 2026-11-22, 13:27:13-13:28:20 UTC (run 2; run 1 lost its JSON to an M3 load failure, below) |
| branch / commit | `worktree-dm-s0-spike` at `1b3a4583`, `git describe --tags` = `v3.5.0-88-g1b3a4583` |
| work order | `instructions/v3.2.0/workorder-dm-leaf-upper-bound.md` §5, DM-S0 |
| build | Release, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, from `git archive 1b3a4583` |
| binary | a copy at `/home/louis/dm-s0-run/kds_server_spike`, sha256 `dd9467f485b5f404a28af0d4af66131e93b329c639e1a7d8a52fa0366bd536fe` |
| device | `/dev/nvme0n1p1`, ext4 (`df -T` at run time) |
| server | `durability = relaxed`, reactors on CPUs 4.., one fresh server per section; `buffer_pool_frames = 131072` for M1/M2, `2048` for M3; port 15751 |
| driver | `tools/dm_s0_premise.py` (written for this stage) |
| host | quiet: load 1.01 before, 2.05 after, no `cc1plus` either side. The driver was amended after the run by DM-S1's review: it now also records a host state before each cell, stops a server that failed to start, and reaps a killed one. The archived files are named `run1.*` and hold run 2's output |
| prior number | DK-S0 (`dk-s0-premise-v3.5.0-13-g187ff9b1.md`) counted sibling fetches for two shapes: a named key mid-chain 1.00 (here 1.00) and a secondary insert 0.74 and 0.088 across its two runs (here 0.99). No timing and no pool-miss count existed before this file |

**What the spike counts.**

- **Sibling fetches, by site.** Each coverage check that fetches the right
  sibling is counted where it runs: in the clustered tree's write descent,
  its read lookup's miss and its ordered seek; in the secondary tree's write
  descent, its seek and its batch remove.
- **Misses.** A fetch is counted as a miss when it reserved a frame on its
  own thread: a `pread` of the data file, which the OS page cache may answer (the fetch creates no page).
- **Time.** The nanoseconds of each fetch are measured by two
  `steady_clock` reads around `GetForRead`. This includes the clocks' own
  cost, so it is an upper bound.
- **The range walk.** Leaves a range walk reads past its range are counted.

## 1. M1: sibling fetches a statement (`cores = 1`, warm)

The relations hold 50,000 rows with even ids, in three tables: `t0` with no
index, `t1` with one on a random `int64` column, and `t4` with four. Each
shape ran 3,000 statements, and no statement failed.

| shape | clustered fetches | index fetches | range leaf past | time in the fetch |
|---|---|---|---|---|
| `INSERT`, issued key, no index | 0 | 0 | - | 0 µs |
| `INSERT`, issued key, 1 index | 0 | 0.99 (write) | - | 0.11 µs |
| `INSERT`, issued key, 4 indexes | 0 | 3.97 (write) | - | 0.40 µs |
| `INSERT`, named key mid-chain | 1.00 (write) | 0 | - | 0.15 µs |
| `UPDATE ... WHERE id = k` | 1.00 (write) | 0 | - | 0.11 µs |
| `DELETE ... WHERE id = k` | 1.00 (write) | 0 | - | 0.11 µs |
| `PURGE FROM ... WHERE id = k` | 1.00 (write) | 0 | - | 0.10 µs |
| tuple key read, hit | 0 | 0 | - | 0 µs |
| tuple key read, miss | 1.00 (lookup) | 0 | - | 0.10 µs |
| `id BETWEEN 1 AND k ORDER BY id DESC LIMIT 5` | 1.04 (seek) | 0 | - | 0.10 µs |
| `v1 BETWEEN 1 AND k ORDER BY v1 DESC LIMIT 5` | 0 | 1.00 (seek) | - | 0.11 µs |
| `id BETWEEN k AND k + 40` (M4) | 0 | 0 | 1.00 | - |

How to read it.

- **The order's §1.2 table is short by three rows.** A point `UPDATE` and a
  point `DELETE` take the write lookup
  (`CommandDispatcher::LocateByTupleKey`, `kWrite` by default,
  `src/server/command_dispatcher.cpp:9798`), and so does rollback's row
  relocation (`:9783`). The write lookup's descent asks coverage under its
  exclusive hold, so each of these pays the fetch everywhere but on the
  rightmost leaf. On a relation of any size, that is nearly every point
  write.
- **An issued-key insert with no index pays nothing**, as the order said.
- **An issued-key insert pays once per secondary index.** The index entry's
  key is random, so it lands mid-chain.
- **The two ordered seeks** answer the descending walks only. The ascending
  index walk seeks with no check (`IndexSeekLeaf`'s directional argument),
  and was measured at 0.
- **A shape needs both ends to reach the reverse walk.** `id <= k ORDER BY id
  DESC` compiles to a plain walk from the top, because the compiler gives a
  one-sided range no window (`TupleKeyRangeOf`, `src/exec/step_compiler.cpp`:
  "Both ends, never one"). That is a stated design, not a DM finding.
- **M4.** A range walk reads exactly one leaf past its range per statement
  when the range ends inside the relation. Whether that leaf missed the pool
  was not instrumented (cut, §5).

## 2. M2: what a statement spends in the fetch (warm, closed loop)

The relation `t1` holds 50,000 rows with one index. Each cell ran four
sessions a core for 8 s. Server CPU is `/proc` utime + stime over the window.
No sampling profiler exists on this host (`perf` is absent), so the
instrumented time stands in for M2's profile.

| cores | shape | statements | server CPU / statement | fetches / statement | time in fetch / statement | share of CPU | p0 | p25 | p50 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | indexed insert | 192,749 | 30.30 µs | 0.997 | 0.149 µs | 0.49 % | 39.0 µs | 84.7 µs | 120.8 µs | 526.7 µs | 1,609.5 µs |
| 1 | point update | 219,041 | 27.48 µs | 1.000 | 0.118 µs | 0.43 % | 31.2 µs | 79.4 µs | 112.3 µs | 469.0 µs | 1,543.7 µs |
| 4 | indexed insert | 162,622 | 47.84 µs | 0.997 | 0.294 µs | 0.61 % | 56.5 µs | 362.6 µs | 620.0 µs | 2,776.4 µs | 4,223.6 µs |
| 4 | point update | 172,582 | 46.99 µs | 1.000 | 0.270 µs | 0.57 % | 50.3 µs | 345.2 µs | 585.9 µs | 2,579.9 µs | 3,795.2 µs |

Latencies are client-side, per statement.

**The `cores = 4` cells are bound by the Python client, not the server.**
Server CPU is 7.78 s over 8.01 s, about one reactor of four, and throughput
is lower than at one core. At `cores = 4` the fetch costs twice what it
costs at one core. That latches shared with three other cores cause it is
unverified. One `cores = 4` update failed,
with a write-write conflict on a row two sessions drew. That is the workload
and not DM's.

## 3. M3: the cold sibling (`cores = 1`, 1,000,000 rows on 2,048 frames)

The data set is `t1` with one index: a 101 MB file, about 12,350 pages and
about 6 times the pool. A row is 73 bytes on its leaf (48 of payload, a
20-byte header and a 5-byte slot), so at most 111 rows fit a leaf and there
are at least 9,000 clustered leaves. It was loaded on 131,072 frames and served on 2,048 after a clean
restart. Each cell ran four writer sessions and one point-read session on
the same core for 8 s.

| shape | statements | sibling fetches / statement | sibling pool misses / statement | sibling misses of all pool misses | time in a missing fetch | share of CPU | server CPU / statement |
|---|---|---|---|---|---|---|---|
| indexed insert | 88,786 | 1.000 (index) | 0.494 | 39.2 % (43,864 of 112,030) | 3.59 µs | 3.35 % | 55.98 µs |
| point update | 48,621 | 1.000 (clustered) | 0.791 | 45.3 % (38,467 of 84,909) | 3.49 µs | 4.73 % | 59.44 µs |

| shape | sessions | p0 | p25 | p50 | p90 | p99 | p99.9 | p100 |
|---|---|---|---|---|---|---|---|---|
| indexed insert | writers | 50.3 µs | 136.8 µs | 200.5 µs | 400.3 µs | 821.3 µs | 16,372.0 µs | 311,140.4 µs |
| indexed insert | point reader | 59.7 µs | 138.8 µs | 195.1 µs | 370.2 µs | 703.4 µs | 16,111.4 µs | 310,674.1 µs |
| point update | writers | 55.2 µs | 139.6 µs | 194.5 µs | 378.1 µs | 826.0 µs | 169,671.9 µs | 587,055.6 µs |
| point update | point reader | 58.9 µs | 134.9 µs | 185.0 µs | 336.2 µs | 677.1 µs | 102,451.2 µs | 586,978.4 µs |

**The wait breakdown is the pool miss**, a `pread` the page cache answered. Of the time spent in the sibling
fetch, 94 % (indexed insert) and 98 % (point update) is in the fetches that
missed. The "share of CPU" column divides that time by server CPU. A missing
fetch blocks the reactor in `pread`, so its time is reactor time rather than
CPU time, and that column is the reactor's share and not a CPU profile. The
reader shares the writers' core, and its p99 sits within 18 % of theirs. The
per-statement columns divide by the writers' statements, but server CPU and
the pool's misses include the reader's statements too. `pool_misses` also
counts page creations.
Every read the sibling makes is a read the core's other sessions wait
behind. The p99.9 and p100 tails (16-170 ms, 0.3-0.6 s) are not
attributable from this run: the tallies carry no per-statement time, and a
checkpoint, an eviction batch and a sibling read all fit them.

## 4. What this decides

- **V0: go to DM-S1** (DM-Q2 (a), both clauses).
- **Where U1 can return anything.** Not warm CPU. The whole fetch is 0.4-0.6
  % of a warm statement, so U1's first V1 clause (3 % of server CPU in the
  indexed-insert cell) cannot be met warm by removing it. The cold cell can,
  as can the second V1 clause (M3's reads with the other sessions' p99):
  0.49-0.79 pool misses a write statement are the sibling's.
- **U1's reach under pressure is its size.** The cold table has about 9,100
  clustered leaves and 2,000-3,000 index leaves. A memo of 1,024 entries (DM-Q7 (a))
  remembers at most 1,024 siblings a core, so S1 measures 1,024 and a wider
  arm (16,384).

## 5. Cuts and budget

The two runs and the spike's build took about 9 minutes together, inside
DM-S0's 20.

- **Cut: the cold miss of M4's past-range leaf.** It was not instrumented,
  and the warm count is above.
- **Cut: `cores` above 4.**
- **Cut: a sampled profile (M2).** There is no profiler on this host; the
  instrumented fetch time stands in for it.
- **Cut: sessions waiting on the held leaf, measured apart (M3).** The point
  reader on the same core stands in for them.
- **Run 1 is not reported.** Its M1 and M2 completed with the same per-shape
  counts. Its M3 failed at `CREATE INDEX` on a 1M-row relation in a
  2,048-frame pool ("buffer pool full ... 2035 dirty"). The index build is
  one statement and cannot fit, so the driver now loads on a large pool and
  restarts on the small one. The run's JSON was not written, and the driver
  now saves after each section.

The JSON and the driver's output are in
`bench/v3.5.0/archive/dm-s0-v3.5.0-88-g1b3a4583/`.

To reproduce: `python3 -I tools/dm_s0_premise.py --bin <spike copy> --workdir
<dir on a block device> --out <json>`.
