#include "kds/stats/txn_stats.hpp"

#include "kds/base/statistics_scope.hpp"

namespace kds::stats {

const char* TxnEndFieldName(TxnEnd end) noexcept {
    switch (end) {
        case TxnEnd::kCommit:
            return "commits";
        case TxnEnd::kRollback:
            return "rollbacks";
        case TxnEnd::kCeilingRollback:
            return "ceiling_rollbacks";
        case TxnEnd::kCloseRollback:
            return "close_rollbacks";
        case TxnEnd::kAbort:
            return "aborts";
        case TxnEnd::kWaitAbort:
            return "wait_aborts";
    }
    return "unknown";
}

std::array<TxnStats::Part, kStatsParts>& TxnStats::parts() noexcept {
    static std::array<Part, kStatsParts> p;
    return p;
}

void TxnStats::Count(std::uint32_t core, TxnEnd end) noexcept {
    const StatisticsRecordScope recording("a transaction's end count");
    parts()[core % kStatsParts].ends[static_cast<std::size_t>(end)].add(1);
}

void TxnStats::RecordLife(std::uint32_t core, bool read_only, std::uint64_t life_ns) noexcept {
    const StatisticsRecordScope recording("sys.txn_histogram");
    parts()[core % kStatsParts].lives[read_only ? 1 : 0][sched::WaitBucketOf(life_ns)].add(1);
}

std::array<std::uint64_t, kTxnEndCount> TxnStats::CountsOf(std::uint32_t core) noexcept {
    std::array<std::uint64_t, kTxnEndCount> out{};
    const Part& part = parts()[core % kStatsParts];
    for (std::size_t i = 0; i < kTxnEndCount; ++i) out[i] = part.ends[i].get();
    return out;
}

std::vector<TxnLifeBucket> TxnStats::SnapshotLives() {
    std::vector<TxnLifeBucket> out;
    std::array<Part, kStatsParts>& all = parts();
    for (std::uint32_t core = 0; core < kStatsParts; ++core) {
        for (std::size_t kind = 0; kind < 2; ++kind) {
            for (std::size_t k = 0; k < sched::kWaitHistogramBuckets; ++k) {
                const std::uint64_t count = all[core].lives[kind][k].get();
                if (count == 0) continue;
                out.push_back({core, kind == 1, static_cast<std::uint32_t>(k), count});
            }
        }
    }
    return out;
}

void TxnStats::ResetForTest() noexcept {
    for (Part& part : parts()) {
        for (sched::RelaxedU64& end : part.ends) end.set(0);
        for (auto& kind : part.lives) {
            for (sched::RelaxedU64& bucket : kind) bucket.set(0);
        }
    }
}

}  // namespace kds::stats
