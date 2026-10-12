#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

#include "kds/base/common.hpp"

// The backup copier's fence over a page device's writes (BR, `br-s0-census.md`
// §2, BR-R5 replaced): **while armed, a write and the copier's read of an
// overlapping page range never run at once**, so no page the copier reads
// is torn by the engine's own `pwrite` - headered, headerless, free map or
// superblock alike, because every data-file write goes through the device.
//
// **Disarmed - every instant no backup copies - a write costs one counter
// increment, one load and one decrement** (BR-R6). The arming is a Dekker
// pair: a writer increments `unfenced_` and then reads `armed_`; `Arm`
// stores `armed_` and then waits for `unfenced_` to drain. Both are
// sequentially consistent, so either the writer sees the fence armed and
// takes the fenced path, or `Arm` sees the writer and waits it out - no
// write that began unfenced is still running when the first fenced read
// starts.
//
// **One reader at a time** (BR-Q4: one backup), on its own thread; writers
// are any core's writeback, map flush or superblock persist. `mu_` is a leaf:
// nothing is acquired under it, and no caller holds a page latch across a
// fenced wait that a page latch holder would need - a writer waits only on
// the copier's `pread`, which waits on nothing.
namespace kds::storage {

class PageWriteFence {
public:
    // A write's passage through the fence, for its whole device transfer.
    class WriteScope {
    public:
        WriteScope(PageWriteFence& fence, PageId first, std::uint32_t pages);
        ~WriteScope();
        WriteScope(const WriteScope&) = delete;
        WriteScope& operator=(const WriteScope&) = delete;

    private:
        PageWriteFence& fence_;
        PageId first_;
        std::uint32_t pages_;
        bool fenced_;
    };

    // The copier's side. `Arm` returns once every unfenced write has ended.
    void Arm();
    void Disarm();
    bool armed() const noexcept { return armed_.load(std::memory_order_acquire); }

    // Brackets one read of `[first, first + pages)`: waits out every fenced
    // write overlapping it, and holds every new one off until `EndRead`.
    void BeginRead(PageId first, std::uint32_t pages);
    void EndRead();

    // How many writes waited on a read (`SHOW META`, and the cost BR-S10
    // measures).
    std::uint64_t writes_waited() const noexcept {
        return writes_waited_.load(std::memory_order_relaxed);
    }

private:
    struct Range {
        PageId first;
        std::uint32_t pages;
        bool Overlaps(PageId f, std::uint32_t n) const noexcept {
            return std::uint64_t{f} < std::uint64_t{first} + pages &&
                   std::uint64_t{first} < std::uint64_t{f} + n;
        }
    };

    std::atomic<bool> armed_{false};
    std::atomic<std::uint64_t> unfenced_{0};
    std::atomic<std::uint64_t> writes_waited_{0};

    std::mutex mu_;
    std::condition_variable changed_;
    std::vector<Range> writes_;  // fenced writes in flight
    bool reading_ = false;
    Range read_{0, 0};
};

}  // namespace kds::storage
