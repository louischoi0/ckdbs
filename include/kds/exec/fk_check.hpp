#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <utility>

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

// ---- The hoisted forward check's held verdicts (AH-T1) -------------------
//
// `docs/spec/foreign-keys.md` §2a. The forward check used to run per row
// from inside an open `WriteScope`, which is where nothing can wait - so a
// check that must ask another core cannot ask from where it stands. AH
// moves the *asking* out to the dispatch fork: every parent pk a statement
// needs is resolved before any row work, and the per-row check then answers
// from what was resolved.
//
// This is that "what was resolved". One entry per **distinct (parent
// relation, parent pk)**, which is AH-R2's deduplication and the whole of
// why a thousand-row insert against one parent costs one descent rather
// than a thousand.
//
// **Not to be confused with `server/fk_intent.hpp`.** That is the
// *parent* side - what a parent's owner remembers so its own DELETE can
// answer busy. This is the *child* side, statement-scoped, and it holds
// answers rather than promises. AH-T2 fills it from a `kFkProbeRequest`
// reply for a foreign parent; AH-T1 fills it locally, and the shape is the
// same either way, which is the point of landing them apart.
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
        for (const Entry& e : entries_) {
            if (e.parent_rel == parent_rel && e.parent_pk == parent_pk) return &e.verdict;
        }
        return nullptr;
    }

    // Records one resolution. Idempotent on a repeat of the same key - the
    // extraction pass deduplicates, and this is the second half of that: a
    // statement naming one parent from twenty rows resolves it once.
    // Returns whether the key was new, so a caller can count descents.
    bool Put(catalog::Oid parent_rel, std::uint64_t parent_pk, FkVerdict verdict) {
        if (Find(parent_rel, parent_pk) != nullptr) return false;
        entries_.push_back(Entry{parent_rel, parent_pk, verdict});
        return true;
    }

    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }

    // ---- The foreign half: one group per owner, not per pk (AH-R2) -----
    //
    // A parent whose owner is not this core cannot be resolved by
    // descending - its pages are another core's - so it is *grouped* here
    // instead, and the group is what one `kFkProbeRequest` carries. The
    // unit is the **owner**, which is why a statement's cross-owner cost
    // counts owners and not rows.
    struct ForeignGroup {
        std::uint32_t owner_core = 0;
        // Deduplicated, in first-seen order. Order is not load-bearing for
        // correctness - the reply is matched by pk - but it is stable,
        // which keeps a refusal's message stable run to run.
        std::vector<std::pair<catalog::Oid, std::uint64_t>> parents;
    };

    // Records that `(parent_rel, parent_pk)` lives on `owner_core`, which
    // is not this core. Idempotent on a repeat, like `Put`.
    void Defer(std::uint32_t owner_core, catalog::Oid parent_rel, std::uint64_t parent_pk) {
        for (ForeignGroup& g : foreign_) {
            if (g.owner_core != owner_core) continue;
            for (const auto& [rel, pk] : g.parents) {
                if (rel == parent_rel && pk == parent_pk) return;
            }
            g.parents.emplace_back(parent_rel, parent_pk);
            return;
        }
        foreign_.push_back(ForeignGroup{owner_core, {{parent_rel, parent_pk}}});
    }

    const std::vector<ForeignGroup>& foreign() const noexcept { return foreign_; }
    bool has_foreign() const noexcept { return !foreign_.empty(); }

private:
    std::vector<ForeignGroup> foreign_;

    // A vector with a linear scan, for `SysObjectRegistry`'s reason: the
    // set is one entry per distinct parent pk a *single statement* names,
    // which is small - and a map would cost an allocation per entry to save
    // comparisons that are not being made. If a statement ever names
    // thousands of distinct parents this is the line to revisit, and
    // AH-T6's counters are what would say so.
    struct Entry {
        catalog::Oid parent_rel;
        std::uint64_t parent_pk;
        FkVerdict verdict;
    };
    std::vector<Entry> entries_;
};

// What the reverse check may use to answer without walking (F6, FK-M5).
// ---- The reverse fan-out's groups (AJ-T2/AJ-T3) --------------------------
//
// The mirror of `FkParentVerdicts::ForeignGroup` above, and here beside it
// for the same reason: the *sender* is the dispatch fork, so the shape it
// builds belongs where the forward's does rather than in the wire service
// that carries it. `command_dispatcher.hpp` includes this header and
// deliberately not `server/fk_probe_service.hpp`.
//
// One entry is one question - *"does any row of `child_oid` reference
// `parent_pk` through column `child_column_no`"* - which is a triple where
// the forward's is a pair, and is why AJ-R6 gives the two directions
// separate wire kinds rather than one with a flag.
struct FkReverseProbeEntry {
    catalog::Oid child_oid = 0;
    std::uint64_t parent_pk = 0;
    std::uint16_t child_column_no = 0;
};

// Every reverse question for one child owner, which is what one round is. A
// parent with three foreign children on one core costs one message, not
// three - AH-R2's deduplication rule, applied to the other direction.
struct FkReverseProbeGroup {
    std::uint32_t owner_core = 0;
    std::vector<FkReverseProbeEntry> entries;
};

struct FkReverseOptions {
    // The core-local Cabin store, or null when cabins are off.
    stats::CabinStore* cabins = nullptr;

    // The Cabin on the child's foreign-key column, or 0 for none.
    //
    // There is deliberately no `declared` flag beside it: that one exists to
    // decide n=1 versus n=2 *when recording*, and this check never records.
    std::uint64_t cabin_id = 0;

    // The core running this check (`CommandDispatcher::core_id_`), which
    // bounds what its answer is good for. The scope guard at the top of
    // `CheckNoChildReferences` is where that matters and why.
    //
    // **The default is safe because there is exactly one caller and it
    // sets it** - not because 0 is a harmless value. A future caller on
    // core 3 that forgot, against a child whose ranges are all core 0's,
    // would pass the guard and walk another core's chains.
    std::uint32_t core_id = 0;
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
