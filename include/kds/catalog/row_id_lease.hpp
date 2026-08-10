#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "kds/base/status.hpp"
#include "kds/catalog/well_known.hpp"

// Row-id leases: how a core that may not write the catalog issues Keystone
// ids (docs/workplan-crosscore.md P5's shape, the blocker P6 recorded).
//
// `Catalog::AllocateRowId()` bumps `sys.tables.next_id` on a catalog page,
// and catalog pages have exactly one writer - core 0. So a peer holds a
// **leased block of ids per relation**, carved by core 0 through the same
// `AllocateRowIdRange()` the bulk INSERT path uses, and issues from it with
// no message and no catalog write - the page-id lease's design
// (storage/extent_lease.hpp), applied to the one sequence that is
// per-relation rather than per-instance.
//
// The same trade as every lease here: ids are **unique and monotonic per
// core, never gapless**. A crash, a dropped core, or a refill that arrives
// while ids remain burns the remainder - K3 calls a burned id free.
// `docs/keystoneid-invariant.md` K-M2's bump-ahead allocator is this
// mechanism again at a different layer, and its measured block-size floor
// (4096 - below it the durable bump stops amortizing) is the default grant
// size the service uses.
//
// Not thread-safe, deliberately: a table belongs to one reactor, like
// everything else on a core (rules.md #3).

namespace kds::catalog {

// One relation's leased run of row ids on one core.
struct RowIdLease {
    std::uint64_t next = 0;  // the id Next() hands out
    std::uint64_t end = 0;   // one past the last leased id

    bool spent() const noexcept { return next >= end; }
    std::uint64_t remaining() const noexcept { return spent() ? 0 : end - next; }
};

// Per-relation leases for one core: oid -> lease.
class RowIdLeaseTable {
public:
    // The next id for `table_oid`, or **retryable exhaustion** when the
    // lease is spent or absent. The message names the refill because the
    // caller's right response is "retry the statement once the grant
    // lands" - the extent lease's contract, and never OutOfRange, which
    // means the 40-bit space itself is gone and retrying is a lie.
    StatusOr<std::uint64_t> Next(Oid table_oid) {
        auto it = leases_.find(table_oid);
        if (it == leases_.end() || it->second.spent()) {
            return Status::ResourceExhausted(
                "row-id lease for relation oid " + std::to_string(table_oid) +
                " is spent; retry after the refill grant lands");
        }
        return it->second.next++;
    }

    // Applies a grant. A grant that begins exactly where the current lease
    // ends extends it; anything else replaces it and burns the remainder -
    // LeasedIdSource::Grant's rule, for its reason.
    void Grant(Oid table_oid, std::uint64_t first, std::uint64_t count) {
        if (count == 0) return;
        RowIdLease& lease = leases_[table_oid];
        if (!lease.spent() && lease.end == first) {
            lease.end = first + count;
            return;
        }
        lease.next = first;
        lease.end = first + count;
    }

    std::uint64_t remaining(Oid table_oid) const {
        auto it = leases_.find(table_oid);
        return it == leases_.end() ? 0 : it->second.remaining();
    }

private:
    std::unordered_map<Oid, RowIdLease> leases_;
};

}  // namespace kds::catalog
