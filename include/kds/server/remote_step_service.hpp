#pragma once

#include <cstdint>
#include <limits>
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
#include "kds/server/session_step_client.hpp"
#include "kds/server/step_pipeline.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/visibility.hpp"

namespace kds::txn {
class TransactionManager;
}

namespace kds::sched {
class Scheduler;
class RingTransport;
}  // namespace kds::sched

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
    // says.
    //
    // **Shared across the siblings of one fan-in** (RD7, §5's fourth
    // cost): every sibling's enclosed open names the same
    // `downstream_step`, which is what keeps `OpenConsumingStage`'s
    // cross-check meaningful - it is the consumer they all feed, and a
    // sibling naming a different one is a mis-plan rather than a second
    // consumer.
    std::uint32_t downstream_step = 0;

    // **Which slice of the relation this stage is responsible for** (RD7).
    // A read of a split relation opens one stage per *maximal contiguous
    // run* of ranges on one core, and this half-open pk span is that run;
    // the stage walks the ranges it owns that fall inside it, and no
    // others.
    //
    // Per run and **not per core**, which is the correction the row's
    // review forced. Grouping by core emits `A₁, A₃, B₂` where ownership
    // interleaves - and interleaving is not a corner case, it is exactly
    // what R4's id-block-aligned insert spreading produces (`crosscore.md`
    // §6b). Per run restores true `lo` order, which is what makes a split
    // relation's answer byte-identical to the unsplit one (§8 test 9).
    //
    // `[0, kIdSpaceEnd)` is the whole relation, which is what every open
    // before RD7 meant and what an unsplit relation's single stage means
    // now - so a zeroed `range_hi` is the one value that would be wrong,
    // and the encoder never writes one.
    std::uint64_t range_lo = 0;
    std::uint64_t range_hi = catalog::kIdSpaceEnd;
};
static_assert(sizeof(StepOpenHead) == 40);

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
// **How wide a fan-in may be: the number of maximal same-owner runs**
// (RD7, corrected by R4-M/RS0 - `workplan-insert-spreading.md` §11a says
// what the old wording claimed and why it was wrong). The width equals the
// **core count** only under *contiguous* ownership; under interleaved
// ownership every run is one range and it equals the **range count**, which
// is exactly what R4's id-block-aligned insert spreading produces
// (`crosscore.md` §6b). R4-M measured the refusal firing at **65-72
// stages** against the old ceiling of 64, so the bound is a real limit on a
// spread relation's readable size - `kMaxFanInUpstreams x range_size_ids`
// rows (`bench/v2.6.0/results-k-sweep-and-read-ceiling-v2.4.0-52-g5b37fec.md`
// §6). Since DA1 and DA3 that arithmetic is 255 x 65,536 ~ **16.7 M rows**,
// which is **not measured**: §6's numbers are the 64 x 4,096 form.
// A **self-directed** run costs a slot like any other (R4-R §10b).
//
// **One constant, two quantities**, and they are different questions: the
// dispatcher's independent-stage count (`command_dispatcher.cpp`, not a
// wire quantity - each stage is its own single-step pipeline with its own
// `request_id`), and the STEP_OPEN envelope's upstream-edge count, which
// the wire carries in one byte.
//
// **255 since DA3** (2026-08-31, `instructions/v2.7.0/ratification-da.md`),
// up from 64, and **DA3's stated ground names the wrong one of the two**.
// The order reasoned from the one byte - *"the wire carries the upstream
// index in one byte, so 255 is reachable without a format change"* - which
// is true of the second quantity and irrelevant to the first, and it is the
// **first** that DA3 raises. The fan-in opens one *independent* pipeline
// per stage, each a whole-row STEP_OPEN carrying **zero** upstreams
// (`session_step_client.cpp`'s `Open`, called per stage from
// `command_dispatcher.cpp`'s stage loop), so a fan-in's width never reaches
// the upstream byte at all. 255 is free on the wire for a stronger reason
// than the order gives: this quantity has no wire representation.
//
// **And the byte is not what bounds the second quantity either.** A
// STEP_OPEN must fit one ring slot - `sched::kCoreRingPayloadBytes` = 1024,
// enforced at `MakeStepSend` - and 255 upstream edges do not, even with
// empty forwarded layouts. Production encodes 0 or 1 (the two-step join's
// `BuildTwoStepPipeline`), so nothing meets it today; a future shape that
// nested opens would meet the payload cap long before the count byte, and
// that is the constraint to size against.
//
// What 255 does cost is per-stage state on the session core - the park
// predicate and the teardown are both linear scans over the stage list, so
// quadratic in it - and that is DA-b's measurement, not the wire's.
//
// **Why the constant moved.** R4-M declined the same option because "4x
// does not change the shape of the problem", reasoning against a 4,096-id
// range where 255 stages buy ~1.04 M rows. DA1 made the range 65,536, and
// the two decisions together take the ceiling from 262,144 rows to roughly
// 16.7 M - a factor of 64, which neither answered alone
// (`bench/v2.6.0/results-k-sweep-and-read-ceiling-v2.4.0-52-g5b37fec.md`
// §6 measured the 64-stage form; the 255-stage form is DA-c's).
inline constexpr std::size_t kMaxFanInUpstreams = 255;

