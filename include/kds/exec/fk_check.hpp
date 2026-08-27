#pragma once

#include <cstdint>

#include "kds/base/status.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/exec/budget.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/read_view.hpp"

// The two foreign-key checks (docs/spec/foreign-keys.md §§2-3, FK-M2 and
// FK-M3): does the parent a child row names exist, and does any child still
// reference a parent about to be deleted.
//
// ---- Why this is a helper and not a step (the F4 amendment) --------------
//
// F4 specifies the forward check as an implicit correlated sub-chain emitted
// by the step compiler - one `kProbe` on the parent - so that FK checks are
// ordinary steps with one evaluator, one executor and one statistics
// surface. **That mechanism does not exist to use.** `exec::Compile()` is
// SELECT-only: INSERT compiles to no chain at all, and UPDATE/DELETE compile
// a single `Step` that the dispatcher walks itself rather than running
// through the step VM. There is nowhere to inject a sub-chain on the paths
// that need one.
//
// So F4's *intent* is what this file keeps, and it keeps all of it: **one**
// implementation of each check, called from every write path, with no
// trigger subsystem and no second evaluator. What is given up is named
// rather than glossed:
//
//   - **no probe memo**, which lives in the step VM. Batch-inserting N
//     children of one parent pays N descents where §2 predicts one. That is
//     the cost to measure first if the checks are ever too slow.
//   - **no free ANALYZE integration.** The caller records the access shape
//     explicitly (FK-M4), where a step would have been counted by being a
//     step.
//
// Converting the write statements to step chains would restore both and is
// the larger change; when it happens, these functions are what the step
// implementation must agree with.
//
// ---- The visibility rule these run under ---------------------------------
//
// Both take a read view minted **at check time**, not the statement's, and
// judge by `txn::CheckVisibility` - latest state, in-flight writers visible
// as such. See docs/spec/foreign-keys.md §4 and the note in
// txn/visibility.hpp for why that is a different question from what a
// reader asks, and why it never walks the undo chain.
//
// ---- Page spans -----------------------------------------------------------
//
// The reverse walk performs **no nested page fetch under its own span**: it
// decodes without resolving spilled values, because the key it compares is
// an integer that always lives in the tuple. The Cabin path does fetch, but
// only outside any walk of its own. What neither can promise is the state of
// the caller's spans - DELETE runs its per-row work inside its own relation
// walk, which is a pre-existing exposure this shares rather than creates.

namespace kds::exec {

enum class FkVerdict : std::uint8_t {
    // The constraint is satisfied. Proceed with the write.
    kPass,
    // The constraint is violated by a state this view can see committed.
    // Re-running the statement will violate it again: kFkViolation.
    kViolation,
    // Another transaction is writing the row the check depends on, and the
    // answer depends on how it ends. F3: refuse now, retryably, rather than
    // wait - there is nothing to wait on under a cooperative single-writer
    // core. Reported as kTxnConflict.
    kBusy,
};

// **The forward check** (§2): is there a row with Keystone id `parent_pk` in
// `parent`, at latest state?
//
// Descends on a btree parent, which `catalog::CheckForeignKeyDeclaration`
// guarantees every declared foreign key has. The heap fall-back is a chain
// walk stopping at the id - unreachable through the DDL surface, and written
// out anyway because a check that cannot run is worse than a check that runs
// slowly, and because "can't happen" is not a thing to encode as an error.
StatusOr<FkVerdict> CheckParentPresent(storage::PageStore& store,
                                       const catalog::TableAccess& parent,
                                       std::uint64_t parent_pk,
                                       const txn::ReadView& check_view, Budget* budget);

// What the reverse check may use to answer without walking (F6, FK-M5).
struct FkReverseOptions {
    // The core-local Cabin store, or null when cabins are off.
    stats::CabinStore* cabins = nullptr;

    // The Cabin on the child's foreign-key column, or 0 for none.
    //
    // There is deliberately no `declared` flag beside it: that one exists to
    // decide n=1 versus n=2 *when recording*, and this check never records.
    std::uint64_t cabin_id = 0;
};

struct FkReverseOutcome {
    FkVerdict verdict = FkVerdict::kPass;

    // The answer came from the Cabin's observed set: no walk happened.
    bool served_from_cabin = false;
};

// **The reverse check** (§3): does any row of `child` reference `parent_pk`
// through column `child_column_no`, at latest state?
//
// Walks the child relation and **stops at the first match** (V03's stoppable
// walk), so a violation costs a prefix of the relation and only a pass costs
// all of it. That asymmetry is the argument for F6: the expensive case is
// the one that succeeds, and a Cabin on the child's fk column turns it into
// a lookup - an observed value's empty entry set is an authoritative "no
// children", which is the one thing RESTRICT wants and the one thing no
// advisory structure can witness.
//
// **The walk reads a Cabin and never fills one**, which corrects F6 rather
// than skipping part of it. The value a reverse check would record is the pk
// being deleted, and a pk is deleted once - so the entry would teach a value
// no later check can ask about, while `cabin_max_values` is a cap that
// refuses to observe once it is full. Recording here would spend a bounded
// budget on values that are dead by construction and could crowd out the
// live ones a query wanted. The values that make this hit arrive the
// ordinary way: from queries that filter children by parent id, which is the
// workload that justifies such a Cabin in the first place.
StatusOr<FkReverseOutcome> CheckNoChildReferences(storage::PageStore& store,
                                                  const catalog::TableAccess& child,
                                                  std::uint16_t child_column_no,
                                                  std::uint64_t parent_pk,
                                                  const txn::ReadView& check_view,
                                                  const FkReverseOptions& options,
                                                  Budget* budget);

}  // namespace kds::exec
