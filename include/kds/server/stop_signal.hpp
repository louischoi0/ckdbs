#pragma once

#include <cstdint>

#include "kds/base/status.hpp"

// `SIGTERM` / `SIGINT` as a file descriptor, so a process-manager stop reaches
// the same shutdown path a client's `STOP` does.
//
// ---- Why this exists ------------------------------------------------------
//
// Until it did, the server installed **no signal handling at all**: `systemctl
// stop`, a container stop and Ctrl-C each terminated the process mid-flight, so
// the final sync and the shutdown checkpoint never ran and the next mount
// recovered as if from a crash. The only route to `Expeditor::Serve`'s shutdown
// tail was a client typing `STOP`, which is not how anything is operated
// (`docs/inflight/known-gaps.md`).
//
// ---- Why a signalfd and not a handler ------------------------------------
//
// A signal handler may do almost nothing safely - set a `volatile
// sig_atomic_t`, `write()` a byte - so the usual shape is a flag plus something
// that polls it, which buys a wakeup latency and a second thing to get right.
// This reactor is already epoll-based and takes arbitrary fds
// (`sched::Scheduler::RegisterIoHandler`), so a `signalfd` is strictly better:
// the delivery becomes an ordinary readable event, handled *in the reactor* on
// the reactor's thread, with no async-signal-safety question anywhere and no
// polling interval to choose.
//
// ---- The ordering that makes it work -------------------------------------
//
// `Install()` **blocks both signals first**, and must be called before any other
// thread exists. Two reasons, and the first is the one that bites:
//
//   1. A signal is delivered to the *process*, and the kernel picks any thread
//      that does not block it. The WAL writer thread starts inside
//      `Expeditor::Open` - so installing this later would leave a thread that
//      still takes the default action, and the process would die anyway,
//      intermittently.
//   2. A blocked signal is what makes the signalfd the only consumer: unblocked,
//      the default disposition races the fd and usually wins.
//
// The mask is inherited, so blocking before the first thread covers every
// thread this process will ever have - the writer, the peer-core reactors, all
// of them.
//
// Platform layer (`docs/rules/rules.md` §4): this file is the only place in the engine
// that calls `sigprocmask` or `signalfd`, and the reactor above it only ever
// sees an fd.

namespace kds::server {

class StopSignal {
public:
    // Blocks SIGTERM and SIGINT and returns a descriptor that becomes readable
    // when either is delivered. **Call before starting any thread.**
    //
    // Fails with whatever `errno` reports, and a caller that treats that as
    // fatal is right to: a server that cannot hear a stop is one that has to be
    // killed, and being killed is the thing this exists to stop.
    static StatusOr<StopSignal> Install();

    StopSignal() = default;
    StopSignal(const StopSignal&) = delete;
    StopSignal& operator=(const StopSignal&) = delete;
    StopSignal(StopSignal&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    StopSignal& operator=(StopSignal&& other) noexcept;
    ~StopSignal();

    // Readable exactly when a stop has been requested. -1 when nothing is
    // installed, which a caller may register nothing for.
    int fd() const noexcept { return fd_; }
    bool installed() const noexcept { return fd_ >= 0; }

    // Drains the delivery, so a level-triggered epoll does not report the same
    // stop forever. The signal number, or 0 if nothing was pending.
    //
    // The caller does not need the number to shut down - one readable event is
    // the whole message - and it is returned because "stopped on SIGINT" and
    // "stopped on SIGTERM" are different sentences in a log an operator reads.
    std::uint32_t Consume() noexcept;

private:
    explicit StopSignal(int fd) noexcept : fd_(fd) {}

    int fd_ = -1;
};

}  // namespace kds::server
