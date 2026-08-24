#include "kds/server/tcp_server.hpp"

#include <algorithm>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/epoll_io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/storage/in_memory_page_store.hpp"

#if KDS_WITH_TLS
#include "kds/server/auth.hpp"
#include "kds/server/scram.hpp"
#endif

// Real loopback-socket integration test: starts a TcpServer on a
// background thread and talks to it as an actual client would, over a
// real socket. This is the one place sockets appear in the test suite -
// CommandDispatcher itself is tested socket-free in
// command_dispatcher_test.cpp.

namespace kds::server {
namespace {

class TcpServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
    }

    // Drives one attached TcpServer on a reactor until STOP stops it -
    // the same shape Expeditor::Serve() uses, so the tests exercise the
    // real path rather than a socket loop that no longer exists.
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

int ConnectToLoopback(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    for (int attempt = 0; attempt < 50; ++attempt) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::close(fd);
    return -1;
}

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

TEST(TcpServerListenTest, ReusePortAdmitsASecondListenerAndItsAbsenceRefusesOne) {
    // PW5's socket mechanics, pinned at the layer they live: with
    // SO_REUSEPORT on both sockets the kernel admits N listeners on one
    // port (the per-core accept fan-out of crosscore.md M3); without it
    // the second bind is EADDRINUSE. The flag must be on the *first*
    // binder too, which is why core 0's listener carries it whenever peer
    // listeners are configured.
    constexpr std::uint16_t kPort = 25431;
    auto first = TcpServer::Listen(kPort, /*reuse_port=*/true);
    ASSERT_TRUE(first.ok()) << first.status().message();
    auto second = TcpServer::Listen(kPort, /*reuse_port=*/true);
    EXPECT_TRUE(second.ok()) << second.status().message();

    constexpr std::uint16_t kExclusive = 25432;
    auto third = TcpServer::Listen(kExclusive);
    ASSERT_TRUE(third.ok()) << third.status().message();
    auto fourth = TcpServer::Listen(kExclusive);
    EXPECT_FALSE(fourth.ok());
}

TEST_F(TcpServerTest, PingRoundTripsOverRealSocket) {
    constexpr std::uint16_t kPort = 25411;
    auto listener = TcpServer::Listen(kPort);
    ASSERT_TRUE(listener.ok()) << listener.status().message();

    std::thread server_thread([&] { RunReactor(listener.value()); });

    int client = ConnectToLoopback(kPort);
    ASSERT_GE(client, 0);

    EXPECT_EQ(SendAndReceiveLine(client, "PING"), "PONG");
    EXPECT_EQ(SendAndReceiveLine(client, "STOP"), "OK bye");

    ::close(client);
    server_thread.join();
}

TEST_F(TcpServerTest, ServesMultipleCommandsBeforeStop) {
    constexpr std::uint16_t kPort = 25412;
    auto listener = TcpServer::Listen(kPort);
    ASSERT_TRUE(listener.ok()) << listener.status().message();

    std::thread server_thread([&] { RunReactor(listener.value()); });

    int client = ConnectToLoopback(kPort);
    ASSERT_GE(client, 0);

    EXPECT_EQ(SendAndReceiveLine(client, "PING"), "PONG");
    EXPECT_NE(SendAndReceiveLine(client, "SHOW META")
                  .find("version=" + std::to_string(server::kSuperBlockVersion)),
              std::string::npos);
    EXPECT_NE(SendAndReceiveLine(client, "DESCRIBE tables")
                  .find("oid=" + std::to_string(catalog::kSysTablesTable)),
              std::string::npos);
    EXPECT_EQ(SendAndReceiveLine(client, "STOP"), "OK bye");

    ::close(client);
    server_thread.join();
}

TEST_F(TcpServerTest, HandlesClientDisconnectThenAcceptsNextClient) {
    constexpr std::uint16_t kPort = 25413;
    auto listener = TcpServer::Listen(kPort);
    ASSERT_TRUE(listener.ok()) << listener.status().message();

    std::thread server_thread([&] { RunReactor(listener.value()); });

    int first = ConnectToLoopback(kPort);
    ASSERT_GE(first, 0);
    EXPECT_EQ(SendAndReceiveLine(first, "PING"), "PONG");
    ::close(first);  // disconnect without STOP

    int second = ConnectToLoopback(kPort);
    ASSERT_GE(second, 0);
    EXPECT_EQ(SendAndReceiveLine(second, "PING"), "PONG");
    EXPECT_EQ(SendAndReceiveLine(second, "STOP"), "OK bye");

    ::close(second);
    server_thread.join();
}

// ---- The async dispatch seam ------------------------------------------
//
// `Dispatch` is a coroutine now, so a reply is no longer produced inside the
// read handler. Nothing suspends yet - the executor is still synchronous -
// so what these pin is that the *plumbing* preserved every property the
// synchronous path had.

TEST_F(TcpServerTest, PipelinedCommandsAreAnsweredInOrder) {
    // The property one-statement-at-a-time exists to protect. A batch
    // arrives in one write; the replies must come back in the order the
    // commands were sent, because the newline protocol has no request ids
    // to match them up with.
    constexpr std::uint16_t kPort = 25414;
    auto listener = TcpServer::Listen(kPort);
    ASSERT_TRUE(listener.ok()) << listener.status().message();

    std::thread server_thread([&] { RunReactor(listener.value()); });

    int fd = ConnectToLoopback(kPort);
    ASSERT_GE(fd, 0);

    // Five commands, one write() - the pipelining case.
    const std::string batch = "PING\nPING\nPING\nPING\nPING\n";
    ASSERT_EQ(::write(fd, batch.data(), batch.size()), static_cast<ssize_t>(batch.size()));

    std::string response;
    char buf[256];
    int newlines = 0;
    while (newlines < 5) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        response.append(buf, static_cast<std::size_t>(n));
        newlines = static_cast<int>(std::count(response.begin(), response.end(), '\n'));
    }
    EXPECT_EQ(response, "PONG\nPONG\nPONG\nPONG\nPONG\n");

