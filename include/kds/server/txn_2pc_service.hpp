#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"

// **Cross-owner transactions: the wire** (R6-1 of `instructions/v2.4.0/2pc.md`).
//
// A transaction whose writes touch relations owned by two or more cores is
// refused today, and the refusal is an abstraction leak: `owner_core` is an
// engine decision the user did not make and cannot see (§0 of the work
// order). R6 commits such a transaction atomically instead, by the two-phase
// protocol D4 states. This header is the **wire half only** - what crosses
// the ring, in what shape, and the confirmation that it fits.
//
// What is *not* here, deliberately: the coordinator's parked waiter over N
// participants, the participant's durable prepare, the decision record, the
// in-doubt wait. Those are R6-3 and R6-5, and each needs a seam this file
// does not invent. R6-1's whole product is the message kinds, the payloads,
// and D6's sizing answer.
//
// ---- The protocol these four kinds carry (D4) ---------------------------
//
//   1. **Prepare.** The coordinator - the arrival core, D1 - sends prepare
//      to every participant. A participant makes its work durable in *its
//      own* stream, writes a PREPARE record naming the coordinator's
//      `(session_id, transaction_id)`, and replies prepared or refused.
//      **After replying prepared it may not unilaterally abort.**
//   2. **Decide.** All prepared, and the coordinator writes COMMIT to its
//      own stream - **that record is the decision**. Any refusal or timeout
//      and it writes ABORT. Either way it then tells the participants, which
//      write their own COMMIT/ABORT and release.
//
// The decision lives in **exactly one stream**, the coordinator's, because a
// decision assembled from several streams would be a cross-stream ordering
// question and `wal.md` guideline 3 forbids one. Recovery (R6-4) reads the
// coordinator's stream to learn the outcome and each participant's to learn
// what to redo.
//
// ---- Why the decide leg is acknowledged ---------------------------------
//
// D4 does not name an ack, and the decision's *durability* does not need
// one: once the coordinator's COMMIT is durable the transaction is
// committed, and a participant that never heard is resolved by recovery or
// by D5's in-doubt ask. The ack exists for the two things that are not
// durability - the coordinator cannot otherwise know when a participant has
// released, and a decide message the ring dropped would leave that
// participant in doubt until R6-5's ceiling rather than until the next
// resend. It costs one enum value and no new payload - the ack rides the
// same `TxnParticipantReplyPayload` the prepare leg answers on; the
// alternative is a protocol whose only repair path is the slow one.
//
// ---- The identity, and why the coordinator's core is not a field --------
//
// A participant keys its side on **(coordinator core, session id,
// transaction id)**. The core is `MessageHeader::src_core` and is
// deliberately *not* repeated in the payload - but it is part of the
// identity all the same, for the reason `ShippedStatementExecutor`'s dedup
// key already carries it: **a session id is minted per core, so two cores
// mint the same one**, and a record keyed on the id alone would answer one
// coordinator's transaction with another's. The transaction id is the
// coordinator's own (D2); a participant runs under a local transaction of
// its own, from its own lease, and holds this pair beside it. One global
// trx id was rejected - it needs either a global counter (guideline 1) or
// cross-stream id ordering (`wal.md` §3), and per-participant local ids with
// a coordinator mapping is what keeps every stream's ids stream-local.
//
// On the reply legs the identity rides back so an answer can be checked
// against the waiter the ring matched it to rather than trusted - SS1's
// rule, and the one failure this protocol must not have is answering one
// transaction with another's result.
//
// ---- The retry bit, inherited rather than reinvented (R6-0) -------------
//
// Both request legs carry `retry`, with exactly the meaning R6-0 gave the
// shipped statement's: 0 on a first send, 1 on a resend of a request already
// sent once. §2 of the work order requires this - R6 introduces the first
// real retry path, so a resend that meets no record must be answerable
// `UnknownOutcome` rather than re-executed, and R6's own messages inherit
// that discipline instead of inventing a second one.

