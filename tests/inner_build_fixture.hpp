#pragma once

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

// The one fixture both inner-build suites run on (workplan JB3-JB5).
//
// One data set on purpose: the exec suite pins these shapes against
// hand-computed reply vectors, and the contract suite ties every cap
// configuration to a cap-0 reference over the same rows - so the
// reference's soundness is closed by the sibling suite *structurally*,
// which two drifting copies of this fixture could not guarantee
// (the JB5 review's S1).
//
// The relations, and the walk-order facts the assertions hand-compute
// from: au(id, name) = alice/bob/carol (ids 1..3). tr(id, au_id, qty) =
// (1,10) (2,20) (1,30) (9,5) (2,50) - au_id is non-pk, unindexed,
// uncabined, so a join on it takes the build annotation; (9,5) matches
// no au row ever. ln(id, tr_qty, amt) = (10,100) (10,101) (30,300)
// (50,500) - joined on tr.qty for the two-annotated-step shapes.

namespace kds::exec {

class InnerBuildFixture : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 4000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));

        Create("CREATE TABLE au (id int64, name varchar)");
        Create("CREATE TABLE tr (id int64, au_id int64, qty int64)");
        Create("CREATE TABLE ln (id int64, tr_qty int64, amt int64)");
        Insert("au", {Str("alice")});
        Insert("au", {Str("bob")});
        Insert("au", {Str("carol")});
        Insert("tr", {Int(1), Int(10)});
        Insert("tr", {Int(2), Int(20)});
        Insert("tr", {Int(1), Int(30)});
        Insert("tr", {Int(9), Int(5)});
        Insert("tr", {Int(2), Int(50)});
        Insert("ln", {Int(10), Int(100)});
        Insert("ln", {Int(10), Int(101)});
        Insert("ln", {Int(30), Int(300)});
        Insert("ln", {Int(50), Int(500)});
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
        auto created = boot_->catalog.CreateTable(catalog::kNamespacePublic, ct.table_name,
                                                  schema, ct.clustered);
        ASSERT_TRUE(created.ok()) << created.status().message();
    }

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

    // Runs `sql`, rendering each row "a|b|c" in emission order. `cap` is
    // the statement's `join_build_max_rows`; `stop_after` non-zero stops
    // the sink after that many rows (the LIMIT interaction).
    std::vector<std::string> Run(const std::string& sql, ExecStats* out_stats = nullptr,
                                 std::size_t cap = kDefaultJoinBuildMaxRows,
                                 std::size_t stop_after = 0) {
        auto parsed = parser::Parse(sql);
        EXPECT_TRUE(parsed.ok()) << parsed.status().message();
        if (!parsed.ok()) return {};
        auto chain = Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()));
        EXPECT_TRUE(chain.ok()) << chain.status().message();
        if (!chain.ok()) return {};

        Budget budget;
        budget.set_join_build_max_rows(cap);
        ExecStats local;
        ExecStats& stats = out_stats != nullptr ? *out_stats : local;
        std::vector<std::string> rows;
        Status ran = Execute(
            boot_->catalog, store_, chain.value(),
            [&](const ChainFrame& frame) -> StatusOr<storage::VisitControl> {
                std::string row;
                for (const ColumnRef& ref : chain.value().projection) {
                    if (!row.empty()) row += '|';
                    row += FormatValue(/*type_val=*/0, frame.Get(ref));
                }
                rows.push_back(std::move(row));
                return (stop_after != 0 && rows.size() >= stop_after)
                           ? storage::VisitControl::kStop
                           : storage::VisitControl::kContinue;
            },
            &stats, budget);
        EXPECT_TRUE(ran.ok()) << sql << " (cap=" << cap << "): " << ran.message();
        return rows;
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

}  // namespace kds::exec
