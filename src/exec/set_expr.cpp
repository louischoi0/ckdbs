#include "kds/exec/set_expr.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

#include "kds/catalog/rows.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/row_codec.hpp"

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
    }
    return "?";
}

class Typer {
public:
    Typer(const catalog::TableAccess& access) : access_(access) {}

    using Node = std::unique_ptr<TypedSetExpr>;

    StatusOr<Node> Type(const Expr& e, const SetType* hint) {
        switch (e.kind) {
            case Expr::Kind::kLiteral: return Literal(e, hint);
            case Expr::Kind::kColumn: return Column(e);
            case Expr::Kind::kUnary: return Unary(e, hint);
            case Expr::Kind::kBinary: return Binary(e, hint);
        }
        return Status::Corruption("an expression node of an unknown kind");
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

    // A literal takes the type its context gives it, through the routines a
    // bare literal goes through at the encode (types.md §3.1).
    StatusOr<Node> Literal(const Expr& e, const SetType* hint) {
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
        if (hint == nullptr || !hint->concrete()) return n;
        if (Status s = CoerceInto(*n, *hint); !s.ok()) return s;
        return n;
    }

    // Gives an untyped literal node `want`, or says why the literal cannot be
    // that type.
    Status CoerceInto(TypedSetExpr& n, const SetType& want) const {
        const SetType::Kind from = n.type.kind;
        if (from == SetType::Kind::kConcrete) return Status::OK();

        catalog::SysColumnRow col{};
        col.type_val = want.type_val;
        col.len = want.len;
        const auto Mismatch = [&] {
            return Status::InvalidArgument(SetTypeName(n.type) + " cannot be used as " +
                                           SetTypeName(want) + At(n.byte_offset) +
                                           "; there is no implicit conversion");
        };
        const auto Positioned = [&](const Status& s) {
            return s.WithMessage(s.message() + At(n.byte_offset));
        };

        switch (from) {
            case SetType::Kind::kNull:
                break;  // NULL is every type's value; the row decides NOT NULL (BJ-R4)
            case SetType::Kind::kInteger:
                if (IsInteger(want.type_val)) {
                    if (Status s = CheckIntegerLiteralFits(col, n.literal); !s.ok()) return Positioned(s);
                } else if (IsDecimal(want.type_val)) {
                    if (Status s = CoerceLiteralToColumn(col, n.literal); !s.ok()) {
                        return Positioned(s);
                    }
                } else {
                    return Mismatch();
                }
                break;
            case SetType::Kind::kNumeric:
                if (!IsDecimal(want.type_val)) return Mismatch();
                if (Status s = CoerceLiteralToColumn(col, n.literal); !s.ok()) {
                    return Positioned(s);
                }
                break;
            case SetType::Kind::kString:
                if (!IsText(want.type_val)) return Mismatch();
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

    StatusOr<Node> Unary(const Expr& e, const SetType* hint) {
        auto child = Type(*e.lhs, hint);
        if (!child.ok()) return child;
        Node n = Make(e);
        n->type = child.value()->type;
        if (n->type.concrete()) {
            const std::uint32_t t = n->type.type_val;
            const bool ok = e.op == ExprOp::kPos ? (IsInteger(t) || IsDecimal(t))
                                                 : (IsSignedInt(t) || IsDecimal(t));
            if (!ok) {
                return Refuse(e, "operator " + std::string(OpText(e.op)) + " cannot apply to " +
                                     SetTypeName(n->type) +
                                     (t == kTypeValUint64 ? " (an unsigned value has no negation)"
                                                          : ""));
            }
        } else if (n->type.kind == SetType::Kind::kString) {
            return Refuse(e, "operator " + std::string(OpText(e.op)) + " cannot apply to " +
                                 SetTypeName(n->type));
        }
        n->lhs = std::move(child.value());
        return n;
    }

    StatusOr<Node> Binary(const Expr& e, const SetType* hint) {
        auto l = Type(*e.lhs, nullptr);
        if (!l.ok()) return l;
        auto r = Type(*e.rhs, nullptr);
        if (!r.ok()) return r;
        Node lhs = std::move(l.value());
        Node rhs = std::move(r.value());

        // Time types refuse before any literal is typed against one: an
        // integer would otherwise coerce into a DATE as an epoch day.
        for (const SetType& t : {lhs->type, rhs->type}) {
            if (t.concrete() && IsTime(t.type_val)) {
                if (t.type_val == kTypeValDate) {
                    return Status::NotImplemented("arithmetic on a date is not supported yet (BJ-S6)" +
                                                  At(e.op_byte_offset));
                }
                return Status::Unsupported(
                    "arithmetic on a timestamp is not supported: there is no INTERVAL type" +
                    At(e.op_byte_offset));
            }
        }

        const bool mul_div = e.op == ExprOp::kMul || e.op == ExprOp::kDiv;
        const auto DecimalScalar = [&](const Node& concrete, const Node& other) {
            return mul_div && concrete->type.concrete() && IsDecimal(concrete->type.type_val) &&
                   other->type.kind == SetType::Kind::kInteger;
        };

        // Give the untyped side the concrete side's type.
        if (lhs->type.concrete() && !rhs->type.concrete()) {
            // `price * 2`: an integer literal scaling a decimal is scale 0
            // (BJ-R3), so it stays an integer and the product keeps the
            // decimal's scale.
            const SetType want =
                DecimalScalar(lhs, rhs) ? Concrete(kTypeValInt64, 0) : lhs->type;
            auto again = Type(*e.rhs, &want);
            if (!again.ok()) return again;
            rhs = std::move(again.value());
        } else if (rhs->type.concrete() && !lhs->type.concrete()) {
            const SetType want =
                DecimalScalar(rhs, lhs) ? Concrete(kTypeValInt64, 0) : rhs->type;
            auto again = Type(*e.lhs, &want);
            if (!again.ok()) return again;
            lhs = std::move(again.value());
        } else if (!lhs->type.concrete() && !rhs->type.concrete()) {
            if (hint != nullptr && hint->concrete() &&
                !(mul_div && IsDecimal(hint->type_val))) {
                auto la = Type(*e.lhs, hint);
                if (!la.ok()) return la;
                auto ra = Type(*e.rhs, hint);
                if (!ra.ok()) return ra;
                lhs = std::move(la.value());
                rhs = std::move(ra.value());
            } else if (hint != nullptr && hint->concrete()) {
                // A decimal target over `2 * 3`: the literals are integers
                // and so is the product; the assignment then refuses it.
                const SetType ints = Concrete(kTypeValInt64, 0);
                auto la = Type(*e.lhs, &ints);
                if (!la.ok()) return la;
                auto ra = Type(*e.rhs, &ints);
                if (!ra.ok()) return ra;
                lhs = std::move(la.value());
                rhs = std::move(ra.value());
            }
        }

        // A literal typed as an integer next to a decimal is the scale-0
        // scalar of `price * 2`, never a column that happens to be int64.
        const auto IsScalar = [](const Node& n) {
            return n->kind == Expr::Kind::kLiteral && n->type.concrete() &&
                   n->type.type_val == kTypeValInt64;
        };
        Node n = Make(e);
        auto result = ResultType(e, lhs->type, rhs->type, IsScalar(lhs), IsScalar(rhs));
        if (!result.ok()) return result.status();
        n->type = result.value();
        n->lhs = std::move(lhs);
        n->rhs = std::move(rhs);
        return n;
    }

    // The declared signatures of family 1 (BJ-R3).
    StatusOr<SetType> ResultType(const Expr& e, const SetType& a, const SetType& b, bool a_scalar,
                                 bool b_scalar) const {
        const auto Bad = [&]() {
            return Refuse(e, "operator " + std::string(OpText(e.op)) + " cannot apply to " +
                                 SetTypeName(a) + " and " + SetTypeName(b));
        };

        // Both still untyped: only literals remain, and a pure-literal
        // expression stays untyped until a context types it.
        if (!a.concrete() && !b.concrete()) {
            const auto K = SetType::Kind::kNull;
            if (e.op == ExprOp::kConcat) {
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
        if (!a.concrete() || !b.concrete()) {
            // One side concrete and the other untyped after re-typing means
            // the literal was NULL (a typed one is concrete by now).
            return a.concrete() ? a : b;
        }

        if (e.op == ExprOp::kConcat) {
            if (IsText(a.type_val) && IsText(b.type_val)) return Concrete(kTypeValVarchar, 0);
            return Bad();
        }

        if ((IsDecimal(a.type_val) && b_scalar) || (IsDecimal(b.type_val) && a_scalar)) {
            if (e.op == ExprOp::kMul) return IsDecimal(a.type_val) ? a : b;
            if (e.op == ExprOp::kDiv && IsDecimal(a.type_val)) return DivisionRefused(e);
            return Bad();
        }

        if (!SameType(a, b)) return Bad();
        const std::uint32_t t = a.type_val;
        if (IsInteger(t)) return a;
        if (IsDecimal(t)) {
            switch (e.op) {
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
                                                  std::to_string(s) + " beyond 38" +
                                                  At(e.op_byte_offset));
                    }
                    return Concrete(p <= 18 ? kTypeValDecimal : kTypeValDecimalWide,
                                    catalog::PackDecimalLen(static_cast<std::uint8_t>(p),
                                                            static_cast<std::uint8_t>(s)));
                }
                case ExprOp::kDiv: return DivisionRefused(e);
                default: return Bad();
            }
        }
        return Bad();
    }

    static Status DivisionRefused(const Expr& e) {
        return Status::NotImplemented(
            "decimal division needs an explicit narrowing (ROUND or CAST), which BJ-S5 and "
            "BJ-S6 build" +
            At(e.op_byte_offset));
    }

    static Status Refuse(const Expr& e, const std::string& what) {
        return Status::InvalidArgument(what + At(e.op_byte_offset) +
                                       "; there is no implicit conversion (BJ-R3)");
    }

    const catalog::TableAccess& access_;
};

}  // namespace

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
            return std::string(t.type_val == kTypeValDecimal ? "decimal(" : "decimal128(") +
                   std::to_string(catalog::DecimalPrecisionOf(t.len)) + "," +
                   std::to_string(catalog::DecimalScaleOf(t.len)) + ")";
        default: return "type " + std::to_string(t.type_val);
    }
}

StatusOr<std::unique_ptr<TypedSetExpr>> TypeSetExpression(const catalog::TableAccess& access,
                                                          const parser::Expr& expr,
                                                          const catalog::SysColumnRow& target) {
    const SetType want = Concrete(target.type_val, target.len);
    Typer typer(access);
    auto typed = typer.Type(expr, &want);
    if (!typed.ok()) return typed;

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
