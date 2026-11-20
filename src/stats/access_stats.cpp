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

AccessStats::~AccessStats() = default;

void AccessStats::Record(std::uint32_t core, exec::AccessKind kind, catalog::Oid rel_id,
                         std::uint64_t column_mask, std::uint64_t now) noexcept {
    Part& part = parts_[core % kStatsParts];
    const LatchGuard held(&part.latch);
    if (!part.table) {
        // The one allocation, at the part's first record, and never retried
        // once it failed: a record must not try an allocation every time.
        if (!part.failed) part.table.reset(new (std::nothrow) Slot[slots_]());
        if (!part.table) {
            part.failed = true;
            ++part.dropped;
            return;
        }
    }
    // At most half the slots are live, so the walk meets an empty slot.
    for (std::size_t i = HomeOf(kind, rel_id, column_mask, slots_);;
         i = i + 1 == slots_ ? 0 : i + 1) {
        Slot& slot = part.table[i];
        if (!slot.used) {
            if (part.live >= slots_ / 2) {
                ++part.dropped;
                return;
            }
            slot = Slot{rel_id, column_mask, 1, now, kind, true};
            ++part.live;
            return;
        }
        if (slot.Is(kind, rel_id, column_mask)) {
            if (slot.uses != std::numeric_limits<std::uint64_t>::max()) ++slot.uses;
            slot.last_seen = now;
            return;
        }
    }
}

void AccessStats::Forget(catalog::Oid rel_id, std::uint16_t col_pos) noexcept {
    const std::uint64_t bit = catalog::ColumnBit(col_pos);
    if (bit == 0) return;  // a mask names positions below 64 only
    for (Part& part : parts_) {
        const LatchGuard held(&part.latch);
        if (!part.table) continue;
        // One pass, starting just past an empty slot (at most half are
        // live, so one exists): no run of used slots then wraps across the
        // pass's start, so a backward shift only moves a slot from later in
        // the pass into the hole, never behind it. The slot a deletion
        // refilled is looked at again before the pass moves on.
        std::size_t start = 0;
        while (part.table[start].used) ++start;
        for (std::size_t n = 1; n < slots_;) {
            const std::size_t i = (start + n) % slots_;
            const Slot& slot = part.table[i];
            if (!slot.used || slot.rel_id != rel_id || (slot.column_mask & bit) == 0) {
                ++n;
                continue;
            }
            // Backward-shift deletion: every slot after the hole whose
            // home does not lie cyclically in (hole, it] moves into it.
            std::size_t hole = i;
            part.table[hole].used = false;
            for (std::size_t j = hole + 1 == slots_ ? 0 : hole + 1; part.table[j].used;
                 j = j + 1 == slots_ ? 0 : j + 1) {
                const Slot& next = part.table[j];
                const std::size_t home = HomeOf(next.kind, next.rel_id, next.column_mask, slots_);
                const bool stays = hole <= j ? (hole < home && home <= j)
                                             : (hole < home || home <= j);
                if (stays) continue;
                part.table[hole] = next;
                part.table[j].used = false;
                hole = j;
            }
            --part.live;
        }
    }
}

std::vector<AccessShape> AccessStats::Snapshot() const {
    std::vector<AccessShape> rows;
    for (Part& part : parts_) {
        const LatchGuard held(&part.latch);
        if (!part.table) continue;
        rows.reserve(rows.size() + part.live);
        for (std::size_t i = 0; i < slots_; ++i) {
            const Slot& slot = part.table[i];
            if (slot.used) {
                rows.push_back({slot.kind, slot.rel_id, slot.column_mask, slot.uses, slot.last_seen});
            }
        }
    }
    // The merge, with no latch held: one row per key for the instance.
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
    for (Part& part : parts_) {
        const LatchGuard held(&part.latch);
        total += part.dropped;
    }
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
