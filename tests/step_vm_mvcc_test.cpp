#include <cstring>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/exec/step_vm.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/txn/visibility.hpp"

// The predicate applied where the step VM reads (docs/spec/txn.md section 4.4 as
// amended by docs/txn-workplan.md A1): one call site, and every access kind
// funnels through it.
//
// The equivalence that matters is txn.md section 10-9's: **the probe path
// and the scan path return identical bytes for the same pk under the same
// read view**. That is what makes invariant 9's fall-through safe. A miss
// falls back to a walk, and if the two could disagree about visibility, a
// Waystone miss would change the answer rather than only the cost.

namespace kds::exec {
namespace {

class StepVmMvccTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 4000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
        undo_ = std::make_unique<txn::UndoLog>(store_, /*wal=*/nullptr);
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

    struct Placed {
        PageId page_id = kInvalidPageId;
        std::uint16_t slot = 0;
        std::uint64_t id = 0;
    };

    Placed Insert(const std::string& table, const std::vector<parser::AstValue>& body) {
        auto oid = boot_->catalog.FindTableOidByName(table);
        EXPECT_TRUE(oid.ok()) << oid.status().message();
        auto access = boot_->catalog.InitTableAccess(oid.value());
        EXPECT_TRUE(access.ok()) << access.status().message();
        auto id = boot_->catalog.AllocateRowId(oid.value());
        EXPECT_TRUE(id.ok()) << id.status().message();

        auto payload = EncodeRow(access.value()->schema, access.value()->layout, id.value(), body);
        EXPECT_TRUE(payload.ok()) << payload.status().message();

        Placed out;
        out.id = id.value();
        if (access.value()->clustered_type == catalog::ClusteredType::kBtree) {
            auto placed = btree::BtreeInsert(store_, access.value()->desc_page_id, id.value(),
                                             payload.value(), catalog::kBootstrapXid,
                                             access.value()->oid);
            EXPECT_TRUE(placed.ok()) << placed.status().message();
            out.page_id = placed.value().page_id;
            out.slot = placed.value().slot;
        } else {
            auto placed = heap::ChainInsert(store_, access.value()->desc_page_id, id.value(),
                                            payload.value(), catalog::kBootstrapXid,
                                            access.value()->oid);
            EXPECT_TRUE(placed.ok()) << placed.status().message();
            out.page_id = placed.value().page_id;
            out.slot = placed.value().slot;
        }
        return out;
    }

    // Supersedes the tuple at `at` the way a transactional UPDATE will: the
    // prior image goes to the undo log, and the tuple is overwritten in
    // place with the new writer's id and a link back. Done by hand here
    // because HandleUpdate does not do it yet (T09) - the point of this
    // test is the *reader*.
    void SupersedeWith(const std::string& table, const Placed& at,
                       const std::vector<parser::AstValue>& new_body, std::uint64_t writer) {
        auto oid = boot_->catalog.FindTableOidByName(table);
        ASSERT_TRUE(oid.ok());
        auto access = boot_->catalog.InitTableAccess(oid.value());
        ASSERT_TRUE(access.ok());

        auto page_bytes = store_.Get(at.page_id);
        ASSERT_TRUE(page_bytes.ok());
        heap::PageView page(page_bytes.value().bytes());
        auto tuple = page.ReadTuple(at.slot);
        ASSERT_TRUE(tuple.ok());

        const std::vector<std::byte> before(tuple.value().payload.begin(),
                                            tuple.value().payload.end());
        txn::UndoRecordFields rec{};
        rec.prior_trx_id = tuple.value().trx_id;
        rec.prior_undo_ptr = tuple.value().undo_ptr;
        rec.target_page_id = at.page_id;
        rec.target_slot = at.slot;
        rec.type = static_cast<std::uint8_t>(txn::UndoRecordType::kOverwrite);
        auto ptr = undo_->Append(writer, rec, before);
        ASSERT_TRUE(ptr.ok()) << ptr.status().message();

        auto encoded =
            EncodeRow(access.value()->schema, access.value()->layout, at.id, new_body);
        ASSERT_TRUE(encoded.ok()) << encoded.status().message();

        auto again = store_.Get(at.page_id);
        ASSERT_TRUE(again.ok());
        heap::PageView fresh(again.value().bytes());
        ASSERT_TRUE(fresh.OverwriteTuple(at.slot, encoded.value(), writer, ptr.value()).ok());
    }

