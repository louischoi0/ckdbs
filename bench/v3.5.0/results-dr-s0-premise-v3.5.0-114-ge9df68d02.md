# DR-S0, DR-S0b and DR-S0c - the premise: what a primary key over an unchanged Keystone costs today

Measured 2026-11-23 between 05:19 and 05:32 UTC on worktree
`dq-cabin-miss-build`, branch `worktree-dr-primary-key`, at `e9df68d02`
(`v3.5.0-114-ge9df68d02`), the commit DR opened from. Order:
`instructions/v3.2.0/workorder-dr-primary-key-over-keystone.md` §5. Driver
`tools/dr_s0_premise.py`, tables and the first section's figures by
`tools/dr_s0_report.py`, raw output in
`bench/v3.5.0/archive/dr-s0-premise-v3.5.0-114-ge9df68d02/`. Nothing in the
engine was changed: every cell runs statements the engine answers today.

One file for the three stages: DR-S0 is M1 and M4, DR-S0b is M2, M3 and
M5, DR-S0c is M6 to M9. M1 to M4 share their cells (one relation a form,
loaded once), so DR-S0b's M2 and M3 came with DR-S0's run.

**This is the second run, and every figure here is its.** A first run
(04:46 to 05:00 UTC) was discarded on its `critics-developer` review: a
phase timed straight after a load was charged the writeback and
checkpoint the load left behind, the absent keys of M8 all sorted past
the loaded range, and one range of the `(int32, int32)` form could return
99 rows. The driver now waits for a quiet server after each load, draws
M8's absent keys through the loaded range, and every cell was run again
with it.

## The answer first

**The frame is sound for a read and unusable for a write.** On a heap
whose primary key is not its Keystone, 100,000 rows, server CPU a
statement:

| what | by the Keystone (`int64 PRIMARY KEY` heap) | by a primary key that is not the Keystone |
|---|---|---|
| a point read naming the whole key (M1) | 25.3 us | 25.9 us to 30.5 us: one index descent and one page fetch more |
| a range of 100 rows, both ends inclusive (M3) | 67.2 us | 90.0 us to 99.1 us, `IndexRange` |
| **an `UPDATE` naming the whole key (M4)** | **31.5 us** | **6,488 us to 9,554 us: the relation is walked, 206x to 304x** |
| an `UPDATE` that moves the key (M4) | refused (K2) | 6,808 us to 9,557 us: the same walk; the move itself is not visible beside it |
| a page, `key >= x ORDER BY key LIMIT 100` (M3) | 5,821 us, a scan and a sort | 6,730 us to 9,041 us, a scan and a sort; 178 us where an equality prefix stands before the range |

