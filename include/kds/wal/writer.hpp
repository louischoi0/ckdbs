#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "kds/base/status.hpp"
#include "kds/wal/log_device.hpp"
#include "kds/wal/record.hpp"

// The WAL writer thread: the one place in this engine that blocks on a
// storage device on purpose.
//
// ---- Why this exists at all ----------------------------------------------
//
// Every sync used to happen on the reactor thread, which serves statements.
// `bench/results-latency-matrix.md` prices both consequences: a commit sync
// pinned throughput at one commit per device sync however many clients asked
// (fixed by parking the committer, W1), and the D3 loss-window sync produced
// one ~2.2 ms statement every 12 ms - a system cost billed to whichever
// client happened to be in flight. Tuning moves that stall; only moving the
// `fsync` off the thread removes it.
//
// ---- The rule this bends, and how far ------------------------------------
//
// `docs/rules/rules.md` §3 makes the engine thread-per-core, shared-nothing, and
// permits a lock only at a partition boundary with a justification naming
// what it protects and its acquisition order. This is that boundary - the
// engine on one side, the device on the other - and this comment is that
// justification.
//
// **What is shared is deliberately tiny**, and it is what keeps the reactor
// lock-free on its own path:
//
//   requested_   reactor writes, writer reads.   atomic, release/acquire
//   durable_     writer writes, reactor reads.   atomic, release/acquire
//   mutex_       held only to wait or to wake. **Never across an fsync**,
//                and never by the reactor except in EnsureDurable(), which
//                is the one call whose whole purpose is to block.
//
// The reactor's hot path - `Append` into the ring, `Flush` into the page
// cache - touches none of it. Those stay `WalStream`'s, single-threaded,
// exactly as before. **This class never touches the stream**: it takes a
// device and a target, and the only thing it knows how to do is make bytes
// that are already written durable.
//
// ---- Why a target LSN rather than "sync everything" -----------------------
//
// The reactor hands over the flushed watermark *as it was when it asked*.
// The fsync that follows may well cover more than that - bytes written while
// it ran - and publishing the snapshot rather than the later position is
// what makes the answer safe rather than optimistic: `durable_lsn` must
// never name a byte whose write might not have been included.

namespace kds::wal {

class WalWriter {
public:
    // `device` must outlive this. The thread starts here and runs until
    // Stop() or destruction.
    explicit WalWriter(LogDevice* device);
    ~WalWriter();

    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    // Asks for everything up to `lsn` to be made durable, and returns
    // immediately. Idempotent and monotonic: a request behind one already
    // pending is absorbed.
    //
    // Called from the reactor, on the statement path, and it does no I/O -
    // which is the entire point.
    void RequestSync(Lsn lsn);

    // The watermark: every byte below this is on the platter.
    Lsn durable_lsn() const noexcept { return durable_.load(std::memory_order_acquire); }

    bool IsDurable(Lsn lsn) const noexcept { return durable_lsn() >= lsn; }

    // Blocks until `lsn` is durable, requesting a sync if none is pending.
    //
    // **A blocking call on a thread that must not block**, which is why the
    // statement path no longer uses it: a committing statement parks on
    // IsDurable() instead (command_dispatcher.hpp). What is left are the
    // callers whose entire meaning is "wait" - a client's `SYNC`, a clean
    // shutdown, and `Dispatch()`'s inline path for callers with no scheduler
    // to park on.
    Status EnsureDurable(Lsn lsn);

    // Stops the thread, after finishing whatever sync was in flight. Safe to
    // call twice; the destructor calls it.
    void Stop();

    // Syncs performed, and failures. A failure leaves the watermark where it
    // was - a sync that failed proves nothing about what reached the platter
    // - and the next request retries it.
    std::uint64_t syncs() const noexcept { return syncs_.load(std::memory_order_relaxed); }
    std::uint64_t failures() const noexcept { return failures_.load(std::memory_order_relaxed); }

    // The last failure, for a caller that wants to report rather than
    // retry. Guarded by the mutex because a Status carries a string.
    Status last_failure() const;

private:
    void Run();

    LogDevice* device_;

    std::atomic<Lsn> requested_{0};
    std::atomic<Lsn> durable_{0};
    std::atomic<std::uint64_t> syncs_{0};
    std::atomic<std::uint64_t> failures_{0};

    mutable std::mutex mutex_;
    std::condition_variable work_;   // reactor -> writer: something to do
    std::condition_variable done_;   // writer -> reactor: watermark moved
    bool stopping_ = false;
    Status last_failure_;

    std::thread thread_;
};

}  // namespace kds::wal
