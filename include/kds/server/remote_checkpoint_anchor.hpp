#pragma once

#include <cstdint>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/wal/checkpointer.hpp"

// A checkpoint anchor for a core that does not own the superblock
// (docs/inflight/in-progress/workplan-crosscore.md M5, P2).
//
// The superblock is core 0's - it is page 0, and M5 gives the system core
// every fixed structure. So a checkpoint completing on core 3 cannot write
// its own anchor: it sends one, and core 0 writes it. This class is the
// sending half; `SuperBlockCheckpointAnchor` remains the receiving half and
// is what core 0's handler calls, so there is exactly one piece of code that
// knows how an anchor reaches the page.
//
// ---- Why it is fire-and-forget ------------------------------------------
//
// No reply, no completion, no retry beyond the ring's own.
//
// That is sound because of *when* an anchor is published: only after this
// core's `CHECKPOINT_END` record is already durable (`docs/spec/wal.md` §8-3).
// The anchor is a statement about where recovery may **start**, so losing
// one costs a longer replay next boot and can never cost a wrong answer -
// recovery falls back to the previous anchor, or to the start of the stream.
// `SuperBlockCheckpointAnchor`'s own header makes the same point about why
// Publish() does I/O at all.
//
// One-way is also what keeps P2 free of the question that blocks everything
// else: a reply would need the sending task to suspend and resume, and task
// representation is an open decision (`docs/spec/sched.md` §3). An anchor write is
// the one cross-core operation that genuinely does not need an answer, which
// is why it is the one P2 wires up.
//
// The send goes through `MakeSendRetryTask`, so a momentarily full ring
// yields and retries rather than dropping - silent drop is forbidden
// (sched.md §5) even for a message whose loss would be survivable.
//
// ---- Why the last one is not (PW3b) --------------------------------------
//
// Both halves of that argument fail for the **shutdown** checkpoint. "A
// later checkpoint follows" - none does; the next thing this stream sees
// is a mount, and an anchor lost here is one `checkpoint_interval` of
// replay at that mount, every graceful restart. And "the ring's own retry"
// - after the worker join no reactor runs on either side: a send queued on
// this core's scheduler is never polled, and core 0's `kAnchorWrite`
// handler never runs. Pumping the two stopped reactors by hand was
// considered and rejected: this core's ready queue after its listener
// closed can hold a task whose session is gone (the PW5 review's BUG 3),
// core 0's ring can hold any pending request whose handler would then run
// against peers mid-teardown, and `Expeditor::BroadcastShutdown`'s pump is
// bounded and best-effort by construction - right for a stop message a
// joined core no longer needs, wrong for the one anchor nothing makes up
// for.
//
// So the last anchor goes **direct**: once the workers are joined the
// startup thread owns every core - this one (its `Sync()` already runs
// there) and core 0 (its reactor ran there) - and `RouteDirectly` hands
// this object core 0's own `SuperBlockCheckpointAnchor` to call in place
// of the send. Same receiving code, same page, one thread, no ring:
// `SuperBlockCheckpointAnchor` stays the one piece of code that knows how
// an anchor reaches page 0, and the shutdown checkpoint's `Publish` is
// synchronous and durable when it returns, as core 0's own is.

namespace kds::server {

class RemoteCheckpointAnchor final : public wal::CheckpointAnchor {
public:
    // `transport` and `scheduler` must outlive this anchor. `scheduler` is
    // this core's own - the retry task runs here, not on core 0.
    RemoteCheckpointAnchor(sched::RingTransport& transport, sched::Scheduler& scheduler,
                           std::uint32_t core_id, std::uint32_t system_core = 0) noexcept
        : transport_(transport),
          scheduler_(scheduler),
          core_id_(core_id),
          system_core_(system_core) {}

    void SetLogger(Logger* log) noexcept { log_ = log; }

    // Submits the anchor to the system core. Returns OK once the send task
    // is queued, **not** once the anchor is durable - see the header. A
    // caller that needs the latter does not exist and, per wal.md §8-3,
    // should not.
    Status Publish(const wal::CheckpointAnchorRecord& anchor) override;

    // Anchors sent over the ring. The counterpart of
    // SuperBlockCheckpointAnchor's publishes(), and what a test asserts
    // against on the sending side. A direct publish (below) is not a send
    // and is not counted here - the receiver counts it.
    std::uint64_t sends() const noexcept { return sends_; }

    // PW3b, the header's last section: from here on Publish() calls
    // `system_anchor` in place of the send. For the shutdown path only -
    // set on the startup thread after this core's worker is joined, when
    // no reactor runs on either side of the ring. `system_anchor` must
    // outlive this object; null restores the send.
    void RouteDirectly(wal::CheckpointAnchor* system_anchor) noexcept {
        direct_ = system_anchor;
    }

private:
    sched::RingTransport& transport_;
    sched::Scheduler& scheduler_;
    std::uint32_t core_id_;
    std::uint32_t system_core_;
    Logger* log_ = nullptr;
    std::uint64_t sends_ = 0;
    wal::CheckpointAnchor* direct_ = nullptr;  // PW3b, RouteDirectly
};

// The wire form of an anchor write. POD, like every ring payload, and under
// ring_message.hpp's exception to the on-disk layout rules: it never leaves
// the process.
//
// It carries `core_id` explicitly rather than letting core 0 read
// `header.src_core`, because the two are not the same fact. The anchor
// belongs to a *WAL stream*, and while today a stream and a core are one to
// one, `wal.md` §3 leaves stream reassignment open - so the slot this anchor
// names is the sender's business, not the transport's.
struct AnchorWritePayload {
    std::uint64_t checkpoint_lsn;
    std::uint64_t redo_start_lsn;
    std::uint64_t durable_lsn;
    std::uint64_t segment_no;
    std::uint32_t core_id;
    std::uint32_t reserved;  // written 0; keeps the struct padding-free
};

static_assert(sizeof(AnchorWritePayload) == 40);

}  // namespace kds::server
