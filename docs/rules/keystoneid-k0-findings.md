# K0 — Keystone id audit, and what issue-once costs

Status: **findings record** — the audit behind
`docs/rules/keystoneid-invariant.md` §5 K-M1. What stands here is each
finding a current rule rests on, stated once. Evidence:
`tests/keystone_id_test.cpp`.

Three findings carry rules:

1. **A sequence whose ceiling is persisted outside the log falls behind
   the log after a crash**, and recovery's redo then hands one id to two
   tuples. The rule it produced: `sys.tables.next_id`'s bump is a logged
   catalog write (§4).
2. **Issue-once is affordable only with bump-ahead allocation, and the
   block size has a floor**: 4096 (§5).
3. **The oid half of (oid, pk) holds only if the oid counter recovers its
   position from the catalog.** It does (§6).

---

## 1. Every path that issues an id

`Catalog::AllocateRowId` (`src/catalog/catalog.cpp`) issues more than one
id space with one implementation:

| caller | what it issues |
|---|---|
| `src/server/command_dispatcher.cpp` | a user tuple's pk — the Keystone id |
| `Catalog::RegisterPattern` | the **oid** a `sys.patterns` row carries: a body field, not a Keystone word |
| `src/exec/assertion_catalog.cpp`, `src/exec/index_ddl.cpp`, the range, cabin and fkey catalog writes | the Keystone id of a catalog row (`sys.assertions`, `sys.indexes`, `sys.ranges`, `sys.cabins`, `sys.fkeys`) |

**K1 binds both spaces, for one reason**: the `sys.patterns` oid takes a
persistent sequence precisely because the general oid counter is not one
by itself (§6), and a claim that covers one space and not the other will
be read as covering neither.

The sequence is per relation, persisted in `sys.tables.next_id`, and
issued by a scan-and-overwrite of that row. Pinned by
`EachRelationHasItsOwnSequence` and
`TheAllocatorAlsoIssuesCatalogOidsAndCatalogKeystones`.

## 2. Every path that could re-issue one

- **A free list.** There is none, in the strong form: even a *physically
  retired slot* does not return its id
  (`RetiringATupleDoesNotReturnItsIdToTheAllocator`).
- **`CREATE TABLE`** sets `next_id = kFirstRowId` for a new relation: the
  id space is per relation.
- **A failed insert.** `AllocateRowId` bumps and persists *before* the
  caller encodes, so a failure between the two burns an id
  (`AnInsertThatFailsAfterAllocationBurnsTheIdRatherThanReusingIt`). K3
  makes the gap legal and the ordering is deliberate — the reverse would
  re-issue after a crash.
- **Exhaustion.** `id > kMaxKeystoneId` answers `OutOfRange` rather than
  wrapping (`AnExhaustedSequenceRefusesRatherThanWrapping`). K4's budget
  has exactly one enforcement point.
- **A crash restart.** §4.

## 3. What the allocator costs

Per issued id, core 0's `AllocateRowId` fetches `sys.tables` **for write**
(dirtying the frame), decodes rows until the oid matches, and overwrites
one — O(relations) across the chained catalog pages (§5), one dirtied
catalog page per insert, and §4's logged write. A peer issues from a
leased block and touches no catalog page
(`include/kds/catalog/row_id_lease.hpp`).

## 4. A ceiling persisted outside the log does not survive a crash

Under `strict` durability the `HEAP_INSERT` records for ids 1, 2, 3 are on
the platter before the client is answered. Were the `sys.tables` row
carrying `next_id` unlogged — reaching disk only at a checkpoint — a crash
between the two would leave the durable log naming three tuples whose ids
the reverted allocator hands out again, and the moment recovery redoes
those `HEAP_INSERT`s one id belongs to two tuples: the snapshot hazard of
`keystoneid-invariant.md` §1.1, arriving by the one door the invariant does
not watch.

`ACrashReissuesIdsThatTheDurableLogStillClaims` pins that shape against a
bare `WalManager` with no recovery, and
`ASyncedShutdownLeavesTheSequenceAboveEveryLoggedId` is its control (one
extra `SYNC`, the allocator resumes at 4) — the failure mode is reuse, not
a broken harness. `AnUnsyncedCrashLosesTheSequenceAndTheTuplesTogether`
shows why the hazard goes unnoticed without a log: the catalog page (id 4)
and the heap pages (ids ≥ 128) are lost as one unit, a consequence of
page-id ordering and not a designed guarantee.

The rule it produced: the mark's bump is a **logged catalog write**, with
an `UNDO_WRITE` ahead of it, replayed with every other catalog write
(`docs/spec/wal.md`). The same exposure is the one `docs/spec/txn.md` §4.2
records for the trx-id block ceiling in the unlogged superblock, and it
closes the same way, with recovery.

## 5. The performance question

**Per-id durability is the disaster, and bump-ahead is the escape.**
Forcing the sequence to the platter per id is one fsync per insert and
caps INSERT at the device's fsync rate; bump-ahead at N=4096 costs about a
quarter more than a non-durable allocator, and N=64 is a 3× INSERT
regression, because one fsync per 64 rows is still one fsync every 64
rows. Measured against the `bench/` tree at `1769487`
(`git show 1769487:bench/keystone_alloc_bench.cpp`). **4096 is a floor,
not a default**; `keystoneid-invariant.md` §5 K-M2 states it for reuse.
The allocator itself was under 5% of an unlogged INSERT, so no allocator
change is a performance change.

**The catalog relations chain** — found while measuring, unrelated to K1.
Each fixed catalog page id is a chain *root*; a full page links to the
next, taken from a reserved range of low page ids
(`kCatalogOverflowFirst`..`kCatalogOverflowLimit`,
`include/kds/catalog/well_known.hpp`). A page holds 68 `sys.columns` rows,
so the instance's ceiling is roughly 7,800 column rows. The range is
reserved rather than unbounded because a catalog page has to sit below the
first user page or a peer core may not fault it
(`DevicePageStore::MayFault`). §3's scan is genuinely O(relations) across
pages, and that scaling belongs to whoever owns the catalog's lookup path.

## 6. The oid half of (oid, pk)

`Catalog::GenerateUserOid()` recovers its position on first use from the
highest oid `sys.objects` and `sys.columns` carry, then increments in
memory — a boot costs one scan and a `CREATE TABLE` costs nothing extra.
`ObjectOidsAreUniqueAcrossABoot` pins it end to end, including the
consequence a re-issued oid would have: `GetSysTableRow` takes the first
row carrying an oid, so a new relation would resolve to the old one.

Recovering rather than persisting, because **there is no durable counter
that can fall behind the rows it describes.** The rows *are* the counter,
so a crash between issuing an oid and writing its row loses the oid rather
than duplicating it, and a lost oid is free under K3's no-density promise.
A superblock field would have needed a format bump and would have
introduced exactly the write-ordering question §4 answers for
`sys.tables.next_id`.

The contract at the function: the scan reads `sys.objects` and
`sys.columns` because those are the only relations a generated oid is
written to; an oid written only to some third relation would be invisible
to the recovery, which is what to check before adding a caller.

## 7. Amendments to `docs/rules/keystoneid-invariant.md`

Applied there. K3 is "No density promise", with what rests on ordering
settled dependency by dependency in that document's §1 (quote that list,
not this file); its §1.2 states (oid, pk) uniqueness with the oid half
resting on §6 here; its §2 persists the mark through the logged catalog
write §4 here required.
