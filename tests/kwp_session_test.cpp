#include <cstdint>
#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/sched/core_load.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/kwp_session.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wire/error_registry.hpp"
#include "kds/wire/handshake.hpp"
#include "kds/wire/kwp.hpp"
#include "kds/wire/kwp_types.hpp"
#include "kds/wire/row_codec.hpp"

// The KWP/1 session state machine (protocol-wp.md P08, P10, P11) against
// **the real dispatcher** - KW-D1 struck the stub layer, so there is none.
//
// **The determinism the stub used to provide lives in this fixture**, which
// is what KW-D1 requires in as many words: one relation, built the same way
// every time, and fixed statements over it. A failure here is a protocol
// failure because nothing below it is free to vary, not because nothing
// below it can fail.
//
// One fixture for the three rows rather than three files. They are one
// state machine - a portal's suspension and a transaction's ack are frames
// of the same session - and splitting them would mean three copies of the
// driver below, which is the duplication the rows themselves warn about
// everywhere else.

namespace kds::server {
namespace {

using wire::ClientFrameType;
using wire::ServerFrameType;

class KwpSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/4000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));

        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*mgr_);

        wire::HandshakeConfig config;
        config.capabilities = wire::kServerCapabilities;
        config.server_info = "kds-test";
        engine_session_.emplace(txn::IsolationLevel::kReadCommitted);
        session_.emplace(*engine_session_, config, wal::DurabilityClass::kRelaxed, &clock_);
        session_->set_identity(0x1111, 0x2222);

        // The fixed relation, built through the dispatcher exactly as a
        // client would - so the rows the protocol reads are rows the engine
        // wrote, not rows a fixture forged.
        Run("CREATE TABLE t (id int64, v int32, name varchar)");
        Run("INSERT INTO t VALUES (10, 'alpha')");
        Run("INSERT INTO t VALUES (20, 'beta')");
        Run("INSERT INTO t VALUES (30, 'gamma')");
    }

    // A statement through the dispatcher's own session, for fixture setup.
    std::string Run(const std::string& sql) {
        return dispatcher_->Dispatch(sql, &session_->session()).response;
    }

    // Feeds one frame and drives whatever dispatch it asks for, exactly as
    // the endpoint does - synchronously, because these statements never
    // park (no peer, no group commit).
    std::vector<wire::DecodedFrame> Feed(ClientFrameType type,
                                         const std::vector<std::byte>& payload) {
        std::vector<std::byte> out;
        FrameAction action =
            session_->OnFrame(wire::DecodedFrame{static_cast<std::uint8_t>(type), 0, payload},
                              out);
        if (action.dispatch) {
            session_->session().set_result_sink(action.sink);
            DispatchOutcome outcome =
                dispatcher_->Dispatch(action.sql, &session_->session());
            session_->session().set_result_sink(nullptr);
            session_->OnStatementComplete(outcome, out);
        }
        closed_ = action.close;
        return Decode(out);
    }

    std::vector<wire::DecodedFrame> Decode(const std::vector<std::byte>& bytes) {
        wire::FrameDecoder decoder;
        EXPECT_TRUE(decoder.Feed(bytes).ok());
        std::vector<wire::DecodedFrame> frames;
        while (auto frame = decoder.PopFrame()) frames.push_back(std::move(*frame));
        return frames;
    }

    // ---- Frame builders ---------------------------------------------------
    std::vector<std::byte> Hello() {
        wire::ClientHello h;
        h.client_name = "test";
        return wire::EncodeClientHello(h);
    }
    static std::vector<std::byte> Parse(std::string_view name, std::string_view sql) {
        wire::PayloadWriter w;
        w.Str(name);
        w.Text(sql);
        return w.Take();
    }
    static std::vector<std::byte> Bind(std::string_view portal, std::string_view stmt,
                                       const std::vector<wire::BoundParam>& params = {}) {
        wire::PayloadWriter w;
        w.Str(portal);
        w.Str(stmt);
        std::vector<std::byte> out = w.Take();
        EXPECT_TRUE(wire::EncodeBindParams(params, out).ok());
        return out;
    }
    static std::vector<std::byte> Execute(std::string_view portal, std::uint32_t max_rows) {
        wire::PayloadWriter w;
        w.Str(portal);
        w.U32(max_rows);
        return w.Take();
    }
    static std::vector<std::byte> Handle(std::uint8_t kind, std::string_view name) {
        wire::PayloadWriter w;
        w.U8(kind);
        w.Str(name);
        return w.Take();
    }
    static std::vector<std::byte> U8Payload(std::uint8_t v) {
        wire::PayloadWriter w;
        w.U8(v);
        return w.Take();
    }

    // Runs the handshake and drops its two frames.
    void Handshake() {
        auto frames = Feed(ClientFrameType::kHello, Hello());
        ASSERT_EQ(frames.size(), 2u);
        ASSERT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kHello));
        ASSERT_EQ(frames[1].type, static_cast<std::uint8_t>(ServerFrameType::kReady));
    }

    // The whole PARSE/BIND/EXECUTE for one statement, answering the frames
    // EXECUTE produced.
    std::vector<wire::DecodedFrame> RunStatement(const std::string& sql,
                                                 std::uint32_t max_rows = 0) {
        EXPECT_EQ(Feed(ClientFrameType::kParse, Parse("s", sql)).size(), 1u);
        EXPECT_EQ(Feed(ClientFrameType::kBind, Bind("p", "s")).size(), 1u);
        return Feed(ClientFrameType::kExecute, Execute("p", max_rows));
    }

    static std::vector<wire::DecodedFrame> Only(std::vector<wire::DecodedFrame> frames,
                                                ServerFrameType type) {
        std::vector<wire::DecodedFrame> out;
        for (auto& f : frames) {
            if (f.type == static_cast<std::uint8_t>(type)) out.push_back(std::move(f));
        }
        return out;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
    sched::ManualClock clock_{1000};
    // Declared before the protocol session, which borrows it.
    std::optional<Session> engine_session_;
    std::optional<KwpSession> session_;
    bool closed_ = false;
};

// ---- P07/P08: the handshake and the statement lifecycle -------------------

TEST_F(KwpSessionTest, TheHandshakeAnswersHelloThenReady) {
    auto frames = Feed(ClientFrameType::kHello, Hello());
    ASSERT_EQ(frames.size(), 2u);
    auto hello = wire::DecodeServerHello(frames[0].payload);
    ASSERT_TRUE(hello.ok()) << hello.status().message();
    EXPECT_EQ(hello.value().version, wire::kKwpVersion);
    EXPECT_EQ(hello.value().session_id, 0x1111u);
    EXPECT_EQ(hello.value().cancel_key, 0x2222u);
    EXPECT_EQ(frames[1].payload.size(), 1u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(frames[1].payload[0]),
              static_cast<std::uint8_t>(Session::State::kIdle));
    EXPECT_TRUE(session_->handshake_done());
}

TEST_F(KwpSessionTest, AnyFrameBeforeTheHandshakeIsFatal) {
    auto frames = Feed(ClientFrameType::kPing, {});
    ASSERT_EQ(frames.size(), 1u);
    ASSERT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kError));
    auto err = wire::DecodeError(frames[0].payload);
    ASSERT_TRUE(err.ok());
    EXPECT_EQ(err.value().severity, wire::Severity::kFatal);
    EXPECT_TRUE(closed_);
}

TEST_F(KwpSessionTest, ParseReturnsThePatternIdAndTheSubstitutedFormSharesIt) {
    Handshake();
    auto frames = Feed(ClientFrameType::kParse,
                       Parse("s", "SELECT id, v FROM t WHERE id = ?"));
    ASSERT_EQ(frames.size(), 1u);
    ASSERT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kParseOk));
    wire::PayloadReader r(frames[0].payload);
    auto pattern_id = r.U64();
    ASSERT_TRUE(pattern_id.has_value());
    EXPECT_NE(*pattern_id, 0u) << "a SELECT is patternable";

    // **The property substitution rests on** (waystone-workplan.md P01): a
    // literal and a `?` emit the same shape marker, so binding a parameter
    // cannot move the pattern. If this ever fails, every prepared statement
    // in this engine starts learning a second waystone for the shape it
    // already knows.
    auto bound = parser::FingerprintOf("SELECT id, v FROM t WHERE id = 20");
    ASSERT_TRUE(bound.has_value());
    EXPECT_EQ(bound->pattern_id, *pattern_id);
}

TEST_F(KwpSessionTest, APreparedDeleteCarriesItsPatternIdAsAnUpdateDoes) {
    // DF-Q11 (a): `delete` is a patternable leading word, so `S_PARSE_OK`
    // answers a prepared DELETE its shape rather than 0.
    Handshake();
    auto frames = Feed(ClientFrameType::kParse, Parse("d", "DELETE FROM t WHERE id = ?"));
    ASSERT_EQ(frames.size(), 1u);
    ASSERT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kParseOk));
    wire::PayloadReader r(frames[0].payload);
    auto pattern_id = r.U64();
    ASSERT_TRUE(pattern_id.has_value());
    auto bound = parser::FingerprintOf("DELETE FROM t WHERE id = 20");
    ASSERT_TRUE(bound.has_value());
    EXPECT_EQ(*pattern_id, bound->pattern_id);
    EXPECT_NE(*pattern_id, 0u);
}

TEST_F(KwpSessionTest, ASelectAnswersADescriptionRowsAndACompletion) {
    Handshake();
    auto frames = RunStatement("SELECT id, v FROM t");
    ASSERT_EQ(frames.size(), 3u);
    ASSERT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kRowDesc));
    ASSERT_EQ(frames[1].type, static_cast<std::uint8_t>(ServerFrameType::kRowBatch));
    ASSERT_EQ(frames[2].type, static_cast<std::uint8_t>(ServerFrameType::kComplete));

    auto desc = wire::DecodeRowDescription(frames[0].payload);
    ASSERT_TRUE(desc.ok()) << desc.status().message();
    ASSERT_EQ(desc.value().size(), 2u);
    EXPECT_EQ(desc.value()[0].name, "id");
    EXPECT_EQ(desc.value()[0].type_oid, catalog::kTypeValInt64)
        << "the type the relation declared, which is what a client decodes by";
    EXPECT_NE(desc.value()[0].flags & wire::kFieldFlagKeystone, 0)
        << "this projection's first field *is* the relation's column 0";
    EXPECT_EQ(desc.value()[1].name, "v");
    EXPECT_EQ(desc.value()[1].type_oid, catalog::kTypeValInt32);

    auto rows = wire::DecodeRowBatch(frames[1].payload, 2);
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    ASSERT_EQ(rows.value().size(), 3u);
    // Values, not text: the second field of the first row is the int32 10
    // in four little-endian bytes.
    ASSERT_EQ(rows.value()[0][1].bytes.size(), 4u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(rows.value()[0][1].bytes[0]), 10);

    wire::PayloadReader r(frames[2].payload);
    auto tag = r.Text();
    auto affected = r.U64();
    ASSERT_TRUE(tag.has_value() && tag->has_value());
    EXPECT_EQ(**tag, "SELECT");
    EXPECT_EQ(*affected, 3u);
}

