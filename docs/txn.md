# KDS Transactions & MVCC — Technical Specification

**Status:** Official specification. Confirmed 2026-07-31. Marker legend follows
`docs/wal.md`: `[CONFIRMED]` — settled; `[PROPOSED]` — this document's default,
adopt or amend before implementing the affected part; `[OPEN]` — must not be
assumed.

This document is the "Transaction/MVCC spec" that `docs/wal.md` §14 item 4 names
as an unwritten prerequisite. It closes two of that item's four clauses (undo-page
layout; and, by ratifying isolation levels, the visibility model) and closes the
`[OPEN]` item **MVCC version identity semantics** from `CLAUDE.md` and
`docs/heap-and-tuple.md` §9. It deliberately does **not** close undo retention,
`SnapshotTooOld`, or `trx_id` wraparound — see §9.

Consistent with `docs/wal.md` (§§1, 2, 5.1, 8, 12), `docs/heap-and-tuple.md`
(§§3.2, 4, 7, invariants 9-12), `docs/rules.md` (§§1-4), `docs/sched.md` §4,
`docs/protocol.md` §§9, 11, and `docs/waystone-concpets.md` §3.1.

---

## 1. Isolation levels `[CONFIRMED 2026-07-31]`

KDS supports exactly two isolation levels. **No level was named anywhere in the
documentation before this section**, so this is a new decision rather than a
restatement.

| Level | Read view | Meaning |
|---|---|---|
| `READ COMMITTED` | taken afresh at the start of **every statement** | a statement sees everything committed before it began |
| `REPEATABLE READ` | taken once at `BEGIN`, held for the transaction | every statement in the transaction sees the same database state |

**`READ COMMITTED` is the default.** Rationale: under first-updater-wins with no
waiting (§5), `REPEATABLE READ` holds one read view for the whole transaction and
therefore converts more concurrent writes into retryable aborts; `READ COMMITTED`
re-snapshots per statement and conflicts strictly less. This differs from
MySQL/InnoDB, whose undo-chain shape this engine otherwise follows (`wal.md` §2),
and matches PostgreSQL and Oracle. Settable per server (config key `isolation`),
per session (`SET ISOLATION LEVEL`), and per transaction (`BEGIN ISOLATION
LEVEL ...`) — the same three-level precedence chain `durability` already uses.

`SERIALIZABLE` is out of scope and is **not** `[OPEN]`: it needs predicate
locking or SSI read-tracking, neither of which fits a design with no lock manager
and no reader registration (§4).

## 2. MVCC version identity `[CONFIRMED 2026-07-31]`

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

## 3. Undo storage `[CONFIRMED 2026-07-31]`

Closes `docs/wal.md` §15's "Undo-page layout details (`UNDO_WRITE` targets)".

### 3.1 Undo pages are headered

Not a free choice. `wal.md` §9 `[CONFIRMED]` already lists **undo** among the
pages carrying the common 32-byte page header, and `docs/page.md` §1 names
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
| 8 | 8 | `owner_trx_id` | the transaction that owns this page; 0 = unowned |
| 16 | 4 | `prev_page_id` | previous undo page of the **same** transaction |
| 20 | 4 | `reserved1` | 0 |

`kUndoPageHeaderSize = 24`; `kUndoRecordsOffset = 56`;
`kUndoPageCapacity = 8192 - 56 = 8136`.

`lower` is absolute for the same reason `HeapPageHeaderFields::lower` is: it is
compared against `kPageSize` and used directly as a `memcpy` destination, and a
body-relative value would invite one missing `+ kPageBodyOffset`.
`prev_page_id` lets a transaction's undo pages form a chain a future purge pass
can free without a side table.

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

### 3.5 WAL mapping — `UndoWritePayload` unchanged

The existing payload (`include/kds/wal/payload.hpp`) fits without amendment:

```
envelope : {type = kUndoWrite, txn_id = the writing transaction,
            page_id = the UNDO page, not the heap page}
payload.prior_trx_id   = record +0
payload.prior_undo_ptr = record +8
payload.offset         = the record's offset within its undo page
payload.image          = record bytes [+16, +28 + image_len)
```

The two chain-link fields are carried as payload *fields* and not repeated inside
`image`, which is exactly why the payload's existing comment — "the one exception
is `UNDO_WRITE`'s *prior* writer, which is a different transaction from the one
that wrote the record" — is already correct.

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

### 3.7 Chain walks

