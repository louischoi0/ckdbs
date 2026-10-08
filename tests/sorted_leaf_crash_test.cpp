#include "frame_budget_override.hpp"
#include "file_rig_crash.hpp"
#include "two_core_rig.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

#include "tree_structure.hpp"

// **A crash across sorted placement and a split** (BD-S1,
// `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md` §1.2, §1.8,
// BD-R3 E2/E5, BD-R12).
//
// Each cell runs statements on a file-backed rig, takes what a crash at that
// instant leaves and mounts it through production (`file_rig_crash.hpp`).
//
// **A log cut is made by hand.** `MemoryLogDevice::Crash()` and the sim cut
// only at a sync, so neither ends the log between two records of one
// statement (`sim/faults.hpp`). `CutEverywhere` takes the snapshot before a
// statement and the one after, and for every record boundary the statement
// added - the full log included - mounts the **before** snapshot's data file
// beside the **after** snapshot's log, zeroed from that boundary on. Zeroes
// are slack, the clean end a scan stops at (`RecordReader::Next`), and every
// page in the before data file is at or below the before log's end, so the
// image is one a crash can leave. Every cut inside a split must leave the
// tree whole: every committed row once, where a descent finds it, every leaf
// reached by both the descent and the sibling chain, and every leaf in key
// order.

namespace kds::server {
namespace {

namespace fs = std::filesystem;

using crash_rig::CutEverywhere;
using crash_rig::Ids;
using crash_rig::Leaves;
using crash_rig::Mount;
using crash_rig::Ok;
using crash_rig::OpenFileRig;
using crash_rig::ReadSegment;
using crash_rig::Segment;
using crash_rig::StartsWith;
using crash_rig::TempDir;

// The mounted relation holds exactly `want`, the walk yields it in key
// order, a descent finds a sample of it - every tenth row and the extremes,
// which crosses every leaf boundary of these cells - and the tree is whole.
void ExpectRelation(Expeditor& db, const std::set<std::uint64_t>& want, const std::string& where) {
    const std::vector<std::uint64_t> walked = Ids(db.dispatcher(), "SELECT id FROM t");
    EXPECT_EQ(walked, std::vector<std::uint64_t>(want.begin(), want.end()))
        << where << ": the walk is not the committed rows in key order";
    std::size_t k = 0;
    for (std::uint64_t id : want) {
        if (k++ % 10 != 0 && id != *want.rbegin()) continue;
        EXPECT_EQ(Ids(db.dispatcher(), "SELECT id FROM t WHERE id = " + std::to_string(id)),
                  std::vector<std::uint64_t>{id})
            << where << ": a descent does not find " << id;
    }
    auto oid = db.catalog().FindTableOidByName("t");
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto access = db.catalog().InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok()) << access.status().message();
    testing_race::ExpectBtreeSeparatorsBoundTheirSubtrees(db.store(), access.value()->desc_page_id);
}

// ---- The log, cut at a record boundary ------------------------------------
//
// `crash_rig::ReadSegment`, `CopyCut` and `CutEverywhere` (`file_rig_crash.hpp`).

// The most pages any BTREE_SPLIT in `wal_dir`'s log names: a leaf's divide
// writes two leaves and a parent, and a divide of the parent at least two
// more.
std::size_t WidestSplit(const fs::path& wal_dir) {
    const std::optional<Segment> seg = ReadSegment(wal_dir);
    if (!seg.has_value()) return 0;
    std::ifstream in(seg->file, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(raw.data()),
                                           raw.size());
    std::size_t widest = 0;
    for (std::size_t k = 0; k < seg->starts.size(); ++k) {
        if (seg->types[k] != wal::RecordType::kBtreeSplit) continue;
        auto record = wal::DecodeRecord(bytes.subspan(seg->starts[k]));
        if (!record.ok()) continue;
        auto images = wal::DecodeBtreeSplit(record.value().payload);
        if (images.ok()) widest = std::max(widest, images.value().size());
    }
    return widest;
}

// `rows` committed rows at ids 10, 20, ..., ascending: 198 fill a leaf of
// `t (id int64, v int64)`, so the default 400 leave two full leaves and a
// third with four rows.
std::set<std::uint64_t> Fill(CommandDispatcher& d0, std::uint64_t rows = 400) {
    std::set<std::uint64_t> ids;
    for (std::uint64_t k = 1; k <= rows; ++k) {
        Ok(d0, "INSERT INTO t VALUES (" + std::to_string(k * 10) + ", " + std::to_string(k) + ")");
        ids.insert(k * 10);
    }
    return ids;
}

// ---- E5: an unlogged placement, then a logged insert, then the crash ------

TEST(SortedLeafCrashTest, ARowPlacedAndNeverLoggedLeavesNoHoleForTheNextRecordsRedo) {
    // E5: a row is placed, its index maintenance fails, and it is never
    // logged - the seam fails that exit. The next insert's record names the
    // slot after it, which redo, never having seen it, must still place.
    // Red at BD-S0 with ascending keys too: the unlogged row keeps its slot
    // and the mount is refused. Green at BD-S2 (E5's take-back).
    TempDir snap;
    std::set<std::uint64_t> ids;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
        ids = Fill(d0, 3);

        d0.SetAfterPlacementForTest([] { return Status::IoError("index maintenance failed"); });
        const std::string failed = d0.Dispatch("INSERT INTO t VALUES (40, 4)").response;
        d0.SetAfterPlacementForTest(nullptr);
        ASSERT_TRUE(StartsWith(failed, "ERR")) << failed;

        Ok(d0, "INSERT INTO t VALUES (50, 5)");
        ids.insert(50);
        ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    }
    auto mounted = Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << "the mount refused the crash: " << mounted.status().message();
    ExpectRelation(*mounted.value(), ids, "after the crash");
}

