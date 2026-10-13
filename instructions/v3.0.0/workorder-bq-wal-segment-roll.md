# Work order BQ — the WAL segment roll: what its latched hold costs, measured, and the cheapest cut that meets the bar

**Written 2026-10-12** on `worktree-wal-segment-roll-order` from `59c4139f`
(`v2.7.0-906-g59c4139f`), on the operator's words W0-W2
(`raft-marks-2026-10-12.md` §13). **Opened 2026-10-12 by W4** (`raft-marks-2026-10-12.md` §16), with
BQ-Q1..Q5 marked as CLA proposed on W3.

**BQ takes BL-S4's ruling 3 out of BL as its subject and does not mark it.**
The ruling is *whether a roll keeps the stream latch across its device I/O*
(`workorder-bl-session-balancer-priority-affinity.md` §6). BQ answers it by
measurement, and BL-S4 waits on that answer and on BL's ruling 2 as before.

W0 and W1 were questions, answered in the session. Their answers are §0.2
and §1.3. W2 is the order's method, and it is BL's W10 restated for one
subject: a unit that shows no utility is re-explored, and if no alternative
survives, it is rejected, reported, and the order stops at that gate.

## 0. What BQ is

### 0.1 Subject and method

**The subject is the time a segment roll holds `WalStream::latch_`, and what
that hold costs every core.** It is not the roll's durability order, which
BQ keeps exactly (BQ-R1).

**Every stage after BQ-S0 is an experiment whose verdict is fixed before it
runs.** Each hypothesis in §3 states five things: its prediction, its
experiment, its reject criterion, its re-exploration, and what rejection
means. A criterion is never adjusted after its run, and never relaxed inside
a stage; changing one is an operator item. A candidate fix is first a
measurement build, never committed and archived as a patch beside its
results, as BL-S1's `slice-override.patch` was. It is committed only when
its cell meets the bar (§5.3).

### 0.2 Why it is its own order (W0's answer)

1. **The owning document is not BL's.** A change rewrites `docs/spec/wal.md`
   §6 step 1 and AL-R1's boundary (`workorder-al-m0-single-wal.md`).
   AL-R1 put the whole append under one latch, the roll included, after the
   reserve/copy/publish split was tried and abandoned.
2. **A wrong cut is `[quiet-wrong]`.** Releasing the latch for the roll's
   I/O re-decides four things:
   - where an append lands while a segment is being made;
   - the flush order;
   - the fail-stop;
   - the recovery scan's premise (`wal.md` §4.1).

   A wrong answer there is a record recovery cannot see, not a refusal.
3. **Its benefit does not depend on BL.** Every writer and every commit
   waits out a roll whether or not a session is ever moved. Inside BL, its
   effect could not be told apart from placement's.

### 0.3 Not BQ

- **The read path's logged write.** That is BL-S4's ruling 1, built at
  `05cd011f`.
- **Core 0's role under BL's partition.** That is BL-S4's ruling 2.
- **Log admission or throttling of a bulk writer** (the archived trace's
  §3, `bench/v3.0.0/archive/bl-s1-v2.7.0-857-g3552a03c/wal-coupling.md`).
  It lowers how often a roll happens, not what one costs. It also cannot
  sit inside `Append`, which runs after its page changed (`wal.md` §6
  step 1).
- **A parked append.** An appending task holds the page latches of the
  mutation it logs. Parked on the stream latch, it would let another task
  on its reactor ask for one of those pages. The blocking `Latch::lock`
  (`include/kds/base/latch.hpp:70-75`) stays.
- **A dedicated WAL device** (CN-9). BQ measures this host's device and
  says so.

### 0.4 What BQ can conclude

| outcome | BL-S4's ruling 3 | what lands |
|---|---|---|
| **BQ rejected at the premise** (H0) | keep the latch | the counters (BQ-Q3), the results file, `known-gaps.md` re-priced |
| **A rung below 4 meets the bar** (H2-H5) | keep the latch | that rung |
| **Only rung 4 meets it** (H6) | release the latch for the roll's I/O | H6, with `wal.md` §6 rewritten and AL-R1 annotated |
| **Nothing meets it** | keep the latch, with the cost named | every rung's numbers and rejection, `known-gaps.md` |

## 1. Survey at `59c4139f` `[source-read]`

### 1.1 The roll, step by step

`WalStream::Append` (`src/wal/stream.cpp:249-299`) takes the stream latch at
`:261`. When the record does not fit the segment, it seals (`:265`) and
rolls (`:268`) **before releasing the latch**. Under that one hold:

| step | code | device work |
|---|---|---|
| a. seal | `SealLocked` (`:206-247`): a `PAD` header, then `FlushLocked` | waits out an unlatched flush in flight (`WaitOutFlushInFlight`, `:346-348`, a yield spin), then a `pwrite` of the staged ring (at most 1 MiB, `kDefaultRingCapacity`) |
| b. flush | `Roll` (`:176-183`): `FlushLocked` again | normally nothing left |
| c. sync the segment left | `StartSegment` (`:87-99`) calls `SyncDevice` (`:74-79`) | takes `sync_mutex_`, so **it waits for the writer thread's sync in flight**, then `FileLogDevice::Sync` (`src/wal/file_log_device.cpp:519-566`): an `fdatasync` of **every live segment** |
| d. the body | `CreateSegment` (`file_log_device.cpp:376-450`) takes `prepare_mutex_` at `:382` | **built ahead** (BA-S12, asked at half a segment, `stream.cpp:291-296`): nothing to do. **Preparer still building**: the roll waits on `prepare_mutex_`, which `BuildAhead` holds across its whole build (`:358-374`). **Nothing built**: `BuildBody` runs inline (`:412`): `posix_fallocate`, a 64 MiB zero prewrite and its `fsync` (`:152-159`) |
| e. the header | `WriteHeaderAndSync` (`:109`, called at `:415`) | a `pwrite` and an `fdatasync` |
| f. the name | `renameat2(RENAME_NOREPLACE)` (`:421`) | a journal operation |
| g. the directory | `SyncDirectory` (`:438`, `:628-645`) | an `fsync` of the WAL directory, serialized with the reclaim's (`:611`) under `dir_sync_mutex_` |

