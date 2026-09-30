# AY-S11 — what AY costs, `58198cb` against `0552d55`

`instructions/v3.0.0/raft-marks-2026-09-30.md` §8: the interleaved A/B overhead
measurement over a milestone's whole code change, at its close. Run 2026-09-30
06:00–06:14 UTC on `worktree-ay-s11-close`, `v2.7.0-530-g0552d55`.

**The short answer, cell by cell.** Every number is B (`0552d55`) minus A
(`58198cb`), `cores = 1`, `relaxed`, BTREE, with the noise floor beside it.

| cell | what | B − A | noise floor | reading |
|---|---|---|---|---|
| 1 | AT-S13's statement mix, 8 sessions, p50 end to end | **−2.4 to +3.0 µs** of ~340 µs on the write arms (`update-hot` +2.3, −0.5, +3.0 µs) | 0.9 / 1.4 / 3.3 µs (B), 0.3 / 0.3 / 0.4 µs (A); the controls moved −4.8 to +0.0 µs | **not resolvable** end to end |
| 1 | the same, the engine's own time per statement (`SHOW META`) | **+0.41, +0.03, −0.44 µs** of ~16 µs | each engine moves 15.6–17.1 µs between its own runs | **not resolvable**; three runs bound it only to about ±1 µs (~6 %) of the engine. Without `SHOW META` (a sixth of the mix, 3.7 µs cheaper on B) the rest reads ~+0.7 µs, estimated |
| 2 | D9(a): child `INSERT` referencing an existing parent row, one row per transaction (autocommit) | **+0.40 µs** p50 (IQR +0.26 to +0.62 µs) of ~51 µs; engine +0.24 µs of ~14 µs | 0.26 µs (B), 0.12 µs (A) (one statement repeated) | **resolved, small**: ~0.8 % at the client, ~2 % of the engine, ~1.5× the floor and ~0.1 µs above the largest control |
| 2 | the same in `BEGIN` / `INSERT` / `COMMIT` | +0.35 µs (IQR −0.33 to +1.12 µs) | | **not resolvable** |
| 2 | controls: parent-row `UPDATE`, child `UPDATE` (fk column untouched), fk-less insert, point read | −0.08, +0.30, −0.13, +0.08 µs | as above | within 0.30 µs; `child-update` just above the same-server floor. `SHOW META` −8.17 µs, changed by AY (`62a6cb3`) |
| 3 | K child rows in one transaction, each a distinct parent, per row, fk | K = 64: −0.12 µs; 1,024: −0.46 µs; 4,096: **+0.76 µs**; 16,384: **+3.43 µs** (IQR +1.1 to +3.9 µs) | fk-less control: −0.37 / −0.08 / −0.35 / −1.47 µs | **a per-row cost that grows with K is there**: not resolved at K ≤ 1,024, marginal at 4,096, clear at 16,384 (+56 ms on a 16,384-row transaction, ~6 %). **The resolvable part is at the decide** (+35 ms of B's `COMMIT` side at 16,384), not in the inserts |

**What AY costs, in one paragraph.** On the statements AY did not touch, at
one core, nothing resolvable: cell 1's mix moves by less than its controls
do, and cell 2's controls sit within ~0.3 µs (`SHOW META`, which AY did
touch, is 8 µs faster). D9(a)'s fence on a
one-row child insert costs about **0.4 µs** (of ~51 µs at the client, ~14 µs
in the engine). What the fence adds to a transaction of K distinct parents
grows with K: about **0.2 ns × K a row** on average (+0.8 µs a row at
K = 4,096, +3.4 µs at 16,384). **Most of it is paid at the decide, not by
the inserts**: B's `COMMIT` side at K = 16,384 is 49 ms against A's 14 ms
in every run, ~2.1 µs of the 3.4 a row, and the inserts' own excess is not
resolvable. What the data resolves is therefore not the per-insert `Holds`
scan `docs/inflight/known-gaps.md` (Foreign keys) predicted; cell 3 says
where it is.
A transaction of 1,024 rows or fewer cannot tell it from noise.

---

## Rules (`bench/README.md`)

1. **Release, rebuilt at the measured commit.** Both arms built `Release`
   (`CMakeCache.txt` `CMAKE_BUILD_TYPE:STRING=Release`) from `git archive` of
   the commit into a scratch tree, target `kds_server` only, with
   `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`. The worktree's own build
   directories were not used.
2. **Block device.** Data files under `/home/cdkbs/bench-runs/ay-s11/` on
   `/dev/root`, ext4 (`df -T`; `$HOME` and `/tmp` are both `/dev/root`).
