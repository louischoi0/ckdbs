#include "kds/server/core_affinity.hpp"

namespace kds::server {


Status CrossCoreReadNotImplemented(std::uint32_t this_core, std::uint32_t target_core,
                                std::string_view relation) {
    // **The pipeline is built; this shape cannot take it** (R4-R/RS0). The
    // old spelling said cross-core reads "need the step pipeline, which is
    // not built", which has been false since RD7 and doubly so since RR2
    // gave every core a fan-in client. What is true is narrower and is what
    // the message now says: the route serves a whole-row read of one
    // relation outside a transaction, and a statement reaching here is not
    // that - a projection, an aggregate, a quota, a sort the compiler could
    // not elide, a sub-chain, an ANALYZE, a scope spanning two owners, or a
    // read inside an explicit transaction.
    return Status::NotImplemented(
        "relation '" + std::string(relation) + "' is owned by core " +
        std::to_string(target_core) + " and this statement is running on core " +
        std::to_string(this_core) +
        "; the step pipeline is built and serves a whole-row read of one relation outside a "
        "transaction, and this statement is not that shape");
}

}  // namespace kds::server
