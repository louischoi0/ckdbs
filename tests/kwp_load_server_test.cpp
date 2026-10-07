#include "kds/server/kwp_load_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

void SendFrame(int fd, wire::ClientFrameType type, std::span<const std::byte> payload) {
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
        // The load endpoint's tests drive STOP over the newline surface,
        // which since KW-D6's cut-over is `debug_text_port`'s rather than
        // the default: named here so the fixture says which protocol it is
        // speaking rather than relying on a default that has moved.
        text.set_protocol(Protocol::kText);
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
        SendFrame(fd, wire::ClientFrameType::kHello, payload);
        auto reply = ReadFrame(fd, decoder);
        if (!reply.has_value() ||
            reply->type != static_cast<std::uint8_t>(wire::ServerFrameType::kHello)) {
            return false;
        }
        wire::PayloadReader r(reply->payload);
        return r.U16().value_or(0) == wire::kKwpVersion &&
               (r.U64().value_or(0) & wire::kCapBulkLoad) != 0;
    }

    // Encodes one chunk of `rows` against `schema` - every column, the pk
    // first - through the same codec the server decodes with.
    static std::vector<std::byte> Chunk(std::uint64_t load_id, std::uint32_t seq,
                                        const catalog::Schema& schema,
                                        const std::vector<std::vector<parser::AstValue>>& rows) {
        wire::RowBatchWriter writer;
        for (const auto& row : rows) EXPECT_TRUE(writer.AppendRow(schema, row).ok());
        wire::PayloadWriter w;
        w.U64(load_id);
        w.U32(seq);
        w.U16(static_cast<std::uint16_t>(rows.size()));
        auto out = w.Take();
        const auto batch = writer.Finish();
        out.insert(out.end(), batch.begin(), batch.end());
        return out;
    }

    // One chunk of int-only rows for `t (id, a, b)`, every pk NULL - the
    // engine issues each.
    static std::vector<std::byte> Chunk(
        std::uint64_t load_id, std::uint32_t seq,
        std::span<const std::pair<std::int64_t, std::int64_t>> rows) {
        catalog::Schema schema;
        for (int i = 0; i < 3; ++i) schema.columns.push_back(Col(catalog::kTypeValInt64));
        std::vector<std::vector<parser::AstValue>> values;
        for (const auto& [a, b] : rows) values.push_back({Null(), Int(a), Int(b)});
        return Chunk(load_id, seq, schema, values);
    }

    static catalog::SysColumnRow Col(std::uint32_t type_val, std::uint32_t len = 0) {
        catalog::SysColumnRow col{};
        col.type_val = type_val;
        col.len = len;
        return col;
    }
    static parser::AstValue Null() { return parser::AstValue{}; }
    static parser::AstValue Int(std::int64_t i) {
        parser::AstValue v;
        v.type = parser::ValueType::kInt;
        v.int_val = i;
        return v;
    }
    static parser::AstValue Str(std::string s) {
        parser::AstValue v;
        v.type = parser::ValueType::kStr;
        v.str_val = std::move(s);
        return v;
    }
    static parser::AstValue Dec(std::int64_t unscaled, std::uint8_t scale) {
        parser::AstValue v;
        v.type = parser::ValueType::kDecimal;
        v.int_val = unscaled;
        v.scale = scale;
        return v;
    }
    static parser::AstValue DecWide(std::int64_t hi, std::int64_t lo, std::uint8_t scale) {
        parser::AstValue v;
        v.type = parser::ValueType::kDecimalWide;
        v.dec_hi = hi;
        v.int_val = lo;
        v.scale = scale;
        return v;
    }

    // BEGIN on `relation`; the load id from S_LOAD_READY and its field
    // count, or nullopt when the server answered anything else.
    static std::optional<std::pair<std::uint64_t, std::uint16_t>> Begin(
        int fd, wire::FrameDecoder& decoder, const std::string& relation) {
        wire::LoadBegin begin;
        begin.relation = relation;
        const auto payload = wire::EncodeLoadBegin(begin);
        SendFrame(fd, wire::ClientFrameType::kLoadBegin, payload);
        auto ready = ReadFrame(fd, decoder);
        if (!ready.has_value() ||
            ready->type != static_cast<std::uint8_t>(wire::ServerFrameType::kLoadReady)) {
            return std::nullopt;
        }
        wire::PayloadReader r(ready->payload);
        const std::uint64_t load_id = r.U64().value_or(0);
        (void)r.U16();
        (void)r.U32();
        return std::pair{load_id, r.U16().value_or(0)};
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
        SendFrame(fd, wire::ClientFrameType::kLoadBegin, begin_payload);
        auto ready = ReadFrame(fd, decoder);
        ASSERT_TRUE(ready.has_value());
        ASSERT_EQ(ready->type, static_cast<std::uint8_t>(wire::ServerFrameType::kLoadReady))
            << "payload size " << ready->payload.size();
        wire::PayloadReader r(ready->payload);
        const std::uint64_t load_id = r.U64().value_or(0);
        EXPECT_EQ(r.U16().value_or(0), kKwpLoadWindow);
        EXPECT_EQ(r.U32().value_or(0), kKwpMaxChunkBytes);
        EXPECT_EQ(r.U16().value_or(0), 3u);  // every column, the pk first (BI15)

        // Two chunks: three rows, then two - the T3 gate is open for `t`
        // (heap, int-only, nothing maintained), so this exercises the
        // sorted fill through the load path.
        const std::pair<std::int64_t, std::int64_t> first[] = {{10, 1}, {20, 2}, {30, 3}};
        const std::pair<std::int64_t, std::int64_t> second[] = {{40, 4}, {50, 5}};
        const auto chunk0 = Chunk(load_id, 0, first);
        SendFrame(fd, wire::ClientFrameType::kLoadChunk, chunk0);
        auto ack0 = ReadFrame(fd, decoder);
        ASSERT_TRUE(ack0.has_value());
        ASSERT_EQ(ack0->type, static_cast<std::uint8_t>(wire::ServerFrameType::kLoadAck));
        wire::PayloadReader a0(ack0->payload);
        EXPECT_EQ(a0.U64().value_or(0), load_id);
        EXPECT_EQ(a0.U32().value_or(9), 0u);
        EXPECT_EQ(a0.U64().value_or(0), 3u);

        const auto chunk1 = Chunk(load_id, 1, second);
        SendFrame(fd, wire::ClientFrameType::kLoadChunk, chunk1);
        auto ack1 = ReadFrame(fd, decoder);
        ASSERT_TRUE(ack1.has_value());
        ASSERT_EQ(ack1->type, static_cast<std::uint8_t>(wire::ServerFrameType::kLoadAck));

        SendFrame(fd, wire::ClientFrameType::kLoadEnd, {});
        auto done = ReadFrame(fd, decoder);
        ASSERT_TRUE(done.has_value());
        ASSERT_EQ(done->type, static_cast<std::uint8_t>(wire::ServerFrameType::kComplete));
        wire::PayloadReader d(done->payload);
        // `Text`: the completion tag is the query surface's payload since
        // the two endpoints were unified on one frame registry.
        EXPECT_EQ(d.Text().value_or(std::optional<std::string>{}).value_or(""), "LOAD");
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
        SendFrame(fd, wire::ClientFrameType::kLoadBegin, begin_payload);
        auto ready = ReadFrame(fd, decoder);
        ASSERT_TRUE(ready.has_value());
        wire::PayloadReader r(ready->payload);
        const std::uint64_t load_id = r.U64().value_or(0);

        const std::pair<std::int64_t, std::int64_t> rows[] = {{7, 1}, {8, 2}};
        const auto chunk0 = Chunk(load_id, 0, rows);
        SendFrame(fd, wire::ClientFrameType::kLoadChunk, chunk0);
        ASSERT_TRUE(ReadFrame(fd, decoder).has_value());  // the ack
        SendFrame(fd, wire::ClientFrameType::kLoadAbort, {});
        auto done = ReadFrame(fd, decoder);
        ASSERT_TRUE(done.has_value());
        ASSERT_EQ(done->type, static_cast<std::uint8_t>(wire::ServerFrameType::kComplete));
        wire::PayloadReader d(done->payload);
        EXPECT_EQ(d.Text().value_or(std::optional<std::string>{}).value_or(""), "ABORT");

        // A second load whose chunk skips a sequence number: the load dies
        // with S_ERROR and its rows unwind (KW4's modality).
        SendFrame(fd, wire::ClientFrameType::kLoadBegin, begin_payload);
        auto ready2 = ReadFrame(fd, decoder);
        ASSERT_TRUE(ready2.has_value());
        wire::PayloadReader r2(ready2->payload);
        const std::uint64_t load2 = r2.U64().value_or(0);
        const auto chunk_skip = Chunk(load2, 5, rows);
        SendFrame(fd, wire::ClientFrameType::kLoadChunk, chunk_skip);
        auto err = ReadFrame(fd, decoder);
        ASSERT_TRUE(err.has_value());
        EXPECT_EQ(err->type, static_cast<std::uint8_t>(wire::ServerFrameType::kError));
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
        SendFrame(fd, wire::ClientFrameType::kLoadBegin, payload);
        auto err = ReadFrame(fd, decoder);
        ASSERT_TRUE(err.has_value());
        EXPECT_EQ(err->type, static_cast<std::uint8_t>(wire::ServerFrameType::kError));
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

TEST_F(KwpLoadServerTest, ALoadConnectionCarriesTheConfiguredKeepalive) {
    // A vanished loader holds its load's open transaction, which is the
    // connection keepalive exists to end - so `kwp_port` takes
    // `tcp_keepalive_s` like every listener (`ConfigureAcceptedSocket`).
    // The server runs in this process, so its accepted socket is one of our
    // fds: found by the address pair, then asked for its options.
    constexpr std::uint16_t kTextPort = 25717;
    constexpr std::uint16_t kKwpPort = 25718;
    constexpr std::uint32_t kIdle = 45;

    auto text = TcpServer::Listen(kTextPort);
    auto kwp = KwpLoadServer::Listen(kKwpPort, kIdle);
    ASSERT_TRUE(text.ok());
    ASSERT_TRUE(kwp.ok());

    std::thread client([&] {
        StopGuard stop{kTextPort};
        int fd = ConnectToLoopback(kKwpPort);
        ASSERT_GE(fd, 0);
        sockaddr_in mine{};
        socklen_t len = sizeof(mine);
        ASSERT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&mine), &len), 0);

        int idle = -1;
        for (int attempt = 0; attempt < 200 && idle < 0; ++attempt) {
            for (int other = 3; other < 4096; ++other) {
                sockaddr_in local{};
                sockaddr_in peer{};
                socklen_t l1 = sizeof(local);
                socklen_t l2 = sizeof(peer);
                if (other == fd ||
                    ::getsockname(other, reinterpret_cast<sockaddr*>(&local), &l1) != 0 ||
                    ::getpeername(other, reinterpret_cast<sockaddr*>(&peer), &l2) != 0 ||
                    ntohs(local.sin_port) != kKwpPort || peer.sin_port != mine.sin_port) {
                    continue;
                }
                socklen_t vlen = sizeof(idle);
                ::getsockopt(other, IPPROTO_TCP, TCP_KEEPIDLE, &idle, &vlen);
                break;
            }
            if (idle < 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_EQ(idle, static_cast<int>(kIdle)) << "the load endpoint's accepted socket";
        ::close(fd);

        int text_fd = ConnectToLoopback(kTextPort);
        ASSERT_GE(text_fd, 0);
        ::close(text_fd);
    });

    RunReactor(text.value(), kwp.value());
    client.join();
}

