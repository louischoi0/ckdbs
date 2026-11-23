#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/budget.hpp"
#include "kds/exec/join_order.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "view_text.hpp"

// **A correlated Cabin probe's miss answered from the statement-local build**
// (DQ E1; docs/spec/join-inner-build.md §5a, docs/spec/cabin.md §4a). A join
// on a cabined inner column serves an observed key from its set; a key the
// set has not observed is answered from the build's map, the statement's
// first such miss walking and bucketing and every later one probing - where
// before every miss walked the inner relation whole. Each test pins one of
// the order's rules:
//
//   - DQ-R1: one answer with the Cabin serving, with the map serving, and
//     with both taken away;
//   - DQ-R3: the Cabin observes exactly what it observed before - the same
//     sightings, recordings and declines - since only a walk that records
//     nothing became a probe;
//   - the premise DQ-S0 measured: a banking gate that declines every
//     recording no longer makes a join walk once per outer row, and so no
//     longer turns a join its build answers into a budget refusal.

namespace kds::server {
namespace {

constexpr int kInner = 64;
constexpr int kOuter = 16;
constexpr const char* kJoin = "SELECT o.id, i.w FROM outr AS o JOIN inn AS i ON i.k = o.k";

exec::Budget NoBuild(exec::Budget budget = exec::Budget()) {
    budget.set_join_build_max_rows(0);
    return budget;
}

// One database; `budget` is the dispatcher's, so a test can cap the row
// touches or take the build away (`NoBuild`).
class Db {
public:
    explicit Db(exec::Budget budget = exec::Budget(), bool cabins = true) {
        if (cabins) cabins_.emplace();
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        EXPECT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            budget, /*recorder=*/nullptr, /*replay=*/false,
                            /*access_statistics=*/true, cabins_ ? &*cabins_ : nullptr, &*mgr_);
        dispatcher_->set_join_order(exec::JoinOrder::kWritten);
        Ok("CREATE TABLE inn (id int64, k int64, w int64) BTREE");
        // One inner row a key, keys 1..64 scattered across the walk.
        for (int i = 0; i < kInner; ++i) {
            Ok("INSERT INTO inn VALUES (" + std::to_string(i + 1) + ", " +
               std::to_string((i * 37) % kInner + 1) + ", " + std::to_string(100 + i) + ")");
        }
        // Sixteen outer rows over twelve keys, 2, 5, ..., 35; the first four
        // (2, 5, 8, 11) come round again in rows 13 to 16.
        Ok("CREATE TABLE outr (id int64, k int64) BTREE");
        for (int o = 0; o < kOuter; ++o) {
            Ok("INSERT INTO outr VALUES (" + std::to_string(o + 1) + ", " +
               std::to_string(3 * (o < 12 ? o : o - 12) + 2) + ")");
        }
        Ok("CREATE TABLE hold (id int64, v int64) BTREE");
    }

    DispatchOutcome Do(const std::string& sql) { return dispatcher_->Dispatch(sql); }
    std::string Run(const std::string& sql) { return Do(sql).response; }
    void Ok(const std::string& sql) {
        const std::string out = Run(sql);
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }
    // A reply's rows, sorted: emission order is no contract (I11).
    std::vector<std::string> Rows(const std::string& sql) {
        const std::string out = Run(sql);
        EXPECT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
        std::vector<std::string> rows;
        std::size_t at = out.find("\\n");
        while (at != std::string::npos) {
            const std::size_t next = out.find("\\n", at + 2);
            rows.push_back(out.substr(at + 2, next == std::string::npos ? next : next - at - 2));
            at = next;
        }
        std::sort(rows.begin(), rows.end());
        return rows;
    }
    // A second session's write held open: every view minted while it lives
    // is unbankable (cabin.md §6a), so every recording the join attempts is
    // declined.
    void HoldAWrite() {
        ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &other_).response.rfind("BEGIN", 0), 0u);
        const std::string inserted =
            dispatcher_->Dispatch("INSERT INTO hold (v) VALUES (1)", &other_).response;
        ASSERT_EQ(inserted.rfind("INSERTED", 0), 0u) << inserted;
    }
    void ReleaseTheWrite() {
        const std::string out = dispatcher_->Dispatch("ROLLBACK", &other_).response;
        ASSERT_EQ(out.rfind("ERR", 0), std::string::npos) << out;
    }

