#include "kds/server/remote_step_service.hpp"

#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "kds/exec/step_vm.hpp"
#include "kds/server/step_descriptor.hpp"
#include "kds/txn/manager.hpp"
#include "kds/wire/row_codec.hpp"

namespace kds::server {

namespace {

// Little-endian u32 append/read for the envelope's variable section. The
// head and the column rows are memcpy'd PODs under ring_message.hpp's
// in-process exception; these frame the variable-length parts between.
void PutU32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(std::byte((v >> (8 * i)) & 0xFF));
}

// One output-spec entry on the wire: the source flag plus the index.
// Named because the encoder's reserve arithmetic and the decoder's
// truncation bound are the same fact, and a literal 5 in two files is how
// they drift.
constexpr std::size_t kOutputColumnBytes = 1 + 4;

StatusOr<std::uint32_t> TakeU32(std::span<const std::byte>& rest) {
    if (rest.size() < 4) {
        return Status::InvalidArgument(
            "STEP_OPEN envelope truncated inside a variable section");
    }
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= std::uint32_t(rest[static_cast<std::size_t>(i)]) << (8 * i);
    rest = rest.subspan(4);
    return v;
}

// The one home for "an empty output spec means the whole row in schema
// order" (the P4c shape) - and for its bound: `cols` was validated against
// the schema at open, but a producer re-fetches its schema after the
// submit, so the position is re-checked against what was actually fetched.
// Unreachable today (nothing data-moving alters a relation), and one
// refusal is cheaper than the day it stops being unreachable.
StatusOr<catalog::Schema> NarrowTo(const catalog::Schema& schema,
                                   std::vector<std::uint16_t>& cols) {
    if (cols.empty()) {
        cols.resize(schema.columns.size());
        for (std::size_t i = 0; i < cols.size(); ++i) cols[i] = static_cast<std::uint16_t>(i);
    }
    catalog::Schema out;
    out.columns.reserve(cols.size());
    for (std::uint16_t pos : cols) {
        if (pos >= schema.columns.size()) {
            return Status::InvalidArgument(
                "an output column sits past the relation's schema");
        }
        out.columns.push_back(schema.columns[pos]);
    }
    return out;
}

}  // namespace

std::vector<std::byte> EncodeStepOpen(const StepOpenHead& head,
                                      std::span<const std::byte> descriptor,
                                      const StepOpenUpstream* upstream,
                                      std::span<const StepOutputColumn> output) {
    std::vector<std::byte> out;
    // One allocation, as the pre-4b encoder had: the envelope's size is
    // fully known up front.
    std::size_t total = sizeof(head) + 1 + 1 + descriptor.size();
    if (upstream != nullptr) {
        total += 4 + 4 + upstream->forwarded.size() * sizeof(catalog::SysColumnRow) + 4 +
                 upstream->enclosed_open.size();
    }
    if (!output.empty()) total += 4 + output.size() * kOutputColumnBytes;
    out.reserve(total);
    out.resize(sizeof(head));
    std::memcpy(out.data(), &head, sizeof(head));

    out.push_back(std::byte{upstream != nullptr ? std::uint8_t{1} : std::uint8_t{0}});
    if (upstream != nullptr) {
        PutU32(out, upstream->upstream_core);
        PutU32(out, static_cast<std::uint32_t>(upstream->forwarded.size()));
        for (const catalog::SysColumnRow& col : upstream->forwarded) {
            const auto* bytes = reinterpret_cast<const std::byte*>(&col);
            out.insert(out.end(), bytes, bytes + sizeof(col));
        }
        PutU32(out, static_cast<std::uint32_t>(upstream->enclosed_open.size()));
        out.insert(out.end(), upstream->enclosed_open.begin(), upstream->enclosed_open.end());
    }

    // The output spec, beside the upstream section rather than inside it
    // (P4d-4b-3): a leaf stage seals its consumer's input layout, and only
    // a section every stage carries can say so. Flag 0 = whole row.
    out.push_back(std::byte{!output.empty() ? std::uint8_t{1} : std::uint8_t{0}});
    if (!output.empty()) {
        PutU32(out, static_cast<std::uint32_t>(output.size()));
        for (const StepOutputColumn& col : output) {
            out.push_back(std::byte{col.from_upstream});
            PutU32(out, col.index);
        }
    }

    out.insert(out.end(), descriptor.begin(), descriptor.end());
    return out;
}

