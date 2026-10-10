// **A parked committer is kicked, not polled** (BA-S7 part 1, BA-R4,
// `instructions/v3.0.0/workorder-ba-parallelism.md`), on the two-core rig.
//
// A `group` commit parks on its durability point. Before BA-S7, a peer's
// drain reported the staged commit as work, so its reactor never blocked
// and polled the watermark until the writer's sync landed (BA-S4 counted
// 2.2 billion such passes), and core 0 synced inline on its reactor. Now
// both cores hand the sync to the writer, the drain reports nothing, the
// reactor blocks, and the writer kicks it when the watermark passes.
//
// The rig's idle block is 5 s, so a commit that waited for its reactor's
// block to time out instead of a kick misses the 2 s bound every cell
// below uses.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/contention.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/session.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/waker_table.hpp"
#include "kds/wal/durability.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

bool StartsWith(const std::string& reply, std::string_view prefix) {
    return reply.rfind(prefix, 0) == 0;
}

struct Script {
    Session* session = nullptr;
    std::vector<std::string> lines;
    std::vector<DispatchOutcome> outs;
    std::function<bool()> go_pred;
    std::atomic<bool> go{false};
    std::atomic<std::size_t> done{0};
};

sched::Coro RunScript(CommandDispatcher& d, Script& s) {
    s.go_pred = [&s] { return s.go.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&s.go_pred};
    s.outs.resize(s.lines.size());
    for (std::size_t i = 0; i < s.lines.size(); ++i) {
        co_await d.DispatchAsync(s.lines[i], s.session, &s.outs[i]);
        s.done.store(i + 1, std::memory_order_release);
    }
    co_return Status::OK();
}

// `rows` `group` inserts on core `core`, each answered within the bound.
void GroupCommitsAreKicked(std::uint32_t core) {
    auto opened = TwoCoreRig::Open({});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d = rig->core(core).dispatcher();
    ASSERT_TRUE(StartsWith(d.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));
    ASSERT_TRUE(rig->wal().KicksDurableWaiters()) << "the rig wires the writer's kick";

    Session session;
    session.set_durability(wal::DurabilityClass::kGroup);
    Script script;
    script.session = &session;
    constexpr int kRows = 5;
    for (int i = 0; i < kRows; ++i) script.lines.push_back("INSERT INTO t VALUES (" + std::to_string(i) + ")");
    rig->core(core).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d, script)));
    struct StopAtExit {
        TwoCoreRig& rig;
        ~StopAtExit() { rig.Stop(); }
    } stop_at_exit{*rig};

    // One commit at a time, each inside the bound: every one parks, and
    // only a kick ends its reactor's 5 s block in time. The start is set
    // before the reactor turns, so no test kick is needed - a single one can
    // be skipped (`WakerTable::Kick`) - and every commit is woken by the
    // writer alone.
    const Contention::Snapshot before = Contention::Read();
    script.go.store(true, std::memory_order_release);
    rig->Start();
    for (int i = 0; i < kRows; ++i) {
        ASSERT_TRUE(Within(2000ms, [&] { return script.done.load() > static_cast<std::size_t>(i); }))
            << "commit " << i << " on core " << core << " was not kicked; kicks skipped "
            << rig->wakers().kicks_skipped();
        EXPECT_TRUE(StartsWith(script.outs[i].response, "INSERTED")) << script.outs[i].response;
    }
    const Contention::Snapshot after = Contention::Read();
    // Every sync was the writer's; none ran on a reactor.
    EXPECT_EQ(after.of(Tally::kSyncsInline), before.of(Tally::kSyncsInline));
    EXPECT_GE(after.of(Tally::kSyncsWriter) - before.of(Tally::kSyncsWriter),
              static_cast<std::uint64_t>(kRows));
}

TEST(DurableKickRigTest, APeersGroupCommitIsKickedByTheWriter) {
    // Red before BA-S7 only in its cost: the peer polled, answered in time.
    GroupCommitsAreKicked(1);
}