namespace kds::server {

// ---- The wire forms -----------------------------------------------------
//
// POD, under `ring_message.hpp`'s exception to the on-disk rules: they never
// leave the process, so no explicit shift/mask encoding is owed. What is
// still owed - fixed-width types, no padding surprises, trivially copyable -
// is asserted below rather than assumed, because the ring copies these byte
// for byte.

// Coordinator -> participant: prepare this transaction.
//
// Carries ids and a flag, no statement text: what the participant is being
// asked to prepare is work it has already run, not anything re-sent here.
// It finds that work by `(src_core, session_id)` - the pair every shipped
// statement already arrives under. `transaction_id` is the coordinator's and
// does **not** cross on the statement path, so it is what the participant
// *records* here beside its own local id (D2), never what it looks up by.
struct TxnPrepareRequestPayload {
    std::uint64_t session_id;      // the coordinator's
    std::uint64_t transaction_id;  // the coordinator's (D2)
    std::uint8_t retry;            // R6-0's bit, same meaning
    std::uint8_t reserved0[7];
};
static_assert(sizeof(TxnPrepareRequestPayload) == 24);

// What a decide message tells the participant to do.
//
// **0 names nothing**, the zero-collision rule `RingMessageKind` and
// `StoredAccessKind` each keep: a zeroed buffer must not decode as a real
// decision, and of the two real values the one it would decode as is the
// one that silently discards a committed transaction.
enum class TxnDecision : std::uint8_t {
    kUnset = 0,
    kCommit = 1,
    kAbort = 2,
};

// Coordinator -> participant: the decision, which the coordinator's stream
// already holds.
struct TxnDecideRequestPayload {
    std::uint64_t session_id;
    std::uint64_t transaction_id;
    std::uint8_t decision;  // TxnDecision
    // R6-0's bit, but **not R6-0's meaning**, and the difference is worth
    // the line: there is nothing to re-execute here, so a resend that meets
    // no prepared state is a no-op rather than a risk. What it separates is
    // a benign resend - the ack was lost after the participant released -
    // from a decide for a transaction this core never prepared, which is a
    // protocol anomaly worth saying so about. Without it the two are one
    // case and the anomaly is invisible.
    std::uint8_t retry;
    std::uint8_t reserved0[6];
};
static_assert(sizeof(TxnDecideRequestPayload) == 24);

// Bytes of refusal text a participant's reply carries. A prepare that
// refuses has a reason the client should see, and it crosses as a code plus
// a message so that `Status::FromWire` rebuilds the participant's own
// spelling - the `retryable` bit included, which is the one bit a client's
// retry loop reads (`docs/spec/protocol.md` §11). `txn/manager.cpp` states
// the rule for this class of string: the message is part of the wire
// contract, not a diagnostic.
//
// **Sized against the refusals a participant can actually produce**, not
// rounded to a power of two. The first cut of this file chose 104 so that
// `sizeof` landed on 128, and 104 is three bytes under the engine's single
// most likely prepare failure - `extent_lease.cpp`'s "this core's lease of
// N pages is spent; a refill must be granted before it can allocate again",
// which measures 107 at N=64 and grows with N. That is the inverse of D6's
// warning: nothing was shrunk to fit the *slot*, but a field was cut to fit
// a self-chosen total while 896 bytes of confirmed headroom went unspent.
//
// The measured population this must hold, longest first: 157
// (`shipped_statement_executor.cpp`'s UnknownOutcome), 144 (`TxnDecisionOf`
// below), 109 (a spent lease at a four-digit count), 51 (a write-write
// conflict). 232 clears the longest by 48% and the reply is still a quarter
// of the slot, so D6's headroom answer stays meaningful rather than being
// spent to the last byte.
inline constexpr std::size_t kTxnParticipantReplyBytes = 256;
inline constexpr std::size_t kTxnParticipantReplyFixedBytes = 24;
inline constexpr std::size_t kTxnParticipantReplyMessageMax =
    kTxnParticipantReplyBytes - kTxnParticipantReplyFixedBytes;

// Participant -> coordinator, on **both** reply legs.
//
// One struct rather than two identical ones: a prepare reply and a decide
// ack differ in what they answer *about*, which is the message kind, and not
// in a single field. `status_code` is a StatusCode - 0 is prepared (or
// applied) and anything else is the refusal, whose message is in `message`.
//
// `message_len` bounds `message` rather than a NUL terminating it. That is
// SS1's discipline and not `IndexBuildReplyPayload`'s: these are bytes this
// core did not compute, and an explicit length that the reader bounds
// against the array is what stands between a forged payload and a read past
// it. A NUL that is simply absent has no such backstop.
struct TxnParticipantReplyPayload {
    std::uint64_t session_id;
    std::uint64_t transaction_id;
    std::uint32_t status_code;
    std::uint16_t message_len;
    std::uint8_t reserved0[2];
    char message[kTxnParticipantReplyMessageMax];  // not NUL-terminated
};
static_assert(sizeof(TxnParticipantReplyPayload) == kTxnParticipantReplyBytes);
// The fixed part is what the message max is derived *from*, so a field added
// above without adjusting it would silently overrun the total above.
static_assert(offsetof(TxnParticipantReplyPayload, message) ==
                  kTxnParticipantReplyFixedBytes,
              "kTxnParticipantReplyFixedBytes must be what the header above `message` costs");

// ---- R6-5: the in-doubt ask, the third exchange (D5) ---------------------
//
// The only leg a **participant** opens. A core that replied prepared and
// has waited out `kTxnInDoubtCeilingNs` with no decide asks its coordinator
// what it decided. Two properties, each one a line here:
//
//   - **The ask is a retry, and says so.** `retry` is R6-0's bit and is
//     always 1 on this leg - there is no first attempt, since the decide
//     the coordinator already sent was the first. A coordinator that no
//     longer holds the record therefore answers `UnknownOutcome` and never
//     re-decides (D5); an ask that arrives with the bit clear is refused as
//     the anomaly it is, because that is the one way the contract known-gaps
//     names - "every retry path built from R6-3 on has to set it" - could be
//     violated by a sender and go unnoticed.
//   - **The answer carries the decision or the reason there is none, and
//     no words.** A participant is not a client: nothing it is told here is
//     rendered for anyone, so the reply is a code and a decision byte. The
//     code says which of three things is true - decided (`kOk`, and the
//     byte is `kCommit`/`kAbort`), not decided yet (`kTxnConflict`: the
//     coordinator's prepare phase is still open, ask again after another
//     ceiling), or unknowable (`kUnknownOutcome`: the record is gone, and
//     the next mount resolves it against the coordinator's stream, R6-4).

// Participant -> coordinator: what did you decide for this transaction?
struct TxnResolveRequestPayload {
    std::uint64_t session_id;      // the coordinator's
    std::uint64_t transaction_id;  // the coordinator's (D2)
    std::uint8_t retry;            // R6-0's bit; **always 1** on this leg
    std::uint8_t reserved0[7];
};
static_assert(sizeof(TxnResolveRequestPayload) == 24);

// Coordinator -> participant: the decision, or why there is none.
struct TxnResolveReplyPayload {
    std::uint64_t session_id;
    std::uint64_t transaction_id;
    std::uint32_t status_code;  // StatusCode: kOk decided, kTxnConflict not yet, kUnknownOutcome gone
    std::uint8_t decision;      // TxnDecision; kUnset unless status_code is kOk
    std::uint8_t reserved0[3];
};
static_assert(sizeof(TxnResolveReplyPayload) == 24);

// The ring copies these byte for byte, which is only defined for a
// trivially copyable type. `sched::SubmitSendPod` asserts the same thing of
// whatever it is handed (send_retry.hpp), so these are the *earlier* check:
// R6-1 has no sender, and without them a payload that stopped being
// trivially copyable would go unnoticed until R6-3 first sent one.
static_assert(std::is_trivially_copyable_v<TxnPrepareRequestPayload>);
static_assert(std::is_trivially_copyable_v<TxnDecideRequestPayload>);
static_assert(std::is_trivially_copyable_v<TxnParticipantReplyPayload>);
static_assert(std::is_trivially_copyable_v<TxnResolveRequestPayload>);
static_assert(std::is_trivially_copyable_v<TxnResolveReplyPayload>);

// ---- D6: the sizing answer, which R6-1 exists to give -------------------
//
// D6 requires R6-1 to **confirm** that R6's messages fit the ring slot that
// exists rather than assume it, and to stop and report if any does not -
// never to shrink a field to make one fit. They fit, with room that is not
// close: the largest is 256 bytes against a 1,024-byte slot, and the two
// request legs are 24. `crosscore.md` §9's payload-sizing decision is
// therefore R6's *neighbour* and not its gate, which is what D6 asked to be
// established here.
//
// **What this answer covers**: the prepare and decide legs, which were
// R6-1's scope, and since R6-5 the in-doubt ask as well - the third
// exchange D5 needs, which R6-1 said was R6-5's to declare and to size. It
// carries ids and a bit, and the answer a code and a byte: 24 bytes each
// leg, the smallest messages in the protocol. The sizing obligation §2 of
// the workplan's ratification record left open for this leg is discharged
// by the two assertions that end this block.
//
// The assertions are written against `kCoreRingPayloadBytes` rather than
// against 1,024, so that a future resize of the slot re-checks them instead
// of leaving a stale number in a comment.
static_assert(sizeof(TxnPrepareRequestPayload) <= sched::kCoreRingPayloadBytes,
              "R6-1/D6: a prepare must fit one ring slot; if it ever does not, "
              "crosscore.md §9's sizing decision becomes R6's gate - do not shrink a field");
static_assert(sizeof(TxnDecideRequestPayload) <= sched::kCoreRingPayloadBytes,
              "R6-1/D6: a decide must fit one ring slot; if it ever does not, "
              "crosscore.md §9's sizing decision becomes R6's gate - do not shrink a field");
static_assert(sizeof(TxnParticipantReplyPayload) <= sched::kCoreRingPayloadBytes,
              "R6-1/D6: a participant reply must fit one ring slot; if it ever does not, "
              "crosscore.md §9's sizing decision becomes R6's gate - do not shrink a field");
static_assert(sizeof(TxnResolveRequestPayload) <= sched::kCoreRingPayloadBytes,
              "R6-5/D6: an in-doubt ask must fit one ring slot; if it ever does not, "
              "crosscore.md §9's sizing decision becomes R6's gate - do not shrink a field");
static_assert(sizeof(TxnResolveReplyPayload) <= sched::kCoreRingPayloadBytes,
              "R6-5/D6: an in-doubt answer must fit one ring slot; if it ever does not, "
              "crosscore.md §9's sizing decision becomes R6's gate - do not shrink a field");

// ---- The decodes, and R6-3's encodes ------------------------------------
//
// The decodes bound bytes this core did not compute. R6-1 had no matching
// *encodes* because nothing sent these; R6-3 sends them, and the encoder
// the header said it owed - a UTF-8-safe truncation of the refusal message -
// is `utf8_prefix.hpp`, hoisted out of `statement_ship_service.cpp` at this
// row, on the condition R6-1 named.

// The decision a decide message names, **refused rather than guessed** when
// the byte names none.
//
// Fail-closed here does not mean "abort": a participant that has replied
// prepared may not unilaterally abort, and one that has not may not commit,
// so neither value is the safe reading of an unreadable byte. Refusing
// leaves the participant *in doubt*, which is the honest state and the one
// D5's resolution ask is built to end.
StatusOr<TxnDecision> TxnDecisionOf(const TxnDecideRequestPayload& decide);

// The refusal message a reply carries, bounded against the array rather
// than trusted. An empty message is legal - a success carries none - so
// only a length past the array is a refusal.
StatusOr<std::string_view> TxnParticipantReplyMessageOf(const TxnParticipantReplyPayload& reply);

// The reply encode (R6-3). A refusal's message is **diagnostic** - what the
// coordinator acts on is the code, which crosses whole - so an over-long
// message is truncated at a character boundary and the participant's log
// keeps all of it. That is `ShippedStatementReplyOf`'s rule for a refusal
// and it applies here without its asymmetry: a participant reply carries no
// answer, only an outcome, so there is no success text a cap could silently
// shorten.
TxnParticipantReplyPayload TxnParticipantReplyOf(std::uint64_t session_id,
                                                 std::uint64_t transaction_id,
                                                 const Status& status);

// ---- R6-3: how long a coordinator waits for one phase --------------------
//
// `kShippedStatementDeadlineNs`'s argument, applied to a phase rather than
// to a statement, and it lands on the same number for the same reason: this
// is not a latency budget but the point past which a reply is presumed lost
// rather than slow. A prepare costs a participant one `fdatasync` (~0.9 ms
// on this host) plus two ring hops (~20 us each), so ten seconds is three
// orders of magnitude above the work.
//
// **The two phases read a timeout differently, and that is the whole reason
// one constant is enough.** A prepare that times out is an ABORT: nothing
// was decided, so nothing committed anywhere, and the coordinator is free
// to take the safe branch. A decide that times out changes no outcome at
// all - the decision is already durable in the coordinator's stream - so
// the wait is only for the release, and its expiry leaves a participant in
// doubt for D5's resolution rather than leaving the transaction undecided.
// Being generous is therefore cheap on both legs and wrong on neither.
inline constexpr sched::MonoTimeNs kTxnPhaseDeadlineNs = 10ull * 1'000'000'000ull;

// ---- R6-5: the in-doubt ceiling (D5's `[OPEN]`, ratified 2026-08-28) -----
//
// **One number, two waits, and both end in something named.** A prepared
// participant that has heard no decide for this long asks its coordinator
// (`Txn2pcServer::Ask`), and asks again every ceiling until it is answered.
// A writer on that core that meets a row the in-doubt transaction holds
// parks for at most this long (`CommandDispatcher`'s `InDoubtBlock`) and is
// then refused - **retryably and by name, and not `UnknownOutcome`**: its
// own statement did nothing and may be retried, which is the opposite of
// what that code tells a client. The operator ratified "block, with a
// bounded ceiling ending in a named refusal" over "refuse retryably up
// front"; this is the bound.
//
// **Derived, not picked, and the derivation is the healthy decision
// window on this host.** From a participant's prepare reply to its decide:
// the coordinator's decision sync (~0.94 ms single-stream,
// `bench/v2.1.0/results-multicore-writers-v2.1.0.md` §3 - 1,066/s - and
// `results-shipping-pretasks-v2.1.0-10-g82a2749.md` §3a - 1,118/s), any
// sibling participant's prepare sync still in flight (the same ~0.94 ms;
// four streams overlap at 3.371× on this volume, §3a, so a four-wide
// prepare costs each participant ~1.1 ms rather than 4 × 0.94), and two
// ring hops at ~21 µs (§3a's "21-23 µs against a ~0.9 ms sync"). About
// 2 ms at the median. The tail is where M3 found shipping's cost (+76% p99
// against +11% p50, `bench/v2.4.0/results-m3-*.md`), and the largest
// unattributed latency on this host is the ~11 ms periodic stall
// (`bench/results-knob-sweep-cell2` §5). 200 ms is ~100× the healthy window
// and ~18× the stall: a writer never meets the refusal on a healthy path,
// and one that meets a genuinely lost decision is refused inside a fifth
// of a second rather than after the coordinator's ten.
//
// **It is deliberately under `kTxnPhaseDeadlineNs`**, and that is a choice
// rather than an oversight. A coordinator may legitimately take up to that
// deadline to decide when another participant is silent, so an ask sent
// inside that window is answered "not yet decided" and costs one round trip
// per ceiling, and a writer refused inside it is refused for a transaction
// that is not lost but slow. Sizing the ceiling to the coordinator's worst
// case instead would make every blocked writer pay a failure's price on a
// healthy day; the ratified rule is a ceiling on the stall, and the stall
// is the writer's.
//
// **Reached through one function** - `CommandDispatcher::InDoubtCeilingNs()`
// - and config-swept as `in_doubt_ceiling_ms` (§2's obligation). This is the
// default and the proposal; the sweep that measures it needs a live
// cross-owner path, which is R6-8's, so it is RP8's cell and not this row's.
inline constexpr sched::MonoTimeNs kTxnInDoubtCeilingNs = 200ull * 1'000'000ull;
static_assert(kTxnInDoubtCeilingNs < kTxnPhaseDeadlineNs,
              "a writer blocked on an in-doubt row must be refused inside the window in which "
              "its coordinator may still legitimately be deciding, not after it - and a "
              "shipped writer must be refused before its own arrival core presumes it lost, "
              "which is the same number (kShippedStatementDeadlineNs, statement_ship_service.hpp)");

// How long the coordinator keeps a decision a participant may still ask
// about. **Not a correctness bound** - a participant that asks after this
// is answered `UnknownOutcome` and resolves at the next mount (R6-4), which
// is D5's own answer - but a bound on how long a rare failure can keep
// asking before it is told to stop. Ten times the longest legitimate
// silence in the protocol, the phase deadline: a participant whose every
// ask and answer inside a hundred seconds was lost is not a participant the
// ring is going to reach. A record is dropped the moment every participant
// acknowledges its decide, so on a healthy path nothing is ever held this
// long, or at all.
inline constexpr sched::MonoTimeNs kTxnDecisionRetentionNs = 10 * kTxnPhaseDeadlineNs;
static_assert(kTxnDecisionRetentionNs > kTxnPhaseDeadlineNs + 2 * kTxnInDoubtCeilingNs,
              "a participant must be able to ask at least once after the coordinator's "
              "slowest legitimate decision and still find the record");

// The memory bound under the time bound, `kShippedDedupMaxRecords`'s shape:
// a record is held only for a transaction whose decide some participant did
// not acknowledge, which is a failure population, so a cap this size is a
// ceiling on a storm and not on a workload. Past it the oldest record is
// dropped early and counted (`Txn2pcClient::decisions_evicted()`).
//
// Trimming happens **before** a record is opened rather than after, so that
// the record being opened can never be the one dropped - which would answer
// `UnknownOutcome` about a transaction whose prepare is going out in the
// next line. The map therefore holds at most this many plus the one just
// opened, which is the bound stated exactly rather than rounded.
inline constexpr std::size_t kTxnDecisionMaxRecords = 1024;

// ---- The participant's half: the transport ------------------------------
//
// `StatementShipServer`'s shape, and deliberately the same one: decode,
// bound, hand to a seam, answer whenever the seam says so. What it does not
// do is decide anything about transactions - the enrolment it prepares
// lives in `ShippedStatementExecutor`, which owns the context keyed on
// `(coordinator core, session id)`, and this class never looks it up.
//
// **Every path replies.** The coordinator is parked on one, and a request
// that produced no reply costs that phase a whole deadline before the
// coordinator can act. The one exception is a payload whose size is wrong,
// which names no waiter to answer.
class Txn2pcServer {
public:
    // What the seam calls when the phase is finished **and, for prepare,
    // durable**. Exactly once, from this core.
    using ReplyFn = std::function<void(const Status&)>;

