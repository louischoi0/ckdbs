#include "kds/server/kwp_load_server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "kds/catalog/catalog.hpp"

// KWP v0's socket half (docs/inflight/in-progress/workplan-kwp-load.md KL02/KL03). The syscall
// idioms are tcp_server.cpp's, including the MSG_NOSIGNAL lesson: a client
// hanging up without reading must cost a closed connection, never the
// process. Everything engine-shaped goes through CommandDispatcher's
// public surface - this file frames, sequences and refuses, and that is
// all it does.

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

// v0's storable wire types (KW4): the 8-byte int family and varchar.
// Decimal needs its scale carried per README's coercion rule and NULL is
// not storable, so both refuse at BEGIN rather than at row 40,000.
bool LoadableColumn(std::uint32_t type_val) {
    if (wire::WireTypeLen(type_val) == 8) return true;
    return wire::WireTypeLen(type_val) == -1 && type_val == catalog::kTypeValVarchar;
}

}  // namespace

StatusOr<KwpLoadServer> KwpLoadServer::Listen(std::uint16_t port) {
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
    // POSIX's own sockaddr idiom, not a persisted-format cast (rules.md #2
    // is about our formats; tcp_server.cpp states the same).
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
    return KwpLoadServer(fd);
}

KwpLoadServer::KwpLoadServer(KwpLoadServer&& other) noexcept
    : listen_fd_(other.listen_fd_),
      scheduler_(other.scheduler_),
      dispatcher_(other.dispatcher_),
      log_(other.log_),
      next_load_id_(other.next_load_id_),
      clients_(std::move(other.clients_)) {
    other.listen_fd_ = -1;
    other.scheduler_ = nullptr;
}

KwpLoadServer& KwpLoadServer::operator=(KwpLoadServer&& other) noexcept {
    if (this != &other) {
        Detach();
        CloseIfOpen();
        listen_fd_ = other.listen_fd_;
        scheduler_ = other.scheduler_;
        dispatcher_ = other.dispatcher_;
        log_ = other.log_;
        next_load_id_ = other.next_load_id_;
        clients_ = std::move(other.clients_);
        other.listen_fd_ = -1;
        other.scheduler_ = nullptr;
    }
    return *this;
}

KwpLoadServer::~KwpLoadServer() {
    Detach();
    CloseIfOpen();
}

void KwpLoadServer::CloseIfOpen() noexcept {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

Status KwpLoadServer::Attach(sched::Scheduler& scheduler, CommandDispatcher& dispatcher,
                             Logger* log) {
    scheduler_ = &scheduler;
    dispatcher_ = &dispatcher;
    log_ = log;
    if (Status s = SetNonBlocking(listen_fd_); !s.ok()) return s;
    return scheduler_->RegisterIoHandler(listen_fd_, sched::IoInterest::kReadable,
                                         [this](const sched::IoEvent&) { OnListenerReadable(); });
}

void KwpLoadServer::Detach() noexcept {
    if (scheduler_ == nullptr) return;
    for (auto& [fd, conn] : clients_) {
        if (conn.phase == Phase::kLoading && dispatcher_ != nullptr) {
            (void)dispatcher_->Dispatch("ROLLBACK", &conn.session);
        }
        (void)scheduler_->UnregisterIoHandler(fd);
        ::close(fd);
    }
    clients_.clear();
    if (listen_fd_ >= 0) (void)scheduler_->UnregisterIoHandler(listen_fd_);
    scheduler_ = nullptr;
}

void KwpLoadServer::OnListenerReadable() {
    for (;;) {
        int client_fd = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) return;  // EAGAIN or a transient error: done for now
        if (!SetNonBlocking(client_fd).ok()) {
            ::close(client_fd);
            continue;
        }
        // TCP_NODELAY, unconditionally - tcp_server.cpp's lesson, relearned
        // by measurement (bench/results-bulk-insert.md Part IV): a small
        // ACK frame held by Nagle against the peer's delayed-ACK timer
        // cost a pipelined load 33% of its throughput, ~40 ms per stall.
        // There is nothing for Nagle to coalesce that the outbox does not
        // already coalesce better.
        int nodelay = 1;
        ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        clients_.emplace(client_fd, Connection{});
        Status s = scheduler_->RegisterIoHandler(
            client_fd, sched::IoInterest::kReadable,
            [this, client_fd](const sched::IoEvent& event) { OnClientEvent(client_fd, event); });
        if (!s.ok()) {
            clients_.erase(client_fd);
            ::close(client_fd);
        }
        if (logging(LogLevel::kDebug)) log_->Debug("kwp", "connection accepted");
    }
}

