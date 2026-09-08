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
// Concurrency: **one instance serves every core's store** - `Expeditor`
// hands the same reference to each `CoreRuntime::Open` - and it is on
// `rules.md` section 3's declared shared list. **Any core grows it**, and
// an implementation owes the synchronisation that takes (the operator's
// decision of 2026-09-08, closing
// `device-growth-is-not-core-0s-any-more.md`): a peer allocating through
// the instance's free map reaches `EnsureCapacity` (since `2663001`, and
// from two more paths since AW-S1b), so `page_capacity()` is an atomic
// load of a bound that only rises - a reader with a stale value under-reads
// the bound and faults nothing it should not - and the grow itself is
// serialised inside the device, so two cores running off the end at once
// produce one grow. How each implementation does that is its own
// header's to state. Growth was core 0's alone while each core had its
// own store and its own extent lease; this paragraph said so until
// 2026-09-08, and "owned by one core" before the AL-S9 review.
//
// **That argument covers `page_capacity_` and nothing else** (AM-S2 R8).
// It is about a monotonically rising bound read racily; it says nothing
// about whether two threads may be *inside* an operation at once. They may,
// and already are: one device serves every core's store, so two cores
// faulting different pages are both inside `ReadPage` - which
// `core_runtime_test.cpp` has been doing over a shared `MemoryPageDevice`
// with a worker thread per core since before AM-S2. AM-S2's step 2b, which
// drops the frame table's latch before the read, adds a second route to the
// same place rather than the first.
//
// **The contract, stated rather than inferred**: `ReadPage`/`ReadPageRun`
// may be called concurrently, including with each other, and an
// implementation owes internal synchronisation for whatever state they
// touch. `MemoryPageDevice` did not - its trace `push_back` reallocates a
// vector under a concurrent reader - and now takes a latch over its whole
// mutating surface. `FilePageDevice` reads through `pread`, which is
// thread-safe for distinct offsets, and holds no per-read state - **read
// rather than assumed** (AM-R11, 2026-09-06): every transfer is a `pread`
// or `pwrite` at an offset computed from the page id
// (`file_page_device.cpp`'s `ReadAt` and `WriteAt`), never `lseek` plus
// `read`, and the loop's `offset`, `buffer` and `remaining` are locals. It
// needed no change; its own header's "core-local" claim did.
// **Growth is the device's own to serialise** since 2026-09-08, per the
// paragraph above; write and sync remain the caller's, and nothing here
// promises they are safe against a concurrent read.

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
