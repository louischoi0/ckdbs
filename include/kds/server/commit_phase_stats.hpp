#pragma once

#include <cstdint>

// **What each leg of a cross-owner commit actually costs** (work order XF,
// row XF4, `instructions/v2.7.1/workorder-xf.md`).
//
// ---- Why this file exists ------------------------------------------------
//
// Three results files have now billed for it and none could pay:
// `results-crosscore-2pc` §8 (R6-B), `results-xd-commit-decomposition` §8
// and `results-xe-ack-at-append` §6 each name the same absence — **every
// mechanism claim about the commit chain is an inference from totals**.
// XD counted the chain's syncs (`wal_syncs`, `SHOW META`) and XE moved
// where one of them is waited for, and neither could say which leg the
// time was in: XE's own §5 offers a reading of its b=8 result and calls it
// "offered, not proven", because nothing measures a leg.
//
// ---- The shape, and why it is not a histogram ---------------------------
//
// Count, total and maximum per leg. A histogram would answer more, and it
// needs **bucket bounds** — constants, which this order's conclusion 4
// says are stopped on and reported rather than chosen. Count/total/max
// needs no bound, answers the question the three files actually asked
// (*did this leg's own time move, or only the total?*), and is the shape
// `SHOW META` already prints for the lease refills beside it
// (`lease_refill_stats.hpp`, the `*_max_us` fields). A second shape for
// one concept is what that file's own header warns against.
//
// Nanoseconds here, microseconds on the wire — the refill block's
// convention, so one reading rule covers both.
//
// ---- Sched-free, deliberately -------------------------------------------
//
// `lease_refill_stats.hpp` states the rule and this file keeps it: the
// dispatcher prints these from `SHOW META` and the dispatcher header must
// not drag the scheduler in. Plain `std::uint64_t` nanoseconds; the caller
// reads its own clock.
//
// ---- What a one-owner commit pays: nothing ------------------------------
//
// Every `Note` below is called from inside the cross-owner path — the
// coordinator's `pending_cross_owner_commit` block and the participant's
// prepare/decide handlers. A transaction with no participants enters
// neither, so it reads no clock it did not read before. That is XF4's cost
// guard, and it holds structurally rather than by measurement.

namespace kds::server {

// One leg: how many times it was walked, how long in total, and the worst.
struct PhaseLeg {
    std::uint64_t count = 0;
    std::uint64_t total_ns = 0;
    std::uint64_t max_ns = 0;

    // A span, taken as an already-computed duration rather than a pair of
    // stamps: the two ends are read in different functions on some legs
    // (a park's start and its resumption), so the subtraction happens where
    // both are in hand and this type stays a pure accumulator.
    //
    // **A negative span cannot arise and is not defended against**, because
    // both stamps come from one core's monotonic clock and the engine has
    // no cross-core stamp comparison (`crosscore.md` §5's correction, about
    // watermarks, is the same argument about a different quantity).
    void Note(std::uint64_t span_ns) noexcept {
        ++count;
        total_ns += span_ns;
        if (span_ns > max_ns) max_ns = span_ns;
    }
};

// The coordinator's four legs. Its own core's, and its own clock's.
//
//   BEGIN … COMMIT
//     │
//     ├── prepare sent ──(1)──> every participant settled
//     ├── local commit ──(2)──> the decision record durable
//     ├── decide sent  ──(3)──> every participant acknowledged
//     └── the whole of it ─(4)
//
// (4) is not (1)+(2)+(3): it spans the refusal arms and the message sends
// between the legs, so `whole − (1+2+3)` is the coordinator's own
// unaccounted time and is the number that says whether these three legs
// are the chain.
struct CoordinatorCommitStats {
    PhaseLeg prepare;   // (1)
    PhaseLeg decision;  // (2)
    PhaseLeg decide;    // (3)
    PhaseLeg whole;     // (4)

    bool observed() const noexcept { return whole.count != 0; }
};

// The participant's three, and the last two are the pair XE1 moved.
//
//   PREPARE appended ──(1)──> durable        (the promise the coordinator waits on)
//   decide received  ──(2)──> coordinator acknowledged
//   decide received  ──(3)──> this core's own terminal record durable
//
// **(2) and (3) are the instrument XE1 needs and could not have.** Before
// XE1 they are the same instant by construction — the ack was given when
// the record was durable — so (3) − (2) is 0 and the pair says so. Under
// XE1's `kAtAppend` the ack leaves at the append and (3) − (2) is the wait
// that moved off the chain. Whether that wait then *disappears* (a drain
// shared with somebody else's commit) or merely *moves* (the next
// transaction pays it) is exactly what `results-xe-ack-at-append` §5 could
// only offer, and it is the difference between (3) growing and (3) staying
// where it was.
//
// (3) costs one parked coroutine per decide on the participant, stated
// because it is a cost on the path being measured: on the pre-XE1 arm the
// record is already durable when the park is submitted, so it resolves on
// its first poll; on the XE1 arm it is a real wait held by nothing else.
struct ParticipantCommitStats {
    PhaseLeg prepare_durable;  // (1)
    PhaseLeg decide_ack;       // (2)
    PhaseLeg decide_durable;   // (3)

    bool observed() const noexcept {
        return prepare_durable.count != 0 || decide_ack.count != 0;
    }
};

// **The foreign key's two rounds** (AH-T6, `foreign-keys.md` §2a/§2b).
//
//   fork: last probe sent ──(1)──> every owner's reply settled
//   autocommit: decide *begun* ──(2)──> every holder acknowledged
//
// (2) opens before the send rather than after it, which is
// `CoordinatorCommitStats::decide`'s own convention - one reading rule for
// both - and means the leg carries the encode and the ring submit as well
// as the wait. (1) does not, because its send happens in a different
// function from its park and the stamp is taken where the last request is
// already away.
//
// The same shape and the same reason as the commit legs above: three
// results files could say a cross-owner write costs more and none could say
// **which round**. `results-ai-t3-fk-crossing-cost` prices the two together
// at +56.8 us and names the split as owed; these are what pay it.
//
// (2) exists only on an autocommit statement. Inside a transaction the
// release rides the commit's own decide, which `CoordinatorCommitStats`
// already times - so a transaction's release is not missing from the
// instrument, it is in the leg above under a different name, and a reader
// comparing the two shapes must not add them.
struct ForeignKeyRoundStats {
    PhaseLeg probe;   // (1)
    PhaseLeg decide;  // (2)

    bool observed() const noexcept { return probe.count != 0 || decide.count != 0; }
};

}  // namespace kds::server
