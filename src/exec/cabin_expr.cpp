#include "kds/exec/cabin_expr.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <string>

#include "kds/exec/row_codec.hpp"

namespace kds::exec {
namespace {

std::string At(std::uint32_t byte_offset) {
    return " (byte " + std::to_string(byte_offset) + ")";
}

// ---- The canonical text -------------------------------------------------
//
//   term      := "$" pos | name "(" "$" pos { "," "$" pos } ")"
//   conjunct  := term " " op [ " " literal ]
//   literal   := "i" int [ "/" digits ]          a kInt, and a uint64's digits
//              | "s" len ":" bytes               a kStr, length-prefixed
//              | "d" scale ":" int               a kDecimal
//              | "w" scale ":" hi ":" lo         a kDecimalWide
//   predicate := conjunct { " AND " conjunct }
//
// Length-prefixed strings keep a literal holding " AND " from splitting a
// conjunct, and the reader never searches - it consumes.

std::string_view OpText(parser::CompareOp op) {
    switch (op) {
        case parser::CompareOp::kEq: return "=";
        case parser::CompareOp::kNeq: return "!=";
        case parser::CompareOp::kLt: return "<";
        case parser::CompareOp::kLte: return "<=";
        case parser::CompareOp::kGt: return ">";
        case parser::CompareOp::kGte: return ">=";
        case parser::CompareOp::kIsNull: return "isnull";
        case parser::CompareOp::kIsNotNull: return "notnull";
        case parser::CompareOp::kLike: return "like";
    }
    return "?";
}

// A uint64 literal's digits, without leading zeros: the text is compared,
// and `01000` and `1000` are one value.
std::string CanonicalDigits(const std::string& raw) {
    const std::size_t first = raw.find_first_not_of('0');
    return first == std::string::npos ? std::string("0") : raw.substr(first);
}

// `masked` folds the literal to its kind, for `expr_id` (AR1 §6: literals as
// shape).
void AppendLiteral(std::string& out, const parser::AstValue& v, bool masked) {
    switch (v.type) {
        case parser::ValueType::kInt:
            out += 'i';
            if (masked) return;
            out += std::to_string(v.int_val);
            // The digits only where they are the value - a literal past int64,
            // which coercion admits on a uint64 column alone - so a literal as
            // a statement wrote it and as a Cabin stored it write one text.
            if (parser::IntLiteralWrapped(v)) out += "/" + CanonicalDigits(v.raw_int_text);
            return;
        case parser::ValueType::kStr:
            out += 's';
            if (masked) return;
            out += std::to_string(v.str_val.size()) + ":" + v.str_val;
            return;
        case parser::ValueType::kDecimal:
            out += 'd';
            if (masked) return;
            out += std::to_string(v.scale) + ":" + std::to_string(v.int_val);
            return;
        case parser::ValueType::kDecimalWide:
            out += 'w';
            if (masked) return;
            out += std::to_string(v.scale) + ":" + std::to_string(v.dec_hi) + ":" +
                   std::to_string(v.int_val);
            return;
        case parser::ValueType::kNull:
        case parser::ValueType::kParam:
            // Refused at compile; never written.
            out += '?';
            return;
    }
}

std::string ConjunctText(const CabinConjunct& c, bool masked) {
    std::string out = CabinTermText(c.term);
    out += ' ';
    out += OpText(c.op);
    if (TakesLiteral(c.op)) {
        out += ' ';
        AppendLiteral(out, c.literal, masked);
    }
    return out;
}

std::uint64_t Fnv1a(std::string_view text) {
    std::uint64_t h = 14695981039346656037ULL;  // the FNV-1a 64-bit offset basis
    for (const char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;  // the FNV-1a 64-bit prime
    }
    return h;
}

// Fills `inputs`, `text` and `expr_id` from the lowered form, sorting and
// de-duplicating a predicate's conjuncts first. The one place both
// compilers end, so a stored text and a freshly compiled one cannot be
// built two ways.
void Finish(CompiledCabinExpr& f) {
    std::vector<std::uint16_t> inputs;
    std::string masked;
    if (f.shape == catalog::kCabinShapeExpression) {
        inputs = f.expr.args;
        f.text = CabinTermText(f.expr);
        masked = f.text;
    } else {
        std::vector<std::pair<std::string, CabinConjunct>> keyed;
        for (CabinConjunct& c : f.conjuncts) {
            keyed.emplace_back(CabinConjunctText(c), std::move(c));
        }
        std::sort(keyed.begin(), keyed.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        keyed.erase(std::unique(keyed.begin(), keyed.end(),
                                [](const auto& a, const auto& b) { return a.first == b.first; }),
                    keyed.end());
        f.conjuncts.clear();
        f.text.clear();
        for (auto& [text, c] : keyed) {
            if (!f.text.empty()) {
                f.text += " AND ";
                masked += " AND ";
            }
            f.text += text;
            masked += ConjunctText(c, /*masked=*/true);
            inputs.insert(inputs.end(), c.term.args.begin(), c.term.args.end());
            f.conjuncts.push_back(std::move(c));
        }
    }
    std::sort(inputs.begin(), inputs.end());
    inputs.erase(std::unique(inputs.begin(), inputs.end()), inputs.end());
    f.inputs = std::move(inputs);
    f.expr_id = Fnv1a(std::to_string(f.shape) + "|" + masked);
}

// ---- Compiling from the parse --------------------------------------------

StatusOr<std::uint16_t> ResolveColumn(const catalog::Schema& schema,
                                      const parser::ColumnName& name) {
    // Exact match and live columns only, as `CREATE CABIN`'s column form has
    // always resolved (cabin_ddl.cpp): a dropped column has no name (BW-R6).
    if (!name.qualifier.empty()) {
        return Status::InvalidArgument("a cabin's columns are its relation's; '" +
                                       name.qualifier + "." + name.name + "' is qualified" +
                                       At(name.byte_offset));
    }
    for (std::size_t i = 0; i < schema.columns.size(); ++i) {
        if (schema.columns[i].dropped() || catalog::NameView(schema.columns[i].name) != name.name) {
            continue;
        }
        if (!catalog::CabinPolicyPermitsCreation(schema.columns[i].cabin_policy)) {
            return Status::InvalidArgument("column '" + name.name +
                                           "' was declared NO CABIN" + At(name.byte_offset));
        }
        return static_cast<std::uint16_t>(i);
    }
    return Status::NotFound("no column '" + name.name + "'" + At(name.byte_offset));
}

StatusOr<CabinTerm> CompileCall(const parser::FunctionCall& call, const catalog::Schema& schema) {
    const FunctionEntry* fn = FindFunction(call.name);
    if (fn == nullptr) {
        return Status::InvalidArgument("unknown function '" + call.name + "'" +
                                       At(call.byte_offset));
    }
    // AQ-R4: `F` runs on the write path, once per row, and its value is
    // banked across statements - so it must be a function of its arguments
    // alone, forever.
    if (fn->purity != Purity::kImmutable) {
        return Status::InvalidArgument("function '" + std::string(fn->name) +
                                       "' is not immutable; a cabin's key may call only "
                                       "immutable functions" +
                                       At(call.byte_offset));
    }
    if (call.args.size() != fn->arity) {
        return Status::InvalidArgument("function '" + std::string(fn->name) + "' takes " +
                                       std::to_string(fn->arity) + " argument(s), got " +
                                       std::to_string(call.args.size()) + At(call.byte_offset));
    }
    CabinTerm term;
    term.fn = fn;
    for (const parser::ColumnName& arg : call.args) {
        auto pos = ResolveColumn(schema, arg);
        if (!pos.ok()) return pos.status();
        if (schema.columns[pos.value()].type_val != fn->arg_type_val) {
            return Status::InvalidArgument("function '" + std::string(fn->name) +
                                           "' does not take column '" + arg.name +
                                           "': it is not of the argument's type" +
                                           At(arg.byte_offset));
        }
        term.args.push_back(pos.value());
    }
    return term;
}

StatusOr<CabinConjunct> CompileConjunct(const parser::Condition& cond,
                                        const catalog::Schema& schema) {
    const auto not_admitted = [](std::string what, std::uint32_t at) {
        return Status::NotImplemented("a predicate cabin is a conjunction of comparisons "
                                      "of a column or an immutable call with a literal; " +
                                      what + " is not one" + At(at));
    };
    if (cond.has_subquery()) return not_admitted("a subquery", cond.col.byte_offset);

    CabinConjunct out;
    out.op = cond.op;
    std::uint32_t at = cond.col.byte_offset;
    switch (cond.kind) {
        case parser::PredicateKind::kCompareValue: {
            if (cond.rhs_kind != parser::RhsKind::kLiteral) {
                return not_admitted("a comparison of two columns", cond.rhs_col.byte_offset);
            }
            auto pos = ResolveColumn(schema, cond.col);
            if (!pos.ok()) return pos.status();
            out.term.args.push_back(pos.value());
            out.type_val = schema.columns[pos.value()].type_val;
            break;
        }
        case parser::PredicateKind::kCompareFunction: {
            if (!cond.lhs_fn.has_value() || cond.rhs_fn.has_value() ||
                cond.rhs_kind != parser::RhsKind::kLiteral) {
                const std::uint32_t where = cond.rhs_fn.has_value() ? cond.rhs_fn->byte_offset
                                            : cond.lhs_fn.has_value() ? cond.lhs_fn->byte_offset
                                                                      : at;
                return not_admitted("a call compared with anything but a literal", where);
            }
            at = cond.lhs_fn->byte_offset;
            auto term = CompileCall(*cond.lhs_fn, schema);
            if (!term.ok()) return term.status();
            out.term = std::move(term.value());
            out.type_val = out.term.fn->result_type_val;
            break;
        }
        case parser::PredicateKind::kBetween:
            // AQ-R6 matches conjunct for conjunct with no normalisation, so a
            // BETWEEN would match only a BETWEEN - and lowers to two bounds a
            // statement writes either way. Asked for as the two it means.
            return not_admitted("BETWEEN (write its two bounds as two conjuncts)", at);
        case parser::PredicateKind::kBoolTree:
            return not_admitted("OR or NOT", at);
        default:
            return not_admitted("a subquery", at);
    }

    if (out.op == parser::CompareOp::kLike) return not_admitted("LIKE", at);
    if (!TakesLiteral(out.op)) return out;

    out.literal = cond.val;
    if (out.literal.type == parser::ValueType::kNull) {
        return Status::InvalidArgument(
            "a comparison with NULL holds for no row; write IS NULL" +
            At(out.literal.byte_offset));
    }
    // Coerced as the statement's own conjunct is (step_compiler.cpp's
    // `CoercePredicate`, `LowerFunctionConjunct`): through the shared
    // coercion, against the column - or, for a call, a column of its result
    // type - so the Cabin's literal and a statement's are one value.
    catalog::SysColumnRow as{};
    if (out.term.fn == nullptr) {
        as = schema.columns[out.term.args.front()];
    } else {
        as.type_val = out.type_val;
    }
    if (Status s = CoerceLiteralToColumn(as, out.literal); !s.ok()) {
        return s.WithContext("a cabin predicate's literal" + At(out.literal.byte_offset));
    }
    // An integer's digit text is kept only where it is the value - a literal
    // past int64, which coercion admits on a uint64 column alone, as unsigned
    // digits - and dropped everywhere else, where `int_val` is (row_codec.hpp's
    // `ValueAsUint64`). Kept as written, `-5`'s text was stored, then refused
    // by the reader, and its relation failed every fill.
    if (out.literal.type == parser::ValueType::kInt) {
        out.literal.raw_int_text = parser::IntLiteralWrapped(out.literal)
                                       ? CanonicalDigits(out.literal.raw_int_text)
                                       : std::string();
    }
    return out;
}

// ---- Decoding the stored text --------------------------------------------

struct Reader {
    std::string_view text;
    std::size_t pos = 0;

    bool Eat(std::string_view s) {
        if (text.substr(pos, s.size()) != s) return false;
        pos += s.size();
        return true;
    }
    bool AtEnd() const { return pos >= text.size(); }
    template <typename T>
    bool Number(T& out) {
        const char* first = text.data() + pos;
        const char* last = text.data() + text.size();
        auto [ptr, ec] = std::from_chars(first, last, out);
        if (ec != std::errc() || ptr == first) return false;
        pos += static_cast<std::size_t>(ptr - first);
        return true;
    }
    std::string_view Until(char stop) {
        const std::size_t end = text.find(stop, pos);
        const std::size_t to = end == std::string_view::npos ? text.size() : end;
        std::string_view out = text.substr(pos, to - pos);
        pos = to;
        return out;
    }
};

bool ReadPosition(Reader& r, const catalog::Schema& schema, std::uint16_t& out) {
    if (!r.Eat("$") || !r.Number(out)) return false;
    return out < schema.columns.size() && !schema.columns[out].dropped();
}

bool ReadTerm(Reader& r, const catalog::Schema& schema, CabinTerm& out) {
    if (r.text.substr(r.pos, 1) == "$") {
        std::uint16_t pos = 0;
        if (!ReadPosition(r, schema, pos)) return false;
        out.args.push_back(pos);
        return true;
    }
    const std::string_view name = r.Until('(');
    out.fn = FindFunction(name);
    if (out.fn == nullptr || out.fn->purity != Purity::kImmutable || !r.Eat("(")) return false;
    do {
        std::uint16_t pos = 0;
        if (!ReadPosition(r, schema, pos)) return false;
        if (schema.columns[pos].type_val != out.fn->arg_type_val) return false;
        out.args.push_back(pos);
    } while (r.Eat(","));
    return r.Eat(")") && out.args.size() == out.fn->arity;
}

bool ReadLiteral(Reader& r, parser::AstValue& out) {
    if (r.Eat("i")) {
        out.type = parser::ValueType::kInt;
        if (!r.Number(out.int_val)) return false;
        if (r.Eat("/")) {
            const std::size_t first = r.pos;
            while (!r.AtEnd() && std::isdigit(static_cast<unsigned char>(r.text[r.pos]))) ++r.pos;
            out.raw_int_text = std::string(r.text.substr(first, r.pos - first));
            if (out.raw_int_text.empty()) return false;
        }
        return true;
    }
    if (r.Eat("s")) {
        out.type = parser::ValueType::kStr;
        std::size_t len = 0;
        if (!r.Number(len) || !r.Eat(":") || r.text.size() - r.pos < len) return false;
        out.str_val = std::string(r.text.substr(r.pos, len));
        r.pos += len;
        return true;
    }
    if (r.Eat("d")) {
        out.type = parser::ValueType::kDecimal;
        unsigned scale = 0;
        if (!r.Number(scale) || !r.Eat(":") || !r.Number(out.int_val)) return false;
        out.scale = static_cast<std::uint8_t>(scale);
        return true;
    }
    if (r.Eat("w")) {
        out.type = parser::ValueType::kDecimalWide;
        unsigned scale = 0;
        if (!r.Number(scale) || !r.Eat(":") || !r.Number(out.dec_hi) || !r.Eat(":") ||
            !r.Number(out.int_val)) {
            return false;
        }
        out.scale = static_cast<std::uint8_t>(scale);
        return true;
    }
    return false;
}

bool ReadConjunct(Reader& r, const catalog::Schema& schema, CabinConjunct& out) {
    if (!ReadTerm(r, schema, out.term) || !r.Eat(" ")) return false;
    const std::string_view op = r.Until(' ');
    static constexpr parser::CompareOp kOps[] = {
        parser::CompareOp::kEq,  parser::CompareOp::kNeq, parser::CompareOp::kLt,
        parser::CompareOp::kLte, parser::CompareOp::kGt,  parser::CompareOp::kGte,
        parser::CompareOp::kIsNull, parser::CompareOp::kIsNotNull,
    };
    const auto* found = std::find_if(std::begin(kOps), std::end(kOps),
                                     [&](parser::CompareOp o) { return OpText(o) == op; });
    if (found == std::end(kOps)) return false;
    out.op = *found;
    out.type_val = out.term.fn != nullptr ? out.term.fn->result_type_val
                                          : schema.columns[out.term.args.front()].type_val;
    if (!TakesLiteral(out.op)) return true;
    return r.Eat(" ") && ReadLiteral(r, out.literal);
}

// The value as an operator would write it: the shared renderer, quoted
// where the grammar quotes it.
std::string QuoteLiteral(const parser::AstValue& v, std::uint32_t type_val) {
    const std::string text = FormatValue(type_val, v);
    const bool quoted = v.type == parser::ValueType::kStr ||
                        type_val == catalog::kTypeValDate || type_val == catalog::kTypeValTimestamp;
    return quoted ? "'" + text + "'" : text;
}

std::string RenderTerm(const CabinTerm& term, const catalog::Schema& schema) {
    const auto name = [&](std::uint16_t pos) {
        return pos < schema.columns.size() ? std::string(catalog::NameView(schema.columns[pos].name))
                                           : "$" + std::to_string(pos);
    };
    if (term.fn == nullptr) return name(term.args.front());
    std::string out;
    for (const char c : term.fn->name) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    out += '(';
    for (std::size_t i = 0; i < term.args.size(); ++i) {
        if (i > 0) out += ", ";
        out += name(term.args[i]);
    }
    return out + ')';
}

}  // namespace

bool TakesLiteral(parser::CompareOp op) {
    return op != parser::CompareOp::kIsNull && op != parser::CompareOp::kIsNotNull;
}

parser::AstValue PredicateKey() {
    parser::AstValue v;
    v.type = parser::ValueType::kInt;
    v.int_val = 1;
    return v;
}

std::string CabinTermText(const CabinTerm& term) {
    if (term.fn == nullptr) return "$" + std::to_string(term.args.front());
    std::string out(term.fn->name);
    out += '(';
    for (std::size_t i = 0; i < term.args.size(); ++i) {
        if (i > 0) out += ',';
        out += "$" + std::to_string(term.args[i]);
    }
    return out + ')';
}

std::string CabinConjunctText(const CabinConjunct& conjunct) {
    return ConjunctText(conjunct, /*masked=*/false);
}

StatusOr<CompiledCabinExpr> CompileCabinExpr(const parser::CabinStmt& stmt,
                                             const catalog::Schema& schema) {
    CompiledCabinExpr f;
    f.shape = stmt.shape;
    if (stmt.shape == catalog::kCabinShapeExpression) {
        auto term = CompileCall(*stmt.expr, schema);
        if (!term.ok()) return term.status();
        f.expr = std::move(term.value());
    } else if (stmt.shape == catalog::kCabinShapePredicate) {
        for (const parser::Condition& cond : stmt.predicate) {
            auto c = CompileConjunct(cond, schema);
            if (!c.ok()) return c.status();
            f.conjuncts.push_back(std::move(c.value()));
        }
    } else {
        return Status::Corruption("a column cabin has no compiled key function");
    }
    Finish(f);

    // The tuple key's Cabin is the clustered tree (cabin.md §2), and a key
    // function of the tuple key alone is a function of what the tree already
    // keys on.
    if (f.inputs.size() == 1 && f.inputs.front() == 0) {
        return Status::InvalidArgument(
            "a cabin's key reads only the tuple key, which the clustered tree already keys on" +
            At(stmt.expr_byte_offset));
    }
    // A key no column feeds is one value for every row: no Cabin at all.
    if (f.inputs.empty()) {
        return Status::InvalidArgument("a cabin's key reads no column" +
                                       At(stmt.expr_byte_offset));
    }
    return f;
}

StatusOr<CompiledCabinExpr> DecodeCabinExpr(std::uint8_t shape, std::string_view text,
                                             const catalog::Schema& schema) {
    CompiledCabinExpr f;
    f.shape = shape;
    Reader r{text};
    bool ok = false;
    if (shape == catalog::kCabinShapeExpression) {
        ok = ReadTerm(r, schema, f.expr) && f.expr.fn != nullptr && r.AtEnd();
    } else if (shape == catalog::kCabinShapePredicate) {
        do {
            CabinConjunct c;
            ok = ReadConjunct(r, schema, c);
            if (ok) f.conjuncts.push_back(std::move(c));
        } while (ok && r.Eat(" AND "));
        ok = ok && r.AtEnd();
    }
    if (!ok) {
        return Status::Corruption("sys.cabin_exprs: a key function this build cannot read: '" +
                                  std::string(text) + "'");
    }
    Finish(f);
    // What was stored is what this build would write, or the stored identity
    // and the compared one are two strings.
    if (f.text != text) {
        return Status::Corruption("sys.cabin_exprs: a key function not in canonical form: '" +
                                  std::string(text) + "'");
    }
    return f;
}

std::string RenderCabinExpr(const CompiledCabinExpr& f, const catalog::Schema& schema) {
    if (f.shape == catalog::kCabinShapeExpression) return RenderTerm(f.expr, schema);
    std::string out;
    for (const CabinConjunct& c : f.conjuncts) {
        if (!out.empty()) out += " AND ";
        out += RenderTerm(c.term, schema);
        switch (c.op) {
            case parser::CompareOp::kIsNull: out += " IS NULL"; continue;
            case parser::CompareOp::kIsNotNull: out += " IS NOT NULL"; continue;
            default: break;
        }
        out += ' ';
        out += OpText(c.op);
        out += ' ';
        out += QuoteLiteral(c.literal, c.type_val);
    }
    return out;
}

namespace {

// `term`'s value on `row`: the column's, or the call's - NULL when any
// argument is, before the call (functions.hpp's contract).
parser::AstValue TermValue(const CabinTerm& term, std::span<const parser::AstValue> row) {
    if (term.fn == nullptr) return row[term.args.front()];
    std::array<const parser::AstValue*, kMaxFunctionArgs> args{};
    for (std::size_t i = 0; i < term.args.size(); ++i) {
        const parser::AstValue& v = row[term.args[i]];
        if (v.type == parser::ValueType::kNull) return parser::AstValue{};
        args[i] = &v;
    }
    // An immutable call reads no statement constant, so the default context
    // is the one every row gets.
    return term.fn->evaluate(std::span<const parser::AstValue* const>(args.data(), term.args.size()),
                             StatementContext{});
}

}  // namespace

std::optional<parser::AstValue> CabinKeyOf(const CompiledCabinExpr& f,
                                           std::span<const parser::AstValue> row) {
    if (f.shape == catalog::kCabinShapeExpression) {
        parser::AstValue v = TermValue(f.expr, row);
        if (v.type == parser::ValueType::kNull) return std::nullopt;
        return v;
    }
    for (const CabinConjunct& c : f.conjuncts) {
        const parser::AstValue v = TermValue(c.term, row);
        if (!CompareValues(c.type_val, v, c.literal, c.op)) return std::nullopt;
    }
    return PredicateKey();
}

}  // namespace kds::exec