Steps c-g also compete for the device with the preparer's zero fill and
`fsync` for the following segment (the archived trace §1 item 1).

**A reactor whose task meets the held latch serves none of its sessions
until the roll ends.** `Latch::lock` is a blocking `std::mutex::lock` that
charges the wait to the latch's kind (`latch.hpp:70-75`).

**A drain meets a roll through `RequestSyncNow`'s latched `Flush`**
(`src/wal/manager.cpp:293`, `stream.cpp:310`). Two drains do this:
- **Core 0's drain** takes the latch once per `relaxed_flush_interval_ns`
  (10 ms, `include/kds/wal/manager.hpp:115`) while anything is unsynced, so
  it meets every roll longer than 10 ms.
- **A peer's drain** takes it whenever a group commit is pending on that
  core (`manager.cpp:486-505`).

### 1.2 What is measured and what is not

- **Measured** (BL-S1,
  `bench/v3.0.0/results-bl-s1-placement-premise-v2.7.0-857-g3552a03c.md` §4):
  under one bulk inserter, cores with no batch session waited 0.69-4.13 s
  per 10 s on the stream latch, and the batch core waited 4.14 s on the
  sync gate. That was **with `TouchPattern`'s logged write still on the
  read path**.
- **Not measured, by anyone**:
  - the roll's own hold;
  - how the hold splits over steps a-g;
  - how often step d missed;
  - any stall after `05cd011f`.

  `known-gaps.md`'s WAL entry, *"A segment roll does I/O inside the
  instance's append latch ... its cost was never priced"*, still holds.
  BL's re-measurement of ruling 1 stopped at 2 of 18 cells.
- **What exists**: `load_c<k>_waited_<latch>_us` per core and latch kind
  (`src/server/command_dispatcher.cpp:1844`). It says who waited, not who
  held or why. `SHOW META` has no roll counter.

### 1.3 Why the segment size is not a free knob (W1's answer)

- **`kDefaultSegmentSize` is 64 MiB** (`include/kds/wal/log_device.hpp:58`).
  It is a constant. `wal.md` §4.1 marks the size `[OPEN: size]`, and §13
  lists "segment size" under Configuration, but no setting exists.
- **The size is a property of the volume.**
  - A segment file of another size is `Corruption` at open
    (`file_log_device.cpp:288-296`).
  - An LSN maps to its segment as `lsn / segment_size`
    (`include/kds/wal/stream.hpp:79-80`), and the anchor's redo start relies
    on that mapping (`src/server/superblock_checkpoint_anchor.cpp:51`).

  So changing the size is a format event (BQ-Q4).
- **A larger segment lowers the roll rate, not a roll's hold.**
  - Steps c, e, f and g do not scale with the size.
  - Step d does: a roll that misses the body waits for the rest of a larger
    build.
  - Whether misses get more frequent is not argued here. The build is asked
    for at half a segment, so the build's length and the time available
    grow together. H4 measures it.
- **Side costs of a larger segment**:
  - the mount's `ScanTail` reads the whole last body into one allocation
    (`stream.cpp:148-150`);
  - recycling (BC) frees whole segments, so up to one more segment of log
    stays on disk;
  - the preparer's temporary file is one more segment.

### 1.4 Two stale premises

- **`FileLogDevice::Sync` syncs every live segment, and its comment forbids
  narrowing it** (`file_log_device.cpp:519-536`). The comment gives two
  reasons:
  - a late write into an earlier segment;
  - a roll landing between `WalStream::Sync`'s capture and its device sync.

  Since `a1679272`, the roll syncs the segment it leaves before the next one
  exists (step c), which may make the second reason moot. Whether it does is
  H3's proof, not a reading.
- **AL-R1 says "the device sync is outside the latch."** Step c put one
  inside it at `a1679272`. `include/kds/wal/stream.hpp:30-45` states the
  current protocol correctly.

## 2. Rulings

### BQ-R1 — The roll's durability order is fixed

Whatever BQ changes, `wal.md` §4.1's order holds:
- the segment left is synced, records and seal, before the next one exists;
- the next segment's header is durable before its name;
- a failed write, sync or creation stops the log (`wal.md` §6 step 5)
  before any later flush can publish past it.

A candidate that relaxes any of the three is rejected without a run.

### BQ-R2 — The rungs, cheapest first

A candidate is built only after every cheaper rung has been measured and
has missed the bar.

| rung | what | hypothesis |
|---|---|---|
| 0 | nothing | the premise, H0 |
| 1 | sizes and timing, no protocol change: the body asked for earlier or two ahead, then the segment size | H4, then H2 |
| 2 | a drain does not block on a roll | H5 |
| 3 | the sync narrowed to the segments written since the last sync, `[quiet-wrong]` | H3 |
| 4 | the latch released for the roll's I/O, `[quiet-wrong]` | H6 |

H1 decides the order of rungs 1-3 and skips none of them. A rung that meets
the bar ends the ladder, and the rungs above it are recorded as not needed,
not as rejected.

### BQ-R3 — How every number is taken

Every number is measured in `build-release`, interleaved A/B, on a quiet
host, and named by `git describe` (CLAUDE.md, `bench/README.md`).

## 3. Hypotheses

"C1", "C2" and "C3" are the cells of §5.1. "The spread" is §5.2's.

### H0 — The premise: after `05cd011f`, a roll still stalls cores with no bulk writer

- **Prediction.** A pure reader no longer appends, so it no longer meets the
  latch. What still meets it is any session that writes, any commit, and a
  drain (§1.1). So:
  - in C1, readers on core 0 still stall;
  - in C2, small writers on every core show tails at the roll's period.
