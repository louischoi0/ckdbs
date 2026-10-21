// **A plain `SELECT`'s rows leave while it runs** (CE,
// `instructions/v3.2.0/workorder-ce-streaming-reply.md`).
//
// Each cell runs the whole engine on the two-core rig behind a real KWP
// socket on core 0, over a relation of ~2 KB rows, so a walk crosses several
// slice boundaries (`exec::SlicePolicy`, 64 pages). The walk's test seam
// (`SetSelectRowEmittedForTest`) is where a cell stops the walk, fails it, or
// counts how far it got; the client reads frames on the test thread.
//
// What the cells pin: the first batch reaches the client before the walk
// ends (CE-R2); rows followed by `S_ERROR` (CE-Q3 (a)); no BA-S14 re-run
// once a row has left (CE-R4); a re-run answers each row once (CE-A1, which
// is a defect of the tree before CE, buffered or not); a client that stops
// reading parks the walk, and one that hangs up ends it (CE-Q5 (a), CE-A5,
// CE-A6), and one with nothing to ship does not park on the frames BN-R7
// holds back; a capped portal does not stream (CE-Q6 (a)).

#include "kwp_test_client.hpp"
#include "two_core_rig.hpp"

#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/contention.hpp"
#include "kds/server/kwp_session.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/insert_placement.hpp"
#include "kds/wal/durability.hpp"
#include "kds/wire/error_registry.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;
using namespace kwp_test;
using wire::ClientFrameType;
using wire::ServerFrameType;

// ~2 KB a row, about four to a leaf: `kRows` rows are ~400 leaves, six
// slice boundaries.
constexpr int kRows = 1600;

template <typename Pred>
bool Within(std::chrono::milliseconds limit, Pred pred) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

std::uint8_t T(ServerFrameType type) { return static_cast<std::uint8_t>(type); }

// The row count an `S_ROW_BATCH` payload opens with.
std::uint32_t BatchRows(const wire::DecodedFrame& frame) {
    return std::to_integer<std::uint32_t>(frame.payload[0]) |
           (std::to_integer<std::uint32_t>(frame.payload[1]) << 8);
}

class StreamingReplyTest : public ::testing::Test {
protected:
    void Open(std::uint16_t port, int rows = kRows) {
        TwoCoreRig::Options options;
        options.max_idle_block_ms = 20;
        auto opened = TwoCoreRig::Open(options);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        rig_ = std::move(opened.value());
        ASSERT_EQ(d().Dispatch("CREATE TABLE t (id int64, name varchar(2000))").response.rfind(
                      "CREATED", 0),
                  0u);
        const std::string wide(1900, 'x');
        for (int i = 0; i < rows;) {
            std::string sql = "INSERT INTO t VALUES ";
            for (int k = 0; k < 100 && i < rows; ++k, ++i) {
                if (k != 0) sql += ", ";
                sql += "(" + std::to_string(i + 1) + ", '" + wide + "')";
            }
            const std::string reply = d().Dispatch(sql).response;
            ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
        }
        ASSERT_TRUE(rig_->core(0).ListenAndAttach(port).ok());
        rig_->Start();
    }
    void TearDown() override {
        for (int fd : fds_) ::close(fd);
        if (rig_ != nullptr) rig_->Stop();
    }

    CommandDispatcher& d() { return rig_->core(0).dispatcher(); }

    int Client(std::uint16_t port, wire::FrameDecoder& decoder, int rcvbuf = 0) {
        const int fd = Connect(port, rcvbuf);
        EXPECT_GE(fd, 0);
        // A server that stops answering fails the cell rather than hanging the
        // run: `ReadFrame` gives up when a read times out.
        const timeval bound{10, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &bound, sizeof(bound));
        EXPECT_TRUE(Handshake(fd, decoder));
        fds_.push_back(fd);
        return fd;
    }

    // PARSE/BIND/EXECUTE/SYNC in one write, as a pipelining client sends
    // them.
    static void Run(int fd, std::string_view sql, std::uint32_t max_rows = 0) {
        std::vector<std::byte> out;
        for (const auto& [type, payload] :
             {std::pair{ClientFrameType::kParse, ParsePayload("q", sql)},
              std::pair{ClientFrameType::kBind, BindPayload("q", "q")},
              std::pair{ClientFrameType::kExecute, ExecutePayload("q", max_rows)},
              std::pair{ClientFrameType::kSync, std::vector<std::byte>{}}}) {
            const auto frame = wire::EncodeFrame(static_cast<std::uint8_t>(type), 0, payload);
            out.insert(out.end(), frame.begin(), frame.end());
        }
        ASSERT_EQ(::write(fd, out.data(), out.size()), static_cast<ssize_t>(out.size()));
    }

