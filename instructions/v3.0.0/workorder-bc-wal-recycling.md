# Work order BC — WAL recycling: segments wholly below the durable redo start are removed

Written 2026-10-07 on `worktree-wal-recycling` from `6dc792c9`
(`v2.7.0-627-g6dc792c9`), on the operator's
*"main을 동기화하고 먼저 wal 회수 기능에 대한 작업 지시서를 작성해줘"*.

**Not opened.** BC-Q0..Q6 (§4) are unmarked. No stage starts until the
operator marks BC-Q0, and each stage then waits for its own word. BC cuts no
tag.

**BC is not part of AR0 §8's chain.** It closes a gap AR0 never addressed:
`known-gaps.md`'s *"The log is never recycled"* (CN-9 §4 C1). It neither
depends on BA, which BB paused, nor blocks it.

## 0. What BC is

The log grows for the life of the instance. `wal.md` §11-4 makes a segment
wholly below the redo start recyclable *once archived*. Archiving is
`[PROPOSED]` and unbuilt (`wal.md` §13), so no segment ever qualifies. An
operator cannot remove segments by hand either: `FileLogDevice::Open` refuses
a numbering that does not start at 0 (§1.2). So the WAL device's capacity
is the instance's lifetime, and three other problems follow from that one:

- **C2** — every sync `fdatasync`s every segment ever created, so commit cost
  rises with the instance's age (`known-gaps.md`).
- **C3** — one descriptor per segment, never closed, until a roll meets
  `RLIMIT_NOFILE` and strands a segment
  (`bugs/wal-segment-descriptors-exhaust-the-open-file-limit.md`).
- **A full device** — `CreateSegment` fails, and every append after it fails.

**BC's premise, stated once because everything rests on it:** the durable
anchor's redo start `D` is a valid place to start recovery. Recovery already
starts its analysis there, and nothing BC adds makes that more or less true.
**A valid `D` stays valid**: every later record has a higher LSN, and the data
file only gains. BC-R1 and BC-R2 are both consequences of that sentence. BC-S1
tests it rather than assuming it (§1.4).

**BC's rules, in one line each:**

- **BC-R1.** A segment is removed only when it lies wholly below `D`, the
  redo start of an anchor **already durable on page 0**. The durable bound
  never decreases.
- **BC-R2.** Nothing reads the log below `D`. Redo starts at
  `max(analysis's recomputed start, D)`.
- **BC-R3.** The device's live segments are a contiguous run `[first, end)`.
  `Open` skips the segments below the anchor's segment and deletes nothing.
- **BC-R4.** A segment leaves the device's table under the locks that
  serialise every reader of that table, and its descriptor outlives every
  `Sync` that copied it.
- **BC-R5.** The unlink and the directory `fsync` run off every reactor.

**BC does not do:**

- **Archiving or PITR.** BC-Q1 decides whether recycling waits for them.
- **Segment reuse.** BC removes files and does not rename them into future
  segments. Reuse would take the prewrite out of the latched roll
  (`known-gaps.md`, C4), but that cost was never priced (BC-Q3).
- **The roll header's durability**
  (`bugs/a-power-loss-after-a-segment-roll-leaves-an-unheadered-tail.md`,
  C5). BC creates segments exactly as today, so it neither opens nor closes
  that bug. Reuse would make it BC's.
- **A new configuration key.** No retention knob, no segment-size knob
  (BC-Q4).

## 1. Survey at `6dc792c9`

Read, not run. Every line number is at `6dc792c9`. §1.4's first version was
wrong; BC-S0's review found it, and §6 records that.

### 1.1 Nothing ever removes a segment

- `LogDevice` has no removal call. Its contract says *"Segments
  0..segment_count()-1 exist. Recovery walks this range"*
  (`include/kds/wal/log_device.hpp:50-51`). `CreateSegment` requires
  `segment_no == segment_count()` (`:53-56`).
