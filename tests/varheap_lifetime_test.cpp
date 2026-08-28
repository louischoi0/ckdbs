#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/tagged_cell.hpp"
#include "kds/storage/varheap.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"

// The var-heap's lifetime, end to end (VC-B of
// `instructions/v2.5.0/varchar-char-architecture.md` §4).
//
// **The model rests on one fact about the code, and the first test here is
// that fact.** An UPDATE re-encodes the whole row, so a spilled value the
// SET list never touched is appended *again* and the new version points at
// the copy - which means no two versions of a tuple share a var-heap slot.
// "A version dies, so its slots die" is exact only while that holds, and an
// optimisation that reused the old slot would silently make the release
// wrong. So it is pinned, not assumed.
//
// Everything else here is the consequence: a rolled-back write releases
// exactly what it wrote, and a committed one releases nothing.
//
// The unlogged path throughout (no WalManager): what is under test is the
// live rollback and the page state it leaves. The record-level half -
// whether a crash mid-flight recovers to the same place - is
// recovery_undo_test.cpp's and insert_wal_test.cpp's.

namespace kds::server {
namespace {

class VarHeapLifetimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/4000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));

        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);

        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*mgr_);
    }

    std::string Run(Session& s, const std::string& sql) {
        return dispatcher_->Dispatch(sql, &s).response;
    }

    const catalog::TableAccess* Access(const std::string& table) {
        auto oid = boot_->catalog.FindTableOidByName(table);
        EXPECT_TRUE(oid.ok()) << oid.status().message();
        if (!oid.ok()) return nullptr;
        auto access = boot_->catalog.InitTableAccess(oid.value());
        EXPECT_TRUE(access.ok()) << access.status().message();
        return access.ok() ? access.value() : nullptr;
    }

    // Slots ever written, and slots still holding a value, over the whole
    // of a relation's chain. The pair is the interesting reading: appends
    // only ever raise the first, so the gap between them *is* what has been
    // released.
    struct ChainCensus {
        std::uint32_t slots = 0;
        std::uint32_t live = 0;
    };

    ChainCensus Census(const std::string& table) {
        ChainCensus out;
        const catalog::TableAccess* ta = Access(table);
        EXPECT_NE(ta, nullptr);
        if (ta == nullptr) return out;
        PageId at = ta->varheap_page_id;
        while (at != kInvalidPageId) {
            auto page = store_.GetForRead(at);
            EXPECT_TRUE(page.ok()) << page.status().message();
            if (!page.ok()) break;
            out.slots += varheap::PageSlotCount(page.value().bytes());
            out.live += varheap::PageLiveSlots(page.value().bytes());
            at = varheap::PageNextPageId(page.value().bytes());
        }
        return out;
    }

    // A value that cannot inline in the default cell, so every write of it
    // spills exactly once.
    static std::string Spilling(char fill) {
        return std::string(storage::InlineCapacity(storage::kDefaultInlineCellWidth) + 20, fill);
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- The premise the whole model rests on --------------------------------

TEST_F(VarHeapLifetimeTest, AnUpdateOfAnotherColumnGivesTheSpilledValueANewSlot) {
    // **Do not "optimise" this away without replacing the lifetime model.**
    // An UPDATE re-encodes the whole row, so an untouched spilled value is
    // copied to a fresh slot and the new version points at the copy. That
    // exclusivity is what makes "the version died, so release its slots"
    // exact; sharing a slot between versions would make it a double free.
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, note varchar, n int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "INSERT INTO t VALUES ('" + Spilling('a') + "', 1)").substr(0, 8),
              "INSERTED");
    ASSERT_EQ(Census("t").slots, 1u);

    // `note` is not in the SET list, and it still gets a second slot.
    ASSERT_EQ(Run(s, "UPDATE t SET n = 2 WHERE id = 1").substr(0, 7), "UPDATED");
    EXPECT_EQ(Census("t").slots, 2u)
        << "an untouched spilled value was not re-appended; the lifetime model's "
           "exclusive-ownership premise no longer holds";
    // Both are live: the old version is still reachable by a snapshot that
    // predates the update, and nothing has settled below the horizon.
    EXPECT_EQ(Census("t").live, 2u);
}

