#include "file_rig_crash.hpp"
#include "two_core_rig.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/catalog/catalog.hpp"

#include "tree_structure.hpp"

// **Sorted placement on two cores** (BD-R9's rigs,
// `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`). Each cell
// drives both cores' dispatchers, core 1 from a thread of its own
// (`CurrentCoreGuard`), against one relation, and checks what a leaf placed
// where keys sort must keep: every row once, a walk in key order, every leaf
// reached by both the descent and the chain.

namespace kds::server {
namespace {

using crash_rig::Ids;
using crash_rig::Leaves;
using crash_rig::Ok;
using crash_rig::StartsWith;

std::unique_ptr<TwoCoreRig> OpenRig() {
    auto opened = TwoCoreRig::Open();
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    return opened.ok() ? std::move(opened.value()) : nullptr;
}

void ExpectTreeWhole(TwoCoreRig& rig, const std::string& table) {
    auto oid = rig.core(0).catalog().FindTableOidByName(table);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto access = rig.core(0).catalog().InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok()) << access.status().message();
    testing_race::ExpectBtreeSeparatorsBoundTheirSubtrees(rig.store(),
                                                          access.value()->desc_page_id);
}

// Runs `fn` as core 1 on its own thread.
class OtherCore {
public:
    template <typename Fn>
    explicit OtherCore(Fn fn)
        : thread_([fn] {
              CurrentCoreGuard as(1);
              fn();
          }) {}
    ~OtherCore() { thread_.join(); }

private:
    std::thread thread_;
};

TEST(SortedLeafRigTest, NamedKeysFromTwoCoresInterleaveIntoOneLeafInKeyOrder) {
    // Core 0 names the even keys and core 1 the odd ones, both descending,
    // at once: every row lands below rows the other core placed, so every
    // placement shifts the other's, and the leaves divide under both.
    std::unique_ptr<TwoCoreRig> rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    CurrentCoreGuard as(0);
    Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
    constexpr int kRows = 600;
    {
        OtherCore other([&] {
            for (int id = kRows - 1; id >= 1; id -= 2) {
                Ok(d1, "INSERT INTO t VALUES (" + std::to_string(id) + ", 1)");
            }
        });
        for (int id = kRows; id >= 2; id -= 2) {
            Ok(d0, "INSERT INTO t VALUES (" + std::to_string(id) + ", 0)");
        }
    }
    std::vector<std::uint64_t> want;
    for (int id = 1; id <= kRows; ++id) want.push_back(static_cast<std::uint64_t>(id));
    EXPECT_EQ(Ids(d0, "SELECT id FROM t"), want) << "a leaf is out of key order";
    EXPECT_EQ(Ids(d0, "SELECT id FROM t ORDER BY id"), want);
    ExpectTreeWhole(*rig, "t");
}

TEST(SortedLeafRigTest, AShiftUnderAnotherCoresUndecidedRowSurvivesItsRollback) {
    // Core 0's undecided 50 sits on the leaf; core 1 places 10..49 below it,
    // each shifting it a slot. Core 0's rollback re-finds 50 by its key
    // (the live trail's locator) and retires that slot alone.
    std::unique_ptr<TwoCoreRig> rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    Session s0;
    {
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
        Ok(d0, "BEGIN", &s0);
        Ok(d0, "INSERT INTO t VALUES (50, 0)", &s0);
    }
    {
        CurrentCoreGuard as(1);
        for (int id = 49; id >= 10; --id) {
            Ok(d1, "INSERT INTO t VALUES (" + std::to_string(id) + ", 1)");
        }
    }
    CurrentCoreGuard as(0);
    Ok(d0, "ROLLBACK", &s0);
    std::vector<std::uint64_t> want;
    for (int id = 10; id <= 49; ++id) want.push_back(static_cast<std::uint64_t>(id));
    EXPECT_EQ(Ids(d0, "SELECT id FROM t"), want) << "the rollback retired the wrong row";
    Ok(d0, "INSERT INTO t VALUES (50, 2)");  // freed by the rollback (W12)
    want.push_back(50);
    EXPECT_EQ(Ids(d0, "SELECT id FROM t"), want);
    ExpectTreeWhole(*rig, "t");
}

TEST(SortedLeafRigTest, AppendsFromTwoCoresKeepTheTreeWholeAndDense) {
    // BD-Q11 (a) at `cores > 1`, a smoke cell: with BB-R1 deleted an id
    // issued first can be placed after a higher one, below it, so the
    // rightmost leaf's divide runs with two cores racing it. The cell checks
    // the tree stays whole and dense; it does not detect a median divide -
    // two cores' interleave keeps the leaf count low either way - and
    // `BtreeTest.AFullRightmostLeafSplitsAtTheInsertionPoint` is what does.
    std::unique_ptr<TwoCoreRig> rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    CurrentCoreGuard as(0);
    Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
    constexpr int kPerCore = 600;
    {
        OtherCore other([&] {
            for (int i = 0; i < kPerCore; ++i) Ok(d1, "INSERT INTO t VALUES (1)");
        });
        for (int i = 0; i < kPerCore; ++i) Ok(d0, "INSERT INTO t VALUES (0)");
    }
    const std::vector<std::uint64_t> ids = Ids(d0, "SELECT id FROM t");
    ASSERT_EQ(ids.size(), static_cast<std::size_t>(2 * kPerCore));
    EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end()));
    // 198 rows of `t (id, v)` fill a leaf: 1200 rows take 7 at the least.
    EXPECT_LE(Leaves(d0, "t"), 9) << "the leaves of an append are left half-empty";
    ExpectTreeWhole(*rig, "t");
}

