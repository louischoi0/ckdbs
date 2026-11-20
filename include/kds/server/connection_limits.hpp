#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

// **The instance's connection cap, and what holds a seat under it** (DJ-S1,
// `docs/spec/protocol.md` §10; `max_connections`,
// `reserved_admin_connections`).
//
// ---- What is counted ------------------------------------------------------
//
// A **session**: a connection on the default port from the moment its
// handshake ends - its `C_HELLO` where no authentication is owed, the
// gate's admission where one is - until it closes. Two kinds of connection
// are therefore never counted, and never refused by the cap:
//
// - **a cancel connection**, whose first frame is `C_CANCEL` and which is no
//   session (DJ-R2): a cancel refused at a full server would leave running
//   the one statement an operator most wants stopped, and nobody would be
//   told, a cancel being answered with nothing by design;
// - **a connection that has not finished its handshake**, one that has sent
//   nothing included. Counting those would let a peer with no credentials
//   hold every seat, the reserve's too, by saying hello and never
//   authenticating; uncounted, what it holds is what it held before there
//   was a cap (the pre-auth inbox, `tcp_server.cpp`).
//
// The debug text port and the KWP load port are not counted and not capped.
//
// ---- The reserve ----------------------------------------------------------
//
// `reserved_admin_connections` seats past `max_connections` are held for a
// session whose role an authentication gate stamped `admin`, so the cap
// cannot lock out the operator who would raise it. The seat is taken once
// the role is known, so the ceiling is the session's own: `max_connections`
// for every role but `admin`, and the reserve past it for `admin`. Where no
// gate stamps a role (`auth = off`) every session is `admin` and there is
// nobody to tell apart: the ceiling is `max_connections` and nothing is
// reserved.
//
// ---- Concurrency ----------------------------------------------------------
//
// One object for the instance, shared by every core's listener. The two
// limits are written before any listener attaches and never after. `seats`
// is the one word the reactors share, written where a session's handshake
// ends and at its close and never on a statement's path (DJ-R6); the two
// counters are written where a session is refused or closed. All three are
// read by `sys.meta` from any core, relaxed: a count, not a decision. The
// decision is the compare-and-swap in `Take`, so no interleaving seats a
// session past its ceiling.

namespace kds::server {

// The default of `reserved_admin_connections`, in force only where
// `max_connections` is set and a gate stamps roles. `[provisional]`
// (DJ-R10): PostgreSQL's `superuser_reserved_connections` default, taken as
// a starting point and not measured here.
inline constexpr std::uint32_t kDefaultReservedAdminConnections = 3;

struct ConnectionLimits {
    // `max_connections`; 0 is no limit (DJ-R1).
    std::uint32_t max_connections = 0;
    // `reserved_admin_connections`, as it applies: 0 where no gate stamps a
    // role.
    std::uint32_t reserved_admin_connections = 0;

    // Sessions holding a seat, on every core together.
    std::atomic<std::uint32_t> seats{0};
    // Sessions the cap refused.
    std::atomic<std::uint64_t> refused{0};
    // Connections of the default port closed by `idle_session_timeout_s`.
    // The debug text port's are closed too and counted nowhere: it is
    // handed no limits.
    std::atomic<std::uint64_t> idle_closes{0};
};

// One session's seat. Given back when the session's connection goes,
// whichever way it goes - a hangup, a keepalive reap, a listener detached, a
// connection dropped between two cores - because the seat is a member of
// what every one of those paths destroys.
class ConnectionSeat {
public:
    ConnectionSeat() noexcept = default;
    ConnectionSeat(const ConnectionSeat&) = delete;
    ConnectionSeat& operator=(const ConnectionSeat&) = delete;
    // Move-only, so a copy cannot count twice: the seat goes with what is
    // moved to and the source is left holding none.
    ConnectionSeat(ConnectionSeat&& other) noexcept
        : limits_(std::exchange(other.limits_, nullptr)) {}
    ConnectionSeat& operator=(ConnectionSeat&&) = delete;
    ~ConnectionSeat() {
        if (limits_ != nullptr) limits_->seats.fetch_sub(1, std::memory_order_relaxed);
    }

    // Takes a seat for a session whose handshake has ended. `reserve` says
    // the reserve is open to it: a gate stamped it `admin`. False - nothing
    // taken, `refused` counted - where every seat it may take is held. Null
    // `limits` admits and counts nothing.
    bool Take(ConnectionLimits* limits, bool reserve) noexcept {
        if (limits == nullptr || limits_ != nullptr) return true;
        const std::uint32_t cap = limits->max_connections;
        const std::uint32_t ceiling = cap + (reserve ? limits->reserved_admin_connections : 0);
        std::uint32_t held = limits->seats.load(std::memory_order_relaxed);
        do {
            if (cap != 0 && held >= ceiling) {
                limits->refused.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        } while (!limits->seats.compare_exchange_weak(held, held + 1, std::memory_order_relaxed));
        limits_ = limits;
        return true;
    }

private:
    ConnectionLimits* limits_ = nullptr;
};

}  // namespace kds::server