    // Delete-marks in place, the way a transactional DELETE will.
    void DeleteMarkWith(const Placed& at, std::uint64_t deleter) {
        auto page_bytes = store_.Get(at.page_id);
        ASSERT_TRUE(page_bytes.ok());
        heap::PageView page(page_bytes.value().bytes());
        auto tuple = page.ReadTuple(at.slot);
        ASSERT_TRUE(tuple.ok());

        txn::UndoRecordFields rec{};
        rec.prior_trx_id = tuple.value().trx_id;
        rec.prior_undo_ptr = tuple.value().undo_ptr;
        rec.target_page_id = at.page_id;
        rec.target_slot = at.slot;
        rec.type = static_cast<std::uint8_t>(txn::UndoRecordType::kDeleteMark);
        auto ptr = undo_->Append(deleter, rec, {});
        ASSERT_TRUE(ptr.ok());

        auto again = store_.Get(at.page_id);
        ASSERT_TRUE(again.ok());
        heap::PageView fresh(again.value().bytes());
        // The mark and the link are two writes: OverwriteTuple carries the
        // new header, DeleteMark sets the slot flag and re-stamps the
        // deleter. Same order the DELETE handler will use.
        auto reread = fresh.ReadTuple(at.slot);
        ASSERT_TRUE(reread.ok());
        const std::vector<std::byte> same(reread.value().payload.begin(),
                                          reread.value().payload.end());
        ASSERT_TRUE(fresh.OverwriteTuple(at.slot, same, deleter, ptr.value()).ok());
        ASSERT_TRUE(fresh.DeleteMark(at.slot, deleter).ok());
    }

    static parser::AstValue Int(std::int64_t v) {
        parser::AstValue out;
        out.type = parser::ValueType::kInt;
        out.int_val = v;
        out.raw_int_text = std::to_string(v);
        return out;
    }

    txn::Snapshot ViewUpTo(std::uint64_t up_to) {
        txn::Snapshot snap;
        snap.view.up_to_trx_id = up_to;
        snap.undo = undo_.get();
        return snap;
    }

    std::vector<std::string> Run(const std::string& sql, const txn::Snapshot* snapshot) {
        auto parsed = parser::Parse(sql);
        EXPECT_TRUE(parsed.ok()) << sql << ": " << parsed.status().message();
        if (!parsed.ok()) return {};
        auto chain = Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()));
        EXPECT_TRUE(chain.ok()) << sql << ": " << chain.status().message();
        if (!chain.ok()) return {};
        return RunChain(chain.value(), snapshot);
    }

    std::vector<std::string> RunChain(const StepChain& chain, const txn::Snapshot* snapshot) {
        std::vector<std::string> rows;
        Status ran = Execute(
            boot_->catalog, store_, chain,
            [&](const ChainFrame& frame) -> StatusOr<storage::VisitControl> {
                std::string row;
                for (const ColumnRef& ref : chain.projection) {
                    if (!row.empty()) row += '|';
                    const parser::AstValue& v = frame.Get(ref);
                    row += v.type == parser::ValueType::kStr ? v.str_val
                                                             : std::to_string(v.int_val);
                }
                rows.push_back(std::move(row));
                return storage::VisitControl::kContinue;
            },
            /*stats=*/nullptr, Budget(), /*trail=*/nullptr, /*replay=*/nullptr,
            /*cabins=*/nullptr, snapshot);
        EXPECT_TRUE(ran.ok()) << ran.message();
        return rows;
    }

    // The same chain with every step downgraded to a plain scan. The
    // equivalence this file exists for: a located row and a walked row must
    // be filtered identically.
    static StepChain AsScan(StepChain chain) {
        for (Step& step : chain.steps) step.kind = AccessKind::kScan;
        return chain;
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
    std::unique_ptr<txn::UndoLog> undo_;
};

