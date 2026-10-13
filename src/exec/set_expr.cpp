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
bool IsDateType(const SetType& t) { return t.concrete() && t.type_val == kTypeValDate; }

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

// A value of `got` stands where `want` is expected: the same type, or both
// text (char and varchar compare and store as the text they hold).
bool Assignable(const SetType& got, const SetType& want) {
    return got.concrete() &&
           (SameType(got, want) || (IsText(got.type_val) && IsText(want.type_val)));
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
        case ExprOp::kFunction: return "function";
    }
    return "?";
}

std::string_view FunctionName(SetFunction f) {
    switch (f) {
        case SetFunction::kAbs: return "abs";
        case SetFunction::kSign: return "sign";
        case SetFunction::kCeil: return "ceil";
        case SetFunction::kFloor: return "floor";
        case SetFunction::kRound: return "round";
        case SetFunction::kTrunc: return "trunc";
        case SetFunction::kUpper: return "upper";
        case SetFunction::kLower: return "lower";
        case SetFunction::kLength: return "length";
        case SetFunction::kSubstring: return "substring";
        case SetFunction::kTrim: return "trim";
        case SetFunction::kLtrim: return "ltrim";
        case SetFunction::kRtrim: return "rtrim";
        case SetFunction::kPosition: return "position";
        case SetFunction::kReplace: return "replace";
        case SetFunction::kLpad: return "lpad";
        case SetFunction::kRpad: return "rpad";
        case SetFunction::kConcat: return "concat";
        case SetFunction::kNow: return "now";
        case SetFunction::kCurrentDate: return "current_date";
        case SetFunction::kDate: return "date";
        case SetFunction::kNone: break;
    }
    return "function";
}

// The one shape of a typing refusal: what is wrong, the byte, and that no
// conversion rescues it (BJ-R3).
Status Refuse(std::uint32_t at, const std::string& what) {
    return Status::InvalidArgument(what + At(at) + "; there is no implicit conversion (BJ-R3)");
}

Status CannotApply(ExprOp op, std::uint32_t at, const SetType& a, const SetType& b) {
    return Refuse(at, "operator " + std::string(OpText(op)) + " cannot apply to " +
                          SetTypeName(a) + " and " + SetTypeName(b));
}

bool IsComparison(ExprOp op) { return op >= ExprOp::kEq && op <= ExprOp::kGe; }
bool IsBooleanBinary(ExprOp op) {
    return IsComparison(op) || op == ExprOp::kAnd || op == ExprOp::kOr;
}

SetType BoolType() { return Concrete(kTypeValBool, 0); }

// `decimal(precision, scale)`: the precision alone selects the width (TY2).
SetType DecimalType(std::uint32_t precision, std::uint32_t scale) {
    return Concrete(precision >= kMinDecimalPrecisionWide ? kTypeValDecimalWide : kTypeValDecimal,
                    catalog::PackDecimalLen(static_cast<std::uint8_t>(precision),
                                            static_cast<std::uint8_t>(scale)));
}

// A decimal of `scale` places whose precision the context gives, naturally
// `precision`.
SetType Scaled(std::uint32_t precision, std::uint32_t scale) {
    precision = std::max<std::uint32_t>(std::min<std::uint32_t>(precision, 38), std::max(scale, 1u));
    SetType t;
    t.kind = SetType::Kind::kScaled;
    t.type_val = precision >= kMinDecimalPrecisionWide ? kTypeValDecimalWide : kTypeValDecimal;
    t.len = catalog::PackDecimalLen(static_cast<std::uint8_t>(precision),
                                    static_cast<std::uint8_t>(scale));
    return t;
}

// The type of `a / b` for decimals (BJ-Q3 (a)): the dividend's scale plus up to
// 16 places, as many as leave the quotient room for the integer digits it can
// need (`p1 - s1 + s2`), and never fewer places than the dividend has. Never
// narrowed implicitly - `ROUND` or `CAST` is how the client narrows it.
SetType DecimalQuotient(const SetType& dividend, std::uint32_t divisor_scale) {
    const int p1 = catalog::DecimalPrecisionOf(dividend.len);
    const int s1 = catalog::DecimalScaleOf(dividend.len);
    const int integer_digits = p1 - s1 + static_cast<int>(divisor_scale);
    const int scale = std::max(s1, std::min(s1 + 16, 38 - integer_digits));
    const int precision = std::min(integer_digits + scale, 38);
    return DecimalType(static_cast<std::uint32_t>(precision), static_cast<std::uint32_t>(scale));
}

// What CAST converts between (BJ-S5, BJ-Q3). The classes are the reference
// dialect's families, restricted to the engine's types; a pair not listed is
// refused at compile, and `bool` converts to nothing but itself.
using Family = TypeFamily;

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
        return DecimalType(spec.precision, spec.scale);
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
    return DecimalType(digits, fraction);
}

