#pragma once

#include <cstdint>

namespace kds::server {

class Session;

// **Where a session's next transaction runs** (BL-R1, BL-R4,
// `instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`).
//
// A session is placed per transaction, not for its life. A listener asks
// its placer each time a session with no transaction open asks for a
// dispatch - the head of the transaction that statement begins, whether an
// autocommit statement or a `BEGIN` - and runs the statement on the core it
// answers, carrying the whole connection there first (`TcpServer`'s move,
// `connection_handoff.hpp`). **Inside a transaction nothing asks**: the
// transaction's in-flight slot, its waits and its commit are its core's
// (`crosscore.md` CC3), so a transaction stays where it began.
//
// **Asked concurrently**: one placer serves every core, and each core asks
// it on its own reactor thread whenever one of its sessions begins a
// transaction - so `Place` must be safe to call from several threads at
// once (read shared state with relaxed atomics, as `sched::CoreLoad`'s
// protocol does, and keep no unsynchronised member).
class TransactionPlacer {
public:
    virtual ~TransactionPlacer() = default;

    // The core the transaction `session` is about to begin runs on;
    // `core` - where the session is - to stay.
    virtual std::uint32_t Place(std::uint32_t core, Session& session) = 0;
};

}  // namespace kds::server
