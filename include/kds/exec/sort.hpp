#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "kds/exec/chain_frame.hpp"
#include "kds/exec/pagination.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/step_chain.hpp"

// The output sort - OB4's execution half. **The argument lives in
// `docs/workplan-order-by.md`**: why a sink decorator and not a step, why
// an index cannot serve the order instead, and what the sort costs. Read
// it before changing the shape of this class; only the rules a caller of
// these methods has to obey are restated here.
//
// Two of those rules, because getting either wrong is silent:
//
//   - **A sorted statement's `LIMIT` bounds output and memory, not work.**
//     The quota runs downstream of the sort, because rows [m, m+n) of the
//     *sorted* reply are not rows [m, m+n) of the emitted one - so the walk
//     cannot stop when the quota fills, and the row-touch budget is the
//     only thing that bounds it. `pagination.hpp`'s division, one clause
//     longer: the quota bounds output, this bounds memory and formatting,
//     the budget bounds work.
//   - **Nothing may be retained by reference from the frame.** Its value
//     buffer is reused per row.

namespace kds::exec {

// The default `sort_max_rows` (`[PROPOSED]`, docs/workplan-order-by.md).
inline constexpr std::size_t kDefaultSortMaxRows = 1048576;

class OutputSort {
public:
    // One buffered row: the normalized keys it is ordered by, its arrival
    // number, and the reply text the client will see.
    struct Row {
        std::vector<OrderKey> keys;
        std::uint64_t seq = 0;
        std::string text;
    };

    // Arms the sort for one statement. `chain.sort_keys` empty means the
    // statement needs none - `active()` is then false and every other
    // method is a no-op, which is what lets the caller wire this in
    // unconditionally.
    //
    // Buffers keep their capacity across a reset, the reason
    // `Aggregator::Reset` does: a hoisted sorter allocates on its first
    // statement and reuses from then on.
    void Reset(const StepChain& chain, std::size_t max_rows);

    bool active() const noexcept { return !keys_.empty(); }

    // Offers one qualifying row, in emission order, and answers whether it
    // can still reach the client. Normalizes and copies the row's keys -
    // the frame's value buffer is reused per row, so nothing may be
    // retained by reference.
    //
    // **False means do not render it.** Under a `LIMIT`, most rows of a
    // large relation are beaten by the heap's worst retained row and will
    // never be seen, and formatting them was pure waste: before this split,
    // `ORDER BY val LIMIT 20` over 10,000 rows spent *more* server CPU than
    // returning the whole relation unsorted, because it rendered 9,980 rows
    // and threw them away. Deciding first is worth 94-104 ns a skipped row
    // and takes that statement to 0.60× a full scan's CPU
    // (`bench/results-order-by.md`).
    //
    // **What this bounds is formatting, not work.** The walk still decodes
    // every qualifying row - 83% of what that statement now costs - because
    // a sorted `LIMIT` cannot stop early, which is the quota's whole reason
    // for moving downstream. Memory is bounded by the heap, formatting by
    // this call, and work by the row-touch budget and nothing else.
    //
    // Split into two calls rather than a predicate the caller could forget:
    // `Take` completes exactly the row the immediately preceding `Admit`
    // accepted, and reuses the keys it already normalized.
    StatusOr<bool> Admit(const ChainFrame& frame);

    // Completes the row the preceding `Admit` returned true for. `row` is
    // its rendered reply text - the caller renders, because it owns the
    // projection and the schema. Taken by reference and left holding
    // whatever buffer this row displaced, so the caller's scratch is
    // recycled rather than reallocated.
    void Take(std::string& row);

    // Orders what was kept, in place. Call once, after the walk.
    void Finish();

    // The rows in the order the client sees them, valid until the next
    // `Reset` and meaningful only after `Finish`. Its `size()` is what
    // ANALYZE reports as `sorted=`: under a `LIMIT` that is the retained
    // count and not the count that arrived, because what was retained is
    // what the sort cost in memory.
    const std::vector<Row>& rows() const noexcept { return buffer_; }

private:
    // Whether `a` sorts strictly before `b` under the statement's keys.
    //
    // `seq` - arrival order - is the last key and always ascending, which
    // does two jobs with one field. It makes the order **total**, so
    // `std::sort` and the heap below it are safe without the caller having
    // to prove the client's keys are unique. And it makes ties resolve to
    // the order the chain emitted them in, so `ORDER BY` *refines* I12's
    // emission contract rather than replacing it: rows the clause does not
    // distinguish come back in the order they would have without it.
    bool Before(const Row& a, const Row& b) const noexcept;

    // Whether the keys `Admit` just normalized sort before `b`. The pending
    // row's `seq` would be `next_seq_`, above every buffered row's, so a
    // row tying on every written key loses to the one already held - which
    // is the stability rule falling out of the same field that gives it.
    bool PendingBefore(const Row& b) const noexcept;

    // The `sort_max_rows` refusal.
    Status CapExceeded() const;

    std::vector<SortKey> keys_;
    std::vector<Row> buffer_;

    // The keys of the row between `Admit` and `Take`. A member so the
    // normalization survives the caller's render without being redone, and
    // so its capacity is reused by the next row.
    std::vector<OrderKey> pending_;
    std::uint64_t next_seq_ = 0;

    // How many rows can still matter, and whether that number is a bound
    // the buffer may discard against. `bounded_` is true only for a
    // `LIMIT` whose `offset + limit` the cap covers - then `retain_` is
    // that target and the buffer is a max-heap of it. Otherwise `retain_`
    // is `max_rows` and nothing is discarded: an unlimited sort, and a
    // limited one asking for more than the cap can hold, both run to the
    // cap and refuse there rather than quietly returning a prefix.
    std::size_t retain_ = 0;
    bool bounded_ = false;
    std::size_t max_rows_ = 0;
};

// Runs `quota` over `sorted`, calling `emit` for each row that survives.
//
// One function because there are two callers and their whole contract is
// that they agree: ANALYZE's `rows=` must be what the real path would have
// sent, and two copies of a four-verdict loop is exactly how that stops
// being true.
template <typename Emit>
void DrainSorted(EmissionQuota& quota, const std::vector<OutputSort::Row>& sorted, Emit emit) {
    for (const OutputSort::Row& row : sorted) {
        const QuotaVerdict verdict = quota.Note();
        if (verdict == QuotaVerdict::kStop) return;
        if (verdict == QuotaVerdict::kSkip) continue;
        emit(row);
        if (verdict == QuotaVerdict::kEmitThenStop) return;
    }
}

}  // namespace kds::exec