    // The coordinator's identity, which is all a phase carries. `retry` is
    // R6-0's bit with R6-0's meaning on prepare and the narrower one
    // `TxnDecideRequestPayload` states on decide.
    struct PrepareAsk {
        std::uint32_t coordinator = 0;  // MessageHeader::src_core, part of the identity
        std::uint64_t session_id = 0;
        std::uint64_t transaction_id = 0;
        bool retry = false;
    };
    struct DecideAsk {
        std::uint32_t coordinator = 0;
        std::uint64_t session_id = 0;
        std::uint64_t transaction_id = 0;
        TxnDecision decision = TxnDecision::kUnset;
        bool retry = false;
    };
    using PrepareFn = std::function<void(PrepareAsk, ReplyFn)>;
    using DecideFn = std::function<void(DecideAsk, ReplyFn)>;

    // R6-5: what the coordinator answered an in-doubt ask with. `status` is
    // `OK` with a real `decision`, or the reason there is none -
    // `kTxnConflict` for "not decided yet, ask again", `kUnknownOutcome`
    // for "the record is gone, the next mount resolves this" - in the
    // coordinator's code with no message, which is all the wire carries.
    struct ResolveAnswer {
        std::uint32_t coordinator = 0;
        std::uint64_t session_id = 0;
        std::uint64_t transaction_id = 0;
        Status status;
        TxnDecision decision = TxnDecision::kUnset;
    };
    using ResolveFn = std::function<void(ResolveAnswer)>;

