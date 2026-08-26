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
    auto descriptor = EncodeStepDescriptor(ShippedForm(step));
    if (!descriptor.ok()) return descriptor.status();

    StepOpenHead head{};
    head.tag = PipelineTag{request_id, core_id_, step.step_id};
    head.downstream_core = core_id_;

    // A single-step read is a pipeline of one - the whole-row P4c shape
    // (no output spec, no upstream) - so registration, the send, and the
    // failure path all live in the one open below.
    PipelinePlan plan;
    plan.final_open = EncodeStepOpen(head, descriptor.value());
    plan.final_core = owner_core;
    plan.final_tag = head.tag;
    plan.final_rel_oid = step.rel_oid;
    plan.stages.push_back(StageAddress{head.tag, owner_core});
    return OpenPipeline(std::move(plan));
}

StatusOr<PipelineTag> SessionStepClient::OpenPipeline(PipelinePlan plan) {
    const PipelineTag tag = plan.final_tag;
    const std::uint32_t final_core = plan.final_core;

    // The read registers **before** the open is sent: replies are matched
    // by tag and an unmatched tag is silently discarded (§3's teardown
    // rule), so state that arrives after the message that generates
    // replies is state that never hears them. The in-process loopback
    // test is what catches this ordering - a real ring cannot reply
    // within the send call, which is exactly why the rule must not lean
    // on that timing - and a chained open makes it one message stronger:
    // the open fans out into stage-to-stage traffic the session never
    // sees, so the first reply can be arbitrarily removed from the send
    // that caused it.
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
    // a second error format). `Status::FromWire` keeps the code faithful
    // and degrades an unknown one to IoError; a STEP_ERROR carrying kOk is
    // a build disagreeing with itself and is IoError too, never a success.
    const std::uint32_t code = error.value().status_code;
    std::string msg = "remote step failed on its owning core";
    read->error = code == static_cast<std::uint32_t>(StatusCode::kOk)
                      ? Status::IoError(std::move(msg))
                      : Status::FromWire(code, std::move(msg));
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

Status TwoStepPipelineEligible(const exec::StepChain& chain) {
    if (chain.steps.size() != 2) {
        return Status::InvalidArgument("a two-step pipeline plans exactly two steps");
    }
    const exec::Step& outer = chain.steps[0];
    const exec::Step& inner = chain.steps[1];

    // ---- What may ship at all --------------------------------------------
    //
    // **The eligible class lives here and nowhere else** - stated once,
    // and read both by the dispatcher (before it pays for anything) and
    // by the planner below, because a rule with two spellings is a
    // correctness bug with two homes (the argument this file's
    // CreditGate/SealAndDrain already make).
    //
    // **Chain-only, and that is load-bearing**: every question here is
    // answered from the compiled chain, with no catalog lookup, so the
    // dispatcher can ask it *before* resolving two relations' schemas.
    // Folding these rules into the planner instead cost every local
    // two-step statement two `InitTableAccess` calls it never used to
    // make - measured and named in `bench/results-p4d-executor.md` §10.8.
    //
    // Every exclusion is a correctness statement, not a shortcut. An
    // aggregate would fold on the wrong core's sink. A quota (LIMIT /
    // OFFSET) and a sort both apply at *emission*, and the pipeline's
    // final edge is not the local sink they decorate - a shipped sorted
    // statement would answer unordered. A sub-chain cannot ship at all
    // (the descriptor codec refuses it). `emit_in_key_order` is how a
    // relation that has taken an out-of-order key is made to emit in key
    // order and the descriptor does not carry it, so a shipped step would
    // walk in slot
    // order and answer the clause wrongly. And a projected form is
    // *required* rather than excluded: the batches carry exactly the
    // planned output layout, which a star read has no list to describe.
    if (chain.aggregated()) {
        return Status::Unsupported("an aggregate folds at its sink and cannot ship");
    }
    if (chain.sorted() || chain.limit.has_value() || chain.offset != 0) {
        return Status::Unsupported(
            "a sort or a quota applies at emission, which a shipped stage is not");
    }
    if (chain.star() || chain.projection.empty()) {
        return Status::Unsupported("a shipped join needs a projection to lay its batches out");
    }
    if (!chain.hoisted.empty() || !outer.sub_chains.empty() || !inner.sub_chains.empty()) {
        return Status::Unsupported("a sub-chain cannot ship in a step descriptor");
    }
    if (outer.emit_in_key_order || inner.emit_in_key_order) {
        return Status::Unsupported(
            "emit_in_key_order does not travel in the descriptor, so a shipped step would "
            "answer the ordering clause in slot order");
    }

    // The inner step's kind decides how much the consuming stage can
    // buffer, which is why it is a named allow-list rather than a
    // catch-all.
    //
    // **kProbe** descends on a key taken from the outer row and emits at
    // most one row per input row.
    //
    // **kScan / kFilterScan that joins** - a walk with at least one
    // residual reaching the outer row, which is what a join on a *non-pk*
    // column compiles to (the compiler reserves kFilterScan for an
    // unindexed equality against a **literal**, so a join predicate leaves
    // the kind kScan). Such a walk would once have sealed every match of
    // one input row inside one synchronous call; since P4d-4c's gated
    // inner walk it parks at its own page boundaries under the downstream
    // credit and buffers a page's matches instead.
    //
    // The "reaches the outer row" half is not decoration: without it this
    // admits a walk that ignores its input entirely - a cross product,
    // correct but quadratic, and never what anybody asked a join to be.
    // It can only ever *pass* on a genuine join, because the compiler
    // places a conjunct at the highest slot it references
    // (`PredicateReadyAt`), so an outer-only conjunct never lands here.
    //
    // **An index or Cabin probe joins the walked class**, because that is
    // what it ships as (`ShippedForm`): the same outer-row requirement,
    // the same 4c gated-walk buffering. The history of refusing these
    // kinds outright is docs/inflight/known-gaps.md's closed entry.
    const bool joins_the_outer_row = [&] {
        for (const exec::StepPredicate& pred : inner.residual) {
            if (pred.lhs.up == 0 && pred.lhs.rel_slot == 0) return true;
            if (pred.rhs.kind == exec::OperandKind::kColumn && pred.rhs.column.up == 0 &&
                pred.rhs.column.rel_slot == 0) {
                return true;
            }
        }
        return false;
    }();
    const bool inner_kind_ok =
        (inner.kind == exec::AccessKind::kProbe && inner.key.has_value() &&
         inner.key->kind == exec::OperandKind::kColumn) ||
        ((inner.kind == exec::AccessKind::kScan ||
          inner.kind == exec::AccessKind::kFilterScan || ShipsAsWalk(inner.kind)) &&
         joins_the_outer_row);
    if (!inner_kind_ok) {
        return Status::Unsupported(
            "the inner step is neither a probe keyed by the outer row nor a walk that "
            "references it, so it is not a join this pipeline can bound");
    }
    return Status::OK();
}

StatusOr<SessionStepClient::PipelinePlan> BuildTwoStepPipeline(
    const exec::StepChain& chain, const catalog::Schema& outer_schema,
    const catalog::Schema& inner_schema, std::uint32_t outer_core, std::uint32_t inner_core,
    std::uint32_t session_core, std::uint64_t request_id) {
    // The same rule the dispatcher already asked, asked again rather than
    // assumed: this function is callable on its own, and a planner that
    // trusted its caller to have checked would be one edit away from
    // planning a shape it cannot plan.
    if (Status s = TwoStepPipelineEligible(chain); !s.ok()) return s;
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
        if (ref.up != 0 || ref.rel_slot > 1) {
            // Nothing at a statement's top level references outward, and a
            // two-step chain has two slots; either violation is a compiler
            // defect, not a plannable shape - refused here so the
            // projection loop below may treat "not slot 0" as the inner
            // relation without a third case.
            noted = Status::InvalidArgument("a reference outside the two-step chain");
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
    // Downgraded before the normalize below: `ShippedForm` drops the
    // structure aux whole, so no reference inside one is ever rewritten,
    // and the join equality the walk filters on rides the residual
    // exactly as a compiled walk's would.
    exec::Step shipped = ShippedForm(inner);
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
        // **A conjunct's lhs decides the comparison's type**, and only the
        // lhs: `EvaluateAll` (exec/chain_frame.cpp) looks the type up in
        // the schema list of the frame the ref names, and a ref that
        // normalizes to the *upstream* row names a frame the consuming
        // stage's chain-of-one does not carry schemas for - so that
        // conjunct falls through to the untyped comparison. For every type
        // but one that is the same answer (`CompareValues` reads
        // `type_val` in exactly one arm); for `uint64` it is not, because
        // a value above INT64_MAX rides in `int_val` as a negative and
        // orders below every small one. The same statement run locally
        // resolves the schema and compares unsigned, so shipping this
        // shape would answer a join differently on two cores. Refused at
        // plan - the caller falls through to the affinity refusal - rather
        // than shipped as a wrong answer. `col_pos` is already known
        // in-bounds: every slot-0 ref was noted into `fwd` and bounded
        // above.
        if (pred.lhs.up == 0 && pred.lhs.rel_slot == 0 &&
            outer_schema.columns[pred.lhs.col_pos].type_val == catalog::kTypeValUint64) {
            return Status::Unsupported(
                "a conjunct with the upstream relation's uint64 column on the left cannot "
                "ship: the consuming stage would compare it without its column type");
        }
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
    // The leaf gets the same ship-time downgrade as the consuming stage: a
    // literal restriction on the outer relation's indexed or cabined
    // column compiles to a structure step too, and it ships as its walk.
    auto leaf_descriptor = EncodeStepDescriptor(ShippedForm(outer));
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
