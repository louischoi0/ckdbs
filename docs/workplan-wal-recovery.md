# WAL recovery — workplan

Status as of 2026-08-12, on `works-known-gaps` at `ed03b44` plus the
uncommitted mount wiring described in RC11 below:

| | |
|---|---|
| RC01, RC02, RC03, RC04, RC04a, RC05, RC06 | **built, compiled and green** (upstream `c1370e8` / `b11cc81` made the tree build and fixed the eleven failures that became visible; `28ee297` added the push guard) |
| **RC11 — recovery at mount** | **built 2026-08-12**, the caller RC04a listed and no task owned. `server/mount_recovery.hpp`, wired into `Expeditor::Open`, `CoreRuntime::Open` and `SimInstance::Boot` |
| RC10's first half | **done**: `kRecoveryImplemented` is flipped, so SIM04's crash contract is asserted, and its firing is proved against a `skip_recovery` boot |
| **RC07 — Bound Cabin replay** | **complete 2026-08-12**. `SHOW ASSERTIONS` reports `enforcing=1` after a restart and the admission boundary answers identically either side of a crash — verified on a running server, not only in tests |
| **RC08 — completion checkpoint** | **built 2026-08-12** for core 0 and for the harness; peer cores deliberately excluded, see the task |
| **RC09 — observability** | **built 2026-08-12**: phase timings, `SHOW META`'s recovery block, and RV3's counter split into the half that can be computed and the half that is stated |
| RC10's remainder | the `txn.md` §8 amendment and EVT08's crash-matrix points |

**The previous status line said "nothing in this series has ever been built
or executed", and that was true of the environment it was written in.** This
one has a toolchain: `scripts/test.sh` runs, and the whole suite is green
(2226 tests) with recovery running at every mount.

**Two findings from actually running it, both worth more than the wiring:**

- **RC01-RC06 were dead code.** Every phase was built and tested, and
  `grep RecoverCore src/ include/` matched only the test file — no mount ran
  a phase, so a crash recovered nothing. §2's second item named the entry
  point and RC04a built the driver; **what was missing was the caller**, and
  it went unwritten for the same reason the driver had: no task's *Done
  when* mentioned it. That is now RC11, and its lesson is the one RC04a
  already recorded once — a step nobody's done-when names is a step nobody
  builds.
- **Two defects in code that predates this series, both found by running it,
  both fixed 2026-08-12.** Neither was in the phases, and neither was
  findable without a mount that reads the log back:

  1. **Var-heap growth was unlogged**, three ways: no `PAGE_INIT` for a page
     `ChainAppend` created, no image for the link that reached it, and an
     UPDATE's spills not logged **at all**. A crash losing a new var-heap
     page's write-back refused the mount.
  2. **A segment sealed with no room for a PAD was read as a torn tail**, so
     the scan stopped at the boundary and every record in every later segment
     was silently dropped — recovery restoring a truncated stream and
     reporting success. `stream.cpp` had claimed a reader would take an
     unmarked tail to "mean exactly what the marker means"; `ScanLog` did not,
     and now does, by the same `kRecordHeaderSize` bound the writer seals with.

  **Both hid behind a green suite for one reason, and it is the reusable
  lesson**: the committed corpus runs at 1500 ops, which neither rolls a 1 MiB
  segment nor fills a var-heap page. A crash harness only tests the boundaries
  its runs actually reach. `SimLoop.ALongRunRollsASegmentAndStillRecoversEveryAcknowledgedRow`
  now reaches both.

  Verified after the fixes, `build-release`: 10 committed seeds × 3 profiles at
  4000 ops × 3 iterations, plus 8 unseen seeds at 6000 ops × 2 iterations — all
  green with the durability assertion armed.

Both of §4's blocking decisions are answered — the assertion replay range
by AS6a (RC07), the insert/enumeration question by RV10 (RC06, and so
RC05). **RV6 is superseded**: §4a records what `origin/main`'s `EXPLICIT`
key mode did to it, §4b what the enumeration gap did.

**Two defects were found by writing this plan, both in code that predates
it, and both had the same signature — a comment asserting a property the
adjacent code did not have.** `redo.cpp` said rollback's delete-mark clear
was a distinct record type; it was not, so a crash after an aborted DELETE
came back with the row still deleted (fixed at `a00c727`). §4a said a
delete-mark's before-image would yield a pk; its image is empty. Treat the
unexecuted parts of this series with that in mind. Spec: `docs/wal.md` §12 (normative, and
still `[PROPOSED]` — this plan proposes the amendments §12 needs and does
not make them). Related: `docs/txn.md` §§3, 6, 8, `docs/page.md` §§2, 8,
10, `docs/keystoneid-invariant.md` K-M2a, `docs/feat-assertion.md` §7,
`docs/workplan-testing.md` (SIM04/SIM11 — the acceptance tests, already
written and gated off), `docs/known-gaps.md` (this is its first item).

