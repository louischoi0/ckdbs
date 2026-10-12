# BL-S1: the placement and priority premises, measured

**Thesis.** BL's two premises read differently. **Placement by load has a
small, real effect, on tails rather than throughput**: with sessions skewed
across cores, a balanced placement brings the per-core p99 spread from
1.12-1.78x down to 1.01-1.05x and the worst core's p99 down 2-25 % at
`relaxed`, and moves aggregate throughput by -10 % to +6 %, inside what this
host can show (its clients share physical cores with the reactors). At
`group` nothing separates the arms: commit latency is the sync's. **Priority
served by placement (BL-R2's partition) has no effect as the engine stands,
and the reason is not the reactor.** One bulk-inserting session stalls point
readers on every core for 0.25-0.8 s, wherever they are placed, through the
one WAL: a repeated point `SELECT` makes a logged catalog write (Waystone's
`TouchPattern`), and a segment roll holds the stream latch across a device
sync. With Waystone recording off and readers kept off core 0, the same
partition brings the readers' schedule p99 from about 500 ms to 0.57-0.76 ms
in two runs and to 41-102 ms in two more, where a checkpoint turn fell on a
reader core. So BL-S4 is worth building only after that coupling is cut, which
is outside BL and needs a ruling. A move costs a statement 28-39 us at its
p50, and BL-S3's balancer, started from a skewed placement, recovers the
balanced placement's throughput and most of its tail fairness (client p99
spread 1.49-2.11x down to 1.10-1.15x) at under six moves a second across
sixteen sessions.

- **Order**: `instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`,
  BL-S1 (§5.1) and the inputs of BL-Q3, BL-Q14 and BL-R4's margin.
- **Drivers** (this file's commit): `tools/ba_census.py` (`--placement`
  kernel / balanced / skew, the `pkmix` shape, `--extra-conf`, mid-span
  gauges), `tools/bl_mixed.py` (the mixed cell and the four-core placements),
  `tools/bl_move_cost.py` (the move-cost cell). Analysis scripts and the
  matrix scripts are in the archive.
- **Raw output**: `archive/bl-s1-v2.7.0-857-g3552a03c/` - one JSON per run,
  per directory below, the console logs, and the two measurement-only patches.

