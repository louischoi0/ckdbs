#include "kds/server/remote_step_service.hpp"

#include <cstring>
#include <string>
#include <utility>

#include "kds/exec/step_vm.hpp"
#include "kds/server/step_descriptor.hpp"
#include "kds/wire/row_codec.hpp"

namespace kds::server {

std::vector<std::byte> EncodeStepOpen(const StepOpenHead& head,
                                      std::span<const std::byte> descriptor) {
    std::vector<std::byte> out(sizeof(head) + descriptor.size());
    std::memcpy(out.data(), &head, sizeof(head));
    std::memcpy(out.data() + sizeof(head), descriptor.data(), descriptor.size());
    return out;
}

RemoteStepServer::Pipeline* RemoteStepServer::Find(const PipelineTag& tag) {
    for (Pipeline& pipe : pipelines_) {
        if (pipe.tag == tag) return &pipe;
    }
    return nullptr;
}

void RemoteStepServer::SendError(const PipelineTag& tag, std::uint32_t session_core,
                                 const Status& status) {
    StepErrorPayload error{};
    error.tag = tag;
    error.status_code = static_cast<std::uint32_t>(status.code());
    error.retryable = status.retryable() ? 1 : 0;
    std::vector<std::byte> bytes;
    EncodePipelinePayload(error, bytes);
    if (Status s = send_(session_core, sched::RingMessageKind::kStepError, std::move(bytes));
        !s.ok() && log_ != nullptr && log_->enabled(LogLevel::kError)) {
        log_->Error("pipeline", "core " + std::to_string(core_id_) +
                                    " could not report a step error: " + s.message());
    }
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pipeline", "core " + std::to_string(core_id_) + " refused step " +
                                    std::to_string(tag.step_id) + ": " + status.message());
    }
}

void RemoteStepServer::OnStepOpen(const sched::MessageHeader& header,
                                  std::span<const std::byte> payload) {
    if (payload.size() < sizeof(StepOpenHead)) {
        // No tag to reply under; log is all there is.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("pipeline", "STEP_OPEN of " + std::to_string(payload.size()) +
                                        " bytes has no head");
        }
        return;
    }
    StepOpenHead head{};
    std::memcpy(&head, payload.data(), sizeof(head));
    const std::uint32_t session = header.src_core;

    auto step = DecodeStepDescriptor(payload.subspan(sizeof(head)));
    if (!step.ok()) {
        SendError(head.tag, session, step.status());
        return;
    }

    // The single-step class: every reference resolves inside this step,
    // and a key is a literal - "produced by an earlier step" has no
    // earlier step here. Multi-step wiring is P4d's.
    if (step.value().key.has_value() &&
        step.value().key->kind == exec::OperandKind::kColumn) {
        SendError(head.tag, session,
                  Status::Unsupported("a probe keyed by another step's column cannot run as a "
                                      "single remote step; multi-step wiring is P4d"));
        return;
    }
    for (const exec::StepPredicate& pred : step.value().residual) {
        if (pred.lhs.up != 0 || (pred.rhs.kind == exec::OperandKind::kColumn &&
                                 pred.rhs.column.up != 0)) {
            SendError(head.tag, session,
                      Status::Unsupported("a residual referencing an enclosing chain cannot "
                                          "run as a single remote step"));
            return;
        }
    }

    auto access = catalog_.InitTableAccess(step.value().rel_oid);
    if (!access.ok()) {
        SendError(head.tag, session, access.status());
        return;
    }
    const catalog::Schema& schema = access.value()->schema;

    // A chain of one: the shipped step becomes slot 0, so its compiled
    // references - written against its slot in the session's chain - are
    // re-slotted to the only slot this chain has.
    exec::StepChain chain;
    exec::Step local = std::move(step.value());
    for (exec::StepPredicate& pred : local.residual) {
        pred.lhs.rel_slot = 0;
        if (pred.rhs.kind == exec::OperandKind::kColumn) pred.rhs.column.rel_slot = 0;
    }
    chain.steps.push_back(std::move(local));

    // Collect-then-stream (see the header). Whole rows in schema order for
    // P4b; the projection narrowing rides with P4c's session side, which
    // is the layer that knows what the statement keeps.
    Pipeline pipe;
    pipe.tag = head.tag;
    pipe.downstream = head.downstream_core;

    wire::RowBatchWriter writer;
    std::vector<parser::AstValue> row(schema.columns.size());
    std::uint32_t seq = 0;
    auto flush = [&] {
        StepBatchHeader batch{};
        batch.tag = head.tag;
        batch.seq = seq++;
        batch.row_count = writer.row_count();
        std::vector<std::byte> rows_bytes = writer.Finish();
        std::vector<std::byte> out(sizeof(batch) + rows_bytes.size());
        std::memcpy(out.data(), &batch, sizeof(batch));
        std::memcpy(out.data() + sizeof(batch), rows_bytes.data(), rows_bytes.size());
        pipe.batches.push_back(std::move(out));
    };

    Status ran = exec::Execute(
        catalog_, store_, chain,
        [&](const exec::ChainFrame& frame) -> StatusOr<storage::VisitControl> {
            for (std::uint16_t pos = 0; pos < schema.columns.size(); ++pos) {
                row[pos] = frame.Get(exec::ColumnRef{0, 0, pos});
            }
            if (Status s = writer.AppendRow(schema, row); !s.ok()) return s;
            if (writer.size_bytes() >= batch_target_ || writer.full()) flush();
            return storage::VisitControl::kContinue;
        });
    if (!ran.ok()) {
        SendError(head.tag, session, ran);
        return;
    }
    if (writer.row_count() > 0) flush();

    pipelines_.push_back(std::move(pipe));
    Drain(pipelines_.back());
}

