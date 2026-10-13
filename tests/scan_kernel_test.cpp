#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/exec/chain_frame.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// BM-S3 (`instructions/v3.0.0/workorder-bm-scan-row-kernel.md`): the scan
// kernel for the integer-encoded types. The kernel may only reject a row the
// residual would reject (BM-R1, `[quiet-wrong]`), so the cells are aimed at
// the one wrong answer it can give - a dropped row - and compare every
// verdict with the kernel off:
//   - the differential matrix: every kernel type, the six operators, the
//     literals at each type's boundaries, through SQL so the literal is the
//     one the compiler coerces, and through `ANALYZE` so `examined=` and
//     `sel=` are compared too (BM-R2);
//   - a literal above an `int32` column's range answers as today's int64
//     comparison;
//   - column 0 is the Keystone id, not its 8-byte cell (BM-R4);
//   - a correlated conjunct is never compiled;
//   - the row-touch budget refuses (the row it refuses on is
//     `scan_row_walk_test.cpp`'s cell, where the rows before it show).
// The MVCC cell (a reconstructed version is read from `version_`, never the
// page) is `step_vm_mvcc_test.cpp`'s; the recording cell is the Cabin
// contract suite's `RecordingIgnoresTheStatementsOtherConjuncts`, whose
// `qty > 30` is a kernel term.

