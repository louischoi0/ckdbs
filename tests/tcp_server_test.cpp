#include "kds/server/tcp_server.hpp"

#include <algorithm>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/epoll_io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/storage/in_memory_page_store.hpp"

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
