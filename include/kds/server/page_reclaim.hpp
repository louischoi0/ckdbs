#pragma once

// **DROP TABLE page reclamation** (`instructions/v3.0.0/workorder-bf-drop-table-page-reclaim.md`).
//
// A dropped relation's tombstone carries the roots its pages hang from
// (`SysObjectRow::pending_roots`, BF-R2). This class turns those roots back
// into free ids, one bounded step at a time, on core 0's system tick
// (BF-Q16):
//
// 1. **The gate (BF-R4).** Nothing is read or freed until the durable redo
//    start `D` is at or past the job's gate: no later mount can then replay
//    a record that names one of the relation's pages. For a tombstone a
//    mount found pending the gate is the end of that mount's recovery scan;
//    every record that names the pages precedes the drop's commit, which
//    precedes that end. No WAL record is added.
// 2. **The walk (BF-R3).** From the anchor: the clustered tree by descent
//    plus its leaf chain, every tree an anchor slot names by descent plus
//    its leaf chain, a pre-SUS-1 heap chain by its links, and the var-heap
//    chain. Every page is checked for the class its parent says it is and
//    for its owner - the relation's oid, or the slot's index oid. A page
//    that fails is counted (`reclaim_skipped`) and neither freed nor
//    descended through; one that is already free is passed over. A page
//    reached twice by descent refuses the job.
// 3. **The frees (BF-R5, BF-R7).** Children before parents: every tree's
//    leaves, a map sync, then each internal level in turn with a sync after
//    it; each chain tail-first with a sync per page (BF-Q18 (c)); the
//    anchor last and a sync. At most `kReclaimBatchPages` frees a step; a
//    deferred free ends the step and is asked again at the next.
// 4. **The clear.** Only once every free is durable is the tombstone's word
//    overwritten to 0 (`Catalog::ClearPendingRoots`), so a crash before it
//    leaves the job pending and the next mount drives it again. The walk's
//    checks make that re-drive idempotent: a freed page is passed over, and
//    one another relation has reused fails the owner check.
//
// **Which jobs exist.** Two kinds. The tombstones a mount finds pending
// (BF-R8): no statement of that run can reach their pages - the name
// resolves NotFound, every memo and cache is built after the mount, and a
// trail entry for the dead oid is dropped at `TrailReplay::Build`
// (BF-S1's Census B). And a drop committed during the run (BF-R9), queued
// at its commit and walked only once every core's statement epoch has
// reached the schema word the commit moved to (`statement_epoch.hpp`). A
// crash loses that queue; the next mount finds the tombstone pending.
//
// **Threading.** Stepped from one thread, core 0's reactor. The inbox
// `EnqueueCommittedDrop` writes is the one structure any core touches, under
// its own latch; the counters are atomic because `SHOW META` reads them
// from any core. The store calls it
// makes are the store's own concurrency (`device_page_store.hpp`), and the
// clear is an ordinary catalog write under the page latch.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/server/statement_epoch.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/wal/record.hpp"

namespace kds::server {

// Frees one step may make (BF-R8, BF-Q13): a constant, not a key, because
// it only paces a background task. A step's cost is up to 64 frame-table
// holds plus its map syncs: one per tree level it finishes, but one per
// page of a chain (BF-Q18 (c)), so a chain step can be 64 fsyncs.
inline constexpr std::size_t kReclaimBatchPages = 64;

// BF-R11's reclaim half, read by `SHOW META` on every core.
struct ReclaimCounters {
    std::atomic<std::uint64_t> pending{0};   // tombstones still owed
    std::atomic<std::uint64_t> deferred{0};  // frees a held frame deferred
    std::atomic<std::uint64_t> skipped{0};   // pages BF-R3's check refused
    std::atomic<std::uint64_t> refused{0};   // jobs refused, left pending

    void Reset() noexcept {
        pending = 0;
        deferred = 0;
        skipped = 0;
        refused = 0;
    }
};

class PageReclaimer {
public:
    PageReclaimer(catalog::Catalog& catalog, storage::DevicePageStore& store,
                  ReclaimCounters& counters, Logger* log) noexcept
        : catalog_(catalog), store_(store), counters_(counters), log_(log) {}

    PageReclaimer(const PageReclaimer&) = delete;
    PageReclaimer& operator=(const PageReclaimer&) = delete;

    // Queues every tombstone the catalog holds pending, gated on `D`
    // reaching `gate_lsn` - the end of the mount's recovery scan.
    Status CollectAtMount(wal::Lsn gate_lsn);

    // **A drop committed during the run** (BF-R9), queued by its commit arm
    // from whichever core committed it: gated on `D` past the commit and on
    // every statement epoch reaching `word`, the schema word after the
    // commit's move. Thread-safe: the inbox is the one structure a peer
    // touches, and `Step` drains it on core 0. A crash loses the inbox; the
    // next mount finds the tombstone pending and drives it (BF-R7).
    void EnqueueCommittedDrop(catalog::Oid oid, catalog::PendingRoots roots,
                              wal::Lsn commit_lsn, std::uint64_t word);

