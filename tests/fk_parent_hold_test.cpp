#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
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
                       /*access_statistics=*/false, /*cabins=*/nullptr, &*txns_);
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

}  // namespace
}  // namespace kds::server
