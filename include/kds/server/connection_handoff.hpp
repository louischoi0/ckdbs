#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/sched/waker_table.hpp"

// D19's fallback (AT-S10c, AT-R12): **how a connection core 0 accepted
// reaches the core that will run it, on a platform that cannot share a port.**
//
// Above one core every core listens on the port with `SO_REUSEPORT` and the
// kernel spreads connections (AT-S8). Where the option is refused, core 0
// alone can bind; D19's mark is that it accepts and hands off, and that
// the handoff is "a ring consumer AU-S5 must list, not one it may strike".
// It is built as AR0-6-R1 builds every cross-core wake since the ring's
// retirement began: **the shared state under its own latch, then a kick**.
// Core 0 places the accepted socket in the destination's inbox and kicks it;
// the destination's reactor, woken, finds it there (`TcpServer::Host`). A
// kick carries no payload, which is why the socket travels in the inbox and
// not in the kick.
//
// ---- Lock protocol ------------------------------------------------------
//
// One `Latch` per core's inbox, never nested and never held across anything
// but a vector push or swap. `pending` is a hint read without the latch, so
// a woken reactor pays one acquire load when its inbox is empty; it is set
// under the latch before the kick (release) and cleared under the latch by
// the take, so **no socket is stranded**: the reactor re-polls its wait
// predicate after every block (AR0-6-R1's level-triggered wait), and the
// predicate reads `pending`. A kick can still be skipped - one that lands
// after the reactor's last poll and before it raises its sleeping flag -
// and that costs at most one idle block (`max_idle_block_ms`), AR0-6-R1's
// stated price, never the socket.
//
// ---- Ownership of a socket ----------------------------------------------
//
// An offered fd belongs to the inbox until taken, and to the taker after.
// One the destination never takes - an instance stopped between the accept
// and the take - is closed by the destructor, so no socket leaks.

namespace kds::server {

class ConnectionHandoff {
public:
    // `wakers` must outlive this, and may be null: nothing is kicked, and a
    // destination finds its inbox at its next wake for another reason.
    ConnectionHandoff(std::uint32_t cores, const sched::WakeRegistry* wakers);
    ~ConnectionHandoff();

    ConnectionHandoff(const ConnectionHandoff&) = delete;
    ConnectionHandoff& operator=(const ConnectionHandoff&) = delete;

    std::uint32_t cores() const noexcept { return static_cast<std::uint32_t>(inboxes_.size()); }

    // The core a newly accepted connection is placed on: round-robin over
    // every core, core 0 included. Core 0's accept path only.
    std::uint32_t NextCore() noexcept;

    // Connections placed so far, on any core. Diagnostics and tests.
    std::uint32_t placed() const noexcept { return next_.load(std::memory_order_relaxed); }

    // Places `fd` in `core`'s inbox, then kicks `core`. Callable from any
    // thread; `core` must be below `cores()`.
    void Offer(std::uint32_t core, int fd);

    // Whether `core`'s inbox may hold a socket. A hint: true may find the
    // inbox already taken by a racing `Take`, never false while one waits.
    bool Pending(std::uint32_t core) const noexcept;

    // Every socket waiting for `core`, in offer order, now owned by the
    // caller.
    std::vector<int> Take(std::uint32_t core);

private:
    struct Inbox {
        Latch latch;
        std::vector<int> fds;
        std::atomic<bool> pending{false};
    };

    std::vector<std::unique_ptr<Inbox>> inboxes_;
    const sched::WakeRegistry* wakers_;
    std::atomic<std::uint32_t> next_{0};
};

}  // namespace kds::server
