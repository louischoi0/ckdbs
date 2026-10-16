#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/kwp_session.hpp"
#include "kds/stats/cabin_optimizer.hpp"
#include "kds/stats/statement_stats.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wire/handshake.hpp"
#include "kds/wire/kwp.hpp"
#include "kds/wire/kwp_types.hpp"
#include "kds/wire/row_codec.hpp"
#include "view_text.hpp"

// **The `show_sugar` contract suite** (`instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md`
// BO-R2 SV1-SV4, BO-R3 SV5).
//
// Two claims, pinned against the real dispatcher and the real KWP session:
//
//   SV5  a `sys` view answers as an ordinary table does - a typed
//        description and typed rows on KWP, the header-and-rows text on
//        the newline protocol - never one `TEXT` column named `line`;
//   SV2  every `SHOW` spelling answers exactly what the `SELECT` it
//        stands for answers, through both protocols.
//
// The SV2 table is the contract: a `SHOW` target is added by a row here,
// a view and nothing else (SV4).

namespace kds::server {
namespace {

using wire::ClientFrameType;
using wire::ServerFrameType;

// One statement's answer over KWP, kept as the frames' bytes: two answers
// are the same result when these compare equal.
struct KwpAnswer {
    std::vector<std::uint8_t> types;
    std::vector<std::vector<std::byte>> payloads;
    bool operator==(const KwpAnswer&) const = default;
};

class ShowSugarTest : public ::testing::Test {
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
                            /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);
        dispatcher_->set_cabin_optimizer_view(&cabin_controller_, /*executor=*/nullptr);

        wire::HandshakeConfig config;
        config.capabilities = wire::kServerCapabilities;
        config.server_info = "kds-test";
        engine_session_.emplace(txn::IsolationLevel::kReadCommitted);
        kwp_.emplace(*engine_session_, config, wal::DurabilityClass::kRelaxed, &clock_);
        kwp_->set_identity(0x1111, 0x2222);
        std::vector<std::byte> out;
        wire::ClientHello hello;
        hello.client_name = "test";
        (void)kwp_->OnFrame(wire::DecodedFrame{static_cast<std::uint8_t>(ClientFrameType::kHello),
                                               0, wire::EncodeClientHello(hello)},
                            out);

        ASSERT_EQ(Text("CREATE TABLE t (id int64, v int32, name varchar)").substr(0, 7),
                  "CREATED");
        ASSERT_EQ(Text("INSERT INTO t VALUES (10, 'alpha')").substr(0, 8), "INSERTED");
        ASSERT_EQ(Text("CREATE INDEX t_v ON t (v)").substr(0, 7), "CREATED");
        ASSERT_EQ(Text("SELECT v FROM t WHERE id = 1").rfind("ERR", 0), std::string::npos);
    }

    // The newline protocol's reply.
    std::string Text(const std::string& sql) {
        return dispatcher_->Dispatch(sql, &kwp_->session()).response;
    }
    std::string Text(Session& s, const std::string& sql) {
        return dispatcher_->Dispatch(sql, &s).response;
    }

    // One statement through PARSE/BIND/EXECUTE, the frames EXECUTE produced.
    std::vector<wire::DecodedFrame> Kwp(const std::string& sql) {
        (void)Feed(ClientFrameType::kParse, [&] {
            wire::PayloadWriter w;
            w.Str("s");
            w.Text(sql);
            return w.Take();
        }());
        (void)Feed(ClientFrameType::kBind, [&] {
            wire::PayloadWriter w;
            w.Str("p");
            w.Str("s");
            std::vector<std::byte> out = w.Take();
            EXPECT_TRUE(wire::EncodeBindParams({}, out).ok());
            return out;
        }());
        return Feed(ClientFrameType::kExecute, [&] {
            wire::PayloadWriter w;
            w.Str("p");
            w.U32(0);
            return w.Take();
        }());
    }

    KwpAnswer Answer(const std::string& sql) {
        KwpAnswer a;
        for (auto& f : Kwp(sql)) {
            a.types.push_back(f.type);
            a.payloads.push_back(std::move(f.payload));
        }
        return a;
    }