- `FileLogDevice` holds `std::vector<FileDescriptor> segments_`, indexed by
  segment number (`include/kds/wal/file_log_device.hpp:112-113`). The only
  removals are the cleanup of a failed creation
  (`src/wal/file_log_device.cpp:264`, `:274`, `:281`).
- `MemoryLogDevice` holds `base_` and `pending_` vectors indexed the same
  way (`include/kds/wal/memory_log_device.hpp`), with `segment_count()` as
  `base_.size()`.

**So the segment number is a vector index in both devices.** Removing
segment 0 means the device must carry a `first` number and translate. Every
`segment_count()` user moves with it: `ScanLog` (`src/wal/log_scanner.cpp:38`),
`WalStream::Open` (`src/wal/stream.cpp:48`, `:53`) and `WalStream::Roll`
(`:148`).

### 1.2 Open refuses a log that does not start at 0

`FileLogDevice::Open` collects this core's segments by number and requires
them numbered from 0 with no gap. Otherwise it returns `Corruption` naming
the first missing one (`src/wal/file_log_device.cpp:208-213`). BC-R3
replaces that check.

The mount reads the superblock **before** it opens the log device:
`BootstrapDatabase` (`src/server/expeditor.cpp:749`), then
`FileLogDevice::Open` (`:764`). So `Open` can be handed the anchor's segment
with no reordering of the mount. Recovery runs later (`:874`), and the
completion checkpoint publishes after it (`:1080`).

### 1.3 Who reads the log, and from where

| reader | start | site |
|---|---|---|
| analysis | the anchor's `redo_start_lsn` | `src/wal/analysis.cpp:221`, fed by `src/server/mount_recovery.cpp:25` |
| redo | **analysis's** `redo_start_lsn`, recomputed (§1.4) | `src/wal/redo.cpp:511` |
| assertion recovery | the anchor's `redo_start_lsn` on the server (`src/server/expeditor.cpp:1055`); `checkpoint_lsn`, which is higher, in the sim (`sim/instance.cpp:122`) | `src/exec/assertion_recover.cpp:316` |
| `WalStream::Open`'s resume | the last segment | `src/wal/stream.cpp:53` |
| tests | from 0 | `tests/wal_log_scanner_test.cpp` and others |

Nothing reads the log at runtime. Rollback reads undo pages, not the log.
The pre-AT prepare arm reads records at or above the redo start their writer
published (`wal.md` §3).

`ScanLog` resolves `from_lsn == 0` to segment 0
(`src/wal/log_scanner.cpp:57`). It refuses a start segment past the end
(`:60`), but nothing refuses one below a first segment, because today there
is none.

### 1.4 Redo routinely starts below the anchor, and that is not a fault

Analysis seeds its dirty-page table from **every** `CHECKPOINT_BEGIN` in the
scan (`src/wal/analysis.cpp:207-208`). It then recomputes the redo start as
the minimum recLSN over that table (`:257`), and redo scans from there
(`src/wal/redo.cpp:511`). Nothing floors that minimum at the scan start.

**On two or more cores, the recomputed start falls below the anchor in
ordinary operation.** One sequence:

1. Core A's checkpoint #1 (BEGIN at `b1`) records page P at recLSN `r` and
   flushes P. P stays clean.
2. Core B's checkpoint snapshots a hot page Q at recLSN `q`, with
   `r < q <= b1`. Q can keep an old recLSN, because writeback keeps it when
   a later write raced the copy (`wal.md` §11-2).
3. Core A's checkpoint #2 has a higher redo start, so the fold becomes `q`.
4. BEGIN `b1` lies inside the scan and seeds P at `r < q`. The recomputed
   start is `r`.

Today redo then reads `[r, q)` and changes nothing. P on disk already
carries those records, so `page_lsn` skips each one (`wal.md` §9). After BC
those records may be gone, and an unfloored redo refuses the mount.