    EXPECT_EQ(SendAndReceiveLine(fd, "STOP"), "OK bye");
    ::close(fd);
    server_thread.join();
}

TEST_F(TcpServerTest, EachStatementSeesWhatTheOneBeforeItDid) {
    // A statement boundary is a task completion now, not a function return.
    // What must not change is that the next statement runs *after* the
    // previous one finished - so its effects are visible.
    //
    // Session/transaction semantics are covered directly at the dispatcher
    // level (txn_session_test.cpp); this fixture has no transaction manager
    // and does not need one to pin the ordering property.
    constexpr std::uint16_t kPort = 25415;
    auto listener = TcpServer::Listen(kPort);
    ASSERT_TRUE(listener.ok()) << listener.status().message();

    std::thread server_thread([&] { RunReactor(listener.value()); });

    int fd = ConnectToLoopback(kPort);
    ASSERT_GE(fd, 0);

    EXPECT_EQ(SendAndReceiveLine(fd, "CREATE TABLE t (id INT64, v INT64)").rfind("ERR", 0),
              std::string::npos);
    // Sent as one batch: the INSERT must not begin before the CREATE has
    // committed its catalog rows, or it resolves against a relation that
    // does not exist yet.
    const std::string batch = "INSERT INTO t VALUES (7)\nSELECT * FROM t\n";
    ASSERT_EQ(::write(fd, batch.data(), batch.size()), static_cast<ssize_t>(batch.size()));

    std::string response;
    char buf[512];
    while (std::count(response.begin(), response.end(), '\n') < 2) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        response.append(buf, static_cast<std::size_t>(n));
    }
    EXPECT_EQ(response.rfind("INSERTED", 0), 0u) << response;
    EXPECT_NE(response.find("7"), std::string::npos)
        << "the SELECT did not see the INSERT that preceded it: " << response;

    EXPECT_EQ(SendAndReceiveLine(fd, "STOP"), "OK bye");
    ::close(fd);
    server_thread.join();
}

// ---- The wire-channel seam --------------------------------------------
//
// The channel path (tcp_server.hpp's ChannelFactory) tested with a
// transform simple enough to invert by hand: every wire byte is the
// application byte XOR 0x5A. No OpenSSL, no handshake - what these pin
// is the *seam*: no plaintext byte reaches the wire, a fatal transform
// error closes only that connection, and the goodbye byte leaves behind
// the last reply, never in front of it.

