#pragma once

#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"

// The block-device seam: "move these 8 KiB between a frame and stable
// storage". This is the layer *below* the buffer pool, and the boundary
// rules.md section 4 requires all file/disk I/O to cross through, so the
// whole engine can run under deterministic simulation with fault injection
// (see MemoryPageDevice for the simulated implementation).
//
// Deliberately not modelled here: which page ids exist or are free (that
// is the SpaceManager, page.md section 5), caching, pinning, dirty
// tracking, checksums, and the WAL flush gate (all buffer-pool concerns,
// page.md sections 6-10). A PageDevice is pure addressed I/O over a flat
// page-indexed space.
//
// Synchronous for now. page.md section 12 puts the real miss path on an
// async submission through an I/O backend whose flavour (plain O_DIRECT vs
// io_uring vs pluggable) is still an open decision in CLAUDE.md, and the
// coroutine/future machinery it needs does not exist yet. Every operation
// here is shaped so an async variant can be added beside it without moving
// the boundary: the caller always owns the buffer, the buffer always
// outlives the call, and nothing returns a span into device-owned memory.
//
// The run forms exist because docs/spec/page.md section 13 names them as the
// concrete payoff of the single-file layout (S5): page-id order is
// literally file order, so an id-sorted batch of adjacent pages is one
// sequential transfer. The default implementations just loop, so an
// implementation only overrides them when it can actually do better.
//
// Concurrency: a PageDevice instance is owned by one core, like everything
// else (rules.md section 3). Nothing here is internally synchronized.

namespace kds::storage {

// Proposed default growth unit, from page.md section 5: 64 pages = 512 KiB
// per extent. Marked [OPEN: size] there, so this is a default that
// implementations take as a parameter - not a constant anything may
// depend on.
inline constexpr std::uint32_t kDefaultExtentPages = 64;

class PageDevice {
public:
    virtual ~PageDevice() = default;

    // How many pages of backing space currently exist. Page ids in
    // [0, page_capacity()) are addressable; ids at or beyond it are not,
    // until EnsureCapacity() grows the device. This is allocated space,
    // not used space - a page inside the capacity that has never been
    // written reads back as zeroes.
    virtual std::uint32_t page_capacity() const noexcept = 0;

    // Reads page `page_id` into `out`. Fails with OutOfRange if page_id is
    // at or beyond page_capacity(), IoError on a device failure.
    virtual Status ReadPage(PageId page_id, std::span<std::byte, kPageSize> out) = 0;

    // Writes `in` to page `page_id`. Not durable until Sync(). Same
    // failure modes as ReadPage().
    virtual Status WritePage(PageId page_id, std::span<const std::byte, kPageSize> in) = 0;

    // Reads/writes `nr_pages` consecutive pages starting at
    // `first_page_id`. The buffer must be exactly nr_pages * kPageSize
    // bytes; InvalidArgument otherwise. A partial failure leaves an
    // unspecified prefix transferred - callers treat a failed run as "the
    // whole run is unknown" and retry or fail out, which is what both the
    // prefetch path and the background writer do anyway.
    virtual Status ReadPageRun(PageId first_page_id, std::uint32_t nr_pages,
                               std::span<std::byte> out);
    virtual Status WritePageRun(PageId first_page_id, std::uint32_t nr_pages,
                                std::span<const std::byte> in);

    // Grows backing space so page_capacity() >= nr_pages. Never shrinks
    // (truncation is a recorded non-goal for v1, page.md section 14), so
    // calling it with a value at or below the current capacity succeeds and
    // does nothing. Fails with InvalidArgument above the kMaxPageCount
    // design ceiling, OutOfSpace if the underlying store cannot reserve the
    // blocks.
    //
    // Crash-safe ordering is the caller's (page.md section 14): the ALLOC
    // WAL record goes first, then this, then first use. Extension is
    // idempotent, which is what makes replay of that order safe.
    virtual Status EnsureCapacity(std::uint32_t nr_pages) = 0;

    // Makes every previously written page - and the device's own size
    // metadata after a growth - durable.
    virtual Status Sync() = 0;
};

// ---- Shared argument validation ----------------------------------------
//
// Every implementation owes callers the same answers for the same bad
// arguments, so the checks live here rather than being reimplemented (and
// drifting) per device.

// Rejects an empty run, and one that runs past `capacity`. The addition is
// done widened: first_page_id + nr_pages can overflow u32 near the top of
// the id space, and a wrapped range would silently address low pages.
// Since no implementation may report a capacity above kMaxPageCount, this
// enforces the design ceiling too.
Status CheckPageRunRange(PageId first_page_id, std::uint32_t nr_pages, std::uint32_t capacity);

// Rejects a buffer that is not exactly nr_pages * kPageSize bytes.
Status CheckPageRunBuffer(std::uint32_t nr_pages, std::size_t buffer_bytes);

}  // namespace kds::storage
