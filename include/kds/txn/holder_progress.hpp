#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

// **A lock holder's progress, read by the fault net** (`txn.md` §5).
//
// The lock family's fault net ends a wait the holder never ends - a missed
// cycle or a stuck holder (AO-R8) - and its 1 s bound rests on D12's
// premise that the longest honest wait is a local statement's own work. A
// relation-`X` build (`CREATE INDEX`, `REINDEX INDEX`, `CREATE ASSERTION`)
// is a local statement whose work is O(relation), so a writer queued behind
// a build longer than the net was refused as though the build were stuck.
// The net therefore asks whether the holder **advanced**: a holder that
// registered here and moved its count since the waiter last looked is not
// stuck, and the waiter's bound restarts. A holder that did not register -
// every holder but a build - is what it was: the net ends the wait.
//
// **Protocol.** One instance, owned by the `LockTable`, read by every core.
// A slot's `holder` is claimed by a CAS from 0 and cleared by the scope that
// claimed it; its `count` is written by that scope's thread alone (a release
// increment no other writer races) and read by waiters as a seqlock reads:
// `holder`, `count`, an acquire fence, `holder` again. A reader that finds
// the same `holder` both times read that holder's count, since ids are
// never reused (`trx_id.hpp`); a count from the slot's next owner would
// have been written after that owner's claim, which the fence orders before
// the second look. A waiter compares two counts and orders nothing else
// against one. No latch: a park predicate runs
// once per reactor iteration and must take none (`LockWaitReady`'s rule).
//
// **A full registry registers nothing** and the build runs unregistered,
// waited for under the plain net - the old behaviour, never a wrong answer.
// `kSlots` is far above the builds an instance runs at once: each holds one
// relation's `X` and runs on its session's core.
namespace kds::txn {

class HolderProgress {
    // One line per slot: its owner writes `count` once per leaf, and a
    // neighbour's owner must not pay for it.
    struct alignas(64) Slot {
        std::atomic<std::uint64_t> holder{0};
        std::atomic<std::uint64_t> count{0};
    };

public:
    static constexpr std::size_t kSlots = 64;

    // A registration for one holder, released by its destructor. A scope
    // that found no slot, or was given no registry, advances nothing.
    class Scope {
    public:
        Scope() = default;
        Scope(HolderProgress* registry, std::uint64_t holder) noexcept {
            if (registry == nullptr || holder == 0) return;
            for (Slot& s : registry->slots_) {
                std::uint64_t empty = 0;
                if (s.holder.compare_exchange_strong(empty, holder, std::memory_order_acq_rel)) {
                    slot_ = &s;
                    return;
                }
            }
        }
        ~Scope() {
            if (slot_ != nullptr) slot_->holder.store(0, std::memory_order_release);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        void Advance() noexcept {
            if (slot_ == nullptr) return;
            slot_->count.store(slot_->count.load(std::memory_order_relaxed) + 1,
                               std::memory_order_release);
        }

    private:
        Slot* slot_ = nullptr;
    };

    // `holder`'s count, or nothing when it holds no registration.
    std::optional<std::uint64_t> Read(std::uint64_t holder) const noexcept {
        if (holder == 0) return std::nullopt;
        for (const Slot& s : slots_) {
            if (s.holder.load(std::memory_order_acquire) != holder) continue;
            const std::uint64_t count = s.count.load(std::memory_order_relaxed);
            // Keeps the count's load before the re-check (a seqlock read).
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s.holder.load(std::memory_order_relaxed) != holder) return std::nullopt;
            return count;
        }
        return std::nullopt;
    }

    // Scopes registered now. For a cell that must act while a build holds.
    std::size_t Registered() const noexcept {
        std::size_t n = 0;
        for (const Slot& s : slots_) n += s.holder.load(std::memory_order_acquire) != 0;
        return n;
    }

private:
    std::array<Slot, kSlots> slots_{};
};

}  // namespace kds::txn
