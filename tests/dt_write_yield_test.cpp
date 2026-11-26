#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "lock_family_fixture.hpp"

// DT (`instructions/v3.2.0/workorder-dt-write-path-yield.md`): a write's walk
// gives its core back between two leaves, and a key loop between two
// descents - a slice boundary's yield, on one reactor.
//
// **Every cell writes `SET v = v + 1`**, so a row written twice reads 2: the
// statement's count and the rows' values are checked against each other, and
// neither alone would show a resume that offered a row again (DT-R3).
//
// The walker runs at the `analytic` level in most cells - a slice of 16
// leaves, so a relation of some sixty leaves is three pauses - and one cell
// runs the default 64.

namespace kds::server {
namespace {

class WriteYieldTest : public LockFamilyTest {
protected:
    // About seven rows a page: the pad is most of an 8 KiB page's tuple.
    static constexpr int kPadWidth = 1000;

    // `name` in both storage forms, `rows` rows each, ids `step`, `2 * step`,
    // ... and every `v` 0. **The two forms resume by different means** - the
    // btree by key, the heap by place - so every cell runs against both. The
    // form is written on both: a table that names none is a btree. `id` is
    // the declared Keystone on both, so a list on it is a key loop on both.
    void Make(const std::string& name, int rows, int step = 1) {
        for (const char* form : {" HEAP", " BTREE"}) {
            const std::string table = name + (form[1] == 'H' ? "h" : "b");
            ASSERT_EQ(Local("CREATE TABLE " + table + " (id int64 PRIMARY KEY, v int64, pad char(" +
                            std::to_string(kPadWidth) + "))" + form)
                          .rfind("CREATED", 0),
                      0u);
            for (int at = 1; at <= rows;) {
                std::string values;
                for (int n = 0; n < 50 && at <= rows; ++n, ++at) {
                    values += (values.empty() ? "(" : ", (") + std::to_string(at * step) +
                              ", 0, 'p')";
                }
                const std::string reply = Local("INSERT INTO " + table + " VALUES " + values);
                ASSERT_EQ(reply.rfind("INSERTED", 0), 0u) << reply;
            }
        }
    }

    // How many rows of `table` pass `where`: a reply is its header and one
    // `\n`-separated line a row, the separator a literal backslash-n.
    int Count(const std::string& table, const std::string& where) {
        const std::string rows = Local("SELECT id FROM " + table + " WHERE " + where);
        EXPECT_NE(rows.rfind("ERR", 0), 0u) << rows;
        int n = 0;
        for (std::size_t at = rows.find("\\n"); at != std::string::npos;
             at = rows.find("\\n", at + 2)) {
            ++n;
        }
        return n;
    }

    void Analytic(Session& s) {
        ASSERT_EQ(dispatcher_->Dispatch("SET PRIORITY analytic", &s).response,
                  "SET priority=analytic");
    }

