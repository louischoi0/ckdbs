#include "kds/catalog/catalog.hpp"

#include "kds/catalog/foreign_key.hpp"
#include "kds/parser/fingerprint.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <optional>

#include "kds/server/superblock.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/log_page_image.hpp"
#include "kds/storage/visit.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/varheap.hpp"
#include "kds/txn/manager.hpp"
#include "kds/storage/anchor_page.hpp"
#include "kds/wal/log_anchor_update.hpp"
#include "kds/wal/log_page_init.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

namespace kds::catalog {

// The overflow range must end exactly where the user pages begin: a catalog
// page at or above this is one a peer core may not fault (well_known.hpp).
static_assert(kCatalogOverflowLimit == server::kFirstUserPageId,
              "the catalog overflow range must end at the first user page");

namespace {

// Scans every live row of type RowT out of the heap page at `page_id`.
// Dead slots (RowT::Decode's caller never sees them - ReadTuple() itself
// reports NotFound for a dead slot) are skipped.
// **GetForRead, not Get.** A scan reads and modifies nothing, and Get()
// marks the frame dirty by convention rather than by what the caller
// actually wrote (page_store.hpp) - so every catalog lookup used to dirty a
// catalog page, and every checkpoint wrote all nine of them back having
// changed none.
//
// Multicore turned that waste into a refusal: a peer may read the catalog
// pages and may never write one (workplan-crosscore.md P6), so a read that
// dirties is a read a peer cannot do at all. Which is the ownership check
// doing exactly its job - the bug predates it by a long way.
//
// **`view`, when given, is the reader's visibility** (DT3): a catalog row
// stamped by a transaction that this view cannot see is not there for this
// reader, exactly as an uncommitted user row is not. Null means "see
// everything", which is what bootstrap, recovery and every internal read
// pass - and what every caller passed before DT3 existed, so a null view
// reproduces the old behaviour byte for byte.
//
// **A null view is not the absence of a rule, it is one rule: an object
// exists from the moment its row is written until its removal commits**
// (`ddl-transactional.md` §5a, DT9). `txn`, when given, is what makes
// the second half true - a delete-mark counts only once its deleter is no
// longer in flight. The two halves are deliberately asymmetric, and the
// symmetric version is a bug: hiding *inserted* rows from unfiltered
// readers too would stop a session's own uncommitted `CREATE INDEX` from
// being maintained by its own `INSERT`s, which is the same wrong-result
// class this arm exists to close, mirrored.
//
// Both halves fail toward "the object is there".
//
// Only the `trx_id` is consulted, not the undo chain: a catalog row's
// `undo_ptr` is always 0 (txn.md §7), so there is no older version to step
// back to. A delete-marked catalog row is DT5's business, not this one's.
template <typename RowT>
StatusOr<std::vector<RowT>> ScanAll(storage::PageStore& store, PageId root,
                                    const txn::ReadView* view = nullptr,
                                    const txn::TransactionManager* txn = nullptr) {
    std::vector<RowT> rows;
    Status inner = Status::OK();
    // **Taken once per scan, not per row.** `IsInFlight` is a walk of the
    // core's live list, and a catalog page can hold many delete-marked
    // rows - the product is what the DT9 measurement priced at
    // `marks x (0.4 ns + 0.45 ns x live)`
    // (`bench/results-ddl-catalog-read-ab.md`). Every id below the oldest
    // running transaction is settled by definition, so the common mark -
    // left by a drop that committed long ago - costs one comparison and
    // the walk is never entered. With nothing running this is
    // `UINT64_MAX` and no mark ever consults the manager at all.
    const std::uint64_t oldest_active =
        txn != nullptr ? txn->OldestActiveTrxId() : std::numeric_limits<std::uint64_t>::max();
    Status walked = heap::ChainVisit(
        store, root, storage::PageAccess::kRead,
        [&](PageId, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            if (!tuple.ok()) {
                if (tuple.status().code() == StatusCode::kNotFound) {
                    return storage::VisitControl::kContinue;
                }
                inner = tuple.status();
                return tuple.status();
            }
            // A delete-marked row (DT5): `trx_id` is the *deleter*, so the
            // question flips. With a view, the row is gone for a reader
            // that can see the deleting transaction and still there for
            // one that cannot, which is what makes a rolled-back drop
            // invisible to everyone but the dropper. Without one, the mark
            // counts once its deleter is no longer in flight (DT9) - and
            // for a catalog that has no manager to ask, immediately, which
            // is the pre-DT9 answer and the wrong one while a drop is
            // open.
            //
            // **No undo chain is consulted**, because a catalog row has
            // none: `undo_ptr` is 0 (txn.md §7). That is exactly why an
            // in-place *overwrite* under a transaction cannot be hidden
            // from other readers - ddl-transactional.md §5a says what
            // that costs DROP TABLE, which DT9 does not change.
            if (tuple.value().deleted) {
                const std::uint64_t deleter = tuple.value().trx_id;
                // The null-manager arm is spelled out rather than left to
                // `oldest_active`'s sentinel: it *would* short-circuit,
                // but only because a trx id cannot reach `UINT64_MAX`
                // (48 bits, `kMaxTrxId`), and no reader should have to
                // prove that to know this cannot dereference null.
                const bool gone = view != nullptr ? view->Visible(deleter)
                                                  : txn == nullptr || deleter < oldest_active ||
                                                        !txn->IsInFlight(deleter);
                if (gone) return storage::VisitControl::kContinue;
            } else if (view != nullptr && !view->Visible(tuple.value().trx_id)) {
                return storage::VisitControl::kContinue;  // not there, for this reader
            }
            auto row = RowT::Decode(tuple.value().payload);
            if (!row.ok()) {
                inner = row.status();
                return row.status();
            }
            rows.push_back(row.value());
            return storage::VisitControl::kContinue;
        });
    if (!inner.ok()) return inner;
    if (!walked.ok()) return walked;
    return rows;
}

// Walks the chain and lets `fn` act on the first row it accepts.
//
// Every catalog mutation has the same shape - find the row that matches,
// change it or retire it, stop - and each used to spell out its own loop
// over a single page's slots. That loop is what chaining breaks, so there
// is now one of it: `fn` returns false to keep looking, true when it has
// acted, or a failure, which is reported as-is.
//
// **kWrite**, unconditionally: every caller of this writes. A reader uses
// ScanAll, which fetches read-only so a lookup does not dirty a catalog
// page (the bug multicore's ownership check surfaced).
// `acted_page`, when given, receives the page the accepted row was on -
// which is not `root` once a catalog relation has overflowed onto a
// chained page, and is what a rollback needs to address the row again
// (workplan-ddl-transactional.md DT5).
template <typename RowT, typename Fn>
StatusOr<bool> ForFirstRow(storage::PageStore& store, PageId root, Fn&& fn,
                            PageId* acted_page = nullptr) {
    // **Its own page walk, not `heap::ChainVisit`.** The walk is four lines
    // and the difference is measurable: ChainVisit takes a `std::function`,
    // so every slot costs an indirect call plus a `StatusOr<VisitControl>`
    // built and unwrapped, and this is on the INSERT path - `AllocateRowId`
    // runs it per row. Routing it through the general walk cost 0.49 us ->
    // 1.11 us per allocation, about 7% of an unlogged INSERT, for a shared
    // loop over a relation that is a handful of rows long.
    //
    // `fn` is a template parameter here, so it inlines. That is the whole of
    // the difference; the walk itself is the same next_page_id chain.
    PageId current = root;
    for (std::uint32_t steps = 0; steps < kCatalogOverflowLimit; ++steps) {
        auto bytes = store.Get(current);
        if (!bytes.ok()) return bytes.status();

        heap::PageView page(bytes.value().bytes());
        const std::uint16_t n = page.slot_count();
        for (std::uint16_t slot = 0; slot < n; ++slot) {
            auto tuple = page.ReadTuple(slot);
            if (!tuple.ok()) {
                if (tuple.status().code() == StatusCode::kNotFound) continue;
                return tuple.status();
            }
            auto row = RowT::Decode(tuple.value().payload);
            if (!row.ok()) return row.status();

            auto done = fn(row.value(), page, current, slot, tuple.value());
            if (!done.ok()) return done.status();
            if (done.value()) {
                if (acted_page != nullptr) *acted_page = current;
                return true;
            }
        }

        const PageId next = page.next_page_id();
        if (next == kInvalidPageId) return false;
        current = next;
    }
    return Status::Corruption("catalog: chain from page " + std::to_string(root) +
                              " is longer than the reserved range can be");
}

// The page a growing catalog chain takes next.
//
// **From the reserved low range, never from the free map's general
// supply** (well_known.hpp says why: a peer may only fault low pages, and
// the invalidation flush has to be able to name every catalog page). The
// range is probed rather than tracked, because `CreateAt` already answers
// "is this id taken" durably through the free map - a second record of it
// here would be a second thing to keep true across a crash.
// ---- RV3: catalog writes log the ordinary record types --------------------
// (docs/workplan-rv3-catalog-recovery.md; wal.md's "Catalog" line.)
//
// One helper per mutation shape, mirroring the DML path's LogInsert
// exactly - same payloads, same stamp discipline - so redo needs nothing
// new. A null `wal` no-ops: bootstrap and the socket-free tests run
// unlogged, the pre-RV3 engine. Every helper takes the store because a
// stamped `page_lsn` is what puts the page under the WAL-before-data gate.

Status LogCatRow(wal::WalManager* wal, storage::PageStore& store, wal::RecordType type,
                 std::uint64_t env_trx, PageId page_id, std::uint16_t slot,
                 std::span<const std::byte> bytes, std::uint64_t hdr_trx,
                 std::uint64_t hdr_undo) {
    if (wal == nullptr) return Status::OK();
    // The envelope is read by **analysis alone** (redo takes the writer
    // from the payload), and analysis notes every named transaction as a
    // loser until a TXN_COMMIT in range says otherwise - so a write that
    // belongs to no bracketed transaction must travel under kNoTxnId,
    // which note_txn skips by design. Naming the header's writer here was
    // the review's B1: a next_id bump logged under the relation's creator
    // made every mount past that creator's checkpointed commit invent a
    // phantom crash loser and write a spurious TXN_ABORT for it.
    if (env_trx == kBootstrapXid) env_trx = wal::kNoTxnId;
    std::vector<std::byte> payload(wal::kHeapWriteFixedSize + bytes.size());
    const wal::HeapWritePayload fields{hdr_trx, hdr_undo, slot,
                                       static_cast<std::uint16_t>(bytes.size())};
    if (auto n = wal::EncodeHeapWrite(payload, fields, bytes); !n.ok()) return n.status();
    auto rec = wal->Append(wal::RecordSpec{type, env_trx, page_id}, payload);
    if (!rec.ok()) return rec.status();
    return store.StampPageLsn(page_id, rec.value());
}

Status LogCatInsert(wal::WalManager* wal, storage::PageStore& store, std::uint64_t trx_id,
                    PageId page_id, std::uint16_t slot, std::span<const std::byte> bytes) {
    // undo_ptr 0 always: a catalog row has no version chain (txn.md §7).
    return LogCatRow(wal, store, wal::RecordType::kHeapInsert, trx_id, page_id, slot, bytes,
                     trx_id, /*hdr_undo=*/0);
}

Status LogCatDeleteMark(wal::WalManager* wal, storage::PageStore& store, std::uint64_t deleter,
                        PageId page_id, std::uint16_t slot) {
    if (wal == nullptr) return Status::OK();
    // LogCatRow's envelope rule (B1), stated there.
    if (deleter == kBootstrapXid) deleter = wal::kNoTxnId;
    std::array<std::byte, wal::kDeleteMarkPayloadSize> buf{};
    const wal::HeapDeleteMarkPayload fields{deleter, slot};
    if (auto n = wal::EncodeHeapDeleteMark(buf, fields); !n.ok()) return n.status();
    auto rec =
        wal->Append(wal::RecordSpec{wal::RecordType::kHeapDeleteMark, deleter, page_id}, buf);
    if (!rec.ok()) return rec.status();
    return store.StampPageLsn(page_id, rec.value());
}

Status LogCatRetire(wal::WalManager* wal, storage::PageStore& store, std::uint64_t env_trx,
                    PageId page_id, std::uint16_t slot) {
    if (wal == nullptr) return Status::OK();
    // LogCatRow's envelope rule (B1), stated there.
    if (env_trx == kBootstrapXid) env_trx = wal::kNoTxnId;
    std::array<std::byte, wal::kSlotRetirePayloadSize> buf{};
    const wal::SlotRetirePayload fields{slot};
    if (auto n = wal::EncodeSlotRetire(buf, fields); !n.ok()) return n.status();
    auto rec = wal->Append(wal::RecordSpec{wal::RecordType::kSlotRetire, env_trx, page_id}, buf);
    if (!rec.ok()) return rec.status();
    return store.StampPageLsn(page_id, rec.value());
}

// The write-then-log pairs the mutation sites call, so a site cannot
// mutate without logging. The envelope transaction is the **acting**
// transaction when one brackets the write, and kNoTxnId otherwise -
// never the header's writer, which LogCatRow's B1 note explains.
Status OverwriteLogged(wal::WalManager* wal, storage::PageStore& store, heap::PageView& page,
                       PageId page_id, std::uint16_t slot, std::span<const std::byte> bytes,
                       std::uint64_t env_trx, std::uint64_t hdr_trx, std::uint64_t hdr_undo) {
    if (Status s = page.OverwriteTuple(slot, bytes, hdr_trx, hdr_undo); !s.ok()) return s;
    return LogCatRow(wal, store, wal::RecordType::kHeapOverwrite, env_trx, page_id, slot, bytes,
                     hdr_trx, hdr_undo);
}

Status DeleteMarkLogged(wal::WalManager* wal, storage::PageStore& store, heap::PageView& page,
                        PageId page_id, std::uint16_t slot, std::uint64_t deleter) {
    if (Status s = page.DeleteMark(slot, deleter); !s.ok()) return s;
    return LogCatDeleteMark(wal, store, deleter, page_id, slot);
}

Status RetireLogged(wal::WalManager* wal, storage::PageStore& store, heap::PageView& page,
                    PageId page_id, std::uint16_t slot, std::uint64_t env_trx) {
    if (Status s = page.RetireSlot(slot); !s.ok()) return s;
    return LogCatRetire(wal, store, env_trx, page_id, slot);
}

// A page a DDL created, deliberately unstamped like the DML path's
// new-tuple-page PAGE_INIT: the first record that lands in it stamps it,
// and an empty formatted page is exactly what redo rebuilds. The defaults
// are the catalog overflow page's (min_key 0, kHeap, unattributed); a
// relation's own root pages pass their type and owning oid.
Status LogCatPageInit(wal::WalManager* wal, std::uint64_t trx_id, PageId page_id,
                      PageType type = PageType::kHeap, Oid owner_oid = 0) {
    // LogCatRow's envelope rule (B1), stated there; min_key 0 - catalog
    // rows carry no key space, and a relation root passes owner + type.
    if (trx_id == kBootstrapXid) trx_id = wal::kNoTxnId;
    auto rec = wal::LogPageInit(wal, trx_id, page_id, type, /*min_key=*/0, owner_oid);
    return rec.ok() ? Status::OK() : rec.status();
}

// One anchor slot's durable half (PW2-1): PAGE_INIT rebuilds only the
// common header, so the roots ride ANCHOR_UPDATE - the same record a
// root move writes. Stamps, unlike LogCatPageInit: this IS the first
// record that lands in the page, and an anchor is not an empty
// formatted page - its roots are body content (the 3f07eda review's
// C3). Replay stays safe without ordering care because the redo range
// is a suffix: a replayed PAGE_INIT that wipes the body is necessarily
// followed in-range by every ANCHOR_UPDATE that refills it.
Status LogCatAnchorUpdate(wal::WalManager* wal, storage::PageStore& store, std::uint64_t trx_id,
                          PageId anchor_page, std::uint64_t index_oid, PageId root) {
    if (trx_id == kBootstrapXid) trx_id = wal::kNoTxnId;
    auto rec = wal::LogAnchorUpdate(wal, trx_id, anchor_page, index_oid, root);
    if (!rec.ok()) return rec.status();
    if (rec.value() == wal::kNoLsn) return Status::OK();  // unlogged store
    return store.StampPageLsn(anchor_page, rec.value());
}

// The old tail's link edit when a chain grows: no record type describes a
// next_page_id edit, so it travels as a full page image - the DML path's
// rule for every structural change (command_dispatcher.cpp's LogInsert).
Status LogCatPageImage(wal::WalManager* wal, storage::PageStore& store, std::uint64_t trx_id,
                       PageId page_id) {
    if (wal == nullptr) return Status::OK();
    // LogCatRow's envelope rule (B1), stated there.
    if (trx_id == kBootstrapXid) trx_id = wal::kNoTxnId;
    return storage::LogFullPageImage(wal, store, trx_id, page_id);
}

// The identity a compensation re-reads (DT5's rule): the Keystone word of
// the payload, or 0 for a payload too short to carry one - which no
// catalog row is, so the fallback is a never-taken safety.
std::uint64_t UndoPkOf(std::span<const std::byte> payload) {
    auto id = KeystoneIdOfPayload(payload);
    return id.ok() ? id.value() : 0;
}

StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> AllocateCatalogPage(
    storage::PageStore& store) {
    for (PageId id = kCatalogOverflowFirst; id < kCatalogOverflowLimit; ++id) {
        auto created = store.CreateAt(id);
        if (created.ok()) return std::make_pair(id, created.value().bytes());
        if (created.status().code() != StatusCode::kAlreadyExists) return created.status();
    }
    return Status::OutOfSpace(
        "catalog: the reserved catalog page range (" + std::to_string(kCatalogOverflowFirst) +
        ".." + std::to_string(kCatalogOverflowLimit - 1) +
        ") is full; every catalog relation's chain together has outgrown it");
}

// Appends one row to the chain rooted at `root`, growing it by a page when
// the tail is full.
//
// This is `heap::ChainInsert` minus the one thing a catalog row does not
// have: a **Keystone id**. A user tuple's first word is its primary key, and
// the heap chain uses it to keep pages key-ordered and to enforce `min_key`
// (invariant 3). A catalog row is a fixed-offset struct with no such word,
// so there is no key to order by and every page carries `min_key = 0` - the
// chain here is an append list, not a semi-sorted heap. Sharing the heap's
// insert would mean inventing an id for a row nothing looks up by id.
// `where`, when given, receives the (page, slot) the row landed at
// (workplan-ddl-transactional.md DT3a). A transactional DDL registers
// that address on its transaction's trail so `Abort` can retire the slot -
// the engine hides aborted work by compensation, not by visibility (spec
// §2's correction), so without this a rolled-back CREATE TABLE stays.
template <typename RowT>
Status InsertRow(wal::WalManager* wal, const Catalog::DdlUndoHook& hook,
                  storage::PageStore& store, PageId root, const RowT& row,
                  std::uint64_t trx_id, CatalogRowRef* where = nullptr) {
    const auto encoded = row.Encode();

    PageId current = root;
    for (std::uint32_t steps = 0; steps < kCatalogOverflowLimit; ++steps) {
        auto bytes = store.Get(current);
        if (!bytes.ok()) return bytes.status();

        heap::PageView page(bytes.value().bytes());
        auto slot = page.InsertTuple(encoded, trx_id);
        if (slot.ok()) {
            // The undo record first, then the row record - the hook's
            // whole contract (catalog.hpp): redo alone must never be able
            // to resurrect a loser's row that undo has no record for.
            if (hook) {
                if (Status s = hook({Catalog::DdlUndoEvent::Kind::kInsert, current,
                                     slot.value(), encoded, 0, 0, UndoPkOf(encoded)});
                    !s.ok()) {
                    return s;
                }
            }
            if (Status s = LogCatInsert(wal, store, trx_id, current, slot.value(), encoded);
                !s.ok()) {
                return s;
            }
            if (where != nullptr) *where = CatalogRowRef{current, slot.value()};
            return Status::OK();
        }
        if (slot.status().code() != StatusCode::kOutOfSpace) return slot.status();

        // Full. Walk on if there is already a next page, otherwise grow.
        const PageId next = page.next_page_id();
        if (next != kInvalidPageId) {
            current = next;
            continue;
        }

        auto created = AllocateCatalogPage(store);
        if (!created.ok()) return created.status();
        auto [new_id, new_bytes] = created.value();

        // min_key 0, like every catalog page: these rows carry no key to
        // prune by, and a nonzero min_key would be a claim about ids that
        // do not exist here.
        auto fresh = heap::PageView::CreateEmpty(new_bytes, 0);
        if (!fresh.ok()) return fresh.status();
        // Logged before the insert below, whose record then stamps the
        // page - the DML path's new-tuple-page discipline exactly.
        if (Status s = LogCatPageInit(wal, trx_id, new_id); !s.ok()) return s;

        auto placed = fresh.value().InsertTuple(encoded, trx_id);
        if (placed.ok() && where != nullptr) {
            *where = CatalogRowRef{new_id, placed.value()};
        }
        if (!placed.ok()) {
            // A row no empty page can hold. The page stays allocated and
            // unlinked rather than freed - there is no free-page path - and
            // nothing reaches it, which is the same trade heap_chain makes.
            return placed.status();
        }
        if (hook) {
            if (Status s = hook({Catalog::DdlUndoEvent::Kind::kInsert, new_id, placed.value(),
                                 encoded, 0, 0, UndoPkOf(encoded)});
                !s.ok()) {
                return s;
            }
        }
        if (Status s = LogCatInsert(wal, store, trx_id, new_id, placed.value(), encoded);
            !s.ok()) {
            return s;
        }

        // Linked **after** the row is in it, and through a re-fetch:
        // CreateAt may have moved frames, and a link published before the
        // page it points at is filled is a link a reader can follow into an
        // empty page - the ordering that protects a concurrent reader on
        // this core. The edit itself travels as a full page image: no
        // record type describes a next_page_id edit (LogCatPageImage).
        auto tail_again = store.Get(current);
        if (!tail_again.ok()) return tail_again.status();
        heap::PageView(tail_again.value().bytes()).set_next_page_id(new_id);
        if (Status s = LogCatPageImage(wal, store, trx_id, current); !s.ok()) return s;
        return Status::OK();
    }
    return Status::Corruption("catalog: chain from page " + std::to_string(root) +
                              " is longer than the reserved range can be");
}

}  // namespace

void Catalog::InitWellKnownObjects() {
    auto register_namespace = [this](Oid oid, std::string_view name) {
        SysObjectRow obj{};
        obj.oid = oid;
        obj.namespace_oid = oid;  // a namespace's own namespace is itself
        obj.type_oid = kTypeNamespace;
        obj.rel_id = 0;
        SetName(obj.name, name);
        sys_objects_.Register(obj);
    };
    auto register_type = [this](Oid oid, std::string_view name) {
        SysObjectRow obj{};
        obj.oid = oid;
        obj.namespace_oid = kNamespaceSys;
        obj.type_oid = oid;  // a type object's type is itself
        obj.rel_id = 0;
        SetName(obj.name, name);
        sys_objects_.Register(obj);
    };

    register_namespace(kNamespaceSys, "namespaceSys");
    register_namespace(kNamespacePublic, "namespacePublic");

    register_type(kTypeInt, "type_int");
    register_type(kTypeVarchar, "type_varchar");
    register_type(kTypeChar, "type_char");
    register_type(kTypeSchema, "type_schema");
    register_type(kTypeBool, "type_bool");
    register_type(kTypeBytes, "type_bytes");
    register_type(kTypeNamespace, "type_namespace");
    register_type(kTypeAttribute, "type_attribute");
    register_type(kTypeColumn, "type_column");
    register_type(kTypePage, "type_page");
    register_type(kTypeTable, "type_table");
    register_type(kTypeOperator, "type_operator");
    register_type(kTypeIndex, "type_index");
    // A dropped relation's row is retyped to this (DT2), so it names an
    // object like every other `type_oid` a sys.objects row can carry. It
    // was reaching a registered object only by sharing kTypeOperator's 22
    // until 2026-08-13; once the oid moved off 22 it would otherwise name
    // nothing at all.
    register_type(kTypeDroppedTable, "type_dropped_table");

    register_type(kTypeInt8, "type_int8");
    register_type(kTypeInt16, "type_int16");
    register_type(kTypeInt32, "type_int32");
    register_type(kTypeFloat, "type_float");
    register_type(kTypeDecimal, "type_decimal");
    register_type(kTypeUint64, "type_uint64");
}

Status Catalog::Bootstrap() {
    InitWellKnownObjects();

    struct SysTableBootstrap {
        Oid oid;
        std::string_view name;
        PageId page_id;
    };
    static constexpr std::array<SysTableBootstrap, 9> kSysTables{{
        {kSysTypesTable, "types", kCatalogPageTypes},
        {kSysObjectsTable, "objects", kCatalogPageObjects},
        {kSysColumnsTable, "columns", kCatalogPageColumns},
        {kSysTablesTable, "tables", kCatalogPageTables},
        {kSysIndexesTable, "indexes", kCatalogPageIndexes},
        // sys.patterns gets no sys.columns rows, exactly like the five
        // above: the catalog relations are read through their typed row
        // codecs (rows.hpp), never through a schema, so a column list for
        // them would describe nothing anyone reads.
        {kSysPatternsTable, "patterns", kCatalogPagePatterns},
        // sys.access_stats is a fixed-offset typed row like the six above,
        // so it needs nothing beyond a page and its two catalog rows -
        // unlike sys.pattern_defs, which stores text and therefore had to
        // become a real row-codec relation in phase 5.
        {kSysAccessStatsTable, "access_stats", kCatalogPageAccessStats},
        // sys.cabins, same shape again: a page and its two catalog rows.
        // It issues Keystone ids (a Cabin's `cabin_id` comes from this
        // relation's own `next_id`, like sys.patterns' oid), which is why
        // it needs the sys.tables row and not just the page.
        {kSysCabinsTable, "cabins", kCatalogPageCabins},
        // sys.fkeys, same shape again. It issues Keystone ids for the same
        // reason sys.cabins does - an `fk_id` comes from this relation's own
        // `next_id`, not from GenerateUserOid(), which numbers objects.
        {kSysFkeysTable, "fkeys", kCatalogPageFkeys},
    }};

    // Phase 1: allocate the fixed catalog heap pages. min_key=0: catalog
    // tables are always scanned in full by oid/name (ScanAll above), never
    // pruned by key range, so there is no meaningful min_key to choose.
    for (const auto& t : kSysTables) {
        auto created = store_.CreateAt(t.page_id);
        if (!created.ok()) {
            // Bootstrap runs once, on a store that should have none of
            // these ids; a conflict here means it is being run against an
            // existing database, which is the destructive mistake
            // bootstrap.hpp's fresh/existing branch exists to prevent.
            if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                log_->Error("catalog", "bootstrap could not create catalog page " +
                                           std::to_string(t.page_id) + " for sys." +
                                           std::string(t.name) + ": " +
                                           created.status().message());
            }
            return created.status();
        }
        auto page = heap::PageView::CreateEmpty(created.value().bytes(), 0);
        if (!page.ok()) return page.status();
    }

