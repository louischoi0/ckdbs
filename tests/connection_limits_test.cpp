#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/epoll_io_backend.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/connection_limits.hpp"
#include "kds/server/tcp_server.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/wire/error_registry.hpp"
#include "kds/wire/handshake.hpp"
#include "kds/wire/kwp.hpp"
#include "kds/wire/kwp_types.hpp"
#include "kwp_test_client.hpp"

// **The connection cap and the idle-session timeout, over a real socket**
// (DJ-S1, `docs/spec/protocol.md` §10;
// `instructions/v3.2.0/workorder-dj-connections-sessions-cursors.md` §5).
//
// One listener on one reactor, which is where the seat is taken and the
// sweep runs; the instance-level halves - one count above one core, a
// session that moves between cores, a transaction and a statement in flight
// under the sweep, the keys as a file spells them - are `expeditor_test.cpp`'s
// and `config_file_test.cpp`'s.

namespace kds::server {
namespace {

using namespace kwp_test;
using wire::ClientFrameType;
using wire::ServerFrameType;

// The system clock plus a jump the test thread makes while the reactor
// thread reads it (`kwp_endpoint_test.cpp`'s, for the same reason).
class JumpableClock final : public sched::Clock {
public:
    sched::MonoTimeNs Now() const override {
        return base_.Now() + offset_.load(std::memory_order_acquire);
    }
    void Jump(sched::MonoTimeNs delta_ns) noexcept {
        offset_.fetch_add(delta_ns, std::memory_order_release);
    }

private:
    sched::SystemClock base_;
    std::atomic<sched::MonoTimeNs> offset_{0};
};

// A gate that admits on its first line and stamps the role the line names:
// what a cell needs of an authentication exchange is its outcome.
class RoleGate final : public AuthGate {
public:
    Result OnLine(std::string_view line) override {
        Result r;
        r.reply = "AUTH OK";
        auto role = ParseRole(line);
        if (!role.ok()) {
            r.reply = "AUTH FAILED";
            r.close = true;
            return r;
        }
        r.authenticated = true;
        r.username = "u";
        r.role = role.value();
        return r;
    }
};

constexpr sched::MonoTimeNs kSecond = 1'000'000'000ULL;

class ConnectionLimitsTest : public ::testing::Test {
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

    // Listens on `port` under `setup` and serves on a thread until `Stop`.
    void Serve(std::uint16_t port, TcpServer::ClientSetup setup) {
        auto listener = TcpServer::Listen(port);
        ASSERT_TRUE(listener.ok()) << listener.status().message();
        listener_.emplace(std::move(listener.value()));
        listener_->Configure(setup);
        server_ = std::thread([this] {
            auto io_backend = sched::EpollIoBackend::Create();
            ASSERT_TRUE(io_backend.ok()) << io_backend.status().message();
            sched::Scheduler scheduler(clock_, io_backend.value());
            scheduler_.store(&scheduler, std::memory_order_release);
            ASSERT_TRUE(listener_->Attach(scheduler, *dispatcher_).ok());
            scheduler.Run();
            listener_->Detach();
            scheduler_.store(nullptr, std::memory_order_release);
        });
    }