**The floor is exactly as safe as recycling.** If `q` is a valid redo start,
redo from `q` is sound. If it is not, recycling below it is unsound whatever
redo does. So BC-R2 floors redo at `D`, and does not refuse.

**What would break the premise.** A record below `D` that the disk lacks.
The analysis that seeds P cannot distinguish that from step 4. Only a
comparison can: recover once from the unfloored start and once from the
floored one, and compare every page. That comparison is BC-S1.

### 1.5 The durable anchor is not the in-memory anchor, and the fold can step back

`SuperBlockCheckpointAnchor::Publish` folds, encodes page 0 under the
superblock latch, and then syncs the store outside it
(`src/server/superblock_checkpoint_anchor.cpp:86-126`). On a failed sync the
in-memory superblock **claims more than the platter does**, which its
comment calls *"the safe direction"* (`:131-136`). That holds only while the
log below the in-memory anchor still exists. So BC-R1 takes the value a
successful `store_.Sync()` made durable, not `superblock_.wal_anchors()`.

**The fold can decrease.** A page dirty with recLSN 0 at one core's snapshot
is skipped (`wal.md` §11-3). The same page can show its real, older recLSN at
the next core's snapshot. A lower fold written after segments were removed
would point the next mount at a missing segment. BC-R1 forbids the decrease
by construction: by the premise, writing `max(fold, D)` is as valid as either
value.

### 1.6 The device table is read unlocked on one thread and copied on another

- **`WriteAt` and `ReadAt` index `segments_` without `segments_mutex_`**
  (`src/wal/file_log_device.cpp:303`, `:313`, `:332`, `:342`).
  - Today this is safe because the one other mutator, `CreateSegment`, runs
    under the stream latch when the stream is shared, or on the same thread
    when it is not.
  - At `cores = 1` there is no latch at all (`src/wal/stream.cpp:17`).
  - So any table change from another thread races every flush.
- **`Sync` copies the descriptors as raw `int`s** under `segments_mutex_`,
  releases it, and then `fdatasync`s each one
  (`src/wal/file_log_device.cpp:386-405`).
  - It is called from core 0's reactor inline (`src/wal/manager.cpp:233`)
    and from the writer thread (`src/wal/writer.cpp:84`).
  - A descriptor closed under such a copy makes that `Sync` use a closed or
    reused number. A closed number or a socket fails the sync, and a failed
    sync refuses the commits waiting on it. A reused regular file is synced
    harmlessly.

BC-R4 covers both.

### 1.7 The writer thread is on every peer's commit path

A peer's durable commit waits on `writer_->EnsureDurable`
(`src/wal/manager.cpp:202`). So an unlink and a directory `fsync` placed on
the writer thread stall every peer commit that arrives during them. Core 0's
own commits sync inline and do not wait for them. That is the cost BC-Q5
weighs.

### 1.8 What already records the bound

The on-disk anchor slot `WalAnchorFields` already carries `segment_no`,
*"segment holding redo_start_lsn"* (`include/kds/server/superblock.hpp:382`),
copied from `CheckpointAnchorRecord::segment_no`
(`include/kds/wal/checkpointer.hpp:196`), which is computed as
`pending_redo_start_ / segment_size` (`src/wal/checkpointer.cpp:277`). So
**BC needs no superblock change and no format-version event**: the bound is
on page 0 already, and the segment and record formats are untouched.

A volume whose log starts above segment 0 does not mount on a pre-BC build,
which refuses the gap (§1.2). There is no compatibility promise
(`wal.md` §4.1), and the refusal is loud.

### 1.9 When the anchor never advances

- **`checkpoint_interval_ms = 0`** arms no cadence tick, on core 0
  (`src/server/expeditor.cpp:1882`) or on a peer
  (`src/server/core_runtime.cpp:541`). The anchor then moves only when some
  other path completes a checkpoint, the mount's completion checkpoint among
  them. Recycling follows the anchor.
- **The warm-up** holds the anchor at the mount's until every core has
  published once (`superblock_checkpoint_anchor.cpp:44-46`).
