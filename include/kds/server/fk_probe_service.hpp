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

// How long the child's core waits for one owner's answer before giving up.
// `[PROPOSED]`, and shorter than the index build's minute by two orders:
// this is an OLTP write's inline cost, not a DDL build's, and a statement
// that waited a minute for a constraint check has already failed the
// client. A timeout answers `TxnConflict` — retryable, because the owner
// being slow is not the statement being wrong.
inline constexpr sched::MonoTimeNs kFkProbeReplyDeadlineNs = 5ull * 1'000'000'000ull;

// ---- The parent owner's half ---------------------------------------------

class FkProbeServer {
public:
    FkProbeServer(catalog::Catalog& catalog, storage::PageStore& store, std::uint32_t core_id,
                  FkIntentTable& intents, sched::Scheduler& scheduler,
                  sched::RingTransport& transport, txn::TransactionManager* txn = nullptr,
                  Logger* log = nullptr) noexcept
        : catalog_(catalog),
          store_(store),
          core_id_(core_id),
          intents_(intents),
          scheduler_(scheduler),
          transport_(transport),
          txn_(txn),
          log_(log) {}

    // The `kFkProbeRequest` handler: bound the count, check that every
    // parent named is **this core's**, resolve each against latest state,
    // grant an intent per pass, reply. Every refusal is a reply — the
    // child's core is parked on one.
    void OnRequest(const sched::MessageHeader& header, std::span<const std::byte> payload);

    // Every intent a holder took, released. Wired to the 2PC decide, which
    // is the only thing that ends an intent (AH-R5): a decide is idempotent
    // and so is this.
    std::size_t ReleaseIntents(std::uint32_t coordinator_core, std::uint64_t session_id) {
        return intents_.Release(FkIntentHolder{coordinator_core, session_id});
    }

    std::uint64_t probes() const noexcept { return probes_; }

private:
    void Reply(std::uint32_t requester, std::uint64_t request_id, std::uint64_t session_id,
               const std::vector<exec::FkVerdict>& verdicts, const Status& status);

    catalog::Catalog& catalog_;
    storage::PageStore& store_;
    std::uint32_t core_id_;
    FkIntentTable& intents_;
    sched::Scheduler& scheduler_;
    sched::RingTransport& transport_;
    txn::TransactionManager* txn_;
    Logger* log_;
    std::uint64_t probes_ = 0;
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

private:
    std::uint32_t core_id_;
    sched::Scheduler& scheduler_;
    sched::RingTransport& transport_;
    const sched::Clock& clock_;
    Logger* log_;
    std::map<std::uint64_t, FkProbeOutcome> waiting_;
    std::uint64_t requests_ = 0;
};

}  // namespace kds::server