    // `resolve` is the third seam (R6-5): what this core does with an
    // answer to an ask it sent. Empty means this core never asks - the
    // fixture that drives prepare and decide by hand - and an answer that
    // arrives anyway is counted and dropped.
    Txn2pcServer(std::uint32_t core_id, sched::Scheduler& scheduler,
                 sched::RingTransport& transport, PrepareFn prepare, DecideFn decide,
                 ResolveFn resolve = {}, Logger* log = nullptr) noexcept
        : core_id_(core_id),
          scheduler_(scheduler),
          transport_(transport),
          prepare_(std::move(prepare)),
          decide_(std::move(decide)),
          resolve_(std::move(resolve)),
          log_(log) {}

    void OnPrepare(const sched::MessageHeader& header, std::span<const std::byte> payload);
    void OnDecide(const sched::MessageHeader& header, std::span<const std::byte> payload);

    // ---- R6-5: the in-doubt ask, from this core as a participant --------

    // Installs the receiver for the coordinator's answers. Captures `this`
    // with no unregister, so this server must outlive every pump of its
    // scheduler - `Txn2pcClient::RegisterReplyReceivers`'s rule.
    Status RegisterResolveReplyReceiver();
    // Asks `coordinator` what it decided for `(session_id, transaction_id)`.
    // R6-0's bit is set on every send: this leg has no first attempt. The
    // answer reaches the `resolve` seam whenever it arrives; nothing parks
    // on it, because the participant's context *is* the waiter and its
    // sweep re-asks on the ceiling.
    void Ask(std::uint32_t coordinator, std::uint64_t session_id, std::uint64_t transaction_id);
    void OnResolveReply(const sched::MessageHeader& header, std::span<const std::byte> payload);

