#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/assertion_check.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/sched/task.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/wal/manager.hpp"

// Building a peer-owned relation's assertion on the core that owns it
// (`docs/inflight/in-progress/workplan-peer-writer.md` §7d, PW1c-6c; the
// finding it closes is `bench/v2.2.0/results-shipping-part-a-v2.2.0-11-g925f483.md`
// Finding 2).
//
// ---- Why the build moves, where a refresh would not have done -------------
//
// An assertion's Bound Cabin is not a structure that is written once and
// read afterwards: **every write to the constrained relation appends to
// it** (`exec::AssertionEnforcer::ReserveInsert`). So the cabin's pages are
// written by whichever core writes the relation, and that is the relation's
// owner, always and only. A cabin core 0 allocated from core 0's lease is a
// cabin the owner may not write - `MayWrite` refuses a page carrying
// neither this lease, a grant, nor this stream's stamp - which is why the
// owner *could not* enforce such an assertion however faithfully its
// registry were refreshed. Teaching the peer's registry about core 0's
// cabin would have produced a refused write in place of an unenforced one.
//
// The fix is ownership, and it is `CREATE INDEX`'s (PW1c-6b, §7c): core 0
// keeps the catalog half - the checks, the id, the `sys.assertions` row -
// and the **owner** runs the page half. The owner allocates the chain from
// its own extent lease, so the pages are own-stamped by its own stream (PL-C)
// and **no handoff record is needed**: nothing crosses a stream, because the
// core that creates the pages is the core that will write them forever
// after. PW3's peer checkpoint carries the cabin's group snapshot (AS6a) in
// that same stream, so the owner's next mount folds its own base.
//
// ---- No refusal window, and why this differs from the index build --------
//
// The index build refuses writes on the owner from the request's arrival
// until core 0's `done`, because between the tree's last page and the
// published row a write would be indexed by nobody. **This protocol opens
// no window**, because the owner adopts the directory at the *end of its
// own build task* rather than at `done`:
//
//   - the build is one synchronous `system` task, so no statement of this
//     core runs between the scan's last row and the adoption, and there is
//     no interval in which a write could go uncounted;
//   - a write admitted after the adoption and before core 0's publish is
//     counted by a cabin whose row is on its way, which is right if the
//     publish commits and reserves into an orphan chain if it does not;
//   - and a window would have been the *worse* answer for this structure:
//     an index missing a row answers wrongly, while a cabin missing a row
//     under-counts its group **forever**, since nothing rebuilds it.
//
// What the owner does at `done` is therefore small and one-directional:
// `aborted` evicts the directory it adopted, `committed` drops the catalog
// cache so the published row is visible to `SHOW ASSERTIONS` here. A `done`
// that never arrives leaves the owner **enforcing** - over-enforcing if
// core 0 in fact aborted, which a remount clears, and which is the
// fail-closed side of the only choice available (`HandleAssertion` makes
// the same call locally when its durability wait fails).
//
// ---- Under kNoTxnId, in the owner's stream --------------------------------
//
// The entry pages, the `ASSERT_BUILD` records and the `ASSERT_SNAPSHOT`
// base carry no transaction, for `index_build_service.hpp`'s reason: core
// 0's DDL transaction lives in core 0's stream, and naming it here would
// mint a loser in this stream's analysis with nothing to roll back. A
// core-0 rollback costs the chain, orphaned.

