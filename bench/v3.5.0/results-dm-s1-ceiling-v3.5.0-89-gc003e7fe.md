# DM-S1 ceiling: a sibling memo returns nothing on a warm pool, and on a cold one about 6 % of CPU only where its reads reach the device, a regime the order does not define

**Thesis.** These results are from the spike `worktree-dm-s0-spike` at
`c003e7fe` (`v3.5.0-89-gc003e7fe`, never landed), which is DM-S0's counters
plus U1. U1 is a per-thread memo of a right sibling's low key, keyed by the
link and the owner oid, and `DM_MEMO_ENTRIES` sizes it in one binary.

- **Warm pool.** The memo hits 93-99.7 % of coverage checks and returns no
  CPU worth reading. Two cells move outside the other arm's range, but both
  are under the 3 % bar and opposite in sign: the one-core update −0.45 µs
  (−1.6 %), the four-core insert +0.41 µs (+0.9 %). The fetch it removes
  cost 0.12-0.29 µs a statement (DM-S0, M2).
- **Cold pool, page-cache regime.** 1,000,000 rows on 2,048 frames, the
  101 MB file resident in the OS page cache, so a pool miss is a 3.5 µs
  copy.
  - At DM-Q7 (a)'s 1,024 entries the memo hits 32 % of the indexed inserts'
    statements and 11 % of the point updates', and CPU moves −1.5 % and
    +0.8 %.
  - At 16,384 entries the indexed insert's CPU falls 5.7 % (spreads
    disjoint), and throughput over a fixed 6 s window rises 4.2 %.
  - At 16,384 entries the point update's CPU does not move, its reader's
    p99 rises 45 %, and EV8's pool refusals rise. The base engine refuses
    too (§3).
- **Cold pool, real-device regime.** The same cells run under a 64 MiB
  `memory.high` cap, so pool misses reach the device. A sibling fetch that
  misses then takes 130-320 µs of wall time, throttling included (§2b).
  - The 1,024 memo lowers the indexed insert's CPU per statement in all 6
    alternated cells, by 2.4-16.0 % (median 5.9 %, arm means −7.7 %, a 95 %
    interval of −2.3 % to −12.9 %).
  - Throughput shows no consistent sign (±33 % between lifetimes). The cap
    throttles every cell, and its page-cache reclaim is part of the
    server's CPU.

**V1 (DM-Q2 (a)) fails at DM-Q7 (a)'s size on the cells the order defines.**

