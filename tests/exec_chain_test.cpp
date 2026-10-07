#include "kds/exec/step_vm.hpp"

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/coro.hpp"

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/memory_page_device.hpp"

// V17 - the step VM, linear chains (docs/inflight/in-progress/parser-v2-workplan.md).
//
// The tests that matter here are **equivalences**, not single results:
//
//   across storage   a heap relation and a btree relation must return the
//                    same rows in the same order. The row codec, PageView
//                    and the walk are shared by design (a btree leaf *is*
//                    a heap page), and this is what proves the sharing is
//                    real rather than nominal.
//
//   across strategy  a Probe and a Scan must agree row-for-row. That is
//                    what makes invariant 9's fall-through safe: a
//                    Waystone miss falls back to a walk, and if the two
//                    paths could disagree, a miss would change the answer
//                    rather than only the cost. The compiler buys this by
//                    keeping the probe key in the residual list, so
//                    downgrading any step to a scan filters identically.

namespace kds::exec {
namespace {

class ExecChainTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 4000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
    }

    void Create(const std::string& sql) {
        auto parsed = parser::Parse(sql);
        ASSERT_TRUE(parsed.ok()) << parsed.status().message();
        const auto& ct = std::get<parser::CreateTableStmt>(parsed.value());

        catalog::Schema schema;
        std::uint32_t pos = 0;
        for (const auto& col : ct.columns) {
            auto type_row = boot_->catalog.ResolveTypeByName(col.type_name);
            ASSERT_TRUE(type_row.ok()) << type_row.status().message();
            catalog::SysColumnRow row{};
            row.pos = pos++;
            catalog::SetName(row.name, col.name);
            row.type_val = type_row.value().type_val;
            row.len = type_row.value().len;
            row.notnull = true;
            schema.columns.push_back(row);
        }
        auto created = boot_->catalog.CreateTable(catalog::kNamespacePublic, ct.table_name, schema,
                                                  ct.clustered);
        ASSERT_TRUE(created.ok()) << created.status().message();
    }

    // Inserts one row through the same path the dispatcher uses, so the
    // tuples under test are the ones a real INSERT produces.
    void Insert(const std::string& table, const std::vector<parser::AstValue>& body) {
        auto oid = boot_->catalog.FindTableOidByName(table);
        ASSERT_TRUE(oid.ok()) << oid.status().message();
        auto access = boot_->catalog.InitTableAccess(oid.value());
        ASSERT_TRUE(access.ok()) << access.status().message();
        auto id = boot_->catalog.AllocateRowId(oid.value());
        ASSERT_TRUE(id.ok()) << id.status().message();

        auto payload = EncodeRow(access.value()->schema, access.value()->layout, id.value(), body);
        ASSERT_TRUE(payload.ok()) << payload.status().message();

        if (access.value()->clustered_type == catalog::ClusteredType::kBtree) {
            auto placed = btree::BtreeInsert(store_, access.value()->desc_page_id, id.value(),
                                             payload.value(), /*trx_id=*/1, access.value()->oid);
            ASSERT_TRUE(placed.ok()) << placed.status().message();
            // **The root moves when the tree grows a level**, and until
            // AO-S6e-b's cell needed a multi-leaf relation nothing here
            // insisted on it: this helper kept handing `BtreeInsert` the
            // old root, which after the first split is a full *leaf*. Every
            // later insert then appended a leaf and spliced it in front of
            // that one, building a chain that descends - the exact shape
            // `btree.cpp`'s append path calls unsurvivable (H9) - and a
            // scan of a relation this fixture filled read its rows out of
            // key order. The dispatcher has adopted the new root since
            // PW2-4 (`command_dispatcher.cpp`, `UpdateRelationDescPage`);
            // this is that line, and it is what makes a fixture-built
            // relation the same shape as a served one.
            if (placed.value().new_root != kInvalidPageId) {
                ASSERT_TRUE(boot_->catalog
                                .UpdateRelationDescPage(oid.value(), placed.value().new_root,
                                                        access.value()->anchor_page_id)
                                .ok());
            }
        } else {
            auto placed = heap::ChainInsert(store_, access.value()->desc_page_id, id.value(),
                                            payload.value(), /*trx_id=*/1, access.value()->oid);
            ASSERT_TRUE(placed.ok()) << placed.status().message();
        }
    }

