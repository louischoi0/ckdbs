# WAL recovery — workplan

Status: **RC01-RC04 and RC04a written (unbuilt); RC05 onward not started.**
§4's assertion-replay decision is **answered** (AS6a), so RC07 is unblocked;
§4a records that `origin/main`'s `EXPLICIT` key mode **amended RV6**. Spec: `docs/wal.md` §12 (normative, and
still `[PROPOSED]` — this plan proposes the amendments §12 needs and does
not make them). Related: `docs/txn.md` §§3, 6, 8, `docs/page.md` §§2, 8,
10, `docs/keystoneid-invariant.md` K-M2a, `docs/feat-assertion.md` §7,
`docs/workplan-testing.md` (SIM04/SIM11 — the acceptance tests, already
written and gated off), `docs/known-gaps.md` (this is its first item).

Decisions are `RV1`-`RV9`, tasks `RC01`-`RC10`. A fresh prefix on purpose:
`P`, `R`, `T`, `V`, `M`, `K-M` and a dozen others are taken and `CLAUDE.md`
warns that a bare number is ambiguous — **cite the file, not the number.**

**Recovery is the engine's largest single gap and the one every other gap
waits behind.** `docs/known-gaps.md` lists five entries under durability;
four of them close, or become closable, with this work. It is also the one
place the engine has said, in three separate documents, *do not ship half
of it* (`docs/txn.md` §8, `docs/feat-index.md` §13, and §12 itself). This
plan is written to make the whole of it a sequence rather than a leap.

---

## 0. What this is, and is not

**Is:** per-core analysis / redo / undo over the existing log, ending in a
checkpoint, so that a restart after a crash yields exactly the
acknowledged-commit state — `docs/wal.md` §1's "no acknowledged loss"
promise, which today is a claim the engine cannot keep.

