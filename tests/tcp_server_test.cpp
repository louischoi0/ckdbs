#include "kds/server/tcp_server.hpp"

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

}  // namespace
}  // namespace kds::server
