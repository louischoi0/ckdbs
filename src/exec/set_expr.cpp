#include "kds/exec/set_expr.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string_view>
#include <utility>

#include <cstdint>

#include "kds/base/int128.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/type_literals.hpp"

namespace kds::exec {

namespace {

using catalog::kTypeValBool;
using catalog::kTypeValChar;
using catalog::kTypeValDate;
using catalog::kTypeValDecimal;
using catalog::kTypeValDecimalWide;
using catalog::kTypeValInt16;
using catalog::kTypeValInt32;
using catalog::kTypeValInt64;
using catalog::kTypeValInt8;
using catalog::kTypeValTimestamp;
using catalog::kTypeValUint64;
using catalog::kTypeValVarchar;
using parser::Expr;
using parser::ExprOp;
using parser::ValueType;

bool IsSignedInt(std::uint32_t t) {
    return t == kTypeValInt8 || t == kTypeValInt16 || t == kTypeValInt32 || t == kTypeValInt64;
}
bool IsInteger(std::uint32_t t) { return IsSignedInt(t) || t == kTypeValUint64; }
bool IsDecimal(std::uint32_t t) { return t == kTypeValDecimal || t == kTypeValDecimalWide; }
bool IsText(std::uint32_t t) { return t == kTypeValVarchar || t == kTypeValChar; }
bool IsTime(std::uint32_t t) { return t == kTypeValDate || t == kTypeValTimestamp; }

SetType Concrete(std::uint32_t type_val, std::uint32_t len) {
    SetType t;
    t.kind = SetType::Kind::kConcrete;
    t.type_val = type_val;
    // Only char and decimal read `len`; anywhere else it is a stored width
    // nothing consults, and comparing it would make two int64 columns two
    // types.
    t.len = (type_val == kTypeValChar || IsDecimal(type_val)) ? len : 0;
    return t;
}

SetType Untyped(SetType::Kind kind) {
    SetType t;
    t.kind = kind;
    return t;
}

// Exact type identity (BJ-Q2 (b)). A char's width is a per-row check, not a
// type, so only a decimal's (precision, scale) is compared.
bool SameType(const SetType& a, const SetType& b) {
    if (a.type_val != b.type_val) return false;
    return !IsDecimal(a.type_val) || a.len == b.len;
}

std::string At(std::uint32_t byte) { return " (byte " + std::to_string(byte) + ")"; }

std::string_view OpText(ExprOp op) {
    switch (op) {
        case ExprOp::kAdd: return "+";
        case ExprOp::kSub: return "-";
        case ExprOp::kMul: return "*";
        case ExprOp::kDiv: return "/";
        case ExprOp::kMod: return "%";
        case ExprOp::kConcat: return "||";
        case ExprOp::kNeg: return "-";
        case ExprOp::kPos: return "+";
        case ExprOp::kEq: return "=";
        case ExprOp::kNe: return "!=";
        case ExprOp::kLt: return "<";
        case ExprOp::kLe: return "<=";
        case ExprOp::kGt: return ">";
        case ExprOp::kGe: return ">=";
        case ExprOp::kAnd: return "AND";
        case ExprOp::kOr: return "OR";
        case ExprOp::kNot: return "NOT";
        case ExprOp::kIsNull: return "IS NULL";
        case ExprOp::kIsNotNull: return "IS NOT NULL";
        case ExprOp::kCase: return "CASE";
        case ExprOp::kCoalesce: return "COALESCE";
        case ExprOp::kNullif: return "NULLIF";
        case ExprOp::kGreatest: return "GREATEST";
        case ExprOp::kLeast: return "LEAST";
        case ExprOp::kCast: return "CAST";
    }
    return "?";
}

// The one shape of a typing refusal: what is wrong, the byte, and that no
// conversion rescues it (BJ-R3).
Status Refuse(std::uint32_t at, const std::string& what) {
    return Status::InvalidArgument(what + At(at) + "; there is no implicit conversion (BJ-R3)");
}

Status DivisionRefused(std::uint32_t at) {
    return Status::NotImplemented(
        "decimal division yields more digits than its operands and needs an explicit "
        "narrowing (ROUND), which BJ-S6 builds" +
        At(at));
}

bool IsComparison(ExprOp op) { return op >= ExprOp::kEq && op <= ExprOp::kGe; }
bool IsBooleanBinary(ExprOp op) {
    return IsComparison(op) || op == ExprOp::kAnd || op == ExprOp::kOr;
}

SetType BoolType() { return Concrete(kTypeValBool, 0); }

// What CAST converts between (BJ-S5, BJ-Q3). The classes are the reference
// dialect's families, restricted to the engine's types; a pair not listed is
// refused at compile, and `bool` converts to nothing but itself.
enum class Family : std::uint8_t { kInt, kDec, kText, kDate, kTimestamp, kBool, kOther };

Family FamilyOf(std::uint32_t type_val) {
    if (IsInteger(type_val)) return Family::kInt;
    if (IsDecimal(type_val)) return Family::kDec;
    if (IsText(type_val)) return Family::kText;
    if (type_val == kTypeValDate) return Family::kDate;
    if (type_val == kTypeValTimestamp) return Family::kTimestamp;
    if (type_val == kTypeValBool) return Family::kBool;
    return Family::kOther;
}

bool CastAdmitted(Family from, Family to) {
    switch (from) {
        case Family::kInt:
        case Family::kDec: return to == Family::kInt || to == Family::kDec || to == Family::kText;
        case Family::kText: return to != Family::kBool && to != Family::kOther;
        case Family::kDate: return to == Family::kText || to == Family::kDate || to == Family::kTimestamp;
        case Family::kTimestamp:
            return to == Family::kText || to == Family::kDate || to == Family::kTimestamp;
        case Family::kBool: return to == Family::kBool;
        case Family::kOther: return false;
    }
    return false;
}

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The type a CAST names. The names are the ones `sys.types` registers;
// they are listed here because the typer is pure and takes no catalog.
StatusOr<SetType> CastTarget(const parser::TypeSpec& spec) {
    const std::string name = Lower(spec.name);
    const std::string where = " (byte " + std::to_string(spec.byte_offset) + ")";
    const auto NoArguments = [&]() -> std::optional<Status> {
        if (spec.has_precision || spec.has_width) {
            return Status::InvalidArgument("type '" + spec.name + "' takes no arguments" + where);
        }
        return std::nullopt;
    };
    static const std::pair<const char*, std::uint32_t> kPlain[] = {
        {"int8", kTypeValInt8},   {"int16", kTypeValInt16}, {"int32", kTypeValInt32},
        {"int64", kTypeValInt64}, {"uint64", kTypeValUint64}, {"bool", kTypeValBool},
        {"date", kTypeValDate},   {"timestamp", kTypeValTimestamp}};
    for (const auto& [n, type_val] : kPlain) {
        if (name == n) {
            if (auto refused = NoArguments(); refused.has_value()) return *refused;
            return Concrete(type_val, 0);
        }
    }
    if (name == "varchar") {
        if (spec.has_width) {
            return Status::InvalidArgument(
                "varchar(N) names a column's cell width, which a CAST has none of; write varchar" +
                where);
        }
        return Concrete(kTypeValVarchar, 0);
    }
    if (name == "char") {
        const std::uint32_t width = spec.has_width ? spec.width : 1;
        if (width == 0) {
            return Status::InvalidArgument("char(0) holds nothing" + where);
        }
        return Concrete(kTypeValChar, width);
    }
    if (name == "decimal") {
        if (!spec.has_precision) {
            return Status::InvalidArgument("decimal needs a precision and a scale" + where);
        }
        const bool wide = spec.precision >= kMinDecimalPrecisionWide;
        if (Status s = wide ? CheckDecimalWidePrecisionScale(spec.precision, spec.scale)
                            : CheckDecimalPrecisionScale(spec.precision, spec.scale);
            !s.ok()) {
            return s.WithMessage(s.message() + where);
        }
        return Concrete(wide ? kTypeValDecimalWide : kTypeValDecimal,
                        catalog::PackDecimalLen(static_cast<std::uint8_t>(spec.precision),
                                                static_cast<std::uint8_t>(spec.scale)));
    }
    if (name == "float") {
        return Status::Unsupported("float is not a type this engine stores" + where);
    }
    return Status::InvalidArgument("unknown type '" + spec.name + "' in CAST" + where);
}

// A bare numeric literal `1.50` as the decimal it is written: its own digits
// are its precision and its fraction its scale. A CAST's operand has no
// other context to give it one.
StatusOr<SetType> DecimalTypeOfLiteral(const parser::AstValue& v) {
    std::uint32_t digits = 0;
    std::uint32_t fraction = 0;
    bool after_point = false;
    for (char c : v.str_val) {
        if (c == '.') {
            after_point = true;
        } else if (c >= '0' && c <= '9') {
            ++digits;
            if (after_point) ++fraction;
        }
    }
    digits = std::max(digits, fraction);
    if (digits == 0) digits = 1;
    if (digits > 38) {
        return Status::OutOfRange("a numeric literal of " + std::to_string(digits) +
                                  " digits exceeds 38" + At(v.byte_offset));
    }
    return Concrete(digits >= kMinDecimalPrecisionWide ? kTypeValDecimalWide : kTypeValDecimal,
                    catalog::PackDecimalLen(static_cast<std::uint8_t>(digits),
                                            static_cast<std::uint8_t>(fraction)));
}

class Typer {
public:
    Typer(const catalog::TableAccess& access) : access_(access) {}

