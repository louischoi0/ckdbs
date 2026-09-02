# KDS Transactions & MVCC

How KDS isolates concurrent statements and what a reader sees. `[PROPOSED]` marks a default to confirm or amend before the affected part is built; `[OPEN]` must not be assumed. Companion specs: `docs/spec/wal.md`, `docs/spec/heap-and-tuple.md` (§3.2, the tuple MVCC header), `docs/spec/cross-owner-txn.md` (a transaction that touches more than one core).

---

## 1. Isolation levels

KDS supports exactly two isolation levels.

| Level | Read view | Meaning |
|---|---|---|
| `READ COMMITTED` | taken afresh at the start of **every statement** — see the note below on what "statement" means | a statement sees everything committed before it began |
| `REPEATABLE READ` | taken once at `BEGIN`, held for the transaction | every statement in the transaction sees the same database state |

**What takes the boundary**: the re-mint happens once per statement,
latched by the dispatcher and taken by whichever reader needs a view
first — so a statement that resolves a relation without reading rows
(`DESCRIBE`, `SHOW TABLES`) resolves under its own view, never the
previous statement's.

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

**Identity is per logical tuple, not per version.** It is forced by facts
confirmed elsewhere, not chosen freely:

- The primary key cannot be updated — it is the tuple's identity, not a field of
  it (`CLAUDE.md` invariant 11).
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

### 3.1 Undo pages are headered

`wal.md` §9 lists **undo** among the pages carrying the common 32-byte page
header, and `docs/spec/page.md` §1 names Waystone entry and directory pages as
the *only* headerless class. Undo pages are allocated with
`PageStore::CreateNew()` and formatted with `FormatPage(page, PageType::kUndo)`
— they need the checksum and, more importantly, the `page_lsn` the
WAL-before-data gate reads, because undo writes are themselves WAL-logged
(`wal.md` §2).

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
| 2 | 2 | `nr_records` | O(1) "is this page empty" |
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
behind it through `prev_page_id`. Nothing relies on a page having one owner:
a reader follows `undo_ptr`, which names a page and an offset directly;
rollback replays the transaction's in-memory trail (§6) and never walks undo
pages; redo names each record's offset explicitly, so interleaved writers
replay onto one page in LSN order. The purge (§4.1) frees by a per-page
bound — the page's newest writer — rather than by owner, because a page's
records outlive their writer. That bound lives **in memory** beside this
run's chain, not in the header — it is a 48-bit writer id and `reserved1` is
32 bits — so `reserved1` stays reserved.

`prev_page_id` chains the log's pages in creation order and does not answer
"which pages are this transaction's". **The purge does not read it**: a
reclaimed page is re-linked without the link that pointed at it being
rewritten, so the on-disk chain is historical once reuse starts, a device
walk can revisit a reused page, and `UndoLog::PageCount()` counts the
in-memory chain instead.

### 3.3 Undo record

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `prior_trx_id` — writer of the version being superseded |
| 8 | 8 | `prior_undo_ptr` — its own predecessor; `kNoUndoPtr` ends the chain |
| 16 | 4 | `target_page_id` — the heap page holding the tuple, or the **kVarHeap** page holding the value, for `kVarHeapAppend` |
| 20 | 2 | `target_slot` |
| 22 | 2 | `image_len` |
| 24 | 1 | `type` — `UndoRecordType` |
| 25 | 1 | `flags` — 0 |
| 26 | 2 | `reserved` — 0 |
| 28 | 8 | `txn_prev_undo_ptr` — the writing transaction's previous undo record; `kNoUndoPtr` for its first |
| 36 | 8 | `pk` — the Keystone id of the row this record is about, zero-extended (invariant 7) |
| 44 | — | before-image bytes begin |

`kUndoRecordHeaderSize = 44`. Records are **unpadded**: every access is a
field-wise `memcpy` (`rules.md` §2), so alignment buys nothing and 8-byte padding
would waste up to 7 bytes on a page holding ~290 records. `lower` advances by
exactly `kUndoRecordHeaderSize + image_len`. `txn_prev_undo_ptr` and `pk` sit
at unaligned offsets: the codec memcpy's each field through its offset
constant and never lays the struct over page bytes.

**Three chains, none the same.** `prev_page_id` is page → page in creation
order; `prior_undo_ptr` is record → record over *one tuple's versions*;
`txn_prev_undo_ptr` is record → record over *one transaction's writes*,
walked from the head that `CHECKPOINT_BEGIN`'s active-transaction table
carries per transaction (`last_undo_ptr`). The third is the only durable
answer to "what did this transaction write": the WAL inside the replay range
does not cover a write whose page was written back before a checkpoint, so
recovery's undo phase walks this chain rather than the log.