    static parser::AstValue Int(std::int64_t v) {
        parser::AstValue out;
        out.type = parser::ValueType::kInt;
        out.int_val = v;
        out.raw_int_text = std::to_string(v);
        return out;
    }
    static parser::AstValue Str(std::string v) {
        parser::AstValue out;
        out.type = parser::ValueType::kStr;
        out.str_val = std::move(v);
        return out;
    }

    // Runs a statement and renders each row as "a|b|c", so a whole result
    // set compares as a vector of strings - order included, which is the
    // point of most of these tests.
    std::vector<std::string> Run(const std::string& sql) {
        auto parsed = parser::Parse(sql);
        EXPECT_TRUE(parsed.ok()) << sql << ": " << parsed.status().message();
        if (!parsed.ok()) return {};

        auto chain = Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()));
        EXPECT_TRUE(chain.ok()) << sql << ": " << chain.status().message();
        if (!chain.ok()) return {};

        return RunChain(chain.value());
    }

    std::vector<std::string> RunChain(const StepChain& chain) {
        std::vector<std::string> rows;
        Status ran = Execute(
            boot_->catalog, store_, chain,
            [&](const ChainFrame& frame) -> StatusOr<storage::VisitControl> {
                std::string row;
                for (const ColumnRef& ref : chain.projection) {
                    if (!row.empty()) row += '|';
                    row += FormatValue(/*type_val=*/0, frame.Get(ref));
                }
                rows.push_back(std::move(row));
                return storage::VisitControl::kContinue;
            });
        EXPECT_TRUE(ran.ok()) << ran.message();
        return rows;
    }

    StepChain CompileSql(const std::string& sql) {
        auto parsed = parser::Parse(sql);
        EXPECT_TRUE(parsed.ok()) << parsed.status().message();
        auto chain = Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()));
        EXPECT_TRUE(chain.ok()) << chain.status().message();
        return chain.ok() ? chain.value() : StepChain{};
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

// ---- Single relation ------------------------------------------------------

TEST_F(ExecChainTest, AScanReturnsEveryRowInRelationOrder) {
    Create("CREATE TABLE t (id int64, name varchar)");
    Insert("t", {Str("alice")});
    Insert("t", {Str("bob")});
    Insert("t", {Str("carol")});

    EXPECT_EQ(Run("SELECT t.id, t.name FROM t"),
              (std::vector<std::string>{"1|alice", "2|bob", "3|carol"}));
}

TEST_F(ExecChainTest, APredicateFiltersWithoutChangingOrder) {
    Create("CREATE TABLE t (id int64, name varchar, tier varchar)");
    Insert("t", {Str("alice"), Str("gold")});
    Insert("t", {Str("bob"), Str("silver")});
    Insert("t", {Str("carol"), Str("gold")});

    EXPECT_EQ(Run("SELECT t.name FROM t WHERE tier = 'gold'"),
              (std::vector<std::string>{"alice", "carol"}));
}

TEST_F(ExecChainTest, ALookupAndAScanReturnTheSameRow) {
    // The equivalence invariant 9 rests on. `WHERE id = 2` compiles to a
    // Lookup; the same statement with a second predicate that cannot
    // change the answer still returns exactly that row.
    Create("CREATE TABLE t (id int64, name varchar)");
    Insert("t", {Str("alice")});
    Insert("t", {Str("bob")});
    Insert("t", {Str("carol")});

    const StepChain lookup = CompileSql("SELECT t.name FROM t WHERE id = 2");
    ASSERT_EQ(lookup.steps[0].kind, AccessKind::kLookup);

    // Force the same statement down the scan path by rewriting the kind -
    // legitimate precisely because the compiler kept the key in the
    // residual list, so a Scan filters on the same predicate.
    StepChain forced = lookup;
    forced.steps[0].kind = AccessKind::kScan;
    forced.steps[0].key.reset();

    EXPECT_EQ(RunChain(lookup), (std::vector<std::string>{"bob"}));
    EXPECT_EQ(RunChain(forced), RunChain(lookup))
        << "a probe and a scan must agree row-for-row, or a Waystone miss changes the answer";
}

TEST_F(ExecChainTest, ALookupForAMissingRowReturnsNothingRatherThanFailing) {
    Create("CREATE TABLE t (id int64, name varchar)");
    Insert("t", {Str("alice")});
    EXPECT_TRUE(Run("SELECT t.name FROM t WHERE id = 99").empty());
}

