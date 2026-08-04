# Transactions & MVCC — Workplan

Work instructions, companion to `txn.md`. Tasks `T01`-`T14`.

**Status: built. `T01`-`T14` are done.** Undo storage, the durable id
sequence, the visibility predicate at the step VM's one choke point, the
transaction manager, sessions, `BEGIN`/`COMMIT`/`ROLLBACK`, transactional
INSERT/UPDATE, `DELETE`, rollback compensations and the `TXN_CONFLICT` wire
surface. **1,119 tests pass**, 106 of them new.

What is deliberately **not** built, and stays that way: recovery (§8's
accepted gap), undo purge and `SnapshotTooOld` (§9 — readers are still
unregistered, which is what makes purge impossible and `SnapshotTooOld`
unreachable), `SERIALIZABLE`, savepoints, transactional DDL, and cross-core
transactions.

**One loose end found after T14 and fixed:** the checkpointer was still
handed `wal::NoActiveTransactions`, whose empty list was a *correct* answer
only while no transaction could be live. With real transactions a
`CHECKPOINT_BEGIN` claiming nothing was in flight would tell recovery's
analysis phase to treat every uncommitted row of those transactions as a
winner. `TransactionManager` now implements `wal::ActiveTransactions` -
which is what that interface's own comment always said would happen.

Two test groups from §10 are short of what the spec asks: §10-7's
"compensation records read back off the device" and §10-4's live-vs-rebuilt
check under a *logged* dispatcher. Both need a `WalManager`-backed session
fixture; `txn_session_test.cpp` runs unlogged on purpose, because what it
tests is what a client sees. The record-level halves are covered separately
in `undo_log_test.cpp` and `insert_wal_test.cpp`.

Every *format* piece the spec needs already existed and had never been used:
`PageType::kUndo`, `RecordType::kUndoWrite` / `kHeapOverwrite` /
`kHeapDeleteMark` / `kSlotRetire` / `kTxnBegin` / `kTxnCommit` /
`kTxnAbort`, `wal::UndoWritePayload`, `TupleHeaderFields::{trx_id,
undo_ptr}`, `PageView::{DeleteMark, RetireSlot, OverwriteTuple}`,
`catalog::kBootstrapXid`, `StatusCode::kTxnConflict`. This workplan is the
machinery, not the formats.

Execution rules:
- Do tasks in numeric order unless "needs" says otherwise.
- Each task ships with its listed tests in the same change.
- If a task turns out to touch an `[OPEN]` item in the spec — stop, flag,
  do not decide. `T04`'s block size is the one place this workplan builds
  *behind* an open item rather than around it.
- `T13`'s suite is regression-mandatory from the moment it exists, and the
  Waystone and Cabin contract suites stay so throughout: turning MVCC on
  must not change what either returns for a single-transaction workload.

---

## What this is

The two isolation levels of `txn.md` §1, undo storage (§3), snapshots and
the visibility predicate (§4), first-updater-wins (§5), rollback (§6), and
the session state machine those need to exist in. Plus `DELETE`, which the
spec's §10 tests require and the grammar does not have.

## What this is not

Recovery (§8 — the accepted gap, restated below) · undo purge and
`SnapshotTooOld` (§9, structurally unreachable while readers are
unregistered) · `SERIALIZABLE` · savepoints and statement-level rollback ·
transactional DDL (§7) · lock-based blocking · cross-core transactions.

---

## Three amendments to `txn.md`, found by reading the code

The spec was written before the step VM landed. These are corrections to
the document, not deviations from it — each one is the spec's own
requirement, relocated to where the code now is.

