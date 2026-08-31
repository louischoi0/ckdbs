#include "kds/server/tcp_server.hpp"

#include "kds/sched/coro.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fcntl.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kds::server {
namespace {

Status SetNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return Status::IoError(std::string("fcntl(F_GETFL) failed: ") + std::strerror(errno));
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return Status::IoError(std::string("fcntl(F_SETFL) failed: ") + std::strerror(errno));
    }
    return Status::OK();
}

}  // namespace

StatusOr<TcpServer> TcpServer::Listen(std::uint16_t port, bool reuse_port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return Status::IoError(std::string("socket() failed: ") + std::strerror(errno));
    }

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (reuse_port) {
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
            // Refused rather than degraded: a listener that silently fell
            // back to exclusive binding would make every later per-core
            // bind fail with EADDRINUSE, which reads as a port clash
            // rather than what it is.
            Status s =
                Status::IoError(std::string("setsockopt(SO_REUSEPORT) failed: ") +
                                std::strerror(errno));
            ::close(fd);
            return s;
        }
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    // The reinterpret_cast below is the standard POSIX sockaddr_in* ->
    // sockaddr* idiom the socket API itself requires - not the on-disk
    // struct-over-buffer cast rules.md #2 forbids (that rule is about our
    // own persisted formats, not interop with an OS ABI we don't control).
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        Status s = Status::IoError(std::string("bind() failed: ") + std::strerror(errno));
        ::close(fd);
        return s;
    }
    if (::listen(fd, 16) < 0) {
        Status s = Status::IoError(std::string("listen() failed: ") + std::strerror(errno));
        ::close(fd);
        return s;
    }

    return TcpServer(fd);
}

TcpServer::TcpServer(TcpServer&& other) noexcept
    : listen_fd_(other.listen_fd_),
      scheduler_(other.scheduler_),
      dispatcher_(other.dispatcher_),
      log_(other.log_),
      channel_factory_(std::move(other.channel_factory_)),
      auth_gate_factory_(std::move(other.auth_gate_factory_)),
      // **The configuration moves with the socket.** A listener is
      // configured and *then* moved into the optional that owns it, so a
      // member left out here is a setting silently dropped - which is
      // exactly what happened to `protocol_` first time round: the debug
      // port was configured for the text protocol, moved, and then served
      // KWP on it while the KWP port served KWP too.
      protocol_(other.protocol_),
      durability_(other.durability_),
      identity_source_(std::move(other.identity_source_)),
      next_identity_(other.next_identity_),
      server_info_(std::move(other.server_info_)),
      stop_handler_(std::move(other.stop_handler_)),
      clients_(std::move(other.clients_)) {
    other.listen_fd_ = -1;
    other.scheduler_ = nullptr;
    other.dispatcher_ = nullptr;
    other.log_ = nullptr;
    other.channel_factory_ = nullptr;
    other.auth_gate_factory_ = nullptr;
    other.clients_.clear();
}

TcpServer& TcpServer::operator=(TcpServer&& other) noexcept {
    if (this != &other) {
        Detach();
        CloseIfOpen();
        listen_fd_ = other.listen_fd_;
        scheduler_ = other.scheduler_;
        dispatcher_ = other.dispatcher_;
        log_ = other.log_;
        channel_factory_ = std::move(other.channel_factory_);
        auth_gate_factory_ = std::move(other.auth_gate_factory_);
        protocol_ = other.protocol_;
        durability_ = other.durability_;
        identity_source_ = std::move(other.identity_source_);
        next_identity_ = other.next_identity_;
        server_info_ = std::move(other.server_info_);
        stop_handler_ = std::move(other.stop_handler_);
        clients_ = std::move(other.clients_);
        other.listen_fd_ = -1;
        other.scheduler_ = nullptr;
        other.dispatcher_ = nullptr;
        other.log_ = nullptr;
        other.channel_factory_ = nullptr;
        other.auth_gate_factory_ = nullptr;
        other.clients_.clear();
    }
    return *this;
}

TcpServer::~TcpServer() {
    Detach();
    CloseIfOpen();
}