**Is not:** point-in-time recovery, archiving or replication (`wal.md` §13
— the log is *designed* to be sufficient for them and none is in scope);
purge and `SnapshotTooOld` (`txn.md` §9 — recovery unblocks them, it does
not build them); the free-map and `ALLOC`/`FREE` (`page.md` §5's
SpaceManager is unbuilt, and §14's growth ordering has nothing to replay);
cross-core commit (`wal.md` §3's 2PC stays `[OPEN]`); logged catalog
writes, which are a **prerequisite of a promise this plan bounds** (RV3)
rather than a task in it.

## 1. What already exists — the survey this plan turned on

`known-gaps.md` says "nothing reads the log back", which is true and reads
as if the substrate were missing too. It is not. Verified against the tree
at `6fccc08`:

| Piece | Where | State |
|---|---|---|
| Record header + CRC + `DecodeRecord` | `include/kds/wal/record.hpp` | built |
| **Forward scan with torn-tail detection** | `wal::RecordReader`, `Next()` / `end_lsn()` / `stopped_early()` | built — §4.2's rule is code |
| LSN → `(segment_no, offset)` | `wal/stream.hpp`, pure arithmetic | built |
| Segment read-back | `LogDevice::ReadAt`, real and in-memory | built |
| **`CHECKPOINT_BEGIN` carrying the active-txn table *and* the dirty-page table with recLSNs** | `EncodeCheckpointBegin` / `DecodeCheckpointBegin` | built |
| `CHECKPOINT_END` carrying `redo_start_lsn` | `CheckpointEndPayload` | built |
| **Per-core superblock anchor** — `checkpoint_lsn`, `redo_start_lsn`, `durable_lsn`, `segment_no` | `WalAnchorFields`, 64 slots | built |
| Redo gate field | `GetPageLsn` / `SetPageLsn`, common header offset 8 | built |
| FPI, and the checksum that detects what it heals | `kFullPageImage`, `page.md` §10 | built |
| Undo pages, records, `undo_ptr`, `UndoLog::Walk` | `txn/undo_page.hpp`, `undo_log.hpp` | built |
| Compensations emitted as **ordinary logged mutations** | `TransactionManager::Abort` (`txn.md` §6, written this way *so recovery reuses it*) | built |
| Assertion record replay | `exec::ReplayAssertionRecord`, explicitly "correct for whatever record range recovery eventually feeds it" | built |
| The acceptance tests | `workplan-testing.md` SIM04/SIM11, written and `[GATED: recovery]` | built, skipped |

**So this is not a from-scratch subsystem.** Analysis has its inputs
durable and decodable, redo has its gate, undo has its chains, and the
oracle that judges the result was written before the feature deliberately
(SIM's "recovery written without an adversarial harness waiting for it
would be tested by its own author's imagination").

## 2. What does not exist — and one of them is structural

Four gaps. The fourth is the one that changes the shape of the work, and
no document currently lists it as a recovery blocker.

1. **A reader that spans segments from an LSN.** `RecordReader` reads one
   buffer; nothing assembles the buffers, follows a segment roll, or stops
   at the durable end across files. Small, and RC01.
2. **The three phases**, and an entry point that runs them before the
   server accepts a connection. RC02-RC05.
3. **Per-record redo appliers.** Every heap/index/var-heap/assertion
   record needs an "apply to this page" function that is idempotent under
   the `page_lsn` gate. The mutations themselves exist as write-path code;
   what does not exist is the same effect driven from a decoded record.
4. **A durable record of an INSERT — the structural one.**
   `TransactionManager::Abort` walks an **in-memory** trail
   (`TrailEntry`), and `UndoRecordType::kInsert` is *defined and never
   written* (`txn.md` §3.6, deliberately, to keep the insert path's cost
   unchanged). After a crash the trail is gone. **So a loser
   transaction's INSERTs have nothing durable to roll back from**, and the
   undo phase cannot be written against today's format without either
   writing that record or deriving the same fact another way.

   `txn.md` §3.6 anticipated exactly this — *"so that persisting the
   insert trail, which recovery-driven rollback will need, is a code
   change and not a format-version event"* — which is why it is a task
   here (RC06) and not a redesign. What it is not is free: it puts an
   undo append on the INSERT hot path, which §3.6 avoided on purpose, and
   `bench/results-keystone-alloc.md`'s lesson applies — **measure before
   and after, in `build-release`.**

## 3. Decision record — what this plan proposes

| ID | Decision |
|----|----------|
| RV1 | **Recovery runs at mount, before the listener binds**, per core, and a failure refuses the mount rather than serving a partial database. `Expeditor` gains the phase between store open and `Serve()`. No statement path can observe a half-recovered instance because none is accepted. |
| RV2 | **Per-core and independent**, as `wal.md` §12 says — valid exactly while transactions are core-local (`CC3`), which they are. Each stream recovers from its own anchor. Cross-stream ordering is not required and must not be introduced; guideline 3 of `workplan-crosscore.md` is the standing rule. |
| RV3 | **v1 recovers data, not the catalog.** Catalog and DDL writes are unlogged (`txn.md` §7), so a crash still loses a `CREATE TABLE` and still leaves `sys.tables.next_id` behind the log (K1). Recovery must therefore state its promise as *"every acknowledged commit to a relation that survived is restored"* — and **RC09 must add the counter that says when it did not**, rather than letting the gap read as closed. Logged catalog writes are K-M2a's and stay there. |
| RV4 | **A hazard RV3 creates, named so it is not discovered:** the superblock's high-water mark is unlogged too, so a crash can revert it while the log still names pages above it. A later allocation could then hand out a page redo has already written. Recovery **must raise the high-water mark to the maximum page id any replayed record names** before the store serves an allocation. Cheap, and it is the difference between a leak and corruption. |
| RV5 | **Redo is idempotent through `page_lsn` only** (`wal.md` §9): replay iff `record.lsn > page_lsn`. No second mechanism, no per-record sequence numbers. A headerless page is never a replay target (`page.md` §1), which is what keeps Waystone out of recovery entirely. |
| RV6 | **Undo reuses the abort path verbatim**, as `txn.md` §6 was written to allow: a loser's compensations are ordinary logged mutations, so undo is crash-restartable by RV5's gate alone and needs no undo-next pointer in v1. If a measurement or a proof says otherwise, a CLR is a format-version event and its own decision. **Amended 2026-08-11 — "verbatim" no longer holds; see §4a.** |
| RV7 | **Advisory structures are rebuilt, never replayed.** Waystone pages are unlogged and correct when empty (invariant 8); Observational Cabins declare every value unobserved (`feat-cabin.md` §9); access statistics resume. Recovery touches none of them, and the contract suites are what prove that costs no result. |
| RV8 | **Bound Cabins are replayed**, because an assertion is authoritative and `feat-assertion.md` §7 promises enforcement at restart *with no gap*. `exec::ReplayAssertionRecord` is the fold; what it needs and does not have is a **record range** — see §4. |
| RV9 | **The gate flips once, in the harness.** SIM04/SIM11's `[GATED: recovery]` assertions are the acceptance criteria; RC10 enables them and the documented-gap counters' expected values become zero. A recovery whose own tests are written by this workplan rather than inherited from SIM is a recovery graded by its author. |

## 4a. RV6 amended: the abort path grew a parameter recovery cannot supply

**Found 2026-08-11, merging `origin/main` into the recovery line.** The
`EXPLICIT` key mode landed on main, and with it btree **leaf division** —
which moves half a leaf's tuples to another page and renumbers the slots of
the ones that stay. A recorded `(page_id, slot)` is therefore no longer a
stable address for the life of a row.

`TransactionManager::Abort` was changed for it, and correctly:
`Compensate` now reads the pk at `(page_id, slot)` and compares it against
`TrailEntry::pk` before writing a byte, and on a mismatch either asks an
installed `RowLocator(rel_oid, pk)` for the row's current address or fails
`Corruption`. Main's own words for why: compensating blindly "is not a
failed rollback, it is a rollback that corrupts a row it never wrote."

**Recovery cannot make that check as the format stands.** It walks the
*durable* chain, not the in-memory trail, and `txn::UndoRecordFields`
carries `target_page_id`, `target_slot`, `type` and the two chain links —
**no `pk` and no `rel_oid`.** So RC05 can supply neither half: not the
identity to check, and not the `(rel_oid, pk)` a locator needs. This is the
same shape as RC03's `UNDO_WRITE` blocker — a fact that lives on the page
and nowhere in the log — arriving from a different direction.

**Proposed resolution, and it needs no format change.** The pk is already
in the record: for `kOverwrite` and `kDelete` the before-image *is* the
prior tuple payload, and `KeystoneIdOfPayload` is what reads an id out of
one — the same call `Compensate` makes. So recovery's undo can check
identity from the image itself and, on a mismatch, take the **no-locator
branch** — refuse the mount rather than guess. That is not a degraded mode;
it is the branch main already ships as "the safe half of the same rule",
and it keeps `rel_oid` out of a format that has no room for it.

What it costs: a crash on an `EXPLICIT` relation whose leaf divided
mid-transaction refuses the mount instead of recovering. Narrow today —
`EXPLICIT` is refused on heap relations, and only `EXPLICIT` can trigger a
division mid-statement — and it fails loudly rather than corrupting.
Lifting it means putting `rel_oid` in the undo record, which is a
format-version event and RC05's to argue if the refusal proves too broad.

**RC05 must not be written as "call `Abort`".** The abort path's signature
now has a parameter with no recovery-side implementation, and the identity
check is the part that matters.

## 4. Open decisions — surface, do not decide

Each blocks a specific task and none is this plan's to settle. The house
rule applies: stop and ask, or build behind an interface that keeps every
option viable.

- ~~**The assertion replay range**~~ — **ANSWERED 2026-08-11, and no longer
  open.** The operator took the first option, amended: the checkpoint
  persists the directory as **headers only**
  (`{group_id, key, count, sum}`, O(groups)), replay folds `ASSERT_*` from
  the last checkpoint forward, and the **entry gains a `group_id`** so the
  header→entry linkage is rebuilt from the cabin's own pages instead of
  persisted. Ratified as `feat-assertion.md` **AS6a** (§2, §5.1, §7), which
  carries the full statement. RC07 is unblocked and owns it.

  Two things the decision turned on that neither option had costed, recorded
  here because they are what made the plain first option unbuildable:
  **a group header's entry-list is O(all writes, forever)**, appended by
  `BoundCabin::Apply`/`ApplyDeparture` per checked write and removed only on
  abort — so persisting the directory whole means writing O(all entries) at
  every checkpoint; and it **cannot simply be omitted**, because
  `Unapply` answers `NotFound` on a missing pair, so a reservation made
  before a checkpoint and rolled back after it would fail the mount. The
  `group_id` field is what makes the linkage reconstructible and the
  snapshot O(groups).
- **Writing `UndoRecordType::kInsert`** (§2.4): a correctness requirement
  with a hot-path cost `txn.md` §3.6 declined to pay. The alternatives —
  persisting the trail wholesale, or deriving inserts from `HEAP_INSERT`
  records during analysis — are not obviously worse and are not costed.
  RC06 builds the record; **which mechanism is the owner's call.**
- **Recovery under a changed core count** (`wal.md` §3, §15). `cores` is
  superblock-pinned and a mismatch already refuses the mount, so v1
  inherits a working refusal rather than a decision. Reassignment stays
  `[OPEN]`.
- **Undo retention and `SnapshotTooOld`** (`txn.md` §9). Recovery makes
  purge *possible* by giving readers a horizon to be registered against;
  it neither needs nor provides one.
- **The `D3` loss window's exact bound** under replay (`wal.md` §16-6).
  The class promises "bounded by the configured interval"; what recovery
  must assert is that bound, and the number is `wal.md`'s.

## 5. Tasks

Order is dependency order. Each ships with its tests in the same change,
deterministic, over `MemoryLogDevice`/`MemoryPageDevice` with injected
faults (`rules.md` §4).

**RC01 — The segment-spanning reader. WRITTEN 2026-08-10, NOT YET BUILT
OR RUN** — the environment it was written in has no C++ toolchain, so
every claim below is an argument until `scripts/test.sh` says otherwise.
`wal/log_scanner.hpp` + `src/wal/log_scanner.cpp`: `ScanLog(device,
core_id, from_lsn, visitor)` walks segments to the durable end, and
`ScanLogToEnd` is the same walk with an accept-everything visitor rather
than a second one. Built on `RecordReader`, which already answers for one
buffer.

Four decisions it made, none of them in the task's own text:

- **A `PAD` is framing and never reaches the visitor.** It seals a
  segment, so the walk continues in the next one — and it is the single
  place a scan must ignore bytes that decode cleanly.
- **A torn record ends the scan; a bad segment *header* fails it.** The
  distinction is the reliability argument: a torn tail is the expected
  shape of a crash, a segment stamped for another core means the file set
  is not the stream it claims to be, and replaying a plausible prefix of
  that is the partial recovery `txn.md` §8 forbids.
- **The visitor may veto.** Returning a non-ok Status stops the scan and
  is returned — which is how a phase above will make an unknown record
  type the hard error `wal.md` §5.2 requires, without the scanner needing
  to know the type list.
- **`ValidateSegmentHeader` is shared with `WalStream::ScanTail`**, whose
  copy is deleted. "Is this segment mine and does it start where I think
  it does" is one question, and the append path and the replay path
  answering it two ways is the drift `exec/tuple_verify.hpp` exists to
  prevent. Two tests assert the two paths agree about the durable end,
  sealed tail included.

*Done when:* a scripted multi-segment stream reads back exactly what was
appended; truncation at **every byte offset** of the last record stops
cleanly and reports `stopped_early`; a sealed-then-rolled segment boundary
is crossed with no record lost or duplicated. All three are written in
`tests/wal_log_scanner_test.cpp` and **none has been executed.**

**RC02 — Analysis. WRITTEN 2026-08-10, NOT YET BUILT OR RUN**, on RC01's
terms — no toolchain here. `wal/analysis.hpp` + `src/wal/analysis.cpp`:
`Analyze(device, core_id, {redo_start_lsn, anchor_durable_lsn})` makes one
forward pass and returns the dirty-page table, the transaction table, the
redo start, and RV4's `max_page_id`/`max_txn_id`. It reads the log and
nothing else — no page, no catalog, no write — which is what lets its
tests be scripted logs rather than crashed databases.

**This task's own text was wrong on one point and is corrected here: the
split is three ways, not two.** It said losers were "everything else,
`TXN_ABORT` included". They are not. `txn.md` §6 emits rollback's
compensations as **ordinary logged mutations** and appends `TXN_ABORT`
after them, and a stream is a durable prefix — so a durable `TXN_ABORT`
means every compensation before it is durable too, and *redo* replays
them. Undo owes that transaction nothing; running it again would be work
the `page_lsn` gate silently absorbs. `TxnOutcome` is therefore
`kWinner` / `kAborted` / `kLoser`, and only the last is undo's.

Two more things it settled:

- **The anchor's `durable_lsn` is what makes an empty scan honest.** A log
  that lost the records its anchor depends on scans to zero records —
  byte-identical to a clean shutdown right after a checkpoint. Analysis
  requires the scan to reach the durable point the anchor was published
  with, and fails `Corruption` otherwise. Without it, recovery's quietest
  failure mode is a silent empty replay onto a database that needed one:
  `txn.md` §8's partial recovery, arrived at by omission.
- **`RedoStartFrom` is shared with the checkpointer but takes the opposite
  bound**, which writing the tests is what surfaced. The checkpointer
  floors at its own `CHECKPOINT_BEGIN` LSN because its recLSNs all
  *predate* it; analysis floors at the durable end because its recLSNs all
  *follow* the scan start — flooring analysis at the scan start returns
  the scan start every time and says nothing. What is shared is §11-3's
  real rule, *skip a recLSN of 0 and take the minimum*, which is the part
  that drifts; the checkpointer's own copy of it is deleted.

*Done when:* a scripted log yields the exact winner/loser split; a log with
no checkpoint recovers from segment 0; an anchor pointing past the durable
end is `Corruption` and refuses the mount, never a silent empty recovery.
All three are in `tests/wal_analysis_test.cpp`, with the seeding path, the
recLSN-of-zero rule and the torn-tail-demotes-a-commit case beside them,
and **none has been executed.**

**RC03 — Redo. WRITTEN 2026-08-10, ALL EIGHT APPLIERS, NOT BUILT OR RUN.**
`wal/redo.hpp` + `src/wal/redo.cpp`, plus the two page primitives it
needed. All eight appliers are written. The eighth was blocked on a format
decision, which the operator took the same day — see the resolved blocker
below, and `docs/txn.md` §3.5, which this closure made true.

Two primitives had to be added, and which page classes already had one is
itself the finding: **the undo path anticipated recovery and the heap and
var-heap paths did not.** `txn::UndoPageWriteAt` already existed with
exactly the redo contract, comment included. `PageView::InsertTuple` and
`varheap::PageAppend` both *allocate* a position and return it, which is
the right contract for a writer and the wrong one for a replayer — so
`PageView::RedoWriteTuple` and `varheap::PageWriteAt` are new. Redo must
place at the position the record names because those positions are
**durably referenced elsewhere**: an undo record names
`(target_page_id, target_slot)`, and a spilled cell names
`(page_id, slot)`. A replay that appended "wherever the page ends" would
reproduce the bytes and break every one of those references. Index entries
need no primitive — their position is a function of their bytes (IX4b), so
`InsertEntry` reproduces its own slot, and the applier asserts that it did.

> **THE BLOCKER — RESOLVED 2026-08-10, operator-decided.** The chosen
> closure was the first option below: make the writer log the record's tail
> as `txn.md` §3.5 already specified. `UndoLog::LogUndoWrite` now does, via
> the one `txn::EncodeUndoRecordTail` / `DecodeUndoRecordTail` pair redo
> reads back, and `UndoWritePayload::image_len` is renamed `tail_len`
> because it now counts `12 + image_len`. No format version moved — nothing
> has ever read the log back, so no existing stream is reinterpreted; the
> same change after recovery ships would be a format event. **All eight
> appliers are written.** The original statement of the problem follows.
>
> ~~`UNDO_WRITE` cannot be redone, and closing it is a decision about a
> persisted format.~~
>
> The on-page undo record carries `target_page_id`, `target_slot` and
> `type` — how the undo phase knows *which tuple* a before-image belongs
> to. `wal::UndoWritePayload` carries only `prior_trx_id`,
> `prior_undo_ptr`, `offset` and `image_len`, and `UndoLog::LogUndoWrite`
> passes the bare before-image. **Those three fields exist on the page and
> nowhere in the log.**
>
> `txn.md` §3.5 says the payload "fits without amendment" and spells the
> mapping as `payload.image = record bytes [+16, +28 + image_len)` — the
> record's tail *including* those fields. The implementation does not do
> that. The spec and the code disagree, and the difference is exactly what
> recovery needs.
>
> Redoing it as-is would rebuild a chain whose records name page 0 slot 0,
> so **RC05's undo would roll back the wrong tuple rather than fail** —
> which is why the applier refuses loudly instead, and the mount fails
> whenever an UPDATE or DELETE is in the replay range. That is the honest
> state, and the same discipline `physical_optimizer = on` follows.
>
> Two ways to close it, and **the choice is the owner's**: make
> `LogUndoWrite` log the record's tail as §3.5 specifies, or give
> `UndoWritePayload` the three fields. The first matches the spec and
> changes no struct; the second is explicit and costs a format-version
> event. Either way `txn.md` §3.5 needs correcting, because one of its
> sentences is currently false.

Original scope, for reference:
Replay forward under RV5, `FULL_PAGE_IMAGE` first per page, restoring a
checksum-failed page from its FPI (`page.md` §10 — checksum detects, FPI
heals). One applier per record type: `HEAP_INSERT`, `HEAP_OVERWRITE`,
`HEAP_DELETE_MARK`, `SLOT_RETIRE`, `PAGE_INIT`, `UNDO_WRITE`,
`VARHEAP_APPEND`, `INDEX_INSERT`. Redo reconstructs crash-time state
**including uncommitted changes and undo pages** — that is what makes the
undo phase possible.
*Done when:* replaying twice is a no-op (the property, asserted, not
argued); a page whose `page_lsn` already exceeds a record is untouched;
every applier has a round-trip test against the write path that produced
the record. The first two are in `tests/wal_redo_test.cpp` with the
primitives' own tests beside them; the third is complete for all
eight. **Nothing has been executed.**

**RC04 — RV4's high-water repair. WRITTEN 2026-08-10, NOT BUILT OR RUN**,
on RC01's terms — still no toolchain here. `wal/high_water.hpp` +
`src/wal/high_water.cpp`: `RaiseHighWater(store, analysis)` moves the
store's allocation floor past `analysis.max_page_id` and reports the
transaction-id ceiling `analysis.max_txn_id` implies. Plus one new seam,
`storage::PageStore::RaiseAllocationFloor`, overridden by
`DevicePageStore`, `InMemoryPageStore` and `BufferPool` (which delegates,
because its allocator is delegated).

**RV4 is wrong about where the mark lives, and the correction changes what
this task had left to do.** RV4 says "the superblock's high-water mark".
The superblock holds no such thing — `server/superblock.hpp` states in as
many words that it "deliberately does not hold allocation state — no
page-id counter, no total/free page counts", and that which ids exist is
answered by `DevicePageStore`'s free-map page. The hazard survives the
correction intact, because the free map is unlogged too and a crash
between a data write-back and the map write-back reverts it (the ordering
note in `device_page_store.cpp`'s `FlushMaps` calls the result an orphan;
the log is what makes it more than one). What changes is the *scope*:

- **Redo already closes the common case.** Its `CreateAt()` re-establishes
  the free-map bit for every page it writes through a `PAGE_INIT` or an
  FPI, so a page whose records are in the replay range is protected by the
  phase before this one.
- **What it does not close, and what makes RC04 a fix rather than a
  formality, is a page the log names that redo never visits.** A
  `CHECKPOINT_BEGIN` dirty-page entry with a recLSN of 0 is
  dirty-but-described-by-no-record (§11-3): no applier ever runs for it,
  no bit is ever set, and `analysis.max_page_id` counts it correctly
  because the log named it. That is the case the headline test is built
  on, and the one the repair exists for.
- **The floor is a constant today.** `DevicePageStore::Open` starts every
  mount at the same `first_new_page_id`, which is a property of the build
  and not of the database in front of it. The repair makes allocation
  safety rest on the log rather than on the free map being complete.

Three decisions the task's own text did not make:

- **The floor is raised; no free-map bit is set.** Marking an id allocated
  that no page was ever written at would turn `Get()`'s honest `NotFound`
  into a read of whatever bytes the device holds there. "This id exists"
  and "do not hand this id out" are two facts, and recovery may only
  assert the second.
- **A store that cannot raise its floor refuses the mount.** The base
  `PageStore` default is `Unsupported`, not a silent no-op — a store that
  ignored the raise would let recovery report a repair that did not
  happen, which is worse than a refusal that names the store (RV1).
  `DevicePageStore` refuses the same way when a **lease** is installed,
  because a leased core takes its ids from its extent and never consults
  the floor.
- **The transaction-id half is computed here and applied by the caller.**
  `analysis.max_txn_id` is the other face of the same unlogged-page
  hazard (`txn/trx_id.hpp` records the exposure), but the superblock is
  `server/`'s and `wal/` sits below it — the boundary `analysis.hpp`
  already drew. `HighWaterRepair::next_trx_id` is the number; the caller
  owes `SuperBlock::SetNextTrxId`, which already refuses to lower.

**Two obligations this leaves named rather than hidden**, both for the
mount driver RC05-RC08 build:

1. `storage::ExtentAllocator` — core 0's carver of per-core page extents —
   is constructed in `expeditor.cpp` with `kFirstUserPageId` as its search
   hint. It must start above `HighWaterRepair::page_floor` instead, or a
   granted extent can cover pages the log names: the same hazard in the
   multicore shape, which is why the repair returns the floor rather than
   only applying it.
2. Recovery must run before any lease is installed — which RV1 already
   requires, and which the leased-store refusal now enforces mechanically.

*Done when:* a scripted crash that reverts the mark cannot hand out a page
redo wrote; the test fails without the repair (a fix whose test passes
either way is not a fix). `tests/wal_high_water_test.cpp`'s first test is
that, end to end — allocate four pages and sync, revert the free map to
its pre-allocation image, reopen, analyse, redo, repair, then allocate
four more and assert the pre-crash page's **bytes** are still there.
Without the raise the third allocation is that page and the sync writes
zeroes over it. The refusals, the monotonicity of the floor and the
transaction ceiling have their own tests beside it, and **none has been
executed.**

**RC04a — The driver, which no task owned. WRITTEN 2026-08-11, NOT BUILT
OR RUN.** `wal/recovery.hpp` + `src/wal/recovery.cpp`:
`RecoverCore(device, core_id, store, start, undo)` runs analysis → redo →
the high-water repair → undo, and returns a `RecoveryReport`.

§2's second item named "an entry point that runs them before the server
accepts a connection" and attributed it to "RC02-RC05", but no task's
*Done when* mentioned it, so it was written by nobody and the three phases
sat as three unconnected functions. This is that entry point. It is
numbered `04a` rather than given a slot in the series because the series
is cited elsewhere and renumbering it would break those citations.

Two decisions it had to make:

- **Undo is injected, and its absence refuses the mount.** RC05 is
  unbuilt, and the tempting shape — run the phases that exist and return
  success — is not merely incomplete, it is **worse than not recovering**.
  Redo restores uncommitted writes deliberately, and `txn.md` §8's
  accepted gap is that a surviving uncommitted row reads as *committed* on
  the next boot. A driver that replayed and stopped would publish every
  loser's writes. So `UndoPhase` is an interface, RC05 implements it, and
  a stream with losers and no phase installed fails `Unsupported` naming
  RC05. A stream with no losers recovers completely today.
- **The refusal is taken before redo, not after.** Once redo has run, a
  mount that then discovers it cannot undo has already put the rows on the
  pages; the only version of the check that leaves the database as it was
  found is the one before the first write. The test asserts exactly that —
  `store.page_count() == 0` after the refusal.

The high-water repair goes **before** undo, because undo writes and RV4's
rule is "before the store serves an allocation"; a test installs an undo
phase that allocates and asserts it cannot be handed a page the log names.

Left to the caller, per the layering `analysis.hpp` drew: reading the
anchor, applying `HighWaterRepair::next_trx_id` to the superblock, looping
over cores (RV2 — and introducing an order between streams is what
`workplan-crosscore.md` guideline 3 forbids), RC08's completion checkpoint
and RC09's report.

*Done when:* a loser with no undo phase refuses and writes nothing; a
winner recovers with no phase installed; a durable `TXN_ABORT` is not
treated as a loser; the phase is called exactly when it is owed; running
the whole driver twice is a no-op. All nine are in
`tests/wal_recovery_test.cpp` and **none has been executed.**

**RC05 — Undo.** *(read §4a first — RV6's "verbatim" no longer holds)*
Implements RC04a's `UndoPhase`.
Roll losers back through their `undo_ptr` chains, emitting compensations
through the **same** code `TransactionManager::Abort` uses — with §4a's
correction: the abort path now checks a row's identity before compensating
and takes a `RowLocator` recovery cannot supply, so RC05 reproduces the
*check* from the before-image's own Keystone id and takes the no-locator
branch on a mismatch. Then `TXN_ABORT`. Depends on RC06 for the insert
case.
*Done when:* a loser's UPDATE is restored byte for byte, its DELETE's mark
cleared, its INSERT's slot retired; a crash *during* undo resumes and
completes; the live-run and recovered-run page images are compared byte
for byte, which is the shape `assertion_wal_test.cpp` already uses.

**RC06 — A durable insert record.** *(gated on §4's decision)*
Make a loser's INSERT undoable from the log alone. Measure the INSERT path
before and after in `build-release`, interleaved A/B, and record it beside
the existing numbers — `txn.md` §3.6 traded this cost away deliberately
and the trade is being reversed with evidence.
*Done when:* a crash mid-transaction leaves no trace of the loser's
inserted rows after recovery; the measured cost is in `bench/`.

**RC07 — Bound Cabin replay.** *(unblocked 2026-08-11 — §4's first item is
answered; build to `feat-assertion.md` AS6a)*
Four parts, in dependency order:

1. **`BoundCabinEntry` gains `group_id` (uint32)**, in the 4 bytes
   `EncodeEntry` writes as a literal zero. Width stays 32 B.
2. **`AssertEntryPayload` gains `group_id`**, so replay reads the id rather
   than re-deriving it and never has to reproduce the live run's allocation
   order. The payload already carries the group key, so nothing else moves.
3. **The checkpoint snapshots each cabin's group headers** —
   `{group_id, key, count, sum}` — and recovery loads it, rebuilds the
   linkage by scanning the cabin's pages and bucketing by `group_id`, then
   feeds `exec::ReplayAssertionRecord` the range **from that checkpoint
   forward**.
4. **Verify** `header == Σ(entries)` through `VerifyAgainstEntries`, which
   is now a check of a rebuilt structure against durable bytes rather than
   of a structure against itself.

Both format touches are free while nothing has read a stream back and cost
a format-version event afterwards, which is why AS6a was ratified before
this task rather than during it.

*Done when:* `SHOW ASSERTIONS` reports `enforcing=1` immediately after a
restart — the claim `feat-assertion.md` §7 makes and the engine currently
contradicts — and the admission boundary answers identically either side
of a crash. Add one test that a group whose entries span a checkpoint
re-sums correctly, since that is the boundary the snapshot introduces.

**RC08 — Completion checkpoint and the anchor.**
Recovery ends by writing a checkpoint and publishing the anchor, bounding
the next crash's work (§12-4).
*Done when:* a second crash immediately after recovery replays only what
followed the completion checkpoint.

**RC09 — Observability and the honest counter.**
`wal.md` §13's recovery phase timings, plus RV3's counter: records
naming a relation the catalog no longer describes, reported rather than
skipped silently. `SHOW META` gains the last recovery's summary.
*Done when:* an operator can read what recovery did and what it could not.

**RC10 — Flip the gates.**
Enable SIM04/SIM11's `[GATED: recovery]` assertions; set the
documented-gap counters' expected value to zero; add EVT08's crash-matrix
points (before/after a dirty evicted page's writeback); close
`known-gaps.md`'s first two entries and amend `txn.md` §8.
*Done when:* the harness's full durability assertion runs, and one test
proves it *fires* against a hand-fed violating image — a gate that cannot
fail is not a gate.

## 6. What v1 still will not promise

Stated now so nobody reads a green suite as more than it is: the catalog
is not recovered (RV3), so DDL loss and K1's crash exposure survive this
work and close with K-M2a; nothing is purged, so undo grows as before; and
`D3`'s window is bounded, not zero. `known-gaps.md` keeps every one of
those entries after RC10, with two removed and three narrowed.