**A1 — §4.4's site has moved, and improved.** The spec names
`HandleSelect`'s `emit` and `HandleUpdate`'s `apply` as the two lambdas that
must call the predicate, so that Waystone §3.1 rule 2 is true by
construction. Since then, every read went through the step VM: the choke
point is now **`ChainRunner::AcceptTupleAt()`** (`src/exec/step_vm.cpp`),
which the chain walk, the btree descent, the probe memo, Waystone replay
(`TryReplay`) and the Cabin resolve phase (`RunCabinStep`) all funnel
through. One call site, not two, and it covers three consumers the spec did
not know about. `HandleUpdate::apply` remains a second site because UPDATE
does not compile to a chain; `T09` keeps it honest, and `HandleCatalogView`
stays unfiltered per §7.

**A2 — the undo walk collides with parser-v2 I15's R1.** §4.3 steps back
through undo records, and stepping back is a **page fetch**. R1 forbids a
page-frame span being live across a nested fetch, and `AcceptTupleAt`
decodes under exactly such a span (`PageSpanGuard`). So the predicate cannot
walk undo where it is called from. The resolution is the one the var-heap
already uses one line above it: **copy the candidate's header and payload
into scratch, release the span, then walk.** The copy is a fixed number of
bytes because invariant 13 makes a row's size a schema constant — the same
property that makes relayout a `memcpy` pays for MVCC here. Cost: one
`memcpy` per examined tuple on the read path, and `T05` measures it.

**A3 — `next_trx_id` has nowhere to live.** §4.2 says
`SuperBlock::CreateFresh` seeds it to `kFirstUserTrxId = 2`. There is no
such field; the dispatcher's `next_txn_id_` is a member that restarts at 1
every boot, and its own comment already calls that a known gap. `T04` adds
the field (format version **8 → 9**) and a bump-ahead allocator. Note what
that does *not* buy: the superblock is unlogged, so this has the same shape
of exposure `keystoneid-k0-findings.md` measured for row ids, and §10-2's
"a crash burns the block remainder" is the honest version of the property —
ids are never *reissued*, and they are not gapless.

## The gap this ships with, restated

§8: **an uncommitted row that survives a crash reads as committed on the
next boot.** Its `trx_id` is below the new boot's high-water mark and is in
no live set, so §4.1's predicate admits it. There is no cheap mitigation —
closing it needs a persisted commit watermark, which is recovery. Every
record below is emitted in the shape `wal.md` §12 wants so that recovery is
purely additive. Do not attempt a partial mitigation; a half-persisted
watermark is worse than a documented gap.

---

## Phase T-1 — undo storage and the id sequence

**T01 — undo page layout and the record codec.** — **done.**
Files: `include/kds/txn/undo_page.hpp`, `src/txn/undo_page.cpp`.
`UndoPageHeaderFields` (24 B at `kPageBodyOffset`: flags, nr_records,
`lower` **absolute**, reserved0, owner_trx_id, prev_page_id, reserved1),
`UndoRecordType` (`kInvalid`/`kOverwrite`/`kDeleteMark`/`kInsert`),
the 28-byte unpadded record header, `kUndoRecordsOffset = 56`,
`kUndoPageCapacity = 8136`, `undo_ptr = (page_id << 16) | offset`,
`kNoUndoPtr = 0`, `UndoPtrIsPlausible()`. Field-wise `memcpy` through named
offsets, `offsetof` static_asserts, no bitfields (`rules.md` §§2, 5).
Tests (§10-1): record round-trip; `undo_ptr` packing across the whole page-id
range with the upper 16 bits asserted zero; `kNoUndoPtr` unreachable from
any legal `(page, offset)`; a `kMaxUndoImageLen` image fits and `+1` does
not.
Needs: nothing.

