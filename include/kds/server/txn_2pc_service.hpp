#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "kds/base/status.hpp"
#include "kds/sched/ring_transport.hpp"

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

// The ring copies these byte for byte, which is only defined for a
// trivially copyable type. `sched::SubmitSendPod` asserts the same thing of
// whatever it is handed (send_retry.hpp), so these are the *earlier* check:
// R6-1 has no sender, and without them a payload that stopped being
// trivially copyable would go unnoticed until R6-3 first sent one.
static_assert(std::is_trivially_copyable_v<TxnPrepareRequestPayload>);
static_assert(std::is_trivially_copyable_v<TxnDecideRequestPayload>);
static_assert(std::is_trivially_copyable_v<TxnParticipantReplyPayload>);

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
// **What this answer covers, stated so it is not read wider than it is**:
// the prepare and decide legs, which are R6-1's scope. D5 needs a *third*
// exchange - an in-doubt participant asking the coordinator, with R6-0's bit
// set, answerable `UnknownOutcome` - and that kind does not exist yet. It
// carries ids and a bit, so it will fit; but it is R6-5's to declare and to
// size, and no assertion here has looked at it.
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

// ---- The decodes --------------------------------------------------------
//
// Both bound bytes this core did not compute. There are no matching
// *encodes* in R6-1: nothing sends these yet, and an encoder owes a
// UTF-8-safe truncation of the refusal message (`statement_ship_service.cpp`
// has one, in an anonymous namespace) which is better hoisted once R6-3 is
// its second caller than duplicated now for no sender.

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

}  // namespace kds::server