    // Pumps `walk` to its end one turn at a time and returns how many turns
    // it was still unfinished after - each one a pause the scheduler saw.
    int TurnsToFinish(const Started& walk, int limit = 400) {
        int unfinished = 0;
        for (int i = 0; i < limit && !*walk.done; ++i) {
            Pump(1);
            if (!*walk.done) ++unfinished;
        }
        EXPECT_TRUE(*walk.done) << "the walk never finished: " << *walk.sql;
        return unfinished;
    }
};

TEST_F(WriteYieldTest, AWalkedUpdateGivesItsCoreBackEverySliceAndWritesEachRowOnce) {
    Make("y", 400);
    for (const std::string table : {"yh", "yb"}) {
        Session w;
        Analytic(w);
        const std::uint64_t before = dispatcher_->write_walk_pauses();
        // `v >= 0`: no tuple key window, so nothing is declared and each row
        // takes its unit - the walk that claims as it goes.
        Started walk = Start("UPDATE " + table + " SET v = v + 1 WHERE v >= 0", w);
        Pump(1);
        ASSERT_FALSE(*walk.done) << table << ": a walk of some sixty leaves took no pause";
        ASSERT_TRUE(w.parked_write().has_value()) << table;
        // Another session's statement runs between two slices: the core was
        // given back, which is the stage.
        EXPECT_EQ(Local("SELECT id FROM t").rfind("ERR", 0), std::string::npos);

        const int turns = TurnsToFinish(walk);
        EXPECT_GE(turns, 1) << table;
        EXPECT_GE(dispatcher_->write_walk_pauses() - before, 2u)
            << table << ": some sixty leaves in slices of sixteen";
        EXPECT_EQ(walk.out->response.rfind("UPDATED 400", 0), 0u)
            << table << ": " << walk.out->response;
        EXPECT_EQ(Count(table, "v = 1"), 400) << table << ": a row was missed or written twice";
        EXPECT_FALSE(w.parked_write().has_value()) << table;
        EXPECT_EQ(locks_->EntryCount(), 0u) << table << ": the statement's units outlived it";
    }
}

TEST_F(WriteYieldTest, TheDefaultSliceIsTheReadWalksSixtyFourLeaves) {
    // DT-Q2 (a): one value with a `SELECT`'s walk. A relation under one
    // slice takes no pause at all (DT-R4), and one of about 140 leaves two.
    Make("s", 60);
    Make("l", 1000);
    for (const std::string form : {"h", "b"}) {
        Session w;
        std::uint64_t before = dispatcher_->write_walk_pauses();
        DispatchOutcome small = RunAsync("UPDATE s" + form + " SET v = v + 1 WHERE v >= 0", w);
        EXPECT_EQ(small.response.rfind("UPDATED 60", 0), 0u) << small.response;
        EXPECT_EQ(dispatcher_->write_walk_pauses(), before) << "s" << form;

        before = dispatcher_->write_walk_pauses();
        DispatchOutcome large =
            RunAsync("UPDATE l" + form + " SET v = v + 1 WHERE v >= 0", w, /*turns=*/400);
        EXPECT_EQ(large.response.rfind("UPDATED 1000", 0), 0u) << large.response;
        const std::uint64_t pauses = dispatcher_->write_walk_pauses() - before;
        EXPECT_GE(pauses, 2u) << "l" << form;
        EXPECT_LE(pauses, 4u) << "l" << form;
        EXPECT_EQ(Count("l" + form, "v = 1"), 1000) << "l" << form;
    }
}

TEST_F(WriteYieldTest, AWriterBetweenTwoSlicesMovesNoRowUnderTheResume) {
    // DT-R3. Ids 10, 20, ... 4000. After the walk's first slice another
    // session commits a row into every gap - below the walk's place and
    // above it - which divides leaves the walk has finished and leaves it
    // has not. The walk's view was minted before them: it writes its four
    // hundred rows once each and none of theirs.
    Make("r", 400, /*step=*/10);
    for (const std::string table : {"rh", "rb"}) {
        Session w;
        Analytic(w);
        Started walk = Start("UPDATE " + table + " SET v = v + 1 WHERE v >= 0", w);
        Pump(1);
        ASSERT_FALSE(*walk.done) << table;

        for (int at = 1; at <= 400;) {
            std::string values;
            for (int n = 0; n < 50 && at <= 400; ++n, ++at) {
                values += (values.empty() ? "(" : ", (") + std::to_string(at * 10 - 5) +
                          ", 100, 'q')";
            }
            const std::string reply = Local("INSERT INTO " + table + " VALUES " + values);
            ASSERT_EQ(reply.rfind("INSERTED", 0), 0u) << table << ": " << reply;
        }

        TurnsToFinish(walk);
        EXPECT_EQ(walk.out->response.rfind("UPDATED 400", 0), 0u)
            << table << ": " << walk.out->response;
        EXPECT_EQ(Count(table, "v = 1"), 400) << table;
        EXPECT_EQ(Count(table, "v = 100"), 400) << table << ": the walk wrote a row it cannot see";
        EXPECT_EQ(Count(table, "v > 1 AND v < 100"), 0) << table << ": a row was written twice";
    }
}

TEST_F(WriteYieldTest, InsideATransactionTheTrailHoldsEachRowOnce) {
    Make("x", 400);
    for (const std::string table : {"xh", "xb"}) {
        Session w;
        Analytic(w);
        ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &w).response.rfind("BEGIN", 0), 0u);
        const std::size_t mark = w.transaction()->trail().size();
        Started walk = Start("UPDATE " + table + " SET v = v + 1 WHERE v >= 0", w);
        Pump(1);
        ASSERT_FALSE(*walk.done) << table;
        // The rows of the first slice are on the trail and nobody else's to see.
        EXPECT_GT(w.transaction()->trail().size(), mark) << table;
        EXPECT_EQ(Count(table, "v = 1"), 0) << table;
        TurnsToFinish(walk);
        EXPECT_EQ(walk.out->response.rfind("UPDATED 400", 0), 0u) << walk.out->response;
        EXPECT_EQ(w.transaction()->trail().size() - mark, 400u)
            << table << ": one trail entry a row";
        ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &w).response.rfind("COMMIT", 0), 0u);
        EXPECT_EQ(Count(table, "v = 1"), 400) << table;
    }
}

