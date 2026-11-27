#include "kds/exec/index_purge_driver.hpp"

#include <algorithm>

namespace kds::exec {

void IndexPurgeDriver::Offer(catalog::Oid oid, std::uint64_t deletes, std::uint64_t key_updates) {
    Relation& r = relations_[oid];
    // A count below the baseline is a statistics reset (a test's): start over.
    if (deletes < r.deletes) r.deletes = deletes;
    if (key_updates < r.key_updates) r.key_updates = key_updates;
    if (r.queued || (pass_.has_value() && pass_->oid == oid)) return;
    const std::uint64_t writes = (r.heap ? 0 : deletes - r.deletes) + (key_updates - r.key_updates);
    if (writes < std::max(r.threshold, min_writes_)) return;
    // Writes after this count toward the next pass: this one may already be
    // past the leaf their entries are on.
    r.deletes = deletes;
    r.key_updates = key_updates;
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
    relations_[pass_->oid].heap = access.value()->heap();
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
    // say what the relation holds: it keeps its last whole pass's threshold.
    if (pass_->walk.done()) {
        // What an index kept, the indexes averaged: the relation's rows, plus
        // what the pass could not take.
        const std::uint64_t kept = (pass_->read - pass_->removed) / pass_->indexes;
        relations_[pass_->oid].threshold = kept / kLiveShare;
    }
    ++counts_.passes;
    pass_.reset();
}

}  // namespace kds::exec
