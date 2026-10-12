# BL's ruling 1, re-measured: deleting `Catalog::TouchPattern` removes the readers' own stall and halves their tail, and leaves the roll

**Thesis.** BL-S4's ruling 1 (a) deleted `Catalog::TouchPattern`, the logged
catalog write every repeated point `SELECT` used to make (`05cd011f`,
`v2.7.0-883-g05cd011f`). Measured against its parent state `3338cf0c`
(`v2.7.0-880-g3338cf0c`) in BL's own C1 cell (mixed, `relaxed`, four cores,
10 s, three placements, three runs each, interleaved), **it removes the
readers' own stall and not their tail**:

- **Reader service p99 falls by 99 %** in every placement: medians 283 /
  265 / 262 ms (segregated-no0 / spread / batch0) to 1.7 / 1.9 / 3.0 ms; the
  run ranges do not touch. A reader's statement no longer waits on the stream
  latch inside the statement: the cores 1-3 stream-latch wait falls from
  2.3-3.5 s per 10 s to 0-0.31 s.
- **Reader schedule p99 falls 28-54 % and stays at 277-295 ms** (medians 577 /
  621 / 409 ms to 277 / 288 / 295 ms). The segregated-no0 fall (-52 %) is
  inside the 3338cf0c runs' own spread (their p99 runs 443-637 ms, the two
  arms' ranges overlap); spread (-54 %) and batch0 (-28 %) clear it. **The
  schedule p99.9 and the maximum do not move** (ranges overlap in all three
  placements; medians 560-689 ms and 619-749 ms at 05cd011f).
- **What is left is the roll.** Core 0's stream-latch wait is the same at
  both commits (3.0-3.9 s of every 10 s in 16 of 18 runs; the two exceptions,
  1.45 and 2.07 s, are 3338cf0c batch0 runs); at `05cd011f` it is the only
  core with a wait of any size. A pure reader no longer appends, but it still waits behind
  a core-0 reactor blocked in the group-commit drain while a roll holds the
  latch (`results-bq-s1-segment-roll-v2.7.0-909-g4cf30270.md`).
