#pragma once

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog_cache.hpp"
#include "kds/catalog/core_placement.hpp"
#include "kds/catalog/row_id_lease.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/catalog/sys_object_registry.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/storage/tagged_cell.hpp"
#include "kds/txn/read_view.hpp"

namespace kds::txn {
// Forward-declared rather than included: the catalog asks it one question
// (`IsInFlight`), and `txn/manager.hpp` drags the WAL manager and the
// checkpointer in behind it, into every translation unit that names a
// relation.
class TransactionManager;
}  // namespace kds::txn

namespace kds::wal {
// Forward-declared for the same reason: the catalog only stores the
// pointer here; the record encoding lives in catalog.cpp.
class WalManager;
}  // namespace kds::wal

// SQL catalog: sys.objects, sys.tables, sys.columns, sys.types,
// sys.indexes, sys.patterns. Four things to know before touching it:
//
//   - It depends on kds::storage::PageStore, an abstract seam, rather than
//     on a buffer pool - so a real buffer-pool-backed store can be swapped
//     in later without touching this file.
//   - CreateTable() supports both ClusteredType values. Either way the
//     relation is one page at creation - a heap page or a B+ tree leaf -
//     rooted at `desc_page_id`; what differs is what grows out of it
//     (heap_chain.hpp vs btree.hpp).
//   - Rows written here are stamped kBootstrapXid **unless a caller
//     supplies a transaction id** (workplan-ddl-transactional.md DT2).
//     kBootstrapXid is visible to every read view, permanently, which is
//     what bootstrap rows require and what every caller still gets by
//     default. A real id only means anything once catalog *reads* filter
//     by visibility (DT3); until then this parameter is a seam and
//     nothing passes a real id.
//   - Object oids (GenerateUserOid()) are unique for the life of the
//     database. The counter is recovered from the catalog's own rows on
//     first use rather than persisted; see that function and
//     kUserOidStart in well_known.hpp. This was a KNOWN GAP until
//     2026-08-08 - the counter was in-memory and restarted every boot,
//     so a clean restart plus one CREATE TABLE produced two relations
//     sharing an oid (docs/rules/keystoneid-k0-findings.md §6).
//
// Logging (component tag "catalog"): catalog pages are the pages whose
// contents explain every other page, so the writes are logged at Info
// (bootstrap, CREATE TABLE - rare, structural, and the thing an operator
// reconstructs a database's history from) and the per-row mutations at
// Trace (id issue, desc-page relink). Reads are not logged at all: they
// are the common case and say nothing about what changed - and that now
// includes cache hits, which are the most common event in this file.
// Cache invalidation is logged at Debug: it is bounded by DDL, and DDL's
// catalog side already reports there.
//
// Reads are served from an in-memory CatalogCache (catalog_cache.hpp),
// which owns the rule for what may be cached. The two things to know
// before touching this file: `next_id` is never cached, so AllocateRowId()
// and GetSysTableRow() always read the page; and every mutation of a
// cached fact goes through BumpVersion(), the single invalidation point.

namespace kds::catalog {

// Where a catalog row landed (workplan-ddl-transactional.md DT3a). A DDL
// running inside a transaction hands these to
// `TransactionManager::NoteInsert` so a rollback can retire the slots:
// the engine undoes aborted work by compensation, and a row nobody
// recorded is a row nobody can take back.
struct CatalogRowRef {
    PageId page_id = kInvalidPageId;
    std::uint16_t slot = 0;
    // The row's own oid, which is what `Compensate` re-reads to confirm it
    // is retiring the row it recorded rather than whatever now occupies
    // the slot. Every catalog row carries its oid in the first eight
    // bytes, which is exactly where a Keystone id would be.
    Oid oid = 0;
    // Which sys relation the row lives in - `kSysObjectsTable`,
    // `kSysTablesTable`, `kSysColumnsTable`. Carried so a rollback's
    // `RowLocator` is handed a real relation if it is ever consulted,
    // rather than a zero nobody can look up. The catalog knows this and
    // the caller would have to guess, which is why it is set here.
    Oid rel_oid = 0;
};

// One catalog row a DROP changed, and enough to undo it
// (workplan-ddl-transactional.md DT5). Two shapes: a delete-mark, which a
// rollback clears; and an in-place overwrite, which a rollback rewrites
// from `prior_image` - the only copy there is, since a catalog row has no
// undo chain.
struct CatalogRowChange {
    PageId page_id = kInvalidPageId;
    std::uint16_t slot = 0;
    Oid oid = 0;
    Oid rel_oid = 0;
    std::uint64_t prior_trx_id = 0;
    std::uint64_t prior_undo_ptr = 0;
    bool deleted = false;  // true: a delete-mark; false: an overwrite
    std::vector<std::byte> prior_image;
};

class Catalog {
public:
    // `inline_cell_width` is the instance-pinned kds.inline_cell_width
    // (storage/tagged_cell.hpp): every RowLayout this catalog builds is
    // built for it, so a relation's row size is the same number for the
    // life of the database. It is a constructor parameter rather than a
    // setter because a catalog that has already handed out a TableAccess
    // must not be able to start answering with a different width. Bootstrap
    // supplies the value the superblock pinned; the default is only for
    // callers that never store a varchar (the bootstrap catalog itself,
    // and tests).
    //
    // `core_count` is the instance-pinned `cores`
    // (docs/inflight/in-progress/workplan-crosscore.md M6), and is here for the same reason and
    // under the same rule: it is fixed for the life of the database, and
    // CreateTable() needs it to assign `sys.tables.owner_core`. The catalog
    // does not *decide* placement - catalog/core_placement.hpp does - it
    // only supplies the two inputs that policy takes.
    // `core_id` exists for one judgment (PW2-3, the f5686f8 review's C1):
    // whether an unresolvable anchor page belongs to a *foreign* relation
    // - whose root is never walked here, so the row's value is a harmless
    // stand-in - or to one of **this core's own**, where a silent
    // fall-through would serve a CREATE-time root once roots move
    // (reachable the day PW2-4 lifts the btree gate). Defaulted to the
    // system core: every pre-existing construction site is core 0's.
    explicit Catalog(storage::PageStore& store,
                     std::uint32_t inline_cell_width = storage::kDefaultInlineCellWidth,
                     std::uint32_t core_count = 1,
                     std::uint32_t core_id = kSystemCore) noexcept
        : store_(store),
          inline_cell_width_(inline_cell_width),
          core_count_(core_count),
          core_id_(core_id) {}

    // Diagnostic log, null (discard) by default. `log` must outlive the
    // catalog. Set rather than constructed with, because bootstrap builds
    // a Catalog before the server has decided anything about logging.
    void SetLogger(Logger* log) noexcept { log_ = log; }

    // The core's transaction manager, for the one question an *unfiltered*
    // catalog read has to ask: is the transaction that delete-marked this
    // row still running? (`ddl-transactional.md` §5b.) Set rather
    // than constructed with, for SetLogger's reason - the manager is built
    // after the catalog, and bootstrap has none at all. What null means is
    // on the member.
    void SetTransactionManager(const txn::TransactionManager* txn) noexcept { txn_ = txn; }

    // The WAL, for RV3 (`docs/workplan-rv3-catalog-recovery.md`): every
    // catalog page mutation logs the ordinary record types - kHeapInsert,
    // kHeapOverwrite, kHeapDeleteMark, kSlotRetire, kPageInit, plus a
    // full page image for a chain-link edit - exactly as wal.md's
    // "Catalog" line always planned. Set rather than constructed with,
    // for SetLogger's reason; null (bootstrap, socket-free tests) means
    // catalog writes are unlogged, which is the pre-RV3 engine exactly.
    void SetWal(wal::WalManager* wal) noexcept { wal_ = wal; }