namespace kds::server {

// The wire forms. POD, under ring_message.hpp's exception to the on-disk
// rules: they never leave the process.

// Bytes of declaration text one request carries, derived from the ring slot
// rather than chosen - `statement_ship_service.hpp`'s derivation, and the
// static_assert below keeps the two honest.
inline constexpr std::size_t kAssertionBuildFixedBytes = 24;
inline constexpr std::size_t kAssertionBuildTextMax =
    sched::kCoreRingPayloadBytes - kAssertionBuildFixedBytes;

// core 0 -> owner: the declaration verbatim, and the id core 0 issued for
// it.
//
// **The text, not a parsed form.** §8.2 keeps `source_text` as the canon
// exactly so that the parsed shape can be rebuilt from it, and the owner
// rebuilds it the same way `exec::ReviveAssertion` does at a mount - one
// implementation of "what does this declaration mean", never a second
// encoding of it on a wire. It also leaves the `GROUP BY` list uncapped in
// the catalog, which §3 requires: what is capped here is bytes.
//
// A declaration longer than the slot is **refused by name**, never
// truncated - a truncated declaration is a different declaration. That
// bound (1000 bytes) is tighter than the catalog's (one var-heap value),
// so a very long declaration is creatable on a core-0-owned relation and
// refused on a peer-owned one; the divergence is named here rather than
// discovered, and raising it means raising the ring's payload, which is
// `docs/spec/crosscore.md` §9's open sizing decision.
struct AssertionBuildRequestPayload {
    std::uint64_t table_oid;
    std::uint64_t assertion_id;
    std::uint16_t text_len;
    std::uint8_t reserved0[6];
    char text[kAssertionBuildTextMax];  // not NUL-terminated; text_len bounds it
};
static_assert(sizeof(AssertionBuildRequestPayload) == sched::kCoreRingPayloadBytes,
              "the request fills exactly one ring slot; kAssertionBuildFixedBytes is what the "
              "fields above the text cost and must match them");

// owner -> core 0: the root the publish names and the two numbers the reply
// reports, or why not. `status_code` is a StatusCode; 0 is success and only
// then is anything else meaningful.
inline constexpr std::size_t kAssertionBuildReplyMessageBytes = 112;
struct AssertionBuildReplyPayload {
    std::uint64_t assertion_id;
    std::uint64_t rows_incorporated;
    std::uint32_t cabin_root;
    std::uint32_t group_count;
    std::uint32_t status_code;
    std::uint32_t reserved0;
    char message[kAssertionBuildReplyMessageBytes];  // NUL-terminated
};
static_assert(sizeof(AssertionBuildReplyPayload) == 144);

// core 0 -> owner: the statement's end. `committed` = 1 keeps the directory
// the owner adopted and drops its catalog cache; 0 evicts it and orphans
// the chain.
struct AssertionBuildDonePayload {
    std::uint64_t assertion_id;
    std::uint8_t committed;
    std::uint8_t reserved0[7];
};
static_assert(sizeof(AssertionBuildDonePayload) == 16);

// Core 0's park (PW1c-6b-3's deadline, the same 60 s). There is no ceiling
// beside it: with no window to release, the owner owes nothing to a `done`
// that never comes.
inline constexpr sched::MonoTimeNs kAssertionBuildReplyDeadlineNs = 60ull * 1'000'000'000ull;

// The encode core 0 sends. **Refuses** rather than truncates, per the
// request's header note.
StatusOr<AssertionBuildRequestPayload> AssertionBuildRequestOf(catalog::Oid table_oid,
                                                               std::uint64_t assertion_id,
                                                               std::string_view source_text);

// Every message of this protocol travels through `sched::SubmitSendPod` on
// the `system` group, with `session_core` the constant 0 on every leg -
// core 0 owns the statement in both directions and nothing here reads the
// field. `request_id` is 0 for `done`, which belongs to no waiter.

// ---- The owner's half ------------------------------------------------------

class AssertionBuildServer {
public:
    // Runs on `done(committed)`: the runtime drops its catalog cache here,
    // so the published `sys.assertions` row is seen by this core's
    // `SHOW ASSERTIONS`. The one seam that reaches back into its owner.
    using OnCommittedFn = std::function<void()>;

    // `enforcer` is this core's live registry - the build adopts into it,
    // which is the whole point of building here. `txn` may be null (the
    // pre-MVCC configuration), and the build then reads everything, exactly
    // as `HandleAssertion` does on a dispatcher without a manager.
    AssertionBuildServer(catalog::Catalog& catalog, storage::PageStore& store,
                         wal::WalManager* wal, txn::TransactionManager* txn,
                         exec::AssertionEnforcer& enforcer, std::uint32_t core_id,
                         sched::Scheduler& scheduler, sched::RingTransport& transport,
                         OnCommittedFn on_committed = {}, Logger* log = nullptr) noexcept
        : catalog_(catalog),
          store_(store),
          wal_(wal),
          txn_(txn),
          enforcer_(enforcer),
          core_id_(core_id),
          scheduler_(scheduler),
          transport_(transport),
          on_committed_(std::move(on_committed)),
          log_(log) {}