class Typer {
public:
    Typer(const catalog::TableAccess& access, const SubqueryResolver* subqueries)
        : access_(access), subqueries_(subqueries) {}

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
            case Expr::Kind::kSubquery: return Subquery(e);
        }
        return Status::Corruption("an expression node of an unknown kind");
    }

    // Gives the untyped literals under `n` the type `want`, in place and in
    // one walk.
    Status Resolve(TypedSetExpr& n, const SetType& want) const {
        if (n.type.concrete()) return Status::OK();
        switch (n.kind) {
            case Expr::Kind::kLiteral: return CoerceInto(n, want);
            case Expr::Kind::kColumn:
            case Expr::Kind::kSubquery: return Status::OK();  // always concrete
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
                if (n.op == ExprOp::kFunction) {
                    if (n.type.kind == SetType::Kind::kNull) {
                        n.type = want;  // a numeric function of NULL: NULL of any type
                        return Status::OK();
                    }
                    // Otherwise a scaled decimal, which takes the context's
                    // precision when the scale is the one it yields.
                    if (!IsDecimal(want.type_val) ||
                        catalog::DecimalScaleOf(want.len) != catalog::DecimalScaleOf(n.type.len)) {
                        return Refuse(n.op_byte_offset,
                                      "the result has " + SetTypeName(n.type) + " but is used as " +
                                          SetTypeName(want));
                    }
                    n.type = want;
                    return Status::OK();
                }
                // The value positions take the context's type; a CASE's
                // conditions are bool already.
                const std::size_t pairs = n.op == ExprOp::kCase ? n.args.size() / 2 : 0;
                for (std::size_t i = 0; i < n.args.size(); ++i) {
                    const bool condition = i < pairs * 2 && i % 2 == 0;
                    if (condition) continue;
                    if (Status s = ResolveExactly(*n.args[i], want); !s.ok()) return s;
                }
                n.type = want;
                return Status::OK();
            }
        }
        return Status::OK();
    }

    // Resolve, for a position that holds a value of `want` beside others of
    // it: an operation of literals settles to its own type (`2 * 3` is int64
    // under a decimal context), and a value of another type there would be
    // compared or chosen as unscaled numbers.
    Status ResolveExactly(TypedSetExpr& n, const SetType& want) const {
        if (Status s = Resolve(n, want); !s.ok()) return s;
        if (Assignable(n.type, want)) return Status::OK();
        return Refuse(n.byte_offset,
                      "the value is " + SetTypeName(n.type) + " but is used as " + SetTypeName(want));
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
            case SetType::Kind::kScaled:
            case SetType::Kind::kConcrete:
                break;
        }
        n.type = want;
        return Status::OK();
    }

    StatusOr<Node> Column(const Expr& e) {
        const catalog::SysColumnRow* col = access_.schema.FindColumn(e.column);
        if (col == nullptr && Lower(e.column) == "current_date") {
            // BJ-Q4: a column of the relation with this name wins, so the
            // function is reachable only where there is none.
            Node n = Make(e);
            n->kind = Expr::Kind::kCall;
            n->op = ExprOp::kFunction;
            n->function = SetFunction::kCurrentDate;
            n->type = Concrete(kTypeValDate, 0);
            return n;
        }
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
    static StatusOr<SetType> DefaultOf(const SetType& t, std::uint32_t at) {
        switch (t.kind) {
            case SetType::Kind::kInteger:
            case SetType::Kind::kNull: return Concrete(kTypeValInt64, 0);
            case SetType::Kind::kString: return Concrete(kTypeValVarchar, 0);
            case SetType::Kind::kScaled: return Concrete(t.type_val, t.len);
            default:
                return Refuse(at,
                              "a numeric literal here has no type; give it a column to take "
                              "one from, or CAST it");
        }
    }

    Status SettleByDefault(TypedSetExpr& n) const {
        if (n.type.concrete()) return Status::OK();
        auto type = DefaultOf(n.type, n.byte_offset);
        if (!type.ok()) return type.status();
        return Resolve(n, type.value());
    }

    // One common type for operands that must agree (a comparison, a CASE's
    // results, COALESCE, ...): the first concrete one's, every other concrete
    // one the same (the text family counts as one), and the untyped ones take
    // it. With none concrete the node stays untyped, of the kinds' merge,
    // until a context types it.
    //
    // `widen_scaled` is for a comparison, whose result is a bool: a scaled
    // decimal beside a decimal of its scale takes the wider of the two
    // precisions, so what is compared can never fail to fit.
    StatusOr<SetType> Unify(const std::vector<TypedSetExpr*>& values, std::uint32_t at,
                            const std::string& who, bool widen_scaled = false) const {
        const SetType* first = nullptr;
        for (const TypedSetExpr* v : values) {
            if (!v->type.concrete()) continue;
            if (first == nullptr) {
                first = &v->type;
            } else if (!Assignable(v->type, *first)) {
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
                if (widen_scaled && v->type.kind == SetType::Kind::kScaled && IsDecimal(t.type_val) &&
                    catalog::DecimalScaleOf(v->type.len) == catalog::DecimalScaleOf(t.len)) {
                    const std::uint32_t precision = std::max(catalog::DecimalPrecisionOf(v->type.len),
                                                             catalog::DecimalPrecisionOf(t.len));
                    const SetType wide = Scaled(precision, catalog::DecimalScaleOf(t.len));
                    if (Status s = ResolveExactly(*v, Concrete(wide.type_val, wide.len)); !s.ok()) return s;
                    continue;
                }
                if (Status s = ResolveExactly(*v, t); !s.ok()) return s;
            }
            return t;
        }
        SetType merged = Untyped(SetType::Kind::kNull);
        for (const TypedSetExpr* v : values) {
            const SetType& vt = v->type;
            if (vt.kind == SetType::Kind::kNull) continue;
            if (merged.kind == SetType::Kind::kNull) {
                merged = vt;
            } else if (merged.kind == SetType::Kind::kString || vt.kind == SetType::Kind::kString) {
                if (merged.kind != vt.kind) {
                    return Refuse(at, who + " needs one type but is given " +
                                          SetTypeName(merged) + " and " + SetTypeName(vt));
                }
            } else if (vt.kind == SetType::Kind::kScaled) {
                if (merged.kind == SetType::Kind::kScaled &&
                    catalog::DecimalScaleOf(merged.len) != catalog::DecimalScaleOf(vt.len)) {
                    return Refuse(at, who + " needs one type but is given " +
                                          SetTypeName(merged) + " and " + SetTypeName(vt));
                }
                // A literal under a scaled decimal takes its scale; two scaled
                // decimals of one scale keep the wider precision, so the order
                // they are written in does not decide it.
                if (merged.kind != SetType::Kind::kScaled ||
                    catalog::DecimalPrecisionOf(vt.len) > catalog::DecimalPrecisionOf(merged.len)) {
                    merged = vt;
                }
            } else if (merged.kind != SetType::Kind::kScaled &&
                       vt.kind == SetType::Kind::kNumeric) {
                merged.kind = SetType::Kind::kNumeric;
            }
        }
        return merged;
    }

    StatusOr<Node> BooleanBinary(const Expr& e, Node lhs, Node rhs) {
        Node n = Make(e);
        if (e.op == ExprOp::kAnd || e.op == ExprOp::kOr) {
            const std::string who = "operator " + std::string(OpText(e.op));
            if (Status s = BoolOperand(*lhs, who, e.op_byte_offset); !s.ok()) return s;
            if (Status s = BoolOperand(*rhs, who, e.op_byte_offset); !s.ok()) return s;
        } else {
            auto t = Unify({lhs.get(), rhs.get()}, e.op_byte_offset,
                           "operator " + std::string(OpText(e.op)), /*widen_scaled=*/true);
            if (!t.ok()) return t.status();
            // Two literals compare as what the two of them are by default; the
            // result is a bool whatever they are, so nothing later types them.
            if (!t.value().concrete()) {
                auto type = DefaultOf(t.value(), e.byte_offset);
                if (!type.ok()) return type.status();
                if (Status s = ResolveExactly(*lhs, type.value()); !s.ok()) return s;
                if (Status s = ResolveExactly(*rhs, type.value()); !s.ok()) return s;
            }
        }
        n->type = BoolType();
        n->lhs = std::move(lhs);
        n->rhs = std::move(rhs);
        return n;
    }

    StatusOr<Node> Function(const Expr& e) {
        struct Spec {
            std::string_view name;
            SetFunction fn;
            std::size_t min_args;
            std::size_t max_args;
        };
        static constexpr Spec kSpecs[] = {
            {"abs", SetFunction::kAbs, 1, 1},         {"sign", SetFunction::kSign, 1, 1},
            {"ceil", SetFunction::kCeil, 1, 1},       {"ceiling", SetFunction::kCeil, 1, 1},
            {"floor", SetFunction::kFloor, 1, 1},     {"round", SetFunction::kRound, 1, 2},
            {"trunc", SetFunction::kTrunc, 1, 2},     {"upper", SetFunction::kUpper, 1, 1},
            {"lower", SetFunction::kLower, 1, 1},     {"length", SetFunction::kLength, 1, 1},
            {"substring", SetFunction::kSubstring, 2, 3},
            {"trim", SetFunction::kTrim, 1, 2},       {"ltrim", SetFunction::kLtrim, 1, 2},
            {"rtrim", SetFunction::kRtrim, 1, 2},     {"position", SetFunction::kPosition, 2, 2},
            {"replace", SetFunction::kReplace, 3, 3}, {"lpad", SetFunction::kLpad, 2, 3},
            {"rpad", SetFunction::kRpad, 2, 3},       {"concat", SetFunction::kConcat, 1, 128},
            {"now", SetFunction::kNow, 0, 0},         {"date", SetFunction::kDate, 1, 1},
        };
        const std::string name = Lower(e.function);
        const Spec* spec = nullptr;
        for (const Spec& candidate : kSpecs) {
            if (candidate.name == name) spec = &candidate;
        }
        if (spec == nullptr) {
            return Status::InvalidArgument("unknown function '" + e.function + "'" +
                                           At(e.byte_offset));
        }
        if (e.args.size() < spec->min_args || e.args.size() > spec->max_args) {
            return Status::InvalidArgument(
                "'" + name + "' takes " +
                (spec->min_args == spec->max_args
                     ? std::to_string(spec->min_args)
                     : std::to_string(spec->min_args) + " to " + std::to_string(spec->max_args)) +
                " argument(s)" + At(e.byte_offset));
        }
        Node n = Make(e);
        n->function = spec->fn;
        for (const auto& arg : e.args) {
            auto a = Type(*arg);
            if (!a.ok()) return a;
            n->args.push_back(std::move(a.value()));
        }

        const auto Wrong = [&](std::size_t i, const char* wanted) {
            return Refuse(e.args[i]->byte_offset, name + " needs " + wanted + " but is given " +
                                                      SetTypeName(n->args[i]->type));
        };
        // An argument that must be text; a NULL or a string literal is varchar.
        const auto Text = [&](std::size_t i) -> Status {
            TypedSetExpr& a = *n->args[i];
            if (a.type.concrete()) return IsText(a.type.type_val) ? Status::OK() : Wrong(i, "text");
            if (a.type.kind == SetType::Kind::kString || a.type.kind == SetType::Kind::kNull) {
                return Resolve(a, Concrete(kTypeValVarchar, 0));
            }
            return Wrong(i, "text");
        };
        // An argument that must be a whole number (a position, a length).
        const auto Whole = [&](std::size_t i) -> Status {
            TypedSetExpr& a = *n->args[i];
            if (a.type.concrete()) {
                return IsInteger(a.type.type_val) ? Status::OK() : Wrong(i, "a whole number");
            }
            if (a.type.kind == SetType::Kind::kInteger || a.type.kind == SetType::Kind::kNull) {
                return Resolve(a, Concrete(kTypeValInt64, 0));
            }
            return Wrong(i, "a whole number");
        };
        // An argument that must be an integer or a decimal. A literal is int64;
        // a NULL stays NULL (the result is NULL of whatever the context wants);
        // a scaled decimal is passed on as it is, so `ABS(ROUND(x, 2))` still
        // takes its context's precision.
        const auto Number = [&](std::size_t i) -> Status {
            TypedSetExpr& a = *n->args[i];
            if (a.type.concrete()) {
                return IsInteger(a.type.type_val) || IsDecimal(a.type.type_val)
                           ? Status::OK()
                           : Wrong(i, "a number");
            }
            if (a.type.kind == SetType::Kind::kNull || a.type.kind == SetType::Kind::kScaled) {
                return Status::OK();
            }
            if (a.type.kind == SetType::Kind::kInteger) {
                return Resolve(a, Concrete(kTypeValInt64, 0));
            }
            if (a.type.kind == SetType::Kind::kNumeric) return SettleByDefault(a);
            return Wrong(i, "a number");
        };

        switch (spec->fn) {
            case SetFunction::kAbs:
            case SetFunction::kSign: {
                if (Status s = Number(0); !s.ok()) return s;
                const SetType& t = n->args[0]->type;
                // `SIGN` is 1, -1 or 0 at the argument's scale, which a
                // decimal whose precision is its scale cannot hold: the
                // result takes its context's precision instead.
                n->type = spec->fn == SetFunction::kSign && IsDecimal(t.type_val)
                              ? Scaled(std::max<std::uint32_t>(catalog::DecimalPrecisionOf(t.len),
                                                catalog::DecimalScaleOf(t.len) + 1u),
                                       catalog::DecimalScaleOf(t.len))
                              : t;
                break;
            }
            case SetFunction::kCeil:
            case SetFunction::kFloor:
            case SetFunction::kRound:
            case SetFunction::kTrunc: {
                if (Status s = Number(0); !s.ok()) return s;
                const bool has_places =
                    spec->fn == SetFunction::kRound || spec->fn == SetFunction::kTrunc;
                if (has_places && n->args.size() == 2) {
                    // The places are part of the type, so they are a literal.
                    const Expr& places = *e.args[1];
                    if (places.kind != Expr::Kind::kLiteral ||
                        places.literal.type != ValueType::kInt) {
                        return Refuse(places.byte_offset,
                                      name + " takes its number of places as a whole-number "
                                             "literal: it is part of the result's type");
                    }
                    if (places.literal.int_val < 0 && !parser::IntLiteralWrapped(places.literal)) {
                        return Status::NotImplemented(
                            name + " takes a count of places from 0; a negative count is not "
                                   "supported" +
                            At(places.byte_offset));
                    }
                    if (parser::IntLiteralWrapped(places.literal) || places.literal.int_val > 38) {
                        return Status::InvalidArgument(name + " takes places from 0 to 38" +
                                                       At(places.byte_offset));
                    }
                    n->places = static_cast<std::uint32_t>(places.literal.int_val);
                    // Read; the tree needs no node for it, and no row should
                    // evaluate one.
                    n->args.resize(1);
                }
                const SetType& t = n->args[0]->type;
                if (t.kind == SetType::Kind::kNull || !IsDecimal(t.type_val)) {
                    n->type = t;  // a NULL, or an integer, which has no places to drop
                } else {
                    // Always scaled: the result is a decimal of `places`
                    // places and no precision of its own (BJ-Q3 (a)).
                    const std::uint32_t p = catalog::DecimalPrecisionOf(t.len);
                    const std::uint32_t s = catalog::DecimalScaleOf(t.len);
                    n->type = Scaled(n->places > s ? p - s + n->places : p, n->places);
                }
                break;
            }
            case SetFunction::kUpper:
            case SetFunction::kLower:
                if (Status s = Text(0); !s.ok()) return s;
                n->type = Concrete(kTypeValVarchar, 0);
                break;
            case SetFunction::kLength:
                if (Status s = Text(0); !s.ok()) return s;
                n->type = Concrete(kTypeValInt32, 0);
                break;
            case SetFunction::kPosition:
                for (std::size_t i = 0; i < 2; ++i) {
                    if (Status s = Text(i); !s.ok()) return s;
                }
                n->type = Concrete(kTypeValInt32, 0);
                break;
            case SetFunction::kSubstring:
                if (Status s = Text(0); !s.ok()) return s;
                for (std::size_t i = 1; i < n->args.size(); ++i) {
                    if (Status s = Whole(i); !s.ok()) return s;
                }
                n->type = Concrete(kTypeValVarchar, 0);
                break;
            case SetFunction::kLpad:
            case SetFunction::kRpad:
                if (Status s = Text(0); !s.ok()) return s;
                if (Status s = Whole(1); !s.ok()) return s;
                if (n->args.size() == 3) {
                    if (Status s = Text(2); !s.ok()) return s;
                }
                n->type = Concrete(kTypeValVarchar, 0);
                break;
            case SetFunction::kTrim:
            case SetFunction::kLtrim:
            case SetFunction::kRtrim:
            case SetFunction::kReplace:
            case SetFunction::kConcat:
                for (std::size_t i = 0; i < n->args.size(); ++i) {
                    if (Status s = Text(i); !s.ok()) return s;
                }
                n->type = Concrete(kTypeValVarchar, 0);
                break;
            case SetFunction::kNow: n->type = Concrete(kTypeValTimestamp, 0); break;
            case SetFunction::kDate: {
                TypedSetExpr& a = *n->args[0];
                if (!a.type.concrete()) {
                    if (Status s = Resolve(a, Concrete(kTypeValTimestamp, 0)); !s.ok()) return s;
                }
                if (a.type.type_val != kTypeValTimestamp && a.type.type_val != kTypeValDate) {
                    return Wrong(0, "a timestamp");
                }
                n->type = Concrete(kTypeValDate, 0);
                break;
            }
            case SetFunction::kCurrentDate:
            case SetFunction::kNone: break;
        }
        return n;
    }

    StatusOr<Node> Call(const Expr& e) {
        if (e.op == ExprOp::kFunction) return Function(e);
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
        auto t = Unify(values, e.op_byte_offset, std::string(OpText(e.op)));
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
                case SetType::Kind::kScaled: s = SettleByDefault(*child); break;
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
        if (!CastAdmitted(TypeFamilyOf(child->type.type_val), TypeFamilyOf(target.value().type_val))) {
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

        // A timestamp has no arithmetic: there is no INTERVAL type to give a
        // difference (BJ-Q4). A date has the two forms below.
        for (const SetType& t : {lhs->type, rhs->type}) {
            if (t.concrete() && t.type_val == kTypeValTimestamp) {
                return Status::Unsupported(
                    "arithmetic on a timestamp is not supported: there is no INTERVAL type" +
                    At(e.op_byte_offset));
            }
        }
        if (IsDateType(lhs->type) || IsDateType(rhs->type)) {
            return DateArithmetic(e, std::move(lhs), std::move(rhs));
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

    // `date + days`, `days + date`, `date - days` (a date) and `date - date`
    // (days, int32) - BJ-Q4 (a); every other operator over a date is refused.
    StatusOr<Node> DateArithmetic(const Expr& e, Node lhs, Node rhs) {
        const auto IsDate = [](const TypedSetExpr& n) { return IsDateType(n.type); };
        const auto Bad = [&]() { return CannotApply(e.op, e.op_byte_offset, lhs->type, rhs->type); };
        // The other operand is a number of days: any integer type, or a literal.
        const auto Days = [&](TypedSetExpr& n) -> Status {
            if (n.type.concrete()) return IsInteger(n.type.type_val) ? Status::OK() : Bad();
            if (n.type.kind == SetType::Kind::kInteger || n.type.kind == SetType::Kind::kNull) {
                return Resolve(n, Concrete(kTypeValInt64, 0));
            }
            return Bad();
        };
        Node n = Make(e);
        if (e.op == ExprOp::kAdd && IsDate(*lhs) && !IsDate(*rhs)) {
            if (Status s = Days(*rhs); !s.ok()) return s;
            n->type = Concrete(kTypeValDate, 0);
        } else if (e.op == ExprOp::kAdd && IsDate(*rhs) && !IsDate(*lhs)) {
            if (Status s = Days(*lhs); !s.ok()) return s;
            n->type = Concrete(kTypeValDate, 0);
        } else if (e.op == ExprOp::kSub && IsDate(*lhs) && IsDate(*rhs)) {
            n->type = Concrete(kTypeValInt32, 0);
        } else if (e.op == ExprOp::kSub && IsDate(*lhs)) {
            if (Status s = Days(*rhs); !s.ok()) return s;
            n->type = Concrete(kTypeValDate, 0);
        } else {
            return Bad();
        }
        n->lhs = std::move(lhs);
        n->rhs = std::move(rhs);
        return n;
    }

    // The kind of a node whose operands are both untyped.
    static StatusOr<SetType> UntypedResult(ExprOp op, std::uint32_t at, const SetType& a,
                                           const SetType& b) {
        constexpr auto K = SetType::Kind::kNull;
        const auto Bad = [&]() { return CannotApply(op, at, a, b); };
        if (op == ExprOp::kConcat) {
            const bool ok = (a.kind == SetType::Kind::kString || a.kind == K) &&
                            (b.kind == SetType::Kind::kString || b.kind == K);
            return ok ? StatusOr<SetType>(Untyped(SetType::Kind::kString))
                      : StatusOr<SetType>(Bad());
        }
        if (a.kind == SetType::Kind::kString || b.kind == SetType::Kind::kString) return Bad();
        if (a.kind == SetType::Kind::kScaled || b.kind == SetType::Kind::kScaled) {
            // Only a sum, a difference or a remainder keeps a scale; a
            // product or a quotient changes it, and two scales must agree.
            if (op != ExprOp::kAdd && op != ExprOp::kSub && op != ExprOp::kMod) {
                return Refuse(at, "operator " + std::string(OpText(op)) + " over " +
                                      SetTypeName(a.kind == SetType::Kind::kScaled ? a : b) +
                                      " has no type of its own; CAST it first");
            }
            if (a.kind == SetType::Kind::kScaled && b.kind == SetType::Kind::kScaled &&
                catalog::DecimalScaleOf(a.len) != catalog::DecimalScaleOf(b.len)) {
                return Bad();
            }
            return a.kind == SetType::Kind::kScaled ? a : b;
        }
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
    // A dividend is never a scalar: the quotient's type is the dividend's,
    // so `2 / price` takes the divisor's type as `2.00 / price` does.
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
            const bool dividend = op == ExprOp::kDiv && &u == &l;
            if (mul_div && !dividend && IsDecimal(target.type_val)) {
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
        const auto Bad = [&]() { return CannotApply(op, at, a, b); };
        if (op == ExprOp::kConcat) {
            if (IsText(a.type_val) && IsText(b.type_val)) return Concrete(kTypeValVarchar, 0);
            return Bad();
        }

        if ((IsDecimal(a.type_val) && b_scalar) || (IsDecimal(b.type_val) && a_scalar)) {
            if (op == ExprOp::kMul) return IsDecimal(a.type_val) && b_scalar ? a : b;
            if (op == ExprOp::kDiv && IsDecimal(a.type_val)) return DecimalQuotient(a, 0);
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
                    return DecimalType(p, s);
                }
                case ExprOp::kDiv: return DecimalQuotient(a, catalog::DecimalScaleOf(b.len));
                default: return Bad();
            }
        }
        return Bad();
    }

    // A `(SELECT ...)` value: run once by the dispatcher, and from here on a
    // typed constant - a literal of the type its one column has.
    StatusOr<Node> Subquery(const Expr& e) {
        if (subqueries_ == nullptr || !*subqueries_) {
            return Status::NotImplemented("a subquery in a SET value needs a statement to run it" +
                                          At(e.byte_offset));
        }
        auto answer = (*subqueries_)(e);
        if (!answer.ok()) return answer.status();
        Node n = Make(e);
        n->kind = Expr::Kind::kLiteral;
        n->literal = std::move(answer.value().value);
        n->type = answer.value().type;
        return n;
    }

    const catalog::TableAccess& access_;
    const SubqueryResolver* subqueries_;
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
    Evaluator(const std::vector<parser::AstValue>& row, const StatementContext& context)
        : row_(row), context_(context) {}

    StatusOr<parser::AstValue> Eval(const TypedSetExpr& e) const {
        switch (e.kind) {
            case Expr::Kind::kLiteral: return e.literal;
            case Expr::Kind::kColumn:
                if (e.column_no >= row_.size()) {
                    return Status::Corruption("an expression names a column past the row");
                }
                // A slot the partial decode left out (BN-R2 of
                // `workorder-bn-opt-read-update-path.md`): the mask that
                // chose the columns missed one this expression reads.
                if (row_[e.column_no].type == parser::ValueType::kParam) {
                    return Status::Corruption("an expression reads column " +
                                              std::to_string(e.column_no) +
                                              ", which the row's decode left out");
                }
                return row_[e.column_no];
            case Expr::Kind::kUnary: return Unary(e);
            case Expr::Kind::kBinary: return Binary(e);
            case Expr::Kind::kCall: return Call(e);
            case Expr::Kind::kSubquery:
                return Status::Corruption("a subquery reached evaluation untyped");
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
        // `CAST(a / b AS decimal(p, s))` divides to `s` places once.
        if (e.op == ExprOp::kCast && IsDecimalQuotient(*e.lhs) &&
            (IsDecimal(e.type.type_val) || IsInteger(e.type.type_val))) {
            return FusedQuotient(*e.lhs, ScaleOf(e.type), Rounding::kHalfAwayFromZero, e);
        }
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
        if (e.op == ExprOp::kFunction) return Function(e);
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
        const Family source = TypeFamilyOf(from.type_val);
        const Family target = TypeFamilyOf(to.type_val);

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
                    out.int_val = FloorEpochDay(v.int_val);
                }
                return out;
            }
            case Family::kBool: return v;
            case Family::kOther: break;
        }
        return Status::Corruption("a CAST between families the typer admitted but cannot run");
    }

    enum class Rounding { kHalfAwayFromZero, kTowardZero, kFloor, kCeiling };

    // `unscaled / 10^from` at `to` decimal places, rounding as asked when
    // places are dropped (CAST and ROUND: half away from zero, BJ-Q3 (a)).
    static StatusOr<Int128> Rescale(Int128 unscaled, int from, int to, const TypedSetExpr& e,
                                    Rounding mode = Rounding::kHalfAwayFromZero) {
        if (to >= from) {
            Int128 out = unscaled;
            for (int i = from; i < to; ++i) {
                if (__builtin_mul_overflow(out, static_cast<Int128>(10), &out)) {
                    return OutOfRangeResult(e);
                }
            }
            return out;
        }
        // Dropping places is a division by 10^(from - to), at most 10^38,
        // which fits 128 bits: it cannot fail.
        return DivideAt(unscaled, 1, from, 0, to, mode, e);
    }

    static Rounding ModeOf(SetFunction f) {
        switch (f) {
            case SetFunction::kCeil: return Rounding::kCeiling;
            case SetFunction::kFloor: return Rounding::kFloor;
            case SetFunction::kTrunc: return Rounding::kTowardZero;
            default: return Rounding::kHalfAwayFromZero;
        }
    }

    // `a / b` of two decimals, divided **once** to `to_scale` places and rounded
    // as asked - so `ROUND(x / y, 2)` rounds the true quotient once, never a
    // quotient that was already rounded to more places. `sa` and `sb` are the
    // operands' scales. The numerator (or, for fewer places than it has, the
    // denominator) is scaled up exactly, which is why operands near 38 digits
    // are refused rather than divided inexactly.
    static StatusOr<Int128> DivideAt(Int128 a, Int128 b, int sa, int sb, int to_scale,
                                     Rounding mode, const TypedSetExpr& division) {
        if (b == 0) {
            return Status::InvalidArgument("division by zero" + At(division.op_byte_offset));
        }
        Int128 numerator = a;
        Int128 denominator = b;
        const int k = to_scale - sa + sb;
        for (int i = 0; i < (k < 0 ? -k : k); ++i) {
            Int128& side = k > 0 ? numerator : denominator;
            if (__builtin_mul_overflow(side, static_cast<Int128>(10), &side)) {
                return Status::OutOfRange("the operands are too wide to divide to " +
                                          std::to_string(to_scale) + " places" +
                                          At(division.op_byte_offset));
            }
        }
        Int128 q = numerator / denominator;
        const Int128 r = numerator % denominator;
        if (r != 0) {
            const bool negative = (numerator < 0) != (denominator < 0);
            switch (mode) {
                case Rounding::kHalfAwayFromZero: {
                    const Int128 magnitude = r < 0 ? -r : r;
                    const Int128 divisor = denominator < 0 ? -denominator : denominator;
                    if (magnitude >= divisor - magnitude) q += negative ? -1 : 1;
                    break;
                }
                case Rounding::kTowardZero: break;
                case Rounding::kFloor:
                    if (negative) --q;
                    break;
                case Rounding::kCeiling:
                    if (!negative) ++q;
                    break;
            }
        }
        return q;
    }

    static int ScaleOf(const SetType& t) {
        return IsDecimal(t.type_val) ? static_cast<int>(catalog::DecimalScaleOf(t.len)) : 0;
    }

    // A decimal `/` node evaluated straight to `to_scale` places, for the
    // rounding functions and CAST that take it as their argument.
    StatusOr<parser::AstValue> FusedQuotient(const TypedSetExpr& division, int to_scale,
                                             Rounding mode, const TypedSetExpr& e) const {
        auto l = Eval(*division.lhs);
        if (!l.ok()) return l;
        auto r = Eval(*division.rhs);
        if (!r.ok()) return r;
        if (IsNull(l.value()) || IsNull(r.value())) return parser::AstValue{};
        auto a = NumberOf(l.value(), division.lhs->type);
        if (!a.ok()) return a.status();
        auto b = NumberOf(r.value(), division.rhs->type);
        if (!b.ok()) return b.status();
        auto q = DivideAt(a.value(), b.value(), ScaleOf(division.lhs->type),
                          ScaleOf(division.rhs->type), to_scale, mode, division);
        if (!q.ok()) return q.status();
        return Checked(q.value(), e);
    }

    static bool IsDecimalQuotient(const TypedSetExpr& n) {
        return n.kind == Expr::Kind::kBinary && n.op == ExprOp::kDiv && IsDecimal(n.type.type_val);
    }

    static StatusOr<parser::AstValue> DecimalQuotient(const TypedSetExpr& e, Int128 a, Int128 b) {
        auto q = DivideAt(a, b, ScaleOf(e.lhs->type), ScaleOf(e.rhs->type), ScaleOf(e.type),
                          Rounding::kHalfAwayFromZero, e);
        if (!q.ok()) return q.status();
        return Checked(q.value(), e);
    }

    // `date +/- days` and `date - date` (BJ-Q4 (a)); `a` and `b` are the
    // operands' day counts.
    static StatusOr<parser::AstValue> DateOp(const TypedSetExpr& e, Int128 a, Int128 b) {
        const Int128 out = e.op == ExprOp::kAdd ? a + b : a - b;
        if (e.type.type_val != kTypeValDate) return Checked(out, e);
        if (out < kMinEpochDay || out > kMaxEpochDay) {
            return Status::OutOfRange("the date is outside the supported range" +
                                      At(e.op_byte_offset));
        }
        parser::AstValue value;
        value.type = ValueType::kInt;
        value.int_val = static_cast<std::int64_t>(out);
        return value;
    }

    // The most bytes a string operation builds, so `LPAD(s, 2000000000)` or a
    // chain of `||` cannot exhaust memory before the encode refuses a value
    // that large (no stored value exceeds `varheap.hpp`'s kMaxValueSize).
    static constexpr std::size_t kMaxStringBytes = 1u << 20;

    static Status BuildsTooMuch(std::string_view what, std::uint32_t at) {
        return Status::OutOfRange(std::string(what) + " builds more than " +
                                  std::to_string(kMaxStringBytes) + " bytes" + At(at));
    }

    static parser::AstValue Text(std::string text) {
        parser::AstValue out;
        out.type = ValueType::kStr;
        out.str_val = std::move(text);
        return out;
    }

    StatusOr<parser::AstValue> Function(const TypedSetExpr& e) const {
        parser::AstValue instant;
        instant.type = ValueType::kInt;
        if (e.function == SetFunction::kNow) {
            instant.int_val = context_.now_us;
            return instant;
        }
        if (e.function == SetFunction::kCurrentDate) {
            instant.int_val = FloorEpochDay(context_.now_us);
            return instant;
        }

        const bool rounding = e.function == SetFunction::kCeil || e.function == SetFunction::kFloor ||
                              e.function == SetFunction::kRound || e.function == SetFunction::kTrunc;
        if (rounding && IsDecimalQuotient(*e.args[0])) {
            return FusedQuotient(*e.args[0], ScaleOf(e.type), ModeOf(e.function), e);
        }

        std::vector<parser::AstValue> v;
        v.reserve(e.args.size());
        for (const auto& a : e.args) {
            auto value = Eval(*a);
            if (!value.ok()) return value;
            v.push_back(std::move(value.value()));
        }
        if (e.function == SetFunction::kConcat) {
            std::size_t total = 0;
            for (const auto& a : v) total += IsNull(a) ? 0 : a.str_val.size();
            if (total > kMaxStringBytes) return BuildsTooMuch("concat", e.op_byte_offset);
            std::string joined;
            joined.reserve(total);
            for (const auto& a : v) {
                if (!IsNull(a)) joined += a.str_val;
            }
            return Text(std::move(joined));
        }
        // Every other function is NULL for a NULL argument.
        for (const auto& a : v) {
            if (IsNull(a)) return parser::AstValue{};
        }
        const auto Whole = [&](std::size_t i) { return NumberOf(v[i], e.args[i]->type); };

        switch (e.function) {
            case SetFunction::kAbs: {
                auto n = Whole(0);
                if (!n.ok()) return n.status();
                return Checked(n.value() < 0 ? -n.value() : n.value(), e);
            }
            case SetFunction::kSign: {
                auto n = Whole(0);
                if (!n.ok()) return n.status();
                const int sign = n.value() < 0 ? -1 : (n.value() > 0 ? 1 : 0);
                return Checked(IsDecimal(e.type.type_val)
                                   ? sign * PowerOfTen(catalog::DecimalScaleOf(e.type.len))
                                   : static_cast<Int128>(sign),
                               e);
            }
            case SetFunction::kCeil:
            case SetFunction::kFloor:
            case SetFunction::kRound:
            case SetFunction::kTrunc: {
                auto n = Whole(0);
                if (!n.ok()) return n.status();
                if (!IsDecimal(e.args[0]->type.type_val)) return Checked(n.value(), e);
                auto rescaled = Rescale(n.value(), ScaleOf(e.args[0]->type), ScaleOf(e.type), e,
                                        ModeOf(e.function));
                if (!rescaled.ok()) return rescaled.status();
                return Checked(rescaled.value(), e);
            }
            case SetFunction::kUpper:
            case SetFunction::kLower: {
                std::string out = v[0].str_val;
                for (char& c : out) {
                    c = static_cast<char>(e.function == SetFunction::kUpper
                                              ? std::toupper(static_cast<unsigned char>(c))
                                              : std::tolower(static_cast<unsigned char>(c)));
                }
                return Text(std::move(out));
            }
            case SetFunction::kLength: return Checked(static_cast<Int128>(v[0].str_val.size()), e);
            case SetFunction::kPosition: {
                const std::size_t at = v[1].str_val.find(v[0].str_val);
                return Checked(at == std::string::npos ? 0 : static_cast<Int128>(at) + 1, e);
            }
            case SetFunction::kSubstring: {
                const std::string& text = v[0].str_val;
                auto from = Whole(1);
                if (!from.ok()) return from.status();
                const Int128 end_of_text = static_cast<Int128>(text.size()) + 1;
                Int128 stop = end_of_text;
                if (v.size() == 3) {
                    auto count = Whole(2);
                    if (!count.ok()) return count.status();
                    if (count.value() < 0) {
                        return Status::InvalidArgument("negative substring length not allowed" +
                                                       At(e.op_byte_offset));
                    }
                    stop = from.value() + count.value();
                }
                const Int128 lo = from.value() < 1 ? 1 : from.value();
                const Int128 hi = stop < end_of_text ? stop : end_of_text;
                if (hi <= lo) return Text("");
                return Text(text.substr(static_cast<std::size_t>(lo - 1),
                                        static_cast<std::size_t>(hi - lo)));
            }
            case SetFunction::kTrim:
            case SetFunction::kLtrim:
            case SetFunction::kRtrim: {
                const std::string& text = v[0].str_val;
                const std::string chars = v.size() > 1 ? v[1].str_val : std::string(" ");
                std::size_t first = 0;
                std::size_t last = text.size();
                if (e.function != SetFunction::kRtrim) {
                    while (first < last && chars.find(text[first]) != std::string::npos) ++first;
                }
                if (e.function != SetFunction::kLtrim) {
                    while (last > first && chars.find(text[last - 1]) != std::string::npos) --last;
                }
                return Text(text.substr(first, last - first));
            }
            case SetFunction::kReplace: {
                const std::string& text = v[0].str_val;
                const std::string& from = v[1].str_val;
                if (from.empty()) return Text(text);
                std::string out;
                std::size_t at = 0;
                for (;;) {
                    const std::size_t hit = text.find(from, at);
                    if (hit == std::string::npos) break;
                    out.append(text, at, hit - at);
                    out += v[2].str_val;
                    at = hit + from.size();
                    if (out.size() > kMaxStringBytes) {
                        return BuildsTooMuch("replace", e.op_byte_offset);
                    }
                }
                out.append(text, at, std::string::npos);
                if (out.size() > kMaxStringBytes) return BuildsTooMuch("replace", e.op_byte_offset);
                return Text(std::move(out));
            }
            case SetFunction::kLpad:
            case SetFunction::kRpad: {
                const std::string& text = v[0].str_val;
                auto length = Whole(1);
                if (!length.ok()) return length.status();
                if (length.value() <= 0) return Text("");
                if (length.value() > static_cast<Int128>(kMaxStringBytes)) {
                    return BuildsTooMuch(FunctionName(e.function), e.op_byte_offset);
                }
                const std::size_t want = static_cast<std::size_t>(length.value());
                if (text.size() >= want) return Text(text.substr(0, want));
                const std::string fill = v.size() > 2 ? v[2].str_val : std::string(" ");
                if (fill.empty()) return Text(text);
                std::string pad;
                for (std::size_t i = 0; pad.size() < want - text.size(); ++i) {
                    pad.push_back(fill[i % fill.size()]);
                }
                return Text(e.function == SetFunction::kLpad ? pad + text : text + pad);
            }
            case SetFunction::kDate: {
                parser::AstValue out;
                out.type = ValueType::kInt;
                out.int_val = e.args[0]->type.type_val == kTypeValTimestamp ? FloorEpochDay(v[0].int_val)
                                                                           : v[0].int_val;
                return out;
            }
            default: return Status::Corruption("a function the evaluator does not know");
        }
    }

    // A decimal string as an unscaled value and its number of places, through
    // the parser every decimal literal goes through (TY01): the text's own
    // fraction digits are its scale, so nothing is rounded here - `Rescale`
    // rounds.
    static StatusOr<std::pair<Int128, int>> ParseNumberText(const std::string& text,
                                                           const TypedSetExpr& e) {
        const std::size_t point = text.find('.');
        const std::size_t places = point == std::string::npos ? 0 : text.size() - point - 1;
        const auto NotANumber = [&](const Status& why) {
            return why.WithMessage("the string '" + text + "' is not a number" +
                                   At(e.op_byte_offset));
        };
        if (places > kMaxDecimalPrecisionWide) {
            return NotANumber(Status::OutOfRange("more than 38 decimal places"));
        }
        auto value = ParseDecimalLiteralWide(text, kMaxDecimalPrecisionWide,
                                             static_cast<std::uint8_t>(places));
        if (!value.ok()) return NotANumber(value.status());
        return std::pair<Int128, int>{value.value(), static_cast<int>(places)};
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
            if (l.value().str_val.size() + r.value().str_val.size() > kMaxStringBytes) {
                return BuildsTooMuch("||", e.op_byte_offset);
            }
            return Text(l.value().str_val + r.value().str_val);
        }
        auto a = NumberOf(l.value(), e.lhs->type);
        if (!a.ok()) return a.status();
        auto b = NumberOf(r.value(), e.rhs->type);
        if (!b.ok()) return b.status();
        if (e.lhs->type.type_val == kTypeValDate || e.rhs->type.type_val == kTypeValDate) {
            return DateOp(e, a.value(), b.value());
        }
        if (e.op == ExprOp::kDiv && IsDecimal(e.type.type_val)) {
            return DecimalQuotient(e, a.value(), b.value());
        }

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
        const std::string what = e.op == ExprOp::kFunction
                                     ? "function " + std::string(FunctionName(e.function))
                                     : "operator " + std::string(OpText(e.op));
        return Status::OutOfRange("the result of " + what + " is outside " + SetTypeName(e.type) +
                                  At(e.op_byte_offset));
    }

    static StatusOr<parser::AstValue> Checked(Int128 v, const TypedSetExpr& e) {
        if (!InRange(v, e.type)) return OutOfRangeResult(e);
        return ValueOf(v, e.type);
    }

    const std::vector<parser::AstValue>& row_;
    const StatementContext& context_;
};

}  // namespace