TEST_F(WriteYieldTest, ADeleteGivesItsCoreBackToo) {
    Make("d", 400);
    for (const std::string table : {"dh", "db"}) {
        Session w;
        Analytic(w);
        Started walk = Start("DELETE FROM " + table + " WHERE v >= 0 AND id > 20", w);
        Pump(1);
        ASSERT_FALSE(*walk.done) << table;
        ASSERT_TRUE(w.parked_write().has_value()) << table;
        TurnsToFinish(walk);
        EXPECT_EQ(walk.out->response.rfind("DELETED 380", 0), 0u) << walk.out->response;
        EXPECT_EQ(Count(table, "v >= 0"), 20) << table;
    }
}

TEST_F(WriteYieldTest, AKeyLoopGivesItsCoreBackBetweenTwoDescents) {
    // A list on the tuple key is one descent a value (CU-S3): a slice of
    // sixteen descents, the loop's place the next value.
    Make("k", 100);
    std::string list;
    for (int id = 2; id <= 100; id += 2) list += (list.empty() ? "" : ", ") + std::to_string(id);
    for (const std::string table : {"kh", "kb"}) {
        Session w;
        Analytic(w);
        const std::uint64_t before = dispatcher_->write_walk_pauses();
        Started walk = Start("UPDATE " + table + " SET v = v + 1 WHERE id IN (" + list + ")", w);
        Pump(1);
        ASSERT_FALSE(*walk.done) << table << ": fifty descents took no pause";
        TurnsToFinish(walk);
        EXPECT_EQ(walk.out->response.rfind("UPDATED 50", 0), 0u) << walk.out->response;
        EXPECT_EQ(dispatcher_->write_walk_pauses() - before, 3u)
            << table << ": fifty descents in slices of sixteen";
        EXPECT_EQ(Count(table, "v = 1"), 50) << table;
        EXPECT_EQ(Count(table, "v = 0"), 50) << table;
    }
}

TEST_F(WriteYieldTest, APausedWalkThatClaimedNoRowStillHoldsItsRelation) {
    // DT-R7. `v < 0` matches nothing, so the walk claims no row - and a walk
    // that held nothing across its pause would resume by key into whatever
    // relation then carried its name. It takes the relation's `IX` before it
    // gives the core back, so a drop is refused while it is away.
    Make("p", 400);
    for (const std::string table : {"ph", "pb"}) {
        Session w;
        Analytic(w);
        Started walk = Start("UPDATE " + table + " SET v = v + 1 WHERE v < 0", w);
        Pump(1);
        ASSERT_FALSE(*walk.done) << table;
        ASSERT_TRUE(w.parked_write().has_value()) << table;

        const std::string dropped = Local("DROP TABLE " + table);
        EXPECT_EQ(dropped.rfind("ERR", 0), 0u)
            << table << ": a drop ran between a walk's two slices: " << dropped;

        TurnsToFinish(walk);
        EXPECT_EQ(walk.out->response.rfind("UPDATED 0", 0), 0u) << walk.out->response;
        EXPECT_EQ(locks_->EntryCount(), 0u) << table;
        EXPECT_EQ(Local("DROP TABLE " + table).rfind("ERR", 0), std::string::npos) << table;
    }
}

