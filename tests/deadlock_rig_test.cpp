// AO-S4b: D12 across cores, on the two-core rig
// (`instructions/v3.0.0/workorder-ao-m2-lock-family.md`, the AO-S4b section).
//
// A cross-core cycle needs a wait on each core and a link between them, and
// the link is the shipped statement: a coordinator parked on its reply is
// waiting for the participant that runs it. AO-S4b makes the owner record
// that edge at enrolment, so the graph is complete and the registration
// that closes a cycle - wherever it runs - refuses the closer naming
// deadlock, with the refusal riding the reply to the coordinator's client.
// That last clause is `workorder-av-two-core-rig.md` §6's cell 5.
//
// Two relations, one per core, one row each; two sessions, one per core,
// each in an explicit transaction: each updates its own core's row, then
// ships an update of the other's. The second ship closes the cycle.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/session.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

// One session's life on its core, as a coroutine the reactor polls. Every
// statement is a child `DispatchAsync`, so a shipped statement parks this
// task the way it parks a client's. The SQL strings are members: a
// `DispatchAsync` takes a `string_view` and outlives the call.
struct Party {
    std::uint32_t core = 0;
    std::string local_update;
    std::string remote_update;
    Session session;
    DispatchOutcome begin_out;
    DispatchOutcome local_out;
    DispatchOutcome remote_out;
    DispatchOutcome end_out;
    std::function<bool()> go_pred;
    std::function<bool()> end_pred;
    std::atomic<bool> local_done{false};
    std::atomic<bool> go{false};
    std::atomic<bool> remote_done{false};
    std::atomic<bool> may_end{false};
    std::atomic<bool> ended{false};
    std::string ending = "ROLLBACK";
};

sched::Coro Run(CommandDispatcher& dispatcher, Party& p) {
    co_await dispatcher.DispatchAsync("BEGIN", &p.session, &p.begin_out);
    co_await dispatcher.DispatchAsync(p.local_update, &p.session, &p.local_out);
    p.local_done.store(true, std::memory_order_release);
    // Both hold before either ships, or there is no cycle to close.
    p.go_pred = [&p] { return p.go.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&p.go_pred};
    co_await dispatcher.DispatchAsync(p.remote_update, &p.session, &p.remote_out);
    p.remote_done.store(true, std::memory_order_release);
    p.end_pred = [&p] { return p.may_end.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&p.end_pred};
    co_await dispatcher.DispatchAsync(p.ending, &p.session, &p.end_out);
    p.ended.store(true, std::memory_order_release);
    co_return Status::OK();
}

struct DeadlockRig {
    explicit DeadlockRig(TwoCoreRig::Options options) {
        auto opened = TwoCoreRig::Open(options);
        EXPECT_TRUE(opened.ok()) << opened.status().message();
        if (!opened.ok()) return;
        rig = std::move(opened.value());
    }
    ~DeadlockRig() {
        if (rig != nullptr) rig->Stop();
    }

    // Two relations, one row each, one owner each - core 0's under
    // `kCreatingCore` and core 1's under `kRotate`, which at two cores
    // places everything on core 1 (`core_runtime_test.cpp`'s
    // `OpenCrossOwnerFkPair` recipe). Both seeded on the rig's thread
    // before any reactor runs, as its own core so the page latch's owner
    // field is honest.
    // A statement's answer as a `Status`, so a failed seed says which step.
    static Status Expect(const char* what, const std::string& response, const char* prefix) {
        if (response.rfind(prefix, 0) == 0) return Status::OK();
        return Status::InvalidArgument(std::string(what) + ": " + response);
    }

    Status Seed() {
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        rig->core(0).catalog().SetPlacementPolicy(catalog::PlacementPolicy::kCreatingCore);
        if (Status s = Expect("create r0",
                              d0.Dispatch("CREATE TABLE r0 (id int64, v int64) BTREE").response,
                              "CRE");
            !s.ok()) {
            return s;
        }
        rig->core(0).catalog().SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
        if (Status s = Expect("create r1",
                              d0.Dispatch("CREATE TABLE r1 (id int64, v int64) BTREE").response,
                              "CRE");
            !s.ok()) {
            return s;
        }
        auto r1 = rig->core(0).catalog().FindTableOidByName("r1");
        if (!r1.ok()) return r1.status();
        auto row = rig->core(0).catalog().GetSysTableRow(r1.value());
        if (!row.ok()) return row.status();
        if (row.value().owner_core != 1) {
            return Status::InvalidArgument("r1 was placed on core " +
                                           std::to_string(row.value().owner_core) + ", not 1");
        }
        if (Status s = rig->store().FlushPages(catalog::kEveryCatalogPage); !s.ok()) return s;
        if (Status s = rig->FundPeerRelation(r1.value()); !s.ok()) return s;
        if (Status s = Expect("insert r0", d0.Dispatch("INSERT INTO r0 VALUES (1, 0)").response,
                              "INSERTED");
            !s.ok()) {
            return s;
        }
        // The peer's row takes an engine-issued key: a caller-supplied
        // primary key is refused on a peer, because admitting one writes the
        // relation's catalog row, which is the system core's page
        // (`workplan-peer-writer.md` §7a). Its updates name no key for the
        // same reason - the relation holds one row, and that is the row.
        const CurrentCoreGuard as_core1(1);
        return Expect("insert r1",
                      rig->core(1).dispatcher().Dispatch("INSERT INTO r1 VALUES (0)").response,
                      "INSERTED");
    }

    void Submit() {
        a.core = 1;
        a.local_update = "UPDATE r1 SET v = 1";
        a.remote_update = "UPDATE r0 SET v = 1 WHERE id = 1";
        b.core = 0;
        b.local_update = "UPDATE r0 SET v = 2 WHERE id = 1";
        b.remote_update = "UPDATE r1 SET v = 2";
        rig->core(1).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, Run(rig->core(1).dispatcher(), a)));
        rig->core(0).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, Run(rig->core(0).dispatcher(), b)));
        rig->Start();
    }

    // The parties before the rig: their sessions, outcomes and predicates
    // are borrowed by coroutine frames the reactors hold, and reverse
    // destruction must tear the frames down before the storage they name.
    Party a;  // core 1's session: holds r1, ships to r0
    Party b;  // core 0's session: holds r0, ships to r1
    std::unique_ptr<TwoCoreRig> rig;
};

// `ATwoCoreCycleThroughShippedStatementsRefusesTheCloserAndItReachesTheClient` stood here until AT-S5: a cycle through two shipped statements needs two shipped writes, and a write runs where the session is since AT-S5; a two-core cycle through the lock table alone is `LockWakeRig`'s.

}  // namespace
}  // namespace kds::server
