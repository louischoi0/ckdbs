#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "kds/storage/page_device.hpp"

// The simulated PageDevice: the same contract as FilePageDevice, backed by
// process memory, with the failure modes a real disk has and a real disk
// will not produce on demand.
//
// This exists because rules.md section 4 makes deterministic simulation a
// first-class constraint: the whole engine must be runnable single-threaded
// under a simulated scheduler with I/O errors and torn writes injected, and
// docs/spec/page.md's testing requirements (sections 18-5 through 18-8) are
// written against exactly that. Every guarantee in wal.md is proven here
// before it is trusted on a real file.
//
// Three things are modelled that a plain in-memory buffer would not:
//
//  1. **Durability.** A write is visible to a later read immediately, but
//     is not *durable* until Sync(). Crash() throws away everything not yet
//     synced, which is what a power loss does. Capacity growth is durable
//     on the same schedule, so a crash between EnsureCapacity() and Sync()
//     correctly loses the extension - the case page.md section 14's
//     ALLOC-before-extend ordering exists to survive.
//
//  2. **Torn writes.** TearNextWrite(n) transfers only the first n bytes of
//     the next write and drops the rest, leaving the tail as whatever was
//     there before. This is what a checksum (page.md section 10) is for,
//     and what a WAL full-page image heals.
//
//  3. **I/O errors.** FailNext*(status) makes the next operation report a
//     given failure, so error paths are reachable in tests instead of
//     merely written.
//
// Storage is sparse: a page inside the capacity that was never written
// costs nothing and reads back as zeroes, matching a sparse file.
//
// The operation trace records one entry per call - including run
// transfers as a single entry, which is what makes it usable to assert
// that the background writer really did coalesce an id-sorted batch into
// one transfer (page.md section 18-7).
//
// Concurrency: core-local, no synchronization, like every PageDevice.

namespace kds::storage {

class MemoryPageDevice final : public PageDevice {
public:
    enum class OpKind : std::uint8_t {
        kRead,
        kWrite,
        kSync,
        kGrow,
    };

    // One recorded device operation. For kSync, first_page_id/nr_pages are
    // 0; for kGrow, nr_pages is the new capacity.
    struct TraceEntry {
        OpKind kind;
        PageId first_page_id;
        std::uint32_t nr_pages;
    };

    struct Stats {
        std::uint64_t reads = 0;          // ReadPage/ReadPageRun calls
        std::uint64_t writes = 0;         // WritePage/WritePageRun calls
        std::uint64_t syncs = 0;
        std::uint64_t grows = 0;          // EnsureCapacity calls that actually grew
        std::uint64_t pages_read = 0;
        std::uint64_t pages_written = 0;
        // One-shot injections that actually fired. A driver that arms a
        // fault cannot otherwise tell whether it was consumed, and
        // "this error had an injected cause" is exactly what the
        // simulation harness's fault runs assert (bench/workplan-
        // teststrategy SIM05).
        std::uint64_t injections_fired = 0;
    };

    // `extent_pages` must be non-zero, same as FilePageDevice; the factory
    // exists because that check makes construction fallible (rules.md
    // section 1). `initial_pages` is a convenience for tests that do not
    // care about growth: it is rounded up and made durable immediately, as
    // if the device had been opened on an already-sized file.
    static StatusOr<std::unique_ptr<MemoryPageDevice>> Create(
        std::uint32_t extent_pages = kDefaultExtentPages, std::uint32_t initial_pages = 0);

    std::uint32_t page_capacity() const noexcept override { return page_capacity_; }
    std::uint32_t extent_pages() const noexcept { return extent_pages_; }

    // Capacity as of the last Sync() - what Crash() would revert to.
    std::uint32_t durable_page_capacity() const noexcept { return durable_page_capacity_; }

    Status ReadPage(PageId page_id, std::span<std::byte, kPageSize> out) override;
    Status WritePage(PageId page_id, std::span<const std::byte, kPageSize> in) override;

    // Overridden so a run is one device operation, not nr_pages of them -
    // matching FilePageDevice's single pread/pwrite. That makes the trace
    // meaningful and makes an injected tear or failure apply to the run as
    // a whole, the way a partial physical transfer would.
    Status ReadPageRun(PageId first_page_id, std::uint32_t nr_pages,
                       std::span<std::byte> out) override;
    Status WritePageRun(PageId first_page_id, std::uint32_t nr_pages,
                        std::span<const std::byte> in) override;

    Status EnsureCapacity(std::uint32_t nr_pages) override;
    Status Sync() override;

    // ---- Fault injection -------------------------------------------------

    // The next matching operation reports `status` and has no effect. Each
    // is one-shot; setting one twice replaces the pending injection.
    void FailNextRead(Status status);
    void FailNextWrite(Status status);
    void FailNextSync(Status status);
    void FailNextGrow(Status status);

    // The next write transfers only its first `prefix_bytes` bytes and then
    // stops, as an interrupted physical write would. The call still
    // reports OK - a torn write is not an error the device knows about,
    // which is the entire reason checksums exist. `prefix_bytes` is clamped
    // to the transfer size; 0 means the write is lost entirely.
    void TearNextWrite(std::size_t prefix_bytes);

    // Disarms every pending injection. For a driver that injects during
    // one phase and needs the next phase clean: a one-shot fault that was
    // never consumed would otherwise fire into the phase that must not
    // have faults in it (SIM05's quiescence probe).
    void ClearInjections() noexcept;

    // Discards every write and every capacity growth since the last
    // Sync(), modelling power loss. Injections and the trace are left
    // alone; call ClearTrace() too if the trace should start fresh.
    void Crash();

    // ---- Instrumentation -------------------------------------------------

    const Stats& stats() const noexcept { return stats_; }
    const std::vector<TraceEntry>& trace() const noexcept { return trace_; }
    void ClearTrace() noexcept { trace_.clear(); }
    void ResetStats() noexcept { stats_ = Stats{}; }

private:
    using Page = std::array<std::byte, kPageSize>;

    MemoryPageDevice(std::uint32_t extent_pages, std::uint32_t page_capacity) noexcept;

    // Current contents of `page_id`: the pending write if there is one,
    // else the durable image, else zeroes. Never null.
    const Page& CurrentPage(PageId page_id) const;

    // Rounds `nr_pages` up to a whole extent, clamped at kMaxPageCount.
    std::uint32_t RoundUpToExtent(std::uint32_t nr_pages) const noexcept;

    std::uint32_t extent_pages_;
    std::uint32_t page_capacity_;
    std::uint32_t durable_page_capacity_;

    // Sparse, and split so Crash() can drop exactly the un-synced half.
    // Reads see pending_ overlaid on durable_.
    std::unordered_map<PageId, Page> durable_;
    std::unordered_map<PageId, Page> pending_;

    std::optional<Status> fail_next_read_;
    std::optional<Status> fail_next_write_;
    std::optional<Status> fail_next_sync_;
    std::optional<Status> fail_next_grow_;
    std::optional<std::size_t> tear_next_write_;

    Stats stats_;
    std::vector<TraceEntry> trace_;
};

}  // namespace kds::storage
