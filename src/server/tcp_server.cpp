#include "kds/server/tcp_server.hpp"

#include "kds/sched/core_load.hpp"
#include "kds/sched/coro.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fcntl.h>

#include <algorithm>
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

Status ConfigureAcceptedSocket(int fd, std::uint32_t keepalive_s) {
    // TCP_NODELAY, unconditionally. Without it Nagle holds a small reply
    // until the previous one is ACKed, and a client that has several
    // requests in flight is not sending anything to carry that ACK - so it
    // waits out the peer's delayed-ACK timer, ~40ms, once per batch - which
    // made pipelining slower than one request at a time. The outbox already
    // coalesces what Nagle would.
    const int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // **Keepalive** (CN-10 §3.3, U7). A peer that vanished without a FIN -
    // power lost, a cable pulled - leaves a connection nothing else ends,
    // since no idle-session timeout exists (protocol.md §10). One number is
    // configured and the rest follow from it: six probes a sixth of the idle
    // apart, and `TCP_USER_TIMEOUT` at the same total. With the user
    // timeout set the kernel ends the connection once that long has passed
    // without an acknowledgement - which reaps a dead peer at about twice
    // the idle instead of `tcp_retries2`'s ~15 minutes when data was in
    // flight, and **also ends a live client that leaves a reply unread**
    // (a zero window) that long: the price, stated in the manual.
    if (keepalive_s == 0) return Status::OK();
    const int idle_s = static_cast<int>(keepalive_s);
    constexpr int kProbes = 6;
    const int interval = std::max(1, idle_s / kProbes);
    const unsigned user_timeout_ms =
        static_cast<unsigned>(idle_s + interval * kProbes) * 1000u;
    const int on = 1;
    const auto set = [fd](int level, int name, const void* value, socklen_t len) {
        return ::setsockopt(fd, level, name, value, len) == 0;
    };
    if (!set(SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) ||
        !set(IPPROTO_TCP, TCP_KEEPIDLE, &idle_s, sizeof(idle_s)) ||
        !set(IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) ||
        !set(IPPROTO_TCP, TCP_KEEPCNT, &kProbes, sizeof(kProbes)) ||
        !set(IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout_ms, sizeof(user_timeout_ms))) {
        return Status::IoError(std::string("keepalive setsockopt failed: ") +
                               std::strerror(errno));
    }
    return Status::OK();
}

bool TcpServer::IsListenAddress(const std::string& address) {
    in_addr parsed{};
    return ::inet_pton(AF_INET, address.c_str(), &parsed) == 1;
}

StatusOr<TcpServer> TcpServer::Listen(std::uint16_t port, bool reuse_port,
                                      const std::string& address) {
    in_addr bind_addr{};
    if (::inet_pton(AF_INET, address.c_str(), &bind_addr) != 1) {
        return Status::InvalidArgument("listen address '" + address +
                                       "' is not an IPv4 address");
    }
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
            // rather than what it is. Unsupported, because it is the
            // platform's answer and not an I/O failure: the caller's cue
            // for D19's fallback (AT-S10c).
            Status s =
                Status::Unsupported(std::string("setsockopt(SO_REUSEPORT) failed: ") +
                                    std::strerror(errno));
            ::close(fd);
            return s;
        }
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr = bind_addr;
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
    if (::listen(fd, kListenBacklog) < 0) {
        Status s = Status::IoError(std::string("listen() failed: ") + std::strerror(errno));
        ::close(fd);
        return s;
    }

    return TcpServer(fd);
}

