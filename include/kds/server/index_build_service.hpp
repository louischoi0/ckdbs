#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/core_affinity.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/manager.hpp"

// Building a peer-owned relation's index on the core that owns it
// (docs/workplan-peer-writer.md §7c, decided 2026-08-25; PW1c-6b-2 is the
// ring half).
//
// `CREATE INDEX` is core 0's statement - the catalog has one writer - but
// the tree it builds is pages, and a relation another core owns has its
// pages in that core's pool, stamped by that core's stream, holding rows
// core 0 cannot see: the owner's uncommitted writes, and every committed
// one core 0 never faulted. So the build moves, not the statement. Core 0
// prepares the definition (`exec::PrepareIndexDef`) and sends it here; the
// owner runs the page half `CREATE INDEX` always ran (`exec::BuildIndexTree`
// - root from its own lease, backfill from its own pool, the tree's images
// into its own stream), seeds its own anchor slot, and replies with the
// root. Core 0 writes the `sys.indexes` row with that root and no seed
// (`Catalog::AnchorSeed::kByOwner`), commits, and tells the owner `done`.
//
// ---- The refusal window --------------------------------------------------
//
// From the request's arrival until `done`, **writes to that relation on
// the owner are refused retryably** (`IndexBuildPending`,
// core_affinity.hpp). The owner's catalog shows no index until core 0's
// commit reaches it, so a row written in the window would be indexed by
// nobody - and an index missing a row is a wrong answer with a right
// answer's shape. `done` ends the window whichever way the statement went;
// on `committed` the owner drops its catalog cache in the same handler, so
// the index is visible to the first write the release admits whatever
// order the invalidation broadcast and `done` reached the ring in.
//
// ---- Two clocks, one invariant ---------------------------------------------
//
// Core 0 parks on the reply under `kIndexBuildReplyDeadlineNs` (PW1c-6b-3)
// and sends `done(aborted)` when it gives up; the owner keeps the window
// under `kIndexBuildPendingCeilingNs` when no `done` comes at all. The
// ceiling exceeds the deadline by construction, and the static_assert
// below is the whole argument: the owner must never release while core 0
// could still commit, because a row written after the release and before
// the commit is exactly the missing row above. A reply that arrives after
// core 0 gave up matches no waiter and is discarded; the tree it names is
// orphaned, like every unreachable page in this engine.
//
// ---- Under kNoTxnId, in the owner's stream --------------------------------
//
// The images and the anchor record carry no transaction. Core 0's DDL
// transaction lives in core 0's stream, and naming it here would mint a
// loser in this stream's analysis with nothing to roll back - the phantom
// a PAGE_HANDOFF with a transaction id is refused for. What a rollback
// costs is the tree, orphaned, and the anchor slot, which stays (PW2-3's
// named debt, one more occupant).

namespace kds::server {

// The wire forms. POD, under ring_message.hpp's exception to the on-disk
// rules: they never leave the process.

// core 0 -> owner: the definition, verbatim from `exec::PrepareIndexDef` -
// the widths it computed, the columns in declared order, the oid it
// issued. The name rides along so the owner's `CheckIndexDef` refuses by
// the rules core 0's did, one implementation.
struct IndexBuildRequestPayload {
    std::uint64_t table_oid;
    std::uint64_t index_oid;
    std::uint16_t key_width;
    std::uint16_t entry_width;
    std::uint8_t nkeys;
    std::uint8_t ncovered;
    std::uint8_t flags;
    std::uint8_t reserved0;
    std::uint16_t key_cols[catalog::kMaxIndexKeyColumns];
    std::uint16_t covered_cols[catalog::kMaxIndexCoveredColumns];
    char name[catalog::kCatalogNameMax];  // NUL-padded, as the catalog stores it
};
static_assert(sizeof(IndexBuildRequestPayload) == 112);

// owner -> core 0: the root, or why not. `status_code` is a StatusCode; 0
// is success and only then is `root_page_id` meaningful. The message is
// the owner's failure, truncated to the wire: the client on core 0 sees
// it, and the owner's log holds the whole.
inline constexpr std::size_t kIndexBuildReplyMessageBytes = 112;
struct IndexBuildReplyPayload {
    std::uint64_t index_oid;
    std::uint32_t root_page_id;
    std::uint32_t status_code;
    char message[kIndexBuildReplyMessageBytes];  // NUL-terminated
};
static_assert(sizeof(IndexBuildReplyPayload) == 128);

// core 0 -> owner: the statement's end. `committed` = 1 publishes the
// tree; 0 orphans it. Either closes the window.
struct IndexBuildDonePayload {
    std::uint64_t index_oid;
    std::uint8_t committed;
    std::uint8_t reserved0[7];
};
static_assert(sizeof(IndexBuildDonePayload) == 16);

inline constexpr sched::MonoTimeNs kIndexBuildReplyDeadlineNs = 60ull * 1'000'000'000ull;
inline constexpr sched::MonoTimeNs kIndexBuildPendingCeilingNs = 180ull * 1'000'000'000ull;
static_assert(kIndexBuildPendingCeilingNs > kIndexBuildReplyDeadlineNs,
              "the owner must outwait core 0: a window released before core 0 gives up "
              "admits rows the published index would miss");

// The encode core 0 sends - a definition `exec::PrepareIndexDef` produced,
// so its column counts are within the caps - and the owner's decode of a
// request `IndexBuildServer::OnRequest` has already bounded.
IndexBuildRequestPayload IndexBuildRequestOf(const catalog::Catalog::IndexDef& def);
catalog::Catalog::IndexDef IndexDefOf(const IndexBuildRequestPayload& request);

// ---- The owner's half ------------------------------------------------------

class IndexBuildServer {
public:
    // `send(dst_core, request_id, kind, payload)` never blocks; the real
    // one submits a send-retry task, as every cross-core sender does.
    using SendFn = std::function<void(std::uint32_t, std::uint64_t, sched::RingMessageKind,
                                      std::span<const std::byte>)>;
    // Hands the build to this core's reactor as a `system` task. Empty
    // runs it inline, for a caller with no reactor.
    using SubmitFn = std::function<void(std::unique_ptr<sched::Task>)>;
    // Runs on `done(committed)` and on an expiry - the runtime drops its
    // catalog cache here, so the published index is seen.
    using OnCommittedFn = std::function<void()>;

