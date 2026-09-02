#pragma once

#include <cstdint>
#include <map>
#include <set>

#include "kds/catalog/oid.hpp"

// SA-T4 — the **row-scoped reference intent**, and the whole of what a
// parent's owner learns from a foreign child (work order
// `instructions/v2.7.1/workorder-sa.md` SA-T4;
// `docs/inflight/in-progress/workplan-auxiliaries-under-split.md` §4).
//
// ---- What problem this exists for --------------------------------------
//
// A foreign key's forward check asks *"does parent row `pk` exist"*. Once
// parent and child are on different cores (SA-R6 relaxing F5), the child's
// core asks that question over the ring — and between the answer and the
// child's commit there is a window in which the parent's owner could delete
// the very row it just vouched for. Nothing local to either core closes it:
// the parent's owner cannot see the child (that relation is not its own),
// and the child's core cannot hold the parent still.
//
// So the probe **leaves something behind**. An intent is one sentence from
// a foreign transaction: *"I am relying on `(relation, pk)` existing."* A
// parent-side DELETE that meets a live intent it does not own answers
// **busy** — retryable, `kTxnConflict`, F3's fail-fast rule unchanged — and
// the client retries once the intent's transaction has decided.
//
// ---- Why this is not a lock --------------------------------------------
//
// It never blocks and nothing ever waits on it: F3 forbids waiting outright
// ("blocking is not expressible on a cooperative single-writer core"), and
// an intent is read exactly once, by a check that immediately answers. Two
// consequences follow and both are wanted: there is no deadlock to detect,
// because there is no wait-for edge; and a transaction holding intents is
// not privileged — the parent's DELETE loses the race and retries, rather
// than the child's insert being rolled back.
//
// ---- Lifecycle: memory-resident, and it dies with its participant -------
//
// An intent is created by a probe and released by the transaction's
// **decide**, which every cross-owner transaction already sends — so there
// is no third message and no `done` leg. It is **not durable**, and under
// SA-T0 that is not an omission: a participant that wrote nothing writes no
// `TXN_PREPARE` at all, so there is no record for an intent to ride in.
// `cross-owner-txn.md` §1a is what makes that safe — such a participant is
// "still prepared, still in doubt, still decided by the coordinator, still
// counted" — the tracking is memory-resident and only the *record* is
// skipped.
//
// **What a restart therefore loses, stated rather than discovered**: a
// participant that restarts mid-transaction forgets its intents, and a
// parent DELETE arriving afterwards no longer sees one. That window is
// closed by the transaction rather than by this table — the coordinator's
// in-doubt resolution fails a transaction whose participant lost its state
// (`prepared_resolver.hpp`), so the child insert that relied on the intent
// does not commit either. Recorded here because the argument lives in two
// files and this is the one a reader of the table reaches first.
//
// Concurrency: core-local, no synchronization (rules.md #3). One table per
// core; every map below is that core's own.

namespace kds::server {

// Who holds an intent: the coordinator's identity for the transaction, not
// this core's.
//
// **`(coordinator core, session_id)` and deliberately no transaction id**,
// because that pair is already this engine's name for "which foreign
// transaction is this" — `ShippedStatementExecutor::DedupKey` keys a
// participant's whole transaction context on exactly it, and the statement
// leg carries no transaction id at all. A third field here would be one
// this table could record from a probe and then never match against, since
// the shipped DELETE that has to ask `HeldByAnotherThan` knows only the
// pair.
//
// **What makes the pair unique per transaction is `Session::Finish()`**,
// which clears `ship_id_` whenever a transaction enrolled anyone — added
// for this exact hazard on the context itself (R6-8's review: two
// consecutive transactions of one session were indistinguishable, and the
// second joined the first's context). This table inherits that guarantee
// rather than restating it, which is the point of using the same key.
struct FkIntentHolder {
    std::uint32_t coordinator_core = 0;
    std::uint64_t session_id = 0;

    bool operator<(const FkIntentHolder& other) const noexcept {
        if (coordinator_core != other.coordinator_core) {
            return coordinator_core < other.coordinator_core;
        }
        return session_id < other.session_id;
    }
    bool operator==(const FkIntentHolder& other) const noexcept {
        return coordinator_core == other.coordinator_core && session_id == other.session_id;
    }
};

class FkIntentTable {
public:
    // The row an intent names. Ordered rather than hashed for the reason
    // `RefusalCounters` is: what this table holds is reportable through
    // `SHOW META`, and a report must be stable run to run (sched.md §8).
    struct Key {
        catalog::Oid parent_oid = 0;
        std::uint64_t parent_pk = 0;

        bool operator<(const Key& other) const noexcept {
            if (parent_oid != other.parent_oid) return parent_oid < other.parent_oid;
            return parent_pk < other.parent_pk;
        }
    };