- **An anchor of 0** means "never checkpointed", and recovery replays from
  segment 0 (`superblock_checkpoint_anchor.cpp:9-24`). Nothing is recycled
  under it.

None of these is a BC defect. Each is the anchor's own semantics, and BC-S5
states them in `wal.md` §11.

### 1.10 What can test it

- **The sim is one core.** `SimInstance` checkpoints as core 0 alone
  (`sim/instance.cpp:130-136`) and never starts a `WalWriter`. So §1.4's
  sequence never happens there, and a writer-thread trigger is never
  reached.
- **The two-core rig** (`tests/two_core_rig.hpp`,
  `workorder-av-two-core-rig.md`) runs two `CoreRuntime`s, and
  `tests/insert_log_crash_rig_test.cpp` already crashes and recovers on it.
  §1.4's sequence is built there.

### 1.11 A reading for BC-S1, outside BC's rules

Found by BC-S0's review, not reproduced. When analysis meets a page record,
it calls `dirty_pages.emplace(page_id, lsn)` (`src/wal/analysis.cpp:131`).
`emplace` does not overwrite an entry seeded from a `CHECKPOINT_BEGIN` with
recLSN 0.

So a page seeded at 0, whose next record `r` follows the BEGIN, stays at 0
and is skipped by `RedoStartFrom`. If no other entry pulls the start to `r`
or below, redo starts past `r`, and the page on disk can lack that record.
That would be a lost committed change, which is a quiet wrong answer.

Whether a dirty frame can carry recLSN 0 at a snapshot while a record for it
is still to come is not established here. BC-S1 writes the cell. A red cell
becomes a bug entry under its own letter. It is outside BC because BC
neither causes it nor depends on it.

## 2. Rulings — CLA's proposals, unmarked

### BC-R1 — The bound is the durable anchor, and it never decreases

- **Removable.** A segment `s` is removable when
  `(s + 1) * segment_size <= D`.
- **What `D` is.** `D` is the `redo_start_lsn` of the last fold whose
  `store_.Sync()` returned OK.
  - `Publish` sets `D` after that sync, as a maximum over the values **it
    encoded**. It does not take the current in-memory value, which another
    core may have raised after the encode.
  - `D = 0` removes nothing (§1.9).
- **Never decreasing.** `Publish` encodes `max(fold, D)` (§1.5).
- **One computation.** If BC-Q1 is marked (b) or (c), the bound becomes
  `min(D, archived)`, and this is the one place that computes it.

### BC-R2 — Nothing reads below the anchor

- **Redo.** Redo starts at `max(analysis.redo_start_lsn, scan_start_lsn)`.
  `SHOW META`'s recovery block reports both numbers whenever they differ, so
  the floor is visible rather than silent.
- **`ScanLog`.** It refuses a start below the device's first segment with
  `Corruption` (*"recycled"*). This is a backstop that the floor keeps
  unreachable. `from_lsn == 0` resolves to the first segment only when that
  segment is 0; otherwise it gets the same refusal.

### BC-R3 — The device's live run

- **The interface.**
  - `LogDevice` gains `first_segment()` and `RemoveBelow(segment_no)`.
  - `segment_count()` becomes `end_segment()`, one past the last.
  - Every caller in §1.1 and §1.3 moves to the pair. `CreateSegment` takes
    `end_segment()`.
  - `RemoveBelow` refuses a bound above the segment holding the stream's
    append point. A bound equal to it is legal and removes everything
    before it.
- **`Open(dir, core_id, segment_size, first_needed)`.**
  - `first_needed` is the mount anchor's `segment_no`.
  - Segments below `first_needed` are **not opened and not deleted**. Gaps
    among them are tolerated: they are leftovers of a removal a crash
    interrupted, and a revived subset is possible because POSIX orders no
    two unlinks before a directory `fsync`. `Open` keeps their numbers.
  - Every segment from `first_needed` to the highest present must exist.
    A gap there is `Corruption`, as today.
  - `Corruption` also covers two more cases, because then the anchor names
    a log that is not there: the highest present segment is below
    `first_needed`, or no segment is present while `first_needed > 0`.
