#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/cabin_bound_page.hpp"

// The Bound Cabin's group directory and running aggregates
// (`docs/spec/assertion.md` §5.2, workplan AST04).
//
// ---- What an admission check costs, and why -------------------------------
//
// **O(1), reading one group header** (§5.2). That is the whole reason the
// predicate class is restricted the way AS1 restricts it: a group cardinality
// or a group sum is a number that can be *maintained*, so a write asks "would
// this delta take the group past its bound?" and never re-evaluates anything.
// Entries exist for violation diagnostics, for re-summation against the
// header, and for the abort path - they are not on the check path.
//
// ---- Collisions are isolated by the key, never by the hash -----------------
//
// The directory is keyed by hash, and a header found by hash is **confirmed
// against the stored group key** before it is used (`docs/spec/cabin.md`
// §12.3). Trusting the hash alone would merge two groups, which for an
// authoritative structure is a wrong answer rather than a slow one - and the
// observational Cabin holds its key by value for exactly this reason (§3).
//
// ---- Checked arithmetic, and what an overflow means ------------------------
//
// Every aggregate mutation is checked int64 (AG3). An overflow **fails the
// statement** and never wraps: a wrapped sum is a number that satisfies a
// bound it does not satisfy, which is the one failure an admission check
// cannot be allowed to produce.
//
// ---- Scope of this file ----------------------------------------------------
//
// The directory and the aggregates, over `storage::cabin::BoundCabinPage`
// entries. Deliberately **not** here: the WAL records that make it
// crash-consistent (AST05), the CREATE-time builder and its cutover (AST06),
// and the write-path reservation protocol that calls `Admit` (AST07). What
// this ships is the structure those three need, with the arithmetic and the
// collision rule already right.
//
// Concurrency: core-local. An assertion is single-relation (AS8) and a
// relation's state belongs to its home core, so the whole protocol is one
// core's (§6.1). No latches, no atomics, no CAS loops.

namespace kds::exec {

// The two predicate shapes AS1 admits. Not `parser::AggFunc`, which has five
// members: a Bound Cabin maintains exactly these two, and a type that could
// hold `kMin` would be a type with three unreachable branches in every switch.
enum class BoundAggregate : std::uint8_t { kCount, kSum };

// One group's running aggregate — §5.2's group header.
struct GroupHeader {
    // The group key as encoded, held **by value**. This is what isolates a
    // hash collision: a header is found by hash and then confirmed against
    // this, never trusted on the hash alone.
    std::string key;

    // **AS6a's id**: dense per cabin, assigned at group creation, never reused
    // while the cabin lives. It is what an entry carries
    // (`storage/cabin_bound_page.hpp`) and what the checkpoint snapshot is keyed
    // by, so the header->entry linkage can be rebuilt by scanning the cabin's
    // own pages instead of being persisted at O(all entries) per checkpoint.
    //
    // Numbered from 1, leaving 0 as "no group" - which is also what every entry
    // written before AS6a reads back as, since those bytes were padding.
    std::uint32_t group_id = 0;

    // Committed **plus reserved** cardinality and sum. Reservations count
    // from the moment of admission, which is what makes a false admission
    // impossible (§6.2) at the price of bounded false rejections.
    std::int64_t count = 0;
    std::int64_t sum = 0;

    // Where this group's entries live, as (page_id, index) pairs. The
    // "entry-list linkage into headered Bound Cabin pages" of §5.2.
    std::vector<std::pair<PageId, std::uint16_t>> entries;

