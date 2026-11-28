# Work order EG - one client's durable write beyond its sync: an insert left unattributed, and a one-core commit the wait statistics do not see

Status: **WRITTEN 2026-11-28, not opened.** Written on worktree
`pg-kds-comparison-orders` (branch `worktree-pg-kds-comparison-orders`)
from `b95be2e86` (`v3.4.17-255-gb95be2e86`) on the operator's word of
2026-11-28 (`raft-marks-2026-11-28.md` §3, §4). No stage is started; EG-S0
waits for the operator's word and for the marks on §4's items. A
`[source-read]` claim with a `path:line` was read at `b95be2e86`. **Two
readers stand behind them**: the figures of §1.1 were computed by CLA from
the archived JSON, and the lines of §1.2 marked "spot-read" were re-read
by CLA; the rest of §1.2 and §1.3 was read by a research pass of this
session and not re-read.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/spec/wal.md` §6, §8, §11; `docs/spec/protocol.md` §9;
`docs/spec/wait-events.md`; `docs/spec/sched.md` §4;
`bench/v3.5.0/comparison-pg-kds-scenarios-v3.4.17-166-g6c98f7c32.md`;
`bench/v3.5.0/results-pg-control-scenarios-v3.4.17-202-g481d101aa.md`;
`docs/inflight/known-gaps.md` ("Every sync covers every live segment");
`instructions/v3.2.0/workorder-dy-segment-roll-off-the-latch.md` (DY,
open at S0, whose scope EG does not enter); EF and EH, written the same
day.

## 0. What EG is

`[measured]` EG was asked for as "the durable commit path, where
PostgreSQL is 16 % to 19 % faster". **That premise is false for the
commit**: with one booker at one core, `strict`, the commit is 135.5 µs
on KDS against 138.2 µs on PostgreSQL (§1.1), and the booking's 16 % sits
in its four reads, which are EF's. What the comparison leaves without a
cause is one cell: scenario 1's `result-insert`, an autocommit insert at
`group`, at 0.81x.

`[source-read]` Reading the path for that premise turned up a second
matter that is the engine's whatever a comparison says: **at one core a
commit syncs on its reactor, and for `strict` that time is booked to no
wait event** (§1.2).

`[design]` EG is therefore two small things and a gate: **attribute the
insert** on `main`, on equal terms (S0); **book the one-core sync** where
the wait statistics can see it (S1, on EG-Q2); and build a cut only if S0
leaves a cost that is the engine's and above the spread (S2, behind
EG-Q1). If S0 leaves nothing, EG is closed there with nothing built but
S1.

## 1. The tree as read

### 1.1 What was measured `[measured]`, by CLA from the archives

One booker, one core. KDS: the pinned A/B's new arm at `9f8f8b354`, five
runs
(`bench/v3.5.0/archive/scenario2-v3.4.17-159-g9f8f8b354/json/pin-ab-new*-b1-s2-c1-s.json`),
`strict`. PostgreSQL 18.7: the control run's three
(`bench/v3.5.0/archive/pg-control-v3.4.17-202-g481d101aa/json/*pg-s2-b1.json`),
`synchronous_commit = on`. Median of the runs' means:

| statement | KDS | PostgreSQL |
|---|---|---|
| `commit` | 135.5 µs | 138.2 µs |
| `cargo-lookup` | 35.4 µs | 25.1 µs |
| `credit-lookup` | 30.2 µs | 22.6 µs |
| `capacity-read` | 29.5 µs | 21.9 µs |
| `recipe-read` | 67.3 µs | 38.8 µs |
| `freight-insert` | 24.4 µs | 29.6 µs |
| `charge-insert` | 21.0 µs | 19.6 µs |
| `operation-update` | 23.3 µs | 23.2 µs |
| `org-update` | 21.8 µs | 20.9 µs |
| the six autocommit loads | 136.0 µs to 152.4 µs | 146.7 µs to 153.6 µs |

- **The sync is at parity and so are the writes.** An autocommit insert
  at `strict` is not slower than PostgreSQL's.
- **The reads carry the booking's gap**, and they are the statements the
  unequal clients bear on (EF §1.1). `recipe-read` is also an index on
  PostgreSQL and a walk of 93 rows on KDS, by the drivers' design.
- **`result-insert` is the open cell**: 4,859.0 qps (205.8 µs, spread
  1.7 %) on KDS at `6c98f7c32`, `cores = 1`, `group`, against 5,981 qps
  (167.2 µs, 4.7 %) on PostgreSQL. It is one `INSERT ... VALUES` of one
  row into `model_results` (`tools/scenario1_backtest.py:952`, `:977`),
  which returns no rows, so the client difference does not explain it.
  Its rung differs from every row of the table above, and nothing else
  about it is read here.
- **An older reading agrees with parity**: at `c390a624` one core at
  `group` matched PostgreSQL, 4,977 against 4,902 statements a second on
  BA-S4's `trade` shape at 16 sessions, not one client
  (`bench/v3.0.0/results-ba-s4-census-v2.7.0-725-gc390a624.md:152`,
  `:175`, `:215`), on another host.

### 1.2 How a commit becomes durable at one core `[source-read]`

- **`strict` syncs inline.** `WalManager::Commit` appends `TXN_COMMIT`
  and calls `Sync()` on the statement's own stack
  (`src/wal/manager.cpp:485-515`, the call at `:496`; `Sync()` at
  `:176-262`): one `pwrite` of everything staged,
  then `fdatasync` of every live segment
  (`src/wal/file_log_device.cpp:581-632`). The deferred path
  (`CommitDeferred`) is taken only where `KicksDurableWaiters()` holds
  (`src/server/command_dispatcher.cpp:1594`, spot-read), which is false at
  one core: no wake registry is built.
- **That sync is inside no `WaitScope`.** `commit.wal_flush`
  (`include/kds/stats/wait_event.hpp:125`, spot-read) is opened on the
  park path (`src/server/command_dispatcher.cpp:1278`, spot-read), which
  `strict` at one core never takes. Its commit time is on-CPU time in
  `sys.wait_stats` and `sys.session_waits`; `sys.ash` does not see it at
  all, because its sampler runs between polls and never shows ON_CPU
  (`docs/spec/wait-events.md:37`, `src/sched/scheduler.cpp:438-448`).
- **`group` stages and parks.** The commit is counted into
  `pending_group_commits_` (`src/wal/manager.cpp:503`, spot-read; `:520`
  is `StageStrictCommit`, the deferred `strict` stage above one core) and
  the statement parks under `commit.wal_flush`. The
  scheduler's post-task hook, phase 4.5
  (`src/sched/scheduler.cpp:453-467`, spot-read), runs `DrainOnce`, which
  with no registry calls the same inline `Sync()`
  (`src/wal/manager.cpp:589`, spot-read); the hook answers "did work", so
  the next iteration polls the parked statement. There is no commit
  delay and no sibling count: a batch is what one iteration staged. A
  1 ms drain timer exists beside it (`wal_drain_interval_us`).
- **Either way the reactor is in the syscall.** At one core every other
  session on the instance waits out each sync. Above one core a commit
  leaves its reactor (`CLAUDE.md`, the WAL row; `wal.md` §3); at one core the WAL writer thread
  runs and takes only the loss-window sync, the checkpoint's syncs and
  the reclaim (`src/wal/writer.cpp:74-146`).
- **No `O_DIRECT`, `O_DSYNC` or `io_uring`**; segments are 64 MiB,
  preallocated and zero-filled once.

### 1.3 What one commit writes `[source-read]`, by the research pass

- A record is a 32-byte header and its payload, aligned to 8;
  `TXN_COMMIT` is 32 bytes. The flush writes the staged bytes at their
  offset with no padding to a block.
- **The commit's `pwrite` carries the whole transaction**, and a page
  image with it: a write fetch of a frame that owes an image appends a
  `PACKED_PAGE_IMAGE` or a `FULL_PAGE_IMAGE`
  (`src/storage/device_page_store.cpp:2866-2915`), and at one core
  nothing flushes it before the commit does.
- **Every sync covers every live segment**
  (`docs/inflight/known-gaps.md:490`, spot-read), about 50 µs a segment
  beyond the first by that entry. Its breadth is DY's (DY-Q2), not EG's.
- **Not measured**: bytes written per commit, images per commit, or the
  number of live segments during any cell above. The images are
  countable: `sys.meta` carries `pool_fetch_images` and
  `pool_fetch_image_bytes` (`src/server/command_dispatcher.cpp:2600`, as
  the review read it), which S0 reads before and after a cell.
- **Two documents disagree with the code**: `wal.md` §6, whose heading is
  marked `[PROPOSED]`, describes in its items 2 and 3 a flush future and a
  phase-5 drain through `IoBackend`, where the code is synchronous
  syscalls in phase 4.5; and
  `manual/server/server.md:163` calls the per-transaction durability
  class "not wired" where `protocol.md` §9 and the session say it is.

## 2. The mechanism `[design]`

- **S0 designs nothing.** It measures `result-insert` on `main` at
  `strict` and at `group` beside PostgreSQL in one window, pinned, and
  splits the difference among named candidates: the statement itself
  (its row, its relation's indexes, its checks) in server CPU; the
  `group` rung's park and re-poll against `strict`'s inline call; the
  bytes and images in the commit's write; the live segments under its
  sync.
- **S1, the booking of the sync.** The inline sync is timed where it is
  called and added to `commit.wal_flush` for the session and the core,
  with no park and no latch: a core's own words, as the statistics rule
  requires (`docs/rules/rules.md` §3a).
- **S2 is not designed.** What it would cut is S0's finding.

## 3. Rules

- **EG-R1. A commit is acknowledged only after the sync that covers it
  returns**, on both rungs, as now. A change that acknowledges earlier is
  `[quiet-wrong]`: a client told "committed" that a crash then loses.
- **EG-R2. Nothing of DY's**: the latched roll, the breadth of a sync and
  the prewrite are not touched, and S0 measures the live-segment count
  without changing it.
- **EG-R3. One core only.** The path above one core is not changed, and
  a cell at eight cores shows it did not move.
- **EG-R4. A statistics record never waits and never fails what it
  describes** (DL): S1 writes its core's own words.
- **EG-R5. Equal terms**: the same rung meaning on both sides
  (`strict` or `group` against `synchronous_commit = on`), the same
  window, the same pinning; reads, where a cell has them, through EF's
  equal client.
- The standing rules, as DT's order lists them.

## 4. Items for the operator (EG-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| EG-Q0 | Is EG opened, and with what scope? | (a) S0 and S1, S2 behind EG-Q1; (b) S0 alone; (c) not opened, the insert left to EF-S2's run and §1.2 recorded in `known-gaps.md` | (a) |
| EG-Q1 | When does a cut open? | (a) only if S0 shows an autocommit insert on `main` behind PostgreSQL's by more than the larger spread **and** names a cause in the engine; otherwise EG closes after S1. (b) regardless | (a) |
| EG-Q2 | The one-core inline sync and the wait statistics | (a) booked as `commit.wal_flush`, timed in place; (b) left, and `wait-events.md` and `known-gaps.md` say a one-core `strict` commit's sync is on-CPU time | (a) |
| EG-Q3 | The reactor in `fdatasync` at one core while other sessions wait | (a) outside EG: measured at eight bookers in S0 and recorded, no premise shows it costs a cell; (b) inside: the sync handed to the WAL writer at one core as above one | (a) |
| EG-Q4 | The two documents of §1.3 | (a) corrected at EG's close whatever the gate; (b) left to their owners | (a) |
| EG-Q5 | S0's PostgreSQL side | (a) run in S0's window; (b) the control run reused | (a) |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **EG-S0** | `result-insert` on `main`, `strict` and `group`, beside PostgreSQL; the split of §2; one cell at eight bookers for EG-Q3 | the difference attributed or shown inside the spread; EG-Q1 read |
| **EG-S1** | The inline sync booked (EG-Q2 (a)); a cell pinning that a one-core `strict` commit shows its sync under `commit.wal_flush` in `sys.wait_stats` and `sys.session_waits` (`sys.ash` samples between polls and cannot see a blocking call on its reactor, booked or not; `wait-events.md` says so) | the suite; `wait-events.md` |
| **EG-S2** | (gated) The cut S0 names | its measured delta at one core and none at eight; the suite |
| **EG-S3** | The close: the documents of EG-Q4, `known-gaps.md`, the overhead measurement | the operator's word on landing |

## 6. What EG does not do

- No change to what `strict`, `group` or `relaxed` promise.
- No commit delay, no sibling threshold, no new setting.
- No `O_DIRECT`, `O_DSYNC` or `io_uring`.
- Nothing of DY's scope, and nothing above one core.
- Nothing of a read's cost (EF) or a fold's (EH).

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-28

On `pg-kds-comparison-orders` from `b95be2e86`, on the operator's word.
No stage is opened and no code is written. The premise EG was asked for -
"PostgreSQL's durable commit path is 16 % to 19 % faster" - was CLA's
reading of the comparison's first version, is false for the commit
(§1.1) and is corrected in that file; the operator chose this order's
re-scoped form over the original (`raft-marks-2026-11-28.md` §4).
