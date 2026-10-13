// BP-S4: the delete-mark purge's trigger (BP-H11,
// `instructions/v3.0.0/workorder-bp-per-core-residue.md`;
// `ddl-transactional.md` §5d). Until BP-S4 only core 0's DDL resolution
// swept, so a peer's `DROP` left its marks until core 0 next ran DDL or the
// next mount. Core 0's `system` tick sweeps now, gated on progress: a sweep
// write-fetches every catalog page, so it runs only when the pending count
// or the settlement moved since the last one. The rig runs no reactor, so
// each cell calls the tick's body, `SweepSettledDeleteMarks`, itself.

#include "two_core_rig.hpp"

#include <cstdint>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "kds/server/session.hpp"
#include "view_text.hpp"

namespace kds::server {
namespace {

std::uint64_t MetaField(CommandDispatcher& d, const std::string& key) {
    Session s;
    const auto value = testing_view::MetaField(d.Dispatch("SHOW META", &s).response, key);
    return value.has_value() ? std::stoull(*value) : UINT64_MAX;
}

std::unique_ptr<TwoCoreRig> OpenRig() {
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    return opened.ok() ? std::move(opened.value()) : nullptr;
}

void Ok(CommandDispatcher& d, Session& s, const std::string& sql) {
    const std::string reply = d.Dispatch(sql, &s).response;
    ASSERT_NE(reply.rfind("ERR", 0), 0u) << sql << ": " << reply;
}

// The premise and the change: a peer's drop, with nothing at all on core 0,
// is swept by core 0's tick. **Mutation** (D7, AT-S5b): drop the rig's
// `config.mark_counter` and core 0 sees no pending mark.
TEST(PurgeTriggerRigTest, APeersDropIsSweptByCoreZerosTickWithNoDdlOnCoreZero) {
    std::unique_ptr<TwoCoreRig> rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    Session on_peer;
    Ok(d1, on_peer, "CREATE TABLE gone (id int64, v int64) BTREE");
    Ok(d1, on_peer, "DROP TABLE gone");

    // The peer's own resolution swept nothing: only core 0 sweeps.
    ASSERT_GT(rig->core(0).catalog().MarkCount(), 0u) << "the drop left no mark to sweep";
    EXPECT_EQ(MetaField(d0, "catalog_marks_purged"), 0u);
    // A peer's tick is a return: one core sweeps, as a placement.
    d1.SweepSettledDeleteMarks();
    EXPECT_EQ(MetaField(d1, "catalog_purge_sweeps"), 0u) << "a peer swept";
    ASSERT_GT(rig->core(0).catalog().MarkCount(), 0u);

    d0.SweepSettledDeleteMarks();
    EXPECT_GE(MetaField(d0, "catalog_marks_purged"), 1u) << "core 0's tick left a peer's marks";
    EXPECT_EQ(rig->core(0).catalog().MarkCount(), 0u);
    EXPECT_EQ(MetaField(d0, "catalog_purge_sweeps"), 1u);

    // Nothing pending: the tick is a predicate and a return.
    d0.SweepSettledDeleteMarks();
    EXPECT_EQ(MetaField(d0, "catalog_purge_sweeps"), 1u) << "a tick swept with no mark pending";
}

// The gate: an older open transaction holds the horizon, so the mark it can
// still see is not retired, and commits elsewhere - which move the commit
// ceiling but not the horizon - do not make the tick sweep again. Its
// resolution moves the horizon, and the next tick retires the mark.
TEST(PurgeTriggerRigTest, AMarkHeldByAnOlderReaderIsSweptAgainOnlyWhenTheHorizonMoves) {
    std::unique_ptr<TwoCoreRig> rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    Session reader;
    Session writer;
    Ok(d1, writer, "CREATE TABLE kept (id int64, v int64) BTREE");
    Ok(d1, writer, "CREATE TABLE gone (id int64, v int64) BTREE");
    Ok(d0, reader, "BEGIN");
    Ok(d0, reader, "SELECT * FROM kept WHERE id = 1");
    Ok(d1, writer, "DROP TABLE gone");

    d0.SweepSettledDeleteMarks();
    EXPECT_EQ(MetaField(d0, "catalog_purge_sweeps"), 1u) << "a new mark did not trigger a sweep";
    EXPECT_EQ(MetaField(d0, "catalog_marks_purged"), 0u) << "a mark an open reader can see was retired";

    for (int i = 1; i <= 3; ++i) {
        Ok(d1, writer, "INSERT INTO kept VALUES (" + std::to_string(i) + ", 0)");
        d0.SweepSettledDeleteMarks();
    }
    EXPECT_EQ(MetaField(d0, "catalog_purge_sweeps"), 1u)
        << "the tick swept again while the horizon stood still";

    Ok(d0, reader, "COMMIT");
    d0.SweepSettledDeleteMarks();
    EXPECT_EQ(MetaField(d0, "catalog_purge_sweeps"), 2u);
    EXPECT_GE(MetaField(d0, "catalog_marks_purged"), 1u) << "the reader's end did not free the mark";
}

// A mark whose deleter is still open is not retired; the deleter's own
// commit moves the settlement, and the next tick retires it.
TEST(PurgeTriggerRigTest, AnOpenDeletersMarkWaitsForItsCommit) {
    std::unique_ptr<TwoCoreRig> rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    Session dropper;
    Ok(d1, dropper, "CREATE TABLE t (id int64, v int64) BTREE");
    Ok(d1, dropper, "CREATE INDEX by_v ON t (v)");
    Ok(d1, dropper, "BEGIN");
    Ok(d1, dropper, "DROP INDEX by_v");

    d0.SweepSettledDeleteMarks();
    EXPECT_EQ(MetaField(d0, "catalog_purge_sweeps"), 1u);
    EXPECT_EQ(MetaField(d0, "catalog_marks_purged"), 0u) << "an open deleter's mark was retired";
    EXPECT_GT(rig->core(0).catalog().MarkCount(), 0u);

    Ok(d1, dropper, "COMMIT");
    d0.SweepSettledDeleteMarks();
    EXPECT_EQ(MetaField(d0, "catalog_purge_sweeps"), 2u) << "the deleter's commit moved nothing";
    EXPECT_GE(MetaField(d0, "catalog_marks_purged"), 1u);
}

}  // namespace
}  // namespace kds::server
