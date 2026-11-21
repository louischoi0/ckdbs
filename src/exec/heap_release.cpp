#include "kds/exec/heap_release.hpp"

#include <string>

#include "kds/exec/catalog_spills.hpp"
#include "kds/exec/index_maintain.hpp"
#include "kds/exec/index_purge.hpp"
#include "kds/exec/wal_row_log.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/heap/rowid.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/index/index_tree.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/visit.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/varheap_release.hpp"

namespace kds::exec {

namespace {

// The first index of `access` with an oid above `after`, or none: creation
// order (`TableAccess::indexes` is sorted by oid).
const catalog::TableAccess::IndexRef* IndexAfter(const catalog::TableAccess& access,
                                                 std::uint64_t after) {
    for (const catalog::TableAccess::IndexRef& ix : access.indexes) {
        if (ix.index_oid > after) return &ix;
    }
    return nullptr;
}

const catalog::TableAccess::IndexRef* IndexById(const catalog::TableAccess& access,
                                                std::uint64_t oid) {
    for (const catalog::TableAccess::IndexRef& ix : access.indexes) {
        if (ix.index_oid == oid) return &ix;
    }
    return nullptr;
}

bool IsDataPageOf(std::span<const std::byte, kPageSize> page, catalog::Oid oid) {
    const storage::PageHeaderFields header = storage::ReadPageHeader(page);
    return header.page_type == static_cast<std::uint8_t>(PageType::kHeapData) &&
           header.owner_oid == oid;
}

}  // namespace

Status HeapSlotRelease::Step(HeapReleaseContext& cx, std::uint32_t budget) {
    if (done()) return Status::OK();
    // **The memo made current under the step's `IX`** (catalog.md CT2), as
    // a DDL makes it under its `X`: a step is stepped from a `system` tick,
    // which is no statement boundary, so the memo is otherwise as old as the
    // core's last statement - missing an index another core built since,
    // which would then be neither purged nor verified, its entries left
    // naming a freed slot (DN-R2), or naming a dropped index whose pages a
    // reclaim has handed on. Nothing here holds a `TableAccess` yet, and the
    // `IX` fences every DDL that changes the relation's indexes from here to
    // the step's end.
    cx.catalog.Revalidate();
    auto access = cx.catalog.InitTableAccess(rel_oid_);
    if (!access.ok() && access.status().code() != StatusCode::kNotFound) {
        return access.status();  // asked again whole; its slots stay retired
    }
    if (!access.ok() || !access.value()->heap()) {
        // Dropped, or never a heap: nothing to release, and its hints name
        // pages a reclaim frees.
        cx.space.Forget(rel_oid_);
        phase_ = Phase::kDone;
        return Status::OK();
    }
    const catalog::TableAccess& ta = *access.value();
    Status stepped = Status::OK();
    switch (phase_) {
        case Phase::kCollect: stepped = Collect(cx, ta, budget); break;
        case Phase::kPurge: stepped = Purge(cx, ta, budget); break;
        case Phase::kVerify: stepped = Verify(cx, ta, budget); break;
        case Phase::kFree: stepped = Free(cx, ta, budget); break;
        case Phase::kDone: break;
    }
    // A job that left a slot owed for a reason that passes - a deleter in
    // flight or still seen by a reader, an index still building - queues
    // its relation again, or nothing would: the delete that queued it has
    // already been taken.
    if (stepped.ok() && done() && deferred_) cx.space.NoteDead(rel_oid_);
    return stepped;
}

Status HeapSlotRelease::Collect(HeapReleaseContext& cx, const catalog::TableAccess& access,
                                std::uint32_t budget) {
    if (!started_) {
        page_ = access.desc_page_id;
        started_ = true;
    }
    // A hidden-Keystone heap's tombstones are retired here (DN-Q14 (a)); a
    // declared Keystone's are kept until a PURGE frees the key.
    const bool retires = access.schema.hidden_keystone();
    storage::PageChainGuard guard("heap release");
    for (std::uint32_t walked = 0; page_ != kInvalidPageId && walked < budget; ++walked) {
        if (Status s = guard.Visit(page_); !s.ok()) return s;
        struct Tombstone {
            std::uint16_t slot;
            std::uint64_t deleter;
        };
        std::vector<Tombstone> tombstones;
        PageId next = kInvalidPageId;
        {
            auto ref = cx.store.GetForRead(page_);
            if (!ref.ok()) return ref.status();
            if (!IsDataPageOf(ref.value().bytes(), access.oid)) {
                return Status::Corruption("heap release: page " + std::to_string(page_) +
                                          " is not a data page of oid " +
                                          std::to_string(access.oid));
            }
            heap::PageView page(ref.value().bytes());
            for (std::uint16_t i = 0; i < page.slot_count(); ++i) {
                auto info = page.DebugSlotInfo(i);
                if (!info.ok()) return info.status();
                const std::uint64_t rowid = heap::EncodeRowid(heap::Rowid{page_, i});
                if ((info.value().flags & heap::kSlotFlagFree) != 0) {
                    cx.space.AddFree(access.oid, heap::Rowid{page_, i});
                    continue;
                }
                if (info.value().dead) {
                    candidates_.insert(rowid);
                    continue;
                }
                if (!retires) continue;
                auto tuple = page.ReadTuple(i);
                if (!tuple.ok() || !tuple.value().deleted) continue;
                const std::uint64_t deleter = tuple.value().trx_id;
                if (cx.txns == nullptr) continue;
                if (cx.txns->IsInFlight(deleter) || !cx.txns->ResolvedForEveryReader(deleter)) {
                    deferred_ = true;
                    continue;
                }
                tombstones.push_back(Tombstone{i, deleter});
            }
            next = page.next_page_id();
        }
        // Each tombstone retired under the page's exclusive hold, asked
        // again there: still delete-marked by the same deleter. Its spills go
        // after the hold, as `PURGE`'s do.
        for (const Tombstone& t : tombstones) {
            std::vector<SpillRef> spills;
            {
                auto ref = cx.store.Get(page_);
                if (!ref.ok()) return ref.status();
                heap::PageView page(ref.value().bytes());
                auto tuple = page.ReadTuple(t.slot);
                if (!tuple.ok() || !tuple.value().deleted || tuple.value().trx_id != t.deleter) {
                    continue;
                }
                if (Status s = RowSpills(access, tuple.value().payload, spills); !s.ok()) return s;
                if (Status s = page.RetireSlot(t.slot); !s.ok()) return s;
                if (Status s = LogSlotRetire(cx.wal, cx.store, wal::kNoTxnId, page_, t.slot);
                    !s.ok()) {
                    return s;
                }
            }
            ++counts_.retired;
            candidates_.insert(heap::EncodeRowid(heap::Rowid{page_, t.slot}));
            for (const auto& [spill_page, spill_slot] : spills) {
                auto released =
                    txn::ReleaseVarHeapSlot(cx.store, cx.wal, wal::kNoTxnId, spill_page, spill_slot);
                if (!released.ok()) return released.status();
            }
        }
        page_ = next;
    }
    if (page_ == kInvalidPageId) {
        phase_ = candidates_.empty() ? Phase::kDone : Phase::kPurge;
    }
    return Status::OK();
}

Status HeapSlotRelease::Purge(HeapReleaseContext& cx, const catalog::TableAccess& access,
                              std::uint32_t budget) {
    if (index_oid_ == 0) {
        const catalog::TableAccess::IndexRef* next = IndexAfter(access, done_oid_);
        if (next == nullptr) {
            phase_ = Phase::kVerify;
            done_oid_ = 0;
            return Status::OK();
        }
        index_oid_ = next->index_oid;
        resume_.clear();
    }
    const catalog::TableAccess::IndexRef* ix = IndexById(access, index_oid_);
    // Dropped, or still being built (DD-R8): not purged; the verify reads it.
    if (ix == nullptr || ix->building) {
        done_oid_ = index_oid_;
        index_oid_ = 0;
        return Status::OK();
    }
    IndexPurgeAttempt attempt;
    const Status ran = PurgeIndexLeaves(cx.store, cx.catalog, access, cx.txns, cx.wal, cx.locks,
                                        cx.holder, cx.holdings, *ix, resume_, budget, attempt);
    counts_.removed += attempt.removed;
    if (!ran.ok()) return ran;
    resume_ = std::move(attempt.resume);
    if (resume_.empty()) {
        done_oid_ = index_oid_;
        index_oid_ = 0;
    }
    return Status::OK();
}

Status HeapSlotRelease::Verify(HeapReleaseContext& cx, const catalog::TableAccess& access,
                               std::uint32_t budget) {
    if (index_oid_ == 0) {
        const catalog::TableAccess::IndexRef* next = IndexAfter(access, done_oid_);
        if (next == nullptr) {
            phase_ = Phase::kFree;
            return Status::OK();
        }
        index_oid_ = next->index_oid;
        const index::IndexLayout layout = IndexLayoutOf(*next);
        std::vector<std::byte> low(layout.sort_key_width(), std::byte{0});
        auto first = index::IndexSeekLeaf(cx.store, next->root_page_id, layout, low);
        if (!first.ok()) return first.status();
        leaf_ = first.value();
    }
    const catalog::TableAccess::IndexRef* ix = IndexById(access, index_oid_);
    if (ix == nullptr) {  // dropped between steps: it names nothing now
        done_oid_ = index_oid_;
        index_oid_ = 0;
        return Status::OK();
    }
    // **An index still building is no proof** (CI): its backfill copies a
    // page's rows, then appends their entries, holding nothing between - so
    // a row copied before the collect retired its slot gets its entry after
    // this walk has passed. Nothing is freed while one builds; the job is
    // asked again, and once the build is published every entry it copied is
    // in the tree this walk reads.
    if (ix->building) {
        deferred_ = true;
        candidates_.clear();
        phase_ = Phase::kDone;
        return Status::OK();
    }
    const index::IndexLayout layout = IndexLayoutOf(*ix);
    for (std::uint32_t walked = 0; leaf_ != kInvalidPageId && walked < budget; ++walked) {
        auto page = index::IndexFetchLeaf(cx.store, leaf_, layout, storage::PageAccess::kRead);
        if (!page.ok()) return page.status();
        const index::IndexLeafView view(page.value().bytes());
        for (std::uint16_t i = 0; i < view.entry_count() && !candidates_.empty(); ++i) {
            auto entry = view.Entry(i);
            if (!entry.ok()) return entry.status();
            if (candidates_.erase(index::GetIndexRowid(layout, entry.value())) != 0) {
                ++counts_.named;
            }
        }
        leaf_ = view.right_sibling();
    }
    if (leaf_ == kInvalidPageId || candidates_.empty()) {
        done_oid_ = index_oid_;
        index_oid_ = 0;
        if (candidates_.empty()) phase_ = Phase::kDone;
    }
    return Status::OK();
}

// A page at a time, `budget` pages a step (DD-R10: the step's `IX` is
// bounded): a rowid sorts by its page first, so a page's candidates are
// adjacent and freed under one hold.
Status HeapSlotRelease::Free(HeapReleaseContext& cx, const catalog::TableAccess& access,
                             std::uint32_t budget) {
    for (std::uint32_t pages = 0; !candidates_.empty() && pages < budget; ++pages) {
        const PageId page_id = heap::DecodeRowid(*candidates_.begin()).page_id;
        const auto end = candidates_.lower_bound(heap::EncodeRowid(heap::Rowid{page_id + 1, 0}));
        std::vector<heap::Rowid> freed;
        {
            auto ref = cx.store.Get(page_id);
            if (!ref.ok()) return ref.status();
            if (IsDataPageOf(ref.value().bytes(), access.oid)) {
                heap::PageView page(ref.value().bytes());
                for (auto it = candidates_.begin(); it != end; ++it) {
                    const std::uint16_t slot = heap::DecodeRowid(*it).slot;
                    auto info = page.DebugSlotInfo(slot);
                    if (!info.ok() || !info.value().dead ||
                        (info.value().flags & heap::kSlotFlagFree) != 0) {
                        continue;
                    }
                    if (Status s = page.FreeSlot(slot); !s.ok()) return s;
                    if (Status s = LogSlotFree(cx.wal, cx.store, page_id, slot); !s.ok()) {
                        return s;
                    }
                    freed.push_back(heap::Rowid{page_id, slot});
                }
            }
        }
        candidates_.erase(candidates_.begin(), end);
        for (const heap::Rowid& r : freed) cx.space.AddFree(access.oid, r);
        counts_.freed += freed.size();
    }
    if (candidates_.empty()) phase_ = Phase::kDone;
    return Status::OK();
}

}  // namespace kds::exec
