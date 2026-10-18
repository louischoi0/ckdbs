// BX-S3 (`instructions/v3.2.0/workorder-bx-unique-index.md`): a unique
// index across two cores. Every write on the two-core rig runs through
// `DispatchAsync`, so a busy verdict is a real park on the holder's decide
// (BX-Q3 (a)) rather than the retryable refusal a synchronous dispatch gives
// (`index_unique_test.cpp`).
//
// Each cell scripts two sessions statement by statement - one on each core -
// and releases each statement only once the cell has seen the one before it
// end, so the interleaving is forced rather than raced. Repeat the suite
// (`--gtest_repeat`) to see one interleaving many times; a single run of a
// concurrency cell proves little.

#include "file_rig_crash.hpp"
#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/session.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

// One session's statements, each started only once the cell allows it.
struct Script {
    Session session;
    std::vector<std::string> statements;
    std::vector<DispatchOutcome> outs;
    std::atomic<std::size_t> allowed{0};
    std::atomic<std::size_t> started{0};
    std::atomic<std::size_t> finished{0};
    std::function<bool()> gate;
};

sched::Coro RunScript(CommandDispatcher& d, Script& s) {
    for (std::size_t i = 0; i < s.statements.size(); ++i) {
        s.gate = [&s, i] { return s.allowed.load(std::memory_order_acquire) > i; };
        co_await sched::WaitUntil{&s.gate};
        s.started.store(i + 1, std::memory_order_release);
        co_await d.DispatchAsync(s.statements[i], &s.session, &s.outs[i]);
        s.finished.store(i + 1, std::memory_order_release);
    }
    co_return Status::OK();
}

class UniqueRig {
public:
    UniqueRig() {
        auto opened = TwoCoreRig::Open();
        EXPECT_TRUE(opened.ok()) << opened.status().message();
        if (opened.ok()) rig = std::move(opened.value());
    }
    ~UniqueRig() {
        if (rig != nullptr) rig->Stop();
    }

    // Before `Start`, synchronously on core 0: no core owns the relation.
    bool Seed(const std::vector<std::string>& statements) {
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        for (const std::string& sql : statements) {
            const std::string out = d0.Dispatch(sql).response;
            if (out.rfind("ERR", 0) == 0) {
                ADD_FAILURE() << sql << " -> " << out;
                return false;
            }
        }
        return rig->store().Flush().ok();
    }

    void Start(std::vector<std::string> on_core0, std::vector<std::string> on_core1) {
        a.statements = std::move(on_core0);
        a.outs.resize(a.statements.size());
        b.statements = std::move(on_core1);
        b.outs.resize(b.statements.size());
        rig->core(0).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, RunScript(rig->core(0).dispatcher(), a)));
        rig->core(1).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, RunScript(rig->core(1).dispatcher(), b)));
        rig->Start();
    }

    // Lets statement `i` of `s` run, on `core`, and answers whether it ended
    // within `limit`. A statement expected to park is asked with a short one.
    bool Step(Script& s, std::uint32_t core, std::size_t i,
              std::chrono::milliseconds limit = 2000ms) {
        s.allowed.store(i + 1, std::memory_order_release);
        return KickUntil(*rig, core,
                         [&] { return s.finished.load(std::memory_order_acquire) > i; }, limit);
    }

    // The statement already allowed: has it ended now?
    bool Ended(Script& s, std::uint32_t core, std::size_t i,
               std::chrono::milliseconds limit = 4000ms) {
        return KickUntil(*rig, core,
                         [&] { return s.finished.load(std::memory_order_acquire) > i; }, limit);
    }

    // Statement `i` of `s`, allowed, has parked: it started, and 150 ms of
    // kicks later it has not ended. Starting is the positive sign - not
    // ending alone would also hold for a statement that never ran.
    bool Parks(Script& s, std::uint32_t core, std::size_t i) {
        s.allowed.store(i + 1, std::memory_order_release);
        if (!KickUntil(*rig, core, [&] { return s.started.load(std::memory_order_acquire) > i; })) {
            return false;
        }
        return !KickUntil(*rig, core,
                          [&] { return s.finished.load(std::memory_order_acquire) > i; }, 150ms);
    }

    // The scripts before the rig: reverse destruction tears the reactors'
    // coroutine frames down before the scripts they borrow.
    Script a;
    Script b;
    std::unique_ptr<TwoCoreRig> rig;
};

