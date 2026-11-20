#include "kds/stats/refusal_stats.hpp"

#include "kds/base/statistics_scope.hpp"

namespace kds::stats {

std::array<RefusalStats::Part, kStatsParts>& RefusalStats::parts() noexcept {
    static std::array<Part, kStatsParts> p;
    return p;
}

void RefusalStats::Record(std::uint32_t core, std::uint16_t code, std::int64_t now_us) noexcept {
    const StatisticsRecordScope recording("sys.refusal_stats");
    if (code >= kRefusalCodes) return;
    Slot& slot = parts()[core % kStatsParts][code];
    slot.count.add(1);
    slot.last_seen_us.set(static_cast<std::uint64_t>(now_us));
}

std::vector<RefusalCount> RefusalStats::Snapshot() {
    std::vector<RefusalCount> out;
    std::array<Part, kStatsParts>& all = parts();
    for (std::uint32_t core = 0; core < kStatsParts; ++core) {
        for (std::size_t code = 0; code < kRefusalCodes; ++code) {
            const std::uint64_t count = all[core][code].count.get();
            if (count == 0) continue;
            out.push_back({core, static_cast<std::uint16_t>(code), count,
                           static_cast<std::int64_t>(all[core][code].last_seen_us.get())});
        }
    }
    return out;
}

void RefusalStats::ResetForTest() noexcept {
    for (Part& part : parts()) {
        for (Slot& slot : part) {
            slot.count.set(0);
            slot.last_seen_us.set(0);
        }
    }
}

}  // namespace kds::stats
