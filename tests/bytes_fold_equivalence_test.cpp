// EH-S1, EH-R1: a global fold over one relation with no predicate takes its
// rows from the page's bytes (`bytes_fold.hpp`); the same fold behind a
// predicate that keeps every row takes them through the frame and the row
// sink, as every fold did before. **The two must answer byte for byte** - the
// same values, the same NULLs, the same refusals with the same words - and
// these cells compare them, statement by statement, over every fixed-width
// type, over NULLs, deleted and updated rows, a version only the undo log
// holds, and an overflow.
//
// What this file cannot see is which path a statement took; that a step that
// keeps every row hands its fold every row but the first is
// `scan_row_walk_test.cpp`'s, with a counter.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "sim/instance.hpp"

namespace kds {
namespace {

bool IsErr(const std::string& reply) { return reply.rfind("ERR", 0) == 0; }

// `k` is NOT NULL and never below -30000, so this keeps every row - and its
// presence puts a conjunct on the step, which keeps the full path.
constexpr const char* kKeepsEveryRow = " WHERE k > -30000";

const std::vector<std::string>& SelectLists() {
    static const std::vector<std::string> lists = {
        "COUNT(*)",
        "COUNT(a)", "COUNT(b)", "COUNT(d)", "COUNT(e)", "COUNT(g)", "COUNT(h)", "COUNT(s)",
        "COUNT(u)", "COUNT(id)",
        "SUM(a)", "SUM(b)", "SUM(k)", "SUM(d)",
        "MIN(a)", "MAX(a)", "MIN(b)", "MAX(b)", "MIN(k)", "MAX(k)", "MIN(d)", "MAX(d)",
        "MIN(e)", "MAX(e)", "MIN(g)", "MAX(g)", "MIN(h)", "MAX(h)", "MIN(u)", "MAX(u)",
        "MIN(id)", "MAX(id)", "MIN(s)", "MAX(s)",
        "AVG(a)", "AVG(b)", "AVG(d)",
        "COUNT(*), MIN(a), MAX(a), SUM(a)",
        "COUNT(*), SUM(b), AVG(a), MIN(e), MAX(h), COUNT(d), MAX(g), MIN(u)",
        "SUM(a), SUM(a), COUNT(a)",
        "COUNT(DISTINCT b)", "SUM(DISTINCT b)",
    };
    return lists;
}

class BytesFoldEquivalenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto created = sim::SimInstance::Create();
        ASSERT_TRUE(created.ok()) << created.status().message();
        db_ = std::move(created.value());
        Must("CREATE TABLE f (id int64, k int16, a int64 NULL, b int32 NULL, "
             "d decimal(12,2) NULL, e date NULL, g timestamp NULL, h char(8) NULL, "
             "s varchar NULL, u uint64 NULL)");
    }

    void Must(const std::string& sql) {
        const std::string reply = db_->Execute(sql);
        ASSERT_FALSE(IsErr(reply)) << sql << " -> " << reply;
    }

    // Enough rows for several leaves, every third with NULLs, signs mixed.
    void Load(int rows) {
        for (int i = 1; i <= rows; ++i) {
            if (i % 3 == 0) {
                Must("INSERT INTO f VALUES (" + std::to_string(i) + ", " + std::to_string(i % 100) +
                     ", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)");
                continue;
            }
            const long long a = (i % 2 == 0 ? -1LL : 1LL) * i * 1000003LL;
            Must("INSERT INTO f VALUES (" + std::to_string(i) + ", " + std::to_string(i % 100 - 50) +
                 ", " + std::to_string(a) + ", " + std::to_string(i % 7 - 3) + ", " +
                 std::to_string(i % 5 - 2) + "." + (i % 2 ? "25" : "50") + ", '2026-01-" +
                 (i % 28 + 1 < 10 ? "0" : "") + std::to_string(i % 28 + 1) + "', '2026-02-01 00:00:" +
                 (i % 60 < 10 ? "0" : "") + std::to_string(i % 60) + "', 'c" + std::to_string(i % 13) +
                 "', 'v" + std::to_string(i) + "', " + std::to_string(1000000 + i) + ")");
        }
    }

    // Every select list, bare and behind the conjunct that keeps every row.
    void ExpectBothPathsAgree(const std::string& when) {
        for (const std::string& list : SelectLists()) {
            const std::string bare = db_->Execute("SELECT " + list + " FROM f");
            const std::string kept = db_->Execute("SELECT " + list + " FROM f" + kKeepsEveryRow);
            EXPECT_EQ(bare, kept) << when << ": SELECT " << list << " FROM f";
        }
    }

    std::unique_ptr<sim::SimInstance> db_;
};