// ---- E2: a mid-leaf insert, replayed --------------------------------------

TEST(SortedLeafCrashTest, AKeyPlacedBetweenTwoRowsIsReplayedBetweenThemNotOverEither) {
    // §1.2 E2: redo reads a slot below `nr_slots` as an overwrite in place,
    // so a mid-leaf insert logged as a HEAP_INSERT would replay over its
    // right neighbour. Red at BD-S0, where SQL refuses the key below the
    // mark - so this cell turns green at BD-S3, when the gate opens; BD-S2's
    // own proof is the redo-level `BTREE_INSERT` cell.
    TempDir snap;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
        Ok(d0, "INSERT INTO t VALUES (10, 1)");
        Ok(d0, "INSERT INTO t VALUES (30, 3)");
        Ok(d0, "INSERT INTO t VALUES (20, 2)");
        ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    }
    auto mounted = Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << "the mount refused the crash: " << mounted.status().message();
    ExpectRelation(*mounted.value(), {10, 20, 30}, "after the crash");
}

// ---- BD-R12: a log cut inside a split -------------------------------------

TEST(SortedLeafCrashTest, ALogCutInsideALeafDivideLeavesTheTreeWhole) {
    // §1.8's divide: 15 sorts inside the full first leaf. A cut between the
    // divide's images leaves the moved half linked and unrouted, or routed
    // and unlinked. Red at BD-S0, where SQL refuses the key below the mark -
    // green at BD-S3; BD-S2's own proof is the `BTREE_SPLIT` cell.
    TempDir before;
    TempDir after;
    std::set<std::uint64_t> ids;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
        ids = Fill(d0);
        ASSERT_TRUE(rig->Snapshot(before.path).ok());
        Ok(d0, "INSERT INTO t VALUES (15, 0)");
        ASSERT_TRUE(rig->Snapshot(after.path).ok());
    }
    CutEverywhere(before.path, after.path,
                  [&](Expeditor& db, bool committed, const std::string& where) {
                      std::set<std::uint64_t> want = ids;
                      if (committed) want.insert(15);
                      ExpectRelation(db, want, where);
                  });
}

