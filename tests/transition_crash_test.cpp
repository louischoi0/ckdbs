#include "file_rig_crash.hpp"

#include <string>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/server/expeditor.hpp"
#include "view_text.hpp"

// Transition across a crash - BV-S3
// (`instructions/v3.2.0/workorder-bv-transition-constraints.md`). A
// Transition is its `sys.transitions` row (BV-R1), logged like any catalog
// write at `kNoTxnId` and replayed by redo, so a mount comes up with the
// declaration or without it - never half - and whatever it comes up with is
// what the write path enforces. Each cell cuts the log after every record
// the statement added and mounts each cut the way a restart does.

namespace kds::server {
namespace {

using crash_rig::CutEverywhere;
using crash_rig::Ok;
using crash_rig::OpenFileRig;
using crash_rig::StartsWith;
using crash_rig::TempDir;

constexpr const char* kOrderStatus =
    "CREATE TRANSITION order_status ON orders(status) "
    "ON INSERT ('NEW') "
    "ON UPDATE ('NEW' => 'CHECKED', 'CHECKED' => 'FINALIZED') "
    "ON DELETE FROM ('NEW')";

// Whether the mounted instance holds the Transition, and - the cell's point
// - that the write path agrees with the catalog about it.
bool DeclaredAndEnforced(Expeditor& db, const std::string& where) {
    CommandDispatcher& d = db.dispatcher();
    const bool declared =
        testing_view::ReadViewText(d.Dispatch("SHOW TRANSITIONS").response).rows.size() == 1;
    const std::string probe = d.Dispatch("INSERT INTO orders VALUES ('CHECKED')").response;
    EXPECT_EQ(StartsWith(probe, "ERR TRANSITION_VIOLATION"), declared)
        << where << ": declared=" << declared << " but the write answered " << probe;
    return declared;
}

TEST(TransitionCrashTest, ACreateCutAnywhereMountsWhole) {
    TempDir before;
    TempDir after;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        Ok(d0, "CREATE TABLE orders (id int64, status varchar(32)) BTREE");
        Ok(d0, "INSERT INTO orders VALUES ('NEW')");
        ASSERT_TRUE(rig->Snapshot(before.path).ok());
        Ok(d0, kOrderStatus);
        ASSERT_TRUE(rig->Snapshot(after.path).ok());
    }
    bool last = false;
    CutEverywhere(before.path, after.path, [&](Expeditor& db, bool, const std::string& where) {
        CurrentCoreGuard as(0);
        last = DeclaredAndEnforced(db, where);
    });
    EXPECT_TRUE(last) << "the whole log did not carry the declaration";
}

TEST(TransitionCrashTest, AnUpdateCutAnywhereLeavesALegalStateAndTheRuleStands) {
    TempDir before;
    TempDir after;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        Ok(d0, "CREATE TABLE orders (id int64, status varchar(32)) BTREE");
        Ok(d0, kOrderStatus);
        Ok(d0, "INSERT INTO orders VALUES (1, 'NEW')");
        ASSERT_TRUE(rig->Snapshot(before.path).ok());
        Ok(d0, "UPDATE orders SET status = 'CHECKED' WHERE id = 1");
        ASSERT_TRUE(rig->Snapshot(after.path).ok());
    }
    CutEverywhere(before.path, after.path, [&](Expeditor& db, bool committed,
                                                const std::string& where) {
        CurrentCoreGuard as(0);
        CommandDispatcher& d = db.dispatcher();
        const std::string row = d.Dispatch("SELECT status FROM orders WHERE id = 1").response;
        const bool checked = row.find("CHECKED") != std::string::npos;
        EXPECT_TRUE(checked || row.find("NEW") != std::string::npos) << where << ": " << row;
        if (committed) EXPECT_TRUE(checked) << where << ": committed, and " << row;
        // The rule still holds over whichever state survived: NEW cannot
        // skip to FINALIZED, CHECKED can take its edge.
        const std::string next =
            d.Dispatch("UPDATE orders SET status = 'FINALIZED' WHERE id = 1").response;
        EXPECT_EQ(StartsWith(next, "UPDATED"), checked) << where << ": " << next;
        EXPECT_TRUE(DeclaredAndEnforced(db, where));
    });
}

TEST(TransitionCrashTest, ADropCutAnywhereMountsWithTheRuleOrWithout) {
    TempDir before;
    TempDir after;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        Ok(d0, "CREATE TABLE orders (id int64, status varchar(32)) BTREE");
        Ok(d0, kOrderStatus);
        // Something after the declaration for the drop's cuts to land past.
        Ok(d0, "INSERT INTO orders VALUES ('NEW')");
        ASSERT_TRUE(rig->Snapshot(before.path).ok());
        Ok(d0, "DROP TRANSITION order_status");
        Ok(d0, "INSERT INTO orders VALUES ('NEW')");
        ASSERT_TRUE(rig->Snapshot(after.path).ok());
    }
    bool last = true;
    CutEverywhere(before.path, after.path, [&](Expeditor& db, bool, const std::string& where) {
        CurrentCoreGuard as(0);
        last = DeclaredAndEnforced(db, where);
    });
    EXPECT_FALSE(last) << "the whole log did not carry the drop";
}

}  // namespace
}  // namespace kds::server
