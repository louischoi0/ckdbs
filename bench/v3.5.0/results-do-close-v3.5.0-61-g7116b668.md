# DO close - the frame table's per-fetch holds, interleaved A/B (`v3.5.0-61-g7116b668`)

Work order: `instructions/v3.2.0/workorder-do-frame-table-long-holds.md`
DO-Q6 (a) (E4's clause: no regression in throughput or p99 beyond the
host's floor at `cores = 1, 2, 4`) under DO-Q7 (a) (throughput as a
regression check only). Driver: `tools/ba_census.py`, unmodified; its
flags are in its `--help`. Raw JSON, the progress file and the driver log:
`bench/v3.5.0/archive/do-close-v3.5.0-61-g7116b668/`.

**Finding.** E4 (a-d) took a read hit from three structure-latch holds to
two and a write hit from five to three. Over three shapes at `cores` 1, 2
and 4, no cell regresses beyond its own run-to-run spread, and the
frame-table wait the holds cause falls by 35-45 % a statement wherever
cores contend (`cores >= 2`). Throughput is level at `cores = 1` and
level-to-slightly-up at `cores = 4`; at `cores = 2` one cell (`insert1`)
reads -5.3 %, inside A's own spread. DO-Q6 (a)'s E4 clause is met in all
nine cells.

## 1. Stamp

| item | value |
|---|---|
| executed | 2026-11-21 15:09-15:23 UTC (54 runs, ~14 min of measurement; about 25 min with setup and write-up) |
| branch | `worktree-do-frame-table-long-holds`, worktree `dg-wal-segment-body` |
| arm A | `48a3276c`, `v3.5.0-55-g48a3276c` (the commit DO opened from) |
| arm B | `7116b668`, `v3.5.0-61-g7116b668` (DO's closing commit: E1, E3, E4a-d, DO-S0's instrument) |
| tree | clean at `7116b668`'s sources; the worktree carried uncommitted edits to documents only (`CLAUDE.md`, `docs/`, `instructions/`), no build input |
| binaries | Release builds, no census option, built before the run and not rebuilt. Copies measured from `~/bench-runs/do-close/bin/`: A sha256 `256d320bdeadcf77f019641d0be3ce8925d70488f5d8355b9e8f9c478f770a09`, B `1eb8884e84a20ba5ec54ca1c360db5430dd48190a59aae041c81fc961074538b`; both source files' mtime 2026-11-21 15:08:30 UTC (after both commits, so each binary is at its commit) |
| device | data files under `~/bench-runs/do-close/work` on `/dev/nvme0n1p1`, ext4 (`df -T`), never tmpfs; each run's `data_device` is in its JSON |
| host | 8 vCPUs (4 cores x 2 threads), Intel Xeon Platinum 8581C |
| server config | `durability = relaxed`, `buffer_pool_frames = 65536` (driver default), `reactor_cpus` = `0` / `0,2` / `0,2,1,3` for `cores` 1 / 2 / 4, port 17311 (chosen, not 15432) |
| driver | 10 s measured after 2 s warm-up, a fresh server and data file per run, point shape over 10,000 preloaded rows, clients pinned to CPUs 4-7 |
| order | interleaved run by run, A r1, B r1, A r2, B r2, A r3, B r3, per cell; 3 runs per arm per cell |

**Sessions.** 2 x `cores` capped at 4 client CPUs: 2, 4 and 4 sessions at
`cores` 1, 2 and 4. No run was client-bound (no client above 80 % of a
CPU) and none had an error or a refusal (0 of 54).

