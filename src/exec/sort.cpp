#include "kds/exec/sort.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace kds::exec {

// ---- The cap: it refuses, it never truncates ----------------------------
//
// A truncated sort is the worst kind of wrong answer - it has a right
// answer's shape, and the rows it dropped are exactly the ones the client
// asked to see last. There is no fallback to decline to, the way a Cabin
// has one, so this fails the statement and names the key an operator would
// raise. Out of line because the message is longer than the branch that
// raises it, not because two branches share it.
Status OutputSort::CapExceeded() const {
    return Status::ResourceExhausted(
        "this statement's ORDER BY would hold more than sort_max_rows (" +
        std::to_string(max_rows_) +
        ") rows; no partial ordering is emitted, because a truncated sort drops exactly the "
        "rows the statement asked to see last - raise sort_max_rows, or add a LIMIT, which "
        "bounds what is held at offset + limit");
}

void OutputSort::Reset(const StepChain& chain, std::size_t max_rows) {
    keys_ = chain.sort_keys;
    buffer_.clear();
    next_seq_ = 0;
    max_rows_ = max_rows;

    // OB5's retention target. Only the first `offset + limit` rows of the
    // sorted order can reach the client, so nothing past that is worth
    // holding - and the addition saturates rather than wrapping, because
    // both halves are client-supplied uint64s and a wrap would turn a huge
    // OFFSET into a tiny buffer that silently drops rows.
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t want =
        !chain.limit.has_value() ? kMax
        : chain.offset > kMax - chain.limit.value()
            ? kMax
            : chain.offset + chain.limit.value();

    // **The bound is only a bound when the cap can cover it.** Discarding a
    // row because `retain_` rows already beat it is sound exactly when
    // `retain_` is the number the client could see; clamping the target down
    // to the cap and discarding against *that* answers with the first
    // `sort_max_rows` rows of the order and no error, which is the
    // truncation the cap exists to prevent. So a `LIMIT` the cap cannot
    // cover is not a bound at all - the sort behaves as the unlimited case
    // and refuses if that many rows arrive. A `LIMIT` larger than the cap
    // is still never *preemptively* refused: a statement that returns ten
    // rows succeeds however large its limit was written.
    bounded_ = want <= static_cast<std::uint64_t>(max_rows_);
    retain_ = bounded_ ? static_cast<std::size_t>(want) : max_rows_;
}

bool OutputSort::Before(const Row& a, const Row& b) const noexcept {
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        const int cmp = a.keys[i].Compare(b.keys[i]);
        if (cmp != 0) return keys_[i].descending ? cmp > 0 : cmp < 0;
    }
    return a.seq < b.seq;
}

bool OutputSort::PendingBefore(const Row& b) const noexcept {
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        const int cmp = pending_[i].Compare(b.keys[i]);
        if (cmp != 0) return keys_[i].descending ? cmp > 0 : cmp < 0;
    }
    return false;  // ties lose to the row already held: its seq is lower
}

StatusOr<bool> OutputSort::Admit(const ChainFrame& frame) {
    pending_.clear();
    for (const SortKey& key : keys_) {
        auto normalized = OrderKeyOf(key.type_val, frame.Get(key.ref));
        if (!normalized.ok()) return normalized.status();
        pending_.push_back(std::move(normalized.value()));
    }

    // ---- OB5: the bounded case, where the buffer is a max-heap ----------
    //
    // `buffer_.front()` is the worst row still retained. A row that does not
    // beat it can never reach the client, so it is refused here - before the
    // caller renders it, which is where the cost is.
    if (bounded_ && buffer_.size() >= retain_) {
        // `retain_ == 0` here means `want == 0`, which is `LIMIT 0` with no
        // OFFSET: a legal statement that emits no rows and therefore needs
        // none held. It is the only shape that reaches this while bounded,
        // because any larger target is a bound only when the cap covers it.
        // A `sort_max_rows` of zero against a statement that does need a row
        // held is refused by the cap below, not smuggled through here.
        if (retain_ == 0) return false;
        return PendingBefore(buffer_.front());
    }

    if (buffer_.size() >= max_rows_) return CapExceeded();
    return true;
}

void OutputSort::Take(std::string& row) {
    auto worse = [this](const Row& a, const Row& b) { return Before(a, b); };
    if (bounded_ && buffer_.size() >= retain_) {
        // The heap is full and this row beat its worst: evict that one and
        // reuse its Row, which hands back the key vector's capacity too.
        std::pop_heap(buffer_.begin(), buffer_.end(), worse);
        Row& evicted = buffer_.back();
        // Swapped, not moved: the evicted row's key vector and text buffer
        // go back to the caller's scratch, so a long top-N run settles into
        // allocating nothing per row.
        std::swap(evicted.keys, pending_);
        std::swap(evicted.text, row);
        evicted.seq = next_seq_++;
        std::push_heap(buffer_.begin(), buffer_.end(), worse);
        return;
    }
    buffer_.push_back(Row{std::move(pending_), next_seq_++, std::move(row)});
    if (bounded_) std::push_heap(buffer_.begin(), buffer_.end(), worse);
}

void OutputSort::Finish() {
    // `std::sort`, not `std::stable_sort`: `seq` is the last key, so the
    // order is already total and stability is a property of the comparator
    // rather than of the algorithm - which is what lets the bounded path
    // above use a heap, where a stable algorithm would have had nothing to
    // offer it.
    std::sort(buffer_.begin(), buffer_.end(),
              [this](const Row& a, const Row& b) { return Before(a, b); });
}

}  // namespace kds::exec
