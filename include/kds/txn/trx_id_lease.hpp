#pragma once

#include <cstdint>
#include <string>

#include "kds/base/status.hpp"

// Transaction-id leases: how a core that may not write the superblock issues
// transaction ids (`docs/inflight/in-progress/workplan-peer-writer.md` PW1).
//
// `TrxIdSequence::Carve()` raises `SuperBlock::next_trx_id` and persists it,
// and the superblock is page 0 - core 0's, by M5. So a peer holds a **leased
// block**, carved by core 0 through that same `Carve()` and delivered over
// `RingMessageKind::kTrxIdLease`, and issues from it with no superblock write
// and no message per id.
//
// This is `catalog/row_id_lease.hpp` again for the sequence that is
// per-instance rather than per-relation - which is why nothing here carries
// an oid, and why the window lives in the `TrxIdSequence` itself instead of
// in a table. Both are `storage/extent_lease.hpp`'s design at a third layer.
//
// The same trade as every lease here: ids are **unique and monotonic per
// core, never gapless**. A crash, a stopped core, or a grant that arrives
// while ids remain burns the remainder, and a burned transaction id costs
// nothing - `trx_id.hpp` already states that gaplessness is a property
// nothing needs.
//
// Not thread-safe, deliberately: a lease belongs to one reactor, like
// everything else on a core (rules.md #3).

namespace kds::txn {

// A reserved run of ids: `[first, first + count)`.
struct TrxIdRange {
    std::uint64_t first = 0;
    std::uint64_t count = 0;

    bool empty() const noexcept { return count == 0; }
};

// One core's pending grant. At most one is held: the sequence consumes the
// whole block when its own window is spent, so a second grant before that
// happens is the only case `Grant` has to reconcile.
class TrxIdLease {
public:
    // Applies a grant. A grant that begins exactly where the pending one
    // ends extends it; anything else replaces it and burns the remainder -
    // `RowIdLeaseTable::Grant`'s rule, for its reason.
    void Grant(std::uint64_t first, std::uint64_t count) noexcept {
        if (count == 0) return;
        if (pending_.count > 0 && pending_.first + pending_.count == first) {
            pending_.count += count;
            return;
        }
        pending_ = TrxIdRange{first, count};
    }

    // The pending block, consumed. **Retryable exhaustion** when there is
    // none, never `OutOfRange`: the caller's right response is "retry the
    // statement once the grant lands", and `OutOfRange` would mean the
    // 48-bit space itself is gone, which retrying cannot fix.
    StatusOr<TrxIdRange> Take() {
        if (pending_.count == 0) {
            // TxnConflict, not ResourceExhausted: status.hpp's IsRetryable says why.
            return Status::TxnConflict(
                "this core's transaction-id lease is spent; retry after the refill grant lands");
        }
        const TrxIdRange taken = pending_;
        pending_ = TrxIdRange{};
        return taken;
    }

    bool pending() const noexcept { return pending_.count > 0; }
    std::uint64_t pending_count() const noexcept { return pending_.count; }

private:
    TrxIdRange pending_{};
};

}  // namespace kds::txn
