#include "kds/exec/index_purge_driver.hpp"

#include <algorithm>

#include "kds/txn/manager.hpp"

namespace kds::exec {

void IndexPurgeDriver::Offer(catalog::Oid oid, std::uint64_t deletes, std::uint64_t updates,
                             std::uint64_t commit_ceiling) {
    Relation& r = relations_[oid];
    // A count below the baseline is a statistics reset (a test's): start over.
    if (deletes < r.deletes) r.deletes = deletes;
    if (updates < r.updates) r.updates = updates;
    if (r.queued || (pass_.has_value() && pass_->oid == oid)) return;
    const std::uint64_t by_deletes = r.heap ? 0 : deletes - r.deletes;
    const std::uint64_t by_updates = updates - r.updates;
    if (by_deletes + (by_updates >> r.update_shift) < std::max(r.threshold, min_writes_)) return;
    // Writes after this count toward the next pass: this one may already be
    // past the leaf their entries are on.
    r.deletes = deletes;
    r.updates = updates;
    r.bought_deletes = by_deletes;
    r.bought_updates = by_updates;
    r.mark = commit_ceiling;
    r.queued = true;
    queue_.push_back(oid);
}

std::optional<catalog::Oid> IndexPurgeDriver::Current() {
    if (!pass_.has_value()) {
        if (queue_.empty()) return std::nullopt;
        pass_.emplace();
        pass_->oid = queue_.front();
        queue_.pop_front();
        relations_[pass_->oid].queued = false;
    }
    return pass_->oid;
}

Status IndexPurgeDriver::Step(storage::PageStore& store, catalog::Catalog& catalog,
                              const txn::TransactionManager* txns, wal::WalManager* wal,
                              txn::LockTable& locks, std::uint64_t holder,
                              txn::LockHoldings& holdings, std::uint32_t max_leaves) {
    if (!pass_.has_value()) return Status::OK();
    catalog.Revalidate();
    auto access = catalog.InitTableAccess(pass_->oid);
    if (!access.ok() && access.status().code() != StatusCode::kNotFound) {
        EndPass();
        return access.status();
    }
    // Dropped, or a catalog chain, which has no index.
    if (!access.ok() ||
        access.value()->clustered_type == catalog::ClusteredType::kCatalogChain) {
        EndPass();
        return Status::OK();
    }
    Relation& r = relations_[pass_->oid];
    r.heap = access.value()->heap();
    if (!pass_->started) {
        pass_->started = true;
        pass_->horizon_past_mark = txns != nullptr && txns->ReadHorizon() >= r.mark;
    }
    pass_->indexes = std::max<std::uint64_t>(1, access.value()->indexes.size());
    IndexPurgeAttempt attempt;
    const Status ran = pass_->walk.Step(store, catalog, *access.value(), txns, wal, locks, holder,
                                        holdings, max_leaves, attempt);
    pass_->read += attempt.read;
    pass_->removed += attempt.removed;
    counts_.read += attempt.read;
    counts_.removed += attempt.removed;
    counts_.skipped += attempt.skipped;
    if (!ran.ok() || pass_->walk.done()) EndPass();
    return ran;
}

void IndexPurgeDriver::EndPass() {
    // A pass a refusal, a drop or a catalog chain ended walked too little to
    // say anything: the relation keeps what its last whole pass found.
    if (pass_->walk.done()) {
        Relation& r = relations_[pass_->oid];
        // What an index kept, the indexes averaged: the relation's rows, plus
        // what the pass could not take.
        const std::uint64_t kept = (pass_->read - pass_->removed) / pass_->indexes;
        r.threshold = std::max(min_writes_, kept / kLiveShare);
        // What the pass says of the relation's updates, where no older view
        // stood between it and the writes that bought it: its removals less
        // its deletes' entries are what the updates left.
        if (r.bought_updates != 0 && pass_->horizon_past_mark) {
            const std::uint64_t by_deletes = r.bought_deletes * pass_->indexes;
            const std::uint64_t by_updates = pass_->removed - std::min(pass_->removed, by_deletes);
            r.update_shift = by_updates * kUsefulShare < r.bought_updates
                                 ? std::min(r.update_shift + 1, kMaxUpdateShift)
                                 : 0;
        }
    }
    ++counts_.passes;
    pass_.reset();
}

}  // namespace kds::exec