**`pk` is the identity check.** Compensation proves it is writing the row it
means to before it writes: a btree leaf division moves tuples and renumbers
slots, so `(target_page_id, target_slot)` is where the row *was*
(`TransactionManager::Compensate`, `txn::RecoveryUndo`). The live path reads
the pk from its in-memory trail; recovery has only this record, and
`kDeleteMark` and `kInsert` carry no image to recover one from, so every type
carries it.

```
UndoRecordType: kInvalid = 0
                kOverwrite = 1    image = the full prior tuple payload
                kDeleteMark = 2   image empty - a delete-mark changes no bytes
                kInsert = 3       image empty - the chain link of an insert (§3.6)
                kVarHeapAppend = 4  image empty - a spilled value this
                                    transaction wrote; compensated by
                                    releasing its slot
```

**`kVarHeapAppend`'s target is a value, not a row**, and that is the one
thing separating it from its three siblings: `target_page_id` names a
`kVarHeap` page, so the pk identity check every other compensation makes
neither applies nor could run — a `heap::PageView` over var-heap bytes reads
a slot directory that is not there. Both compensation paths,
`TransactionManager::Compensate` and `txn::RecoveryUndo`, therefore handle it
**before** the page is opened as a heap page. `pk` is still carried, for the
diagnostic, and is never checked. The record exists because an undo record is
a link in the writing transaction's chain (§3.6): a write that produced none
would orphan everything the transaction did before it. A crash between a
`VARHEAP_APPEND` and its tuple write therefore rolls back like any other
loser write.

**The type list is a whitelist in code, in one place.** `IsWritableUndoRecordType`
(`undo_page.hpp`) is consulted by both the appender and the decoder, so a
type admitted by one cannot be refused by the other.

**Known ceiling.** Undo overhead is 32 + 24 + 44 = 100 bytes against the heap
page's 32 + 16 + 5 + 20 + 4 = 77, so a tuple within 23 bytes of the maximum
heap payload cannot be updated: the undo append fails `OutOfSpace` naming the
*undo* page (`UndoPageTest.TheWidestHeapTupleCannotBeUndone` pins the number).

### 3.4 `undo_ptr` encoding

```
undo_ptr = (uint64(page_id) << 16) | offset
```

The page id occupies bits 16..47, so bits 48..63 are always zero — the same
zero-extension convention invariant 7 imposes on ids and `trx_id`.

**`kNoUndoPtr = 0` means "no predecessor", and it is unambiguous
structurally** rather than by convention: page 0 is the superblock, and offset 0
is inside the common page header, below `kUndoRecordsOffset` (56). Neither can
ever name a real undo record. `UndoPtrIsPlausible()` reports `Corruption` for
page 0, an offset outside `[kUndoRecordsOffset, kPageSize - kUndoRecordHeaderSize]`,
or nonzero upper 16 bits.

### 3.5 WAL mapping — the record's tail, not its image

The payload (`include/kds/wal/payload.hpp`):

```
envelope : {type = kUndoWrite, txn_id = the writing transaction,
            page_id = the UNDO page, not the heap page}
payload.prior_trx_id   = record +0
payload.prior_undo_ptr = record +8
payload.offset         = the record's offset within its undo page
payload.tail_len       = 28 + image_len
payload.tail           = record bytes [+16, +44 + image_len)
```

The two chain-link fields are carried as payload *fields* and not repeated inside
the tail, which is why the payload's comment — "the one exception is
`UNDO_WRITE`'s *prior* writer, which is a different transaction from the one
that wrote the record" — is correct.

**Everything else about the record is inside the tail, and that is the point.**
Bytes `[+16, +44)` are `target_page_id`, `target_slot`, `image_len`, `type`,
`flags`, `reserved`, `txn_prev_undo_ptr` and `pk` — the fields that say
**which tuple** a before-image belongs to and which transaction wrote it.
Without them a chain rebuilt by redo names no row, and the undo phase would
restore the wrong one rather than fail. The writer and redo share the one
`txn::EncodeUndoRecordTail` / `DecodeUndoRecordTail` pair — one shape, two
callers.

`lower` and `nr_records` are derivable by replaying a page's `UNDO_WRITE`s in LSN
order, so no undo-page-header record type is needed. Page creation logs
`PAGE_INIT{min_key = 0, page_type = kUndo}`; `PageInitPayload` provides for
`min_key` 0 on non-heap page types.