    using Node = std::unique_ptr<TypedSetExpr>;

    // Types a node from its children alone. A literal stays untyped until
    // `Resolve` gives it its context's type, so a node's children are typed
    // once and a chain of n terms costs n.
    StatusOr<Node> Type(const Expr& e) {
        switch (e.kind) {
            case Expr::Kind::kLiteral: return Literal(e);
            case Expr::Kind::kColumn: return Column(e);
            case Expr::Kind::kUnary: return Unary(e);
            case Expr::Kind::kBinary: return Binary(e);
            case Expr::Kind::kCall: return Call(e);
        }
        return Status::Corruption("an expression node of an unknown kind");
    }

    // Gives the untyped literals under `n` the type `want`, in place and in
    // one walk.
    Status Resolve(TypedSetExpr& n, const SetType& want) const {
        if (n.type.concrete()) return Status::OK();
        switch (n.kind) {
            case Expr::Kind::kLiteral: return CoerceInto(n, want);
            case Expr::Kind::kColumn: return Status::OK();  // always concrete
            case Expr::Kind::kUnary: {
                if (Status s = Resolve(*n.lhs, want); !s.ok()) return s;
                n.type = n.lhs->type;
                return CheckUnary(n.op, n.type, n.op_byte_offset);
            }
            case Expr::Kind::kBinary: {
                if (n.op == ExprOp::kConcat && !IsText(want.type_val)) {
                    return Refuse(n.op_byte_offset,
                                  "operator || yields a string, not " + SetTypeName(want));
                }
                auto result = Settle(n.op, n.op_byte_offset, *n.lhs, *n.rhs, &want);
                if (!result.ok()) return result.status();
                n.type = result.value();
                return Status::OK();
            }
            case Expr::Kind::kCall: {
                // The value positions take the context's type; a CASE's
                // conditions are bool already.
                const std::size_t pairs = n.op == ExprOp::kCase ? n.args.size() / 2 : 0;
                for (std::size_t i = 0; i < n.args.size(); ++i) {
                    const bool condition = i < pairs * 2 && i % 2 == 0;
                    if (condition) continue;
                    if (Status s = Resolve(*n.args[i], want); !s.ok()) return s;
                }
                n.type = want;
                return Status::OK();
            }
        }
        return Status::OK();
    }

private:
    static Node Make(const Expr& e) {
        auto n = std::make_unique<TypedSetExpr>();
        n->kind = e.kind;
        n->op = e.op;
        n->byte_offset = e.byte_offset;
        n->op_byte_offset = e.op_byte_offset;
        return n;
    }

    StatusOr<Node> Literal(const Expr& e) {
        Node n = Make(e);
        n->literal = e.literal;
        switch (e.literal.type) {
            case ValueType::kNull: n->type = Untyped(SetType::Kind::kNull); break;
            case ValueType::kInt: n->type = Untyped(SetType::Kind::kInteger); break;
            case ValueType::kStr:
                n->type = Untyped(e.bare_numeric ? SetType::Kind::kNumeric
                                                 : SetType::Kind::kString);
                break;
            default:
                return Status::InvalidArgument("this literal cannot appear in an expression" +
                                               At(e.literal.byte_offset));
        }
        return n;
    }