TcpServer::TcpServer(TcpServer&& other) noexcept
    : listen_fd_(other.listen_fd_),
      hosting_(other.hosting_),
      handoff_(other.handoff_),
      mover_(other.mover_),
      placer_(other.placer_),
      host_(std::move(other.host_)),
      scheduler_(other.scheduler_),
      dispatcher_(other.dispatcher_),
      log_(other.log_),
      channel_factory_(std::move(other.channel_factory_)),
      auth_gate_factory_(std::move(other.auth_gate_factory_)),
      // **The configuration moves with the socket.** A listener is
      // configured and *then* moved into the optional that owns it, so a
      // member left out here is a setting silently dropped.
      protocol_(other.protocol_),
      durability_(other.durability_),
      keepalive_s_(other.keepalive_s_),
      identity_source_(std::move(other.identity_source_)),
      cancels_(other.cancels_),
      next_identity_(other.next_identity_),
      server_info_(std::move(other.server_info_)),
      stop_handler_(std::move(other.stop_handler_)),
      txn_lifetime_ceiling_ns_(other.txn_lifetime_ceiling_ns_),
      clients_(std::move(other.clients_)),
      reply_writes_(other.reply_writes_) {
    if (host_ != nullptr) host_->server = this;
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
        hosting_ = other.hosting_;
        handoff_ = other.handoff_;
        mover_ = other.mover_;
        placer_ = other.placer_;
        host_ = std::move(other.host_);
        if (host_ != nullptr) host_->server = this;
        scheduler_ = other.scheduler_;
        dispatcher_ = other.dispatcher_;
        log_ = other.log_;
        channel_factory_ = std::move(other.channel_factory_);
        auth_gate_factory_ = std::move(other.auth_gate_factory_);
        protocol_ = other.protocol_;
        durability_ = other.durability_;
        keepalive_s_ = other.keepalive_s_;
        identity_source_ = std::move(other.identity_source_);
        cancels_ = other.cancels_;
        next_identity_ = other.next_identity_;
        reply_writes_ = other.reply_writes_;
        server_info_ = std::move(other.server_info_);
        stop_handler_ = std::move(other.stop_handler_);
        txn_lifetime_ceiling_ns_ = other.txn_lifetime_ceiling_ns_;
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
    if (listen_fd_ < 0 && !hosting_) {
        return Status::IoError("TcpServer: no listening socket to attach");
    }
    scheduler_ = &scheduler;
    dispatcher_ = &dispatcher;
    log_ = log;

    // Non-blocking from here on: the reactor decides when to wait, and it
    // waits in exactly one place (the io backend).
    if (listen_fd_ >= 0) {
        if (Status s = SetNonBlocking(listen_fd_); !s.ok()) return s;
        if (Status s = scheduler_->RegisterIoHandler(
                listen_fd_, sched::IoInterest::kReadable,
                [this](const sched::IoEvent&) { OnListenerReadable(); });
            !s.ok()) {
            return s;
        }
    }

    // **The portal-idle timeout's caller** (§10, KW-D3): what bounds the
    // memory a suspended portal holds (§7).
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

    // **The transaction lifetime ceiling's caller** (AN-R14, txn.md §1), on
    // both protocols. A quarter of the ceiling for the portal sweep's
    // reason, and never under a millisecond, so a ceiling of a few
    // milliseconds cannot turn the timer into a spin.
    constexpr sched::MonoTimeNs kMinTxnSweepPeriodNs = 1'000'000;
    txn_sweep_ = scheduler_->SubmitEvery(
        std::max(txn_lifetime_ceiling_ns_ / 4, kMinTxnSweepPeriodNs),
        [this] { ExpireOverdueTransactions(); });
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
        if (txn_sweep_ != sched::kInvalidTimerId) {
            scheduler_->CancelTimer(txn_sweep_);
            txn_sweep_ = sched::kInvalidTimerId;
        }
        // Copied first: CloseClient mutates clients_.
        std::vector<int> fds;
        fds.reserve(clients_.size());
        for (const auto& [fd, conn] : clients_) fds.push_back(fd);
        for (int fd : fds) CloseClient(fd);

        if (listen_fd_ >= 0) {
            (void)scheduler_->UnregisterIoHandler(listen_fd_);
        }
        // `Host`'s task ends at its next wake rather than adopting into a
        // server that is gone - and the state is dropped, so a later move of
        // this detached server cannot re-point the task at it.
        if (host_ != nullptr) {
            host_->server = nullptr;
            host_.reset();
        }
        // What `CloseClient` could not erase (a statement still in flight)
        // leaves the count with the map below.
        CountSession(-static_cast<int>(clients_.size()));
        scheduler_ = nullptr;
        dispatcher_ = nullptr;
    }
    // Every session leaves the registry with its connection, including one
    // whose statement was still in flight and so never reached `CloseClient`.
    for (auto& [fd, conn] : clients_) UnregisterCancel(conn);
    clients_.clear();
}

void TcpServer::UnregisterCancel(Connection& conn) {
    if (!conn.cancel_registered) return;
    cancels_->Unregister(conn.kwp->session_id());
    conn.cancel_registered = false;
}

