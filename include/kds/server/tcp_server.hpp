#pragma once

#include <cstdint>

#include "kds/base/status.hpp"
#include "kds/server/command_dispatcher.hpp"

// The client-facing accept/read/dispatch/write loop - the actual
// "platform layer" doing real socket syscalls (rules.md #4: engine logic
// must go through injectable interfaces for I/O; this file *is* one of
// the boundary places, like main.cpp, allowed to call real syscalls
// directly). All the logic that can be unit-tested without a socket lives
// in CommandDispatcher instead; this file is deliberately as thin as
// possible around the syscalls themselves.
//
// Concurrency: serves one client connection at a time. Concurrent client
// handling needs the thread-per-core worker pool (rules.md #3) and
// whatever session/scheduling model KDS ends up using for it - neither
// exists yet, so this does not attempt to fake concurrency with e.g. ad
// hoc threads.

namespace kds::server {

class TcpServer {
public:
    // Binds and listens on 127.0.0.1:port. Fails with IoError if the
    // socket/bind/listen syscalls fail (e.g. the port is already in use).
    static StatusOr<TcpServer> Listen(std::uint16_t port);

    TcpServer(TcpServer&& other) noexcept;
    TcpServer& operator=(TcpServer&& other) noexcept;
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    ~TcpServer();

    // Accepts one client connection at a time, reads newline-terminated
    // command lines from it, runs each through `dispatcher`, and writes
    // back its response (newline-terminated). Moves on to accept the next
    // client when one disconnects. Returns once some dispatched command
    // sets DispatchOutcome::should_stop, or the listening socket itself
    // fails in a way that means no more clients can ever be accepted.
    void Serve(CommandDispatcher& dispatcher);

private:
    explicit TcpServer(int fd) noexcept : listen_fd_(fd) {}
    void CloseIfOpen() noexcept;

    int listen_fd_;
};

}  // namespace kds::server