    // One catalog write a crash loser would need rolled back, reported
    // **before the write's own record is logged** - which is the entire
    // reason this is a hook inside the write points rather than a list
    // the caller reads afterwards: the undo record the hook appends must
    // precede the row record in the log, or redo alone can resurrect a
    // loser's row that recovery's undo phase has no record to retire
    // (the DML path's UNDO_WRITE-before-HEAP_INSERT order, mirrored).
    //
    // `bytes` is the encoded row for an insert, the **prior image** for
    // an overwrite, and empty for a delete-mark. `prior_trx`/`prior_undo`
    // are the header being replaced (meaningless for an insert).
    struct DdlUndoEvent {
        enum class Kind : std::uint8_t { kInsert, kOverwrite, kDeleteMark };
        Kind kind = Kind::kInsert;
        PageId page_id = kInvalidPageId;
        std::uint16_t slot = 0;
        std::span<const std::byte> bytes;
        std::uint64_t prior_trx = 0;
        std::uint64_t prior_undo = 0;
        // The identity the compensation will re-read - the Keystone word
        // of the row's payload, never a `.oid` member (DT5's rule, which
        // not every catalog row has).
        std::uint64_t pk = 0;
    };
    using DdlUndoHook = std::function<Status(const DdlUndoEvent&)>;

    // Installed by the dispatcher for the duration of one DDL statement
    // that runs under a transaction, cleared right after; empty means no
    // undo record is wanted (bootstrap, a manager-less dispatcher). The
    // hook deliberately does NOT fire for the non-DDL catalog writes -
    // the row-id carve and bump, heat - which are never rolled back by
    // design (an abort burns ids, K3).
    void SetDdlUndoHook(DdlUndoHook hook) { ddl_undo_hook_ = std::move(hook); }

    // Called after every DDL that invalidates cached facts - i.e. from
    // BumpVersion(), the single choke point, and from nowhere else
    // (docs/inflight/in-progress/workplan-crosscore.md P6).
    //
    // On the system core this is what broadcasts `kCatalogInvalidate` so
    // peers drop their caches and re-read. Hooking the choke point rather
    // than each DDL site is the whole reason that choke point was built:
    // a DDL added later broadcasts without knowing this exists.
    //
    // Deliberately **not** fired by `AllocateRowId()` or `RegisterPattern()`.
    // Neither bumps the version - a sequence position and a pattern's
    // existence are not cached facts (catalog_cache.hpp) - so neither is a
    // DDL a peer needs to hear about, and firing on them would put a
    // broadcast on the statement path.
    using InvalidationHook = std::function<void()>;
    void SetInvalidationHook(InvalidationHook hook) { on_invalidate_ = std::move(hook); }

    // Called at the end of a CreateTable whose owner is **not** the system
    // core (workplan P6c) - the send side of CC7's flush-then-grant
    // handoff. The system core's installer flushes the relation's pages and
    // sends the owner a `kRelationFaultGrant`; with no hook installed a
    // rotated relation is created and never granted, which the affinity
    // check already refuses honestly. Arguments: the relation's oid, its
    // owner core, its root page, its var-heap root (kInvalidPageId when
    // none), and its anchor page (PW2-1).
    using RelationPublishHook =
        std::function<void(Oid, std::uint32_t, PageId, PageId, PageId)>;
    void SetRelationPublishHook(RelationPublishHook hook) { on_publish_ = std::move(hook); }

    // The placement rule CreateTable hands to catalog::AssignOwnerCore()
    // (workplan P6c, the `placement` config key). Default kCreatingCore;
    // set once at startup, before any DDL.
    void SetPlacementPolicy(PlacementPolicy policy) noexcept { placement_ = policy; }

    // Row-id leases for a core that may not write the catalog (P5's shape;
    // catalog/row_id_lease.hpp). With a table installed, AllocateRowId()
    // draws from it - no catalog write, retryable exhaustion when a
    // relation's lease is spent - and the catalog page is only ever
    // touched by core 0's AllocateRowIdRange() carving the blocks. Null
    // (the default) is core 0's arrangement and the path that always
    // existed. `leases` must outlive the catalog.
    void SetRowIdLeases(RowIdLeaseTable* leases) noexcept { row_id_leases_ = leases; }

    // Drops every cached fact without bumping the version. What a **peer**
    // does on receiving `kCatalogInvalidate`: the version counter is
    // per-instance and means nothing across cores, but the cache contents
    // are what would otherwise be stale.
    void InvalidateFromPeer();

    // A transaction that wrote catalog rows has resolved, and what this
    // catalog cached while it was open may now be a lie. **Both endings
    // reach here**, for two different reasons:
    //
    //   - a **rollback** (workplan-ddl-transactional.md DT4) compensates
    //     through the page directly, retiring slots the catalog is never
    //     told about, so a fact cached while the DDL was open outlives the
    //     rows it describes;
    //   - a **commit** is the moment a delete-mark starts counting (DT9,
    //     spec §5b). A cache filled during an open `DROP INDEX` holds the
    //     index deliberately - that is what keeps maintenance writing
    //     entries a rollback would need - and must not hold it afterwards.
    //
    // Without either, resolution goes back to the cache once the
    // transaction resolves and serves what it found mid-flight.
    //
    // The name says "compensation" for the case it was built for; it is
    // the resolution of a catalog-writing transaction that calls it.
    //
    // Drops cached content **and bumps the version**, unlike
    // `InvalidateFromPeer`: this *is* an event in this instance's
    // numbering - the rows really did change here - and a bound statement
    // compiled against the relation that just vanished must not be
    // considered current.
    void InvalidateAfterCompensation();

    // **DT10** (`ddl-transactional.md` §5c): retires every
    // delete-marked catalog row, and answers how many. Runs once at mount,
    // on the system core, before the listener binds — never afterwards,
    // because afterwards a mark may belong to a transaction that is still
    // open, and retiring one of those destroys the row a rollback needs.
    //
    // It exists because DT9 made a mark's meaning depend on whether its
    // deleter is in flight, and a mark that outlived its mount has no
    // deleter to ask about. The id could even be **reissued** — the
    // transaction-id ceiling is unlogged (`txn/trx_id.hpp`), so a crash
    // reissues the block — and then a live transaction wearing a dead
    // one's id would make a committed drop read as open, re-arm a dropped
    // index, and answer probes from a btree missing every row written
    // since. Finalizing at mount deletes that question instead of
    // answering it.
    //
    // **Retiring is the only available answer, not the conservative one.**
    // A mark whose transaction committed should be gone. A mark whose
    // transaction did not commit cannot be rolled back either: the trail
    // that would compensate it died with the process, and the catalog is
    // not recovered (RV3), so nothing can reconstruct the intent. Both
    // already read as gone to every unfiltered reader before DT9. What
    // this changes is that they stop being ambiguous.
    //
    // Second effect, and the reason it is worth a page sweep on its own:
    // nothing else ever purges these rows. A transactional `DROP TABLE`
    // leaves one mark per column, index and foreign key, forever, re-read
    // on every catalog cache miss.
    //
    // Unlogged, like every catalog write (RV3). A crash mid-sweep leaves
    // some marks retired and some not, which is exactly the state it
    // started from and what the next mount sweeps again.
    StatusOr<std::uint64_t> FinalizeDeleteMarksAtMount();