- **The bulk writer is unaffected** (inserter medians -0.7 %, -0.7 %, +1.1 %,
  inside the 3338cf0c runs' own spread of 2.0-8.0k rows/s), and **the walker
  completes 1.5-1.65x as many statements** in the same 10 s.

**This is the first measurement of this pair with this driver, and the
baseline for any later one.** BL's order recorded ruling 1's effect on the
committed build as "not measured" (the re-measurement stopped at BL's pause
with 2 of 18 cells taken); `docs/inflight/known-gaps.md`'s BL entry says "the
stall has not been re-measured since" and describes the engine with both
couplings. This file is that re-measurement, taken by BQ-S1 under BQ-Q1 (a).
No earlier number with this driver exists: `results-bl-s1-placement-premise-v2.7.0-857-g3552a03c.md`
used the driver before BQ-S1 (a reader's service time then included building
its SQL text and retryable conflicts counted as errors), so a delta against
it would price the driver change as well. PostgreSQL is not part of this file:
the floor was not measured for this shape.

- **Order**: `instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`
  (§6 ruling 1; "BL paused 2026-10-12"); owed by
  `instructions/v3.0.0/workorder-bq-wal-segment-roll.md` BQ-Q1 (a).
- **Driver**: `tools/bl_mixed.py` at `4cf30270` (the file the matrix ran;
  identical to the commit's), run by the BQ matrix driver `bq_s1_matrix.py`
  (phase `bl`) on the same host session as BQ-S1's matrix. Flags are the
  driver's own docstring; `bench/docs/` is closed (`bench/README.md`).
- **Raw output**: `archive/bl-ruling1-remeasure-v2.7.0-909-g4cf30270/` — per
  run the driver JSON (gzip), the server log and the console log; the matrix
  driver and `hoststamps.log`; the table scripts.
- **Companion**: `results-bq-s1-segment-roll-v2.7.0-909-g4cf30270.md` (the
  roll itself, measured with instruments these two older binaries lack).

| Item | Value |
|---|---|
| Executed | 2026-10-12, **12:38-12:45 UTC** (18 cells, `hoststamps.log`; BQ's matrix followed at 12:45-13:29). **This file was written 2026-10-12 ~14:40 UTC from the existing data; nothing was run in the session that wrote it** |
| Engines measured | **BL's own builds at those two commits, not BQ's instrument build**, copied to `/home/cdkbs/bench-runs/bl/bin/kds_server.<sha>` and run by the BQ matrix driver: |
| `3338cf0c` | `v2.7.0-880-g3338cf0c`, "Merge origin/main into worktree-bl-session-balancer" (BL-S1..S3, `Catalog::TouchPattern` present), committed 05:56:44 UTC. **sha256 `a3595856cf0f6f06cc88759b6721d571c006fd86c9ff7fd66136fe4760c2d480`**; copy mtime 06:14:53; its source, `bl-session-balancer/build-release/kds_server`, was built at 06:14:34 (18 min *after* the commit; that branch has no later commit) and still hashes the same |
| `05cd011f` | `v2.7.0-883-g05cd011f`, "BL-S4 ruling 1 (a): delete Catalog::TouchPattern", committed 06:47:29 UTC. **sha256 `891d90ea409f99e7b46a0e32d8a6af3d2fee4138decad70cd33a0a03c7c2c6ed`**; copy mtime 06:47:51; its source, `bl-touchpattern-delete/build-release/kds_server`, was built at **06:33:48 — 14 min *before* the commit**, and still hashes the same |
| What differs between them | `3338cf0c` is an ancestor of `05cd011f`: two `CLAUDE.md` commits and the deletion (8 files under `src/` and `include/`, +41/-63 lines). **The `05cd011f` binary predates its commit**; whether the tree built at 06:33:48 equals the committed tree could not be established without a rebuild, which this session may not run. That the deletion is in the binary is shown by the measurement itself (the reader service tail it removes). Tree cleanliness at either build is not recorded |
| Binaries copied? | Yes: each server started from a copy; `bin_sha256` in every JSON is the hash above (9 runs each) |
| Build | Release (`CMAKE_BUILD_TYPE:STRING=Release` in both worktrees' `build-release/CMakeCache.txt`) |
| Device | `/dev/root`, **ext4** (`/dev/nvme0n1p1` on `/`, `rw,relatime,discard,errors=remount-ro,commit=30`), an Azure VM's NVMe disk; data file and WAL under the run's work directory on it; not tmpfs (recorded per run) |
| Host | 8 vCPU (AMD EPYC 9V74), Linux 7.0.0-1014-azure; **servers on CPUs 0,2,4,6, clients on 1,3,5,7 — the reactors' SMT siblings** |
| Server configuration | `cores = 4`, `durability = relaxed`, `buffer_pool_frames = 65536` (BL-S1's own pool, so the pair compares), `log_level = info`, `reactor_cpus = 0,2,4,6`; every other key default (64 MiB segments, 1 MiB ring, checkpoint cadence 5000 ms, BL-S3's balancer live in both) |
| Cells | 3 placements x 2 commits x 3 runs = 18, measured 10 s after a 2 s warm-up; one fresh server and data file per run |
| Baseline | `3338cf0c` is the baseline and `05cd011f` the measured state: the delta is `05cd011f` against `3338cf0c`. No earlier number with this driver exists. The PostgreSQL floor was not measured for this shape |
| Suite / overhead | Suite not executed for this file. Overhead of ruling 1: not measured by this file (BL's overhead A/B is owed at BL's close) |

## 0. The cell, and what "placement" means here

C1 is BL-S1's mixed cell: one core is shared by a **walker** (an unindexed
one-row `SELECT` over 10,000 wide rows, closed loop), an **inserter**
(1,024-row `INSERT` statements, closed loop) and six open-loop **pk point
readers** at 400 statements/s in total. The flags are `--cores 4 --arm mixed
--readers 6 --rate 400 --placement P --durability relaxed --seconds 10
--warmup 2 --frames 65536 --log-level info --ctl-core 0 --server-cpus
0,2,4,6 --client-cpus 1,3,5,7`, `P` in `segregated-no0`, `batch0`, `spread`.
`bl_point` and `bl_walk` hold 10,000 rows each (`--point-rows`,
`--walk-rows`).

**The placement name is a starting placement, not a held one.** BL-S3's
balancer is live in both binaries and moved sessions in every run (2-31 moves
in the window; `per_core_valid` is false in all 18 runs), so "readers on
cores with no batch session" is not what any cell measures; the table of
moves is in Appendix C. The same holds for both arms, which is what keeps
the comparison fair.

**Interleaving.** For each run index and each placement the two commits ran
back to back (`3338cf0c` first, always), the next placement after; three
passes. The fixed order inside a pair is an order effect this design cannot
separate from the commit: the host's 1-minute load was 1.38-2.47 before and
1.81-2.72 after each run (the matrix's gate required under 2.5 before a cell).

**Latency.** *Schedule* latency is taken from the instant the open-loop
schedule said to send; *service* latency from the send. A table cell is the
median of the three runs' percentile; every run is in Appendix A. *Spread*
is the maximum minus the minimum of three runs.

**The row-set sweep (200 / 1K / 10K) was not run.** The cell's relations are
fixed at 10,000 rows, and what it prices is a stall of the stream latch,
which is a function of log bytes and the device, not of a relation's
cardinality. Nothing at 200 or 1,000 rows was measured.

## 1. Reader service latency: the readers' own stall is gone

The deleted write made every repeated point `SELECT` append a record, so a
reader's *statement* waited on the stream latch a roll was holding. At
`05cd011f` the service tail is two orders of magnitude shorter:

| placement | commit | n (3 runs) | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| segregated-no0 | 3338cf0c | 12018 | 59.4 µs | 122 µs | 137 µs | 371 µs | 283 ms | 589 ms | 591 ms |
| segregated-no0 | 05cd011f | 12018 | 58.4 µs | 113 µs | 128 µs | 214 µs | 1.7 ms | 284 ms | 748 ms |
| spread | 3338cf0c | 11914 | 59.4 µs | 121 µs | 140 µs | 375 µs | 265 ms | 702 ms | 732 ms |
| spread | 05cd011f | 12018 | 61.0 µs | 114 µs | 126 µs | 334 µs | 1.9 ms | 283 ms | 619 ms |
| batch0 | 3338cf0c | 11956 | 57.9 µs | 121 µs | 141 µs | 472 µs | 262 ms | 495 ms | 523 ms |
| batch0 | 05cd011f | 12018 | 60.3 µs | 114 µs | 128 µs | 192 µs | 3.0 ms | 295 ms | 749 ms |

The service p99 falls 99 % (medians 283 → 1.7 ms, 265 → 1.9 ms, 262 → 3.0
ms), the p50 by 7-10 %, and the service p99.9 by 40-60 % (to medians of
283-295 ms, runs 266-327 ms: the statements that are sent into a reactor
blocked in the drain). In the
comparison below the service p50, p99 and p99.9 have disjoint run ranges in
every placement; the p99's fall is by far the largest, and it is far outside
the 3338cf0c runs' own spread (18-41 ms). One `05cd011f` spread run (r3) has
a service p99 of 40 ms, the only service p99 above 4 ms at that commit:

| placement | figure | 3338cf0c (r1 / r2 / r3) | spread of the 3338cf0c runs (max - min) | 05cd011f (r1 / r2 / r3) | spread of the 05cd011f runs | median 05cd011f / median 3338cf0c | ranges overlap? |
|---|---|---|---|---|---|---|---|
| segregated-no0 | schedule p50 | 244 µs / 236 µs / 232 µs | 12.4 µs | 207 µs / 202 µs / 210 µs | 8.6 µs | 0.8803 (-12 %) | no |
| segregated-no0 | schedule p99 | 637 ms / 577 ms / 443 ms | 194 ms | 277 ms / 240 ms / 455 ms | 215 ms | 0.4804 (-52 %) | yes |
| segregated-no0 | schedule p99.9 | 746 ms / 719 ms / 532 ms | 214 ms | 688 ms / 425 ms / 723 ms | 298 ms | 0.9577 (-4 %) | yes |
| segregated-no0 | schedule max | 769 ms / 748 ms / 542 ms | 227 ms | 748 ms / 485 ms / 753 ms | 268 ms | 0.9990 (-0 %) | yes |
| segregated-no0 | service p50 | 137 µs / 140 µs / 136 µs | 3.8 µs | 128 µs / 125 µs / 130 µs | 5.3 µs | 0.9322 (-7 %) | no |
| segregated-no0 | service p99 | 283 ms / 247 ms / 288 ms | 40.7 ms | 817 µs / 1.7 ms / 3.3 ms | 2.5 ms | 0.0059 (-99 %) | no |
| segregated-no0 | service p99.9 | 696 ms / 589 ms / 532 ms | 164 ms | 284 ms / 270 ms / 293 ms | 22.7 ms | 0.4825 (-52 %) | no |
| segregated-no0 | service max | 769 ms / 591 ms / 539 ms | 229 ms | 748 ms / 485 ms / 753 ms | 268 ms | 1.2649 (+26 %) | yes |
| spread | schedule p50 | 242 µs / 237 µs / 242 µs | 5.5 µs | 200 µs / 209 µs / 205 µs | 9.1 µs | 0.8470 (-15 %) | no |
| spread | schedule p99 | 621 ms / 318 ms / 671 ms | 354 ms | 248 ms / 288 ms / 290 ms | 42.0 ms | 0.4636 (-54 %) | no |
| spread | schedule p99.9 | 707 ms / 377 ms / 760 ms | 383 ms | 560 ms / 726 ms / 401 ms | 326 ms | 0.7911 (-21 %) | yes |
| spread | schedule max | 733 ms / 384 ms / 770 ms | 386 ms | 619 ms / 786 ms / 457 ms | 329 ms | 0.8450 (-16 %) | yes |
| spread | service p50 | 138 µs / 140 µs / 141 µs | 3.3 µs | 126 µs / 128 µs / 126 µs | 2.4 µs | 0.9012 (-10 %) | no |
| spread | service p99 | 265 ms / 271 ms / 253 ms | 18.2 ms | 1.9 ms / 1.3 ms / 40.4 ms | 39.1 ms | 0.0070 (-99 %) | no |
| spread | service p99.9 | 702 ms / 377 ms / 760 ms | 383 ms | 266 ms / 283 ms / 322 ms | 55.7 ms | 0.4035 (-60 %) | no |
| spread | service max | 732 ms / 384 ms / 770 ms | 386 ms | 619 ms / 786 ms / 457 ms | 329 ms | 0.8450 (-16 %) | yes |
| batch0 | schedule p50 | 227 µs / 245 µs / 246 µs | 18.6 µs | 215 µs / 207 µs / 209 µs | 7.9 µs | 0.8530 (-15 %) | no |
| batch0 | schedule p99 | 687 ms / 345 ms / 409 ms | 343 ms | 295 ms / 293 ms / 337 ms | 43.5 ms | 0.7205 (-28 %) | no |
| batch0 | schedule p99.9 | 776 ms / 397 ms / 498 ms | 379 ms | 689 ms / 695 ms / 560 ms | 135 ms | 1.3842 (+38 %) | yes |
| batch0 | schedule max | 786 ms / 414 ms / 523 ms | 371 ms | 749 ms / 754 ms / 590 ms | 165 ms | 1.4319 (+43 %) | yes |
| batch0 | service p50 | 135 µs / 144 µs / 141 µs | 8.8 µs | 133 µs / 127 µs / 128 µs | 6.4 µs | 0.9084 (-9 %) | no |
| batch0 | service p99 | 262 ms / 274 ms / 256 ms | 17.8 ms | 3.0 ms / 2.9 ms / 3.6 ms | 656 µs | 0.0115 (-99 %) | no |
| batch0 | service p99.9 | 776 ms / 394 ms / 495 ms | 382 ms | 295 ms / 286 ms / 327 ms | 41.4 ms | 0.5950 (-40 %) | no |
| batch0 | service max | 786 ms / 414 ms / 523 ms | 371 ms | 749 ms / 754 ms / 590 ms | 165 ms | 1.4319 (+43 %) | yes |

("Ranges overlap?" is yes when the three runs of one arm are not entirely
below or above the three of the other.)

## 2. Reader schedule latency: halved, not removed

| placement | commit | n (3 runs) | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| segregated-no0 | 3338cf0c | 12018 | 134 µs | 207 µs | 236 µs | 295 ms | 577 ms | 719 ms | 748 ms |
| segregated-no0 | 05cd011f | 12018 | 129 µs | 186 µs | 207 µs | 50.5 ms | 277 ms | 688 ms | 748 ms |
| spread | 3338cf0c | 11914 | 140 µs | 204 µs | 242 µs | 306 ms | 621 ms | 707 ms | 733 ms |
| spread | 05cd011f | 12018 | 116 µs | 184 µs | 205 µs | 131 ms | 288 ms | 560 ms | 619 ms |
| batch0 | 3338cf0c | 11956 | 135 µs | 206 µs | 245 µs | 261 ms | 409 ms | 498 ms | 523 ms |
| batch0 | 05cd011f | 12018 | 119 µs | 188 µs | 209 µs | 115 ms | 295 ms | 689 ms | 749 ms |

Schedule p99 falls from 577 / 621 / 409 ms to 277 / 288 / 295 ms. **Against
the noise floor** (the 3338cf0c runs' own spread of 194 / 354 / 343 ms at p99):
the segregated-no0 fall (-52 %) is inside it, and its two arms' ranges overlap
(443 ms against 455 ms); spread (-54 %) and batch0 (-28 %) have disjoint
ranges, batch0's by 8 ms (345 ms against 337 ms). The p99.9 and the maximum
are not different from before in any placement (all ranges overlap), and
batch0's median p99.9 is higher (+38 %, inside a 379 ms spread). The p50
falls 12-15 % with disjoint ranges, a real but small gain, consistent with
the shorter common path (no logged write per read).

The p99 that remains is a queue, not a statement: a reader's service p99 is
1.7-3.0 ms but its schedule p99 is 277-295 ms, because a reader on core 0
waits for the reactor while the group-commit drain holds it, and each stalled
reader's following ~15 ms-spaced scheduled sends are charged as late (a 300
ms stall makes ~20 of them late).

## 3. Where the wait went

The stream-latch wait per core, over each run's window (`m1 - m0`, an absent
key 0). At `3338cf0c` the cores that run readers wait inside their own
statements (cores 1-3 together: 2.3-3.5 s per 10 s); at `05cd011f` they do
not (0-0.31 s), and core 0's wait, the drain's, is unchanged:

| placement | commit | run | core 0: stream-latch wait | core 1 | core 2 | core 3 | cores 1-3 together | window |
|---|---|---|---|---|---|---|---|---|
| segregated-no0 | 3338cf0c | r1 | 3.89 s | 0.00 s | 3.19 s | 0.00 s | 3.19 s | 9.7 s |
| segregated-no0 | 3338cf0c | r2 | 3.61 s | 1.34 s | 0.00 s | 1.48 s | 2.82 s | 9.7 s |
| segregated-no0 | 3338cf0c | r3 | 3.80 s | 3.49 s | 0.05 s | 0.00 s | 3.54 s | 10.0 s |
| segregated-no0 | 05cd011f | r1 | 3.90 s | 0.00 s | 0.00 s | 0.31 s | 0.31 s | 10.0 s |
| segregated-no0 | 05cd011f | r2 | 3.23 s | 0.00 s | 0.00 s | 0.11 s | 0.11 s | 9.7 s |
| segregated-no0 | 05cd011f | r3 | 3.54 s | 0.00 s | 0.00 s | 0.24 s | 0.24 s | 10.0 s |
| spread | 3338cf0c | r1 | 3.44 s | 0.00 s | 0.00 s | 2.28 s | 2.28 s | 9.8 s |
| spread | 3338cf0c | r2 | 3.90 s | 0.00 s | 3.40 s | 0.00 s | 3.40 s | 10.0 s |
| spread | 3338cf0c | r3 | 3.73 s | 0.00 s | 3.47 s | 0.00 s | 3.47 s | 9.7 s |
| spread | 05cd011f | r1 | 3.05 s | 0.00 s | 0.00 s | 0.17 s | 0.17 s | 9.7 s |
| spread | 05cd011f | r2 | 3.61 s | 0.00 s | 0.07 s | 0.22 s | 0.29 s | 9.8 s |
| spread | 05cd011f | r3 | 3.25 s | 0.00 s | 0.00 s | 0.00 s | 0.00 s | 9.8 s |
| batch0 | 3338cf0c | r1 | 2.07 s | 0.00 s | 0.13 s | 3.37 s | 3.50 s | 10.0 s |
| batch0 | 3338cf0c | r2 | 3.18 s | 0.34 s | 2.64 s | 0.00 s | 2.98 s | 9.9 s |
| batch0 | 3338cf0c | r3 | 1.45 s | 2.91 s | 0.00 s | 0.25 s | 3.16 s | 9.7 s |
| batch0 | 05cd011f | r1 | 3.68 s | 0.00 s | 0.00 s | 0.00 s | 0.00 s | 9.7 s |
| batch0 | 05cd011f | r2 | 3.67 s | 0.00 s | 0.00 s | 0.25 s | 0.25 s | 9.7 s |
| batch0 | 05cd011f | r3 | 3.46 s | 0.00 s | 0.00 s | 0.00 s | 0.00 s | 9.7 s |

Core 0's 3.0-3.9 s per 10 s is 30-39 % of the window in 16 of 18 runs; the
two exceptions (1.45 s and 2.07 s) are 3338cf0c batch0 runs, where a roller
on core 0 would not wait on its own drain (the cause is not in these data).
These older binaries have no `wal_roll_*` or drain counters, so the drain's
share of core 0's wait and the roll holds are not in this data; they are
BQ-S1's (the 30 s C1 cell at `4cf30270`: core 0's drain is 100 % of its
stream-latch wait and 34-40 % of the window).

## 4. The bulk writer and the walker

The inserter's rate is the guard that the deletion cost the bulk writer
nothing; the walker is the closed-loop statement that used to pay for the
logged write:

| placement | median rows/s 3338cf0c | median rows/s 05cd011f | delta | 3338cf0c spread |
|---|---|---|---|---|
| segregated-no0 | 174,284 | 173,091 | -0.7 % | 2,020 |
| spread | 175,543 | 174,378 | -0.7 % | 6,215 |
| batch0 | 173,511 | 175,378 | +1.1 % | 8,043 |

| placement | commit | inserter rows/s (r1 / r2 / r3) | median | spread (max - min) |
|---|---|---|---|---|
| segregated-no0 | 3338cf0c | 174,284 / 175,221 / 173,201 | 174,284 | 2,020 (1.2 %) |
| segregated-no0 | 05cd011f | 171,929 / 173,091 / 175,262 | 173,091 | 3,333 (1.9 %) |
| spread | 3338cf0c | 175,543 / 180,586 / 174,371 | 175,543 | 6,215 (3.5 %) |
| spread | 05cd011f | 174,378 / 173,980 / 174,524 | 174,378 | 544 (0.3 %) |
| batch0 | 3338cf0c | 171,716 / 179,759 / 173,511 | 173,511 | 8,043 (4.6 %) |
| batch0 | 05cd011f | 175,378 / 174,375 / 176,142 | 175,378 | 1,767 (1.0 %) |

All three differences are inside the 3338cf0c spread. The walker, which is
unindexed and closed loop, completes more statements in the same window:

| placement | walker statements completed 3338cf0c (r1 / r2 / r3) | 05cd011f (r1 / r2 / r3) | median ratio |
|---|---|---|---|
| segregated-no0 | 5,431 / 4,877 / 5,443 | 6,668 / 8,296 / 10,274 | 1.53x |
| spread | 5,267 / 5,435 / 4,959 | 7,695 / 7,876 / 8,265 | 1.50x |
| batch0 | 5,391 / 5,564 / 5,307 | 10,032 / 8,237 / 8,916 | 1.65x |

The walker's own p50 is 1.2 ms in 14 of the 18 runs and 0.95-1.1 ms in four
`05cd011f` runs (Appendix C).

## 5. Accounting for the waits

A reader's schedule latency at the p99 is a sum of, at `3338cf0c`: the
client and socket round trip and the read (service p50 137-141 µs); **its own
append waiting on the stream latch during a roll** (the service p99,
262-283 ms; cores 1-3 together wait 2.3-3.5 s per 10 s); the **reactor queue**
behind core 0's drain; and the schedule pile-up behind a stalled reader. At
`05cd011f` the second term is gone (service p99 0.8-40 ms, median 1.7-3.0)
and the third and fourth remain. Lock or conflict waits: `errors` and
`refusals` are 0 in all 18 runs. Commit/fsync of the reader: not applicable
(`relaxed`, and a read). Write-statement waits of the inserter: p50 3.1-3.3
ms, max 0.4-0.8 s (a roll) at both commits.

## 6. What the run teaches

1. **The read path's logged write was one of two couplings, and it was the
   cheaper one to cut.** Removing it takes the readers' own wait out
   completely and moves their tail's cause, not its length: the tail is now
   the drain's, which is on the other side of the same latch. Ruling 1 is
   necessary for BL-S4's partition and does not by itself deliver it. BL's
   recorded variant experiment (a measurement build with the call removed and
   the readers kept off core 0 and off the batch core) reached a schedule
   p99 of 19-125 ms; no cell here keeps a reader off core 0.
2. **Readers end up on core 0.** At the end of every one of the nine
   `05cd011f` runs exactly one reader is on core 0 (`cores_end`), and at
   `3338cf0c` one to four are — including `segregated-no0`, whose starting
   placement puts no reader there (3, 3 and 2 readers at the end of its three
   runs). With the drain on core 0, the tail is core 0's whenever a reader
   is there.
3. **Where the stall lives changed the p99.9 not at all.** The service p99.9
   falls 40-60 % but its runs stay at 266-327 ms, and the schedule maximum is
   619-749 ms at `05cd011f`: a roll still holds the latch for 0.3-0.8 s at
   `relaxed` (BQ-S1), and anything sent into core 0 while it does waits it out.
4. **The headline numbers in `known-gaps.md`'s BL entry** ("0.25-0.8 s for
   point readers on every core") describe `3338cf0c`; at `05cd011f` the
   service tail is 1.7-3.0 ms at p99 and the schedule tail 277-295 ms at p99
   with the same maximum, and the "every core" part holds only for core 0.

## 7. What this file does not establish

- **Binary provenance for `05cd011f`** (above): built 14 min before its
  commit; the deletion is evidenced by the effect, not by a rebuild.
- **Only 10 s per run**, BL-S1's own length: about 11-12 rolls per run (an
  inference from BQ's C1 at `relaxed`, 33-37 rolls per 30 s; these binaries
  log none), so the p99.9 and the maximum rest on a dozen stalls.
- **The balancer was live**; placement is a start, not a held state, and the
  order within each pair is fixed (§0). A pairing effect of 3338cf0c running
  first cannot be separated from the commit.
- **Six readers, one inserter, one walker, one filesystem, one host, clients
  on the reactors' SMT siblings**, `relaxed` only (not `group`, not `strict`).
- **No kernel-level view and no roll counters**: the drain's share of core 0's
  wait is read from BQ's instrument build at a different commit, not from
  these binaries.
- **Overhead of the deletion** is not measured here; **no suite was run**.

## Appendix A. Every run's percentiles

Schedule latency:

| placement | commit | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|---|
| segregated-no0 | 3338cf0c | r1 | 4006 | 135 µs | 208 µs | 244 µs | 332 ms | 637 ms | 746 ms | 769 ms |
| segregated-no0 | 3338cf0c | r2 | 4006 | 134 µs | 207 µs | 236 µs | 295 ms | 577 ms | 719 ms | 748 ms |
| segregated-no0 | 3338cf0c | r3 | 4006 | 122 µs | 196 µs | 232 µs | 286 ms | 443 ms | 532 ms | 542 ms |
| segregated-no0 | 05cd011f | r1 | 4006 | 113 µs | 183 µs | 207 µs | 27.9 ms | 277 ms | 688 ms | 748 ms |
| segregated-no0 | 05cd011f | r2 | 4006 | 131 µs | 186 µs | 202 µs | 50.5 ms | 240 ms | 425 ms | 485 ms |
| segregated-no0 | 05cd011f | r3 | 4006 | 129 µs | 194 µs | 210 µs | 185 ms | 455 ms | 723 ms | 753 ms |
| spread | 3338cf0c | r1 | 4006 | 140 µs | 203 µs | 242 µs | 306 ms | 621 ms | 707 ms | 733 ms |
| spread | 3338cf0c | r2 | 3902 | 149 µs | 204 µs | 237 µs | 253 ms | 318 ms | 377 ms | 384 ms |
| spread | 3338cf0c | r3 | 4006 | 129 µs | 205 µs | 242 µs | 346 ms | 671 ms | 760 ms | 770 ms |
| spread | 05cd011f | r1 | 4006 | 124 µs | 184 µs | 200 µs | 131 ms | 248 ms | 560 ms | 619 ms |
| spread | 05cd011f | r2 | 4006 | 109 µs | 186 µs | 209 µs | 116 ms | 288 ms | 726 ms | 786 ms |
| spread | 05cd011f | r3 | 4006 | 116 µs | 182 µs | 205 µs | 162 ms | 290 ms | 401 ms | 457 ms |
| batch0 | 3338cf0c | r1 | 4006 | 135 µs | 200 µs | 227 µs | 359 ms | 687 ms | 776 ms | 786 ms |
| batch0 | 3338cf0c | r2 | 3944 | 124 µs | 207 µs | 245 µs | 261 ms | 345 ms | 397 ms | 414 ms |
| batch0 | 3338cf0c | r3 | 4006 | 139 µs | 206 µs | 246 µs | 261 ms | 409 ms | 498 ms | 523 ms |
| batch0 | 05cd011f | r1 | 4006 | 130 µs | 192 µs | 215 µs | 88.7 ms | 295 ms | 689 ms | 749 ms |
| batch0 | 05cd011f | r2 | 4006 | 119 µs | 183 µs | 207 µs | 115 ms | 293 ms | 695 ms | 754 ms |
| batch0 | 05cd011f | r3 | 4006 | 99.7 µs | 188 µs | 209 µs | 176 ms | 337 ms | 560 ms | 590 ms |

Service latency:

| placement | commit | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|---|
| segregated-no0 | 3338cf0c | r1 | 4006 | 57.5 µs | 122 µs | 137 µs | 322 µs | 283 ms | 696 ms | 769 ms |
| segregated-no0 | 3338cf0c | r2 | 4006 | 59.4 µs | 122 µs | 140 µs | 433 µs | 247 ms | 589 ms | 591 ms |
| segregated-no0 | 3338cf0c | r3 | 4006 | 60.1 µs | 119 µs | 136 µs | 371 µs | 288 ms | 532 ms | 539 ms |
| segregated-no0 | 05cd011f | r1 | 4006 | 58.4 µs | 112 µs | 128 µs | 192 µs | 817 µs | 284 ms | 748 ms |
| segregated-no0 | 05cd011f | r2 | 4006 | 57.2 µs | 113 µs | 125 µs | 222 µs | 1.7 ms | 270 ms | 485 ms |
| segregated-no0 | 05cd011f | r3 | 4006 | 78.2 µs | 117 µs | 130 µs | 214 µs | 3.3 ms | 293 ms | 753 ms |
| spread | 3338cf0c | r1 | 4006 | 52.5 µs | 120 µs | 138 µs | 397 µs | 265 ms | 702 ms | 732 ms |
| spread | 3338cf0c | r2 | 3902 | 62.5 µs | 122 µs | 140 µs | 375 µs | 271 ms | 377 ms | 384 ms |
| spread | 3338cf0c | r3 | 4006 | 59.4 µs | 121 µs | 141 µs | 283 µs | 253 ms | 760 ms | 770 ms |
| spread | 05cd011f | r1 | 4006 | 76.8 µs | 114 µs | 126 µs | 219 µs | 1.9 ms | 266 ms | 619 ms |
| spread | 05cd011f | r2 | 4006 | 58.4 µs | 114 µs | 128 µs | 334 µs | 1.3 ms | 283 ms | 786 ms |
| spread | 05cd011f | r3 | 4006 | 61.0 µs | 112 µs | 126 µs | 447 µs | 40.4 ms | 322 ms | 457 ms |
| batch0 | 3338cf0c | r1 | 4006 | 57.9 µs | 120 µs | 135 µs | 322 µs | 262 ms | 776 ms | 786 ms |
| batch0 | 3338cf0c | r2 | 3944 | 53.4 µs | 121 µs | 144 µs | 1.4 ms | 274 ms | 394 ms | 414 ms |
| batch0 | 3338cf0c | r3 | 4006 | 58.9 µs | 122 µs | 141 µs | 472 µs | 256 ms | 495 ms | 523 ms |
| batch0 | 05cd011f | r1 | 4006 | 60.3 µs | 118 µs | 133 µs | 481 µs | 3.0 ms | 295 ms | 749 ms |
| batch0 | 05cd011f | r2 | 4006 | 58.4 µs | 111 µs | 127 µs | 192 µs | 2.9 ms | 286 ms | 754 ms |
| batch0 | 05cd011f | r3 | 4006 | 72.2 µs | 114 µs | 128 µs | 188 µs | 3.6 ms | 327 ms | 590 ms |

## Appendix B. Host load before and after each run

| commit | cell | 1-min load before the run (r1 / r2 / r3) | 1-min load after the run (r1 / r2 / r3) | competing processes listed by the run's two host stamps (r1 / r2 / r3) |
|---|---|---|---|---|
| 05cd011f | batch0 | 2.12 / 2.22 / 2.29 | 2.33 / 2.28 / 2.46 | 0 / 0 / 0 |
| 05cd011f | segregated-no0 | 1.81 / 2.26 / 2.28 | 2.46 / 2.38 / 2.72 | 0 / 0 / 0 |
| 05cd011f | spread | 2.47 / 2.36 / 2.13 | 2.46 / 2.34 / 2.27 | 0 / 0 / 0 |
| 3338cf0c | batch0 | 2.46 / 2.38 / 2.38 | 2.51 / 2.22 / 2.39 | 0 / 0 / 0 |
| 3338cf0c | segregated-no0 | 1.38 / 2.42 / 2.34 | 1.81 / 2.58 / 2.28 | 0 / 0 / 0 |
| 3338cf0c | spread | 2.33 / 2.28 / 2.35 | 2.42 / 2.36 / 2.52 | 0 / 0 / 0 |

## Appendix C. Per run: the inserter, the walker, the balancer

| placement | commit | run | inserter rows/s | inserter statement p50 | p99 | max | walker statements completed (closed loop) | walker p50 | walker p99 | moves (run / in window) | per_core_valid | errors / refusals |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| segregated-no0 | 3338cf0c | r1 | 174,284 | 3.2 ms | 6.3 ms | 775 ms | 5,431 | 1.2 ms | 1.7 ms | 13 / 6 | False | 0 / 0 |
| segregated-no0 | 3338cf0c | r2 | 175,221 | 3.3 ms | 6.8 ms | 795 ms | 4,877 | 1.2 ms | 2.2 ms | 22 / 15 | False | 0 / 0 |
| segregated-no0 | 3338cf0c | r3 | 173,201 | 3.1 ms | 9.7 ms | 545 ms | 5,443 | 1.2 ms | 1.7 ms | 10 / 4 | False | 0 / 0 |
| segregated-no0 | 05cd011f | r1 | 171,929 | 3.1 ms | 10.7 ms | 762 ms | 6,668 | 1.2 ms | 1.9 ms | 15 / 6 | False | 0 / 0 |
| segregated-no0 | 05cd011f | r2 | 173,091 | 3.3 ms | 8.9 ms | 506 ms | 8,296 | 1.2 ms | 1.8 ms | 8 / 3 | False | 0 / 0 |
| segregated-no0 | 05cd011f | r3 | 175,262 | 3.3 ms | 8.6 ms | 766 ms | 10,274 | 951 µs | 1.4 ms | 16 / 9 | False | 0 / 0 |
| spread | 3338cf0c | r1 | 175,543 | 3.2 ms | 9.9 ms | 737 ms | 5,267 | 1.2 ms | 2.1 ms | 37 / 17 | False | 0 / 0 |
| spread | 3338cf0c | r2 | 180,586 | 3.1 ms | 4.2 ms | 395 ms | 5,435 | 1.2 ms | 2.1 ms | 29 / 10 | False | 0 / 0 |
| spread | 3338cf0c | r3 | 174,371 | 3.2 ms | 8.3 ms | 779 ms | 4,959 | 1.2 ms | 2.4 ms | 14 / 5 | False | 0 / 0 |
| spread | 05cd011f | r1 | 174,378 | 3.3 ms | 10.4 ms | 637 ms | 7,695 | 1.2 ms | 2.0 ms | 10 / 5 | False | 0 / 0 |
| spread | 05cd011f | r2 | 173,980 | 3.1 ms | 8.1 ms | 797 ms | 7,876 | 1.2 ms | 2.1 ms | 33 / 12 | False | 0 / 0 |
| spread | 05cd011f | r3 | 174,524 | 3.1 ms | 10.2 ms | 478 ms | 8,265 | 1.1 ms | 1.6 ms | 51 / 31 | False | 0 / 0 |
| batch0 | 3338cf0c | r1 | 171,716 | 3.1 ms | 9.6 ms | 793 ms | 5,391 | 1.2 ms | 1.9 ms | 30 / 15 | False | 0 / 0 |
| batch0 | 3338cf0c | r2 | 179,759 | 3.1 ms | 5.3 ms | 432 ms | 5,564 | 1.2 ms | 2.0 ms | 36 / 20 | False | 0 / 0 |
| batch0 | 3338cf0c | r3 | 173,511 | 3.1 ms | 11.6 ms | 537 ms | 5,307 | 1.2 ms | 2.1 ms | 21 / 8 | False | 0 / 0 |
| batch0 | 05cd011f | r1 | 175,378 | 3.2 ms | 9.7 ms | 766 ms | 10,032 | 956 µs | 1.7 ms | 24 / 8 | False | 0 / 0 |
| batch0 | 05cd011f | r2 | 174,375 | 3.2 ms | 9.5 ms | 773 ms | 8,237 | 1.2 ms | 1.9 ms | 21 / 2 | False | 0 / 0 |
| batch0 | 05cd011f | r3 | 176,142 | 3.1 ms | 9.7 ms | 596 ms | 8,916 | 1.1 ms | 1.6 ms | 12 / 4 | False | 0 / 0 |
