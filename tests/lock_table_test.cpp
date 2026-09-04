#include "kds/txn/lock_table.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "kds/wire/error_registry.hpp"

// AO-S1 (`instructions/v3.0.0/workorder-ao-m2-lock-family.md`): the lock
// family's pure core - the table, its keys, its modes, its compatibility,
// the relation entry's counters, the queue as a data structure, and the
// cap. **No scheduler**: a conflict is an answer here, not a park, so
// nothing in this file waits on anything. The park is AO-S2's and the
// wait-for graph AO-S4a's.

namespace kds::txn {
namespace {

constexpr catalog::Oid kRel = 4018;
constexpr catalog::Oid kOtherRel = 4026;

std::unique_ptr<LockTable> MakeTable(std::uint32_t cores = 8,
                                     std::size_t cap = kMaxLocksPerTxnDefault) {
    auto t = LockTable::Create(cores, cap);
    EXPECT_TRUE(t.ok()) << t.status().message();
    return std::move(t.value());
}

// ---- The compatibility matrix, one cell per mode pair --------------------

TEST(LockTableTest, TheCompatibilityMatrixIsTheOneAR2States) {
    // AR2 §2: `IS` conflicts only with `X`; `IX` with `S` and `X`; `S` with
    // `IX` and `X`; `X` with everything. Written out as sixteen literal
    // expectations rather than looped over `Compatible`, so that a change
    // to the function cannot also change what the cell believes.
    constexpr LockMode kIS = LockMode::kIntentionShared;
    constexpr LockMode kIX = LockMode::kIntentionExclusive;
    constexpr LockMode kS = LockMode::kShared;
    constexpr LockMode kX = LockMode::kExclusive;

    EXPECT_TRUE(Compatible(kIS, kIS));
    EXPECT_TRUE(Compatible(kIS, kIX));
    EXPECT_TRUE(Compatible(kIS, kS));
    EXPECT_FALSE(Compatible(kIS, kX));

    EXPECT_TRUE(Compatible(kIX, kIS));
    EXPECT_TRUE(Compatible(kIX, kIX));
    EXPECT_FALSE(Compatible(kIX, kS));
    EXPECT_FALSE(Compatible(kIX, kX));

    EXPECT_TRUE(Compatible(kS, kIS));
    EXPECT_FALSE(Compatible(kS, kIX));
    EXPECT_TRUE(Compatible(kS, kS));
    EXPECT_FALSE(Compatible(kS, kX));

    EXPECT_FALSE(Compatible(kX, kIS));
    EXPECT_FALSE(Compatible(kX, kIX));
    EXPECT_FALSE(Compatible(kX, kS));
    EXPECT_FALSE(Compatible(kX, kX));

    // Symmetric, which is a property of the relation and not of the table's
    // walk order: the table tests a held mode against a wanted one and
    // would answer differently per arrival order if this failed.
    for (LockMode a : {kIS, kIX, kS, kX}) {
        for (LockMode b : {kIS, kIX, kS, kX}) {
            EXPECT_EQ(Compatible(a, b), Compatible(b, a))
                << LockModeName(a) << " vs " << LockModeName(b);
        }
    }
}

TEST(LockTableTest, EveryConflictingModePairIsRefusedOnOneUnit) {
    // The matrix above as behaviour: for each pair, two transactions on one
    // tuple. The compatible pairs are both granted and the conflicting ones
    // name the holder.
    constexpr LockMode kModes[] = {LockMode::kIntentionShared, LockMode::kIntentionExclusive,
                                   LockMode::kShared, LockMode::kExclusive};
    for (LockMode held : kModes) {
        for (LockMode want : kModes) {
            auto table = MakeTable();
            LockHoldings h1;
            LockHoldings h2;
            const LockKey key = LockKey::Tuple(kRel, 7);

            auto first = table->Acquire(1, key, held, h1);
            ASSERT_TRUE(first.ok());
            EXPECT_TRUE(first.value().granted);

            auto second = table->Acquire(2, key, want, h2);
            ASSERT_TRUE(second.ok());
            EXPECT_EQ(second.value().granted, Compatible(held, want))
                << "holder " << LockModeName(held) << ", requester " << LockModeName(want);
            if (!second.value().granted) {
                EXPECT_EQ(second.value().blocking_txn, 1u)
                    << "a refused borrow names the holder it waits for - the edge AO-S4a's "
                       "wait-for graph reads";
                EXPECT_EQ(table->WaiterCount(key), 1u)
                    << "the queue is a data structure at S1: a refused request is recorded even "
                       "though nothing parks on it yet";
                EXPECT_EQ(h2.waiting_on(), key)
                    << "and it is recorded on the requester, which is what lets it be withdrawn";
            }
        }
    }
}

TEST(LockTableTest, ATransactionNeverConflictsWithItself) {
    // A statement that takes `IX` on a unit and then `X` on it must proceed:
    // a transaction cannot deadlock against itself, and the table refusing
    // here would turn an ordinary escalation-free write path into a
    // permanent refusal.
    auto table = MakeTable();
    LockHoldings h;
    const LockKey key = LockKey::Tuple(kRel, 9);

    ASSERT_TRUE(table->Acquire(1, key, LockMode::kIntentionExclusive, h).value().granted);
    auto second = table->Acquire(1, key, LockMode::kExclusive, h);
    ASSERT_TRUE(second.ok());
    EXPECT_TRUE(second.value().granted);
    EXPECT_FALSE(second.value().already_held) << "IX does not cover X; this is a second entry";
    EXPECT_EQ(h.size(), 2u);
}

TEST(LockTableTest, ARepeatAcquireOfACoveredModeAddsNoEntryAndSpendsNoCap) {
    // A walk that re-enters a unit it already borrowed must not spend the
    // cap per visit (AR2-R4 counts entries).
    auto table = MakeTable();
    LockHoldings h;
    const LockKey key = LockKey::Relation(kRel);

    ASSERT_TRUE(table->Acquire(1, key, LockMode::kExclusive, h).value().granted);
    EXPECT_EQ(h.size(), 1u);

    for (LockMode weaker : {LockMode::kIntentionShared, LockMode::kIntentionExclusive,
                            LockMode::kShared, LockMode::kExclusive}) {
        auto again = table->Acquire(1, key, weaker, h);
        ASSERT_TRUE(again.ok());
        EXPECT_TRUE(again.value().granted);
        EXPECT_TRUE(again.value().already_held) << "X covers " << LockModeName(weaker);
    }
    EXPECT_EQ(h.size(), 1u) << "no entry was added by any of the four";
    EXPECT_EQ(table->EntryCount(), 1u);

    // `S` does not cover `IX` and `IX` does not cover `S`, because v1 has no
    // `SIX` (E1). A transaction needing both holds both entries, which is
    // the honest accounting rather than a silent upgrade.
    EXPECT_FALSE(Covers(LockMode::kShared, LockMode::kIntentionExclusive));
    EXPECT_FALSE(Covers(LockMode::kIntentionExclusive, LockMode::kShared));
}

// ---- What an uncontended borrow costs the table --------------------------

TEST(LockTableTest, AnUncontendedExclusiveRegistersOneEntryAndTheTableEmptiesOnRelease) {
    // **Not AR2-R2's "an uncontended write touches no table entry for its
    // tuple"** - that claim is about the Keystone stamp being the fast path
    // and becomes testable only at AO-S3, where a writer chooses between
    // the stamp and the table. This table registers what it is asked to
    // register; the cell pins that it registers exactly one and gives it
    // all back.
    auto table = MakeTable();
    LockHoldings h;
    EXPECT_EQ(table->EntryCount(), 0u);

    ASSERT_TRUE(table->Acquire(1, LockKey::Tuple(kRel, 42), LockMode::kExclusive, h).value().granted);
    EXPECT_EQ(table->EntryCount(), 1u) << "one entry per borrowed unit, and no entry for the "
                                          "transaction itself";
    EXPECT_EQ(h.size(), 1u);
    EXPECT_EQ(table->WaiterCount(LockKey::Tuple(kRel, 42)), 0u);

    table->Release(1, h);
    EXPECT_EQ(table->EntryCount(), 0u)
        << "an entry nobody holds and nobody waits on is erased, so the table measures live "
           "borrows rather than every key ever touched";
    EXPECT_TRUE(h.empty());
}

TEST(LockTableTest, TheUnitKindIsInTheKeyBecauseFourUnitsShareABound) {
    // What the unit kind in AO-R2's key actually buys. For a tuple with
    // `pk != 0` the bound alone already separates it from its relation, so
    // a collision census over many tuples measures the hash rather than the
    // design. The keys that need the unit kind are the four carrying the
    // **same** `lo = 0`: without it they would be one entry, and a relation
    // `X` would be indistinguishable from a tuple `X` on row 0.
    auto table = MakeTable();
    const LockKey rel = LockKey::Relation(kRel);
    const LockKey tuple0 = LockKey::Tuple(kRel, 0);
    const LockKey slice0 = LockKey::Slice(kRel, 0, 100);
    const LockKey range0 = LockKey::Range(kRel, 0, 100);

    EXPECT_FALSE(rel == tuple0);
    EXPECT_FALSE(rel == slice0);
    EXPECT_FALSE(slice0 == range0) << "same relation, same interval, different unit";
    EXPECT_FALSE(LockKey::Relation(kRel) == LockKey::Relation(kOtherRel));

    // Separated in the table and not merely as values: two transactions
    // taking `X` on the relation and on row 0 both succeed, which they
    // could not if the keys collided into one entry.
    LockHoldings h1;
    LockHoldings h2;
    ASSERT_TRUE(table->Acquire(1, rel, LockMode::kExclusive, h1).value().granted);
    auto row = table->Acquire(2, tuple0, LockMode::kExclusive, h2);
    ASSERT_TRUE(row.ok());
    EXPECT_TRUE(row.value().granted);
    EXPECT_EQ(table->EntryCount(), 2u);
    table->Release(1, h1);
    table->Release(2, h2);
}

TEST(LockTableTest, TheTableDoesNotInferTheAncestorChain) {
    // The behaviour above, stated as the obligation it puts on a caller.
    // AR2-R3's design is that a relation `X` conflicts with a tuple `X`
    // beneath it **because the tuple's writer takes `IX` at the relation**
    // - the table checks one unit at a time and infers nothing. A caller
    // that borrows a tuple without the intention chain gets a wrong answer
    // quietly, the `[quiet-wrong]` class AR0 marks; AO-S3 is where callers
    // exist and where the obligation becomes checkable.
    auto table = MakeTable();
    LockHoldings ddl;
    LockHoldings writer;
    ASSERT_TRUE(
        table->Acquire(1, LockKey::Relation(kRel), LockMode::kExclusive, ddl).value().granted);

    auto no_chain = table->Acquire(2, LockKey::Tuple(kRel, 5), LockMode::kExclusive, writer);
    ASSERT_TRUE(no_chain.ok());
    EXPECT_TRUE(no_chain.value().granted)
        << "the table cannot see that a relation X is held above this tuple; nothing here is "
           "wrong, and everything about it depends on the caller taking IX first";

    // Taken properly, the same writer is refused at the relation entry -
    // one compatibility check, which is what the intention modes are for.
    LockHoldings proper;
    auto chained =
        table->Acquire(3, LockKey::Relation(kRel), LockMode::kIntentionExclusive, proper);
    ASSERT_TRUE(chained.ok());
    EXPECT_FALSE(chained.value().granted);
    EXPECT_EQ(chained.value().blocking_txn, 1u);
    table->Release(3, proper);
    table->Release(2, writer);
    table->Release(1, ddl);
}

// ---- `cores = 1` ---------------------------------------------------------

TEST(LockTableTest, AtOneCoreNoLatchIsConstructed) {
    // AO-R9: what compiles out at one core is the **synchronization**,
    // never the wait. The table exists, the conflict is still answered, and
    // no `std::mutex` is built.
    auto one = MakeTable(1);
    EXPECT_FALSE(one->latched());
    EXPECT_EQ(one->partition_count(), kLockPartitionsPerCore);

    auto eight = MakeTable(8);
    EXPECT_TRUE(eight->latched());
    EXPECT_EQ(eight->partition_count(), kLockPartitionsPerCore * 8);

    // The conflict is answered at one core exactly as at eight - finding
    // AO-3 H: two sessions on core 0 conflict today, so a borrow model that
    // no-opped at one core would keep the refusal axis 1 exists to remove.
    LockHoldings h1;
    LockHoldings h2;
    const LockKey key = LockKey::Tuple(kRel, 3);
    ASSERT_TRUE(one->Acquire(1, key, LockMode::kExclusive, h1).value().granted);
    auto blocked = one->Acquire(2, key, LockMode::kExclusive, h2);
    ASSERT_TRUE(blocked.ok());
    EXPECT_FALSE(blocked.value().granted);
    EXPECT_EQ(blocked.value().blocking_txn, 1u);
}

// ---- The cap -------------------------------------------------------------

TEST(LockTableTest, TheCapRefusesTheEntryPastItAndNeverTruncates) {
    // AO-R10 at a small cap, so the cell is a test and not a benchmark; the
    // shipped value is `kMaxLocksPerTxnDefault` and asserted below.
    constexpr std::size_t kCap = 64;
    auto table = MakeTable(8, kCap);
    LockHoldings h;

    for (std::uint64_t pk = 0; pk < kCap; ++pk) {
        auto r = table->Acquire(1, LockKey::Tuple(kRel, pk), LockMode::kExclusive, h);
        ASSERT_TRUE(r.ok()) << "at " << pk;
        ASSERT_TRUE(r.value().granted);
    }
    EXPECT_EQ(h.size(), kCap);

    auto over = table->Acquire(1, LockKey::Tuple(kRel, kCap), LockMode::kExclusive, h);
    ASSERT_FALSE(over.ok());
    EXPECT_EQ(over.status().code(), StatusCode::kResourceExhausted);
    EXPECT_FALSE(IsRetryable(over.status().code()))
        << "a retry meets the same cap, so the wire's retryable bit must not invite one";

    // Never truncates: the borrows it already held are all still held.
    EXPECT_EQ(h.size(), kCap) << "the refusal added nothing and removed nothing";
    EXPECT_EQ(table->EntryCount(), kCap);

    EXPECT_EQ(kMaxLocksPerTxnDefault, 65536u) << "AO-R10's value; E2 is where it may move";
    auto zero = LockTable::Create(8, 0);
    EXPECT_FALSE(zero.ok()) << "a cap of zero is a disabled lock family, not a cap";
}

TEST(LockTableTest, TheCapsRefusalCarriesItsOwnWireDetail) {
    // `kwp_error_test` owns the golden numbering; this asserts the engine
    // side of the same fact - the detail exists, is `ResourceExhausted`'s,
    // and is not one of the three that were there before (protocol.md §11's
    // details are append-only).
    EXPECT_EQ(static_cast<std::uint16_t>(wire::ResourceDetail::kLockCap), 4);
    EXPECT_NE(wire::ResourceDetail::kLockCap, wire::ResourceDetail::kStatementLimit);
    EXPECT_NE(wire::ResourceDetail::kLockCap, wire::ResourceDetail::kPortalLimit);
    EXPECT_NE(wire::ResourceDetail::kLockCap, wire::ResourceDetail::kPortalIdleTimeout);
}

// ---- The slice is keyed in key space -------------------------------------

TEST(LockTableTest, ASliceIsFoundAfterThePageItCameFromChangesItsBounds) {
    // E4, ratified: a slice is `(rel_oid, [lo, hi))` in **key space**. The
    // structural half of that claim is what makes it survive a btree leaf
    // division - `LockKey` has no page field, so a division has no identity
    // to invalidate - and the behavioural half is that the fence answers at
    // every key it covers and no other.
    auto table = MakeTable();
    LockHoldings fence_holder;
    const LockKey fence = LockKey::Slice(kRel, 100, 200);

    ASSERT_TRUE(table->Acquire(1, fence, LockMode::kShared, fence_holder).value().granted);
    EXPECT_EQ(table->RelationFenceCount(kRel), 1u);

    // A division would move keys between pages here. Nothing in `fence`
    // named a page, so there is nothing to update and nothing to lose.
    EXPECT_EQ(fence, LockKey::Slice(kRel, 100, 200)) << "identity is the bounds and nothing else";
    EXPECT_TRUE(table->FenceCoversKey(kRel, 100, /*txn=*/2));
    EXPECT_TRUE(table->FenceCoversKey(kRel, 150, 2));
    EXPECT_TRUE(table->FenceCoversKey(kRel, 199, 2));
    EXPECT_FALSE(table->FenceCoversKey(kRel, 200, 2)) << "the upper bound is exclusive";
    EXPECT_FALSE(table->FenceCoversKey(kRel, 99, 2));
    EXPECT_FALSE(table->FenceCoversKey(kOtherRel, 150, 2)) << "a fence is one relation's";

    // The holder's own fence does not block the holder.
    EXPECT_FALSE(table->FenceCoversKey(kRel, 150, /*txn=*/1));

    table->Release(1, fence_holder);
    EXPECT_EQ(table->RelationFenceCount(kRel), 0u);
    EXPECT_FALSE(table->FenceCoversKey(kRel, 150, 2));
}

TEST(LockTableTest, TheFenceCounterIsTheProbesGateAndARelationLockIsNotAFence) {
    // AO-R3's fast path in place of a persisted lock bit: a writer reads
    // this counter on the `IX` it takes anyway and probes only when it is
    // nonzero. A relation-level `S` is deliberately not counted - it
    // conflicts with the writer's `IX` at the relation entry itself, so the
    // writer never reaches the probe.
    auto table = MakeTable();
    LockHoldings rel_s;
    ASSERT_TRUE(table->Acquire(1, LockKey::Relation(kRel), LockMode::kShared, rel_s).value().granted);
    EXPECT_EQ(table->RelationFenceCount(kRel), 0u)
        << "a relation `S` is caught by the intention rule, not by the probe";

    LockHoldings writer;
    auto ix = table->Acquire(2, LockKey::Relation(kRel), LockMode::kIntentionExclusive, writer);
    ASSERT_TRUE(ix.ok());
    EXPECT_FALSE(ix.value().granted) << "IX against S at the relation is one compatibility check";

    // A refused acquire raises no counter it does not hold.
    LockHoldings slice_waiter;
    LockHoldings slice_holder;
    const LockKey slice = LockKey::Slice(kOtherRel, 0, 50);
    ASSERT_TRUE(table->Acquire(3, slice, LockMode::kShared, slice_holder).value().granted);
    EXPECT_EQ(table->RelationFenceCount(kOtherRel), 1u);
    auto refused = table->Acquire(4, slice, LockMode::kExclusive, slice_waiter);
    ASSERT_TRUE(refused.ok());
    EXPECT_FALSE(refused.value().granted);
    EXPECT_EQ(table->RelationFenceCount(kOtherRel), 1u)
        << "the refused `X` is not a fence and must not have raised the counter";
}

TEST(LockTableTest, ARefusedTransactionTakesItsQueuedRequestWithIt) {
    // The queue must be unwindable, or the entry outlives every holder and
    // `WaiterCount` - AO-R3's gate on whether a decide probes at all -
    // stays nonzero for the life of the instance. A transaction is one
    // thread of control, so it is queued on at most one unit and one record
    // on the requester is enough to withdraw it.
    auto table = MakeTable();
    const LockKey key = LockKey::Tuple(kRel, 11);
    LockHoldings holder;
    LockHoldings refused;

    ASSERT_TRUE(table->Acquire(1, key, LockMode::kExclusive, holder).value().granted);
    auto blocked = table->Acquire(2, key, LockMode::kExclusive, refused);
    ASSERT_TRUE(blocked.ok());
    ASSERT_FALSE(blocked.value().granted);
    EXPECT_EQ(table->WaiterCount(key), 1u);
    EXPECT_TRUE(refused.waiting_on().has_value());
    EXPECT_TRUE(refused.empty()) << "a refused request holds nothing";

    // The refused transaction aborts - which at S1 is what always happens,
    // since nothing parks. Its request goes with it.
    table->Release(2, refused);
    EXPECT_EQ(table->WaiterCount(key), 0u);
    EXPECT_FALSE(refused.waiting_on().has_value());

    table->Release(1, holder);
    EXPECT_EQ(table->EntryCount(), 0u)
        << "no holder, no waiter, no entry - a dead waiter would have pinned this forever";
}

TEST(LockTableTest, ACappedTransactionIsRefusedRatherThanQueued) {
    // The cap is checked before the conflict, so a transaction at its cap
    // meets the refusal immediately. Queuing it would make it wait for a
    // grant this same check refuses on arrival - a wait ending in the
    // refusal it was already owed.
    constexpr std::size_t kCap = 2;
    auto table = MakeTable(8, kCap);
    LockHoldings capped;
    LockHoldings other;

    ASSERT_TRUE(
        table->Acquire(1, LockKey::Tuple(kRel, 1), LockMode::kExclusive, capped).value().granted);
    ASSERT_TRUE(
        table->Acquire(1, LockKey::Tuple(kRel, 2), LockMode::kExclusive, capped).value().granted);
    const LockKey contended = LockKey::Tuple(kRel, 3);
    ASSERT_TRUE(table->Acquire(9, contended, LockMode::kExclusive, other).value().granted);

    auto over = table->Acquire(1, contended, LockMode::kExclusive, capped);
    ASSERT_FALSE(over.ok()) << "the cap answers before the conflict does";
    EXPECT_EQ(over.status().code(), StatusCode::kResourceExhausted);
    EXPECT_EQ(table->WaiterCount(contended), 0u) << "and nothing was queued behind it";
    EXPECT_FALSE(capped.waiting_on().has_value());

    // A borrow it already covers still succeeds at the cap: a re-borrow
    // spends nothing, so the covering test runs first.
    auto repeat = table->Acquire(1, LockKey::Tuple(kRel, 1), LockMode::kExclusive, capped);
    ASSERT_TRUE(repeat.ok());
    EXPECT_TRUE(repeat.value().already_held);

    table->Release(1, capped);
    table->Release(9, other);
}

TEST(LockTableTest, AFenceIsSharedOrExclusiveAndOnlyOverAKeyInterval) {
    // AR2 §2 defines a gap lock as "an `S` **or `X`** fence on a slice", and
    // AR2 §3's assertion row takes `X` on the slice when the writer changes
    // the group's state. The probe matches both: a gate that saw only `S`
    // would miss half the fences AR2 defines.
    auto table = MakeTable();
    LockHoldings x_fence;
    ASSERT_TRUE(table->Acquire(1, LockKey::Slice(kRel, 10, 20), LockMode::kExclusive, x_fence)
                    .value()
                    .granted);
    EXPECT_EQ(table->RelationFenceCount(kRel), 1u);
    EXPECT_TRUE(table->FenceCoversKey(kRel, 15, /*txn=*/2));

    // A range is a fence unit too - a mover borrows one (AR2-R13).
    LockHoldings range_fence;
    ASSERT_TRUE(
        table->Acquire(3, LockKey::Range(kOtherRel, 0, 64), LockMode::kExclusive, range_fence)
            .value()
            .granted);
    EXPECT_TRUE(table->FenceCoversKey(kOtherRel, 30, 4));

    // A tuple `X` is not a fence: its conflict is found by looking its own
    // key up, which is the ordinary row lock and not a probe.
    LockHoldings row;
    ASSERT_TRUE(table->Acquire(5, LockKey::Tuple(kOtherRel, 500), LockMode::kExclusive, row)
                    .value()
                    .granted);
    EXPECT_FALSE(table->FenceCoversKey(kOtherRel, 500, 6))
        << "a row lock is not a gap lock; a probe reporting it would refuse writers the tuple key "
           "itself is responsible for";

    table->Release(1, x_fence);
    table->Release(3, range_fence);
    table->Release(5, row);
    EXPECT_EQ(table->RelationFenceCount(kRel), 0u);
}

TEST(LockTableTest, AReleaseThatRemovesNoHolderLeavesTheFenceCounterAlone) {
    // The counter is lowered only when a holder was actually removed. An
    // unconditional decrement underflows to 2^64-1 on a double release, and
    // a permanently nonzero gate sends every writer on the relation through
    // the all-partition scan for the life of the instance - the defect this
    // stage's review found and fixed.
    auto table = MakeTable();
    LockHoldings h;
    const LockKey fence = LockKey::Slice(kRel, 0, 10);
    ASSERT_TRUE(table->Acquire(1, fence, LockMode::kShared, h).value().granted);
    EXPECT_EQ(table->RelationFenceCount(kRel), 1u);

    table->Release(1, h);
    EXPECT_EQ(table->RelationFenceCount(kRel), 0u);

    // Releasing a transaction that holds nothing is a no-op, not an
    // underflow. `LockHoldings` is move-only, so the two-owners route the
    // review used to provoke this is closed at compile time; the arithmetic
    // is pinned anyway.
    table->Release(1, h);
    EXPECT_EQ(table->RelationFenceCount(kRel), 0u);
    EXPECT_EQ(table->EntryCount(), 0u);
}

// ---- Partitions ----------------------------------------------------------

TEST(LockTableTest, DistinctPartitionsCarryDistinctLatchesAndDisjointBorrowsAllGrant) {
    // Two claims, and the cell is named for both because neither alone is
    // "never serialize". **Structural**: two keys in different partitions
    // are serialized by different `Latch` objects, and two threads holding
    // different mutexes cannot wait on each other - that is
    // non-serialization, and the only form of it a unit test can establish
    // (a timing measurement is AO-S7's). **Behavioural**: eight threads
    // taking disjoint borrows are all granted and give everything back,
    // which is a data-race check rather than a concurrency one.
    auto table = MakeTable(8);
    const LockKey a = LockKey::Tuple(kRel, 1);
    LockKey b = LockKey::Tuple(kRel, 2);
    for (std::uint64_t pk = 2; pk < 4096; ++pk) {
        b = LockKey::Tuple(kRel, pk);
        if (table->PartitionOf(b) != table->PartitionOf(a)) break;
    }
    ASSERT_NE(table->PartitionOf(a), table->PartitionOf(b));
    ASSERT_NE(table->LatchOf(a), nullptr) << "at eight cores the partitions are latched";
    EXPECT_NE(table->LatchOf(a), table->LatchOf(b))
        << "different partitions, different mutexes: neither thread can block the other";
    EXPECT_EQ(table->LatchOf(a), table->LatchOf(LockKey::Tuple(kRel, 1)))
        << "and one partition is one mutex, so a key always serializes with itself";

    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;
    std::atomic<int> granted{0};
    std::atomic<int> refused{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            LockHoldings h;
            for (int i = 0; i < kPerThread; ++i) {
                // Disjoint by construction: thread t owns every key with
                // `pk % kThreads == t`, so no two threads ever meet on a
                // unit and every acquire must be granted.
                const std::uint64_t pk =
                    static_cast<std::uint64_t>(i) * kThreads + static_cast<std::uint64_t>(t);
                auto r = table->Acquire(static_cast<std::uint64_t>(t) + 1,
                                        LockKey::Tuple(kRel, pk), LockMode::kExclusive, h);
                if (r.ok() && r.value().granted) {
                    granted.fetch_add(1, std::memory_order_relaxed);
                } else {
                    refused.fetch_add(1, std::memory_order_relaxed);
                }
            }
            table->Release(static_cast<std::uint64_t>(t) + 1, h);
        });
    }
    for (std::thread& th : threads) th.join();