void TcpServer::CloseIfOpen() noexcept {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

Status TcpServer::Attach(sched::Scheduler& scheduler, CommandDispatcher& dispatcher,
                          Logger* log) {
    if (listen_fd_ < 0) {
        return Status::IoError("TcpServer: no listening socket to attach");
    }
    scheduler_ = &scheduler;
    dispatcher_ = &dispatcher;
    log_ = log;

    // Non-blocking from here on: the reactor decides when to wait, and it
    // waits in exactly one place (the io backend). A blocking accept() here
    // would re-create the problem this class exists to solve.
    if (Status s = SetNonBlocking(listen_fd_); !s.ok()) return s;

    if (Status s = scheduler_->RegisterIoHandler(
            listen_fd_, sched::IoInterest::kReadable,
            [this](const sched::IoEvent&) { OnListenerReadable(); });
        !s.ok()) {
        return s;
    }

    // **The portal-idle timeout's caller** (§10, KW-D3). Without one the
    // 60 s bound was a constant and a method with no producer, and the
    // memory a suspended portal holds - which is what the spec's amended §7
    // says the timeout bounds - was bounded by nothing.
    //
    // A sweep rather than a timer per portal: portals are few (64 a
    // session), the sweep is a walk over one map per connection, and a
    // timer per portal would be a registration on every `C_BIND`. The
    // period is a quarter of the timeout, so a portal is released within
    // 25% of the bound rather than at a multiple of it.
    if (protocol_ == Protocol::kKwp) {
        idle_sweep_ = scheduler_->SubmitEvery(kPortalIdleTimeoutNs / 4, [this] {
            for (auto& [fd, conn] : clients_) {
                (void)fd;
                if (conn.kwp.has_value()) conn.kwp->ExpireIdlePortals();
            }
        });
    }
    return Status::OK();
}

void TcpServer::Detach() noexcept {
    if (scheduler_ != nullptr) {
        // Before the handlers, and before the map they walk: a timer left
        // registered would fire against a `clients_` this function is about
        // to empty, on a scheduler the caller is about to destroy.
        if (idle_sweep_ != sched::kInvalidTimerId) {
            scheduler_->CancelTimer(idle_sweep_);
            idle_sweep_ = sched::kInvalidTimerId;
        }
        // Copied first: CloseClient mutates clients_.
        std::vector<int> fds;
        fds.reserve(clients_.size());
        for (const auto& [fd, conn] : clients_) fds.push_back(fd);
        for (int fd : fds) CloseClient(fd);

        if (listen_fd_ >= 0) {
            (void)scheduler_->UnregisterIoHandler(listen_fd_);
        }
        scheduler_ = nullptr;
        dispatcher_ = nullptr;
    }
    clients_.clear();
}

void TcpServer::OnListenerReadable() {
    // One accept per event, not an EAGAIN drain loop: the backend is
    // level-triggered (epoll_io_backend.hpp), so a still-pending
    // connection is reported again next iteration. Bounded work per
    // handler is what keeps one busy socket from starving the timers.
    int client_fd = ::accept(listen_fd_, nullptr, nullptr);
    if (client_fd < 0) return;  // EAGAIN, EINTR, or a broken listener

    if (!SetNonBlocking(client_fd).ok()) {
        ::close(client_fd);
        return;
    }

    // TCP_NODELAY, unconditionally. Without it Nagle holds a small reply
    // until the previous one is ACKed, and a client that has several
    // requests in flight is not sending anything to carry that ACK - so it
    // waits out the peer's delayed-ACK timer, ~40ms, once per batch. That
    // turned pipelining from the fastest way to talk to this server into
    // 30x slower than one-request-at-a-time. Replies are small and
    // request/response is the whole protocol; there is nothing here for
    // Nagle to coalesce that the outbox does not already coalesce better.
    int nodelay = 1;
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Stamped with the server's configured level, so `isolation` in the
    // config file is what a fresh connection actually starts at rather than
    // the compiled-in default.
    Connection fresh;
    if (dispatcher_ != nullptr) fresh.session = Session(dispatcher_->default_isolation());
    if (channel_factory_) {
        fresh.channel = channel_factory_();
        // Fail closed: a factory that could not produce a channel must
        // not produce a *plaintext* connection on a port that promised a
        // transform. No channel, no connection.
        if (fresh.channel == nullptr) {
            ::close(client_fd);
            return;
        }
    }
    if (auth_gate_factory_) {
        fresh.auth_gate = auth_gate_factory_();
        // The same fail-closed rule: no gate on a port that promised
        // one means no connection, never an unauthenticated one.
        if (fresh.auth_gate == nullptr) {
            ::close(client_fd);
            return;
        }
        // Defense in depth: a gated connection starts at the floor, so
        // "never admin unless the gate said so" is a structural property
        // of this line, not an argument about the drain loop's control
        // flow. Unreachable today - the gate absorbs every pre-auth
        // line - and cheap to make untrue-by-construction anyway.
        fresh.session.set_role(Role::kReadOnly);
    }
    auto [entry, inserted] = clients_.emplace(client_fd, std::move(fresh));
    if (protocol_ == Protocol::kKwp) {
        Connection& conn = entry->second;
        wire::HandshakeConfig config;
        config.server_info = server_info_;
        config.tls_active = conn.channel != nullptr;
        config.capabilities = wire::kServerCapabilities;
        conn.kwp.emplace(conn.session, config, durability_);
        conn.kwp->set_identity(NextIdentity(), NextIdentity());
        // The gate moves into the protocol session: a KWP connection's
        // exchange runs in `C_AUTH` frames, not in lines, and `auth.hpp`'s
        // gate is the same object either way ("only this line framing is
        // protocol-specific"). Moved rather than shared, so there is one
        // owner and `conn.auth_gate` stays the text path's alone.
        if (conn.auth_gate != nullptr) conn.kwp->set_auth_gate(std::move(conn.auth_gate));
    }
    if (logging(LogLevel::kDebug)) {
        log_->Debug("client", "accepted fd=" + std::to_string(client_fd) +
                                  " open_connections=" + std::to_string(clients_.size()));
    }
    Status s = scheduler_->RegisterIoHandler(
        client_fd, sched::IoInterest::kReadable,
        [this, client_fd](const sched::IoEvent& event) { OnClientEvent(client_fd, event); });
    if (!s.ok()) {
        clients_.erase(client_fd);
        ::close(client_fd);
    }
}

void TcpServer::OnClientEvent(int client_fd, const sched::IoEvent& event) {
    // Writable first: draining the backlog is what frees the outbox for
    // whatever this read is about to produce, and a connection with a
    // reply tail pending has already been told to expect this event.
    if (event.writable) {
        auto it = clients_.find(client_fd);
        if (it == clients_.end()) return;
        if (!FlushOutbox(client_fd, it->second)) return;  // closed
        SyncWriteInterest(client_fd, it->second);
    }
    if (event.readable) OnClientReadable(client_fd);
}

void TcpServer::OnClientReadable(int client_fd) {
    auto it = clients_.find(client_fd);
    if (it == clients_.end()) return;  // closed earlier this iteration

    char chunk[4096];
    ssize_t n = ::read(client_fd, chunk, sizeof(chunk));
    if (n == 0) {
        CloseClient(client_fd);  // orderly hangup
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        CloseClient(client_fd);
        return;
    }

    Connection& conn = it->second;
    // Pre-auth, the inbox is capped: an anonymous peer that streams bytes
    // without ever completing a line - or an exchange - must not grow
    // server memory at will. SCRAM lines are a few hundred bytes; 4 KiB
    // is generous, and an authenticated connection is uncapped as before.
    // (A pre-auth *deadline* needs a timer and is future hardening.)
    constexpr std::size_t kPreAuthInboxCap = 4096;
    // **"Pre-auth" is not `auth_gate != nullptr` any more.** A KWP
    // connection's gate is *moved* into its session at accept, so that
    // pointer is null on every one of them and the cap never fired - an
    // anonymous peer could push a 16 MiB frame at the decoder where a text
    // client was held to 4 KiB. The condition is now "this connection has
    // not finished its handshake", which is what the cap always meant.
    const bool pre_auth = conn.auth_gate != nullptr ||
                          (conn.kwp.has_value() && !conn.kwp->handshake_done());
    if (pre_auth && conn.inbox.size() + static_cast<std::size_t>(n) > kPreAuthInboxCap) {
        if (logging(LogLevel::kInfo)) {
            log_->Info("client", "fd=" + std::to_string(client_fd) +
                                     " overflowed the pre-auth budget; closing");
        }
        CloseClient(client_fd);
        return;
    }
    if (conn.channel != nullptr) {
        // Wire bytes go through the channel: decrypted command bytes land
        // in the inbox, and whatever the channel must say back (handshake
        // replies, alerts) lands in the outbox ahead of any statement
        // reply. A failed transform is fatal to the connection - flush
        // what the channel managed to append (typically its alert),
        // best effort, and close.
        //
        // Fatal *once*. When that close could not happen there and then
        // because a statement was still running, CloseClient only marked
        // the connection (conn.closing) and the fd stays readable until
        // the statement finishes - so a peer that keeps sending would
        // re-enter a channel that has already reported a fatal error,
        // which its contract does not survive. Drop those bytes instead.
        if (conn.closing) return;
        Status s = conn.channel->OnWireData(std::string_view(chunk, static_cast<std::size_t>(n)),
                                            conn.inbox, conn.outbox);
        if (!s.ok()) {
            if (logging(LogLevel::kInfo)) {
                log_->Info("client", "fd=" + std::to_string(client_fd) +
                                         " TLS failure: " + s.message());
            }
            (void)FlushOutbox(client_fd, conn);
            CloseClient(client_fd);
            return;
        }
    } else {
        conn.inbox.append(chunk, static_cast<std::size_t>(n));
    }
    if (protocol_ == Protocol::kKwp) {
        if (!DrainFrames(client_fd, conn)) return;
    } else if (!DrainCommands(client_fd, conn)) {
        return;  // closed or server stopping
    }
    if (!FlushOutbox(client_fd, conn)) return;
    SyncWriteInterest(client_fd, conn);
}

std::uint64_t TcpServer::NextIdentity() {
    if (identity_source_) return identity_source_();
    // splitmix64 over a counter: distinct and well-spread, and **not a
    // secret** - which is why the `CANCEL` capability is withheld when this
    // is what is minting keys. Distinctness is all the session id needs.
    std::uint64_t z = (next_identity_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

bool TcpServer::FlushFrames(int client_fd, Connection& conn) {
    if (conn.frames_out.empty()) return true;
    const std::string_view bytes(reinterpret_cast<const char*>(conn.frames_out.data()),
                                 conn.frames_out.size());
    // **Nothing touches `conn` after a false.** `AppendReplyBytes` closes
    // the connection on a channel failure, and `CloseClient` erases the map
    // entry whenever no statement is in flight - which is every caller of
    // this function. Clearing the buffer afterwards wrote into the freed
    // Connection.
    if (!AppendReplyBytes(client_fd, conn, bytes)) return false;
    conn.frames_out.clear();
    return true;
}

bool TcpServer::DrainFrames(int client_fd, Connection& conn) {
    // The same loop shape and the same one-at-a-time rule as
    // `DrainCommands`: a frame that dispatches returns, and
    // `OnStatementComplete` comes back here. Pipelining still works -
    // PARSE/BIND/EXECUTE arrive in one read and are handled in order -
    // and it is only *concurrency within one connection* that is excluded,
    // which §5's `C_SYNC` barrier already assumes.
    if (conn.closing) return true;

    // The inbox holds frame bytes on this path. Handed to the decoder
    // whole and cleared, because the decoder does its own accumulation
    // across chunk boundaries (kwp.hpp).
    if (!conn.inbox.empty()) {
        const auto* p = reinterpret_cast<const std::byte*>(conn.inbox.data());
        Status fed = conn.decoder.Feed(std::span<const std::byte>(p, conn.inbox.size()));
        conn.inbox.clear();
        if (!fed.ok()) {
            // §2: framing-level corruption where resync is impossible ends
            // the connection. The error frame is best effort - the peer
            // that sent a bad length may not be reading.
            const wire::WireError e = wire::ProtocolError(
                wire::ProtocolDetail::kMalformedFrame, fed.message(), wire::Severity::kFatal);
            const auto frame = wire::EncodeFrame(
                static_cast<std::uint8_t>(wire::ServerFrameType::kError), 0,
                wire::EncodeError(e));
            // Same rule as `FlushFrames`: a failed append has already
            // closed and erased the connection, so the flush and the close
            // below must not run against it.
            if (!AppendReplyBytes(
                    client_fd, conn,
                    std::string_view(reinterpret_cast<const char*>(frame.data()),
                                     frame.size()))) {
                return false;
            }
            (void)FlushOutbox(client_fd, conn);
            CloseClient(client_fd);
            return false;
        }
    }

    while (!conn.in_flight && !conn.closing) {
        auto frame = conn.decoder.PopFrame();
        if (!frame.has_value()) return true;

        FrameAction action = conn.kwp->OnFrame(*frame, conn.frames_out);
        if (!action.dispatch) {
            if (!FlushFrames(client_fd, conn)) return false;
            if (action.close) {
                (void)FlushOutbox(client_fd, conn);
                CloseClient(client_fd);
                return false;
            }
            continue;
        }
        if (dispatcher_ == nullptr || scheduler_ == nullptr) return true;

        // The statement text has to outlive the coroutine: the parser's
        // tokens view it (`DispatchAsync`'s contract), and the portal that
        // owns it may be closed while the statement runs.
        conn.current_line = std::move(action.sql);
        conn.session.set_result_sink(action.sink);
        conn.in_flight = true;
        scheduler_->Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground,
            dispatcher_->DispatchAsync(conn.current_line, &conn.session, &conn.pending),
            [this, client_fd](const Status&) { OnStatementComplete(client_fd); }));
        return true;
    }
    return true;
}

bool TcpServer::FlushOutbox(int client_fd, Connection& conn) {
    std::size_t sent = 0;
    while (sent < conn.outbox.size()) {
        // **send() with MSG_NOSIGNAL, not write().** Writing to a socket the
        // peer has closed raises SIGPIPE, whose default action is to
        // terminate the process - so a client that hangs up without reading
        // its reply could kill the server. MSG_NOSIGNAL turns that into a
        // plain EPIPE, which the error path below already handles.
        //
        // The hole predates the async dispatch seam and was simply hard to
        // hit: one write() per readable event gave a client a narrow window
        // to disappear in. A reply per statement completion widened it
        // enough that a pipelined client hanging up killed the process every
        // time, which is how it was found.
        ssize_t n = ::send(client_fd, conn.outbox.data() + sent, conn.outbox.size() - sent,
                           MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;  // send buffer full; the rest waits for a writable event
        }
        if (n < 0 && errno == EINTR) continue;
        conn.outbox.clear();
        CloseClient(client_fd);
        return false;
    }
    conn.outbox.erase(0, sent);
    return true;
}

void TcpServer::SyncWriteInterest(int client_fd, Connection& conn) {
    const bool want = !conn.outbox.empty();
    if (want == conn.want_writable) return;  // epoll already says the right thing
    if (scheduler_ == nullptr) return;

    auto interest = want ? static_cast<sched::IoInterest>(
                               static_cast<std::uint8_t>(sched::IoInterest::kReadable) |
                               static_cast<std::uint8_t>(sched::IoInterest::kWritable))
                         : sched::IoInterest::kReadable;
    if (scheduler_->ModifyIoHandler(client_fd, interest).ok()) {
        conn.want_writable = want;
    }
}

bool TcpServer::DrainCommands(int client_fd, Connection& conn) {
    // A loop, and the only thing that goes round it is the gate below: a
    // statement returns after starting one, so the iteration count is a
    // *pre-auth* peer's to choose. Tail recursion here would let one
    // 4 KiB read of newlines from an anonymous client cost 4096 stack
    // frames, which is a stack overflow that has not authenticated.
    //
    // One at a time (tcp_server.hpp's Connection::in_flight): the session is
    // stateful and the protocol has no request ids, so a second statement
    // must not start until this one's reply has been appended.
    while (!conn.in_flight && !conn.closing) {
        const std::size_t nl = conn.inbox.find('\n');
        if (nl == std::string::npos) return true;  // no complete command yet

        conn.current_line.assign(conn.inbox, 0, nl);
        if (!conn.current_line.empty() && conn.current_line.back() == '\r') {
            conn.current_line.pop_back();  // tolerate CRLF clients
        }
        // Erased now, not on completion: the statement runs against
        // `current_line`, and leaving its bytes in the inbox would mean the
        // next read had to reason about which prefix was already taken.
        conn.inbox.erase(0, nl + 1);

        // The request as the client sent it, before anything interprets it.
        // Trace, because it is one line per command and it echoes whatever the
        // client typed - including anything they should not have typed. The
        // exception is a pre-auth line: it carries the SCRAM exchange (a
        // claimed username, a proof), which no log level is entitled to.
        if (logging(LogLevel::kTrace)) {
            log_->Trace("client", "fd=" + std::to_string(client_fd) + " request \"" +
                                      (conn.auth_gate != nullptr
                                           ? std::string("<pre-auth line, redacted>")
                                           : conn.current_line) +
                                      "\"");
        }

        // The authentication gate: an unauthenticated connection's lines go
        // to the gate, never the dispatcher - STOP included, because an
        // admin command an anonymous peer can run is not an admin command.
        // Handled synchronously (SCRAM is a few HMACs, no I/O) and drained
        // on, so pipelined AUTH lines behave like pipelined statements.
        if (conn.auth_gate != nullptr) {
            AuthGate::Result result = conn.auth_gate->OnLine(conn.current_line);
            if (!AppendReplyLine(client_fd, conn, std::move(result.reply))) return false;
            if (result.authenticated) {
                conn.auth_gate.reset();  // authenticated = the gate's absence
                // The one hand-over of identity: the gate is the only
                // code that learns who got in, the session is the only
                // place the dispatcher looks.
                conn.session.set_role(result.role);
                if (logging(LogLevel::kDebug)) {
                    log_->Debug("client", "fd=" + std::to_string(client_fd) +
                                              " authenticated as '" + result.username +
                                              "' role=" + std::string(RoleName(result.role)));
                }
            }
            if (result.close) {
                if (logging(LogLevel::kInfo)) {
                    log_->Info("client", "fd=" + std::to_string(client_fd) +
                                             " failed authentication; closing");
                }
                (void)FlushOutbox(client_fd, conn);
                CloseClient(client_fd);
                return false;
            }
            continue;  // the next pipelined line
        }

        if (dispatcher_ == nullptr || scheduler_ == nullptr) return true;

        conn.in_flight = true;
        scheduler_->Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground,
            dispatcher_->DispatchAsync(conn.current_line, &conn.session, &conn.pending),
            [this, client_fd](const Status&) { OnStatementComplete(client_fd); }));
        return true;
    }
    return true;
}