bool IsConstantSetExpression(const TypedSetExpr& expr) {
    switch (expr.kind) {
        case Expr::Kind::kLiteral: return true;
        case Expr::Kind::kColumn:
        case Expr::Kind::kSubquery: return false;
        case Expr::Kind::kUnary: return IsConstantSetExpression(*expr.lhs);
        case Expr::Kind::kBinary:
            return IsConstantSetExpression(*expr.lhs) && IsConstantSetExpression(*expr.rhs);
        case Expr::Kind::kCall:
            // `NOW()` and `CURRENT_DATE` are one value per statement, not
            // constants a compile can fold (BJ-Q10).
            if (expr.function == SetFunction::kNow || expr.function == SetFunction::kCurrentDate) {
                return false;
            }
            for (const auto& a : expr.args) {
                if (!IsConstantSetExpression(*a)) return false;
            }
            return true;
    }
    return false;
}

std::uint64_t SetExpressionColumns(const TypedSetExpr& expr) {
    switch (expr.kind) {
        case Expr::Kind::kLiteral:
        case Expr::Kind::kSubquery: return 0;
        case Expr::Kind::kColumn:
            return expr.column_no < 64 ? std::uint64_t{1} << expr.column_no : kAllColumnsMask;
        case Expr::Kind::kUnary: return SetExpressionColumns(*expr.lhs);
        case Expr::Kind::kBinary:
            return SetExpressionColumns(*expr.lhs) | SetExpressionColumns(*expr.rhs);
        case Expr::Kind::kCall: {
            std::uint64_t mask = 0;
            for (const auto& a : expr.args) mask |= SetExpressionColumns(*a);
            return mask;
        }
    }
    return kAllColumnsMask;
}

