#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/role.hpp"

// **Statement shipping, the wire and the waiter** (SS1 of the
// statement-shipping work order): a single-statement transaction that
// arrives on a core which does not own its target relation is carried to
// the owner, executed there, and answered back through the arrival core -
// where today it is refused (`docs/spec/crosscore.md` §6, and the 80-92%
// refusal rate `bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md`
// §9b measured).
//
// This header is the **transport half only**. What crosses the ring, who
// parks, how long, and how an answer is matched to its waiter. The dispatch
// fork that decides to ship (SS2) and the owner-side execution (SS3) are
// separate rows; the seam between them is `StatementShipServer`'s
// `ExecuteFn`, which this file defines and does not fill.
//
// ---- What crosses: the statement's text, not a plan --------------------
//
// D2 asks for "the bound statement". This engine has no serialisable bound
// form - step chains, slot tables and expression trees are in-process
// pointers - so what actually travels is the statement **text**, and the
// owner parses and binds it against its own catalog and its own pages.
//
// That is not a concession, it is the same argument PW1c-6b made when it
// moved `CREATE INDEX`'s page half to the owner rather than shipping a
// plan: a plan bound on the arrival core is bound against *that* core's
// view, and a peer's view of a relation it does not own is a device image
// that can be behind. `index_build_service.hpp` states it as "Backfill
// here would read the device's stale image and miss every row the owner
// holds". Binding on the owner is the only place the answer is authoritative.
//
// The cost is one extra parse. Against the 21-23 microseconds a statement
// costs and the ~0.9 ms a commit's sync costs (pretasks §4), it is not
// where this design's budget goes.
//
// ---- The ring bounds the statement, and refuses rather than truncates ---
//
// Both PODs fill exactly one ring slot, so the longest shippable statement
// is `kShippedStatementTextMax` bytes and a longer one is **refused by
// name**: a truncated statement is a different statement.
//
// The same bound applies to the reply, which carries the statement's whole
// answer because that answer is what the client gets. A write's reply is
// tens of bytes; a `SELECT`'s can be any size, so a read whose reply
// overflows is refused by name too. That is a real bound on D1's read half
// and it is stated here rather than discovered later. Raising either means
// raising the ring's payload, which is `docs/spec/crosscore.md` §9's open
// sizing decision and not this row's to take.
//
// ---- The waiter, and the two ways it ends ------------------------------
//
// The arrival core opens a waiter under a deadline and parks the statement
// on it (the dispatcher's `co_await sched::WaitUntil`, the same park
// `IndexBuildClient` uses). It ends two ways and they are **not** the same
// answer:
//
//   - a **reply** arrives. Its status crosses as a code and a message and
//     is rebuilt with `Status::FromWire`, so the owner's refusal reaches
//     the client as the owner spelled it - the PW6-(2) retryable bit
//     included, which is the one bit clients build retry loops on.
//
//   - the **deadline** passes. This is *not* a refusal, and treating it as
//     one would be the design's worst available mistake: the statement may
//     have executed on the owner and had its reply lost, so answering
//     anything retryable invites a double-insert against an engine-issued
//     pk. D4 requires the arrival core to say **unknown outcome** and to
//     say it in a code no retry loop follows. `Status::UnknownOutcome` is
//     that code, added for this and non-retryable by construction
//     (`IsRetryable` is one code wide, by `docs/spec/protocol.md` §11).
//
// **A statement parked when the reactor is destroyed never replies**, and
// that is correct rather than a leak: `~CoroTask` destroys a suspended
// frame without invoking its completion, so nothing calls back into an
// executor or a server that teardown is dismantling, and the outcome of a
// statement interrupted mid-flight genuinely is unknown - which is what the
// arrival core's deadline says. Written here because a reader would
// otherwise have to derive it from `sched/coro.hpp`.
//
// A reply that matches no waiter is the deadline having already fired.
// There is nothing to do with it but count it: unlike 6b's tree, a
// committed DML statement cannot be un-done by telling the owner to
// abandon it. It is counted rather than dropped silently because that
// count is the population D4's dedup record (SS3) exists to answer for.
//
// ---- Two rules SS2 inherits, written here because breaking either
// ---- undoes D4 from outside this file
//
// **1. After `Ship` returns OK, the only legal refusal is
// `UnknownOutcome`.** The dispatcher's no-reactor arm answers the foreign
// `CREATE INDEX` with `TxnConflict("… needs the reactor path …")`, which
// is right there because nothing was sent. Copying that shape *after* a
// shipped statement has left the core would hand `retryable=1` to a
// statement the owner may have committed, and a client's retry loop would
// then insert the row twice. The no-reactor refusal must be taken
// **before** `Ship`, never after.
//
// **2. Both handlers are registered on every core, or the mount fails.**
// An owner with no `kShippedStatementRequest` handler drops the request
// with a Debug line; an arrival core that never called
// `RegisterReplyReceiver` drops the reply the same way. Either costs a
// full deadline and a false `UnknownOutcome` **per statement**, and from
// the arrival core the two are indistinguishable from a slow owner. There
// is no runtime check that can tell them apart, so the wiring is where it
// has to be caught.
//
// ---- (session, sequence): the dedup identity ---------------------------
//
// Every shipped statement carries `(session_id, sequence)` - stable across
// a retry of the *same* statement by the same session, distinct for a new
// one. It is not the ring's `request_id`, which is a per-core counter that
// matches a reply to a waiter and means nothing to the owner. The owner
// uses the pair to answer a duplicate with the recorded outcome instead of
// re-executing it (D4). SS3 keeps that record; SS1 carries the identity
// and pins that it survives the round trip unchanged.

