#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/functions.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wire/error_registry.hpp"

// The lock family on one core, driven through the dispatcher's served path
// (`instructions/v3.0.0/workorder-ay-following-letter.md`, AY-S1).
//
// **These cells are R8.3's, restored.** They lived in
// `tests/txn_2pc_protocol_test.cpp` on fixtures derived from its 2PC
// participant, and AT-S6 (`ac4bd64`) deleted the file whole with the 2PC
// service - taking five fixtures that test no 2PC with it, and leaving the
// deadlock detector, the borrow cap, the bind's declarations, the mid-walk
// park and `DROP TABLE`'s wait for a reader pinned by no cell
// (`docs/inflight/known-gaps.md`, Testing). The originals are at
// `git show ac4bd64^:tests/txn_2pc_protocol_test.cpp`.
//
// What they needed of the participant was one core, a WAL, a scheduler to
// park on and a lock table wired into both the dispatcher and the manager.
// `LockFamilyTest` is that base with the participant's machinery - its
// shipped-statement executor and its in-doubt ceiling - left out, because
// neither exists any more. Everything here runs on one reactor, so a wait
// ends by the slot flip its holder's decide makes, or at the fault net,
// never by a kick; the
// cross-core half is `row_wait_wake_rig_test.cpp`'s.

namespace kds::server {
namespace {

class LockFamilyTest : public ::testing::Test {
protected:
    static constexpr std::size_t kSegmentSize = 1 << 20;

    // The borrow cap of the table this fixture builds. The default is the
    // instance's; the cap fixtures below shrink it so a statement reaches it.
    virtual std::size_t LockCap() const { return txn::kMaxLocksPerTxnDefault; }

    // `group` by default, because that is what a server runs. The failed
    // commit fixture below overrides it.
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

        // **HEAP, pinned since SUS-1** (AS-R5, `instructions/v3.0.0/
        // workorder-as-sus1-heap-suspended.md`). `t` is this file's only
        // heap relation, and `MidWalkWaitTest` below spells out why it has
        // to stay one: it resumes its walk **by position** while `tb`
        // resumes **by key**, so on the flipped default the two arms would
        // be the same walk run twice and the position arm - AO-S3b's, the
        // one that already shipped a wrong answer once - would execute
        // nowhere. The test binary lifts the suspension
        // (`tests/heap_suspension_env.cpp`), which is what lets this word
        // stand.
        ASSERT_EQ(Local("CREATE TABLE t (id int64, v int64) HEAP").rfind("CREATED", 0), 0u);
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

// ---- The borrow cap ------------------------------------------------------
//
// AO-S6a's cap, on a table sized so three rows reach it: one entry for the
// relation `IX` and one per row, so a cap of 3 admits the relation and two
// rows and stops on the third. **The cap refuses** (AO-R10, since
// AO-S6c-c): the lock is the wait, so a truncated ledger would be rows with
// no fence over them and no wait behind them.
class LockCapTest : public LockFamilyTest {
protected:
    std::size_t LockCap() const override { return 3; }
};

TEST_F(LockCapTest, AWritePastTheBorrowCapIsRefusedAndNamesTheCap) {
    // **The cap refuses since AO-S6c-c.** It was swallowed while the borrow
    // was advisory, on the operator's decision of 2026-09-08: a ledger that
    // guards nothing must not fail a statement that would otherwise
    // succeed. That condition has lapsed - the lock is the wait now, and a
    // fence stops an insert - so a truncated ledger would be rows with no
    // fence over them and no wait behind them, which is AO-R10's reason for
    // refusing instead.
    //
    // The predicate names no pk window, which is the shape item 14's
    // declared units deliberately do not reach, so it still accumulates one
    // entry per row - and is the one shape that can meet the cap at all.
    Session session;
    ASSERT_EQ(Local("INSERT INTO t VALUES (1, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (2, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (3, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (4, 0)").rfind("INSERTED", 0), 0u);

    const DispatchOutcome out = RunAsync("UPDATE t SET v = 9 WHERE v = 0", session);
    EXPECT_EQ(out.status.code(), StatusCode::kResourceExhausted) << out.response;
    EXPECT_FALSE(IsRetryable(out.status.code()))
        << "a retry meets the same cap, so the wire's retryable bit must not invite one";
    EXPECT_EQ(out.resource_detail, static_cast<std::uint16_t>(wire::ResourceDetail::kLockCap))
        << "the client's fix for this is a shorter transaction, not a smaller statement, which "
           "is why protocol.md gave it a detail of its own - and it had no setter until now";
    EXPECT_GE(dispatcher_->borrow_cap_stops(), 1u);
}

TEST_F(LockCapTest, AWriteInsideTheBorrowCapCountsNoTruncation) {
    Session session;
    ASSERT_EQ(Local("INSERT INTO t VALUES (1, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (2, 0)").rfind("INSERTED", 0), 0u);

    // Two rows plus the relation is exactly the cap, and the cap refuses
    // the entry *at* it rather than past it - so two rows fit in three.
    // Per-row for the reason the cell above states.
    const DispatchOutcome out = RunAsync("UPDATE t SET v = 9 WHERE v = 0", session);
    EXPECT_EQ(out.response, "UPDATED 2") << out.response;
    EXPECT_EQ(dispatcher_->borrow_cap_stops(), 0u)
        << "a statement inside the cap must record every borrow it takes";
}

// ---- AO-0 item 14, marked 2026-09-08: the unit a bulk write declares ----
//
// Both cells are sized so the *difference* is what fails them: four rows
// under a cap of three, where per-row accumulation reaches the cap and a
// declared coarse unit does not. Without the declaration each of these
// counts a truncation.

TEST_F(LockCapTest, AWhereLessWriteDeclaresTheRelationAndTakesOneBorrow) {
    Session session;
    ASSERT_EQ(Local("INSERT INTO t VALUES (1, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (2, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (3, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (4, 0)").rfind("INSERTED", 0), 0u);

    // A write with no predicate touches every row there is, so it declares
    // the relation: one entry for four rows, and a cap of three is never
    // approached.
    const DispatchOutcome out = RunAsync("UPDATE t SET v = 9", session);
    EXPECT_EQ(out.response, "UPDATED 4") << out.response;
    EXPECT_EQ(dispatcher_->borrow_cap_stops(), 0u)
        << "a declared relation unit is one borrow; accumulating per row would reach the cap "
           "at the third of these four rows";
}

// ---- AO-S6c-a: every writer takes its borrow, and gives it back --------

TEST_F(LockCapTest, AnAutocommitStatementGivesItsBorrowsBackAtItsDecide) {
    // **The cell that would have caught the half-wired fixture.** Borrows
    // are released by the *manager* at a decide (AO-R6), not by the
    // dispatcher, so a table wired into one and not the other takes
    // tenancies and never gives them back - and the next statement then
    // conflicts with rows whose writers committed long ago. That is what
    // this fixture did until AO-S6c, invisibly, because only UPDATE and
    // DELETE borrowed and no cell ran two writes over the same rows.
    ASSERT_EQ(locks_->EntryCount(), 0u) << "nothing is held before the first statement";
    ASSERT_EQ(Local("INSERT INTO t VALUES (1, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (2, 0)").rfind("INSERTED", 0), 0u);
    EXPECT_EQ(locks_->EntryCount(), 0u)
        << "two committed autocommit inserts left tenancies in the table; the manager is what "
           "releases them and it must have been given the table";

    Session session;
    const DispatchOutcome out = RunAsync("UPDATE t SET v = 9 WHERE v = 0", session);
    ASSERT_EQ(out.response, "UPDATED 2") << out.response;
    EXPECT_EQ(locks_->EntryCount(), 0u) << "and an autocommit UPDATE's borrows go the same way";
}

// A cap of one, so the *declaration itself* is what the cap refuses.
class LockCapOfOneTest : public LockFamilyTest {
protected:
    std::size_t LockCap() const override { return 1; }
};

TEST_F(LockCapOfOneTest, ADeclaredUnitTheCapRefusesEndsTheStatement) {
    // The behaviour that replaced `ARefusedDeclarationFallsBackToThePerRowBorrow`
    // when AO-S6c-c removed the demotion, and the review's B4: under a cap
    // of one the relation `IX` fills the ledger and the range under it is
    // refused, so the statement ends there having written nothing rather
    // than falling back to a per-row walk that would meet the same cap on
    // its first row.
    //
    // No row is seeded, and none is needed: under this cap no INSERT can
    // succeed either, and the declaration is borrowed **before** the walk -
    // so what the cell measures happens whether or not the relation is
    // empty, which is itself the point.
    Session session;
    const DispatchOutcome out = RunAsync("UPDATE t SET v = 9 WHERE id > 0", session);
    EXPECT_EQ(out.status.code(), StatusCode::kResourceExhausted) << out.response;
    EXPECT_EQ(out.resource_detail, static_cast<std::uint16_t>(wire::ResourceDetail::kLockCap))
        << out.response;
}

TEST_F(LockCapOfOneTest, AnInsertTakesTheBorrowOfTheRowItWrites) {
    // AO-S6c-a. Until this sub-stage an INSERT borrowed nothing, which was
    // not a gap but the thing standing between AO-S6 and its own goal: the
    // wait's blocker is moving from the tuple header to the lock table, and
    // a row whose only writer was an INSERT would have a header `trx_id`
    // and no entry - so the table alone would answer that the row is free.
    //
    // A cap of one is what makes the borrow observable without a second
    // session: the relation `IX` fills the ledger, so the row's own tuple
    // is refused by the cap and counted. A statement that borrowed nothing
    // would count nothing.
    // A cap of one admits the relation `IX` and nothing under it, so an
    // insert that borrows its row is refused and an insert that borrows
    // nothing sails through. Since AO-S6c-c that refusal is the statement's,
    // which is a sharper observation than the counter this used to read.
    // **Observed on the outcome, not on the rendered line.** A text-only
    // check passed while the cap's refusal was reaching a KWP client as
    // `INVALID_ARGUMENT` carrying the cap's detail - the review's B1 - so
    // the category and the detail are what this asserts.
    Session session;
    const DispatchOutcome out = RunAsync("INSERT INTO t VALUES (1, 0)", session);
    EXPECT_EQ(out.status.code(), StatusCode::kResourceExhausted)
        << "the INSERT took no borrow, so the wait's blocker cannot come from the table: "
        << out.response;
    EXPECT_EQ(out.resource_detail, static_cast<std::uint16_t>(wire::ResourceDetail::kLockCap))
        << out.response;
}

TEST_F(LockCapOfOneTest, ASortedFillBorrowsTheIdBlockItCarved) {
    // The INSERT path that routes **past** `InsertOneRow`: `t` is HEAP with
    // no var-heap, no index, no cabin and no assertion, so a multi-row
    // VALUES whose every row omits its pk goes to `SortedFillInner`. Until
    // AO-S6c-a it wrote rows and borrowed nothing at all - the one writer
    // left in the "header `trx_id`, no table entry" shape the sub-stage
    // exists to remove, and the review is what found it.
    //
    // One `Range` over the carved block rather than an entry per row, so
    // the observation is the same as any coarse unit's under a cap of one:
    // the relation `IX` fills the ledger and the range itself is refused
    // and counted. A run that borrowed nothing would count nothing.
    // As the single-row cell above: the cap admits the relation `IX` and
    // refuses the range under it, so a fill that borrows its block is
    // refused and one that borrows nothing is not.
    Session session;
    const DispatchOutcome out = RunAsync("INSERT INTO t VALUES (5), (6)", session);
    EXPECT_EQ(out.status.code(), StatusCode::kResourceExhausted)
        << "the sorted fill placed rows without borrowing the ids it carved: " << out.response;
    EXPECT_EQ(out.resource_detail, static_cast<std::uint16_t>(wire::ResourceDetail::kLockCap))
        << out.response;
}

TEST_F(LockCapTest, ARangePredicateDeclaresItsWindowRatherThanAccumulatingRows) {
    Session session;
    ASSERT_EQ(Local("INSERT INTO t VALUES (1, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (2, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (3, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (4, 0)").rfind("INSERTED", 0), 0u);

    // `id > 1` is a pk window of three rows. Declared, it is the relation
    // `IX` plus one range - two entries under a cap of three. Per-row it
    // would be the `IX` plus three tuples, which reaches the cap on the
    // last of them.
    const DispatchOutcome out = RunAsync("UPDATE t SET v = 9 WHERE id > 1", session);
    EXPECT_EQ(out.response, "UPDATED 3") << out.response;
    EXPECT_EQ(dispatcher_->borrow_cap_stops(), 0u)
        << "a range-shaped predicate declares one window; accumulating its rows would reach "
           "the cap";
}

// The default cap; the name is the cells', kept from the file they came
// from.
class LockDeadlockTest : public LockFamilyTest {
protected:
    // BB-S2's guard, defined beside the cells that use it.
    void SpilledInsertWaitsOnAFence(const std::string& insert);
};

// ---- AO-S6e-b: the read borrow, and the DDL that waits for one ---------

// The oid an INSERT reports, which is how a cell names a relation the
// dispatcher created (`drop-table.md` DT2 uses the same handle).
std::uint64_t OidIn(const std::string& insert_reply) {
    const auto at = insert_reply.find("oid=");
    EXPECT_NE(at, std::string::npos) << insert_reply;
    return at == std::string::npos ? 0 : std::strtoull(insert_reply.c_str() + at + 4, nullptr, 10);
}

TEST_F(LockDeadlockTest, AReadDeclaresItsPositionAndGivesItBack) {
    // AR2 §3's `SELECT` row: the borrow is **the statement's**. Taken at
    // the bind since AT-S1 (`step_compiler.cpp`'s seam), re-reported into
    // by the walk, and released when the statement ends,
    // which is what `EntryCount` reads here - a read that kept its position
    // would leave the relation entry standing and a later `DROP TABLE`
    // would wait for a reader that finished long ago. **The count does not
    // pin the bind**: one relation's walk reports its own first position
    // (`step_vm.cpp`'s `index == 0` guard), so this reads `+1` with the
    // bind's declaration removed; the join and subquery cells below pin it.
    ASSERT_EQ(Local("CREATE TABLE rb (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO rb VALUES (1, 1)").rfind("INSERTED", 0), 0u);

    const std::uint64_t before = dispatcher_->read_borrows();
    const std::string rows = Local("SELECT * FROM rb");
    EXPECT_NE(rows.rfind("ERR", 0), 0u) << rows;
    EXPECT_EQ(dispatcher_->read_borrows(), before + 1)
        << "the walk declared no position at all";
    EXPECT_EQ(locks_->EntryCount(), 0u) << "a statement-scoped borrow outlived its statement";
}

TEST_F(LockDeadlockTest, ADropWaitsForAPositionedReaderAndThenRunsAgain) {
    // The AO-S6 row's cell. The reader's borrow is held here directly
    // rather than by a running `SELECT`, and that is a property of the
    // engine rather than of the cell: a local read is synchronous, so a
    // statement on this core cannot observe another one mid-walk. What the
    // holder stands for is exactly what a walk takes -
    // `Relation(oid)` in `IS` under a read holder id - and
    // `AReadDeclaresItsPositionAndGivesItBack` above is what says a real
    // read takes it.
    ASSERT_EQ(Local("CREATE TABLE rb (id int64, v int64)").rfind("CREATED", 0), 0u);
    const std::uint64_t oid = OidIn(Local("INSERT INTO rb VALUES (1, 1)"));
    ASSERT_NE(oid, 0u);

    txn::LockHoldings reader;
    const std::uint64_t reader_id = txn::kReadHolderBit | 7;
    ASSERT_TRUE(locks_
                    ->Acquire(reader_id, txn::LockKey::Relation(static_cast<catalog::Oid>(oid)),
                              txn::LockMode::kIntentionShared, reader)
                    .value()
                    .granted);

    Session s;
    Started drop = Start("DROP TABLE rb", s);
    Pump();
    ASSERT_FALSE(*drop.done) << "the drop ran over a positioned reader: " << drop.out->response;

    // And the wait ends where the position does. The re-run is a whole
    // statement - the drop had written nothing when it was refused - so
    // what the client gets is the reply the first attempt would have given.
    locks_->Release(reader_id, reader);
    Pump();
    ASSERT_TRUE(*drop.done) << "the drop never resumed after the reader released";
    EXPECT_EQ(drop.out->response.rfind("DROPPED TABLE rb", 0), 0u) << drop.out->response;
    EXPECT_EQ(locks_->EntryCount(), 0u) << "the wake registration outlived its wait";
}

TEST_F(LockDeadlockTest, ADropWithNoReactorToParkOnNamesThePositionedReader) {
    // The synchronous path has no reactor, so its honest answer is the
    // refusal itself - the same division `index_window` and `write_block`
    // draw. What it must not do is name a transaction: the holder is a
    // statement's read borrow, and an operator sent looking for
    // "transaction 9223372036854775815" would be looking for something that
    // never existed.
    ASSERT_EQ(Local("CREATE TABLE rb (id int64, v int64)").rfind("CREATED", 0), 0u);
    const std::uint64_t oid = OidIn(Local("INSERT INTO rb VALUES (1, 1)"));
    ASSERT_NE(oid, 0u);

    txn::LockHoldings reader;
    const std::uint64_t reader_id = txn::kReadHolderBit | 7;
    ASSERT_TRUE(locks_
                    ->Acquire(reader_id, txn::LockKey::Relation(static_cast<catalog::Oid>(oid)),
                              txn::LockMode::kIntentionShared, reader)
                    .value()
                    .granted);

    const std::string refused = Local("DROP TABLE rb");
    EXPECT_EQ(refused.rfind("ERR", 0), 0u) << refused;
    EXPECT_NE(refused.find("a positioned reader"), std::string::npos) << refused;

    locks_->Release(reader_id, reader);
    EXPECT_EQ(Local("DROP TABLE rb").rfind("DROPPED TABLE rb", 0), 0u);
}

TEST_F(LockDeadlockTest, AWholeRelationWriteIsNotHeldUpByAPositionedReader) {
    // **The rule at the unit where it actually lands** (the AO-S6e-b
    // review's B2). `ConflictingOverlap` skips intention holders, so a
    // reader's slice `IS` passes a declared range - but a `WHERE`-less
    // write used to collapse onto the **relation** unit, where the conflict
    // is decided by the entry's own compatibility test and `IS` against `X`
    // is a refusal. So the rule was written on one path and the traffic ran
    // down another: a scan of the relation would have refused
    // `DELETE FROM rb`, and refused rather than waited, because a read
    // borrow's holder is not a transaction and nothing can wait for it.
    //
    // Since AO-S6e-b a `WHERE`-less write declares the whole id space as a
    // range: the relation unit means the relation as an object, which is
    // what DDL claims, and a write's claim is over keys.
    ASSERT_EQ(Local("CREATE TABLE rb (id int64, v int64)").rfind("CREATED", 0), 0u);
    const std::uint64_t oid = OidIn(Local("INSERT INTO rb VALUES (1, 1)"));
    ASSERT_NE(oid, 0u);

    txn::LockHoldings reader;
    const std::uint64_t reader_id = txn::kReadHolderBit | 13;
    ASSERT_TRUE(locks_
                    ->Acquire(reader_id, txn::LockKey::Relation(static_cast<catalog::Oid>(oid)),
                              txn::LockMode::kIntentionShared, reader)
                    .value()
                    .granted);
    // And the slice under it, which is the other half of what a walk holds.
    ASSERT_TRUE(locks_
                    ->Acquire(reader_id,
                              txn::LockKey::Slice(static_cast<catalog::Oid>(oid), 1,
                                                  kIdSpaceEnd),
                              txn::LockMode::kIntentionShared, reader)
                    .value()
                    .granted);

    const std::string deleted = Local("DELETE FROM rb");
    EXPECT_EQ(deleted.rfind("DELETED", 0), 0u)
        << "a positioned reader held up a write of every row, which MVCC answers it from its "
           "snapshot: "
        << deleted;

    const std::string rows = Local("SELECT * FROM rb");
    EXPECT_NE(rows.rfind("ERR", 0), 0u) << "the read that follows it is refused: " << rows;

    locks_->Release(reader_id, reader);
}

TEST_F(LockDeadlockTest, ADropInsideATransactionWaitsWithoutPoisoningIt) {
    // The shape the autocommit cells do not reach, and the one that would
    // have made the wait worse than the refusal it replaced: a `DROP TABLE`
    // inside an explicit transaction (DT5) that meets a reader. `EndWrite`
    // poisons a transaction whose statement failed - so without the rule
    // that withholds it while a wait is still possible, the re-run would
    // answer "transaction is aborted", non-retryable, where the client used
    // to get its relation dropped. It is `blocking_writer_`'s own rule
    // applied to the second failure that a wait can get past.
    ASSERT_EQ(Local("CREATE TABLE rb (id int64, v int64)").rfind("CREATED", 0), 0u);
    const std::uint64_t oid = OidIn(Local("INSERT INTO rb VALUES (1, 1)"));
    ASSERT_NE(oid, 0u);

    txn::LockHoldings reader;
    const std::uint64_t reader_id = txn::kReadHolderBit | 9;
    ASSERT_TRUE(locks_
                    ->Acquire(reader_id, txn::LockKey::Relation(static_cast<catalog::Oid>(oid)),
                              txn::LockMode::kIntentionShared, reader)
                    .value()
                    .granted);

    Session s;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &s).response.rfind("BEGIN", 0), 0u);
    Started drop = Start("DROP TABLE rb", s);
    Pump();
    ASSERT_FALSE(*drop.done) << "the drop ran over a positioned reader: " << drop.out->response;

