#include "kds/server/tcp_server.hpp"

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

StatusOr<TcpServer> TcpServer::Listen(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return Status::IoError(std::string("socket() failed: ") + std::strerror(errno));
    }

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

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
      clients_(std::move(other.clients_)) {
    other.listen_fd_ = -1;
    other.scheduler_ = nullptr;
    other.dispatcher_ = nullptr;
    other.clients_.clear();
}

TcpServer& TcpServer::operator=(TcpServer&& other) noexcept {
    if (this != &other) {
        Detach();
        CloseIfOpen();
        listen_fd_ = other.listen_fd_;
        scheduler_ = other.scheduler_;
        dispatcher_ = other.dispatcher_;
        clients_ = std::move(other.clients_);
        other.listen_fd_ = -1;
        other.scheduler_ = nullptr;
        other.dispatcher_ = nullptr;
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

    return scheduler_->RegisterIoHandler(listen_fd_, sched::IoInterest::kReadable,
                                          [this](const sched::IoEvent&) { OnListenerReadable(); });
}

void TcpServer::Detach() noexcept {
    if (scheduler_ != nullptr) {
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
    clients_.emplace(client_fd, std::move(fresh));
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

    it->second.inbox.append(chunk, static_cast<std::size_t>(n));
    if (!DrainCommands(client_fd, it->second)) return;  // closed or server stopping
    if (!FlushOutbox(client_fd, it->second)) return;
    SyncWriteInterest(client_fd, it->second);
}

bool TcpServer::FlushOutbox(int client_fd, Connection& conn) {
    std::size_t sent = 0;
    while (sent < conn.outbox.size()) {
        ssize_t n = ::write(client_fd, conn.outbox.data() + sent, conn.outbox.size() - sent);
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
    std::size_t nl;
    while ((nl = conn.inbox.find('\n')) != std::string::npos) {
        std::string_view line(conn.inbox.data(), nl);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);  // tolerate CRLF clients
        }

        // The request as the client sent it, before anything interprets
        // it. Trace, because it is one line per command and it echoes
        // whatever the client typed - including anything they should not
        // have typed.
        if (logging(LogLevel::kTrace)) {
            log_->Trace("client", "fd=" + std::to_string(client_fd) + " request \"" +
                                      std::string(line) + "\"");
        }

        DispatchOutcome outcome = dispatcher_->Dispatch(line, &conn.session);
        // Appended, not written: one readable event can carry a whole batch
        // of pipelined commands, and a write() per command is a syscall per
        // command plus a separate small segment per command on the wire.
        // The batch leaves in one write() below.
        conn.outbox.append(outcome.response);
        conn.outbox.push_back('\n');

        conn.inbox.erase(0, nl + 1);

        if (outcome.should_stop) {
            // Best effort, exactly as the per-command write() it replaces
            // was: the socket is non-blocking, so a full send buffer loses
            // the goodbye. Nothing downstream depends on the client seeing
            // it, and the alternative is blocking the reactor on shutdown.
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
            scheduler_->Stop();
            return false;
        }
    }
    return true;
}

void TcpServer::CloseClient(int client_fd) {
    // **A connection that goes away rolls back** (docs/txn.md section
    // 10-8). Anything else would leave an open transaction holding its
    // writes and its place in every other session's in-flight set, with
    // nobody left to end it.
    if (auto it = clients_.find(client_fd); it != clients_.end()) {
        if (dispatcher_ != nullptr && it->second.session.in_explicit_txn()) {
            if (logging(LogLevel::kInfo)) {
                log_->Info("client", "fd=" + std::to_string(client_fd) +
                                         " closed with a transaction open; rolling it back");
            }
            (void)dispatcher_->Dispatch("ROLLBACK", &it->second.session);
        }
    }
    if (clients_.erase(client_fd) == 0) return;
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