class XorChannel final : public WireChannel {
public:
    static constexpr unsigned char kMask = 0x5A;
    static constexpr char kPoison = '\xFF';  // wire byte that kills the transform
    static constexpr char kBye = '#';        // stands in for TLS close_notify

    Status OnWireData(std::string_view wire_in, std::string& plain,
                      std::string& wire_out) override {
        for (char c : wire_in) {
            if (c == kPoison) {
                wire_out.push_back('!');  // the "alert" flushed before closing
                return Status::IoError("poison wire byte");
            }
            plain.push_back(static_cast<char>(static_cast<unsigned char>(c) ^ kMask));
        }
        return Status::OK();
    }
    Status Send(std::string_view plain, std::string& wire_out) override {
        for (char c : plain) {
            wire_out.push_back(static_cast<char>(static_cast<unsigned char>(c) ^ kMask));
        }
        return Status::OK();
    }
    void Close(std::string& wire_out) override { wire_out.push_back(kBye); }
};

std::string XorMask(std::string_view s) {
    std::string out;
    for (char c : s) out.push_back(static_cast<char>(static_cast<unsigned char>(c) ^ XorChannel::kMask));
    return out;
}

TEST_F(TcpServerTest, ChannelTransformsBothDirections) {
    constexpr std::uint16_t kPort = 25417;
    auto listener = TcpServer::Listen(kPort);
    ASSERT_TRUE(listener.ok()) << listener.status().message();
    listener.value().set_channel_factory([] { return std::make_unique<XorChannel>(); });

    std::thread server_thread([&] { RunReactor(listener.value()); });

    int fd = ConnectToLoopback(kPort);
    ASSERT_GE(fd, 0);

    // The command goes out in wire form; the reply must come back in wire
    // form - a raw "PONG" on the socket would be plaintext escaping the
    // channel.
    const std::string wire_cmd = XorMask("PING\n");
    ASSERT_EQ(::write(fd, wire_cmd.data(), wire_cmd.size()),
              static_cast<ssize_t>(wire_cmd.size()));
    const std::string want = XorMask("PONG\n");
    std::string got;
    char buf[64];
    while (got.size() < want.size()) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        got.append(buf, static_cast<std::size_t>(n));
    }
    EXPECT_EQ(got, want);

    // STOP: the encoded goodbye reply, then the channel's close byte
    // *behind* it - the ordering CloseClient's outbox path guarantees.
    const std::string wire_stop = XorMask("STOP\n");
    ASSERT_EQ(::write(fd, wire_stop.data(), wire_stop.size()),
              static_cast<ssize_t>(wire_stop.size()));
    std::string tail;
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;  // server closed after STOP
        tail.append(buf, static_cast<std::size_t>(n));
    }
    EXPECT_EQ(tail, XorMask("OK bye\n") + XorChannel::kBye);

    ::close(fd);
    server_thread.join();
}

TEST_F(TcpServerTest, ChannelFatalErrorClosesOnlyThatConnection) {
    constexpr std::uint16_t kPort = 25418;
    auto listener = TcpServer::Listen(kPort);
    ASSERT_TRUE(listener.ok()) << listener.status().message();
    listener.value().set_channel_factory([] { return std::make_unique<XorChannel>(); });

    std::thread server_thread([&] { RunReactor(listener.value()); });

    int doomed = ConnectToLoopback(kPort);
    ASSERT_GE(doomed, 0);
    const char poison = XorChannel::kPoison;
    ASSERT_EQ(::write(doomed, &poison, 1), 1);
    // The server flushes the channel's alert and goodbye, then closes: the
    // socket must reach EOF, and the last byte before it is the goodbye.
    std::string dying;
    char buf[16];
    while (true) {
        ssize_t n = ::read(doomed, buf, sizeof(buf));
        if (n <= 0) break;
        dying.append(buf, static_cast<std::size_t>(n));
    }
    EXPECT_EQ(dying, std::string("!") + XorChannel::kBye);
    ::close(doomed);

    // The listener survived and serves the next channelled client.
    int next = ConnectToLoopback(kPort);
    ASSERT_GE(next, 0);
    const std::string wire_stop = XorMask("STOP\n");
    ASSERT_EQ(::write(next, wire_stop.data(), wire_stop.size()),
              static_cast<ssize_t>(wire_stop.size()));
    std::string tail;
    while (true) {
        ssize_t n = ::read(next, buf, sizeof(buf));
        if (n <= 0) break;
        tail.append(buf, static_cast<std::size_t>(n));
    }
    EXPECT_EQ(tail, XorMask("OK bye\n") + XorChannel::kBye);
    ::close(next);
    server_thread.join();
}