- **No deletion at mount.** A mount refused later must leave the lower log
  where it was. The completion checkpoint's publish (§1.2) reaches
  `RemoveBelow`, which unlinks the leftovers by number with the rest.
- **The file device** keeps one directory descriptor, opened in `Open`. Every
  `RemoveBelow` `fsync`s the directory once. This is the C3 bug file's
  *"always, and local"* fix: a roll that meets the descriptor limit then
  fails before it creates a file, and strands nothing.
- **`MemoryLogDevice`** models the same crash: a removal whose directory
  sync did not complete revives any subset of the removed segments.

### BC-R4 — Table changes and descriptor lifetime

- **The table changes under the stream latch.** A segment leaves the table,
  and `first` moves, under the stream latch and `segments_mutex_` both. That
  serialises the change against `WriteAt`, `ReadAt` and `CreateSegment` (§1.6)
  and against `Sync`'s copy.
- **At `cores = 1`, where there is no latch,** the change runs on the
  reactor thread, which is the only thread that calls `WriteAt`.
- **A `Sync` copy holds shared ownership** (for example,
  `std::shared_ptr<FileDescriptor>`). The table drops its reference, and the
  file closes when the last `Sync` that copied it returns. Syncing a removed
  segment is harmless: it lies below a durable anchor and is already durable.
- **The cost this moves.** The filesystem frees a removed file's blocks at
  the last `close`. When the reactor's inline `Sync` (§1.6) holds the last
  reference, the reactor pays to free one segment. BC-S5's measurement
  reports whether that happened.

### BC-R5 — Where the I/O runs

- **The trigger.** `Publish` detaches the removable segments (BC-R4) and
  queues their paths and descriptors. That does no I/O on the caller's
  thread.
- **The I/O.** The unlink and the directory `fsync` run off every reactor,
  on the thread BC-Q5 picks.
- **The sim** has no writer thread, so there the I/O runs inline after the
  publish, as `WalManager::Sync` falls back when no writer exists.

### BC-R6 — What a removal costs, and what it is shown as

- **`SHOW META`** gains three fields on core 0: `wal_first_segment=`,
  `wal_segments_removed=` and `wal_remove_failures=`. A peer prints none,
  following the `wal_syncs`/peer precedent (`wal.md` §3).