namespace kds::server {

// ---- The wire forms ---------------------------------------------------
//
// POD, under ring_message.hpp's exception to the on-disk rules: they never
// leave the process, so no explicit shift/mask encoding is owed.

// Bytes of statement text one request carries. Derived from the ring slot
// rather than chosen, so a slot resize moves it and the static_assert below
// keeps the two honest.
inline constexpr std::size_t kShippedStatementFixedBytes = 32;
inline constexpr std::size_t kShippedStatementTextMax =
    sched::kCoreRingPayloadBytes - kShippedStatementFixedBytes;

// arrival core -> owner: the statement, and the identity that makes a
// duplicate recognisable.
//
// `target_oid` is **the relation the arrival core routed on, and nothing
// more**: which oid's owner this request was addressed to.
//
// **Retracted 2026-08-26** (the SS3 review): this paragraph used to say the
// owner cross-checks it against the relation the text resolves to and
// refuses if the two disagree. Nothing did that, and on inspection nothing
// should. The owner parses and binds under its own catalog, which is the
// only authoritative resolution there is (the argument above) - so a
// disagreement means the arrival core's catalog was behind, and the two
// outcomes are already right: if the owner's resolution is a relation it
// owns, running it is the correct answer to what the client asked *by
// name*; if it is not, `CheckWriteAffinity` refuses, and the hop limit
// (session.hpp) keeps that refusal from becoming a second ship. A check
// here could only turn a correct answer into a refusal. It is carried
// because a log line that names the oid the routing decision was made on is
// what makes a mis-route legible afterwards.
struct ShippedStatementRequestPayload {
    std::uint64_t session_id;
    std::uint64_t sequence;
    std::uint64_t target_oid;
    std::uint16_t text_len;
    // The arrival core's **authenticated rank** (role.hpp), carried rather
    // than assumed (SS3). A `Session` holds `kAdmin` by default - the
    // auth-off contract - so an owner that minted its own session would run
    // every shipped statement as admin, and the authorization the arrival
    // core performed would be the only one there is. Carrying the rank
    // makes the owner ask `RequiredRole` the same question the arrival core
    // asked, of the same answer, which is what keeps a wire that crosses an
    // authorization boundary from widening it. A byte outside the enum is
    // **refused**, never defaulted: fail-closed is the only reading of an
    // unreadable rank.
    std::uint8_t role;
    std::uint8_t reserved0[5];
    char text[kShippedStatementTextMax];  // not NUL-terminated; text_len bounds it
};
static_assert(sizeof(ShippedStatementRequestPayload) == sched::kCoreRingPayloadBytes,
              "the request fills exactly one ring slot; kShippedStatementFixedBytes is what "
              "the header costs and must match the fields above it");

inline constexpr std::size_t kShippedStatementReplyFixedBytes = 32;
inline constexpr std::size_t kShippedStatementReplyTextMax =
    sched::kCoreRingPayloadBytes - kShippedStatementReplyFixedBytes;

// owner -> arrival core: what the statement answered, or why not.
//
// `status_code` is a StatusCode; 0 is success and `text` is then the
// reply line the client receives verbatim. Non-zero and `text` is the
// refusal's message, which `Status::FromWire` rebuilds so the client sees
// the owner's own words and the owner's own retryable bit.
//
// The identity rides back so a reply can be recognised without the ring's
// request_id - which is what lets a late reply be counted rather than
// mistaken for another statement's.
struct ShippedStatementReplyPayload {
    std::uint64_t session_id;
    std::uint64_t sequence;
    std::uint32_t status_code;
    std::uint16_t text_len;
    std::uint8_t reserved0[10];
    char text[kShippedStatementReplyTextMax];  // not NUL-terminated
};
static_assert(sizeof(ShippedStatementReplyPayload) == sched::kCoreRingPayloadBytes,
              "the reply fills exactly one ring slot; kShippedStatementReplyFixedBytes is "
              "what the header costs and must match the fields above it");

// How long the arrival core parks before it says "unknown outcome".
//
// A shipped statement costs the owner one statement (21-23 us) plus its
// share of a group commit's sync (~0.9 ms), and PW6 measured a committing
// session's p50 near 2 ms with the trx-id refill's longest wait at 3.0-3.3
// ms after PW7. Ten seconds is three orders of magnitude above that: it is
// not a latency budget, it is the point past which the reply is presumed
// lost rather than slow. It is deliberately generous because the answer on
// the other side of it is `UnknownOutcome`, which costs the client a
// statement it cannot safely retry - so a deadline that fires early is
// strictly worse than one that fires late.
inline constexpr sched::MonoTimeNs kShippedStatementDeadlineNs = 10ull * 1'000'000'000ull;

// The encode. **Refuses rather than truncates** - both because a shortened
// statement is a different statement, and because the bound is the ring's
// and a caller cannot be expected to know it.
StatusOr<ShippedStatementRequestPayload> ShippedStatementRequestOf(std::uint64_t session_id,
                                                                   std::uint64_t sequence,
                                                                   std::uint64_t target_oid,
                                                                   Role role,
                                                                   std::string_view text);
// The reply encode, same discipline: a reply too long to carry is turned
// into a refusal that says so, never a truncated answer presented as one.
StatusOr<ShippedStatementReplyPayload> ShippedStatementReplyOf(std::uint64_t session_id,
                                                               std::uint64_t sequence,
                                                               const Status& status,
                                                               std::string_view text);

// The statement text a request carries, bounded by `text_len` against the
// array rather than trusted: these are bytes this core did not compute.
StatusOr<std::string_view> ShippedStatementTextOf(const ShippedStatementRequestPayload& request);

// The rank a request carries, refused rather than defaulted when the byte
// names no role: a rank this core cannot read is not a rank it may assume.
StatusOr<Role> ShippedStatementRoleOf(const ShippedStatementRequestPayload& request);

// Both legs travel through `sched::SubmitSendPod` on the `system` group,
// with `session_core` echoed from the requester so a reader of a captured
// header can see which core's client is parked. There is no wrapper here:
// a per-service `Send*Message` is what made the header fill a thing every
// service copied.

// ---- The owner's half -------------------------------------------------

class StatementShipServer {
public:
    // What the executor calls when the statement is finished **and
    // durable**. Exactly once, from this core, with the reply line on a
    // success or the refusal on anything else.
    using ReplyFn = std::function<void(const Status&, std::string_view)>;

