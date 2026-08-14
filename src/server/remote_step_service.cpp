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

    // Whole rows in schema order for P4b; the projection narrowing rides
    // with the session side, which is the layer that knows what the
    // statement keeps.
    Pipeline pipe;
    pipe.tag = head.tag;
    pipe.downstream = head.downstream_core;

    // The streaming shape (P4d-4a, see the header): state first, then the
    // producer task. The pipeline entry must exist before the task can
    // run, and the task re-finds it by tag - never through a pointer into
    // the vector.
    if (submit_) {
        pipe.producing = true;
        pipelines_.push_back(std::move(pipe));
        submit_(sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                    RunProducer(head, std::move(chain))));
        return;
    }

    // Collect-then-stream, the reactorless fallback (see the header).
    wire::RowBatchWriter writer;
    std::vector<parser::AstValue> row(schema.columns.size());
    auto flush = [&] {
        StepBatchHeader batch{};
        batch.tag = head.tag;
        batch.seq = pipe.seq++;
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

sched::Coro RemoteStepServer::RunProducer(StepOpenHead head, exec::StepChain chain) {
    const PipelineTag tag = head.tag;

    // Borrowed for the statement, exactly as the executor's own Bind
    // borrows it - which means the same exposure across suspensions: DDL
    // against this relation while a producer is parked would invalidate
    // both borrows alike. Recorded as an open hazard with P4d-4b in the
    // workplan; the affinity rules make it unreachable from a client
    // today.
    auto access = catalog_.InitTableAccess(chain.steps[0].rel_oid);
    if (!access.ok()) {
        SendError(tag, tag.session_core, access.status());
        Erase(tag);
        co_return access.status();
    }
    const catalog::Schema& schema = access.value()->schema;

    wire::RowBatchWriter writer;
    std::vector<parser::AstValue> row(schema.columns.size());

    // Seals the writer's rows into a batch, queues it, and ships as far
    // as credit allows - synchronously, so a credit already in hand costs
    // no park at all.
    auto seal = [&]() {
        Pipeline* pipe = Find(tag);
        if (pipe == nullptr || pipe->cancelled) return;
        StepBatchHeader batch{};
        batch.tag = tag;
        batch.seq = pipe->seq++;
        batch.row_count = writer.row_count();
        std::vector<std::byte> rows_bytes = writer.Finish();
        std::vector<std::byte> out(sizeof(batch) + rows_bytes.size());
        std::memcpy(out.data(), &batch, sizeof(batch));
        std::memcpy(out.data() + sizeof(batch), rows_bytes.data(), rows_bytes.size());
        pipe->batches.push_back(std::move(out));
        Drain(*pipe);
    };

    // The walk parks at a page boundary while a sealed batch waits on
    // credit; anything that ends the wait for good (teardown, cancel)
    // opens the gate so the sink can stop the walk. The predicate lives
    // in this frame, which outlives every poll that reads it (WaitUntil's
    // lifetime rule).
    const std::function<bool()> gate = [this, tag] {
        Pipeline* pipe = Find(tag);
        return pipe == nullptr || pipe->cancelled || pipe->next == pipe->batches.size() ||
               pipe->credit.can_send();
    };

    Status ran = co_await exec::ExecuteAsync(
        catalog_, store_, chain,
        [&](const exec::ChainFrame& frame) -> StatusOr<storage::VisitControl> {
            Pipeline* pipe = Find(tag);
            if (pipe == nullptr || pipe->cancelled) return storage::VisitControl::kStop;
            for (std::uint16_t pos = 0; pos < schema.columns.size(); ++pos) {
                row[pos] = frame.Get(exec::ColumnRef{0, 0, pos});
            }
            if (Status s = writer.AppendRow(schema, row); !s.ok()) return s;
            if (writer.size_bytes() >= batch_target_ || writer.full()) seal();
            return storage::VisitControl::kContinue;
        },
        /*stats=*/nullptr, exec::Budget(), /*trail=*/nullptr, /*replay=*/nullptr,
        /*cabins=*/nullptr, /*snapshot=*/nullptr, /*indexes=*/true, &gate);

    Pipeline* pipe = Find(tag);
    if (pipe == nullptr) co_return Status::OK();  // torn down mid-run; nothing to say
    if (pipe->cancelled) {
        Erase(tag);
        co_return Status::OK();
    }
    if (!ran.ok()) {
        SendError(tag, tag.session_core, ran);
        Erase(tag);
        co_return ran;
    }
    if (writer.row_count() > 0) seal();

    // Production is over; what remains is the queue. Drain EOFs and
    // erases if everything has shipped, and otherwise the next credit's
    // drain does - the producer does not park for it, because nothing it
    // still holds is needed to finish.
    if (Pipeline* done = Find(tag); done != nullptr) {
        done->producing = false;
        Drain(*done);
    }
    co_return Status::OK();
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
    // Still filling (the producer's walk is parked mid-relation, not
    // finished) or still waiting on credit: the edge stays open either
    // way, and the next seal or grant re-enters here.
    if (pipe.producing || pipe.next < pipe.batches.size()) return;

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
    Erase(tag);
}

void RemoteStepServer::Erase(const PipelineTag& tag) {
    for (std::size_t i = 0; i < pipelines_.size(); ++i) {
        if (pipelines_[i].tag == tag) {
            pipelines_.erase(pipelines_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
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
    Pipeline* pipe = Find(eof.value().tag);
    if (pipe == nullptr) return;  // teardown rule: silently discarded
    if (pipe->producing) {
        // A live producer owns its own teardown: the handler cannot erase
        // state the parked walk will re-find, so it marks, and the
        // producer stops at its next row or page boundary and erases.
        pipe->cancelled = true;
        return;
    }
    Erase(eof.value().tag);
}

}  // namespace kds::server