void TcpServer::OnStatementComplete(int client_fd) {
    auto it = clients_.find(client_fd);
    if (it == clients_.end()) return;  // nothing left to reply to
    Connection& conn = it->second;
    conn.in_flight = false;
    // Cleared whatever happens next: the sink belongs to the portal that
    // asked for it, and a portal closed while its statement ran must not
    // leave the next statement encoding into a dead one.
    conn.session.set_result_sink(nullptr);

    if (conn.kwp.has_value()) {
        if (conn.closing) {
            conn.closing = false;
            CloseClient(client_fd);
            return;
        }
        const bool stop = conn.pending.should_stop;
        conn.kwp->OnStatementComplete(conn.pending, conn.frames_out);
        conn.pending = DispatchOutcome{};
        if (!FlushFrames(client_fd, conn)) return;
        if (stop) {
            (void)FlushOutbox(client_fd, conn);
            if (stop_handler_) {
                stop_handler_();
            } else if (scheduler_ != nullptr) {
                scheduler_->Stop();
            }
            return;
        }
        if (!DrainFrames(client_fd, conn)) return;
        if (!FlushOutbox(client_fd, conn)) return;
        SyncWriteInterest(client_fd, conn);
        return;
    }

    // The connection went away while the statement ran (Connection::closing).
    // Its reply has nowhere to go, and *now* it is safe to destroy the
    // session the statement was holding.
    if (conn.closing) {
        conn.closing = false;
        CloseClient(client_fd);
        return;
    }

    // Appended, not written: one readable event can carry a whole batch of
    // pipelined commands, and a write() per command is a syscall per command
    // plus a separate small segment per command on the wire. The batch leaves
    // in one write() below.
    const bool stop = conn.pending.should_stop;
    std::string response = std::move(conn.pending.response);
    conn.pending = DispatchOutcome{};
    if (!AppendReplyLine(client_fd, conn, std::move(response))) return;

    if (stop) {
        // Best effort, exactly as the per-command write() it replaces was:
        // the socket is non-blocking, so a full send buffer loses the
        // goodbye. Nothing downstream depends on the client seeing it, and
        // the alternative is blocking the reactor on shutdown.
        (void)FlushOutbox(client_fd, conn);
        if (logging(LogLevel::kInfo)) {
            log_->Info("client", "STOP from fd=" + std::to_string(client_fd) +
                                     "; shutting the server down");
        }
        // Stops the reactor, not just this connection - STOP has always
        // meant the whole server. The connection is closed first so the
        // client sees the reply land and the socket shut, rather than
        // waiting on a process that is already tearing down.
        CloseClient(client_fd);
        if (stop_handler_) {
            stop_handler_();
        } else if (scheduler_ != nullptr) {
            scheduler_->Stop();
        }
        return;
    }

    // The next pipelined command, then the batch's replies in one write.
    if (!DrainCommands(client_fd, conn)) return;
    if (!FlushOutbox(client_fd, conn)) return;
    SyncWriteInterest(client_fd, conn);
}

