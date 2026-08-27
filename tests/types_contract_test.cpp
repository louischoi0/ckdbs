#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/type_literals.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// **The types contract** (docs/spec/types.md §6, workplan TY08).
//
// One test per numbered item, in order, and regression-mandatory from the
// moment this file exists: these are the eight statements the types work
// makes about what the engine does, and each is here so that changing it
// requires deleting a test that says why.
//
// This is not a second copy of the unit tests. `types_predicate_test.cpp`
// checks *how* something is compiled and `types_e2e_test.cpp` checks that
// subsystems needed no teaching; this file checks the **product claims**,
// end to end, at the boundary a client sees. Where the two overlap the
// overlap is deliberate - a contract that only holds because another test
// file also happens to cover it is not pinned.

namespace kds::server {
namespace {

class Instance {
public:
    Instance() {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        EXPECT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    // Re-mounts the same store through a fresh Catalog and dispatcher -
    // the closest thing to a restart an in-memory store has, and enough
    // for item 8: the catalog is re-read from the pages rather than from
    // the cache that answered before.
    void Remount() {
        dispatcher_.reset();
        boot_.reset();
        auto boot = bootstrap::BootstrapDatabase(store_, 2000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
    }

private:
    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- Item 1: round trip -------------------------------------------------

TEST(TypesContract, Item1_EncodeDecodeFormatReproducesTheLiteral) {
    // Through the dispatcher, so the trip is the real one: parse, encode
    // to the page, decode, render. The range **edges** are the point -
    // 1900-01-01 and 2999-12-31 are the declared bounds, and an off-by-one
    // in either direction shows up here and nowhere else.
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE t (id int64, d date, ts timestamp, m decimal(18, 6))")
                  .substr(0, 7),
              "CREATED");

    struct Case {
        const char* d;
        const char* ts;
        const char* m;
    };
    const Case cases[] = {
        {"1900-01-01", "1900-01-01 00:00:00", "-999999999999.999999"},
        {"1969-12-31", "1969-12-31 23:59:59.999999", "-0.000001"},
        {"1970-01-01", "1970-01-01 00:00:00", "0.000000"},
        {"2026-08-07", "2026-08-07 09:15:00.250000", "12.300000"},
        {"2999-12-31", "2999-12-31 23:59:59.999999", "999999999999.999999"},
    };

    for (const Case& c : cases) {
        const std::string insert = std::string("INSERT INTO t VALUES ('") + c.d + "', '" + c.ts +
                                   "', '" + c.m + "')";
        ASSERT_EQ(db.Run(insert).substr(0, 8), "INSERTED") << insert;
    }

    std::string want = "d,ts,m";
    for (const Case& c : cases) {
        want += std::string("\\n") + c.d + ',' + c.ts + ',' + c.m;
    }
    EXPECT_EQ(db.Run("SELECT d, ts, m FROM t"), want);
}

TEST(TypesContract, Item1_OutsideTheRangeIsAPositionedEncodeError) {
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE t (id int64, d date)").substr(0, 7), "CREATED");
    for (const char* literal : {"1899-12-31", "3000-01-01"}) {
        const std::string reply = db.Run(std::string("INSERT INTO t VALUES ('") + literal + "')");
        EXPECT_EQ(reply.substr(0, 3), "ERR") << literal << " -> " << reply;
    }
}

// ---- Item 2: ordering ---------------------------------------------------

TEST(TypesContract, Item2_OrderingAgreesWithIntegerOrderOnAllThreeTypes) {
    // Inserted out of order on purpose: if any of these compared as text,
    // '2026-1...' would sort before '2026-0...' and the range would answer
    // differently.
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE t (id int64, d date, ts timestamp, m decimal(10, 2))")
                  .substr(0, 7),
              "CREATED");
    const char* rows[][3] = {
        {"2026-11-05", "2026-11-05 00:00:00", "9.90"},
        {"2026-02-09", "2026-02-09 00:00:00", "100.00"},
        {"2026-09-30", "2026-09-30 00:00:00", "10.00"},
    };
    for (const auto& r : rows) {
        ASSERT_EQ(db.Run(std::string("INSERT INTO t VALUES ('") + r[0] + "', '" + r[1] + "', '" +
                         r[2] + "')")
                      .substr(0, 8),
                  "INSERTED");
    }

    // A range whose text ordering and calendar ordering disagree.
    EXPECT_EQ(db.Run("SELECT d FROM t WHERE d BETWEEN '2026-09-01' AND '2026-12-31'"),
              "d\\n2026-11-05\\n2026-09-30");
    EXPECT_EQ(db.Run("SELECT MIN(d), MAX(d) FROM t"), "min(d),max(d)\\n2026-02-09,2026-11-05");
    EXPECT_EQ(db.Run("SELECT MIN(ts), MAX(ts) FROM t"),
              "min(ts),max(ts)\\n2026-02-09 00:00:00,2026-11-05 00:00:00");
    // 9.90 < 10.00 < 100.00 numerically; as text, "10.00" < "100.00" <
    // "9.90".
    EXPECT_EQ(db.Run("SELECT MIN(m), MAX(m) FROM t"), "min(m),max(m)\\n9.90,100.00");
}

TEST(TypesContract, Item2_TwelvePointThreeEqualsTwelvePointThirtyAtScaleTwo) {
    // The spec names this one explicitly: at scale 2 the two literals are
    // the same value, and both render as `12.30`.
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE t (id int64, m decimal(10, 2))").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Run("INSERT INTO t VALUES ('12.3')").substr(0, 8), "INSERTED");
    ASSERT_EQ(db.Run("INSERT INTO t VALUES ('12.30')").substr(0, 8), "INSERTED");

    EXPECT_EQ(db.Run("SELECT m FROM t"), "m\\n12.30\\n12.30");
    EXPECT_EQ(db.Run("SELECT id FROM t WHERE m = '12.3'"), "id\\n1\\n2");
    EXPECT_EQ(db.Run("SELECT id FROM t WHERE m = '12.30'"), "id\\n1\\n2");
}

TEST(TypesContract, Item2_ABtreeRelationOrdersTheSameAsAHeapOne) {
    // Clustering is on the Keystone pk, not on a typed column, so what is
    // actually being pinned is that the storage form does not change the
    // answer - the same equivalence the heap/btree suite pins for
    // everything else.
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE h (id int64, d date)").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Run("CREATE TABLE b (id int64, d date) BTREE").substr(0, 7), "CREATED");
    for (const char* d : {"2026-11-05", "2026-02-09", "2026-09-30"}) {
        ASSERT_EQ(db.Run(std::string("INSERT INTO h VALUES ('") + d + "')").substr(0, 8),
                  "INSERTED");
        ASSERT_EQ(db.Run(std::string("INSERT INTO b VALUES ('") + d + "')").substr(0, 8),
                  "INSERTED");
    }
    EXPECT_EQ(db.Run("SELECT id, d FROM h WHERE d BETWEEN '2026-03-01' AND '2026-12-31'"),
              db.Run("SELECT id, d FROM b WHERE d BETWEEN '2026-03-01' AND '2026-12-31'"));
    EXPECT_EQ(db.Run("SELECT MIN(d), MAX(d) FROM h"), db.Run("SELECT MIN(d), MAX(d) FROM b"));
}

// ---- Item 3: coercion errors --------------------------------------------

TEST(TypesContract, Item3_EveryMalformedLiteralIsAPositionedError) {
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE t (id int64, d date, ts timestamp, m decimal(10, 2))")
                  .substr(0, 7),
              "CREATED");