    locks_->Release(reader_id, reader);
    Pump();
    ASSERT_TRUE(*drop.done) << "the drop never resumed";
    EXPECT_EQ(drop.out->response.rfind("DROPPED TABLE rb", 0), 0u) << drop.out->response;

    // And the transaction is still a transaction: a poisoned one answers
    // this with "transaction is aborted".
    EXPECT_EQ(dispatcher_->Dispatch("COMMIT", &s).response.rfind("COMMIT", 0), 0u);
}

TEST_F(LockDeadlockTest, ADropWhoseWaitReachesTheFaultNetPoisonsItsTransaction) {
    // **The exit the second review found untested, and it was wrong.**
    // `EndWrite` withholds the poison a failed statement owes an explicit
    // transaction while a wait is still possible - or the re-run would
    // answer "transaction is aborted" instead of dropping the relation.
    // Two of the three exits restore it (the victim through
    // `RefuseParkedWrite`, a failing re-run through `EndWrite` itself, the
    // field being empty by then); the fault net is the third, and without
    // it a refusal at the net left the transaction usable and
    // committable - §6's failure atomicity broken by a wait that is
    // supposed to be invisible when it works.
    ASSERT_EQ(Local("CREATE TABLE rb (id int64, v int64)").rfind("CREATED", 0), 0u);
    const std::uint64_t oid = OidIn(Local("INSERT INTO rb VALUES (1, 1)"));
    ASSERT_NE(oid, 0u);

    txn::LockHoldings reader;
    const std::uint64_t reader_id = txn::kReadHolderBit | 21;
    ASSERT_TRUE(locks_
                    ->Acquire(reader_id, txn::LockKey::Relation(static_cast<catalog::Oid>(oid)),
                              txn::LockMode::kIntentionShared, reader)
                    .value()
                    .granted);

    Session s;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &s).response.rfind("BEGIN", 0), 0u);
    Started drop = Start("DROP TABLE rb", s);
    Pump();
    ASSERT_FALSE(*drop.done) << drop.out->response;

    // The holder never releases. The net is the only thing that can end
    // this, and reaching it is a defect report rather than an outcome
    // (AO-R8) - which does not make it any less a failed statement.
    clock_.Advance(txn::kLockWaitFaultNetNs + 1);
    Pump();
    ASSERT_TRUE(*drop.done) << "the wait outlived its own fault net";
    EXPECT_EQ(drop.out->response.rfind("ERR", 0), 0u) << drop.out->response;
    const std::string commit = dispatcher_->Dispatch("COMMIT", &s).response;
    EXPECT_EQ(commit.rfind("ERR", 0), 0u)
        << "the transaction committed after a statement of it failed: " << commit;

    locks_->Release(reader_id, reader);
}

TEST_F(LockDeadlockTest, ARowWaitAtTheFaultNetIsRefusedRetryablyAndNamesTheNet) {
    // The premise of `Txn2pcBlockedWriterTest.AtTheFaultNetTheWriterIsAbortedAndTheRefusalNamesTheNet`,
    // which AT-S6 deleted because its holder was a prepared 2PC participant.
    // The net outlived the holder: restored at AY-S1 over an ordinary open
    // transaction.
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 7", &holder).response,
              "UPDATED 1");

    Session w;
    Started wait = Start("UPDATE t SET v = 3 WHERE id = 7", w);
    Pump();
    ASSERT_FALSE(*wait.done) << "the writer answered instead of waiting: " << wait.out->response;

    // The holder never decides; the net (AO-R8) is the only thing that ends
    // this, and a hang is not the alternative (HP3).
    clock_.Advance(txn::kLockWaitFaultNetNs + 1);
    Pump();
    ASSERT_TRUE(*wait.done) << "the wait has no bound at all, which is the hang HP3 forbids";

    // Retryable, so a client's loop reads the bit the engine means; *not*
    // `UnknownOutcome`, which would send a client to read back data its
    // statement never touched; and named as the **net**, on the rendered
    // line and on the carried status a KWP client reads, so an operator
    // meeting it looks for the fault rather than concluding the row was
    // busy.
    const Status refused = StatusFromErrorReply(wait.out->response);
    EXPECT_EQ(refused.code(), StatusCode::kTxnConflict) << wait.out->response;
    EXPECT_TRUE(refused.retryable()) << wait.out->response;
    EXPECT_NE(wait.out->response.find("fault net"), std::string::npos) << wait.out->response;
    EXPECT_NE(wait.out->status.message().find("fault net"), std::string::npos)
        << wait.out->status.message();

    // The net refuses the waiter and never the holder.
    EXPECT_EQ(dispatcher_->Dispatch("COMMIT", &holder).response.rfind("COMMIT", 0), 0u);
    EXPECT_NE(Rows().find(",2"), std::string::npos) << Rows();
}

// ---- AY-Q9: what the base fixture's own cells pinned ----------------------
//
// `Txn2pcBlockedWriterTest` held seven cells of its own that test no 2PC
// (old `:3833-4156`), run with **no lock table** - an arm no production
// assembly builds. AY-Q9, marked as proposed on 2026-09-29, ports the
// premises that still hold onto this table-backed base. Four do and are
// below. Three are not ported: `ATransactionThatAlreadyWroteIsRefusedRatherThanWaited`
// is the no-table guard, which `WithoutATableTheNarrowGuardIsWhatKeepsTheStageSafe`
// pins in the same shape and which a table inverts
// (`ATwoCycleAbortsTheWaiterThatClosedItAndTheOtherProceeds`); the child
// that waits out a parent's commit, and the one whose parent rolls back,
// are pinned table-backed on the two-core rig (`fk_cross_core_rig_test.cpp`).

TEST_F(LockDeadlockTest, ARepeatableReadWriterIsRefusedRatherThanOfferedANarrowerWait) {
    // The repeatable-read guard in `NoteBlockingWriter`. The view is minted
    // at `BEGIN` and never re-minted, so a holder that **commits** after it
    // stays invisible and the re-run refuses on the ground it refused on the
    // first time - a stall ending in the refusal already owed. Where the
    // blocker is the row's own writer - the header names it - the wait is
    // futile and the level keeps the refusal. The fence-holder cells below
    // are the case that waits.
    //
    // **The exclusion is conservative rather than exact**: a holder that
    // *aborts* restores the row's prior writer id, and the same view would
    // then admit the write. A wait that pays off only on a rollback is a
    // narrower promise than the family makes elsewhere.
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);

    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 7", &holder).response,
              "UPDATED 1");

    Session rr;
    ASSERT_EQ(dispatcher_->Dispatch("SET ISOLATION LEVEL REPEATABLE READ", &rr)
                  .response.rfind("ERR", 0),
              std::string::npos)
        << "the level must be settable for this cell to mean anything";
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &rr).response.rfind("BEGIN", 0), 0u);

    Started w = Start("UPDATE t SET v = 3 WHERE id = 7", rr);
    Pump();
    ASSERT_TRUE(*w.done) << "a repeatable-read writer waited for a decision its own view will "
                            "never see: " << w.out->response;
    EXPECT_EQ(StatusFromErrorReply(w.out->response).code(), StatusCode::kTxnConflict)
        << w.out->response;
    EXPECT_EQ(locks_->WaitEdgeCount(), 0u) << "a refused wait left its edge";
}

