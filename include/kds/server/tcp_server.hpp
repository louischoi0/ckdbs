#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/command_dispatcher.hpp"

// The client-facing accept/read/dispatch/write loop - the actual
// "platform layer" doing real socket syscalls (rules.md #4: engine logic
// must go through injectable interfaces for I/O; this file *is* one of
// the boundary places, like main.cpp, allowed to call real syscalls
// directly). All the logic that can be unit-tested without a socket lives
// in CommandDispatcher instead; this file is deliberately as thin as
// possible around the syscalls themselves.
//
// Concurrency: this is a *reactor participant*, not a loop of its own. The
// listening socket and every accepted client are non-blocking fds
// registered with the Scheduler's io backend, and all the work happens in
// handlers the reactor calls (sched.md phase 1). That is what lets
// `system`-group work - the checkpointer's cadence above all (wal.md
// section 11) - actually get a turn: the old blocking accept()/read() loop
// left the process with nowhere to run anything else.
//
// Many clients are served concurrently as a consequence, but still on one
// thread, cooperatively: no client blocks another, and no request is
// handled in parallel with any other. The thread-per-core fan-out
// (rules.md #3) is unchanged and still Phase 2+.

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

    // Registers the listening socket with `scheduler` and starts accepting.
    // From here on the reactor drives everything: accepted clients are
    // registered too, readable events turn into newline-framed command
    // lines run through `dispatcher`, and replies are written back
    // newline-terminated.
    //
    // A dispatched command setting DispatchOutcome::should_stop calls
    // Scheduler::Stop(), which ends the caller's Run() - the whole server,
    // not just that connection, exactly as before.
    //
    // Both references must outlive this TcpServer. Call Detach() before
    // destroying the scheduler if it might outlive this object.
    // `log` is optional; null disables the connection/request diagnostics.
    Status Attach(sched::Scheduler& scheduler, CommandDispatcher& dispatcher,
                   Logger* log = nullptr);

    // Unregisters the listener and every live client, and closes the
    // clients. Idempotent; called by the destructor.
    void Detach() noexcept;

    // Live client connections, for tests and for the shutdown path.
    std::size_t open_connections() const noexcept { return clients_.size(); }

private:
    // One client's in-flight read buffer. Commands are newline-framed and
    // a read can stop mid-line, so the remainder has to survive until the
    // next readable event - which is the whole reason a connection needs
    // state at all.
    struct Connection {
        std::string inbox;
    };

    explicit TcpServer(int fd) noexcept : listen_fd_(fd) {}
    void CloseIfOpen() noexcept;

    void OnListenerReadable();
    void OnClientReadable(int client_fd);
    // Returns false if the connection was closed and must not be touched
    // again - either the client hung up or a dispatched command stopped
    // the server.
    bool DrainCommands(int client_fd, Connection& conn);
    void CloseClient(int client_fd);

    bool logging(LogLevel level) const noexcept {
        return log_ != nullptr && log_->enabled(level);
    }

    int listen_fd_;
    sched::Scheduler* scheduler_ = nullptr;
    CommandDispatcher* dispatcher_ = nullptr;
    Logger* log_ = nullptr;
    std::unordered_map<int, Connection> clients_;
};

}  // namespace kds::server