    // Phase 2: sys.objects rows for the catalog tables themselves.
    for (const auto& t : kSysTables) {
        if (Status s = InsertObjectRow(t.oid, kNamespaceSys, kTypeTable, t.name); !s.ok()) {
            return s;
        }
    }

    // Phase 3: sys.tables rows.
    for (const auto& t : kSysTables) {
        // kInvalidPageId: the bootstrap catalog tables encode their rows
        // through SysXxxRow::Encode(), never the schema-driven row codec,
        // so nothing of theirs can ever spill.
        if (Status s = InsertRelationRow(t.oid, kNamespaceSys, t.name, t.page_id,
                                          ClusteredType::kHeap, kInvalidPageId);
            !s.ok()) {
            return s;
        }
    }

    // Phase 4: sys.types rows for the well-known scalar types. type_val
    // below (well_known.hpp's kTypeVal* constants) is a placeholder tag:
    // there is no type registry to source them from yet. They are not
    // numbering trivia - src/exec/row_codec.cpp switches on these values to
    // pick an on-disk encoding, and Catalog::ResolveTypeByName() below
    // resolves a parsed CREATE TABLE column's type_name to one of these
    // rows by name. Replace with real registry-sourced values later; the
    // numbers themselves still don't need to match anything external.
    struct SysTypeBootstrap {
        Oid oid;
        std::string_view name;
        std::uint32_t type_val;
        std::uint32_t len;
    };
    static constexpr std::array<SysTypeBootstrap, 13> kTypes{{
        {kTypeInt8, "int8", kTypeValInt8, 1},
        {kTypeInt16, "int16", kTypeValInt16, 2},
        {kTypeInt32, "int32", kTypeValInt32, 4},
        {kTypeInt64, "int64", kTypeValInt64, 8},
        {kTypeUint64, "uint64", kTypeValUint64, 8},
        {kTypeFloat, "float", kTypeValFloat, 4},
        {kTypeDecimal, "decimal", kTypeValDecimal, 8},
        {kTypeBool, "bool", kTypeValBool, 1},
        {kTypeVarchar, "varchar", kTypeValVarchar, 0},
        {kTypeChar, "char", kTypeValChar, 1},
        // docs/spec/types.md TY1. `len` here is the type's *width*, which
        // is what sys.types has always meant by it - a DECIMAL column
        // carries its own (p, s) elsewhere (TY9).
        {kTypeDate, "date", kTypeValDate, 4},
        {kTypeTimestamp, "timestamp", kTypeValTimestamp, 8},
        // The wide decimal under its own name, so name->type_val stays a
        // function: `decimal(p >= 19, s)` reaches it by promotion at the
        // DDL site, `decimal128(p, s)` by this row directly, and DESCRIBE
        // renders whichever a column's type_val says it is.
        {kTypeDecimalWide, "decimal128", kTypeValDecimalWide, 16},
    }};
    for (const auto& t : kTypes) {
        if (Status s = InsertTypeRow(t.oid, t.name, t.type_val, t.len); !s.ok()) {
            return s;
        }
    }

    // Phase 5: sys.pattern_defs, the one catalog relation stored in
    // ordinary user tuple format (well_known.hpp explains why). It is built
    // here by hand rather than through CreateTable() for two reasons:
    // CreateTable() takes its oids from GenerateUserOid(), which is
    // in-memory at bootstrap time and could not recover a position from the
    // very pages being built, and it roots the relation on a
    // dynamically allocated page, which nothing could find at bootstrap
    // without first reading the catalog it is part of.
    //
    // It must come after phase 4: BuildPatternDefsSchema() names its column
    // types by the type_val tags phase 4 just registered.
    if (Status s = BootstrapPatternDefs(); !s.ok()) return s;

    // Phase 6: sys.assertions, the second relation of that kind and for the
    // same reason - it stores a declaration's text verbatim (AS10), which is
    // what lets an assertion's GROUP BY list have no cap at all.
    if (Status s = BootstrapAssertions(); !s.ok()) return s;

    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("catalog", "bootstrapped " + std::to_string(kSysTables.size() + 1) +
                                  " system tables and " + std::to_string(kTypes.size()) +
                                  " types on the fixed catalog pages");
    }
    return Status::OK();
}

namespace {

Schema PatternDefsSchema() {
    // Five columns. The first is not in the spec's list and has to be:
    // every relation's first column is its system-generated Keystone pk
    // (invariant 11), and a relation stored in user tuple format is a
    // relation that has one.
    //
    // `param_count` is the declaration's **materialized arity** - the number
    // of value slots the body has, per spec section 3.3. Stored rather than
    // recomputed because recomputing it means re-fingerprinting the source
    // text, and the two could then disagree for a row written by an older
    // build. There is deliberately **no sibling relation for the parameters
    // themselves**: their names and types are recoverable from
    // `source_text`, which is the canon, and a second relation would be a
    // second thing to keep consistent with it.
    //
    // `name` and `source_text` are varchar, so the relation can spill and
    // gets a var-heap chain. A declaration longer than one var-heap page
    // (8144 bytes) is refused at CREATE PATTERN rather than chained - the
    // spilled-value size cap is still open, and this feature does not settle
    // it.
    Schema schema;
    auto column = [](Oid oid, std::uint32_t pos, std::string_view name, std::uint32_t type_val,
                     std::uint32_t len) {
        SysColumnRow col{};
        col.oid = oid;
        col.rel_id = kSysPatternDefsTable;
        col.pos = pos;
        SetName(col.name, name);
        col.type_val = type_val;
        col.len = len;
        col.notnull = true;
        return col;
    };
    schema.columns.push_back(column(kSysPatternDefsColumnOidBase + 0, 0, "id", kTypeValInt64, 8));
    schema.columns.push_back(
        column(kSysPatternDefsColumnOidBase + 1, 1, "pattern_id", kTypeValUint64, 8));
    schema.columns.push_back(
        column(kSysPatternDefsColumnOidBase + 2, 2, "param_count", kTypeValInt32, 4));
    schema.columns.push_back(
        column(kSysPatternDefsColumnOidBase + 3, 3, "name", kTypeValVarchar, 0));
    schema.columns.push_back(
        column(kSysPatternDefsColumnOidBase + 4, 4, "source_text", kTypeValVarchar, 0));
    return schema;
}

}  // namespace

Status Catalog::BootstrapPatternDefs() {
    const Schema schema = PatternDefsSchema();

    // Refused here rather than at the first CREATE PATTERN, for the reason
    // CreateTable() gives: a relation whose row size cannot be computed is
    // one no row could ever be written to, and finding that out at bootstrap
    // is finding it out before there is anything to lose. It also proves the
    // schema above is expressible under whatever inline_cell_width this
    // instance pinned.
    if (auto layout = RowLayout::Build(schema, inline_cell_width_); !layout.ok()) {
        return layout.status();
    }

    auto created = store_.CreateAt(kCatalogPagePatternDefs);
    if (!created.ok()) return created.status();
    // min_key 0: like the other catalog pages, this relation is scanned in
    // full - by name or by pattern_id, never by key range. Stamped with the
    // relation's own oid (page.md §2a), unlike the fixed catalog core:
    // this chain grows through ChainInsert, which stamps, so the root must
    // agree with the pages that follow it.
    auto root = heap::PageView::CreateEmpty(created.value().bytes(), 0, kSysPatternDefsTable);
    if (!root.ok()) return root.status();

    auto varheap_root = varheap::CreateChain(store_, kSysPatternDefsTable);
    if (!varheap_root.ok()) return varheap_root.status();

    if (Status s = InsertObjectRow(kSysPatternDefsTable, kNamespaceSys, kTypeTable,
                                    "pattern_defs");
        !s.ok()) {
        return s;
    }
    if (Status s = InsertRelationRow(kSysPatternDefsTable, kNamespaceSys, "pattern_defs",
                                      kCatalogPagePatternDefs, ClusteredType::kHeap,
                                      varheap_root.value());
        !s.ok()) {
        return s;
    }
    for (const auto& col : schema.columns) {
        if (Status s = InsertColumnRow(col.oid, kSysPatternDefsTable, col.pos,
                                        NameView(col.name), col.type_val, col.len, col.notnull);
            !s.ok()) {
            return s;
        }
    }
    return Status::OK();
}

