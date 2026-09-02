#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/budget.hpp"
#include "kds/exec/fk_check.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/fk_intent.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/manager.hpp"

// The foreign key's forward check across owners — the sender the wire's two
// kinds were landed ahead of (`instructions/v2.8.0/workorder-ah.md` AH-T2,
// `docs/spec/foreign-keys.md` §2a).
//
// ---- What crosses, and why it is one round per owner ---------------------
//
// The child's core cannot descend a parent it does not own: those pages are
// another core's, and `exec::CheckParentPresent` has no ownership question
// anywhere in it. So the *asking* crosses. The extraction pass at the
// dispatch fork groups every parent pk the statement names by resolved
// owner (`exec::FkParentVerdicts::Defer`), and each foreign owner receives
// **one** request carrying its whole set — AH-R2. A statement's cross-owner
// cost is therefore a function of how many distinct owners its parents live
// on, never of its row count.
//
// ---- What the parent's owner leaves behind -------------------------------
//
// A pass is not just an answer, it is a promise: between the reply and the
// child's commit the parent's owner could delete the very row it vouched
// for. So a passing probe **grants a reference intent** (`fk_intent.hpp`) —
// and a parent-side DELETE meeting a live foreign intent answers busy
// rather than racing it. The intent is released by the transaction's
// **decide**, which every cross-owner transaction already sends, so there is
// no third message and no `done` leg (AH-R5).
//
// It is memory-resident, and under `cross-owner-txn.md` §1a that is not an
// omission: an intent-only participant writes no `TXN_PREPARE` record, so
// there is nothing for an intent to ride in. What makes it safe is an
// invariant rather than an argument — a participant that restarts after
// granting an intent and before its prepare leg forces the coordinator's
// transaction to fail, because the prepare cannot be answered by a process
// that has lost the enrolment. AH-T5 proves it by killing the participant
// in exactly that window; until it does, that is a claim this file states
// and does not demonstrate.

