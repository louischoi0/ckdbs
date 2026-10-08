#include "file_rig_crash.hpp"
#include "tree_structure.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/catalog_spills.hpp"
#include "kds/exec/purge_key.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/varheap.hpp"
#include "kds/wal/record.hpp"

// **The purge of one key, at the storage level** (BH-S2,
// `instructions/v3.0.0/workorder-bh-purge-key.md` BH-R3, BH-R6 phase 2,
// BH-R7): `exec::PurgeKey` driven directly - there is no statement until
// BH-S3 - on the file-backed rig, so a crash can be cut at every record it
// appends and mounted through production.
//
// What these cells pin, and the mutation each kills:
//
//   - a purged key is named again and placed; the tombstone's spill is
//     released (the release omitted);
//   - a slot that is not the judged tombstone is skipped - live, stamped by
//     another deleter, absent, or re-placed after a purge (the skip turned
//     into a retire);
//   - the records are `SLOT_RETIRE` then `VARHEAP_RELEASE`, both at
//     `kNoTxnId` (the release before the retire; the envelope given a
//     transaction id);
//   - a crash at every record boundary mounts, and the key is free exactly
//     when the retire survived the cut; a restart after a recovered purge
//     recovers it the same way;
//   - a loser `INSERT` of the purged key is undone at mount, by key.
//
// The judging - a committed deleter, resolved for every reader - is
// BH-S3's, so a deleter still in flight is not a cell here: the primitive
// trusts the caller's judgment and checks only the slot's identity. The
// statement's own crash cells, at the end, are BH-S4's.

namespace kds::server {
namespace {

namespace fs = std::filesystem;

using crash_rig::CutEverywhere;
using crash_rig::Mount;
using crash_rig::Ok;
using crash_rig::OpenFileRig;
using crash_rig::ReadSegment;
using crash_rig::Segment;
using crash_rig::StartsWith;
using crash_rig::TempDir;

// Long enough to spill out of a 16-byte inline cell.
const std::string kLong(100, 'x');

// The trx id stamped on `pk`'s slot - the deleter, once it is delete-marked -
// and the var-heap slots its version points at.
struct SlotFacts {
    std::uint64_t trx_id = 0;
    bool deleted = false;
    std::vector<exec::SpillRef> spills;
};
SlotFacts FactsOf(storage::PageStore& store, const catalog::TableAccess& access,
                  std::uint64_t pk) {
    SlotFacts facts;
    auto at = btree::BtreeLookup(store, access.desc_page_id, pk);
    EXPECT_TRUE(at.ok()) << pk << ": " << at.status().message();
    if (!at.ok()) return facts;
    heap::PageView leaf(at.value().leaf.bytes());
    auto tuple = leaf.ReadTuple(at.value().slot);
    EXPECT_TRUE(tuple.ok()) << tuple.status().message();
    if (!tuple.ok()) return facts;
    facts.trx_id = tuple.value().trx_id;
    facts.deleted = tuple.value().deleted;
    EXPECT_TRUE(exec::RowSpills(access, tuple.value().payload, facts.spills).ok());
    return facts;
}

std::uint16_t LiveValues(storage::PageStore& store, PageId varheap_page) {
    auto page = store.GetForRead(varheap_page);
    EXPECT_TRUE(page.ok()) << page.status().message();
    return page.ok() ? varheap::PageLiveSlots(page.value().bytes()) : 0;
}

// What every cell starts from: `t (id, s varchar(16))` with rows 5 and 9,
// each with a spilled `s`, and 5 deleted and committed - the tombstone a
// purge judges. The caller holds the rig and its `CurrentCoreGuard`.
struct Loaded {
    CommandDispatcher* d0 = nullptr;
    const catalog::TableAccess* access = nullptr;
    SlotFacts tomb;

