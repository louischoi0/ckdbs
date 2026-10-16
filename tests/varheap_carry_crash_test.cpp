#include "file_rig_crash.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/exec/catalog_spills.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/varheap.hpp"

// **A carried spill through a crash and through `PURGE`** (BN-S5,
// `instructions/v3.0.0/workorder-bn-opt-read-update-path.md` BN-R3, BN-Q4
// (a)): an UPDATE that does not assign a spilled column carries its cell, so
// the new version names the old version's var-heap slot. The live rollback's
// cells are `varheap_lifetime_test.cpp`'s; these are the other two releasers
// that exist, on the file-backed rig through production's mount:
//
//   - recovery's undo: a carrying UPDATE cut at every record it appends, or
//     left undecided, mounts with the value still live and readable - its
//     undo restores a version that names the same slot and releases nothing,
//     because the carry appended nothing;
//   - `PURGE` of a row whose older versions carried the value releases that
//     one slot, and no other row's (a second release of it would not show:
//     `varheap::PageRelease` is idempotent).

namespace kds::server {
namespace {

using crash_rig::CutEverywhere;
using crash_rig::Mount;
using crash_rig::Ok;
using crash_rig::OpenFileRig;
using crash_rig::TempDir;

// Long enough to spill out of a 16-byte inline cell.
const std::string kLong(100, 'x');

// The var-heap slots `tuple_key`'s current version names.
std::vector<exec::SpillRef> SpillsOf(storage::PageStore& store,
                                     const catalog::TableAccess& access, std::uint64_t tuple_key) {
    std::vector<exec::SpillRef> out;
    auto at = btree::BtreeLookup(store, access.desc_page_id, tuple_key);
    EXPECT_TRUE(at.ok()) << tuple_key << ": " << at.status().message();
    if (!at.ok()) return out;
    heap::PageView leaf(at.value().leaf.bytes());
    auto tuple = leaf.ReadTuple(at.value().slot);
    EXPECT_TRUE(tuple.ok()) << tuple.status().message();
    if (tuple.ok()) EXPECT_TRUE(exec::RowSpills(access, tuple.value().payload, out).ok());
    return out;
}

std::uint16_t LiveValues(storage::PageStore& store, PageId varheap_page) {
    auto page = store.GetForRead(varheap_page);
    EXPECT_TRUE(page.ok()) << page.status().message();
    return page.ok() ? varheap::PageLiveSlots(page.value().bytes()) : 0;
}

const catalog::TableAccess* AccessOf(TwoCoreRig& rig) {
    auto oid = rig.core(0).catalog().FindTableOidByName(::kds::catalog::kNamespacePublic, "t");
    EXPECT_TRUE(oid.ok()) << oid.status().message();
    auto access = rig.core(0).catalog().InitTableAccess(oid.value());
    EXPECT_TRUE(access.ok()) << access.status().message();
    return access.ok() ? access.value() : nullptr;
}

// `t (id, s varchar(16), n)`, rows 5 and 9, each `s` spilled.
void Load(CommandDispatcher& d) {
    Ok(d, "CREATE TABLE t (id int64, s varchar(16), n int64) BTREE");
    Ok(d, "INSERT INTO t VALUES (5, '" + kLong + "', 1)");
    Ok(d, "INSERT INTO t VALUES (9, '" + kLong + "', 1)");
}

std::string Row(Expeditor& db, std::uint64_t tuple_key) {
    return db.dispatcher().Dispatch("SELECT s, n FROM t WHERE id = " + std::to_string(tuple_key)).response;
}

TEST(VarHeapCarryCrashTest, ACarryingUpdateCutAtEveryRecordKeepsTheValue) {
    TempDir before;
    TempDir after;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        CommandDispatcher& d = rig->core(0).dispatcher();
        Load(d);
        const catalog::TableAccess* access = AccessOf(*rig);
        ASSERT_NE(access, nullptr);
        const auto spills = SpillsOf(rig->store(), *access, 5);
        ASSERT_EQ(spills.size(), 1u) << "the row did not spill, so the carry is untested";
        ASSERT_TRUE(rig->Snapshot(before.path).ok());
        Ok(d, "UPDATE t SET n = 2 WHERE id = 5");
        EXPECT_EQ(SpillsOf(rig->store(), *access, 5), spills) << "the value was not carried";
        ASSERT_TRUE(rig->Snapshot(after.path).ok());
    }
    CutEverywhere(before.path, after.path, [](Expeditor& db, bool committed,
                                              const std::string& where) {
        CurrentCoreGuard as(0);
        const std::string row = Row(db, 5);
        EXPECT_NE(row.find(kLong + (committed ? ",2" : ",1")), std::string::npos)
            << where << ": " << row;
        EXPECT_NE(Row(db, 9).find(kLong + ",1"), std::string::npos) << where;
    });
}

TEST(VarHeapCarryCrashTest, AnUndecidedCarryingUpdateIsUndoneAtMountAndReleasesNothing) {
    TempDir image;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        CommandDispatcher& d = rig->core(0).dispatcher();
        Load(d);
        Session s;
        Ok(d, "BEGIN", &s);
        Ok(d, "UPDATE t SET n = 2 WHERE id = 5", &s);
        Ok(d, "UPDATE t SET n = 3 WHERE id = 5", &s);
        ASSERT_TRUE(rig->Snapshot(image.path).ok());
        Ok(d, "ROLLBACK", &s);
    }
    auto mounted = Mount(image.path);
    ASSERT_TRUE(mounted.ok()) << mounted.status().message();
    CurrentCoreGuard as(0);
    EXPECT_NE(Row(*mounted.value(), 5).find(kLong + ",1"), std::string::npos)
        << Row(*mounted.value(), 5);
    // And the value is still the live version's to update: a carry after
    // the mount reads it back unchanged.
    Ok(mounted.value()->dispatcher(), "UPDATE t SET n = 4 WHERE id = 5");
    EXPECT_NE(Row(*mounted.value(), 5).find(kLong + ",4"), std::string::npos);
}

