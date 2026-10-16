#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "file_rig_crash.hpp"

#include "kds/base/current_core.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/anchor_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"

// BY-S1 (`instructions/v3.2.0/workorder-by-reindex-drop-index-reclaim.md`):
// an anchor slot can be removed (BY-R5), the mount's predicate finds the
// trees a relation owes (BY-R1), and a live index whose slot is gone is
// refused rather than read through its creation root (BY-R10).

namespace kds::catalog {
namespace {

// ---- The slot table (BY-R5) ------------------------------------------------

TEST(AnchorSlotRemoveTest, ARemovalCompactsTheTableAndFreesAPlaceAtTheCap) {
    std::array<std::byte, kPageSize> bytes{};
    std::span<std::byte, kPageSize> page(bytes);
    storage::FormatAnchorPage(page, /*owner_oid=*/4001, /*clustered_root=*/200);
    for (std::uint64_t oid = 1; oid <= storage::kAnchorMaxIndexEntries; ++oid) {
        ASSERT_TRUE(storage::SetAnchorIndexRoot(page, oid, static_cast<PageId>(1000 + oid)).ok());
    }
    EXPECT_EQ(storage::SetAnchorIndexRoot(page, 99999, 5).code(), StatusCode::kResourceExhausted);

    ASSERT_TRUE(storage::RemoveAnchorIndexSlot(page, 2).ok());
    auto slots = storage::AnchorIndexSlots(page);
    ASSERT_TRUE(slots.ok());
    ASSERT_EQ(slots.value().size(), storage::kAnchorMaxIndexEntries - 1);
    EXPECT_EQ(slots.value()[0].index_oid, 1u);
    EXPECT_EQ(slots.value()[1].index_oid, 3u) << "the table is compacted";
    EXPECT_EQ(slots.value()[1].root, 1003u);
    EXPECT_EQ(storage::AnchorIndexRoot(page, 2).value(), kInvalidPageId);

    EXPECT_TRUE(storage::SetAnchorIndexRoot(page, 99999, 5).ok()) << "the removed place is free";
    EXPECT_EQ(storage::RemoveAnchorIndexSlot(page, 2).code(), StatusCode::kNotFound);
    EXPECT_EQ(storage::AnchorClusteredRoot(page), 200u) << "the clustered root is untouched";
}

// ---- The owed predicate and BY-R10 ----------------------------------------

class IndexReclaimCatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
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

    std::string Run(const std::string& sql, server::Session* session = nullptr) {
        return dispatcher_->Dispatch(sql, session).response;
    }

    void Ok(const std::string& sql, server::Session* session = nullptr) {
        const std::string out = Run(sql, session);
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }

    Catalog& catalog() { return boot_->catalog; }

    Oid IndexOid(const std::string& name) {
        auto row = catalog().FindIndexByName(name);
        EXPECT_TRUE(row.ok()) << row.status().message();
        return row.ok() ? row.value().index_oid : 0;
    }

