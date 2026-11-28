#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "kds/parser/kept_parse.hpp"
#include "kds/parser/parser.hpp"

// EA-S1 (`instructions/v3.2.0/workorder-ea-prepared-statement-reuse.md`):
// a kept parse bound with a statement's own literals is that statement's
// parse.
//
// **EA-R1 `[quiet-wrong]`, at the AST**: every statement of the V01 golden
// corpus and of `kExtra` below is taken apart into its `?` form - each
// literal token written `?` - and, where that form keeps a parse, the parse
// is bound with the literals it lost. The bound AST must be the text's
// parse, value for value and type for type. The comparison is `Dump`'s,
// which is written over every field of the statement kinds a parse keeps
// and is not the bind's own walk, so a position the walk missed shows as a
// hole left in the dump. Byte offsets are left out, and only they: a bound
// value's byte is the marker's in the `?` text (EA-R3), and everything
// after a marker sits at another byte than in the text.
//
// **EA-R2 at the fingerprint**: every field of the bound statement's
// fingerprint is the text's.

namespace kds::parser {
namespace {

// ---- An independent rendering of a parsed statement -----------------------

void D(std::ostream& o, const AstValue& v) {
    o << "V(" << static_cast<int>(v.type) << "," << v.int_val << ",'" << v.str_val << "',"
      << v.raw_int_text << "," << static_cast<int>(v.scale) << "," << v.dec_hi << ")";
}
void D(std::ostream& o, const ColumnName& c) {
    o << "C(" << c.qualifier << "." << c.name << "," << c.lens << ")";
}
void D(std::ostream& o, const SelectStmt& s);
void D(std::ostream& o, const BoolExpr& b);
void D(std::ostream& o, const std::shared_ptr<const Expr>& e);

void D(std::ostream& o, const Condition& c) {
    o << "Cond(" << static_cast<int>(c.kind) << ",";
    D(o, c.col);
    o << "," << static_cast<int>(c.op) << "," << static_cast<int>(c.rhs_kind) << ",";
    D(o, c.val);
    D(o, c.val_high);
    D(o, c.rhs_col);
    for (const auto* fn : {&c.lhs_fn, &c.rhs_fn}) {
        if (!fn->has_value()) {
            o << "fn-";
            continue;
        }
        o << "fn(" << (*fn)->name;
        for (const ColumnName& a : (*fn)->args) D(o, a);
        o << ")";
    }
    if (c.subquery != nullptr) {
        o << "sub[";
        D(o, *c.subquery);
        o << "]";
    }
    if (c.tree != nullptr) {
        o << "tree[";
        D(o, *c.tree);
        o << "]";
    }
    o << c.at_most_one << ")";
}
void D(std::ostream& o, const BoolExpr& b) {
    o << "B(" << static_cast<int>(b.op) << ",";
    D(o, b.leaf);
    for (const BoolExpr& child : b.children) D(o, child);
    o << ")";
}
void D(std::ostream& o, const SelectItem& i) {
    o << "I(" << i.is_aggregate << static_cast<int>(i.func) << i.star_arg << i.distinct;
    D(o, i.column);
    o << ")";
}
void D(std::ostream& o, const RelationRef& r) {
    o << "R(" << r.schema << "." << r.table_name << " " << r.alias << ")";
}
void D(std::ostream& o, const SelectStmt& s) {
    o << "Select(";
    for (const ColumnName& c : s.projection) D(o, c);
    D(o, s.from);
    for (const JoinClause& j : s.joins) {
        D(o, j.relation);
        D(o, j.left);
        D(o, j.right);
    }
    o << s.shares_parent_names;
    for (const Condition& c : s.where) D(o, c);
    for (const SelectItem& i : s.agg_items) D(o, i);
    for (const ColumnName& c : s.group_by) D(o, c);
    for (const HavingCondition& h : s.having) {
        D(o, h.agg);
        o << static_cast<int>(h.op);
        D(o, h.val);
    }
    for (const SortKey& k : s.order_by) {
        D(o, k.key);
        o << k.descending;
    }
    o << "L" << (s.limit.has_value() ? std::to_string(*s.limit) : "-") << "O" << s.offset
      << "F" << s.lock.has_value() << ")";
}
void D(std::ostream& o, const std::shared_ptr<const Expr>& e) {
    if (e == nullptr) {
        o << "E-";
        return;
    }
    o << "E(" << static_cast<int>(e->kind) << ",";
    D(o, e->literal);
    o << e->bare_numeric << "," << e->qualifier << "." << e->column << ","
      << static_cast<int>(e->op) << ",";
    D(o, e->lhs);
    D(o, e->rhs);
    for (const auto& a : e->args) D(o, a);
    o << "T(" << e->cast_to.name << e->cast_to.has_precision << e->cast_to.precision
      << e->cast_to.scale << e->cast_to.has_width << e->cast_to.width << ")" << e->function
      << ",'" << e->subquery_sql << "'," << (e->query != nullptr) << static_cast<int>(e->agg_func)
      << e->agg_distinct << e->height << "/" << e->size;
    if (e->subquery != nullptr) {
        o << "sub[";
        D(o, *e->subquery);
        o << "]";
    }
    o << ")";
}
void D(std::ostream& o, const Assignment& a) {
    o << "A(" << a.col_name << ",";
    D(o, a.val);
    D(o, a.expr);
    o << ")";
}
void D(std::ostream& o, const Returning& r) {
    o << "Ret(";
    for (const OutputItem& i : r.items) {
        D(o, i.expr);
        o << i.alias << i.star << i.star_qualifier << "'" << i.text << "'";
    }
    o << ")";
}

std::string Dump(const Statement& stmt) {
    std::ostringstream o;
    if (const auto* s = std::get_if<SelectStmt>(&stmt)) {
        D(o, *s);
    } else if (const auto* i = std::get_if<InsertStmt>(&stmt)) {
        o << "Insert(" << i->schema << "." << i->table_name;
        for (const IndexColumnRef& c : i->columns) o << "," << c.name;
        for (const auto& row : i->rows) {
            o << "[";
            for (const AstValue& v : row) D(o, v);
            o << "]";
        }
        for (const auto& ve : i->value_exprs) {
            o << "VE" << ve.row << "," << ve.col;
            D(o, ve.expr);
        }
        o << (i->select != nullptr) << "'" << i->select_text << "'" << i->on_conflict_do_nothing
          << (i->conflict_target.empty() ? "-" : "");
        for (const IndexColumnRef& t : i->conflict_target) o << t.name << ",";
        o << i->on_conflict_do_update;
        for (const Assignment& a : i->conflict_assignments) D(o, a);
        D(o, i->returning);
        o << ")";
    } else if (const auto* u = std::get_if<UpdateStmt>(&stmt)) {
        o << "Update(" << u->schema << "." << u->table_name;
        for (const Assignment& a : u->assignments) D(o, a);
        for (const Condition& c : u->where) D(o, c);
        D(o, u->returning);
        o << ")";
    } else if (const auto* d = std::get_if<DeleteStmt>(&stmt)) {
        o << "Delete(" << d->schema << "." << d->table_name;
        for (const Condition& c : d->where) D(o, c);
        D(o, d->returning);
        o << ")";
    } else {
        o << "other";
    }
    return o.str();
}

// ---- A statement taken apart into its `?` form ----------------------------

struct Apart {
    std::string holes;  // the text, each literal token written `?`
    std::vector<HoleValue> values;
};

std::optional<Apart> TakeApart(const std::string& sql) {
    Apart out;
    Lexer lexer(sql);
    std::size_t copied = 0;
    for (Token tok = lexer.Next(); tok.type != TokenType::kEof; tok = lexer.Next()) {
        // A statement already holding a `?` has no text form to compare with.
        if (tok.type == TokenType::kError || tok.type == TokenType::kParam) return std::nullopt;
        if (tok.type != TokenType::kIntLit && tok.type != TokenType::kNumLit &&
            tok.type != TokenType::kStrLit) {
            continue;
        }
        out.holes += sql.substr(copied, tok.byte_offset - copied);
        out.holes += "?";
        copied = tok.byte_offset + tok.length;
        if (tok.type == TokenType::kStrLit) {
            out.values.push_back(HoleValueOfString(std::string(tok.text)));
        } else {
            auto v = HoleValueOfLiteral(tok.text);
            if (!v.ok()) return std::nullopt;
            out.values.push_back(std::move(v.value()));
        }
    }
    out.holes += sql.substr(copied);
    return out;
}

// What the comparison found for one statement: whether its `?` form kept a
// parse, and the failure text where the bound statement is not the text's.
struct Verdict {
    bool kept = false;
    std::string failure;
};

Verdict Compare(const std::string& sql) {
    Verdict out;
    auto apart = TakeApart(sql);
    if (!apart.has_value()) return out;
    auto kept = KeptParse::Of(apart->holes);
    if (!kept.has_value()) return out;
    out.kept = true;
    auto bound = Bind(std::make_shared<const KeptParse>(std::move(*kept)),
                      std::move(apart->values), sql);
    if (!bound.ok()) {
        out.failure = "bind refused: " + bound.status().message();
        return out;
    }
    Parser parser(sql);
    auto parsed = parser.Parse();
    if (!parsed.ok()) {
        out.failure = "the text refuses what the kept parse binds: " + parsed.status().message();
        return out;
    }
    const std::string want = Dump(parsed.value());
    const std::string got = Dump(bound.value()->Materialize());
    if (want != got) out.failure = "AST differs:\n text  " + want + "\n bound " + got;
    const auto fp = parser.fingerprint();
    const Fingerprint& b = bound.value()->fingerprint;
    if (!fp.has_value() || fp->pattern_id != b.pattern_id || fp->fetch_id != b.fetch_id ||
        fp->arg_hash != b.arg_hash || fp->literal_count != b.literal_count ||
        fp->param_count != b.param_count) {
        out.failure += " fingerprint differs";
    }
    return out;
}

std::vector<std::string> CorpusStatements() {
    std::ifstream in(KDS_PARSER_CORPUS);
    EXPECT_TRUE(in.is_open());
    std::vector<std::string> out;
    std::string line;
    while (std::getline(in, line)) {
        std::size_t pos = 0;
        auto skip_space = [&] {
            while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
        };
        skip_space();
        if (pos >= line.size() || line[pos] == '#') continue;
        for (int field = 0; field < 4; ++field) {
            while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
            skip_space();
        }
        std::string sql;
        for (std::size_t i = pos; i < line.size(); ++i) {
            if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == 'n') {
                sql.push_back('\n');
                ++i;
            } else {
                sql.push_back(line[i]);
            }
        }
        if (!sql.empty()) out.push_back(sql);
    }
    return out;
}

// Shapes the corpus holds few of: every value position the walk reaches.
const char* const kExtra[] = {
    "SELECT * FROM t WHERE id = 5",
    "SELECT a, b FROM t WHERE a = -7 AND b != 'x' AND c >= 12.50",
    "SELECT * FROM t WHERE id IN (1, 2, 3) AND v NOT IN ('a', 'b')",
    "SELECT * FROM t WHERE v BETWEEN 3 AND 9 AND w NOT BETWEEN 1.5 AND 2.5",
    "SELECT * FROM t WHERE name LIKE 'ab%' OR (v = 1 AND NOT w = 2)",
    "SELECT * FROM a JOIN b ON a.x = b.y WHERE a.id = 10 AND b.v < 3",
    "SELECT * FROM t WHERE v IN (SELECT w FROM u WHERE z = 4)",
    "SELECT * FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.k = 2)",
    "SELECT v, COUNT(*) FROM t WHERE id > 0 GROUP BY v HAVING COUNT(*) > 3",
    "SELECT * FROM t WHERE d = '2026-01-31' AND ts < '2026-01-31 10:00:00'",
    "SELECT * FROM t WHERE id = 5 FOR UPDATE",
    "SELECT * FROM t WHERE id = 18446744073709551615",
    "UPDATE t SET v = 5, w = 'x' WHERE id = 9",
    "UPDATE t SET v = v + 1.5, w = w || 'y' WHERE id = 9",
    "UPDATE t SET v = CASE WHEN v > 3 THEN 1 ELSE 2 END WHERE id = 1",
    "UPDATE t SET v = COALESCE(w, 4), x = GREATEST(v, 2, 3) WHERE id = 1",
    "UPDATE t SET v = 3 * (w - 2) WHERE id IN (1, 2)",
    "UPDATE t SET v = CAST(7 AS int64) WHERE id = 1",
    "UPDATE t SET v = (SELECT MAX(w) FROM u WHERE k = 3) WHERE id = 1",
    "UPDATE t SET v = 1 WHERE id = 1 RETURNING v + 1",
    "DELETE FROM t WHERE id = 3",
    "DELETE FROM t WHERE v BETWEEN 1 AND 2 RETURNING id",
    "INSERT INTO t VALUES (1, 'a', 2.25)",
    "INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')",
    "INSERT INTO t (v, w) VALUES (5, 6 + 1)",
    "INSERT INTO t VALUES (1, 'a') ON CONFLICT (id) DO UPDATE SET v = 3, w = EXCLUDED.w",
    "INSERT INTO t VALUES (1, 2) ON CONFLICT DO NOTHING",
    "INSERT INTO t SELECT * FROM u WHERE k = 1",
    "SELECT * FROM t WHERE v = 'it''s'",
    "UPDATE t SET v = CASE -5 WHEN 1 THEN 10 ELSE 20 END WHERE id = 1",
    "UPDATE t SET v = w - -5, x = w * -3 WHERE id = -1",
    "INSERT INTO t VALUES (-1 + 2, -2.50 * 3, 'x' || 'y')",
};

TEST(KeptParseTest, ArgumentHashOfIsTheLexersFoldOverTheCorpus) {
    std::size_t checked = 0;
    for (const std::string& sql : CorpusStatements()) {
        const auto fp = FingerprintOf(sql);
        if (!fp.has_value()) continue;
        const auto args = ArgumentsOf(sql);
        EXPECT_EQ(ArgumentHashOf(args), fp->arg_hash) << sql;
        ++checked;
    }
    EXPECT_GT(checked, 150u);
}

TEST(KeptParseTest, EveryBoundStatementIsItsTextsParse) {
    std::size_t kept = 0;
    std::vector<std::string> statements = CorpusStatements();
    for (const char* extra : kExtra) statements.emplace_back(extra);
    for (const std::string& sql : statements) {
        const Verdict v = Compare(sql);
        if (v.kept) ++kept;
        EXPECT_TRUE(v.failure.empty()) << sql << "\n" << v.failure;
    }
    // Not vacuous: most accepted statements of both sets keep a parse.
    EXPECT_GT(kept, 120u);
}

TEST(KeptParseTest, EachShapeOfTheExtraSetKeepsWhereTheHeaderSaysItDoes) {
    // Kept: every value position the walk reaches.
    for (const char* sql : {"SELECT * FROM t WHERE id = 5",
                            "SELECT * FROM t WHERE v IN (SELECT w FROM u WHERE z = 4)",
                            "UPDATE t SET v = v + 1.5, w = w || 'y' WHERE id = 9",
                            "INSERT INTO t VALUES (1, 'a') ON CONFLICT (id) DO UPDATE SET v = 3, "
                            "w = EXCLUDED.w"}) {
        EXPECT_TRUE(Compare(sql).kept) << sql;
    }
    // Not kept: a hole the walk does not reach - a RETURNING item, a SET
    // value's scalar subquery, the query grammar's source - and a form the
    // native grammar does not read with a hole in it.
    for (const char* sql : {"UPDATE t SET v = 1 WHERE id = 1 RETURNING v + 1",
                            "UPDATE t SET v = (SELECT MAX(w) FROM u WHERE k = 3) WHERE id = 1",
                            "INSERT INTO t SELECT * FROM u WHERE k = 1",
                            "SELECT * FROM t LIMIT 5",
                            // `CASE -5 WHEN` is the column `case` minus 5 to
                            // the grammar, `CASE ? WHEN` a simple CASE.
                            "UPDATE t SET v = CASE -5 WHEN 1 THEN 10 ELSE 20 END WHERE id = 1"}) {
        EXPECT_FALSE(Compare(sql).kept) << sql;
    }
}

TEST(KeptParseTest, AHoleAgainstAByteItCouldFuseWithKeepsNoParse) {
    for (const char* sql : {"UPDATE t SET v = 100 -? WHERE id = 1", "SELECT * FROM t WHERE v = ?1",
                            "SELECT * FROM t WHERE v =?.5", "SELECT * FROM t WHERE v = ??"}) {
        EXPECT_FALSE(KeptParse::Of(sql).has_value()) << sql;
    }
    EXPECT_TRUE(KeptParse::Of("UPDATE t SET v = 100 - ? WHERE id = ?").has_value());
    EXPECT_TRUE(KeptParse::Of("SELECT * FROM t WHERE v=?").has_value());
}

TEST(KeptParseTest, AQuoteBindsAndTheMarkersByteIsTheValues) {
    auto kept = KeptParse::Of("SELECT * FROM t WHERE name = ? AND id = ?");
    ASSERT_TRUE(kept.has_value());
    ASSERT_EQ(kept->holes(), 2u);
    auto id = HoleValueOfLiteral("7");
    ASSERT_TRUE(id.ok());
    auto bound = Bind(std::make_shared<const KeptParse>(std::move(*kept)),
                      {HoleValueOfString("O'Brien"), id.value()}, "unused");
    ASSERT_TRUE(bound.ok());
    const Statement stmt = bound.value()->Materialize();
    const auto& select = std::get<SelectStmt>(stmt);
    ASSERT_EQ(select.where.size(), 2u);
    EXPECT_EQ(select.where[0].val.type, ValueType::kStr);
    EXPECT_EQ(select.where[0].val.str_val, "O'Brien");
    EXPECT_EQ(select.where[0].val.byte_offset, 29u) << "the marker's byte (EA-R3)";
    EXPECT_EQ(select.where[1].val.int_val, 7);
    EXPECT_EQ(bound.value()->arguments.size(), 2u);
    EXPECT_EQ(bound.value()->arguments[0].text, "O'Brien");
}

TEST(KeptParseTest, ACountThatDisagreesIsRefused) {
    auto kept = KeptParse::Of("SELECT * FROM t WHERE id = ?");
    ASSERT_TRUE(kept.has_value());
    auto bound = Bind(std::make_shared<const KeptParse>(std::move(*kept)), {}, "");
    EXPECT_FALSE(bound.ok());
}

TEST(KeptParseTest, ALiteralIsReadAsTheLexerReadsIt) {
    auto wide = HoleValueOfLiteral("18446744073709551615");
    ASSERT_TRUE(wide.ok());
    EXPECT_EQ(wide.value().value.int_val, -1) << "the lexer's wrap";
    EXPECT_EQ(wide.value().value.raw_int_text, "18446744073709551615");
    auto numeric = HoleValueOfLiteral("-12.50");
    ASSERT_TRUE(numeric.ok());
    EXPECT_EQ(numeric.value().value.type, ValueType::kStr);
    EXPECT_EQ(numeric.value().value.str_val, "-12.50");
    EXPECT_TRUE(numeric.value().bare_numeric);
    auto date = HoleValueOfLiteral("'2026-01-27'");
    ASSERT_TRUE(date.ok());
    EXPECT_EQ(date.value().value.str_val, "2026-01-27");
    EXPECT_FALSE(date.value().bare_numeric);
    for (const char* bad : {"", "-", "1.", ".5", "1e5", "1 2", "NULL", "x"}) {
        EXPECT_FALSE(HoleValueOfLiteral(bad).ok()) << bad;
    }
}

TEST(KeptParseTest, ATextPastTheCapKeepsNoParse) {
    std::string sql = "SELECT * FROM t WHERE id = ? AND name = '";
    sql.append(KeptParse::kMaxText, 'x');
    sql += "'";
    EXPECT_FALSE(KeptParse::Of(sql).has_value()) << "EA-R6";
}

}  // namespace
}  // namespace kds::parser
