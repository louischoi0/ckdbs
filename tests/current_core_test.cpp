#include "kds/base/current_core.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

// AM-S2 step 3's identity (`include/kds/base/current_core.hpp`).
//
// **The thing under test is a premise, not a mechanism.** A thread-local
// holding a number is not interesting; what is interesting is that the
// number is *exact* - that one thread declaring itself cannot change what
// another thread reads, and that a scope acting as another core cannot leak
// that identity into the rest of its thread. Both are what let the page
// latch's owner field mean "the core that asked" once one store serves every
// core, and neither is visible from the store's own tests.

namespace kds {
namespace {

TEST(CurrentCoreTest, AThreadThatNeverDeclaredItselfIsCoreZero) {
    // The default is the startup and mount thread's honest answer: it does
    // its work before any peer worker exists, so it *is* core 0's.
    EXPECT_EQ(CurrentCore(), 0u);
}

TEST(CurrentCoreTest, EachThreadSeesOnlyItsOwnDeclaration) {
    // **The property the shared pool rests on.** Eight threads declare eight
    // different cores and each must read back its own - if this were a plain
    // global, the page latch's owner field would record whichever core
    // declared last and the re-entrancy rule would let one core release
    // another's exclusive hold.
    constexpr std::uint32_t kThreads = 8;
    std::atomic<std::uint32_t> mismatches{0};
    std::atomic<std::uint32_t> arrived{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::uint32_t core = 0; core < kThreads; ++core) {
        threads.emplace_back([core, &mismatches, &arrived] {
            SetCurrentCore(core);
            // Every thread declares before any thread reads, so a read that
            // returns another core's number is a genuine crossing rather
            // than a race with a thread that had not started yet.
            arrived.fetch_add(1, std::memory_order_acq_rel);
            while (arrived.load(std::memory_order_acquire) < kThreads) {
                std::this_thread::yield();
            }
            for (int round = 0; round < 1000; ++round) {
                if (CurrentCore() != core) mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& t : threads) t.join();
    EXPECT_EQ(mismatches.load(), 0u) << "a thread read a core id another thread declared";
    // And the joining thread is untouched by all of it.
    EXPECT_EQ(CurrentCore(), 0u);
}

TEST(CurrentCoreTest, AGuardRestoresTheIdentityItFound) {
    // The mount pass uses this: `CoreRuntime::Open` runs on the startup
    // thread and acts as the core it is opening, then must hand that thread
    // back unchanged for the next core it opens.
    SetCurrentCore(3);
    {
        CurrentCoreGuard as_seven(7);
        EXPECT_EQ(CurrentCore(), 7u);
        {
            CurrentCoreGuard as_one(1);
            EXPECT_EQ(CurrentCore(), 1u);
        }
        EXPECT_EQ(CurrentCore(), 7u) << "an inner scope leaked its identity outward";
    }
    EXPECT_EQ(CurrentCore(), 3u) << "the guard restored something other than what it found";
    SetCurrentCore(0);
}

}  // namespace
}  // namespace kds
