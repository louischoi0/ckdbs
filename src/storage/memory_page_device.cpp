#include "kds/storage/memory_page_device.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace kds::storage {
namespace {

// Returned for any page inside the capacity that has never been written -
// the sparse-file behaviour FilePageDevice gets from the filesystem.
const std::array<std::byte, kPageSize>& ZeroPage() {
    static const std::array<std::byte, kPageSize> kZeroes{};
    return kZeroes;
}

// Consumes a one-shot injection, if one is armed.
template <typename T>
std::optional<T> Take(std::optional<T>& slot) {
    if (!slot.has_value()) {
        return std::nullopt;
    }
    std::optional<T> taken = std::move(slot);
    slot.reset();
    return taken;
}

}  // namespace

MemoryPageDevice::MemoryPageDevice(std::uint32_t extent_pages,
                                   std::uint32_t page_capacity) noexcept
    : extent_pages_(extent_pages),
      page_capacity_(page_capacity),
      durable_page_capacity_(page_capacity) {}

StatusOr<std::unique_ptr<MemoryPageDevice>> MemoryPageDevice::Create(std::uint32_t extent_pages,
                                                                     std::uint32_t initial_pages) {
    if (extent_pages == 0) {
        return Status::InvalidArgument("MemoryPageDevice: extent_pages must be non-zero");
    }
    if (initial_pages > kMaxPageCount) {
        return Status::InvalidArgument("MemoryPageDevice: initial_pages " +
                                       std::to_string(initial_pages) + " exceeds the " +
                                       std::to_string(kMaxPageCount) + "-page design ceiling");
    }

    // Rounded the same way EnsureCapacity() would, so a device created with
    // an initial size is indistinguishable from one grown to it.
    const std::uint64_t rounded = initial_pages == 0
                                      ? 0
                                      : std::min<std::uint64_t>(
                                            (static_cast<std::uint64_t>(initial_pages) +
                                             extent_pages - 1) /
                                                extent_pages * extent_pages,
                                            kMaxPageCount);
    return std::unique_ptr<MemoryPageDevice>(
        new MemoryPageDevice(extent_pages, static_cast<std::uint32_t>(rounded)));
}

std::uint32_t MemoryPageDevice::RoundUpToExtent(std::uint32_t nr_pages) const noexcept {
    const std::uint64_t rounded =
        (static_cast<std::uint64_t>(nr_pages) + extent_pages_ - 1) / extent_pages_ * extent_pages_;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(rounded, kMaxPageCount));
}

const MemoryPageDevice::Page& MemoryPageDevice::CurrentPage(PageId page_id) const {
    if (auto it = pending_.find(page_id); it != pending_.end()) {
        return it->second;
    }
    if (auto it = durable_.find(page_id); it != durable_.end()) {
        return it->second;
    }
    return ZeroPage();
}

Status MemoryPageDevice::ReadPage(PageId page_id, std::span<std::byte, kPageSize> out) {
    return ReadPageRun(page_id, 1, out);
}

Status MemoryPageDevice::WritePage(PageId page_id, std::span<const std::byte, kPageSize> in) {
    return WritePageRun(page_id, 1, in);
}

Status MemoryPageDevice::ReadPageRun(PageId first_page_id, std::uint32_t nr_pages,
                                     std::span<std::byte> out) {
    Status status = CheckPageRunRange(first_page_id, nr_pages, page_capacity_);
    if (!status.ok()) {
        return status;
    }
    status = CheckPageRunBuffer(nr_pages, out.size());
    if (!status.ok()) {
        return status;
    }

    // Recorded before the injected failure is applied: the operation was
    // attempted, and a test asserting on the trace wants to see that.
    trace_.push_back(TraceEntry{OpKind::kRead, first_page_id, nr_pages});
    ++stats_.reads;
    if (auto failure = Take(fail_next_read_); failure.has_value()) {
        ++stats_.injections_fired;
        return std::move(*failure);
    }
    stats_.pages_read += nr_pages;

    for (std::uint32_t i = 0; i < nr_pages; ++i) {
        const Page& page = CurrentPage(first_page_id + i);
        std::memcpy(out.data() + static_cast<std::size_t>(i) * kPageSize, page.data(), kPageSize);
    }
    return Status::OK();
}