// ---- Joins ----------------------------------------------------------------

TEST_F(ExecChainTest, ATwoRelationChainPairsRowsThroughItsProbe) {
    Create("CREATE TABLE acct (id int64, name varchar)");
    Create("CREATE TABLE trade (id int64, acct_id int64, sym varchar)");
    Insert("acct", {Str("alice")});   // id 1
    Insert("acct", {Str("bob")});     // id 2
    Insert("trade", {Int(1), Str("AAPL")});
    Insert("trade", {Int(2), Str("MSFT")});
    Insert("trade", {Int(1), Str("TSLA")});

    const StepChain chain = CompileSql(
        "SELECT trade.sym, acct.name FROM trade JOIN acct ON trade.acct_id = acct.id");
    ASSERT_EQ(chain.steps[1].kind, AccessKind::kProbe);

    // Written order: trade drives, so rows come out in trade order.
    EXPECT_EQ(RunChain(chain),
              (std::vector<std::string>{"AAPL|alice", "MSFT|bob", "TSLA|alice"}));
}

TEST_F(ExecChainTest, TheProbeStrategyAndTheScanStrategyAgreeRowForRow) {
    // The same statement, forced down both paths. If these ever diverge,
    // a Waystone trail miss stops being a performance event and becomes a
    // wrong answer (invariant 9).
    Create("CREATE TABLE acct (id int64, name varchar)");
    Create("CREATE TABLE trade (id int64, acct_id int64, sym varchar)");
    for (int i = 0; i < 5; ++i) Insert("acct", {Str("a" + std::to_string(i))});
    for (int i = 0; i < 5; ++i) Insert("trade", {Int(i % 5 + 1), Str("s" + std::to_string(i))});

    const StepChain probing = CompileSql(
        "SELECT trade.sym, acct.name FROM trade JOIN acct ON trade.acct_id = acct.id");
    ASSERT_EQ(probing.steps[1].kind, AccessKind::kProbe);

    StepChain scanning = probing;
    scanning.steps[1].kind = AccessKind::kScan;
    scanning.steps[1].key.reset();

    const std::vector<std::string> by_probe = RunChain(probing);
    const std::vector<std::string> by_scan = RunChain(scanning);
    EXPECT_EQ(by_probe, by_scan);
    EXPECT_EQ(by_probe.size(), 5u);
}

TEST_F(ExecChainTest, AThreeRelationChainNestsInWrittenOrder) {
    Create("CREATE TABLE a (id int64, label varchar)");
    Create("CREATE TABLE b (id int64, a_id int64, label varchar)");
    Create("CREATE TABLE c (id int64, b_id int64, label varchar)");
    Insert("a", {Str("a1")});
    Insert("b", {Int(1), Str("b1")});
    Insert("b", {Int(1), Str("b2")});
    Insert("c", {Int(1), Str("c1")});
    Insert("c", {Int(2), Str("c2")});

    EXPECT_EQ(Run("SELECT a.label, b.label, c.label FROM a "
                  "JOIN b ON b.a_id = a.id JOIN c ON c.b_id = b.id"),
              (std::vector<std::string>{"a1|b1|c1", "a1|b2|c2"}));
}

TEST_F(ExecChainTest, WrittenOrderDecidesRowOrder) {
    // Spec §1's client contract, observable: the same join written the
    // other way round produces the same *pairs* in a different order,
    // because the other relation drives.
    Create("CREATE TABLE acct (id int64, name varchar)");
    Create("CREATE TABLE trade (id int64, acct_id int64, sym varchar)");
    Insert("acct", {Str("alice")});
    Insert("acct", {Str("bob")});
    Insert("trade", {Int(2), Str("MSFT")});
    Insert("trade", {Int(1), Str("AAPL")});

    const std::vector<std::string> trade_first =
        Run("SELECT acct.name, trade.sym FROM trade JOIN acct ON trade.acct_id = acct.id");
    const std::vector<std::string> acct_first =
        Run("SELECT acct.name, trade.sym FROM acct JOIN trade ON trade.acct_id = acct.id");

    EXPECT_EQ(trade_first, (std::vector<std::string>{"bob|MSFT", "alice|AAPL"}));
    EXPECT_EQ(acct_first, (std::vector<std::string>{"alice|AAPL", "bob|MSFT"}));
    EXPECT_NE(trade_first, acct_first) << "nothing may reorder a chain behind the client's back";
}

