#include "kds/storage/page_reader.hpp"

#include <utility>

namespace kds::storage {

void PageReader::Start(unsigned threads, const sched::WakeRegistry* wake) {
    std::lock_guard lock(mu_);
    if (!threads_.empty()) return;
    stopping_ = false;
    wake_ = wake;
    threads_.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this] { Run(); });
    threaded_.store(!threads_.empty(), std::memory_order_relaxed);
}

void PageReader::Stop() noexcept {
    std::vector<std::thread> joining;
    {
        std::lock_guard lock(mu_);
        if (threads_.empty()) return;
        stopping_ = true;
        threaded_.store(false, std::memory_order_relaxed);
        joining.swap(threads_);
    }
    cv_.notify_all();
    for (std::thread& t : joining) t.join();
    // Nothing reads these now. Done, so no asker waits for a read that will
    // not be made; no wake, because the reactors are on their way out and a
    // parked statement is re-polled when its reactor next turns.
    std::lock_guard lock(mu_);
    for (const auto& request : queue_) request->done.store(true, std::memory_order_release);
    queue_.clear();
    wake_ = nullptr;
}

PageReader::Ticket PageReader::Submit(PageId page_id, std::uint32_t core) {
    Ticket ticket;
    {
        std::lock_guard lock(mu_);
        if (!threads_.empty() && !stopping_) {
            ticket.request_ = std::make_shared<Ticket::Request>();
            ticket.request_->page_id = page_id;
            ticket.request_->core = core;
            queue_.push_back(ticket.request_);
        }
    }
    if (ticket.request_ == nullptr) {
        // No thread: the read is made where it was asked for.
        device_.WarmPage(page_id);
        return ticket;
    }
    submitted_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_one();
    return ticket;
}

void PageReader::Run() {
    for (;;) {
        std::shared_ptr<Ticket::Request> request;
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            request = std::move(queue_.front());
            queue_.pop_front();
        }
        device_.WarmPage(request->page_id);
        completed_.fetch_add(1, std::memory_order_relaxed);
        // Published, then woken (AR0-6-R1): the asker's reactor reads `done`.
        request->done.store(true, std::memory_order_release);
        if (wake_ != nullptr) wake_->Wake(request->core);
    }
}

}  // namespace kds::storage