    // Reads to `S_READY`, counting rows and noting the frame types in order.
    struct Reply {
        std::vector<std::uint8_t> types;
        std::uint64_t rows = 0;
        std::optional<wire::DecodedFrame> error;
    };
    static Reply ReadToReady(int fd, wire::FrameDecoder& decoder) {
        Reply reply;
        while (auto frame = ReadFrame(fd, decoder)) {
            reply.types.push_back(frame->type);
            if (frame->type == T(ServerFrameType::kRowBatch)) reply.rows += BatchRows(*frame);
            if (frame->type == T(ServerFrameType::kError)) reply.error = frame;
            if (frame->type == T(ServerFrameType::kReady)) break;
        }
        return reply;
    }

    std::unique_ptr<TwoCoreRig> rig_;
    std::vector<int> fds_;
};

TEST_F(StreamingReplyTest, TheFirstBatchArrivesBeforeTheWalkEnds) {
    constexpr std::uint16_t kPort = 25611;
    Open(kPort);
    // The walk stops one row short of its end - past its last slice
    // boundary - until the client has read a batch. A reply that leaves only
    // at completion cannot arrive while the walk is stopped.
    std::atomic<bool> batch_read{false};
    std::atomic<bool> waited_out{false};
    d().SetSelectRowEmittedForTest([&](std::uint64_t emitted) {
        if (emitted == kRows - 1 && !Within(5000ms, [&] { return batch_read.load(); })) {
            waited_out.store(true);
        }
        return Status::OK();
    });
    wire::FrameDecoder decoder;
    const int fd = Client(kPort, decoder);
    Run(fd, "SELECT id, name FROM t");

    // BN-R7's frames lead, then the rows.
    for (ServerFrameType want : {ServerFrameType::kParseOk, ServerFrameType::kBindOk,
                                 ServerFrameType::kRowDesc, ServerFrameType::kRowBatch}) {
        auto frame = ReadFrame(fd, decoder);
        ASSERT_TRUE(frame.has_value());
        ASSERT_EQ(frame->type, T(want));
        if (want == ServerFrameType::kRowBatch) {
            batch_read.store(true);
            Reply rest = ReadToReady(fd, decoder);
            EXPECT_EQ(BatchRows(*frame) + rest.rows, static_cast<std::uint64_t>(kRows));
            EXPECT_FALSE(rest.error.has_value());
            EXPECT_EQ(std::count(rest.types.begin(), rest.types.end(),
                                 T(ServerFrameType::kRowDesc)),
                      0)
                << "S_ROW_DESC is sent once";
            EXPECT_EQ(std::count(rest.types.begin(), rest.types.end(),
                                 T(ServerFrameType::kComplete)),
                      1);
        }
    }
    EXPECT_FALSE(waited_out.load()) << "no batch left before the walk's end";
}

TEST_F(StreamingReplyTest, RowsFollowedByAnErrorAreNoResult) {
    constexpr std::uint16_t kPort = 25612;
    Open(kPort);
    // Past the first slice boundaries, so rows have left when it fails.
    d().SetSelectRowEmittedForTest([&](std::uint64_t emitted) {
        return emitted == kRows - 1 ? Status::ResourceExhausted("the walk's budget, in a test")
                                    : Status::OK();
    });
    wire::FrameDecoder decoder;
    const int fd = Client(kPort, decoder);
    Run(fd, "SELECT id, name FROM t");
    const Reply reply = ReadToReady(fd, decoder);
    EXPECT_GT(reply.rows, 0u) << "nothing streamed before the failure";
    EXPECT_LT(reply.rows, static_cast<std::uint64_t>(kRows));
    ASSERT_TRUE(reply.error.has_value());
    EXPECT_EQ(std::count(reply.types.begin(), reply.types.end(), T(ServerFrameType::kComplete)),
              0)
        << "a failed statement completes nothing";
    // The portal went with the statement (XG-R8): there is nothing to resume.
    SendFrame(fd, ClientFrameType::kContinue, ExecutePayload("q", 0));
    SendFrame(fd, ClientFrameType::kSync, {});
    const Reply after = ReadToReady(fd, decoder);
    EXPECT_TRUE(after.error.has_value());
    EXPECT_EQ(after.rows, 0u);
}