3. **A copy of the binary, hashed.** Every server started from the copy:

   | arm | commit | describe | binary | sha256 | source binary mtime |
   |---|---|---|---|---|---|
   | A | `58198cb` (AY-S0's base, committed 2026-09-29 05:45:15 UTC) | `v2.7.0-481-g58198cb` | `kds_server-A` | `56c2a6d874d46dbf73fa27093186ace7cffde50446d418c0f7b1b46d25835fcd` | 2026-09-30 05:58:45 UTC |
   | B | `0552d55` (AY's last code commit, committed 2026-09-30 05:52:30 UTC) | `v2.7.0-530-g0552d55` | `kds_server-B` | `1d07d33386541ec3a13090d37918e229957e29c2c777ff15f2a63db6957f8746` | 2026-09-30 05:59:20 UTC |

   Both binaries were built after both commits, from exact `git archive`
   trees, so neither is older than the commit it names. The worktree was
   clean at `0552d55` when the run started; the one file added since,
   `tools/fk_overhead_benchmark.py` (below), enters neither binary. A's
   superblock is version 17 and B's 19; each arm ran on its own fresh
   volume in every run.
4. **Host load, per cell.** `/proc/loadavg` and the
   `pgrep -a -f "cc1plus|cmake --build|ctest"` result are in every
   `host.txt` and every JSON under
   `archive/ay-s11-overhead-v2.7.0-530-g0552d55/`, and reproduced in the raw
   tables. **No competing build in any run**; one-minute load before each
   run 0.52–0.98 (the driver's own threads). The run script waited (bounded)
   for the one-minute load to fall under 1.0 before each pair.
5. **Ports chosen.** 15600 (B) and 15601 (A) for every measured pair,
   15602 / 15603 for the behavioural check. Not 15432.

Host: 8 CPUs. Every server `cores = 1`, `durability = relaxed` (`group`'s
commit is 82–85 % of an update and would hide what is measured), BTREE,
`log_level = warn`, otherwise defaults, **a fresh server and data file for
every (cell, run)** - eleven pairs. The two servers of a pair run side by
side and the driver alternates which is measured first per block, so only
one is active at a time. `strict` was not measured.

## Drivers

- **Cell 1**: `tools/lock_contention_benchmark.py`, **unmodified** - the
  driver AT-S13's cell 2 used (no change to it, `bench_common.py` or `kwp.py`
  since `f6f2073`). Arguments as AT-S13's: `--sessions 8 --rows 256 --ops
  2000 --blocks 4 --label B --ab-label A`, three runs.
- **Cells 2 and 3**: a **new driver, `tools/fk_overhead_benchmark.py`**,
  because no existing driver has a foreign key or a multi-row transaction.
  It lands with this stage, enters neither binary, and its usage is in its
  docstring (`bench/docs/` is closed). `--cell price` (cell 2) and `--cell
  ledger` (cell 3); one session per server, serial, so a statement's latency
  is the client's round trip. It reads `SHOW META` around every block for the
  engine's time per statement and records a p50 per block. Cell 2: `--ops
  4000 --blocks 8`, five runs. Cell 3: `--ks 1,64,1024,4096,16384
  --rows-per-k 32768`, three runs (reps per K = rows / K, at least 5).
- Behavioural check, not a price: `archive/.../chk.sh` opens a child
  transaction on each binary and deletes its parent from a second session.
  **B**: the delete waits on the parent row's `S` and is refused
  `TXN_CONFLICT` at the 1 s fault net. **A**: refused at once by the reverse
  check (`chk-output.txt`). The fence cell 2 prices is live in B and absent
  from A.

## Cell 1 — AT-S13's statement mix, the whole-milestone A/B

End to end, B − A p50, three runs (8 sessions, 16,000 statements per arm per
server per run):

| run | `update-hot` | `update-disjoint` | `insert-omitted` | `select-hot` | `ping` (`SHOW META`) | floor B / A (`update-disjoint` against its repeat) |
|---|---|---|---|---|---|---|
| 1 | +2.3 µs | −0.2 µs | −2.4 µs | −1.6 µs | −0.3 µs | 0.9 µs / 0.3 µs |
| 2 | −0.5 µs | −1.9 µs | +1.0 µs | −4.8 µs | −2.4 µs | 1.4 µs / 0.3 µs |
| 3 | +3.0 µs | +2.2 µs | −0.2 µs | +0.0 µs | −3.2 µs | 3.3 µs / 0.4 µs |

**Not resolvable.** The write arms move −2.4 to +3.0 µs of ~340 µs with no
consistent sign, and the two controls (a read, `SHOW META`), which take no
borrow, move as much (−4.8 to +0.0 µs). `SHOW META` is not a clean control:
AY made it cheaper (cell 2, `ping`).
`update-hot` is +2.3 / −0.5 / +3.0 µs against a B floor of up to 3.3 µs.
Absolute p50s are 337–344 µs for every write arm on both engines, throughput
18.4–20.5 k qps (raw tables below). This driver saturates at its Python
client, as AT-S13 recorded, so it says what a client sees and not what the
engine does.

The engine's own time per statement, `sched_foreground_polled_us /
sched_foreground_polls` from `SHOW META`, 96,001 statements per server per
run:

| run | B | A | B − A |
|---|---|---|---|
| 1 | 17.15 µs | 16.74 µs | +0.41 µs |
| 2 | 16.06 µs | 16.03 µs | +0.03 µs |
| 3 | 15.63 µs | 16.07 µs | −0.44 µs |

**Not resolvable, and small.** The sign changes across runs and each engine
moves 15.6–17.1 µs between its own runs, more than the delta. Run 1's +0.41
µs is the one number a reader could seize on: it is the run in which both
engines read highest and it is not repeated. **Bound: the three paired
differences average 0.0 µs, and three runs bound AY's addition only to about
±1 µs of ~16 µs (~6 %) per statement on this mix at one core** (a t-interval
on three pairs: 4.3 × 0.43 µs / √3). **One arm biases it towards B**:
`ping` is `SHOW META`, a sixth of the 96,001 statements, and cell 2 finds
B's engine 3.7 µs cheaper on it (the deleted `split_relations` walk). If that
holds here - estimated, not measured per arm - the other five arms read
about +1.2, +0.8 and +0.2 µs, all three runs positive, mean +0.7 µs (~4 %),
still inside the ±1 µs a three-run interval allows. The mix takes no foreign key, so it prices what AY did to
every statement, not the fence. No stall of the ~0.4 s kind AT-S13 recorded
appears in cell 1 (cell 3 has one, on both engines): the largest latency of any arm in cell 1 is 87 ms (B, run 1) and the
p99s are ~1.0 ms on both engines.

## Cell 2 — D9(a)'s price: a child insert's parent `IS` and `S`

**The fence costs a one-row child insert about 0.4 µs.** B takes `IS` on the
parent relation and `S` on the parent row before the descent and holds them to
the decide; A takes neither. Serial, one session, `cores = 1`. Five runs of
4,000 statements per arm per server, in 8 alternating blocks.

**Read this first: the host has a slow mode.** A server's p50 for an arm
sits at ~51 µs or, for consecutive blocks, ~75 µs (`child-update` on A, block
p50s in order: 51 51 51 51 74 75 75 75 75 52 52 52 76 75 …), on **either**
engine: 6 of 40 blocks of `child-insert-fk` on A, 12 of 40 of
`child-insert-plain` on B, 21 of 40 of `child-update` on A. The engine's own
time per statement rises with it (26 µs against 14 µs, `child-update`), so
it is server-side, not the client. A whole-arm p50 is then a coin toss
between two levels (the per-run table below shows arms flipping 50 -> 72 µs
on either engine), and an unfiltered B − A on `child-update` reads **−23 µs**
on runs 2, 3 and 4. **The table below uses only block pairs in which both
servers ran in their fast mode** (a block is slow when its p50 exceeds 1.12 ×
that server's own minimum block for the arm) and takes the median of the
per-pair B − A differences. Nothing is dropped from the raw tables.

| arm | clean pairs of 40 | B p50 | A p50 | derived qps B / A (1e6 / p50) | **B − A p50** (IQR) | B − A engine per op |
|---|---|---|---|---|---|---|
| `child-insert-fk` (autocommit, one txn) | 34 pairs | 51.5 µs | 51.1 µs | 19,417 / 19,569 qps | **+0.40 µs** (+0.26 to +0.62 µs) | +0.24 µs |
| `child-insert-fk-txn` (BEGIN, INSERT, COMMIT; the unit) | 23 pairs | 111.5 µs | 111.1 µs | 8,969 / 9,001 txn/s | +0.35 µs (−0.33 to +1.12 µs) | +0.35 µs |
| control `child-insert-plain` (no fk declared) | 23 pairs | 49.6 µs | 49.8 µs | 20,161 / 20,080 qps | −0.13 µs (−0.48 to +0.13 µs) | −0.04 µs |
| control `parent-update` (no child writer open) | 28 pairs | 50.9 µs | 50.7 µs | 19,646 / 19,724 qps | −0.08 µs (−0.60 to +0.42 µs) | −0.38 µs |
| control `child-update` (fk column untouched) | 16 pairs | 51.4 µs | 50.9 µs | 19,455 / 19,646 qps | +0.30 µs (−0.19 to +0.68 µs) | −0.26 µs |
| noise floor `child-update-again` | 33 pairs | 51.1 µs | 51.1 µs | 19,569 / 19,569 qps | −0.09 µs (−0.66 to +0.29 µs) | −0.20 µs |
| control `select` (pk read) | 34 pairs | 64.8 µs | 64.8 µs | 15,432 / 15,432 qps | +0.08 µs (−0.41 to +0.52 µs) | −0.06 µs |
| control `ping` (`SHOW META`) | 33 pairs | 38.0 µs | 46.3 µs | 26,316 / 21,598 qps | **−8.17 µs** (−8.39 to −7.09 µs) | −3.69 µs |

Noise floor from inside the run: the same statement repeated on one server
(`child-update` against `child-update-again`, same block position, both
fast) differs by a median **0.26 µs (B) and 0.12 µs (A)** in absolute value
(IQR −0.16 to +0.34 µs on B). Against it:

- **`child-insert-fk` +0.40 µs is resolved, and small**: its IQR excludes
  zero (34 pairs), the engine's per-op difference agrees in sign (+0.24 µs),
  and the fk-less insert, the same shape with no fence, reads −0.13 µs. It is
  ~1.5× the floor and not an order above it, and only ~0.1 µs above the
  largest control (`child-update`, +0.30 µs; the engine-time sign is what
  separates them: +0.24 µs against −0.26): read it as "about 0.4 µs,
  between 0.2 and 0.6".
- The **explicit-transaction unit** (`BEGIN`, `INSERT`, `COMMIT`: three round
  trips) has +0.35 µs with an IQR spanning zero: **not resolvable** there.
  The fence is one part of one of three statements.
- **Every control but `ping` is within 0.30 µs**; `child-update`'s +0.30 µs
  sits just above the same-server floor. **`ping` is the exception and is
  not AY's fence**: B answers `SHOW META` 8.2 µs faster than A (38 against
  46 µs) with 3.7 µs less engine time, in every one of the five runs
  (per-run medians −8.5, −20.6, −8.3, −7.1, −7.6 µs). AY changed that path:
  at `58198cb`, `SHOW META`'s `split_relations` block ran `ListTables()`
  and `RangesOf()` for every relation, and `62a6cb3` (the split relation's
  retirement) deleted it. Read from the source, not profiled.
- The engine's own time for the fk insert is ~14 µs a statement (block
  median 14.1 µs on both B and A) of the ~51 µs the client sees; the rest is
  the client's round trip and the reactor's wake. The engine's +0.24 µs is
  ~2 % of its statement; the client's +0.40 µs is ~0.8 % of what it sees.

**Latency decomposition (ck-tester's documentation rule 3).** The unit is one serial statement on one
session: ~51 µs = ~14 µs engine (`SHOW META`) + ~37 µs client, socket and
reactor wake (not separable here). No fsync wait (`relaxed`: the commit
returns without one), no lock or conflict wait (one session, nothing
contends: the fence is *taken* and never *waited on* in this cell), no read
wait of note. What cell 2 prices is the acquisition and release of two lock
entries, and the answer is ~0.4 µs.

**Row-count sweep (ck-tester's documentation rule 9).** A pk point statement does not scale with rows,
and this cell did **not** show it: the parent has 256 rows and the child 256
named rows at the start, one cardinality. Cell 3 is the row-count axis, at
the transaction level.

## Cell 3 — a transaction of K distinct parents (the ledger question)

**A per-row cost that grows with the transaction's size is there, small, and
mostly at the decide: +0.76 µs a row at K = 4,096, +3.4 µs at 16,384,
nothing resolvable at K ≤ 1,024.** One explicit transaction
inserts K child rows, each referencing a distinct parent (parents 1..K of
16,384; child pk named, every insert the same shape), `BEGIN` to `COMMIT`
timed whole and divided by K. `plain` is the same K inserts into a child with
no fk declared. K = 1 is one transaction of three round trips and is per
transaction, not per row. Three runs, reps pooled: K = 1: 98,304
transactions; 64: 1,536; 1,024: 96; 4,096: 24; 16,384: 15. No errors at any K
(`max_locks_per_txn` is 65,536 and was not reached). Row-count mapping: K is
the rows in one transaction; 1,024 / 4,096 / 16,384 bracket ck-tester's rule-9 1K /
10K axis and K = 64 stands for the small end (a 200-row transaction was not
run).

Median per-row wall time, with derived rows per second (1e6 / µs):

| K | `fk` B | `fk` A | derived rows/s B / A | `plain` B | `plain` A | `fk` − `plain` B / A | paired B − A per row, `fk` (IQR) | paired B − A per row, `plain` (IQR) |
|---|---|---|---|---|---|---|---|---|
| 1 (per txn) | 120.1 µs | 121.1 µs | 8,326 / 8,257 txn/s | 116.9 µs | 118.7 µs | +3.2 / +2.3 µs | −1.08 µs (−8.9 to +7.5 µs) | −2.29 µs (−8.6 to +5.0 µs) |
| 64 | 53.8 µs | 53.9 µs | 18,587 / 18,553 rows/s | 52.8 µs | 53.2 µs | +1.0 / +0.7 µs | −0.12 µs (−1.0 to +0.7 µs) | −0.37 µs (−1.0 to +0.2 µs) |
| 1,024 | 52.5 µs | 53.1 µs | 19,048 / 18,832 rows/s | 51.8 µs | 51.8 µs | +0.7 / +1.2 µs | −0.46 µs (−1.1 to +0.4 µs) | −0.08 µs (−0.7 to +0.4 µs) |
| 4,096 | 53.4 µs | 52.5 µs | 18,727 / 19,048 rows/s | 51.2 µs | 51.5 µs | +2.2 / +1.0 µs | **+0.76 µs** (+0.0 to +1.2 µs) | −0.35 µs (−2.4 to +0.4 µs) |
| 16,384 | 57.2 µs | 54.3 µs | 17,483 / 18,416 rows/s | 51.7 µs | 53.3 µs | +5.5 / +1.0 µs | **+3.43 µs** (+1.1 to +3.9 µs) | −1.47 µs (−5.2 to −0.8 µs) |

Paired B − A is per repetition (both engines ran the same rep back to back,
order alternating). The `plain` column is this cell's floor: what run-to-run
swing does to a same-shape transaction. It is ±0.4 µs at K ≤ 4,096 and
**−1.5 µs at 16,384**, so the `fk` +3.43 µs at 16,384 is ~2× its own
control's excursion and in the other direction. `fk` − `plain` on B alone
(+0.7, +2.2, +5.5 µs at 1,024, 4,096, 16,384) grows with K while A's
(+1.2, +1.0, +1.0 µs) does not.

**Reading.** The paired `fk` B − A is −0.46 µs at 1,024, +0.76 at 4,096 and
+3.43 at 16,384: about ×4.5 for ×4 in K, which a per-row cost linear in K (a
quadratic transaction) predicts, at roughly **0.2 ns × K a row on average,
+3 ms on a 4,096-row transaction, +56 ms on a 16,384-row one**. That is a
two-point fit with a ~1.5 µs floor beside it: it does not prove the shape,
and nothing was measured above K = 16,384.

**Where the excess is.** The driver records each statement's latency
(`stmt_us_by_pos`) apart from the transaction's wall time, so the
transaction splits into its inserts and the rest - `BEGIN`, `COMMIT` and
the driver's own loop, which is the same code on both arms. The rest, per
transaction, pooled per run (`t3b.py`):

| K | `fk` B | `fk` A | `plain` B | `plain` A | `fk` B − A |
|---|---|---|---|---|---|
| 1,024 | 0.70 / 0.69 / 0.73 ms | 0.50 / 0.49 / 0.44 ms | 0.50 / 0.44 / 0.47 ms | 0.46 / 0.49 / 0.44 ms | +0.23 ms (+0.23 µs a row) |
| 4,096 | 4.56 / 4.39 / 4.49 ms | 2.12 / 1.95 / 1.92 ms | 2.27 / 1.95 / 2.08 ms | 2.06 / 1.96 / 2.18 ms | +2.5 ms (+0.61 µs a row) |
| 16,384 | 49.12 / 49.04 / 49.13 ms | 14.62 / 13.84 / 14.20 ms | 13.85 / 13.77 / 13.88 ms | 14.05 / 14.79 / 14.24 ms | +34.9 ms (+2.13 µs a row) |

The rest is the steadiest number in the cell - three runs agree to 0.1 ms on
B's `fk` arm - and B's excess over A grows ~×14 for ×4 in K: **of the +3.43 µs a
row at K = 16,384, ~2.1 µs is paid after the last insert**. The inserts'
own B − A (the per-rep total less that run's rest, median) is −0.77,
+0.32 and +0.81 µs a row at 1,024, 4,096 and 16,384, against the `plain`
control's −1.47 µs at 16,384: not resolvable. So the resolved cost is
not the per-insert `Holds` scan the gap names. By source read at `0552d55`,
not profiled: `Holds` is asked of a relation intention, which a
transaction takes on its first insert and which sits among the first
entries of `held_`, so the scan returns at once here; and the decide's
`LockTable::Release` (`manager.cpp`) runs
`ReleaseHeld` per borrow, which finds the entry in its partition's vector,
erases it from the front, and then `WakeWaiters` scans the same partition
for the key it just erased - O(n / 64) a borrow, O(n² / 64) a decide, with
D9(a) doubling n. That fits A's rest growing too (0.49 -> 0.87 µs a row
from 4,096 to 16,384 on both of A's arms and on B's `plain`).

The per-row medians (B / A 57.2 / 54.3 µs at 16,384) carry the host's slow
mode on A's side (A's `fk` p75 is 73.0 µs); the paired medians and the rest
above do not depend on it.

**A ~0.4 s stall, on both engines, not AY's.** In every run the first
K = 1,024 `fk` transaction (~440 µs a row against ~53) and the second
K = 16,384 `fk` transaction (~81 µs a row against ~57) each carry ~0.4 s
more, inside their seventh octile of statements (octile means 153 µs on B,
152 on A at 1,024; 96 / 90 µs at 16,384). It never falls in `plain`, which
runs after `fk` at each K. It is what lifts A's engine time per row at
16,384 to 19.7 µs, and B's to 22.4; it is not a K-dependent term. Its cause
is not identified here.

**Latency decomposition (ck-tester's documentation rule 3).** `relaxed`: no
fsync wait inside the transaction (the decide's log write is one per
transaction), no lock wait (one session). At K ≥ 64 the unit is ~52–57 µs a
row, of which the engine is ~13–15 µs (`plain`); the fence's term is the
excess above the A arm, most of it at the decide.

## What this decides, and what it does not

- **AY's overhead on the general path at one core is not resolvable**
  (cell 1), and on the statement D9(a) added a lock to it is ~0.4 µs (cell
  2). Nothing here is a regression for a one-row write. The overhead
  measurement `CLAUDE.md`'s step 3 asks at a milestone's close is, for AY,
  **measured**.
- **D9(a)'s open cost on a large transaction (`known-gaps.md`, Foreign
  keys) has its first data point**: ~0.2 ns × K a row - not visible to a
  transaction of 1,000 distinct parents, ~6 % at 16,000 - **and it is at the
  decide, not in the ledger's `Holds` scan** the entry predicted. Indexing
  `Holds` would not remove what was measured; by source read, the lock
  table's per-partition release would. Whether it stands as the price is the
  operator's to decide; measured at one core only.
- **Not measured, and not implied**: `cores > 1` (the fence is *waited on*
  across cores, and a single session cannot exercise the wait D9(a) exists to
  impose on a parent `DELETE`), `group` or `strict` durability, a child
  `INSERT` whose parent lock is contended, a parent `DELETE` (the reverse
  check), PostgreSQL's floor for these shapes, and any client faster than this
  Python driver (~37 µs of a 51 µs statement lies outside the engine, so a
  faster client would see a 0.4 µs fence as a larger share).
- **Qualifications.** (1) Cell 2's slow mode is a host state present on both
  engines; the clean-pair filter and its 1.12× threshold are this file's
  choice, so the unfiltered rows are in the raw tables. The choice does not
  make the number: any threshold from 1.05× to 1.3× keeps the same pairs
  (the two modes are ~24 µs apart), and with no filter the `child-insert-fk`
  median is +0.31 µs over all 40 pairs. (2) A first pass of
  cells 2 and 3 was made with the driver before it recorded per-block p50s,
  showed the same bimodality, and was redone with the block records; it is
  not reported. (3) Cell 3's K = 4,096 and 16,384 rest on 24 and 15 paired
  repetitions. (4) No `SHOW META` control runs inside cell 3's transactions;
  its control is the fk-less `plain` insert. (5) B's `SHOW META` is 8 µs
  faster than A's; `62a6cb3` deleted a per-relation catalog walk from it
  (source read, not profiled), so it is AY's and not a control. (6) Cells 2
  and 3 run on a driver written in this stage; `bench/README.md` asks for
  unmodified `tools/` drivers so that a driver change is not measured as an
  engine change, which holds here only because both arms ran the same
  driver.

---

## Raw tables

Every arm, both engines, p0 / p25 / p50 / p95 / p99 with throughput. The JSON
under `archive/ay-s11-overhead-v2.7.0-530-g0552d55/` (`c1-run*`, `c2-run*`,
`c3-run*`, each with `result.json` - gzipped in `c3-run*` (`result.json.gz`,
~4 MB each raw; `gunzip -k` before `t3.py`) - `driver.txt`, `host.txt` and
both servers' configs; the servers' warn logs are `*.log`, which
`.gitignore` keeps out of the commit) carries the per-block records (cells
2 and 3) and the per-core `SHOW META` before and after (cell 1);
`t2b.py`, `t3.py`, `t3b.py` (cell 3's decide split), `breakdown.py` and
`gen.py` in that directory reproduce every derived number here from them,
each reading `R` - the run directory, `/home/cdkbs/bench-runs/ay-s11` -
which a reader points at this one. `pair.sh`, `all.sh` and `all2.sh` are
the orchestration: cell 1's three runs are `all.sh`'s, and its cell 2 and 3
runs are the unreported first pass (qualification 2), overwritten by
`all2.sh`'s.

`child-insert-fk-txn:stmt` is the `INSERT` alone inside the explicit
transaction and has no throughput of its own (shown as 0 qps).

### Cell 1 raw

**`c1-run1/result.json`** - load before `0.98 2.10 2.56 1/249 149551`, after `1.05 2.08 2.54 1/243 149944`; competing builds: none

| arm | B (`0552d55`): p0 / p25 / p50 / p95 / p99, throughput | A (`58198cb`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `update-hot` | 45.2 / 239.5 / 343.5 / 755.4 / 1050.6 µs, 20035 qps | 43.9 / 239.7 / 341.2 / 749.7 / 1008.9 µs, 19683 qps | 0 / 0 errors |
| `update-disjoint` | 45.0 / 238.7 / 341.0 / 757.5 / 1022.5 µs, 20347 qps | 42.6 / 239.1 / 341.2 / 759.5 / 1016.7 µs, 19869 qps | 0 / 0 errors |
| `update-disjoint-again` | 48.5 / 241.1 / 340.1 / 752.8 / 1022.0 µs, 18354 qps | 40.2 / 238.3 / 340.9 / 756.7 / 1055.8 µs, 19913 qps | 0 / 0 errors |
| `insert-omitted` | 37.1 / 236.2 / 337.7 / 749.6 / 1011.9 µs, 19226 qps | 35.6 / 237.2 / 340.1 / 742.2 / 1004.1 µs, 19611 qps | 0 / 0 errors |
| `select-hot` | 51.8 / 306.2 / 429.2 / 914.7 / 1219.6 µs, 16638 qps | 57.2 / 310.5 / 430.8 / 925.6 / 1228.5 µs, 16518 qps | 0 / 0 errors |
| `ping` | 36.0 / 222.6 / 329.4 / 766.2 / 1017.3 µs, 20826 qps | 37.9 / 214.0 / 329.7 / 778.4 / 1044.7 µs, 20718 qps | 0 / 0 errors |

**`c1-run2/result.json`** - load before `0.96 1.83 2.42 1/247 150293`, after `1.12 1.84 2.41 1/237 150686`; competing builds: none

| arm | B (`0552d55`): p0 / p25 / p50 / p95 / p99, throughput | A (`58198cb`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `update-hot` | 37.8 / 240.1 / 340.3 / 760.9 / 1019.3 µs, 20267 qps | 44.4 / 240.7 / 340.8 / 761.1 / 1016.8 µs, 20328 qps | 0 / 0 errors |
| `update-disjoint` | 50.9 / 238.2 / 340.8 / 754.6 / 1003.9 µs, 19712 qps | 46.5 / 240.6 / 342.7 / 752.6 / 1020.0 µs, 20218 qps | 0 / 0 errors |
| `update-disjoint-again` | 35.5 / 240.7 / 342.2 / 760.4 / 1038.7 µs, 19698 qps | 37.7 / 237.7 / 342.4 / 752.8 / 1031.8 µs, 20263 qps | 0 / 0 errors |
| `insert-omitted` | 45.7 / 237.2 / 339.7 / 759.2 / 1019.1 µs, 20214 qps | 38.6 / 237.8 / 338.7 / 750.6 / 1033.6 µs, 20381 qps | 0 / 0 errors |
| `select-hot` | 58.0 / 307.8 / 429.3 / 933.2 / 1232.8 µs, 16551 qps | 61.8 / 310.4 / 434.1 / 909.6 / 1222.5 µs, 16492 qps | 0 / 0 errors |
| `ping` | 36.7 / 215.7 / 325.8 / 764.3 / 1012.5 µs, 20958 qps | 40.8 / 221.4 / 328.2 / 770.0 / 1025.4 µs, 20580 qps | 0 / 0 errors |

**`c1-run3/result.json`** - load before `0.97 1.62 2.28 1/250 151094`, after `1.20 1.65 2.28 1/250 151493`; competing builds: none

| arm | B (`0552d55`): p0 / p25 / p50 / p95 / p99, throughput | A (`58198cb`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `update-hot` | 34.9 / 238.1 / 340.3 / 755.4 / 1014.3 µs, 20535 qps | 49.3 / 239.1 / 337.3 / 747.6 / 1013.5 µs, 20414 qps | 0 / 0 errors |
| `update-disjoint` | 42.1 / 240.8 / 344.1 / 748.5 / 1012.7 µs, 20040 qps | 45.5 / 239.1 / 341.9 / 760.4 / 1010.5 µs, 20315 qps | 0 / 0 errors |
| `update-disjoint-again` | 46.3 / 240.6 / 340.8 / 763.5 / 1024.9 µs, 20067 qps | 42.2 / 240.7 / 342.3 / 750.8 / 1027.8 µs, 20144 qps | 0 / 0 errors |
| `insert-omitted` | 36.4 / 238.6 / 338.7 / 733.2 / 983.4 µs, 20530 qps | 43.8 / 235.8 / 338.9 / 750.4 / 1028.7 µs, 20246 qps | 0 / 0 errors |
| `select-hot` | 56.3 / 310.0 / 434.7 / 913.2 / 1196.1 µs, 16520 qps | 59.3 / 310.7 / 434.7 / 908.3 / 1222.0 µs, 16572 qps | 0 / 0 errors |
| `ping` | 32.1 / 213.1 / 323.3 / 771.9 / 1040.1 µs, 20986 qps | 32.1 / 212.6 / 326.5 / 773.3 / 1063.6 µs, 20850 qps | 0 / 0 errors |

### Cell 2 raw (median across the five runs of each run's whole-arm summary, slow-mode blocks included)

| arm | B: p0 / p25 / p50 / p95 / p99, throughput | A: p0 / p25 / p50 / p95 / p99, throughput |
|---|---|---|
| `child-insert-fk` | 44.1 / 51.0 / 51.7 / 62.5 / 73.6 µs, 17896 qps | 43.2 / 50.5 / 51.5 / 64.0 / 75.1 µs, 17954 qps |
| `child-insert-fk-txn` | 103.0 / 110.6 / 112.1 / 129.6 / 138.5 µs, 8471 qps | 103.8 / 110.2 / 112.4 / 158.0 / 171.9 µs, 7958 qps |
| `child-insert-fk-txn:stmt` | 45.6 / 51.1 / 52.3 / 63.5 / 72.8 µs, 0 qps | 45.9 / 50.9 / 52.4 / 77.5 / 91.9 µs, 0 qps |
| `child-insert-plain` | 41.3 / 48.9 / 50.0 / 60.3 / 70.6 µs, 18528 qps | 42.2 / 48.9 / 50.1 / 74.3 / 88.5 µs, 17383 qps |
| `parent-update` | 40.8 / 50.3 / 51.0 / 62.0 / 72.7 µs, 18378 qps | 42.2 / 51.4 / 52.2 / 76.5 / 90.3 µs, 16744 qps |
| `child-update` | 41.7 / 50.8 / 51.4 / 63.1 / 73.7 µs, 18182 qps | 45.8 / 52.3 / 74.1 / 86.0 / 97.7 µs, 14394 qps |
| `child-update-again` | 40.6 / 50.6 / 51.2 / 62.3 / 73.4 µs, 18754 qps | 42.4 / 50.6 / 51.9 / 74.2 / 80.6 µs, 18324 qps |
| `select` | 59.3 / 64.2 / 64.9 / 80.5 / 90.7 µs, 14660 qps | 60.8 / 64.2 / 64.8 / 81.3 / 91.0 µs, 14679 qps |
| `ping` | 37.0 / 37.9 / 38.2 / 51.2 / 59.7 µs, 23092 qps | 39.6 / 45.3 / 46.6 / 54.7 / 65.3 µs, 20840 qps |

Per run, whole-arm p50 B / A in µs (a slow-mode block moves an arm's p50 by ~24 µs):

| arm | run 1 | run 2 | run 3 | run 4 | run 5 |
|---|---|---|---|---|---|
| `child-insert-fk` | 52.1 / 66.8 µs | 51.7 / 51.5 µs | 51.4 / 50.9 µs | 51.4 / 51.6 µs | 51.7 / 51.0 µs |
| `child-insert-fk-txn` | 112.1 / 121.6 µs | 146.7 / 112.4 µs | 111.8 / 110.9 µs | 110.8 / 112.2 µs | 112.7 / 117.5 µs |
| `child-insert-fk-txn:stmt` | 52.3 / 56.3 µs | 73.1 / 52.4 µs | 51.8 / 51.1 µs | 51.4 / 52.3 µs | 52.4 / 54.7 µs |
| `child-insert-plain` | 50.0 / 50.5 µs | 72.1 / 50.8 µs | 49.6 / 50.1 µs | 49.5 / 50.1 µs | 70.9 / 49.7 µs |
| `parent-update` | 51.3 / 50.6 µs | 51.7 / 52.2 µs | 50.9 / 74.8 µs | 51.0 / 74.1 µs | 50.4 / 50.4 µs |
| `child-update` | 51.5 / 50.9 µs | 53.7 / 74.1 µs | 51.1 / 75.5 µs | 51.4 / 75.4 µs | 50.6 / 50.9 µs |
| `child-update-again` | 51.4 / 52.3 µs | 51.6 / 51.9 µs | 51.2 / 51.1 µs | 51.0 / 52.3 µs | 50.7 / 50.8 µs |
| `select` | 64.5 / 64.1 µs | 64.9 / 73.3 µs | 65.5 / 65.4 µs | 64.3 / 64.8 µs | 65.0 / 64.7 µs |
| `ping` | 38.1 / 46.6 µs | 41.3 / 63.9 µs | 38.2 / 46.6 µs | 37.9 / 45.1 µs | 38.2 / 46.0 µs |

Load before each run:
- `c2-run1`: before `0.52 1.29 2.06 1/248 151859`, after `0.56 1.28 2.06 1/246 151860`, competing: none
- `c2-run2`: before `0.95 1.31 1.98 1/241 152233`, after `1.04 1.32 1.98 1/239 152236`, competing: none
- `c2-run3`: before `0.94 1.30 1.90 1/243 152592`, after `1.03 1.31 1.90 1/241 152593`, competing: none
- `c2-run4`: before `0.94 1.27 1.82 1/243 152953`, after `1.02 1.28 1.82 1/240 152956`, competing: none
- `c2-run5`: before `0.94 1.26 1.81 1/242 153012`, after `1.03 1.28 1.82 1/243 153018`, competing: none

### Cell 3 raw

- `c3-run1`: before `0.68 1.29 2.06 1/248 151907`, after `1.63 1.47 2.06 1/250 151999`, competing: none
- `c3-run2`: before `0.95 1.30 1.97 1/241 152294`, after `1.70 1.46 1.97 1/250 152390`, competing: none
- `c3-run3`: before `0.95 1.29 1.89 1/243 152652`, after `1.68 1.43 1.89 1/249 152743`, competing: none

Per K and kind, per row = transaction wall time / K; min, p25, median, p75 over every rep of the three runs, in µs per row.

| K | kind | reps | B: min / p25 / median / p75 | A: min / p25 / median / p75 |
|---|---|---|---|---|
| 1 | `fk` | 98,304 reps | 105.0 / 115.9 / 120.1 / 127.0 µs | 103.9 / 116.9 / 121.1 / 127.9 µs |
| 1 | `plain` | 98,304 reps | 102.2 / 113.4 / 116.9 / 121.8 µs | 104.8 / 115.0 / 118.7 / 124.1 µs |
| 64 | `fk` | 1,536 reps | 51.3 / 53.3 / 53.8 / 54.5 µs | 51.3 / 53.2 / 53.9 / 54.8 µs |
| 64 | `plain` | 1,536 reps | 50.0 / 52.4 / 52.8 / 53.3 µs | 51.2 / 52.7 / 53.2 / 53.8 µs |
| 1024 | `fk` | 96 reps | 51.4 / 52.1 / 52.5 / 53.4 µs | 51.2 / 52.6 / 53.1 / 53.7 µs |
| 1024 | `plain` | 96 reps | 50.1 / 51.4 / 51.8 / 52.2 µs | 50.2 / 51.5 / 51.8 / 52.3 µs |
| 4096 | `fk` | 24 reps | 52.2 / 52.9 / 53.4 / 54.0 µs | 51.8 / 52.1 / 52.5 / 54.0 µs |
| 4096 | `plain` | 24 reps | 50.3 / 51.1 / 51.3 / 52.5 µs | 50.8 / 51.2 / 51.5 / 54.9 µs |
| 16384 | `fk` | 15 reps | 56.2 / 56.7 / 57.2 / 59.8 µs | 52.1 / 52.8 / 54.3 / 73.0 µs |
| 16384 | `plain` | 15 reps | 50.4 / 50.9 / 51.7 / 54.2 µs | 52.0 / 52.3 / 53.3 / 56.9 µs |