// BI15 and BI16: every storable type loads, NULL included, and the pk field
// decides each row - NULL takes an issued id, a value names one - so one
// chunk mixes the two exactly as one T1 statement does.
TEST_F(KwpLoadServerTest, EveryTypeLoadsAndOneChunkMixesNamedAndIssuedKeys) {
    constexpr std::uint16_t kTextPort = 25719;
    constexpr std::uint16_t kKwpPort = 25720;
    const std::string created =
        dispatcher_
            ->Dispatch("CREATE TABLE t (id int64, a int8, b int16, c int32, d uint64, "
                       "e bool, f date, g timestamp, h decimal(10,2), i decimal(30,4), "
                       "j char(4), k varchar, n int64 NULL)")
            .response;
    ASSERT_EQ(created.rfind("CREATED", 0), 0u) << created;
    auto access = boot_->catalog.InitTableAccess(boot_->catalog.FindTableOidByName("t").value());
    ASSERT_TRUE(access.ok());
    const catalog::Schema schema = access.value()->schema;

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
        const auto ready = Begin(fd, decoder, "t");
        ASSERT_TRUE(ready.has_value());
        EXPECT_EQ(ready->second, 13u);

        // Row 1 takes an issued id, row 2 names 500, row 3 takes the next
        // issued one - above the named key, which moved the mark.
        const auto row = [&](parser::AstValue pk, std::int64_t tag) {
            return std::vector<parser::AstValue>{
                std::move(pk),       Int(-7),     Int(-300),       Int(70000),
                Int(9),              Int(1),      Int(20454),      Int(1767225600000000),
                Dec(-12345, 2),      DecWide(-1, -123456789, 4),
                Str("ab"),           Str(std::string(100, 'x')),
                tag == 0 ? Null() : Int(tag)};
        };
        const auto chunk = Chunk(ready->first, 0, schema,
                                 {row(Null(), 0), row(Int(500), 2), row(Null(), 3)});
        SendFrame(fd, wire::ClientFrameType::kLoadChunk, chunk);
        auto ack = ReadFrame(fd, decoder);
        ASSERT_TRUE(ack.has_value());
        ASSERT_EQ(ack->type, static_cast<std::uint8_t>(wire::ServerFrameType::kLoadAck))
            << (ack->type == static_cast<std::uint8_t>(wire::ServerFrameType::kError)
                    ? wire::DecodeError(ack->payload).value().message
                    : std::string());
        SendFrame(fd, wire::ClientFrameType::kLoadEnd, {});
        auto done = ReadFrame(fd, decoder);
        ASSERT_TRUE(done.has_value());
        ASSERT_EQ(done->type, static_cast<std::uint8_t>(wire::ServerFrameType::kComplete));
        ::close(fd);

        int text_fd = ConnectToLoopback(kTextPort);
        ASSERT_GE(text_fd, 0);
        EXPECT_EQ(SendAndReceiveLine(text_fd, "SELECT id, n FROM t"),
                  "id,n\\n1,NULL\\n500,2\\n501,3");
        EXPECT_EQ(SendAndReceiveLine(text_fd,
                                     "SELECT a, b, c, d, e, f, g, h, i, j FROM t WHERE id = 500"),
                  "a,b,c,d,e,f,g,h,i,j\\n-7,-300,70000,9,1,2026-01-01,2026-01-01 "
                  "00:00:00,-123.45,-12345.6789,ab");
        EXPECT_EQ(SendAndReceiveLine(text_fd, "SELECT COUNT(*) FROM t WHERE k = '" +
                                                  std::string(100, 'x') + "'"),
                  "count(*)\\n3");
        ::close(text_fd);
    });

    RunReactor(text.value(), kwp.value());
    client.join();
}

