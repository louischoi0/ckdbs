#include "kds/exec/assertion_build.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/assertion_catalog.hpp"
#include "kds/exec/assertion_replay.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/cabin_bound_page.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

// The CREATE-time builder and its cutover (docs/spec/assertion.md §8.1,
// workplan AST06), tested through the statement surface: what a client sees
// is what the acceptance criteria are written in terms of - a CREATE that
// incorporates every live row or refuses whole, a reply that reports what
// was built and is honest that nothing enforces, and a failed CREATE that
// leaves no catalog trace.
//
// The interleaved-write criterion is met structurally rather than by a
// scenario: the build runs synchronously inside the CREATE statement on one
// cooperative core, so no write *can* interleave - `index_ddl.cpp`'s
// "nothing can observe the half-built tree" fact. What is testable instead,
// and is: a write **in flight** when the build starts (an open transaction's
// uncommitted row) refuses the build retryably, because counting it and
// losing the abort would overstate the group forever.

namespace kds::server {
namespace {

class AssertionBuildTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/5000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));

        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);

        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);

        ASSERT_EQ(Run("CREATE TABLE trades (id int64, account int64, qty int64) BTREE")
                      .substr(0, 7),
                  "CREATED");
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    std::string Run(Session& s, const std::string& sql) {
        return dispatcher_->Dispatch(sql, &s).response;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(AssertionBuildTest, ACountBuildIncorporatesEveryLiveRowAndReportsIt) {
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 5)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 3)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (8, 1)").substr(0, 8), "INSERTED");

    const std::string out = Run(
        "CREATE ASSERTION per_account_cap ON trades GROUP BY (account) CHECK COUNT(*) <= 5");
    EXPECT_EQ(out.substr(0, 7), "CREATED") << out;
    EXPECT_NE(out.find(" rows=3"), std::string::npos) << out;
    EXPECT_NE(out.find(" groups=2"), std::string::npos) << out;
    // A real root was published...
    EXPECT_EQ(out.find("cabin_root=4294967295"), std::string::npos) << out;
    EXPECT_NE(out.find("cabin_root="), std::string::npos) << out;
    // ...and the reply says it enforces (AST07): built, checked on the
    // write path, and held by this dispatcher's registry - all three.
    EXPECT_NE(out.find("enforcing=1"), std::string::npos) << out;
    const std::string shown = Run("SHOW ASSERTIONS");
    EXPECT_NE(shown.find("enforcing=1"), std::string::npos) << shown;
}

TEST_F(AssertionBuildTest, ViolatingDataFailsTheCreateNamingTheFirstGroup) {
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 60)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 50)").substr(0, 8), "INSERTED");

    const std::string out = Run(
        "CREATE ASSERTION exposure_cap ON trades GROUP BY (account) CHECK SUM(qty) <= 100");
    EXPECT_EQ(out.substr(0, 23), "ERR ASSERTION_VIOLATION") << out;
    EXPECT_NE(out.find("retryable=0"), std::string::npos) << out;
    // §4.4's message, from the one place it lives: the group, the aggregate
    // spelling, and the enforced ceiling.
    EXPECT_NE(out.find("account=7"), std::string::npos) << out;
    EXPECT_NE(out.find("SUM(qty) would exceed bound 100"), std::string::npos) << out;

    // No trace: no catalog row, and the name is free for a corrected retry.
    EXPECT_NE(Run("SHOW ASSERTIONS").find("assertions=0"), std::string::npos);
    EXPECT_EQ(Run("CREATE ASSERTION exposure_cap ON trades GROUP BY (account) "
                  "CHECK SUM(qty) <= 200")
                  .substr(0, 7),
              "CREATED");
}