namespace {

Schema AssertionsSchema() {
    // Six columns, and the shape is §8.2's table with one addition and one
    // subtraction, both forced.
    //
    // Added: `id`, the Keystone pk, because every relation stored in user
    // tuple format has one (invariant 11). It **is** §8.2's `assertion_id` -
    // engine-issued, unique, never reused (K1) - rather than a second
    // sequence beside it, because a second sequence is a second thing to keep
    // monotonic and there is nothing the first cannot say.
    //
    // Subtracted: nothing stores the parsed declaration. The group columns,
    // the aggregate, the operator and the bound are all recoverable from
    // `source_text` by re-parsing it, which is the sys.pattern_defs model
    // AS10 names - and it is what lets the GROUP BY list be uncapped, since a
    // longer list costs text rather than a wider row. A decoded sibling table
    // would be a second copy that can drift from the canon.
    //
    // `cabin_root` is the Bound Cabin anchor and is written kInvalidPageId
    // here: AST04 builds the structure, AST06 fills the field in. Reserved
    // now rather than added later because it is in §8.2's table and because
    // adding a catalog column costs the same superblock bump this relation
    // already cost - paying it twice would be paying it for nothing.
    //
    // `name` and `source_text` are varchar, so the relation can spill and
    // gets a var-heap chain. A declaration longer than one var-heap page is
    // refused rather than chained: the spilled-value size cap is open and
    // this does not settle it.
    Schema schema;
    auto column = [](Oid oid, std::uint32_t pos, std::string_view name, std::uint32_t type_val,
                     std::uint32_t len) {
        SysColumnRow col{};
        col.oid = oid;
        col.rel_id = kSysAssertionsTable;
        col.pos = pos;
        SetName(col.name, name);
        col.type_val = type_val;
        col.len = len;
        col.notnull = true;
        return col;
    };
    schema.columns.push_back(column(kSysAssertionsColumnOidBase + 0, 0, "id", kTypeValInt64, 8));
    schema.columns.push_back(
        column(kSysAssertionsColumnOidBase + 1, 1, "target_oid", kTypeValInt32, 4));
    schema.columns.push_back(
        column(kSysAssertionsColumnOidBase + 2, 2, "cabin_root", kTypeValInt64, 8));
    schema.columns.push_back(
        column(kSysAssertionsColumnOidBase + 3, 3, "flags", kTypeValInt32, 4));
    schema.columns.push_back(
        column(kSysAssertionsColumnOidBase + 4, 4, "name", kTypeValVarchar, 0));
    schema.columns.push_back(
        column(kSysAssertionsColumnOidBase + 5, 5, "source_text", kTypeValVarchar, 0));
    return schema;
}

}  // namespace

Status Catalog::BootstrapAssertions() {
    const Schema schema = AssertionsSchema();

    // Refused at bootstrap rather than at the first CREATE ASSERTION, for
    // BootstrapPatternDefs()' reason: a relation whose row size cannot be
    // computed is one no row could ever be written to, and it also proves the
    // schema is expressible under whatever inline_cell_width this instance
    // pinned.
    if (auto layout = RowLayout::Build(schema, inline_cell_width_); !layout.ok()) {
        return layout.status();
    }

    auto created = store_.CreateAt(kCatalogPageAssertions);
    if (!created.ok()) return created.status();
    // min_key 0: scanned in full, by name or by target_oid, never by key
    // range - like every other catalog page. Stamped for the reason
    // pattern_defs' root is: this chain grows through ChainInsert.
    auto root = heap::PageView::CreateEmpty(created.value().bytes(), 0, kSysAssertionsTable);
    if (!root.ok()) return root.status();

    auto varheap_root = varheap::CreateChain(store_, kSysAssertionsTable);
    if (!varheap_root.ok()) return varheap_root.status();

    if (Status s = InsertObjectRow(kSysAssertionsTable, kNamespaceSys, kTypeTable, "assertions");
        !s.ok()) {
        return s;
    }
    if (Status s = InsertRelationRow(kSysAssertionsTable, kNamespaceSys, "assertions",
                                      kCatalogPageAssertions, ClusteredType::kHeap,
                                      varheap_root.value());
        !s.ok()) {
        return s;
    }
    for (const auto& col : schema.columns) {
        if (Status s = InsertColumnRow(col.oid, kSysAssertionsTable, col.pos, NameView(col.name),
                                        col.type_val, col.len, col.notnull);
            !s.ok()) {
            return s;
        }
    }
    return Status::OK();
}

StatusOr<Oid> Catalog::HighestIssuedUserOid() {
    // The two relations every `GenerateUserOid()` result is written to, and
    // the reason that function's comment states the contract: an oid landing
    // in some third relation would be invisible here and reissued after a
    // restart.
    //
    // The bootstrap oids are in these pages too and are all far below
    // kUserOidStart, which is why the floor below is a `max` and not a
    // special case for the empty database.
    Oid highest = kUserOidStart - 1;

    auto objects = ScanAll<SysObjectRow>(store_, kCatalogPageObjects, nullptr, txn_);
    if (!objects.ok()) return objects.status();
    for (const SysObjectRow& row : objects.value()) {
        if (row.oid > highest) highest = row.oid;
    }

    // Columns are scanned as well as objects, and leaving them out is the
    // subtle way to get this wrong: column oids come from the *same* counter
    // as relation oids, so a database whose last DDL was a `CREATE TABLE`
    // has its high-water mark on a column and not on the table.
    auto columns = ScanAll<SysColumnRow>(store_, kCatalogPageColumns, nullptr, txn_);
    if (!columns.ok()) return columns.status();
    for (const SysColumnRow& row : columns.value()) {
        if (row.oid > highest) highest = row.oid;
    }

    return highest;
}

StatusOr<Oid> Catalog::GenerateUserOid() {
    // Recovered once, on first use, then incremented in memory. Lazy rather
    // than done at construction because a `Catalog` is constructed on two
    // paths - `BootstrapDatabase()`'s fresh branch and its existing-database
    // branch - and only one of them calls anything afterwards. Recovering
    // where the value is *needed* covers both without either having to
    // remember, and costs an empty database nothing.
    if (!next_user_oid_.has_value()) {
        auto highest = HighestIssuedUserOid();
        if (!highest.ok()) {
            return highest.status().WithContext(
                "cannot issue an object oid without reading back the ones already issued");
        }
        next_user_oid_ = highest.value() + 1;

        if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
            log_->Debug("catalog", "object oid sequence recovered at " +
                                       std::to_string(*next_user_oid_));
        }
    }
    return (*next_user_oid_)++;
}

void Catalog::BumpVersion(std::string_view what) {
    ++catalog_version_;
    cache_.Invalidate();
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "cache invalidated by " + std::string(what) + ": version " +
                                   std::to_string(catalog_version_));
    }
    // After the local invalidation, never before: the hook flushes the
    // catalog pages and tells peers to re-read, and a peer that re-read
    // while this instance still held stale entries would be reading a
    // catalog its own owner disagrees with.
    if (on_invalidate_) on_invalidate_();
}

void Catalog::InvalidateAfterCompensation() {
    BumpVersion("a transaction that wrote catalog rows resolved");
}

StatusOr<std::uint64_t> Catalog::RetireDeleteMarksBelow(std::uint64_t horizon,
                                                          std::uint64_t* remaining_out) {
    std::uint64_t retired = 0;
    for (const PageId root : kAllCatalogPages) {
        Status inner = Status::OK();
        // **kWrite**, and one forward pass per page is enough:
        // `RetireSlot` sets the dead flag in place, so it never renumbers
        // the slots behind the walk. (The DROP sweep loops for a different
        // reason - `ForFirstRow` returns at its first match.)
        Status walked = heap::ChainVisit(
            store_, root, storage::PageAccess::kWrite,
            [&](PageId page_id, heap::PageView& page,
                std::uint16_t slot) -> StatusOr<storage::VisitControl> {
                auto tuple = page.ReadTuple(slot);
                if (!tuple.ok()) {
                    // An already-dead slot, which is what a retired one
                    // is - nothing to finalize.
                    if (tuple.status().code() == StatusCode::kNotFound) {
                        return storage::VisitControl::kContinue;
                    }
                    inner = tuple.status();
                    return tuple.status();
                }
                if (!tuple.value().deleted) return storage::VisitControl::kContinue;
                // A mark's trx_id is its *deleter*. At or above the
                // horizon proves nothing - some live view may still see
                // the row - so the mark stays for a later pass.
                if (tuple.value().trx_id >= horizon) {
                    if (remaining_out != nullptr) ++*remaining_out;
                    return storage::VisitControl::kContinue;
                }
                if (Status s = page.RetireSlot(slot); !s.ok()) {
                    inner = s;
                    return s;
                }
                // Logged at kBootstrapXid: the sweep acts for no
                // transaction, and the retire must reach the log like
                // every other catalog write (RV3) or a crash re-marks
                // what a reader was already told is gone.
                if (Status s = LogCatRetire(wal_, store_, kBootstrapXid, page_id, slot);
                    !s.ok()) {
                    inner = s;
                    return s;
                }
                ++retired;
                return storage::VisitControl::kContinue;
            });
        if (!inner.ok()) return inner;
        if (!walked.ok()) return walked;
    }
    return retired;
}

StatusOr<std::uint64_t> Catalog::FinalizeDeleteMarksAtMount() {
    // Every mark, unconditionally: a real id is 48 bits, so the horizon
    // that admits everything is any value above kMaxTrxId.
    auto swept = RetireDeleteMarksBelow(std::numeric_limits<std::uint64_t>::max());
    if (!swept.ok()) return swept;
    pending_marks_ = 0;  // nothing survives a horizon above every id
    const std::uint64_t retired = swept.value();

    if (retired > 0) {
        // Info, not Debug: this is a structural catalog write, and it is
        // the only record that rows a previous mount left behind are gone.
        if (log_ != nullptr) {
            log_->Info("catalog", "finalized " + std::to_string(retired) +
                                      " delete-marked catalog row(s) left by a previous mount");
        }
        // Nothing can have cached them yet - this runs before the listener
        // binds - but the version is what a bound statement is stamped
        // with, and the rows really did change on this instance.
        BumpVersion("delete-marks finalized at mount");
    }
    return retired;
}

StatusOr<std::uint64_t> Catalog::PurgeSettledDeleteMarks() {
    // No manager, no marks: DDL then writes at kBootstrapXid and retires
    // its rows directly, so there is nothing this sweep could find.
    if (txn_ == nullptr) return std::uint64_t{0};
    // The gate: a sweep is a kWrite fetch of every catalog page - it
    // dirties the whole catalog even when it retires nothing - so it runs
    // only while some mark written this run may still be on a page.
    if (pending_marks_ == 0) return std::uint64_t{0};
    std::uint64_t remaining = 0;
    auto swept = RetireDeleteMarksBelow(txn_->ReadHorizon(), &remaining);
    if (!swept.ok()) return swept;
    pending_marks_ = remaining;
    if (swept.value() > 0 && log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "purged " + std::to_string(swept.value()) +
                                   " settled delete-marked catalog row(s)");
    }
    // No version bump, unlike the mount sweep: every retired row was
    // already gone to every reader, so no cached answer changes. The
    // proof lives once, in ddl-transactional.md §5d.
    return swept;
}

void Catalog::InvalidateFromPeer() {
    // No version bump. `catalog_version_` is the counter parser-v2.md I5
    // stamps *this instance's* bound statements with; another core's DDL is
    // not an event in this instance's numbering, and advancing it here
    // would invalidate bound statements for a reason they cannot check.
    // What has to go is the cached content.
    cache_.Invalidate();
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "cache invalidated by a peer's DDL");
    }
}

Status Catalog::InsertObjectRow(Oid oid, Oid namespace_oid, Oid type_oid,
                                 std::string_view name, std::uint64_t trx_id,
                                 CatalogRowRef* where) {
    SysObjectRow row{};
    row.oid = oid;
    row.namespace_oid = namespace_oid;
    row.type_oid = type_oid;
    row.rel_id = 0;
    SetName(row.name, name);
    Status s = InsertRow(wal_, ddl_undo_hook_, store_, kCatalogPageObjects, row, trx_id, where);
    if (where != nullptr) where->rel_oid = kSysObjectsTable;
    // A new sys.objects row changes what FindTableOidByName and ListTables
    // would answer, so it stales cached name lookups.
    if (s.ok()) BumpVersion("sys.objects insert");
    return s;
}

Status Catalog::CheckAnchorRoomForIndex(PageId anchor_page, Oid expected_owner_oid,
                                        std::uint64_t index_oid) {
    // A relation whose anchor predates PW2 has none, and there is then no
    // slot to run out of.
    if (anchor_page == kInvalidPageId) return Status::OK();
    auto anchor = store_.GetForRead(anchor_page);
    if (!anchor.ok()) return anchor.status().WithContext("faulting anchor to check index room");
    // The same two structural checks WriteAnchorRoot makes, for the same
    // reason: a page that is not this relation's anchor cannot answer for
    // this relation's table, and reading one as though it were would
    // refuse - or admit - on another relation's count.
    if (Status s = storage::ValidatePageHeader(anchor.value().bytes(), PageType::kAnchor);
        !s.ok()) {
        return s;
    }
    if (storage::GetOwnerOid(anchor.value().bytes()) != expected_owner_oid) {
        return Status::Corruption(
            "anchor page " + std::to_string(anchor_page) + " belongs to relation oid " +
            std::to_string(storage::GetOwnerOid(anchor.value().bytes())) + ", not oid " +
            std::to_string(expected_owner_oid));
    }
    return storage::CheckAnchorRoomForIndex(anchor.value().bytes(), index_oid);
}

Status Catalog::WriteAnchorRoot(PageId anchor_page, Oid expected_owner_oid,
                                std::uint64_t index_oid, PageId root, std::uint64_t trx_id) {
    auto anchor = store_.Get(anchor_page);
    if (!anchor.ok()) return anchor.status().WithContext("faulting anchor for a root move");
    // The write is where the damage is created, so it checks what the
    // redo applier and the fill already check (the 96b0343 review's C3):
    // aimed at a non-anchor page, SetAnchorClusteredRoot would overwrite
    // four bytes of somebody's tuple data with no error anywhere; aimed at
    // another relation's anchor, it would move the wrong tree's root.
    if (Status s = storage::ValidatePageHeader(anchor.value().bytes(), PageType::kAnchor);
        !s.ok()) {
        return s;
    }
    if (storage::GetOwnerOid(anchor.value().bytes()) != expected_owner_oid) {
        return Status::Corruption(
            "anchor page " + std::to_string(anchor_page) + " belongs to relation oid " +
            std::to_string(storage::GetOwnerOid(anchor.value().bytes())) + ", not oid " +
            std::to_string(expected_owner_oid));
    }
    if (index_oid == 0) {
        storage::SetAnchorClusteredRoot(anchor.value().bytes(), root);
    } else if (Status s = storage::SetAnchorIndexRoot(anchor.value().bytes(), index_oid, root);
               !s.ok()) {
        return s;
    }
    return LogCatAnchorUpdate(wal_, store_, trx_id, anchor_page, index_oid, root);
}

Status Catalog::InsertRelationRow(Oid oid, Oid namespace_oid, std::string_view name,
                                   PageId desc_page_id, ClusteredType clustered_type,
                                   PageId varheap_page_id, std::uint32_t owner_core,
                                   PageId anchor_page_id, std::uint64_t trx_id,
                                   CatalogRowRef* where) {
    SysTableRow row{};
    row.oid = oid;
    row.namespace_oid = namespace_oid;
    SetName(row.name, name);
    row.desc_page_id = desc_page_id;
    row.clustered_type = clustered_type;
    row.next_id = kFirstRowId;
    row.varheap_page_id = varheap_page_id;
    row.owner_core = owner_core;
    // Every relation starts ascending: it holds no ids at all, so no id has
    // landed out of order. Nothing may pass this in - it is an observation,
    // and the only writer is AdmitExplicitRowId.
    row.key_order = KeyOrder::kAscending;
    row.anchor_page_id = anchor_page_id;
    Status s = InsertRow(wal_, ddl_undo_hook_, store_, kCatalogPageTables, row, trx_id, where);
    if (where != nullptr) where->rel_oid = kSysTablesTable;
    if (s.ok()) BumpVersion("sys.tables insert");
    return s;
}

