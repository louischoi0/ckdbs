| AL-S8 | **Measured 2026-09-03** at `f6ed10c` (`v2.7.0-157-gf6ed10c`), from a hashed binary copy, on the 8-core AMD EPYC 9V74 with data on ext4 — all eleven cells ran, none blocked. Three files under `bench/v3.0.0/`, raw JSON and `SHOW META` dumps beside them. **First, the instrument the stage had none of**: `WalStats::ring_full_drains` had been counted since the first drain path and never printed, so surfacing it was a prerequisite rather than part of the measurement — as **two** fields, since the drain loop hides two events (a stall paid and got through, versus an append refused `OutOfSpace` after exhausting `kRingDrainAttempts`, a counter that did not exist because AL-R1's bound is what made the case reachable). **The headline is about the matrix, not the engine**: neither scenario driver issues `CREATE NAMESPACE`, so under the shipped `placement = namespace` every relation is owned by its creating core — core 0 — and **the eight-cell matrix at `cores = 8` never exercises a peer's WAL append at all**; it prices statement shipping, where scenario 0 gains 7.6% and scenario 2 **loses 46-48%**, the shipping hop compounding across an eight-statement transaction held open over every round trip. The M0 cells therefore used `placement = rotate` to get relations genuinely onto peers, and found three things: a peer's commit tail under `group` is **indistinguishable from core 0's** at both p50 and p99 on this host, where `manager.cpp`'s `Sync()` comment records a doubled p99 from a 2-core one; the `fdatasync` cost is **not** lost as unattributed reactor gap but sits inside the waiting peer's own `foreground` polled time, which group batching cuts ~20-23x per commit; and both ring counters read **0 on every core in both durability classes** at up to 16 writers, which the file states as characterizing nothing about the ring's limit rather than as a clean bill of health. **Suite: 3253/3253** (`ctest -j8`, Debug) |
| AL-S8 review | **In flight 2026-09-03** — a `critics-developer` pass over the three results files, recomputing every derived figure against the archived raw data and checking the two claims that would outrank the rest if wrong: that the scenario-2 `--verify` failures are a pre-existing driver race rather than an engine lost update, and that the placement finding holds against `core_placement.hpp`. Its findings land in a follow-up row |
# Work order AL — AR0 M0: the single WAL stream

Drafted 2026-09-02 by CLA on `worktree-v3.0.0-arch-revision` at `d15b5ac`
(`v2.7.0-134-gd15b5ac`). Enacts AR0 §8 step 3 under the operator's
go-ahead of the same day (AL-1). Governed by
`instructions/v3.0.0/ar0-architecture-revision.md` (the body for the
direction, AR0-V for what the tree says), `docs/spec/wal.md`,
`docs/spec/page-lsn-cross-stream.md`, `docs/spec/cross-owner-txn.md` §2c,
`docs/spec/sched.md` §5/§7, `docs/rules/rules.md` §3. "AR0 M0" is cited
with the file, never as a bare "M0": `workplan-crosscore.md` at `1769487`
already numbers its milestones M1–M6.

## AL-1 — The direction, verbatim

> set first milestone go ahead for it for an hour task

Read as: AR0's first milestone is set; work on it starts now; the first
task is sized to one hour. The hour is AL-S0 — this document, the
source-read survey it carries, and the two lines that reopen
`instructions/`. Nothing in the engine changes at AL-S0.

## AL-2 — Where this sits against AR0

**What AR0 M0 is** (§8 step 3): one WAL stream for the instance, **one
flush and sync point** — every core still appends, which is where AL-R1
departs from D3(a) — the `cores = 1` path unchanged, and a fresh
baseline. **What it is not**: M0 keeps relation ownership, per-core
buffer pools, the ring, the cross-owner protocol, per-core undo chains
and the trx-id leases. M0 changes exactly one quantity — how many
streams there are — and everything the survey (AL-3) lists follows from
that one change.

**D-items M0 consumes**, taken as ratified for M0's scope by the
go-ahead and by AR0 §5's standing rule (CLA's proposal stands unless the
operator says otherwise; constants are never CLA's):

| D | how M0 takes it |
|---|---|
| D3 (log appender) | the *mechanism* is AL-R1, a proposal that differs from D3(a)'s letter for the reason AR0-V4 gives; the *constant* (the group-commit cadence) stays `wal_drain_interval_ns` at its current 1 ms and is re-measured at AL-S8 |
| D4 (free-map / superblock authority) | untouched by M0: `crosscore.md` CC11 stands — core 0 alone writes the superblock, the free map and the catalog, and core 0 is also where the one log's drain runs (AL-R1). "The log core" of D4 *is* core 0 in M0 |
| D14 (format) | **Not consumed after all** — AL-R3's draft proposed superblock format 15 → 16, and AL-S2 amended it: `log_topology` took the former `reserved1` word, which every existing image already holds as 0 = `kPerCoreStreams`, so M0 is a **format-event-free** change and a pre-M0 volume still mounts. D14's "a new major version mounts only volumes created by it" is therefore **unspent**, and AM-R4 is where it gets paid |
| D15 (baseline) | AL-R8, **ratified by CLA 2026-09-03**: `bench/v3.0.0/`, a fresh series, no delta to any v2.x number |

**D-items M0 does not touch**: D1, D2, D5–D13, D16 beyond the citation
rule above. The page header keeps its bytes (AR0-V3).

**What the verification changed about M0's case.** AR0 §0 argued for
the single log from scheduler latency; AR0-V1 shows the tree attributes
the numbers to device syncs. The case for M0 is therefore the one
`bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md` §8b makes
at `1769487`: 94–98% of a writing reactor's wall time is charged to no
group — "the WAL drain's `fdatasync` above all" — because a group-commit
sync runs **on the reactor thread** and only core 0 starts a writer thread
(`src/server/expeditor.cpp:800-812`; `core_runtime.cpp:113-123` starts
none). One stream with one off-reactor sync point removes that from
every core. The counter-measurement M0 owes: `src/wal/manager.cpp`'s
`Sync()` comment records that handing a waited-on sync to another thread
"doubled `group`'s p99 while barely moving its median" on a 2-core host
— under one stream every core but the drain's pays that wake-up, and
AL-S8 measures whether the tail moves.

## AL-3 — The survey: every per-core-stream assumption at `d15b5ac`

Source-read by CLA on 2026-09-02. One line per site; the stage that
retires each is named in AL-5.

**A. Stream identity.** There is no `stream_id`; the stream's identity
is `core_id` everywhere. `record.hpp:366-387` persists `core_id` in the
segment header (`kSegmentCoreIdOffset = 12`), `record.cpp:215,258`
codes it. `file_log_device.hpp:15,47-53` names segments
`wal-<core_id>-<segment_no>.log` and adopts only that core's;
`file_log_device.cpp:48-49,150,179` builds and filters by the prefix.
`stream.hpp:12-17,89-95,154,181` is "one core's WAL stream", refusing a
header naming another core (`stream.cpp:67,95`). `manager.hpp:155,164`
opens one manager per core (`manager.cpp:44-46`). `log_scanner.hpp:62-71,121,128`
and `log_scanner.cpp:20-23,115,200-204` validate every segment against
a core. The record header (`record.hpp:256-276`) carries **no** core id.

**B. Superblock.** `superblock.hpp:212,315-338,341,343-360,363-372,377,466-503`:
the per-core anchor table indexed by `core_id`, `kMaxWalCores = 64`,
`wal_anchor_count`, `CheckCoreCount`, `wal_anchor(core_id)`,
`SetWalAnchor`; `superblock.cpp:18-21,93,103-105,151-153,163-190` code
and validate it. `bootstrap.cpp:19,68-78,112` pins `cores` and refuses
a mismatch naming both numbers; `bootstrap.hpp:53-63` and
`expeditor.hpp:523-545` state why. `command_dispatcher.cpp:1476` prints
`wal_anchor_count=`.

**C. Recovery.** `recovery.hpp:59-62,91-108,210`: "recovery is this
function once per core", `RecoverCore(device, core_id, …)`;
`recovery.cpp:33-42,56,147` and every message scoped "recovery of core
N". `analysis.hpp:14,73-77,115-121,141-147,159-162,200-201,237` and
`analysis.cpp:43,226,240`: one forward scan of a core's stream, the
prepared set keyed by this stream, `PAGE_HANDOFF` removing a page from
this stream's scope. `redo.hpp:91`. `mount_recovery.hpp:29-58,186-212,265-286,331`
and `mount_recovery.cpp:16-21,35-40`: `RecoverCoreAtMount(core_id,
anchor, …, wal_dir, anchors)`, the `CoordinatorStreamResolver` per
participant, a completion checkpoint per core. `expeditor.cpp:840-843,863-869,1001-1003,1027`
recovers core 0's stream; `core_runtime.cpp:175-177,420-421,824-825`
each peer's. `checkpointer.hpp:179-192`, `checkpointer.cpp:240,256`:
the anchor record carries `core_id`; `remote_checkpoint_anchor.{hpp,cpp}`
and `expeditor.cpp:1451-1465` ship a peer's anchor to core 0.
`assertion_recover.hpp:104`, `assertion_recover.cpp:123,285` scan one
core's stream.

**D. The PL-C stamp and the PL-B handoff.** `page_header.hpp:139-159`
(`GetPageStreamStamp`, `SetPageStreamStamp`, `StreamStampFor(core_id)
= core_id + 1`, `StampIsForeign`), `page_header.cpp:138-143`.
`redo.cpp:359-373` refuses a foreign stamp inside this stream's scope
as `Corruption`; `redo.cpp:478-479` **restamps every applied page** with
`StreamStampFor(core_id)`. `device_page_store.cpp:702-725` claims a
page at fault iff the stamp names this core (PW1c-7); `:1056-1062`
stamps beside `page_lsn` in `StampPageLsn`, on the mutation path;
`:417,440,448,482-486` refuse by stamp;
`device_page_store.hpp:290-308,341-346,376-378,903-923` hold
`core_id_` and `stamp_claims_`. `core_runtime.cpp:160-166,931-932`.
Handoff: `record.hpp:161-175` (`kPageHandoff = 25`),
`payload.hpp:84-104` (`incoming_core`), `log_page_handoff.hpp:11-41`
the one emitter, `analysis.cpp:91-134`, `redo.cpp:43-48`,
`core_runtime.cpp:935,962-992`, `relation_grant_service.hpp:24-30`,
`ring_message.hpp:96`. Spec: `page.md:34,108`.

**E. Cross-owner records.** `payload.hpp:128-163`: `TxnPreparePayload
{coordinator_session_id, coordinator_txn_id, coordinator_core}`.
`txn_2pc_service.hpp:20-26,41-53,66-95,102-152`: the verdict "lives in
exactly one stream", the coordinator's whole stream scanned, another
core's device opened at mount. `prepared_resolver.cpp:15-147`: groups
by `coordinator_core`, opens `FileLogDevice(wal_dir_, coordinator_core)`,
full scan from LSN 0, must reach the coordinator's anchor.
`cross-owner-txn.md:229-262`: the file read, the prepared floor, the
retention obligation.

**F. Who syncs.** `writer.hpp:13-55`: the writer thread is the one
boundary lock, one device per writer, and **stream-agnostic**
(`:45`, "this class never touches the stream"). `manager.hpp:184-219,246-249,282-291,311-315`;
`manager.cpp:75-77` (`StartWriter`), `:232-267` (`DrainOnce` hands the
tick to the writer if present, else syncs inline). `expeditor.cpp:789-812`:
core 0 opens its device and manager and **alone** calls `StartWriter()`;
`:2009-2050` core 0's drain in the post-task hook and on
`wal_drain_interval_ns`. `core_runtime.cpp:113-123,1078-1128`: each peer
opens its own device and manager, no writer, drains inline on the same
cadence. `sim/instance.cpp:36` opens one manager at core 0, no writer.

**G. `cores = 1`.** No WAL-specific branch exists. The peer loop
`expeditor.cpp:1492` never runs at 1, the ring matrix is skipped at
`:1399-1401`, `core_runtime.cpp:788` skips the remote anchor for core
0. At `cores = 1` the only WAL objects are core 0's, `wal_anchor(0)` is
the only live slot, `StreamStampFor(0) == 1` the only stamp written,
and no `PAGE_HANDOFF` or `TXN_PREPARE` is ever emitted. **A single-stream
design keeps this path byte-identical iff it preserves `wal-0-N.log`,
segment-header `core_id = 0`, and stamp value 1** — AL-R2/AL-R6.

**H. Counters.** `manager.hpp:104-146`: `WalStats` per manager, so per
core. `command_dispatcher.cpp:1483,1519-1551,1583-1593,2034-2097`:
`core=`, `wal_syncs`/`wal_interval_syncs` core-local with the
writer-on-core-0 comment, the group-commit block, the per-core recovery
block; `client-manual.md:344-345`, `cross-owner-txn.md:391,430`.

**I. Tests and the simulator.** Tests whose bodies name a core, a stream,
an anchor or a stamp: `wal_stream_test` 23/23, `wal_analysis_test`
19/21, `wal_redo_test` 14/21, `prepared_recovery_test` 15/16 (a fixture
of **two** streams and two anchors, `:161,191-198,337,355`),
`mount_recovery_test` 12/16, `wal_log_scanner_test` 13/14,
`superblock_test` 13/16, `wal_recovery_test` 10/10,
`file_log_device_test` 10/15 (literal `wal-0-*.log`),
`wal_checkpointer_test` 9/17, `core_runtime_test` 46/131,
`device_page_store_test` 12/48, `superblock_checkpoint_anchor_test`
8/8, `txn_2pc_protocol_test` 9/53, `wal_record_test` 7/13,
`assertion_recover_test` 6/13, `sim_loop_test` 6/45,
`wal_high_water_test` 5/10, `bootstrap_test` 4/12, `insert_wal_test`
3/33, `wal_payload_test` 3/41, `page_header_test` 1/19,
`command_dispatcher_test:140` (`wal_anchor_count=0`). Core-agnostic
today: `wal_manager_test`, `wal_writer_test`, `recovery_undo_test`,
`high_water.{hpp,cpp}`. **The simulator already models one log**:
`sim/instance.{hpp,cpp}` holds a single `MemoryLogDevice`, recovers at
core 0, injects faults against the one device (`sim/faults.*`,
`sim/loop.cpp:472,618,625`).

**J. Spec sentences that state the topology as a rule.** `wal.md:7,35,42-49,127,131,178,192,196,214`;
`page.md:34,104-108,196`; `sched.md:9,58,132-137,154,186`;
`crosscore.md:37` (CC11: "no DDL can span two WAL streams");
`cross-owner-txn.md:139,229-262`; `rules.md:23,25,51`;
`page-lsn-cross-stream.md:25-33,47,96-100,226-232` — **§6 declines PL-A,
"one global LSN", because it puts a shared atomic on the append path;
PL-A is what M0 builds, and that cost is the one AR0-2 accepts and
AL-S8 prices**; `CLAUDE.md:45`; `manual/server/server.md:53,77,78,156`.

## AL-4 — Rulings AL-R1..AL-R8

**AL-R1 — The append: reserve, copy, publish; one writer thread; the
drain on core 0.** *CLA's proposal, in place of D3(a)'s letter.* D3(a)
fans every append through the ring to a log core. The ring carries POD
messages (a 32-byte header, `ring_message.hpp:354`) into a slot whose
payload is `kCoreRingPayloadBytes = 1024` bytes
(`ring_transport.hpp:224`; the text arm's cap is 992 of them,
`crosscore.md` §4a), and a `FULL_PAGE_IMAGE` is 8 KiB, so D3(a) as
written cannot carry the record every checkpoint cycle's first touch of
a page logs. It would also put a
ring hop and a wake on every record, which is the cost D2 rejects per
lock. What M0 builds instead:

- **A latch around the append, not a lock-free reservation.** *Amended
  2026-09-02 from building AL-S1a; the first draft of this ruling said
  `fetch_add` on the staging cursor, and it does not survive the code.*
  `WalStream::Append` is not only a memcpy and a cursor bump: before the
  bump it refuses an oversized record (`stream.cpp:186-193`), and it
  **seals and rolls the segment** when the record does not fit
  (`:195-202`), which does device I/O and moves `append_lsn_`; after it,
  a full buffer returns `OutOfSpace` (`:204-209`). A bare `fetch_add`
  can express none of the three — the roll is not reservable and a
  refused reservation would have to be unwound. So the shared stream
  takes **one latch** (`base/latch.hpp`) across the size checks, the
  roll, the encode and the bump, and the LSN is fixed under it. `StampPageLsn` and the frame's `recLSN` discipline (`page.md` §8)
  are unchanged, the LSN still being known when `Append` returns.
  **At `cores = 1` the stream is opened unshared and the latch is a null
  pointer**: one branch per guard, which is `sched.md` §5's accepted
  cost class ("phase 3 costs one null test").
- **What the latch covers, and why it is a mutex.** The flush (staging
  buffer → segment) happens **under** the latch, and so does the roll's
  `CreateSegment` — which is a `posix_fallocate`, a full-segment
  prewrite and two `fsync`s (`file_log_device.cpp:244-299`), not the
  nanoseconds a spin is for. Against a holder inside `fsync`,
  `sched_yield` returns immediately when nothing else is runnable on
  that CPU, so N−1 pinned reactors would burn a core each for the length
  of a segment creation; a futex sleep is the right wait and costs the
  same single atomic operation uncontended. The device **sync** is
  outside the latch. AL-S8 prices what remains; staging into a second
  buffer and writing it unlatched is the follow-on if it shows.
- One `WalWriter` for the instance, started by the expeditor
  (`writer.hpp:45` already promises it never touches the stream). The
  **flush** and the **drain** run on each core's own reactor tick as
  they do today (`expeditor.cpp:2009-2050`, `core_runtime.cpp:1078-1096`),
  but a peer's **sync** becomes a request to the writer. A peer's
  committer parks on `IsDurable(lsn)`, which after M0 reads the writer's
  atomic watermark rather than its own stream's plain field — new for a
  peer, which owns no writer today. A peer no longer owns a device.
- **Which of a peer's calls block, and why the distinction is
  load-bearing** (AL-S1b). `EnsureDurable`, `SyncAll` and a D1 commit
  wait on the writer's watermark: the first is the WAL-before-data gate
  the page store calls before writing a dirty page (§8-1), so an OK
  meaning "asked for" would let a data page overtake its log record —
  a durability defect, not a latency one. The **drain** does not wait:
  it runs once a tick on the reactor and blocking it would hold every
  session on that core for another thread's `fdatasync`; it asks, and a
  later tick closes the batch.
- The staging buffer's capacity (`kDefaultRingCapacity`, 1 MiB) is now
  shared by every core; a full buffer fails the append `OutOfSpace` as
  today (`stream.hpp`, "Backpressure is a Status"). **One drain-and-retry
  is no longer a proof of progress** — another core can take the space
  it freed — so the retry is a bounded loop that reports rather than
  spins. Whether one buffer suffices at eight cores is AL-S8's
  `wal_ring_full` cell, not a constant decided here.

What the ruling keeps viable: the latch sits behind the same
`Append(record) → Lsn` signature every emitter uses today, so the
unshared stream is one implementation of it and the shared one another;
the operator may still choose D3(a) or D3(c) for the flush side without
touching an emitter.

**AL-R2 — The LSN is the instance's byte offset; the record *header* and
the segment format do not change; the one stream is stream 0.** The
record header carries no core id (`record.hpp:256-276`) and gains none.
**Amended 2026-09-02 from building AL-S4a**: this ruling said AL-R4's
`core_id` would go in the two **checkpoint payloads**, and it does not —
it rides the envelope's per-type `flags` byte, which is part of the
record header this sentence says does not change. The header's *layout*
still does not change, and no format event was needed, but the claim as
first written was wrong about where the field went and why; `payload.hpp`
carries the argument. Segments stay
`wal-0-<segment_no>.log` with segment-header `core_id = 0`, which is
what keeps the `cores = 1` bytes identical (AL-3 G) and makes a
`cores = 1` log and a `cores = 8` log the same file set. `core_id` in
the segment header is retained as the stream's number, always 0, and
its `Corruption` check on a mismatch stays.

**AL-R3 — The superblock records the topology; one anchor under one
stream; `core_count` stays pinned.** *Amended 2026-09-02 from building
AL-S2; the first draft said "`core_count` recorded and not pinned,
format 15 → 16", and two thirds of that was wrong.*

- **The volume records how many streams its log is** —
  `log_topology`, `kPerCoreStreams` (0) or `kSingleStream` — because
  that is what tells recovery how many streams to look for, and it is a
  durable fact rather than a setting a mount may choose. Everything
  below keys on it, so the cutover flips one field rather than
  rewriting the rules.
- **No format event, against the work order's own first draft.** The
  field consumes `reserved1` at offset 12, which `CreateFresh` has
  always zeroed — and 0 *is* `kPerCoreStreams`, so every image ever
  written already states the truth about itself. This is
  `page_header.hpp`'s precedent for `relayout_epoch` and `owner_oid`,
  verbatim. The draft also said "15 → 16": the tree has been at **16**
  since 2026-08-27 (`sys.ranges`), so that bump was already spent. The
  version event D14 owes lands with the cutover and the stamp change,
  where the *pages* stop meaning what they meant; landing one here
  would refuse every existing volume for three stages and buy nothing.
- **`wal_anchor(0)` is the anchor under one stream**, slots 1..63 stay
  in the layout as zeros, and `SetWalAnchor` refuses any core but 0 —
  so a caller cannot bypass the fold AL-R4 gives core 0. Under
  `kPerCoreStreams` every core keeps its own slot, unchanged, which is
  what lets this land before the cutover.
- **`core_count` stays pinned, and the draft was wrong to unpin it.**
  The WAL reason does dissolve with one stream. A second reason does
  not: `sys.tables.owner_core` and `sys.ranges.owner_core` name cores,
  so a mount at fewer cores leaves relations owned by a core that does
  not exist. `superblock.hpp`'s own `[OPEN]` says so — *"correctness
  needs only that relations whose owner core no longer exists are
  moved"* — and moving them is AR0 §2's affinity work, not M0's.
  Unpinning belongs to whichever milestone builds that; until then the
  refusal at the door is the only honest answer.
- `kMaxWalCores` stays the anchor table's slot count **and keeps
  bounding `cores`**: under one stream the bound is vestigial, but
  nothing asks for more than 64 cores, and lifting a limit with no
  caller is a change with no way to be wrong usefully.

**AL-R4 — Checkpoints in M0: per-core records in one stream, the anchor
is the minimum.** Buffer pools stay per core in M0 (`page.md` §6, EV4),
so a dirty-page table is a per-core fact and each core's checkpointer
keeps running its fuzzy checkpoint (`wal.md` §11) — into the one stream,
its `CHECKPOINT_BEGIN`/`END` naming the publishing core — **in the
envelope's per-type `flags` byte, not the payload** (amended from
building AL-S4a; `payload.hpp` says why, and AL-R2 carries the same
correction). The in-memory `CheckpointAnchorRecord` already carries a
`core_id` (`checkpointer.hpp:180-191`), but it is not a log record. The anchor's
`redo_start_lsn` is the **minimum** over the cores' latest completed
checkpoints, floored by the oldest live `TXN_PREPARE` as today.