TEST_F(AssertionBuildTest, AHeapRelationBuildsThroughItsChain) {
    ASSERT_EQ(Run("CREATE TABLE ledger (id int64, book int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO ledger VALUES (1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO ledger VALUES (1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO ledger VALUES (2)").substr(0, 8), "INSERTED");

    const std::string out =
        Run("CREATE ASSERTION book_cap ON ledger GROUP BY (book) CHECK COUNT(*) < 10");
    EXPECT_EQ(out.substr(0, 7), "CREATED") << out;
    EXPECT_NE(out.find(" rows=3"), std::string::npos) << out;
    EXPECT_NE(out.find(" groups=2"), std::string::npos) << out;
}

TEST_F(AssertionBuildTest, ADeleteMarkedRowIsNotIncorporated) {
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 5)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 3)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (8, 1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("DELETE FROM trades WHERE id = 2"), "DELETED 1");

    const std::string out = Run(
        "CREATE ASSERTION per_account_cap ON trades GROUP BY (account) CHECK COUNT(*) <= 5");
    EXPECT_EQ(out.substr(0, 7), "CREATED") << out;
    // The delete-marked row is absent at latest settled state: two rows, and
    // account 7 still forms a group from its surviving row.
    EXPECT_NE(out.find(" rows=2"), std::string::npos) << out;
    EXPECT_NE(out.find(" groups=2"), std::string::npos) << out;
}

TEST_F(AssertionBuildTest, AnInFlightWriteRefusesTheBuildRetryably) {
    Session s;
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "INSERT INTO trades VALUES (7, 5)").substr(0, 8), "INSERTED");

    // The uncommitted row is neither countable (its abort would overstate
    // the group forever - nothing prunes) nor skippable (its commit would
    // understate it, a false admission). F3's answer: refuse, retryably.
    const std::string out = Run(
        "CREATE ASSERTION per_account_cap ON trades GROUP BY (account) CHECK COUNT(*) <= 5");
    EXPECT_EQ(out.substr(0, 16), "ERR TXN_CONFLICT") << out;
    EXPECT_NE(out.find("retryable=1"), std::string::npos) << out;

    ASSERT_EQ(Run(s, "COMMIT").substr(0, 6), "COMMIT");
    const std::string retried = Run(
        "CREATE ASSERTION per_account_cap ON trades GROUP BY (account) CHECK COUNT(*) <= 5");
    EXPECT_EQ(retried.substr(0, 7), "CREATED") << retried;
    EXPECT_NE(retried.find(" rows=1"), std::string::npos) << retried;
}

TEST_F(AssertionBuildTest, AnEmptyRelationBuildsAnEmptyCabinWithARealRoot) {
    const std::string out = Run(
        "CREATE ASSERTION per_account_cap ON trades GROUP BY (account) CHECK COUNT(*) <= 5");
    EXPECT_EQ(out.substr(0, 7), "CREATED") << out;
    EXPECT_NE(out.find(" rows=0"), std::string::npos) << out;
    EXPECT_NE(out.find(" groups=0"), std::string::npos) << out;
    // The root exists even with nothing under it: the publish names it, and
    // AST07's write hook needs a chain to append to from the first write.
    EXPECT_EQ(out.find("cabin_root=4294967295"), std::string::npos) << out;
}

TEST_F(AssertionBuildTest, DropThenRecreateLeavesNoResidue) {
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 5)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("CREATE ASSERTION per_account_cap ON trades GROUP BY (account) "
                  "CHECK COUNT(*) <= 5")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("DROP ASSERTION per_account_cap").substr(0, 7), "DROPPED");
    EXPECT_NE(Run("SHOW ASSERTIONS").find("assertions=0"), std::string::npos);
    EXPECT_EQ(Run("CREATE ASSERTION per_account_cap ON trades GROUP BY (account) "
                  "CHECK COUNT(*) <= 5")
                  .substr(0, 7),
              "CREATED");
}