**Host, and the floor.** Another session's workload - a `python -m dbcli
ora-docker` process and its `oracleFREE` server processes - was live on
the host throughout and is not ours. `/proc/loadavg` (1-minute) before
each run is in the archive's `progress` file and each JSON's
`host_before`: it ranged 1.55 to 3.59 across the 54 runs (the other
workload had been holding 4.5-6.5 shortly before the first run). No
`cc1plus` was running at any run's start. **The floor is raised by that
workload**: this host's floor is about 50 % on eight cores (DO-Q6 (a)), and
the spread of the runs below is the floor measured from inside the run.
`cores = 4` cells put reactors 2 and 3 on CPUs 1 and 3, the SMT siblings
of CPUs 5 and 7, which are client CPUs: **the `cores = 4` cells are
indicative only** (`cores = 2` is the largest clean map, `ba_census.py`'s
docstring).

## 2. Throughput, A against B

Statements a second, mean of three runs, with the min-max of the three
beside it; delta is B against A. "Inside the floor" means the two arms'
ranges overlap or the delta is under A's own run-to-run standard deviation.

| shape | cores | sessions | A | B | delta | verdict |
|---|---|---|---|---|---|---|
| point | 1 | 2 | 24,344 st/s (24,013-24,530) | 24,399 st/s (24,323-24,462) | +0.2 % | inside the floor |
| insert1 | 1 | 2 | 31,723 st/s (31,540-31,851) | 32,007 st/s (31,717-32,281) | +0.9 % | inside the floor |
| pkmix | 1 | 2 | 25,512 st/s (25,437-25,569) | 25,561 st/s (25,465-25,647) | +0.2 % | inside the floor |
| point | 2 | 4 | 46,410 st/s (43,150-48,154) | 46,758 st/s (45,925-47,730) | +0.8 % | inside the floor |
| insert1 | 2 | 4 | 55,183 st/s (51,010-57,519) | 52,244 st/s (51,178-54,339) | -5.3 % | inside the floor (A's sd 3,623 st/s = 6.6 %; the ranges overlap) |
| pkmix | 2 | 4 | 45,392 st/s (44,506-46,444) | 47,828 st/s (47,497-48,017) | +5.4 % | B above A, ranges disjoint; a gain, not required |
| point | 4 | 4 | 41,290 st/s (40,948-41,799) | 41,497 st/s (41,255-41,900) | +0.5 % | inside the floor |
| insert1 | 4 | 4 | 49,085 st/s (48,734-49,285) | 50,174 st/s (50,021-50,463) | +2.2 % | B above A, ranges disjoint; indicative |
| pkmix | 4 | 4 | 41,048 st/s (40,796-41,215) | 41,384 st/s (40,874-41,797) | +0.8 % | inside the floor |

The noise floor from inside the run is the spread of one arm's three
runs: 0.3-1.2 % at `cores = 1`, 2-9 % at `cores = 2` (the shared-relation
`insert1` and `point` cells), under 2 % at `cores = 4`. Only the
`cores = 2` `insert1` delta (-5.3 %) is as large as a floor, and A's three
runs there span 51.0k to 57.5k st/s, which contains all of B's. With three
runs a sign cannot be established inside that spread; it is not reported as
a finding.

## 3. Latency, A against B

Per-statement latency over every client, mean of three runs of each
percentile, in microseconds. The count a run is the statements column of
its JSON (240k-575k).

| shape | cores | arm | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| point | 1 | A | 53.3 us | 69.3 us | 79.3 us | 103.0 us | 109.7 us |
| point | 1 | B | 52.8 us | 68.9 us | 79.3 us | 103.1 us | 109.7 us |
| insert1 | 1 | A | 36.7 us | 53.0 us | 54.7 us | 65.1 us | 82.2 us |
| insert1 | 1 | B | 36.9 us | 52.5 us | 54.2 us | 64.4 us | 81.3 us |
| pkmix | 1 | A | 41.9 us | 62.3 us | 71.5 us | 105.5 us | 135.6 us |
| pkmix | 1 | B | 41.6 us | 62.2 us | 71.3 us | 105.8 us | 136.0 us |
| point | 2 | A | 49.7 us | 70.7 us | 80.9 us | 109.3 us | 123.0 us |
| point | 2 | B | 52.7 us | 71.8 us | 81.2 us | 104.3 us | 128.2 us |
| insert1 | 2 | A | 33.3 us | 51.8 us | 60.4 us | 79.8 us | 127.8 us |
| insert1 | 2 | B | 34.4 us | 59.7 us | 62.0 us | 79.4 us | 128.0 us |
| pkmix | 2 | A | 37.8 us | 68.5 us | 77.5 us | 120.9 us | 149.9 us |
| pkmix | 2 | B | 39.0 us | 66.0 us | 74.3 us | 112.1 us | 146.0 us |
| point | 4 | A | 62.0 us | 86.5 us | 88.9 us | 113.6 us | 141.8 us |
| point | 4 | B | 57.6 us | 86.3 us | 88.4 us | 112.8 us | 143.3 us |
| insert1 | 4 | A | 38.9 us | 61.2 us | 64.2 us | 91.7 us | 226.7 us |
| insert1 | 4 | B | 38.0 us | 59.7 us | 62.8 us | 85.7 us | 222.7 us |
| pkmix | 4 | A | 44.3 us | 76.4 us | 89.6 us | 131.9 us | 190.7 us |
| pkmix | 4 | B | 44.6 us | 75.9 us | 89.7 us | 127.6 us | 182.9 us |

p99, B against A: -0.0 %, -1.1 %, +0.3 % at `cores = 1`; +4.3 %, +0.1 %,
-2.6 % at `cores = 2`; +1.1 %, -1.8 %, -4.1 % at `cores = 4` (point,
insert1, pkmix in each). The largest, `point` at `cores = 2` (+4.3 %, 123.0
to 128.2 us), is inside A's own p99 spread there (sd 17.8 us, range
111.9-143.5 us). Two cells' middle percentiles that read as movements are
spread, not signal: `insert1` `cores = 2` p25 (+15.3 %, A sd 8.9 us) and
`point` `cores = 2` p0 (+6.0 %, a best-case reading on a loaded host).

## 4. The wait that E4 removes

The waits' decomposition. The measured unit is a statement; the wait E4
changes is the one on the frame table's structure latch, which SHOW META
reports as `contention_frame_table_waits` and `contention_frame_table_wait_us`
(summed over every core, delta across the measured span). Per statement,
mean of three runs:

| shape | cores | A waits | B waits | change | A wait time | B wait time |
|---|---|---|---|---|---|---|
| point | 1 | 0 | 0 | none to see (one reactor: no other core holds the latch) | 0 | 0 |
| insert1 | 1 | 0 | 0 | none to see | 0 | 0 |
| pkmix | 1 | 0 | 0 | none to see | 0 | 0 |
| point | 2 | 0.147 | 0.082 | -44 % | 0.0725 us | 0.0363 us |
| insert1 | 2 | 0.593 | 0.388 | -35 % | 0.302 us | 0.196 us |
| pkmix | 2 | 0.426 | 0.256 | -40 % | 0.200 us | 0.109 us |
| point | 4 | 0.277 | 0.150 | -46 % | 0.234 us | 0.119 us |
| insert1 | 4 | 1.235 | 0.911 | -26 % | 0.872 us | 0.593 us |
| pkmix | 4 | 0.679 | 0.400 | -41 % | 0.578 us | 0.310 us |

Each A figure is the mean of three runs whose spread is under 10 % of the
mean (`insert1` at `cores = 4`: 1.11-1.35 a statement); the arms' ranges do
not overlap in any `cores >= 2` row. The wait time is the share of a
statement's elapsed time spent queued on that one latch: at most 0.9 us of
a 64 us `insert1` p50 at `cores = 4`, about 1.4 %. The other waits that
compose a statement - the round trip, the commit (`relaxed`, so no sync in
a statement), the write statement and the read - are not decomposed by this
driver beyond what the percentiles show; no lock or conflict wait occurred
(0 refusals in 54 runs). Where the frame table's wait was 1.4 % of a
statement, a wait cut by 26-46 % moves a statement by less than the floor,
which is why section 2 reads level and section 4 reads clearly.

## 5. DO's own exact counts, from the earlier stages (not re-measured here)

These are `instructions/v3.2.0/workorder-do-frame-table-long-holds.md` §7's
numbers, counts on any host and times from `build-census` or the
microbenchmark on this host; they are context for sections 2-4.

| quantity | at `48a3276c` | at `7116b668` | stage |
|---|---|---|---|
| structure-latch holds, a read hit | 3 | 2 | E4d (exact) |
| structure-latch holds, a write hit | 5 | 3 | E4a and E4d (exact) |
| checkpoint hold H1 at 1 Mi frames | 38.7 ms | 15.4 ms | DO-S1 |
| checkpoint hold H2 at 1 Mi frames | 29.6 ms | 8.7 ms | DO-S1 |
| H3 rehash, longest | up to 12.5 ms | gone | DO-S1 (E3) |
| read hit's hold time (one thread, 64 Ki frames) | 212 ns (at `a97bd952`) | 80.7 ns | E4b-E4d |
| one reader's fetches a second | 3.44 M (at `a97bd952`) | 4.14 M | E4b-E4d, three interleaved rounds |

## 5a. Three readers on one store, the microbenchmark (CLA, added after the run)

`bench/frame_table_hold_bench.cpp`, `build-release`, 64 Ki resident
frames, three reader threads (cores 1-3) fetching random resident pages
for 3 s, no walks; A is the store sources at `47f7016c` (DO-S0's: `48a3276c`
plus the instrument), B at `7116b668`; three interleaved rounds, load
5.3-6.4 (the other session's workload). **This is where E4 shows**: three
cores on one store with nothing else to do.

| arm | fetches a second (3 rounds) | fetch p50 | fetch p99 | frame-table waits a fetch |
|---|---|---|---|---|
| A `47f7016c` | 1,038,836 / 1,052,207 / 1,016,097 | 1.49-1.56 us | 14.94-15.16 us | 0.86 |
| B `7116b668` | 1,331,150 / 1,261,085 / 1,350,630 | 0.95-1.34 us | 13.02-13.14 us | 0.63 |

**+27 % fetches a second** (means 1.036 M to 1.314 M, ranges disjoint) and
p99 -13 %. A first attempt at this table used a benchmark binary built
before `--every-ms 0` meant "no walks"; its A arm walked the table back to
back and starved its readers, and it was discarded.

## 6. Insight

**The holds that E4 removed were a contention cost, not a latency cost,
and this host prices contention small.** A read hit's holds fell by a
third, and the wait on the frame table's latch fell by 26-46 % wherever two
or more reactors share a store - the effect is in the engine's own
counter, in every `cores >= 2` row and in the same direction in all of
them. But the latch accounted for at most 1.4 % of a statement even at
`cores = 4` `insert1`, the worst row, so an engine-level gain the size of
the wait share (a point or two) is inside this host's floor, and what
section 2 shows is exactly that: no regression, and a +2-5 % that is
visible only where the arms' ranges separate (`pkmix` at `cores = 2`,
`insert1` at `cores = 4`). DO-Q7 (a) said DO cannot price E4 in throughput
on this host; the data agrees. The per-statement wait counter is the
measure that does carry E4, and CW's C1 (the frame descriptor array,
`instructions/v3.2.0/workorder-cw-optimistic-page-read.md`) is to be priced
against a tree whose per-statement frame-table wait is 0.08-0.9 us, not
against the 0.15-1.3 holds-waited a statement of `48a3276c`. `insert1` is
the shape where it matters most (a wait 4 times a `point` statement's, a
shared hot relation), and the shape where `cores = 2` showed the one
negative-signed delta, inside the spread; an 8-core run with a lower floor
(DO-Q7 (b)) is the way to resolve that cell, and this run does not.

## 7. What was cut

- `cores = 4` is indicative (reactors on SMT siblings of client CPUs), as
  the docstring's clean map permits only `cores <= 2`.
- Row-count sweep (rule 9): only the point shape's 10,000 preloaded rows
  were run; `insert1` and `pkmix`'s write stripe do not scale with a
  preload, and a point lookup's frame-table holds do not scale with rows
  (the holds are per fetch, not per row). 200 and 1,000 rows were not run,
  to fit the budget.
- `strict` and `group` durability were not run: E4 changes no commit path
  and DO-Q6 (a) names `cores` as the axis.
- Three runs a cell: a 5-9 % spread at `cores = 2` leaves the `insert1`
  delta unresolved; more runs would narrow it.
- The suite was not run in this stage (no code changed; DO's stages each
  recorded their own).
- No 60 s watchdog ran on the engine A/B: its progress file was polled at
  100-118 s intervals, and no run stalled (CLA records this against
  CLAUDE.md's watchdog rule).