Status Catalog::InsertColumnRow(Oid oid, Oid rel_id, std::uint32_t pos, std::string_view name,
                                 std::uint32_t type_val, std::uint32_t len, bool notnull,
                                 std::uint8_t cabin_policy, std::uint64_t trx_id,
                                 CatalogRowRef* where) {
    SysColumnRow row{};
    row.oid = oid;
    row.rel_id = rel_id;
    row.pos = pos;
    SetName(row.name, name);
    row.type_val = type_val;
    row.len = len;
    row.notnull = notnull;
    row.cabin_policy = cabin_policy;
    Status s = InsertRow(wal_, ddl_undo_hook_, store_, kCatalogPageColumns, row, trx_id, where);
    if (where != nullptr) where->rel_oid = kSysColumnsTable;
    // A new column row changes the schema half of a cached TableAccess.
    if (s.ok()) BumpVersion("sys.columns insert");
    return s;
}

Status Catalog::InsertTypeRow(Oid oid, std::string_view name, std::uint32_t type_val,
                               std::uint32_t len) {
    SysTypeRow row{};
    row.oid = oid;
    SetName(row.name, name);
    row.type_val = type_val;
    row.len = len;
    Status s = InsertRow(wal_, ddl_undo_hook_, store_, kCatalogPageTypes, row, kBootstrapXid);
    // The cache treats sys.types as immutable and keeps its snapshot across
    // Invalidate(). This is the only writer, so it is the only place that
    // assumption can be broken - drop the snapshot here rather than rely on
    // "Bootstrap() runs first" staying true.
    if (s.ok()) cache_.InvalidateTypes();
    return s;
}

StatusOr<Oid> Catalog::CreateTable(Oid namespace_oid, std::string_view name, const Schema& schema,
                                    ClusteredType clustered_type,
                                    std::uint64_t trx_id, std::vector<CatalogRowRef>* written) {
    // Refused at definition time rather than at the first INSERT: a table
    // whose first column cannot hold the Keystone id is one no row can
    // ever be written to (heap-and-tuple.md section 4).
    if (Status s = CheckKeystoneColumn(schema); !s.ok()) return s;

    // No key-mode refusal here any more. Until 2026-08-25 an EXPLICIT
    // relation was required to be btree-clustered and this is where the
    // pairing was refused; the mode is gone, both storage types take a
    // caller-supplied pk, and what the heap cannot do is now refused per
    // *id* by AdmitExplicitRowId rather than per relation - a heap relation
    // that only ever omits its pk was never in doubt and no longer has to be
    // declared.
    if (Status s = CheckDeclarableColumnTypes(schema); !s.ok()) return s;

    // Same argument, extended by the fixed-length rule: the relation's row
    // size is a schema constant, so if it cannot be computed - a column
    // with no decided on-disk width, or a row wider than a page - there is
    // no row this table could hold either. Computing it here also means
    // the layout every later InitTableAccess() builds is known to succeed.
    if (auto layout = RowLayout::Build(schema, inline_cell_width_); !layout.ok()) {
        return layout.status();
    }

    // The oid, issued before any page is formatted so every page of the
    // relation carries it from birth (page.md §2a). An oid burned by a
    // failure below is fine by this function's own argument: an oid counts
    // objects ever created, not objects that exist.
    auto generated_oid = GenerateUserOid();
    if (!generated_oid.ok()) return generated_oid.status();
    const Oid new_oid = generated_oid.value();

    // The root's PageRef is scoped so its pin drops before the publish
    // hook at the tail of this function: on a rotated relation that hook
    // EvictCleans the departed pages, and EvictClean refuses a pinned
    // frame - held to the function's end, the eviction failed on every
    // peer CREATE TABLE (the 25059bf review's C-1).
    PageId root_id = kInvalidPageId;
    {
        auto created = store_.CreateNew();
        if (!created.ok()) return created.status();
        root_id = created.value().first;
        const std::span<std::byte, kPageSize> root_bytes = created.value().second.bytes();

        // Both clustered types root at `desc_page_id` and both start as one
        // page - a heap page for kHeap, a B+ tree leaf for kBtree (btree.hpp).
        // A btree relation grows its first internal level only when that leaf
        // splits, so a small table costs exactly what it did before, and the
        // choice is invisible to every layer above until the relation is big
        // enough for it to matter.
        Status formatted = Status::OK();
        switch (clustered_type) {
            case ClusteredType::kHeap: {
                auto root_page = heap::PageView::CreateEmpty(root_bytes, 0, new_oid);
                if (!root_page.ok()) formatted = root_page.status();
                break;
            }
            case ClusteredType::kBtree:
                formatted = btree::FormatRoot(root_bytes, new_oid);
                break;
        }
        if (!formatted.ok()) return formatted;
        // RV3: the relation's root must exist in the log or a crash that
        // loses the unflushed page makes the first HEAP_INSERT irreplayable -
        // redo refuses a record naming a page nothing creates. Both clustered
        // types start as one leaf-shaped page, which is exactly what a
        // PAGE_INIT rebuilds.
        if (Status s = LogCatPageInit(wal_, trx_id, root_id,
                                      clustered_type == ClusteredType::kBtree
                                          ? PageType::kBtreeLeaf
                                          : PageType::kHeap,
                                      new_oid);
            !s.ok()) {
            return s;
        }
    }

    // The var-heap root, allocated here or not at all. Eager rather than
    // on-first-spill, and the reason is the catalog cache's rule
    // (catalog_cache.hpp): a root allocated lazily would be a fact that
    // changes without DDL, and this one is cached on every TableAccess.
    // Allocating at CREATE TABLE keeps it DDL-immutable. The cost is one
    // page per relation that *could* spill, whether or not it ever does -
    // which is why relations that cannot spill get kInvalidPageId and no
    // page at all.
    PageId varheap_root = kInvalidPageId;
    if (SchemaCanSpill(schema)) {
        auto created_varheap = varheap::CreateChain(store_, new_oid);
        if (!created_varheap.ok()) return created_varheap.status();
        varheap_root = created_varheap.value();
        // Same argument as the root above: a VARHEAP_APPEND replayed into
        // a page nothing creates refuses the mount (RC03's reproducer).
        if (Status s = LogCatPageInit(wal_, trx_id, varheap_root, PageType::kVarHeap, new_oid);
            !s.ok()) {
            return s;
        }
    }

    // PW2-1 (workplan-peer-writer.md §7a): the relation's anchor page.
    // PAGE_INIT rebuilds only the common header, and the clustered root is
    // *body* content - so the anchor's durable story is PAGE_INIT plus an
    // ANCHOR_UPDATE record, the same record a root move will write
    // (PW2-3). A crash losing the unflushed anchor otherwise loses the
    // relation's entry points: RC03's argument, one page over. The ref is
    // scoped for the reason the root's is - the publish hook evicts.
    PageId anchor_id = kInvalidPageId;
    {
        auto created_anchor = store_.CreateNew();
        if (!created_anchor.ok()) return created_anchor.status();
        anchor_id = created_anchor.value().first;
        storage::FormatAnchorPage(created_anchor.value().second.bytes(), new_oid, root_id);
        if (Status s = LogCatPageInit(wal_, trx_id, anchor_id, PageType::kAnchor, new_oid);
            !s.ok()) {
            return s;
        }
        if (Status s =
                LogCatAnchorUpdate(wal_, store_, trx_id, anchor_id, /*index_oid=*/0, root_id);
            !s.ok()) {
            return s;
        }
    }

    // Placement (docs/inflight/in-progress/workplan-crosscore.md M1). The rotation counter is
    // how many relations already exist, read off the page rather than
    // derived from the oid. That was originally because the oid restarted
    // at kUserOidStart every boot; it no longer does, but the reason still
    // holds and is a better one - an oid counts objects *ever created*,
    // including columns, where placement wants relations that exist now.
    // Nothing here decides the policy - AssignOwnerCore does, and it is
    // `[PROPOSED]`.
    auto existing_relations = ScanAll<SysTableRow>(store_, kCatalogPageTables, nullptr, txn_);
    if (!existing_relations.ok()) return existing_relations.status();
    // DDL runs on the system core and allocates from its free map, so the
    // relation's pages are the system core's - and a relation must be owned
    // by the core that can fault its pages (core_placement.hpp).
    const std::uint32_t owner_core = AssignOwnerCore(placement_, kSystemCore, core_count_,
                                                     existing_relations.value().size());

    // All three rows of a relation carry the *same* stamp: a reader that
    // could see the sys.tables row but not its sys.columns rows would see
    // a relation with no schema, which is worse than not seeing it at all.
    // One id, one visibility answer for the whole relation.
    // Every row's address is reported when a caller asked for them (DT3a),
    // and the *partial* list is kept on failure too: rows written before
    // the failure are on the page whether or not the statement finished,
    // and a rollback that skipped them would leave exactly the half-built
    // relation this feature exists to prevent.
    auto note = [written](const CatalogRowRef& ref) {
        if (written != nullptr) written->push_back(ref);
    };
    CatalogRowRef ref;
    if (Status s = InsertObjectRow(new_oid, namespace_oid, kTypeTable, name, trx_id, &ref);
        !s.ok()) {
        return s;
    }
    ref.oid = new_oid;
    note(ref);
    if (Status s = InsertRelationRow(new_oid, namespace_oid, name, root_id, clustered_type,
                                      varheap_root, owner_core, anchor_id, trx_id,
                                      &ref);
        !s.ok()) {
        return s;
    }
    ref.oid = new_oid;
    note(ref);

    for (const auto& col : schema.columns) {
        auto col_oid = GenerateUserOid();
        if (!col_oid.ok()) return col_oid.status();
        Status s = InsertColumnRow(col_oid.value(), new_oid, col.pos, NameView(col.name),
                                    col.type_val, col.len, col.notnull, col.cabin_policy,
                                    trx_id, &ref);
        if (!s.ok()) return s;
        ref.oid = col_oid.value();
        note(ref);
    }

    // Debug, not Info: the dispatcher already reports the client-visible
    // DDL at Info ("[ddl] created table ..."), and two Info lines for one
    // statement is how a log stops being readable. What this one adds is
    // the physical detail the client never sees - the root page.
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "created table '" + std::string(name) + "' oid=" +
                                  std::to_string(new_oid) + " root_page=" +
                                  std::to_string(root_id) + " columns=" +
                                  std::to_string(schema.columns.size()));
    }

    // The send side of CC7's handoff (workplan P6c): a relation placed on a
    // core other than the creator's needs that core granted fault rights,
    // or it is unreachable - the pre-CC7 defect, now closed at the one site
    // that knows a non-creating owner was chosen.
    if (owner_core != kSystemCore && on_publish_) {
        on_publish_(new_oid, owner_core, root_id, varheap_root, anchor_id);
    }
    return new_oid;
}

StatusOr<SysTableRow> Catalog::GetSysTableRow(Oid table_oid) {
    auto rows = ScanAll<SysTableRow>(store_, kCatalogPageTables, nullptr, txn_);
    if (!rows.ok()) return rows.status();

    for (const auto& row : rows.value()) {
        if (row.oid == table_oid) return row;
    }
    return Status::NotFound("no sys.tables row for this oid");
}

StatusOr<Oid> Catalog::FindTableOidByName(std::string_view name, const txn::ReadView* view) {
    // **A transactional lookup neither reads nor fills the cache** (DT3;
    // ddl-transactional.md §4's option (a)). The cache is one map for
    // the whole instance and answers "does this name exist" with no idea
    // whose transaction is asking - so serving one session from it would
    // hand it another session's uncommitted relation, and filling it from
    // one would publish that relation to everybody. Scoped to callers that
    // pass a view: with none, this is the path that always existed.
    if (view == nullptr) {
        if (const Oid* cached = cache_.FindOidByName(name); cached != nullptr) {
            return *cached;
        }
    }

    auto rows = ScanAll<SysObjectRow>(store_, kCatalogPageObjects, view, txn_);
    if (!rows.ok()) return rows.status();

    for (const auto& row : rows.value()) {
        if (row.type_oid == kTypeTable && NameView(row.name) == name) {
            if (view == nullptr) cache_.PutOidByName(name, row.oid);
            return row.oid;
        }
    }
    // An absent name is not remembered. Two callers depend on that:
    // HandleCreateTable / HandleCreateTableSql look a name up *expecting*
    // NotFound before creating it, and a second Catalog over the same store
    // can create a table this instance never saw. A remembered absence
    // makes both answers wrong, where a repeated scan is only slow.
    return Status::NotFound("no table with this name");
}

StatusOr<bool> Catalog::NameHeldByPendingDrop(std::string_view name,
                                              const txn::ReadView& view) {
    // Its own walk rather than `ScanAll`, for one reason: the answer is in
    // the tuple *header*, and `ScanAll` hands out decoded rows only. A row
    // this view cannot see is one whose last writer is still in flight -
    // and on a `sys.objects` row that writer can only be a `DROP TABLE`'s
    // retype, since nothing else overwrites one under a transaction.
    bool held = false;
    Status inner = Status::OK();
    Status walked = heap::ChainVisit(
        store_, kCatalogPageObjects, storage::PageAccess::kRead,
        [&](PageId, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            if (!tuple.ok()) {
                if (tuple.status().code() == StatusCode::kNotFound) {
                    return storage::VisitControl::kContinue;
                }
                inner = tuple.status();
                return tuple.status();
            }
            if (view.Visible(tuple.value().trx_id)) return storage::VisitControl::kContinue;
            auto row = SysObjectRow::Decode(tuple.value().payload);
            if (!row.ok()) {
                inner = row.status();
                return row.status();
            }
            if (row.value().type_oid == kTypeDroppedTable &&
                NameView(row.value().name) == name) {
                held = true;
                return storage::VisitControl::kStop;
            }
            return storage::VisitControl::kContinue;
        });
    if (!inner.ok()) return inner;
    if (!walked.ok()) return walked;
    return held;
}

// ALTER TABLE's catalog half (docs/spec/alter.md, workplan ALT02). Both
// renames are one fixed-width Name rewrite - MutatePatternRow's shape -
// followed by BumpVersion(): a name is read by resolution itself, so the
// in-place-cache exception the pattern setters use does not apply.
namespace {

Status CheckRenameName(std::string_view what, std::string_view name) {
    if (name.empty()) {
        return Status::InvalidArgument(std::string(what) + " name is empty");
    }
    // `>=` rather than `>`: SetName stores at most kCatalogNameMax bytes
    // and NameView stops at the array's end, so a name that exactly fills
    // the array round-trips - but the check refuses at the same boundary
    // CreateTable's rows actually hold, and never lets SetName truncate,
    // because a truncated name is not the one that was asked for.
    if (name.size() >= kCatalogNameMax) {
        return Status::InvalidArgument(std::string(what) + " name '" + std::string(name) +
                                        "' is longer than " +
                                        std::to_string(kCatalogNameMax - 1) + " bytes");
    }
    return Status::OK();
}

}  // namespace

Status Catalog::RenameTable(Oid table_oid, std::string_view new_name) {
    if (Status s = CheckRenameName("table", new_name); !s.ok()) return s;

    // The collision check and the write run on one core (DDL is core 0's),
    // so check-then-write is atomic by the event loop.
    if (auto taken = FindTableOidByName(new_name); taken.ok()) {
        return Status::AlreadyExists("a relation named '" + std::string(new_name) +
                                      "' already exists");
    } else if (taken.status().code() != StatusCode::kNotFound) {
        return taken.status();
    }

    auto acted = ForFirstRow<SysObjectRow>(
        store_, kCatalogPageObjects,
        [&](SysObjectRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
            if (row.type_oid != kTypeTable || row.oid != table_oid) return false;
            // The catalog's own names are load-bearing for bootstrap and
            // are nobody's to change (AL7).
            if (row.namespace_oid != kNamespacePublic) {
                return Status::InvalidArgument("relation '" +
                                                std::string(NameView(row.name)) +
                                                "' is a system relation and cannot be renamed");
            }
            SetName(row.name, new_name);
            const auto encoded = row.Encode();
            if (Status s = OverwriteLogged(wal_, store_, page, page_id, i, encoded, wal::kNoTxnId, tuple.trx_id, tuple.undo_ptr);
                !s.ok()) {
                return s;
            }
            return true;
        });
    if (!acted.ok()) return acted.status();
    if (!acted.value()) return Status::NotFound("no sys.objects row for this relation");

    BumpVersion("rename table");
    return Status::OK();
}

Status Catalog::RenameColumn(Oid table_oid, std::string_view old_name,
                             std::string_view new_name) {
    if (Status s = CheckRenameName("column", new_name); !s.ok()) return s;

    // Sibling collision and old-name existence in one read, before any
    // write - the same core-local atomicity argument RenameTable makes.
    auto rows = ScanAll<SysColumnRow>(store_, kCatalogPageColumns, nullptr, txn_);
    if (!rows.ok()) return rows.status();
    bool found_old = false;
    for (const SysColumnRow& row : rows.value()) {
        if (row.rel_id != table_oid) continue;
        if (NameView(row.name) == new_name) {
            return Status::AlreadyExists("the relation already has a column named '" +
                                          std::string(new_name) + "'");
        }
        if (NameView(row.name) == old_name) found_old = true;
    }
    if (!found_old) {
        return Status::NotFound("the relation has no column named '" + std::string(old_name) +
                                 "'");
    }

    auto acted = ForFirstRow<SysColumnRow>(
        store_, kCatalogPageColumns,
        [&](SysColumnRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
            if (row.rel_id != table_oid || NameView(row.name) != old_name) return false;
            SetName(row.name, new_name);
            const auto encoded = row.Encode();
            if (Status s = OverwriteLogged(wal_, store_, page, page_id, i, encoded, wal::kNoTxnId, tuple.trx_id, tuple.undo_ptr);
                !s.ok()) {
                return s;
            }
            return true;
        });
    if (!acted.ok()) return acted.status();
    if (!acted.value()) return Status::NotFound("the column vanished between check and write");

    BumpVersion("rename column");
    return Status::OK();
}

