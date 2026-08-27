#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/page_store.hpp"

// `CREATE PATTERN` / `DROP PATTERN`: the validation chain and the catalog
// writes behind them
// (docs/spec/create-pattern-user-defined-patterns-v1.md section 6).
//
// ---- Why a declaration is validated this hard ----------------------------
//
// Declarative registration's entire payoff is *early feedback*. An
// auto-registered pattern is inferred from traffic that already ran, so
// there is nothing to tell an operator: whatever the statement was, it
// worked. A declaration is the one moment the engine can say "this will
// never match anything" or "this will convert a value on every execution"
// before a single row is read - and a declaration that is merely stored
// unchecked would give up the only thing declaring buys over observing.
//
// So every check below runs at CREATE, in spec order, **first failure
// wins**. Errors are InvalidArgument naming the check; the checks that
// describe a *cost* rather than a defect produce warnings instead, carried
// out in the result because the one-line protocol has no side channel.
//
// The line between the two is worth stating once: an **error** is a
// declaration that could never do what it says - a comparison that cannot
// evaluate, a relation that does not exist, a name already taken. A
// **warning** is a declaration that works and will disappoint - a per
// execution conversion, or a body whose trail can never replay. Refusing
// the second would be the engine overruling an operator about their own
// workload.

namespace kds::wal {
class WalManager;
}  // namespace kds::wal

namespace kds::exec {

// What a successful `CREATE PATTERN` did.
struct PatternDdlResult {
    std::uint64_t pattern_id = 0;

    // The directory depth the pattern ended up with - from
    // `expected_instances`, or 1 by default, or whatever an adopted row
    // already had.
    std::uint8_t dir_depth = 0;

    // True when an auto-registered row for this exact shape already existed
    // and was upgraded in place rather than replaced. Reported because the
    // difference matters to the operator: an adopted pattern keeps the
    // trails traffic already recorded under it, so declaring a hot shape is
    // never a performance regression.
    bool adopted = false;

    // The body's value-slot count, stored as the definition's arity.
    std::uint32_t param_count = 0;

    // One line per check that passed but has something to say. Never a
    // reason the declaration failed - a failure is a Status.
    std::vector<std::string> warnings;
};

// Runs section 6's checks and, if they pass, registers the pattern: the
// `sys.patterns` row (fresh or adopted), its `sys.pattern_defs` definition,
// and its waystone directory root.
//
// Fails with InvalidArgument for a failed check, Unsupported for a body form
// the compiler declines or a declaration too long to store, and whatever the
// catalog reports for a write that could not be made.
StatusOr<PatternDdlResult> CreatePattern(catalog::Catalog& catalog, storage::PageStore& store,
                                         wal::WalManager* wal,
                                          const parser::CreatePatternStmt& stmt);

// Removes a declaration by name: the `sys.pattern_defs` row and the
// `sys.patterns` row it named.
//
// The waystone tree under the pattern's root is **not** freed. Nothing
// reclaims pages yet, and invariant 8 makes leaving it safe - an orphaned
// trail can cost space, never an answer. Reclaiming it belongs to retention
// (P15), which is also the only thing that will know how.
//
// DROP removes the *declaration*, not the shape: if auto-registration later
// re-learns it, it reappears as a nameless auto row.
//
// Fails with NotFound when no declaration carries `name`.
StatusOr<std::uint64_t> DropPattern(catalog::Catalog& catalog, storage::PageStore& store,
                                    wal::WalManager* wal,
                                     std::string_view name);

}  // namespace kds::exec
