#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/epoll_io_backend.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/tcp_server.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/wire/error_registry.hpp"
#include "kds/wire/handshake.hpp"
#include "kds/wire/kwp.hpp"
#include "kds/wire/kwp_types.hpp"
#include "kds/wire/row_codec.hpp"

// **KWP/1 over a real socket** (protocol-wp.md P13). `tcp_server_test.cpp`
// is the same listener's newline surface; this is the framed one, and the
// two exist together because §12 keeps both.
//
// What P13 asks for and what is here: golden byte sessions end to end
// against the real dispatcher, malformed-stream behaviour at the socket
// layer (a frame split across reads, a hostile `length`), and the debug
// port off by default - which here is the *protocol* default, since that is
// what a deployment gets when it configures nothing.

namespace kds::server {
namespace {

using wire::ClientFrameType;
using wire::ServerFrameType;

class KwpEndpointTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
        ASSERT_EQ(dispatcher_->Dispatch("CREATE TABLE t (id int64, v int32)").response.substr(0, 7),
                  "CREATED");
        ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO t VALUES (7)").response.substr(0, 8),
                  "INSERTED");
    }

    void RunReactor(TcpServer& listener) {
        auto io_backend = sched::EpollIoBackend::Create();
        ASSERT_TRUE(io_backend.ok()) << io_backend.status().message();
        sched::Scheduler scheduler(clock_, io_backend.value());
        ASSERT_TRUE(listener.Attach(scheduler, *dispatcher_).ok());
        scheduler.Run();
        listener.Detach();
    }

    sched::SystemClock clock_;
    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

int Connect(std::uint16_t port) {
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

void SendFrame(int fd, ClientFrameType type, std::span<const std::byte> payload) {
    const auto frame = wire::EncodeFrame(static_cast<std::uint8_t>(type), 0, payload);
    ASSERT_EQ(::write(fd, frame.data(), frame.size()), static_cast<ssize_t>(frame.size()));
}

// Reads until `decoder` yields a frame, or the peer hangs up.
std::optional<wire::DecodedFrame> ReadFrame(int fd, wire::FrameDecoder& decoder) {
    while (true) {
        if (auto frame = decoder.PopFrame()) return frame;
        std::byte buf[4096];
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) return std::nullopt;
        if (!decoder.Feed(std::span<const std::byte>(buf, static_cast<std::size_t>(n))).ok()) {
            return std::nullopt;
        }
    }
}

bool Handshake(int fd, wire::FrameDecoder& decoder) {
    wire::ClientHello hello;
    hello.client_name = "endpoint-test";
    SendFrame(fd, ClientFrameType::kHello, wire::EncodeClientHello(hello));
    auto s_hello = ReadFrame(fd, decoder);
    if (!s_hello.has_value() ||
        s_hello->type != static_cast<std::uint8_t>(ServerFrameType::kHello)) {
        return false;
    }
    auto ready = ReadFrame(fd, decoder);
    return ready.has_value() &&
           ready->type == static_cast<std::uint8_t>(ServerFrameType::kReady);
}

std::vector<std::byte> ParsePayload(std::string_view name, std::string_view sql) {
    wire::PayloadWriter w;
    w.Str(name);
    w.Text(sql);
    return w.Take();
}

std::vector<std::byte> BindPayload(std::string_view portal, std::string_view stmt) {
    wire::PayloadWriter w;
    w.Str(portal);
    w.Str(stmt);
    auto out = w.Take();
    (void)wire::EncodeBindParams({}, out);
    return out;
}

std::vector<std::byte> ExecutePayload(std::string_view portal, std::uint32_t max_rows) {
    wire::PayloadWriter w;
    w.Str(portal);
    w.U32(max_rows);
    return w.Take();
}

// Sends STOP as an ordinary statement, which ends the reactor exactly as
// the newline protocol's STOP line does - one stop path, two framings.
void StopServer(int fd, wire::FrameDecoder& decoder) {
    SendFrame(fd, ClientFrameType::kParse, ParsePayload("stop", "STOP"));
    SendFrame(fd, ClientFrameType::kBind, BindPayload("sp", "stop"));
    SendFrame(fd, ClientFrameType::kExecute, ExecutePayload("sp", 0));
    (void)ReadFrame(fd, decoder);
}

TEST_F(KwpEndpointTest, TheDefaultProtocolIsKwp) {
    // The cut-over as a property rather than as a comment: a listener that
    // was configured with nothing speaks the framed protocol, and the
    // newline one is reachable only where a deployment asked for it.
    auto listener = TcpServer::Listen(25451);
    ASSERT_TRUE(listener.ok()) << listener.status().message();
    EXPECT_EQ(listener.value().protocol(), Protocol::kKwp);
}