StatusOr<parser::AstValue> EvaluateSetExpression(const TypedSetExpr& expr,
                                                 const std::vector<parser::AstValue>& row,
                                                 const StatementContext& context) {
    return Evaluator(row, context).Eval(expr);
}

std::string SetTypeName(const SetType& t) {
    switch (t.kind) {
        case SetType::Kind::kNull: return "NULL";
        case SetType::Kind::kInteger: return "integer literal";
        case SetType::Kind::kNumeric: return "numeric literal";
        case SetType::Kind::kString: return "string literal";
        case SetType::Kind::kScaled:
            return "a decimal of scale " + std::to_string(catalog::DecimalScaleOf(t.len));
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
                                                          const catalog::SysColumnRow& target,
                                                          const SubqueryResolver* subqueries) {
    const SetType want = Concrete(target.type_val, target.len);
    Typer typer(access, subqueries);
    auto typed = typer.Type(expr);
    if (!typed.ok()) return typed;
    if (Status st = typer.Resolve(*typed.value(), want); !st.ok()) return st;

    const SetType& got = typed.value()->type;
    if (!Assignable(got, want)) {
        return Status::InvalidArgument(
            "the expression is " + SetTypeName(got) + " but column '" +
            std::string(catalog::NameView(target.name)) + "' is " + SetTypeName(want) +
            At(expr.byte_offset) + "; there is no implicit conversion (BJ-R3), CAST is the only one");
    }
    return typed;
}

}  // namespace kds::exec