    // Gives an untyped literal node `want` through the routines a bare
    // literal goes through at the encode (types.md §3.1), or says why the
    // literal cannot be that type. The routines' own messages name a column,
    // which a literal in an expression has none of, so the refusal is worded
    // here and keeps their code.
    Status CoerceInto(TypedSetExpr& n, const SetType& want) const {
        const SetType::Kind from = n.type.kind;
        if (from == SetType::Kind::kConcrete) return Status::OK();

        catalog::SysColumnRow col{};
        col.type_val = want.type_val;
        col.len = want.len;
        const auto Mismatch = [&] {
            return Refuse(n.byte_offset,
                          SetTypeName(n.type) + " cannot be used as " + SetTypeName(want));
        };
        const auto DoesNotFit = [&](const Status& s) {
            return s.WithMessage(SetTypeName(n.type) + " does not fit " + SetTypeName(want) +
                                 At(n.byte_offset));
        };

        switch (from) {
            case SetType::Kind::kNull:
                break;  // NULL is every type's value; the row decides NOT NULL (BJ-R4)
            case SetType::Kind::kInteger:
                if (IsInteger(want.type_val)) {
                    if (Status s = CheckIntegerLiteralFits(col, n.literal); !s.ok()) {
                        return DoesNotFit(s);
                    }
                } else if (IsDecimal(want.type_val)) {
                    if (Status s = CoerceLiteralToColumn(col, n.literal); !s.ok()) {
                        return DoesNotFit(s);
                    }
                } else {
                    return Mismatch();
                }
                break;
            case SetType::Kind::kNumeric:
                if (!IsDecimal(want.type_val)) return Mismatch();
                if (Status s = CoerceLiteralToColumn(col, n.literal); !s.ok()) {
                    return DoesNotFit(s);
                }
                break;
            case SetType::Kind::kString:
                // A string literal is what a date, a timestamp or a decimal is
                // written as (types.md TY7): the encode's own parse judges it.
                if (want.type_val == kTypeValDate || want.type_val == kTypeValTimestamp ||
                    IsDecimal(want.type_val)) {
                    if (Status s = CoerceLiteralToColumn(col, n.literal); !s.ok()) {
                        return DoesNotFit(s);
                    }
                } else if (!IsText(want.type_val)) {
                    return Mismatch();
                }
                break;
            case SetType::Kind::kConcrete:
                break;
        }
        n.type = want;
        return Status::OK();
    }

    StatusOr<Node> Column(const Expr& e) {
        const catalog::SysColumnRow* col = access_.schema.FindColumn(e.column);
        if (col == nullptr) {
            return Status::InvalidArgument("unknown column '" + e.column + "'" + At(e.byte_offset));
        }
        Node n = Make(e);
        n->column_no = static_cast<std::size_t>(col - access_.schema.columns.data());
        n->type = Concrete(col->type_val, col->len);
        return n;
    }

    // The unary signatures: a sign over a signed integer or a decimal, and a
    // `+` over an unsigned one too. An untyped operand is judged when its
    // context types it.
    static Status CheckUnary(ExprOp op, const SetType& t, std::uint32_t at) {
        if (t.kind == SetType::Kind::kString ||
            (t.concrete() && !(op == ExprOp::kPos ? (IsInteger(t.type_val) || IsDecimal(t.type_val))
                                                  : (IsSignedInt(t.type_val) ||
                                                     IsDecimal(t.type_val))))) {
            return Refuse(at, "operator " + std::string(OpText(op)) + " cannot apply to " +
                                  SetTypeName(t) +
                                  (t.concrete() && t.type_val == kTypeValUint64
                                       ? " (an unsigned value has no negation)"
                                       : ""));
        }
        return Status::OK();
    }

    StatusOr<Node> Unary(const Expr& e) {
        auto child = Type(*e.lhs);
        if (!child.ok()) return child;
        Node n = Make(e);
        switch (e.op) {
            case ExprOp::kNot:
                if (Status s = BoolOperand(*child.value(), "operator NOT", e.op_byte_offset);
                    !s.ok()) {
                    return s;
                }
                n->type = BoolType();
                break;
            case ExprOp::kIsNull:
            case ExprOp::kIsNotNull:
                if (Status s = SettleByDefault(*child.value()); !s.ok()) return s;
                n->type = BoolType();
                break;
            case ExprOp::kCast: return Cast(e, std::move(child.value()));
            default:
                n->type = child.value()->type;
                if (Status s = CheckUnary(e.op, n->type, e.op_byte_offset); !s.ok()) return s;
                break;
        }
        n->lhs = std::move(child.value());
        return n;
    }

    // An operand of `AND`, `OR`, `NOT` or a CASE condition: a bool, or a NULL,
    // which takes bool.
    Status BoolOperand(TypedSetExpr& n, const std::string& who, std::uint32_t at) const {
        if (n.type.concrete()) {
            if (n.type.type_val == kTypeValBool) return Status::OK();
        } else if (n.type.kind == SetType::Kind::kNull) {
            return Resolve(n, BoolType());
        }
        return Refuse(at, who + " needs a bool but is given " + SetTypeName(n.type));
    }

    // The type an operand with no context takes when nothing else will give
    // it one: an integer or a NULL is int64, a string varchar. A bare number
    // with a point has no default - which scale would it be?
    Status SettleByDefault(TypedSetExpr& n) const {
        if (n.type.concrete()) return Status::OK();
        switch (n.type.kind) {
            case SetType::Kind::kInteger:
            case SetType::Kind::kNull: return Resolve(n, Concrete(kTypeValInt64, 0));
            case SetType::Kind::kString: return Resolve(n, Concrete(kTypeValVarchar, 0));
            default:
                return Refuse(n.byte_offset,
                              "a numeric literal here has no type; give it a column to take "
                              "one from, or CAST it");
        }
    }