    // The seam SS3 fills: run `text` under this core's ordinary local
    // implicit transaction, and answer through `reply`.
    //
    // **Asynchronous, and that is the whole performance thesis** (D3). A
    // shipped statement must join the *owner's group commit* - the drain
    // that `core_runtime.cpp` installs as a post-task hook so that one
    // `fdatasync` covers every statement staged since the last one, which
    // the pretasks measured at 79x. Joining it means staging the commit
    // and **parking** on `IsDurable(lsn)`, the way `DispatchAsync` already
    // does. A seam that returned its answer would have to be finished
    // before it returned, which reaches only the inline
    // `DrainOnce()` + `EnsureDurable()` path - one blocking sync per
    // shipped statement on the owner's reactor, which is precisely the
    // cost shipping exists to remove.
    //
    // The statement is handed over **by value**: the ring payload it came
    // from dies with `OnRequest`, and an executor that parks outlives it.
    // Everything the owner needs to run it as the arrival core's client is
    // in one struct, so that this class stays the transport - it looks
    // nothing up and decides nothing about execution.
    //
    // `(requester, session_id, sequence)` is D4's identity, for the dedup
    // record `ShippedStatementExecutor` keeps. **The requester is part of
    // it**, and that is not decoration: a session id is minted per core, so
    // two cores mint the same one, and a record keyed on the id alone would
    // answer one core's statement with another core's outcome - the same
    // failure the reply path's identity check exists to prevent, one level
    // down.
    //
    // A server built with no executor refuses every request by name: the
    // wire working and nothing executing on it must not look alike (SS1's
    // rule, kept because a mis-wired core would otherwise time out per
    // statement instead of saying what is wrong).
    struct ShippedStatement {
        std::uint32_t requester = 0;
        std::uint64_t session_id = 0;
        std::uint64_t sequence = 0;
        std::uint64_t target_oid = 0;
        // Fail-closed default: a statement whose rank was never set runs at
        // the lowest one, not at the highest.
        Role role = Role::kReadOnly;
        std::string text;
    };
    using ExecuteFn = std::function<void(ShippedStatement statement, ReplyFn reply)>;