    // At **compile**, for a predicate.
    for (const char* sql : {"SELECT id FROM t WHERE m = '12.345'",
                            "SELECT id FROM t WHERE d = '2026-02-30'",
                            "SELECT id FROM t WHERE d = 'not a date'",
                            "SELECT id FROM t WHERE ts = '2026-08-07 25:00:00'"}) {
        const std::string reply = db.Run(sql);
        EXPECT_EQ(reply.substr(0, 3), "ERR") << sql << " -> " << reply;
        EXPECT_NE(reply.find("at byte"), std::string::npos) << sql << " -> " << reply;
    }

    // At **encode**, for an INSERT. Same literals, same verdicts - which is
    // the property TY01's one-parser rule exists to give.
    for (const char* values : {"'2026-08-07', '2026-08-07 00:00:00', '12.345'",
                               "'2026-02-30', '2026-08-07 00:00:00', '1.00'",
                               "'not a date', '2026-08-07 00:00:00', '1.00'",
                               "'2026-08-07', '2026-08-07 25:00:00', '1.00'"}) {
        const std::string reply = db.Run(std::string("INSERT INTO t VALUES (") + values + ")");
        EXPECT_EQ(reply.substr(0, 3), "ERR") << values << " -> " << reply;
    }
    // And none of them wrote anything.
    EXPECT_EQ(db.Run("SELECT COUNT(*) FROM t"), "count(*)\\n0");
}

TEST(TypesContract, Item3_PrecisionAndScaleBoundsAreRefusedAtCreateTable) {
    Instance db;
    // `decimal(19, 0)` left this list when the wide type landed
    // (types.md TY2's separate int128 type, 2026-08-07): 19..38 now
    // selects `decimal128`, and the refusals move to the new edges.
    for (const char* decl : {"decimal(0, 0)", "decimal(39, 0)", "decimal(5, 6)",
                             "decimal(24, 25)", "decimal128(10, 2)", "decimal"}) {
        const std::string reply =
            db.Run(std::string("CREATE TABLE bad (id int64, m ") + decl + ")");
        EXPECT_EQ(reply.substr(0, 3), "ERR") << decl << " -> " << reply;
    }
}

// ---- Item 4: mixed-scale residual ---------------------------------------

TEST(TypesContract, Item4_AMixedScaleResidualIsUnsupportedWithAPosition) {
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE a (id int64, m decimal(10, 2))").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Run("CREATE TABLE b (id int64, m decimal(10, 3))").substr(0, 7), "CREATED");

    // In a WHERE, and in a JOIN's ON - the spec names the join, and the
    // two lower through different code, so both are pinned.
    for (const char* sql : {"SELECT a.id FROM a AS a JOIN b AS b ON a.id = b.id WHERE a.m = b.m",
                            "SELECT a.id FROM a AS a JOIN b AS b ON a.m = b.m"}) {
        const std::string reply = db.Run(sql);
        EXPECT_EQ(reply.substr(0, 3), "ERR") << sql << " -> " << reply;
        EXPECT_NE(reply.find("rescale"), std::string::npos) << sql << " -> " << reply;
        EXPECT_NE(reply.find("at byte"), std::string::npos) << sql << " -> " << reply;
    }

    // Matching scales are fine, which is what keeps the refusal about
    // scale rather than about decimals.
    ASSERT_EQ(db.Run("CREATE TABLE c (id int64, m decimal(10, 2))").substr(0, 7), "CREATED");
    // The full reply, not a prefix: over two empty relations this is the
    // heading and no rows, and asserting it whole is what makes the
    // difference from the refusal above unambiguous.
    EXPECT_EQ(db.Run("SELECT a.id FROM a AS a JOIN c AS c ON a.m = c.m"), "a.id");
}

// ---- Item 5: aggregates -------------------------------------------------

TEST(TypesContract, Item5_SumOverDecimalIsExactAtScale) {
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE t (id int64, m decimal(18, 2))").substr(0, 7), "CREATED");
    // Values a binary float cannot represent exactly: 0.1 + 0.2 is the
    // canonical demonstration, and the whole reason a financial engine
    // stores an integer.
    for (const char* m : {"0.10", "0.20", "0.30"}) {
        ASSERT_EQ(db.Run(std::string("INSERT INTO t VALUES ('") + m + "')").substr(0, 8),
                  "INSERTED");
    }
    EXPECT_EQ(db.Run("SELECT SUM(m) FROM t"), "sum(m)\\n0.60");
}

TEST(TypesContract, Item5_SumOverflowAtTheInt64EdgeIsAStatementError) {
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE t (id int64, m decimal(18, 0))").substr(0, 7), "CREATED");
    // Two values whose unscaled sum passes INT64_MAX. The accumulator is
    // checked, so this is an error rather than a wrapped answer - the one
    // output the fold must never produce.
    for (int i = 0; i < 20; ++i) {
        ASSERT_EQ(db.Run("INSERT INTO t VALUES ('999999999999999999')").substr(0, 8), "INSERTED");
    }
    const std::string reply = db.Run("SELECT SUM(m) FROM t");
    EXPECT_EQ(reply.substr(0, 3), "ERR") << reply;
    EXPECT_NE(reply.find("overflow"), std::string::npos) << reply;
}

TEST(TypesContract, Item5_SumOverADateIsRefusedAndMinMaxAreNot) {
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE t (id int64, d date, ts timestamp)").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Run("INSERT INTO t VALUES ('2026-08-07', '2026-08-07 00:00:00')").substr(0, 8),
              "INSERTED");

