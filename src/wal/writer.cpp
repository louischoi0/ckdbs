#include "kds/wal/writer.hpp"

namespace kds::wal {

WalWriter::WalWriter(LogDevice* device) : device_(device) {
    thread_ = std::thread([this] { Run(); });
}

WalWriter::~WalWriter() { Stop(); }

void WalWriter::RequestSync(Lsn lsn) {
    // Monotonic: a request behind one already pending changes nothing, and
    // the compare-exchange loop is what keeps two reactors' worth of future
    // callers from moving it backwards.
    Lsn seen = requested_.load(std::memory_order_relaxed);
    while (lsn > seen &&
           !requested_.compare_exchange_weak(seen, lsn, std::memory_order_release,
                                             std::memory_order_relaxed)) {
    }
    if (durable_.load(std::memory_order_acquire) >= lsn) return;  // already there

    // The lock is taken to *wake*, never to work: the fsync below happens
    // with nothing held.
    {
        std::lock_guard<std::mutex> guard(mutex_);
    }
    work_.notify_one();
}

Status WalWriter::EnsureDurable(Lsn lsn) {
    if (IsDurable(lsn)) return Status::OK();
    RequestSync(lsn);

    std::unique_lock<std::mutex> guard(mutex_);
    done_.wait(guard, [&] {
        return stopping_ || durable_.load(std::memory_order_acquire) >= lsn ||
               !last_failure_.ok();
    });
    if (durable_.load(std::memory_order_acquire) >= lsn) return Status::OK();
    if (!last_failure_.ok()) return last_failure_;
    return Status::IoError("wal writer: stopped before lsn " + std::to_string(lsn) +
                           " became durable");
}

Status WalWriter::last_failure() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return last_failure_;
}

void WalWriter::Stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        stopping_ = true;
    }
    work_.notify_all();
    done_.notify_all();
    thread_.join();
}

void WalWriter::Run() {
    for (;;) {
        Lsn target = 0;
        {
            std::unique_lock<std::mutex> guard(mutex_);
            work_.wait(guard, [&] {
                return stopping_ ||
                       requested_.load(std::memory_order_acquire) >
                           durable_.load(std::memory_order_relaxed);
            });
            if (stopping_) return;
            target = requested_.load(std::memory_order_acquire);
        }

        // **The snapshot rule.** `target` is where the reactor's writes had
        // reached when it asked. The fsync below may cover bytes written
        // since, and publishing those would be claiming durability for a
        // write that might not have been included - so the watermark moves
        // to what was asked for and no further.
        //
        // No lock is held here. That is the whole design: the reactor keeps
        // appending, flushing and serving statements for the milliseconds
        // this takes.
        const Status synced = device_->Sync();

        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (synced.ok()) {
                // Only ever forwards - two syncs can complete out of order
                // only if a future version runs more than one, and the max
                // is what stays true if that happens.
                Lsn seen = durable_.load(std::memory_order_relaxed);
                while (target > seen &&
                       !durable_.compare_exchange_weak(seen, target,
                                                       std::memory_order_release,
                                                       std::memory_order_relaxed)) {
                }
                syncs_.fetch_add(1, std::memory_order_relaxed);
                last_failure_ = Status::OK();
            } else {
                // The watermark stays where it was: a failed sync proves
                // nothing about what reached the platter. Recorded so a
                // waiter can be told rather than left waiting for a
                // watermark that is not coming.
                failures_.fetch_add(1, std::memory_order_relaxed);
                last_failure_ = synced;
            }
        }
        done_.notify_all();
    }
}

}  // namespace kds::wal
