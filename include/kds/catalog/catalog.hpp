#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "kds/base/contention.hpp"
#include "kds/base/function_ref.hpp"
#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog_cache.hpp"
#include "kds/catalog/instance_words.hpp"

#include "kds/catalog/rows.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/catalog/sys_object_registry.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/storage/tagged_cell.hpp"
#include "kds/txn/read_view.hpp"

namespace kds::txn {
// Forward-declared rather than included: the catalog asks it two questions
// (`IsInFlight`, `NoneInFlightBelow`), and `txn/manager.hpp` drags the WAL
// manager and the
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
//     supplies a transaction id** (`ddl-transactional.md`, DT2): a DDL
//     under a transaction passes its id and catalog reads filter by
//     visibility (DT3); kBootstrapXid, visible to every read view
//     permanently, is what bootstrap rows and every default caller get.
//   - Object oids (GenerateUserOid()) are unique for the life of the
//     database. The counter is recovered from the catalog's own rows on
//     first use rather than persisted; see that function and
//     kUserOidStart in well_known.hpp
//     (docs/rules/keystoneid-k0-findings.md §6).
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
// cached fact goes through BumpVersion(), the single invalidation point -
// save the mount's `RenewPatternDirectories`, which drops this catalog's
// cache instead, before the listener binds. The opted patterns are the one
// cached absence, complete per generation: `catalog_cache.hpp` states the
// rule that keeps them so, and `opted_patterns`' comment the atomic
// protocol of the instance's opted count that gates them (CF-S1c).

namespace kds::catalog {

// **Can this caller-named key be stored at all?** Within [kFirstRowId,
// kMaxKeystoneId], since 0 means "unset" and the Keystone field is kKeystoneIdBits
// wide; `OutOfRange` otherwise - BD-R5's "exhausted", one of the two reasons
// a named key is refused (`instructions/v3.0.0/workorder-bd-sorted-leaf-
// named-keys.md`). The insert path judges its literal before it reaches
// here; the admission asks again for any other caller.
Status CheckNamedRowIdSpellable(std::uint64_t id);

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
    // The core-count and core-id parameters retired with ownership (AT-S9);
    // their history is at cf889b63.
    explicit Catalog(storage::PageStore& store,
                     std::uint32_t inline_cell_width = storage::kDefaultInlineCellWidth) noexcept
        : store_(store), inline_cell_width_(inline_cell_width) {}

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
        std::uint64_t tuple_key = 0;
    };
    using DdlUndoHook = std::function<Status(const DdlUndoEvent&)>;

    // Installed by the dispatcher for the duration of one DDL statement
    // that runs under a transaction, cleared right after; empty means no
    // undo record is wanted (bootstrap, a manager-less dispatcher). The
    // hook deliberately does NOT fire for the non-DDL catalog writes -
    // the row-id carve and bump, heat - which are never rolled back by
    // design (an abort burns ids, K3).
    void SetDdlUndoHook(DdlUndoHook hook) { ddl_undo_hook_ = std::move(hook); }

    // **The declaration loader** (BV-S2, AQ-S1): fills a user relation's
    // `TableAccess::transitions` from sys.transitions and each
    // `TableAccess::expr_cabins` entry's `F` from sys.cabin_exprs. A seam
    // rather than a call because the rows are row-codec and their texts
    // re-parsed, which is `exec/`'s, and `exec/` depends on this layer.
    // Armed by the `CommandDispatcher` constructor - every serving catalog
    // has one, and every write runs through a dispatcher. Unarmed
    // (bootstrap, recovery, a bare-store test) a fill leaves the Transitions
    // empty and every expression Cabin's `F` null, which nothing serves or
    // witnesses.
    using DeclarationLoader = std::function<Status(TableAccess&)>;
    // Drops the memo: an entry filled before the loader was armed would
    // serve its relation without its Transitions.
    void SetDeclarationLoader(DeclarationLoader loader) {
        declaration_loader_ = std::move(loader);
        cache_.Invalidate();
    }

    // The schema word moved for a declaration row whose readers cache it -
    // `CREATE`/`DROP TRANSITION`, which write through `exec/`. After the
    // row's write, before the DDL's relation `X` is released (D21's order).
    // A named door onto `BumpVersion`, which stays private: every other
    // caller is a catalog write of this class's own.
    void NoteDeclarationsChanged(std::string_view what) { BumpVersion(what); }

    // **The instance's four words** (`catalog.md` CT6; `instance_words.hpp`
    // carries each word's atomic protocol): one struct, owned by `Expeditor`
    // and handed to every core's catalog by this one setter since CF-S1c.
    // Null is a catalog with no instance around it (a fixture, bootstrap,
    // the sim), which keeps a local copy of each word: its own oid counter,
    // its own mark and opted counts, and no schema word, so it revalidates
    // nothing. What each word is to this class:
    //
    //   - **The schema version word** (AT-S2; AR0-5 §2.1, D21).
    //     `BumpVersion()` - the single DDL choke point - advances it, and
    //     `Revalidate()` below drops this cache when it has moved: a peer is
    //     not told, it asks. **Bumped by every invalidation that can stale
    //     another core's memo** (AT-R2): this core's `catalog_version_` is
    //     not - a peer's drop never advanced it - which is why the word is a
    //     second thing and not that counter shared (`catalog.md` CT1 owns
    //     that rule). `AllocateRowId()` does not bump it: an id is not a
    //     cached fact. `SetPatternWaystone()` does, after its hold is
    //     released: an opt-in is the fact a peer's pattern entry caches.
    //     **Correctness never rests on the word** (AT-R3): it saves a
    //     re-parse. The relation `IS` a statement holds from its bind is
    //     what a DDL's `X` waits for; `read_borrow.hpp` states the defence
    //     and its direction.
    //   - **The object-oid sequence** (AT-S5b). A counter per catalog would
    //     let two catalogs issue the same oid, since every core runs
    //     `CREATE TABLE` (AT-S5): whichever catalog first needs an oid seeds
    //     it by CAS from `HighestIssuedUserOid()`, and every issue after
    //     that is a `fetch_add` (`GenerateUserOid`).
    //   - **The delete-mark count** (AT-S5b). It gates the purge; why the
    //     gate has to be the instance's is at `PurgeSettledDeleteMarks`
    //     below, where it is read.
    //   - **The opted count** (CF-S1c), at `opted_patterns` below.
    void SetInstanceWords(InstanceWords* words) noexcept { words_ = words; }

    // **Asked at a task boundary and nowhere inside one**: a drop frees
    // every `const TableAccess*` a running statement holds. `catalog.md`
    // CT2-CT4 carry the rule, the nine sites, what an unasking task serves
    // and why a drop of the memo is enough. `after_read` runs once the word
    // is read: a test seam (BF-R12), empty in production.
    void Revalidate(const std::function<void()>& after_read = {});
    // The schema word this core's cache was last revalidated against, and
    // the instance's word now (BF-R9's two readings); 0 with no word.
    std::uint64_t cache_built_at() const noexcept { return cache_built_at_; }

    // **The memo held for a walk that outlives its statement's head**
    // (BA-S15, `CatalogCache::Pin`): while one is alive, nothing that frees
    // the memo - a later head's `Revalidate`, a DDL's version bump,
    // `DropCache` - frees an entry the walk bound.
    class MemoPin {
    public:
        explicit MemoPin(Catalog& catalog)
            : catalog_(catalog), generation_(catalog.cache_.Pin()) {}
        ~MemoPin() { catalog_.cache_.Unpin(generation_); }
        MemoPin(const MemoPin&) = delete;
        MemoPin& operator=(const MemoPin&) = delete;