void KwpLoadServer::OnClientEvent(int client_fd, const sched::IoEvent& event) {
    if (event.writable) {
        auto it = clients_.find(client_fd);
        if (it == clients_.end()) return;
        if (!FlushOutbox(client_fd, it->second)) return;
        SyncWriteInterest(client_fd, it->second);
    }
    if (event.readable) OnClientReadable(client_fd);
}

void KwpLoadServer::OnClientReadable(int client_fd) {
    auto it = clients_.find(client_fd);
    if (it == clients_.end()) return;

    std::byte chunk[4096];
    ssize_t n = ::read(client_fd, chunk, sizeof(chunk));
    if (n == 0) {
        CloseClient(client_fd);  // orderly hangup; a mid-load one rolls back
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        CloseClient(client_fd);
        return;
    }
    if (Status s = it->second.decoder.Feed(std::span(chunk, static_cast<std::size_t>(n)));
        !s.ok()) {
        // Framing corruption has no resync (kwp.hpp): close.
        CloseClient(client_fd);
        return;
    }
    if (!DrainFrames(client_fd, it->second)) return;
    SyncWriteInterest(client_fd, it->second);
}

bool KwpLoadServer::DrainFrames(int client_fd, Connection& conn) {
    while (auto frame = conn.decoder.PopFrame()) {
        if (!HandleFrame(client_fd, conn, frame.value())) return false;
        auto it = clients_.find(client_fd);
        if (it == clients_.end()) return false;
    }
    return true;
}

bool KwpLoadServer::HandleFrame(int client_fd, Connection& conn,
                                const wire::DecodedFrame& frame) {
    const auto type = static_cast<wire::ClientFrameType>(frame.type);

    // The two frames legal in every phase past the handshake (KW4).
    if (conn.phase != Phase::kAwaitHello && type == wire::ClientFrameType::kPing) {
        Send(conn, wire::ServerFrameType::kPong, {});
        return true;
    }
    if (type == wire::ClientFrameType::kTerminate) {
        CloseClient(client_fd);
        return false;
    }

    switch (conn.phase) {
        case Phase::kAwaitHello: {
            if (type != wire::ClientFrameType::kHello) {
                SendError(conn, wire::ProtocolDetail::kUnexpectedFrame, "expected C_HELLO first");
                CloseClient(client_fd);
                return false;
            }
            auto hello = wire::DecodeClientHello(frame.payload);
            if (!hello.ok()) {
                SendError(conn, wire::ProtocolDetail::kBadMagic, hello.status().message());
                CloseClient(client_fd);
                return false;
            }
            if (hello.value().min_version > wire::kKwpVersion ||
                hello.value().max_version < wire::kKwpVersion) {
                SendError(conn, wire::ProtocolDetail::kUnsupportedVersion, "server speaks KWP version 1 only");
                CloseClient(client_fd);
                return false;
            }
            if (hello.value().auth_method != 0) {
                SendError(conn, wire::ProtocolDetail::kUnexpectedFrame, "v0 authenticates NONE only (loopback)");
                CloseClient(client_fd);
                return false;
            }
            // **The one `S_HELLO` payload** (`wire::EncodeServerHello`).
            // The frame *numbers* were unified on 2026-08-31; writing a
            // different payload under a shared number is the same defect
            // one layer in, and worse than the split was - a client pointed
            // at the wrong endpoint used to fail on an unknown type and
            // would now decode garbage.
            //
            // This endpoint mints no session id and no cancel key: it has
            // no cancel connection and no session state to name, so both
            // stay zero, which is what the field means where nothing issues
            // one.
            wire::ServerHello hello_out;
            hello_out.version = wire::kKwpVersion;
            hello_out.capabilities = wire::kCapBulkLoad;
            hello_out.server_info = "kds-load";
            Send(conn, wire::ServerFrameType::kHello, wire::EncodeServerHello(hello_out));
            conn.phase = Phase::kIdle;
            return true;
        }

        case Phase::kIdle: {
            if (type == wire::ClientFrameType::kLoadBegin) {
                HandleLoadBegin(conn, frame.payload);
                return true;
            }
            SendError(conn, wire::ProtocolDetail::kUnexpectedFrame, "frame not legal outside a load session");
            return true;
        }

        case Phase::kLoading: {
            switch (type) {
                case wire::ClientFrameType::kLoadChunk:
                    HandleLoadChunk(conn, frame);
                    return true;
                case wire::ClientFrameType::kLoadEnd:
                    HandleLoadEnd(conn, /*abort=*/false);
                    return true;
                case wire::ClientFrameType::kLoadAbort:
                    HandleLoadEnd(conn, /*abort=*/true);
                    return true;
                default:
                    // KW4's modality: the load is dead and the transaction
                    // with it - v0's collapse of §5's discard-to-sync.
                    SendError(conn, wire::ProtocolDetail::kUnexpectedFrame, "frame not legal inside a load session");
                    (void)dispatcher_->Dispatch("ROLLBACK", &conn.session);
                    conn.phase = Phase::kIdle;
                    conn.load = LoadState{};
                    return true;
            }
        }
    }
    return true;
}

