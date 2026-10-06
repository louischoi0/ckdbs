#include <cstdint>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/parser/ast.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"

#include "act_on_fetch_store.hpp"

// **A lookup's slot is the row when the statement reads it** (AT-S14, the
// SQL-level cells its row left owed; AY-S4). `btree_test.cpp`'s
// `ALookupsSlotIsStillItsRowWhenTheCallerReadsIt` pins `BtreeLookup`
// returning the leaf it descended to, held; these pin the two statement
// paths that read through it - a point `SELECT` (`step_vm`) and an FK
// forward check (`fk_check`) - so a caller that went back to re-fetching the
// page by id is caught by what a client sees, not only by the tree.
//
// The divide is another core's `BtreeInsert` of 15 into the full first leaf,
// run at that leaf's **second** fetch (`ActOnFetchStore`): after the
// descent's, which is where a re-fetch by id would meet it. **Through the
// storage contract, not SQL**: since BB-R3 a SQL insert reaches no clustered
// divide - 15 is below `p`'s mark and is refused `OutOfRange` before anything
// is written - while `BtreeInsert` keeps the middle divide
// (`SplitLeafAndInsert`). With the leaf held there is no second fetch and the
// cells pass without the divide running; the mutant - `BtreeLookup`
// releasing the leaf and re-fetching it by id before it returns - runs it
// every time, and both cells go red on what the client sees (the row,
// AY-S4), not on the hook.

namespace kds::server {
namespace {

class BtreeLookupStatementTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/7000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        reader_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                        /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                        exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                        /*access_statistics=*/false, /*cabins=*/nullptr, &*mgr_);

        // Rows of about 1 KiB, eight to a leaf, at ids 10, 20, ...: ascending
        // ids append, so the first leaf fills in key order and a second opens
        // when it is full.
        ASSERT_EQ(Run("CREATE TABLE p (id int64, pad char(1000)) BTREE").rfind("CREATED", 0), 0u);
        auto oid = boot_->catalog.FindTableOidByName("p");
        ASSERT_TRUE(oid.ok()) << oid.status().message();
        auto access = boot_->catalog.InitTableAccess(oid.value());
        ASSERT_TRUE(access.ok()) << access.status().message();
        root_ = access.value()->desc_page_id;
        for (std::uint64_t id = 10;; id += 10) {
            ASSERT_EQ(Run("INSERT INTO p VALUES (" + std::to_string(id) + ", 'x')").rfind("INS", 0),
                      0u);
            auto leaf = btree::BtreeSeekLeaf(store_, root_, 10);
            ASSERT_TRUE(leaf.ok()) << leaf.status().message();
            auto bytes = store_.GetForRead(leaf.value());
            ASSERT_TRUE(bytes.ok()) << bytes.status().message();
            if (heap::PageView(bytes.value().bytes()).next_page_id() != kInvalidPageId) break;
        }
        auto first = btree::BtreeSeekLeaf(store_, root_, 10);
        ASSERT_TRUE(first.ok()) << first.status().message();
        first_leaf_ = first.value();
        auto bytes = store_.GetForRead(first_leaf_);
        ASSERT_TRUE(bytes.ok()) << bytes.status().message();
        // The first leaf's highest key: a divide moves it to a new page.
        upper_ = 10 * static_cast<std::uint64_t>(heap::PageView(bytes.value().bytes()).slot_count());
        ASSERT_GE(upper_, 30u) << "the first leaf must hold enough rows to divide";
    }

    std::string Run(const std::string& sql) { return reader_->Dispatch(sql).response; }

    // Another core's divide of the first leaf, at its second fetch: 15 sorts
    // inside it, and the leaf is full. The row is encoded and the root read
    // here, before the seam is armed - the root from the catalog, which a
    // level growth repoints, not `root_`, which the growth left a leaf.
    void DivideOnRefetch() {
        auto oid = boot_->catalog.FindTableOidByName("p");
        ASSERT_TRUE(oid.ok()) << oid.status().message();
        auto access = boot_->catalog.InitTableAccess(oid.value());
        ASSERT_TRUE(access.ok()) << access.status().message();
        parser::AstValue pad;
        pad.type = parser::ValueType::kStr;
        pad.str_val = "x";
        auto row = exec::EncodeRow(access.value()->schema, access.value()->layout, 15, {pad});
        ASSERT_TRUE(row.ok()) << row.status().message();
        store_.OnFetch(first_leaf_, 2,
                       [this, root = access.value()->desc_page_id, owner = access.value()->oid,
                        payload = std::move(row.value())] {
                           auto placed = btree::BtreeInsert(store_, root, 15, payload,
                                                            /*trx_id=*/1, owner);
                           EXPECT_TRUE(placed.ok())
                               << "the divide must land: " << placed.status().message();
                       });
    }

    storage::InMemoryPageStore backing_{kFirstUserPageId};
    testing_race::ActOnFetchStore store_{backing_};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> reader_;
    PageId root_ = kInvalidPageId;
    PageId first_leaf_ = kInvalidPageId;
    std::uint64_t upper_ = 0;
};

// `step_vm`'s point read: re-fetched by id after the divide, the slot names
// another row, and the residual answers zero rows for a row that exists.
TEST_F(BtreeLookupStatementTest, APointSelectAnswersItsRowUnderADivide) {
    DivideOnRefetch();
    const std::string got = Run("SELECT id FROM p WHERE id = " + std::to_string(upper_));
    EXPECT_EQ(got, "id\\n" + std::to_string(upper_));
}

// `fk_check`'s forward check: re-fetched by id after the divide, the verdict
// is decided on whatever the slot holds - here past the leaf's end, a
// "no such parent" for a parent that exists.
TEST_F(BtreeLookupStatementTest, AForeignKeyCheckDecidesOnTheParentItAskedAbout) {
    ASSERT_EQ(Run("CREATE TABLE c (id int64, pid int64 REFERENCES p) BTREE").rfind("CREATED", 0),
              0u);
    DivideOnRefetch();
    const std::string got = Run("INSERT INTO c VALUES (" + std::to_string(upper_) + ")");
    EXPECT_EQ(got.rfind("INSERTED", 0), 0u) << got;
}

}  // namespace
}  // namespace kds::server
