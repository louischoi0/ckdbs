#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/page_header.hpp"

// **The cabin contract** (docs/spec/cabin.md, workplan CB10). The Waystone
// suite's shape, pointed at a structure with the opposite trust class - and
// the difference is what this file has to prove.
//
// A trail is advisory: invariant 8 says deleting one may cost performance
// and must never change a result. A Cabin is **authoritative for observed
// values**, so "deleting it changes nothing" is still required but is no
// longer the whole story. Two more things have to hold, and neither is
// checkable by looking at one execution:
//
//   - **Serving must not lose a row.** An observed value's entry set is a
//     superset of the qualifying pks; if the write hook ever misses an
//     append, a query returns *fewer* rows and looks perfectly plausible.
//   - **Serving must not gain one.** The set is a superset, so surplus
//     entries are expected - a row updated away from the value, a duplicate
//     from a v→v′→v round trip - and the read has to subtract them.
//
// So the same query set runs over the same data in several configurations
// and every reply is compared **byte for byte** against a database with no
// Cabin at all:
//
//   1. cabins off                    the baseline - no Cabin can be involved
//   2. cabins on, none declared      the switch alone changes nothing
//   3. a Cabin on the filtered column    the real one
//   4. writes after observation      the witness (§5)
//   5. corrupted location hints      entries naming valid pages and slots
//                                    holding *different* tuples
//   6. dangling pks planted in a set entries naming rows that do not exist
//
// Cases 5 and 6 are the ones that prove C2 load-bearing rather than
// decorative: authority lives in the pk, the location is advice, and a
// wrong hint must cost a descent and not a row.