    // The in-mount sibling of the sweep above: retires every delete-marked
    // catalog row whose deleter has cleared the core's read horizon, and
    // answers how many. Callable while the listener is bound; no version
    // bump, unlogged like every catalog write; a held-back mark survives
    // to the next call or to the mount sweep. The caller is DDL resolution
    // (`CommandDispatcher::EndDdlScope`, system core only). The soundness
    // proof lives once, in `ddl-transactional.md` §5d.
    StatusOr<std::uint64_t> PurgeSettledDeleteMarks();

    // Registers the fixed namespace/type sys-objects in the in-memory
    // registry (no disk I/O - these are well-known constants, not stored
    // as catalog rows themselves).
    void InitWellKnownObjects();

    // Allocates the fixed catalog pages (kCatalogPage*) via store_ and
    // populates them with bootstrap rows for sys.types/objects/columns/
    // tables/indexes. Must be called exactly once, against a PageStore
    // that has none of those page ids yet - fails with whatever error
    // the first conflicting CreateAt() reports.
    Status Bootstrap();

    // Allocates one fresh object oid, **unique for the life of the
    // database** and not merely for the life of the process
    // (docs/rules/keystoneid-k0-findings.md §6).
    //
    // The counter is not stored anywhere. It is *recovered* on first use by
    // reading back the highest oid the catalog already carries, and every
    // call after that is an in-memory increment - so a boot costs one scan
    // and a `CREATE TABLE` costs nothing extra. Recovering beats persisting
    // here because there is no durable counter to fall behind the rows it
    // describes: the rows **are** the counter, so a crash between issuing an
    // oid and writing its row loses the oid rather than duplicating it, and
    // losing one is free (`keystoneid-invariant.md` K3 - no density promise).
    //
    // **The contract this depends on**, and the one thing to check before
    // adding a caller: every oid handed out here must end up in `sys.objects`
    // or `sys.columns`, because those are the two relations
    // `HighestIssuedUserOid()` reads back. An oid written only to some third
    // relation would be invisible to the recovery and reissued after a
    // restart. Nothing else in the engine takes its ids from here - patterns,
    // cabins, foreign keys and indexes each use their own persistent
    // per-relation `next_id` sequence - so the contract holds today, and the
    // scan is where to extend it if that changes.
    //
    // Fallible now, where it used to be `noexcept`: reading the catalog can
    // fail, and an oid guessed after a failed read is exactly the collision
    // this exists to prevent.
    StatusOr<Oid> GenerateUserOid();

    // Creates a new table: allocates its storage root page, formats it for
    // the requested clustered type, and inserts the corresponding
    // sys.objects/sys.tables/sys.columns rows.
    //
    // There is no key-mode parameter. Until 2026-08-25 there was, and it was
    // required rather than defaulted because getting it wrong made a relation
    // refuse every INSERT its author wrote; the mode is gone (section 4.1),
    // every relation now takes a supplied pk or issues one per row, and the
    // parameter went with it.
    // `trx_id` stamps the `sys.objects` / `sys.tables` / `sys.columns`
    // rows this writes (workplan-ddl-transactional.md DT2). Defaulting to
    // `kBootstrapXid` is what keeps every existing caller - bootstrap,
    // tests, recovery - byte-identical, and it is the *correct* value for
    // them: those rows must be visible to a read view minted before any
    // transaction existed (ddl-transactional.md §3).
    //
    // **Passing a real id is only correct once catalog reads filter by
    // visibility** (DT3). Until then a real id would make the rows
    // invisible to nobody and visible to everybody, which is what they
    // already are - so DT2 changes no behaviour anywhere, deliberately.
    // `written`, when given, receives every catalog row this created, in
    // write order, so a transactional caller can register them for
    // rollback (DT3a). Filled even when the create fails partway: rows
    // already on the page are rows a rollback must still retire.
    StatusOr<Oid> CreateTable(Oid namespace_oid, std::string_view name, const Schema& schema,
                               ClusteredType clustered_type,
                               std::uint64_t trx_id = kBootstrapXid,
                               std::vector<CatalogRowRef>* written = nullptr);

    StatusOr<SysTableRow> GetSysTableRow(Oid table_oid);

    // Scans sys.objects (disk, not the in-memory well-known registry -
    // CreateTable() only writes the disk row) for a row named `name`
    // with type_oid == kTypeTable.
    //
    // `view` is the reader's visibility (workplan-ddl-transactional.md
    // DT3). With one, a relation whose creating transaction this view
    // cannot see **does not exist** for this caller, and the lookup
    // neither reads nor fills the shared cache - see the definition for
    // why that bypass is the whole of the isolation. Null (the default)
    // is every pre-DT3 caller and behaves exactly as it always did.
    //
    // **This is the gate.** SQL reaches a relation by name and never by
    // oid, so a name that does not resolve is a relation that cannot be
    // touched - which is why DT3 filters here and leaves `InitTableAccess`
    // on the cache.
    StatusOr<Oid> FindTableOidByName(std::string_view name,
                                      const txn::ReadView* view = nullptr);

    // Is `name` claimed by a **tombstone whose drop has not committed**?
    //
    // `DROP TABLE` frees a name the instant it runs, for everybody: the
    // retype to `kTypeDroppedTable` is an in-place overwrite and a catalog
    // row has no undo chain, so no reader can be isolated from it
    // (ddl-transactional.md §5a). But the drop can still roll back,
    // and the rollback rewrites that row back to a live `kTypeTable` -
    // so a `CREATE TABLE` that took the name meanwhile leaves **two live
    // rows claiming one name**, which is precisely the corruption §6's
    // unfiltered duplicate check exists to prevent. That check cannot see
    // it, because the retype hid the name from it.
    //
    // True when some `kTypeDroppedTable` row carries `name` and its stamp -
    // the dropping transaction's, written by the retype - is one `view`
    // cannot see. A view minted by the asking session answers "another
    // transaction's pending drop" and not its own, so `DROP TABLE t;
    // CREATE TABLE t` inside one transaction still works.
    StatusOr<bool> NameHeldByPendingDrop(std::string_view name, const txn::ReadView& view);

    // Lists every table registered in sys.objects (type_oid == kTypeTable),
    // including the catalog's own bootstrap tables - not just user-created
    // ones. Added for the
    // server's command dispatcher (src/server), which needs "what tables
    // exist" without the caller already knowing a name to look up.
    StatusOr<std::vector<SysObjectRow>> ListTables(const txn::ReadView* view = nullptr);

    // ALTER TABLE's two catalog writes (docs/spec/alter.md AL2, AL5,
    // workplan ALT02): rewrite one fixed-width Name field in place - same
    // size, no relayout - then BumpVersion(), with no in-place-cache
    // exception, because a name is read by resolution itself. Identity is
    // the oid, so nothing else in the catalog moves.
    //
    // Refusals: an empty name or one that does not fit kCatalogNameMax is
    // InvalidArgument (SetName would truncate silently, and a truncated
    // name is not the one that was asked for); a taken name is
    // AlreadyExists; a relation outside the public namespace is refused -
    // the catalog's own names are load-bearing for bootstrap (AL7).
    Status RenameTable(Oid table_oid, std::string_view new_name);
    Status RenameColumn(Oid table_oid, std::string_view old_name, std::string_view new_name);

