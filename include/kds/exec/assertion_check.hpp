#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "kds/base/latch.hpp"
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
// ---- Concurrency: one registry for the instance (AT-S5d, AT-R15) ----------
//
// **One `AssertionEnforcer` serves every core**: `Expeditor` owns it,
// `CoreRuntime::Config` hands it to each peer, and a dispatcher built with
// none builds its own - `LockTable`'s idiom. It was one per core, holding
// the directories of the relations that core owned, until AT-S5 made a
// write run where its session is; a write on a core whose registry held no
// directory was then admitted unchecked (D1). Memory-resident still: a
// restart loses it, and SHOW ASSERTIONS derives `enforcing` from its
// presence so the loss is reported rather than silent.
//
// **Two latches, both null unarmed** (`base/latch.hpp`'s G2 shape - a
// registry built for one core takes neither):
//
//   - the **directory latch** (`latch_`) guards every map below, every
//     Bound Cabin's group directory and counters, and the admission holds.
//     **It never spans page work**: nothing under it fetches, creates or
//     stamps a page. What it does span is a WAL append - the one that
//     describes a header change - because that pairing is what keeps a
//     checkpoint's snapshot equal to the fold of the records before it
//     (below).
//   - a **chain latch** per assertion (`Live::chain_latch`), which does
//     span page work: it serialises one Bound Cabin's appends, whose tail
//     and growth are one writer's state, so two cores never both grow the
//     tail and lose a link.
//
// The order, **stated and not checked**, the page latch's own status
// (`rules.md` §3): a relation's page latch (UPDATE and DELETE reserve from
// inside their walk), then the chain latch, then the cabin page's latch,
// then the directory latch, then the WAL stream latch. Nothing takes the
// directory latch and then a page latch: commit and abort take it, release
// it, and only then touch a page. Nothing parks under either - every call
// here is synchronous.
//
// **The admission holds its delta** (§6.2 steps 2-3 made one step across
// cores). `AdmitInsert` checks and *holds* the row's contribution under the
// directory latch, and `ReserveInsert` converts the hold into the logged
// reservation after placement - so two cores cannot both admit at count 0
// and both reserve, which is what the pure admission of the one-core engine
// could not stop once a second thread existed. A hold is **not** in the
// group header: it counts in every admission and in nothing a checkpoint
// snapshots, and it becomes header exactly when its `ASSERT_RESERVE` is
// appended. `Hold` releases whatever it did not convert, so a statement
// that fails - or parks and re-runs - between the two calls gives back
// what it held.
//
// **The snapshot is taken with the latch held across its records**
// (`VisitSnapshots`). A checkpoint's `ASSERT_SNAPSHOT` must equal the fold
// of every `ASSERT_*` record before it (`assertion_recover.hpp`), and with
// a reserver on another thread that holds only if the header change and
// its record are atomic against the snapshot and its records - which is
// the directory latch around both.

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
    // `shared` arms both latches - the instance's registry at `cores > 1`.
    // A registry built unarmed takes neither, which is every fixture's and a
    // one-core instance's shape (`base/latch.hpp`'s G2).
    explicit AssertionEnforcer(bool shared = false)
        : latch_(shared ? std::make_unique<Latch>() : nullptr) {}
    AssertionEnforcer(const AssertionEnforcer&) = delete;
    AssertionEnforcer& operator=(const AssertionEnforcer&) = delete;

    bool empty() const;
    bool Holds(std::uint64_t assertion_id) const;
    bool AnyOn(catalog::Oid oid) const;

    // ---- What the instance knows about but cannot enforce (PW1c-6c) -----
    //
    // An assertion whose declaration can be read and whose Bound Cabin could
    // not be revived. What the knowledge buys is the *refusal*: the
    // relation's writes are declined by name instead of admitted unchecked,
    // which is the failure
    // `bench/v2.2.0/results-shipping-part-a-v2.2.0-11-g925f483.md`
    // Finding 2 measured. What reaches this set is a revive that failed or
    // a checkpoint whose snapshots do not cover the base
    // (`server/mount_recovery.cpp`).
    //
    // Deliberately not a `LiveAssertion`: nothing is enforced from this,
    // and holding a directory nobody may append to would put a second
    // writer's shape on a chain that already has one.
    void NoteUnenforceable(catalog::Oid oid, std::uint64_t assertion_id);
    bool CannotEnforce(catalog::Oid oid) const;
    std::size_t unenforceable() const;

    // The counters, copied under the latch, or nothing while the registry
    // does not hold the assertion - the caller prints nothing then, rather
    // than zeros that would read as "counted and none happened". A copy
    // rather than a pointer: another core may be moving them.
    std::optional<LiveAssertion::Counters> CountersOf(std::uint64_t assertion_id) const;

    void Adopt(LiveAssertion assertion);
    // Forgets `assertion_id` in **both** senses: the live directory if the
    // registry holds one, and the unenforceable record if it holds that
    // instead. A DROP says the id, not which.
    void Evict(std::uint64_t assertion_id);

    // The checkpoint's base (AS6a, RC07): every live cabin's group headers,
    // `{group_id, key, count, sum}` and never the entry lists, and never a
    // hold. Keys are owned by the returned value.
    std::vector<wal::AssertionCabinSnapshot> SnapshotAssertions() const override;
    // The same, with the directory latch held until `visit` returns - the
    // checkpointer's entry point, so its `ASSERT_SNAPSHOT` records are
    // appended with no `ASSERT_*` record able to land between the headers
    // they carry and their own LSN (the header's snapshot paragraph).
    Status VisitSnapshots(const SnapshotVisitor& visit) const override;

    // ---- An admission's held contribution (AT-S5d) ------------------------
    //
    // What `AdmitInsert` took and `ReserveInsert` has not yet logged. The
    // destructor gives back whatever was not converted, so every exit from
    // the statement between the two calls - a refusal, a failed placement,
    // a borrow that parks and re-runs the statement - releases it. Neither
    // copyable nor movable: it lives in the frame of the one row it admits.
    class Hold {
    public:
        Hold() = default;
        ~Hold() { Release(); }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;

        void Release();

    private:
        friend class AssertionEnforcer;
        struct Item {
            std::uint64_t serial = 0;
            std::uint64_t assertion_id = 0;
        };
        AssertionEnforcer* owner_ = nullptr;
        std::vector<Item> items_;
    };

    // INSERT's admission - run before the row id is allocated, FK's
    // ordering, so a refusal burns nothing. `values` are the statement's
    // VALUES list: columns after the pk, so schema position p is values[p-1].
    //
    // With `hold`, an admitted row's contribution is **held** until
    // `ReserveInsert` logs it; without one the call is a pure check, which
    // is a test's and a diagnostic's use and never a write path's.
    //
    // `reserver`, when given, is filled on a refusal with a transaction
    // whose reservation or hold is part of what refused it - the caller's
    // cue to wait rather than fail (AO-S6e-c). Left at 0 when the refusing
    // aggregate is settled.
    Status AdmitInsert(catalog::Oid oid, std::span<const parser::AstValue> values,
                       std::uint64_t writer_txn = 0, std::uint64_t* reserver = nullptr,
                       Hold* hold = nullptr);

    // INSERT's reservation, after placement: the arrival entry, the
    // ASSERT_RESERVE record, and the hold becoming header. An assertion the
    // hold does not cover - adopted between the two calls - is admitted
    // here instead, which can refuse: the row is placed by then, so that
    // refusal is the statement's failure and not a wait.
    Status ReserveInsert(storage::PageStore& store, wal::WalManager* wal, std::uint64_t txn_id,
                         Hold& hold, catalog::Oid oid, std::span<const parser::AstValue> values,
                         std::uint64_t pk, PageId row_page, std::uint16_t row_slot);

    // UPDATE's per-row check-and-reserve, §4.2's table. `old_row`/`new_row`
    // are schema-indexed (pk at 0). Refusal leaves the row untouched - the
    // caller runs this before the undo record and the overwrite.
    Status AdmitAndReserveUpdate(storage::PageStore& store, wal::WalManager* wal,
                                 std::uint64_t txn_id, catalog::Oid oid,
                                 std::span<const parser::AstValue> old_row,
                                 std::span<const parser::AstValue> new_row, std::uint64_t pk,
                                 PageId row_page, std::uint16_t row_slot,
                                 std::uint64_t* reserver = nullptr);

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
    // One live assertion and the latch its chain appends under. Held by
    // `shared_ptr` so a reservation that copied it out under the directory
    // latch can finish its page work after a concurrent `Evict` - into a
    // chain no catalog row names any more, which orphans pages and changes
    // no answer.
    struct Live {
        LiveAssertion a;
        std::unique_ptr<Latch> chain_latch;  // null unarmed
    };

    // One applied reservation, remembered so commit and abort can find it.
    struct Reservation {
        std::uint64_t assertion_id = 0;
        std::string key;
        bool departure = false;
        std::int64_t value = 0;
        PageId page = kInvalidPageId;
        std::uint16_t index = 0;
    };

    // One admitted, unlogged contribution. `held_` carries the sum per
    // group for the admission's arithmetic; this record is what a waiter's
    // `ReserverOnLocked` and a release need.
    struct HeldContribution {
        std::uint64_t txn_id = 0;
        std::uint64_t assertion_id = 0;
        std::string key;
        std::int64_t value = 0;
    };

    // **Who to wait for when an admission is refused** (AO-S6e-c, census
    // row 11). Answers a transaction other than `exclude_txn` whose net
    // reservations and holds on `(assertion_id, key)` are positive, or 0
    // when the aggregate that refused is settled - in which case the
    // refusal is true and no wait could change it. Any one of them, not
    // all: the caller waits for that decide and asks again. Whether it is
    // still in flight is the caller's question - the enforcer holds no
    // transaction manager, and `NoteBlockingWriter` tests it anyway. On the
    // refusal path only: an admitted write never asks.
    std::uint64_t ReserverOnLocked(std::uint64_t assertion_id, const std::string& key,
                                   std::uint64_t exclude_txn) const;

    // Under the directory latch, all four. `FitsLocked`: whether `check`
    // more fits `key`'s group with every held contribution counted.
    // `HoldLocked`: holds `value` for `txn_id` and answers the hold's
    // serial. The amount checked and the amount held differ for an `UPDATE`
    // (`AdmitAndReserveUpdate` says why); an `INSERT` passes one number to
    // both.
    StatusOr<bool> FitsLocked(const LiveAssertion& a, const std::string& key,
                              std::int64_t check) const;
    StatusOr<std::uint64_t> HoldLocked(std::uint64_t txn_id, std::uint64_t assertion_id,
                                       const std::string& key, std::int64_t value);
    void ReleaseHoldLocked(std::uint64_t serial);
    std::vector<wal::AssertionCabinSnapshot> SnapshotLocked() const;

    // Appends one entry and logs it, the header change and the hold's
    // conversion (`serial`, 0 for none) atomic with the record under the
    // directory latch; the page work runs under `live`'s chain latch only.
    Status ReserveOne(storage::PageStore& store, wal::WalManager* wal, std::uint64_t txn_id,
                      Live& live, const std::string& key, bool departure, std::int64_t value,
                      std::uint64_t pk, PageId row_page, std::uint16_t row_slot,
                      std::uint64_t serial);

    std::vector<std::shared_ptr<Live>> OnLocked(catalog::Oid oid) const;

    std::unique_ptr<Latch> latch_;  // the directory latch; null unarmed
    std::unordered_map<std::uint64_t, std::shared_ptr<Live>> live_;
    std::unordered_map<catalog::Oid, std::vector<std::uint64_t>> by_oid_;
    // The oids of `NoteUnenforceable`, with the ids that made them so - the
    // ids so that adopting one later clears exactly it and not its
    // relation's others.
    std::unordered_map<catalog::Oid, std::vector<std::uint64_t>> unenforceable_;
    std::unordered_map<std::uint64_t, std::vector<Reservation>> pending_;
    // Held contributions by serial, and their sum per (assertion, group).
    std::unordered_map<std::uint64_t, HeldContribution> holds_;
    std::unordered_map<std::uint64_t, std::unordered_map<std::string, std::int64_t>> held_;
    std::uint64_t next_hold_serial_ = 0;
};

}  // namespace kds::exec