// A unique refusal: `AlreadyExists` renders with no token of its own, so the
// reply is matched on the refusal's words.
bool HeldReply(const std::string& reply) {
    return reply.rfind("ERR ", 0) == 0 &&
           reply.find("is already held by tuple key") != std::string::npos;
}

const std::vector<std::string> kSeed = {
    "CREATE TABLE t (id int64, owner int64, note int64) BTREE",
    "CREATE UNIQUE INDEX by_owner ON t (owner)",
};

// A busy key parks the second writer; the holder's decide answers it.
void InsertMeetsAnUndecidedInsert(const char* decide, const char* expected_prefix) {
    UniqueRig r;
    ASSERT_NE(r.rig, nullptr);
    ASSERT_TRUE(r.Seed(kSeed));
    r.Start({"BEGIN", "INSERT INTO t VALUES (7, 0)", decide}, {"INSERT INTO t VALUES (7, 1)"});
    ASSERT_TRUE(r.Step(r.a, 0, 0));
    ASSERT_TRUE(r.Step(r.a, 0, 1));
    ASSERT_EQ(r.a.outs[1].response.rfind("INSERTED", 0), 0u) << r.a.outs[1].response;

    ASSERT_TRUE(r.Parks(r.b, 1, 0)) << "the second writer did not wait for the first";

    ASSERT_TRUE(r.Step(r.a, 0, 2)) << r.a.outs[2].response;
    ASSERT_TRUE(r.Ended(r.b, 1, 0)) << "the waiter never proceeded after the decide";
    if (expected_prefix != nullptr) {
        EXPECT_EQ(r.b.outs[0].response.rfind(expected_prefix, 0), 0u) << r.b.outs[0].response;
    } else {
        EXPECT_TRUE(HeldReply(r.b.outs[0].response)) << r.b.outs[0].response;
    }
}

TEST(UniqueIndexRigTest, AnInsertWaitsOutAnUndecidedInsertOfItsKeyAndIsRefusedAtItsCommit) {
    InsertMeetsAnUndecidedInsert("COMMIT", nullptr);
}

TEST(UniqueIndexRigTest, AnInsertWaitsOutAnUndecidedInsertOfItsKeyAndPassesAtItsRollback) {
    InsertMeetsAnUndecidedInsert("ROLLBACK", "INSERTED");
}

// An undecided writer that frees the key - an UPDATE moving the row off it,
// or a DELETE, which takes no key unit (BX-R5) - parks an insert of the key:
// its commit frees the key, its rollback leaves it held.
void InsertMeetsAnUndecidedRelease(const char* release, const char* decide) {
    UniqueRig r;
    ASSERT_NE(r.rig, nullptr);
    ASSERT_TRUE(r.Seed({kSeed[0], kSeed[1], "INSERT INTO t VALUES (7, 0)"}));
    r.Start({"BEGIN", release, decide}, {"INSERT INTO t VALUES (7, 1)"});
    ASSERT_TRUE(r.Step(r.a, 0, 0));
    ASSERT_TRUE(r.Step(r.a, 0, 1));
    ASSERT_EQ(r.a.outs[1].response.rfind("ERR", 0), std::string::npos) << r.a.outs[1].response;

    ASSERT_TRUE(r.Parks(r.b, 1, 0)) << "the insert did not wait for the release's decide";
    ASSERT_TRUE(r.Step(r.a, 0, 2));
    ASSERT_TRUE(r.Ended(r.b, 1, 0));
    if (std::string(decide) == "COMMIT") {
        EXPECT_EQ(r.b.outs[0].response.rfind("INSERTED", 0), 0u) << r.b.outs[0].response;
    } else {
        EXPECT_TRUE(HeldReply(r.b.outs[0].response)) << r.b.outs[0].response;
    }
}

TEST(UniqueIndexRigTest, AnInsertWaitsOutAnUndecidedMoveOffItsKeyAndPassesAtItsCommit) {
    InsertMeetsAnUndecidedRelease("UPDATE t SET owner = 8 WHERE owner = 7", "COMMIT");
}

TEST(UniqueIndexRigTest, AnInsertWaitsOutAnUndecidedMoveOffItsKeyAndIsRefusedAtItsRollback) {
    InsertMeetsAnUndecidedRelease("UPDATE t SET owner = 8 WHERE owner = 7", "ROLLBACK");
}