void RemoteStepServer::Drain(Pipeline& pipe) {
    // By value before anything mutates the vector: `pipe` is a reference
    // into `pipelines_`, and the erase below would leave it dangling while
    // its tag was still being read.
    const PipelineTag tag = pipe.tag;
    const std::uint32_t downstream = pipe.downstream;

    while (pipe.next < pipe.batches.size() && pipe.credit.can_send()) {
        if (Status s = pipe.credit.ConsumeOnSend(); !s.ok()) break;  // unreachable; belt
        if (Status s = send_(downstream, sched::RingMessageKind::kStepBatch,
                             std::move(pipe.batches[pipe.next]));
            !s.ok()) {
            SendError(tag, tag.session_core, s);
            break;
        }
        ++pipe.next;
    }
    if (pipe.next < pipe.batches.size()) return;  // waiting on credit

    // Everything sent: EOF closes the edge. Control, not data - it needs
    // no credit, exactly as CREDIT itself needs none coming back.
    StepEofPayload eof{tag};
    std::vector<std::byte> bytes;
    EncodePipelinePayload(eof, bytes);
    if (Status s = send_(downstream, sched::RingMessageKind::kStepEof, std::move(bytes));
        !s.ok() && log_ != nullptr && log_->enabled(LogLevel::kError)) {
        log_->Error("pipeline", "EOF for step " + std::to_string(tag.step_id) +
                                    " could not be sent: " + s.message());
    }
    for (std::size_t i = 0; i < pipelines_.size(); ++i) {
        if (pipelines_[i].tag == tag) {
            pipelines_.erase(pipelines_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
}

void RemoteStepServer::OnStepCredit(std::span<const std::byte> payload) {
    auto credit = DecodePipelinePayload<StepCreditPayload>(payload);
    if (!credit.ok()) return;  // malformed: logged nowhere useful, dropped
    Pipeline* pipe = Find(credit.value().tag);
    if (pipe == nullptr) return;  // teardown rule: silently discarded
    if (Status s = pipe->credit.Grant(credit.value().credits); !s.ok()) {
        // A grant past the ceiling is a protocol defect on the other side;
        // refusing it here keeps the bound honest and the pipeline alive.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("pipeline", s.message());
        }
        return;
    }
    Drain(*pipe);
}

void RemoteStepServer::OnStepCancel(std::span<const std::byte> payload) {
    auto eof = DecodePipelinePayload<StepEofPayload>(payload);
    if (!eof.ok()) return;
    for (std::size_t i = 0; i < pipelines_.size(); ++i) {
        if (pipelines_[i].tag == eof.value().tag) {
            pipelines_.erase(pipelines_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

}  // namespace kds::server
