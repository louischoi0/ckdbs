#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/sched/waker_table.hpp"
#include "kds/storage/page_device.hpp"

// **The data file's reads that leave a reactor** (DE-S1,
// `instructions/v3.2.0/workorder-de-page-read-off-the-reactor.md` DE-Q5,
// DE-Q9, DE-Q14; `docs/spec/page.md` §12).
//
// A statement whose fetch was refused because its page would have to be
// waited for (`miss_refusal.hpp`) hands the page here and parks. A thread
// calls `PageDevice::WarmPage`, which waits for the device and keeps
// nothing, then marks the request done and wakes the asker's core. The
// statement makes its fetch again and finds the page where a read that does
// not wait can take it.
//
// **A thread reads and does nothing else** (DE-Q9, `[quiet-wrong]`). It
// touches no frame, no frame table and no byte a statement will read: the
// one image of a page outside the device is the pool's, put there by the
// fetch every statement makes, on a reactor. So a request needs no owner
// once it is queued - an asker that was cancelled or timed out drops its
// ticket and the read finishes into nothing - and nobody waits for a
// publish a reactor owes.
//
// **The threads** are the instance's, a fixed number started by the server
// after the mount and stopped before the device goes (`Start`, `Stop`):
// threads created at startup, which `sched.md` §9's invariant 1 allows.
// Without them - a test, a tool, the simulator, the mount itself - a
// request is served where it is made and its ticket is done on return, so
// every caller has one shape (DE-Q12).
//
// **The wake is written whatever the destination's flag says**
// (`WakeRegistry::Wake`, DE-Q14): a read can complete inside the window a
// best-effort kick leaves open, and one idle block is twenty times a read.
//
// Lock protocol: `mu_` guards the queue and the two run flags and is never
// held across the device call or the wake; `threaded_` mirrors "the threads
// run" in an atomic written under `mu_` and read with no lock. A request's `done` is an atomic
// the asker's reactor polls with no lock. Nothing is taken under `mu_`, and
// no reactor parks holding it: `Submit` takes it for a push.

namespace kds::storage {

class PageReader {
public:
    // **How many reads may wait for the device at once.** Fixed, not a
    // setting (DE-Q5 (a), DE-Q13 (a)); `[provisional]` until a cell prices
    // another number.
    static constexpr unsigned kPageReadThreads = 4;

    explicit PageReader(PageDevice& device) noexcept : device_(device) {}
    ~PageReader() { Stop(); }
    PageReader(const PageReader&) = delete;
    PageReader& operator=(const PageReader&) = delete;

    // Starts `threads` threads; a no-op while they run. `wake` is where a
    // finished read's asker is woken, and outlives the threads; null wakes
    // nobody, and an asker then waits out its reactor's idle block.
    void Start(unsigned threads, const sched::WakeRegistry* wake);
    // Joins the threads after the reads in flight. A request left queued is
    // marked done unread: its asker makes its fetch again and reads the
    // page itself. Idempotent. **Called before the device's descriptor
    // closes and before any reactor registered in `wake` goes.**
    void Stop() noexcept;
    // The threads run: after `Start`, until `Stop` begins. Read with no
    // lock - a `SELECT` asks it as its walk is handed on - and a stale
    // answer is harmless either way: `Submit` decides under `mu_`, and a
    // request made of a reader with no thread is served where it is made.
    bool threaded() const noexcept { return threaded_.load(std::memory_order_relaxed); }

    // One page's read, as its asker holds it. Copyable, and safe to drop at
    // any time: the read does not need it.
    class Ticket {
    public:
        // The read was made - or was abandoned at `Stop`. Either way the
        // asker's next fetch decides what the page is.
        bool done() const noexcept {
            return request_ == nullptr || request_->done.load(std::memory_order_acquire);
        }

    private:
        friend class PageReader;
        struct Request {
            PageId page_id = kInvalidPageId;
            std::uint32_t core = 0;
            std::atomic<bool> done{false};
        };
        std::shared_ptr<Request> request_;
    };

    // Asks for `page_id` to be read for a statement of `core`. With no
    // thread the read is made here and the ticket is done on return.
    Ticket Submit(PageId page_id, std::uint32_t core);

    // Requests handed to a thread, and reads finished by one.
    std::uint64_t submitted() const noexcept { return submitted_.load(std::memory_order_relaxed); }
    std::uint64_t completed() const noexcept { return completed_.load(std::memory_order_relaxed); }

private:
    void Run();

    PageDevice& device_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Ticket::Request>> queue_;  // under mu_
    std::vector<std::thread> threads_;                    // under mu_
    bool stopping_ = false;                               // under mu_
    std::atomic<bool> threaded_{false};                   // written under mu_
    const sched::WakeRegistry* wake_ = nullptr;           // set before the threads start
    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> completed_{0};
};

}  // namespace kds::storage