private:
    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<stats::CabinStore> cabins_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
    Session other_;
};

// The inner step's ANALYZE counter `name=`, or -1 where it is not printed.
long Counter(const std::string& plan, const std::string& name) {
    const std::size_t step = plan.find("step 1 CabinProbe inn AS i opens=");
    if (step == std::string::npos) return -1;
    const std::string line = plan.substr(step, plan.find("\\n", step) - step);
    const std::size_t at = line.find(" " + name + "=");
    if (at == std::string::npos) return -1;
    return std::stol(line.substr(at + name.size() + 2));
}

// What the join answers with no Cabin and no build: the per-row walk.
std::vector<std::string> Walked() {
    Db reference(NoBuild(), /*cabins=*/false);
    return reference.Rows(kJoin);
}

TEST(CabinMissBuildTest, AGateDeclinedJoinIsAnsweredFromTheMapInEveryStatement) {
    Db db;
    db.Ok("CREATE CABIN ON inn(k)");
    const std::vector<std::string> want = Walked();
    ASSERT_EQ(want.size(), static_cast<std::size_t>(kOuter));
    db.HoldAWrite();
    for (int run = 0; run < 3; ++run) {
        const std::string plan = db.Run(std::string("ANALYZE ") + kJoin);
        // Every key misses - the gate declines each recording - and the
        // first miss's walk built the map the other fifteen probed.
        EXPECT_EQ(Counter(plan, "cabin_misses"), kOuter) << plan;
        EXPECT_EQ(Counter(plan, "cabin_recorded"), -1) << plan;
        EXPECT_EQ(Counter(plan, "inner_built"), 1) << plan;
        EXPECT_EQ(Counter(plan, "build_rows"), kInner) << plan;
        EXPECT_EQ(Counter(plan, "build_probes"), kOuter - 1) << plan;
        // One walk and a row a probe, where the per-row walk examined
        // sixteen walks' worth.
        EXPECT_EQ(Counter(plan, "examined"), kInner + kOuter - 1) << plan;
        EXPECT_EQ(db.Rows(kJoin), want) << "run " << run;
    }
    db.ReleaseTheWrite();
}

TEST(CabinMissBuildTest, TheFallbackKeepsAGateDeclinedJoinInsideItsBudget) {
    // The refusal DQ-S0 measured, in miniature: the per-row walk touches
    // 16 x 64 inner rows, the map 64 and a row a probe. A budget between
    // the two answers the join with the fallback and refuses it without.
    {
        Db db(exec::Budget(400));
        db.Ok("CREATE CABIN ON inn(k)");
        db.HoldAWrite();
        EXPECT_EQ(db.Rows(kJoin), Walked());
        db.ReleaseTheWrite();
    }
    Db db(NoBuild(exec::Budget(400)));
    db.Ok("CREATE CABIN ON inn(k)");
    db.HoldAWrite();
    const DispatchOutcome refused = db.Do(kJoin);
    EXPECT_EQ(refused.status.code(), StatusCode::kResourceExhausted) << refused.response;
    db.ReleaseTheWrite();
}