    // DROP TABLE's catalog half (docs/spec/drop-table.md DT1-DT3, workplan
    // DT02). The sys.objects row is **retyped to kTypeDroppedTable, never
    // retired** - the rows are GenerateUserOid()'s counter, so the row
    // must stay to keep the dead oid from being reissued, while name
    // resolution's kTypeTable filter frees the name at once. Everything
    // the relation owns retires: its sys.tables row, every sys.columns /
    // sys.indexes / sys.cabins row and its child-side sys.fkeys rows.
    // Pages are NOT reclaimed (DT1 - reclamation is gated elsewhere).
    // The dropped cabin ids land in `dropped_cabins` for the caller's
    // in-memory CabinStore::Forget. RESTRICT checks (a referencing fk, an
    // assertion) are the dispatcher's, made *before* this call - this
    // function only refuses a non-public relation and an unknown oid.
    // One BumpVersion() at the end.
    // `trx_id` and `written` make the drop transactional (DT5): the
    // dependent rows are **delete-marked** rather than retired, so a
    // rollback can clear the marks, and every change is reported for the
    // caller to register on its trail. Defaulted to the autocommit path,
    // which retires exactly as it always did.
    //
    // **Atomic, not isolated** - see ddl-transactional.md §5a. The
    // `sys.objects` retype is in place and a catalog row has no undo
    // chain, so other sessions see the relation become a tombstone the
    // moment the drop runs, before it commits.
    Status DropTable(Oid table_oid, std::vector<std::uint64_t>& dropped_cabins,
                      std::uint64_t trx_id = kBootstrapXid,
                      std::vector<CatalogRowChange>* written = nullptr);

    // T3's contiguous id range (docs/inflight/in-progress/workplan-t3.md T3-3): one catalog
    // write bumps next_id by `count` and returns the first id - issuance
    // inside the range is monotone by construction. Refused whole when it
    // would cross the 40-bit ceiling; an aborted statement burns the whole
    // range (K3: a burned id is free). Never cached, like every sequence.
    StatusOr<std::uint64_t> AllocateRowIdRange(Oid table_oid, std::uint64_t count);

    // Returns an owned copy, served from the cache when the relation has
    // been opened before. NotFound for a relation with no sys.columns rows
    // (the bootstrap catalog tables) - an absence, and so never cached.
    StatusOr<Schema> BuildSchemaFromColumns(Oid rel_id);

    // Resolves a CREATE TABLE column's raw, unresolved type_name (e.g.
    // "int64", "varchar") to its sys.types row, case-insensitively.
    // NotFound if no such type is registered. This is today's stand-in
    // for the not-yet-ported type registry (ast.hpp's file comment):
    // Bootstrap() already populates sys.types with every scalar type
    // Catalog knows about, so resolving by name against that table is a
    // real lookup, not a guess.
    StatusOr<SysTypeRow> ResolveTypeByName(std::string_view name);

    // The reverse lookup, for rendering a stored column's type back as a
    // name (DESCRIBE). NotFound if no sys.types row carries this type_val.
    StatusOr<SysTypeRow> ResolveTypeByVal(std::uint32_t type_val);

    // Opens a relation, caching the result. The returned pointer belongs to
    // this Catalog's cache: it stays valid until the next DDL through this
    // instance (which drops the entry) or the Catalog is destroyed, and it
    // survives caching other relations, because cache entries are
    // reference-stable (catalog_cache.hpp). Callers must not hold one past
    // the statement that took it.
    //
    // Issuing a row id does *not* invalidate it - AllocateRowId() touches
    // only `next_id`, which TableAccess does not carry - so a statement may
    // hold this across its own inserts, as HandleInsert does.
    //
    // The namespace is read from the relation's sys.tables row; it used to
    // be a caller-supplied parameter echoed into the result, which a shared
    // cache entry cannot honor.
    StatusOr<const TableAccess*> InitTableAccess(Oid oid);

    // Issues the next Keystone id for `table_oid` and persists the bumped
    // sequence. This is what an `INSERT` that **omits** the pk calls, on
    // every relation - there is no longer a key mode that makes it illegal
    // (section 4.1). Ids are unique by construction and rise above every id
    // the relation has ever placed, since `AdmitExplicitRowId` moves the same
    // mark past every supplied one; they are not gapless, since a failed
    // insert after a successful allocation burns one.
    //
    // Fails with NotFound if no sys.tables row names `table_oid`, and with
    // OutOfRange once the relation has issued its 40-bit id space -
    // reclamation policy is an open decision, so exhaustion is reported
    // rather than wrapped.
    StatusOr<std::uint64_t> AllocateRowId(Oid table_oid);

    // What an `INSERT` that **supplies** the pk calls (docs/spec/heap-and-tuple.md
    // section 4.1): admits a caller-named id and moves the relation's
    // high-water mark past it. Every relation may be inserted into either
    // way; which of the two functions runs is a property of the row, not of
    // the relation.
    //
    // ---- What this checks -------------------------------------------------
    //
    // **Spellability, always.** Within [kFirstRowId, kMaxKeystoneId], since 0
    // means "unset" and the Keystone field is 40 bits wide. `InvalidArgument`
    // otherwise - a value outside the id space is simply wrong, not a
    // declined feature. Checked before the catalog page is touched at all.
    //
    // **Then the ordering rule, and it is the storage type that decides it:**
    //
    //   - **Btree-clustered: any id.** Uniqueness is proved completely by the
    //     descent, which lands on the one leaf that may hold the key and
    //     finds the duplicate or does not, so `next_id` is asked nothing. An
    //     id below the mark leaves it untouched, writes no catalog page for
    //     the mark, and flips `key_order` to kUnordered if it was not already
    //     - once per relation, ever.
    //   - **Heap-clustered: at or above the mark only**, `OutOfRange`
    //     otherwise. The semi-sorted chain grows at its tail, so an id below
    //     the tail page's `min_key` has no legal page (invariant 3); and its
    //     duplicate check reads the tail page alone, which is sound only
    //     while every earlier page's ids sit below the tail's bound. Both
    //     properties are the ascent, and the mark is what preserves it: at or
    //     above it, the id is above every id the relation has ever placed, so
    //     uniqueness follows from the mark without a page being read. A heap
    //     relation therefore never reaches kUnordered.
    //
    // ---- Why the high-water mark moves ------------------------------------
    //
    // `next_id` is a ceiling on what has been *placed*, which is what makes
    // it the uniqueness proof on a heap relation and what keeps K4's lifetime
    // budget and the 40-bit exhaustion refusal truthful on a btree one -
    // DESCRIBE and SHOW BUDGET both read it. Advanced with max(), never
    // assigned, and persisted before the caller places anything: a crash
    // between here and the insert leaves a ceiling that is too high (K3 calls
    // a burned id free) rather than one too low, and a too-low mark is how
    // the engine later issues an id that is already a tuple's identity.
    //
    // Fails with NotFound if no sys.tables row names `table_oid`.
    Status AdmitExplicitRowId(Oid table_oid, std::uint64_t id);

    // ---- sys.patterns (docs/spec/waystone-concpets.md section 4) --------------

    // The pattern `pattern_id` names, served from the cache after the first
    // lookup. The pointer is reference-stable and stays valid until the
    // next Invalidate() - same contract as InitTableAccess().
    //
    // Fails with NotFound when the pattern has never been registered **and
    // when the only row for it came from another fingerprint revision**.
    // Those two are deliberately one outcome: a row recorded under
    // different fingerprinting rules names a shape that is not the one it
    // claims, so the only safe reading of it is that this pattern has not
    // been seen. The caller registers it afresh and the stale row becomes
    // garbage for retention to reclaim (P15). A version mismatch is never
    // an error - nothing about it should fail a statement.
    StatusOr<const PatternAccess*> FindPattern(std::uint64_t pattern_id);

