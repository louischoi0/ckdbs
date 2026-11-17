#include <cstdint>
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
// (`ActOnFetchStore::FailFetch`, a device that will not read, once).
//
// **Every fetch the statement makes of that page is refused in turn**, the
// first, then the second, and on to `kFetches`: which fetch is the lookup
// under test is the statement's business and moves with it, so no cell
// names one. Whichever read the refusal lands on, the statement must answer
// that read's failure - never "no such row" - and leave the catalog as it
// was; a refusal past the statement's last fetch is not reached and the
// statement simply succeeds. **Each cell ran red with its fix taken out.**

namespace kds::server {
namespace {

// More fetches of one catalog root than any statement here makes.
constexpr int kFetches = 12;

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

    // The statement, with the `nth` fetch of `page` from here on refused -
    // and the refusal disarmed after it, reached or not, so that what the
    // cell asks next is not what trips it.
    std::string RunRefusing(const std::string& sql, PageId page, int nth) {
        store_.FailFetch(page, nth);
        std::string reply = Run(sql);
        Disarm();
        return reply;
    }
    void Disarm() { store_.FailFetch(kInvalidPageId, 0); }

    // A refused statement answered the read's failure, in the read's words.
    static void ExpectTheReadsFailure(const std::string& reply, const std::string& what,
                                      const char* never) {
        EXPECT_NE(reply.find("refused a read"), std::string::npos) << what << " -> " << reply;
        EXPECT_EQ(reply.find(never), std::string::npos) << what << " -> " << reply;
    }

    // The oid of index `ix`, or 0 where there is none.
    std::uint64_t IndexOid() {
        auto row = boot_->catalog.LookupIndexByName("ix");
        EXPECT_TRUE(row.ok()) << row.status().message();
        return row.ok() && row.value().has_value() ? row.value()->index_oid : 0;
    }
    std::size_t Cabins() {
        auto rows = boot_->catalog.ListCabins();
        EXPECT_TRUE(rows.ok()) << rows.status().message();
        return rows.ok() ? rows.value().size() : 0;
    }

    storage::InMemoryPageStore backing_{kFirstUserPageId};
    testing_race::ActOnFetchStore store_{backing_};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

// G35. Two lookups read `sys.indexes`: the dispatcher's, which decides
// whether a relation is borrowed at all, and the one under the relation's
// `X`. Either read as a miss answered "no index named 'ix'" about an index
// that is there.
TEST_F(CatalogReadFailureTest, ADropIndexWhoseCatalogReadFailedDoesNotAnswerNoSuchIndex) {
    ASSERT_FALSE(IsErr(Run("CREATE INDEX ix ON t (v)")));
    int refused = 0;
    for (int nth = 1; nth <= kFetches; ++nth) {
        const std::uint64_t before = IndexOid();
        ASSERT_NE(before, 0u);
        const std::string reply =
            RunRefusing("DROP INDEX ix", ::kds::catalog::kCatalogPageIndexes, nth);
        if (!IsErr(reply)) {
            // The refusal lay past the statement's last fetch: it dropped.
            EXPECT_EQ(IndexOid(), 0u);
            ASSERT_FALSE(IsErr(Run("CREATE INDEX ix ON t (v)")));
            continue;
        }
        ++refused;
        ExpectTheReadsFailure(reply, "DROP INDEX, fetch " + std::to_string(nth),
                              "no index named");
        EXPECT_EQ(IndexOid(), before) << "a refused drop dropped the index";
    }
    EXPECT_GE(refused, 2) << "both lookups read sys.indexes";

    // A read that works still tells a miss from it.
    const std::string miss = Run("DROP INDEX nothing_by_this_name");
    EXPECT_NE(miss.find("no index named 'nothing_by_this_name'"), std::string::npos) << miss;
}

TEST_F(CatalogReadFailureTest, AReindexWhoseCatalogReadFailedDoesNotAnswerNoSuchIndex) {
    ASSERT_FALSE(IsErr(Run("CREATE INDEX ix ON t (v)")));
    int refused = 0;
    for (int nth = 1; nth <= kFetches; ++nth) {
        const std::uint64_t before = IndexOid();
        ASSERT_NE(before, 0u);
        const std::string reply =
            RunRefusing("REINDEX INDEX ix", ::kds::catalog::kCatalogPageIndexes, nth);
        if (!IsErr(reply)) {
            EXPECT_NE(IndexOid(), before) << "a rebuild takes a new oid";
            continue;
        }
        ++refused;
        ExpectTheReadsFailure(reply, "REINDEX INDEX, fetch " + std::to_string(nth),
                              "no index named");
        EXPECT_EQ(IndexOid(), before) << "a refused rebuild replaced the index";
    }
    EXPECT_GE(refused, 2) << "both lookups read sys.indexes";
}

// G35's neighbour, the same collapse one statement over.
TEST_F(CatalogReadFailureTest, ADropCabinWhoseCatalogReadFailedDoesNotAnswerNoSuchCabin) {
    ASSERT_FALSE(IsErr(Run("CREATE CABIN ON t (v)")));
    int refused = 0;
    for (int nth = 1; nth <= kFetches; ++nth) {
        ASSERT_EQ(Cabins(), 1u);
        const std::string reply =
            RunRefusing("DROP CABIN ON t (v)", ::kds::catalog::kCatalogPageCabins, nth);
        if (!IsErr(reply)) {
            EXPECT_EQ(Cabins(), 0u);
            ASSERT_FALSE(IsErr(Run("CREATE CABIN ON t (v)")));
            continue;
        }
        ++refused;
        ExpectTheReadsFailure(reply, "DROP CABIN, fetch " + std::to_string(nth), "no cabin on");
        EXPECT_EQ(Cabins(), 1u) << "a refused drop dropped the cabin";
    }
    EXPECT_GE(refused, 1);
}

// G40. `CreateCabin` scans `sys.cabins` for a row on the column under the
// root's hold. A scan that failed was read as "this column is bare", and
// the insert that followed wrote a second row for one column - which
// `catalog.md` CT7 exists to prevent.
TEST_F(CatalogReadFailureTest, ACreateCabinWhoseDuplicateScanFailedWritesNoSecondRow) {
    auto oid = boot_->catalog.FindTableOidByName(::kds::catalog::kNamespacePublic, "t");
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(boot_->catalog.CreateCabin(oid.value(), 1).ok());

    int refused = 0;
    for (int nth = 1; nth <= kFetches; ++nth) {
        store_.FailFetch(::kds::catalog::kCatalogPageCabins, nth);
        auto second = boot_->catalog.CreateCabin(oid.value(), 1);
        Disarm();
        ASSERT_FALSE(second.ok()) << "a second cabin on one column was admitted, fetch " << nth;
        // The read's failure where the refusal was reached, the duplicate it
        // always was where it was not.
        if (second.status().code() == StatusCode::kIoError) {
            ++refused;
        } else {
            EXPECT_EQ(second.status().code(), StatusCode::kAlreadyExists)
                << second.status().message();
        }
        ASSERT_EQ(Cabins(), 1u) << "fetch " << nth;
    }
    EXPECT_GE(refused, 1);
}

}  // namespace
}  // namespace kds::server