// ---- The authentication gate ------------------------------------------
//
// The gate's own logic is covered socket-free in scram_test.cpp; what
// these pin is the *routing*: an unauthenticated line never reaches the
// dispatcher, a refused connection closes without taking the server
// down, and a completed exchange hands the connection to the dispatcher
// it was kept from.

#if KDS_WITH_TLS
TEST_F(TcpServerTest, AuthGateRefusesThenAdmitsOverRealSocket) {
    constexpr std::uint16_t kPort = 25419;
    auto verifier = scram::DeriveVerifier("pencil", "0123456789abcdef", 4096);
    ASSERT_TRUE(verifier.ok());
    auto store = FileCredentialStore::Parse("user admin " + verifier.value().Serialize() +
                                                "\nviewer readonly " +
                                                verifier.value().Serialize() + "\n",
                                            "users.probe");
    ASSERT_TRUE(store.ok()) << store.status().message();

    auto listener = TcpServer::Listen(kPort);
    ASSERT_TRUE(listener.ok()) << listener.status().message();
    listener.value().set_auth_gate_factory(
        [s = &store.value()] { return std::make_unique<ScramAuthGate>(s); });

    std::thread server_thread([&] { RunReactor(listener.value()); });

    // An anonymous statement - STOP, of all things - is refused and the
    // connection closed; the server must survive it. Sent as *one*
    // pipelined write with a second statement behind it, because a batch
    // is the way a line would slip past a gate that drained before it
    // checked: exactly one ERR comes back, the socket reaches EOF, and
    // the PING never runs (the server is still up two blocks below).
    int anon = ConnectToLoopback(kPort);
    ASSERT_GE(anon, 0);
    const std::string refused = SendAndReceiveLine(anon, "STOP\nPING");
    EXPECT_EQ(refused.rfind("ERR ", 0), 0u) << refused;
    EXPECT_EQ(refused.find('\n'), std::string::npos)
        << "only the gate may answer an unauthenticated batch: " << refused;
    char probe = 0;
    // Anything but EOF here is the second line having been served.
    EXPECT_EQ(::read(anon, &probe, 1), 0) << "the refused connection must reach EOF";
    ::close(anon);

    // A readonly user clears the gate but not the write wall: the role
    // travelled gate -> session -> dispatcher through the whole stack.
    int viewer = ConnectToLoopback(kPort);
    ASSERT_GE(viewer, 0);
    scram::Client viewer_client("viewer", "pencil");
    std::string vf = SendAndReceiveLine(viewer, "AUTH SCRAM-SHA-256 " + viewer_client.First());
    ASSERT_EQ(vf.rfind("AUTH+ ", 0), 0u) << vf;
    auto vcf = viewer_client.OnServerFirst(vf.substr(6));
    ASSERT_TRUE(vcf.ok());
    ASSERT_EQ(SendAndReceiveLine(viewer, "AUTH " + vcf.value()).rfind("AUTH+ ", 0), 0u);
    EXPECT_EQ(SendAndReceiveLine(viewer, "PING"), "PONG");
    std::string refusal = SendAndReceiveLine(viewer, "INSERT INTO t VALUES (1)");
    EXPECT_EQ(refusal.rfind("ERR permission: ", 0), 0u) << refusal;
    EXPECT_NE(refusal.find("this connection is readonly"), std::string::npos) << refusal;
    ::close(viewer);

    // The real exchange, then the dispatcher path as usual.
    int fd = ConnectToLoopback(kPort);
    ASSERT_GE(fd, 0);
    scram::Client client("user", "pencil");
    std::string server_first = SendAndReceiveLine(fd, "AUTH SCRAM-SHA-256 " + client.First());
    ASSERT_EQ(server_first.rfind("AUTH+ ", 0), 0u) << server_first;
    auto client_final = client.OnServerFirst(server_first.substr(6));
    ASSERT_TRUE(client_final.ok()) << client_final.status().message();
    std::string server_final = SendAndReceiveLine(fd, "AUTH " + client_final.value());
    ASSERT_EQ(server_final.rfind("AUTH+ ", 0), 0u) << server_final;
    EXPECT_TRUE(client.OnServerFinal(server_final.substr(6)).ok());

    EXPECT_EQ(SendAndReceiveLine(fd, "PING"), "PONG");
    EXPECT_EQ(SendAndReceiveLine(fd, "STOP"), "OK bye");
    ::close(fd);
    server_thread.join();
}

