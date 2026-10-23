#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "kds/base/status.hpp"
#include "kds/storage/page_device.hpp"

// **The data file's one sync** (BI-S4: BI-Q2 (a), BI-Q3 (a)).
//
// Every `fsync` of the data file goes through here: a checkpoint step's, the
// publish's map write barrier (`DevicePageStore::WriteHeaderlessMaps`) and
// the anchor publish's as a request the caller parks on, and `SYNC`'s, the
// carve's, `PersistMaps`', shutdown's and a blocking map flush's barrier
// (`DevicePageStore::FlushMaps`) as a call that waits. **One at a
// time**, because two overlapping `fsync`s on one descriptor can hand the
// error to one caller and OK to the other (`wal.md` §6 item 5, the log's
// own reason); and **a failure is sticky**, because on Linux a failed
// `fsync` can drop the dirty pages and clear the error, so a later sync
// reporting OK would cover bytes that are gone. Before BI-S4
// `FilePageDevice::Sync` returned the failure and recorded nothing
// (the bug entry BI-S4 closed and deleted, at `397bd0fc`).
//
// **A failure stops the instance** (BI-Q3 (a)). A writeback cleans its
// frames before the sync, so after a failure frames this pool calls clean
// may hold bytes the device lost; only a restart, which re-reads the log
// over them, can serve them again. The failure handler is that stop - the
// server's aborts the process after fail-stopping the log, the simulation's
// fail-stops the log and crashes the instance - and with none installed the
// failure is still sticky: every later sync is refused, and the store
// writes nothing back (`DevicePageStore::WriteBack`).
//
// **The thread** is the instance's one, started by the server after the
// mount and stopped before the store goes (`Start`, `Stop`). Without it -
// tests, tools, the mount itself - a request syncs inline and returns a
// ticket already complete, so every caller has one shape.
//
// **A ticket is satisfied by a sync that began after it was issued**: the
// thread reads the latest ticket before it syncs and completes exactly up
// to it, so every byte written before `Request` returned is covered.
//
// Lock protocol: `mu_` guards the queue state and `failure_`, and is never
// held across the device call; `sync_mu_` serializes the device call and is
// the innermost lock any caller of this class takes. Neither is held across
// a reactor park: a caller parks on `Done`, which reads atomics.
namespace kds::storage {

class DataSyncer {
public:
    explicit DataSyncer(PageDevice& device) noexcept : device_(device) {}
    ~DataSyncer() { Stop(); }
    DataSyncer(const DataSyncer&) = delete;
    DataSyncer& operator=(const DataSyncer&) = delete;

    // Starts the thread; a no-op when it runs. One thread at startup, which
    // `sched.md` invariant 1 allows.
    void Start();
    // Joins the thread after the sync in flight, if any. Requests left
    // behind are not synced; a caller that needs them syncs with `SyncNow`,
    // which falls back to the inline arm. Idempotent.
    void Stop() noexcept;

    // Issues a ticket for a sync of everything written so far. Does no I/O
    // on the caller's thread when the thread runs; syncs inline otherwise.
    std::uint64_t Request();
    // The sync for `ticket` has completed, or the data file has failed.
    bool Done(std::uint64_t ticket) const noexcept {
        return completed_.load(std::memory_order_acquire) >= ticket || failed();
    }
    // OK once `Done(ticket)` and nothing failed; the failure once one did.
    Status Result(std::uint64_t ticket) const;

    // Request and wait: the synchronous callers' shape.
    Status SyncNow();

    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }

    // Called once, with the first failure, on whichever thread synced.
    void SetFailureHandler(std::function<void(const Status&)> handler) {
        std::lock_guard lock(mu_);
        on_failure_ = std::move(handler);
    }

private:
    // One device sync under `sync_mu_`, refused once failed; the first
    // failure is recorded and handed to the handler.
    Status SyncOnce();
    void Run();

    PageDevice& device_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::mutex sync_mu_;
    std::thread thread_;
    bool running_ = false;   // under mu_
    bool stopping_ = false;  // under mu_
    std::atomic<std::uint64_t> requested_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<bool> failed_{false};
    Status failure_;  // under mu_
    std::function<void(const Status&)> on_failure_;  // under mu_
};

}  // namespace kds::storage