- **Experiment.** BQ-S1's counters and per-roll log, at the `main` current
  when BQ-S1 starts. Run C1 and C2 at `relaxed` and `group`, interleaved,
  with at least 50 rolls per arm (§5.1).
- **Reject criterion.** H0 is rejected if, in both C1 and C2:
  - the largest roll hold is under 1 ms; **or**
  - on cores with no bulk writer, the stream-latch wait overlapping a roll
    is under 10 % of the cores' total latch wait, **and** the readers' and
    small writers' schedule p99 is within 2x of their isolated p99 (BL-Q14
    (ii)'s factor).
- **If rejected.** There is no re-exploration inside BQ. A stall with
  another cause is named by its holder (a checkpoint turn, a carve, a
  writeback) and handed to `known-gaps.md`. BQ closes on §0.4's first row.

### H1 — One or two steps carry most of the hold

- **Prediction.** Step c (the gate wait and the sync) and step d (a missed
  body) carry most of the hold. Steps e-g together are a few milliseconds on
  this host.
- **Experiment.** BQ-S1's per-step timers, in the same runs as H0.
- **Measure.** A step's share is its time summed over the rolls whose hold
  is at or above the hold's p99, divided by those rolls' summed hold.
- **Reject criterion.** Rejected if no step's share reaches 40 %.
- **If rejected.** Rungs 1-3 are all still measured. H1 only sets their
  order.

### H2 — A larger segment lowers the stall enough (W1)

- **Prediction.**
  - The roll rate falls as 1/size.
  - The per-roll hold stays flat, or grows where step d misses.
  - Total stall time falls roughly as 1/size.
  - The stall's p99.9 and maximum do not fall.
- **Experiment.** Measurement builds at 16, 64 (the base), 256 and 1,024
  MiB, each on a fresh volume, running C1, C2 and C3.
- **Reject criterion.** Rejected as the fix if either holds:
  - at the smallest size meeting the bar's p99 term, its p99.9 term is not
    met, which means the size made the stall rare rather than cheap;
  - the size that meets the bar costs C3 more than 3 %, or adds more than
    1 s to the mount (`ScanTail`).
- **Re-exploration.** If H4 lands, H2 is re-run on H4's build.
- **If accepted.** The size change is a format event, BQ-Q4.

### H3 — The sync's breadth is a material share of step c

- **Prediction.** `fdatasync`ing the live segments other than the tail is a
  measurable share of step c, both in the roll's own sync and in the
  writer's sync it waits out. The share grows with the live run (the
  checkpoint interval's worth of log; `known-gaps.md` WAL, *"Every sync
  covers every live segment"*).
- **Experiment.** BQ-S1's split of `Sync`'s loop into the tail's `fdatasync`
  and the rest, with the live segment count beside it. Then a measurement
  build that syncs only the segments written since the last sync.
- **Reject criterion.** Rejected if either holds:
  - the non-tail `fdatasync`s are under 10 % of step c's summed time over
    the rolls at or above the hold's p99;
  - the proof below cannot be written.
- **The proof owed before any commit** (`[quiet-wrong]`). Every byte below a
  published durable LSN must be covered by a device sync that started after
  that byte was written. It must hold:
  - for both callers of `Sync`;
  - when a roll lands between `WalStream::Sync`'s capture and its device
    sync;
  - across `DetachBelow`;
  - across a failed sync stopping the log;
  - **at open.** The stream takes whatever is on the device as durable
    (`stream.cpp:62-64`), and after a process crash the previous run's
    unsynced page-cache bytes in non-tail segments are made durable only by
    the first all-segment sync. So the written-since set starts as every
    live segment.
  - **against the comment's first reason**, a late write into an earlier
    segment: either show that the only `WriteAt` callers
    (`stream.cpp:334`, `:364`) write into the current segment, or record
    that reason as stale.

  Each interleaving gets a cell that forces it with a barrier and is
  repeated.
- **If rejected.** The comment at `file_log_device.cpp:519-536` stays and is
  re-dated.

### H4 — Step d misses under a bulk writer

- **Prediction.** At C3's rate, the preparer has not finished the zero fill
  when the roll arrives, so the roll waits on `prepare_mutex_` or builds the
  body inline.
- **Experiment.** BQ-S1's step d outcomes (hit, waited on a build, built
  inline) and their wait times.
- **Reject criterion.** Rejected if the missed rolls' summed hold is under
  10 % of all rolls' summed hold in C3. The share of hold time is the test,
  not the share of rolls: 3 % of rolls at 500 ms each can be the whole
  p99.9.
- **Re-exploration, in order**, each as a measurement build:
  1. ask for the body at a quarter of a segment;
  2. keep two bodies ahead;
  3. skip the zero prewrite where `posix_fallocate` succeeds.
     - **The prewrite and the body's `fsync` stay where `Reserve` fell back
       to `ftruncate`** (`file_log_device.cpp:95-104`). That file is sparse,
       and the prewrite is what keeps an accepted append from failing for
       space.
     - **The body's `fsync` stays in every case**, because
       `WriteHeaderAndSync`'s `fdatasync` relies on it (`:106-108`).
     - Rejected if C2's `group` commit p99 rises by more than the spread,
       since the first write to an unwritten extent may cost every commit's
       `fdatasync` a metadata commit (`:552-556`).

### H5 — A drain meeting a roll is a material share of its core's stalls

- **Prediction.** On core 0, and on a peer with a group commit pending,
  reactor stalls line up with rolls through `RequestSyncNow`'s latched
  `Flush`.
- **Experiment.** BQ-S1's `wal_drain_latch_wait_us`, set against
  `load_c<k>_waited_wal_stream_us`, and the per-roll log. Then a measurement
  build:
  - a non-blocking `WalStream::TryFlush` that `try_lock`s the latch;
  - a drain refused by it skips the tick, but **still asks the writer to sync
    `flushed_lsn()`**, which is atomic and needs no latch, so the writer's
    kick reaches parked committers;
  - after a bounded number of refused ticks, the drain blocks as today.