Status MemoryPageDevice::WritePageRun(PageId first_page_id, std::uint32_t nr_pages,
                                      std::span<const std::byte> in) {
    Status status = CheckPageRunRange(first_page_id, nr_pages, page_capacity_);
    if (!status.ok()) {
        return status;
    }
    status = CheckPageRunBuffer(nr_pages, in.size());
    if (!status.ok()) {
        return status;
    }

    trace_.push_back(TraceEntry{OpKind::kWrite, first_page_id, nr_pages});
    ++stats_.writes;
    if (auto failure = Take(fail_next_write_); failure.has_value()) {
        ++stats_.injections_fired;
        return std::move(*failure);
    }
    stats_.pages_written += nr_pages;

    // A torn write transfers a prefix of the run and then stops; the rest
    // of the affected bytes keep whatever they held. Nothing is reported -
    // the device does not know it happened, which is the point.
    std::size_t transferable = in.size();
    if (auto tear = Take(tear_next_write_); tear.has_value()) {
        ++stats_.injections_fired;
        transferable = std::min(*tear, in.size());
    }

    for (std::uint32_t i = 0; i < nr_pages; ++i) {
        const std::size_t page_start = static_cast<std::size_t>(i) * kPageSize;
        if (page_start >= transferable) {
            break;  // this page, and everything after it, never landed
        }
        const std::size_t n = std::min<std::size_t>(kPageSize, transferable - page_start);

        const PageId page_id = first_page_id + i;
        // Start from the page's current contents so a partial write leaves
        // the untransferred tail as it was, rather than as zeroes.
        Page updated = CurrentPage(page_id);
        std::memcpy(updated.data(), in.data() + page_start, n);
        pending_[page_id] = updated;

        if (n < kPageSize) {
            break;
        }
    }
    return Status::OK();
}

Status MemoryPageDevice::EnsureCapacity(std::uint32_t nr_pages) {
    if (nr_pages > kMaxPageCount) {
        return Status::InvalidArgument("MemoryPageDevice: requested capacity " +
                                       std::to_string(nr_pages) + " pages exceeds the " +
                                       std::to_string(kMaxPageCount) + "-page design ceiling");
    }
    if (nr_pages <= page_capacity_) {
        return Status::OK();  // never shrinks, and so is idempotent on replay
    }

    if (auto failure = Take(fail_next_grow_); failure.has_value()) {
        ++stats_.injections_fired;
        return std::move(*failure);
    }

    page_capacity_ = RoundUpToExtent(nr_pages);
    ++stats_.grows;
    trace_.push_back(TraceEntry{OpKind::kGrow, 0, page_capacity_});
    return Status::OK();
}

Status MemoryPageDevice::Sync() {
    trace_.push_back(TraceEntry{OpKind::kSync, 0, 0});
    ++stats_.syncs;
    if (auto failure = Take(fail_next_sync_); failure.has_value()) {
        ++stats_.injections_fired;
        return std::move(*failure);
    }

    for (auto& [page_id, page] : pending_) {
        durable_[page_id] = page;
    }
    pending_.clear();
    durable_page_capacity_ = page_capacity_;
    return Status::OK();
}

void MemoryPageDevice::FailNextRead(Status status) { fail_next_read_ = std::move(status); }
void MemoryPageDevice::FailNextWrite(Status status) { fail_next_write_ = std::move(status); }
void MemoryPageDevice::FailNextSync(Status status) { fail_next_sync_ = std::move(status); }
void MemoryPageDevice::FailNextGrow(Status status) { fail_next_grow_ = std::move(status); }

void MemoryPageDevice::TearNextWrite(std::size_t prefix_bytes) {
    tear_next_write_ = prefix_bytes;
}

void MemoryPageDevice::ClearInjections() noexcept {
    fail_next_read_.reset();
    fail_next_write_.reset();
    fail_next_sync_.reset();
    fail_next_grow_.reset();
    tear_next_write_.reset();
}

void MemoryPageDevice::Crash() {
    pending_.clear();
    page_capacity_ = durable_page_capacity_;
}

}  // namespace kds::storage