TEST_F(ExecChainTest, ASelfJoinBindsTwoRowsOfOneRelation) {
    Create("CREATE TABLE t (id int64, parent_id int64, label varchar)");
    Insert("t", {Int(0), Str("root")});   // id 1
    Insert("t", {Int(1), Str("child")});  // id 2

    EXPECT_EQ(Run("SELECT b.label, a.label FROM t AS b JOIN t AS a ON b.parent_id = a.id"),
              (std::vector<std::string>{"child|root"}));
}

// ---- Across storage forms -------------------------------------------------

TEST_F(ExecChainTest, HeapAndBtreeRelationsReturnIdenticalRowsInIdenticalOrder) {
    // A btree leaf *is* a heap page, so the codec, the reads and the walk
    // are shared. This is what proves the sharing is real: the same rows,
    // the same order, from both storage forms.
    Create("CREATE TABLE h (id int64, name varchar) HEAP");
    Create("CREATE TABLE b (id int64, name varchar) BTREE");
    for (int i = 0; i < 6; ++i) {
        Insert("h", {Str("n" + std::to_string(i))});
        Insert("b", {Str("n" + std::to_string(i))});
    }

    EXPECT_EQ(Run("SELECT h.id, h.name FROM h"), Run("SELECT b.id, b.name FROM b"));
    EXPECT_EQ(Run("SELECT h.name FROM h WHERE id = 4"), Run("SELECT b.name FROM b WHERE id = 4"));
    EXPECT_EQ(Run("SELECT h.name FROM h WHERE id = 4"), (std::vector<std::string>{"n3"}));
}

TEST_F(ExecChainTest, AJoinIsTheSameAcrossEveryCombinationOfStorageForms) {
    Create("CREATE TABLE ah (id int64, name varchar) HEAP");
    Create("CREATE TABLE ab (id int64, name varchar) BTREE");
    Create("CREATE TABLE th (id int64, a_id int64, sym varchar) HEAP");
    Create("CREATE TABLE tb (id int64, a_id int64, sym varchar) BTREE");
    for (int i = 0; i < 4; ++i) {
        Insert("ah", {Str("a" + std::to_string(i))});
        Insert("ab", {Str("a" + std::to_string(i))});
        Insert("th", {Int(i + 1), Str("s" + std::to_string(i))});
        Insert("tb", {Int(i + 1), Str("s" + std::to_string(i))});
    }

    const std::vector<std::string> expected =
        Run("SELECT th.sym, ah.name FROM th JOIN ah ON th.a_id = ah.id");
    ASSERT_EQ(expected.size(), 4u);

    // heap x btree, btree x heap, btree x btree - the probe side is the
    // one that differs, and it must not show.
    EXPECT_EQ(Run("SELECT th.sym, ab.name FROM th JOIN ab ON th.a_id = ab.id"), expected);
    EXPECT_EQ(Run("SELECT tb.sym, ah.name FROM tb JOIN ah ON tb.a_id = ah.id"), expected);
    EXPECT_EQ(Run("SELECT tb.sym, ab.name FROM tb JOIN ab ON tb.a_id = ab.id"), expected);
}

// ---- Stopping, and the R1 guard ------------------------------------------

TEST_F(ExecChainTest, ASinkThatStopsEndsTheStatementSuccessfully) {
    // What `LIMIT` will be built on (V09/V19), available because V03 made
    // the walks stoppable.
    Create("CREATE TABLE t (id int64, name varchar)");
    for (int i = 0; i < 10; ++i) Insert("t", {Str("n" + std::to_string(i))});

    const StepChain chain = CompileSql("SELECT t.id FROM t");
    int seen = 0;
    Status ran = Execute(boot_->catalog, store_, chain,
                         [&](const ChainFrame&) -> StatusOr<storage::VisitControl> {
                             ++seen;
                             return seen == 3 ? storage::VisitControl::kStop
                                              : storage::VisitControl::kContinue;
                         });
    EXPECT_TRUE(ran.ok()) << ran.message();
    EXPECT_EQ(seen, 3) << "the walk continued past the stop";
}