Status Catalog::DropTable(Oid table_oid, std::vector<std::uint64_t>& dropped_cabins,
                          std::uint64_t trx_id, std::vector<CatalogRowChange>* written) {
    // Transactional only when a caller supplies an id (DT5). Without one
    // this is the path that always existed: retype, then **retire** the
    // dependents outright, which no rollback can put back - and which is
    // right for autocommit, where there is no rollback to serve.
    const bool transactional = trx_id != kBootstrapXid;
    auto note = [written](const CatalogRowChange& change) {
        if (written != nullptr) written->push_back(change);
    };
    // 1. The tombstone (DT2): retype, never retire. The row is the oid
    //    floor's evidence, and a reissued oid could serve a dead table's
    //    row as a live answer through a stale advisory structure.
    PageId retyped_page = kInvalidPageId;
    CatalogRowChange retype_change;
    auto retyped = ForFirstRow<SysObjectRow>(
        store_, kCatalogPageObjects,
        [&](SysObjectRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
            if (row.type_oid != kTypeTable || row.oid != table_oid) return false;
            if (row.namespace_oid != kNamespacePublic) {
                return Status::InvalidArgument("relation '" +
                                                std::string(NameView(row.name)) +
                                                "' is a system relation and cannot be dropped");
            }
            // The before-image, captured *before* the overwrite: it is
            // what a rollback writes back, and it is the only copy - a
            // catalog row has no undo chain to recover it from.
            CatalogRowChange change;
            change.slot = i;
            change.oid = row.oid;
            change.rel_oid = kSysObjectsTable;
            change.prior_trx_id = tuple.trx_id;
            change.prior_undo_ptr = tuple.undo_ptr;
            const auto before = row.Encode();
            change.prior_image.assign(before.begin(), before.end());

            row.type_oid = kTypeDroppedTable;
            const auto encoded = row.Encode();
            // The undo record first (hook contract, catalog.hpp): the
            // prior image is the only copy a crash leaves of these bytes.
            if (ddl_undo_hook_) {
                if (Status s = ddl_undo_hook_({DdlUndoEvent::Kind::kOverwrite, page_id, i,
                                               change.prior_image, tuple.trx_id,
                                               tuple.undo_ptr,
                                               UndoPkOf(change.prior_image)});
                    !s.ok()) {
                    return s;
                }
            }
            // Stamped with the dropping transaction when there is one, so
            // the retype is this transaction's to undo.
            if (Status s = OverwriteLogged(wal_, store_, page, page_id, i, encoded,
                                           transactional ? trx_id : wal::kNoTxnId,
                                           transactional ? trx_id : tuple.trx_id,
                                           tuple.undo_ptr);
                !s.ok()) {
                return s;
            }
            retype_change = std::move(change);
            return true;
        },
        &retyped_page);
    if (!retyped.ok()) return retyped.status();
    if (!retyped.value()) return Status::NotFound("no sys.objects row for this relation");
    if (transactional) {
        retype_change.page_id = retyped_page;  // known only once the walk found it
        note(retype_change);
    }

    // 2. Everything the relation owns retires (DT3's "dependents out").
    //    Retired, not delete-marked - RetirePattern() states the argument:
    //    catalog reads have no snapshot to filter a mark against. Each
    //    sweep loops ForFirstRow until nothing matches, because a relation
    //    owns many columns and may own several indexes and cabins.
    // One loop shape, four row types: the tag parameter names the type and
    // the predicate is the one line that differs per sweep.
    const auto sweep = [&](auto row_tag, PageId root, auto matches) -> Status {
        using RowT = decltype(row_tag);
        for (;;) {
            PageId acted_page = kInvalidPageId;
            CatalogRowChange marked;
            auto acted = ForFirstRow<RowT>(
                store_, root,
                [&](RowT& row, heap::PageView& page, PageId page_id, std::uint16_t i,
                    const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
                    if (!matches(row)) return false;
                    // **Already marked is already done.** The sweep loops
                    // until nothing matches, and a retired slot stops
                    // matching because it is gone - a delete-marked one
                    // does not. Without this the loop never terminates.
                    if (tuple.deleted) return false;
                    if (!transactional) {
                        if (Status s = RetireLogged(wal_, store_, page, page_id, i, trx_id);
                            !s.ok()) {
                            return s;
                        }
                        return true;
                    }
                    // Delete-marked, not retired: a retired slot has no
                    // compensation that puts it back, and `ScanAll` now
                    // knows how to read a mark (DT5). The row's bytes stay
                    // where they are, which is what a rollback needs and
                    // what nothing purges.
                    if (ddl_undo_hook_) {
                        if (Status s = ddl_undo_hook_({DdlUndoEvent::Kind::kDeleteMark, page_id,
                                                       i, {}, tuple.trx_id, tuple.undo_ptr,
                                                       UndoPkOf(tuple.payload)});
                            !s.ok()) {
                            return s;
                        }
                    }
                    if (Status s = DeleteMarkLogged(wal_, store_, page, page_id, i, trx_id);
                        !s.ok()) {
                        return s;
                    }
                    ++pending_marks_;
                    CatalogRowChange change;
                    change.slot = i;
                    // The identity `Compensate` will re-read before it
                    // clears the mark, taken the way that check takes it -
                    // not from a `.oid` member, which not every catalog row
                    // has.
                    if (auto id = KeystoneIdOfPayload(tuple.payload); id.ok()) {
                        change.oid = static_cast<Oid>(id.value());
                    }
                    change.prior_trx_id = tuple.trx_id;
                    change.prior_undo_ptr = tuple.undo_ptr;
                    change.deleted = true;
                    marked = std::move(change);
                    return true;
                },
                &acted_page);
            if (!acted.ok()) return acted.status();
            if (!acted.value()) return Status::OK();
            if (transactional) {
                marked.page_id = acted_page;
                note(marked);
            }
        }
    };

    if (Status s = sweep(SysTableRow{}, kCatalogPageTables,
                         [&](const SysTableRow& r) { return r.oid == table_oid; });
        !s.ok()) {
        return s;
    }
    if (Status s = sweep(SysColumnRow{}, kCatalogPageColumns,
                         [&](const SysColumnRow& r) { return r.rel_id == table_oid; });
        !s.ok()) {
        return s;
    }
    if (Status s = sweep(SysIndexRow{}, kCatalogPageIndexes,
                         [&](const SysIndexRow& r) { return r.table_oid == table_oid; });
        !s.ok()) {
        return s;
    }
    if (Status s = sweep(SysFkeyRow{}, kCatalogPageFkeys,
                         [&](const SysFkeyRow& r) { return r.child_rel_oid == table_oid; });
        !s.ok()) {
        return s;
    }

    // Cabins: the ids are collected first so the in-memory store can
    // forget them - the catalog row is what makes the compiler emit a
    // probe, but the entry sets live beside the dispatcher.
    auto cabin_rows = ScanAll<SysCabinRow>(store_, kCatalogPageCabins, nullptr, txn_);
    if (!cabin_rows.ok()) return cabin_rows.status();
    for (const SysCabinRow& row : cabin_rows.value()) {
        if (row.rel_oid == table_oid) dropped_cabins.push_back(row.cabin_id);
    }
    if (Status s = sweep(SysCabinRow{}, kCatalogPageCabins,
                         [&](const SysCabinRow& r) { return r.rel_oid == table_oid; });
        !s.ok()) {
        return s;
    }

    BumpVersion("drop table");
    return Status::OK();
}

StatusOr<std::vector<SysObjectRow>> Catalog::ListTables(const txn::ReadView* view) {
    if (view == nullptr) {
        if (const std::vector<SysObjectRow>* cached = cache_.FindTableList();
            cached != nullptr) {
            return *cached;
        }
    }

    auto rows = ScanAll<SysObjectRow>(store_, kCatalogPageObjects, view, txn_);
    if (!rows.ok()) return rows.status();

    std::vector<SysObjectRow> tables;
    for (auto& row : rows.value()) {
        if (row.type_oid == kTypeTable) tables.push_back(row);
    }
    // The same rule as the name lookup: a transactional list is this
    // reader's list and is not published to the instance.
    if (view != nullptr) return tables;
    return *cache_.PutTableList(std::move(tables));
}

StatusOr<Schema> Catalog::BuildSchemaFromColumns(Oid rel_id) {
    // Serves the cached copy when the relation has been opened before. The
    // return stays by value: this is the only caller-facing schema API that
    // hands out an owned Schema, DESCRIBE is not a hot path, and a copy
    // costs one vector allocation against the page scan below.
    if (const TableAccess* cached = cache_.FindTableAccess(rel_id); cached != nullptr) {
        return cached->schema;
    }
    return ScanSchemaFromColumns(rel_id);
}

StatusOr<Schema> Catalog::ScanSchemaFromColumns(Oid rel_id) {
    auto rows = ScanAll<SysColumnRow>(store_, kCatalogPageColumns, nullptr, txn_);
    if (!rows.ok()) return rows.status();

    Schema schema;
    for (const auto& row : rows.value()) {
        if (row.rel_id == rel_id) schema.columns.push_back(row);
    }

    if (schema.columns.empty()) {
        return Status::NotFound("no columns for this rel_id");
    }

    // ScanAll() returns rows in heap slot order, which happens to match
    // insertion order (CreateTable() inserts columns in position order)
    // but isn't guaranteed to stay that way once updates/compaction exist.
    // Row codec callers (src/exec/row_codec.cpp) depend on schema.columns
    // being in `pos` order to line up positionally with an INSERT's value
    // list, so pin that ordering explicitly here rather than leaning on
    // scan-order coincidence.
    std::sort(schema.columns.begin(), schema.columns.end(),
              [](const SysColumnRow& a, const SysColumnRow& b) { return a.pos < b.pos; });

    return schema;
}

StatusOr<const std::vector<SysTypeRow>*> Catalog::EnsureTypes() {
    if (const std::vector<SysTypeRow>* cached = cache_.FindTypes(); cached != nullptr) {
        return cached;
    }
    auto rows = ScanAll<SysTypeRow>(store_, kCatalogPageTypes, nullptr, txn_);
    if (!rows.ok()) return rows.status();
    return cache_.PutTypes(std::move(rows.value()));
}

StatusOr<SysTypeRow> Catalog::ResolveTypeByName(std::string_view name) {
    // One snapshot per process, and unlike every other cached fact its
    // *misses* are authoritative: only InsertTypeRow() writes sys.types and
    // it drops the snapshot, so "not in the snapshot" means "not in the
    // table" and needs no rescan. That takes the page read off the CREATE
    // TABLE error path too.
    auto types = EnsureTypes();
    if (!types.ok()) return types.status();

    for (const auto& row : *types.value()) {
        std::string_view row_name = NameView(row.name);
        if (row_name.size() == name.size() &&
            std::equal(row_name.begin(), row_name.end(), name.begin(), [](char x, char y) {
                return std::tolower(static_cast<unsigned char>(x)) ==
                       std::tolower(static_cast<unsigned char>(y));
            })) {
            return row;
        }
    }
    return Status::NotFound("unknown type '" + std::string(name) + "'");
}

StatusOr<const TableAccess*> Catalog::InitTableAccess(Oid oid) {
    if (const TableAccess* cached = cache_.FindTableAccess(oid); cached != nullptr) {
        return cached;
    }

    auto table_row = GetSysTableRow(oid);
    if (!table_row.ok()) return table_row.status();

    // Scan, not BuildSchemaFromColumns(): that one would probe the same
    // cache entry this call is in the middle of filling.
    auto schema = ScanSchemaFromColumns(oid);
    if (!schema.ok()) return schema.status();

    TableAccess access{};
    // The relation's namespace comes from its sys.tables row rather than
    // from the caller. It used to be a parameter echoed straight into the
    // result, which is untenable once the entry is shared: whichever caller
    // filled it first would decide the field for everyone else. CreateTable
    // already persists the right value.
    access.namespace_oid = table_row.value().namespace_oid;
    access.oid = oid;
    access.schema = std::move(schema.value());
    access.desc_page_id = table_row.value().desc_page_id;
    access.clustered_type = table_row.value().clustered_type;
    access.varheap_page_id = table_row.value().varheap_page_id;
    access.owner_core = table_row.value().owner_core;
    access.key_order = table_row.value().key_order;
    access.anchor_page_id = table_row.value().anchor_page_id;

    // PW2-2 (workplan-peer-writer.md §7a): the durable truth for a user
    // relation's clustered root is its **anchor page** - from PW2-3 on,
    // sys.tables.desc_page_id is CREATE-fixed and a root move writes the
    // anchor, so the row's value can be behind. Resolved here, at fill;
    // between fills the cached access is updated in place on a root move,
    // the same license the in-place root updates always had. A system
    // relation (no anchor) keeps the row's value: its fixed-page root
    // never moves. The fall-through arm is deliberate: a *foreign*
    // relation's anchor may be unfaultable on this core (no grant, or a
    // stale map) and its root is never walked here - execution ships to
    // the owner, whose own fill resolves through its own anchor - so only
    // Corruption is loud.
    // Scoped to this core's OWN relations (PW2-4, the C3 decision taken as
    // "owner-readable": the anchor lives above kCatalogOverflowLimit,
    // outside the invalidation set, so a cross-core reader's frame would
    // never refresh - and a foreign relation's root is never walked here
    // anyway, execution ships to the owner). A foreign relation keeps the
    // row's CREATE-time value, and diagnostics of a foreign relation show
    // exactly that - stated, build-invariant, no faultability probe.
    // Held for the whole fill (the 96b0343 review's C2): the first form
    // dropped the ref and re-fetched per index row - an N+1 that, under a
    // sized pool, could fail mid-fill and leave the access half-anchored
    // (clustered root from the anchor, index roots from the CREATE-time
    // rows - a silently wrong tree for maintenance to append into). One
    // read, one failure decision, no tear expressible.
    std::optional<storage::PageRef> anchor_held;
    if (access.anchor_page_id != kInvalidPageId && access.owner_core == core_id_) {
        auto anchor = store_.GetForRead(access.anchor_page_id);
        if (anchor.ok()) {
            if (Status s = storage::ValidatePageHeader(anchor.value().bytes(),
                                                       PageType::kAnchor);
                !s.ok()) {
                return s;
            }
            // Checked redundancy one level up (the f5686f8 review's C5): a
            // row naming another relation's anchor would silently walk the
            // wrong tree; the owner stamp is what refuses it.
            if (storage::GetOwnerOid(anchor.value().bytes()) != oid) {
                return Status::Corruption(
                    "anchor page " + std::to_string(access.anchor_page_id) +
                    " belongs to relation oid " +
                    std::to_string(storage::GetOwnerOid(anchor.value().bytes())) +
                    ", not oid " + std::to_string(oid));
            }
            access.desc_page_id = storage::AnchorClusteredRoot(anchor.value().bytes());
            anchor_held.emplace(std::move(anchor.value()));
        } else if (anchor.status().code() == StatusCode::kCorruption) {
            return anchor.status().WithContext(
                "resolving the clustered root through anchor page " +
                std::to_string(access.anchor_page_id) + " of this core's own relation");
        }
        // An own relation's *unfaultable* anchor is the pre-grant window
        // (P6's resolve-before-grant contract: a peer fills its relation's
        // access before any grant arrives), and it falls back to the
        // CREATE-time row - safe because nothing can have moved a root the
        // owner could not yet write, and self-healing because the grant
        // receiver drops this cache when the rights land (the f5686f8
        // review's C1, closed at the grant rather than by refusing the
        // fill). Corruption alone is loud.
    }

    // The row-size constant, computed once here and carried for the life of
    // the entry (invariant 13). This is the only place it is derived: a
    // second computation on an execute path is a second chance to disagree
    // with the bytes already on disk.
    auto layout = RowLayout::Build(access.schema, inline_cell_width_);
    if (!layout.ok()) return layout.status();
    access.layout = std::move(layout.value());

    // The relation's cabins, in **one** sys.cabins scan rather than one per
    // column. A DDL fact, so it belongs on this entry and dies with it
    // (schema.hpp says why it is a mask rather than a per-step probe).
    //
    // A failure here is not fatal to opening the relation: a Cabin the
    // compiler cannot see costs the acceleration and never a row, since the
    // step compiles to the scan it would have compiled to anyway. But an
    // unreadable catalog page is not something to swallow either, so it is
    // reported - the relation is opened by the same call that would then
    // fail on its first read.
    auto cabins = ListCabins();
    if (!cabins.ok()) return cabins.status();
    access.cabin_ids.assign(access.schema.columns.size(), TableAccess::CabinRef{});
    for (const SysCabinRow& cabin : cabins.value()) {
        if (cabin.rel_oid != oid) continue;
        if (!IsCabinServing(cabin)) continue;
        if (cabin.column_no == 0 || cabin.column_no >= access.cabin_ids.size()) continue;
        access.cabin_ids[cabin.column_no] = TableAccess::CabinRef{cabin.cabin_id, cabin.origin};
        if (cabin.column_no < 64) {
            access.cabin_mask |= (std::uint64_t{1} << cabin.column_no);
        }
    }

    // The relation's foreign keys at **both** ends, in one sys.fkeys scan
    // (schema.hpp says why both lists live here). Unlike the Cabin scan
    // above, a failure here is not a lost accelerator: a foreign key the
    // write path cannot see is a constraint that does not run, so an
    // unreadable sys.fkeys must fail opening the relation rather than open
    // it unconstrained.
    auto fkeys = ListForeignKeys();
    if (!fkeys.ok()) return fkeys.status();
    for (const SysFkeyRow& fk : fkeys.value()) {
        if (fk.child_rel_oid == oid) {
            access.fkeys_out.push_back(
                ForeignKeyRef{fk.fk_id, fk.parent_rel_oid, fk.child_column_no, fk.flags});
        }
        if (fk.parent_rel_oid == oid) {
            access.fkeys_in.push_back(
                ForeignKeyRef{fk.fk_id, fk.child_rel_oid, fk.child_column_no, fk.flags});
        }
    }

    // The relation's secondary indexes, in one sys.indexes scan
    // (docs/spec/index.md §12, workplan IX04).
    //
    // A failure here is fatal to opening the relation, and for the *fkeys*
    // reason rather than the Cabin one. An index the compiler cannot see
    // would only cost speed - but an index the **write hook** cannot see is
    // an entry never appended, and an index missing an entry is a row lost
    // to every later probe. Both halves read this one list, so it fails
    // shut.
    auto indexes = ListIndexes();
    if (!indexes.ok()) return indexes.status();
    for (const SysIndexRow& row : indexes.value()) {
        if (row.table_oid != oid) continue;

        TableAccess::IndexRef ref;
        ref.index_oid = row.index_oid;
        ref.root_page_id = row.root_page_id;
        // PW2-3: the anchor is the index root's truth too - the
        // sys.indexes row is CREATE-fixed, and a root move writes the
        // anchor slot. Read from the ref held across the whole fill, so
        // clustered and index roots come from one page state and a forged
        // count is as loud here as it is for the clustered arm. A slot
        // the anchor does not hold (an index seeded before anchors, never
        // moved) keeps the row's value.
        if (anchor_held.has_value()) {
            auto slot = storage::AnchorIndexRoot(anchor_held->bytes(), row.index_oid);
            if (!slot.ok()) return slot.status();
            if (slot.value() != kInvalidPageId) ref.root_page_id = slot.value();
        }
        ref.key_width = row.key_width;
        ref.entry_width = row.entry_width;
        ref.nkeys = row.nkeys;
        ref.ncovered = row.ncovered;
        ref.key_cols = row.key_cols;
        ref.covered_cols = row.covered_cols;
        access.indexes.push_back(ref);

        // Leading column only - schema.hpp says why "any indexed column"
        // would be the wrong question.
        const std::uint16_t leading = ref.leading_column();
        if (ref.nkeys > 0 && leading > 0 && leading < 64) {
            access.index_mask |= (std::uint64_t{1} << leading);
        }
    }
    // Creation order, so §9's lowest-oid tie-break is a property of the list
    // rather than of how the rows happened to land on the page.
    std::sort(access.indexes.begin(), access.indexes.end(),
              [](const TableAccess::IndexRef& a, const TableAccess::IndexRef& b) {
                  return a.index_oid < b.index_oid;
              });

    return cache_.PutTableAccess(std::move(access));
}