- **The correctness owed.**
  - **The loss window.** While a roll holds the latch, no append completes,
    and the roll publishes the segment it synced (`stream.cpp:96-98`). So a
    skipped tick adds nothing to the loss window.
  - **The wake.** Above one core, a parked committer is woken only by the
    writer's kick (`src/wal/writer.cpp:143`), and the roll's
    `PublishDurable` kicks nobody. **A skipped drain that asked nothing would
    leave the committer to wait out an idle block.** That is why the skip
    still asks the writer.
  - **Starvation.** The bound keeps ordinary append contention from starving
    the drain.
  - **Cells, each repeated:**
    - a `group` committer parked across a roll is woken by the writer's kick
      within one sync;
    - the drain blocks after the bound.
  - **Mutation:** the skip's writer request removed, killed by the wake cell.
- **Reject criterion.** Rejected if either holds:
  - in C1 with readers on core 0, the readers' schedule p99 does not fall by
    more than the spread;
  - C2's `group` commit p99 rises by more than the spread.

### H6 — Only the latch's release for steps c-g meets the bar

Taken up only after rungs 1-3 miss, and only on BQ-Q5's mark.

- **The shape** (the archived trace's §2 (c), to be re-derived at the code
  current when the rung is reached):
  - **under the latch**: seal, `FlushLocked`, move `append_lsn_` to the next
    segment's first LSN, claim `rolling_`;
  - **outside it**: steps c-g;
  - **then** release the claim.
- **It must keep** all three clauses of BQ-R1. The stop is raised before the
  claim is released, as the unlatched flush already does
  (`stream.cpp:335-341`).
- **The hazards it must close:**
  1. **The ring fills during the roll.** `Append` returns `OutOfSpace`, and
     the manager drains up to `kRingDrainAttempts` times before
     fail-stopping (`manager.cpp:365-386`). Those drains must wait out the
     roll and not count as attempts.
  2. **A second roll while the first is claimed.** At the floors, ring and
     segment are both 512 KiB (`kMinRingCapacity`, `kMinSegmentSize`), so
     appends during a roll can fill the new segment. `Append` and `Seal`
     must wait out the claim.
  3. **A reader of the moved `append_lsn_` that skips the latch.** The
     drain's early-out (`manager.cpp:483`) and a checkpoint's redo start can
     see an LSN whose segment does not exist yet. An anchor naming a missing
     segment is `Corruption` at mount (`wal.md` §4.1).
  4. **The numbering.** `Roll` numbers the new segment from
     `device_->end_segment()`, on the premise that it and `append_lsn_`
     cannot disagree (`stream.cpp:180-182`). H6 breaks that premise.
- **Its gain is capped by the ring.** At a bulk writer's rate, the 1 MiB
  ring may fill within the roll, and then every appending core waits the
  claim out anyway. Under the latched roll no append runs during a roll,
  so no counter can see the ring fill during one. The cap is estimated
  from BQ-S1's numbers instead: the log's byte rate (the delta of
  `wal_append_lsn` over a window) times the roll hold's distribution (the
  per-roll lines' `hold_us`), against the ring's capacity (1 MiB default,
  512 KiB floor). The rate is the one achieved with appenders stalled by
  the hold, not the one offered, so the estimate is a lower bound.
- **Cells owed, each forced with a barrier and repeated:**
  - an append at the new segment's first LSN during its creation;
  - a flush claimed during the roll;
  - a sync captured during the roll;
  - a second roll during the first;
  - an anchor taken during the roll;
  - a failed step c, e, f or g, which must stop the log before any flush
    publishes past it;
  - the ring filled during the roll;
  - a crash inside each of steps c-g, in `scripts/sim.sh`'s corpus.
- **Mutations, each repeated:**
  - the claim not waited out by `Flush`;
  - the ring-full drain counted as an attempt;
  - the stop raised after the claim's release.
- **Reject criterion.** Rejected if it misses the bar, if C3 loses more than
  3 %, or if any owed cell cannot be written. A cell that cannot be written
  means the interleaving cannot be forced, so there is nothing for the
  proof to stand on.

## 4. Stages

