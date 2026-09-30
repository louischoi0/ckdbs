#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"

#include "act_on_fetch_store.hpp"

// **A foreign key's parent is held before it is read** (D9(a), AY-Q2 -
// `[quiet-wrong]` if reversed; `raft-marks-2026-09-30.md` §2). The forward
// check takes `IS` on the parent relation and `S` on the parent row, and only
// then descends. Reversed, a check that passes leaves a gap between its read
// and its hold in which a whole `DELETE` of the parent fits.
//
// That gap holds no page fetch, so no seam can put another core's `DELETE`
// in it, and the two-core window cell
// (`FkCrossCoreRigTest.AParentDeletedBetweenAChildsCheckAndItsWriteLeavesNoOrphan`)
// passes with the order reversed. **What the order changes deterministically
// is whether the parent is read at all while its row is held**: with the
// `S` first, a writer's `X` refuses the ask before the descent; reversed,
// the descent reads the parent's leaf and then the ask is refused. This cell
// watches that read (`ActOnFetchStore`), on one dispatcher with a lock table
// and a synchronous dispatch, where a refused hold is the refusal itself.

namespace kds::server {
namespace {

class FkParentHoldTest : public ::testing::Test {
protected:
    // Above the manager, which releases its live transactions' borrows
    // through the table when it dies (`lock_family_test.cpp` says why).
    std::unique_ptr<txn::LockTable> locks_;

    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/8000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        auto table = txn::LockTable::Create(/*core_count=*/1, /*max_locks_per_txn=*/1024);
        ASSERT_TRUE(table.ok()) << table.status().message();
        locks_ = std::move(table.value());
        txns_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr, /*visibility=*/nullptr, /*core=*/0,
                      locks_.get());
        // Two sessions' dispatchers over one catalog, manager and table:
        // `other_` runs what another session does inside a fetch seam.
        for (auto* d : {&dispatcher_, &other_}) {
            d->emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                       /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                       exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                       /*access_statistics=*/false, &cabins_, &*txns_);
            (*d)->set_locks(locks_.get());
        }

