#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"

#include "act_on_fetch_store.hpp"

// **A catalog relation that could not be read is not an empty one** (the
// whole-engine audit's G35 and G40, `docs/pending/whole-engine-audit-second-run-twelve-slices.md`
// §11).
//
// Three statements asked a catalog scan a question and read every failure of
// the scan as the answer "no such row":
//
// - `DROP INDEX` / `REINDEX INDEX` answered "no index named" for an index
//   that exists, at either of its two lookups;
// - `DROP CABIN` answered "no cabin on" for a column that has one;
// - `CREATE CABIN` took a failed duplicate scan for "none exists" and went
//   on to write a second row for the column.
//
// A catalog chain past its root page is an ordinary evictable frame
// (`catalog.md` CT5), so the read can fail for want of a frame as well as
// for damage. The cells refuse one fetch of the relation's root page
// (`ActOnFetchStore::FailFetch`, a device that will not read, once) and
// assert that the statement answers the read's failure and leaves the
// catalog as it was. **Each ran red with its fix taken out.**

namespace kds::server {
namespace {

bool IsErr(const std::string& reply) { return reply.rfind("ERR", 0) == 0; }

class CatalogReadFailureTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/7000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*mgr_);
        ASSERT_FALSE(IsErr(Run("CREATE TABLE t (id int64, v int64, w int64)")));
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    // The statement, with the `nth` fetch of `page` from here on refused.
    std::string RunRefusing(const std::string& sql, PageId page, int nth) {
        store_.FailFetch(page, nth);
        return Run(sql);
    }

    static void ExpectTheReadsFailure(const std::string& reply, const std::string& what) {
        EXPECT_TRUE(IsErr(reply)) << what << " -> " << reply;
        EXPECT_NE(reply.find("refused a read"), std::string::npos) << what << " -> " << reply;
    }

    storage::InMemoryPageStore backing_{kFirstUserPageId};
    testing_race::ActOnFetchStore store_{backing_};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

// G35. The first refused fetch of `sys.indexes` is the dispatcher's lookup,
// the one that decides whether a relation is borrowed at all; the second is
// the lookup under the relation's `X`. Either read as a miss answered "no
// index named 'ix'" about an index that is there.
TEST_F(CatalogReadFailureTest, ADropIndexWhoseCatalogReadFailedDoesNotAnswerNoSuchIndex) {
    ASSERT_FALSE(IsErr(Run("CREATE INDEX ix ON t (v)")));

    for (int nth : {1, 2}) {
        const std::string reply =
            RunRefusing("DROP INDEX ix", ::kds::catalog::kCatalogPageIndexes, nth);
        ExpectTheReadsFailure(reply, "DROP INDEX, fetch " + std::to_string(nth));
        EXPECT_EQ(reply.find("no index named"), std::string::npos) << reply;
    }
    for (int nth : {1, 2}) {
        const std::string reply =
            RunRefusing("REINDEX INDEX ix", ::kds::catalog::kCatalogPageIndexes, nth);
        ExpectTheReadsFailure(reply, "REINDEX INDEX, fetch " + std::to_string(nth));
        EXPECT_EQ(reply.find("no index named"), std::string::npos) << reply;
    }

    // The index is still there, and a read that works still tells a miss
    // from it.
    const std::string miss = Run("DROP INDEX nothing_by_this_name");
    EXPECT_NE(miss.find("no index named 'nothing_by_this_name'"), std::string::npos) << miss;
    EXPECT_FALSE(IsErr(Run("DROP INDEX ix")));
}

// G35's neighbour, the same collapse one statement over.
TEST_F(CatalogReadFailureTest, ADropCabinWhoseCatalogReadFailedDoesNotAnswerNoSuchCabin) {
    ASSERT_FALSE(IsErr(Run("CREATE CABIN ON t (v)")));

    const std::string reply =
        RunRefusing("DROP CABIN ON t (v)", ::kds::catalog::kCatalogPageCabins, 1);
    ExpectTheReadsFailure(reply, "DROP CABIN");
    EXPECT_EQ(reply.find("no cabin on"), std::string::npos) << reply;

    auto rows = boot_->catalog.ListCabins();
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    EXPECT_EQ(rows.value().size(), 1u) << "the cabin was not dropped by a statement that failed";
    EXPECT_FALSE(IsErr(Run("DROP CABIN ON t (v)")));
}

// G40. `CreateCabin` holds `sys.cabins`' root (its first fetch) and then
// scans for a row on the column (its second). A scan that failed was read
// as "this column is bare", and the insert that followed wrote a second row
// for one column - which `catalog.md` CT7 exists to prevent.
TEST_F(CatalogReadFailureTest, ACreateCabinWhoseDuplicateScanFailedWritesNoSecondRow) {
    auto oid = boot_->catalog.FindTableOidByName(::kds::catalog::kNamespacePublic, "t");
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(boot_->catalog.CreateCabin(oid.value(), 1).ok());

    store_.FailFetch(::kds::catalog::kCatalogPageCabins, 2);
    auto second = boot_->catalog.CreateCabin(oid.value(), 1);
    ASSERT_FALSE(second.ok()) << "a second cabin on one column was admitted";
    EXPECT_EQ(second.status().code(), StatusCode::kIoError) << second.status().message();

    auto rows = boot_->catalog.ListCabins();
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    EXPECT_EQ(rows.value().size(), 1u);

    // With the read working, the same call is the duplicate it always was.
    auto third = boot_->catalog.CreateCabin(oid.value(), 1);
    ASSERT_FALSE(third.ok());
    EXPECT_EQ(third.status().code(), StatusCode::kAlreadyExists);
}

}  // namespace
}  // namespace kds::server