    // One bounded step against the durable redo start `durable_redo_start`.
    // Returns OK when it did nothing or made progress; a job that cannot
    // proceed for a reason a later step cannot change is refused - counted,
    // logged, left pending for the next mount - and never fails the step.
    Status Step(wal::Lsn durable_redo_start, const StatementEpochs* epochs = nullptr);

    // Jobs this run still holds, refused ones included.
    std::size_t jobs() const noexcept { return jobs_.size(); }

    // **The walk alone, freeing nothing** (BF-R12): every page `roots`
    // reach for relation `oid`, by the same descent, links and checks a
    // reclaim makes, with the count of pages the check refused - a link to a
    // page that is free or never written among them. The sim's
    // census ledgers a drop with it, and asks it of every live relation:
    // a live relation's walk skips nothing and shares no page with another.
    // Not gated, and not counted in `ReclaimCounters`.
    struct Reach {
        std::vector<PageId> pages;
        std::uint64_t skipped = 0;
    };
    StatusOr<Reach> ReachFrom(catalog::Oid oid, catalog::PendingRoots roots);

    // **The crash-cut seam**: runs at every point a crash can cut a reclaim
    // - after each free, after each map sync, and on each side of the
    // tombstone's clear - naming which. A cell takes a crash image there.
    // Set and cleared while no step runs; empty in production.
    void SetCutForTest(std::function<void(const char*)> hook) { cut_for_test_ = std::move(hook); }

private:
    // What the parent says a page is: the class to check, and the owner.
    enum class Expect : std::uint8_t { kAnchor, kBtree, kIndex, kHeapChain, kVarHeapChain };

    struct Visit {
        PageId page = kInvalidPageId;
        Expect expect = Expect::kBtree;
        std::uint64_t owner = 0;
        // A tree page: the level its parent routes to (0 is a leaf); a
        // root's is unknown until read. A chain page: its chain's index.
        std::uint16_t level = 0;
        std::uint32_t chain = 0;
        // Reached by a leaf's sibling link rather than by descent. Such a
        // page is often one a descent already reached, which is no cycle;
        // a page a *descent* reaches twice is.
        bool by_link = false;
    };
    static constexpr std::uint16_t kAnyLevel = 0xFFFF;

    // One free of the plan, and whether a map sync follows it.
    struct Free {
        PageId page = kInvalidPageId;
        bool sync_after = false;
    };

    enum class Phase : std::uint8_t { kWalk, kFree, kClear, kRefused };

    struct Job {
        catalog::Oid oid = 0;
        catalog::PendingRoots roots;
        wal::Lsn gate = 0;
        // The schema word every running statement must have revalidated at
        // (BF-R9); 0 for a tombstone a mount found, which no statement of
        // its run can reach (BF-R8).
        std::uint64_t word = 0;
        Phase phase = Phase::kWalk;
        // The walk's frontier, every page it took, and what it found: tree
        // pages by level (every tree's level-L pages together), and each
        // chain in link order. A leaf's sibling link, kept until the
        // descents are done, finds a leaf no descent reaches.
        std::vector<Visit> frontier;
        std::vector<Visit> leaf_links;
        std::unordered_set<PageId> seen;
        std::vector<std::vector<PageId>> levels;
        std::vector<std::vector<PageId>> chains;
        bool anchor_live = false;
        // Pages BF-R3's check refused, this job's own count.
        std::uint64_t skipped = 0;
        // A walk for `ReachFrom`: nothing here reaches the counters.
        bool census = false;
        std::string refusal;
        // The last visit met a refused fault (BE-R4's `ResourceExhausted`):
        // put back, and the step ends.
        bool stalled = false;
        // The plan, and how far it has gone.
        std::vector<Free> plan;
        std::size_t next = 0;
    };

    // Reads up to `budget` pages of the walk; false when the job refused.
    bool WalkSome(Job& job, std::size_t& budget);
    // Reads one page and queues what it names; false when the job refused.
    bool VisitOne(Job& job, const Visit& visit);
    // Takes a page the check passed: into the walk's record, false on a
    // descent that reaches it twice.
    bool Take(Job& job, const Visit& visit, PageId page);
    void BuildPlan(Job& job);
    // Frees up to `budget` pages; false when the job refused.
    bool FreeSome(Job& job, std::size_t& budget);
    void Refuse(Job& job, const std::string& why);
    void Skip(Job& job, PageId page, const std::string& why);

    catalog::Catalog& catalog_;
    storage::DevicePageStore& store_;
    ReclaimCounters& counters_;
    Logger* log_;
    std::deque<Job> jobs_;
    // Committed drops from every core, drained into `jobs_` by `Step`.
    std::mutex inbox_latch_;
    std::vector<Job> inbox_;
    std::function<void(const char*)> cut_for_test_;
    void Cut(const char* what) {
        if (cut_for_test_) cut_for_test_(what);
    }
};

}  // namespace kds::server