void KwpLoadServer::HandleLoadBegin(Connection& conn, std::span<const std::byte> payload) {
    auto begin = wire::DecodeLoadBegin(payload);
    if (!begin.ok()) {
        SendError(conn, wire::ProtocolDetail::kMalformedPayload, begin.status().message());
        return;
    }
    if (begin.value().flags != 0) {
        SendError(conn, wire::ProtocolDetail::kMalformedPayload, "C_LOAD_BEGIN flags are reserved 0");
        return;
    }

    auto& catalog = dispatcher_->catalog();
    auto oid = catalog.FindTableOidByName(begin.value().relation);
    if (!oid.ok()) {
        SendError(conn, oid.status());
        return;
    }
    auto access = catalog.InitTableAccess(oid.value());
    if (!access.ok()) {
        SendError(conn, access.status());
        return;
    }
    const catalog::Schema& schema = access.value()->schema;
    for (std::size_t i = 1; i < schema.columns.size(); ++i) {
        if (!LoadableColumn(schema.columns[i].type_val)) {
            SendError(conn, Status::Unsupported(
                                "column '" +
                                std::string(catalog::NameView(schema.columns[i].name)) +
                                "' has a type v0 cannot load (int family and varchar only)"));
            return;
        }
    }

    // The implicit transaction (KW5, BI11): the same BEGIN the text
    // protocol runs, so every semantics is the session's own. A session
    // already inside a transaction is refused by BEGIN itself.
    if (auto out = dispatcher_->Dispatch("BEGIN", &conn.session);
        out.response.rfind("ERR", 0) == 0) {
        SendError(conn, StatusFromErrorReply(out.response));
        return;
    }

    conn.load = LoadState{};
    conn.load.load_id = next_load_id_++;
    conn.load.relation = begin.value().relation;
    conn.load.field_count = schema.columns.size() - 1;
    for (std::size_t i = 1; i < schema.columns.size(); ++i) {
        conn.load.type_vals.push_back(schema.columns[i].type_val);
    }
    conn.phase = Phase::kLoading;

    // S_LOAD_READY: the numbers, then the post-pk field descriptors in the
    // S_ROW_DESC encoding - one description format on the wire (CC2's
    // argument), stated by the server so drift is impossible.
    wire::PayloadWriter w;
    w.U64(conn.load.load_id);
    w.U16(kKwpLoadWindow);
    w.U32(kKwpMaxChunkBytes);
    w.U16(static_cast<std::uint16_t>(conn.load.field_count));
    auto head = w.Take();
    auto fields = wire::DescribeSchema(schema);
    fields.erase(fields.begin());  // the pk is the engine's, never the client's
    wire::EncodeRowDescription(fields, head);
    Send(conn, wire::ServerFrameType::kLoadReady, head);

    if (logging(LogLevel::kInfo)) {
        log_->Info("kwp", "load " + std::to_string(conn.load.load_id) + " on " +
                              conn.load.relation + " begins");
    }
}