TEST_F(LockDeadlockTest, ARepeatableReadChildWaitsOutItsParentBecauseTheCheckViewIsFresh) {
    // AO-S6d's item 17, on the forward check. A constraint check reads
    // latest state - `CheckView` mints a view of *now* whatever the level -
    // so the wait pays off in both arms: the parent's commit makes it
    // visible to the re-run, its abort makes the answer a terminal
    // `FkViolation`. A level test here would answer the same `INSERT`
    // differently depending on nothing the client can see.
    //
    // **The mutation** (measured at AY-Q9, against the wait's shape then):
    // a repeatable-read test on the parent row's `S` ask - which the
    // hoist's `BorrowOrWait` makes with `kCapable` since AY-S5 - and the
    // child is refused `TxnConflict` here, where the same statement at
    // READ COMMITTED waits.
    ASSERT_EQ(Local("CREATE TABLE accounts (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("CREATE TABLE orders (id int64, account_id int64 REFERENCES accounts) BTREE")
                  .rfind("CREATED", 0),
              0u);

    Session parent;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &parent).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO accounts VALUES (5, 1)", &parent)
                  .response.rfind("INSERTED", 0),
              0u);

    Session child;
    ASSERT_EQ(dispatcher_->Dispatch("SET ISOLATION LEVEL REPEATABLE READ", &child)
                  .response.rfind("ERR", 0),
              std::string::npos);
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &child).response.rfind("BEGIN", 0), 0u);

    Started insert = Start("INSERT INTO orders VALUES (1, 5)", child);
    Pump();
    ASSERT_FALSE(*insert.done) << "the repeatable-read child was refused instead of waiting for "
                                  "its parent: " << insert.out->response;

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &parent).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*insert.done) << "the wait never ended";
    EXPECT_EQ(insert.out->response.rfind("INSERTED", 0), 0u)
        << "the parent committed before the check view was minted, so the child is legal: "
        << insert.out->response;
}

TEST_F(LockDeadlockTest, AnAutocommitWriterWaitsOutALocalHolderAndThenSeesItsValue) {
    // AO-5's S3 row: an autocommit `UPDATE` against a row an open
    // transaction holds returns after its `COMMIT`, and writes over the
    // value the commit left - the re-check is mandatory, so it does not
    // resume with the answer it had when it parked.
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);

    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 7", &holder).response,
              "UPDATED 1");

    Session waiter;
    Started w = Start("UPDATE t SET v = 3 WHERE id = 7", waiter);
    Pump();
    ASSERT_FALSE(*w.done) << "the writer was refused instead of waiting: " << w.out->response;

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &holder).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*w.done) << "the wait never ended";
    EXPECT_EQ(w.out->response, "UPDATED 1") << w.out->response;
    EXPECT_NE(Rows().find(",3"), std::string::npos) << Rows();
}

TEST_F(LockDeadlockTest, AWriterWaitingOutAHolderThatRollsBackWritesOverThePriorVersion) {
    // The other decide. The holder's compensations put the old value back
    // before its borrows go (AO-R6's ordering), so the waiter re-runs
    // against the version that was there all along rather than a
    // half-undone one.
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);

    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 7", &holder).response,
              "UPDATED 1");

    Session waiter;
    Started w = Start("UPDATE t SET v = 3 WHERE id = 7", waiter);
    Pump();
    ASSERT_FALSE(*w.done) << w.out->response;

    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*w.done) << "an abort must end the wait exactly as a commit does";
    EXPECT_EQ(w.out->response, "UPDATED 1") << w.out->response;
    EXPECT_NE(Rows().find(",3"), std::string::npos) << Rows();
}

TEST_F(LockDeadlockTest, ThePathThatCannotWaitPoisonsExactlyAsItAlwaysDid) {
    // One of the six uncounted cells (`known-gaps.md`, Testing), whose
    // holder was a prepared participant; ported here over an ordinary one.
    // **The poison is withheld for the wait, so where there is no wait it
    // must stand.** `Dispatch()` has no reactor to park on and answers the
    // conflict itself; a failed statement inside an explicit transaction
    // poisons the session whatever refused it (txn.md §6 - failure atomicity
    // is per transaction), and a session left unpoisoned here would tell the
    // client `ERR` and then let its COMMIT succeed without the statement.
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 7", &holder).response,
              "UPDATED 1");

    Session local;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &local).response.rfind("BEGIN", 0), 0u);
    const DispatchOutcome out = dispatcher_->Dispatch("UPDATE t SET v = 3 WHERE id = 7", &local);
    EXPECT_EQ(StatusFromErrorReply(out.response).code(), StatusCode::kTxnConflict)
        << out.response;
    EXPECT_TRUE(local.failed()) << "the failed statement left the transaction committable";
    EXPECT_NE(dispatcher_->Dispatch("COMMIT", &local).response.rfind("COMMIT", 0), 0u);
    EXPECT_EQ(dispatcher_->Dispatch("ROLLBACK", &local).response.rfind("ROLLBACK", 0), 0u);
    EXPECT_EQ(locks_->EntryCount(), 2u) << "only the holder's relation IX and row X remain";
}

// ---- AY-S2: the containment wake ------------------------------------------

TEST_F(LockDeadlockTest, AChildWhoseParentIsUnderARangeFenceWaitsForTheFence) {
    // A parent `DELETE` with a pk window declares a range `X` and takes no
    // tuple `X` per row, so the child's forward check - its tuple `S` on the
    // parent row, held from before the descent since AY-S5 - meets the fence
    // only in the lock table's verify arm. Until AY-S2 that refusal had no
    // slot and the child was refused at once; it now waits on the fence's
    // entry for its release.
    //
    // **Mutation**: the verify's scans registering nothing - the child is
    // answered at once, with the busy verdict.
    ASSERT_EQ(Local("CREATE TABLE p (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("CREATE TABLE c (id int64, pid int64 REFERENCES p) BTREE").rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Local("INSERT INTO p VALUES (5, 0)").rfind("INSERTED", 0), 0u);

    Session deleter;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &deleter).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("DELETE FROM p WHERE id > 0 AND id < 100", &deleter).response,
              "DELETED 1");

    Session child;
    Started insert = Start("INSERT INTO c VALUES (1, 5)", child);
    Pump();
    ASSERT_FALSE(*insert.done) << "the child was answered instead of waiting for the fence: "
                               << insert.out->response;

    // The deleter rolls back, the parent row is live again, and the child's
    // re-run finds it.
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &deleter).response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*insert.done) << "the child never resumed after the fence was released";
    EXPECT_EQ(insert.out->response.rfind("INSERTED", 0), 0u) << insert.out->response;
    EXPECT_EQ(locks_->EntryCount(), 0u) << "the fence's registration outlived the wait";
}

// ---- AT-S3: the sys.tables row has no contender the page latch does not serialise --
//
// E13 asked whether the relation's `sys.tables` row becomes borrowable at
// the tuple unit - `X` on the row - so that a named-key `INSERT` waits on
// it. The answer is no, and this cell pins why. Admitting a named
// key writes the row's mark or flips its key order **outside the caller's
// transaction** (`wal::kNoTxnId`; `heap-and-tuple.md` §4.1: "both writes
// outlive a rollback"), under the page latch - a monotone in-place
// overwrite, not a row version. A transaction-length `X` on that row would
// serialise every named-key insert into a relation for the length of each
// transaction, and protect nothing the latch does not. Another core's
// named-key insert meets the same row under the same page latch, since
// AT-S5 wrote every relation's pages from wherever the session is.

TEST_F(LockDeadlockTest, TwoTransactionsNamedKeysIntoOneRelationDoNotWaitOnItsCatalogRow) {
    ASSERT_EQ(Local("CREATE TABLE nk (id int64, v int64)").rfind("CREATED", 0), 0u);

    Session a;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO nk VALUES (7, 1)", &a).response.rfind("INSERTED", 0),
              0u);
    // `a` holds its user row's `X` and has moved the relation's mark. A
    // second transaction naming another key admits at once: the only thing
    // it shares with `a` is the catalog row, and nothing borrows that.
    Session b;
    Started second = Start("INSERT INTO nk VALUES (8, 1)", b);
    Pump();
    ASSERT_TRUE(*second.done) << "a named key waited on another transaction's catalog-row write";
    EXPECT_EQ(second.out->response.rfind("INSERTED", 0), 0u) << second.out->response;

    // And the ledger holds `a`'s two borrows on the user relation - the
    // relation `IX` and row 7's `X` (AO-S6a) - and nothing keyed on the
    // catalog relation, which is the third entry a row `X` would have added.
    // Probed, not only counted: an `X` on the catalog relation is granted
    // at once, which it could not be if anything stood there.
    EXPECT_EQ(locks_->EntryCount(), 2u) << "a borrow stands on something other than a's relation and row";
    txn::LockHoldings probe;
    auto free = locks_->TryAcquire(/*txn=*/4242, txn::LockKey::Relation(catalog::kSysTablesTable),
                                   txn::LockMode::kExclusive, probe);
    ASSERT_TRUE(free.ok());
    EXPECT_TRUE(free.value()) << "something holds the catalog relation while a's key is admitted";
    locks_->Release(4242, probe);
    // This fixture is one core over an in-memory store, where no page latch
    // is armed: it proves the ledger claim and nothing about the latch.
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    EXPECT_EQ(locks_->EntryCount(), 0u);
}

// ---- AT-S1: the declaration moves to the bind ---------------------------
//
// AT-R1 (`instructions/v3.0.0/workorder-at-m3-uniformity.md`). Until this
// stage a statement declared its relation from the **outermost walk**, at
// `step_vm.cpp`'s `index == 0` guard - so a join's inner relation, a
// subquery's relation and a write's own relation were resolved and executed
// against with no `IS` held, and a DDL's `X` was granted over them. The
// declaration is now made by the compiler at the bind, between the name and
// the schema, which is what closes the window `ar0-5-amendment-uniformity.md`
// §8 names: a DDL cannot commit and release between this statement reading a
// layout and holding a position over it.
//
// `read_borrows()` counts **granted** asks, one per relation per statement,
// which is what these cells read. The defence is one-directional and
// `read_borrow.hpp` says why: a refused ask is read on, and
// `AReadIsNotRefusedByADropThatHoldsTheRelation` below pins that.
//
// **R8.3's mutant M1** - the compiler's bind declaring nothing
// (`step_compiler.cpp`, AT-R1's `declare->Position`) - is killed by the
// join and subquery cells, each counting one borrow short. The three write
// cells do not see it: a write declares at resolve through its own
// `ReadBorrow`, and the three write cells pin `InsertParsed`'s,
// `UpdateInner`'s and `DeleteInner`'s (measured, AY-S1; the DELETE cell is
// AY-S1's, the site having had none).

