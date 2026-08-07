#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// **The cabin contract** (docs/feat-cabin.md, workplan CB10). The Waystone
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

// One database, one dispatcher, one configuration.
class Instance {
public:
    explicit Instance(bool cabins) {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        EXPECT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        if (cabins) cabins_.emplace();
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, cabins_ ? &*cabins_ : nullptr);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    catalog::Catalog& catalog() { return boot_->catalog; }
    stats::CabinStore& cabins() { return *cabins_; }

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
    // C7 (docs/feat-cabin.md §8.1). Three policies, three behaviours, and
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

}  // namespace
}  // namespace kds::server