**T02 — `UndoLog`: allocate, append, walk.** — **done.**
Files: `include/kds/txn/undo_log.hpp`, `src/txn/undo_log.cpp`,
`CMakeLists.txt`.
A per-transaction chain of undo pages: `PageStore::CreateNew()` +
`FormatPage(page, PageType::kUndo)` — **never** `CreateNewHeaderless()`,
because the page needs the `page_lsn` the WAL gate reads (§3.1) — linked by
`prev_page_id`, owned by `owner_trx_id`. `Append(record) → undo_ptr` grows
`lower` by exactly `kUndoRecordHeaderSize + image_len`; a full page chains a
new one. `Walk()` follows `prior_undo_ptr` newest→oldest bounded by
`kMaxUndoChainLength = 2^16`, and exceeding it is `Corruption`, not a hang.
WAL per §3.5: `PAGE_INIT{min_key = 0, page_type = kUndo}` on creation, then
`UNDO_WRITE` per append with the envelope's `page_id` naming the **undo**
page and the two chain-link fields carried as payload fields rather than
inside `image`. **No `FULL_PAGE_IMAGE` for undo pages** — reconstructible
from `PAGE_INIT` plus its `UNDO_WRITE`s, so FPI-exempt on the merits.
Tests (§10-1, §10-4): append until `OutOfSpace`; a self-referential link is
`Corruption`; the `UNDO_WRITE` mapping round-trips through
`EncodeUndoWrite`/`DecodeUndoWrite` byte for byte; an undo page image is
rebuilt from records read **off the device** and asserted identical to the
live page.
Needs: T01.

Known ceiling to carry forward, not to fix: undo overhead is 84 bytes
against the heap page's 77, so a tuple within ~7 bytes of the maximum heap
payload cannot be updated — the undo append fails `OutOfSpace` naming the
*undo* page (§3.3). A spilling image or a long-image record type is the
deferred fix.

**T03 — `ReadView` and the visibility predicate.** — **done.**
Files: `include/kds/txn/read_view.hpp`, `include/kds/txn/visibility.hpp`,
`src/txn/visibility.cpp`.
`ReadView` is a copyable POD — `up_to_trx_id`, `own_trx_id`,
`in_flight[kMaxTrackedLiveTxns = 64]` sorted, `in_flight_count` — with no
heap allocation, because the reactor body allocates nothing in steady state.
`Begin` past 64 live transactions is `OutOfSpace`: a documented, testable
bound. `Visible(t)` per §4.1; `kAlwaysVisibleTrxId = catalog::kBootstrapXid`
is visible unconditionally and permanently (§4.2).
The predicate implements §4.3's four steps and is the **first consumer of
`Tuple::deleted`**, which the engine has set and never read. Per **A2** its
signature takes the caller's scratch buffer and the tuple bytes are copied
into it before any undo page is fetched.
Tests (§10-3): `kBootstrapXid` always visible; own writes visible;
`trx_id >= up_to_trx_id` invisible; in-flight invisible; `undo_ptr == 0`
with an invisible writer ⇒ no version; delete-mark by a visible deleter ⇒ no
version, by an invisible one ⇒ the prior version; a 3-version chain read
from three read views yields three payloads; and garbage in the tuple
header's two free bytes changes nothing — the mechanized form of "no `xmax`
anywhere".
Needs: T01, T02.

**T04 — the durable `trx_id` sequence.** — **done.**
Files: `include/kds/server/superblock.hpp`, `src/server/superblock.cpp`,
`include/kds/txn/trx_id.hpp`.
`SuperBlockFields` gains `next_trx_id`; format version **8 → 9**, which
refuses every existing data file at the door exactly as 4→5, 5→6, 6→7 and
7→8 did. `CreateFresh` seeds `kFirstUserTrxId = 2`, so 1 is never reissued
to a real transaction. Allocation is **bump-ahead**: reserve a block of `N`,
persist the ceiling, hand ids out of memory, and burn the remainder on a
crash. `N = 4096` `[PROPOSED]`, taking the floor
`keystoneid-k0-findings.md` measured for the row-id allocator — at N=64 the
same scheme cost 43× and per-id durability 2629×, so the number is a floor
established by measurement, not a preference. Past `kMaxTxnId` (48 bits) is
`OutOfRange`, never wrapped.
Tests (§10-2): monotonic; never 1; never reissued across a simulated
restart; a crash burns the block remainder; past `kMaxTxnId` is
`OutOfRange`.
Needs: nothing (parallel with T01-T03).

