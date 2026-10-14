#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/exec/functions.hpp"
#include "kds/parser/ast.hpp"

// An expression or predicate Cabin's key function `F`, compiled
// (instructions/v3.0.0/workorder-aq-expression-cabins.md AQ-R1, R2, R4, R6;
// docs/spec/cabin.md §3).
//
// AR1-4 makes a Cabin a key function `F: tuple -> K`. The **column** shape is
// `F = t.x` and keeps its own path (`TableAccess::cabin_mask`); this file is
// the other two:
//
//   - **expression**: `F = f(x, ...)`, an immutable function of columns. The
//     key is `F`'s value; a NULL value observes nothing.
//   - **predicate**: `F = (c1 AND c2 ...)`, a conjunction of comparisons of
//     a column or an immutable call against a literal. The key is the one
//     value `true` (`PredicateKey()`); a row for which `p` does not hold
//     has no key.
//
// ---- The canonical form is the identity (AQ-R2, `[quiet-wrong]`) ---------
//
// Every `F` has one canonical text, built from its **lowered** form: column
// **positions** (AQ-Q2), the function's catalog name, the operator, and the
// literal **after coercion** to the type it is compared as, every field of
// it. A predicate's conjuncts are sorted and de-duplicated, so `a AND b` and
// `b AND a` are one `F`. Two compiled `F`s are the same Cabin key function
// exactly when their texts are equal, and that comparison - never the hash -
// is what a probe, a witness and `CREATE CABIN`'s duplicate check confirm.
//
// `expr_id` is AR1 §6's fingerprint: FNV-1a over the same text with every
// literal folded to its kind, so it is shared by `F`s that differ only in a
// literal, and by any two that collide. It finds candidates; it decides
// nothing.
//
// The text is also what `sys.cabin_exprs` stores, and `DecodeCabinExpr`
// recompiles it at load - so the stored form and the compared form are one
// string by construction. It is a storage form, not SQL: `$3` is column 3,
// and `RenderCabinExpr` gives the operator-facing spelling.

namespace kds::exec {

// One side a Cabin's `F` reads: a column (`fn == nullptr`, one argument) or
// an immutable call over columns.
struct CabinTerm {
    const FunctionEntry* fn = nullptr;
    std::vector<std::uint16_t> args;
};

// One conjunct of a predicate Cabin: `term op literal`, or `term IS [NOT]
// NULL` with no literal. `type_val` is the type both sides compare as.
struct CabinConjunct {
    CabinTerm term;
    parser::CompareOp op = parser::CompareOp::kEq;
    parser::AstValue literal;
    std::uint32_t type_val = 0;
};

struct CompiledCabinExpr {
    std::uint8_t shape = catalog::kCabinShapeUnset;

    // kCabinShapeExpression: the call.
    CabinTerm expr;

    // kCabinShapePredicate: the conjuncts, in canonical order, unique.
    std::vector<CabinConjunct> conjuncts;

    // Every column `F` reads, ascending and unique - the witness's "an input
    // column of F changed" (AR1 §10).
    std::vector<std::uint16_t> inputs;

    std::string text;
    std::uint64_t expr_id = 0;
};

// The one key a predicate Cabin's set is filed under.
parser::AstValue PredicateKey();

// The canonical text of one term or conjunct - what `F`'s text is made of,
// and what the serve path builds from a statement's lowered conjuncts to
// match against a Cabin's (AQ-R5, R6).
std::string CabinTermText(const CabinTerm& term);
std::string CabinConjunctText(const CabinConjunct& conjunct);

// `F` from `CREATE CABIN ON t (...)`'s parse, against the relation's
// schema. Refuses at the offending byte: an unknown or dropped column, a
// function that is not `kImmutable` (AQ-R4), an argument of the wrong type,
// an `F` reading no column or only the tuple key, an input column declared
// `NO CABIN`, a
// literal that does not coerce, and every predicate form AQ-R6 does not
// admit.
StatusOr<CompiledCabinExpr> CompileCabinExpr(const parser::CabinStmt& stmt,
                                             const catalog::Schema& schema);

// `F` again from its stored text, against the relation's current schema.
// `Corruption` for text this build did not write, or that names a column
// the relation no longer has.
StatusOr<CompiledCabinExpr> DecodeCabinExpr(std::uint8_t shape, std::string_view text,
                                             const catalog::Schema& schema);

// `F` spelled for an operator, with the columns' current names:
// `DATE(ts)`, `state = 'closed' AND amount > 1000`.
std::string RenderCabinExpr(const CompiledCabinExpr& f, const catalog::Schema& schema);

// The key `F` gives `row`, or nullopt when it gives none - a NULL
// expression value, or a predicate that does not hold. `row` is indexed by
// schema position and must hold every input's value, coerced to its
// column's type; nothing outside `inputs` is read.
std::optional<parser::AstValue> CabinKeyOf(const CompiledCabinExpr& f,
                                           std::span<const parser::AstValue> row);

}  // namespace kds::exec