void TcpServer::OnListenerReadable() {
    // Up to `kMaxAcceptsPerEvent` accepts, not an EAGAIN drain loop: the
    // backend is level-triggered (epoll_io_backend.hpp), so what is left
    // is reported again next iteration. Bounded work per handler is what
    // keeps a burst of connections from starving the timers.
    for (int i = 0; i < kMaxAcceptsPerEvent; ++i) {
        int client_fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) return;  // EAGAIN, EINTR, or a broken listener
        SetUpAccepted(client_fd);
    }
}

void TcpServer::SetUpAccepted(int client_fd) {
    // Fail closed, like the channel and the gate below: the operator asked
    // for vanished peers to be reaped, and a connection that cannot be is
    // one nothing would ever end.
    if (Status s = ConfigureAcceptedSocket(client_fd, keepalive_s_); !s.ok()) {
        if (logging(LogLevel::kWarn)) log_->Warn("client", s.message());
        ::close(client_fd);
        return;
    }

    // D19's fallback (AT-S10c): the socket is set up for whichever core
    // runs it, and one routed elsewhere is that core's from here on.
    if (handoff_ != nullptr) {
        const std::uint32_t core = handoff_->NextCore();
        if (core != 0) {
            handoff_->Offer(core, client_fd);
            return;
        }
    }
    AdoptConnection(client_fd);
}

void TcpServer::AdoptConnection(int client_fd) {
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
        // flow (the gate absorbs every pre-auth line).
        fresh.session.set_role(Role::kReadOnly);
    }
    auto [entry, inserted] = clients_.emplace(client_fd, std::move(fresh));
    if (protocol_ == Protocol::kKwp) {
        Connection& conn = entry->second;
        wire::HandshakeConfig config;
        config.server_info = server_info_;
        config.tls_active = conn.channel != nullptr;
        // **`CANCEL` is offered where a cancel can be authorized** (§10,
        // BA-S15): a registry to match in and an identity source whose keys
        // are secrets. The default counter's are not.
        const bool cancellable = cancels_ != nullptr && identity_source_;
        config.capabilities = wire::kServerCapabilities |
                              (cancellable ? static_cast<std::uint64_t>(wire::Capability::kCancel)
                                           : std::uint64_t{0});
        // The reactor's clock, which is what the idle sweep above runs on:
        // a session built without one stamps no portal and expires none.
        conn.kwp.emplace(conn.session, config, durability_, &scheduler_->clock());
        conn.kwp->set_identity(NextIdentity(), NextIdentity());
        // A source that failed answers 0, which is no secret: such a
        // session is offered the bit and is simply never matched. The
        // registry is still this connection's to match a `C_CANCEL` in.
        if (cancellable) {
            conn.kwp->set_cancel_registry(cancels_);
            if (conn.kwp->session_id() != 0 && conn.kwp->cancel_key() != 0) {
                cancels_->Register(conn.kwp->session_id(), conn.kwp->cancel_key(),
                                   conn.session.cancel_flag());
                conn.cancel_registered = true;
            }
        }
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
        return;
    }
    CountSession(+1);
}

void TcpServer::CountSession(int delta) noexcept {
    // Every listener on the reactor shares the core's slot, so a count
    // moves by its delta rather than being set to one listener's size. At
    // one core there is no other core to read it (BL-R9).
    if (!scheduler_->wake_armed()) return;
    std::atomic<std::uint32_t>& n = sched::CoreLoad::Of(scheduler_->core_id()).sessions;
    n.store(static_cast<std::uint32_t>(static_cast<int>(n.load(std::memory_order_relaxed)) + delta),
            std::memory_order_relaxed);
}

Status TcpServer::Host(ConnectionHandoff& handoff, std::uint32_t core) {
    if (scheduler_ == nullptr) return Status::InvalidArgument("TcpServer::Host before Attach");
    if (core >= handoff.cores()) {
        return Status::InvalidArgument("TcpServer::Host: core " + std::to_string(core) +
                                       " is past the handoff's " +
                                       std::to_string(handoff.cores()));
    }
    // One task per server: D19's fallback hosts the inbox already where
    // BL-S2's moves ask for it again.
    if (host_ != nullptr) return Status::OK();
    host_ = std::make_shared<HostState>();
    host_->server = this;
    scheduler_->Submit(sched::MakeCoroTask(sched::SchedulingGroup::kSystem,
                                           RunHost(host_, handoff, core)));
    return Status::OK();
}