    // Records that `holder` is relying on `(parent_oid, parent_pk)`.
    //
    // Idempotent, and it has to be: a statement may reference one parent
    // from several rows, a retry may re-probe, and neither is a second
    // reliance. The set is what makes that free rather than a check.
    void Add(catalog::Oid parent_oid, std::uint64_t parent_pk, const FkIntentHolder& holder) {
        intents_[Key{parent_oid, parent_pk}].insert(holder);
        ++stats_.recorded;
    }

    // Whether anyone **other than `self`** is relying on this row.
    //
    // The exclusion is not a courtesy: a transaction that inserted a child
    // and then deletes the parent has met its own reliance, and answering
    // busy there would be a retry that fails identically forever. What that
    // case actually is — a violation the transaction can see for itself —
    // is the *local* check's to say, and it says it, because the child row
    // is visible to the transaction that wrote it.
    bool HeldByAnotherThan(catalog::Oid parent_oid, std::uint64_t parent_pk,
                           const FkIntentHolder& self) const {
        auto it = intents_.find(Key{parent_oid, parent_pk});
        if (it == intents_.end()) return false;
        for (const FkIntentHolder& holder : it->second) {
            if (!(holder == self)) return true;
        }
        return false;
    }

    // Every intent this holder took, released. The decide's half, and the
    // only way an intent ends.
    //
    // Returns how many rows it freed, which is what the counter reports.
    // **Idempotent for the decide's reason**: a resent decide is a benign
    // no-op here, exactly as `TxnDecideRequestPayload::retry` describes for
    // the prepared state it accompanies.
    std::size_t Release(const FkIntentHolder& holder) {
        std::size_t freed = 0;
        for (auto it = intents_.begin(); it != intents_.end();) {
            if (it->second.erase(holder) != 0) ++freed;
            it = it->second.empty() ? intents_.erase(it) : std::next(it);
        }
        stats_.released += freed;
        return freed;
    }

    // How many distinct rows currently carry an intent. A number with a
    // cost rather than a rate: each one is a row a foreign transaction can
    // make a local DELETE retry on.
    std::size_t live_rows() const noexcept { return intents_.size(); }

    struct Stats {
        std::uint64_t recorded = 0;   // intents taken (idempotent adds included)
        std::uint64_t released = 0;   // rows freed by a decide
        std::uint64_t refusals = 0;   // DELETEs answered busy by a foreign intent
    };

    void NoteRefusal() noexcept { ++stats_.refusals; }
    const Stats& stats() const noexcept { return stats_; }

    const std::map<Key, std::set<FkIntentHolder>>& intents() const noexcept { return intents_; }

private:
    std::map<Key, std::set<FkIntentHolder>> intents_;
    Stats stats_;
};

// AJ-T1 — the **pending-delete set**, and the mirror of everything above
// (work order `instructions/v2.8.0/workorder-aj.md` AJ-R3(a)).
//
// ---- What problem this exists for --------------------------------------
//
// `FkIntentTable` closes the window in which a parent's owner could delete
// a row a foreign child is relying on. AJ opens the other direction — a
// parent DELETE that fans out to the child's owner — and that has a window
// of its own, running the other way:
//
//   1. a DELETE of parent `P` on this core probes the child's owner, which
//      answers **no children**;
//   2. a child INSERT on that owner probes *back* for `P`, finds it present
//      and not yet marked, and is granted a reference intent;
//   3. that INSERT commits, and its decide releases the intent;
//   4. the DELETE's per-row check finds no intent, holds a "no children"
//      that stopped being true at step 2, and marks `P`.
//
// The result is a dangling reference with RESTRICT reporting success —
// which is the one outcome the whole crossing exists to prevent.
//
// **The fix is to register before probing.** A row named here is a row this
// core is about to delete, and `FkProbeServer` consults this table *before*
// it reads whether the row exists — so from the registration onward no new
// reference intent on `P` can be granted, and step 2 is told busy instead.
// A child that probed *before* the registration holds an intent, which is
// the case `CheckNoChildrenBeforeDelete` already meets.
//
// ---- Why this side and not the child's ---------------------------------
//
// The symmetric design leaves a "P is being deleted" record on the child's
// owner. That would make the child's owner an intent holder, and an
// intent-holding participant coordinates its own release: measured at
// **720x** a plain participant's acknowledgement leg (4.4 us -> 3.1 ms,
// `bench/v2.8.0/results-ah-t6-participant-release-cost-*`). Registering on
// the deleting core instead costs one map insert and one map erase, enrols
// nobody, adds no decide target and needs no ring message to end — AJ-R5,
// which is a ruling precisely so a later "symmetry" refactor cannot undo it
// without saying so.
//
// ---- Lifecycle ---------------------------------------------------------
//
// Memory-resident, like the intents beside it, and cleared when the
// deleting statement's transaction ends. **Not at the decide sites**, which
// is where the work order first put it: every one of those is guarded by
// `Session::has_intent_holders()`, and a DELETE whose only cross-core
// contact was a reverse round holds none — so the entry would never be
// cleared and the row would answer busy forever (AJ-T0's finding S4). The
// clear is `CommandDispatcher::EndWrite` for an autocommit statement and
// the explicit `COMMIT`/`ROLLBACK` paths for the other, both unconditional
// - plus the two foreign-key probe paths that end a parked autocommit
// statement without re-running it, which are `EndWrite`'s absence rather
// than a third kind of site (`ClearPendingDeletes` names them).
//
// What a restart loses is answered by the WAL rather than by this table
// (AJ-R4): a delete-mark this set covered is either committed — the row is
// gone and a forward probe answers absent — or rolled back, and the row is
// referenceable again. Neither outcome dangles.
//
// Concurrency: core-local, no synchronization (rules.md #3).
class FkPendingDeleteTable {
public:
    // The row a pending delete names — the same shape `FkIntentTable::Key`
    // has, and ordered for the same reason: what this table holds is
    // reportable, and a report must be stable run to run (sched.md §8).
    struct Key {
        catalog::Oid parent_oid = 0;
        std::uint64_t parent_pk = 0;