    // The aggregate an admission check compares against the bound.
    std::int64_t aggregate(BoundAggregate agg) const noexcept {
        return agg == BoundAggregate::kCount ? count : sum;
    }
};

// What one row's SUM column contributes to its group.
//
// **One rule, both callers**: the write path's admission
// (`exec/assertion_check.cpp`) and CREATE ASSERTION's backfill
// (`exec/assertion_build.cpp`) have to agree, or a cabin is built from a
// total the enforcement would never have produced - and the two would then
// differ only on the rows nobody looked at.
//
// A NULL contributes nothing (`docs/spec/null.md` §4). Stated here rather
// than inherited from the decoder zeroing `int_val`, because a slot reused
// across rows can carry a stale one and the answer must still be 0.
inline std::int64_t SumContribution(const parser::AstValue& value) noexcept {
    return value.type == parser::ValueType::kNull ? 0 : value.int_val;
}

// The canonical encoding of one group key (§5.2, "a canonical encoding").
//
// Tags each value's kind and length-prefixes strings, so `('a','bc')` and
// `('ab','c')` cannot collide — the same rule and the same reason as the
// aggregate fold's key encoding (`aggregate.md`), and NULL is a value
// here rather than an absence, so two NULL keys are one group.
std::string EncodeGroupKey(const std::vector<parser::AstValue>& values);

// A 64-bit hash of an encoded key. Collisions are *expected* to be possible
// and are handled by confirming the key, so this is a mixing function and not
// a cryptographic one.
std::uint64_t HashGroupKey(const std::string& encoded) noexcept;

// What an admission check answered, and why.
struct AdmissionResult {
    bool admitted = false;

    // Set when `admitted` is false: the aggregate the write would have
    // produced, for the error message §4.4 specifies. Not the current value —
    // the client wants to know what they asked for, not what is there.
    std::int64_t would_be = 0;
};

// One assertion's Bound Cabin: the group directory over its entry pages.
//
// **Memory-resident in v1**, exactly as the observational Cabin's sets are and
// on the same licence (`docs/spec/cabin.md` §9). What makes that *not* a
// contradiction of AS6's "logged, headered authority class" is sequencing:
// the entries are already on durable, checksummed `kCabinBound` pages, and
// AST05's WAL records are what let the directory be rebuilt exactly at
// restart. Until AST05 lands, a restart loses the directory — which is why
// AST06's publish step, not this class, is what turns enforcement on.
class BoundCabin {
public:
    BoundCabin(BoundAggregate aggregate, std::int64_t bound) noexcept
        : aggregate_(aggregate), bound_(bound) {}

    BoundAggregate aggregate() const noexcept { return aggregate_; }

    // The **enforced ceiling**, already reduced from the declared operator by
    // the parser (`AssertionStmt::enforced_max`). This class never sees `<`
    // versus `<=`, which is the point of reducing it once.
    std::int64_t bound() const noexcept { return bound_; }

    std::size_t group_count() const noexcept;

    // The header for `key`, or nullptr. Confirms the stored key, so a
    // colliding hash answers nullptr rather than someone else's group.
    const GroupHeader* Find(const std::string& key) const;

    // AS6a: the id of `key`'s group, creating the group empty if it is new.
    //
    // Called **before** the entry is appended, because the entry has to carry
    // the id (`cabin_bound_page.hpp`) and the page write happens before
    // `Apply`. Creating the header early is safe and is not a state a check can
    // see wrongly: a group at (0, 0) admits exactly what no group admits, and
    // the very next step is the `Apply` that populates it.
    //
    // Not used by the departure path, which requires an existing group and
    // reads `Find(key)->group_id` - a departure that created a group would be
    // creating one to immediately go negative in.
    std::uint32_t EnsureGroupId(const std::string& key);

    // AS6a: bind `key`'s group to the id a replayed record carries, creating
    // the group if the fold has not met it yet.
    //
    // **Why replay cannot just let `Apply` create the group.** `Apply` assigns
    // the next dense id, which is the live run's allocation order - and a fold
    // starting from a checkpoint sees groups in record order, not creation
    // order. The ids would drift, and the *next* recovery's linkage rebuild
    // would attribute entries to the wrong groups. So the record's id is
    // authoritative and this is where it is adopted.
    //
    // Corruption when an existing group already carries a different id: the
    // record and the directory then disagree about which entries are that
    // group's, and there is no safe way to pick.
    Status AdoptGroupId(const std::string& key, std::uint32_t group_id);

