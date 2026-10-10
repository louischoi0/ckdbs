#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/parser/ast.hpp"

// **Typing a SET value expression at compile** (BJ-S3,
// `instructions/v3.0.0/workorder-bj-expression-update.md` BJ-R3, BJ-R4,
// BJ-R5; the rules as a table in `docs/spec/types.md` §3.2).
//
// `TypeSetExpression` resolves a parsed `parser::Expr` against the target
// relation's schema into a `TypedSetExpr`: every column reference bound to
// its position, every literal coerced to the type its context gives it by
// the routines `EncodeOneValue` and `CoerceLiteralToColumn` already own, and
// every node carrying the type it yields. A tree that does not type is
// refused here, at compile, naming the types and the byte - whether or not
// any row matches (§1.3's point: today a mistyped literal is refused only at
// a row's encode).
//
// **No implicit conversion** (W3, BJ-Q2 (b)): an operator's operands are one
// exact type. The only conversions are the literal's - an untyped literal
// takes the other operand's type, or the target's - and the explicit `CAST`
// BJ-S5 builds. `varchar` and `char` are one family for `||` and for an
// assignment (a `char`'s length is a per-row check, as it is for a bare
// literal), and an integer literal scaling a decimal is scale 0 (BJ-R3).
//
// Pure: no page is read, no state is kept. The tree it returns is what
// BJ-S4's evaluator walks; until then `CompileAssignments` types an
// assignment's expression for the refusals alone and still refuses it
// `NotImplemented` (BJ-S2's rule).

namespace kds::exec {

// What a sub-expression yields. A literal has no type until its context
// gives one; everything else is `kConcrete`, meaning a concrete
// `(type_val, len)` exactly as a `sys.columns` row spells it.
struct SetType {
    enum class Kind : std::uint8_t {
        kNull,      // the NULL literal, before a context types it
        kInteger,   // an integer literal
        kNumeric,   // a bare number with a point, `1.5`
        kString,    // a quoted string
        kConcrete,  // a column's type: `type_val`, and `len` for char and decimal
    };
    Kind kind = Kind::kNull;
    std::uint32_t type_val = 0;
    std::uint32_t len = 0;

    bool concrete() const noexcept { return kind == Kind::kConcrete; }
};

// `int64`, `decimal(10,2)`, `char(8)`, `integer literal`, ... - what a
// refusal names.
std::string SetTypeName(const SetType& t);

struct TypedSetExpr {
    parser::Expr::Kind kind = parser::Expr::Kind::kLiteral;
    parser::ExprOp op = parser::ExprOp::kAdd;
    SetType type;

    std::uint32_t byte_offset = 0;
    std::uint32_t op_byte_offset = 0;

    // kLiteral: the value coerced to `type` when the context typed it.
    parser::AstValue literal;
    // kColumn: the position in the relation's schema (0 is the pk, which an
    // expression may read, BJ-R5).
    std::size_t column_no = 0;

    std::unique_ptr<TypedSetExpr> lhs;  // kUnary's operand, kBinary's left
    std::unique_ptr<TypedSetExpr> rhs;
};

// Types `expr` for assignment to `target`. The result is concrete and
// assignable to `target`, or the status says why not.
StatusOr<std::unique_ptr<TypedSetExpr>> TypeSetExpression(const catalog::TableAccess& access,
                                                          const parser::Expr& expr,
                                                          const catalog::SysColumnRow& target);

}  // namespace kds::exec