TEST(SortedLeafCrashTest, ALogCutInsideAnAppendSplitLeavesEveryLaterRowOnTheWalk) {
    // §1.8's append split, which predates BD: the new leaf's record, the
    // parent's image, then the old leaf's image carrying the link. A cut
    // after the parent's image routes ids to a leaf no walk reaches, and
    // the rows inserted after the restart land there. Decided at BD-S1: it
    // reproduced, after record 5 of 8 (the parent's image), and BD-R12's
    // one BTREE_SPLIT record fixed it at BD-S2.
    constexpr std::uint64_t kSplitter = 10 * (198 * 2 + 1);
    TempDir before;
    TempDir after;
    std::set<std::uint64_t> ids;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
        ids = Fill(d0, 198 * 2);  // two full leaves: the next row splits the second right
        ASSERT_TRUE(rig->Snapshot(before.path).ok());
        Ok(d0, "INSERT INTO t VALUES (" + std::to_string(kSplitter) + ", 0)");
        ASSERT_TRUE(rig->Snapshot(after.path).ok());
    }
    CutEverywhere(before.path, after.path,
                  [&](Expeditor& db, bool committed, const std::string& where) {
                      std::set<std::uint64_t> want = ids;
                      if (committed) want.insert(kSplitter);
                      ExpectRelation(db, want, where);
                      // The rows after the restart: each lands where the
                      // parent routes it, and the walk has to reach it there.
                      for (std::uint64_t id = 100000; id < 100005; ++id) {
                          Ok(db.dispatcher(),
                             "INSERT INTO t VALUES (" + std::to_string(id) + ", 0)");
                          want.insert(id);
                      }
                      ExpectRelation(db, want, where + ", then five more rows");
                  });
}

TEST(SortedLeafCrashTest, ALogCutInsideAnIndexSplitLeavesEveryRowOnItsIndex) {
    // §1.8's open question: the secondary index tree's splits log as images
    // too. A dry run on an in-memory rig finds the row whose insert moves
    // the index's root - the statement that splits its root leaf - and a
    // file-backed run cuts that statement's log everywhere. Decided at
    // BD-S1: it does not reproduce, and the cell stays as a guard.
    const auto create = [](CommandDispatcher& d0) {
        Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
        Ok(d0, "CREATE INDEX tv ON t (v)");
    };
    std::uint64_t splitter = 0;  // the k whose row splits the index's root
    {
        auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        TwoCoreRig& rig = *opened.value();
        CommandDispatcher& d0 = rig.core(0).dispatcher();
        CurrentCoreGuard as(0);
        create(d0);
        const auto index_root = [&]() -> PageId {
            auto oid = rig.core(0).catalog().FindTableOidByName("t");
            EXPECT_TRUE(oid.ok());
            auto access = rig.core(0).catalog().InitTableAccess(oid.value());
            EXPECT_TRUE(access.ok());
            EXPECT_EQ(access.value()->indexes.size(), 1u);
            return access.value()->indexes.front().root_page_id;
        };
        const PageId first_root = index_root();
        for (std::uint64_t k = 1; k <= 4000 && splitter == 0; ++k) {
            Ok(d0, "INSERT INTO t VALUES (" + std::to_string(k * 10) + ", " + std::to_string(k) +
                       ")");
            if (index_root() != first_root) splitter = k;
        }
        ASSERT_NE(splitter, 0u) << "the index never split its root";
    }

    TempDir before;
    TempDir after;
    std::set<std::uint64_t> ids;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CurrentCoreGuard as(0);
        create(d0);
        ids = Fill(d0, splitter - 1);
        ASSERT_TRUE(rig->Snapshot(before.path).ok());
        Ok(d0, "INSERT INTO t VALUES (" + std::to_string(splitter * 10) + ", " +
                   std::to_string(splitter) + ")");
        ASSERT_TRUE(rig->Snapshot(after.path).ok());
    }
    CutEverywhere(before.path, after.path,
                  [&](Expeditor& db, bool committed, const std::string& where) {
                      std::set<std::uint64_t> want = ids;
                      if (committed) want.insert(splitter * 10);
                      ExpectRelation(db, want, where);
                      // Every row through the index would be ~400 probes
                      // per cut; every seventh and the last cross each of
                      // the index's two leaves.
                      for (std::uint64_t id : want) {
                          if ((id / 10) % 7 != 0 && id != *want.rbegin()) continue;
                          EXPECT_EQ(Ids(db.dispatcher(),
                                        "SELECT id FROM t WHERE v = " + std::to_string(id / 10)),
                                    std::vector<std::uint64_t>{id})
                              << where << ": the index does not find " << id;
                      }
                  });
}