// ---- Rollback releases exactly what it wrote -----------------------------

TEST_F(VarHeapLifetimeTest, ARolledBackSpillingInsertReleasesExactlyItsSlots) {
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, a varchar, b varchar)").substr(0, 7), "CREATED");
    // A committed row first, so the test can tell "released the loser's
    // slots" from "released everything".
    ASSERT_EQ(Run(s, "INSERT INTO t VALUES ('" + Spilling('k') + "', '" + Spilling('k') + "')")
                  .substr(0, 8),
              "INSERTED");
    const ChainCensus before = Census("t");
    ASSERT_EQ(before.slots, 2u);
    ASSERT_EQ(before.live, 2u);

    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "INSERT INTO t VALUES ('" + Spilling('x') + "', '" + Spilling('y') + "')")
                  .substr(0, 8),
              "INSERTED");
    ASSERT_EQ(Census("t").slots, 4u);
    ASSERT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");

    const ChainCensus after = Census("t");
    // Slots are never un-written - the directory does not shrink - so the
    // proof is that exactly the two the loser wrote went dead.
    EXPECT_EQ(after.slots, 4u);
    EXPECT_EQ(after.live, 2u) << "a rolled-back insert left its spilled values reachable";
    // And the committed row is untouched by every route.
    EXPECT_NE(Run(s, "SELECT a FROM t").find(Spilling('k')), std::string::npos);
}

TEST_F(VarHeapLifetimeTest, ARolledBackUpdateReleasesTheNewCopyAndKeepsTheOld) {
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, note varchar)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "INSERT INTO t VALUES ('" + Spilling('o') + "')").substr(0, 8), "INSERTED");
    ASSERT_EQ(Census("t").live, 1u);

    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "UPDATE t SET note = '" + Spilling('n') + "' WHERE id = 1").substr(0, 7),
              "UPDATED");
    ASSERT_EQ(Census("t").slots, 2u);
    ASSERT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");

    const ChainCensus after = Census("t");
    EXPECT_EQ(after.slots, 2u);
    // The new copy died with the version that wrote it; the old one is the
    // live version's and must not have been touched.
    EXPECT_EQ(after.live, 1u) << "the rolled-back UPDATE's copy is still reachable";
    EXPECT_NE(Run(s, "SELECT note FROM t").find(Spilling('o')), std::string::npos)
        << "the surviving version's value was released with the loser's";
}

TEST_F(VarHeapLifetimeTest, ACommittedSpillIsReleasedByNothing) {
    // The other direction, and the one a too-eager release would break: a
    // committed value is live for as long as a version points at it, and
    // committing is not a death.
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, note varchar)").substr(0, 7), "CREATED");

    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "INSERT INTO t VALUES ('" + Spilling('c') + "')").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run(s, "COMMIT").substr(0, 6), "COMMIT");

    const ChainCensus after = Census("t");
    EXPECT_EQ(after.slots, 1u);
    EXPECT_EQ(after.live, 1u);
    EXPECT_NE(Run(s, "SELECT note FROM t").find(Spilling('c')), std::string::npos);
}

TEST_F(VarHeapLifetimeTest, ARolledBackInlineWriteReleasesNothing) {
    // The guard against a release that fires on rows it has no business
    // touching: a value that never spilled has no slot, so a rollback of it
    // must leave the chain exactly as it found it.
    Session s;
    ASSERT_EQ(Run(s, "CREATE TABLE t (id int64, note varchar)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run(s, "INSERT INTO t VALUES ('" + Spilling('a') + "')").substr(0, 8), "INSERTED");
    const ChainCensus before = Census("t");

    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "INSERT INTO t VALUES ('short')").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");

    const ChainCensus after = Census("t");
    EXPECT_EQ(after.slots, before.slots);
    EXPECT_EQ(after.live, before.live);
}

}  // namespace
}  // namespace kds::server