    EXPECT_EQ(db.Run("SELECT SUM(d) FROM t").substr(0, 3), "ERR");
    EXPECT_EQ(db.Run("SELECT SUM(ts) FROM t").substr(0, 3), "ERR");
    EXPECT_EQ(db.Run("SELECT MIN(d), MAX(ts) FROM t"),
              "min(d),max(ts)\\n2026-08-07,2026-08-07 00:00:00");
}

TEST(TypesContract, Item5_GroupByADateKeyEmitsFirstSeen) {
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE t (id int64, d date, m decimal(10, 2))").substr(0, 7),
              "CREATED");
    const char* rows[][2] = {
        {"2026-05-05", "1.00"}, {"2026-01-01", "2.00"},
        {"2026-05-05", "3.00"}, {"2026-01-01", "4.00"},
        {"2026-09-09", "5.00"},
    };
    for (const auto& r : rows) {
        ASSERT_EQ(db.Run(std::string("INSERT INTO t VALUES ('") + r[0] + "', '" + r[1] + "')")
                      .substr(0, 8),
                  "INSERTED");
    }
    // First-seen order, not sorted order - the fold preserves the order a
    // group's key first appeared, and 2026-05-05 appeared before
    // 2026-01-01.
    EXPECT_EQ(db.Run("SELECT d, COUNT(*), SUM(m) FROM t GROUP BY d"),
              "d,count(*),sum(m)"
              "\\n2026-05-05,2,4.00"
              "\\n2026-01-01,2,6.00"
              "\\n2026-09-09,1,5.00");
}