TEST_F(WriteYieldTest, TheSynchronousPathResumesAPauseWhereItStands) {
    // `Dispatch` has nowhere to give the core to: it crosses the same
    // boundaries and resumes at once, so the suite and the simulator walk
    // the resume path with every long write they run.
    Make("z", 400);
    for (const std::string table : {"zh", "zb"}) {
        Session w;
        Analytic(w);
        const std::uint64_t before = dispatcher_->write_walk_pauses();
        const DispatchOutcome out =
            dispatcher_->Dispatch("UPDATE " + table + " SET v = v + 1 WHERE v >= 0", &w);
        EXPECT_EQ(out.response.rfind("UPDATED 400", 0), 0u) << out.response;
        EXPECT_GE(dispatcher_->write_walk_pauses() - before, 2u) << table;
        EXPECT_FALSE(w.parked_write().has_value()) << table;
        EXPECT_EQ(Count(table, "v = 1"), 400) << table;
        EXPECT_EQ(locks_->EntryCount(), 0u) << table;
    }
}

TEST_F(WriteYieldTest, AHeldRowAfterAPauseParksAndTheResumeFinishesOnce) {
    // The two stops of one walk: slice boundaries first, then a row an
    // undecided writer holds (AO-S3b). The statement has written by then, so
    // it parks on the holder and resumes under the view it began with.
    Make("m", 400);
    for (const std::string table : {"mh", "mb"}) {
        Session holder;
        ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
        ASSERT_EQ(dispatcher_->Dispatch("UPDATE " + table + " SET v = 9 WHERE id = 390", &holder)
                      .response.rfind("UPDATED 1", 0),
                  0u);
        Session w;
        Analytic(w);
        const std::uint64_t before = dispatcher_->write_walk_pauses();
        Started walk = Start("UPDATE " + table + " SET v = v + 1 WHERE v >= 0", w);
        Pump(200);
        ASSERT_FALSE(*walk.done) << table << ": the walk passed a held row";
        EXPECT_GE(dispatcher_->write_walk_pauses() - before, 2u)
            << table << ": the walk met the held row before its first boundary";
        ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
        TurnsToFinish(walk);
        EXPECT_EQ(walk.out->response.rfind("UPDATED 400", 0), 0u) << walk.out->response;
        EXPECT_EQ(Count(table, "v = 1"), 400) << table;
    }
}

TEST_F(WriteYieldTest, ReturningAnswersEveryRowOfAPausedStatement) {
    // `RETURNING` is read off the statement's trail once, at its end, from
    // the mark the first run took - which every resume restores.
    Make("g", 400);
    for (const std::string table : {"gh", "gb"}) {
        Session w;
        Analytic(w);
        const std::uint64_t before = dispatcher_->write_walk_pauses();
        const DispatchOutcome out = RunAsync(
            "UPDATE " + table + " SET v = v + 1 WHERE v >= 0 RETURNING id", w, /*turns=*/400);
        EXPECT_GE(dispatcher_->write_walk_pauses() - before, 2u) << table;
        // The header, one line a row, then the tag.
        std::size_t lines = 0;
        for (std::size_t at = out.response.find("\\n"); at != std::string::npos;
             at = out.response.find("\\n", at + 2)) {
            ++lines;
        }
        EXPECT_EQ(lines, 401u) << table << ": " << out.response.substr(0, 80);
        EXPECT_NE(out.response.find("UPDATED 400"), std::string::npos) << table;
    }
}

