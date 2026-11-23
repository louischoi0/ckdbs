# CW-S0 - what a read of a resident page pays for its latches, counted and timed (`v3.5.0-91-g6ddcae43`)

Work order: `instructions/v3.2.0/workorder-cw-optimistic-page-read.md`
CW-S0 (M1-M5, the census), judged against V0 (CW-Q1 (a), adopted on the
operator's standing go-ahead of 2026-11-22). Driver: `tools/ba_census.py`
with three additions made for this stage (a `pointN` shape, the server's
CPU over the measured span, `--server-env` and `--sample`). Raw JSON, the
driver scripts, the spike's diff, the census, the sampler reports and the
calibration program: `bench/v3.5.0/archive/cw-s0-v3.5.0-91-g6ddcae43/`. Every table in §2-§4 is
`python3 tables_s0.py` over that archive's `runs/`.

**Finding.** **V0 fails.** At `cores = 2`, a clean
cell, a primary-key point read makes 4 page fetches. Its whole fetch
protocol is 626 ns a statement, or 2.8 % of the statement's 22.2 us of
server CPU. That protocol is 2 structure-latch holds and 2 page-latch
compare-and-swaps a fetch, the frame lookup and the pin count. What arming
adds over the unarmed path is 471 ns (2.1 %); that is not the words alone,
since the unarmed hit takes three lookups where the armed hit takes one
(census C1). At the order's
largest clean cell (`cores = 3`) the protocol is 2.6 %. In the unclean
`cores = 3` cell, where the clients sit on the reactors' SMT siblings, it
is 3.3 %. V0's bar is 5 %. With no bracket floor subtracted at all the
three read 3.8 %, 3.4 % and 3.9 %. No cell saturated the server: each
reactor was 23-66 % busy in the `point` cells (§5).

Sharing the anchor and the root (3 of a point read's 4 fetches; the leaf is
one of 61) costs about 30 ns a fetch: 626 ns on one relation against 500 ns
with each session on its own relation. In throughput one relation reads
1.3 % under a relation per session at `cores = 2` and 0.4 % at the clean
`cores = 3`, inside the runs' own spread (§4). V0's other bar, M4, is also
5 %.

A sampled profile agrees: the fetch functions' own samples and every mutex
in the process together are 3.6 % of a point read's server CPU at
`cores = 2`.

**CW closes at V0 with nothing built** (CW-Q0 (a)). The cost the order
asked about is real, and it grows with arming and with a second core
(155 ns unarmed, 397 ns armed on one core, 626 ns at two cores). A third
clean core adds little (675 ns); the unclean three's 1,128 ns comes with
its clients on the reactors' SMT siblings and about twice the clean three's
reactor load, and its statement's whole server CPU is 29 % above the clean
three's.
At every multi-core cell this host can measure, it is half to two thirds of
the bar.

**Where the latches do cost something is the write path, which no
candidate takes.** An `insert1` statement makes three exclusive fetches:
page 7 and the rightmost leaf, which every core's sessions write, and the
core's own undo tail. Its fetch protocol is 1,392 ns (6.0 %) at
`cores = 2` and 1,862 ns (7.0 %) at the clean `cores = 3`, with page-latch
spins on the hot pages. CW leaves every writer latched (§6).

## 1. Stamp

| item | value |
|---|---|
| executed | 2026-11-22, 14:43:31 to 15:03:15 UTC: 54 timed runs, the frame-pointer build (84 s) between them. Builds before the first run: `build-release` 77 s, the spike 97 s. **19 min 44 s from the first run to the last, inside CW-Q9 (a)'s 20 minutes**; no hold by a quiet gate beyond the driver's own load wait (`--max-load 2.5`) |
| branch | `worktree-cw-optimistic-page-read` (worktree `sync-stale-records`), at `6ddcae43` (`v3.5.0-91-g6ddcae43`), the commit CW opened at; its build inputs are `56b44964`'s |
| arm R | `6ddcae43` as it stands, `build-release`, rebuilt at the commit before the first run; copy sha256 `6fa434243fed382b63d8245560bda2db1dd1b7a38ca8ae78109bd5369c247a48`, 26 runs |
| arm S | the spike, `worktree-cw-s0-spike` at `cba85f1f` over `6ddcae43`, **never landed**, built in its own tree (`git archive` of `6ddcae43` plus the six files); sha256 `4fd4f6d86ac2fd9b9e8e687c2e4f72c2899255ffaae59453ac0936c066aa49d7`, 25 runs. With `KDS_CW_COUNT` and `KDS_CW_TICKS` unset it differs from R by a predictable branch at each instrumented site (about a dozen a fetch and its release, armed) and an environment read at open: M5's unarmed `point` reads 19.65 us a statement against R's 19.86 us at `cores = 1` |
| arm Rfp | `6ddcae43` built with `-fno-omit-frame-pointer` for the sampler; sha256 `77d88e010f8e743d0e597ad24659ad6a650df3b6ef8d6d56bc2756201089c18d`, 3 runs |
| device | every data file on `/dev/nvme0n1p1`, ext4 (each JSON's `data_device`), never tmpfs |
| host | 8 vCPUs, Intel Xeon Platinum 8581C @ 2.10 GHz, no PMU, `perf_event_paranoid = 3`, no `perf` or `gdb`. **SMT siblings per `/sys/.../thread_siblings_list` on 2026-11-22: (0,4) (1,5) (2,6) (3,7)** |
| server | `durability = relaxed`, `buffer_pool_frames = 65536`, port 17411 (chosen), 10,000 preloaded rows per relation, a fresh server and data file per run |
| driver | 8 s measured after 2 s warm-up, one run per invocation, every client a process of its own |
| load | `/proc/loadavg` 1.88-2.50 before every run: the previous run's own clients and server decaying. No competing build or suite in any run (`host_before.competing` empty in all 54) |
| census | read while the runs went, read-only (`grep`, `sed`): Part A by this session, Parts B and C by a subagent; its file is the archive's `census.md` |
| clean | 0 of 54 runs client-bound by the tool's mark, 0 errors, 0 refusals |

**The cells.** The CPU maps are drawn from the topology above, not from the
order's text (§5):

| cell | reactors | clients | sessions | clean? |
|---|---|---|---|---|
| c1 | CPU 0 | 2, 3, 6, 7 | 2 | yes |
| c2 | 0, 1 | 2, 3, 6, 7 | 4 | yes: no reactor shares a physical core with a client or another reactor |
| c3c | 0, 1, 2 | 3, 7 | 6 | yes, and **client-limited**: six clients share two CPUs, so throughput falls under c2's. The tool's per-client mark (80 % of a CPU) does not see this |
| c3i | 0, 1, 2 | 3, 7, 4, 5, 6 | 6 | **no, indicative only**: clients run on the reactors' siblings 4, 5 and 6 |

## 2. M1 - shared writes a statement, counted (arm S, `KDS_CW_COUNT=1`)

Counted at the sites themselves:
- every fetch through `FetchPinned`, by kind;
- every frame-table (structure-latch) acquisition, by `Latch::lock` and `try_lock`;
- every compare-and-swap attempt on a page-latch word, in `PageLatch::TryAcquire` and `Release`;
- every grant, by mode.

| shape | cell | read fetches | write fetches | frame-table holds | page-latch CAS attempts | shared grants | exclusive grants | load before |
|---|---|---|---|---|---|---|---|---|
| `point` | c1 armed | 4.000 | 0.000 | 8.00 | 8.000 | 4.000 | 0.000 | 2.39 |
| `point` | c2 | 4.000 | 0.000 | 8.00 | 8.001 | 4.000 | 0.000 | 1.95 |
| `pkmix` | c2 | 2.999 | 1.503 | 12.51 | 9.006 | 2.999 | 1.503 | 2.37 |
| `insert1` | c2 | 2.000 | 3.012 | 18.10 | 10.045 | 2.005 | 3.018 | 2.16 |

All counts are per statement.

- **A read hit is 2 holds and 2 compare-and-swaps, exactly** (DO's tree). A
  point read makes 4 fetches, all shared:
  - the anchor and the root, for the bind's ownership check (`StillOwned`, `step_vm.cpp:326`);
  - the root again, for the descent;
  - the leaf (census A.3).
- **An `insert1` makes 5 fetches, 2 of them shared, and 18.1 holds.** The
  census read 4 fetches and 1 shared off the code, so this count corrects it:
  - the 3 exclusive fetches take 3 holds each;
  - the 2 shared fetches take 2 each;
  - the rest, 5.1 holds a statement, the count does not attribute: the
    census infers 3 `StampPageLsn` holds (census A.3, `[inferred]`), which
    leaves about 2 unread.
- Failed compare-and-swaps are 0.005 an `insert1` and 0 a point read.

## 3. M3 - where the cycles go

### 3.1 The fetch protocol, timed (arm S, `KDS_CW_TICKS=1`)

Two `rdtsc` reads bracket every `FetchPinned` (the fetch, its lookup, its
pin and its page latch) and every `PageRef::Release` (the latch release and
the unpin). The ticks are summed per core and read as a delta across the
span. The TSC runs at 2.100 GHz (`tsc_cal.cpp`, against `steady_clock`).
An empty bracket is 55 ticks (26 ns) in a tight loop, and that floor is
subtracted from every call. The unarmed release then reads 2-4 ns, so the
floor is over-subtracted in place, by an amount the runs do not give. With
no floor subtracted at all, the `point` shares are 3.8 % at c2, 3.4 % at
c3c and 3.9 % at c3i (`insert1`'s 7.2 % and 7.9 % at c2 and c3c), and no
verdict moves.

| shape | cell | read fetch, each | write fetch, each | release, each | fetch protocol a statement | its share of server CPU | server CPU a statement (this run) | load before |
|---|---|---|---|---|---|---|---|---|
| `point` | c1 unarmed | 37 ns | - | 2 ns | 155 ns | 0.8 % | 20.18 us | 2.40 |
| `point` | c1 armed | 69 ns | - | 30 ns | 397 ns | 1.9 % | 20.54 us | 2.23 |
| `point` | c2 | 118 ns | - | 38 ns | **626 ns** | **2.8 %** | 22.21 us | 2.04 |
| `point` | c3c | 130 ns | - | 39 ns | **675 ns** | **2.6 %** | 26.35 us | 2.46 |
| `point` | c3i | 198 ns | - | 84 ns | 1,128 ns | 3.3 % | 33.97 us | 2.35 |
| `pointN` | c2 | 88 ns | - | 37 ns | 500 ns | 2.3 % | 22.07 us | 2.35 |
| `pointN` | c3c | 107 ns | - | 41 ns | 593 ns | 2.3 % | 26.15 us | 2.46 |
| `pointN` | c3i | 147 ns | - | 80 ns | 910 ns | 2.8 % | 32.55 us | 2.45 |
| `insert1` | c1 unarmed | 38 ns | 38 ns | 4 ns | 207 ns | 1.1 % | 18.70 us | 2.47 |
| `insert1` | c1 armed | 66 ns | 87 ns | 34 ns | 551 ns | 2.9 % | 18.95 us | 2.31 |
| `insert1` | c2 | 159 ns | 256 ns | 60 ns | 1,392 ns | 6.0 % | 23.05 us | 2.18 |
| `insert1` | c3c | 179 ns | 387 ns | 71 ns | 1,862 ns | 7.0 % | 26.71 us | 2.45 |
| `insert1` | c3i | 345 ns | 828 ns | 150 ns | 3,940 ns | 11.5 % | 34.13 us | 2.48 |

How to read it:

- **The unarmed fetch is 155 ns a point read, and it is not a subset of the
  armed one**: it takes three hash lookups and an `IsAllocated` test where
  the armed hit takes one lookup (census C1).
- **What arming adds is the protocol less the unarmed path**: 242 ns
  (1.2 %) on one core, 471 ns (2.1 %) at `cores = 2`, 520 ns (2.0 %) at the
  clean `cores = 3` and 973 ns (2.9 %) unclean. That is the words less two
  lookups, not the words alone.
- **The most any candidate could return is the whole protocol**, 2.8 % at
  c2 and 2.6 % at c3c, less what C3's memo and copy would cost. C2, which
  keeps the pin and its structure-latch holds, could return at most the
  compare-and-swaps' part of it.
- **A shared anchor and root cost about 31 ns a fetch at `cores = 2`**
  (118 ns against 88 ns a read fetch), or 126 ns a statement.

### 3.2 A sampled profile (arm Rfp, `tools/kds_sampler.c`, `cores = 2`)

ITIMER_PROF at a requested 1 kHz over the measured span. The kernel's tick
bounds the rate, so a run gives 1,704-2,034 samples. Each dump was
symbolized against `bin/Rfp` itself. The first symbolization used the
driver's shared copy path `work/kds_server.census`, which a later run had
overwritten with another arm, and it named functions not on the path; it
was discarded (§5).

| shape | samples | fetch and release functions, self | mutex under a fetch | every other mutex | sum: the upper bound on the latch family |
|---|---|---|---|---|---|
| `point` | 1,704 | 1.23 % | 0.41 % | 1.94 % | 3.6 % |
| `pointN` | 1,739 | 1.44 % | 0.40 % | 1.15 % | 3.0 % |
| `insert1` | 2,034 | 2.90 % | 1.13 % | 5.75 % | 9.8 % |

- **"Every other mutex" includes the unpin's structure-latch hold.** glibc's
  `pthread_mutex_*` keeps no frame pointer, so the chain above it skips its
  immediate caller. It also includes every other latch in the process: lock
  partitions, the window latch and the stream latch.
- **The largest self entry is libc's syscall wrappers, at 34-38 %.** They
  symbolize as `sem_trywait`, the nearest exported name, and are the network
  path's `recv`, `send` and `epoll`. A point read's server CPU is mostly the
  wire and the parse, not the page.

## 4. M2, M4 and M5 - throughput and server CPU (arm R; arm S for M5)

**M2 and M4.** Two runs a cell, interleaved by run; each cell shows the mean
with the two runs in parentheses. Server CPU is the sum over every server
thread of `/proc/<pid>/task/*/schedstat` run time across the measured span,
divided by the statements.

| shape | cell | stmt/s | server CPU a statement | fetches a statement | frame-table waits a statement | frame-table wait a statement | page-latch waits a statement | page-latch spin turns a statement | p50 | p99 | load before |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `point` | c1 | 32,008 (30,957 / 33,059) stmt/s | 19.86 us (20.18 / 19.55) | 4.00 | 0.000 | 0 ns | 0.000 | 0.00 | 57.8 us | 85.8 us | 2.31-2.34 |
| `point` | c2 | 44,673 (44,267 / 45,080) stmt/s | 21.94 us (22.11 / 21.76) | 4.00 | 0.033 | 19 ns | 0.000 | 0.00 | 82.8 us | 128.3 us | 2.26-2.29 |
| `point` | c3c | 30,449 (30,434 / 30,464) stmt/s | 25.81 us (25.83 / 25.79) | 4.00 | 0.028 | 16 ns | 0.000 | 0.00 | 190.3 us | 324.9 us | 2.33-2.45 |
| `point` | c3i | 53,991 (52,765 / 55,216) stmt/s | 32.47 us (33.04 / 31.91) | 4.00 | 0.165 | 103 ns | 0.000 | 0.00 | 94.5 us | 194.2 us | 2.28-2.48 |
| `pointN` (M4) | c2 | 45,258 (44,779 / 45,738) stmt/s | 21.90 us (21.81 / 22.00) | 4.00 | 0.044 | 23 ns | 0.000 | 0.00 | 82.3 us | 114.8 us | 2.36-2.39 |
| `pointN` (M4) | c3c | 30,559 (30,568 / 30,550) stmt/s | 25.91 us (26.27 / 25.55) | 4.00 | 0.035 | 20 ns | 0.000 | 0.00 | 189.7 us | 323.5 us | 2.37-2.45 |
| `pointN` (M4) | c3i | 54,034 (53,791 / 54,278) stmt/s | 32.04 us (32.37 / 31.71) | 4.00 | 0.133 | 86 ns | 0.000 | 0.00 | 94.9 us | 190.8 us | 2.34-2.39 |
| `pkmix` | c1 | 33,190 (32,985 / 33,395) stmt/s | 22.20 us (22.28 / 22.13) | 4.50 | 0.000 | 0 ns | 0.000 | 0.00 | 56.9 us | 85.0 us | 2.40-2.46 |
| `pkmix` | c2 | 47,648 (47,677 / 47,619) stmt/s | 26.15 us (26.61 / 25.70) | 4.50 | 0.184 | 89 ns | 0.001 | 0.07 | 79.7 us | 132.8 us | 2.25-2.28 |
| `pkmix` | c3i | 55,488 (54,079 / 56,898) stmt/s | 37.71 us (37.78 / 37.65) | 4.50 | 0.454 | 291 ns | 0.003 | 0.32 | 92.7 us | 208.4 us | 2.31-2.47 |
| `insert1` | c1 | 40,329 (40,316 / 40,341) stmt/s | 18.38 us (18.35 / 18.42) | 4.91 | 0.000 | 0 ns | 0.000 | 0.00 | 41.3 us | 66.4 us | 2.46-2.50 |
| `insert1` | c2 | 55,144 (54,884 / 55,405) stmt/s | 22.85 us (23.01 / 22.68) | 5.01 | 0.279 | 160 ns | 0.033 | 1.79 | 57.0 us | 115.9 us | 2.22-2.39 |
| `insert1` | c3i | 62,196 (62,161 / 62,231) stmt/s | 33.07 us (32.95 / 33.19) | 5.03 | 0.910 | 671 ns | 0.147 | 9.00 | 69.3 us | 217.4 us | 2.34-2.42 |

`pkmix` and `insert1` were not run at c3c. The pool missed 0.006 fetches a
statement on `insert1` (its growing relation's new pages) and none on the
other shapes.

**M4, V0's second bar.** Throughput and server CPU of `point` (one shared
relation) against `pointN` (a relation per session):

| cell | throughput, `point` against `pointN` | server CPU |
|---|---|---|
| c2 | -1.3 % | +0.2 % |
| c3c | -0.4 % | -0.4 % |
| c3i | -0.1 % | +1.3 % |

No cell is near 5 %. The two runs of one cell differ by up to 4.6 %
(c3i point), so none of these differences is resolved; the widest pairing
of a `point` run against a `pointN` run of one cell is -3.2 % (c2).

**M5: the latches armed on one core** (arm S, both switches off,
`KDS_TEST_PAGE_LATCH=1` against unset; two runs each, interleaved):

| shape | arm | stmt/s | server CPU a statement | load before |
|---|---|---|---|---|
| `point` | one core, unarmed | 32,609 (32,552 / 32,665) stmt/s | 19.65 us (19.53 / 19.76) | 2.35-2.39 |
| `point` | one core, armed | 32,355 (32,403 / 32,306) stmt/s | 20.11 us (20.15 / 20.07) | 2.20-2.33 |
| `insert1` | one core, unarmed | 40,562 (40,694 / 40,429) stmt/s | 18.24 us (18.26 / 18.22) | 2.02-2.33 |
| `insert1` | one core, armed | 40,330 (40,633 / 40,026) stmt/s | 18.51 us (18.53 / 18.50) | 1.88-2.20 |

Arming costs `point` +0.46 us a statement (+2.3 %) and `insert1` +0.27 us
(+1.5 %). That is the price, with no line moving, of what `cores > 1` turns
on.
- **It is more than the page words.** The flag also arms the structure
  latch, the free-map latch and the map-flush latch, and it changes the
  hit's path from three lookups to one (census C1).
- **The ticks put the fetch protocol's share of it at 242 ns** for `point`.
  The rest is the other latches and the process floor: two lifetimes of one
  binary differ by up to 0.27 us here
  (`bench/v3.4.13/results-cx-close-bisect-v3.4.13-1-g11f72ce9.md`).
- **A release server cannot arm one core without this spike.** The
  environment switch is under `#ifndef NDEBUG` at `6ddcae43`
  (`device_page_store.cpp:405-428`); only a harness that opens the store and
  calls `SetLatchArmed` can (census C1). The spike moves the switch out of
  that block.

## 5. What was cut, and what differs from the order

- **The order's clean cell is not clean on today's topology.** CW §5 names
  `cores = 3` on CPUs 0, 2 and 4 with the clients on 6 and 7. That reading
  rests on siblings 0-1, 2-3, 4-5 and 6-7, as
  `docs/pending/multicore-scaling-cores4-below-cores2.md` §3 states.
  `thread_siblings_list` reports (0,4) (1,5) (2,6) (3,7), so that map puts
  two reactors on one physical core. c3c is the order's cell redrawn:
  three physical cores for the reactors and one for the clients.
- **`ba_census.py`'s docstring is wrong on the same count.** Its "clean map"
  of `--server-cpus 0,2` with clients on 4-7 shares cores 0/4 and 2/6. It
  is corrected in this commit.
- **c3c is client-limited.** Six clients on two CPUs reach 30.4k stmt/s
  against c2's 44.7k. Its per-statement figures stand. Its contention is a
  floor on what three loaded reactors would show.
- **No cell saturated the server, c3c least of all.** Each JSON's
  `server_cpu_ns_by_thread` puts every reactor at 48-50 % busy in the
  `point` c2 runs, 23-29 % at c3c, 54-64 % at c3i and 62-66 % at c1, and
  39-75 % in the `insert1` cells: the closed-loop clients bound every cell,
  under the tool's 80 % mark. The shares are a statement's at that load; a
  saturated cell, where the frame-table waits would grow, was not run.
- **M3's profile was taken at `cores = 2` only**, for `point`, `pointN`
  and `insert1`. The c3 cells have the ticks and no samples.
- **Not run:**
  - `pkmix` and `insert1` at c3c, and `pkmix` under the ticks;
  - `cores = 4`: no clean map exists on this host;
  - more than two runs a cell, or one run where a cell is a census (M1, M3).
- **The sampler's first symbolization was discarded.** `ba_census.py`
  copies every arm's binary to one path (`<workdir>/kds_server.census`).
  `kds_sampler_report.py` symbolizes against the path the dump names, which
  the next run had replaced, so the dumps were re-pointed at `bin/Rfp`. The
  timed runs are unaffected: every JSON's `bin_sha256` matches the arm its
  label names, 54 of 54.

## 6. V0

CW-Q1 (a): go on only if either holds.

**(i) The read path's shared words are at least 5 % of server CPU a `point`
statement at the clean multi-core cell.**
- The whole fetch protocol is 2.8 % at c2 and 2.6 % at c3c; with no
  bracket floor subtracted, 3.8 % and 3.4 %.
- Net of the unarmed path, 2.1 % and 2.0 % (not the words alone, §3.1).
- The sampler's upper bound over every mutex is 3.6 % at c2 (61 samples).
- **Not met.** The unclean c3i, 3.3 % (3.9 % with no floor), does not meet
  it either. Every `point` cell's reactors were at most 66 % busy (§5).

**(ii) The same-relation cell is at least 5 % under the distinct-relations
cell (M4).**
- -1.3 % at c2, -0.4 % at c3c and -0.1 % at c3i, inside the runs' spread;
  the widest run pairing is -3.2 % (c2).
- **Not met.**

**Verdict: close with the numbers.** CW-Q0 (a) makes CW exploratory, so
closing at V0 with nothing built is a result. C0 is the candidate.
- **C2** removes only the page word's two compare-and-swaps of a fetch.
  That is a part of the 2.8 %, which the ticks do not separate.
- **C3** has at most the whole protocol, 2.6-2.8 %, less its memo and copy.
- **C1** returns a share of the same 2.8 % and of the write path's
  structure-latch waits: 0.16 us an `insert1` at c2, 0.7 %.

**What the verdict does not exclude: a saturated server** (CW-Q10, raised
by this stage's review; (a) is adopted on the operator's standing
go-ahead).
- Every cell above was read at the load ba_census's closed-loop clients
  give. The reactors were 23-66 % busy in the `point` cells (per-thread
  schedstat in each JSON) and 39-75 % in the `insert1` cells.
- At saturation the per-statement CPU falls (less idle wake-up a
  statement), and the protocol's line traffic and waits rise, so the share
  can only grow.
- The dearest per-fetch protocol measured (c3i: 1,128 ns a statement) over
  the cheapest statement measured (`cores = 1`, M5 unarmed: 19.65 us) is
  5.7 %, and 6.8 % with no bracket floor. That combination is pessimistic,
  since it puts an unclean cell's line traffic beside a one-core statement,
  but it crosses the bar.
- **A saturated clean `cores = 2` cell is not measured, because the
  stage's budget is spent.** It is the one measurement that could reopen CW
  at S0 (CW-Q10 (b)).

## 7. What the numbers say about the engine

- **A point read's server CPU is the wire and the parse, not the page.** Of
  22 us at `cores = 2`, the page fetches are 0.6 us and the latch words 0.5
  us. libc's syscall wrappers alone are a third of the samples.
- **The fetch protocol grows with arming and with a second core.** A
  fetch's protocol goes from 39 ns unarmed to 99 ns armed on one core,
  157 ns at two cores, 169 ns at the clean three and 282 ns at the unclean
  three, whose clients share the reactors' physical cores. Net of the
  unarmed path it is 60 ns a fetch on one armed core and 118 ns at two:
  about half of what arming adds exists with no other core. `[design]`
  How much of the other half is cache lines moving is not measured (no
  PMU); a design that removed the line moves and kept the atomics would
  return at most that half.
- **The write path is where the latch family weighs.** `insert1` spends
  6.0 % (`cores = 2`) to 11.5 % (`cores = 3`, unclean) of its server CPU in
  fetches. That is three exclusive fetches - two of pages every core's
  sessions write (the relation's `sys.tables` row on page 7, the rightmost
  leaf) and the core's own undo tail (one `UndoLog` a core,
  `core_runtime.hpp:523`) - each 256-828 ns against 38 ns unarmed, with 1.8-9.0 spin turns a
  statement on their page latches. The cost is write contention on hot
  pages, not the read path. No CW candidate touches it, and C1 only its
  structure-latch share. A write-side order would start from `insertN`
  against `insert1`.
- **Half a point read's fetches are the bind's ownership check.**
  `StillOwned` fetches the anchor and the root to read one 8-byte owner oid
  each, and the descent then fetches the root again (census A.4 #1).
  `[design]` Folding the check into the descent would remove two of four
  fetches, about 0.3 us a statement at `cores = 2` (1.4 %). It is cheaper
  than any CW candidate and is not built; no order covers it.
- **A shared anchor and root cost a point read about 31 ns a fetch at
  `cores = 2`**, under the run-to-run spread in throughput.

## 8. The census (CW-S0's first half)

The archive's `census.md` (413 lines) is the list CW-R3 asked for, should
the order ever be reopened. It was read at `6ddcae43`.

**Part A: 55 direct shared fetch sites.**
- 40 copy-out, 2 in-place, 13 held across another fetch, a callback or a lock.
- 20 of the 40 copy-out sites raise `Corruption` from bytes under the hold,
  which CW-R1 would have to defer.
- 38 caller rows say what runs under a ref a helper hands back.
- Every internal-node read in both trees is copy-out already.
- A point lookup's leaf stays shared across the row's emit, though the row
  is already copied into `version_` (`step_vm.cpp:2877`).
- A shared hold is taken under the same core's exclusive hold
  (`index_maintain.cpp:249`).

**Part B: every act that changes a frame's bytes or identity, and under what.**
- **B1: writeback stamps the checksum into the live frame under a shared
  page latch** (`device_page_store.cpp:2228`). It is a byte write under S,
  so a whole-page copy under S can capture a torn checksum field: the
  created-page copy in `LogBtreeSplit`, `log_page_image.hpp:93-96`.
  - It is value-benign today: no hit-path reader interprets the field, and
    writeback stamps again before every write.
  - It is a data race by the language's rule.
  - It is entered in `docs/inflight/known-gaps.md` (Eviction).
- B2b and B2c stamp `page_lsn` after a release, ordered by the structure
  latch.
- The C3-only identity sites are listed: fault-in, the create's zero fill,
  `InsertFrame`, `ReleaseFrameLocked` and slot reuse.

**Part C: the frame and a hit's costs.**
- A read hit is 2 holds and 2 compare-and-swaps, as M1 counts.
- It also takes a store-wide `hits_` `fetch_add` and the store-wide pin
  gauge, which the order did not list.
- `Frame` is 56 bytes, unaligned, inside a node that eviction frees. A C3
  version word would have to live in slot-owned memory.