TEST_F(KwpSessionTest, ASelectStarDescribesTheWholeSchema) {
    Handshake();
    auto frames = RunStatement("SELECT * FROM t");
    ASSERT_GE(frames.size(), 3u);
    auto desc = wire::DecodeRowDescription(frames[0].payload);
    ASSERT_TRUE(desc.ok());
    ASSERT_EQ(desc.value().size(), 3u) << "id, v, name";
    EXPECT_EQ(desc.value()[2].name, "name");
    EXPECT_EQ(desc.value()[2].type_oid, catalog::kTypeValVarchar);
    EXPECT_EQ(desc.value()[2].type_len, -1) << "a varchar is variable on the wire";
    EXPECT_NE(desc.value()[0].flags & wire::kFieldFlagKeystone, 0)
        << "field 0 of every user relation is the Keystone id";
}

TEST_F(KwpSessionTest, AReorderedJoinStillFlagsTheFromRelationsKey) {
    // CB-S2: `w`'s literal makes it the driver, so `u` runs second - and the
    // flag stays on the FROM relation's tuple key, whatever order ran.
    Handshake();
    const std::string sql =
        "SELECT u.id, w.id FROM t AS u JOIN t AS w ON u.id = w.v WHERE w.name = 'x'";
    // (join_planner_test.cpp pins that this statement is reordered.)
    auto frames = RunStatement(sql);
    ASSERT_GE(frames.size(), 2u);
    auto desc = wire::DecodeRowDescription(frames[0].payload);
    ASSERT_TRUE(desc.ok()) << desc.status().message();
    ASSERT_EQ(desc.value().size(), 2u);
    EXPECT_NE(desc.value()[0].flags & wire::kFieldFlagKeystone, 0);
    EXPECT_EQ(desc.value()[1].flags & wire::kFieldFlagKeystone, 0);
}

TEST_F(KwpSessionTest, ANullableDrivingRelationsKeyIsNotFlaggedTheTupleKey) {
    // The flag promises a value that is never NULL; a RIGHT join's left
    // relation is null-extended (BU-S6, the query layer).
    Handshake();
    auto frames = RunStatement("SELECT u.id, w.id FROM t AS u RIGHT JOIN t AS w ON u.id = w.id");
    ASSERT_GE(frames.size(), 3u);
    auto desc = wire::DecodeRowDescription(frames[0].payload);
    ASSERT_TRUE(desc.ok()) << desc.status().message();
    ASSERT_EQ(desc.value().size(), 2u);
    EXPECT_EQ(desc.value()[0].flags & wire::kFieldFlagKeystone, 0);
    frames = RunStatement("SELECT u.id, w.id FROM t AS u LEFT JOIN t AS w ON u.id = w.id");
    ASSERT_GE(frames.size(), 3u);
    desc = wire::DecodeRowDescription(frames[0].payload);
    ASSERT_TRUE(desc.ok()) << desc.status().message();
    EXPECT_NE(desc.value()[0].flags & wire::kFieldFlagKeystone, 0);
}

TEST_F(KwpSessionTest, ADerivedTableKeepsItsOwnOutputsTupleKeyFlag) {
    // A derived table's column 0 is flagged only if its own output flagged
    // it (BU-S7): a LEFT join's right key is NULL where nothing matched.
    Handshake();
    auto frames = RunStatement(
        "SELECT x.k FROM (SELECT w.id AS k FROM t AS u LEFT JOIN t AS w ON w.id = u.id + 100) AS x");
    ASSERT_GE(frames.size(), 3u);
    auto desc = wire::DecodeRowDescription(frames[0].payload);
    ASSERT_TRUE(desc.ok()) << desc.status().message();
    EXPECT_EQ(desc.value()[0].flags & wire::kFieldFlagKeystone, 0);
    frames = RunStatement("SELECT x.id FROM (SELECT id FROM t) AS x");
    ASSERT_GE(frames.size(), 3u);
    desc = wire::DecodeRowDescription(frames[0].payload);
    ASSERT_TRUE(desc.ok()) << desc.status().message();
    EXPECT_NE(desc.value()[0].flags & wire::kFieldFlagKeystone, 0);
}

TEST_F(KwpSessionTest, ANarrowTupleKeyPastItsDeclaredWidthReachesTheClientWhole) {
    // protocol.md §6: a relation's column 0 is its Keystone id, u64 on the
    // wire whatever integer width it declares - in any projected position
    // and on a joined relation as much as the first one.
    Handshake();
    ASSERT_EQ(Run("CREATE TABLE np (id int32, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE TABLE nq (id int8, w int64)").substr(0, 7), "CREATED");
    const std::int64_t id = (std::int64_t{1} << 31) + 5;
    ASSERT_EQ(Run("INSERT INTO np VALUES (" + std::to_string(id) + ", 300)").substr(0, 8),
              "INSERTED");
    ASSERT_EQ(Run("INSERT INTO nq VALUES (300, 7)").substr(0, 8), "INSERTED");

    const auto expect_id = [&](const std::string& sql, std::size_t field, std::int64_t want) {
        auto frames = RunStatement(sql);
        ASSERT_EQ(frames.size(), 3u) << sql;
        auto desc = wire::DecodeRowDescription(frames[0].payload);
        ASSERT_TRUE(desc.ok()) << desc.status().message();
        EXPECT_EQ(desc.value()[field].type_len, 8) << sql;
        auto rows = wire::DecodeRowBatch(frames[1].payload, desc.value().size());
        ASSERT_TRUE(rows.ok()) << rows.status().message();
        ASSERT_EQ(rows.value().size(), 1u) << sql;
        ASSERT_EQ(rows.value()[0][field].bytes.size(), 8u) << sql;
        std::int64_t got = 0;
        for (std::size_t b = 0; b < 8; ++b) {
            got |= std::int64_t{std::to_integer<std::uint8_t>(rows.value()[0][field].bytes[b])}
                   << (8 * b);
        }
        EXPECT_EQ(got, want) << sql;
    };
    expect_id("SELECT * FROM np", 0, id);
    expect_id("SELECT v, id FROM np", 1, id);
    expect_id("SELECT np.v, nq.id FROM np JOIN nq ON nq.id = np.v", 1, 300);
    expect_id("SELECT id FROM np ORDER BY v", 0, id);
    // A fold's MIN/MAX and a group key over column 0 are the id too.
    expect_id("SELECT MAX(id) FROM np", 0, id);
    expect_id("SELECT id, COUNT(*) FROM np GROUP BY id", 0, id);
}

TEST_F(KwpSessionTest, AWriteAnswersACompletionWithItsRowCountAndNoDescription) {
    Handshake();
    auto frames = RunStatement("INSERT INTO t VALUES (40, 'delta')");
    ASSERT_EQ(frames.size(), 1u);
    ASSERT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kComplete));
    wire::PayloadReader r(frames[0].payload);
    auto tag = r.Text();
    auto affected = r.U64();
    ASSERT_TRUE(tag.has_value() && tag->has_value());
    EXPECT_EQ((**tag).substr(0, 8), "INSERTED");
    EXPECT_EQ(*affected, 1u) << "the tag's own count, read off the reply the engine wrote";
}

// RETURNING (BU-S8, BU-Q17): the rows as a SELECT's, then the write's own
// completion - its tag and its count, not `SELECT`'s.
TEST_F(KwpSessionTest, AReturningWriteAnswersItsRowsThenItsOwnCompletion) {
    Handshake();
    auto frames = RunStatement("UPDATE t SET name = 'z' WHERE id < 3 RETURNING id, name");
    ASSERT_EQ(frames.size(), 3u);
    ASSERT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kRowDesc));
    auto desc = wire::DecodeRowDescription(frames[0].payload);
    ASSERT_TRUE(desc.ok()) << desc.status().message();
    ASSERT_EQ(desc.value().size(), 2u);
    EXPECT_NE(desc.value()[0].flags & wire::kFieldFlagKeystone, 0);
    ASSERT_EQ(frames[1].type, static_cast<std::uint8_t>(ServerFrameType::kRowBatch));
    ASSERT_EQ(frames[2].type, static_cast<std::uint8_t>(ServerFrameType::kComplete));
    wire::PayloadReader r(frames[2].payload);
    auto tag = r.Text();
    auto affected = r.U64();
    ASSERT_TRUE(tag.has_value() && tag->has_value());
    EXPECT_EQ(**tag, "UPDATED 2");
    EXPECT_EQ(*affected, 2u);
}

// A locking read (DC): a `SELECT`'s description, rows and completion, frame
// for frame the plain statement's - no code in the session knows the clause.
TEST_F(KwpSessionTest, ALockingReadAnswersThePlainSelectsFramesByteForByte) {
    Handshake();
    ASSERT_EQ(Run("BEGIN").rfind("BEGIN", 0), 0u);
    for (const char* sql : {"SELECT id, v FROM t", "SELECT * FROM t WHERE id = 2",
                            "SELECT name FROM t WHERE id IN (1, 3)"}) {
        const auto plain = RunStatement(sql);
        const auto locking = RunStatement(std::string(sql) + " FOR UPDATE");
        ASSERT_EQ(plain.size(), 3u) << sql;
        ASSERT_EQ(locking.size(), plain.size()) << sql;
        for (std::size_t i = 0; i < plain.size(); ++i) {
            EXPECT_EQ(locking[i].type, plain[i].type) << sql << ", frame " << i;
            EXPECT_EQ(locking[i].payload, plain[i].payload) << sql << ", frame " << i;
        }
    }
    // Outside a transaction it is an error frame, and no description.
    ASSERT_EQ(Run("COMMIT").rfind("COMMIT", 0), 0u);
    const auto refused = RunStatement("SELECT id FROM t FOR UPDATE");
    ASSERT_FALSE(refused.empty());
    EXPECT_EQ(refused[0].type, static_cast<std::uint8_t>(ServerFrameType::kError));
}