    private:
        Catalog& catalog_;
        std::uint64_t generation_;
    };
    std::uint64_t schema_word_now() const noexcept {
        return words_ == nullptr ? 0 : words_->schema_word.load(std::memory_order_acquire);
    }

    // Whether the memo `Revalidate` last settled is still the word's: false
    // once another core's catalog write has moved it since. Read, never
    // acted on - a drop is `Revalidate`'s, at a boundary. What asks it is a
    // writer's **first** relation `IX` (`CommandDispatcher::BorrowChain`,
    // AT-S5e): the grant fences every later DDL on the relation, so a word
    // unmoved at that moment proves the statement resolved the relation's
    // indexes and assertions as they now stand.
    bool MemoIsCurrent() const noexcept {
        return words_ == nullptr ||
               words_->schema_word.load(std::memory_order_acquire) == cache_built_at_;
    }

    // Drops every cached fact without bumping anything. Its callers are
    // the mount's post-redo drops - `Expeditor`'s and the sim harness's -
    // after redo has mutated catalog pages under a catalog built above it
    // (RV3 D3a). Every other drop is the word's, at `Revalidate()`.
    void DropCache();

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
    // Drops cached content **and bumps the version** - and the word -
    // unlike `DropCache`: this *is* an event in this instance's
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
    // Sound at mount because recovery has already rolled every loser back:
    // every mark left belongs to a committed deleter, and a mark that
    // outlived its mount has no live deleter for DT9 to ask about.
    //
    // Each retire is logged (`SLOT_RETIRE` at `kNoTxnId`, RV3), so a
    // crash mid-sweep leaves a prefix retired and the next mount sweeps the
    // rest.
    StatusOr<std::uint64_t> FinalizeDeleteMarksAtMount();

    // The in-mount sibling of the sweep above: retires every delete-marked
    // catalog row whose deleter every live and future reader on any core
    // sees as committed (`TransactionManager::ResolvedForEveryReader`, the
    // instance's floor and horizon), and answers how many.
    // Callable while the listener is bound; no version
    // bump, each retire logged as at mount; a held-back mark survives
    // to the next call or to the mount sweep. The caller is core 0's
    // `system` tick (`CommandDispatcher::SweepSettledDeleteMarks`, BP-H11),
    // on **one** core - a placement, so that two cores do not walk the same
    // chains at once, not an authority (AT-S5). Which is why the gate it reads is the
    // instance's (`SetInstanceWords`, AT-S5b): the sweeping core has to see
    // a mark another core wrote, or a peer's `DROP` would wait for the
    // next mount. The soundness proof lives once, in
    // `ddl-transactional.md` §5d.
    StatusOr<std::uint64_t> PurgeSettledDeleteMarks();
    // Marks written this run and not yet retired - the instance's count
    // where one is wired (`SetInstanceWords`), the local one for a bare
    // catalog. Public for the purge trigger's first test (BP-H11).
    std::uint64_t MarkCount() const noexcept {
        return words_ != nullptr ? words_->mark_count.load(std::memory_order_acquire)
                                 : pending_marks_;
    }

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
    // per-relation `next_id` sequence - so the contract holds, and the
    // scan is where to extend it if that changes.
    //
    // Fallible: reading the catalog can fail, and an oid guessed after a
    // failed read is exactly the collision this exists to prevent.
    StatusOr<Oid> GenerateUserOid();

    // Creates a new table: allocates its storage root page, formats it for
    // the requested clustered type, and inserts the corresponding
    // sys.objects/sys.tables/sys.columns rows.
    //
    // No key-mode parameter: every relation takes a supplied tuple key or issues
    // one per row (heap-and-tuple.md §4.1; the mode went 2026-08-25).
    // `trx_id` stamps the `sys.objects` / `sys.tables` / `sys.columns`
    // rows this writes (DT2). `kBootstrapXid` is the default and the
    // correct value for bootstrap, tests and recovery: those rows must be
    // visible to a read view minted before any transaction existed
    // (ddl-transactional.md §3). A DDL under a transaction passes its id.
    // `written`, when given, receives every catalog row this created, in
    // write order, so a transactional caller can register them for
    // rollback (DT3a). Filled even when the create fails partway: rows
    // already on the page are rows a rollback must still retire.
    StatusOr<Oid> CreateTable(Oid namespace_oid, std::string_view name, const Schema& schema,
                               std::uint64_t trx_id = kBootstrapXid,
                               std::vector<CatalogRowRef>* written = nullptr);

    // ---- Namespaces (AF-T1) ---------------------------------------------
    //
    // A namespace is a **logical grouping** that declares the affinity of
    // its relations - a hint nothing reads since placement retired
    // (AT-S9). It partitions
    // nothing and bounds no transaction
    // (`instructions/v2.8.0/ratification-af-namespace.md` AF-2).
    //
    // **It is an ordinary `sys.objects` row of `kTypeNamespace`**, with a
    // `GenerateUserOid()` oid and - following the bootstrap convention at
    // `InitWellKnownObjects` - its own oid as its `namespace_oid`, because
    // a namespace's namespace is itself. No new catalog relation, no new
    // page, no format change: `SysObjectRow` has carried `namespace_oid`
    // and `kTypeNamespace = 17` since the format existed.
    //
    // The two that exist before any DDL - `kNamespaceSys`,
    // `kNamespacePublic` - live in the in-memory registry rather than on a
    // catalog page, so every lookup here answers from both.

    // Creates one, name-unique across the registry and the page. Refuses
    // `AlreadyExists` for a taken name and `InvalidArgument` for the two
    // reserved spellings (`well_known.hpp`'s `kNamespaceSysName` /
    // `kNamespacePublicName`) and for a name that would not survive
    // `SetName` intact.
    //
    // `trx_id` / `written` are `CreateTable`'s, for the same reason and
    // with the same defaults: one row, registered for rollback when a
    // transactional caller asks.
    StatusOr<Oid> CreateNamespace(std::string_view name,
                                   std::uint64_t trx_id = kBootstrapXid,
                                   std::vector<CatalogRowRef>* written = nullptr);

    // Resolves a namespace's SQL spelling. Answers the two reserved names
    // first, then the `kTypeNamespace` rows on `sys.objects`; `kNotFound`
    // otherwise. A dropped namespace does not resolve - the retype to
    // `kTypeDroppedNamespace` frees the name the instant the drop runs,
    // which is `DROP TABLE`'s own rule (ddl-transactional.md §5a).
    //
    // **`view` is the reader's visibility, and this is a resolution route**
    // (`ddl-transactional.md` §5's rule 1): a namespace is a schema object,
    // so a session that cannot see the transaction which created one must
    // not be able to create a relation in it. `FindTableOidByName`'s
    // contract, one object kind over. Null (the default) is every internal
    // caller and every pre-AF-T3 behaviour; `CreateNamespace`'s own
    // collision check passes null **on purpose** and says why at the call.
    StatusOr<Oid> FindNamespaceOidByName(std::string_view name,
                                          const txn::ReadView* view = nullptr);

    // Every live user namespace, registry entries excluded - the callers
    // that want those have `sys_objects()`. `view` as above: `SHOW
    // NAMESPACES` is a schema-object surface, so it filters exactly as
    // `SHOW TABLES` does.
    StatusOr<std::vector<SysObjectRow>> ListNamespaces(const txn::ReadView* view = nullptr);

