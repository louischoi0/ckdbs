#include "kds/server/session_step_client.hpp"

#include <algorithm>
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
    read.stages.push_back(StageAddress{head.tag, owner_core});
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

StatusOr<PipelineTag> SessionStepClient::OpenPipeline(PipelinePlan plan) {
    const PipelineTag tag = plan.final_tag;
    const std::uint32_t final_core = plan.final_core;

    // Registered before the send, exactly as Open() registers - and the
    // reason is one message stronger here: the chained open fans out into
    // stage-to-stage traffic the session never sees, so the first reply
    // can be arbitrarily removed from the send that caused it.
    RemoteRead read;
    read.tag = tag;
    read.owner_core = final_core;
    read.rel_oid = plan.final_rel_oid;
    read.stages = std::move(plan.stages);
    read.output_layout = std::move(plan.output_layout);
    read.column_names = std::move(plan.column_names);
    read.projection_types = std::move(plan.projection_types);
    reads_.push_back(std::move(read));

    if (Status s = send_(final_core, sched::RingMessageKind::kStepOpen,
                         std::move(plan.final_open));
        !s.ok()) {
        Close(tag);
        return s;
    }
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pipeline", "core " + std::to_string(core_id_) +
                                    " opened a pipeline ending at step " +
                                    std::to_string(tag.step_id) + " on core " +
                                    std::to_string(final_core));
    }
    return tag;
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
    // By statement, not by exact tag: a pipeline's failure arrives under
    // the *failing stage's* tag while the read is registered under the
    // final stage's, and `request_id` identifies the statement (§3 -
    // sequential per session core). Data stays exact-tag - only the final
    // edge carries the session's rows - but an error anywhere is the
    // statement's error.
    RemoteRead* read = nullptr;
    for (RemoteRead& r : reads_) {
        if (r.tag.request_id == error.value().tag.request_id &&
            r.tag.session_core == error.value().tag.session_core) {
            read = &r;
            break;
        }
    }
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
        // Anything but a clean EOF may leave stages live anywhere in the
        // chain: an error names only the stage that failed, and its peers
        // may be parked - a leaf's failure leaves its consumer waiting on
        // input forever, and the consumer cannot know (P4d-4b-3). The
        // session holds the whole stage list, so it cancels every stage;
        // one already torn down discards the cancel silently (§3).
        if (!reads_[i].done || !reads_[i].error.ok()) {
            for (const StageAddress& stage : reads_[i].stages) {
                StepEofPayload cancel{stage.tag};
                std::vector<std::byte> bytes;
                EncodePipelinePayload(cancel, bytes);
                (void)send_(stage.core, sched::RingMessageKind::kStepCancel,
                            std::move(bytes));
            }
        }
        reads_.erase(reads_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

StatusOr<SessionStepClient::PipelinePlan> BuildTwoStepPipeline(
    const exec::StepChain& chain, const catalog::Schema& outer_schema,
    const catalog::Schema& inner_schema, std::uint32_t outer_core, std::uint32_t inner_core,
    std::uint32_t session_core, std::uint64_t request_id) {
    if (chain.steps.size() != 2) {
        return Status::InvalidArgument("a two-step pipeline plans exactly two steps");
    }
    const exec::Step& outer = chain.steps[0];
    const exec::Step& inner = chain.steps[1];

    // ---- The forwarded layout of edge 0->1 (fact 3) ---------------------
    //
    // The unique outer columns any consumer of the edge reads: the probe
    // key, the residuals attached to the inner step, and the projection.
    // Ascending col_pos - deterministic, and both edge ends receive the
    // same list, the upstream to encode, the downstream to decode.
    std::vector<std::uint16_t> fwd;
    Status noted = Status::OK();
    auto note = [&](const exec::ColumnRef& ref) {
        if (ref.up != 0) {
            // Nothing at a statement's top level references outward; a
            // ref that does is a compiler defect, not a plannable shape.
            noted = Status::InvalidArgument("a top-level chain reference cannot point outward");
            return;
        }
        if (ref.rel_slot == 0) fwd.push_back(ref.col_pos);
    };
    if (inner.key.has_value() && inner.key->kind == exec::OperandKind::kColumn) {
        note(inner.key->column);
    }
    for (const exec::StepPredicate& pred : inner.residual) {
        note(pred.lhs);
        if (pred.rhs.kind == exec::OperandKind::kColumn) note(pred.rhs.column);
    }
    for (const exec::ColumnRef& ref : chain.projection) note(ref);
    if (!noted.ok()) return noted;
    std::sort(fwd.begin(), fwd.end());
    fwd.erase(std::unique(fwd.begin(), fwd.end()), fwd.end());
    for (std::uint16_t pos : fwd) {
        if (pos >= outer_schema.columns.size()) {
            return Status::InvalidArgument("a forwarded column sits past the outer schema");
        }
    }
    if (fwd.empty()) {
        // Unreachable for the probe class - the key alone forwards one
        // column - kept as a refusal because a forwarding edge with no
        // columns serves nobody (the stage-side rule, applied at plan).
        return Status::InvalidArgument("the edge would forward no columns");
    }
    auto fwd_index = [&](std::uint16_t col_pos) {
        return static_cast<std::uint16_t>(
            std::lower_bound(fwd.begin(), fwd.end(), col_pos) - fwd.begin());
    };

    // ---- Stage 1, normalized (fact 4) -----------------------------------
    //
    // The shipped inner step's references are rewritten to the frame shape
    // the consuming stage runs under: its own columns at (up=0, slot=0) -
    // it executes as a chain of one - and the outer row's at (up=1,
    // slot=0) with col_pos into the *forwarded* layout, read through the
    // one-slot parent frame RunConsumer fills per input row.
    exec::Step shipped = inner;
    auto normalize = [&](exec::ColumnRef& ref) -> Status {
        if (ref.up != 0 || ref.rel_slot > 1) {
            return Status::InvalidArgument("a reference outside the two-step chain");
        }
        if (ref.rel_slot == 0) {
            ref = exec::ColumnRef{1, 0, fwd_index(ref.col_pos)};
        } else {
            ref.rel_slot = 0;
        }
        return Status::OK();
    };
    if (shipped.key.has_value() && shipped.key->kind == exec::OperandKind::kColumn) {
        if (Status s = normalize(shipped.key->column); !s.ok()) return s;
    }
    for (exec::StepPredicate& pred : shipped.residual) {
        if (Status s = normalize(pred.lhs); !s.ok()) return s;
        if (pred.rhs.kind == exec::OperandKind::kColumn) {
            if (Status s = normalize(pred.rhs.column); !s.ok()) return s;
        }
    }

    // ---- The output specs, both decided here (the stages never guess) ---
    std::vector<StepOutputColumn> leaf_output;
    leaf_output.reserve(fwd.size());
    for (std::uint16_t pos : fwd) {
        leaf_output.push_back(StepOutputColumn{/*from_upstream=*/0, pos});
    }
    std::vector<StepOutputColumn> final_output;
    final_output.reserve(chain.projection.size());
    SessionStepClient::PipelinePlan plan;
    plan.output_layout.reserve(chain.projection.size());
    for (const exec::ColumnRef& ref : chain.projection) {
        if (ref.rel_slot == 0) {
            final_output.push_back(StepOutputColumn{1, fwd_index(ref.col_pos)});
            plan.output_layout.push_back(outer_schema.columns[ref.col_pos]);
        } else {
            if (ref.col_pos >= inner_schema.columns.size()) {
                return Status::InvalidArgument("a projected column sits past the inner schema");
            }
            final_output.push_back(StepOutputColumn{0, ref.col_pos});
            plan.output_layout.push_back(inner_schema.columns[ref.col_pos]);
        }
    }

    // ---- The two opens, the leaf's enclosed in the final's (fact 1) -----
    auto leaf_descriptor = EncodeStepDescriptor(outer);
    if (!leaf_descriptor.ok()) return leaf_descriptor.status();
    auto final_descriptor = EncodeStepDescriptor(shipped);
    if (!final_descriptor.ok()) return final_descriptor.status();

    StepOpenHead leaf_head{};
    leaf_head.tag = PipelineTag{request_id, session_core, outer.step_id};
    leaf_head.downstream_core = inner_core;
    leaf_head.downstream_step = inner.step_id;

    StepOpenUpstream up;
    up.upstream_core = outer_core;
    up.forwarded.reserve(fwd.size());
    for (std::uint16_t pos : fwd) up.forwarded.push_back(outer_schema.columns[pos]);
    up.enclosed_open = EncodeStepOpen(leaf_head, leaf_descriptor.value(), nullptr, leaf_output);

    StepOpenHead final_head{};
    final_head.tag = PipelineTag{request_id, session_core, inner.step_id};
    final_head.downstream_core = session_core;
    final_head.downstream_step = 0;  // the session's own read, the historical zero

    plan.final_open = EncodeStepOpen(final_head, final_descriptor.value(), &up, final_output);
    plan.final_core = inner_core;
    plan.final_tag = final_head.tag;
    plan.final_rel_oid = inner.rel_oid;
    plan.stages.push_back(SessionStepClient::StageAddress{leaf_head.tag, outer_core});
    plan.stages.push_back(SessionStepClient::StageAddress{final_head.tag, inner_core});
    plan.column_names = chain.column_names;
    plan.projection_types = chain.projection_types;
    return plan;
}

}  // namespace kds::server