    // One common type for operands that must agree (a comparison, a CASE's
    // results, COALESCE, ...): the first concrete one's, every other concrete
    // one the same (the text family counts as one), and the untyped ones take
    // it. With none concrete the node stays untyped, of the kinds' merge,
    // until a context types it (`ctx`, when the caller has one).
    StatusOr<SetType> Unify(const std::vector<TypedSetExpr*>& values, const SetType* ctx,
                            std::uint32_t at, const std::string& who) const {
        const SetType* first = nullptr;
        for (const TypedSetExpr* v : values) {
            if (!v->type.concrete()) continue;
            if (first == nullptr) {
                first = &v->type;
            } else if (!SameType(*first, v->type) &&
                       !(IsText(first->type_val) && IsText(v->type.type_val))) {
                return Refuse(at, who + " needs one type but is given " + SetTypeName(*first) +
                                      " and " + SetTypeName(v->type));
            }
        }
        if (first != nullptr) {
            SetType t = *first;
            for (const TypedSetExpr* v : values) {
                if (v->type.concrete() && IsText(t.type_val) && v->type.type_val != t.type_val) {
                    t = Concrete(kTypeValVarchar, 0);  // char with varchar
                }
            }
            for (TypedSetExpr* v : values) {
                if (Status s = Resolve(*v, t); !s.ok()) return s;
            }
            return t;
        }
        if (ctx != nullptr) {
            for (TypedSetExpr* v : values) {
                if (Status s = Resolve(*v, *ctx); !s.ok()) return s;
            }
            return *ctx;
        }
        SetType::Kind merged = SetType::Kind::kNull;
        for (const TypedSetExpr* v : values) {
            const SetType::Kind k = v->type.kind;
            if (k == SetType::Kind::kNull) continue;
            if (merged == SetType::Kind::kNull) {
                merged = k;
            } else if (merged == SetType::Kind::kString || k == SetType::Kind::kString) {
                if (merged != k) {
                    return Refuse(at, who + " needs one type but is given " +
                                          SetTypeName(Untyped(merged)) + " and " +
                                          SetTypeName(Untyped(k)));
                }
            } else if (k == SetType::Kind::kNumeric) {
                merged = SetType::Kind::kNumeric;
            }
        }
        return Untyped(merged);
    }

    StatusOr<Node> BooleanBinary(const Expr& e, Node lhs, Node rhs) {
        Node n = Make(e);
        if (e.op == ExprOp::kAnd || e.op == ExprOp::kOr) {
            const std::string who = "operator " + std::string(OpText(e.op));
            if (Status s = BoolOperand(*lhs, who, e.op_byte_offset); !s.ok()) return s;
            if (Status s = BoolOperand(*rhs, who, e.op_byte_offset); !s.ok()) return s;
        } else {
            auto t = Unify({lhs.get(), rhs.get()}, nullptr, e.op_byte_offset,
                           "operator " + std::string(OpText(e.op)));
            if (!t.ok()) return t.status();
            // Two literals compare as what each is by default; the result is
            // a bool whatever they are, so nothing later types them.
            if (Status s = SettleByDefault(*lhs); !s.ok()) return s;
            if (Status s = SettleByDefault(*rhs); !s.ok()) return s;
            if (!SameType(lhs->type, rhs->type) &&
                !(IsText(lhs->type.type_val) && IsText(rhs->type.type_val))) {
                return Refuse(e.op_byte_offset, "operator " + std::string(OpText(e.op)) +
                                                    " needs one type but is given " +
                                                    SetTypeName(lhs->type) + " and " +
                                                    SetTypeName(rhs->type));
            }
        }
        n->type = BoolType();
        n->lhs = std::move(lhs);
        n->rhs = std::move(rhs);
        return n;
    }

    StatusOr<Node> Call(const Expr& e) {
        Node n = Make(e);
        for (const auto& arg : e.args) {
            auto a = Type(*arg);
            if (!a.ok()) return a;
            n->args.push_back(std::move(a.value()));
        }
        std::vector<TypedSetExpr*> values;
        if (e.op == ExprOp::kCase) {
            const std::size_t pairs = n->args.size() / 2;
            for (std::size_t i = 0; i < pairs; ++i) {
                if (Status s = BoolOperand(*n->args[2 * i], "a CASE condition",
                                           e.args[2 * i]->byte_offset);
                    !s.ok()) {
                    return s;
                }
                values.push_back(n->args[2 * i + 1].get());
            }
            if (n->args.size() % 2 == 1) values.push_back(n->args.back().get());
        } else {
            for (auto& a : n->args) values.push_back(a.get());
        }
        auto t = Unify(values, nullptr, e.op_byte_offset, std::string(OpText(e.op)));
        if (!t.ok()) return t.status();
        n->type = t.value();
        return n;
    }

    StatusOr<Node> Cast(const Expr& e, Node child) {
        auto target = CastTarget(e.cast_to);
        if (!target.ok()) return target.status();
        if (!child->type.concrete()) {
            Status s = Status::OK();
            switch (child->type.kind) {
                case SetType::Kind::kNull: s = Resolve(*child, target.value()); break;
                case SetType::Kind::kInteger: s = Resolve(*child, Concrete(kTypeValInt64, 0)); break;
                case SetType::Kind::kString: s = Resolve(*child, Concrete(kTypeValVarchar, 0)); break;
                default: {
                    if (child->kind != Expr::Kind::kLiteral) {
                        return Refuse(e.op_byte_offset,
                                      "a CAST of an expression of numeric literals has no type "
                                      "to convert from");
                    }
                    auto literal_type = DecimalTypeOfLiteral(child->literal);
                    if (!literal_type.ok()) return literal_type.status();
                    s = Resolve(*child, literal_type.value());
                }
            }
            if (!s.ok()) return s;
        }
        if (!CastAdmitted(FamilyOf(child->type.type_val), FamilyOf(target.value().type_val))) {
            return Status::InvalidArgument("CAST from " + SetTypeName(child->type) + " to " +
                                           SetTypeName(target.value()) + " is not supported" +
                                           At(e.op_byte_offset));
        }
        Node n = Make(e);
        n->type = target.value();
        n->lhs = std::move(child);
        return n;
    }

    StatusOr<Node> Binary(const Expr& e) {
        auto l = Type(*e.lhs);
        if (!l.ok()) return l;
        auto r = Type(*e.rhs);
        if (!r.ok()) return r;
        Node lhs = std::move(l.value());
        Node rhs = std::move(r.value());

        if (IsBooleanBinary(e.op)) return BooleanBinary(e, std::move(lhs), std::move(rhs));

        // Time types are refused by name, whatever the other operand is.
        for (const SetType& t : {lhs->type, rhs->type}) {
            if (t.concrete() && IsTime(t.type_val)) {
                if (t.type_val == kTypeValDate) {
                    return Status::NotImplemented(
                        "binary arithmetic on a date is not supported yet (BJ-S6)" +
                        At(e.op_byte_offset));
                }
                return Status::Unsupported(
                    "arithmetic on a timestamp is not supported: there is no INTERVAL type" +
                    At(e.op_byte_offset));
            }
        }

        Node n = Make(e);
        // With only literals under this node it stays untyped until a
        // context types it (`Resolve`).
        const auto result = lhs->type.concrete() || rhs->type.concrete()
                                ? Settle(e.op, e.op_byte_offset, *lhs, *rhs, nullptr)
                                : UntypedResult(e.op, e.op_byte_offset, lhs->type, rhs->type);
        if (!result.ok()) return result.status();
        n->type = result.value();
        n->lhs = std::move(lhs);
        n->rhs = std::move(rhs);
        return n;
    }