**No `FULL_PAGE_IMAGE` for undo pages.** An undo page is fully reconstructible
from its `PAGE_INIT` plus its `UNDO_WRITE`s, which makes it FPI-exempt on the
merits rather than by omission.

### 3.6 Every write leaves an undo record, the insert included

A tuple with `undo_ptr == kNoUndoPtr` whose writer is invisible means "inserted
by a transaction I cannot see" ⇒ no visible version. That is sound because the
only writer that ever leaves `undo_ptr == 0` is an insert, and every
pre-existing row carries `trx_id == 1`, which is always visible (§4.2). Reading
an insert therefore needs no undo record.

**One is written anyway** — `kInsert`, image empty, carrying the row's `pk` —
because an undo record is a link in the writing transaction's chain.
`txn_prev_undo_ptr` (§3.3) is the only durable answer to "what did this
transaction write", and an insert that wrote no record would break that chain
and orphan everything the transaction did before it: a crash loser's insert
whose page was written back before a checkpoint would survive redo, and §4.1's
predicate would then read it as **committed**. `CHECKPOINT_BEGIN`'s
active-transaction table carries each transaction's `last_undo_ptr` as the
durable head of the chain.

Live rollback of an insert uses the transaction's **in-memory** trail (§6);
recovery's undo phase uses the record.

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
synchronously, in-process (§6) — and, across a crash, by recovery's undo
phase (`wal.md` §12), which rolls back every loser before the database is
served. That is the load-bearing assumption of the whole design.

**Readers are registered.** Two records together name every reader on a
core: live transactions in the manager's `live_`, and every other snapshot
that can read a superseded version across a park — an autocommit
statement's, a shipped pipeline stage's — through a move-only `ReaderLease`
that `txn::AutocommitSnapshot` returns beside the snapshot, so registering
is structural rather than disciplinary. `TransactionManager::ReadHorizon()`
folds both into one bound: a version superseded by a **committed**
transaction below it is invisible to every live and future view, so a purge
may retire it. Views exempt by proof: latest-state check views (they never
read a superseded version) and views that never outlive one synchronous
span on the core's single thread — an exemption to re-check whenever the
executor gains a suspension point. The horizon is **per-core**, sound while
every reader reads its own core's versions (`crosscore.md` §5); a cross-core
writer must extend it.

Two purges consume the horizon: the catalog delete-mark purge
(`ddl-transactional.md` §5d) and the **undo purge**: a settled undo page —
newest writer below the horizon — recycles into the log's own next growth,
triggered by growth, so this run's chain plateaus instead of growing without
bound. Retention is **horizon-only**: nothing a live view can reach is ever
freed, so `SnapshotTooOld` is never raised, and the price is that one
long-running transaction holds reclamation for its lifetime. A byte-cap
retention that would make the error reachable is declined. A previous run's
undo pages are not reclaimed at mount: each run starts a fresh chain and
the old pages leak.

### 4.2 The always-visible transaction id

`trx_id == 1` (`catalog::kBootstrapXid`) is visible to **every** read view,
unconditionally and permanently. This is not a migration shim that ages out:

- Every row written before the transaction manager existed carries it.
- Bootstrap catalog rows keep it (`ddl-transactional.md`); a DDL statement's
  catalog rows carry the real transaction id.
- It is the tail of every undo chain built over a pre-existing row.

`SuperBlock::CreateFresh` seeds `next_trx_id = kFirstUserTrxId = 2`, so 1 is
never reissued to a real transaction. The field was added in superblock
format version **9** and lives past the WAL anchor table
(`kNextTrxIdOffset`); ids are handed out a block at a time
(`txn::TrxIdSequence`, `kTrxIdBlockSize = 4096` `[PROPOSED]`), so a crash
burns the block's remainder - ids are unique and monotonic, never gapless,
the same promise the row-id sequence makes. Exhaustion of the 48-bit space is
reported `OutOfRange` and never wrapped, exactly as the row-id sequence does.
The superblock is unlogged, so a crash between raising the ceiling and the
page reaching the platter reissues the block; that is the exposure
`keystoneid-k0-findings.md` records for row ids, and it closes the same way,
with recovery. This mirrors PostgreSQL's `FrozenTransactionId`, which is what
`kBootstrapXid`'s own comment says.

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

The predicate is the only consumer of `Tuple::deleted`.