TEST_F(LockDeadlockTest, AJoinDeclaresItsInnerRelationAtTheBind) {
    // Two relations bound, two declarations. Before AT-S1 this was one:
    // `step_vm.cpp:1960`'s guard reports only for `index == 0`, so the
    // inner side of a walked join declared nothing at all and its schema
    // was read under no borrow.
    ASSERT_EQ(Local("CREATE TABLE jo (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("CREATE TABLE ji (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO jo VALUES (1, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO ji VALUES (1, 1)").rfind("INSERTED", 0), 0u);

    const std::uint64_t before = dispatcher_->read_borrows();
    const std::string rows = Local("SELECT jo.v, ji.v FROM jo JOIN ji ON jo.id = ji.id");
    ASSERT_NE(rows.rfind("ERR", 0), 0u) << rows;
    EXPECT_EQ(dispatcher_->read_borrows(), before + 2)
        << "the inner relation of a join was bound without being declared";
    EXPECT_EQ(locks_->EntryCount(), 0u) << "a statement-scoped borrow outlived its statement";
}

TEST_F(LockDeadlockTest, ASubqueryRelationIsDeclaredAtItsOwnBind) {
    // The nested block binds through the same loop
    // (`step_compiler.cpp`'s "the one site that covers FROM, every JOIN and
    // every subquery block"), so the declaration reaches it without the
    // compiler knowing anything about locks.
    ASSERT_EQ(Local("CREATE TABLE so (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("CREATE TABLE si (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO so VALUES (1, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO si VALUES (1, 1)").rfind("INSERTED", 0), 0u);

    const std::uint64_t before = dispatcher_->read_borrows();
    const std::string rows = Local("SELECT v FROM so WHERE id IN (SELECT id FROM si)");
    ASSERT_NE(rows.rfind("ERR", 0), 0u) << rows;
    EXPECT_EQ(dispatcher_->read_borrows(), before + 2)
        << "a subquery's relation was resolved under no borrow";
    EXPECT_EQ(locks_->EntryCount(), 0u);
}

TEST_F(LockDeadlockTest, AnInsertDeclaresItsRelationAtResolveToo) {
    // The write verb the first draft of AT-S1 missed: an
    // INSERT resolves its relation's layout exactly as the others do, and
    // its own borrows are taken rows later.
    ASSERT_EQ(Local("CREATE TABLE ins (id int64, v int64)").rfind("CREATED", 0), 0u);

    const std::uint64_t before = dispatcher_->read_borrows();
    ASSERT_EQ(Local("INSERT INTO ins VALUES (1, 1)").rfind("INSERTED", 0), 0u);
    EXPECT_EQ(dispatcher_->read_borrows(), before + 1)
        << "an INSERT resolved its relation under no borrow";
    EXPECT_EQ(locks_->EntryCount(), 0u) << "the insert's read borrow outlived its statement";
}

TEST_F(LockDeadlockTest, AWriteDeclaresItsRelationAtResolveToo) {
    // A write resolves a schema exactly as a read does and carries the same
    // window - its own `IX` is taken well after the compile. The
    // declaration is an `IS`, which is compatible with that `IX`, so it
    // costs the writer nothing and gives the compile a `SELECT`'s
    // protection.
    ASSERT_EQ(Local("CREATE TABLE wr (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO wr VALUES (1, 1)").rfind("INSERTED", 0), 0u);

    const std::uint64_t before = dispatcher_->read_borrows();
    ASSERT_EQ(Local("UPDATE wr SET v = 2 WHERE id = 1").rfind("UPDATED", 0), 0u);
    EXPECT_EQ(dispatcher_->read_borrows(), before + 1)
        << "an UPDATE resolved its relation under no borrow";
    EXPECT_EQ(locks_->EntryCount(), 0u) << "the write's read borrow outlived its statement";
}

TEST_F(LockDeadlockTest, ADeleteDeclaresItsRelationAtResolveToo) {
    // DELETE's own resolve site, which none of R8.3's cells pinned.
    ASSERT_EQ(Local("CREATE TABLE de (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO de VALUES (1, 1)").rfind("INSERTED", 0), 0u);

    const std::uint64_t before = dispatcher_->read_borrows();
    ASSERT_EQ(Local("DELETE FROM de WHERE id = 1").rfind("DELETED", 0), 0u);
    EXPECT_EQ(dispatcher_->read_borrows(), before + 1)
        << "a DELETE resolved its relation under no borrow";
    EXPECT_EQ(locks_->EntryCount(), 0u) << "the delete's read borrow outlived its statement";
}

TEST_F(LockDeadlockTest, ADropThatWouldCloseACycleIsTheVictimRatherThanWaiting) {
    // The wait's other half, and the reason it draws an edge at all. A
    // relation `X` is refused by the `IX` of any *writer* of that relation,
    // not only by a reader's `IS` - so a transaction that holds rows and
    // then drops a relation somebody else is writing can be one half of a
    // cycle, and the holder it waits for can be waiting on a row it holds.
    // An autocommit drop holds nothing and registers nothing; this one
    // holds a row, so the registration is what refuses it (AO-R7: the
    // waiter that closes the cycle is the victim).
    ASSERT_EQ(Local("CREATE TABLE rb (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO rb VALUES (1, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);

    // A holds a row of `t`; B holds a row of `rb` - and so `IX` on `rb`.
    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &b).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 1 WHERE id = 5", &a).response, "UPDATED 1");
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE rb SET v = 2 WHERE id = 1", &b).response, "UPDATED 1");

    // B waits for A's row: one edge, `B -> A`.
    Started wb = Start("UPDATE t SET v = 3 WHERE id = 5", b);
    Pump();
    ASSERT_FALSE(*wb.done) << "B did not wait for A: " << wb.out->response;

    // A now drops the relation B holds `IX` on. The edge `A -> B` closes
    // the cycle, so A's statement is refused naming deadlock instead of
    // parking on a slot nothing would flip until the fault net.
    //
    // On the served path, because that is where a wait exists to be
    // refused: the synchronous `Dispatch` registers no wake and takes no
    // edge, and its honest answer is the plain conflict.
    Started drop = Start("DROP TABLE rb", a);
    Pump();
    ASSERT_TRUE(*drop.done) << "the victim parked instead of being refused";
    EXPECT_NE(drop.out->response.find("deadlock"), std::string::npos) << drop.out->response;
    EXPECT_EQ(locks_->WaitEdgeCount(), 1u) << "the victim's edge outlived its refusal";

    // The survivor proceeds on A's rollback, which is what the victim's
    // message tells the client to do.
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &a).response.rfind("ROLL", 0), 0u);
    Pump();
    EXPECT_TRUE(*wb.done) << "B never resumed after A rolled back";
}

TEST_F(LockDeadlockTest, AReadIsNotRefusedByADropThatHoldsTheRelation) {
    // **A read borrow never refuses a read and never makes one wait.** The
    // reader needs no protection to be correct - DT1 leaves its pages
    // allocated and its oid never reissued - so a refused ask leaves it
    // holding nothing and reading on. It is also what keeps a reader out of
    // the wait-for graph, which is why a DDL waiting for one cannot be in a
    // cycle through it.
    ASSERT_EQ(Local("CREATE TABLE rb (id int64, v int64)").rfind("CREATED", 0), 0u);
    const std::uint64_t oid = OidIn(Local("INSERT INTO rb VALUES (1, 1)"));
    ASSERT_NE(oid, 0u);

    txn::LockHoldings ddl;
    ASSERT_TRUE(locks_
                    ->Acquire(4242, txn::LockKey::Relation(static_cast<catalog::Oid>(oid)),
                              txn::LockMode::kExclusive, ddl)
                    .value()
                    .granted);

    const std::uint64_t before = dispatcher_->read_borrows();
    const std::string rows = Local("SELECT * FROM rb");
    EXPECT_NE(rows.rfind("ERR", 0), 0u) << "a read waited for or was refused by a DDL: " << rows;
    EXPECT_EQ(dispatcher_->read_borrows(), before)
        << "the ask was refused, so no position was declared - and the read ran anyway";
    locks_->Release(4242, ddl);
}

// ---- AO-S6c-b: the lock is what the statement waits for ----------------

TEST_F(LockDeadlockTest, AWriteBlockedByARangeFenceWaitsForItsHolder) {
    // **The wait the tuple header could not express.** A declares a window
    // over `(4, ...)` whose second conjunct matches nothing, so it holds
    // `Range(t, 5, ...)` and writes **no row** - which is the whole point:
    // row 5's header still names the committed inserter, so the MVCC check
    // passes it and first-updater-wins has nothing to say. Before AO-S6c-b
    // the blocker came from that header, so B had nobody to wait for and
    // the fence either refused it outright or, worse, was not consulted at
    // the wait at all.
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (6, 0)").rfind("INSERTED", 0), 0u);

    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    const DispatchOutcome fenced =
        dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id > 4 AND v = 99", &a);
    ASSERT_EQ(fenced.response, "UPDATED 0") << fenced.response;

    // B writes a row inside A's window. Nothing about the row itself
    // refuses it; the declared range does.
    Started wb = Start("UPDATE t SET v = 3 WHERE id = 5", b);
    Pump();
    ASSERT_FALSE(*wb.done) << "B did not wait on the fence: " << wb.out->response;

    // And the wait ends where every wait in this family ends - at the
    // holder's decide, with the statement run again.
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*wb.done) << "B never resumed after the fence was released";
    EXPECT_EQ(wb.out->response, "UPDATED 1") << wb.out->response;
}

TEST_F(LockDeadlockTest, AnInsertIntoAFencedWindowWaitsForItsHolder) {
    // AO-S6c-c's first piece, and the gap AO-S6c-b's review named: until
    // this cell an INSERT wrote straight through a range fence, because the
    // insert sites took their borrow and never read the answer. There is no
    // header here to fall back on - the row does not exist yet - so the
    // borrow is the *only* thing that can refuse it, which is why a fence
    // guarded its holder against writers of rows that exist and not against
    // rows that appear.
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);

    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    const DispatchOutcome fenced =
        dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id > 4 AND v = 99", &a);
    ASSERT_EQ(fenced.response, "UPDATED 0") << fenced.response;

    // Above the relation's high-water mark, because `t` is heap-clustered
    // and its chain grows only at the tail - a key below the mark is a
    // btree-only shape and would be refused before the borrow is reached.
    Started wb = Start("INSERT INTO t VALUES (100, 1)", b);
    Pump();
    ASSERT_FALSE(*wb.done) << "the insert went through A's fence: " << wb.out->response;

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*wb.done) << "the insert never resumed after the fence was released";
    EXPECT_EQ(wb.out->response.rfind("INSERTED", 0), 0u) << wb.out->response;
}

TEST_F(LockDeadlockTest, AnIllegalKeyIsRefusedWithoutWaitingOnAFence) {
    // The regression AO-S6c-c's own first draft introduced and its review
    // caught. Moving the borrow ahead of `AdmitExplicitRowId` was right for
    // the re-run, but that call is the **only** validation a caller-named
    // key ever gets - `TableAccess` carries no `next_id` on purpose - so it
    // also put the borrow ahead of every reason to refuse the key at all.
    // An insert that could never succeed then waited out a fence, burned
    // ledger entries, and inside an explicit transaction could be killed as
    // a deadlock victim for a statement with no future.
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (6, 0)").rfind("INSERTED", 0), 0u);

    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id > 4 AND v = 99", &a).response,
              "UPDATED 0");

    // Key 5 sits inside A's window and below the relation's high-water
    // mark, so on a heap relation it can never be written by anyone. It
    // must be told so at once.
    Started wb = Start("INSERT INTO t VALUES (5, 9)", b);
    Pump();
    ASSERT_TRUE(*wb.done) << "an illegal key waited on a fence it could never write past";
    EXPECT_NE(wb.out->response.find("high-water mark"), std::string::npos) << wb.out->response;
}

TEST_F(LockDeadlockTest, ASortedFillWaitsOnAFenceOverTheBlockItCarves) {
    // The other insert path, which routes past `InsertOneRow` entirely and
    // whose park had no cell of its own until here - the review's C5. Every
    // row omits its key, so the fill carves one contiguous block and
    // borrows it as a range.
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);

    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id > 4 AND v = 99", &a).response,
              "UPDATED 0");

    // The block is carved from the mark, which sits inside A's window, and
    // the window runs to the end of the id space - so a re-run that carves
    // a fresh block is still covered.
    Started wb = Start("INSERT INTO t VALUES (1), (2)", b);
    Pump();
    ASSERT_FALSE(*wb.done) << "the fill wrote through A's fence: " << wb.out->response;

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*wb.done) << "the fill never resumed after the fence was released";
    EXPECT_EQ(wb.out->response.rfind("INSERTED", 0), 0u) << wb.out->response;
}

// ---- BB-S2's guards: a spilled row still waits on a fence ----------------
//
// `instructions/v3.0.0/workorder-bb-issue-under-the-leaf.md` §1.4. A recorded
// wait survives only while the statement's trail is unchanged, and a spill is
// a trail entry - so a row whose borrow came after its encode would turn this
// wait into a refusal. BB moves an omitted pk's issue, borrow and encode under
// its leaf's hold (BB-R2) and a named key's admission after its descent
// (BB-R3); both orders keep the borrow ahead of the encode, and these cells
// are what says so. `varchar(16)` and a longer value make the encode spill.

void LockDeadlockTest::SpilledInsertWaitsOnAFence(const std::string& insert) {
    ASSERT_EQ(Local("CREATE TABLE sp (id int64, s varchar(16)) BTREE").rfind("CREATED", 0), 0u);
    // Load-bearing: it moves the mark to 6, inside the fence's window below,
    // so an issued id lands there too.
    ASSERT_EQ(Local("INSERT INTO sp VALUES (5, 'five')").rfind("INSERTED", 0), 0u);

    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    // Declares `Range(sp, 5, end)` and writes no row.
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE sp SET s = 'x' WHERE id > 4 AND s = 'zz'", &a).response,
              "UPDATED 0");

    Started wb = Start(insert, b);
    Pump();
    ASSERT_FALSE(*wb.done) << "the spilled insert did not wait on A's fence: " << wb.out->response;

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*wb.done) << "the spilled insert never resumed after the fence was released";
    EXPECT_EQ(wb.out->response.rfind("INSERTED", 0), 0u) << wb.out->response;
}

TEST_F(LockDeadlockTest, AnIssuedRowWithASpilledValueWaitsOnAFence) {
    SpilledInsertWaitsOnAFence("INSERT INTO sp VALUES ('a value well past sixteen bytes long')");
}

TEST_F(LockDeadlockTest, ANamedRowWithASpilledValueWaitsOnAFence) {
    SpilledInsertWaitsOnAFence("INSERT INTO sp VALUES (100, 'a value well past sixteen bytes long')");
}

TEST_F(LockDeadlockTest, ATwoCycleAbortsTheWaiterThatClosedItAndTheOtherProceeds) {
    // AO-5's S4a cell. Without a detector this is the deadlock AO-S3's
    // guard exists to prevent; with one, the guard lifts and the cycle is
    // resolved at the instant it closes.
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (8, 1)").rfind("INSERTED", 0), 0u);

    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &b).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 7", &a)
                  .response.rfind("UPDATED", 0),
              0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 8", &b)
                  .response.rfind("UPDATED", 0),
              0u);

    // A wants B's row. It holds rows, so under AO-S3 alone it would have
    // been refused; with a detector it parks, and one edge is recorded.
    Started wa = Start("UPDATE t SET v = 3 WHERE id = 8", a);
    Pump();
    ASSERT_FALSE(*wa.done) << "A did not wait, so the guard did not lift: " << wa.out->response;
    EXPECT_EQ(locks_->WaitEdgeCount(), 1u);

    // B now wants A's row, which closes the cycle. B is the waiter that
    // closed it, so B is the victim - deterministically, not by a race.
    Started wb = Start("UPDATE t SET v = 3 WHERE id = 7", b);
    Pump();
    ASSERT_TRUE(*wb.done) << "the cycle was not detected; only the fault net would end this";
    const Status victim = StatusFromErrorReply(wb.out->response);
    EXPECT_EQ(victim.code(), StatusCode::kTxnConflict) << wb.out->response;
    EXPECT_TRUE(victim.retryable()) << "the survivor will have released by the time it retries";
    EXPECT_NE(wb.out->response.find("deadlock"), std::string::npos)
        << "an operator meeting this needs to know to look for a lock-order bug rather than "
           "for contention: " << wb.out->response;
    // **And on the carried `Status`, which is the one a KWP client reads**
    // (`KwpSession::OnStatementComplete` prefers `outcome.status` over the
    // rendered line). A deadlock reported only in the text arm is reported
    // to nobody on the default port.
    EXPECT_NE(wb.out->status.message().find("deadlock"), std::string::npos)
        << "the carried status still holds the pre-wait conflict: " << wb.out->status.message();

    // A is untouched - a detector aborts the waiter, never the holder - and
    // proceeds the moment B's transaction lets go.
    EXPECT_FALSE(*wa.done);
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &b).response.rfind("ROLLBACK", 0), 0u);
    Pump();
    EXPECT_TRUE(*wa.done) << "the survivor never proceeded, so the cycle was broken at both ends";
    EXPECT_EQ(wa.out->response.rfind("UPDATED", 0), 0u) << wa.out->response;

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    EXPECT_EQ(locks_->WaitEdgeCount(), 0u) << "every wait ended, so no edge is left behind";
}