    // **Resolves a relation name as a statement wrote it** (namespace.md
    // NS5): `qualifier.name`, or `name` alone, which means
    // `default_namespace.name` - the session's namespace (`USE`, NS8), and
    // `public` for a caller with no session. One namespace is asked, never
    // a search: a relation in another namespace is reached only by its
    // qualifier.
    //
    // `sys` is refused whichever way it is reached: its relations are read
    // as catalog views (`SELECT ... FROM sys.tables`), which never come
    // here, and have no column rows a statement could bind - the
    // `no columns for this rel_id` an unqualified `tables` used to reach.
    //
    // A miss is `kNotFound`, `no relation named '<as written>'` at
    // `byte_offset`, and a name that does live in other namespaces gets them
    // named, because writing the qualifier is the user's next act.
    // `byte_offset` 0 means nowhere: a statement's first token is its verb,
    // so no relation name sits at byte 0, and the debug surfaces that take a
    // bare argument have no offset to give.
    //
    // `view` is `FindTableOidByName`'s: a resolution route (DT3), for the
    // namespace and the relation alike.
    StatusOr<Oid> ResolveRelation(std::string_view qualifier, std::string_view name,
                                  Oid default_namespace, std::uint32_t byte_offset = 0,
                                  const txn::ReadView* view = nullptr);

    // The SQL spelling of a namespace oid for a message: `sys`, `public`, a
    // live namespace's name, or `oid N` for one no longer live.
    std::string NamespaceSpelling(Oid namespace_oid, const txn::ReadView* view = nullptr);

    // Drops one. **RESTRICT**, the rule `DROP TABLE` already uses for
    // foreign keys and assertions: a namespace holding any relation is
    // refused rather than emptied, because the alternative is a cascade
    // nobody asked for over relations whose data is not the namespace's.
    //
    // Retypes to `kTypeDroppedNamespace` and never retires the row
    // (well_known.hpp's note): the row is `GenerateUserOid()`'s floor
    // evidence, and a reissued namespace oid could make a live relation
    // read as a member of a namespace that no longer exists.
    //
    // The two reserved namespaces are refused `InvalidArgument`.
    Status DropNamespace(Oid namespace_oid, std::uint64_t trx_id = kBootstrapXid,
                          std::vector<CatalogRowChange>* written = nullptr);

    StatusOr<SysTableRow> GetSysTableRow(Oid table_oid);

    // Scans sys.objects (disk, not the in-memory well-known registry -
    // CreateTable() only writes the disk row) for the relation named `name`
    // in namespace `namespace_oid` - the pair is the relation's name
    // (namespace.md NS6). Statements resolve through `ResolveRelation`;
    // this is the lookup under it, for callers that already hold the
    // namespace.
    //
    // `view` is the reader's visibility (workplan-ddl-transactional.md
    // DT3). With one, a relation whose creating transaction this view
    // cannot see **does not exist** for this caller, and the lookup
    // neither reads nor fills the shared cache - see the definition for
    // why that bypass is the whole of the isolation. Null (the default)
    // reads unfiltered, through the cache.
    //
    // **This is the gate.** SQL reaches a relation by name and never by
    // oid, so a name that does not resolve is a relation that cannot be
    // touched - which is why DT3 filters here and leaves `InitTableAccess`
    // on the cache.
    StatusOr<Oid> FindTableOidByName(Oid namespace_oid, std::string_view name,
                                      const txn::ReadView* view = nullptr);

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
    // `own_trx_id` is the renaming session's open transaction, or
    // `txn::kNoTrxId`: it only classifies a refusal (`CheckNameFree`).
    Status RenameTable(Oid table_oid, std::string_view new_name,
                       std::uint64_t own_trx_id = txn::kNoTrxId);
    Status RenameColumn(Oid table_oid, std::string_view old_name, std::string_view new_name);

    // `ALTER TABLE ... DROP COLUMN` (BW-R6, BW-R7 of
    // `instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`): the
    // live column at `pos` gets `kColumnDropped` on its `sys.columns` row and
    // nothing else moves - its `pos`, its bytes in every tuple, its bit. The
    // overwrite is `trx_id`'s, its prior image handed to the DDL undo hook
    // and to `changed` (when non-null) so a rollback restores it, as
    // `DropTable`'s retype is. The lookup and the write are one hold of
    // `sys.columns`' root page (CT7). `pos` is the column the caller
    // resolved `name` to (`Schema::FindColumn`) and asked its RESTRICT of;
    // the door never resolves the name again, so a RENAME COLUMN racing
    // the drop cannot move the drop onto another column - a position no
    // longer carrying `name` (ignoring case) is `TxnConflict`. The tuple key
    // is refused `InvalidArgument`; an absent or already-dropped position is
    // `NotFound`. What depends on the column is the caller's to refuse
    // first, under the relation `X`.
    Status DropColumn(Oid table_oid, std::uint16_t pos, std::string_view name,
                      std::uint64_t trx_id, std::vector<CatalogRowChange>* changed);

    // `PURGE DROPPED COLUMNS` (CD-S3): `kColumnSlotClean` ORed into the
    // `sys.columns` row of each position in `positions` that is still a
    // dropped column of `table_oid` and not yet flagged, each one in-place
    // `OverwriteForDdl` under `trx_id`, under one hold of `sys.columns`'
    // root page (CT7) - so a rollback, here or at mount, clears it again.
    // Returns how many rows it flagged.
    StatusOr<std::size_t> MarkColumnsSlotClean(Oid table_oid,
                                               std::span<const std::uint16_t> positions,
                                               std::uint64_t trx_id,
                                               std::vector<CatalogRowChange>* changed);

    // The position an `ALTER TABLE ... ADD COLUMN` on `table_oid` takes now:
    // the next position, in the lowest vacant slot, while one is free (BW-R4);
    // else the lowest-position dropped slot column flagged clean, at its own
    // position and so in its own slot (CD-S4, CD-OI-12 (a)). Neither is
    // `NotImplemented`, naming the remedy when dropped slots exist. Stable
    // under the relation `X`, which every ADD and DROP takes and the clean's
    // `IX` waits for.
    StatusOr<std::uint16_t> AddColumnPosition(Oid table_oid);

    // `ALTER TABLE ... ADD COLUMN` (BW-R4, BW-R5): `column` - its name, type,
    // `len` and nothing else read - becomes a nullable column at `pos`, which
    // the caller took from `AddColumnPosition` and asked its dependants of;
    // a position that is no longer the one is `TxnConflict`. A vacant slot's
    // row is inserted under `trx_id` and reported through `inserted`; a clean
    // dropped slot's row is rewritten in place (CD-OI-4 (a)) - its oid, and so
    // its Keystone word, kept, `generation` + 1 - and reported through
    // `reused`, so a rollback restores the dropped row. The name check, the
    // position and the write are one hold of `sys.columns`' root page (CT7).
    // A taken live name is `AlreadyExists`; a cell wider than a slot is
    // `NotImplemented` (D-c, BW-R4).
    Status AddColumn(Oid table_oid, const SysColumnRow& column, std::uint16_t pos,
                     std::uint64_t trx_id, CatalogRowRef* inserted,
                     std::vector<CatalogRowChange>* reused);