// ---- Item 6: fingerprint invariance -------------------------------------

TEST(TypesContract, Item6_ADateLiteralFingerprintsAsAStringLiteral) {
    // The fingerprint is folded from **tokens**, and a date literal is a
    // string literal to the lexer - which is what the types work must not
    // have changed, since `pattern_id` is stored in `sys.patterns` and a
    // moved hash orphans every recorded trail.
    const auto date = parser::FingerprintOf("SELECT id FROM t WHERE d = '2026-08-07'");
    const auto text = parser::FingerprintOf("SELECT id FROM t WHERE d = 'some string'");
    ASSERT_TRUE(date.has_value());
    ASSERT_TRUE(text.has_value());

    // Same shape: same pattern.
    EXPECT_EQ(date->pattern_id, text->pattern_id);
    // Different argument: different arg_hash. The pair is the key, so a
    // pattern_id that matched with a colliding arg_hash would serve one
    // date's trail for another.
    EXPECT_NE(date->arg_hash, text->arg_hash);
    EXPECT_EQ(date->literal_count, text->literal_count);
}

TEST(TypesContract, Item6_TheVersionDidNotMove) {
    // A blunt guard, and the one that matters: every stored `pattern_id`
    // is qualified by this number, so bumping it invalidates the corpus.
    // The types work did not need to, and this fails if someone assumes
    // otherwise.
    EXPECT_EQ(parser::kFingerprintVersion, 1u);
}

// ---- Item 7: rendering --------------------------------------------------