// The whole reason the default snapshot exists: a caller that passes none
// reads exactly what the pre-MVCC executor read.
TEST_F(StepVmMvccTest, NoSnapshotReadsEverythingExactlyAsBefore) {
    Create("CREATE TABLE t (id int64, v int32)");
    Insert("t", {Int(10)});
    Insert("t", {Int(20)});

    const std::vector<std::string> expected{"1|10", "2|20"};
    EXPECT_EQ(Run("SELECT id, v FROM t", nullptr), expected);

    // And an explicit everything-view is the same thing said out loud.
    txn::Snapshot everything;
    everything.undo = undo_.get();
    EXPECT_EQ(Run("SELECT id, v FROM t", &everything), expected);
}

TEST_F(StepVmMvccTest, AnOldViewReadsThePriorVersionThroughTheChain) {
    Create("CREATE TABLE t (id int64, v int32)");
    const Placed row = Insert("t", {Int(10)});
    SupersedeWith("t", row, {Int(99)}, /*writer=*/60);

    // A view taken before 60 committed sees the old value...
    const txn::Snapshot before = ViewUpTo(60);
    EXPECT_EQ(Run("SELECT id, v FROM t", &before), (std::vector<std::string>{"1|10"}));
    // ...and one taken after sees the new one.
    const txn::Snapshot after = ViewUpTo(61);
    EXPECT_EQ(Run("SELECT id, v FROM t", &after), (std::vector<std::string>{"1|99"}));
}

TEST_F(StepVmMvccTest, AnInsertByAnInvisibleWriterIsNotAVersionAtAll) {
    Create("CREATE TABLE t (id int64, v int32)");
    Insert("t", {Int(10)});
    const Placed newer = Insert("t", {Int(20)});

    // Restamp the second row's writer to a transaction the view cannot
    // see. Its undo_ptr stays 0, which is what an INSERT leaves (section
    // 3.6) - and that alone means "no visible version".
    auto page = store_.Get(newer.page_id);
    ASSERT_TRUE(page.ok());
    heap::PageView view(page.value().bytes());
    auto tuple = view.ReadTuple(newer.slot);
    ASSERT_TRUE(tuple.ok());
    const std::vector<std::byte> same(tuple.value().payload.begin(), tuple.value().payload.end());
    ASSERT_TRUE(view.OverwriteTuple(newer.slot, same, /*trx_id=*/60, txn::kNoUndoPtr).ok());

    const txn::Snapshot before = ViewUpTo(60);
    EXPECT_EQ(Run("SELECT id, v FROM t", &before), (std::vector<std::string>{"1|10"}));

    const txn::Snapshot after = ViewUpTo(61);
    EXPECT_EQ(Run("SELECT id, v FROM t", &after), (std::vector<std::string>{"1|10", "2|20"}));
}

TEST_F(StepVmMvccTest, ADeleteMarkIsReadFromBothSides) {
    Create("CREATE TABLE t (id int64, v int32)");
    const Placed row = Insert("t", {Int(10)});
    Insert("t", {Int(20)});
    DeleteMarkWith(row, /*deleter=*/60);

    // An older view still has the row: the mark carried no bytes, so
    // stepping back over it keeps the tuple's own payload.
    const txn::Snapshot before = ViewUpTo(60);
    EXPECT_EQ(Run("SELECT id, v FROM t", &before), (std::vector<std::string>{"1|10", "2|20"}));

    // A newer view does not.
    const txn::Snapshot after = ViewUpTo(61);
    EXPECT_EQ(Run("SELECT id, v FROM t", &after), (std::vector<std::string>{"2|20"}));
}