1. **A write by primary key walks the relation.** `UPDATE`, `DELETE` and
   `SELECT ... FOR UPDATE` descend only by the tuple key or a list on it;
   no index arm exists for a write (census, the order's §1.8). A primary
   key that is not the Keystone is therefore a key a row is read by and
   not a key it is written by. This is the cost that decides the frame, and
   it is a build, not a limit: the run of a unique key names the rows, and
   the write's point arm writes each (candidate P7).
2. **A read by primary key costs 0.6 us to 5.2 us over a read by
   Keystone** (2 % to 20 %), by key width and column count. An inclusive
   range is served by the index in every form, at 1.3x to 1.5x the
   declared-Keystone heap's.
3. **A page by key is a walk on every structure**, the btree's tuple key
   included (1,014 us there, a filtered scan from the head). It is the
   access ladder's gap, not the primary key's: a one-sided bound is no
   range to any arm.
4. **A primary key's index costs an insert 4.5 us to 5.8 us a row** over a
   heap with none (7.27 us to 11.74..13.10 us, +61 % to +80 %).
5. **Keys sharing their first 32 bytes are a quadratic hazard** (M5): a
   point read costs 25.9 us at a run of 1 and 1,044 us at a run of 4,096;
   a load costs 11.1 us and 2,726 us a row.

**For a btree (DR-S0c), the hashed home buys a read 1 us to 2 us and
costs an insert 2 us to 2.5 us.** With every page resident:

| what | F0, `int64` key | F2 emulated, hashed home | P1 emulated, issued key and the index |
|---|---|---|---|
| point read, present key, 100,000 rows, client p50 | 39.5 us | 42.1 us | 43.0 us |
| point read, present key, 1,000,000 rows, client p50 | 39.8 us | 42.2 us | 44.0 us |
| point read, present key, 100,000 rows, server CPU | 23.2 us | 25.7 us | 26.9 us |
| point read, present key, 1,000,000 rows, server CPU | 23.3 us | 30.9 us (27.2..31.6) | 27.6 us (26.9..31.6) |
| point read, absent key, 1,000,000 rows, client p50 | - | 41.1 us and 40.2 us, two statements | 40.2 us |
| load, 100,000 rows | 4.43 us/row | 9.90 us/row | 8.07 us/row |
| load, 1,000,000 rows | 4.65 us/row | 11.75 us/row | 9.28 us/row |
| leaf divisions of the relation, 1,000,000 rows | 0 | 18,646 | 0 |
| data file after the load, 1,000,000 rows | 108.0 MB | 218.1 MB | 169.9 MB |
| range of 100 rows, 1,000,000 rows | 47.2 us | 212.7 us | 218.2 us |
| a scan of the relation (the bare order, unserved), 1,000,000 rows | - | 1.46x P1's | 1.00x |

- **F2's point read is ahead of P1's**: 0.9 us at 100,000 rows and 1.8 us
  at 1,000,000 by the client's p50, 3 % and 6 % of P1's statement. At
  100,000 rows the server CPU agrees (1.2 us). **At 1,000,000 rows the
  server CPU does not measure the statement**: a checkpoint's writeback
  lands inside the read phase of one arm or the other from round to round
  (27.2 us to 31.6 us for F2, 26.9 us to 31.6 us for P1, while each arm's
  p50 moves by under 0.5 us), so the p50 is the figure there.
- **F2's insert is 23 % and 27 % dearer** (+1.83 us and +2.48 us a row),
  its data file 26 % and 28 % larger, its scans 1.35x to 1.46x P1's at
  1,000,000 rows, and an absent key costs it a second descent. Its log is
  within 2.3 % of P1's.
- **The break-even is 2.0 keyed reads of a row an insert at 100,000 rows
  and 1.4 at 1,000,000**: F2 pays for itself on a relation whose rows are
  each read by key more often than that, and loses on one that is written
  and ranged.

**DR-Q11 (a)'s first condition is met; its second is the bar DR-S1 sets**
(the order's §5a). The cells do not decide between the forms by
themselves: the read's gain is a few percent of a statement and the
insert's cost a quarter of one.

**The limit of every figure above**: every page was resident. On a
relation larger than the pool F2 would read one leaf where P1 reads two,
and would write two random leaves an insert where P1 writes one random
leaf and the right edge. That cell was not run.

## How it was measured

- Binary: `build-release/kds_server` rebuilt at `e9df68d02` (mtime
  04:41:22 UTC, after the commit's 04:37:37), copied to
  `/home/louis/dr-bench/s0/kds_server_e9df68d0`, sha256 `e33a55473f81fade`
  (first 16), every server started from the copy.
- Host: INTEL XEON PLATINUM 8581C, 8 CPUs (SMT pairs (0,4) (1,5) (2,6)
  (3,7)), Linux 6.12.116. Server on CPU 2, driver on CPUs 0 and 4. Data
  files on `/dev/nvme0n1p1`, ext4.
- `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`
  (1 GiB; the largest data file is 218 MB), one fresh server and data file
  a cell, the cells of a round in an order rotated per round. Ports 15881
  and 15882.
- Server CPU is `/proc/<pid>/task/*/schedstat` over the phase, read once
  no server thread is runnable, divided by the statements: the median of
  the rounds, with their range. Latency is the client's, around one
  statement: each percentile is the median of the rounds'. Steps are the
  statement's own `ANALYZE`, the first round's.
- After each load the driver waits until the server's CPU over 0.5 s is
  back within twice its idle baseline plus 1 ms (0.5 s to 2.0 s a cell).
  That keeps the load's own tail out of the next phase; it does not keep a
  later checkpoint out, which is what the 1,000,000-row CPU shows.
- Load: `cc1plus` 0 at every cell; no quiet-gate hold. The 1-minute load
  average was 0.84 to 2.09, this session's own driver and file edits.

| run | cells | started (UTC) | elapsed |
|---|---|---|---|
| heap (M1 to M4) | 7 forms x 3 rounds, 100,000 rows | 05:19:21 | 251.3 s |
| prefix (M5) | 4 run lengths x 3 rounds, 8,192 rows | 05:23:33 | 108.1 s |
| btree (M6 to M9) | 3 arms x 3 rounds, 100,000 rows | 05:25:21 | 120.4 s |
| btree (M6 to M9) | 3 arms x 3 rounds, 1,000,000 rows, 100 ranges a shape | 05:27:21 | 257.1 s |

**The budget** (20 minutes a stage, DR-Q9 (a)). This run: DR-S0 and its
share of DR-S0b 251 s, M5 108 s, DR-S0c 378 s. With the discarded first
run (232 s, 95 s, 274 s and two volume cells of 47 s and 52 s) DR-S0c's
measuring came to 12.5 minutes and no stage passed 20. **Cut to it**: M2's
cell at the largest clean `cores` and its two sessions racing for one key;
any cell with the relation larger than the pool; a per-batch CPU reading
that would have kept a checkpoint out of the 1,000,000-row read phases.

**Wait breakdown**: none taken. One session at `cores = 1` waits on
nothing but its own socket, and the figures are server CPU and client
latency.

**Delta against this engine's previous number**: none exists for these
shapes. `bench/v3.5.0/results-dn-close-v3.5.0-48-g9c061907.md` measured a
heap by its Keystone, not by a primary key beside it.

## DR-S0 and DR-S0b: a heap of 100,000 rows (M1 to M4)

The forms: `ref_btree` is `(id int64, v, w) BTREE` read by its tuple key;
`ref_heap` is `(id int64 PRIMARY KEY, v, w) HEAP`, the declared Keystone;
`nopk_heap` is a heap with a hidden Keystone and no primary key, loaded
only; the others are heaps with a hidden Keystone and the primary key
named: `int32` is `(a)`, `int32x2` is `(a, b)`, `varchar64` is one
`varchar(64)` whose 45-byte values differ inside their first 32 bytes,
`mixed4` is `(a int32, d date, n int64, s varchar(16))`. A primary key's
rows arrive shuffled, the reference forms' in key order. 20,000 point
reads, 1,000 ranges, 1,000 pages, 200 bare ordered limits and 500 updates
of each kind a cell. On both reference forms a move of the key is refused:
`tuple key column 'id' cannot be updated ...; it is the tuple's identity,
not a field of it`.

Run started 2026-11-23 05:19:21 UTC, 251.3 s; binary sha256 `e33a55473f81fade`; quiet-gate hold 0.0 s; 1-minute load 0.84 before and 2.09 after; the wait for a quiet server after a load 0.5 s to 0.5 s a cell.

M2, the load:

| form | server CPU a row, median (range) | rows a second |
|---|---|---|
| ref_btree | 3.51 us/row (3.48..3.59) | 274,895 rows/s |
| ref_heap | 7.48 us/row (7.44..7.69) | 131,358 rows/s |
| nopk_heap | 7.27 us/row (7.13..7.37) | 128,887 rows/s |
| int32 | 11.74 us/row (11.72..11.83) | 82,887 rows/s |
| int32x2 | 11.94 us/row (11.78..12.08) | 75,405 rows/s |
| varchar64 | 12.41 us/row (12.38..12.42) | 70,868 rows/s |
| mixed4 | 13.10 us/row (12.90..13.25) | 68,781 rows/s |

M1, a point read naming the whole key:

| form | server CPU a statement, median (range) | client latency p0 / p25 / p50 / p95 / p99 | ANALYZE step |
|---|---|---|---|
| ref_btree | 22.9 us (22.8..23.4) | 36 / 39 / 39 / 45 / 53 us | `Lookup t opens=1 examined=1 matched=1 sel=100% pages=1` |
| ref_heap | 25.3 us (23.9..25.7) | 37 / 40 / 41 / 50 / 61 us | `Lookup t opens=1 examined=1 matched=1 sel=100% pages=1` |
| int32 | 25.9 us (25.8..26.5) | 39 / 42 / 43 / 50 / 59 us | `IndexProbe t opens=1 examined=1 matched=1 sel=100% pages=2 index_scanned=1 index_resolved=1` |
| int32x2 | 27.3 us (27.2..27.4) | 40 / 43 / 44 / 51 / 62 us | `IndexProbe t opens=1 examined=1 matched=1 sel=100% pages=2 index_scanned=1 index_resolved=1` |
| varchar64 | 26.5 us (26.0..26.6) | 39 / 42 / 42 / 52 / 61 us | `IndexProbe t opens=1 examined=1 matched=1 sel=100% pages=2 index_scanned=1 index_resolved=1` |
| mixed4 | 30.5 us (30.4..30.7) | 43 / 46 / 47 / 55 / 67 us | `IndexProbe t opens=1 examined=1 matched=1 sel=100% pages=2 index_scanned=1 index_resolved=1` |

M3, a range of 100 rows bounded inclusively at both ends:

| form | server CPU a statement, median (range) | client latency p0 / p25 / p50 / p95 / p99 | ANALYZE step |
|---|---|---|---|
| ref_btree | 46.3 us (46.2..69.0) | 68 / 73 / 75 / 91 / 138 us | `Range t opens=1 examined=332 matched=100 sel=30% pages=3 range_stopped_early=1` |
| ref_heap | 67.2 us (66.2..67.4) | 90 / 93 / 94 / 107 / 131 us | `IndexRange t opens=1 examined=100 matched=100 sel=100% pages=101 index_scanned=100 index_resolved=100` |
| int32 | 90.0 us (88.2..92.3) | 106 / 115 / 118 / 143 / 204 us | `IndexRange t opens=1 examined=100 matched=100 sel=100% pages=101 index_scanned=100 index_resolved=100` |
| int32x2 | 96.8 us (96.0..99.2) | 113 / 120 / 123 / 147 / 193 us | `IndexRange t opens=1 examined=100 matched=100 sel=100% pages=101 index_scanned=100 index_resolved=100` |
| varchar64 | 99.1 us (94.0..99.6) | 104 / 121 / 124 / 166 / 221 us | `IndexRange t opens=1 examined=100 matched=100 sel=100% pages=101 index_scanned=100 index_resolved=100` |

M3, a page: `key >= x ORDER BY key LIMIT 100`:

| form | server CPU a statement, median (range) | client latency p0 / p25 / p50 / p95 / p99 | ANALYZE step |
|---|---|---|---|
| ref_btree | 1,013.9 us (990.6..1,043.7) | 106 / 641 / 1,084 / 1,963 / 2,144 us | `Scan t opens=1 examined=75565 matched=100 sel=0% pages=456` |
| ref_heap | 5,821.0 us (5,742.4..5,828.4) | 2,079 / 4,021 / 5,897 / 9,233 / 9,528 us | `Scan t opens=1 examined=100000 matched=98134 sel=98% pages=603` |
| int32 | 6,730.2 us (6,723.7..6,875.9) | 2,200 / 5,049 / 7,269 / 9,968 / 10,377 us | `Scan t opens=1 examined=100000 matched=25655 sel=25% pages=654` |
| int32x2 | 177.8 us (169.1..181.9) | 112 / 178 / 224 / 336 / 384 us | `IndexProbe t opens=1 examined=124 matched=100 sel=80% pages=127 index_scanned=233 index_resolved=124` |
| varchar64 | 9,041.0 us (9,027.6..9,083.1) | 2,606 / 6,299 / 9,588 / 13,956 / 14,880 us | `Scan t opens=1 examined=100000 matched=28030 sel=28% pages=1389` |

M3, the bare `ORDER BY key LIMIT 100`:

| form | server CPU a statement, median (range) | client latency p0 / p25 / p50 / p95 / p99 | ANALYZE step |
|---|---|---|---|
| ref_btree | 37.5 us (36.9..38.1) | 62 / 64 / 65 / 85 / 211 us | `Scan t opens=1 examined=100 matched=100 sel=100% pages=1` |
| ref_heap | 8,361.4 us (8,197.2..9,090.7) | 8,138 / 8,234 / 8,301 / 9,032 / 12,330 us | `Scan t opens=1 examined=100000 matched=100000 sel=100% pages=603` |
| int32 | 77.3 us (75.6..79.5) | 97 / 100 / 101 / 162 / 342 us | `IndexRange t opens=1 examined=100 matched=100 sel=100% pages=101 index_scanned=331 index_resolved=100` |
| int32x2 | 73.7 us (72.5..73.8) | 96 / 99 / 101 / 121 / 179 us | `IndexRange t opens=1 examined=100 matched=100 sel=100% pages=101 index_scanned=191 index_resolved=100` |
| varchar64 | 12,162.2 us (12,083.9..12,194.2) | 11,894 / 12,068 / 12,175 / 12,754 / 13,326 us | `Scan t opens=1 examined=100000 matched=100000 sel=100% pages=1389` |

M4, an `UPDATE` of a column no index covers, by the whole key:

| form | server CPU a statement, median (range) | client latency p0 / p25 / p50 / p95 / p99 |
|---|---|---|
| ref_btree | 24.9 us (24.7..27.1) | 37 / 39 / 40 / 48 / 77 us |
| ref_heap | 31.5 us (30.4..31.7) | 38 / 42 / 46 / 56 / 132 us |
| int32 | 6,488.2 us (6,483.8..6,539.1) | 6,354 / 6,482 / 6,532 / 6,771 / 7,113 us |
| int32x2 | 7,234.1 us (7,208.0..7,296.2) | 6,990 / 7,176 / 7,246 / 7,877 / 8,450 us |
| varchar64 | 7,690.2 us (7,671.1..7,919.7) | 7,411 / 7,620 / 7,729 / 8,011 / 8,604 us |
| mixed4 | 9,554.4 us (9,551.2..9,603.4) | 9,163 / 9,376 / 9,471 / 10,273 / 11,477 us |

M4, an `UPDATE` that moves the key's last column:

| form | server CPU a statement, median (range) | client latency p0 / p25 / p50 / p95 / p99 |
|---|---|---|
| int32 | 6,807.8 us (6,585.5..6,866.9) | 6,382 / 6,514 / 6,578 / 7,499 / 8,409 us |
| int32x2 | 7,248.3 us (7,228.5..7,253.2) | 7,012 / 7,171 / 7,239 / 7,934 / 8,666 us |
| varchar64 | 7,832.3 us (7,765.9..7,878.9) | 7,414 / 7,657 / 7,793 / 8,758 / 10,153 us |
| mixed4 | 9,556.5 us (9,481.5..9,761.1) | 9,159 / 9,373 / 9,457 / 10,498 / 12,869 us |

Three readings of M3:

- **The range is served in every form** (DR-R10): IX8's `IndexRange`, a
  descent a row.
- **The ordered walk (CV) holds for integer keys and not for a string
  key** (IX6: a truncated key keeps its sort), and the declared-Keystone
  heap sorts a scan for `ORDER BY id LIMIT`, since no walk of a heap is in
  key order and its Keystone index is not an ordered walk's choice.
- **The page is unserved everywhere** but behind an equality prefix.

**P3's price cannot be read off M4.** The move and the plain update cost
the same within their ranges because both walk 100,000 rows; the unique
check and the new entry are inside the noise of the walk. The price of a
move is measurable only once a write by key descends, at the close.

## DR-S0b: M5, string keys sharing their first 32 bytes

`varchar(64)` primary keys in runs sharing their first 32 bytes; 8,192
rows a cell, the load's rows shuffled, 4,000 point reads.

Run started 2026-11-23 05:23:33 UTC, 108.1 s; binary sha256 `e33a55473f81fade`; quiet-gate hold 0.0 s; 1-minute load 2.09 before and 1.22 after; the wait for a quiet server after a load 0.5 s to 0.5 s a cell.

| run length | load, server CPU a row | point read, server CPU a statement | read latency p0 / p25 / p50 / p95 / p99 | ANALYZE step |
|---|---|---|---|---|
| 1 | 11.10 us/row (10.93..11.16) | 25.9 us (25.4..26.5) | 39 / 41 / 42 / 52 / 64 us | `IndexProbe t opens=1 examined=1 matched=1 sel=100% pages=2 index_scanned=1 index_resolved=1` |
| 16 | 22.45 us/row (21.97..22.88) | 31.5 us (30.8..32.2) | 44 / 46 / 47 / 57 / 69 us | `IndexProbe t opens=1 examined=16 matched=1 sel=6% pages=17 index_scanned=16 index_resolved=16` |
| 256 | 187.24 us/row (185.03..187.66) | 101.2 us (100.7..103.6) | 108 / 114 / 117 / 139 / 176 us | `IndexProbe t opens=1 examined=256 matched=1 sel=0% pages=257 index_scanned=256 index_resolved=256` |
| 4096 | 2,725.63 us/row (2,722.78..2,748.94) | 1,044.2 us (1,029.0..1,065.0) | 995 / 1,071 / 1,100 / 1,182 / 1,340 us | `IndexProbe t opens=1 examined=4096 matched=1 sel=0% pages=4097 index_scanned=4096 index_resolved=4096` |

A read resolves every row of the run (about 0.25 us a row). An insert's
unique check resolves the run built so far, about 1.3 us a row of it,
which over a whole load is 0.66 us a row of the run's length. A true
duplicate was refused on the full value at every run length. The limit is
a constraint to state, not a defect: a string key should differ inside
its first 32 bytes.

## DR-S0c: a btree, three arms (M6 to M9)

`t (id int64, k varchar(64), v int64) BTREE`, the keys arriving shuffled.
**F0**: `id` is twice the arrival order, no index. **F2 emulated**: `id`
is the driver's 47-bit hash of `k` (BLAKE2b, folded into `[1, 2^47)`),
with `CREATE UNIQUE INDEX` on `k`; a read names `id` and `k`. **P1
emulated**: `id` is issued, the same unique index, read by `k` alone. No
hash collided at either size. 20,000 point reads of present keys and as
many of absent ones, spread through the loaded range and differing from
every key inside the indexed 32 bytes.

**M8 as an emulation.** F2's absent key is two statements here (the home,
then the index), each carrying a statement's fixed cost. In an engine
that built F2 it would be one statement and two descents, so its cost is
above P1's and below the sum; the cells bound it and do not price it.

### 100,000 rows

Run started 2026-11-23 05:25:21 UTC, 120.4 s; binary sha256 `e33a55473f81fade`; quiet-gate hold 0.0 s; 1-minute load 1.22 before and 1.56 after; the wait for a quiet server after a load 0.5 s to 0.5 s a cell.

M6, the load:

| arm | server CPU a row, median (range) | rows a second | leaf divisions (relation / index) | data file | log written |
|---|---|---|---|---|---|
| F0 | 4.43 us/row (4.40..4.49) | 133,901 rows/s | 0 / - | 12.1 MB | 69.5 MB |
| F2e | 9.90 us/row (9.52..9.97) | 86,288 rows/s | 1,859 / 738 | 23.1 MB | 94.0 MB |
| P1e | 8.07 us/row (7.86..8.11) | 95,985 rows/s | 0 / 738 | 18.4 MB | 95.8 MB |

M7 to M9:

| arm | shape | server CPU a statement, median (range) | client latency p0 / p25 / p50 / p95 / p99 | ANALYZE step |
|---|---|---|---|---|
| F0 | M7 present | 23.2 us (22.9..24.4) | 36 / 39 / 40 / 46 / 54 us | `Lookup t opens=1 examined=1 matched=1 sel=100% pages=1` |
| F0 | M8 absent | 22.6 us (22.3..23.5) | 34 / 38 / 38 / 44 / 52 us | `Lookup t opens=1 examined=0 matched=0 pages=1` |
| F0 | M9 range | 45.8 us (45.5..46.5) | 68 / 72 / 74 / 88 / 102 us | `Range t opens=1 examined=154 matched=100 sel=64% pages=3 range_stopped_early=1` |
| F0 | M9 page | 1,099.6 us (1,075.3..1,204.0) | 114 / 689 / 1,156 / 2,180 / 2,349 us | `Scan t opens=1 examined=39841 matched=100 sel=0% pages=518` |
| F0 | M9 bare | 36.6 us (35.8..36.8) | 62 / 64 / 64 / 78 / 110 us | `Scan t opens=1 examined=100 matched=100 sel=100% pages=2` |
| F2e | M7 present | 25.7 us (25.6..26.6) | 39 / 41 / 42 / 49 / 60 us | `Lookup t opens=1 examined=1 matched=1 sel=100% pages=1` |
| F2e | M8 absent, the home | 24.9 us (24.5..25.8) | 36 / 40 / 40 / 46 / 54 us | `Lookup t opens=1 examined=0 matched=0 pages=1` |
| F2e | M8 absent, the index | 25.2 us (25.1..25.5) | 36 / 39 / 40 / 47 / 60 us | `IndexProbe t opens=1 examined=0 matched=0 pages=1` |
| F2e | M9 range | 137.4 us (135.5..144.2) | 149 / 160 / 164 / 210 / 294 us | `IndexRange t opens=1 examined=100 matched=100 sel=100% pages=101 index_scanned=100 index_resolved=100` |
| F2e | M9 page | 9,880.8 us (9,748.2..10,000.4) | 3,379 / 7,077 / 10,544 / 14,702 / 15,318 us | `Scan t opens=1 examined=100000 matched=45749 sel=45% pages=1877` |
| F2e | M9 bare | 13,727.9 us (13,501.6..14,032.1) | 13,093 / 13,495 / 13,749 / 14,758 / 15,084 us | `Scan t opens=1 examined=100000 matched=100000 sel=100% pages=1877` |
| P1e | M7 present | 26.9 us (26.9..27.0) | 39 / 42 / 43 / 51 / 60 us | `IndexProbe t opens=1 examined=1 matched=1 sel=100% pages=2 index_scanned=1 index_resolved=1` |
| P1e | M8 absent | 24.7 us (23.7..25.0) | 36 / 39 / 40 / 49 / 55 us | `IndexProbe t opens=1 examined=0 matched=0 pages=1` |
| P1e | M9 range | 137.1 us (136.1..146.7) | 141 / 160 / 164 / 211 / 286 us | `IndexRange t opens=1 examined=100 matched=100 sel=100% pages=101 index_scanned=100 index_resolved=100` |
| P1e | M9 page | 8,957.0 us (8,758.5..9,107.8) | 2,515 / 6,205 / 9,468 / 13,875 / 14,487 us | `Scan t opens=1 examined=100000 matched=45749 sel=45% pages=1299` |
| P1e | M9 bare | 12,081.1 us (12,024.8..13,059.3) | 11,858 / 12,030 / 12,117 / 12,738 / 14,960 us | `Scan t opens=1 examined=100000 matched=100000 sel=100% pages=1299` |

### 1,000,000 rows

Run started 2026-11-23 05:27:21 UTC, 257.1 s; binary sha256 `e33a55473f81fade`; quiet-gate hold 0.0 s; 1-minute load 1.56 before and 1.46 after; the wait for a quiet server after a load 0.5 s to 2.0 s a cell.

M6, the load:

| arm | server CPU a row, median (range) | rows a second | leaf divisions (relation / index) | data file | log written |
|---|---|---|---|---|---|
| F0 | 4.65 us/row (4.61..4.73) | 114,884 rows/s | 0 / - | 108.0 MB | 695.2 MB |
| F2e | 11.75 us/row (11.52..11.85) | 69,278 rows/s | 18,646 / 7,499 | 217.6 MB | 1,022.6 MB |
| P1e | 9.28 us/row (9.12..9.31) | 75,816 rows/s | 0 / 7,499 | 168.8 MB | 1,007.5 MB |

M7 to M9:

| arm | shape | server CPU a statement, median (range) | client latency p0 / p25 / p50 / p95 / p99 | ANALYZE step |
|---|---|---|---|---|
| F0 | M7 present | 23.3 us (23.1..23.6) | 37 / 39 / 40 / 45 / 54 us | `Lookup t opens=1 examined=1 matched=1 sel=100% pages=1` |
| F0 | M8 absent | 22.9 us (22.9..23.1) | 36 / 38 / 39 / 44 / 52 us | `Lookup t opens=1 examined=0 matched=0 pages=1` |
| F0 | M9 range | 47.2 us (46.8..47.2) | 71 / 73 / 75 / 92 / 184 us | `Range t opens=1 examined=154 matched=100 sel=64% pages=3 range_stopped_early=1` |
| F0 | M9 page | 12,261.6 us (11,745.3..14,290.5) | 604 / 5,793 / 11,769 / 25,269 / 28,600 us | `Scan t opens=1 examined=99359 matched=100 sel=0% pages=1291` |
| F0 | M9 bare | 40.6 us (39.8..42.0) | 64 / 65 / 66 / 264 / 264 us | `Scan t opens=1 examined=100 matched=100 sel=100% pages=2` |
| F2e | M7 present | 30.9 us (27.2..31.6) | 38 / 42 / 42 / 48 / 61 us | `Lookup t opens=1 examined=1 matched=1 sel=100% pages=1` |
| F2e | M8 absent, the home | 25.6 us (25.5..30.7) | 37 / 40 / 41 / 48 / 57 us | `Lookup t opens=1 examined=0 matched=0 pages=1` |
| F2e | M8 absent, the index | 24.6 us (24.5..26.4) | 37 / 39 / 40 / 46 / 55 us | `IndexProbe t opens=1 examined=0 matched=0 pages=1` |
| F2e | M9 range | 212.7 us (210.7..308.6) | 233 / 244 / 260 / 336 / 504 us | `IndexRange t opens=1 examined=100 matched=100 sel=100% pages=101 index_scanned=100 index_resolved=100` |
| F2e | M9 page | 130,930.9 us (127,341.5..134,783.3) | 54,961 / 88,326 / 138,403 / 184,128 / 193,908 us | `Scan t opens=1 examined=1000000 matched=70070 sel=7% pages=18861` |
| F2e | M9 bare | 194,501.0 us (182,114.3..195,743.5) | 171,698 / 192,929 / 197,263 / 203,278 / 203,278 us | `Scan t opens=1 examined=1000000 matched=1000000 sel=100% pages=18861` |
| P1e | M7 present | 27.6 us (26.9..31.6) | 39 / 43 / 44 / 51 / 60 us | `IndexProbe t opens=1 examined=1 matched=1 sel=100% pages=2 index_scanned=1 index_resolved=1` |
| P1e | M8 absent | 24.4 us (24.2..25.2) | 36 / 39 / 40 / 46 / 53 us | `IndexProbe t opens=1 examined=0 matched=0 pages=1` |
| P1e | M9 range | 218.2 us (202.9..222.7) | 235 / 252 / 265 / 360 / 484 us | `IndexRange t opens=1 examined=100 matched=100 sel=100% pages=101 index_scanned=100 index_resolved=100` |
| P1e | M9 page | 97,198.5 us (94,253.0..97,451.2) | 34,018 / 64,954 / 100,351 / 147,782 / 165,578 us | `Scan t opens=1 examined=1000000 matched=70070 sel=7% pages=12988` |
| P1e | M9 bare | 132,991.8 us (130,631.9..135,165.8) | 128,120 / 131,678 / 133,477 / 136,780 / 136,780 us | `Scan t opens=1 examined=1000000 matched=1000000 sel=100% pages=12988` |

## What the cells say about the engine

- **A statement's fixed cost is about 22 us and a descent about 1 us to
  3 us**, so a form that saves one descent on a point read moves the
  statement by a few percent, and F2's whole case is one descent.
- **What a second structure costs is on the insert, not the read**: +4.5 us
  to +5.8 us a row for the index, against +0.6 us to +5.2 us a read.
- **Where a key arrives matters more than how it is found.** F2's rows
  land across the whole tree: 18,646 divisions and a data file 28 % larger
  at 1,000,000 rows, and every unserved shape scans the larger relation.
- **Server CPU over a phase is not a statement's cost once a checkpoint
  can land in the phase.** At 1,000,000 rows the same arm read 27 us to
  32 us from round to round while its p50 held; the median of batches, or
  the p50, is the estimator a close measurement over a loaded relation
  should use.
- **An embedded key (F1) is F0's arm with keys in the user's order**: one
  descent, no index, and a range by the tuple key (46 us against 137 us
  through an index at 100,000 rows). Where the keys arrive out of order it
  pays F2's placement cost without F2's index.

## Unverified

- A relation larger than the pool (above).
- `cores` above 1, and two sessions writing one key.
- P3's own price, which the walk hides (M4).
- F1 itself: the reading above is from F0's arm and F2e's placement, not
  from a cell of its own.
- The 1,000,000-row server CPU of M7 and M8 as a statement's cost (above).
