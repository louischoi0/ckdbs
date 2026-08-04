#include "kds/txn/trx_id.hpp"

#include <string>

namespace kds::txn {

Status TrxIdSequence::ReserveBlock() {
    if (next_ > kMaxTrxId) {
        return Status::OutOfRange("transaction id space exhausted at " + std::to_string(next_) +
                                  "; ids are never wrapped");
    }

    // Clamped at the top of the space rather than allowed to overflow past
    // it: the last block is whatever is left, and it is still a block.
    std::uint64_t ceiling = next_ + kTrxIdBlockSize;
    if (ceiling > kMaxTrxId + 1 || ceiling < next_) {
        ceiling = kMaxTrxId + 1;
    }

    if (Status s = superblock_.SetNextTrxId(ceiling); !s.ok()) return s;
    if (persist_ != nullptr) {
        if (Status s = persist_(); !s.ok()) return s;
    }
    ceiling_ = ceiling;
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