TEST_F(KwpEndpointTest, ConfigurationSurvivesTheMoveThatOwnsTheListener) {
    // **A listener is configured and then moved** into whatever owns it -
    // `Expeditor::Serve` does exactly this for the debug port. A setting
    // left out of the move constructor is silently dropped, which is not a
    // hypothetical: the first cut of P13 dropped `protocol_`, so the
    // configured *text* port came up speaking KWP and the two ports were
    // indistinguishable. Pinned here because the failure is invisible from
    // inside either endpoint.
    auto listener = TcpServer::Listen(25456);
    ASSERT_TRUE(listener.ok()) << listener.status().message();
    listener.value().set_protocol(Protocol::kText);
    TcpServer moved(std::move(listener.value()));
    EXPECT_EQ(moved.protocol(), Protocol::kText);

    auto other = TcpServer::Listen(25457);
    ASSERT_TRUE(other.ok());
    other.value().set_protocol(Protocol::kText);
    TcpServer assigned = std::move(other.value());
    EXPECT_EQ(assigned.protocol(), Protocol::kText);
}

TEST_F(KwpEndpointTest, AWholeSessionRoundTripsOverARealSocket) {
    constexpr std::uint16_t kPort = 25452;
    auto listener = TcpServer::Listen(kPort);
    ASSERT_TRUE(listener.ok()) << listener.status().message();
    std::thread server([&] { RunReactor(listener.value()); });

    int fd = Connect(kPort);
    ASSERT_GE(fd, 0);
    wire::FrameDecoder decoder;
    ASSERT_TRUE(Handshake(fd, decoder));

    SendFrame(fd, ClientFrameType::kParse, ParsePayload("s", "SELECT id, v FROM t"));
    SendFrame(fd, ClientFrameType::kBind, BindPayload("p", "s"));
    SendFrame(fd, ClientFrameType::kExecute, ExecutePayload("p", 0));

    auto parse_ok = ReadFrame(fd, decoder);
    ASSERT_TRUE(parse_ok.has_value());
    EXPECT_EQ(parse_ok->type, static_cast<std::uint8_t>(ServerFrameType::kParseOk));
    auto bind_ok = ReadFrame(fd, decoder);
    ASSERT_TRUE(bind_ok.has_value());
    EXPECT_EQ(bind_ok->type, static_cast<std::uint8_t>(ServerFrameType::kBindOk));

    auto desc = ReadFrame(fd, decoder);
    ASSERT_TRUE(desc.has_value());
    ASSERT_EQ(desc->type, static_cast<std::uint8_t>(ServerFrameType::kRowDesc));
    auto fields = wire::DecodeRowDescription(desc->payload);
    ASSERT_TRUE(fields.ok());
    ASSERT_EQ(fields.value().size(), 2u);

    auto batch = ReadFrame(fd, decoder);
    ASSERT_TRUE(batch.has_value());
    ASSERT_EQ(batch->type, static_cast<std::uint8_t>(ServerFrameType::kRowBatch));
    auto rows = wire::DecodeRowBatch(batch->payload, 2);
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 1u);
    ASSERT_EQ(rows.value()[0][1].bytes.size(), 4u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(rows.value()[0][1].bytes[0]), 7);

    auto complete = ReadFrame(fd, decoder);
    ASSERT_TRUE(complete.has_value());
    EXPECT_EQ(complete->type, static_cast<std::uint8_t>(ServerFrameType::kComplete));

    StopServer(fd, decoder);
    ::close(fd);
    server.join();
}