- **Clause 1** is 3 % of server CPU "in that cell" - the indexed-insert cell
  - "at any core count".
  - The order defines that cell twice: warm (M1), and as M3, "the
    indexed-insert cell on a table three times the pool (DB's regime)". DB's
    regime is this host's page-cache one.
  - The cell returns 0 % warm at `cores` 1 and 4, and −1.5 % in M3's
    regime.
- **Clause 2** asks that the memo remove M3's reads with the other
  sessions' p99 moving outside its spread.
  - At 1,024 entries the memo removes 20 % and 7 % of them in the page-cache
    regime, and 19.7 % and 6.1 % in the real-device one.
  - The reader's p99 stays inside its spread in the page-cache regime. In
    the real-device one it moves only upward: the update's mean is 6.98 ms
    against 5.99-6.73 ms, and one insert lifetime reads 56 ms. So it is not
    met.
- **The real-device regime is a cell the order does not define**, and in it
  clause 1 is met at modest confidence.
  - **Whether it counts is DM-Q10.** The order carried no proposal for it, so
    it is added to its items table. CLA proposes (a), not counted (§4), and
    (a) is adopted on the operator's standing go-ahead. It is the
    operator's to reverse.
  - **DM-S0's §4 had read clause 1 as reachable by the cold cell.** That
    reading is kept: the cold cell it meant is M3's, and M3's own regime
    returns −1.5 %.

| field | value |
|---|---|
| date executed | 2026-11-22: the arms 13:31:32-13:37:57 UTC; the update check 13:39-13:40 UTC; the real-device cells 14:03-14:07 UTC |
| branch / commit | `worktree-dm-s0-spike` at `c003e7fe`, `git describe --tags` = `v3.5.0-89-gc003e7fe` |
| work order | `instructions/v3.2.0/workorder-dm-leaf-upper-bound.md` §5, DM-S1 |
| build | Release, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, from `git archive c003e7fe` |
| binary | a copy at `/home/louis/dm-s0-run/kds_server_s1`, sha256 `5667019d0073558c66930e74031d818f8a649eade5b7a03e49ef774231d15db1`; one binary for every arm |
| device | `/dev/nvme0n1p1`, ext4; the data file opened without `O_DIRECT` (`file_page_device.hpp:36`) |
| server | `durability = relaxed`, reactors on CPUs 4.. |
| warm cells | `buffer_pool_frames = 131072`, 50,000 rows |
| cold cells | `buffer_pool_frames = 2048`, 1,000,000 rows (a 101 MB file, about 12,350 pages, 6 times the pool, at least 9,000 clustered leaves at 111 rows a leaf), the loaded directory copied back before every lifetime |
| real-device cells | the copy synced and dropped from the page cache (`posix_fadvise(DONTNEED)`), the server run under `systemd-run --user --scope -p MemoryHigh=64M -p MemorySwapMax=0`; server RSS (`VmRSS`) 38.8-41.2 MiB, so at most about 23-25 MiB of page cache for the 101 MB (96.5 MiB) file |
| ports | 15771, 15781 |
| arms | `DM_MEMO_ENTRIES` = 0 (today's path), 1,024 (DM-Q7 (a)), 16,384; alternated within each lifetime, 6 s a cell. Lifetimes: three for the page-cache cells, two for the real-device cells, then four more of 0 and 1,024 on the real-device insert |
| drivers | `tools/dm_s1_spike.py`, `tools/dm_s1_check.py`, on `tools/dm_s0_premise.py` |
| drivers amended | after the page-cache runs, by the stage's review (a server that failed to start is stopped, a killed one reaped, a host state taken before each cell, an issue window `window_s` timed apart from start-up and join), and again for the real-device cells (`--mem-max`, `--cold-base`, `--shapes`). The page-cache JSON predates both changes |
| host | quiet: load 0.90-3.04, no build or suite process before or after any run |
| prior number | DM-S0's base cells (`results-dm-s0-premise-v3.5.0-88-g1b3a4583.md`), whose spike is this one without the memo. Its per-statement CPU agrees with this file's arm 0 within 3.4 % (the four-core update: 46.99 against 45.45 µs; the cold insert: 55.98 against 57.83 µs). That gap between sessions is the size of V1's 3 % bar, which is why every comparison here is between arms alternated within one session |

## 1. Warm (a pool that holds the relation)

The cells are closed loop, four sessions a core. Each value is the mean of
three lifetimes, with the range across them in brackets. "Memo hits /
statement" divides hits by successful statements.

| cores | shape | arm | server CPU / statement | throughput | memo hits / statement | sibling fetches / statement | p0 | p25 | p50 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | indexed insert | off | 29.95 µs [29.36-30.65] | 24,080 /s | - | 0.997 | 38.2 µs | 86.2 µs | 122.6 µs | 523.6 µs | 1,671.4 µs |
| 1 | indexed insert | 1,024 | 29.93 µs [29.83-30.07] | 24,018 /s | 0.927 | 0.069 | 38.5 µs | 86.4 µs | 122.9 µs | 521.7 µs | 1,714.2 µs |
| 1 | indexed insert | 16,384 | 29.76 µs [29.64-29.89] | 24,229 /s | 0.994 | 0.003 | 38.2 µs | 86.9 µs | 122.4 µs | 506.9 µs | 1,662.2 µs |
| 1 | point update | off | 27.61 µs [27.25-28.32] | 27,265 /s | - | 1.000 | 36.3 µs | 76.9 µs | 110.3 µs | 523.0 µs | 1,655.7 µs |
| 1 | point update | 1,024 | 27.16 µs [27.07-27.24] | 27,018 /s | 0.997 | 0.003 | 36.1 µs | 77.5 µs | 110.5 µs | 496.2 µs | 1,591.9 µs |
| 1 | point update | 16,384 | 27.18 µs [26.97-27.35] | 27,139 /s | 0.997 | 0.003 | 36.2 µs | 78.0 µs | 111.4 µs | 487.4 µs | 1,543.6 µs |
| 4 | indexed insert | off | 47.38 µs [46.93-47.69] | 20,216 /s | - | 0.997 | 48.2 µs | 336.4 µs | 606.4 µs | 2,757.6 µs | 4,050.3 µs |
| 4 | indexed insert | 1,024 | 47.79 µs [47.46-48.02] | 20,273 /s | 0.928 | 0.069 | 59.9 µs | 356.7 µs | 616.3 µs | 2,762.5 µs | 4,073.5 µs |
| 4 | indexed insert | 16,384 | 47.61 µs [46.59-48.77] | 20,214 /s | 0.981 | 0.016 | 54.0 µs | 350.7 µs | 612.2 µs | 2,745.9 µs | 4,067.4 µs |
| 4 | point update | off | 45.45 µs [44.66-46.28] | 21,735 /s | - | 1.000 | 47.4 µs | 326.8 µs | 570.5 µs | 2,556.0 µs | 3,703.5 µs |
| 4 | point update | 1,024 | 45.53 µs [45.45-45.69] | 21,865 /s | 0.986 | 0.014 | 59.1 µs | 337.9 µs | 574.7 µs | 2,545.6 µs | 3,737.4 µs |
| 4 | point update | 16,384 | 45.91 µs [45.45-46.23] | 21,841 /s | 0.986 | 0.014 | 54.6 µs | 337.7 µs | 574.5 µs | 2,553.5 µs | 3,730.7 µs |

**What the warm cells show.**

- **Two cells move, both under the bar and in opposite directions.**
  - The one-core update at 1,024 entries is −0.45 µs (−1.6 %), its range
    disjoint from the base's.
  - The four-core insert at 1,024 entries is +0.41 µs (+0.9 %), each mean
    outside the other's range.
  - The ranges span 0.17-2.18 µs. The fetch the memo removes cost
    0.12-0.29 µs (DM-S0, M2). The 3 % bar is 0.8-1.4 µs, and no warm cell
    reaches it.
- **The four-core cells are bound by the Python client, not the server.**
  Server CPU is 5.7-6.1 s per 6 s window, about one reactor of four, and
  throughput is lower than at one core. Clause 1's "at any core count" was
  therefore not tested under a loaded four-core server. The warm fetch's
  0.27-0.29 µs bounds what such a cell could return, at about 0.6 %.

## 2. Cold, page-cache regime (1,000,000 rows on 2,048 frames, one point reader beside four writers)

Per-statement columns divide by the writers' successful statements. Server
CPU and pool misses also include the reader's statements, about 18,000 a
lifetime. `pool_misses` counts page creations too. Throughput is statements
over `seconds`, which included thread start-up and join: arm 0's windows
ran 6.17-6.22 s against the 16,384 arm's 6.00-6.01 s. Over a fixed 6 s, the
16,384 arm's gain is +4.2 % on the insert and +4.7 % on the update.

| shape | arm | server CPU / statement | throughput | memo hits / statement | sibling misses / statement | pool misses / statement | writers p0 | p25 | p50 | p99 | p99.9 | reader p50 | reader p99 | refused |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| indexed insert | off | 57.83 µs [57.37-58.27] | 10,862 /s | - | 0.487 | 1.246 | 49.7 µs | 138.0 µs | 201.2 µs | 869.0 µs | 17,033.9 µs | 195.6 µs | 725.0 µs [706.2-759.4] | 0 |
| indexed insert | 1,024 | 56.99 µs [56.69-57.26] | 11,234 /s | 0.316 | 0.387 | 1.152 | 54.4 µs | 137.7 µs | 200.2 µs | 849.8 µs | 16,516.3 µs | 193.5 µs | 709.0 µs [689.6-721.9] | 0 |
| indexed insert | 16,384 | 54.55 µs [54.11-55.11] | 11,553 /s | 0.949 | 0.031 | 0.811 | 45.7 µs | 133.3 µs | 197.8 µs | 847.2 µs | 16,475.4 µs | 193.2 µs | 727.1 µs [715.3-738.1] | 0 |
| point update | off | 59.17 µs [58.51-60.20] | 6,556 /s | - | 0.790 | 1.755 | 55.5 µs | 140.3 µs | 195.3 µs | 761.9 µs | 118,434.9 µs | 187.1 µs | 622.1 µs [606.8-639.7] | 0 |
| point update | 1,024 | 59.67 µs [59.05-59.99] | 6,553 /s | 0.112 | 0.733 | 1.692 | 55.2 µs | 140.9 µs | 196.7 µs | 787.3 µs | 90,075.1 µs | 186.3 µs | 615.2 µs [593.5-635.7] | 0 |
| point update | 16,384 | 59.85 µs [58.85-60.54] | 6,743 /s | 0.794 | 0.175 | 1.158 | 50.1 µs | 135.1 µs | 193.2 µs | 749.2 µs | 58,237.0 µs | 183.8 µs | 905.0 µs [840.8-966.4] | 817 |

**What the page-cache cells show.**

- **The memo's reach is its size against the trees' leaves.** The cold
  relation has about 9,100 clustered leaves and 2,000-3,000 index leaves.
  - 1,024 direct-mapped entries remember 1,024 siblings. The update's
    11.2 % hits is about 1,024 / 9,100.
  - 16,384 entries cover about every page id the file has (12,350). The
    update's 79 % fits a first-visit model of about 40,000 checks over
    9,100 leaves from an empty memo, which gives 0.79.
  - Any fixed size has this ceiling. A table a hundred times the pool has
    hundreds of thousands of leaves.
- **The indexed insert gains CPU, and the excess is not attributed.**
  Removing 0.46 page-cache `pread`s a statement returns 3.3 µs. That is more
  than the 1.8 µs a statement DM-S0 measured inside the missing fetches.
  Two candidates are not separated:
  - the writeback of the dirty victims those reads evicted, outside the
    fetch;
  - per-second background costs spread over more statements.
- **The update gains no CPU, and its reader loses p99, because the pool
  then refuses.** §3.

## 2b. Cold, real-device regime (the same cells under a 64 MiB `memory.high` cap)

**The cap makes a pool miss a device read.** RSS (`VmRSS`) was 38.8-41.2
MiB and the file 101 MB (96.5 MiB), so most of the file is not cached. The cost is that the cap
throttles every cell (339-803 `memory.high` events a cell, in every arm),
and its page-cache reclaim runs in the server's threads and counts in its
CPU.

Run 1 was two lifetimes of each arm and both shapes. Run 2 was four more
lifetimes of 0 and 1,024 on the indexed insert. "Missing fetch" is the wall
time of a sibling fetch that missed the pool. Throughput is over the issue
window (`window_s`).

| shape | arm | lifetimes | server CPU / statement | throughput | sibling misses / statement | pool misses / statement | missing fetch | reader p99 |
|---|---|---|---|---|---|---|---|---|
| indexed insert | off | 6 | 84.68 µs [78.78-96.31] | 3,693 /s [2,976-4,590] | 0.45 | 1.10 | 135-287 µs | 9.0-22.0 ms |
| indexed insert | 1,024 | 6 | 78.15 µs [70.17-89.39] | 3,531 /s [3,087-3,933] | 0.36 | 1.00 | 129-192 µs | 9.1-56.0 ms |
| indexed insert | 16,384 | 2 | 75.94 µs [75.32-76.55] | 3,720 /s | 0.09 | 0.74 | 295-317 µs | 9.4-10.1 ms |
| point update | off | 2 | 124.95 µs [121.15-128.76] | 1,540 /s | 0.81 | 1.62 | 276-290 µs | 6.0-6.7 ms |
| point update | 1,024 | 2 | 121.48 µs [120.25-122.72] | 1,669 /s | 0.76 | 1.57 | 248-259 µs | 6.6-7.4 ms |
| point update | 16,384 | 2 | 111.18 µs [110.95-111.40] | 1,754 /s | 0.51 | 1.31 | 277-278 µs | 5.6-5.7 ms |

**The alternated view.** Within each lifetime, the off cell ran first and
the 1,024 cell next. **Every cell ran on its own fresh copy of the loaded
directory**, dropped from the page cache (`tools/dm_s1_spike.py` copies per
cell), so the two cells of a row are neighbours in time and share no data.
For the indexed insert, 1,024 against off:

| pair | CPU off | CPU 1,024 | CPU change | throughput change | pool misses off → 1,024 |
|---|---|---|---|---|---|
| run 1, life 0 | 80.68 µs | 76.93 µs | −4.6 % | +15.3 % | 1.087 → 0.988 |
| run 1, life 1 | 90.25 µs | 75.78 µs | −16.0 % | −0.1 % | 1.092 → 0.986 |
| run 2, life 0 | 79.74 µs | 77.81 µs | −2.4 % | −0.1 % | 1.086 → 1.008 |
| run 2, life 1 | 78.78 µs | 70.17 µs | −10.9 % | −32.7 % | 1.090 → 0.986 |
| run 2, life 2 | 96.31 µs | 89.39 µs | −7.2 % | +32.2 % | 1.146 → 1.023 |
| run 2, life 3 | 82.33 µs | 78.79 µs | −4.3 % | −21.6 % | 1.112 → 1.002 |

**What the real-device cells show.**

- **CPU per statement falls with the memo in all 6 rows**, a median of
  −5.9 %, while the pool's misses fall 7-11 %.
  - **The statistics.** The rows are neighbours in time, not true pairs.
    The sign test gives p = 0.031. Unpaired, 6 against 6, Mann-Whitney U = 5
    gives p ≈ 0.041. Pairing each 1,024 cell with the next off cell instead
    leaves one row where the memo costs more (+8.6 %).
  - **What an avoided miss saves is not attributed.** The CPU removed per
    avoided pool miss runs from 25 to 138 µs across the rows. In one row
    (run 1, life 1) the CPU removed, 14.5 µs a statement, exceeds the wall
    time removed inside missing fetches (11.2 µs), so part of the saving
    sits outside the fetch.
  - No user/system split was recorded. Such a split would test whether the
    cap's page-cache reclaim is the excess; on a host whose RAM holds the
    page cache, kswapd and not the server would do most of that reclaim.
- **Throughput is not resolved.** It swings ±33 % between lifetimes with no
  sign; the throttle dominates it. The reader's p99 runs 9-56 ms, and moves
  only upward at 1,024 entries.
- **At 16,384 entries CPU falls further**: the update by −11.0 % (110.95-
  111.40 against 121.15-128.76 µs, disjoint) and the insert by −10.3 %, in 2
  lifetimes. The update's reader p99 is 5.6-5.7 ms against 6.0-6.7 ms, on
  about 2,500 reads a lifetime: each p99 is about the 25th-largest sample.
  At that size the memo hits only 41 %. A first-visit model over 9,100
  leaves predicts the measured fills within 0.3 %.
- No real-device cell refused a statement or returned an error.
- **What this does to V1**: it adds a cell the order does not define.
  Whether it counts is DM-Q10 (the thesis, §4).

## 3. The refusals in the cold update (`tools/dm_s1_check.py`, page-cache regime)

The check was four writer sessions with no reader, 6 s a cell, run as two
lifetimes of each arm alternated (off, 16,384, off, 16,384) from the same
loaded directory. It counted `rows_affected` on every update, and the pool's
counters across the window. The archived `s1check.json` predates the
driver's amendment (a list of arms, no binary hash or host state); the
binary and the host are this file's.

| arm | updates (rows affected = 1) | rows affected ≠ 1 | refused, client | refused, `pool_refused` | pool misses | inline reclaim batches | frames reclaimed inline | reclaimed / batch | dirty frames queued for writeback |
|---|---|---|---|---|---|---|---|---|---|
| off | 40,177 | 0 | 220 | 220 | 64,494 | 10,143 | 55,019 | 5.4 | 35,482 |
| 16,384 | 39,416 | 0 | 578 | 596 | 40,593 | 23,288 | 31,384 | 1.35 | 34,215 |
| off | 39,020 | 0 | 143 | 143 | 62,374 | 6,891 | 53,429 | 7.8 | 32,712 |
| 16,384 | 40,392 | 0 | 536 | 557 | 41,268 | 21,795 | 31,653 | 1.45 | 36,103 |

- **No answer moved in the update cell.** Each of the 79,808 memo-arm
  updates that did not fail affected its one row. A memo that answered
  "covered" wrongly would have sent a write lookup to a leaf that does not
  hold the key, and it would have answered 0 rows. The insert cell, the
  only one with splits, has no answer check (§5).
- **The refusal is the pool's, and it is EV8 as built** (`eviction.md`
  §3.3). The base arm refuses too.
  - With 35 % fewer pool misses, the resident set is more of the updates'
    dirty leaves and less of the sibling reads' clean pages.
  - An inline reclaim batch then frees 1.35-1.45 frames where it freed
    5.4-7.8.
  - The built retry re-walks without yielding, so a reservation runs out of
    retries more often.

  The memo's arm refuses 2.6-3.9 times as often. This is recorded in
  `known-gaps.md` (Eviction) as a property of the pool that any change
  removing reads from a write path meets, and not as DM's defect.
- **Why the arms run said 0 for the base.** The point reader, faulting
  clean pages beside the writers, keeps the base below the refusal there.

## 4. What this decides, and what U2 would add

- **V1 at DM-Q7 (a), on the order's cells: U0.** Clause 1 returns 0 % warm
  and −1.5 % in M3's regime. Clause 2 is not met in either regime.
- **DM-Q10: does a cold cell in a regime the order does not define count
  toward V1?**
  - (a) No: V1 is read on M1 and on M3 in DB's regime. **CLA's proposal,
    adopted on the operator's standing go-ahead.**
  - (b) Yes: the real-device cell's median −5.9 % meets clause 1, and DM
    goes on to DM-S2 and DM-S3, U1 built at 1,024 entries.
  - **Why (a).**
    - The return is a property of this table's size against the memo's. At
      1,024 entries against about 3,000 index leaves the memo hits 32 %. A
      table ten times larger would hit about a tenth of that, so the return
      shrinks as 1,024 / leaves on exactly the tables whose data outgrows
      memory.
    - The cell's 95 % interval reaches below the bar (−2.3 %).
    - Its CPU includes the cap's reclaim, whose share is not measured.
    - Its throughput and tail are unresolved or worse.
    - The build puts a `[quiet-wrong]` surface (DM-R1) into every write
      descent.
  - **Trade-off of (a):** a measured return of about 6 % on a cold indexed
    insert of this size is left unbuilt.
- **What a different ruling would rest on.**
  - DM-Q10 (b) or DM-Q2 (b): the 1,024 memo's −5.9 % median CPU on the
    real-device insert (§2b), with throughput unresolved.
  - DM-Q7 (b), a memo sized to the hot trees' leaves.
    - It returns −5.7 % CPU on the page-cache insert, and −10 to −11 % on
      the real-device insert and update.
    - It costs about 1.5 MiB a thread: 16,384 × (24 + 72) bytes, the index
      entry padded for the census's 49-byte bound.
    - It raises EV8's refusals in the page-cache update.
  - Every one of these rulings builds the `[quiet-wrong]` surface above.
- **What U2 would add on these cells** (stated, not measured). A bound on
  the page is a memo with a 100 % hit rate at any table size and no memory.
  - It removes every sibling read, 0.45-0.81 a statement, which is more
    than any memo arm removed.
  - It removes the range walk's extra leaf (DM-S0, M4; 1.00 a statement).
  - It meets the same pool cell (§3).
  - Its price is DM-R6's format event, which waits for the operator's word.

## 5. Cuts and budget

The page-cache arms ran 6 min 25 s, the check 1 min, the real-device probes
and cells about 4 min 30 s, and the spike's build about 4 min: about 16
minutes in all, inside DM-S1's 20.

- **Cut: `cores` above 4.** The four-core cells are bound by the client
  (§1).
- **Cut: the cold cells at `cores` > 1.**
- **Cut: the real-device regime without a cgroup.** It needs `O_DIRECT` or
  a host whose RAM cannot hold the file. The cap's throttle is a stated
  cost, not a removed one.
- **Cut: a separate arm for an index-only and a clustered-only memo.**
- **Cut: a debug-build correctness run.** The rows-affected count in §3
  stands for it in the update cell. The insert cell has no answer check,
  and the spike is not landed.

The JSON, the driver output and the spike's patch (`dm-spike.patch`, which
is `bf5fce58..c003e7fe` over `include/` and `src/`) are in
`bench/v3.5.0/archive/dm-s1-v3.5.0-89-gc003e7fe/`. The page-cache cells are
`s1.json` and `s1check.json`, and the real-device cells `s1real.json` and
`s1real2.json`.

To reproduce:

- the page-cache cells: `python3 -I tools/dm_s1_spike.py --bin <spike copy>
  --workdir <dir> --out <json>`;
- the check: `python3 -I tools/dm_s1_check.py --bin <spike copy> --base
  <workdir>/cold_base --workdir <dir> --out <json> --arms 0,16384,0,16384`;
- the real-device cells: add `--only cold --cold-base <workdir>/cold_base
  --mem-max 64M` to the first, and `--arms 0,1024 --lifetimes 4 --shapes
  ins_ix1` for run 2.