TEST_F(KwpSessionTest, ACancelDuringALockingReadIsSeenAtTheNextFrameAndFailsTheTransaction) {
    // A locking read's walk is a write's: it reads no cancel flag, so a
    // cancel that arrives while it runs does not cut it short - the rows
    // come back - and refuses the next frame, leaving the transaction
    // failed with its claims held until the ROLLBACK (`protocol.md` §10).
    Handshake();
    ASSERT_EQ(Run("BEGIN").rfind("BEGIN", 0), 0u);
    dispatcher_->SetAfterViewForTest([&] { session_->RequestCancel(); });
    const auto ran = RunStatement("SELECT id FROM t WHERE v > 0 FOR UPDATE");
    dispatcher_->SetAfterViewForTest(nullptr);
    ASSERT_EQ(ran.size(), 3u) << "the walk was cut short by the cancel";
    EXPECT_EQ(ran[2].type, static_cast<std::uint8_t>(ServerFrameType::kComplete));

    const auto next = Feed(ClientFrameType::kPing, {});
    ASSERT_EQ(next.size(), 1u);
    auto err = wire::DecodeError(next[0].payload);
    ASSERT_TRUE(err.ok());
    EXPECT_EQ(err.value().category(), wire::ErrorCategory::kCancelled);
    EXPECT_TRUE(session_->session().failed());
    ASSERT_EQ(Feed(ClientFrameType::kSync, {}).size(), 1u);
    EXPECT_EQ(Run("ROLLBACK").rfind("ROLLBACK", 0), 0u);
}

TEST_F(KwpSessionTest, AMultiLineDiagnosticArrivesAsAOneColumnTextResultSet) {
    // `ANALYZE` - a plan, not a table - is the one text-lines reply BO-R3
    // leaves; every `sys` view and `SHOW` answers as a typed result set.
    Handshake();
    auto frames = RunStatement("ANALYZE SELECT * FROM t");
    ASSERT_EQ(frames.size(), 3u);
    auto desc = wire::DecodeRowDescription(frames[0].payload);
    ASSERT_TRUE(desc.ok());
    ASSERT_EQ(desc.value().size(), 1u);
    EXPECT_EQ(desc.value()[0].name, "line")
        << "a reply the engine has never typed is one column of text, not a bespoke shape";
    auto rows = wire::DecodeRowBatch(frames[1].payload, 1);
    ASSERT_TRUE(rows.ok());
    EXPECT_GT(rows.value().size(), 1u) << "one row per line of the answer";
    EXPECT_EQ(frames[2].type, static_cast<std::uint8_t>(ServerFrameType::kComplete));
}

TEST_F(KwpSessionTest, ASingleLineDiagnosticIsTheCompletionTagItself) {
    // The rule, stated where it can be checked: a statement with no typed
    // result set answers with its reply as the tag, and only a reply
    // carrying lines becomes a text result set - because a tag has no
    // lines. `PING` is one line, so it is a tag.
    Handshake();
    auto frames = RunStatement("PING");
    ASSERT_EQ(frames.size(), 1u);
    ASSERT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kComplete));
    wire::PayloadReader r(frames[0].payload);
    auto tag = r.Text();
    ASSERT_TRUE(tag.has_value() && tag->has_value());
    EXPECT_EQ(**tag, "PONG");
}

// ---- P08: named statements, portals and their caps ------------------------

TEST_F(KwpSessionTest, TheUnnamedStatementIsOverwrittenAndANamedOneSurvives) {
    Handshake();
    Feed(ClientFrameType::kParse, Parse("", "SELECT id FROM t"));
    Feed(ClientFrameType::kParse, Parse("keep", "SELECT v FROM t"));
    Feed(ClientFrameType::kParse, Parse("", "SELECT name FROM t"));
    EXPECT_EQ(session_->statement_count(), 2u) << "the unnamed one was replaced, not added";

    Feed(ClientFrameType::kBind, Bind("p", ""));
    auto frames = Feed(ClientFrameType::kExecute, Execute("p", 0));
    auto desc = wire::DecodeRowDescription(frames[0].payload);
    ASSERT_TRUE(desc.ok());
    EXPECT_EQ(desc.value()[0].name, "name") << "the unnamed statement is the latest one";
}

TEST_F(KwpSessionTest, ClosingAStatementClosesThePortalsBoundFromIt) {
    Handshake();
    Feed(ClientFrameType::kParse, Parse("s", "SELECT id FROM t"));
    Feed(ClientFrameType::kBind, Bind("a", "s"));
    Feed(ClientFrameType::kBind, Bind("b", "s"));
    ASSERT_EQ(session_->portal_count(), 2u);
    Feed(ClientFrameType::kClose, Handle(1, "s"));
    EXPECT_EQ(session_->statement_count(), 0u);
    EXPECT_EQ(session_->portal_count(), 0u)
        << "a portal is a binding of a statement; one left behind names text nothing holds";
}

TEST_F(KwpSessionTest, TheStatementAndPortalCapsRefuseWithTheirOwnDetail) {
    Handshake();
    for (std::size_t i = 0; i < kMaxSessionStatements; ++i) {
        ASSERT_EQ(Feed(ClientFrameType::kParse,
                       Parse("s" + std::to_string(i), "SELECT id FROM t"))
                      .size(),
                  1u);
    }
    auto frames = Feed(ClientFrameType::kParse, Parse("one-too-many", "SELECT id FROM t"));
    ASSERT_EQ(frames.size(), 1u);
    ASSERT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kError));
    auto err = wire::DecodeError(frames[0].payload);
    ASSERT_TRUE(err.ok());
    EXPECT_EQ(err.value().category(), wire::ErrorCategory::kResourceExhausted);
    EXPECT_EQ(err.value().detail_code(),
              static_cast<std::uint16_t>(wire::ResourceDetail::kStatementLimit));
    EXPECT_FALSE(err.value().retryable) << "a cap is not a race";
}

TEST_F(KwpSessionTest, AFailedStatementTakesItsPortalWithIt) {
    // **XG-R8.** The leak this closes was not the client's to fix: §5's
    // skip-to-sync discards every frame up to the next `C_SYNC`, so a
    // `C_CLOSE` pipelined behind the statement that just failed is dropped
    // on exactly the statements that most need it.
    Handshake();
    ASSERT_EQ(session_->portal_count(), 0u);

    auto failed = RunStatement("SELECT id FROM no_such_relation");
    ASSERT_EQ(failed.size(), 1u);
    ASSERT_EQ(failed[0].type, static_cast<std::uint8_t>(ServerFrameType::kError));
    EXPECT_EQ(session_->portal_count(), 0u)
        << "a failed statement left its portal behind for a close that skip-to-sync eats";

    // The lifecycle a client sees, per `protocol.md` §7: the name is gone,
    // so a later `C_CLOSE` of it is the no-op it already is on an absent
    // name rather than an error. Sent after the barrier, since the session
    // is skipping to sync.
    Feed(ClientFrameType::kSync, {});
    EXPECT_TRUE(Feed(ClientFrameType::kClose, Handle(2, "p")).empty())
        << "closing a portal that failed must not itself be an error";
}

TEST_F(KwpSessionTest, ErroringPastThePortalCapNoLongerWedgesTheSession) {
    // The consequence, and the reason this was found by being stopped by
    // it: a session that errors `kMaxSessionPortals` times used to hold 64
    // dead portals and refuse every further `C_BIND` until the 60 s sweep
    // freed one. A retry loop then ran at one statement per portal
    // lifetime, which is what stalled an entire benchmark matrix.
    Handshake();
    for (std::size_t i = 0; i < kMaxSessionPortals + 8; ++i) {
        auto failed = RunStatement("SELECT id FROM no_such_relation");
        ASSERT_EQ(failed.size(), 1u) << "at attempt " << i;
        ASSERT_EQ(failed[0].type, static_cast<std::uint8_t>(ServerFrameType::kError))
            << "at attempt " << i;
        Feed(ClientFrameType::kSync, {});
    }
    EXPECT_EQ(session_->portal_count(), 0u);

    // And the session still works, which is the whole point - the previous
    // behaviour answered `RESOURCE_EXHAUSTED` here.
    auto ok = RunStatement("SELECT id FROM t");
    ASSERT_FALSE(ok.empty());
    EXPECT_NE(ok[0].type, static_cast<std::uint8_t>(ServerFrameType::kError))
        << "the session was still wedged after erroring past the portal cap";
}

// ---- P08: the pipelining error contract (§5, §15-3) ----------------------

TEST_F(KwpSessionTest, AfterAnErrorEveryFrameIsDiscardedUntilTheNextSync) {
    Handshake();
    // A statement that fails in the engine, not in the protocol - so the
    // skip-to-sync posture is armed by a real refusal.
    auto failed = RunStatement("SELECT id FROM no_such_relation");
    ASSERT_EQ(failed.size(), 1u);
    ASSERT_EQ(failed[0].type, static_cast<std::uint8_t>(ServerFrameType::kError));

    // Everything the client pipelined behind it is swallowed.
    EXPECT_TRUE(Feed(ClientFrameType::kParse, Parse("x", "SELECT id FROM t")).empty());
    EXPECT_TRUE(Feed(ClientFrameType::kBind, Bind("q", "x")).empty());
    EXPECT_TRUE(Feed(ClientFrameType::kExecute, Execute("q", 0)).empty());
    EXPECT_TRUE(Feed(ClientFrameType::kPing, {}).empty())
        << "even a PING; the rule is every frame, so a client cannot half-recover";

    auto resumed = Feed(ClientFrameType::kSync, {});
    ASSERT_EQ(resumed.size(), 1u);
    EXPECT_EQ(resumed[0].type, static_cast<std::uint8_t>(ServerFrameType::kReady));

    // And the session works again.
    EXPECT_EQ(RunStatement("SELECT id FROM t").size(), 3u);
}

TEST_F(KwpSessionTest, AnEngineRefusalKeepsItsOwnCategory) {
    // **The taxonomy has to survive the dispatcher**, and for a while it
    // did not: the only route to a category was parsing the rendered reply
    // line, whose bare arm folds every code but four into
    // `InvalidArgument` - so a missing relation, an unsupported form and a
    // spent budget all reached a client as INVALID_ARGUMENT, which is P12's
    // R2 defeated at the one seam that feeds it.
    Handshake();
    struct Case {
        const char* sql;
        wire::ErrorCategory expect;
    };
    const Case kCases[] = {
        {"SELECT id FROM no_such_relation", wire::ErrorCategory::kNotFound},
        // An outer join is reserved by the grammar and refused with its
        // keyword's position (parser-v2.md I9) - a *well-formed* statement
        // the engine will not run, which is exactly the distinction
        // folding into INVALID_ARGUMENT erased.
        //
        // `NotImplemented` and not `Unsupported`, which is the operator's
        // 2026-08-31 rule read correctly: a CTE is a form the design admits
        // and nobody built, so a later release could lift the refusal
        // without changing the architecture (an outer join was this case
        // until BU-S6 built it). It is also the case
        // that makes this test worth having - the two codes are one word
        // apart in the message and two categories apart on the wire, and a
        // client's feature detection reads only the second.
        {"WITH w AS (SELECT id FROM t) SELECT id FROM w", wire::ErrorCategory::kNotImplemented},
    };
    for (const Case& c : kCases) {
        auto frames = RunStatement(c.sql);
        ASSERT_EQ(frames.size(), 1u) << c.sql;
        ASSERT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kError)) << c.sql;
        auto err = wire::DecodeError(frames[0].payload);
        ASSERT_TRUE(err.ok());
        EXPECT_EQ(err.value().category(), c.expect) << c.sql << " -> " << err.value().message;
        EXPECT_FALSE(err.value().retryable);
        ASSERT_EQ(Feed(ClientFrameType::kSync, {}).size(), 1u);
    }
}