TEST(UniqueIndexRigTest, AnInsertWaitsOutAnUndecidedDeleteOfItsKeyAndPassesAtItsCommit) {
    InsertMeetsAnUndecidedRelease("DELETE FROM t WHERE owner = 7", "COMMIT");
}

TEST(UniqueIndexRigTest, AnInsertWaitsOutAnUndecidedDeleteOfItsKeyAndIsRefusedAtItsRollback) {
    InsertMeetsAnUndecidedRelease("DELETE FROM t WHERE owner = 7", "ROLLBACK");
}

TEST(UniqueIndexRigTest, AMultiRowInsertsLaterRowMeetingABusyKeyIsRefusedNotParked) {
    // BX-Q3 (a): row 2 cannot wait - the statement has written row 1 - so it
    // is refused retryably, and autocommit takes row 1 back with it.
    UniqueRig r;
    ASSERT_NE(r.rig, nullptr);
    ASSERT_TRUE(r.Seed(kSeed));
    r.Start({"BEGIN", "INSERT INTO t VALUES (7, 0)", "ROLLBACK"},
            {"INSERT INTO t VALUES (5, 1), (7, 1)", "INSERT INTO t VALUES (5, 2)"});
    ASSERT_TRUE(r.Step(r.a, 0, 0));
    ASSERT_TRUE(r.Step(r.a, 0, 1));
    // Inside the lock family's 1 s fault net, and with the busy refusal's own
    // words: a parked row 2 the net then refused would also be `TxnConflict`.
    ASSERT_TRUE(r.Step(r.b, 1, 0, 800ms)) << "row 2 parked where it cannot be resumed";
    EXPECT_EQ(StatusFromErrorReply(r.b.outs[0].response).code(), StatusCode::kTxnConflict)
        << r.b.outs[0].response;
    EXPECT_NE(r.b.outs[0].response.find("is being written by"), std::string::npos)
        << r.b.outs[0].response;
    ASSERT_TRUE(r.Step(r.b, 1, 1));
    EXPECT_EQ(r.b.outs[1].response.rfind("INSERTED", 0), 0u)
        << "row 1 outlived its refused statement: " << r.b.outs[1].response;
    ASSERT_TRUE(r.Step(r.a, 0, 2));
}

// An UPDATE that has written a row parks mid-walk on a busy key and resumes
// at the row it stopped on (AO-S3b, BX-R4).
void WalkParksMidStatement(const char* decide, const char* expected,
                           const char* rows_after) {
    UniqueRig r;
    ASSERT_NE(r.rig, nullptr);
    ASSERT_TRUE(r.Seed({kSeed[0], kSeed[1], "INSERT INTO t VALUES (1, 0), (2, 0)"}));
    r.Start({"BEGIN", "INSERT INTO t VALUES (20, 9)", decide},
            {"UPDATE t SET owner = owner * 10 WHERE note = 0",
             "SELECT id, owner FROM t WHERE note = 0"});
    ASSERT_TRUE(r.Step(r.a, 0, 0));
    ASSERT_TRUE(r.Step(r.a, 0, 1));
    // Row 1 (1 -> 10) is written; row 2 (2 -> 20) meets the open insert. A
    // re-run from the top instead of a resume would read row 1's own 10 and
    // write 100, which the exact rows below would show.
    ASSERT_TRUE(r.Parks(r.b, 1, 0)) << "the walk did not park on the busy key";
    ASSERT_TRUE(r.Step(r.a, 0, 2));
    ASSERT_TRUE(r.Ended(r.b, 1, 0)) << "the parked walk never resumed";
    if (expected != nullptr) {
        EXPECT_EQ(r.b.outs[0].response, expected);
    } else {
        EXPECT_TRUE(HeldReply(r.b.outs[0].response)) << r.b.outs[0].response;
    }
    // Autocommit: a refused walk takes row 1 back with it.
    ASSERT_TRUE(r.Step(r.b, 1, 1));
    EXPECT_EQ(r.b.outs[1].response, rows_after);
}

TEST(UniqueIndexRigTest, AWalkParkedOnABusyKeyResumesAndWritesAtTheHoldersRollback) {
    WalkParksMidStatement("ROLLBACK", "UPDATED 2", "id,owner\\n1,10\\n2,20");
}

