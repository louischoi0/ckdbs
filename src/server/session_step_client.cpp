#include "kds/server/session_step_client.hpp"

#include <cstring>
#include <string>
#include <utility>

#include "kds/server/remote_step_service.hpp"
#include "kds/server/step_descriptor.hpp"

namespace kds::server {

StatusOr<PipelineTag> SessionStepClient::Open(const exec::Step& step, std::uint32_t owner_core,
                                              std::uint64_t request_id) {
    auto descriptor = EncodeStepDescriptor(step);
    if (!descriptor.ok()) return descriptor.status();

    StepOpenHead head{};
    head.tag = PipelineTag{request_id, core_id_, step.step_id};
    head.downstream_core = core_id_;

    // The read registers **before** the open is sent: replies are matched
    // by tag and an unmatched tag is silently discarded (§3's teardown
    // rule), so state that arrives after the message that generates
    // replies is state that never hears them. The in-process loopback
    // test is what catches this ordering - a real ring cannot reply
    // within the send call, which is exactly why the rule must not lean
    // on that timing.
    RemoteRead read;
    read.tag = head.tag;
    read.owner_core = owner_core;
    read.rel_oid = step.rel_oid;
    reads_.push_back(std::move(read));

    if (Status s = send_(owner_core, sched::RingMessageKind::kStepOpen,
                         EncodeStepOpen(head, descriptor.value()));
        !s.ok()) {
        // By tag, not pop_back: a send that partially processed before
        // failing may have already grown or completed other state.
        Close(head.tag);
        return s;
    }
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pipeline", "core " + std::to_string(core_id_) + " opened step " +
                                    std::to_string(step.step_id) + " on core " +
                                    std::to_string(owner_core));
    }
    return head.tag;
}

SessionStepClient::RemoteRead* SessionStepClient::Find(const PipelineTag& tag) {
    for (RemoteRead& read : reads_) {
        if (read.tag == tag) return &read;
    }
    return nullptr;
}

void SessionStepClient::OnStepBatch(std::span<const std::byte> payload) {
    std::span<const std::byte> rows;
    auto header = DecodeStepBatchHeader(payload, rows);
    if (!header.ok()) return;
    RemoteRead* read = Find(header.value().tag);
    if (read == nullptr) return;  // torn down; §3's silent discard

    read->rows += header.value().row_count;
    read->batches.emplace_back(payload.begin(), payload.end());

    // Grant-on-receive: storing is this client's drain (header note).
    StepCreditPayload credit{read->tag, 1};
    std::vector<std::byte> bytes;
    EncodePipelinePayload(credit, bytes);
    if (Status s = send_(read->owner_core, sched::RingMessageKind::kStepCredit,
                         std::move(bytes));
        !s.ok() && log_ != nullptr && log_->enabled(LogLevel::kError)) {
        log_->Error("pipeline", "credit for step " + std::to_string(read->tag.step_id) +
                                    " could not be sent: " + s.message());
    }
}

void SessionStepClient::OnStepEof(std::span<const std::byte> payload) {
    auto eof = DecodePipelinePayload<StepEofPayload>(payload);
    if (!eof.ok()) return;
    RemoteRead* read = Find(eof.value().tag);
    if (read == nullptr) return;
    read->done = true;
}

void SessionStepClient::OnStepError(std::span<const std::byte> payload) {
    auto error = DecodePipelinePayload<StepErrorPayload>(payload);
    if (!error.ok()) return;
    RemoteRead* read = Find(error.value().tag);
    if (read == nullptr) return;
    // The remote code arrives as its enum value; the message is generic
    // because messages do not travel (a Status string on the wire would be
    // a second error format). The factory switch keeps the code faithful -
    // Status's constructor is deliberately private - and an unknown code
    // degrades to IoError rather than being trusted.
    const auto code = static_cast<StatusCode>(error.value().status_code);
    std::string msg = "remote step failed on its owning core";
    switch (code) {
        case StatusCode::kTxnConflict: read->error = Status::TxnConflict(std::move(msg)); break;
        case StatusCode::kNotFound: read->error = Status::NotFound(std::move(msg)); break;
        case StatusCode::kUnsupported: read->error = Status::Unsupported(std::move(msg)); break;
        case StatusCode::kInvalidArgument:
            read->error = Status::InvalidArgument(std::move(msg));
            break;
        case StatusCode::kResourceExhausted:
            read->error = Status::ResourceExhausted(std::move(msg));
            break;
        case StatusCode::kOutOfRange: read->error = Status::OutOfRange(std::move(msg)); break;
        case StatusCode::kCorruption: read->error = Status::Corruption(std::move(msg)); break;
        default: read->error = Status::IoError(std::move(msg)); break;
    }
    read->done = true;
}

void SessionStepClient::Close(const PipelineTag& tag) {
    for (std::size_t i = 0; i < reads_.size(); ++i) {
        if (!(reads_[i].tag == tag)) continue;
        if (!reads_[i].done) {
            // Still producing remotely: cancel so the owner drops its
            // queue rather than parking on credits nobody will grant.
            StepEofPayload cancel{tag};
            std::vector<std::byte> bytes;
            EncodePipelinePayload(cancel, bytes);
            (void)send_(reads_[i].owner_core, sched::RingMessageKind::kStepCancel,
                        std::move(bytes));
        }
        reads_.erase(reads_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

}  // namespace kds::server
