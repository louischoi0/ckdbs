#include "kds/exec/heap_store.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "kds/exec/index_key.hpp"
#include "kds/exec/index_maintain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/index/index_tree.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/visit.hpp"

namespace kds::exec {

// ---- HeapSpace ------------------------------------------------------------

PageId HeapSpace::CurrentPage(std::uint64_t oid, std::uint32_t core) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto rel = relations_.find(oid);
    if (rel == relations_.end()) return kInvalidPageId;
    auto page = rel->second.current.find(core);
    return page == rel->second.current.end() ? kInvalidPageId : page->second;
}

void HeapSpace::SetCurrentPage(std::uint64_t oid, std::uint32_t core, PageId page) {
    std::lock_guard<std::mutex> lock(mu_);
    relations_[oid].current[core] = page;
}

std::optional<heap::Rowid> HeapSpace::TakeFree(std::uint64_t oid) {
    std::lock_guard<std::mutex> lock(mu_);
    auto rel = relations_.find(oid);
    if (rel == relations_.end() || rel->second.free.empty()) return std::nullopt;
    const auto first = rel->second.free.begin();
    const heap::Rowid out = heap::DecodeRowid(*first);
    rel->second.free.erase(first);
    return out;
}

void HeapSpace::AddFree(std::uint64_t oid, heap::Rowid rowid) {
    std::lock_guard<std::mutex> lock(mu_);
    relations_[oid].free.insert(heap::EncodeRowid(rowid));
}

void HeapSpace::NoteDead(std::uint64_t oid) {
    std::lock_guard<std::mutex> lock(mu_);
    if (dead_set_.insert(oid).second) dead_.push_back(oid);
}

std::optional<std::uint64_t> HeapSpace::TakeDead() {
    std::lock_guard<std::mutex> lock(mu_);
    if (dead_.empty()) return std::nullopt;
    const std::uint64_t oid = dead_.front();
    dead_.pop_front();
    dead_set_.erase(oid);
    return oid;
}

void HeapSpace::Forget(std::uint64_t oid) {
    std::lock_guard<std::mutex> lock(mu_);
    relations_.erase(oid);
    if (dead_set_.erase(oid) != 0) dead_.erase(std::find(dead_.begin(), dead_.end(), oid));
}

// ---- Reads ----------------------------------------------------------------

namespace {

// A page of this heap, or `Corruption`: a rowid that names another page
// type or another relation's page is damage, never a miss.
Status RequireDataPage(std::span<const std::byte, kPageSize> page, PageId id,
                       const catalog::TableAccess& access) {
    const storage::PageHeaderFields header = storage::ReadPageHeader(page);
    if (header.page_type != static_cast<std::uint8_t>(PageType::kHeapData) ||
        header.owner_oid != access.oid) {
        return Status::Corruption("page " + std::to_string(id) + " (type " +
                                  std::to_string(header.page_type) + ", owner " +
                                  std::to_string(header.owner_oid) +
                                  ") is not a data page of heap oid " +
                                  std::to_string(access.oid));
    }
    return Status::OK();
}

}  // namespace

const catalog::TableAccess::IndexRef* KeystoneIndexOf(const catalog::TableAccess& access) {
    for (const catalog::TableAccess::IndexRef& ix : access.indexes) {
        if (ix.keystone) return &ix;
    }
    return nullptr;
}

StatusOr<btree::Location> FetchHeapRow(storage::PageStore& store,
                                       const catalog::TableAccess& access, std::uint64_t rowid,
                                       std::uint64_t tuple_key, storage::PageAccess mode) {
    const heap::Rowid at = heap::DecodeRowid(rowid);
    auto ref = store.Fetch(at.page_id, mode);
    if (!ref.ok()) return ref.status();
    // **A rowid naming a page that is not this heap's is a dangling entry**,
    // a miss as an entry naming no row is (IX1): a crash can keep an index
    // entry whose growth's page redo never created, and the page id is then
    // another relation's. Answered `Corruption` it would refuse every probe
    // and every judge that could remove it.
    if (Status s = RequireDataPage(ref.value().bytes(), at.page_id, access); !s.ok()) {
        return Status::NotFound(s.message());
    }
    heap::PageView page(ref.value().bytes());
    auto tuple = page.ReadTuple(at.slot);
    if (!tuple.ok()) {
        if (tuple.status().code() == StatusCode::kNotFound) {
            return Status::NotFound("rowid " + std::to_string(rowid) + " holds no tuple");
        }
        return tuple.status();
    }
    auto id = kds::KeystoneIdOfPayload(tuple.value().payload);
    if (!id.ok()) return id.status();
    if (id.value() != tuple_key) {
        return Status::NotFound("rowid " + std::to_string(rowid) + " holds Keystone " +
                                std::to_string(id.value()) + ", not " +
                                std::to_string(tuple_key));
    }
    return btree::Location{at.page_id, at.slot, std::move(ref.value())};
}