TEST_F(LockDeadlockTest, AThreeCycleIsCaughtByTheTransitiveWalk) {
    // The chain the walk has to follow: A waits for B, B waits for C, and
    // C's wait for A is what closes it. A cycle test that only looked one
    // edge deep would miss this and leave three transactions for the net.
    for (int id = 1; id <= 3; ++id) {
        ASSERT_EQ(Local("INSERT INTO t VALUES (" + std::to_string(id) + ", 1)")
                      .rfind("INSERTED", 0),
                  0u);
    }
    Session a;
    Session b;
    Session c;
    Session* sessions[] = {&a, &b, &c};
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(dispatcher_->Dispatch("BEGIN", sessions[i]).response.rfind("BEGIN", 0), 0u);
        ASSERT_EQ(dispatcher_
                      ->Dispatch("UPDATE t SET v = 2 WHERE id = " + std::to_string(i + 1),
                                 sessions[i])
                      .response.rfind("UPDATED", 0),
                  0u);
    }

    Started wa = Start("UPDATE t SET v = 3 WHERE id = 2", a);  // A -> B
    Pump();
    ASSERT_FALSE(*wa.done);
    Started wb = Start("UPDATE t SET v = 3 WHERE id = 3", b);  // B -> C
    Pump();
    ASSERT_FALSE(*wb.done);
    EXPECT_EQ(locks_->WaitEdgeCount(), 2u);

    Started wc = Start("UPDATE t SET v = 3 WHERE id = 1", c);  // C -> A closes it
    Pump();
    ASSERT_TRUE(*wc.done) << "the three-cycle was not detected";
    EXPECT_NE(wc.out->response.find("deadlock"), std::string::npos) << wc.out->response;
    EXPECT_FALSE(*wa.done) << "A and B are untouched; only the closer is aborted";
    EXPECT_FALSE(*wb.done);
}

TEST_F(LockDeadlockTest, AChainThatDoesNotCloseIsNotADeadlock) {
    // The false positive a naive detector would produce: A waits for B and
    // B waits for C, which is three transactions and two edges and no
    // cycle. Both waits must stand.
    for (int id = 1; id <= 3; ++id) {
        ASSERT_EQ(Local("INSERT INTO t VALUES (" + std::to_string(id) + ", 1)")
                      .rfind("INSERTED", 0),
                  0u);
    }
    Session a;
    Session b;
    Session c;
    Session* sessions[] = {&a, &b, &c};
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(dispatcher_->Dispatch("BEGIN", sessions[i]).response.rfind("BEGIN", 0), 0u);
        ASSERT_EQ(dispatcher_
                      ->Dispatch("UPDATE t SET v = 2 WHERE id = " + std::to_string(i + 1),
                                 sessions[i])
                      .response.rfind("UPDATED", 0),
                  0u);
    }

    Started wa = Start("UPDATE t SET v = 3 WHERE id = 2", a);  // A -> B
    Pump();
    Started wb = Start("UPDATE t SET v = 3 WHERE id = 3", b);  // B -> C, no cycle
    Pump();
    EXPECT_FALSE(*wa.done) << wa.out->response;
    EXPECT_FALSE(*wb.done) << wb.out->response;
    EXPECT_EQ(locks_->WaitEdgeCount(), 2u);

    // C decides, and the chain unwinds from the far end.
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", sessions[2]).response.rfind("COMMIT", 0), 0u);
    Pump();
    EXPECT_TRUE(*wb.done) << wb.out->response;
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", sessions[1]).response.rfind("COMMIT", 0), 0u);
    Pump();
    EXPECT_TRUE(*wa.done) << wa.out->response;
    EXPECT_EQ(locks_->WaitEdgeCount(), 0u);
}


TEST_F(LockDeadlockTest, AStatementThatHasWrittenRowsNowWaitsInsteadOfBeingRefused) {
    // **The line AO-S3b moved, and this cell is where it now sits.**
    //
    // It used to read the other way: a statement that had already written
    // rows was refused even with a detector present, because a re-run would
    // have written those rows a second time and there are no savepoints to
    // undo them with. AO-S3b does not re-run it - the walk stops at the held
    // row, the scope and the position stay with the session, and the resume
    // carries on from there - so restartability stops being the thing that
    // forbids the wait.
    //
    // What did *not* move is the guard below it: the wait is offered only
    // where a detector exists, because a waiter holding rows can join a
    // cycle (`WithoutATableTheNarrowGuardIsWhatKeepsTheStageSafe`).
    ASSERT_EQ(Local("INSERT INTO t VALUES (1, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (2, 1)").rfind("INSERTED", 0), 0u);

    // A holder takes row 2 and stays open.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 2", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    // A multi-row UPDATE that writes row 1 and then meets row 2.
    //
    // **The predicate names no pk window, and since AO-S6c-c that is what
    // keeps this cell about a mid-walk park.** `WHERE id <= 2` would now
    // declare `Range(t, 0, 3)` (item 14), which overlaps the holder's tuple
    // and is refused *before* the walk starts - so the statement would wait
    // having written nothing, hold no rows, and correctly register no edge
    // at all. That is the better behaviour and it is why the declaration
    // exists; it is simply not the shape this cell measures. Both rows
    // are matched by `v > 0` whichever value the page carries - `apply`
    // evaluates the WHERE against the page's current bytes, so row 2 reads
    // as the holder's uncommitted `9` rather than as the `1` this writer's
    // view would resolve.
    Session writer;
    Started w = Start("UPDATE t SET v = 3 WHERE v > 0", writer);
    Pump();
    ASSERT_FALSE(*w.done) << "the statement was refused rather than parked: " << w.out->response;

    // **And it is in the graph.** An autocommit statement parked mid-walk
    // holds rows, so it can be the other half of a cycle - which is why its
    // waiter identity comes from the parked scope rather than from
    // `session.transaction()`, which is null here. An edge that went
    // unregistered would be a deadlock only the fault net could end.
    EXPECT_EQ(locks_->WaitEdgeCount(), 1u)
        << "a parked autocommit writer holding rows registered no edge, so a cycle through it "
           "would be invisible to the detector";

    // The holder rolls back, which restores the row's prior writer id along
    // with its bytes - so the parked statement's own unchanged view admits
    // the write it stopped on, and the statement finishes.
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*w.done) << "the wait never ended after the holder decided";
    EXPECT_EQ(w.out->response.rfind("UPDATED 2", 0), 0u)
        << "both rows, counted across the park: " << w.out->response;
    // And the edge is gone, however the wait ended.
    EXPECT_EQ(locks_->WaitEdgeCount(), 0u);
}

TEST_F(LockDeadlockTest, WithoutATableTheNarrowGuardIsWhatKeepsTheStageSafe) {
    // The two states stated side by side. Take the table away and the same
    // transaction that waited above is refused instead, because nothing
    // would catch the cycle it could join - which is AO-S3's rule, and why
    // it is a guard rather than a limitation.
    dispatcher_->set_locks(nullptr);
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (8, 1)").rfind("INSERTED", 0), 0u);

    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &b).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 7", &a)
                  .response.rfind("UPDATED", 0),
              0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 8", &b)
                  .response.rfind("UPDATED", 0),
              0u);

    Started wa = Start("UPDATE t SET v = 3 WHERE id = 8", a);
    Pump();
    ASSERT_TRUE(*wa.done) << "a transaction holding rows waited with no detector present";
    EXPECT_EQ(StatusFromErrorReply(wa.out->response).code(), StatusCode::kTxnConflict)
        << wa.out->response;
    EXPECT_EQ(wa.out->response.find("deadlock"), std::string::npos)
        << "and it is an ordinary conflict, not a deadlock report: " << wa.out->response;
}

// ---- AO-S6d item 17: the repeatable-read wait, made exact ---------------
//
// A repeatable-read writer whose blocker is the row's own writer is still
// refused rather than offered a wait (`NoteBlockingWriter`'s guard; its
// cell, `ARepeatableReadWriterIsRefusedRatherThanOfferedANarrowerWait`,
// above, ported by AY-Q9), because a commit
// makes the row invisible to the waiter's view for the rest of its
// transaction, so the wait could only ever pay off on the abort arm. These
// two are the case that does not - a holder of a *unit* over the key that
// has written no version of it. Its commit changes what this view admits
// only for rows it wrote, and the row in hand was written by somebody the
// view has already judged.

TEST_F(LockDeadlockTest, ARepeatableReadWriterWaitsOnAFenceHolderThatWroteNoRow) {
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (6, 0)").rfind("INSERTED", 0), 0u);

    Session rr;
    ASSERT_EQ(dispatcher_->Dispatch("SET ISOLATION LEVEL REPEATABLE READ", &rr)
                  .response.rfind("ERR", 0),
              std::string::npos)
        << "the level must be settable for this cell to mean anything";
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &rr).response.rfind("BEGIN", 0), 0u);

    // A holds `[2, 100)` and writes nothing inside it, so row 5's header
    // still names the committed inserter - a version this view can see.
    Session a;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id > 1 AND id < 100 AND v = 99",
                                    &a)
                  .response,
              "UPDATED 0");

    Started w = Start("UPDATE t SET v = 3 WHERE id = 5", rr);
    Pump();
    ASSERT_FALSE(*w.done) << "the repeatable-read writer was refused instead of waiting on a "
                             "holder that wrote no version of its row: "
                          << w.out->response;

    // **The mutation**: restore the whole-level exclusion in
    // `NoteBlockingWriter` and this is refused at once, with the assertion
    // above failing rather than the one below.
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*w.done) << "the wait never ended";
    EXPECT_EQ(w.out->response, "UPDATED 1") << w.out->response;
    // Read back through the waiter's own commit: it is an explicit
    // transaction, so until it decides its write is nobody else's to see.
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &rr).response.rfind("COMMIT", 0), 0u);
    EXPECT_NE(Rows().find(",3"), std::string::npos) << Rows();
}

TEST_F(LockDeadlockTest, ARepeatableReadWaiterIsStillRefusedIfTheFenceHolderWritesTheRow) {
    // The half the operator ruled on (2026-09-09, AO-0 item 17): the wait
    // is offered on what the holder has done *so far*, and a holder is free
    // to write the row afterwards. The re-run then meets a header naming a
    // transaction that committed after this view was minted, and is refused
    // first-updater-wins - the correct repeatable-read answer, reached
    // after a wait rather than instead of one. What this pins is that the
    // wait did not widen what the level admits.
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (6, 0)").rfind("INSERTED", 0), 0u);

    Session rr;
    ASSERT_EQ(dispatcher_->Dispatch("SET ISOLATION LEVEL REPEATABLE READ", &rr)
                  .response.rfind("ERR", 0),
              std::string::npos);
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &rr).response.rfind("BEGIN", 0), 0u);

    Session a;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id > 1 AND id < 100 AND v = 99",
                                    &a)
                  .response,
              "UPDATED 0");

    Started w = Start("UPDATE t SET v = 3 WHERE id = 5", rr);
    Pump();
    ASSERT_FALSE(*w.done) << w.out->response;

    // Now A writes the row the waiter wants - its own fence does not block
    // it - and commits.
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 5", &a).response,
              "UPDATED 1");
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*w.done) << "the wait never ended";
    EXPECT_EQ(StatusFromErrorReply(w.out->response).code(), StatusCode::kTxnConflict)
        << w.out->response;
    EXPECT_NE(Rows().find(",9"), std::string::npos)
        << "the holder's value is what stands: " << Rows();
}

TEST_F(LockDeadlockTest, ARepeatableReadInsertThatWaitsOnAFenceStillCannotDuplicateAKey) {
    // The boundary the lift has to be checked against, because it is the
    // one shape where "the view cannot see the holder's commit" would be a
    // wrong *answer* rather than a refusal: an INSERT of a caller-named key
    // that waits on a fence whose holder then inserts that very key.
    //
    // It is safe, and not by luck. The uniqueness proof is **physical** -
    // `btree.cpp`'s descent scans the one leaf that may hold the key and
    // `heap_chain.cpp`'s scans the tail page, neither of them through a
    // read view - so a version this repeatable-read session cannot see is
    // still a key it cannot take. The refusal is `AlreadyExists`, which is
    // not retryable and says so.
    ASSERT_EQ(Local("CREATE TABLE tb (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO tb VALUES (10, 0)").rfind("INSERTED", 0), 0u);

    Session rr;
    ASSERT_EQ(dispatcher_->Dispatch("SET ISOLATION LEVEL REPEATABLE READ", &rr)
                  .response.rfind("ERR", 0),
              std::string::npos);
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &rr).response.rfind("BEGIN", 0), 0u);

    Session a;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("DELETE FROM tb WHERE id > 1 AND id < 100 AND v = 999", &a)
                  .response,
              "DELETED 0");

    Started w = Start("INSERT INTO tb VALUES (50, 1)", rr);
    Pump();
    ASSERT_FALSE(*w.done) << w.out->response;

    // The holder takes the key the waiter wants - its own fence does not
    // block it - and commits.
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO tb VALUES (50, 2)", &a)
                  .response.rfind("INSERTED", 0),
              0u);
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*w.done) << "the wait never ended";
    EXPECT_NE(w.out->response.find("duplicate primary key"), std::string::npos)
        << "a repeatable-read insert wrote a key its own view could not see: "
        << w.out->response;
}

// ---- AO-S6d item 15: a failed commit unwinds instead of leaking ---------
//
// `CommitLocal` has aborted on a failed commit since the DT9 review, and
// says at the site why merely reporting it is not enough. `EndWrite`'s
// autocommit arm did not, and the two are the same transaction state: a
// commit fails only *before* `PublishCommit`, so the transaction is still
// active and `Release` refuses to free an active one. What the leak cost
// stopped being memory at AO-S6c-a, which gave every writer a borrow: the
// leaked transaction's tenancies were never released either, so every
// later writer of a row it touched waited out the fault net and was told a
// defect report.