sched::Coro TcpServer::RunHost(std::shared_ptr<HostState> state, ConnectionHandoff& handoff,
                               std::uint32_t core) {
    // A frame local: the wait holds a pointer to it only while this frame
    // is suspended, which is while the frame exists.
    const std::function<bool()> ready = [s = state.get(), &handoff, core] {
        return s->server == nullptr || handoff.Pending(core);
    };
    while (true) {
        // Level-triggered (AR0-6-R1): the reactor re-polls this after every
        // block, so the kick an offer sends only has to end the block.
        co_await sched::WaitUntil{&ready};
        if (state->server == nullptr) co_return Status::OK();
        for (ConnectionHandoff::Arrival& a : handoff.Take(core)) {
            if (a.moved != nullptr) {
                state->server->AdoptMoved(std::move(a.moved));
            } else {
                state->server->AdoptConnection(a.fd);
            }
        }
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
    // **"Pre-auth" is "has not finished its handshake", not
    // `auth_gate != nullptr`**: a KWP connection's gate is *moved* into its
    // session at accept, so that pointer is null on every one of them.
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
    // **A statement this read started replies at its completion, in one
    // write with everything queued ahead of it** (BN-R7,
    // `workorder-bn-opt-read-update-path.md`). A pipelined
    // PARSE/BIND/EXECUTE leaves `S_PARSE_OK` and `S_BIND_OK` in the outbox
    // when the statement is submitted; flushing them here was a second
    // `send()` per statement and a second wake of a client that had nothing
    // to do with them but read on (BN-S1's census ranked it first). Nothing
    // waits on those frames: a client that wanted them before executing
    // would not have sent the EXECUTE - one that blocks on them while its
    // other connection holds a lock this statement waits for is broken by
    // the lock family's 1 s fault net, as any such cycle is.
    // `OnStatementComplete` flushes the whole outbox.
    if (conn.in_flight) return;
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
    // entry whenever no statement is in flight - every caller but
    // `ShipRunning`, which returns at a false all the same.
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
        const bool head = !conn.session.in_explicit_txn();
        if (scheduler_->wake_armed()) {
            sched::CoreLoad::Slot& load = sched::CoreLoad::Of(scheduler_->core_id());
            sched::CoreLoad::Bump(head ? load.heads : load.inside);
            const std::uint64_t window = load.window_end_ns.load(std::memory_order_relaxed);
            if (conn.session.counted_in_window() != window) {
                conn.session.set_counted_in_window(window);
                load.active_now.store(load.active_now.load(std::memory_order_relaxed) + 1,
                                      std::memory_order_relaxed);
            }
        }
        // **The head of a transaction is where it is placed** (BL-R1): no
        // transaction is open and no statement of this connection runs, so
        // nothing of it is this core's - and the whole `KwpSession` moves,
        // portals, pipelined frames and reply tail with it.
        if (head && placer_ != nullptr) {
            const std::uint32_t here = scheduler_->core_id();
            const std::uint32_t there = placer_->Place(here, conn.session);
            if (there != here && there < mover_->cores()) {
                // A finished statement's reply leaves now, as it would have
                // had the connection stayed (BN-R7 holds back only a
                // pipelined statement's own `S_PARSE_OK`/`S_BIND_OK`).
                if (!FlushOutbox(client_fd, conn)) return false;
                MoveConnection(client_fd, there);
                return false;  // the connection is another core's now
            }
        }
        SubmitStatement(client_fd, conn);
        return true;
    }
    return true;
}