namespace {

// A Keystone id as the Keystone index's key: the leading `key_width` bytes
// of `entry`, the rest left as the caller padded it.
Status EncodeKeystoneKey(const catalog::TableAccess& access, const index::IndexLayout& layout,
                         std::uint64_t tuple_key, std::span<std::byte> entry) {
    parser::AstValue value;
    value.type = parser::ValueType::kInt;
    value.int_val = static_cast<std::int64_t>(tuple_key);
    return EncodeIndexKeyColumn(access.schema.columns.front(), value,
                                entry.first(layout.key_width));
}

}  // namespace

StatusOr<std::pair<std::vector<std::byte>, std::vector<std::byte>>> KeystoneIndexBounds(
    const catalog::TableAccess& access, const catalog::TableAccess::IndexRef& ix,
    std::uint64_t lo, std::uint64_t hi) {
    // The tail past the key - the Keystone word, the rowid - is padded as
    // an `IndexProbe`'s bounds are: 0x00 below, 0xFF above.
    const index::IndexLayout layout = IndexLayoutOf(ix);
    std::vector<std::byte> low(layout.sort_key_width(), std::byte{0x00});
    std::vector<std::byte> high(layout.sort_key_width(), std::byte{0xFF});
    if (Status s = EncodeKeystoneKey(access, layout, lo, low); !s.ok()) return s;
    if (Status s = EncodeKeystoneKey(access, layout, hi - 1, high); !s.ok()) return s;
    return std::make_pair(std::move(low), std::move(high));
}

StatusOr<std::vector<std::uint64_t>> KeystoneRowids(storage::PageStore& store,
                                                    const catalog::TableAccess& access,
                                                    std::uint64_t tuple_key) {
    const catalog::TableAccess::IndexRef* ix = KeystoneIndexOf(access);
    if (ix == nullptr) {
        return Status::Corruption("heap oid " + std::to_string(access.oid) +
                                  " has no Keystone index");
    }
    const index::IndexLayout layout = IndexLayoutOf(*ix);
    std::vector<std::byte> low(layout.sort_key_width(), std::byte{0});
    if (Status s = EncodeKeystoneKey(access, layout, tuple_key, low); !s.ok()) return s;
    auto first = index::IndexSeekLeaf(store, ix->root_page_id, layout, low);
    if (!first.ok()) return first.status();
    std::vector<std::uint64_t> rowids;
    Status walked = index::IndexVisitFrom(
        store, first.value(), layout, storage::PageAccess::kRead,
        [&](PageId, index::IndexLeafView& leaf,
            std::uint16_t at) -> StatusOr<storage::VisitControl> {
            auto entry = leaf.Entry(at);
            if (!entry.ok()) return entry.status();
            const int order = std::memcmp(entry.value().data(), low.data(), layout.key_width);
            if (order < 0) return storage::VisitControl::kContinue;
            if (order > 0) return storage::VisitControl::kStop;
            rowids.push_back(index::GetIndexRowid(layout, entry.value()));
            return storage::VisitControl::kContinue;
        });
    if (!walked.ok()) return walked;
    return rowids;
}

StatusOr<btree::Location> LocateRow(storage::PageStore& store,
                                    const catalog::TableAccess& access, std::uint64_t tuple_key,
                                    storage::PageAccess mode) {
    if (!access.heap()) return btree::BtreeLookup(store, access.desc_page_id, tuple_key, mode);
    // Collected before any data page is fetched, so no data page is taken
    // under an index leaf's span.
    auto rowids = KeystoneRowids(store, access, tuple_key);
    if (!rowids.ok()) return rowids.status();
    for (std::uint64_t rowid : rowids.value()) {
        auto row = FetchHeapRow(store, access, rowid, tuple_key, mode);
        if (row.ok() || row.status().code() != StatusCode::kNotFound) return row;
    }
    return Status::NotFound("no row keyed " + std::to_string(tuple_key));
}

// ---- Placement ------------------------------------------------------------

namespace {

// The row on `page`, held exclusive as `ref`: a free slot of the page first,
// then the page's free space. nullopt when neither holds it or the page's
// `min_key` is above the row's Keystone (invariant 3).
StatusOr<std::optional<HeapPlacement>> PlaceOn(storage::PageRef& ref, PageId id,
                                               std::uint64_t tuple_key,
                                               std::span<const std::byte> payload,
                                               std::uint64_t trx_id) {
    heap::PageView page(ref.bytes());
    if (page.min_key() > tuple_key) return std::optional<HeapPlacement>{};
    HeapPlacement out;
    if (auto free = page.FindFreeSlot(payload.size()); free.has_value()) {
        if (Status s = page.ReuseSlot(*free, payload, trx_id, /*undo_ptr=*/0); !s.ok()) return s;
        out.placed.slot = *free;
        out.reused = true;
    } else {
        auto slot = page.InsertTuple(payload, trx_id);
        if (!slot.ok()) {
            if (slot.status().code() == StatusCode::kOutOfSpace) {
                return std::optional<HeapPlacement>{};
            }
            return slot.status();
        }
        out.placed.slot = slot.value();
    }
    out.placed.page_id = id;
    out.placed.held.push_back(std::move(ref));
    return std::optional<HeapPlacement>{std::move(out)};
}

}  // namespace

