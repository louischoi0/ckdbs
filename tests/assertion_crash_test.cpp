#include "file_rig_crash.hpp"

#include <string>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/storage/page_store_checkpoint_target.hpp"
#include "kds/wal/checkpointer.hpp"

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

// CL-S1 (`instructions/v3.2.0/workorder-cl-assertion-reservations-of-an-undecided-transaction.md`):
// a transaction no commit decided at the crash gives its reservations back at
// the mount, whether it reserved after the last checkpoint's base (the log's
// `ASSERT_RESERVE`) or before it (the base counts it, and the checkpoint's
// `ASSERT_PENDING` says whose it is). Red on the base binary (CL-S0, `b66a0dbe`).
TEST(AssertionCrashTest, AnUndecidedArrivalAtTheCrashIsGivenBack) {
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
    // The loser's row is undone, so two rows stand and one slot is free.
    EXPECT_EQ(Probe(d, 4, 1), "INSERTED");
    EXPECT_TRUE(StartsWith(Probe(d, 5, 1), "ERR ASSERTION_VIOLATION"));
}

TEST(AssertionCrashTest, AnUndecidedDepartureAtTheCrashIsGivenBack) {
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
    // The loser's delete is undone, so three rows stand and the group is full
    // (`[quiet-wrong]` before CL: a fourth was admitted).
    EXPECT_TRUE(StartsWith(Probe(d, 4, 1), "ERR ASSERTION_VIOLATION"));
}

// The same two, with a checkpoint between the loser's write and the crash: the
// checkpoint's base counts the reservation and no page entry names its
// transaction (CL-S0 item 3), so only `ASSERT_PENDING` can.
std::unique_ptr<TwoCoreRig> OpenAnchoredRig() {
    TwoCoreRig::Options options;
    options.file_backed = true;
    options.fold_anchor = true;
    auto opened = TwoCoreRig::Open(options);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    return opened.ok() ? std::move(opened.value()) : nullptr;
}

// **A checkpoint whose redo start is its own.** A page is written back only up
// to the durable log and the cadence's checkpoint leaves a statement's pages
// dirty, so its anchor's redo start stays back at the oldest of them - and a
// mount from it scans through the assertion's own create run, takes *that* as
// the base and reads the loser's `ASSERT_RESERVE` itself, never reaching
// `ASSERT_PENDING`. Here the log is made durable and every dirty page written
// back first, so the checkpoint finds nothing dirty and a mount starts at its
// `CHECKPOINT_BEGIN`: its snapshot run is the base.
bool CheckpointFromItsOwnBegin(TwoCoreRig& rig) {
    if (!rig.wal().Flush().ok()) return false;
    storage::PageStoreCheckpointTarget target(rig.store());
    std::vector<PageId> dirty;
    for (const wal::CheckpointDirtyPage& page : target.DirtyTable()) dirty.push_back(page.page_id);
    if (!target.FlushPages(dirty).ok()) return false;
    EXPECT_TRUE(target.DirtyTable().empty()) << "a page stayed dirty after its write-back";
    wal::Checkpointer checkpointer(rig.wal(), target, rig.core(0).transactions(),
                                   rig.checkpoint_anchor());
    checkpointer.SetAssertionSource(&rig.core(0).dispatcher().assertions());
    if (!checkpointer.RunToCompletion().ok()) return false;
    // The anchor in page 0 is the fold of both cores' checkpoints, at the lower
    // redo start (`expeditor.cpp`'s note on the scan's start): the peer's, taken
    // at open, would hold the scan back, so it checkpoints too.
    if (!rig.core(1).Checkpoint().ok()) return false;
    // The publishes dirtied page 0, where the anchor lives; a crash image holds
    // it only once it is written back.
    if (!rig.wal().Flush().ok()) return false;
    dirty.clear();
    for (const wal::CheckpointDirtyPage& page : target.DirtyTable()) dirty.push_back(page.page_id);
    if (!target.FlushPages(dirty).ok()) return false;
    const WalAnchorFields anchor = rig.folded_anchor();
    EXPECT_EQ(anchor.redo_start_lsn, anchor.checkpoint_lsn)
        << "the checkpoint's redo start is not its own begin";
    return anchor.redo_start_lsn == anchor.checkpoint_lsn;
}

TEST(AssertionCrashTest, AnArrivalUndecidedAcrossACheckpointIsGivenBack) {
    TempDir image;
    {
        auto rig = OpenAnchoredRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        Ok(d0, "CREATE TABLE t (id int64, grp int64) BTREE");
        Ok(d0, "INSERT INTO t VALUES (1, 1), (2, 1)");
        Ok(d0, "CREATE ASSERTION a1 ON t GROUP BY (grp) CHECK COUNT(*) <= 3");
        Session loser;
        Ok(d0, "BEGIN", &loser);
        Ok(d0, "INSERT INTO t VALUES (3, 1)", &loser);
        ASSERT_TRUE(CheckpointFromItsOwnBegin(*rig));
        ASSERT_TRUE(rig->Snapshot(image.path).ok());
    }
    auto db = Mount(image.path);
    ASSERT_TRUE(db.ok()) << db.status().message();
    CurrentCoreGuard as(0);
    CommandDispatcher& d = db.value()->dispatcher();
    EXPECT_EQ(Probe(d, 4, 1), "INSERTED");
    EXPECT_TRUE(StartsWith(Probe(d, 5, 1), "ERR ASSERTION_VIOLATION"));
}

TEST(AssertionCrashTest, ADepartureUndecidedAcrossACheckpointIsGivenBack) {
    TempDir image;
    {
        auto rig = OpenAnchoredRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        Ok(d0, "CREATE TABLE t (id int64, grp int64) BTREE");
        Ok(d0, "INSERT INTO t VALUES (1, 1), (2, 1), (3, 1)");
        Ok(d0, "CREATE ASSERTION a1 ON t GROUP BY (grp) CHECK COUNT(*) <= 3");
        Session loser;
        Ok(d0, "BEGIN", &loser);
        Ok(d0, "DELETE FROM t WHERE id = 3", &loser);
        ASSERT_TRUE(CheckpointFromItsOwnBegin(*rig));
        ASSERT_TRUE(rig->Snapshot(image.path).ok());
    }
    auto db = Mount(image.path);
    ASSERT_TRUE(db.ok()) << db.status().message();
    CurrentCoreGuard as(0);
    CommandDispatcher& d = db.value()->dispatcher();
    EXPECT_TRUE(StartsWith(Probe(d, 4, 1), "ERR ASSERTION_VIOLATION"))
        << "the base counts the loser's departure and nothing named whose it was";
}

}  // namespace
}  // namespace kds::server
