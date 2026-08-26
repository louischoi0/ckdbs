#include "kds/server/kwp_load_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/epoll_io_backend.hpp"
#include "kds/server/tcp_server.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/wire/row_codec.hpp"

// KL06 - the KWP v0 load endpoint, end to end over real sockets
// (docs/inflight/in-progress/workplan-kwp-load.md). The text listener rides beside it in every
// test for two reasons: verification reads go through the surface a client
// would use, and its STOP verb is what ends the reactor - KWP v0 has no
// stop of its own, deliberately.

namespace kds::server {
namespace {

int ConnectToLoopback(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) return fd;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::close(fd);
    return -1;
}

std::string SendAndReceiveLine(int fd, const std::string& line);

// Ends the reactor whatever the client thread did: a gtest ASSERT aborts
// the thread's function, and without this the missing STOP would hang the
// whole suite - the failure mode becomes a clean red instead.
struct StopGuard {
    std::uint16_t text_port;
    ~StopGuard() {
        int fd = ConnectToLoopback(text_port);
        if (fd < 0) return;
        SendAndReceiveLine(fd, "STOP");
        ::close(fd);
    }
};

std::string SendAndReceiveLine(int fd, const std::string& line) {
    std::string out = line + "\n";
    ::write(fd, out.data(), out.size());
    std::string response;
    char buf[256];
    while (response.find('\n') == std::string::npos) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        response.append(buf, static_cast<std::size_t>(n));
    }
    if (!response.empty() && response.back() == '\n') response.pop_back();
    return response;
}

void SendFrame(int fd, wire::ClientFrame type, std::span<const std::byte> payload) {
    const auto bytes = wire::EncodeFrame(static_cast<std::uint8_t>(type), 0, payload);
    ::write(fd, bytes.data(), bytes.size());
}

// Blocking read until one complete frame closes. Test-side only.
std::optional<wire::DecodedFrame> ReadFrame(int fd, wire::FrameDecoder& decoder) {
    for (;;) {
        if (auto frame = decoder.PopFrame()) return frame;
        std::byte buf[4096];
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) return std::nullopt;
        if (!decoder.Feed(std::span(buf, static_cast<std::size_t>(n))).ok()) {
            return std::nullopt;
        }
    }
}

class KwpLoadServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);
    }

    // Both listeners on one reactor - Expeditor::Serve()'s shape - run on
    // the calling thread until the text port's STOP ends it.
    void RunReactor(TcpServer& text, KwpLoadServer& kwp) {
        auto io_backend = sched::EpollIoBackend::Create();
        ASSERT_TRUE(io_backend.ok());
        sched::Scheduler scheduler(clock_, io_backend.value());
        ASSERT_TRUE(text.Attach(scheduler, *dispatcher_).ok());
        ASSERT_TRUE(kwp.Attach(scheduler, *dispatcher_).ok());
        scheduler.Run();
        kwp.Detach();
        text.Detach();
    }

    // The client's half of the handshake, shared by every test.
    bool Handshake(int fd, wire::FrameDecoder& decoder) {
        wire::ClientHello hello;
        hello.capabilities = wire::kCapBulkLoad;
        const auto payload = wire::EncodeClientHello(hello);
        SendFrame(fd, wire::ClientFrame::kHello, payload);
        auto reply = ReadFrame(fd, decoder);
        if (!reply.has_value() ||
            reply->type != static_cast<std::uint8_t>(wire::ServerFrame::kHello)) {
            return false;
        }
        wire::PayloadReader r(reply->payload);
        return r.U16().value_or(0) == wire::kKwpVersion &&
               (r.U64().value_or(0) & wire::kCapBulkLoad) != 0;
    }

    // Encodes one chunk of int-only rows for `t (id, a, b)` - the post-pk
    // schema, through the same codec the server decodes with.
    std::vector<std::byte> Chunk(std::uint64_t load_id, std::uint32_t seq,
                                 std::span<const std::pair<std::int64_t, std::int64_t>> rows) {
        catalog::Schema post_pk;
        for (int i = 0; i < 2; ++i) {
            catalog::SysColumnRow col{};
            col.type_val = catalog::kTypeValInt64;
            post_pk.columns.push_back(col);
        }
        wire::RowBatchWriter writer;
        for (const auto& [a, b] : rows) {
            parser::AstValue va;
            va.type = parser::ValueType::kInt;
            va.int_val = a;
            parser::AstValue vb;
            vb.type = parser::ValueType::kInt;
            vb.int_val = b;
            const parser::AstValue vals[] = {va, vb};
            EXPECT_TRUE(writer.AppendRow(post_pk, vals).ok());
        }
        wire::PayloadWriter w;
        w.U64(load_id);
        w.U32(seq);
        w.U16(static_cast<std::uint16_t>(rows.size()));
        auto out = w.Take();
        const auto batch = writer.Finish();
        out.insert(out.end(), batch.begin(), batch.end());
        return out;
    }

    sched::SystemClock clock_;
    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(KwpLoadServerTest, ALoadLandsRowsThroughTheOneWritePath) {
    constexpr std::uint16_t kTextPort = 25711;
    constexpr std::uint16_t kKwpPort = 25712;
    ASSERT_EQ(dispatcher_->Dispatch("CREATE TABLE t (id int64, a int64, b int64)")
                  .response.substr(0, 7),
              "CREATED");

    auto text = TcpServer::Listen(kTextPort);
    auto kwp = KwpLoadServer::Listen(kKwpPort);
    ASSERT_TRUE(text.ok());
    ASSERT_TRUE(kwp.ok());

    std::thread client([&] {
        StopGuard stop{kTextPort};
        int fd = ConnectToLoopback(kKwpPort);
        ASSERT_GE(fd, 0);
        wire::FrameDecoder decoder;
        ASSERT_TRUE(Handshake(fd, decoder));

        wire::LoadBegin begin;
        begin.relation = "t";
        const auto begin_payload = wire::EncodeLoadBegin(begin);
        SendFrame(fd, wire::ClientFrame::kLoadBegin, begin_payload);
        auto ready = ReadFrame(fd, decoder);
        ASSERT_TRUE(ready.has_value());
        ASSERT_EQ(ready->type, static_cast<std::uint8_t>(wire::ServerFrame::kLoadReady))
            << "payload size " << ready->payload.size();
        wire::PayloadReader r(ready->payload);
        const std::uint64_t load_id = r.U64().value_or(0);
        EXPECT_EQ(r.U16().value_or(0), kKwpLoadWindow);
        EXPECT_EQ(r.U32().value_or(0), kKwpMaxChunkBytes);
        EXPECT_EQ(r.U16().value_or(0), 2u);  // post-pk fields

        // Two chunks: three rows, then two - the T3 gate is open for `t`
        // (heap, int-only, nothing maintained), so this exercises the
        // sorted fill through the load path.
        const std::pair<std::int64_t, std::int64_t> first[] = {{10, 1}, {20, 2}, {30, 3}};
        const std::pair<std::int64_t, std::int64_t> second[] = {{40, 4}, {50, 5}};
        const auto chunk0 = Chunk(load_id, 0, first);
        SendFrame(fd, wire::ClientFrame::kLoadChunk, chunk0);
        auto ack0 = ReadFrame(fd, decoder);
        ASSERT_TRUE(ack0.has_value());
        ASSERT_EQ(ack0->type, static_cast<std::uint8_t>(wire::ServerFrame::kLoadAck));
        wire::PayloadReader a0(ack0->payload);
        EXPECT_EQ(a0.U64().value_or(0), load_id);
        EXPECT_EQ(a0.U32().value_or(9), 0u);
        EXPECT_EQ(a0.U64().value_or(0), 3u);

        const auto chunk1 = Chunk(load_id, 1, second);
        SendFrame(fd, wire::ClientFrame::kLoadChunk, chunk1);
        auto ack1 = ReadFrame(fd, decoder);
        ASSERT_TRUE(ack1.has_value());
        ASSERT_EQ(ack1->type, static_cast<std::uint8_t>(wire::ServerFrame::kLoadAck));

        SendFrame(fd, wire::ClientFrame::kLoadEnd, {});
        auto done = ReadFrame(fd, decoder);
        ASSERT_TRUE(done.has_value());
        ASSERT_EQ(done->type, static_cast<std::uint8_t>(wire::ServerFrame::kComplete));
        wire::PayloadReader d(done->payload);
        EXPECT_EQ(d.Str().value_or(""), "LOAD");
        EXPECT_EQ(d.U64().value_or(0), 5u);
        ::close(fd);

        // Verification through the surface a client would use, then STOP.
        int text_fd = ConnectToLoopback(kTextPort);
        ASSERT_GE(text_fd, 0);
        EXPECT_EQ(SendAndReceiveLine(text_fd, "SELECT COUNT(*) FROM t"), "count(*)\\n5");
        EXPECT_EQ(SendAndReceiveLine(text_fd, "SELECT b FROM t WHERE id = 4"), "b\\n4");
        ::close(text_fd);
    });

    RunReactor(text.value(), kwp.value());
    client.join();
}