    std::vector<wire::DecodedFrame> Feed(ClientFrameType type, std::vector<std::byte> payload) {
        std::vector<std::byte> out;
        FrameAction action = kwp_->OnFrame(
            wire::DecodedFrame{static_cast<std::uint8_t>(type), 0, std::move(payload)}, out);
        if (action.dispatch) {
            kwp_->session().set_result_sink(action.sink);
            DispatchOutcome outcome = dispatcher_->Dispatch(action.sql, &kwp_->session());
            kwp_->session().set_result_sink(nullptr);
            kwp_->OnStatementComplete(outcome, out);
        }
        wire::FrameDecoder decoder;
        EXPECT_TRUE(decoder.Feed(out).ok());
        std::vector<wire::DecodedFrame> frames;
        while (auto frame = decoder.PopFrame()) frames.push_back(std::move(*frame));
        return frames;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    // A controller with nothing managed: `sys.cabin_optimizer` refuses on an
    // instance that runs none, and the SV2 cell needs one that answers.
    stats::CabinOptimizer cabin_controller_{stats::CabinOptimizerConfig{}};
    std::optional<CommandDispatcher> dispatcher_;
    sched::ManualClock clock_{1000};
    std::optional<Session> engine_session_;
    std::optional<KwpSession> kwp_;
};

// ---- SV5: a view is an ordinary table ------------------------------------

TEST_F(ShowSugarTest, AViewAnswersKwpWithATypedDescriptionAndTypedRows) {
    auto frames = Kwp("SELECT oid, name FROM sys.tables WHERE name = 't'");
    ASSERT_EQ(frames.size(), 3u);
    ASSERT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kRowDesc));
    auto desc = wire::DecodeRowDescription(frames[0].payload);
    ASSERT_TRUE(desc.ok()) << desc.status().message();
    ASSERT_EQ(desc.value().size(), 2u) << "one field per projected column, not one `line`";
    EXPECT_EQ(desc.value()[0].name, "oid");
    EXPECT_EQ(desc.value()[0].type_oid, catalog::kTypeValUint64);
    EXPECT_EQ(desc.value()[0].flags & wire::kFieldFlagKeystone, 0)
        << "a view has no Keystone column";
    EXPECT_EQ(desc.value()[1].name, "name");
    EXPECT_EQ(desc.value()[1].type_oid, catalog::kTypeValVarchar);

    ASSERT_EQ(frames[1].type, static_cast<std::uint8_t>(ServerFrameType::kRowBatch));
    auto rows = wire::DecodeRowBatch(frames[1].payload, 2);
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    ASSERT_EQ(rows.value().size(), 1u);
    ASSERT_EQ(rows.value()[0][0].bytes.size(), 8u) << "a uint64, in its eight bytes";
    std::uint64_t oid = 0;
    for (std::size_t b = 0; b < 8; ++b) {
        oid |= std::uint64_t{std::to_integer<std::uint8_t>(rows.value()[0][0].bytes[b])} << (8 * b);
    }
    EXPECT_EQ("oid\\n" + std::to_string(oid), Text("SELECT oid FROM sys.tables WHERE name = 't'"))
        << "the value the text protocol prints";
    const auto& name = rows.value()[0][1].bytes;
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(name.data()), name.size()), "t");

    wire::PayloadReader r(frames[2].payload);
    auto tag = r.Text();
    ASSERT_TRUE(tag.has_value() && tag->has_value());
    EXPECT_EQ(**tag, "SELECT");
    EXPECT_EQ(*r.U64(), 1u);
}

TEST_F(ShowSugarTest, AnEmptyViewAnswerIsADescriptionAndACompletionOfZero) {
    auto frames = Kwp("SELECT * FROM sys.tables WHERE name = 'nothing'");
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0].type, static_cast<std::uint8_t>(ServerFrameType::kRowDesc));
    EXPECT_EQ(frames[1].type, static_cast<std::uint8_t>(ServerFrameType::kComplete));
}

TEST_F(ShowSugarTest, TheNewlineProtocolPrintsAViewAsItPrintsARelation) {
    EXPECT_EQ(Text("SELECT name, clustered_type FROM sys.tables WHERE name = 't'"),
              "name,clustered_type\\nt,BTREE");
    EXPECT_EQ(Text("SELECT name FROM t"), "name\\nalpha");
}