StatusOr<SysTypeRow> Catalog::ResolveTypeByVal(std::uint32_t type_val) {
    auto types = EnsureTypes();
    if (!types.ok()) return types.status();

    for (const auto& row : *types.value()) {
        if (row.type_val == type_val) return row;
    }
    return Status::NotFound("no sys.types row for this type_val");
}

StatusOr<std::uint64_t> Catalog::AllocateRowIdRange(Oid table_oid, std::uint64_t count) {
    if (count == 0) return Status::InvalidArgument("a row-id range of zero has no first id");
    std::uint64_t first = 0;
    auto acted = ForFirstRow<SysTableRow>(
        store_, kCatalogPageTables,
        [&](SysTableRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
            if (row.oid != table_oid) return false;
            // No key-mode refusal: a carve is legal on every relation now,
            // because every relation's omitted-pk inserts draw from this same
            // mark. What a carve costs is stated at the call site and in
            // section 4.1 - the ids inside it are spent from the mark's point
            // of view before they are placed, so a *supplied* id landing
            // inside a live carve collides with whichever of the two lands
            // second. On a heap relation that cannot happen (a supplied id
            // must be at or above the mark, which the carve has already moved
            // past its own block); on a btree one the descent reports it as
            // the duplicate it is.
            //
            // Exhaustion checked against the range's *last* id: a range
            // that would cross the ceiling is refused whole, never split.
            if (row.next_id > kMaxKeystoneId - (count - 1)) {
                return Status::OutOfRange("relation has exhausted the Keystone id space");
            }
            first = row.next_id;
            // Bumped and persisted before anything is placed, exactly as
            // the single-id allocator: an abort burns the range, which K3
            // calls free and BI9's class already accepted
            // (docs/inflight/in-progress/workplan-t3.md T3-3).
            row.next_id = first + count;
            const auto encoded = row.Encode();
            if (Status s = OverwriteLogged(wal_, store_, page, page_id, i, encoded, wal::kNoTxnId, tuple.trx_id, tuple.undo_ptr);
                !s.ok()) {
                return s;
            }
            return true;
        });
    if (!acted.ok()) return acted.status();
    if (!acted.value()) return Status::NotFound("no sys.tables row for this relation");
    return first;
}

StatusOr<std::uint64_t> Catalog::AllocateRowId(Oid table_oid) {
    // A core with leases installed may not write the catalog page this
    // function would otherwise bump - it draws from its per-relation block
    // instead, and a spent block is retryable exhaustion, not OutOfRange
    // (catalog/row_id_lease.hpp).
    if (row_id_leases_ != nullptr) {
        auto id = row_id_leases_->Next(table_oid);
        if (id.ok() && log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
            log_->Trace("catalog", "issued leased row id " + std::to_string(id.value()) +
                                       " for table oid " + std::to_string(table_oid));
        }
        return id;
    }
    std::uint64_t issued = 0;
    auto acted = ForFirstRow<SysTableRow>(
        store_, kCatalogPageTables,
        [&](SysTableRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
        if (row.oid != table_oid) return false;

        // No key-mode refusal: this is what every INSERT that omits the pk
        // calls, whatever the relation. The id it hands out is safe from a
        // caller-supplied one for the same reason it always was - the mark
        // only ever moves forward, and AdmitExplicitRowId moves it past every
        // supplied id at or above it. A supplied id *below* the mark is a
        // value this function has already issued, and the btree descent that
        // admits it is what proves the row is not there twice.
        const std::uint64_t id = row.next_id;
        if (id > kMaxKeystoneId) {
            // The sequence is exhausted, not wrapped: reissuing from the
            // bottom would hand out an id that is still some tuple's
            // identity. Id-reuse / low-range reclamation is an open
            // decision (CLAUDE.md), so this refuses rather than picking one.
            return Status::OutOfRange("relation has exhausted the Keystone id space");
        }

        // Bumped and persisted before the caller inserts anything. A crash
        // between here and the insert burns an id, which is harmless - the
        // sequence only has to be unique and monotonic, never gapless. The
        // reverse order would reissue an id after a crash, which is not.
        row.next_id = id + 1;
        auto encoded = row.Encode();
        if (Status s = OverwriteLogged(wal_, store_, page, page_id, i, encoded, wal::kNoTxnId, tuple.trx_id, tuple.undo_ptr); !s.ok()) {
            return s;
        }
        // One line per issued id: the sequence is the tuple identity
        // (invariant 10), so "which id did this row get" is a question
        // that gets asked about every insert that later looks wrong.
        if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
            log_->Trace("catalog", "issued row id " + std::to_string(id) + " for table oid " +
                                       std::to_string(table_oid));
        }
        issued = id;
        return true;
    });
    if (!acted.ok()) return acted.status();
    if (!acted.value()) return Status::NotFound("no sys.tables row for this oid");
    return issued;
}

Status Catalog::AdmitExplicitRowId(Oid table_oid, std::uint64_t id) {
    // Spellability first, before the catalog page is touched at all: an id
    // outside the Keystone field cannot be stored by any path, so there is
    // nothing to check a relation for.
    if (id < kFirstRowId) {
        return Status::InvalidArgument("primary key " + std::to_string(id) +
                                       " is below the first issuable id (" +
                                       std::to_string(kFirstRowId) +
                                       "); 0 is reserved for \"unset\"");
    }
    if (id > kMaxKeystoneId) {
        return Status::InvalidArgument("primary key " + std::to_string(id) +
                                       " does not fit the 40-bit Keystone id space (max " +
                                       std::to_string(kMaxKeystoneId) + ")");
    }

    bool flipped_order = false;
    auto acted = ForFirstRow<SysTableRow>(
        store_, kCatalogPageTables,
        [&](SysTableRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
            if (row.oid != table_oid) return false;

            if (id < row.next_id) {
                // ---- Below the mark ------------------------------------
                //
                // A heap relation cannot take it. Its chain grows at the
                // tail, so an id below the tail page's min_key has no legal
                // page (invariant 3) - and worse than the placement, its
                // duplicate check reads the tail page alone, which is sound
                // only while every earlier page's ids sit below the tail's
                // bound. Both are the ascent. The mark is exactly the ascent
                // expressed as one number, so refusing here is what keeps
                // section 3.1b true rather than a second rule that could
                // drift from it (heap-and-tuple.md section 3.1b, 4.1).
                if (row.clustered_type != ClusteredType::kBtree) {
                    return Status::OutOfRange(
                        "primary key " + std::to_string(id) + " is below relation " +
                        std::to_string(table_oid) + "'s high-water mark " +
                        std::to_string(row.next_id) +
                        "; a heap-clustered relation's ids must ascend, because its chain "
                        "grows only at its tail - use BTREE to name keys in any order");
                }

                // A btree relation takes it: the descent that follows lands
                // on the one leaf that may hold the key and proves it unused,
                // so the mark is asked nothing. The mark itself does not
                // move - it is a ceiling on what has been placed, and this id
                // is under it.
                //
                // What *does* move, once per relation ever, is the order
                // flag: from here on a page's slot order is not its key
                // order, so ORDER BY <pk> can no longer be discarded
                // (well_known.hpp's KeyOrder). Guarded on the current value
                // so a backfill of old ids writes the catalog page once, not
                // once per row.
                if (row.key_order == KeyOrder::kUnordered) return true;
                row.key_order = KeyOrder::kUnordered;
                auto reordered = row.Encode();
                if (Status s = OverwriteLogged(wal_, store_, page, page_id, i, reordered,
                                               wal::kNoTxnId, tuple.trx_id, tuple.undo_ptr);
                    !s.ok()) {
                    return s;
                }
                flipped_order = true;
                if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
                    log_->Trace("catalog", "table oid " + std::to_string(table_oid) +
                                               " took primary key " + std::to_string(id) +
                                               " below its high-water mark " +
                                               std::to_string(row.next_id) +
                                               "; key order is now unordered");
                }
                return true;
            }

            // At or above: the mark moves past it, persisted before the
            // caller places anything. Same ordering as AllocateRowId and the
            // same reason - a crash between here and the insert leaves a
            // ceiling that is too high, which burns ids (K3 calls that free)
            // where the reverse would leave one too low, and a too-low mark
            // is how the engine later issues an id that is already a tuple's
            // identity.
            row.next_id = id + 1;
            auto encoded = row.Encode();
            if (Status s = OverwriteLogged(wal_, store_, page, page_id, i, encoded, wal::kNoTxnId, tuple.trx_id, tuple.undo_ptr); !s.ok()) {
                return s;
            }
            if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
                log_->Trace("catalog", "admitted explicit row id " + std::to_string(id) +
                                           " for table oid " + std::to_string(table_oid) +
                                           ", high-water now " + std::to_string(row.next_id));
            }
            return true;
        });
    if (!acted.ok()) return acted.status();
    if (!acted.value()) return Status::NotFound("no sys.tables row for this oid");
    // Publishing the flip: **in place here, invalidation everywhere else.**
    // Neither half is optional and they are optional for opposite reasons.
    //
    // In place locally, because this runs inside an ordinary INSERT and
    // `BumpVersion`'s `cache_.Invalidate()` would destroy the
    // `const TableAccess*` that statement is holding - the collateral damage
    // catalog_cache.hpp's four other in-place updates were each written to
    // avoid. The first form of this flip did bump, and the dangling access
    // re-read the pk out of a freed vector: a second insert on one relation
    // answered "tuple's Keystone id N does not match the id being inserted".
    //
    // But **another core reads this field**, which is where it parts company
    // with the index root and the desc page - those belong to one owner and
    // nobody else looks. `key_order` is read by `CompileStepChain` on the
    // *session's* core, which for a cross-core read is not the owner, and a
    // stale kAscending there discards an `ORDER BY <pk>` this relation now
    // needs. Worse, the elision is exactly what makes such a statement
    // shippable (`session_step_client.cpp` refuses a sorted chain but not an
    // elided one), so the stale read would answer out of order rather than
    // refuse. Hence the version bump and the peer notification, without the
    // local drop - and in that order, `BumpVersion`'s rule verbatim: a peer
    // must never re-read while this instance still holds a stale entry.
    //
    // Affordable because it happens **once per relation, ever**. `next_id`'s
    // advance - once per ascending key - is not cached and publishes nothing.
    if (flipped_order) {
        cache_.MarkKeysUnordered(table_oid);
        ++catalog_version_;
        if (on_invalidate_) on_invalidate_();
    }
    return Status::OK();
}

Status Catalog::UpdateRelationDescPage(Oid table_oid, PageId new_desc_page_id,
                                       PageId anchor_page_id) {
    // PW2-3/PW2-4: a root move writes the **anchor alone** - the
    // sys.tables row is CREATE-fixed, which is the whole point of the
    // indirection (a growth writes a relation page, never a catalog page;
    // on a peer, its own granted page in its own stream). The anchor id
    // comes from the caller's own TableAccess, UpdateIndexRoot's S2 rule -
    // no catalog scan inside a level-grow - and the cached entry updates
    // **in place**: the pre-anchor BumpVersion here destroyed the entry
    // the running INSERT was holding, and on a peer it would have
    // broadcast a cluster-wide invalidation per split. Same one-field/
    // one-owner license as the index root's in-place update. A system
    // relation (no anchor) cannot reach this function - its fixed catalog
    // page never grows a level - so an anchorless call is a defect.
    if (anchor_page_id == kInvalidPageId) {
        return Status::InvalidArgument(
            "relation oid " + std::to_string(table_oid) +
            " has no anchor page; a system relation's root cannot move");
    }
    if (Status s = WriteAnchorRoot(anchor_page_id, table_oid, /*index_oid=*/0,
                                   new_desc_page_id, wal::kNoTxnId);
        !s.ok()) {
        return s;
    }
    cache_.UpdateDescPage(table_oid, new_desc_page_id);
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        // A relation's entry page moving is a structural change to the
        // relation, rare enough to deserve Debug rather than Trace.
        log_->Debug("catalog", "table oid " + std::to_string(table_oid) + " root -> " +
                                   std::to_string(new_desc_page_id) + " (anchor " +
                                   std::to_string(anchor_page_id) + ")");
    }
    return Status::OK();
}

// ---- sys.patterns ----------------------------------------------------

namespace {

// The identity + location + policy halves of a pattern row - everything DDL
// alone can change. Heat is dropped on the floor here on purpose; see
// schema.hpp.
PatternAccess AccessOf(const SysPatternRow& row) noexcept {
    PatternAccess access{};
    access.oid = row.oid;
    access.pattern_id = row.pattern_id;
    access.fingerprint_version = row.fingerprint_version;
    access.waystone_root = row.waystone_root;
    access.stmt_class = row.stmt_class;
    access.dir_depth = row.dir_depth;
    access.origin = row.origin;
    access.flags = row.flags;
    return access;
}

// Whether a stored root/depth pair may be written. Checked before the page
// is touched, because a half-written pair is the one outcome that leaves a
// pattern's waystones unreachable.
Status CheckWaystonePair(PageId root, std::uint8_t depth) {
    if (depth == 0) {
        if (root != kInvalidPageId) {
            return Status::InvalidArgument(
                "catalog: a pattern with no waystone directory must carry no root");
        }
        return Status::OK();
    }
    if (root == kInvalidPageId) {
        return Status::InvalidArgument("catalog: a waystone directory needs a root page");
    }
    if (depth > kMaxPatternDirDepth) {
        return Status::InvalidArgument("catalog: waystone directory depth " +
                                       std::to_string(depth) + " exceeds " +
                                       std::to_string(kMaxPatternDirDepth));
    }
    return Status::OK();
}

}  // namespace

StatusOr<const std::vector<SysTypeRow>*> Catalog::ListTypes() { return EnsureTypes(); }

StatusOr<std::vector<SysPatternRow>> Catalog::ListPatterns() {
    return ScanAll<SysPatternRow>(store_, kCatalogPagePatterns, nullptr, txn_);
}