Decisions are `RV1`-`RV10`, tasks `RC01`-`RC10` plus `RC04a`. A fresh prefix on purpose:
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

   **This item was too narrow, and §4b says why.** Costing the three
   mechanisms found that a loser's writes are not enumerable after a crash
   *at all* — not only its inserts — because no chain in this engine is
   per-transaction. RV10 answers the wider problem and the insert record is
   one of its three parts.

## 3. Decision record — what this plan proposes

| ID | Decision |
|----|----------|
| RV1 | **Recovery runs at mount, before the listener binds**, per core, and a failure refuses the mount rather than serving a partial database. `Expeditor` gains the phase between store open and `Serve()`. No statement path can observe a half-recovered instance because none is accepted. |
| RV2 | **Per-core and independent**, as `wal.md` §12 says — valid exactly while transactions are core-local (`CC3`), which they are. Each stream recovers from its own anchor. Cross-stream ordering is not required and must not be introduced; guideline 3 of `workplan-crosscore.md` is the standing rule. |
| RV3 | **v1 recovers data, not the catalog.** Catalog and DDL writes are unlogged (`txn.md` §7), so a crash still loses a `CREATE TABLE` and still leaves `sys.tables.next_id` behind the log (K1). Recovery must therefore state its promise as *"every acknowledged commit to a relation that survived is restored"* — and **RC09 must add the counter that says when it did not**, rather than letting the gap read as closed. Logged catalog writes are K-M2a's and stay there. |
| RV4 | **A hazard RV3 creates, named so it is not discovered:** the superblock's high-water mark is unlogged too, so a crash can revert it while the log still names pages above it. A later allocation could then hand out a page redo has already written. Recovery **must raise the high-water mark to the maximum page id any replayed record names** before the store serves an allocation. Cheap, and it is the difference between a leak and corruption. |
| RV5 | **Redo is idempotent through `page_lsn` only** (`wal.md` §9): replay iff `record.lsn > page_lsn`. No second mechanism, no per-record sequence numbers. A headerless page is never a replay target (`page.md` §1), which is what keeps Waystone out of recovery entirely. |
| RV6 | ~~**Undo reuses the abort path verbatim**, needing no undo-next pointer in v1.~~ **Superseded 2026-08-11; both halves failed.** "Verbatim" fell to the `RowLocator` the abort path grew for `EXPLICIT` leaf division (§4a). "No undo-next pointer" fell to the enumeration gap (§4b) and is replaced by RV10. **What survives is the shape**: a loser's compensations are still ordinary logged mutations, so undo is still crash-restartable by RV5's gate alone and still needs no CLR — which is the part `txn.md` §6 was written to allow, and it holds. |
| RV7 | **Advisory structures are rebuilt, never replayed.** Waystone pages are unlogged and correct when empty (invariant 8); Observational Cabins declare every value unobserved (`feat-cabin.md` §9); access statistics resume. Recovery touches none of them, and the contract suites are what prove that costs no result. |
| RV8 | **Bound Cabins are replayed**, because an assertion is authoritative and `feat-assertion.md` §7 promises enforcement at restart *with no gap*. `exec::ReplayAssertionRecord` is the fold; what it needs and does not have is a **record range** — see §4. |
| RV10 | **Decided 2026-08-11. A loser's writes are enumerated by its own undo chain, not by the scan range** — and the decision is wider than the INSERT question that surfaced it (§4b). `UndoRecordFields` gains `txn_prev_undo_ptr`, the writing transaction's previous record; `CHECKPOINT_BEGIN`'s active-transaction table becomes `[{txn_id, last_undo_ptr}]`, the durable head each chain is walked from; and `UndoRecordType::kInsert` is **written**, carrying the row's `pk`. Amends RV6 and reverses `txn.md` §3.6. Built by RC06, measured before it is kept. |
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

**Proposed resolution.** Recovery's undo checks identity from the record
itself and, on a mismatch, takes the **no-locator branch** — refuse the
mount rather than guess. That is not a degraded mode; it is the branch main
already ships as "the safe half of the same rule", and it keeps `rel_oid`
out of a format that has no room for it.

> **Corrected 2026-08-11, at RC06.** This paragraph first claimed the
> resolution "needs no format change", because "for `kOverwrite` and
> `kDelete` the before-image *is* the prior tuple payload" and
> `KeystoneIdOfPayload` could read the pk out of it. **That is false for two
> of the three types.** `kDeleteMark`'s image is empty by design — the enum
> says so: *"a delete-mark changes no tuple bytes, so there are none to
> restore"* — and `kInsert`'s is empty too. Only `kOverwrite` carries a
> payload to recover a pk from.
>
> So `pk` is stored on **every** undo record, as one of RV10's two added
> fields (§4b), and the identity check is uniform across the three types
> rather than available for one. The correction cost nothing, because RV10
> was moving the format anyway — but the claim was wrong when written, and
> it was wrong in the same way `redo.cpp`'s delete-unmark comment was:
> reasoning about the design instead of reading the adjacent enum.

