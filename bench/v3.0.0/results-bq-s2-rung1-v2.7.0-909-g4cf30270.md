# BQ-S2: rung 1 of the WAL segment-roll ladder, measured — no zero prewrite makes the hold short, a larger segment makes it rare and proportionally long, and no arm comes within 2x of the bar

**Thesis.** On `v2.7.0-909-g4cf30270` (BQ-S1's instruments), five relaxed arms
- the base and four measurement builds patched onto an export of the same
commit (H4 option 3, and the segment size at 16, 256 and 1,024 MiB) - were
run through BQ's cells C1-C3 three times each. **None of the 60 (arm, cell,
run) results meets the relaxed bar** (§5.3 term i: schedule p99 within 2x of
the isolated baseline on cores with no bulk writer). The closest is 2.9x
against the most generous isolated value (8.8x against BQ-S1's isolated
median), and it is one 1,024 MiB run in which five of six readers happened to sit on
a core no roll or drain touched and the sixth lost 556 samples to a stall
(its p99.9 is still 52x); the next closest is
20x (an opt3 run) in C1, and every C2 run is at least 3.1x against an
isolated arm that is itself 289x worse than BQ-S1's. Rung 1 is exhausted.

The two rung-1 candidates fail in opposite ways, and the pair says what the
roll's hold is made of:

- **H4 option 3 (no zero prewrite where `posix_fallocate` reserved the
  extents) makes the hold short and leaves a slow mode.** Pooled over the 15
  relaxed load runs the hold's median falls from **325 ms to 16.0 ms**, the
  time under a hold from a median of 12.2 s to 3.3 s per 30 s window, and the
  gate - the roll waiting out the WAL writer's `fdatasync` - from 318 ms to
  5.2 ms at the median. But **96 of its 728 rolls (13 %) still hold more than
  150 ms and carry 80 % of its summed hold**; its pooled hold p99 is 1.14 s
  against the base's 809 ms, and the readers' p99 (medians 42-113 ms in C1)
  is 150-500x BQ-S1's isolated arm. The fast mode has a floor of 9.0 ms
  (p0; p5 is 9.9 ms): the tail sync, the header's `fdatasync` and the
  directory's `fsync`, which are device operations on this host.
- **A larger segment makes rolls rare and proportionally longer (H2).** The
  roll rate falls as 1/size (4.58, 1.16, 0.256 and 0.066 per second at 16,
  64, 256 and 1,024 MiB, against 4.59, 1.16, 0.261 and 0.073 predicted from
  the log rate), but the hold grows at **4.9-5.5 ms per MiB of segment**, so
  the time under a hold stays at **12.9, 12.2, 13.2 and 9.4 s per 30 s
  window**, and the maximum rises from 1.3 s to 11.6 s. At 256 MiB and above
  the body is no longer ready when the roll arrives (94 of 112 rolls, 28 of
  30), and step d joins the gate.

**The deltas in this file are read against BQ-S1's base** (the same binary,
cells and driver, three runs, `results-bq-s1-segment-roll-v2.7.0-909-g4cf30270.md`);
this session's own base arm reproduces it on every roll and throughput figure
(§6), which is the noise floor the arms are read against. PostgreSQL is not
part of this file: the floor was not measured for this shape.

