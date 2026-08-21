#include "sim/faults.hpp"

#include <algorithm>

namespace kds::sim {

const char* FaultProfileName(FaultProfile profile) {
    switch (profile) {
        case FaultProfile::kNone: return "none";
        case FaultProfile::kIo: return "io";
    }
    return "unknown";
}

const char* FaultKindName(FaultKind kind) {
    switch (kind) {
        case FaultKind::kPageFailRead: return "page-fail-read";
        case FaultKind::kPageFailWrite: return "page-fail-write";
        case FaultKind::kPageFailSync: return "page-fail-sync";
        case FaultKind::kPageFailGrow: return "page-fail-grow";
        case FaultKind::kLogFailWrite: return "log-fail-write";
        case FaultKind::kLogFailSync: return "log-fail-sync";
    }
    return "unknown";
}

std::optional<FaultProfile> ParseFaultProfile(std::string_view name) {
    for (const FaultProfile profile : {FaultProfile::kNone, FaultProfile::kIo}) {
        if (name == FaultProfileName(profile)) return profile;
    }
    return std::nullopt;
}

std::optional<FaultKind> ParseFaultKind(std::string_view name) {
    for (const FaultKind kind :
         {FaultKind::kPageFailRead, FaultKind::kPageFailWrite, FaultKind::kPageFailSync,
          FaultKind::kPageFailGrow, FaultKind::kLogFailWrite, FaultKind::kLogFailSync}) {
        if (name == FaultKindName(kind)) return kind;
    }
    return std::nullopt;
}

std::string ScheduledFault::Describe() const {
    return "op " + std::to_string(op_index) + " " + FaultKindName(kind);
}

namespace {

// Weights, not a uniform draw: the two operations a workload performs
// constantly are page reads and page writes, and a fault surface that
// spends an eighth of its budget on capacity growth would be testing the
// rarest path as hard as the hottest one. The numbers are a shape, not a
// measurement — what matters is that no kind is starved.
struct WeightedKind {
    FaultKind kind;
    std::uint32_t weight;
};

constexpr WeightedKind kIoKinds[] = {
    {FaultKind::kPageFailRead, 20}, {FaultKind::kPageFailWrite, 20},
    {FaultKind::kPageFailSync, 15}, {FaultKind::kPageFailGrow, 5},
    {FaultKind::kLogFailWrite, 15}, {FaultKind::kLogFailSync, 15},
};

FaultKind DrawKind(Rng& rng) {
    std::uint32_t total = 0;
    for (const WeightedKind& entry : kIoKinds) total += entry.weight;

    std::uint64_t roll = rng.Below(total);
    for (const WeightedKind& entry : kIoKinds) {
        if (roll < entry.weight) return entry.kind;
        roll -= entry.weight;
    }
    return kIoKinds[0].kind;  // unreachable: the roll is below the total
}

}  // namespace

FaultSchedule::FaultSchedule(Rng rng, std::size_t ops, FaultProfile profile,
                             std::uint32_t rate) {
    if (profile == FaultProfile::kNone || ops == 0 || rate == 0) return;

    // At least one: a run short enough to round down to zero faults would
    // report itself as a fault run and inject nothing.
    std::size_t count = ops * rate / 1000;
    if (count == 0) count = 1;

    faults_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        ScheduledFault fault;
        fault.op_index = static_cast<std::size_t>(rng.Below(ops));
        fault.kind = DrawKind(rng);
        faults_.push_back(fault);
    }
    std::sort(faults_.begin(), faults_.end(),
              [](const ScheduledFault& a, const ScheduledFault& b) {
                  return a.op_index < b.op_index;
              });
}

std::span<const ScheduledFault> FaultSchedule::At(std::size_t op_index) const {
    const auto first = std::lower_bound(faults_.begin(), faults_.end(), op_index,
                                        [](const ScheduledFault& f, std::size_t at) {
                                            return f.op_index < at;
                                        });
    const auto last = std::upper_bound(first, faults_.end(), op_index,
                                       [](std::size_t at, const ScheduledFault& f) {
                                           return at < f.op_index;
                                       });
    return {faults_.data() + (first - faults_.begin()),
            static_cast<std::size_t>(last - first)};
}

void ArmFault(const ScheduledFault& fault, storage::MemoryPageDevice& pages,
              wal::MemoryLogDevice& log) {
    const Status injected =
        Status::IoError(std::string("injected fault: ") + FaultKindName(fault.kind));
    switch (fault.kind) {
        case FaultKind::kPageFailRead: pages.FailNextRead(injected); break;
        case FaultKind::kPageFailWrite: pages.FailNextWrite(injected); break;
        case FaultKind::kPageFailSync: pages.FailNextSync(injected); break;
        case FaultKind::kPageFailGrow: pages.FailNextGrow(injected); break;
        case FaultKind::kLogFailWrite: log.FailNextWrite(injected); break;
        case FaultKind::kLogFailSync: log.FailNextSync(injected); break;
    }
}

std::uint64_t InjectionsFired(const storage::MemoryPageDevice& pages,
                              const wal::MemoryLogDevice& log) {
    return pages.stats().injections_fired + log.stats().injections_fired;
}

}  // namespace kds::sim
