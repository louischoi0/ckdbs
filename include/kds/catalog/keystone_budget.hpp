#pragma once

#include <cstdint>

#include "kds/catalog/rows.hpp"
#include "kds/storage/keystone.hpp"

// How much of a relation's lifetime id budget has been spent
// (`docs/rules/keystoneid-invariant.md` K4 and K-M4).
//
// K4 turns 2^40 from a live-row bound into a **lifetime issuance budget per
// relation**, and that is the whole reason this file exists as arithmetic
// rather than as a line of `<<` in the dispatcher. Three things follow from
// it that a caller doing the subtraction inline gets wrong:
//
//   1. **Issued is not a row count.** Every id the sequence hands out is
//      spent - including one burned by a failed insert, and (once K-M2
//      lands) the unissued remainder of a bump-ahead chunk after a crash.
//      A relation with three live rows can have spent thousands. Anything
//      rendering this must say "issued", never "rows".
//   2. **The budget starts at `kFirstRowId`, not at zero.** Id 0 is
//      reserved as "unset" (rows.hpp), so the capacity is one short of
//      2^40 and an off-by-one here is an off-by-one in the warning.
//   3. **Exhaustion is a real state, not the tail of a percentage.**
//      `AllocateRowId` refuses with `OutOfRange` rather than wrapping, so
//      "100% used" and "the next insert fails" are the same event and this
//      reports it as a flag rather than leaving it to be inferred from a
//      rounded fraction.
//
// It reads `next_id` today. K-M2 replaces that with a persisted high-water
// mark, at which point the *source* changes and none of the arithmetic
// does - which is why this takes a bare id rather than a catalog row.

namespace kds::catalog {

// The share of a relation's budget at which consumption becomes a warning.
//
// `[PROPOSED: 90%]` in `docs/rules/keystoneid-invariant.md` K-M4 and still
// proposed: nothing has yet argued for a particular number, and the honest
// input for one is how long a relation takes to cross the remaining 10% at
// its own insert rate (§3's table), which is per-deployment. Named here so
// moving it is one edit rather than a search.
inline constexpr double kKeystoneBudgetWarnFraction = 0.90;

// The lifetime issuance budget of one relation: every id from kFirstRowId
// through kMaxKeystoneId inclusive.
inline constexpr std::uint64_t kKeystoneBudgetCapacity = kMaxKeystoneId - kFirstRowId + 1;

struct KeystoneBudget {
    // Ids handed out over the relation's lifetime, **gaps included**. Not
    // a live-row count and not a high-water mark of live rows: an id spent
    // on a failed insert is spent.
    std::uint64_t issued = 0;

    // Ids the relation can still issue before `AllocateRowId` refuses.
    // Zero means the next insert fails.
    std::uint64_t remaining = 0;

    // kKeystoneBudgetCapacity, carried so a renderer never has to
    // re-derive it and get the kFirstRowId offset wrong.
    std::uint64_t capacity = kKeystoneBudgetCapacity;

    // issued / capacity, in [0, 1].
    double used_fraction = 0.0;

    // used_fraction >= kKeystoneBudgetWarnFraction.
    bool warn = false;

    // The sequence is spent: the next AllocateRowId answers OutOfRange.
    bool exhausted = false;
};

// Budget as of a relation whose next issuable id is `next_id`.
//
// Total, never failing: a `next_id` below kFirstRowId or above
// kMaxKeystoneId is a corrupt or exhausted catalog row rather than a
// caller error, and an inspection surface that refuses to render the one
// relation an operator is looking at is worse than useless. Both clamp.
KeystoneBudget BudgetOf(std::uint64_t next_id) noexcept;

}  // namespace kds::catalog