StatusOr<StepOpenParts> DecodeStepOpenEnvelope(std::span<const std::byte> payload) {
    if (payload.size() < sizeof(StepOpenHead) + 2) {
        return Status::InvalidArgument("STEP_OPEN of " + std::to_string(payload.size()) +
                                       " bytes cannot hold a head and its section flags");
    }
    StepOpenParts parts;
    std::memcpy(&parts.head, payload.data(), sizeof(parts.head));
    std::span<const std::byte> rest = payload.subspan(sizeof(parts.head));

    const std::uint8_t has_upstream = std::uint8_t(rest.front());
    rest = rest.subspan(1);
    if (has_upstream > 1) {
        return Status::InvalidArgument("STEP_OPEN envelope carries upstream flag " +
                                       std::to_string(has_upstream) +
                                       ", which no encoder writes");
    }
    if (has_upstream == 1) {
        StepOpenUpstream up;
        auto core = TakeU32(rest);
        if (!core.ok()) return core.status();
        up.upstream_core = core.value();

        auto count = TakeU32(rest);
        if (!count.ok()) return count.status();
        // Widened before the multiply: a u32 count times a 96-byte row
        // cannot overflow a 64-bit size_t, and the bound is checked
        // before the resize so a bogus count allocates nothing.
        const std::size_t layout_bytes =
            static_cast<std::size_t>(count.value()) * sizeof(catalog::SysColumnRow);
        if (rest.size() < layout_bytes) {
            return Status::InvalidArgument(
                "STEP_OPEN envelope truncated inside its forwarded layout");
        }
        up.forwarded.resize(count.value());
        // An edge may forward no columns, and an empty vector's data() is
        // null - memcpy with a null argument is undefined even for zero
        // bytes (UBSan flags it on the refusal test's empty layout).
        if (layout_bytes > 0) {
            std::memcpy(up.forwarded.data(), rest.data(), layout_bytes);
        }
        rest = rest.subspan(layout_bytes);

        auto enclosed = TakeU32(rest);
        if (!enclosed.ok()) return enclosed.status();
        if (rest.size() < enclosed.value()) {
            return Status::InvalidArgument(
                "STEP_OPEN envelope truncated inside its enclosed open");
        }
        up.enclosed_open.assign(rest.begin(), rest.begin() + enclosed.value());
        rest = rest.subspan(enclosed.value());
        parts.upstream = std::move(up);
    }

    if (rest.empty()) {
        return Status::InvalidArgument("STEP_OPEN envelope truncated before its output flag");
    }
    const std::uint8_t has_output = std::uint8_t(rest.front());
    rest = rest.subspan(1);
    if (has_output > 1) {
        return Status::InvalidArgument("STEP_OPEN envelope carries output flag " +
                                       std::to_string(has_output) +
                                       ", which no encoder writes");
    }
    if (has_output == 1) {
        auto out_count = TakeU32(rest);
        if (!out_count.ok()) return out_count.status();
        // Bounded before the reserve, exactly as the layout above is bounded
        // before its resize: `out_count` is a wire u32, and reserving on it
        // first would let four bytes ask for gigabytes.
        const std::size_t output_bytes =
            static_cast<std::size_t>(out_count.value()) * kOutputColumnBytes;
        if (rest.size() < output_bytes) {
            return Status::InvalidArgument("STEP_OPEN envelope truncated inside its output spec");
        }
        parts.output.reserve(out_count.value());
        for (std::uint32_t i = 0; i < out_count.value(); ++i) {
            StepOutputColumn col;
            const std::uint8_t from = std::uint8_t(rest.front());
            rest = rest.subspan(1);
            if (from > 1) {
                return Status::InvalidArgument("STEP_OPEN output column carries source flag " +
                                               std::to_string(from) +
                                               ", which no encoder writes");
            }
            col.from_upstream = from;
            auto idx = TakeU32(rest);
            if (!idx.ok()) return idx.status();
            if (idx.value() > std::numeric_limits<std::uint16_t>::max()) {
                return Status::InvalidArgument("STEP_OPEN output column index " +
                                               std::to_string(idx.value()) +
                                               " does not fit a column position");
            }
            col.index = static_cast<std::uint16_t>(idx.value());
            parts.output.push_back(col);
        }
    }

    parts.descriptor = rest;
    return parts;
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

// The message header is unused as of P4d-4b-2: every error this handler
// can raise is addressed by the tag's session_core, because a chained
// open's sender is a stage rather than the session. Kept in the signature
// only because the runtime binds it as a MessageHandler.
void RemoteStepServer::OnStepOpen(const sched::MessageHeader&,
                                  std::span<const std::byte> payload) {
    auto parts = DecodeStepOpenEnvelope(payload);
    if (!parts.ok()) {
        // With a whole head present the tag is real even when the rest
        // is not: answer the session so its read completes instead of
        // waiting on a stage that silently never opened. Addressed by the
        // tag's own session_core rather than the sender: a chained open's
        // sender is the *downstream stage*, which registers no kStepError
        // handler at all, so routing there would drop the refusal and hang
        // the session on a stage that never opened. With less than a head
        // there is nobody to address - log is all there is.
        if (payload.size() >= sizeof(StepOpenHead)) {
            StepOpenHead head{};
            std::memcpy(&head, payload.data(), sizeof(head));
            SendError(head.tag, head.tag.session_core, parts.status());
        } else if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("pipeline", "STEP_OPEN refused: " + parts.status().message());
        }
        return;
    }
    const StepOpenParts& env = parts.value();
    const StepOpenHead head = env.head;
    // The session's core comes off the tag, not the message header: since
    // the chained-open amendment (crosscore.md §2) an open's sender may be
    // the downstream *stage*, and an error must still reach the session.
    const std::uint32_t session = head.tag.session_core;

    auto step = DecodeStepDescriptor(env.descriptor);
    if (!step.ok()) {
        SendError(head.tag, session, step.status());
        return;
    }

    auto access = catalog_.InitTableAccess(step.value().rel_oid);
    if (!access.ok()) {
        SendError(head.tag, session, access.status());
        return;
    }
    const catalog::Schema& schema = access.value()->schema;

    if (env.upstream.has_value()) {
        OpenConsumingStage(head, *env.upstream, env.output, std::move(step.value()), schema);
        return;
    }

    // The leaf class: every reference resolves inside this step, and a
    // key is a literal - "produced by an earlier step" has no earlier
    // step on a leaf. A consuming stage's upstream references took the
    // branch above.
    if (step.value().key.has_value() &&
        step.value().key->kind == exec::OperandKind::kColumn) {
        SendError(head.tag, session,
                  Status::Unsupported("a probe keyed by another step's column cannot run as a "
                                      "leaf stage; it needs an upstream edge (P4d-4b)"));
        return;
    }
    for (const exec::StepPredicate& pred : step.value().residual) {
        if (pred.lhs.up != 0 || (pred.rhs.kind == exec::OperandKind::kColumn &&
                                 pred.rhs.column.up != 0)) {
            SendError(head.tag, session,
                      Status::Unsupported("a residual referencing an enclosing chain cannot "
                                          "run as a leaf stage"));
            return;
        }
    }

    // A leaf's output spec (P4d-4b-3): local columns only - there is no
    // input layout for `from_upstream` to index - each within the schema.
    // Kept as positions; empty means every column, the P4c whole-row shape.
    std::vector<std::uint16_t> out_cols;
    out_cols.reserve(env.output.size());
    for (const StepOutputColumn& col : env.output) {
        if (col.from_upstream != 0 || col.index >= schema.columns.size()) {
            SendError(head.tag, session,
                      Status::InvalidArgument(
                          "a leaf stage's output spec may name only its own relation's "
                          "columns; the plan was not normalized"));
            return;
        }
        out_cols.push_back(col.index);
    }

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
                                    RunProducer(head.tag, std::move(chain),
                                                std::move(out_cols))));
        return;
    }

    // Collect-then-stream, the reactorless fallback (see the header).
    auto narrowed = NarrowTo(schema, out_cols);
    if (!narrowed.ok()) {
        SendError(head.tag, session, narrowed.status());
        return;
    }
    const catalog::Schema out_schema = std::move(narrowed.value());
    auto snapshot = txn::AutocommitSnapshot(txns_);
    if (!snapshot.ok()) {
        SendError(head.tag, session, snapshot.status());
        return;
    }
    wire::RowBatchWriter writer;
    std::vector<parser::AstValue> row(out_cols.size());

    Status ran = exec::Execute(
        catalog_, store_, chain,
        [&](const exec::ChainFrame& frame) -> StatusOr<storage::VisitControl> {
            for (std::size_t i = 0; i < out_cols.size(); ++i) {
                row[i] = frame.Get(exec::ColumnRef{0, 0, out_cols[i]});
            }
            if (Status s = writer.AppendRow(out_schema, row); !s.ok()) return s;
            if (writer.size_bytes() >= batch_target_ || writer.full()) Seal(pipe, writer);
            return storage::VisitControl::kContinue;
        },
        /*stats=*/nullptr, budget_, /*trail=*/nullptr, /*replay=*/nullptr,
        /*cabins=*/nullptr, &snapshot.value().snap);
    if (!ran.ok()) {
        SendError(head.tag, session, ran);
        return;
    }
    if (writer.row_count() > 0) Seal(pipe, writer);

    pipelines_.push_back(std::move(pipe));
    Drain(pipelines_.back());
}

