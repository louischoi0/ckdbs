#include "file_rig_crash.hpp"

#include <string>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/server/expeditor.hpp"

// An assertion across a crash with no checkpoint after its create, mounted
// through production's mount (`docs/spec/assertion.md` §7). The create's
// build entries precede its own base and its cabin page is never written
// back, so only the recovery pass's page half puts them back; without it the
// checked write's entry landed past the page's append point and every
// assertion came up unenforcing (anti-kdbs P002, case R).

namespace kds::server {
namespace {

using crash_rig::Mount;
using crash_rig::Ok;
using crash_rig::OpenFileRig;
using crash_rig::StartsWith;
using crash_rig::TempDir;

TEST(AssertionCrashTest, ACheckedWriteAfterACreateMountsEnforcingWithNoCheckpointBetween) {
    TempDir image;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        Ok(d0, "CREATE TABLE t (id int64, grp int64) BTREE");
        Ok(d0, "INSERT INTO t VALUES (1, 1), (2, 1)");
        Ok(d0, "CREATE ASSERTION a1 ON t GROUP BY (grp) CHECK COUNT(*) <= 3");
        Ok(d0, "INSERT INTO t VALUES (3, 1)");
        ASSERT_TRUE(rig->Snapshot(image.path).ok());
    }
    auto db = Mount(image.path);
    ASSERT_TRUE(db.ok()) << db.status().message();
    EXPECT_EQ(db.value()->recovery().assertions_enforcing, 1u);
    EXPECT_EQ(db.value()->recovery().assertions_unrecovered, 0u);

    CurrentCoreGuard as(0);
    CommandDispatcher& d = db.value()->dispatcher();
    const std::string over = d.Dispatch("INSERT INTO t VALUES (4, 1)").response;
    EXPECT_TRUE(StartsWith(over, "ERR ASSERTION_VIOLATION")) << "1, 2 and 3 fill group 1: " << over;
    EXPECT_TRUE(StartsWith(d.Dispatch("INSERT INTO t VALUES (4, 2)").response, "INSERTED"));
}


// CK-S0 part 3 (`instructions/v3.2.0/workorder-ck-assertion-commit-settle.md`
// §5): what a mount makes of a group total when a reservation is committed and
// when it is undecided at the crash, as an arrival and as a departure.

// The reply to an INSERT into group `grp` of `t`, as a short word.
std::string Probe(CommandDispatcher& d, int id, int grp) {
    const std::string reply =
        d.Dispatch("INSERT INTO t VALUES (" + std::to_string(id) + ", " + std::to_string(grp) + ")")
            .response;
    return StartsWith(reply, "INSERTED") ? "INSERTED" : reply.substr(0, 24);
}

TEST(AssertionCrashTest, ACommittedDepartureFreesItsSlotAcrossACrash) {
    TempDir image;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        Ok(d0, "CREATE TABLE t (id int64, grp int64) BTREE");
        Ok(d0, "INSERT INTO t VALUES (1, 1), (2, 1), (3, 1)");
        Ok(d0, "CREATE ASSERTION a1 ON t GROUP BY (grp) CHECK COUNT(*) <= 3");
        Ok(d0, "DELETE FROM t WHERE id = 3");
        ASSERT_TRUE(rig->Snapshot(image.path).ok());
    }
    auto db = Mount(image.path);
    ASSERT_TRUE(db.ok()) << db.status().message();
    CurrentCoreGuard as(0);
    CommandDispatcher& d = db.value()->dispatcher();
    EXPECT_EQ(Probe(d, 4, 1), "INSERTED") << "two rows stand, one slot is free";
    EXPECT_TRUE(StartsWith(Probe(d, 5, 1), "ERR ASSERTION_VIOLATION"));
}

