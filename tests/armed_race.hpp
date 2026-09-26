#pragma once

// The two pieces every real-thread race cell over a shared store needs
// (`btree_race_test.cpp`, `assertion_race_test.cpp`,
// `catalog_name_race_test.cpp`), in one place so a fourth cell does not
// grow a fourth copy.

#include <atomic>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"

namespace kds::testing_race {

// A store shared the way an instance's is: **armed**, because the page latch
// queues a second core only where the store is shared, and an unarmed latch
// never queues the second writer - so without it no window a race cell is
// about can open at all.
inline std::unique_ptr<storage::DevicePageStore> ArmedStore(
    std::unique_ptr<storage::MemoryPageDevice>& device, PageId first_new_page_id = 16) {
    auto made = storage::MemoryPageDevice::Create(/*extent_pages=*/512, /*initial_pages=*/0);
    EXPECT_TRUE(made.ok()) << made.status().message();
    device = std::move(made.value());
    auto store = storage::DevicePageStore::Open(*device, first_new_page_id);
    EXPECT_TRUE(store.ok()) << store.status().message();
    store.value()->SetLatchArmed(true, /*concurrent_pinners=*/16);
    return std::move(store.value());
}

// **The rendezvous, and why a race cell is worth nothing without it.**
// Measured in `btree_race_test.cpp`: two threads merely started together
// drift apart after a few calls, and once they are in different steps
// neither is queued on the other and the window never opens - the mutant
// there survived three runs in six. Re-synchronising before every step is
// what makes the window reliable.
//
// A generation counter rather than a count alone: a thread that leaves the
// barrier and arrives at the next one before its partner has left the first
// would otherwise be counted into the wrong round.
class Rendezvous {
  public:
    explicit Rendezvous(int parties) noexcept : parties_(parties) {}

    void Wait() noexcept {
        const int generation = generation_.load(std::memory_order_acquire);
        if (waiting_.fetch_add(1, std::memory_order_acq_rel) + 1 == parties_) {
            waiting_.store(0, std::memory_order_release);
            generation_.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        while (generation_.load(std::memory_order_acquire) == generation) {
            std::this_thread::yield();
        }
    }

  private:
    const int parties_;
    std::atomic<int> waiting_{0};
    std::atomic<int> generation_{0};
};

}  // namespace kds::testing_race