    // The kind of a node whose operands are both untyped.
    static StatusOr<SetType> UntypedResult(ExprOp op, std::uint32_t at, const SetType& a,
                                           const SetType& b) {
        constexpr auto K = SetType::Kind::kNull;
        const auto Bad = [&]() {
            return Refuse(at, "operator " + std::string(OpText(op)) + " cannot apply to " +
                                  SetTypeName(a) + " and " + SetTypeName(b));
        };
        if (op == ExprOp::kConcat) {
            const bool ok = (a.kind == SetType::Kind::kString || a.kind == K) &&
                            (b.kind == SetType::Kind::kString || b.kind == K);
            return ok ? StatusOr<SetType>(Untyped(SetType::Kind::kString))
                      : StatusOr<SetType>(Bad());
        }
        if (a.kind == SetType::Kind::kString || b.kind == SetType::Kind::kString) return Bad();
        if (a.kind == K) return b;
        if (b.kind == K) return a;
        return Untyped(a.kind == SetType::Kind::kNumeric || b.kind == SetType::Kind::kNumeric
                           ? SetType::Kind::kNumeric
                           : SetType::Kind::kInteger);
    }

    // Gives each untyped operand its type and returns the node's. An operand
    // takes the other one's type, or `ctx` when the other is untyped too.
    // **The one place a literal is a scalar:** under `*` or `/` against a
    // decimal, an integer literal (or an expression of them) stays an int64
    // of scale 0 and a NULL stays NULL, so `price * 2` and `price * NULL`
    // keep the decimal's own scale (BJ-R3); an int64 *column* is never one.
    StatusOr<SetType> Settle(ExprOp op, std::uint32_t at, TypedSetExpr& l, TypedSetExpr& r,
                             const SetType* ctx) const {
        const bool mul_div = op == ExprOp::kMul || op == ExprOp::kDiv;
        const bool l_untyped = !l.type.concrete();
        const bool r_untyped = !r.type.concrete();
        // Both targets are read before either operand is given its type.
        const SetType l_target = r_untyped ? (ctx != nullptr ? *ctx : SetType{}) : r.type;
        const SetType r_target = l_untyped ? (ctx != nullptr ? *ctx : SetType{}) : l.type;
        if ((l_untyped && r_untyped) && ctx == nullptr) {
            return Status::Corruption("an untyped pair of operands with no context");
        }

        bool l_scalar = false;
        bool r_scalar = false;
        const auto Give = [&](TypedSetExpr& u, const SetType& target, bool& scalar) {
            SetType t = target;
            if (mul_div && IsDecimal(target.type_val)) {
                if (u.type.kind == SetType::Kind::kInteger) {
                    t = Concrete(kTypeValInt64, 0);
                    scalar = true;
                } else if (u.type.kind == SetType::Kind::kNull) {
                    scalar = true;
                }
            }
            return Resolve(u, t);
        };
        if (l_untyped) {
            if (Status s = Give(l, l_target, l_scalar); !s.ok()) return s;
        }
        if (r_untyped) {
            if (Status s = Give(r, r_target, r_scalar); !s.ok()) return s;
        }
        return ResultType(op, at, l.type, r.type, l_scalar, r_scalar);
    }

    // The declared signatures of family 1 (BJ-R3), over two concrete types.
    static StatusOr<SetType> ResultType(ExprOp op, std::uint32_t at, const SetType& a,
                                        const SetType& b, bool a_scalar, bool b_scalar) {
        const auto Bad = [&]() {
            return Refuse(at, "operator " + std::string(OpText(op)) + " cannot apply to " +
                                  SetTypeName(a) + " and " + SetTypeName(b));
        };
        if (op == ExprOp::kConcat) {
            if (IsText(a.type_val) && IsText(b.type_val)) return Concrete(kTypeValVarchar, 0);
            return Bad();
        }

        if ((IsDecimal(a.type_val) && b_scalar) || (IsDecimal(b.type_val) && a_scalar)) {
            if (op == ExprOp::kMul) return IsDecimal(a.type_val) && b_scalar ? a : b;
            if (op == ExprOp::kDiv && IsDecimal(a.type_val)) return DivisionRefused(at);
            return Bad();
        }

        if (!SameType(a, b)) return Bad();
        const std::uint32_t t = a.type_val;
        if (IsInteger(t)) return a;
        if (IsDecimal(t)) {
            switch (op) {
                case ExprOp::kAdd:
                case ExprOp::kSub:
                case ExprOp::kMod: return a;
                case ExprOp::kMul: {
                    const std::uint32_t p = std::min<std::uint32_t>(
                        catalog::DecimalPrecisionOf(a.len) + catalog::DecimalPrecisionOf(b.len), 38);
                    const std::uint32_t s =
                        catalog::DecimalScaleOf(a.len) + catalog::DecimalScaleOf(b.len);
                    if (s > p) {
                        return Status::OutOfRange("the product of " + SetTypeName(a) + " and " +
                                                  SetTypeName(b) + " has scale " +
                                                  std::to_string(s) + " beyond 38" + At(at));
                    }
                    return Concrete(p <= 18 ? kTypeValDecimal : kTypeValDecimalWide,
                                    catalog::PackDecimalLen(static_cast<std::uint8_t>(p),
                                                            static_cast<std::uint8_t>(s)));
                }
                case ExprOp::kDiv: return DivisionRefused(at);
                default: return Bad();
            }
        }
        return Bad();
    }

