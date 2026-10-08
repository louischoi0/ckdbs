#include "file_rig_crash.hpp"
#include "two_core_rig.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"

// **`PURGE` against another core's writer of the same key** (BH-S4,
// `instructions/v3.0.0/workorder-bh-purge-key.md` PU12, BH-R4, BH-R11): no
// row or range unit is taken, so the leaf's exclusive latch and the phase-2
// skip are what order a `PURGE` on one core against what another core does
// to the key it judged. Each cell runs core 1's `PURGE` with a seam between
// its two phases, and runs core 0's statement there - the window a second
// core's write lands in.

namespace kds::server {
namespace {

using crash_rig::Ok;
using crash_rig::StartsWith;

class PurgeKeyRigTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto opened = TwoCoreRig::Open();
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        rig_ = std::move(opened.value());
        CurrentCoreGuard as(0);
        Ok(d(0), "CREATE TABLE t (id int64, v int64) BTREE");
        Ok(d(0), "INSERT INTO t VALUES (5, 1)");
        Ok(d(0), "INSERT INTO t VALUES (9, 1)");
        Ok(d(0), "DELETE FROM t WHERE id = 5");
    }
    void TearDown() override {
        if (rig_) d(1).SetPurgeSeamForTest(nullptr);
    }

    CommandDispatcher& d(std::uint32_t core) { return rig_->core(core).dispatcher(); }
    std::string On(std::uint32_t core, const std::string& sql) {
        CurrentCoreGuard as(core);
        return d(core).Dispatch(sql).response;
    }
    // Core 1's `PURGE`, with `between` run on core 0 between its phases.
    std::string PurgeOnCoreOne(const std::string& sql, std::function<void()> between) {
        bool fired = false;
        d(1).SetPurgeSeamForTest([&] {
            if (fired) return;
            fired = true;
            between();
        });
        const std::string reply = On(1, sql);
        d(1).SetPurgeSeamForTest(nullptr);
        EXPECT_TRUE(fired) << "the purge never reached its phase 2: " << reply;
        return reply;
    }

    std::unique_ptr<TwoCoreRig> rig_;
};

TEST_F(PurgeKeyRigTest, ANamedInsertOnAnotherCoreIsRefusedBeforeTheRetireAndPlacedAfter) {
    std::string before;
    const std::string purged = PurgeOnCoreOne("PURGE FROM t WHERE id = 5", [&] {
        before = On(0, "INSERT INTO t VALUES (5, 2)");
    });
    EXPECT_EQ(purged, "PURGED 1");
    EXPECT_NE(before.find("PURGE frees its key"), std::string::npos)
        << "a named INSERT reached the key before the retire and was not refused as the "
           "tombstone's duplicate: "
        << before;
    const std::string after = On(0, "INSERT INTO t VALUES (5, 3)");
    EXPECT_TRUE(StartsWith(after, "INSERTED")) << after;
    EXPECT_NE(On(1, "SELECT * FROM t WHERE id = 5").find("5,3"), std::string::npos);
}

TEST_F(PurgeKeyRigTest, TwoPurgesOfOneKeyOnTwoCoresFreeItOnce) {
    std::string other;
    const std::string purged = PurgeOnCoreOne("PURGE FROM t WHERE id = 5", [&] {
        other = On(0, "PURGE FROM t WHERE id = 5");
    });
    EXPECT_EQ(other, "PURGED 1");
    EXPECT_EQ(purged, "PURGED 0") << "the second purge counted a key the first had freed";
    EXPECT_TRUE(StartsWith(On(0, "INSERT INTO t VALUES (5, 2)"), "INSERTED"));
}

TEST_F(PurgeKeyRigTest, AKeyPurgedAndPlacedAgainBetweenThePhasesIsSkippedLive) {
    // PU12's quiet-wrong case: the slot phase 1 judged is gone and a live row
    // carries its key. A retire there would delete a committed row with no
    // DELETE - the phase-2 skip is what BH-R4's lock-free design rests on.
    const std::string purged = PurgeOnCoreOne("PURGE FROM t WHERE id = 5", [&] {
        ASSERT_EQ(On(0, "PURGE FROM t WHERE id = 5"), "PURGED 1");
        ASSERT_TRUE(StartsWith(On(0, "INSERT INTO t VALUES (5, 7)"), "INSERTED"));
    });
    EXPECT_EQ(purged, "PURGED 0");
    EXPECT_NE(On(1, "SELECT * FROM t WHERE id = 5").find("5,7"), std::string::npos)
        << "the re-placed live row was retired";
}

TEST_F(PurgeKeyRigTest, AKeyRedeletedByAnotherDeleterBetweenThePhasesIsSkipped) {
    // The tombstone is new: its deleter is one phase 1 never judged against
    // the horizon, so retiring it could take a row from a reader still owed
    // it. A verify of the delete flag alone would retire it; the judged
    // deleter's id is what the skip compares (BH-S4's review, T1).
    const std::string purged = PurgeOnCoreOne("PURGE FROM t WHERE id = 5", [&] {
        ASSERT_EQ(On(0, "PURGE FROM t WHERE id = 5"), "PURGED 1");
        ASSERT_TRUE(StartsWith(On(0, "INSERT INTO t VALUES (5, 7)"), "INSERTED"));
        ASSERT_EQ(On(0, "DELETE FROM t WHERE id = 5"), "DELETED 1");
    });
    EXPECT_EQ(purged, "PURGED 0") << "a tombstone another deleter stamped was retired";
    EXPECT_NE(On(0, "INSERT INTO t VALUES (5, 8)").find("PURGE frees its key"),
              std::string::npos);
}

}  // namespace
}  // namespace kds::server