namespace kds::server {

// The most distinct parent rows one request carries.
//
// **Derived, not chosen.** A ring payload is `sched::kCoreRingPayloadBytes`
// (1024) and a parent costs an oid plus a pk, so the count is what is left
// after the request's fixed head divided by 16 - which is 62, and 62 is
// therefore what it is rather than a round number someone liked. The
// `static_assert` below is the real statement; this expression is how it
// stays true if either quantity moves.
//
// A first draft picked 64 flat and produced a 1048-byte payload the ring
// silently refuses - every cell of `fk_probe_service_test` failed on a
// reply that never came, which is what a cap chosen instead of derived
// looks like from the outside.
inline constexpr std::size_t kFkProbeRequestHeadBytes = 24;  // ids + count + reserved
inline constexpr std::size_t kFkProbeMaxParents =
    (sched::kCoreRingPayloadBytes - kFkProbeRequestHeadBytes) / (2 * sizeof(std::uint64_t));

// **A statement naming more distinct parents on one foreign owner is
// refused, not chunked**, and deliberately: chunking means several requests
// per owner and a waiter that aggregates them, which is protocol for a
// shape nobody has produced. The refusal is fail-closed and names this
// order. If AH-T6 measures a real statement past the cap, chunking is what
// the refusal converts into - and the cap is a measured item then rather
// than a derived ceiling now.

// child's core -> parent's owner. `count` entries of the two arrays are
// meaningful; the rest are unread. `session_id` and the header's
// `src_core` are the intent's holder, which is what a later decide
// releases by.
struct FkProbeRequestPayload {
    std::uint64_t session_id;
    std::uint64_t transaction_id;
    std::uint32_t count;
    std::uint32_t reserved0;
    std::uint64_t parent_oid[kFkProbeMaxParents];
    std::uint64_t parent_pk[kFkProbeMaxParents];
};
static_assert(sizeof(FkProbeRequestPayload) <= sched::kCoreRingPayloadBytes,
              "a probe request must fit one ring payload; the cap above is derived from that "
              "budget, so a head that grew without the cap shrinking would refuse every send "
              "at run time instead of failing here");

// ---- The reverse pair (AJ-T2, `docs/spec/foreign-keys.md` §3a) ------------
//
// One entry is one question: *"does any row of child relation `child_oid`
// reference `parent_pk` through column `child_column_no`"*. The forward's
// entry is a pair and this one is a triple, which is the whole reason
// AJ-R6 gives them separate kinds rather than a direction flag.
//
// **Derived like the forward's cap and by the same rule, and the arithmetic
// differs because the entry does.** `child_column_no` is a `std::uint16_t`
// — `SysFkeyRow::child_column_no`'s type and `CheckNoChildReferences`'
// parameter — so an entry is 8 + 8 + 2 = 18 bytes, not the 17 a byte-wide
// column would give. The order's own survey wrote 58 from that wrong width;
// the expression below is what makes the number follow the types instead of
// a paragraph, and the `static_assert` is the real statement.
inline constexpr std::size_t kFkReverseProbeRequestHeadBytes = 24;  // ids + count + reserved
inline constexpr std::size_t kFkReverseProbeMaxEntries =
    (sched::kCoreRingPayloadBytes - kFkReverseProbeRequestHeadBytes) /
    (2 * sizeof(std::uint64_t) + sizeof(std::uint16_t));

inline constexpr std::size_t kFkProbeReplyMessageBytes = 128;

// parent's owner -> child's core. `status_code` non-zero means the whole
// probe failed and no verdict is meaningful — the owner could not answer,
// which is different from answering "no such parent". Verdicts are
// **positional**: entry i answers the request's entry i, which is why the
// request's order is stable.
struct FkProbeReplyPayload {
    std::uint64_t session_id;
    std::uint32_t count;
    std::uint32_t status_code;
    // `exec::FkVerdict` per parent, one byte each.
    std::uint8_t verdict[kFkProbeMaxParents];
    char message[kFkProbeReplyMessageBytes];  // NUL-terminated
};
static_assert(sizeof(FkProbeReplyPayload) <= sched::kCoreRingPayloadBytes,
              "and so must a reply");

// parent's owner -> child's owner. The three arrays are parallel and
// `count` entries of each are meaningful, the forward request's shape for
// its reason: a reader of a captured frame sees the questions in the order
// the verdicts answer them.
struct FkReverseProbeRequestPayload {
    std::uint64_t session_id;
    std::uint64_t transaction_id;
    std::uint32_t count;
    std::uint32_t reserved0;
    std::uint64_t child_oid[kFkReverseProbeMaxEntries];
    std::uint64_t parent_pk[kFkReverseProbeMaxEntries];
    std::uint16_t child_column_no[kFkReverseProbeMaxEntries];
};
static_assert(sizeof(FkReverseProbeRequestPayload) <= sched::kCoreRingPayloadBytes,
              "a reverse probe request must fit one ring payload; the cap above is derived from "
              "that budget, so a head or an entry that grew without the cap shrinking would "
              "refuse every send at run time instead of failing here");
// **And the head the cap was derived from is the head the struct has.** The
// size assert above does not say this: a fifth head field added without
// moving `kFkReverseProbeRequestHeadBytes` shrinks the payload by one entry
// and still fits, so the derivation would quietly stop being one while every
// assert kept passing. This is the line that fails instead.
static_assert(offsetof(FkReverseProbeRequestPayload, child_oid) ==
                  kFkReverseProbeRequestHeadBytes,
              "the declared head bytes must be the offset of the first entry array");

// child's owner -> parent's owner. `status_code` non-zero means the whole
// probe failed and no verdict is meaningful — the owner could not answer,
// which is different from answering "no children". Verdicts are
// **positional**, entry i answering request entry i.
//
// `kPass` here reads "no children" and is what lets the DELETE proceed;
// `kViolation` is a committed visible child, terminal; `kBusy` is a child
// row a foreign transaction is writing, retryable (F3). The same three
// values the local reverse check produces, because it *is* the local
// reverse check — run on the core that can see the rows.
struct FkReverseProbeReplyPayload {
    std::uint64_t session_id;
    std::uint32_t count;
    std::uint32_t status_code;
    std::uint8_t verdict[kFkReverseProbeMaxEntries];
    char message[kFkProbeReplyMessageBytes];  // NUL-terminated
};
static_assert(sizeof(FkReverseProbeReplyPayload) <= sched::kCoreRingPayloadBytes,
              "and so must its reply");

// How long the child's core waits for one owner's answer before giving up.
// `[PROPOSED]`, and shorter than the index build's minute by two orders:
// this is an OLTP write's inline cost, not a DDL build's, and a statement
// that waited a minute for a constraint check has already failed the
// client. A timeout answers `TxnConflict` — retryable, because the owner
// being slow is not the statement being wrong.
//
// **Both directions share it**, because both are the same statement's
// inline wait: a DELETE parked on a reverse round has failed its client at
// the same point an INSERT parked on a forward one has.
inline constexpr sched::MonoTimeNs kFkProbeReplyDeadlineNs = 5ull * 1'000'000'000ull;

// ---- The parent owner's half ---------------------------------------------

class FkProbeServer {
public:
    FkProbeServer(catalog::Catalog& catalog, storage::PageStore& store, std::uint32_t core_id,
                  FkIntentTable& intents, FkPendingDeleteTable& pending_deletes,
                  sched::Scheduler& scheduler,
                  sched::RingTransport& transport, txn::TransactionManager* txn = nullptr,
                  Logger* log = nullptr, stats::CabinStore* cabins = nullptr) noexcept
        : catalog_(catalog),
          store_(store),
          core_id_(core_id),
          intents_(intents),
          pending_deletes_(pending_deletes),
          scheduler_(scheduler),
          transport_(transport),
          txn_(txn),
          log_(log),
          cabins_(cabins) {}