TEST(DurableKickRigTest, AnIdleReactorDoesNotSpinOnAStagedCommit) {
    // The drain no longer reports a staged commit as work: with the
    // writer's sync held at the gate, the parked commit's reactor blocks
    // rather than taking a drain pass per iteration.
    TwoCoreRig::Options options;
    options.gated_log_sync = true;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    ASSERT_TRUE(StartsWith(d1.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));

    Session session;
    session.set_durability(wal::DurabilityClass::kGroup);
    Script script;
    script.session = &session;
    script.lines = {"INSERT INTO t VALUES (1)"};
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d1, script)));
    struct StopAtExit {
        TwoCoreRig& rig;
        ~StopAtExit() {
            rig.log_gate().Release();
            rig.Stop();
        }
    } stop_at_exit{*rig};
    rig->log_gate().Hold();
    script.go.store(true, std::memory_order_release);
    rig->Start();
    ASSERT_TRUE(Within(2000ms, [&] { return rig->log_gate().parked() >= 1; }))
        << "the writer never reached its sync";

    const std::uint64_t passes = Contention::Read().of(Tally::kDrainPassesPending);
    std::this_thread::sleep_for(300ms);
    // A spinning reactor takes a pass per iteration - hundreds of thousands
    // in 300 ms (BA-S4). A blocked one takes the post-task pass of the
    // statement that parked, and one per kick or timer at most.
    EXPECT_LT(Contention::Read().of(Tally::kDrainPassesPending) - passes, 100u);
    EXPECT_EQ(script.done.load(), 0u) << "the commit answered before its sync landed";

    rig->log_gate().Release();
    ASSERT_TRUE(Within(2000ms, [&] { return script.done.load() == 1; }))
        << "the sync landed and the parked commit was not kicked";
    EXPECT_TRUE(StartsWith(script.outs[0].response, "INSERTED")) << script.outs[0].response;
}

// **Core 0's arm** - the one rig cores never take, because both rig cores
// are attached to the stream. Under `Expeditor` core 0 owns it, and a
// `group` commit's drain on an owning manager synced inline until BA-S7.
class NullWakers final : public sched::WakeRegistry {
public:
    void Register(std::uint32_t, const std::atomic<bool>*, const sched::Waker*) override {}
    void Kick(std::uint32_t) const noexcept override {}
    std::uint64_t kicks() const noexcept override { return 0; }
};

// One `group` commit drained on an owning manager with a writer: which arm
// synced it.
void DrainOneGroupCommit(bool with_registry, std::uint64_t& inline_syncs,
                         std::uint64_t& writer_syncs) {
    auto device = wal::MemoryLogDevice::Create(wal::kDefaultSegmentSize);
    ASSERT_TRUE(device.ok()) << device.status().message();
    sched::ManualClock clock;
    auto opened = wal::WalManager::Open(device.value().get(), clock, /*core_id=*/0);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    wal::WalManager& wal = *opened.value();
    wal.StartWriter();
    NullWakers wakers;
    if (with_registry) wal.SetWakeRegistry(&wakers);
    EXPECT_EQ(wal.KicksDurableWaiters(), with_registry);

    const Contention::Snapshot before = Contention::Read();
    auto lsn = wal.Commit(/*txn_id=*/7, wal::DurabilityClass::kGroup);
    ASSERT_TRUE(lsn.ok()) << lsn.status().message();
    ASSERT_TRUE(wal.DrainOnce().ok());
    ASSERT_TRUE(Within(2000ms, [&] { return wal.IsDurable(lsn.value()); }));
    const Contention::Snapshot after = Contention::Read();
    inline_syncs = after.of(Tally::kSyncsInline) - before.of(Tally::kSyncsInline);
    writer_syncs = after.of(Tally::kSyncsWriter) - before.of(Tally::kSyncsWriter);
}

TEST(DurableKickRigTest, AnOwningManagersGroupSyncIsTheWritersOnceTheWriterKicks) {
    std::uint64_t inline_syncs = 0, writer_syncs = 0;
    DrainOneGroupCommit(/*with_registry=*/true, inline_syncs, writer_syncs);
    EXPECT_EQ(inline_syncs, 0u);
    EXPECT_GE(writer_syncs, 1u);
}

TEST(DurableKickRigTest, AtOneCoreAnOwningManagersGroupSyncStaysInline) {
    // No registry is set at `cores = 1`, so nothing would kick a parked
    // committer: the drain syncs inline, as before BA-S7.
    std::uint64_t inline_syncs = 0, writer_syncs = 0;
    DrainOneGroupCommit(/*with_registry=*/false, inline_syncs, writer_syncs);
    EXPECT_EQ(inline_syncs, 1u);
}

}  // namespace
}  // namespace kds::server
