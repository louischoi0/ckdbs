#include <gtest/gtest.h>

#include "column_cycle.hpp"
#include "learn_plan_stats.hpp"
#include "unordered_reply.hpp"

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/trail_collector.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/stats/trail_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/page_header.hpp"
#include "catalog_chain_pages.hpp"

// **The advisory contract** (docs/spec/waystone-concpets.md §11-3 and §11-4,
// workplan P12). The spec calls §11-3 "the test that must never be allowed
// to fail", and this is it.
//
// Invariant 8: deleting every waystone may cost performance and must never
// change a result. That is not a quality bar, it is the property the whole
// structure is permitted to exist under - Waystone is allowed to be
// approximate, stale, collision-prone and lossy *precisely because* none of
// it can reach an answer. A single case where a trail changes a row makes
// every other design decision in the subsystem unsound.
//
// So the same query set runs over the same data in five configurations and
// every reply is compared **byte for byte**:
//
//   1. recording on, replay on      the real one
//   2. recording off                no trails exist at all - the baseline
//   3. replay off                   trails exist and are ignored
//   4. waystones deleted mid-run    a trail vanishes between executions
//   5. a deliberately corrupted trail   entries naming valid pages and
//                                       slots holding *different* tuples
//
// Case 5 is the one that proves §2 rule 1's identity check is load-bearing
// rather than decorative: without it the engine returns real rows from the
// wrong places, and every other case in this file would still pass.
//
// **Opted against not opted** (CF-S1c, `workorder-cf-waystone-premise.md`
// §8.5): since CF-S1b only an opted shape records or replays, so the
// sharpest reference is the same instance under the same ceilings with
// nothing opted - every reply byte for byte, under each ceiling, with its
// trails deleted mid-run, with a corrupted trail, and with the bit flipped
// mid-run. **Every recordable opted cell asserts that it replayed**
// (`EveryRecordableOptedQueryReplays`), so a fixture that opted nothing, or
// a gate that let nothing through, cannot pass any of these vacuously.

namespace kds::server {
namespace {

// One database, one dispatcher, one configuration.
class Instance {
public:
    Instance(bool record, bool replay, bool scan_kernel = true) {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        EXPECT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        recorder_.emplace(boot_->catalog, store_);
        exec::Budget budget;
        budget.set_scan_kernel(scan_kernel);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            budget, record ? &*recorder_ : nullptr, replay);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    // `WAYSTONE ON` (CF-S1b): nothing records until a shape is opted in.
    // Answers whether it was accepted; a shape with no tuple key equality
    // is refused, and stays a shape that never records.
    bool OptIn(const std::string& sql) {
        return Run("WAYSTONE ON " + sql).rfind("OK WAYSTONE ON", 0) == 0;
    }
    void KeepWrittenOrder() { dispatcher_->set_plan_join_order(false); }
    // The join planner's statistics, learned from `sqls` (CC-S3).
    void Learn(const std::vector<std::string>& sqls) {
        testing_plan::LearnPlanStats(*dispatcher_, plan_stats_, boot_->catalog, store_, sqls);
    }

