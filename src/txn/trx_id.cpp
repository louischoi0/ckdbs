#include "kds/txn/trx_id.hpp"

#include <string>

#include "kds/base/contention.hpp"

namespace kds::txn {

StatusOr<TrxIdRange> TrxIdSequence::Carve(std::uint64_t count) {
    // BA-R0's carve count and longest carve: the latched raise and the
    // persist, which is the whole stall a carve puts on its reactor.
    const StallTimer stall(Tally::kCarves, Longest::kCarveNs);
    auto raised = Raise(count);
    if (!raised.ok()) return raised.status();
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
    return raised;
}

StatusOr<TrxIdRange> TrxIdSequence::Raise(std::uint64_t count) {
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

    // A block the early carve staged and whose sync is not yet known done
    // (DZ-S3): made durable here, on this path, as a carve here always is.
    if (pending_.has_value()) {
        if (!staged_.done(pending_->ticket)) {  // pending only where `persist_` is set
            const StallTimer stall(Tally::kCarves, Longest::kCarveNs);
            if (Status s = persist_(); !s.ok()) return s;
        } else if (Status s = staged_.result(pending_->ticket); !s.ok()) {
            pending_.reset();
            return s;
        }
        ahead_ = pending_->block;
        pending_.reset();
    }
    // The block carved ahead, if a tick got there first (BA-S13): durable
    // already, so installing it is the whole of the work.
    if (ahead_.has_value()) {
        InstallWindow(*ahead_);
        ahead_.reset();
        return Status::OK();
    }
    auto carved = Carve(kTrxIdBlockSize);
    if (!carved.ok()) return carved.status();
    InstallWindow(carved.value());
    return Status::OK();
}

Status TrxIdSequence::BurnWindow() {
    // `ReserveBlock()` unconditional: it is already the one place a window
    // is replaced, and a carved block is already durable before it is
    // returned. Burning is calling it while the current window still has
    // room, which is the whole of the mechanism - with any block carved
    // ahead dropped first, so the burn carves above every id this core has.
    ahead_.reset();
    pending_.reset();  // its ids burned with the block ahead's
    return ReserveBlock();
}

Status TrxIdSequence::CarveAheadIfLow() {
    if (pending_.has_value()) {
        // The staged block's sync (DZ-S3): ahead once done, dropped on a
        // failure - its ids burned - and asked again at the next tick.
        if (!staged_.done(pending_->ticket)) return Status::OK();
        const Status synced = staged_.result(pending_->ticket);
        if (synced.ok()) ahead_ = pending_->block;
        pending_.reset();
        return synced;
    }
    if (ahead_.has_value() || remaining() > kTrxIdBlockSize / 4) return Status::OK();
    if (!staged_.stage || persist_ == nullptr) {
        auto carved = Carve(kTrxIdBlockSize);
        if (!carved.ok()) return carved.status();
        ahead_ = carved.value();
        return Status::OK();
    }
    // The raise and the page write on this reactor, the sync on the
    // data-sync thread: what `Carve` waits for here is no longer waited for.
    const StallTimer stall(Tally::kCarves, Longest::kCarveNs);
    auto raised = Raise(kTrxIdBlockSize);
    if (!raised.ok()) return raised.status();
    auto ticket = staged_.stage();
    if (!ticket.ok()) return ticket.status();  // the raise stays: a burn, the safe direction
    pending_ = Pending{raised.value(), ticket.value()};
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