void TcpServer::SubmitStatement(int client_fd, Connection& conn) {
    conn.in_flight = true;
    if (conn.kwp.has_value()) {
        // **Armed here, for every statement** (CE-A3): this is the one
        // place both a frame's dispatch and a moved connection's adoption
        // pass, so the stream always names the core that runs it. Both
        // closures look the connection up by fd: it is a map entry, and
        // `CloseClient` defers its erasure while a statement runs.
        conn.stream.ship = [this, client_fd] { ShipRunning(client_fd); };
        // **The kernel's send buffer is the cap** (CE-A5): the walk goes on
        // while the outbox is empty - everything it shipped was taken by
        // `send()` - and parks while bytes wait for a writable event. A
        // closing connection always resumes it, to its cancel (CE-A6).
        // **Only a write interest is a wait**: bytes BN-R7 holds back - a
        // pipelined `S_PARSE_OK`/`S_BIND_OK` - sit in the outbox with none
        // armed, and a boundary with no sealed batch ships nothing to arm
        // one, so parking on them would wait for an event never coming.
        // **A cancel resumes it too**: `C_CANCEL` arrives on another
        // connection and kicks nothing, so a parked walk reads the flag
        // here - read, not taken - and takes it at the boundary's check.
        conn.stream.resume_when = [this, client_fd] {
            auto it = clients_.find(client_fd);
            if (it == clients_.end()) return true;
            const Connection& c = it->second;
            return c.closing || c.outbox.empty() || !c.want_writable ||
                   c.session.cancel_flag()->load(std::memory_order_acquire);
        };
        conn.kwp->ArmStream(&conn.stream);
    }
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_->DispatchAsync(conn.current_line, &conn.session, &conn.pending),
        [this, client_fd](const Status&) { OnStatementComplete(client_fd); }));
}

void TcpServer::ShipRunning(int client_fd) {
    auto it = clients_.find(client_fd);
    if (it == clients_.end()) return;
    Connection& conn = it->second;
    if (conn.closing) return;  // nobody to read it; the walk ends at its cancel
    // **BN-R7's one write, amended** (CE-R2): what waited for the statement
    // - a pipelined `S_PARSE_OK`/`S_BIND_OK` - leaves with its first rows,
    // in order, and the completion writes the rest as before. A failed
    // write closes the connection the way any reply's does: marked, the
    // walk cancelled, torn down at the statement's end.
    conn.kwp->StreamRunning(conn.frames_out);
    if (!FlushFrames(client_fd, conn)) return;
    if (!FlushOutbox(client_fd, conn)) return;
    SyncWriteInterest(client_fd, conn);
}

// ---- BL-S2: the move -------------------------------------------------------
//
// **Order is the protocol.** This core's reactor stops watching the socket
// before the connection leaves its map, and the destination registers it
// only after taking it: at no moment do two reactors watch one socket, and
// bytes that arrive in between wait in the kernel's buffer, where the
// destination's level-triggered registration reports them. Bytes this core
// already read travel in the connection (`decoder`, `outbox`). The inbox's
// latch is the handoff's happens-before edge (`connection_handoff.hpp`).
//
// **Nothing per core travels.** At a transaction's head the session holds no
// transaction, lock, borrow, view or lease; the clock is the instance's one
// (every core's scheduler is handed it), and the cancel registry and its
// entry, keyed by session id, are the instance's.
void TcpServer::MoveConnection(int client_fd, std::uint32_t core) {
    (void)scheduler_->UnregisterIoHandler(client_fd);
    CountSession(-1);
    sched::CoreLoad::Bump(sched::CoreLoad::Of(scheduler_->core_id()).moves_out);
    mover_->Offer(core, client_fd,
                  std::make_unique<MovedConnection>(clients_.extract(client_fd), cancels_));
}

// **A moved connection always carries the statement whose head moved it**,
// which runs here first.
void TcpServer::AdoptMoved(std::unique_ptr<ConnectionHandoff::Transit> moved) {
    auto& transit = static_cast<MovedConnection&>(*moved);
    const int client_fd = transit.node.key();
    Connection& conn = clients_.insert(std::move(transit.node)).position->second;
    CountSession(+1);
    Status s = scheduler_->RegisterIoHandler(
        client_fd, sched::IoInterest::kReadable,
        [this, client_fd](const sched::IoEvent& event) { OnClientEvent(client_fd, event); });
    if (!s.ok()) {
        conn.session.set_result_sink(nullptr);
        CloseClient(client_fd);
        return;
    }
    SubmitStatement(client_fd, conn);
    // Registered readable only: a reply tail the source could not send
    // (EAGAIN) needs this reactor's writable interest too.
    conn.want_writable = false;
    SyncWriteInterest(client_fd, conn);
}

TcpServer::MovedConnection::~MovedConnection() {
    if (node.empty()) return;  // taken
    Connection& conn = node.mapped();
    if (conn.cancel_registered) cancels->Unregister(conn.kwp->session_id());
    ::close(node.key());
}