| stage | what | done when | size |
|---|---|---|---|
| BQ-S0 | **The order** | <ul><li>This file and its review.</li><li>`index.md`'s BQ row.</li><li>`raft-marks-2026-10-12.md` §13.</li><li>BL's §6 pointer.</li></ul> | S |
| BQ-S1 | **The instruments and the premise** (H0, H1, and the measurement half of H3, H4, H5) | <ul><li>**Code, instrumentation only.** Printed by `SHOW META` as `wal_roll_*` (BQ-Q3):<ul><li>per roll: the hold, and the time in each of steps a-g;</li><li>step c split into its gate wait, the tail's `fdatasync` and the rest, with the live segment count;</li><li>step d's outcome;</li><li>`wal_drain_latch_wait_us`, the drains' share of the stream-latch wait. The rest of `load_c<k>_waited_wal_stream_us` is every other stream-latch acquisition on that core - statements' appends and commit flushes, the checkpoint's flush, writeback's `EnsureDurable` flush, the anchor publish's `DetachBelow`, and on core 0 the threads that declared no core - so it bounds the statements' share from above, and is not that share.</li></ul></li><li>**A per-roll log line**: start and end in steady-clock ns, with the step times, so stalls can be lined up with rolls by timestamp.</li><li>**Cells:**<ul><li>a forced roll on a two-core rig moves every counter;</li><li>at `cores = 1`, a forced roll moves the step timers while `wal_drain_latch_wait_us` stays 0, because the latch is null.</li></ul></li><li>**The measurement**: §5.1's C1-C3, and BL's ruling 1 re-measurement if BQ-Q1 is (a).</li><li>**The verdicts** of H0 and H1 in §7.</li><li>**The suite green.**</li></ul> | M |
| BQ-S2 | **Rung 1** (H4, then H2) | <ul><li>The measurement builds and their verdicts.</li><li>A candidate that meets the bar is committed, with its cells and with BQ-Q4 answered if it is a size.</li><li>The suite green.</li></ul> | M |
| BQ-S3 | **Rungs 2 and 3** (H5, H3), each only if the rung below missed | <ul><li>H5: the measurement build, then the build with its cells and mutation.</li><li>H3: the proof first, then the measurement build, then the build with its interleaving cells.</li><li>The verdicts.</li><li>The suite green.</li></ul> | M |
| BQ-S4 | **Rung 4** (H6), only if rungs 1-3 missed and BQ-Q5 is marked (a) | <ul><li>The design re-derived at the current code, and its review before any code.</li><li>The measurement build.</li><li>The build with every cell and mutation of H6, and the `scripts/sim.sh` corpus with the new crash points.</li><li>The suite green.</li></ul> | L |
| BQ-S5 | **The close** | <ul><li>The overhead A/B over BQ's whole change (CLAUDE.md step 3).</li><li>**Text:**<ul><li>`wal.md` §4.1 (the size) and §13 ("segment size" under Configuration, a setting that does not exist);</li><li>`wal.md` §6 step 1, whose self-contradiction is resolved whichever way BQ ends, and §6 step 5's "that cost is not measured", priced by BQ's results;</li><li>`wal.md` §13's observability list, naming the roll's `SHOW META` fields;</li><li>`stream.hpp`'s protocol header;</li><li>`known-gaps.md`'s two WAL entries and its BL entry, re-priced.</li></ul></li><li>AL-R1 annotated with a dated note pointing at `wal.md`, not rewritten, since it is the operator's ruling.</li><li>BL's §6 ruling 3 given BQ's answer.</li></ul> | S |

## 5. Measurement

### 5.1 The cells

All cells run at 4 cores, interleaved, at both `relaxed` and `group`, since
`group`'s sync hides anything smaller than itself. Runs are long enough for
**at least 50 rolls per arm per durability**: the roll-hold statistics are
otherwise a handful of maxima.

- **C1 - the mixed cell.** `tools/bl_mixed.py --cores 4 --arm mixed`, with
  placements `segregated-no0`, `batch0` and `spread`. These are BL-S1's
  cells.
- **C2 - the writer cell.** New in BQ-S1, an arm of `bl_mixed.py` or a
  sibling driver. Eight autocommit sessions run single-row pk `UPDATE`s on
  cores 0-2, beside the bulk inserter on core 3. Read for each session:
  statement p50, p99, p99.9 and maximum. **An autocommit session's commit
  latency is its statement's round trip**, since its one reply follows the
  commit (and, at `group`, the sync); it is read from the send
  (`writer_service`) and from the due instant (`writer_from_schedule`), and
  "C2's commit p99" below is `writer_from_schedule`'s p99.
- **C3 - the bulk writer alone.** C1's inserter with nothing else. Read rows
  per second and log MiB per second. It prices a candidate's cost to the
  writer that causes the rolls.
- **Isolated baselines.** C1's `isolated` arm, and C2 without the inserter.

### 5.2 What every results file carries

- the commit and `git describe`;
- the full percentile table, p0 and p25 included;
- the `wal_roll_*` counters and the roll count per run;
- the per-core `load_c<k>_waited_*`;
- the device and filesystem of the WAL directory;
- the delta against BQ-S1's own base;
- **the spread**: the maximum minus the minimum of the base arm's runs, for
  each figure a criterion reads.

### 5.3 The bar (BQ-Q2)

CLA's proposal, on cores with no bulk writer, in C1 and C2 at `relaxed`:

- **(i)** the schedule p99 is within **2x** of its isolated baseline;
- **(ii)** the p99.9 is within **5x** of its isolated baseline;
- **(iii)** C3 loses no more than **3 %**.

Term (ii) is there because H2 can meet (i) by making rolls rare rather than
cheap. At `group`, the same bar applies against `group`'s own isolated
baseline.

## 6. Items for the operator

Every proposal is `[design]` unless a cell says otherwise.