    EXPECT_EQ(granted.load(), kThreads * kPerThread);
    EXPECT_EQ(refused.load(), 0);
    EXPECT_EQ(table->EntryCount(), 0u) << "every thread released everything it took";
}

TEST(LockTableTest, ContendedThreadsOnOneUnitProduceExactlyOneHolder) {
    // The other half of the same property: when eight threads *do* meet on
    // one unit, the table admits exactly one and names it to the other
    // seven. Without the partition latch this is where a lost update would
    // appear as two granted exclusives.
    auto table = MakeTable(8);
    const LockKey key = LockKey::Tuple(kRel, 77);
    constexpr int kThreads = 8;

    std::atomic<int> granted{0};
    std::atomic<int> blocked{0};
    std::vector<std::thread> threads;
    std::vector<LockHoldings> holdings(kThreads);
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            auto r = table->Acquire(static_cast<std::uint64_t>(t) + 1, key, LockMode::kExclusive,
                                    holdings[static_cast<std::size_t>(t)]);
            ASSERT_TRUE(r.ok());
            if (r.value().granted) {
                granted.fetch_add(1, std::memory_order_relaxed);
            } else {
                EXPECT_NE(r.value().blocking_txn, 0u);
                blocked.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& th : threads) th.join();

    EXPECT_EQ(granted.load(), 1) << "an exclusive borrow admits exactly one holder";
    EXPECT_EQ(blocked.load(), kThreads - 1);
    EXPECT_EQ(table->WaiterCount(key), static_cast<std::size_t>(kThreads - 1))
        << "every refused request is queued, which is what a decide reads before it probes";
}

}  // namespace
}  // namespace kds::txn