    // Restores one group's header from a checkpoint snapshot (AS6a, RC07):
    // the id, the key, and the aggregates as of the snapshot, with an empty
    // entry list that the cabin-page scan then fills.
    //
    // Fails with InvalidArgument on a duplicate id or key, because a snapshot
    // that names one group twice is not a base to fold onto.
    Status RestoreGroup(std::uint32_t group_id, const std::string& key, std::int64_t count,
                        std::int64_t sum);

    // Attaches one scanned entry to the group its `group_id` names (AS6a's
    // linkage rebuild). NotFound when no restored group carries that id -
    // which is a real condition rather than a defect: an entry whose group is
    // not in the snapshot belongs to a group created after it, and the
    // ASSERT_* fold from the checkpoint forward is what creates that group.
    //
    // **Prefer `AttachEntries` for a whole cabin.** This resolves the id by
    // walking every bucket, so one call per scanned entry is O(entries × groups)
    // - ~2.5e8 comparisons for a 1000-page cabin with 1000 groups, on a mount
    // already measured at ~90 ms.
    Status AttachEntry(std::uint32_t group_id, PageId page_id, std::uint16_t index);

    // One scanned entry, as the cabin-page walk reads it.
    struct ScannedEntry {
        std::uint32_t group_id = 0;
        PageId page_id = kInvalidPageId;
        std::uint16_t index = 0;
    };

    // The batch form, and the one a rebuild should use: it builds the
    // id -> header index **once** and then attaches in one pass, so the cost is
    // O(entries + groups) rather than their product.
    //
    // The map is scratch, local to the call and gone when it returns. That is
    // deliberately not a member: the objection to a persistent id index (a second
    // container to keep true across every mutation) is a good one and does not
    // apply to one built inside a single walk.
    //
    // Returns how many entries were attached. An entry whose group no restored
    // header carries is **skipped, not an error**, for `AttachEntry`'s reason -
    // it belongs to a group the fold has yet to create.
    StatusOr<std::uint64_t> AttachEntries(std::span<const ScannedEntry> entries);

    // Drops a duplicated `(page_id, index)` pair from every group's entry list,
    // and reports how many it dropped.
    //
    // **Why a recovered cabin has any.** The page walk attaches every entry the
    // cabin's pages carry, including one written *after* the checkpoint into a
    // group that existed *at* it; the fold then replays that entry's record and
    // `Apply` appends the same pair again. Both steps are right on their own -
    // the walk cannot know which entries the fold will also see, and the fold
    // must append for a group it creates itself - so the reconciliation belongs
    // here, once, at the end of a rebuild.
    //
    // A slot holds one entry, so a repeated pair is always the duplicate and
    // never two distinct entries. Nothing on the live path calls this: a live
    // directory cannot produce one.
    std::uint64_t DedupeEntryLinkage();

    // The snapshot AS6a asks a checkpoint to write: one record per group,
    // `{group_id, key, count, sum}`, O(groups) and never the entry lists.
    struct GroupSnapshot {
        std::uint32_t group_id = 0;
        std::string key;
        std::int64_t count = 0;
        std::int64_t sum = 0;
    };
    std::vector<GroupSnapshot> SnapshotGroups() const;

    // Would applying `delta` to `key`'s aggregate exceed the bound?
    //
    // Pure: nothing is mutated, so a refusal needs no cleanup — which is
    // §6.2's step 2, and the reason the protocol has no rollback for a failed
    // admission. A group that does not exist yet has aggregate 0.
    //
    // Fails with `OutOfRange` on a checked-arithmetic overflow: the statement
    // error AG3 requires, never a wraparound.
    StatusOr<AdmissionResult> Admit(const std::string& key, std::int64_t delta) const;

