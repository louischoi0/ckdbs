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
    // The signals run under a manual clock parked at t=1: parked, decay is
    // a no-op and every earlier test reads exactly as it did under a null
    // clock; advanced, scores decay - which is what the lifecycle E2E
    // needs a workload shift to look like.
    Instance() : signals_(&clock_, kHalfLife) {
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
    CommandDispatcher& dispatcher() { return *dispatcher_; }
    catalog::Catalog& catalog() { return boot_->catalog; }
    storage::InMemoryPageStore& store() { return store_; }
    stats::CabinStore& cabins() { return *cabins_; }
    stats::OptimizerSignals& signals() { return signals_; }
    sched::ManualClock& clock() { return clock_; }
    txn::TransactionManager& txn() { return *txn_; }

private:
    storage::InMemoryPageStore store_{kFirstUserPageId};
    sched::ManualClock clock_{1};
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

// ---- PHY06: the view, the counters, the completion edges -----------------

TEST(CabinOptimizerExecTest, AnExtendReportsItsNewPageTotalToTheBudget) {
    Instance db;
    LoadBtree(db);
    const std::uint64_t cabin_id = MakeAutoCabin(db, "b");
    db.Run("SELECT * FROM b WHERE sym = 'aaa'");  // seed one value

    stats::CabinOptimizer controller;
    exec::CabinOptimizerExecutor executor(db.catalog(), db.store(), db.cabins(), controller);
    // A deliberately wrong CREATE-time count, so the edge is visible: the
    // report is the proxy's *total* (1 page for these sets), not a delta on
    // top of whatever the estimate said.
    controller.NoteCreated(db.catalog().FindTableOidByName("b").value(), 1, cabin_id, 7);
    ASSERT_EQ(controller.pages_committed(), 7u);

    ASSERT_TRUE(executor.Apply({ExtendAction(db, "b", cabin_id)}, kAlwaysOn).ok());
    EXPECT_EQ(controller.pages_committed(), 1u)
        << "the completed extend did not report the new total";
    EXPECT_EQ(executor.counters().extends, 1u);
}

TEST(CabinOptimizerExecTest, TheViewReportsTheLifecycleAndTheCountersCount) {
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE h (id int64, sym varchar)").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Run("INSERT INTO h VALUES ('aaa')").substr(0, 8), "INSERTED");

    // No view wired yet: the surface reports absence, never a zero-filled
    // table wearing a fresh face.
    EXPECT_EQ(db.Run("SHOW CABIN_OPTIMIZER").substr(0, 23), "CABIN_OPTIMIZER absent ");

    stats::CabinOptimizerConfig config;
    config.p_cabin_pages = 0;
    config.confirm_snapshots = 2;
    stats::CabinOptimizer controller(config);
    exec::CabinOptimizerExecutor executor(db.catalog(), db.store(), db.cabins(), controller);
    db.dispatcher().set_cabin_optimizer_view(&controller, &executor);
    ASSERT_EQ(db.Run("SET CABIN_OPTIMIZER on").substr(0, 2), "OK");

    // Two ticks: the streak confirms on the second and the CREATE lands,
    // and the view is read *at* that tick - a third tick would already
    // have decided the next action (an EXTEND, from the misses the
    // pre-observation probes left) and the newest log record would
    // truthfully name it instead.
    const std::string probe = "SELECT * FROM h WHERE sym = 'aaa'";
    for (int tick = 0; tick < 2; ++tick) {
        for (int i = 0; i < 5; ++i) db.Run(probe);
        ASSERT_TRUE(executor.Tick(db.signals(), kAlwaysOn).ok());
    }

    // The applied-action counters: two enabled ticks, one CREATE landed.
    EXPECT_EQ(executor.counters().ticks, 2u);
    EXPECT_EQ(executor.counters().creates, 1u);
    EXPECT_EQ(executor.counters().drops, 0u);

    // The view: header with the budget line, one managed entry in ACTIVE,
    // named by relation and column, carrying its last logged action.
    const std::string view = db.Run("SHOW CABIN_OPTIMIZER");
    EXPECT_EQ(view.substr(0, 19), "cabin_optimizer=on ") << view;
    EXPECT_NE(view.find("managed=1"), std::string::npos) << view;
    EXPECT_NE(view.find("page_budget=1024"), std::string::npos) << view;
    EXPECT_NE(view.find("rel=h column=sym state=ACTIVE"), std::string::npos) << view;
    EXPECT_NE(view.find("last_action=CREATE reason=sustained-benefit"), std::string::npos)
        << view;

    // ANALYZE marks the optimizer-managed probe (PO9): the operator can
    // see the serving structure is one the engine may drop on its own.
    const std::string analyzed = db.Run("ANALYZE " + probe);
    EXPECT_NE(analyzed.find("cabin_optimizer=true"), std::string::npos) << analyzed;
}

// PHY08's E2E: the whole lifecycle in one scripted run, observed through
// the view at every stage - hot traffic earns a CREATE, the Cabin serves,
// the workload shifts and the entry DECAYs, the cooldown elapses and the
// DROP retires everything, and the reply never moves at any point. The
// advisory family's standing assertion, run across an entire lifetime.
TEST(CabinOptimizerExecTest, TheFullLifecycleObservedThroughTheView) {
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE h (id int64, sym varchar)").substr(0, 7), "CREATED");
    const char* kSyms[] = {"aaa", "aaa", "bbb", "ccc"};
    for (const char* sym : kSyms) {
        ASSERT_EQ(db.Run(std::string("INSERT INTO h VALUES ('") + sym + "')").substr(0, 8),
                  "INSERTED");
    }
    const std::string probe = "SELECT * FROM h WHERE sym = 'aaa'";
    const std::string baseline = db.Run(probe);

    stats::CabinOptimizerConfig config;
    config.p_cabin_pages = 0;
    config.confirm_snapshots = 2;
    // T_amort and the cooldown at the neutral unit so the lifecycle runs
    // in test time: the scripted 50-half-life silences map to DECAYING
    // and DROP here, where the shipped 64/128 exist to stretch exactly
    // these gaps out to overnight scale (their sizing is
    // cabin_optimizer_test's own case).
    config.amort_windows = stats::kFixOne;
    config.cooldown_half_lives = 2;
    stats::CabinOptimizer controller(config);
    exec::CabinOptimizerExecutor executor(db.catalog(), db.store(), db.cabins(), controller);
    db.dispatcher().set_cabin_optimizer_view(&controller, &executor);
    ASSERT_EQ(db.Run("SET CABIN_OPTIMIZER on").substr(0, 2), "OK");

    // Phase 1 - hot: sustained traffic confirms twice and the CREATE lands.
    for (int tick = 0; tick < 2; ++tick) {
        for (int i = 0; i < 5; ++i) db.Run(probe);
        ASSERT_TRUE(executor.Tick(db.signals(), kAlwaysOn).ok());
    }
    ASSERT_EQ(executor.counters().creates, 1u);
    std::string view = db.Run("SHOW CABIN_OPTIMIZER");
    EXPECT_NE(view.find("state=ACTIVE"), std::string::npos) << view;
    EXPECT_NE(view.find("last_action=CREATE reason=sustained-benefit"), std::string::npos)
        << view;

    // Phase 2 - the Cabin serves: two probes observe the value (auto is
    // n=2), the next is a served hit on a marked-managed probe, and the
    // reply is byte-identical to the pre-optimizer baseline.
    db.Run(probe);
    db.Run(probe);
    const std::string analyzed = db.Run("ANALYZE " + probe);
    EXPECT_NE(analyzed.find("cabin_hits=1"), std::string::npos) << analyzed;
    EXPECT_NE(analyzed.find("cabin_optimizer=true"), std::string::npos) << analyzed;
    EXPECT_EQ(db.Run(probe), baseline);

    // Phase 3 - the workload shifts: 50 half-lives of silence zero every
    // score, and the next tick moves the entry to DECAYING.
    db.clock().Advance(50 * kHalfLife);
    ASSERT_TRUE(executor.Tick(db.signals(), kAlwaysOn).ok());
    view = db.Run("SHOW CABIN_OPTIMIZER");
    EXPECT_NE(view.find("state=DECAYING"), std::string::npos) << view;

    // Phase 4 - the cooldown elapses and the DROP retires the catalog row,
    // the entry sets and the controller entry.
    db.clock().Advance(50 * kHalfLife);
    ASSERT_TRUE(executor.Tick(db.signals(), kAlwaysOn).ok());
    EXPECT_EQ(executor.counters().drops, 1u);
    view = db.Run("SHOW CABIN_OPTIMIZER");
    EXPECT_NE(view.find("managed=0"), std::string::npos) << view;

    // No residue: the catalog is clean, the probe compiles back to the
    // walk it started as, and the reply still never moved.
    EXPECT_EQ(db.Run("SHOW CABINS").substr(0, 8), "cabins=0");
    EXPECT_EQ(db.Run("ANALYZE " + probe).find(" cabin="), std::string::npos);
    EXPECT_EQ(db.Run(probe), baseline);
}

TEST(CabinOptimizerExecTest, ADeclaredCabinIsNeverMarkedAsManaged) {
    Instance db;
    LoadBtree(db);
    ASSERT_EQ(db.Run("CREATE CABIN ON b(sym)").substr(0, 7), "CREATED");
    const std::string analyzed = db.Run("ANALYZE SELECT * FROM b WHERE sym = 'aaa'");
    ASSERT_NE(analyzed.find(" cabin="), std::string::npos) << analyzed;
    EXPECT_EQ(analyzed.find("cabin_optimizer=true"), std::string::npos)
        << "a user-declared Cabin wore the optimizer's tag: " << analyzed;
}

}  // namespace
}  // namespace kds::server
