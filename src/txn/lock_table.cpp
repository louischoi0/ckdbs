#include "kds/txn/lock_table.hpp"

#include <algorithm>
#include <string>

namespace kds::txn {
namespace {

// Splitmix64's finalizer: cheap, and it moves the low bits, which a
// modulo by the partition count reads.
constexpr std::uint64_t Mix(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// A fence is a range- or slice-unit borrow in `S` or `X` (AR2 §2 defines a
// gap lock as either). The counter gates the probe, so it must count
// exactly what the probe matches.
constexpr bool IsFence(const LockKey& key, LockMode mode) noexcept {
    return key.IsFenceUnit() && (mode == LockMode::kShared || mode == LockMode::kExclusive);
}

}  // namespace

const char* LockUnitName(LockUnit unit) noexcept {
    switch (unit) {
        case LockUnit::kRelation:
            return "relation";
        case LockUnit::kRange:
            return "range";
        case LockUnit::kSlice:
            return "slice";
        case LockUnit::kTuple:
            return "tuple";
    }
    return "?";
}

const char* LockModeName(LockMode mode) noexcept {
    switch (mode) {
        case LockMode::kIntentionShared:
            return "IS";
        case LockMode::kIntentionExclusive:
            return "IX";
        case LockMode::kShared:
            return "S";
        case LockMode::kExclusive:
            return "X";
    }
    return "?";
}

LockTable::LockTable(std::uint32_t core_count, std::size_t max_locks_per_txn)
    : partitions_(kLockPartitionsPerCore * core_count),
      max_locks_per_txn_(max_locks_per_txn),
      // Sized in the member-init list: a `vector<atomic<T>>` cannot be
      // assigned into place, and its elements value-initialize to zero.
      fence_counters_(kLockPartitionsPerCore * core_count) {
    // **At one core no latch is constructed at all** (AO-R9): the table
    // exists, the conflicts are still answered, and the synchronization is
    // what compiles out. `LatchGuard(nullptr)` is then two predictable
    // branches, `base/latch.hpp`'s stated shape for exactly this.
    if (core_count > 1) {
        wait_latch_ = std::make_unique<Latch>();
        latches_.reserve(partitions_.size());
        for (std::size_t i = 0; i < partitions_.size(); ++i) {
            latches_.push_back(std::make_unique<Latch>());
            partitions_[i].latch = latches_.back().get();
        }
    }
}

StatusOr<std::unique_ptr<LockTable>> LockTable::Create(std::uint32_t core_count,
                                                       std::size_t max_locks_per_txn) {
    if (core_count == 0) {
        return Status::InvalidArgument(
            "a lock table needs at least one core: zero would size the partition array to nothing "
            "and every key would hash out of range");
    }
    if (max_locks_per_txn == 0) {
        return Status::InvalidArgument(
            "max_locks_per_txn must be at least 1: a cap of zero refuses every borrow, which is "
            "not a cap but a disabled lock family");
    }
    return std::unique_ptr<LockTable>(new LockTable(core_count, max_locks_per_txn));
}

std::size_t LockTable::PartitionOf(const LockKey& key) const noexcept {
    const std::uint64_t h =
        Mix(key.rel_oid * 0x100000001B3ULL) ^
        Mix(static_cast<std::uint64_t>(key.unit) * 0xD6E8FEB86659FD93ULL) ^ Mix(key.lo);
    return static_cast<std::size_t>(h % partitions_.size());
}

LockTable::Partition& LockTable::PartitionFor(const LockKey& key) noexcept {
    return partitions_[PartitionOf(key)];
}

const LockTable::Partition& LockTable::PartitionFor(const LockKey& key) const noexcept {
    return partitions_[PartitionOf(key)];
}

const Latch* LockTable::LatchOf(const LockKey& key) const noexcept {
    return PartitionFor(key).latch;
}

std::atomic<std::uint64_t>& LockTable::FenceCounterFor(catalog::Oid rel) noexcept {
    return fence_counters_[static_cast<std::size_t>(Mix(rel) % fence_counters_.size())];
}

const std::atomic<std::uint64_t>& LockTable::FenceCounterFor(catalog::Oid rel) const noexcept {
    return fence_counters_[static_cast<std::size_t>(Mix(rel) % fence_counters_.size())];
}

StatusOr<AcquireResult> LockTable::Acquire(std::uint64_t txn, const LockKey& key, LockMode mode,
                                           LockHoldings& holdings) {
    AcquireResult result;

    // **A transaction waits on at most one unit**, which `LockHoldings`
    // records as one field - so asking for a *different* unit abandons the
    // previous wait, and the queued record has to go with it. Left behind
    // it is an orphan nothing can ever withdraw: `Release` unwinds only
    // `waiting_`, which now names the new key, so the old entry keeps a
    // waiter for the life of the instance, `WaiterCount` - AO-R3's probe
    // gate - never returns to zero, and AO-S4a's wait-for graph would read
    // an edge for a wait nobody is in and close a cycle on it.
    //
    // Withdrawn **before** the partition latch below, so only one partition
    // latch is ever held at a time (AO-R2's order).
    if (holdings.waiting_ && !(*holdings.waiting_ == key)) {
        DequeueWaiter(txn, *holdings.waiting_);
        holdings.waiting_.reset();
    }

    const bool fence = IsFence(key, mode);

    // A fence's counter rises **before** its entry is published, so that a
    // thread which observes the entry observes the counter with it - both
    // are published under the partition mutex below. It does not order a
    // writer that read zero against a taker that has not incremented yet;
    // the header says why, and closing that is AO-S6's.
    if (fence) FenceCounterFor(key.rel_oid).fetch_add(1, std::memory_order_release);

    {
        Partition& part = PartitionFor(key);
        LatchGuard guard(part.latch);

        Entry* entry = nullptr;
        for (Entry& e : part.entries) {
            if (e.key == key) {
                entry = &e;
                break;
            }
        }

        if (entry != nullptr) {
            // Already covered by this transaction's own hold: no entry, no
            // cap spent. A walk that re-enters a unit it borrowed must not
            // pay the cap per visit.
            for (const Tenant& h : entry->holders) {
                if (h.txn == txn && Covers(h.mode, mode)) {
                    if (fence) FenceCounterFor(key.rel_oid).fetch_sub(1, std::memory_order_release);
                    result.granted = true;
                    result.already_held = true;
                    return result;
                }
            }
        }

        // **The cap, before the conflict test.** A capped transaction must
        // be refused outright rather than queued: the grant it would wait
        // for is one this same check would refuse on arrival, so queuing it
        // would turn a refusal into a wait that ends in the same refusal -
        // which is the shape AR2-R10 forbids for a different reason and
        // AO-R10's "a cap refuses" forbids for this one.
        if (holdings.held_.size() >= max_locks_per_txn_) {
            if (fence) FenceCounterFor(key.rel_oid).fetch_sub(1, std::memory_order_release);
            return Status::ResourceExhausted(
                "transaction reached max_locks_per_txn (" + std::to_string(max_locks_per_txn_) +
                " borrows); the borrow model never escalates a transaction's fine borrows into a "
                "coarse one, so a cap refuses rather than truncating (AR2-R4)");
        }

        if (entry != nullptr) {
            // A conflicting holder that is not us. Two holds by one
            // transaction never conflict: a transaction cannot deadlock
            // against itself, and refusing here would make a statement that
            // takes `IX` then `X` on one unit fail rather than proceed.
            for (const Tenant& h : entry->holders) {
                if (h.txn == txn) continue;
                if (!Compatible(h.mode, mode)) {
                    if (fence)
                        FenceCounterFor(key.rel_oid).fetch_sub(1, std::memory_order_release);
                    result.granted = false;
                    result.blocking_txn = h.txn;
                    // Queued, and **recorded in `holdings`** so it can be
                    // withdrawn: a transaction is one thread of control, so
                    // it waits on at most one unit and one record suffices.
                    // Without this the entry would outlive every holder and
                    // `WaiterCount` - AO-R3's probe gate - would stay
                    // nonzero for the life of the instance.
                    //
                    // A re-ask by an already-queued waiter keeps the slot it
                    // is parked on. Handing it a fresh one would strand the
                    // `WaitUntil` predicate on a slot the next decide no
                    // longer knows about - a lost wakeup whose only exit is
                    // the fault net, which AO-R8 says is never the normal
                    // end of a wait.
                    Tenant* queued = nullptr;
                    for (Tenant& w : entry->waiters) {
                        if (w.txn == txn) {
                            queued = &w;
                            break;
                        }
                    }
                    if (queued == nullptr) {
                        entry->waiters.push_back(
                            Tenant{txn, mode, std::make_shared<LockWaitSlot>()});
                        queued = &entry->waiters.back();
                    }
                    // **The wake is consumed here, under this latch.** A
                    // waiter that was woken, re-asked, and is refused again
                    // must park on a slot that is *not* already flipped, or
                    // its predicate is satisfied on entry, `WaitUntil` never
                    // suspends, and the acquire loop spins the reactor
                    // instead of waiting on it.
                    //
                    // Clearing it here rather than in the caller is what
                    // makes the clear race-free: `WakeWaiters` takes this
                    // same partition latch, so a wake either lands before
                    // this clear - in which case the conflict check above
                    // has just seen the state that wake announced - or
                    // after it, in which case the flip stands and the next
                    // poll picks it up. There is no interleaving that
                    // clears a wake the waiter has not already accounted
                    // for, which is the lost wakeup AO-R8's fault net would
                    // otherwise be the only exit from.
                    queued->slot->ready.store(false, std::memory_order_release);
                    queued->mode = mode;
                    result.slot = queued->slot;
                    holdings.waiting_ = key;
                    return result;
                }
            }
        }

        if (entry == nullptr) {
            part.entries.push_back(Entry{key, {}, {}});
            entry = &part.entries.back();
        }
        entry->holders.push_back(Tenant{txn, mode, nullptr});
        // A granted request leaves no queued request of its own behind. Its
        // slot dies with the record unless the waiter still holds a
        // `shared_ptr` to it, which is exactly the parked-and-since-granted
        // case: that predicate reads a slot nobody will flip again, and it
        // does not matter, because the waiter has already been granted and
        // is not parked on it any more.
        entry->waiters.erase(std::remove_if(entry->waiters.begin(), entry->waiters.end(),
                                            [&](const Tenant& w) { return w.txn == txn; }),
                             entry->waiters.end());
    }

    if (holdings.waiting_ && *holdings.waiting_ == key) holdings.waiting_.reset();
    holdings.held_.push_back(LockHoldings::Held{key, mode});
    result.granted = true;
    return result;
}

bool LockTable::NoteWaitFor(std::uint64_t waiter, std::uint64_t holder) {
    LatchGuard guard(wait_latch_.get());
    // Walk from the holder along the edges already recorded. If the walk
    // reaches the waiter, the edge about to be added closes a cycle and the
    // waiter is the victim (AO-R7). The walk is bounded by the edge count,
    // which is bounded by the live transactions on this core, and it cannot
    // loop forever: an existing cycle would have been refused at the
    // registration that closed it, so the graph is acyclic on entry.
    std::uint64_t at = holder;
    for (std::size_t steps = 0; steps <= wait_edges_.size(); ++steps) {
        if (at == waiter) return true;
        bool advanced = false;
        for (const auto& [w, h] : wait_edges_) {
            if (w == at) {
                at = h;
                advanced = true;
                break;
            }
        }
        if (!advanced) break;  // the chain ends at a transaction that waits for nothing
    }

    for (auto& [w, h] : wait_edges_) {
        if (w == waiter) {
            h = holder;
            return false;
        }
    }
    wait_edges_.emplace_back(waiter, holder);
    return false;
}

void LockTable::ClearWaitFor(std::uint64_t waiter) {
    LatchGuard guard(wait_latch_.get());
    wait_edges_.erase(std::remove_if(wait_edges_.begin(), wait_edges_.end(),
                                     [&](const auto& e) { return e.first == waiter; }),
                      wait_edges_.end());
}

std::size_t LockTable::WaitEdgeCount() const {
    LatchGuard guard(wait_latch_.get());
    return wait_edges_.size();
}

void LockTable::WakeWaiters(const LockKey& key) {
    // Flip every queued slot on the unit. **A wake is not a grant**: each
    // woken waiter re-asks through `Acquire`, and those that still conflict
    // queue again on the slot they already hold. That costs a poll per
    // waiter per decide and never a wrong answer, which is the trade
    // AO-R4's mandatory re-check makes unavoidable - a grant handed out
    // here would be a grant decided under this latch and consumed after it,
    // with nothing holding the unit in between.
    Partition& part = PartitionFor(key);
    LatchGuard guard(part.latch);
    for (Entry& e : part.entries) {
        if (!(e.key == key)) continue;
        for (Tenant& w : e.waiters) {
            if (w.slot != nullptr) w.slot->ready.store(true, std::memory_order_release);
        }
        return;
    }
}

void LockTable::DequeueWaiter(std::uint64_t txn, const LockKey& key) {
    Partition& part = PartitionFor(key);
    LatchGuard guard(part.latch);
    for (std::size_t i = 0; i < part.entries.size(); ++i) {
        Entry& e = part.entries[i];
        if (!(e.key == key)) continue;
        // **Flipped on the way out.** A withdrawal drops the record the
        // next decide would have flipped, so a waiter still parked on this
        // slot - AO-S4a's deadlock victim, AO-R8's fault net - would poll a
        // bit nobody owns any more and never be entered again. The wake is
        // not a grant (`WakeWaiters`), so telling a withdrawn waiter to
        // look again costs one poll and cannot admit it to anything.
        for (Tenant& w : e.waiters) {
            if (w.txn == txn && w.slot != nullptr) {
                w.slot->ready.store(true, std::memory_order_release);
            }
        }
        e.waiters.erase(std::remove_if(e.waiters.begin(), e.waiters.end(),
                                       [&](const Tenant& w) { return w.txn == txn; }),
                        e.waiters.end());
        if (e.holders.empty() && e.waiters.empty()) {
            part.entries.erase(part.entries.begin() + static_cast<std::ptrdiff_t>(i));
        }
        return;
    }
}

void LockTable::Release(std::uint64_t txn, LockHoldings& holdings) {
    for (const LockHoldings::Held& held : holdings.held_) {
        bool removed_holder = false;
        {
            Partition& part = PartitionFor(held.key);
            LatchGuard guard(part.latch);
            for (std::size_t i = 0; i < part.entries.size(); ++i) {
                Entry& e = part.entries[i];
                if (!(e.key == held.key)) continue;
                for (std::size_t j = 0; j < e.holders.size(); ++j) {
                    if (e.holders[j].txn == txn && e.holders[j].mode == held.mode) {
                        e.holders.erase(e.holders.begin() + static_cast<std::ptrdiff_t>(j));
                        removed_holder = true;
                        break;
                    }
                }
                // An entry nobody holds and nobody waits on is erased, so
                // the table measures live borrows rather than every key
                // ever touched.
                if (e.holders.empty() && e.waiters.empty()) {
                    part.entries.erase(part.entries.begin() + static_cast<std::ptrdiff_t>(i));
                }
                break;
            }
        }
        // Lowered **only when a holder was actually removed**, and after
        // the entry is gone - the mirror of raising it before publication.
        // Unconditional here would underflow the counter to 2^64-1 on any
        // double release, and a permanently nonzero gate sends every writer
        // on the relation through the all-partition scan for the life of
        // the instance.
        if (removed_holder && IsFence(held.key, held.mode)) {
            FenceCounterFor(held.key.rel_oid).fetch_sub(1, std::memory_order_release);
        }
        // Then wake whoever was queued on it, in a second acquisition of the
        // same partition latch rather than inside the one above: a wake is
        // a store nobody is waiting on synchronously, and keeping it out of
        // the release's own critical section keeps that section the length
        // of a vector erase.
        if (removed_holder) WakeWaiters(held.key);
    }
    holdings.held_.clear();

    // A transaction that ended while queued takes its request with it, and
    // its wait-for edge with that: a decided transaction waits for nothing,
    // and an edge left behind would be a false cycle for the next waiter to
    // close against.
    if (holdings.waiting_) {
        DequeueWaiter(txn, *holdings.waiting_);
        holdings.waiting_.reset();
    }
    ClearWaitFor(txn);
}

std::uint64_t LockTable::RelationFenceCount(catalog::Oid rel) const {
    return FenceCounterFor(rel).load(std::memory_order_acquire);
}

bool LockTable::FenceCoversKey(catalog::Oid rel, std::uint64_t pk, std::uint64_t txn) const {
    // Every partition, because a fence's partition is decided by its own
    // `lo` and the key being probed is not it. Reached only when the
    // relation's fence counter is nonzero (AO-R3), which is why a scan is
    // the right shape and an interval index is not: the common case does
    // not run this at all.
    for (const Partition& part : partitions_) {
        LatchGuard guard(part.latch);
        for (const Entry& e : part.entries) {
            if (e.key.rel_oid != rel || !e.key.IsFenceUnit()) continue;
            if (!e.key.ContainsKey(pk)) continue;
            for (const Tenant& h : e.holders) {
                if (h.txn == txn) continue;
                if (h.mode == LockMode::kShared || h.mode == LockMode::kExclusive) return true;
            }
        }
    }
    return false;
}

std::size_t LockTable::WaiterCount(const LockKey& key) const {
    const Partition& part = PartitionFor(key);
    LatchGuard guard(part.latch);
    for (const Entry& e : part.entries) {
        if (e.key == key) return e.waiters.size();
    }
    return 0;
}

std::size_t LockTable::EntryCount() const {
    std::size_t n = 0;
    for (const Partition& part : partitions_) {
        LatchGuard guard(part.latch);
        n += part.entries.size();
    }
    return n;
}

}  // namespace kds::txn
