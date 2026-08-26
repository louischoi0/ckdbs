#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/budget.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/server/step_pipeline.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/visibility.hpp"

namespace kds::txn {
class TransactionManager;
}

// The remote step server (docs/spec/crosscore.md §2-§4, workplan P4b): an owning
// core takes a STEP_OPEN, executes the described step against its **local**
// relation state, and streams STEP_BATCHes to the downstream core under
// credit, EOF at the end, ERROR with the code and the retryable bit on any
// failure. Single-step chains only in P4b - the shippable class the
// descriptor codec already enforces, narrowed further here to what a lone
// step can mean: every column reference resolves within the step (`up == 0`)
// and a key must be a literal, because "a value produced by an earlier
// step" has no earlier step to come from.
//
// **Execution shape: stream-under-credit when a reactor is present**
// (workplan P4d-4a). With a `SubmitFn`, STEP_OPEN submits a producer
// coroutine: the step runs through `exec::ExecuteAsync` with a resume
// gate, seals a batch at the size target, ships it while a credit is
// held, and **parks at the page boundary** - holding no pin and no span
// (P4d-3) - when sealed batches wait on credit. Buffering is therefore
// bounded by the credit ceiling plus what one page can seal, not by the
// relation. A STEP_CREDIT drains the queue and the parked walk resumes
// through its gate; a STEP_CANCEL stops it at the next row or boundary.
//
// Without a `SubmitFn` (tests with no reactor), the P4b shape survives
// as the fallback: collect-then-stream through the synchronous
// `exec::Execute`, drain on credit. The wire protocol is identical in
// both shapes - no batch without a credit, grants bounded by the
// preallocated ceiling, EOF after the last batch ships.
//
// The sender is injected (`SendFn`) so every rule here is testable without
// a reactor; `CoreRuntime` wires it to the real ring through the send-retry
// task. Handlers, drain and the producer all run on the owning core's
// thread - no locks, like everything else on a core (rules.md #3).

namespace kds::server {

// The STEP_OPEN envelope: the head, an optional upstream-edge section
// (P4d-4b), then the step descriptor (step_descriptor.hpp) as the
// remainder of the payload.
struct StepOpenHead {
    PipelineTag tag{};
    std::uint32_t downstream_core = 0;
    // The step id whose pipeline consumes this stage's batches at the
    // downstream core (workplan P4d-4b fact 2). Zero for the session's
    // own read - unambiguous because step ids are assigned in compile
    // order from zero, so a step that consumes an upstream edge always
    // has a producer numbered below it and can never itself be step 0.
    // Every pre-4b encoder's zero therefore already meant what it now
    // says. Keeps the head padding-free at 24 bytes.
    std::uint32_t downstream_step = 0;
};
static_assert(sizeof(StepOpenHead) == 24);

// One column of a stage's output row: either a pass-through of an
// input-layout column or a column of the stage's own relation, in output
// order. What a stage forwards is decided by the session at plan time
// (fact 3) and told to the stage here - a stage never guesses what its
// downstream needs. A **leaf** stage has no input layout, so its spec may
// only name local columns (P4d-4b-3); an absent spec means the whole row
// in schema order, which is exactly the P4c shape, so every pre-4b-3
// encoder's envelope still means what it meant.
struct StepOutputColumn {
    std::uint8_t from_upstream = 0;  // 1: `index` into the forwarded layout
    std::uint16_t index = 0;         // 0: `index` into the local relation's schema
};

// The upstream edge of a chained open (workplan P4d-4b fact 1): a
// consuming stage learns what its input batches carry and receives the
// *enclosed* open it must forward to the upstream core once its own
// state exists - which is what makes "no batch before its consumer" hold
// by construction rather than by racing two opens.
struct StepOpenUpstream {
    std::uint32_t upstream_core = 0;
    // The batch row layout, in order: the upstream relation's column
    // rows for exactly the forwarded columns (fact 3). Full catalog rows
    // rather than (pos, type) pairs because decode and re-encode both
    // need the width/scale semantics only SysColumnRow carries.
    std::vector<catalog::SysColumnRow> forwarded;
    // The upstream stage's complete STEP_OPEN payload, forwarded verbatim.
    std::vector<std::byte> enclosed_open;
};

// The output spec stands beside the upstream section, not inside it
// (P4d-4b-3): a leaf has an output too - the forwarded layout it seals
// for its consumer - and a section owned by the upstream half could
// never say so. Empty means the whole row.
std::vector<std::byte> EncodeStepOpen(const StepOpenHead& head,
                                      std::span<const std::byte> descriptor,
                                      const StepOpenUpstream* upstream = nullptr,
                                      std::span<const StepOutputColumn> output = {});

// One STEP_BATCH framing for every producer of one - the server's Seal,
// the tests' hand-built inputs, and the session side to come. Leaves the
// writer empty and reusable, as RowBatchWriter::Finish promises.
std::vector<std::byte> EncodeStepBatch(const PipelineTag& tag, std::uint32_t seq,
                                       wire::RowBatchWriter& writer);

// Splits an envelope into its parts. `descriptor` is a view into
// `payload` - the caller keeps the payload alive across the decode of
// what it points at, which every message handler already does.
struct StepOpenParts {
    StepOpenHead head{};
    std::optional<StepOpenUpstream> upstream;
    // The stage's output row, in the order its downstream decodes.
    // Empty = the whole row in schema order (the P4c shape).
    std::vector<StepOutputColumn> output;
    std::span<const std::byte> descriptor;
};
StatusOr<StepOpenParts> DecodeStepOpenEnvelope(std::span<const std::byte> payload);

class RemoteStepServer {
public:
    // `send(dst_core, kind, payload)` must deliver or report; it never
    // blocks (the real one submits a send-retry task).
    using SendFn =
        std::function<Status(std::uint32_t, sched::RingMessageKind, std::vector<std::byte>)>;