TEST_F(WriteYieldTest, ARowThatMovesAUniqueKeyIsCheckedAndWrittenOnceAcrossPauses) {
    // Every row stops the walk for its unique check (BX-R4) and is offered
    // again under the clearance; the walk between those stops still pauses
    // at its slice boundaries, and the key units are back before each.
    for (const std::string form : {"HEAP", "BTREE"}) {
        const std::string table = form == "HEAP" ? "uh" : "ub";
        ASSERT_EQ(Local("CREATE TABLE " + table + " (id int64 PRIMARY KEY, u int64, pad char(" +
                        std::to_string(kPadWidth) + ")) " + form)
                      .rfind("CREATED", 0),
                  0u);
        for (int at = 1; at <= 200;) {
            std::string values;
            for (int n = 0; n < 50 && at <= 200; ++n, ++at) {
                values += (values.empty() ? "(" : ", (") + std::to_string(at) + ", " +
                          std::to_string(at) + ", 'p')";
            }
            ASSERT_EQ(Local("INSERT INTO " + table + " VALUES " + values).rfind("INSERTED", 0), 0u);
        }
        ASSERT_EQ(Local("CREATE UNIQUE INDEX " + table + "_u ON " + table + " (u)")
                      .rfind("ERR", 0),
                  std::string::npos);
        Session w;
        Analytic(w);
        const std::uint64_t before = dispatcher_->write_walk_pauses();
        const DispatchOutcome out =
            RunAsync("UPDATE " + table + " SET u = u + 1000 WHERE u >= 0", w, /*turns=*/400);
        EXPECT_EQ(out.response.rfind("UPDATED 200", 0), 0u) << out.response;
        EXPECT_GE(dispatcher_->write_walk_pauses() - before, 1u) << table;
        EXPECT_EQ(Count(table, "u > 1000 AND u <= 1200"), 200) << table;
        EXPECT_EQ(locks_->EntryCount(), 0u) << table;
    }
}

TEST_F(WriteYieldTest, CollectedTargetsAreCarriedAcrossAPause) {
    // A `WHERE` holding a subquery is evaluated once, in a read pass, and
    // the write walk writes the keys it collected. The pass itself is not
    // sliced; the walk is, and each resume carries the same set.
    Make("c", 400);
    ASSERT_EQ(Local("CREATE TABLE src (id int64 PRIMARY KEY) BTREE").rfind("CREATED", 0), 0u);
    std::string values;
    for (int id = 2; id <= 400; id += 2) {
        values += (values.empty() ? "(" : ", (") + std::to_string(id) + ")";
    }
    ASSERT_EQ(Local("INSERT INTO src VALUES " + values).rfind("INSERTED", 0), 0u);
    for (const std::string table : {"ch", "cb"}) {
        Session w;
        Analytic(w);
        const std::uint64_t before = dispatcher_->write_walk_pauses();
        const DispatchOutcome out = RunAsync(
            "UPDATE " + table + " SET v = v + 1 WHERE id IN (SELECT id FROM src)", w,
            /*turns=*/400);
        EXPECT_EQ(out.response.rfind("UPDATED 200", 0), 0u) << out.response;
        EXPECT_GE(dispatcher_->write_walk_pauses() - before, 2u) << table;
        EXPECT_EQ(Count(table, "v = 1"), 200) << table;
        EXPECT_EQ(Count(table, "v = 0"), 200) << table;
    }
}

// ---- DT §2.4: a cancel is taken where a write gives its core back ----------
//
// The session's flag is what a `C_CANCEL` sets through the cancel registry
// and what a connection closed under its statement sets
// (`TcpServer::CloseClient`), so these cells set it directly.