        ASSERT_EQ(Run("CREATE TABLE p (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
        ASSERT_EQ(Run("CREATE TABLE c (id int64, pid int64 REFERENCES p) BTREE").rfind("CREATED", 0),
                  0u);
        auto oid = boot_->catalog.FindTableOidByName("p");
        ASSERT_TRUE(oid.ok()) << oid.status().message();
        auto access = boot_->catalog.InitTableAccess(oid.value());
        ASSERT_TRUE(access.ok()) << access.status().message();
        parent_leaf_ = access.value()->desc_page_id;  // one row: the root is the leaf
        auto child = boot_->catalog.FindTableOidByName("c");
        ASSERT_TRUE(child.ok()) << child.status().message();
        auto child_access = boot_->catalog.InitTableAccess(child.value());
        ASSERT_TRUE(child_access.ok()) << child_access.status().message();
        child_leaf_ = child_access.value()->desc_page_id;
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    std::string Run(Session& s, const std::string& sql) {
        return dispatcher_->Dispatch(sql, &s).response;
    }

    storage::InMemoryPageStore backing_{kFirstUserPageId};
    testing_race::ActOnFetchStore store_{backing_};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> txns_;
    stats::CabinStore cabins_;  // the instance's one store, as since AT-S7
    std::optional<CommandDispatcher> dispatcher_;
    std::optional<CommandDispatcher> other_;
    PageId parent_leaf_ = kInvalidPageId;
    PageId child_leaf_ = kInvalidPageId;
};

// **Mutation**: the `S` asked after `CheckParentPresent` in
// `ResolveForeignKeyParents` - the order AY-Q2 marks against. The child's
// statement then reads the parent's leaf before its hold is refused.
TEST_F(FkParentHoldTest, AParentRowHeldByAWriterIsNotReadBeforeTheChildHoldsIt) {
    Session writer;
    ASSERT_EQ(Run(writer, "BEGIN").rfind("BEGIN", 0), 0u);
    ASSERT_EQ(Run(writer, "INSERT INTO p VALUES (7, 0)").rfind("INSERTED", 0), 0u);

    bool read_while_held = false;
    store_.OnFetch(parent_leaf_, 1, [&] { read_while_held = true; });
    const std::string refused = Run("INSERT INTO c VALUES (7)");
    EXPECT_EQ(refused.rfind("ERR TXN_CONFLICT", 0), 0u) << refused;
    EXPECT_FALSE(read_while_held) << "the child read its parent before holding it: " << refused;

    // The converse: with the writer decided, the check holds and then reads.
    ASSERT_EQ(Run(writer, "COMMIT").rfind("COMMIT", 0), 0u);
    store_.OnFetch(parent_leaf_, 1, [&] { read_while_held = true; });
    EXPECT_EQ(Run("INSERT INTO c VALUES (7)").rfind("INSERTED", 0), 0u);
    EXPECT_TRUE(read_while_held) << "the check passed without descending the parent";
}

// **The reverse check reads under a view minted after the parent row is
// held** (AY-S5's review). A `DELETE` whose view is older than a child it
// meets can read that child's earlier version as absent: `ResolveThroughUndo`
// answers "no version" where the chain ends at an insert the view cannot
// see. So a child inserted and committed after the `DELETE` began, then
// moved off the parent by a writer still open, was answered "no children" -
// and that writer's rollback put a reference to a deleted parent back.
//
// Driven on one thread: inside the `DELETE`'s first read of the child's
// leaf - row 1's reverse check, row 7's `X` not yet asked - another session
// inserts and commits `c(1, pid = 7)`, and a third moves it to 8 and stays
// open. The `DELETE` then holds 7 and walks.
//
// **Mutation**: the reverse check's view minted once per statement, at the
// `DELETE`'s start - `DeleteInner`'s shape at `826d15b`. The `DELETE`
// answers `DELETED 2` and the rollback leaves a child of 7 with no 7.
TEST_F(FkParentHoldTest, AChildCommittedAfterTheDeleteBeganIsSeenUnderItsMove) {
    ASSERT_EQ(Run("INSERT INTO p VALUES (1, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Run("INSERT INTO p VALUES (7, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Run("INSERT INTO p VALUES (8, 1)").rfind("INSERTED", 0), 0u);

    Session mover;
    std::string inserted;
    std::string moved;
    store_.OnFetch(child_leaf_, 1, [&] {
        inserted = other_->Dispatch("INSERT INTO c VALUES (1, 7)").response;
        (void)other_->Dispatch("BEGIN", &mover);
        moved = other_->Dispatch("UPDATE c SET pid = 8 WHERE id = 1", &mover).response;
    });
    const std::string deleted = Run("DELETE FROM p WHERE v = 0");
    ASSERT_TRUE(store_.fired()) << "the seam never ran; the cell tested nothing";
    ASSERT_EQ(inserted.rfind("INSERTED", 0), 0u) << inserted;
    ASSERT_EQ(moved, "UPDATED 1");

    // Whatever the DELETE answered, the child's rollback references 7 again.
    ASSERT_EQ(other_->Dispatch("ROLLBACK", &mover).response.rfind("ROLLBACK", 0), 0u);
    EXPECT_EQ(Run("SELECT id FROM p WHERE id = 7"), "id\\n7")
        << "the DELETE answered '" << deleted << "' and left c(1, pid = 7) without its parent";
}

// ---- The Cabin's "no children" answer rests on D9(a) (AY-S6) --------------
//
// A set's count is fixed at `Find`, so an entry appended while the reverse
// check's loop runs is not in it, and a loop that exhausts clears the
// parent. That is sound only because no child can be *made* to reference a
// parent whose `DELETE` holds its row: the child's check asks `S` on that
// row, which the `DELETE`'s `X` refuses.
//
// Driven on one thread: inside the loop's verify of the one dead entry -
// the first fetch of the child's leaf - another session inserts a child of 7.
//
// **Mutation**: the forward check's `S` not asked. The insert passes on the
// parent's committed header, commits, and appends past the loop's count; the
// set clears 7, and the child is left referencing nothing. With the walk in
// place of the clearing return the same mutation leaves no orphan - the walk
// reads the committed child and refuses the `DELETE` - which is why the
// return, and not the walk, is what rests on the `S`.
TEST_F(FkParentHoldTest, AChildWrittenWhileTheSetIsReadCannotBeMadeToReferenceTheParent) {
    ASSERT_EQ(Run("INSERT INTO p VALUES (7, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Run("CREATE CABIN ON c(pid)").rfind("CRE", 0), 0u);
    ASSERT_EQ(Run("INSERT INTO c VALUES (1, 7)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Run("SELECT id FROM c WHERE pid = 7"), "id\\n1");  // banks 7's set
    ASSERT_EQ(Run("DELETE FROM c WHERE id = 1"), "DELETED 1");   // a dead entry

    std::string inserted;
    store_.OnFetch(child_leaf_, 1,
                   [&] { inserted = other_->Dispatch("INSERT INTO c VALUES (2, 7)").response; });
    const std::string deleted = Run("DELETE FROM p WHERE id = 7");
    ASSERT_TRUE(store_.fired()) << "the seam never ran; the cell tested nothing";

    const bool child_written = inserted.rfind("INSERTED", 0) == 0;
    const bool parent_gone = deleted.rfind("DELETED 1", 0) == 0;
    EXPECT_FALSE(child_written && parent_gone) << "insert: " << inserted << "; delete: " << deleted;
    EXPECT_TRUE(parent_gone) << deleted;  // the set, drained, cleared it
}

// AY-Q8 through the Cabin: an `UPDATE` moving a child off 7 leaves its pk in
// 7's set (maintenance is append-only), and its page holds the new value, so
// the loop skips it as not a child of 7. A set the loop exhausts that way is
// not a "no children": the writer is undecided and its rollback puts the
// reference back. The loop gives the set up and the walk answers, waiting on
// that writer - here, on the synchronous path, refusing retryably.
//
// **Mutation**: a non-matching entry skipped whatever its writer's state -
// `DELETED 1`, and the rollback leaves a child of a deleted 7.
TEST_F(FkParentHoldTest, AChildMovedOffTheParentInABankedSetIsNotAClear) {
    ASSERT_EQ(Run("INSERT INTO p VALUES (7, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Run("INSERT INTO p VALUES (8, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Run("CREATE CABIN ON c(pid)").rfind("CRE", 0), 0u);
    ASSERT_EQ(Run("INSERT INTO c VALUES (1, 7)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Run("SELECT id FROM c WHERE pid = 7"), "id\\n1");  // banks 7's set

    Session mover;
    ASSERT_EQ(Run(mover, "BEGIN").rfind("BEGIN", 0), 0u);
    ASSERT_EQ(Run(mover, "UPDATE c SET pid = 8 WHERE id = 1"), "UPDATED 1");
    const std::string deleted = Run("DELETE FROM p WHERE id = 7");
    EXPECT_EQ(deleted.rfind("ERR TXN_CONFLICT", 0), 0u) << deleted;

    ASSERT_EQ(Run(mover, "ROLLBACK").rfind("ROLLBACK", 0), 0u);
    EXPECT_EQ(Run("SELECT id FROM p WHERE id = 7"), "id\\n7")
        << "the DELETE answered '" << deleted << "' and left c(1, pid = 7) without its parent";
}


// ---- A failed check gives back the `S` its own ask took (AZ-S5, AZ-R5) ----
//
// D9(a) holds the parent row's `S` from before the descent to the decide. A
// check that finds no parent fails the statement `FK_VIOLATION` and poisons
// the transaction, which then writes nothing that `S` protects; kept, it
// refused every insert of that parent key until the client's `ROLLBACK`
// (`TXN_CONFLICT` here, where a refused hold is the refusal; the 1 s fault
// net on a served session). Only the ask's own `S` goes: one the transaction
// held before the statement is left, and so is the relation's `IS`, which the
// statement's other parent keys still stand under.
//
// **The self-referencing arm gives back its own `S` the same way, and no
// cell reaches it**: a self-referencing foreign key cannot be declared
// (`ForeignKeyCheckTest.ASelfReferencingForeignKeyCannotBeDeclared` pins
// that, and fails if it changes).
//
// **Mutations**: the held-before answer removed (release whatever the
// violation names), killed by the held-before cell; the `IS` released with
// the `S`, killed by the intention cell.

std::string OtherRun(std::optional<CommandDispatcher>& other, const std::string& sql) {
    return other->Dispatch(sql).response;
}

TEST_F(FkParentHoldTest, AFailedCheckGivesBackTheAbsentParentsHold) {
    Session child;
    ASSERT_EQ(Run(child, "BEGIN").rfind("BEGIN", 0), 0u);
    const std::string failed = Run(child, "INSERT INTO c VALUES (99)");
    ASSERT_NE(failed.find("FK_VIOLATION"), std::string::npos) << failed;

    const std::string parent = OtherRun(other_, "INSERT INTO p VALUES (99, 0)");
    EXPECT_EQ(parent.rfind("INSERTED", 0), 0u)
        << "the failed check still holds the absent parent: " << parent;
    ASSERT_EQ(Run(child, "ROLLBACK").rfind("ROLLBACK", 0), 0u);
}

TEST_F(FkParentHoldTest, AHoldTakenByAnEarlierStatementSurvivesTheViolation) {
    // A zero-row `UPDATE` resolves its SET's parent and holds its `S`
    // (`UPDATED 0`, byte-identical by design) - an `S` on an absent key the
    // transaction had before the failing statement asked.
    Session child;
    ASSERT_EQ(Run(child, "BEGIN").rfind("BEGIN", 0), 0u);
    ASSERT_EQ(Run(child, "UPDATE c SET pid = 99 WHERE id = 12345").rfind("UPDATED 0", 0), 0u);
    const std::string failed = Run(child, "INSERT INTO c VALUES (99)");
    ASSERT_NE(failed.find("FK_VIOLATION"), std::string::npos) << failed;

    const std::string parent = OtherRun(other_, "INSERT INTO p VALUES (99, 0)");
    EXPECT_EQ(parent.rfind("ERR TXN_CONFLICT", 0), 0u)
        << "the violation gave back a hold an earlier statement took: " << parent;
    ASSERT_EQ(Run(child, "ROLLBACK").rfind("ROLLBACK", 0), 0u);
}

TEST_F(FkParentHoldTest, TheRelationsIntentionOutlivesAFailedChecksHold) {
    // One statement, an absent parent and a present one: the violation gives
    // back S(99) and keeps S(10) - and the `IS` on `p` both stood under. A
    // relation `X` on `p` must still be refused while S(10) is held.
    ASSERT_EQ(Run("INSERT INTO p VALUES (10, 0)").rfind("INSERTED", 0), 0u);
    Session child;
    ASSERT_EQ(Run(child, "BEGIN").rfind("BEGIN", 0), 0u);
    const std::string failed = Run(child, "INSERT INTO c VALUES (99), (10)");
    ASSERT_NE(failed.find("FK_VIOLATION"), std::string::npos) << failed;

    const std::string index = OtherRun(other_, "CREATE INDEX pv ON p (v)");
    EXPECT_EQ(index.rfind("ERR TXN_CONFLICT", 0), 0u)
        << "a relation X was granted over a held parent row: " << index;
    EXPECT_EQ(OtherRun(other_, "INSERT INTO p VALUES (99, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Run(child, "ROLLBACK").rfind("ROLLBACK", 0), 0u);
}
}  // namespace
}  // namespace kds::server
