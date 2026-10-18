# Cross-Core Execution

What crosses a core when a statement runs, and what does not. **Almost
nothing**: no relation or page belongs to a core; every statement runs,
whole, on the core its session is on; every core reads and writes every
page under the page latch; no transaction has a part on another core, and
there is no message between cores - one core reaches another only by
writing shared state and kicking it (`docs/spec/sched.md` §5). What
crosses is a **connection**, where the port cannot be shared or where a
session is moved at a transaction's head (CC1), and a **kick**.

Consistent with `docs/rules/rules.md` (thread-per-core, core-local by
default with what is shared declared, deterministic testability).

## 1. Decisions

| # | Decision | Resolution |
|---|----------|-----------|
| CC1 | Execution model | **Every statement runs on the core its session is on, and no part of one runs anywhere else.** A session's core is first the one that accepted its connection (every core listens with `SO_REUSEPORT`), or, where the port cannot be shared, the core core 0 handed it to (D19's fallback; `include/kds/server/connection_handoff.hpp`, `sched.md` §5). **Since BL-S3 it is re-decided at every transaction's head** above one core (`include/kds/server/load_placer.hpp`, BL-R1, BL-R4): a session with no transaction open that asks for a statement may be moved, its whole connection with it, to a less-busy core through the same inboxes, and the transaction that statement begins runs, waits and commits where it began - a transaction never crosses. There is no message between cores (§3) |
| CC2 | Intermediate transfer | **None**: no row crosses a core. The rule that stands is **one row encoding**, `wire/row_codec.hpp`, whose one consumer is the KWP wire - result batches and the load stream's chunks (`protocol.md` §6, `bulkinsert.md` BI6) |
| CC3 | Write scope | A transaction's writes run on the core its session is on, and a transaction is one core's, whole. **Nothing is refused for touching two relations**, nor for where a write lands |
| CC4 | Read isolation | Every read takes the snapshot its own statement or transaction holds on the session's core (`txn.md` §4.1; §5) |
| CC5 | Cancellation & errors | **Nothing cross-core to cancel**: a statement's errors are its own core's, framed by its session (§7) |
| CC6 | Scheduling | **Nothing crosses to be scheduled**: a kick carries no group, and the woken core's own parked task runs in its own group |
| CC7 | Page placement | **No page belongs to a core** (AR0-5 D17). `sys.tables.owner_core`'s four bytes are a reserved word - written 0, never read - and nothing names a core on disk, so the core count is not pinned: a mount at another `cores` records it. Every core reads and writes every page under the page latch (CC11, `page.md` §6) |
| CC8 | Relation structure | **A relation is one structure headed by `sys.tables.desc_page_id`** - one clustered btree for a user relation, one catalog chain for a system relation - and nothing divides it by tuple key (`instructions/v3.0.0/raft-marks-2026-09-30.md` §9). A volume at any superblock version but the current one (31, `server/superblock.hpp`) is refused at mount |
| CC9 | Reserved catalog ids | Root page 10 and oid 133 are unused and not reissued (page 15, `sys.ranges`' root once, is `sys.cabin_exprs`' since AQ). **Ids 17..127 are unused**, left for a future bootstrap root: no catalog chain grows into them since superblock 31, and `kFirstUserPageId` stays 128 (D8 of the catalog page ceiling, under the operator's ruling of 2026-10-18, "$!recommended") |
| CC10 | Refused keys | `range_size_ids`, `placement` and `peer_listeners` are refused by name at boot, as is `physical_optimizer` (retired since BZ, `physical-optimizer.md` §8) (`src/server/expeditor.cpp`) |
| CC11 | Shared-structure access | **Every core reads and writes with the same authority** (AR0-5 §0). **Which transactions are running** is every core's to read since AX-S1: each core publishes its own into an in-flight table no other core writes, so `IsInFlight` is the instance's answer (`txn.md` §4.1, `rules.md` §3). What serialises the superblock, the free map and the catalog pages is named where each is written: the **page latch** across cores for the bytes of any page (`page.md` §6), and within one core the rule that no task parks under a page span, which is what makes a catalog row's read-modify-write atomic (`catalog.md` CT5, `AdmitExplicitRowId`'s hook); the free map's own `map_latch_` for allocation (AM-S3, `page.md` §5); the **schema version word** for every core's memo of the catalog (`catalog.md` CT2); and page 0's persisted transaction-id ceiling, which every core's `TrxIdSequence::Carve` reads and raises in one step under the superblock latch (`txn.md` §4.2). A relation's row-id mark is a catalog row like any other, bumped in place under its page latch by whichever core issues (`heap-and-tuple.md` §4.1a). DDL runs where the session is, under the DDL's own relation `X` (`txn.md` §5) where it has one; a name-creating DDL - `CREATE TABLE`, `RENAME TO`, `CREATE NAMESPACE`, `RENAME COLUMN`, an index's or an assertion's name - checks and writes the name under one hold of its catalog relation's root page since AT-S17 (`catalog.md` CT7). A page faulted on one core is served from the frame another loaded (`eviction.md`). |
| CC12 | Catalog page placement | **CR1 — a catalog relation's root page is fixed; nothing else of it is.** `kCatalogPageTypes = 4` through `kCatalogPageTransitions = 16` (`include/kds/catalog/well_known.hpp`'s `kCatalogRoots`; page 10 unused) sit below `kFirstUserPageId = 128` because they must be findable at bootstrap *without* a catalog read. A catalog relation's **var-heap** root is allocated through `CreateNew()` and recorded in `sys.tables`, binding on any catalog relation with a var-heap. Every core faults every page, so a page's id does not decide who can read it; `exec::CatalogSpillPages` serves the mount's var-heap sweep. DDL runs where the session is (CC11), and a core that has to **grow** a catalog chain places the page itself (`catalog.md` CT5). **CR3 — a catalog chain grows from the general supply since superblock 31** (the catalog page ceiling, the operator's ruling of 2026-10-18, "$!recommended"): every page a catalog chain grows onto past its root is a `CreateNew()` page on the ordinary relation rules (`catalog.md` CT5). **`sys.access_stats` takes them too**, unlogged under CR6 (CC13), and its mount discard cuts the chain at a page not its own - a foreign owner, a non-heap page, an unallocated or unreadable id - or at a spent walk budget (`Catalog::DiscardAccessStats`). Reclaim passes them by (`page.md` §5). A var-heap root is found through `sys.tables` — the indirection `varheap_page_id` uses, DDL-immutable and therefore cacheable per `rows.hpp` |
| CC13 | Where a core's statistics are written | **CR6 — a catalog relation whose content is only a statistic is written unlogged**, the sole exception to the rule that catalog writes are WAL-logged and replayed (`docs/spec/ddl-transactional.md` §7; the rule text is `docs/rules/rules.md` §5). **A core writes its own access statistics where the statement ran**, and a count is in the row before its statement returns. **`sys.access_stats` is one relation at page 11 (`kCatalogPageAccessStats`)** and there are no per-core statistics relations - `SysAccessStatRow` is a 33-byte fixed row with no `core_id` field (`catalog/rows.hpp`), so every core's counts **fold into the one `(kind, rel_id, column_mask)` shape**. `Catalog::RecordAccess` holds that relation's root page exclusive for its whole body, which is what makes the fold and the admission of a shape nobody has seen one act across cores. `RecordAccess` costs +1-2% on a point lookup (`docs/spec/heap-and-tuple.md` §7) |