class FailedCommitTest : public LockDeadlockTest {
protected:
    // **Strict, because it is the class whose commit record is synced
    // inside `WalManager::Commit`.** Under `kGroup` the commit stages and
    // the sync happens at a later drain, where its failure is nobody's
    // statement; `FailNextSync` under `kStrict` fails the one call
    // `TransactionManager::Commit` can fail on, which is this arm.
    wal::DurabilityClass Durability() const override { return wal::DurabilityClass::kStrict; }
};

TEST_F(FailedCommitTest, AnAutocommitWriteWhoseCommitFailsReleasesEverythingItHeld) {
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);
    const std::size_t held_before = locks_->EntryCount();

    log_device_->FailNextSync(Status::IoError("injected"));
    const DispatchOutcome out = dispatcher_->Dispatch("UPDATE t SET v = 1 WHERE id = 5");
    ASSERT_EQ(out.response.rfind("ERR ", 0), 0u)
        << "the injected sync failure did not reach the statement: " << out.response;
    EXPECT_NE(out.response.find("injected"), std::string::npos) << out.response;

    // The three things the leak was: a transaction still in flight, its
    // borrows still held, and its rows still written.
    EXPECT_EQ(txns_->ActiveCount(), 0u)
        << "the transaction whose commit failed is still active";
    EXPECT_EQ(locks_->EntryCount(), held_before)
        << "the failed commit left its borrows in the table";
    EXPECT_NE(Rows().find(",0"), std::string::npos)
        << "the write the commit never made durable was not compensated: " << Rows();

    // **The mutation**: reinstate the release-only arm and this second
    // writer parks on a holder nothing will ever decide, reaching the
    // fault net and failing here rather than below.
    Session b;
    Started wb = Start("UPDATE t SET v = 3 WHERE id = 5", b);
    Pump();
    ASSERT_TRUE(*wb.done) << "a later writer of the row is waiting on a transaction that no "
                             "longer exists: " << wb.out->response;
    EXPECT_EQ(wb.out->response, "UPDATED 1") << wb.out->response;
}

TEST_F(FailedCommitTest, AFailedStatementInsideATransactionStillPoisonsAndStillHolds) {
    // The other arm of `EndWrite`, pinned so the fix above is seen not to
    // widen: inside an explicit transaction a failed statement does **not**
    // unwind. Failure atomicity is per transaction (txn.md §6), so the rows
    // already written stay, the borrows stay with them, and the client must
    // ROLLBACK.
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (6, 0)").rfind("INSERTED", 0), 0u);
    const std::size_t held_before = locks_->EntryCount();

    Session a;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 5", &a).response,
              "UPDATED 1");
    // A key below the relation's high-water mark: refused, on a heap
    // relation, after the write scope is open.
    const DispatchOutcome refused = dispatcher_->Dispatch("INSERT INTO t VALUES (5, 9)", &a);
    ASSERT_EQ(refused.response.rfind("ERR ", 0), 0u) << refused.response;

    EXPECT_GT(locks_->EntryCount(), held_before)
        << "the poisoned transaction gave its borrows back before ROLLBACK";
    EXPECT_EQ(dispatcher_->Dispatch("SELECT * FROM t", &a).response.rfind("ERR ", 0), 0u)
        << "the session was not poisoned";

    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &a).response.rfind("ROLLBACK", 0), 0u);
    EXPECT_EQ(locks_->EntryCount(), held_before);
    EXPECT_NE(Rows().find(",0"), std::string::npos) << Rows();
}

// ---- AO-S6e-c: the bounded false rejection becomes a wait ---------------
//
// `assertion.md` §6.2 listed four properties of the admission protocol and
// accepted a **bounded false rejection** as one of them: "a statement can
// be rejected due to a reservation of a transaction that later aborts".
// The owner of that reservation is knowable, so census row 11 turns the
// rejection into a wait for its decide. What the wait rides is the channel
// every other wait in this family uses, which is what gives it the wait-for
// graph - two transactions can each hold a reservation the other's
// admission needs, and that cycle is real.

TEST_F(LockDeadlockTest, AnAssertionsFalseRejectionWaitsAndIsAdmittedWhenTheReserverAborts) {
    ASSERT_EQ(Local("CREATE TABLE trades (id int64, account int64, qty int64) BTREE")
                  .rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Local("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK SUM(qty) <= 100")
                  .rfind("CREATED", 0),
              0u);

    // The reserver takes 60 of the 100 and holds it undecided.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO trades VALUES (7, 60)", &holder)
                  .response.rfind("INSERTED", 0),
              0u);

    // 50 more would be 110. The aggregate that refuses is 60 of somebody
    // else's reservation, so the refusal is one a decide can undo.
    //
    // **The mutation**: drop the `NoteBlockingWriter` at the admission
    // site and this is `ERR ASSERTION_VIOLATION` here instead - which is
    // what `AssertionEnforceTest`'s own cell still asserts on the
    // synchronous path, where nothing can park.
    Session rival;
    Started w = Start("INSERT INTO trades VALUES (7, 50)", rival);
    Pump();
    ASSERT_FALSE(*w.done) << "the false rejection was answered instead of waited: "
                          << w.out->response;

    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*w.done) << "the wait never ended";
    EXPECT_EQ(w.out->response.rfind("INSERTED", 0), 0u) << w.out->response;
}

TEST_F(LockDeadlockTest, AnAssertionsRejectionIsFinalWhenTheReserverCommits) {
    // The other decide, and the reason the wait is worth having: the same
    // statement gets two different *correct* answers depending on how the
    // reservation ends, and only one of them is the violation. A wait that
    // ended in the violation either way would be a stall bought for
    // nothing.
    ASSERT_EQ(Local("CREATE TABLE trades (id int64, account int64, qty int64) BTREE")
                  .rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Local("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK SUM(qty) <= 100")
                  .rfind("CREATED", 0),
              0u);

    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO trades VALUES (7, 60)", &holder)
                  .response.rfind("INSERTED", 0),
              0u);

    Session rival;
    Started w = Start("INSERT INTO trades VALUES (7, 50)", rival);
    Pump();
    ASSERT_FALSE(*w.done) << w.out->response;

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &holder).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*w.done) << "the wait never ended";
    EXPECT_NE(w.out->response.find("ASSERTION_VIOLATION"), std::string::npos)
        << "the reservation became the group's real weight, so the refusal is now true: "
        << w.out->response;
}

TEST_F(LockDeadlockTest, AnAssertionRejectionWithNothingReservedIsRefusedAtOnce) {
    // The third answer, and the one that says the wait is not unconditional:
    // where the aggregate that refuses is **settled**, no decide can change
    // it and the registry names nobody to wait for, so the violation is
    // delivered now.
    ASSERT_EQ(Local("CREATE TABLE trades (id int64, account int64, qty int64) BTREE")
                  .rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Local("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK SUM(qty) <= 100")
                  .rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Local("INSERT INTO trades VALUES (7, 60)").rfind("INSERTED", 0), 0u);

    Session rival;
    Started w = Start("INSERT INTO trades VALUES (7, 50)", rival);
    Pump();
    ASSERT_TRUE(*w.done) << "a refusal nothing can undo was waited on anyway";
    EXPECT_NE(w.out->response.find("ASSERTION_VIOLATION"), std::string::npos) << w.out->response;
}

TEST_F(LockDeadlockTest, ACreateAssertionLeavesNoTransactionOfItsOwnBehind) {
    // **The AT-S5e review's C5.** The build holds the relation under a
    // transaction of its own, and `Abort` leaves a transaction tracked by
    // design - so without `Release` every `CREATE ASSERTION`, and every
    // re-run of one after a wake, left an inactive entry in the manager's
    // list for the life of the core, walked on every begin and every mint.
    //
    // **Mutation**, measured: `BuildLock` without its `Release`, killed 1
    // in 1.
    ASSERT_EQ(Local("CREATE TABLE leftover (id int64, account int64, qty int64) BTREE")
                  .rfind("CREATED", 0),
              0u);
    const std::size_t before = txns_->tracked_transactions();
    ASSERT_EQ(Local("CREATE ASSERTION cap ON leftover GROUP BY (account) CHECK SUM(qty) <= 100")
                  .rfind("CREATED", 0),
              0u);
    EXPECT_EQ(txns_->tracked_transactions(), before)
        << "the build's own transaction is still tracked after the statement";
}

TEST_F(LockDeadlockTest, ACreateAssertionOverItsOwnTransactionsWriteIsRefusedAtOnce) {
    // **AT-0 item 13's one case with no one to wait for.** The build takes
    // the relation `X` under a transaction of its own (AT-S5e), and the
    // session's own open transaction holds the relation's `IX` from its
    // INSERT - so the ask is refused by a holder whose decide only this
    // session can make. Parking would wait out the fault net; the build's
    // in-flight refusal is answered at once instead, and it poisons
    // nothing, the statement being non-transactional DDL.
    //
    // **Mutation**, measured: the own-transaction test removed, killed 1 in
    // 1.
    ASSERT_EQ(Local("CREATE TABLE own (id int64, account int64, qty int64) BTREE")
                  .rfind("CREATED", 0),
              0u);
    Session mine;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &mine).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO own VALUES (7, 60)", &mine)
                  .response.rfind("INSERTED", 0),
              0u);

    Started w =
        Start("CREATE ASSERTION cap ON own GROUP BY (account) CHECK SUM(qty) <= 100", mine);
    Pump();
    ASSERT_TRUE(*w.done) << "the build parked on its own session's transaction";
    EXPECT_NE(w.out->response.find("TXN_CONFLICT"), std::string::npos) << w.out->response;
    EXPECT_NE(w.out->response.find("this session's own transaction"), std::string::npos)
        << w.out->response;
    EXPECT_FALSE(mine.failed()) << "a refused non-transactional DDL poisoned the transaction";
    EXPECT_EQ(dispatcher_->Dispatch("COMMIT", &mine).response.rfind("COMMIT", 0), 0u);

    // Settled, the same declaration builds.
    EXPECT_EQ(Local("CREATE ASSERTION cap ON own GROUP BY (account) CHECK SUM(qty) <= 100")
                  .rfind("CREATED", 0),
              0u);
}

TEST_F(LockDeadlockTest, AnUpdateThatHasAlreadyReservedIsRefusedRatherThanWaited) {
    // **The review's B1, and the cell that would have caught it.**
    // `AdmitAndReserveUpdate` is per *assertion* and not atomic across
    // them: when the second refuses, the first is already applied to its
    // cabin and appended to its chain. A wait re-runs the whole statement,
    // and the first assertion is reserved a second time - permanently,
    // because a reservation never enters the transaction's trail, so
    // `EndWrite`'s re-runnability test cannot see it and both entries are
    // `kAssertReserve` records a rebuild reproduces.
    //
    // So a call that has reserved hands back no reserver, and the statement
    // gets the violation it always gave. **The mutation**: drop
    // `reserved_any`, and the `UPDATE` below waits instead of answering -
    // and after the holder rolls back, `wide`'s aggregate is 30 too high
    // for the life of the relation.
    ASSERT_EQ(Local("CREATE TABLE trades (id int64, account int64, qty int64) BTREE")
                  .rfind("CREATED", 0),
              0u);
    // `wide` is asked first (creation order) and admits the +30; `tight`
    // is asked second and refuses it. The two bounds are 10 apart so that
    // `wide`'s aggregate is observable *below* `tight`'s ceiling - with a
    // far-apart pair, `tight` shadows `wide` and a double count in `wide`
    // could not be seen at all.
    ASSERT_EQ(Local("CREATE ASSERTION wide ON trades GROUP BY (account) CHECK SUM(qty) <= 100")
                  .rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Local("CREATE ASSERTION tight ON trades GROUP BY (account) CHECK SUM(qty) <= 90")
                  .rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Local("INSERT INTO trades VALUES (7, 10)").rfind("INSERTED", 0), 0u);

    // A reservation of 60 that has not decided: both groups read 70.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO trades VALUES (7, 60)", &holder)
                  .response.rfind("INSERTED", 0),
              0u);

    // 10 -> 40 is +30: `wide` admits it and **reserves**, `tight` refuses.
    Session rival;
    Started w = Start("UPDATE trades SET qty = 40 WHERE id = 1", rival);
    Pump();
    ASSERT_TRUE(*w.done) << "the update waited after it had already reserved: " << w.out->response;
    EXPECT_NE(w.out->response.find("ASSERTION_VIOLATION"), std::string::npos) << w.out->response;

    // And the refusal left `wide` where it was. The holder rolls back, so
    // account 7's only weight is the committed 10 - the update was refused,
    // so row 1 is still qty 10. 65 more is 75: inside `tight`'s 90 and
    // inside `wide`'s 100. Had the refused attempt's `wide` reservation
    // been counted a second time, `wide` would read 40 and refuse this.
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    const std::string probe = Local("INSERT INTO trades VALUES (7, 65)");
    EXPECT_EQ(probe.rfind("INSERTED", 0), 0u)
        << "`wide` counted the refused attempt's reservation a second time: " << probe;
}