// CK-R3: a commit writes no `ASSERT_COMMIT` (its replay arm stays for a log
// written before CK), and a log without one mounts to the totals a commit
// produced: the reservation counted from admission, the group full.
TEST(AssertionCrashTest, ACommitLogsNoAssertCommitAndTheMountKeepsItsTotals) {
    TempDir image;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        Ok(d0, "CREATE TABLE t (id int64, grp int64) BTREE");
        Ok(d0, "INSERT INTO t VALUES (1, 1)");
        Ok(d0, "CREATE ASSERTION a1 ON t GROUP BY (grp) CHECK COUNT(*) <= 3");
        Ok(d0, "INSERT INTO t VALUES (2, 1), (3, 1)");
        ASSERT_TRUE(rig->Snapshot(image.path).ok());
    }
    const std::optional<crash_rig::Segment> seg = crash_rig::ReadSegment(image.path / "wal");
    ASSERT_TRUE(seg.has_value());
    int reserves = 0;
    for (const wal::RecordType type : seg->types) {
        EXPECT_NE(type, wal::RecordType::kAssertCommit);
        reserves += type == wal::RecordType::kAssertReserve ? 1 : 0;
    }
    EXPECT_GE(reserves, 2) << "the committed insert reserved its two rows";

    auto db = Mount(image.path);
    ASSERT_TRUE(db.ok()) << db.status().message();
    CurrentCoreGuard as(0);
    CommandDispatcher& d = db.value()->dispatcher();
    EXPECT_TRUE(StartsWith(Probe(d, 4, 1), "ERR ASSERTION_VIOLATION"));
    EXPECT_EQ(Probe(d, 4, 2), "INSERTED");
}

// **Disabled, and red when enabled** (`--gtest_also_run_disabled_tests`): the
// two cells below state what a mount should answer and the tree does not
// (`docs/inflight/bugs/an-assertion-keeps-the-reservations-of-a-crash-loser-and-a-failed-settle.md`,
// CK §1.8). They are reproduced on the base binary (CK-S0 part 3, CK-Q5 (a)),
// not fixed by CK, and are enabled by the order that carries the fix.
TEST(AssertionCrashTest, DISABLED_AnUndecidedArrivalAtTheCrashIsGivenBack) {
    TempDir image;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        Ok(d0, "CREATE TABLE t (id int64, grp int64) BTREE");
        Ok(d0, "INSERT INTO t VALUES (1, 1), (2, 1)");
        Ok(d0, "CREATE ASSERTION a1 ON t GROUP BY (grp) CHECK COUNT(*) <= 3");
        Session loser;
        Ok(d0, "BEGIN", &loser);
        Ok(d0, "INSERT INTO t VALUES (3, 1)", &loser);
        ASSERT_TRUE(rig->Snapshot(image.path).ok());
    }
    auto db = Mount(image.path);
    ASSERT_TRUE(db.ok()) << db.status().message();
    CurrentCoreGuard as(0);
    CommandDispatcher& d = db.value()->dispatcher();
    // The loser's row is undone, so two rows stand and one slot is free. The
    // base answers both probes ERR (fails closed).
    EXPECT_EQ(Probe(d, 4, 1), "INSERTED");
    EXPECT_TRUE(StartsWith(Probe(d, 5, 1), "ERR ASSERTION_VIOLATION"));
}

TEST(AssertionCrashTest, DISABLED_AnUndecidedDepartureAtTheCrashIsGivenBack) {
    TempDir image;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        Ok(d0, "CREATE TABLE t (id int64, grp int64) BTREE");
        Ok(d0, "INSERT INTO t VALUES (1, 1), (2, 1), (3, 1)");
        Ok(d0, "CREATE ASSERTION a1 ON t GROUP BY (grp) CHECK COUNT(*) <= 3");
        Session loser;
        Ok(d0, "BEGIN", &loser);
        Ok(d0, "DELETE FROM t WHERE id = 3", &loser);
        ASSERT_TRUE(rig->Snapshot(image.path).ok());
    }
    auto db = Mount(image.path);
    ASSERT_TRUE(db.ok()) << db.status().message();
    CurrentCoreGuard as(0);
    CommandDispatcher& d = db.value()->dispatcher();
    // The loser's delete is undone, so three rows stand and the group is
    // full. The base admits a fourth (fails open, `[quiet-wrong]`).
    EXPECT_TRUE(StartsWith(Probe(d, 4, 1), "ERR ASSERTION_VIOLATION"));
}

}  // namespace
}  // namespace kds::server