    // DROP TABLE's catalog half (docs/spec/drop-table.md DT1-DT3, workplan
    // DT02). The sys.objects row is **retyped to kTypeDroppedTable, never
    // retired** - the rows are GenerateUserOid()'s counter, so the row
    // must stay to keep the dead oid from being reissued, while name
    // resolution's kTypeTable filter frees the name at once. Everything
    // the relation owns retires: its sys.tables row, every sys.columns /
    // sys.indexes / sys.cabins row and its child-side sys.fkeys rows.
    // **Pages are not touched here** (BF-R1): the retype records the
    // relation's anchor and var-heap root in the tombstone's
    // `pending_roots` (BF-R2), and the reclaim walks them once the drop's
    // commit is past every replay (BF-R4) - never inside the drop.
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
    //
    // `owed`, when given, receives the roots the tombstone now carries
    // (BF-R2) - what a reclaim within the run is queued with (BF-R9).
    Status DropTable(Oid table_oid, std::vector<std::uint64_t>& dropped_cabins,
                      std::uint64_t trx_id = kBootstrapXid,
                      std::vector<CatalogRowChange>* written = nullptr,
                      PendingRoots* owed = nullptr);

    // ---- Pending reclaims (BF-R2) ------------------------------------
    //
    // Every tombstone whose `pending_roots` is nonzero: a dropped relation
    // whose pages a reclaim still owes. One scan of `sys.objects`. Read at
    // mount, after recovery, where every drop is decided - a loser's retype
    // was undone with its word - so no stamp is consulted.
    struct PendingReclaim {
        Oid oid = 0;
        PendingRoots roots;
    };
    StatusOr<std::vector<PendingReclaim>> PendingReclaims();

    // ---- Owed index trees (BY-R1,
    // `instructions/v3.2.0/workorder-by-reindex-drop-index-reclaim.md`) ----
    //
    // Every anchor slot of a live user relation whose index oid names no
    // live `sys.indexes` row: a dropped index's tree, or a rolled-back or
    // failed create's logged one. **Read at mount only, after undo, and
    // unfiltered** `[quiet-wrong]`: a view-filtered read would call an open
    // `CREATE INDEX`'s slot owed and hand a tree being built to the
    // reclaimer. A dropped relation's slots are its tombstone's to reclaim
    // (BY-R3) and are not listed.
    struct OwedIndexTree {
        Oid rel_oid = 0;
        PageId anchor_page_id = kInvalidPageId;
        Oid index_oid = 0;
    };
    StatusOr<std::vector<OwedIndexTree>> OwedIndexTrees();

    // Removes `index_oid`'s slot from `anchor_page_id` under the page's
    // latch, logging ANCHOR_SLOT_REMOVE under the same hold (BY-R5). Called
    // once the slot's tree is reclaimed and every free durable; outside any
    // transaction, as `ClearPendingRoots` is.
    Status RemoveIndexSlot(Oid rel_oid, PageId anchor_page_id, Oid index_oid);

    // Overwrites `oid`'s tombstone word to 0 once its reclaim is durable
    // (BF-R7): logged outside any transaction, as the mark retire is, and
    // keeping the row's `trx_id` and `undo_ptr`, so the decided-tombstone
    // reading of `CheckNameFree` and `CheckNamespaceEmpty` does not move.
    // No schema word moves - no name lookup resolves a tombstone. NotFound
    // when no tombstone carries `oid`.
    Status ClearPendingRoots(Oid oid);

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
    // sequence. This is what an `INSERT` that **omits** the tuple key calls, on
    // every relation - there is no longer a key mode that makes it illegal
    // (section 4.1). Ids are unique by construction and rise above every id
    // the relation has ever placed, since `AdmitExplicitRowId` moves the same
    // mark past every supplied one; they are not gapless, since a failed
    // insert after a successful allocation burns one.
    //
    // Fails with NotFound if no sys.tables row names `table_oid`, and with
    // OutOfRange once the relation has issued its whole id space -
    // reclamation policy is an open decision, so exhaustion is reported
    // rather than wrapped.
    //
    // **Every core calls it, and none caches** (AT-S10b). The bump is an
    // in-place write of the relation's `sys.tables` row under its page
    // latch, so two cores issuing into one relation are serialised by the
    // latch and the ids stay a sequence in issue order - invariant 11 with
    // spreading off.
    StatusOr<std::uint64_t> AllocateRowId(Oid table_oid);

    // What an `INSERT` that **supplies** the tuple key calls (docs/spec/heap-and-tuple.md
    // section 4.1): admits a caller-named id and moves the relation's
    // high-water mark past it. Every relation may be inserted into either
    // way; which of the two functions runs is a property of the row, not of
    // the relation.
    //
    // ---- What this checks -------------------------------------------------
    //
    // **Spellability, always** (`CheckNamedRowIdSpellable`, below).
    //
    // **Then the mark, which never refuses.** A leaf places a key where it
    // sorts (BD-R2, BD-R7,
    // `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`), so the
    // admission is advance-or-nothing: an id at or above the mark moves it to
    // `id + 1`, one below leaves it (W2 of
    // `instructions/v3.2.0/workorder-bz-heap-hidden-key.md`). Called before
    // the descent, under no page (BD-R6).
    //
    // ---- Why the high-water mark moves ------------------------------------
    //
    // `next_id` is a ceiling on what has been *placed*, which is what makes
    // it the uniqueness proof for an id at or above it and what keeps K4's
    // lifetime budget and the exhaustion refusal truthful - DESCRIBE
    // and SHOW BUDGET both read it. Advanced, never lowered, and persisted
    // before the caller places anything: a crash between here and the insert
    // leaves a ceiling that is too high (K3 calls a burned id free) rather
    // than one too low, and a too-low mark is how the engine later issues an
    // id that is already a tuple's identity.
    //
    // Fails with NotFound if no sys.tables row names `table_oid`.
    //
    // **No hook any more.** `before_mark` took the row's lock between the
    // judgement and the mark's move, because a statement that parked and
    // re-ran found its own key below a mark its first attempt advanced. Since
    // BB-R3 the borrow precedes the admission entirely - a refused borrow
    // advanced nothing - so the hook's reason is gone, and with it the
    // partition latch taken under page 7 (BA-R8's last bullet).
    Status AdmitExplicitRowId(Oid table_oid, std::uint64_t id);

    // ---- sys.patterns (docs/spec/waystone-concpets.md section 4) --------------

    // **The instance's opted count** (CF-S1c, §8.3 "The gate and its hot-path
    // cost"): how many current-version `sys.patterns` rows carry
    // `kPatternWaystone`. The fourth of the instance's words
    // (`SetInstanceWords`); a bare catalog keeps a local count.
    //
    // **Moved only after a successful write, under page 9's hold**, and only
    // on a 0->1 or 1->0 change of the bit on a **current-version** row:
    // OFF clearing a lapsed row's bit leaves it, and a decrement saturates at
    // 0. **Set from the chain at every boot**, after redo where redo runs:
    // `RenewPatternDirectories`' walk sets it, and a boot that runs no
    // renewal runs `RecountOptedPatterns`. A stale count costs performance
    // only (invariant 8): it gates a lookup, never what a reader sees. Its
    // memory orders are at the field (`instance_words.hpp`).
    //
    // One load, with no fill: the `SELECT` path's whole cost while nothing
    // on the instance is opted.
    std::uint64_t opted_patterns() const noexcept {
        return words_ != nullptr ? words_->opted_count.load(std::memory_order_acquire)
                                 : local_opted_;
    }
    bool NoOptedPatterns() const noexcept { return opted_patterns() == 0; }

