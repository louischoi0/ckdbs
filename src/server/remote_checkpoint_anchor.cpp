#include "kds/server/remote_checkpoint_anchor.hpp"

#include <cstring>
#include <span>
#include <string>

#include "kds/sched/send_retry.hpp"

namespace kds::server {

Status RemoteCheckpointAnchor::Publish(const wal::CheckpointAnchorRecord& anchor) {
    // The shutdown route (the header's last section): no reactor on either
    // side, so the startup thread writes page 0 itself through core 0's
    // anchor. Synchronous, and durable when it returns.
    if (direct_ != nullptr) return direct_->Publish(anchor);

    AnchorWritePayload payload{};
    payload.checkpoint_lsn = anchor.checkpoint_lsn;
    payload.redo_start_lsn = anchor.redo_start_lsn;
    payload.durable_lsn = anchor.durable_lsn;
    payload.segment_no = anchor.segment_no;
    payload.core_id = anchor.core_id;
    payload.reserved = 0;

    sched::MessageHeader header{};
    header.src_core = core_id_;
    header.dst_core = system_core_;
    header.session_core = core_id_;
    header.request_id = 0;  // belongs to no statement
    header.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kAnchorWrite);
    // `system`, not `foreground`: publishing an anchor is checkpoint
    // housekeeping (sched.md §4), and it must not compete with core 0's OLTP
    // work. crosscore.md CC6 puts *step* traffic in foreground for the
    // opposite reason.
    header.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);

    std::byte bytes[sizeof(AnchorWritePayload)];
    std::memcpy(bytes, &payload, sizeof(payload));

    scheduler_.Submit(sched::MakeSendRetryTask(
        transport_, header, std::span<const std::byte>(bytes, sizeof(bytes)),
        [log = log_, core = core_id_](Status s) {
            if (!s.ok() && log != nullptr && log->enabled(LogLevel::kError)) {
                // Nothing upstream to return this to - Publish() has long
                // since returned, by design. A lost anchor costs a longer
                // replay and never an answer (see the header), so the log is
                // both the right place for it and the only one.
                log->Error("checkpoint", "core " + std::to_string(core) +
                                             ": anchor write to the system core failed: " +
                                             s.message());
            }
        }));

    ++sends_;
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("checkpoint", "core " + std::to_string(core_id_) +
                                      ": anchor queued for the system core, redo_start=" +
                                      std::to_string(anchor.redo_start_lsn));
    }
    return Status::OK();
}

}  // namespace kds::server