    StatusOr<exec::PurgeOutcome> Purge(TwoCoreRig& rig, std::uint64_t pk,
                                       std::uint64_t deleter) const {
        return exec::PurgeKey(rig.store(), &rig.wal(), *access, pk, deleter);
    }
    StatusOr<exec::PurgeOutcome> PurgeFive(TwoCoreRig& rig) const {
        return Purge(rig, 5, tomb.trx_id);
    }
};
Loaded Load(TwoCoreRig& rig) {
    Loaded l;
    l.d0 = &rig.core(0).dispatcher();
    Ok(*l.d0, "CREATE TABLE t (id int64, s varchar(16)) BTREE");
    Ok(*l.d0, "INSERT INTO t VALUES (5, '" + kLong + "')");
    Ok(*l.d0, "INSERT INTO t VALUES (9, '" + kLong + "')");
    Ok(*l.d0, "DELETE FROM t WHERE id = 5");
    auto oid = rig.core(0).catalog().FindTableOidByName("t");
    EXPECT_TRUE(oid.ok()) << oid.status().message();
    auto access = rig.core(0).catalog().InitTableAccess(oid.value());
    EXPECT_TRUE(access.ok()) << access.status().message();
    l.access = access.value();
    l.tomb = FactsOf(rig.store(), *l.access, 5);
    EXPECT_TRUE(l.tomb.deleted);
    return l;
}

// Whether `pk` is free on a mounted image: an `INSERT` naming it is placed.
bool KeyIsFree(Expeditor& db, std::uint64_t pk) {
    const std::string insert = "INSERT INTO t VALUES (" + std::to_string(pk) + ", 'new')";
    const std::string reply = db.dispatcher().Dispatch(insert).response;
    if (StartsWith(reply, "INSERTED")) return true;
    EXPECT_NE(reply.find("duplicate primary key"), std::string::npos) << reply;
    return false;
}

std::vector<std::uint64_t> MountedIds(Expeditor& db) {
    return crash_rig::Ids(db.dispatcher(), "SELECT id FROM t");
}

TEST(PurgeKeyStorageTest, APurgedKeyReleasesItsSpillAndIsNamedAgainAndPlaced) {
    auto rig = OpenFileRig();
    ASSERT_NE(rig, nullptr);
    CurrentCoreGuard as(0);
    const Loaded l = Load(*rig);
    ASSERT_EQ(l.tomb.spills.size(), 1u) << "the row did not spill, so the release is untested";
    const std::uint16_t live = LiveValues(rig->store(), l.tomb.spills[0].first);

    auto purged = l.PurgeFive(*rig);
    ASSERT_TRUE(purged.ok()) << purged.status().message();
    EXPECT_EQ(purged.value(), exec::PurgeOutcome::kPurged);
    EXPECT_EQ(LiveValues(rig->store(), l.tomb.spills[0].first), live - 1)
        << "the tombstone's spill was not released";

    const std::string again = l.d0->Dispatch("INSERT INTO t VALUES (5, 'back')").response;
    EXPECT_TRUE(StartsWith(again, "INSERTED")) << again;
    EXPECT_NE(l.d0->Dispatch("SELECT * FROM t WHERE id = 5").response.find("5,back"),
              std::string::npos);
}

TEST(PurgeKeyStorageTest, ASlotThatIsNotTheJudgedTombstoneIsSkipped) {
    auto rig = OpenFileRig();
    ASSERT_NE(rig, nullptr);
    CurrentCoreGuard as(0);
    const Loaded l = Load(*rig);
    const SlotFacts live = FactsOf(rig->store(), *l.access, 9);

    struct Case {
        const char* what;
        std::uint64_t pk;
        std::uint64_t deleter;
    };
    const Case skips[] = {
        {"a live row", 9, live.trx_id},
        {"a tombstone another deleter stamped", 5, l.tomb.trx_id + 1},
        {"a key never placed", 7, l.tomb.trx_id},
    };
    for (const Case& c : skips) {
        auto out = l.Purge(*rig, c.pk, c.deleter);
        ASSERT_TRUE(out.ok()) << c.what << ": " << out.status().message();
        EXPECT_EQ(out.value(), exec::PurgeOutcome::kSkipped) << c.what;
    }
    EXPECT_TRUE(FactsOf(rig->store(), *l.access, 5).deleted) << "the tombstone was retired";

    // Purged, then placed again live: a second purge judged against the old
    // deleter must skip the new row (PU12), never retire it.
    ASSERT_EQ(l.PurgeFive(*rig).value(), exec::PurgeOutcome::kPurged);
    Ok(*l.d0, "INSERT INTO t VALUES (5, 'back')");
    auto again = l.PurgeFive(*rig);
    ASSERT_TRUE(again.ok()) << again.status().message();
    EXPECT_EQ(again.value(), exec::PurgeOutcome::kSkipped);
    EXPECT_NE(l.d0->Dispatch("SELECT * FROM t WHERE id = 5").response.find("5,back"),
              std::string::npos)
        << "the re-placed live row was retired";
    EXPECT_NE(l.d0->Dispatch("SELECT * FROM t WHERE id = 9").response.find("9,"),
              std::string::npos);
}

TEST(PurgeKeyStorageTest, ThePurgeLogsTheRetireThenTheReleaseOutsideAnyTransaction) {
    // BH-R7: retire first (a crash after a release with the tombstone still
    // standing would let a re-run release another value's slot), and both
    // at `kNoTxnId` (analysis would otherwise count them as a loser's and
    // undo would reverse them).
    TempDir before;
    TempDir after;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        const Loaded l = Load(*rig);
        ASSERT_TRUE(rig->Snapshot(before.path).ok());
        ASSERT_EQ(l.PurgeFive(*rig).value(), exec::PurgeOutcome::kPurged);
        ASSERT_TRUE(rig->Snapshot(after.path).ok());
    }
    const std::optional<Segment> head = ReadSegment(before.path / "wal");
    const std::optional<Segment> seg = ReadSegment(after.path / "wal");
    ASSERT_TRUE(head.has_value() && seg.has_value());
    const std::size_t first = head->starts.size();
    ASSERT_EQ(seg->starts.size(), first + 2) << "a purge with one spill appends two records";
    EXPECT_EQ(seg->types[first], wal::RecordType::kSlotRetire);
    EXPECT_EQ(seg->types[first + 1], wal::RecordType::kVarHeapRelease);
    for (std::size_t k = first; k < seg->starts.size(); ++k) {
        EXPECT_EQ(seg->txn_ids[k], wal::kNoTxnId)
            << wal::RecordTypeName(seg->types[k]) << " carries a transaction id";
    }
}

TEST(PurgeKeyStorageTest, ACrashAtEveryRecordOfAPurgeMountsAndFreesTheKeyOnlyOnceRetired) {
    TempDir before;
    TempDir after;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        const Loaded l = Load(*rig);
        ASSERT_TRUE(rig->Snapshot(before.path).ok());
        ASSERT_EQ(l.PurgeFive(*rig).value(), exec::PurgeOutcome::kPurged);
        ASSERT_TRUE(rig->Snapshot(after.path).ok());
    }
    // Every cut keeps at least the retire, the purge's first record:
    // `CutEverywhere` starts after it.
    CutEverywhere(before.path, after.path, [&](Expeditor& db, bool, const std::string& where) {
        CurrentCoreGuard as(0);
        EXPECT_EQ(MountedIds(db), std::vector<std::uint64_t>{9}) << where;
        EXPECT_TRUE(KeyIsFree(db, 5)) << where << ": the retire survived and the key is bound";
    });