    std::uint64_t prepares() const noexcept { return prepares_; }
    std::uint64_t decides() const noexcept { return decides_; }
    std::uint64_t replies() const noexcept { return replies_; }
    // **The ask leg deliberately has no counters here.** The three above
    // each have a reader (the coordinator fixture asserts them), which is
    // the test R6-3's review set for keeping one; a pair counting the asks
    // this core sends would fail it, because the same sends are already
    // counted where they are *read* -
    // `ShippedStatementExecutor::in_doubt_asks()`, which `SHOW META` prints
    // beside the population it is about. Two names for one number is what
    // the review that removed them called it.

private:
    void Reply(std::uint32_t coordinator, std::uint64_t request_id,
               sched::RingMessageKind kind, std::uint64_t session_id,
               std::uint64_t transaction_id, const Status& status);

    std::uint32_t core_id_;
    sched::Scheduler& scheduler_;
    sched::RingTransport& transport_;
    PrepareFn prepare_;
    DecideFn decide_;
    ResolveFn resolve_;
    Logger* log_;
    std::uint64_t prepares_ = 0;
    std::uint64_t decides_ = 0;
    std::uint64_t replies_ = 0;
    // An ask's request id, minted here since no statement owns one. Not a
    // waiter key - nothing parks on an ask - but the header needs one and a
    // captured header should say which ask an answer was to.
    std::uint64_t next_ask_id_ = 1;
};

// ---- The coordinator's half ---------------------------------------------

// Which leg a waiter is waiting on. Carried on the waiter rather than
// inferred from the map it is in, so that a **late reply from the other
// leg** - a prepare answer that arrives after the phase timed out and the
// decide phase has opened - is recognised as late instead of being
// delivered into this phase. The identity check cannot catch that one: both
// legs of one transaction carry the same session and transaction id.
enum class TxnPhase : std::uint8_t { kPrepare, kDecide };

// One participant's answer within a phase.
struct TxnParticipantOutcome {
    std::uint32_t core = 0;
    bool replied = false;
    Status status;  // meaningful once `replied`
};

// What one phase's waiter holds. `IndexBuildClient`'s shape widened from
// one respondent to N: the phase settles when every participant has replied
// or the deadline has passed, and **a phase that settles on the deadline is
// not a refusal** - what it means is the leg's own, above.
struct TxnPhaseOutcome {
    TxnPhase phase = TxnPhase::kPrepare;
    std::uint64_t session_id = 0;
    std::uint64_t transaction_id = 0;
    std::vector<TxnParticipantOutcome> participants;
    std::size_t outstanding = 0;
    sched::MonoTimeNs deadline_ns = 0;

