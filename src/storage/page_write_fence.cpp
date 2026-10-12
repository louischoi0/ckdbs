#include "kds/storage/page_write_fence.hpp"

#include <algorithm>
#include <thread>

namespace kds::storage {

PageWriteFence::WriteScope::WriteScope(PageWriteFence& fence, PageId first, std::uint32_t pages)
    : fence_(fence), first_(first), pages_(pages), fenced_(false) {
    fence_.unfenced_.fetch_add(1, std::memory_order_seq_cst);
    if (!fence_.armed_.load(std::memory_order_seq_cst)) return;  // the disarmed path
    fence_.unfenced_.fetch_sub(1, std::memory_order_seq_cst);
    fenced_ = true;
    std::unique_lock<std::mutex> lock(fence_.mu_);
    if (fence_.reading_ && fence_.read_.Overlaps(first, pages)) {
        fence_.writes_waited_.fetch_add(1, std::memory_order_relaxed);
        fence_.changed_.wait(lock, [&] {
            return !(fence_.reading_ && fence_.read_.Overlaps(first, pages));
        });
    }
    fence_.writes_.push_back({first, pages});
}

PageWriteFence::WriteScope::~WriteScope() {
    if (!fenced_) {
        fence_.unfenced_.fetch_sub(1, std::memory_order_seq_cst);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(fence_.mu_);
        auto it = std::find_if(fence_.writes_.begin(), fence_.writes_.end(), [&](const Range& r) {
            return r.first == first_ && r.pages == pages_;
        });
        if (it != fence_.writes_.end()) fence_.writes_.erase(it);
    }
    fence_.changed_.notify_all();
}

void PageWriteFence::Arm() {
    armed_.store(true, std::memory_order_seq_cst);
    // Every write that read the fence disarmed is still counted here until
    // its transfer ends: microseconds, so a yield and then a short sleep.
    for (std::uint32_t turn = 0; unfenced_.load(std::memory_order_seq_cst) != 0; ++turn) {
        if (turn < 64) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
}

void PageWriteFence::Disarm() { armed_.store(false, std::memory_order_seq_cst); }

void PageWriteFence::BeginRead(PageId first, std::uint32_t pages) {
    std::unique_lock<std::mutex> lock(mu_);
    changed_.wait(lock, [&] {
        return std::none_of(writes_.begin(), writes_.end(),
                            [&](const Range& r) { return r.Overlaps(first, pages); });
    });
    reading_ = true;
    read_ = {first, pages};
}

void PageWriteFence::EndRead() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        reading_ = false;
    }
    changed_.notify_all();
}

}  // namespace kds::storage
