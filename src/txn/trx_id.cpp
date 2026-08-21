#include "kds/txn/trx_id.hpp"

#include <string>

namespace kds::txn {

StatusOr<TrxIdRange> TrxIdSequence::Carve(std::uint64_t count) {
    // Taken from the **superblock's** high-water, not from this sequence's
    // `next_`. On one core the two are equal at every reserve - a sequence
    // only reserves when its window is spent, and a spent window ends
    // exactly where the ceiling it persisted does - so this is
    // behaviour-identical to the pre-PW1 arithmetic. With a second consumer
    // it is the difference between correct and impossible: a peer's block
    // raises the ceiling above core 0's `next_`, and reserving from `next_`
    // would then compute a ceiling *below* the durable one, which
    // `SetNextTrxId` refuses to write.
    const std::uint64_t first = superblock_.next_trx_id();
    if (first > kMaxTrxId) {
        return Status::OutOfRange("transaction id space exhausted at " + std::to_string(first) +
                                  "; ids are never wrapped");
    }
    if (count == 0) {
        // Refused rather than answered with a zero-width range, which
        // `InstallWindow` would turn into a window `Next()` issues straight
        // past. `ExtentAllocator::Reserve` answers the same way to the same
        // question.
        return Status::InvalidArgument("a transaction-id block of 0 ids was asked for");
    }

    // Clamped at the top of the space rather than allowed to overflow past
    // it: the last block is whatever is left, and it is still a block.
    std::uint64_t ceiling = first + count;
    if (ceiling > kMaxTrxId + 1 || ceiling < first) {
        ceiling = kMaxTrxId + 1;
    }

    if (Status s = superblock_.SetNextTrxId(ceiling); !s.ok()) return s;
    if (persist_ != nullptr) {
        // **Raised in memory first, then made durable, and not rolled back
        // on failure.** The asymmetry is deliberate and it only errs one
        // way: a lost persist leaves the in-memory ceiling *above* the
        // durable one, so the next carve starts higher and burns the
        // difference. Ids are never gapless and a burned one costs nothing.
        // The reverse - lowering the ceiling after a failed write - is what
        // would reissue an id, and invariant 12 has no room for that.
        if (Status s = persist_(); !s.ok()) return s;
    }
    return TrxIdRange{first, ceiling - first};
}

void TrxIdSequence::InstallWindow(TrxIdRange window) noexcept {
    next_ = window.first;
    ceiling_ = window.first + window.count;
    window_ = window.count;
}

Status TrxIdSequence::ReserveBlock() {
    if (next_ > kMaxTrxId) {
        return Status::OutOfRange("transaction id space exhausted at " + std::to_string(next_) +
                                  "; ids are never wrapped");
    }

    if (lease_ != nullptr) {
        auto granted = lease_->Take();
        if (!granted.ok()) return granted.status();
        // A grant below what this core has already issued would reissue an
        // id - invariant 12's one unforgivable failure. It cannot happen
        // while every block comes from one monotonic `Carve()`, which is
        // exactly why it is worth saying out loud rather than assuming:
        // this is the same check `CoreRuntime::Open` makes against a
        // recovered stream, one layer down.
        if (granted.value().first < next_) {
            return Status::Corruption(
                "transaction-id grant starts at " + std::to_string(granted.value().first) +
                ", below the " + std::to_string(next_) +
                " this core would issue next; a block was carved out of order");
        }
        InstallWindow(granted.value());
        return Status::OK();
    }

    auto carved = Carve(kTrxIdBlockSize);
    if (!carved.ok()) return carved.status();
    InstallWindow(carved.value());
    return Status::OK();
}

StatusOr<std::uint64_t> TrxIdSequence::Next() {
    if (next_ >= ceiling_) {
        if (Status s = ReserveBlock(); !s.ok()) return s;
    }
    if (next_ > kMaxTrxId) {
        return Status::OutOfRange("transaction id space exhausted at " + std::to_string(next_) +
                                  "; ids are never wrapped");
    }
    return next_++;
}

}  // namespace kds::txn
