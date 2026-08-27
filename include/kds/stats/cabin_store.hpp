#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/parser/ast.hpp"

namespace kds::stats {
// Forward-declared rather than included: the store only holds a pointer
// and forwards through it (optimizer_signals.hpp), and an include here
// would put the optimizer's header under every file that touches a Cabin.
class OptimizerSignals;
}  // namespace kds::stats

// The Cabin runtime store: the observed sets, the sighting counter, and the
// policy that decides when a value becomes observed
// (docs/spec/cabin.md §§3-5, docs/cabin-workplan.md CB04).
//
// ---- What "observed" means, precisely -----------------------------------
//
// **Observed ⇒ complete, superset form, per snapshot.** For an observed
// value `v`, this store's entry set is a *superset* of the pks of every
// visible tuple whose key column equals `v`. Missing a qualifying pk
// violates authority; a surplus entry never does, because the reader
// subtracts it - MVCC visibility plus a re-check of the key equality, both
// of which happen anyway on the authoritative path (spec §4).
//
// Three rules follow, and none of them is a preference:
//
//   1. **Maintenance is append-only.** INSERT appends if the new value is
//      observed; DELETE does nothing; UPDATE v→v′ appends to v′ and leaves
//      v's entry alone. Removing on the hot path is *incorrect*, not merely
//      unnecessary: an older snapshot may still be entitled to match the
//      row through the undo chain.
//   2. **A cap refuses to observe; it never truncates a set.** A truncated
//      set marked observed is precisely the "missing a qualifying pk" the
//      invariant forbids - a wrong answer, not a smaller one.
//   3. **Un-observing is always legal**, and is therefore the correct
//      response to *any* failure here. Dropping a value returns queries for
//      it to the authoritative scan: a performance event. This is what lets
//      an authoritative structure be evictable at all (§1's corollary), and
//      it is the value-granular analogue of Waystone's invariant 8.
//
// ---- Why this can live in memory ----------------------------------------
//
// §9 makes Cabin pages **unlogged authoritative**: the completeness promise
// holds only while the system is up and the write hook is live, so recovery
// declares every Cabin fully unobserved and traffic rebuilds them. v1 takes
// that to its conclusion and keeps the sets in memory only. What persists is
// the `sys.cabins` row - that a Cabin *exists*, which is DDL.
//
// ---- Why a scan can safely fill it --------------------------------------
//
// The classic build-by-observation hazard is a write slipping between the
// recording scan and the mark. KDS deletes it structurally: statements for a
// relation run to completion on its owning core (D3), so scan + record +
// mark-observed is atomic against every other statement (§6). This whole
// file is valid **only** under that model and under the Keystone issue-once
// invariant (K1), which is what makes a stored pk a name that can dangle but
// can never mis-attribute.
//
// Concurrency: core-local, no synchronization (rules.md #3). One store per
// core; every table below is that core's own.
//
// It **never fails a statement**: every policy path returns void or a bool,
// exactly as `TrailRecorder` does. A Cabin that cannot record is a Cabin
// that stays unobserved, which is the state every value starts in.

namespace kds::stats {

// Bit 0 of `CabinEntry::flags`: the location hint is worth trying.
//
// Cleared - never the entry dropped - when a hint has been proven wrong and
// there was nothing better to heal it with. The **pk is the authority**
// (C2), so an entry with no usable hint is still a complete, correct entry;
// it just costs a descent.
inline constexpr std::uint16_t kCabinHintValid = 0x1;

// One entry of one value's set: the authoritative half and the advisory
// half, in 24 bytes (spec §3, C2 + C6).
//
// **Authority lives in the pk, never in the location.** Under K1 a stored
// Keystone id is a forever-unique, immutable name: it can dangle - the row
// may be gone - but it can never come to mean a different row. That buys
// relocation invariance (the physical optimizer may move pages without ever
// touching a Cabin), no incarnation protocol, and "dangling ⇒ dead forever,
// droppable on sight".
//
// The location is waystone-class advice riding on cabin-class truth: it is
// verified by the reader under the same rules and the *same code* as a
// waystone entry (exec/tuple_verify.hpp), and any failure falls back to the
// pk. Hints cost nothing to produce - the recording scan and the write hook
// both hold the location in hand when they write the entry.
struct CabinEntry {
    std::uint64_t pk = 0;
    PageId page_id = kInvalidPageId;