TEST(CabinMissBuildTest, AMixedStatementServesSetsAndProbesTheMapForOneAnswer) {
    // DQ-R1's mixed statement. Keys 2, 5 and 8 are observed by a literal
    // probe first (a declared Cabin records a literal value at its first
    // selection, cabin.md §8.1), so their six outer rows are served from
    // sets. Of the ten misses, row 4's (key 11, first seen) walks and
    // builds, row 16's (key 11 again) is a recording touch and walks and
    // records as it always did (DQ-Q2 (a)), and the other eight probe.
    const auto observe_three = [](Db& db) {
        db.Ok("CREATE CABIN ON inn(k)");
        for (const char* key : {"2", "5", "8"}) {
            EXPECT_EQ(db.Rows(std::string("SELECT * FROM inn WHERE k = ") + key).size(), 1u);
        }
    };
    Db db;
    observe_three(db);
    const std::string plan = db.Run(std::string("ANALYZE ") + kJoin);
    EXPECT_EQ(Counter(plan, "cabin_hits"), 6) << plan;
    EXPECT_EQ(Counter(plan, "cabin_misses"), 10) << plan;
    EXPECT_EQ(Counter(plan, "cabin_recorded"), 1) << plan;
    EXPECT_EQ(Counter(plan, "inner_built"), 1) << plan;
    EXPECT_EQ(Counter(plan, "build_probes"), 8) << plan;

    // The Cabin and the map together answer what the walk answers; and
    // with the map taken away, the same Cabin state answers the same.
    Db fresh;
    observe_three(fresh);
    const std::vector<std::string> served = fresh.Rows(kJoin);
    EXPECT_EQ(served, Walked());
    Db unbuilt(NoBuild());
    observe_three(unbuilt);
    const std::string unbuilt_plan = unbuilt.Run(std::string("ANALYZE ") + kJoin);
    EXPECT_EQ(Counter(unbuilt_plan, "cabin_hits"), 6) << unbuilt_plan;
    EXPECT_EQ(Counter(unbuilt_plan, "inner_built"), 0) << unbuilt_plan;
    Db again(NoBuild());
    observe_three(again);
    EXPECT_EQ(again.Rows(kJoin), served);
}

TEST(CabinMissBuildTest, TheCabinObservesWhatItObservedWithoutTheFallback) {
    // DQ-R3: one statement sequence with the fallback and without it - a
    // run that sights every key, one under a held write whose recordings
    // the gate declines, and two that record and serve. Every reply and the
    // Cabin's whole report (observed values, entries, hits, misses) are the
    // same; only the walks the map replaced differ.
    // The join and, after it, a correlated `EXISTS` (DQ-S3) keyed by the
    // outer row's tuple key over the same Cabin, so the sub-chain's misses
    // go through the prefix map in the same sequence.
    const auto sequence = [](exec::Budget budget) {
        Db db(budget);
        db.Ok("CREATE CABIN ON inn(k)");
        std::vector<std::string> out;
        for (int run = 0; run < 4; ++run) {
            if (run == 1) db.HoldAWrite();
            for (const std::string& row : db.Rows(kJoin)) out.push_back(row);
            for (const std::string& row :
                 db.Rows("SELECT o.id FROM outr AS o WHERE EXISTS "
                         "(SELECT i.id FROM inn AS i WHERE i.k = o.id)")) {
                out.push_back("exists " + row);
            }
            if (run == 1) db.ReleaseTheWrite();
        }
        for (const std::string& row :
             db.Rows("SELECT observed, entries, hits, misses FROM sys.cabins")) {
            out.push_back(row);
        }
        // The gate's declines and the caps' refusals, the store's own words.
        const std::string meta = db.Run("SHOW META");
        for (const char* field : {"cabins_unbankable_views", "cabins_cap_refusals"}) {
            out.push_back(std::string(field) + "=" +
                          testing_view::MetaField(meta, field).value_or("absent"));
        }
        return out;
    };
    {
        Db db;
        db.Ok("CREATE CABIN ON inn(k)");
        const std::string plan = db.Run(
            "ANALYZE SELECT o.id FROM outr AS o WHERE EXISTS "
            "(SELECT i.id FROM inn AS i WHERE i.k = o.id)");
        ASSERT_NE(plan.find("CabinProbe inn AS i cabin=1 on=col1 key=1:0.0 build on=col1"),
                  std::string::npos)
            << "the sub-chain is a Cabin probe carrying the fallback: " << plan;
    }
    const std::vector<std::string> without = sequence(NoBuild());
    const std::vector<std::string> with = sequence(exec::Budget());
    EXPECT_EQ(with, without);
    ASSERT_GE(with.size(), 3u);
    EXPECT_NE(with[with.size() - 3].rfind("0,", 0), 0u) << "sets were observed";
    EXPECT_NE(with[with.size() - 2], "cabins_unbankable_views=0") << "the gate declined";
    EXPECT_NE(with[with.size() - 2], "cabins_unbankable_views=absent");
}

