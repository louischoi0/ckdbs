// AU-S2: the cross-core lock wake as write-then-kick, on the two-core rig
// (`instructions/v3.0.0/workorder-au-ring-retirement.md` AU-S2;
// `workorder-av-two-core-rig.md` §6 cells 4, 6 and 7, which are one shape at
// the table's level - the waiter on core 1, the decide on core 0, the kick
// between them).
//
// **What is under test is the table's own wake, not a statement's.** No
// production wait parks on a `LockWaitSlot` yet - the dispatcher's write
// wait polls its core's `IsInFlight`, which is AO-S5's cutover to make -
// so these cells drive `LockTable::Acquire`/`Release` from coroutines on
// the two reactors exactly as `lock_table_test.cpp`'s `LockWaitOnOneReactor`
// does on one, over a table the rig shares between them and a
// `SimWakerTable` that logs and holds the kick the decide sends.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include <gtest/gtest.h>

#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/txn/lock_table.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;
using txn::LockHoldings;
using txn::LockKey;
using txn::LockMode;
using txn::LockTable;

constexpr catalog::Oid kRel = 4018;

// One transaction's life as a coroutine on its core: take the borrow,
// parking if somebody holds it; wait to be told to decide; release. The
// predicates are members because `WaitUntil` holds a pointer across the
// park; the flags are atomic because the rig's thread flips `may_decide`
// and reads the rest.
struct Session {
    LockHoldings holdings;
    std::function<bool()> after_pred;
    std::function<bool()> pred;
    std::function<bool()> decide_pred;
    std::atomic<bool> granted{false};
    std::atomic<int> parks{0};
    std::atomic<bool> may_decide{false};
    std::atomic<bool> released{false};
};

// `after`, when given, is a barrier the task parks on before its first
// ask (AV-R3: a state a cell needs first is reached by a barrier, never by
// a race between two reactors' first polls). T2 waits for T1's grant, so
// T1 always holds and T2 always parks - the other order would put T2 in
// the holder's chair and every assertion below on the wrong session.
sched::Coro Borrow(LockTable& table, Session& s, std::uint64_t txn, LockKey key,
                   const std::atomic<bool>* after = nullptr) {
    if (after != nullptr) {
        s.after_pred = [after] { return after->load(std::memory_order_acquire); };
        co_await sched::WaitUntil{&s.after_pred};
    }
    for (;;) {
        auto r = table.Acquire(txn, key, LockMode::kExclusive, s.holdings);
        if (!r.ok()) co_return r.status();
        if (r.value().granted) break;
        s.parks.fetch_add(1, std::memory_order_relaxed);
        s.pred = [slot = r.value().slot] { return txn::LockWaitReady(slot); };
        co_await sched::WaitUntil{&s.pred};
        // The re-check is the loop going round again (AO-3's finding G).
    }
    s.granted.store(true, std::memory_order_release);
    s.decide_pred = [&s] { return s.may_decide.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&s.decide_pred};
    table.Release(txn, s.holdings);
    s.released.store(true, std::memory_order_release);
    co_return Status::OK();
}

struct LockWakeRig {
    explicit LockWakeRig(TwoCoreRig::Options options) {
        auto opened = TwoCoreRig::Open(options);
        EXPECT_TRUE(opened.ok()) << opened.status().message();
        if (!opened.ok()) return;
        rig = std::move(opened.value());
        // The instance's table (AO-S5), which the rig builds as `Expeditor`
        // does and whose registry is the sim over the real table - so the
        // decide's kick is logged and held like any other kick.
        table = &rig->locks();
    }

    // **The reactors are stopped before anything they run on dies.** A cell
    // that fails an `ASSERT` leaves this fixture early with both reactors
    // still polling coroutines that name `table`, `t1` and `t2` - members
    // declared before `rig` and so destroyed after it would have been, had
    // the rig gone first. The first version of this fixture segfaulted
    // exactly that way, on the eighth of ten repeats.
    ~LockWakeRig() {
        if (rig != nullptr) rig->Stop();
    }