## Phase T-2 — visibility on the read path

**T05 — apply the predicate at `AcceptTupleAt`.** — **done.**
Files: `src/exec/step_vm.cpp`, `include/kds/exec/step_vm.hpp`,
`src/server/command_dispatcher.cpp`.
Per **A1**, one call site covers the chain walk, the btree descent, the
probe memo, Waystone replay and the Cabin resolve phase. Per **A2**, the
copy happens before the span is released and the undo walk after. A
`ChainRunner` built without a `ReadView` behaves exactly as it does today —
which is what keeps every pre-existing test passing unchanged, and is not a
special case: a view containing only `kBootstrapXid`-visible rows *is* the
current behaviour.
`HandleUpdate::apply` gets the same call. `HandleCatalogView` does not:
catalog reads do not go through the predicate (§7), and `heap::ChainVisit`
stays a purely physical walk so `storage/` keeps no dependency on `txn/`.
Tests (§10-9): the probe path and the scan path return identical bytes for
the same pk under the same read view, across an unmodified row, an updated
row read with an old view, a delete-marked row read with an old view, a
trail entry cleared on delete, and an entry corrupted to a wrong location.
Extend `waystone_contract_test.cpp` and `cabin_contract_test.cpp` rather
than starting a third suite. Measure the A2 copy on the existing read
benchmark and record it.
Needs: T03.

## Phase T-3 — the manager, the session, the write paths

**T06 — `TransactionManager` and the in-memory trail.** — **done.**
Files: `include/kds/txn/manager.hpp`, `src/txn/manager.cpp`.
`Begin`/`Commit`/`Abort`, the live set, and `MintReadView()` — per statement
under `READ COMMITTED`, once at `BEGIN` under `REPEATABLE READ` (§1). The
conflict check of §5 is a pure function of the current header `trx_id` and
the writer's view: `kAlwaysVisibleTrxId` proceeds, `T` itself proceeds, a
visible `cur` proceeds, anything else is `kTxnConflict`. No lock manager, no
waiting, no deadlock detection, and the Keystone lock byte stays unused.
The trail is in memory and per transaction. **An insert writes no undo
record** (§3.6) — `undo_ptr == 0` plus an invisible writer already means "no
visible version" — so the insert path's cost is unchanged and rollback of an
insert reads the trail. `UndoRecordType::kInsert` stays defined and never
written, so persisting the trail later is a code change and not a
format-version event.
Tests: conflict verdicts over the four arms of §5's table; RC re-snapshots
per statement and RR does not.
Needs: T03, T04.

**T07 — `Session`, and threading it through `Dispatch`.** — **done.**
Files: `include/kds/server/session.hpp`, `include/kds/server/tcp_server.hpp`,
`src/server/tcp_server.cpp`, `include/kds/server/command_dispatcher.hpp`,
`src/server/command_dispatcher.cpp`.
This is the structural change in the whole workplan. `Dispatch(line)` is
stateless today and `TcpServer` shares one dispatcher across every
connection, so there is nowhere for "this connection is inside a
transaction" to live. A `Session` — isolation level, current transaction,
autocommit vs explicit, and the `failed-txn` flag — is owned by
`TcpServer::Connection` and passed to `Dispatch`. Closing a connection with
an open transaction rolls it back.
`failed-txn` admits only `ROLLBACK`/`ABORT`/`SYNC`/`STOP`/`PING`; everything
else is refused until the client rolls back.
Tests (§10-8): the state machine's admitted set; two sessions over one
dispatcher do not interfere; a closed connection rolls back.
Needs: T06.