What it costs: a crash on an `EXPLICIT` relation whose leaf divided
mid-transaction refuses the mount instead of recovering. Narrow today —
`EXPLICIT` is refused on heap relations, and only `EXPLICIT` can trigger a
division mid-statement — and it fails loudly rather than corrupting.
Lifting it means putting `rel_oid` in the undo record, which is a
format-version event and RC05's to argue if the refusal proves too broad.

**RC05 must not be written as "call `Abort`".** The abort path's signature
now has a parameter with no recovery-side implementation, and the identity
check is the part that matters.

## 4b. RV10: the enumeration gap, which is what §4's INSERT question really was

**Found 2026-08-11, pricing §4's second open item.** That item asked how a
loser's INSERT becomes undoable. Reading the code to cost the three
mechanisms turned up a larger fact: **a loser's writes are not enumerable
at all after a crash, inserts or otherwise.**

`undo_log.hpp` states the topology in its own words — *"there are two chains
here and they are still not the same chain"*: `prev_page_id` (page → page,
creation order) and `prior_undo_ptr` (record → record, **one tuple's
versions**). Neither is per-transaction. `UndoRecordFields` carries no
owning transaction id, and `CheckpointBeginPayload`'s active list is bare
`uint64` ids. So recovery's only route to "what did this loser write" is the
WAL records inside the replay range.

**That range does not cover it, and the hole is reachable:**

> Loser `T` inserts into page `P` at LSN 100. `P` is written back — WAL steal
> permits it, and `FlushPages` and the eviction drain both do it — so
> `recLSN(P)` is cleared and `P` is absent from the checkpoint's dirty table
> at LSN 500. Other pages carry recLSN 600, so `redo_start` is 600. Analysis
> knows `T` is a loser, because the checkpoint's active list names it, and
> **never sees the record that says where it wrote.**

The row survives undo, and `txn.md` §8's gap makes it read as *committed*.
Silent wrong data — the failure recovery exists to prevent. RV6's "needs no
undo-next pointer in v1" is the sentence that does not survive: an
undo-next pointer is exactly what enumerates a loser's writes independently
of the redo start.

**The three parts of RV10, and why each is load-bearing:**

1. `UndoRecordFields` gains **`txn_prev_undo_ptr`** — a third chain beside
   the two the file already names, and the only one that answers "what did
   *this transaction* do".
2. `CHECKPOINT_BEGIN`'s active-transaction table becomes
   **`[{txn_id, last_undo_ptr}]`** — the durable head. It is already the
   record carrying the active set; this is 8 bytes per active transaction
   more.
3. **`kInsert` is written, with the row's `pk`.** It now has a reason beyond
   "somewhere to put the fact": an insert that wrote no record would break
   the chain and orphan everything the transaction did before it. The `pk`
   is what lets §4a's identity check run on the one case with no
   before-image to recover a pk from.

**Why not the cheaper-looking options.** Deriving inserts from
`HEAP_INSERT` records is free on the write path and has exactly the hole
above — and fails *silently*, which is the worst of the three. Snapshotting
the trail into the checkpoint closes the hole but makes `CHECKPOINT_BEGIN`
proportional to uncommitted inserts: unbounded under a bulk load, and a
record must fit a segment. Capping it needs a fallback, and the only
available fallback is writing the record — so it is RV10 plus a second
mechanism.

**Cost, from `bench/results-txn-layers.md` rather than from argument.** At
the shipped `group` default the WAL-append phase is 0.95 µs for INSERT
against 5.38 µs for UPDATE, on a 951 µs statement whose fsync is 933.69 µs —
**~0.5 %**. Unlogged the same phase is 0.06 µs against 2.14 µs on a 7.21 µs
statement — **up to ~29 %**, in the regime that actually scales. A `kInsert`
record carries no image, so that is an upper bound and not the number. RC06
measures the real one.

**Both format changes are free today** and are format-version events once
recovery ships — nothing has ever read a stream back, the same argument
that moved RC03's `UNDO_WRITE` correction and AS6a's entry field.