TEST_F(StreamingReplyTest, AStreamedStatementIsNotReRun) {
    constexpr std::uint16_t kPort = 25613;
    Open(kPort);
    // A stale descent's refusal, after rows have left: BA-S14 would run the
    // statement again and the client would read its rows twice (CE-R4).
    std::atomic<int> refusals{0};
    std::atomic<std::uint64_t> most{0};
    d().SetSelectRowEmittedForTest([&](std::uint64_t emitted) {
        if (emitted > most.load()) most.store(emitted);
        if (emitted == kRows - 1 && refusals.fetch_add(1) == 0) {
            storage::NoteStructuralRefusal();
            return Status::TxnConflict("a stale descent, in a test");
        }
        return Status::OK();
    });
    const std::uint64_t reruns_before = Contention::Read().of(Tally::kStructuralReruns);
    wire::FrameDecoder decoder;
    const int fd = Client(kPort, decoder);
    Run(fd, "SELECT id, name FROM t");
    const Reply reply = ReadToReady(fd, decoder);
    ASSERT_TRUE(reply.error.has_value()) << "the refusal is answered, not re-run";
    EXPECT_EQ(refusals.load(), 1) << "the statement ran once";
    EXPECT_LT(reply.rows, static_cast<std::uint64_t>(kRows));
    EXPECT_EQ(Contention::Read().of(Tally::kStructuralReruns), reruns_before);
}

TEST_F(StreamingReplyTest, AClientThatStopsReadingParksTheWalkAndAHangupEndsIt) {
    constexpr std::uint16_t kPort = 25614;
    // ~12 MB of rows, past anything the two sockets' buffers can hold.
    constexpr int kMany = 6400;
    Open(kPort, kMany);
    std::atomic<std::uint64_t> emitted{0};
    d().SetSelectRowEmittedForTest([&](std::uint64_t n) {
        emitted.store(n);
        return Status::OK();
    });
    wire::FrameDecoder decoder;
    const int fd = Client(kPort, decoder, /*rcvbuf=*/16 * 1024);
    Run(fd, "SELECT id, name FROM t");

    // Not reading: the walk goes as far as the buffers take and stops.
    ASSERT_TRUE(Within(5000ms, [&] { return emitted.load() > 0; }));
    std::uint64_t seen = 0;
    ASSERT_TRUE(Within(5000ms, [&] {
        const std::uint64_t now = emitted.load();
        if (now == seen) return true;
        seen = now;
        std::this_thread::sleep_for(300ms);
        return false;
    }));
    EXPECT_LT(emitted.load(), static_cast<std::uint64_t>(kMany)) << "the walk did not park";

    // A hangup resumes the parked walk to its cancel, and it lets go of the
    // relation: a DROP no longer waits on its borrow (CE-A6).
    ::shutdown(fd, SHUT_RDWR);
    std::this_thread::sleep_for(200ms);
    wire::FrameDecoder other_decoder;
    const int other = Client(kPort, other_decoder);
    Run(other, "DROP TABLE t");
    const Reply dropped = ReadToReady(other, other_decoder);
    EXPECT_FALSE(dropped.error.has_value()) << "the parked walk still holds the relation";
    EXPECT_LT(emitted.load(), static_cast<std::uint64_t>(kMany));
}

TEST_F(StreamingReplyTest, AClientThatReadsAgainGetsEveryRow) {
    constexpr std::uint16_t kPort = 25615;
    constexpr int kMany = 6400;
    Open(kPort, kMany);
    std::atomic<std::uint64_t> emitted{0};
    d().SetSelectRowEmittedForTest([&](std::uint64_t n) {
        emitted.store(n);
        return Status::OK();
    });
    wire::FrameDecoder decoder;
    const int fd = Client(kPort, decoder, /*rcvbuf=*/16 * 1024);
    Run(fd, "SELECT id, name FROM t");
    ASSERT_TRUE(Within(5000ms, [&] { return emitted.load() > 0; }));
    std::this_thread::sleep_for(500ms);
    EXPECT_LT(emitted.load(), static_cast<std::uint64_t>(kMany)) << "the walk did not park";
    const Reply reply = ReadToReady(fd, decoder);
    EXPECT_FALSE(reply.error.has_value());
    EXPECT_EQ(reply.rows, static_cast<std::uint64_t>(kMany));
}