    // The `kFkProbeRequest` handler: bound the count, check that every
    // parent named is **this core's**, resolve each against latest state,
    // grant an intent per pass, reply. Every refusal is a reply — the
    // child's core is parked on one.
    void OnRequest(const sched::MessageHeader& header, std::span<const std::byte> payload);

    // The `kFkReverseProbeRequest` handler (AJ-T2): bound the count, check
    // that every **child** named is this core's, and run the reverse check
    // this core already runs for its own parents — `CheckNoChildReferences`
    // with `options.core_id = core_id_`, Cabin-first where the fk column
    // carries one.
    //
    // **It leaves nothing behind, which is the asymmetry with `OnRequest`
    // above.** A forward probe grants a reference intent; this answers a
    // read and forgets it. The core running it is enrolled in nothing, gets
    // no decide and needs no release leg (AJ-R5) — what holds the window
    // open lives on the *deleting* core, in `FkPendingDeleteTable`.
    // **Validates on the drain, walks on a task**, which is the one
    // structural difference from `OnRequest` above and the reason for it:
    // the forward's `CheckParentPresent` is a btree descent, but a reverse
    // check without a Cabin is a stoppable walk of the *whole* child
    // relation, and up to `kFkReverseProbeMaxEntries` of them ride one
    // message. Run inline that holds the reactor's message drain for the
    // length of a relation scan - delaying every other core's decides and
    // probes queued behind it - and is charged to no scheduling group at
    // all (`sched.md` §4), so the cost AJ-T5 measures would be a cost
    // `SHOW META` cannot locate. `IndexBuildServer::OnRequest` is the
    // engine's one precedent for relation-scale work arriving as a message
    // and it validates inline then submits; this follows it.
    void OnReverseRequest(const sched::MessageHeader& header,
                          std::span<const std::byte> payload);

    // Every intent a holder took, released. Wired to the 2PC decide, which
    // is the only thing that ends an intent (AH-R5): a decide is idempotent
    // and so is this.
    std::size_t ReleaseIntents(std::uint32_t coordinator_core, std::uint64_t session_id) {
        return intents_.Release(FkIntentHolder{coordinator_core, session_id});
    }

    std::uint64_t probes() const noexcept { return probes_; }
    std::uint64_t reverse_probes() const noexcept { return reverse_probes_; }

private:
    void Reply(std::uint32_t requester, std::uint64_t request_id, std::uint64_t session_id,
               const std::vector<exec::FkVerdict>& verdicts, const Status& status);
    void ReverseReply(std::uint32_t requester, std::uint64_t request_id, std::uint64_t session_id,
                      const std::vector<exec::FkVerdict>& verdicts, const Status& status);

    // The walk itself, off the message drain. **By value**: the span it
    // arrived in is the ring slot's, which is reused as soon as the drain
    // moves on, so a task holding a reference to it would read whatever
    // message landed next.
    void AnswerReverse(std::uint32_t requester, std::uint64_t request_id,
                       const FkReverseProbeRequestPayload& request);

    catalog::Catalog& catalog_;
    storage::PageStore& store_;
    std::uint32_t core_id_;
    FkIntentTable& intents_;
    // AJ-T1: what this core is about to delete, consulted **before** the
    // existence read below so a row on its way out cannot be vouched for.
    FkPendingDeleteTable& pending_deletes_;
    sched::Scheduler& scheduler_;
    sched::RingTransport& transport_;
    txn::TransactionManager* txn_;
    Logger* log_;
    // F6's half of the reverse check: the verified-empty entry set of a
    // Cabin on the child's fk column is an authoritative "no children",
    // which turns a stoppable walk into a lookup.
    //
    // **Null on every core but 0 as the engine ships**, and that is a
    // deliberate configuration rather than an oversight here:
    // `CoreRuntime` passes `/*cabins=*/nullptr` to a peer's dispatcher,
    // reasoning that a peer "returns identical rows without them; what it
    // loses is speed". The consequence for this handler is worth stating
    // where it bites - a reverse probe answered by a **peer** is a walk,
    // and only one answered by core 0 can be a Cabin lookup. H-AJ4's
    // Cabin-versus-walk arm is therefore measurable in one placement and
    // not the other, which AJ-T5 must arrange rather than assume.
    stats::CabinStore* cabins_;
    std::uint64_t probes_ = 0;
    std::uint64_t reverse_probes_ = 0;
};

// ---- The child core's half -----------------------------------------------

// What one owner's reply lands in, addressed by the statement's request id.
struct FkProbeOutcome {
    bool arrived = false;
    Status status;
    // Positional, matching the request this waiter was opened with.
    std::vector<exec::FkVerdict> verdicts;
    sched::MonoTimeNs deadline_ns = 0;
};

class FkProbeClient {
public:
    FkProbeClient(std::uint32_t core_id, sched::Scheduler& scheduler,
                  sched::RingTransport& transport, const sched::Clock& clock,
                  Logger* log = nullptr) noexcept
        : core_id_(core_id), scheduler_(scheduler), transport_(transport), clock_(clock),
          log_(log) {}