| Item | Value |
|---|---|
| Executed | 2026-10-12, 02:55-05:01 UTC (file times of the logs) |
| Worktree / branch | `bl-session-balancer`, `worktree-bl-session-balancer` |
| Binaries | **`3552a03c`** (`v2.7.0-857-g3552a03c`, BL-S1 + BL-S2): the skew cells, the CPU-bound skew cells, and the convergence base, sha256 `3b4b8bf0...a350c8`. **Earlier, uncommitted states of the same worktree** for the cells measured before that commit, each named by its copy's hash: `bls1-slice` (`a3208be7` + BL-S1's first draft + the slice override; the mixed cells) `dd94d562...`; `bls3` (BL-S1 and BL-S2 as reviewed, before `SetCores`; the attribution cells) `4db6e5b0...`; `probe-moves` (BL-S3's draft placer files present but unwired + the move probe) `8e274baf...`; `bls3-balancer` (BL-S3's first rule, §6) `4ba68d92...`; `bls3-balancer-v2` (BL-S3 as committed with this file) `2c264a8c...`. Every server ran from a copy |
| Measurement-only patches | `slice-override.patch` (reads `KDS_BL_PAGES_PER_SLICE` into `SelectRun`'s slice) and `probe-moves.patch` (`KDS_BL_PROBE_MOVES`: every head moves to the next core); never committed, archived |
| Build | Release, target `kds_server` |
| Host / device | 8 vCPU (4 physical cores, SMT pairs 0/1, 2/3, 4/5, 6/7), Linux 7.0.0-1014-azure; data files under `/home/cdkbs/bench-runs/bl/` on `/dev/root`, **ext4**, not tmpfs (recorded per run) |
| Pinning | `cores = 2`: reactors 0,2, clients 4-7 (BA-Q2's clean map). `cores = 4`: reactors 0,2,4,6, clients 1,3,5,7 - **the SMT siblings of the reactors, so every four-core cell is indicative only** |
| Host load | A gate before every run waited for no compiler, build or test process of any session and a 1-minute load under 2.5 (`quiet_gate.sh`); `host_before`/`host_after` are in every JSON, and no run recorded a competing process. The first attempt at these matrices, started under two other sessions' `ctest -j8` (load 13-20), was discarded whole |
| Suite | Debug, at the BL-S2 state that became `3552a03c`: 3596 of 3596 (label `heap-suspended` excluded, 5 disabled) |

## 1. Skew cells: does placement by load matter (gate (i))

`ba_census.py --shape pkmix` (each statement a pk point read or, with even
odds, a pk `UPDATE` of the client's own stripe), 16,000 rows, 8 s after 2 s
of warm-up, one fresh server per run, arms interleaved per run. `kernel`
is the kernel's `SO_REUSEPORT` hash; `balanced` reconnects until client *i*
is on core *i mod cores*; `skew` is two thirds balanced and the last third
on core 1 - arm D's drift at its worst. **Three runs per arm, not ten**: at
about one run a minute behind the load gate, the order's ten were cut to
three after the third round, and a fourth `kernel` run of one cell is kept.
Per-core p99 is the median of that core's clients' p99.

| cores x sessions, durability | arm | stmt/s (mean ± sd) | p99 us | worst-core p99 us | per-core p99 spread | placement (one run) |
|---|---|---|---|---|---|---|
| 2 x 16, relaxed | balanced | 81,951 ± 137 | 370 | 374 | 1.01 | 8/8 |
| | kernel | 77,499 ± 2,818 | 416 | 444 | 1.58 | 9/7 |
| | skew | 79,756 ± 914 | 387 | 380 | 1.22 | 10/6 |
| 4 x 8, relaxed | balanced | 60,962 ± 299 | 245 | 251 | 1.04 | 2/2/2/2 |
| | kernel | 59,567 ± 376 | 260 | 277 | 1.12 | 3/2/2/1 |
| | skew | 58,327 ± 312 | 278 | 303 | 1.45 | 4/2/1/1 |
| 4 x 16, relaxed | balanced | 70,108 ± 1,099 | 454 | 464 | 1.05 | 4/4/4/4 |
| | kernel | 72,174 ± 984 | 478 | 505 | 1.21 | 5/5/3/3 |
| | skew | 69,014 ± 954 | 560 | 621 | 1.78 | 8/3/3/2 |
| 2 x 4, relaxed | all three | 49,310-49,631 | 116-131 | 117-134 | 1.01-1.02 | (4 sessions saturate the clients) |
| `group`, every cell | all three | within one sd | 6,094-7,338 | | 1.01-1.05 | |

**The CPU-bound variant** removes the log: `--shape point` (reads only),
`waystone_recording = off`, `relaxed`, three runs per arm.

| cores x sessions | arm | stmt/s | worst-core p99 us | spread |
|---|---|---|---|---|
| 4 x 8 | balanced | 56,495 ± 374 | 227 | 1.02 |
| | kernel | 62,264 ± 2,726 | 253 | 1.25 |
| | skew | 60,848 ± 772 | 224 | 1.22 |
| 2 x 8 | all three | 64,943-66,025 | 200-204 | 1.01 |
| 2 x 4 | all three | 44,111-45,014 | 112-114 | 1.00-1.02 |

**Reading.** Gate (i) asks whether the balanced arm improves throughput or
the per-core p99 spread past the run-to-run spread. **Throughput: no** - it is
better by 2-6 % in two `pkmix` cells and worse by 3 % and 10 % in two others,
where the unbalanced arms leave a reactor's SMT sibling, a client CPU, less
contended; on this host the clients are the other half of every physical
core, so a reactor's idle time is a client's gain, and no cell here prices
server-side placement alone. **The spread: yes**, in every `relaxed` cell with
a skew in it (1.12-1.78 to 1.01-1.05, against run-to-run spreads of
0.01-0.05 in the balanced arm), and the worst core's p99 falls 2-25 %. The
gate passes on the spread, which is a fairness claim: a session on a crowded
core is up to a quarter slower at its tail than one on an empty core.

## 2. Which signal (BL-Q3)

`SHOW META`'s `load_c<k>_*` gauges, read once mid-span. Per run, the rank
correlation between each core's signal and that core's p99, averaged over
runs (a signal must order one run's cores as their p99 does):

| signal | pkmix, 72 runs | CPU-bound, 24 runs |
|---|---|---|
| `busy_ppm` (reactor not blocked idle, 50 ms window) | **0.36** | 0.30 |
| `foreground_ppm` | 0.33 | **0.32** |
| `sessions` | 0.15 | 0.20 |
| `queued_foreground` (parked tasks included) | -0.07 | - |

`busy_ppm` correlates 1.0 at `relaxed` on two cores and around 0.5-0.6 on
four; at `group` no signal predicts the p99 (-0.67 to +0.55): latency there
is the commit sync's, not the reactor's. **BL-Q3's answer is `busy_ppm`**, the
best overall; in the CPU-bound cells it is level with `foreground_ppm` (0.30
against 0.32), where nothing but statements holds a reactor.
The widest gap between two evenly placed cores' `busy_ppm` was 179,793 ppm
(`group`, 4 x 16), at most 92,132 at `relaxed`; skewed placements read
138,897-960,000, so the two overlap at `group` and below 200,000 at
`relaxed` (175,580 and 196,651 in the two-by-sixteen skew). BL-R4's margin of
200,000 sits above the even placements' widest gap, from one mid-span
reading per run - a provisional number, not a separation.

## 3. The mixed cell: one core (gates (ii), (iii))

`bl_mixed.py`, `cores = 1`: four pk point readers open-loop at 400/s in
all, a walker (an unindexed one-row `SELECT` over 10,000 rows of a 200-byte
`char` - about 280 pages, so it crosses its slice boundary), a 1,024-row
multi-row `INSERT` session. The reader's latency is taken from its scheduled
send (no coordinated omission) and from its actual send. Five runs per
cell, interleaved; the slice is the measurement build's override.

| arm | slice | service p99 us | schedule p99 us (min..max of 5) |
|---|---|---|---|
| readers alone | 64 | 182 | 379 (272..732) |
| + walker | 64 | 505 | 636 (510..1,382) |
| + walker | 32 | 327 | 580 (350..624) |
| + walker | 16 | 226 | 321 (294..639) |
| + inserter | - | 442,302 | 524,558 (510,132..608,542) |
| + walker + inserter | 64 / 32 / 16 | 333,940 / 407,449 / 406,372 | 478,492 / 576,563 / 543,618 |

**Gate (ii) does not stop BL**: the mixed reader's p99 is over 1,000x its
isolated one, so there is a great deal for priority to serve. **Gate (iii)**:
a walk alone is served by the slice (16 pages brings the reader under 2x its
isolated p99), but nothing reactor-side touches the mixed cell - no slice
moves it, and the half-second stalls are not a walker's or the inserter's
own statements (the inserter's p50 is 2-4 ms). The next section says whose
they are.

## 4. The partition premise: four cores, and what the stall is

`bl_mixed.py --cores 4 --readers 6 --arm mixed`. `kernel`: as accepted;
`spread`: every session round-robin; `segregated`: walker and inserter on
core 3, readers on 0-2 - BL-R2's partition, forced by the driver.

| placement | runs | schedule p99 us (min..max) | service p99 us |
|---|---|---|---|
| readers alone (`spread`) | 5 | 964 (718..1,681) | 444 |
| kernel | 5 | 695,499 (453,393..745,776) | 333,142 |
| spread | 5 | 691,809 (646,206..792,713) | 315,048 |
| segregated | 5 | 559,349 (422,280..617,948) | 263,594 |

**Placement does not isolate the readers.** The partition is measurably
better than none - its five runs and `spread`'s do not overlap, about 19 %
lower - but every arm stalls for half a second. In a segregated run the three
cores that hold no batch session had 7-11 reactor holds over 100 ms each in
10 s - as many as the batch core. The per-core contention slots
(`load_c<k>_waited_<latch>_us`, BL-S1's holder split) name the latch: the
reader-only cores waited **4.13 s, 0.69 s and 3.41 s on the WAL stream
latch**, the batch core **4.14 s on the WAL sync gate**. The mechanism, traced
from source (`[source-read]`; the trace is archived as `wal-coupling.md`):

1. a pk point `SELECT` seen a second time records its Waystone trail and then
   `Catalog::TouchPattern` updates the pattern's `use_count`/`last_seen` with
   a **logged** row overwrite - a WAL append from every repeated read;
2. the inserter fills a 64 MiB segment every few seconds, and the roll runs
   under the stream latch: seal, flush, then `StartSegment`'s device sync,
   which waits out the writer thread's fsync of every live segment;
3. a reader's append - and core 0's 10 ms group-commit drain - blocks its
   reactor on the latch for the whole roll; `Latch::lock` is a blocking
   mutex, so every session on that core waits with it.

**The attribution, by experiment** (two runs per condition, interleaved):

| condition | placement | schedule p99 us | service p99 us |
|---|---|---|---|
| Waystone recording on | segregated | 501,210 | 283,150 |
| | readers off core 0 | 502,149 | 269,010 |
| **Waystone recording off** | segregated (readers on 0-2) | 367,123 | 1,280 |
| | **readers off core 0** (on 1-2, batch on 3) | **660** (565..756) | **413** |
| | batch on core 0, readers on 1-3 | 10,010 (1,089..18,931) | 500 |
| | spread | 623,097 | 16,822 |
| | kernel | 611,800 | 5,925 |
| | readers alone | 494 | 206 |

With the read path's WAL write gone, a reader on core 0 still meets the
roll through the drain, and a reader sharing a core with the batch sessions
meets their holds; **a reader on neither is within 1.4x of the readers
alone**. In a second pair of runs the readers-off-core-0 arm read 41-102 ms,
from a checkpoint turn on a reader core (each core checkpoints in rotation
and writes back the shared pool); the batch-on-core-0 arm read 1-19 ms.

**What this means for BL-S4.** The partition is the right mechanism once two
things hold: the read path makes no logged write, and core 0 - the log's
drain - serves no interactive session while a batch session runs (BL-R2 has
core 0 as never a batch core; the measurement says it is the best one).
Neither is BL's to decide: the first changes a catalog write the Waystone
spec calls best-effort but `rules.md` §5 requires to be logged; the second
reverses a clause of BL-R2.

## 5. The cost of a move

`bl_move_cost.py`: one session, a prepared pk point read in a closed loop,
against two servers of the probe build - one where nothing moves, one where
every head moves to the next core (`KDS_BL_PROBE_MOVES`) - in 20 alternating
blocks of 2,000, `relaxed`. The client was not pinned.

| cores | stay p50 / p99 us | move p50 / p99 us | move - stay at p50 |
|---|---|---|---|
| 2 | 49.1 / 83.8 | 77.1 / 121.1 | **28.0 us** |
| 4 | 50.2 / 89.7 | 89.3 / 144.1 | **39.1 us** |

A move costs one statement about one more round trip: the source's flush and
handoff, the kick, and the destination's registration. Under BL-R4's dwell
(one move per session per half second at most) it is amortised away; it does
not size the margin (section 2 does).

## 6. BL-S3's balancer from a skewed start (convergence)

`ba_census.py --shape pkmix`, `relaxed`, 12 s after 2 s of warm-up, three
runs per arm, interleaved: the skew placement on the balancer build against
the same skew and a balanced placement on `3552a03c`, which places nothing.
Sessions move under the balancer, so nothing is attributed to a core: the
worst client's p99 and the client p99 spread (max/min) instead, and the
moves the cores counted inside the measured span (the first corrections fall
in the warm-up). **The load gate's threshold was 4.5 for these runs**, not
2.5: other sessions held the host's 1-minute load near 3.8 with no build or
test running.

**The rule as first built did not hold**, and its run is kept
(`converge-v1/`, binary `4ba68d92...`): it moved a session only when its
share was under half the gap, counting every connection as a share, so it
left 3/1 splits in place, and in one four-by-sixteen run 88 moves in 12 s
took the pooled p99 to 1,052 us against 474-557 in the other two. The rule
now asks only that a move shrink the gap (share under the gap), counts the
sessions that ran a statement in the window rather than every connection,
and reads a core as held after four windows rather than two
(`load_placer.hpp`). **That build** (`converge-v2/`, `2c264a8c...`):

| cores x sessions | arm | stmt/s (mean ± sd) | p99 us | worst client p99 us | client spread | moves in span (per run) |
|---|---|---|---|---|---|---|
| 2 x 16 | skew, balancer | 83,883 ± 275 | 359 | 381 | 1.15 | 1, 0, 3 |
| | balanced, none | 81,413 ± 4,186 | 356 | 360 | 1.02 | 0 |
| | skew, none | 80,983 ± 116 | 383 | 472 | 1.51 | 0 |
| 4 x 8 | skew, balancer | 59,420 ± 312 | 269 | 282 | 1.13 | 0, 14, 0 |
| | balanced, none | 60,368 ± 785 | 249 | 254 | 1.04 | 0 |
| | skew, none | 58,099 ± 63 | 283 | 322 | 1.49 | 0 |
| 4 x 16 | skew, balancer | 73,504 ± 540 | 479 | 502 | 1.10 | 69, 38, 23 |
| | balanced, none | 69,999 ± 449 | 462 | 474 | 1.05 | 0 |
| | skew, none | 69,030 ± 248 | 561 | 702 | 2.11 | 0 |

**The balancer undoes most of a skew it did not cause, in every run.** The
client spread falls from 1.49-2.11x to 1.10-1.15x, the worst client's p99
by 12-29 %, and throughput rises 2.3-6.5 % over the same skew - to the
balanced arm's or past it. The moves inside the span are at most 69 in 12 s
across sixteen sessions (under six a second, against a ceiling of two per
session per second), and no run's p99 rose with them (471-484 us at four by
sixteen).

## 7. What was not run

- **`cores = 8`** and **64 sessions**: on an 8-vCPU host the clients would
  outnumber the free CPUs; no number would be the engine's.
- **Ten runs per arm**: three (skew) and five (mixed); two for each
  attribution condition.
- **`strict`**, the boundary cell as a separate measurement (the
  `load_c<k>_heads`/`_inside` counters carry it: every `pkmix` statement is a
  head), and the kick cost of the accept-time handoff (the move cost above
  includes a kick).
- **The mixed driver changed during its matrix**: from about 03:10 UTC the
  runs read gauges mid-span and placed the control session on the cell's
  core for `same`; no latency path changed.