### 4.4 Where it is applied

Every SELECT-class read goes through a compiled step chain, so the choke
point is **`ChainRunner::AcceptTupleAt()`** (`src/exec/step_vm.cpp`) — one
call site, reached by the chain walk, the btree descent, the probe memo,
Waystone replay and the Cabin resolve alike. That makes
`waystone-concpets.md` §3.1 rule 2 — "MVCC visibility is applied exactly as
it would be on the authoritative path" — true **by construction** rather than
by discipline.

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
bootstrap row forever, costs one integer comparison and no copy at all.

`heap::ChainVisit` remains a purely physical walk. Visibility belongs to its
callback: that keeps `storage/` free of a dependency on `txn/`, and keeps
`ChainVisit` usable by the catalog, which must not filter.

## 5. Write conflicts — first-updater-wins

No lock manager, no waiting, no deadlock detection, and the Keystone lock byte
stays unused. A conflict is detected from the tuple header alone. For writer `T`
with read view `V` over the *current* header `trx_id` (`cur`):

| `cur` | Verdict |
|---|---|
| `kAlwaysVisibleTrxId` | proceed — pre-existing or bootstrap-stamped row |
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
it is part of the compatibility surface"). On the text surface the spelling
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
recovery-driven rollback reuses this code path:

| Trail entry | Compensation | Record |
|---|---|---|
| insert | `RetireSlot` + clear the Waystone entry | `SLOT_RETIRE` |
| overwrite | `OverwriteTuple(slot, image, prior_trx_id, prior_undo_ptr)` | `HEAP_OVERWRITE` |
| delete-mark | `ClearDeleteMark(slot, prior_trx_id, prior_undo_ptr)` | `HEAP_DELETE_MARK` |
| var-heap append | `varheap::PageRelease(slot)` — the value dies with the version that wrote it | `VARHEAP_RELEASE` |

Then `TXN_ABORT`, with no durability wait — a transaction whose abort record did
not survive is a transaction with no commit record, which recovery rolls back
anyway. Undo pages are not freed by rollback; §4.1's purge recycles them once
settled.

**`SLOT_RETIRE` and `VARHEAP_RELEASE` split their envelope's `txn_id` the
same way**: a rollback compensation carries the aborting transaction's id,
because analysis must see the rollback; a purge drain carries `kNoTxnId`,
because no transaction owns it.

**Failure atomicity is per transaction, not per statement.** An `UPDATE` that
fails on row 7 of 10 inside an explicit transaction leaves rows 1-6 written and
the session in `failed-txn`; the client must `ROLLBACK`, which undoes all six. In
autocommit the abort is automatic, so behaviour is statement-atomic there. This
deviates from SQL's statement atomicity, which needs savepoints or a
statement-level trail high-water mark — a non-goal that the trail's shape
supports additively.

## 7. Catalog and DDL

DDL is transactional and durable, specified in
`docs/spec/ddl-transactional.md`:

- Every DDL statement — autocommit included — runs under a real transaction.
- Catalog rows a DDL statement writes carry the real transaction id, and
  catalog reads that serve a session's schema view are filtered through the
  same visibility predicate user reads use (which reads filter, and which
  deliberately do not, is `ddl-transactional.md`'s). Live rollback therefore
  needs no undo record: the rows are registered on the transaction's trail
  and retired by `Abort`'s ordinary compensation.
- Catalog writes log the ordinary record types (`wal.md` §11a). A crash
  loser's catalog writes carry undo records, appended inside the write
  points so they precede the row records in the log, and recovery's undo
  phase rolls them back. `SHOW META` prints `ddl_durable=1
  catalog_recovered=1`.
- `CREATE TABLE` is atomic, isolated and consistent; `DROP TABLE` is atomic
  and deliberately **not** isolated (`ddl-transactional.md` §5a).

What stays unlogged is named in `wal.md` §11a.

## 8. MVCC ships before recovery — a known correctness gap

Closed. Recovery runs at mount (`wal.md` §12): analysis, redo and undo per
core, then a completion checkpoint, so a crash loser's rows are rolled back
before the database is served rather than read as committed under §4.1's
predicate. A stream that analysis found losers in, recovered with no undo
phase installed, refuses the mount rather than replaying and stopping.

## 9. Open Decisions — do not assume

The open decisions of this subsystem are unrecorded here. Explicitly **not**
open, and out of scope: `SERIALIZABLE` (§1), savepoints and statement-level
rollback (§6), lock-based blocking (§5).

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