TEST(UniqueIndexRigTest, AWalkParkedOnABusyKeyIsRefusedAtTheHoldersCommit) {
    WalkParksMidStatement("COMMIT", nullptr, "id,owner\\n1,1\\n2,2");
}

TEST(UniqueIndexRigTest, AWriterPausedBetweenPlacementAndAppendHoldsItsKeyAgainstTheOther) {
    // The window the key unit exists for (BX-R3, BX-Q2 (a)): writer A's row
    // is placed and its entry not yet appended, so a check by writer B finds
    // nothing in the run. Only A's unit keeps B out; without it both pass.
    // B names a tuple key in another leaf than A's, so no page latch A holds
    // could stop B in its place.
    // Declared before the rig, so the hook core 0 holds outlives neither.
    std::atomic<bool> paused{false};
    std::atomic<bool> release{false};
    UniqueRig r;
    ASSERT_NE(r.rig, nullptr);
    // Named tuple keys from 1001, so B's named key 5 is free and sorts into
    // the leftmost leaf, and A's issued one lands past 1800 in the rightmost.
    std::vector<std::string> seed = kSeed;
    for (int i = 0; i < 4; ++i) {
        std::string rows = "INSERT INTO t VALUES ";
        for (int j = 0; j < 200; ++j) {
            const int n = 1001 + i * 200 + j;
            rows += (j == 0 ? "(" : ", (") + std::to_string(n) + ", " + std::to_string(n) +
                    ", 0)";
        }
        seed.push_back(rows);
    }
    ASSERT_TRUE(r.Seed(seed));
    // Declared after the rig, so it is destroyed first: however the cell
    // leaves - a failed assertion included - core 0's hook is released
    // before the rig's stop waits on that core's reactor.
    struct ReleaseOnExit {
        std::atomic<bool>& release;
        ~ReleaseOnExit() { release.store(true, std::memory_order_release); }
    } release_on_exit{release};
    r.rig->core(0).dispatcher().SetAfterPlacementForTest([&]() -> Status {
        if (!paused.exchange(true)) {
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(1ms);
            }
        }
        return Status::OK();
    });
    r.Start({"INSERT INTO t VALUES (7, 0)"}, {"INSERT INTO t VALUES (5, 7, 1)"});
    r.a.allowed.store(1, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*r.rig, 0, [&] { return paused.load(std::memory_order_acquire); }));

    ASSERT_TRUE(r.Parks(r.b, 1, 0))
        << "the second writer was not held off while the first's entry was missing";
    release.store(true, std::memory_order_release);
    ASSERT_TRUE(r.Ended(r.a, 0, 0));
    EXPECT_EQ(r.a.outs[0].response.rfind("INSERTED", 0), 0u) << r.a.outs[0].response;
    ASSERT_TRUE(r.Ended(r.b, 1, 0));
    EXPECT_TRUE(HeldReply(r.b.outs[0].response)) << r.b.outs[0].response;
}

TEST(UniqueIndexRigTest, ARefusedUniqueBuildGivesItsPartialTreeBackEveryTime) {
    // BX-Q6 (b): the duplicate is the relation's last row, so the build has
    // grown a tree of several levels when it is refused. Its pages were never
    // logged and nothing names them, so they go back - and a client repeating
    // the statement does not grow the file.
    UniqueRig r;
    ASSERT_NE(r.rig, nullptr);
    std::vector<std::string> seed = {kSeed[0]};
    for (int i = 0; i < 10; ++i) {
        std::string rows = "INSERT INTO t VALUES ";
        for (int j = 0; j < 200; ++j) {
            rows += (j == 0 ? "(" : ", (") + std::to_string(i * 200 + j) + ", 0)";
        }
        seed.push_back(rows);
    }
    seed.push_back("INSERT INTO t VALUES (0, 1)");
    ASSERT_TRUE(r.Seed(seed));

    CommandDispatcher& d0 = r.rig->core(0).dispatcher();
    const std::uint32_t before = r.rig->store().allocated_pages();
    const std::uint64_t freed_before = r.rig->store().pages_freed();
    for (int attempt = 0; attempt < 3; ++attempt) {
        const std::string out = d0.Dispatch(kSeed[1]).response;
        ASSERT_NE(out.find("is held by both tuple key"), std::string::npos) << out;
        EXPECT_EQ(r.rig->store().allocated_pages(), before)
            << "attempt " << attempt << " left its partial tree allocated";
    }
    EXPECT_GT(r.rig->store().pages_freed() - freed_before, 3u)
        << "a tree of one page would pass the count without the walk";
}

