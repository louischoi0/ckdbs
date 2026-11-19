// The lock family's fixture: one core, a WAL, a scheduler to park on and a
// lock table wired into both the dispatcher and the manager, driven through
// the dispatcher's served path. Shared by `lock_family_test.cpp` and
// `locking_read_test.cpp`, which park statements the same way.
//
// Everything runs on one reactor, so a wait ends by the slot flip its
// holder's decide makes, or at the fault net, never by a kick.

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"

namespace kds::server {

class LockFamilyTest : public ::testing::Test {
protected:
    static constexpr std::size_t kSegmentSize = 1 << 20;

    // The borrow cap of the table this fixture builds. The default is the
    // instance's; `lock_family_test.cpp`'s cap fixtures shrink it so a statement
    // reaches it.
    virtual std::size_t LockCap() const { return txn::kMaxLocksPerTxnDefault; }

    // `group` by default, because that is what a server runs;
    // `lock_family_test.cpp`'s failed commit fixture overrides it.
    virtual wal::DurabilityClass Durability() const { return wal::DurabilityClass::kGroup; }

    // **Declared here, above every member below, and it has to be**:
    // `~TransactionManager` releases the borrows of every still-live
    // transaction through this table (AO-R6's other end), so `txns_` must
    // die first and a member dies before the ones declared above it. Moving
    // this down beside `txns_` is a use-after-free at teardown, not a tidy.
    // Both production owners order it the same way (`expeditor.hpp`,
    // `core_runtime.hpp`).
    std::unique_ptr<txn::LockTable> locks_;

    void SetUp() override {
        auto device = wal::MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(device.ok()) << device.status().message();
        log_device_ = std::move(device.value());
        wal::WalManagerConfig config;
        config.ring_capacity = wal::kMinRingCapacity;
        auto wal = wal::WalManager::Open(log_device_.get(), clock_, /*core_id=*/0, config);
        ASSERT_TRUE(wal.ok()) << wal.status().message();
        wal_ = std::move(wal.value());

        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, wal_.get());
        // **The table goes into the manager as well as the dispatcher, and
        // it has to** (AO-S6c). The dispatcher takes a borrow and the manager
        // gives it back at the decide (`txn/manager.hpp`, AO-R6), so wiring
        // one and not the other builds a family whose tenancies accumulate:
        // the next statement then conflicts with rows whose writers
        // committed long ago. Production passes it to both
        // (`core_runtime.cpp`, `expeditor.cpp`).
        auto table = txn::LockTable::Create(/*core_count=*/1, LockCap());
        ASSERT_TRUE(table.ok()) << table.status().message();
        locks_ = std::move(table.value());
        txns_.emplace(*ids_, *undo_, store_, wal_.get(), /*visibility=*/nullptr, /*core=*/0,
                      locks_.get());
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr, &clock_,
                            wal_.get(), Durability(), exec::Budget(),
                            /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*txns_);
        dispatcher_->set_locks(locks_.get());
        scheduler_.emplace(clock_, io_);

        ASSERT_EQ(Local("CREATE TABLE t (id int64, v int64)").rfind("CREATED", 0), 0u);
    }

    std::string Local(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    std::string Rows() { return Local("SELECT * FROM t"); }

    // Starts a statement on the served path and returns its handles; the
    // caller decides what it is waiting to observe, which is the only way
    // to tell a park from a slow grant.
    //
    // **It owns the statement text, and that is not tidiness.**
    // `DispatchAsync` takes a `std::string_view` and is a coroutine, so the
    // view is copied into the frame while the characters are not: the
    // caller must keep them alive until the statement finishes. Every cell
    // that calls `DispatchAsync` directly passes a string *literal*, which
    // has static storage and hides the requirement; a helper taking
    // `const std::string&` binds a temporary that dies at the end of the
    // caller's statement, long before the first `Pump`, and the parked
    // coroutine then parses freed memory. That reads as `ERR unknown
    // command` from a statement that is plainly a valid `UPDATE`.
    struct Started {
        std::shared_ptr<std::string> sql = std::make_shared<std::string>();
        std::shared_ptr<DispatchOutcome> out = std::make_shared<DispatchOutcome>();
        std::shared_ptr<bool> done = std::make_shared<bool>(false);
    };

    Started Start(std::string sql, Session& session) {
        Started s;
        *s.sql = std::move(sql);
        scheduler_->Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground,
            dispatcher_->DispatchAsync(*s.sql, &session, s.out.get()),
            [d = s.done](const Status&) { *d = true; }));
        return s;
    }

    void Pump(int turns = 64) {
        for (int i = 0; i < turns; ++i) {
            (void)wal_->DrainOnce();
            scheduler_->RunOnce();
        }
    }

    // A statement run to its end on the served path, for a cell that
    // observes an outcome rather than a wait. **It fails rather than
    // returning a half-outcome**: a caller asserting on `response` of a
    // statement still parked would read the pre-wait reply
    // `DispatchAndStage` left there. A cell that means to observe a wait
    // uses `Start` and asserts on its own `done` flag.
    DispatchOutcome RunAsync(std::string sql, Session& session, int turns = 64) {
        Started s = Start(std::move(sql), session);
        for (int i = 0; i < turns && !*s.done; ++i) Pump(1);
        EXPECT_TRUE(*s.done) << "the statement was still parked after " << turns
                             << " turns, so what follows would read a stale reply: " << *s.sql;
        return *s.out;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    sched::ManualClock clock_;
    sched::NullIoBackend io_;
    std::unique_ptr<wal::MemoryLogDevice> log_device_;
    std::unique_ptr<wal::WalManager> wal_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> txns_;
    std::optional<CommandDispatcher> dispatcher_;
    std::optional<sched::Scheduler> scheduler_;
};

}  // namespace kds::server
