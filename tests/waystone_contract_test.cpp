#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/trail_collector.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/stats/trail_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/page_header.hpp"

// **The advisory contract** (docs/waystone-concpets.md §11-3 and §11-4,
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

namespace kds::server {
namespace {

// One database, one dispatcher, one configuration.
class Instance {
public:
    Instance(bool record, bool replay) {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        EXPECT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        recorder_.emplace(boot_->catalog, store_);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), record ? &*recorder_ : nullptr, replay);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    catalog::Catalog& catalog() { return boot_->catalog; }
    storage::InMemoryPageStore& store() { return store_; }

private:
    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::TrailRecorder> recorder_;
    std::optional<CommandDispatcher> dispatcher_;
};

// The dataset every configuration runs over. Two relations, one heap and
// one btree, so both the descent path and the chain-scan path are covered -
// they are the two things replay replaces and they fail differently.
void Load(Instance& db) {
    ASSERT_EQ(db.Run("CREATE TABLE b (id int64, v int64, label varchar) BTREE").substr(0, 7),
              "CREATED");
    ASSERT_EQ(db.Run("CREATE TABLE h (id int64, v int64, label varchar)").substr(0, 7),
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
        ASSERT_EQ(db.Run("INSERT INTO h VALUES (" + v + ", " + label + ")").substr(0, 8),
                  "INSERTED");
        ASSERT_EQ(db.Run("INSERT INTO j VALUES (" + std::to_string(i * 100) + ")").substr(0, 8),
                  "INSERTED");
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

        // ---- Aggregated statements (docs/feat-aggregate.md §9 item 8) --
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

        // ---- Indexed statements (docs/feat-index.md §8) ----------------
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
// because no mover exists (docs/feat-physical-optimizer.md §6). Sweeps the
// user id range; absent ids just miss.
std::size_t BumpEveryUserPage(Instance& db) {
    std::size_t bumped = 0;
    const PageId end = static_cast<PageId>(kFirstUserPageId + db.store().page_count());
    for (PageId id = kFirstUserPageId; id < end; ++id) {
        auto page = db.store().Get(id);
        if (!page.ok()) continue;
        storage::BumpRelayoutEpoch(page.value());
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

    auto patterns = db.catalog().ListPatterns();
    ASSERT_TRUE(patterns.ok());
    ASSERT_FALSE(patterns.value().empty()) << "nothing was recorded; the test proves nothing";
    for (const catalog::SysPatternRow& row : patterns.value()) {
        if (!catalog::HasWaystoneDirectory(row)) continue;
        // The directory root and everything under it. Zeroing the root is
        // enough: every walk from it now reads kEmptyDirSlot and misses.
        auto page = db.store().Get(row.waystone_root);
        ASSERT_TRUE(page.ok());
        std::fill(page.value().begin(), page.value().end(), std::byte{0xFF});
    }

    const std::vector<std::string> after = RunAll(db, 2);
    replies.insert(replies.end(), after.begin(), after.end());

    Instance reference(/*record=*/false, /*replay=*/false);
    Load(reference);
    ExpectSame(replies, RunAll(reference, 4), "waystones deleted mid-run");
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
    for (const catalog::SysPatternRow& row : patterns.value()) {
        if (!catalog::HasWaystoneDirectory(row)) continue;
        // Every instance of this pattern that we know how to name: the
        // queries above, re-fingerprinted.
        for (const std::string& sql : Queries()) {
            auto fp = parser::FingerprintOf(sql);
            if (!fp.has_value() || fp->pattern_id != row.pattern_id) continue;
            const stats::InstanceKey key{fp->pattern_id, fp->arg_hash};

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
                t.pk = entry.pk;  // still claims its key, so it is still consulted
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

    ExpectSame(RunAll(db, 3), Reference(), "corrupted trail");
}

// ---- §11-4: lookup-not-search --------------------------------------------

TEST(WaystoneContractTest, ANonPkPredicateStillSearchesAndSaysSo) {
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

TEST(WaystoneContractTest, AnUnindexedNonPkPredicateStillScans) {
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
    // **Spec §2 rule 2, proven load-bearing** (docs/feat-physical-optimizer.md
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
        if (row.pattern_id != fp->pattern_id || !catalog::HasWaystoneDirectory(row)) continue;
        auto trail = stats::ReadTrail(db.store(), row.waystone_root, row.dir_depth,
                                      stats::InstanceKey{fp->pattern_id, fp->arg_hash});
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

}  // namespace
}  // namespace kds::server