    // No purge record at all: the tombstone stands.
    TempDir whole;
    fs::copy(before.path, whole.path, fs::copy_options::recursive);
    auto mounted = Mount(whole.path);
    ASSERT_TRUE(mounted.ok()) << mounted.status().message();
    CurrentCoreGuard as(0);
    EXPECT_FALSE(KeyIsFree(*mounted.value(), 5)) << "a key no retire reached was freed";
}

TEST(PurgeKeyStorageTest, ARestartAfterARecoveredPurgeRecoversItTheSameWay) {
    // A mount recovers the purge, writes its pages back and checkpoints;
    // the next mount must come up the same. It does not reach the
    // appliers' already-applied arms - redo skips a record at or below the
    // page's LSN first, and the retire is stamped under the hold that wrote
    // it - so this pins the restart, not the idempotence.
    TempDir image;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        const Loaded l = Load(*rig);
        ASSERT_EQ(l.PurgeFive(*rig).value(), exec::PurgeOutcome::kPurged);
        ASSERT_TRUE(rig->Snapshot(image.path).ok());
    }
    for (int round = 1; round <= 2; ++round) {
        auto mounted = Mount(image.path);
        ASSERT_TRUE(mounted.ok()) << "mount " << round << ": " << mounted.status().message();
        CurrentCoreGuard as(0);
        EXPECT_EQ(MountedIds(*mounted.value()), std::vector<std::uint64_t>{9}) << "mount " << round;
    }
    auto last = Mount(image.path);
    ASSERT_TRUE(last.ok()) << last.status().message();
    CurrentCoreGuard as(0);
    EXPECT_TRUE(KeyIsFree(*last.value(), 5));
}