- **A failed unlink is retried at the next advance.** It refuses nothing:
  the log is only larger than it needs to be, and the counter says so.

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BC-S0 | **The order** | this file, its review, the marks recorded | S |
| BC-S1 | **The premise, tested before anything is built** (§1.4, §1.11) | <ul><li>**The comparison cell, on the two-core rig.** Build §1.4's sequence, crash, and recover twice from copies of one image: once from analysis's start and once from `max(start, D)`. Every page must be byte-identical. Assert that the sequence actually drove the start below `D`, so the cell cannot pass vacuously.</li><li>**The same comparison in a randomized loop** on the rig, at least 3 checkpoints and 2 segment rolls per run (small segments), with the run count stated in §6.</li><li>**§1.11's cell.** Red becomes a bug entry, not a BC stage.</li><li>**A page that differs stops BC at this row**, and the operator rules.</li></ul> | M |
| BC-S2 | **The device** (BC-R3, BC-R4) | <ul><li>**Code.** `first_segment`/`end_segment`/`RemoveBelow` in both devices, `Open`'s new rule, the kept directory descriptor, `Sync`'s shared descriptors, and `ScanLog` and `WalStream` moved to the pair.</li><li>**Cells.**<ul><li>The `Open` matrix: a revived subset below `first_needed`, a gap above it, nothing present, and the highest below `first_needed`.</li><li>A `RemoveBelow` racing a `Sync` and a `WriteAt` on separate threads, repeated with a barrier. The assertion is that no descriptor is closed while a `Sync` holds it, checked by the device's own count.</li><li>The descriptor-limit roll strands no file.</li></ul></li><li>**Mutations, each repeated:** `Sync` copying raw ints again; the table changed outside the stream latch; `Open` deleting below `first_needed`; `RemoveBelow` reaching past the append segment.</li><li>**The suite green.**</li></ul> | M |
| BC-S3 | **The trigger and the floors** (BC-R1, BC-R2, BC-R5) | <ul><li>**Code.** `D` and `max(fold, D)` in `Publish`, the queued removal, redo's floor, the `SHOW META` fields.</li><li>**Cells.**<ul><li>A failed anchor sync leaves every segment in place.</li><li>A successful publish removes exactly the segments wholly below `D`.</li><li>A fold lower than `D` is encoded as `D`.</li><li>On the rig, §1.4's sequence followed by recycling mounts, with the floor reported.</li><li>The leftovers below `first_needed` are removed by the completion checkpoint, not by `Open`.</li></ul></li><li>**Mutations, each repeated:** the bound read from the in-memory superblock, killed by the failed-sync cell; the whole-segment test dropped; the `max(fold, D)` dropped; redo's floor dropped, killed by the rig cell.</li><li>**The suite green.**</li></ul> | M |
| BC-S4 | **The sim** | <ul><li>`sim/` drives long runs over small segments with recycling armed, on BC-R5's inline path. It crashes at seed-chosen operations, including **between an unlink and its directory sync**, and reconciles against the oracle.</li><li>The corpus green, with the run counts stated in §6.</li></ul> | M |
| BC-S5 | **The close** | <ul><li>**The measurement** (§5).</li><li>**Text restated:**<ul><li>`wal.md`: §2; §4.1 (*"named by (core_id, segment_no)"*); §9 and §12, for redo's floor; §11-4, on BC-Q1's mark; §13, where *"retention"* is listed as configuration and BC-Q4 says there is none; and §1.9's three cases.</li><li>The contracts in `log_device.hpp` and `file_log_device.hpp`.</li><li>`manual/`, if `SHOW META` is documented there.</li></ul></li><li>**`docs/inflight/`:**<ul><li>C1's entry is deleted.</li><li>C2's entry is restated: the cost is bounded by the live run, no longer by the instance's age.</li><li>C3's bug file is deleted: its cause and its stranding are both closed.</li><li>CN-9 §9 O4 records the mark.</li></ul></li><li>**`CLAUDE.md`'s WAL row** flipped.</li></ul> | S |

## 4. Items for the operator

| item | question | kind | CLA's proposal | mark |
|---|---|---|---|---|
| BC-Q0 | **Open BC**, with BC-S0..S5 and BC-R1..R6 as written | process | Yes | unmarked |
| BC-Q1 | **Does recycling wait for archiving?** (CN-9 §9 O4).<br>(a) Recycle below the durable anchor now. Archiving, when built, adds its floor at BC-R1's one computation.<br>(b) Recycling waits for `wal.md` §13's archive hook, and BC waits for that work order.<br>(c) Recycle now behind a per-instance switch, off by default | durability | (a). PITR does not exist, so (a) gives up nothing the engine offers. (b) leaves C1-C3 standing for as long as no one orders archiving. (c) adds a configuration key for a choice that has one right answer today | unmarked |
| BC-Q2 | **Redo is floored at the anchor** (BC-R2). Recovery changes behaviour: it no longer reads records below the anchor, which §1.4 shows it reads today and changes nothing with | recovery semantics, **[quiet-wrong] if BC-S1's comparison is skipped** | Yes, conditional on BC-S1's comparison being green | unmarked |
| BC-Q3 | **Segment reuse** (rename a removed segment into a spare, so a roll skips the prewrite).<br>(a) Out of BC; C4 priced first, under its own order.<br>(b) In BC as a stage after BC-S4, taking C5's fix with it | scope | (a). C4 was never priced, and `CLAUDE.md` says re-measure the premise before building the fix | unmarked |
| BC-Q4 | **No retention knob.** Every segment wholly below the bound is removed, and no extra is kept | user-visible | Yes. A retention count would be a second name for what the checkpoint cadence already expresses, as an RTO trade | unmarked |
| BC-Q5 | **Which thread does the unlink** (BC-R5, §1.7).<br>(a) Core 0's WAL writer thread, between syncs.<br>(b) A thread of its own, under `rules.md` §3's partition-boundary justification | design, user-visible as peer commit latency | (a), measured at the close at `cores > 1`. If the stall is material, (b) follows, with no other change to BC. (a) adds no thread, and its cost falls only on peer commits that arrive during an unlink | unmarked |
| BC-Q6 | **`SHOW META`'s three fields** (BC-R6) | user-visible | Yes | unmarked |