void RemoteStepServer::OpenConsumingStage(const StepOpenHead& head, const StepOpenUpstream& up,
                                          std::span<const StepOutputColumn> output,
                                          exec::Step step, const catalog::Schema& schema) {
    const std::uint32_t session = head.tag.session_core;
    if (!submit_) {
        SendError(head.tag, session,
                  Status::Unsupported("a consuming stage parks between batches and needs a "
                                      "reactor; this server is reactorless"));
        return;
    }
    if (up.enclosed_open.size() < sizeof(StepOpenHead) + 2) {
        SendError(head.tag, session,
                  Status::InvalidArgument(
                      "a consuming stage's enclosed upstream open has no head"));
        return;
    }
    if (output.empty()) {
        SendError(head.tag, session,
                  Status::InvalidArgument(
                      "a consuming stage that forwards nothing serves nobody"));
        return;
    }
    for (const StepOutputColumn& col : output) {
        const std::size_t bound =
            col.from_upstream != 0 ? up.forwarded.size() : schema.columns.size();
        if (col.index >= bound) {
            SendError(head.tag, session,
                      Status::InvalidArgument(
                          "a consuming stage's output column indexes past its " +
                          std::string(col.from_upstream != 0 ? "input layout" : "relation")));
            return;
        }
    }

    // The stage's references arrive normalized by the session (fact 4 /
    // 4b-3): its own columns at (up=0, slot=0), the upstream row's at
    // (up=1, slot=0) with col_pos into the *forwarded* layout. Anything
    // else is a malformed plan - refused, never guessed at.
    auto own = [&](const exec::ColumnRef& ref) {
        return ref.up == 0 && ref.rel_slot == 0 && ref.col_pos < schema.columns.size();
    };
    auto upstream = [&](const exec::ColumnRef& ref) {
        return ref.up == 1 && ref.rel_slot == 0 && ref.col_pos < up.forwarded.size();
    };
    bool refs_ok = true;
    if (step.key.has_value() && step.key->kind == exec::OperandKind::kColumn) {
        // **The key comes from the upstream row and nowhere else.** A key
        // naming this stage's own slot would be read out of a frame this
        // step has not decoded yet: the first input row probes a kNull
        // (a miss) and every later one probes the *previous* row's column -
        // plausible rows for a fabricated id, which is the one failure
        // class that leaves no trace.
        refs_ok = upstream(step.key->column);
    }
    for (const exec::StepPredicate& pred : step.residual) {
        // Either side may name either row: an ON clause is written in
        // whichever orientation the client typed (`a.b_id = b.id` puts the
        // upstream column on the *left*), and the compiler attaches the
        // conjunct without reorienting it.
        refs_ok = refs_ok && (own(pred.lhs) || upstream(pred.lhs));
        if (pred.rhs.kind == exec::OperandKind::kColumn) {
            refs_ok = refs_ok && (own(pred.rhs.column) || upstream(pred.rhs.column));
        }
    }
    if (!refs_ok) {
        SendError(head.tag, session,
                  Status::InvalidArgument(
                      "a consuming stage's references must name its own slot or, at up=1, "
                      "the forwarded layout (and its key the forwarded layout only); the "
                      "plan was not normalized"));
        return;
    }

    exec::StepChain chain;
    chain.steps.push_back(std::move(step));

    catalog::Schema input_schema;
    input_schema.columns = up.forwarded;
    catalog::Schema output_schema;
    output_schema.columns.reserve(output.size());
    for (const StepOutputColumn& col : output) {
        output_schema.columns.push_back(col.from_upstream != 0 ? up.forwarded[col.index]
                                                               : schema.columns[col.index]);
    }

    StepOpenHead upstream_head{};
    std::memcpy(&upstream_head, up.enclosed_open.data(), sizeof(upstream_head));

    // The enclosed open must actually address *this* stage: its
    // `downstream_core`/`downstream_step` name where its batches will be
    // sent and which pipeline consumes them, and a mismatch is a plan
    // wired to the wrong consumer - rows would flow to a core or a tag
    // that never opened. Refused here, where the two halves first meet
    // (P4d-4b-3; this is also what makes `downstream_step` a read field
    // rather than a written-and-forgotten one).
    if (upstream_head.downstream_core != core_id_ ||
        upstream_head.downstream_step != head.tag.step_id) {
        SendError(head.tag, session,
                  Status::InvalidArgument(
                      "a consuming stage's enclosed open addresses core " +
                      std::to_string(upstream_head.downstream_core) + " step " +
                      std::to_string(upstream_head.downstream_step) + ", not this stage"));
        return;
    }

    Pipeline pipe;
    pipe.tag = head.tag;
    pipe.downstream = head.downstream_core;
    pipe.producing = true;
    Pipeline::InputEdge edge;
    edge.input_tag = upstream_head.tag;
    edge.upstream_core = up.upstream_core;
    pipe.consumer.emplace(std::move(edge));
    pipelines_.push_back(std::move(pipe));

    submit_(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        RunConsumer(head.tag, std::move(chain), std::move(input_schema),
                    std::vector<StepOutputColumn>(output.begin(), output.end()),
                    std::move(output_schema))));

    // The chained open, last (fact 1): this stage's state exists, so the
    // upstream may start the moment it opens. A forward that cannot be
    // sent kills the stage now rather than hanging the session on
    // silence.
    std::vector<std::byte> enclosed = up.enclosed_open;
    if (Status s =
            send_(up.upstream_core, sched::RingMessageKind::kStepOpen, std::move(enclosed));
        !s.ok()) {
        SendError(head.tag, session, s);
        if (Pipeline* p = Find(head.tag); p != nullptr) p->cancelled = true;
    }
}