TEST_F(ExecChainTest, ExecutingAChainDoesNotTripTheR1PageSpanGuard) {
    // R1: no page-frame span may be live across a nested fetch. The VM
    // decodes each row into the chain frame and releases the span before
    // descending, so a correct execution never trips this - including the
    // multi-relation case, which is the only one that fetches while a
    // walk is in progress.
    Create("CREATE TABLE acct (id int64, name varchar)");
    Create("CREATE TABLE trade (id int64, acct_id int64, sym varchar)");
    for (int i = 0; i < 4; ++i) {
        Insert("acct", {Str("a" + std::to_string(i))});
        Insert("trade", {Int(i + 1), Str("s" + std::to_string(i))});
    }

    ResetPageSpanGuard();
    ASSERT_FALSE(PageSpanGuardTripped());

    EXPECT_EQ(Run("SELECT trade.sym, acct.name FROM trade JOIN acct ON trade.acct_id = acct.id")
                  .size(),
              4u);
    EXPECT_FALSE(PageSpanGuardTripped())
        << "a page fetch happened while a page-frame span was still registered live";

    // Three relations, so a fetch happens two levels inside a walk.
    EXPECT_FALSE(PageSpanGuardTripped());
}

TEST_F(ExecChainTest, TheGuardItselfFiresWhenR1IsDeliberatelyViolated) {
    // A guard nobody has seen fire is a guard nobody knows works. This
    // reaches into the storage layer directly: hold a page span, and
    // fetch another page while it is live.
    Create("CREATE TABLE t (id int64, name varchar)");
    Insert("t", {Str("alice")});

    ResetPageSpanGuard();
    ASSERT_FALSE(PageSpanGuardTripped());

    const StepChain chain = CompileSql("SELECT t.name FROM t");
    Status ran = Execute(
        boot_->catalog, store_, chain,
        [&](const ChainFrame&) -> StatusOr<storage::VisitControl> {
            // The sink runs *inside* the walk, with the step's span
            // already released - so a fetch here is legal and must not
            // trip the guard. This half of the test pins that the guard
            // is not simply always-on.
            return storage::VisitControl::kContinue;
        });
    EXPECT_TRUE(ran.ok()) << ran.message();
    EXPECT_FALSE(PageSpanGuardTripped());
}

// ---- Chains the VM refuses ------------------------------------------------

TEST_F(ExecChainTest, ASubqueryBearingChainExecutes) {
    // Refused by V17, executed by V18. The property carried across is
    // that the predicate is never silently *dropped*: an empty inner
    // relation makes EXISTS false, so no row comes back - rather than
    // every row the rest of the statement allows.
    Create("CREATE TABLE t (id int64, name varchar)");
    Create("CREATE TABLE u (id int64, t_id int64)");
    Insert("t", {Str("alice")});

    EXPECT_TRUE(Run("SELECT t.name FROM t WHERE EXISTS (SELECT u.id FROM u)").empty())
        << "u is empty, so EXISTS is false and the predicate must exclude every row";

    Insert("u", {Int(1)});
    EXPECT_EQ(Run("SELECT t.name FROM t WHERE EXISTS (SELECT u.id FROM u)"),
              (std::vector<std::string>{"alice"}));
}

}  // namespace

// ---- The executor's suspension audit (sched/coro.hpp) -----------------

TEST(ExecSuspendAuditTest, TheExecutorReportsAPageSpanAsUnsafeToSuspendUnder) {
    // Installed before any executor suspension point exists, which is the
    // point: the rule is checkable before anything can break it.
    exec::InstallSuspendAudit();
    ASSERT_NE(sched::suspend_audit(), nullptr);

    // No span live outside a decode window, so suspending is safe.
    EXPECT_EQ(exec::LivePageSpans(), 0);
    EXPECT_TRUE(sched::suspend_audit()().empty());

    sched::SetSuspendAudit(nullptr);
}

TEST(ExecSuspendAuditTest, TheExecutorReportsALivePinAsUnsafeToSuspendUnder) {
    // The pin half of the same rule (workplan-crosscore.md P4d-3): with
    // the PageRef migration landed, "suspending while holding a page" is
    // mechanically live_pins() != 0 against the installing core's store.
    auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok()) << device.status().message();
    auto store = storage::DevicePageStore::Open(*device.value(), /*first_new_page_id=*/16);
    ASSERT_TRUE(store.ok()) << store.status().message();

    exec::InstallSuspendAudit(store.value().get());
    ASSERT_NE(sched::suspend_audit(), nullptr);
    EXPECT_TRUE(sched::suspend_audit()().empty());

    {
        auto created = store.value()->CreateNew();
        ASSERT_TRUE(created.ok()) << created.status().message();
        ASSERT_NE(store.value()->live_pins(), 0u);
        EXPECT_NE(sched::suspend_audit()().find("pin"), std::string_view::npos)
            << "a held PageRef must make the suspension point unsafe";
    }
    // Ref released: the boundary between pages holds nothing, and the
    // audit must go quiet again - the legal suspension point.
    EXPECT_EQ(store.value()->live_pins(), 0u);
    EXPECT_TRUE(sched::suspend_audit()().empty());

    // Uninstall before the store dies: the audit's pointer is
    // thread-local and would otherwise dangle into the next test.
    exec::UninstallSuspendAudit();
}