**The fold lands before the first write of `kSingleStream`** — AL-S3
built it, which discharges the constraint below in the safer direction:
the flip now arrives to a publish path that already folds.

**The fold and the first write of `kSingleStream` may not be separated
with the flip first, and this is a hard constraint rather than a
preference.**
`SuperBlockCheckpointAnchor::Publish` passes a peer's own `core_id`
straight to `SetWalAnchor` (`superblock_checkpoint_anchor.cpp:8`), which
AL-S2's refusal now rejects under one stream. So the instant anything
writes `kSingleStream`, every peer's checkpoint publish **fails** rather
than degrading. The same stage owes the second half of that: a peer's
`superblock_` is a default-constructed copy, so `single_stream()` reads
false on every peer until the topology is carried on
`CoreRuntime::Config` and applied at `Open`, the way `next_trx_id`
already is (`superblock.hpp`'s accessor states the trap).

**The prepare floor survives the fold, and here is why it is not a
coincidence** (source-read 2026-09-02, AL-S3). `Checkpointer::Begin`
applies it per core when it computes that core's redo start —
`pending_redo_start_ = min(pending_redo_start_, OldestPreparedLsn())`
(`checkpointer.cpp:149-151`) — so every number the fold sees is already
floored, and a minimum over floored values is floored. The fold cannot
lift it, and would have to be rewritten to a *maximum* to break it.
`SetWalAnchor` has exactly one caller in the tree, the publish path, so
there is no second route into the anchor that could skip either rule.

**What still reads a peer's slot, and therefore what AL-S4 owes.** Under
one stream, slots 1..63 are zeros, and three readers ask for them today:
`expeditor.cpp:1523` hands each peer `wal_anchor(core_id)` at
`CoreRuntime::Open`, `expeditor.cpp:869,1527` pass `wal_anchors()` to
recovery, and `prepared_resolver.cpp` uses a coordinator's anchor to
bound its scan. All three read an all-zero anchor as "replay from the
start of the stream", which is **slow and safe, never wrong** — and all
three are AL-S4's, where recovery becomes one pass that reads slot 0
alone.

**Who folds, and where** — the seam AL-R3's "refuses any core but 0"
would otherwise leave unnamed. The publish path today writes
`SetWalAnchor(anchor.core_id, …)` with the *peer's* id
(`superblock_checkpoint_anchor.cpp:8`). Under M0 that call site keeps
every core's latest published anchor in a core-indexed **in-memory**
table on core 0, and writes slot 0 with the minimum over it; the
peers' `RemoteCheckpointAnchor` ship stays as the mechanism that brings
a peer's number there. `SetWalAnchor` itself refuses any core but 0, so
the fold is the caller's and is testable on its own. A single gathered
checkpoint is M1's, when the pools merge.

**AL-R5 — Recovery in M0: one pass at mount, on core 0, before any peer
opens.** Analysis scans the one stream from the anchor, building the
dirty and active tables per `core_id` from each core's `CHECKPOINT_BEGIN`
and the transaction table by `txn_id` (instance-unique already,
`cross-owner-txn.md` §3). Redo applies by page id through core 0's
store, which CC11 leaves ungated, and **does not restamp** (AL-R6).
Undo rolls back every loser. `CoreRuntime::Open` no longer recovers
anything. A participant's `TXN_PREPARE` and its coordinator's decision
are now records of one stream, so `PreparedResolver`'s cross-file read
becomes a forward lookup in the same scan; `cross-owner-txn.md` §2c's
retention obligation collapses into the ordinary redo-start floor,
because a decision-bearing segment is the prepare's own segment set.
`SHOW META`'s recovery block becomes one block on core 0.

**AL-R6 — The stamp keeps its claim meaning and loses its redo meaning;
the handoff record stays.** Under one LSN space redo's idempotence test
is `record.lsn > page_lsn` alone, so `redo.cpp:359-373`'s foreign-stamp
`Corruption` goes and `redo.cpp:478-479`'s restamp goes — a restamp at
mount would write stamp 1 onto every peer-owned page and defeat the
claim-at-fault of `device_page_store.cpp:702-725`. The mutation-path
stamp (`StampPageLsn`) and the claim stay until M1 replaces ownership
with affinity.
`PAGE_HANDOFF` stays as the logged ownership transfer (its analysis-side
scope removal, `analysis.cpp:110-135`, becomes a no-op and is deleted).
`page-lsn-cross-stream.md` is marked **superseded in part at M0**: its
§9 rules 1–3 (handoff over a flushed page) stand, rule 4's stamp is a
claim and not a stream, and its §6 PL-A is what M0 built.

**AL-R7 — `cores = 1` is byte-identical, and a golden log proves it.**
At `cores = 1` every WAL object is core 0's as today, behind AL-R1's
single-core implementation. The gate: the contract suites byte-for-byte
(`CLAUDE.md`, Working Rules), plus one new cell — the same statement
script against a fresh volume at `d15b5ac` and after AL-S1 yields
**identical `wal-0-*.log` bytes**, the superblock differing only in
`format_version` and the fields AL-R3 names.

**AL-R8 — Measurement: what a v3 number is and where it goes.**
**Ratified 2026-09-03 by CLA** under the operator's standing "I will
follow CLA's proposal"; `bench/` reopens with AL-S8 and not before.
`bench/v3.0.0/`, its README restated for the new engine; the same
drivers (`tools/scenario*`, `tools/*_benchmark.py`), unmodified — a
driver change inside a measurement stage measures the driver; this host
— 8 cores, AMD EPYC 9V74, the host the v2.8.0 result files name —
`build-release` only; every file named by `git describe --tags`; a
**fresh series** with no delta against any v2.x number (D15). The M0
cells are AL-S8's.

Four rules carry over from `bench/docs/README.md` at `1769487`, because
each of them has already invalidated a run on this box and none is
implied by "measure in release":

1. **Release, rebuilt at the measured commit.** Debug has reported the
   wrong sign; a stale `build-release` silently prices an older engine.
2. **A block device, never tmpfs.** Check with `df -T` at run time, and
   **name the device in the results file** — `/tmp` is tmpfs here.
3. **Measure a copy of the binary.** `cp` it into the run's directory,
   hash the copy, start every server from the copy. The build tree is
   shared with every other session in this repository and a `cmake
   --build` landing mid-matrix would swap the engine with nothing in the
   driver output to show it.
4. **Record the host's load, and re-check it per cell** — three session
   scratchpads were live here on 2026-09-02 and a competing build moved
   a p99 by 12×. `/proc/loadavg` plus `pgrep -a -f "cc1plus|cmake
   --build|ctest"` before each cell, both written into the file.

And one that is new to v3, because M0 is what makes it matter: **the
port is chosen, never defaulted.** 15432 on this box is held by an
unrelated instance under `/home/cdkbs/autotrade`; a cell that binds the
default either fails or, worse, talks to that server.

## AL-5 — Stages

Sizes: S ≤ ½ day, M ≤ 2 days, L more. Every stage: `critics-developer`
review, the full suite, sync with `origin/main` on the branch, stop.

| # | Stage | Cells (definition of done) | Size |
|---|---|---|---|
| AL-S0 | This document; `instructions/v3.0.0/index.md`; AR0 filed with AR0-V; `instructions/README.md` and `CLAUDE.md`'s one paragraph reopening `instructions/` | the five files at the commit that carries them | the hour |
| AL-S1a | **The seam** — the mechanism only, no cutover. `WalStream` opens shared or unshared, the shared one serializing the staging state on one latch; `WalManager::Attach` over a borrowed stream and writer; a peer's `Sync` becomes a `RequestSync`; the batch bookkeeping closes against a watermark another thread moved | AL-R7's golden log at `cores = 1`; four threads interleaving appends, every record at the LSN its appender was handed, none twice, all scanning back in LSN order; the watermark never retreating under concurrent syncs; `Attach` refusing an unshared stream and a null writer; a peer's `strict` commit made durable by the writer with the peer's own `syncs` at 0; a peer's `group` batch closing on the drain after another core's sync | M |
| AL-S1c | **The cutover.** `FileLogDevice::Open(wal_dir, 0)` the only open; the expeditor owns the stream and the writer; every `CoreRuntime` attaches; peer drains keep their tick but sync through the writer. **`FileLogDevice` needs no change for it** — source-read 2026-09-02 and recorded so the stage does not re-derive it: `CreateSegment` grows `segments_` under `segments_mutex_` (`:290-295`) while `Sync` copies the descriptors under the same lock and syncs outside it (`:365-396`), and the unlocked reads in `WriteAt` (`:303,313`) are safe because the shared stream's latch already serializes every `WriteAt` against every `CreateSegment`; concurrent `pwrite` and `fdatasync` on one descriptor need no lock, which is the header's own justification | at `cores = 4` a mount, a write on every core and a clean shutdown leave one `wal-0-*` segment set; `SHOW META` on a peer reports no device sync of its own | M |
| AL-S1b | **The writer.** One `WalWriter` for the instance, started by the expeditor; `SyncAll` on core 0 covers the writer's in-flight sync | `wal_writer_test` unchanged (core-agnostic today); a peer's `strict` commit waits for the platter and no longer calls `fdatasync` on its own thread (`wal_syncs` on a peer reads 0) | S |
| AL-S2 | **The superblock** (AL-R3): the volume records its log topology, and the anchor rule keys on it. No format event, and `core_count` stays pinned — the amended AL-R3 says why both | `superblock_test`: an image from before the field reads as per-core; the topology survives a round trip; an unknown value is refused rather than defaulted; under one stream only slot 0 takes an anchor and a peer's attempt leaves the slot untouched; under per-core every core still takes its own | S |
| AL-S3 | **The fold** (AL-R4): the anchor publish path keys on the topology — slot `core_id` under per-core streams, slot 0 alone holding the minimum over every core under one stream, and held at the mount's own anchor until every core has published. **The `core_id` on `CHECKPOINT_BEGIN`/`END` is not here**: its only consumer is recovery rebuilding per-core tables from one stream, so it lands with AL-S4, which is also where its record-format event belongs | two cores checkpoint at different LSNs and the one anchor names the lower; the fold carries the whole set from the core that supplied the minimum; a core's later checkpoint replaces its own contribution; the anchor does not advance while a core has never published, and the floor it holds is the mount's anchor rather than the start of the log; a peer's publish succeeds rather than hitting AL-S2's slot refusal; nothing folds under per-core streams | M |
| AL-S4a | **The checkpoint record names its core** (AL-R4's half that AL-S3 deferred): the publishing core rides the envelope's per-type `flags` byte, which both records have always written as 0 and nothing has ever read | a peer's checkpoint carries its core in both records; core 0's still carry a zero byte, so a single-core log is unchanged; a core id past the byte is refused rather than truncated into another core's identity | S |
| AL-S4b | **Recovery** (AL-R5): one pass at mount on core 0 under one stream; per-core dirty and active tables rebuilt from the `CHECKPOINT_BEGIN`s AL-S4a stamped; `PreparedResolver` becomes an in-stream forward lookup; `CoreRuntime::Open` recovers nothing; one `SHOW META` recovery block; the three peer-slot readers AL-R4 names retire | `prepared_recovery_test`'s two-stream fixture rewritten as one stream with two cores' records; `mount_recovery_test`: a loser on core 2 is rolled back by the mount before core 2 opens; every `sim/` corpus seed reconciles (`scripts/sim.sh`) | M–L |
| AL-S5 | **Handoff, and the rest of the stamp** (AL-R6). **Redo's two halves moved into AL-S4b** — the foreign-stamp `Corruption` and the restamp — because the ordering was wrong: S5 sat *after* the cutover, so the first multi-core mount after S1c would have refused on a peer-stamped page before S5 ever ran. What is left here: analysis's handoff scope removal deleted; `page-lsn-cross-stream.md` marked superseded in part | `wal_redo_test`: a page stamped by core 2 is redone by the mount pass and keeps stamp 3; `core_runtime_test:561-562` still holds; `device_page_store_test`'s own/foreign fixture unchanged; **and the case AL-R7's golden log cannot see** (AL-7) — a page carrying stamp 0, "never stamped", which the removed restamp used to set to 1: the cell is on the data file, since the golden log compares log bytes and the superblock only | S–M |
| AL-S6 | **Observability**: instance-level `wal_*` counters on core 0, `wal_anchor_count` and the per-core `wal_syncs` comment gone; `client-manual.md` §3 rows | `command_dispatcher_test:140` updated; `SHOW META` on a peer prints no WAL block | S |
| AL-S7 | **Simulator**: already one device (AL-3 I); the crash-at-op corpus re-run against the new mount pass, any seed that assumed a per-core anchor fixed | `scripts/sim.sh` green on the full corpus | S |
| AL-S8 | **The baseline** (AL-R8, `ck-tester`, `build-release`): scenario 0 and scenario 2 at `cores = 1` and `cores = 8`, `group` and `strict`; the M0-specific cells — a peer's commit tail under `group` (the p99 claim of `manager.cpp`'s `Sync()` comment re-measured), `fdatasync` share of reactor wall time per core (§8b's instrument), `wal_ring_full` count at eight writers | one results file per cell, each carrying `git describe --tags`, p0–p100 with p25, the wait breakdown, and no delta to any v2.x number | M |
| AL-S9 | **Prose, last**: `wal.md` §3/§11/§12/§16-9, `page.md` §6 line 108, `sched.md` §5's atomics sentence, `crosscore.md` CC11's "one stream" reasoning, `cross-owner-txn.md` §2c, `rules.md` §3, `page-lsn-cross-stream.md`'s status line, `CLAUDE.md`'s WAL and cross-core rows, `manual/server/server.md` `wal_dir`/`cores` rows | every sentence AL-3 J lists rewritten or struck; no spec states a per-core stream | M |

**Order**: S1a → S1b → S2 → S3 → S4a → S4b → **S1c** → S5 → S6 → S7 → S8 → S9.
S2 and S3 are independent of each other; S5 needs S4; S8 needs every
earlier stage green on the full suite; S9 needs S8's numbers where a spec
quotes one.

**Why the cutover sits after recovery** (amended 2026-09-02, from
building S1a). The first draft of this table put the whole of S1a
— mechanism *and* cutover — before S2/S3/S4. That order cannot hold: a
peer recovers its own stream at `CoreRuntime::Open`
(`core_runtime.cpp:175-177`) and `ValidateSegmentHeader` refuses a
segment whose header names another core (`log_scanner.cpp:20-23`), so
the first mount at `cores > 1` after a cutover would fail in analysis on
core 1 with a `Corruption` naming the segment. Recovery must be the
single mount-time pass of AL-R5 **before** the peers stop opening their
own devices. The mechanism is separable from the cutover and lands
first, which is what S1a now is.

## AL-6 — Row status (CLA, appended as rows land)

| row | status |
|---|---|
| AL-S0 | **Written 2026-09-02** on `worktree-v3.0.0-arch-revision` at `d15b5ac`; the survey is source-read at that commit. No engine code changed; the suite was not executed for this row and is not claimed |
| AL-S1a | **Built 2026-09-02** on `worktree-v3.0.0-arch-revision`. The seam, and nothing that cuts over to it. `WalStream::Open` takes `shared`; when set, one latch (`include/kds/base/latch.hpp`, the primitive AR0's revised G1 admits) serializes the staging state, the gauges become relaxed atomics, `flushed_lsn_` becomes a field rather than `append_lsn_ - ring_used_`, and every public entry point takes the latch once and calls a `*Locked` half — so the roll inside `Append` reaches the seal and the flush without taking it twice. `Sync` holds the latch for the flush and **drops it for the device sync**, publishing the watermark it captured as a compare-exchange maximum. `WalManager::Attach` builds a manager over a borrowed stream and writer: it refuses an unshared stream and a null writer, its `Sync` is a `RequestSync` and never a device call, and `ResolveBatches` closes the group batch against a watermark another thread moved (the drain calls it every tick for that case). `MemoryLogDevice` takes a mutex, being the test double a shared stream writes to while the writer syncs; `log_device.hpp` states the pairing an implementation must accept. **Nothing is wired up**: the expeditor and every `CoreRuntime` still open their own device, and `Open`'s default is unshared, so `cores = 1` and `cores > 1` both run exactly the code they ran at `d15b5ac`. Cells: `tests/wal_golden_log_test.cpp` (AL-R7 — a 17-statement script over the simulator, clean shutdown, CRC32C over every log byte, pinned at `0x07b052c3` from the engine at `d15b5ac` **before** this change, and it did not move) and `tests/wal_shared_stream_test.cpp` (cells: the unshared default; four threads appending past both the ring and the segment, with every LSN issued once, every record on the device at an LSN its appender was handed, in order; the watermark never retreating under three concurrent syncers; the two `Attach` refusals; a peer's `strict` commit made durable by the writer with the peer's own `syncs` at 0; a peer's `group` batch closing on its drain after core 0's sync). One defect found and fixed while building: `WalManager::Append`'s single drain-and-retry is no longer a proof of progress on a shared stream, since another core can take the space the drain freed — now a bounded loop (`kRingDrainAttempts = 4`) that reports `OutOfSpace` rather than spinning. **Suite: 3214/3214**, re-run against the final tree after the retry fix (`ctest -j8`, Debug, 69.9 s). Overhead not measured (the interleaved A/B is suspended by operator decision). **Amendment**: the cutover left this stage and became AL-S1c, after recovery — see the note under the stage table |
| AL-S1b | **Built 2026-09-02** on `worktree-v3.0.0-arch-revision`, and it closed a durability defect AL-S1a had left. S1a routed an attached manager's `Sync()` to `writer_->RequestSync` and returned — so on a peer, **`EnsureDurable` returned OK for a sync merely asked for**. That call is the WAL-before-data gate the page store takes before writing a dirty page (`device_page_store.cpp:1098`, `page_mgr.cpp:200`, wal.md §8-1), so a data page could have reached the platter ahead of its log record; a D1 commit and `SyncAll` were untrue in the same way. The path is now split by what the caller means: **`Sync()` blocks** — on the owner by syncing the device on its own thread as before, on a peer by flushing and waiting on the writer's watermark (`WalWriter::EnsureDurable`, the condition variable that class already had) — and serves the three callers whose whole meaning is "wait", a D1 commit, the gate, and `SyncAll`. **`RequestSyncNow()` does not block** and is the drain's path: a tick may not hold the reactor, because blocking it would stall every session on that core for another thread's `fdatasync`; the batch closes on a later tick at `DrainOnce`'s opening `ResolveBatches`. `writer_syncs()`/`writer_sync_failures()` now read the **owned** writer only, so a peer reports 0 rather than N cores each reporting the same shared count. One regression caught before it landed: routing the D3 interval tick through the new call would have put core 0's `fdatasync` back on its reactor (the stall whose removal took `relaxed`'s p99 from 2,208 µs to 194 µs), so `RequestSyncNow` hands off whenever a writer exists, owned or borrowed, and only a writerless manager syncs inline. Cells, all in `tests/wal_shared_stream_test.cpp`: a peer's `strict` commit durable when `Commit` returns with the peer's own `syncs` at 0 and its `writer_syncs()` at 0; `EnsureDurable` on a peer returning only once the record is on the platter; `SyncAll` on a peer leaving `durable_lsn == appended_lsn`; the drain asking without waiting and the batch closing on the next tick. `wal_writer_test` and `wal_manager_test` unchanged and green. **Suite: 3217/3217** (`ctest -j8`, Debug, 75.3 s). Overhead not measured (the interleaved A/B is suspended by operator decision). Not in this stage: the expeditor still starts the only writer it ever started, on core 0's own manager — one writer *for the instance* arrives with the cutover, AL-S1c |
| AL-S2 | **Built 2026-09-02** on `worktree-v3.0.0-arch-revision`, and **two thirds of AL-R3's first draft was wrong**; the ruling is amended above and this row is what landed. The volume now records its log topology — `SuperBlockFields::log_topology`, `kPerCoreStreams` (0) or `kSingleStream` — and `SetWalAnchor` refuses any core but 0 under one stream, so no caller can bypass the fold AL-R4 gives core 0. `Decode` refuses a value it has no reading for rather than defaulting, ordered after the magic and version checks so a garbage page still fails on magic. **No format event**, against the draft's "format 15 → 16": the field consumes `reserved1` at body offset 12, which `CreateFresh` has zeroed and `Encode` has written from the struct since the initial commit (`9cde1b4`), with no other writer of the superblock body anywhere in the tree — so every image ever written already holds the value that means "per-core", which is the `page_header.hpp` precedent for `relayout_epoch` and `owner_oid`. The draft's "15 → 16" was also already spent: the tree has been at 16 since `sys.ranges` on 2026-08-27. **`core_count` stays pinned**, against the draft's unpinning: the WAL half of the reason dissolves under one stream, but `sys.tables.owner_core` and `sys.ranges.owner_core` mean a mount at fewer cores leaves relations owned by a core that does not exist, and moving them is AR0 §2's affinity work. Cells (`tests/superblock_test.cpp`, `SuperBlockTopologyTest`): a fresh image's topology byte is the zero every old image holds, asserted **on the encoded page** over a poisoned buffer rather than on a value poked back in; a non-zero topology survives **both halves** of the codec, re-encoded and re-read, which is what would catch an `Encode` that dropped the field; an unknown value refused; under one stream only slot 0 takes an anchor and the peer's slot is untouched; under per-core every core still takes its own. **Nothing writes `kSingleStream`**, so a running instance is byte-identical — AL-S1c writes it at bootstrap. **Suite: 3223/3223** before the two test strengthenings, re-run after them (`ctest -j8`, Debug). Overhead not measured (the interleaved A/B is suspended by operator decision) |
| AL-S3 | **Built 2026-09-02** on `worktree-v3.0.0-arch-revision`. `SuperBlockCheckpointAnchor::Publish` keys on the topology AL-S2 recorded: under per-core streams it writes slot `core_id`, exactly as before; under one stream it records the publishing core in an in-memory map and writes **slot 0** with the record whose `redo_start_lsn` is lowest — the whole record from that core, never a field-wise minimum, since four numbers from four cores would describe a checkpoint that never happened. The publish path already carried the publishing core (`remote_checkpoint_anchor` ships it, `expeditor.cpp` reconstructs it), so the fold needed no record change. **One defect found and fixed while building, and it is the reason this stage is not three lines**: a minimum over the map alone is a minimum over the cores that happen to have published *in this process lifetime*. At a fresh mount core 0 checkpoints first, cores 1..3 have not, and slot 0 would advance to core 0's redo start — past records the other cores' still-dirty pages need, which the next crash would then not replay. So the anchor is **held at the mount's own anchor until every core has published at least once**; that bound is sound because recovery replayed from it, so no core's earliest needed record can precede it. The cost is one checkpoint of warm-up per mount. **What is not here**: `core_id` on the two checkpoint records. Its only consumer is recovery rebuilding per-core tables out of one stream, and adding it now would cost a record-format event for nothing — it moves to AL-S4, and AL-R4 says so. Cells (`SuperBlockFoldTest`, and one added to `SuperBlockAnchorTest`): the lowest redo start wins over four cores; the winning core's whole set is carried; a core's later checkpoint replaces its own contribution and the anchor rises to the new laggard; the anchor does not move while a core has never published, then moves when the fourth speaks; the warm-up holds at the mount's anchor rather than resetting to the start of the log; a peer's publish succeeds rather than hitting AL-S2's slot refusal, which is the constraint the S2 review raised, discharged by building the fold *before* the flip; and nothing folds under per-core streams, which pins the no-change property. **Suite: 3229/3229** before the warm-up fix; re-run after it (`ctest -j8`, Debug). Overhead not measured (the interleaved A/B is suspended by operator decision) |
| AL-S4a | **Built 2026-09-02** on `worktree-v3.0.0-arch-revision`. Both checkpoint records now name the core that published them, in the envelope's per-type `flags` byte rather than in either payload. Why the envelope: `CHECKPOINT_BEGIN`'s payload is a fixed header followed by two variable-length tables, so a field in the header shifts every entry and a field at the tail is readable only by inferring its presence from the length — while the envelope has a byte set aside for exactly this kind of per-type fact, and `ASSERT_RESERVE` already uses bit 0 of it that way. **No format event**, for the third time in this milestone and the same reason each time: both records have been appended with `flags = 0` since they existed and nothing has ever read the byte, so every record ever written already says "core 0", which is what a record in core 0's stream *is*. `CheckpointCoreFlag` refuses an id past the byte rather than truncating one core's identity into another's; the WAL layer knows nothing of core counts, so the byte is the only bound it can enforce. Cells: a peer's checkpoint carries its core in both records and in its anchor; core 0's still carry a zero byte, so a single-core log is unchanged; the refusal. **Suite: 3236/3236** (`ctest -j8`, Debug) — including AL-R7's golden log, which did not move, the independent confirmation that a single-core log's bytes are unchanged. Overhead not measured (the interleaved A/B is suspended by operator decision) |
| AL-S4a review | **Applied 2026-09-02.** The review found no correctness bug and confirmed the load-bearing facts independently: **nothing in the tree validates `header.flags`** for any type but `ASSERT_ROLLBACK`, `sim/` decodes no records at all, and the golden log's simulator is core 0 end to end — so the pin is the strongest existing proof of the no-format-event claim, stronger than the cell written for it. Four findings applied. (1) **AL-R2 and AL-R4 still said "payload"**; both are amended in place, which is the point of amending rulings rather than quietly diverging — AL-S4b would otherwise have read AL-R5 next to a ruling describing a field that does not exist. (2) The peer cell opened a *per-core* stream at core 5, the one topology where the byte is redundant; it now attaches to a **shared** stream, where the segment header says 0 and only the record can answer whose checkpoint it is, and both cells share one records helper. (3) `CheckpointCoreFlag`'s refusal sat on the checkpoint path, where a violation would make a core log an error every interval forever — or wedge it, since `Complete` never clears `in_progress_`; the bound moved to `WalManager::Open`/`Attach`, where it fails the mount, and the helper is gone. (4) The comment lost its duplicated bullet, gained the clause that matters — every record ever written says "core 0", which is what a record **in stream 0** is, and a legacy `wal-N` file must never enter this attribution — and moved above the `CHECKPOINT_BEGIN` banner instead of splitting one payload's two structs. Declined for now with a reason: `CheckpointAfterRecovery`'s `core_id` parameter is a second name for `wal.core_id()` and should go, but it has six call sites in recovery, which AL-S4b is already rewriting |
| AL-S4b | **In progress 2026-09-02** on `worktree-v3.0.0-arch-revision`. Two of its pieces are built. **A peer recovers nothing under one stream**: core 0 recovered the whole log before the peer was built, so a second pass would redo another core's pages through this core's store — outside the extent grants that say which pages are its to write — and undo losers already rolled back. `CoreRuntime::Config` carries the topology for it, the third field copied off core 0's superblock because a peer's own copy is default-constructed and `kPerCoreStreams` is 0, so a peer never told would conclude its own stream is its to recover; the peer's transaction-ceiling check now reads the stored report, so it is vacuously true rather than skipped. **And redo's stamp discipline**, pulled forward out of AL-S5 on an ordering defect this stage found: S5 sat after the cutover, so the first multi-core mount after S1c would have met a peer-stamped page and refused before S5 ever ran. Under one stream the foreign-stamp `Corruption` is meaningless — there is no other stream to have crossed from, so core 2's stamp met in core 0's pass is core 2 owning its page — and the restamp is harmful, since it would hand that page to core 0 and `device_page_store`'s claim-at-fault would read it that way at the next mount. `AnalysisStart` carries the fact down, a bool rather than the superblock's enum because `wal/` sits below `server/`. Cells: another core's stamp is not foreign under one stream **and still is under per-core**, the same bytes read both ways; and a record applied to an already-stamped page leaves the stamp alone while the page_lsn still moves. **And the prepared-transaction resolution**, which under one stream stops being a second file to open: the participant's `TXN_PREPARE` and its coordinator's decision are records of the same log, so resolving is a lookup of the coordinator's own transaction id in the table the pass just built. **Absence means aborted, and that is sound rather than a default** — the redo start is floored by the oldest live prepare (`checkpointer.cpp`) and the fold takes the minimum over cores, so a scan holding the prepare holds any decision made after it; this is `cross-owner-txn.md` §2c's retention obligation collapsing into the ordinary floor, there being no second stream whose segments could have been recycled out from under the question. Cells: a commit found in the same scan; an undecided prepare rolled back with undo called; a coordinator's abort reaching the same outcome by the other route; and the per-core path still refusing without a resolver, which is the promise a participant makes about not deciding for itself. **Suite: 3242/3242** (`ctest -j8`, Debug). Still owed by this stage: one `SHOW META` recovery block and the three peer-slot readers. Overhead not measured (the interleaved A/B is suspended by operator decision) |
| AL-S4b | **Built 2026-09-02**, over five commits, each carrying its own reason: the peer skip and the topology on `CoreRuntime::Config` (`776c56e`); redo's stamp discipline, pulled forward out of AL-S5 on an ordering defect (`9e2c0fa`); the in-stream resolution of a prepared transaction (`96b2c16`); the two stamp holes the review found — undo restamping a peer's page, and a page redo creates ending unstamped — closed at `b0157ef` and `cc4c83e`; the assertion resume's two conflated core ids (`6c7c802`); and the skip's own cell with the metadata block (`eecf9e0`, this one). What the stage did **not** need turned out to be as informative as what it did: analysis already merges several cores' checkpoints correctly, so no per-core dirty table was built, and AL-S4a's core byte earns its place on `PAGE_INIT` rather than on the checkpoint records it was cut for. **Suite: 3246/3246** (`ctest -j8`, Debug). Overhead not measured (the interleaved A/B is suspended by operator decision) |
| AL-S1c | **In progress 2026-09-02**: the wiring is built, the flip is not. Core 0's `WalManager` opens its stream **shared** where the volume says its log is the instance's, and hands the stream and the writer to every peer on `CoreRuntime::Config`; a peer told the volume has one stream then **attaches** rather than opening `wal-<core>-*`, so it appends through core 0's latch and asks the writer for every sync. A peer told that and handed nothing to attach to is **refused**, because opening one of its own would write records into a file no mount of this volume ever replays — a failure that would surface as lost rows after a crash, arbitrarily later, rather than at the mount that caused it. Cells: the restart test's third open now attaches for real and asserts the peer's manager *is* core 0's stream; and the refusal. **And the flip landed the same day**: `BootstrapDatabase` creates every new database as `kSingleStream`, so AR0-1 is true of the engine rather than of a branch. `CreateFresh` keeps `kPerCoreStreams` as its default, the way `core_count` keeps 1 — a caller that is not testing the topology gets the shape needing no other party — and a volume written before this build still says per-core and keeps every per-core rule, which is what lets the two be read side by side rather than one misread as the other. **The flip broke exactly two tests, and both were the flip working.** `CoreRuntimeTest`'s fixture bootstrapped a *one-core* volume while every test in it drives two, which nothing checked until AL-S3's fold began asking which cores a volume has; it now bootstraps the two it models. And the two anchor tests read a peer's slot, which the fold no longer writes: one now proves arrival through the fold's own input and movement by completing the fold, and the other had to model a core 0 that *also* checkpointed recently — because the anchor is the minimum over cores, so one laggard holds the instance's replay range open however recent everyone else's is. That is true of one stream and is why AL-R4 leaves a single gathered checkpoint to M1. **Suite: 3247/3247** (`ctest -j8`, Debug). Overhead not measured (the interleaved A/B is suspended by operator decision) |
| AL-S5 | **Built 2026-09-02**, and the ruling it enacts was wrong about its own subject. AL-R6 said analysis's handoff erase "becomes a no-op and is deleted" under one stream. It does not become a no-op — it becomes **unsound**, which is a different reason and a sharper one. What licenses the erase per core is not the handoff but the sentence around it: *this stream* owes the page nothing below this LSN, which holds because a handoff is logged by the **receiver** as an acquisition and the receiver's stream has nothing for the page below it (`core_runtime.cpp`'s `AdmitWritePages` says so). One stream also holds the **giver's** records for that page, and erasing drops them from the dirty table — which makes redo's not-dirty filter skip every one of them. So the erase is skipped under one stream, and the handoff seeds nothing either, being an ownership fact rather than a mutation; `max_page_id` still takes it, which the erase never governed. Keeping the entry costs redo re-applying records the image may already hold, idempotent under the `page_lsn` gate: slower at worst, where the erase is wrong at worst. This also gives `AnalysisStart::single_stream` a reader in `Analyze`, which the AL-S4b review had objected it lacked. Cells: under one stream the giver's record still seeds the page, the handoff does not become its recLSN, a page named only by a handoff is still not dirty, and the high-water still rises past it — with the per-core erase cell unchanged beside it. `docs/spec/page-lsn-cross-stream.md` now carries a **superseded-in-part** header saying, rule by rule, what one stream keeps and what it drops — including that AR0 §7's flat "superseded" overstates it, since a pre-change volume is still governed by every word. **Suite: 3248/3248** (`ctest -j8`, Debug). Overhead not measured (the interleaved A/B is suspended by operator decision). **Amended by AL-7d**: the reason written into this row and into `analysis.cpp` was still wrong in its second draft. The erase is not licensed by *who logged the handoff* — it is licensed by rule 1(a)'s **flush**, and a flush covers one core's page store. Under per-core streams the erase's reach matched the flush's; under one stream the erase speaks for every core's records while the flush still speaks for one core's frames, and the concrete loser is `relation_grant_service.cpp`'s re-delivery, which re-runs the publish and appends a handoff from core 0 for a page a peer holds dirty and unflushed. The conclusion did not move; the proof did |
| AL-S6 | **Built 2026-09-02**, and its largest piece was a behaviour fix rather than a field rename. **An idle peer's drain stopped taking its early-out at the cutover**, because `appended_lsn()` on an attached manager is the *instance's* append point, which core 0 moves constantly — so the equality was never true on a peer and every tick fell through to the D3 branch, asking for a sync of bytes it does not own, every interval, forever. Two changes: the free-tick test is now this core's own work, and **an attached manager does not take the interval branch at all**, because the loss window belongs to the log and the log has one drain bounding it. That also makes the counters honest without inventing one: a peer's `wal_syncs` and `wal_interval_syncs` are structurally 0 under one stream, which is true — it performs no device sync, flushing through core 0's stream and waiting on core 0's writer — so `SHOW META`'s stated subtraction rule holds on a peer trivially instead of underflowing, and a peer's durability cost is read on core 0. `SHOW META` gains `wal_topology` and prints `wal_anchor_count` **only under per-core**, since under one stream it can only ever say 1; `client-manual.md` §3's two rows say all of it. **Suite: 3249/3249** (`ctest -j8`, Debug). Overhead not measured (the interleaved A/B is suspended by operator decision) — though this stage removes a per-peer, per-interval sync request that AL-S8 would otherwise have measured as noise it could not attribute. **Amended by AL-7d** on two counts. `wal_sync_failures` is **not** structurally 0 on a peer: its flush of the shared ring can fail and so can its wait on the writer, and a failed wait is counted on core 0 too — the one field where a peer and core 0 report the same event. And the free-tick early-out is **not** "this core's own work" as a principle: on core 0 it is deliberately over the whole log, and that asymmetry is the only thing bounding a peer's `relaxed` loss window now that peers do not tick the interval — core 0 cannot take the early-out while any peer has bytes unflushed, so it reaches the D3 branch and flushes them within one `relaxed_flush_interval_ns`, exactly as before the cutover |
| AL-S7 | **Run 2026-09-02: `scripts/sim.sh` green, 190 runs, 0 failures** — the full committed corpus plus four date-derived fresh seeds, across `clean`/`sync-crash`/`crash`, with and without injected device faults, over all three value profiles, plus the advisory-feature pairing per seed. **No seed needed fixing**, and the reason is worth recording rather than reading as luck: the harness has always modelled one log (`sim/instance.hpp` holds a single `MemoryLogDevice`), so the cutover moved it from "one device, per-core rules" to "one device, one-stream rules" — the topology now comes from the volume it bootstraps rather than being assumed, which is the one line that changed. Its instance is single-core, so the fold is an identity, the warm-up is satisfied by the first publish, and the redo/undo stamp suppression is a no-op there; what the corpus does exercise is the crash-at-op contract against the new mount pass, which is the property AL-R5 changed. **Also fixed here, and it is a miss rather than this stage's own work**: the AL-S1c review named *two* sites for the assertion-scan floor and AL-7c's fix reached only the peer's. Core 0 is not exempt from the reason — slot 0 holds the fold, so its `checkpoint_lsn` belongs to whichever core had the lowest redo start and can sit past core 0's own snapshot exactly as it can past a peer's. Both sites now take `redo_start_lsn` under one stream. **Suite: 3249/3249** (`ctest -j8`, Debug) |
| AL-S9 | **Built 2026-09-03** on `worktree-v3.0.0-arch-revision`, and the review found more than the pass did. `wal.md` §3 is rewritten as *One Stream Per Instance* with the per-core topology kept as the **other branch a volume can be in** rather than deleted — a pre-M0 image still says `kPerCoreStreams` and every old rule still governs it. `rules.md` §3 no longer says shared-nothing: core-local is the default and shared state is **declared**, in a table of three rows that indexes the owning specs rather than being the authority — the WAL stream, the reactor wake flag, and the data file's capacity. `sched.md` §9-2 names the three locks a reactor thread can take and their order; `page-lsn-cross-stream.md`'s superseded table now says **latch**, not atomic. `cross-owner-txn.md` §2c states the in-stream resolution, `crosscore.md` CC3/CC7/CC10/CC11 stop saying *stream* where they mean *owner*, and `client-manual.md`, `manual/server/server.md`, `kds.conf.sample` and `CLAUDE.md` follow. `bench/` reopens with AL-R8 ratified. **Suite: 3252/3252** (`ctest -j8`, Debug). Overhead not measured (the interleaved A/B is suspended by operator decision) |
| AL-S9 review | **Applied 2026-09-03**, and it is the sharpest of the milestone: four statements the source flatly refutes, eleven surviving per-core-stream sentences across seven files (two of them contradicting paragraphs the same diff wrote), one **live code bug**, and four bloat cuts. AL-7e is the record. **And its fix broke 123 cells at the next suite run**, which is the finding rather than the accident: `CoreRuntimeTest` bootstraps a single-stream volume and models a peer under per-core streams — a combination no instance can be in — and it only ran because two sources both said per-core and neither was the volume. Every refusal was correct. Fixed narrowly at `ef65eba`, filed open at `docs/inflight/bugs/core-runtime-fixture-models-per-core-streams.md`, and the rebuild is AM-S0(b) |
| AL-S8 | **In progress 2026-09-03**: the instrument first. `WalStats::ring_full_drains` has been counted since the first drain path and `SHOW META` never printed it, so the stage's *`wal_ring_full` at eight writers* cell had no instrument at all. Surfaced as **two** fields rather than one, because the drain loop hides two events: `wal_ring_full` is a stall the appender paid a flush for and got through, `wal_ring_full_refusals` an append that exhausted `kRingDrainAttempts` and was refused `OutOfSpace` — a counter that did not exist, because AL-R1's bound is what made the case reachable and nothing counted it. The refusal path is **proved to stay at zero where it must and is unproved where it fires**: it needs an append to lose the drained space to another core four times running, which cannot be forced from one thread, and the test file and the manual both say so rather than implying coverage. `build-release` rebuilt, the binary hashed into the run directory per `bench/README.md` rule 3. **Suite: 3252/3252**. The measurement itself is not run and no number is claimed |
| AL-S8..S9 | superseded by the two rows above |

## AL-7d — AL-S5 and AL-S6's review record

**Applied 2026-09-03** on `worktree-v3.0.0-arch-revision`. The review found
**no correctness bug in the code** — both stages behave as their rows claim —
and every finding it raised was against the *reasoning* attached to that
behaviour. That is not a soft result here: AL-S5's whole content is a
decision to skip a line, and AL-S6's largest piece is a decision not to take
a branch. A wrong reason recorded next to either is what makes the next
person delete it.

**The load-bearing finding, and the one worth the stage.** AL-S6's row and
its comment both stated the drain's free-tick test as a principle — *the
test is this core's own work, not the log's* — and that principle, applied
to core 0, is a durability bug. On core 0 `appended_lsn()` is the shared
stream's, so it includes every peer's staged bytes, and that is now the
**only** thing bounding a peer's `relaxed` loss window: peers no longer tick
the interval, so if core 0 took the early-out while a peer had bytes
unflushed, that peer's data would sit unbounded. It does not, because the
test is over the instance. The asymmetry is the design. Nothing in the suite
would have failed had someone "fixed" it — `src/wal/manager.cpp` now says so
in the place a symmetry argument would be made.

**The second finding is a correction of an over-claim CLA made.** AL-S6's
row said a peer's `wal_syncs`, `wal_interval_syncs` **and**
`wal_sync_failures` are all 0. The third is false: a peer's flush of the
shared ring can fail, and so can its wait on core 0's writer, and both are
counted on the peer — a failed wait being counted on core 0 as well, which
makes it the one field where two cores report one event. `SHOW META`'s
reading rule needed the exception stated, not the claim repeated.

**The third re-proves AL-S5 rather than changing it.** The erase's licence is
rule 1(a)'s flush, not the identity of whoever logged the handoff; a flush
covers one core's page store, and under one stream the erase would speak for
every core's records. The row above carries the amended proof and names the
case: the re-delivery path in `relation_grant_service.cpp` re-runs the
publish and appends a handoff from core 0 for a page a peer holds dirty and
unflushed — erase it and redo's not-dirty filter skips that peer's record.
A lost update, not slow work.

**The rest were documents disagreeing with the code**, each fixed where it
sat: `analysis.hpp`'s two contract comments (`dirty_pages` still described an
unconditional erase; `AnalysisStart::single_stream` still listed redo as its
only reader), `core_runtime.cpp`'s justification of an erase that no longer
runs, and `manager.cpp`'s account of what a peer's interval tick cost —
"N-1 redundant `fdatasync`es" is wrong in both directions, since on an idle
instance `WalWriter::RequestSync` returns early past the watermark (cost: one
shared-latch acquisition and a no-op flush) while on a busy one the peer's
flush frequently carried **core 0's** staged bytes, charging them to that
peer and forcing syncs above the coalescing a single drain exists to get.

**One finding was applied in a different form than proposed**: the review
asked for the `SHOW META` ternary to be kept and commented; CLA replaced it
with a plain `if`, which is the file's own idiom and needs no comment.

**One test was sharpened rather than added.** AL-S5's new cell asserted the
recLSN with `EXPECT_LT(seeded->second, end_lsn)`, which passes for the
handoff's LSN as readily as the giver's. It now pins the giver's LSN by
value, which is the property the stage exists to hold.

**Suite: 3249/3249** (`ctest -j8`, Debug). Overhead not measured — the
interleaved A/B is suspended by operator decision.

## AL-7e — AL-S9's review record

**Applied 2026-09-03** on `worktree-v3.0.0-arch-revision`. The review's own
summary is the fair one: *the technical content is right — every
load-bearing claim about the fold, recovery, the resolver, the stamp and
the counters checks out — but the pass shipped four statements the source
flatly refutes and missed eleven surviving per-core-stream sentences across
seven files.* It fixed all fifteen itself. What follows is what CLA decided
on top of that.

**The four false statements, because the pattern in them is worth naming.**
Each was CLA writing what the design *should* have been rather than reading
what it is: that the record copy happens outside the latch (it does not —
`WalStream::Append` holds it through `EncodeRecord`, and this diff's own
`page-lsn-cross-stream.md` line says the reserve/copy/publish split was
tried and abandoned); that one thread issues every `fdatasync` (two do —
core 0's reactor for anything anyone is parked on, the writer for D3's tick
and a peer's request); and twice that the WAL latch is *the one lock on a
reactor thread* when there are three (`FileLogDevice::segments_mutex_`
under it, `WalWriter::mutex_` with it released). A prose stage is exactly
where this happens, and the guard is the review reading source rather than
the diff.

**The live code bug, which the prose is how CLA found.**
`manual/server/server.md` now claims `SHOW META`'s `wal_topology` reports
the volume's topology — and on a peer it did not. A peer's `superblock_` is
a default-constructed copy and **zero is a legal value of that field**, so a
peer answered `wal_topology=per-core` on a single-stream volume and printed
`wal_anchor_count` beside it: wrong rather than absent, reachable under
`peer_listeners = on`. The header at `superblock.hpp` had predicted this
exact trap and named the fix; AL-S1c carried `log_topology` on
`CoreRuntime::Config` and never applied it to the copy. Fixed with a
`SetLogTopology` beside the `next_trx_id` copy it is the twin of, and
pinned by a cell that **fails without the fix** — checked, not assumed.

**The highest-value decision the review left open, taken here.**
`rules.md` §3 said the declared shared list "is short and is the whole of
it: the WAL", which as written brands two live structures as defects: the
reactor's sleep flag and `Waker` (declared, but in `sched.md` §7), and the
data-file `PageDevice`, whose capacity core 0 grows and every core's store
reads unsynchronized. The list is now a **three-row table that indexes the
owning specs rather than being the authority**, and the device's row is
paid for: `page_device.hpp`'s "a PageDevice instance is owned by one core"
was false and had been since the store went per core, so it now says what
makes an unsynchronized `uint32_t` sound there — core 0 alone writes and
capacity only rises. Three rows is not a small number for an engine that
called itself shared-nothing a week ago; writing them down is what makes
the fourth an argument instead of a discovery.

**Accepted without change**: the CC11 scoping (its "no subsystem header
carries an acquisition order" now says *for these three*, since
`wal/stream.hpp` carries one over a different structure), and all four
bloat cuts — the snapshot argument was written twice in two files, the
retention rule twice, `page.md` restated `wal.md` §3's handoff bullet
immediately after citing it, and one paragraph restated the two sentences
above it.

**Nothing was declined.** Every finding was either applied as written or
applied with the decision above made explicit.

**Suite: 3252/3252** (`ctest -j8`, Debug). Overhead not measured — the
interleaved A/B is suspended by operator decision.

## AL-7c — AL-S1c's review record

**Reviewed 2026-09-02** (`critics-developer`), and it found the worst
defect of the milestone. It also verified independently what the cutover
rests on: the lock ordering with the whole live set in play (no path holds
the stream latch across a ring send, a park or a store operation), the
watermark under two flushers, the group-commit bookkeeping across cores,
the prepare floor surviving the fold, the crash path end to end, and that
reverse-order destruction tears peers down before the stream they borrow.

Three findings applied, all live on the default multi-core path:

| finding | what was done |
|---|---|
| **A peer dereferences a null `log_device_`** — the cutover made it null on the attached branch, and the assertion resume was still handed `*log_device_`. A `cores > 1` volume with one assertion **segfaults at mount, deterministically**. Nothing caught it because the resume short-circuits on an empty assertion list and no test opened a single-stream peer that had one | the device now comes from the stream, which answers correctly in both topologies; and the cell that would have caught it exists |
| **The assertion scan's floor was the fold's `checkpoint_lsn`** — a *third* conflation in that call, after the `owner_core`/`stream_core` pair. The fold selects on `redo_start_lsn`, so the record it carries belongs to whichever core had the lowest one, and its `checkpoint_lsn` can sit past an idle core's own snapshot. Missing the base is not a slow scan: it is `NoteUnenforceable` for every assertion that core owns | under one stream the scan starts at `redo_start_lsn`, the field the fold does bound — at or below every core's redo start, which is at or below every core's own `CHECKPOINT_BEGIN` |
| **`cores = 1` armed the latch.** `single_stream()` is true of every new database including the shipped single-core default, so a mutex landed on every logged page mutation for a section no second thread can reach — retiring AR0's G2 silently, which `latch.hpp` calls a property of the code rather than of a build flag | one conjunct on `core_count() > 1` |

Two test findings applied. The anchor cell had become a **tautology**: the
stand-in core-0 publish tied the peer at 4096 and won the tie (strict `<`,
ascending), so slot 0 carried the test's own synthetic record and the
peer's anchor never reached the superblock the test is named for. It now
publishes above the peer and pins the winner by identity. And the other
test's `1u << 30` stand-in was inert but a landmine — an anchor no core of
this volume could publish, which if it ever won the fold would refuse the
mount through `Analyze`'s honesty check rather than fail an assertion; it
is now just above the peer's own append point.

**Recorded, not yet done.** `wal_syncs` is structurally 0 on a peer while
`wal_interval_syncs` counts, so the block's own subtraction rule
underflows — AL-S6's. An idle peer's drain no longer takes its early-out,
because `appended_lsn()` is now the instance's, so every peer issues a
sync request per D3 interval with nothing of its own staged — AL-S6's, and
AL-S8 would otherwise measure it as noise it cannot attribute. Core 0's
pool keeps clean frames for peer-owned pages after the mount pass, with no
reachable stale read today. And **nothing in the tree constructs an
`Expeditor`** — every server-level test hand-assembles the shape `Serve`
uses, which is the gap both live defects sat in.

## AL-7b — AL-S4b's review record (in progress)

**Reviewed 2026-09-02** (`critics-developer`) against the two pieces built
then. It confirmed the skip is safe on every point that worried CLA — the
peer's ceiling check is vacuous rather than inverted; nothing after the
skip depends on recovery having run; losing a peer's high-water repair
costs nothing, because a leased store draws from its lease and never
consults the floor; analysis already folds several cores' checkpoints
correctly, `emplace` being first-wins; and core 0's completion checkpoint
is ordered before the first peer is built, which is what makes the whole
design work.

**And it found the defect that gated the stage.** AL-S4b had stopped redo
restamping a peer's page, and left **undo** doing exactly the same thing
one phase later: every compensation reaches the store through the ordinary
mutation path, whose `StampPageLsn` writes the stream stamp
unconditionally. The failing case is an ordinary crash — a peer's
uncommitted insert. Core 0 rolls it back, stamps the page 1, and at the
next mount core 2 faults its own page, is granted nothing, and **can never
write it again**: a heap data page is in no relation write grant and the
extent lease is drawn fresh each mount. Fixed at the store, where every
writer passes, rather than at either phase: `SetStampSuppressed` withholds
the claim for the length of a pass that is recovering on every core's
behalf, the page_lsn still being stamped because idempotence is that
field's job and is not ownership. The mount sets it under an RAII guard, so
a refused mount leaves the store as it found it.

**Owed, and named rather than deferred silently:**

- ~~A page redo *creates* ends unstamped~~ — **closed 2026-09-02**, by
  the proposal as stated: `PAGE_INIT` names the core that logged it, in
  the same envelope byte AL-S4a used and with the same no-format-event
  argument, and `ApplyPageInit` stamps the page it formats for that core
  rather than for the recovering one. The two alternatives stay rejected
  and recorded: a server-layer post-redo sweep keyed on
  `sys.tables.owner_core` puts the catalog inside recovery, and having the
  owner log an acquisition when it meets an unstamped page puts a write on
  the fault path. The stamp is now written **inside** `ApplyPageInit`
  rather than by redo's tail, because every format it dispatches to clears
  the header's flags word; under per-core streams the record's core and the
  replaying core are the same, so that path is unchanged. `CheckpointCoreOf`
  became `LoggingCoreOf` in the same edit — one name for "which core
  appended this record", rather than a second name arriving with the second
  record type that needs it.
- ~~`ResumeAssertionsAfterRecovery` is the per-core log scan AL-R5 did not
  remove~~ — **closed 2026-09-02**. It was right to sit outside the
  topology branch: core 0 counts a peer's assertions as foreign and skips
  them, so only the peer can resume its own. What was wrong is that one
  parameter answered two questions. **`owner_core`** is whose *relations*
  these are, the filter that makes a core adopt its own and count everyone
  else's as foreign; it is the calling core's under every topology.
  **`stream_core`** is whose *log* is being scanned, which the scanner
  validates every segment header against. Under per-core streams they are
  one number, which is why they were one parameter; under one stream a
  peer's assertions are the peer's and the log is stream 0's, so passing
  the peer's id as both would have refused the scan on the segment header,
  marked every one of that peer's assertions unenforceable, and stopped its
  writes to every relation they cover. The peer also now receives **slot
  0's anchor** rather than its own, which is all zeros under one stream and
  would have sent the scan to the head of the whole log at every mount.

- ~~No cell sets `log_topology = kSingleStream` on a `CoreRuntime`~~ and
  ~~`timings.timed` is false on a skipped peer~~ — **both closed
  2026-09-02**, together, because the second is only visible through the
  first. The skip is now exercised as a **third open of the existing
  restart test**, differing from the second only in what the volume says
  its log is: the peer recovers nothing (`records == 0`, against a
  clean-stop control that reads the checkpoint's own handful) and still
  serves its 200 rows. Folding it into that test rather than writing a
  standalone one was not only economy — the first attempt at a standalone
  cell had the peer issue `CREATE TABLE`, which a peer refuses, and the
  existing fixture is the thing that knows how to place a relation on core
  1 from core 0's catalog and fund the peer for it. The skip arm now also
  sets `timings.timed`, because that core measures a completion checkpoint
  at `AttachTransport` even when it recovered nothing, and `SHOW META`
  prints the whole `_us` block only where a clock was supplied — so leaving
  it false hid a number that had been taken. ~~One `SHOW META` recovery block~~ and ~~the last peer-slot reader~~ —
  **both closed 2026-09-02**. AL-R5's "one block on core 0" turned out not
  to mean deleting a peer's: a peer's block reading zero is the truth and
  worth printing. What was missing is *why* it is zero, since an operator
  cannot otherwise tell "this core did not scan" from "this core scanned an
  empty log" — so `MountRecovery::ran` is false on a skipped peer and the
  block carries `recovery_by=core0`, printed only where the pass was
  somebody else's, the rule the cross-owner three already keep. And
  `RecoverCoreAtMount` no longer builds a `CoordinatorStreamResolver` under
  one stream: `RecoverCore` takes the in-stream branch and would never call
  it, so constructing one is harmless and misleading, and it was the last
  reader of the per-core `anchors` vector — under one stream nothing
  consults a peer's anchor slot at all. Remaining smaller items: `AnalysisStart::single_stream` is read only by
  `RecoverCore` and misleads on `Analyze`'s parameter struct;
  `RecoverCoreAtMount` is at eleven parameters and wants the options object
  that retires `wal_dir`/`anchors` too; and core 0's pool now holds clean
  frames for peer-owned pages after the pass.

## AL-7a — AL-S3's review record

**Reviewed 2026-09-02** (`critics-developer`). **No incorrect advance of
the anchor found**: it verified that the prepare floor is preserved
structurally (each core's redo start is floored in `Checkpointer::Start`
before publish, so a minimum over floored values is floored — the fold
would have to become a *maximum* to break it); that `mount_anchor_` is
captured after recovery and before the completion checkpoint, so it is
exactly what recovery used; that `per_core_` is not raced, because all
three `Publish` callers are core 0's reactor or a post-join shutdown
thread; that whole-record-from-the-minimum is right, with `segment_no`
the proof, since it is `redo_start / segment_size` **of the same core**
and a field-wise minimum could name a segment that does not hold the
LSN; and that the test fixture's byte-patching is a real decode, the
superblock body carrying no checksum.

Six findings applied:

| finding | what was done |
|---|---|
| The warm-up gate counted map entries, which answers "how many anchors arrived", not "which cores have published" — and nothing bounds `core_id` on the way in (the ring path memcpys it out of a peer's payload). One out-of-range id would release the warm-up early and put a phantom core into the minimum | the state is a fixed array plus a `std::uint64_t` published mask, so the gate is `popcount(published_) == core_count` — a fact the type enforces; and `Publish` refuses `core_id >= core_count` under one stream |
| The warm-up is load-bearing for the **prepare floor**, not only for dirty pages: a peer with a live `TXN_PREPARE` that has not checkpointed contributes nothing to the minimum | stated in the header, so that optimising the warm-up away has to answer that case too |
| The warm-up is unbounded when `checkpoint_interval_ns` is 0, since no cadence tick is armed | the header says so rather than claiming "one checkpoint per mount" |
| `per_core()` exposed a `std::map` in the API for two `.size()` calls | `folded_cores()` |
| `UnderPerCoreStreamsNothingIsFolded` duplicated an existing cell for one extra assertion | cut; the assertion moved into `StreamsFromDifferentCoresDoNotOverwriteEachOther` |
| No cell for a failed sync leaving the map ahead of the page — a state the fold created | added: core 3's publish fails its sync, republishes higher, and the fold still lands on the true minimum |

**One finding was applied and then reverted, because building it proved
it wrong.** The review proposed taking the mount floor as the minimum
over *every* anchor slot rather than slot 0, to harden against a volume
converted in place. Under one stream slots 1..63 are never written, so
that minimum is 0 on every mount — which would pin the warm-up at
replay-the-whole-log **and write that zero over the real anchor**. The
existing warm-up cell failed immediately. The floor now takes the lowest
slot that was ever *published into* (`redo_start_lsn != 0`, which
`superblock.hpp` already defines as "never checkpointed"), which keeps
the review's hardening and drops its defect, and a new cell pins it.

Two findings declined. The `lowest == nullptr` branch in `FoldedAnchor`
is provably dead and stays as a guard rather than a dereference. And the
three hand-written conversions among the structs holding these four LSNs
are real duplication, but collapsing them touches the ring payload path
and belongs to AL-S4, which is named there rather than done here.

## AL-7 — AL-S0's review record

**The AL-S1a/S1b code, reviewed 2026-09-02** (`critics-developer`,
against the post-S1b image). **No live durability, race or deadlock
defect**, and it verified several things a reader would reasonably
doubt: the `flushed_lsn_` invariant holds on all ten paths including the
`OutOfSpace` return and a failed device write; no lock cycle exists, the
device lock being innermost everywhere and the attached `Sync` correctly
dropping the stream latch before its condition-variable wait (holding it
there would hang the instance); the strict-vs-non-strict `IsDurable`
mismatch between `WalDurability` and `WalWriter` produces no off-by-one
on the attached path; `ResolveBatches` cannot double-count and its new
call is provably unreachable on an unshared owning manager, so
`cores = 1` behaviour is unchanged; the drain's D3 refactor is
behaviour-identical on the owning path; no relaxed atomic sits in a loop
condition or body; and the golden log is a real byte pin, deterministic
because the record encoder zeroes its alignment padding and the
simulator's clock never advances, so the D3 interval never fires.

Six findings acted on:

| finding | what was done |
|---|---|
| The latch is held across `CreateSegment` — `posix_fallocate`, a full-segment prewrite and two `fsync`s — while `spin_latch.hpp` justified itself as guarding nanoseconds. Against a holder in `fsync`, `sched_yield` returns immediately and N−1 pinned reactors burn a core each | `spin_latch.hpp` **deleted**; `base/latch.hpp` is a `std::mutex` behind the same null-pointer compile-out, so `cores = 1` is unchanged and a waiter sleeps. AL-R1 restated |
| `EveryThreadsRecordLandsAtTheLsnItWasGiven` never filled the ring and never rolled: 232 KiB against a 1 MiB ring and segment, so the concurrent flush ran once after the join and the roll never ran at all | volume raised to ~4 MiB and the ring sized to the 64 KiB **minimum** — a ring as large as a segment can never fill, because the roll drains it first. Two `EXPECT_GT`s now fail the cell if either path stops being reached |
| `WalStream::Sync`'s correctness silently depends on the device syncing **every** open segment, since a roll can land between the capture and the sync | named at both ends: `file_log_device.cpp`'s `Sync` says why it must not be narrowed to the tail, and `stream.hpp` states the dependency |
| `stats_.flushes` under-reported on a peer — the attached `Sync` and `RequestSyncNow` call `stream_->Flush()` directly | both count their own flush; `DrainOnce`'s D3 branch no longer counts one for them |
| `Attach` could not tell that the writer syncs the stream's device; the header stated it as a caller obligation | `WalWriter::device()` added and `Attach` refuses the mismatch, naming the wait that would never end |
| `MemoryLogDevice`'s injection setters mutated state the mutex now protects; `FileLogDevice`'s header still declared itself core-local | the four setters take the lock; the header states what it actually satisfies |

Two left as they are, with reasons. The `*Locked` suffix on two of five
private methods was replaced by one line saying **every** private method
runs latched and none may call a public one, which is shorter than the
comment it replaces and covers all five. The review's remaining item —
that `RequestSyncNow` and the attached `Sync` share three lines — is
declined: they read as two named policies, and a `bool wait` parameter
would make both call sites worse.



**AL-S0's documents, reviewed 2026-09-02** (`critics-developer`, ~210
citations re-opened at `d15b5ac`). Ten were wrong or overstated and the
review fixed each in place: `device_page_store.cpp`'s stamp is on the
mutation path in `StampPageLsn`, not at flush; the ring's payload bound
is `kCoreRingPayloadBytes = 1024` at `ring_transport.hpp:224`, not
anything in `ring_message.hpp`; §8b attributes the unaccounted time to
`fdatasync` **above all** rather than wholly; four line ranges off by a
few; "the four files" for a five-file row; and RW-C1 is *described* at
one site while three later lists name it bare. Nine findings were left
for CLA, of which this revision applies six:

| finding | what was done |
|---|---|
| AL-R1's `fetch_add` reservation does not survive `Append`'s roll and `OutOfSpace` paths, and the implementation chose a latch | **AL-R1 rewritten** to the latch, naming the three things a bare reservation cannot express and the flush-under-latch cost |
| AL-R2 ("formats do not change") contradicts AL-R4 (`core_id` on the checkpoint records) | AL-R2 narrowed to the record *header* and the segment format; the payload change named |
| Nobody folds `core_id → 0` between AL-R3's refusal and AL-R4's per-core anchors | named: core 0 keeps an in-memory per-core table and writes slot 0 with the minimum; `SetWalAnchor` stays strict |
| AL-2's "one appender" contradicts AL-R1 | "one flush and sync point" |
| AR0-V4 said AR0-3 needs a global trx-id counter; ids are already instance-unique | corrected to the missing thing being commit **order**, with §1/§3's actual rejection cited |
| `CLAUDE.md` made AL-R8 the gate reopening `bench/` and `docs/inflight/` | the operator is the gate; AL-R8 carries CLA's proposal |

Two findings are **declined for now**, with reasons: AL-R1's "the
watermark being an atomic it already reads" is fixed rather than
declined (it now says a peer starts reading one at M0), and AL-3 E's
`txn_2pc_service.hpp:66-95` citation stays, because that section's job
is to enumerate per-core-stream *assumptions* and those lines carry one
("keeps every stream's ids stream-local") even though they do not carry
the sentence they sat beside. **AL-R7's golden log does not cover
AL-R6's stamp change**, which is a real gap: the cell compares log bytes
and the superblock, not the data file, so a page left at stamp 0 by the
removed restamp would pass it. AL-S5 owes that cell, and its row now
says so.

The review's bloat findings (AL-1, AL-2's framing paragraphs, AL-R7/R8
duplication, ~30 lines) are **not** applied: the document is an
instruction that will be read once per stage by someone who did not
write it, and the duplication it names is between a ruling and the stage
that enacts it, which is where a reader looks. Recorded rather than
silently dropped.
