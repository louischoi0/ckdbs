#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/server/step_pipeline.hpp"

// The session side of a remote read (docs/spec/crosscore.md §2, workplan P4c):
// opens a pipeline for one shipped step, collects its batches, grants a
// credit back per batch drained, and completes - with rows or with the
// remote error - when EOF or STEP_ERROR arrives. The completion flag is a
// stable address a dispatcher coroutine `co_await WaitFor{...}`s on, which
// is the whole reason the statement path became suspendable (workplan P2,
// sched/coro.hpp).
//
// Grant-on-receive: this client stores a batch and immediately returns one
// credit, because storing *is* its drain - the reply is framed after
// completion. When P4d streams rows onward instead of collecting, the
// grant moves to the true drain point and nothing on the wire changes.
//
// Like RemoteStepServer, the sender is injected so the protocol tests
// without a reactor; CoreRuntime and the Expeditor wire the real ring.

namespace kds::server {

class SessionStepClient {
public:
    using SendFn =
        std::function<Status(std::uint32_t, sched::RingMessageKind, std::vector<std::byte>)>;

    SessionStepClient(std::uint32_t core_id, SendFn send, Logger* log = nullptr) noexcept
        : core_id_(core_id), send_(std::move(send)), log_(log) {}

    // One stage of the statement's pipeline: where a CANCEL must go when
    // the read tears down other than by a clean EOF.
    struct StageAddress {
        PipelineTag tag{};
        std::uint32_t core = 0;
    };

    // One remote read's state. `done` is the WaitFor flag; `batches` holds
    // the raw STEP_BATCH payloads (header included) so the decoded views
    // a reader takes stay backed for as long as the read is open.
    struct RemoteRead {
        PipelineTag tag{};
        std::uint32_t owner_core = 0;
        catalog::Oid rel_oid = 0;  // what the batches are rows of
        bool done = false;
        Status error = Status::OK();
        std::vector<std::vector<std::byte>> batches;
        std::uint64_t rows = 0;
        // Every stage of the statement, upstream first (P4d-4b-3). One
        // entry for a single-step read. The session is the only holder of
        // the whole list, which is why teardown-by-error cancels from
        // here: an error names only the stage that failed, and its peers
        // may be parked anywhere in the chain.
        std::vector<StageAddress> stages;
        // The final edge's row layout, for the typed decode; empty means
        // whole rows of `rel_oid` in schema order (the P4c shape).
        std::vector<catalog::SysColumnRow> output_layout;
        // Rendering facts for the projected form, carried here because
        // the compiled chain dies with the statement frame while the
        // read completes on the reactor.
        std::vector<std::string> column_names;
        std::vector<std::uint32_t> projection_types;
    };

    // A computed two-step pipeline, ready to open (P4d-4b-3): the chained
    // final-stage envelope, where it goes, every stage's address, and the
    // decode/render facts for the final edge.
    struct PipelinePlan {
        std::vector<std::byte> final_open;
        std::uint32_t final_core = 0;
        PipelineTag final_tag{};
        catalog::Oid final_rel_oid = 0;
        std::vector<StageAddress> stages;
        std::vector<catalog::SysColumnRow> output_layout;
        std::vector<std::string> column_names;
        std::vector<std::uint32_t> projection_types;
    };

    // Ships `step` to its owner and registers the read. `request_id` is
    // the session core's per-statement sequence (§3 - sequential, never
    // pointer-derived). Refuses what the descriptor codec refuses.
    StatusOr<PipelineTag> Open(const exec::Step& step, std::uint32_t owner_core,
                               std::uint64_t request_id);

    // Registers the pipeline's read and sends the chained open to the
    // final stage's core - which forwards its enclosed upstream open
    // itself (crosscore.md §2's chained-open rule), so this is the one
    // message the session sends to start the whole chain.
    StatusOr<PipelineTag> OpenPipeline(PipelinePlan plan);

    // Handlers for the three inbound kinds. A tag matching no open read is
    // discarded silently (§3's teardown rule).
    void OnStepBatch(std::span<const std::byte> payload);
    void OnStepEof(std::span<const std::byte> payload);
    void OnStepError(std::span<const std::byte> payload);

    RemoteRead* Find(const PipelineTag& tag);

    // Ends the read: sends STEP_CANCEL when it is still open remotely
    // (error/EOF already closed the remote side otherwise) and drops the
    // state. Every Open is paired with exactly one Close.
    void Close(const PipelineTag& tag);

    std::size_t open_reads() const noexcept { return reads_.size(); }

private:
    std::uint32_t core_id_;
    SendFn send_;
    Logger* log_;
    std::vector<RemoteRead> reads_;
};

// Whether a compiled chain is a shape the two-step pipeline may ship -
// **the eligible class, stated once** (workplan P4d-4b-3, widened by
// 4c). Answered from the chain alone, with no catalog lookup, so a
// caller can ask before it pays to resolve any schema; `Unsupported`
// names which rule declined. `BuildTwoStepPipeline` asks it too, so the
// planner is safe to call on its own.
Status TwoStepPipelineEligible(const exec::StepChain& chain);

// Derives the pipeline plan for an eligible two-step chain - scan feeding
// probe (workplan P4d-4b-3). Everything falls out of the chain's own
// fields: the forwarded layout of edge 0->1 is the unique step-0 columns
// among step 1's key and residual references and the projection's
// slot-0 references; stage 0's output spec is that layout as local
// entries; stage 1's is the projection in order; and step 1's references
// are normalized before encoding - its own columns to (up=0, slot=0),
// step 0's to (up=1, slot=0) with col_pos into the *forwarded* layout -
// so no stage guesses another's slots. Refuses (for the caller to fall
// through to the affinity refusal) anything the descriptor codec
// refuses, and any reference shape outside the eligible class.
StatusOr<SessionStepClient::PipelinePlan> BuildTwoStepPipeline(
    const exec::StepChain& chain, const catalog::Schema& outer_schema,
    const catalog::Schema& inner_schema, std::uint32_t outer_core, std::uint32_t inner_core,
    std::uint32_t session_core, std::uint64_t request_id);

}  // namespace kds::server