sched::Coro RemoteStepServer::RunConsumer(PipelineTag tag, exec::StepChain chain,
                                          catalog::Schema input_schema,
                                          std::vector<StepOutputColumn> output,
                                          catalog::Schema output_schema) {
    // The outer frame (fact 4): one slot, the upstream row, refreshed per
    // input row. The inner chain's (up=1) references read through it
    // exactly as a correlated sub-chain reads its outer row.
    exec::ChainFrame outer;
    const std::vector<const catalog::Schema*> outer_schemas{&input_schema};
    outer.Open(outer_schemas, nullptr);

    // One view for every input row this stage joins against: a re-mint
    // per row would let the same statement's later rows see writes its
    // earlier rows could not.
    auto snapshot = txn::AutocommitSnapshot(txns_);
    if (!snapshot.ok()) {
        // §7's upstream half applies to this exit too: by the time this
        // coroutine first runs, OpenConsumingStage has already forwarded
        // the enclosed open, so a producer may be live and would park on
        // a credit gate nobody will open again.
        SendError(tag, tag.session_core, snapshot.status());
        if (Pipeline* pipe = Find(tag); pipe != nullptr && pipe->consumer.has_value()) {
            CancelUpstream(pipe->consumer->input_tag, pipe->consumer->upstream_core);
        }
        Erase(tag);
        co_return snapshot.status();
    }

    // **One row-touch bound for the whole stage, not one per input row.**
    // `ExecuteAsync` seeds a *fresh* counter from whatever limit it is
    // handed (step_vm.cpp), so handing it the limit again per row would
    // restart the count and leave this stage with no statement-wide bound
    // at all. That was harmless while the admitted inner was a probe -
    // one descent per input row - and stopped being harmless the moment
    // P4d-4c admitted a *walked* inner: two relations of n rows is n²
    // decodes, which `exec/budget.hpp` exists to refuse and which the
    // local path does refuse, so without this the pipeline answers a
    // statement its local twin errors on. A passed-in `ExecStats` is
    // written through and never reset, so the stage accumulates there and
    // hands each row only what is left.
    //
    // The ceiling is this core's configured one (`budget_`), not a fresh
    // default - see the member's comment for why that is the session's
    // limit in every deployment that configures its cores alike.
    exec::ExecStats spend;
    const std::uint64_t limit = budget_.limit();
    const bool bounded = limit != exec::kUnlimitedRowTouchBudget;

    wire::RowBatchWriter writer;
    std::vector<parser::AstValue> out_row(output_schema.columns.size());

    // Parks between input batches; anything that ends the wait for good
    // (teardown, cancel, the upstream's EOF) opens it too.
    const std::function<bool()> actionable = [this, tag] {
        Pipeline* pipe = Find(tag);
        return pipe == nullptr || pipe->cancelled || !pipe->consumer.has_value() ||
               !pipe->consumer->input.empty() || pipe->consumer->input_eof;
    };
    // The shared credit gate, parked on after every joined row: buffering
    // stays bounded at the credit ceiling plus one *row's* seals for the
    // probe/lookup shapes. A filter-scan inner can still seal every match
    // of ONE input row inside one synchronous Execute - the gated inner
    // walk that bounds that is P4d-4c's, named in the workplan.
    const std::function<bool()> output_ok = CreditGate(tag);

    // Every stopping exit owes the upstream a cancel (§7): without one
    // the producer parks on its credit gate for the process's life.
    auto fail = [&](const PipelineTag& input_tag, std::uint32_t upstream_core,
                    const Status& status) {
        SendError(tag, tag.session_core, status);
        CancelUpstream(input_tag, upstream_core);
        Erase(tag);
    };

    for (;;) {
        co_await sched::WaitUntil{&actionable};
        Pipeline* pipe = Find(tag);
        if (pipe == nullptr) co_return Status::OK();  // torn down; nothing to say
        if (pipe->cancelled || !pipe->consumer.has_value()) {
            if (pipe->consumer.has_value()) {
                CancelUpstream(pipe->consumer->input_tag, pipe->consumer->upstream_core);
            }
            Erase(tag);
            co_return Status::OK();
        }

        if (pipe->consumer->input.empty()) {
            // Input EOF with the queue empty: this stage's own end - the
            // upstream already finished, so there is nothing left to
            // cancel. The final seal, then EOF downstream rides the
            // producing=false drain.
            if (writer.row_count() > 0) SealAndDrain(tag, writer);
            if (Pipeline* done = Find(tag); done != nullptr) {
                if (done->cancelled) {
                    Erase(tag);
                    co_return Status::OK();
                }
                done->producing = false;
                Drain(*done);
            }
            co_return Status::OK();
        }

        const std::vector<std::byte> batch = std::move(pipe->consumer->input.front());
        pipe->consumer->input.pop_front();
        const PipelineTag input_tag = pipe->consumer->input_tag;
        const std::uint32_t upstream_core = pipe->consumer->upstream_core;

        std::span<const std::byte> rows_bytes;
        auto batch_header = DecodeStepBatchHeader(batch, rows_bytes);
        if (!batch_header.ok()) {
            fail(input_tag, upstream_core, batch_header.status());
            co_return batch_header.status();
        }
        auto rows = wire::DecodeRowBatch(rows_bytes, input_schema.columns.size());
        if (!rows.ok()) {
            fail(input_tag, upstream_core, rows.status());
            co_return rows.status();
        }
        // Two independently-encoded counts of one thing, never compared
        // until now: a skew means this stage decodes with a layout the
        // producer did not encode with, and a silently wrong join is
        // invariant 13's forbidden shape one level up.
        if (rows.value().size() != batch_header.value().row_count) {
            const Status skew = Status::Corruption(
                "input batch declares " + std::to_string(batch_header.value().row_count) +
                " rows but decodes to " + std::to_string(rows.value().size()) +
                " under the forwarded layout; the edge's two ends disagree");
            fail(input_tag, upstream_core, skew);
            co_return skew;
        }

        bool stopped = false;
        for (const auto& in_row : rows.value()) {
            auto slots = outer.SlotsFor(0);
            Status filled = Status::OK();
            for (std::size_t i = 0; i < input_schema.columns.size(); ++i) {
                // Checked per field (invariant 13 one level up): the batch
                // decode bounded each length against the payload, but only
                // the column knows what length the value *should* have,
                // and a short field zero-extended is a wrong number that
                // joins plausibly.
                auto value = wire::FieldToValueChecked(input_schema.columns[i], in_row[i]);
                if (!value.ok()) {
                    filled = value.status();
                    break;
                }
                slots[i] = std::move(value.value());
            }
            if (!filled.ok()) {
                fail(input_tag, upstream_core, filled);
                co_return filled;
            }
            // **Awaited and gated** (P4d-4c's gated inner walk): the inner
            // run parks at its own page boundaries whenever the sealed
            // output cannot ship, so an inner *walk* - a join on a
            // non-pk column, which is a filter-scan, not a probe - buffers
            // one page's matches rather than every match of one input
            // row. That bound is what lets the dispatcher ship the shape
            // at all; a synchronous Execute here could only run the walk
            // to its end, however much it sealed.
            //
            // A probe inner never reaches the gate (it descends rather
            // than walking), which is why the per-row park below stays:
            // it is the only backpressure a probe has.
            // What this stage has left. Refused *before* the call when
            // nothing is: `Budget(0)` is the **unlimited** sentinel
            // (exec/budget.hpp), so subtracting to zero would remove the
            // bound rather than enforce it.
            const std::uint64_t used = spend.Total().rows_examined;
            if (bounded && used >= limit) {
                const Status over = Status::ResourceExhausted(
                    "this pipeline stage examined " + std::to_string(used) +
                    " rows, its whole row-touch budget; a join whose inner side is walked "
                    "once per input row is what that budget exists to stop");
                fail(input_tag, upstream_core, over);
                co_return over;
            }
            Status ran = co_await exec::ExecuteAsync(
                catalog_, store_, chain,
                [&](const exec::ChainFrame& frame) -> StatusOr<storage::VisitControl> {
                    Pipeline* q = Find(tag);
                    if (q == nullptr || q->cancelled) return storage::VisitControl::kStop;
                    for (std::size_t o = 0; o < output.size(); ++o) {
                        out_row[o] = frame.Get(exec::ColumnRef{
                            static_cast<std::uint16_t>(output[o].from_upstream != 0 ? 1 : 0),
                            0, output[o].index});
                    }
                    if (Status s = writer.AppendRow(output_schema, out_row); !s.ok()) return s;
                    if (writer.size_bytes() >= batch_target_ || writer.full()) {
                        SealAndDrain(tag, writer);
                    }
                    return storage::VisitControl::kContinue;
                },
                &spend,
                exec::Budget(bounded ? limit - used : exec::kUnlimitedRowTouchBudget),
                /*trail=*/nullptr, /*replay=*/nullptr,
                /*cabins=*/nullptr, &snapshot.value().snap, /*indexes=*/true, &output_ok,
                /*parent=*/&outer);
            if (!ran.ok()) {
                fail(input_tag, upstream_core, ran);
                co_return ran;
            }
            if (Pipeline* q = Find(tag); q == nullptr || q->cancelled) {
                stopped = true;
                break;
            }
            // Backpressure per row: sealed output must be shippable before
            // the next row joins.
            co_await sched::WaitUntil{&output_ok};
        }
        if (stopped) continue;  // the loop head routes teardown and cancel

        // Grant-on-drain: this batch is consumed, the upstream may
        // replace it.
        StepCreditPayload credit{input_tag, 1};
        std::vector<std::byte> bytes;
        EncodePipelinePayload(credit, bytes);
        if (Status s =
                send_(upstream_core, sched::RingMessageKind::kStepCredit, std::move(bytes));
            !s.ok() && log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("pipeline", "input credit could not be sent: " + s.message());
        }
    }
}

