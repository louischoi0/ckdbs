// **Another core's open drop is not done** (AX-S2, AX-Q2;
// `instructions/v3.0.0/workorder-ax-inflight-publication.md`), on the
// two-core rig.
//
// DT9 (`ddl-transactional.md` §5b): an unfiltered catalog read - what index
// maintenance resolves with - counts a delete-mark only once its deleter is
// no longer in flight. `ScanAll` answers most marks by one comparison
// against a bound and asks `IsInFlight` only above it. Both were the reading
// core's: AX-S1 made `IsInFlight` the instance's, and the bound stayed this
// core's oldest running id, so on a core running nothing it was
// `UINT64_MAX` - every mark settled, the tables never asked. Since AX-S2 the
// bound is the instance's floor candidate. Core 0's
// uncommitted `DROP INDEX` then read as done on core 1, which is the isolation
// §5a promised and could not keep across cores (AX-Q2).
//
// Synchronous from the test thread before the reactors start, as
// `catalog_name_rig_test.cpp` is: the question is what one core's catalog
// read sees of another core's open transaction.

#include "two_core_rig.hpp"

#include <memory>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "kds/server/session.hpp"

namespace kds::server {
namespace {

std::unique_ptr<TwoCoreRig> OpenRig() {
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    return opened.ok() ? std::move(opened.value()) : nullptr;
}

bool StartsWith(const std::string& reply, std::string_view prefix) {
    return reply.rfind(prefix, 0) == 0;
}

// How many indexes named `name` core `core`'s unfiltered read resolves -
// the read index maintenance takes.
int IndexesNamed(TwoCoreRig& rig, std::uint32_t core, std::string_view name) {
    auto indexes = rig.core(core).catalog().ListIndexes();
    EXPECT_TRUE(indexes.ok()) << indexes.status().message();
    int named = 0;
    if (indexes.ok()) {
        for (const catalog::SysIndexRow& row : indexes.value()) {
            named += catalog::NameView(row.name) == name ? 1 : 0;
        }
    }
    return named;
}

TEST(Dt9AcrossCoresRigTest, APeerStillResolvesAnIndexWhoseDropIsOpenOnAnotherCore) {
    // Red at `353d465`: core 1 answered 0 while the drop was open.
    //
    // **Mutation**: `ScanAll`'s bound as this core's oldest running id again
    // - core 1, running nothing, counts the mark settled without asking.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE p (id int64, v int64)").response, "CREATED"));
    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE INDEX ix ON p (v)").response, "CREATED INDEX"));
    ASSERT_EQ(IndexesNamed(*rig, 1, "ix"), 1);

    Session ddl;
    ASSERT_TRUE(StartsWith(d0.Dispatch("BEGIN", &ddl).response, "BEGIN"));
    ASSERT_TRUE(StartsWith(d0.Dispatch("DROP INDEX ix", &ddl).response, "DROPPED INDEX"));
    EXPECT_EQ(IndexesNamed(*rig, 1, "ix"), 1)
        << "core 1 read core 0's uncommitted DROP INDEX as done";
    EXPECT_EQ(IndexesNamed(*rig, 0, "ix"), 1) << "the dropping core's own read, unchanged";

    ASSERT_TRUE(StartsWith(d0.Dispatch("COMMIT", &ddl).response, "COMMIT"));
    EXPECT_EQ(IndexesNamed(*rig, 1, "ix"), 0) << "the committed drop still reads as open";
    EXPECT_EQ(IndexesNamed(*rig, 0, "ix"), 0);
}

TEST(Dt9AcrossCoresRigTest, APeerStillResolvesADroppedTablesIndexWhileTheDropIsOpen) {
    // The same rule over `DROP TABLE`, whose dependents are delete-marked
    // inside a transaction (§5a): the relation's own row is an overwrite
    // every reader sees at once, and that is §5a's "not isolated"; its
    // index rows are marks, and those are this rule's.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));
    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE INDEX tix ON t (v)").response, "CREATED INDEX"));

    Session ddl;
    ASSERT_TRUE(StartsWith(d0.Dispatch("BEGIN", &ddl).response, "BEGIN"));
    ASSERT_TRUE(StartsWith(d0.Dispatch("DROP TABLE t", &ddl).response, "DROPPED TABLE"));
    EXPECT_EQ(IndexesNamed(*rig, 1, "tix"), 1)
        << "core 1 read core 0's uncommitted DROP TABLE's index mark as done";

    ASSERT_TRUE(StartsWith(d0.Dispatch("ROLLBACK", &ddl).response, "ROLLBACK"));
    EXPECT_EQ(IndexesNamed(*rig, 1, "tix"), 1) << "the rolled-back drop lost its index";

    ASSERT_TRUE(StartsWith(d0.Dispatch("BEGIN", &ddl).response, "BEGIN"));
    ASSERT_TRUE(StartsWith(d0.Dispatch("DROP TABLE t", &ddl).response, "DROPPED TABLE"));
    ASSERT_TRUE(StartsWith(d0.Dispatch("COMMIT", &ddl).response, "COMMIT"));
    EXPECT_EQ(IndexesNamed(*rig, 1, "tix"), 0) << "the committed drop still reads as open";
}

}  // namespace
}  // namespace kds::server
