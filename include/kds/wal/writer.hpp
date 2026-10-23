#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <mutex>
#include <thread>

#include "kds/base/status.hpp"
#include "kds/sched/waker_table.hpp"
#include "kds/wal/log_device.hpp"
#include "kds/wal/record.hpp"

// The WAL writer thread: the one place in this engine that blocks on a
// storage device on purpose.
//
// ---- Why this exists at all ----------------------------------------------
//
// A sync on the reactor thread bills its `fsync` to whichever statement is
// in flight (the D3 loss-window tick measured ~2.2 ms every 12 ms,
// `bench/results-latency-matrix.md`); only moving it off the thread removes
// that stall.
//
// ---- The rule this bends, and how far ------------------------------------
//
// `docs/rules/rules.md` §3 keeps state core-local unless a spec declares it
// shared, and admits a lock only with a justification naming what it
// protects and its acquisition order. This is such a boundary - the
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
//   sync_        the stream's `SyncDevice`, which holds the stream's
//                `sync_mutex_` across the fsync - the one lock this thread
//                and the reactor's own sync share. This thread takes it with
//                no other held; a segment roll takes it under the stream
//                latch, so a roll waits out this thread's fsync with every
//                appender behind it (`wal/stream.hpp`).
//   kick_mutex_  held across the whole of a kick round (`KickDurableWaiters`:
//                the load of `wake_` and every `Kick` through it) and by
//                `SetWakeRegistry`'s store. **It protects the registry - and
//                the reactors registered in it - against teardown**: once
//                `SetWakeRegistry(nullptr)` returns, no kick through the old
//                registry is in flight and none will start. Taken only by
//                this thread and by the thread that sets the registry (the
//                instance's boot and teardown), never by a reactor, never
//                together with `mutex_`. Order: `kick_mutex_`, then whatever
//                a registry's `Kick` takes (`SimWakerTable::latch_`); so the
//                registry is never set under such a latch, nor from this
//                thread.
//
// The reactor's hot path - `Append` into the ring, `Flush` into the page
// cache - touches none of it. Those stay `WalStream`'s. **This class touches
// the stream only through `sync_`**: it takes a device, a sync and a
// target, and the only thing it knows how to do is make bytes that are
// already written durable.
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
    // Stop() or destruction. **`sync` is what every sync calls, and it is
    // required**: the manager passes the stream's `SyncDevice`, the gate
    // every syncer of the log goes through (`wal/stream.hpp`), and a writer
    // that synced the device itself would race the reactor's own sync past
    // a failure. A test without a stream passes the device's `Sync`.
    WalWriter(LogDevice* device, std::function<Status()> sync);
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

    // Asks the thread to run the device's `ReclaimDetached` (BC-R5): the
    // unlinks and the directory sync of the segments recycling detached.
    // Returns at once and does no I/O, like `RequestSync`. The removal runs
    // right after the sync the same pass makes, so a committer already
    // waiting is not put behind it and a stream of commits cannot starve
    // it; a peer committer that asks during it waits it out
    // (`workorder-bc-wal-recycling.md` §1.7, BC-Q5).
    void RequestReclaim();

    // **Parked committers are kicked, not polled** (BA-R4 part 1,
    // `instructions/v3.0.0/workorder-ba-parallelism.md`). A statement parked
    // on a durability point counts itself into its core here for the life of
    // its wait (`WalManager::DurableWait`); every time the watermark moves,
    // and when the writer fails, each core with a waiter is kicked through
    // `wake` - write-then-kick, as the lock table and the commit marker do
    // (`sched/waker_table.hpp`). Null until set, and then no core is kicked:
    // a reactor whose drain still reports a staged commit as work never
    // blocks, so it needs none.
    //
    // **The order that makes a waiter visible.** A waiter counts itself in
    // and then reads the watermark; the writer moves the watermark and then
    // reads the counts. A full fence on each side between the two means at
    // least one of them sees the other: the waiter sees the new watermark,
    // or the writer sees the waiter and kicks it. The kick itself can still
    // be skipped by the registry's sleeping-flag race (`WakerTable::Kick`),
    // costing that core one idle block.
    //
    // **Setting it waits out a kick in flight**, under `kick_mutex_`: on
    // return, every kick of a round that loaded the previous registry has
    // finished, and every later round loads the new one. So
    // `SetWakeRegistry(nullptr)` is the writer's quiesce - after it, this
    // thread touches neither the old registry nor any reactor registered in
    // it - and teardown calls it before it destroys the first reactor
    // (`WalManager::StopKicking`; `sched.md` §5). It blocks for at most one
    // round's kicks, which take no lock this caller can hold.
    void SetWakeRegistry(const sched::WakeRegistry* wake) noexcept;
    bool kicks_durable_waiters() const noexcept {
        return wake_.load(std::memory_order_acquire) != nullptr;
    }
    void EnterDurableWait(std::uint32_t core) noexcept {
        durable_waiters_[core].fetch_add(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    void LeaveDurableWait(std::uint32_t core) noexcept {
        durable_waiters_[core].fetch_sub(1, std::memory_order_relaxed);
    }

    // Stops the thread, after finishing whatever sync was in flight. Safe to
    // call twice; the destructor calls it.
    void Stop();

    // Syncs performed, and failures. **A failure is the writer's last sync**
    // (the stream's fail-stop, `wal/stream.hpp`): the watermark stays where
    // it was, every later wait is answered with that failure, and no later
    // request is synced - a retried fsync can report success over pages the
    // failed one dropped, and publishing that would claim durability for
    // bytes that may be gone.
    std::uint64_t syncs() const noexcept { return syncs_.load(std::memory_order_relaxed); }
    std::uint64_t failures() const noexcept { return failures_.load(std::memory_order_relaxed); }

    // Reclaims that failed. A failed one leaves its segments queued on the
    // device, and the next request retries them.
    std::uint64_t reclaim_failures() const noexcept {
        return reclaim_failures_.load(std::memory_order_relaxed);
    }

    // The failure that ended this writer's syncing, OK before one.
    // Guarded by the mutex because a Status carries a string.
    Status last_failure() const;

    // The device this writer syncs. Exposed for one caller:
    // `WalManager::Attach` refuses a writer that is not the one over the
    // stream it was handed, which would otherwise be a comment the caller
    // could quietly disobey and a durability wait that never completes.
    const LogDevice* device() const noexcept { return device_; }

private:
    void Run();
    void Reclaim();
    void KickDurableWaiters() noexcept;

    // `kMaxCores` (superblock.hpp, which this layer does not include):
    // no instance has more cores, and `CheckCoreCount` refuses one that
    // would, so a core always has its own slot.
    static constexpr std::uint32_t kWaitSlots = 64;
    std::array<std::atomic<std::uint32_t>, kWaitSlots> durable_waiters_{};
    // Atomic because a reactor reads it unlatched (`kicks_durable_waiters`);
    // written and kicked through only under `kick_mutex_`.
    std::atomic<const sched::WakeRegistry*> wake_{nullptr};
    std::mutex kick_mutex_;

    LogDevice* device_;

    std::atomic<Lsn> requested_{0};
    std::atomic<Lsn> durable_{0};
    std::atomic<std::uint64_t> syncs_{0};
    std::atomic<std::uint64_t> failures_{0};
    std::atomic<std::uint64_t> reclaim_failures_{0};

    mutable std::mutex mutex_;
    std::condition_variable work_;   // reactor -> writer: something to do
    std::condition_variable done_;   // writer -> reactor: watermark moved
    bool stopping_ = false;
    bool reclaim_requested_ = false;  // under mutex_
    std::function<Status()> sync_;
    Status last_failure_;

    std::thread thread_;
};

}  // namespace kds::wal
