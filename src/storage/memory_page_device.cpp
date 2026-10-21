#include <mutex>

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
    std::lock_guard<std::mutex> guard(mu_);
    Status status = CheckPageRunRange(first_page_id, nr_pages,
                                      page_capacity_.load(std::memory_order_relaxed));
    if (!status.ok()) {
        return status;
    }
    status = CheckPageRunBuffer(nr_pages, out.size());
    if (!status.ok()) {
        return status;
    }
    if (power_lost_) return Status::IoError("MemoryPageDevice: the power is lost");

    // Recorded before the injected failure is applied: the operation was
    // attempted, and a test asserting on the trace wants to see that.
    trace_.push_back(TraceEntry{OpKind::kRead, first_page_id, nr_pages});
    ++stats_.reads;
    if (auto failure = fail_next_read_.Take(); failure.has_value()) {
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
    std::lock_guard<std::mutex> guard(mu_);
    Status status = CheckPageRunRange(first_page_id, nr_pages,
                                      page_capacity_.load(std::memory_order_relaxed));
    if (!status.ok()) {
        return status;
    }
    status = CheckPageRunBuffer(nr_pages, in.size());
    if (!status.ok()) {
        return status;
    }
    if (power_lost_) return Status::IoError("MemoryPageDevice: the power is lost");
    if (power_loss_skip_.has_value() && (*power_loss_skip_)-- == 0) {
        // The write in flight when the power went: what of it landed is
        // the crash's to decide (`CrashPersisting`).
        power_loss_skip_.reset();
        power_lost_ = true;
        for (std::uint32_t i = 0; i < nr_pages; ++i) {
            Page image;
            std::memcpy(image.data(), in.data() + static_cast<std::size_t>(i) * kPageSize,
                        kPageSize);
            in_flight_.emplace_back(first_page_id + i, image);
        }
        ++stats_.injections_fired;
        if (on_power_loss_) on_power_loss_();
        return Status::IoError("MemoryPageDevice: the power is lost");
    }

    trace_.push_back(TraceEntry{OpKind::kWrite, first_page_id, nr_pages});
    ++stats_.writes;
    if (auto failure = fail_next_write_.Take(); failure.has_value()) {
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
    std::lock_guard<std::mutex> guard(mu_);
    if (nr_pages > kMaxPageCount) {
        return Status::InvalidArgument("MemoryPageDevice: requested capacity " +
                                       std::to_string(nr_pages) + " pages exceeds the " +
                                       std::to_string(kMaxPageCount) + "-page design ceiling");
    }
    if (power_lost_) return Status::IoError("MemoryPageDevice: the power is lost");
    if (nr_pages <= page_capacity_.load(std::memory_order_relaxed)) {
        return Status::OK();  // never shrinks, and so is idempotent on replay
    }

    if (auto failure = fail_next_grow_.Take(); failure.has_value()) {
        ++stats_.injections_fired;
        return std::move(*failure);
    }

    const std::uint32_t grown = RoundUpToExtent(nr_pages);
    page_capacity_.store(grown, std::memory_order_release);
    ++stats_.grows;
    trace_.push_back(TraceEntry{OpKind::kGrow, 0, grown});
    return Status::OK();
}

Status MemoryPageDevice::Sync() {
    std::lock_guard<std::mutex> guard(mu_);
    if (power_lost_) return Status::IoError("MemoryPageDevice: the power is lost");
    trace_.push_back(TraceEntry{OpKind::kSync, 0, 0});
    ++stats_.syncs;
    if (auto failure = fail_next_sync_.Take(); failure.has_value()) {
        ++stats_.injections_fired;
        return std::move(*failure);
    }

    for (auto& [page_id, page] : pending_) {
        durable_[page_id] = page;
    }
    pending_.clear();
    durable_page_capacity_.store(page_capacity_.load(std::memory_order_relaxed),
                                 std::memory_order_release);
    return Status::OK();
}

void MemoryPageDevice::FailNextRead(Status status, std::uint64_t skip) {
    std::lock_guard<std::mutex> guard(mu_);
    fail_next_read_.Arm(std::move(status), skip);
}
void MemoryPageDevice::FailNextWrite(Status status, std::uint64_t skip) {
    std::lock_guard<std::mutex> guard(mu_);
    fail_next_write_.Arm(std::move(status), skip);
}
void MemoryPageDevice::FailNextSync(Status status, std::uint64_t skip) {
    std::lock_guard<std::mutex> guard(mu_);
    fail_next_sync_.Arm(std::move(status), skip);
}
void MemoryPageDevice::FailNextGrow(Status status, std::uint64_t skip) {
    std::lock_guard<std::mutex> guard(mu_);
    fail_next_grow_.Arm(std::move(status), skip);
}

void MemoryPageDevice::TearNextWrite(std::size_t prefix_bytes) {
    std::lock_guard<std::mutex> guard(mu_);
    tear_next_write_ = prefix_bytes;
}

void MemoryPageDevice::ClearInjections() noexcept {
    std::lock_guard<std::mutex> guard(mu_);
    fail_next_read_.Clear();
    fail_next_write_.Clear();
    fail_next_sync_.Clear();
    fail_next_grow_.Clear();
    tear_next_write_.reset();
    // A power loss not yet struck is an injection like any other; one that
    // struck is the device's state, which only a crash restores.
    power_loss_skip_.reset();
}

void MemoryPageDevice::LosePowerAtWrite(std::uint64_t skip, std::function<void()> on_loss) {
    std::lock_guard<std::mutex> guard(mu_);
    power_loss_skip_ = skip;
    on_power_loss_ = std::move(on_loss);
}

bool MemoryPageDevice::power_lost() const noexcept {
    std::lock_guard<std::mutex> guard(mu_);
    return power_lost_;
}

void MemoryPageDevice::Crash() {
    std::lock_guard<std::mutex> guard(mu_);
    power_loss_skip_.reset();
    on_power_loss_ = nullptr;
    power_lost_ = false;
    in_flight_.clear();
    pending_.clear();
    page_capacity_.store(durable_page_capacity_.load(std::memory_order_relaxed),
                         std::memory_order_release);
}

namespace {

// splitmix64's finalizer: the seed's choices, page by page, with no state.
std::uint64_t Mix(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

}  // namespace

MemoryPageDevice::PowerCut MemoryPageDevice::CrashPersisting(std::uint64_t seed, bool tear) {
    std::lock_guard<std::mutex> guard(mu_);
    const std::uint32_t durable_capacity = durable_page_capacity_.load(std::memory_order_relaxed);
    // In id order, so the choice is the seed's and not the map's.
    std::vector<PageId> ids;
    for (const auto& [id, page] : pending_) {
        if (id < durable_capacity) ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    // The in-flight run's pages are unsynced writes too, newer than any
    // pending image of theirs.
    for (const auto& [id, image] : in_flight_) {
        if (id >= durable_capacity) continue;
        if (!pending_.count(id)) ids.push_back(id);
        pending_[id] = image;
    }
    PowerCut cut;
    cut.unsynced = ids.size();
    // A torn page is the in-flight write's: the one transfer the power cut.
    std::optional<PageId>& torn = cut.torn;
    std::optional<Page> torn_image;
    std::vector<PageId> flying;
    for (const auto& [id, image] : in_flight_) {
        if (id < durable_capacity) flying.push_back(id);
    }
    cut.in_flight = flying.size();
    if (tear && !flying.empty()) {
        const PageId id = flying[Mix(seed ^ 0x7EA2) % flying.size()];
        constexpr std::size_t kSector = 512;
        const std::size_t prefix = kSector * (1 + Mix(seed ^ 0x5EC7) % (kPageSize / kSector - 1));
        const auto old = durable_.find(id);
        const Page old_image = old != durable_.end() ? old->second : Page{};
        Page image = old_image;
        std::memcpy(image.data(), pending_.at(id).data(), prefix);
        // A tear whose image is the new one or the old one whole - the bytes
        // either side of the sector boundary unchanged - tore nothing: the
        // page is kept or lost as any other, and is never reported torn.
        if (image != pending_.at(id) && image != old_image) {
            torn = id;
            torn_image = image;
        }
    }
    for (const PageId id : ids) {
        if (id == torn) continue;
        if ((Mix(seed ^ id) & 1) != 0) {
            durable_[id] = pending_.at(id);
            ++cut.kept;
        }
    }
    if (torn.has_value()) durable_[*torn] = *torn_image;
    pending_.clear();
    in_flight_.clear();
    power_loss_skip_.reset();
    on_power_loss_ = nullptr;
    power_lost_ = false;
    page_capacity_.store(durable_capacity, std::memory_order_release);
    return cut;
}

}  // namespace kds::storage