// The upstream count is one byte, so a ceiling above 255 would make
// `EncodeStepOpen`'s cast truncate an edge list into a short answer
// reported as a complete one. Unreachable from production, which encodes 0
// or 1, and asserted rather than trusted for that reason.
static_assert(kMaxFanInUpstreams <= 255,
              "the STEP_OPEN upstream count is one byte; a wider ceiling needs a format change");

std::vector<std::byte> EncodeStepOpen(const StepOpenHead& head,
                                      std::span<const std::byte> descriptor,
                                      std::span<const StepOpenUpstream> upstreams = {},
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
    // **Plural since RD7**: one entry per upstream stage of a fan-in, in
    // range order. Empty for a leaf; one entry for every chained open
    // before RD7 and every chained open over an unsplit relation after it.
    std::vector<StepOpenUpstream> upstreams;
    // The stage's output row, in the order its downstream decodes.
    // Empty = the whole row in schema order (the P4c shape).
    std::vector<StepOutputColumn> output;
    std::span<const std::byte> descriptor;
};
StatusOr<StepOpenParts> DecodeStepOpenEnvelope(std::span<const std::byte> payload);

// The one step-send, for every core that has a ring.
//
// Both production wirings built this by hand and differed in exactly
// three initialisers - `src_core`, `session_core`, and which scheduler
// they submitted to - which meant the oversize guard and its nine-line
// comment were written twice. `session_core` was the interesting one: the
// two disagreed (`0` against the destination) and **nothing on the step
// path reads it** - every reader in `remote_step_service.cpp` and
// `session_step_client.cpp` takes `tag.session_core` out of the payload,
// not the header. So this fills it from that same tag, which every step
// payload begins with, and the header states the truth for the first
// time instead of two different guesses - all three of the tag's fields,
// because filling one of three would read as authoritative and be false.
//
// **`scheduler` and `transport` are captured by reference and must
// outlive every server built from the returned seam.** That is the one
// thing a caller can get wrong, and both production wirings had to be
// corrected for it in opposite directions: `Expeditor::Serve` destroyed
// the transport while the endpoints still held it, and `~CoreRuntime`
// destroyed the scheduler the same way.
StepSendSeam MakeStepSend(sched::Scheduler& scheduler, sched::RingTransport& transport,
                          std::uint32_t src_core);

class RemoteStepServer {
public:
    using SendFn = StepSendFn;

    // Hands a producer task to this core's reactor. Empty selects the
    // synchronous collect-then-stream fallback (see the header comment).
    using SubmitFn = std::function<void(std::unique_ptr<sched::Task>)>;

    // The seam is **required and one argument**, so the ceiling cannot be
    // omitted and cannot come from a different transport than the sender.
    // A fixture passes `StepSendSeam{lambda}` and gets `kNoRingSlot` by
    // the struct's own default, which is the only place that default
    // lives.
    RemoteStepServer(catalog::Catalog& catalog, storage::PageStore& store, std::uint32_t core_id,
                     StepSendSeam seam, Logger* log = nullptr,
                     std::size_t batch_target_bytes = kStepBatchTargetBytes,
                     SubmitFn submit = {}, txn::TransactionManager* txns = nullptr,
                     exec::Budget budget = exec::Budget()) noexcept
        : catalog_(catalog),
          store_(store),
          core_id_(core_id),
          send_(std::move(seam.send)),
          log_(log),
          batch_target_(batch_target_bytes),
          // No special case for `kNoRingSlot`: `StepBatchCeiling(SIZE_MAX)`
          // is `SIZE_MAX - 24`, which no writer can approach either, and a
          // branch here would only invite the reader to think the function
          // misbehaves at the sentinel.
          batch_ceiling_(StepBatchCeiling(seam.max_message_bytes)),
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

        // The input edges, present only on a consuming stage (P4d-4b-2):
        // where its rows come from, each keyed by the *upstream* stage's
        // tag. Batches queue here raw; the consumer coroutine decodes
        // them, which keeps the message handler allocation-light and the
        // decode where the input schema lives.
        struct InputEdge {
            PipelineTag input_tag{};
            std::uint32_t upstream_core = 0;
            std::deque<std::vector<std::byte>> input;
            bool input_eof = false;
        };