    // Records a newly observed pattern and returns its cached access, so a
    // caller that just registered does not look it back up.
    //
    // **The version is stamped here, not passed in.** No caller has any
    // business recording a pattern under a revision other than the one
    // that computed its `pattern_id`, so a version parameter could only
    // ever be passed wrong - and passing it wrong writes a row no build
    // will resolve. Removing the parameter is what makes "the cache holds
    // current-version entries only" true by construction.
    //
    // Registration bumps **no** catalog version, and that is worth stating
    // because the deleted per-relation Waystone got it wrong in exactly
    // this spot. Nothing cached can go stale from a pattern appearing:
    // absences are never cached (catalog_cache.hpp), so no entry claims
    // this pattern is missing, and no other cached fact mentions it. The
    // consequence matters on the statement path - registering a pattern
    // mid-statement cannot dangle the `const TableAccess*` that statement
    // is holding, which is the hazard `waystone-concpets.md` section 4
    // flagged for this task.
    //
    // The pattern's `oid` comes from AllocateRowId(kSysPatternsTable) - the
    // persistent sequence in sys.patterns' own sys.tables row. That is a
    // repurposing worth naming: catalog rows carry no Keystone word, so
    // this is the sequence used as an oid source rather than as a tuple id.
    // The alternative, GenerateUserOid(), is in-memory and restarts at
    // kUserOidStart every boot (well_known.hpp), which for a *persisted*
    // row means two patterns sharing an oid across a restart.
    //
    // Fails with AlreadyExists if `pattern_id` is already registered at
    // the current version. A row left by an older revision does not count
    // as registered, so a version bump leaves every shape re-learnable
    // rather than permanently blocked.
    //
    // `origin` and `flags` (rows.hpp) default to what auto-registration
    // wants - an unpinned kOriginAuto row - so the observing path never has
    // to name them. `CREATE PATTERN` passes kOriginUser and, by default,
    // kPatternPinned: declaring a pattern and then letting retention evict
    // it silently would defeat the declaration.
    //
    // The row is always written with **no waystone directory**. A declared
    // pattern's `expected_instances` becomes a depth through the one writer
    // of that pair, SetPatternWaystoneRoot(), immediately after.
    StatusOr<const PatternAccess*> RegisterPattern(std::uint64_t pattern_id,
                                                    std::uint8_t stmt_class,
                                                    std::uint8_t origin = kOriginAuto,
                                                    std::uint16_t flags = 0);

    // Points a pattern at its waystone directory, writing root and depth as
    // one unit.
    //
    // The two are one fact and there is deliberately no setter for either
    // alone: a root without its depth is unwalkable, and a depth that
    // disagrees with the root sends every walk to the wrong leaf. Validated
    // before the page is touched - depth 0 requires kInvalidPageId, and any
    // other depth requires a real root and a depth within
    // kMaxPatternDirDepth - so every reader downstream may trust the pair
    // without re-checking it.
    //
    // Updates the cached PatternAccess in place rather than invalidating
    // (catalog_cache.hpp explains why that is the exception it is), so a
    // caller holding a `const PatternAccess*` keeps a valid pointer and
    // sees the new directory.
    //
    // Fails with NotFound if no sys.patterns row carries `pattern_id`, and
    // with InvalidArgument for an incoherent pair. Page lifetime of the old
    // directory is the caller's business: this writes the row.
    Status SetPatternWaystoneRoot(std::uint64_t pattern_id, PageId root, std::uint8_t depth);

    // Rewrites a pattern's lifecycle policy - who owns it, and whether
    // retention may evict its waystones. This is what `CREATE PATTERN`
    // calls when it finds an auto-registered row for the shape being
    // declared: the pattern is *adopted*, not replaced.
    //
    // **`waystone_root` and `dir_depth` are not touched**, which is the
    // whole point of adopting rather than re-registering. The trails
    // already recorded under this pattern_id are trails for the same shape,
    // and throwing them away to honour a declaration would make declaring a
    // hot pattern a performance regression.
    //
    // Updates the cached PatternAccess in place, exactly as
    // SetPatternWaystoneRoot() does and for the reason catalog_cache.hpp
    // gives. Fails with NotFound if no current-version row carries
    // `pattern_id`, and InvalidArgument for an origin that is neither
    // kOriginAuto nor kOriginUser.
    Status SetPatternOrigin(std::uint64_t pattern_id, std::uint8_t origin, std::uint16_t flags);

    // Records that a pattern was executed: bumps `use_count` and stamps
    // `last_seen`. The trail recorder's catalog half.
    //
    // **Deliberately not cached, and it must not become so.** Both fields
    // move on every execution, which is not DDL - so a cached copy would be
    // stale the moment it was taken and no invalidation could fix it. That
    // is exactly why `PatternAccess` omits them (schema.hpp), and this
    // writes the page without touching the cache at all.
    //
    // Best-effort by contract (rows.hpp): these counters rank patterns for
    // retention and nothing reports them, so a caller that drops a failure
    // here loses a statistic, never a result. `use_count` saturates rather
    // than wrapping - a wrapped count would make the hottest pattern look
    // cold.
    //
    // Fails with NotFound when no current-version row carries `pattern_id`.
    Status TouchPattern(std::uint64_t pattern_id, std::uint64_t last_seen);

    // Removes a pattern's sys.patterns row. `DROP PATTERN`'s catalog half.
    //
    // **Retires the slot rather than delete-marking it.** A delete-mark is
    // filtered by a reader's snapshot, and catalog reads have no snapshot -
    // so a marked row would still be found by every lookup here, and
    // re-registering the same shape would fail against a row nobody can
    // see. This is the first path in the engine that removes a catalog row;
    // when a transaction manager exists it is one of the places that will
    // need revisiting, because a retire is not rollback-able.
    //
    // The waystone tree under the row's root is **not** freed. Nothing
    // reclaims pages yet, and invariant 8 makes leaving it safe: an orphaned
    // trail can cost space, never an answer.
    //
    // Fails with NotFound when no current-version row carries `pattern_id`.
    Status RetirePattern(std::uint64_t pattern_id);

    // Every sys.patterns row, in page order.
    //
    // **Unfiltered, unlike GetSysPatternRow()** - rows from another
    // fingerprint revision are included. That is not a hole in the version
    // rule: the rule protects *lookup by pattern_id*, so a stale row can
    // never resolve as the pattern it names. This is an inspection surface,
    // and the stale rows are exactly what an operator wants to see - they
    // are the garbage a version bump left behind, waiting on retention.
    // A caller that wants the version rule applied asks for a pattern by
    // id; a caller that wants to look at the relation reads this.
    StatusOr<std::vector<SysPatternRow>> ListPatterns();

    // Every sys.types row, through the same cached snapshot
    // ResolveTypeByName() reads. Exposed for the `sys.types` catalog view
    // (exec/catalog_view.hpp), which needs to enumerate what the
    // by-name lookup can only probe one at a time.
    //
    // The snapshot is safe to hand out because types are bootstrap-only:
    // nothing creates one at runtime, so unlike the table list it cannot
    // go stale under a caller.
    StatusOr<const std::vector<SysTypeRow>*> ListTypes();

    // The pattern's row straight off the page, never from the cache.
    //
    // The counterpart of GetSysTableRow(): it exists for the fields
    // PatternAccess deliberately omits - `use_count` and `last_seen` -
    // which change on every execution and so are not cacheable facts
    // (schema.hpp). A caller that wants heat calls this; a caller that
    // wants identity or location calls FindPattern().
    //
    // **Rows from another fingerprint revision are invisible here**, and
    // this is the single place that filter lives. A version bump leaves
    // the old rows on the page - nothing rewrites them - so a lookup that
    // did not filter would let a stale row shadow the current one. Putting
    // the filter in the row lookup rather than in each caller is what
    // makes that impossible rather than merely unlikely.
    StatusOr<SysPatternRow> GetSysPatternRow(std::uint64_t pattern_id);

