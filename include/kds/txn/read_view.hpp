#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "kds/base/status.hpp"
#include "kds/catalog/well_known.hpp"

// A snapshot: what one statement, or one transaction, is entitled to see
// (docs/spec/txn.md section 4.1).
//
// ---- A POD, deliberately ---------------------------------------------------
//
// Copyable, no heap allocation, fixed size. The reactor body allocates
// nothing in steady state (docs/spec/sched.md), and a read view is minted per
// statement under READ COMMITTED - so a view that allocated would put a
// malloc on every statement's front door. Begin past kMaxTrackedLiveTxns is
// OutOfSpace: a documented, testable bound rather than an unbounded vector.
//
// ---- Why no commit table is needed, and the condition on that -------------
//
// "Committed before my snapshot" collapses to "below the high-water mark and
// not in my in-flight set" *only* because an aborted transaction's page
// changes are physically undone, synchronously, in-process (txn.md section
// 6). That is the load-bearing assumption of the whole design, and section 8
// states the crash consequence it implies: after a crash mid-transaction,
// an uncommitted row's writer is below the new boot's high-water mark and in
// no live set, so it reads as **committed**. Closing that needs a persisted
// commit watermark, i.e. recovery. It is the single thing recovery must
// revisit here.
//
// ---- Reader registration lives on the manager, not here -------------------
//
// The view itself stays a POD that nothing tracks. What records that a
// reader exists is a ReaderLease from TransactionManager::RegisterReader
// (docs/workplan-reader-registration.md): holders whose view can read a
// superseded version across a park register there, and a purge consults
// the manager's ReadHorizon(). MinVisibleBound below is the one number a
// registration stores about a view.

namespace kds::txn {

// As kMaxWalCores. A bound on *concurrently live* transactions, not on
// transactions ever seen.
inline constexpr std::size_t kMaxTrackedLiveTxns = 64;

// Visible to every read view, unconditionally and permanently (txn.md
// section 4.2). Not a migration shim that ages out: every catalog row
// carries it forever, and it is the tail of every undo chain built over a
// pre-existing row. SuperBlock seeds the sequence at kFirstUserTrxId so it
// is never reissued to a real transaction.
inline constexpr std::uint64_t kAlwaysVisibleTrxId = catalog::kBootstrapXid;

// Read-only views carry this as their own id: no transaction owns them, and
// no real transaction is ever issued 0.
inline constexpr std::uint64_t kNoTrxId = 0;

struct ReadView {
    // Exclusive high-water mark: ids >= this had not started when the view
    // was taken, so they are invisible however they end.
    std::uint64_t up_to_trx_id = 0;

    // The viewing transaction, or kNoTrxId for a read-only view. A
    // transaction always sees its own writes, including uncommitted ones.
    std::uint64_t own_trx_id = kNoTrxId;

    // Live-but-not-committed when the view was taken, sorted ascending so
    // the membership test can stop early. Not a set type: 64 entries scan
    // faster than they hash, and a std::array keeps the whole view a POD.
    std::array<std::uint64_t, kMaxTrackedLiveTxns> in_flight{};
    std::size_t in_flight_count = 0;

    // txn.md section 4.1's predicate, verbatim. The order matters: the
    // always-visible arm comes first so a catalog row never touches the
    // in-flight scan, and the high-water test comes before it so a
    // transaction that started after this view can never be found in a set
    // it was too late to join.
    constexpr bool Visible(std::uint64_t trx_id) const noexcept {
        if (trx_id == kAlwaysVisibleTrxId) return true;
        if (own_trx_id != kNoTrxId && trx_id == own_trx_id) return true;
        if (trx_id >= up_to_trx_id) return false;
        for (std::size_t i = 0; i < in_flight_count; ++i) {
            if (in_flight[i] == trx_id) return false;
            if (in_flight[i] > trx_id) break;  // sorted: no later entry can match
        }
        return true;
    }

    // Adds one live transaction, keeping the array sorted. Fails with
    // OutOfSpace past kMaxTrackedLiveTxns - the bound above, surfaced
    // rather than silently dropping an id, because a dropped in-flight id
    // makes an uncommitted row visible.
    Status AddInFlight(std::uint64_t trx_id) {
        if (in_flight_count >= kMaxTrackedLiveTxns) {
            return Status::OutOfSpace("more than " + std::to_string(kMaxTrackedLiveTxns) +
                                      " live transactions; the read view cannot track them");
        }
        std::size_t at = in_flight_count;
        while (at > 0 && in_flight[at - 1] > trx_id) {
            in_flight[at] = in_flight[at - 1];
            --at;
        }
        in_flight[at] = trx_id;
        ++in_flight_count;
        return Status::OK();
    }

    // The smallest transaction id this view might still need a superseded
    // version from - equivalently, the smallest id for which Visible() is
    // not yet guaranteed true. Everything below it is visible to this view
    // whatever else happens, so a purge holding "superseded by a committed
    // id < every live view's bound" retires nothing this view can reach.
    // The three terms mirror Visible()'s three refusal routes: the
    // high-water mark, the smallest in-flight id (sorted, so [0]), and the
    // owner's own id - whose versions its rollback may still need.
    constexpr std::uint64_t MinVisibleBound() const noexcept {
        std::uint64_t bound = up_to_trx_id;
        if (in_flight_count > 0 && in_flight[0] < bound) bound = in_flight[0];
        if (own_trx_id != kNoTrxId && own_trx_id < bound) bound = own_trx_id;
        return bound;
    }

    // The view a caller with no transaction manager holds: everything below
    // the high-water mark is committed, nothing is in flight. This is
    // exactly what the engine did before MVCC existed - every row carried
    // kBootstrapXid and every row was visible - which is why wiring the
    // predicate in behind one of these changes no pre-existing behaviour.
    static ReadView Everything() noexcept {
        ReadView view;
        view.up_to_trx_id = UINT64_MAX;
        view.own_trx_id = kNoTrxId;
        view.in_flight_count = 0;
        return view;
    }
};

}  // namespace kds::txn