TEST_F(LockDeadlockTest, TwoTransactionsWaitingOnEachOthersReservationAreADeadlockAndOneIsTheVictim) {
    // **The claim the whole sub-stage rests on**, and it had no cell:
    // riding the family's channel is what gives an assertion wait the
    // wait-for graph, and the reason that matters is that two transactions
    // can each hold a reservation the other's admission needs. That cycle
    // is real, and AO-S4a's detector is what ends it.
    ASSERT_EQ(Local("CREATE TABLE trades (id int64, account int64, qty int64) BTREE")
                  .rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Local("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK SUM(qty) <= 100")
                  .rfind("CREATED", 0),
              0u);

    // Each takes 60 of a different account's 100, so neither refuses the
    // other yet - and each holds rows, which is what makes an edge *out* of
    // a holder possible at all.
    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &b).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO trades VALUES (7, 60)", &a)
                  .response.rfind("INSERTED", 0),
              0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO trades VALUES (8, 60)", &b)
                  .response.rfind("INSERTED", 0),
              0u);

    // A now wants B's account and B wants A's: each is refused by the
    // other's undecided 60, and each waits for it.
    Started wa = Start("INSERT INTO trades VALUES (8, 50)", a);
    Pump();
    ASSERT_FALSE(*wa.done) << "A did not wait on B's reservation: " << wa.out->response;

    Started wb = Start("INSERT INTO trades VALUES (7, 50)", b);
    Pump();
    // **The mutation**: take the lock table away and neither edge is
    // recorded - both stall to the fault net and both are refused as defect
    // reports, which is the hang AO-R7 exists to convert into a verdict.
    ASSERT_TRUE(*wb.done) << "the waiter that closed the cycle was not refused";
    EXPECT_NE(wb.out->response.find("deadlock"), std::string::npos)
        << "the cycle was ended by something other than the detector: " << wb.out->response;
    EXPECT_EQ(StatusFromErrorReply(wb.out->response).code(), StatusCode::kTxnConflict)
        << wb.out->response;

    // The victim is the waiter that closed it; the survivor's wait ends
    // when that transaction rolls back, which is the contract every other
    // failed statement inside a transaction has.
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &b).response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*wa.done) << "A never resumed after its holder rolled back";
    EXPECT_EQ(wa.out->response.rfind("INSERTED", 0), 0u) << wa.out->response;
}

// ---- AO-S3b: the mid-statement wait --------------------------------------
//
// Everything above waits *between* statements: the statement that meets a
// held row has written nothing, is refused, and is run again from the top
// once the holder decides. A statement that has already written rows could
// not do that - there are no savepoints, so its rows cannot be rolled back
// to a statement boundary - and so was answered with its conflict. These
// cells are the other shape: the walk stops at the held row, the statement
// keeps its scope and its position, and the resume carries on from there.

class MidWalkWaitTest : public LockDeadlockTest {
protected:
    // Ten rows, ids 1..10, every one of them v = 0. Inserted in id order,
    // so the walk meets them in that order too - which is what lets the
    // cell name "row 7" and mean the seventh row the walk reaches.
    void SetUp() override {
        LockDeadlockTest::SetUp();
        // A second relation in the other storage form, ten rows like `t`.
        // **The two arms resume by different means** - `t` by position and
        // `tb` by key - so a cell that ran only against the fixture's
        // default would leave one of them unexercised.
        ASSERT_EQ(Local("CREATE TABLE tb (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
        for (int id = 1; id <= 10; ++id) {
            const std::string vals = " VALUES (" + std::to_string(id) + ", 0)";
            ASSERT_EQ(Local("INSERT INTO t" + vals).rfind("INSERTED", 0), 0u) << "id " << id;
            ASSERT_EQ(Local("INSERT INTO tb" + vals).rfind("INSERTED", 0), 0u) << "id " << id;
        }
    }

    // **Why every cell here writes `WHERE v >= 0` rather than no predicate
    // or a pk one.** A `WHERE`-less write declares the relation (AO-0 item
    // 14) and a pk-shaped one declares its range, and a declared unit is
    // borrowed **before the walk starts** - so such a statement waits
    // having written nothing and re-runs cleanly, which is the better
    // behaviour and has its own cell
    // (`ACoarseDeclarationWaitsBeforeItWritesAnythingAndThenSucceeds`).
    // The mid-walk park these cells exist to measure is what a predicate
    // naming no pk window still takes. Every row is inserted with `v = 0`
    // and a holder's row carries `9`, so `v >= 0` matches all ten whichever
    // value the page holds - `apply` evaluates the WHERE against the page's
    // current bytes, not against the version the view would resolve.
    //
    // How many rows carry the new value. The point of every cell here is
    // *which* rows the statement wrote, so this is what they assert on.
    int RowsWith(int v) {
        // `Rows()` renders "id,v" per row and a header, so a row's value is
        // whatever follows its last comma. **Rows are separated by a
        // literal backslash-n**, not by a newline character - the reply is
        // one line on the wire, which is why every other cell in this file
        // writes its expectation with an escaped separator.
        const std::string rows = Rows();
        const std::string want = std::to_string(v);
        const std::string sep = "\\n";
        int n = 0;
        std::size_t at = 0;
        while (at <= rows.size()) {
            const std::size_t eol = rows.find(sep, at);
            const std::string line = rows.substr(at, eol == std::string::npos ? eol : eol - at);
            const std::size_t comma = line.rfind(',');
            if (comma != std::string::npos && line.substr(comma + 1) == want) ++n;
            if (eol == std::string::npos) break;
            at = eol + sep.size();
        }
        return n;
    }
};

TEST_F(MidWalkWaitTest, ATenRowUpdateMeetingAHeldRowKeepsWhatItWroteAndWaits) {
    // AO-5's S3b cell. The holder takes row 7 and does not decide; the
    // ten-row UPDATE walks into it having written six rows.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    // `v >= 0` for the fixture's reason: the per-row path.
    Started walk = Start("UPDATE t SET v = 1 WHERE v >= 0", w);
    Pump();

    // **It waited rather than answering**, which is the whole stage: before
    // AO-S3b a statement that had written rows was refused outright, because
    // re-running it would have written them twice.
    ASSERT_FALSE(*walk.done) << "the statement did not park: " << walk.out->response;

    // **And it kept what it wrote.** Six rows carry the new value while the
    // statement is parked - the rows before the held one - which is what
    // makes this a mid-statement wait and not a re-run.
    // **What another session sees while it waits: nothing.** The parked
    // statement's six rows are written but uncommitted, so ordinary MVCC
    // hides them - "keeps rows 1-6" is a claim about the statement's own
    // transaction, and the explicit-transaction cell below is where it is
    // observable, on the trail.
    EXPECT_EQ(RowsWith(1), 0) << "a parked statement's writes must not be visible to anyone else";

    // The holder aborts, so the row's prior writer id comes back with its
    // bytes and the parked statement's own view - unchanged across the
    // park - admits the write it refused. That is why the abort arm
    // completes rather than re-refusing.
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Pump();

    ASSERT_TRUE(*walk.done) << "the wait never ended after the holder decided";
    EXPECT_EQ(walk.out->response.rfind("UPDATED 10", 0), 0u)
        << "all ten rows under one snapshot: " << walk.out->response;
    EXPECT_EQ(RowsWith(1), 10);
}

// AP-S4 (the review of 010b8e0, B1): `NOW()` is one instant for the whole
// statement, and a mid-walk park splits a statement into two compiles. Rows
// 1-7 are a day before the pinned instant and rows 8-10 half a day after
// it; the clock moves two days on while the statement waits at row 7. Read
// against one instant the statement writes rows 1-7. Re-taken at the resume,
// rows 8-10 would pass `ts < NOW()` too.
TEST_F(MidWalkWaitTest, NowIsOneInstantAcrossAMidWalkPark) {
    ASSERT_EQ(Local("CREATE TABLE tn (id int64, v int64, ts timestamp) BTREE").rfind("CREATED", 0),
              0u);
    for (int id = 1; id <= 10; ++id) {
        const std::string ts = id <= 7 ? "'2026-10-01 00:00:00'" : "'2026-10-02 12:00:00'";
        ASSERT_EQ(Local("INSERT INTO tn VALUES (" + std::to_string(id) + ", 0, " + ts + ")")
                      .rfind("INSERTED", 0),
                  0u);
    }
    // 2026-10-02 00:00:00 UTC, then 2026-10-04: epoch day 20728 and 20730.
    constexpr std::int64_t kMicrosPerDay = 86'400LL * 1'000'000LL;
    exec::ScopedStatementClockForTest clock(20728 * kMicrosPerDay);

    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE tn SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    Started walk = Start("UPDATE tn SET v = 1 WHERE ts < NOW()", w);
    Pump();
    ASSERT_FALSE(*walk.done) << "the statement did not park: " << walk.out->response;

    clock.Set(20730 * kMicrosPerDay);
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Pump();

    ASSERT_TRUE(*walk.done) << "the wait never ended after the holder decided";
    EXPECT_EQ(walk.out->response.rfind("UPDATED 7", 0), 0u)
        << "the resume filtered against a later instant: " << walk.out->response;
    EXPECT_NE(Local("ANALYZE SELECT id FROM tn WHERE v = 1").find("rows=7 "), std::string::npos);
}

TEST_F(MidWalkWaitTest, ACoarseDeclarationWaitsBeforeItWritesAnythingAndThenSucceeds) {
    // **The behaviour item 14's declared units buy, marked by the operator
    // on 2026-09-08 against `txn.md` §5's ratified refusal.** A `WHERE`-less
    // write declares the whole id space - the relation until AO-S6e-b,
    // which moved the declaration off the relation entry so that a
    // positioned reader's `IS` there does not refuse it - and borrows it
    // before the walk, so it waits having written nothing - and an autocommit statement's re-run
    // mints a fresh view, so it succeeds where the same statement used to
    // write six rows, meet the seventh, and be refused `TxnConflict` with
    // those six compensated. That is AR2-A §1's "refusal → wait" delivered,
    // and it is a real semantic change: for autocommit this shape is now
    // closer to PostgreSQL's READ COMMITTED than to the stricter rule §5
    // states. An explicit transaction keeps its view across the re-run and
    // is refused exactly as before.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    Started walk = Start("UPDATE t SET v = 1", w);
    Pump();
    ASSERT_FALSE(*walk.done) << walk.out->response;
    EXPECT_EQ(RowsWith(1), 0)
        << "the declaration is borrowed before the walk, so nothing is written while it waits";

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &holder).response.rfind("COMMIT", 0), 0u);
    Pump();

    ASSERT_TRUE(*walk.done) << "the wait never ended after the holder committed";
    EXPECT_EQ(walk.out->response.rfind("UPDATED 10", 0), 0u)
        << "the re-run mints a fresh view and writes every row: " << walk.out->response;
    EXPECT_EQ(RowsWith(1), 10);
}

TEST_F(MidWalkWaitTest, TheCommitArmRefusesAndUnwindsWhatTheStatementHadWritten) {
    // The other decide. `txn.md` section 5 already ratifies this as
    // deliberate - stricter than PostgreSQL's READ COMMITTED, which would
    // re-read and update the new version - so what this cell pins is that
    // the refusal arrives with the statement's own six rows **unwound**,
    // not left behind as a partial UPDATE.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    // `v >= 0` for the fixture's reason: the per-row path.
    Started walk = Start("UPDATE t SET v = 1 WHERE v >= 0", w);
    Pump();
    ASSERT_FALSE(*walk.done) << walk.out->response;
    ASSERT_EQ(RowsWith(1), 0) << "uncommitted rows must stay invisible";

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &holder).response.rfind("COMMIT", 0), 0u);
    Pump();

    ASSERT_TRUE(*walk.done) << "the wait never ended after the holder committed";
    EXPECT_EQ(StatusFromErrorReply(walk.out->response).code(), StatusCode::kTxnConflict)
        << walk.out->response;
    // Autocommit, so the statement is the whole transaction and its failure
    // is atomic: the six rows it had written are compensated.
    EXPECT_EQ(RowsWith(1), 0) << "a refused autocommit statement left rows behind";
}

TEST_F(MidWalkWaitTest, InsideAnExplicitTransactionTheRowsAreWrittenOnceNotTwice) {
    // The same statement inside a transaction, and the cell that tells a
    // **resume** from a **re-run**: the trail is this statement's undo
    // record per row, so a resumed walk leaves ten entries where a re-run
    // would leave sixteen - the six it redid plus the ten it then wrote.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &w).response.rfind("BEGIN", 0), 0u);
    // `v >= 0` for the fixture's reason: the per-row path.
    Started walk = Start("UPDATE t SET v = 1 WHERE v >= 0", w);
    Pump();
    ASSERT_FALSE(*walk.done) << walk.out->response;

    ASSERT_NE(w.transaction(), nullptr);
    EXPECT_EQ(w.transaction()->trail().size(), 6u)
        << "the parked statement should have written exactly the rows before the held one";

    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Pump();

    ASSERT_TRUE(*walk.done) << "the wait never ended";
    EXPECT_EQ(walk.out->response.rfind("UPDATED 10", 0), 0u) << walk.out->response;
    EXPECT_EQ(w.transaction()->trail().size(), 10u)
        << "sixteen would mean the statement re-ran from the top instead of resuming";
    EXPECT_FALSE(w.failed()) << "a park is not a failure, so the session must not be poisoned";
}

TEST_F(MidWalkWaitTest, ADeleteParksInTheMiddleOfItsWalkToo) {
    // The same mechanism on the other verb - `DeleteInner` carries its own
    // copy of the stop, and a stage that built it for UPDATE alone would
    // leave DELETE answering a partial count.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    // `v >= 0` for the fixture's reason: the per-row path.
    Started walk = Start("DELETE FROM t WHERE v >= 0", w);
    Pump();
    ASSERT_FALSE(*walk.done) << "the DELETE did not park: " << walk.out->response;

    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*walk.done) << "the wait never ended";
    EXPECT_EQ(walk.out->response.rfind("DELETED 10", 0), 0u) << walk.out->response;
}

TEST_F(MidWalkWaitTest, AClusteredBtreeParksAndResumesByKey) {
    // **The arm SUS-1 makes load-bearing.** A btree cannot resume from a
    // remembered leaf, because a split moves the upper half of a leaf to a
    // new sibling and would carry already-written rows into a page the walk
    // has yet to visit. It resumes by descending to whichever leaf now
    // holds the key it stopped at, so the cell that matters is that the
    // resumed walk finishes the relation exactly once.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE tb SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &w).response.rfind("BEGIN", 0), 0u);
    // `v >= 0` for the fixture's reason: the per-row path.
    Started walk = Start("UPDATE tb SET v = 1 WHERE v >= 0", w);
    Pump();
    ASSERT_FALSE(*walk.done) << "the btree walk did not park: " << walk.out->response;
    ASSERT_NE(w.transaction(), nullptr);
    EXPECT_EQ(w.transaction()->trail().size(), 6u)
        << "the parked btree walk should hold exactly the keys below the held one";

    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*walk.done) << "the wait never ended";
    EXPECT_EQ(walk.out->response.rfind("UPDATED 10", 0), 0u) << walk.out->response;
    // Ten, not sixteen: the four keys at and above the held one were the
    // only ones the resume wrote.
    EXPECT_EQ(w.transaction()->trail().size(), 10u)
        << "the key-ordered skip did not hold - rows below the resume key were written twice";
}