StatusOr<SysPatternRow> Catalog::GetSysPatternRow(std::uint64_t pattern_id) {
    auto rows = ScanAll<SysPatternRow>(store_, kCatalogPagePatterns, nullptr, txn_);
    if (!rows.ok()) return rows.status();

    for (const auto& row : rows.value()) {
        if (row.pattern_id != pattern_id) continue;
        // Rows from another fingerprint revision are invisible here, and
        // this is the only place that decision is made. Putting the filter
        // in the row lookup rather than in each caller is what stops a
        // stale row from shadowing a current one when both are on the page
        // - which is the state a version bump leaves behind, since nothing
        // rewrites the old rows.
        if (!parser::IsCurrentFingerprintVersion(row.fingerprint_version)) continue;
        return row;
    }
    return Status::NotFound("no current sys.patterns row for this pattern_id");
}

StatusOr<const PatternAccess*> Catalog::FindPattern(std::uint64_t pattern_id) {
    if (const PatternAccess* cached = cache_.FindPattern(pattern_id); cached != nullptr) {
        return cached;
    }

    // Version filtering lives in GetSysPatternRow(), so a row from another
    // revision arrives here as NotFound and never reaches the cache. That
    // ordering is the point: the cache holds current-version entries only,
    // by construction rather than by a check every reader has to remember.
    auto row = GetSysPatternRow(pattern_id);
    if (!row.ok()) return row.status();

    return cache_.PutPattern(AccessOf(row.value()));
}

StatusOr<const PatternAccess*> Catalog::RegisterPattern(std::uint64_t pattern_id,
                                                         std::uint8_t stmt_class,
                                                         std::uint8_t origin,
                                                         std::uint16_t flags) {
    if (origin != kOriginAuto && origin != kOriginUser) {
        return Status::InvalidArgument("catalog: unknown pattern origin");
    }

    // Read the page, not the cache: absences are never cached, so a cache
    // miss says nothing about whether the row exists. A row left behind by
    // an older revision does not count as existing - GetSysPatternRow()
    // does not return one - so a version bump leaves every shape
    // re-learnable rather than permanently blocked. The stale row stays
    // where it is; reclaiming it is retention's job (P15), and rewriting it
    // here would be an overwrite of a live tuple for no gain.
    if (GetSysPatternRow(pattern_id).ok()) {
        return Status::AlreadyExists("catalog: this pattern is already registered");
    }

    auto oid = AllocateRowId(kSysPatternsTable);
    if (!oid.ok()) return oid.status();

    SysPatternRow row{};
    row.oid = oid.value();
    row.pattern_id = pattern_id;
    row.last_seen = 0;
    row.fingerprint_version = parser::kFingerprintVersion;
    row.waystone_root = kInvalidPageId;
    row.use_count = 0;
    row.stmt_class = stmt_class;
    // No directory until one is built. A declared pattern gets its
    // directory immediately afterwards, through SetPatternWaystoneRoot(),
    // because `expected_instances` is a pre-sizing knob and pre-sizing a
    // directory that does not exist yet means nothing; an auto pattern
    // waits for its first trail. Either way this row is written with no
    // directory, so the root/depth pair is only ever set by its one writer.
    row.dir_depth = 0;
    row.origin = origin;
    row.flags = flags;

    if (Status s = InsertRow(wal_, ddl_undo_hook_, store_, kCatalogPagePatterns, row, kBootstrapXid); !s.ok()) {
        return s;
    }
    // No BumpVersion(): nothing cached can go stale from a pattern
    // appearing (catalog.hpp states the argument, and the statement path
    // depends on it).
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "registered pattern " + std::to_string(pattern_id) + " as oid " +
                                   std::to_string(row.oid));
    }
    return cache_.PutPattern(AccessOf(row));
}

Status Catalog::MutatePatternRow(std::uint64_t pattern_id,
                                  const std::function<void(SysPatternRow&)>& mutate) {
    auto acted = ForFirstRow<SysPatternRow>(
        store_, kCatalogPagePatterns,
        [&](SysPatternRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
        if (row.pattern_id != pattern_id) return false;
        // The same version filter GetSysPatternRow() applies, and for the
        // same reason: a row from another revision names a shape that is
        // not the one it claims, so it is not this pattern and must not be
        // written through.
        if (!parser::IsCurrentFingerprintVersion(row.fingerprint_version)) return false;

        mutate(row);
        auto encoded = row.Encode();
        // In place, never delete+insert: the row size is unchanged, and a
        // pattern that briefly does not exist is a pattern a concurrent
        // lookup misses.
        if (Status s = OverwriteLogged(wal_, store_, page, page_id, i, encoded, wal::kNoTxnId, tuple.trx_id, tuple.undo_ptr); !s.ok()) {
            return s;
        }
        return true;
    });
    if (!acted.ok()) return acted.status();
    if (!acted.value()) return Status::NotFound("no sys.patterns row for this pattern_id");
    return Status::OK();
}

Status Catalog::SetPatternWaystoneRoot(std::uint64_t pattern_id, PageId root,
                                        std::uint8_t depth) {
    if (Status s = CheckWaystonePair(root, depth); !s.ok()) return s;

    Status s = MutatePatternRow(pattern_id, [root, depth](SysPatternRow& row) {
        row.waystone_root = root;
        row.dir_depth = depth;
    });
    if (!s.ok()) return s;

    // Updated in place rather than invalidated, and only on success: a
    // failed overwrite moved nothing, and publishing the new pair into the
    // cache would make it disagree with the page.
    cache_.UpdatePatternWaystone(pattern_id, root, depth);
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "pattern " + std::to_string(pattern_id) +
                                   " waystone root=" + std::to_string(root) +
                                   " depth=" + std::to_string(depth));
    }
    return Status::OK();
}

Status Catalog::SetPatternOrigin(std::uint64_t pattern_id, std::uint8_t origin,
                                  std::uint16_t flags) {
    if (origin != kOriginAuto && origin != kOriginUser) {
        return Status::InvalidArgument("catalog: unknown pattern origin");
    }

    Status s = MutatePatternRow(pattern_id, [origin, flags](SysPatternRow& row) {
        row.origin = origin;
        row.flags = flags;
    });
    if (!s.ok()) return s;

    // **`waystone_root` and `dir_depth` are deliberately untouched.** This
    // is the adoption path, and adopting an auto-registered pattern must
    // not throw away a warm cache: the trails recorded under it are still
    // trails for the same shape, and the directory that indexes them is
    // still the right depth for the traffic that built it.
    cache_.UpdatePatternOrigin(pattern_id, origin, flags);
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "pattern " + std::to_string(pattern_id) + " origin=" +
                                   std::to_string(origin) + " flags=" + std::to_string(flags));
    }
    return Status::OK();
}

Status Catalog::TouchPattern(std::uint64_t pattern_id, std::uint64_t last_seen) {
    return MutatePatternRow(pattern_id, [last_seen](SysPatternRow& row) {
        // Saturating, not wrapping: a use_count that rolled over would make
        // the hottest pattern in the database look like the coldest, which
        // is the one reading retention must never be handed.
        if (row.use_count != std::numeric_limits<std::uint32_t>::max()) ++row.use_count;
        row.last_seen = last_seen;
    });
    // No cache update: heat is not a cached fact (catalog.hpp), so there is
    // nothing to keep coherent.
}

Status Catalog::RetirePattern(std::uint64_t pattern_id) {
    auto acted = ForFirstRow<SysPatternRow>(
        store_, kCatalogPagePatterns,
        [&](SysPatternRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple&) -> StatusOr<bool> {
        if (row.pattern_id != pattern_id) return false;
        if (!parser::IsCurrentFingerprintVersion(row.fingerprint_version)) return false;

        if (Status s = RetireLogged(wal_, store_, page, page_id, i, kBootstrapXid); !s.ok()) {
            return s;
        }

        // The cache holds an entry keyed on this pattern_id, and it is now
        // a fact about a row that no longer exists. There is no
        // "UpdatePattern..." for removal, so this drops everything - which
        // is heavier than the in-place updates beside it and deliberately
        // so: a dangling PatternAccess would outlive its row, and DROP is
        // rare enough that one re-scan costs nothing worth protecting.
        BumpVersion("sys.patterns retire");
        if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
            log_->Debug("catalog", "retired pattern " + std::to_string(pattern_id));
        }
        return true;
    });
    if (!acted.ok()) return acted.status();
    if (!acted.value()) return Status::NotFound("no sys.patterns row for this pattern_id");
    return Status::OK();
}

Status Catalog::RecordAccess(std::uint8_t kind, Oid rel_id, std::uint64_t column_mask,
                              std::uint64_t last_seen) {
    if (kind == kAccessKindUnset) {
        return Status::InvalidArgument("catalog: access kind 0 is reserved for an unset row");
    }

    // `live` counts every row the walk passed, across every page of the
    // chain - which is what the cap below has to be measured against now
    // that the relation is not one page.
    std::size_t live = 0;
    auto acted = ForFirstRow<SysAccessStatRow>(
        store_, kCatalogPageAccessStats,
        [&](SysAccessStatRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
        ++live;
        if (row.kind != kind || row.rel_id != rel_id || row.column_mask != column_mask) {
            return false;
        }

        // Saturating for the reason rows.hpp gives: a wrapped count would
        // invert the ranking this exists to produce.
        if (row.use_count != std::numeric_limits<std::uint64_t>::max()) ++row.use_count;
        row.last_seen = last_seen;
        auto encoded = row.Encode();
        // In place - the row size is fixed, so there is nothing to move.
        if (Status s = OverwriteLogged(wal_, store_, page, page_id, i, encoded, wal::kNoTxnId, tuple.trx_id, tuple.undo_ptr); !s.ok()) {
            return s;
        }
        return true;
    });
    if (!acted.ok()) return acted.status();
    if (acted.value()) return Status::OK();

    // A shape nobody has recorded. Admitted only under the cap: an
    // unbounded catalog relation written from the statement path is the
    // failure this guards, and refusing a *new* shape while continuing to
    // count every known one degrades the statistic rather than the server.
    if (live >= kMaxAccessShapes) {
        return Status::ResourceExhausted("catalog: sys.access_stats is at its shape cap");
    }

    SysAccessStatRow row{};
    row.rel_id = rel_id;
    row.column_mask = column_mask;
    row.use_count = 1;
    row.last_seen = last_seen;
    row.kind = kind;
    // No BumpVersion(): nothing cached is derived from these rows, and the
    // argument is the one sys.patterns' registration already makes - a
    // statistic appearing cannot stale a cached relation, and this runs on
    // the statement path where a cache drop would dangle a held pointer.
    return InsertRow(wal_, ddl_undo_hook_, store_, kCatalogPageAccessStats, row, kBootstrapXid);
}

StatusOr<std::uint64_t> Catalog::CreateCabin(Oid rel_oid, std::uint16_t col_pos,
                                              std::uint8_t origin) {
    if (origin != kCabinOriginAuto && origin != kCabinOriginUser) {
        return Status::InvalidArgument("catalog: unknown cabin origin");
    }
    if (col_pos == 0) {
        // Not a policy choice and not a limitation: the pk column's Cabin
        // is the clustered tree, which is already authoritative for every
        // value rather than for the observed ones (spec section 2).
        return Status::InvalidArgument(
            "catalog: the primary-key column needs no cabin - the clustered tree is its cabin");
    }

    // Through InitTableAccess() rather than the sys.columns scan, because
    // the column count has to come from the same schema the compiler will
    // see. A cabin on a column that relation does not have would compile to
    // nothing and be invisible until someone read the catalog by hand.
    auto access = InitTableAccess(rel_oid);
    if (!access.ok()) return access.status();
    if (col_pos >= access.value()->schema.columns.size()) {
        return Status::InvalidArgument("catalog: relation oid " + std::to_string(rel_oid) +
                                       " has no column at position " + std::to_string(col_pos));
    }

    // The column's declared policy, enforced here because this is the one
    // door every Cabin comes through - an operator's `CREATE CABIN`, a
    // `CABIN` clause at CREATE TABLE, and whatever auto-creation is built
    // later. `NO CABIN` means never, by any route (rows.hpp), and a check
    // that lived in the DDL layer instead would be a check the future
    // promotion pipeline could forget.
    const SysColumnRow& column = access.value()->schema.columns[col_pos];
    if (!CabinPolicyPermitsCreation(column.cabin_policy)) {
        return Status::InvalidArgument("catalog: column '" +
                                       std::string(NameView(column.name)) +
                                       "' was declared NO CABIN");
    }

    if (FindCabinOnColumn(rel_oid, col_pos).ok()) {
        return Status::AlreadyExists("catalog: this column already has a cabin");
    }

    auto cabin_id = AllocateRowId(kSysCabinsTable);
    if (!cabin_id.ok()) return cabin_id.status();

    SysCabinRow row{};
    row.cabin_id = cabin_id.value();
    row.rel_oid = rel_oid;
    // Nothing is observed by creating a Cabin: the read path's miss branch
    // fills it (spec section 4). A count written here would be a claim about
    // runtime state made by DDL.
    row.observed_ct = 0;
    row.column_no = col_pos;
    row.origin = origin;
    // Active immediately. kCabinStatusBuilding exists for a phase-2
    // background build and has no writer while the only way to fill a Cabin
    // is a statement that was going to scan anyway.
    row.status = kCabinStatusActive;

    if (Status s = InsertRow(wal_, ddl_undo_hook_, store_, kCatalogPageCabins, row, kBootstrapXid); !s.ok()) {
        return s;
    }

    // **Bumped, unlike RegisterPattern().** A pattern appearing stales
    // nothing cached; a Cabin appearing stales `TableAccess::cabin_mask` on
    // every held entry for this relation, which is a fact the compiler
    // reads. That is what makes this DDL and not a statement-path write.
    BumpVersion("sys.cabins create");
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("catalog", "created cabin " + std::to_string(row.cabin_id) + " on relation oid " +
                                  std::to_string(rel_oid) + " column " + std::to_string(col_pos));
    }
    return row.cabin_id;
}

Status Catalog::DropCabin(std::uint64_t cabin_id) {
    auto acted = ForFirstRow<SysCabinRow>(
        store_, kCatalogPageCabins,
        [&](SysCabinRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple&) -> StatusOr<bool> {
        if (row.cabin_id != cabin_id) return false;

        // Retired, not delete-marked - RetirePattern() states the argument,
        // and it is the same one: a catalog read has no snapshot to filter a
        // mark against, so a marked row would still be found by every lookup
        // and a re-created Cabin would collide with a row nobody can see.
        if (Status s = RetireLogged(wal_, store_, page, page_id, i, kBootstrapXid); !s.ok()) {
            return s;
        }

        BumpVersion("sys.cabins drop");
        if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
            log_->Info("catalog", "dropped cabin " + std::to_string(cabin_id));
        }
        return true;
    });
    if (!acted.ok()) return acted.status();
    if (!acted.value()) return Status::NotFound("no sys.cabins row for this cabin_id");
    return Status::OK();
}

StatusOr<std::vector<SysCabinRow>> Catalog::ListCabins() {
    return ScanAll<SysCabinRow>(store_, kCatalogPageCabins, nullptr, txn_);
}

StatusOr<SysCabinRow> Catalog::FindCabinOnColumn(Oid rel_oid, std::uint16_t col_pos) {
    auto rows = ListCabins();
    if (!rows.ok()) return rows.status();
    for (const SysCabinRow& row : rows.value()) {
        if (row.rel_oid == rel_oid && row.column_no == col_pos) return row;
    }
    return Status::NotFound("no cabin on this column");
}

StatusOr<std::uint64_t> Catalog::CreateForeignKey(Oid child_rel_oid, std::uint16_t child_column_no,
                                                  Oid parent_rel_oid, std::uint16_t flags) {
    auto child = InitTableAccess(child_rel_oid);
    if (!child.ok()) return child.status();
    auto parent = InitTableAccess(parent_rel_oid);
    if (!parent.ok()) return parent.status();

    if (child_column_no >= child.value()->schema.columns.size()) {
        return Status::InvalidArgument("catalog: relation oid " + std::to_string(child_rel_oid) +
                                       " has no column at position " +
                                       std::to_string(child_column_no));
    }

    // The declaration's own checks, shared with the CREATE TABLE pre-check
    // (foreign_key.hpp) so the two doors cannot answer differently.
    if (Status s = CheckForeignKeyDeclaration(*parent.value(),
                                              child.value()->schema.columns[child_column_no],
                                              child_column_no);
        !s.ok()) {
        return s.WithContext("catalog");
    }
    if (Status s = CheckForeignKeyColocation(*parent.value(), *child.value()); !s.ok()) {
        return s.WithContext("catalog");
    }

    // One foreign key per (child, column). A second would mean a column
    // referencing two parents, which under F1 - the value *is* a parent's
    // Keystone id - would require one id to name a row in both.
    if (FindForeignKeyOnColumn(child_rel_oid, child_column_no).ok()) {
        return Status::AlreadyExists("catalog: this column already has a foreign key");
    }

    auto fk_id = AllocateRowId(kSysFkeysTable);
    if (!fk_id.ok()) return fk_id.status();

    SysFkeyRow row{};
    row.fk_id = fk_id.value();
    row.child_rel_oid = child_rel_oid;
    row.parent_rel_oid = parent_rel_oid;
    row.child_column_no = child_column_no;
    row.flags = flags;
    // kFkNullable's one writer, stamped from the column's declaration - the
    // moment the fact is known and the only moment it can change (no ALTER
    // touches nullability). Enforcement never reads it: a NULL fk value
    // skips the probe by being a NULL, and a NOT NULL column refuses the
    // NULL in the codec before any row lands. The bit records the
    // declaration for display.
    if (!child.value()->schema.columns[child_column_no].notnull) {
        row.flags |= kFkNullable;
    }

    if (Status s = InsertRow(wal_, ddl_undo_hook_, store_, kCatalogPageFkeys, row, kBootstrapXid); !s.ok()) {
        return s;
    }

    // **Bumped**, and note which entries it has to reach: a new foreign key
    // stales `fkeys_out` on the child *and* `fkeys_in` on the parent, which
    // is a relation this call's arguments do not otherwise touch. That is
    // why this is a global invalidation and not an in-place update of the
    // child's cached entry.
    BumpVersion("sys.fkeys create");
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("catalog", "created foreign key " + std::to_string(row.fk_id) + " on relation "
                                  "oid " + std::to_string(child_rel_oid) + " column " +
                                  std::to_string(child_column_no) + " referencing relation oid " +
                                  std::to_string(parent_rel_oid));
    }
    return row.fk_id;
}