TEST_F(KwpSessionTest, AnUnknownFrameTypeIsRefusedWithoutClosing) {
    Handshake();
    auto frames = Feed(static_cast<ClientFrameType>(200), {});
    ASSERT_EQ(frames.size(), 1u);
    auto err = wire::DecodeError(frames[0].payload);
    ASSERT_TRUE(err.ok());
    EXPECT_EQ(err.value().category(), wire::ErrorCategory::kProtocol);
    EXPECT_EQ(err.value().detail_code(),
              static_cast<std::uint16_t>(wire::ProtocolDetail::kUnknownFrameType));
    EXPECT_EQ(err.value().severity, wire::Severity::kError);
    EXPECT_FALSE(closed_) << "a frame from a later version is what capabilities exist to survive";
}

// ---- P08: bound parameters ----------------------------------------------

TEST_F(KwpSessionTest, AParameterIsBoundAndTheStatementRunsWithIt) {
    Handshake();
    Feed(ClientFrameType::kParse, Parse("s", "SELECT id, v FROM t WHERE v = ?"));

    const std::int32_t twenty = 20;
    std::vector<std::byte> bytes(4);
    for (int i = 0; i < 4; ++i) {
        bytes[i] = static_cast<std::byte>((static_cast<std::uint32_t>(twenty) >> (8 * i)) & 0xFF);
    }
    wire::BoundParam p;
    p.type_oid = catalog::kTypeValInt32;
    p.bytes = std::span<const std::byte>(bytes);
    Feed(ClientFrameType::kBind, Bind("p", "s", {p}));

    auto frames = Feed(ClientFrameType::kExecute, Execute("p", 0));
    ASSERT_EQ(frames.size(), 3u);
    auto rows = wire::DecodeRowBatch(frames[1].payload, 2);
    ASSERT_TRUE(rows.ok());
    EXPECT_EQ(rows.value().size(), 1u) << "one row has v = 20";
}

TEST_F(KwpSessionTest, ANegativeValueBoundAfterAMinusIsNotAComment) {
    // `x -?` bound with -5 used to paste as `x --5`, a line comment that ate
    // the rest of the statement - here the WHERE, so a statement meant for one
    // row was a statement for all of them (BJ-S2's review). It is a minus
    // and a negative literal now: the expression `100 - -5` under a WHERE
    // that matches no row, so nothing is written. Without the fix the WHERE
    // is gone and every row is set to 100.
    Handshake();
    auto Int32 = [](std::int32_t v) {
        std::vector<std::byte> bytes(4);
        for (int i = 0; i < 4; ++i) {
            bytes[i] = static_cast<std::byte>((static_cast<std::uint32_t>(v) >> (8 * i)) & 0xFF);
        }
        return bytes;
    };
    const std::vector<std::byte> minus5 = Int32(-5);
    const std::vector<std::byte> ten = Int32(10);
    wire::BoundParam a;
    a.type_oid = catalog::kTypeValInt32;
    a.bytes = std::span<const std::byte>(minus5);
    wire::BoundParam b;
    b.type_oid = catalog::kTypeValInt32;
    b.bytes = std::span<const std::byte>(ten);

    const std::string before = Run("SELECT id, v FROM t");
    Feed(ClientFrameType::kParse, Parse("s", "UPDATE t SET v = 100 -? WHERE id = ?"));
    Feed(ClientFrameType::kBind, Bind("p", "s", {a, b}));
    auto frames = Feed(ClientFrameType::kExecute, Execute("p", 0));
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_NE(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kError))
        << "the statement parses and runs; it kept its WHERE";
    EXPECT_EQ(Run("SELECT id, v FROM t"), before);
}

TEST_F(KwpSessionTest, ANullParameterBindsAsNull) {
    Handshake();
    Feed(ClientFrameType::kParse, Parse("s", "SELECT id FROM t WHERE v = ?"));
    wire::BoundParam p;
    p.type_oid = catalog::kTypeValInt32;  // absent bytes = NULL
    Feed(ClientFrameType::kBind, Bind("p", "s", {p}));
    auto frames = Feed(ClientFrameType::kExecute, Execute("p", 0));
    // `= NULL` is unknown for every row, so the answer is a description
    // and no rows - three-valued WHERE, unchanged by the protocol.
    ASSERT_GE(frames.size(), 2u);
    EXPECT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kRowDesc));
    EXPECT_EQ(frames.back().type, static_cast<std::uint8_t>(ServerFrameType::kComplete));
}

TEST_F(KwpSessionTest, ATextParameterCarryingAQuoteIsRefusedWithTheGrammarsReason) {
    // The unnamed statement is the text path's (EA-R4): its value is
    // substituted, and a quote has no spelling. A named one binds it (EA-S1,
    // `AQuoteBindsThroughANamedStatementsKeptParse`).
    Handshake();
    Feed(ClientFrameType::kParse, Parse("", "SELECT id FROM t WHERE name = ?"));
    const std::string value = "O'Brien";
    wire::BoundParam p;
    p.type_oid = catalog::kTypeValVarchar;
    p.bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()),
                                         value.size());
    auto frames = Feed(ClientFrameType::kBind, Bind("p", "", {p}));
    ASSERT_EQ(frames.size(), 1u);
    ASSERT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kError));
    auto err = wire::DecodeError(frames[0].payload);
    ASSERT_TRUE(err.ok());
    EXPECT_NE(err.value().message.find("no escape for a quote"), std::string::npos)
        << "the refusal names the grammar's limit, which is whose it is";
}

// ---- EA-S1: a named statement's kept parse --------------------------------

namespace ea {

wire::BoundParam Fixed(std::uint32_t type, std::vector<std::byte>& store, std::uint64_t v,
                       std::size_t width, std::uint32_t type_mod = 0) {
    store.assign(width, std::byte{0});
    for (std::size_t i = 0; i < width; ++i) {
        store[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
    wire::BoundParam p;
    p.type_oid = type;
    p.type_mod = type_mod;
    p.bytes = std::span<const std::byte>(store);
    return p;
}

wire::BoundParam Text(std::uint32_t type, const std::string& s) {
    wire::BoundParam p;
    p.type_oid = type;
    p.bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
    return p;
}

// A frame list with each error reduced to its code: a refusal's byte is the
// marker's on the kept path and the substituted literal's on the text path
// (EA-R3), and nothing else may differ. A completion's tag is its text with
// the relation's oid and page left out, so twin relations compare.
std::string Answer(const std::vector<wire::DecodedFrame>& frames) {
    std::string out;
    for (const auto& f : frames) {
        out += "[" + std::to_string(f.type) + ":";
        if (f.type == static_cast<std::uint8_t>(ServerFrameType::kError)) {
            auto err = wire::DecodeError(f.payload);
            out += err.ok() ? std::to_string(err.value().code) : "?";
        } else if (f.type == static_cast<std::uint8_t>(ServerFrameType::kComplete)) {
            wire::PayloadReader r(f.payload);
            auto tag = r.Text();
            const std::string text = tag.has_value() && tag->has_value() ? **tag : "?";
            out += std::regex_replace(text, std::regex("(oid|page)=[0-9]+"), "$1=#");
        } else {
            for (std::byte b : f.payload) out += std::to_string(std::to_integer<int>(b)) + ",";
        }
        out += "]";
    }
    return out;
}

}  // namespace ea

TEST_F(KwpSessionTest, AQuoteBindsThroughANamedStatementsKeptParse) {
    Handshake();
    const std::string value = "O'Brien";
    Feed(ClientFrameType::kParse, Parse("ins", "INSERT INTO t (id, v, name) VALUES (?, ?, ?)"));
    std::vector<std::byte> id_bytes, v_bytes;
    auto bound = Feed(ClientFrameType::kBind,
                      Bind("p", "ins",
                           {ea::Fixed(catalog::kTypeValInt64, id_bytes, 40, 8),
                            ea::Fixed(catalog::kTypeValInt32, v_bytes, 7, 4),
                            ea::Text(catalog::kTypeValVarchar, value)}));
    ASSERT_EQ(bound.size(), 1u);
    ASSERT_EQ(bound[0].type, static_cast<std::uint8_t>(ServerFrameType::kBindOk));
    auto wrote = Feed(ClientFrameType::kExecute, Execute("p", 0));
    ASSERT_FALSE(wrote.empty());
    ASSERT_NE(wrote[0].type, static_cast<std::uint8_t>(ServerFrameType::kError));

    Feed(ClientFrameType::kParse, Parse("sel", "SELECT id, v FROM t WHERE name = ?"));
    Feed(ClientFrameType::kBind, Bind("q", "sel", {ea::Text(catalog::kTypeValVarchar, value)}));
    auto frames = Feed(ClientFrameType::kExecute, Execute("q", 0));
    ASSERT_EQ(frames.size(), 3u);
    auto rows = wire::DecodeRowBatch(frames[1].payload, 2);
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 1u) << "the row holding the quote, found by it";
}

TEST_F(KwpSessionTest, AQuoteBindsInAWriteThatAnswersWithReturning) {
    // The write and its `RETURNING` both read the statement's parse - the
    // walk's `UpdateInner` and `WithReturning` - and both take the kept one,
    // so a value no literal can spell is written and answered.
    Handshake();
    const std::string value = "it's";
    Feed(ClientFrameType::kParse,
         Parse("u", "UPDATE t SET name = ? WHERE v = ? RETURNING v, name"));
    std::vector<std::byte> v_bytes;
    Feed(ClientFrameType::kBind, Bind("p", "u",
                                      {ea::Text(catalog::kTypeValVarchar, value),
                                       ea::Fixed(catalog::kTypeValInt32, v_bytes, 10, 4)}));
    auto frames = Feed(ClientFrameType::kExecute, Execute("p", 0));
    ASSERT_EQ(frames.size(), 3u) << ea::Answer(frames);
    EXPECT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kRowDesc));
    auto rows = wire::DecodeRowBatch(frames[1].payload, 2);
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 1u);
    EXPECT_EQ(Run("SELECT name FROM t WHERE v = 10"), "name\\nit's");
}