**T08 — `BEGIN` / `COMMIT` / `ROLLBACK`, and the `isolation` key.** — **done.**
Files: `src/parser/parser.cpp`, `include/kds/parser/ast.hpp`,
`src/server/command_dispatcher.cpp`, `src/server/expeditor.cpp`,
`include/kds/server/expeditor.hpp`, `kds.conf.sample`.
`BEGIN [ISOLATION LEVEL {READ COMMITTED | REPEATABLE READ}]`, `COMMIT`,
`ROLLBACK` (`ABORT` as a synonym), `SET ISOLATION LEVEL ...`. The three-level
precedence chain `durability` already uses: server config → session →
transaction. New config key `isolation`, default `read committed`, added to
`Expeditor::Config::KnownConfigKeys()` — an unknown key is a startup error,
so forgetting this makes every existing config file fail loudly rather than
silently ignore the setting.
**These words are matched by text, not reserved** (`token.hpp`: `SELECT`,
`INSERT`, `UPDATE` are handled the same way), so no `Keyword` enum entry and
no `kFingerprintVersion` bump. `fingerprint.hpp`'s bump rule names this
exact case as one that does *not* require one: nothing already stored
changes meaning.
Tests: the precedence chain; an unknown isolation name is refused with a
position.
Needs: T07.

**T09 — transactional INSERT and UPDATE.** — **done.**
Files: `src/server/command_dispatcher.cpp`.
INSERT stamps the session's real `trx_id` instead of `catalog::kBootstrapXid`
and pushes a trail entry. UPDATE is the larger change: today it is
**unlogged entirely** and carries the old `trx_id`/`undo_ptr` forward
unchanged, so it must gain the §5 conflict check, an `UNDO_WRITE` carrying
the prior tuple image, and a `HEAP_OVERWRITE` — new WAL emission on a path
that emits nothing. Expect a measurable cost and record it beside the
INSERT numbers in `bench/`; do not tune it in this task.
Failure atomicity is **per transaction, not per statement** (§6): an UPDATE
failing on row 7 of 10 inside an explicit transaction leaves 1-6 written and
the session in `failed-txn`. In autocommit the abort is automatic, so
behaviour is statement-atomic there. This is a deviation from SQL that
savepoints would close and that the trail's shape supports additively.
Needs: T06, T07.

**T10 — `DELETE`.** — **done.**
Files: `src/parser/parser.cpp`, `include/kds/parser/ast.hpp`,
`src/parser/ast.cpp`, `src/server/command_dispatcher.cpp`,
`docs/parser-v2.md`.
`DELETE FROM <t> [WHERE <cond> [AND <cond>]*]`, the same WHERE the UPDATE
path compiles through `exec::CompileWhere`, with the same pk-equality fast
path. It delete-marks: `PageView::DeleteMark` gets its first caller, as do
`RecordType::kHeapDeleteMark` and `UndoRecordType::kDeleteMark`, whose image
is empty because a delete-mark changes no tuple bytes.
`DELETE` is a *language* addition, so it lands in `docs/parser-v2.md`'s
grammar too. It needs no fingerprint bump for the reason T08 gives.
**The Cabin write hook does nothing on DELETE** — by `feat-cabin.md` §5,
removal is forbidden, because an older snapshot may still match the row
through the undo chain. The surplus is subtracted at read time, which after
T05 includes the visibility predicate.
Tests: delete-mark then read under an older and a newer view; the Cabin
contract suite gains a DELETE case asserting nothing is removed.
Needs: T06, T07, T09.