RemoteStepServer::Pipeline* RemoteStepServer::FindByInputTag(const PipelineTag& input_tag) {
    for (Pipeline& pipe : pipelines_) {
        if (pipe.consumer.has_value() && pipe.consumer->input_tag == input_tag) return &pipe;
    }
    return nullptr;  // no consuming pipeline wants it: §3's silent discard
}

void RemoteStepServer::OnStepBatch(std::span<const std::byte> payload) {
    std::span<const std::byte> rows;
    auto batch_header = DecodeStepBatchHeader(payload, rows);
    if (!batch_header.ok()) return;  // malformed: dropped, as the session client drops
    if (Pipeline* pipe = FindByInputTag(batch_header.value().tag); pipe != nullptr) {
        pipe->consumer->input.emplace_back(payload.begin(), payload.end());
    }
}

void RemoteStepServer::OnStepEof(std::span<const std::byte> payload) {
    auto eof = DecodePipelinePayload<StepEofPayload>(payload);
    if (!eof.ok()) return;
    if (Pipeline* pipe = FindByInputTag(eof.value().tag); pipe != nullptr) {
        pipe->consumer->input_eof = true;
    }
}

std::vector<std::byte> EncodeStepBatch(const PipelineTag& tag, std::uint32_t seq,
                                       wire::RowBatchWriter& writer) {
    StepBatchHeader batch{};
    batch.tag = tag;
    batch.seq = seq;
    batch.row_count = writer.row_count();
    std::vector<std::byte> rows_bytes = writer.Finish();
    std::vector<std::byte> out(sizeof(batch) + rows_bytes.size());
    std::memcpy(out.data(), &batch, sizeof(batch));
    std::memcpy(out.data() + sizeof(batch), rows_bytes.data(), rows_bytes.size());
    return out;
}