TEST(PurgeKeyStorageTest, ALoserInsertOfAPurgedKeyIsUndoneAtMount) {
    // BH-R7's recovery arm: an `INSERT` of the purged key, never decided,
    // is a loser; undo finds it by key (`recovery_undo.cpp`), so the key is
    // free again after the mount and the tombstone the purge retired does
    // not come back.
    TempDir image;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        const Loaded l = Load(*rig);
        ASSERT_EQ(l.PurgeFive(*rig).value(), exec::PurgeOutcome::kPurged);
        Session open;
        Ok(*l.d0, "BEGIN", &open);
        Ok(*l.d0, "INSERT INTO t VALUES (5, '" + kLong + "')", &open);
        ASSERT_TRUE(rig->Snapshot(image.path).ok());
        Ok(*l.d0, "ROLLBACK", &open);
    }
    auto mounted = Mount(image.path);
    ASSERT_TRUE(mounted.ok()) << mounted.status().message();
    CurrentCoreGuard as(0);
    EXPECT_EQ(MountedIds(*mounted.value()), std::vector<std::uint64_t>{9});
    EXPECT_TRUE(KeyIsFree(*mounted.value(), 5));
}

// ---- The statement, crashed (BH-S4) ----------------------------------------

TEST(PurgeKeySqlCrashTest, ACrashInsideARangedPurgeFreesAPrefixOfItsKeysInKeyOrder) {
    // BH-R6: phase 2 writes key by key in key order, each whole, so a crash
    // anywhere inside the statement leaves its window purged up to some key
    // and untouched after it - never a hole. With the commit record in, all
    // of it.
    TempDir before;
    TempDir after;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        Ok(d0, "CREATE TABLE t (id int64, s varchar(16)) BTREE");
        for (int id : {5, 6, 7, 9}) {
            Ok(d0, "INSERT INTO t VALUES (" + std::to_string(id) + ", '" + kLong + "')");
        }
        for (int id : {5, 6, 7}) Ok(d0, "DELETE FROM t WHERE id = " + std::to_string(id));
        ASSERT_TRUE(rig->Snapshot(before.path).ok());
        ASSERT_EQ(d0.Dispatch("PURGE FROM t WHERE id BETWEEN 5 AND 8").response, "PURGED 3");
        ASSERT_TRUE(rig->Snapshot(after.path).ok());
    }
    int partial = 0;  // cuts that freed some keys and not all: the cell's point
    CutEverywhere(before.path, after.path, [&](Expeditor& db, bool committed,
                                                const std::string& where) {
        CurrentCoreGuard as(0);
        EXPECT_EQ(MountedIds(db), std::vector<std::uint64_t>{9}) << where;
        bool bound_seen = false;
        int freed = 0;
        for (std::uint64_t key : {5, 6, 7}) {
            const bool free = KeyIsFree(db, key);
            EXPECT_FALSE(free && bound_seen) << where << ": key " << key
                                             << " is free after a bound one - a hole";
            bound_seen = bound_seen || !free;
            freed += free ? 1 : 0;
            if (committed) EXPECT_TRUE(free) << where << ": committed, and " << key << " is bound";
        }
        if (freed > 0 && freed < 3) ++partial;
    });
    EXPECT_GT(partial, 0) << "no cut landed inside the window, so no prefix was tested";
}

TEST(PurgeKeySqlCrashTest, APurgedKeyPlacedAgainIsKeptCommittedAndUndoneUncommitted) {
    // BH-R11's crash cell after a re-insert of the purged key: committed, the
    // new row survives the crash; undecided, it is undone by key and the key
    // is free again - the tombstone does not come back either way.
    for (const bool commit : {true, false}) {
        SCOPED_TRACE(commit ? "committed" : "undecided");
        TempDir image;
        {
            auto rig = OpenFileRig();
            ASSERT_NE(rig, nullptr);
            CurrentCoreGuard as(0);
            const Loaded l = Load(*rig);
            ASSERT_EQ(l.d0->Dispatch("PURGE FROM t WHERE id = 5").response, "PURGED 1");
            Session s;
            Ok(*l.d0, "BEGIN", &s);
            Ok(*l.d0, "INSERT INTO t VALUES (5, '" + kLong + "')", &s);
            if (commit) Ok(*l.d0, "COMMIT", &s);
            ASSERT_TRUE(rig->Snapshot(image.path).ok());
            if (!commit) Ok(*l.d0, "ROLLBACK", &s);
        }
        auto mounted = Mount(image.path);
        ASSERT_TRUE(mounted.ok()) << mounted.status().message();
        CurrentCoreGuard as(0);
        if (commit) {
            EXPECT_EQ(MountedIds(*mounted.value()), (std::vector<std::uint64_t>{5, 9}));
            EXPECT_FALSE(KeyIsFree(*mounted.value(), 5));
        } else {
            EXPECT_EQ(MountedIds(*mounted.value()), std::vector<std::uint64_t>{9});
            EXPECT_TRUE(KeyIsFree(*mounted.value(), 5));
        }
    }
}

