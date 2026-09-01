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

}  // namespace kds::server