TEST_F(KwpSessionTest, EveryStorableTypeAndNullBindsAsItsTextBinds) {
    // One relation of every storable type, written twice - once by a text
    // statement with literals, once by a named statement's kept parse - and
    // read back both ways: the rows and every frame agree (EA-R1).
    Run("CREATE TABLE ty (id int64, a int16, b int32, c uint64, d bool, e date, f timestamp, "
        "g decimal(10,2), h decimal(30,4), i varchar, j char(8), n int32 NULL)");
    Handshake();
    std::vector<std::byte> b_id, b_a, b_b, b_c, b_d, b_e, b_f, b_g, b_h_lo;
    std::vector<std::byte> wide(16, std::byte{0});
    // 123456789.0123 at scale 4: unscaled 1234567890123, low half first (§6).
    const std::uint64_t unscaled = 1234567890123ULL;
    for (int i = 0; i < 8; ++i) wide[i] = static_cast<std::byte>((unscaled >> (8 * i)) & 0xFF);
    wire::BoundParam h;
    h.type_oid = catalog::kTypeValDecimalWide;
    h.type_mod = 4;
    h.bytes = std::span<const std::byte>(wide);
    wire::BoundParam null_n;
    null_n.type_oid = catalog::kTypeValInt32;
    const std::vector<wire::BoundParam> params = {
        ea::Fixed(catalog::kTypeValInt64, b_id, 2, 8),
        ea::Fixed(catalog::kTypeValInt16, b_a, static_cast<std::uint64_t>(-3), 2),
        ea::Fixed(catalog::kTypeValInt32, b_b, 70000, 4),
        ea::Fixed(catalog::kTypeValUint64, b_c, 18446744073709551615ULL, 8),
        ea::Fixed(catalog::kTypeValBool, b_d, 1, 1),
        ea::Fixed(catalog::kTypeValDate, b_e, 20480, 4),
        ea::Fixed(catalog::kTypeValTimestamp, b_f, 1769472000123456ULL, 8),
        ea::Fixed(catalog::kTypeValDecimal, b_g, static_cast<std::uint64_t>(-1250), 8, 2),
        h,
        ea::Text(catalog::kTypeValVarchar, "text"),
        ea::Text(catalog::kTypeValChar, "chars"),
    };
    Run("INSERT INTO ty VALUES (1, -3, 70000, 18446744073709551615, 1, '2026-01-27', "
        "'2026-01-27 00:00:00.123456', -12.50, 123456789.0123, 'text', 'chars', NULL)");

    std::vector<wire::BoundParam> with_null = params;
    with_null.push_back(null_n);
    Feed(ClientFrameType::kParse,
         Parse("ins", "INSERT INTO ty VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    Feed(ClientFrameType::kBind, Bind("p", "ins", with_null));
    auto wrote = Feed(ClientFrameType::kExecute, Execute("p", 0));
    ASSERT_FALSE(wrote.empty());
    ASSERT_NE(wrote[0].type, static_cast<std::uint8_t>(ServerFrameType::kError))
        << ea::Answer(wrote);
    const std::string rows = Run("SELECT a, b, c, d, e, f, g, h, i, j, n FROM ty");
    const auto nl = rows.find("\\n");
    ASSERT_NE(nl, std::string::npos);
    const std::string body = rows.substr(nl + 2);
    const auto split = body.find("\\n");
    ASSERT_NE(split, std::string::npos) << rows;
    EXPECT_EQ(body.substr(0, split), body.substr(split + 2)) << "both rows alike: " << rows;

    // Each column found by its value, the text way and the kept way.
    const char* columns[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j"};
    const char* literals[] = {"-3",           "70000",
                              "18446744073709551615", "1",
                              "'2026-01-27'", "'2026-01-27 00:00:00.123456'",
                              "-12.50",       "123456789.0123",
                              "'text'",       "'chars'"};
    for (std::size_t k = 0; k < 10; ++k) {
        const std::string text =
            std::string("SELECT id FROM ty WHERE ") + columns[k] + " = " + literals[k];
        Feed(ClientFrameType::kParse, Parse("", text));
        Feed(ClientFrameType::kBind, Bind("t", ""));
        const std::string want = ea::Answer(Feed(ClientFrameType::kExecute, Execute("t", 0)));
        const std::string held = std::string("SELECT id FROM ty WHERE ") + columns[k] + " = ?";
        Feed(ClientFrameType::kParse, Parse("k", held));
        Feed(ClientFrameType::kBind, Bind("kp", "k", {params[k + 1]}));
        const std::string got = ea::Answer(Feed(ClientFrameType::kExecute, Execute("kp", 0)));
        EXPECT_EQ(got, want) << text;
    }
}

TEST_F(KwpSessionTest, AKeptParseAnswersAsItsTextOverAShapeSet) {
    // EA-R1 through the protocol: each statement run with its literals by the
    // unnamed statement, then with `?` by a named one, on twin copies of one
    // relation, so a write's effect is compared too.
    Run("CREATE TABLE u (id int64, v int32, w varchar)");
    Run("INSERT INTO u VALUES (1, 5, 'a'), (2, 6, 'b'), (3, 7, 'c')");
    Run("CREATE TABLE u2 (id int64, v int32, w varchar)");
    Run("INSERT INTO u2 VALUES (1, 5, 'a'), (2, 6, 'b'), (3, 7, 'c')");
    Handshake();
    struct Case {
        const char* text;  // `%` stands for the relation
        const char* held;
        std::vector<std::int64_t> ints;
    };
    const std::vector<Case> cases = {
        {"SELECT * FROM % WHERE id = 2", "SELECT * FROM % WHERE id = ?", {2}},
        {"SELECT id FROM % WHERE v > 5 AND v < 7", "SELECT id FROM % WHERE v > ? AND v < ?",
         {5, 7}},
        {"SELECT id FROM % WHERE id IN (1, 3)", "SELECT id FROM % WHERE id IN (?, ?)", {1, 3}},
        {"SELECT id FROM % WHERE v BETWEEN 5 AND 6", "SELECT id FROM % WHERE v BETWEEN ? AND ?",
         {5, 6}},
        {"SELECT COUNT(*) FROM % WHERE v >= 6", "SELECT COUNT(*) FROM % WHERE v >= ?", {6}},
        {"SELECT * FROM % WHERE id = 99", "SELECT * FROM % WHERE id = ?", {99}},
        {"UPDATE % SET v = v + 10 WHERE id = 1", "UPDATE % SET v = v + ? WHERE id = ?", {10, 1}},
        {"UPDATE % SET v = 3 * (v - 2) WHERE id IN (2, 3)",
         "UPDATE % SET v = ? * (v - ?) WHERE id IN (?, ?)",
         {3, 2, 2, 3}},
        {"DELETE FROM % WHERE id = 3 RETURNING v", "DELETE FROM % WHERE id = ? RETURNING v", {3}},
        {"INSERT INTO % VALUES (4, 8, 'd')", "INSERT INTO % VALUES (?, ?, 'd')", {4, 8}},
        {"INSERT INTO % VALUES (4, 9, 'e') ON CONFLICT (id) DO UPDATE SET v = 9",
         "INSERT INTO % VALUES (?, ?, 'e') ON CONFLICT (id) DO UPDATE SET v = ?",
         {4, 9, 9}},
        {"SELECT * FROM % ORDER BY v DESC LIMIT 2", "SELECT * FROM % ORDER BY v DESC LIMIT 2",
         {}},
    };
    auto subst = [](std::string s, const char* rel) {
        const auto at = s.find('%');
        return s.replace(at, 1, rel);
    };
    std::vector<std::vector<std::byte>> stores(8);
    for (const Case& c : cases) {
        Feed(ClientFrameType::kParse, Parse("", subst(c.text, "u")));
        Feed(ClientFrameType::kBind, Bind("t", ""));
        const std::string want = ea::Answer(Feed(ClientFrameType::kExecute, Execute("t", 0)));

        std::vector<wire::BoundParam> params;
        for (std::size_t i = 0; i < c.ints.size(); ++i) {
            params.push_back(ea::Fixed(catalog::kTypeValInt64, stores[i],
                                       static_cast<std::uint64_t>(c.ints[i]), 8));
        }
        // Each held form keeps a parse, so the kept path is what is compared.
        EXPECT_TRUE(parser::KeptParse::Of(subst(c.held, "u2")).has_value()) << c.held;
        Feed(ClientFrameType::kParse, Parse("k", subst(c.held, "u2")));
        Feed(ClientFrameType::kBind, Bind("kp", "k", params));
        const std::string got = ea::Answer(Feed(ClientFrameType::kExecute, Execute("kp", 0)));
        EXPECT_EQ(got, want) << c.text;
    }
    EXPECT_EQ(Run("SELECT * FROM u2"), Run("SELECT * FROM u")) << "every write alike";
}

TEST_F(KwpSessionTest, AKeptParsesRefusalNamesTheMarkersByte) {
    // EA-R3: the compile refuses a string against an integer column at the
    // value's byte - on the kept path the `?` the client wrote.
    Handshake();
    const std::string held = "SELECT id FROM t WHERE id = ?";
    Feed(ClientFrameType::kParse, Parse("k", held));
    Feed(ClientFrameType::kBind, Bind("p", "k", {ea::Text(catalog::kTypeValVarchar, "zz")}));
    auto frames = Feed(ClientFrameType::kExecute, Execute("p", 0));
    ASSERT_FALSE(frames.empty());
    ASSERT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kError));
    auto err = wire::DecodeError(frames[0].payload);
    ASSERT_TRUE(err.ok());
    EXPECT_NE(err.value().message.find("byte " + std::to_string(held.find('?'))),
              std::string::npos)
        << err.value().message;
}

TEST_F(KwpSessionTest, APreparedStatementKeepsItsTextsPatternIdAndStatementKey) {
    // EA-R2: the `pattern_id` a PARSE answers, and the shape the statement is
    // counted under, are the text's on both paths.
    Handshake();
    auto text = Feed(ClientFrameType::kParse, Parse("", "SELECT id FROM t WHERE id = 20"));
    auto held = Feed(ClientFrameType::kParse, Parse("k", "SELECT id FROM t WHERE id = ?"));
    ASSERT_EQ(text.size(), 1u);
    ASSERT_EQ(held.size(), 1u);
    EXPECT_EQ(text[0].payload, held[0].payload) << "one pattern_id";
}

// BW-S6 (BW-Q12.11 of `instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`):
// a prepared column-listed INSERT takes its `?` in VALUES through the same
// substitution as every other statement, and each value lands in its named
// column.
TEST_F(KwpSessionTest, APreparedColumnListedInsertBindsItsValues) {
    Handshake();
    Feed(ClientFrameType::kParse, Parse("s", "INSERT INTO t (name, v) VALUES (?, ?)"));
    const std::string name = "delta";
    std::vector<std::byte> v(4);
    v[0] = static_cast<std::byte>(40);
    wire::BoundParam a;
    a.type_oid = catalog::kTypeValVarchar;
    a.bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(name.data()),
                                         name.size());
    wire::BoundParam b;
    b.type_oid = catalog::kTypeValInt32;
    b.bytes = std::span<const std::byte>(v);
    Feed(ClientFrameType::kBind, Bind("p", "s", {a, b}));
    auto frames = Feed(ClientFrameType::kExecute, Execute("p", 0));
    ASSERT_FALSE(frames.empty());
    EXPECT_NE(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kError));
    EXPECT_EQ(Run("SELECT v, name FROM t WHERE name = 'delta'"), "v,name\\n40,delta");
}

