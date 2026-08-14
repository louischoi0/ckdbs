#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/sched/coro.hpp"
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
// **Execution shape: stream-under-credit when a reactor is present**
// (workplan P4d-4a). With a `SubmitFn`, STEP_OPEN submits a producer
// coroutine: the step runs through `exec::ExecuteAsync` with a resume
// gate, seals a batch at the size target, ships it while a credit is
// held, and **parks at the page boundary** - holding no pin and no span
// (P4d-3) - when sealed batches wait on credit. Buffering is therefore
// bounded by the credit ceiling plus what one page can seal, not by the
// relation. A STEP_CREDIT drains the queue and the parked walk resumes
// through its gate; a STEP_CANCEL stops it at the next row or boundary.
//
// Without a `SubmitFn` (tests with no reactor), the P4b shape survives
// as the fallback: collect-then-stream through the synchronous
// `exec::Execute`, drain on credit. The wire protocol is identical in
// both shapes - no batch without a credit, grants bounded by the
// preallocated ceiling, EOF after the last batch ships.
//
// The sender is injected (`SendFn`) so every rule here is testable without
// a reactor; `CoreRuntime` wires it to the real ring through the send-retry
// task. Handlers, drain and the producer all run on the owning core's
// thread - no locks, like everything else on a core (rules.md #3).

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

    // Hands a producer task to this core's reactor. Empty selects the
    // synchronous collect-then-stream fallback (see the header comment).
    using SubmitFn = std::function<void(std::unique_ptr<sched::Task>)>;

    RemoteStepServer(catalog::Catalog& catalog, storage::PageStore& store, std::uint32_t core_id,
                     SendFn send, Logger* log = nullptr,
                     std::size_t batch_target_bytes = kStepBatchTargetBytes,
                     SubmitFn submit = {}) noexcept
        : catalog_(catalog),
          store_(store),
          core_id_(core_id),
          send_(std::move(send)),
          log_(log),
          batch_target_(batch_target_bytes),
          submit_(std::move(submit)) {}

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

    // Sealed-but-unsent batches across every open pipeline. What the
    // bounded-buffering test reads: streaming's whole point is that this
    // stays around one page's seals plus the credit ceiling, where
    // collect-then-stream held the relation.
    std::size_t unsent_batches() const noexcept {
        std::size_t n = 0;
        for (const Pipeline& pipe : pipelines_) n += pipe.batches.size() - pipe.next;
        return n;
    }

private:
    struct Pipeline {
        PipelineTag tag{};
        std::uint32_t downstream = 0;
        EdgeCredit credit;
        std::vector<std::vector<std::byte>> batches;
        std::size_t next = 0;      // first unsent batch
        std::uint32_t seq = 0;     // next batch's per-edge sequence number
        // Streaming state (P4d-4a). While `producing`, Drain must not EOF
        // however empty the queue looks - the walk is parked mid-relation,
        // not finished. `cancelled` is how a CANCEL reaches a live
        // producer: the state must outlive the message handler, so the
        // handler marks it and the producer erases itself at the next
        // sink call or page boundary.
        bool producing = false;
        bool cancelled = false;
    };

    Pipeline* Find(const PipelineTag& tag);
    void Erase(const PipelineTag& tag);
    void Drain(Pipeline& pipe);
    void SendError(const PipelineTag& tag, std::uint32_t session_core, const Status& status);

    // The streaming producer (P4d-4a): one coroutine per open pipeline,
    // owning the writer and the executor run. It re-finds its Pipeline by
    // tag at every touch - the vector reallocates and CANCEL erases, so a
    // held pointer would be the dangling-reference bug the dispatcher's
    // remote read already solved the same way.
    sched::Coro RunProducer(StepOpenHead head, exec::StepChain chain);

    catalog::Catalog& catalog_;
    storage::PageStore& store_;
    std::uint32_t core_id_;
    SendFn send_;
    Logger* log_;
    std::size_t batch_target_;
    SubmitFn submit_;
    std::vector<Pipeline> pipelines_;
};

}  // namespace kds::server