TEST(CabinMissBuildTest, ANullOuterKeyAndASwitchedOffStoreFallToTheMap) {
    // Two of spec §5a's walks that record nothing. With no store the probe
    // has no key for any row, so the map answers every outer row; a NULL
    // outer key is unkeyable, so the set is never asked (no miss is
    // counted), it matches no inner row on either path, and as the first
    // outer row it builds the map the other three probe.
    {
        Db db(exec::Budget(), /*cabins=*/false);
        db.Ok("CREATE CABIN ON inn(k)");
        const std::string plan = db.Run(std::string("ANALYZE ") + kJoin);
        EXPECT_EQ(Counter(plan, "inner_built"), 1) << plan;
        EXPECT_EQ(Counter(plan, "build_probes"), kOuter - 1) << plan;
        EXPECT_EQ(db.Rows(kJoin), Walked());
    }
    const char* nullable = "SELECT o.id, i.w FROM nk AS o JOIN inn AS i ON i.k = o.k";
    const auto load = [](Db& db) {
        db.Ok("CREATE TABLE nk (id int64, k int64 NULL) BTREE");
        db.Ok("INSERT INTO nk VALUES (1, NULL)");
        db.Ok("INSERT INTO nk VALUES (2, 5)");
        db.Ok("INSERT INTO nk VALUES (3, NULL)");
        db.Ok("INSERT INTO nk VALUES (4, 9)");
    };
    Db reference(NoBuild(), /*cabins=*/false);
    load(reference);
    const std::vector<std::string> want = reference.Rows(nullable);
    ASSERT_EQ(want.size(), 2u);
    Db db;
    db.Ok("CREATE CABIN ON inn(k)");
    load(db);
    const std::string plan = db.Run(std::string("ANALYZE ") + nullable);
    EXPECT_EQ(Counter(plan, "cabin_misses"), 2) << plan;
    EXPECT_EQ(Counter(plan, "inner_built"), 1) << plan;
    EXPECT_EQ(Counter(plan, "build_probes"), 3) << plan;
    EXPECT_EQ(db.Rows(nullable), want);
}

// ---- DQ-S3: a sub-chain's miss falls to the prefix map ---------------------
//
// A correlated `EXISTS`-class sub-chain over a cabined column stops each walk
// at its first qualifying row, so its fallback is the build's prefix form
// (join-inner-build.md §6): a miss replays the bucket the walks so far filled
// and resumes at the mark. `w > 130` admits 33 of the 64 inner rows, so some
// outer keys qualify and some do not, and a `NOT EXISTS` must conclude an
// absence - which a prefix map may do only once it covers the relation.
const char* const kSubChains[] = {
    "SELECT o.id FROM outr AS o WHERE EXISTS "
    "(SELECT i.id FROM inn AS i WHERE i.k = o.k AND i.w > 130)",
    "SELECT o.id FROM outr AS o WHERE NOT EXISTS "
    "(SELECT i.id FROM inn AS i WHERE i.k = o.k AND i.w > 130)",
};