## 2. Execution Model

The session core runs the statement end to end: it parses, compiles the
step chain (`docs/spec/parser-v2.md` I12 - *the chain is the plan*: the
written order, or the join planner's for a planned block, the same on every
core for one catalog) from its catalog cache,
and executes every step of it itself, reading every page through the
instance's one frame table (CC7, CC11). There is **one path**.

Trail and statistics recording is the executing core's: a core writes
`sys.access_stats` and `sys.patterns` itself, under those relations' root
page latches (CC13).

## 3. Messages

**There are no messages between cores.** A core that needs another to act
writes shared state and kicks it through the instance's one wake registry
(`docs/spec/sched.md` §5 and its invariant 7). Transaction ids and a
relation's row ids are issued by every core itself: each carves its own
transaction-id window and bumps a relation's row-id mark in place
(`txn.md` §4.2, `heap-and-tuple.md` §4.1a). A result row reaches a client
from the core the statement ran on, through the session's `ResultSink`
(`protocol.md` §12).

## 5. Isolation Semantics

**A statement reads under one view, on one core.** Every step of it runs
where the session is (CC1), so the snapshot it reads under - its own, or
its transaction's (`docs/spec/txn.md` §4.1) - is the only one it has, and
no part of its answer is read at another instant.

- **READ COMMITTED** statements: each statement observes the latest
  committed state as of its own view.
