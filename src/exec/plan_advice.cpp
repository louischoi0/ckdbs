#include "kds/exec/plan_advice.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

namespace kds::exec {

std::string RatioText(double ratio) {
    char text[32];
    std::snprintf(text, sizeof text, "%.3f", ratio);
    return text;
}

void PlanAdvices::Record(PlanAdvice advice) {
    const LatchGuard hold(&latch_);
    if (rows_.size() >= kMaxShapes && rows_.find(advice.key) == rows_.end()) {
        const auto oldest = std::min_element(rows_.begin(), rows_.end(), [](const auto& a,
                                                                           const auto& b) {
            return a.second.seq < b.second.seq;
        });
        rows_.erase(oldest);
    }
    const CapturedKey key = advice.key;
    rows_[key] = Entry{std::move(advice), next_seq_++};
}

std::vector<PlanAdvice> PlanAdvices::Snapshot() {
    std::vector<const Entry*> entries;
    std::vector<PlanAdvice> out;
    const LatchGuard hold(&latch_);
    for (const auto& [key, entry] : rows_) entries.push_back(&entry);
    std::sort(entries.begin(), entries.end(),
              [](const Entry* a, const Entry* b) { return a->seq < b->seq; });
    for (const Entry* entry : entries) out.push_back(entry->advice);
    return out;
}

}  // namespace kds::exec
