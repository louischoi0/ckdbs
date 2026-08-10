#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/server/step_pipeline.hpp"
#include "kds/storage/page_store.hpp"

// The remote step server (docs/crosscore.md §2-§4, workplan P4b): an owning
// core takes a STEP_OPEN, executes the described step against its **local**
// relation state, and streams STEP_BATCHes to the downstream core under
// credit, EOF at the end, ERROR with the code and the retryable bit on any
// failure. Single-step chains only in P4b - the shippable class the
// descriptor codec already enforces, narrowed further here to what a lone
// step can mean: every column reference resolves within the step (`up == 0`)
// and a key must be a literal, because "a value produced by an earlier
// step" has no earlier step to come from.
//
// **Execution shape: collect-then-stream, deliberately.** The step runs to
// completion through the synchronous executor (`exec::Execute` - the same
// entry, the same visibility, the same budget the local path uses), its
// batches queue, and an event-driven drain sends while credits hold and
// resumes when a STEP_CREDIT arrives. The wire protocol is exactly §4's -
// no batch without a credit, grants bounded by the preallocated ceiling -
// and only the *internal buffering* differs from the final form:
// per-batch suspension inside the walk is P4d's executor conversion, and
// this service's send path does not change when it lands.
//
// The sender is injected (`SendFn`) so every rule here is testable without
// a reactor; `CoreRuntime` wires it to the real ring through the send-retry
// task. Handlers and drain all run on the owning core's thread - no locks,
// like everything else on a core (rules.md #3).

namespace kds::server {

// The STEP_OPEN envelope: the tag, where the output goes, then the step
// descriptor (step_descriptor.hpp) as the remainder of the payload.
struct StepOpenHead {
    PipelineTag tag{};
    std::uint32_t downstream_core = 0;
    std::uint32_t reserved = 0;  // keeps the head padding-free at 24 bytes
};
static_assert(sizeof(StepOpenHead) == 24);

std::vector<std::byte> EncodeStepOpen(const StepOpenHead& head,
                                      std::span<const std::byte> descriptor);

class RemoteStepServer {
public:
    // `send(dst_core, kind, payload)` must deliver or report; it never
    // blocks (the real one submits a send-retry task).
    using SendFn =
        std::function<Status(std::uint32_t, sched::RingMessageKind, std::vector<std::byte>)>;

    RemoteStepServer(catalog::Catalog& catalog, storage::PageStore& store, std::uint32_t core_id,
                     SendFn send, Logger* log = nullptr,
                     std::size_t batch_target_bytes = kStepBatchTargetBytes) noexcept
        : catalog_(catalog),
          store_(store),
          core_id_(core_id),
          send_(std::move(send)),
          log_(log),
          batch_target_(batch_target_bytes) {}

    // The kStepOpen handler: decode, validate the single-step class,
    // execute, queue, drain. A failure at any point answers STEP_ERROR to
    // the session core and opens nothing.
    void OnStepOpen(const sched::MessageHeader& header, std::span<const std::byte> payload);

    // The kStepCredit handler: grants and resumes the drain. A tag
    // matching no live pipeline is discarded silently - §3's teardown
    // rule, correctness rather than an error.
    void OnStepCredit(std::span<const std::byte> payload);

    // The kStepCancel handler: drops the pipeline whole, sends nothing.
    void OnStepCancel(std::span<const std::byte> payload);

    std::size_t open_pipelines() const noexcept { return pipelines_.size(); }

private:
    struct Pipeline {
        PipelineTag tag{};
        std::uint32_t downstream = 0;
        EdgeCredit credit;
        std::vector<std::vector<std::byte>> batches;
        std::size_t next = 0;  // first unsent batch
    };

    Pipeline* Find(const PipelineTag& tag);
    void Drain(Pipeline& pipe);
    void SendError(const PipelineTag& tag, std::uint32_t session_core, const Status& status);

    catalog::Catalog& catalog_;
    storage::PageStore& store_;
    std::uint32_t core_id_;
    SendFn send_;
    Logger* log_;
    std::size_t batch_target_;
    std::vector<Pipeline> pipelines_;
};

}  // namespace kds::server
