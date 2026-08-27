# KDS Transactions & MVCC

How KDS isolates concurrent statements and what a reader sees. `[PROPOSED]` marks a default to confirm or amend before the affected part is built; `[OPEN]` must not be assumed. Companion specs: `docs/spec/wal.md`, `docs/spec/heap-and-tuple.md` (§3.2, the tuple MVCC header).

---

## 1. Isolation levels

KDS supports exactly two isolation levels. **No level was named anywhere in the
documentation before this section**, so this is a new decision rather than a
restatement.

| Level | Read view | Meaning |
|---|---|---|
| `READ COMMITTED` | taken afresh at the start of **every statement** — see the note below on what "statement" means | a statement sees everything committed before it began |
| `REPEATABLE READ` | taken once at `BEGIN`, held for the transaction | every statement in the transaction sees the same database state |

**What takes the boundary** (amended 2026-08-16): the re-mint happens
once per statement, latched by the dispatcher and taken by whichever
reader needs a view first. Before that it was taken only by the routes
reaching `SnapshotFor`/`BeginWrite`, so a statement that resolved a
relation without reading rows — `DESCRIBE`, `SHOW TABLES` — could
resolve under the *previous* statement's view. See
`docs/inflight/in-progress/workplan-ddl-transactional.md` DT3d.

**`READ COMMITTED` is the default.** Rationale: under first-updater-wins with no
waiting (§5), `REPEATABLE READ` holds one read view for the whole transaction and
therefore converts more concurrent writes into retryable aborts; `READ COMMITTED`
re-snapshots per statement and conflicts strictly less. This differs from
MySQL/InnoDB, whose undo-chain shape this engine otherwise follows (`wal.md` §2),
and matches PostgreSQL and Oracle. Settable per server (config key `isolation`),
per session (`SET ISOLATION LEVEL`), and per transaction (`BEGIN ISOLATION
LEVEL ...`) — the same three-level precedence chain `durability` already uses.

`SERIALIZABLE` is out of scope and is **not** `[OPEN]`: it needs predicate
locking or SSI read-tracking, neither of which fits a design with no lock
manager and no row-level read tracking. (§4.1's reader registration is not
that: it records which *snapshots* exist, never which rows they read.)

## 2. MVCC version identity

**Identity is per logical tuple, not per version.** This resolves the `[OPEN]`
item "MVCC version identity semantics (identity per version vs per logical
tuple)".

It is forced by facts already confirmed elsewhere, not chosen freely:

- The primary key cannot be updated — it is the tuple's identity, not a field of
  it (`CLAUDE.md` invariant 10).
- `PageView::OverwriteTuple` is in-place and keeps `(page_id, slot)`, so a
  tuple's physical address survives an update.
- Waystone addresses entries directly by pk (`waystone-concpets.md` §4), and a
  pk names a row, not a version.
- Old versions live in undo pages (§3), where they have no slot and therefore no
  address.

Consequence: a version is only ever "the state of tuple X as of read view R".
Undo records are not independently addressable rows, nothing outside the undo
chain may hold a reference to one, and `undo_ptr` is meaningful only when reached
from the tuple it belongs to.

## 3. Undo storage

Closes `docs/spec/wal.md` §15's "Undo-page layout details (`UNDO_WRITE` targets)".

### 3.1 Undo pages are headered

Not a free choice. `wal.md` §9 already lists **undo** among the
pages carrying the common 32-byte page header, and `docs/spec/page.md` §1 names
Waystone entry and directory pages as the *only* headerless class. Undo pages are
allocated with `PageStore::CreateNew()` and formatted with
`FormatPage(page, PageType::kUndo)` — they need the checksum and, more
importantly, the `page_lsn` the WAL-before-data gate reads, because undo writes
are themselves WAL-logged (`wal.md` §2).

`DevicePageStore::CreateNewHeaderless()` must **not** be used for undo pages.

### 3.2 Page layout

```
byte 0     common page header (32 B)     PageType::kUndo, checksum @4, page_lsn @8
byte 32    UndoPageHeaderFields (24 B)
byte 56    UndoRecord 0, 1, 2, ...       append-only, grows upward to `lower`
byte 8192  end
```

