#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>

// ---- The page latch word (AM-R3, AR2-R2) ----------------------------------
//
// One 32-bit word per resident buffer-pool frame, operated on through
// `std::atomic_ref` so the frame that carries it stays a plain, movable
// aggregate (`DevicePageStore::Frame`). This header is the pure protocol
// over the word - encode, decode, try, acquire, release - and knows nothing
// about frames, pins or cores beyond the id it is handed.
//
//     bit 31        `X`     - held exclusively
//     bits 24..30   owner   - `core_id + 1` of the exclusive holder; 0 = none
//     bits 0..23    count   - under `X`: the owner core's hold depth, every
//                             mode counted; with `X` clear: the number of
//                             shared holders, from any core
//
// **The rules the word enforces**, `docs/rules/rules.md` section 3's row for
// it and `docs/spec/page.md` section 6:
//
//   - shared acquire: admitted while `X` is clear (count + 1), and admitted
//     under this core's own exclusive hold (count + 1). No path in the tree
//     takes that second shape today - `LogFullPageImage`, the obvious
//     candidate, re-fetches with `Get` and so is exclusive-under-exclusive
//     - but a `GetForRead` under a live `Get` on one page is one edit away
//     on any read-under-write path, and refusing it would be a hang, not a
//     refusal: the store's self-deadlock check only fires on an exclusive
//     request, so an X-then-S it did not admit would spin undiagnosed;
//   - exclusive acquire: admitted on a free word, and **re-entrant for the
//     owning core** (count + 1) - one task holds a page twice on every chain
//     growth and split path, and the owning core stands for the running
//     task on the discipline that no task parks holding a pin - recorded,
//     not enforced, by exec::InstallSuspendAudit in debug builds; the word
//     itself cannot tell two tasks on one core apart;
//   - **never upgraded**: an exclusive request against shared holders is
//     `kBusy`, whoever holds the shares. The word cannot tell whose they
//     are; the store can (its pins are this core's through M1), and it is
//     the store that diagnoses "this core holds it shared and asked for
//     exclusive" as the self-deadlock it is - a debug abort naming the page,
//     a hang in release, as a recursive std::mutex acquisition is
//     (base/latch.hpp);
//   - release: under `X` the owner must be this core, and the word returns
//     to free when the depth reaches zero; otherwise one shared holder
//     fewer.
//
// **Waiting** spins with a pause hint for a few turns, then yields the
// thread on every turn. There is no queue and no fairness: a holder is in a
// critical section measured in nanoseconds, or in a WAL append (the page
// latch is *outer* to the stream latch - device_page_store.hpp says why),
// and a queue would be a lifetime question this primitive must not own.
// `Acquire` returns how many turns it spun so a store can count contention.
//
// **Not a mutex, deliberately.** `base/latch.hpp` argues for `std::mutex`
// because the WAL's sections are long (a `pwrite`, a segment roll under
// `fsync`); a page latch's are the opposite shape, and a pool of thousands
// of frames cannot carry a mutex each (AM-R3). At `cores = 1` the store
// never calls into this header at all (`DevicePageStore::latch_armed()`),
// which is G2's zero overhead as a property of the code.
//
// Concurrency: every access to the word goes through `std::atomic_ref`;
// the caller must not read or write the word non-atomically while any
// thread can reach the frame. Acquire orders: an acquisition succeeds with
// `acquire` semantics and a release publishes with `release` semantics, so
// bytes written under an exclusive hold are visible to the next holder.