| item | question | kind | CLA's proposal | mark |
|---|---|---|---|---|
| BQ-Q0 | **Open BQ**, with BQ-S0..S5 and BQ-R1..R3 as written | process | Yes. It depends on no open order, and BL stays paused | **Opened by W4** (2026-10-12, `raft-marks-2026-10-12.md` §16) |
| BQ-Q1 | **Does BQ-S1 also take BL's owed ruling 1 re-measurement?**<br>(a) Yes: same driver and host session, `05cd011f` against `3338cf0c`, its own results file under BL's name.<br>(b) No: BL takes it when it resumes | process | (a). It is the C1 run BQ-S1 needs anyway, plus one arm, and it stops `known-gaps.md`'s BL headline describing an engine that no longer exists | **(a)**, as proposed, on W3 (§16) |
| BQ-Q2 | **The bar** (§5.3).<br>(a) As proposed.<br>(b) p99 only.<br>(c) Other numbers | measurement | (a). Under (b), H2 can pass on rarity alone | **(a)**, as proposed, on W3 (§16) |
| BQ-Q3 | **The `wal_roll_*` counters after BQ.**<br>(a) Kept as `SHOW META` fields. `wal.md` §13 lists flush latency and ring-full stall time as product observability, and a roll's hold belongs beside them.<br>(b) Removed at BQ-S5 | user-visible | (a), and not a `sys` relation: those are BO's | **(a)**, as proposed, on W3 (§16) |
| BQ-Q4 | **If H2 lands, how the size changes.**<br>(a) The size recorded on the volume and read at mount, with the superblock bumped and every older volume refused.<br>(b) A new constant, with the superblock bumped and every older volume refused.<br>(c) A setting | format | (b). (a) adds a field that pays only if a later change wants a second size. (c) widens the scope to a tuning surface no workload has asked for. Under the standing order, none of the three mounts an older volume | **(b)**, as proposed, on W3 (§16) |
| BQ-Q5 | **Rung 4's license.** H6 rewrites `wal.md` §6 step 1 and crosses AL-R1.<br>(a) BQ-S4 may build it if rungs 1-3 miss, with its review before the code.<br>(b) BQ stops at rung 3 and reports, and H6 needs its own word | process, **[quiet-wrong]** | (b). AL-R1 was the operator's ruling, and H6's proof is the largest in the order | **(b)**, as proposed, on W3 (§16): BQ stops at rung 3 |
| BQ-Q6 | **May a bound from BQ-S1 record a rung as unable to meet the bar?** BQ-R2 builds a rung only after every cheaper one "has been measured and has missed".<br>(a) Record H4 options 1-2, H5 and H3 as unable to meet the relaxed bar from BQ-S1's bounds, each bound in §7; build only H4 option 3 and H2, whose bounds rest on premises BQ-S1 did not measure. If both miss, BQ closes on §0.4's row 4 and stops before rung 4.<br>(b) Build every rung as written.<br>(c) Bound everything, H2 and option 3 included, and close now | process | (a). The H5 and H3 bounds remove the very stalls each rung targets and still miss: H3's by 590x or more (every non-tail sync removed leaves hold p99 at 652-780 ms), H5's with C1 reader p99 at 2.6-145 ms against an isolated 0.21-0.56 ms; H3's build carries a `[quiet-wrong]` proof that would buy nothing. H2 and option 3 are cheap, change no protocol, and their bounds rest on an unmeasured flat hold and on device contention | **(a)**, as proposed, adopted by CLA under the operator's standing go-ahead of 2026-10-12 (go-ahead-achieving-milestone, §16), not marked by the operator |
| BQ-Q7 | **Group C2's miss has a holder no rung touches.** Its writer p99 is 28-30x its isolated arm, and 0 of about 240 samples at or above p99 line up with a roll: it is commit-sync queueing behind the bulk writer (half the syncs, about 3x service p50).<br>(a) Name that holder in `known-gaps.md` and judge the group bar on C1 only.<br>(b) Keep the bar as written, so every rung misses at group | measurement | (a), by H0's own rule: a stall with another cause is named by its holder | **(a)**, as proposed, adopted by CLA under the operator's standing go-ahead of 2026-10-12 (go-ahead-achieving-milestone, §16), not marked by the operator |

## 7. Row status

### BQ-S0 - written 2026-10-12

Written on `worktree-wal-segment-roll-order` from `59c4139f`
(`v2.7.0-906-g59c4139f`), on W0-W2. Its `critics-developer` review was
applied in the same stage:
- H5's skipped drain now asks the writer, because the roll's publish kicks
  nobody;
- H3's proof gains the open and the late-write reason;
- H6 gains three hazards and the ring's cap;
- H4 option 3 keeps the prewrite on the `ftruncate` path and the body's
  `fsync`;
- the instruments behind H0 and H3 were added;
- the H-numbering was made contiguous;
- BQ-Q4 was re-cast under the standing order;
- the duplication was cut.

No code, spec edit or test. The suite was not executed and no pass is
claimed. Not opened (BQ-Q0).

### BQ opened 2026-10-12 (W3, W4)

W3 asked for BQ to start once BP had closed or paused, and to go ahead
until the milestone closes or a failure or ineffectiveness is found; W4
lifted the BP gate. BQ-Q0 opened, BQ-Q1..Q5 marked as proposed
(`raft-marks-2026-10-12.md` §16). BP-S1 was measuring on the host when BQ
opened, so BQ's builds ran niced beside it and BQ's measurements wait for
a host with no BP run live.


### BQ-S1 - instruments built; the measurement and the verdicts owed

Built on `worktree-wal-segment-roll-order` from `dbff867b`, landing in the
commit that carries this entry. Overhead not measured; measured at the
milestone's close. The measurement (C1-C3), BQ-Q1 (a)'s BL re-measurement
and the H0/H1 verdicts are **not yet taken**: they follow on the release
build of this commit, on a host with no BP run live, and land as their own
results files and as the next entry here.

**What was built.**
- `RollRecord`: one roll's hold, steps a-g, the gate's wait, its own
  sync's tail/rest split, the syncs that finished while it waited (summed,
  with their count), and the body's outcome.
- `RollStat` totals, printed by `SHOW META` on every core (BQ-Q3 (a)), with
  `wal_append_lsn` beside them.
- Three contention tallies, per core as `load_c<k>_*` and summed as
  `contention_*`: the drain's stream-latch wait, and the waits a roll's hold
  covered, with the covered part.
- One Info line per roll, `[wal] roll seg=...`, with steady-clock start and
  end and every step.
- The driver's C2 (`--writers`) and C3 arms, p99.9, durability in the tag,
  the server log kept, the control reads pinned and re-read
  (`--ctl-core`), and the balancer's moves counted per run.

**Cells.** Six in `tests/wal_roll_instruments_test.cpp`:
- a roll behind a held sync, with a drain blocked behind it;
- two rolls at one core, with their log lines;
- a body built inline;
- a body waited for;
- `SHOW META` on an owning core;
- `SHOW META` on an attached core.

Thirteen mutants, each built and run three times, were all killed three
times out of three:
- the overlap charge;
- the drain guard;
- the waited-out syncs, their split, and the running totals they are read
  from;
- the gate charged the sync;
- `RecordRoll`;
- `LogRoll`;
- tail/rest swapped;
- inline and waited reported as hit;
- the totals owner-only;
- the flush timer.

**Where the build departs from this row.**
1. **"Ring-full events during a roll" is not a counter.** Under the latched
   roll no append runs during a roll, so none can see the ring fill during
   one. A first draft counted ring-full drains that waited out a roll; the
   review deleted it as a different event that would be read as H6's cap.
   H6's cap is derived instead (H6, "Its gain is capped by the ring").