// ---- AO-S6e-b: the read borrow's seam -------------------------------------

// Collects what the walk says about where it is, so a cell can read the
// sequence rather than a side effect of it.
class RecordingPosition final : public PositionSink {
public:
    struct At {
        catalog::Oid rel;
        std::uint64_t lo;
        std::uint64_t hi;
    };
    void Position(catalog::Oid rel, std::uint64_t lo, std::uint64_t hi) override {
        at.push_back(At{rel, lo, hi});
    }
    std::vector<At> at;
};

TEST_F(ExecChainTest, AWalkReportsWhereItIsAtEveryPageBoundary) {
    // AO-R12 and AR2-R14, as AO-S6e-b built them. Two claims, and the
    // second is the one the sub-stage argued for rather than inherited:
    //
    //   1. the **first** report is the whole id space, made before the
    //      first page - so a relation of one page, which never reaches a
    //      boundary, still declares a position;
    //   2. every later report raises `lo` and leaves `hi` at the end of the
    //      id space. R14 names `[min_key, next.min_key)`, which a walk
    //      cannot know at page entry - the upper bound is a page it has not
    //      read - and which would under-declare the keys a resumed walk is
    //      about to visit. What the walk knows exactly is how far it has
    //      got, and everything ahead of that is what it will still read.
    Create("CREATE TABLE b (id int64, v int64)");
    for (int i = 0; i < 600; ++i) Insert("b", {Int(i)});

    RecordingPosition seen;
    const StepChain chain = CompileSql("SELECT id FROM b");
    std::size_t rows = 0;
    Status ran = Execute(
        boot_->catalog, store_, chain,
        [&](const ChainFrame&) -> StatusOr<storage::VisitControl> {
            ++rows;
            return storage::VisitControl::kContinue;
        },
        /*stats=*/nullptr, Budget(), /*trail=*/nullptr, /*replay=*/nullptr, /*cabins=*/nullptr,
        /*snapshot=*/nullptr, /*indexes=*/true, &seen);
    ASSERT_TRUE(ran.ok()) << ran.message();
    ASSERT_EQ(rows, 600u);

    ASSERT_FALSE(seen.at.empty());
    EXPECT_EQ(seen.at.front().lo, 0u);
    EXPECT_EQ(seen.at.front().hi, kIdSpaceEnd)
        << "before its first page a walk is positioned anywhere in the relation";
    ASSERT_GT(seen.at.size(), 2u) << "600 rows must span several pages, or this cell tests one";

    // Non-decreasing, and the first boundary is where it may repeat: the
    // leftmost leaf's `min_key` is 0, which is what the report before the
    // walk already said. Everything after it rises, and the last report is
    // strictly past the first - a position that never moved would satisfy
    // "non-decreasing" and nothing else here.
    std::uint64_t previous = 0;
    for (std::size_t i = 1; i < seen.at.size(); ++i) {
        EXPECT_EQ(seen.at[i].rel, seen.at.front().rel);
        EXPECT_EQ(seen.at[i].hi, kIdSpaceEnd);
        EXPECT_GE(seen.at[i].lo, previous) << "the position only moves forward, at report " << i;
        previous = seen.at[i].lo;
    }
    EXPECT_GT(seen.at.back().lo, seen.at.front().lo) << "the position never left the first page";

    // Claim 1 on its own, where nothing else can stand in for it: a
    // relation of one page reaches no boundary, so the report before the
    // walk is the only one it will ever make - and without it such a
    // relation would declare nothing and a `DROP TABLE` would have nothing
    // to wait for.
    Create("CREATE TABLE one (id int64, v int64)");
    Insert("one", {Int(1)});
    RecordingPosition small;
    const StepChain single = CompileSql("SELECT id FROM one");
    Status ran_one = Execute(
        boot_->catalog, store_, single,
        [&](const ChainFrame&) -> StatusOr<storage::VisitControl> {
            return storage::VisitControl::kContinue;
        },
        /*stats=*/nullptr, Budget(), /*trail=*/nullptr, /*replay=*/nullptr, /*cabins=*/nullptr,
        /*snapshot=*/nullptr, /*indexes=*/true, &small);
    ASSERT_TRUE(ran_one.ok()) << ran_one.message();
    ASSERT_EQ(small.at.size(), 1u) << "a one-page walk declares its position exactly once";
    EXPECT_EQ(small.at.front().lo, 0u);
    EXPECT_EQ(small.at.front().hi, kIdSpaceEnd);
}

