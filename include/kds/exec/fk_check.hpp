#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>
#include <utility>

#include "kds/base/status.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/exec/budget.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/read_view.hpp"
#include "kds/txn/undo_log.hpp"

// The two foreign-key checks (docs/spec/foreign-keys.md §§2-3, FK-M2 and
// FK-M3): does the parent a child row names exist, and does any child still
// reference a parent about to be deleted.
//
// ---- Why this is a helper and not a step (the F4 amendment) --------------
//
// F4 specifies the forward check as a compiled `kProbe` sub-chain, but the
// write paths do not run through the step VM (INSERT compiles to no chain;
// UPDATE/DELETE walk one `Step` in the dispatcher), so there is nowhere to
// inject one. F4's *intent* is kept whole: **one** implementation of each
// check, called from every write path, no trigger subsystem, no second
// evaluator. Given up: the VM's probe memo (the hoisted check's
// deduplication, `FkParentVerdicts` below, stands in for it on the forward
// side) and free ANALYZE integration (the caller records the access shape,
// FK-M4). If the writes ever become step chains, these functions are what
// the step implementation must agree with.
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
    // answer depends on how it ends. **The caller waits** on the writer's
    // decide and asks again (AO-S3); a path that cannot wait reports it as
    // kTxnConflict (F3).
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
// `busy_trx`, when given, is set to the transaction holding the parent row
// on a `kBusy` verdict and to 0 otherwise. **Since AY-S5 only a dispatcher
// with no lock table reads it**: with one, the caller holds the parent
// row's `S` before it descends, a writer's `X` refuses that ask, and the
// refusal is the wait - so a busy answer under the `S` does not arise.
// Without one, `IsInFlight(busy_trx)` is AO-S3's poll. Optional so the
// paths that cannot wait keep F3's retryable refusal.
StatusOr<FkVerdict> CheckParentPresent(storage::PageStore& store,
                                       const catalog::TableAccess& parent,
                                       std::uint64_t parent_pk,
                                       const txn::ReadView& check_view, Budget* budget,
                                       std::uint64_t* busy_trx = nullptr);

// ---- The hoisted forward check's held verdicts (AH-T1) -------------------
//
// `docs/spec/foreign-keys.md` §2a. Nothing can wait inside an open
// `WriteScope`, so the *asking* happens at the dispatch fork: every parent pk
// a statement needs is resolved before any row work, and the per-row check
// answers from what was resolved.
//
// This is that "what was resolved". One entry per **distinct (parent
// relation, parent pk)**, which is AH-R2's deduplication and the whole of
// why a thousand-row insert against one parent costs one descent rather
// than a thousand.
//
// **Filled in one place**: the extraction pass, from a descent this core
// makes; this holds answers, and nothing else. The FK probe reply and the
// parent-side intent retired (AT-S5f); their history is at cf889b63.
class FkParentVerdicts {
public:
    // The verdict for a parent pk this statement needs, or nullptr when it
    // was never resolved.
    //
    // **A miss is a caller bug, not a fall-back.** AH-R3: a statement whose
    // parent set could not be enumerated is refused at the fork rather than
    // run against a partial set, so by the time a row is written every pk
    // it names has an entry. Callers report a miss; they must never quietly
    // check it themselves, which would put a descent back inside the write
    // scope and make the crossing unreachable again.
    const FkVerdict* Find(catalog::Oid parent_rel, std::uint64_t parent_pk) const noexcept {
        auto at = entries_.find(Key{parent_rel, parent_pk});
        return at == entries_.end() ? nullptr : &at->second;
    }

    // Records one resolution. Idempotent on a repeat of the same key - the
    // extraction pass deduplicates, and this is the second half of that: a
    // statement naming one parent from twenty rows resolves it once.
    // Returns whether the key was new, so a caller can count descents.
    bool Put(catalog::Oid parent_rel, std::uint64_t parent_pk, FkVerdict verdict) {
        return entries_.emplace(Key{parent_rel, parent_pk}, verdict).second;
    }

    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }

    // AK-S3: whether the pks this holds were **collected** by a read-only
    // pass over the relation rather than named by the statement. It changes
    // what a miss in the per-row check means: for a named pk a miss is the
    // caller bug above; for a collected set it is a row that became visible
    // after the pass, which is retryable and never a fall-through.
    void set_collected() noexcept { collected_ = true; }
    bool collected() const noexcept { return collected_; }

private:
    // A map (AK-S3): the reverse direction keys it per collected row,
    // thousands for one DELETE, where a linear scan made the hoist quadratic
    // inside a reactor's synchronous span.
    using Key = std::pair<catalog::Oid, std::uint64_t>;
    std::map<Key, FkVerdict> entries_;
    bool collected_ = false;
};

// What the reverse check may use to answer without walking (F6, FK-M5).
struct FkReverseOptions {
    // The instance's Cabin store (AT-S7), or null when cabins are off.
    stats::CabinStore* cabins = nullptr;

    // The Cabin on the child's foreign-key column, or 0 for none.
    //
    // There is deliberately no `declared` flag beside it: that one exists to
    // decide n=1 versus n=2 *when recording*, and this check never records.
    std::uint64_t cabin_id = 0;

    // The log a child row's earlier version is read back through (AY-Q8):
    // a row an undecided `UPDATE` moved off the parent carries another
    // value in its page and the parent's in the version before it. Null
    // answers every such row busy, which waits where a read would not have
    // had to and never answers "no children" wrongly.
    txn::UndoLog* undo = nullptr;
};

struct FkReverseOutcome {
    FkVerdict verdict = FkVerdict::kPass;

    // The answer came from the Cabin's observed set: no walk happened.
    bool served_from_cabin = false;

    // On `kBusy`, the child row the answer rests on and the undecided
    // transaction writing it (AY-Q8): what a parent `DELETE` waits for,
    // since that writer holds no `S` on the parent to be refused by.
    std::uint64_t busy_trx = 0;
    std::uint64_t busy_pk = 0;
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
// **The walk reads a Cabin and never fills one** (a correction of F6): the
// value it would record is a pk being deleted, dead by construction, and
// would spend `cabin_max_values`' bounded budget that live query values need.
// The values that make this hit arrive from queries that filter children by
// parent id.
StatusOr<FkReverseOutcome> CheckNoChildReferences(storage::PageStore& store,
                                                  const catalog::TableAccess& child,
                                                  std::uint16_t child_column_no,
                                                  std::uint64_t parent_pk,
                                                  const txn::ReadView& check_view,
                                                  const FkReverseOptions& options,
                                                  Budget* budget);

}  // namespace kds::exec