TEST_F(KwpSessionTest, AParameterCountThatDisagreesWithThePlaceholdersIsRefused) {
    Handshake();
    Feed(ClientFrameType::kParse, Parse("s", "SELECT id FROM t WHERE v = ? AND id = ?"));
    const std::int32_t v = 20;
    std::vector<std::byte> bytes(4);
    bytes[0] = static_cast<std::byte>(v);
    wire::BoundParam p;
    p.type_oid = catalog::kTypeValInt32;
    p.bytes = std::span<const std::byte>(bytes);
    auto frames = Feed(ClientFrameType::kBind, Bind("p", "s", {p}));
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kError));
}

// ---- P10: portals and streaming (§7, §15-4) -----------------------------

TEST_F(KwpSessionTest, MaxRowsZeroDeliversEverythingInOneGo) {
    Handshake();
    auto frames = RunStatement("SELECT id, v FROM t", /*max_rows=*/0);
    EXPECT_EQ(Only(std::move(frames), ServerFrameType::kPortalSuspended).size(), 0u);
}

TEST_F(KwpSessionTest, ASuspendedPortalResumesOnContinueAndThenCompletes) {
    // Three rows in one batch is the ordinary case, so suspension needs a
    // result set wider than one batch. The batch target is a size, so the
    // rows have to be many rather than wide - and this is the one place the
    // fixture builds a second relation, because the shared one is fixed.
    Handshake();
    ASSERT_EQ(Run("CREATE TABLE wide (id int64, note varchar)").substr(0, 7), "CREATED");
    const std::string note(120, 'x');
    for (int i = 0; i < 600; ++i) {
        ASSERT_EQ(Run("INSERT INTO wide VALUES ('" + note + "')").substr(0, 8), "INSERTED");
    }
    Feed(ClientFrameType::kParse, Parse("s", "SELECT id, note FROM wide"));
    Feed(ClientFrameType::kBind, Bind("p", "s"));

    // A quota below the row count suspends after the first batch.
    auto first = Feed(ClientFrameType::kExecute, Execute("p", 1));
    ASSERT_GE(first.size(), 2u);
    EXPECT_EQ(first[0].type, static_cast<std::uint8_t>(ServerFrameType::kRowDesc));
    EXPECT_EQ(first.back().type, static_cast<std::uint8_t>(ServerFrameType::kPortalSuspended))
        << "600 rows of 120 bytes cross the batch target, so a quota of 1 leaves\n"
           "rows behind";

    std::size_t batches = Only(first, ServerFrameType::kRowBatch).size();
    while (true) {
        auto more = Feed(ClientFrameType::kContinue, Execute("p", 1));
        batches += Only(more, ServerFrameType::kRowBatch).size();
        ASSERT_FALSE(more.empty());
        if (more.back().type == static_cast<std::uint8_t>(ServerFrameType::kComplete)) break;
        ASSERT_EQ(more.back().type,
                  static_cast<std::uint8_t>(ServerFrameType::kPortalSuspended));
    }
    EXPECT_GT(batches, 1u) << "the whole result crossed a batch boundary, which is the point";
}

TEST_F(KwpSessionTest, ExecutingAPortalTwiceIsRefusedRatherThanReRunningTheStatement) {
    Handshake();
    Feed(ClientFrameType::kParse, Parse("s", "INSERT INTO t VALUES (99, 'x')"));
    Feed(ClientFrameType::kBind, Bind("p", "s"));
    ASSERT_EQ(Feed(ClientFrameType::kExecute, Execute("p", 0))[0].type,
              static_cast<std::uint8_t>(ServerFrameType::kComplete));

    auto again = Feed(ClientFrameType::kExecute, Execute("p", 0));
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].type, static_cast<std::uint8_t>(ServerFrameType::kError))
        << "re-executing a portal would be a second write";
}

TEST_F(KwpSessionTest, APortalIdleBeyondTheTimeoutIsReleased) {
    Handshake();
    Feed(ClientFrameType::kParse, Parse("s", "SELECT id FROM t"));
    Feed(ClientFrameType::kBind, Bind("p", "s"));
    ASSERT_EQ(session_->portal_count(), 1u);

    // **Only on the injected clock** (§15-4): one nanosecond short does
    // nothing, which is what makes the timeout a decision rather than a
    // race.
    clock_.Advance(kPortalIdleTimeoutNs - 1);
    session_->ExpireIdlePortals();
    EXPECT_EQ(session_->portal_count(), 1u);

    clock_.Advance(1);
    session_->ExpireIdlePortals();
    EXPECT_EQ(session_->portal_count(), 0u);

    // **Not "unknown portal"** (§7, §11): the client did hold it and was
    // too slow, and the spec answers that `RESOURCE_EXHAUSTED` with its own
    // detail because the client's fix differs from a misspelled name.
    auto frames = Feed(ClientFrameType::kExecute, Execute("p", 0));
    ASSERT_EQ(frames.size(), 1u);
    auto err = wire::DecodeError(frames[0].payload);
    ASSERT_TRUE(err.ok());
    EXPECT_EQ(err.value().category(), wire::ErrorCategory::kResourceExhausted);
    EXPECT_EQ(err.value().detail_code(),
              static_cast<std::uint16_t>(wire::ResourceDetail::kPortalIdleTimeout));
}

TEST_F(KwpSessionTest, APortalWhoseStatementIsRunningIsNotIdle) {
    // A statement parked on a wait (group commit, a lock) holds its
    // portal's sink and needs the portal to answer at completion. The sweep
    // must not release it however long the statement takes: that freed the
    // sink under the parked statement and dropped a committed write's reply.
    Handshake();
    Feed(ClientFrameType::kParse, Parse("s", "SELECT id FROM t"));
    Feed(ClientFrameType::kBind, Bind("p", "s"));

    std::vector<std::byte> out;
    FrameAction action = session_->OnFrame(
        wire::DecodedFrame{static_cast<std::uint8_t>(ClientFrameType::kExecute), 0,
                           Execute("p", 0)},
        out);
    ASSERT_TRUE(action.dispatch);
    clock_.Advance(kPortalIdleTimeoutNs);
    session_->ExpireIdlePortals();
    EXPECT_EQ(session_->portal_count(), 1u) << "a running statement's portal is not idle";

    session_->session().set_result_sink(action.sink);
    DispatchOutcome outcome = dispatcher_->Dispatch(action.sql, &session_->session());
    session_->session().set_result_sink(nullptr);
    session_->OnStatementComplete(outcome, out);
    auto frames = Decode(out);
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(frames.back().type, static_cast<std::uint8_t>(ServerFrameType::kComplete))
        << "the statement's reply reaches the client";

    // Its idleness counts from the completion, not from the C_EXECUTE.
    session_->ExpireIdlePortals();
    EXPECT_EQ(session_->portal_count(), 1u);
}

TEST_F(KwpSessionTest, ClosingItsStatementForgetsAnExpiredPortal) {
    // "Bind it again" is advice a client cannot follow once the statement is
    // gone, so the name answers as unknown, as it would had it not expired.
    Handshake();
    Feed(ClientFrameType::kParse, Parse("s", "SELECT id FROM t"));
    Feed(ClientFrameType::kBind, Bind("p", "s"));
    clock_.Advance(kPortalIdleTimeoutNs);
    session_->ExpireIdlePortals();
    Feed(ClientFrameType::kClose, Handle(1, "s"));

    auto frames = Feed(ClientFrameType::kExecute, Execute("p", 0));
    ASSERT_EQ(frames.size(), 1u);
    auto err = wire::DecodeError(frames[0].payload);
    ASSERT_TRUE(err.ok());
    EXPECT_EQ(err.value().category(), wire::ErrorCategory::kProtocol);
    EXPECT_EQ(err.value().detail_code(),
              static_cast<std::uint16_t>(wire::ProtocolDetail::kUnknownPortal));
}

TEST_F(KwpSessionTest, AnExpiredPortalNameIsForgottenOnceItIsBoundOrClosedAgain) {
    // The timeout answer is about a portal the client held; a name bound
    // afresh is a new portal, and a closed one is one the client let go.
    Handshake();
    Feed(ClientFrameType::kParse, Parse("s", "SELECT id FROM t"));
    Feed(ClientFrameType::kBind, Bind("p", "s"));
    Feed(ClientFrameType::kBind, Bind("q", "s"));
    clock_.Advance(kPortalIdleTimeoutNs);
    session_->ExpireIdlePortals();
    ASSERT_EQ(session_->portal_count(), 0u);

    Feed(ClientFrameType::kBind, Bind("p", "s"));
    auto rebound = Feed(ClientFrameType::kExecute, Execute("p", 0));
    ASSERT_FALSE(rebound.empty());
    EXPECT_NE(rebound[0].type, static_cast<std::uint8_t>(ServerFrameType::kError))
        << "a rebound name is a live portal";

    Feed(ClientFrameType::kClose, Handle(2, "q"));
    auto closed = Feed(ClientFrameType::kExecute, Execute("q", 0));
    ASSERT_EQ(closed.size(), 1u);
    auto err = wire::DecodeError(closed[0].payload);
    ASSERT_TRUE(err.ok());
    EXPECT_EQ(err.value().category(), wire::ErrorCategory::kProtocol);
    EXPECT_EQ(err.value().detail_code(),
              static_cast<std::uint16_t>(wire::ProtocolDetail::kUnknownPortal));
}

// ---- P11: transaction and durability frames (§9) ------------------------

