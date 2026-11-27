# EE-S0 premise: a decide wait with no slot behind it ends at the core's 1 ms drain tick, 0.6-0.7 ms after a woken one; no wait on one hot row ends at the fault net; a standing fence costs about +1.8 us of server CPU a point write at eight cores

EE-S0 (`instructions/v3.2.0/workorder-ee-decide-slot.md` §5) prices the two
behaviours EE-Q1 (a)'s bar names, and runs the eight-core cell EE-Q5 (a)
asks for.

- **A wait for a decide that polls ends at the waiter core's next poll, and
  every core polls at least every 1 ms**: each core arms the WAL drain
  timer at `wal_drain_interval_us` (1,000 by default;
  `src/server/core_runtime.cpp:426-428`, `src/server/expeditor.cpp:2461`), and a timer
  caps the idle block (`Scheduler::IdleTimeoutMs`, `src/sched/scheduler.cpp:182-221`).
  Cross-core, an assertion reservation wait and a `PURGE`'s wait on a
  deleter answer **+629 us and +572 us later at p50** than a row wait woken
  at its slot (§3). With the drain tick off (`wal_drain_interval_us = 0`)
  the ceiling is the idle block's 10 ms, and 15 of 30 assertion waits and
  11 of 30 `PURGE` waits ran past 2 ms (p95 9.8-10.5 ms).
- **No wait ended at the fault net** in 3,637,789 waits on one row over
  eight sessions, with and without 2 ms held inside the transaction (§4).
  A wake is not a grant: at 2 ms held, a commit costs about 7 parks, and the
  slowest transaction took 344 ms - a third of the net.
- **EE-Q5's eight-core cell**: a standing range fence costs the uncovered
  point writes it walks for **+1.78 us of server CPU a statement**, paired
  over 22 blocks (14 positive; t-interval +0.63 to +2.93 us), against an
  unfenced 12.1 us: about +15 %, beside BP-H10's +12 %. The client's p50
  does not move outside its block spread (§5).

## 1. Stamp