    // The opted pattern `fetch_id` names, or NotFound (CF-S1c). Served from
    // this core's **opted map, complete per cache generation**: while the
    // count is non-zero, the first ask after a cache drop fills it with
    // every current-version row that has `kPatternWaystone` set, in one
    // `ScanAll` of `sys.patterns`, and every later ask in the generation is
    // one hash probe - **a miss is NotFound with no scan**. While the count
    // is zero nothing fills and every ask is NotFound. The soundness rule
    // that lets a miss be answered from memory is `catalog_cache.hpp`'s.
    //
    // The pointer is reference-stable and stays valid until the next
    // Invalidate() - same contract as InitTableAccess(). A row of another
    // fingerprint revision is never in the map: it names a shape that is
    // not the one it claims, so the only safe reading of it is that the
    // pattern was never opted in (a re-issued `WAYSTONE ON` inserts a
    // current row beside it). Any answer but NotFound is a read that
    // failed.
    StatusOr<const PatternAccess*> FindOptedPattern(std::uint64_t fetch_id);

    // What `SetPatternWaystone` did, and the LSN its answer is durable at.
    struct PatternWaystoneWrite {
        // False is `unchanged`: nothing was written, because the state asked
        // for already stood.
        bool written = false;
        // The record this answer rests on, for the caller to park on
        // (`IsDurable(durable_at)`), or 0 when there is no log. Read after the
        // write when one was made - the append point less one, which bounds
        // this write's record and perhaps later ones, a later bound and never
        // an earlier one - and **under page 9's hold** when none was, which
        // bounds the record of whichever earlier statement set the state this
        // answer reports: every writer of the bit writes under that hold.
        std::uint64_t durable_at = 0;
    };

    // **`WAYSTONE ON` and `WAYSTONE OFF`'s one writer** (CF-S1b,
    // `instructions/v3.2.0/workorder-cf-waystone-premise.md` §8.3): bit 0 of
    // a pattern row's `flags` (`kPatternWaystone`), the opt-in that decides
    // whether `fetch_id`'s shape records and replays at all.
    //
    // **Page 9 held exclusive across the decision and the write**, as
    // `RecordAccess` holds its own root: the existence check and the insert
    // under it are one act across cores, so two concurrent ONs serialize and
    // no shape owns two current rows. Released before the schema word moves
    // (`BumpVersion`, after a write only), and before any wait - the caller
    // parks on `durable_at` with no latch held.
    //
    // `on`:
    //   - no current-version row: a new one, `flags = kPatternWaystone`,
    //     `stmt_class` as given, rooted at `root` (depth 1) or at none (depth
    //     0) when `root` is `kInvalidPageId`. Logged at `kBootstrapXid`.
    //   - a current-version row: the bit set, and `root` written if the row
    //     has no directory - one logged overwrite at `kNoTxnId`. Unchanged
    //     when the bit stood and there is no root to write.
    //   A row of another fingerprint version is not this pattern's, so it
    //   neither counts as the row nor is written (a re-issued ON after a
    //   bump inserts beside it). A new row's `oid` comes from
    //   AllocateRowId(kSysPatternsTable) - the persistent sequence in
    //   sys.patterns' own sys.tables row, used as an oid source: catalog rows
    //   carry no Keystone word, and GenerateUserOid()'s recovery reads back
    //   only `sys.objects` and `sys.columns`, so an oid written only here
    //   would be reissued after a restart.
    //
    // `!on` (`root` must be `kInvalidPageId`): **every** row carrying
    // `fetch_id`, of any version, has its bit and its directory cleared, in
    // one whole-chain walk - a `fetch_id` is not salted by
    // `kFingerprintVersion`, so after a bump a lapsed opted row and the
    // current one commonly share it, and a first-match OFF would clear the
    // lapsed row and never reach the current one. Unchanged when no row it
    // met had the bit. The rows stay, and the directories become
    // unreachable, as at a mount.
    //
    // **The opted count moves here and nowhere else at run time** (CF-S1c):
    // under the hold, after each write that succeeded, by one per current-
    // version row whose bit it changed - never for a lapsed row, never for a
    // root alone, never for a write that failed.
    //
    // **`root` is created by the caller before this is called, never under
    // the hold**: `CreateDirPage` can block on the data-sync thread
    // (`device_page_store.cpp`'s headerless create), and a page latch is
    // never held across a durability wait. A page passed in and not used - a
    // concurrent ON rooted the row first, the row was found opted with a
    // root, the write failed - is logged as leaked, which is what every
    // unreferenced page here is.
    StatusOr<PatternWaystoneWrite> SetPatternWaystone(std::uint64_t fetch_id,
                                                      std::uint8_t stmt_class, bool on,
                                                      PageId root);

    // **The mount's renewal of every opted pattern's directory** (CF-S1b,
    // §8.3 Mount): the discard of unlogged Waystone state
    // (`DiscardAdvisoryState`), core 0's, after recovery and before the
    // listener binds. Answers how many current-version rows are opted.
    //
    // Two passes, **so no page is created under a catalog page latch**:
    // a read-only walk collects the current-version opted rows; then, with
    // no latch held and only where `waystone_recording` is on, one fresh
    // directory each; then one `kWrite` walk writes each collected row's
    // fresh root at depth 1 and clears every other root, **whatever its
    // fingerprint version**, so a lapsed row keeps no stale root. With
    // recording off every root is cleared and none is created. A creation
    // that fails clears the row's root, keeps its bit, is logged, and the
    // mount still opens. A row that changed between the passes keeps its
    // write and its unused page is logged as leaked.
    //
    // Opted rows of another fingerprint version are logged as lapsed, only
    // where no current-version row of their `fetch_id` is opted (a re-issued
    // ON is not reported again at every mount).
    //
    // **Ends by setting the opted count** from the second walk (CF-S1c) **and
    // dropping this catalog's cache** `[quiet-wrong]`: a pattern entry filled
    // before the walk would keep the previous run's root, whose children can
    // be ids the cursor has since handed to logged pages, so a trail written
    // through it would overwrite them with no record.
    StatusOr<std::uint64_t> RenewPatternDirectories(bool waystone_recording);

    // **The counting walk of a boot that runs no renewal** (CF-S1c): the
    // sim's `skip_recovery` boot skips `DiscardAdvisoryState`, and with it
    // the walk that sets the opted count, which would otherwise stay 0 over
    // opted rows for the whole run. One read-only walk of the chain, which
    // on a v33 volume holds only the rows ever opted; sets the count and
    // answers it.
    StatusOr<std::uint64_t> RecountOptedPatterns();

    // A `TouchPattern()` stood here - the trail recorder's catalog half,
    // which bumped `use_count` and stamped `last_seen` on every recorded
    // execution. It went with BL-S4's ruling 1 (2026-10-12, rows.hpp): it
    // was a logged catalog write on the read path, so a repeated point
    // `SELECT` waited on the one WAL behind any bulk writer. The two fields
    // stay on disk, written 0 at `WAYSTONE ON`'s insert and advanced by
    // nothing.

    // Every sys.patterns row, in page order.
    //
    // **Unfiltered, unlike GetSysPatternRow()** - rows from another
    // fingerprint revision are included. That is not a hole in the version
    // rule: the rule protects *lookup by fetch_id*, so a stale row can
    // never resolve as the pattern it names. This is an inspection surface,
    // and the stale rows are exactly what an operator wants to see - they
    // are the garbage a version bump left behind (retention is unbuilt).
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
    // PatternAccess deliberately omits - `use_count` and `last_seen`, heat
    // that moves without DDL and so is not a cacheable fact (schema.hpp;
    // reserved since BL-S4's ruling 1, rows.hpp). A caller that wants heat
    // calls this; a caller that wants an opted pattern's identity or location
    // calls FindOptedPattern().
    //
    // **Rows from another fingerprint revision are invisible here**, and
    // this is the single place that filter lives. A version bump leaves
    // the old rows on the page - nothing rewrites them - so a lookup that
    // did not filter would let a stale row shadow the current one. Putting
    // the filter in the row lookup rather than in each caller is what
    // makes that impossible rather than merely unlikely.
    StatusOr<SysPatternRow> GetSysPatternRow(std::uint64_t fetch_id);