TEST_F(ShowSugarTest, AViewIsOrderedByItsDeclaredTypes) {
    ASSERT_EQ(Text("CREATE TABLE zb (id int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Text("CREATE TABLE za (id int64)").substr(0, 7), "CREATED");
    // `public` alone (namespace 1): the catalog's own relations are rows too.
    const std::string user = "SELECT name FROM sys.tables WHERE namespace_oid = 1 ";
    EXPECT_EQ(Text(user + "ORDER BY name"), "name\\nt\\nza\\nzb");
    EXPECT_EQ(Text(user + "ORDER BY name DESC LIMIT 2"), "name\\nzb\\nza");
    // By a uint64: the oids ascend in creation order, which is not the names'.
    EXPECT_EQ(Text(user + "ORDER BY oid"), "name\\nt\\nzb\\nza");
    EXPECT_EQ(Text(user + "ORDER BY oid DESC"), "name\\nza\\nzb\\nt");
    // Two keys, ties on the first broken by the second. User relations
    // only (oids from 4000): a catalog relation with column rows -
    // `sys.transitions` - would sort among them.
    EXPECT_EQ(Text("SELECT rel_name, pos FROM sys.columns WHERE rel_id >= 4000 "
                   "ORDER BY rel_name DESC, pos DESC"),
              "rel_name,pos\\nzb,0\\nza,0\\nt,2\\nt,1\\nt,0");
    // An unknown key is `InvalidArgument` (no code token) at its byte.
    const std::string sql = "SELECT name FROM sys.tables ORDER BY nope";
    EXPECT_EQ(Text(sql), "ERR view sys.tables has no column 'nope' (byte " +
                             std::to_string(sql.find("nope")) + ")");
}

TEST_F(ShowSugarTest, ASchemaObjectViewReadsUnderTheSessionsView) {
    // BO-Q2 (a) `[quiet-wrong]`: `SHOW TABLES` filters (DT4), so its
    // `SELECT` must, or the two spellings disagree on what exists.
    Session other(txn::IsolationLevel::kReadCommitted);
    ASSERT_EQ(Text(other, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Text(other, "CREATE TABLE hidden (id int64, v int64)").substr(0, 7), "CREATED");
    for (const char* sql : {"SELECT name FROM sys.tables", "SELECT name FROM sys.objects",
                            "SELECT rel_name FROM sys.columns"}) {
        EXPECT_EQ(Text(sql).find("hidden"), std::string::npos) << "leaked through: " << sql;
    }
    EXPECT_NE(Text(other, "SELECT name FROM sys.tables").find("hidden"), std::string::npos)
        << "the creator sees its own relation";
    EXPECT_NE(Text(other, "SELECT rel_name FROM sys.columns").find("hidden"), std::string::npos);
    ASSERT_EQ(Text(other, "ROLLBACK").substr(0, 8), "ROLLBACK");
    // DT3c's hole: a read taken while the DDL was open must not leave the
    // rolled-back relation in the catalog's cache.
    EXPECT_EQ(Text("SELECT name FROM sys.tables").find("hidden"), std::string::npos);
}

// ---- SV2: every `SHOW` is its `SELECT` -----------------------------------

struct Spelling {
    const char* show;
    const char* select;
};

// BO-S2 turns these green one at a time, deleting each handler as it does.
constexpr Spelling kSpellings[] = {
    {"SHOW TABLES", "SELECT * FROM sys.tables"},
    {"SHOW NAMESPACES", "SELECT * FROM sys.namespaces"},
    {"SHOW PATTERNS", "SELECT * FROM sys.patterns"},
    {"SHOW ACCESS", "SELECT * FROM sys.access_stats"},
    {"SHOW BUDGET",
     "SELECT oid, namespace, name, next_id, ids_issued, ids_remaining, budget_used_ppm, "
     "budget_warning, "
     "budget_exhausted FROM sys.relation_stats"},
    {"SHOW CABINS", "SELECT * FROM sys.cabins"},
    {"SHOW INDEXES", "SELECT * FROM sys.indexes"},
    {"SHOW FKEYS", "SELECT * FROM sys.fkeys"},
    {"SHOW ASSERTIONS", "SELECT * FROM sys.assertions"},
    {"SHOW TRANSITIONS", "SELECT * FROM sys.transitions"},
    {"SHOW RELAYOUT", "SELECT * FROM sys.relayout"},
    {"SHOW RELAYOUT t", "SELECT * FROM sys.relayout WHERE name = 't'"},
    {"SHOW CABIN_OPTIMIZER", "SELECT * FROM sys.cabin_optimizer"},
    {"SHOW PAGE 4",
     "SELECT page_id, page_type, page_lsn, level, min_key, nr_slots, lower, upper, free_space, "
     "next_page_id, slot, offset, length, dead, sep_key, child FROM sys.page_slots "
     "WHERE page_id = 4"},
    {"SHOW PAGE 4 VALUES", "SELECT * FROM sys.page_slots WHERE page_id = 4"},
    {"DESCRIBE t", "SELECT * FROM sys.columns WHERE rel_name = 't'"},
};

class ShowSugarSpellingTest : public ShowSugarTest,
                              public ::testing::WithParamInterface<Spelling> {};

TEST_P(ShowSugarSpellingTest, TheShowAnswersWhatItsSelectAnswers) {
    const Spelling& s = GetParam();
    const std::string selected = Text(s.select);
    ASSERT_EQ(selected.rfind("ERR", 0), std::string::npos) << s.select << ": " << selected;
    EXPECT_EQ(Text(s.show), selected) << s.show;
    const KwpAnswer shown = Answer(s.show);
    EXPECT_EQ(shown, Answer(s.select)) << s.show;
    ASSERT_FALSE(shown.types.empty()) << s.show;
    EXPECT_EQ(shown.types.front(), static_cast<std::uint8_t>(ServerFrameType::kRowDesc))
        << s.show << " answers a result set";
}

INSTANTIATE_TEST_SUITE_P(Targets, ShowSugarSpellingTest, ::testing::ValuesIn(kSpellings),
                         [](const ::testing::TestParamInfo<Spelling>& info) {
                             std::string name;
                             for (const char* c = info.param.show; *c != '\0'; ++c) {
                                 if (std::isalnum(static_cast<unsigned char>(*c)) != 0) name += *c;
                             }
                             return name;
                         });

TEST_F(ShowSugarTest, ShowMetaIsTheMetaViewsFieldsWhoseValuesMove) {
    // `sys.meta`'s values are live counters, so two statements in a row
    // read different numbers; the contract is the shape and the fields.
    const auto show = Kwp("SHOW META");
    const auto select = Kwp("SELECT * FROM sys.meta");
    ASSERT_GE(select.size(), 3u);
    ASSERT_EQ(show.size(), select.size());
    EXPECT_EQ(show[0].payload, select[0].payload) << "the same description";
    auto desc = wire::DecodeRowDescription(select[0].payload);
    ASSERT_TRUE(desc.ok());
    ASSERT_EQ(desc.value().size(), 3u);
    EXPECT_EQ(desc.value()[0].name, "section");
    EXPECT_EQ(desc.value()[1].name, "field");
    EXPECT_EQ(desc.value()[2].name, "value");
    const auto fields = [](const std::vector<wire::DecodedFrame>& frames) {
        std::vector<std::vector<std::byte>> out;
        for (std::size_t i = 1; i + 1 < frames.size(); ++i) {
            auto rows = wire::DecodeRowBatch(frames[i].payload, 3);
            EXPECT_TRUE(rows.ok());
            for (auto& row : rows.value()) {
                out.emplace_back(row[0].bytes.begin(), row[0].bytes.end());
                out.emplace_back(row[1].bytes.begin(), row[1].bytes.end());
            }
        }
        return out;
    };
    EXPECT_EQ(fields(show), fields(select));
    // A field is filed under the block that prints it.
    EXPECT_EQ(Text("SELECT section, field FROM sys.meta WHERE field = 'version'"),
              "section,field\\nvolume,version");
    EXPECT_EQ(Text("SELECT section FROM sys.meta WHERE field = 'pool_budget'"),
              "section\\npool");
}

TEST_F(ShowSugarTest, ASurfaceTheInstanceDoesNotRunIsRefusedTheSameByBothSpellings) {
    // SV2's refusal half: with no controller the view reports the surface
    // absent rather than an empty table, and the `SHOW` says it in the same
    // words.
    // Each names the byte of the view in the statement it was sent: the
    // `SHOW`'s target word, the `SELECT`'s relation.
    dispatcher_->set_cabin_optimizer_view(nullptr, nullptr);
    const std::string refusal =
        "ERR sys.cabin_optimizer has nothing to report: this instance runs no cabin optimizer";
    EXPECT_EQ(Text("SELECT * FROM sys.cabin_optimizer"), refusal + " (byte 14)");
    EXPECT_EQ(Text("SHOW CABIN_OPTIMIZER"), refusal + " (byte 5)");
}

TEST_F(ShowSugarTest, AQualifiedNameDescribesTheRelationItNames) {
    // The key's literal is `ns.t` and the rows' `rel_name` is `t`: the
    // builder resolves the qualified name, and the conjunct must not then
    // filter every row it read away - a relation answered with no columns.
    ASSERT_EQ(Text("CREATE NAMESPACE ns").substr(0, 7), "CREATED");
    ASSERT_EQ(Text("CREATE TABLE ns.q (id int64, w int32)").substr(0, 7), "CREATED");
    const std::string described = Text("DESCRIBE ns.q");
    ASSERT_EQ(Text("USE ns"), "USE namespace=ns");  // a bare `q` is `ns.q` (NS5)
    EXPECT_EQ(described, Text("DESCRIBE q"));
    EXPECT_EQ(described, Text("SELECT * FROM sys.columns WHERE rel_name = 'ns.q'"));
    EXPECT_NE(described.find("\\n"), std::string::npos) << "no rows: " << described;
    const std::string relayout = Text("SHOW RELAYOUT ns.q");
    EXPECT_EQ(relayout, Text("SHOW RELAYOUT q"));
    EXPECT_NE(relayout.find("\\n"), std::string::npos) << "no rows: " << relayout;
    // A wrong qualifier is still refused, not answered with no rows.
    EXPECT_EQ(Text("DESCRIBE public.q").rfind("ERR", 0), 0u);
    EXPECT_EQ(Text("SHOW RELAYOUT public.q").rfind("ERR", 0), 0u);
}

TEST_F(ShowSugarTest, ARefusalWithNoArgumentNamesTheStatementsEnd) {
    // `DESCRIBE` and a bare `SHOW` have no token to point at; the byte is
    // the end of the statement, never an address outside it.
    EXPECT_NE(Text("DESCRIBE").find("(byte 8)"), std::string::npos) << Text("DESCRIBE");
    EXPECT_NE(Text("DESC").find("(byte 4)"), std::string::npos) << Text("DESC");
    EXPECT_NE(Text("SHOW").find("(byte 4)"), std::string::npos) << Text("SHOW");
    EXPECT_NE(Text("DESC a b").find("(byte 7)"), std::string::npos) << Text("DESC a b");
    // An unknown name is refused at the name's byte.
    EXPECT_NE(Text("DESCRIBE nope").find("(byte 9)"), std::string::npos) << Text("DESCRIBE nope");
    // `DESCRIBE` is a statement of its own, not a `SHOW` target.
    EXPECT_EQ(Text("SHOW DESCRIBE t").rfind("ERR", 0), 0u) << Text("SHOW DESCRIBE t");
}

// ---- BO-S5: `sys.statement_stats` ----------------------------------------

TEST_F(ShowSugarTest, OneShapeIsOneRowWhateverItsLiterals) {
    stats::StatementStats::ResetForTest();
    ASSERT_EQ(Text("SELECT v FROM t WHERE id = 1").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Text("SELECT v FROM t WHERE id = 2").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Text("SELECT v FROM t WHERE id = 3").rfind("ERR", 0), std::string::npos);
    // A refusal is a call too: the shape parses, the relation does not exist.
    ASSERT_EQ(Text("SELECT v FROM nope WHERE id = 1").rfind("ERR", 0), 0u);

    const auto fetch = parser::FingerprintOf("SELECT v FROM t WHERE id = 1");
    ASSERT_TRUE(fetch.has_value());
    const std::string reply = Text(
        "SELECT fetch_id, calls, refused, shape_text FROM sys.statement_stats WHERE kind = "
        "'SELECT'");
    const auto shown = testing_view::ReadViewText(reply);
    const std::string id = std::to_string(fetch->fetch_id);
    EXPECT_EQ(shown.Find("fetch_id", id, "calls"), "3") << reply;
    EXPECT_EQ(shown.Find("fetch_id", id, "refused"), "0") << reply;
    EXPECT_EQ(shown.Find("fetch_id", id, "shape_text"), "SELECT v FROM t WHERE id = 1")
        << "the first statement of the shape: " << reply;
    // The refused one is a shape of its own (another relation), refused once.
    const std::vector<std::string> refused = shown.Column("refused");
    EXPECT_NE(std::find(refused.begin(), refused.end(), "1"), refused.end()) << reply;
}

TEST_F(ShowSugarTest, AKindWithNoFingerprintIsOneRowAtFetchIdZero) {
    stats::StatementStats::ResetForTest();
    ASSERT_EQ(Text("DELETE FROM t WHERE id = 1").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Text("DELETE FROM t WHERE id = 99").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Text("FLIBBERTIGIBBET EVERYTHING").rfind("ERR", 0), 0u);
    ASSERT_EQ(Text("INSERT INTO t VALUES (20, 'beta')").rfind("INSERTED", 0), 0u);

    const std::string reply =
        Text("SELECT kind, fetch_id, calls, refused FROM sys.statement_stats ORDER BY kind");
    const auto shown = testing_view::ReadViewText(reply);
    EXPECT_EQ(shown.Column("kind"), (std::vector<std::string>{"DELETE", "INSERT", "OTHER"}))
        << reply;
    EXPECT_EQ(shown.Find("kind", "DELETE", "fetch_id"), "0") << reply;
    EXPECT_EQ(shown.Find("kind", "DELETE", "calls"), "2") << reply;
    EXPECT_EQ(shown.Find("kind", "OTHER", "refused"), "1") << reply;
    // An INSERT carries its fingerprint, taken from the parse it ran.
    EXPECT_EQ(shown.Find("kind", "INSERT", "fetch_id"),
              std::to_string(parser::FingerprintOf("INSERT INTO t VALUES (20, 'beta')")->fetch_id))
        << reply;
    EXPECT_EQ(testing_view::MetaField(Text("SHOW META"), "statement_stats_dropped"), "0");
}

TEST_F(ShowSugarTest, AnUpdateWithASubqueryIsKeyedByItsOwnShape) {
    // The SET value's subquery runs through the SELECT path inside the
    // UPDATE; its fingerprint must not become the UPDATE's key.
    stats::StatementStats::ResetForTest();
    const std::string update = "UPDATE t SET v = (SELECT v FROM t WHERE id = 1) WHERE id = 1";
    ASSERT_EQ(Text(update).rfind("ERR", 0), std::string::npos) << Text(update);

    const std::string reply =
        Text("SELECT kind, fetch_id FROM sys.statement_stats WHERE kind = 'UPDATE'");
    const auto shown = testing_view::ReadViewText(reply);
    EXPECT_EQ(shown.Column("fetch_id"),
              std::vector<std::string>{std::to_string(parser::FingerprintOf(update)->fetch_id)})
        << reply;
}

TEST_F(ShowSugarTest, TheTraceSurfaceIsRetired) {
    // BO-Q9 (a): nothing ever installed a sink, so these always answered ERR.
    for (const char* sql : {"SHOW TRACES", "SHOW TRACE 1", "TRACE ON"}) {
        const std::string reply = Text(sql);
        EXPECT_EQ(reply.rfind("ERR", 0), 0u) << sql;
        EXPECT_EQ(reply.find("trace sink"), std::string::npos)
            << sql << " is refused as unknown, not as a surface with no sink: " << reply;
    }
}

}  // namespace
}  // namespace kds::server
