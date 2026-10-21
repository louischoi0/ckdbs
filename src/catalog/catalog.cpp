#include "kds/catalog/catalog.hpp"

#include "kds/catalog/foreign_key.hpp"
#include "kds/parser/fingerprint.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "kds/server/superblock.hpp"
#include "kds/stats/waystone_dir.hpp"
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

// Every root bootstrap creates at its id sits below the first id the free
// map hands out, and no two share one (well_known.hpp).
static_assert(CatalogRootsAreDistinctAndFixed(server::kFirstUserPageId),
              "every fixed catalog root must be unique and sit below the first user page; "
              "add a new root past the highest, in the unused ids 17..127");

namespace {

// Scans every live row of type RowT out of the heap page at `page_id`.
// Dead slots (RowT::Decode's caller never sees them - ReadTuple() itself
// reports NotFound for a dead slot) are skipped.
// **GetForRead, not Get.** A scan reads and modifies nothing, and Get()
// marks the frame dirty by convention rather than by what the caller
// actually wrote (page_store.hpp), so a lookup through Get() would dirty a
// catalog page for every checkpoint to write back unchanged.
//
// **`view`, when given, is the reader's visibility** (DT3): a catalog row
// stamped by a transaction that this view cannot see is not there for this
// reader, exactly as an uncommitted user row is not. Null means "see
// everything", which is what bootstrap, recovery and every internal read
// pass.
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
    // **Taken once per scan, not per row**, and sound for the whole of it
    // (`InstanceVisibility::FloorCandidate`): the common mark - left by a
    // drop that committed long ago - costs one comparison, and only a mark
    // at or above it reads the in-flight tables.
    //
    // **The instance's, not this core's** (AX-S2, AX-Q2). This core's
    // oldest running id says nothing about another core's deleter: on a
    // core running nothing it was `UINT64_MAX`, so every mark read as
    // settled here while the drop that wrote it was still open elsewhere.
    const std::uint64_t none_in_flight_below =
        txn != nullptr ? txn->NoneInFlightBelow() : std::numeric_limits<std::uint64_t>::max();
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
                // `none_in_flight_below`'s sentinel: it *would* short-circuit,
                // but only because a trx id cannot reach `UINT64_MAX`
                // (48 bits, `kMaxTrxId`), and no reader should have to
                // prove that to know this cannot dereference null.
                const bool gone = view != nullptr
                                      ? view->Visible(deleter)
                                      : txn == nullptr || deleter < none_in_flight_below ||
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
// change it or retire it, stop - over the whole chain, so there is one
// loop: `fn` returns false to keep looking, true when it has acted, or a
// failure, which is reported as-is.
//
// **kWrite**, unconditionally: every caller of this writes. A reader uses
// ScanAll, which fetches read-only so a lookup does not dirty a catalog
// page.
// `acted_page`, when given, receives the page the accepted row was on -
// which is not `root` once a catalog relation has overflowed onto a
// chained page, and is what a rollback needs to address the row again
// (workplan-ddl-transactional.md DT5).
template <typename RowT, typename Fn>
StatusOr<bool> ForFirstRow(storage::PageStore& store, PageId root, Fn&& fn,
                            PageId* acted_page = nullptr,
                            storage::PageAccess access = storage::PageAccess::kWrite) {
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
    //
    // `access` is the walk's hold: exclusive for a writer, shared for a
    // reader that must neither queue behind nor block the writers of the
    // pages it passes.
    //
    // Bounded by the budget every next-page loop owes
    // (`storage::CheckPageWalkBudget`), not by a count of catalog pages:
    // what it stops is a cyclic link, and a long chain is not a fault.
    PageId current = root;
    for (std::uint32_t steps = 0;; ++steps) {
        if (Status s = storage::CheckPageWalkBudget(steps, root, "catalog chain"); !s.ok()) {
            return s;
        }
        auto bytes = store.Fetch(current, access);
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
}

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
    // which note_txn skips by design. Naming the header's writer instead
    // (a next_id bump under the relation's creator, say) would make every
    // mount past that creator's checkpointed commit invent a phantom loser
    // and write a spurious TXN_ABORT for it.
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
// and an empty formatted page is exactly what redo rebuilds. min_key is
// always 0; every caller names the type and the owning oid - a catalog
// chain's growth page its relation's (kHeap), a relation's own root pages
// theirs.
Status LogCatPageInit(wal::WalManager* wal, std::uint64_t trx_id, PageId page_id,
                      PageType type, Oid owner_oid) {
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
// formatted page - its roots are body content. Replay stays safe without ordering care because the redo range
// is a suffix: a replayed PAGE_INIT that wipes the body is necessarily
// followed in-range by every ANCHOR_UPDATE that refills it, and by every
// BTREE_SPLIT that carries the page whole after a root growth.
Status LogCatAnchorUpdate(wal::WalManager* wal, storage::PageStore& store, std::uint64_t trx_id,
                          PageId anchor_page, std::uint64_t index_oid, PageId root) {
    if (trx_id == kBootstrapXid) trx_id = wal::kNoTxnId;
    auto rec = wal::LogAnchorUpdate(wal, trx_id, anchor_page, index_oid, root);
    if (!rec.ok()) return rec.status();
    if (rec.value() == wal::kNoLsn) return Status::OK();  // unlogged store
    return store.StampPageLsn(anchor_page, rec.value());
}

// The two structural checks every anchor reader makes: a page that is not
// `owner`'s anchor cannot answer for `owner`'s slots, and reading one as
// though it were would act on another relation's table.
Status CheckAnchorOf(std::span<const std::byte, kPageSize> bytes, PageId anchor_page,
                     Oid owner) {
    if (Status s = storage::ValidatePageHeader(bytes, PageType::kAnchor); !s.ok()) return s;
    if (storage::GetOwnerOid(bytes) != owner) {
        return Status::Corruption("anchor page " + std::to_string(anchor_page) +
                                  " belongs to relation oid " +
                                  std::to_string(storage::GetOwnerOid(bytes)) + ", not oid " +
                                  std::to_string(owner));
    }
    return Status::OK();
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
std::uint64_t UndoTupleKeyOf(std::span<const std::byte> payload) {
    auto id = KeystoneIdOfPayload(payload);
    return id.ok() ? id.value() : 0;
}

// **A DDL's overwrite of one catalog row** - `DropTable`'s and
// `DropNamespace`'s retype, `DropColumn`'s flag. The prior image goes to the
// undo hook first (the hook's contract: it is the only copy a crash leaves of
// these bytes, since a catalog row has no undo chain), then the row is
// rewritten and logged, stamped with `trx_id` so the change is that
// transaction's to undo - or, at `kBootstrapXid`, outside any transaction
// with the row's own stamp kept. Returns the change a rollback restores.
StatusOr<CatalogRowChange> OverwriteForDdl(const Catalog::DdlUndoHook& undo_hook,
                                           wal::WalManager* wal, storage::PageStore& store,
                                           heap::PageView& page, PageId page_id,
                                           std::uint16_t slot,
                                           const heap::PageView::Tuple& tuple, Oid rel_oid,
                                           Oid oid, std::span<const std::byte> before,
                                           std::span<const std::byte> after,
                                           std::uint64_t trx_id) {
    CatalogRowChange change;
    change.page_id = page_id;
    change.slot = slot;
    change.oid = oid;
    change.rel_oid = rel_oid;
    change.prior_trx_id = tuple.trx_id;
    change.prior_undo_ptr = tuple.undo_ptr;
    change.prior_image.assign(before.begin(), before.end());
    if (undo_hook) {
        if (Status s = undo_hook({Catalog::DdlUndoEvent::Kind::kOverwrite, page_id, slot,
                                  change.prior_image, tuple.trx_id, tuple.undo_ptr,
                                  UndoTupleKeyOf(change.prior_image)});
            !s.ok()) {
            return s;
        }
    }
    const bool transactional = trx_id != kBootstrapXid;
    if (Status s = OverwriteLogged(wal, store, page, page_id, slot, after,
                                   transactional ? trx_id : wal::kNoTxnId,
                                   transactional ? trx_id : tuple.trx_id, tuple.undo_ptr);
        !s.ok()) {
        return s;
    }
    return change;
}

// Reports a row `InsertRow` just placed at (`page_id`, `slot`) - the undo
// record first, then the row record: the hook's whole contract
// (catalog.hpp), since redo alone must never be able to resurrect a loser's
// row that undo has no record for. **A failure of either leaves nothing
// behind to retire** (AZ-S1): the caller reports no `where` for this row, so
// a rollback could not find one.
//
// - The hook refused: nothing names the row, so it is taken back whole and
//   the page is as it was. A dead slot would do for the rollback, but not
//   for redo: a later logged insert on this page would sit past a slot no
//   record describes, which a replay onto an older image refuses (the
//   dense-slot rule, `heap_page.cpp`).
// - The log refused: the undo record the hook appended names the slot, so
//   it is retired rather than taken back - a retired slot is never handed
//   out again, so recovery's undo meets it dead and never another row
//   (`recovery_undo.cpp`, "nothing to retire"). `LogCatInsert` fails before
//   its append or not at all: the stamp after it is on a page this holds
//   pinned and exclusive, which is always resident. **This arm leaves the
//   dead slot no record describes** that the first arm avoids. On a tail
//   page, a later logged insert followed by a crash before writeback would
//   refuse the mount; taking the row back instead would let another row
//   reach a slot the undo record names. **Fail-stop closes it** (`wal.md`
//   §6-5): every refusal `LogCatInsert` can meet stops the log, so no later
//   insert is logged, and this page - held across the placement and the
//   append - is never written back with the dead slot.
Status ReportPlacedRow(wal::WalManager* wal, const Catalog::DdlUndoHook& hook,
                       storage::PageStore& store, PageId page_id, heap::PageView& page,
                       std::uint16_t slot, std::span<const std::byte> encoded,
                       std::uint64_t trx_id) {
    if (hook) {
        if (Status s = hook({Catalog::DdlUndoEvent::Kind::kInsert, page_id, slot, encoded, 0, 0,
                             UndoTupleKeyOf(encoded)});
            !s.ok()) {
            if (Status back = page.UnInsertTuple(slot); !back.ok()) {
                return back.WithContext("taking back a catalog row after: " + s.message());
            }
            return s;
        }
    }
    if (Status s = LogCatInsert(wal, store, trx_id, page_id, slot, encoded); !s.ok()) {
        if (Status retired = page.RetireSlot(slot); !retired.ok()) {
            return retired.WithContext("retiring a catalog row after: " + s.message());
        }
        return s;
    }
    return Status::OK();
}

// Appends one row to the chain rooted at `root`, growing it by a page when
// the tail is full.
//
// This is `heap::ChainInsert` minus the one thing a catalog row does not
// have: a **Keystone id**. A user tuple's first word is its tuple key, and
// the heap chain uses it to keep pages key-ordered and to enforce `min_key`
// (invariant 3). A catalog row is a fixed-offset struct with no such word,
// so there is no key to order by and every page carries `min_key = 0` - the
// chain here is an append list, not a semi-sorted heap. Sharing the heap's
// insert would mean inventing an id for a row nothing looks up by id.
// `where`, when given, receives the (page, slot) the row landed at, once
// the row's record is logged (workplan-ddl-transactional.md DT3a; AZ-S1).
// A transactional DDL registers that address on its transaction's trail so
// `Abort` can retire the slot -
// the engine hides aborted work by compensation, not by visibility (spec
// §2's correction), so without this a rolled-back CREATE TABLE stays.
template <typename RowT>
Status InsertRow(wal::WalManager* wal, const Catalog::DdlUndoHook& hook,
                  storage::PageStore& store, PageId root, const RowT& row,
                  std::uint64_t trx_id, CatalogRowRef* where = nullptr) {
    const auto encoded = row.Encode();

    // ForFirstRow's bound, for its reason.
    PageId current = root;
    for (std::uint32_t steps = 0;; ++steps) {
        if (Status s = storage::CheckPageWalkBudget(steps, root, "catalog chain"); !s.ok()) {
            return s;
        }
        auto bytes = store.Get(current);
        if (!bytes.ok()) return bytes.status();

        heap::PageView page(bytes.value().bytes());
        auto slot = page.InsertTuple(encoded, trx_id);
        if (slot.ok()) {
            if (Status s = ReportPlacedRow(wal, hook, store, current, page, slot.value(), encoded,
                                           trx_id);
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

        // From the free map's general supply, as `heap::ChainInsert` grows
        // a declaration relation: no catalog page past the fixed roots has a
        // reserved id (`crosscore.md` CC12/CR3), and a freed user id taken
        // here is a new creation (invariant 2). `CreateNew` returns it
        // pinned and exclusive, held until the row's record stamps it:
        // until then its `page_lsn` is 0, and another core's writeback
        // would put the row on disk ahead of its record (AY-S7).
        auto created = store.CreateNew();
        if (!created.ok()) return created.status();
        const PageId new_id = created.value().first;
        storage::PageRef new_page = std::move(created.value().second);

        // min_key 0, like every catalog page: these rows carry no key to
        // prune by, and a nonzero min_key would be a claim about ids that
        // do not exist here. Owned by the relation the chain is
        // (`kCatalogRoots`), as `heap::ChainInsert` stamps a declaration
        // relation's growth, and the record carries the same owner, so
        // redo rebuilds the page this run wrote.
        const Oid owner = CatalogRelationAtRoot(root);
        auto fresh = heap::PageView::CreateEmpty(new_page.bytes(), 0, owner);
        if (!fresh.ok()) return fresh.status();
        // Logged before the insert below, whose record then stamps the
        // page - the DML path's new-tuple-page discipline exactly.
        if (Status s = LogCatPageInit(wal, trx_id, new_id, PageType::kHeap, owner); !s.ok()) {
            return s;
        }

        auto placed = fresh.value().InsertTuple(encoded, trx_id);
        if (!placed.ok()) {
            // A row no empty page can hold. The page is left allocated and
            // empty rather than freed - a free needs BF-R4's replay gate -
            // and it is not linked in, so nothing reaches it and no reclaim
            // walk does either: a stated leak of one page (`drop-table.md`
            // DT1), the trade `heap::ChainInsert` makes. A report refused
            // below leaves the page the same way.
            return placed.status();
        }
        if (Status s = ReportPlacedRow(wal, hook, store, new_id, fresh.value(), placed.value(),
                                       encoded, trx_id);
            !s.ok()) {
            return s;
        }
        // Reported from here: the row is logged, so a failure of the link
        // below leaves it for the rollback to retire.
        if (where != nullptr) *where = CatalogRowRef{new_id, placed.value()};
        new_page.Release();

        // Linked **after** the row is in it: a link published before the
        // page it points at is filled is a link a reader can follow into an
        // empty page - the ordering that protects a concurrent reader on
        // this core. The re-fetch is a re-entrant hit on the tail this loop
        // still pins through `bytes` (frames are slots that never move), so
        // it names the same frame. The edit itself travels as a full page
        // image: no record type describes a next_page_id edit
        // (LogCatPageImage).
        auto tail_again = store.Get(current);
        if (!tail_again.ok()) return tail_again.status();
        heap::PageView(tail_again.value().bytes()).set_next_page_id(new_id);
        if (Status s = LogCatPageImage(wal, store, trx_id, current); !s.ok()) return s;
        return Status::OK();
    }
}

}  // namespace

void Catalog::InitWellKnownObjects() {
    auto register_namespace = [this](Oid oid, std::string_view name) {
        SysObjectRow obj{};
        obj.oid = oid;
        obj.namespace_oid = oid;  // a namespace's own namespace is itself
        obj.type_oid = kTypeNamespace;
        obj.pending_roots = 0;
        SetName(obj.name, name);
        sys_objects_.Register(obj);
    };
    auto register_type = [this](Oid oid, std::string_view name) {
        SysObjectRow obj{};
        obj.oid = oid;
        obj.namespace_oid = kNamespaceSys;
        obj.type_oid = oid;  // a type object's type is itself
        obj.pending_roots = 0;
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

namespace {

Schema DeclarationSchema(Oid rel_oid, Oid column_oid_base, std::string_view aux_name,
                         std::uint32_t aux_type_val, std::uint32_t aux_len) {
    // Six columns, the shape `assertion.md` §8.2 gave sys.assertions and
    // sys.transitions copies (`exec/declaration_relation.hpp`): the
    // Keystone tuple key `id`, the constrained relation, one fixed column of the
    // relation's own (`aux`), `flags`, and two varchars - the name and the
    // declaration verbatim.
    //
    // `id` is the Keystone tuple key because every relation stored in user tuple
    // format has one (invariant 11), and it **is** the declaration's id -
    // engine-issued, unique, never reused (K1) - rather than a second
    // sequence beside it.
    //
    // Nothing stores the parsed declaration: it is recovered by re-parsing
    // `source_text` (AS10's model), so a longer list costs text rather than
    // a wider row, and no decoded sibling can drift from the canon.
    //
    // `name` and `source_text` are varchar, so the relation can spill and
    // gets a var-heap chain. A declaration longer than one var-heap page is
    // refused rather than chained.
    Schema schema;
    auto column = [rel_oid](Oid oid, std::uint32_t pos, std::string_view name,
                            std::uint32_t type_val, std::uint32_t len) {
        SysColumnRow col{};
        col.oid = oid;
        col.rel_id = rel_oid;
        col.pos = pos;
        SetName(col.name, name);
        col.type_val = type_val;
        col.len = len;
        col.notnull = true;
        return col;
    };
    schema.columns.push_back(column(column_oid_base + 0, 0, "id", kTypeValInt64, 8));
    schema.columns.push_back(column(column_oid_base + 1, 1, "target_oid", kTypeValInt32, 4));
    schema.columns.push_back(column(column_oid_base + 2, 2, aux_name, aux_type_val, aux_len));
    schema.columns.push_back(column(column_oid_base + 3, 3, "flags", kTypeValInt32, 4));
    schema.columns.push_back(column(column_oid_base + 4, 4, "name", kTypeValVarchar, 0));
    schema.columns.push_back(column(column_oid_base + 5, 5, "source_text", kTypeValVarchar, 0));
    return schema;
}

}  // namespace

Status Catalog::Bootstrap() {
    InitWellKnownObjects();

    struct SysTableBootstrap {
        Oid oid;
        std::string_view name;
        PageId page_id;
    };
    static constexpr SysTableBootstrap kSysTables[] = {
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
        // unlike sys.assertions, which stores text and therefore had to
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
    };

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
        if (Status s = InsertRelationRow(t.oid, kNamespaceSys, t.page_id,
                                          ClusteredType::kCatalogChain, kInvalidPageId);
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

    // Phase 5: the three declaration relations, sys.assertions,
    // sys.transitions and sys.cabin_exprs - the catalog relations stored in ordinary user tuple
    // format (well_known.hpp explains why). Built here by hand rather than
    // through CreateTable() for two reasons: CreateTable() takes its oids
    // from GenerateUserOid(), which is in-memory at bootstrap time and could
    // not recover a position from the very pages being built, and it roots
    // the relation on a dynamically allocated page, which nothing could find
    // at bootstrap without first reading the catalog it is part of.
    //
    // After phase 4: their schemas name column types by the type_val tags
    // phase 4 just registered. Each has one fixed column of its own:
    // sys.assertions' Bound Cabin root, sys.transitions' governed column,
    // sys.cabin_exprs' Cabin.
    struct Declarations {
        Oid oid;
        std::string_view name;
        PageId page_id;
        Oid column_oid_base;
        std::string_view aux_name;
        std::uint32_t aux_type_val;
        std::uint32_t aux_len;
    };
    static constexpr Declarations kDeclarations[] = {
        {kSysAssertionsTable, "assertions", kCatalogPageAssertions,
         kSysAssertionsColumnOidBase, "cabin_root", kTypeValInt64, 8},
        {kSysTransitionsTable, "transitions", kCatalogPageTransitions,
         kSysTransitionsColumnOidBase, "column_no", kTypeValInt32, 4},
        // AQ-S1: an expression or predicate Cabin's `F`, by `cabin_id`.
        {kSysCabinExprsTable, "cabin_exprs", kCatalogPageCabinExprs,
         kSysCabinExprsColumnOidBase, "cabin_id", kTypeValInt64, 8},
    };
    for (const Declarations& d : kDeclarations) {
        if (Status s = BootstrapDeclarations(
                d.oid, d.name, d.page_id,
                DeclarationSchema(d.oid, d.column_oid_base, d.aux_name, d.aux_type_val, d.aux_len));
            !s.ok()) {
            return s;
        }
    }

    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("catalog", "bootstrapped " +
                                  std::to_string(std::size(kSysTables) + std::size(kDeclarations)) +
                                  " system tables and " + std::to_string(kTypes.size()) +
                                  " types on the fixed catalog pages");
    }
    return Status::OK();
}


Status Catalog::BootstrapDeclarations(Oid rel_oid, std::string_view name, PageId root_page,
                                      const Schema& schema) {
    // Refused at bootstrap rather than at the first CREATE, for
    // CreateTable()'s reason: a relation whose row size cannot be computed
    // is one no row could ever be written to, and it also proves the schema
    // is expressible under whatever inline_cell_width this instance
    // pinned.
    if (auto layout = RowLayout::Build(schema, inline_cell_width_); !layout.ok()) {
        return layout.status();
    }

    auto created = store_.CreateAt(root_page);
    if (!created.ok()) return created.status();
    // min_key 0: scanned in full, by name or by target_oid, never by key
    // range - like every other catalog page. Stamped with the relation's
    // own oid (page.md §2a), unlike the fixed catalog core: this chain
    // grows through ChainInsert, which stamps, so the root must agree with
    // the pages that follow it.
    auto root = heap::PageView::CreateEmpty(created.value().bytes(), 0, rel_oid);
    if (!root.ok()) return root.status();

    auto varheap_root = varheap::CreateChain(store_, rel_oid);
    if (!varheap_root.ok()) return varheap_root.status();

    if (Status s = InsertObjectRow(rel_oid, kNamespaceSys, kTypeTable, name); !s.ok()) {
        return s;
    }
    if (Status s = InsertRelationRow(rel_oid, kNamespaceSys, root_page,
                                      ClusteredType::kCatalogChain, varheap_root.value());
        !s.ok()) {
        return s;
    }
    for (const auto& col : schema.columns) {
        if (Status s = InsertColumnRow(col.oid, rel_oid, col.pos, NameView(col.name),
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

StatusOr<std::vector<Catalog::PendingReclaim>> Catalog::PendingReclaims() {
    auto objects = ScanAll<SysObjectRow>(store_, kCatalogPageObjects, nullptr, txn_);
    if (!objects.ok()) return objects.status();
    std::vector<PendingReclaim> out;
    for (const SysObjectRow& row : objects.value()) {
        if (row.type_oid != kTypeDroppedTable || row.pending_roots == 0) continue;
        out.push_back(PendingReclaim{row.oid, UnpackPendingRoots(row.pending_roots)});
    }
    return out;
}

StatusOr<std::vector<Catalog::OwedIndexTree>> Catalog::OwedIndexTrees() {
    auto objects = ScanAll<SysObjectRow>(store_, kCatalogPageObjects, nullptr, txn_);
    if (!objects.ok()) return objects.status();
    auto tables = ScanAll<SysTableRow>(store_, kCatalogPageTables, nullptr, txn_);
    if (!tables.ok()) return tables.status();
    auto indexes = ListIndexes();
    if (!indexes.ok()) return indexes.status();
    std::unordered_set<Oid> live_relations;
    for (const SysObjectRow& object : objects.value()) {
        if (object.type_oid == kTypeTable) live_relations.insert(object.oid);
    }
    std::unordered_set<Oid> live_indexes;
    for (const SysIndexRow& row : indexes.value()) live_indexes.insert(row.index_oid);

    std::vector<OwedIndexTree> out;
    for (const SysTableRow& table : tables.value()) {
        // A system relation has no anchor and carries no index slot; a
        // dropped one's slots are its tombstone's (BY-R3).
        if (table.anchor_page_id == kInvalidPageId || !live_relations.contains(table.oid)) {
            continue;
        }
        auto anchor = store_.GetForRead(table.anchor_page_id);
        if (!anchor.ok()) return anchor.status();
        if (Status s = CheckAnchorOf(anchor.value().bytes(), table.anchor_page_id, table.oid);
            !s.ok()) {
            return s;
        }
        auto slots = storage::AnchorIndexSlots(anchor.value().bytes());
        if (!slots.ok()) return slots.status();
        for (const storage::AnchorIndexSlot& slot : slots.value()) {
            // BY-R1's root clause: no slot is written without a root today,
            // and one that were would name no tree to walk.
            if (live_indexes.contains(slot.index_oid) || slot.root == kInvalidPageId) continue;
            out.push_back(OwedIndexTree{table.oid, table.anchor_page_id, slot.index_oid});
        }
    }
    return out;
}

Status Catalog::RemoveIndexSlot(Oid rel_oid, PageId anchor_page_id, Oid index_oid) {
    auto anchor = store_.Get(anchor_page_id);
    if (!anchor.ok()) return anchor.status();
    if (Status s = CheckAnchorOf(anchor.value().bytes(), anchor_page_id, rel_oid); !s.ok()) {
        return s;
    }
    if (Status s = storage::RemoveAnchorIndexSlot(anchor.value().bytes(), index_oid); !s.ok()) {
        return s;
    }
    auto rec = wal::LogAnchorSlotRemove(wal_, wal::kNoTxnId, anchor_page_id, index_oid);
    if (!rec.ok()) return rec.status();
    if (rec.value() == wal::kNoLsn) return Status::OK();  // unlogged store
    return store_.StampPageLsn(anchor_page_id, rec.value());
}

Status Catalog::ClearPendingRoots(Oid oid) {
    auto cleared = ForFirstRow<SysObjectRow>(
        store_, kCatalogPageObjects,
        [&](SysObjectRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
            if (row.type_oid != kTypeDroppedTable || row.oid != oid) return false;
            row.pending_roots = 0;
            const auto encoded = row.Encode();
            // Outside any transaction, with the row's own stamp kept (BF-R2).
            if (Status s = OverwriteLogged(wal_, store_, page, page_id, i, encoded,
                                           wal::kNoTxnId, tuple.trx_id, tuple.undo_ptr);
                !s.ok()) {
                return s;
            }
            return true;
        });
    if (!cleared.ok()) return cleared.status();
    if (!cleared.value()) {
        return Status::NotFound("no tombstone carries oid " + std::to_string(oid));
    }
    return Status::OK();
}

StatusOr<Oid> Catalog::GenerateUserOid() {
    if (words_ != nullptr) {
        // **The instance's sequence** (AT-S5b), seeded by whichever catalog
        // asks first. Zero is the unseeded value and no oid can be zero
        // (`kUserOidStart`), so the CAS below distinguishes the two without
        // a second word. Two cores seeding at once agree: one CAS wins, the
        // loser's fails and falls through to the `fetch_add`, which reads
        // the winner's value - the scan it did on the way is wasted and
        // nothing else.
        if (words_->oid_sequence.load(std::memory_order_acquire) == 0) {
            auto highest = HighestIssuedUserOid();
            if (!highest.ok()) {
                return highest.status().WithContext(
                    "cannot issue an object oid without reading back the ones already issued");
            }
            Oid expected = 0;
            words_->oid_sequence.compare_exchange_strong(expected, highest.value() + 1,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire);
        }
        return words_->oid_sequence.fetch_add(1, std::memory_order_acq_rel);
    }
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

void Catalog::BumpVersion(std::string_view what, Tally cause) {
    ++catalog_version_;
    cache_.Invalidate();
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "cache invalidated by " + std::string(what) + ": version " +
                                   std::to_string(catalog_version_));
    }
    // After the local drop, never before (AT-S2): a peer that revalidated
    // against the new word while this instance still held stale entries
    // would be reading a catalog its own writer disagrees with. The word
    // is bumped at the write, which is before the DDL's relation `X` is
    // released at its decide - D21's order.
    BumpWord(cause);
}

void Catalog::BumpWord(Tally cause) {
    if (words_ == nullptr) return;
    Contention::Add(cause);
    const std::uint64_t prev = words_->schema_word.fetch_add(1, std::memory_order_acq_rel);
    // Adopt the bump only if this cache was current. A writer whose cache
    // `BumpVersion()` just dropped, or a root move keeping its entry in
    // place (`AdoptRoot`), has
    // `cache_built_at_ == prev` and moves with the word. A bump from
    // another core this cache has not yet revalidated against leaves it
    // behind, so the next boundary still drops - otherwise this write would
    // swallow that change and serve the other relation's old schema for
    // good, a wrong answer and not a refusal. Required since a peer writes
    // the catalog (AT-S3, AT-S5).
    if (cache_built_at_ == prev) cache_built_at_ = prev + 1;
}

void Catalog::Revalidate(const std::function<void()>& after_read) {
    if (words_ == nullptr) return;
    const std::uint64_t now = words_->schema_word.load(std::memory_order_acquire);
    if (after_read) after_read();
    if (now == cache_built_at_) return;
    Contention::Add(Tally::kCatalogMemoDrops);
    cache_.Invalidate();
    cache_built_at_ = now;
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "cache dropped: the schema word moved to " + std::to_string(now));
    }
}

void Catalog::InvalidateAfterCompensation() {
    BumpVersion("a transaction that wrote catalog rows resolved", Tally::kWordMovesResolve);
}

StatusOr<std::uint64_t> Catalog::RetireDeleteMarks(
    const std::function<bool(std::uint64_t deleter)>& settled, std::uint64_t* remaining_out) {
    std::uint64_t retired = 0;
    for (const CatalogRoot& relation : kCatalogRoots) {
        const PageId root = relation.page;
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
                // A mark's trx_id is its *deleter*. A deleter some live
                // view may still look through proves nothing, so the mark
                // stays for a later pass.
                if (!settled(tuple.value().trx_id)) {
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
    // Every mark, unconditionally: nothing that was live before the mount
    // is live now, and recovery has rolled every loser back.
    auto swept = RetireDeleteMarks([](std::uint64_t) { return true; });
    if (!swept.ok()) return swept;
    // Nothing survives a sweep that admits every deleter. Still the delta
    // and not a store: subtracting what this sweep saw leaves a mark that
    // arrived between the read and the add counted, where a store would
    // drop it. Nothing can, here - this runs before any peer starts - and
    // there is no second spelling of the resettle for the one caller that
    // could not race.
    ResettleMarks(MarkCount(), 0);
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
    const std::uint64_t before = MarkCount();
    if (before == 0) return std::uint64_t{0};
    std::uint64_t remaining = 0;
    auto swept = RetireDeleteMarks(
        [this](std::uint64_t deleter) { return txn_->ResolvedForEveryReader(deleter); },
        &remaining);
    if (!swept.ok()) return swept;
    ResettleMarks(before, remaining);
    if (swept.value() > 0 && log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "purged " + std::to_string(swept.value()) +
                                   " settled delete-marked catalog row(s)");
    }
    // No version bump, unlike the mount sweep: every retired row was
    // already gone to every reader, so no cached answer changes. The
    // proof lives once, in ddl-transactional.md §5d.
    return swept;
}

void Catalog::DropCache() {
    // No bump of either counter: nothing changed on this instance's
    // numbering, and the word belongs to whoever wrote. What has to go is
    // the memo, because the frames under it were rewritten by redo.
    cache_.Invalidate();
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "cache dropped after redo");
    }
}

Status Catalog::InsertObjectRow(Oid oid, Oid namespace_oid, Oid type_oid,
                                 std::string_view name, std::uint64_t trx_id,
                                 CatalogRowRef* where) {
    SysObjectRow row{};
    row.oid = oid;
    row.namespace_oid = namespace_oid;
    row.type_oid = type_oid;
    row.pending_roots = 0;
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
    if (Status s = CheckAnchorOf(anchor.value().bytes(), anchor_page, expected_owner_oid);
        !s.ok()) {
        return s;
    }
    return storage::CheckAnchorRoomForIndex(anchor.value().bytes(), index_oid);
}

Status Catalog::WriteAnchorRoot(PageId anchor_page, Oid expected_owner_oid,
                                std::uint64_t index_oid, PageId root, std::uint64_t trx_id) {
    auto staged = StageAnchorRoot(anchor_page, expected_owner_oid, index_oid, root);
    if (!staged.ok()) return staged.status();
    return LogCatAnchorUpdate(wal_, store_, trx_id, anchor_page, index_oid, root);
}

StatusOr<storage::PageRef> Catalog::StageAnchorRoot(PageId anchor_page, Oid expected_owner_oid,
                                                    std::uint64_t index_oid, PageId root) {
    // A system relation has no anchor and never grows a level - its fixed
    // catalog page does not - so an anchorless root move is a defect.
    if (anchor_page == kInvalidPageId) {
        return Status::InvalidArgument("relation oid " + std::to_string(expected_owner_oid) +
                                       " has no anchor page; a system relation's root cannot move");
    }
    auto anchor = store_.Get(anchor_page);
    if (!anchor.ok()) return anchor.status().WithContext("faulting anchor for a root move");
    // The write is where the damage is created, so it checks what the
    // redo applier and the fill already check (the 96b0343 review's C3):
    // aimed at a non-anchor page, SetAnchorClusteredRoot would overwrite
    // four bytes of somebody's tuple data with no error anywhere; aimed at
    // another relation's anchor, it would move the wrong tree's root.
    if (Status s = CheckAnchorOf(anchor.value().bytes(), anchor_page, expected_owner_oid);
        !s.ok()) {
        return s;
    }
    if (index_oid == 0) {
        storage::SetAnchorClusteredRoot(anchor.value().bytes(), root);
    } else if (Status s = storage::SetAnchorIndexRoot(anchor.value().bytes(), index_oid, root);
               !s.ok()) {
        return s;
    }
    return std::move(anchor.value());
}

Status Catalog::InsertRelationRow(Oid oid, Oid namespace_oid, PageId desc_page_id,
                                   ClusteredType clustered_type, PageId varheap_page_id,
                                   PageId anchor_page_id, std::uint64_t trx_id,
                                   CatalogRowRef* where, std::uint16_t spare_slots) {
    SysTableRow row{};
    row.oid = oid;
    row.namespace_oid = namespace_oid;
    row.desc_page_id = desc_page_id;
    row.clustered_type = clustered_type;
    row.next_id = kFirstRowId;
    row.varheap_page_id = varheap_page_id;
    row.anchor_page_id = anchor_page_id;
    row.spare_slots = spare_slots;
    Status s = InsertRow(wal_, ddl_undo_hook_, store_, kCatalogPageTables, row, trx_id, where);
    if (where != nullptr) where->rel_oid = kSysTablesTable;
    if (s.ok()) BumpVersion("sys.tables insert");
    return s;
}

Status Catalog::InsertColumnRow(Oid oid, Oid rel_id, std::uint32_t pos, std::string_view name,
                                 std::uint32_t type_val, std::uint32_t len, bool notnull,
                                 std::uint8_t cabin_policy, std::uint64_t trx_id,
                                 CatalogRowRef* where, std::uint8_t flags) {
    SysColumnRow row{};
    row.oid = oid;
    row.rel_id = rel_id;
    row.pos = pos;
    SetName(row.name, name);
    row.type_val = type_val;
    row.len = len;
    row.notnull = notnull;
    row.cabin_policy = cabin_policy;
    row.flags = flags;
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
                                    std::uint64_t trx_id, std::vector<CatalogRowRef>* written) {
    // Refused at definition time rather than at the first INSERT: a table
    // whose first column cannot hold the Keystone id is one no row can
    // ever be written to (heap-and-tuple.md section 4).
    if (Status s = CheckKeystoneColumn(schema); !s.ok()) return s;

    // No key-mode refusal here any more. Until 2026-08-25 an EXPLICIT
    // relation was required to be btree-clustered and this is where the
    // pairing was refused; the mode is gone, both storage types take a
    // caller-supplied tuple key, and what a relation cannot take is refused per
    // *id* by the insert path rather than per relation - a duplicate or an
    // exhausted key (BD-R5).
    if (Status s = CheckDeclarableColumnTypes(schema); !s.ok()) return s;

    // Column names are unique ignoring case. The dispatcher refuses a twin
    // first, at its byte; this is the door's own defense for a schema built
    // by any other caller.
    if (Status s = CheckColumnNamesUnique(schema); !s.ok()) return s;

    // Same argument, extended by the fixed-length rule: the relation's row
    // size is a schema constant, so if it cannot be computed - a column
    // with no decided on-disk width, or a row wider than a page - there is
    // no row this table could hold either. Computing it here also means
    // the layout every later InitTableAccess() builds is known to succeed.
    if (auto layout = RowLayout::Build(schema, inline_cell_width_); !layout.ok()) {
        return layout.status();
    }

    // The name, asked early so a taken one - or one an open drop holds -
    // costs no oid and no page. Not the answer that binds: that is asked
    // again under page 6 at the insert below.
    if (Status s = CheckNameFree(name, kTypeTable, namespace_oid, trx_id,
                                 /*own_drop_frees=*/true);
        !s.ok()) {
        return s;
    }

    // The oid, issued before any page is formatted so every page of the
    // relation carries it from birth (page.md §2a). An oid burned by a
    // failure below is fine by this function's own argument: an oid counts
    // objects ever created, not objects that exist.
    auto generated_oid = GenerateUserOid();
    if (!generated_oid.ok()) return generated_oid.status();
    const Oid new_oid = generated_oid.value();

    // The root's PageRef is scoped so its pin drops before the rest of this
    // function - a pin held no longer than its use.
    PageId root_id = kInvalidPageId;
    {
        auto created = store_.CreateNew();
        if (!created.ok()) return created.status();
        root_id = created.value().first;
        const std::span<std::byte, kPageSize> root_bytes = created.value().second.bytes();

        // Every user relation is a clustered B+ tree (BZ-R1), rooted at
        // `desc_page_id` as one leaf. It grows its first internal level only
        // when that leaf splits, so a small table costs one page.
        const Status formatted = btree::FormatRoot(root_bytes, new_oid);
        if (!formatted.ok()) return formatted;
        // RV3: the relation's root must exist in the log or a crash that
        // loses the unflushed page makes the first HEAP_INSERT irreplayable -
        // redo refuses a record naming a page nothing creates. The root is
        // one leaf, which is exactly what a PAGE_INIT rebuilds.
        if (Status s = LogCatPageInit(wal_, trx_id, root_id, PageType::kBtreeLeaf, new_oid);
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
    //
    // A relation with slots gets its chain whether or not a declared column
    // can spill (BW-R10): a `varchar(16)` a later `ADD COLUMN` places in a
    // slot spills into it, and allocating then would move a root this rule
    // keeps fixed at CREATE.
    PageId varheap_root = kInvalidPageId;
    if (SchemaCanSpill(schema) || schema.spare_slots > 0) {
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
    {
        // **One name, one row** (AT-S17, `catalog.md` CT7): `sys.objects`'
        // root page held exclusive across the binding check and the insert,
        // so no other core writes the name in between. Only page 6's own
        // chain growth and the WAL nest under it. A create that loses here
        // has burned an oid and its pages, which a failure anywhere below
        // already does.
        auto names = store_.Get(kCatalogPageObjects);
        if (!names.ok()) return names.status();
        if (Status s = CheckNameFree(name, kTypeTable, namespace_oid, trx_id,
                                     /*own_drop_frees=*/true);
            !s.ok()) {
            return s;
        }
        // And the membership (AT-S17b): the namespace the caller resolved
        // is still there, asked under the hold its drop's RESTRICT takes.
        if (Status s = CheckNamespaceLive(namespace_oid, trx_id); !s.ok()) return s;
        if (Status s = InsertObjectRow(new_oid, namespace_oid, kTypeTable, name, trx_id, &ref);
            !s.ok()) {
            return s;
        }
    }
    ref.oid = new_oid;
    note(ref);
    if (Status s = InsertRelationRow(new_oid, namespace_oid, root_id, ClusteredType::kBtree,
                                      varheap_root, anchor_id, trx_id,
                                      &ref, schema.spare_slots);
        !s.ok()) {
        return s;
    }
    ref.oid = new_oid;
    note(ref);

    for (const auto& col : schema.columns) {
        auto col_oid = GenerateUserOid();
        if (!col_oid.ok()) return col_oid.status();
        // `flags` carries a heap's hidden tuple key (BZ-R2); every declared
        // column's is 0.
        Status s = InsertColumnRow(col_oid.value(), new_oid, col.pos, NameView(col.name),
                                    col.type_val, col.len, col.notnull, col.cabin_policy,
                                    trx_id, &ref, col.flags);
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

StatusOr<Oid> Catalog::FindTableOidByName(Oid namespace_oid, std::string_view name,
                                          const txn::ReadView* view) {
    // **A transactional lookup neither reads nor fills the cache** (DT3;
    // ddl-transactional.md §4's option (a)). The cache is one map for
    // the whole instance and answers "does this name exist" with no idea
    // whose transaction is asking - so serving one session from it would
    // hand it another session's uncommitted relation, and filling it from
    // one would publish that relation to everybody. Scoped to callers that
    // pass a view: with none, this is the path that always existed.
    if (view == nullptr) {
        if (const Oid* cached = cache_.FindOidByName(namespace_oid, name); cached != nullptr) {
            return *cached;
        }
    }

    auto rows = ScanAll<SysObjectRow>(store_, kCatalogPageObjects, view, txn_);
    if (!rows.ok()) return rows.status();

    for (const auto& row : rows.value()) {
        if (row.type_oid == kTypeTable && row.namespace_oid == namespace_oid &&
            NameView(row.name) == name) {
            if (view == nullptr) cache_.PutOidByName(namespace_oid, name, row.oid);
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

namespace {

// Refused rather than allowed: spurious if that transaction commits its
// drop, and it names an object the asker may no longer see - but allowing
// it is the outcome that corrupts, because the drop's rollback restores
// the name beside whatever took it (`catalog.md` CT7). Retryable: the name
// is free once that transaction resolves.
Status PendingDropRefusal(std::string_view kind, std::string_view name) {
    return Status::TxnConflict(std::string(kind) + " '" + std::string(name) +
                               "' is being dropped by a transaction that has not committed; "
                               "the name is not free until that transaction resolves");
}

// Whose a row's stamp is, as a check reads it: settled for every reader,
// or still undecided - the asker's own transaction, or another's.
enum class Stamp : std::uint8_t { kDecided, kOwn, kOther };

// **Every `sys.objects` row as a check reads it** - the one walk
// `CheckNameFree`, `CheckNamespaceLive` and `CheckNamespaceEmpty` share. A
// delete-mark the view has seen is gone; every other row is handed to `fn`
// with its stamp classified. **The view is the instance's** (AN-S2), minted
// with no own id, so a transaction open on another core is undecided here
// too, and the asker's own (`own_trx_id`, or `txn::kNoTrxId`) is told apart
// by its stamp. `fn` returns a verdict to stop on, or OK to go on.
template <typename Fn>
Status CheckObjects(storage::PageStore& store, const txn::TransactionManager* txn,
                    std::uint64_t own_trx_id, Fn fn) {
    const txn::ReadView view =
        txn != nullptr ? txn->MintCheckView(txn::kNoTrxId) : txn::ReadView::Everything();
    Status verdict = Status::OK();
    Status walked = heap::ChainVisit(
        store, kCatalogPageObjects, storage::PageAccess::kRead,
        [&](PageId, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            if (!tuple.ok()) {
                if (tuple.status().code() == StatusCode::kNotFound) {
                    return storage::VisitControl::kContinue;
                }
                return tuple.status();
            }
            const std::uint64_t stamp = tuple.value().trx_id;
            if (tuple.value().deleted && view.Visible(stamp)) {
                return storage::VisitControl::kContinue;
            }
            auto row = SysObjectRow::Decode(tuple.value().payload);
            if (!row.ok()) return row.status();
            const Stamp whose = view.Visible(stamp) ? Stamp::kDecided
                                : own_trx_id != txn::kNoTrxId && stamp == own_trx_id
                                    ? Stamp::kOwn
                                    : Stamp::kOther;
            verdict = fn(row.value(), whose);
            return verdict.ok() ? storage::VisitControl::kContinue : storage::VisitControl::kStop;
        });
    if (!walked.ok()) return walked;
    return verdict;
}

}  // namespace

Status Catalog::CheckNameFree(std::string_view name, Oid live_type, Oid namespace_oid,
                              std::uint64_t own_trx_id, bool own_drop_frees) {
    const bool table = live_type == kTypeTable;
    const std::string_view kind = table ? "relation" : "namespace";
    const Oid dropped_type = table ? kTypeDroppedTable : kTypeDroppedNamespace;
    // Each row of the name read by its type and its stamp (`CheckObjects`):
    // a live row is taken, whoever wrote it and whether or not it has
    // committed (the unfiltered duplicate check); a tombstone still
    // undecided is a drop whose rollback restores the name. The
    // dispatcher's test this replaced minted a view only when the asking
    // core had DDL open. Not through the name cache either: an absence is
    // not cached, and a presence the cache still remembers is one the page
    // has already let go of.
    return CheckObjects(store_, txn_, own_trx_id, [&](const SysObjectRow& row,
                                                      Stamp whose) -> Status {
        if (NameView(row.name) != name) return Status::OK();
        if (table && row.namespace_oid != namespace_oid) return Status::OK();
        if (row.type_oid == live_type) {
            return Status::AlreadyExists(
                "a " + std::string(kind) + " named '" + std::string(name) + "' already exists" +
                (table ? " in namespace '" + NamespaceSpelling(namespace_oid) + "'" : ""));
        }
        if (row.type_oid != dropped_type || whose == Stamp::kDecided) return Status::OK();
        if (whose == Stamp::kOther) return PendingDropRefusal(kind, name);
        if (own_drop_frees) return Status::OK();
        // Not retryable: the only transaction it waits on is the asker's own.
        return Status::Unsupported(std::string(kind) + " '" + std::string(name) +
                                   "' is being dropped by this transaction, and a rename is not "
                                   "undone by its rollback - commit the drop first");
    });
}

Status Catalog::CheckNamespaceLive(Oid namespace_oid, std::uint64_t own_trx_id) {
    // The well-known two are the registry's (`InitWellKnownObjects`), have
    // no page row, and cannot be dropped (`DropNamespace`).
    if (namespace_oid == kNamespaceSys || namespace_oid == kNamespacePublic) {
        return Status::OK();
    }
    bool found = false;
    Status verdict = CheckObjects(store_, txn_, own_trx_id, [&](const SysObjectRow& row,
                                                                Stamp whose) -> Status {
        if (row.oid != namespace_oid) return Status::OK();
        const std::string ns(NameView(row.name));
        if (row.type_oid == kTypeNamespace) {
            // Another transaction's create, undecided (AT-S17b's review):
            // its rollback removes the row, and a relation admitted into it
            // would be left in a namespace that never existed. An autocommit
            // resolution on a core with no DDL open reads unfiltered and
            // hands this check that oid.
            if (whose == Stamp::kOther) {
                return Status::TxnConflict("namespace '" + ns +
                                           "' is being created by a transaction that has not "
                                           "committed");
            }
            found = true;
            return Status::OK();
        }
        if (row.type_oid != kTypeDroppedNamespace) return Status::OK();
        if (whose == Stamp::kOther) return PendingDropRefusal("namespace", ns);
        return Status::NotFound("namespace '" + ns + "' has been dropped");
    });
    if (!verdict.ok()) return verdict;
    if (!found) {
        return Status::NotFound("no namespace with oid " + std::to_string(namespace_oid));
    }
    return Status::OK();
}

Status Catalog::CheckNamespaceEmpty(Oid namespace_oid, std::uint64_t own_trx_id) {
    // `sys.objects` and not `sys.tables`: a create's `sys.objects` row is
    // the one written under page 6's hold, and its `sys.tables` row comes
    // after it is dropped - a RESTRICT check of page 7 could pass between
    // the two. Read unfiltered, `sys.tables`' delete-mark counted an open
    // relation drop as a relation still there; here the drop is a retype,
    // visible at once, so an undecided tombstone is what counts. (A
    // namespace's own row is a namespace type, so the type tests skip it.)
    return CheckObjects(store_, txn_, own_trx_id, [&](const SysObjectRow& row,
                                                      Stamp whose) -> Status {
        if (row.namespace_oid != namespace_oid) return Status::OK();
        const bool live = row.type_oid == kTypeTable;
        if (!live && !(row.type_oid == kTypeDroppedTable && whose != Stamp::kDecided)) {
            return Status::OK();
        }
        const std::string still = "namespace oid " + std::to_string(namespace_oid) +
                                  " cannot be dropped: relation '" +
                                  std::string(NameView(row.name)) + "' is still in it";
        // Another transaction's drop is retryable; the asker's own is not,
        // and was refused before AT-S17b too - commit it first.
        if (!live && whose == Stamp::kOther) {
            return Status::TxnConflict(still + " until its drop, not yet committed, resolves");
        }
        return Status::InvalidArgument(still);
    });
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

Status Catalog::RenameTable(Oid table_oid, std::string_view new_name,
                            std::uint64_t own_trx_id) {
    if (Status s = CheckRenameName("table", new_name); !s.ok()) return s;

    // **Check and rewrite under one hold** (AT-S17), `CreateTable`'s: the
    // row being renamed and the name being taken are both on `sys.objects`,
    // so page 6 held across the pair is the whole of it. A rename is not
    // transactional (it lands for good, logged under no transaction), so
    // even this session's open drop of the name holds it - that drop's
    // rollback would restore the name beside a rename nothing takes back -
    // and `own_trx_id` only makes that refusal say so instead of retryable.
    //
    // The new name is asked in the relation's own namespace (NS6), read
    // before the hold: no statement moves a relation between namespaces.
    auto table_row = GetSysTableRow(table_oid);
    if (!table_row.ok()) return table_row.status();
    auto names = store_.Get(kCatalogPageObjects);
    if (!names.ok()) return names.status();
    if (Status s = CheckNameFree(new_name, kTypeTable, table_row.value().namespace_oid, own_trx_id,
                                 /*own_drop_frees=*/false);
        !s.ok()) {
        return s;
    }

    auto acted = ForFirstRow<SysObjectRow>(
        store_, kCatalogPageObjects,
        [&](SysObjectRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
            if (row.type_oid != kTypeTable || row.oid != table_oid) return false;
            // The catalog's own names are load-bearing for bootstrap and
            // are nobody's to change (AL7). AF-P3: identity, not
            // permission (well_known.hpp).
            if (IsSystemNamespace(row.namespace_oid)) {
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

    // Sibling collision and old-name existence in one read, and the rewrite,
    // **under one hold of `sys.columns`' root page** (AT-S17): `HandleAlter`
    // holds no relation lock, so without it two cores renaming two columns
    // of one relation onto one name both pass the read.
    auto columns = store_.Get(kCatalogPageColumns);
    if (!columns.ok()) return columns.status();
    // A dropped column's name is free (BW-R6) **once its drop is decided**
    // (DT9: an object exists until its removal commits). The rename is
    // transactionless and the drop's overwrite in place, so a name taken
    // from an undecided drop would survive the drop's rollback as a second
    // live column of that name. Hence a walk that reads `trx_id`, which
    // `ScanAll` drops.
    const auto settled = [this](std::uint64_t trx) {
        return txn_ == nullptr || !txn_->IsInFlight(trx);
    };
    // The old name resolves as every written column name does - ignoring
    // case (`ColumnNamesCollide`), two or more live matches refused as
    // ambiguous (`Schema::FindColumn`'s rule, defended here under the hold).
    std::size_t old_matches = 0;
    std::uint32_t old_pos = 0;
    std::string old_stored;  // the renamed column's name as declared
    std::string holder;      // the colliding column's name as declared
    auto scanned = ForFirstRow<SysColumnRow>(
        store_, kCatalogPageColumns,
        [&](SysColumnRow& row, heap::PageView&, PageId, std::uint16_t,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
            if (row.rel_id != table_oid) return false;
            if (tuple.deleted && settled(tuple.trx_id)) return false;  // ScanAll's rule
            // Names collide ignoring case (`ColumnNamesCollide`); the column
            // being renamed collides only with itself, so `id` may become
            // `ID`.
            if (row.dropped()) {
                if (holder.empty() && !settled(tuple.trx_id) &&
                    ColumnNamesCollide(NameView(row.name), new_name)) {
                    holder = NameView(row.name);
                }
                return false;
            }
            if (ColumnNamesCollide(NameView(row.name), old_name)) {
                if (old_matches++ == 0) {
                    old_pos = row.pos;
                    old_stored = NameView(row.name);
                }
                return false;
            }
            if (holder.empty() && ColumnNamesCollide(NameView(row.name), new_name)) {
                holder = NameView(row.name);
            }
            return false;
        },
        nullptr, storage::PageAccess::kRead);
    if (!scanned.ok()) return scanned.status();
    if (old_matches == 0) {
        return Status::NotFound("the relation has no column named '" + std::string(old_name) +
                                 "'");
    }
    if (old_matches > 1) {
        return Status::InvalidArgument("column name '" + std::string(old_name) +
                                       "' is ambiguous: " + std::to_string(old_matches) +
                                       " live columns have it ignoring case");
    }
    if (!holder.empty()) {
        return Status::AlreadyExists("the relation already has a column named '" + holder +
                                     "' (names are unique ignoring case)");
    }
    // A rename onto the name the column already has, byte for byte, is
    // refused: it would rewrite, log and bump the version for nothing. A
    // new name differing from the stored one only in case is a rename.
    if (old_stored == new_name) {
        return Status::AlreadyExists("the column is already named '" + std::string(new_name) +
                                     "'");
    }

    auto acted = ForFirstRow<SysColumnRow>(
        store_, kCatalogPageColumns,
        [&](SysColumnRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
            if (row.rel_id != table_oid || row.dropped() || row.pos != old_pos ||
                (tuple.deleted && settled(tuple.trx_id))) {
                return false;
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
    if (!acted.value()) return Status::NotFound("the column vanished between check and write");

    BumpVersion("rename column");
    return Status::OK();
}

namespace {

// `AddColumnPosition`'s rule over a schema read under the CT7 hold.
StatusOr<std::uint16_t> ChooseAddPosition(const Schema& schema, std::uint16_t spare_slots) {
    std::uint16_t held = 0;
    std::uint16_t dropped_slots = 0;
    std::optional<std::uint16_t> clean;
    for (const SysColumnRow& existing : schema.columns) {
        if (!existing.in_slot()) continue;
        ++held;
        if (!existing.dropped()) continue;
        ++dropped_slots;
        if (existing.slot_clean() && !clean.has_value()) {
            clean = static_cast<std::uint16_t>(existing.pos);
        }
    }
    if (held < spare_slots) return static_cast<std::uint16_t>(schema.columns.size());
    if (clean.has_value()) return *clean;
    if (spare_slots == 0) {
        return Status::NotImplemented(
            "the relation reserved no column slots (CREATE TABLE ... SPARE n); adding a column "
            "to it would rewrite every row (invariant 13)");
    }
    std::string why = "all " + std::to_string(spare_slots) +
                      " of the relation's column slots are taken - a dropped column keeps its "
                      "slot - and adding a column would rewrite every row (invariant 13)";
    if (dropped_slots != 0) {
        why += "; " + std::to_string(dropped_slots) +
               " slot(s) are held by dropped columns, and PURGE DROPPED COLUMNS frees them";
    }
    return Status::NotImplemented(why);
}

}  // namespace

StatusOr<std::uint16_t> Catalog::AddColumnPosition(Oid table_oid) {
    // No hold: `AddColumn` asks again under its own.
    auto table_row = GetSysTableRow(table_oid);
    if (!table_row.ok()) return table_row.status();
    auto schema = ScanSchemaFromColumns(table_oid);
    if (!schema.ok()) return schema.status();
    return ChooseAddPosition(schema.value(), table_row.value().spare_slots);
}

Status Catalog::AddColumn(Oid table_oid, const SysColumnRow& column, std::uint16_t pos,
                          std::uint64_t trx_id, CatalogRowRef* inserted,
                          std::vector<CatalogRowChange>* reused) {
    // The slot count is fixed at CREATE, so it is read before the hold.
    auto table_row = GetSysTableRow(table_oid);
    if (!table_row.ok()) return table_row.status();
    // One hold of `sys.columns`' root page across the name check, the
    // position and the write (CT7), so two adds cannot take one name or one
    // slot.
    auto columns_page = store_.Get(kCatalogPageColumns);
    if (!columns_page.ok()) return columns_page.status();

    auto schema = ScanSchemaFromColumns(table_oid);
    if (!schema.ok()) return schema.status();
    schema.value().spare_slots = table_row.value().spare_slots;
    for (const SysColumnRow& existing : schema.value().columns) {
        if (!existing.dropped() &&
            ColumnNamesCollide(NameView(existing.name), NameView(column.name))) {
            return Status::AlreadyExists("the relation already has a column named '" +
                                         std::string(NameView(existing.name)) +
                                         "' (names are unique ignoring case)");
        }
    }
    auto chosen = ChooseAddPosition(schema.value(), table_row.value().spare_slots);
    if (!chosen.ok()) return chosen.status();
    if (chosen.value() != pos) {
        return Status::TxnConflict("the position an ADD COLUMN takes moved from " +
                                   std::to_string(pos) + " to " +
                                   std::to_string(chosen.value()) +
                                   " while its dependants were asked");
    }
    const bool reuse = pos < schema.value().columns.size();

    SysColumnRow row = column;
    row.rel_id = table_oid;
    row.pos = pos;
    row.notnull = false;
    row.flags = SysColumnRow::kColumnInSlot;
    auto width = RowLayout::ColumnWidth(row, inline_cell_width_);
    if (!width.ok()) return width.status();
    if (width.value() > kColumnSlotWidth) {
        return Status::NotImplemented(
            "a column slot is " + std::to_string(kColumnSlotWidth) + " bytes and this column's " +
            "cell is " + std::to_string(width.value()) +
            "; a string column added to a relation is varchar(16), which holds a value of any "
            "length and spills one past 13 bytes");
    }
    // The layout the relation will have, built before anything is written:
    // the row size it computes is the one every row already has, and a
    // reused slot keeps its offset and its bit (row_layout.cpp's j-th rule).
    if (reuse) {
        row.oid = schema.value().columns[pos].oid;
        row.generation = schema.value().columns[pos].generation + 1;
        schema.value().columns[pos] = row;
    } else {
        schema.value().columns.push_back(row);
    }
    if (Status s = CheckDeclarableColumnTypes(schema.value()); !s.ok()) return s;
    if (auto layout = RowLayout::Build(schema.value(), inline_cell_width_); !layout.ok()) {
        return layout.status();
    }

    if (reuse) {
        // In place, the oid kept (CD-OI-4 (a)): the row's Keystone word is its
        // oid, which a rollback's identity check reads back.
        auto acted = RewriteColumnRow(
            table_oid, pos,
            [](const SysColumnRow& old) { return old.dropped() && old.slot_clean(); },
            [&row](SysColumnRow& old) { old = row; }, trx_id, reused);
        if (!acted.ok()) return acted.status();
        if (!acted.value()) {
            // `ChooseAddPosition` read it under this hold.
            return Status::Corruption("sys.columns: the clean dropped column at position " +
                                      std::to_string(pos) + " of relation oid " +
                                      std::to_string(table_oid) +
                                      " was chosen and then not found under one hold");
        }
        BumpVersion("add column");
        return Status::OK();
    }

    auto col_oid = GenerateUserOid();
    if (!col_oid.ok()) return col_oid.status();
    if (Status s = InsertColumnRow(col_oid.value(), table_oid, row.pos, NameView(row.name),
                                   row.type_val, row.len, row.notnull, row.cabin_policy, trx_id,
                                   inserted, row.flags);
        !s.ok()) {
        return s;
    }
    if (inserted != nullptr) inserted->oid = col_oid.value();
    return Status::OK();
}

Status Catalog::DropColumn(Oid table_oid, std::uint16_t pos, std::string_view name,
                           std::uint64_t trx_id, std::vector<CatalogRowChange>* changed) {
    // One hold of `sys.columns`' root page across the check and the write
    // (CT7), as `RenameColumn` takes it.
    auto columns = store_.Get(kCatalogPageColumns);
    if (!columns.ok()) return columns.status();

    // The column is the one the caller resolved (`Schema::FindColumn`) and
    // asked its RESTRICT of, named by position - never resolved again here
    // by name: RENAME COLUMN takes no relation lock, so between the
    // caller's ask and this hold another core can move `name` onto another
    // column, and a second resolution would drop a column nobody asked the
    // RESTRICT of. The position must still be live and still carry `name`
    // (ignoring case); a column renamed in that window refuses the drop.
    std::string now_named;
    bool live = false;
    auto checked = ForFirstRow<SysColumnRow>(
        store_, kCatalogPageColumns,
        [&](SysColumnRow& row, heap::PageView&, PageId, std::uint16_t,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
            if (row.rel_id != table_oid || tuple.deleted || row.dropped() || row.pos != pos) {
                return false;
            }
            live = true;
            now_named = NameView(row.name);
            return true;
        },
        nullptr, storage::PageAccess::kRead);
    if (!checked.ok()) return checked.status();
    if (!live) {
        return Status::NotFound("the relation has no column named '" + std::string(name) + "'");
    }
    if (!ColumnNamesCollide(now_named, name)) {
        return Status::TxnConflict("column '" + std::string(name) + "' (position " +
                                   std::to_string(pos) + ") was renamed to '" + now_named +
                                   "' while the drop resolved it");
    }

    // Identity is the Keystone word at position 0 (invariant 11).
    if (pos == 0) {
        return Status::InvalidArgument("column '" + std::string(name) +
                                       "' is the tuple key and cannot be dropped "
                                       "(invariant 11)");
    }
    auto acted = RewriteColumnRow(
        table_oid, pos, [](const SysColumnRow& row) { return !row.dropped(); },
        [](SysColumnRow& row) { row.flags |= SysColumnRow::kColumnDropped; }, trx_id, changed);
    if (!acted.ok()) return acted.status();
    if (!acted.value()) {
        return Status::NotFound("the relation has no column named '" + std::string(name) + "'");
    }
    BumpVersion("drop column");
    return Status::OK();
}

StatusOr<bool> Catalog::RewriteColumnRow(Oid table_oid, std::uint16_t pos,
                                         FunctionRef<bool(const SysColumnRow&)> accept,
                                         FunctionRef<void(SysColumnRow&)> mutate,
                                         std::uint64_t trx_id,
                                         std::vector<CatalogRowChange>* changed) {
    return ForFirstRow<SysColumnRow>(
        store_, kCatalogPageColumns,
        [&](SysColumnRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
            if (row.rel_id != table_oid || tuple.deleted || row.pos != pos || !accept(row)) {
                return false;
            }
            const auto before = row.Encode();
            mutate(row);
            auto overwritten = OverwriteForDdl(ddl_undo_hook_, wal_, store_, page, page_id, i,
                                               tuple, kSysColumnsTable, row.oid, before,
                                               row.Encode(), trx_id);
            if (!overwritten.ok()) return overwritten.status();
            if (trx_id != kBootstrapXid && changed != nullptr) {
                changed->push_back(std::move(overwritten.value()));
            }
            return true;
        });
}

StatusOr<std::size_t> Catalog::MarkColumnsSlotClean(Oid table_oid,
                                                    std::span<const std::uint16_t> positions,
                                                    std::uint64_t trx_id,
                                                    std::vector<CatalogRowChange>* changed) {
    // DropColumn's hold (CT7): an ADD or DROP of this relation takes the
    // relation `X`, which the caller's `IX` excludes, and a RENAME COLUMN
    // refuses a dropped row; the hold orders this against both all the same.
    auto columns = store_.Get(kCatalogPageColumns);
    if (!columns.ok()) return columns.status();
    std::size_t flagged = 0;
    for (const std::uint16_t pos : positions) {
        auto acted = RewriteColumnRow(
            table_oid, pos,
            [](const SysColumnRow& row) { return row.dropped() && !row.slot_clean(); },
            [](SysColumnRow& row) { row.flags |= SysColumnRow::kColumnSlotClean; }, trx_id,
            changed);
        if (!acted.ok()) return acted.status();
        if (acted.value()) ++flagged;
    }
    if (flagged != 0) BumpVersion("columns cleaned");
    return flagged;
}

// ---- Namespaces (AF-T1) ---------------------------------------------------

StatusOr<Oid> Catalog::FindNamespaceOidByName(std::string_view name,
                                              const txn::ReadView* view) {
    // The reserved spellings first, and **before** the page scan rather
    // than after it: `CreateNamespace` refuses these two names, so a row
    // carrying one cannot exist - and if a file written by some other
    // build ever held one, answering it ahead of the reserved oid would
    // silently redirect `public.` at a user relation. Fail towards the
    // fixed answer.
    if (name == kNamespaceSysName) return kNamespaceSys;
    if (name == kNamespacePublicName) return kNamespacePublic;

    auto rows = ScanAll<SysObjectRow>(store_, kCatalogPageObjects, view, txn_);
    if (!rows.ok()) return rows.status();
    for (const SysObjectRow& row : rows.value()) {
        // `kTypeNamespace` and not `kTypeDroppedNamespace`: the retype is
        // what frees the name, exactly as a dropped table's does.
        if (row.type_oid == kTypeNamespace && NameView(row.name) == name) return row.oid;
    }
    // Not cached, for `FindTableOidByName`'s reason: a caller looking a
    // name up *expecting* NotFound before creating it would be answered
    // from a remembered absence that its own create just falsified.
    return Status::NotFound("no namespace with this name");
}

std::string Catalog::NamespaceSpelling(Oid namespace_oid, const txn::ReadView* view) {
    // A relation in `public` or `sys` has no row of its own spelling
    // (`well_known.hpp`), so the two are answered from the constants.
    if (namespace_oid == kNamespaceSys) return std::string(kNamespaceSysName);
    if (namespace_oid == kNamespacePublic) return std::string(kNamespacePublicName);
    auto live = ListNamespaces(view);
    if (live.ok()) {
        for (const SysObjectRow& row : live.value()) {
            if (row.oid == namespace_oid) return std::string(NameView(row.name));
        }
    }
    return "oid " + std::to_string(namespace_oid);
}

StatusOr<Oid> Catalog::ResolveRelation(std::string_view qualifier, std::string_view name,
                                       Oid default_namespace, std::uint32_t byte_offset,
                                       const txn::ReadView* view) {
    const auto at = [byte_offset](Status s) {
        return byte_offset == 0
                   ? s
                   : s.WithMessage(s.message() + " (byte " + std::to_string(byte_offset) + ")");
    };
    Oid ns = default_namespace;
    if (!qualifier.empty()) {
        auto named = FindNamespaceOidByName(qualifier, view);
        if (!named.ok()) {
            if (named.status().code() != StatusCode::kNotFound) return named.status();
            return at(Status::NotFound("no namespace named '" + std::string(qualifier) + "'"));
        }
        ns = named.value();
    }
    if (ns == kNamespaceSys) {
        return at(Status::Unsupported(
            "'" + std::string(kNamespaceSysName) + "." + std::string(name) +
            "' is a catalog relation: read it as a catalog view (SELECT ... FROM sys." +
            std::string(name) + "); no other statement names one"));
    }

    auto oid = FindTableOidByName(ns, name, view);
    if (oid.ok() || oid.status().code() != StatusCode::kNotFound) return oid;

    // The miss, and which other namespaces hold the name - each is a
    // relation the statement could have meant, reached only by its qualifier.
    const std::string written =
        qualifier.empty() ? std::string(name) : std::string(qualifier) + "." + std::string(name);
    auto rows = ScanAll<SysObjectRow>(store_, kCatalogPageObjects, view, txn_);
    if (!rows.ok()) return rows.status();
    std::string catalog_hint;
    std::string where;
    std::string example;
    for (const SysObjectRow& row : rows.value()) {
        if (row.type_oid != kTypeTable || row.namespace_oid == ns || NameView(row.name) != name) {
            continue;
        }
        if (row.namespace_oid == kNamespaceSys) {
            catalog_hint = "; '" + std::string(name) +
                           "' is a catalog relation: read it as sys." + std::string(name);
            continue;
        }
        const std::string spelled = NamespaceSpelling(row.namespace_oid, view);
        where += (where.empty() ? "'" : ", '") + spelled + "'";
        if (example.empty()) example = spelled + "." + std::string(name);
    }
    std::string message = "no relation named '" + written + "'";
    if (!catalog_hint.empty() || !where.empty()) {
        message += " in namespace '" + NamespaceSpelling(ns, view) + "'" + catalog_hint;
        if (!where.empty()) {
            message += "; '" + std::string(name) + "' is in namespace " + where + " - write " +
                       example;
        }
    }
    return at(Status::NotFound(message));
}

StatusOr<std::vector<SysObjectRow>> Catalog::ListNamespaces(const txn::ReadView* view) {
    auto rows = ScanAll<SysObjectRow>(store_, kCatalogPageObjects, view, txn_);
    if (!rows.ok()) return rows.status();
    std::vector<SysObjectRow> out;
    for (const SysObjectRow& row : rows.value()) {
        if (row.type_oid == kTypeNamespace) out.push_back(row);
    }
    return out;
}

StatusOr<Oid> Catalog::CreateNamespace(std::string_view name, std::uint64_t trx_id,
                                        std::vector<CatalogRowRef>* written) {
    // Same validator as a rename's, and deliberately the same one: a name
    // this refuses is a name `SetName` would truncate, and a truncated
    // name is not the one that was asked for.
    if (Status s = CheckRenameName("namespace", name); !s.ok()) return s;

    if (name == kNamespaceSysName || name == kNamespacePublicName) {
        return Status::InvalidArgument("catalog: '" + std::string(name) +
                                        "' is a reserved namespace name (well_known.hpp)");
    }
    // A user namespace may not take a well-known object's name either -
    // "namespaceSys" and "namespacePublic" are what those rows carry, and
    // two objects answering one `GetByName` is the collision the registry
    // has no way to report.
    if (sys_objects_.GetByName(name) != nullptr) {
        return Status::AlreadyExists("catalog: '" + std::string(name) +
                                      "' is a well-known catalog object");
    }

    // **Check and insert under one hold** (AT-S17), `CreateTable`'s: page 6
    // held across both, so no other core writes the name in between. The
    // check is **unfiltered** - `CREATE TABLE`'s duplicate-name check for
    // its reason (`ddl-transactional.md` §6): resolving it under a view
    // would hide another transaction's uncommitted namespace of this name
    // and let both creates succeed. And it asks after a **pending drop**,
    // which this function owed from the moment AF-T3 made `DROP NAMESPACE`
    // a statement a transaction could hold open: the retype frees the name
    // at once and its rollback restores it. Every *resolution* route
    // filters; this is a collision check, not a resolution.
    //
    // The oid is issued first, so the hold nests nothing but page 6's own
    // chain growth - `GenerateUserOid`'s first use seeds from `sys.columns`
    // too. A refusal burns it, which `CreateTable`'s argument permits.
    auto generated_oid = GenerateUserOid();
    if (!generated_oid.ok()) return generated_oid.status();
    const Oid new_oid = generated_oid.value();

    auto names = store_.Get(kCatalogPageObjects);
    if (!names.ok()) return names.status();
    if (Status s = CheckNameFree(name, kTypeNamespace, kNamespaceSys, trx_id,
                                 /*own_drop_frees=*/true);
        !s.ok()) {
        return s;
    }

    CatalogRowRef where;
    // `namespace_oid = new_oid`: a namespace's own namespace is itself,
    // which is `InitWellKnownObjects`' convention for the two that
    // bootstrap - so a user namespace and a well-known one decode the
    // same way and `IsSystemNamespace` answers false for this row without
    // being told about it.
    if (Status s = InsertObjectRow(new_oid, new_oid, kTypeNamespace, name, trx_id, &where);
        !s.ok()) {
        return s;
    }
    // **`InsertObjectRow` does not fill `oid`, and a trail entry without it
    // cannot be rolled back.** `CreateTable` sets it by hand at each of its
    // three inserts; this one did not, so the entry recorded tuple key 0, the
    // abort found the row's real oid at that slot, decided it had moved,
    // and asked `RowLocatorForRollback` to relocate a *catalog* row - which
    // answers "no columns for this rel_id", because a bootstrap relation
    // has no `sys.columns` entries. Unreachable until AF-T3 made
    // `CREATE NAMESPACE` a statement a transaction could hold.
    where.oid = new_oid;
    if (written != nullptr) written->push_back(where);
    return new_oid;
}

Status Catalog::DropNamespace(Oid namespace_oid, std::uint64_t trx_id,
                               std::vector<CatalogRowChange>* written) {
    if (namespace_oid == kNamespaceSys || namespace_oid == kNamespacePublic) {
        return Status::InvalidArgument(
            "catalog: namespace oid " + std::to_string(namespace_oid) +
            " is a reserved namespace and cannot be dropped");
    }

    // **RESTRICT, asked before anything is written** - the rule DROP TABLE
    // already uses for foreign keys and assertions. A cascade here would
    // delete relations whose data has nothing to do with the namespace;
    // emptying it silently would be worse. The refusal names the relation
    // that blocked it, not a count: the user's next act is to drop or move
    // that relation.
    //
    // **Under page 6 held exclusive through the retype** (AT-S17b): a
    // create's membership is written under the same hold, and it checks
    // the namespace is live there (`CreateTable`), so the two are one act
    // across cores - the create is refused, or the drop sees its row.
    auto names = store_.Get(kCatalogPageObjects);
    if (!names.ok()) return names.status();
    if (Status s = CheckNamespaceEmpty(namespace_oid, trx_id); !s.ok()) return s;

    CatalogRowChange retype_change;
    auto retyped = ForFirstRow<SysObjectRow>(
        store_, kCatalogPageObjects,
        [&](SysObjectRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
            if (row.type_oid != kTypeNamespace || row.oid != namespace_oid) return false;
            // `DropTable`'s step 1, one row wide.
            const auto before = row.Encode();
            row.type_oid = kTypeDroppedNamespace;
            auto overwritten = OverwriteForDdl(ddl_undo_hook_, wal_, store_, page, page_id, i,
                                               tuple, kSysObjectsTable, row.oid, before,
                                               row.Encode(), trx_id);
            if (!overwritten.ok()) return overwritten.status();
            retype_change = std::move(overwritten.value());
            return true;
        });
    if (!retyped.ok()) return retyped.status();
    if (!retyped.value()) return Status::NotFound("no sys.objects row for this namespace");
    if (trx_id != kBootstrapXid && written != nullptr) written->push_back(retype_change);

    BumpVersion("drop namespace");
    return Status::OK();
}

Status Catalog::DropTable(Oid table_oid, std::vector<std::uint64_t>& dropped_cabins,
                          std::uint64_t trx_id, std::vector<CatalogRowChange>* written,
                          PendingRoots* owed_out) {
    // Transactional only when a caller supplies an id (DT5). Without one
    // this is the path that always existed: retype, then **retire** the
    // dependents outright, which no rollback can put back. No production
    // drop takes it - autocommit runs in an implicit transaction and
    // delete-marks like an explicit one - so only a bootstrap-id caller,
    // which is a test, retires (BF-Q12 (a)).
    const bool transactional = trx_id != kBootstrapXid;
    auto note = [written](const CatalogRowChange& change) {
        if (written != nullptr) written->push_back(change);
    };
    // 1. The tombstone (DT2): retype, never retire. The row is the oid
    //    floor's evidence, and a reissued oid could serve a dead table's
    //    row as a live answer through a stale advisory structure. **It also
    //    carries what the drop owes** (BF-R2): the anchor and the var-heap
    //    root, read here, before anything is written, from the `sys.tables`
    //    row the sweep below retires - the only other copy, and one the
    //    purge or the mount removes long before a reclaim can run.
    auto owed = GetSysTableRow(table_oid);
    if (!owed.ok()) return owed.status();
    const std::uint64_t pending_roots = PackPendingRoots(
        PendingRoots{owed.value().anchor_page_id, owed.value().varheap_page_id});
    CatalogRowChange retype_change;
    auto retyped = ForFirstRow<SysObjectRow>(
        store_, kCatalogPageObjects,
        [&](SysObjectRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
            if (row.type_oid != kTypeTable || row.oid != table_oid) return false;
            // AF-P3, as at the rename above: identity, not permission.
            if (IsSystemNamespace(row.namespace_oid)) {
                return Status::InvalidArgument("relation '" +
                                                std::string(NameView(row.name)) +
                                                "' is a system relation and cannot be dropped");
            }
            const auto before = row.Encode();
            row.type_oid = kTypeDroppedTable;
            row.pending_roots = pending_roots;
            auto overwritten = OverwriteForDdl(ddl_undo_hook_, wal_, store_, page, page_id, i,
                                               tuple, kSysObjectsTable, row.oid, before,
                                               row.Encode(), trx_id);
            if (!overwritten.ok()) return overwritten.status();
            retype_change = std::move(overwritten.value());
            return true;
        });
    if (!retyped.ok()) return retyped.status();
    if (!retyped.value()) return Status::NotFound("no sys.objects row for this relation");
    if (transactional) note(retype_change);

    // 2. Everything the relation owns retires (DT3's "dependents out").
    //    Retired, not delete-marked - DropCabin() states the argument:
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
                    // where they are, which is what a rollback needs; the
                    // mark is retired later by the catalog's own purge once
                    // every reader has settled (`PurgeSettledDeleteMarks`).
                    if (ddl_undo_hook_) {
                        if (Status s = ddl_undo_hook_({DdlUndoEvent::Kind::kDeleteMark, page_id,
                                                       i, {}, tuple.trx_id, tuple.undo_ptr,
                                                       UndoTupleKeyOf(tuple.payload)});
                            !s.ok()) {
                            return s;
                        }
                    }
                    if (Status s = DeleteMarkLogged(wal_, store_, page, page_id, i, trx_id);
                        !s.ok()) {
                        return s;
                    }
                    NoteMark();
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
    if (owed_out != nullptr) *owed_out = UnpackPendingRoots(pending_roots);
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
    auto schema = ScanSchemaFromColumns(rel_id);
    if (!schema.ok()) return schema;
    // After the column scan, so a relation with no columns answers NotFound
    // as it always has (BW-R2's slot count, as `InitTableAccess` sets it).
    auto table_row = GetSysTableRow(rel_id);
    if (!table_row.ok()) return table_row.status();
    schema.value().spare_slots = table_row.value().spare_slots;
    return schema;
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
    access.oid = oid;
    access.schema = std::move(schema.value());
    // The slot count rides with the columns because the layout is the
    // schema's alone (BW-R2).
    access.schema.spare_slots = table_row.value().spare_slots;
    access.desc_page_id = table_row.value().desc_page_id;
    access.clustered_type = table_row.value().clustered_type;
    access.varheap_page_id = table_row.value().varheap_page_id;
    access.anchor_page_id = table_row.value().anchor_page_id;

    // PW2-2 (workplan-peer-writer.md §7a): the durable truth for a user
    // relation's clustered root is its **anchor page** - from PW2-3 on,
    // sys.tables.desc_page_id is CREATE-fixed and a root move writes the
    // anchor, so the row's value can be behind. Resolved here, at fill;
    // between fills the cached access is updated in place on a root move,
    // the same license the in-place root updates always had. A system
    // relation (no anchor) keeps the row's value: its fixed-page root
    // never moves.
    // **Every core** (AT-S5c): a write runs where the session is, so every
    // core walks every relation's tree, and a core that skipped this would
    // keep the **CREATE-time** root out of the `sys.tables` row: after any
    // level growth it would descend into what had become the leftmost
    // subtree and write rows into a leaf that could not hold them. One
    // frame table serves every core (AM-S2 step 3), and the schema word is
    // bumped at the repoint (`AdoptRoot`), so a stale memo is dropped at
    // the next task boundary.
    // The owner-scoped fill retired (AT-S5c); its history is at cf889b63.
    // Held for the whole fill: a re-fetch per index row could fail mid-fill
    // under a sized pool and leave the access half-anchored (clustered root
    // from the anchor, index roots from the CREATE-time rows - a silently
    // wrong tree for maintenance to append into). One read, one failure
    // decision.
    std::optional<storage::PageRef> anchor_held;
    if (access.anchor_page_id != kInvalidPageId) {
        auto anchor = store_.GetForRead(access.anchor_page_id);
        if (anchor.ok()) {
            if (Status s = storage::ValidatePageHeader(anchor.value().bytes(),
                                                       PageType::kAnchor);
                !s.ok()) {
                return s;
            }
            // Checked redundancy one level up: a row naming another relation's anchor would silently walk the
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
        } else {
            // **An unfaultable anchor is a refusal, not a fall-through**
            // (AT-S5c): a fall-back would land on the **CREATE-time** root
            // of a relation another core may have grown - a silent wrong
            // answer - and for the *index* roots filled from this same
            // anchor the coverage check underneath (AT-S15) refuses only a
            // key outside the stale subtree, not the level growth a divide
            // there would make.
            //
            // Refusing is recoverable and admitting is not, which is the
            // rule this engine states elsewhere; a caller that cannot read
            // the one page carrying a relation's current roots does not
            // know where the relation is.
            return anchor.status().WithContext(
                "resolving the clustered root of relation oid " + std::to_string(oid) +
                " through anchor page " + std::to_string(access.anchor_page_id) +
                "; without it this core cannot tell where the relation's tree begins");
        }
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
        // An expression or predicate Cabin joins the list; its `F` is the
        // declaration loader's to fill (below), from sys.cabin_exprs.
        if (cabin.shape == kCabinShapeExpression || cabin.shape == kCabinShapePredicate) {
            access.expr_cabins.push_back(
                TableAccess::ExprCabinRef{cabin.cabin_id, cabin.origin, cabin.shape, cabin.expr_id, {}});
            continue;
        }
        if (cabin.shape != kCabinShapeColumn) continue;
        if (cabin.column_no == 0 || cabin.column_no >= access.cabin_ids.size()) continue;
        access.cabin_ids[cabin.column_no] = TableAccess::CabinRef{cabin.cabin_id, cabin.origin};
    }
    // Read off `cabin_ids` rather than pushed in the loop, so the list is
    // ascending and names each position once whatever the page order.
    for (std::uint16_t col = 1; col < access.cabin_ids.size(); ++col) {
        if (access.cabin_ids[col].id != 0) access.cabin_columns.push_back(col);
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
        // count is as loud here as it is for the clustered arm.
        //
        // **A live index with no slot is `Corruption`** (BY-R10): the slot
        // is written at create (PW2-3) and removed only once its tree is
        // reclaimed (BY-R5), so its absence means a freed tree, and the
        // row's creation root names a page that may already be another's.
        if (anchor_held.has_value()) {
            auto slot = storage::AnchorIndexRoot(anchor_held->bytes(), row.index_oid);
            if (!slot.ok()) return slot.status();
            if (slot.value() == kInvalidPageId) {
                return Status::Corruption("index oid " + std::to_string(row.index_oid) +
                                          " has a live sys.indexes row but no slot in its "
                                          "relation's anchor page " +
                                          std::to_string(anchor_held->page_id()));
            }
            ref.root_page_id = slot.value();
        }
        ref.key_width = row.key_width;
        ref.entry_width = row.entry_width;
        ref.nkeys = row.nkeys;
        ref.ncovered = row.ncovered;
        ref.unique = (row.flags & kIndexFlagUnique) != 0;
        ref.name = NameView(row.name);
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

    // The relation's Transitions (BV-S2) and its expression Cabins' `F`
    // (AQ-S1), failing shut for the fkeys' reason - a Cabin one core
    // witnesses and another cannot would lose a row. After the anchor is let
    // go: the loader reads sys.transitions, sys.cabin_exprs and their
    // var-heaps, and holds nothing of this relation. A catalog relation
    // carries neither, and asking would recurse into its own fill.
    anchor_held.reset();
    if (declaration_loader_ && oid >= kUserOidStart) {
        if (Status s = declaration_loader_(access); !s.ok()) return s;
    }

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

StatusOr<std::uint64_t> Catalog::AllocateRowId(Oid table_oid) {
    std::uint64_t issued = 0;
    auto acted = ForFirstRow<SysTableRow>(
        store_, kCatalogPageTables,
        [&](SysTableRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
        if (row.oid != table_oid) return false;

        // No key-mode refusal: this is what every INSERT that omits the tuple key
        // calls, whatever the relation. The id it hands out is safe from a
        // caller-supplied one for the same reason it always was - the mark
        // only ever moves forward, and AdmitExplicitRowId moves it past every
        // supplied id at or above it. A supplied id below the mark is placed
        // (BD-R7), and the two sources meet only on an issued id a named key
        // took before it was placed - serialised by its row unit, the issue
        // re-drawn if it lost (`CommandDispatcher::InsertOneRow`). It runs
        // under no page (BD-R6).
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

Status CheckNamedRowIdSpellable(std::uint64_t id) {
    if (id < kFirstRowId) {
        return Status::OutOfRange("tuple key " + std::to_string(id) +
                                  " is below the first issuable id (" +
                                       std::to_string(kFirstRowId) +
                                       "); 0 is reserved for \"unset\"");
    }
    if (id > kMaxKeystoneId) {
        return Status::OutOfRange("tuple key " + std::to_string(id) +
                                  " does not fit the Keystone id space (max " +
                                       std::to_string(kMaxKeystoneId) + ")");
    }
    return Status::OK();
}

Status Catalog::AdmitExplicitRowId(Oid table_oid, std::uint64_t id) {
    // Spellability first, before the catalog page is touched at all: an id
    // outside the Keystone field cannot be stored by any path, so there is
    // nothing to check a relation for. The insert path judged its literal
    // before its borrow; this keeps the call's contract whole for any other
    // caller.
    if (Status s = CheckNamedRowIdSpellable(id); !s.ok()) return s;

    auto acted = ForFirstRow<SysTableRow>(
        store_, kCatalogPageTables,
        [&](SysTableRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
            if (row.oid != table_oid) return false;

            // **Below the mark it is placed anyway** (BD-R6, BD-R7,
            // `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`;
            // W2 of BZ): a leaf places the key where it sorts, so the mark
            // gates nothing and is left where it is - advance or nothing.
            if (id < row.next_id) return true;

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
    // `next_id`'s advance is not cached and publishes nothing: no cached
    // field moved.
    return Status::OK();
}

Status Catalog::PublishRoot(Oid rel_oid, std::uint64_t index_oid, PageId new_root,
                           PageId anchor_page_id) {
    if (Status s = WriteAnchorRoot(anchor_page_id, rel_oid, index_oid, new_root, wal::kNoTxnId);
        !s.ok()) {
        return s;
    }
    AdoptRoot(rel_oid, index_oid, new_root);
    return Status::OK();
}

void Catalog::AdoptRoot(Oid rel_oid, std::uint64_t index_oid, PageId new_root) {
    // PW2-3/PW2-4: a root move writes the **anchor alone** - the sys.tables
    // and sys.indexes rows are CREATE-fixed, which is the whole point of the
    // indirection (a growth writes a relation page, never a catalog page) -
    // and this core's cached entry updates **in place**, so the running
    // statement's TableAccess, and MaintainIndexes' `ix` inside it, stay
    // valid (catalog_cache.hpp's one-field license).
    if (index_oid == 0) {
        cache_.UpdateDescPage(rel_oid, new_root);
    } else {
        cache_.UpdateIndexRoot(rel_oid, index_oid, new_root);
    }
    // **And every other core is told** (AT-S5c). A write runs where the
    // session is, so every core's TableAccess memoizes this root, and a
    // peer holding the pre-growth one descends into what is now the new
    // root's *leftmost subtree*. Since AT-S15/AT-S16 that costs a retryable
    // refusal rather than a misplaced row or a second root - the coverage
    // checks and the grown-over mark - and this bump is what bounds those
    // refusals to one task. `BumpWord` and not `BumpVersion`: this core's
    // entry was just repaired and adopts the bump, while every other core
    // drops its memo at its next task boundary and re-reads the anchor
    // (`catalog.md` CT2).
    BumpWord(Tally::kWordMovesRoot);
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        // Rare and structural, so Debug rather than Trace.
        log_->Debug("catalog", "relation oid " + std::to_string(rel_oid) +
                                   (index_oid == 0 ? std::string()
                                                   : " index oid " + std::to_string(index_oid)) +
                                   " root -> " + std::to_string(new_root));
    }
}

// ---- sys.patterns ----------------------------------------------------

namespace {

// The identity + location + policy halves of a pattern row - everything DDL
// alone can change. Heat is dropped on the floor here on purpose; see
// schema.hpp.
PatternAccess AccessOf(const SysPatternRow& row) noexcept {
    PatternAccess access{};
    access.oid = row.oid;
    access.fetch_id = row.fetch_id;
    access.fingerprint_version = row.fingerprint_version;
    access.waystone_root = row.waystone_root;
    access.stmt_class = row.stmt_class;
    access.dir_depth = row.dir_depth;
    access.flags = row.flags;
    return access;
}

}  // namespace

StatusOr<const std::vector<SysTypeRow>*> Catalog::ListTypes() { return EnsureTypes(); }

StatusOr<std::vector<SysPatternRow>> Catalog::ListPatterns() {
    return ScanAll<SysPatternRow>(store_, kCatalogPagePatterns, nullptr, txn_);
}

StatusOr<SysPatternRow> Catalog::GetSysPatternRow(std::uint64_t fetch_id) {
    auto rows = ScanAll<SysPatternRow>(store_, kCatalogPagePatterns, nullptr, txn_);
    if (!rows.ok()) return rows.status();

    for (const auto& row : rows.value()) {
        if (row.fetch_id != fetch_id) continue;
        // Rows from another fingerprint revision are invisible here, and
        // this is the only place that decision is made. Putting the filter
        // in the row lookup rather than in each caller is what stops a
        // stale row from shadowing a current one when both are on the page
        // - which is the state a version bump leaves behind, since nothing
        // rewrites the old rows.
        if (!parser::IsCurrentFingerprintVersion(row.fingerprint_version)) continue;
        return row;
    }
    return Status::NotFound("no current sys.patterns row for this fetch_id");
}

StatusOr<const PatternAccess*> Catalog::FindOptedPattern(std::uint64_t fetch_id) {
    // Nothing opted on the instance: nothing fills, and the answer costs
    // the one load (CF-S1c).
    if (NoOptedPatterns()) return Status::NotFound("no pattern is opted in");
    if (!cache_.opted_patterns_filled()) {
        // **The whole opted set, once per generation**: a fill of one entry
        // would leave the map unable to answer a miss without a scan. The
        // version filter is applied here, so a row from another revision
        // never reaches the map - by construction rather than by a check
        // every reader has to remember.
        auto rows = ScanAll<SysPatternRow>(store_, kCatalogPagePatterns, nullptr, txn_);
        if (!rows.ok()) return rows.status();
        std::vector<PatternAccess> opted;
        for (const SysPatternRow& row : rows.value()) {
            if (IsCurrentOptIn(row)) opted.push_back(AccessOf(row));
        }
        cache_.FillOptedPatterns(std::move(opted));
    }
    // A miss is an answer, not a reason to read the page: the map is
    // complete for this generation (catalog_cache.hpp's soundness rule).
    if (const PatternAccess* cached = cache_.FindPattern(fetch_id); cached != nullptr) {
        return cached;
    }
    return Status::NotFound("the pattern is not opted in");
}

void Catalog::MoveOptedCount(bool up) noexcept {
    if (words_ == nullptr) {
        if (up) {
            ++local_opted_;
        } else if (local_opted_ > 0) {
            --local_opted_;
        }
        return;
    }
    if (up) {
        words_->opted_count.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    // Saturating, so a CAS rather than a `fetch_sub`: every mover writes
    // under page 9's hold, but the boot's store is not one of them.
    std::uint64_t now = words_->opted_count.load(std::memory_order_acquire);
    while (now > 0 &&
           !words_->opted_count.compare_exchange_weak(now, now - 1, std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
    }
}

void Catalog::StoreOptedCount(std::uint64_t value) noexcept {
    if (words_ != nullptr) {
        words_->opted_count.store(value, std::memory_order_release);
    } else {
        local_opted_ = value;
    }
}

StatusOr<bool> Catalog::MutatePatternRow(std::uint64_t fetch_id,
                                         const std::function<bool(SysPatternRow&)>& mutate) {
    bool wrote = false;
    auto acted = ForFirstRow<SysPatternRow>(
        store_, kCatalogPagePatterns,
        [&](SysPatternRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
        if (row.fetch_id != fetch_id) return false;
        // The same version filter GetSysPatternRow() applies, and for the
        // same reason: a row from another revision names a shape that is
        // not the one it claims, so it is not this pattern and must not be
        // written through.
        if (!parser::IsCurrentFingerprintVersion(row.fingerprint_version)) return false;

        if (!mutate(row)) return true;
        auto encoded = row.Encode();
        // In place, never delete+insert: the row size is unchanged, and a
        // pattern that briefly does not exist is a pattern a concurrent
        // lookup misses.
        if (Status s = OverwriteLogged(wal_, store_, page, page_id, i, encoded, wal::kNoTxnId, tuple.trx_id, tuple.undo_ptr); !s.ok()) {
            return s;
        }
        wrote = true;
        return true;
    });
    if (!acted.ok()) return acted.status();
    if (!acted.value()) return Status::NotFound("no sys.patterns row for this fetch_id");
    return wrote;
}

StatusOr<Catalog::PatternWaystoneWrite> Catalog::SetPatternWaystone(std::uint64_t fetch_id,
                                                                   std::uint8_t stmt_class,
                                                                   bool on, PageId root) {
    if (!on && root != kInvalidPageId) {
        return Status::InvalidArgument("catalog: WAYSTONE OFF takes no directory root");
    }
    // The append point less one: everything below the append point has been
    // appended, and `IsDurable` is strict (`wal/durability.hpp`), so this is
    // the LSN whose durability covers every record written so far.
    const auto append_bound = [this]() -> std::uint64_t {
        return wal_ != nullptr && wal_->appended_lsn() > 0 ? wal_->appended_lsn() - 1 : 0;
    };
    PatternWaystoneWrite out;
    // Whether an opt-in was undone - OFF's answer, which is about the bit,
    // not about whether a stray root was cleared beside it.
    bool cleared_opt_in = false;
    bool root_used = false;
    Status wrote = Status::OK();
    {
        // **The relation's root page, held exclusive across the decision and
        // the write** (AT-S7's hold, `RecordAccess`'s reason): every core
        // runs `WAYSTONE ON`, and the existence check and the insert under it
        // are a read-then-write two cores would both pass. The loser's row
        // would be a second current row for one shape, which
        // `GetSysPatternRow` never returns - it answers at the first match.
        auto held = store_.Get(kCatalogPagePatterns);
        if (!held.ok()) {
            // Not returned from here: the caller's page is unused, and it
            // is reported as leaked below like every other unused page.
            wrote = held.status();
        } else if (on) {
            // A 0->1 change of the bit, which the opted count follows once
            // the write has succeeded (`MutatePatternRow` writes current
            // rows only).
            bool opts = false;
            auto mutated = MutatePatternRow(fetch_id, [&](SysPatternRow& row) {
                const bool opted = (row.flags & kPatternWaystone) != 0;
                const bool roots = root != kInvalidPageId && !HasWaystoneDirectory(row);
                if (opted && !roots) return false;
                row.flags = static_cast<std::uint16_t>(row.flags | kPatternWaystone);
                if (roots) {
                    row.waystone_root = root;
                    row.dir_depth = 1;
                }
                root_used = roots;
                opts = !opted;
                return true;
            });
            if (mutated.ok()) {
                out.written = mutated.value();
                if (out.written && opts) MoveOptedCount(/*up=*/true);
            } else if (mutated.status().code() != StatusCode::kNotFound) {
                // A failed overwrite wrote nothing, so the page it carried
                // is reachable from nothing.
                root_used = false;
                wrote = mutated.status();
            } else {
                // No current row: the shape's first opt-in, or its first
                // since a fingerprint bump left the old row lapsed beside it.
                auto oid = AllocateRowId(kSysPatternsTable);
                if (!oid.ok()) {
                    wrote = oid.status();
                } else {
                    SysPatternRow row{};
                    row.oid = oid.value();
                    row.fetch_id = fetch_id;
                    row.last_seen = 0;
                    // **The version is stamped here, not passed in**: no
                    // caller has any business opting a pattern in under a
                    // revision other than the one that computed its
                    // `fetch_id`, so the row is current by construction.
                    row.fingerprint_version = parser::kFingerprintVersion;
                    row.use_count = 0;
                    row.stmt_class = stmt_class;
                    // A directory only where the caller made one: with
                    // `waystone_recording` off nothing records into it, and
                    // the mount roots the row once recording is on.
                    row.waystone_root = root;
                    row.dir_depth = root != kInvalidPageId ? 1 : 0;
                    row.flags = kPatternWaystone;
                    // Reserved since CF-S1b, written 0 (rows.hpp).
                    row.origin = 0;
                    wrote = InsertRow(wal_, ddl_undo_hook_, store_, kCatalogPagePatterns, row,
                                      kBootstrapXid);
                    out.written = wrote.ok();
                    if (out.written) MoveOptedCount(/*up=*/true);
                    root_used = wrote.ok() && root != kInvalidPageId;
                }
            }
        } else {
            // **Every row of the `fetch_id`, of any version, in one walk**,
            // not `MutatePatternRow`'s first current match: after a
            // fingerprint bump a lapsed opted row and the current row a
            // re-issued ON inserted commonly share the id, and stopping at
            // the first would clear one, answer OK, and leave the other
            // recording. The visitor never accepts, so the walk runs the
            // whole chain under the same kWrite walk and budget as every
            // catalog mutation.
            auto walked = ForFirstRow<SysPatternRow>(
                store_, kCatalogPagePatterns,
                [&](SysPatternRow& row, heap::PageView& page, PageId page_id, std::uint16_t slot,
                    const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
                    if (row.fetch_id != fetch_id) return false;
                    const bool opted = (row.flags & kPatternWaystone) != 0;
                    // The count holds current-version rows only: a lapsed
                    // row's bit was never in it.
                    const bool counted = IsCurrentOptIn(row);
                    if (!opted && !HasWaystoneDirectory(row)) return false;
                    cleared_opt_in = cleared_opt_in || opted;
                    row.flags = static_cast<std::uint16_t>(row.flags & ~kPatternWaystone);
                    row.waystone_root = kInvalidPageId;
                    row.dir_depth = 0;
                    const auto encoded = row.Encode();
                    if (Status s = OverwriteLogged(wal_, store_, page, page_id, slot, encoded,
                                                   wal::kNoTxnId, tuple.trx_id, tuple.undo_ptr);
                        !s.ok()) {
                        return s;
                    }
                    out.written = true;
                    if (counted) MoveOptedCount(/*up=*/false);
                    return false;
                });
            wrote = walked.status();
        }
        // `unchanged` wrote nothing, so its bound is read **here, under the
        // hold**: every writer of the bit writes under it, so the append
        // point now is past whichever record set the state this answers.
        if (wrote.ok() && !out.written) out.durable_at = append_bound();
    }
    if (root != kInvalidPageId && !root_used && log_ != nullptr &&
        log_->enabled(LogLevel::kWarn)) {
        log_->Warn("waystone", "directory page " + std::to_string(root) +
                                   " leaked by WAYSTONE ON");
    }
    if (!wrote.ok()) {
        // OFF's walk can fail after it overwrote earlier rows of the
        // `fetch_id`; those writes stand, so the word moves for them as for
        // a whole write, or every core's cache would keep the bit and root
        // they cleared. The refusal then reports the row it stopped at.
        if (out.written) BumpVersion("waystone opt-in");
        return wrote;
    }
    if (!out.written) return out;

    // After the hold is released and after the write, never before (D21's
    // order): a peer that revalidated against the new word while this
    // catalog still held its old entry would read an opt-in its writer
    // disagrees with. Every core's next statement drops its pattern entries
    // and refills from the page, so the bit reaches a peer within one
    // statement.
    BumpVersion("waystone opt-in");
    // After the write: it bounds this write's records, and possibly later
    // ones - a later bound, never an earlier one.
    out.durable_at = append_bound();
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("waystone", std::string(on ? "opted in" : "opted out") + " fetch_id " +
                                   std::to_string(fetch_id));
    }
    // OFF that met only a stray root wrote, but undid no opt-in: `unchanged`.
    if (!on && !cleared_opt_in) out.written = false;
    return out;
}

StatusOr<std::uint64_t> Catalog::RenewPatternDirectories(bool waystone_recording) {
    // ---- Pass 1: what is opted, read under no write hold ------------------
    auto rows = ScanAll<SysPatternRow>(store_, kCatalogPagePatterns, nullptr, txn_);
    if (!rows.ok()) return rows.status();
    std::unordered_set<std::uint64_t> opted_fetch_ids;  // current-version rows only
    std::vector<Oid> renew;                             // the rows to root, in page order
    for (const SysPatternRow& row : rows.value()) {
        if (!IsCurrentOptIn(row)) continue;
        opted_fetch_ids.insert(row.fetch_id);
        renew.push_back(row.oid);
    }
    // A lapse is reported only while it is unreplaced: once the operator
    // re-issued ON, the current row is opted and the old one is history.
    std::map<std::uint32_t, std::uint64_t> lapsed;  // fingerprint version -> rows
    for (const SysPatternRow& row : rows.value()) {
        if ((row.flags & kPatternWaystone) == 0) continue;
        if (parser::IsCurrentFingerprintVersion(row.fingerprint_version)) continue;
        if (opted_fetch_ids.count(row.fetch_id) != 0) continue;
        ++lapsed[row.fingerprint_version];
    }

    // ---- The directories, with no catalog page latch held -----------------
    //
    // `CreateDirPage` is a headerless create, which can zero an id the
    // device still holds and wait on the data-sync thread's sync
    // (`device_page_store.cpp`); a catalog page latch is never held across
    // that, so the pages are made here, between the two walks.
    std::unordered_map<Oid, PageId> fresh;
    if (waystone_recording) {
        for (const Oid oid : renew) {
            auto made = stats::CreateDirPage(store_);
            if (!made.ok()) {
                // The row keeps its bit and goes without a directory until
                // the next mount or an OFF and ON; nothing records for it
                // meanwhile, which costs performance only (invariant 8).
                if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
                    log_->Warn("waystone", "pattern oid " + std::to_string(oid) +
                                               " has no directory this run: " +
                                               made.status().message());
                }
                continue;
            }
            fresh.emplace(oid, made.value());
        }
    }

    // ---- Pass 2: the one kWrite walk --------------------------------------
    //
    // **One pass over the chain, not one lookup per row**: each would walk
    // `sys.patterns` from its root, so rows x pages, at every mount. A fresh
    // root names no stale child, so the discard's reason still holds; every
    // other root is cleared, whatever its fingerprint version, so a lapsed
    // row keeps none. The visitor never accepts, so the walk runs the whole
    // chain.
    std::uint64_t opted = 0;
    std::unordered_set<Oid> used;
    auto walked = ForFirstRow<SysPatternRow>(
        store_, kCatalogPagePatterns,
        [&](SysPatternRow& row, heap::PageView& page, PageId page_id, std::uint16_t slot,
            const heap::PageView::Tuple& tuple) -> StatusOr<bool> {
            const bool is_opted = IsCurrentOptIn(row);
            if (is_opted) ++opted;
            const auto made = is_opted ? fresh.find(row.oid) : fresh.end();
            if (made == fresh.end() && !HasWaystoneDirectory(row)) return false;
            if (made != fresh.end()) {
                row.waystone_root = made->second;
                row.dir_depth = 1;
                used.insert(row.oid);
            } else {
                row.waystone_root = kInvalidPageId;
                row.dir_depth = 0;
            }
            const auto encoded = row.Encode();
            if (Status s = OverwriteLogged(wal_, store_, page, page_id, slot, encoded,
                                           wal::kNoTxnId, tuple.trx_id, tuple.undo_ptr);
                !s.ok()) {
                return s;
            }
            return false;
        });
    // A row that changed between the passes - none can, before the listener
    // binds, but the walk does not rely on it - kept its write, and the page
    // made for it is reachable from nothing.
    for (const auto& [oid, page_id] : fresh) {
        if (used.count(oid) != 0) continue;
        if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
            log_->Warn("waystone", "directory page " + std::to_string(page_id) +
                                       " leaked by the mount's renewal");
        }
    }
    // **Dropped, not updated in place** `[quiet-wrong]`: an entry filled
    // before the walk holds the previous run's root, whose children can be
    // ids the cursor has handed to logged pages since, so a trail written
    // through it would overwrite them with no record.
    cache_.Invalidate();
    if (!walked.ok()) return walked.status();
    // The boot's count (CF-S1c), from the walk that just read every row:
    // after the drop, so no generation filled under the old count survives.
    StoreOptedCount(opted);

    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        for (const auto& [version, count] : lapsed) {
            log_->Info("waystone", std::to_string(count) +
                                       " opt-ins lapsed at fingerprint version " +
                                       std::to_string(version));
        }
    }
    return opted;
}

StatusOr<std::uint64_t> Catalog::RecountOptedPatterns() {
    auto rows = ScanAll<SysPatternRow>(store_, kCatalogPagePatterns, nullptr, txn_);
    if (!rows.ok()) return rows.status();
    std::uint64_t opted = 0;
    for (const SysPatternRow& row : rows.value()) {
        if (IsCurrentOptIn(row)) ++opted;
    }
    StoreOptedCount(opted);
    return opted;
}

Status Catalog::ForgetAccessStats(Oid rel_id, std::uint16_t col_pos) {
    const std::uint64_t bit = ColumnBit(col_pos);
    if (bit == 0) return Status::OK();  // a mask names positions below 64 only
    // RecordAccess's hold, so no admission races the retire.
    auto held = store_.Get(kCatalogPageAccessStats);
    if (!held.ok()) return held.status();
    for (;;) {
        auto acted = ForFirstRow<SysAccessStatRow>(
            store_, kCatalogPageAccessStats,
            [&](SysAccessStatRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
                const heap::PageView::Tuple&) -> StatusOr<bool> {
                if (row.rel_id != rel_id || (row.column_mask & bit) == 0) return false;
                // Unlogged, by CR6 (RecordAccess states it).
                if (Status s = RetireLogged(/*wal=*/nullptr, store_, page, page_id, i,
                                            kBootstrapXid);
                    !s.ok()) {
                    return s;
                }
                return true;
            });
        if (!acted.ok()) return acted.status();
        if (!acted.value()) return Status::OK();
    }
}

Status Catalog::RecordAccess(std::uint8_t kind, Oid rel_id, std::uint64_t column_mask,
                              std::uint64_t last_seen, std::uint64_t count) {
    if (kind == kAccessKindUnset) {
        return Status::InvalidArgument("catalog: access kind 0 is reserved for an unset row");
    }

    // **The relation's root page, held exclusive across the whole call**
    // (AT-S7, `crosscore.md` CC13). Every core records its own accesses,
    // so the walk below, the increment it makes and the
    // admission of a shape nobody has seen have to be one act against a
    // relation two reactors reach. Page 11 is the door: `ForFirstRow` and
    // `InsertRow` both enter the chain here, so a core holding it exclusive
    // is the only core inside, and the acquisition each of them makes on
    // this core is the re-entrant one the latch grants its owner
    // (`device_page_store.hpp`, "Re-entrant for the owning core").
    //
    // **The race it closes is the admission, not the increment.** A match
    // is read, changed and written under `ForFirstRow`'s own hold on the
    // page it was found on, so no count is lost without this; what is lost
    // without it is the *shape* - two cores that both miss the same new one
    // both admit it, and the second row no later increment ever reaches,
    // because `ForFirstRow` stops at the first match. One shape's count
    // would split in two and stay split, which is the one way an advisory
    // statistic can mislead the mover rather than merely thin out.
    //
    // **What it costs, unmeasured:** once per step per statement on every
    // core, so at `cores > 1` every statement end takes one exclusive latch
    // on one page; the hold spans a chain walk bounded by
    // `kMaxAccessShapes` and, on the admit path, an `InsertRow` that may
    // grow the chain. `access_statistics = off` is the way out.
    auto held = store_.Get(kCatalogPageAccessStats);
    if (!held.ok()) return held.status();

    // `live` counts every row the walk passed, across every page of the
    // chain - what the cap below is measured against.
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
        // invert the ranking this exists to produce. `count` is 1 on the
        // statement path, the one path since AT-S7 retired CR7's batch
        // (whose fold passed more); the saturation stays an add.
        if (row.use_count > std::numeric_limits<std::uint64_t>::max() - count) {
            row.use_count = std::numeric_limits<std::uint64_t>::max();
        } else {
            row.use_count += count;
        }
        row.last_seen = last_seen;
        auto encoded = row.Encode();
        // In place - the row size is fixed, so there is nothing to move.
        //
        // **Unlogged, by CR6** (`docs/rules/rules.md` §5): `nullptr` where
        // every other catalog write passes `wal_`. This relation's whole
        // content is a statistic - invariant 8's advisory class, priced as
        // performance and never as a result - so it is neither redone nor
        // undone, and every mount discards it (`DiscardAccessStats`). It is the **sole** exception to
        // RV3's rule that catalog writes are logged as ordinary record
        // types; a second one has to be shown to meet the same test rather
        // than to resemble this one.
        if (Status s = OverwriteLogged(/*wal=*/nullptr, store_, page, page_id, i, encoded, wal::kNoTxnId, tuple.trx_id, tuple.undo_ptr); !s.ok()) {
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
    row.use_count = count;
    row.last_seen = last_seen;
    row.kind = kind;
    // No BumpVersion(): nothing cached is derived from these rows - a
    // statistic appearing cannot stale a cached relation - and this runs on
    // the statement path, where a cache drop would dangle a held pointer.
    // Unlogged and un-undone, by CR6 - the same exception the update path
    // above takes, and for the same reason. The undo hook goes with the WAL
    // pointer: a row nothing redoes has nothing to compensate.
    return InsertRow(/*wal=*/nullptr, /*undo_hook=*/nullptr, store_, kCatalogPageAccessStats,
                     row, kBootstrapXid);
}

StatusOr<std::uint64_t> Catalog::CreateCabin(Oid rel_oid, std::uint16_t col_pos,
                                              std::uint8_t origin, std::uint64_t trx_id,
                                              CatalogRowRef* where) {
    if (origin != kCabinOriginAuto && origin != kCabinOriginUser) {
        return Status::InvalidArgument("catalog: unknown cabin origin");
    }
    if (col_pos == 0) {
        // Not a policy choice and not a limitation: the tuple key column's Cabin
        // is the clustered tree, which is already authoritative for every
        // value rather than for the observed ones (spec section 2).
        return Status::InvalidArgument(
            "catalog: the tuple key column needs no cabin - the clustered tree is its cabin");
    }

    // Through InitTableAccess() rather than the sys.columns scan, because
    // the column count has to come from the same schema the compiler will
    // see. A cabin on a column that relation does not have would compile to
    // nothing and be invisible until someone read the catalog by hand.
    auto access = InitTableAccess(rel_oid);
    if (!access.ok()) return access.status();
    // §6a's converse and the split relation it guarded retired (SB3,
    // 2026-09-30); their history is at cf889b63. A set is
    // authoritative for the observed value (`docs/spec/cabin.md` §4b as
    // rewritten at AT-S7), and every walk that banks one covers the whole
    // relation. The **auto** path came through here
    // too and is admitted on the same terms.
    // A dropped column keeps its position and has no name (BW-R6), so no
    // door accepts it.
    if (col_pos >= access.value()->schema.columns.size() ||
        access.value()->schema.columns[col_pos].dropped()) {
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

    // Issued before the hold below, so the hold nests no other catalog
    // page; a refused create burns it, which the row-id sequence permits.
    auto cabin_id = AllocateRowId(kSysCabinsTable);
    if (!cabin_id.ok()) return cabin_id.status();

    // **One cabin per column** (AT-S17, `catalog.md` CT7's shape for a key
    // that is not a name): `sys.cabins`' root page held exclusive across
    // the check and the insert, so an operator's `CREATE CABIN` and the
    // controller's, or two operators', on two cores cannot both find the
    // column bare. Two rows for one column leave one set nothing serves.
    auto held = store_.Get(kCatalogPageCabins);
    if (!held.ok()) return held.status();
    if (FindCabinOnColumn(rel_oid, col_pos).ok()) {
        return Status::AlreadyExists("catalog: this column already has a cabin");
    }

    SysCabinRow row{};
    row.cabin_id = cabin_id.value();
    row.rel_oid = rel_oid;
    // Nothing is observed by creating a Cabin: the read path's miss branch
    // fills it (spec section 4). A count written here would be a claim about
    // runtime state made by DDL.
    row.observed_ct = 0;
    row.column_no = col_pos;
    row.shape = kCabinShapeColumn;
    row.origin = origin;
    // Active immediately. kCabinStatusBuilding exists for a phase-2
    // background build and has no writer while the only way to fill a Cabin
    // is a statement that was going to scan anyway.
    row.status = kCabinStatusActive;

    if (Status s = InsertCabinRow(row, trx_id, where); !s.ok()) return s;
    return row.cabin_id;
}

Status Catalog::InsertCabinRow(const SysCabinRow& row, std::uint64_t trx_id,
                               CatalogRowRef* where) {
    const Status inserted =
        InsertRow(wal_, ddl_undo_hook_, store_, kCatalogPageCabins, row, trx_id, where);
    // The identity a rollback's compensation re-reads, set on a failure too:
    // InsertRow reports a row it logged before failing, and that row is
    // still the rollback's to retire.
    if (where != nullptr) {
        where->oid = row.cabin_id;
        where->rel_oid = kSysCabinsTable;
    }
    if (!inserted.ok()) return inserted;
    // **Bumped**: a Cabin appearing stales `TableAccess::cabin_columns` and
    // `expr_cabins` on every held entry for this relation, which are facts
    // the compiler reads. That is what makes this DDL and not a
    // statement-path write.
    BumpVersion("sys.cabins create", Tally::kWordMovesCabin);
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("catalog", "created cabin " + std::to_string(row.cabin_id) +
                                  " on relation oid " + std::to_string(row.rel_oid) + " shape " +
                                  std::to_string(row.shape) + " column " +
                                  std::to_string(row.column_no) + " expr_id " +
                                  std::to_string(row.expr_id));
    }
    return Status::OK();
}

Status Catalog::DropCabin(std::uint64_t cabin_id) {
    auto acted = ForFirstRow<SysCabinRow>(
        store_, kCatalogPageCabins,
        [&](SysCabinRow& row, heap::PageView& page, PageId page_id, std::uint16_t i,
            const heap::PageView::Tuple&) -> StatusOr<bool> {
        if (row.cabin_id != cabin_id) return false;

        // **Retired, not delete-marked**, and this is where the argument
        // lives (catalog.hpp restates it for the two other removers): a
        // catalog read has no snapshot to filter a mark against, so a marked
        // row would still be found by every lookup and a re-created Cabin
        // would collide with a row nobody can see. `RetirePattern()` was the
        // first path in the engine to remove a catalog row and carried the
        // argument until 2026-08-31, when withdrawing declared patterns left
        // it with no caller.
        if (Status s = RetireLogged(wal_, store_, page, page_id, i, kBootstrapXid); !s.ok()) {
            return s;
        }

        BumpVersion("sys.cabins drop", Tally::kWordMovesCabin);
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
        if (row.shape == kCabinShapeColumn && row.rel_oid == rel_oid && row.column_no == col_pos) {
            return row;
        }
    }
    return Status::NotFound("no cabin on this column");
}

StatusOr<std::uint64_t> Catalog::CreateExprCabin(Oid rel_oid, std::uint8_t shape,
                                                  std::uint64_t expr_id, std::uint8_t origin,
                                                  const SameKeyFunction& same_f,
                                                  const RecordKeyFunction& record_f) {
    if (shape != kCabinShapeExpression && shape != kCabinShapePredicate) {
        return Status::InvalidArgument("catalog: not an expression or predicate cabin shape");
    }
    if (origin != kCabinOriginAuto && origin != kCabinOriginUser) {
        return Status::InvalidArgument("catalog: unknown cabin origin");
    }
    // Issued before the hold, for CreateCabin()'s reason. The text's own row
    // id is the caller's, issued before this call for the same one.
    auto cabin_id = AllocateRowId(kSysCabinsTable);
    if (!cabin_id.ok()) return cabin_id.status();

    // **One Cabin per `F`** (AQ-R2), under the hold CreateCabin() takes for
    // one per column: candidates by `expr_id`, each confirmed against the
    // compiled `F` - two `F`s sharing an `expr_id` are two Cabins. **The
    // order under it**: sys.cabins' root, then sys.cabin_exprs' root, then
    // its var-heap - `same_f` reads and `record_f` writes the last two, and
    // nothing takes them in the other order (their readers latch nothing).
    auto held = store_.Get(kCatalogPageCabins);
    if (!held.ok()) return held.status();
    auto rows = ListCabins();
    if (!rows.ok()) return rows.status();
    for (const SysCabinRow& row : rows.value()) {
        if (row.rel_oid != rel_oid || row.shape != shape || row.expr_id != expr_id) continue;
        auto same = same_f(row);
        if (!same.ok()) return same.status();
        if (same.value()) {
            return Status::AlreadyExists("catalog: this relation already has a cabin on this key");
        }
    }

    // The text first: a sys.cabins row is what makes a Cabin exist, and one
    // whose text is missing would fail its relation's every fill. A text
    // whose row never lands is inert - nothing reads a text no row names.
    if (Status s = record_f(cabin_id.value()); !s.ok()) return s;

    SysCabinRow row{};
    row.cabin_id = cabin_id.value();
    row.rel_oid = rel_oid;
    row.expr_id = expr_id;
    row.shape = shape;
    row.origin = origin;
    row.status = kCabinStatusActive;
    if (Status s = InsertCabinRow(row, kBootstrapXid, nullptr); !s.ok()) return s;
    return row.cabin_id;
}

StatusOr<std::uint64_t> Catalog::CreateForeignKey(Oid child_rel_oid, std::uint16_t child_column_no,
                                                  Oid parent_rel_oid, std::uint16_t flags) {
    auto child = InitTableAccess(child_rel_oid);
    if (!child.ok()) return child.status();
    auto parent = InitTableAccess(parent_rel_oid);
    if (!parent.ok()) return parent.status();

    if (child_column_no >= child.value()->schema.columns.size() ||
        child.value()->schema.columns[child_column_no].dropped()) {
        return Status::InvalidArgument("catalog: relation oid " + std::to_string(child_rel_oid) +
                                       " has no column at position " +
                                       std::to_string(child_column_no));
    }

    if (child_rel_oid == parent_rel_oid) {
        return RefuseSelfReference(NameView(child.value()->schema.columns[child_column_no].name))
            .WithContext("catalog");
    }
    // The declaration's own checks, shared with the CREATE TABLE pre-check
    // (foreign_key.hpp) so the two doors cannot answer differently.
    if (Status s = CheckForeignKeyDeclaration(*parent.value(),
                                              child.value()->schema.columns[child_column_no],
                                              child_column_no);
        !s.ok()) {
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

Status Catalog::DiscardAccessStats() {
    // Every page of the chain re-formatted in place, its link and its owner
    // kept, so the rows are gone and the pages stay the chain's for
    // `InsertRow` to fill again - a discard that left the growth pages
    // behind would spend a page per growth at every restore (BR-S8's
    // review). The head stays at its fixed id, so the `sys.objects` and
    // `sys.tables` rows naming page 11 stay true.
    //
    // **A page is the chain's by what it says, not by where its id falls**:
    // the head is page 11, and every page after it is a heap page owned by
    // `sys.access_stats` - `InsertRow` stamps each growth page with the
    // relation's oid (`kCatalogRoots`). Page 11 met again past the
    // head is a cycle, and its own owner, bootstrap's 0, says so.
    //
    // **Bounded, because the damaged path comes through here**: a link to a
    // page that is not allocated (a growth whose free-map bit no crash kept:
    // the relation is unlogged, CR6, so nothing redoes the bit), a page that
    // fails its checksum, a page that is not a heap page or is another
    // relation's, or more steps than any walk may take (a cycle) ends the
    // chain at the page before it - never a page of another relation
    // re-formatted, never a mount refused or one that does not return. A
    // head that fails so is refused, since there is no page before it to
    // cut at, and any other failure is returned. A cut is the damage
    // report, and the one place it is logged.
    PageId at = kCatalogPageAccessStats;
    PageId previous = kInvalidPageId;
    for (std::uint32_t step = 0; at != kInvalidPageId; ++step) {
        if (step >= storage::kMaxPageWalkLength) break;  // a cycle
        auto page = store_.Get(at);
        if (!page.ok() && page.status().code() != StatusCode::kCorruption &&
            page.status().code() != StatusCode::kNotFound) {
            return page.status();
        }
        const bool ours =
            page.ok() &&
            storage::RawPageType(page.value().bytes()) ==
                static_cast<std::uint8_t>(PageType::kHeap) &&
            (previous == kInvalidPageId ||
             storage::GetOwnerOid(page.value().bytes()) == kSysAccessStatsTable);
        if (!ours) break;
        const auto bytes = page.value().bytes();
        const PageId next = heap::PageView(bytes).next_page_id();
        // The head keeps bootstrap's owner, a growth page the relation's.
        const std::uint64_t owner = storage::GetOwnerOid(bytes);
        auto fresh = heap::PageView::CreateEmpty(bytes, 0, owner);
        if (!fresh.ok()) return fresh.status();
        fresh.value().set_next_page_id(next);
        previous = at;
        at = next;
    }
    if (at == kInvalidPageId) return Status::OK();
    // The walk stopped at a page that is not the chain's: cut before it.
    if (previous == kInvalidPageId) {
        return Status::Corruption("catalog: sys.access_stats' head page is unreadable");
    }
    auto before = store_.Get(previous);
    if (!before.ok()) return before.status();
    heap::PageView(before.value().bytes()).set_next_page_id(kInvalidPageId);
    if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
        log_->Error("catalog", "sys.access_stats was damaged past page " +
                                   std::to_string(previous) +
                                   " and its chain has been cut there: it is unlogged by CR6, "
                                   "so nothing redoes it, and an empty statistic is a slower "
                                   "optimizer rather than a wrong answer");
    }
    return Status::OK();
}

StatusOr<std::vector<SysAccessStatRow>> Catalog::ListAccessStats() {
    return ScanAll<SysAccessStatRow>(store_, kCatalogPageAccessStats, nullptr, txn_);
}

Status Catalog::CheckIndexDef(const IndexDef& def, std::uint64_t own_trx_id) {
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
    if ((def.flags & ~kIndexFlagUnique) != 0) {
        return Status::InvalidArgument("catalog: index flags " + std::to_string(def.flags) +
                                       " name a bit nothing defines");
    }

    auto access = InitTableAccess(def.table_oid);
    if (!access.ok()) return access.status();
    const Schema& schema = access.value()->schema;
    // A secondary index resolves an entry through the clustered tree's
    // descent (docs/spec/index.md IX3), which a system relation's catalog
    // chain has none of. Every user relation is a btree (BZ-R1).
    if (access.value()->clustered_type != ClusteredType::kBtree) {
        return Status::InvalidArgument(
            "catalog: relation oid " + std::to_string(def.table_oid) +
            " is a system relation; a secondary index resolves an entry through the tuple key "
            "descent (docs/spec/index.md IX3)");
    }

    const auto check_column = [&](std::uint16_t pos, const char* role) -> Status {
        if (pos >= schema.columns.size()) {
            return Status::InvalidArgument("catalog: " + std::string(role) + " column position " +
                                            std::to_string(pos) + " is past the relation's " +
                                            std::to_string(schema.columns.size()) + " columns");
        }
        if (schema.columns[pos].dropped()) {
            return Status::InvalidArgument("catalog: " + std::string(role) + " column position " +
                                            std::to_string(pos) + " is a dropped column");
        }
        return Status::OK();
    };
    for (std::size_t i = 0; i < def.key_cols.size(); ++i) {
        if (Status s = check_column(def.key_cols[i], "key"); !s.ok()) return s;
        // The tuple key is carried only by the Keystone word and the clustered tree
        // already indexes it; a second copy would be maintained forever to
        // answer questions the descent answers faster.
        if (def.key_cols[i] == 0) {
            return Status::InvalidArgument(
                "catalog: the tuple key already has an index - the clustered tree is it");
        }
        // D2 (`docs/workplan-null.md`): a nullable key column is refused in
        // v1, so the entry format, maintenance and backfill keep the
        // property that every row has exactly one entry of a key that
        // always exists. IS NULL answers by scan; revisit with a measured
        // need, like index.md §13's other opens.
        if (!schema.columns[def.key_cols[i]].notnull) {
            return Status::NotImplemented(
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
            return Status::NotImplemented(
                "catalog: covered column '" + std::string(NameView(schema.columns[pos].name)) +
                "' is nullable, and an index entry has no NULL encoding (docs/spec/null.md D2)");
        }
    }

    // The early answer, before anything is built; `CreateIndex` asks again
    // under the held page, which is the answer that binds.
    if (Status s = CheckIndexNameFree(def.name, own_trx_id); !s.ok()) return s;

    // The anchor's entry table, asked **here** so that a refusal costs no
    // pages: both build paths reach the seed only after the whole tree is
    // allocated, and nothing in this engine frees one. This is the door
    // both of them already come through, and it is early enough on the
    // local arm that a refused declaration burns no index oid either -
    // `PrepareIndexDef` issues that after this returns.
    //
    if (Status s = CheckAnchorRoomForIndex(access.value()->anchor_page_id, def.table_oid,
                                           def.index_oid);
        !s.ok()) {
        return s;
    }
    return Status::OK();
}

StatusOr<Oid> Catalog::CreateIndex(const IndexDef& def, std::uint64_t trx_id,
                                    CatalogRowRef* where) {
    // Re-checked here even when the caller already asked: this is the door
    // every non-DDL caller comes through, and a check that only runs when
    // someone remembers to ask is not a check.
    if (Status s = CheckIndexDef(def, trx_id); !s.ok()) return s;

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

    // PW2-3: the anchor slot is seeded at creation, so the anchor is the
    // index root's whole truth from birth. The creating core seeds it.
    //
    // **Before the row** (BY-S1's review): `InitTableAccess` reads rows
    // unfiltered and refuses a live row with no slot (BY-R10), so a row
    // written first is a window in which another core opening the relation
    // reads `Corruption`. A slot written first, with no row, is read by
    // nothing while the instance runs; a refusal below leaves it, and its
    // tree, owed (BY-R1, BY-R2).
    {
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

    // **One name, one row** (AT-S17): `sys.indexes`' root page held
    // exclusive across the name check and the insert. Index names are
    // instance-wide and the relation `X` this DDL holds covers its own
    // relation only, so a second core creating an index of this name on
    // another relation passed `CheckIndexDef`'s check alongside this one.
    // Nothing but the relation's own chain growth nests under it.
    {
        auto names = store_.Get(kCatalogPageIndexes);
        if (!names.ok()) return names.status();
        if (Status s = CheckIndexNameFree(def.name, trx_id); !s.ok()) return s;
        if (Status s = InsertRow(wal_, ddl_undo_hook_, store_, kCatalogPageIndexes, row, trx_id,
                                 where);
            !s.ok()) {
            return s;
        }
    }
    if (where != nullptr) {
        where->oid = row.index_oid;
        where->rel_oid = kSysIndexesTable;
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

            // Retired outside a transaction - DropCabin() states that
            // argument and it still holds.
            // **Inside one, delete-marked**, which a rollback can clear
            // (DT5's mechanism).
            //
            // Unlike `DROP TABLE`, this is also **isolated**, but for a
            // reason §5a had to correct: not because the payload survives
            // an overwrite, which was the original and wrong argument, but
            // because DT9 taught the unfiltered read to leave an open mark
            // alone. On every core since AT-S5e, because the drop holds the
            // relation `X` (`HandleIndex`).
            if (!transactional) {
                if (Status s = RetireLogged(wal_, store_, page, page_id, i, trx_id); !s.ok()) {
                    return s;
                }
            } else {
                if (ddl_undo_hook_) {
                    if (Status s = ddl_undo_hook_({DdlUndoEvent::Kind::kDeleteMark, page_id, i,
                                                   {}, tuple.trx_id, tuple.undo_ptr,
                                                   UndoTupleKeyOf(tuple.payload)});
                        !s.ok()) {
                        return s;
                    }
                }
                if (Status s = DeleteMarkLogged(wal_, store_, page, page_id, i, trx_id);
                    !s.ok()) {
                    return s;
                }
                NoteMark();
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
        // is in the middle of removing it. The caller appends the byte
        // position.
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

StatusOr<SysIndexRow> Catalog::FindIndexByName(std::string_view name,
                                               const txn::ReadView* view) {
    auto rows = ListIndexes(view);
    if (!rows.ok()) return rows.status();
    // BY-R8: a name can be held twice inside one transaction - the row it
    // delete-marked, which the unfiltered read still returns while it is in
    // flight, and the row it then created. The later row is the live one,
    // and the later row has the larger oid - `sys.indexes`' row-id sequence
    // only rises - where scan order says nothing: a row may land in a
    // retired slot of an earlier page.
    std::optional<SysIndexRow> found;
    for (const SysIndexRow& row : rows.value()) {
        if (NameView(row.name) == name && (!found.has_value() || row.index_oid > found->index_oid)) {
            found = row;
        }
    }
    if (found.has_value()) return *found;
    return Status::NotFound("no index by that name");
}

Status Catalog::CheckIndexNameFree(std::string_view name, std::uint64_t own_trx_id) {
    // Its own walk, for `CheckNameFree`'s reason one relation over: the
    // answer is in the tuple header. An index's drop **delete-marks** its
    // row rather than retyping it, and `ListIndexes`' unfiltered read
    // settled a mark by *this core's* in-flight test until AX-S2 (`ScanAll`,
    // DT9) - so another core's open `DROP INDEX` read as done here, and its
    // rollback restored the name beside the one this create took. The check view is
    // the instance's, minted with no own id: the asker's own drop is
    // classified by its stamp.
    const txn::ReadView view =
        txn_ != nullptr ? txn_->MintCheckView(txn::kNoTrxId) : txn::ReadView::Everything();
    Status verdict = Status::OK();
    Status walked = heap::ChainVisit(
        store_, kCatalogPageIndexes, storage::PageAccess::kRead,
        [&](PageId, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            if (!tuple.ok()) {
                if (tuple.status().code() == StatusCode::kNotFound) {
                    return storage::VisitControl::kContinue;
                }
                return tuple.status();
            }
            // A mark whose deleter this view sees is gone; any other row of
            // the name - live, or marked by a drop still undecided - holds it.
            const std::uint64_t stamp = tuple.value().trx_id;
            if (tuple.value().deleted && view.Visible(stamp)) {
                return storage::VisitControl::kContinue;
            }
            auto row = SysIndexRow::Decode(tuple.value().payload);
            if (!row.ok()) return row.status();
            if (NameView(row.value().name) != name) return storage::VisitControl::kContinue;
            const bool own_mark =
                tuple.value().deleted && own_trx_id != txn::kNoTrxId && stamp == own_trx_id;
            // **A row this transaction delete-marked frees the name for it**
            // (BY-R8): `REINDEX`'s new row takes the old one's name, and so may
            // `CREATE INDEX x` after `DROP INDEX x` in one transaction. Its
            // rollback clears the mark and retires the new row together.
            if (own_mark) return storage::VisitControl::kContinue;
            const bool pending_elsewhere = tuple.value().deleted;
            verdict = pending_elsewhere
                          ? PendingDropRefusal("index", name)
                          : Status::AlreadyExists("catalog: an index named '" +
                                                  std::string(name) + "' already exists");
            return storage::VisitControl::kStop;
        });
    if (!walked.ok()) return walked;
    return verdict;
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
