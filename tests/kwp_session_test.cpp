#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/parser/fingerprint.hpp"
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

TEST_F(KwpSessionTest, AMultiLineDiagnosticArrivesAsAOneColumnTextResultSet) {
    Handshake();
    auto frames = RunStatement("DESCRIBE t");
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
    // lines. `SHOW META` is one line, so it is a tag.
    Handshake();
    auto frames = RunStatement("SHOW META");
    ASSERT_EQ(frames.size(), 1u);
    ASSERT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kComplete));
    wire::PayloadReader r(frames[0].payload);
    auto tag = r.Text();
    ASSERT_TRUE(tag.has_value() && tag->has_value());
    EXPECT_NE((**tag).find("version="), std::string::npos);
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
        // 2026-08-31 rule read correctly: an outer join is a form the
        // design admits and nobody built, so a later release could lift the
        // refusal without changing the architecture. It is also the case
        // that makes this test worth having - the two codes are one word
        // apart in the message and two categories apart on the wire, and a
        // client's feature detection reads only the second.
        {"SELECT t.id FROM t LEFT JOIN t AS u ON t.id = u.id",
         wire::ErrorCategory::kNotImplemented},
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
    Handshake();
    Feed(ClientFrameType::kParse, Parse("s", "SELECT id FROM t WHERE name = ?"));
    const std::string value = "O'Brien";
    wire::BoundParam p;
    p.type_oid = catalog::kTypeValVarchar;
    p.bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()),
                                         value.size());
    auto frames = Feed(ClientFrameType::kBind, Bind("p", "s", {p}));
    ASSERT_EQ(frames.size(), 1u);
    ASSERT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kError));
    auto err = wire::DecodeError(frames[0].payload);
    ASSERT_TRUE(err.ok());
    EXPECT_NE(err.value().message.find("no escape for a quote"), std::string::npos)
        << "the refusal names the grammar's limit, which is whose it is";
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

    auto frames = Feed(ClientFrameType::kExecute, Execute("p", 0));
    ASSERT_EQ(frames.size(), 1u);
    auto err = wire::DecodeError(frames[0].payload);
    ASSERT_TRUE(err.ok());
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

TEST_F(KwpSessionTest, TerminateClosesInEveryPhase) {
    EXPECT_TRUE(Feed(ClientFrameType::kTerminate, {}).empty());
    EXPECT_TRUE(closed_);
}

}  // namespace
}  // namespace kds::server
