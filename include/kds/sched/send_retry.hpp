#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <span>
#include <utility>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/sched/task.hpp"

// The engine-wide answer to a full ring (docs/inflight/in-progress/workplan-crosscore.md M7,
// docs/spec/sched.md §5).
//
// The rule M7 fixes is short: **the sending task yields and retries. Never
// an error to the client, never a reactor block, never a drop.** Each of
// those three is a way the obvious implementations go wrong -
//
//   - Reporting ResourceExhausted upward turns transient peer backpressure
//     into a failed statement, which is a lie about what happened.
//   - Spinning inside the task blocks the reactor: there is no preemption
//     (sched.md §3), so a task that waits for a peer to drain waits with
//     the whole core.
//   - Dropping is forbidden outright (sched.md §5), and silently at that.
//
// A `SendRetryTask` is the fourth option: it returns `kSuspended`, which
// puts it back on its group's ready queue, and the peer gets drained by the
// *other* core's reactor in the meantime. Progress is the scheduler's
// problem, which is where it belongs.
//
// ---- What this deliberately does not do ---------------------------------
//
// **No backoff, no retry ceiling, no deadline.** sched.md §10 leaves "the
// suspension/retry protocol for ring_full" open, and every one of those
// would be a way of settling it: a ceiling decides what happens to a
// message that cannot be delivered, a backoff decides the fairness policy
// between a stalled sender and the tasks behind it. Both belong with
// crosscore.md §4's credit accounting, which is what actually bounds
// per-request buffering - the ring's own fullness is meant to be rare once
// credits exist. Until then, the plainest form that satisfies M7 is the one
// that pre-commits to nothing.
//
// The one thing a caller must know: a `SendRetryTask` retries forever
// against a peer that never drains. That is correct today, because a peer
// that never drains is a stopped reactor and the statement behind this
// message is not going to complete either way. It stops being correct the
// moment a core can be removed at runtime, which nothing supports.

namespace kds::sched {

// Owns its payload, because the task outlives the call that created it: the
// sender submits this and returns, and the bytes have to survive until a
// later reactor iteration gets them into the ring.
class SendRetryTask final : public Task {
public:
    // `on_done` is invoked exactly once, with the send's final status, when
    // the message is delivered into the ring. Optional - most senders have
    // nothing to do afterwards, and a send that eventually succeeds is the
    // uninteresting case. It reports a *non-retryable* failure (a bad core
    // id, an oversized payload); a full ring is never reported through it,
    // because a full ring is not a result.
    using DoneFn = std::function<void(Status)>;

    SendRetryTask(SchedulingGroup group, RingTransport& transport, const MessageHeader& header,
                  std::span<const std::byte> payload, DoneFn on_done = {})
        : Task(group),
          transport_(&transport),
          header_(header),
          payload_(payload.begin(), payload.end()),
          on_done_(std::move(on_done)) {}

    PollResult Poll() override {
        Status s = transport_->TrySend(header_, std::span<const std::byte>(payload_));
        if (s.code() == StatusCode::kResourceExhausted) {
            // Backpressure, not failure. Back onto the ready queue; the
            // peer's reactor drains in the meantime.
            ++attempts_;
            return PollResult::kSuspended;
        }
        ++attempts_;
        if (on_done_) on_done_(std::move(s));
        return PollResult::kDone;
    }

    // How many times Poll() has run - one on the happy path. Diagnostics
    // and tests; the retry count is not a signal anything acts on.
    std::uint64_t attempts() const noexcept { return attempts_; }

private:
    RingTransport* transport_;
    MessageHeader header_;
    std::vector<std::byte> payload_;
    DoneFn on_done_;
    std::uint64_t attempts_ = 0;
};

// Sends `header` + `payload`, retrying on a full ring, in the scheduling
// group the header designates.
//
// This is the call every cross-core sender should use. Reaching for
// `RingTransport::TrySend()` directly is legal but means owning M7 at that
// call site, and there is no reason for two answers to the same question.
inline std::unique_ptr<SendRetryTask> MakeSendRetryTask(RingTransport& transport,
                                                        const MessageHeader& header,
                                                        std::span<const std::byte> payload,
                                                        SendRetryTask::DoneFn on_done = {}) {
    return std::make_unique<SendRetryTask>(GroupOf(header), transport, header, payload,
                                           std::move(on_done));
}

// Fills the header and submits the retry task, for the ten-odd services
// whose every send is one POD on the `system` group. Each of them had its
// own file-local `SendPod` plus a `Send*Message` wrapper differing in a
// line or two, and the statement-shipping wire would have been the second
// verbatim copy of the first.
//
// `session_core` is a parameter because the services genuinely disagree
// about it: the index build fixes it at 0 (core 0 owns the statement both
// ways), the lease services echo the requester's, and statement shipping
// echoes the arrival core's. It is the one field with no defensible
// default, so it is named at every call site.
//
// **The payload is read straight out of `pod`**, with no intermediate
// buffer: `SendRetryTask` copies into its own storage, and a second copy
// on the way there cost nothing at 128 bytes on a DDL path but costs a
// kilobyte per statement on a shipped one.
template <typename Pod>
void SubmitSendPod(Scheduler& scheduler, RingTransport& transport, std::uint32_t src_core,
                   std::uint32_t dst_core, std::uint32_t session_core, std::uint64_t request_id,
                   RingMessageKind kind, const Pod& pod) {
    static_assert(std::is_trivially_copyable_v<Pod>,
                  "a ring payload is memcpy'd whole; it must be trivially copyable");
    MessageHeader header{};
    header.request_id = request_id;
    header.src_core = src_core;
    header.dst_core = dst_core;
    header.session_core = session_core;
    header.kind = static_cast<std::uint16_t>(kind);
    header.sched_group = static_cast<std::uint16_t>(SchedulingGroup::kSystem);
    scheduler.Submit(MakeSendRetryTask(
        transport, header,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(&pod), sizeof(pod))));
}

}  // namespace kds::sched