`UndoPageHeaderFields`, all offsets relative to `kPageBodyOffset` (32):

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | `flags` | `kUndoPageFlagInitialized = 0x1` |
| 2 | 2 | `nr_records` | O(1) "is this page empty" for a future purge pass |
| 4 | 2 | `lower` | **absolute** page offset of the next free byte |
| 6 | 2 | `reserved0` | 0 |
| 8 | 8 | `first_trx_id` | the transaction whose append created this page — a diagnostic, **not** an owner |
| 16 | 4 | `prev_page_id` | the log's previous undo page, in creation order |
| 20 | 4 | `reserved1` | 0 |

`kUndoPageHeaderSize = 24`; `kUndoRecordsOffset = 56`;
`kUndoPageCapacity = 8192 - 56 = 8136`.

`lower` is absolute for the same reason `HeapPageHeaderFields::lower` is: it is
compared against `kPageSize` and used directly as a `memcpy` destination, and a
body-relative value would invite one missing `+ kPageBodyOffset`.
**One current page, shared by every transaction.** The log appends each
transaction's records to the same page until it fills, then chains a new one
behind it through `prev_page_id`. This is a change of 2026-08-05: a page *was*
owned by one transaction, and since an autocommitted statement is a
transaction, that cost a fresh 8 KB page per `UPDATE` for one ~88-byte record —
132 MB of data file for 16,414 updates, and the instance's whole ~510 MB
page-id space exhausted after ~65,000 of them, after which every further write
failed (`bench/results-txn-layer-budget.md` §3). Sharing puts ~92 records on a
page: the same 45-second workload now writes 19 MB instead of 510 MB and
sustains 1,344 TPS instead of 716 with 97,826 failures.

Nothing was relying on the exclusivity. A reader follows `undo_ptr`, which
names a page and an offset directly; rollback replays the transaction's
in-memory trail (§3.6) and never walks undo pages; redo names each record's
offset explicitly, so interleaved writers replay onto one page in LSN order
correctly. Exclusivity would only have let a purge free a transaction's pages
without a side table — and the purge that now exists (2026-08-19,
`docs/inflight/in-progress/workplan-undo-purge.md`) confirms the analysis: it frees by a
per-page bound rather than by owner, because a page's records outlive
their writer. The bound lives **in memory** beside this run's chain, not
in the header — it is a 48-bit writer id and `reserved1` is 32 bits — so
`reserved1` stays reserved.

`prev_page_id` therefore chains the log's pages in creation order and no longer
answers "which pages are this transaction's" — which sharing makes
unanswerable. The on-disk layout is unchanged: same offsets, same widths, two
fields with new meanings, so no format version moves. **The purge does not read
it**: a reclaimed page is re-linked without the link that pointed at it being
rewritten, so a device walk can revisit a reused page — the on-disk chain is
historical once reuse starts, and `UndoLog::PageCount()` counts the in-memory
chain instead.

### 3.3 Undo record

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `prior_trx_id` — writer of the version being superseded |
| 8 | 8 | `prior_undo_ptr` — its own predecessor; `kNoUndoPtr` ends the chain |
| 16 | 4 | `target_page_id` — the heap page holding the tuple |
| 20 | 2 | `target_slot` |
| 22 | 2 | `image_len` |
| 24 | 1 | `type` — `UndoRecordType` |
| 25 | 1 | `flags` — 0 |
| 26 | 2 | `reserved` — 0 |
| 28 | — | before-image bytes begin |

`kUndoRecordHeaderSize = 28`. Records are **unpadded**: every access is a
field-wise `memcpy` (`rules.md` §2), so alignment buys nothing and 8-byte padding
would waste up to 7 bytes on a page holding ~290 records. `lower` advances by
exactly `kUndoRecordHeaderSize + image_len`.

```
UndoRecordType: kInvalid = 0
                kOverwrite = 1    image = the full prior tuple payload
                kDeleteMark = 2   image empty - a delete-mark changes no bytes
                kInsert = 3       image empty - DEFINED, NOT WRITTEN (§3.6)
```