TEST(PurgeKeySqlCrashTest, ACompactedLeafSurvivesACrashAtEveryRecordAndStaysRouted) {
    // BH-S5's fix: a full leaf of retired slots is rebuilt in place to take
    // the row - one page image in one `BTREE_SPLIT`. A cut anywhere leaves
    // the leaf as it was or as it became, never between, and every
    // separator still bounds its subtree.
    TempDir before;
    TempDir after;
    std::uint64_t low = 0;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        Ok(d0, "CREATE TABLE t (id int64, s varchar(16)) BTREE");
        std::string load = "INSERT INTO t VALUES ";
        for (int id = 1; id <= 600; ++id) {
            load += "(" + std::to_string(id) + ", 'v')" + (id < 600 ? ", " : "");
        }
        Ok(d0, load);
        // The second leaf, emptied of keys: every row of it deleted and purged.
        auto oid = rig->core(0).catalog().FindTableOidByName("t");
        ASSERT_TRUE(oid.ok());
        auto access = rig->core(0).catalog().InitTableAccess(oid.value());
        ASSERT_TRUE(access.ok());
        auto first = btree::BtreeLeftmostLeaf(rig->store(), access.value()->desc_page_id);
        ASSERT_TRUE(first.ok());
        PageId second = kInvalidPageId;
        std::uint64_t high = 0;
        {
            auto page = rig->store().GetForRead(first.value());
            ASSERT_TRUE(page.ok());
            second = heap::PageView(page.value().bytes()).next_page_id();
        }
        ASSERT_NE(second, kInvalidPageId) << "600 rows did not fill a second leaf";
        {
            auto page = rig->store().GetForRead(second);
            ASSERT_TRUE(page.ok());
            heap::PageView leaf(page.value().bytes());
            low = leaf.min_key();
            const PageId third = leaf.next_page_id();
            ASSERT_NE(third, kInvalidPageId) << "the second leaf is the last";
            auto next = rig->store().GetForRead(third);
            ASSERT_TRUE(next.ok());
            high = heap::PageView(next.value().bytes()).min_key() - 1;
        }
        const std::string window =
            " WHERE id BETWEEN " + std::to_string(low) + " AND " + std::to_string(high);
        Ok(d0, "DELETE FROM t" + window);
        Ok(d0, "PURGE FROM t" + window);
        ASSERT_TRUE(rig->Snapshot(before.path).ok());
        Ok(d0, "INSERT INTO t VALUES (" + std::to_string(low) + ", 'back')");
        ASSERT_TRUE(rig->Snapshot(after.path).ok());
    }
    CutEverywhere(before.path, after.path, [&](Expeditor& db, bool committed,
                                                const std::string& where) {
        CurrentCoreGuard as(0);
        const std::vector<std::uint64_t> at =
            crash_rig::Ids(db.dispatcher(), "SELECT id FROM t WHERE id = " + std::to_string(low));
        if (committed) {
            EXPECT_EQ(at, std::vector<std::uint64_t>{low}) << where;
        } else {
            EXPECT_TRUE(at.empty()) << where << ": an uncommitted row survived";
        }
        auto oid = db.catalog().FindTableOidByName("t");
        ASSERT_TRUE(oid.ok());
        auto access = db.catalog().InitTableAccess(oid.value());
        ASSERT_TRUE(access.ok());
        testing_race::ExpectBtreeSeparatorsBoundTheirSubtrees(db.store(),
                                                              access.value()->desc_page_id);
        EXPECT_TRUE(KeyIsFree(db, low + 1)) << where;
    });
}

}  // namespace
}  // namespace kds::server
