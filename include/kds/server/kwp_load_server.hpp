#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/wire/kwp.hpp"
#include "kds/wire/kwp_types.hpp"
#include "kds/wire/row_codec.hpp"

// KWP v0: the load endpoint (docs/inflight/in-progress/workplan-kwp-load.md KW1-KW7). A second
// listener speaking exactly the subset T2 needs - the handshake and the
// modal load session - while the query surface stays on the newline
// protocol. TcpServer's shape deliberately: a reactor participant with
// fd-keyed connections, as thin as possible around the syscalls; the
// engine-facing work all happens through CommandDispatcher's public
// surface (ExecuteInsert and the session statements), so a load and a T1
// statement are indistinguishable from the write pipeline's side (BI2).
//
// The transaction story is KW5's: C_LOAD_BEGIN opens the implicit
// transaction with the same BEGIN the text protocol runs, every applied
// chunk is one ExecuteInsert under that session, C_LOAD_END commits and
// C_LOAD_ABORT - or the connection dying mid-load - rolls back. Zero new
// transaction code; every semantics (poisoning, unwind, first-updater-
// wins) is the session's own.

namespace kds::server {

// `[PROPOSED]` per BI7; parameters of the READY frame, never promises.
inline constexpr std::uint16_t kKwpLoadWindow = 4;
inline constexpr std::uint32_t kKwpMaxChunkBytes = 256u * 1024u;

class KwpLoadServer {
public:
    static StatusOr<KwpLoadServer> Listen(std::uint16_t port);

    KwpLoadServer(KwpLoadServer&& other) noexcept;
    KwpLoadServer& operator=(KwpLoadServer&& other) noexcept;
    KwpLoadServer(const KwpLoadServer&) = delete;
    KwpLoadServer& operator=(const KwpLoadServer&) = delete;
    ~KwpLoadServer();

    Status Attach(sched::Scheduler& scheduler, CommandDispatcher& dispatcher,
                  Logger* log = nullptr);
    void Detach() noexcept;

    std::size_t open_connections() const noexcept { return clients_.size(); }

private:
    // The connection's modality (KW4): frames legal in one phase are
    // protocol errors in another, and the answer to an illegal frame is
    // S_ERROR followed by the load's death - v0's collapse of §5's
    // discard-to-sync, honest because a load client has nothing to resync.
    enum class Phase : std::uint8_t { kAwaitHello, kIdle, kLoading };

    struct LoadState {
        std::uint64_t load_id = 0;
        std::string relation;
        std::uint32_t next_seq = 0;      // strictly increasing from 0
        std::uint64_t rows_accepted = 0; // cumulative, echoed in every ACK
        std::size_t field_count = 0;     // post-pk columns the client encodes
        std::vector<std::uint32_t> type_vals;  // per field, schema order
    };

    struct Connection {
        wire::FrameDecoder decoder;
        std::vector<std::byte> outbox;
        bool want_writable = false;
        bool closing = false;
        Phase phase = Phase::kAwaitHello;
        LoadState load;
        // Braced: `Session`'s constructor is `explicit`, so aggregate-
        // initializing a Connection would have to copy-list-initialize this
        // member through it. Direct-list-initialization here is what makes
        // `Connection{}` legal without loosening the constructor.
        Session session{};
    };

    explicit KwpLoadServer(int fd) noexcept : listen_fd_(fd) {}
    void CloseIfOpen() noexcept;

    void OnListenerReadable();
    void OnClientEvent(int client_fd, const sched::IoEvent& event);
    void OnClientReadable(int client_fd);
    // Handles every complete frame the decoder holds. Returns false when
    // the connection was closed and must not be touched again.
    bool DrainFrames(int client_fd, Connection& conn);
    bool HandleFrame(int client_fd, Connection& conn, const wire::DecodedFrame& frame);

    // The load session's three verbs (KW4/KW5).
    void HandleLoadBegin(Connection& conn, std::span<const std::byte> payload);
    void HandleLoadChunk(Connection& conn, const wire::DecodedFrame& frame);
    void HandleLoadEnd(Connection& conn, bool abort);

    // Frame writers. Send() appends to the outbox and flushes what the
    // socket will take.
    void Send(Connection& conn, wire::ServerFrame type, std::span<const std::byte> payload);
    void SendError(Connection& conn, std::string_view code, std::string_view message);
    bool FlushOutbox(int client_fd, Connection& conn);
    void SyncWriteInterest(int client_fd, Connection& conn);
    void CloseClient(int client_fd);

    bool logging(LogLevel level) const noexcept {
        return log_ != nullptr && log_->enabled(level);
    }

    int listen_fd_;
    sched::Scheduler* scheduler_ = nullptr;
    CommandDispatcher* dispatcher_ = nullptr;
    Logger* log_ = nullptr;
    std::uint64_t next_load_id_ = 1;
    std::unordered_map<int, Connection> clients_;
};

}  // namespace kds::server