**What stays open:** whether the measured `relaxed` cost justifies a
narrower rule — writing the record only for transactions that survive a
checkpoint, say. That is RC06's call *with a number in hand*, not this
section's.

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
- ~~**Writing `UndoRecordType::kInsert`**~~ — **ANSWERED 2026-08-11, and
  the question was too narrow.** Costing the three mechanisms turned up the
  enumeration gap in §4b: a loser's writes are not reachable after a crash
  at all, inserts or otherwise, because no chain is per-transaction. The
  answer is **RV10** — `txn_prev_undo_ptr` on the undo record, each active
  transaction's `last_undo_ptr` in `CHECKPOINT_BEGIN`, and `kInsert`
  written with the row's `pk`. RC06 builds it and measures it; `txn.md`
  §3.6 is corrected.
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

**RC05 — Undo.** *(read §4a and §4b first — RV6 fails on both counts: not
"verbatim", and not "no undo-next pointer". **Now depends on RC06 for the
whole phase, not only for the insert case.**)*
Implements RC04a's `UndoPhase`.

Roll each loser back by walking **its own chain** from the
`last_undo_ptr` the checkpoint recorded (RV10), newest to oldest — not by
enumerating its writes from the scan range, which §4b shows is silently
incomplete. Emit compensations through the **same** code
`TransactionManager::Abort` uses, with §4a's correction: the abort path
checks a row's identity before compensating and takes a `RowLocator`
recovery cannot supply, so RC05 reproduces the *check* — from the
before-image's Keystone id for `kOverwrite`/`kDeleteMark`, and from the
record's own `pk` for `kInsert` — and takes the no-locator branch on a
mismatch. Then `TXN_ABORT`.

**WRITTEN 2026-08-11, NOT BUILT OR RUN.** `txn/recovery_undo.hpp` +
`src/txn/recovery_undo.cpp`: `txn::RecoveryUndo` implements
`wal::UndoPhase`. It lives in `txn/` because it needs `UndoLog`,
`heap::PageView` and the record format, and `wal/` sits below `txn/`.

**A bug found before a line of it was written, and fixed first.**
`TransactionManager::Compensate` logged `HEAP_DELETE_MARK` to *clear* a
delete-mark, and redo replays that type by **setting** one — so a
transaction that delete-marked a row and then aborted came back from
recovery still deleted, the abort undone by its own compensation.
`redo.cpp` asserted the opposite in a comment, which is why it survived
RC03. `RecordType::kHeapDeleteUnmark` now exists, with a payload carrying
`undo_ptr` as well as the writer — `ClearDeleteMark` restores both, and a
record carrying only the writer would leave the version chain pointing at
an undo record for a change that no longer happened.

`UndoVersion` gained the four fields the *version* walk never needed and
undo does: `target_page_id`, `target_slot`, `txn_prev_undo_ptr`, `pk`. A
reader stepping back through versions has the tuple in hand; undo arrives
with a chain of records and no idea which tuple any of them is about.

*Done when:* a loser's UPDATE is restored byte for byte, its DELETE's mark
cleared, its INSERT's slot retired; **a loser whose write predates the redo
start is rolled back too** (§4b); a crash *during* undo resumes and
completes; the live-run and recovered-run page images are compared byte
for byte, which is the shape `assertion_wal_test.cpp` already uses.

Ten tests in `tests/recovery_undo_test.cpp` cover the first three, the
re-run no-op, the two classes undo owes nothing to, §4a's refusal when a
row has moved, and a self-linked chain reported rather than hung on.
**The byte-for-byte live-vs-recovered comparison is not among them** — it
needs a driver that runs both, which is RC08/RC10's harness. **Nothing has
been executed.**

**RC06 — The per-transaction undo chain, and a durable insert record.**
*(unblocked 2026-08-11 — build to RV10; read §4b for why the scope grew)*
Three parts, and the third is the one the task was originally named for:

1. **`UndoRecordFields` gains `txn_prev_undo_ptr`**, set on every undo
   record to the writing transaction's previous one.
2. **`CHECKPOINT_BEGIN`'s active-transaction table becomes
   `[{txn_id, last_undo_ptr}]`** — `CheckpointBeginPayload`,
   `EncodeCheckpointBegin`/`DecodeCheckpointBegin`, and analysis's seeding
   of `TxnOutcome::kLoser` from it, which now carries a head per loser.
3. **`kInsert` is written, carrying the row's `pk`** — the chain link an
   insert would otherwise omit, and §4a's identity check for the one record
   type with no before-image.

Then RC05's undo walks each loser's chain from its head rather than
enumerating from the scan range.

Measure the INSERT path before and after in `build-release`, interleaved
A/B, and record it beside the existing numbers — `txn.md` §3.6 traded this
cost away deliberately and the trade is being reversed with evidence.
§4b's read of `bench/results-txn-layers.md` predicts ~0.5 % at `group` and
an upper bound of ~29 % unlogged; **a prediction is not the measurement**,
and `bench/results-keystone-alloc.md`'s lesson is that this engine has had
the sign of such a number wrong before.