TEST(TypesContract, Item7_TheRenderedFormsAreTheSpecs) {
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE t (id int64, d date, ts timestamp, m decimal(10, 2))")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(db.Run("INSERT INTO t VALUES ('2026-08-06', '2026-08-06 09:15:00.250000', '12.30')")
                  .substr(0, 8),
              "INSERTED");
    ASSERT_EQ(db.Run("INSERT INTO t VALUES ('2026-08-06', '2026-08-06 09:15:00', '0.05')")
                  .substr(0, 8),
              "INSERTED");

    // §3.3, literally: a date, a timestamp with six fractional digits when
    // non-zero and none when zero, and a decimal with exactly `s` places.
    EXPECT_EQ(db.Run("SELECT d, ts, m FROM t"),
              "d,ts,m"
              "\\n2026-08-06,2026-08-06 09:15:00.250000,12.30"
              "\\n2026-08-06,2026-08-06 09:15:00,0.05");
}

TEST(TypesContract, Item7_TypeValZeroPreservesEveryPreTypesCaller) {
    // The compatibility half. `0` must mean "what FormatValue did before
    // types existed" for every kind that predates them, because that is
    // what a plan's literal, a catalog view and a group label all pass.
    using exec::FormatValue;

    parser::AstValue v;
    v.type = parser::ValueType::kInt;
    v.int_val = -5;
    EXPECT_EQ(FormatValue(0, v), "-5");

    v.raw_int_text = "18446744073709551615";
    EXPECT_EQ(FormatValue(0, v), "18446744073709551615");

    parser::AstValue s;
    s.type = parser::ValueType::kStr;
    s.str_val = "";
    EXPECT_EQ(FormatValue(0, s), "");

    parser::AstValue n;
    n.type = parser::ValueType::kNull;
    EXPECT_EQ(FormatValue(0, n), "NULL");
}

// ---- Item 8: catalog ----------------------------------------------------

TEST(TypesContract, Item8_PrecisionAndScaleSurviveARestart) {
    // TY02 packed `(p, s)` into `SysColumnRow::len` rather than widening
    // the row, so this is the test that the packing is read back the same
    // way it was written - by a Catalog that has never seen the CREATE.
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE t (id int64, d date, ts timestamp, m decimal(12, 4))")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(db.Run("INSERT INTO t VALUES ('2026-08-07', '2026-08-07 09:15:00', '1.2345')")
                  .substr(0, 8),
              "INSERTED");

    const std::string described = db.Run("DESCRIBE t");
    const std::string rows = db.Run("SELECT d, ts, m FROM t");

    db.Remount();

    // The declared type reads back exactly, scale included - if `(p, s)`
    // were lost, `decimal(12,4)` would come back as something else and the
    // value would render at the wrong scale.
    EXPECT_EQ(db.Run("DESCRIBE t"), described);
    EXPECT_NE(db.Run("DESCRIBE t").find("decimal(12,4)"), std::string::npos)
        << db.Run("DESCRIBE t");
    EXPECT_EQ(db.Run("SELECT d, ts, m FROM t"), rows);

    // And the scale still governs new writes and new predicates after the
    // remount, which a cached-but-wrong `(p, s)` would not.
    ASSERT_EQ(db.Run("INSERT INTO t VALUES ('2026-08-08', '2026-08-08 00:00:00', '2.5')")
                  .substr(0, 8),
              "INSERTED");
    EXPECT_EQ(db.Run("SELECT m FROM t WHERE m = '2.5000'"), "m\\n2.5000");
    EXPECT_EQ(db.Run("SELECT id FROM t WHERE m = '1.23456'").substr(0, 3), "ERR");
}

TEST(TypesContract, Item8_TheTypesAreVisibleThroughTheCatalogViews) {
    // `sys.columns` renders the declared type through the same
    // `ColumnTypeText` DESCRIBE uses (TY02's note), so the two cannot
    // disagree about what a column was declared as.
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE t (id int64, d date, ts timestamp, m decimal(9, 3))")
                  .substr(0, 7),
              "CREATED");
    const std::string columns = db.Run("SELECT * FROM sys.columns");
    EXPECT_NE(columns.find("date"), std::string::npos) << columns;
    EXPECT_NE(columns.find("timestamp"), std::string::npos) << columns;
    EXPECT_NE(columns.find("decimal(9,3)"), std::string::npos) << columns;
}

}  // namespace
}  // namespace kds::server