        bool operator<(const Key& other) const noexcept {
            if (parent_oid != other.parent_oid) return parent_oid < other.parent_oid;
            return parent_pk < other.parent_pk;
        }
    };

    // **The registrant is a session id alone, not an `FkIntentHolder`.**
    // Every row here was registered by a statement running on *this* core —
    // that is what "coordinator-local" means — so the core half of the
    // identity would be the same constant in every entry, and the table
    // itself is what carries it.
    //
    // A `set` rather than one id, for `FkIntentTable`'s reason: two
    // sessions can both be mid-DELETE of the same row across a park, and
    // one deciding must not clear the other's registration.
    void Add(catalog::Oid parent_oid, std::uint64_t parent_pk, std::uint64_t session_id) {
        pending_[Key{parent_oid, parent_pk}].insert(session_id);
        ++stats_.recorded;
    }

    // Whether anyone is about to delete this row.
    //
    // **No self-exclusion, unlike `FkIntentTable::HeldByAnotherThan` beside
    // it, and the asymmetry is forced rather than chosen.** The registrants
    // here are *this* core's session ids; a forward probe carries the
    // *asking* core's, and both are minted from a per-dispatcher counter
    // that starts at 1 (`command_dispatcher.hpp`'s `next_ship_session_id_`).
    // The two spaces therefore overlap numerically from the very first
    // session on each core, so an exclusion comparing them would answer
    // "not pending" for a row that *is* pending whenever the two numbers
    // collided - and the probe would then be vouched for and granted an
    // intent on a row on its way out. That is the dangling reference this
    // table exists to prevent, reintroduced by the guard meant to be
    // harmless.
    //
    // **Nothing is lost by dropping it.** A forward probe never originates
    // on the core that answers it - `SendForeignKeyProbes` defers only a
    // parent whose owner is *another* core - so no registrant of this table
    // can ever be the asker. And a transaction meeting its own pending
    // delete through a peer is answered `kBusy` by the delete-mark itself
    // under `CheckParentPresent`, with or without this test, so even a
    // working exclusion would not change that answer. The identity that
    // could make one meaningful is the *coordinator's*, which this table
    // does not store and which AJ-R5 gives it no reason to.
    bool Pending(catalog::Oid parent_oid, std::uint64_t parent_pk) const {
        // An entry is erased the moment its last registrant leaves
        // (`Release`), so a key that is present is a row someone is
        // deleting.
        return pending_.find(Key{parent_oid, parent_pk}) != pending_.end();
    }

    // Every row this session registered, released. Returns how many rows it
    // freed, which is what the counter reports.
    //
    // **Idempotent**, and it has to be: the clear runs at the end of every
    // write statement and again when a transaction ends, so a session that
    // registered nothing pays one empty walk and a session that did is not
    // harmed by being told twice.
    std::size_t Release(std::uint64_t session_id) {
        std::size_t freed = 0;
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.erase(session_id) != 0) ++freed;
            it = it->second.empty() ? pending_.erase(it) : std::next(it);
        }
        stats_.released += freed;
        return freed;
    }

    // How many distinct rows are currently being deleted with a fan-out
    // outstanding. A number with a cost rather than a rate: each one is a
    // row a foreign child's INSERT is told to retry on.
    std::size_t live_rows() const noexcept { return pending_.size(); }

    struct Stats {
        std::uint64_t recorded = 0;  // registrations (idempotent adds included)
        std::uint64_t released = 0;  // rows freed by a clear
        std::uint64_t refusals = 0;  // forward probes answered busy by a pending delete
    };

    void NoteRefusal() noexcept { ++stats_.refusals; }
    const Stats& stats() const noexcept { return stats_; }

    // **No whole-map accessor**, deliberately, where `FkIntentTable` has
    // one: nothing reads it. `live_rows()` and `stats()` are what a report
    // needs, and an accessor added against a reader that does not exist is
    // surface to keep working for nobody. The day `SHOW META` grows a
    // pending-delete block, it wants those two and not this.

private:
    std::map<Key, std::set<std::uint64_t>> pending_;
    Stats stats_;
};

}  // namespace kds::server