namespace kds::server {
namespace {

class Instance {
public:
    explicit Instance(bool scan_kernel, std::uint64_t row_budget = exec::kDefaultRowTouchBudget) {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        EXPECT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        exec::Budget budget(row_budget);
        budget.set_scan_kernel(scan_kernel);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            budget);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

private:
    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

// One kernel type: its declared name, the values its rows carry (each
// stored in the NOT NULL column `c` and the nullable column `n`, and once
// more with `n` NULL), and the literals every operator is tried against.
struct TypeCase {
    const char* type;
    std::vector<std::string> values;
    std::vector<std::string> literals;
};

std::vector<TypeCase> Cases() {
    const std::vector<std::string> i8 = {"-128", "-1", "0", "1", "127"};
    const std::vector<std::string> i16 = {"-32768", "-1", "0", "1", "32767"};
    const std::vector<std::string> i32 = {"-2147483648", "-1", "0", "1", "2147483647"};
    const std::vector<std::string> i64 = {"-9223372036854775808", "-1", "0", "1",
                                          "9223372036854775807"};
    const std::vector<std::string> u64 = {"0", "1", "9223372036854775807",
                                          "9223372036854775808", "18446744073709551615"};
    const std::vector<std::string> dates = {"'1969-12-31'", "'1970-01-01'", "'1970-01-02'",
                                            "'2026-08-09'"};
    const std::vector<std::string> stamps = {"'1969-12-31 23:59:59'", "'1970-01-01 00:00:00'",
                                             "'1970-01-01 00:00:01'", "'2026-08-09 12:00:00'"};
    const std::vector<std::string> dec = {"'-9999999999.99'", "'-0.01'", "'0.00'", "'0.01'",
                                          "'9999999999.99'"};
    // Unscaled values across 2^63 and the sign: the high halves equal, the
    // low halves differing only in their top bit, and a negative pair.
    const std::vector<std::string> wide = {
        "'-922337203685477.5808'", "'-922337203685477.5807'", "'-0.0001'", "'0.0000'",
        "'922337203685477.5807'",  "'922337203685477.5808'",  "'922337203685477.5809'"};
    auto with = [](std::vector<std::string> v, std::vector<std::string> extra) {
        v.insert(v.end(), extra.begin(), extra.end());
        return v;
    };
    return {
        {"int8", i8, with(i8, {"-129", "128", "1099511627776"})},
        {"int16", i16, with(i16, {"-32769", "32768"})},
        // 2^40 and -2^40: outside the column's range, compared as the int64
        // they are, never narrowed (BM-R4).
        {"int32", i32, with(i32, {"-2147483649", "2147483648", "1099511627776", "-1099511627776"})},
        {"int64", i64, i64},
        {"uint64", u64, u64},
        {"bool", {"0", "1"}, {"0", "1", "2", "-1"}},
        {"date", dates, dates},
        {"timestamp", stamps, stamps},
        {"decimal(12,2)", dec, dec},
        {"decimal(30,4)", wide, wide},
    };
}

const char* const kOps[] = {"=", "!=", "<", "<=", ">", ">="};

TEST(ScanKernelTest, EveryTypeOperatorAndBoundaryAnswersAsTheResidualDoes) {
    Instance on(/*scan_kernel=*/true);
    Instance off(/*scan_kernel=*/false);
    std::size_t compared = 0;
    for (const TypeCase& c : Cases()) {
        const std::string table = std::string("k_") + std::to_string(compared);
        for (Instance* db : {&on, &off}) {
            ASSERT_EQ(db->Run("CREATE TABLE " + table + " (id int64, c " + c.type + ", n " +
                              c.type + " NULL) BTREE")
                          .substr(0, 7),
                      "CREATED")
                << c.type;
            // The first visible row of a step takes the full path, by
            // design, so it is a filler the kernel never sees.
            const std::string filler = c.values.front();
            ASSERT_EQ(db->Run("INSERT INTO " + table + " VALUES (" + filler + ", " + filler + ")")
                          .substr(0, 8),
                      "INSERTED");
            for (const std::string& v : c.values) {
                ASSERT_EQ(db->Run("INSERT INTO " + table + " VALUES (" + v + ", " + v + ")")
                              .substr(0, 8),
                          "INSERTED")
                    << c.type << " " << v;
                ASSERT_EQ(db->Run("INSERT INTO " + table + " VALUES (" + v + ", NULL)")
                              .substr(0, 8),
                          "INSERTED")
                    << c.type << " " << v;
            }
        }
        for (const char* column : {"c", "n"}) {
            for (const char* op : kOps) {
                for (const std::string& lit : c.literals) {
                    const std::string sql = "SELECT id, c, n FROM " + table + " WHERE " + column +
                                            " " + op + " " + lit;
                    EXPECT_EQ(on.Run(sql), off.Run(sql)) << c.type << ": " << sql;
                    EXPECT_EQ(on.Run("ANALYZE " + sql), off.Run("ANALYZE " + sql))
                        << c.type << ": " << sql;
                    ++compared;
                }
            }
        }
    }
    EXPECT_GT(compared, 500u);
}

// A literal outside an `int32` column's range is compared as the int64 it
// is: `c < 2^40` keeps every row (and `!=`), `c = 2^40` and `c > 2^40` none.
TEST(ScanKernelTest, ALiteralAboveAnInt32ColumnsRangeComparesAsAnInt64) {
    Instance db(/*scan_kernel=*/true);
    ASSERT_EQ(db.Run("CREATE TABLE w (id int64, c int32) BTREE").substr(0, 7), "CREATED");
    for (int i = 0; i < 6; ++i) {
        ASSERT_EQ(db.Run("INSERT INTO w VALUES (" + std::to_string(i * 1000) + ")").substr(0, 8),
                  "INSERTED");
    }
    for (const char* op : {"<", "<=", "!="}) {
        EXPECT_EQ(db.Run(std::string("SELECT COUNT(*) FROM w WHERE c ") + op + " 1099511627776"),
                  "count(*)\\n6")
            << op;
    }
    for (const char* op : {"=", ">", ">="}) {
        EXPECT_EQ(db.Run(std::string("SELECT COUNT(*) FROM w WHERE c ") + op + " 1099511627776"),
                  "count(*)\\n0")
            << op;
    }
}

// The row-touch budget refuses on the same row with the kernel on and off,
// on a statement whose every row the kernel rejects.
TEST(ScanKernelTest, TheBudgetRefusesOnTheSameRowWhenTheKernelRejectsEveryRow) {
    Instance on(/*scan_kernel=*/true, /*row_budget=*/40);
    Instance off(/*scan_kernel=*/false, /*row_budget=*/40);
    for (Instance* db : {&on, &off}) {
        ASSERT_EQ(db->Run("CREATE TABLE r (id int64, v int64) BTREE").substr(0, 7), "CREATED");
        for (int i = 0; i < 100; ++i) {
            ASSERT_EQ(db->Run("INSERT INTO r VALUES (" + std::to_string(i) + ")").substr(0, 8),
                      "INSERTED");
        }
    }
    const std::string sql = "SELECT id FROM r WHERE v = -1";
    const std::string refused = on.Run(sql);
    EXPECT_EQ(refused.substr(0, 3), "ERR") << refused;
    EXPECT_NE(refused.find("row-touch budget of 40"), std::string::npos) << refused;
    EXPECT_EQ(refused, off.Run(sql));
}

}  // namespace
}  // namespace kds::server

namespace kds::exec {
namespace {

catalog::SysColumnRow Column(std::uint32_t pos, const char* name, std::uint32_t type_val,
                             std::uint32_t len) {
    catalog::SysColumnRow row{};
    row.pos = pos;
    catalog::SetName(row.name, name);
    row.type_val = type_val;
    row.len = len;
    row.notnull = true;
    return row;
}

// Column 0 is the Keystone id, never its cell: the word's byte 6 is a flags
// byte and byte 7 reserved (keystone.hpp), and the engine writes both 0, so
// only a hand-set byte tells the id from the cell. With byte 6 set, every
// pk comparison still answers on the id - for an int64, int32 and int16 pk.
// The id has bit 31 set: the codec stores the 48-bit id whatever the pk's
// declared width, so a read at that width (2 bytes: 1; 4 bytes: negative)
// is a wrong answer this cell sees. Driven at the codec, not through a
// hand-built leaf: the walk hands the kernel the payload `ReadTupleIn`
// returns, the bytes this cell builds.
TEST(ScanKernelCodecTest, ColumnZeroIsTheKeystoneIdNotItsCell) {
    struct Pk {
        std::uint32_t type_val;
        std::uint32_t len;
    };
    for (const Pk pk : {Pk{catalog::kTypeValInt64, 8}, Pk{catalog::kTypeValInt32, 4},
                        Pk{catalog::kTypeValInt16, 2}}) {
        catalog::Schema schema;
        schema.columns.push_back(Column(0, "id", pk.type_val, pk.len));
        schema.columns.push_back(Column(1, "v", catalog::kTypeValInt64, 8));
        auto layout = catalog::RowLayout::Build(schema, /*inline_cell_width=*/64);
        ASSERT_TRUE(layout.ok()) << layout.status().message();

        parser::AstValue v;
        v.type = parser::ValueType::kInt;
        v.int_val = 7;
        constexpr std::uint64_t kId = 0x80000001;  // 2147483649
        auto row = EncodeRow(schema, layout.value(), kId, {v});
        ASSERT_TRUE(row.ok()) << row.status().message();
        std::vector<std::byte> payload = row.value();
        payload[6] = std::byte{0x5A};

        for (const parser::CompareOp op : {parser::CompareOp::kEq, parser::CompareOp::kLte,
                                           parser::CompareOp::kGte, parser::CompareOp::kNeq}) {
            parser::AstValue lit;
            lit.type = parser::ValueType::kInt;
            lit.int_val = static_cast<std::int64_t>(kId);
            auto term = CompileScanKernelTerm(schema, layout.value(), /*column=*/0, op, lit);
            ASSERT_TRUE(term.has_value());
            ScanKernel kernel{*term};
            const bool keeps = op != parser::CompareOp::kNeq;
            EXPECT_EQ(ScanKernelRejects(kernel, layout.value(), payload), !keeps)
                << "pk type " << pk.type_val << ", op " << static_cast<int>(op);
        }
    }
}

// The witness the differential matrix needs: every type it sweeps, with the
// literal kind the compiler coerces to, compiles to a term - so on and off
// agreeing there is the kernel agreeing, not the kernel declining.
TEST(ScanKernelCodecTest, EveryMatrixTypeCompilesToATerm) {
    struct Case {
        std::uint32_t type_val;
        std::uint32_t len;
        parser::ValueType kind;
        std::uint8_t scale;
    };
    const Case cases[] = {
        {catalog::kTypeValInt8, 1, parser::ValueType::kInt, 0},
        {catalog::kTypeValInt16, 2, parser::ValueType::kInt, 0},
        {catalog::kTypeValInt32, 4, parser::ValueType::kInt, 0},
        {catalog::kTypeValInt64, 8, parser::ValueType::kInt, 0},
        {catalog::kTypeValUint64, 8, parser::ValueType::kInt, 0},
        {catalog::kTypeValBool, 1, parser::ValueType::kInt, 0},
        {catalog::kTypeValDate, 4, parser::ValueType::kInt, 0},
        {catalog::kTypeValTimestamp, 8, parser::ValueType::kInt, 0},
        {catalog::kTypeValDecimal, catalog::PackDecimalLen(12, 2), parser::ValueType::kDecimal, 2},
        {catalog::kTypeValDecimalWide, catalog::PackDecimalLen(30, 4),
         parser::ValueType::kDecimalWide, 4},
    };
    for (const Case& c : cases) {
        catalog::Schema schema;
        schema.columns.push_back(Column(0, "id", catalog::kTypeValInt64, 8));
        schema.columns.push_back(Column(1, "c", c.type_val, c.len));
        auto layout = catalog::RowLayout::Build(schema, /*inline_cell_width=*/64);
        ASSERT_TRUE(layout.ok()) << c.type_val << ": " << layout.status().message();
        parser::AstValue lit;
        lit.type = c.kind;
        lit.int_val = 1;
        lit.scale = c.scale;
        for (const parser::CompareOp op :
             {parser::CompareOp::kEq, parser::CompareOp::kNeq, parser::CompareOp::kLt,
              parser::CompareOp::kLte, parser::CompareOp::kGt, parser::CompareOp::kGte}) {
            EXPECT_TRUE(CompileScanKernelTerm(schema, layout.value(), 1, op, lit).has_value())
                << "type " << c.type_val << " op " << static_cast<int>(op);
        }
        lit.scale = static_cast<std::uint8_t>(c.scale + 1);
        if (c.kind != parser::ValueType::kInt) {
            EXPECT_FALSE(CompileScanKernelTerm(schema, layout.value(), 1, parser::CompareOp::kEq,
                                               lit)
                             .has_value())
                << "a decimal of another scale is the residual's (type " << c.type_val << ")";
        }
    }
}

// A correlated conjunct (a column on the right) and an outward left side are
// never compiled, so the inner build's bucketing predicate is never in a
// kernel; a literal conjunct on the step's own column is.
TEST(ScanKernelCodecTest, OnlyAnOwnColumnAgainstALiteralIsCompiled) {
    catalog::Schema schema;
    schema.columns.push_back(Column(0, "id", catalog::kTypeValInt64, 8));
    schema.columns.push_back(Column(1, "v", catalog::kTypeValInt64, 8));
    auto layout = catalog::RowLayout::Build(schema, /*inline_cell_width=*/64);
    ASSERT_TRUE(layout.ok());

    StepPredicate own;
    own.lhs = ColumnRef{0, 1, 1};
    own.op = parser::CompareOp::kGt;
    own.rhs.kind = OperandKind::kLiteral;
    own.rhs.literal.type = parser::ValueType::kInt;
    own.rhs.literal.int_val = 5;

    StepPredicate correlated = own;
    correlated.rhs.kind = OperandKind::kColumn;
    correlated.rhs.column = ColumnRef{1, 0, 1};

    StepPredicate outward = own;
    outward.lhs = ColumnRef{1, 0, 1};

    StepPredicate other_step = own;
    other_step.lhs = ColumnRef{0, 0, 1};

    const ScanKernel kernel = CompileScanKernel(schema, layout.value(),
                                                {correlated, outward, other_step, own},
                                                /*rel_slot=*/1);
    ASSERT_EQ(kernel.size(), 1u);
    EXPECT_EQ(kernel[0].op, parser::CompareOp::kGt);
    EXPECT_EQ(kernel[0].column, 1u);
}

}  // namespace
}  // namespace kds::exec