// A row the storage gate refuses kills the load whole (BI4), and the
// refusal keeps its code: a named key already present is `AlreadyExists`, a
// decimal past its precision and a NULL in a `NOT NULL` column are the
// gate's own answers - the same ones a T1 statement gets for the same row.
TEST_F(KwpLoadServerTest, ARefusedRowKillsTheLoadWithTheGatesOwnCode) {
    constexpr std::uint16_t kTextPort = 25721;
    constexpr std::uint16_t kKwpPort = 25722;
    ASSERT_EQ(dispatcher_->Dispatch("CREATE TABLE t (id int64, h decimal(5,2), e bool)")
                  .response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO t VALUES (100, '1.00', 0)").response.substr(0, 8),
              "INSERTED");
    catalog::Schema schema;
    schema.columns = {Col(catalog::kTypeValInt64),
                      Col(catalog::kTypeValDecimal, catalog::PackDecimalLen(5, 2)),
                      Col(catalog::kTypeValBool)};

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

        // Each load: one good row first, so the unwind is observable, then
        // the refused one as row 2.
        // `bool_byte`, when set, overwrites row 2's last field - its bool -
        // after the encode: the client codec refuses a bool that is not 0
        // or 1, so a wire value it cannot produce is spelled by hand.
        const auto refused = [&](std::vector<parser::AstValue> bad, StatusCode code,
                                 const std::string& what,
                                 std::optional<std::uint8_t> bool_byte = std::nullopt) {
            const auto ready = Begin(fd, decoder, "t");
            ASSERT_TRUE(ready.has_value()) << what;
            auto chunk = Chunk(ready->first, 0, schema,
                               {{Null(), Dec(250, 2), Int(1)}, std::move(bad)});
            if (bool_byte.has_value()) chunk.back() = static_cast<std::byte>(*bool_byte);
            SendFrame(fd, wire::ClientFrameType::kLoadChunk, chunk);
            auto err = ReadFrame(fd, decoder);
            ASSERT_TRUE(err.has_value()) << what;
            ASSERT_EQ(err->type, static_cast<std::uint8_t>(wire::ServerFrameType::kError)) << what;
            auto decoded = wire::DecodeError(err->payload);
            ASSERT_TRUE(decoded.ok()) << what;
            EXPECT_EQ(decoded.value().code,
                      wire::ErrorFromStatus(
                          Status::FromWire(static_cast<std::uint32_t>(code), "x")).code)
                << what << ": " << decoded.value().message;
            EXPECT_NE(decoded.value().message.find("chunk 0"), std::string::npos)
                << what << ": " << decoded.value().message;
            EXPECT_NE(decoded.value().message.find("row 2"), std::string::npos)
                << what << ": " << decoded.value().message;
        };
        refused({Int(100), Dec(1, 2), Int(0)}, StatusCode::kAlreadyExists, "duplicate key");
        refused({Null(), Dec(1000000, 2), Int(0)}, StatusCode::kOutOfRange, "precision");
        refused({Null(), Null(), Int(0)}, StatusCode::kInvalidArgument, "NOT NULL");
        refused({Null(), Dec(1, 2), Int(1)}, StatusCode::kInvalidArgument, "bool", 2);
        ::close(fd);

        int text_fd = ConnectToLoopback(kTextPort);
        ASSERT_GE(text_fd, 0);
        EXPECT_EQ(SendAndReceiveLine(text_fd, "SELECT id FROM t"), "id\\n100");
        ::close(text_fd);
    });

    RunReactor(text.value(), kwp.value());
    client.join();
}

}  // namespace
}  // namespace kds::server