void KwpLoadServer::HandleLoadChunk(Connection& conn, const wire::DecodedFrame& frame) {
    const auto fail_load = [&](const Status& status) {
        SendError(conn, status);
        (void)dispatcher_->Dispatch("ROLLBACK", &conn.session);
        conn.phase = Phase::kIdle;
        conn.load = LoadState{};
    };

    if (frame.payload.size() > kKwpMaxChunkBytes) {
        fail_load(Status::InvalidArgument("chunk exceeds the announced max_chunk_bytes"));
        return;
    }
    wire::PayloadReader reader(frame.payload);
    auto header = wire::DecodeLoadChunkHeader(reader);
    if (!header.ok()) {
        fail_load(Status::InvalidArgument(header.status().message()));
        return;
    }
    if (header.value().load_id != conn.load.load_id) {
        fail_load(Status::InvalidArgument("chunk names a load this connection is not running"));
        return;
    }
    if (header.value().chunk_seq != conn.load.next_seq) {
        fail_load(Status::InvalidArgument("chunk_seq " + std::to_string(header.value().chunk_seq) +
                                  ", expected " + std::to_string(conn.load.next_seq) +
                                  " (BI14: no resume, no reorder)"));
        return;
    }

    auto rows = wire::DecodeRowBatch(reader.Rest(), conn.load.field_count);
    if (!rows.ok()) {
        fail_load(Status::InvalidArgument(rows.status().message()));
        return;
    }
    if (rows.value().size() != header.value().row_count) {
        fail_load(Status::InvalidArgument("row_count disagrees with the rows the chunk holds"));
        return;
    }

    // Wire rows to the parser's value shape - the one conversion in the
    // path, and it is a *transliteration*, not a coercion: the column's
    // type decides at the same gates a T1 statement goes through.
    parser::InsertStmt stmt;
    stmt.table_name = conn.load.relation;
    stmt.rows.reserve(rows.value().size());
    for (std::size_t r = 0; r < rows.value().size(); ++r) {
        std::vector<parser::AstValue> values;
        values.reserve(conn.load.field_count);
        for (std::size_t f = 0; f < conn.load.field_count; ++f) {
            const wire::DecodedField& field = rows.value()[r][f];
            if (field.is_null) {
                fail_load(Status::InvalidArgument("chunk " + std::to_string(header.value().chunk_seq) +
                                      ", row " + std::to_string(r + 1) +
                                      ": NULL is not storable"));
                return;
            }
            parser::AstValue v;
            if (wire::WireTypeLen(conn.load.type_vals[f]) == 8) {
                if (field.bytes.size() != 8) {
                    fail_load(Status::InvalidArgument("chunk " + std::to_string(header.value().chunk_seq) +
                                              ", row " + std::to_string(r + 1) +
                                              ": fixed field of the wrong width"));
                    return;
                }
                std::uint64_t raw = 0;
                std::memcpy(&raw, field.bytes.data(), 8);
                v.type = parser::ValueType::kInt;
                v.int_val = static_cast<std::int64_t>(raw);
                // The digit text preserves the full unsigned range through
                // the uint64 encode path (ast.hpp's raw_int_text note).
                v.raw_int_text = std::to_string(raw);
            } else {
                v.type = parser::ValueType::kStr;
                v.str_val.assign(reinterpret_cast<const char*>(field.bytes.data()),
                                 field.bytes.size());
            }
            values.push_back(std::move(v));
        }
        stmt.rows.push_back(std::move(values));
    }

    // BI2, through KL04's seam: this IS the T1 path, T3's sorted fill
    // included when the relation is inside the gate.
    auto out = dispatcher_->ExecuteInsert(stmt, conn.session);
    if (out.response.rfind("ERR", 0) == 0) {
        fail_load(Status::InvalidArgument("chunk " + std::to_string(header.value().chunk_seq) + ": " + out.response));
        return;
    }

    conn.load.rows_accepted += stmt.rows.size();
    ++conn.load.next_seq;

    wire::PayloadWriter w;
    w.U64(conn.load.load_id);
    w.U32(header.value().chunk_seq);
    w.U64(conn.load.rows_accepted);
    const auto payload = w.Take();
    Send(conn, wire::ServerFrameType::kLoadAck, payload);
}