    // The page's relayout epoch at the time the entry was written or last
    // healed (docs/spec/physical-optimizer.md R4; recorded and compared for
    // real since workplan PX04 - the field was here from C6, written 0
    // while the engine had no epoch, precisely so there would be nowhere
    // for the check *not* to happen the day it landed). Compared in
    // `exec/tuple_verify.hpp`, the one shared verifier; a mismatch is a
    // hint miss, resolved through the pk like every other miss. Until a
    // mover exists every page's epoch is 0 and the comparison's inputs are
    // constant.
    std::uint32_t page_epoch = 0;

    std::uint16_t slot = 0;
    std::uint16_t flags = 0;
    std::uint32_t reserved = 0;

    bool hint_valid() const noexcept {
        return (flags & kCabinHintValid) != 0 && page_id != kInvalidPageId;
    }
};

static_assert(sizeof(CabinEntry) == 24, "C6 fixes the entry at 24 bytes");

// The identity of one entry set: a Cabin, and one value of its key column.
//
// The value is held **by value**, not hashed to an integer. A hash would
// make two distinct values share a set, and a Cabin's set is authoritative -
// so where Waystone's `arg_hash` can afford a collision (it costs a replay),
// a collision here would serve one value's rows for another's. Cheap to say
// and expensive to get wrong, so it is said in the type.
struct CabinKey {
    std::uint64_t cabin_id = 0;
    parser::ValueType type = parser::ValueType::kNull;
    std::int64_t int_val = 0;
    std::string str_val;

    bool operator==(const CabinKey&) const noexcept = default;
};

struct CabinKeyHash {
    std::size_t operator()(const CabinKey& key) const noexcept;
};

// The value half of a key, `cabin_id` left 0 - the Cabin's value identity
// for anything that needs it without being a Cabin (the statement-local
// inner build, workplan JB2, is the first). A zero cabin_id is not a cabin
// and MakeCabinKey below refuses it, so a value-only key handed to a
// CabinStore by mistake can never match a stored entry set: it fails
// closed, as a miss, instead of serving an authoritative set it does not
// name.
//
// Two kinds are refused, for one reason each. **kNull**: `WHERE c = NULL` is
// not an equality any SQL evaluates to true, so a set keyed on NULL would
// answer a predicate that never matches - and NULLs are not storable today
// regardless. **kParam**: a declared pattern's `$x` is a value *position*
// with no value in it, and nothing ever binds one on an execute path.
std::optional<CabinKey> MakeValueKey(const parser::AstValue& value);

// The key for `value` in `cabin_id`, or nullopt for a zero cabin_id or a
// value MakeValueKey refuses - the refusal reasons are its.
std::optional<CabinKey> MakeCabinKey(std::uint64_t cabin_id, const parser::AstValue& value);

// §8's budget is `[OPEN]` - per-cabin page budget, per-value set caps,
// demotion of write-hot values - so the numbers live here, are `[PROPOSED]`,
// and are configurable (`cabin_max_values`, `cabin_max_entries_per_value`).
// Nothing may depend on either value; what the code depends on is rule 2
// above, which holds whatever the numbers are.
struct CabinLimits {
    std::size_t max_values = 4096;
    std::size_t max_entries_per_value = 4096;
};

class CabinStore {
public:
    // Sightings held before the table is cleared. `TrailRecorder`'s policy
    // verbatim, including the crudest-possible eviction: overflow clears the
    // whole table, which merely restarts counting - a value loses a sighting
    // and needs one more execution to be observed. A performance event.
    static constexpr std::size_t kMaxSightings = 4096;

    // Executions of a value before its set is recorded.
    //
    // `n = 2` for a Cabin the engine created: the policy `docs/spec/parser-v2.md`
    // J5 settled for trails, reused rather than invented a second time. The
    // first miss only counts, the second records. Two is the smallest n that
    // excludes the one-shot query, and a Cabin's per-value cost - a
    // directory probe on every write to the relation - is exactly what a
    // one-shot value should not buy.
    //
    // `n = 1` for a **declared** Cabin - `CREATE CABIN`, or a `CABIN` clause
    // on the column at CREATE TABLE. Same rule TrailRecorder applies to a
    // declared pattern, same argument: the declaration *is* the evidence
    // n=2 waits for, and an operator who wrote `CABIN` on a column has
    // already said it is probed by value.
    static constexpr std::uint8_t kAutoRecordThreshold = 2;
    static constexpr std::uint8_t kDeclaredRecordThreshold = 1;