bool TcpServer::FlushOutbox(int client_fd, Connection& conn) {
    std::size_t sent = 0;
    while (sent < conn.outbox.size()) {
        // **send() with MSG_NOSIGNAL, not write().** Writing to a socket the
        // peer has closed raises SIGPIPE, whose default action is to
        // terminate the process - so a client that hangs up without reading
        // its reply could kill the server. MSG_NOSIGNAL turns that into a
        // plain EPIPE, which the error path below already handles.
        ssize_t n = ::send(client_fd, conn.outbox.data() + sent, conn.outbox.size() - sent,
                           MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            ++reply_writes_;
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

        SubmitStatement(client_fd, conn);
        return true;
    }
    return true;
}

void TcpServer::ExpireOverdueTransactions() {
    const sched::MonoTimeNs now = scheduler_->clock().Now();
    for (auto& [fd, conn] : clients_) {
        // In flight: the statement holds the session, so ending its
        // transaction under it would pull state from under a coroutine.
        // Deferred to a tick after it ends, not exempted.
        if (conn.in_flight) continue;
        const txn::Transaction* open = conn.session.transaction();
        // Autocommit, or already expired and waiting for its ROLLBACK.
        if (open == nullptr || open->id() != conn.txn_stamped_id) continue;
        if (now - conn.txn_began_at_ns < txn_lifetime_ceiling_ns_) continue;

        const std::uint64_t id = open->id();
        // The connection-close path's own rollback (`CloseClient`), so the
        // sweep adds no second way to end a transaction.
        const DispatchOutcome rolled = dispatcher_->DispatchInternal("ROLLBACK", &conn.session);
        // A failure is logged whichever side of the transaction's end it
        // fell on: the client's refusal will say its writes are undone,
        // and this line is where an operator learns when that is untrue.
        if (!rolled.status.ok() && logging(LogLevel::kWarn)) {
            log_->Warn("txn", "fd=" + std::to_string(fd) + " trx_id=" + std::to_string(id) +
                                  " is past the lifetime ceiling and its rollback failed: " +
                                  rolled.response);
        }
        // Refused before it ended the transaction (an assertion
        // reservation's unwind): still open, and the next tick tries again.
        if (conn.session.transaction() != nullptr) continue;
        conn.session.Expire(id);
        if (logging(LogLevel::kInfo)) {
            log_->Info("txn", "fd=" + std::to_string(fd) + " trx_id=" + std::to_string(id) +
                                  " rolled back at the lifetime ceiling of " +
                                  std::to_string(txn_lifetime_ceiling_ns_ / 1'000'000) + " ms");
        }
    }
}

void TcpServer::OnStatementComplete(int client_fd) {
    auto it = clients_.find(client_fd);
    if (it == clients_.end()) return;  // nothing left to reply to
    Connection& conn = it->second;
    conn.in_flight = false;
    // The lifetime sweep's stamp (`Connection::txn_began_at_ns`). Here
    // because every statement either protocol runs ends here, so the
    // `BEGIN` that opened a transaction is the first completion to see it.
    if (const txn::Transaction* open = conn.session.transaction();
        open != nullptr && open->id() != conn.txn_stamped_id) {
        conn.txn_stamped_id = open->id();
        conn.txn_began_at_ns = scheduler_->clock().Now();
    }
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
        // Best effort: the socket is non-blocking, so a full send buffer
        // loses the goodbye. Nothing downstream depends on the client
        // seeing it, and the alternative is blocking the reactor on shutdown.
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
    // Marked instead, and torn down by OnStatementComplete. **The
    // statement is asked to stop** (BA-S15): a `SELECT`'s walk sees the
    // flag at its next slice boundary, and nothing else observes it - a
    // write may park on a lock or a commit, but runs to its end - bounded
    // by the statement's own row-touch budget (exec/budget.hpp), which is
    // what stops a hung client from pinning a connection forever.
    if (conn.in_flight) {
        conn.closing = true;
        conn.session.RequestCancel();
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
        (void)dispatcher_->DispatchInternal("ROLLBACK", &conn.session);
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
    UnregisterCancel(conn);
    clients_.erase(it);
    if (logging(LogLevel::kDebug)) {
        log_->Debug("client", "closed fd=" + std::to_string(client_fd) +
                                  " open_connections=" + std::to_string(clients_.size()));
    }
    if (scheduler_ != nullptr) {
        (void)scheduler_->UnregisterIoHandler(client_fd);
        CountSession(-1);
    }
    ::close(client_fd);
}

}  // namespace kds::server