StatusOr<HeapPlacement> HeapInsert(storage::PageStore& store, const catalog::TableAccess& access,
                                   HeapSpace& space, std::uint32_t core, std::uint64_t tuple_key,
                                   std::span<const std::byte> payload, std::uint64_t trx_id) {
    if (!access.heap()) {
        return Status::InvalidArgument("relation oid " + std::to_string(access.oid) +
                                       " is not a heap");
    }
    if (Status s = kds::RequirePayloadCarries(payload, tuple_key); !s.ok()) return s;

    // A page by its id, exclusive, checked to be this heap's.
    const auto fetch = [&](PageId id) -> StatusOr<storage::PageRef> {
        auto ref = store.Get(id);
        if (!ref.ok()) return ref.status();
        if (Status s = RequireDataPage(ref.value().bytes(), id, access); !s.ok()) return s;
        return ref;
    };

    // 1. A slot the slot release freed. A hint whose page does not admit the
    // key goes back; one no longer free is dropped.
    std::vector<heap::Rowid> declined;
    std::optional<HeapPlacement> placed;
    while (!placed.has_value()) {
        const std::optional<heap::Rowid> hint = space.TakeFree(access.oid);
        if (!hint.has_value()) break;
        auto ref = fetch(hint->page_id);
        if (!ref.ok()) return ref.status();
        heap::PageView page(ref.value().bytes());
        if (page.min_key() > tuple_key) {
            declined.push_back(*hint);
            continue;
        }
        Status reused = page.ReuseSlot(hint->slot, payload, trx_id, /*undo_ptr=*/0);
        if (reused.code() == StatusCode::kInvalidArgument) continue;  // taken already
        if (!reused.ok()) return reused;
        HeapPlacement out;
        out.placed.page_id = hint->page_id;
        out.placed.slot = hint->slot;
        out.placed.held.push_back(std::move(ref.value()));
        out.reused = true;
        placed = std::move(out);
    }
    for (const heap::Rowid& r : declined) space.AddFree(access.oid, r);
    if (placed.has_value()) return std::move(*placed);

    // 2. The page this core fills, then the head. The last one that was
    // full is where a new page links on, so one core's rows are walked in
    // the order it wrote them; no order is promised (DN-D4).
    const PageId current = space.CurrentPage(access.oid, core);
    PageId link_after = access.desc_page_id;
    for (PageId id : {current, access.desc_page_id}) {
        if (id == kInvalidPageId) continue;
        auto ref = fetch(id);
        if (!ref.ok()) return ref.status();
        const bool admits = heap::PageView(ref.value().bytes()).min_key() <= tuple_key;
        auto on = PlaceOn(ref.value(), id, tuple_key, payload, trx_id);
        if (!on.ok()) return on.status();
        if (on.value().has_value()) return std::move(*on.value());
        if (admits && id == current) {
            link_after = id;
            break;
        }
        if (id == access.desc_page_id) break;
    }

    // 3. A new page, `min_key` the row's Keystone, linked on after the page
    // that filled. The new page first - nothing else can reach it - then
    // the page it links from.
    auto created = store.CreateNew();
    if (!created.ok()) return created.status();
    const PageId new_id = created.value().first;
    storage::PageRef new_ref = std::move(created.value().second);
    auto formatted = heap::PageView::CreateEmptyAs(new_ref.bytes(), tuple_key,
                                                   PageType::kHeapData, access.oid);
    if (!formatted.ok()) return formatted.status();
    auto slot = formatted.value().InsertTuple(payload, trx_id);
    if (!slot.ok()) return slot.status();
    auto prev = fetch(link_after);
    if (!prev.ok()) return prev.status();
    heap::PageView prev_view(prev.value().bytes());
    formatted.value().set_next_page_id(prev_view.next_page_id());
    prev_view.set_next_page_id(new_id);
    space.SetCurrentPage(access.oid, core, new_id);

    HeapPlacement out;
    out.placed.page_id = new_id;
    out.placed.slot = slot.value();
    out.placed.Record(new_id);
    out.placed.Record(link_after);
    out.placed.held.push_back(std::move(new_ref));
    out.placed.held.push_back(std::move(prev.value()));
    return out;
}

}  // namespace kds::exec
