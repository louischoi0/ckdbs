#pragma once

#include <cstdint>

#include "kds/catalog/well_known.hpp"
#include "kds/txn/instance_visibility.hpp"

// A snapshot: what one statement, or one transaction, is entitled to see
// (docs/spec/txn.md section 4.1). **A commit-LSN snapshot over the instance
// read view** since AN-S2 (`instructions/v3.0.0/workorder-an-read-view.md`
// AN-R3, AN-R7): a transaction is visible when its commit record's LSN is
// at or below the LSN this view was minted at.
//
// ---- Why not a bound on transaction ids -----------------------------------
//
// Until AN-S2 a view was an exclusive high-water mark over trx ids plus the
// ids in flight when it was minted, and "committed before my snapshot"
// collapsed to "below the mark and not in the set". That holds only while
// issue order is id order, and ids are leased to each core in disjoint
// blocks (`trx_id.hpp`), so across cores it is not: a commit on a core
// holding a higher block reads as "not yet started" (AN-3 E's H1), and a
// transaction begun after the mint out of a lower core's unspent range
// reads as committed (H2). Commit order is the commit record's LSN - one
// stream, one latch, one total order (AR0 M0) - and
// `instance_visibility.hpp` is where that order is published.
//
// ---- A POD, deliberately --------------------------------------------------
//
// Copyable, no heap allocation, fixed size: two integers, a pointer and two
// flags. The reactor body allocates nothing in steady state
// (docs/spec/sched.md), and a read view is minted per statement under READ
// COMMITTED. The 64-entry in-flight array and the bound on live
// transactions it implied went with the trx-id predicate; nothing here
// bounds anything.
//
// ---- What is copied and what is read live ---------------------------------
//
// `snapshot_lsn` is fixed at the mint and never moves. **The floor and the
// window are read through `visibility` at every call**, never copied: a
// copied floor is a lost-row hazard (AN-R3). Reclamation drops a window
// entry *because* the floor has risen past its transaction, so a view
// holding a stale lower floor would miss the entry, decline the floor, and
// answer "not committed" for a row committed long ago. Reading the floor
// live makes the predicate and reclamation agree by construction, and a
// floor that only rises can only move an answer from false to true for a
// transaction every live snapshot already includes.
//
// ---- Reader registration lives on the manager, not here -------------------
//
// The view itself stays a POD that nothing tracks. What records that a
// reader exists is a ReaderLease from TransactionManager::RegisterReader:
// holders whose view can read a superseded version across a park register
// there, the manager publishes its oldest live `snapshot_lsn` into the
// instance's per-core slot, and a purge consults the minimum over cores.

namespace kds::txn {

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
    // Every commit whose record sits at or below this LSN was published
    // when the view was minted (`InstanceVisibility::SnapshotCeiling`,
    // AN-R9); everything above it had not committed, whatever its id.
    std::uint64_t snapshot_lsn = 0;

    // The viewing transaction, or kNoTrxId for a read-only view. A
    // transaction always sees its own writes, including uncommitted ones.
    std::uint64_t own_trx_id = kNoTrxId;

    // Where the floor and the window are read from. Null only for a view
    // built by hand with no instance behind it, which then admits nothing
    // but the always-visible id and its own writes - the safe default, and
    // the one `CheckVisibility` reads as "every writer busy".
    const InstanceVisibility* visibility = nullptr;

    // `Everything()`'s mechanism (AN-R3): every writer visible, no lookup.
    // One predictable branch, and it keeps the POD - the alternative, a
    // sentinel floor, is the copied floor the header above refuses.
    bool sees_everything = false;

    // Another transaction was in flight on the minting core when this view
    // was taken. **Not consulted by `Visible`** - the window answers that
    // per writer - but read by the Cabin's banking rule (cabin.md section
    // 6a): a set banked from a view that could not see an in-flight
    // writer's rows is missing them the moment that writer commits, and
    // this is the fact that used to be `in_flight_count != 0`.
    bool in_flight_at_mint = false;

    // txn.md section 4.1's predicate, in `ratification-an-commit-order.md`
    // AN-Q2's four branches. The order matters: the always-visible arm
    // comes first so a catalog row never reaches the latch, the floor comes
    // before the window so a resolved writer costs one atomic load, and
    // the window is consulted last because it is the branch that takes a
    // latch.
    //
    // **The floor is read twice, and the second read is the one that
    // decides** (AN-R12). The atomic read before the latch is a fast path
    // that can only answer `true`: a floor only rises, so an id below what
    // it read is below what it reads under the hold. An id at or above it
    // goes to `LookupCommit`, which answers the window *and the floor*
    // under one hold - so a reclamation pass that erased this transaction's
    // entry between the two reads is seen with the floor it raised, never
    // as a miss beside a stale floor.
    bool Visible(std::uint64_t trx_id) const {
        if (trx_id == kAlwaysVisibleTrxId) return true;
        if (own_trx_id != kNoTrxId && trx_id == own_trx_id) return true;
        if (sees_everything) return true;
        if (visibility == nullptr) return false;
        if (trx_id < visibility->Floor()) return true;
        const InstanceVisibility::CommitLookup found = visibility->LookupCommit(trx_id);
        if (trx_id < found.floor) return true;
        return found.commit_lsn != kNoCommitLsn && found.commit_lsn <= snapshot_lsn;
    }

    // The view a caller with no transaction manager holds: every writer
    // visible, nothing consulted. This is exactly what the engine did
    // before MVCC existed - every row carried kBootstrapXid and every row
    // was visible - which is why wiring the predicate in behind one of
    // these changes no pre-existing behaviour. Its `snapshot_lsn` is the
    // unbounded bound so that, registered, it holds no horizon: a view that
    // admits every writer needs no superseded version, ever.
    static ReadView Everything() noexcept {
        ReadView view;
        view.sees_everything = true;
        view.snapshot_lsn = kUnboundedBound;
        return view;
    }
};

}  // namespace kds::txn