    // Every participant replied and none refused. The coordinator's commit
    // branch tests exactly this, so it is written once here rather than at
    // each call site.
    bool AllPrepared() const noexcept {
        if (outstanding != 0) return false;
        for (const TxnParticipantOutcome& p : participants) {
            if (!p.replied || !p.status.ok()) return false;
        }
        return true;
    }
};

// The arrival core's side of the protocol: the waiters, the deadline, the
// sends and the reply receivers. `StatementShipClient`'s shape, one level
// up - a map for its stable addresses, a per-phase request id, and an
// identity check on every reply, because answering one transaction with
// another's result is the one failure this protocol must not have.
class Txn2pcClient {
public:
    Txn2pcClient(std::uint32_t core_id, sched::Scheduler& scheduler,
                 sched::RingTransport& transport, const sched::Clock& clock,
                 Logger* log = nullptr) noexcept
        : core_id_(core_id),
          scheduler_(scheduler),
          transport_(transport),
          clock_(clock),
          log_(log) {}

    // Installs both reply receivers, and (R6-5) the receiver for the one
    // *request* this half takes - a participant's in-doubt ask. The handlers
    // capture `this` and there is no unregister, so **the client must
    // outlive every pump of that scheduler** -
    // `StatementShipClient::RegisterReplyReceiver`'s rule.
    Status RegisterReplyReceivers();