TEST_F(StreamingReplyTest, AWalkThatSealsNothingDoesNotParkOnHeldFrames) {
    constexpr std::uint16_t kPort = 25617;
    Open(kPort);
    // No row matches, so no slice boundary has a batch to ship - and the
    // pipelined `S_PARSE_OK`/`S_BIND_OK` BN-R7 holds back sit in the outbox
    // with no write interest armed. A walk parked on them never resumes.
    wire::FrameDecoder decoder;
    const int fd = Client(kPort, decoder);
    Run(fd, "SELECT id FROM t WHERE name = 'none'");
    const Reply reply = ReadToReady(fd, decoder);
    ASSERT_FALSE(reply.types.empty());
    EXPECT_EQ(reply.types.back(), T(ServerFrameType::kReady)) << "the walk parked";
    EXPECT_FALSE(reply.error.has_value());
    EXPECT_EQ(reply.rows, 0u);
    EXPECT_EQ(std::count(reply.types.begin(), reply.types.end(), T(ServerFrameType::kComplete)),
              1);
}

TEST_F(StreamingReplyTest, ACappedPortalDoesNotStream) {
    constexpr std::uint16_t kPort = 25616;
    Open(kPort);
    // CE-Q6 (a): `max_rows` keeps "bounds delivery, not execution" - the
    // walk has ended before the first row leaves.
    std::atomic<std::uint64_t> emitted{0};
    d().SetSelectRowEmittedForTest([&](std::uint64_t n) {
        emitted.store(n);
        return Status::OK();
    });
    wire::FrameDecoder decoder;
    const int fd = Client(kPort, decoder);
    Run(fd, "SELECT id, name FROM t", /*max_rows=*/100);
    for (ServerFrameType want : {ServerFrameType::kParseOk, ServerFrameType::kBindOk,
                                 ServerFrameType::kRowDesc}) {
        auto frame = ReadFrame(fd, decoder);
        ASSERT_TRUE(frame.has_value());
        ASSERT_EQ(frame->type, T(want));
    }
    EXPECT_EQ(emitted.load(), static_cast<std::uint64_t>(kRows));
    auto batch = ReadFrame(fd, decoder);
    ASSERT_TRUE(batch.has_value());
    EXPECT_EQ(batch->type, T(ServerFrameType::kRowBatch));
    EXPECT_GT(BatchRows(*batch), 0u);
    EXPECT_LE(BatchRows(*batch), 100u) << "a capped batch seals at the cap or at the size target";
    auto suspended = ReadFrame(fd, decoder);
    ASSERT_TRUE(suspended.has_value());
    EXPECT_EQ(suspended->type, T(ServerFrameType::kPortalSuspended));
}

// **CE-A1, without a socket**: the defect is the portal sink's, buffered or
// streamed. A walk refused after it emitted is re-run by BA-S14, and the
// sink that outlives the run answered both attempts' rows.
TEST(StreamingReplyReRunTest, AReRunAnswersEachRowOnce) {
    TwoCoreRig::Options options;
    options.max_idle_block_ms = 20;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d = rig->core(0).dispatcher();
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int64)").response.rfind("CREATED", 0), 0u);
    std::string sql = "INSERT INTO t VALUES ";
    for (int i = 1; i <= 50; ++i) sql += (i == 1 ? "(" : ", (") + std::to_string(i) + ", 0)";
    ASSERT_EQ(d.Dispatch(sql).response.rfind("ERR", 0), std::string::npos);

    std::atomic<int> refusals{0};
    d.SetSelectRowEmittedForTest([&](std::uint64_t emitted) {
        if (emitted == 10 && refusals.fetch_add(1) == 0) {
            storage::NoteStructuralRefusal();
            return Status::TxnConflict("a stale descent, in a test");
        }
        return Status::OK();
    });
    Session session;
    WireResultSink sink;
    session.set_result_sink(&sink);
    const DispatchOutcome out = d.Dispatch("SELECT id, v FROM t", &session);
    ASSERT_TRUE(out.status.ok()) << out.response;
    EXPECT_EQ(refusals.load(), 2) << "the statement was not re-run";
    sink.Finish();
    EXPECT_EQ(sink.row_count(), 50u) << "the re-run answered the first attempt's rows again";
    std::uint64_t in_batches = 0;
    for (const auto& batch : sink.batches()) in_batches += batch.rows;
    EXPECT_EQ(in_batches, 50u);
    rig->Stop();
}

}  // namespace
}  // namespace kds::server