    // A root move - the clustered tree's when `index_oid` is 0, otherwise
    // that index's - written to the relation's anchor alone, never a
    // catalog row (PW2-3/PW2-4), with the anchor id from the caller's own
    // TableAccess (no catalog scan inside a level growth).
    //
    // A root a split grew is published in two halves, so the anchor rides
    // in the split's own record (`storage::LogBtreeSplit`): `StageAnchorRoot`
    // before the record, `AdoptRoot` after it. `PublishRoot` is both with
    // its own ANCHOR_UPDATE, for a growth no split record carries - one
    // made where nothing logs index writes as they happen.
    Status PublishRoot(Oid rel_oid, std::uint64_t index_oid, PageId new_root,
                       PageId anchor_page_id);

    // The in-memory half of a root move whose anchor slot is already
    // written and logged: this core's cached entry in place, and the schema
    // word bumped so every other core re-reads the anchor.
    void AdoptRoot(Oid rel_oid, std::uint64_t index_oid, PageId new_root);

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
                        std::uint64_t last_seen,
                        std::uint64_t count = 1);

    // Every access shape, in page order. The inspection surface behind
    // `SHOW ACCESS`.
    // Empties `sys.access_stats` unconditionally: every page of its chain
    // re-formatted in place and kept, the chain cut before the first link
    // that does not reach one of its pages (CR6's discard). Every mount's
    // (BR-S8, `server::DiscardAdvisoryState`).
    //
    // The relation is unlogged, so nothing redoes or undoes it and no repair
    // exists; damage would otherwise surface as a failing statistic on every
    // statement and a failing `SHOW ACCESS`, for the life of the file.
    // Discarding is sound on invariant 8's terms - a deleted trail costs
    // performance and never a result - and the pages past a cut are leaked:
    // no reclaim walk reaches a catalog relation's discarded chain
    // (`drop-table.md` DT1's stated leaks).
    Status DiscardAccessStats();

    StatusOr<std::vector<SysAccessStatRow>> ListAccessStats();

    // Retires every `sys.access_stats` row of `rel_id` whose column mask
    // names `col_pos` (CD-OI-13 (a)): a reused position's history is the
    // dropped column's, not the new one's. Unlogged, as every write here is.
    Status ForgetAccessStats(Oid rel_id, std::uint16_t col_pos);

    // ---- sys.cabins (docs/spec/cabin.md §10) -----------------------------

    // Declares a Cabin on one non-tuple-key column and returns its `cabin_id`.
    //
    // What this writes is that the Cabin **exists**. Nothing is observed by
    // creating one: the read path's miss branch is what fills it (spec §4),
    // so a fresh Cabin costs a catalog row and changes no answer until
    // traffic teaches it something.
    //
    // Fails with InvalidArgument for column 0 - the tuple key's Cabin is the
    // clustered tree itself (§2), which is not a refusal a caller can talk
    // its way out of - and for a `col_pos` past the relation's schema;
    // NotFound for an unknown relation; AlreadyExists for a second Cabin on
    // the same column.
    //
    // **Bumps the catalog version**: the mask on
    // TableAccess is derived from these rows, so a Cabin appearing stales
    // every cached relation entry. That makes this a DDL-shaped call and it
    // must not be made from a statement path holding a `const TableAccess*`.
    //
    // `trx_id` and `where` as CreateIndex() takes them: a `CABIN` column
    // clause writes its row under the `CREATE TABLE`'s transaction and
    // reports it for that transaction's trail, so a rollback retires the
    // Cabin with the relation. `CREATE CABIN` and the controller write
    // committed (`kBootstrapXid`) on a relation already committed.
    StatusOr<std::uint64_t> CreateCabin(Oid rel_oid, std::uint16_t col_pos,
                                        std::uint8_t origin = kCabinOriginUser,
                                        std::uint64_t trx_id = kBootstrapXid,
                                        CatalogRowRef* where = nullptr);

    // Removes a Cabin's row. **Retires the slot rather than delete-marking
    // it**, and the .cpp states the argument in full: catalog reads have no
    // snapshot to filter a mark against, so a marked row would still be
    // found by every lookup here and a re-created Cabin would collide with
    // a row nobody can see. A retire is not rollback-able, which is why the
    // transactional removers delete-mark instead (DT5).
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
    // why the *compiler* asks `TableAccess::cabin_columns` instead of calling
    // this per step (catalog_cache.hpp's absence rule).
    StatusOr<SysCabinRow> FindCabinOnColumn(Oid rel_oid, std::uint16_t col_pos);

    // Declares an **expression or predicate** Cabin (AQ-R1, R2) and returns
    // its `cabin_id`. `F` is `exec/`'s, so the two questions only it can
    // answer are asked back: `same_f(row)` - whether an existing Cabin of
    // this relation, shape and `expr_id` has the compiled `F` being declared
    // - and `record_f(cabin_id)`, which writes `F`'s text into
    // sys.cabin_exprs. Both run under sys.cabins' root page held exclusive,
    // so two cores cannot both declare one `F`; `record_f` runs before the
    // row lands. AlreadyExists for a second Cabin on one `F`.
    using SameKeyFunction = std::function<StatusOr<bool>(const SysCabinRow&)>;
    using RecordKeyFunction = std::function<Status(std::uint64_t cabin_id)>;
    StatusOr<std::uint64_t> CreateExprCabin(Oid rel_oid, std::uint8_t shape, std::uint64_t expr_id,
                                            std::uint8_t origin, const SameKeyFunction& same_f,
                                            const RecordKeyFunction& record_f);

private:
    // Both doors' last act: the row, the schema word, the log line.
    Status InsertCabinRow(const SysCabinRow& row, std::uint64_t trx_id, CatalogRowRef* where);

