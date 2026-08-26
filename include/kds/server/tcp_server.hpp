#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/auth.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/wire_channel.hpp"

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
    // `reuse_port` sets SO_REUSEPORT before bind, so N listeners - one per
    // core - share one port and the kernel distributes accepted
    // connections among them (crosscore.md M3, workplan-peer-writer.md
    // PW5). Every socket on the port must pass it, core 0's included: the
    // option must be set on the first binder or later binds fail.
    static StatusOr<TcpServer> Listen(std::uint16_t port, bool reuse_port = false);

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

    // Installs the wire-byte transform every *subsequently accepted*
    // connection runs through (wire_channel.hpp) - in practice the TLS
    // channel, installed by the Expeditor when `tls = on`. Set it before
    // Attach(): connections accepted earlier keep the identity path.
    // Unset (the default) means plaintext, byte for byte the pre-seam
    // behaviour.
    using ChannelFactory = std::function<std::unique_ptr<WireChannel>()>;
    void set_channel_factory(ChannelFactory factory) noexcept {
        channel_factory_ = std::move(factory);
    }

    // Installs the authentication gate every subsequently accepted
    // connection must pass (auth.hpp) - in practice the SCRAM gate,
    // installed by the Expeditor when `auth = scram`. Set before
    // Attach(). Unset (the default) means every connection starts
    // authenticated, the pre-auth behaviour.
    using AuthGateFactory = std::function<std::unique_ptr<AuthGate>()>;
    // What STOP does. Unset (core 0, single-core): Scheduler::Stop() on
    // the attached reactor, which ends Serve() and takes the whole
    // instance down. A *peer* listener must not do that - stopping its own
    // reactor alone leaves a half-dead instance whose bound socket keeps
    // receiving a share of new connections nobody will drain - so
    // CoreRuntime installs a handler that routes the stop to the system
    // core instead, keeping "STOP has always meant the whole server" true
    // on every core.
    void set_stop_handler(std::function<void()> handler) { stop_handler_ = std::move(handler); }

    void set_auth_gate_factory(AuthGateFactory factory) noexcept {
        auth_gate_factory_ = std::move(factory);
    }

    // Live client connections, for tests and for the shutdown path.
    std::size_t open_connections() const noexcept { return clients_.size(); }

private:
    // One client's in-flight buffers. Commands are newline-framed and a
    // read can stop mid-line, so the remainder has to survive until the
    // next readable event - which is the whole reason a connection needs
    // state at all. The outbox is the same story on the way out: a
    // non-blocking socket can accept a partial reply, and the tail has to
    // survive until the fd is writable again. It is also what makes one
    // write() serve a whole batch of pipelined commands - see
    // DrainCommands.
    struct Connection {
        // `inbox` holds *application* bytes (command lines); `outbox`
        // holds *wire* bytes, ready for send(). With no channel the two
        // vocabularies coincide; with one, the channel is the only code
        // that converts between them.
        std::string inbox;
        std::string outbox;
        bool want_writable = false;

        // The wire transform (TLS), or null for plaintext. Owned by the
        // connection: the TLS handshake and record state are exactly as
        // per-connection as the inbox tail is.
        std::unique_ptr<WireChannel> channel;

        // The authentication gate, or null once passed (or never
        // required). Non-null means every line routes to the gate and
        // none reaches the dispatcher - "authenticated" is the gate's
        // absence, so there is no flag to disagree with it.
        std::unique_ptr<AuthGate> auth_gate;

        // ---- One statement at a time (the async dispatch seam) ----------
        //
        // A statement may now suspend, so its reply is not ready when
        // DrainCommands returns. Three fields carry that.
        //
        // **`in_flight` serializes the connection**, and it is not a
        // simplification - it is required twice over. The session is
        // stateful (an open transaction, the failed-txn flag), so a second
        // statement starting before the first finished would interleave
        // with it; and the newline protocol has no request ids, so replies
        // must leave in the order the commands arrived. Pipelining still
        // works exactly as it did - a batch of commands is drained one at a
        // time and the replies leave in one write - it is only *concurrency
        // within one connection* that is excluded, which the protocol never
        // offered.
        bool in_flight = false;

        // The statement being run, copied out of `inbox` before dispatch.
        // It has to live somewhere stable: the parser's tokens are views
        // into it (parser-v2.md's zero-copy tokens) and the inbox is
        // rewritten as more bytes arrive.
        std::string current_line;

        // The client hung up, or a write failed, while a statement was
        // running. The connection cannot be destroyed yet - the statement
        // holds a reference to its session - so it is marked and torn down
        // when the statement finishes. Cancelling instead would need
        // cancellation the engine does not have.
        bool closing = false;

        // The finished statement's reply, waiting to be appended to the
        // outbox. Owned by the connection because the coroutine writes
        // through a pointer to it and may outlive the call that started it.
        DispatchOutcome pending;


        // This connection's transaction state (session.hpp). Per
        // connection and not per server: two clients on one dispatcher must
        // not see each other's open transaction, which is exactly what
        // docs/spec/txn.md section 10-8 requires and what a shared dispatcher
        // could not provide before.
        Session session;
    };

    explicit TcpServer(int fd) noexcept : listen_fd_(fd) {}
    void CloseIfOpen() noexcept;

    void OnListenerReadable();
    void OnClientEvent(int client_fd, const sched::IoEvent& event);
    void OnClientReadable(int client_fd);
    // Starts the next complete command, if the connection has one and is
    // not already running one. Returns false if the connection was closed
    // and must not be touched again.
    //
    // It no longer *runs* commands - it starts one and returns. The reply
    // is appended by OnStatementComplete when the statement finishes, which
    // then calls this again for the next line.
    bool DrainCommands(int client_fd, Connection& conn);

    // The other half: a statement finished, so append its reply, honour
    // STOP, and continue draining.
    void OnStatementComplete(int client_fd);
    // Appends one reply line to the outbox, through the wire channel if
    // one is installed. Returns false if the connection was closed (a
    // channel Send failure).
    bool AppendReplyLine(int client_fd, Connection& conn, std::string reply);
    // Writes as much of conn.outbox as the socket will take and drops what
    // went out. Returns false if the connection was closed (write error).
    bool FlushOutbox(int client_fd, Connection& conn);
    // Keeps the fd's epoll interest in step with whether there is a reply
    // tail still waiting to go out.
    void SyncWriteInterest(int client_fd, Connection& conn);
    void CloseClient(int client_fd);

    bool logging(LogLevel level) const noexcept {
        return log_ != nullptr && log_->enabled(level);
    }

    int listen_fd_;
    sched::Scheduler* scheduler_ = nullptr;
    CommandDispatcher* dispatcher_ = nullptr;
    Logger* log_ = nullptr;
    ChannelFactory channel_factory_;
    AuthGateFactory auth_gate_factory_;
    std::function<void()> stop_handler_;
    std::unordered_map<int, Connection> clients_;
};

}  // namespace kds::server