TEST_F(KwpSessionTest, TheTransactionFramesOpenCommitAndAbort) {
    Handshake();
    auto begun = Feed(ClientFrameType::kTxnBegin,
                      U8Payload(static_cast<std::uint8_t>(wire::DurabilityLevel::kStrict)));
    ASSERT_EQ(begun.size(), 1u);
    ASSERT_EQ(begun[0].type, static_cast<std::uint8_t>(ServerFrameType::kTxnOk));
    EXPECT_EQ(begun[0].flags, 0) << "strict is not relaxed";
    EXPECT_TRUE(session_->session().in_explicit_txn());
    ASSERT_TRUE(session_->session().txn_durability().has_value());
    EXPECT_EQ(*session_->session().txn_durability(), wal::DurabilityClass::kStrict);

    // `S_READY` mirrors the state (§3).
    auto ready = Feed(ClientFrameType::kSync, {});
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(ready[0].payload[0]),
              static_cast<std::uint8_t>(Session::State::kInTxn));

    auto committed = Feed(ClientFrameType::kTxnCommit, {});
    ASSERT_EQ(committed.size(), 1u);
    EXPECT_EQ(committed[0].type, static_cast<std::uint8_t>(ServerFrameType::kTxnOk));
    EXPECT_FALSE(session_->session().in_explicit_txn());

    Feed(ClientFrameType::kTxnBegin,
         U8Payload(static_cast<std::uint8_t>(wire::DurabilityLevel::kSessionDefault)));
    auto aborted = Feed(ClientFrameType::kTxnAbort, {});
    ASSERT_EQ(aborted.size(), 1u);
    EXPECT_EQ(aborted[0].type, static_cast<std::uint8_t>(ServerFrameType::kTxnOk));
    EXPECT_FALSE(session_->session().in_explicit_txn());
}

TEST_F(KwpSessionTest, TheRelaxedFlagRidesTheClassTheCommitActuallyUsed) {
    Handshake();
    // The fixture's server default *is* relaxed, so a session-default
    // transaction is the case where the flag cannot be read off the byte
    // the client sent - which is exactly why it is not read off it.
    Feed(ClientFrameType::kTxnBegin,
         U8Payload(static_cast<std::uint8_t>(wire::DurabilityLevel::kSessionDefault)));
    auto committed = Feed(ClientFrameType::kTxnCommit, {});
    ASSERT_EQ(committed.size(), 1u);
    EXPECT_EQ(committed[0].flags, 0x1)
        << "D3's ack semantics differ, and an audit log has to be able to tell";

    Feed(ClientFrameType::kTxnBegin,
         U8Payload(static_cast<std::uint8_t>(wire::DurabilityLevel::kGroup)));
    auto grouped = Feed(ClientFrameType::kTxnCommit, {});
    ASSERT_EQ(grouped.size(), 1u);
    EXPECT_EQ(grouped[0].flags, 0) << "the override outranks the server's class";
}

TEST_F(KwpSessionTest, AnUnknownDurabilityByteIsRefused) {
    Handshake();
    auto frames = Feed(ClientFrameType::kTxnBegin, U8Payload(9));
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kError));
    EXPECT_FALSE(session_->session().in_explicit_txn());
}

TEST_F(KwpSessionTest, AFailedTransactionAdmitsOnlyItsWayOutAndSaysSoInReady) {
    Handshake();
    Feed(ClientFrameType::kTxnBegin,
         U8Payload(static_cast<std::uint8_t>(wire::DurabilityLevel::kSessionDefault)));
    auto failed = RunStatement("INSERT INTO no_such_relation VALUES (1)");
    ASSERT_EQ(failed.size(), 1u);
    ASSERT_EQ(failed[0].type, static_cast<std::uint8_t>(ServerFrameType::kError));

    auto ready = Feed(ClientFrameType::kSync, {});
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(ready[0].payload[0]),
              static_cast<std::uint8_t>(Session::State::kFailedTxn));

    auto aborted = Feed(ClientFrameType::kTxnAbort, {});
    ASSERT_EQ(aborted.size(), 1u);
    EXPECT_EQ(aborted[0].type, static_cast<std::uint8_t>(ServerFrameType::kTxnOk));
    EXPECT_FALSE(session_->session().in_explicit_txn());
}

// ---- P14's flag, observed where §10 says it is --------------------------

TEST_F(KwpSessionTest, ACancelIsObservedAtTheNextFrame) {
    Handshake();
    session_->RequestCancel();
    auto frames = Feed(ClientFrameType::kPing, {});
    ASSERT_EQ(frames.size(), 1u);
    auto err = wire::DecodeError(frames[0].payload);
    ASSERT_TRUE(err.ok());
    EXPECT_EQ(err.value().category(), wire::ErrorCategory::kCancelled);

    // One cancel cancels one statement: the session recovers at its sync.
    ASSERT_EQ(Feed(ClientFrameType::kSync, {}).size(), 1u);
    EXPECT_EQ(RunStatement("SELECT id FROM t").size(), 3u);
}

TEST_F(KwpSessionTest, ACancelMeetingACSyncRefusesItAndStillAnswersTheBarrier) {
    // A client that pipelined `EXECUTE, SYNC` has its cancel taken at the
    // `C_SYNC`: the refusal is sent, and so is the barrier's `S_READY` -
    // §3 item 4's "after every C_SYNC" - with nothing left skipped.
    Handshake();
    session_->RequestCancel();
    const auto frames = Feed(ClientFrameType::kSync, {});
    ASSERT_EQ(frames.size(), 2u);
    ASSERT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kError));
    auto err = wire::DecodeError(frames[0].payload);
    ASSERT_TRUE(err.ok());
    EXPECT_EQ(err.value().category(), wire::ErrorCategory::kCancelled);
    EXPECT_EQ(frames[1].type, static_cast<std::uint8_t>(ServerFrameType::kReady));

    // Not skipping: the next frame is answered, with no second sync.
    const auto pong = Feed(ClientFrameType::kPing, {});
    ASSERT_EQ(pong.size(), 1u);
    EXPECT_EQ(pong[0].type, static_cast<std::uint8_t>(ServerFrameType::kPong));
    EXPECT_EQ(RunStatement("SELECT id FROM t").size(), 3u);
}

// ---- BA-S15: the cancel connection --------------------------------------

TEST_F(KwpSessionTest, ACancelConnectionSetsTheNamedSessionsFlagSilentlyAndCloses) {
    Handshake();
    CancelRegistry registry;
    registry.Register(0x1111, 0x2222, session_->session().cancel_flag());

    // A second connection whose first frame is `C_CANCEL`.
    Session cancel_engine;
    wire::HandshakeConfig config;
    config.capabilities = wire::kServerCapabilities;
    KwpSession canceller(cancel_engine, config);
    canceller.set_cancel_registry(&registry);
    const auto send = [&](std::uint64_t key) {
        std::vector<std::byte> out;
        const FrameAction action = canceller.OnFrame(
            wire::DecodedFrame{static_cast<std::uint8_t>(ClientFrameType::kCancel), 0,
                               wire::EncodeCancelRequest({0x1111, key})},
            out);
        EXPECT_TRUE(action.close);
        EXPECT_TRUE(out.empty()) << "a cancel is answered with nothing, hit or miss";
    };
    send(0x2223);
    EXPECT_FALSE(session_->session().cancel_flag()->load()) << "a wrong key is ignored";
    send(0x2222);
    EXPECT_TRUE(session_->session().cancel_flag()->load());

    // Observed at the target's next frame, as any cancel between statements.
    auto frames = Feed(ClientFrameType::kPing, {});
    ASSERT_EQ(frames.size(), 1u);
    auto err = wire::DecodeError(frames[0].payload);
    ASSERT_TRUE(err.ok());
    EXPECT_EQ(err.value().category(), wire::ErrorCategory::kCancelled);
}

TEST_F(KwpSessionTest, ACancelFrameWithNoRegistryIsAnUnexpectedFirstFrame) {
    std::vector<std::byte> out;
    const FrameAction action = session_->OnFrame(
        wire::DecodedFrame{static_cast<std::uint8_t>(ClientFrameType::kCancel), 0,
                           wire::EncodeCancelRequest({0x1111, 0x2222})},
        out);
    EXPECT_TRUE(action.close);
    auto frames = Decode(out);
    ASSERT_EQ(frames.size(), 1u);
    auto err = wire::DecodeError(frames[0].payload);
    ASSERT_TRUE(err.ok());
    EXPECT_EQ(err.value().category(), wire::ErrorCategory::kProtocol);
}

TEST_F(KwpSessionTest, TerminateClosesInEveryPhase) {
    EXPECT_TRUE(Feed(ClientFrameType::kTerminate, {}).empty());
    EXPECT_TRUE(closed_);
}

// ---- DJ-S2: a session handed back clean -----------------------------------
//
// `docs/spec/protocol.md` §3, §10. `RESET SESSION` returns a session to a
// new connection's settings, and `S_READY` says - to a client that offered
// `SESSION_DEFAULTS` - whether it stands at them.

constexpr std::uint64_t kOffersSessionDefaults =
    static_cast<std::uint64_t>(wire::Capability::kSessionDefaults);

// A `C_HELLO` offering `capabilities`.
std::vector<std::byte> HelloOffering(std::uint64_t capabilities) {
    wire::ClientHello h;
    h.client_name = "test";
    h.capabilities = capabilities;
    return wire::EncodeClientHello(h);
}

// Whether `frames` ends in an `S_READY` carrying the session-default flag.
::testing::AssertionResult SaysDefault(const std::vector<wire::DecodedFrame>& frames) {
    if (frames.empty() ||
        frames.back().type != static_cast<std::uint8_t>(ServerFrameType::kReady)) {
        return ::testing::AssertionFailure() << "no S_READY";
    }
    if ((frames.back().flags & wire::kReadySessionDefaults) == 0) {
        return ::testing::AssertionFailure() << "S_READY without the session-default flag";
    }
    return ::testing::AssertionSuccess();
}

TEST_F(KwpSessionTest, ReadySaysTheSessionIsAtItsDefaultsOnlyToAClientThatOfferedTheBit) {
    // A flag on an existing frame, so a client that did not ask for it is
    // sent the frame it always was.
    auto plain = Feed(ClientFrameType::kHello, Hello());
    ASSERT_EQ(plain.size(), 2u);
    EXPECT_FALSE(SaysDefault(plain));
    EXPECT_EQ(plain.back().flags, 0u);
    EXPECT_FALSE(SaysDefault(Feed(ClientFrameType::kSync, {})));
    EXPECT_TRUE(session_->AtDefaults()) << "the session is at its defaults all the same";
}