    // Ends the reactor from outside, whatever the cap has left of the port:
    // a cell at a full server cannot send `STOP` through a session it would
    // be refused.
    void Stop() {
        if (!server_.joinable()) return;
        for (int i = 0; i < 500 && scheduler_.load(std::memory_order_acquire) == nullptr; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // The stop flag is atomic and read between blocks, and an idle block
        // is bounded (`max_idle_block_ms`), so nothing has to wake it.
        if (sched::Scheduler* s = scheduler_.load(std::memory_order_acquire); s != nullptr) {
            s->Stop();
        }
        server_.join();
    }

    void TearDown() override { Stop(); }

    // Waits for `limits_.seats` to read `want`: a close is seen by the
    // reactor at its next read, not when the client's `close` returns.
    bool SeatsBecome(std::uint32_t want) {
        for (int i = 0; i < 1000; ++i) {
            if (limits_.seats.load(std::memory_order_relaxed) == want) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    JumpableClock clock_;
    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
    ConnectionLimits limits_;
    std::optional<TcpServer> listener_;
    std::thread server_;
    std::atomic<sched::Scheduler*> scheduler_{nullptr};
    std::uint16_t port_ = 0;
};

// One client connection: its socket and its decoder.
struct Client {
    explicit Client(std::uint16_t port) : fd(Connect(port)) {}
    ~Client() { Close(); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    void Close() {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }

    // `C_HELLO`, and the first frame the server answers with.
    std::optional<wire::DecodedFrame> Hello(std::uint8_t auth_method = wire::kAuthNone) {
        wire::ClientHello hello;
        hello.client_name = "dj-s1";
        hello.auth_method = auth_method;
        SendFrame(fd, ClientFrameType::kHello, wire::EncodeClientHello(hello));
        if (auth_method != wire::kAuthNone) return std::nullopt;  // the exchange is owed first
        return ReadFrame(fd, decoder);
    }

    // The whole unauthenticated handshake; keeps `S_HELLO`'s identifiers.
    bool Admitted() {
        auto first = Hello();
        if (!first.has_value() || first->type != static_cast<std::uint8_t>(ServerFrameType::kHello)) {
            refusal = first;
            return false;
        }
        auto hello = wire::DecodeServerHello(first->payload);
        if (!hello.ok()) return false;
        session_id = hello.value().session_id;
        cancel_key = hello.value().cancel_key;
        auto ready = ReadFrame(fd, decoder);
        return ready.has_value() &&
               ready->type == static_cast<std::uint8_t>(ServerFrameType::kReady);
    }

    // `C_HELLO` asking for an exchange, then one `C_AUTH` naming `role`.
    // True when `S_HELLO` and `S_READY` followed the gate's answer;
    // otherwise `refusal` holds what came instead of `S_HELLO`.
    bool AdmittedAs(std::string_view role) {
        (void)Hello(wire::kAuthScramSha256);
        wire::PayloadWriter w;
        w.Text(role);
        SendFrame(fd, ClientFrameType::kAuth, w.Take());
        auto auth = ReadFrame(fd, decoder);
        if (!auth.has_value()) return false;
        if (auth->type != static_cast<std::uint8_t>(ServerFrameType::kAuth)) {
            refusal = auth;
            return false;
        }
        auto next = ReadFrame(fd, decoder);
        if (!next.has_value() || next->type != static_cast<std::uint8_t>(ServerFrameType::kHello)) {
            refusal = next;
            return false;
        }
        auto ready = ReadFrame(fd, decoder);
        return ready.has_value() &&
               ready->type == static_cast<std::uint8_t>(ServerFrameType::kReady);
    }

    bool Ping() {
        SendFrame(fd, ClientFrameType::kPing, {});
        auto pong = ReadFrame(fd, decoder);
        return pong.has_value() && pong->type == static_cast<std::uint8_t>(ServerFrameType::kPong);
    }

    // One statement through the unnamed statement and portal "p", closed
    // behind it unless the cell leaves it; true when it completed.
    bool Run(std::string_view sql, bool close_portal = true) {
        SendFrame(fd, ClientFrameType::kParse, ParsePayload("", sql));
        SendFrame(fd, ClientFrameType::kBind, BindPayload("p", ""));
        SendFrame(fd, ClientFrameType::kExecute, ExecutePayload("p", 0));
        if (close_portal) {
            wire::PayloadWriter close;
            close.U8(2);
            close.Str("p");
            SendFrame(fd, ClientFrameType::kClose, close.Take());
        }
        SendFrame(fd, ClientFrameType::kSync, {});
        bool completed = false;
        while (true) {
            auto f = ReadFrame(fd, decoder);
            if (!f.has_value()) return false;
            if (f->type == static_cast<std::uint8_t>(ServerFrameType::kComplete)) completed = true;
            if (f->type == static_cast<std::uint8_t>(ServerFrameType::kError)) refusal = f;
            if (f->type == static_cast<std::uint8_t>(ServerFrameType::kReady)) return completed;
        }
    }

    // The error frame the server sent in place of what was asked for.
    wire::WireError Refusal() const {
        if (!refusal.has_value() ||
            refusal->type != static_cast<std::uint8_t>(ServerFrameType::kError)) {
            return {};
        }
        auto e = wire::DecodeError(refusal->payload);
        return e.ok() ? e.value() : wire::WireError{};
    }

    // Reads until the server closes; the last error frame seen is `refusal`.
    // False when `timeout_ms` passes with the connection still open.
    bool ClosedByServer(int timeout_ms = 5000) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < until) {
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) continue;
            auto f = ReadFrame(fd, decoder);
            if (!f.has_value()) return true;
            if (f->type == static_cast<std::uint8_t>(ServerFrameType::kError)) refusal = f;
        }
        return false;
    }

    int fd;
    wire::FrameDecoder decoder;
    std::uint64_t session_id = 0;
    std::uint64_t cancel_key = 0;
    std::optional<wire::DecodedFrame> refusal;
};

constexpr auto kConnectionLimit = static_cast<std::uint16_t>(wire::ResourceDetail::kConnectionLimit);
constexpr auto kIdleSessionTimeout =
    static_cast<std::uint16_t>(wire::ResourceDetail::kIdleSessionTimeout);

// ---- The cap --------------------------------------------------------------

TEST_F(ConnectionLimitsTest, ASessionPastTheCapIsRefusedByNameAndOneBelowItIsServed) {
    port_ = 25811;
    limits_.max_connections = 2;
    TcpServer::ClientSetup setup;
    setup.limits = &limits_;
    Serve(port_, setup);

    Client a(port_), b(port_);
    ASSERT_TRUE(a.Admitted());
    ASSERT_TRUE(b.Admitted());
    EXPECT_EQ(limits_.seats.load(), 2u);

    // The third is told which limit it met, by the pinned code and the
    // key's name (DJ-R4), and is closed: the refusal is fatal.
    Client c(port_);
    ASSERT_FALSE(c.Admitted());
    const wire::WireError refused = c.Refusal();
    EXPECT_EQ(refused.category(), wire::ErrorCategory::kResourceExhausted);
    EXPECT_EQ(refused.detail_code(), kConnectionLimit);
    EXPECT_EQ(refused.severity, wire::Severity::kFatal);
    EXPECT_NE(refused.message.find("max_connections = 2"), std::string::npos) << refused.message;
    EXPECT_TRUE(c.ClosedByServer());
    EXPECT_EQ(limits_.refused.load(), 1u);
    EXPECT_EQ(limits_.seats.load(), 2u) << "a refused session holds no seat";

    // The sessions inside the cap are served as before.
    EXPECT_TRUE(a.Run("SELECT id FROM t"));
    EXPECT_TRUE(b.Ping());

    // A seat given back is a seat: the next session is admitted.
    b.Close();
    ASSERT_TRUE(SeatsBecome(1));
    Client d(port_);
    EXPECT_TRUE(d.Admitted());
    EXPECT_EQ(limits_.seats.load(), 2u);
}

TEST_F(ConnectionLimitsTest, WithNoCapEverySessionIsAdmittedAndCounted) {
    // DJ-R1: `max_connections = 0` refuses nobody. The count still moves,
    // which is what `sys.meta` reads.
    port_ = 25812;
    TcpServer::ClientSetup setup;
    setup.limits = &limits_;
    Serve(port_, setup);

    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < 40; ++i) {
        clients.push_back(std::make_unique<Client>(port_));
        ASSERT_TRUE(clients.back()->Admitted()) << "session " << i;
    }
    EXPECT_EQ(limits_.seats.load(), 40u);
    EXPECT_EQ(limits_.refused.load(), 0u);
    clients.clear();
    EXPECT_TRUE(SeatsBecome(0));
}

TEST_F(ConnectionLimitsTest, ACancelIsAdmittedAtAFullServerAndItsTargetIsCancelled) {
    // DJ-R2 `[quiet-wrong]`: a cancel connection holds no seat, so a server
    // with none left still takes it. **The mutation**: take the seat in
    // `AdoptConnection` instead of where the handshake ends, and the cancel
    // is refused at the door with nothing said - its target's next frame is
    // then answered, not refused.
    port_ = 25813;
    limits_.max_connections = 1;
    CancelRegistry cancels;
    std::atomic<std::uint64_t> next{0x1234567800000000ull};
    TcpServer::ClientSetup setup;
    setup.limits = &limits_;
    setup.cancels = &cancels;
    setup.identity = [&next] { return next.fetch_add(0x9E3779B97F4A7C15ull); };
    Serve(port_, setup);

    Client target(port_);
    ASSERT_TRUE(target.Admitted());
    ASSERT_NE(target.session_id, 0u);
    ASSERT_EQ(limits_.seats.load(), 1u);

    // The server is full: an ordinary session is refused...
    Client late(port_);
    ASSERT_FALSE(late.Admitted());
    EXPECT_EQ(late.Refusal().detail_code(), kConnectionLimit);

    // ...and the cancel is not. It is answered with nothing and closed.
    Client cancel(port_);
    wire::CancelRequest request;
    request.session_id = target.session_id;
    request.cancel_key = target.cancel_key;
    SendFrame(cancel.fd, ClientFrameType::kCancel, wire::EncodeCancelRequest(request));
    EXPECT_TRUE(cancel.ClosedByServer());
    EXPECT_FALSE(cancel.refusal.has_value()) << "a cancel is answered with nothing";
    EXPECT_EQ(limits_.seats.load(), 1u) << "a cancel connection holds no seat";

    // The target's next frame is the one refused `CANCELLED`.
    EXPECT_FALSE(target.Run("SELECT id FROM t"));
    EXPECT_EQ(target.Refusal().category(), wire::ErrorCategory::kCancelled);

    // Before this cell's registry and counter go: the listener holds both.
    Stop();
}

TEST_F(ConnectionLimitsTest, TheReserveAdmitsAnAdminAndRefusesAReadWrite) {
    port_ = 25814;
    limits_.max_connections = 1;
    limits_.reserved_admin_connections = 1;
    TcpServer::ClientSetup setup;
    setup.limits = &limits_;
    setup.auth = [] { return std::make_unique<RoleGate>(); };
    Serve(port_, setup);

    // The one general seat.
    Client writer(port_);
    ASSERT_TRUE(writer.AdmittedAs("readwrite"));
    EXPECT_EQ(limits_.seats.load(), 1u);

    // A second `readwrite` is let through its exchange - its role is not
    // known before it - and refused once it is: the seat left is admin's.
    // Its `S_AUTH` came first; the refusal stands where `S_HELLO` would.
    Client second(port_);
    ASSERT_FALSE(second.AdmittedAs("readwrite"));
    const wire::WireError refused = second.Refusal();
    EXPECT_EQ(refused.category(), wire::ErrorCategory::kResourceExhausted);
    EXPECT_EQ(refused.detail_code(), kConnectionLimit);
    EXPECT_EQ(refused.severity, wire::Severity::kFatal);
    EXPECT_NE(refused.message.find("reserved_admin_connections = 1"), std::string::npos)
        << refused.message;
    EXPECT_NE(refused.message.find("readwrite"), std::string::npos) << refused.message;
    EXPECT_TRUE(second.ClosedByServer());
    EXPECT_EQ(limits_.seats.load(), 1u) << "a refused session took no seat";

    // An `admin` takes it.
    Client admin(port_);
    ASSERT_TRUE(admin.AdmittedAs("admin"));
    EXPECT_EQ(limits_.seats.load(), 2u);
    EXPECT_TRUE(admin.Ping());

    // And past the reserve an `admin` is refused too, told both numbers.
    Client third(port_);
    ASSERT_FALSE(third.AdmittedAs("admin"));
    EXPECT_EQ(third.Refusal().detail_code(), kConnectionLimit);
    EXPECT_NE(third.Refusal().message.find("max_connections = 1"), std::string::npos);
    EXPECT_NE(third.Refusal().message.find("reserved_admin_connections = 1 past them"),
              std::string::npos)
        << third.Refusal().message;
    EXPECT_EQ(limits_.refused.load(), 2u);
    EXPECT_EQ(limits_.seats.load(), 2u);
}

TEST_F(ConnectionLimitsTest, AConnectionStillInItsExchangeHoldsNoSeat) {
    // The seat is taken once the role is known. **The mutation**: take it
    // at `C_HELLO`, and a peer with no credentials fills the cap and the
    // reserve by saying hello - here, the `admin` below is refused.
    port_ = 25826;
    limits_.max_connections = 1;
    limits_.reserved_admin_connections = 1;
    TcpServer::ClientSetup setup;
    setup.limits = &limits_;
    setup.auth = [] { return std::make_unique<RoleGate>(); };
    Serve(port_, setup);

    std::vector<std::unique_ptr<Client>> strangers;
    for (int i = 0; i < 6; ++i) {
        strangers.push_back(std::make_unique<Client>(port_));
        (void)strangers.back()->Hello(wire::kAuthScramSha256);
    }
    // Served in order, so by the time this session is admitted every hello
    // above has been read.
    Client writer(port_);
    ASSERT_TRUE(writer.AdmittedAs("readwrite"));
    EXPECT_EQ(limits_.seats.load(), 1u);
    Client admin(port_);
    EXPECT_TRUE(admin.AdmittedAs("admin"));
    EXPECT_EQ(limits_.seats.load(), 2u);
    EXPECT_EQ(limits_.refused.load(), 0u);
}

TEST_F(ConnectionLimitsTest, WithNoGateNothingIsReserved) {
    // Where no gate stamps a role every session is `admin` (`session.hpp`),
    // so a reserve would be seats for everyone: the ceiling is
    // `max_connections` itself.
    port_ = 25815;
    limits_.max_connections = 1;
    limits_.reserved_admin_connections = 3;
    TcpServer::ClientSetup setup;
    setup.limits = &limits_;
    Serve(port_, setup);

    Client a(port_);
    ASSERT_TRUE(a.Admitted());
    Client b(port_);
    EXPECT_FALSE(b.Admitted());
    EXPECT_EQ(b.Refusal().detail_code(), kConnectionLimit);
}

TEST_F(ConnectionLimitsTest, TheCountReturnsToZeroAfterEveryClosePath) {
    port_ = 25816;
    limits_.max_connections = 8;
    limits_.reserved_admin_connections = 2;
    TcpServer::ClientSetup setup;
    setup.limits = &limits_;
    setup.auth = [] { return std::make_unique<RoleGate>(); };
    Serve(port_, setup);

    {
        // A hangup of a served session.
        Client served(port_);
        ASSERT_TRUE(served.AdmittedAs("readonly"));
        // A hangup in the middle of the exchange, which holds no seat.
        Client mid(port_);
        (void)mid.Hello(wire::kAuthScramSha256);
        // An exchange the gate refuses.
        Client failed(port_);
        EXPECT_FALSE(failed.AdmittedAs("nobody"));
        // A connection that never says hello holds no seat at all.
        Client silent(port_);
        ASSERT_GE(silent.fd, 0);
        // A statement's session, closed with `C_TERMINATE`.
        Client polite(port_);
        ASSERT_TRUE(polite.AdmittedAs("admin"));
        SendFrame(polite.fd, ClientFrameType::kTerminate, {});
        EXPECT_TRUE(polite.ClosedByServer());
        ASSERT_TRUE(SeatsBecome(1)) << "served holds the one seat; seats="
                                    << limits_.seats.load();
    }
    EXPECT_TRUE(SeatsBecome(0)) << "seats=" << limits_.seats.load();

    // And a listener detached under live sessions gives every seat back.
    Client left(port_), left_too(port_);
    ASSERT_TRUE(left.AdmittedAs("readwrite"));
    ASSERT_TRUE(left_too.AdmittedAs("admin"));
    ASSERT_EQ(limits_.seats.load(), 2u);
    Stop();
    EXPECT_EQ(limits_.seats.load(), 0u);
}

// ---- The idle-session timeout ---------------------------------------------

TEST_F(ConnectionLimitsTest, AnIdleSessionIsClosedAtTheTimeoutAndToldWhy) {
    port_ = 25817;
    TcpServer::ClientSetup setup;
    setup.limits = &limits_;
    setup.idle_session_timeout_ns = 600 * kSecond;
    Serve(port_, setup);

    Client c(port_);
    ASSERT_TRUE(c.Admitted());
    // Its portal is left open and spent - `S_COMPLETE` sent - as a client
    // that reuses the unnamed portal leaves it: a name, not a held portal,
    // or such a client would be idle only once the portal timeout ran.
    ASSERT_TRUE(c.Run("SELECT id FROM t", /*close_portal=*/false));

    // Short of the timeout nothing happens, however many ticks pass.
    clock_.Jump(590 * kSecond);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(c.Ping()) << "closed before its timeout";

    // Past it - counted from the tick that saw the statement, so a whole
    // timeout more - the session is told why and closed.
    clock_.Jump(601 * kSecond);
    ASSERT_TRUE(c.ClosedByServer());
    const wire::WireError told = c.Refusal();
    EXPECT_EQ(told.category(), wire::ErrorCategory::kResourceExhausted);
    EXPECT_EQ(told.detail_code(), kIdleSessionTimeout);
    EXPECT_EQ(told.severity, wire::Severity::kFatal);
    EXPECT_NE(told.message.find("idle_session_timeout_s = 600"), std::string::npos)
        << told.message;
    EXPECT_EQ(limits_.idle_closes.load(), 1u);
    EXPECT_TRUE(SeatsBecome(0));
}

TEST_F(ConnectionLimitsTest, APingDoesNotKeepAnIdleSessionOpenAndAStatementDoes) {
    // DJ-Q5 (a). Two sessions, the same instants: one pings at each, the
    // other runs a statement at each. **The mutation**: mark `worked` for a
    // `C_PING`, and the pinging session outlives its timeout.
    port_ = 25818;
    TcpServer::ClientSetup setup;
    setup.idle_session_timeout_ns = 600 * kSecond;
    Serve(port_, setup);

    Client pinger(port_), worker(port_);
    ASSERT_TRUE(pinger.Admitted());
    ASSERT_TRUE(worker.Admitted());
    for (int step = 0; step < 3; ++step) {
        clock_.Jump(250 * kSecond);
        // The worker's statement is seen by the tick the next jump brings,
        // at the latest - a tick is 75 s of this clock, so none falls in the
        // sleep below, which only lets the reactor run the one the jump made
        // due.
        ASSERT_TRUE(worker.Run("SELECT id FROM t")) << "step " << step;
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        if (step < 2) {
            ASSERT_TRUE(pinger.Ping()) << "step " << step;
        }
    }
    // 750 s on: the pinger pinged at 250 and 500 and is closed all the
    // same; the worker ran a statement inside every 600 s and is not.
    EXPECT_TRUE(pinger.ClosedByServer());
    EXPECT_EQ(pinger.Refusal().detail_code(), kIdleSessionTimeout);
    EXPECT_TRUE(worker.Ping());
}

TEST_F(ConnectionLimitsTest, ASessionHoldingAPortalIsNotClosedUntilThePortalGoes) {
    // DJ-R3: a held portal is the portal timeout's, not the session
    // timeout's. The session timeout here is under the portal's 60 s - a
    // value the config file refuses and a listener takes - so the portal
    // outlives several of it.
    port_ = 25819;
    TcpServer::ClientSetup setup;
    setup.idle_session_timeout_ns = 10 * kSecond;
    Serve(port_, setup);

    Client c(port_);
    ASSERT_TRUE(c.Admitted());
    SendFrame(c.fd, ClientFrameType::kParse, ParsePayload("s", "SELECT id FROM t"));
    SendFrame(c.fd, ClientFrameType::kBind, BindPayload("p", "s"));
    ASSERT_TRUE(ReadFrame(c.fd, c.decoder).has_value());  // S_PARSE_OK
    ASSERT_TRUE(ReadFrame(c.fd, c.decoder).has_value());  // S_BIND_OK

    clock_.Jump(45 * kSecond);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(c.Ping()) << "closed while it held a portal";

    // The portal closed, the session holds nothing: one timeout on it goes.
    wire::PayloadWriter close;
    close.U8(2);
    close.Str("p");
    SendFrame(c.fd, ClientFrameType::kClose, close.Take());
    ASSERT_TRUE(c.Ping());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    clock_.Jump(11 * kSecond);
    EXPECT_TRUE(c.ClosedByServer());
    EXPECT_EQ(c.Refusal().detail_code(), kIdleSessionTimeout);
}

TEST_F(ConnectionLimitsTest, ASpentPortalDoesNotHoldASessionOpen) {
    // A portal whose `S_COMPLETE` has gone is a name, not something held: a
    // client that reuses the unnamed portal and never closes it is idle all
    // the same. The timeout is under the portal's 60 s, so the portal sweep
    // has not released it. **The mutation**: read `portal_count()` in the
    // sweep, and this session outlives its timeout by the portal's.
    port_ = 25825;
    TcpServer::ClientSetup setup;
    setup.idle_session_timeout_ns = 10 * kSecond;
    Serve(port_, setup);

    Client c(port_);
    ASSERT_TRUE(c.Admitted());
    ASSERT_TRUE(c.Run("SELECT id FROM t", /*close_portal=*/false));
    // The tick this jump makes due sees the statement and counts from it.
    clock_.Jump(2 * kSecond);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(c.Ping());
    clock_.Jump(11 * kSecond);
    EXPECT_TRUE(c.ClosedByServer());
    EXPECT_EQ(c.Refusal().detail_code(), kIdleSessionTimeout);
}

TEST_F(ConnectionLimitsTest, AConnectionThatNeverSaysHelloIsClosedAtTheTimeoutToo) {
    port_ = 25820;
    TcpServer::ClientSetup setup;
    setup.idle_session_timeout_ns = 600 * kSecond;
    Serve(port_, setup);

    Client silent(port_);
    ASSERT_GE(silent.fd, 0);
    // Accepted before the jump, so the timeout is counted from its accept.
    Client witness(port_);
    ASSERT_TRUE(witness.Admitted());
    clock_.Jump(601 * kSecond);
    EXPECT_TRUE(silent.ClosedByServer());
    EXPECT_EQ(silent.Refusal().detail_code(), kIdleSessionTimeout);
}

TEST_F(ConnectionLimitsTest, WithNoTimeoutAnIdleSessionStaysOpen) {
    // DJ-R1: the key at 0 closes nothing, however long the session sits.
    port_ = 25821;
    TcpServer::ClientSetup setup;
    Serve(port_, setup);

    Client c(port_);
    ASSERT_TRUE(c.Admitted());
    clock_.Jump(30ull * 24 * 3600 * kSecond);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(c.Ping());
    EXPECT_TRUE(c.Run("SELECT id FROM t"));
}

TEST_F(ConnectionLimitsTest, TheTextProtocolsIdleSessionIsClosedWithANamedLine) {
    port_ = 25822;
    TcpServer::ClientSetup setup;
    setup.protocol = Protocol::kText;
    setup.idle_session_timeout_ns = 600 * kSecond;
    Serve(port_, setup);

    const int fd = Connect(port_);
    ASSERT_GE(fd, 0);
    // Bounded reads: a close that never comes fails the cell below instead
    // of hanging it until ctest's timeout.
    timeval rcv{10, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
    const std::string ping = "PING\n";
    ASSERT_EQ(::write(fd, ping.data(), ping.size()), static_cast<ssize_t>(ping.size()));
    char buf[512];
    ASSERT_GT(::read(fd, buf, sizeof(buf)), 0);  // PONG
    // The first jump's tick sees the `PING` - a statement on this protocol -
    // and counts from there; the second is the timeout.
    clock_.Jump(601 * kSecond);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    clock_.Jump(601 * kSecond);

    std::string told;
    while (true) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        told.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    EXPECT_EQ(told.rfind("ERR ", 0), 0u) << told;
    EXPECT_NE(told.find("idle_session_timeout_s = 600"), std::string::npos) << told;
}

TEST_F(ConnectionLimitsTest, TheSettingsSurviveTheMoveThatOwnsTheListener) {
    // `kwp_endpoint_test.cpp`'s cell, for the two members DJ-S1 added: a
    // listener is configured and then moved into what owns it.
    auto listener = TcpServer::Listen(25823);
    ASSERT_TRUE(listener.ok()) << listener.status().message();
    TcpServer::ClientSetup setup;
    setup.limits = &limits_;
    setup.idle_session_timeout_ns = 90 * kSecond;
    listener.value().Configure(setup);
    TcpServer moved(std::move(listener.value()));
    EXPECT_EQ(moved.connection_limits(), &limits_);
    EXPECT_EQ(moved.idle_session_timeout_ns(), 90 * kSecond);

    auto other = TcpServer::Listen(25824);
    ASSERT_TRUE(other.ok());
    other.value().Configure(setup);
    TcpServer assigned = std::move(other.value());
    EXPECT_EQ(assigned.connection_limits(), &limits_);
    EXPECT_EQ(assigned.idle_session_timeout_ns(), 90 * kSecond);
}

}  // namespace
}  // namespace kds::server