TEST(UniqueIndexRigTest, TwoWritersEachWaitingOnTheOthersKeyAreADeadlockAndOneIsRefused) {
    UniqueRig r;
    ASSERT_NE(r.rig, nullptr);
    ASSERT_TRUE(r.Seed(kSeed));
    r.Start({"BEGIN", "INSERT INTO t VALUES (7, 0)", "INSERT INTO t VALUES (8, 0)", "ROLLBACK"},
            {"BEGIN", "INSERT INTO t VALUES (8, 1)", "INSERT INTO t VALUES (7, 1)", "ROLLBACK"});
    ASSERT_TRUE(r.Step(r.a, 0, 0));
    ASSERT_TRUE(r.Step(r.a, 0, 1));
    ASSERT_TRUE(r.Step(r.b, 1, 0));
    ASSERT_TRUE(r.Step(r.b, 1, 1));
    // a waits for b's 8; b's wait for a's 7 closes the cycle and is refused.
    ASSERT_TRUE(r.Parks(r.a, 0, 2)) << "a did not wait for b's key";
    ASSERT_TRUE(r.Step(r.b, 1, 2)) << "the cycle's closer waited instead of being refused";
    EXPECT_NE(r.b.outs[2].response.find("deadlock"), std::string::npos) << r.b.outs[2].response;
    ASSERT_TRUE(r.Step(r.b, 1, 3));
    ASSERT_TRUE(r.Ended(r.a, 0, 2)) << "the survivor never proceeded after the victim's rollback";
    EXPECT_EQ(r.a.outs[2].response.rfind("INSERTED", 0), 0u) << r.a.outs[2].response;
    ASSERT_TRUE(r.Step(r.a, 0, 3));
}

// ---- Across a crash (BX-R8, BX-R9) ---------------------------------------
//
// The mount replays committed state and rolls the losers back; it checks
// nothing (BX-R9). What these cells pin is that the flag, the tree a unique
// build logged and the entries of committed writes all survive, so the
// mounted instance enforces exactly the keys committed rows hold - and none a
// rolled-back writer held.

TEST(UniqueIndexCrashTest, TheMountEnforcesWhatCommittedRowsHoldAndNothingALoserHeld) {
    crash_rig::TempDir snap;
    {
        auto rig = crash_rig::OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CommandDispatcher& d1 = rig->core(1).dispatcher();
        CurrentCoreGuard as(0);
        crash_rig::Ok(d0, "CREATE TABLE t (id int64, owner int64) BTREE");
        // Built over rows, so its tree reaches the log as images (RV3).
        crash_rig::Ok(d0, "INSERT INTO t VALUES (7), (8)");
        crash_rig::Ok(d0, "CREATE UNIQUE INDEX by_owner ON t (owner)");
        crash_rig::Ok(d0, "UPDATE t SET owner = 9 WHERE owner = 8");
        crash_rig::Ok(d0, "INSERT INTO t VALUES (8)");
        // A loser on the other core: its key is held when the crash comes.
        Session open;
        {
            CurrentCoreGuard on1(1);
            crash_rig::Ok(d1, "BEGIN", &open);
            crash_rig::Ok(d1, "INSERT INTO t VALUES (10)", &open);
        }
        ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    }

    auto mounted = crash_rig::Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << mounted.status().message();
    CommandDispatcher& d = mounted.value()->dispatcher();
    for (const char* held : {"INSERT INTO t VALUES (7)", "INSERT INTO t VALUES (8)",
                             "INSERT INTO t VALUES (9)"}) {
        EXPECT_TRUE(HeldReply(d.Dispatch(held).response)) << held;
    }
    EXPECT_EQ(d.Dispatch("INSERT INTO t VALUES (10)").response.rfind("INSERTED", 0), 0u)
        << "the loser's key outlived its rollback";
    EXPECT_EQ(crash_rig::Ids(d, "SELECT id FROM t WHERE owner = 9").size(), 1u);
}

}  // namespace
}  // namespace kds::server