    // Opens the waiter and sends prepare to every participant. **Every
    // refusal here happens before anything is sent**, so a caller that gets
    // one knows no participant was asked: an empty participant list, a core
    // this instance does not have, a duplicate participant, or a request id
    // that still has a phase parked on it.
    //
    // R6-5: also opens this transaction's **decision record** as undecided,
    // so an ask that lands between the first prepare and the decision is
    // answered "not yet" rather than "unknown" - the window in which a
    // participant would otherwise stop asking about a transaction whose
    // decide is seconds away.
    Status Prepare(std::uint64_t request_id, std::uint64_t session_id,
                   std::uint64_t transaction_id, std::span<const std::uint32_t> participants);

    // The decide leg, same discipline. `decision` may not be `kUnset` - a
    // decide that names no decision is the one thing a participant refuses
    // into doubt, so it is refused here first, where it costs nothing.
    //
    // R6-5: records the decision **before** the sends, whatever they do. The
    // caller has already made it durable (that is the order
    // `DispatchAsync` keeps), so from here an ask is answered with it even
    // if the decide messages themselves are refused by the ring.
    Status Decide(std::uint64_t request_id, std::uint64_t session_id,
                  std::uint64_t transaction_id, TxnDecision decision,
                  std::span<const std::uint32_t> participants);

    // The parked coordinator's predicate: every participant answered, the
    // deadline passed, or the waiter is gone. One clock read per turn.
    bool Settled(std::uint64_t request_id) const;
    const TxnPhaseOutcome* Find(std::uint64_t request_id) const;
    // Closes the phase. R6-5: a decide phase every participant acknowledged
    // takes its decision record with it - nobody is left to ask - and one
    // that was not keeps the record for `kTxnDecisionRetentionNs`.
    void Close(std::uint64_t request_id);

    // ---- R6-5: answering an in-doubt participant ------------------------

    // A participant's ask (`kTxnResolveRequest`). Answered from the record
    // and never by re-deciding: decided → the decision; opened but not
    // decided → `kTxnConflict`, ask again; no record → `kUnknownOutcome`.
    // An ask with R6-0's bit clear is refused `kInvalidArgument`, since a
    // sender that leaves it clear is the defect the bit exists to catch.
    void OnResolveAsk(const sched::MessageHeader& header, std::span<const std::byte> payload);

    std::size_t waiting() const noexcept { return waiting_.size(); }
    // Decision records held right now. Non-zero means a transaction whose
    // decide some participant has not acknowledged, or one still between
    // its prepare and its decision.
    std::size_t decisions_held() const noexcept { return decisions_.size(); }
    // Asks answered with a decision, with "not yet", and with
    // `UnknownOutcome`. The first is the population R6-5 exists for; the
    // third is a participant that stays in doubt until the next mount.
    std::uint64_t resolutions_answered() const noexcept { return resolutions_answered_; }
    std::uint64_t resolutions_undecided() const noexcept { return resolutions_undecided_; }
    std::uint64_t resolutions_unknown() const noexcept { return resolutions_unknown_; }
    // Asks refused outright: a clear retry bit, or a payload of the wrong
    // size. A protocol anomaly, never a workload property.
    std::uint64_t resolve_refusals() const noexcept { return resolve_refusals_; }
    // Records dropped by the retention, and by the memory bound before it.
    // Either turns a later ask into `UnknownOutcome`; the second means a
    // storm, and the first means a participant that never asked.
    std::uint64_t decisions_forgotten() const noexcept { return decisions_forgotten_; }
    std::uint64_t decisions_evicted() const noexcept { return decisions_evicted_; }