    const catalog::TableAccess& access_;
};

// ---- Evaluation (BJ-S4) --------------------------------------------------

// Whole-number values, decimals' unscaled values and uint64 all ride one
// 128-bit integer, so one checked arithmetic serves every type and the range
// of the node's own type is what judges the result.
Int128 PowerOfTen(unsigned exponent) {
    // 10^0 .. 10^38, the widest decimal precision.
    static const std::array<Int128, 39> kTable = [] {
        std::array<Int128, 39> t{};
        t[0] = 1;
        for (std::size_t i = 1; i < t.size(); ++i) t[i] = t[i - 1] * 10;
        return t;
    }();
    return kTable[exponent];
}

bool InRange(Int128 v, const SetType& t) {
    switch (t.type_val) {
        case kTypeValInt8: return v >= INT8_MIN && v <= INT8_MAX;
        case kTypeValInt16: return v >= INT16_MIN && v <= INT16_MAX;
        case kTypeValInt32: return v >= INT32_MIN && v <= INT32_MAX;
        case kTypeValInt64: return v >= INT64_MIN && v <= INT64_MAX;
        case kTypeValUint64: return v >= 0 && v <= static_cast<Int128>(UINT64_MAX);
        case kTypeValDecimal:
        case kTypeValDecimalWide: {
            const Int128 bound = PowerOfTen(catalog::DecimalPrecisionOf(t.len));
            return v > -bound && v < bound;
        }
        default: return true;
    }
}

StatusOr<Int128> NumberOf(const parser::AstValue& v, const SetType& t) {
    switch (v.type) {
        case ValueType::kInt:
            if (t.type_val == kTypeValUint64) {
                auto u = ValueAsUint64(v);
                if (!u.ok()) return u.status();
                return static_cast<Int128>(u.value());
            }
            return static_cast<Int128>(v.int_val);
        case ValueType::kDecimal: return static_cast<Int128>(v.int_val);
        case ValueType::kDecimalWide: return Int128FromHalves(v.dec_hi, v.int_val);
        default:
            return Status::Corruption("a value that is not a number reached arithmetic");
    }
}

parser::AstValue ValueOf(Int128 v, const SetType& t) {
    parser::AstValue out;
    if (IsDecimal(t.type_val)) {
        out.type = t.type_val == kTypeValDecimal ? ValueType::kDecimal : ValueType::kDecimalWide;
        out.scale = catalog::DecimalScaleOf(t.len);
        out.int_val = Int128Low(v);
        if (t.type_val == kTypeValDecimalWide) out.dec_hi = Int128High(v);
        return out;
    }
    out.type = ValueType::kInt;
    out.int_val = Int128Low(v);
    // The digits carry a uint64 above INT64_MAX, which `int_val` cannot
    // (`ValueAsUint64`'s rule).
    if (t.type_val == kTypeValUint64 && v > static_cast<Int128>(INT64_MAX)) {
        out.raw_int_text = std::to_string(static_cast<std::uint64_t>(v));
    }
    return out;
}

class Evaluator {
public:
    explicit Evaluator(const std::vector<parser::AstValue>& row) : row_(row) {}

    StatusOr<parser::AstValue> Eval(const TypedSetExpr& e) const {
        switch (e.kind) {
            case Expr::Kind::kLiteral: return e.literal;
            case Expr::Kind::kColumn:
                if (e.column_no >= row_.size()) {
                    return Status::Corruption("an expression names a column past the row");
                }
                return row_[e.column_no];
            case Expr::Kind::kUnary: return Unary(e);
            case Expr::Kind::kBinary: return Binary(e);
            case Expr::Kind::kCall: return Call(e);
        }
        return Status::Corruption("an expression node of an unknown kind");
    }

private:
    // A bool is `kInt` 0 or 1, as a bool column decodes; unknown is NULL.
    static parser::AstValue Bool(bool v) {
        parser::AstValue out;
        out.type = ValueType::kInt;
        out.int_val = v ? 1 : 0;
        return out;
    }
    static bool IsNull(const parser::AstValue& v) { return v.type == ValueType::kNull; }
    static bool IsTrue(const parser::AstValue& v) { return v.type == ValueType::kInt && v.int_val != 0; }

    StatusOr<parser::AstValue> Unary(const TypedSetExpr& e) const {
        auto operand = Eval(*e.lhs);
        if (!operand.ok()) return operand;
        switch (e.op) {
            case ExprOp::kIsNull: return Bool(IsNull(operand.value()));
            case ExprOp::kIsNotNull: return Bool(!IsNull(operand.value()));
            case ExprOp::kNot:
                if (IsNull(operand.value())) return operand;
                return Bool(!IsTrue(operand.value()));
            case ExprOp::kCast: return Cast(e, operand.value());
            default: break;
        }
        if (IsNull(operand.value())) return operand;
        if (e.op == ExprOp::kPos) return operand;
        auto n = NumberOf(operand.value(), e.lhs->type);
        if (!n.ok()) return n.status();
        return Checked(-n.value(), e);
    }