TEST_F(MidWalkWaitTest, ThePointLookupArmStillReportsTheConflictItCannotWriteThrough) {
    // **The arm that does not walk at all.** A bare `WHERE id = k` on a
    // btree relation is answered by a descent, not by `VisitRelation` - so
    // it never reaches the stop the walk carries, and it renders its reply
    // straight from the row counter. AO-S3b made `apply` answer OK for a
    // row it declined to write, which turned that reply into `UPDATED 0`
    // for a row a writer holds: a success line for a write that never
    // happened, with the autocommit commit behind it.
    //
    // One row is in scope, so nothing was written and there is nothing to
    // resume from: the answer is the conflict, and the wait is the
    // whole-statement one AO-S3 already built.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE tb SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    Started point = Start("UPDATE tb SET v = 1 WHERE id = 7", w);
    Pump();
    ASSERT_FALSE(*point.done)
        << "the point UPDATE answered instead of waiting: " << point.out->response;

    // The holder commits and the wait ends. **This is AO-S3's arm, not
    // AO-S3b's**: the statement wrote nothing, so it is re-run whole under
    // a *fresh* snapshot, which sees the committed version and writes
    // through it. One row, written once - the outcome the premature
    // `UPDATED 0` replaced with a silent no-op.
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &holder).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*point.done) << "the wait never ended";
    EXPECT_EQ(point.out->response.rfind("UPDATED 1", 0), 0u) << point.out->response;
    // Read back off `tb` itself - the fixture's `Rows()` helper selects
    // from `t`, so it would answer this question about the wrong relation.
    EXPECT_NE(Local("SELECT * FROM tb WHERE id = 7").find(",1"), std::string::npos)
        << "the row the wait was for was never written";

    // And the DELETE arm beside it, which renders its count the same way.
    Session holder2;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder2).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE tb SET v = 9 WHERE id = 3", &holder2)
                  .response.rfind("UPDATED", 0),
              0u);
    Session d;
    Started point_delete = Start("DELETE FROM tb WHERE id = 3", d);
    Pump();
    ASSERT_FALSE(*point_delete.done)
        << "the point DELETE answered instead of waiting: " << point_delete.out->response;
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &holder2).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*point_delete.done);
    EXPECT_EQ(point_delete.out->response.rfind("DELETED 1", 0), 0u)
        << "a held row was answered as a delete of zero rows: " << point_delete.out->response;
}

TEST_F(MidWalkWaitTest, ABtreeResumeSurvivesALeafSplitUnderThePark) {
    // The hazard the key-ordered resume exists for, made to happen: while
    // the statement is parked, another session inserts enough keys to split
    // the leaf it stopped in. A positional cursor would come back to a page
    // whose upper half - including rows this statement already wrote - now
    // lives in a new sibling it has yet to visit, and would write them
    // twice. Descending by key cannot see that difference.
    //
    // **The keys are sparse, and that is the whole setup.** A split divides
    // a leaf at its middle key, so filling `tb` (ids 1..10) with ids above
    // 100 divides it far above the resume key and leaves every row this
    // walk wrote exactly where it was - a cell that meets a split and never
    // meets the hazard, which a positional cursor passes. The relation here
    // is keyed 1000, 2000 .. 10000 and the fillers are all *below* the held
    // key, so the division falls between the rows the walk already wrote
    // and the ones it has yet to reach. That is the case that tells the two
    // resume shapes apart.
    ASSERT_EQ(Local("CREATE TABLE ts (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
    for (int id = 1000; id <= 10000; id += 1000) {
        ASSERT_EQ(Local("INSERT INTO ts VALUES (" + std::to_string(id) + ", 0)")
                      .rfind("INSERTED", 0),
                  0u)
            << "id " << id;
    }

    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE ts SET v = 9 WHERE id = 7000", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &w).response.rfind("BEGIN", 0), 0u);
    // `v >= 0` rather than no predicate: a `WHERE`-less write declares the
    // relation (item 14) and is borrowed before the walk, so it would wait
    // having written nothing and there would be no parked cursor for a
    // split to straddle. Every row of `ts` is inserted with `v = 0`.
    Started walk = Start("UPDATE ts SET v = 1 WHERE v >= 0", w);
    Pump();
    ASSERT_FALSE(*walk.done) << walk.out->response;
    ASSERT_EQ(w.transaction()->trail().size(), 6u)
        << "the park must be at the seventh key for the split to straddle it";

    // **The leaf count before and after, so the cell cannot pass
    // vacuously.** `DESCRIBE`'s whole line will not do: it carries
    // `ids_issued`, which moves with every insert whether or not a leaf
    // ever divided, so comparing the lines would assert nothing about the
    // tree. The assertion is on `leaves=` itself.
    const auto leaf_count = [&]() -> int {
        const std::string shape = Local("DESCRIBE ts");
        const std::size_t at = shape.find("leaves=");
        EXPECT_NE(at, std::string::npos) << shape;
        if (at == std::string::npos) return -1;
        return std::atoi(shape.c_str() + at + std::string("leaves=").size());
    };
    const int leaves_before = leaf_count();
    ASSERT_EQ(leaves_before, 1)
        << "the ten rows must start in one leaf for it to be the leaf the split divides";

    // Enough keys *below* the held one to force the division there. They
    // are all invisible to the parked walk's snapshot, so none of them may
    // appear in its count.
    for (int id = 1; id <= 400; ++id) {
        ASSERT_EQ(Local("INSERT INTO ts VALUES (" + std::to_string(id) + ", 7)")
                      .rfind("INSERTED", 0),
                  0u)
            << "id " << id;
    }
    EXPECT_GT(leaf_count(), leaves_before)
        << "no leaf ever divided, so this cell would pass without exercising the hazard it is "
           "named for";

    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*walk.done) << "the wait never ended after a split under the park";
    EXPECT_EQ(walk.out->response.rfind("UPDATED 10", 0), 0u)
        << "the resumed walk must see its own ten rows and none of the 400 inserted after its "
           "snapshot: " << walk.out->response;
    EXPECT_EQ(w.transaction()->trail().size(), 10u)
        << "a row below the resume key was written a second time after the split";
}

TEST_F(MidWalkWaitTest, ADeadlockBetweenTwoMidWalkParksUnwindsTheVictimsOpenScope) {
    // A cycle in which **both** transactions are parked mid-walk holding
    // rows, which no other cell in this file produces: every existing
    // deadlock cell uses `WHERE id = k` statements that write nothing
    // before conflicting.
    //
    // **What it does not prove**, stated because the first version of this
    // comment claimed it did: it does not exercise `RefuseParkedWrite`'s
    // scope cleanup. Both victims here are inside explicit transactions,
    // where `EndWrite`'s unowned arm only poisons - which `RefuseParkedWrite`
    // does anyway - so the cell passes with the cleanup compiled out. It was
    // checked that way. The cleanup's own cell is the autocommit one below,
    // where the scope is *owned* and someone has to end it.
    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &b).response.rfind("BEGIN", 0), 0u);

    // **The anchors carry distinguishing values, and since AO-S6c-c that is
    // what makes the shape reachable at all.** The cell used to give the two
    // walkers disjoint pk windows (`id >= 5`, `id >= 2`); a pk-shaped
    // predicate now declares a range and is borrowed before the walk, so
    // neither would write anything before conflicting. A non-pk predicate
    // keeps the per-row path - but both walkers then start at row 1, and
    // the second meets the first's rows immediately and writes nothing,
    // which is the coverage this cell exists to add. So the anchors' values
    // are what carve the two walks apart: A takes row 10 and B row 5, and
    // each walker's predicate selects the rows the other has not touched.
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 1 WHERE id = 10", &a)
                  .response.rfind("UPDATED", 0),
              0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 5", &b)
                  .response.rfind("UPDATED", 0),
              0u);

    // A walks 1..4, writes them, and meets row 5 - B's. It parks holding
    // four rows, and the graph gets the edge A -> B.
    Started wa = Start("UPDATE t SET v = 3 WHERE v >= 0", a);
    Pump();
    ASSERT_FALSE(*wa.done) << "A did not park mid-walk: " << wa.out->response;
    EXPECT_EQ(locks_->WaitEdgeCount(), 1u);

    // B walks 6..9, writes them, and meets row 10 - which A is holding.
    // That edge closes the cycle, so B is the victim by AO-R7's rule, and
    // B has an open scope with four rows of its own already written.
    //
    // `v <= 1` selects exactly those: rows 1..4 now read as A's uncommitted
    // `3`, row 5 as B's own `2`, and row 10 as A's anchor `1` - `apply`
    // evaluates the WHERE against the page's current bytes, so the
    // uncommitted values are what the predicate sees.
    Started wb = Start("UPDATE t SET v = 4 WHERE v <= 1", b);
    Pump();
    ASSERT_TRUE(*wb.done) << "the cycle was not detected; only the fault net would end this";
    const Status victim = StatusFromErrorReply(wb.out->response);
    EXPECT_EQ(victim.code(), StatusCode::kTxnConflict) << wb.out->response;
    EXPECT_NE(wb.out->response.find("deadlock"), std::string::npos) << wb.out->response;

    // **The scope was ended, not abandoned.** Inside an explicit
    // transaction that means poisoned rather than unwound - the rows stay
    // and the client must ROLLBACK - which is exactly what `EndWrite`'s
    // unowned arm does for every other failed statement.
    EXPECT_TRUE(b.failed()) << "the victim's scope was left open and its session unpoisoned";
    EXPECT_EQ(dispatcher_->Dispatch("SELECT id, v FROM t", &b).response,
              "ERR current transaction is aborted; commands are ignored until ROLLBACK");

    // And the ROLLBACK is clean: it takes back both B's anchor and the
    // three rows its parked statement had written.
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &b).response.rfind("ROLLBACK", 0), 0u);
    Pump();

    // A was never touched - a detector aborts the waiter, never the holder
    // - and proceeds once B's rollback releases row 10.
    ASSERT_TRUE(*wa.done) << "the survivor never proceeded, so the cycle was broken at both ends";
    EXPECT_EQ(wa.out->response.rfind("UPDATED 10", 0), 0u)
        << "every row, counted across A's park: " << wa.out->response;
    // Non-vacuous only because B genuinely wrote rows 6..9 before it was
    // refused: the assertion below is what proves they went with its
    // rollback, and it says nothing at all if B never wrote any.
    ASSERT_EQ(RowsWith(2), 0) << "B's anchor outlived its rollback";
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    EXPECT_EQ(locks_->WaitEdgeCount(), 0u);
    // B's writes are gone and A's are all there: nothing of the victim's
    // open scope survived its refusal.
    EXPECT_EQ(RowsWith(4), 0) << "the victim's rows outlived its rollback";
    EXPECT_EQ(RowsWith(3), 10);
}

TEST_F(MidWalkWaitTest, ARefusedAutocommitParkDoesNotLeaveItsScopeForTheNextStatement) {
    // **`RefuseParkedWrite`'s scope cleanup, and the hazard it exists for.**
    // An autocommit statement parked mid-walk owns its transaction: the
    // session does not know about it (`session.transaction()` is null and
    // `in_explicit_txn()` is false), so if the refusal does not end that
    // scope, nothing does. Worse than the leak is what it leaves behind -
    // `session.parked_write()` stays set, and the **next** statement on this
    // session takes the resume branch at the top of `HandleUpdate`, picking
    // up a dead transaction and a stale cursor and skipping every row below
    // it.
    //
    // The wait is ended by the fault net rather than a decide, because that
    // is the refusal arm reachable with a holder that never decides.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 10", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    Started walk = Start("UPDATE t SET v = 3 WHERE id >= 5", w);
    Pump();
    ASSERT_FALSE(*walk.done) << "the autocommit statement did not park: " << walk.out->response;

    // Past the net. The holder never decides, so this is the arm that ends
    // it - and AO-R8 calls reaching it a defect report rather than an
    // outcome, which is what this cell is standing in for.
    clock_.Advance(txn::kLockWaitFaultNetNs + 1);
    Pump();
    ASSERT_TRUE(*walk.done) << "the fault net never fired";
    EXPECT_EQ(StatusFromErrorReply(walk.out->response).code(), StatusCode::kTxnConflict)
        << walk.out->response;

    // **The decisive assertion.** A fresh statement on the same session must
    // be a *fresh* statement. With the cleanup compiled out it resumes from
    // the refused statement's cursor instead and answers `UPDATED 6`,
    // covering only rows 5..10 - the rows below the stale cursor silently
    // skipped, which is the wrong answer this block prevents.
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Started after = Start("UPDATE t SET v = 5 WHERE id >= 1", w);
    Pump();
    ASSERT_TRUE(*after.done) << after.out->response;
    EXPECT_EQ(after.out->response.rfind("UPDATED 10", 0), 0u)
        << "the next statement resumed into the refused one's scope and cursor: "
        << after.out->response;
    EXPECT_EQ(RowsWith(5), 10);
}

TEST_F(MidWalkWaitTest, WithoutADetectorTheMidWalkParkIsNotOffered) {
    // **The guard is unchanged and this is where it shows.** A statement
    // parked mid-walk is a waiter that holds rows, which is exactly what
    // AO-S3's narrow rule excludes when there is no detector - so with the
    // table taken away the ten-row UPDATE is refused at row 7 rather than
    // parking, and its own rows are unwound. AO-S3b widens what a waiter
    // may be, never where it may wait.
    dispatcher_->set_locks(nullptr);
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    // `v >= 0` for the fixture's reason: the per-row path.
    Started walk = Start("UPDATE t SET v = 1 WHERE v >= 0", w);
    Pump();
    ASSERT_TRUE(*walk.done) << "it parked with no detector to end a cycle it could join";
    EXPECT_EQ(StatusFromErrorReply(walk.out->response).code(), StatusCode::kTxnConflict)
        << walk.out->response;
    EXPECT_EQ(RowsWith(1), 0) << "the refused statement left rows behind";
}


}  // namespace
}  // namespace kds::server
