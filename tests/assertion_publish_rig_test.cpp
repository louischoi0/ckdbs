// AZ-S2: a `CREATE ASSERTION` on a peer, and the window between its publish
// run and its `sys.assertions` row, on the two-core rig
// (`instructions/v3.0.0/workorder-az-ay-carry-forward.md` §3, AZ-R2; the bug
// entry it closes, `a-peers-create-assertion-can-miss-every-snapshot-in-the-
// mounts-scan.md`, is deleted by it).
//
// An assertion's recovery base is an `ASSERT_SNAPSHOT` run, written by its
// own create and by core 0's checkpoints, which snapshot the registry. The
// create logged its run and entered the registry only after it returned, so
// a core-0 checkpoint in between wrote a run without it; once the redo start
// passed the create's own run, a mount found no base and the assertion came
// up unenforcing. Each cell puts something in that window through the
// dispatcher's seam, `SetAfterAssertionPublishRunForTest`:
//
//   - **a core-0 checkpoint**, whose run must carry the new assertion - read
//     back from the checkpoint's `BEGIN`, the scan a mount makes once every
//     page dirtied before the create's run is clean;
//   - **a writer on the other core**, which waits on the build's relation
//     `X` and is then checked against the assertion;
//   - **a create of the same name on the other core**, which makes this one
//     fail after it is adopted - and a failed create must leave nothing
//     enforcing.
//
// What a checkpoint's run and the create's run of one id do when their
// chunks interleave is `assertion_recover_test.cpp`'s cell, where both
// writers can be stopped between chunks.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/exec/assertion_catalog.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/mount_recovery.hpp"
#include "kds/storage/page_store_checkpoint_target.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/wal/checkpointer.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

// One statement run on a core's reactor once `go` is set
// (`ddl_fence_rig_test.cpp`'s shape).
struct Statement {
    Session own;
    DispatchOutcome out;
    std::string sql;
    std::function<bool()> start_pred;
    std::atomic<bool> go{false};
    std::atomic<bool> done{false};
};

sched::Coro RunWhenGo(CommandDispatcher& d, Statement& x) {
    x.start_pred = [&x] { return x.go.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&x.start_pred};
    co_await d.DispatchAsync(x.sql, &x.own, &x.out);
    x.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

void Submit(TwoCoreRig& rig, std::uint32_t core, Statement& x) {
    rig.core(core).scheduler().Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground, RunWhenGo(rig.core(core).dispatcher(), x)));
}

catalog::Oid Relation(TwoCoreRig& rig, const std::string& name) {
    const std::string made = rig.core(0)
                                 .dispatcher()
                                 .Dispatch("CREATE TABLE " + name + " (id int64, v int64) BTREE")
                                 .response;
    EXPECT_EQ(made.rfind("CREATED", 0), 0u) << made;
    auto oid = rig.core(0).catalog().FindTableOidByName(name);
    EXPECT_TRUE(oid.ok()) << oid.status().message();
    return oid.ok() ? oid.value() : 0;
}

// An `INSERT`'s values after the pk: the one column, `v`.
std::vector<parser::AstValue> Row(std::int64_t v) {
    std::vector<parser::AstValue> row(1);
    row[0].type = parser::ValueType::kInt;
    row[0].int_val = v;
    return row;
}

TEST(AssertionPublishRigTest, ACoreZeroCheckpointInTheCreatesWindowCarriesTheNewAssertion) {
    // **The bug entry's sequence.** Core 1's create logs its publish run at
    // `P`; core 0 checkpoints before the row is written; the scan a mount
    // makes from that checkpoint's `BEGIN` - past `P` - must still find a
    // base for the assertion. Until AZ-S2 the checkpoint's run did not name
    // it, and the assertion came up unrecovered.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    const catalog::Oid oid = Relation(*rig, "capped");
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    ASSERT_EQ(d1.Dispatch("INSERT INTO capped VALUES (5)").response.rfind("INSERTED", 0), 0u);

    // Core 0's checkpoint, on a thread of its own as core 0's reactor would
    // run it, over the instance's registry as `Expeditor` wires it.
    wal::Lsn checkpoint_lsn = 0;
    d1.SetAfterAssertionPublishRunForTest([&] {
        std::thread core0([&] {
            storage::PageStoreCheckpointTarget target(rig->store());
            wal::NoActiveTransactions none;
            wal::InMemoryCheckpointAnchor anchor;
            wal::Checkpointer checkpointer(rig->wal(), target, none, anchor);
            checkpointer.SetAssertionSource(&rig->core(0).dispatcher().assertions());
            const Status s = checkpointer.RunToCompletion();
            EXPECT_TRUE(s.ok()) << s.message();
            checkpoint_lsn = anchor.anchor().checkpoint_lsn;
        });
        core0.join();
    });
    const std::string created =
        d1.Dispatch("CREATE ASSERTION one ON capped GROUP BY (v) CHECK COUNT(*) <= 1").response;
    d1.SetAfterAssertionPublishRunForTest({});
    ASSERT_EQ(created.rfind("CREATED ASSERTION", 0), 0u) << created;
    ASSERT_NE(checkpoint_lsn, 0u) << "the seam never ran a checkpoint";
    ASSERT_TRUE(rig->wal().Flush().ok());

    // The mount's assertion pass, from the checkpoint, into an empty
    // registry.
    exec::AssertionEnforcer fresh;
    const MountRecovery report = ResumeAssertionsAfterRecovery(
        rig->core(0).catalog(), rig->store(), rig->log_device(), /*stream_core=*/0,
        checkpoint_lsn, fresh, MountRecovery{}, /*log=*/nullptr);
    EXPECT_EQ(report.assertions_unrecovered, 0u)
        << "the checkpoint in the create's window wrote a run without the assertion";
    EXPECT_EQ(report.assertions_enforcing, 1u);

    // With the built row counted: the next row in its group is refused.
    exec::AssertionEnforcer::Hold hold;
    EXPECT_EQ(fresh.AdmitInsert(oid, Row(5), /*writer_txn=*/0, hold).code(),
              StatusCode::kAssertionViolation);
}

