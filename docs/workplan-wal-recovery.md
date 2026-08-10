# WAL recovery — workplan

Status: **RC01-RC02 written (unbuilt); RC03 onward not started.** Spec: `docs/wal.md` §12 (normative, and
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
| RV6 | **Undo reuses the abort path verbatim**, as `txn.md` §6 was written to allow: a loser's compensations are ordinary logged mutations, so undo is crash-restartable by RV5's gate alone and needs no undo-next pointer in v1. If a measurement or a proof says otherwise, a CLR is a format-version event and its own decision. |
| RV7 | **Advisory structures are rebuilt, never replayed.** Waystone pages are unlogged and correct when empty (invariant 8); Observational Cabins declare every value unobserved (`feat-cabin.md` §9); access statistics resume. Recovery touches none of them, and the contract suites are what prove that costs no result. |
| RV8 | **Bound Cabins are replayed**, because an assertion is authoritative and `feat-assertion.md` §7 promises enforcement at restart *with no gap*. `exec::ReplayAssertionRecord` is the fold; what it needs and does not have is a **record range** — see §4. |
| RV9 | **The gate flips once, in the harness.** SIM04/SIM11's `[GATED: recovery]` assertions are the acceptance criteria; RC10 enables them and the documented-gap counters' expected values become zero. A recovery whose own tests are written by this workplan rather than inherited from SIM is a recovery graded by its author. |

## 4. Open decisions — surface, do not decide

Each blocks a specific task and none is this plan's to settle. The house
rule applies: stop and ask, or build behind an interface that keeps every
option viable.

- **The assertion replay range** (`feat-assertion.md` §7's
  checkpoint-genesis gap). A group directory folded from records needs the
  records from the Bound Cabin's *birth*, not from the last checkpoint,
  because nothing durable holds the headers a checkpoint-bounded replay
  would start from. Either the checkpoint persists the directory, or
  assertion replay starts at each cabin's `ASSERT_BUILD`. **RC07 cannot be
  written until this is answered**, and it is the one open item on the
  critical path.
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

**RC03 — Redo.**
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
the record.

**RC04 — RV4's high-water repair.**
Raise the superblock's page high-water mark past every page id any
replayed record named, before any allocation can run.
*Done when:* a scripted crash that reverts the mark cannot hand out a page
redo wrote; the test fails without the repair (a fix whose test passes
either way is not a fix).

**RC05 — Undo.**
Roll losers back through their `undo_ptr` chains, emitting compensations
through the **same** code `TransactionManager::Abort` uses. Then
`TXN_ABORT`. Depends on RC06 for the insert case.
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

**RC07 — Bound Cabin replay.** *(blocked on §4's first item)*
Feed `exec::ReplayAssertionRecord` the decided record range and rebuild
every group directory, then verify `header == Σ(entries)` through
`VerifyAgainstEntries`.
*Done when:* `SHOW ASSERTIONS` reports `enforcing=1` immediately after a
restart — the claim `feat-assertion.md` §7 makes and the engine currently
contradicts — and the admission boundary answers identically either side
of a crash.

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