StatusOr<std::vector<SysFkeyRow>> Catalog::ListForeignKeys() {
    return ScanAll<SysFkeyRow>(store_, kCatalogPageFkeys, nullptr, txn_);
}

StatusOr<SysFkeyRow> Catalog::FindForeignKeyOnColumn(Oid child_rel_oid,
                                                     std::uint16_t child_column_no) {
    auto rows = ListForeignKeys();
    if (!rows.ok()) return rows.status();
    for (const SysFkeyRow& row : rows.value()) {
        if (row.child_rel_oid == child_rel_oid && row.child_column_no == child_column_no) {
            return row;
        }
    }
    return Status::NotFound("no foreign key on this column");
}

StatusOr<std::vector<SysAccessStatRow>> Catalog::ListAccessStats() {
    return ScanAll<SysAccessStatRow>(store_, kCatalogPageAccessStats, nullptr, txn_);
}

Status Catalog::CheckIndexDef(const IndexDef& def, AnchorSeed seed) {
    if (def.key_cols.empty()) {
        return Status::InvalidArgument("catalog: an index needs at least one key column");
    }
    // A cap **refuses** and never truncates (spec §11): a truncated index
    // declared complete is a wrong answer with a right answer's shape. That
    // is the opposite of a Cabin cap, which may decline because a Cabin is
    // only ever a shortcut.
    if (def.key_cols.size() > kMaxIndexKeyColumns) {
        return Status::InvalidArgument("catalog: an index takes at most " +
                                        std::to_string(kMaxIndexKeyColumns) +
                                        " key columns, this one declares " +
                                        std::to_string(def.key_cols.size()));
    }
    if (def.covered_cols.size() > kMaxIndexCoveredColumns) {
        return Status::InvalidArgument("catalog: an index covers at most " +
                                        std::to_string(kMaxIndexCoveredColumns) +
                                        " columns, this one declares " +
                                        std::to_string(def.covered_cols.size()));
    }
    if ((def.flags & kIndexFlagUnique) != 0) {
        return Status::Unsupported(
            "catalog: UNIQUE indexes are not supported (docs/spec/index.md IX11); v1 is a read "
            "accelerator that cannot fail a write for a reason of its own");
    }

    auto access = InitTableAccess(def.table_oid);
    if (!access.ok()) return access.status();
    const Schema& schema = access.value()->schema;

    // The owner refusal lives here, not only in the dispatcher (the
    // 96b0343 review's C4): since PW2-3's anchor seed, CreateIndex writes
    // a *relation* page, and this core writing a peer-owned relation's
    // anchor would make it the second writer of a page whose handoff
    // already granted the peer - PL §9 rule 5's mount refusal. This is
    // the door every non-DDL caller comes through, this file's own
    // doctrine; the dispatcher's PW1c-6 refusal stays for the byte
    // position. Keyed on the publish hook's presence beside ownership:
    // an installed publisher is what makes ownership a *handoff* fact -
    // the P4e harness builds indexed fixtures on rotated relations
    // through a hook-less catalog, where nothing was ever granted away
    // and core 0 is still the only writer (PW4's predicate-on-incapacity
    // rule, one layer down). A row whose seed is the owner's
    // (`kByOwner`) writes no relation page here, so the refusal has
    // nothing to guard.
    if (seed == AnchorSeed::kHere && access.value()->owner_core != core_id_ && on_publish_) {
        return Status::Unsupported(
            "catalog: relation oid " + std::to_string(def.table_oid) + " is owned by core " +
            std::to_string(access.value()->owner_core) + " and core " +
            std::to_string(core_id_) +
            " may not seed its anchor (workplan-peer-writer.md PW1c-6)");
    }

    // A heap relation has no pk index, so resolving an entry's pk would be a
    // chain scan and an index over it would turn one full scan into N
    // partial ones. The same rule and the same argument as
    // foreign-keys.md F1's refusal of a heap parent.
    if (access.value()->clustered_type != ClusteredType::kBtree) {
        return Status::InvalidArgument(
            "catalog: relation oid " + std::to_string(def.table_oid) +
            " is heap-clustered; a secondary index resolves an entry through the primary key, "
            "which a heap relation has no index for (docs/spec/index.md IX3)");
    }

    const auto check_column = [&](std::uint16_t pos, const char* role) -> Status {
        if (pos >= schema.columns.size()) {
            return Status::InvalidArgument("catalog: " + std::string(role) + " column position " +
                                            std::to_string(pos) + " is past the relation's " +
                                            std::to_string(schema.columns.size()) + " columns");
        }
        return Status::OK();
    };
    for (std::size_t i = 0; i < def.key_cols.size(); ++i) {
        if (Status s = check_column(def.key_cols[i], "key"); !s.ok()) return s;
        // The pk is carried only by the Keystone word and the clustered tree
        // already indexes it; a second copy would be maintained forever to
        // answer questions the descent answers faster.
        if (def.key_cols[i] == 0) {
            return Status::InvalidArgument(
                "catalog: the primary key already has an index - the clustered tree is it");
        }
        // D2 (`docs/workplan-null.md`): a nullable key column is refused in
        // v1, so the entry format, maintenance and backfill keep the
        // property that every row has exactly one entry of a key that
        // always exists. IS NULL answers by scan; revisit with a measured
        // need, like index.md §13's other opens.
        if (!schema.columns[def.key_cols[i]].notnull) {
            return Status::Unsupported(
                "catalog: column '" +
                std::string(NameView(schema.columns[def.key_cols[i]].name)) +
                "' is nullable, and an index key must always exist (docs/spec/null.md D2; "
                "declare the column NOT NULL or leave it unindexed)");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (def.key_cols[j] == def.key_cols[i]) {
                return Status::InvalidArgument("catalog: column " +
                                                std::to_string(def.key_cols[i]) +
                                                " appears twice in the index key");
            }
        }
    }
    for (std::uint16_t pos : def.covered_cols) {
        if (Status s = check_column(pos, "covered"); !s.ok()) return s;
        // D2 covers covered columns too: an entry encodes their values
        // with no null bitmap of its own, so a NULL has no representation
        // there either.
        if (!schema.columns[pos].notnull) {
            return Status::Unsupported(
                "catalog: covered column '" + std::string(NameView(schema.columns[pos].name)) +
                "' is nullable, and an index entry has no NULL encoding (docs/spec/null.md D2)");
        }
    }

    if (FindIndexByName(def.name).ok()) {
        return Status::AlreadyExists("catalog: an index named '" + def.name + "' already exists");
    }

    // The anchor's entry table, asked **here** so that a refusal costs no
    // pages: both build paths reach the seed only after the whole tree is
    // allocated, and nothing in this engine frees one. This is the door
    // both of them already come through, and it is early enough on the
    // local arm that a refused declaration burns no index oid either -
    // `PrepareIndexDef` issues that after this returns.
    //
    // `kByOwner` asks nothing, and must not: the anchor is then another
    // core's page, which this core neither seeds nor may fault. The owner
    // asks for itself when it comes through here with its own `kHere`.
    if (seed == AnchorSeed::kHere) {
        if (Status s = CheckAnchorRoomForIndex(access.value()->anchor_page_id, def.table_oid,
                                               def.index_oid);
            !s.ok()) {
            return s;
        }
    }
    return Status::OK();
}

StatusOr<Oid> Catalog::CreateIndex(const IndexDef& def, std::uint64_t trx_id,
                                    CatalogRowRef* where, AnchorSeed seed) {
    // Re-checked here even when the caller already asked: this is the door
    // every non-DDL caller comes through, and a check that only runs when
    // someone remembers to ask is not a check.
    if (Status s = CheckIndexDef(def, seed); !s.ok()) return s;

    // A caller that formatted pages already pre-issued the oid to stamp
    // them (IndexDef::index_oid); allocate only when it did not.
    std::uint64_t issued = def.index_oid;
    if (issued == 0) {
        auto index_oid = AllocateRowId(kSysIndexesTable);
        if (!index_oid.ok()) return index_oid.status();
        issued = index_oid.value();
    }

    SysIndexRow row{};
    row.index_oid = issued;
    row.table_oid = def.table_oid;
    row.root_page_id = def.root_page_id;
    row.key_width = def.key_width;
    row.entry_width = def.entry_width;
    SetName(row.name, def.name);
    row.nkeys = static_cast<std::uint8_t>(def.key_cols.size());
    row.ncovered = static_cast<std::uint8_t>(def.covered_cols.size());
    row.flags = def.flags;
    row.reserved0 = 0;
    for (std::size_t i = 0; i < def.key_cols.size(); ++i) row.key_cols[i] = def.key_cols[i];
    for (std::size_t i = 0; i < def.covered_cols.size(); ++i) {
        row.covered_cols[i] = def.covered_cols[i];
    }

    if (Status s = InsertRow(wal_, ddl_undo_hook_, store_, kCatalogPageIndexes, row, trx_id, where); !s.ok()) {
        return s;
    }
    if (where != nullptr) {
        where->oid = row.index_oid;
        where->rel_oid = kSysIndexesTable;
    }

    // PW2-3: the anchor slot is seeded at creation, so the anchor is the
    // index root's whole truth from birth - without this the row would
    // stay a second source forever ("slot absent, fall back") instead of
    // for the transition alone. A peer-owned relation's slot is the
    // owner's (`kByOwner`): it built the tree in its own pages and wrote
    // its own anchor before this row, so the row is all this core writes.
    if (seed == AnchorSeed::kHere) {
        auto rel = GetSysTableRow(def.table_oid);
        if (!rel.ok()) {
            return rel.status().WithContext("resolving the relation for the index anchor seed");
        }
        if (rel.value().anchor_page_id != kInvalidPageId) {
            if (Status s = WriteAnchorRoot(rel.value().anchor_page_id, def.table_oid,
                                           row.index_oid, row.root_page_id, trx_id);
                !s.ok()) {
                return s;
            }
        }
    }

    // **Bumped**, where InsertIndexRow() deliberately did not. That comment
    // was true while nothing cached anything derived from sys.indexes; an
    // index appearing now stales `TableAccess::index_mask` on every held
    // entry for this relation (IX04), which is a fact the compiler reads.
    BumpVersion("sys.indexes create");
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("catalog", "created index " + def.name + " (oid " +
                                  std::to_string(row.index_oid) + ") on relation oid " +
                                  std::to_string(def.table_oid));
    }
    return row.index_oid;
}

Status Catalog::DropIndex(Oid index_oid, std::uint64_t trx_id, CatalogRowChange* change) {
    const bool transactional = trx_id != kBootstrapXid;
    PageId acted_page = kInvalidPageId;
    CatalogRowChange marked;
    bool already_marked = false;
    auto acted = ForFirstRow<SysIndexRow>(
        store_, kCatalogPageIndexes,
        [&](SysIndexRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
            if (row.index_oid != index_oid) return false;
            if (tuple.deleted) {
                // Matched, but a transaction has already marked it. Since
                // DT9 an unfiltered read keeps such a row alive, so
                // `FindIndexByName` hands it to us and this is now
                // reachable from a client - it was not before. Recorded
                // rather than skipped silently, because falling through to
                // "no sys.indexes row for this index_oid" answers a user's
                // statement with a system row's name and no byte position.
                already_marked = true;
                return false;
            }

            // Retired outside a transaction - DropCabin() and
            // RetirePattern() state that argument and it still holds.
            // **Inside one, delete-marked**, which a rollback can clear
            // (DT5's mechanism).
            //
            // Unlike `DROP TABLE`, this is also **isolated**, but for a
            // reason §5a had to correct: not because the payload survives
            // an overwrite, which was the original and wrong argument, but
            // because DT9 taught the unfiltered read to leave an open mark
            // alone. On core 0, which is where every writer is.
            if (!transactional) {
                if (Status s = RetireLogged(wal_, store_, page, page_id, i, trx_id); !s.ok()) {
                    return s;
                }
            } else {
                if (ddl_undo_hook_) {
                    if (Status s = ddl_undo_hook_({DdlUndoEvent::Kind::kDeleteMark, page_id, i,
                                                   {}, tuple.trx_id, tuple.undo_ptr,
                                                   UndoPkOf(tuple.payload)});
                        !s.ok()) {
                        return s;
                    }
                }
                if (Status s = DeleteMarkLogged(wal_, store_, page, page_id, i, trx_id);
                    !s.ok()) {
                    return s;
                }
                ++pending_marks_;
                marked.slot = i;
                marked.oid = index_oid;
                marked.rel_oid = kSysIndexesTable;
                marked.prior_trx_id = tuple.trx_id;
                marked.prior_undo_ptr = tuple.undo_ptr;
                marked.deleted = true;
            }

            BumpVersion("sys.indexes drop");
            if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
                log_->Info("catalog", "dropped index oid " + std::to_string(index_oid));
            }
            return true;
        },
        &acted_page);
    if (!acted.ok()) return acted.status();
    if (!acted.value()) {
        // Understood and declined, not "simply wrong": the index is there
        // and named correctly, and a transaction nobody here can wait for
        // is in the middle of removing it. The wording matches
        // `RefuseIfNameHeldByPendingDrop`'s for the table case, so the two
        // read as one rule; the caller appends the byte position.
        if (already_marked) {
            return Status::Unsupported(
                "the index is being dropped by a transaction that has not committed; it is "
                "not gone, and not droppable again, until that transaction resolves");
        }
        return Status::NotFound("no sys.indexes row for this index_oid");
    }
    if (transactional && change != nullptr) {
        marked.page_id = acted_page;  // known only once the walk found it
        *change = std::move(marked);
    }
    return Status::OK();
}

Status Catalog::UpdateIndexRoot(Oid rel_oid, Oid index_oid, PageId new_root,
                                PageId anchor_page_id) {
    // PW2-4, the 96b0343 review's C1: the ForFirstRow scan that lived here
    // fetched sys.indexes through the *write* accessor - dirtying a
    // catalog page per index root split on core 0, and refusing outright
    // on a peer, so half of PW-B2's blocker survived the retirement. The
    // scan yielded only rel_oid and an existence proof, both of which the
    // caller holds (MaintainIndexes iterates access.indexes) - so this is
    // now UpdateRelationDescPage's exact mirror: anchor write, in-place
    // cache update, no catalog touch. The in-place license is
    // catalog_cache.hpp's: a root belongs to one index and is read by
    // nothing else, and a drop would dangle the running statement's
    // TableAccess.
    if (anchor_page_id == kInvalidPageId) {
        return Status::InvalidArgument(
            "relation oid " + std::to_string(rel_oid) +
            " has no anchor page; a system relation carries no secondary index");
    }
    if (Status s =
            WriteAnchorRoot(anchor_page_id, rel_oid, index_oid, new_root, wal::kNoTxnId);
        !s.ok()) {
        return s;
    }
    cache_.UpdateIndexRoot(rel_oid, index_oid, new_root);
    return Status::OK();
}

StatusOr<std::vector<SysIndexRow>> Catalog::ListIndexes(const txn::ReadView* view) {
    return ScanAll<SysIndexRow>(store_, kCatalogPageIndexes, view, txn_);
}

StatusOr<std::vector<SysIndexRow>> Catalog::FindIndexesForTable(Oid table_oid) {
    auto rows = ListIndexes();
    if (!rows.ok()) return rows.status();

    std::vector<SysIndexRow> out;
    for (const SysIndexRow& row : rows.value()) {
        if (row.table_oid == table_oid) out.push_back(row);
    }
    return out;
}

StatusOr<SysIndexRow> Catalog::FindIndexByName(std::string_view name) {
    auto rows = ListIndexes();
    if (!rows.ok()) return rows.status();
    for (const SysIndexRow& row : rows.value()) {
        if (NameView(row.name) == name) return row;
    }
    return Status::NotFound("no index by that name");
}

StatusOr<SysIndexRow> Catalog::FindIndexOnColumn(Oid table_oid, std::uint32_t col_pos) {
    auto rows = ListIndexes();
    if (!rows.ok()) return rows.status();

    for (const SysIndexRow& row : rows.value()) {
        // `key_cols[0]` only - see the header for why "contains" would be
        // the wrong question.
        if (row.table_oid == table_oid && row.key_cols[0] == col_pos) return row;
    }
    return Status::NotFound("no index on this column");
}

}  // namespace kds::catalog