TEST(AssertionPublishRigTest, AWriterOnTheOtherCoreInTheWindowWaitsAndIsCheckedAgainstIt) {
    // **The adoption stays under the build's relation `X`** (§1.1): a writer
    // on core 0 arriving in the window parks on it, and when it resumes the
    // assertion is enforcing - its row, the group's second, is refused.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    const catalog::Oid oid = Relation(*rig, "fenced");
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    ASSERT_EQ(d1.Dispatch("INSERT INTO fenced VALUES (7)").response.rfind("INSERTED", 0), 0u);

    Statement create;
    create.sql = "CREATE ASSERTION one ON fenced GROUP BY (v) CHECK COUNT(*) <= 1";
    Statement writer;
    writer.sql = "INSERT INTO fenced VALUES (7)";
    const txn::LockKey rel = txn::LockKey::Relation(oid);
    bool parked = false;
    bool ran_past = true;
    d1.SetAfterAssertionPublishRunForTest([&] {
        writer.go.store(true, std::memory_order_release);
        parked = KickUntil(*rig, 0, [&] { return rig->locks().WaiterCount(rel) == 1; });
        ran_past = writer.done.load(std::memory_order_acquire);
    });
    Submit(*rig, 1, create);
    Submit(*rig, 0, writer);
    rig->Start();
    create.go.store(true, std::memory_order_release);

    ASSERT_TRUE(
        KickUntil(*rig, 1, [&] { return create.done.load(std::memory_order_acquire); }, 5000ms))
        << "the create never finished";
    ASSERT_TRUE(
        KickUntil(*rig, 0, [&] { return writer.done.load(std::memory_order_acquire); }, 5000ms))
        << "the writer never resumed after the create";
    rig->Stop();
    d1.SetAfterAssertionPublishRunForTest({});

    EXPECT_EQ(create.out.response.rfind("CREATED ASSERTION", 0), 0u) << create.out.response;
    EXPECT_TRUE(parked) << "the writer never parked on the build's relation";
    EXPECT_FALSE(ran_past) << "the writer ran inside the create's window: "
                           << writer.out.response;
    EXPECT_NE(writer.out.response.find("ASSERTION_VIOLATION"), std::string::npos)
        << "the writer was not checked against the assertion: " << writer.out.response;
}

TEST(AssertionPublishRigTest, ACreateThatFailsAfterItsAdoptionLeavesNothingEnforcing) {
    // Core 1's create is adopted with its publish run, then refused at its
    // row: core 0 published the same name in the window, and assertion names
    // are instance-wide. The refused create's directory must leave the
    // registry - a relation whose writes are checked against an assertion no
    // catalog row names would refuse them for good.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    Relation(*rig, "cap_a");
    Relation(*rig, "cap_b");
    CommandDispatcher& d1 = rig->core(1).dispatcher();

    Statement mine;
    mine.sql = "CREATE ASSERTION cap ON cap_a GROUP BY (v) CHECK COUNT(*) <= 1";
    Statement theirs;
    theirs.sql = "CREATE ASSERTION cap ON cap_b GROUP BY (v) CHECK COUNT(*) <= 1";
    bool theirs_ran = false;
    d1.SetAfterAssertionPublishRunForTest([&] {
        theirs.go.store(true, std::memory_order_release);
        theirs_ran =
            KickUntil(*rig, 0, [&] { return theirs.done.load(std::memory_order_acquire); }, 5000ms);
    });
    Submit(*rig, 1, mine);
    Submit(*rig, 0, theirs);
    rig->Start();
    mine.go.store(true, std::memory_order_release);
    ASSERT_TRUE(
        KickUntil(*rig, 1, [&] { return mine.done.load(std::memory_order_acquire); }, 10000ms))
        << "the create never finished";
    rig->Stop();
    d1.SetAfterAssertionPublishRunForTest({});

    ASSERT_TRUE(theirs_ran) << "core 0's create never ran in the window";
    ASSERT_EQ(theirs.out.response.rfind("CREATED ASSERTION", 0), 0u) << theirs.out.response;
    ASSERT_EQ(mine.out.response.rfind("ERR", 0), 0u)
        << "the second create of one name was not refused: " << mine.out.response;

    // `cap_a` carries no assertion: both rows of one group are admitted.
    EXPECT_EQ(d1.Dispatch("INSERT INTO cap_a VALUES (1)").response.rfind("INSERTED", 0), 0u);
    const std::string second = d1.Dispatch("INSERT INTO cap_a VALUES (1)").response;
    EXPECT_EQ(second.rfind("INSERTED", 0), 0u)
        << "the refused create is still enforcing: " << second;
    // `cap_b` does.
    EXPECT_EQ(d1.Dispatch("INSERT INTO cap_b VALUES (1)").response.rfind("INSERTED", 0), 0u);
    EXPECT_NE(d1.Dispatch("INSERT INTO cap_b VALUES (1)").response.find("ASSERTION_VIOLATION"),
              std::string::npos);
}

}  // namespace
}  // namespace kds::server