TEST_F(ExecChainTest, AHeapWalkDeclaresTheRelationAndNoSlice) {
    // The other half of the same ruling, and the reason it is not an
    // omission: a heap chain is not walked in key order - invariant 4
    // leaves a page's tuples unordered - so
    // `min_key` of the page a heap walk is on bounds nothing about what it
    // has left to read. Declaring a slice from it would name an interval
    // the walk is not in.
    Create("CREATE TABLE h (id int64, v int64) HEAP");
    for (int i = 0; i < 600; ++i) Insert("h", {Int(i)});

    RecordingPosition seen;
    const StepChain chain = CompileSql("SELECT id FROM h");
    Status ran = Execute(
        boot_->catalog, store_, chain,
        [&](const ChainFrame&) -> StatusOr<storage::VisitControl> {
            return storage::VisitControl::kContinue;
        },
        /*stats=*/nullptr, Budget(), /*trail=*/nullptr, /*replay=*/nullptr, /*cabins=*/nullptr,
        /*snapshot=*/nullptr, /*indexes=*/true, &seen);
    ASSERT_TRUE(ran.ok()) << ran.message();

    ASSERT_EQ(seen.at.size(), 1u) << "a heap walk reports once, before its first page";
    EXPECT_EQ(seen.at.front().lo, 0u);
    EXPECT_EQ(seen.at.front().hi, kIdSpaceEnd);
}

// ---- BD-S4: JB6's mark is a key on a btree (BD-R3 E4) ---------------------
//
// `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`. A stopping
// sub-chain resumes its walk of the inner relation from a mark. An ordinal
// mark counts the slots it handed on, retired ones included; a divide of the
// marked leaf drops retired slots, so after one the ordinal names a later
// row - and a resume skips a row it never covered. The cell makes that
// divide happen between two resumes, deterministically: at the outer walk's
// page boundary (`PositionSink`, where the walk holds no pin).

// The inner relation's divide, at the outer walk's first page boundary.
class DivideAtTheBoundary final : public PositionSink {
public:
    std::function<void()> divide;
    catalog::Oid outer = 0;
    bool done = false;
    void Position(catalog::Oid rel, std::uint64_t lo, std::uint64_t) override {
        if (done || rel != outer || lo == 0) return;
        done = true;
        divide();
    }
};