public:

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
    // NotImplemented for a heap parent or a cross-core pair (F5, both named
    // open in foreign-keys.md); AlreadyExists
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
    // There is no owner-core parameter (the row's word is reserved and
    // written 0), and no key-mode parameter: the row's retired
    // key-order byte is written 0 here as on every row (BB-R10), never passed
    // in.
    // `anchor_page_id` defaults to kInvalidPageId, the bootstrap value -
    // rows.hpp owns the rule (a system relation carries no anchor).
    // The one anchor write path: validate the page and its owner stamp
    // (the write is where damage is created), set the slot (index_oid 0 = the clustered
    // root, the record's own discriminator), log and stamp. Three callers
    // - both movers and the CREATE INDEX seed - so the PW2 ordering
    // discipline lives once.
    Status WriteAnchorRoot(PageId anchor_page, Oid expected_owner_oid, std::uint64_t index_oid,
                           PageId root, std::uint64_t trx_id);

    // WriteAnchorRoot's checks and slot write, **unlogged**, the page handed
    // back still held exclusive: for a root a split grew, whose caller
    // images the anchor into the split's one record before the hold drops
    // (`storage::LogBtreeSplit`), so the slot and the root it names are
    // replayed together or not at all. A record that fails to append for a
    // device fault fail-stops the log (wal.md §6), so its staged slot is
    // never written back; one refused for its size does not
    // (`known-gaps.md`, WAL).
    StatusOr<storage::PageRef> StageAnchorRoot(PageId anchor_page, Oid expected_owner_oid,
                                               std::uint64_t index_oid, PageId root);

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

    Status InsertRelationRow(Oid oid, Oid namespace_oid, PageId desc_page_id,
                              ClusteredType clustered_type, PageId varheap_page_id,
                              PageId anchor_page_id = kInvalidPageId,
                              std::uint64_t trx_id = kBootstrapXid,
                              CatalogRowRef* where = nullptr,
                              std::uint16_t spare_slots = 0);

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

    // `AnchorSeed` retired with the owner-built index path (AT-S5e); its
    // history is at cf889b63. The creating core always seeds.

    // Writes the sys.indexes row and returns its `index_oid`.
    //
    // Refuses, each naming the reason: a name already in use, an empty or
    // over-cap column list (a cap **refuses**, never truncates - spec §11),
    // a duplicate or out-of-range column position, an index on the tuple
    // key (the clustered tree already is one), a heap-clustered relation
    // (spec IX3 - there is no tuple key descent to resolve an entry through), and
    // a flag bit other than `kIndexFlagUnique` (spec IX11).
    //
    // It does **not** build the tree or check the key columns' types: the
    // root page is allocated and formatted by the caller, which is what
    // keeps the catalog free of the index page format.
    // `trx_id` / `where` as on `CreateTable` (DT5): the row is stamped
    // with the creating transaction and its address reported, so a
    // rollback can retire it.
    StatusOr<Oid> CreateIndex(const IndexDef& def, std::uint64_t trx_id = kBootstrapXid,
                               CatalogRowRef* where = nullptr);

    // Every refusal `CreateIndex` makes, without writing anything.
    //
    // `CreateIndex` is this plus the write, so there is still exactly one
    // implementation of each refusal. It is public because the DDL layer
    // **backfills before it publishes** (docs/spec/index.md §10.1) - and a
    // declaration that could never work should be refused by name before it
    // walks a relation, not after, and certainly not as a page-type error
    // from inside the build.
    // `own_trx_id` is the declaring transaction's, for the name check's
    // own-drop case (`CheckIndexNameFree`), or `txn::kNoTrxId`.
    Status CheckIndexDef(const IndexDef& def, std::uint64_t own_trx_id = txn::kNoTrxId);

    // Retires the row. Retired rather than delete-marked, for DropCabin()'s
    // reason: a catalog read has no snapshot to filter a mark against, so a
    // marked row would still be found by every lookup.
    //
    // The index's **pages are not freed here** (BF-Q8): its anchor slot
    // stays, so the tree is reclaimed with its relation once that is
    // dropped (BF-R3), and leaks until then.
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
    // `view` as on `ListIndexes`: `DROP INDEX` resolves a name, so it must
    // not find an index another transaction's open `CREATE INDEX` wrote.
    StatusOr<SysIndexRow> FindIndexByName(std::string_view name,
                                          const txn::ReadView* view = nullptr);

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
    std::size_t cache_retired_generations() const noexcept { return cache_.retired_generations(); }

