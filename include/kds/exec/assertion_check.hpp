#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/assertion_build.hpp"
#include "kds/exec/bound_cabin.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/checkpointer.hpp"  // AS6a: the snapshot seam this registry implements

// The write-path admission and reservation protocol (docs/spec/assertion.md
// §§4.2, 6.2; workplan AST07): the one place a write is checked against a
// declared assertion, and the bookkeeping that makes a refused race lose
// cleanly and an aborted transaction restore the aggregates exactly.
//
// ---- The FK shape, for FK's reason ----------------------------------------
//
// The spec says "compile the check into the step chain"; that mechanism does
// not exist to use, exactly as `fk_check.hpp` records - INSERT compiles to
// no chain and UPDATE/DELETE walk a single Step outside the step VM. So this
// is a helper called from the three write paths, **one implementation, no
// trigger machinery**, which is the part of the placement decision that is
// not up for discussion. A consequence worth naming: because no compiled
// plan embeds a check step, the plan cache does not depend on the assertion
// set, and CREATE/DROP ASSERTION need no plan invalidation - the door
// `assertion_catalog.cpp`'s publish comment left to this task closes itself.
//
// ---- Every write is a departure, an arrival, or both (§4.2) ---------------
//
//   INSERT                     arrival, admission-checked
//   UPDATE, aggregate invariant  nothing (no entry, no delta)
//   UPDATE, same group, SUM moved  departure + arrival; checked iff delta > 0
//   UPDATE, group moved        departure + arrival; the arrival is checked
//   DELETE                     departure, check-free (AS11)
//
// A departure entry carries kEntryDeparture and contributes (-1, -value);
// DELETE's is required by §5's coverage contract - "100% of live rows" - not
// by any check: a header that kept counting deleted rows would overstate
// forever (nothing prunes) and refuse valid writes without bound.
//
// ---- Reservations and the transaction (§6.2) ------------------------------
//
// A reservation counts in the aggregate from the moment of admission, which
// is what makes a false admission impossible and is §4.3's deliberate
// stricter-than-snapshot semantics. The pending set is keyed by the writing
// transaction's id; COMMIT clears the RESERVED flags (batched per page,
// ASSERT_COMMIT), ABORT reverses each reservation (ASSERT_ROLLBACK). In the
// no-transaction-manager configuration every statement is its own
// transaction under kBootstrapXid, and the dispatcher ends it either way at
// statement end - one statement at a time per core is what makes the shared
// key safe.
//
// ---- Page spans -----------------------------------------------------------
//
// Reserving fetches cabin pages. UPDATE and DELETE call this from inside
// their own relation walk, which is the pre-existing exposure `fk_check.hpp`
// names and this shares rather than creates - safe only because nothing
// evicts.
//
// Concurrency: core-local, no latches (§6.1). The registry is
// dispatcher-owned and memory-resident; a restart loses it, and SHOW
// ASSERTIONS derives `enforcing` from its presence so the loss is reported
// rather than silent.

namespace kds::wal {
class WalManager;
}

namespace kds::exec {

// One published assertion, live on this core: everything the write hook
// needs, resolved once at CREATE (or by future recovery) so the per-write
// cost is lookups and never a catalog scan or a re-parse.
struct LiveAssertion {
    std::uint64_t assertion_id = 0;
    catalog::Oid target_oid = 0;
    std::string name;
    BoundAggregate aggregate = BoundAggregate::kCount;
    std::vector<std::uint16_t> group_cols;  // schema positions
    std::uint16_t sum_col = 0;              // schema position; read for kSum only
    std::string sum_col_name;
    std::vector<std::string> group_col_names;      // for the §4.4 message
    std::vector<std::uint32_t> group_type_vals;    // for the §4.4 message
    BoundCabinChainWriter chain;
    BoundCabin cabin;

    // §9's production counters, registry-resident like everything else here
    // - they die with the directory at restart, and SHOW prints them only
    // while the registry holds the assertion, so a zero is never a stale
    // number wearing a fresh face. Monotonic; nothing resets them.
    //
    // `hint_heals` from §9 is deliberately absent: no code path reads a
    // Bound Cabin entry's location hint today (admission is O(1) against
    // the header, and no read path walks entries), so the counter could
    // never move - and a counter nothing can increment is worse than none,
    // the INDEX_PAGE_INIT argument.
    struct Counters {
        std::uint64_t checks = 0;      // admission checks run
        std::uint64_t violations = 0;  // refusals answered
        std::uint64_t reserved = 0;    // entries reserved (arrivals and departures)
        std::uint64_t aborted = 0;     // reservations reversed by abort
    };
    Counters counters;

    LiveAssertion() : cabin(BoundAggregate::kCount, 0) {}
};

// The registry, and - since RC07 - the checkpoint's snapshot source: it is what
// holds the live directories, so it is what can hand their group headers to a
// checkpoint (AS6a). Implementing the seam here rather than wrapping it
// elsewhere keeps "who owns the directory" and "who can snapshot it" the same
// answer.
class AssertionEnforcer final : public wal::AssertionSnapshotSource {
public:
    bool empty() const noexcept { return live_.empty(); }
    bool Holds(std::uint64_t assertion_id) const { return live_.count(assertion_id) != 0; }
    bool AnyOn(catalog::Oid oid) const { return by_oid_.count(oid) != 0; }

