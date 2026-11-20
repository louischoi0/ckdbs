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
                       wal::WalManager* wal, txn::LockTable& locks, std::uint64_t txn,
                       txn::LockHoldings& holdings, const catalog::TableAccess::IndexRef& ix,
                       const index::IndexLayout& layout, const std::vector<Entry>& copied,
                       IndexPurgeAttempt& out) {
    std::map<std::uint64_t, std::vector<const Entry*>> named;
    for (const Entry& e : copied) {
        named[index::GetIndexTupleKey(std::span<const std::byte>(e).subspan(layout.key_width))]
            .push_back(&e);
    }
    std::vector<Entry> dead;
    for (const auto& [tuple_key, entries] : named) {
        auto granted = locks.TryAcquire(txn, txn::LockKey::Tuple(access.oid, tuple_key),
                                        txn::LockMode::kUpdate, holdings);
        if (!granted.ok()) return granted.status();
        if (!granted.value()) {
            ++out.skipped;
            continue;
        }
        auto owed = OwedIndexEntries(store, catalog, access, txns, ix, tuple_key);
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
                        wal::WalManager* wal, txn::LockTable& locks, std::uint64_t txn,
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
        locks.ReleaseIf(txn, holdings, [](const txn::LockHoldings::Held& h) {
            return h.key.unit == txn::LockUnit::kTuple && h.mode == txn::LockMode::kUpdate;
        });
    };

    storage::PageChainGuard guard("index leaf chain");
    for (std::uint32_t walked = 0; leaf != kInvalidPageId && walked < max_leaves; ++walked) {
        if (Status s = guard.Visit(leaf); !s.ok()) return s;
        auto copied = CopyLeaf(store, leaf, layout);
        if (!copied.ok()) return copied.status();
        const Status purged = PurgeCopiedLeaf(store, catalog, access, txns, wal, locks, txn,
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

}  // namespace kds::exec