    // Updates the desc_page_id field of table_oid's sys.tables row in
    // place - for a future btree root split/collapse to repoint at a new
    // root page. Uses an in-place overwrite (row size is unchanged), not
    // delete+insert, since the row size is unchanged.
    // The anchor id comes from the caller's TableAccess (S2's rule); the
    // move writes the anchor alone and updates the cached entry in place -
    // no row write, no invalidation, no catalog scan (PW2-3/PW2-4).
    Status UpdateRelationDescPage(Oid table_oid, PageId new_desc_page_id,
                                  PageId anchor_page_id);

    // ---- sys.access_stats (docs/spec/heap-and-tuple.md §7) --------------------

    // Counts one execution of an access shape, creating its row the first
    // time the shape is seen.
    //
    // `kind` is `exec::StoredAccessKind()`'s value, never a raw cast of the
    // enum. `column_mask` is a bit per `col_pos` the access keyed or
    // filtered on - **columns, never values**, which is what keeps this
    // relation bounded by the schema (rows.hpp).
    //
    // Called from the statement path after a successful execution, and
    // deliberately bumps no catalog version: a statistic appearing cannot
    // stale a cached fact, and a cache drop here would dangle the
    // `const TableAccess*` the running statement holds.
    //
    // Fails with ResourceExhausted once `kMaxAccessShapes` distinct shapes
    // exist - which refuses a *new* shape and keeps counting every known
    // one. Every caller is expected to drop the failure: a statistic that
    // could fail a query would be a worse trade than no statistic.
    Status RecordAccess(std::uint8_t kind, Oid rel_id, std::uint64_t column_mask,
                        std::uint64_t last_seen);

    // Every access shape, in page order. The inspection surface behind
    // `SHOW ACCESS`.
    StatusOr<std::vector<SysAccessStatRow>> ListAccessStats();

    // ---- sys.cabins (docs/spec/cabin.md §10) -----------------------------

    // Declares a Cabin on one non-pk column and returns its `cabin_id`.
    //
    // What this writes is that the Cabin **exists**. Nothing is observed by
    // creating one: the read path's miss branch is what fills it (spec §4),
    // so a fresh Cabin costs a catalog row and changes no answer until
    // traffic teaches it something.
    //
    // Fails with InvalidArgument for column 0 - the pk's Cabin is the
    // clustered tree itself (§2), which is not a refusal a caller can talk
    // its way out of - and for a `col_pos` past the relation's schema;
    // NotFound for an unknown relation; AlreadyExists for a second Cabin on
    // the same column.
    //
    // **Bumps the catalog version**, unlike RegisterPattern(): the mask on
    // TableAccess is derived from these rows, so a Cabin appearing stales
    // every cached relation entry. That makes this a DDL-shaped call and it
    // must not be made from a statement path holding a `const TableAccess*`.
    StatusOr<std::uint64_t> CreateCabin(Oid rel_oid, std::uint16_t col_pos,
                                        std::uint8_t origin = kCabinOriginUser);

    // Removes a Cabin's row. Retires the slot rather than delete-marking it,
    // for the reason RetirePattern() records: catalog reads have no snapshot
    // to filter a mark against, so a marked row would still be found by
    // every lookup here.
    //
    // The observed sets are **not** this function's to drop - they live in a
    // core-local runtime store that the catalog cannot see. The caller drops
    // them, and dropping them late costs memory and never an answer, because
    // a set nothing consults is inert. Bumps the catalog version.
    //
    // Fails with NotFound when no row carries `cabin_id`.
    Status DropCabin(std::uint64_t cabin_id);

    // Every sys.cabins row, in page order. The inspection surface behind
    // `SHOW CABINS`.
    StatusOr<std::vector<SysCabinRow>> ListCabins();

    // The Cabin on `(rel_oid, col_pos)`. NotFound is the ordinary answer -
    // most columns have no Cabin - and is never cached, which is exactly
    // why the *compiler* asks `TableAccess::cabin_mask` instead of calling
    // this per step (catalog_cache.hpp's absence rule).
    StatusOr<SysCabinRow> FindCabinOnColumn(Oid rel_oid, std::uint16_t col_pos);

    // ---- Foreign keys (docs/spec/foreign-keys.md, FK-M1) ----------------

    // Records that `child_rel_oid`'s column `child_column_no` references
    // `parent_rel_oid`'s Keystone id, and returns the new `fk_id`.
    //
    // **The one door.** Every route to a foreign key comes through here -
    // the `REFERENCES` clause today, an `ALTER TABLE ADD CONSTRAINT` if one
    // is ever built - so the declaration checks live here rather than in the
    // DDL layer, the argument Catalog::CreateCabin() already makes about
    // `NO CABIN`. The checks themselves are catalog::CheckForeignKey*()
    // (foreign_key.hpp), shared with CREATE TABLE's pre-check so a
    // declaration cannot be legal at one door and not the other.
    //
    // What it writes is that the constraint **exists**. It validates no
    // existing rows: a foreign key declared at CREATE TABLE is declared on
    // an empty relation, and since there is no ALTER TABLE there is no way
    // to declare one over rows that might already violate it. A back-check
    // is what that path would need, and it does not exist to need it.
    //
    // Fails with NotFound for either relation; InvalidArgument for column 0,
    // a column past the schema, or a column that cannot hold a Keystone id;
    // Unsupported for a heap parent or a cross-core pair (F5); AlreadyExists
    // for a second foreign key on the same column.
    //
    // **Bumps the catalog version**, and reaches further than its arguments:
    // the parent's cached `fkeys_in` is stale too. Not to be called from a
    // statement path holding a `const TableAccess*`.
    StatusOr<std::uint64_t> CreateForeignKey(Oid child_rel_oid, std::uint16_t child_column_no,
                                             Oid parent_rel_oid, std::uint16_t flags = 0);

    // Every sys.fkeys row, in page order. The inspection surface behind
    // `SHOW FKEYS`, and what InitTableAccess() builds both per-relation
    // lists from in one scan.
    StatusOr<std::vector<SysFkeyRow>> ListForeignKeys();

    // The foreign key on `(child_rel_oid, child_column_no)`. NotFound is the
    // ordinary answer - most columns reference nothing - and is never
    // cached, which is why a *write path* asks `TableAccess::fkeys_out`
    // instead of calling this per row (catalog_cache.hpp's absence rule).
    StatusOr<SysFkeyRow> FindForeignKeyOnColumn(Oid child_rel_oid, std::uint16_t child_column_no);