**Known ceiling.** Undo overhead is 32 + 24 + 28 = 84 bytes against the heap
page's 32 + 16 + 5 + 20 + 4 = 77, so a tuple within ~7 bytes of the maximum heap
payload cannot be updated: the undo append fails `OutOfSpace` naming the *undo*
page. Deferred fix: a spilling image or a long-image record type.

### 3.4 `undo_ptr` encoding

```
undo_ptr = (uint64(page_id) << 16) | offset
```

The page id occupies bits 16..47, so bits 48..63 are always zero — the same
zero-extension convention invariant 6 imposes on ids and `trx_id`.

**`kNoUndoPtr = 0` means "no predecessor", and it is unambiguous
structurally** rather than by convention: page 0 is the superblock, and offset 0
is inside the common page header, below `kUndoRecordsOffset` (56). Neither can
ever name a real undo record. `UndoPtrIsPlausible()` reports `Corruption` for
page 0, an offset outside `[kUndoRecordsOffset, kPageSize - kUndoRecordHeaderSize]`,
or nonzero upper 16 bits.

### 3.5 WAL mapping — the record's tail, not its image

The existing payload (`include/kds/wal/payload.hpp`) fits without amendment:

```
envelope : {type = kUndoWrite, txn_id = the writing transaction,
            page_id = the UNDO page, not the heap page}
payload.prior_trx_id   = record +0
payload.prior_undo_ptr = record +8
payload.offset         = the record's offset within its undo page
payload.tail           = record bytes [+16, +28 + image_len)
```

The two chain-link fields are carried as payload *fields* and not repeated inside
the tail, which is exactly why the payload's existing comment — "the one exception
is `UNDO_WRITE`'s *prior* writer, which is a different transaction from the one
that wrote the record" — is already correct.

**Everything else about the record is inside the tail, and that is the point.**
Bytes `[+16, +28)` are `target_page_id`, `target_slot`, `image_len`, `type`,
`flags` and `reserved` — the fields that say **which tuple** a before-image
belongs to. Without them a chain rebuilt by redo names no row, and the undo
phase would restore the wrong one rather than fail.

> **Corrected 2026-08-10.** This section was right and the code was not:
> `UndoLog::LogUndoWrite` logged the bare before-image, so those three fields
> lived on the page and nowhere in the log. Found building
> `docs/workplan-wal-recovery.md` RC03, whose redo applier could not be
> written against it. The writer now logs the tail this section always
> specified, through the one `txn::EncodeUndoRecordTail` /
> `DecodeUndoRecordTail` pair that redo reads back — one shape, two callers.
> The payload's length field is renamed `tail_len` accordingly, since it now
> counts `12 + image_len` bytes and a field called `image_len` that does not
> hold one is the kind of trap this codebase refuses.
>
> **No format version moved**, and the reason is narrow: nothing has ever read
> the log back, so no stream in existence is interpreted under the old reading.
> The same change after recovery ships would be a format event.

`lower` and `nr_records` are derivable by replaying a page's `UNDO_WRITE`s in LSN
order, so no undo-page-header record type is needed. Page creation logs
`PAGE_INIT{min_key = 0, page_type = kUndo}`; `PageInitPayload` already provides
for `min_key` 0 on non-heap page types.

**No `FULL_PAGE_IMAGE` for undo pages.** An undo page is fully reconstructible
from its `PAGE_INIT` plus its `UNDO_WRITE`s, which makes it FPI-exempt on the
merits rather than by omission.

### 3.6 INSERT writes no undo record

A tuple with `undo_ptr == kNoUndoPtr` whose writer is invisible means "inserted
by a transaction I cannot see" ⇒ no visible version. That is sound because the
only writer that ever leaves `undo_ptr == 0` is an insert, and every
pre-existing row carries `trx_id == 1`, which is always visible (§4.2).

Rollback of an insert therefore uses the transaction's **in-memory** undo trail
rather than an undo record, which keeps the insert path's cost unchanged.
`UndoRecordType::kInsert` is defined but never written, so that persisting the
insert trail — which recovery-driven rollback will need — is a code change and
not a format-version event.