- **REPEATABLE READ** transactions: one view, on one core. The
  transaction pins its snapshot at `BEGIN` and every statement of it runs
  where the session is.
- Catalog: the plan is resolved and executed on the session core from
  its catalog cache, under the relation `IS` every bound relation holds
  for the statement (`docs/spec/txn.md` §5).
- **Whether a writer is decided is the instance's answer** (AX-S1, AX-S2):
  a writer's conflict check and an unfiltered catalog read ask every
  core's in-flight table (`docs/spec/txn.md` §4.1), so another core's open
  write or open drop reads as in flight here as it
  does on its own core - a delete-mark it wrote is not settled until it
  decides (`docs/spec/ddl-transactional.md` §5b), and a row it holds is
  waited for rather than read as released.

## 6. Writes

**No statement crosses, and no write is refused for where it lands.** A
read and a write both run on the core their session is on, through the one
frame table, and a transaction is one core's, whole (CC3). Two writers on
two cores meet at the page latch for a page's bytes and at the lock family
for a row or relation (`txn.md` §5) - the same two places two writers on
one core meet. An auxiliary - a secondary index, a Bound Cabin, a spilling
var-heap, a foreign key, an assertion - is written by whichever core runs
the statement, under the latch or registry its own spec names (`index.md`,
`cabin.md`, `assertion.md` §6.1, `foreign-keys.md`).

**Foreign keys** (`docs/spec/foreign-keys.md`): a parent and a child on
two relations is **not a crossing at all**. The forward check hoists to
the dispatch fork - for deduplication, and because the wait has to happen
where nothing has been written yet - and descends every parent here; the
reverse check walks every child here. Nothing is probed and nothing is
left behind, and the validation-to-commit window closes on the child row's
own header: an uncommitted child answers a parent's `DELETE` busy because
the walk reads it (`foreign-keys.md` §2a/§3a). Nothing inside a
`WriteScope` waits.

### 6b. Inserts and the Tail

Every core issues an omitted tuple key from the relation's one row-id mark,
bumped in place under its page latch, so **an issued tuple key is an identity
and a sequence**, monotonic in issue order across every core; a named key
carries no order on a btree
(`docs/spec/heap-and-tuple.md` §4.1, §4.1a; invariant 11).
`range_size_ids` is refused by name (CC10).

## 7. Cancellation, Errors, Early Termination

**Nothing crosses to cancel.** A statement runs on its session's core, so
an early stop (`LIMIT`) is the walk's own quota, an error is framed once by
the session that ran the statement, and a connection close tears down only
that core's state (the same hook that rolls back an open transaction).

## 8. Determinism and Testing

What crosses a core is a kick, whose deterministic shape is the two-core
rig's seeded `SimWakerTable` (`docs/spec/sched.md` §8).

1. **Shared statement view:** a transaction writing two relations
   commits while a statement reads them; the statement's answer contains
   all of that transaction's writes or none (§5's one view per
   statement, pinned against the torn read).
2. **One sequence across cores:** k cores inserting omitted-tuple key rows into
   one relation at once issue ids unique across cores (K1's issue-once
   contract) and ascending in issue order (§6b).

## 9. Open Items

The open decisions of this subsystem are unrecorded here.
