#include "kds/server/core_affinity.hpp"

#include <algorithm>

namespace kds::server {

void RelationGrantDemand::Record(catalog::Oid table_oid) {
    if (std::find(pending_.begin(), pending_.end(), table_oid) != pending_.end()) return;
    pending_.push_back(table_oid);
}

std::optional<catalog::Oid> RelationGrantDemand::Pop() {
    if (pending_.empty()) return std::nullopt;
    const catalog::Oid oid = pending_.front();
    pending_.erase(pending_.begin());
    return oid;
}

Status CrossCoreWriteRefused(std::uint32_t home_core, std::uint32_t target_core,
                             std::string_view relation) {
    // kTxnConflict, not a new code: from the client's side this *is* the
    // first-updater-wins abort - the transaction cannot proceed and a retry
    // may work - and a client that already handles TXN_CONFLICT needs no new
    // code for it. The message says which restriction it hit, so the two are
    // still distinguishable by a human reading a log.
    return Status::TxnConflict(
        "this transaction's writes are bound to core " + std::to_string(home_core) +
        " and relation '" + std::string(relation) + "' is owned by core " +
        std::to_string(target_core) +
        "; a transaction may write on one core only until two-phase commit exists");
}

Status CrossCoreReadUnsupported(std::uint32_t this_core, std::uint32_t target_core,
                                std::string_view relation) {
    return Status::Unsupported(
        "relation '" + std::string(relation) + "' is owned by core " +
        std::to_string(target_core) + " and this statement is running on core " +
        std::to_string(this_core) +
        "; cross-core reads need the step pipeline, which is not built");
}

Status RelationWriteRightsPending(std::uint32_t this_core, std::string_view relation) {
    // kTxnConflict for CrossCoreWriteRefused's reason: a retry may work -
    // the drain tick asks the system core to re-deliver, and the grant
    // lands within a few ticks. The message names the relation and the
    // cause class, where the store's own refusal would name a page id.
    return Status::TxnConflict(
        "relation '" + std::string(relation) + "' is owned by core " +
        std::to_string(this_core) +
        " but its write rights are not held here - a grant lost to a restart, a crash before "
        "acquisition, or the ring; re-delivery has been requested from the system core, retry "
        "(workplan-peer-writer.md PW1c-7)");
}

Status IndexBuildPending(std::uint32_t this_core, std::string_view relation) {
    // kTxnConflict for RelationWriteRightsPending's reason: the window
    // closes when the statement ends on the system core, and a retry then
    // writes.
    return Status::TxnConflict(
        "relation '" + std::string(relation) + "' has an index being built on core " +
        std::to_string(this_core) +
        " and takes no writes until that CREATE INDEX ends on the system core; retry "
        "(workplan-peer-writer.md PW1c-6b)");
}

void PendingIndexBuilds::Open(catalog::Oid table_oid, std::uint64_t index_oid,
                              std::uint64_t now_ns) {
    entries_.push_back(Entry{table_oid, index_oid, now_ns});
}

bool PendingIndexBuilds::Close(std::uint64_t index_oid) {
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [index_oid](const Entry& e) { return e.index_oid == index_oid; });
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
}

bool PendingIndexBuilds::Covers(catalog::Oid table_oid) const noexcept {
    return std::any_of(entries_.begin(), entries_.end(),
                       [table_oid](const Entry& e) { return e.table_oid == table_oid; });
}

std::vector<PendingIndexBuilds::Entry> PendingIndexBuilds::Expire(std::uint64_t now_ns,
                                                                  std::uint64_t ceiling_ns) {
    std::vector<Entry> expired;
    auto keep = entries_.begin();
    for (const Entry& e : entries_) {
        if (now_ns >= e.opened_at_ns && now_ns - e.opened_at_ns >= ceiling_ns) {
            expired.push_back(e);
        } else {
            *keep++ = e;
        }
    }
    entries_.erase(keep, entries_.end());
    return expired;
}

Status PeerDdlRefused(std::uint32_t this_core, std::string_view verb) {
    return Status::Unsupported(
        std::string(verb) + " is DDL, and core " + std::to_string(this_core) +
        " takes no DDL: the catalog has one writer, the system core - connect to core 0 "
        "(workplan-peer-writer.md PW4)");
}

}  // namespace kds::server