    // Applies `delta` to `key`'s aggregate and records `entry` against the
    // group, creating the group if it is new. §6.2's step 3.
    //
    // **Checked**: the same overflow test `Admit` makes, repeated here rather
    // than assumed, because a caller that skipped `Admit` — the CREATE-time
    // builder does, since it validates at the end — must not be able to wrap
    // the aggregate.
    Status Apply(const std::string& key, std::int64_t delta, PageId page_id,
                 std::uint16_t index);

    // Removes an entry's contribution: subtracts `delta` and forgets the
    // entry. §6.2's step 5, the abort path.
    //
    // The group is kept even at zero. A group that reached zero may be
    // written to again, and re-creating a header is not free — but more to
    // the point, `cabin.md` §5's "removal is forbidden" is about the
    // *observational* class's entries and does not govern this: a reserved
    // entry that aborted was never visible to any snapshot, so removing it
    // restores a state, it does not hide one.
    Status Unapply(const std::string& key, std::int64_t delta, PageId page_id,
                   std::uint16_t index);

    // The departure pair (AST07, §4.2): a row leaving its group. A
    // departure *entry* contributes (-1, -delta) where an arrival's is
    // (+1, +delta) — `storage::cabin::kEntryDeparture` is the flag that
    // says which, and re-summation reads it. ApplyDeparture records one;
    // UnapplyDeparture is its abort, restoring what the departure took.
    //
    // A departure's group must exist — the row it removes was incorporated
    // by build or insert — so a missing one answers NotFound rather than
    // creating an empty group to go negative in.
    Status ApplyDeparture(const std::string& key, std::int64_t delta, PageId page_id,
                          std::uint16_t index);
    Status UnapplyDeparture(const std::string& key, std::int64_t delta, PageId page_id,
                            std::uint16_t index);

    // Re-sums the group headers from a caller-supplied entry reader — §7's
    // verification hook, and the reason the aggregate value rides inline in
    // every entry. Answers Corruption naming the first group whose header
    // disagrees with its entries.
    //
    // Reserved entries are counted, because the header counts them (§4.1:
    // committed *and* reserved).
    using EntryReader = std::function<StatusOr<storage::cabin::BoundCabinEntry>(
        PageId page_id, std::uint16_t index)>;
    Status VerifyAgainstEntries(const EntryReader& read) const;

private:
    // Hash -> the groups that hashed there. A vector per bucket, because a
    // collision is possible and the key is what disambiguates: two groups
    // sharing a hash both live here and are told apart by `GroupHeader::key`.
    //
    // **This is the only container**, and deliberately: a parallel vector of
    // `GroupHeader*` for iteration would dangle the moment a bucket
    // reallocated, and iteration is a maintenance path where the extra
    // indirection buys nothing.
    std::unordered_map<std::uint64_t, std::vector<GroupHeader>> groups_by_hash_;

    GroupHeader* FindMutable(const std::string& key);

    // The one place a group is born, so the one place an id is assigned. Two
    // creation sites would be two chances to leave `group_id` at 0, which reads
    // as "no group" and would silently unlink every entry of that group at the
    // next recovery.
    GroupHeader& EnsureGroup(const std::string& key);

    // The group a `group_id` names, for the linkage rebuild. A linear walk of
    // the buckets rather than a second index: it runs once per scanned entry
    // during recovery and never on a check path, and a parallel id->header map
    // would be a second container to keep true (the note above on why there is
    // only one).
    GroupHeader* FindById(std::uint32_t group_id);

    BoundAggregate aggregate_;
    std::int64_t bound_;

    // AS6a's dense per-cabin counter. Ids are never reused while the cabin
    // lives, so this only ever rises; a restore raises it past every id the
    // snapshot carried, which is what keeps a group created after the snapshot
    // from being handed an id the snapshot already used.
    std::uint32_t next_group_id_ = 1;
};

}  // namespace kds::exec
