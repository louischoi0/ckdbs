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

// The one shape of a typing refusal: what is wrong, the byte, and that no
// conversion rescues it (BJ-R3).
Status Refuse(std::uint32_t at, const std::string& what) {
    return Status::InvalidArgument(what + At(at) + "; there is no implicit conversion (BJ-R3)");
}

Status DivisionRefused(std::uint32_t at) {
    return Status::NotImplemented(
        "decimal division needs an explicit narrowing (ROUND or CAST), which BJ-S5 and "
        "BJ-S6 build" +
        At(at));
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
        n->type = child.value()->type;
        if (Status s = CheckUnary(e.op, n->type, e.op_byte_offset); !s.ok()) return s;
        n->lhs = std::move(child.value());
        return n;
    }

    StatusOr<Node> Binary(const Expr& e) {
        auto l = Type(*e.lhs);
        if (!l.ok()) return l;
        auto r = Type(*e.rhs);
        if (!r.ok()) return r;
        Node lhs = std::move(l.value());
        Node rhs = std::move(r.value());

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