TEST_F(BytesFoldEquivalenceTest, AnEmptyRelationAnswersTheSame) {
    ExpectBothPathsAgree("empty");
}

TEST_F(BytesFoldEquivalenceTest, OneRowAnswersTheSame) {
    // One row is the row that checks the step: nothing is folded in place.
    Load(1);
    ExpectBothPathsAgree("one row");
}

TEST_F(BytesFoldEquivalenceTest, EveryTypeAndEveryNullAnswerTheSameOverSeveralLeaves) {
    Load(700);
    ExpectBothPathsAgree("700 rows");
    EXPECT_EQ(db_->Execute("SELECT COUNT(*), COUNT(a) FROM f"), "count(*),count(a)\\n700,467");
}

TEST_F(BytesFoldEquivalenceTest, DeletedAndUpdatedRowsAnswerTheSame) {
    Load(300);
    Must("DELETE FROM f WHERE id = 2");
    Must("DELETE FROM f WHERE id = 299");
    Must("UPDATE f SET a = 77 WHERE id = 4");
    Must("UPDATE f SET b = NULL WHERE id = 5");
    Must("UPDATE f SET a = 5 WHERE id = 3");  // a NULL made a value
    ExpectBothPathsAgree("after deletes and updates");
    EXPECT_EQ(db_->Execute("SELECT COUNT(*) FROM f"), "count(*)\\n298");
}

TEST_F(BytesFoldEquivalenceTest, AColumnOfNullsAnswersNullTheSame) {
    for (int i = 1; i <= 40; ++i) {
        Must("INSERT INTO f VALUES (" + std::to_string(i) +
             ", 1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)");
    }
    ExpectBothPathsAgree("all NULL");
    EXPECT_EQ(db_->Execute("SELECT SUM(a), MIN(a), COUNT(a), COUNT(*) FROM f"),
              "sum(a),min(a),count(a),count(*)\\nNULL,NULL,0,40");
}

// EH-R2: a version this reader may not see is not folded from the page. An
// open transaction has updated one row and deleted another; the reader's
// answers are the ones from before it, by both paths, and move when it
// commits.
TEST_F(BytesFoldEquivalenceTest, AnUndecidedWritersRowsAreFoldedAsTheReaderSeesThem) {
    Load(120);
    const std::string before = db_->Execute("SELECT COUNT(*), SUM(a), MIN(a), MAX(b) FROM f");
    ASSERT_FALSE(IsErr(before)) << before;

    server::Session writer;
    ASSERT_FALSE(IsErr(db_->dispatcher().Dispatch("BEGIN", &writer).response));
    ASSERT_FALSE(IsErr(
        db_->dispatcher().Dispatch("UPDATE f SET a = 900000000000 WHERE id = 7", &writer).response));
    ASSERT_FALSE(IsErr(db_->dispatcher().Dispatch("DELETE FROM f WHERE id = 8", &writer).response));
    ASSERT_FALSE(IsErr(
        db_->dispatcher().Dispatch("UPDATE f SET b = 6 WHERE id = 10", &writer).response));

    EXPECT_EQ(db_->Execute("SELECT COUNT(*), SUM(a), MIN(a), MAX(b) FROM f"), before);
    ExpectBothPathsAgree("beside an open writer");

    ASSERT_FALSE(IsErr(db_->dispatcher().Dispatch("COMMIT", &writer).response));
    const std::string after = db_->Execute("SELECT COUNT(*), SUM(a), MIN(a), MAX(b) FROM f");
    EXPECT_NE(after, before);
    ExpectBothPathsAgree("after its commit");
}