## 5. Measurement

Once, at BC's close, per `CLAUDE.md`'s Session Workflow step 3:

- **What.** An interleaved A/B in `build-release` (`ck-tester`), comparing
  the commit BC opened from with the commit that closes it.
- **Two shapes**, each `durability = strict` updates over a log long enough
  that C2 bites at A. That means at least 64 segments of history, at the
  default size, or at a smaller size if the host's disk cannot hold it.
  The results file states which.
  - `cores = 1`.
  - `cores = 4`, with commits on peers. This is the BC-Q5 shape.
- **Reported:**
  - commit latency percentiles, p0 through p99.9, per shape;
  - `wal_syncs`;
  - B's `wal_segments_removed`;
  - each side's open descriptor count;
  - whether a reactor ever paid a last `close` (BC-R4).
- **The expected sign.** B's commit p50 falls with the length of the live
  run. That is a prediction, not a claim, until it is measured. A peer p99
  regression from the unlink is a regression, and it stops the merge or
  moves BC-Q5 to (b).
- **Where.** `bench/v3.0.0/`, named by `git describe --tags`.

## 6. Row status

### BC-S0 — written 2026-10-07, reviewed the same day

Written on `worktree-wal-recycling` from `6dc792c9`. The `critics-developer`
review read the draft against `6dc792c9`.

**Taken:**

- **The draft's §1.4 was wrong.** It argued that no seeded recLSN can lie
  below the fold, and it missed the page that is cleaned and stays clean.
  The draft's refusal would have refused sound mounts on two or more cores.
  It is replaced by the floor, BC-R2, on the premise stated in §0.
- **The draft's §1.5 relied on that argument.** The fold can step back, so
  `max(fold, D)` is now a construction rather than a tested reading.
- **The draft tested on the sim, which is one core with no writer**
  (§1.10). The tests moved to the two-core rig, and BC-R5 gained an inline
  path for the sim.
- **The unlocked `WriteAt`/`ReadAt` table reads** (§1.6). BC-R4 now covers
  every reader of the table, not only `Sync`.
- **`Open` deleted files before recovery succeeded.** It now deletes
  nothing.
- **The writer thread is on peers' commit path** (§1.7). This became
  BC-Q5, with the `cores = 4` shape in §5.
- **The last `close` moves to whoever drops the last reference.** This is
  stated in BC-R4 and measured in §5.
- **Survey omissions.** `Roll`'s `segment_count()`; the sim's assertion
  start; `wal.md` §13's *"retention"*; core 0's cadence at
  `expeditor.cpp:1882`; the `WalAnchorFields` and `CheckpointAnchorRecord`
  citations; an off-by-one in `RemoveBelow`'s bound; the
  `git describe` string.
- **Questions that were not operator decisions** were moved into the
  rulings: the directory descriptor (BC-R3), the descriptor ownership
  (BC-R4) and *"S1 runs first"* (now the stage order).
- **§1.11's reading** was recorded, outside BC's rules.

**Not taken:** none.