TEST(VarHeapCarryCrashTest, APurgeOfARowWhoseVersionsCarriedAValueReleasesIt) {
    TempDir image;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        CommandDispatcher& d = rig->core(0).dispatcher();
        Load(d);
        const catalog::TableAccess* access = AccessOf(*rig);
        ASSERT_NE(access, nullptr);
        const auto five = SpillsOf(rig->store(), *access, 5);
        const auto nine = SpillsOf(rig->store(), *access, 9);
        ASSERT_EQ(five.size(), 1u);
        ASSERT_EQ(nine.size(), 1u);
        ASSERT_EQ(five[0].first, nine[0].first) << "the cell counts one page's slots";
        const std::uint16_t live = LiveValues(rig->store(), five[0].first);

        Ok(d, "UPDATE t SET n = 2 WHERE id = 5");
        Ok(d, "UPDATE t SET n = 3 WHERE id = 5");
        Ok(d, "DELETE FROM t WHERE id = 5");
        EXPECT_EQ(LiveValues(rig->store(), five[0].first), live)
            << "an update or the delete released a value";
        ASSERT_EQ(d.Dispatch("PURGE FROM t WHERE id = 5").response, "PURGED 1");
        EXPECT_EQ(LiveValues(rig->store(), five[0].first), live - 1)
            << "the purge did not release the shared value";
        ASSERT_TRUE(rig->Snapshot(image.path).ok());
    }
    auto mounted = Mount(image.path);
    ASSERT_TRUE(mounted.ok()) << mounted.status().message();
    CurrentCoreGuard as(0);
    EXPECT_NE(Row(*mounted.value(), 9).find(kLong + ",1"), std::string::npos)
        << "the purge released another row's value";
    Ok(mounted.value()->dispatcher(), "INSERT INTO t VALUES (5, 'back', 7)");
}

}  // namespace
}  // namespace kds::server