    // Hands a producer task to this core's reactor. Empty selects the
    // synchronous collect-then-stream fallback (see the header comment).
    using SubmitFn = std::function<void(std::unique_ptr<sched::Task>)>;

    RemoteStepServer(catalog::Catalog& catalog, storage::PageStore& store, std::uint32_t core_id,
                     SendFn send, Logger* log = nullptr,
                     std::size_t batch_target_bytes = kStepBatchTargetBytes,
                     SubmitFn submit = {}, txn::TransactionManager* txns = nullptr,
                     exec::Budget budget = exec::Budget()) noexcept
        : catalog_(catalog),
          store_(store),
          core_id_(core_id),
          send_(std::move(send)),
          log_(log),
          batch_target_(batch_target_bytes),
          submit_(std::move(submit)),
          txns_(txns),
          budget_(budget) {}

    // The kStepOpen handler: decode, validate the single-step class,
    // execute, queue, drain. A failure at any point answers STEP_ERROR to
    // the session core and opens nothing.
    void OnStepOpen(const sched::MessageHeader& header, std::span<const std::byte> payload);

    // The kStepCredit handler: grants and resumes the drain. A tag
    // matching no live pipeline is discarded silently - §3's teardown
    // rule, correctness rather than an error.
    void OnStepCredit(std::span<const std::byte> payload);

    // The input-edge handlers (P4d-4b-2): a batch or EOF whose tag names
    // a consuming pipeline's upstream edge queues there; anything else is
    // §3's silent discard.
    //
    // **A scheduler holds exactly one handler per message kind** - the
    // registration map assigns, it does not append. On a peer these are
    // the kind's only claimant. On core 0 - which hosts a session client
    // *and*, since P4d-4b-3, this server - the expeditor registers one
    // lambda per kind that calls both consumers in turn: safe exactly
    // because both discard unmatched tags silently, so the tag is the
    // demultiplexer and neither consumer can see the other's edge.
    void OnStepBatch(std::span<const std::byte> payload);
    void OnStepEof(std::span<const std::byte> payload);

    // The kStepCancel handler: drops the pipeline whole, sends nothing.
    void OnStepCancel(std::span<const std::byte> payload);

    std::size_t open_pipelines() const noexcept { return pipelines_.size(); }

    // Sealed-but-unsent batches across every open pipeline. What the
    // bounded-buffering test reads: streaming's whole point is that this
    // stays around one page's seals plus the credit ceiling, where
    // collect-then-stream held the relation.
    std::size_t unsent_batches() const noexcept {
        std::size_t n = 0;
        for (const Pipeline& pipe : pipelines_) n += pipe.batches.size();
        return n;
    }

private:
    struct Pipeline {
        PipelineTag tag{};
        std::uint32_t downstream = 0;
        EdgeCredit credit;
        // Sealed, unsent batches: sent ones are popped, so the deque IS
        // the backlog - a streaming pipeline must not accumulate husks of
        // what it already shipped.
        std::deque<std::vector<std::byte>> batches;
        std::uint32_t seq = 0;     // next batch's per-edge sequence number
        // Streaming state (P4d-4a). While `producing`, Drain must not EOF
        // however empty the queue looks - the walk is parked mid-relation,
        // not finished. `cancelled` is how a CANCEL reaches a live
        // producer: the state must outlive the message handler, so the
        // handler marks it and the producer erases itself at the next
        // sink call or page boundary.
        bool producing = false;
        bool cancelled = false;
        // Drain's reentrancy latch. A synchronous SendFn (the loopback
        // tests deliver inline) can carry a batch out, bring the
        // grant-on-receive credit back, and re-enter Drain for this same
        // pipeline all inside one send_ call - the ASan-caught shape that
        // double-popped the queue and erased the pipeline under the outer
        // loop's reference. While set, a re-entered Drain returns and the
        // outer frame's loop condition picks the new credit up itself.
        bool draining = false;

