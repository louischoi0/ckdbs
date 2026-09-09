# KDS Transactions & MVCC

How KDS isolates concurrent statements and what a reader sees. `[PROPOSED]` marks a default to confirm or amend before the affected part is built; `[OPEN]` must not be assumed. Companion specs: `docs/spec/wal.md`, `docs/spec/heap-and-tuple.md` (§3.2, the tuple MVCC header), `docs/spec/cross-owner-txn.md` (a transaction that touches more than one core).

---

## 1. Isolation levels

### The transaction this engine serves

**A transaction completes within 10 seconds; one that exceeds 60 seconds
may be aborted.** (Operator, 2026-09-06; AN-R14 in
`instructions/v3.0.0/workorder-aw-m1-close.md`.) This is scope, not a
mechanism: it is the shape of transaction the engine is built for, and it
is the input every retention-sized quantity is derived from — undo
retention, the visibility window, the lock-wait fault nets, and later the
WAL-replication ack timeout.

**The 10 seconds already exist under another name**:
`kShippedStatementDeadlineNs` (`statement_ship_service.hpp`), the point
past which a shipped statement's reply is presumed lost rather than slow.
AN-R14 makes that coincidence a statement — the per-statement deadline
*is* the envelope — and when statement shipping retires at AT the envelope
survives under its own name.

**The 60 seconds are the mechanism**, `kds.txn_lifetime_ceiling`
(`kTxnLifetimeCeilingNs`, default 60 s, **provisional**: set by the
operator, not measured, and re-read when AS-E measures the lifetime
distribution). Wall-clock from `BEGIN`. A transaction past it is aborted
by a sweep and the abort surfaces at its next statement **as an ordinary
abort** — §4.1 stays literally true, because no reader is ever told its
snapshot expired.

**What it costs, stated rather than discovered**: a long *busy*
transaction is aborted, where a bound on *idleness* accepted it. That was
the earlier proposal, and the operator's numbers say this engine does not
serve that shape.

**Three exemptions**, each with its reason so none is later struck as a
special case. **Only the third is built**; 1 and 2 are `[PROPOSED]` and
described here so the instance-wide sweep is not written without them:

1. **DDL** — a single statement holding relation `X`. Aborting a
   `CREATE INDEX` at 60 s gains nothing and forbids large indexes. No
   ceiling. `[PROPOSED]` — nothing exempts it today because nothing
   reaches it: DDL is never a shipped statement and so never an enrolled
   context.
2. **Background read views** (the Cabin build, the checkpoint, the
   relayout survey) — they hold undo like any view, so the ceiling would
   apply, but the response is not abort: the task **re-mints its view and
   resumes** at its next resumable point. A background task that cannot
   resume is a task defect, not a ceiling exemption. `[PROPOSED]` — no
   sweep reaches a read view, and no task re-mints on a ceiling's account;
   this is a second mechanism on a different object, not this one.
