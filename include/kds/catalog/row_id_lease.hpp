#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

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
    // The size of the run **currently in hand**, so `low_water()` measures
    // against what was granted rather than against a constant a smaller
    // grant would sit permanently below. Zero on an entry that has never
    // held a grant - which is what "asked for and not yet answered" looks
    // like, and why it reads as low.
    //
    // In hand rather than cumulative, which is `LeasedIdSource`'s rule and
    // arithmetic rather than taste: a contiguous top-up that *added* to this
    // would raise the quarter mark by count/4 every refill, so the run would
    // be asked for again after only 3/4 of it had been issued - a permanent
    // 25% burn of the 40-bit space and a mark that drifts up forever.
    std::uint64_t window = 0;

    // Set when core 0 answered this relation's request with **none**: a
    // relation that no longer has a `sys.tables` row, one whose key mode
    // names its own ids, or a genuinely exhausted 40-bit space. All three
    // are permanent, and a spent entry reads as low water forever, so
    // without this the drain tick would re-ask every cadence and - since
    // one request is in flight per core and `NeediestRelation` answers in
    // oid order - would never get past this oid to any other relation on
    // the core. Demand is cleared rather than the entry dropped: the
    // remainder of a low-water lease is still this core's to issue.
    // `Next()` re-arms it, so a denial costs one request per *failing
    // statement* instead of one per tick, forever.
    bool denied = false;

    bool spent() const noexcept { return next >= end; }
    std::uint64_t remaining() const noexcept { return spent() ? 0 : end - next; }

    // Whether it is time to ask for another run. A leased core must ask
    // **before** the run is spent: `AllocateRowId()` is called from inside
    // an INSERT and cannot await a grant, which is
    // `storage/extent_lease.hpp`'s rule and its quarter-window threshold.
    bool low_water() const noexcept { return window == 0 || remaining() <= window / 4; }
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
        // **A miss records the demand rather than only reporting it** (PW1b).
        // A row-id lease is per relation, so unlike the per-instance
        // transaction-id lease it has no standing subject to pre-empt for -
        // nothing on this core knows an oid needs ids until a statement asks
        // for one. Inserting the spent entry here is what turns the failure
        // into a request the refill tick can act on, so the retry this
        // message promises is one that can succeed rather than a loop.
        RowIdLease& lease = leases_[table_oid];
        if (lease.spent()) {
            // Fresh demand, so a previous "none" stops suppressing it: the
            // message below promises a retry, and a request nobody makes
            // again would make that promise a lie.
            lease.denied = false;
            return Status::ResourceExhausted(
                "row-id lease for relation oid " + std::to_string(table_oid) +
                " is spent; retry after the refill grant lands");
        }
        return lease.next++;
    }

    // The relation most in need of a run, or none. Ordered by oid so the
    // answer is stable run to run - sched.md section 8's determinism rule
    // for anything observable. A denied relation is not need, it is an
    // answer already given (see `RowIdLease::denied`).
    std::optional<Oid> NeediestRelation() const {
        for (const auto& [oid, lease] : leases_) {
            if (!lease.denied && lease.low_water()) return oid;
        }
        return std::nullopt;
    }

    // Records core 0's "none" for a relation this core asked about. Only an
    // entry that already exists: the oid arrives off the ring, and creating
    // one here would let a malformed reply grow this map.
    void Deny(Oid table_oid) {
        if (auto it = leases_.find(table_oid); it != leases_.end()) it->second.denied = true;
    }

    // Applies a grant. A grant that begins exactly where the current lease
    // ends extends it; anything else replaces it and burns the remainder -
    // LeasedIdSource::Grant's rule, for its reason.
    void Grant(Oid table_oid, std::uint64_t first, std::uint64_t count) {
        if (count == 0) return;
        RowIdLease& lease = leases_[table_oid];
        if (!lease.spent() && lease.end == first) {
            lease.end = first + count;
            lease.window = lease.end - lease.next;
            return;
        }
        lease.next = first;
        lease.end = first + count;
        lease.window = count;  // == end - next, the run in hand
    }

    std::uint64_t remaining(Oid table_oid) const {
        auto it = leases_.find(table_oid);
        return it == leases_.end() ? 0 : it->second.remaining();
    }

private:
    // Ordered, for `NeediestRelation`'s stability.
    std::map<Oid, RowIdLease> leases_;
};

}  // namespace kds::catalog