- **Order**: `instructions/v3.0.0/workorder-bq-wal-segment-roll.md` (§3 H2
  and H4, §5.3 the bar, BQ-Q5 (b), BQ-Q6 (a), BQ-Q7 (a), §7's BQ-S1 entry).
- **Driver**: `tools/bl_mixed.py` (`ba_census.py` underneath), documented by
  its own docstring; `bench/docs/` is closed (`bench/README.md`). The
  patches that make the arms are `driver/variants.py` in the archive.
- **Raw output**: `archive/bq-s2-rung1-v2.7.0-909-g4cf30270/` (see its
  `README.md`).
- **Companions**: `results-bq-s1-segment-roll-v2.7.0-909-g4cf30270.md` (the
  baseline) and `results-bl-ruling1-remeasure-v2.7.0-909-g4cf30270.md`.

| Item | Value |
|---|---|
| Executed | 2026-10-12. Builds of the four measurement arms 17:01-17:06 UTC; matrix cells **17:13-19:09 UTC** (first JSON 17:13:02, last 19:08:42, `all.done` 19:08:42). The matrix was stopped and edited twice (§0.3). **This file was written 2026-10-12 19:57 UTC from the existing data; no run, server, build or test was started in the session that wrote it** |
| Measured tree | The engine is `4cf30270` = **`v2.7.0-909-g4cf30270`** (BQ-S1's instruments), as an export: `4cf30270.tar` is byte-identical to `git archive 4cf30270` (sha256 `0190fe9e...1e99`, both). The four variants are that export plus `variants.py`'s one-file patch each (§0.1); **they are uncommitted measurement builds**. The drivers ran from worktree `bp-per-core-residue` (branch `worktree-bp-per-core-residue`), whose HEAD moved during the run; this file is written there at `1fb71d1a` (`v2.7.0-925-g1fb71d1a`) |
| Tree cleanliness | Engine: the base is the unmodified `4cf30270` build; each variant differs from the export in one file (diffs in §0.1). Driver: `tools/bl_mixed.py` is `4cf30270`'s plus the `meta_delta` fix (committed `ffe7d4b8`, 14:47 UTC, before this run; the fix is in the counters, not in the latency samples); `tools/ba_census.py` is `4cf30270`'s plus `KDS_CLIENT_TIMEOUT` (edited 17:54:38 UTC, committed `1fb71d1a`), so the cells before 17:54:38 ran with the 5 s client timeout and none of them recorded an error |
| Binaries | Every server ran from a copy; the driver copies once more into its work directory and records `bin_sha256` in every JSON, **all 83 cell JSONs carry their arm's hash** (table in §0.1). Base: `/home/cdkbs/bench-runs/bq/bin/kds_server.4cf30270`, sha256 `024ef4dc863ebc6b164dd5a585a9fd6ccbfcf12817be1a0e1d29ef492d2e9540`, the very copy BQ-S1 measured (source binary mtime 10:33:54.49, copy 10:33:54.52). The base was not rebuilt from the export; the five binaries are the same size (5,792,080 bytes) |
| Build | Release (`CMAKE_BUILD_TYPE:STRING=Release` in each `build-<arm>/CMakeCache.txt` and in BQ-S1's build) |
| Device | `/dev/root`, **ext4**, `rw,relatime,discard,errors=remount-ro,commit=30` (`df -T` and `/proc/mounts` read at 19:32 UTC; every JSON records `data_device: /dev/root ext4`), an Azure VM's NVMe disk, 125 GB free of 259 GB. The WAL directory and the data file share it; not tmpfs. No dedicated WAL device (CN-9) |
| Host | 8 vCPU (AMD EPYC 9V74), Linux 7.0.0-1014-azure. Servers pinned to CPUs 0,2,4,6, clients to 1,3,5,7 (the SMT siblings of the reactors, `results-bl-s1-placement-premise-v2.7.0-857-g3552a03c.md`). Two other KDS instances were up throughout (`autotrade` on port 15432 since 09-30; one under `claude-code-channels` since 10-11); their load during the run was not recorded |
| Server configuration | `cores = 4`, `durability = relaxed`, `buffer_pool_frames = 131072`, `log_level = info`, `reactor_cpus = 0,2,4,6`; every other key at its default (64 MiB segments in the base and opt3; 1 MiB ring; checkpoint cadence 5000 ms; BL-S3's balancer live). One fresh server and one fresh data file per cell; port 17561 |
| Cells | C1 x 3 placements, C2, C3, for each of 5 arms, 3 runs: **60 results**, plus 4 isolated base cells (once each at `relaxed` and at `group`) and 8 mount probes; **0 errors and 0 refusals in all 83 cell JSONs**. 30 s measured after a 2 s warm-up |
| Baseline | BQ-S1's base arm (cited above); this session's own base arm is the in-run control |
| Suite / overhead | **Not executed**; nothing is committed. The measurement builds change how the WAL creates a segment (opt3) and the segment constant (the sizes); **no test was run against them and no correctness claim is made for them**. Overhead of BQ's instruments: not measured here (BQ-S5) |

## 0. What was measured, and how to read the tables

### 0.1 The arms

All five are Release builds of the same commit. Each variant is the export
plus `driver/variants.py`'s one-file change, and the diffs below are the
whole of it (read against `git show 4cf30270:<path>`).

| Arm | Change | Binary (copy under `bench-runs/bq/s2/bin/`) | sha256 | Built (UTC) |
|---|---|---|---|---|
| base | none | `bq/bin/kds_server.4cf30270` | `024ef4dc863ebc6b164dd5a585a9fd6ccbfcf12817be1a0e1d29ef492d2e9540` | 10:33:54 (BQ-S1) |
| opt3 | H4 option 3: `Reserve` reports whether it fell back to `ftruncate`; the 64 MiB zero `Prewrite` runs only then, otherwise the body gets one `fsync` (`src/wal/file_log_device.cpp`, +13 / -3 lines) | `kds_server.opt3` | `16ff440ce0560b9e5756db65e97c7278ea351df033311e78c15c01d93c1f28cf` | 17:01:26 |
| s16 | `kDefaultSegmentSize` = 16 MiB (`include/kds/wal/log_device.hpp`, 1 line) | `kds_server.s16` | `e8d4f3be45e4bfd143487bd8066c5fa18bc2682d4613a6082f23c3761c26763f` | 17:02:57 |
| s256 | 256 MiB | `kds_server.s256` | `dcc140d63f3961c472ab1097c914084e30af73c228ce4369978a29e620e730d3` | 17:04:47 |
| s1024 | 1,024 MiB | `kds_server.s1024` | `9692df2294d95a611dd676156ed09ffb6f6932fb0d4f677e2294f81e5dafc744` | 17:06:00 |

On this volume (ext4) `posix_fallocate` is the call the base's own `Reserve`
makes before its `Prewrite`, and opt3's behaviour (no body wait in 728 of 728
rolls, a gate that collapses) is that of the fallocate branch in every
segment; that it never took the `ftruncate` branch is **inferred from that
behaviour, not traced**. A reserved but unwritten extent is converted by the
later header and log writes, which is the cost the work order's guard names
(§3 H4 option 3: "the first write to an unwritten extent may cost every
commit's `fdatasync` a metadata commit"); opt3's header step is the one that
got slower (§3.3).

### 0.2 The cells

Identical to BQ-S1's (`results-bq-s1-segment-roll-v2.7.0-909-g4cf30270.md` §0),
at `relaxed` only. Every cell adds `--cores 4 --seconds 30 --warmup 2 --runs 1
--max-wait 0 --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 --frames 131072
--log-level info --ctl-core 0 --port 17561` and the driver's `--durability`.

| Cell | Flags | What runs | Read |
|---|---|---|---|
| C3 | `--arm insert --readers 0 --writers 0 --placement segregated` | the bulk inserter alone (1,024-row `INSERT`s into `bl_ins`, closed loop) | rows/s, log MiB/s (term iii) |
| C2 | `--arm insert --readers 0 --writers 8 --writer-rate 800 --placement segregated` | inserter + 8 open-loop single-row pk `UPDATE` sessions (800/s in total) | writers' schedule and service latency |
| C2 isolated | `--arm isolated --readers 0 --writers 8 --writer-rate 800 ...` | the writers alone | baseline |
| C1 | `--arm mixed --readers 6 --rate 400 --placement P`, `P` in `segregated-no0`, `batch0`, `spread` | inserter + a walker + 6 open-loop pk readers (400/s in total) | readers' schedule and service latency |
| C1 isolated | `--arm isolated --readers 6 --rate 400 --placement P` | the readers alone | baseline |

Readers and writers are open-loop (12,006 and 24,007 scheduled samples per
run), the bulk inserter is closed-loop, so **work is equal for the measured
population and not for the bulk writer**: an arm that lets the inserter run
faster offers the stall-measuring population a heavier log. That is stated
wherever it matters (opt3: +43 % rows/s on average).

**Latency.** *Schedule* latency is taken from the instant the schedule said
to send, *service* latency from the send. **A table cell is the median of the
three runs' percentile**; every run is in Appendix A1. *Spread* is the
maximum minus the minimum of the three runs. **Samples still in flight when
the window closes are not recorded**: 21 of the 60 runs are short of the
scheduled count, by up to 556 samples (4.6 %, 1,024 MiB C1 segregated-no0 r1)
and 1.3 % in C2 (base r1, 1,024 MiB r1). A stall that straddles the end of
the window therefore lowers the percentiles of the arms that stall longest;
no verdict depends on it, because it can only move a miss toward the bar.

**Rolls** are the `[wal] roll` lines whose `start_ns` lies in `[m0, m1]`
(BQ-S1's rule); their count equals the `wal_rolls` delta in every run but one.
**Counters** are `SHOW META` at `m1` minus `m0` with a key absent at `m0` read
as 0 (the driver's fixed `meta_delta` and this file's analysis agree).

**The row-set sweep (200 / 1K / 10K) was not run, and does not apply**, for
BQ-S1's reason: a roll's cost is a function of log bytes and of the device's
state, not of a relation's cardinality. The relations are fixed (`bl_point`
and `bl_walk` 10,000 rows, `bl_ins` growing by the bulk writer).

### 0.3 The run order, and what was done to the matrix

The matrix (`driver/s2_matrix.py`; `driver/s2_matrix.py.v1` is the first
version) gates each cell on a one-minute load average under 2.5 and no
process from a fixed list, runs one fresh server per cell, and skips a cell
whose result exists. **It is not cell-interleaved.** Each pass runs an arm's
five cells as a block and rotates the block order between passes, so an
arm's runs are separated from the base's by up to an hour in pass 1.

| UTC | Event |
|---|---|
| 17:01-17:06 | the four variants built (`cmake --build -j8`, one after the other) |
| 17:13-17:19 | the isolated base cells: four at `relaxed`, four at `group`; the five- and fifteen-minute load averages were 8.8 / 7.8 at the first and 4.1 / 5.8 at the last (the builds had just ended), and the gate only reads the one-minute average |
| 17:21-17:35 | pass 1, first two blocks: opt3, then s16 |
| ~17:36-17:43 | **the matrix was stopped and edited** (restart 1; the watchdog's "chain was dead without all.done" at 17:43): relaxed for every arm before any group cell, and group only for an arm that meets the relaxed bar (`group.arms`, never written, because none did) |
| 17:49-17:52 | mount probe v1 (SIGKILL, §4.5), then the 1,024 MiB probe's client timed out at 5 s and left its server **orphaned, idle, on port 17571 from 17:52 to 17:54**; four further probes talked to it ("duplicate primary key 1"); `c3/s256/r1` failed at 17:53:10 on the 5 s client timeout (rc 1); `c2/s256/r1` ran 17:53:40-17:54:26 beside the orphan and was kept (0 errors) |
| 17:54:38-17:55 | `ba_census.py` gained `KDS_CLIENT_TIMEOUT`; **restart 2**: the matrix sets it to 120 s and uses mount probe v2 |
| 17:55-17:59 | mount probe v2, all eight probes re-taken |
| 18:01-19:09 | the interrupted `c3/s256/r1` re-run (18:01:09), then everything else; pass 1: s256, s1024, base; pass 2: s16, s256, s1024, base, opt3 (18:18-18:48); pass 3: s256, s1024, base, opt3, s16 (18:49-19:09) |

## 1. The verdicts

Each is the checked analysis's (an analyst's recomputation from the raw
files, then two adversarial recomputations; `verdicts/final-verdicts-h4opt3-h2-bar.json`
in the archive), **with the checkers' corrections adopted and this file's own
recomputation where they differ** (§1.1). Every criterion is the work
order's text, applied without adjustment.

| Hypothesis | `relaxed` | `group` |
|---|---|---|
| **H4 option 3** - the candidate is committed only if its cells meet the bar (§0.1); its own guard: C2's `group` commit p99 must not rise by more than the spread | **Rejected as the fix**: the p99 term is missed in all 12 of its C1 and C2 runs (closest C1 run 20.3x the most generous isolated value, closest C2 run 6.9x against an isolated arm that is 289x BQ-S1's). **The nearest miss**: hold p50 325 ms to 16.0 ms, summed hold 12.2 s to 3.3 s per window, C1 reader p99 medians 381 / 241 / 312 ms to 42.0 / 71.2 / 113 ms (two of its three runs per placement below the base's minimum; its r1 is above the base's maximum in two placements and inside the base's range in batch0). It leaves a slow mode (13 % of rolls, 80 % of the hold), its C2 writers are worse by median (p99 773 ms to 2.23 s, inside the base's 3.27 s spread), and C3's mean gain of 42.9 % has a 68 % spread | **Not run**: the guard is unevaluated, neither passed nor failed. `[inferred]` from the relaxed data that it would be tested hard: step e (the header's `fdatasync` into an unwritten extent) went from 1.1 to 2.8 ms at the median and its p99 from 91 to 292 ms |
| **H2** - rejected as the fix if the size meeting the p99 term misses the p99.9 term, or if the size that meets the bar costs C3 more than 3 % or adds more than 1 s to the mount | **Rejected as the fix, more basically than either clause anticipates: no size meets the p99 term in any of its 12 runs** (closest 2.9x, 1,024 MiB C1 segregated-no0 r1; the other eight C1 runs of that size are 3,500x or more and its three C2 runs 3.1-39x against the inflated isolated arm). **Neither reject clause applies**, because both presuppose a size that meets the bar. The prediction fares: roll rate 1/size **held**; per-roll hold flat **refuted** (4.9-5.5 ms/MiB); total stall 1/size **refuted** (flat); p99.9 and maximum do not fall **held, and worse** | **Not run** |
| **The bar** (§5.3 terms i and ii; BQ-Q2 (a)) | **Missed in all 60**. Term i by 2.9x at best (one 1,024 MiB run); the best run per placement is 20-71x for opt3, 125-278x for s16, 331-372x for the base, 813x or more for 256 MiB and 3,500x or more for 1,024 MiB (its one run aside); term ii (p99.9 within 5x) is met in 8 C2 runs and only against the 467 ms p99.9 of a contaminated isolated arm (BQ-S1's: 1.8-3.1 ms) | **Not run for any arm**: group runs only for an arm meeting the relaxed bar (§5.3: an arm that misses relaxed misses the rung); `out/group.arms` was never written. The four isolated `group` cells that exist are not used (§8) |
| **Rung 1 / BQ** | **Rung 1 missed.** Under BQ-Q6 (a) rungs 2 and 3 were recorded unable to meet the bar from BQ-S1's bounds (§2.3 re-derives the roll-aligned ceiling on this session's data), and BQ-Q5 (b) stops BQ before rung 4. BQ closes on §0.4's fourth row: **nothing below rung 4 meets the bar; the latch is kept; the cost is named** (§9). H6 stays unmeasured and needs the operator's own word | |

### 1.1 Where this file departs from the checked verdicts

None of these moves a bar term or a rejection.

1. **H2's second reject clause is not applied to 1,024 MiB.** The analyst's
   verdict said it "also fires". Both checkers' point stands: the clause
   covers "the size that meets the bar", none does, and the longest `INSERT`
   is not in the clause. The mount figures are reported in §4.5 as what the
   probe measured, with its confound (it writes 2.5 segments at every size,
   so 1,024 MiB mounts 2,570 MiB of log against 188 MiB at 64 MiB).
2. **"Closest run 21x / 20x" is two different runs and leaves C2 out.** The
   per-run table (§2) is the source: C1's closest opt3 run is 20.3x on p99
   (c1-segregated-no0 r2) and its closest p99.9 is 19.7x (c1-spread r2); C2's
   closest opt3 run is 6.9x / 2.5x (r3) against the inflated isolated arm. (The
   checkers' 21.3x and 20.5x are the non-bulk views of those same two runs.)
3. **"The slow mode fills most of every opt3 r1 cell" is too broad.** It holds
   for C1 segregated-no0 r1, C2 r1 and C3 r1 (from its second third); C1
   batch0 r1 and C1 spread r1 are fast by median (§3.2).
4. **opt3's hold tail**: the pooled p99 rose (809 ms to 1.14 s) and the
   maximum rose in four of five cells (it fell in C2, whose base maximum is one
   4.3 s roll); every one of these changes is inside the base's spread. "The
   tail did not improve" is the reading, not "it got worse".
5. **The C2 floor against BQ-S1's isolated arm is 893x, not 1,300x**
   (1,024 MiB r1, 435 ms over BQ-S1's largest isolated p99 of 487 µs; the
   smallest p99.9 ratio, 287x, is 16 MiB r3's 888 ms over 3.1 ms).
6. **The gate missed competing jobs in seven cells** (§8), and an orphaned
   idle server overlapped one (§0.3); neither changes a verdict, because each
   arm's other runs miss as well.
7. **The mechanism for opt3's gate collapse and for the hold's 5 ms/MiB is
   `[inferred]`**, not traced (no device-level instrument ran).

## 2. The bar: what readers and writers saw

At `relaxed` every arm's body is unchanged (schedule p50 within 5 % of the
base's in C1) and the tail is the roll's length or longer. The tables are the
median of three runs per percentile; the first block is **schedule** latency,
the second **service** latency (the difference is a send that was late
because the session was held behind an earlier stall).

**Schedule latency, median of the three runs per percentile (isolated arms are controls)**

| cell | arm | runs | n per run | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|---|
| c1-segregated-no0 | c1-iso-segregated-no0 (BQ-S1, base) | 3 | 12,006 | 117 µs | 168 µs | 174 µs | 199 µs | 279 µs | 2.5 ms | 4.1 ms |
| c1-segregated-no0 | c1-iso-segregated-no0 (BQ-S2, base) | 1 | 12,006 | 122 µs | 174 µs | 181 µs | 230 µs | 853 µs | 3.0 ms | 4.6 ms |
| c1-segregated-no0 | base (64 MiB) | 3 | 11,997 | 116 µs | 185 µs | 205 µs | 158 ms | 381 ms | 659 ms | 757 ms |
| c1-segregated-no0 | opt3 (64 MiB) | 3 | 12,006 | 121 µs | 190 µs | 209 µs | 467 µs | 42.0 ms | 194 ms | 783 ms |
| c1-segregated-no0 | s16 (16 MiB) | 3 | 12,006 | 121 µs | 189 µs | 208 µs | 42.8 ms | 160 ms | 416 ms | 563 ms |
| c1-segregated-no0 | s256 (256 MiB) | 3 | 11,861 | 123 µs | 184 µs | 208 µs | 429 ms | 1.03 s | 1.35 s | 1.51 s |
| c1-segregated-no0 | s1024 (1,024 MiB) | 3 | 11,450 | 104 µs | 186 µs | 201 µs | 1.71 s | 3.63 s | 4.52 s | 4.90 s |
| c1-batch0 | c1-iso-batch0 (BQ-S1, base) | 3 | 12,006 | 120 µs | 171 µs | 177 µs | 199 µs | 354 µs | 1.6 ms | 3.8 ms |
| c1-batch0 | c1-iso-batch0 (BQ-S2, base) | 1 | 12,006 | 127 µs | 172 µs | 178 µs | 202 µs | 602 µs | 2.4 ms | 4.4 ms |
| c1-batch0 | base (64 MiB) | 3 | 12,006 | 121 µs | 183 µs | 204 µs | 23.4 ms | 241 ms | 474 ms | 634 ms |
| c1-batch0 | opt3 (64 MiB) | 3 | 12,006 | 122 µs | 192 µs | 209 µs | 504 µs | 71.2 ms | 490 ms | 550 ms |
| c1-batch0 | s16 (16 MiB) | 3 | 12,006 | 114 µs | 186 µs | 203 µs | 33.5 ms | 245 ms | 555 ms | 597 ms |
| c1-batch0 | s256 (256 MiB) | 3 | 12,004 | 123 µs | 184 µs | 205 µs | 430 ms | 982 ms | 1.27 s | 1.42 s |
| c1-batch0 | s1024 (1,024 MiB) | 3 | 12,006 | 119 µs | 192 µs | 211 µs | 3.24 s | 4.37 s | 4.99 s | 5.17 s |
| c1-spread | c1-iso-spread (BQ-S1, base) | 3 | 12,006 | 140 µs | 183 µs | 188 µs | 210 µs | 226 µs | 1.1 ms | 3.6 ms |
| c1-spread | c1-iso-spread (BQ-S2, base) | 1 | 12,006 | 146 µs | 185 µs | 192 µs | 228 µs | 747 µs | 3.4 ms | 5.2 ms |
| c1-spread | base (64 MiB) | 3 | 11,992 | 114 µs | 184 µs | 200 µs | 85.5 ms | 312 ms | 619 ms | 753 ms |
| c1-spread | opt3 (64 MiB) | 3 | 12,006 | 131 µs | 194 µs | 209 µs | 584 µs | 113 ms | 843 ms | 892 ms |
| c1-spread | s16 (16 MiB) | 3 | 11,970 | 117 µs | 187 µs | 205 µs | 35.9 ms | 201 ms | 496 ms | 549 ms |
| c1-spread | s256 (256 MiB) | 3 | 12,006 | 119 µs | 183 µs | 197 µs | 101 ms | 1.06 s | 1.28 s | 1.35 s |
| c1-spread | s1024 (1,024 MiB) | 3 | 12,006 | 114 µs | 184 µs | 205 µs | 496 ms | 3.72 s | 4.88 s | 5.02 s |
| c2 | c2-iso (BQ-S1, base) | 3 | 24,007 | 119 µs | 154 µs | 159 µs | 184 µs | 471 µs | 2.9 ms | 10.4 ms |
| c2 | c2-iso (BQ-S2, base) | 1 | 24,007 | 83.2 µs | 157 µs | 164 µs | 1.2 ms | 141 ms | 467 ms | 616 ms |
| c2 | base (64 MiB) | 3 | 23,693 | 90.8 µs | 170 µs | 357 µs | 467 ms | 773 ms | 1.00 s | 1.03 s |
| c2 | opt3 (64 MiB) | 3 | 23,991 | 106 µs | 165 µs | 179 µs | 708 ms | 2.23 s | 2.66 s | 2.71 s |
| c2 | s16 (16 MiB) | 3 | 24,006 | 82.0 µs | 172 µs | 4.3 ms | 302 ms | 952 ms | 1.22 s | 1.25 s |
| c2 | s256 (256 MiB) | 3 | 24,007 | 93.0 µs | 168 µs | 331 µs | 1.43 s | 2.02 s | 2.82 s | 2.94 s |
| c2 | s1024 (1,024 MiB) | 3 | 23,698 | 85.4 µs | 166 µs | 216 µs | 4.47 s | 5.44 s | 5.70 s | 5.73 s |

**Service latency, median of the three runs per percentile**

| cell | arm | runs | n per run | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|---|
| c1-segregated-no0 | c1-iso-segregated-no0 (BQ-S1, base) | 3 | 12,006 | 85.4 µs | 99.7 µs | 105 µs | 127 µs | 160 µs | 1.0 ms | 3.0 ms |
| c1-segregated-no0 | c1-iso-segregated-no0 (BQ-S2, base) | 1 | 12,006 | 88.5 µs | 105 µs | 112 µs | 147 µs | 452 µs | 1.7 ms | 3.8 ms |
| c1-segregated-no0 | base (64 MiB) | 3 | 11,997 | 51.0 µs | 113 µs | 126 µs | 199 µs | 1.2 ms | 334 ms | 757 ms |
| c1-segregated-no0 | opt3 (64 MiB) | 3 | 12,006 | 62.5 µs | 121 µs | 135 µs | 210 µs | 7.6 ms | 152 ms | 783 ms |
| c1-segregated-no0 | s16 (16 MiB) | 3 | 12,006 | 62.9 µs | 117 µs | 131 µs | 342 µs | 63.8 ms | 139 ms | 563 ms |
| c1-segregated-no0 | s256 (256 MiB) | 3 | 11,861 | 58.4 µs | 112 µs | 128 µs | 210 µs | 1.0 ms | 906 ms | 1.51 s |
| c1-segregated-no0 | s1024 (1,024 MiB) | 3 | 11,450 | 57.2 µs | 113 µs | 124 µs | 246 µs | 788 µs | 515 ms | 4.90 s |
| c1-batch0 | c1-iso-batch0 (BQ-S1, base) | 3 | 12,006 | 86.1 µs | 102 µs | 108 µs | 126 µs | 177 µs | 1.0 ms | 3.2 ms |
| c1-batch0 | c1-iso-batch0 (BQ-S2, base) | 1 | 12,006 | 86.8 µs | 104 µs | 109 µs | 129 µs | 189 µs | 1.0 ms | 2.9 ms |
| c1-batch0 | base (64 MiB) | 3 | 12,006 | 61.0 µs | 112 µs | 128 µs | 183 µs | 1.1 ms | 306 ms | 634 ms |
| c1-batch0 | opt3 (64 MiB) | 3 | 12,006 | 69.6 µs | 122 µs | 135 µs | 218 µs | 6.0 ms | 48.6 ms | 550 ms |
| c1-batch0 | s16 (16 MiB) | 3 | 12,006 | 59.6 µs | 115 µs | 128 µs | 344 µs | 47.6 ms | 136 ms | 597 ms |
| c1-batch0 | s256 (256 MiB) | 3 | 12,004 | 59.4 µs | 110 µs | 126 µs | 192 µs | 906 µs | 254 ms | 1.42 s |
| c1-batch0 | s1024 (1,024 MiB) | 3 | 12,006 | 56.3 µs | 117 µs | 130 µs | 310 µs | 2.0 ms | 280 ms | 5.17 s |
| c1-spread | c1-iso-spread (BQ-S1, base) | 3 | 12,006 | 106 µs | 115 µs | 120 µs | 141 µs | 155 µs | 1.1 ms | 3.5 ms |
| c1-spread | c1-iso-spread (BQ-S2, base) | 1 | 12,006 | 107 µs | 118 µs | 124 µs | 155 µs | 321 µs | 2.3 ms | 5.1 ms |
| c1-spread | base (64 MiB) | 3 | 11,992 | 56.0 µs | 112 µs | 125 µs | 182 µs | 1.0 ms | 302 ms | 753 ms |
| c1-spread | opt3 (64 MiB) | 3 | 12,006 | 73.4 µs | 124 µs | 136 µs | 261 µs | 8.2 ms | 58.4 ms | 892 ms |
| c1-spread | s16 (16 MiB) | 3 | 11,970 | 58.2 µs | 116 µs | 130 µs | 506 µs | 54.5 ms | 130 ms | 549 ms |
| c1-spread | s256 (256 MiB) | 3 | 12,006 | 57.5 µs | 111 µs | 121 µs | 193 µs | 1.0 ms | 196 ms | 1.35 s |
| c1-spread | s1024 (1,024 MiB) | 3 | 12,006 | 61.3 µs | 113 µs | 128 µs | 190 µs | 1.0 ms | 230 ms | 5.02 s |
| c2 | c2-iso (BQ-S1, base) | 3 | 24,007 | 73.9 µs | 85.6 µs | 90.6 µs | 114 µs | 218 µs | 1.8 ms | 10.3 ms |
| c2 | c2-iso (BQ-S2, base) | 1 | 24,007 | 39.3 µs | 87.7 µs | 94.2 µs | 213 µs | 1.6 ms | 148 ms | 537 ms |
| c2 | base (64 MiB) | 3 | 23,693 | 36.0 µs | 90.4 µs | 103 µs | 253 µs | 319 ms | 620 ms | 1.03 s |
| c2 | opt3 (64 MiB) | 3 | 23,991 | 34.3 µs | 90.6 µs | 102 µs | 269 µs | 21.3 ms | 341 ms | 2.71 s |
| c2 | s16 (16 MiB) | 3 | 24,006 | 33.6 µs | 95.8 µs | 110 µs | 2.4 ms | 98.7 ms | 386 ms | 1.25 s |
| c2 | s256 (256 MiB) | 3 | 24,007 | 33.9 µs | 88.0 µs | 99.9 µs | 220 µs | 951 µs | 1.58 s | 2.94 s |
| c2 | s1024 (1,024 MiB) | 3 | 23,698 | 36.2 µs | 88.5 µs | 99.9 µs | 203 µs | 2.5 ms | 291 ms | 5.73 s |

**Reading the schedule table.** In C1 the base, s16 and opt3 differ in where
the mass of the tail sits (p95 158 ms, 42.8 ms and 467 µs in segregated-no0),
not in whether it is there at p99. At 256 and 1,024 MiB the stall is
seconds long and both p95 and p99 go with it. **C2's body moves too**: a
writer that appends meets the latch itself, and the C2 p50 is bimodal from run
to run in both sessions (base: BQ-S1 2.4 / 7.1 / 0.29 ms; this session 53 /
0.35 / 0.36 ms), so C2's median-of-three p50 is not a stable statistic and its
p99 range is the figure to read.

### 2.1 The bar, per run, and the baselines it is read against

The isolated arms are the controls. **BQ-S2's own isolated arms are not usable
as the honest baseline**: they ran 7-13 minutes after the builds (§0.3, §8),
and the C2 one has a p99 of 141 ms against BQ-S1's 213-487 µs. The tables
therefore carry the ratio against BQ-S1's isolated median and against the most
generous of the four isolated values.

**The bar (§5.3 terms i and ii), per run, schedule latency, relaxed**

Isolated baselines: BQ-S1 (3 runs, same base binary, quiet host) and BQ-S2 (1 run, host settling after the builds). "Most generous" = the largest of those four isolated values.

| cell | arm | p99 r1 / r2 / r3 | spread of p99 | median p99 ÷ S1 iso median | median p99 ÷ S2 iso | best run ÷ most generous iso | p99.9 r1 / r2 / r3 | spread of p99.9 | median p99.9 ÷ S1 iso median | best run ÷ most generous iso |
|---|---|---|---|---|---|---|---|---|---|---|
| c1-segregated-no0 | base | 451 ms / 381 ms / 312 ms | 140 ms | 1,364x | 446.5x | 365.7x | 659 ms / 568 ms / 669 ms | 101 ms | 267x | 186.8x |
| c1-segregated-no0 | opt3 | 640 ms / 17.4 ms / 42.0 ms | 623 ms | 150x | 49.3x | 20.3x | 906 ms / 87.5 ms / 194 ms | 819 ms | 78x | 28.8x |
| c1-segregated-no0 | s16 | 320 ms / 160 ms / 107 ms | 213 ms | 572x | 187.2x | 125.5x | 600 ms / 416 ms / 398 ms | 201 ms | 168x | 130.9x |
| c1-segregated-no0 | s256 | 4.91 s / 1.03 s / 951 ms | 3.96 s | 3,705x | 1,212.9x | 1,115.2x | 5.71 s / 1.35 s / 1.17 s | 4.53 s | 545x | 386.0x |
| c1-segregated-no0 | s1024 | 2.5 ms / 4.18 s / 3.63 s | 4.18 s | 13,012x | 4,259.9x | 2.9x | 160 ms / 4.52 s / 4.72 s | 4.56 s | 1,829x | 52.4x |
| c1-batch0 | base | 224 ms / 460 ms / 241 ms | 236 ms | 682x | 401.2x | 372.0x | 373 ms / 691 ms / 474 ms | 319 ms | 305x | 152.8x |
| c1-batch0 | opt3 | 231 ms / 42.6 ms / 71.2 ms | 189 ms | 201x | 118.4x | 70.7x | 1.19 s / 203 ms / 490 ms | 990 ms | 315x | 83.1x |
| c1-batch0 | s16 | 245 ms / 303 ms / 167 ms | 136 ms | 692x | 406.5x | 278.1x | 703 ms / 555 ms / 442 ms | 261 ms | 357x | 181.3x |
| c1-batch0 | s256 | 2.28 s / 960 ms / 982 ms | 1.32 s | 2,776x | 1,632.0x | 1,596.1x | 2.88 s / 1.22 s / 1.27 s | 1.66 s | 818x | 502.1x |
| c1-batch0 | s1024 | 4.37 s / 3.94 s / 4.42 s | 476 ms | 12,358x | 7,265.1x | 6,549.1x | 4.99 s / 4.89 s / 5.22 s | 328 ms | 3,211x | 2,006.0x |
| c1-spread | base | 312 ms / 247 ms / 325 ms | 77.6 ms | 1,379x | 418.1x | 331.2x | 619 ms / 376 ms / 707 ms | 330 ms | 542x | 110.4x |
| c1-spread | opt3 | 616 ms / 22.4 ms / 113 ms | 593 ms | 499x | 151.2x | 30.0x | 846 ms / 67.2 ms / 843 ms | 778 ms | 738x | 19.7x |
| c1-spread | s16 | 352 ms / 160 ms / 201 ms | 192 ms | 889x | 269.6x | 213.9x | 496 ms / 502 ms / 494 ms | 8.4 ms | 434x | 144.9x |
| c1-spread | s256 | 1.38 s / 608 ms / 1.06 s | 769 ms | 4,663x | 1,413.5x | 813.0x | 2.14 s / 1.20 s / 1.28 s | 934 ms | 1,119x | 353.3x |
| c1-spread | s1024 | 3.72 s / 2.62 s / 4.07 s | 1.46 s | 16,413x | 4,975.2x | 3,499.8x | 4.97 s / 3.64 s / 4.88 s | 1.33 s | 4,272x | 1,067.6x |
| c2 | base | 4.03 s / 773 ms / 752 ms | 3.27 s | 1,641x | 5.5x | 5.3x | 4.29 s / 1.00 s / 973 ms | 3.32 s | 343x | 2.1x |
| c2 | opt3 | 2.53 s / 2.23 s / 975 ms | 1.56 s | 4,735x | 15.9x | 6.9x | 3.25 s / 2.66 s / 1.18 s | 2.07 s | 910x | 2.5x |
| c2 | s16 | 1.03 s / 952 ms / 646 ms | 387 ms | 2,019x | 6.8x | 4.6x | 1.28 s / 1.22 s / 888 ms | 390 ms | 417x | 1.9x |
| c2 | s256 | 4.26 s / 2.02 s / 1.88 s | 2.38 s | 4,288x | 14.4x | 13.4x | 4.53 s / 2.26 s / 2.82 s | 2.27 s | 966x | 4.8x |
| c2 | s1024 | 435 ms / 5.44 s / 5.52 s | 5.09 s | 11,534x | 38.6x | 3.1x | 1.14 s / 5.70 s / 5.70 s | 4.56 s | 1,951x | 2.4x |

Across the 60 runs, the smallest p99 ratio to the most generous isolated value is 2.89x (c1-segregated-no0, s1024 r1); 0 runs have a p99 ratio of 2 or less; 8 have a p99.9 ratio of 5 or less (all in C2, against the contaminated isolated arm); 0 meet both terms.

**Term ii (p99.9 within 5x)** is met by 8 runs, all in C2, all against the
contaminated 467 ms: base r2 and r3, opt3 r3, s16 r1-r3, s256 r2, s1024 r1.
Against BQ-S1's isolated p99.9 (1.8-3.1 ms) none meets it (the smallest is
287x). C1 meets it nowhere (closest 19.7x, opt3 c1-spread r2).

The run-to-run spread of the base arm is the noise floor for a claim about an
arm's p99: 140 ms, 236 ms and 77.6 ms (C1 segregated-no0, batch0, spread) and
3.27 s (C2, one 4.3 s roll in r1). **opt3's C1 p99 improvement exceeds the
base's spread in the median (381 to 42.0 ms against a spread of 140 ms) but not
in every run: its r1 is 640 ms, above the base's maximum of 451 ms.** Its
C2 change (773 ms to 2.23 s) is inside the base's spread and is no finding.
s16's C1 p99 medians (381 to 160 ms, 241 to 245 ms, 312 to 201 ms) move by
1.6x, 0x and 1.4x the base's spread and the run ranges overlap in all three
placements: no reliable difference.

### 2.2 Which readers: the bar's "cores with no bulk writer"

The balancer moved sessions in every mixed run (2-62 moves in the window in C1, 17-132 in C2), so
a reader is attributed to the core it connected on and "cores with no bulk
writer" is approximate. Appendix A2 carries, per run, the non-bulk p99 and
p99.9 (clients whose connect core is not the core of any in-window roll),
which agrees with the aggregate in order of magnitude everywhere except two
runs in which the one reader connected on the rolls' core drives the aggregate:
1,024 MiB spread r3 (4.07 s against a non-bulk 105 ms) and 256 MiB batch0 r1
(2.28 s against 62 ms). Those two are the nearest the matrix comes to "cores
with no bulk writer" seeing less, and the non-bulk figures still miss the bar by
140x and 104x.

The closest run of the matrix shows what "no stall" looks like and why it is
not a win: 1,024 MiB, C1 segregated-no0, r1 (p99 2.5 ms, p99.9 160 ms, 556
samples never completed). Its two rolls held 5.5 s and 11.6 s (17.1 s of the 30 s window) on core 2,
and core 0's drain waited 17.1 s; five of the six readers ended on core 1,
whose stream-latch wait was 0 µs, and **the one that was moved onto core 0
completed 1,445 of 2,001 samples** (the run's maximum, 11.6 s). The p99 looks fine
because one reader in six was stalled and its samples are missing from the
window, not because the roll is cheap.

### 2.3 What removing the roll-aligned stalls leaves

BQ-Q6 (a) recorded rungs 2 and 3 as unable to meet the bar from BQ-S1's bounds,
which were computed on the base regime. opt3 changes the regime, so the same
bound is recomputed here on this session's runs: every slow sample (≥ 1 ms)
whose wait overlaps any roll's hold is dropped and the percentile is read
again. It is a ceiling for any rung that only removes roll-aligned stalls.

**Ceiling with every roll-aligned stall removed (C1, relaxed): the readers' schedule p99 and p99.9 after dropping each sample whose wait [due, due + latency] overlaps any in-window roll hold**

This is a bound computed from the recorded slow samples (those ≥ 1 ms), not a run: it removes the stalls a roll overlaps and leaves every other cause in. `< 1 ms` means the percentile falls inside the samples the driver does not keep.

| cell | arm | run | readers' samples | samples ≥ 1 ms | of them overlapping a roll | p99 as measured | p99 with roll-overlapping samples removed | p99.9 as measured | p99.9 with them removed | p99 removed ÷ largest isolated p99 |
|---|---|---|---|---|---|---|---|---|---|---|
| c1-segregated-no0 | base | r1 | 11,997 | 1,376 | 1,180 (86 %) | 451 ms | 3.4 ms | 659 ms | 129 ms | 3.9x |
| c1-segregated-no0 | base | r2 | 12,006 | 1,587 | 1,409 (89 %) | 381 ms | 3.2 ms | 568 ms | 171 ms | 3.7x |
| c1-segregated-no0 | base | r3 | 12,006 | 1,192 | 982 (82 %) | 312 ms | 39.9 ms | 669 ms | 190 ms | 46.7x |
| c1-segregated-no0 | opt3 | r1 | 12,006 | 2,811 | 1,205 (43 %) | 640 ms | 189 ms | 906 ms | 236 ms | 221.4x |
| c1-segregated-no0 | opt3 | r2 | 12,006 | 397 | 113 (28 %) | 17.4 ms | 9.8 ms | 87.5 ms | 50.1 ms | 11.5x |
| c1-segregated-no0 | opt3 | r3 | 11,994 | 442 | 202 (46 %) | 42.0 ms | 5.8 ms | 194 ms | 31.6 ms | 6.8x |
| c1-segregated-no0 | s16 | r1 | 12,006 | 2,055 | 1,667 (81 %) | 320 ms | 32.4 ms | 600 ms | 119 ms | 38.0x |
| c1-segregated-no0 | s16 | r2 | 12,006 | 1,289 | 987 (77 %) | 160 ms | 26.0 ms | 416 ms | 61.2 ms | 30.5x |
| c1-segregated-no0 | s16 | r3 | 12,006 | 1,514 | 1,041 (69 %) | 107 ms | 47.4 ms | 398 ms | 92.1 ms | 55.6x |
| c1-segregated-no0 | s256 | r1 | 11,861 | 3,918 | 1,839 (47 %) | 4.91 s | 5.04 s | 5.71 s | 5.71 s | 5910.2x |
| c1-segregated-no0 | s256 | r2 | 11,997 | 1,318 | 954 (72 %) | 1.03 s | 97.8 ms | 1.35 s | 216 ms | 114.7x |
| c1-segregated-no0 | s256 | r3 | 12,006 | 1,173 | 812 (69 %) | 951 ms | 121 ms | 1.17 s | 224 ms | 142.1x |
| c1-segregated-no0 | s1024 | r1 | 11,450 | 201 | 40 (20 %) | 2.5 ms | 1.8 ms | 160 ms | 157 ms | 2.1x |
| c1-segregated-no0 | s1024 | r2 | 12,006 | 3,909 | 2,431 (62 %) | 4.18 s | 198 ms | 4.52 s | 486 ms | 232.3x |
| c1-segregated-no0 | s1024 | r3 | 12,006 | 1,997 | 1,392 (70 %) | 3.63 s | 351 ms | 4.72 s | 708 ms | 411.5x |
| c1-batch0 | base | r1 | 12,006 | 895 | 519 (58 %) | 224 ms | 52.5 ms | 373 ms | 245 ms | 87.2x |
| c1-batch0 | base | r2 | 12,006 | 1,504 | 1,255 (83 %) | 460 ms | 96.9 ms | 691 ms | 284 ms | 161.1x |
| c1-batch0 | base | r3 | 12,006 | 558 | 467 (84 %) | 241 ms | < 1 ms | 474 ms | 63.0 ms | < 1.7x |
| c1-batch0 | opt3 | r1 | 12,006 | 497 | 209 (42 %) | 231 ms | 2.9 ms | 1.19 s | 162 ms | 4.7x |
| c1-batch0 | opt3 | r2 | 12,006 | 498 | 218 (44 %) | 42.6 ms | 7.5 ms | 203 ms | 36.0 ms | 12.5x |
| c1-batch0 | opt3 | r3 | 12,006 | 477 | 218 (46 %) | 71.2 ms | 10.7 ms | 490 ms | 61.2 ms | 17.7x |
| c1-batch0 | s16 | r1 | 12,006 | 1,530 | 923 (60 %) | 245 ms | 26.6 ms | 703 ms | 83.1 ms | 44.2x |
| c1-batch0 | s16 | r2 | 12,006 | 1,482 | 1,162 (78 %) | 303 ms | 14.9 ms | 555 ms | 74.8 ms | 24.8x |
| c1-batch0 | s16 | r3 | 12,006 | 1,080 | 868 (80 %) | 167 ms | 13.7 ms | 442 ms | 83.1 ms | 22.7x |
| c1-batch0 | s256 | r1 | 12,004 | 1,338 | 897 (67 %) | 2.28 s | 99.0 ms | 2.88 s | 190 ms | 164.6x |
| c1-batch0 | s256 | r2 | 12,006 | 1,411 | 1,004 (71 %) | 960 ms | 82.0 ms | 1.22 s | 211 ms | 136.2x |
| c1-batch0 | s256 | r3 | 12,006 | 1,671 | 1,019 (61 %) | 982 ms | 201 ms | 1.27 s | 922 ms | 334.1x |
| c1-batch0 | s1024 | r1 | 12,006 | 3,121 | 2,418 (77 %) | 4.37 s | 537 ms | 4.99 s | 794 ms | 893.3x |
| c1-batch0 | s1024 | r2 | 12,006 | 4,115 | 2,766 (67 %) | 3.94 s | 290 ms | 4.89 s | 895 ms | 482.9x |
| c1-batch0 | s1024 | r3 | 12,006 | 1,759 | 1,268 (72 %) | 4.42 s | 86.4 ms | 5.22 s | 233 ms | 143.7x |
| c1-spread | base | r1 | 11,992 | 1,202 | 973 (81 %) | 312 ms | 72.9 ms | 619 ms | 259 ms | 97.5x |
| c1-spread | base | r2 | 11,999 | 1,015 | 582 (57 %) | 247 ms | 85.6 ms | 376 ms | 207 ms | 114.6x |
| c1-spread | base | r3 | 11,986 | 909 | 741 (82 %) | 325 ms | 14.1 ms | 707 ms | 180 ms | 18.8x |
| c1-spread | opt3 | r1 | 12,006 | 1,457 | 714 (49 %) | 616 ms | 16.6 ms | 846 ms | 71.7 ms | 22.2x |
| c1-spread | opt3 | r2 | 12,006 | 475 | 106 (22 %) | 22.4 ms | 18.9 ms | 67.2 ms | 68.4 ms | 25.3x |
| c1-spread | opt3 | r3 | 12,006 | 521 | 210 (40 %) | 113 ms | 10.0 ms | 843 ms | 51.4 ms | 13.3x |
| c1-spread | s16 | r1 | 11,970 | 1,641 | 1,075 (66 %) | 352 ms | 122 ms | 496 ms | 474 ms | 163.6x |
| c1-spread | s16 | r2 | 12,006 | 1,323 | 887 (67 %) | 160 ms | 35.2 ms | 502 ms | 83.4 ms | 47.1x |
| c1-spread | s16 | r3 | 12,005 | 1,114 | 902 (81 %) | 201 ms | 15.8 ms | 494 ms | 76.6 ms | 21.2x |
| c1-spread | s256 | r1 | 12,006 | 1,041 | 507 (49 %) | 1.38 s | 168 ms | 2.14 s | 891 ms | 225.0x |
| c1-spread | s256 | r2 | 11,958 | 1,304 | 378 (29 %) | 608 ms | 87.1 ms | 1.20 s | 180 ms | 116.6x |
| c1-spread | s256 | r3 | 12,006 | 1,534 | 1,005 (66 %) | 1.06 s | 283 ms | 1.28 s | 980 ms | 378.8x |
| c1-spread | s1024 | r1 | 12,006 | 1,101 | 624 (57 %) | 3.72 s | 158 ms | 4.97 s | 381 ms | 211.0x |
| c1-spread | s1024 | r2 | 12,006 | 2,162 | 1,360 (63 %) | 2.62 s | 114 ms | 3.64 s | 278 ms | 152.0x |
| c1-spread | s1024 | r3 | 11,994 | 1,061 | 677 (64 %) | 4.07 s | 131 ms | 4.88 s | 233 ms | 175.8x |

**On opt3 the ceiling is 4.7x-221x the largest isolated p99; 8 of 9 runs stay
above 6.8x.** On the base it is 3.2-97 ms in 8 of 9 runs, with one exception:
c1-batch0 r3, whose remaining p99 falls inside the unrecorded range (< 1 ms),
so BQ-S1's "never within 2x" is not uniform. What remains after the roll's
stalls are removed is every other cause (a checkpoint turn, a reactor hold, a
carve, a writeback), which this run does not attribute: opt3's remaining p99
is 2.9-189 ms, one to three orders of magnitude over the isolated arm. **In
opt3's best runs the roll is no longer what sets the readers' p99**: in
c1-spread r2 (no roll over 150 ms) only 106 of the 475 slow samples overlap a
roll and removing them moves p99 from 22.4 to 18.9 ms (§9.2).

## 3. H4 option 3: the hold is short, and a slow mode remains

### 3.1 The hold

Pooled over the three runs of each cell (BQ-S1's base beside it):

**Roll hold per arm and cell, relaxed (pooled over the three runs; rolls with `start_ns` in `[m0, m1]`)**

| cell | arm | rolls r1 / r2 / r3 | pooled n | p0 | p25 | p50 | p95 | p99 | max | summed hold per run (r1 / r2 / r3) | share of the window under a hold (r1 / r2 / r3) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c1-segregated-no0 | base | 35 / 36 / 36 | 107 | 46.1 ms | 264 ms | 298 ms | 646 ms | 768 ms | 769 ms | 11.0 s / 12.5 s / 11.2 s | 37.0 % / 42.1 % / 37.1 % |
| c1-segregated-no0 | opt3 | 25 / 50 / 53 | 128 | 9.0 ms | 11.8 ms | 14.3 ms | 795 ms | 993 ms | 1.01 s | 9.4 s / 1.4 s / 2.6 s | 31.1 % / 4.7 % / 8.4 % |
| c1-segregated-no0 | s16 | 125 / 142 / 142 | 409 | 7.3 ms | 58.2 ms | 75.0 ms | 190 ms | 412 ms | 585 ms | 13.2 s / 10.9 s / 10.5 s | 43.9 % / 36.2 % / 34.9 % |
| c1-segregated-no0 | s256 | 5 / 8 / 8 | 21 | 859 ms | 1.23 s | 1.30 s | 2.65 s | 3.46 s | 3.46 s | 12.2 s / 9.9 s / 10.0 s | 48.1 % / 34.1 % / 34.5 % |
| c1-segregated-no0 | s1024 | 2 / 2 / 2 | 6 | 4.16 s | 4.49 s | 4.70 s | 11.6 s | 11.6 s | 11.6 s | 17.1 s / 9.2 s / 9.1 s | 51.3 % / 30.5 % / 30.1 % |
| c1-batch0 | base | 35 / 35 / 34 | 104 | 9.7 ms | 271 ms | 303 ms | 654 ms | 771 ms | 791 ms | 10.6 s / 12.1 s / 11.6 s | 35.8 % / 40.7 % / 38.9 % |
| c1-batch0 | opt3 | 49 / 53 / 52 | 154 | 9.2 ms | 12.2 ms | 16.7 ms | 340 ms | 769 ms | 1.37 s | 4.6 s / 2.4 s / 3.0 s | 15.3 % / 8.0 % / 9.8 % |
| c1-batch0 | s16 | 129 / 142 / 143 | 414 | 6.6 ms | 48.3 ms | 66.9 ms | 116 ms | 569 ms | 820 ms | 10.6 s / 11.3 s / 9.9 s | 35.4 % / 37.5 % / 32.7 % |
| c1-batch0 | s256 | 5 / 9 / 8 | 22 | 814 ms | 1.06 s | 1.23 s | 2.83 s | 3.06 s | 3.06 s | 13.2 s / 10.2 s / 9.1 s | 50.7 % / 33.9 % / 31.3 % |
| c1-batch0 | s1024 | 2 / 2 / 2 | 6 | 4.07 s | 4.09 s | 4.11 s | 5.32 s | 5.32 s | 5.32 s | 9.3 s / 9.2 s / 9.4 s | 30.9 % / 30.5 % / 31.2 % |
| c1-spread | base | 35 / 35 / 36 | 106 | 7.9 ms | 253 ms | 273 ms | 600 ms | 784 ms | 790 ms | 10.3 s / 10.4 s / 10.9 s | 34.4 % / 35.0 % / 36.1 % |
| c1-spread | opt3 | 52 / 59 / 53 | 164 | 9.2 ms | 11.6 ms | 14.1 ms | 95.0 ms | 902 ms | 1.01 s | 3.3 s / 1.0 s / 3.0 s | 10.8 % / 3.2 % / 10.1 % |
| c1-spread | s16 | 125 / 142 / 143 | 410 | 7.8 ms | 54.5 ms | 69.4 ms | 169 ms | 534 ms | 595 ms | 12.8 s / 10.7 s / 10.2 s | 43.0 % / 35.5 % / 33.9 % |
| c1-spread | s256 | 6 / 8 / 8 | 22 | 346 ms | 1.14 s | 1.32 s | 2.56 s | 2.88 s | 2.88 s | 12.8 s / 9.3 s / 9.2 s | 44.0 % / 32.1 % / 31.5 % |
| c1-spread | s1024 | 2 / 2 / 2 | 6 | 3.83 s | 4.06 s | 4.81 s | 5.15 s | 5.15 s | 5.15 s | 9.2 s / 8.6 s / 9.9 s | 30.6 % / 28.7 % / 32.9 % |
| c2 | base | 30 / 34 / 35 | 99 | 9.3 ms | 335 ms | 363 ms | 797 ms | 4.32 s | 4.32 s | 16.9 s / 13.1 s / 13.7 s | 54.5 % / 43.7 % / 45.3 % |
| c2 | opt3 | 17 / 58 / 57 | 132 | 9.8 ms | 13.3 ms | 21.0 ms | 1.01 s | 1.28 s | 1.45 s | 12.7 s / 4.7 s / 4.7 s | 42.6 % / 15.6 % / 15.6 % |
| c2 | s16 | 131 / 137 / 137 | 405 | 8.8 ms | 72.4 ms | 89.4 ms | 250 ms | 520 ms | 1.31 s | 14.1 s / 13.9 s / 14.6 s | 46.8 % / 46.0 % / 48.7 % |
| c2 | s256 | 5 / 9 / 8 | 22 | 1.07 s | 1.41 s | 1.51 s | 3.50 s | 4.56 s | 4.56 s | 16.1 s / 13.9 s / 11.4 s | 57.2 % / 45.3 % / 39.1 % |
| c2 | s1024 | 2 / 2 / 2 | 6 | 236 ms | 561 ms | 4.69 s | 5.73 s | 5.73 s | 5.73 s | 0.8 s / 10.4 s / 11.3 s | 2.6 % / 34.2 % / 36.9 % |
| c3 | base | 36 / 33 / 36 | 105 | 101 ms | 342 ms | 376 ms | 715 ms | 809 ms | 847 ms | 13.2 s / 14.4 s / 14.5 s | 43.9 % / 48.5 % / 48.4 % |
| c3 | opt3 | 29 / 64 / 57 | 150 | 9.5 ms | 13.0 ms | 17.2 ms | 819 ms | 1.29 s | 1.50 s | 10.4 s / 2.2 s / 4.4 s | 33.9 % / 7.4 % / 14.6 % |
| c3 | s16 | 142 / 142 / 142 | 426 | 7.2 ms | 75.3 ms | 93.1 ms | 151 ms | 553 ms | 612 ms | 13.6 s / 14.0 s / 13.8 s | 45.3 % / 46.8 % / 46.2 % |
| c3 | s256 | 7 / 9 / 9 | 25 | 935 ms | 1.48 s | 1.59 s | 2.55 s | 2.79 s | 2.79 s | 17.1 s / 13.8 s / 13.7 s | 54.5 % / 45.4 % / 45.2 % |
| c3 | s1024 | 2 / 2 / 2 | 6 | 5.30 s | 5.58 s | 5.64 s | 10.5 s | 10.5 s | 10.5 s | 18.5 s / 10.9 s / 11.6 s | 56.4 % / 36.4 % / 39.0 % |

**BQ-S1 base rows for the same cells (the baseline; 3 runs, same binary `4cf30270`)**

| cell | arm | rolls r1 / r2 / r3 | pooled n | p0 | p25 | p50 | p95 | p99 | max | summed hold per run | share of window |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c1-segregated-no0 | base, BQ-S1 | 35 / 35 / 37 | 107 | 47.0 ms | 274 ms | 297 ms | 472 ms | 697 ms | 710 ms | 11.4 s / 10.4 s / 12.0 s | 38.4 % / 34.9 % / 39.9 % |
| c1-batch0 | base, BQ-S1 | 36 / 35 / 33 | 104 | 16.9 ms | 278 ms | 318 ms | 565 ms | 722 ms | 725 ms | 12.1 s / 11.3 s / 11.2 s | 40.6 % / 37.8 % / 37.3 % |
| c1-spread | base, BQ-S1 | 35 / 35 / 34 | 104 | 9.9 ms | 272 ms | 307 ms | 655 ms | 788 ms | 828 ms | 10.6 s / 11.7 s / 12.7 s | 35.1 % / 39.3 % / 42.3 % |
| c2 | base, BQ-S1 | 34 / 33 / 34 | 101 | 64.3 ms | 332 ms | 367 ms | 707 ms | 789 ms | 1.21 s | 13.9 s / 13.8 s / 13.2 s | 45.4 % / 45.6 % / 44.2 % |
| c3 | base, BQ-S1 | 36 / 36 / 36 | 108 | 8.3 ms | 330 ms | 359 ms | 575 ms | 794 ms | 911 ms | 13.5 s / 13.3 s / 12.8 s | 45.1 % / 44.2 % / 42.8 % |

opt3's median hold is 14-21 ms in every cell, against 273-376 ms for the base,
and its summed hold per window is 1.0-4.7 s in r2 and r3 and 3.3-12.7 s in r1.
**The hold p99 and the maximum do not fall**: C1 segregated-no0 768 to 993 ms,
batch0 771 to 769 ms, spread 784 to 902 ms; the maxima rise in C1 (769 to
1,009, 791 to 1,373, 790 to 1,012 ms) and in C3 (847 ms to 1.50 s) and fall in
C2 (4.32 s to 1.45 s, the base's being one roll).

### 3.2 Two modes

**opt3 and base, per run: the two modes of the hold (relaxed)**

| cell | arm | run | JSON written (UTC) | rolls | hold p50 | p90 | p99 | max | rolls under 50 ms | first roll over 150 ms, s into the window | hold p50 by thirds of the window (1st / 2nd / 3rd) | checkpoint runs in window | segments removed in window |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| c1-segregated-no0 | base | r1 | 18:15:32 | 35 | 271 ms | 646 ms | 769 ms | 769 ms | 0 of 35 | 0.5 | 275 ms / 264 ms / 251 ms | 1 | 0 |
| c1-segregated-no0 | base | r2 | 18:42:37 | 36 | 324 ms | 432 ms | 629 ms | 629 ms | 0 of 36 | 0.5 | 343 ms / 313 ms / 312 ms | 1 | 0 |
| c1-segregated-no0 | base | r3 | 18:59:23 | 36 | 286 ms | 480 ms | 768 ms | 768 ms | 1 of 36 | 0.8 | 308 ms / 269 ms / 287 ms | 1 | 0 |
| c1-segregated-no0 | opt3 | r1 | 17:26:51 | 25 | 212 ms | 946 ms | 1.01 s | 1.01 s | 12 of 25 | 1.1 | 792 ms / 497 ms / 16.1 ms | 1 | 4 |
| c1-segregated-no0 | opt3 | r2 | 18:46:25 | 50 | 13.7 ms | 61.3 ms | 198 ms | 198 ms | 44 of 50 | 29.6 | 12.9 ms / 14.6 ms / 13.4 ms | 6 | 37 |
| c1-segregated-no0 | opt3 | r3 | 19:03:12 | 53 | 12.9 ms | 104 ms | 798 ms | 798 ms | 44 of 53 | 15.8 | 13.0 ms / 12.2 ms / 14.1 ms | 5 | 37 |
| c1-batch0 | base | r1 | 18:16:23 | 35 | 281 ms | 381 ms | 771 ms | 771 ms | 1 of 35 | 1.3 | 290 ms / 281 ms / 264 ms | 1 | 0 |
| c1-batch0 | base | r2 | 18:43:27 | 35 | 310 ms | 702 ms | 791 ms | 791 ms | 0 of 35 | 1.3 | 306 ms / 312 ms / 304 ms | 1 | 0 |
| c1-batch0 | base | r3 | 19:00:04 | 34 | 316 ms | 467 ms | 643 ms | 643 ms | 0 of 34 | 0.5 | 316 ms / 298 ms / 317 ms | 2 | 12 |
| c1-batch0 | opt3 | r1 | 17:29:51 | 49 | 18.1 ms | 340 ms | 1.37 s | 1.37 s | 39 of 49 | 0.2 | 17.6 ms / 17.5 ms / 18.1 ms | 1 | 0 |
| c1-batch0 | opt3 | r2 | 18:47:14 | 53 | 17.1 ms | 124 ms | 281 ms | 281 ms | 42 of 53 | 15.6 | 14.8 ms / 17.1 ms / 17.1 ms | 6 | 47 |
| c1-batch0 | opt3 | r3 | 19:04:03 | 52 | 14.9 ms | 71.1 ms | 571 ms | 571 ms | 43 of 52 | 18.3 | 14.0 ms / 14.8 ms / 19.9 ms | 4 | 29 |
| c1-spread | base | r1 | 18:17:13 | 35 | 263 ms | 458 ms | 767 ms | 767 ms | 2 of 35 | 1.3 | 259 ms / 260 ms / 267 ms | 1 | 0 |
| c1-spread | base | r2 | 18:44:18 | 35 | 271 ms | 401 ms | 790 ms | 790 ms | 1 of 35 | 1.4 | 295 ms / 260 ms / 270 ms | 2 | 0 |
| c1-spread | base | r3 | 19:01:03 | 36 | 282 ms | 464 ms | 784 ms | 784 ms | 2 of 36 | 1.6 | 303 ms / 263 ms / 282 ms | 1 | 3 |
| c1-spread | opt3 | r1 | 17:30:51 | 52 | 14.1 ms | 95.0 ms | 902 ms | 902 ms | 44 of 52 | 18.4 | 13.8 ms / 16.4 ms / 13.7 ms | 4 | 24 |
| c1-spread | opt3 | r2 | 18:48:03 | 59 | 14.0 ms | 23.6 ms | 64.3 ms | 64.3 ms | 58 of 59 | none | 13.1 ms / 12.5 ms / 14.3 ms | 6 | 45 |
| c1-spread | opt3 | r3 | 19:04:53 | 53 | 14.3 ms | 40.5 ms | 1.01 s | 1.01 s | 48 of 53 | 16.9 | 12.8 ms / 18.0 ms / 13.8 ms | 4 | 34 |
| c2 | base | r1 | 18:14:33 | 30 | 366 ms | 652 ms | 4.32 s | 4.32 s | 0 of 30 | 0.4 | 366 ms / 382 ms / 348 ms | 1 | 0 |
| c2 | base | r2 | 18:41:48 | 34 | 358 ms | 585 ms | 1.03 s | 1.03 s | 1 of 34 | 0.5 | 359 ms / 375 ms / 349 ms | 1 | 0 |
| c2 | base | r3 | 18:58:43 | 35 | 361 ms | 515 ms | 1.00 s | 1.00 s | 0 of 35 | 0.4 | 371 ms / 345 ms / 347 ms | 1 | 0 |
| c2 | opt3 | r1 | 17:24:16 | 17 | 801 ms | 1.28 s | 1.45 s | 1.45 s | 0 of 17 | 0.8 | 801 ms / 744 ms / 571 ms | 0 | 0 |
| c2 | opt3 | r2 | 18:45:43 | 58 | 16.9 ms | 221 ms | 1.15 s | 1.15 s | 46 of 58 | 15.4 | 16.7 ms / 14.7 ms / 21.0 ms | 3 | 16 |
| c2 | opt3 | r3 | 19:02:31 | 57 | 16.5 ms | 207 ms | 1.14 s | 1.14 s | 46 of 57 | 9.0 | 14.9 ms / 13.6 ms / 19.4 ms | 3 | 16 |
| c3 | base | r1 | 18:13:52 | 36 | 350 ms | 442 ms | 847 ms | 847 ms | 0 of 36 | 0.0 | 379 ms / 333 ms / 386 ms | 3 | 14 |
| c3 | base | r2 | 18:41:07 | 33 | 403 ms | 647 ms | 809 ms | 809 ms | 0 of 33 | 0.4 | 427 ms / 393 ms / 395 ms | 2 | 0 |
| c3 | base | r3 | 18:57:51 | 36 | 373 ms | 696 ms | 753 ms | 753 ms | 0 of 36 | 0.0 | 379 ms / 373 ms / 354 ms | 2 | 0 |
| c3 | opt3 | r1 | 17:22:13 | 29 | 53.1 ms | 1.12 s | 1.50 s | 1.50 s | 14 of 29 | 8.8 | 14.7 ms / 525 ms / 1.00 s | 1 | 0 |
| c3 | opt3 | r2 | 18:45:04 | 64 | 16.5 ms | 69.1 ms | 310 ms | 310 ms | 52 of 64 | 26.1 | 15.9 ms / 13.9 ms / 18.7 ms | 6 | 55 |
| c3 | opt3 | r3 | 19:01:51 | 57 | 17.3 ms | 186 ms | 838 ms | 838 ms | 44 of 57 | 13.4 | 17.0 ms / 19.4 ms / 17.3 ms | 5 | 33 |

Of opt3's 728 rolls, **576 hold less than 50 ms** (the fast mode: gate 3.9 ms,
own sync 4.7 ms, header 2.8 ms, directory 2.0 ms at the median, p0 9.0 ms, p5
9.9 ms) and **96 hold more than 150 ms** (13.2 % of the rolls, **79.8 % of the
summed hold**; 73.9 % of their hold is the gate, 11.3 % the header, 8.9 % the
own sync, 5.6 % the directory fsync). The slow rolls are the base's slow gate
again: a roll arrives while the writer thread is inside a sync that takes
hundreds of milliseconds, and nothing in opt3 changes that sync.

Which runs are in which mode is not explained by anything this run recorded.
Nine of the ten r2 and r3 runs have a first roll over 150 ms 9-30 s into the
window, and one (c1-spread r2) none; the three r1 runs of C1
segregated-no0, C2 and C3 are slow from the start or from the second third,
while C1 batch0 r1 and C1 spread r1 are fast by median and slow in the tail.
opt3 r1 (17:21-17:31) is not interleaved with the base's r1 (18:13-18:17): it
ran 15-25 minutes after the last build finished, which the data cannot
separate from the build itself. The fast mode's survival beyond 30 s is not measured.

### 3.3 The step split

**The step split of the hold, relaxed, shares of the summed hold (every load cell pooled per arm)**

| arm | rolls | summed hold | seal | flush | gate (c1) | own sync (c2+c3) | body wait (d) | inline body (d) | header (e) | rename (f) | dirsync (g) | residual |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| base (S1) | 524 | 184 s | 0.2 % | 0.0 % | 95.6 % | 1.0 % | 0.6 % | 0.0 % | 1.3 % | 0.0 % | 1.1 % | 0.0 % |
| base | 521 | 186 s | 0.1 % | 0.0 % | 95.6 % | 1.5 % | 0.5 % | 0.0 % | 1.2 % | 0.0 % | 1.2 % | 0.0 % |
| opt3 | 728 | 70 s | 0.5 % | 0.0 % | 68.9 % | 11.3 % | 0.0 % | 0.0 % | 12.4 % | 0.1 % | 6.9 % | 0.0 % |
| s16 | 2064 | 184 s | 0.8 % | 0.0 % | 82.5 % | 7.7 % | 0.2 % | 0.0 % | 4.9 % | 0.0 % | 3.8 % | 0.0 % |
| s256 | 112 | 182 s | 0.1 % | 0.0 % | 78.0 % | 6.1 % | 14.2 % | 0.0 % | 0.5 % | 0.0 % | 1.1 % | 0.0 % |
| s1024 | 30 | 155 s | 0.0 % | 0.0 % | 46.9 % | 0.7 % | 52.0 % | 0.0 % | 0.2 % | 0.0 % | 0.2 % | 0.0 % |

Medians of each step per roll (the typical roll, not the summed share):

| arm | hold | seal | gate | own sync | body wait | header | rename | dirsync |
|---|---|---|---|---|---|---|---|---|
| base (S1) | 331 ms | 30.0 µs | 324 ms | 1.2 ms | 0.0 µs | 1.1 ms | 32.0 µs | 2.1 ms |
| base | 325 ms | 30.0 µs | 318 ms | 1.2 ms | 0.0 µs | 1.1 ms | 32.0 µs | 2.2 ms |
| opt3 | 16.0 ms | 82.0 µs | 5.2 ms | 4.4 ms | 0.0 µs | 2.8 ms | 31.0 µs | 2.0 ms |
| s16 | 78.2 ms | 31.0 µs | 68.3 ms | 4.1 ms | 0.0 µs | 1.1 ms | 37.0 µs | 2.2 ms |
| s256 | 1.41 s | 28.0 µs | 1.22 s | 1.2 ms | 163 ms | 1.1 ms | 31.0 µs | 2.1 ms |
| s1024 | 5.03 s | 32.0 µs | 2.17 s | 1.0 ms | 2.74 s | 1.1 ms | 32.0 µs | 2.0 ms |

Tail of the create steps (p99 per roll) and of the gate:

| arm | gate p50 | gate p95 | gate p99 | header p50 | header p99 | header max | dirsync p50 | dirsync p99 | waited writer sync, summed per roll, p50 | p95 | p99 | per waited sync p50 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| base (S1) | 324 ms | 582 ms | 718 ms | 1.1 ms | 90.8 ms | 239 ms | 2.1 ms | 47.4 ms | 302 ms | 660 ms | 896 ms | 151 ms |
| base | 318 ms | 621 ms | 731 ms | 1.1 ms | 91.0 ms | 202 ms | 2.2 ms | 48.0 ms | 301 ms | 694 ms | 891 ms | 151 ms |
| opt3 | 5.2 ms | 473 ms | 849 ms | 2.8 ms | 292 ms | 596 ms | 2.0 ms | 153 ms | 9.9 ms | 510 ms | 958 ms | 7.9 ms |
| s16 | 68.3 ms | 127 ms | 416 ms | 1.1 ms | 103 ms | 303 ms | 2.2 ms | 24.5 ms | 116 ms | 157 ms | 429 ms | 58.2 ms |
| s256 | 1.22 s | 2.03 s | 2.85 s | 1.1 ms | 144 ms | 168 ms | 2.1 ms | 298 ms | 1.45 s | 2.62 s | 3.69 s | 727 ms |
| s1024 | 2.17 s | 5.59 s | 6.47 s | 1.1 ms | 283 ms | 283 ms | 2.0 ms | 137 ms | 2.90 s | 6.79 s | 7.47 s | 1.45 s |

**What changed is the gate**: 318 ms to 5.2 ms at the median, and the waited
writer sync (the sum per roll) from 301 ms to 9.9 ms (per waited sync, 151 ms to
7.9 ms), with its p99 about unchanged (891 to 958 ms, the slow mode). The
create steps got slower: the header from 1.1 to 2.8 ms at the median
(p99 91 to 292 ms) and the directory fsync's p99 from 48 to 153 ms, so steps d-g
are 19.4 % of opt3's pooled hold against 2.9 % of the base's. The reading that the
64 MiB zero fill and its `fsync` were competing with the writer's `fdatasync`
for the device, and that opt3 removes the competitor, is `[inferred]`: no
device-level measurement ran, but it fits the three things the data do show
(the gate collapses by 60x, the body is never missed, and the header into an
unwritten extent gets slower, which is where the work went).

### 3.4 The log is recycled in the fast mode

BQ-S1 found that at `relaxed` the log is almost never recycled during the load
(13 of 15 runs removed 0 segments) because the fold's redo start stays in
segment 0 (`docs/inflight/bugs/a-page-re-dirtied-after-its-writeback-copy-pins-the-redo-start.md`).
This session's base reproduces it (12 of 15 runs removed 0). **opt3 removes
segments in 12 of its 15 runs (median 29 per run, up to 55) and completes 3-6
checkpoint runs in the window in every fast r2 and r3 run against 1-3 in the
base's**:

**The log at the end of the window: segments removed (recycled) and live, per arm (mean of the 15 relaxed load runs)**

| arm | segments removed in window, per run (min / median / max) | runs with 0 removed | live segments at the last roll of the window (median [max]) |
|---|---|---|---|
| base (S1) | 0 / 0 / 4 | 13 of 15 | 38 [39] |
| base | 0 / 0 / 14 | 12 of 15 | 38 [39] |
| opt3 | 0 / 29 / 55 | 3 of 15 | 23 [51] |
| s16 | 0 / 14 / 52 | 3 of 15 | 137 [153] |
| s256 | 0 / 0 / 3 | 10 of 15 | 8 [10] |
| s1024 | 0 / 0 / 1 | 14 of 15 | 2 [2] |

The three opt3 runs that removed nothing in the window are r1 runs (C1 batch0, C2, C3), and
their checkpoint's longest run was 3.9, 20.5 and 17.3 s. This is an association
and the cause is not separated: a faster gate lets the checkpoint's log waits
finish sooner, or a checkpoint that finishes lets the log shrink and the sync
narrow, or the host period decides both. What the data do say is that **the
unrecycled log of BQ-S1's finding (b) is a property of the base's regime, not a
constant of the engine**, and that the live-segment count does not predict the
hold inside opt3 (correlation 0.11 over its 728 rolls; slow rolls' median live
count 18, fast rolls' 15).

## 4. H2: a larger segment makes rolls rare and long, and the time held is the same

### 4.1 What scales with the size

**H2's prediction, clause by clause: what scales with the segment size (every relaxed load cell, 15 runs per arm)**

| arm | log rate (mean of 15 runs) | predicted rolls/s = rate ÷ size | measured rolls/s | hold p50 | held time per 30 s window (median [min, max]) | writer syncs/s (mean) | log per writer sync | rolls whose body was not ready |
|---|---|---|---|---|---|---|---|---|
| base (BQ-S1) (64 MiB) | 74.6 MiB/s | 1.166 /s | 1.164 /s | 331 ms | 12.2 s [10.5, 13.7] | 32.0 /s | 2.36 MiB | 39 of 524 |
| base (64 MiB) | 74.3 MiB/s | 1.161 /s | 1.159 /s | 325 ms | 12.2 s [10.3, 16.3] | 32.5 /s | 2.32 MiB | 46 of 521 |
| opt3 (64 MiB) | 103.2 MiB/s | 1.612 /s | 1.611 /s | 16.0 ms | 3.3 s [1.0, 12.8] | 76.2 /s | 1.78 MiB | 0 of 728 |
| s16 (16 MiB) | 73.4 MiB/s | 4.587 /s | 4.580 /s | 78.2 ms | 12.9 s [9.8, 14.6] | 40.0 /s | 1.88 MiB | 15 of 2064 |
| s256 (256 MiB) | 66.9 MiB/s | 0.261 /s | 0.256 /s | 1.41 s | 13.2 s [9.4, 17.2] | 17.6 /s | 6.78 MiB | 94 of 112 |
| s1024 (1,024 MiB) | 74.9 MiB/s | 0.073 /s | 0.066 /s | 5.03 s | 9.4 s [0.8, 16.9] | 18.7 /s | 10.82 MiB | 28 of 30 |

The roll rate follows the log rate over the segment size to within 0.2 % at 16
and 64 MiB, 2 % at 256 MiB and 10 % at 1,024 MiB (the last two are 7.7 and 2.0
rolls per window, where a segment more or less at a window's edge is a large
fraction). **The hold follows the size instead of staying flat**,
and **the summed hold per window does not fall**. The writer thread's sync
cadence falls with the size too (32.5 syncs/s at 64 MiB, 17.6 and 18.7 at 256
and 1,024 MiB, and in the r1 C3 runs 2.7 and 0.7 per second), which is the
direction the mechanism below predicts.

**Hold by segment size, every relaxed load cell pooled (C1 x 3, C2, C3; 3 runs each)**

| arm | rolls | rolls per 30 s window (mean of 15 runs) | p0 | p25 | p50 | p95 | p99 | max | p50 ÷ segment MiB | summed hold per 30 s window, median [min, max] of 15 runs |
|---|---|---|---|---|---|---|---|---|---|---|
| base (64 MiB) | 521 | 34.8 | 7.9 ms | 276 ms | 325 ms | 693 ms | 809 ms | 4.32 s | 5.08 ms/MiB | 12.2 s [10.3, 16.3] |
| opt3 (64 MiB) | 728 | 48.3 | 9.0 ms | 12.2 ms | 16.0 ms | 760 ms | 1.14 s | 1.50 s | 0.25 ms/MiB | 3.3 s [1.0, 12.8] |
| s16 (16 MiB) | 2064 | 137.4 | 6.6 ms | 58.9 ms | 78.2 ms | 183 ms | 534 ms | 1.31 s | 4.89 ms/MiB | 12.9 s [9.8, 14.6] |
| s256 (256 MiB) | 112 | 7.7 | 346 ms | 1.23 s | 1.41 s | 2.99 s | 3.50 s | 4.56 s | 5.49 ms/MiB | 13.2 s [9.4, 17.2] |
| s1024 (1,024 MiB) | 30 | 2.0 | 236 ms | 4.16 s | 5.03 s | 10.5 s | 11.6 s | 11.6 s | 4.91 ms/MiB | 9.4 s [0.8, 16.9] |

### 4.2 The steps

The gate is the main term at every size and grows with it (median 68 ms,
318 ms, 1.22 s, 2.17 s); **it grows even when the body was ready** (the 18 rolls
of 256 MiB that did not wait for the body hold 1.13 s at the median, gate
1.05 s; the two 1,024 MiB rolls that found their body ready, both in one run,
held 0.24 and 0.56 s). From 256 MiB the
body is missed: step d carries 14.2 % of the summed hold at 256 MiB and 52.0 %
at 1,024 MiB, the missed rolls 89 % and 99.5 %. Seal, flush, header, rename and
directory fsync are flat (about 30 µs, 0, 1.1 ms, 0.03 ms, 2.1 ms); the roll's
own sync is 1 ms except at 16 MiB (4.1 ms), where up to 153 segments are live
because the log is not recycled (the H3 breadth effect, small).

**Step d, the body: outcomes per arm (every relaxed load cell)**

| arm | rolls | hit | waited | inline | missed rolls: summed hold ÷ all hold | body wait + inline build ÷ all hold | C3 only: missed ÷ all hold (r1 / r2 / r3) |
|---|---|---|---|---|---|---|---|
| base (S1) | 524 | 485 | 39 | 0 | 9.0 % | 0.6 % | 8.3 % / 16.3 % / 11.1 % |
| base | 521 | 475 | 46 | 0 | 12.8 % | 0.5 % | 2.6 % / 0.0 % / 2.4 % |
| opt3 | 728 | 728 | 0 | 0 | 0.0 % | 0.0 % | 0.0 % / 0.0 % / 0.0 % |
| s16 | 2064 | 2049 | 15 | 0 | 2.2 % | 0.2 % | 0.0 % / 4.1 % / 0.0 % |
| s256 | 112 | 18 | 94 | 0 | 89.3 % | 14.2 % | 100.0 % / 92.8 % / 83.9 % |
| s1024 | 30 | 2 | 28 | 0 | 99.5 % | 52.0 % | 100.0 % / 100.0 % / 100.0 % |

`[inferred]` The hold/MiB constant (4.9, 5.1, 5.5 and 4.9 ms/MiB) and the
collapsing sync cadence are what a writer `fdatasync` queued behind the
preparer's zero prewrite and `fsync` of S bytes looks like on a device that
drains roughly 200 MiB/s: S/200 MiB/s reproduces the hold, and the fraction
of the window held (about 75/200) does not depend on S. opt3's gate
collapse is the cross-check. The ext4 `data=ordered` journal commit as the
route is a reading, not a trace.

### 4.3 The bulk writer

C3 is term (iii)'s cell; the full table is §5. At 16 MiB the writer is
unchanged (+1.4 % on the mean) and its statement p99 is 103 ms (the base's 10.4 ms) because 2.8 % of its statements meet a roll (138 rolls over about 4,970 statements) against the base's 0.7 %; at 256 and
1,024 MiB the longest statement is 1.8-2.8 s and 5.6-10.5 s.

### 4.4 The stall that remains per roll at the extreme

At 1,024 MiB each run sees two rolls, both on a fresh volume (segments 1 and 2),
holding 4.1-11.6 s (the exception is C2 r1's 0.24 and 0.56 s, whose bodies were
ready). **Each run is a handful of rolls, below §5.1's floor of 50
per arm**; the floor was not unreachable, it was not reached because the
matrix chose 30 s windows. The 256 MiB rolls are 112 in total and cover
segments 1-10, and their holds are the same in every segment (1.05-1.74 s
median per segment), so the young volume is not what makes them long.

### 4.5 The mount

H2's second clause asks whether the size adds more than 1 s to the mount
(`ScanTail`). Probe v2 loads 2.5 segments of log, stops the server cleanly,
and times the restart from exec to the first accepted connection, with
`SHOW META`'s recovery block. **The probe cannot separate the two things that
grow together**: the work order's §1.3 has `ScanTail` read the whole last body
into one allocation, which scales with the segment size, and analysis and redo
scale with the log behind the anchor; the probe writes 2.5 segments at each
size, so the log is 63, 188, 658 and 2,570 MiB for the four sizes.

**Mount probe v2 (clean `STOP`, restart timed from exec to the first accepted connection), two probes per size; the load is 2.5 segments of log at every size**

| segment size | probe | log written before the stop | mount (exec to listening) | completion checkpoint inside it | mount minus the checkpoint | analysis + redo | longest `INSERT` statement of the load |
|---|---|---|---|---|---|---|---|
| 64 MiB | r1 | 188 MiB (2.94 segments) | 0.729 s | 0.679 s | 0.050 s | 0.004 s | 1.67 s |
| 64 MiB | r2 | 188 MiB (2.94 segments) | 0.235 s | 0.179 s | 0.056 s | 0.004 s | 0.31 s |
| 16 MiB | r1 | 63 MiB (3.92 segments) | 0.174 s | 0.154 s | 0.020 s | 0.001 s | 0.12 s |
| 16 MiB | r2 | 63 MiB (3.92 segments) | 0.051 s | 0.031 s | 0.020 s | 0.001 s | 0.05 s |
| 256 MiB | r1 | 658 MiB (2.57 segments) | 1.193 s | 0.942 s | 0.251 s | 0.102 s | 4.22 s |
| 256 MiB | r2 | 658 MiB (2.57 segments) | 0.282 s | 0.010 s | 0.272 s | 0.109 s | 3.36 s |
| 1,024 MiB | r1 | 2,570 MiB (2.51 segments) | 1.161 s | 0.030 s | 1.131 s | 0.521 s | 7.95 s |
| 1,024 MiB | r2 | 2,570 MiB (2.51 segments) | 2.803 s | 1.310 s | 1.493 s | 0.552 s | 12.82 s |

Net of the completion checkpoint (whose own time is the device-stall
thermometer and ranges 0.01-1.31 s here) the mount is 0.02, 0.05, 0.25-0.27
and 1.13-1.49 s; analysis plus redo is 0.001, 0.004, 0.10-0.11 and 0.52-0.55 s
with eight records replayed in every probe, so they are scans, not replays.
**At 1,024 MiB the mount is 1.1-1.4 s above the base's net 0.05 s and about
0.6-0.9 s of it is not attributed to any recovery phase the probe reads**;
how much of it is the volume and how much the segment is not known. The
longest `INSERT` of the load is 7.9 and 12.8 s at 1,024 MiB (0.31-1.67 s at
64 MiB): the probe's one session waits out the first roll's body, which the
preparer builds 1 GiB at a time (28 of the 30 rolls of the load cells waited
on the body; none was built inline).

**Probe v1 is discarded** and its three files are kept only to show why. It
killed the server (SIGKILL) and timed the whole restart, so recovery replayed the whole log (0.1-3.2 million records) and the completion
checkpoint took 0.37-10.7 s (9.2 and 10.7 s at 64 and 256 MiB), which swamped
any tail scan, and its 5 s client timeout lost the
1,024 MiB arm to one stalled `INSERT` and left an orphaned server behind:

**Mount probe v1 (discarded): SIGKILL, restart timed**

| segment size | log written | mount | completion checkpoint | analysis + redo | records replayed |
|---|---|---|---|---|---|
| 64 MiB | 191 MiB | 9.685 s | 9.230 s | 0.403 s | 1,204,136 |
| 16 MiB | 16 MiB | 0.678 s | 0.370 s | 0.040 s | 101,651 |
| 256 MiB | 512 MiB | 12.123 s | 10.745 s | 1.229 s | 3,221,252 |

## 5. Term (iii): the bulk writer

**C3, the bulk writer alone (term iii): rows/s and log MiB/s per run; the delta is the arm's mean against the base mean of this session**

| arm | rows/s r1 / r2 / r3 | mean | spread (max - min) | Δ mean vs base (this session) | Δ per run vs base mean | log MiB/s r1 / r2 / r3 | mean log rate | Δ vs base mean | syncs/s by the writer r1 / r2 / r3 | log MiB per sync (mean) |
|---|---|---|---|---|---|---|---|---|---|---|
| base (BQ-S1) | 169,643 / 168,427 / 169,766 | 169,278 rows/s | 1,339 rows/s (0.8 %) | +0.9 % | +1.1 % / +0.4 % / +1.2 % | 75.8 / 75.2 / 75.8 MiB/s | 75.6 MiB/s | +0.9 % | 29.8 / 31.3 / 27.4 /s | 2.57 MiB |
| base | 169,212 / 164,384 / 169,894 | 167,830 rows/s | 5,509 rows/s (3.3 %) | +0.0 % | +0.8 % / -2.1 % / +1.2 % | 75.6 / 73.4 / 75.9 MiB/s | 75.0 MiB/s | +0.0 % | 34.3 / 25.2 / 26.7 /s | 2.65 MiB |
| opt3 | 141,243 / 304,305 / 273,802 | 239,784 rows/s | 163,062 rows/s (68.0 %) | +42.9 % | -15.8 % / +81.3 % / +63.1 % | 60.0 / 135.9 / 122.3 MiB/s | 106.0 MiB/s | +41.5 % | 28.2 / 95.8 / 78.1 /s | 1.70 MiB |
| s16 | 170,262 / 170,277 / 169,949 | 170,162 rows/s | 328 rows/s (0.2 %) | +1.4 % | +1.4 % / +1.5 % / +1.3 % | 75.8 / 76.1 / 75.9 MiB/s | 75.9 MiB/s | +1.3 % | 34.9 / 34.2 / 33.8 /s | 2.21 MiB |
| s256 | 119,302 / 179,334 / 178,928 | 159,188 rows/s | 60,033 rows/s (37.7 %) | -5.1 % | -28.9 % / +6.9 % / +6.6 % | 49.3 / 75.9 / 75.9 MiB/s | 67.0 MiB/s | -10.6 % | 2.7 / 15.7 / 16.3 /s | 9.18 MiB |
| s1024 | 156,197 / 180,917 / 188,004 | 175,040 rows/s | 31,807 rows/s (18.2 %) | +4.3 % | -6.9 % / +7.8 % / +12.0 % | 52.7 / 80.8 / 84.0 MiB/s | 72.5 MiB/s | -3.3 % | 0.7 / 8.8 / 8.3 /s | 32.72 MiB |

**C3 inserter statement latency (one 1,024-row `INSERT` per statement, closed loop), median of the three runs per percentile**

| arm | statements per run (mean) | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| base (BQ-S1) | 4,958 | 2.5 ms | 2.6 ms | 2.7 ms | 3.2 ms | 10.9 ms | 452 ms | 798 ms |
| base | 4,896 | 2.5 ms | 2.6 ms | 2.7 ms | 3.1 ms | 10.4 ms | 500 ms | 813 ms |
| opt3 | 6,971 | 2.5 ms | 2.7 ms | 2.8 ms | 3.6 ms | 16.7 ms | 155 ms | 842 ms |
| s16 | 4,972 | 2.5 ms | 2.6 ms | 2.7 ms | 3.6 ms | 103 ms | 202 ms | 614 ms |
| s256 | 4,487 | 2.5 ms | 2.7 ms | 2.7 ms | 3.1 ms | 8.7 ms | 1.48 s | 2.23 s |
| s1024 | 4,852 | 2.5 ms | 2.7 ms | 2.8 ms | 3.3 ms | 8.0 ms | 267 ms | 6.04 s |

**Bulk writer throughput inside every cell (rows/s, mean of the three runs; range in brackets)**

| cell | base (BQ-S1) | base | opt3 | s16 | s256 | s1024 |
|---|---|---|---|---|---|---|
| c3 | 169,278 rows/s [168,427-169,766] | 167,830 rows/s [164,384-169,894] | 239,784 rows/s [141,243-304,305] | 170,162 rows/s [169,949-170,277] | 159,188 rows/s [119,302-179,334] | 175,040 rows/s [156,197-188,004] |
| c2 | 164,483 rows/s [162,743-165,837] | 159,694 rows/s [145,977-168,306] | 210,279 rows/s [85,680-274,358] | 161,378 rows/s [157,020-163,932] | 156,677 rows/s [117,593-177,302] | 179,276 rows/s [164,845-191,236] |
| c1-segregated-no0 | 171,574 rows/s [169,106-175,835] | 171,522 rows/s [169,057-175,643] | 203,799 rows/s [120,777-254,751] | 162,755 rows/s [148,427-170,160] | 154,437 rows/s [123,912-170,718] | 175,290 rows/s [169,484-185,085] |
| c1-batch0 | 167,662 rows/s [158,427-174,926] | 169,387 rows/s [168,425-170,361] | 246,091 rows/s [234,982-254,850] | 165,163 rows/s [154,783-170,901] | 149,250 rows/s [114,360-168,259] | 173,286 rows/s [168,251-177,513] |
| c1-spread | 166,178 rows/s [160,996-169,056] | 169,961 rows/s [169,574-170,238] | 261,721 rows/s [250,973-281,655] | 164,312 rows/s [152,709-170,137] | 150,973 rows/s [119,603-167,195] | 174,056 rows/s [166,853-181,396] |

**Longest bulk `INSERT` statement (max of the inserter column, median of three runs)**

| cell | base (BQ-S1) | base | opt3 | s16 | s256 | s1024 |
|---|---|---|---|---|---|---|
| c3 | 677 ms / 915 ms / 798 ms | 852 ms / 813 ms / 757 ms | 1.29 s / 313 ms / 842 ms | 616 ms / 614 ms / 576 ms | 2.80 s / 1.83 s / 2.23 s | 10.5 s / 5.64 s / 6.04 s |
| c2 | 794 ms / 1.21 s / 750 ms | 4.32 s / 1.04 s / 1.01 s | 1.46 s / 1.15 s / 1.14 s | 1.31 s / 1.25 s / 922 ms | 4.56 s / 2.27 s / 1.92 s | 902 ms / 5.73 s / 5.72 s |
| c1-segregated-no0 | 715 ms / 682 ms / 592 ms | 773 ms / 633 ms / 774 ms | 1.01 s / 203 ms / 282 ms | 434 ms / 589 ms / 559 ms | 3.46 s / 1.52 s / 1.69 s | 5.52 s / 4.70 s / 4.91 s |
| c1-batch0 | 657 ms / 728 ms / 729 ms | 775 ms / 795 ms / 649 ms | 1.38 s / 295 ms / 577 ms | 825 ms / 609 ms / 503 ms | 3.07 s / 1.39 s / 1.45 s | 5.20 s / 5.09 s / 5.32 s |
| c1-spread | 673 ms / 793 ms / 832 ms | 771 ms / 794 ms / 789 ms | 907 ms / 68.6 ms / 1.02 s | 537 ms / 571 ms / 599 ms | 2.89 s / 1.53 s / 1.33 s | 5.16 s / 4.81 s / 5.03 s |

**Term (iii) (C3 loses no more than 3 %) cannot be resolved with three runs.**
The base's own spread is 3.3 % of its mean, and the S1 base differs from this
session's by +0.9 %. By mean: s16 is +1.4 % (met), opt3 +42.9 % (met), s256
-5.1 % (missed), s1024 +4.3 % on rows/s and -3.3 % on log MiB/s (the two
measures of the same runs disagree in sign: undetermined). By run, the r1 runs
of opt3 (-15.8 %), s256 (-28.9 %) and s1024 (-6.9 %) fall below the base mean
and ran in the first, non-interleaved blocks (§0.3); what made them slow is not
identified. A gain of 63-81 % for opt3 in r2 and r3 is large against any
noise floor here, but its mean carries a 68 % spread.

## 6. The delta against BQ-S1's base, and the noise floor

The base arm is the control that cannot be affected by the change: the same
binary as BQ-S1's, the same driver cells, a different session, hours later.

**The delta against BQ-S1's base: the same binary (`4cf30270`, sha256 `024ef4dc...`), the same cells, 3 runs each; the ratio is S2 median ÷ S1 median**

| cell | figure | BQ-S1 base r1 / r2 / r3 | BQ-S2 base r1 / r2 / r3 | S1 spread | S2 spread | median ratio S2 ÷ S1 |
|---|---|---|---|---|---|---|
| c1-segregated-no0 | schedule p50 | 206 µs / 208 µs / 202 µs | 210 µs / 205 µs / 202 µs | 6.5 µs | 7.9 µs | 0.99x |
| c1-segregated-no0 | schedule p99 | 306 ms / 211 ms / 288 ms | 451 ms / 381 ms / 312 ms | 95.8 ms | 140 ms | 1.32x |
| c1-segregated-no0 | schedule p99.9 | 626 ms / 296 ms / 496 ms | 659 ms / 568 ms / 669 ms | 330 ms | 101 ms | 1.33x |
| c1-segregated-no0 | schedule max | 697 ms / 360 ms / 584 ms | 757 ms / 618 ms / 758 ms | 337 ms | 141 ms | 1.30x |
| c1-segregated-no0 | hold p50 (rolls of the run) | 297 ms / 283 ms / 331 ms | 271 ms / 324 ms / 286 ms | 48.0 ms | 52.8 ms | 0.96x |
| c1-segregated-no0 | hold p99 (rolls of the run) | 710 ms / 678 ms / 587 ms | 769 ms / 629 ms / 768 ms | 123 ms | 141 ms | 1.13x |
| c1-segregated-no0 | rolls in window | 35 / 35 / 37 | 35 / 36 / 36 | 2 | 1 | 1.03x |
| c1-segregated-no0 | summed hold per 30 s | 11.5 s / 10.5 s / 12.0 s | 11.1 s / 12.6 s / 11.1 s | 1.50 s | 1.51 s | 0.96x |
| c1-segregated-no0 | bulk writer rows/s | 169,106 rows/s / 169,783 rows/s / 175,835 rows/s | 169,866 rows/s / 175,643 rows/s / 169,057 rows/s | 6,730 rows/s | 6,586 rows/s | 1.00x |
| c1-batch0 | schedule p50 | 211 µs / 193 µs / 221 µs | 204 µs / 208 µs / 192 µs | 27.7 µs | 16.5 µs | 0.96x |
| c1-batch0 | schedule p99 | 430 ms / 354 ms / 366 ms | 224 ms / 460 ms / 241 ms | 75.6 ms | 236 ms | 0.66x |
| c1-batch0 | schedule p99.9 | 593 ms / 674 ms / 624 ms | 373 ms / 691 ms / 474 ms | 81.1 ms | 319 ms | 0.76x |
| c1-batch0 | schedule max | 645 ms / 718 ms / 713 ms | 551 ms / 787 ms / 634 ms | 73.3 ms | 236 ms | 0.89x |
| c1-batch0 | hold p50 (rolls of the run) | 326 ms / 291 ms / 317 ms | 281 ms / 310 ms / 316 ms | 35.7 ms | 34.7 ms | 0.98x |
| c1-batch0 | hold p99 (rolls of the run) | 654 ms / 722 ms / 725 ms | 771 ms / 791 ms / 643 ms | 71.1 ms | 148 ms | 1.07x |
| c1-batch0 | rolls in window | 36 / 35 / 33 | 35 / 35 / 34 | 3 | 1 | 1.00x |
| c1-batch0 | summed hold per 30 s | 12.2 s / 11.3 s / 11.2 s | 10.7 s / 12.2 s / 11.7 s | 995 ms | 1.48 s | 1.03x |
| c1-batch0 | bulk writer rows/s | 174,926 rows/s / 169,634 rows/s / 158,427 rows/s | 169,374 rows/s / 170,361 rows/s / 168,425 rows/s | 16,498 rows/s | 1,936 rows/s | 1.00x |
| c1-spread | schedule p50 | 197 µs / 212 µs / 217 µs | 205 µs / 199 µs / 200 µs | 19.8 µs | 6.0 µs | 0.94x |
| c1-spread | schedule p99 | 205 ms / 508 ms / 452 ms | 312 ms / 247 ms / 325 ms | 303 ms | 77.6 ms | 0.69x |
| c1-spread | schedule p99.9 | 475 ms / 1.00 s / 735 ms | 619 ms / 376 ms / 707 ms | 526 ms | 330 ms | 0.84x |
| c1-spread | schedule max | 563 ms / 1.06 s / 810 ms | 753 ms / 399 ms / 771 ms | 496 ms | 372 ms | 0.93x |
| c1-spread | hold p50 (rolls of the run) | 271 ms / 308 ms / 326 ms | 263 ms / 271 ms / 282 ms | 54.3 ms | 18.9 ms | 0.88x |
| c1-spread | hold p99 (rolls of the run) | 668 ms / 788 ms / 828 ms | 767 ms / 790 ms / 784 ms | 160 ms | 23.2 ms | 0.99x |
| c1-spread | rolls in window | 35 / 35 / 34 | 35 / 35 / 36 | 1 | 1 | 1.00x |
| c1-spread | summed hold per 30 s | 10.5 s / 11.8 s / 12.7 s | 10.3 s / 10.5 s / 10.8 s | 2.17 s | 487 ms | 0.89x |
| c1-spread | bulk writer rows/s | 168,483 rows/s / 169,056 rows/s / 160,996 rows/s | 170,069 rows/s / 169,574 rows/s / 170,238 rows/s | 8,061 rows/s | 664 rows/s | 1.01x |
| c2 | schedule p50 | 2.4 ms / 7.1 ms / 291 µs | 53.0 ms / 353 µs / 357 µs | 6.8 ms | 52.7 ms | 0.15x |
| c2 | schedule p99 | 658 ms / 1.30 s / 659 ms | 4.03 s / 773 ms / 752 ms | 637 ms | 3.27 s | 1.17x |
| c2 | schedule p99.9 | 778 ms / 1.72 s / 878 ms | 4.29 s / 1.00 s / 973 ms | 941 ms | 3.32 s | 1.14x |
| c2 | schedule max | 820 ms / 1.78 s / 1.09 s | 4.32 s / 1.03 s / 1.00 s | 956 ms | 3.32 s | 0.94x |
| c2 | hold p50 (rolls of the run) | 369 ms / 374 ms / 365 ms | 366 ms / 358 ms / 361 ms | 8.8 ms | 8.5 ms | 0.98x |
| c2 | hold p99 (rolls of the run) | 789 ms / 1.21 s / 746 ms | 4.32 s / 1.03 s / 1.00 s | 464 ms | 3.32 s | 1.30x |
| c2 | rolls in window | 34 / 33 / 34 | 30 / 34 / 35 | 1 | 5 | 1.00x |
| c2 | summed hold per 30 s | 13.6 s / 13.7 s / 13.3 s | 16.3 s / 13.1 s / 13.6 s | 427 ms | 3.23 s | 1.00x |
| c2 | bulk writer rows/s | 164,869 rows/s / 162,743 rows/s / 165,837 rows/s | 145,977 rows/s / 164,800 rows/s / 168,306 rows/s | 3,094 rows/s | 22,328 rows/s | 1.00x |
| c3 | hold p50 (rolls of the run) | 359 ms / 364 ms / 349 ms | 350 ms / 403 ms / 373 ms | 14.8 ms | 53.1 ms | 1.04x |
| c3 | hold p99 (rolls of the run) | 673 ms / 911 ms / 794 ms | 847 ms / 809 ms / 753 ms | 238 ms | 94.1 ms | 1.02x |
| c3 | rolls in window | 36 / 36 / 36 | 36 / 33 / 36 | 0 | 3 | 1.00x |
| c3 | summed hold per 30 s | 13.5 s / 13.3 s / 12.8 s | 13.2 s / 14.5 s / 14.5 s | 685 ms | 1.39 s | 1.10x |
| c3 | bulk writer rows/s | 169,643 rows/s / 168,427 rows/s / 169,766 rows/s | 169,212 rows/s / 164,384 rows/s / 169,894 rows/s | 1,339 rows/s | 5,509 rows/s | 1.00x |

**Every roll and throughput figure reproduces**: hold p50 0.88-1.04x, rolls
per window 1.00-1.03x, bulk rows/s 1.00x-1.01x in every cell, summed hold 0.89-1.10x.
**The latency percentiles do not reproduce tightly** and are inside the two
sessions' own spreads: C1 p99 ratios are 1.32x, 0.66x and 0.69x with spreads of
95.8 / 75.6 / 303 ms (S1) and 140 / 236 / 77.6 ms (S2). The sessions' shared
bimodal C2 p50 (a run's median is either 0.3 ms or milliseconds) is stated in
§2. **A delta between arms smaller than these is not a finding**; the findings
above exceed them in the median and, for opt3's C1 p99, not in every run.

## 7. Accounting for the waits

The unit is one reader sample (C1) or one writer sample (C2). Its schedule
latency is a sum, and its parts are these:

| Part | What carries it | Measured |
|---|---|---|
| client and socket round trip, and the read | the isolated arm's service p50 (105-120 µs C1, 91 µs C2); the mixed arms' service p50 is 121-136 µs C1 and 100-110 µs C2 | table in §2: it moves by 10-30 µs under any arm and is not a part of the tail |
| late send | schedule minus service at p50 (about 70-85 µs, the open-loop timer's slack) | same table |
| durability / commit (fsync) | a statement does not wait for an fsync at `relaxed`; the writers' commit is the statement's round trip | does not apply per statement at this durability; the roll's own syncs are the next rows |
| the wait for a roll's hold | the stream latch a roll holds, summed over cores (below), and core 0's drain which waits it out | 10.5 s per 30 s window in C1 for the base, 3.9 s for opt3, 11.4-13.3 s for the other sizes; 100 % of the stream-latch wait is covered by a roll hold in every arm |
| the roll's own waits | the gate on the writer thread's sync, the body, the header, the directory fsync | §3.3, §4.2 |
| lock and conflict wait | `contention_lock_partition_wait_us` | 0.000-0.002 s per window in every arm: not a part |
| write statement | the bulk `INSERT` (C3) | statement p50 2.7-2.8 ms per 1,024 rows in every arm (§5) |

**Where the waits are, summed over the four cores per 30 s window (mean of the arm's runs; `SHOW META` `contention_*` deltas)**

The stream latch is the one a roll holds. "Blocked by a roll" is the part of the stream-latch wait the roll's hold covered; "drain" is core 0's group-commit drain; the sync gate is the wait on the WAL writer's sync in flight. Other latches are the ones this workload moves at all.

| cell group | arm | runs | roll hold, summed | stream-latch wait | of which covered by a roll hold | drain's share of it | sync-gate wait | window latch | frame-table | lock partition |
|---|---|---|---|---|---|---|---|---|---|---|
| C1 (3 placements) | base | 9 | 11.2 s | 10.5 s | 10.5 s (100 %) | 9.9 s (95 %) | 10.8 s | 1.00 s | 1.52 s | 0.001 s |
| C1 (3 placements) | opt3 | 9 | 3.4 s | 3.9 s | 3.9 s (100 %) | 3.1 s (79 %) | 2.3 s | 1.14 s | 2.00 s | 0.001 s |
| C1 (3 placements) | s16 | 9 | 11.1 s | 11.4 s | 11.4 s (100 %) | 10.2 s (89 %) | 9.1 s | 0.89 s | 1.45 s | 0.001 s |
| C1 (3 placements) | s256 | 9 | 11.3 s | 12.7 s | 12.7 s (100 %) | 10.5 s (83 %) | 9.5 s | 0.96 s | 1.40 s | 0.002 s |
| C1 (3 placements) | s1024 | 9 | 9.9 s | 13.3 s | 13.3 s (100 %) | 9.9 s (74 %) | 4.6 s | 0.96 s | 1.61 s | 0.002 s |
| C2 | base | 3 | 14.3 s | 41.5 s | 41.5 s (100 %) | 5.7 s (14 %) | 13.3 s | 0.06 s | 0.19 s | 0.001 s |
| C2 | opt3 | 3 | 7.4 s | 19.7 s | 19.7 s (100 %) | 3.3 s (16 %) | 5.5 s | 0.05 s | 0.17 s | 0.000 s |
| C2 | s16 | 3 | 14.2 s | 40.2 s | 40.1 s (100 %) | 3.2 s (8 %) | 12.1 s | 0.05 s | 0.18 s | 0.001 s |
| C2 | s256 | 3 | 14.2 s | 41.8 s | 41.8 s (100 %) | 3.4 s (8 %) | 9.8 s | 0.06 s | 0.18 s | 0.000 s |
| C2 | s1024 | 3 | 7.4 s | 21.9 s | 21.9 s (100 %) | 0.0 s (0 %) | 3.6 s | 0.06 s | 0.21 s | 0.000 s |
| C3 | base | 3 | 14.1 s | 14.2 s | 14.2 s (100 %) | 13.8 s (97 %) | 13.6 s | 0.04 s | 0.07 s | 0.000 s |
| C3 | opt3 | 3 | 5.6 s | 5.5 s | 5.5 s (100 %) | 5.2 s (94 %) | 3.6 s | 0.05 s | 0.13 s | 0.000 s |
| C3 | s16 | 3 | 13.8 s | 13.3 s | 13.3 s (100 %) | 12.6 s (95 %) | 11.6 s | 0.03 s | 0.06 s | 0.000 s |
| C3 | s256 | 3 | 14.5 s | 16.2 s | 16.2 s (100 %) | 14.5 s (89 %) | 10.8 s | 0.05 s | 0.06 s | 0.000 s |
| C3 | s1024 | 3 | 13.2 s | 17.3 s | 17.3 s (100 %) | 13.2 s (76 %) | 6.2 s | 0.13 s | 0.07 s | 0.000 s |

The same for BQ-S1's base, for the same groups:

| cell group | arm | runs | roll hold, summed | stream-latch wait | of which covered by a roll hold | drain's share of it | sync-gate wait | window latch | frame-table | lock partition |
|---|---|---|---|---|---|---|---|---|---|---|
| C1 (3 placements) | base (BQ-S1) | 9 | 11.5 s | 11.0 s | 11.0 s (100 %) | 10.6 s (96 %) | 11.1 s | 0.76 s | 1.48 s | 0.001 s |
| C2 | base (BQ-S1) | 3 | 13.5 s | 38.5 s | 38.5 s (100 %) | 4.5 s (12 %) | 12.5 s | 0.04 s | 0.21 s | 0.001 s |
| C3 | base (BQ-S1) | 3 | 13.2 s | 13.7 s | 13.7 s (100 %) | 13.0 s (95 %) | 12.8 s | 0.04 s | 0.07 s | 0.000 s |

The stream-latch wait is the roll's hold, seen from the other side: it is
**100 % covered by a roll hold in every arm and cell group**, it is 95 % core
0's drain in C1 at the base (79 % at opt3, 74 % at 1,024 MiB), and the sync-gate
wait (the roll's wait for the writer's sync, summed over the cores that wait on
it) follows the hold (10.8 s base, 2.3 s opt3 in C1). Neither the window latch
nor the frame table nor the lock partitions moves with the arm.

## 8. Host, noise floor and defects of this run

**The load average and the competing jobs of every cell are in Appendix A2**,
read by the driver before the server started and after the window.

- **The gate missed other sessions' builds in seven cells.** The matrix's
  `busy()` drops every `pgrep` line that contains `shell-snapshots`, which is
  every command another session launches from its shell. The driver's own
  `competing` list caught them: pid 744955 (`cmake --build build -j8` and a
  simulation sweep) during the five 1,024 MiB r2 cells and 256 MiB
  c1-spread r2, pid 746465 (`cmake --build ... br-backup-restore` and `ctest -j8`)
  after 1,024 MiB c1-spread r2, and a `kds_tests` build and gtest after 16 MiB
  c1-batch0 r1. Dropping those runs changes no verdict: each arm's other runs
  miss the same terms. It breaks the quiet-host requirement (the ck-tester's third
  pre-measurement check); `bench/README.md`'s fourth rule (record the load and
  the competing jobs per cell) is met only because the driver records the list
  itself.
- **BQ-S2's isolated arms are inflated.** They ran 17:13-17:19 with
  five-minute load averages of 6.1-8.8; the C1 isolated p99 is 602-853 µs
  against BQ-S1's 206-561 µs, and **the C2 isolated p99 is 141 ms and p99.9 467
  ms against 213-487 µs and 1.8-3.1 ms**, with no roll in the window: a carve of
  542 ms, a checkpoint of 948 ms and a 292 ms reactor hold out of its run. The
  group C2 isolated cell is worse: its writers completed 14,557 of 24,007
  samples with a schedule p50 of **16.6 s** (service p50 1.96 ms, p99 344 ms;
  BQ-S1's: 1.1 ms and 10.3 ms), checkpoint 2.9 s. None of the isolated group
  cells is used. Every ratio against BQ-S2's own isolated arms is therefore an
  understatement of the miss, and BQ-S1's isolated arms are the baseline.
- **The device-stall thermometer** (the mount checkpoint of a fresh file;
  BQ-S1's median 10.5 ms, maximum 21 ms) is 7-18 ms in the relaxed isolated
  cells and 10.7 ms at the median of the 60 mixed runs; it flags the four group
  isolated cells (161-641 ms) and six cells, all r1: C1 segregated-no0 256 MiB
  (805 ms), C3 256 MiB (639 ms), C1 segregated-no0 opt3 (607 ms), C3 1,024 MiB
  (352 ms), C2 opt3 (324 ms), C2 1,024 MiB (260 ms). The r1 cells ran in the
  matrix's first, blocked pass (§0.3); what preceded each is in the timeline,
  and the cause is not separated.
  The base's own 4.3 s roll in C2 r1 (18:14) has a thermometer of 11 ms and is
  unexplained.
- **A cell starts on whatever the previous cell left in the page cache.**
  Nothing quiesces the device between cells (2-3 GiB of log per cell), which
  is a candidate carrier of the r1 effects above and cannot be separated from
  them here.
- **Samples in flight at the window's end are lost** (§0.2).
- **The orphaned probe server** (§0.3) ran idle on port 17571, pinned to the
  server CPUs, beside `c2/s256/r1`, which has 256 MiB's highest C2 p99
  (4.26 s) and its largest hold (4.56 s); that run is kept, and the 256 MiB C2
  medians use r2 and r3 as well.
- **The matrix is not cell-interleaved** (§0.3), and the base's r1 block sits
  50 minutes after opt3's.
- **No tool defect reaches a figure here**: `bl_mixed.py`'s `meta_delta` was
  fixed before the run; the analysis reads the raw JSON and the roll lines and
  uses no `analyze_s1.py` code.

## 9. What the run teaches about the engine

1. **The hold is a sync queued behind writes the roll itself caused, and
   which writes depends on the build.** The base waits a median 318 ms for the
   writer's tail sync; with the preparer's 64 MiB zero write out of the queue
   (opt3) the median gate is 5.2 ms; with a larger segment it is multiplied
   (68 ms, 318 ms, 1.22 s, 2.17 s). Three configurations, one reading,
   `[inferred]` (nothing traced the device): the roll's own next-segment build
   shares the device with the sync the roll waits for. The roll's durability
   order (BQ-R1) is untouched by any of this; what the data put in question
   is the order in which the *device* sees the roll's work.
2. **Once the roll is short, it is no longer what sets the readers' p99.** In
   opt3's best run (c1-spread r2: no roll over 150 ms, hold p50 14 ms, summed
   hold 1.0 s, 3.2 % of the window) the readers' p99 is 22.4 ms and p99.9
   67.2 ms, and only 106 of its 475 slow samples overlap a roll; removing
   those leaves p99 at 18.9 ms (§2.3). The same run's counters show other
   candidates, none attributed: 14 `system` polls over 10 ms (longest 66 ms),
   `sched_hold_in_run_over_10ms` 17 and `sched_hold_out_run_over_10ms` 32, a
   checkpoint step of 65 ms, and 44 balancer moves. BQ-Q7 (a) treats group C2's miss the
   same way (a stall with another cause is named by its holder); this is the
   same finding at `relaxed` once the roll is cheap. **A rung that shrinks the
   hold further has an unmeasured holder behind it.**
3. **The floor of the hold, and what it implies for a rarer roll.** opt3's
   rolls have p0 9.0 ms and p5 9.9 ms (the tail sync, the header's
   `fdatasync`, the directory `fsync`). At the roll rates measured this is a
   minimum of 0.92 % of the window under a hold for the base (7.9 ms x 1.16
   per second), 1.45 % for opt3 (9.0 ms x 1.61) and 3.0 % for 16 MiB (6.6 ms x
   4.58). This is arithmetic over measured p0 and roll rate, not a prediction
   of any run: it is the least a session on the stream latch's cores can find
   stalled. H2 spends the rate to lower it, and the data show the hold growing
   with the size.
4. **H2 and H4 were measured apart, and the matrix has no cell with both.** The
   work order's clause "if H4 lands, H2 is re-run on H4's build" did not
   trigger, because H4 did not land. But the 5 ms/MiB by which the hold grows
   and the gate opt3 removes are, under the reading in §4.2, the same zero
   fill, so the one cell that separates them (opt3 at a large segment) was
   not in the ladder. Whether to run it is a call for the order's owner.
5. **BQ-S1's finding (b), the unrecycled log, is regime-dependent.** opt3
   removes segments in 12 of 15 runs; the live-segment count at the last roll
   is 38 [max 39] at the base, 23 [max 51] at opt3 and 2 at 1,024 MiB.
   `known-gaps.md`'s WAL entry "Every sync covers every live segment" and the
   bug entry on the redo start price a count that is the base's, not a
   constant (§3.4).
6. **Where this contradicts a stated expectation.**
   `workorder-bq-wal-segment-roll.md` §1.3 says "steps c, e, f and g do not
   scale with the size"; step c does (the gate grows 68 ms to 2.17 s across
   the sizes, and for the 256 MiB rolls whose body was ready). H2's prediction
   that the hold "stays flat, or grows where step d misses" is refuted for the
   same reason. §1.3's other point stands: step d does scale, and from 256
   MiB it is missed in 84-93 % of rolls although the build is asked for at
   half a segment.
7. **BQ closes on §0.4's fourth row, and the cost it names is this**: at
   `relaxed` and cores = 4 the latched roll holds a median 325 ms (opt3: 16
   ms) once per 0.86 s (opt3: 0.62 s), the readers' p99 is two to four orders
   of magnitude over BQ-S1's isolated arm in every arm, and the only rung that
   changes the structure (H6, release the latch for the roll's I/O) is not
   licensed (BQ-Q5 (b)). The close rests on BQ-Q6 (a), which CLA adopted and
   the operator did not mark; §2.3 shows its H5-type bound holds on opt3's
   data as on the base's, except in one base run, and that on opt3 the bound
   is set by a holder other than the roll.

## 10. What this file does not establish

- **Group.** No mixed `group` cell ran for any arm. The `group` bar, H4
  option 3's C2 commit-p99 guard and BQ-Q7 (a)'s group C1 judgement are
  unevaluated.
- **The mechanism** of the gate, of the hold's 5 ms/MiB and of the mount cost is
  inferred, not traced.
- **A longer window.** Whether opt3's fast mode holds beyond 30 s is not
  measured; the slow mode appears 9-30 s into the window in r2 and r3.
- **The combination** of opt3 with a large segment, and rungs 2 and 3 on any
  build, were not run; §2.3 is a ceiling over recorded samples, not a run.
- **Tests.** No suite was run against the measurement builds, which change how
  segments are created (opt3) and the segment constant.
- **Overhead of BQ's instruments** (BQ-S5), the three-run C3 resolution of a
  3 % term (§5), and the PostgreSQL floor for this shape.

## Appendix A. Every run

**Appendix A1. Every run: schedule latency, relaxed**

| cell | arm | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max | errors / refusals | slow samples dropped (reader or writer clients) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| c1-segregated-no0 | c1-iso-segregated-no0 (BQ-S2) | r1 | 12,006 | 122 µs | 174 µs | 181 µs | 230 µs | 853 µs | 3.0 ms | 4.6 ms | 0 / 0 | 0 |
| c1-segregated-no0 | c1-iso-segregated-no0 (BQ-S1) | r1 | 12,006 | 112 µs | 168 µs | 174 µs | 199 µs | 279 µs | 2.5 ms | 4.1 ms | 0 / 0 | 0 |
| c1-segregated-no0 | c1-iso-segregated-no0 (BQ-S1) | r2 | 12,006 | 120 µs | 168 µs | 174 µs | 198 µs | 216 µs | 803 µs | 4.1 ms | 0 / 0 | 0 |
| c1-segregated-no0 | c1-iso-segregated-no0 (BQ-S1) | r3 | 12,006 | 117 µs | 168 µs | 174 µs | 202 µs | 561 µs | 2.5 ms | 4.0 ms | 0 / 0 | 0 |
| c1-segregated-no0 | base | r1 | 11,997 | 116 µs | 185 µs | 210 µs | 158 ms | 451 ms | 659 ms | 757 ms | 0 / 0 | 0 |
| c1-segregated-no0 | base | r2 | 12,006 | 110 µs | 186 µs | 205 µs | 199 ms | 381 ms | 568 ms | 618 ms | 0 / 0 | 0 |
| c1-segregated-no0 | base | r3 | 12,006 | 116 µs | 183 µs | 202 µs | 123 ms | 312 ms | 669 ms | 758 ms | 0 / 0 | 0 |
| c1-segregated-no0 | opt3 | r1 | 12,006 | 121 µs | 190 µs | 223 µs | 272 ms | 640 ms | 906 ms | 995 ms | 0 / 0 | 0 |
| c1-segregated-no0 | opt3 | r2 | 12,006 | 131 µs | 189 µs | 202 µs | 467 µs | 17.4 ms | 87.5 ms | 192 ms | 0 / 0 | 0 |
| c1-segregated-no0 | opt3 | r3 | 11,994 | 105 µs | 193 µs | 209 µs | 393 µs | 42.0 ms | 194 ms | 783 ms | 0 / 0 | 0 |
| c1-segregated-no0 | s16 | r1 | 12,006 | 121 µs | 189 µs | 208 µs | 102 ms | 320 ms | 600 ms | 636 ms | 0 / 0 | 0 |
| c1-segregated-no0 | s16 | r2 | 12,006 | 136 µs | 190 µs | 208 µs | 38.1 ms | 160 ms | 416 ms | 563 ms | 0 / 0 | 0 |
| c1-segregated-no0 | s16 | r3 | 12,006 | 115 µs | 185 µs | 205 µs | 42.8 ms | 107 ms | 398 ms | 545 ms | 0 / 0 | 0 |
| c1-segregated-no0 | s256 | r1 | 11,861 | 124 µs | 191 µs | 223 µs | 2.21 s | 4.91 s | 5.71 s | 5.78 s | 0 / 0 | 0 |
| c1-segregated-no0 | s256 | r2 | 11,997 | 123 µs | 183 µs | 202 µs | 429 ms | 1.03 s | 1.35 s | 1.51 s | 0 / 0 | 0 |
| c1-segregated-no0 | s256 | r3 | 12,006 | 116 µs | 184 µs | 208 µs | 281 ms | 951 ms | 1.17 s | 1.26 s | 0 / 0 | 0 |
| c1-segregated-no0 | s1024 | r1 | 11,450 | 104 µs | 176 µs | 188 µs | 311 µs | 2.5 ms | 160 ms | 11.6 s | 0 / 0 | 0 |
| c1-segregated-no0 | s1024 | r2 | 12,006 | 130 µs | 213 µs | 260 µs | 2.75 s | 4.18 s | 4.52 s | 4.70 s | 0 / 0 | 0 |
| c1-segregated-no0 | s1024 | r3 | 12,006 | 97.0 µs | 186 µs | 201 µs | 1.71 s | 3.63 s | 4.72 s | 4.90 s | 0 / 0 | 0 |
| c1-batch0 | c1-iso-batch0 (BQ-S2) | r1 | 12,006 | 127 µs | 172 µs | 178 µs | 202 µs | 602 µs | 2.4 ms | 4.4 ms | 0 / 0 | 0 |
| c1-batch0 | c1-iso-batch0 (BQ-S1) | r1 | 12,006 | 118 µs | 171 µs | 177 µs | 213 µs | 379 µs | 2.3 ms | 3.8 ms | 0 / 0 | 0 |
| c1-batch0 | c1-iso-batch0 (BQ-S1) | r2 | 12,006 | 120 µs | 168 µs | 174 µs | 190 µs | 206 µs | 1.0 ms | 2.1 ms | 0 / 0 | 0 |
| c1-batch0 | c1-iso-batch0 (BQ-S1) | r3 | 12,006 | 120 µs | 171 µs | 177 µs | 199 µs | 354 µs | 1.6 ms | 4.1 ms | 0 / 0 | 0 |
| c1-batch0 | base | r1 | 12,006 | 120 µs | 183 µs | 204 µs | 23.4 ms | 224 ms | 373 ms | 551 ms | 0 / 0 | 0 |
| c1-batch0 | base | r2 | 12,006 | 140 µs | 186 µs | 208 µs | 189 ms | 460 ms | 691 ms | 787 ms | 0 / 0 | 0 |
| c1-batch0 | base | r3 | 12,006 | 121 µs | 180 µs | 192 µs | 548 µs | 241 ms | 474 ms | 634 ms | 0 / 0 | 0 |
| c1-batch0 | opt3 | r1 | 12,006 | 122 µs | 192 µs | 209 µs | 854 µs | 231 ms | 1.19 s | 1.37 s | 0 / 0 | 0 |
| c1-batch0 | opt3 | r2 | 12,006 | 124 µs | 192 µs | 210 µs | 504 µs | 42.6 ms | 203 ms | 280 ms | 0 / 0 | 0 |
| c1-batch0 | opt3 | r3 | 12,006 | 114 µs | 192 µs | 207 µs | 419 µs | 71.2 ms | 490 ms | 550 ms | 0 / 0 | 0 |
| c1-batch0 | s16 | r1 | 12,006 | 128 µs | 196 µs | 234 µs | 33.5 ms | 245 ms | 703 ms | 811 ms | 0 / 0 | 0 |
| c1-batch0 | s16 | r2 | 12,006 | 98.2 µs | 186 µs | 203 µs | 48.1 ms | 303 ms | 555 ms | 597 ms | 0 / 0 | 0 |
| c1-batch0 | s16 | r3 | 12,006 | 114 µs | 182 µs | 200 µs | 29.6 ms | 167 ms | 442 ms | 486 ms | 0 / 0 | 0 |
| c1-batch0 | s256 | r1 | 12,004 | 114 µs | 178 µs | 196 µs | 852 ms | 2.28 s | 2.88 s | 3.06 s | 0 / 0 | 0 |
| c1-batch0 | s256 | r2 | 12,006 | 132 µs | 184 µs | 205 µs | 307 ms | 960 ms | 1.22 s | 1.37 s | 0 / 0 | 0 |
| c1-batch0 | s256 | r3 | 12,006 | 123 µs | 185 µs | 206 µs | 430 ms | 982 ms | 1.27 s | 1.42 s | 0 / 0 | 0 |
| c1-batch0 | s1024 | r1 | 12,006 | 119 µs | 192 µs | 211 µs | 3.41 s | 4.37 s | 4.99 s | 5.17 s | 0 / 0 | 0 |
| c1-batch0 | s1024 | r2 | 12,006 | 126 µs | 215 µs | 286 µs | 3.24 s | 3.94 s | 4.89 s | 5.07 s | 0 / 0 | 0 |
| c1-batch0 | s1024 | r3 | 12,006 | 106 µs | 187 µs | 207 µs | 2.46 s | 4.42 s | 5.22 s | 5.31 s | 0 / 0 | 0 |
| c1-spread | c1-iso-spread (BQ-S2) | r1 | 12,006 | 146 µs | 185 µs | 192 µs | 228 µs | 747 µs | 3.4 ms | 5.2 ms | 0 / 0 | 0 |
| c1-spread | c1-iso-spread (BQ-S1) | r1 | 12,006 | 132 µs | 183 µs | 188 µs | 210 µs | 226 µs | 1.1 ms | 3.6 ms | 0 / 0 | 0 |
| c1-spread | c1-iso-spread (BQ-S1) | r2 | 12,006 | 170 µs | 181 µs | 184 µs | 201 µs | 216 µs | 976 µs | 3.3 ms | 0 / 0 | 0 |
| c1-spread | c1-iso-spread (BQ-S1) | r3 | 12,006 | 140 µs | 183 µs | 188 µs | 216 µs | 444 µs | 2.2 ms | 5.4 ms | 0 / 0 | 0 |
| c1-spread | base | r1 | 11,992 | 114 µs | 181 µs | 205 µs | 122 ms | 312 ms | 619 ms | 753 ms | 0 / 0 | 0 |
| c1-spread | base | r2 | 11,999 | 119 µs | 184 µs | 199 µs | 55.9 ms | 247 ms | 376 ms | 399 ms | 0 / 0 | 0 |
| c1-spread | base | r3 | 11,986 | 105 µs | 184 µs | 200 µs | 85.5 ms | 325 ms | 707 ms | 771 ms | 0 / 0 | 0 |
| c1-spread | opt3 | r1 | 12,006 | 131 µs | 199 µs | 217 µs | 53.9 ms | 616 ms | 846 ms | 892 ms | 0 / 0 | 0 |
| c1-spread | opt3 | r2 | 12,006 | 134 µs | 194 µs | 209 µs | 439 µs | 22.4 ms | 67.2 ms | 101 ms | 0 / 0 | 0 |
| c1-spread | opt3 | r3 | 12,006 | 130 µs | 189 µs | 209 µs | 584 µs | 113 ms | 843 ms | 996 ms | 0 / 0 | 0 |
| c1-spread | s16 | r1 | 11,970 | 113 µs | 193 µs | 221 µs | 54.9 ms | 352 ms | 496 ms | 531 ms | 0 / 0 | 0 |
| c1-spread | s16 | r2 | 12,006 | 122 µs | 187 µs | 205 µs | 35.9 ms | 160 ms | 502 ms | 549 ms | 0 / 0 | 0 |
| c1-spread | s16 | r3 | 12,005 | 117 µs | 182 µs | 203 µs | 33.6 ms | 201 ms | 494 ms | 587 ms | 0 / 0 | 0 |
| c1-spread | s256 | r1 | 12,006 | 124 µs | 177 µs | 192 µs | 101 ms | 1.38 s | 2.14 s | 2.32 s | 0 / 0 | 0 |
| c1-spread | s256 | r2 | 11,958 | 117 µs | 197 µs | 217 µs | 3.2 ms | 608 ms | 1.20 s | 1.35 s | 0 / 0 | 0 |
| c1-spread | s256 | r3 | 12,006 | 119 µs | 183 µs | 197 µs | 472 ms | 1.06 s | 1.28 s | 1.31 s | 0 / 0 | 0 |
| c1-spread | s1024 | r1 | 12,006 | 127 µs | 183 µs | 205 µs | 336 ms | 3.72 s | 4.97 s | 5.15 s | 0 / 0 | 0 |
| c1-spread | s1024 | r2 | 12,006 | 114 µs | 194 µs | 215 µs | 1.60 s | 2.62 s | 3.64 s | 3.82 s | 0 / 0 | 0 |
| c1-spread | s1024 | r3 | 11,994 | 113 µs | 184 µs | 204 µs | 496 ms | 4.07 s | 4.88 s | 5.02 s | 0 / 0 | 0 |
| c2 | c2-iso (BQ-S2) | r1 | 24,007 | 83.2 µs | 157 µs | 164 µs | 1.2 ms | 141 ms | 467 ms | 616 ms | 0 / 0 | 0 |
| c2 | c2-iso (BQ-S1) | r1 | 24,007 | 90.8 µs | 152 µs | 158 µs | 184 µs | 487 µs | 2.9 ms | 13.8 ms | 0 / 0 | 0 |
| c2 | c2-iso (BQ-S1) | r2 | 24,007 | 120 µs | 154 µs | 160 µs | 181 µs | 213 µs | 1.8 ms | 8.2 ms | 0 / 0 | 0 |
| c2 | c2-iso (BQ-S1) | r3 | 24,007 | 119 µs | 154 µs | 159 µs | 187 µs | 471 µs | 3.1 ms | 10.4 ms | 0 / 0 | 0 |
| c2 | base | r1 | 23,693 | 57.9 µs | 172 µs | 53.0 ms | 2.86 s | 4.03 s | 4.29 s | 4.32 s | 0 / 0 | 0 |
| c2 | base | r2 | 23,888 | 98.5 µs | 163 µs | 353 µs | 467 ms | 773 ms | 1.00 s | 1.03 s | 0 / 0 | 0 |
| c2 | base | r3 | 23,966 | 90.8 µs | 170 µs | 357 µs | 391 ms | 752 ms | 973 ms | 1.00 s | 0 / 0 | 0 |
| c2 | opt3 | r1 | 23,991 | 107 µs | 180 µs | 54.5 ms | 1.10 s | 2.53 s | 3.25 s | 3.34 s | 0 / 0 | 0 |
| c2 | opt3 | r2 | 24,007 | 106 µs | 165 µs | 179 µs | 708 ms | 2.23 s | 2.66 s | 2.71 s | 0 / 0 | 0 |
| c2 | opt3 | r3 | 24,007 | 91.1 µs | 160 µs | 172 µs | 405 ms | 975 ms | 1.18 s | 1.24 s | 0 / 0 | 0 |
| c2 | s16 | r1 | 24,006 | 82.0 µs | 172 µs | 4.3 ms | 476 ms | 1.03 s | 1.28 s | 1.31 s | 0 / 0 | 0 |
| c2 | s16 | r2 | 23,983 | 98.7 µs | 174 µs | 1.4 ms | 299 ms | 952 ms | 1.22 s | 1.25 s | 0 / 0 | 0 |
| c2 | s16 | r3 | 24,007 | 71.8 µs | 172 µs | 6.2 ms | 302 ms | 646 ms | 888 ms | 918 ms | 0 / 0 | 0 |
| c2 | s256 | r1 | 24,007 | 94.2 µs | 186 µs | 559 ms | 3.29 s | 4.26 s | 4.53 s | 4.56 s | 0 / 0 | 0 |
| c2 | s256 | r2 | 23,994 | 93.0 µs | 164 µs | 331 µs | 1.43 s | 2.02 s | 2.26 s | 2.38 s | 0 / 0 | 0 |
| c2 | s256 | r3 | 24,007 | 88.0 µs | 168 µs | 213 µs | 1.35 s | 1.88 s | 2.82 s | 2.94 s | 0 / 0 | 0 |
| c2 | s1024 | r1 | 23,698 | 85.4 µs | 152 µs | 165 µs | 188 ms | 435 ms | 1.14 s | 1.34 s | 0 / 0 | 0 |
| c2 | s1024 | r2 | 24,007 | 95.6 µs | 176 µs | 516 µs | 4.47 s | 5.44 s | 5.70 s | 5.73 s | 0 / 0 | 0 |
| c2 | s1024 | r3 | 24,007 | 75.6 µs | 166 µs | 216 µs | 4.93 s | 5.52 s | 5.70 s | 5.74 s | 0 / 0 | 0 |

**Appendix A2. Host state at every relaxed cell: load average and competing jobs (the driver's `host_before` / `host_after`), JSON written time (UTC), balancer moves**

The load average is `/proc/loadavg` as the driver read it before the server started and after the window (1 / 5 / 15 minute). "Competing" is the driver's `pgrep` list (cc1plus, cmake --build, ctest, kds_tests and the like).

| cell | arm | run | JSON written (UTC) | load before (1 / 5 / 15 min) | load after | competing jobs before | competing jobs after | balancer moves in window | rolls in window | bulk core(s) (cores of the window's rolls) | non-bulk p99 | non-bulk p99.9 | mount checkpoint of the fresh file (device thermometer) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| c1-iso-segregated-no0 | base | r1 | 17:13:02 | 2.25 / 8.79 / 7.80 | 1.47 / 7.77 / 7.50 | none | none | 0 | 0 | - | n/a | n/a | 7.3 ms |
| c1-segregated-no0 | base | r1 | 18:15:32 | 2.32 / 2.62 / 3.19 | 2.91 / 2.74 / 3.21 | none | none | 18 | 35 | [1] | 523 ms | 698 ms | 7.5 ms |
| c1-segregated-no0 | base | r2 | 18:42:37 | 2.12 / 4.32 / 3.97 | 2.83 / 4.21 / 3.96 | none | none | 6 | 36 | [1] | 410 ms | 555 ms | 7.8 ms |
| c1-segregated-no0 | base | r3 | 18:59:23 | 2.24 / 2.39 / 2.88 | 2.48 / 2.41 / 2.87 | none | none | 8 | 36 | [1] | 446 ms | 714 ms | 10.2 ms |
| c1-segregated-no0 | opt3 | r1 | 17:26:51 | 2.43 / 3.15 / 4.74 | 3.72 / 3.40 / 4.76 | none | none | 35 | 25 | [1] | 532 ms | 904 ms | 607 ms |
| c1-segregated-no0 | opt3 | r2 | 18:46:25 | 2.10 / 3.22 / 3.61 | 2.78 / 3.26 / 3.61 | none | none | 40 | 50 | [0, 2] | 18.2 ms | 127 ms | 15.2 ms |
| c1-segregated-no0 | opt3 | r3 | 19:03:12 | 1.88 / 2.24 / 2.72 | 2.66 / 2.37 / 2.75 | none | none | 33 | 53 | [1] | 39.3 ms | 164 ms | 15.9 ms |
| c1-segregated-no0 | s16 | r1 | 17:32:58 | 2.38 / 2.75 / 4.07 | 3.26 / 2.89 / 4.05 | none | none | 22 | 125 | [3] | 320 ms | 600 ms | 16.0 ms |
| c1-segregated-no0 | s16 | r2 | 18:19:30 | 2.04 / 2.42 / 2.99 | 3.11 / 2.64 / 3.04 | none | none | 25 | 142 | [3] | 160 ms | 416 ms | 24.5 ms |
| c1-segregated-no0 | s16 | r3 | 19:07:10 | 2.06 / 2.26 / 2.64 | 2.52 / 2.36 / 2.65 | none | none | 30 | 142 | [1] | 118 ms | 456 ms | 11.2 ms |
| c1-segregated-no0 | s256 | r1 | 18:02:12 | 2.36 / 2.88 / 3.87 | 3.35 / 3.06 / 3.88 | none | none | 16 | 5 | [3] | 4.91 s | 5.71 s | 805 ms |
| c1-segregated-no0 | s256 | r2 | 18:23:37 | 2.32 / 2.45 / 2.88 | 2.85 / 2.56 / 2.90 | none | none | 12 | 8 | [3] | 1.03 s | 1.35 s | 7.5 ms |
| c1-segregated-no0 | s256 | r3 | 18:50:38 | 2.35 / 2.74 / 3.32 | 2.71 / 2.77 / 3.30 | none | none | 29 | 8 | [1] | 966 ms | 1.20 s | 9.7 ms |
| c1-segregated-no0 | s1024 | r1 | 18:10:38 | 2.30 / 2.81 / 3.45 | 3.44 / 3.00 / 3.48 | none | none | 4 | 2 | [2] | 2.0 ms | 162 ms | 40.2 ms |
| c1-segregated-no0 | s1024 | r2 | 18:32:01 | 2.33 / 2.62 / 2.85 | 2.92 / 2.73 / 2.87 | another session: cmake --build + sim sweep (pids 744955) | another session: cmake --build + sim sweep (pids 744955) | 22 | 2 | [2] | 3.58 s | 4.39 s | 9.0 ms |
| c1-segregated-no0 | s1024 | r3 | 18:55:04 | 2.25 / 2.48 / 3.07 | 2.47 / 2.48 / 3.04 | none | none | 17 | 2 | [2] | 4.01 s | 4.81 s | 12.3 ms |
| c1-iso-batch0 | base | r1 | 17:13:40 | 1.47 / 7.77 / 7.50 | 0.89 / 6.82 / 7.19 | none | none | 0 | 0 | - | n/a | n/a | 8.3 ms |
| c1-batch0 | base | r1 | 18:16:23 | 2.47 / 2.65 / 3.17 | 2.84 / 2.70 / 3.17 | none | none | 20 | 35 | [0, 1] | 234 ms | 432 ms | 8.3 ms |
| c1-batch0 | base | r2 | 18:43:27 | 2.39 / 4.08 / 3.91 | 2.54 / 3.89 / 3.86 | none | none | 14 | 35 | [3] | 519 ms | 698 ms | 18.0 ms |
| c1-batch0 | base | r3 | 19:00:04 | 2.48 / 2.41 / 2.87 | 3.21 / 2.59 / 2.91 | none | none | 8 | 34 | [3] | 256 ms | 515 ms | 10.7 ms |
| c1-batch0 | opt3 | r1 | 17:29:51 | 2.43 / 3.04 / 4.44 | 2.86 / 3.08 / 4.39 | none | none | 6 | 49 | [3] | 477 ms | 1.25 s | 257 ms |
| c1-batch0 | opt3 | r2 | 18:47:14 | 2.35 / 3.15 / 3.57 | 2.89 / 3.16 / 3.55 | none | none | 42 | 53 | [1] | 36.4 ms | 187 ms | 10.7 ms |
| c1-batch0 | opt3 | r3 | 19:04:03 | 2.41 / 2.33 / 2.73 | 2.56 / 2.38 / 2.73 | none | none | 28 | 52 | [1] | 15.0 ms | 86.1 ms | 9.2 ms |
| c1-batch0 | s16 | r1 | 17:34:07 | 2.15 / 2.66 / 3.94 | 2.51 / 2.66 / 3.88 | none | another session: cmake --build kds_tests + gtest (pids 726150, 726151, 726152) | 23 | 129 | [1] | 359 ms | 733 ms | 7.5 ms |
| c1-batch0 | s16 | r2 | 18:20:20 | 2.49 / 2.53 / 3.00 | 2.60 / 2.55 / 2.99 | none | none | 24 | 142 | [3] | 291 ms | 555 ms | 7.6 ms |
| c1-batch0 | s16 | r3 | 19:08:00 | 2.21 / 2.30 / 2.63 | 2.41 / 2.33 / 2.62 | none | none | 17 | 143 | [2] | 240 ms | 457 ms | 14.5 ms |
| c1-batch0 | s256 | r1 | 18:03:26 | 2.47 / 2.87 / 3.79 | 3.38 / 3.03 / 3.80 | none | none | 2 | 5 | [1] | 62.4 ms | 173 ms | 8.8 ms |
| c1-batch0 | s256 | r2 | 18:24:29 | 2.41 / 2.47 / 2.87 | 3.06 / 2.61 / 2.89 | none | none | 14 | 9 | [2] | 1.04 s | 1.25 s | 7.7 ms |
| c1-batch0 | s256 | r3 | 18:51:31 | 2.29 / 2.67 / 3.27 | 2.72 / 2.72 / 3.26 | none | none | 10 | 8 | [1] | 932 ms | 1.30 s | 7.5 ms |
| c1-batch0 | s1024 | r1 | 18:11:57 | 2.42 / 2.79 / 3.39 | 2.69 / 2.80 / 3.36 | none | none | 19 | 2 | [3] | 4.27 s | 4.53 s | 8.2 ms |
| c1-batch0 | s1024 | r2 | 18:33:08 | 2.37 / 2.62 / 2.83 | 3.31 / 2.83 / 2.89 | another session: cmake --build + sim sweep (pids 744955) | another session: cmake --build + sim sweep (pids 744955) | 24 | 2 | [2] | 4.01 s | 4.95 s | 8.2 ms |
| c1-batch0 | s1024 | r3 | 18:55:56 | 2.43 / 2.47 / 3.03 | 2.80 / 2.54 / 3.03 | none | none | 19 | 2 | [1] | 4.71 s | 5.25 s | 11.3 ms |
| c1-iso-spread | base | r1 | 17:14:18 | 0.89 / 6.82 / 7.19 | 0.55 / 6.08 / 6.93 | none | none | 0 | 0 | - | n/a | n/a | 8.1 ms |
| c1-spread | base | r1 | 18:17:13 | 2.47 / 2.63 / 3.14 | 3.09 / 2.73 / 3.15 | none | none | 11 | 35 | [1] | 342 ms | 649 ms | 9.7 ms |
| c1-spread | base | r2 | 18:44:18 | 2.15 / 3.77 / 3.82 | 2.71 / 3.70 / 3.79 | none | none | 36 | 35 | [0, 1, 2] | 145 ms | 349 ms | 7.9 ms |
| c1-spread | base | r3 | 19:01:03 | 2.29 / 2.43 / 2.85 | 2.54 / 2.46 / 2.84 | none | none | 7 | 36 | [2] | 307 ms | 681 ms | 7.7 ms |
| c1-spread | opt3 | r1 | 17:30:51 | 2.27 / 2.93 / 4.31 | 2.68 / 2.93 / 4.25 | none | none | 62 | 52 | [2, 3] | 534 ms | 823 ms | 9.5 ms |
| c1-spread | opt3 | r2 | 18:48:03 | 2.45 / 3.05 / 3.51 | 3.00 / 3.10 / 3.51 | none | none | 44 | 59 | [3] | 23.9 ms | 69.7 ms | 11.8 ms |
| c1-spread | opt3 | r3 | 19:04:53 | 2.16 / 2.30 / 2.70 | 2.99 / 2.49 / 2.75 | none | none | 32 | 53 | [3] | 212 ms | 858 ms | 9.1 ms |
| c1-spread | s16 | r1 | 17:35:18 | 2.07 / 2.53 / 3.80 | 2.87 / 2.67 / 3.79 | none | none | 34 | 125 | [0, 3] | 354 ms | 482 ms | 7.8 ms |
| c1-spread | s16 | r2 | 18:21:01 | 2.47 / 2.53 / 2.98 | 2.95 / 2.64 / 3.00 | none | none | 58 | 142 | [3] | 141 ms | 494 ms | 10.6 ms |
| c1-spread | s16 | r3 | 19:08:40 | 2.30 / 2.30 / 2.62 | 2.90 / 2.45 / 2.65 | none | none | 18 | 143 | [2] | 200 ms | 498 ms | 10.9 ms |
| c1-spread | s256 | r1 | 18:04:48 | 2.30 / 2.81 / 3.69 | 3.70 / 3.05 / 3.74 | none | none | 6 | 6 | [0, 2] | 1.72 s | 2.23 s | 8.0 ms |
| c1-spread | s256 | r2 | 18:28:31 | 1.73 / 2.65 / 2.89 | 2.81 / 2.80 / 2.93 | another session: cmake --build + sim sweep (pids 744955) | another session: cmake --build + sim sweep (pids 744955) | 10 | 8 | [2] | 707 ms | 1.23 s | 8.3 ms |
| c1-spread | s256 | r3 | 18:52:23 | 2.27 / 2.62 / 3.22 | 2.71 / 2.68 / 3.21 | none | none | 14 | 8 | [2] | 1.07 s | 1.28 s | 8.5 ms |
| c1-spread | s1024 | r1 | 18:12:59 | 2.37 / 2.72 / 3.33 | 2.41 / 2.67 / 3.28 | none | none | 5 | 2 | [3] | 3.87 s | 5.00 s | 15.1 ms |
| c1-spread | s1024 | r2 | 18:34:24 | 2.47 / 2.67 / 2.83 | 3.66 / 2.93 / 2.91 | another session: cmake --build + sim sweep (pids 744955) | another session: cmake --build + ctest -j8 (br-backup-restore) (pids 746465, 746466) | 8 | 2 | [2] | 2.65 s | 3.67 s | 24.1 ms |
| c1-spread | s1024 | r3 | 18:56:59 | 2.45 / 2.48 / 3.00 | 2.59 / 2.49 / 2.97 | none | none | 6 | 2 | [1] | 105 ms | 223 ms | 14.5 ms |
| c2-iso | base | r1 | 17:14:57 | 0.55 / 6.08 / 6.93 | 1.45 / 5.59 / 6.72 | none | none | 12 | 0 | - | n/a | n/a | 17.9 ms |
| c2 | base | r1 | 18:14:33 | 2.48 / 2.66 / 3.24 | 3.24 / 2.81 / 3.26 | none | none | 20 | 30 | [3] | 4.03 s | 4.29 s | 11.2 ms |
| c2 | base | r2 | 18:41:48 | 2.44 / 4.74 / 4.09 | 2.50 / 4.46 / 4.02 | none | none | 17 | 34 | [3] | 773 ms | 1.00 s | 7.2 ms |
| c2 | base | r3 | 18:58:43 | 2.23 / 2.40 / 2.91 | 2.24 / 2.39 / 2.88 | none | none | 22 | 35 | [3] | 752 ms | 973 ms | 13.7 ms |
| c2 | opt3 | r1 | 17:24:16 | 2.42 / 3.27 / 5.05 | 3.96 / 3.54 / 5.06 | none | none | 47 | 17 | [3] | 2.53 s | 3.25 s | 324 ms |
| c2 | opt3 | r2 | 18:45:43 | 2.39 / 3.45 / 3.70 | 2.20 / 3.26 / 3.62 | none | none | 96 | 58 | [3] | 2.23 s | 2.66 s | 12.5 ms |
| c2 | opt3 | r3 | 19:02:31 | 1.94 / 2.29 / 2.76 | 1.96 / 2.26 / 2.73 | none | none | 109 | 57 | [3] | 975 ms | 1.18 s | 23.0 ms |
| c2 | s16 | r1 | 17:32:18 | 2.42 / 2.82 / 4.15 | 2.38 / 2.75 / 4.07 | none | none | 122 | 131 | [3] | 1.03 s | 1.28 s | 15.2 ms |
| c2 | s16 | r2 | 18:18:50 | 2.16 / 2.49 / 3.03 | 2.05 / 2.43 / 2.99 | none | none | 132 | 137 | [3] | 952 ms | 1.22 s | 19.3 ms |
| c2 | s16 | r3 | 19:06:30 | 2.15 / 2.32 / 2.67 | 2.06 / 2.26 / 2.64 | none | none | 118 | 137 | [3] | 646 ms | 888 ms | 10.9 ms |
| c2 | s256 | r1 | 17:54:21 | 2.31 / 3.61 / 4.64 | 3.00 / 3.59 / 4.59 | none | none | 28 | 5 | [3] | 4.26 s | 4.53 s | 8.8 ms |
| c2 | s256 | r2 | 18:22:44 | 2.22 / 2.45 / 2.91 | 2.71 / 2.53 / 2.91 | none | none | 18 | 9 | [3] | 2.02 s | 2.26 s | 12.0 ms |
| c2 | s256 | r3 | 18:49:45 | 2.18 / 2.80 / 3.38 | 2.65 / 2.82 / 3.36 | none | none | 33 | 8 | [3] | 1.88 s | 2.82 s | 14.6 ms |
| c2 | s1024 | r1 | 18:08:52 | 2.11 / 2.80 / 3.52 | 3.73 / 3.10 / 3.58 | none | none | 30 | 2 | [3] | 435 ms | 1.14 s | 260 ms |
| c2 | s1024 | r2 | 18:30:48 | 2.30 / 2.65 / 2.87 | 2.85 / 2.73 / 2.89 | another session: cmake --build + sim sweep (pids 744955) | another session: cmake --build + sim sweep (pids 744955) | 19 | 2 | [3] | 5.44 s | 5.70 s | 13.3 ms |
| c2 | s1024 | r3 | 18:54:11 | 2.41 / 2.57 / 3.14 | 2.27 / 2.49 / 3.07 | none | none | 24 | 2 | [3] | 5.52 s | 5.70 s | 7.7 ms |
| c3 | base | r1 | 18:13:52 | 2.41 / 2.66 / 3.26 | 2.48 / 2.66 / 3.24 | none | none | 0 | 36 | [3] | n/a | n/a | 7.7 ms |
| c3 | base | r2 | 18:41:07 | 2.28 / 5.06 / 4.16 | 2.44 / 4.74 / 4.09 | none | none | 0 | 33 | [3] | n/a | n/a | 5.6 ms |
| c3 | base | r3 | 18:57:51 | 2.15 / 2.39 / 2.94 | 2.64 / 2.49 / 2.94 | none | none | 0 | 36 | [3] | n/a | n/a | 18.6 ms |
| c3 | opt3 | r1 | 17:22:13 | 2.29 / 3.47 / 5.34 | 3.47 / 3.60 / 5.31 | none | none | 0 | 29 | [3] | n/a | n/a | 10.3 ms |
| c3 | opt3 | r2 | 18:45:04 | 2.30 / 3.58 / 3.75 | 2.39 / 3.45 / 3.70 | none | none | 0 | 64 | [3] | n/a | n/a | 11.8 ms |
| c3 | opt3 | r3 | 19:01:51 | 2.15 / 2.37 / 2.81 | 2.02 / 2.32 / 2.77 | none | none | 0 | 57 | [3] | n/a | n/a | 9.9 ms |
| c3 | s16 | r1 | 17:31:39 | 2.49 / 2.89 / 4.22 | 2.42 / 2.82 / 4.15 | none | none | 0 | 142 | [3] | n/a | n/a | 7.1 ms |
| c3 | s16 | r2 | 18:18:10 | 2.21 / 2.55 / 3.08 | 2.17 / 2.49 / 3.04 | none | none | 0 | 142 | [3] | n/a | n/a | 7.9 ms |
| c3 | s16 | r3 | 19:05:50 | 2.29 / 2.36 / 2.70 | 2.15 / 2.32 / 2.67 | none | none | 0 | 142 | [3] | n/a | n/a | 7.1 ms |
| c3 | s256 | r1 | 18:01:06 | 2.35 / 2.95 / 3.96 | 2.90 / 3.01 / 3.93 | none | none | 0 | 7 | [3] | n/a | n/a | 639 ms |
| c3 | s256 | r2 | 18:22:01 | 2.24 / 2.50 / 2.94 | 2.22 / 2.45 / 2.91 | none | none | 0 | 9 | [3] | n/a | n/a | 15.4 ms |
| c3 | s256 | r3 | 18:49:02 | 2.15 / 2.90 / 3.43 | 2.18 / 2.80 / 3.38 | none | none | 0 | 9 | [3] | n/a | n/a | 11.1 ms |
| c3 | s1024 | r1 | 18:07:03 | 2.46 / 2.84 / 3.60 | 3.41 / 3.05 / 3.63 | none | none | 0 | 2 | [3] | n/a | n/a | 352 ms |
| c3 | s1024 | r2 | 18:29:35 | 2.44 / 2.72 / 2.90 | 2.77 / 2.76 / 2.91 | another session: cmake --build + sim sweep (pids 744955) | another session: cmake --build + sim sweep (pids 744955) | 0 | 2 | [3] | n/a | n/a | 8.9 ms |
| c3 | s1024 | r3 | 18:53:17 | 2.29 / 2.60 / 3.18 | 2.36 / 2.56 / 3.14 | none | none | 0 | 2 | [3] | n/a | n/a | 18.3 ms |

## Appendix B. The isolated cells at `group`

**The four isolated cells run at `group`, beside their `relaxed` twins (BQ-S2 base, 1 run each; no mixed `group` cell was run for any arm)**

| cell | durability | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max | load before | JSON written (UTC) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c1-iso-segregated-no0 | relaxed | 12,006 | 122 µs | 174 µs | 181 µs | 230 µs | 853 µs | 3.0 ms | 4.6 ms | 2.25 / 8.79 / 7.80 | 17:13:02 |
| c1-iso-segregated-no0 | group | 12,006 | 126 µs | 183 µs | 192 µs | 674 µs | 3.0 ms | 106 ms | 136 ms | 1.45 / 5.59 / 6.72 | 17:15:45 |
| c1-iso-batch0 | relaxed | 12,006 | 127 µs | 172 µs | 178 µs | 202 µs | 602 µs | 2.4 ms | 4.4 ms | 1.47 / 7.77 / 7.50 | 17:13:40 |
| c1-iso-batch0 | group | 12,006 | 125 µs | 181 µs | 193 µs | 689 µs | 3.1 ms | 80.3 ms | 102 ms | 2.36 / 5.23 / 6.54 | 17:16:34 |
| c1-iso-spread | relaxed | 12,006 | 146 µs | 185 µs | 192 µs | 228 µs | 747 µs | 3.4 ms | 5.2 ms | 0.89 / 6.82 / 7.19 | 17:14:18 |
| c1-iso-spread | group | 12,006 | 131 µs | 188 µs | 198 µs | 277 µs | 1.6 ms | 51.1 ms | 101 ms | 2.45 / 4.33 / 6.03 | 17:18:48 |
| c2-iso | relaxed | 24,007 | 83.2 µs | 157 µs | 164 µs | 1.2 ms | 141 ms | 467 ms | 616 ms | 0.55 / 6.08 / 6.93 | 17:14:57 |
| c2-iso | group | 14,557 | 6.32 s | 13.4 s | 16.6 s | 21.8 s | 22.5 s | 22.9 s | 23.0 s | 2.48 / 4.09 / 5.87 | 17:19:36 |