namespace kds::storage {

inline constexpr std::uint32_t kPageLatchExclusiveBit = 1u << 31;
inline constexpr std::uint32_t kPageLatchOwnerShift = 24;
inline constexpr std::uint32_t kPageLatchOwnerBits = 7;
inline constexpr std::uint32_t kPageLatchOwnerMask = ((1u << kPageLatchOwnerBits) - 1u)
                                                     << kPageLatchOwnerShift;
inline constexpr std::uint32_t kPageLatchCountMask = (1u << kPageLatchOwnerShift) - 1u;

// The owner field holds `core_id + 1`, so the largest core id the word can
// name is one less than the field's maximum. The server layer asserts its
// own core cap against this at the arming site rather than this header
// including a server constant (storage sits below server).
inline constexpr std::uint32_t kPageLatchMaxCoreId = (1u << kPageLatchOwnerBits) - 2u;

// How many turns `Acquire` spins with a pause hint before it starts
// yielding the thread on every turn. Small on purpose: a holder that is
// not back within a few dozen turns is in an append, not a memcpy.
inline constexpr std::uint32_t kPageLatchSpinTurns = 64;

static_assert((kPageLatchExclusiveBit & kPageLatchOwnerMask) == 0);
static_assert((kPageLatchOwnerMask & kPageLatchCountMask) == 0);
static_assert(kPageLatchExclusiveBit + kPageLatchOwnerMask + kPageLatchCountMask == 0xFFFFFFFFu,
              "the three fields tile the word");

enum class PageLatchMode : std::uint8_t { kShared, kExclusive };

enum class PageLatchOutcome : std::uint8_t {
    kAcquired,  // held now, in the requested mode
    kBusy,      // a conflicting hold exists; the caller may spin
};

// The word, decoded. `owner_core` is meaningful only while `exclusive`.
struct PageLatchWord {
    bool exclusive = false;
    std::uint32_t owner_core = 0;
    std::uint32_t count = 0;
};

constexpr PageLatchWord DecodePageLatch(std::uint32_t word) noexcept {
    PageLatchWord w;
    w.exclusive = (word & kPageLatchExclusiveBit) != 0;
    const std::uint32_t owner_field = (word & kPageLatchOwnerMask) >> kPageLatchOwnerShift;
    w.owner_core = owner_field == 0 ? 0 : owner_field - 1;
    w.count = word & kPageLatchCountMask;
    return w;
}

constexpr std::uint32_t EncodePageLatch(bool exclusive, std::uint32_t owner_core,
                                        std::uint32_t count) noexcept {
    std::uint32_t word = count & kPageLatchCountMask;
    if (exclusive) {
        word |= kPageLatchExclusiveBit;
        word |= ((owner_core + 1u) << kPageLatchOwnerShift) & kPageLatchOwnerMask;
    }
    return word;
}

class PageLatch {
public:
    // One attempt, no waiting. `kBusy` on any conflicting hold, including
    // shared holders met by an exclusive request (the upgrade the store
    // diagnoses).
    static PageLatchOutcome TryAcquire(std::uint32_t& word, PageLatchMode mode,
                                       std::uint32_t core) noexcept {
        // The owner field cannot name a core past the cap: EncodePageLatch
        // masks `core + 1` into the field, so a core past it would decode as
        // another core's hold (127 as core 0's).
        assert(core <= kPageLatchMaxCoreId);
        std::atomic_ref<std::uint32_t> ref(word);
        std::uint32_t cur = ref.load(std::memory_order_acquire);
        for (;;) {
            std::uint32_t desired = 0;
            if (!Next(cur, mode, core, desired)) return PageLatchOutcome::kBusy;
            if (ref.compare_exchange_weak(cur, desired, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
                return PageLatchOutcome::kAcquired;
            }
            // `cur` now holds the value that beat us; loop with it.
        }
    }

    // Waits until acquired. Returns the number of turns spent waiting, 0
    // when the first attempt succeeded, so a store can count contention.
    static std::uint64_t Acquire(std::uint32_t& word, PageLatchMode mode,
                                 std::uint32_t core) noexcept {
        std::uint64_t turns = 0;
        while (TryAcquire(word, mode, core) == PageLatchOutcome::kBusy) {
            ++turns;
            if (turns <= kPageLatchSpinTurns) {
                Pause();
            } else {
                std::this_thread::yield();
            }
        }
        return turns;
    }

    // Drops one hold. Under `X` the caller must be the owning core - the
    // word says so, and a release by another core is a protocol defect this
    // header leaves to the debug build's caller to catch, since the word
    // alone cannot name the page.
    static void Release(std::uint32_t& word, std::uint32_t core) noexcept {
        std::atomic_ref<std::uint32_t> ref(word);
        std::uint32_t cur = ref.load(std::memory_order_relaxed);
        for (;;) {
            const PageLatchWord w = DecodePageLatch(cur);
            std::uint32_t desired = 0;
            if (w.exclusive) {
                if (w.owner_core != core || w.count == 0) return;  // not ours; leave it
                desired = w.count == 1 ? 0u : cur - 1u;
            } else {
                if (w.count == 0) return;  // nothing held; a double release is left alone
                desired = cur - 1u;
            }
            if (ref.compare_exchange_weak(cur, desired, std::memory_order_acq_rel,
                                          std::memory_order_relaxed)) {
                return;
            }
        }
    }

    static bool IsHeld(const std::uint32_t& word) noexcept {
        std::atomic_ref<const std::uint32_t> ref(word);
        return ref.load(std::memory_order_acquire) != 0;
    }

    static bool IsHeldExclusiveBy(const std::uint32_t& word, std::uint32_t core) noexcept {
        const PageLatchWord w =
            DecodePageLatch(std::atomic_ref<const std::uint32_t>(word).load(std::memory_order_acquire));
        return w.exclusive && w.owner_core == core;
    }

    // True when shared holders exist and nobody holds it exclusive - the
    // state an exclusive request cannot pass, and the one the store reads
    // to tell an upgrade (its own shares) from a foreign reader.
    static bool HasSharedHolders(const std::uint32_t& word) noexcept {
        const PageLatchWord w =
            DecodePageLatch(std::atomic_ref<const std::uint32_t>(word).load(std::memory_order_acquire));
        return !w.exclusive && w.count != 0;
    }

    static std::uint32_t Load(const std::uint32_t& word) noexcept {
        return std::atomic_ref<const std::uint32_t>(word).load(std::memory_order_acquire);
    }

private:
    // The transition table. False means "no admissible next word for this
    // request against `cur`" - the busy answer.
    static bool Next(std::uint32_t cur, PageLatchMode mode, std::uint32_t core,
                     std::uint32_t& desired) noexcept {
        const PageLatchWord w = DecodePageLatch(cur);
        if (w.count == kPageLatchCountMask) return false;  // saturated; treat as busy
        if (mode == PageLatchMode::kShared) {
            if (!w.exclusive || w.owner_core == core) {
                desired = cur + 1u;
                return true;
            }
            return false;
        }
        if (cur == 0) {
            desired = EncodePageLatch(true, core, 1);
            return true;
        }
        if (w.exclusive && w.owner_core == core) {
            desired = cur + 1u;
            return true;
        }
        return false;  // a foreign exclusive, or shared holders: never an upgrade
    }

    static void Pause() noexcept {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
        __builtin_ia32_pause();
#endif
    }
};

}  // namespace kds::storage