TEST_F(AssertionBuildTest, ASpilledGroupValueResolvesBeforeGrouping) {
    // A tag longer than the inline cell width spills to the var-heap, and
    // grouping on the *pointer* instead of the value would split one group
    // in two - the class of failure types.md calls invisible without a
    // baseline. Two rows share one long tag; one differs late in the string.
    ASSERT_EQ(Run("CREATE TABLE tagged (id int64, tag varchar) BTREE").substr(0, 7), "CREATED");
    const std::string long_a(100, 'a');
    ASSERT_EQ(Run("INSERT INTO tagged VALUES ('" + long_a + "')").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO tagged VALUES ('" + long_a + "')").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO tagged VALUES ('" + long_a + "b')").substr(0, 8), "INSERTED");

    const std::string out =
        Run("CREATE ASSERTION tag_cap ON tagged GROUP BY (tag) CHECK COUNT(*) <= 5");
    EXPECT_EQ(out.substr(0, 7), "CREATED") << out;
    EXPECT_NE(out.find(" rows=3"), std::string::npos) << out;
    EXPECT_NE(out.find(" groups=2"), std::string::npos) << out;
}

// ---- A relation split before AT-S9 (AR0 D7, AY-S9) -------------------------
//
// Nothing opens a range since AT-S9, so no volume reaches a split relation
// through SQL; the directory rows are written directly, as a pre-AT volume
// carries them. A split relation is always heap (CC8), so the relation is
// `HEAP`. The row that decides each cell lives in the **second** chain, and
// `desc_page_id` alone is the first: a build that stopped there would
// under-count the group and admit what the assertion forbids.

// Splits `name` at `lo`, and checks the split took.
void SplitRelation(catalog::Catalog& catalog, const char* name, std::uint64_t lo) {
    auto oid = catalog.FindTableOidByName(name, nullptr);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto head = catalog.CreateRangeEntryPage(oid.value(), lo);
    ASSERT_TRUE(head.ok()) << head.status().message();
    ASSERT_TRUE(catalog.OpenRangeRows(oid.value(), lo, head.value()).ok());
    auto access = catalog.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok()) << access.status().message();
    ASSERT_EQ(access.value()->ranges.size(), 2u);
}

// The slots on the second chain's entry page: the cells' premise, that the
// deciding row is in the chain `desc_page_id` does not reach.
std::uint16_t SecondChainSlots(catalog::Catalog& catalog, storage::PageStore& store,
                               const char* name) {
    auto oid = catalog.FindTableOidByName(name, nullptr);
    EXPECT_TRUE(oid.ok()) << oid.status().message();
    auto access = catalog.InitTableAccess(oid.value());
    EXPECT_TRUE(access.ok()) << access.status().message();
    auto page = store.GetForRead(access.value()->ranges.at(1).entry_page);
    EXPECT_TRUE(page.ok()) << page.status().message();
    return heap::PageView(page.value().bytes()).slot_count();
}

TEST_F(AssertionBuildTest, ABuildOverASplitRelationCountsEveryChain) {
    ASSERT_EQ(Run("CREATE TABLE ledger_h (id int64, book int64) HEAP").substr(0, 7), "CREATED");
    // Ids 1 and 2, in the first chain.
    ASSERT_EQ(Run("INSERT INTO ledger_h VALUES (7)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO ledger_h VALUES (7)").substr(0, 8), "INSERTED");
    SplitRelation(boot_->catalog, "ledger_h", /*lo=*/3);
    // Id 3, the first row of the second chain: the group's third row.
    ASSERT_EQ(Run("INSERT INTO ledger_h VALUES (7)").substr(0, 8), "INSERTED");
    ASSERT_EQ(SecondChainSlots(boot_->catalog, store_, "ledger_h"), 1u);

    const std::string refused =
        Run("CREATE ASSERTION book_cap ON ledger_h GROUP BY (book) CHECK COUNT(*) <= 2");
    EXPECT_EQ(refused.substr(0, 23), "ERR ASSERTION_VIOLATION") << refused;
    EXPECT_NE(refused.find("book=7"), std::string::npos) << refused;

    // The converse, so the cell is not passing by refusing everything.
    const std::string out =
        Run("CREATE ASSERTION book_cap ON ledger_h GROUP BY (book) CHECK COUNT(*) <= 3");
    EXPECT_EQ(out.substr(0, 7), "CREATED") << out;
    EXPECT_NE(out.find(" rows=3"), std::string::npos) << out;
    EXPECT_NE(out.find(" groups=1"), std::string::npos) << out;
    EXPECT_NE(out.find("enforcing=1"), std::string::npos) << out;
}