        // **Plural since RD7** (§5's second cost), and the singular form
        // was a silent wrong answer waiting for a split relation: with k
        // upstreams, siblings 2..k's batches found no edge to match and
        // hit `FindInputEdge`'s "no consuming pipeline wants it - §3's
        // silent discard". Their rows simply vanished, and the statement
        // reported success.
        //
        // **In range order, and consumed in that order** (CC9's ascending
        // `lo`): `consumers[i]` is sibling `i`, and `consumed` names the
        // one the coroutine is draining. Strict concatenation rather than
        // interleaving, which is what makes the fan-in's output
        // range-ordered - and it is affordable because each edge's credit
        // ceiling bounds what a not-yet-consumed sibling may buffer, so a
        // waiting producer stalls on credit rather than filling memory.
        std::vector<InputEdge> consumers;
        std::size_t consumed = 0;

        // Whether this stage consumes at all - the test the singular
        // `std::optional` used to answer, kept as a name so no site
        // rediscovers that an empty vector means "a leaf stage".
        bool consuming() const noexcept { return !consumers.empty(); }
        // The edge being drained, or null once every sibling has ended.
        InputEdge* active() noexcept {
            return consumed < consumers.size() ? &consumers[consumed] : nullptr;
        }
    };

    Pipeline* Find(const PipelineTag& tag);
    // The **edge** `input_tag` feeds, or null - the one match OnStepBatch
    // and OnStepEof share. It answers the edge rather than the pipeline
    // because since RD7 a consuming stage has k of them, and a caller
    // handed the pipeline would have to re-run this search to know which
    // sibling's batch it is holding.
    Pipeline::InputEdge* FindInputEdge(const PipelineTag& input_tag);
    void Erase(const PipelineTag& tag);
    void Drain(Pipeline& pipe);

    // Places `row` in `writer`, sealing through `seal` first if it will
    // not fit - so no batch ever exceeds what the transport carries.
    //
    // **Exact, not predictive.** A batch is sealed *after* a row joins, so
    // any check on the size alone admits one row more than it meant to;
    // the first form of this fix predicted the next row's width from the
    // last, which holds only where the wire row width is constant, and it
    // is not. A NULL field costs 4 bytes and a present int64 costs 12
    // (`wire/row_codec.cpp`'s `PutField`), a varchar costs 4 + its length,
    // and `CommandDispatcher`'s shipping gate admits `SELECT *` over
    // nullable and text relations with no column-type restriction at all.
    // So the row is appended, measured, and **rolled back** into the next
    // batch when it does not fit - `RowBatchWriter::RollbackLastRow`, the
    // rollback `AppendRow` already performed internally.
    //
    // The one case sealing cannot answer is a single row wider than the
    // ceiling: it is refused rather than sent, because no batching policy
    // can make it fit and the alternative is the silent loss this whole
    // change exists to end (`step_pipeline.hpp` carries the retraction of
    // the "never stuck" rule that used to promise otherwise).
    Status PlaceRow(wire::RowBatchWriter& writer, const catalog::Schema& schema,
                    std::span<const parser::AstValue> row,
                    const std::function<void()>& seal);

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
    // `ups` is the fan-in, in range order (RD7). Every sibling forwards
    // the **same** layout and names the same `downstream_step` - they are
    // one step's stages - so the first decides the input schema and the
    // rest are checked against it rather than each building their own.
    //
    // Takes the **`TableAccess`**, not the schema it used to take: SA-T1's
    // re-derivation needs this core's indexes and cabin mask, and the
    // schema is one field of the thing that carries them. One parameter
    // fewer, not one more.
    void OpenConsumingStage(const StepOpenHead& head, std::span<const StepOpenUpstream> ups,
                            std::span<const StepOutputColumn> output, exec::Step step,
                            const catalog::TableAccess& access);

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
    // The hard bound `batch_target_` is only a target against.
    // `kNoRingSlot` - not the target - when the seam named no ring, so a
    // fixture never seals on the ceiling and only production tightens.
    std::size_t batch_ceiling_;
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

// **A core's step endpoints, wired once** (R4-R/RR2). Every core runs a
// client and a server sharing one send seam, and the six ring kinds route
// to them by a rule that is easy to get wrong in exactly one way:
//
//   kStepOpen / kStepCredit / kStepCancel  -> the server, which serves
//   kStepError                             -> the client, which opened
//   kStepBatch / kStepEof                  -> **both**, in one lambda
//
// The last line is the trap. `Scheduler::RegisterMessageHandler` *assigns*
// rather than appends, so registering the two claimants separately is a
// silent replacement — the survivor works and the other's messages vanish
// with nothing logged. Until RR2 only core 0 had a client, so the rule had
// one home and the peers' registration named only the server; giving every
// core a client made the two wirings identical and this is where they
// live, rather than in two files that must be kept in step by hand.
//
// Registration only: the caller owns both endpoints and their lifetimes,
// and hands the client to its dispatcher **after** this returns, so a
// reply cannot beat its receiver into existence.
Status WireStepEndpoints(sched::Scheduler& scheduler, SessionStepClient& client,
                         RemoteStepServer& server);

}  // namespace kds::server