`UndoLog::Walk` follows `prior_undo_ptr` newest→oldest, bounded by
`kMaxUndoChainLength = 2^16`. Exceeding the bound is `Corruption`, not a hang —
the same guard `kMaxChainPages` provides for the heap chain.

## 4. Snapshots and visibility `[CONFIRMED 2026-07-31]`

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

Readers are **not registered anywhere**. That is what makes undo retention a
non-goal today — nothing can be purged, so nothing can be purged too early, so
`SnapshotTooOld` is structurally unreachable — and it is exactly what has to
change when purge lands.

### 4.2 The always-visible transaction id

`trx_id == 1` (`catalog::kBootstrapXid`) is visible to **every** read view,
unconditionally and permanently. This is not a migration shim that ages out:

- Every row written before 2026-07-31 carries it (`HandleInsert` passed
  `kBootstrapXid` for all of them).
- Every catalog row carries it **forever**, because catalog writes use
  `kBootstrapXid` and catalog in-place updates carry the old header forward (§7).
- It is the tail of every undo chain built over a pre-existing row.

`SuperBlock::CreateFresh` seeds `next_trx_id = kFirstUserTrxId = 2`, so 1 is
never reissued to a real transaction. This mirrors PostgreSQL's
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
set since 2026-07-29 and never read.

### 4.4 Where it is applied

`HandleSelect` and `HandleUpdate` each own a single lambda (`emit`, `apply`)
shared by the Waystone probe path *and* the `ChainVisit` scan. Calling the
predicate there makes `waystone-concpets.md` §3.1 rule 2 — "MVCC visibility is
applied exactly as it would be on the authoritative path" — true **by
construction** rather than by discipline. Before this document it was vacuously
true, because nothing applied visibility anywhere.

`heap::ChainVisit` remains a purely physical walk. Visibility belongs to its
callback: that keeps `storage/` free of a dependency on `txn/`, and keeps
`ChainVisit` usable by the catalog, which must not filter.

## 5. Write conflicts — first-updater-wins `[CONFIRMED 2026-07-31]`

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

The engine reports `StatusCode::kTxnConflict`, which maps to the `[CONFIRMED]`
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

## 6. Rollback `[CONFIRMED 2026-07-31]`

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
rollback from recovery's analysis phase. As of 2026-07-31: **a `SLOT_RETIRE`
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

## 7. Catalog and DDL `[CONFIRMED 2026-07-31]`

Nothing in `src/catalog/` participates in transactions:

- Every catalog write is stamped `kBootstrapXid` and every catalog in-place
  update carries the old header forward, so catalog rows keep `trx_id == 1` and
  `undo_ptr == 0` permanently and are visible to every read view (§4.2).
- Catalog reads do not go through the visibility predicate — they scan pages
  directly.
- Therefore **DDL is neither logged nor transactional, and `CREATE TABLE` inside
  an explicit transaction is not rolled back by `ROLLBACK`.** A known limitation,
  consistent with the catalog's existing instance-scoped coherency caveat.

## 8. Shipping MVCC before recovery — a known correctness gap `[2026-07-31]`

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

Carried over from `docs/wal.md` §15 and `CLAUDE.md`, still open, with the seam
that keeps each one viable:

- **Undo retention policy** and `SnapshotTooOld` surfacing (error class,
  retryability). Nothing frees an undo page today, so a write-heavy relation
  grows undo monotonically — the same trade `heap_chain.hpp` already documents
  for deleted heap space. Purge needs the reader registration §4.1 deliberately
  omits, and would be a `maintenance`-group task.
- **48-bit `trx_id` wraparound / epoch handling.** Exhaustion is reported
  `OutOfRange` and never wrapped, exactly as the row-id sequence does.
- **Cross-core transaction commit protocol** — `wal.md` §3 says "do not design it
  now". One `WalManager` owns one stream; a transaction spanning cores is not
  representable. Everything here is core-local.
- **Buffer-pool page-frame reclamation** under the page-latch model.
- **Page compaction / free-space reuse.** `heap-and-tuple.md` §3.1b says
  compaction "needs a transaction manager to know no snapshot still needs the
  bytes". This manager *could* answer that once readers are registered, but the
  split policy it interacts with is open.

Explicitly **not** open, and out of scope: `SERIALIZABLE` (§1), savepoints and
statement-level rollback (§6), transactional DDL (§7), lock-based blocking (§5),
recovery (§8).

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