void KwpLoadServer::HandleLoadEnd(Connection& conn, bool abort) {
    const std::uint64_t rows = abort ? 0 : conn.load.rows_accepted;
    auto out = dispatcher_->Dispatch(abort ? "ROLLBACK" : "COMMIT", &conn.session);
    if (!abort && out.response.rfind("ERR", 0) == 0) {
        SendError(conn, StatusFromErrorReply(out.response));
        (void)dispatcher_->Dispatch("ROLLBACK", &conn.session);
        conn.phase = Phase::kIdle;
        conn.load = LoadState{};
        return;
    }

    // `Text` and not `Str`, which is the query surface's `S_COMPLETE` shape
    // (§7: `{tag Text, rows_affected u64}`) - one frame number, one payload.
    wire::PayloadWriter w;
    w.Text(abort ? "ABORT" : "LOAD");
    w.U64(rows);
    Send(conn, wire::ServerFrameType::kComplete, w.Take());

    if (logging(LogLevel::kInfo)) {
        log_->Info("kwp", "load " + std::to_string(conn.load.load_id) +
                              (abort ? " aborted" : " committed " + std::to_string(rows) +
                                                        " row(s)"));
    }
    conn.phase = Phase::kIdle;
    conn.load = LoadState{};
}

void KwpLoadServer::Send(Connection& conn, wire::ServerFrameType type,
                         std::span<const std::byte> payload) {
    const auto bytes = wire::EncodeFrame(static_cast<std::uint8_t>(type), 0, payload);
    conn.outbox.insert(conn.outbox.end(), bytes.begin(), bytes.end());
}

void KwpLoadServer::SendError(Connection& conn, wire::ProtocolDetail detail,
                              std::string_view message) {
    // **The one `S_ERROR` payload** (`wire::EncodeError`, §11): a code a
    // client switches on, a `retryable` bit, a severity, and the message.
    // The private `{Str code, Str message}` this replaced was a second
    // shape under a shared frame number, and its "code" was a *word* - so
    // no client could branch on it without matching strings.
    Send(conn, wire::ServerFrameType::kError,
         wire::EncodeError(wire::ProtocolError(detail, std::string(message),
                                               wire::Severity::kError)));
}

void KwpLoadServer::SendError(Connection& conn, const Status& status) {
    Send(conn, wire::ServerFrameType::kError, wire::EncodeError(wire::ErrorFromStatus(status)));
}

bool KwpLoadServer::FlushOutbox(int client_fd, Connection& conn) {
    while (!conn.outbox.empty()) {
        // MSG_NOSIGNAL: a hung-up reader is a closed connection, never a
        // SIGPIPE through the whole server (tcp_server.cpp's lesson).
        ssize_t n = ::send(client_fd, conn.outbox.data(), conn.outbox.size(), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            CloseClient(client_fd);
            return false;
        }
        conn.outbox.erase(conn.outbox.begin(), conn.outbox.begin() + n);
    }
    return true;
}

void KwpLoadServer::SyncWriteInterest(int client_fd, Connection& conn) {
    (void)FlushOutbox(client_fd, conn);
    auto it = clients_.find(client_fd);
    if (it == clients_.end()) return;
    const bool want = !it->second.outbox.empty();
    if (want == it->second.want_writable || scheduler_ == nullptr) return;
    auto interest = want ? static_cast<sched::IoInterest>(
                               static_cast<std::uint8_t>(sched::IoInterest::kReadable) |
                               static_cast<std::uint8_t>(sched::IoInterest::kWritable))
                         : sched::IoInterest::kReadable;
    if (scheduler_->ModifyIoHandler(client_fd, interest).ok()) {
        it->second.want_writable = want;
    }
}

void KwpLoadServer::CloseClient(int client_fd) {
    auto it = clients_.find(client_fd);
    if (it == clients_.end()) return;
    // A refusal that closes must still *say so*: the S_ERROR sits in the
    // outbox, and closing under it would hand the client a bare EOF where
    // the protocol promised a reason. One best-effort send - the socket
    // is nonblocking, so a full buffer loses the message rather than the
    // reactor, which is the right trade for a connection that is dying
    // anyway.
    if (!it->second.outbox.empty()) {
        (void)::send(client_fd, it->second.outbox.data(), it->second.outbox.size(),
                     MSG_NOSIGNAL);
    }
    if (it->second.phase == Phase::kLoading && dispatcher_ != nullptr) {
        // BI11: a connection dying mid-load unwinds its transaction.
        (void)dispatcher_->Dispatch("ROLLBACK", &it->second.session);
    }
    if (scheduler_ != nullptr) (void)scheduler_->UnregisterIoHandler(client_fd);
    ::close(client_fd);
    clients_.erase(it);
}

}  // namespace kds::server