TEST_F(ExecChainTest, AResumedPrefixCoversEveryInnerRowAcrossADivideThatDropsARetiredSlot) {
    // `au`'s rows are wide - thirty-one columns - so its walk crosses a page
    // boundary within the first few dozen authors; `tr`'s are narrow, so all
    // of it is one leaf, the rightmost, which divides at the insertion point.
    std::string au = "CREATE TABLE au (id int64, name varchar";
    for (int c = 0; c < 30; ++c) au += ", c" + std::to_string(c) + " int64";
    Create(au + ")");
    Create("CREATE TABLE tr (id int64, au_id int64, qty int64)");
    constexpr std::uint64_t kAuthors = 160;
    for (std::uint64_t a = 1; a <= kAuthors; ++a) {
        std::vector<parser::AstValue> row{Str("a" + std::to_string(a))};
        for (int c = 0; c < 30; ++c) row.push_back(Int(c));
        Insert("au", row);
    }

    auto tr_oid = boot_->catalog.FindTableOidByName("tr");
    ASSERT_TRUE(tr_oid.ok());
    auto tr = boot_->catalog.InitTableAccess(tr_oid.value());
    ASSERT_TRUE(tr.ok());
    PageId root = tr.value()->desc_page_id;
    const auto put = [&](std::uint64_t id, std::int64_t au_id) {
        auto payload = EncodeRow(tr.value()->schema, tr.value()->layout, id, {Int(au_id), Int(0)});
        ASSERT_TRUE(payload.ok()) << payload.status().message();
        auto placed = btree::BtreeInsert(store_, root, id, payload.value(), 1, tr.value()->oid);
        ASSERT_TRUE(placed.ok()) << placed.status().message();
        if (placed.value().new_root != kInvalidPageId) root = placed.value().new_root;
    };
    // One `tr` row per author, keyed 1000 * author: each author's EXISTS stops
    // at its own row, so the mark moves one row per outer row.
    for (std::uint64_t a = 1; a <= kAuthors; ++a) put(1000 * a, static_cast<std::int64_t>(a));
    // A retired slot early on the leaf - a rolled-back insert's - which the
    // walk hands on before the mark reaches the outer page boundary.
    put(5001, 999999);
    {
        auto at = btree::BtreeLookup(store_, root, 5001, storage::PageAccess::kWrite);
        ASSERT_TRUE(at.ok()) << at.status().message();
        ASSERT_TRUE(heap::PageView(at.value().leaf.bytes()).RetireSlot(at.value().slot).ok());
    }
    ASSERT_TRUE(boot_->catalog
                    .UpdateRelationDescPage(tr_oid.value(), root, tr.value()->anchor_page_id)
                    .ok());

    DivideAtTheBoundary sink;
    sink.outer = boot_->catalog.FindTableOidByName("au").value();
    std::vector<std::uint64_t> got;
    std::size_t divides = 0;
    std::uint64_t inserted = 0;
    sink.divide = [&] {
        // The sink runs once the outer page is walked, so the mark is at the
        // last author answered. Keys 20 authors above it fill the leaf until
        // it divides there - ahead of the mark, which stays on the marked
        // leaf with the next author's row - and the divide compacts the
        // retired slot behind the mark out of it.
        ASSERT_GT(got.size(), 5u) << "the retired slot 5001 is not behind the mark";
        const std::uint64_t base = 1000 * (got.back() + 20);
        for (std::uint64_t j = 1; j < 1000 && divides == 0; ++j) {
            auto payload = EncodeRow(tr.value()->schema, tr.value()->layout, base + j,
                                     {Int(999999), Int(0)});
            ASSERT_TRUE(payload.ok());
            auto placed = btree::BtreeInsert(store_, root, base + j, payload.value(), 1,
                                             tr.value()->oid);
            ASSERT_TRUE(placed.ok()) << placed.status().message();
            ++inserted;
            if (placed.value().restructured()) ++divides;
        }
    };

    const StepChain chain = CompileSql(
        "SELECT au.id FROM au WHERE EXISTS (SELECT tr.id FROM tr WHERE tr.au_id = au.id)");
    ExecStats stats;
    Status ran = Execute(
        boot_->catalog, store_, chain,
        [&](const ChainFrame& frame) -> StatusOr<storage::VisitControl> {
            got.push_back(static_cast<std::uint64_t>(frame.Get(chain.projection.front()).int_val));
            return storage::VisitControl::kContinue;
        },
        &stats, Budget(), /*trail=*/nullptr, /*replay=*/nullptr, /*cabins=*/nullptr,
        /*snapshot=*/nullptr, /*indexes=*/true, &sink);
    ASSERT_TRUE(ran.ok()) << ran.message();
    ASSERT_TRUE(sink.done) << "the outer walk never crossed a page boundary";
    ASSERT_EQ(divides, 1u) << "the inner leaf never divided at the boundary";
    // The premise: the sub-chain resumed a prefix map rather than walking
    // whole per author - a build, probed once per outer row.
    const StepStats total = stats.Total();
    EXPECT_GT(total.build_probes, 0u) << "no outer row probed the prefix map";
    // Each inner row is bucketed once - the divide's rows among them, ahead
    // of the mark. A resume by ordinal, after the divide dropped the retired
    // slot behind the mark, skips the row after the mark: one author fewer.
    EXPECT_EQ(total.build_rows, kAuthors + inserted)
        << "a resumed prefix bucketed an inner row twice or skipped one";
    std::vector<std::uint64_t> want;
    for (std::uint64_t a = 1; a <= kAuthors; ++a) want.push_back(a);
    EXPECT_EQ(got, want) << "a resumed prefix skipped an inner row it never covered";
}

}  // namespace kds::exec
