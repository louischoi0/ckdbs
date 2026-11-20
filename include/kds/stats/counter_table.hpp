#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>

// **One core's counts keyed by a 64-bit id, with no latch** - the shape of
// `docs/rules/rules.md` §3a for a row of fixed counters (DL's W1,
// `instructions/v3.2.0/workorder-dl-statistics-queue-no-statement.md` §2):
// a record takes nothing and waits for nothing, and a reader reads the
// words as they stand.
//
// ---- The protocol ----------------------------------------------------------
//
// A fixed table of `2 * kBound` slots (rounded up to a power of two), probed
// linearly from the id's hash, **insert-only**: a slot's id is claimed once,
// by a compare-exchange from 0, and never changed or released, so a slot's
// address holds for the table's life and a writer may keep it. The table is
// allocated whole at the part's first sight of any id - the one allocation -
// and published by a compare-exchange; it is never grown, moved or freed
// before the destructor.
//
// `V` is a row of `sched::RelaxedU64` words (or anything else whose every
// concurrent read and write is an atomic operation) that value-initialises
// to zero. **One writer a part is the caller's premise, and nothing here
// depends on it for memory safety**: two threads recording as one part find
// or claim the same slot and can lose a count between a word's load and its
// store, and nothing else. A reader on any thread walks the slots with
// acquire loads; a row's words may differ by a record in flight.
//
// **A record never fails, delays or reorders what it describes.** A new id
// past `kBound`, or a part whose table could not be allocated - tried once,
// never again - is dropped and counted.

namespace kds::stats {

template <typename V, std::size_t kBound>
class CounterTable {
public:
    CounterTable() noexcept = default;
    ~CounterTable() { delete[] table_.load(std::memory_order_relaxed); }
    CounterTable(const CounterTable&) = delete;
    CounterTable& operator=(const CounterTable&) = delete;

    // The counts of `id`, which is not 0, claimed at its first sight. Null,
    // and one drop counted, where the part is at its bound or has no table.
    V* CountsOf(std::uint64_t id) noexcept {
        Slot* table = table_.load(std::memory_order_acquire);
        if (table == nullptr) table = Allocate();
        if (table == nullptr) return Drop();
        // At most `kBound` of `kSlots >= 2 * kBound` slots are claimed, so
        // the walk meets an empty slot; the count bounds it all the same.
        for (std::size_t i = HomeOf(id), n = 0; n < kSlots; i = (i + 1) & (kSlots - 1), ++n) {
            std::uint64_t seen = table[i].id.load(std::memory_order_acquire);
            if (seen == id) return &table[i].counts;
            if (seen != 0) continue;
            if (live_.load(std::memory_order_relaxed) >= kBound) break;
            if (table[i].id.compare_exchange_strong(seen, id, std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
                live_.fetch_add(1, std::memory_order_relaxed);
                return &table[i].counts;
            }
            // Another thread recording as this part took the slot: for this
            // id, which is then found; for another, and the walk goes on.
            if (seen == id) return &table[i].counts;
        }
        return Drop();
    }

    // The counts of `id` if the part holds it, claiming nothing: a reader's
    // lookup of one row.
    const V* Peek(std::uint64_t id) const noexcept {
        const Slot* table = table_.load(std::memory_order_acquire);
        if (table == nullptr) return nullptr;
        for (std::size_t i = HomeOf(id), n = 0; n < kSlots; i = (i + 1) & (kSlots - 1), ++n) {
            const std::uint64_t seen = table[i].id.load(std::memory_order_acquire);
            if (seen == id) return &table[i].counts;
            if (seen == 0) return nullptr;
        }
        return nullptr;
    }

    // The same, for a caller that writes the row it finds and wants no slot
    // claimed where there is none.
    V* Find(std::uint64_t id) noexcept {
        return const_cast<V*>(static_cast<const CounterTable*>(this)->Peek(id));
    }

    // `fn(id, const V&)` for every id the part holds, in slot order. Takes
    // nothing: a row is read as its words stand.
    template <typename Fn>
    void ForEach(Fn&& fn) const {
        const Slot* table = table_.load(std::memory_order_acquire);
        if (table == nullptr) return;
        for (std::size_t i = 0; i < kSlots; ++i) {
            const std::uint64_t id = table[i].id.load(std::memory_order_acquire);
            if (id != 0) fn(id, table[i].counts);
        }
    }

    // New ids the part could not take.
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    // Zeroes the part where it stands, and with it every row a caller kept
    // the address of. **For a test, with no writer running**: an id claimed
    // while this runs may be lost with its count.
    void ResetForTest() noexcept {
        if (Slot* table = table_.load(std::memory_order_acquire); table != nullptr) {
            for (std::size_t i = 0; i < kSlots; ++i) {
                table[i].id.store(0, std::memory_order_release);
                table[i].counts = V{};
            }
        }
        live_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
    }

private:
    struct Slot {
        std::atomic<std::uint64_t> id{0};  // 0: unclaimed
        V counts{};
    };
    static constexpr std::size_t kSlots = std::bit_ceil(2 * kBound);

    static std::size_t HomeOf(std::uint64_t id) noexcept {
        // splitmix64's finaliser: the index is a mask, so the low bits must
        // carry the whole id (oids are issued in sequence).
        id ^= id >> 30;
        id *= 0xBF58476D1CE4E5B9ull;
        id ^= id >> 27;
        id *= 0x94D049BB133111EBull;
        id ^= id >> 31;
        return static_cast<std::size_t>(id) & (kSlots - 1);
    }

    // The table, made at the part's first sight. A failed allocation is
    // never tried again: a record must not try one every time.
    Slot* Allocate() noexcept {
        if (failed_.load(std::memory_order_relaxed)) return nullptr;
        Slot* fresh = new (std::nothrow) Slot[kSlots]();
        if (fresh == nullptr) {
            failed_.store(true, std::memory_order_relaxed);
            return nullptr;
        }
        Slot* expected = nullptr;
        if (table_.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            return fresh;
        }
        delete[] fresh;  // another thread recording as this part published first
        return expected;
    }

    V* Drop() noexcept {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    std::atomic<Slot*> table_{nullptr};
    std::atomic<std::uint64_t> live_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool> failed_{false};
};

}  // namespace kds::stats