    std::vector<Oid> Owed() {
        auto owed = catalog().OwedIndexTrees();
        EXPECT_TRUE(owed.ok()) << owed.status().message();
        std::vector<Oid> out;
        if (owed.ok()) {
            for (const auto& tree : owed.value()) out.push_back(tree.index_oid);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    Oid TableOid(const std::string& table) {
        auto oid = catalog().FindTableOidByName(table);
        EXPECT_TRUE(oid.ok());
        return oid.ok() ? oid.value() : 0;
    }

    PageId Anchor(const std::string& table) {
        auto row = catalog().GetSysTableRow(TableOid(table));
        EXPECT_TRUE(row.ok());
        return row.value().anchor_page_id;
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<server::CommandDispatcher> dispatcher_;
};

TEST_F(IndexReclaimCatalogTest, ALiveIndexOwesNothingAndACommittedDropOwesItsTree) {
    Ok("CREATE TABLE t (id int64, a int64, b int64) BTREE");
    Ok("CREATE INDEX by_a ON t (a)");
    Ok("CREATE INDEX by_b ON t (b)");
    EXPECT_TRUE(Owed().empty());

    const Oid a = IndexOid("by_a");
    Ok("DROP INDEX by_a");
    EXPECT_EQ(Owed(), std::vector<Oid>{a});
}

TEST_F(IndexReclaimCatalogTest, ARolledBackDropOwesNothing) {
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX by_a ON t (a)");
    server::Session s;
    Ok("BEGIN", &s);
    Ok("DROP INDEX by_a", &s);
    Ok("ROLLBACK", &s);
    EXPECT_TRUE(Owed().empty());
}

TEST_F(IndexReclaimCatalogTest, ARolledBackCreateOwesItsTree) {
    // The tree was built and its slot written before the rollback, which
    // keeps the slot (§1): the tree is owed though no row ever committed.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("INSERT INTO t VALUES (1), (2)");
    server::Session s;
    Ok("BEGIN", &s);
    Ok("CREATE INDEX by_a ON t (a)", &s);
    const Oid a = IndexOid("by_a");
    EXPECT_TRUE(Owed().empty()) << "an open create's slot is its own, not owed";
    Ok("ROLLBACK", &s);
    EXPECT_EQ(Owed(), std::vector<Oid>{a});
}

TEST_F(IndexReclaimCatalogTest, ARemovedSlotIsOwedNoMore) {
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX by_a ON t (a)");
    const Oid a = IndexOid("by_a");
    Ok("DROP INDEX by_a");
    ASSERT_EQ(Owed(), std::vector<Oid>{a});
    ASSERT_TRUE(catalog().RemoveIndexSlot(TableOid("t"), Anchor("t"), a).ok());
    EXPECT_TRUE(Owed().empty());
    EXPECT_EQ(catalog().RemoveIndexSlot(TableOid("t"), Anchor("t"), a).code(), StatusCode::kNotFound);
}

TEST_F(IndexReclaimCatalogTest, ARemovedSlotsPlaceIsTakenByALaterCreate) {
    // The cap lifts through the SQL path, not only the storage call: a
    // removed slot's place is the next create's.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX by_a ON t (a)");
    const Oid a = IndexOid("by_a");
    Ok("DROP INDEX by_a");
    ASSERT_TRUE(catalog().RemoveIndexSlot(TableOid("t"), Anchor("t"), a).ok());
    Ok("CREATE INDEX by_a2 ON t (a)");
    Ok("INSERT INTO t VALUES (7)");
    EXPECT_NE(Run("SELECT id FROM t WHERE a = 7").find("1"), std::string::npos);
    EXPECT_TRUE(Owed().empty());
}

TEST_F(IndexReclaimCatalogTest, ADroppedRelationsSlotsAreItsTombstonesNotOwedTrees) {
    // BY-R3: a relation's drop reclaims every slot; listing them here too
    // would hand one tree to two jobs.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX by_a ON t (a)");
    Ok("DROP INDEX by_a");
    Ok("DROP TABLE t");
    EXPECT_TRUE(Owed().empty());
}

TEST_F(IndexReclaimCatalogTest, ALiveIndexWhoseSlotIsGoneIsCorruptionNotItsCreationRoot) {
    // BY-R10: the row's creation root may name a page a reclaim freed.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX by_a ON t (a)");
    ASSERT_TRUE(catalog().RemoveIndexSlot(TableOid("t"), Anchor("t"), IndexOid("by_a")).ok());
    catalog().InvalidateAfterCompensation();
    auto oid = catalog().FindTableOidByName("t");
    ASSERT_TRUE(oid.ok());
    auto access = catalog().InitTableAccess(oid.value());
    ASSERT_FALSE(access.ok());
    EXPECT_EQ(access.status().code(), StatusCode::kCorruption) << access.status().message();
}

}  // namespace
}  // namespace kds::catalog

namespace kds::server {
namespace {

// BY-R5 across a crash: the removal's record replays after every
// ANCHOR_UPDATE that named the slot, so the mounted anchor holds neither the
// removed slot nor an owed tree, and the live index keeps its own.
TEST(AnchorSlotRemoveCrashTest, AMountReplaysTheRemovalAndOwesNothing) {
    crash_rig::TempDir snap;
    catalog::Oid kept = 0;
    {
        auto rig = crash_rig::OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CurrentCoreGuard as(0);
        crash_rig::Ok(d0, "CREATE TABLE t (id int64, a int64, b int64) BTREE");
        crash_rig::Ok(d0, "CREATE INDEX by_a ON t (a)");
        crash_rig::Ok(d0, "CREATE INDEX by_b ON t (b)");
        catalog::Catalog& cat = rig->core(0).catalog();
        const catalog::Oid dropped = cat.FindIndexByName("by_a").value().index_oid;
        kept = cat.FindIndexByName("by_b").value().index_oid;
        crash_rig::Ok(d0, "DROP INDEX by_a");
        const PageId anchor =
            cat.GetSysTableRow(cat.FindTableOidByName("t").value()).value().anchor_page_id;
        ASSERT_TRUE(cat.RemoveIndexSlot(cat.FindTableOidByName("t").value(), anchor, dropped).ok());
        ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    }
    auto mounted = crash_rig::Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << mounted.status().message();
    catalog::Catalog& cat = mounted.value()->catalog();
    auto owed = cat.OwedIndexTrees();
    ASSERT_TRUE(owed.ok()) << owed.status().message();
    EXPECT_TRUE(owed.value().empty());
    const PageId anchor =
        cat.GetSysTableRow(cat.FindTableOidByName("t").value()).value().anchor_page_id;
    auto page = mounted.value()->store().GetForRead(anchor);
    ASSERT_TRUE(page.ok());
    auto slots = storage::AnchorIndexSlots(page.value().bytes());
    ASSERT_TRUE(slots.ok());
    ASSERT_EQ(slots.value().size(), 1u);
    EXPECT_EQ(slots.value()[0].index_oid, kept);
}

// BY-R1 at mount: a `CREATE INDEX` open at the crash is undone, and its slot
// - written with its tree before the row - is then owed.
TEST(AnchorSlotRemoveCrashTest, ACreateOpenAtTheCrashIsOwedAfterUndo) {
    crash_rig::TempDir snap;
    catalog::Oid open_index = 0;
    {
        auto rig = crash_rig::OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CurrentCoreGuard as(0);
        crash_rig::Ok(d0, "CREATE TABLE t (id int64, a int64) BTREE");
        crash_rig::Ok(d0, "INSERT INTO t VALUES (1), (2)");
        Session s;
        crash_rig::Ok(d0, "BEGIN", &s);
        crash_rig::Ok(d0, "CREATE INDEX by_a ON t (a)", &s);
        open_index = rig->core(0).catalog().FindIndexByName("by_a").value().index_oid;
        ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    }
    auto mounted = crash_rig::Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << mounted.status().message();
    auto owed = mounted.value()->catalog().OwedIndexTrees();
    ASSERT_TRUE(owed.ok()) << owed.status().message();
    ASSERT_EQ(owed.value().size(), 1u);
    EXPECT_EQ(owed.value()[0].index_oid, open_index);
}

}  // namespace
}  // namespace kds::server
