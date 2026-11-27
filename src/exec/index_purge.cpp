#include "kds/exec/index_purge.hpp"

#include <algorithm>
#include <map>
#include <string>

#include "kds/exec/index_ddl.hpp"
#include "kds/exec/index_maintain.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/index/index_tree.hpp"
#include "kds/storage/log_page_image.hpp"
#include "kds/storage/visit.hpp"
#include "kds/wal/record.hpp"

namespace kds::exec {

namespace {

using Entry = std::vector<std::byte>;

// Phase 1: one leaf's entries, copied under its shared hold, and the leaf
// to its right.
struct CopiedLeaf {
    std::vector<Entry> entries;
    PageId right = kInvalidPageId;
};

StatusOr<CopiedLeaf> CopyLeaf(storage::PageStore& store, PageId leaf,
                              const index::IndexLayout& layout) {
    auto page = index::IndexFetchLeaf(store, leaf, layout, storage::PageAccess::kRead);
    if (!page.ok()) return page.status();
    const index::IndexLeafView view(page.value().bytes());
    CopiedLeaf out;
    out.right = view.right_sibling();
    out.entries.reserve(view.entry_count());
    for (std::uint16_t i = 0; i < view.entry_count(); ++i) {
        auto e = view.Entry(i);
        if (!e.ok()) return e.status();
        out.entries.emplace_back(e.value().begin(), e.value().end());
    }
    return out;
}

// Phases 2 and 3 for one copied leaf: each row it names judged under its
// unit, the dead entries taken out under the same units.
Status PurgeCopiedLeaf(storage::PageStore& store, catalog::Catalog& catalog,
                       const catalog::TableAccess& access, const txn::TransactionManager* txns,
                       wal::WalManager* wal, txn::LockTable& locks, std::uint64_t holder,
                       txn::LockHoldings& holdings, const catalog::TableAccess::IndexRef& ix,
                       const index::IndexLayout& layout, const std::vector<Entry>& copied,
                       IndexPurgeAttempt& out) {
    // By the row each entry names: its tuple key, and on a heap its rowid
    // too (DN) - the judge reads the row where the entry says it is, and an
    // entry left at a slot a key no longer holds is judged as that slot's.
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<const Entry*>> named;
    for (const Entry& e : copied) {
        const std::span<const std::byte> entry(e);
        const std::uint64_t rowid =
            layout.rowid_width != 0 ? index::GetIndexRowid(layout, entry) : 0;
        named[{index::GetIndexTupleKey(entry.subspan(layout.key_width)), rowid}].push_back(&e);
    }
    std::vector<Entry> dead;
    for (const auto& [row, entries] : named) {
        const auto& [tuple_key, rowid] = row;
        auto granted = locks.TryAcquire(holder, txn::LockKey::Tuple(access.oid, tuple_key),
                                        txn::LockMode::kUpdate, holdings);
        if (!granted.ok()) return granted.status();
        if (!granted.value()) {
            ++out.skipped;
            continue;
        }
        auto owed = OwedIndexEntries(store, catalog, access, txns, ix, tuple_key, rowid);
        if (!owed.ok()) {
            return owed.status().WithContext("judging the index entries of tuple key " +
                                             std::to_string(tuple_key));
        }
        for (const Entry* e : entries) {
            const bool served =
                std::any_of(owed.value().begin(), owed.value().end(),
                            [&](const Entry& o) { return SameOwedEntry(access, ix, *e, o); });
            if (!served) dead.push_back(*e);
        }
    }
    if (dead.empty()) return Status::OK();
    std::sort(dead.begin(), dead.end());
    // The root the slot names now: a divide grows it under the attempt.
    auto root = SlotRoot(store, access.anchor_page_id, ix.index_oid);
    if (!root.ok()) return root.status();
    auto removed = index::IndexRemoveEntries(
        store, root.value(), layout, dead, [&](storage::PageRef& held) -> Status {
            return storage::LogFullPageImage(wal, store, wal::kNoTxnId, held.page_id());
        });
    if (!removed.ok()) return removed.status();
    out.removed += removed.value();
    return Status::OK();
}

}  // namespace

Status PurgeIndexLeaves(storage::PageStore& store, catalog::Catalog& catalog,
                        const catalog::TableAccess& access, const txn::TransactionManager* txns,
                        wal::WalManager* wal, txn::LockTable& locks, std::uint64_t holder,
                        txn::LockHoldings& holdings, const catalog::TableAccess::IndexRef& ix,
                        std::span<const std::byte> from, std::uint32_t max_leaves,
                        IndexPurgeAttempt& out) {
    const index::IndexLayout layout = IndexLayoutOf(ix);
    out.resume.clear();

    auto root = SlotRoot(store, access.anchor_page_id, ix.index_oid);
    if (!root.ok()) return root.status();
    Entry seek(layout.sort_key_width(), std::byte{0});
    if (!from.empty()) {
        if (from.size() != seek.size()) {
            return Status::InvalidArgument("an index purge resumes at a whole entry");
        }
        std::copy(from.begin(), from.end(), seek.begin());
    }
    auto at = index::IndexSeekLeaf(store, root.value(), layout, seek);
    if (!at.ok()) return at.status();
    PageId leaf = at.value();

    // Every tuple unit this attempt took, given back in one ledger pass; the
    // relation's `IX` stays to the attempt's decide.
    const auto give_back = [&]() {
        locks.ReleaseIf(holder, holdings, [](const txn::LockHoldings::Held& h) {
            return h.key.unit == txn::LockUnit::kTuple && h.mode == txn::LockMode::kUpdate;
        });
    };

    storage::PageChainGuard guard("index leaf chain");
    for (std::uint32_t walked = 0; leaf != kInvalidPageId && walked < max_leaves; ++walked) {
        if (Status s = guard.Visit(leaf); !s.ok()) return s;
        auto copied = CopyLeaf(store, leaf, layout);
        if (!copied.ok()) return copied.status();
        out.read += copied.value().entries.size();
        const Status purged = PurgeCopiedLeaf(store, catalog, access, txns, wal, locks, holder,
                                              holdings, ix, layout, copied.value().entries, out);
        give_back();
        if (!purged.ok()) return purged;
        leaf = copied.value().right;
    }
    if (leaf != kInvalidPageId) {
        // The next leaf's first entry, read again: it is never removed, so
        // it names that leaf - or one a divide moved it into - next time.
        auto next = CopyLeaf(store, leaf, layout);
        if (!next.ok()) return next.status();
        if (!next.value().entries.empty()) out.resume = std::move(next.value().entries.front());
    }
    return Status::OK();
}

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

Status RelationIndexPurge::Step(storage::PageStore& store, catalog::Catalog& catalog,
                                const catalog::TableAccess& access,
                                const txn::TransactionManager* txns, wal::WalManager* wal,
                                txn::LockTable& locks, std::uint64_t holder,
                                txn::LockHoldings& holdings, std::uint32_t max_leaves,
                                IndexPurgeAttempt& out) {
    const catalog::TableAccess::IndexRef* ix = nullptr;
    while (!done_ && ix == nullptr) {
        if (index_oid_ == 0) {
            const catalog::TableAccess::IndexRef* next = IndexAfter(access, done_oid_);
            if (next == nullptr) {
                done_ = true;
                break;
            }
            index_oid_ = next->index_oid;
            resume_.clear();
        }
        ix = IndexById(access, index_oid_);
        if (ix == nullptr || ix->building) {  // dropped, or DD-R8: passed over
            done_oid_ = index_oid_;
            index_oid_ = 0;
            ix = nullptr;
        }
    }
    if (ix == nullptr) return Status::OK();
    const Status ran = PurgeIndexLeaves(store, catalog, access, txns, wal, locks, holder, holdings,
                                        *ix, resume_, max_leaves, out);
    if (!ran.ok()) return ran;
    resume_ = std::move(out.resume);
    if (resume_.empty()) {
        done_oid_ = index_oid_;
        index_oid_ = 0;
        // The last index walked ends the pass in this step, not the next.
        if (IndexAfter(access, done_oid_) == nullptr) done_ = true;
    }
    return Status::OK();
}

}  // namespace kds::exec
