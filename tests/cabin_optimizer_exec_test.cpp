#include "kds/exec/cabin_optimizer_exec.hpp"

#include "kds/exec/tuple_verify.hpp"

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/cabin_optimizer.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/optimizer_signals.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// PHY04 - the cabin optimizer's executor. The acceptance: each action end
// to end on small relations, a mid-build interruption discarding cleanly,
// and the kill switch harmless in both directions. The build's busy-row
// deferral - the completeness argument inherited from AST06 - gets its own
// case, because it is the one rule whose violation would be invisible in a
// reply and fatal to the superset invariant.

namespace kds::server {
namespace {

constexpr sched::MonoTimeNs kHalfLife = 600'000'000'000ULL;

class Instance {
public:
    Instance() : signals_(/*clock=*/nullptr, kHalfLife) {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        EXPECT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        cabins_.emplace();
        cabins_->set_signals(&signals_);
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        txn_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, &*cabins_, &*txn_);
        dispatcher_->set_optimizer_signals(&signals_);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    catalog::Catalog& catalog() { return boot_->catalog; }
    storage::InMemoryPageStore& store() { return store_; }
    stats::CabinStore& cabins() { return *cabins_; }
    stats::OptimizerSignals& signals() { return signals_; }
    txn::TransactionManager& txn() { return *txn_; }

private:
    storage::InMemoryPageStore store_{kFirstUserPageId};
    stats::OptimizerSignals signals_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> txn_;
    std::optional<CommandDispatcher> dispatcher_;
};

const std::function<bool()> kAlwaysOn = [] { return true; };

void LoadBtree(Instance& db) {
    ASSERT_EQ(db.Run("CREATE TABLE b (id int64, sym varchar, qty int64) BTREE").substr(0, 7),
              "CREATED");
    const char* kSyms[] = {"aaa", "bbb", "aaa", "ccc"};
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(db.Run(std::string("INSERT INTO b VALUES ('") + kSyms[i] + "', " +
                         std::to_string((i + 1) * 10) + ")")
                      .substr(0, 8),
                  "INSERTED");
    }
}

// Creates an optimizer-owned Cabin directly - the CREATE action's catalog
// half, isolated - and returns its id.
std::uint64_t MakeAutoCabin(Instance& db, const char* table) {
    auto oid = db.catalog().FindTableOidByName(table);
    EXPECT_TRUE(oid.ok());
    auto created = db.catalog().CreateCabin(oid.value(), /*col_pos=*/1,
                                            catalog::kCabinOriginAuto);
    EXPECT_TRUE(created.ok()) << created.status().message();
    return created.value();
}

stats::ActionItem ExtendAction(Instance& db, const char* table, std::uint64_t cabin_id) {
    stats::ActionItem action;
    action.action = stats::CabinAction::kExtend;
    action.reason = stats::ActionReason::kCoverageExpansion;
    action.cabin_id = cabin_id;
    action.rel_oid = db.catalog().FindTableOidByName(table).value();
    action.col_pos = 1;
    return action;
}

TEST(CabinOptimizerExecTest, TheFullLoopCreatesACabinFromHotTraffic) {
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE h (id int64, sym varchar)").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Run("INSERT INTO h VALUES ('aaa')").substr(0, 8), "INSERTED");

    // A tight config so a one-page relation can clear the bar: the probe
    // cost is zeroed and two confirmations suffice. Nothing depends on the
    // numbers - the loop is what is under test.
    stats::CabinOptimizerConfig config;
    config.p_cabin_pages = 0;
    config.confirm_snapshots = 2;
    stats::CabinOptimizer controller(config);
    exec::CabinOptimizerExecutor executor(db.catalog(), db.store(), db.cabins(), controller);

    // Hot filter-scan traffic: sym has no Cabin, so the shape compiles to
    // kFilterScan and arrives as a candidate. Two ticks of sustained
    // evidence and the CREATE lands - snapshot, Decide, Apply, catalog.
    const std::string probe = "SELECT * FROM h WHERE sym = 'aaa'";
    for (int tick = 0; tick < 3; ++tick) {
        for (int i = 0; i < 5; ++i) db.Run(probe);
        ASSERT_TRUE(executor.Tick(db.signals(), kAlwaysOn).ok());
    }

    auto rows = db.catalog().ListCabins();
    ASSERT_TRUE(rows.ok());
    bool found = false;
    for (const catalog::SysCabinRow& row : rows.value()) {
        if (row.column_no == 1 && row.origin == catalog::kCabinOriginAuto) found = true;
    }
    EXPECT_TRUE(found) << "hot traffic never became an optimizer-owned Cabin";
    EXPECT_GE(controller.pages_committed(), 1u);
}

TEST(CabinOptimizerExecTest, ExtendBuildsEverySeededSetInOneCompleteWalk) {
    Instance db;
    LoadBtree(db);
    const std::uint64_t cabin_id = MakeAutoCabin(db, "b");

    // Sight two values once each - below the auto n=2 record threshold, so
    // both are seeded-but-unobserved. 'zzz' matches nothing, which is the
    // case worth having: an observed empty set is the authoritative
    // zero-rows answer.
    db.Run("SELECT * FROM b WHERE sym = 'aaa'");
    db.Run("SELECT * FROM b WHERE sym = 'zzz'");
    ASSERT_EQ(db.cabins().SightedUnobservedOf(cabin_id).size(), 2u);

    stats::CabinOptimizer controller;
    exec::CabinOptimizerExecutor executor(db.catalog(), db.store(), db.cabins(), controller);
    ASSERT_TRUE(executor.Apply({ExtendAction(db, "b", cabin_id)}, kAlwaysOn).ok());

    auto aaa = stats::MakeCabinKey(cabin_id, [] {
        parser::AstValue v;
        v.type = parser::ValueType::kStr;
        v.str_val = "aaa";
        return v;
    }());
    auto zzz = stats::MakeCabinKey(cabin_id, [] {
        parser::AstValue v;
        v.type = parser::ValueType::kStr;
        v.str_val = "zzz";
        return v;
    }());
    ASSERT_TRUE(aaa.has_value() && zzz.has_value());

    std::vector<stats::CabinEntry>* aaa_set = db.cabins().Find(*aaa);
    ASSERT_NE(aaa_set, nullptr) << "the seeded value was not observed";
    EXPECT_EQ(aaa_set->size(), 2u) << "'aaa' has exactly two rows";
    std::vector<stats::CabinEntry>* zzz_set = db.cabins().Find(*zzz);
    ASSERT_NE(zzz_set, nullptr) << "the empty value was not observed";
    EXPECT_TRUE(zzz_set->empty()) << "an observed no-rows value is an *empty* set";

    // And the served path agrees: the next probe is a Cabin hit.
    const std::string analyzed = db.Run("ANALYZE SELECT * FROM b WHERE sym = 'aaa'");
    EXPECT_NE(analyzed.find("cabin_hits=1"), std::string::npos) << analyzed;
}

TEST(CabinOptimizerExecTest, TheKillSwitchMidBuildDiscardsCleanly) {
    Instance db;
    LoadBtree(db);
    const std::uint64_t cabin_id = MakeAutoCabin(db, "b");
    db.Run("SELECT * FROM b WHERE sym = 'aaa'");  // seed one value

    // On for the action boundary, off by the first page boundary: the
    // build aborts mid-walk and commits nothing.
    int calls = 0;
    const std::function<bool()> off_mid_build = [&] { return ++calls <= 1; };
    stats::CabinOptimizer controller;
    exec::CabinOptimizerExecutor executor(db.catalog(), db.store(), db.cabins(), controller);
    ASSERT_TRUE(executor.Apply({ExtendAction(db, "b", cabin_id)}, off_mid_build).ok());
    EXPECT_EQ(db.cabins().ObservedValuesOf(cabin_id).size(), 0u)
        << "an interrupted build committed a partial walk";

    // The evidence survived the discard: switched back on, the same
    // action completes.
    ASSERT_TRUE(executor.Apply({ExtendAction(db, "b", cabin_id)}, kAlwaysOn).ok());
    EXPECT_EQ(db.cabins().ObservedValuesOf(cabin_id).size(), 1u);
}

TEST(CabinOptimizerExecTest, ABusyRowDefersTheBuildUntilItSettles) {
    Instance db;
    LoadBtree(db);
    const std::uint64_t cabin_id = MakeAutoCabin(db, "b");
    db.Run("SELECT * FROM b WHERE sym = 'aaa'");  // seed

    // An in-flight row: counted, its abort leaves a phantom; skipped, its
    // commit was already missed by the write hook. The build must defer.
    ASSERT_EQ(db.Run("BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(db.Run("INSERT INTO b VALUES ('aaa', 99)").substr(0, 8), "INSERTED");

    stats::CabinOptimizer controller;
    exec::CabinOptimizerExecutor executor(db.catalog(), db.store(), db.cabins(), controller,
                                          &db.txn());
    ASSERT_TRUE(executor.Apply({ExtendAction(db, "b", cabin_id)}, kAlwaysOn).ok());
    EXPECT_TRUE(db.cabins().ObservedValuesOf(cabin_id).empty())
        << "the build observed a set while a writer was in flight";

    ASSERT_EQ(db.Run("COMMIT").substr(0, 6), "COMMIT");
    ASSERT_TRUE(executor.Apply({ExtendAction(db, "b", cabin_id)}, kAlwaysOn).ok());
    auto key = stats::MakeCabinKey(cabin_id, [] {
        parser::AstValue v;
        v.type = parser::ValueType::kStr;
        v.str_val = "aaa";
        return v;
    }());
    std::vector<stats::CabinEntry>* set = db.cabins().Find(*key);
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->size(), 3u) << "the settled build must see the committed third row";
}

TEST(CabinOptimizerExecTest, HealRepairsBrokenHintsAndErasesDanglingPks) {
    Instance db;
    LoadBtree(db);
    const std::uint64_t cabin_id = MakeAutoCabin(db, "b");
    db.Run("SELECT * FROM b WHERE sym = 'aaa'");
    stats::CabinOptimizer controller;
    exec::CabinOptimizerExecutor executor(db.catalog(), db.store(), db.cabins(), controller);
    ASSERT_TRUE(executor.Apply({ExtendAction(db, "b", cabin_id)}, kAlwaysOn).ok());

    auto key = stats::MakeCabinKey(cabin_id, [] {
        parser::AstValue v;
        v.type = parser::ValueType::kStr;
        v.str_val = "aaa";
        return v;
    }());
    std::vector<stats::CabinEntry>* set = db.cabins().Find(*key);
    ASSERT_NE(set, nullptr);
    ASSERT_EQ(set->size(), 2u);

    // Break every hint, and plant a dangling pk with a plausible hint.
    for (stats::CabinEntry& entry : *set) {
        entry.slot = static_cast<std::uint16_t>(entry.slot + 3);
    }
    stats::CabinEntry dangling = (*set)[0];
    dangling.pk = 999'999;
    set->push_back(dangling);

    stats::ActionItem heal = ExtendAction(db, "b", cabin_id);
    heal.action = stats::CabinAction::kHeal;
    heal.reason = stats::ActionReason::kQualityHeal;
    ASSERT_TRUE(executor.Apply({heal}, kAlwaysOn).ok());

    set = db.cabins().Find(*key);
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->size(), 2u) << "the dangling pk was not erased";
    for (const stats::CabinEntry& entry : *set) {
        exec::VerifiedTuple verified = exec::VerifyTupleAt(db.store(), entry.page_id,
                                                           entry.slot, entry.pk,
                                                           entry.page_epoch);
        EXPECT_TRUE(verified.ok()) << "a healed hint still fails verification";
    }
}

TEST(CabinOptimizerExecTest, DropRemovesTheRowTheSetsAndTheControllerEntry) {
    Instance db;
    LoadBtree(db);
    const std::uint64_t cabin_id = MakeAutoCabin(db, "b");
    db.Run("SELECT * FROM b WHERE sym = 'aaa'");
    stats::CabinOptimizer controller;
    exec::CabinOptimizerExecutor executor(db.catalog(), db.store(), db.cabins(), controller);
    ASSERT_TRUE(executor.Apply({ExtendAction(db, "b", cabin_id)}, kAlwaysOn).ok());
    controller.NoteCreated(db.catalog().FindTableOidByName("b").value(), 1, cabin_id, 3);
    ASSERT_EQ(controller.pages_committed(), 3u);

    stats::ActionItem drop = ExtendAction(db, "b", cabin_id);
    drop.action = stats::CabinAction::kDrop;
    drop.reason = stats::ActionReason::kSustainedDecay;
    ASSERT_TRUE(executor.Apply({drop}, kAlwaysOn).ok());

    auto rows = db.catalog().ListCabins();
    ASSERT_TRUE(rows.ok());
    for (const catalog::SysCabinRow& row : rows.value()) {
        EXPECT_NE(row.cabin_id, cabin_id) << "the catalog row survived the drop";
    }
    EXPECT_TRUE(db.cabins().ObservedValuesOf(cabin_id).empty());
    EXPECT_EQ(controller.pages_committed(), 0u);
}

}  // namespace
}  // namespace kds::server