2. **The drain's share is `contention_wal_drain_latch_wait_us`** and, per
   core, `load_c<k>_wal_drain_latch_wait_us`. The per-core field is absent
   when 0. The rest of `load_c<k>_waited_wal_stream_us` bounds the
   statements' share from above and is not that share.
3. **`SHOW META` carries totals, `wal_roll_hold_max_us` and the segment
   count's sum and maximum, never per-roll values.** H1's share over the
   rolls at or above the p99, and H3's per-roll split, are read from the
   per-roll lines. C1-C3 therefore run at `log_level = info`.
4. **The cells force each counter in its own shape** rather than all of
   them in one two-core roll: hit, waited and inline exclude each other.

**Review.** Two `critics-developer` passes.
- The first had four reviewers, each finding verified adversarially: 35
  confirmed and 6 refuted. All 35 were applied except E5, which is
  rejected. E5 asked to collapse the roll's four field lists into one
  table. The lists serve different readers (the record, the names, the
  totals, the log line), the drift the review found was fixed directly
  (E2), and an accessor table adds indirection for no reader.
- The second found five defects in the first's fixes, all applied:
  - a torn read of the sync totals, now under a mutex of their own;
  - a waited-out sync reported with no wait;
  - two false clauses in the `rules.md` Logger row;
  - a misleading "a peer's roll time" sentence;
  - a `per_core_valid` that counted the control session's own moves.

### BQ-S1 - measured; H0 and H1 held, the bar missed at relaxed

Measured at `4cf30270` (`v2.7.0-909-g4cf30270`) on a release build, 3 runs
of every §5.1 cell at `relaxed` and `group`, interleaved and gated per cell
on a quiet host (54 cells, every one with a result); BQ-Q1 (a)'s BL pair in
the same host session. Results:
`bench/v3.0.0/results-bq-s1-segment-roll-v2.7.0-909-g4cf30270.md` and
`bench/v3.0.0/results-bl-ruling1-remeasure-v2.7.0-909-g4cf30270.md`. Every
verdict below was recomputed from the raw files by one agent and then
attacked by two more (one recomputing, one re-reading the criterion and
the instruments); `analyze_s1.py`'s two errors (a hardcoded bulk core, and
deltas that drop keys absent from the first read) do not reach them.

- **H0 - held** (`[measured]`). Relaxed, by 100x or more on every clause:
  the largest hold is 587 ms to 1.21 s, rolls cover 99.4-100 % of the
  stream-latch wait on cores with no bulk writer, and readers' and small
  writers' schedule p99 is 1,032-1,996x their isolated arm in C1 and
  1,399x in C2 (medians; 375-6,082x over run pairings). Group, by the
  letter: C2 fails both clauses (hold 15-21 ms, 81-84 % covered, p99
  28-30x), but none of C2's tail lines up with a roll (BQ-Q7). BQ goes on to
  the rungs (§0.4).
- **H1 - held.** Relaxed: step c carries 89.8-99.5 % of the hold over the
  rolls at or above p99, almost all of it the **gate wait** - the roll
  waiting out the WAL writer's sync already in flight, whose tail
  `fdatasync` alone takes 300-700 ms on a device that also takes 75-80 MiB/s
  of log and the checkpoint's writeback. Group: held by the letter on
  one-roll bins, the leading step flipping between c and g.
- **H3 (measurement half) - not rejected, and bounded out.** The non-tail
  syncs are 39.8-72.0 % of step c in C1-spread and C2. But removing every
  non-tail sync, the roll's own and the waited-out writer syncs', leaves
  hold p99 at 652-780 ms in every relaxed cell (`h3_bound.py`), so H3 cannot
  meet the bar (BQ-Q6 (a)).
- **H4 - not rejected at relaxed, rejected at group.** Relaxed C3: the
  missed rolls hold 11.9 % of all hold (per run 8.3 / 16.3 / 11.1 %), but
  step d's own wait is 0.3-0.8 % of it; missed rolls are long for the gate,
  not the miss. Group: 0 misses in 75 rolls. Options 1-2 are bounded out
  (relaxed p99 stays at 106 ms or more with step d removed); option 3 acts
  on the device and needs its build (BQ-Q6 (a)).
- **H5 (measurement half) - material at relaxed on core 0 only.** Core 0's
  drain is about 100 % of its stream-latch wait; peers carry none at
  relaxed. Bounded out: with every roll-aligned stall removed, relaxed C1
  reader p99 is still 2.6-145 ms (BQ-Q6 (a)).
- **The bar (§5.3) - missed at relaxed** by 200-2,000x in every C1 placement
  and in C2; at group C1 is borderline by run pairing, and C2's miss is not
  the roll's (BQ-Q7).
- **BL ruling 1** (`05cd011f` against `3338cf0c`): reader service p99 fell from
  262-283 ms to 1.7-3.0 ms (medians); schedule p99 is still 277-295 ms
  (medians), which is the roll's.

**Found beside the hypotheses:** at relaxed the log is almost never recycled
during the load (`wal_segments_removed` 0 in 13 of 15 runs, 2 and 4 in the
other two; 33-39 live segments at the last roll, against 20-22 removed and
5-6 live at group). A page re-dirtied after its writeback copy keeps its
first `rec_lsn` and pins the fold's redo start in segment 0, and the gate's
rotation stretches each core to one run every 25 s. Every sync's breadth
grows with it. Filed as
`docs/inflight/bugs/a-page-re-dirtied-after-its-writeback-copy-pins-the-redo-start.md`;
not BQ's subject, and outside its scope to fix. Fixed since on
`fix-redirtied-page-rec-lsn` (`wal.md` §11-2), and the entry deleted.