    // T1 on core `holder`, T2 on core `waiter`, both submitted before the
    // reactors start; T2 asks only once T1 holds (`Borrow`'s barrier), and
    // core `waiter` is kicked until it has asked and parked, since T1's
    // grant on another core is a flip nothing kicks for. Returns whether
    // the cell reached its starting state: T1 holding, T2 parked.
    bool Submit(std::uint32_t holder, std::uint32_t waiter) {
        rig->core(holder).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, Borrow(*table, t1, 1, row)));
        rig->core(waiter).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, Borrow(*table, t2, 2, row, &t1.granted)));
        rig->Start();
        if (!Within(2000ms, [&] { return t1.granted.load(std::memory_order_acquire); })) {
            return false;
        }
        return KickUntil(*rig, waiter,
                         [&] { return t2.parks.load(std::memory_order_relaxed) >= 1; });
    }

    // The holder decides. Its reactor is parked too, so the cell wakes it
    // through the **real** table until it has released: that kick is the
    // cell's barrier, not the mechanism under test.
    bool Decide(std::uint32_t holder) {
        t1.may_decide.store(true, std::memory_order_release);
        return KickUntil(*rig, holder,
                         [&] { return t1.released.load(std::memory_order_acquire); });
    }

    std::unique_ptr<TwoCoreRig> rig;
    LockTable* table = nullptr;
    const LockKey row = LockKey::Tuple(kRel, 5);
    Session t1;
    Session t2;
};

TEST(LockWakeRigTest, TheWaiterOnCore1ProceedsAtTheKickRatherThanAtBlockExpiry) {
    // §6 cell 4 (AU-S2) and cell 6 (AO-S5), at the table's level: T2 on
    // core 1 is parked on T1's row and its reactor is asleep; T1 decides on
    // core 0; the decide's kick - held by the sim for two ticks - is what
    // ends core 1's block, and `sched_wakes_received` on core 1 moves.
    TwoCoreRig::Options options;
    options.wake.min_delay_ticks = 2;
    options.wake.max_delay_ticks = 2;
    LockWakeRig r(options);
    ASSERT_NE(r.rig, nullptr);
    ASSERT_TRUE(r.Submit(/*holder=*/0, /*waiter=*/1)) << "T1 holding and T2 parked was never reached";

    EXPECT_EQ(r.t1.parks.load(), 0);
    sched::Scheduler& peer = r.rig->core(1).scheduler();
    ASSERT_TRUE(Within(2000ms, [&] { return peer.idle_blocks() >= 1; }))
        << "core 1 never blocked with T2 parked";
    // The barrier kicks that parked T2 were written to core 1 too; what the
    // cell asserts is the change from here.
    const std::uint64_t received_before = peer.wakes_received();
    const std::size_t kicks_to_peer_before = KicksTo(*r.rig, 1).size();

    ASSERT_TRUE(r.Decide(/*holder=*/0)) << "the holder never released";
    // The decide flipped T2's slot and kicked core 1 - through the sim, so
    // the kick is in the log, addressed to core 1, and held.
    const std::vector<sched::SimWakerTable::Record> to_peer = KicksTo(*r.rig, 1);
    ASSERT_EQ(to_peer.size(), kicks_to_peer_before + 1) << "the decide did not kick core 1";
    EXPECT_EQ(to_peer.back().due, to_peer.back().tick + 2);
    EXPECT_EQ(peer.wakes_received(), received_before) << "a held kick reached core 1's eventfd";
    EXPECT_FALSE(r.t2.granted.load(std::memory_order_acquire))
        << "T2 proceeded before any kick reached its reactor";

    r.rig->wake().Advance(2);
    EXPECT_TRUE(Within(1000ms, [&] { return r.t2.granted.load(std::memory_order_acquire); }))
        << "T2 did not proceed at the kick";
    EXPECT_EQ(peer.wakes_received(), received_before + 1)
        << "the kick was skipped, so core 1 was not asleep";

    r.t2.may_decide.store(true, std::memory_order_release);
    r.rig->wakers().Kick(1);
    EXPECT_TRUE(Within(2000ms, [&] { return r.t2.released.load(std::memory_order_acquire); }));
    EXPECT_EQ(r.table->EntryCount(), 0u);
}

