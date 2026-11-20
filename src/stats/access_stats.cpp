#include "kds/stats/access_stats.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <tuple>

#include "kds/catalog/rows.hpp"
#include "kds/exec/plan_printer.hpp"

namespace kds::stats {

namespace {

std::size_t HomeOf(exec::AccessKind kind, catalog::Oid rel_id, std::uint64_t mask,
                   std::size_t slots) noexcept {
    // splitmix64's finaliser over the three fields: the table's index is a
    // modulus, so the low bits must carry every field.
    std::uint64_t h = rel_id * 0x9E3779B97F4A7C15ull ^ mask ^
                      (static_cast<std::uint64_t>(kind) << 56);
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    return static_cast<std::size_t>(h % slots);
}

}  // namespace

Status AccessStats::CheckBytesPerCore(std::uint64_t bytes) {
    if (bytes >= kMinBytesPerCore && bytes <= kMaxBytesPerCore) return Status::OK();
    return Status::InvalidArgument("access_stats_bytes_per_core must be in [" +
                                   std::to_string(kMinBytesPerCore) + ", " +
                                   std::to_string(kMaxBytesPerCore) + "], got " +
                                   std::to_string(bytes));
}

AccessStats::AccessStats(std::uint64_t bytes_per_core) noexcept
    : slots_(static_cast<std::size_t>(
          std::clamp(bytes_per_core, kMinBytesPerCore, kMaxBytesPerCore) / kSlotBytes)) {}

AccessStats::~AccessStats() {
    for (Part& part : parts_) delete[] part.table.load(std::memory_order_relaxed);
}

AccessStats::Slot* AccessStats::TableOf(Part& part) noexcept {
    Slot* table = part.table.load(std::memory_order_acquire);
    if (table != nullptr) return table;
    // The one allocation, at the part's first record, and never retried
    // once it failed: a record must not try an allocation every time.
    if (part.failed.load(std::memory_order_relaxed)) return nullptr;
    Slot* fresh = new (std::nothrow) Slot[slots_]();
    if (fresh == nullptr) {
        part.failed.store(true, std::memory_order_relaxed);
        return nullptr;
    }
    if (part.table.compare_exchange_strong(table, fresh, std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
        return fresh;
    }
    delete[] fresh;  // another thread recording as this part published first
    return table;
}

void AccessStats::Fill(Slot& slot, std::uint32_t claiming, exec::AccessKind kind,
                       catalog::Oid rel_id, std::uint64_t column_mask,
                       std::uint64_t now) noexcept {
    // The claim is ordered before the key's words: a reader that copies a
    // word of the new key then sees the state moved on its second read
    // (paired with the acquire fence in `Snapshot`).
    std::atomic_thread_fence(std::memory_order_release);
    slot.rel_id.set(rel_id);
    slot.column_mask.set(column_mask);
    slot.kind.store(static_cast<std::uint8_t>(kind), std::memory_order_relaxed);
    slot.uses.set(1);
    slot.last_seen.set(now);
    slot.state.store((claiming & ~kStateMask) | kLive, std::memory_order_release);
}

bool AccessStats::RoomForANewShape(const Part& part) const noexcept {
    const std::size_t claimed = part.claimed.load(std::memory_order_relaxed);
    const std::size_t dead = part.dead.load(std::memory_order_relaxed);
    return claimed - std::min(dead, claimed) < slots_ / 2;
}

void AccessStats::Record(std::uint32_t core, exec::AccessKind kind, catalog::Oid rel_id,
                         std::uint64_t column_mask, std::uint64_t now) noexcept {
    Part& part = parts_[core % kStatsParts];
    Slot* const table = TableOf(part);
    if (table == nullptr) {
        part.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The walk runs to the shape's own slot or to the first empty one - the
    // end of every run a key homed here could be in - and remembers the
    // first dead slot it passes: a forgotten slot a new shape may take.
    const std::size_t none = slots_;
    std::size_t reusable = none;
    std::uint32_t reusable_state = 0;
    std::size_t empty = none;
    std::uint32_t empty_state = 0;
    std::size_t walked = 0;  // the slots passed before `empty`
    for (std::size_t i = HomeOf(kind, rel_id, column_mask, slots_), n = 0; n < slots_;
         i = i + 1 == slots_ ? 0 : i + 1, ++n) {
        Slot& slot = table[i];
        const std::uint32_t state = slot.state.load(std::memory_order_acquire);
        if (StateOf(state) == kEmpty) {
            empty = i;
            empty_state = state;
            walked = n;
            break;
        }
        // A slot mid-claim has no key to read yet (another thread recording
        // as this part): walked past, which at worst gives this shape a
        // second slot the reader's merge adds.
        if (StateOf(state) == kClaiming) continue;
        if (!slot.Is(kind, rel_id, column_mask)) {
            if (StateOf(state) == kDead && reusable == none) {
                reusable = i;
                reusable_state = state;
            }
            continue;
        }
        if (StateOf(state) == kDead) {
            // Forgotten: the shape starts again from this execution - as a
            // new shape, under the bound, in the slot it already has. The
            // counts are set while a reader still skips the slot.
            if (!RoomForANewShape(part)) {
                part.dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            slot.uses.set(1);
            slot.last_seen.set(now);
            std::uint32_t dead = state;
            if (slot.state.compare_exchange_strong(dead, NextAs(state, kLive),
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
                part.dead.fetch_sub(1, std::memory_order_relaxed);
                return;
            }
            // Revived by another thread recording as this part: counted below.
        }
        if (const std::uint64_t uses = slot.uses.get();
            uses != std::numeric_limits<std::uint64_t>::max()) {
            slot.uses.set(uses + 1);
        }
        slot.last_seen.set(now);
        return;
    }
    // **Trailing dead slots are given back.** Every stored key's slots from
    // its home to its own are non-empty, so no key's run passes a dead slot
    // whose next slot is empty: a walk that reaches it stops at the empty
    // one after it. The part's core - the only claimer - makes such a slot
    // empty again, and the one before it in turn, back to this walk's home;
    // so forgets leave no slots that lengthen every later miss, and the
    // load bound stays a bound on the walk.
    while (empty != none && walked > 0) {
        const std::size_t before = empty == 0 ? slots_ - 1 : empty - 1;
        std::uint32_t state = table[before].state.load(std::memory_order_acquire);
        if (StateOf(state) != kDead) break;
        const std::uint32_t freed = NextAs(state, kEmpty);
        if (!table[before].state.compare_exchange_strong(state, freed, std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
            break;
        }
        part.claimed.fetch_sub(1, std::memory_order_relaxed);
        part.dead.fetch_sub(1, std::memory_order_relaxed);
        if (reusable == before) reusable = none;
        empty = before;
        empty_state = freed;
        --walked;
    }
    // A new shape: admitted while fewer than half the slots are live.
    if (RoomForANewShape(part)) {
        // A forgotten slot on the walk first: it keeps the run as short as
        // it was. Its generation moves, so a reader mid-copy drops the row.
        if (reusable != none) {
            const std::uint32_t claiming = NextAs(reusable_state, kClaiming);
            if (table[reusable].state.compare_exchange_strong(reusable_state, claiming,
                                                              std::memory_order_acq_rel,
                                                              std::memory_order_acquire)) {
                part.dead.fetch_sub(1, std::memory_order_relaxed);
                Fill(table[reusable], claiming, kind, rel_id, column_mask, now);
                return;
            }
        }
        if (empty != none) {
            const std::uint32_t claiming = NextAs(empty_state, kClaiming);
            if (table[empty].state.compare_exchange_strong(empty_state, claiming,
                                                           std::memory_order_acq_rel,
                                                           std::memory_order_acquire)) {
                part.claimed.fetch_add(1, std::memory_order_relaxed);
                Fill(table[empty], claiming, kind, rel_id, column_mask, now);
                return;
            }
        }
        // Lost both to another thread recording as this part: the record is
        // dropped rather than the walk begun again.
    }
    part.dropped.fetch_add(1, std::memory_order_relaxed);
}

void AccessStats::Forget(catalog::Oid rel_id, std::uint16_t col_pos) noexcept {
    const std::uint64_t bit = catalog::ColumnBit(col_pos);
    if (bit == 0) return;  // a mask names positions below 64 only
    for (Part& part : parts_) {
        Slot* const table = part.table.load(std::memory_order_acquire);
        if (table == nullptr) continue;
        for (std::size_t i = 0; i < slots_; ++i) {
            Slot& slot = table[i];
            std::uint32_t state = slot.state.load(std::memory_order_acquire);
            if (StateOf(state) != kLive || slot.rel_id.get() != rel_id ||
                (slot.column_mask.get() & bit) == 0) {
                continue;
            }
            // Marked where it stands, the generation kept: the exchange
            // fails if the slot changed hands since its key was read here,
            // and then it is no longer the shape that was to be forgotten -
            // or another forget marked it first.
            if (slot.state.compare_exchange_strong(state, (state & ~kStateMask) | kDead,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
                part.dead.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

std::vector<AccessShape> AccessStats::Snapshot() const {
    std::vector<AccessShape> rows;
    for (const Part& part : parts_) {
        const Slot* const table = part.table.load(std::memory_order_acquire);
        if (table == nullptr) continue;
        for (std::size_t i = 0; i < slots_; ++i) {
            const Slot& slot = table[i];
            const std::uint32_t state = slot.state.load(std::memory_order_acquire);
            if (StateOf(state) != kLive) continue;
            const AccessShape row{
                static_cast<exec::AccessKind>(slot.kind.load(std::memory_order_relaxed)),
                slot.rel_id.get(), slot.column_mask.get(), slot.uses.get(), slot.last_seen.get()};
            // The slot changed hands under the copy - forgotten, or forgotten
            // and given to another shape: the row is left out. The fence
            // keeps the copy's loads ahead of the second read (`Fill`'s pair).
            std::atomic_thread_fence(std::memory_order_acquire);
            if (slot.state.load(std::memory_order_relaxed) != state) continue;
            rows.push_back(row);
        }
    }
    // The merge: one row per key for the instance.
    const auto key = [](const AccessShape& s) {
        return std::make_tuple(s.rel_id, std::string_view(exec::AccessKindName(s.kind)),
                               s.column_mask);
    };
    std::sort(rows.begin(), rows.end(),
              [&](const AccessShape& a, const AccessShape& b) { return key(a) < key(b); });
    std::vector<AccessShape> merged;
    for (const AccessShape& row : rows) {
        if (!merged.empty() && key(merged.back()) == key(row)) {
            AccessShape& into = merged.back();
            into.uses = row.uses > std::numeric_limits<std::uint64_t>::max() - into.uses
                            ? std::numeric_limits<std::uint64_t>::max()
                            : into.uses + row.uses;
            into.last_seen = std::max(into.last_seen, row.last_seen);
        } else {
            merged.push_back(row);
        }
    }
    return merged;
}

std::uint64_t AccessStats::Dropped() const noexcept {
    std::uint64_t total = 0;
    for (const Part& part : parts_) total += part.dropped.load(std::memory_order_relaxed);
    return total;
}

std::uint64_t ColumnMaskOf(const exec::Step& step) noexcept {
    std::uint64_t mask = 0;
    for (std::uint16_t col : step.access_columns) mask |= catalog::ColumnBit(col);
    return mask;
}

void RecordChainAccess(AccessStats& store, std::uint32_t core, const exec::StepChain& chain,
                       std::uint64_t now) noexcept {
    // Every step, sub-chains and hoisted ones included: each is a real
    // access against a real relation, and where in the statement it sat is
    // not part of the shape. **No branch on `step.kind`.**
    exec::ForEachStep(chain, [&](const exec::Step& step) {
        store.Record(core, step.kind, step.rel_oid, ColumnMaskOf(step), now);
        return true;
    });
}

}  // namespace kds::stats