TEST(CabinMissBuildTest, AGateDeclinedSubChainIsAnsweredFromThePrefixMap) {
    for (const char* sql : kSubChains) {
        Db reference(NoBuild(), /*cabins=*/false);
        const std::vector<std::string> want = reference.Rows(sql);

        Db db;
        db.Ok("CREATE CABIN ON inn(k)");
        db.HoldAWrite();
        for (int run = 0; run < 2; ++run) {
            const std::string plan = db.Run(std::string("ANALYZE ") + sql);
            // Every key misses, and every miss is the prefix form's: the
            // walks visit each inner row once (46, 7, 7 and 4 rows, the
            // last reaching the end), and fifteen replays read seven
            // bucketed rows - 71 examined, where the per-row walk examined
            // 896. The map ends holding every row the `w > 130` admits.
            EXPECT_EQ(Counter(plan, "cabin_misses"), kOuter) << sql << plan;
            EXPECT_EQ(Counter(plan, "cabin_recorded"), -1) << sql << plan;
            EXPECT_EQ(Counter(plan, "examined"), 71) << sql << plan;
            EXPECT_EQ(Counter(plan, "build_rows"), 33) << sql << plan;
            EXPECT_EQ(Counter(plan, "inner_built"), 1) << sql << plan;
            EXPECT_EQ(Counter(plan, "build_probes"), kOuter - 1) << sql << plan;
            EXPECT_EQ(db.Rows(sql), want) << sql << ", run " << run;
        }
        db.ReleaseTheWrite();
    }
}

TEST(CabinMissBuildTest, ASubChainAnswersTheSameWithTheCabinTheMapAndNeither) {
    // DQ-R1 for the sub-chain, the gate open: rounds that sight, record and
    // serve, against the walk with neither and against the Cabin alone.
    for (const char* sql : kSubChains) {
        Db reference(NoBuild(), /*cabins=*/false);
        const std::vector<std::string> want = reference.Rows(sql);
        Db with;
        Db cabin_alone(NoBuild());
        with.Ok("CREATE CABIN ON inn(k)");
        cabin_alone.Ok("CREATE CABIN ON inn(k)");
        for (int run = 0; run < 4; ++run) {
            EXPECT_EQ(with.Rows(sql), want) << sql << ", run " << run;
            EXPECT_EQ(cabin_alone.Rows(sql), want) << sql << ", run " << run;
        }
    }
}


TEST(CabinMissBuildTest, ASubChainInterleavesHitsRecordingsAndThePrefixMapForOneAnswer) {
    // DQ-S0 (ii)'s claim, in the order it bears weight: one `EXISTS`
    // statement whose outer rows take every arm - a set, a recording walk
    // through the stop, the prefix map - with recordings between the
    // fallbacks. Key 2 is banked by a literal probe; keys 5 and 8 are
    // sighted by a statement over outer rows 2 and 3. Then, row by row:
    // 1 a hit; 2 and 3 recording walks; 4 to 12 first sightings, the
    // fallback walking from the head and resuming at its mark; 13 to 15
    // hits; 16 a recording. The map buckets every admitted row once and
    // reaches the end: the recordings neither moved its mark nor filled it.
    const char* sql = kSubChains[0];
    Db reference(NoBuild(), /*cabins=*/false);
    const std::vector<std::string> want = reference.Rows(sql);

    Db db;
    db.Ok("CREATE CABIN ON inn(k)");
    EXPECT_EQ(db.Rows("SELECT * FROM inn WHERE k = 2").size(), 1u);
    db.Rows(std::string(sql) + " AND o.id >= 2 AND o.id <= 3");
    const std::string plan = db.Run(std::string("ANALYZE ") + sql);
    EXPECT_EQ(Counter(plan, "cabin_hits"), 4) << plan;
    EXPECT_EQ(Counter(plan, "cabin_misses"), 12) << plan;
    EXPECT_EQ(Counter(plan, "cabin_recorded"), 3) << plan;
    EXPECT_EQ(Counter(plan, "inner_built"), 1) << plan;
    EXPECT_EQ(Counter(plan, "build_rows"), 33) << plan;
    EXPECT_EQ(Counter(plan, "build_probes"), 8) << plan;

    Db again;
    again.Ok("CREATE CABIN ON inn(k)");
    EXPECT_EQ(again.Rows("SELECT * FROM inn WHERE k = 2").size(), 1u);
    again.Rows(std::string(sql) + " AND o.id >= 2 AND o.id <= 3");
    EXPECT_EQ(again.Rows(sql), want);
}
}  // namespace
}  // namespace kds::server