TEST_F(KwpLoadServerTest, AnAbortUnwindsAndAProtocolErrorKillsTheLoad) {
    constexpr std::uint16_t kTextPort = 25713;
    constexpr std::uint16_t kKwpPort = 25714;
    ASSERT_EQ(dispatcher_->Dispatch("CREATE TABLE t (id int64, a int64, b int64)")
                  .response.substr(0, 7),
              "CREATED");

    auto text = TcpServer::Listen(kTextPort);
    auto kwp = KwpLoadServer::Listen(kKwpPort);
    ASSERT_TRUE(text.ok());
    ASSERT_TRUE(kwp.ok());

    std::thread client([&] {
        StopGuard stop{kTextPort};
        int fd = ConnectToLoopback(kKwpPort);
        ASSERT_GE(fd, 0);
        wire::FrameDecoder decoder;
        ASSERT_TRUE(Handshake(fd, decoder));

        // Abort after an accepted chunk: BI11 - nothing survives.
        wire::LoadBegin begin;
        begin.relation = "t";
        const auto begin_payload = wire::EncodeLoadBegin(begin);
        SendFrame(fd, wire::ClientFrame::kLoadBegin, begin_payload);
        auto ready = ReadFrame(fd, decoder);
        ASSERT_TRUE(ready.has_value());
        wire::PayloadReader r(ready->payload);
        const std::uint64_t load_id = r.U64().value_or(0);

        const std::pair<std::int64_t, std::int64_t> rows[] = {{7, 1}, {8, 2}};
        const auto chunk0 = Chunk(load_id, 0, rows);
        SendFrame(fd, wire::ClientFrame::kLoadChunk, chunk0);
        ASSERT_TRUE(ReadFrame(fd, decoder).has_value());  // the ack
        SendFrame(fd, wire::ClientFrame::kLoadAbort, {});
        auto done = ReadFrame(fd, decoder);
        ASSERT_TRUE(done.has_value());
        ASSERT_EQ(done->type, static_cast<std::uint8_t>(wire::ServerFrame::kComplete));
        wire::PayloadReader d(done->payload);
        EXPECT_EQ(d.Str().value_or(""), "ABORT");

        // A second load whose chunk skips a sequence number: the load dies
        // with S_ERROR and its rows unwind (KW4's modality).
        SendFrame(fd, wire::ClientFrame::kLoadBegin, begin_payload);
        auto ready2 = ReadFrame(fd, decoder);
        ASSERT_TRUE(ready2.has_value());
        wire::PayloadReader r2(ready2->payload);
        const std::uint64_t load2 = r2.U64().value_or(0);
        const auto chunk_skip = Chunk(load2, 5, rows);
        SendFrame(fd, wire::ClientFrame::kLoadChunk, chunk_skip);
        auto err = ReadFrame(fd, decoder);
        ASSERT_TRUE(err.has_value());
        EXPECT_EQ(err->type, static_cast<std::uint8_t>(wire::ServerFrame::kError));
        ::close(fd);

        int text_fd = ConnectToLoopback(kTextPort);
        ASSERT_GE(text_fd, 0);
        EXPECT_EQ(SendAndReceiveLine(text_fd, "SELECT COUNT(*) FROM t"), "count(*)\\n0");
        ::close(text_fd);
    });

    RunReactor(text.value(), kwp.value());
    client.join();
}

TEST_F(KwpLoadServerTest, AFrameBeforeHelloIsRefusedAndClosed) {
    constexpr std::uint16_t kTextPort = 25715;
    constexpr std::uint16_t kKwpPort = 25716;

    auto text = TcpServer::Listen(kTextPort);
    auto kwp = KwpLoadServer::Listen(kKwpPort);
    ASSERT_TRUE(text.ok());
    ASSERT_TRUE(kwp.ok());

    std::thread client([&] {
        StopGuard stop{kTextPort};
        int fd = ConnectToLoopback(kKwpPort);
        ASSERT_GE(fd, 0);
        wire::FrameDecoder decoder;
        wire::LoadBegin begin;
        begin.relation = "t";
        const auto payload = wire::EncodeLoadBegin(begin);
        SendFrame(fd, wire::ClientFrame::kLoadBegin, payload);
        auto err = ReadFrame(fd, decoder);
        ASSERT_TRUE(err.has_value());
        EXPECT_EQ(err->type, static_cast<std::uint8_t>(wire::ServerFrame::kError));
        // The server closes after the refusal: the next read is EOF.
        std::byte buf[16];
        EXPECT_LE(::read(fd, buf, sizeof(buf)), 0);
        ::close(fd);

        int text_fd = ConnectToLoopback(kTextPort);
        ASSERT_GE(text_fd, 0);
        ::close(text_fd);
    });

    RunReactor(text.value(), kwp.value());
    client.join();
}

}  // namespace
}  // namespace kds::server