    StatementShipServer(std::uint32_t core_id, sched::Scheduler& scheduler,
                        sched::RingTransport& transport, ExecuteFn execute,
                        Logger* log = nullptr) noexcept
        : core_id_(core_id),
          scheduler_(scheduler),
          transport_(transport),
          execute_(std::move(execute)),
          log_(log) {}

    // The kShippedStatementRequest handler: bound the payload, hand the
    // statement to the seam, and answer whenever the seam says so.
    // **Every path replies** - the arrival core is parked on one, and a
    // request that produced no reply would cost that statement the whole
    // deadline before answering `UnknownOutcome`. The one exception is a
    // payload whose size is wrong, which names no waiter to answer.
    void OnRequest(const sched::MessageHeader& header, std::span<const std::byte> payload);

    std::uint64_t requests() const noexcept { return requests_; }
    std::uint64_t replies() const noexcept { return replies_; }

private:
    // By value, not by reference into the request payload: the executor may
    // park, and the payload dies with `OnRequest`.
    void Reply(std::uint32_t requester, std::uint64_t request_id, std::uint64_t session_id,
               std::uint64_t sequence, const Status& status, std::string_view text);

    std::uint32_t core_id_;
    sched::Scheduler& scheduler_;
    sched::RingTransport& transport_;
    ExecuteFn execute_;
    Logger* log_;
    std::uint64_t requests_ = 0;
    std::uint64_t replies_ = 0;
};

// ---- The arrival core's half ------------------------------------------

// What a reply lands in. `arrived` is what the parked statement tests;
// `deadline_ns` is when it stops testing.
struct ShippedStatementOutcome {
    bool arrived = false;
    Status status;
    // The reply line, on the success arm only. A refusal's message is in
    // `status`, where `Status::FromWire` put it.
    std::string text;
    // D4's identity, kept so an arriving reply can be checked against the
    // waiter the ring matched it to rather than trusted. The one failure
    // this protocol must not have is answering one statement with
    // another's result.
    std::uint64_t session_id = 0;
    std::uint64_t sequence = 0;
    sched::MonoTimeNs deadline_ns = 0;
    // When the statement left, so the wait can be measured rather than
    // inferred from the deadline (D7's `shipped_wait_us_max`).
    sched::MonoTimeNs sent_ns = 0;
};

// The arrival core's side: the waiters, the deadline, the send and the
// reply receiver. A map for its stable addresses - the receiver writes
// into an entry while other entries come and go, and a vector would move
// it. `IndexBuildClient`'s shape, minus the `done` leg: an autocommit
// statement opens no window on the owner, so there is nothing to close.
class StatementShipClient {
public:
    StatementShipClient(std::uint32_t core_id, sched::Scheduler& scheduler,
                        sched::RingTransport& transport, const sched::Clock& clock,
                        Logger* log = nullptr) noexcept
        : core_id_(core_id),
          scheduler_(scheduler),
          transport_(transport),
          clock_(clock),
          log_(log) {}

