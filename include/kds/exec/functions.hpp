#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "kds/parser/ast.hpp"

// The function catalog (instructions/v3.0.0/ar1-architecture-revision-cabin-
// function.md §2 and §3; workorder-ap-function-catalog-fetch-id.md AP-R4).
//
// Every scalar function the grammar admits is an entry here, and no function
// exists without one. An entry declares its **purity**, and the purity of the
// functions a statement's predicates call folds to the statement's
// **determinism class** - which decides whether the statement may be recorded
// as a Waystone trail at all.
//
// ---- What is built, and what is not -------------------------------------
//
// Two entries ship (AP-Q1): `DATE(timestamp) -> date`, `kImmutable`, and
// `NOW() -> timestamp`, `kStable`. Both are evaluated where a WHERE conjunct
// is: as a **function conjunct** of their own residual kind
// (step_chain.hpp's `FunctionPredicate`), which no key, bound, index, join,
// build or Cabin reader walks. A function's predicate is filtered on a row
// the step already located; nothing locates a row by one. Serving a function
// predicate from a structure - AR1 §9.2's cover and monotone methods - is
// AQ's, so neither field is declared here: a field with no reader is a
// promise nothing checks.
//
// ---- Purity, and the default (AR1 D2, `[quiet-wrong]`, ratified) --------
//
// **An entry that declares nothing is `kVolatileRow`**, the conservative end:
// a statement calling one is D2, which is never registered, recorded or
// sighted. The default is the struct's own initializer, so an entry written
// without a purity gets it by construction rather than by a reviewer noticing.
// Inside AP the class guards cost rather than answers - a mis-recorded D2 trail
// still replays through rules 0, 0' and 1 and misses - and it becomes a guard
// on answers once AQ evaluates a function on the write path (AR1 §10).

namespace kds::exec {

enum class Purity : std::uint8_t {
    kImmutable,          // a function of its arguments, forever
    kStable,             // constant within one statement: NOW()
    kVolatileStatement,  // one fresh value per statement, not from its arguments
    kVolatileRow,        // one fresh value per row - the default
};

// A statement's class, the most volatile purity among the functions in its
// predicates (AR1 §3). Ordered, so the fold is a max.
//
// **D1 is folded into the instance key nowhere** (AP-Q5, deferred to AQ): a D1
// statement is treated as D0 by every consumer AP has. That is sound for the
// trail - a replayed row is re-filtered by the function conjunct with this
// statement's value - and the fold buys nothing until AQ observes into a Cabin.
enum class DeterminismClass : std::uint8_t { kD0 = 0, kD1 = 1, kD2 = 2 };

DeterminismClass ClassOf(Purity purity) noexcept;

// The statement's own constants a function may read. Taken once per statement
// by the compiler (step_compiler.cpp), so `NOW()` is one value however many
// times a statement calls it and however many rows it filters.
struct StatementContext {
    std::int64_t now_us = 0;  // microseconds since the epoch, UTC (types.md TY4)
};

// The most arguments an entry may take. The evaluator keeps a call's
// arguments on the stack in an array this wide, so an entry declaring more
// fails its own static_assert in functions.cpp.
inline constexpr std::size_t kMaxFunctionArgs = 4;

struct FunctionEntry {
    std::string_view name;  // compared case-insensitively
    std::uint8_t arity = 0;

    // **The default is the ratified one (D2).** Do not reorder this field's
    // initializer away from kVolatileRow.
    Purity purity = Purity::kVolatileRow;

    // Every argument's required catalog type (`kTypeVal*`), and the result's.
    // One type for all arguments: neither shipped function takes two.
    std::uint32_t arg_type_val = 0;
    std::uint32_t result_type_val = 0;

    // A NULL argument yields NULL before this is called, so it never sees one.
    parser::AstValue (*evaluate)(std::span<const parser::AstValue> args,
                                 const StatementContext& context) = nullptr;
};

// The entry `name` names, or nullptr. Looked up case-insensitively, the way
// every other name in the grammar is.
const FunctionEntry* FindFunction(std::string_view name) noexcept;

// Adds `entry` to the catalog for this object's lifetime - **tests only**.
//
// The catalog is compiled into the binary and no `kVolatileRow` function ships
// (AP-Q1), so this is how the D2 path is reached at all. Registrations nest
// and unregister in reverse; nothing else may hold one across a statement
// another thread runs.
class ScopedTestFunction {
public:
    explicit ScopedTestFunction(const FunctionEntry& entry);
    ~ScopedTestFunction();
    ScopedTestFunction(const ScopedTestFunction&) = delete;
    ScopedTestFunction& operator=(const ScopedTestFunction&) = delete;
};

}  // namespace kds::exec