// The types `f` does not carry: `int8`, `bool`, the wide decimal (its int128
// arm in `FoldValue`) and a `uint64` above INT64_MAX, which MIN and MAX
// compare through the digit text the decoder must set on the bytes path too.
TEST_F(BytesFoldEquivalenceTest, TheRemainingTypesAnswerTheSame) {
    Must("CREATE TABLE x (id int64, k int16, t int8 NULL, o bool NULL, w decimal(30,4) NULL, "
         "u uint64 NULL)");
    const char* const u_values[] = {"5", "18446744073709551615", "9223372036854775808",
                                    "9223372036854775807", "12"};
    for (int i = 1; i <= 400; ++i) {
        if (i % 4 == 0) {
            Must("INSERT INTO x VALUES (" + std::to_string(i) + ", 1, NULL, NULL, NULL, NULL)");
            continue;
        }
        Must("INSERT INTO x VALUES (" + std::to_string(i) + ", 1, " +
             std::to_string(i % 200 - 100) +
             ", " + (i % 2 ? "1" : "0") + ", " + (i % 3 ? "-" : "") +
             "12345678901234567890." + std::to_string(1000 + i % 9000) + ", " + u_values[i % 5] +
             ")");
    }
    const std::vector<std::string> lists = {
        "COUNT(t)", "SUM(t)", "MIN(t)", "MAX(t)", "AVG(t)", "COUNT(o)", "MIN(o)", "MAX(o)",
        "COUNT(w)", "SUM(w)", "MIN(w)", "MAX(w)", "AVG(w)", "COUNT(u)", "MIN(u)", "MAX(u)",
        "COUNT(*), SUM(w), MIN(u), MAX(u), MAX(t)",
    };
    for (const std::string& list : lists) {
        const std::string bare = db_->Execute("SELECT " + list + " FROM x");
        const std::string kept = db_->Execute("SELECT " + list + " FROM x" + kKeepsEveryRow);
        EXPECT_EQ(bare, kept) << "SELECT " << list << " FROM x";
    }
    EXPECT_EQ(db_->Execute("SELECT MIN(u), MAX(u) FROM x"),
              "min(u),max(u)\\n5,18446744073709551615");
}

// EH-R2 from the writer's side: inside its own transaction a reader sees its
// uncommitted writes, and a repeatable-read `READ ONLY` transaction sees its
// one view's rows across the writer's commit, by both paths.
TEST_F(BytesFoldEquivalenceTest, ATransactionsOwnWritesAndAReadOnlyViewAnswerTheSame) {
    Load(150);
    const auto both = [&](server::Session& s, const std::string& list) {
        const std::string bare =
            db_->dispatcher().Dispatch("SELECT " + list + " FROM f", &s).response;
        const std::string kept =
            db_->dispatcher().Dispatch("SELECT " + list + " FROM f" + kKeepsEveryRow, &s).response;
        EXPECT_FALSE(IsErr(bare)) << bare;
        EXPECT_EQ(bare, kept) << "SELECT " << list << " FROM f";
        return bare;
    };
    const std::string list = "COUNT(*), SUM(a), MIN(a), MAX(b), COUNT(d)";

    server::Session reader;
    ASSERT_FALSE(IsErr(db_->dispatcher()
                           .Dispatch("BEGIN READ ONLY ISOLATION LEVEL REPEATABLE READ", &reader)
                           .response));
    const std::string viewed = both(reader, list);

    server::Session writer;
    ASSERT_FALSE(IsErr(db_->dispatcher().Dispatch("BEGIN", &writer).response));
    const std::string before = both(writer, list);
    EXPECT_EQ(before, viewed);
    ASSERT_FALSE(IsErr(
        db_->dispatcher().Dispatch("UPDATE f SET a = 900000000000 WHERE id = 7", &writer).response));
    ASSERT_FALSE(IsErr(db_->dispatcher().Dispatch("DELETE FROM f WHERE id = 8", &writer).response));
    ASSERT_FALSE(IsErr(db_->dispatcher()
                           .Dispatch("INSERT INTO f VALUES (1000, 1, 1, 1, 1.25, NULL, NULL, NULL, "
                                     "NULL, NULL)",
                                     &writer)
                           .response));
    const std::string own = both(writer, list);
    EXPECT_NE(own, before);
    ASSERT_FALSE(IsErr(db_->dispatcher().Dispatch("COMMIT", &writer).response));

    // The read-only view still answers from its snapshot, by both paths.
    EXPECT_EQ(both(reader, list), viewed);
    ASSERT_FALSE(IsErr(db_->dispatcher().Dispatch("COMMIT", &reader).response));
    EXPECT_EQ(db_->Execute("SELECT " + list + " FROM f"), own);
}

// EH-R4: an overflow is refused on the same row with the same words.
TEST_F(BytesFoldEquivalenceTest, AnOverflowIsRefusedTheSame) {
    Must("INSERT INTO f VALUES (1, 1, 1, NULL, NULL, NULL, NULL, NULL, NULL, NULL)");
    Must("INSERT INTO f VALUES (2, 1, 9000000000000000000, NULL, NULL, NULL, NULL, NULL, NULL, NULL)");
    Must("INSERT INTO f VALUES (3, 1, 9000000000000000000, NULL, NULL, NULL, NULL, NULL, NULL, NULL)");
    const std::string bare = db_->Execute("SELECT SUM(a) FROM f");
    const std::string kept = db_->Execute(std::string("SELECT SUM(a) FROM f") + kKeepsEveryRow);
    EXPECT_TRUE(IsErr(bare)) << bare;
    EXPECT_EQ(bare, kept);
    ExpectBothPathsAgree("with an overflowing column");
}

}  // namespace
}  // namespace kds
