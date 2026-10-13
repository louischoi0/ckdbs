#include <string>

#include <gtest/gtest.h>

#include "kds/base/status.hpp"
#include "kds/txn/undo_page.hpp"
#include "sim/instance.hpp"

// `docs/inflight/bugs/a-fetch-refused-after-a-page-write-leaves-the-mutation-
// half-done.md`: a mutation that wrote a page and then failed must leave a
// volume recovery can mount and an answer rollback can stand behind.

namespace kds::sim {
namespace {

int CountRows(SimInstance& db, const std::string& table) {
    const std::string reply = db.Execute("SELECT COUNT(*) FROM " + table);
    // A debug-text reply escapes its row separator as a backslash and an `n`.
    const auto newline = reply.find("\\n");
    if (newline == std::string::npos) return -1;
    return std::stoi(reply.substr(newline + 2));
}

TEST(HalfDoneMutationTest, ASpillWhoseUndoRecordFailsIsStillLoggedAndTheVolumeMounts) {
    // The append wrote the var-heap page - its slot, and on the chain's first
    // spill the page and the link to it - before its undo record failed. Left
    // unlogged, the next spill logs the following slot against a page redo
    // never built, and redo refuses the mount (`varheap.cpp`'s dense-slot
    // check). Killed by returning before `LogSpill` when the undo append fails.
    auto created = SimInstance::Create();
    ASSERT_TRUE(created.ok()) << created.status().message();
    SimInstance& db = *created.value();
    const std::string spilled(300, 'v');  // past the instance's inline width
    const auto insert = "INSERT INTO t VALUES (1, '" + spilled + "')";

    ASSERT_EQ(db.Execute("CREATE TABLE t (id int64, v int64, name varchar)").rfind("ERR", 0),
              std::string::npos);
    ASSERT_TRUE(db.RunCheckpoint().ok());

    bool failed_once = false;
    db.txn_manager().SetBeforeUndoAppendForTest([&](const txn::UndoRecordFields& f) {
        if (failed_once ||
            f.type != static_cast<std::uint8_t>(txn::UndoRecordType::kVarHeapAppend)) {
            return Status::OK();
        }
        failed_once = true;
        return Status::IoError("injected undo-tail failure");
    });
    const std::string refused = db.Execute(insert);
    ASSERT_TRUE(failed_once) << "no spill reached the undo log";
    EXPECT_EQ(refused.rfind("ERR", 0), 0u) << refused;

    for (int i = 0; i < 3; ++i) {
        const std::string reply = db.Execute(insert);
        ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
    }
    EXPECT_EQ(CountRows(db, "t"), 3);

    db.Crash();  // nothing written back since the checkpoint: redo rebuilds the page
    const Status rebooted = db.Reboot();
    ASSERT_TRUE(rebooted.ok()) << rebooted.message();
    EXPECT_EQ(CountRows(db, "t"), 3);
    const std::string read = db.Execute("SELECT name FROM t");
    EXPECT_NE(read.find(spilled), std::string::npos) << read;
}

// A heap insert that fails after its placement and before its record: the
// row is taken back under the tail's hold, and a chain the insert grew keeps
// its new page, logged. Killed by the take-back skipping a heap (the row
// survives the abort and the next insert's slot refuses the mount) and by
// it skipping the growth's records (the next insert lands in a page redo
// never built).
void TakeBackOnAHeap(const std::string& columns, const std::string& values) {
    auto created = SimInstance::Create();
    ASSERT_TRUE(created.ok()) << created.status().message();
    SimInstance& db = *created.value();
    const std::string insert = "INSERT INTO t VALUES (" + values + ")";

    ASSERT_EQ(db.Execute("CREATE TABLE t (" + columns + ") HEAP").substr(0, 7), "CREATED");
    for (int i = 0; i < 2; ++i) ASSERT_EQ(db.Execute(insert).rfind("ERR", 0), std::string::npos);
    ASSERT_TRUE(db.RunCheckpoint().ok());

    db.dispatcher().SetAfterPlacementForTest(
        [] { return Status::IoError("injected failure after placement"); });
    const std::string refused = db.Execute(insert);
    EXPECT_EQ(refused.rfind("ERR", 0), 0u) << refused;
    db.dispatcher().SetAfterPlacementForTest(nullptr);

    for (int i = 0; i < 2; ++i) {
        const std::string reply = db.Execute(insert);
        ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
    }
    EXPECT_EQ(CountRows(db, "t"), 4) << "the refused row survived";

    db.Crash();  // nothing written back since the checkpoint: redo rebuilds the tail
    const Status rebooted = db.Reboot();
    ASSERT_TRUE(rebooted.ok()) << rebooted.message();
    EXPECT_EQ(CountRows(db, "t"), 4);
}

TEST(HalfDoneMutationTest, AHeapInsertFailedAfterPlacementIsTakenBack) {
    TakeBackOnAHeap("id int64, v int64", "7");
}

TEST(HalfDoneMutationTest, AHeapInsertThatGrewTheChainIsTakenBackAndTheGrowthLogged) {
    // One ~6 KB row per page, so every insert grows the chain.
    TakeBackOnAHeap("id int64, a char(2000), b char(2000), c char(2000)", "'x', 'y', 'z'");
}

}  // namespace
}  // namespace kds::sim