**T11 — rollback.** — **done.** The dispatcher drives it now (`ROLLBACK`, and every autocommit failure).
Files: `src/txn/manager.cpp`, `include/kds/wal/payload.hpp`.
`Abort` walks the trail **in reverse** and emits each compensation as an
ordinary logged page mutation, so recovery-driven rollback later reuses this
path verbatim (§6): insert → `RetireSlot` + clear the Waystone entry
(`SLOT_RETIRE`); overwrite → `OverwriteTuple(slot, image, prior_trx_id,
prior_undo_ptr)` (`HEAP_OVERWRITE`); delete-mark → `ClearDeleteMark`
(`HEAP_DELETE_MARK`). `PageView::ClearDeleteMark` does not exist and is
added with this task. Then `TXN_ABORT`, with **no durability wait** — a
transaction whose abort record did not survive is a transaction with no
commit record, which recovery rolls back anyway. Undo pages are not freed.
**Amend `payload.hpp`'s `SLOT_RETIRE` comment**, which currently states that
no transaction owns one. True of a purge pass, false of a rollback
compensation: a `SLOT_RETIRE` emitted by rollback carries the aborting
transaction's id, one emitted by purge carries `kNoTxnId`. Stamping
`kNoTxnId` on a rollback would hide it from recovery's analysis phase.
Tests (§10-7): restores bytes for UPDATE, clears the mark for DELETE,
retires the slot for INSERT; a multi-row multi-statement transaction unwinds
in reverse; the compensation records plus `TXN_ABORT` are read back off the
device.
Needs: T09, T10.

**T12 — the `TXN_CONFLICT` surface.** — **done.**
Files: `src/server/command_dispatcher.cpp`, `include/kds/wire/kwp.hpp`.
```
ERR TXN_CONFLICT retryable=1 row id=42 was written by transaction 118
```
Machine-parsable, keeping the `ERR ` prefix that drives the dispatcher's
Warn-vs-Debug logging, and mapped to `wire::ErrorCategory::kTxnConflict`
with **`retryable = 1`** — `protocol.md` §11 makes that bit part of the
compatibility surface, because financial client libraries build retry loops
on it. A conflict inside an explicit transaction enters `failed-txn`; in
autocommit it aborts immediately.
Tests (§10-6): S1 and S2 both update one row ⇒ S2 gets the conflict and
enters `failed-txn`; after S1 rolls back, S2's retry succeeds; a
same-transaction double update is no conflict, and `ROLLBACK` restores the
*original*, which is what proves the second undo record linked to the first.
Needs: T09.

## Phase T-4 — the suite and the documents

**T13 — the §10 suite.** — **done**, less the two device-level groups noted at the top.
Files: `tests/undo_page_test.cpp`, `tests/undo_log_test.cpp`,
`tests/visibility_test.cpp`, `tests/txn_isolation_test.cpp`,
`tests/txn_conflict_test.cpp`, `tests/txn_rollback_test.cpp`,
`tests/session_test.cpp`, `tests/CMakeLists.txt`.
All nine groups, all deterministic — injected clock, `MemoryPageDevice`,
`MemoryLogDevice`, no sockets (`rules.md` §4). Group 5 is the one worth
naming: **RC vs RR over two sessions on one dispatcher** — under RR, S1
snapshots, S2 commits an update, S1's second `SELECT` still sees the old
value and sees the new one only after `COMMIT`; under RC the same script
sees the new value at the second `SELECT`. The same pair for `DELETE`.
Needs: everything above.

**T14 — the documents.** — **done.**
Files: `CLAUDE.md`, `docs/txn.md`, `docs/parser-v2.md`,
`docs/feat-cabin.md`, `bench/`.
`CLAUDE.md`'s transaction bullet moves from "specified, not built" to built,
keeping §8's crash gap stated precisely. `txn.md` takes amendments **A1**,
**A2** and **A3** into §§4.4, 4.3 and 4.2. `parser-v2.md` takes `DELETE`.
`feat-cabin.md` §1's "per snapshot" stops degenerating to "exists and
matches" and says what it now means. The settled items leave `CLAUDE.md`'s
Open Decisions; **undo retention, `trx_id` wraparound and the cross-core
commit protocol stay open** and are named as still-open here so nobody
reads a built subsystem as a finished one.
Needs: T13.
