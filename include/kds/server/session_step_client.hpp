#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/server/step_pipeline.hpp"

// The session side of a remote read (docs/crosscore.md §2, workplan P4c):
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

    // One remote read's state. `done` is the WaitFor flag; `batches` holds
    // the raw STEP_BATCH payloads (header included) so the decoded views
    // a reader takes stay backed for as long as the read is open.
    struct RemoteRead {
        PipelineTag tag{};
        std::uint32_t owner_core = 0;
        bool done = false;
        Status error = Status::OK();
        std::vector<std::vector<std::byte>> batches;
        std::uint64_t rows = 0;
    };

    // Ships `step` to its owner and registers the read. `request_id` is
    // the session core's per-statement sequence (§3 - sequential, never
    // pointer-derived). Refuses what the descriptor codec refuses.
    StatusOr<PipelineTag> Open(const exec::Step& step, std::uint32_t owner_core,
                               std::uint64_t request_id);

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

}  // namespace kds::server