bool TcpServer::AppendReplyBytes(int client_fd, Connection& conn, std::string_view bytes) {
    if (conn.channel != nullptr) {
        Status s = conn.channel->Send(bytes, conn.outbox);
        if (!s.ok()) {
            (void)FlushOutbox(client_fd, conn);
            CloseClient(client_fd);
            return false;
        }
    } else {
        conn.outbox.append(bytes);
    }
    return true;
}

bool TcpServer::AppendReplyLine(int client_fd, Connection& conn, std::string reply) {
    // The newline goes onto the reply first so a wire channel seals one
    // record per reply, not a one-byte second record.
    reply.push_back('\n');
    return AppendReplyBytes(client_fd, conn, reply);
}

void TcpServer::CloseClient(int client_fd) {
    // One lookup for the whole teardown; a re-entrant call (FlushOutbox's
    // error path arrives here from inside this function's own send) finds
    // the entry already erased and stops at the door.
    auto it = clients_.find(client_fd);
    if (it == clients_.end()) return;
    Connection& conn = it->second;

    // **Deferred while a statement is running.** The coroutine holds a
    // pointer to this connection's session and writes its reply into
    // this connection's buffer, so destroying it now would pull both out
    // from under a task that is still on a ready queue.
    //
    // Marked instead, and torn down by OnStatementComplete. Cancelling
    // the statement would be better and needs cancellation the engine
    // does not have; waiting for it is bounded by the statement's own
    // row-touch budget (exec/budget.hpp), which is what stops a hung
    // client from pinning a connection forever.
    if (conn.in_flight) {
        conn.closing = true;
        return;
    }

    // **A connection that goes away rolls back** (docs/spec/txn.md section
    // 10-8). Anything else would leave an open transaction holding its
    // writes and its place in every other session's in-flight set, with
    // nobody left to end it. The dispatcher cannot reach clients_, so
    // `it` survives the call.
    if (dispatcher_ != nullptr && conn.session.in_explicit_txn()) {
        if (logging(LogLevel::kInfo)) {
            log_->Info("client", "fd=" + std::to_string(client_fd) +
                                     " closed with a transaction open; rolling it back");
        }
        (void)dispatcher_->Dispatch("ROLLBACK", &conn.session);
    }

    // The orderly TLS goodbye (close_notify), appended to the outbox and
    // sent behind whatever is still queued there. Order is the point: the
    // outbox holds wire bytes the socket has not taken yet, quite possibly
    // the tail of a half-sent record, and splicing close_notify in front
    // of them hands the peer a corrupt record instead of a clean goodbye.
    // One best-effort send, directly rather than through FlushOutbox -
    // this is teardown, and FlushOutbox's error path calls back into
    // CloseClient.
    if (conn.channel != nullptr) {
        conn.channel->Close(conn.outbox);
        if (!conn.outbox.empty()) {
            (void)::send(client_fd, conn.outbox.data(), conn.outbox.size(), MSG_NOSIGNAL);
        }
    }
    clients_.erase(it);
    if (logging(LogLevel::kDebug)) {
        log_->Debug("client", "closed fd=" + std::to_string(client_fd) +
                                  " open_connections=" + std::to_string(clients_.size()));
    }
    if (scheduler_ != nullptr) {
        (void)scheduler_->UnregisterIoHandler(client_fd);
    }
    ::close(client_fd);
}

}  // namespace kds::server