// ---- txn.md section 10-9: probe and scan agree ---------------------------
//
// Run on both storage forms, because only a btree relation takes the
// descent - and a descent is exactly the path a Waystone hit replaces.
class StepVmMvccStorageTest : public StepVmMvccTest,
                              public ::testing::WithParamInterface<const char*> {};

TEST_P(StepVmMvccStorageTest, ThePointPathAndTheScanAgreeUnderEveryView) {
    Create(std::string("CREATE TABLE t (id int64, v int32) ") + GetParam());
    const Placed one = Insert("t", {Int(10)});
    Insert("t", {Int(20)});
    const Placed three = Insert("t", {Int(30)});

    SupersedeWith("t", one, {Int(11)}, /*writer=*/60);
    DeleteMarkWith(three, /*deleter=*/70);

    for (std::uint64_t up_to : {std::uint64_t{60}, std::uint64_t{61}, std::uint64_t{70},
                                std::uint64_t{71}}) {
        const txn::Snapshot snap = ViewUpTo(up_to);
        for (const char* sql : {"SELECT id, v FROM t WHERE id = 1", "SELECT id, v FROM t WHERE id = 3"}) {
            const StepChain located = CompileSql(sql);
            const std::vector<std::string> point = RunChain(located, &snap);
            const std::vector<std::string> walked = RunChain(AsScan(located), &snap);
            EXPECT_EQ(point, walked)
                << sql << " under up_to " << up_to << " on " << GetParam();
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Storage, StepVmMvccStorageTest, ::testing::Values("HEAP", "BTREE"),
                         [](const ::testing::TestParamInfo<const char*>& info) {
                             return std::string(info.param);
                         });

// A view that needs the chain with no log to walk is a caller defect, and
// it is reported rather than guessed at: guessing either way invents a row
// or hides one.
TEST_F(StepVmMvccTest, AViewWithNoUndoLogIsRefusedRatherThanGuessed) {
    Create("CREATE TABLE t (id int64, v int32)");
    const Placed row = Insert("t", {Int(10)});
    SupersedeWith("t", row, {Int(99)}, /*writer=*/60);

    txn::Snapshot no_log;
    no_log.view.up_to_trx_id = 60;
    no_log.undo = nullptr;

    auto chain = Compile(boot_->catalog,
                         std::get<parser::SelectStmt>(parser::Parse("SELECT id, v FROM t").value()));
    ASSERT_TRUE(chain.ok());
    Status ran = Execute(
        boot_->catalog, store_, chain.value(),
        [](const ChainFrame&) -> StatusOr<storage::VisitControl> {
            return storage::VisitControl::kContinue;
        },
        /*stats=*/nullptr, Budget(), /*trail=*/nullptr, /*replay=*/nullptr, /*cabins=*/nullptr,
        &no_log);
    EXPECT_EQ(ran.code(), StatusCode::kInvalidArgument) << ran.message();
}

// R1 is enforced by a guard, not by discipline - and the undo walk is
// exactly the fetch that would have tripped it if the predicate had been
// applied inline. This is the mechanized form of workplan amendment A2.
TEST_F(StepVmMvccTest, TheUndoWalkDoesNotViolateTheNestedAccessRule) {
    Create("CREATE TABLE t (id int64, v int32)");
    const Placed row = Insert("t", {Int(10)});
    SupersedeWith("t", row, {Int(99)}, /*writer=*/60);

    ResetPageSpanGuard();
    const txn::Snapshot before = ViewUpTo(60);
    EXPECT_EQ(Run("SELECT id, v FROM t", &before), (std::vector<std::string>{"1|10"}));
    EXPECT_FALSE(PageSpanGuardTripped())
        << "the undo chain was walked with a page span still registered";
}

}  // namespace
}  // namespace kds::exec
