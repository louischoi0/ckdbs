#include "kds/storage/data_syncer.hpp"
#include "kds/base/contention.hpp"

namespace kds::storage {

void DataSyncer::Start() {
    std::lock_guard lock(mu_);
    if (running_) return;
    stopping_ = false;
    running_ = true;
    thread_ = std::thread([this] { Run(); });
}

void DataSyncer::Stop() noexcept {
    {
        std::lock_guard lock(mu_);
        if (!running_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    thread_.join();
    {
        std::lock_guard lock(mu_);
        running_ = false;
    }
    // A `SyncNow` waiter woken by the notify above saw `running_` still set
    // and waits again; this is what lets it take the inline arm.
    cv_.notify_all();
}

std::uint64_t DataSyncer::Request() {
    const std::uint64_t ticket = requested_.fetch_add(1, std::memory_order_acq_rel) + 1;
    bool threaded;
    {
        std::lock_guard lock(mu_);
        threaded = running_;
    }
    if (threaded) {
        cv_.notify_all();
        return ticket;
    }
    // No thread: the sync is the caller's, and covers this ticket.
    if (SyncOnce().ok()) {
        std::uint64_t seen = completed_.load(std::memory_order_relaxed);
        while (seen < ticket &&
               !completed_.compare_exchange_weak(seen, ticket, std::memory_order_acq_rel)) {
        }
    }
    return ticket;
}

Status DataSyncer::Result(std::uint64_t ticket) const {
    if (failed()) {
        std::lock_guard lock(mu_);
        return failure_;
    }
    if (completed_.load(std::memory_order_acquire) >= ticket) return Status::OK();
    return Status::OutOfRange("data file sync " + std::to_string(ticket) + " is still pending");
}

Status DataSyncer::SyncNow() {
    const std::uint64_t ticket = Request();
    std::unique_lock lock(mu_);
    cv_.wait(lock, [&] { return Done(ticket) || !running_; });
    lock.unlock();
    // The thread stopped under this wait: sync here instead.
    if (!Done(ticket)) return SyncOnce();
    return Result(ticket);
}

Status DataSyncer::SyncOnce() {
    std::lock_guard sync(sync_mu_);
    if (failed()) {
        std::lock_guard lock(mu_);
        return failure_;
    }
    Status s = device_.Sync();
    Contention::Add(Tally::kDataSyncs);
    if (s.ok()) return s;
    Status failure = Status::IoError(
        "pagestore: the data file's sync failed (" + s.message() +
        "); its pages may be lost to the device, so nothing is written back or synced until "
        "a restart replays the log over them");
    std::function<void(const Status&)> handler;
    {
        std::lock_guard lock(mu_);
        failure_ = failure;
        failed_.store(true, std::memory_order_release);
        handler = on_failure_;
    }
    cv_.notify_all();
    if (handler) handler(failure);
    return failure;
}

void DataSyncer::Run() {
    std::unique_lock lock(mu_);
    while (true) {
        cv_.wait(lock, [this] {
            return stopping_ ||
                   requested_.load(std::memory_order_acquire) >
                       completed_.load(std::memory_order_acquire);
        });
        if (stopping_) return;
        // The latest ticket, read before the sync: every byte written before
        // it was issued is on the descriptor now, so this sync covers it.
        const std::uint64_t target = requested_.load(std::memory_order_acquire);
        lock.unlock();
        const Status s = SyncOnce();
        lock.lock();
        if (s.ok()) completed_.store(target, std::memory_order_release);
        cv_.notify_all();
        if (failed()) {
            // Sticky: nothing more to do but answer waiters, who read it.
            cv_.wait(lock, [this] { return stopping_; });
            return;
        }
    }
}

}  // namespace kds::storage