namespace kds::server {
namespace {

// One database, one dispatcher, one configuration. Limits and budget are
// parameters because the cap tests need caps a real workload would hit
// and a budget tight enough to catch a walk that should not have run.
class Instance {
public:
    explicit Instance(bool cabins, stats::CabinLimits limits = {},
                      exec::Budget budget = exec::Budget()) {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        EXPECT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        if (cabins) cabins_.emplace(limits);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            budget, /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, cabins_ ? &*cabins_ : nullptr);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    catalog::Catalog& catalog() { return boot_->catalog; }
    stats::CabinStore& cabins() { return *cabins_; }
    storage::InMemoryPageStore& store() { return store_; }

private:
    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<CommandDispatcher> dispatcher_;
};

// Two relations, one btree and one heap. Both are covered because the two
// fail differently on a bad hint: a btree resolves the pk and heals, a heap
// has no descent at all and must abandon the Cabin for the walk.
//
// `sym` repeats across rows on purpose - a Cabin on a column where every
// value is unique would exercise nothing about entry sets.
void Load(Instance& db) {
    ASSERT_EQ(db.Run("CREATE TABLE b (id int64, sym varchar, qty int64) BTREE").substr(0, 7),
              "CREATED");
    ASSERT_EQ(db.Run("CREATE TABLE h (id int64, sym varchar, qty int64)").substr(0, 7),
              "CREATED");
    const char* kSyms[] = {"aaa", "bbb", "aaa", "ccc", "bbb", "aaa", "ddd", "bbb"};
    for (int i = 0; i < 8; ++i) {
        const std::string row =
            std::string("('") + kSyms[i] + "', " + std::to_string((i + 1) * 10) + ")";
        ASSERT_EQ(db.Run("INSERT INTO b VALUES " + row).substr(0, 8), "INSERTED");
        ASSERT_EQ(db.Run("INSERT INTO h VALUES " + row).substr(0, 8), "INSERTED");
    }
}

void DeclareCabins(Instance& db) {
    ASSERT_EQ(db.Run("CREATE CABIN ON b(sym)").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Run("CREATE CABIN ON h(sym)").substr(0, 7), "CREATED");
}

// The query set. Mixed on purpose: values with several matching rows, a
// value matching exactly one, a value matching **nothing** (the case where
// an observed Cabin answers authoritatively without reading the relation),
// a cabined equality with an extra conjunct (which must not narrow what the
// Cabin records), a bare scan, and a pk lookup that no Cabin touches.
const std::vector<std::string>& Queries() {
    static const std::vector<std::string> kQueries = {
        "SELECT * FROM b WHERE sym = 'aaa'",
        "SELECT * FROM b WHERE sym = 'bbb'",
        "SELECT * FROM b WHERE sym = 'ddd'",
        "SELECT * FROM b WHERE sym = 'zzz'",
        "SELECT * FROM b WHERE sym = 'aaa' AND qty > 30",
        "SELECT id, qty FROM b WHERE sym = 'bbb'",
        "SELECT * FROM h WHERE sym = 'aaa'",
        "SELECT * FROM h WHERE sym = 'zzz'",
        "SELECT * FROM h WHERE sym = 'ccc' AND qty = 40",
        "SELECT * FROM b WHERE id = 3",
        "SELECT * FROM b",

        // ---- The correlated probe (cabin.md §4a) ------------------
        //
        // Self-joins whose inner side is keyed by the outer row's `sym` -
        // no literal, so the inner step is a per-outer-row CabinProbe on
        // the cabined configurations and a walk on the reference. The
        // rounds exercise the whole ladder per distinct key: miss, record,
        // serve. The heap self-join is the shape this form exists for
        // (IX3 refuses a heap relation an index), and it also exercises
        // the hint-failure re-record path when a round's writes move rows.
        "SELECT x.id, y.id FROM b AS x JOIN b AS y ON y.sym = x.sym "
        "WHERE x.id = 2",
        "SELECT x.id, y.qty FROM b AS x JOIN b AS y ON y.sym = x.sym "
        "WHERE x.id BETWEEN 1 AND 3",
        "SELECT x.id, y.id FROM h AS x JOIN h AS y ON y.sym = x.sym "
        "WHERE x.id BETWEEN 1 AND 3",
        // The correlated EXISTS, both clustering types.
        "SELECT id FROM b AS o WHERE EXISTS "
        "(SELECT i.id FROM b AS i WHERE i.sym = o.sym AND i.qty > 30)",
        "SELECT id FROM h AS o WHERE EXISTS "
        "(SELECT i.id FROM h AS i WHERE i.sym = o.sym AND i.qty > 30)",
    };
    return kQueries;
}

// Runs the set `rounds` times. Repeated because the interesting window is
// never the first execution: with `n = 2` the first miss only counts, the
// second records, and the third is the first that can be served.
std::vector<std::string> RunAll(Instance& db, int rounds) {
    std::vector<std::string> out;
    for (int r = 0; r < rounds; ++r) {
        for (const std::string& sql : Queries()) out.push_back(db.Run(sql));
    }
    return out;
}

// The reference: a database with cabins switched off entirely, so no entry
// set can possibly be involved in any answer it gives.
std::vector<std::string> Reference(int rounds = 4) {
    Instance db(/*cabins=*/false);
    Load(db);
    return RunAll(db, rounds);
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
// user id range; absent ids just miss.
std::size_t BumpEveryUserPage(Instance& db) {
    std::size_t bumped = 0;
    const PageId end = static_cast<PageId>(kFirstUserPageId + db.store().page_count());
    for (PageId id = kFirstUserPageId; id < end; ++id) {
        auto page = db.store().Get(id);
        if (!page.ok()) continue;
        storage::BumpRelayoutEpoch(page.value().bytes());
        ++bumped;
    }
    return bumped;
}

// ---- The configurations --------------------------------------------------

TEST(CabinContractTest, TheSwitchAloneChangesNoReply) {
    // Cabins enabled but none declared. Nothing should differ, and if it
    // does, the fault is in the compiler or the write hook rather than in
    // anything a Cabin did.
    Instance db(/*cabins=*/true);
    Load(db);
    ExpectSame(RunAll(db, 4), Reference(), "cabins on, none declared");
}

TEST(CabinContractTest, ServingFromACabinChangesNoReply) {
    Instance db(/*cabins=*/true);
    Load(db);
    DeclareCabins(db);
    ExpectSame(RunAll(db, 4), Reference(), "cabins serving");
}

TEST(CabinContractTest, DroppingACabinMidRunChangesNoReply) {
    // §1's corollary, tested as directly as it can be: un-observing is
    // always legal. The Cabins are built, then dropped under the running
    // database, and the answers do not move.
    Instance db(/*cabins=*/true);
    Load(db);
    DeclareCabins(db);
    std::vector<std::string> replies = RunAll(db, 2);

    ASSERT_EQ(db.Run("DROP CABIN ON b(sym)").substr(0, 7), "DROPPED");
    ASSERT_EQ(db.Run("DROP CABIN ON h(sym)").substr(0, 7), "DROPPED");

    const std::vector<std::string> after = RunAll(db, 2);
    replies.insert(replies.end(), after.begin(), after.end());
    ExpectSame(replies, Reference(), "cabins dropped mid-run");
}

TEST(CabinContractTest, WritesAfterObservationChangeNoReply) {
    // **The witness** (§5). Rows are inserted and updated *after* the values
    // are observed, which is the whole reason a Cabin can be authoritative:
    // if the hook misses an append the query returns fewer rows, and if it
    // wrongly removed one it would return more.
    //
    // The two databases are driven identically, so any divergence is the
    // Cabin's doing and nothing else's.
    Instance db(/*cabins=*/true);
    Instance ref(/*cabins=*/false);
    Load(db);
    Load(ref);
    DeclareCabins(db);

    std::vector<std::string> got = RunAll(db, 3);
    std::vector<std::string> want = RunAll(ref, 3);

    const std::vector<std::string> kWrites = {
        "INSERT INTO b VALUES ('aaa', 999)",   // a new row for an observed value
        "INSERT INTO h VALUES ('aaa', 999)",
        "INSERT INTO b VALUES ('zzz', 111)",   // a value observed as *empty*
        "INSERT INTO h VALUES ('zzz', 111)",
        "UPDATE b SET sym = 'ccc' WHERE id = 1",  // observed value -> another
        "UPDATE h SET sym = 'ccc' WHERE id = 1",
        "UPDATE b SET sym = 'aaa' WHERE id = 1",  // and back: the v->v'->v duplicate
        "UPDATE h SET sym = 'aaa' WHERE id = 1",
        "UPDATE b SET qty = 7 WHERE id = 2",   // key column untouched
        "UPDATE h SET qty = 7 WHERE id = 2",
    };
    for (const std::string& write : kWrites) {
        const std::string a = db.Run(write);
        const std::string b = ref.Run(write);
        ASSERT_EQ(a, b) << write;
        ASSERT_NE(a.substr(0, 3), "ERR") << write << " -> " << a;

        // Re-read after **every** write, not just at the end: a hook that
        // misses one append and is saved by a later one would pass a
        // check made only at the end.
        const std::vector<std::string> after_db = RunAll(db, 1);
        const std::vector<std::string> after_ref = RunAll(ref, 1);
        got.insert(got.end(), after_db.begin(), after_db.end());
        want.insert(want.end(), after_ref.begin(), after_ref.end());
    }
    ExpectSame(got, want, "writes after observation");
}

TEST(CabinContractTest, CorruptedLocationHintsChangeNoReply) {
    // **C6, proven load-bearing.** Every hint in every entry set is pointed
    // at a page and slot holding a *different* tuple. On a btree relation the
    // reader must notice, resolve the pk, and heal; on a heap relation it
    // must abandon the Cabin and walk. Either way the rows must not move.
    //
    // Without the id check in `VerifyTupleAt`, this test returns real rows
    // from the wrong places and every other test in this file still passes.
    Instance db(/*cabins=*/true);
    Load(db);
    DeclareCabins(db);
    std::vector<std::string> replies = RunAll(db, 3);

    // Reach into the store's entry sets directly. There is no statement that
    // corrupts a Cabin - which is the point - so the test does what a bug
    // would do.
    auto b_oid = db.catalog().FindTableOidByName("b");
    ASSERT_TRUE(b_oid.ok());
    auto h_oid = db.catalog().FindTableOidByName("h");
    ASSERT_TRUE(h_oid.ok());

    std::size_t poisoned = 0;
    for (const catalog::Oid oid : {b_oid.value(), h_oid.value()}) {
        auto access = db.catalog().InitTableAccess(oid);
        ASSERT_TRUE(access.ok());
        const std::uint64_t cabin_id = access.value()->CabinOn(1).id;
        ASSERT_NE(cabin_id, 0u);
        for (const char* sym : {"aaa", "bbb", "ccc", "ddd", "zzz"}) {
            parser::AstValue value;
            value.type = parser::ValueType::kStr;
            value.str_val = sym;
            auto key = stats::MakeCabinKey(cabin_id, value);
            ASSERT_TRUE(key.has_value());
            std::vector<stats::CabinEntry>* entries = db.cabins().Find(*key);
            if (entries == nullptr) continue;
            for (stats::CabinEntry& entry : *entries) {
                // A slot that exists and holds someone else's row - the
                // dangerous corruption, not an obviously broken one.
                entry.slot = static_cast<std::uint16_t>((entry.slot + 3) % 8);
                ++poisoned;
            }
        }
    }
    ASSERT_GT(poisoned, 0u) << "nothing was observed; the test proves nothing";

    const std::vector<std::string> after = RunAll(db, 2);
    replies.insert(replies.end(), after.begin(), after.end());
    ExpectSame(replies, Reference(5), "corrupted location hints");
}

TEST(CabinContractTest, ABumpedPageEpochMissesHealsAndChangesNoReply) {
    // **The epoch check made real for the Cabin's hints**
    // (docs/spec/physical-optimizer.md R4, workplan PX04). Every hint was
    // recorded at epoch 0; the page side moves here, as a relayout mover
    // will move it. A btree relation must notice per entry, resolve the pk,
    // and stamp the healed hint with the *bumped* epoch - a heal that wrote
    // 0 back would miss and re-heal forever. A heap relation must abandon
    // the Cabin and re-record from the walk. Either way no reply moves.
    Instance db(/*cabins=*/true);
    Load(db);
    DeclareCabins(db);
    std::vector<std::string> replies = RunAll(db, 3);

    ASSERT_GT(BumpEveryUserPage(db), 0u);

    const std::vector<std::string> after = RunAll(db, 2);
    replies.insert(replies.end(), after.begin(), after.end());
    ExpectSame(replies, Reference(5), "bumped page epoch");

    // The heal is stamped, not inferred: every surviving hint now carries
    // the bumped epoch, on the btree relation (healed in place) and the
    // heap one (re-recorded from the walk) alike.
    auto b_oid = db.catalog().FindTableOidByName("b");
    ASSERT_TRUE(b_oid.ok());
    auto h_oid = db.catalog().FindTableOidByName("h");
    ASSERT_TRUE(h_oid.ok());
    std::size_t checked = 0;
    for (const catalog::Oid oid : {b_oid.value(), h_oid.value()}) {
        auto access = db.catalog().InitTableAccess(oid);
        ASSERT_TRUE(access.ok());
        const std::uint64_t cabin_id = access.value()->CabinOn(1).id;
        ASSERT_NE(cabin_id, 0u);
        for (const char* sym : {"aaa", "bbb", "ccc", "ddd"}) {
            parser::AstValue value;
            value.type = parser::ValueType::kStr;
            value.str_val = sym;
            auto key = stats::MakeCabinKey(cabin_id, value);
            ASSERT_TRUE(key.has_value());
            std::vector<stats::CabinEntry>* entries = db.cabins().Find(*key);
            if (entries == nullptr) continue;
            for (const stats::CabinEntry& entry : *entries) {
                if (!entry.hint_valid()) continue;
                EXPECT_EQ(entry.page_epoch, 1u)
                    << "a hint kept a stale epoch after the resolve (" << sym << ")";
                ++checked;
            }
        }
    }
    ASSERT_GT(checked, 0u) << "no hint was checked; the test proves nothing";
}

TEST(CabinContractTest, DanglingPksChangeNoReply) {
    // **C2's third consequence.** A pk absent from the clustered tree can
    // never resurface under a new tuple (K1), so a dangling entry is dead
    // forever and must be skipped - never an error, and never a row.
    Instance db(/*cabins=*/true);
    Load(db);
    DeclareCabins(db);
    std::vector<std::string> replies = RunAll(db, 3);

    auto b_oid = db.catalog().FindTableOidByName("b");
    ASSERT_TRUE(b_oid.ok());
    auto access = db.catalog().InitTableAccess(b_oid.value());
    ASSERT_TRUE(access.ok());
    const std::uint64_t cabin_id = access.value()->CabinOn(1).id;

    std::size_t planted = 0;
    for (const char* sym : {"aaa", "bbb", "zzz"}) {
        parser::AstValue value;
        value.type = parser::ValueType::kStr;
        value.str_val = sym;
        auto key = stats::MakeCabinKey(cabin_id, value);
        ASSERT_TRUE(key.has_value());
        std::vector<stats::CabinEntry>* entries = db.cabins().Find(*key);
        if (entries == nullptr) continue;

        stats::CabinEntry dangling;
        dangling.pk = 999999;  // issued to nothing, and by K1 never will be
        dangling.page_id = kInvalidPageId;
        dangling.flags = 0;  // no usable hint: the pk is all there is
        entries->push_back(dangling);
        ++planted;
    }
    ASSERT_GT(planted, 0u) << "nothing was observed; the test proves nothing";

    const std::vector<std::string> after = RunAll(db, 2);
    replies.insert(replies.end(), after.begin(), after.end());
    ExpectSame(replies, Reference(5), "dangling pks");
}

// ---- The claim no advisory structure can make ----------------------------

TEST(CabinContractTest, AnObservedEmptyValueAnswersWithoutReadingTheRelation) {
    // §1's first corollary: an observed value's empty entry set is an
    // authoritative "no rows". This is the one behaviour that separates a
    // Cabin from every advisory structure in the engine, and the evidence is
    // **work not done** - so it is checked through ANALYZE's counters, which
    // is the only place work-not-done leaves a trace.
    Instance db(/*cabins=*/true);
    Load(db);
    DeclareCabins(db);

    // These Cabins were **declared** (`CREATE CABIN`), so they record at
    // n=1: the first execution misses - there was nothing to serve from -
    // and records, and the second is already served.
    const std::string sql = "ANALYZE SELECT * FROM b WHERE sym = 'zzz'";
    const std::string first = db.Run(sql);
    EXPECT_NE(first.find("cabin_misses=1"), std::string::npos) << first;
    EXPECT_NE(first.find("cabin_recorded=1"), std::string::npos) << first;

    const std::string second = db.Run(sql);
    EXPECT_NE(second.find("cabin_hits=1"), std::string::npos) << second;
    // The relation was not read: no tuple was decoded, and no row came back.
    // This is the claim no advisory structure can make, and the only trace
    // it leaves is the work not done.
    EXPECT_NE(second.find("examined=0"), std::string::npos) << second;
    EXPECT_NE(second.find("rows=0"), std::string::npos) << second;
}

TEST(CabinContractTest, ServingStopsReadingTheWholeRelation) {
    // The other half: a value that *does* match must stop scanning once it
    // is observed. Without this the feature could be "correct" and useless,
    // and every byte-for-byte test above would still pass.
    Instance db(/*cabins=*/true);
    Load(db);
    DeclareCabins(db);

    const std::string sql = "ANALYZE SELECT * FROM b WHERE sym = 'aaa'";
    db.Run(sql);
    db.Run(sql);
    const std::string served = db.Run(sql);
    // Three rows carry 'aaa' out of eight: a served execution decodes three.
    EXPECT_NE(served.find("cabin_hits=1"), std::string::npos) << served;
    EXPECT_NE(served.find("examined=3"), std::string::npos) << served;
    EXPECT_NE(served.find("cabin_entries=3"), std::string::npos) << served;
}

TEST(CabinContractTest, AnUpdateThatDoesNotTouchTheKeyColumnAppendsNothing) {
    // §5's third row, and the reason it is a rule rather than an
    // optimization. Appending on every UPDATE stays *correct* - the set
    // remains a superset and the read dedupes - but it is unbounded: a
    // workload that repeatedly updates a row's other columns would grow one
    // value's entry set by an entry per write until the per-value cap
    // un-observed it, and the Cabin would stop serving the relation it was
    // declared for. Correct and useless is still a defect.
    //
    // This is exactly the shape `tools/scenario0_stockmarket.py` drives: two
    // account UPDATEs per trade, neither touching the `user_id` the Cabin
    // is on.
    Instance db(/*cabins=*/true);
    Load(db);
    DeclareCabins(db);

    // Observe 'aaa' (declared, so one execution is enough).
    ASSERT_EQ(db.Run("SELECT * FROM b WHERE sym = 'aaa'").substr(0, 2), "id");

    auto oid = db.catalog().FindTableOidByName("b");
    ASSERT_TRUE(oid.ok());
    auto access = db.catalog().InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    const std::uint64_t cabin_id = access.value()->CabinOn(1).id;

    parser::AstValue aaa;
    aaa.type = parser::ValueType::kStr;
    aaa.str_val = "aaa";
    auto key = stats::MakeCabinKey(cabin_id, aaa);
    ASSERT_TRUE(key.has_value());
    ASSERT_NE(db.cabins().Find(*key), nullptr);
    const std::size_t before = db.cabins().Find(*key)->size();

    // Twenty writes to a row carrying the observed value, none of them
    // touching the key column.
    for (int i = 0; i < 20; ++i) {
        ASSERT_EQ(db.Run("UPDATE b SET qty = " + std::to_string(100 + i) + " WHERE id = 1"),
                  "UPDATED 1");
    }
    EXPECT_EQ(db.cabins().Find(*key)->size(), before) << "an unchanged key column appended";

    // And the row is still served, with its new value.
    const std::string served = db.Run("SELECT * FROM b WHERE sym = 'aaa'");
    EXPECT_NE(served.find("119"), std::string::npos) << served;

    // A write that *does* move the key column still appends - the rule is
    // "unchanged does nothing", not "UPDATE does nothing".
    ASSERT_EQ(db.Run("UPDATE b SET sym = 'aaa' WHERE id = 4"), "UPDATED 1");
    EXPECT_EQ(db.cabins().Find(*key)->size(), before + 1);
}

TEST(CabinContractTest, AQuotaStoppedWalkCommitsNoObservation) {
    // "Only a completed walk may be committed" had no SQL that could reach
    // it until V09 gave the sink a way to stop a walk mid-relation: a
    // filled LIMIT ends the scan on the row that filled it. A truncated
    // entry set committed as observed would be authoritative and *wrong* -
    // the next unlimited execution would serve one row where three exist,
    // the fewer-rows-plausible-answer failure §5 calls invisible without a
    // baseline.
    Instance db(/*cabins=*/true);
    Load(db);
    DeclareCabins(db);

    // The limited scan stops after the first of 'aaa''s three rows...
    ASSERT_EQ(db.Run("SELECT id FROM h WHERE sym = 'aaa' LIMIT 1"), "id\\n1");

    // ...and observed nothing: no entry set exists for the value, even
    // though the cabin is declared (n = 1) and the walk it stopped was
    // recording.
    auto oid = db.catalog().FindTableOidByName("h");
    ASSERT_TRUE(oid.ok());
    auto access = db.catalog().InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    parser::AstValue aaa;
    aaa.type = parser::ValueType::kStr;
    aaa.str_val = "aaa";
    auto key = stats::MakeCabinKey(access.value()->CabinOn(1).id, aaa);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(db.cabins().Find(*key), nullptr) << "a stopped walk was committed";

    // The completed walk answers with every row and is the one that
    // observes; a served execution afterwards - sliced or not - answers
    // the same bytes the walk did.
    const std::string all = db.Run("SELECT id FROM h WHERE sym = 'aaa'");
    EXPECT_EQ(all, "id\\n1\\n3\\n6");
    EXPECT_NE(db.cabins().Find(*key), nullptr);
    EXPECT_EQ(db.Run("SELECT id FROM h WHERE sym = 'aaa'"), all);
    EXPECT_EQ(db.Run("SELECT id FROM h WHERE sym = 'aaa' LIMIT 2"), "id\\n1\\n3");
    EXPECT_EQ(db.Run("SELECT id FROM h WHERE sym = 'aaa' LIMIT 1 OFFSET 2"), "id\\n6");

    // The btree twin, same sequence: the stopped walk commits nothing,
    // the completed one observes, and a *served* probe under a quota
    // resolves through the pk and slices identically.
    ASSERT_EQ(db.Run("SELECT id FROM b WHERE sym = 'aaa' LIMIT 1"), "id\\n1");
    EXPECT_EQ(db.Run("SELECT id FROM b WHERE sym = 'aaa'"), "id\\n1\\n3\\n6");
    EXPECT_EQ(db.Run("SELECT id FROM b WHERE sym = 'aaa' LIMIT 2"), "id\\n1\\n3");
}

TEST(CabinContractTest, RecordingIgnoresTheStatementsOtherConjuncts) {
    // **The subtlest way this feature could be wrong**, and it leaves no
    // trace anywhere else. The set recorded for a value must be the rows
    // whose *key column* equals it - not the rows the recording statement
    // wanted. A statement is `WHERE sym = 'aaa' AND qty > 30`; if its
    // narrowed result became the entry set, the next statement asking only
    // `WHERE sym = 'aaa'` would be served a set missing rows and told it was
    // authoritative. Every row it returned would be real, so nothing would
    // look wrong.
    //
    // The narrowed query goes first here precisely so it is the one that
    // records.
    Instance db(/*cabins=*/true);
    Instance ref(/*cabins=*/false);
    Load(db);
    Load(ref);
    DeclareCabins(db);

    const std::string narrow = "SELECT * FROM b WHERE sym = 'aaa' AND qty > 30";
    const std::string broad = "SELECT * FROM b WHERE sym = 'aaa'";
    ASSERT_EQ(db.Run(narrow), ref.Run(narrow));  // this execution records
    EXPECT_EQ(db.Run(broad), ref.Run(broad)) << "the narrowed query's set was served as complete";
    EXPECT_EQ(db.Run(broad), ref.Run(broad));

    // And the same on a heap relation, where serving falls back differently.
    const std::string heap_narrow = "SELECT * FROM h WHERE sym = 'bbb' AND qty = 20";
    const std::string heap_broad = "SELECT * FROM h WHERE sym = 'bbb'";
    ASSERT_EQ(db.Run(heap_narrow), ref.Run(heap_narrow));
    EXPECT_EQ(db.Run(heap_broad), ref.Run(heap_broad));

    // Stated as a count too, so a future change that starts filtering the
    // recording fails loudly rather than only under comparison: three rows
    // carry 'aaa', and the set holds all three though the recorder emitted
    // one.
    const std::string served = db.Run("ANALYZE " + broad);
    EXPECT_NE(served.find("cabin_entries=3"), std::string::npos) << served;
}

TEST(CabinContractTest, ACabinIsNotTrailReplayable) {
    // Invariant 9's line is *lookup versus search*, not *authoritative
    // versus advisory*. A cabin probe is authoritative and still must not be
    // replayable from a trail - this is the check that the two trust models
    // did not quietly merge.
    EXPECT_FALSE(exec::IsTrailReplayable(exec::AccessKind::kCabinProbe));
    EXPECT_FALSE(exec::IsTrailReplayable(exec::AccessKind::kFilterScan));
    EXPECT_FALSE(exec::IsTrailReplayable(exec::AccessKind::kRange));
    EXPECT_FALSE(exec::IsTrailReplayable(exec::AccessKind::kScan));
    // A secondary index is authoritative too, and lands on the same side of
    // the line for the same reason (docs/spec/index.md §8): an index probe
    // answers with a *set*, and a trail has no witness for a row inserted
    // since it was recorded.
    EXPECT_FALSE(exec::IsTrailReplayable(exec::AccessKind::kIndexProbe));
    EXPECT_FALSE(exec::IsTrailReplayable(exec::AccessKind::kIndexRange));
    EXPECT_TRUE(exec::IsTrailReplayable(exec::AccessKind::kLookup));
    EXPECT_TRUE(exec::IsTrailReplayable(exec::AccessKind::kProbe));
}

// ---- DDL ------------------------------------------------------------------

TEST(CabinContractTest, DdlRefusesWhatCanNeverWork) {
    Instance db(/*cabins=*/true);
    Load(db);

    EXPECT_EQ(db.Run("CREATE CABIN ON b(id)").substr(0, 3), "ERR");   // the pk
    EXPECT_EQ(db.Run("CREATE CABIN ON b(nope)").substr(0, 3), "ERR");  // no column
    EXPECT_EQ(db.Run("CREATE CABIN ON nope(sym)").substr(0, 3), "ERR");  // no relation
    EXPECT_EQ(db.Run("CREATE CABIN ON b(sym, qty)").substr(0, 3), "ERR");  // C3
    EXPECT_EQ(db.Run("CREATE CABIN b(sym)").substr(0, 3), "ERR");  // missing ON
    EXPECT_EQ(db.Run("DROP CABIN ON b(sym)").substr(0, 3), "ERR");  // none exists

    ASSERT_EQ(db.Run("CREATE CABIN ON b(sym)").substr(0, 7), "CREATED");
    EXPECT_EQ(db.Run("CREATE CABIN ON b(sym)").substr(0, 3), "ERR");  // duplicate
    EXPECT_EQ(db.Run("DROP CABIN ON b(sym)").substr(0, 7), "DROPPED");
}

TEST(CabinContractTest, ColumnPolicyDecidesWhoMayCreateACabin) {
    // C7 (docs/spec/cabin.md §8.1). Three policies, three behaviours, and
    // the one that matters most is `NO CABIN`: it must be refused at the
    // catalog, not merely absent from the grammar, because that is the door
    // every future auto-creator will also come through.
    Instance db(/*cabins=*/true);
    ASSERT_EQ(db.Run("CREATE TABLE p (id int64, a varchar CABIN, b varchar NO CABIN, "
                     "c varchar CABIN AUTO, d varchar)")
                  .substr(0, 7),
              "CREATED");

    // `CABIN` created one already; the other three did not.
    const std::string listed = db.Run("SHOW CABINS");
    EXPECT_NE(listed.find("cabins=1"), std::string::npos) << listed;
    EXPECT_NE(listed.find("column=a"), std::string::npos) << listed;

    EXPECT_EQ(db.Run("CREATE CABIN ON p(b)").substr(0, 3), "ERR");  // disabled
    EXPECT_EQ(db.Run("CREATE CABIN ON p(c)").substr(0, 7), "CREATED");  // auto permits asking
    EXPECT_EQ(db.Run("CREATE CABIN ON p(d)").substr(0, 7), "CREATED");  // unset reads as auto

    // A policy on the pk is refused, not ignored.
    EXPECT_EQ(db.Run("CREATE TABLE q (id int64 CABIN, x varchar)").substr(0, 3), "ERR");
    EXPECT_EQ(db.Run("CREATE TABLE r (id int64 NO CABIN, x varchar)").substr(0, 3), "ERR");

    // And DESCRIBE reports the effective policy per column.
    const std::string described = db.Run("DESCRIBE p");
    EXPECT_NE(described.find("name=a type=varchar notnull=yes pk=no autoincrement=no "
                             "cabin=yes"),
              std::string::npos)
        << described;
    EXPECT_NE(described.find("name=b type=varchar notnull=yes pk=no autoincrement=no "
                             "cabin=no"),
              std::string::npos)
        << described;
    EXPECT_NE(described.find("name=c type=varchar notnull=yes pk=no autoincrement=no "
                             "cabin=auto"),
              std::string::npos)
        << described;
}

TEST(CabinContractTest, ADeclaredCabinObservesOnFirstSelection) {
    // C7's n=1 half. A `CABIN` column's values are observed on their first
    // selection, where an engine-created Cabin would wait for the second -
    // the same split `CREATE PATTERN` settled, on the same argument.
    Instance db(/*cabins=*/true);
    ASSERT_EQ(db.Run("CREATE TABLE d (id int64, sym varchar CABIN, qty int64)").substr(0, 7),
              "CREATED");
    ASSERT_EQ(db.Run("INSERT INTO d VALUES ('aaa', 1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(db.Run("INSERT INTO d VALUES ('aaa', 2)").substr(0, 8), "INSERTED");

    const std::string sql = "ANALYZE SELECT * FROM d WHERE sym = 'aaa'";
    const std::string first = db.Run(sql);
    // First execution: still a miss - there was nothing to serve from - but
    // it records rather than merely counting.
    EXPECT_NE(first.find("cabin_recorded=1"), std::string::npos) << first;

    const std::string second = db.Run(sql);
    EXPECT_NE(second.find("cabin_hits=1"), std::string::npos) << second;
}

TEST(CabinContractTest, TwoCabinStepsInOneChainNeitherShareBuffersNorCancelEachOther) {
    // **The nested case**, which the shared query set cannot reach because
    // every statement in it opens one relation. A join whose both sides are
    // cabined puts two cabin steps in one chain, and the inner one runs
    // inside the outer one's recording walk and inside its serve loop. Two
    // pieces of runner state were live on two frames at once:
    //
    //   - `recording_`, cleared to null on the inner walk's way out, which
    //     cancelled the outer recording from its second row - committing a
    //     set as observed while missing qualifying pks, exactly the C1
    //     break the completed-walk check exists to prevent;
    //   - the phase-2 location buffer, cleared and refilled by the inner
    //     serve while the outer serve was walking it.
    //
    // Either one alone loses rows, and a Cabin that loses rows is being
    // treated as authoritative while disagreeing with storage.
    Instance on(/*cabins=*/true);
    Instance off(/*cabins=*/false);
    const char* kDecl = " (id int64, sym varchar, qty int64) BTREE";
    for (Instance* db : {&on, &off}) {
        ASSERT_EQ(db->Run(std::string("CREATE TABLE p") + kDecl).substr(0, 7), "CREATED");
        ASSERT_EQ(db->Run(std::string("CREATE TABLE q") + kDecl).substr(0, 7), "CREATED");
        // Deliberately different set sizes and different pks: two relations
        // whose sets happen to agree would hide a buffer served from the
        // wrong step.
        for (int i = 1; i <= 4; ++i) {
            const std::string qty = std::to_string(i);
            ASSERT_EQ(db->Run("INSERT INTO p VALUES ('x', " + qty + ")").substr(0, 8), "INSERTED");
        }
        for (int i = 1; i <= 6; ++i) {
            const std::string qty = std::to_string(i);
            ASSERT_EQ(db->Run("INSERT INTO q VALUES ('z', " + qty + ")").substr(0, 8), "INSERTED");
        }
        ASSERT_EQ(db->Run("INSERT INTO q VALUES ('y', 3)").substr(0, 8), "INSERTED");
        ASSERT_EQ(db->Run("CREATE CABIN ON p(sym)").substr(0, 7), "CREATED");
        ASSERT_EQ(db->Run("CREATE CABIN ON q(sym)").substr(0, 7), "CREATED");
    }

    const std::string sql =
        "SELECT a.id, c.id FROM p AS a JOIN q AS c ON a.qty = c.qty "
        "WHERE a.sym = 'x' AND c.sym = 'y'";
    // Warmed past the n=2 threshold so the *serve* path runs, not only the
    // recording walk - the two defects live one on each side of it.
    for (int i = 0; i < 4; ++i) {
        on.Run(sql);
        off.Run(sql);
    }
    EXPECT_EQ(on.Run(sql), off.Run(sql));
}

TEST(CabinContractTest, AWriteHookAppendServesInPkOrder) {
    // **The ordering rule, minimised to one relation and one write.** A
    // walk emits a step's rows in pk order (I12), and a committed set
    // starts that way - but the write hook appends at the set's *end*, so
    // an UPDATE moving an earlier pk into an observed value leaves the set
    // out of order. Serving it in entry order then reorders a reply, which
    // is an accelerator changing a query result (§1).
    //
    // Reachable by plain single-relation SQL, which is why it lived
    // undetected: the query set above never exposed a whole multi-row set
    // for a value a write had moved into. Both clustering types, because
    // the CB12 join queries that found this catch the heap half only.
    Instance db(/*cabins=*/true);
    Instance ref(/*cabins=*/false);
    Load(db);
    Load(ref);
    DeclareCabins(db);

    // 'ccc' is one row, pk 4. Declared, so this execution observes it.
    const std::string sql = "SELECT id FROM b WHERE sym = 'ccc'";
    ASSERT_EQ(db.Run(sql), ref.Run(sql));

    // pk 1 moves in, and lands after pk 4 in the entry set.
    const std::string write = "UPDATE b SET sym = 'ccc' WHERE id = 1";
    ASSERT_EQ(db.Run(write), ref.Run(write));
    EXPECT_EQ(db.Run(sql), ref.Run(sql)) << "btree: a served set emitted out of pk order";

    // The heap twin. Same append; the serve path differs (no descent to
    // heal a hint with), the ordering rule does not.
    const std::string heap_sql = "SELECT id FROM h WHERE sym = 'ccc'";
    ASSERT_EQ(db.Run(heap_sql), ref.Run(heap_sql));
    const std::string heap_write = "UPDATE h SET sym = 'ccc' WHERE id = 1";
    ASSERT_EQ(db.Run(heap_write), ref.Run(heap_write));
    EXPECT_EQ(db.Run(heap_sql), ref.Run(heap_sql))
        << "heap: a served set emitted out of pk order";
}

TEST(CabinContractTest, AnExplicitKeyRelationServesTheOrderItWalks) {
    // The other half of the ordering rule, and the half a pk sort gets
    // wrong. Under `EXPLICIT` (heap-and-tuple.md §4.1) a caller-supplied id
    // need not ascend, so a page's slots - always in *insertion* order -
    // are not in key order, and the walk emits them as they lie. A serve
    // that sorted by pk would answer one order on the recording execution
    // and another on every execution after it, with the cabin-free
    // baseline agreeing with neither.
    //
    // Five rows, descending ids, one page: walk order is the reverse of pk
    // order, which is what makes the two distinguishable at all.
    Instance db(/*cabins=*/true);
    Instance base(/*cabins=*/false);
    for (Instance* d : {&db, &base}) {
        ASSERT_EQ(d->Run("CREATE TABLE e (id int64, sym varchar, qty int64) BTREE EXPLICIT")
                      .substr(0, 7),
                  "CREATED");
        for (int id : {50, 40, 30, 20, 10}) {
            const std::string n = std::to_string(id);
            ASSERT_EQ(d->Run("INSERT INTO e VALUES (" + n + ", 'aaa', " + n + ")").substr(0, 8),
                      "INSERTED")
                << id;
        }
    }
    ASSERT_EQ(db.Run("CREATE CABIN ON e(sym)").substr(0, 7), "CREATED");

    const std::string sql = "SELECT id FROM e WHERE sym = 'aaa'";
    const std::string want = base.Run(sql);
    EXPECT_EQ(db.Run(sql), want) << "the recording walk";
    EXPECT_EQ(db.Run(sql), want) << "the served execution reordered the reply";
    EXPECT_EQ(db.Run(sql), want) << "the served execution reordered the reply";

    // And the clause that *does* ask for pk order still gets it, from the
    // sink rather than from the entry set.
    const std::string ordered = sql + " ORDER BY id";
    EXPECT_EQ(db.Run(ordered), base.Run(ordered));
}

TEST(CabinContractTest, ACorrelatedExistsConvergesToObservedSets) {
    // cabin.md §4a's non-convergence, closed: an EXISTS whose every
    // outer key has a qualifying match stops each recording walk before it
    // can commit, so the probed values re-observed forever and every
    // execution paid the full miss path. In sub-chain mode the walk now
    // completes *through* the stop - the answer is already decided, the
    // remaining rows feed the recording alone - and the set commits.
    Instance db(/*cabins=*/true);
    Load(db);
    DeclareCabins(db);

    // Every sym has a row with qty > 30 ('aaa' has 60, 'bbb' 80, 'ccc' 40,
    // 'ddd' 70), so pre-fix no key ever recorded.
    const std::string q =
        "SELECT id FROM h AS o WHERE EXISTS "
        "(SELECT i.id FROM h AS i WHERE i.sym = o.sym AND i.qty > 30)";
    const std::string first = db.Run(q);
    ASSERT_EQ(first.rfind("ERR", 0), std::string::npos) << first;
    // The correlated form earns observation per key (§4a), so the ladder
    // is per key and not per execution: 'aaa' and 'bbb' repeat *within*
    // one execution (outer rows 1/3/6 and 2/5/8) and record on their
    // second outer row; 'ccc' and 'ddd' are touched once per execution
    // and need this second one. Both record through the stops.
    EXPECT_EQ(db.Run(q), first);

    // The witness: after the second execution the probed values are
    // observed sets, not eternal misses - the completed-through-stop walk
    // is what lets each commit.
    auto oid = db.catalog().FindTableOidByName("h", nullptr);
    ASSERT_TRUE(oid.ok());
    auto access = db.catalog().InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    const std::uint64_t cabin_id = access.value()->CabinOn(/*col_pos=*/1).id;
    ASSERT_NE(cabin_id, 0u);
    for (const char* sym : {"aaa", "bbb", "ccc", "ddd"}) {
        parser::AstValue v;
        v.type = parser::ValueType::kStr;
        v.str_val = sym;
        auto key = stats::MakeCabinKey(cabin_id, v);
        ASSERT_TRUE(key.has_value());
        EXPECT_NE(db.cabins().Find(*key), nullptr) << sym;
    }

    // And the served executions answer exactly what the first did.
    for (int r = 0; r < 3; ++r) EXPECT_EQ(db.Run(q), first);

    // The completed set is whole, not the prefix the stop saw: a probe of
    // a recorded value returns every matching row byte-identically to a
    // cabin-free walk.
    //
    // **'bbb' is the value that discriminates**, which is why every sym is
    // probed rather than one. Its rows are 2, 5 and 8 and the EXISTS stop
    // landed on 5 (qty 50, the first over 30), so a set committed at the
    // stop would be missing row 8 - the C1 break. 'aaa' cannot show that:
    // its stop lands on 6, the last of its rows, so its prefix and its
    // whole set are the same list and a partial commit would pass.
    Instance ref(/*cabins=*/false);
    Load(ref);
    for (const char* sym : {"aaa", "bbb", "ccc", "ddd"}) {
        const std::string probe = std::string("SELECT * FROM h WHERE sym = '") + sym + "'";
        EXPECT_EQ(db.Run(probe), ref.Run(probe)) << sym;
    }
}

TEST(CabinContractTest, ACapRefusedValueKeepsTheShortCircuitedCost) {
    // The review's reproduction of the completion license's cap hole,
    // pinned: with caps a value cannot fit under, CB13's walk-through-stops
    // must not run - each doomed attempt was a full relation walk, re-armed
    // on every probe, and under a tight row budget the cabined database
    // answered ResourceExhausted where the cabin-free one answered rows.
    // An accelerator changing a result is what §1 forbids outright.
    //
    // Caps: 'aaa' and 'bbb' have three rows each, past the entry cap of 2;
    // the value cap of 2 lets at most two values observe at all. The
    // per-statement budget of 60 rows bounds one execution comfortably
    // when every stop ends its walk (~25 examined), and not when doomed
    // completions walk the whole relation per outer row (8 x 8 and up).
    stats::CabinLimits limits;
    limits.max_values = 2;
    limits.max_entries_per_value = 2;
    Instance db(/*cabins=*/true, limits, exec::Budget(60));
    Instance ref(/*cabins=*/false, {}, exec::Budget(60));
    Load(db);
    Load(ref);
    DeclareCabins(db);

    const std::string q =
        "SELECT id FROM h AS o WHERE EXISTS "
        "(SELECT i.id FROM h AS i WHERE i.sym = o.sym AND i.qty > 30)";
    for (int r = 0; r < 4; ++r) {
        const std::string got = db.Run(q);
        const std::string want = ref.Run(q);
        ASSERT_EQ(got.rfind("ERR", 0), std::string::npos)
            << "round " << r << ": the cabined side burned budget on doomed walks: " << got;
        EXPECT_EQ(got, want) << "round " << r;
    }

    // The teeth (re-armed 2026-08-19 - review R1): the budget assertion
    // above went hollow the moment completion rows stopped charging it,
    // which is itself correct - so the bound moves to the work counter,
    // which completion rows still honestly increment. With the gates in
    // place a probe of a cap-refused value ends at the sub-chain's own
    // stop; with either gate removed the doomed completions walk the
    // relation per outer row and `examined=` says so. Measured with the
    // gates: ~30 inner rows; with them removed: 65+.
    const std::string plan = db.Run("ANALYZE " + q);
    const std::size_t at = plan.find("examined=");
    ASSERT_NE(at, std::string::npos) << plan;
    const long examined = std::strtol(plan.c_str() + at + 9, nullptr, 10);
    EXPECT_LT(examined, 45) << "doomed completion walks ran: " << plan;
}

TEST(CabinContractTest, CorrelatedScansCountWalksNotKinds) {
    // The counter's execution-level definition (step_vm.hpp): a sub-chain
    // driving step that actually walked. Three facts pinned, each a gap of
    // the old compile-time kind test:
    Instance db(/*cabins=*/true);
    Load(db);
    DeclareCabins(db);

    // 1. A cabined EXISTS's early executions walk per outer row - the
    //    first sights, the second records (§4a's per-key n = 2) - and the
    //    counter says so both times.
    const std::string q =
        "ANALYZE SELECT id FROM h AS o WHERE EXISTS "
        "(SELECT i.id FROM h AS i WHERE i.sym = o.sym)";
    EXPECT_NE(db.Run(q).find("corr_scans="), std::string::npos);
    EXPECT_NE(db.Run(q).find("corr_scans="), std::string::npos);

    // 2. Once converged, every probe serves and nothing walks - the
    //    counter goes quiet exactly when the quadratic work does. (A zero
    //    counter is not printed.)
    const std::string third = db.Run(q);
    EXPECT_EQ(third.find("corr_scans="), std::string::npos) << third;

    // 3. A kFilterScan driver - a literal beside the correlation, no cabin
    //    involved - walks every evaluation and now counts, where the old
    //    kind test never counted it at all.
    Instance plain(/*cabins=*/true);
    Load(plain);
    const std::string fs =
        "ANALYZE SELECT id FROM b AS o WHERE EXISTS "
        "(SELECT i.id FROM b AS i WHERE i.qty = 45 AND i.sym = o.sym)";
    EXPECT_NE(plain.Run(fs).find("corr_scans="), std::string::npos);
    EXPECT_NE(plain.Run(fs).find("corr_scans="), std::string::npos);  // never converges
}

TEST(CabinContractTest, ANeverRepeatingKeyObservesNothing) {
    // §7b.8's uncovered distribution, closed on the Cabin side: a join
    // whose outer keys never repeat used to record a set - and its
    // forever write-hook tax - for every first touch, because a declared
    // Cabin's n = 1 spoke for values the operator never named. The
    // correlated form now earns observation per key: one execution of a
    // join touching a key once leaves it unobserved, and the reply is
    // byte-identical to the cabin-free walk.
    Instance db(/*cabins=*/true);
    Instance ref(/*cabins=*/false);
    Load(db);
    Load(ref);
    DeclareCabins(db);

    // 'ccc' has one row (id 4), so x.id = 4 probes it exactly once.
    const std::string q =
        "SELECT x.id, y.id FROM h AS x JOIN h AS y ON y.sym = x.sym "
        "WHERE x.id BETWEEN 4 AND 4";
    EXPECT_EQ(db.Run(q), ref.Run(q));

    auto oid = db.catalog().FindTableOidByName("h", nullptr);
    ASSERT_TRUE(oid.ok());
    auto access = db.catalog().InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    const std::uint64_t cabin_id = access.value()->CabinOn(/*col_pos=*/1).id;
    parser::AstValue v;
    v.type = parser::ValueType::kStr;
    v.str_val = "ccc";
    auto key = stats::MakeCabinKey(cabin_id, v);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(db.cabins().Find(*key), nullptr) << "a once-touched key must not record";

    // And the literal form is untouched: the operator named this value,
    // and the declaration's n = 1 still records it on the first miss.
    //
    // **'ddd', not 'ccc'** - the join above already sighted 'ccc' once, so
    // a literal probe of it arrives at count 2 and would record even at
    // n = 2. 'ddd' (one row, id 7) is untouched by the `x.id = 4` join, so
    // this is a genuine first touch and the assertion is the A/B this test
    // exists to make: same instance, same cabin, same single-row key
    // shape, differing only in probe form.
    parser::AstValue lit;
    lit.type = parser::ValueType::kStr;
    lit.str_val = "ddd";
    auto lit_key = stats::MakeCabinKey(cabin_id, lit);
    ASSERT_TRUE(lit_key.has_value());
    ASSERT_EQ(db.cabins().Find(*lit_key), nullptr);
    EXPECT_EQ(db.Run("SELECT * FROM h WHERE sym = 'ddd'"),
              ref.Run("SELECT * FROM h WHERE sym = 'ddd'"));
    EXPECT_NE(db.cabins().Find(*lit_key), nullptr)
        << "the declared literal probe records first-touch";
}

}  // namespace
}  // namespace kds::server