    catalog::Catalog& catalog() { return boot_->catalog; }
    storage::InMemoryPageStore& store() { return store_; }

private:
    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::TrailRecorder> recorder_;
    std::optional<CommandDispatcher> dispatcher_;
    stats::PlanStats plan_stats_{1};
};

const std::vector<std::string>& Queries();

// The dataset every configuration runs over. A btree whose `id` is its tuple
// key, so the descent path replay replaces is covered, and a heap (BZ-R1)
// whose `id` is an ordinary column holding the same numbers, so the same
// statement text over it is a search replay must never serve.
void Load(Instance& db, testing_cycle::Added added = testing_cycle::Added::kNone,
          bool opt = true) {
    const std::string spare = testing_cycle::Spare(added);
    ASSERT_EQ(db.Run("CREATE TABLE b (id int64, v int64, label varchar)" + spare + " BTREE")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(db.Run("CREATE TABLE h (id int64, v int64, label varchar)" + spare + " HEAP")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(db.Run("CREATE TABLE j (id int64, w int64) BTREE").substr(0, 7), "CREATED");
    // Declared before the rows, so the write hook fills it rather than the
    // backfill - which is the shape ordinary traffic produces. An index is
    // catalog state, so every configuration below gets it and a difference
    // between them would mean the index changed an answer.
    ASSERT_EQ(db.Run("CREATE INDEX b_by_v ON b (v)").substr(0, 7), "CREATED");
    for (int i = 1; i <= 8; ++i) {
        const std::string v = std::to_string(i * 10);
        const std::string label = "'row" + std::to_string(i) + "'";
        ASSERT_EQ(db.Run("INSERT INTO b VALUES (" + v + ", " + label + ")").substr(0, 8),
                  "INSERTED");
        ASSERT_EQ(db.Run("INSERT INTO h VALUES (" + std::to_string(i) + ", " + v + ", " + label +
                         ")")
                      .substr(0, 8),
                  "INSERTED");
        ASSERT_EQ(db.Run("INSERT INTO j VALUES (" + std::to_string(i * 100) + ")").substr(0, 8),
                  "INSERTED");
    }
    // CD-S4: the rows' last column through a vacant slot or a cleaned one.
    const auto run = [&db](const std::string& sql) { return db.Run(sql); };
    testing_cycle::AddColumn(run, "b", added);
    testing_cycle::AddColumn(run, "h", added);
    // **Every query the set can opt in, opted** (CF-S1b), in every
    // configuration: a recording arm with nothing opted would record
    // nothing and compare vacuously. A configuration with no recorder holds
    // the bits and no directory, which changes no reply either. The scans,
    // the heap's `id` (an ordinary column) and the built join are refused -
    // no tuple key equality - and never record. `opt = false` is the
    // not-opted reference (CF-S1c).
    if (opt) {
        for (const std::string& sql : Queries()) db.OptIn(sql);
    }
}

// The query set. Deliberately mixed: keyed steps that replay, searches that
// must not, a join whose trail spans relations, and queries that match
// nothing - a trail cannot help those, and it must not hurt them either.
const std::vector<std::string>& Queries() {
    static const std::vector<std::string> kQueries = {
        "SELECT * FROM b WHERE id = 3",
        "SELECT * FROM b WHERE id = 7",
        "SELECT * FROM b WHERE id = 99",
        "SELECT * FROM h WHERE id = 3",
        "SELECT * FROM h WHERE id = 8",
        "SELECT * FROM b WHERE v = 50",
        "SELECT * FROM b",
        "SELECT id, label FROM b WHERE id = 2",
        "SELECT a.label, c.w FROM b AS a JOIN j AS c ON a.id = c.id WHERE a.id = 4",
        "SELECT a.label, c.w FROM h AS a JOIN j AS c ON a.id = c.id WHERE a.id = 6",

        // ---- The walked join (docs/spec/join-inner-build.md, JB7) ------
        //
        // The inner is `h` keyed on `h.v`: not a tuple key, no index (only
        // `b(v)` has one), no Cabin - so every arm of the structure
        // ladder declines and the step builds a statement-local map,
        // then serves seven of the eight outer rows from it.
        //
        // It belongs in *this* set for the aggregates' reason one step
        // further on: the build is a **search-class** structure, so "can
        // a trail change a built join's reply?" is answered by the five
        // configurations already compared here rather than by a suite of
        // its own - including the corrupted and deleted trails, under
        // which this reply must not move either.
        //
        // **What it does not pin, stated so nobody quotes it for that.**
        // The converse - "a build feeds no trail" (spec §3's hard rule) -
        // is *not* tested by these comparisons: recording is gated on
        // `IsTrailReplayable(step.kind)` and consultation happens only in
        // `RunPointStep`, so entries wrongly recorded for this kScan step
        // would be written and never read, and every reply here would
        // stay byte-identical. Pinning that needs the trail itself
        // inspected - no pattern row exists for a statement that recorded
        // nothing - not a reply compared.
        "SELECT a.label, d.label FROM b AS a JOIN h AS d ON d.v = a.v",

        // ---- Aggregated statements (docs/spec/aggregate.md §9 item 8) --
        //
        // They belong in *this* set rather than in a suite of their own,
        // and the reason is AG1. The fold consumes rows and has no opinion
        // about where they came from, so an aggregated statement's
        // exposure to a trail is exactly its chain's - which means the
        // question "can a trail change an aggregated answer?" is answered
        // by the five configurations already compared here, at the cost of
        // adding lines to a list.
        //
        // A fold is also a *sharper* probe than a projection for the
        // corrupted-trail case below. A projection prints whatever row the
        // trail pointed at, so a wrong row shows up as wrong text; a
        // COUNT collapses every row to one number, and a trail that
        // silently produced a row twice or dropped one moves that number
        // while every column that could have shown it is gone.
        "SELECT COUNT(*) FROM b WHERE id = 3",
        "SELECT COUNT(*) FROM b WHERE id = 99",
        "SELECT COUNT(*), MIN(v), MAX(v) FROM h WHERE id = 5",
        "SELECT v, COUNT(*) FROM b GROUP BY v",
        "SELECT COUNT(DISTINCT label) FROM h",
        "SELECT COUNT(*), SUM(c.w) FROM b AS a JOIN j AS c ON a.id = c.id WHERE a.id = 4",
        "SELECT a.label, COUNT(*) FROM h AS a JOIN j AS c ON a.id = c.id GROUP BY a.label",

        // ---- Indexed statements (docs/spec/index.md §8) ----------------
        //
        // Here for the aggregates' reason, one step further along: an index
        // probe is search-class, so a trail may prefetch for it and must
        // never replace it. That claim is exactly what these five
        // configurations test, and adding lines to a list is cheaper than a
        // second suite that would have to re-derive them.
        //
        // `b.v` carries an index in every configuration below - the index
        // is catalog state, so it is created for the reference instance
        // too, and a difference between them would mean the *index* changed
        // an answer.
        "SELECT * FROM b WHERE v = 50",
        "SELECT * FROM b WHERE v = 999",
        "SELECT id FROM b WHERE v = 50",
        "SELECT COUNT(*) FROM b WHERE v = 50",
        "SELECT a.label FROM b AS a JOIN j AS c ON a.id = c.id WHERE a.v = 50",

        // ---- Paginated statements (docs/spec/parser-v2.md I11, V09) ---------
        //
        // Here for the aggregates' reason a third time: the emission
        // quota is a sink decorator, so a limited statement's exposure to
        // a trail is exactly its chain's, and the five configurations
        // already compared here answer "can a trail change a limited
        // reply?" at the cost of lines in a list.
        //
        // The slice cases are the sharp ones. A trail that changed the
        // *order* rows arrive in would move which rows survive the quota
        // - an unlimited statement shows that as the same set reordered,
        // which invariant 8's byte-for-byte comparison already refuses,
        // but a sliced statement shows it as *different rows*, which is
        // the louder failure and the one a client would file.
        "SELECT * FROM b WHERE id = 3 LIMIT 1",
        "SELECT * FROM b LIMIT 4",
        "SELECT * FROM b LIMIT 3 OFFSET 2",
        "SELECT * FROM h OFFSET 5",
        "SELECT * FROM b ORDER BY id ASC LIMIT 2",
        // OB4's half of the same question, and a sharper one. The sort is
        // a sink decorator downstream of trail recording, so a trail can
        // reorder *execution* freely while the reply stays fixed - these
        // cases are the ones that would catch a sort that had leaked into
        // the VM, and the DESC and multi-key forms are the ones with no
        // walk order to fall back on if it had.
        "SELECT * FROM b ORDER BY v DESC",
        "SELECT * FROM b ORDER BY v DESC LIMIT 3 OFFSET 1",
        "SELECT * FROM b ORDER BY label, id DESC",
        "SELECT * FROM h ORDER BY v",
        "SELECT * FROM b WHERE v = 50 LIMIT 1",
        "SELECT * FROM b WHERE id = 99 LIMIT 1",
        "SELECT label FROM b LIMIT 0",
        "SELECT a.label, c.w FROM b AS a JOIN j AS c ON a.id = c.id WHERE a.id = 4 LIMIT 1",

        // ---- One fetch, several select lists (AP-S2) -------------------
        //
        // A trail is keyed by `fetch_id`, so every statement below shares
        // a trail with one above - `SELECT * FROM b WHERE id = 3` and its
        // COUNT, the heap twin, and the join - and each is served from a
        // trail another select list recorded. That is the claim AP-S2
        // rests on (the order's §1.5: the chain does not read the
        // projection), and these five configurations are where it is
        // tested, including under the corrupted and deleted trails.
        "SELECT label FROM b WHERE id = 3",
        "SELECT v, id FROM b WHERE id = 3",
        "SELECT label FROM h WHERE id = 3",
        "SELECT MAX(v) FROM h WHERE id = 3",
        "SELECT c.w FROM b AS a JOIN j AS c ON a.id = c.id WHERE a.id = 4",
        "SELECT COUNT(*) FROM h AS a JOIN j AS c ON a.id = c.id WHERE a.id = 6",
    };
    return kQueries;
}

// Runs the set `rounds` times and returns every reply in order. Repeated
// because the interesting window is not the first execution - it is the one
// *after* a trail exists.
std::vector<std::string> RunAll(Instance& db, int rounds) {
    std::vector<std::string> out;
    for (int r = 0; r < rounds; ++r) {
        for (const std::string& sql : Queries()) out.push_back(db.Run(sql));
    }
    return out;
}

// The reference: a database that never recorded anything, so no trail can
// possibly be involved in any answer it gives.
std::vector<std::string> Reference() {
    Instance db(/*record=*/false, /*replay=*/false);
    Load(db);
    return RunAll(db, 3);
}

// The not-opted reference (CF-S1c): the same ceilings, nothing opted, so the
// gate lets no statement near a trail.
std::vector<std::string> NotOpted(bool record, bool replay, int rounds) {
    Instance db(record, replay);
    Load(db, testing_cycle::Added::kNone, /*opt=*/false);
    return RunAll(db, rounds);
}

// Whether `sql`'s shape is opted on `db`: its current row carries the bit.
bool IsOpted(Instance& db, const std::string& sql) {
    auto fp = parser::FingerprintOf(sql);
    if (!fp.has_value()) return false;
    auto row = db.catalog().GetSysPatternRow(fp->fetch_id);
    return row.ok() && (row.value().flags & catalog::kPatternWaystone) != 0;
}

void ExpectSame(const std::vector<std::string>& got, const std::vector<std::string>& want,
                const char* what) {
    ASSERT_EQ(got.size(), want.size()) << what;
    for (std::size_t i = 0; i < want.size(); ++i) {
        const std::size_t query = i % Queries().size();
        EXPECT_EQ(got[i], want[i])
            << what << " diverged at reply " << i << " (" << Queries()[query] << ")";
    }
}

// What a relayout mover will do to every page it touches, done by hand
// because no mover exists (docs/spec/physical-optimizer.md §6). Sweeps the
// user id range; absent ids just miss, and a catalog chain's growth page,
// which shares the range, is passed by: a mover reorganizes user relations.
std::size_t BumpEveryUserPage(Instance& db) {
    auto catalog_pages = testing_catalog::EveryCatalogPage(db.store());
    EXPECT_TRUE(catalog_pages.ok()) << catalog_pages.status().message();
    if (!catalog_pages.ok()) return 0;
    std::size_t bumped = 0;
    const PageId end = static_cast<PageId>(kFirstUserPageId + db.store().page_count());
    for (PageId id = kFirstUserPageId; id < end; ++id) {
        if (catalog_pages.value().count(id) != 0) continue;
        auto page = db.store().Get(id);
        if (!page.ok()) continue;
        storage::BumpRelayoutEpoch(page.value().bytes());
        ++bumped;
    }
    return bumped;
}

// ---- §11-3, the five configurations --------------------------------------

TEST(WaystoneContractTest, RecordingAndReplayingChangesNoReply) {
    Instance db(/*record=*/true, /*replay=*/true);
    Load(db);
    ExpectSame(RunAll(db, 3), Reference(), "recording+replay");
}

// CD-S4: recording and replaying over relations whose last column took a
// cleaned dropped slot answer what a column through a vacant slot answers
// with no trail at all - `SELECT *` included, which prints the column.
TEST(WaystoneContractTest, RecordingAndReplayingOverAReusedColumnSlotChangesNoReply) {
    Instance db(/*record=*/true, /*replay=*/true);
    Load(db, testing_cycle::Added::kReused);
    Instance reference(/*record=*/false, /*replay=*/false);
    Load(reference, testing_cycle::Added::kVacant);
    ExpectSame(RunAll(db, 3), RunAll(reference, 3), "recording+replay over a reused slot");
}

// BM-S3: recording and replaying with the scan kernel off answer what the
// kernel's default answers, byte for byte, every round.
TEST(WaystoneContractTest, TheScanKernelSwitchChangesNoReply) {
    Instance on_db(/*record=*/true, /*replay=*/true);
    Instance off_db(/*record=*/true, /*replay=*/true, /*scan_kernel=*/false);
    Load(on_db);
    Load(off_db);
    ExpectSame(RunAll(on_db, 3), RunAll(off_db, 3), "scan kernel on against off");
}

// CB-Q3 (a): a reordered chain records its probe step under the relation's
// written position, and replay finds it there. `b`'s indexed literal drives,
// so `j` - written first - is probed second, and its trail entries carry
// step id 0. Every round answers the walk's rows, recorded and replayed
// alike, and the trail was really written.
TEST(WaystoneContractTest, AReorderedChainRecordsAndReplaysUnderWrittenIds) {
    const std::string sql = "SELECT b.label, j.w FROM j JOIN b ON j.id = b.id WHERE b.v = 30";
    Instance db(/*record=*/true, /*replay=*/true);
    Load(db);
    // Its probe admits a descent whatever the plan, so it is opted in.
    ASSERT_TRUE(db.OptIn(sql));
    const std::string plan = db.Run("ANALYZE " + sql);
    ASSERT_NE(plan.find("reordered=1"), std::string::npos) << plan;
    ASSERT_NE(plan.find("step 0 Probe j"), std::string::npos) << plan;
    for (int round = 0; round < 4; ++round) {
        EXPECT_EQ(db.Run(sql), "b.label,j.w\\nrow3,300") << "round " << round;
    }
    // A pattern row exists from the opt-in alone, so the evidence that the
    // trail was written is that it replays.
    const std::string replayed = db.Run("ANALYZE " + sql);
    EXPECT_NE(replayed.find("replays="), std::string::npos)
        << "the reordered chain recorded no trail: " << replayed;
}

// CC-S3: recording and replay under the join planner's learned statistics.
// The plans may differ from the data-free reference's, so rows compare as a
// multiset where a statement has no ORDER BY; a trail recorded under one
// order replays under another by written step ids (CB-Q3).
TEST(WaystoneContractTest, RecordingAndReplayingUnderLearnedStatisticsChangesNoRow) {
    Instance db(/*record=*/true, /*replay=*/true);
    Load(db);
    db.Learn(Queries());
    const std::vector<std::string> got = RunAll(db, 3);
    const std::vector<std::string> want = Reference();
    ASSERT_EQ(got.size(), want.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        const std::string& sql = Queries()[i % Queries().size()];
        EXPECT_EQ(testing_reply::Unordered(got[i], sql), testing_reply::Unordered(want[i], sql))
            << "reply " << i << " (" << sql << ")";
    }
}

TEST(WaystoneContractTest, RecordingWithoutReplayingChangesNoReply) {
    Instance db(/*record=*/true, /*replay=*/false);
    Load(db);
    ExpectSame(RunAll(db, 3), Reference(), "recording only");
}

TEST(WaystoneContractTest, ReplayingWithoutRecordingChangesNoReply) {
    // Trails from an earlier era, replayed by a server that records no new
    // ones. Reachable in production by flipping `waystone_recording` off.
    Instance db(/*record=*/true, /*replay=*/true);
    Load(db);
    RunAll(db, 2);  // build the trails

    Instance quiet(/*record=*/false, /*replay=*/true);
    Load(quiet);
    ExpectSame(RunAll(quiet, 3), Reference(), "replay without recording");
}

TEST(WaystoneContractTest, DeletingEveryWaystoneMidRunChangesNoReply) {
    // Invariant 8, stated as directly as it can be tested: the trails are
    // built, then destroyed under the running database, and the answers do
    // not move. Destroyed by zeroing each pattern's waystone pages, which is
    // what "deleting a waystone" means to every reader - the header check
    // then reports false and every consultation falls through.
    Instance db(/*record=*/true, /*replay=*/true);
    Load(db);
    std::vector<std::string> replies = RunAll(db, 2);

    // A trail stands before it is destroyed: since CF-S1b a row exists as
    // soon as `Load` opts the set in, so the rows alone prove nothing.
    ASSERT_NE(db.Run("ANALYZE SELECT * FROM b WHERE id = 3").find("replays="), std::string::npos)
        << "nothing replays before the deletion; the test proves nothing";
    auto patterns = db.catalog().ListPatterns();
    ASSERT_TRUE(patterns.ok());
    std::size_t deleted = 0;
    for (const catalog::SysPatternRow& row : patterns.value()) {
        if (!catalog::HasWaystoneDirectory(row)) continue;
        // The directory root and everything under it. Zeroing the root is
        // enough: every walk from it now reads kEmptyDirSlot and misses.
        auto page = db.store().Get(row.waystone_root);
        ASSERT_TRUE(page.ok());
        std::fill(page.value().bytes().begin(), page.value().bytes().end(), std::byte{0xFF});
        ++deleted;
    }
    ASSERT_GT(deleted, 0u) << "no directory was deleted; the test proves nothing";

    const std::vector<std::string> after = RunAll(db, 2);
    replies.insert(replies.end(), after.begin(), after.end());

    // Against the same ceilings with nothing opted (CF-S1c).
    ExpectSame(replies, NotOpted(/*record=*/true, /*replay=*/true, 4),
               "opted, waystones deleted mid-run, against not opted");
}

TEST(WaystoneContractTest, ACorruptedTrailChangesNoReply) {
    // **The case that proves §2 rule 1 is load-bearing.**
    //
    // Every trail is rewritten to point at a real, live tuple that is *not*
    // the one it claims - the shape §11-3 asks for: "entries naming valid
    // pages and slots holding different tuples". Without the Keystone check
    // this returns wrong rows and no error anywhere; with it, every entry
    // misses and every answer is unchanged.
    Instance db(/*record=*/true, /*replay=*/true);
    Load(db);
    RunAll(db, 2);

    auto patterns = db.catalog().ListPatterns();
    ASSERT_TRUE(patterns.ok());
    ASSERT_FALSE(patterns.value().empty());

    std::size_t poisoned = 0;
    // Once per instance, not once per query: since AP-S2 several queries
    // above share one `{fetch_id, arg_hash}`, and a second pass would read
    // back the poisoned trail and flip each slot again - two passes put a
    // slot-0 or slot-1 entry back where it was recorded.
    std::unordered_set<stats::InstanceKey, stats::InstanceKeyHash> done;
    for (const catalog::SysPatternRow& row : patterns.value()) {
        if (!catalog::HasWaystoneDirectory(row)) continue;
        // Every instance of this pattern that we know how to name: the
        // queries above, re-fingerprinted.
        for (const std::string& sql : Queries()) {
            auto fp = parser::FingerprintOf(sql);
            if (!fp.has_value() || fp->fetch_id != row.fetch_id) continue;
            const stats::InstanceKey key{fp->fetch_id, fp->arg_hash};
            if (!done.insert(key).second) continue;

            auto existing = stats::ReadTrail(db.store(), row.waystone_root, row.dir_depth, key);
            ASSERT_TRUE(existing.ok());
            if (existing.value().empty()) continue;

            // Keep every entry's relation and step, keep a *valid* page and
            // slot - just point each one at a different row of the same
            // relation. Slot 0 is live in every relation this test loads.
            std::vector<exec::TouchedTuple> corrupt;
            for (const stats::WaystoneEntry& entry : existing.value()) {
                exec::TouchedTuple t;
                t.rel_oid = entry.rel_oid;
                t.tuple_key = entry.tuple_key;  // still claims its key, so it is still consulted
                t.page_id = entry.page_id;
                t.slot = static_cast<std::uint16_t>(entry.slot == 0 ? 1 : 0);
                t.step_id = entry.step_id;
                corrupt.push_back(t);
            }
            ASSERT_TRUE(stats::WriteTrail(db.store(), row.waystone_root, row.dir_depth, key,
                                          corrupt, /*ts=*/9)
                            .ok());
            ++poisoned;
        }
    }
    ASSERT_GT(poisoned, 0u) << "no trail was corrupted; the test proves nothing";

    // Against the same ceilings with nothing opted (CF-S1c).
    ExpectSame(RunAll(db, 3), NotOpted(/*record=*/true, /*replay=*/true, 3),
               "opted with a corrupted trail, against not opted");
}

// **The bit flipped mid-run** (CF-S1c): trails recorded, every shape opted
// out - its root cleared, the trail unreachable - then opted in again, which
// gives it a fresh, empty directory that the next executions record into
// and replay from. No reply moves at either flip.
TEST(WaystoneContractTest, FlippingTheBitMidRunChangesNoReply) {
    Instance db(/*record=*/true, /*replay=*/true);
    Load(db);
    std::vector<std::string> replies = RunAll(db, 2);
    ASSERT_NE(db.Run("ANALYZE SELECT * FROM b WHERE id = 3").find("replays="), std::string::npos)
        << "nothing replayed before the flip; the test proves nothing";

    std::size_t flipped = 0;
    for (const std::string& sql : Queries()) {
        if (db.Run("WAYSTONE OFF " + sql).find(" unchanged") == std::string::npos) ++flipped;
    }
    ASSERT_GT(flipped, 0u) << "nothing was opted out; the test proves nothing";
    std::vector<std::string> off = RunAll(db, 1);
    replies.insert(replies.end(), off.begin(), off.end());
    EXPECT_EQ(db.Run("ANALYZE SELECT * FROM b WHERE id = 3").find("replays="), std::string::npos)
        << "an opted-out shape replayed";

    for (const std::string& sql : Queries()) db.OptIn(sql);
    std::vector<std::string> on = RunAll(db, 2);
    replies.insert(replies.end(), on.begin(), on.end());
    EXPECT_NE(db.Run("ANALYZE SELECT * FROM b WHERE id = 3").find("replays="), std::string::npos)
        << "the re-opted shape did not replay";

    ExpectSame(replies, NotOpted(/*record=*/true, /*replay=*/true, 5),
               "opted with the bit flipped mid-run, against not opted");
}

// **Opted against not opted, under every ceiling** (CF-S1c): the two
// boot-only keys are the instance's ceiling over opted patterns, and no
// combination of them lets an opt-in move a reply.
TEST(WaystoneContractTest, OptedAgainstNotOptedChangesNoReplyUnderEveryCeiling) {
    for (const auto& [record, replay] : {std::pair{true, true}, std::pair{true, false},
                                         std::pair{false, true}, std::pair{false, false}}) {
        SCOPED_TRACE("waystone_recording=" + std::string(record ? "on" : "off") +
                     " waystone_replay=" + (replay ? "on" : "off"));
        Instance db(record, replay);
        Load(db);
        ExpectSame(RunAll(db, 3), NotOpted(record, replay, 3), "opted against not opted");
        // The one ceiling pair under which an opt-in can replay does, so
        // that arm is not vacuous; under the others nothing may.
        const bool replays = db.Run("ANALYZE SELECT * FROM b WHERE id = 3").find("replays=") !=
                             std::string::npos;
        EXPECT_EQ(replays, record && replay);
    }
}

// **No opted cell passes vacuously** (CF-S1c): every query the set opted
// replays once it has run twice, except the few listed here, each for a
// reason of its own that a trail could never change. A gate that let nothing
// through, or a fixture that opted nothing, fails every other line.
TEST(WaystoneContractTest, EveryRecordableOptedQueryReplays) {
    // Opted, and with nothing to record: the key each names matches no row,
    // so the collector stays empty and there is no trail to replay.
    static const std::unordered_set<std::string> kRecordsNothing = {
        "SELECT * FROM b WHERE id = 99",
        "SELECT COUNT(*) FROM b WHERE id = 99",
        "SELECT * FROM b WHERE id = 99 LIMIT 1",
    };
    Instance db(/*record=*/true, /*replay=*/true);
    Load(db);
    RunAll(db, 2);
    std::size_t opted = 0;
    for (const std::string& sql : Queries()) {
        if (!IsOpted(db, sql)) continue;
        ++opted;
        const std::string analyzed = db.Run("ANALYZE " + sql);
        if (kRecordsNothing.count(sql) != 0) continue;
        EXPECT_NE(analyzed.find("replays="), std::string::npos)
            << "an opted, recordable cell did not replay: " << sql << "\n" << analyzed;
    }
    EXPECT_GT(opted, kRecordsNothing.size()) << "the set opted almost nothing";
    for (const std::string& sql : kRecordsNothing) {
        EXPECT_TRUE(IsOpted(db, sql)) << "listed as opted and recording nothing, but not opted: "
                                      << sql;
    }
}

// ---- §11-4: lookup-not-search --------------------------------------------

TEST(WaystoneContractTest, ANonTupleKeyPredicateStillSearchesAndSaysSo) {
    // The instrumented half. A trail may replace a lookup and never a
    // search (invariant 9), and a wrongly skipped search returns *plausible*
    // rows - so the only evidence is the work that was still done.
    Instance db(/*record=*/true, /*replay=*/true);
    Load(db);

    const std::string search = "SELECT * FROM b WHERE v = 50";
    for (int i = 0; i < 4; ++i) db.Run(search);

    const std::string analyzed = db.Run("ANALYZE " + search);
    // **The kind is not the claim; being search-class is.** This asserted
    // the word "Scan" until `b.v` gained an index, and the statement now
    // compiles to an IndexProbe - which is authoritative, faster, and still
    // a *search*. Reading the trust line off `IsTrailReplayable` rather than
    // off a rendered kind name is what keeps this test about invariant 9
    // instead of about whichever accelerator exists this month.
    EXPECT_FALSE(exec::IsTrailReplayable(exec::AccessKind::kIndexProbe));
    EXPECT_NE(analyzed.find("IndexProbe"), std::string::npos) << analyzed;
    EXPECT_EQ(analyzed.find("replays="), std::string::npos)
        << "a search was served from a trail, which invariant 9 forbids: " << analyzed;
    // It reads fewer rows than the relation holds now - that is the index
    // working - so the evidence that no trail served it is `replays=`
    // being absent, not the row count. The unindexed control below is what
    // still checks a search reads everything.
    EXPECT_NE(analyzed.find("matched=1"), std::string::npos) << analyzed;
}

TEST(WaystoneContractTest, AnUnindexedNonTupleKeyPredicateStillScans) {
    // The control the test above used to be. `h.v` carries no index, so the
    // same predicate is a filter scan - which is the shape that proves the
    // index above changed the *kind* and not the trust class.
    Instance db(/*record=*/true, /*replay=*/true);
    Load(db);

    const std::string search = "SELECT * FROM h WHERE v = 50";
    for (int i = 0; i < 4; ++i) db.Run(search);

    const std::string analyzed = db.Run("ANALYZE " + search);
    EXPECT_NE(analyzed.find("Scan"), std::string::npos) << analyzed;
    EXPECT_EQ(analyzed.find("replays="), std::string::npos) << analyzed;
    EXPECT_NE(analyzed.find("examined=8"), std::string::npos) << analyzed;
}

TEST(WaystoneContractTest, ABumpedPageEpochMissesHealsAndChangesNoReply) {
    // **Spec §2 rule 2, proven load-bearing** (docs/spec/physical-optimizer.md
    // R4, workplan PX04). Until this test the epoch comparison's inputs were
    // constant - recorded 0 against a page at 0 - so it passed vacuously.
    // Here the page side moves, which is exactly what a relayout mover will
    // do: every recorded location on a bumped page must become untrusted at
    // once, every consult must miss and fall through, no reply may move, and
    // the next recording must heal the trail at the new epoch.
    Instance db(/*record=*/true, /*replay=*/true);
    Load(db);
    std::vector<std::string> replies = RunAll(db, 2);

    // The contrast: replay fires before the bump, or the miss below proves
    // nothing.
    const std::string keyed = "SELECT * FROM b WHERE id = 3";
    const std::string before = db.Run("ANALYZE " + keyed);
    ASSERT_NE(before.find("replays="), std::string::npos) << before;

    ASSERT_GT(BumpEveryUserPage(db), 0u);

    // Every consult now meets a page whose epoch moved: a per-entry miss,
    // the ordinary descent, and the same rows.
    const std::string missed = db.Run("ANALYZE " + keyed);
    EXPECT_EQ(missed.find("replays="), std::string::npos) << missed;
    EXPECT_NE(missed.find("trail_misses="), std::string::npos) << missed;

    // Self-healing: re-recording stores the new epoch, and replay resumes
    // without anyone repairing anything by hand.
    db.Run(keyed);
    db.Run(keyed);
    const std::string healed = db.Run("ANALYZE " + keyed);
    EXPECT_NE(healed.find("replays="), std::string::npos) << healed;

    // The healed trail carries the bumped epoch - read it back and say so,
    // rather than inferring it from the replay above.
    auto fp = parser::FingerprintOf(keyed);
    ASSERT_TRUE(fp.has_value());
    auto patterns = db.catalog().ListPatterns();
    ASSERT_TRUE(patterns.ok());
    bool checked = false;
    for (const catalog::SysPatternRow& row : patterns.value()) {
        if (row.fetch_id != fp->fetch_id || !catalog::HasWaystoneDirectory(row)) continue;
        auto trail = stats::ReadTrail(db.store(), row.waystone_root, row.dir_depth,
                                      stats::InstanceKey{fp->fetch_id, fp->arg_hash});
        ASSERT_TRUE(trail.ok());
        ASSERT_FALSE(trail.value().empty());
        for (const stats::WaystoneEntry& entry : trail.value()) {
            EXPECT_EQ(entry.page_epoch, 1u) << "re-recorded entry kept a stale epoch";
        }
        checked = true;
    }
    ASSERT_TRUE(checked) << "the keyed query's trail was not found; the test proves nothing";

    // And the standing invariant: nothing above moved a reply.
    const std::vector<std::string> after = RunAll(db, 2);
    replies.insert(replies.end(), after.begin(), after.end());
    Instance reference(/*record=*/false, /*replay=*/false);
    Load(reference);
    ExpectSame(replies, RunAll(reference, 4), "bumped page epoch");
}

TEST(WaystoneContractTest, AKeyedStepDoesReplaySoTheContrastIsReal) {
    // The control for the test above: the same database, a keyed predicate,
    // and the replay counter non-zero. Without this, "no replays on a scan"
    // would be satisfied by replay never working at all.
    Instance db(/*record=*/true, /*replay=*/true);
    Load(db);

    const std::string keyed = "SELECT * FROM b WHERE id = 3";
    for (int i = 0; i < 3; ++i) db.Run(keyed);

    const std::string analyzed = db.Run("ANALYZE " + keyed);
    EXPECT_NE(analyzed.find("replays="), std::string::npos)
        << "replay never fired, so the scan test above proves nothing: " << analyzed;
}

TEST(WaystoneContractTest, ABuiltJoinFeedsNoTrail) {
    // Spec-join-inner-build.md §3's hard rule, pinned where it can
    // actually fail. The query-set comparisons above cannot reach it: a
    // trail wrongly recorded for the annotated kScan step would be
    // *written and never read* - consultation happens only inside
    // `RunPointStep` - so every reply in this file would stay
    // byte-identical while the rule was broken.
    //
    // Since CF-S1b a shape records only once opted in, and a walked join
    // with no tuple key equality cannot be: `WAYSTONE ON` refuses it, so
    // no row is written and nothing records for it. The gate at the
    // recording site is then a second guard behind the opt-in's, which
    // this cell no longer reaches alone; the refusal and the absence are
    // what it pins.
    Instance db(/*record=*/true, /*replay=*/true);
    db.KeepWrittenOrder();  // the walked join as written (CB-S2)
    Load(db);

    const std::string built = "SELECT a.label, d.label FROM b AS a JOIN h AS d ON d.v = a.v";
    EXPECT_FALSE(db.OptIn(built)) << "a walked join with no tuple key equality was opted in";
    for (int i = 0; i < 4; ++i) db.Run(built);

    // The build really engaged - otherwise this asserts nothing about a
    // build, only about a walk.
    const std::string analyzed = db.Run("ANALYZE " + built);
    EXPECT_NE(analyzed.find("inner_built=1"), std::string::npos) << analyzed;

    auto fingerprint = parser::FingerprintOf(built);
    ASSERT_TRUE(fingerprint.has_value());
    auto patterns = db.catalog().ListPatterns();
    ASSERT_TRUE(patterns.ok()) << patterns.status().message();
    for (const catalog::SysPatternRow& row : patterns.value()) {
        EXPECT_NE(row.fetch_id, fingerprint->fetch_id)
            << "the built join has a pattern row, so a build fed a trail";
    }

    // The control, for `AKeyedStepDoesReplaySoTheContrastIsReal`'s
    // reason: a keyed statement in the same database, opted in by `Load`,
    // *does* record and replay, so the absence above is not a recorder that
    // never ran.
    const std::string keyed = "SELECT * FROM b WHERE id = 3";
    for (int i = 0; i < 3; ++i) db.Run(keyed);
    const std::string replayed = db.Run("ANALYZE " + keyed);
    EXPECT_NE(replayed.find("replays="), std::string::npos)
        << "nothing recorded at all, so the absence above proves nothing: " << replayed;
}

// The recording site's own gate, reached past the opt-in: a built join
// under a tuple key lookup admits a descent, so `WAYSTONE ON` takes it and
// the statement records - and only the lookup's rows may be in its trail.
// A trail entry for the built step's rows would be written and never read
// (consultation is inside `RunPointStep` only), so every reply would stay
// byte-identical while the rule was broken; the trail's contents are what
// fails if someone drops `IsTrailReplayable(step.kind)` at the recording
// site.
TEST(WaystoneContractTest, AnOptedShapeWithABuiltJoinRecordsItsLookupStepOnly) {
    Instance db(/*record=*/true, /*replay=*/true);
    db.KeepWrittenOrder();  // the walked join as written (CB-S2)
    Load(db);

    const std::string sql =
        "SELECT a.label, d.label FROM b AS a JOIN h AS d ON d.v = a.v WHERE a.id = 3";
    ASSERT_TRUE(db.OptIn(sql)) << "the premise moved: a key lookup under a build was refused";
    for (int i = 0; i < 3; ++i) db.Run(sql);
    const std::string analyzed = db.Run("ANALYZE " + sql);
    ASSERT_NE(analyzed.find("inner_built=1"), std::string::npos)
        << "the build did not engage, so this asserts nothing about a build: " << analyzed;

    auto b_oid = db.catalog().FindTableOidByName(catalog::kNamespacePublic, "b");
    ASSERT_TRUE(b_oid.ok()) << b_oid.status().message();
    auto fp = parser::FingerprintOf(sql);
    ASSERT_TRUE(fp.has_value());
    auto pattern = db.catalog().FindOptedPattern(fp->fetch_id);
    ASSERT_TRUE(pattern.ok()) << pattern.status().message();
    ASSERT_TRUE(pattern.value()->has_waystone_directory());
    auto trail = stats::ReadTrail(db.store(), pattern.value()->waystone_root,
                                  pattern.value()->dir_depth,
                                  stats::InstanceKey{fp->fetch_id, fp->arg_hash});
    ASSERT_TRUE(trail.ok()) << trail.status().message();
    ASSERT_FALSE(trail.value().empty())
        << "the lookup recorded nothing, so the absence of the build's rows proves nothing";
    for (const stats::WaystoneEntry& entry : trail.value()) {
        EXPECT_EQ(entry.rel_oid, b_oid.value()) << "the built step's row fed the trail";
        EXPECT_EQ(entry.step_id, 0u) << "the built step's row fed the trail";
    }
}

// An outer replay hit over an inner replay miss. Moving the outer row's join
// column keeps its own trail entry valid and leaves the inner one recorded
// for the old key: the outer step replays, the next step - run inside that
// replay - misses and descends, and the outer step must still count its row
// as served. A replay flag shared by the chain was cleared by the inner miss,
// so the outer step descended too and the joined row came back twice.
//
// **Mutation**: the outcome kept in one member the chain shares - the replay
// configuration answers `(1, 30)` twice.
TEST(WaystoneContractTest, AnInnerReplayMissUnderAnOuterHitEmitsTheRowOnce) {
    const std::vector<std::string> statements = {
        "CREATE TABLE p (id int64, code int64) BTREE",
        "CREATE TABLE ch (id int64, r1 int64, qty int64) BTREE",
        "INSERT INTO p VALUES (1, 10)",
        "INSERT INTO p VALUES (2, 20)",
        "INSERT INTO p VALUES (3, 30)",
        "INSERT INTO ch VALUES (1, 1, 0)",
        "SELECT ch.id, p.code FROM ch JOIN p ON ch.r1 = p.id WHERE ch.id = 1",
        "UPDATE ch SET r1 = 2 WHERE id = 1",
        "SELECT ch.id, p.code FROM ch JOIN p ON ch.r1 = p.id WHERE ch.id = 1",
        "UPDATE ch SET r1 = 3 WHERE id = 1",
        "SELECT ch.id, p.code FROM ch JOIN p ON ch.r1 = p.id WHERE ch.id = 1",
    };
    Instance replaying(/*record=*/true, /*replay=*/true);
    Instance baseline(/*record=*/false, /*replay=*/false);
    std::string last;
    for (const std::string& sql : statements) {
        last = replaying.Run(sql);
        EXPECT_EQ(last, baseline.Run(sql)) << sql;
        // Opted in once its relations exist (CF-S1b); on the replaying side
        // only, whose reply names its ceilings.
        if (sql.rfind("INSERT INTO ch", 0) == 0) ASSERT_TRUE(replaying.OptIn(statements.back()));
    }
    EXPECT_EQ(last, "ch.id,p.code\\n1,30");

    // The shape itself, under ANALYZE: one more move, so this run's outer
    // step replays and its inner step misses. The rows count does not show
    // the duplicate; the inner step's opens do - twice, had the outer step
    // descended after its replay.
    replaying.Run("INSERT INTO p VALUES (4, 40)");
    replaying.Run("UPDATE ch SET r1 = 4 WHERE id = 1");
    const std::string analyzed = replaying.Run("ANALYZE " + statements.back());
    const auto line_of = [&analyzed](const std::string& head) {
        const std::size_t at = analyzed.find(head);
        return at == std::string::npos ? std::string()
                                       : analyzed.substr(at, analyzed.find("\\n", at) - at) + " ";
    };
    const std::string outer = line_of("step 0 Lookup ch opens=");
    const std::string inner = line_of("step 1 Probe p opens=");
    EXPECT_NE(outer.find(" replays=1 "), std::string::npos) << analyzed;
    EXPECT_NE(inner.find(" opens=1 "), std::string::npos) << analyzed;
    EXPECT_EQ(inner.find(" replays="), std::string::npos) << analyzed;
}

}  // namespace
}  // namespace kds::server
