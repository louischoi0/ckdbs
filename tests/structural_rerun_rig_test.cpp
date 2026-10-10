// **A structural refusal is re-run, not answered** (BA-S14, BA-R11, BA-Q10,
// `instructions/v3.0.0/workorder-ba-parallelism.md`), on the two-core rig.
//
// Two cores inserting into one relation keep splitting its rightmost leaf
// and growing its root, and a descent whose path one of those made stale is
// refused `TxnConflict retryable=1` after its bounded restarts. Until BA-S14
// that refusal reached the client; BA-S4's census counted 20. Now a
// statement that wrote nothing before it is re-run from the top after one
// yield, under the statement's deadline. The writers below never retry:
// every reply must be a success, and the re-run counter shows the refusals
// happened.

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
#include "kds/wal/durability.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

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

// `in_txn` wraps each core's inserts in one `BEGIN ... COMMIT`: there a
// re-run must not find its transaction poisoned by the refusal it re-runs.
void RunTwoWriters(bool in_txn) {
    const std::uint64_t reruns_before = Contention::Read().of(Tally::kStructuralReruns);
    constexpr int kRounds = 20;
    constexpr int kRows = 300;
    for (int round = 0;
         round < kRounds && Contention::Read().of(Tally::kStructuralReruns) == reruns_before;
         ++round) {
        TwoCoreRig::Options options;
        options.max_idle_block_ms = 20;
        auto opened = TwoCoreRig::Open(options);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
        ASSERT_EQ(rig->core(0).dispatcher().Dispatch("CREATE TABLE t (id int64, v int64)").response.rfind("CREATED", 0), 0u);
        Session s0, s1;
        s0.set_durability(wal::DurabilityClass::kRelaxed);
        s1.set_durability(wal::DurabilityClass::kRelaxed);
        Script a{&s0}, b{&s1};
        for (Script* s : {&a, &b}) {
            if (in_txn) s->lines.push_back("BEGIN");
            for (int i = 0; i < kRows; ++i) {
                s->lines.push_back("INSERT INTO t VALUES (" + std::to_string(i) + ")");
            }
            if (in_txn) s->lines.push_back("COMMIT");
        }
        a.go.store(true);
        b.go.store(true);
        rig->core(0).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, RunScript(rig->core(0).dispatcher(), a)));
        rig->core(1).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, RunScript(rig->core(1).dispatcher(), b)));
        struct StopAtExit {
            TwoCoreRig& rig;
            ~StopAtExit() { rig.Stop(); }
        } stop_at_exit{*rig};
        rig->Start();
        ASSERT_TRUE(Within(60000ms, [&] {
            return a.done.load() == a.lines.size() && b.done.load() == b.lines.size();
        })) << "the writers never finished: core 0 at " << a.done.load() << ", core 1 at " << b.done.load();
        for (const Script* s : {&a, &b}) {
            for (const DispatchOutcome& out : s->outs) {
                ASSERT_EQ(out.response.rfind("ERR", 0), std::string::npos)
                    << "a client was answered a refusal: " << out.response;
            }
        }
    }
    EXPECT_GT(Contention::Read().of(Tally::kStructuralReruns), reruns_before)
        << "no round met a structural refusal, so the re-run was never exercised";
}

TEST(StructuralRerunRigTest, TwoCoresSplittingOneRelationNeverAnswerAStaleDescent) {
    RunTwoWriters(/*in_txn=*/false);
}

TEST(StructuralRerunRigTest, AReRunInsideBeginFindsItsTransactionUsable) {
    RunTwoWriters(/*in_txn=*/true);
}

}  // namespace
}  // namespace kds::server