TEST_F(KwpSessionTest, EverySettingLeftBehindClearsTheFlagAndResetSessionRestoresIt) {
    // DJ-R8 `[quiet-wrong]`: no session reads as clean with one item left.
    // Each item a client can leave on a connection, one at a time: the flag
    // goes, `RESET SESSION` brings it back, and the item is at its default.
    ASSERT_EQ(Run("CREATE NAMESPACE app").rfind("ERR", 0), std::string::npos);
    auto hello = Feed(ClientFrameType::kHello, HelloOffering(kOffersSessionDefaults));
    ASSERT_TRUE(SaysDefault(hello));
    auto decoded = wire::DecodeServerHello(hello[0].payload);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().capabilities, kOffersSessionDefaults)
        << "S_HELLO carries the intersection";

    const auto sync = [&] { return Feed(ClientFrameType::kSync, {}); };
    // One statement through the unnamed statement and the unnamed portal,
    // both closed behind it - so what a cell leaves is the setting alone,
    // and the flag's answer is about the setting.
    const auto exec = [&](const std::string& sql) {
        (void)Feed(ClientFrameType::kParse, Parse("", sql));
        (void)Feed(ClientFrameType::kBind, Bind("", ""));
        auto frames = Feed(ClientFrameType::kExecute, Execute("", 0));
        (void)Feed(ClientFrameType::kClose, Handle(2, ""));
        (void)Feed(ClientFrameType::kClose, Handle(1, ""));
        return frames;
    };
    struct Item {
        const char* what;
        std::function<void()> leave;
        std::function<bool()> at_default;
    };
    Session& engine = session_->session();
    const Item items[] = {
        {"the isolation level", [&] { (void)exec("SET ISOLATION LEVEL repeatable read"); },
         [&] { return engine.isolation() == txn::IsolationLevel::kReadCommitted; }},
        {"the namespace", [&] { (void)exec("USE app"); },
         [&] { return engine.default_namespace() == catalog::kNamespacePublic; }},
        {"the durability rung", [&] { (void)exec("SET DURABILITY strict"); },
         [&] { return !engine.durability().has_value(); }},
        {"the priority level", [&] { (void)exec("SET PRIORITY analytic"); },
         [&] { return engine.priority() == Priority::kNormal; }},
        {"a named statement",
         [&] { (void)Feed(ClientFrameType::kParse, Parse("kept", "SELECT id FROM t")); },
         [&] { return session_->statement_count() == 0; }},
        {"a named portal",
         [&] {
             (void)Feed(ClientFrameType::kParse, Parse("", "SELECT id FROM t"));
             (void)Feed(ClientFrameType::kBind, Bind("held", ""));
         },
         [&] { return session_->portal_count() == 0; }},
    };
    for (const Item& item : items) {
        item.leave();
        EXPECT_FALSE(item.at_default()) << item.what << " was not left behind";
        EXPECT_FALSE(SaysDefault(sync())) << item.what << " left behind, and S_READY says clean";

        const auto reset = exec("RESET SESSION");
        ASSERT_EQ(Only(reset, ServerFrameType::kComplete).size(), 1u) << item.what;
        EXPECT_TRUE(item.at_default()) << item.what << " survived RESET SESSION";
        EXPECT_TRUE(SaysDefault(sync())) << item.what;
    }
    // **The empty name is a name**: a suspended unnamed portal would hand
    // its rows to the next borrower's `C_CONTINUE ""`, and the unnamed
    // statement its text to a `C_BIND`. Neither reads as clean until it is
    // closed.
    (void)Feed(ClientFrameType::kParse, Parse("", "SELECT id FROM t"));
    (void)Feed(ClientFrameType::kBind, Bind("", ""));
    const auto first = Feed(ClientFrameType::kExecute, Execute("", 1));
    ASSERT_EQ(Only(first, ServerFrameType::kPortalSuspended).size(), 1u);
    EXPECT_FALSE(SaysDefault(sync())) << "a suspended unnamed portal, and S_READY says clean";
    (void)Feed(ClientFrameType::kClose, Handle(2, ""));
    EXPECT_FALSE(SaysDefault(sync())) << "the unnamed statement, and S_READY says clean";
    (void)Feed(ClientFrameType::kClose, Handle(1, ""));
    EXPECT_TRUE(SaysDefault(sync()));
}

TEST_F(KwpSessionTest, ResetSessionReturnsTheIsolationLevelTheSessionBeganAt) {
    // The server's `isolation`, as the connection was stamped with it - not
    // the engine's compiled-in level. **The mutation**: reset to READ
    // COMMITTED, and a server configured for REPEATABLE READ hands a
    // borrower a level its operator did not set.
    Session started_repeatable(txn::IsolationLevel::kRepeatableRead);
    EXPECT_TRUE(started_repeatable.AtDefaults());
    ASSERT_EQ(dispatcher_->Dispatch("SET ISOLATION LEVEL read committed", &started_repeatable)
                  .response.rfind("ERR", 0),
              std::string::npos);
    EXPECT_FALSE(started_repeatable.AtDefaults());
    EXPECT_EQ(dispatcher_->Dispatch("RESET SESSION", &started_repeatable).response,
              "RESET SESSION");
    EXPECT_EQ(started_repeatable.isolation(), txn::IsolationLevel::kRepeatableRead);
    EXPECT_TRUE(started_repeatable.AtDefaults());
}

TEST_F(KwpSessionTest, ResetSessionGivesItsBatchSeatBack) {
    // `SET PRIORITY analytic` seats the session in the instance's batch
    // count, which is what splits the cores (`priority.hpp`); a reset that
    // left the seat would keep the partition up for a session that is
    // `normal` again.
    Session& engine = session_->session();
    const std::uint32_t before = sched::CoreLoad::BatchSessions().load();
    ASSERT_EQ(Run("SET PRIORITY analytic").rfind("ERR", 0), std::string::npos);
    EXPECT_EQ(sched::CoreLoad::BatchSessions().load(), before + 1);
    ASSERT_EQ(Run("RESET SESSION"), "RESET SESSION");
    EXPECT_EQ(sched::CoreLoad::BatchSessions().load(), before);
    EXPECT_EQ(engine.priority(), Priority::kNormal);
}

TEST_F(KwpSessionTest, ResetSessionInsideAnyKindOfTransactionLeavesItAsItWas) {
    // The three kinds, each answered by its own gate and none changed: an
    // open `READ ONLY` one by the reset's own refusal (not the read-only
    // gate's, which would tell the client to go and write), and a failed
    // one by the failed-transaction gate.
    Session& engine = session_->session();
    ASSERT_EQ(Run("SET DURABILITY strict").rfind("ERR", 0), std::string::npos);

    ASSERT_EQ(Run("BEGIN READ ONLY").rfind("ERR", 0), std::string::npos);
    const std::string in_read_only = Run("RESET SESSION");
    EXPECT_NE(in_read_only.find("inside a transaction"), std::string::npos) << in_read_only;
    EXPECT_TRUE(engine.in_read_only_txn()) << "the refusal ended the transaction";
    EXPECT_FALSE(engine.failed());
    EXPECT_TRUE(engine.durability().has_value());
    ASSERT_EQ(Run("COMMIT").rfind("ERR", 0), std::string::npos);

    ASSERT_EQ(Run("BEGIN").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Run("INSERT INTO t VALUES (777, 1, 'a')").rfind("ERR", 0), std::string::npos);
    // The same key again: a write that fails inside a transaction fails it.
    ASSERT_EQ(Run("INSERT INTO t VALUES (777, 1, 'a')").rfind("ERR", 0), 0u);
    ASSERT_TRUE(engine.failed());
    const std::string in_failed = Run("RESET SESSION");
    EXPECT_EQ(in_failed.rfind("ERR", 0), 0u) << in_failed;
    EXPECT_TRUE(engine.failed());
    EXPECT_TRUE(engine.durability().has_value());
    ASSERT_EQ(Run("ROLLBACK").rfind("ERR", 0), std::string::npos);

    EXPECT_EQ(Run("RESET SESSION"), "RESET SESSION");
    EXPECT_FALSE(engine.durability().has_value());
}

TEST_F(KwpSessionTest, ResetSessionIsRefusedInsideATransactionAndChangesNothing) {
    (void)Feed(ClientFrameType::kHello, HelloOffering(kOffersSessionDefaults));
    Session& engine = session_->session();
    ASSERT_EQ(Run("SET PRIORITY analytic").rfind("ERR", 0), std::string::npos);
    (void)Feed(ClientFrameType::kParse, Parse("kept", "SELECT id FROM t"));
    ASSERT_EQ(Feed(ClientFrameType::kTxnBegin, U8Payload(0)).size(), 1u);

    const auto refused = RunStatement("RESET SESSION");
    ASSERT_EQ(Only(refused, ServerFrameType::kError).size(), 1u);
    auto err = wire::DecodeError(refused[0].payload);
    ASSERT_TRUE(err.ok());
    EXPECT_NE(err.value().message.find("inside a transaction"), std::string::npos)
        << err.value().message;
    EXPECT_EQ(engine.priority(), Priority::kAnalytic) << "a refused reset changed a setting";
    EXPECT_EQ(session_->statement_count(), 2u) << "a refused reset dropped a statement";
    // An open transaction is not a clean session, whatever its settings -
    // and it is still open: the refusal did not fail it.
    const auto ready = Feed(ClientFrameType::kSync, {});
    EXPECT_FALSE(SaysDefault(ready));
    ASSERT_EQ(ready.back().payload.size(), 1u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(ready.back().payload[0]),
              static_cast<std::uint8_t>(Session::State::kInTxn));

    // Out of the transaction it is taken.
    (void)Feed(ClientFrameType::kTxnAbort, {});
    ASSERT_EQ(Only(RunStatement("RESET SESSION"), ServerFrameType::kComplete).size(), 1u);
    EXPECT_EQ(engine.priority(), Priority::kNormal);
    EXPECT_EQ(session_->statement_count(), 0u);
    EXPECT_TRUE(SaysDefault(Feed(ClientFrameType::kSync, {})));
}

TEST_F(KwpSessionTest, ResetTakesSessionAndNothingElse) {
    // One spelling, so nothing is read as a narrower or a wider reset than
    // was written; each refusal carries the byte of what it refuses.
    Session& engine = session_->session();
    ASSERT_EQ(Run("SET DURABILITY strict").rfind("ERR", 0), std::string::npos);
    const std::pair<const char*, const char*> refused[] = {
        {"RESET", "(byte 5)"},
        {"RESET ALL", "(byte 6)"},
        {"RESET durability", "(byte 6)"},
        {"RESET SESSION now", "(byte 14)"},
        {"RESET ;", "(byte 6)"},
    };
    for (const auto& [sql, at] : refused) {
        const std::string reply = Run(sql);
        EXPECT_EQ(reply.rfind("ERR", 0), 0u) << sql << ": " << reply;
        EXPECT_NE(reply.find(at), std::string::npos) << sql << ": " << reply;
        EXPECT_TRUE(engine.durability().has_value()) << sql << " reset something";
    }
    for (const char* sql : {"RESET SESSION", "reset session;", "Reset Session ;"}) {
        ASSERT_EQ(Run("SET DURABILITY strict").rfind("ERR", 0), std::string::npos);
        EXPECT_EQ(Run(sql), "RESET SESSION") << sql;
        EXPECT_FALSE(engine.durability().has_value()) << sql;
    }
    // A session's own state: the lowest role may reset it.
    engine.set_role(Role::kReadOnly);
    EXPECT_EQ(Run("RESET SESSION"), "RESET SESSION");
}

}  // namespace
}  // namespace kds::server