TEST_F(WriteYieldTest, ACancelAtAPauseEndsAnAutocommitWriteWithNoRowWritten) {
    Make("a", 400);
    for (const std::string table : {"ah", "ab"}) {
        for (const std::string write : {"UPDATE " + table + " SET v = v + 1 WHERE v >= 0",
                                        "DELETE FROM " + table + " WHERE v >= 0"}) {
            Session w;
            Analytic(w);
            Started walk = Start(write, w);
            Pump(1);
            ASSERT_FALSE(*walk.done) << write;
            w.RequestCancel();
            TurnsToFinish(walk);
            EXPECT_EQ(walk.out->status.code(), StatusCode::kCancelled) << walk.out->response;
            EXPECT_FALSE(w.TakeCancel()) << "the write took the cancel: one cancel, one statement";
            EXPECT_FALSE(w.parked_write().has_value()) << write;
            // The slices it had written are undone with its transaction.
            EXPECT_EQ(Count(table, "v = 0"), 400) << write;
            EXPECT_EQ(locks_->EntryCount(), 0u) << write << ": the cancelled write kept a unit";
            // Aborted, not left open: another writer takes a row it wrote.
            EXPECT_EQ(Local("UPDATE " + table + " SET v = v WHERE id = 1"), "UPDATED 1") << write;
        }
    }
}

TEST_F(WriteYieldTest, ACancelAtAPauseInsideATransactionFailsTheTransaction) {
    Make("f", 400);
    for (const std::string table : {"fh", "fb"}) {
        for (const std::string write : {"UPDATE " + table + " SET v = v + 1 WHERE v >= 0",
                                        "DELETE FROM " + table + " WHERE v >= 0"}) {
            Session w;
            Analytic(w);
            ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &w).response.rfind("BEGIN", 0), 0u);
            const std::size_t mark = w.transaction()->trail().size();
            Started walk = Start(write, w);
            Pump(1);
            ASSERT_FALSE(*walk.done) << write;
            w.RequestCancel();
            TurnsToFinish(walk);
            EXPECT_EQ(walk.out->status.code(), StatusCode::kCancelled) << walk.out->response;
            EXPECT_TRUE(w.failed()) << write << ": a cancelled statement fails its transaction";
            EXPECT_FALSE(w.parked_write().has_value()) << write;
            // Its rows stay written until the client rolls back, as any
            // failed statement's do.
            ASSERT_NE(w.transaction(), nullptr) << write;
            EXPECT_GT(w.transaction()->trail().size(), mark) << write;
            EXPECT_EQ(dispatcher_->Dispatch("COMMIT", &w).response.rfind("ERR", 0), 0u);
            ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &w).response.rfind("ROLLBACK", 0), 0u);
            EXPECT_EQ(Count(table, "v = 0"), 400) << write;
            EXPECT_EQ(locks_->EntryCount(), 0u) << write;
        }
    }
}

TEST_F(WriteYieldTest, AWriteThatTakesNoPauseTakesNoCancel) {
    // A walk shorter than a slice - sixty rows, some nine leaves - gives no
    // core back and sees no flag, and neither does a write by one key; the
    // cancel stays for whoever takes it next - the KWP session at its frame.
    Make("n", 60);
    for (const std::string table : {"nh", "nb"}) {
        for (const char* where : {"v >= 0", "id = 5"}) {
            Session w;
            w.RequestCancel();
            const std::uint64_t before = dispatcher_->write_walk_pauses();
            const DispatchOutcome out =
                RunAsync("UPDATE " + table + " SET v = v + 1 WHERE " + where, w);
            EXPECT_EQ(out.response.rfind("UPDATED", 0), 0u) << out.response;
            EXPECT_EQ(dispatcher_->write_walk_pauses(), before) << table << " " << where;
            EXPECT_TRUE(w.TakeCancel()) << table << " " << where;
        }
    }
}

TEST_F(WriteYieldTest, ACancelIsTakenAtAKeyLoopsPauseToo) {
    Make("j", 100);
    std::string list;
    for (int id = 2; id <= 100; id += 2) list += (list.empty() ? "" : ", ") + std::to_string(id);
    for (const std::string table : {"jh", "jb"}) {
        Session w;
        Analytic(w);
        Started walk = Start("UPDATE " + table + " SET v = v + 1 WHERE id IN (" + list + ")", w);
        Pump(1);
        ASSERT_FALSE(*walk.done) << table;
        w.RequestCancel();
        TurnsToFinish(walk);
        EXPECT_EQ(walk.out->status.code(), StatusCode::kCancelled) << walk.out->response;
        EXPECT_EQ(Count(table, "v = 0"), 100) << table;
        EXPECT_EQ(locks_->EntryCount(), 0u) << table;
    }
}