TEST_F(AssertionBuildTest, AnUpperRangeWritePastTheBoundOfASplitRelationIsRefused) {
    ASSERT_EQ(Run("CREATE TABLE ledger_h (id int64, book int64) HEAP").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO ledger_h VALUES (7)").substr(0, 8), "INSERTED");  // id 1
    SplitRelation(boot_->catalog, "ledger_h", /*lo=*/2);
    ASSERT_EQ(Run("INSERT INTO ledger_h VALUES (7)").substr(0, 8), "INSERTED");  // id 2
    ASSERT_EQ(SecondChainSlots(boot_->catalog, store_, "ledger_h"), 1u);

    const std::string out =
        Run("CREATE ASSERTION book_cap ON ledger_h GROUP BY (book) CHECK COUNT(*) <= 2");
    ASSERT_EQ(out.substr(0, 7), "CREATED") << out;
    EXPECT_NE(out.find(" rows=2"), std::string::npos) << out;

    // Id 3, in the second chain. Against a build that saw one row this
    // would be the second and admitted.
    const std::string refused = Run("INSERT INTO ledger_h VALUES (7)");
    EXPECT_EQ(refused.substr(0, 23), "ERR ASSERTION_VIOLATION") << refused;

    // The delete locates the upper row, and its departure frees the
    // group's ground.
    ASSERT_EQ(Run("DELETE FROM ledger_h WHERE id = 2"), "DELETED 1");
    EXPECT_EQ(Run("INSERT INTO ledger_h VALUES (7)").substr(0, 8), "INSERTED");
    EXPECT_EQ(Run("INSERT INTO ledger_h VALUES (7)").substr(0, 23), "ERR ASSERTION_VIOLATION");
}

// ---- The WAL half: emission, then the AST05 fold over what was emitted ----
//
// The strongest statement AST05 and AST06 can make together: the records the
// builder emits are sufficient to rebuild the directory it built. The replay
// side starts from nothing but the stream - pages created where PAGE_INIT
// says, entries and groups folded from ASSERT_BUILD - and must land on the
// same aggregates the live build reported.

class AssertionBuildWalTest : public AssertionBuildTest {
protected:
    void SetUp() override {
        auto device = wal::MemoryLogDevice::Create(kSegmentBytes);
        ASSERT_TRUE(device.ok()) << device.status().message();
        device_ = std::move(device.value());
        wal::WalManagerConfig config;
        config.ring_capacity = wal::kMinRingCapacity;
        auto wal = wal::WalManager::Open(device_.get(), clock_, 0, config);
        ASSERT_TRUE(wal.ok()) << wal.status().message();
        wal_ = std::move(wal.value());

        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/5000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, wal_.get());
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, wal_.get(), wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);
        ASSERT_EQ(Run("CREATE TABLE trades (id int64, account int64, qty int64) BTREE")
                      .substr(0, 7),
                  "CREATED");
    }

    static constexpr std::size_t kSegmentBytes = 1 << 20;

    sched::ManualClock clock_;
    std::unique_ptr<wal::MemoryLogDevice> device_;
    std::unique_ptr<wal::WalManager> wal_;
};