    // The `trx_id` on these three is the row's MVCC stamp (DT2). It
    // defaults to `kBootstrapXid` because every caller that does not pass
    // one is bootstrap, and a bootstrap row must be visible to every read
    // view forever (ddl-transactional.md §3).
    Status InsertObjectRow(Oid oid, Oid namespace_oid, Oid type_oid, std::string_view name,
                            std::uint64_t trx_id = kBootstrapXid,
                            CatalogRowRef* where = nullptr);
    // `owner_core` defaults to kSystemCore because every caller but
    // CreateTable() is bootstrap writing a system relation, and M5 puts
    // those on core 0 by definition - it owns the catalog pages they live
    // on. There is no key-mode parameter to default: the row's `key_order`
    // is an observation set to kAscending here and moved only by
    // AdmitExplicitRowId, never passed in.
    // `anchor_page_id` defaults to kInvalidPageId, the bootstrap value -
    // rows.hpp owns the rule (a system relation carries no anchor).
    // The one anchor write path (the f5686f8 review's S1): validate the
    // page and its owner stamp (the 96b0343 review's C3 - the write is
    // where damage is created), set the slot (index_oid 0 = the clustered
    // root, the record's own discriminator), log and stamp. Three callers
    // - both movers and the CREATE INDEX seed - so the PW2 ordering
    // discipline lives once.
    Status WriteAnchorRoot(PageId anchor_page, Oid expected_owner_oid, std::uint64_t index_oid,
                           PageId root, std::uint64_t trx_id);

    // What WriteAnchorRoot would refuse for `index_oid`, asked without
    // writing anything: the same page validation, then
    // storage::CheckAnchorRoomForIndex, which says why it is worth asking.
    // `kInvalidPageId` is OK - a relation with no anchor has no table to
    // fill, and both callers skip the write for the same id.
    //
    // Read-only through GetForRead, and that is required rather than
    // adequate: `Get` dirties the frame at fetch, so asking through it
    // would make every refused CREATE INDEX cost a write-back of an
    // unmodified page.
    Status CheckAnchorRoomForIndex(PageId anchor_page, Oid expected_owner_oid,
                                   std::uint64_t index_oid);

    Status InsertRelationRow(Oid oid, Oid namespace_oid, std::string_view name,
                              PageId desc_page_id, ClusteredType clustered_type,
                              PageId varheap_page_id,
                              std::uint32_t owner_core = kSystemCore,
                              PageId anchor_page_id = kInvalidPageId,
                              std::uint64_t trx_id = kBootstrapXid,
                              CatalogRowRef* where = nullptr);

    // ---- Secondary indexes (docs/spec/index.md §12) --------------------

    // What CREATE INDEX has settled by the time it reaches the catalog.
    // The widths are computed by the caller from the key columns
    // (exec::IndexKeyWidth) rather than here, because deriving them needs
    // the key encoding and `catalog/` sits below `exec/`.
    struct IndexDef {
        Oid table_oid = 0;
        // Pre-issued index oid (AllocateRowId(kSysIndexesTable)), so the
        // root and every backfill-created page can be stamped with their
        // owner before the sys.indexes row exists (page.md §2a). 0 means
        // CreateIndex() allocates - the pre-§2a behavior, kept for callers
        // that create no pages of their own.
        Oid index_oid = 0;
        std::string name;
        PageId root_page_id = kInvalidPageId;
        std::uint16_t key_width = 0;
        std::uint16_t entry_width = 0;
        std::vector<std::uint16_t> key_cols;      // declared order; part of the format
        std::vector<std::uint16_t> covered_cols;  // declared order
        std::uint8_t flags = 0;
    };

    // Who seeds the relation's anchor slot with the new root (PW2-3).
    // `kHere` is a relation this core owns: the row and the seed are one
    // statement on one core. `kByOwner` is a relation another core owns
    // (workplan-peer-writer.md §7c, PW1c-6b): the owner built the tree in
    // its own pages and seeded its own anchor, so this core writes the
    // row alone - and `CheckIndexDef`'s owner refusal, which guards
    // exactly that seed, stands down. `kByOwner` *asserts* the owner
    // seeded: passed for a relation this core owns, it leaves the slot
    // empty and readers on the row's root (PW2-3's transitional path -
    // degraded, not wrong). Not checked at runtime.
    enum class AnchorSeed : std::uint8_t { kHere, kByOwner };

    // Writes the sys.indexes row and returns its `index_oid`.
    //
    // Refuses, each naming the reason: a name already in use, an empty or
    // over-cap column list (a cap **refuses**, never truncates - spec §11),
    // a duplicate or out-of-range column position, an index on the primary
    // key (the clustered tree already is one), a heap-clustered relation
    // (spec IX3 - there is no pk descent to resolve an entry through), and
    // `kIndexFlagUnique` (spec IX11).
    //
    // It does **not** build the tree or check the key columns' types: the
    // root page is allocated and formatted by the caller, which is what
    // keeps the catalog free of the index page format.
    // `trx_id` / `where` as on `CreateTable` (DT5): the row is stamped
    // with the creating transaction and its address reported, so a
    // rollback can retire it.
    StatusOr<Oid> CreateIndex(const IndexDef& def, std::uint64_t trx_id = kBootstrapXid,
                               CatalogRowRef* where = nullptr,
                               AnchorSeed seed = AnchorSeed::kHere);

    // Every refusal `CreateIndex` makes, without writing anything.
    //
    // `CreateIndex` is this plus the write, so there is still exactly one
    // implementation of each refusal. It is public because the DDL layer
    // **backfills before it publishes** (docs/spec/index.md §10a) - and a
    // declaration that could never work should be refused by name before it
    // walks a relation, not after, and certainly not as a page-type error
    // from inside the build.
    Status CheckIndexDef(const IndexDef& def, AnchorSeed seed = AnchorSeed::kHere);

    // Retires the row. Retired rather than delete-marked, for DropCabin()'s
    // reason: a catalog read has no snapshot to filter a mark against, so a
    // marked row would still be found by every lookup.
    //
    // The index's **pages are not freed** - nothing frees a page in this
    // engine yet - so a dropped index leaks its tree until page reclamation
    // exists, exactly as a dropped Cabin's memory and a superseded var-heap
    // value do.
    // Transactional when given an id: the row is **delete-marked**
    // rather than retired and the change reported, so a rollback clears
    // the mark. Unlike `DROP TABLE` this is also *isolated* - there is no
    // in-place retype, so a reader that cannot see the dropper still sees
    // the index (ddl-transactional.md §5a).
    Status DropIndex(Oid index_oid, std::uint64_t trx_id = kBootstrapXid,
                      CatalogRowChange* change = nullptr);

    // `view` as on `ListTables` (DT3): `SHOW INDEXES` answers "which
    // indexes exist", which is a schema question and not a diagnostic -
    // so it filters like the other schema routes rather than reporting
    // whatever is on the page.
    StatusOr<std::vector<SysIndexRow>> ListIndexes(const txn::ReadView* view = nullptr);
    StatusOr<std::vector<SysIndexRow>> FindIndexesForTable(Oid table_oid);
    StatusOr<SysIndexRow> FindIndexByName(std::string_view name);

    // An index whose **leading** key column is `col_pos`.
    //
    // Leading, not "contains", and the distinction is load-bearing: an index
    // on `(a, b)` can serve an equality on `a` and cannot serve one on `b`,
    // so answering yes for `b` would stop the compiler calling that step a
    // filter scan while leaving it exactly as slow - a lie to the access
    // statistics, which is the one consumer that exists today.
    //
    // NotFound is the ordinary answer and is never cached
    // (catalog_cache.hpp's absence rule).
    StatusOr<SysIndexRow> FindIndexOnColumn(Oid table_oid, std::uint32_t col_pos);

    // Republishes an index's root after a split grew the tree a level.
    // The counterpart of UpdateRelationDescPage(), and it exists for the
    // same reason: the storage layer has no catalog, so it reports a new
    // root and someone above it records one.
    // `rel_oid` and `anchor_page_id` come from the caller's own
    // TableAccess (the f5686f8 review's S2, extended by 96b0343's C1: the
    // sys.indexes scan that used to live here fetched a catalog page
    // through the write accessor - dirtying it per split on core 0,
    // refusing outright on a peer - for two values MaintainIndexes holds).
    Status UpdateIndexRoot(Oid rel_oid, Oid index_oid, PageId new_root, PageId anchor_page_id);