    // Installs the reply receiver. The handler captures `this` and there is
    // no unregister, so this must outlive every pump of that scheduler —
    // `IndexBuildClient`'s rule and for its reason.
    Status RegisterReplyReceiver();

    // Opens the waiter under the deadline, then sends one owner's group.
    // A group past `kFkProbeMaxParents` opens nothing and refuses.
    Status Request(std::uint32_t owner_core, std::uint64_t request_id, std::uint64_t session_id,
                   std::uint64_t transaction_id,
                   const exec::FkParentVerdicts::ForeignGroup& group);

    // The reverse direction's send (AJ-T2). Deliberately **the same waiter
    // map and the same deadline**: `FkProbeOutcome` is `arrived`, a status
    // and positional verdicts, none of which is forward-specific, so
    // `DispatchAsync`'s settle-collect-decide block parks over a mixed set
    // of request ids without knowing which direction each went. Request ids
    // come from one per-core counter, so the two directions cannot collide.
    //
    // A group past `kFkReverseProbeMaxEntries` opens nothing and refuses,
    // as the forward's does and for its reason: a request this core cannot
    // phrase is a statement it cannot run, not one it runs partially.
    Status RequestReverse(std::uint32_t owner_core, std::uint64_t request_id,
                          std::uint64_t session_id, std::uint64_t transaction_id,
                          const exec::FkReverseProbeGroup& group);

    // The parked statement's predicate: the reply arrived, the deadline
    // passed, or the waiter is gone.
    bool Settled(std::uint64_t request_id) const;
    const FkProbeOutcome* Find(std::uint64_t request_id) const;
    void Close(std::uint64_t request_id);

    std::size_t waiting() const noexcept { return waiting_.size(); }

    // **Rounds this core sent**, one per distinct foreign owner per
    // statement (AH-R2). Counted rather than inferred: a measurement that
    // reads a latency and *assumes* a probe crossed cannot tell a crossing
    // from a colocated statement that never left, which is the whole
    // subject of AI-T3. XD0's rule, on the leg AI added.
    std::uint64_t requests() const noexcept { return requests_; }

    // Reverse rounds this core sent, counted separately from `requests()`:
    // a statement's cost is one or the other, never both, and a single
    // figure would make a DELETE's crossing indistinguishable from an
    // INSERT's in `SHOW META`.
    //
    // **`SHOW META` does not carry it yet**, and saying so here is the
    // difference between a counter and a claim: the block at
    // `command_dispatcher.cpp`'s `fk_probes_sent` reads `requests()` alone,
    // so a reverse round is invisible on that surface until AJ-T3 lands the
    // sender and the field together. Nothing sends one before then, so the
    // number is 0 rather than wrong - but it is not yet the distinction the
    // paragraph above describes.
    std::uint64_t reverse_requests() const noexcept { return reverse_requests_; }

private:
    // **Both directions land the same way**, so they land in one place: the
    // waiter lookup, the status decode and the positional copy have nothing
    // direction-specific in them, and only the payload each arrives in
    // differs. Each receiver decodes its own POD and hands the pieces here.
    void Land(std::uint64_t request_id, std::uint32_t status_code, const char* message,
              const std::uint8_t* verdict, std::uint32_t count, std::uint32_t cap);

    std::uint32_t core_id_;
    sched::Scheduler& scheduler_;
    sched::RingTransport& transport_;
    const sched::Clock& clock_;
    Logger* log_;
    std::map<std::uint64_t, FkProbeOutcome> waiting_;
    std::uint64_t requests_ = 0;
    std::uint64_t reverse_requests_ = 0;
};

}  // namespace kds::server
