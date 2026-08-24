#include "kds/server/core_affinity.hpp"

namespace kds::server {

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

Status PeerDdlRefused(std::uint32_t this_core, std::string_view verb) {
    return Status::Unsupported(
        std::string(verb) + " is DDL, and core " + std::to_string(this_core) +
        " takes no DDL: the catalog has one writer, the system core - connect to core 0 "
        "(workplan-peer-writer.md PW4)");
}

}  // namespace kds::server