> **Reversed 2026-08-11 by RV10** (`docs/workplan-wal-recovery.md` §3), and
> the trade is being paid rather than merely reconsidered. The paragraph
> above is correct about *visibility* — an insert genuinely needs no undo
> record to be read correctly — and that is what made "no record" look like
> a free choice. It is not free for **recovery**, and for a reason wider
> than the insert itself.
>
> This engine has two undo chains and `undo_log.hpp` says outright that they
> are not the same chain: `prev_page_id` (page → page, creation order) and
> `prior_undo_ptr` (record → record, **one tuple's versions**). Neither is
> per-transaction, `UndoRecordFields` carries no owning transaction id, and
> `CHECKPOINT_BEGIN`'s active list is bare ids. So after a crash the only
> way to learn what a loser wrote is the WAL records inside the replay
> range — and that range does not cover it. A page written back before a
> checkpoint has its recLSN cleared, so the redo start can advance past a
> still-uncommitted write, and the record naming it is never scanned. The
> row survives, and §8's gap then reads it as **committed**.
>
> RV10's answer is a third chain: `UndoRecordFields` gains
> `txn_prev_undo_ptr`, `CHECKPOINT_BEGIN`'s active-transaction table gains
> each transaction's `last_undo_ptr` as the durable head, and `kInsert` is
> written — carrying the row's `pk`, since an insert has no before-image for
> the identity check `Compensate` now makes (§6). An insert that wrote no
> record would break the chain and orphan everything the transaction did
> before it, which is the reason the record exists that "somewhere to put
> the fact" never gave it.
>
> **The cost §3.6 avoided is real and is now measured**:
> `bench/results-txn-layers.md` puts the WAL-append phase at 0.95 µs for
> INSERT against 5.38 µs for UPDATE, on a 951 µs logged statement whose
> fsync is 933.69 µs — so ~0.5 % at the shipped `group` default, and up to
> ~29 % unlogged, where the same phase is 0.06 µs against 2.14 µs on a
> 7.21 µs statement. A `kInsert` record carries no image, so that is an
> upper bound. `docs/workplan-wal-recovery.md` RC06 measures the real
> figure in `build-release` before the change is kept.

### 3.7 Chain walks

`UndoLog::Walk` follows `prior_undo_ptr` newest→oldest, bounded by
`kMaxUndoChainLength = 2^16`. Exceeding the bound is `Corruption`, not a hang —
the same guard `kMaxChainPages` provides for the heap chain.

## 4. Snapshots and visibility

### 4.1 `ReadView`

```
up_to_trx_id     exclusive high-water: ids >= this had not started
own_trx_id       0 for a read-only view
in_flight[64]    sorted; kMaxTrackedLiveTxns = 64, as kMaxWalCores
in_flight_count
```

A copyable POD with no heap allocation — the reactor body allocates nothing in
steady state (`sched.md`). `Begin` past `kMaxTrackedLiveTxns` is `OutOfSpace`: a
documented, testable bound rather than an unbounded vector.

```
Visible(t):  t == kAlwaysVisibleTrxId -> true
             t == own_trx_id          -> true
             t >= up_to_trx_id        -> false
             otherwise                -> not in in_flight
```

**Why no commit table is needed, and the condition on that.** "Committed before
my snapshot" collapses to "below the high-water mark and not in my in-flight set"
*only* because an aborted transaction's page changes are physically undone,
synchronously, in-process (§6). That is the load-bearing assumption of the whole
design, and §8 states the crash consequence it implies. It is the single thing
recovery must revisit.

Readers **are registered** as of 2026-08-19
(`docs/workplan-reader-registration.md`). Two records together name every
reader on a core: live transactions in the manager's `live_`, and every
other snapshot that can read a superseded version across a park — an
autocommit statement's, a shipped pipeline stage's — through a move-only
`ReaderLease` that `txn::AutocommitSnapshot` now returns beside the
snapshot, so registering is structural rather than disciplinary.
`TransactionManager::ReadHorizon()` folds both into one bound: a version
superseded by a **committed** transaction below it is invisible to every
live and future view, so a purge may retire it. Views exempt by proof:
latest-state check views (they never read a superseded version) and views
that never outlive one synchronous span on the core's single thread — an
exemption to re-check whenever the executor gains a suspension point.
The horizon is **per-core**, sound while every reader reads its own
core's versions (CC3/CC4); a cross-core writer must extend it.

Two purges consume the horizon: the catalog delete-mark purge
(`ddl-transactional.md` §5d) and, **as of 2026-08-19, the undo
purge** (`docs/inflight/in-progress/workplan-undo-purge.md`, ratified horizon-only /
internal-recycle / purge-on-growth): a settled undo page — newest writer
below the horizon — recycles into the log's own next growth, so this
run's chain plateaus instead of growing without bound. The policy is
deliberately horizon-only, so `SnapshotTooOld` **stays structurally
unreachable**: nothing a live view can reach is ever freed, and the
price is that one long-running transaction holds reclamation for its
lifetime. A byte-cap retention that makes the error reachable was
drafted and declined for v1 — the workplan's D1 records both sides.

### 4.2 The always-visible transaction id

`trx_id == 1` (`catalog::kBootstrapXid`) is visible to **every** read view,
unconditionally and permanently. This is not a migration shim that ages out:

- Every row written before the transaction manager existed carries it (`HandleInsert` passed
  `kBootstrapXid` for all of them).
- Every catalog row carries it **forever**, because catalog writes use
  `kBootstrapXid` and catalog in-place updates carry the old header forward (§7).
- It is the tail of every undo chain built over a pre-existing row.

`SuperBlock::CreateFresh` seeds `next_trx_id = kFirstUserTrxId = 2`, so 1 is
never reissued to a real transaction. The field was added in superblock
format version **9** and lives past the WAL anchor table
(`kNextTrxIdOffset`); ids are handed out a block at a time
(`txn::TrxIdSequence`, `kTrxIdBlockSize = 4096` `[PROPOSED]`), so a crash
burns the block's remainder - ids are unique and monotonic, never gapless,
the same promise the row-id sequence makes. The superblock is unlogged, so
a crash between raising the ceiling and the page reaching the platter
reissues the block; that is the exposure `keystoneid-k0-findings.md`
records for row ids, and it closes the same way, with recovery. This mirrors PostgreSQL's
`FrozenTransactionId`, which is what `kBootstrapXid`'s own comment already says.

### 4.3 The predicate

Given a read view and a `(PageView, slot)`:

1. `ReadTuple(slot)` — `NotFound` (out of range, or `kSlotFlagDead`) ⇒ no version.
2. If the candidate's `trx_id` is visible: the version exists iff it is not
   delete-marked. Done.
3. Else if `undo_ptr == kNoUndoPtr`: an insert by an invisible writer ⇒ no
   version. Done.
4. Else step back one undo record and repeat from 2:
   - `kOverwrite` → payload becomes the record's image; not deleted
   - `kDeleteMark` → **keep the current payload** (a delete-mark changes no tuple
     bytes; if a later overwrite changed them, the newer undo record already
     restored them on the way down); not deleted
   - `kInsert` → the version did not exist ⇒ no version
   
   In every case `trx_id = prior_trx_id`, `undo_ptr = prior_undo_ptr`.

Every chain terminates definitively: at an always-visible `trx_id == 1` version,
at `undo_ptr == 0`, or at a `kInsert` record.

The predicate is the **first consumer of `Tuple::deleted`**, which the engine has
set and never read.

### 4.4 Where it is applied

**Amended 2026-08-04 (`txn-workplan.md` A1).** This section was written
before the step VM existed, and named `HandleSelect`'s `emit` and
`HandleUpdate`'s `apply` as the two sites. Every SELECT-class read now goes
through a compiled step chain, so the choke point is
**`ChainRunner::AcceptTupleAt()`** (`src/exec/step_vm.cpp`) — one call site,
reached by the chain walk, the btree descent, the probe memo, Waystone
replay and the Cabin resolve alike. That makes `waystone-concpets.md` §3.1
rule 2 — "MVCC visibility is applied exactly as it would be on the
authoritative path" — true **by construction** rather than by discipline,
and over three consumers this document did not know about.

`HandleUpdate` and `HandleDelete` keep a call of their own, because neither
compiles to a chain. Both reach the same `txn::Classify`, never a second
predicate.

**The predicate is split in two, and the split is not a style choice.**
Stepping back an undo record is a page fetch, and `parser-v2.md` I15's R1
forbids one while a page-frame span is live — which is exactly the state
`AcceptTupleAt` decodes in. So `Classify()` answers with no fetch (safe
under the span), and `ResolveThroughUndo()` walks after the span is
released, over a copy of the tuple taken while it was still held. The copy
is a fixed number of bytes because invariant 13 makes a row's size a schema
constant, and it is taken **only** when the writer is invisible — a visible
writer, which is every row of a single-transaction workload and every
catalog row forever, costs one integer comparison and no copy at all.

`heap::ChainVisit` remains a purely physical walk. Visibility belongs to its
callback: that keeps `storage/` free of a dependency on `txn/`, and keeps
`ChainVisit` usable by the catalog, which must not filter.

## 5. Write conflicts — first-updater-wins

No lock manager, no waiting, no deadlock detection, and the Keystone lock byte
stays unused. A conflict is detected from the tuple header alone. For writer `T`
with read view `V` over the *current* header `trx_id` (`cur`):

| `cur` | Verdict |
|---|---|
| `kAlwaysVisibleTrxId` | proceed — pre-existing or catalog-stamped row |
| `T` itself | proceed — my own earlier write; the new undo record links to the old one, so rollback unwinds both and lands on the original |
| visible to `V` | proceed — `cur` committed before my read view, so I am the first updater since it |
| otherwise | **conflict** — `cur` is either still in flight, or committed after my read view |

Under `REPEATABLE READ` this is exactly first-updater-wins. Under `READ
COMMITTED` the last arm can still fire in the narrow window between a statement's
snapshot and its write; KDS aborts retryably rather than re-reading. That is
stricter than PostgreSQL's `READ COMMITTED` and is a deliberate simplification —
there is no re-read loop and no lock to wait on.

The engine reports `StatusCode::kTxnConflict`, which maps to the
wire contract `wire::ErrorCategory::kTxnConflict` with **`retryable = 1`**
(`protocol.md` §11: "financial client libraries build retry loops on this bit, so
it is part of the compatibility surface"). On the newline protocol the spelling
is machine-parsable and keeps the `ERR ` prefix that drives the dispatcher's
Warn-vs-Debug logging:

```
ERR TXN_CONFLICT retryable=1 row id=42 was written by transaction 118
```

A conflict inside an explicit transaction puts the session in `failed-txn`; in
autocommit it aborts immediately.

## 6. Rollback

`Abort` walks the transaction's trail **in reverse** and emits each compensation
as an ordinary logged page mutation — the shape `wal.md` §12-3 asks for, so that
recovery-driven rollback later reuses this code path verbatim:

| Trail entry | Compensation | Record |
|---|---|---|
| insert | `RetireSlot` + clear the Waystone entry | `SLOT_RETIRE` |
| overwrite | `OverwriteTuple(slot, image, prior_trx_id, prior_undo_ptr)` | `HEAP_OVERWRITE` |
| delete-mark | `ClearDeleteMark(slot, prior_trx_id, prior_undo_ptr)` | `HEAP_DELETE_MARK` |

Then `TXN_ABORT`, with no durability wait — a transaction whose abort record did
not survive is a transaction with no commit record, which recovery rolls back
anyway. Undo pages are not freed; purge is a non-goal (§9).

**Amendment to `SLOT_RETIRE`'s `txn_id` semantics.** `payload.hpp` currently
states that no transaction owns a `SLOT_RETIRE`, so its envelope carries
`kNoTxnId`. That is true of a purge pass and false of a rollback compensation,
which *is* owned by the aborting transaction — stamping `kNoTxnId` would hide the
rollback from recovery's analysis phase. Today: **a `SLOT_RETIRE`
emitted by rollback carries the aborting transaction's id; one emitted by a purge
pass carries `kNoTxnId`.**

**Failure atomicity is per transaction, not per statement.** An `UPDATE` that
fails on row 7 of 10 inside an explicit transaction leaves rows 1-6 written and
the session in `failed-txn`; the client must `ROLLBACK`, which undoes all six. In
autocommit the abort is automatic, so behaviour is statement-atomic there. This
deviates from SQL's statement atomicity, which needs savepoints or a
statement-level trail high-water mark — a non-goal that the trail's shape
supports additively. It is nonetheless a strict improvement on the previous
behaviour, whose own comment read "Partial by design: rows updated before the
failure stay updated."

## 7. Catalog and DDL

Nothing in `src/catalog/` participates in transactions:

- Every catalog write is stamped `kBootstrapXid` and every catalog in-place
  update carries the old header forward, so catalog rows keep `trx_id == 1` and
  `undo_ptr == 0` permanently and are visible to every read view (§4.2).
- Catalog reads do not go through the visibility predicate — they scan pages
  directly.
- Therefore **DDL is neither logged nor transactional, and `CREATE TABLE` inside
  an explicit transaction is not rolled back by `ROLLBACK`.** A known limitation,
  consistent with the catalog's existing instance-scoped coherency caveat.

**Amended 2026-08-15, and the sentence above is now false in part.**
`CREATE TABLE` inside an explicit transaction **is** rolled back by
`ROLLBACK` as of workplan DT3b - the catalog rows it wrote are registered
on the transaction's trail and retired by `Abort`'s existing
compensation. What is *not* yet true is isolation at the SQL surface
(another session can still see the uncommitted relation, DT3c) and
durability (below). Atomicity,
isolation and consistency for DDL are specified in
`docs/spec/ddl-transactional.md` and built per
`docs/inflight/in-progress/workplan-ddl-transactional.md` — by stamping catalog rows with the
real transaction id and filtering catalog reads through the same
visibility predicate user reads use, which is why *live* rollback needs
no undo record. **Durability joined 2026-08-19** (RV3,
`docs/workplan-rv3-catalog-recovery.md`): catalog writes log the
ordinary record types, every DDL statement — autocommit included — runs
under a real transaction, and a crash loser's catalog writes *do* carry
undo records now, appended inside the write points so they precede the
row records in the log; recovery's ordinary undo phase rolls them back.
`SHOW META` prints `ddl_durable=1 catalog_recovered=1`. What stays
unlogged is named in `wal.md` §11a's closing paragraph.

## 8. MVCC ships before recovery — a known correctness gap

An explicit transaction spans reactor iterations, so a `system`-group checkpoint
can flush pages holding uncommitted tuples. WAL-before-data still holds:
`page_lsn` is stamped and the store's gate applies, and `wal.md` §12-3's undo
phase is exactly what exists to clean such pages up on restart.

**But recovery does not exist.** After a crash mid-transaction and a restart, the
uncommitted row's `trx_id` is below the new boot's `next_trx_id` high-water mark
and appears in no live set, so §4.1's predicate reads it as **committed**.

There is no cheap mitigation. Making it read as invisible requires a persisted
"committed up to" watermark, which is recovery. This is accepted deliberately:
records are emitted in the shape §12 wants, so recovery is purely additive, and
this is the same class of exposure the engine already carries (unlogged `UPDATE`,
no replay) — now stated precisely rather than left to be discovered.

## 9. Open Decisions — do not assume

Carried over from `docs/spec/wal.md` §15 and `CLAUDE.md`, still open, with the seam
that keeps each one viable:

- ~~**Undo retention policy** and `SnapshotTooOld` surfacing~~ — **decided
  and built 2026-08-19** (`docs/inflight/in-progress/workplan-undo-purge.md` D1-D3, §4.1):
  horizon-only retention, pages recycling within the log, triggered by
  growth. `SnapshotTooOld` stays structurally unreachable *by that
  decision* — surfacing it belongs to the byte-cap policy D1 declined,
  and reopens with it, generation-stamped pages and all. Still open
  underneath: UP4's mount-time reclaim of a previous run's pages (they
  leak today, as they always have) and any `maintenance`-group cadence
  beyond the growth trigger.
- **48-bit `trx_id` wraparound / epoch handling.** Exhaustion is reported
  `OutOfRange` and never wrapped, exactly as the row-id sequence does.
- **Cross-core transaction commit protocol** — `wal.md` §3 says "do not design it
  now". One `WalManager` owns one stream; a transaction spanning cores is not
  representable. Everything here is core-local.
- **Buffer-pool page-frame reclamation** under the page-latch model.
- **Page compaction / free-space reuse.** `heap-and-tuple.md` §3.1b says
  compaction "needs a transaction manager to know no snapshot still needs the
  bytes". This manager **can now answer that** (§4.1's `ReadHorizon()`), but
  the split policy it interacts with is open.

Explicitly **not** open, and out of scope: `SERIALIZABLE` (§1), savepoints and
statement-level rollback (§6), lock-based blocking (§5).

**Two items left this list rather than staying on it, which is worth
noting about the list itself — it records priorities, not
impossibilities.** *Recovery* (§8) was built (RC01-RC11,
`docs/workplan-wal-recovery.md`). *Transactional DDL* (§7) was reopened
by direction 2026-08-15 and is specified in
`docs/spec/ddl-transactional.md`; its atomicity/isolation half is being
built, its durability half is not (that is RV3, and §7's paragraph
still holds for it).

## 10. Testing Requirements

All deterministic — injected clock, `MemoryPageDevice`, `MemoryLogDevice`, no
sockets (`rules.md` §4).

1. **Undo codec & addressing:** record round-trips; `undo_ptr` packing over the
   whole page-id range with upper-16-bits-zero asserted; `kNoUndoPtr` unreachable
   from any legal `(page, offset)`; append until `OutOfSpace`; a
   `kMaxUndoImageLen` image fits and `+1` does not; a self-referential link is
   `Corruption`, not a hang; and the `UNDO_WRITE` mapping round-trips through
   `EncodeUndoWrite`/`DecodeUndoWrite` byte-for-byte.
2. **Txn ids:** monotonic, never 1, never reissued across a simulated restart;
   a crash burns the block remainder; past `kMaxTxnId` is `OutOfRange`.
3. **Visibility (satisfies `wal.md` §16-5):** `kBootstrapXid` always visible; own
   writes visible; `trx_id >= up_to_trx_id` invisible; in-flight invisible;
   `undo_ptr == 0` with an invisible writer ⇒ no version; delete-mark by a
   visible deleter ⇒ no version; by an invisible deleter ⇒ prior version visible;
   a 3-version chain read from three read views yields three payloads; and
   garbage written into the tuple header's two free bytes changes nothing — the
   mechanized form of "no `xmax` anywhere".
4. **§16-5 end to end:** a delete-mark by a winner survives its commit; by a
   loser is cleared by undo; and the `UNDO_WRITE` records read **off the device**
   are decoded, an undo page image rebuilt from those records alone, and the
   reader fixture run over the rebuilt chain, asserted identical to the live page.
5. **RC vs RR:** two sessions on one dispatcher. RR — S1 snapshots, S2 commits an
   update, S1's second `SELECT` still sees the old value, and sees the new one
   only after `COMMIT`. RC — the same script, where the second `SELECT` sees the
   new value. Same pair for `DELETE`.
6. **Conflicts:** S1 and S2 both update one row ⇒ S2 gets `TXN_CONFLICT
   retryable=1` and enters `failed-txn`; after S1 rolls back, S2's retry
   succeeds. Same-transaction double update ⇒ no conflict, and `ROLLBACK`
   restores the *original*.
7. **Rollback:** restores bytes for `UPDATE`, clears the mark for `DELETE`,
   retires the slot for `INSERT`; a multi-row multi-statement transaction unwinds
   in reverse; the compensation records plus `TXN_ABORT` are read back off the
   device.
8. **Session state machine:** `failed-txn` admits only `ROLLBACK`/`ABORT`/`SYNC`/
   `STOP`/`PING`; two sessions over one dispatcher do not interfere; closing a
   connection with an open transaction rolls it back.
9. **Waystone equivalence:** the probe path and the scan path return identical
   bytes for the same pk under the same read view, across an unmodified row, an
   updated row read with an old view, a delete-marked row read with an old view,
   an entry cleared by `OnDelete`, and an entry corrupted to a wrong location.