TEST_F(TcpServerTest, WrongPasswordOverSocketClosesAndServerSurvives) {
    constexpr std::uint16_t kPort = 25420;
    auto verifier = scram::DeriveVerifier("pencil", "0123456789abcdef", 4096);
    ASSERT_TRUE(verifier.ok());
    auto store = FileCredentialStore::Parse("user admin " + verifier.value().Serialize() + "\n",
                                            "users.probe");
    ASSERT_TRUE(store.ok());

    auto listener = TcpServer::Listen(kPort);
    ASSERT_TRUE(listener.ok()) << listener.status().message();
    listener.value().set_auth_gate_factory(
        [s = &store.value()] { return std::make_unique<ScramAuthGate>(s); });

    std::thread server_thread([&] { RunReactor(listener.value()); });

    int fd = ConnectToLoopback(kPort);
    ASSERT_GE(fd, 0);
    scram::Client client("user", "wrong");
    std::string server_first = SendAndReceiveLine(fd, "AUTH SCRAM-SHA-256 " + client.First());
    ASSERT_EQ(server_first.rfind("AUTH+ ", 0), 0u);
    auto client_final = client.OnServerFirst(server_first.substr(6));
    ASSERT_TRUE(client_final.ok());
    EXPECT_EQ(SendAndReceiveLine(fd, "AUTH " + client_final.value()).rfind("ERR ", 0), 0u);
    char probe = 0;
    EXPECT_EQ(::read(fd, &probe, 1), 0);
    ::close(fd);

    // An authorized client still gets in and can stop the server.
    int fd2 = ConnectToLoopback(kPort);
    ASSERT_GE(fd2, 0);
    scram::Client good("user", "pencil");
    std::string sf = SendAndReceiveLine(fd2, "AUTH SCRAM-SHA-256 " + good.First());
    auto cf = good.OnServerFirst(sf.substr(6));
    ASSERT_TRUE(cf.ok());
    ASSERT_EQ(SendAndReceiveLine(fd2, "AUTH " + cf.value()).rfind("AUTH+ ", 0), 0u);
    EXPECT_EQ(SendAndReceiveLine(fd2, "STOP"), "OK bye");
    ::close(fd2);
    server_thread.join();
}
#endif  // KDS_WITH_TLS

TEST_F(TcpServerTest, ADisconnectMidBatchDoesNotTakeTheServerDown) {
    // The path Connection::closing exists for: the client goes away while
    // the server still has work queued for it. Nothing may dangle, and the
    // next client must still be served.
    constexpr std::uint16_t kPort = 25416;
    auto listener = TcpServer::Listen(kPort);
    ASSERT_TRUE(listener.ok()) << listener.status().message();

    std::thread server_thread([&] { RunReactor(listener.value()); });

    int fd = ConnectToLoopback(kPort);
    ASSERT_GE(fd, 0);
    const std::string batch = "PING\nPING\nPING\nPING\n";
    ASSERT_EQ(::write(fd, batch.data(), batch.size()), static_cast<ssize_t>(batch.size()));
    // Hang up immediately, without reading a single reply.
    ::close(fd);

    int next = ConnectToLoopback(kPort);
    ASSERT_GE(next, 0);
    EXPECT_EQ(SendAndReceiveLine(next, "PING"), "PONG");
    EXPECT_EQ(SendAndReceiveLine(next, "STOP"), "OK bye");
    ::close(next);
    server_thread.join();
}

}  // namespace
}  // namespace kds::server