        // The input edge, present only on a consuming stage (P4d-4b-2):
        // where its rows come from, keyed by the *upstream* stage's tag.
        // Batches queue here raw; the consumer coroutine decodes them,
        // which keeps the message handler allocation-light and the
        // decode where the input schema lives.
        struct InputEdge {
            PipelineTag input_tag{};
            std::uint32_t upstream_core = 0;
            std::deque<std::vector<std::byte>> input;
            bool input_eof = false;
        };
        std::optional<InputEdge> consumer;
    };

    Pipeline* Find(const PipelineTag& tag);
    // The consuming pipeline whose input edge `input_tag` feeds, or null -
    // the one match OnStepBatch and OnStepEof share.
    Pipeline* FindByInputTag(const PipelineTag& input_tag);
    void Erase(const PipelineTag& tag);
    void Drain(Pipeline& pipe);
    void Seal(Pipeline& pipe, wire::RowBatchWriter& writer);
    void SendError(const PipelineTag& tag, std::uint32_t session_core, const Status& status);

    // The one credit gate and the one seal-then-ship, shared by producer
    // and consumer: when a batch may ship is a protocol rule, and a rule
    // with two spellings is a correctness bug with two homes.
    std::function<bool()> CreditGate(const PipelineTag& tag);
    void SealAndDrain(const PipelineTag& tag, wire::RowBatchWriter& writer);

    // crosscore.md §7's upstream half: a stage that stops consuming -
    // error or cancel - tells its producer to stop too, or that producer
    // parks on its credit gate for the process's life. A duplicate cancel
    // from the session later is harmless under §3's teardown rule.
    void CancelUpstream(const PipelineTag& input_tag, std::uint32_t upstream_core);

    // OnStepOpen's consuming branch (P4d-4b-2): validates the edge and
    // the normalized references, builds the input/output schemas, opens
    // the pipeline, submits RunConsumer, and forwards the enclosed open
    // upstream - in exactly that order, because the chained-open contract
    // is "state first, upstream last".
    void OpenConsumingStage(const StepOpenHead& head, const StepOpenUpstream& up,
                            std::span<const StepOutputColumn> output, exec::Step step,
                            const catalog::Schema& schema);

    // The streaming producer (P4d-4a): one coroutine per open pipeline,
    // owning the writer and the executor run. It re-finds its Pipeline by
    // tag at every touch - the vector reallocates and CANCEL erases, so a
    // held pointer would be the dangling-reference bug the dispatcher's
    // remote read already solved the same way. `output` narrows the row
    // it seals to the spec's local columns (P4d-4b-3); empty keeps the
    // whole row.
    sched::Coro RunProducer(PipelineTag tag, exec::StepChain chain,
                            std::vector<std::uint16_t> output);

    // The consuming stage (P4d-4b-2): parks until input arrives, decodes
    // each upstream row into a one-slot outer frame, runs the local step
    // against it through the executor's parent-frame machinery (fact 4),
    // seals the mixed output row downstream under the same credit
    // machinery the producer uses, and grants the upstream one credit
    // per consumed batch. Same re-find-by-tag discipline as RunProducer.
    sched::Coro RunConsumer(PipelineTag tag, exec::StepChain chain,
                            catalog::Schema input_schema, std::vector<StepOutputColumn> output,
                            catalog::Schema output_schema);

    catalog::Catalog& catalog_;
    storage::PageStore& store_;
    std::uint32_t core_id_;
    SendFn send_;
    Logger* log_;
    std::size_t batch_target_;
    SubmitFn submit_;
    // The manager whose committed state every stage on this core reads at
    // (crosscore.md CC4). **No view crosses a core**: each stage mints its
    // own from *its* core through `txn::AutocommitSnapshot`, once, held
    // across its parks - which is the per-core weakening of REPEATABLE
    // READ `docs/inflight/known-gaps.md` records. Null (the reactorless protocol
    // tests) means every writer visible, exactly the pre-MVCC behaviour.
    txn::TransactionManager* txns_;
    // The row-touch ceiling every stage on this core runs under - **this
    // core's** configured limit, which the server ignored before P4d-4c's
    // review (`exec::Budget()` fresh at each call site). A homogeneous
    // deployment configures every core alike, so this is the session's
    // limit too; carrying the session's own across a heterogeneous one is
    // an envelope field, recorded in the workplan rather than guessed at.
    exec::Budget budget_;
    std::vector<Pipeline> pipelines_;
};

}  // namespace kds::server
