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
// (`docs/feat-assertion.md` §5.2, workplan AST04).
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
// against the stored group key** before it is used (`docs/feat-cabin.md`
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

// The canonical encoding of one group key (§5.2, "a canonical encoding").
//
// Tags each value's kind and length-prefixes strings, so `('a','bc')` and
// `('ab','c')` cannot collide — the same rule and the same reason as the
// aggregate fold's key encoding (`feat-aggregate.md`), and NULL is a value
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
// on the same licence (`docs/feat-cabin.md` §9). What makes that *not* a
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
    // the point, `feat-cabin.md` §5's "removal is forbidden" is about the
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

    BoundAggregate aggregate_;
    std::int64_t bound_;
};

}  // namespace kds::exec