std::function<bool()> RemoteStepServer::CreditGate(const PipelineTag& tag) {
    return [this, tag] {
        Pipeline* pipe = Find(tag);
        return pipe == nullptr || pipe->cancelled || pipe->batches.empty() ||
               pipe->credit.can_send();
    };
}

void RemoteStepServer::SealAndDrain(const PipelineTag& tag, wire::RowBatchWriter& writer) {
    Pipeline* pipe = Find(tag);
    if (pipe == nullptr || pipe->cancelled) return;
    Seal(*pipe, writer);
    Drain(*pipe);
}

void RemoteStepServer::CancelUpstream(const PipelineTag& input_tag,
                                      std::uint32_t upstream_core) {
    StepEofPayload cancel{input_tag};
    std::vector<std::byte> bytes;
    EncodePipelinePayload(cancel, bytes);
    (void)send_(upstream_core, sched::RingMessageKind::kStepCancel, std::move(bytes));
}

// One batch encoder for both execution shapes: the equivalence test pins
// the two byte-identical, and an encoder existing twice is exactly what
// would let them drift.
void RemoteStepServer::Seal(Pipeline& pipe, wire::RowBatchWriter& writer) {
    std::vector<std::byte> out = EncodeStepBatch(pipe.tag, pipe.seq++, writer);
    pipe.batches.push_back(std::move(out));
}