private:
    // One `sys.columns` row of `table_oid` at `pos` that `accept` admits,
    // changed by `mutate` and written back in place by `OverwriteForDdl`
    // under `trx_id`, its change appended to `changed` when a transaction
    // owns it. The caller holds `sys.columns`' root page (CT7). False when
    // no row is admitted.
    StatusOr<bool> RewriteColumnRow(Oid table_oid, std::uint16_t pos,
                                    FunctionRef<bool(const SysColumnRow&)> accept,
                                    FunctionRef<void(SysColumnRow&)> mutate,
                                    std::uint64_t trx_id,
                                    std::vector<CatalogRowChange>* changed);

    // ---- The delete-mark count, and the one seam it is reached through -
    //
    // Two shapes: the instance's `mark_count` when one was wired
    // (`SetInstanceWords`), the local `pending_marks_` for a bare catalog.
    // Nothing outside these two and the public `MarkCount` touches either,
    // which is what keeps the two from diverging. The word's memory orders,
    // and why, are at the field (`instance_words.hpp`).
    void NoteMark() noexcept {
        if (words_ != nullptr) {
            words_->mark_count.fetch_add(1, std::memory_order_acq_rel);
        } else {
            ++pending_marks_;
        }
    }
    // A **delta**, not an assignment, and that is the whole of it: another
    // core may have marked a row while this sweep walked, and its
    // `NoteMark` has to survive. `before` is what the count held when the
    // sweep began and `remaining` what it saw unsettled, so adding the
    // difference - unsigned, so a smaller `remaining` wraps and adds as
    // the subtraction it is - leaves a concurrent mark counted.
    // `catalog.md` CT6 carries which direction of error is allowed and
    // why.
    void ResettleMarks(std::uint64_t before, std::uint64_t remaining) noexcept {
        if (words_ != nullptr) {
            words_->mark_count.fetch_add(remaining - before, std::memory_order_acq_rel);
        } else {
            pending_marks_ = remaining;
        }
    }

    // The one sweep both delete-mark retirers share: every mark whose
    // deleter `settled` answers true for is retired in place, and the count
    // answered. The mount sweep passes "every deleter"; the in-mount purge
    // passes the manager's `ResolvedForEveryReader` - below the instance's
    // floor, or committed at or below every live snapshot (AN-S2), which is
    // the two-branch test the reader's own predicate decides by.
    // `remaining_out`, when given, receives how many marks the sweep saw
    // and left - what `ResettleMarks` takes after a purge.
    StatusOr<std::uint64_t> RetireDeleteMarks(
        const std::function<bool(std::uint64_t deleter)>& settled,
        std::uint64_t* remaining_out = nullptr);

    // Phases 5 and 6 of Bootstrap(): the two row-codec declaration
    // relations, sys.assertions and sys.transitions - one shape (fixed root
    // page, var-heap chain, six sys.columns rows at fixed oids), built by
    // hand rather than through CreateTable() for the reasons Bootstrap()
    // states. Split out because they are the bootstrap relations that are
    // not just a page and two rows.
    Status BootstrapDeclarations(Oid rel_oid, std::string_view name, PageId root_page,
                                 const Schema& schema);

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
                            CatalogRowRef* where = nullptr, std::uint8_t flags = 0);
    Status InsertTypeRow(Oid oid, std::string_view name, std::uint32_t type_val,
                          std::uint32_t len);

    // **Is `name` free for a new `live_type` row** (`kTypeTable` or
    // `kTypeNamespace`) - the check every write that gives a `sys.objects`
    // row a name makes (`catalog.md` CT7). **Binding only under page 6
    // held exclusive** across the check and the caller's insert or rewrite
    // (AT-S17, the AT-close order's Q2), as `SetPatternWaystone`'s is on
    // page 9; asked unheld, it is an early refusal and nothing more.
    //
    // Two questions, in one walk so no caller can ask one:
    //
    //   - **A live row of the name**, unfiltered (`ddl-transactional.md`'s
    //     duplicate check): another transaction's uncommitted row counts,
    //     whichever core wrote it. `AlreadyExists`.
    //   - **A tombstone whose drop has not committed**, on any core: its
    //     rollback rewrites the row live again, beside whatever took the
    //     name meanwhile. `TxnConflict` - free once that transaction
    //     resolves. The drop of `own_trx_id` (the asker's transaction, or
    //     `txn::kNoTrxId`) frees the name when `own_drop_frees` - `DROP
    //     TABLE t; CREATE TABLE t` in one transaction, which the create's
    //     own rollback undoes with it - and otherwise is `Unsupported`: a
    //     rename is not undone by that rollback.
    //
    // A relation's name is taken within its namespace (namespace.md NS6), so
    // for `kTypeTable` only rows in `namespace_oid` are asked - a tombstone
    // keeps the namespace it was dropped from. A namespace's name is
    // instance-wide and `namespace_oid` is not read.
    Status CheckNameFree(std::string_view name, Oid live_type, Oid namespace_oid,
                         std::uint64_t own_trx_id, bool own_drop_frees);

    // **A relation's namespace, asked where its membership is written**
    // (AT-S17b, the AT-close order's §7.3): a relation's `sys.objects` row
    // carries its `namespace_oid`, so page 6 held exclusive is what makes
    // a create's membership and a drop's RESTRICT one act. Both binding
    // only under that hold, `CheckNameFree`'s rule, and both over the same
    // unfiltered walk and instance check view.
    //
    // `CheckNamespaceLive`: the namespace a `CREATE TABLE` names is still
    // there, and not only another transaction's. Its retype by a drop that
    // has committed - or by the asker's own - is `NotFound`; by another
    // transaction's that has not, and its row while another transaction's
    // create of it has not committed, are `TxnConflict`, retryable. A
    // well-known namespace is always live.
    Status CheckNamespaceLive(Oid namespace_oid, std::uint64_t own_trx_id);
    // `CheckNamespaceEmpty`: `DROP NAMESPACE`'s RESTRICT. A relation row in
    // it - live, whoever wrote it and whether or not it has committed - is
    // `InvalidArgument` naming the relation; a relation's tombstone whose
    // drop has not committed counts too, since its rollback puts the
    // relation back: `InvalidArgument` when the drop is the asker's own
    // (commit it first), `TxnConflict` when it is another transaction's.
    Status CheckNamespaceEmpty(Oid namespace_oid, std::uint64_t own_trx_id);

    // `CheckNameFree` for an index's name, over `sys.indexes`: a live row
    // of the name is `AlreadyExists`, and one delete-marked by a drop the
    // instance's check view cannot see yet is `TxnConflict` - unless the
    // drop is `own_trx_id`'s, which keeps the name `AlreadyExists` until it
    // commits, as it did before AT-S17 (recreating an index inside the
    // transaction that drops it is not admitted). Held under page 8 by
    // `CreateIndex`; asked unheld by `CheckIndexDef`, whose answer is the
    // early one.
    Status CheckIndexNameFree(std::string_view name, std::uint64_t own_trx_id);

    // The uncached sys.columns scan behind BuildSchemaFromColumns(). Split
    // out so InitTableAccess() can fill an entry without re-probing the
    // cache slot it is filling.
    StatusOr<Schema> ScanSchemaFromColumns(Oid rel_id);

    // The sys.types snapshot, loaded on first use. Non-owning: the vector
    // lives in the cache.
    StatusOr<const std::vector<SysTypeRow>*> EnsureTypes();

    // The in-place rewrite behind `WAYSTONE ON` on an existing row: finds
    // the current-version sys.patterns row for `fetch_id`, hands it to
    // `mutate`, and writes it back over the same slot when `mutate` answers
    // true. Answers whether it wrote; NotFound when no current-version row
    // carries `fetch_id`.
    //
    // Does not touch the cache - the caller's `BumpVersion` drops it.
    StatusOr<bool> MutatePatternRow(std::uint64_t fetch_id,
                                    const std::function<bool(SysPatternRow&)>& mutate);

    // The single invalidation point: bumps the version and drops every
    // DDL-invalidatable cache entry. `what` names the mutation for the
    // Debug line. Call it *after* the page write succeeds - a failed
    // mutation staled nothing.
    void BumpVersion(std::string_view what, Tally cause = Tally::kWordMovesSchema);

    storage::PageStore& store_;

    // Instance-pinned, never mutated after construction - see the
    // constructor. Every RowLayout in the cache was built for this value.
    std::uint32_t inline_cell_width_ = storage::kDefaultInlineCellWidth;


    Logger* log_ = nullptr;
    // Armed by the `CommandDispatcher` constructor, so it is null for
    // bootstrap, for recovery and for a test over a bare store; every
    // core's `CoreRuntime` builds a dispatcher, so every serving catalog is
    // armed. **The predicate is the instance's since AX-S2**
    // (`ddl-transactional.md` §5b): a mark written by another core's open
    // transaction stays unsettled here until that transaction decides.
    // Until then it read as settled - `IsInFlight` walked one core's live
    // set and the scan's bound was that core's - and what stood between
    // that and a wrong answer was the DDL's relation `X`. Null is the
    // pre-DT9 answer: a mark counts the moment it is written.
    const txn::TransactionManager* txn_ = nullptr;
    // RV3: null means unlogged catalog writes, the pre-RV3 engine.
    wal::WalManager* wal_ = nullptr;
    // RV3-3: per-DDL-statement, dispatcher-installed; empty = no undo.
    DdlUndoHook ddl_undo_hook_;
    DeclarationLoader declaration_loader_;
    // Delete-marks written this run and possibly still on a page - the
    // gate that keeps PurgeSettledDeleteMarks() from Get()-dirtying every
    // catalog page after a DDL that marked nothing (found when D2 made
    // every autocommit DDL reach the purge and the sweep's kWrite fetches
    // turned the whole catalog dirty per statement). Grows on each mark and
    // is resettled by the sweep - a *delta* since AT-S5b, `ResettleMarks`
    // saying why; a rollback clears its marks behind the catalog's back,
    // so the counter may overshoot, costing at most one sweep that finds
    // less than it expected and never a missed mark.
    //
    // **This member is the fallback**: `words_->mark_count` is where the
    // count lives when an instance named its words (`SetInstanceWords`), and
    // the three accessors above are the only things that touch either.
    std::uint64_t pending_marks_ = 0;

    // **The opted count's fallback** (CF-S1c), `pending_marks_`'s shape:
    // `words_->opted_count` is where the count lives when an instance named
    // its words (`SetInstanceWords`). Moved by the two below and set at a
    // boot; nothing else writes either.
    std::uint64_t local_opted_ = 0;
    // A 0->1 (`up`) or 1->0 change of the bit on a current-version row,
    // after its write and under page 9's hold. The decrement saturates at 0:
    // a count set at a boot from a chain a raw write moved since must not
    // wrap to "everything is opted".
    void MoveOptedCount(bool up) noexcept;
    void StoreOptedCount(std::uint64_t value) noexcept;
    // Unset until the first GenerateUserOid() recovers it from the catalog.
    // An optional rather than a sentinel value, because every integer in
    // this type's range is a legal oid and a sentinel would be one more
    // thing that has to stay outside the range it guards. Unused, and
    // empty, while `words_` is set: the instance's `oid_sequence` issues
    // instead.
    std::optional<Oid> next_user_oid_;

    // The highest oid `sys.objects` and `sys.columns` carry, or
    // kUserOidStart - 1 if they carry none above it. Reads the pages; called
    // once per process, by the first GenerateUserOid().
    StatusOr<Oid> HighestIssuedUserOid();

    SysObjectRegistry sys_objects_;
    CatalogCache cache_;
    std::uint64_t catalog_version_ = 0;

    // The value of the instance's schema word this cache was built at
    // (`Revalidate()`). `BumpWord()` runs at every DDL write; its adopt-only-
    // if-current rule is stated where it is decided, in `catalog.cpp`.
    void BumpWord(Tally cause);
    std::uint64_t cache_built_at_ = 0;

    // The instance's four words (`SetInstanceWords`, CT6), or null for a
    // bare catalog, which then runs on the local copies above.
    InstanceWords* words_ = nullptr;
};

}  // namespace kds::catalog