TEST_F(WriteYieldTest, TheSynchronousPathTakesACancelAtAPauseToo) {
    Make("q", 400);
    for (const std::string table : {"qh", "qb"}) {
        Session w;
        Analytic(w);
        w.RequestCancel();
        const DispatchOutcome out =
            dispatcher_->Dispatch("UPDATE " + table + " SET v = v + 1 WHERE v >= 0", &w);
        EXPECT_EQ(out.status.code(), StatusCode::kCancelled) << out.response;
        EXPECT_FALSE(w.parked_write().has_value()) << table;
        EXPECT_EQ(Count(table, "v = 0"), 400) << table;
        EXPECT_EQ(locks_->EntryCount(), 0u) << table;
    }
}

// ---- Beside DU: a write that finds its rows by a key -------------------------
//
// `instructions/v3.2.0/workorder-du-keyed-write.md`: a write's rows are found
// through an index its WHERE enters, a heap's tuple key window through its
// Keystone index, a btree's by a walk of the window. Each is a loop of
// descents or a walk, and each gives its core back as the list's loop and
// the whole walk do.

TEST_F(WriteYieldTest, AnIndexedWritePausesBetweenDescentsAndCollectsItsKeysOnce) {
    // Every row's `v` is 0, so `v`'s index names all 120 rows and the write
    // is 120 descents: seven pauses at sixteen a slice. **Another session
    // commits a row the index names between two slices**: an index read
    // again at a resume would name 121 keys, and `ANALYZE`'s `keys=` is the
    // collection the last run wrote from, so 120 says the paused loop took
    // up its first one (`WalkCursor::collected`). The row is invisible to
    // the statement's view either way, and is left as it was placed.
    Make("i", 120);
    for (const std::string table : {"ih", "ib"}) {
        ASSERT_EQ(Local("CREATE INDEX " + table + "_v ON " + table + " (v)").rfind("CREATED", 0),
                  0u);
        Session w;
        Analytic(w);
        const std::uint64_t before = dispatcher_->write_walk_pauses();
        Started walk = Start("ANALYZE UPDATE " + table + " SET v = v + 1 WHERE v = 0", w);
        Pump(1);
        ASSERT_FALSE(*walk.done) << table << ": 120 descents took no pause";
        ASSERT_TRUE(w.parked_write().has_value()) << table;
        ASSERT_NE(w.parked_write()->cursor.collected, nullptr)
            << table << ": the loop paused with no collection to take up";
        EXPECT_EQ(w.parked_write()->cursor.collected->keys.size(), 120u) << table;
        ASSERT_EQ(Local("INSERT INTO " + table + " VALUES (1000, 0, 'p')").rfind("INSERTED", 0),
                  0u);
        TurnsToFinish(walk);
        EXPECT_EQ(dispatcher_->write_walk_pauses() - before, 7u)
            << table << ": 120 descents in slices of sixteen";
        const std::string& reply = walk.out->response;
        EXPECT_EQ(reply.rfind("analyze rows=120 ", 0), 0u) << reply;
        EXPECT_NE(reply.find(" index=" + table + "_v"), std::string::npos) << reply;
        EXPECT_NE(reply.find(" keys=120"), std::string::npos)
            << table << ": the resume read the index again: " << reply;
        EXPECT_EQ(Count(table, "v = 1"), 120) << table << ": a row was missed or written twice";
        EXPECT_EQ(Count(table, "v = 0 AND id = 1000"), 1)
            << table << ": the row placed after the statement's view was written";
        EXPECT_FALSE(w.parked_write().has_value()) << table;
        EXPECT_EQ(locks_->EntryCount(), 0u) << table;
    }
}