TEST(SortedLeafCrashTest, ATakenBackRowWhoseSplitGrewTheRootLeavesTheRootPublished) {
    // E5 on a split that grew a level (BD-S2's review, C1): the root leaf is
    // full, the next row splits it and grows a root over it, and its index
    // maintenance fails. The take-back retires the row and logs the split -
    // and must publish the new root, or every later descent starts at a
    // grown-over leaf that no longer covers the keys above the separator.
    TempDir snap;
    std::set<std::uint64_t> ids;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
        ids = Fill(d0, 198);  // the root leaf, full

        d0.SetAfterPlacementForTest([] { return Status::IoError("index maintenance failed"); });
        const std::string failed = d0.Dispatch("INSERT INTO t VALUES (1990, 0)").response;
        d0.SetAfterPlacementForTest(nullptr);
        ASSERT_TRUE(StartsWith(failed, "ERR")) << failed;

        for (std::uint64_t id = 2000; id < 2010; ++id) {
            Ok(d0, "INSERT INTO t VALUES (" + std::to_string(id) + ", 0)");
            ids.insert(id);
        }
        ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    }
    auto mounted = Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << "the mount refused the crash: " << mounted.status().message();
    ExpectRelation(*mounted.value(), ids, "after the crash");
}

// ---- BD-S4: the crash cells BD-R9 names -----------------------------------

TEST(SortedLeafCrashTest, ALoserWhoseRowsShiftedEachOtherIsRolledBackWholeAcrossACrash) {
    // §1.2's silent case through SQL: the loser places 50, then 40 below it
    // (shifting 50), then 45 between them, and the crash comes before its
    // commit. Recovery undo re-finds each by its key (E1); counting a
    // shifted row's old slot as done would leave one standing.
    TempDir snap;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
        Ok(d0, "INSERT INTO t VALUES (10, 1)");
        Ok(d0, "INSERT INTO t VALUES (60, 6)");
        Session loser;
        Ok(d0, "BEGIN", &loser);
        for (const char* key : {"50", "40", "45"}) {
            Ok(d0, std::string("INSERT INTO t VALUES (") + key + ", 0)", &loser);
        }
        ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    }
    auto mounted = Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << "the mount refused the crash: " << mounted.status().message();
    ExpectRelation(*mounted.value(), {10, 60}, "after the crash");
}