| | |
|---|---|
| Executed | 2026-11-27, 19:56:40 to 19:57:26 UTC (the final run, §3-§5); a first pass at 19:50-19:55 UTC from the same binary (§5, kept as corroboration) |
| Branch / commit | `worktree-ee-decide-slot` at `046b09053`, `git describe --tags` = `v3.4.17-220-g046b09053`; no EE code - EE-S0 builds nothing |
| Build | `build-release`, `CMAKE_BUILD_TYPE=Release`, target `kds_server`, built in this worktree at this commit |
| Binary | a copy at `/home/ukjin/ee-s0-run/final/kds_server`, `sha256` `cfa053555a112a8ab2b86f8ac64ce07b6f3b508a70c49c865330f1a047aedbf0` (`archive/.../binary.sha256`). The first pass ran the build tree's file, whose hash is the same |
| Device | data files and WAL under `/home/ukjin/ee-s0-run`: `/dev/mapper/fedora-root`, xfs (`df -T`); not tmpfs |
| Server config | `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, every other key default; `cores = 2` (wake), `4` (hot), `8` (fence); the drain-off wake cell adds `wal_drain_interval_us = 0` |
| Ports | 15961-15966, chosen |
| Pinning | server reactors `reactor_cpus` 2,3 (wake), 0-3 (hot), 0-7 (fence, and the process); clients on CPUs 8-15 (`taskset`) |
| Host | Intel Core Ultra 7 270K Plus, 24 CPUs (P/E hybrid: memory's rule, server on 0-7). Load per cell (`archive/.../host.txt`): 0.59-1.44 one-minute average, no `cc1plus`, `cmake --build` or `ctest` running at any cell |
| Drivers | `tools/ee_s0_premise.py --cell wake --rounds 60` (and `--rounds 30 --drain-us 0`), `--cell hot --sessions 8 --seconds 15 --think-ms 0` and `2`; `tools/cp_s0_fence.py --writers 8 --ops 2000 --blocks 8 --cpu-schedstat` (`--cpu-schedstat` added by EE-S0: on-CPU ns from `/proc/<pid>/task/*/schedstat`, where CP-S0 read 100 Hz ticks) |
| Raw | `bench/v3.4.17/archive/ee-s0-premise-v3.4.17-220-g046b09053/` (every cell's JSON, the fence driver's text, the run and watchdog scripts) |

## 2. The census: every wait for a decide, and what ends it

Read at `046b09053` in `src/server/command_dispatcher.cpp`. A **slot** is a
`txn::LockWaitSlot` a release flips and whose core it kicks; a **poll** is
a `sched::WaitUntil` predicate re-asked when the reactor next turns.

| Wait | Site | Ends on | Reached, with a lock table |
|---|---|---|---|
| A row's unit refused (tuple, range, index key) | `AwaitWriteBlock` (`:503-552`), the ask's wake from `BorrowChain` | slot | every contended write |
| A decided holder not yet released (AY-S3) | the same, `woken_at_release` (`:15867`) | slot | the decide-to-release window |
| A granted probe whose holder committed above the ceiling | the same, `on_ceiling` | the ceiling's lift, kicked | foreign key and unique probes |
| A relation's lock (`IS`/`IX`/`X`) | `AwaitRelationLock` (`:930-1000`) | slot | DDL against open writers |
| An online index build's drain | `IndexBuildRun` (`:3610-3650`) | slot | `CREATE INDEX` online |
| **An assertion group's reserver** | `AdmitInsert` (`:9105`), `AdmitAndReserveUpdate` (`:14681`) → `NoteBlockingWriter` with no wake | **poll** of `IsInFlight` | an insert or update into a group at its bound while another transaction holds a reservation in it |
| **A `PURGE`'s undecided deleter** | `PurgeWait` (`:1194-1215`), `wait.horizon` false | **poll** of `IsInFlight`, bounded by `kPurgeHorizonWaitNs` (1 s) | an administrator's `PURGE` over a key whose delete is undecided |
| A `PURGE`'s read horizon | the same, `wait.horizon` true | poll of `ResolvedForEveryReader` | an older open snapshot; not a decide (EE-Q4 (a)) |
| A header-only holder | `CheckWriteConflictBlocking` (`:16360-16408`), `blocker = cur` | poll | **no writer known**: every `UPDATE`/`DELETE` row is claimed under its tuple unit before the check (`Claim`, `:13597-13615`), every `INSERT` takes its tuple `X`, and the KWP load inserts through `ExecuteInsert`; only a dispatcher with no lock table reaches it |
| A granted probe whose holder is still in flight | `WaitForRowWriter` (`:6530-6556`) | poll | no writer known (`known-gaps.md`, Locks) |
| A foreign key's busy parent, no table | `:6262-6264` | poll | a dispatcher with no lock table |

**Two of the polling waits are reachable by a statement an operator writes**:
the assertion group's (the manual's own `CHECK COUNT(*) <= 180` at its 180th
seat) and `PURGE`'s deleter. **`sys.lock_waits` and `relation_stats`'
`lock_waits` count neither**: a row is opened only on a slot
(`:516-521`), so CP-S0's "every parked ask granted" said nothing of them.

## 3. Cell `wake`: what the poll costs a wait

Two sessions on different cores (180 of 180 asks cross-core). The holder
writes, the waiter's autocommit statement meets it from a thread, the holder
decides 20 ms later. Lag = waiter's reply time less the moment the decide
was sent: the decide's own round trip, the wake and the re-run. 60 rounds,
the arms interleaved and reversed round by round.

**Default drain tick (1 ms)**, 60 asks an arm:

| Arm | ends on | p0 | p25 | p50 | p75 | p95 | p100 | mean | answer |
|---|---|---|---|---|---|---|---|---|---|
| `slot` (row `UPDATE`, holder commits) | slot | 273 us | 578 us | 676 us | 734 us | 809 us | 1,232 us | 653 us | `UPDATED` 60 |
| `assertion` (group at its bound, holder rolls back) | poll | 499 us | 1,175 us | 1,305 us | 1,454 us | 1,641 us | 1,818 us | 1,279 us | `INSERTED` 60 |
| `purge` (key deleted, holder commits) | poll | 185 us | 1,039 us | 1,248 us | 1,386 us | 1,616 us | 1,742 us | 1,160 us | `PURGED` 60 |

| Δ against `slot` | p25 | p50 | p75 | p95 | mean |
|---|---|---|---|---|---|
| `assertion` | +597 us | +629 us | +720 us | +832 us | +626 us |
| `purge` | +461 us | +572 us | +652 us | +807 us | +507 us |

The deltas sit inside one tick: a wait settled at the decide is noticed at
the waiter core's next drain tick, uniform over its 1 ms, plus the poll's
own turn. The first pass (19:50 UTC, same binary) read +673 us and +707 us
at p50.

**Drain tick off** (`wal_drain_interval_us = 0`; the server warns at boot),
30 asks an arm:

| Arm | p0 | p25 | p50 | p75 | p95 | p100 | mean |
|---|---|---|---|---|---|---|---|
| `slot` | 453 us | 620 us | 663 us | 763 us | 958 us | 1,688 us | 709 us |
| `assertion` | 530 us | 707 us | 1,029 us | 6,817 us | 10,451 us | 10,622 us | 3,602 us |
| `purge` | 362 us | 677 us | 738 us | 4,648 us | 9,770 us | 9,946 us | 2,719 us |

With no timer the block runs to its 10 ms ceiling, and the polling arms are
bimodal: 15 of 30 assertion waits and 11 of 30 `PURGE` waits ran past
2 ms, up to the ceiling, and the rest answered within the slot arm's range. **What ends the fast ones is not verified** (another
event on the waiter's core; `perf` is locked out on this host).

## 4. Cell `hot`: one row, eight sessions, four cores, 15 s

`BEGIN; UPDATE ee_hot SET v = v + 1 WHERE id = 1; [think]; COMMIT` in a
loop. Every wait here is the row's tuple unit, on a slot. Counts are
`sys.relation_stats`' cumulative columns for the relation; the percentiles
of `waited_us` are `sys.lock_waits`' last 16,384 rows (its ring: 4,096 a
core).

| think | commits | refused | lock waits | waited (sum) | deadlocks | **fault nets** | `wait_aborts` |
|---|---|---|---|---|---|---|---|
| 0 ms | 599,178 | 1,001 `TXN_CONFLICT` (decided-newer, `txn.md` §5) | 3,596,623 | 59.7 s | 0 | **0** | 0 |
| 2 ms | 5,885 | 0 | 41,166 | 100.7 s | 0 | **0** | 0 |

| think | percentile of | p0 | p25 | p50 | p75 | p95 | p100 | mean |
|---|---|---|---|---|---|---|---|---|
| 0 ms | one wait (`waited_us`) | 5 us | 14 us | 16 us | 18 us | 22 us | 1,257 us | 17 us |
| 0 ms | one transaction (client) | 50 us | 137 us | 176 us | 243 us | 372 us | 7,417 us | 200 us |
| 2 ms | one wait | 497 us | 2,273 us | 2,480 us | 2,654 us | 2,841 us | 7,335 us | 2,450 us |
| 2 ms | one transaction | 2,187 us | 10,497 us | 15,014 us | 23,400 us | 52,675 us | 343,953 us | 20,395 us |

At 2 ms held, 41,166 waits for 5,885 commits is 7.0 parks a commit: each
release wakes every parked waiter, one takes the unit and the rest park
again (`TryAcquire` does not queue). The unfairness is in the tail - a
transaction's p95 is 3.5x its p50 and its worst 23x - but the worst, 344
ms, is a third of the 1 s net.

**Against this engine's own number**: CP-S0's M4 (`bench/v3.4.7/results-cp-s0-premise-v3.4.7-1-gcc54a296.md`
§5, eight sessions on one row, autocommit) parked 17 asks and refused 7.9 %
of statements by the decided-newer verdict; here an explicit transaction
on one row parks 6.0 asks a commit at 0 ms held and refuses 0.17 %; why the explicit
transaction meets the decided-newer window less often is not verified.
Neither run ends a wait at the net.

## 5. EE-Q5's cell: a standing fence at eight cores

CP-S0's M5 at eight cores (`tools/cp_s0_fence.py`, its docstring the
method): one session holds `id > 4080` open, a range `X`; eight writers
update their own rows `<= 4080`, so every ask walks the 64 partitions in
the fenced arm and none in the unfenced. 8 blocks, the arms interleaved and
reversed; 128,000 statements an arm; `sys.lock_waits` empty (nothing
conflicted).

| Arm | ops | p0 | p25 | p50 | p95 | p99 | max | mean | server CPU a statement |
|---|---|---|---|---|---|---|---|---|---|
| fenced | 128,000 | 19.3 us | 67.8 us | 95.9 us | 219.4 us | 315.7 us | 2,102.1 us | 109.2 us | 14.13 us |
| unfenced | 128,000 | 18.5 us | 60.6 us | 87.9 us | 208.1 us | 304.5 us | 271,663.0 us | 107.9 us | 12.46 us |

The client p50's delta (+8.0 us) is inside its block spread (35.4 us: two
unfenced blocks ran at a 63 us p50 where the rest ran at 94-98 us); the
max is one 272 ms stall in an unfenced block.

**Server CPU, paired block by block** (`schedstat` ns, every thread):

| Run | blocks | mean of (fenced − unfenced) | fenced higher | partition-latch waits, fenced / unfenced | unfenced CPU |
|---|---|---|---|---|---|
| final, 19:57 UTC | 8 | +1.66 us | 5 of 8 | 5,073 / 2,078 | 12.46 us |
| first pass, schedstat | 8 | +1.75 us | 5 of 8 | 4,942 / 2,122 | 11.83 us |
| first pass, 100 Hz ticks | 6 | +1.98 us | 4 of 6 | 4,245 / 1,828 | 11.98 us |
| **all three** | **22** | **+1.78 us** (sd 2.59, t-interval +0.63 to +2.93) | **14 of 22** | | |

The first pass broke bench/README rules 3 and 4 - it ran the build tree's
binary, the same bytes by hash, and recorded the host's load once, not per
cell - and is shown beside the final run because the three agree, not as a
result of its own.

**Against this engine's own number**: at four cores CP-S0 read the walk at
about 1 us a statement and could not resolve its CPU (100 Hz ticks, 10-50 us
spreads). At eight it is +1.78 us of 12.1 us, **about +15 % of the
statement's server CPU, with 2.4x the partition-latch waits**: BP-H10's
+12 % reproduces within the interval (+5 % to +24 %). The client does not
see it at this concurrency; a server short of CPU would.

## 6. What the run teaches about the engine

- **"What still polls" is a 1 ms matter, not a 10 ms one.** The drain tick
  every core arms makes the poll's interval the WAL drain's, so a wait
  that nothing kicks costs at most one tick after its decide - unless an
  operator turns the drain off, when it costs up to the idle ceiling. The
  order's §0 presumed the ceiling.
- **The polled waits an operator can reach mostly delay a refusal.** An
  assertion admission refused by a reservation is admitted only if the
  reserver rolls back or gives the room back; if it commits, the re-run is
  refused as a violation. The poll adds up to a tick to that answer. A
  `PURGE` is an administrator's statement bounded by its own 1 s wait.
- **The hot row's cost is the wake's herd, not a lost wake.** Every flip
  wakes every waiter and all but one park again: 6-7 parks a commit. That
  is CPU spent, never the net, at eight sessions; a queueing grant (EE-Q2,
  outside EE) is what would remove it.
- **The fence's price is real at eight cores** and is CPU, about 1.8 us a
  point write while any range fence stands on the relation.

## 7. Limits and what was cut

- **The wake cell's lag carries the client's own timing** (a Python thread
  waking on its reply, and the decide's round trip): the slot arm's 676 us
  is mostly that, so the arms' difference is the finding, not their level.
- **One hold time** (20 ms) and **one placement** (two cores, the sessions
  apart); a same-core waiter is woken by its own core's turn and was not
  run.
- **The drain-off cell's fast mode is unexplained** (§3).
- **No header-only holder was driven**: the census found no writer that
  reaches it with a lock table, by reading; no cell confirms the absence.
- **The fence cell is one concurrency** (eight writers at eight cores); the
  client-latency effect under CPU saturation was not run.
- Time: about 12 minutes of building and running, inside the stage's 20.

## 8. Reproduce

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release -j16 --target kds_server
# the cells, as archive/.../run.sh runs them, from a copy of the binary:
python3 tools/ee_s0_premise.py --binary <copy> --workdir <dir> --port 15961 --cell wake --rounds 60 --cpus 2,3
python3 tools/ee_s0_premise.py ... --cell wake --rounds 30 --cpus 2,3 --drain-us 0
python3 tools/ee_s0_premise.py ... --cell hot --cores 4 --cpus 0,1,2,3 --sessions 8 --seconds 15 --think-ms 0   # and 2
# fence: a cores = 8 server pinned to 0-7, then
python3 tools/cp_s0_fence.py --port <p> --writers 8 --ops 2000 --blocks 8 --server-pid <pid> --cpu-schedstat
```