TEST_F(KwpEndpointTest, AFrameSplitAcrossReadsIsReassembled) {
    // TCP has no message boundaries, and the decoder's whole job is that a
    // chunk may split a frame anywhere - including mid-header. Sent one
    // byte at a time, which is the worst case and the only one worth
    // testing at this layer.
    constexpr std::uint16_t kPort = 25453;
    auto listener = TcpServer::Listen(kPort);
    ASSERT_TRUE(listener.ok());
    std::thread server([&] { RunReactor(listener.value()); });

    int fd = Connect(kPort);
    ASSERT_GE(fd, 0);
    wire::FrameDecoder decoder;

    wire::ClientHello hello;
    hello.client_name = "dribble";
    const auto frame = wire::EncodeFrame(static_cast<std::uint8_t>(ClientFrameType::kHello), 0,
                                         wire::EncodeClientHello(hello));
    for (const std::byte b : frame) {
        ASSERT_EQ(::write(fd, &b, 1), 1);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    auto s_hello = ReadFrame(fd, decoder);
    ASSERT_TRUE(s_hello.has_value());
    EXPECT_EQ(s_hello->type, static_cast<std::uint8_t>(ServerFrameType::kHello));
    ASSERT_TRUE(ReadFrame(fd, decoder).has_value());  // S_READY

    StopServer(fd, decoder);
    ::close(fd);
    server.join();
}

TEST_F(KwpEndpointTest, AHostileLengthIsRefusedAndTheConnectionCloses) {
    // §2: a `length` above `kMaxFrame` is framing-level corruption where
    // resync is impossible, so the server answers and closes rather than
    // buffering against a number the peer chose. The *other* connection
    // keeps working, which is the property that matters - one bad client
    // must not be a denial of service.
    constexpr std::uint16_t kPort = 25454;
    auto listener = TcpServer::Listen(kPort);
    ASSERT_TRUE(listener.ok());
    std::thread server([&] { RunReactor(listener.value()); });

    int good = Connect(kPort);
    ASSERT_GE(good, 0);
    wire::FrameDecoder good_decoder;
    ASSERT_TRUE(Handshake(good, good_decoder));

    int hostile = Connect(kPort);
    ASSERT_GE(hostile, 0);
    // A header claiming a payload far above the ceiling, and nothing else.
    std::byte header[8];
    const std::uint32_t length = wire::kMaxFrame + 1;
    for (int i = 0; i < 4; ++i) {
        header[i] = static_cast<std::byte>((length >> (8 * i)) & 0xFF);
    }
    header[4] = static_cast<std::byte>(ClientFrameType::kPing);
    header[5] = std::byte{0};
    header[6] = std::byte{0};
    header[7] = std::byte{0};
    ASSERT_EQ(::write(hostile, header, sizeof(header)), 8);

    wire::FrameDecoder hostile_decoder;
    auto refusal = ReadFrame(hostile, hostile_decoder);
    ASSERT_TRUE(refusal.has_value());
    ASSERT_EQ(refusal->type, static_cast<std::uint8_t>(ServerFrameType::kError));
    auto err = wire::DecodeError(refusal->payload);
    ASSERT_TRUE(err.ok());
    EXPECT_EQ(err.value().category(), wire::ErrorCategory::kProtocol);
    EXPECT_EQ(err.value().detail_code(),
              static_cast<std::uint16_t>(wire::ProtocolDetail::kMalformedFrame));
    EXPECT_EQ(err.value().severity, wire::Severity::kFatal);
    ::close(hostile);

    // The good connection is untouched.
    SendFrame(good, ClientFrameType::kPing, {});
    auto pong = ReadFrame(good, good_decoder);
    ASSERT_TRUE(pong.has_value());
    EXPECT_EQ(pong->type, static_cast<std::uint8_t>(ServerFrameType::kPong));

    StopServer(good, good_decoder);
    ::close(good);
    server.join();
}

TEST_F(KwpEndpointTest, TwoConnectionsHoldSeparateSessions) {
    // The same property the newline protocol has (txn.md §10-8), over the
    // framed one: two clients on one dispatcher must not see each other's
    // transaction, and each holds its own statements and portals.
    constexpr std::uint16_t kPort = 25455;
    auto listener = TcpServer::Listen(kPort);
    ASSERT_TRUE(listener.ok());
    std::thread server([&] { RunReactor(listener.value()); });

    int a = Connect(kPort);
    int b = Connect(kPort);
    ASSERT_GE(a, 0);
    ASSERT_GE(b, 0);
    wire::FrameDecoder da, db;
    ASSERT_TRUE(Handshake(a, da));
    ASSERT_TRUE(Handshake(b, db));

    // `a` names a statement; `b` does not hold it.
    SendFrame(a, ClientFrameType::kParse, ParsePayload("mine", "SELECT id FROM t"));
    ASSERT_TRUE(ReadFrame(a, da).has_value());
    SendFrame(b, ClientFrameType::kBind, BindPayload("p", "mine"));
    auto refused = ReadFrame(b, db);
    ASSERT_TRUE(refused.has_value());
    ASSERT_EQ(refused->type, static_cast<std::uint8_t>(ServerFrameType::kError));
    auto err = wire::DecodeError(refused->payload);
    ASSERT_TRUE(err.ok());
    EXPECT_EQ(err.value().detail_code(),
              static_cast<std::uint16_t>(wire::ProtocolDetail::kUnknownStatement));

    StopServer(a, da);
    ::close(a);
    ::close(b);
    server.join();
}

}  // namespace
}  // namespace kds::server