    // ---- What this core knows about but cannot enforce (PW1c-6c) --------
    //
    // An assertion whose declaration this core can read and whose Bound
    // Cabin it may **not write**: an assertion built on core 0 for a
    // relation a peer owns, which is what every such assertion in a file
    // written before PW1c-6c is. There is no route to enforcing one - the
    // cabin's pages carry another core's stamp and `MayWrite` refuses them
    // - so what the knowledge buys is the *refusal*: the relation's owner
    // declines writes by name instead of admitting them unchecked, which
    // is the failure `bench/v2.2.0/results-shipping-part-a-v2.2.0-11-g925f483.md`
    // Finding 2 measured.
    //
    // Deliberately not a `LiveAssertion`: nothing is enforced from this,
    // and holding a directory nobody may append to would put a second
    // writer's shape on a chain that already has one.
    void NoteUnenforceable(catalog::Oid oid, std::uint64_t assertion_id);
    bool CannotEnforce(catalog::Oid oid) const { return unenforceable_.count(oid) != 0; }
    std::size_t unenforceable() const noexcept { return unenforceable_.size(); }

    // The counters, or nullptr while the registry does not hold the
    // assertion - the caller prints nothing then, rather than zeros that
    // would read as "counted and none happened".
    const LiveAssertion::Counters* CountersOf(std::uint64_t assertion_id) const {
        auto it = live_.find(assertion_id);
        return it == live_.end() ? nullptr : &it->second.counters;
    }

    void Adopt(LiveAssertion assertion);
    // Forgets `assertion_id` in **both** senses: the live directory if this
    // core holds one, and the unenforceable record if it holds that
    // instead. A DROP is the one statement that has to reach whichever of
    // the two a core is carrying, and it says the id, not which.
    void Evict(std::uint64_t assertion_id);

    // The checkpoint's base (AS6a, RC07): every live cabin's group headers,
    // `{group_id, key, count, sum}` and never the entry lists. Keys are owned by
    // the returned value, so nothing here dangles once the checkpoint encodes
    // them.
    std::vector<wal::AssertionCabinSnapshot> SnapshotAssertions() const override;

    // INSERT's admission, pure - run before the row id is allocated, FK's
    // ordering, so a refusal burns nothing. `values` are the statement's
    // VALUES list: columns after the pk, so schema position p is values[p-1].
    Status AdmitInsert(catalog::Oid oid, std::span<const parser::AstValue> values);

    // INSERT's reservation, after placement: the arrival entry, the delta,
    // the ASSERT_RESERVE record. The admission already passed and nothing
    // ran in between (one statement at a time, nothing suspends).
    Status ReserveInsert(storage::PageStore& store, wal::WalManager* wal, std::uint64_t txn_id,
                         catalog::Oid oid, std::span<const parser::AstValue> values,
                         std::uint64_t pk, PageId row_page, std::uint16_t row_slot);

    // UPDATE's per-row check-and-reserve, §4.2's table. `old_row`/`new_row`
    // are schema-indexed (pk at 0). Refusal leaves the row untouched - the
    // caller runs this before the undo record and the overwrite.
    Status AdmitAndReserveUpdate(storage::PageStore& store, wal::WalManager* wal,
                                 std::uint64_t txn_id, catalog::Oid oid,
                                 std::span<const parser::AstValue> old_row,
                                 std::span<const parser::AstValue> new_row, std::uint64_t pk,
                                 PageId row_page, std::uint16_t row_slot);

    // DELETE's per-row departure: check-free (AS11), maintenance only.
    Status ReserveDelete(storage::PageStore& store, wal::WalManager* wal, std::uint64_t txn_id,
                         catalog::Oid oid, std::span<const parser::AstValue> old_row,
                         std::uint64_t pk, PageId row_page, std::uint16_t row_slot);

    // Transaction end. Commit clears RESERVED flags on the entry pages and
    // logs ASSERT_COMMIT per (assertion, page); the aggregates were correct
    // from admission, so nothing else moves. Abort reverses each
    // reservation and logs ASSERT_ROLLBACK per entry. Both forget the
    // transaction's pending set; a transaction with none is a no-op.
    Status CommitTxn(storage::PageStore& store, wal::WalManager* wal, std::uint64_t txn_id);
    Status AbortTxn(storage::PageStore& store, wal::WalManager* wal, std::uint64_t txn_id);

private:
    // One applied reservation, remembered so commit and abort can find it.
    struct Reservation {
        std::uint64_t assertion_id = 0;
        std::string key;
        bool departure = false;
        std::int64_t value = 0;
        PageId page = kInvalidPageId;
        std::uint16_t index = 0;
    };

    Status ReserveOne(storage::PageStore& store, wal::WalManager* wal, std::uint64_t txn_id,
                      LiveAssertion& a, const std::string& key, bool departure,
                      std::int64_t value, std::uint64_t pk, PageId row_page,
                      std::uint16_t row_slot);

    std::unordered_map<std::uint64_t, LiveAssertion> live_;
    std::unordered_map<catalog::Oid, std::vector<std::uint64_t>> by_oid_;
    // The oids of `NoteUnenforceable`, with the ids that made them so - the
    // ids so that adopting one later clears exactly it and not its
    // relation's others.
    std::unordered_map<catalog::Oid, std::vector<std::uint64_t>> unenforceable_;
    std::unordered_map<std::uint64_t, std::vector<Reservation>> pending_;
};

}  // namespace kds::exec
