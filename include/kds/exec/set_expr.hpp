#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/functions.hpp"
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
// BJ-S4's evaluator walks (`EvaluateSetExpression`), once per qualifying row.

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
        // A decimal of a fixed scale whose precision its context gives: what
        // `ROUND(x, 2)` yields, so `SET price = ROUND(price / 3, 2)` is a
        // `decimal(10, 2)` whatever precision the quotient had (BJ-Q3 (a)).
        // `type_val` and `len` carry the natural type a context-free use takes.
        kScaled,
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

// The scalar functions a SET value may call (BJ-Q5, BJ-Q4). All are pure
// functions of their arguments except `NOW()` and `CURRENT_DATE`, which are
// stable - one value per statement (`StatementContext`). **No purity is
// declared in `functions.hpp`'s catalog**: that catalog's entries are what a
// WHERE conjunct compiles, and its own rule is that a field nothing reads is a
// promise nothing checks; an UPDATE has no determinism class until AQ.
enum class SetFunction : std::uint8_t {
    kNone, kAbs, kSign, kCeil, kFloor, kRound, kTrunc, kUpper, kLower, kLength, kSubstring,
    kTrim, kLtrim, kRtrim, kPosition, kReplace, kLpad, kRpad, kConcat, kNow, kCurrentDate, kDate,
};

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
    // kCall with kFunction: which one, and ROUND/TRUNC's number of places.
    SetFunction function = SetFunction::kNone;
    std::uint32_t places = 0;

    std::unique_ptr<TypedSetExpr> lhs;  // kUnary's operand, kBinary's left
    std::unique_ptr<TypedSetExpr> rhs;
    // kCall: `kCase`'s `[when, then, ..., else?]`, or the n-ary forms' operands.
    std::vector<std::unique_ptr<TypedSetExpr>> args;
};

// Types `expr` for assignment to `target`. The result is concrete and
// assignable to `target`, or the status says why not.
StatusOr<std::unique_ptr<TypedSetExpr>> TypeSetExpression(const catalog::TableAccess& access,
                                                          const parser::Expr& expr,
                                                          const catalog::SysColumnRow& target);

// Whether the expression reads no column - a constant, which BJ-Q10 folds
// at compile so a foreign-key column keeps its hoisted parent.
bool IsConstantSetExpression(const TypedSetExpr& expr);

// Evaluates a typed expression against `row`, the decoded before-image of the
// row being updated (BJ-R2: every assignment reads the row as the walk read
// it, never an earlier assignment's result). The value is in the form
// `EncodeOneValue` takes for the expression's type: `kInt`, `kDecimal`,
// `kDecimalWide`, `kStr` or `kNull`.
//
// Arithmetic is checked (BJ-R3): a result outside its type's range is
// `OutOfRange`, never a wrap, and a division by zero is `InvalidArgument`;
// each names the failing operator's byte. A NULL operand yields NULL (BJ-R4).
// Whether a NULL fits a `NOT NULL` target is the row's verdict, not this
// function's. Pure: no page is read.
StatusOr<parser::AstValue> EvaluateSetExpression(const TypedSetExpr& expr,
                                                 const std::vector<parser::AstValue>& row,
                                                 const StatementContext& context);

}  // namespace kds::exec
