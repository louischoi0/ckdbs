#include "kds/txn/trx_id.hpp"

#include <string>

namespace kds::txn {

StatusOr<TrxIdRange> TrxIdSequence::Carve(std::uint64_t count) {
    if (count == 0) {
        // Refused rather than answered with a zero-width range, which
        // `InstallWindow` would turn into a window `Next()` issues straight
        // past.
        return Status::InvalidArgument("a transaction-id block of 0 ids was asked for");
    }

    std::uint64_t first = 0;
    std::uint64_t ceiling = 0;
    {
        // **The read and the raise are one step under the latch** (AT-S10b):
        // every core's sequence carves from this one superblock, and a read
        // outside it would let two cores read one high-water and carve the
        // same block. Taken from the **superblock's** high-water, not from
        // this sequence's `next_`, for the same reason: another core's carve
        // raises it above this core's window, and reserving from `next_`
        // would compute a ceiling *below* the durable one, which
        // `SetNextTrxId` refuses to write.
        LatchGuard hold(superblock_latch_);
        first = superblock_.next_trx_id();
        if (first > kMaxTrxId) {
            return Status::OutOfRange("transaction id space exhausted at " +
                                      std::to_string(first) + "; ids are never wrapped");
        }
        // Clamped at the top of the space rather than allowed to overflow
        // past it: the last block is whatever is left, and it is still a
        // block.
        ceiling = first + count;
        if (ceiling > kMaxTrxId + 1 || ceiling < first) {
            ceiling = kMaxTrxId + 1;
        }
        if (Status s = superblock_.SetNextTrxId(ceiling); !s.ok()) return s;
    }
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
}

Status TrxIdSequence::ReserveBlock() {
    if (next_ > kMaxTrxId) {
        return Status::OutOfRange("transaction id space exhausted at " + std::to_string(next_) +
                                  "; ids are never wrapped");
    }

    auto carved = Carve(kTrxIdBlockSize);
    if (!carved.ok()) return carved.status();
    InstallWindow(carved.value());
    return Status::OK();
}

Status TrxIdSequence::BurnWindow() {
    // `ReserveBlock()` unchanged and unconditional: it is already the one
    // place a window is replaced, and a carved block is already durable
    // before it is returned. Burning is calling it
    // while the current window still has room, which is the whole of the
    // mechanism.
    return ReserveBlock();
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