TEST_F(AssertionBuildWalTest, TheEmittedRecordsRebuildTheDirectoryTheBuildProduced) {
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 5)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 3)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (8, 1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("CREATE ASSERTION exposure_cap ON trades GROUP BY (account) "
                  "CHECK SUM(qty) <= 100")
                  .substr(0, 7),
              "CREATED");
    // Everything appended, to the device: Flush moves the whole ring where
    // DrainOnce moves one batch.
    ASSERT_TRUE(wal_->Flush().ok());

    // Replay side: nothing but the stream.
    storage::InMemoryPageStore replayed(kFirstUserPageId);
    exec::BoundCabin rebuilt(exec::BoundAggregate::kSum, /*bound=*/100);
    struct Context : exec::AssertionReplayContext {
        exec::BoundCabin* cabin = nullptr;
        std::uint64_t id = 0;
        exec::BoundCabin* CabinOf(std::uint64_t assertion_id) override {
            return assertion_id == id ? cabin : nullptr;
        }
        void Drop(std::uint64_t) override {}
    } context;
    context.cabin = &rebuilt;

    std::size_t builds = 0;
    for (std::uint64_t seg = 0; seg < device_->segment_count(); ++seg) {
        std::vector<std::byte> body(kSegmentBytes - wal::kSegmentHeaderSize);
        ASSERT_TRUE(device_->ReadAt(seg, wal::kSegmentHeaderSize, body).ok());
        wal::RecordReader reader(body, seg * kSegmentBytes + wal::kSegmentHeaderSize);
        while (std::optional<wal::DecodedRecord> record = reader.Next()) {
            if (record->type() == wal::RecordType::kPad) break;
            // PAGE_INIT with the Bound Cabin page type is the page's birth;
            // recovery's redo applies it, so this test does too.
            if (record->type() == wal::RecordType::kPageInit) {
                auto init = wal::DecodePageInit(record->payload);
                ASSERT_TRUE(init.ok());
                if (init.value().page_type !=
                    static_cast<std::uint8_t>(PageType::kCabinBound)) {
                    continue;
                }
                auto page = replayed.CreateAt(record->header.page_id);
                ASSERT_TRUE(page.ok());
                ASSERT_TRUE(storage::cabin::BoundCabinPage::Format(page.value().bytes()).ok());
                continue;
            }
            if (!exec::IsAssertionRecord(record->type())) continue;
            if (record->type() == wal::RecordType::kAssertBuild && context.id == 0) {
                auto decoded = wal::DecodeAssertEntry(record->payload);
                ASSERT_TRUE(decoded.ok());
                context.id = decoded.value().fields.assertion_id;
            }
            ASSERT_TRUE(exec::ReplayAssertionRecord(*record, replayed, context).ok());
            if (record->type() == wal::RecordType::kAssertBuild) ++builds;
        }
    }
    EXPECT_EQ(builds, 3u) << "one ASSERT_BUILD per incorporated row";

    // The fold landed on what the live build reported: account 7 holds
    // {count 2, sum 8}, account 8 {count 1, sum 1}.
    parser::AstValue seven;
    seven.type = parser::ValueType::kInt;
    seven.int_val = 7;
    parser::AstValue eight = seven;
    eight.int_val = 8;
    const exec::GroupHeader* g7 = rebuilt.Find(exec::EncodeGroupKey({seven}));
    const exec::GroupHeader* g8 = rebuilt.Find(exec::EncodeGroupKey({eight}));
    ASSERT_NE(g7, nullptr);
    ASSERT_NE(g8, nullptr);
    EXPECT_EQ(g7->count, 2);
    EXPECT_EQ(g7->sum, 8);
    EXPECT_EQ(g8->count, 1);
    EXPECT_EQ(g8->sum, 1);

    // And header == Σ(entries) over the replayed pages - §7's invariant,
    // through the verification hook.
    const exec::BoundCabin::EntryReader reader =
        [&replayed](PageId page_id,
                    std::uint16_t index) -> StatusOr<storage::cabin::BoundCabinEntry> {
        auto page = replayed.Get(page_id);
        if (!page.ok()) return page.status();
        auto view = storage::cabin::BoundCabinPage::Open(page.value().bytes());
        if (!view.ok()) return view.status();
        return view.value().Read(index);
    };
    EXPECT_TRUE(rebuilt.VerifyAgainstEntries(reader).ok());
}

}  // namespace
}  // namespace kds::server