    // Installs the reply receiver. The handler captures `this` and there is
    // no unregister, so **the client must outlive every pump of that
    // scheduler**.
    Status RegisterReplyReceiver();

    // Opens the waiter under the deadline, then sends. **Every refusal here
    // happens before anything is sent**, so a caller that gets one knows
    // the statement did not run: the wire's own refusals (an empty or
    // over-long statement), an `owner_core` this instance does not have,
    // and a `request_id` that still has a statement parked on it - which
    // would otherwise replace that statement's waiter and let this one's
    // reply wake it.
    Status Ship(std::uint32_t owner_core, std::uint64_t request_id, std::uint64_t session_id,
                std::uint64_t sequence, std::uint64_t target_oid, Role role,
                std::string_view text);

    // The parked statement's predicate: the reply arrived, the deadline
    // passed, or the waiter is gone. One clock read per reactor turn.
    bool Settled(std::uint64_t request_id) const;
    // The outcome, or null once closed. `arrived` false after `Settled`
    // answered true is the deadline - which is `UnknownOutcome`, never a
    // refusal.
    const ShippedStatementOutcome* Find(std::uint64_t request_id) const;
    void Close(std::uint64_t request_id);

    std::size_t waiting() const noexcept { return waiting_.size(); }

    // ---- What D7 asks this core to report ------------------------------
    //
    // The arrival core's half: what it sent, what came back, and how long
    // the longest one took. `shipped()` counts statements that **left** -
    // a refusal from `Ship` sent nothing and is not one of them, which is
    // what keeps `shipped() - replies()` readable as "still in flight or
    // lost" rather than as a mix of that and statements that never went.
    std::uint64_t shipped() const noexcept { return shipped_; }
    // Answers delivered to the waiter that asked for them. A late reply is
    // not one (it has no waiter left); it is in `late_*_replies()` above.
    std::uint64_t replies() const noexcept { return replies_; }
    // Of those, the ones that carried a refusal. The owner's own refusals
    // and the wire's, together: from here they are one population - the
    // statements shipping did not turn into work.
    std::uint64_t refusals() const noexcept { return refusals_; }
    // The longest a delivered answer kept its statement parked. The
    // population this measures is the one SS-B4 prices - a waiter is a
    // parked coroutine, and this says how long the worst one held one.
    sched::MonoTimeNs wait_ns_max() const noexcept { return wait_ns_max_; }

    // **The two halves of "a reply that matched no waiter", kept apart
    // because they mean opposite things.**
    //
    // A late *result*: the deadline fired, the client was told the outcome
    // is unknown, and the owner had in fact executed the statement. This
    // number is the size of the problem D4's dedup record exists for, which
    // is why it is a counter and not a log line.
    std::uint64_t late_executed_replies() const noexcept { return late_executed_replies_; }
    // A late *refusal*: nothing ran, so nothing is owed. It measures only
    // that the deadline is tight, and folding it into the number above
    // would overstate the one that matters.
    std::uint64_t late_refused_replies() const noexcept { return late_refused_replies_; }

    // A reply whose (session, sequence) is not its waiter's. Strictly worse
    // than either above - it is the shape of answering one statement with
    // another's result - and a counter is the only cheap evidence that it
    // stays 0.
    std::uint64_t identity_mismatches() const noexcept { return identity_mismatches_; }

private:
    std::uint32_t core_id_;
    sched::Scheduler& scheduler_;
    sched::RingTransport& transport_;
    const sched::Clock& clock_;
    Logger* log_;
    std::map<std::uint64_t, ShippedStatementOutcome> waiting_;
    std::uint64_t late_executed_replies_ = 0;
    std::uint64_t late_refused_replies_ = 0;
    std::uint64_t identity_mismatches_ = 0;
    std::uint64_t shipped_ = 0;
    std::uint64_t replies_ = 0;
    std::uint64_t refusals_ = 0;
    sched::MonoTimeNs wait_ns_max_ = 0;
};

}  // namespace kds::server