3. **Prepared contexts** — D4's exclusion, inherited unchanged: after a
   participant replies prepared it may not unilaterally abort. It leaves
   with 2PC at AT. **Built** (`ExpireEnrolled`'s `prepared` arm).

**Where it is enforced today, which is narrower than the rule**: the sweep
exists only over cross-owner *participant* contexts
(`ShippedStatementExecutor::ExpireEnrolled`), so a plain local transaction
carries no start time and nothing sweeps it — `txn/manager.hpp` has no
clock. The rule above is the engine's scope; the instance-wide half of its
enforcement is not built (AW-S3's record says what remains).

**Two consequences of the sweep being a periodic tick, not a deadline.**
It rides `wal_drain_interval_ns` (1 ms by default), so a context is swept
on the first tick after its lifetime *on which no statement or phase is in
flight on it* — a statement in flight defers the sweep, it does not exempt
the context, and `phase_running` is itself bounded by
`kTxnPhaseDeadlineNs`. And **`wal_drain_interval_ns = 0` disables the
ceiling entirely**, because that is the registration the sweep is on.

**`kds.txn_lifetime_ceiling` is a name, not yet a key.** AN-R14 asks for
the config key; it is not registered in `Expeditor`'s key set, so the
number is only the compiled `kTxnLifetimeCeilingNs` today.

### The levels

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

**`READ COMMITTED` is the default.** Rationale: under first-updater-wins (§5), `REPEATABLE READ` holds one read view for the whole transaction and
therefore converts more concurrent writes into retryable aborts; `READ COMMITTED`
re-snapshots per statement and conflicts strictly less. This differs from
MySQL/InnoDB, whose undo-chain shape this engine otherwise follows (`wal.md` §2),
and matches PostgreSQL and Oracle. Settable per server (config key `isolation`),
per session (`SET ISOLATION LEVEL`), and per transaction (`BEGIN ISOLATION
LEVEL ...`) — the same three-level precedence chain `durability` already uses.

`SERIALIZABLE` is out of scope and is **not** `[OPEN]`: it needs predicate
locking or SSI read-tracking, and this engine has neither. **It does have a
lock manager** since M2 — the lock family of §5, with units, modes, a
wait-for graph and a deadlock detector — so the reason `SERIALIZABLE` is out
is the *read* side and only that: nothing tracks which rows a reader read.
(§4.1's reader registration is not that: it records which *snapshots* exist,
never which rows they read.)

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
snapshot_lsn       every commit whose record is at or below this LSN was
                   published when the view was minted; nothing above it had
                   committed, whatever its id
own_trx_id         0 for a read-only view
visibility         the instance read view the floor and the window are read
                   through (txn/instance_visibility.hpp)
sees_everything    Everything()'s mechanism: every writer visible, nothing consulted
in_flight_at_mint  another transaction was live on the minting core (cabin.md §6a)
```

A copyable POD with no heap allocation — the reactor body allocates nothing in
steady state (`sched.md`). Nothing here bounds anything: the 64-entry
in-flight array, and the `OutOfSpace` past 64 live transactions that was its
width, went with the trx-id predicate at AN-S2.

```
Visible(t):  t == kAlwaysVisibleTrxId  -> true      (§4.2, unconditional and permanent)
             t == own_trx_id           -> true
             t <  floor                -> true      (resolved, and still on the page: a winner)
             otherwise                 -> window holds t, and commit_lsn(t) <= snapshot_lsn
```

**A commit-LSN snapshot, not a bound on transaction ids.** Until AN-S2 a view
was an exclusive high-water mark over trx ids plus the ids in flight when it
was minted. That is sound only while issue order is id order, and ids are
leased to each core in disjoint blocks of `kTrxIdBlockSize` (§4.2), so across
cores it is not: a commit on a core holding a higher block read as "not yet
started" until the reader's core burned its own block (H1), and a transaction
begun after the mint out of a lower core's unspent range read as committed
before it had started (H2). One stream gives every commit one LSN in one
order (`wal.md` §3), and the **instance read view** carries it: a window
`trx_id → commit_lsn` for every transaction committed and not yet reclaimed,
a **floor**, a slot per core, and the ceiling a mint reads. `instance_visibility.hpp`
owns the mechanism; `instructions/v3.0.0/workorder-an-read-view.md` AN-3 E
is the source read that found H1 and H2, and its AN-8 the build.

**Why no commit table older than the window is needed, and the condition on
that.** Above the floor the window answers; below it the answer is
"committed" with no lookup — *only* because an aborted transaction's page
changes are physically undone, synchronously, in-process (§6) and, across a
crash, by recovery's undo phase (`wal.md` §12), which rolls back every loser
before the database is served. That is the load-bearing assumption of the
whole design. The floor is a trx id below which every transaction is
resolved **and no id will ever be issued again**: the second bound is what
block leases require, because a core holding an unspent range below the
floor could otherwise issue into it, and it is the minimum issue cursor over
attached cores. **The floor is read live and never copied into the view**:
reclamation drops a window entry *because* the floor rose past it, so a view
holding a stale lower floor would miss the entry, decline the floor, and
answer "not committed" for a row committed long ago. Branch 4 reads the
window and the floor together under one hold, so a reclamation pass is seen
whole or not at all.

**When a commit becomes visible.** The committing core publishes its window
entry where the transaction leaves the in-flight set — after
`WalManager::Commit` returns, never under the append latch — so a reader
sees a commit as soon as the record is appended, which under `group` is
before the platter has it, exactly as before (`wal.md` §3). The ceiling a
mint takes is the highest published commit LSN **capped by every core's
pending-commit marker**, set before that core's append: no view ever covers
a commit whose entry it cannot yet see, so a **held** view's answer for any
transaction never changes for its life. An unregistered check view holds no
slot, so reclamation can move its answer for a committed writer to
"committed" a moment earlier — the exemption below, and the only direction
it moves. An unlogged instance has no LSN and the window assigns the next
position in commit order.

**Readers are registered.** Two records together name every reader on a
core: live transactions in the manager's `live_`, and every other snapshot
that can read a superseded version across a park — an autocommit
statement's, a shipped pipeline stage's — through a move-only `ReaderLease`
that `txn::AutocommitSnapshot` returns beside the snapshot, so registering
is structural rather than disciplinary. Each core publishes the oldest
`snapshot_lsn` over both into its slot, and a held mint lowers that slot
*before* it reads the ceiling, so a pass on another core can never outrun a
view in the gap between its mint and its registration. A snapshot that is
**adopted** rather than minted — a cross-owner REPEATABLE READ participant
taking its coordinator's (`cross-owner-txn.md` §3) — lowers the slot the
same way before the view moves, and is safe to hold because the
coordinator's live transaction already holds it.
`TransactionManager::ReadHorizon()` is the minimum over cores: **the
instance's oldest live snapshot**, not this core's. A version superseded by
a transaction below the floor, or committed at or below every live and
future snapshot, is invisible to every view, so a purge may retire it. Views
exempt by proof: latest-state check views (`MintCheckView`: they never read
a superseded version, and they touch no slot) and views that never outlive
one synchronous span on the core's single thread — an exemption to re-check
whenever the executor gains a suspension point. **The horizon is
instance-wide since AN-S2**: a reader on core 3 holding a snapshot from
before core 0's commit keeps that commit's entry, and the undo it
superseded, wherever the undo lives.

Two purges consume it. The catalog delete-mark purge (`ddl-transactional.md`
§5d) judges each mark's deleter by the two branches
(`TransactionManager::ResolvedForEveryReader`). The **undo purge** settles a
page by its newest writer's id against the **floor** alone — a page knows
its writers' ids and not their commit LSNs — and the floor reaches it with
the second branch already applied, since reclamation never raises the floor
past a commit some live or future snapshot cannot see; a settled page
recycles into the log's own next growth, triggered by growth, so this run's
chain plateaus instead of growing without bound. Retention is
**horizon-only**: nothing a live view can reach is ever freed, so
`SnapshotTooOld` is never raised, and the price is that one long-running
transaction holds reclamation for its lifetime — the instance's. A byte-cap
retention that would make the error reachable is declined.

**What bounds that price is the transaction, not the snapshot** (AN-R10,
AN-R14; §1). The lifetime ceiling ends a transaction that has held the
horizon too long, and the reader learns of it at its next statement as an
**ordinary abort** — this paragraph stays literally true, since no reader
is ever told its snapshot expired. That is the whole of the difference
between the ruling taken and the `SnapshotTooOld` one declined: the same
reclamation, reached by ending the holder rather than by failing the read.

**Whose undo the price is paid in is the instance's** (AN-S2). Each core
still owns its own `UndoLog` (`CoreRuntime`/`Expeditor` each hold one), but
the horizon folds every core's readers, so an idle `BEGIN` on core 3 holds
*every* core's undo above its snapshot, and the commit window with it. That
is the exposure AN-R10 was marked to bound and the reason the bound is a
wall-clock lifetime rather than a byte cap. A previous run's undo pages are
not reclaimed at mount: each run starts a fresh chain and the old pages
leak.

### 4.2 The always-visible transaction id

`trx_id == 1` (`catalog::kBootstrapXid`) is visible to **every** read view,
unconditionally and permanently — `Visible`'s first branch, ahead of the
floor and the window, so a catalog row never reaches either. This is not a
migration shim that ages out:

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
writer costs no copy at all: a bootstrap row or the view's own writer one
comparison, a writer below the floor one atomic load, and a writer above it
one latched window lookup, which is the cost AN-S5 measures. `Classify()`
is no longer `constexpr` for that last reason; it still fetches no page,
which is the property the span rule needs.

`heap::ChainVisit` remains a purely physical walk. Visibility belongs to its
callback: that keeps `storage/` free of a dependency on `txn/`, and keeps
`ChainVisit` usable by the catalog, which must not filter.

## 5. Write conflicts — first-updater-wins

A conflict is detected from the tuple header alone, and the Keystone lock byte
stays unused — **the lock family holds no bit on the page** (AO-R3: no
persisted lock bit, no page bit, no superblock field; a crash releases every
lock because the loser is rolled back at mount). **The header is no longer the only source of one**
(AO-S6c-b): a transaction may hold a *unit* rather than a row - a range
declared by a predicate-covering write (AO-0 item 14) - and the header of a
row inside that range names whoever wrote it last, which says nothing about
the holder. A borrow the lock table refuses is therefore a conflict of its
own, reported in the same shape and naming the **holder** rather than the
writer, because that is who the waiter is waiting for. Where a dispatcher
has no lock table the header is all there is and this section reads as it
always did. **What follows the detection is no longer a refusal alone**
(M2, `instructions/v3.0.0/workorder-ao-m2-lock-family.md`, until AO-S8
moves it here): a writer meeting an undecided holder waits for its decide
(AO-S3), a transaction holding rows may wait because a wait-for graph in
the instance's lock table refuses the waiter whose registration would close
a cycle, naming deadlock (AO-S4a on one core, AO-S4b across cores, where the
shipped-statement park records the edge only its owner can), and a wait
that reaches the fault net is logged as the defect it is (AO-R8). For writer `T`
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
stricter than PostgreSQL's `READ COMMITTED` and was a deliberate simplification —
there is no re-read loop.

**"And no lock to wait on" is no longer true, and for one shape the verdict
above is now reached less often** (the operator's mark of 2026-09-08, built
in AO-S6c-c). A write whose predicate covers a key window declares that
window and borrows it **before it walks** (AO-0 item 14). If the unit is
held, the statement waits having written nothing — and an autocommit
statement's re-run mints a fresh read view, so it proceeds where the same
statement used to write part of its rows, meet a held one, and be refused
with the written rows compensated. For that shape autocommit is now closer
to PostgreSQL's `READ COMMITTED` than to the rule this section states,
which is what AR2-A §1 measures M2 by: a refusal converted into a wait.

**The widest case is a write with no `WHERE` at all**, and it is worth
naming because it is the commonest bulk shape: it declares **the whole id
space** and borrows it in `X`. So `UPDATE t SET v = 1` waits for any
transaction holding a key of `t`, and once granted blocks every other
writer of `t` for the length of its walk. That is a table-level
serialization point where the per-row path had none, and such a statement
can join a deadlock cycle where before it could not. It is the price of the
guarantee the declaration buys — the statement's whole walk sees one state —
and it is paid only by writes that name no key window.

**The unit it declares is a range and not the relation** (AO-S6e-b), and
the difference matters in exactly one direction. The relation *unit* means
the relation as an **object** — its existence and its schema — which is
what DDL claims and what a positioned reader declares its `IS` on; a
write's claim is over **keys**. Collapsing a `WHERE`-less write onto the
relation entry was cheaper (one entry, no fence counter raised) and put it
against a reader's `IS` there by the intention rule, for rows the reader's
snapshot already answers. Against *writers* nothing changes: a key of `t`
is inside `[0, kIdSpaceEnd)`, so the overlap scan finds it where the
relation entry used to, and a concurrent DDL still meets the `IX` this
takes above its range. Two consequences are worth stating rather than
discovering: a transaction holding a bare `IX` and no key at all no longer
conflicts with it, which is a transaction holding nothing this write
touches; and the relation's fence counter now rises for the length of the
write, so a writer of the same relation pays the all-partition probe
AO-R3's counter gates — the same cost a range-shaped bulk write has always
had, and paid by the declaring transaction's *own* later single-row writes
as well, which reach the probe before its `h.txn == txn` test spares them.

Three things bound it, and none of them is a re-read loop. An **explicit
transaction** keeps its read view across the re-run, so `REPEATABLE READ`
reaches the verdict above exactly as before. A predicate that names **no pk
window** — `WHERE name = 'x'` — declares nothing, takes the per-row path,
and can still park mid-walk with rows already written, where the table
above is what decides. And the wait is the ordinary one: it ends at the
holder's decide, it is an edge in the wait-for graph, and it is refused
rather than entered where it would close a cycle.

**What `REPEATABLE READ` waits for** (AO-S6d, AO-0 item 17, the operator's
ruling of 2026-09-09). The question is asked per wait rather than per
level: **can the re-run answer differently once this holder decides?**

- **No, where the blocker wrote the row's current version.** A commit makes
  that version invisible to a view minted at `BEGIN` for the rest of the
  transaction, so the re-run is refused on the ground it was refused on the
  first time and the wait could only ever pay off on the abort arm. The
  level stays excluded, which is what it always was.
- **Yes, where the blocker holds a coarser unit over the key and has
  written no version of it** - the fence of the paragraphs above. Its
  commit changes what this view admits only for rows it wrote, and the row
  in hand was written by somebody the view has already judged.
- **Yes, for an `INSERT`,** whatever the blocker is. An insert's verdict is
  not a function of the waiter's read view at all: a caller-named key's
  uniqueness is proved by a physical descent onto the one page that may
  hold it, and an issued key comes from the relation's own sequence. So the
  wait ends in a row written, or in `AlreadyExists` for a key the holder
  took - and `AlreadyExists` is not retryable, which is the honest answer
  either way.
- **Yes, for the foreign-key forward check.** Its `check_view` is minted
  at the check rather than at `BEGIN` — a constraint reads latest state
  (`foreign-keys.md` §4, which is where that rule lives; §4.4 below is
  where a view is *applied*, not where this one is minted) — so the level
  does not enter: the holder's commit makes the
  parent visible to the re-run, and its abort makes the answer a terminal
  `FkViolation` instead of a retryable conflict. The cross-owner half of
  the same check parks without asking the level at all, so this is also
  what keeps one statement's answer independent of which core its parent
  lives on.
- **No, where the site cannot tell.** A statement that declared a coarse
  unit and had it refused knows *who* refused it and not *what* they hold,
  so a holder that already wrote a row the walk will reach is
  indistinguishable from a fence that wrote nothing. The exclusion stands
  there rather than guessing.

A holder is free to write the row while the waiter is parked. The re-run
then meets a header naming a transaction that committed after the waiter's
view was minted and is refused first-updater-wins - the correct
`REPEATABLE READ` answer, reached after a wait rather than instead of one.
That is PostgreSQL's shape, and it is the intended one: what the wait buys
is the case where the holder never touches the row, which is every fence
declared over a window wider than what it wrote.

**What a read borrows, and what that borrow is for** (AO-S6e-b; AR2 §3's
`SELECT` row, AO-R12; **at the bind since AT-S1**, AT-R1). A statement
declares **every relation it binds** — `IS` on the relation, asked by the
compiler between the name and the schema, for the `FROM`, every `JOIN`,
every subquery block, and a write's own relation — and its outermost walk
declares **where it is**, `IS` on the slice it has reached, moved at every
page boundary. The scope is **the statement**, not the transaction, so two
`SELECT`s in one transaction declare twice and hold nothing between them.
A stage executing on another core declares for itself in its own frame,
because the session core's borrow ends when its statement returns
`pending` (`remote_step_service.cpp`).

- **It is a position, never a permission.** Visibility is the snapshot's
  and nothing here changes it. A borrow the table refuses leaves the reader
  holding nothing and reading on: a read is never refused and never waits
  for a borrow, because it needs none to be correct - a dropped relation's
  pages stay allocated and its oid is never reissued (`drop-table.md` DT1),
  every catalog row is an MVCC version the reader's view filters, and no
  DDL moves data (`alter.md`), so a plan compiled while a DDL's `X` stood
  reads a snapshot-consistent past. **What the bind-time ask therefore
  gives is one-directional**: a reader that was *granted* the `IS` is not
  overtaken - no DDL takes the `X` until the statement ends - and a reader
  *refused* it is right for the three reasons above, not for the lock's
  (`read_borrow.hpp`; `workorder-at-m3-uniformity.md` AT-7 item 10). It is
  also what keeps a reader out of the wait-for graph: a reader never waits,
  so it is always a sink, and a chain that reaches one ends there.
- **Its one consumer in M2 is DDL's relation `X`** — `DROP TABLE`
  (`drop-table.md` DT7). AO-R12 puts the read borrow there for a *mover*,
  and no mover exists (`physical-optimizer.md` is shadow-only), so what a
  position is declared to today is the drop that must not overtake it.
  **That `X` is refused by a writer's `IX` too**, so the drop waits for an
  open writer of the relation as well - transaction-length, since a write's
  borrow is the transaction's. A drop **inside a transaction that holds
  rows** is therefore a waiter that holds something, which is the one waiter
  here that can close a cycle: it registers a `waiter -> holder` edge and is
  refused naming deadlock where the registration would close one. An
  autocommit drop registers none - its transaction is unwound before the
  park, so it holds nothing - and a reader never waits, so a chain that
  reaches one ends there. The wait itself is on the table's **slot** rather
  than on the holder's decide, because `IsInFlight` is one core's live set
  and a read borrow may be held on another; the slot is flipped by whichever
  core releases and carried across by AU-S2's kick.
- **A bulk write's declared unit is not a consumer**, and that is a rule
  rather than an omission: an intention mode on an interval unit neither
  fences nor is fenced. A `DELETE FROM t WHERE id < 50` changes no key's
  assignment - it writes row versions, which MVCC already answers a
  concurrent reader from its snapshot - so it passes a reader's slice and a
  reader passes its range. What still stops a write inside a fence is every
  unit that is not an intention: a tuple `X`, another fence.
- **The interval is `[min_key(page), kIdSpaceEnd)`** and not AR2-R14's
  `[min_key, next.min_key)`: the upper bound is a page the walk has not
  read, and a forward walk's future is the whole tail of the key space
  anyway, so the narrower interval would under-declare exactly the keys a
  resumed walk is about to visit. Btree only - a heap chain is not walked
  in key order, so it declares the relation and no slice.
- **The holder is not a transaction.** An autocommit `SELECT` has no
  transaction, so a read borrow holds under an id from a space of its own
  (bit 63, which no 48-bit trx id reaches; bit 62 tells the dispatcher's
  holders from the remote-step server's on the same core, `read_borrow.hpp`).
  A refusal naming one says "a positioned reader".
- **What declares the relation and no slice**: a nested step's walk (the
  per-page cost would be the page count times the outer cardinality), a
  point, index or Cabin read on the path that serves it, and a write. Each
  is bound, so each holds the relation `IS`; the slice is the outermost
  walk's alone, and each of the three reads falls through to that walk when
  its own path cannot answer. **What takes none: the foreign-key check
  family** - an `INSERT`'s or `UPDATE`'s parent, a `DELETE`'s children, and
  the relation an `FkProbeServer` resolves on the answering core - because
  those checks are helpers beside the executor and not steps
  (`fk_check.hpp`: "`exec::Compile()` is SELECT-only"), so none of them
  passes the compiler seam AT-S1 threaded. The following letter's, with
  D9(a), whose `S` fence is the borrow that path needs.

**The table itself, and what serializes it** — `rules.md` §3's row, moved
here at AO-S8 because §3's own rule is that a declared-shared structure is
declared in the spec that owns the subsystem, and this is that spec:

> The **lock table** is the lock family's partitioned table, **one for the
> instance** — built by the `Expeditor` for every core count, borrowed by
> every peer's transaction manager, and taken by every dispatcher. It is
> `64 × cores` partitions keyed by `(rel_oid, unit kind, lo)`, so a
> relation's entry and its tuples hash apart. Each partition carries a
> `base/latch.hpp` latch, **null at `cores = 1`**. One thing is read
> *without* that latch and it is not a field of the entry: a **striped
> `S`/`X`-fence counter per relation**, an atomic array beside the
> partitions, which is what lets a writer ask "is any fence over this
> relation" without scanning. An entry's holders and waiters are vectors
> and are read under the latch.
>
> **The order.** A partition latch is taken with no page latch held, with
> no other partition latch held (one partition per operation), never under
> the WAL latch or the window latch, and **released before any park** — a
> statement parks on a decide, and a partition held across that park would
> be held against the very core that would end it.
>
> **Stated, not enforced**, and the distinction is `rules.md` §3's own
> ("a stated order that nothing checks is a comment"): `lock_table.hpp`
> says only the second of the four is structurally true, and the thing
> that would check the fourth — a suspend audit that trips on a park with
> a span held — was AO-S2's and was not delivered. It holds in the tree
> today because every guard in `src/txn/lock_table.cpp` is function-scoped
> and the file contains no `co_await`.
>
> Nothing of it is persisted (AO-R3): no WAL record type, no page bit, no
> superblock field. A crash releases every lock because the loser is rolled
> back at mount.

The engine reports `StatusCode::kTxnConflict`, which maps to the
wire contract `wire::ErrorCategory::kTxnConflict` with **`retryable = 1`**
(`protocol.md` §11: "financial client libraries build retry loops on this bit, so
it is part of the compatibility surface"). On the text surface the spelling
is machine-parsable and keeps the `ERR ` prefix that drives the dispatcher's
Warn-vs-Debug logging:

```
ERR TXN_CONFLICT retryable=1 row id=42 was written by transaction 118
ERR TXN_CONFLICT retryable=1 row id=42 is held by transaction 118
ERR TXN_CONFLICT retryable=1 rows id=[42, 51) are held by transaction 118
```

The second and third are the lock's (AO-S6c-b, AO-S6c-c) and name the
holder; the third is a declared unit, and it says a window rather than a row
because the key that conflicted may be any of them.

**One refusal in this family is not retryable and is not a conflict**: a
transaction that reaches `max_locks_per_txn` is refused `ResourceExhausted`
carrying `wire::ResourceDetail::kLockCap` (AO-R10, AO-S6c-c). A retry meets
the same cap, so the bit is 0 and the client's fix is a shorter transaction
rather than a later one.

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

**A commit that fails aborts, on both paths** (AO-S6d, AO-0 item 15). The
commit of an autocommit statement and the `COMMIT` of an explicit
transaction can both fail — the assertion entries could not be cleared, or
the commit record could not be written — and `TransactionManager::Commit`
fails only *before* it publishes, so the transaction is still active and
its trail intact. `Abort` is the operation defined for that state and both
paths now take it: the compensations run, the borrows and the transaction
are released, and the client's answer is the commit's own failure rather
than the unwind's. Reporting the failure without unwinding leaves a
transaction that is active for the life of the process — counted in flight
by every later snapshot, and, since every writer takes a borrow, holding
tenancies that make every later writer of its rows wait out the lock-wait
fault net.

A commit record that reached the platter under a failed sync is followed in
the log by the compensations and by `TXN_ABORT`, and analysis reads that as
aborted rather than as a winner (`wal.md` §12) — `kAborted` overwrites
`kWinner`, which neither `kLoser` nor `kPrepared` may do. **What that does not
cover is a crash inside the unwind**: `TXN_ABORT` is appended without a
durability wait, so a stream whose `TXN_COMMIT` bytes reached the device
while the compensations and the abort record did not replays the
transaction as a winner and redoes writes its client was told had failed.
The truthful code for a strict commit whose sync failed is
`UnknownOutcome` rather than the device's own error, and it is not what the
engine returns; the gap is the durability layer's and is stated here rather
than left to be inferred from "aborts".

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
   writes visible; a commit above the snapshot LSN invisible; a live writer
   invisible whatever its id; below the floor visible whatever the window
   says; `undo_ptr == 0` with an invisible writer ⇒ no version; delete-mark by a
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