    // -1, 0, 1 of two non-NULL values of one type.
    static StatusOr<int> Compare(const parser::AstValue& a, const parser::AstValue& b,
                                 const SetType& t) {
        if (IsText(t.type_val)) {
            const int c = a.str_val.compare(b.str_val);
            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
        auto x = NumberOf(a, t);
        if (!x.ok()) return x.status();
        auto y = NumberOf(b, t);
        if (!y.ok()) return y.status();
        return x.value() < y.value() ? -1 : (x.value() > y.value() ? 1 : 0);
    }

    StatusOr<parser::AstValue> Call(const TypedSetExpr& e) const {
        switch (e.op) {
            case ExprOp::kCase: {
                const std::size_t pairs = e.args.size() / 2;
                for (std::size_t i = 0; i < pairs; ++i) {
                    auto when = Eval(*e.args[2 * i]);
                    if (!when.ok()) return when;
                    if (IsTrue(when.value())) return Eval(*e.args[2 * i + 1]);
                }
                if (e.args.size() % 2 == 1) return Eval(*e.args.back());
                return parser::AstValue{};
            }
            case ExprOp::kCoalesce:
                for (const auto& a : e.args) {
                    auto v = Eval(*a);
                    if (!v.ok() || !IsNull(v.value())) return v;
                }
                return parser::AstValue{};
            case ExprOp::kNullif: {
                auto a = Eval(*e.args[0]);
                if (!a.ok() || IsNull(a.value())) return a;
                auto b = Eval(*e.args[1]);
                if (!b.ok()) return b;
                if (IsNull(b.value())) return a;
                auto c = Compare(a.value(), b.value(), e.args[0]->type);
                if (!c.ok()) return c.status();
                return c.value() == 0 ? parser::AstValue{} : a.value();
            }
            case ExprOp::kGreatest:
            case ExprOp::kLeast: {
                // NULLs are skipped, as the reference dialect does; the
                // result is NULL only when every argument is.
                parser::AstValue best;
                for (const auto& a : e.args) {
                    auto v = Eval(*a);
                    if (!v.ok()) return v;
                    if (IsNull(v.value())) continue;
                    if (IsNull(best)) {
                        best = std::move(v.value());
                        continue;
                    }
                    auto c = Compare(v.value(), best, e.type);
                    if (!c.ok()) return c.status();
                    if (e.op == ExprOp::kGreatest ? c.value() > 0 : c.value() < 0) {
                        best = std::move(v.value());
                    }
                }
                return best;
            }
            default: return Status::Corruption("a call the evaluator does not know");
        }
    }

    // CAST, source `e.lhs->type` to target `e.type` (BJ-S5, BJ-Q3). A
    // narrowing of scale rounds half away from zero and a result outside the
    // target is `OutOfRange`; nothing is truncated or wrapped.
    StatusOr<parser::AstValue> Cast(const TypedSetExpr& e, const parser::AstValue& v) const {
        if (IsNull(v)) return v;
        const SetType& from = e.lhs->type;
        const SetType& to = e.type;
        const Family source = FamilyOf(from.type_val);
        const Family target = FamilyOf(to.type_val);

        // An unscaled value and its scale, for the numeric sources.
        const auto AsDecimal = [&]() -> StatusOr<std::pair<Int128, int>> {
            if (source == Family::kText) return ParseNumberText(v.str_val, e);
            auto n = NumberOf(v, from);
            if (!n.ok()) return n.status();
            return std::pair<Int128, int>{
                n.value(), source == Family::kDec ? static_cast<int>(catalog::DecimalScaleOf(from.len)) : 0};
        };

        switch (target) {
            case Family::kInt:
            case Family::kDec: {
                auto d = AsDecimal();
                if (!d.ok()) return d.status();
                const int to_scale =
                    target == Family::kDec ? static_cast<int>(catalog::DecimalScaleOf(to.len)) : 0;
                auto rescaled = Rescale(d.value().first, d.value().second, to_scale, e);
                if (!rescaled.ok()) return rescaled.status();
                return Checked(rescaled.value(), e);
            }
            case Family::kText: {
                std::string text;
                if (source == Family::kText) {
                    text = v.str_val;
                } else {
                    text = FormatValue(from.type_val, v);
                }
                if (to.type_val == kTypeValChar && text.size() > to.len) {
                    return Status::OutOfRange("the value of " + std::to_string(text.size()) +
                                              " byte(s) is too long for " + SetTypeName(to) +
                                              At(e.op_byte_offset));
                }
                parser::AstValue out;
                out.type = ValueType::kStr;
                out.str_val = std::move(text);
                return out;
            }
            case Family::kDate:
            case Family::kTimestamp: {
                parser::AstValue out;
                if (source == Family::kText) {
                    catalog::SysColumnRow col{};
                    col.type_val = to.type_val;
                    out = v;
                    out.type = ValueType::kStr;
                    if (Status s = CoerceLiteralToColumn(col, out); !s.ok()) {
                        return s.WithMessage(s.message() + At(e.op_byte_offset));
                    }
                    return out;
                }
                out.type = ValueType::kInt;
                if (source == target) {
                    out.int_val = v.int_val;
                } else if (target == Family::kTimestamp) {  // date -> timestamp
                    out.int_val = v.int_val * 86400LL * 1000000LL;
                } else {  // timestamp -> date: the day it falls in, floored
                    constexpr std::int64_t kDay = 86400LL * 1000000LL;
                    std::int64_t days = v.int_val / kDay;
                    if (v.int_val % kDay < 0) --days;
                    out.int_val = days;
                }
                return out;
            }
            case Family::kBool: return v;
            case Family::kOther: break;
        }
        return Status::Corruption("a CAST between families the typer admitted but cannot run");
    }

    // `unscaled / 10^from` at `to` decimal places, rounding half away from
    // zero when places are dropped.
    static StatusOr<Int128> Rescale(Int128 unscaled, int from, int to, const TypedSetExpr& e) {
        if (to >= from) {
            Int128 out = unscaled;
            for (int i = from; i < to; ++i) {
                if (__builtin_mul_overflow(out, static_cast<Int128>(10), &out)) {
                    return OutOfRangeResult(e);
                }
            }
            return out;
        }
        Int128 divisor = 1;
        for (int i = to; i < from; ++i) {
            if (__builtin_mul_overflow(divisor, static_cast<Int128>(10), &divisor)) {
                return static_cast<Int128>(0);  // the whole value is a fraction of 10^38+
            }
        }
        Int128 q = unscaled / divisor;
        const Int128 r = unscaled % divisor;
        const Int128 twice = (r < 0 ? -r : r) * 2;  // |r| < 10^38, so this fits
        if (twice >= divisor) q += unscaled < 0 ? -1 : 1;
        return q;
    }

    // `[+-]digits[.digits]` with surrounding spaces, as an unscaled value and
    // its number of places; anything else is refused.
    static StatusOr<std::pair<Int128, int>> ParseNumberText(const std::string& text,
                                                           const TypedSetExpr& e) {
        std::size_t i = 0;
        std::size_t end = text.size();
        while (i < end && text[i] == ' ') ++i;
        while (end > i && text[end - 1] == ' ') --end;
        bool negative = false;
        if (i < end && (text[i] == '+' || text[i] == '-')) negative = text[i++] == '-';
        Int128 value = 0;
        int places = 0;
        bool digits = false;
        bool point = false;
        for (; i < end; ++i) {
            const char c = text[i];
            if (c == '.' && !point) {
                point = true;
            } else if (c >= '0' && c <= '9') {
                digits = true;
                if (__builtin_mul_overflow(value, static_cast<Int128>(10), &value) ||
                    __builtin_add_overflow(value, static_cast<Int128>(c - '0'), &value)) {
                    return OutOfRangeResult(e);
                }
                if (point) ++places;
            } else {
                digits = false;
                break;
            }
        }
        if (!digits) {
            return Status::InvalidArgument("the string '" + text + "' is not a number" +
                                           At(e.op_byte_offset));
        }
        return std::pair<Int128, int>{negative ? -value : value, places};
    }

    // Three-valued AND and OR, left to right, the right operand skipped once
    // the left decides the answer - so `b <> 0 AND a / b > 1` is safe.
    StatusOr<parser::AstValue> Logic(const TypedSetExpr& e) const {
        const bool is_and = e.op == ExprOp::kAnd;
        auto l = Eval(*e.lhs);
        if (!l.ok()) return l;
        if (!IsNull(l.value()) && IsTrue(l.value()) != is_and) return Bool(!is_and);
        auto r = Eval(*e.rhs);
        if (!r.ok()) return r;
        if (!IsNull(r.value()) && IsTrue(r.value()) != is_and) return Bool(!is_and);
        if (IsNull(l.value()) || IsNull(r.value())) return parser::AstValue{};
        return Bool(is_and);
    }

    StatusOr<parser::AstValue> Binary(const TypedSetExpr& e) const {
        if (e.op == ExprOp::kAnd || e.op == ExprOp::kOr) return Logic(e);
        auto l = Eval(*e.lhs);
        if (!l.ok()) return l;
        auto r = Eval(*e.rhs);
        if (!r.ok()) return r;
        // A NULL operand yields NULL, before a division by zero is judged
        // (BJ-R4): `NULL / 0` is NULL.
        if (l.value().type == ValueType::kNull || r.value().type == ValueType::kNull) {
            return parser::AstValue{};
        }
        if (IsComparison(e.op)) {
            auto c = Compare(l.value(), r.value(), e.lhs->type);
            if (!c.ok()) return c.status();
            switch (e.op) {
                case ExprOp::kEq: return Bool(c.value() == 0);
                case ExprOp::kNe: return Bool(c.value() != 0);
                case ExprOp::kLt: return Bool(c.value() < 0);
                case ExprOp::kLe: return Bool(c.value() <= 0);
                case ExprOp::kGt: return Bool(c.value() > 0);
                default: return Bool(c.value() >= 0);
            }
        }
        if (e.op == ExprOp::kConcat) {
            parser::AstValue out;
            out.type = ValueType::kStr;
            out.str_val = l.value().str_val + r.value().str_val;
            return out;
        }
        auto a = NumberOf(l.value(), e.lhs->type);
        if (!a.ok()) return a.status();
        auto b = NumberOf(r.value(), e.rhs->type);
        if (!b.ok()) return b.status();

        Int128 result = 0;
        bool overflow = false;
        switch (e.op) {
            case ExprOp::kAdd: overflow = __builtin_add_overflow(a.value(), b.value(), &result); break;
            case ExprOp::kSub: overflow = __builtin_sub_overflow(a.value(), b.value(), &result); break;
            case ExprOp::kMul: overflow = __builtin_mul_overflow(a.value(), b.value(), &result); break;
            case ExprOp::kDiv:
            case ExprOp::kMod:
                if (b.value() == 0) {
                    return Status::InvalidArgument("division by zero" + At(e.op_byte_offset));
                }
                result = e.op == ExprOp::kDiv ? a.value() / b.value() : a.value() % b.value();
                break;
            default: return Status::Corruption("an operator the evaluator does not know");
        }
        if (overflow) return OutOfRangeResult(e);
        return Checked(result, e);
    }

    static Status OutOfRangeResult(const TypedSetExpr& e) {
        return Status::OutOfRange("the result of operator " + std::string(OpText(e.op)) +
                                  " is outside " + SetTypeName(e.type) + At(e.op_byte_offset));
    }

    static StatusOr<parser::AstValue> Checked(Int128 v, const TypedSetExpr& e) {
        if (!InRange(v, e.type)) return OutOfRangeResult(e);
        return ValueOf(v, e.type);
    }

    const std::vector<parser::AstValue>& row_;
};

}  // namespace

bool IsConstantSetExpression(const TypedSetExpr& expr) {
    switch (expr.kind) {
        case Expr::Kind::kLiteral: return true;
        case Expr::Kind::kColumn: return false;
        case Expr::Kind::kUnary: return IsConstantSetExpression(*expr.lhs);
        case Expr::Kind::kBinary:
            return IsConstantSetExpression(*expr.lhs) && IsConstantSetExpression(*expr.rhs);
        case Expr::Kind::kCall:
            for (const auto& a : expr.args) {
                if (!IsConstantSetExpression(*a)) return false;
            }
            return true;
    }
    return false;
}

StatusOr<parser::AstValue> EvaluateSetExpression(const TypedSetExpr& expr,
                                                 const std::vector<parser::AstValue>& row) {
    return Evaluator(row).Eval(expr);
}

std::string SetTypeName(const SetType& t) {
    switch (t.kind) {
        case SetType::Kind::kNull: return "NULL";
        case SetType::Kind::kInteger: return "integer literal";
        case SetType::Kind::kNumeric: return "numeric literal";
        case SetType::Kind::kString: return "string literal";
        case SetType::Kind::kConcrete: break;
    }
    switch (t.type_val) {
        case kTypeValInt8: return "int8";
        case kTypeValInt16: return "int16";
        case kTypeValInt32: return "int32";
        case kTypeValInt64: return "int64";
        case kTypeValUint64: return "uint64";
        case kTypeValBool: return "bool";
        case kTypeValVarchar: return "varchar";
        case kTypeValChar: return "char(" + std::to_string(t.len) + ")";
        case kTypeValDate: return "date";
        case kTypeValTimestamp: return "timestamp";
        case kTypeValDecimal:
        case kTypeValDecimalWide:
            // Both spell `decimal(p,s)`: the wide type is not a name a
            // statement can write.
            return "decimal(" + std::to_string(catalog::DecimalPrecisionOf(t.len)) + "," +
                   std::to_string(catalog::DecimalScaleOf(t.len)) + ")";
        default: return "type " + std::to_string(t.type_val);
    }
}

StatusOr<std::unique_ptr<TypedSetExpr>> TypeSetExpression(const catalog::TableAccess& access,
                                                          const parser::Expr& expr,
                                                          const catalog::SysColumnRow& target) {
    const SetType want = Concrete(target.type_val, target.len);
    Typer typer(access);
    auto typed = typer.Type(expr);
    if (!typed.ok()) return typed;
    if (Status st = typer.Resolve(*typed.value(), want); !st.ok()) return st;

    const SetType& got = typed.value()->type;
    const bool assignable = got.concrete() && (SameType(got, want) || (IsText(got.type_val) &&
                                                                       IsText(want.type_val)));
    if (!assignable) {
        return Status::InvalidArgument(
            "the expression is " + SetTypeName(got) + " but column '" +
            std::string(catalog::NameView(target.name)) + "' is " + SetTypeName(want) +
            At(expr.byte_offset) + "; there is no implicit conversion (BJ-R3), CAST is the only one");
    }
    return typed;
}

}  // namespace kds::exec