TEST_F(WriteYieldTest, AnIndexedWriteHeldAfterAPauseCollectsAgainAndWritesEachRowOnce) {
    // The two stops of one loop: slice boundaries, then a row an undecided
    // writer holds. A held row's park hands no collection on (DU-S2: its
    // resume collects again), and every row is still written once.
    Make("j", 120);
    for (const std::string table : {"jh", "jb"}) {
        ASSERT_EQ(Local("CREATE INDEX " + table + "_v ON " + table + " (v)").rfind("CREATED", 0),
                  0u);
        Session holder;
        ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
        ASSERT_EQ(dispatcher_->Dispatch("UPDATE " + table + " SET pad = 'q' WHERE id = 110", &holder)
                      .response.rfind("UPDATED 1", 0),
                  0u);
        Session w;
        Analytic(w);
        const std::uint64_t before = dispatcher_->write_walk_pauses();
        Started walk = Start("UPDATE " + table + " SET v = v + 1 WHERE v = 0", w);
        Pump(200);
        ASSERT_FALSE(*walk.done) << table << ": the loop passed a held row";
        EXPECT_GE(dispatcher_->write_walk_pauses() - before, 2u)
            << table << ": the loop met the held row before its first boundary";
        ASSERT_TRUE(w.parked_write().has_value()) << table;
        EXPECT_EQ(w.parked_write()->cursor.collected, nullptr)
            << table << ": a held row's park carried a collection";
        ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
        TurnsToFinish(walk);
        EXPECT_EQ(walk.out->response.rfind("UPDATED 120", 0), 0u) << walk.out->response;
        EXPECT_EQ(Count(table, "v = 1"), 120) << table;
        EXPECT_EQ(locks_->EntryCount(), 0u) << table;
    }
}

TEST_F(WriteYieldTest, AWriteOverATupleKeyWindowGivesItsCoreBackInsideTheWindow) {
    // `id BETWEEN 50 AND 350`: a btree walks the window's leaves and pauses
    // between two of them (DU-S1), a heap descends for each key its Keystone
    // index names in it and pauses between two descents. Nothing outside the
    // window is written, on either side of a pause.
    Make("n", 400);
    for (const std::string table : {"nh", "nb"}) {
        Session w;
        Analytic(w);
        const std::uint64_t before = dispatcher_->write_walk_pauses();
        Started walk =
            Start("UPDATE " + table + " SET v = v + 1 WHERE id BETWEEN 50 AND 350", w);
        Pump(1);
        ASSERT_FALSE(*walk.done) << table << ": a window of 301 rows took no pause";
        TurnsToFinish(walk);
        EXPECT_GE(dispatcher_->write_walk_pauses() - before, 2u) << table;
        EXPECT_EQ(walk.out->response.rfind("UPDATED 301", 0), 0u) << walk.out->response;
        EXPECT_EQ(Count(table, "v = 1"), 301) << table;
        EXPECT_EQ(Count(table, "v = 1 AND id < 50"), 0) << table;
        EXPECT_EQ(Count(table, "v = 1 AND id > 350"), 0) << table;
        EXPECT_EQ(Count(table, "v > 1"), 0) << table << ": a row was written twice";
        EXPECT_EQ(locks_->EntryCount(), 0u) << table;
    }
}

TEST_F(WriteYieldTest, ACancelIsTakenAtAnIndexedWritesPause) {
    Make("o", 120);
    for (const std::string table : {"oh", "ob"}) {
        ASSERT_EQ(Local("CREATE INDEX " + table + "_v ON " + table + " (v)").rfind("CREATED", 0),
                  0u);
        Session w;
        Analytic(w);
        Started walk = Start("UPDATE " + table + " SET v = v + 1 WHERE v = 0", w);
        Pump(1);
        ASSERT_FALSE(*walk.done) << table;
        w.RequestCancel();
        TurnsToFinish(walk);
        EXPECT_EQ(walk.out->status.code(), StatusCode::kCancelled) << walk.out->response;
        EXPECT_EQ(Count(table, "v = 0"), 120) << table;
        EXPECT_FALSE(w.parked_write().has_value()) << table;
        EXPECT_EQ(locks_->EntryCount(), 0u) << table;
    }
}

}  // namespace
}  // namespace kds::server