    IndexBuildServer(catalog::Catalog& catalog, storage::PageStore& store, wal::WalManager* wal,
                     std::uint32_t core_id, PendingIndexBuilds& pending,
                     const sched::Clock& clock, SendFn send, SubmitFn submit = {},
                     OnCommittedFn on_committed = {}, Logger* log = nullptr) noexcept
        : catalog_(catalog),
          store_(store),
          wal_(wal),
          core_id_(core_id),
          pending_(pending),
          clock_(clock),
          send_(std::move(send)),
          submit_(std::move(submit)),
          on_committed_(std::move(on_committed)),
          log_(log) {}

    // The kIndexBuildRequest handler: bound the counts, check the owner,
    // open the window, build (as a `system` task), seed, reply. Every
    // refusal is a reply - core 0 is parked on one.
    void OnRequest(const sched::MessageHeader& header, std::span<const std::byte> payload);
    // The kIndexBuildDone handler: closes the window; `committed` also
    // drops the catalog cache. A `done` naming no open window is ignored.
    void OnDone(const sched::MessageHeader& header, std::span<const std::byte> payload);
    // The tick: closes every window older than the ceiling, each logged as
    // the `done` that never came, and drops the cache in case it was a
    // commit whose `done` was lost.
    void Expire(sched::MonoTimeNs now);

    // Builds attempted. Diagnostics and tests.
    std::uint64_t builds() const noexcept { return builds_; }

private:
    void Build(std::uint32_t requester, std::uint64_t request_id,
               const IndexBuildRequestPayload& request);
    void Reply(std::uint32_t requester, std::uint64_t request_id, std::uint64_t index_oid,
               PageId root, const Status& status);

    catalog::Catalog& catalog_;
    storage::PageStore& store_;
    wal::WalManager* wal_;
    std::uint32_t core_id_;
    PendingIndexBuilds& pending_;
    const sched::Clock& clock_;
    SendFn send_;
    SubmitFn submit_;
    OnCommittedFn on_committed_;
    Logger* log_;
    std::uint64_t builds_ = 0;
};

// ---- Core 0's half ---------------------------------------------------------

// What a reply lands in, addressed by the statement's request id.
// `arrived` is the flag the statement parks on (PW1c-6b-3).
struct IndexBuildOutcome {
    bool arrived = false;
    Status status;
    PageId root_page_id = kInvalidPageId;
};

// The replies core 0 is waiting for. A map, for its stable addresses: a
// parked coroutine holds a pointer into it across the wait.
class IndexBuildWaiters {
public:
    IndexBuildOutcome& Open(std::uint64_t request_id);
    IndexBuildOutcome* Find(std::uint64_t request_id);
    void Close(std::uint64_t request_id);
    std::size_t size() const noexcept { return waiting_.size(); }

private:
    std::map<std::uint64_t, IndexBuildOutcome> waiting_;
};

// Installs core 0's reply receiver. A reply whose request id matches no
// waiter is discarded silently - crosscore.md §3's teardown rule; core 0
// gave up on it. `waiters` must outlive the scheduler.
Status RegisterIndexBuildReplyReceiver(sched::Scheduler& system_scheduler,
                                       IndexBuildWaiters& waiters, Logger* log = nullptr);

// Core 0's two sends, each a send-retry task in the `system` group.
void SendIndexBuildRequest(sched::Scheduler& scheduler, sched::RingTransport& transport,
                           std::uint32_t owner_core, std::uint64_t request_id,
                           const IndexBuildRequestPayload& request,
                           std::uint32_t system_core = 0);
void SendIndexBuildDone(sched::Scheduler& scheduler, sched::RingTransport& transport,
                        std::uint32_t owner_core, std::uint64_t index_oid, bool committed,
                        std::uint32_t system_core = 0);

}  // namespace kds::server