    const SysObjectRegistry& sys_objects() const noexcept { return sys_objects_; }

    // Monotonic count of catalog mutations that can stale a cached fact.
    // Bumped by DDL - CREATE TABLE, a desc-page relink, a bootstrap row -
    // and deliberately *not* by AllocateRowId(), which changes only
    // `next_id`, a fact nothing caches (catalog_cache.hpp).
    //
    // This is the counter parser.md I5 / parser-workplan.md PR20 stamp
    // parsed statements with. PR20 owns the stamp policy (which version a
    // bound statement records, and what a stale stamp does); this file owns
    // only the counter and its bump points. PR20 should consume this rather
    // than introduce a second counter that can drift from it. It counts
    // mutations, not statements: one CREATE TABLE bumps it once per row it
    // writes, so the contract is "monotonic", never "+1 per DDL".
    std::uint64_t catalog_version() const noexcept { return catalog_version_; }

    const CatalogCache::Stats& cache_stats() const noexcept { return cache_.stats(); }

private:
    // The one sweep both delete-mark retirers share: every mark whose
    // deleter is below `horizon` is retired in place, and the count
    // answered. The mount sweep passes a horizon above every possible id;
    // the in-mount purge passes the manager's ReadHorizon().
    // `remaining_out`, when given, receives how many marks the sweep saw
    // and left (deleter at or above the horizon) - what resettles
    // `pending_marks_` after a purge.
    StatusOr<std::uint64_t> RetireDeleteMarksBelow(std::uint64_t horizon,
                                                   std::uint64_t* remaining_out = nullptr);

    // Phase 5 of Bootstrap(): creates sys.pattern_defs as an ordinary
    // row-codec relation - fixed root page, var-heap chain, four
    // sys.columns rows at fixed oids. Split out because it is the one
    // bootstrap relation that is not just a page and two rows, and folding
    // it into Bootstrap() would bury the reason it differs.
    Status BootstrapPatternDefs();

    // Phase 6 of Bootstrap(): creates sys.assertions, the *second* row-codec
    // catalog relation (docs/spec/assertion.md §8.2, workplan AST03). Same
    // shape as the phase above it and for the same reason - it stores the
    // declaration's text verbatim - so the two read as one pattern rather
    // than as a special case and a copy of it.
    Status BootstrapAssertions();

    // `cabin_policy` is one of the `kCabinPolicy*` values (rows.hpp) and
    // defaults to unset, which every reader treats as `auto`. It is a
    // parameter rather than read off a struct because this function is the
    // one place a sys.columns row is written, and a field it silently
    // dropped would be a column policy an operator declared and never got -
    // which is exactly what happened before it was threaded through here.
    Status InsertColumnRow(Oid oid, Oid rel_id, std::uint32_t pos, std::string_view name,
                            std::uint32_t type_val, std::uint32_t len, bool notnull,
                            std::uint8_t cabin_policy = kCabinPolicyUnset,
                            std::uint64_t trx_id = kBootstrapXid,
                            CatalogRowRef* where = nullptr);
    Status InsertTypeRow(Oid oid, std::string_view name, std::uint32_t type_val,
                          std::uint32_t len);

    // The uncached sys.columns scan behind BuildSchemaFromColumns(). Split
    // out so InitTableAccess() can fill an entry without re-probing the
    // cache slot it is filling.
    StatusOr<Schema> ScanSchemaFromColumns(Oid rel_id);

    // The sys.types snapshot, loaded on first use. Non-owning: the vector
    // lives in the cache.
    StatusOr<const std::vector<SysTypeRow>*> EnsureTypes();

    // The shared in-place rewrite behind SetPatternWaystoneRoot() and
    // SetPatternOrigin(): finds the current-version sys.patterns row for
    // `pattern_id`, hands it to `mutate`, and writes it back over the same
    // slot. One scan and one version filter for both writers, so the two
    // cannot come to disagree about which row is "this pattern".
    //
    // Does not touch the cache - each caller publishes its own field set,
    // because a blanket "re-read the row into the cache" would republish
    // fields the caller did not write and had no lock on.
    Status MutatePatternRow(std::uint64_t pattern_id,
                            const std::function<void(SysPatternRow&)>& mutate);

    // The single invalidation point: bumps the version and drops every
    // DDL-invalidatable cache entry. `what` names the mutation for the
    // Debug line. Call it *after* the page write succeeds - a failed
    // mutation staled nothing.
    void BumpVersion(std::string_view what);

    storage::PageStore& store_;

    // Instance-pinned, never mutated after construction - see the
    // constructor. Every RowLayout in the cache was built for this value.
    std::uint32_t inline_cell_width_ = storage::kDefaultInlineCellWidth;

    // Instance-pinned, never mutated after construction, same as the width.
    // Read only by CreateTable(), to hand catalog::AssignOwnerCore() the
    // core count it takes.
    std::uint32_t core_count_ = 1;
    std::uint32_t core_id_ = kSystemCore;  // see the constructor's comment

    Logger* log_ = nullptr;
    // Armed by the `CommandDispatcher` constructor, so it is null for
    // bootstrap, for recovery, for a test over a bare store - and **for
    // every peer core**, whose `CoreRuntime` builds a Catalog and no
    // dispatcher. That last one is deliberate rather than missed: a peer's
    // live list can never hold the core-0 transaction that wrote a
    // catalog mark, so armed and unarmed answer identically there
    // (`ddl-transactional.md` §5b's core-0 scope, from the other
    // side). Null is the pre-DT9 answer: a mark counts the moment it is
    // written.
    const txn::TransactionManager* txn_ = nullptr;
    // RV3: null means unlogged catalog writes, the pre-RV3 engine.
    wal::WalManager* wal_ = nullptr;
    // RV3-3: per-DDL-statement, dispatcher-installed; empty = no undo.
    DdlUndoHook ddl_undo_hook_;
    // Delete-marks written this run and possibly still on a page - the
    // gate that keeps PurgeSettledDeleteMarks() from Get()-dirtying every
    // catalog page after a DDL that marked nothing (found when D2 made
    // every autocommit DDL reach the purge and the sweep's kWrite fetches
    // turned the whole catalog dirty per statement). Grows on each mark,
    // resettled to the sweep's remaining count when one runs; a rollback
    // clears its marks behind the catalog's back, so the counter may
    // overshoot - costing at most one sweep that finds less than it
    // expected, never a missed mark.
    std::uint64_t pending_marks_ = 0;
    InvalidationHook on_invalidate_;
    RelationPublishHook on_publish_;
    PlacementPolicy placement_ = PlacementPolicy::kCreatingCore;
    RowIdLeaseTable* row_id_leases_ = nullptr;
    // Unset until the first GenerateUserOid() recovers it from the catalog.
    // An optional rather than a sentinel value, because every integer in
    // this type's range is a legal oid and a sentinel would be one more
    // thing that has to stay outside the range it guards.
    std::optional<Oid> next_user_oid_;

    // The highest oid `sys.objects` and `sys.columns` carry, or
    // kUserOidStart - 1 if they carry none above it. Reads the pages; called
    // once per process, by the first GenerateUserOid().
    StatusOr<Oid> HighestIssuedUserOid();
    SysObjectRegistry sys_objects_;
    CatalogCache cache_;
    std::uint64_t catalog_version_ = 0;
};

}  // namespace kds::catalog