TEST(SortedLeafRigTest, AnInnerBuildAndAResumedPrefixAnswerTheSameUnderAnotherCoresShifts) {
    // E3 and E4: an inner build buckets `tr`'s rows by location, and a
    // stopping sub-chain's mark names the last `tr` row it covered. Core 1
    // places rows below theirs meanwhile - shifting them - with an author no
    // `au` row has, so no reply may change: a probe that read a shifted slot
    // unverified, or a resume that skipped by ordinal, answers differently.
    std::unique_ptr<TwoCoreRig> rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    CurrentCoreGuard as(0);
    Ok(d0, "CREATE TABLE au (id int64, name varchar)");
    Ok(d0, "CREATE TABLE tr (id int64, au_id int64, qty int64)");
    for (int a = 1; a <= 20; ++a) Ok(d0, "INSERT INTO au VALUES ('a" + std::to_string(a) + "')");
    // tr keyed 100000, 101000, ...: every author's first row is among the
    // first 20, where every resume lands, with room below each for core 1's.
    for (int k = 0; k < 400; ++k) {
        Ok(d0, "INSERT INTO tr VALUES (" + std::to_string(100000 + 1000 * k) + ", " +
                   std::to_string(1 + k % 20) + ", " + std::to_string(k) + ")");
    }
    const std::string join = "SELECT au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id";
    const std::string exists =
        "SELECT au.name FROM au WHERE EXISTS (SELECT tr.id FROM tr WHERE tr.au_id = au.id)";
    const std::string want_join = d0.Dispatch(join).response;
    const std::string want_exists = d0.Dispatch(exists).response;
    ASSERT_FALSE(StartsWith(want_join, "ERR")) << want_join;
    ASSERT_FALSE(StartsWith(want_exists, "ERR")) << want_exists;
    // The premise: the join builds its inner map, and the EXISTS walks a
    // prefix of `tr` - or the cell would test neither E3 nor E4.
    const std::string plan = d0.Dispatch("ANALYZE " + join).response;
    ASSERT_NE(plan.find("inner_built=1"), std::string::npos) << plan;
    const std::string prefix = d0.Dispatch("ANALYZE " + exists).response;
    ASSERT_NE(prefix.find("build_rows="), std::string::npos) << prefix;  // printed when > 0

    std::atomic<bool> stop{false};
    {
        // Core 1 fills the gaps of the first 20 rows for as long as the
        // rounds run, so every resume meets shifted slots.
        OtherCore other([&] {
            for (int j = 1; j < 1000 && !stop.load(); ++j) {
                for (int k = 0; k < 20 && !stop.load(); ++k) {
                    Ok(d1, "INSERT INTO tr VALUES (" + std::to_string(100000 + 1000 * k + j) +
                               ", 999999, 0)");
                }
            }
        });
        for (int round = 0; round < 60; ++round) {
            EXPECT_EQ(d0.Dispatch(join).response, want_join) << "round " << round;
            EXPECT_EQ(d0.Dispatch(exists).response, want_exists) << "round " << round;
        }
        stop.store(true);
    }
    ExpectTreeWhole(*rig, "tr");
}

}  // namespace
}  // namespace kds::server
