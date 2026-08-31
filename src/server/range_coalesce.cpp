#include "kds/server/range_coalesce.hpp"

#include <algorithm>
#include <map>

#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/log_page_image.hpp"
#include "kds/storage/visit.hpp"
#include "kds/wal/log_page_handoff.hpp"

namespace kds::server {

Status CollectRangePages(storage::PageStore& store, PageId head, std::uint64_t hi,
                         std::vector<PageId>& out) {
    // An invalid head appends nothing, and `PlanCoalesce` reads
    // `pages.back()` on the promise that a chain always has its head - so
    // this refusal is what makes that promise true rather than a read of an
    // empty vector. `OpenRangeRows` refuses a row naming no entry page, so
    // reaching here means a `sys.ranges` row that disagrees with it.
    if (head == kInvalidPageId) {
        return Status::Corruption(
            "coalesce: a range of this relation names no entry page; CC8 makes a range its own "
            "chain, so a row naming none describes a partition with nowhere to put a row");
    }
    PageId cur = head;
    for (std::uint32_t steps = 0; cur != kInvalidPageId; ++steps) {
        if (Status s = storage::CheckPageWalkBudget(steps, head, "coalesce chain census");
            !s.ok()) {
            return s;
        }
        auto page = store.GetForRead(cur);
        if (!page.ok()) {
            return page.status().WithContext("coalesce census reading chain page " +
                                             std::to_string(cur));
        }
        heap::PageView view(page.value().bytes());
        // The range bound. The head cannot trip it - a head carries
        // `min_key = lo` and the bound is that range's `hi` - so a first
        // page that does is a directory and a chain that disagree, which
        // is corruption rather than an empty range.
        if (view.min_key() >= hi) {
            if (cur == head) {
                return Status::Corruption(
                    "coalesce: range head page " + std::to_string(head) + " has min_key " +
                    std::to_string(view.min_key()) + " at or above the range's hi " +
                    std::to_string(hi));
            }
            break;
        }
        out.push_back(cur);
        cur = view.next_page_id();
    }
    return Status::OK();
}

StatusOr<CoalescePlan> PlanCoalesce(catalog::Catalog& catalog, storage::PageStore& store,
                                    catalog::Oid rel_oid) {
    auto access = catalog.InitTableAccess(rel_oid);
    if (!access.ok()) return access.status();

    // Both refusals are for a caller that reached here without asking,
    // and both are `InvalidArgument` rather than a decline: the merge
    // doing nothing quietly is how a wrong call gets to look like a right
    // one.
    if (access.value()->ranges.size() <= 1) {
        return Status::InvalidArgument(
            "relation oid " + std::to_string(rel_oid) + " has " +
            std::to_string(access.value()->ranges.size()) +
            " range(s) and nothing to coalesce; the caller should have asked before planning");
    }
    if (access.value()->clustered_type != catalog::ClusteredType::kHeap) {
        return Status::Corruption(
            "relation oid " + std::to_string(rel_oid) +
            " is split and is not a heap relation; D1 of workplan-range-directory.md declines "
            "every btree relation, so a split one cannot exist");
    }

    CoalescePlan plan;
    plan.rel_oid = rel_oid;
    plan.segments.reserve(access.value()->ranges.size());
    // Pages per core, which is the whole of AX-D3's input.
    std::map<std::uint32_t, std::uint64_t> pages_by_core;

    for (const catalog::RangeTarget& range : access.value()->ranges) {
        CoalesceSegment segment;
        segment.lo = range.lo;
        segment.hi = range.hi;
        segment.owner_core = range.owner_core;
        segment.entry_page = range.entry_page;
        if (Status s = CollectRangePages(store, range.entry_page, range.hi, segment.pages);
            !s.ok()) {
            return s;
        }
        // A chain always has its head, which `CollectRangePages` refuses
        // to leave out, so the tail is the last page it named.
        segment.tail = segment.pages.back();
        pages_by_core[segment.owner_core] += segment.pages.size();
        plan.pages_total += segment.pages.size();
        plan.segments.push_back(std::move(segment));
    }

    // AX-D3: the most pages, ties to the lowest `core_id`. The map is
    // ordered by key, so a scan that only replaces on a **strictly**
    // greater count meets the lowest core first and keeps it - the tie
    // rule falls out of the container rather than being a second
    // comparison that could disagree with it.
    std::uint64_t best = 0;
    bool chosen = false;
    for (const auto& [core, pages] : pages_by_core) {
        if (!chosen || pages > best) {
            plan.absorber = core;
            best = pages;
            chosen = true;
        }
    }
    plan.pages_to_move = plan.pages_total - best;
    return plan;
}

Status LinkSegments(storage::DevicePageStore& store, wal::WalManager* wal,
                    const CoalescePlan& plan, std::uint32_t core_id, Logger* log) {
    if (core_id != plan.absorber) {
        return Status::InvalidArgument(
            "coalesce: core " + std::to_string(core_id) + " is not relation oid " +
            std::to_string(plan.rel_oid) + "'s absorber (core " +
            std::to_string(plan.absorber) + "); the link is the absorber's write");
    }

    // ---- The stamp, PL §9 rule 6 ----------------------------------------
    //
    // **The stamp is checked here and not left to the write grant**, and
    // the reason is a hole the grant path has by construction. The
    // acquisition sequence (`CoreRuntime::AdmitWritePages`) skips its
    // restamp when `MayWrite(id)` is already true - sound where it was
    // written, since a page inside this core's lease is already this
    // core's - but **`MayWrite` is unconditionally true on core 0**
    // (`DevicePageStore::MayWrite`: no lease, no test), so core 0 absorbing
    // a peer's pages would take every one of them without restamping any.
    // And core 0 is the *ordinary* absorber: `placement = creating` (DA2)
    // puts the `lo = 0` range there on almost every relation.
    //
    // What that would cost is exactly what CC7's cell says the mover must
    // not do - keep the catalog and the stamps disagreeing. The pages would
    // still say the peer's stream, so after a restart the peer's
    // `TryClaimByStamp` takes write rights over pages `sys.tables` says are
    // core 0's, and the two are then writers of one page held apart by
    // nothing but statement dispatch. PL §9 rule 6 states the rule this
    // closes: no page leaves a stream without being restamped.
    //
    // Ordered before any link, so a crash between the two leaves pages
    // restamped and chains unlinked - which the intact directory still
    // describes correctly. The converse would leave a linked relation whose
    // pages the absorber does not durably own, and that is the one prefix
    // a re-run could not repair.
    std::vector<PageId> restamped;
    wal::Lsn acquired_max = wal::kNoLsn;
    for (const CoalesceSegment& segment : plan.segments) {
        for (PageId id : segment.pages) {
            auto page = store.GetForRead(id);
            if (!page.ok()) {
                return page.status().WithContext("coalesce acquiring page " + std::to_string(id) +
                                                 " of relation oid " +
                                                 std::to_string(plan.rel_oid));
            }
            // Already this stream's: the acquisition happened, here or on
            // the grant path. Skipped rather than repeated, because a
            // second acquisition record implies an erase that a repeat
            // cannot promise is sound (`AdmitWritePages`' own argument).
            if (storage::GetPageStreamStamp(page.value().bytes()) ==
                storage::StreamStampFor(core_id)) {
                continue;
            }
            if (!store.MayWrite(id)) {
                // `InvalidArgument` and not a retryable status,
                // deliberately: the grant is step 3 of a sequence this call
                // is step 4 of, so a missing one is a driver that ran them
                // out of order, and a retry of the same wrong order answers
                // the same way.
                return Status::InvalidArgument(
                    "coalesce: core " + std::to_string(core_id) + " may not write page " +
                    std::to_string(id) + " of relation oid " + std::to_string(plan.rel_oid) +
                    "; the write grant (docs/spec/crosscore.md §6c step 3) has not arrived");
            }
            auto acquired = wal::LogPageHandoff(wal, id, core_id);
            if (!acquired.ok()) {
                return acquired.status().WithContext("coalesce acquisition record for page " +
                                                     std::to_string(id));
            }
            // An unlogged store answers `kNoLsn` throughout, and there is
            // then nothing to stamp *at*: `page_lsn` must name a record
            // that exists, which is why `StampPageLsn` refuses zero.
            // `range_alloc.cpp` treats its handoff the same way.
            //
            // **Be exact about what this skips.** `StampPageLsn` writes the
            // LSN and the stream stamp together, so skipping it leaves the
            // departed core's stamp on the page - and the stamp is a
            // *runtime* rights input as well as a recovery one
            // (`TryClaimByStamp`), so on an unlogged multi-core instance
            // that core would re-claim write rights at its next fault. An
            // unlogged store is a fixture and never a served instance,
            // which is what makes this acceptable - not the recovery
            // argument on its own.
            if (acquired.value() == wal::kNoLsn) continue;
            if (Status s = store.StampPageLsn(id, acquired.value()); !s.ok()) {
                return s.WithContext("coalesce acquisition restamp of page " + std::to_string(id));
            }
            acquired_max = std::max(acquired_max, acquired.value());
            restamped.push_back(id);
        }
    }
    if (!restamped.empty()) {
        // **The record before the page**, and this is the one acquisition
        // site that has to take the wait itself. `FlushPages` syncs the
        // device and nothing in it waits on the WAL, so without this a
        // crash between the two leaves a page whose `page_lsn` names a
        // record that does not exist — the state `StampPageLsn`'s refusal
        // of zero and PL §9 rule 6 both exist to prevent. The grant path
        // (`CoreRuntime::AdmitWritePages`) omits it soundly because the
        // *giver* already waited on the departure record; here the
        // acquisition is this core's own and nobody else waits on it. One
        // `EnsureDurable` for the batch maximum — the gate is a watermark,
        // so a per-page wait is the same fsync asked for n times.
        if (acquired_max != wal::kNoLsn && wal != nullptr) {
            if (Status s = wal->EnsureDurable(acquired_max); !s.ok()) {
                return s.WithContext("coalesce acquisition records not durable");
            }
        }
        if (Status s = store.FlushPages(restamped); !s.ok()) {
            return s.WithContext("flushing coalesce acquisition restamps of relation oid " +
                                 std::to_string(plan.rel_oid));
        }
    }

    // ---- The links, in `lo` order ---------------------------------------
    //
    // Segment i's tail points at segment i+1's head. Written as a full
    // page image so redo restores the link with the page, and stamped by
    // `LogFullPageImage` - the half a hand-copied append loses.
    //
    // Under `kNoTxnId`: a link is an ownership-and-structure event, not
    // part of the DDL transaction's atom. AX-D5 puts the merge outside
    // that transaction precisely because it has no compensation, and
    // naming the transaction here would mint a loser in this stream with
    // nothing to roll back.
    std::vector<PageId> written;
    for (std::size_t i = 0; i + 1 < plan.segments.size(); ++i) {
        const PageId tail = plan.segments[i].tail;
        const PageId next_head = plan.segments[i + 1].entry_page;
        // **Read first, write only to write.** `Get` dirties the frame, so
        // testing the link through it would dirty every tail on a re-run
        // for bytes that do not change - which is the cost the skip below
        // exists to avoid, paid anyway.
        PageId current_link = kInvalidPageId;
        {
            auto probe = store.GetForRead(tail);
            if (!probe.ok()) {
                return probe.status().WithContext("coalesce reading tail page " +
                                                  std::to_string(tail));
            }
            current_link = heap::PageView(probe.value().bytes()).next_page_id();
        }
        // Already linked is already done - a re-run after a crash, which
        // §6c makes the repair.
        if (current_link == next_head) continue;
        if (current_link != kInvalidPageId) {
            return Status::Corruption(
                "coalesce: relation oid " + std::to_string(plan.rel_oid) + " range at lo " +
                std::to_string(plan.segments[i].lo) + " ends at page " + std::to_string(tail) +
                ", which already links to " + std::to_string(current_link) + " rather than "
                "to the next range's head " + std::to_string(next_head));
        }
        auto page = store.Get(tail);
        if (!page.ok()) {
            return page.status().WithContext("coalesce linking tail page " +
                                             std::to_string(tail));
        }
        heap::PageView(page.value().bytes()).set_next_page_id(next_head);
        if (Status s = storage::LogFullPageImage(wal, store, wal::kNoTxnId, tail); !s.ok()) {
            return s.WithContext("coalesce link image for page " + std::to_string(tail));
        }
        written.push_back(tail);
    }

    // Durable before the caller may contract the directory: the
    // contraction is what makes the concatenated chain the relation, and a
    // contraction over links the device does not hold is the one state
    // that loses rows.
    if (!written.empty()) {
        if (Status s = store.FlushPages(written); !s.ok()) {
            return s.WithContext("flushing coalesce links of relation oid " +
                                 std::to_string(plan.rel_oid));
        }
    }

    if (log != nullptr && log->enabled(LogLevel::kInfo)) {
        log->Info("range", "core " + std::to_string(core_id) + " absorbed relation oid " +
                               std::to_string(plan.rel_oid) + ": " +
                               std::to_string(plan.segments.size()) + " ranges, " +
                               std::to_string(plan.pages_total) + " pages, " +
                               std::to_string(written.size()) + " link(s) written");
    }
    return Status::OK();
}

}  // namespace kds::server