sched::Coro RemoteStepServer::RunProducer(PipelineTag tag, exec::StepChain chain,
                                          std::vector<std::uint16_t> output) {

    // Copied into this frame, not borrowed: a park can cross a catalog
    // invalidation (`kCatalogInvalidate` is broadcast by *any* DDL and its
    // handler clears the whole TableAccess cache), which frees a borrowed
    // Schema under a parked coroutine. The executor's own borrows get the
    // same treatment one layer down - RunWalkStep re-Binds after every
    // real park - and the copy is priced per statement, not per row. The
    // copy also fixes what the batch *means*: §5 says a remote step
    // trusts the descriptor and does not re-resolve, and a copy is
    // exactly a view that cannot re-resolve.
    catalog::Schema schema;
    {
        auto access = catalog_.InitTableAccess(chain.steps[0].rel_oid);
        if (!access.ok()) {
            SendError(tag, tag.session_core, access.status());
            Erase(tag);
            co_return access.status();
        }
        schema = access.value()->schema;  // the borrow dies with this scope
    }

    auto narrowed = NarrowTo(schema, output);
    if (!narrowed.ok()) {
        SendError(tag, tag.session_core, narrowed.status());
        Erase(tag);
        co_return narrowed.status();
    }
    const catalog::Schema out_schema = std::move(narrowed.value());

    // Minted once and held **by value** in this frame: a ReadView is a
    // POD and the undo pointer outlives the reactor, so the view survives
    // every page-boundary park - which is what makes the whole stream one
    // statement's answer rather than a series of re-reads.
    auto snapshot = txn::AutocommitSnapshot(txns_);
    if (!snapshot.ok()) {
        SendError(tag, tag.session_core, snapshot.status());
        Erase(tag);
        co_return snapshot.status();
    }

    wire::RowBatchWriter writer;
    std::vector<parser::AstValue> row(output.size());

    // The walk parks at a page boundary while a sealed batch waits on
    // credit; anything that ends the wait for good (teardown, cancel)
    // opens the gate so the sink can stop the walk. The predicate lives
    // in this frame, which outlives every poll that reads it (WaitUntil's
    // lifetime rule).
    const std::function<bool()> gate = CreditGate(tag);

    Status ran = co_await exec::ExecuteAsync(
        catalog_, store_, chain,
        [&](const exec::ChainFrame& frame) -> StatusOr<storage::VisitControl> {
            Pipeline* pipe = Find(tag);
            if (pipe == nullptr || pipe->cancelled) return storage::VisitControl::kStop;
            for (std::size_t i = 0; i < output.size(); ++i) {
                row[i] = frame.Get(exec::ColumnRef{0, 0, output[i]});
            }
            if (Status s = writer.AppendRow(out_schema, row); !s.ok()) return s;
            if (writer.size_bytes() >= batch_target_ || writer.full()) {
                SealAndDrain(tag, writer);
            }
            return storage::VisitControl::kContinue;
        },
        /*stats=*/nullptr, budget_, /*trail=*/nullptr, /*replay=*/nullptr,
        /*cabins=*/nullptr, &snapshot.value().snap, /*indexes=*/true, &gate);

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
    if (writer.row_count() > 0) SealAndDrain(tag, writer);

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
    // **A cancelled pipeline ships nothing more.** Its entry outlives the
    // CANCEL only so the parked producer can erase it, and a credit
    // arriving in that window must not turn the grace into a licence to
    // send: the pre-streaming shape erased in the handler and could not
    // ship past a cancel, so neither may this one.
    if (pipe.cancelled) return;

    // **One drain frame per pipeline.** send_ may deliver synchronously
    // (the loopback tests do), and the receiver's grant-on-receive credit
    // then re-enters here from inside the send - which, unlatched, popped
    // the queue and erased the pipeline under this frame's reference
    // (ASan-caught in the loopback sim while P4d-4a's review landed). The
    // outer loop re-tests can_send() per iteration, so a credit granted
    // mid-send is spent by the frame that already owns the loop.
    if (pipe.draining) return;
    pipe.draining = true;

    // By value before anything mutates the vector: `pipe` is a reference
    // into `pipelines_`, and the erase below would leave it dangling while
    // its tag was still being read.
    const PipelineTag tag = pipe.tag;
    const std::uint32_t downstream = pipe.downstream;

    while (!pipe.batches.empty() && !pipe.cancelled && pipe.credit.can_send()) {
        if (Status s = pipe.credit.ConsumeOnSend(); !s.ok()) break;  // unreachable; belt
        if (Status s = send_(downstream, sched::RingMessageKind::kStepBatch,
                             std::move(pipe.batches.front()));
            !s.ok()) {
            SendError(tag, tag.session_core, s);
            break;
        }
        pipe.batches.pop_front();
    }
    pipe.draining = false;

    // A cancel that landed inside a send: the loop stopped on it, and
    // teardown belongs to the producer if one is live, to us otherwise.
    if (pipe.cancelled) {
        if (!pipe.producing) Erase(tag);
        return;
    }
    // Still filling (the producer's walk is parked mid-relation, not
    // finished) or still waiting on credit: the edge stays open either
    // way, and the next seal or grant re-enters here.
    if (pipe.producing || !pipe.batches.empty()) return;

    // Everything sent: EOF closes the edge. Control, not data - it needs
    // no credit, exactly as CREDIT itself needs none coming back. The
    // erase comes *first*: a synchronous EOF send can carry back one more
    // inline message for this tag, and a torn-down tag is discarded by
    // §3's rule where a live one would be served twice.
    Erase(tag);
    StepEofPayload eof{tag};
    std::vector<std::byte> bytes;
    EncodePipelinePayload(eof, bytes);
    if (Status s = send_(downstream, sched::RingMessageKind::kStepEof, std::move(bytes));
        !s.ok() && log_ != nullptr && log_->enabled(LogLevel::kError)) {
        log_->Error("pipeline", "EOF for step " + std::to_string(tag.step_id) +
                                    " could not be sent: " + s.message());
    }
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
    if (pipe->producing || pipe->draining) {
        // A live producer owns its own teardown, and so does a drain
        // frame currently on the stack (a synchronous send can route a
        // cancel here from inside Drain's loop): the handler cannot erase
        // state either will touch again, so it marks, and the owner
        // tears down on its way out.
        pipe->cancelled = true;
        return;
    }
    Erase(eof.value().tag);
}

}  // namespace kds::server