    struct Stats {
        std::uint64_t hits = 0;             // probes served from an observed set
        std::uint64_t misses = 0;           // probes that fell through to a scan
        std::uint64_t recordings = 0;       // values that became observed
        std::uint64_t appends = 0;          // write-hook appends performed
        std::uint64_t unobserved = 0;       // values dropped, for any reason
        std::uint64_t cap_refusals = 0;     // values not observed, at a cap
        std::uint64_t sighting_clears = 0;  // sighting table cleared wholesale
        // Walks that would have recorded and were declined because the
        // view could still be contradicted (§6a). Nonzero is normal on a
        // transactional workload and is the number that says *why* a Cabin
        // is not filling, which no other counter can distinguish from
        // "nobody probed it".
        std::uint64_t unbankable_views = 0;
    };

    // What one Cabin holds and how it has been doing. Per cabin rather than
    // global so `SHOW CABINS` can answer "is this Cabin earning its write
    // hook" - which is the question §8's demotion policy will need, and
    // which a global counter cannot answer for one Cabin among several.
    struct CabinInfo {
        std::size_t values = 0;
        std::size_t entries = 0;
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t recordings = 0;
        std::uint64_t appends = 0;
    };

    explicit CabinStore(CabinLimits limits = CabinLimits()) noexcept : limits_(limits) {}

    // ---- Read path ------------------------------------------------------

    // The entry set for `key`, or **nullptr when the value is not
    // observed**. Those two are entirely different answers: a non-null
    // empty set is an authoritative "no rows", and nullptr means "scan, this
    // Cabin knows nothing about that value".
    //
    // Non-const, and returning a mutable set, because the reader heals a
    // stale hint in place (C6). The store is core-local, so there is no
    // aliasing question - the caller is the only thing running.
    std::vector<CabinEntry>* Find(const CabinKey& key);

    // Counts a probe that was served / that fell through. Split from Find()
    // so a caller may look without being counted (tests, inspection), and so
    // the miss is counted once at the decision point rather than once per
    // fallback path.
    void NoteHit(std::uint64_t cabin_id);
    void NoteMiss(std::uint64_t cabin_id);

    // Counts a C6 location hint's verification outcome, per cabin. The
    // callers are the two hint sites that already bump the per-step
    // counters (step_vm's ServeFromCabin, fk_check's reverse fast path) -
    // this is the per-*cabin* attribution those per-step counters cannot
    // carry, and the cabin optimizer's S3 needs (workplan PHY01).
    void NoteHint(std::uint64_t cabin_id, bool ok);

    // Where the optimizer's decayed quality signals live, when a collector
    // exists (physical-optimizer.md §II.2 S3). Null is the ordinary
    // no-optimizer configuration and costs one predicate per note. The
    // store forwards and never owns: eviction, snapshotting and caps are
    // the collector's policy, not this class's.
    void set_signals(OptimizerSignals* signals) noexcept { signals_ = signals; }

    // ---- Recording ------------------------------------------------------

    // Bumps `key`'s sighting count and returns the new value, clearing the
    // table wholesale if it is full. Saturates at 255 rather than wrapping:
    // a wrapped count would send a hot value back below the threshold.
    std::uint8_t Observe(const CabinKey& key);

    // Whether a value with this many sightings should record now. The policy
    // lives in one place and this is it.
    //
    // `declared` is "**the declaration speaks for this probe**", not "the
    // Cabin is declared" - the two parted at CB14 (cabin.md §4a): a
    // correlated probe's key is a value no operator named, so it passes
    // false however the Cabin was created. The caller owns that test
    // because only the caller knows the probe's shape.
    bool WouldRecord(std::uint8_t sightings, bool declared) const noexcept {
        return sightings >= (declared ? kDeclaredRecordThreshold : kAutoRecordThreshold);
    }

    // Whether Commit could possibly accept a set for `key` today - the
    // per-cabin value cap, asked **before** a recording walk is paid for.
    // Without this gate a cap-refused value re-armed on every probe, and
    // under CB13's completion license each doomed attempt was a full
    // relation walk (the bug cabin.md §4a's cap paragraph records).
    // The entry cap cannot be asked here - a set's size is only known
    // mid-walk - so the recording path bounds that one itself. A false is
    // never an error: the value simply stays unobserved (rule 2).
    bool MayObserve(const CabinKey& key) const {
        if (observed_.contains(key)) return true;  // a heal replaces in place
        if (entry_capped_.contains(key)) return false;
        auto info = info_.find(key.cabin_id);
        return info == info_.end() || info->second.values < limits_.max_values;
    }

    // Counts a value refused ahead of its walk (MayObserve's value-cap
    // half) - the same `cap_refusals` Commit counts, so `SHOW CABINS`'
    // signal does not go dark because the refusal moved earlier. It is
    // printed there beside `unbankable_views`, the store-wide pair that
    // answers "why is this Cabin not filling".
    void NoteCapRefusal() { ++stats_.cap_refusals; }

