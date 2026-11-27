// **How often each index served a read** (DF-S3,
// `instructions/v3.2.0/workorder-df-statistics-coverage.md` §2 part C):
// `sys.indexes`' five usage columns, on the two-core rig so a count is seen
// to be the sum of both cores' reads, folded into the one store the
// instance hands every core (`stats/index_stats.hpp`).

#include "two_core_rig.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/session.hpp"
#include "view_text.hpp"

namespace kds::server {
namespace {

bool StartsWith(const std::string& reply, std::string_view prefix) {
    return reply.rfind(prefix, 0) == 0;
}

class IndexUsageRig : public ::testing::Test {
protected:
    void SetUp() override {
        auto opened = TwoCoreRig::Open();
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        rig_ = std::move(opened.value());
        ASSERT_TRUE(StartsWith(Run(0, "CREATE TABLE t (id int64, v int64, w int64) BTREE"),
                               "CREATED"));
        // v repeats every five ids; w is the id.
        for (int id = 1; id <= 10; ++id) {
            ASSERT_TRUE(StartsWith(Run(0, "INSERT INTO t VALUES (" + std::to_string(id) + ", " +
                                              std::to_string(id % 5) + ", " +
                                              std::to_string(id) + ")"),
                                   "INSERTED"));
        }
        ASSERT_TRUE(StartsWith(Run(0, "CREATE INDEX t_v ON t (v)"), "CREATED"));
        ASSERT_TRUE(StartsWith(Run(0, "CREATE INDEX t_w ON t (w)"), "CREATED"));
    }

    std::string Run(std::uint32_t core, const std::string& sql) {
        return rig_->core(core).dispatcher().Dispatch(sql).response;
    }

    // An index's five usage columns, as `core`'s `sys.indexes` shows them.
    struct Usage {
        std::uint64_t probes = 0;
        std::uint64_t ranges = 0;
        std::uint64_t entries_scanned = 0;
        std::uint64_t rows_resolved = 0;
        std::string last_used;
    };
    Usage UsageOf(std::uint32_t core, std::string_view index) {
        const auto shown = testing_view::ReadViewText(
            Run(core, "SELECT name, probes, ranges, entries_scanned, rows_resolved, last_used "
                      "FROM sys.indexes"));
        Usage out;
        const auto column = [&](std::string_view name) {
            const auto value = shown.Find("name", index, name);
            EXPECT_TRUE(value.has_value()) << index << " has no row";
            return value.value_or("0");
        };
        out.probes = std::stoull(column("probes"));
        out.ranges = std::stoull(column("ranges"));
        out.entries_scanned = std::stoull(column("entries_scanned"));
        out.rows_resolved = std::stoull(column("rows_resolved"));
        out.last_used = column("last_used");
        return out;
    }