*Done when:* a crash mid-transaction leaves no trace of the loser's
inserted rows after recovery; **a loser whose write predates the redo start
is still rolled back** — the §4b case, and the one a scan-range enumeration
would miss silently; the measured cost is in `bench/`.

**RC07 — Bound Cabin replay.** *(unblocked 2026-08-11 — §4's first item is
answered; build to `feat-assertion.md` AS6a)*

**Parts 1 and 2 built 2026-08-12; parts 3 and 4 are what remain.** The two
persisted formats have moved, which was the half AS6a said had to happen while
it was still free, and the directory now has the primitives the replay path
will call. What is *not* built is the plumbing: nothing writes a snapshot at a
checkpoint, nothing loads one at a mount, and `SHOW ASSERTIONS` still reports
`enforcing=0` after a restart.

Built:

- `BoundCabinEntry::group_id` (uint32) in the first 4 bytes of AST04's padding
  word, which was written as a literal zero — so the 32-byte width is unchanged
  and every entry on every existing page reads back as `group_id = 0`. Stamped
  at all three writing sites: the CREATE-time builder, the enforcer's reserve
  path, and (adopted rather than assigned) the replay fold.
- `AssertEntryPayload::group_id`, appended past `reserved`, so no existing
  offset moved. **Its `sizeof` assert had to go** — the wire payload is 20
  bytes and the C++ struct pads to 24 for its leading `uint64_t`, which is
  exactly the layout-versus-format confusion that shipped broken at RC06; the
  offsets the codec actually uses are asserted instead.
- `GroupHeader::group_id`, dense per cabin from 1, assigned at the **one**
  creation site (`EnsureGroup`) so a group cannot be born without an id — an
  entry stamped 0 is one recovery cannot attribute.
- `EnsureGroupId` / `AdoptGroupId` / `RestoreGroup` / `AttachEntry` /
  `SnapshotGroups`: the primitives for AS6a's recovery order. `AdoptGroupId` is
  the one worth reading — the fold must take the **record's** id rather than let
  `Apply` assign one, because a fold starting from a checkpoint meets groups in
  record order and the ids would drift from the entries already on the pages,
  misattributing them at the *next* recovery. It refuses a disagreement rather
  than picking.

A departure reads its group's id instead of ensuring one, mirroring the live
path's own asymmetry: a departure's group must already exist, and creating one
would be creating a group to immediately go negative in.

Four parts, in dependency order:

1. ~~**`BoundCabinEntry` gains `group_id` (uint32)**~~ — **built**, in the 4
   bytes `EncodeEntry` wrote as a literal zero. Width stays 32 B.
2. ~~**`AssertEntryPayload` gains `group_id`**~~ — **built**, so replay reads
   the id rather than re-deriving it and never has to reproduce the live run's
   allocation order. The payload already carried the group key, so nothing else
   moved.
3. **Mechanism built 2026-08-12; the mount wiring is what remains.** The
   checkpoint snapshots each cabin's group headers —
   `{group_id, key, count, sum}` — and recovery loads it, rebuilds the
   linkage by scanning the cabin's pages and bucketing by `group_id`, then
   feeds `exec::ReplayAssertionRecord` the range **from that checkpoint
   forward**.
   Built: `RecordType::kAssertSnapshot` and its payload, **chunked** because a
   cabin's group count is bounded by the data and a record must fit a segment —
   the loader is additive over chunks, so no continuation flag exists;
   `wal::AssertionSnapshotSource`, the seam the checkpointer asks (the shape
   `ActiveTransactions` already had), emitted immediately after
   `CHECKPOINT_BEGIN` so the base precedes every record folded onto it; and
   `exec::RecoverAssertions`, which scans **from the anchor's `checkpoint_lsn`**
   rather than from redo's start — a narrower range in which the first snapshot
   per assertion is by construction its base, which is what makes the pass a
   single forward walk with nothing buffered.

   Two rules it enforces rather than assumes. An assertion whose records appear
   with **no snapshot** is left unrecovered and counted
   (`records_without_a_base`): folding onto nothing yields aggregates that are
   too small, and an admission check on those *admits a write that violates the
   assertion*. And an **empty** cabin still gets a record, so "recovered and
   empty" cannot read like "no base found" — the first may enforce, the second
   may not.

   **The mount wiring, built the same day.** `AssertionEnforcer` implements the
   seam (it owns the directories, so it is what can snapshot them);
   `Expeditor::Open` calls `ResumeAssertionsAfterRecovery`, which revives each
   surviving declaration through `exec::ReviveAssertion` — §8.2's `source_text`
   is the canon precisely so the group columns can be recovered by re-parsing it
   — refills the directories through the pass, and **adopts only what came back
   whole**. `SimInstance::Boot` does the same, so the harness mounts what a
   server mounts.

   Two ordering traps, both found by building it and both now written into the
   code:

   - **The resume must precede the completion checkpoint.** That checkpoint
     becomes the anchor, so it is the record the *next* mount folds from — and it
     can only carry the group snapshots if the registry has already been
     refilled. Written the other way round (as the first version was),
     enforcement survived exactly one restart and the mount after it found no
     base.
   - **An unrecovered cabin is never adopted.** A directory at zero admits every
     write, so adopting one would report `enforcing=1` for a constraint enforcing
     nothing — strictly worse than the honest `enforcing=0` that was already
     true.