    // A recording walk declined because its view is not one a set may be
    // banked from: an in-flight transaction can still make a row it could
    // not see live, and its own transaction's uncommitted DELETE can be
    // rolled back under it (§6a, and the header's authority rule above).
    void NoteUnbankableView() { ++stats_.unbankable_views; }

    // A key whose set outgrew the per-value entry cap mid-recording. The
    // mark is **sticky**: sets only grow under append-only maintenance, so
    // re-attempting on the next probe was doomed work re-armed forever -
    // the completion-license bug cabin.md §4a's cap paragraph records.
    // What clears it: `Unobserve` (the heal path - the world provably
    // changed) and the sighting table's wholesale reset (the store's one
    // crude eviction). An UPDATE moving rows *away* from the value can
    // shrink a future set below the cap with no hook firing for the old
    // value - the mark outliving that is a performance conservatism §8's
    // open budget policy owns, never a wrong answer.
    void NoteEntryCapRefusal(const CabinKey& key) {
        entry_capped_.insert(key);
        sightings_.erase(key);
        ++stats_.cap_refusals;
    }

    // The per-value entry cap, for the recording path's mid-walk bound.
    std::size_t max_entries_per_value() const noexcept {
        return limits_.max_entries_per_value;
    }

    // Marks `key` observed with `entries` as its set. Returns false when the
    // value was **not** observed - a cap was in the way - which is a
    // complete answer and never an error.
    //
    // The caller must only reach here from a walk that **completed**: a
    // partial walk's matches are not a superset of anything, and a set
    // recorded from one would be missing exactly the rows the walk did not
    // reach. Not checkable from inside this class, which is why it is
    // stated at both ends (see step_vm.cpp's recording branch).
    bool Commit(const CabinKey& key, std::vector<CabinEntry> entries);

    // Drops a value's set and its observed mark, returning queries for it to
    // the authoritative scan path. Always legal, by §1's corollary - which
    // is what makes it the right answer to a failed append, a cap, or any
    // other surprise.
    void Unobserve(const CabinKey& key);

    // Drops everything belonging to one Cabin. `DROP CABIN`'s runtime half.
    //
    // Late is fine: the compiler stops emitting cabin probes the moment the
    // catalog row is gone, so a set nobody can reach costs memory and never
    // an answer.
    void Forget(std::uint64_t cabin_id);

    // ---- Write path -----------------------------------------------------

    // The witness (§5). Appends `entry` to `key`'s set **if that value is
    // observed**, and does nothing at all if it is not - which is the common
    // case and the whole reason the hook is affordable.
    //
    // At the per-value cap it **un-observes** instead of appending, because
    // the alternative - dropping the append and keeping the mark - is the
    // one outcome that breaks completeness.
    void NoteWrite(const CabinKey& key, const CabinEntry& entry);

    // ---- Inspection -----------------------------------------------------

    const Stats& stats() const noexcept { return stats_; }
    CabinInfo InfoFor(std::uint64_t cabin_id) const;
    std::size_t observed_value_count() const noexcept { return observed_.size(); }

    // The cabin optimizer's two worklists (workplan PHY04), sorted for
    // deterministic builds. `SightedUnobservedOf` is EXTEND's seed - the
    // values traffic has probed that no set answers for yet, exactly
    // §II.5's "the predicate values that generated the evidence" - and
    // `ObservedValuesOf` is HEAL's, the sets whose hints a batch
    // re-validation walks.
    std::vector<CabinKey> SightedUnobservedOf(std::uint64_t cabin_id) const;
    std::vector<CabinKey> ObservedValuesOf(std::uint64_t cabin_id) const;

private:
    // A whole entry set arriving or leaving, so `values` and `entries` are
    // maintained in one place rather than at each of the four sites that can
    // create or drop one. An *append* is not a set and does not go through
    // here - it moves `entries` alone.
    void AddSet(std::uint64_t cabin_id, std::size_t entries);
    void RemoveSet(std::uint64_t cabin_id, std::size_t entries);

    CabinLimits limits_;

    std::unordered_map<CabinKey, std::vector<CabinEntry>, CabinKeyHash> observed_;
    std::unordered_map<CabinKey, std::uint8_t, CabinKeyHash> sightings_;
    // Keys past the per-value entry cap - see NoteEntryCapRefusal.
    std::unordered_set<CabinKey, CabinKeyHash> entry_capped_;
    std::unordered_map<std::uint64_t, CabinInfo> info_;
    Stats stats_;
    OptimizerSignals* signals_ = nullptr;
};

}  // namespace kds::stats
