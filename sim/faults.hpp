#pragma once

// sim/faults.hpp — the seeded fault schedule (bench/workplan-teststrategy
// SIM05). SIM04 kills an instance once, at a seed-chosen op; this widens
// the failure surface to what a real device does *while the workload is
// running*: a write that reports an error, a read that does, a sync that
// fails with committers waiting on it, and a growth refusal.
//
// The schedule is a pure function of (seed, ops, profile, rate): the
// faults, their op indices, and the tear prefixes are all drawn before the
// first statement runs, so a failing run replays exactly and the failure
// can name the fault that provoked it.
//
// **The engine's obligation under injection**, which is what the loop
// asserts: every statement either succeeds or returns a truthful `Status`.
// No crash, no wrong answer, and no silent acceptance of a write that did
// not happen — the oracle applies only what the engine acknowledged, and an
// acknowledgement that came back as an error makes that write's outcome
// *unknown*, never *absent* (sim/oracle.hpp's indeterminate set).
//
// **Errors only, and torn transfers deliberately absent.** The workplan
// lists torn writes here and the memory devices offer `TearNextWrite`, but
// a tear the run then *continues past* is not the failure it looks like:
// it models a device that reported success for a partial transfer and kept
// working, which leaves a hole in the middle of a log whose later records
// all landed. Nothing in `docs/spec/wal.md` is written against that — a torn
// transfer is what the power cut leaves in flight, so the realistic image
// is a partial record at the **tail** with nothing after it, made durable
// by the flush the cut interrupted.
//
// That image needs a device primitive neither memory device has: a crash
// that promotes a *prefix* of the unsynced overlay instead of dropping all
// of it. Injecting a tear and carrying on was the harness's first false
// alarm — it broke the mount three seeds out of five with an LSN past the
// append point, which is the hole, not a defect. The realistic case is
// already pinned where it belongs, at the stream (`tests/wal_stream_test.cpp`,
// "the torn record is the end of the stream"). So: **torn injection waits
// for `Crash(prefix)`**, and when it lands the log half asserts (recovery's
// CRC is what it is for) while the page half is [GATED: FPI] — the cadence
// `docs/inflight/known-gaps.md` records as unbuilt for every page class, which
// leaves a torn *page* unhealable and the mount refusing it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kds/storage/memory_page_device.hpp"
#include "kds/wal/memory_log_device.hpp"

#include "sim/rng.hpp"

namespace kds::sim {

enum class FaultProfile : std::uint8_t {
    kNone = 0,
    kIo = 1,
    // A failed capacity growth alone. `io`'s two log kinds fail-stop the log
    // (BC-S4, `fsync-fail-stop`), so under `io` a run rarely serves long
    // past its first log fault, and what a refused growth
    // leaves for the inserts after it - a split that could not place its
    // new page - is reached only where no stopping fault came first (H9,
    // `SimFaults.AnErroredGrowLeavesTheLeafChainAscending`). A growth
    // refusal is a statement's error, never a stop.
    kGrow = 2,
};

const char* FaultProfileName(FaultProfile profile);
std::optional<FaultProfile> ParseFaultProfile(std::string_view name);

enum class FaultKind : std::uint8_t {
    kPageFailRead = 0,
    kPageFailWrite = 1,
    kPageFailSync = 2,
    kPageFailGrow = 3,
    kLogFailWrite = 4,
    kLogFailSync = 5,
};

const char* FaultKindName(FaultKind kind);
std::optional<FaultKind> ParseFaultKind(std::string_view name);

struct ScheduledFault {
    std::size_t op_index = 0;  // armed immediately before this op
    FaultKind kind = FaultKind::kPageFailRead;

    std::string Describe() const;
};

// Faults sorted by op index. Empty for kNone, which is the default and
// costs the loop one branch.
class FaultSchedule {
public:
    FaultSchedule() = default;

    // `rate` is faults per 1000 ops, expected: the count is exact
    // (ops * rate / 1000, at least one when the rate is nonzero), the
    // positions and kinds are drawn.
    FaultSchedule(Rng rng, std::size_t ops, FaultProfile profile, std::uint32_t rate);

    // The faults armed before `op_index`. Usually empty.
    std::span<const ScheduledFault> At(std::size_t op_index) const;

    const std::vector<ScheduledFault>& all() const noexcept { return faults_; }
    std::size_t size() const noexcept { return faults_.size(); }
    bool empty() const noexcept { return faults_.empty(); }

private:
    std::vector<ScheduledFault> faults_;
};

// Arms one fault on the devices. One-shot: it fires on the next matching
// device operation, which may be this statement's or a later one's — which
// is the honest model, since a page write happens when the store decides
// to write, not when the client asks.
void ArmFault(const ScheduledFault& fault, storage::MemoryPageDevice& pages,
              wal::MemoryLogDevice& log);

// **One fault at a chosen device operation** (BS-S3's I/O sweep,
// `instructions/v3.0.0/workorder-bs-test-hardening.md`): the `n`th operation
// of `kind` the devices see from the iteration's first op on fails, once,
// and nothing else is injected. The sweep raises `n` until the fault no
// longer fires; F5's obligation is asserted at every point, as for a
// scheduled fault. Spelled `kind:n`.
//
// **`kind:n@mount` counts from the reboot's start instead** (BS-A7): the
// mount's own I/O - recovery reads every page it redoes, which a workload
// barely does - and its obligation is the restart's: a mount the fault fails
// leaves a volume the next mount, fault-free, recovers to the oracle; one it
// does not fail is held to the run's whole contract.
struct NthFault {
    FaultKind kind = FaultKind::kPageFailRead;
    std::uint64_t n = 1;  // 1 is the first
    bool at_mount = false;

    std::string Describe() const;
};
std::optional<NthFault> ParseNthFault(std::string_view text);
void ArmNthFault(const NthFault& fault, storage::MemoryPageDevice& pages,
                 wal::MemoryLogDevice& log);

// Injections the two devices have actually consumed. The loop reports it
// beside the armed count: a schedule whose faults never fire is a schedule
// that proved nothing, and that has to be visible rather than inferred.
std::uint64_t InjectionsFired(const storage::MemoryPageDevice& pages,
                              const wal::MemoryLogDevice& log);

}  // namespace kds::sim