4. **Part 4, folded into part 3's tests.** `VerifyAgainstEntries` now runs over a
   directory rebuilt from a snapshot plus the cabin's real pages
   (`AssertionRecoverTest.TheLinkageComesBackFromTheCabinsOwnPages`) — a check of
   a rebuild against durable bytes rather than of a structure against itself.
   What is missing is the same wiring: a mount that calls it.


Both format touches are free while nothing has read a stream back and cost
a format-version event afterwards, which is why AS6a was ratified before
this task rather than during it.

**Reviewed 2026-08-12, and the review found three bugs plus two things I had to
stop and not decide.** Fixed: a second checkpoint's snapshot inside the scan
range failed the whole pass (the *ordinary* crash shape — the anchor is published
only at `Complete()`, so a crash mid-checkpoint leaves its snapshot records in
range — and it left every assertion unenforcing); a chunked snapshot relinked
each earlier chunk's entries again; `DecodeAssertSnapshot` reserved from an
unvalidated on-disk count. Also fixed, from the reported half: an assertion
created after the last checkpoint could **never** recover — it stayed out of the
registry, and the completion checkpoint snapshots only the registry, so
`enforcing=0` was permanent until DROP + CREATE. `CREATE ASSERTION` now writes
its own base at publish through the shared `wal::LogAssertionSnapshot`.