TEST(LockWakeRigTest, WithEveryKickDelayedToTheMaximumTheWaiterStillProceeds) {
    // §6 cell 7 (AO-S5). Every kick is held to the sim's maximum and the
    // cell never advances the tick, so the decide's kick never lands: T2
    // proceeds anyway, at its block's expiry - correct and slow, AR0-6-R1's
    // cost - and nothing was written to core 1's eventfd.
    TwoCoreRig::Options options;
    options.max_idle_block_ms = 200;
    options.wake.min_delay_ticks = 9;
    options.wake.max_delay_ticks = 9;
    LockWakeRig r(options);
    ASSERT_NE(r.rig, nullptr);
    ASSERT_TRUE(r.Submit(/*holder=*/0, /*waiter=*/1)) << "T1 holding and T2 parked was never reached";
    sched::Scheduler& peer = r.rig->core(1).scheduler();
    ASSERT_TRUE(Within(2000ms, [&] { return peer.idle_blocks() >= 1; }));
    // The barrier kicks that parked T2 were written to core 1 too; what the
    // cell asserts is the change from here.
    const std::uint64_t received_before = peer.wakes_received();

    ASSERT_TRUE(r.Decide(/*holder=*/0)) << "the holder never released";
    EXPECT_TRUE(Within(3000ms, [&] { return r.t2.granted.load(std::memory_order_acquire); }))
        << "T2 never proceeded: a held kick cost liveness rather than latency";
    EXPECT_EQ(peer.wakes_received(), received_before) << "something wrote core 1's eventfd";
    const std::vector<sched::SimWakerTable::Record> to_peer = KicksTo(*r.rig, 1);
    ASSERT_GE(to_peer.size(), 1u);
    EXPECT_GT(to_peer.back().due, r.rig->wake().tick()) << "the decide's kick was not held";

    r.t2.may_decide.store(true, std::memory_order_release);
    r.rig->wakers().Kick(1);
    EXPECT_TRUE(Within(2000ms, [&] { return r.t2.released.load(std::memory_order_acquire); }));
}

TEST(LockWakeRigTest, ASameCoreWaiterIsNeverKicked) {
    // The other half of the rule: a waiter on the decide's own core is
    // polled by the reactor running the decide, so a kick would be a
    // syscall for nothing. Both transactions on core 1; the decide flips
    // the slot and the log gains no kick to core 1.
    LockWakeRig r({});
    ASSERT_NE(r.rig, nullptr);
    ASSERT_TRUE(r.Submit(/*holder=*/1, /*waiter=*/1)) << "T1 holding and T2 parked was never reached";
    const std::size_t kicks_to_core1_before = KicksTo(*r.rig, 1).size();

    ASSERT_TRUE(r.Decide(/*holder=*/1)) << "the holder never released";
    EXPECT_TRUE(Within(2000ms, [&] { return r.t2.granted.load(std::memory_order_acquire); }))
        << "the same-core waiter was not woken by the flip alone";
    EXPECT_EQ(KicksTo(*r.rig, 1).size(), kicks_to_core1_before)
        << "the decide kicked the core it was running on";

    r.t2.may_decide.store(true, std::memory_order_release);
    r.rig->wakers().Kick(1);
    EXPECT_TRUE(Within(2000ms, [&] { return r.t2.released.load(std::memory_order_acquire); }));
}

}  // namespace
}  // namespace kds::server
