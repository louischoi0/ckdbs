Measured again at `7e18598d` after a remedy: `results-dc-close-v3.4.13-16-g7e18598d.md`.

# DC close: a statement without `FOR UPDATE` pays nothing the floor can resolve, and the locking read loses no increment

On worktree `dc-select-for-update` (branch `worktree-dc-select-for-update`) the
milestone's close measurement of DC-Q10 (a)
(`instructions/v3.2.0/workorder-dc-select-for-update.md` §4) was run between
A = `8e7c820f` (`v3.4.13-10-g8e7c820f`, where DC opened) and B = `cd80d31d`
(`v3.4.13-14-gcd80d31d`, the closing commit; its server sources are
`f6a12f5d`'s). Three findings:

1. **No lost increment.** Eight sessions looping `BEGIN; SELECT ... FOR UPDATE;
   UPDATE ... SET v = <read + 1>; COMMIT` on one row committed 90,681
   transactions over three 6 s cells and the final `v` of each cell equals its
   committed count (0 lost, three of three). The same loop with a plain
   `SELECT` committed 111,545 and left `v` at 11,233 / 11,235 / 11,172: 77,905
   increments (69.8 %) lost, with no error, at `READ COMMITTED`.
2. **The window DC-Q3 (a) names refuses 2.7 % of locking reads**, every one
   `TXN_CONFLICT ... was written by transaction`, every one at the `SELECT`.
   No deadlock, no lock-wait fault net, nothing else.
3. **The barred cells** (a point `SELECT` and a point `UPDATE` by tuple key,
   without the clause, B against A): the `SELECT` is +0.10, +0.23 and +0.17
   µs of server CPU a statement at 200, 1K and 10K rows - 0.5 to 1.1 % of
   about 20.5 µs and inside the 0.27 µs floor between two servers of one binary
   that the CX close bisect measured - **bar met, a lean of 0.1 to 0.2 µs not
   excluded**. The `UPDATE` cell is **not resolved from its floor**: +0.17,
   +0.42 and +0.78 µs of about 75 µs, against two servers of one binary
   disagreeing by up to 2.5 µs inside one lifetime.

Driver and raw output: `archive/dc-close-v3.4.13-14-gcd80d31d/`
(`point_arms.py`, `rmw_cell.py`, `an_point.py`, the `run_*.sh` scripts, every
lifetime's JSON and log). No PostgreSQL twin: the question is a delta inside
this engine, and the floor was not measured for these shapes
(`bench/README.md` rule 4).

## Run stamp

| Item | Value |
|---|---|
| Executed | 2026-11-19, 06:34 to about 07:08 UTC: builds 06:34 to 06:39; barred cells 06:39:34 to 07:03:43 (eight lifetimes); descriptive cell 07:05:23 to 07:06:04 (two earlier attempts at 07:03:47 and 07:04:27 were discarded, below) |
| Branch / worktree | `worktree-dc-select-for-update`, worktree `dc-select-for-update`; the worktree carries uncommitted document edits (`CLAUDE.md`, `docs/`, `instructions/`, `manual/`) and no source change; both arms were built from a clean tree of their commit, below |
| A | `8e7c820f`, `v3.4.13-10-g8e7c820f` |
| B | `cd80d31d`, `v3.4.13-14-gcd80d31d` |
| Build | each commit exported to its own source directory (`/home/louis/ck-tmp/dc-close/src<commit>`), Release (`-DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`), `kds_server` target only, GNU 14.2.0, outside the shared `build/`; 298 s (A) and 296 s (B), built concurrently with `-j6` each on a host another session was also building on. The binaries' mtimes (06:38:57 and 06:39:00 UTC) are after both commits |
| Binary sha256 | A `ab5b9ee7cc7875bbe199ee402214b8e12176c8a00f909eb6e0ca1aae4cba9437`, B `460473efaa9aee2ac5e8ac7c46736d01b6187a4f3f6666ad73fd560e6ab2faec`; every server started from a copy in `/home/louis/ck-tmp/dc-close/run/` (`A.bin`, `A2.bin` - a second copy of A's, the control - and `B.bin`), the copies' hashes equal to these |
| Device | data files, WAL and logs under `/home/louis/ck-tmp/dc-close/run/wd-*/` on `/dev/nvme0n1p1`, ext4 (`df -T` at run time); not tmpfs |
| Host | Intel Xeon Platinum 8581C @ 2.10 GHz, 8 CPUs, Linux 6.12.116+deb13-cloud-amd64 |
| Barred-cell server config | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`, every other key default (so `READ COMMITTED`, `join_order = fixed`); server pinned to CPU 2 and queried one at a time, the client to CPU 4; ports 27710 to 27782 |
| Descriptive-cell server config | `cores = 4`, `durability = group`, same keys; server pinned to CPUs 0-3, the client process (eight threads) to CPU 5; ports 27901 to 27906 |
| Protocol | KWP/1 (`tools/kwp.py`), the CX close's client; the barred cells use `Connection.query`, the descriptive cell `Connection.execute` |
| Host load | In every block's `host` record, taken as the block ends (the driver's order; `bench/README.md` rule 4 says before). Lifetime 1: load 10.2 to 12.1 and **every one of its 144 blocks had another session's `ctest -j8` running; the lifetime is excluded**. Lifetime 2: 3 competing blocks (another session's build), load 2.0 to 7.8; they are dropped from the round pairing. Lifetime 3: no competing process, load 1.6 to 5.9; **excluded from the pool** as the tail of that session's load. Lifetimes 4 to 8: load 1.2 to 2.3, 0 competing blocks of 144 each. Descriptive cells: load 2.2 to 3.9 on 8 CPUs, no competing process at the start or end of any |

## Method

**Barred cells.** One server lifetime is three servers started fresh with a
fresh data file each: **A**, **B** and **A2**, a second server of A's binary
which is the control that says what two servers of one binary differ by
(the noise floor from inside the run). Each holds three
tables `t200`, `t1k`, `t10k` of `(id int64, v int64)` BTREE with ids 1..N, N
= 200, 1,000, 10,000 - the row-set sweep; a point statement does not scale
with rows and the three sizes are shown as the evidence of that. A cell is a
fixed count of completed statements at one size:

| Cell | Statement | Statements a block | Warm-up |
|---|---|---|---|
| `sel-N` | `SELECT * FROM t<N> WHERE id = <random 1..N>`, autocommit | 10,000 | 1,000 untimed |
| `upd-N` | `UPDATE t<N> SET v = <random> WHERE id = <random 1..N>`, autocommit | 3,000 | 300 untimed |

A round is every cell once on every arm; the arm order is rotated and
reversed on alternate rounds; a lifetime is 8 rounds. **Server CPU a statement
is the server threads' `schedstat` over the block** (`bn_census.sched_ns`),
the CX close's measure. A delta is the median over rounds of the round-paired
difference, with a bootstrap 95 % interval of that median (2,000 resamples,
seeded), pooled over the five clean lifetimes (4 to 8: 40 rounds, 400,000
statements a `sel` arm and cell, 120,000 an `upd` one); the per-lifetime
medians are shown beside it because **the floor is a property of the server
process, not of the block count** (`bench/v3.4.13/results-cx-close-bisect-v3.4.13-1-g11f72ce9.md`).
"B - M" is B against the mean of A and A2 in the same round, which uses the
control as a second sample of A's binary.

**Descriptive cell.** Eight client threads on eight connections, one table
`t(id int64, v int64)` holding one row `(1, 0)`, each looping `BEGIN; SELECT v
FROM t WHERE id = 1 [FOR UPDATE]; UPDATE t SET v = <v + 1> WHERE id = 1;
COMMIT`; on any error the client sends `ROLLBACK` and retries the whole
transaction; 6 s a cell, a fresh server and data file per cell, three cells
of each mode alternating (`fu`, `plain`, `fu`, ...), B's binary only (A has no
such statement). **Every unit counts committed transactions only**; the final
`v` is read after the sessions stop.

## Result 1: the barred cells

The select, pooled over lifetimes 4 to 8 (round-paired server CPU, median
[95 % interval], 40 rounds; per-lifetime medians beside):

| Pair | Cell | life 4 | life 5 | life 6 | life 7 | life 8 | pooled median [95 % interval] |
|---|---|---|---|---|---|---|---|
| **B - A** | sel-200 | +0.11 | +0.32 | +0.09 | +0.42 | -0.08 | **+0.10 µs [+0.01, +0.24]** |
| **B - A** | sel-1k | -0.06 | +0.06 | +0.07 | +0.70 | +0.23 | **+0.23 µs [+0.04, +0.53]** |
| **B - A** | sel-10k | +0.07 | +0.22 | +0.13 | +0.78 | +0.14 | **+0.17 µs [+0.03, +0.39]** |
| B - M | sel-200 | -0.03 | +0.14 | +0.20 | +0.25 | -0.25 | +0.10 µs [-0.09, +0.22] |
| B - M | sel-1k | +0.07 | +0.00 | -0.01 | +0.65 | +0.16 | +0.15 µs [+0.03, +0.31] |
| B - M | sel-10k | +0.15 | +0.29 | +0.01 | +0.78 | +0.05 | +0.20 µs [-0.04, +0.36] |
| A2 - A (control) | sel-200 | -0.09 | +0.11 | -0.08 | +0.19 | +0.21 | +0.08 µs [-0.05, +0.19] |
| A2 - A (control) | sel-1k | +0.15 | +0.11 | +0.09 | -0.06 | +0.20 | +0.09 µs [+0.01, +0.22] |
| A2 - A (control) | sel-10k | -0.12 | -0.07 | +0.34 | -0.22 | +0.14 | -0.02 µs [-0.14, +0.13] |

The update, same layout (server CPU is the server's threads, the WAL writer's
included; a statement here is a commit and waits for the group sync):

| Pair | Cell | life 4 | life 5 | life 6 | life 7 | life 8 | pooled median [95 % interval] |
|---|---|---|---|---|---|---|---|
| **B - A** | upd-200 | -0.65 | -1.08 | +1.97 | +1.96 | -1.14 | **+0.17 µs [-1.51, +1.73]** |
| **B - A** | upd-1k | +0.36 | +1.37 | -0.75 | +1.18 | -2.10 | **+0.42 µs [-1.01, +1.18]** |
| **B - A** | upd-10k | +0.25 | +2.88 | +1.79 | -0.27 | +0.26 | **+0.78 µs [-0.53, +2.29]** |
| B - M | upd-200 | +0.96 | -0.07 | +1.39 | +1.25 | +0.58 | +0.67 µs [-0.29, +2.00] |
| B - M | upd-1k | +1.28 | +1.06 | -0.74 | +0.79 | -0.65 | +0.30 µs [-0.50, +1.09] |
| B - M | upd-10k | +1.48 | +1.80 | +0.71 | -0.02 | +0.29 | +0.78 µs [+0.10, +1.98] |
| A2 - A (control) | upd-200 | -0.13 | -0.52 | -0.08 | -1.69 | -2.10 | -0.64 µs [-1.62, -0.10] |
| A2 - A (control) | upd-1k | -0.24 | -0.96 | -0.38 | +0.76 | -2.37 | -0.35 µs [-1.22, +0.31] |
| A2 - A (control) | upd-10k | -1.82 | -0.54 | +0.32 | -0.69 | -0.51 | -0.65 µs [-1.22, +0.35] |

(Lifetimes 2 and 3 are in `archive/.../an_point.txt` and not pooled; they
show the same picture: `sel` B - A -0.04 to +0.33, `upd` B - A -0.59 to
+1.91.)

**Verdicts against the bar "no cell slower outside its own spread".**

| Cell family | B - A pooled | The floor it is read against | Verdict |
|---|---|---|---|
| `SELECT` by tuple key, 200 / 1K / 10K rows | +0.10, +0.23, +0.17 µs (0.5 to 1.1 % of 20.5 µs) | two servers of one binary: A2 - A pooled +0.08, +0.09, -0.02 µs, per lifetime -0.28 to +0.34 here; 0.27 µs over 30 rounds in the CX close bisect | **Met, inside the floor.** The pooled intervals lie above zero by 0.01 to 0.04 µs, so a lean of 0.1 to 0.2 µs is not excluded; lifetime 7 alone puts B 0.4 to 0.8 µs above A on all three sizes (and above A2 by 0.16, 0.64, 0.69), which is the size of one lifetime's layout offset in the bisect, and no other lifetime has any size above +0.33 |
| `UPDATE` by tuple key, 200 / 1K / 10K rows | +0.17, +0.42, +0.78 µs (0.2 to 1.0 % of 75 µs) | A2 - A per lifetime -2.4 to +2.5 µs, pooled -0.64, -0.35, -0.65 | **Not resolved from the floor.** The cell carries the group sync and the WAL writer's CPU, so a lifetime's spread is 2 to 3 µs and the pooled intervals (+/- 1 to 2 µs) contain zero on two of three cells; B sits above the mean of A and A2 by +0.30 to +0.78 µs and above A2 in 18 of 21 lifetime-cells, which says a cost under 1 µs of 75 is not excluded and not shown |

What the numbers do not say: that the clause's tail peek is free. They say
that, at this host's resolution, a statement without it is not measurably
slower on the `SELECT` and is not resolved on the `UPDATE`; if a cost exists
it is under about 0.25 µs of 20.5 on the former and under about 1 µs of 75
on the latter. The throughput and latency of every arm and cell (median over
blocks of each block's statistic; `QPS` is the block's statements over its
wall time):

| Cell | Arm | Blocks | QPS | p0 | p25 | p50 | p95 | p99 | Server CPU a statement |
|---|---|---|---|---|---|---|---|---|---|
| sel-200 | A | 40 | 25,192 | 34.5 µs | 37.2 µs | 37.7 µs | 46.6 µs | 55.6 µs | 20.41 µs |
| sel-200 | A2 | 40 | 25,103 | 33.6 µs | 37.2 µs | 37.8 µs | 47.0 µs | 56.1 µs | 20.58 µs |
| sel-200 | B | 40 | 25,147 | 34.4 µs | 37.3 µs | 37.8 µs | 46.6 µs | 55.5 µs | 20.63 µs |
| sel-1k | A | 40 | 25,164 | 34.3 µs | 37.3 µs | 37.8 µs | 46.7 µs | 55.6 µs | 20.51 µs |
| sel-1k | A2 | 40 | 25,030 | 34.5 µs | 37.3 µs | 37.8 µs | 46.9 µs | 55.6 µs | 20.58 µs |
| sel-1k | B | 40 | 25,125 | 34.9 µs | 37.4 µs | 37.9 µs | 46.5 µs | 55.3 µs | 20.62 µs |
| sel-10k | A | 40 | 24,949 | 34.3 µs | 37.5 µs | 38.1 µs | 47.3 µs | 56.1 µs | 20.67 µs |
| sel-10k | A2 | 40 | 25,018 | 35.3 µs | 37.6 µs | 38.1 µs | 47.0 µs | 55.0 µs | 20.64 µs |
| sel-10k | B | 40 | 24,971 | 35.4 µs | 37.7 µs | 38.3 µs | 46.6 µs | 55.3 µs | 20.71 µs |
| upd-200 | A | 40 | 1,948 | 309.0 µs | 438.8 µs | 494.5 µs | 686.4 µs | 822.5 µs | 75.16 µs |
| upd-200 | A2 | 40 | 1,958 | 308.0 µs | 438.0 µs | 492.5 µs | 679.9 µs | 805.2 µs | 74.12 µs |
| upd-200 | B | 40 | 1,947 | 310.6 µs | 442.0 µs | 496.2 µs | 686.1 µs | 827.3 µs | 75.39 µs |
| upd-1k | A | 40 | 1,954 | 311.5 µs | 440.1 µs | 494.6 µs | 686.4 µs | 822.0 µs | 75.53 µs |
| upd-1k | A2 | 40 | 1,967 | 306.1 µs | 437.2 µs | 491.2 µs | 677.3 µs | 809.0 µs | 74.68 µs |
| upd-1k | B | 40 | 1,954 | 310.0 µs | 440.3 µs | 493.8 µs | 685.9 µs | 825.5 µs | 75.70 µs |
| upd-10k | A | 40 | 1,983 | 307.4 µs | 434.5 µs | 488.5 µs | 675.8 µs | 804.6 µs | 73.84 µs |
| upd-10k | A2 | 40 | 1,957 | 305.8 µs | 437.8 µs | 492.8 µs | 683.7 µs | 812.3 µs | 74.09 µs |
| upd-10k | B | 40 | 1,942 | 310.7 µs | 439.7 µs | 493.9 µs | 683.9 µs | 828.8 µs | 76.59 µs |

Counts: a `sel` block is 10,000 statements (40 blocks, 400,000 an arm and
cell); an `upd` block is 3,000 (120,000). QPS and latency move together with
CPU and inside the same floor: B's QPS is within 0.3 % of A's on every `sel`
row, and within 0.1 to 2.1 % on `upd` (A's `upd-10k` is the one fast row; A2
sits between A and B there).

**Wait breakdown of a barred statement.** A `SELECT` by tuple key is 20.5 µs
of server CPU in a 37.7 µs round trip: the rest is the socket and the Python
client, not split here. An `UPDATE` is 75 µs of server CPU in a 494 µs round
trip; the remaining 420 µs is the group sync (`commit.wal_flush`) and the
socket, not separated by this driver (the wait views are read in the
descriptive cell below). Neither has a lock wait: one connection, no
contention.

**Against this engine's previous number for the same shape.** There is none:
no earlier results file measures `t(id, v)` at these three sizes. The nearest
is the CX close bisect's `pk-point-read` on a 9-column 60,000-row table at
21.4 to 21.9 µs of server CPU, a different table and select list, so no delta
is drawn. This file is the baseline the next run of the shape is read against.

## Result 2: eight sessions on one row (descriptive, no bar)

**`FOR UPDATE` costs 11 to 33 % of the transaction rate of the loop it
replaces (12 % at the median cell) and is the only one of the two that is correct.** B's binary,
`cores = 4`, 6 s a cell, three cells each, in the order run:

| Mode | Cell | Committed txns | Txns/s | Final `v` | Lost increments | Locking reads issued | Refused |
|---|---|---|---|---|---|---|---|
| `FOR UPDATE` | 1 | 33,450 | 5,574 | 33,450 | **0** | 34,381 | 931 |
| `FOR UPDATE` | 2 | 32,457 | 5,408 | 32,457 | **0** | 33,412 | 955 |
| `FOR UPDATE` | 3 | 24,774 | 4,128 | 24,774 | **0** | 25,400 | 626 |
| plain `SELECT` | 1 | 37,419 | 6,235 | 11,233 | 26,186 | 38,525 | 1,106 |
| plain `SELECT` | 2 | 37,077 | 6,178 | 11,235 | 25,842 | 38,321 | 1,244 |
| plain `SELECT` | 3 | 37,049 | 6,173 | 11,172 | 25,877 | 38,113 | 1,064 |

**Final `v` equals the committed count in all three `FOR UPDATE` cells (90,681
of 90,681); no increment was lost.** Without the clause 77,905 of 111,545
committed increments (69.8 %) were lost, none of them with an error: the
sessions' reads and the other sessions' commits interleave at `READ
COMMITTED`, so `SET v = <old + 1>` writes over a commit that landed between.
The rate range inside `FOR UPDATE` is wide (4.1k to 5.6k; cell 3 ran with the
host's load at 3.2 to 3.9, no competing process) and cell 3 is 26 % under cell
1, so the cost against the plain loop is read as 11 % (cell 1), 12 % (cell 2) and 33 % (cell 3) against the plain loop's 6.2k (median cell
5,408 against 6,178: -12 %), and the loop is not equal work: the plain one
commits transactions that overwrite each other.

**Refusals, by kind, over the three cells of each mode** (a share of the
statements the refusal belongs to):

| Mode | Refused text | Where | Count | Share |
|---|---|---|---|---|
| `FOR UPDATE` | `TXN_CONFLICT ... was written by transaction` (DC-Q3 (a)'s window) | the locking read | 2,512 | **2.70 %** of 93,193 locking reads (cells 2.71, 2.86, 2.46 %) |
| `FOR UPDATE` | `deadlock` | - | 0 | 0 % |
| `FOR UPDATE` | `hit the lock-wait fault net` | - | 0 | 0 % |
| `FOR UPDATE` | anything else | - | 0 | 0 % |
| plain `SELECT` | `TXN_CONFLICT ... was written by transaction` | the `UPDATE` | 3,414 | 2.97 % of 114,959 plain reads |
| plain `SELECT` | `deadlock`, fault net, anything else | - | 0 | 0 % |

A refused locking read fails its transaction (DC-Q11 (a)); the client's
`ROLLBACK` and re-run is counted in the rate. A refusal costs one retry, no
more: refused attempts are 2.7 % of attempts, so the retries are not what
slows the loop down. The longest lock wait the server recorded in any
`FOR UPDATE` cell was 9.5 ms (`sys.wait_stats`' `app.lock_wait` max), far from
the 1 s fault net.

Transaction latency as the client saw it (from `BEGIN` sent to `COMMIT`
answered, committed transactions only; eight Python threads under one GIL, so
the high percentiles carry the client's own queueing):

| Mode | Cell | Txns | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `FOR UPDATE` | 1 | 33,450 | 565 µs | 1,099 µs | 1,280 µs | 2,455 µs | 3,759 µs |
| `FOR UPDATE` | 2 | 32,457 | 579 µs | 1,109 µs | 1,306 µs | 2,643 µs | 3,746 µs |
| `FOR UPDATE` | 3 | 24,774 | 597 µs | 1,236 µs | 1,533 µs | 4,065 µs | 8,139 µs |
| plain `SELECT` | 1 | 37,419 | 595 µs | 1,039 µs | 1,214 µs | 1,825 µs | 2,241 µs |
| plain `SELECT` | 2 | 37,077 | 587 µs | 1,060 µs | 1,233 µs | 1,827 µs | 2,207 µs |
| plain `SELECT` | 3 | 37,049 | 605 µs | 1,057 µs | 1,240 µs | 1,837 µs | 2,176 µs |

The best case (p0) is the same 565 to 605 µs in both modes: one group sync
plus the round trips. The locking loop's cost is in its tail: the p99 is
1.7 to 3.7 times the plain loop's, because a session that meets a held row
waits for the holder's decide.

**Where the time goes.** The server's own wait views, read after each cell
(`sys.wait_stats` summed over the four cores and the three cells of a mode;
the 144 session-seconds are 8 sessions x 6 s x 3 cells):

| Wait event | `FOR UPDATE`, total | plain, total |
|---|---|---|
| `commit.wal_flush` (durability) | 114.8 s | 145.6 s |
| `app.lock_wait` (lock) | **91.5 s** | 28.5 s |
| `cpu.on_cpu` | 18.9 s | 20.2 s |
| `sched.run_queue` | 1.0 s | 1.4 s |
| `sysio.wal_write` + `checkpoint` + `wal_roll` | 0.1 s | 0.1 s |
| `app.horizon_wait`, `io.*`, `xcore.*`, `net.send_backpressure` | 0 | 0 |

These do **not** sum to the session-seconds and must not be read as shares of
them: the two largest overlap (a session parked on a row's lock is parked
behind a holder that is itself in `commit.wal_flush`), and `commit.wal_flush`
records exactly two counts a commit (181,374 for 90,681 commits; 223,102 for
111,545), so its count is not a commit count. What they order is clear
enough: the commit's durability wait and the lock wait are the whole of it,
CPU is a seventh and the scheduler a hundredth. The locking loop's
`app.lock_wait` is 3.2 times the plain loop's, and `sys.lock_waits`' rows are
tuple-unit `U` waits on this one row of 35 to 260 µs each, `granted` (the
archive's JSON keeps the view's summary - counts by outcome and unit, the
waits' percentiles, its first rows - and not its 16,384 raw rows). No
wait type outside commit and lock was non-zero. `idle.background` is the
cores that had no work and is left out of the sum.

## What the run teaches about the engine

* **The read path's price for the clause is below what this host resolves.**
  The claim DC-Q10 tests - one peek at the tail token and one test of a
  parsed field - holds at the 0.1 to 0.2 µs level on the select, and a
  statement that writes cannot show it (75 µs, floor 2 to 3 µs). The
  UPDATE's number is the weaker evidence, and the result that nothing is
  measured there is "not resolved", not "free".
* **One hot row is bounded by the commit, not by the lock.** A transaction
  that holds the row's claim through its `COMMIT` serialises on one group
  sync: both loops commit 5 to 6 thousand transactions a second on four cores
  with 0.5 ms of p0, so the ceiling is the sync's batch size and not CPU
  (`cpu.on_cpu` is a seventh of the wait). `FOR UPDATE`'s own price is the
  tail: sessions queue at the claim instead of racing and being overwritten.
* **`READ COMMITTED`'s read-modify-write is wrong 70 % of the time on one
  hot row and says nothing.** That is the contract `manual/client/client.md`
  states (read with `FOR UPDATE`, or write an expression over the old value);
  this run puts a number on it, and shows the locking read closing it with 0
  of 90,681 lost.
* **The refusal DC-Q3 (a) accepts is small and one kind.** 2.7 % of locking
  reads were refused, all as `TXN_CONFLICT` at the claim (a commit landing
  between the statement's view and its claim) and all retryable; the plain
  loop meets the same text at the `UPDATE` at about the same rate (3.0 %), so
  the clause moves the refusal rather than adding one. No deadlock arose with
  eight sessions on one row, and nothing reached the fault net.
* **No open decision in `CLAUDE.md` gained a contradicting data point.** The
  numbers support DC-Q3 (a) and DC-Q11 (a) as adopted; DC-Q12's missing
  `RETURNING`-style row bound was not exercised (one row).

## What was cut, what was lost, and the time

* **The CX close bisect's ten-arm stage was not rerun**; one three-arm
  design (A, B, A2) at eight lifetimes of eight rounds replaced it, and the
  row-set sweep (200, 1K, 10K) is in the barred cells, not in the
  descriptive one (one row by construction).
* **Statement counts were cut for time**: 10,000 a `sel` block (20,000 in the
  bisect) and 3,000 an `upd` block, because an `UPDATE` commits at about 2,000
  a second.
* **Lifetimes 1 to 3 are excluded from the pool**: 1 ran under another
  session's `ctest -j8` for its whole length (load 10 to 12), 2 had three
  blocks overlapping another session's build, 3 started at load 6.3 and ended
  at 1.6 (no competing process, but the tail of that load). They are kept in
  the archive. The pool is lifetimes 4 to 8 (five).
* **The descriptive cell was run three times in two modes** (3 cells of 6 s
  each, the task's design) after two discarded attempts: the first
  (07:03:47) died on the client's reading of an `int64` cell (raw bytes, not
  text) and ran no transaction; the second (07:04:27) ran cleanly and was
  rerun only because its wait-view output had been written unrendered. The
  numbers above are the third run's alone.
* **No PostgreSQL floor** (rule 4: a missing twin is not a hole); **no A
  server in the descriptive cell** (A has no such statement); the host has no
  PMU, so no instruction counts.
* **Time.** Building 5 min (two concurrent builds, `-j6` each, on a host
  another session was also building and testing on); running 36 min wall
  (barred cells 24 min, of which lifetimes 1 to 3, excluded, were about 8;
  descriptive cell 1 min plus two discarded attempts, 2 min); **held by
  another session's build or suite: about 5 min** (11 polls of the quiet
  check between 06:42 and 06:47, `archive/.../held.log`; lifetime 1 started
  at 06:39 with a `ctest` already running, was not held, and was discarded).
  A 60 s watchdog wrote the driver's liveness and status to
  `archive/.../watch.txt` throughout. The whole is inside the one-hour budget;
  the hold is outside it.
* **The correctness suite** was not executed here: no engine code was
  changed for this measurement, and no data file or WAL segment is in the
  archive.