    std::unique_ptr<TwoCoreRig> rig_;
};

TEST_F(IndexUsageRig, AnIndexNoReadNamesStaysAtZeroAndOneItNamesMoves) {
    // Built, maintained by ten inserts, and used by no read yet.
    for (const char* index : {"t_v", "t_w"}) {
        const Usage unused = UsageOf(0, index);
        EXPECT_EQ(unused.probes, 0u) << index;
        EXPECT_EQ(unused.ranges, 0u) << index;
        EXPECT_EQ(unused.entries_scanned, 0u) << index;
        EXPECT_EQ(unused.rows_resolved, 0u) << index;
        EXPECT_EQ(unused.last_used, "NULL") << index;
    }

    // An equality on v: one probe, the two entries keyed 3, the two rows.
    ASSERT_EQ(Run(0, "SELECT id FROM t WHERE v = 3 ORDER BY id"), "id\\n3\\n8");
    Usage v = UsageOf(0, "t_v");
    EXPECT_EQ(v.probes, 1u);
    EXPECT_EQ(v.ranges, 0u);
    EXPECT_EQ(v.entries_scanned, 2u);
    EXPECT_EQ(v.rows_resolved, 2u);
    EXPECT_NE(v.last_used, "NULL");

    // The same read on the other core: one row, the cores' parts summed, read
    // from either core.
    ASSERT_EQ(Run(1, "SELECT id FROM t WHERE v = 3 ORDER BY id"), "id\\n3\\n8");
    for (std::uint32_t core : {0u, 1u}) {
        v = UsageOf(core, "t_v");
        EXPECT_EQ(v.probes, 2u) << "read on core " << core;
        EXPECT_EQ(v.entries_scanned, 4u);
        EXPECT_EQ(v.rows_resolved, 4u);
    }

    // Core 1 again: its second read of the index adds through the pointer
    // its dispatcher kept at the first, and is counted all the same.
    ASSERT_EQ(Run(1, "SELECT id FROM t WHERE v = 0 ORDER BY id"), "id\\n5\\n10");
    v = UsageOf(0, "t_v");
    EXPECT_EQ(v.probes, 3u);
    EXPECT_EQ(v.entries_scanned, 6u);
    EXPECT_EQ(v.rows_resolved, 6u);

    // A value list is a key set (CU): one walk a value, so two probes.
    ASSERT_EQ(Run(0, "SELECT id FROM t WHERE v IN (1, 2) ORDER BY id"), "id\\n1\\n2\\n6\\n7");
    v = UsageOf(0, "t_v");
    EXPECT_EQ(v.probes, 5u);
    EXPECT_EQ(v.entries_scanned, 10u);
    EXPECT_EQ(v.rows_resolved, 10u);

    // The index no read named has not moved; nor does a read by tuple key
    // move either, nor a refused statement.
    ASSERT_EQ(Run(0, "SELECT v FROM t WHERE id = 4"), "v\\n4");
    ASSERT_TRUE(StartsWith(Run(0, "SELECT nope FROM t WHERE v = 3"), "ERR"));
    EXPECT_EQ(UsageOf(0, "t_v").probes, 5u);
    const Usage w = UsageOf(0, "t_w");
    EXPECT_EQ(w.probes + w.ranges + w.entries_scanned + w.rows_resolved, 0u);
    EXPECT_EQ(w.last_used, "NULL");
    EXPECT_EQ(testing_view::MetaField(Run(0, "SELECT * FROM sys.meta"), "index_stats_dropped"),
              "0");
}

TEST_F(IndexUsageRig, ARangeIsCountedApartFromAProbe) {
    ASSERT_EQ(Run(1, "SELECT id FROM t WHERE w >= 3 AND w <= 5 ORDER BY id"), "id\\n3\\n4\\n5");
    Usage w = UsageOf(0, "t_w");
    EXPECT_EQ(w.probes, 0u);
    EXPECT_EQ(w.ranges, 1u);
    EXPECT_EQ(w.entries_scanned, 3u);
    EXPECT_EQ(w.rows_resolved, 3u);
    EXPECT_NE(w.last_used, "NULL");
    EXPECT_EQ(UsageOf(0, "t_v").ranges, 0u);

    // A strict window is a range too, at the last rank (`index.md` IX9,
    // EB-S1): each strict end is read inclusively, so the entries at 2 and 6
    // are scanned and resolved and the residual drops their rows.
    ASSERT_EQ(Run(1, "SELECT id FROM t WHERE w > 2 AND w < 6 ORDER BY id"), "id\\n3\\n4\\n5");
    w = UsageOf(0, "t_w");
    EXPECT_EQ(w.ranges, 2u);
    EXPECT_EQ(w.entries_scanned, 8u);
    EXPECT_EQ(w.rows_resolved, 8u);

    // `ANALYZE` runs the statement, and its run is a use like any other.
    ASSERT_FALSE(StartsWith(Run(0, "ANALYZE SELECT id FROM t WHERE w >= 3 AND w <= 5"), "ERR"));
    w = UsageOf(0, "t_w");
    EXPECT_EQ(w.ranges, 3u);
    EXPECT_EQ(w.entries_scanned, 11u);
}

TEST_F(IndexUsageRig, AWriteUsesTheIndexItsWhereEntersAndNoOther) {
    // An UPDATE's and a DELETE's WHERE that enters an index collects its
    // rows through it (DU-S2): one use a statement, as a read's probe of it
    // is - the entries it read and the rows the write then descended for.
    // The entries a write adds are no read, and an INSERT reads none.
    // (A write's read through the query layer - `INSERT ... SELECT`'s
    // source - is a run of its own and is counted; no cell here.)
    ASSERT_EQ(Run(0, "UPDATE t SET w = 100 WHERE v = 3"), "UPDATED 2");
    ASSERT_EQ(Run(0, "DELETE FROM t WHERE v = 4"), "DELETED 2");
    ASSERT_TRUE(StartsWith(Run(0, "INSERT INTO t VALUES (11, 3, 11)"), "INSERTED"));
    const Usage v = UsageOf(0, "t_v");
    EXPECT_EQ(v.probes, 2u);
    EXPECT_EQ(v.ranges, 0u);
    EXPECT_EQ(v.entries_scanned, 4u);
    EXPECT_EQ(v.rows_resolved, 4u);
    EXPECT_NE(v.last_used, "NULL");
    // `w` is written and never asked.
    const Usage unused = UsageOf(0, "t_w");
    EXPECT_EQ(unused.probes + unused.ranges + unused.entries_scanned + unused.rows_resolved, 0u);
    EXPECT_EQ(unused.last_used, "NULL");
    // A tuple key range with both ends outranks the index, as on a read:
    // the walk is bounded and `w`'s index is a use of nothing.
    ASSERT_EQ(Run(0, "UPDATE t SET w = 101 WHERE id BETWEEN 1 AND 100 AND w = 100"), "UPDATED 2");
    EXPECT_EQ(UsageOf(0, "t_w").probes + UsageOf(0, "t_w").ranges, 0u);
}

TEST_F(IndexUsageRig, ARebuiltIndexIsANewIndexAndStartsAtZero) {
    ASSERT_EQ(Run(0, "SELECT id FROM t WHERE v = 3 ORDER BY id"), "id\\n3\\n8");
    ASSERT_EQ(UsageOf(0, "t_v").probes, 1u);
    // `REINDEX` builds under a new oid (`index.md`); the old oid's counts
    // stay in the store and no row shows them.
    ASSERT_FALSE(StartsWith(Run(0, "REINDEX INDEX t_v"), "ERR"));
    const Usage rebuilt = UsageOf(0, "t_v");
    EXPECT_EQ(rebuilt.probes, 0u);
    EXPECT_EQ(rebuilt.entries_scanned, 0u);
    EXPECT_EQ(rebuilt.last_used, "NULL");
    ASSERT_EQ(Run(1, "SELECT id FROM t WHERE v = 3 ORDER BY id"), "id\\n3\\n8");
    EXPECT_EQ(UsageOf(0, "t_v").probes, 1u);
}

}  // namespace
}  // namespace kds::server
