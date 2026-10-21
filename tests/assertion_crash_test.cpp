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

}  // namespace
}  // namespace kds::server