**Next (BQ-S2, rung 1, per BQ-Q6 (a)):** the driver's delta fix first
(`meta_delta` as m1 - m0 with an absent key read as 0, and each slow
sample's serving core recorded); then H4 option 3 (`BuildBody` prewrites
only where `Reserve` fell back to `ftruncate`, the body's `fsync` kept) and
H2 at 16, 256 and 1,024 MiB, each a measurement build on a fresh volume,
against `4cf30270`.

### BQ-S2 - rung 1 measured: H4 option 3 and H2 miss the bar

Measured on `worktree-bp-per-core-residue` (BQ joined BP's branch at
`6139c2de`) with measurement builds patched onto an export of `4cf30270`
(`~/bench-runs/bq/s2/variants.py`): H4 option 3 (no zero prewrite where
`posix_fallocate` reserved the extents; the body's `fsync` kept) and H2 at
16, 256 and 1,024 MiB, against base (`4cf30270`, 64 MiB). Relaxed C1 at
three placements, C2 and C3, every arm, three runs in per-arm blocks with
the arm order rotated per run, each cell gated (the gate missed other
sessions' builds in 7 cells, which the results file flags);
the mount probe (a clean `STOP`, the restart timed) twice per size. Results:
`bench/v3.0.0/results-bq-s2-rung1-v2.7.0-909-g4cf30270.md`. Every verdict
recomputed from the raw files and attacked by two independent checks.

**The procedure, decided while measuring** (recorded here, not marked): the
matrix ran relaxed for every arm before any group cell, and group only for
an arm meeting the relaxed bar - §5.3 needs both, so an arm that misses
relaxed misses the rung whatever group shows. No arm met it, so no group
cell ran. The client timeout was raised to 120 s after a 256 MiB cell's
client timed out at 5 s: at large sizes a single statement's stall is the
measurement, not a failure.

- **The bar - missed** in all 60 relaxed (arm, cell, run) results: no run of
  any arm is within 2x of its isolated p99 on cores with no bulk writer.
- **H4 option 3 - rejected as the fix, and the nearest miss.** It cuts the
  hold's median from about 325 ms to 16 ms - the preparer's 64 MiB zero
  fill no longer competes for the device (`[inferred]`, no device-level
  instrument ran), so the writer's sync the roll waits out falls from
  301 ms to 9.9 ms per roll at its median, the gate from 318 to 5.2 ms - and C3 gains
  43 % on average; it also lets the log recycle (12 of 15 runs, against 3
  of 15 at base). The tail does not fall: pooled hold p99 rose from 809 ms
  to 1.14 s, inside base's spread, and 13 % of rolls - a slow mode - carry
  80 % of the hold. Its own guard, C2's group commit p99, was not evaluated,
  and option 3 combined with a larger segment was never measured.
- **H2 - rejected.** The roll rate falls as 1/size, as predicted, but each
  roll's hold grows about 5 ms per MiB of segment, so the time held is flat
  across sizes and the p99.9 and maximum rise. H2's second reject clause
  has nothing to apply to, since no size meets the bar; as probe facts, at
  1,024 MiB the restart took 1.1-1.4 s longer and one `INSERT` stalled up to
  12.8 s while its roll waited on the preparer's build (28 of 30 large rolls
  waited, none built inline). The size stays a constant (BQ-Q4
  unexercised).

### BQ-S3 - rungs 2 and 3: recorded unable from BQ-S1's bounds (BQ-Q6 (a))

Not built. H5: with every roll-aligned stall removed, relaxed C1 reader p99
stays 2.6-145 ms against an isolated 0.2-0.6 ms. H3: with every non-tail
sync removed, the relaxed hold p99 stays 652-780 ms. Both from §7's BQ-S1
entry.

### BQ-S4 - rung 4: not run (BQ-Q5 (b))

H6 (the latch released for steps c-g) stays unmeasured and needs the
operator's own word. BQ closes on §0.4's row 4: nothing below rung 4 meets
the bar, the latch is kept, and its cost is named (BQ-S5).

### BQ-S5 - the close: texts landed, the overhead measured, one question open

On `worktree-bp-per-core-residue`. **Texts:** `wal.md` §4.1 (the segment
size a constant and a volume's property; a larger one makes rolls rarer and
longer, not cheaper), §6 step 1 (a roll's sync is device I/O under the latch
since `a1679272`, and the latched roll stands), §6 step 5 (priced by
BQ-S1), §13 (no "segment size" setting; the roll's `SHOW META` fields named);
`stream.hpp`'s and `log_device.hpp`'s comments; `known-gaps.md`'s two WAL
entries and its BL entry re-priced, with the open leads named (H4 option 3's
median cut, its unevaluated group guard and bimodal tail; rung 4's
license); AL-R1 annotated in `workorder-al-m0-single-wal.md`, not rewritten;
BL's §6 ruling 3 given BQ's answer. BQ-Q3 (a): the `wal_roll_*` counters
stay.

**The overhead A/B** (`bench/v3.0.0/results-bp-bq-close-v2.7.0-924-g3e77e7d1.md`): BQ's only
engine change is `4cf30270`'s instruments, against its opening `59c4139f`
(byte-identical to `cf889b63`). At `cores = 4` nothing moves outside noise.
At `cores = 1` the result is undetermined: the baseline itself read 5.4-5.6 %
higher in the sessions that paired it with `4cf30270` than in the others, so
the pair's -1.5 to -1.7 % on the every-row `UPDATE` and +3.2 to +3.5 %
against the other sessions' level bracket it.

**The open question** is the close's, shared with BP and stated once, in `workorder-bp-per-core-residue.md` §7's BP-S8 entry: a `cores = 1` every-row `UPDATE` runs 3-6 % above BP's last build before BQ joined in every build from the merge on, about half of it in `WalManager::Append`'s own frame, cause not found.

**Where BQ ends:** §0.4's row 4 - nothing below rung 4 met the bar, the
latch is kept with its cost named, and rung 4 needs the operator's own word.
Every stage has landed; the close waits at the push gate.