Two findings are recorded in `docs/known-gaps.md` rather than fixed, because both
are AS6a's call and not a maintainer's: a recovered cabin's entry list is a
superset of the live one wherever a pre-checkpoint abort orphaned an entry (the
aggregate is right; §5.2's `VerifyAgainstEntries` proof is what breaks), and
`AssertEntryPayload`'s offsets moved without a format-version bump while the
"nothing has read a stream back" licence expired in the same range.

**A test that was 0.5% short of the boundary it was named for.**
`ManyGroupsChunkAcrossRecords…` used 600 groups against a 61,408-byte budget and
came to 61,106 — so it never chunked, and the chunk-relink bug it should have
caught went unnoticed. It now asserts `SnapshotRecords(...) > 1` rather than
trusting arithmetic done by eye. Same lesson as the corpus running at 1500 ops:
a test named for a boundary has to be made to *reach* it.

*Done when:* `SHOW ASSERTIONS` reports `enforcing=1` immediately
after a restart — the claim `feat-assertion.md` §7 makes and the engine currently
contradicts — and the admission boundary answers identically either side
of a crash. **Held, on a running server**: `CREATE ASSERTION cap ON accounts
GROUP BY (branch) CHECK SUM(amount) <= 100`, two inserts summing to 70,
`kill -9`, restart. `SHOW ASSERTIONS` reports `enforcing=1`, `SHOW META` reports
`recovery_assertions_enforcing=1 recovery_assertions_unrecovered=0`, and the
boundary answers identically to pre-crash: `+50` refused, `+30` admitted
(70 + 30 = 100, exactly at the bound), `+1` then refused. The last three are what
prove the *recovered aggregate* is 70 — neither 0 (which would admit all three)
nor 140 (which would refuse all three).

The workplan's named extra test is
`AssertionRecoverTest.AGroupWhoseEntriesSpanTheCheckpointReSumsCorrectly`; the
wiring itself is guarded by `AssertionResumeTest`'s two, which drive a real
`CREATE ASSERTION` and then resume a **fresh** registry from the log.

**RC11 — Recovery at mount. BUILT 2026-08-12.** `server/mount_recovery.hpp`
+ `src/server/mount_recovery.cpp`: `RecoverCoreAtMount(core_id, anchor,
device, store, undo_log, wal, log)` turns the anchor into an `AnalysisStart`,
installs `txn::RecoveryUndo`, runs `RecoverCore`, and returns the report.
Called once per core by whoever owns that core's stack — `Expeditor::Open`
for core 0, `CoreRuntime::Open` for each peer, `SimInstance::Boot` for the
harness — so RV2's independence is structural rather than promised.

Numbered `11` rather than inserted, for the reason RC04a gave: the series is
cited elsewhere and renumbering it would break those citations.

Four things it had to get right, none of them in any task's text:

- **The anchor cannot be read from the superblock in front of you.** A
  peer's `CoreRuntime::superblock_` is a default-constructed copy whose
  anchor slots are all zero, and a peer's checkpointer publishes *through*
  core 0 (`remote_checkpoint_anchor.hpp`). So `CoreRuntime::Config` gains an
  `anchor` copied on the startup thread. A zeroed anchor is legal — "no
  checkpoint yet, scan from the head" — which is exactly why getting this
  wrong would have been silent.
- **`TrxIdSequence` caches the ceiling at construction** (`txn/trx_id.hpp`),
  so the ceiling must be applied and persisted *before* the sequence exists.
  Raising it afterwards writes a field nothing reads again, and the sequence
  hands out ids the log already names — RV4's transaction half, defeated by
  ordering alone.
- **A peer recovers before `SetCoreOwnership`.** `DevicePageStore` refuses to
  raise its allocation floor once a lease is installed (RC04's own decision,
  and correct), so a peer that took its lease first would refuse its own
  mount the moment its stream held anything.
- **The extent allocator's hint is RC04's obligation 1, and it was unmet.**
  `Expeditor::Serve` built `ExtentAllocator` at `kFirstUserPageId`; it now
  starts above `page_floor`. `Reserve` never scans below its hint
  (`extent_lease.cpp`), so that is a guarantee rather than an optimization.

*Done when:* a mount recovers its own stream and refuses rather than serving
a partial database; the anchor's two fields both reach analysis; the two
caller obligations come back as numbers. Eight tests in
`tests/mount_recovery_test.cpp`, written against what the *seam* adds rather
than re-testing the driver — each of them fails on a seam that would pass
every test in `wal_recovery_test.cpp`.

**Measured** (`build-release`, interleaved A/B against `--skip-recovery`,
three reps × three seeds): recovery adds ~0.16-0.21 s to a 3000-op crash run
of three reboots, ≈55-70 ms per mount over a ~1500-op stream. **No statement
path changed, so steady-state per-statement cost is zero by construction** —
this is mount cost, and it grows with the log because **nothing publishes an
anchor after recovery**. That is RC08, and this measurement is its argument.

**RC08 — Completion checkpoint and the anchor. BUILT 2026-08-12.**
Recovery ends by writing a checkpoint and publishing the anchor, bounding
the next crash's work (§12-4). `server::CheckpointAfterRecovery` in
`mount_recovery.hpp`, called by `Expeditor::Open` and by `SimInstance::Boot`
right after the ceiling is applied.

Three things it settled:

- **The active-transaction table is empty as a fact, not as a stand-in.**
  `CHECKPOINT_BEGIN` carries the live set so undo can find its losers (RV10),
  and immediately after recovery there are none: losers rolled back and given
  their `TXN_ABORT`, winners committed before the crash, no statement accepted
  because the listener is not bound (RV1). So the function takes no transaction
  source and **cannot be handed a stale one** - which matters, because a
  checkpoint written with a stale active list would send the next recovery
  walking the undo chain of a transaction that no longer exists.
- **It runs unconditionally, including when the scan found nothing.** A
  database that has never completed a checkpoint has a zeroed anchor, and a
  zeroed anchor means "scan from the head of the stream" - so the mount that
  recovers nothing is exactly the mount whose *successor* pays. Two records and
  one dirty-table flush is the price of not being that.
- **`Expeditor` builds its checkpoint target and anchor early** and the cadence
  checkpointer borrows the same two objects, so the anchor's publish count spans
  the mount and the interval rather than resetting when the reactor starts.

**Not done for peer cores, deliberately and by name.** Publishing means writing
page 0, which is the system core's (M5), and the ring a peer would send its
anchor over is not attached until `AttachTransport()`. A peer's next mount
therefore still scans from whatever anchor core 0 last wrote it. That costs
nothing today - a peer cannot reserve a transaction id, so its stream holds no
writes of its own - and it is written into `core_runtime.cpp` rather than left
to be discovered when P5's id leases make peer writes real.

*Done when:* a second crash immediately after recovery replays only what
followed the completion checkpoint. **Held**, in
`MountRecoveryTest.TheCompletionCheckpointStopsTheNextRecoveryRescanningTheStream`:
the second mount reads **2 records** where the first read 5, with
`redo_skipped_by_lsn == 0` - the bounded records are not scanned-then-skipped,
they are not scanned at all, which is the difference between a bounded recovery
and a cheap-looking one. A second test asserts the published anchor satisfies
analysis's own durable-point check, since an anchor that failed it would hand
the next mount a refusal.

**Measured twice, and the second measurement is the one to quote.** The first
was `ckdbs-sim`'s A/B against `--skip-recovery`: 0.21-0.34 s per crash run either
side, cost-neutral to within noise - because the harness reboots once per log and
so pays the checkpoint without ever taking the cheaper scan. A ck-tester pass
then built the multi-mount measurement that shape cannot produce
(`tools/mount_cost_benchmark.py`, `bench/results-wal-recovery.md`), and **the
benefit is large**:

| log before the sweep | mount 1 | mounts 2-9 p50 | saved |
|---|---|---|---|
| 200 rows, SIGKILL | 142.7 ms | 133.1 ms | 9.6 ms |
| 2000 rows, SIGKILL | 184.2 ms | 128.0 ms | 56.2 ms |
| 10000 rows, SIGKILL | 336.9 ms | 93.8 ms | **243.1 ms** (3.6×) |

Recurring cost is 7-8 ms and ~4.3 KB per mount, against 36 B for a mount that
writes no checkpoint. 68% of the 10k saving is the fat checkpoint the first mount
had to write, which is the point: RC08 moves that work off every *later* mount.

**And it found what RC08 does not cover:** a **clean shutdown publishes no
anchor**, so the first mount after a graceful stop rescans everything the last
run wrote. Recorded in `docs/known-gaps.md`; the fix is this same call on the way
out.

**RC09 — Observability and the honest counter. BUILT 2026-08-12.**
`wal.md` §13's recovery phase timings, plus RV3's counter. `SHOW META` gains
the last mount's recovery block; `docs/client-manual.md` carries the field
list.

**The counter RV3 asked for cannot be computed, and that is the finding.**
RV3 wanted "records naming a relation the catalog no longer describes".
Resolving a page to its relation needs a page→relation index; `page.md` has
none, and its absence is already a named gate elsewhere
(`feat-physical-optimizer.md` §6 gate 3 blocks page reuse for exactly the same
missing map). The only alternative is walking every live relation's chains to
build the set, which is O(every page in the database) at every mount — that is
not a counter, it is a second recovery. So RC09 splits the gap in two and
reports both halves honestly:

- **Computable, and now computed**: user relations the catalog still describes
  whose descriptor or var-heap root page the crash took.
  `AuditCatalogAfterRecovery`, O(relations) with one page read each. It
  **reports rather than refuses** — an unopenable relation is the finding, not
  an error hit while producing it.
- **Not computable, and now stated**: rows whose relation the catalog lost.
  `SHOW META` prints `catalog_recovered=0` as a standing constant, so
  "recovery succeeded" can never be read as "nothing was lost".

Three things the implementation had to get right, each found by a test failing:

- **`GetSysTableRow`, not `InitTableAccess`.** An access is cached and a row
  read never is (`catalog.hpp`), so an audit on the cached path answers from
  memory for any relation something already opened. The first version missed
  one of two dead relations for exactly that reason.
- **User relations only.** A bootstrap catalog relation is listed in
  `sys.objects` and has no `sys.tables` row at all, so opening one fails for a
  reason that has nothing to do with a crash. The first version reported **nine
  missing relations on a healthy mount**.
- **`timings.timed`, and durations omitted without it.** Four zeroes from an
  untimed run and four from an instant one are the same bytes, and an operator
  tuning RTO has to tell them apart. The sim passes no clock deliberately: its
  `ManualClock` never advances, so timing against it would print zeroes
  wearing a measurement's face.

*Done when:* an operator can read what recovery did and what it could not.
**Held, on a real server** rather than in a test: `durability = strict`,
`checkpoint_interval_ms = 3600000` so no cadence checkpoint flushes the page,
two INSERTs, `kill -9`, restart. `SHOW META` reports
`recovery_records=11 recovery_committed=2 recovery_redo_applied=5
recovery_relations_checked=1 recovery_relations_missing_pages=0
catalog_recovered=0`, and `SELECT * FROM t` returns both rows — replayed from
the log, because the pages were never written back. That is the engine's first
crash recovery of acknowledged data, read back through the operator surface.

**A cost RC09's own timings exposed, and it dominates the mount.** On the same
server, three separate mounts each reported `recovery_analysis_us≈44000` and
`recovery_redo_us≈42000` — **86 ms of a ~90 ms mount, on a log holding
nothing**. The cause is not the phases: `ScanLog` allocates and reads a
segment's **entire body** (`body.assign(...)`, one `ReadAt`), analysis and redo
each run their own scan, and the default segment is 64 MiB — so a mount reads
128 MiB and allocates two 64 MiB buffers before it can serve a statement.
`log_scanner.cpp`'s own comment anticipated it ("when segments are 64 MiB this
becomes a streaming read"); now it has a number. It is a **performance finding,
not a defect**, and it is not RC09's to fix: the fix is to read only as far as
the durable end, or to stream in chunks, and it belongs beside the segment-size
decision (`wal.md` §15, still `[OPEN]`). Recorded in `docs/known-gaps.md`.

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