    // The kAssertionBuildRequest handler: bound the text, check the owner,
    // build (as a `system` task), adopt, reply. Every refusal is a reply -
    // core 0 is parked on one.
    void OnRequest(const sched::MessageHeader& header, std::span<const std::byte> payload);
    // The kAssertionBuildDone handler: `aborted` evicts what the build
    // adopted, `committed` drops the catalog cache.
    void OnDone(const sched::MessageHeader& header, std::span<const std::byte> payload);

    // Builds attempted. Diagnostics and tests.
    std::uint64_t builds() const noexcept { return builds_; }

private:
    void Build(std::uint32_t requester, std::uint64_t request_id,
               const AssertionBuildRequestPayload& request);
    void Reply(std::uint32_t requester, std::uint64_t request_id, std::uint64_t assertion_id,
               PageId root, std::uint64_t rows, std::uint32_t groups, const Status& status);

    catalog::Catalog& catalog_;
    storage::PageStore& store_;
    wal::WalManager* wal_;
    txn::TransactionManager* txn_;
    exec::AssertionEnforcer& enforcer_;
    std::uint32_t core_id_;
    sched::Scheduler& scheduler_;
    sched::RingTransport& transport_;
    OnCommittedFn on_committed_;
    Logger* log_;
    std::uint64_t builds_ = 0;
};

// ---- Core 0's half ---------------------------------------------------------

// What a reply lands in, addressed by the statement's request id.
struct AssertionBuildOutcome {
    bool arrived = false;
    Status status;
    PageId cabin_root = kInvalidPageId;
    std::uint64_t rows_incorporated = 0;
    std::uint32_t group_count = 0;
    sched::MonoTimeNs deadline_ns = 0;
};

// Core 0's side: the waiters, the deadline, the two sends and the reply
// receiver. The dispatcher's foreign `CREATE ASSERTION` arm is its one
// client - phase 1 calls `Request`, the parked statement re-tests `Settled`
// once per reactor turn, phase 2 reads `Find`, then `Close`s and says
// `Done`. A map, for its stable addresses.
//
// Deliberately **not** shared with `IndexBuildClient`, which has the same
// waiter shape: the two protocols agree on three of ten lines (the deadline
// test, `Find`, `Close`) and disagree on every payload, every send and
// every no-waiter stance. A common base would have carried the agreement
// and re-stated the rest.
class AssertionBuildClient {
public:
    AssertionBuildClient(sched::Scheduler& scheduler, sched::RingTransport& transport,
                         const sched::Clock& clock, Logger* log = nullptr) noexcept
        : scheduler_(scheduler), transport_(transport), clock_(clock), log_(log) {}

    // Installs the reply receiver on the scheduler this was built over. The
    // handler captures `this` and there is no unregister, so the client must
    // outlive every pump of that scheduler.
    //
    // A reply matching no waiter is core 0 having given up: a successful one
    // is answered with `done(aborted)`, so the directory the owner adopted
    // is evicted rather than left enforcing a constraint no row names.
    Status RegisterReplyReceiver();

    // Phase 1's send: opens the waiter under the deadline, then sends. A
    // declaration the wire refuses opens nothing.
    Status Request(std::uint32_t owner_core, std::uint64_t request_id, catalog::Oid table_oid,
                   std::uint64_t assertion_id, std::string_view source_text);
    // The parked statement's predicate: the reply arrived, the deadline
    // passed, or the waiter is gone.
    bool Settled(std::uint64_t request_id) const;
    // The outcome, or null once closed. `arrived` false after `Settled`
    // answered true is the deadline.
    const AssertionBuildOutcome* Find(std::uint64_t request_id) const;
    void Close(std::uint64_t request_id);
    // Phase 2's end, either way.
    void Done(std::uint32_t owner_core, std::uint64_t assertion_id, bool committed);

    std::size_t waiting() const noexcept { return waiting_.size(); }

private:
    sched::Scheduler& scheduler_;
    sched::RingTransport& transport_;
    const sched::Clock& clock_;
    Logger* log_;
    std::map<std::uint64_t, AssertionBuildOutcome> waiting_;
};

}  // namespace kds::server