    // ---- What this core reports (D7's counters, one level up) -----------

    // Prepare requests that **left** this core - so `prepare_messages()` is
    // what §5's "a one-owner commit sends zero prepare messages" assertion
    // reads, and a refusal from `Prepare` sent none and is not counted.
    std::uint64_t prepare_messages() const noexcept { return prepare_messages_; }
    std::uint64_t decide_messages() const noexcept { return decide_messages_; }
    // Phases that ended with a participant unheard from. Non-zero means a
    // transaction was aborted (prepare) or a participant left in doubt
    // (decide) for the ring or a core, not for anything the data said.
    std::uint64_t phase_timeouts() const noexcept { return phase_timeouts_; }
    // Participants that answered a prepare with a refusal. The population
    // that turns a commit into an abort with a reason the client can read.
    std::uint64_t prepare_refusals() const noexcept { return prepare_refusals_; }
    // Decide acknowledgements that carried a refusal. **Worse than a
    // timeout**: the decision is durable and a participant says it did not
    // apply it, which is the one anomaly on this leg that is not a lost
    // message.
    std::uint64_t decide_refusals() const noexcept { return decide_refusals_; }
    // A reply whose identity is not its waiter's, or whose leg is not.
    std::uint64_t identity_mismatches() const noexcept { return identity_mismatches_; }
    // A reply that matched no waiter: its phase had already settled on the
    // deadline.
    std::uint64_t late_replies() const noexcept { return late_replies_; }

private:
    // Both legs' send path, which differ only in the kind and the payload.
    Status OpenPhase(std::uint64_t request_id, TxnPhase phase, std::uint64_t session_id,
                     std::uint64_t transaction_id, std::span<const std::uint32_t> participants);
    void OnReply(TxnPhase phase, const sched::MessageHeader& header,
                 std::span<const std::byte> payload);

    // R6-5: one transaction's decision, for a participant that asks. Keyed
    // by the coordinator's `(session_id, transaction_id)` - this core's own
    // ids, so no collision is possible - and opened undecided at the first
    // prepare. `decided_at_ns` is 0 until `Decide`; the retention runs from
    // it, or from `opened_at_ns` for a record nothing ever decided.
    struct DecisionRecord {
        TxnDecision decision = TxnDecision::kUnset;
        sched::MonoTimeNs opened_at_ns = 0;
        sched::MonoTimeNs decided_at_ns = 0;
    };
    using DecisionKey = std::pair<std::uint64_t, std::uint64_t>;

    // Opens or refreshes this transaction's record, expiring first. The one
    // way a record is created, so `Prepare` and `Decide` cannot come to
    // disagree about when the retention starts or whether the cap applies -
    // which they did in the first cut, where only `Prepare` expired.
    DecisionRecord& OpenDecisionRecord(std::uint64_t session_id, std::uint64_t transaction_id,
                                       sched::MonoTimeNs now);

    // Drops records past `kTxnDecisionRetentionNs`, then, while over
    // `kTxnDecisionMaxRecords`, drops the oldest. **One ordering, which is
    // time**: the first cut had the retention drop by age and the cap drop
    // by map key, so which record a storm discarded depended on a
    // `(session, transaction)` ordering that means nothing across sessions.
    // Two counters still, because the two mean different things - a
    // forgotten record is a participant that never asked, an evicted one is
    // a storm - but one rule decides which record goes.
    //
    // Lazy, at the two moments a record matters - an ask, and a new
    // record's insertion - rather than on a timer: a record nobody asks
    // about costs a map node, and a timer to reclaim that would cost more
    // than it reclaims.
    void ExpireDecisions(sched::MonoTimeNs now);

    std::uint32_t core_id_;
    sched::Scheduler& scheduler_;
    sched::RingTransport& transport_;
    const sched::Clock& clock_;
    Logger* log_;
    std::map<std::uint64_t, TxnPhaseOutcome> waiting_;
    std::map<DecisionKey, DecisionRecord> decisions_;
    std::uint64_t prepare_messages_ = 0;
    std::uint64_t decide_messages_ = 0;
    std::uint64_t phase_timeouts_ = 0;
    std::uint64_t prepare_refusals_ = 0;
    std::uint64_t decide_refusals_ = 0;
    std::uint64_t identity_mismatches_ = 0;
    std::uint64_t late_replies_ = 0;
    std::uint64_t resolutions_answered_ = 0;
    std::uint64_t resolutions_undecided_ = 0;
    std::uint64_t resolutions_unknown_ = 0;
    std::uint64_t resolve_refusals_ = 0;
    std::uint64_t decisions_forgotten_ = 0;
    std::uint64_t decisions_evicted_ = 0;
};

}  // namespace kds::server