TEST(SortedLeafCrashTest, ALoserRowADivideMovedIsRolledBackAcrossACrash) {
    // A loser's rows below the mark divide the full first leaf, again and
    // again, moving the earlier ones to new right siblings; the crash comes
    // before its commit. Undo finds each rightward of the leaf its record
    // names (BD-Q8 (a)), and the committed rows stay whole.
    TempDir snap;
    std::set<std::uint64_t> ids;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
        ids = Fill(d0);
        const int filled = Leaves(d0, "t");
        Session loser;
        loser.set_durability(wal::DurabilityClass::kRelaxed);
        Ok(d0, "BEGIN", &loser);
        // Descending, so every row the loser placed sorts above the next one
        // and a divide after the first moves some of them right, whichever
        // point it cuts at.
        for (std::uint64_t id = 991; id >= 11; id -= 10) {
            Ok(d0, "INSERT INTO t VALUES (" + std::to_string(id + 1) + ", 0)", &loser);
            Ok(d0, "INSERT INTO t VALUES (" + std::to_string(id) + ", 0)", &loser);
        }
        // The premise: the leaves divided more than once under the loser,
        // so a later divide moved rows the loser had already placed.
        ASSERT_GE(Leaves(d0, "t"), filled + 2) << "the loser's rows divided no leaf twice";
        ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    }
    auto mounted = Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << "the mount refused the crash: " << mounted.status().message();
    ExpectRelation(*mounted.value(), ids, "after the crash");
}

TEST(SortedLeafCrashTest, AMidLeafRowPlacedAndNeverLoggedIsTakenBackBeforeTheNextRecord) {
    // E5 mid-leaf (§1.2's own example): the leaf holds [10, 30]; 20 is placed
    // between them and its index maintenance fails, so it is never logged;
    // 25 is then logged at the slot it sorts to. Left on the leaf, 20 would
    // have shifted 30 and made 25's slot name a row redo never placed.
    TempDir snap;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
        Ok(d0, "INSERT INTO t VALUES (10, 1)");
        Ok(d0, "INSERT INTO t VALUES (30, 3)");
        d0.SetAfterPlacementForTest([] { return Status::IoError("index maintenance failed"); });
        const std::string failed = d0.Dispatch("INSERT INTO t VALUES (20, 2)").response;
        d0.SetAfterPlacementForTest(nullptr);
        ASSERT_TRUE(StartsWith(failed, "ERR")) << failed;
        Ok(d0, "INSERT INTO t VALUES (25, 5)");
        ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    }
    auto mounted = Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << "the mount refused the crash: " << mounted.status().message();
    ExpectRelation(*mounted.value(), {10, 25, 30}, "after the crash");
}

TEST(SortedLeafCrashTest, ALogCutInsideAnInternalNodesDivideLeavesTheTreeWhole) {
    // Fills thousands of wide rows through a rig that runs no writeback tick,
    // so at the debug floor the pool fills with dirty pages and a row's
    // window is refused - BE-R4's refusal, not this cell's subject.
    const WithoutFrameBudgetOverride full_capacity;
    // BD-R12's third shape: a separator sorting inside a full internal node
    // divides it. Rows of about 4 KB put two in a leaf, so 1,400 ascending
    // rows fill a root of 678 children and grow a level over it; a key
    // inside the first leaf then divides that leaf, and its separator lands
    // in the full old root, which divides too. The statement's records are
    // cut everywhere.
    const std::string pad(3900, 'x');
    TempDir before;
    TempDir after;
    std::set<std::uint64_t> ids;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, s varchar(4000)) BTREE");
        Session load;
        load.set_durability(wal::DurabilityClass::kRelaxed);
        for (std::uint64_t k = 1; k <= 1400; ++k) {
            Ok(d0, "INSERT INTO t VALUES (" + std::to_string(k * 10) + ", '" + pad + "')", &load);
            ids.insert(k * 10);
        }
        ASSERT_TRUE(rig->Snapshot(before.path).ok());
        Ok(d0, "INSERT INTO t VALUES (15, '" + pad + "')");
        ASSERT_TRUE(rig->Snapshot(after.path).ok());
    }
    ASSERT_GE(WidestSplit(after.path / "wal"), 5u)
        << "no internal node divided, so the cell would cut nothing it is named for";
    CutEverywhere(before.path, after.path,
                  [&](Expeditor& db, bool committed, const std::string& where) {
                      std::set<std::uint64_t> want = ids;
                      if (committed) want.insert(15);
                      ExpectRelation(db, want, where);
                  });
}

}  // namespace
}  // namespace kds::server
