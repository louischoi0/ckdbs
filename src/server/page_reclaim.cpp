#include "kds/server/page_reclaim.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <utility>

#include "kds/storage/anchor_page.hpp"
#include "kds/storage/btree/btree_page.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/varheap.hpp"

namespace kds::server {

namespace {

constexpr std::uint8_t TypeByte(PageType type) noexcept { return static_cast<std::uint8_t>(type); }

}  // namespace

Status PageReclaimer::CollectAtMount(wal::Lsn gate_lsn) {
    auto pending = catalog_.PendingReclaims();
    if (!pending.ok()) return pending.status();
    for (const catalog::Catalog::PendingReclaim& owed : pending.value()) {
        jobs_.push_back(MakeJob(owed.oid, /*index_oid=*/0, owed.roots, gate_lsn, /*word=*/0));
    }
    // BY-R1: the trees live relations owe - dropped indexes, rolled-back
    // creates - read here, after recovery's undo, unfiltered. A dropped
    // relation's slots are not listed: its tombstone's job above owns them.
    auto trees = catalog_.OwedIndexTrees();
    if (!trees.ok()) return trees.status();
    for (const catalog::Catalog::OwedIndexTree& tree : trees.value()) {
        jobs_.push_back(MakeJob(tree.rel_oid, tree.index_oid,
                                catalog::PendingRoots{tree.anchor_page_id, kInvalidPageId},
                                gate_lsn, /*word=*/0));
    }
    counters_.pending.store(jobs_.size(), std::memory_order_relaxed);
    if (!jobs_.empty() && log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("reclaim", std::to_string(jobs_.size()) +
                                  " dropped relation(s) or index tree(s) owe pages; reclaimed "
                                  "once the redo start passes lsn " + std::to_string(gate_lsn));
    }
    return Status::OK();
}

Status PageReclaimer::Clear(const Job& job) {
    if (job.index_oid == 0) return catalog_.ClearPendingRoots(job.oid);
    // An index job's clear is its slot's removal (BY-R5); a slot already gone
    // is a re-drive finding the clear done. An anchor that is no longer the
    // relation's stays a refusal: it frees nothing, and telling a reused
    // anchor from a damaged one is not this call's to guess.
    Status removed = catalog_.RemoveIndexSlot(job.oid, job.roots.anchor, job.index_oid);
    return removed.code() == StatusCode::kNotFound ? Status::OK() : removed;
}

std::string PageReclaimer::JobName(const Job& job) {
    if (job.index_oid == 0) return "dropped relation oid=" + std::to_string(job.oid);
    return "orphaned index oid=" + std::to_string(job.index_oid) + " of relation oid=" +
           std::to_string(job.oid);
}

PageReclaimer::Job PageReclaimer::MakeJob(catalog::Oid oid, catalog::Oid index_oid,
                                          catalog::PendingRoots roots, wal::Lsn gate,
                                          std::uint64_t word) {
    Job job;
    job.oid = oid;
    job.index_oid = index_oid;
    job.roots = roots;
    job.gate = gate;
    job.word = word;
    job.frontier.push_back(Visit{roots.anchor, Expect::kAnchor, oid, kAnyLevel});
    return job;
}

void PageReclaimer::Enqueue(Job job) {
    // Counted under the latch, before the push: a step on core 0 that
    // drains and finishes the job then decrements a count that already
    // holds it, so `pending` never wraps below zero.
    const std::lock_guard<std::mutex> hold(inbox_latch_);
    counters_.pending.fetch_add(1, std::memory_order_relaxed);
    inbox_.push_back(std::move(job));
}

void PageReclaimer::EnqueueCommittedDrop(catalog::Oid oid, catalog::PendingRoots roots,
                                         wal::Lsn commit_lsn, std::uint64_t word) {
    // Every record that names the relation's pages precedes the commit, so
    // a redo start past the commit replays none of them (BF-R4).
    Enqueue(MakeJob(oid, /*index_oid=*/0, roots, commit_lsn + 1, word));
}

void PageReclaimer::EnqueueOrphanedIndex(catalog::Oid rel_oid, PageId anchor,
                                         catalog::Oid index_oid, wal::Lsn gate,
                                         std::uint64_t word) {
    Enqueue(MakeJob(rel_oid, index_oid, catalog::PendingRoots{anchor, kInvalidPageId}, gate,
                    word));
}

void PageReclaimer::DropIndexJobsOfRelationJobs() {
    // BY-R3: one job owns a tree. A relation's job walks every slot, so any
    // index job for the same relation is dropped before it walks: a page that
    // job already freed is `NotFound` or another owner's to the relation's
    // walk, and both are skipped, never freed twice. Same thread as every
    // step, so no plan of the dropped job runs after this.
    std::unordered_set<catalog::Oid> relations;
    for (const Job& job : jobs_) {
        if (job.index_oid == 0) relations.insert(job.oid);
    }
    if (relations.empty()) return;
    const auto before = jobs_.size();
    std::erase_if(jobs_, [&](const Job& job) {
        return job.index_oid != 0 && relations.contains(job.oid);
    });
    counters_.pending.fetch_sub(before - jobs_.size(), std::memory_order_relaxed);
}

Status PageReclaimer::Step(wal::Lsn durable_redo_start, const StatementEpochs* epochs) {
    {
        const std::lock_guard<std::mutex> hold(inbox_latch_);
        for (Job& job : inbox_) jobs_.push_back(std::move(job));
        inbox_.clear();
    }
    DropIndexJobsOfRelationJobs();
    std::size_t budget = kReclaimBatchPages;
    for (auto it = jobs_.begin(); it != jobs_.end() && budget > 0;) {
        Job& job = *it;
        // The gate (BF-R4): no page of the relation is read or freed while a
        // later mount could still replay a record that names it.
        if (job.phase == Phase::kRefused || durable_redo_start < job.gate) {
            ++it;
            continue;
        }
        // The statement epoch (BF-R9): no statement still running may hold
        // a memo from before the drop's commit, checked before the walk
        // reads a page.
        if (job.word != 0) {
            if (epochs == nullptr || !epochs->AllAtLeast(job.word)) {
                ++it;
                continue;
            }
            // Passed once is passed for good: a statement that starts later
            // revalidated past the drop, so its memo cannot hold the relation.
            job.word = 0;
        }
        if (job.phase == Phase::kWalk) {
            if (!WalkSome(job, budget)) {
                ++it;
                continue;
            }
            if (!job.frontier.empty()) return Status::OK();  // the budget ran out
            BuildPlan(job);
            job.phase = Phase::kFree;
        }
        if (job.phase == Phase::kFree) {
            if (!FreeSome(job, budget)) {
                ++it;
                continue;
            }
            if (job.next < job.plan.size()) return Status::OK();  // budget, or a deferral
            job.phase = Phase::kClear;
        }
        // Every free is durable: the last plan step synced the map. Only now
        // may the tombstone stop owing (BF-R7).
        Cut("before the clear");
        if (Status cleared = Clear(job); !cleared.ok()) {
            Refuse(job, "its owing record could not be cleared: " + cleared.message());
            ++it;
            continue;
        }
        Cut("after the clear");
        if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
            log_->Info("reclaim", JobName(job) + " reclaimed: " +
                                      std::to_string(job.plan.size()) + " page(s) freed");
        }
        it = jobs_.erase(it);
        counters_.pending.fetch_sub(1, std::memory_order_relaxed);
    }
    return Status::OK();
}

bool PageReclaimer::WalkSome(Job& job, std::size_t& budget) {
    for (;;) {
        while (!job.frontier.empty() && budget > 0) {
            const Visit visit = job.frontier.back();
            job.frontier.pop_back();
            --budget;
            if (!VisitOne(job, visit)) return false;
            if (job.stalled) {
                // The pool refused a fault (BE-R4): the visit goes back and
                // the step ends; the next one asks again (BF-R3).
                job.stalled = false;
                job.frontier.push_back(visit);
                budget = 0;
                return true;
            }
        }
        if (!job.frontier.empty()) return true;  // out of budget, mid-walk
        // The descents are done. A leaf's sibling link that names a page no
        // descent took is a leaf a failed promotion left on the chain alone
        // (BF-S1's Census D): walked now, as a leaf of the same tree.
        if (job.leaf_links.empty()) return true;
        for (const Visit& link : job.leaf_links) {
            if (link.page != kInvalidPageId && job.seen.count(link.page) == 0) {
                job.frontier.push_back(link);
            }
        }
        job.leaf_links.clear();
        if (job.frontier.empty()) return true;
    }
}

bool PageReclaimer::Take(Job& job, const Visit& visit, PageId page) {
    (void)visit;  // a link to a page already taken never gets here (`VisitOne`)
    if (!job.seen.insert(page).second) {
        Refuse(job, "page " + std::to_string(page) + " is reached twice by the walk");
        return false;
    }
    return true;
}

bool PageReclaimer::VisitOne(Job& job, const Visit& visit) {
    if (visit.page == kInvalidPageId) return true;
    if (visit.by_link && job.seen.count(visit.page) != 0) return true;
    auto ref = store_.GetForRead(visit.page);
    if (!ref.ok()) {
        // Already free: a re-drive meeting what an earlier pass freed. Its
        // children went first (BF-R7), so there is nothing below it to find.
        // A census walk has no earlier pass: there it is a link to a page
        // that is not there, and counts.
        if (ref.status().code() == StatusCode::kNotFound) {
            if (job.census) Skip(job, visit.page, "is not allocated, or was never written");
            return true;
        }
        if (ref.status().code() == StatusCode::kResourceExhausted) {
            job.stalled = true;
            return true;
        }
        Refuse(job, "page " + std::to_string(visit.page) + " could not be read: " +
                        ref.status().message());
        return false;
    }
    const std::span<std::byte, kPageSize> bytes = ref.value().bytes();
    const std::uint8_t type = storage::RawPageType(bytes);
    const std::uint64_t owner = storage::GetOwnerOid(bytes);

    switch (visit.expect) {
        case Expect::kAnchor: {
            if (type != TypeByte(PageType::kAnchor) || owner != visit.owner) {
                // The anchor is freed last, after every other page's free is
                // durable, so an anchor that is gone - reused by another
                // relation - says the rest is gone too.
                Skip(job, visit.page, "is not this relation's anchor");
                return true;
            }
            // **An index job reads its one slot and takes nothing else**
            // (BY-R4): the anchor is the live relation's, never freed, and
            // the root is the slot's now - after the gates - not the drop's,
            // since the dropper's own writes can split it before commit
            // (BY-R2). No slot, or no root, is a tree already reclaimed.
            if (job.index_oid != 0) {
                auto root = storage::AnchorIndexRoot(bytes, job.index_oid);
                if (!root.ok()) {
                    Refuse(job, "its anchor's index slots do not read: " +
                                    root.status().message());
                    return false;
                }
                if (root.value() != kInvalidPageId) {
                    job.frontier.push_back(
                        Visit{root.value(), Expect::kIndex, job.index_oid, kAnyLevel});
                }
                return true;
            }
            if (!Take(job, visit, visit.page)) return false;
            job.anchor_live = true;
            job.frontier.push_back(
                Visit{storage::AnchorClusteredRoot(bytes), Expect::kBtree, job.oid, kAnyLevel});
            auto slots = storage::AnchorIndexSlots(bytes);
            if (!slots.ok()) {
                Refuse(job, "its anchor's index slots do not read: " + slots.status().message());
                return false;
            }
            for (const storage::AnchorIndexSlot& slot : slots.value()) {
                job.frontier.push_back(Visit{slot.root, Expect::kIndex, slot.index_oid, kAnyLevel});
            }
            if (job.roots.varheap != kInvalidPageId) {
                job.chains.emplace_back();
                job.frontier.push_back(Visit{job.roots.varheap, Expect::kVarHeapChain, job.oid, 0,
                                             static_cast<std::uint32_t>(job.chains.size() - 1)});
            }
            return true;
        }

        case Expect::kBtree:
        case Expect::kIndex: {
            const bool index = visit.expect == Expect::kIndex;
            const PageType leaf = index ? PageType::kIndexLeaf : PageType::kBtreeLeaf;
            const PageType internal = index ? PageType::kIndexInternal : PageType::kBtreeInternal;
            if (owner != visit.owner) {
                Skip(job, visit.page, "is owned by oid " + std::to_string(owner));
                return true;
            }
            // A pre-SUS-1 heap relation's anchor names its chain's head:
            // walked as a chain, from this page.
            if (!index && visit.level == kAnyLevel && type == TypeByte(PageType::kHeap)) {
                job.chains.emplace_back();
                job.frontier.push_back(Visit{visit.page, Expect::kHeapChain, visit.owner, 0,
                                             static_cast<std::uint32_t>(job.chains.size() - 1)});
                return true;
            }
            if (type == TypeByte(leaf) && (visit.level == 0 || visit.level == kAnyLevel)) {
                if (!Take(job, visit, visit.page)) return false;
                if (job.levels.empty()) job.levels.emplace_back();
                job.levels[0].push_back(visit.page);
                const PageId next = index ? index::IndexLeafView(bytes).right_sibling()
                                          : heap::PageView(bytes).next_page_id();
                if (next != kInvalidPageId) {
                    Visit link{next, visit.expect, visit.owner, 0};
                    link.by_link = true;
                    job.leaf_links.push_back(link);
                }
                return true;
            }
            if (type != TypeByte(internal)) {
                Skip(job, visit.page, "is not the tree page its parent names");
                return true;
            }
            const std::uint16_t level =
                index ? index::IndexInternalView(bytes).level() : btree::InternalView(bytes).level();
            if (level == 0 || (visit.level != kAnyLevel && level != visit.level)) {
                Skip(job, visit.page, "is at level " + std::to_string(level) +
                                          " where its parent routes to another");
                return true;
            }
            if (!Take(job, visit, visit.page)) return false;
            if (job.levels.size() <= level) job.levels.resize(level + 1u);
            job.levels[level].push_back(visit.page);
            // Children pushed right to left, so the leftmost is walked first.
            std::vector<PageId> children;
            if (index) {
                index::IndexInternalView node(bytes);
                children.push_back(node.leftmost_child());
                for (std::uint16_t i = 0; i < node.entry_count(); ++i) {
                    auto child = node.Child(i);
                    if (!child.ok()) {
                        Refuse(job, "index node " + std::to_string(visit.page) +
                                        " does not read: " + child.status().message());
                        return false;
                    }
                    children.push_back(child.value());
                }
            } else {
                btree::InternalView node(bytes);
                children.push_back(node.leftmost_child());
                for (std::uint16_t i = 0; i < node.entry_count(); ++i) {
                    auto entry = node.Entry(i);
                    if (!entry.ok()) {
                        Refuse(job, "tree node " + std::to_string(visit.page) +
                                        " does not read: " + entry.status().message());
                        return false;
                    }
                    children.push_back(entry.value().child);
                }
            }
            for (auto child = children.rbegin(); child != children.rend(); ++child) {
                job.frontier.push_back(Visit{*child, visit.expect, visit.owner,
                                             static_cast<std::uint16_t>(level - 1)});
            }
            return true;
        }

        case Expect::kHeapChain:
        case Expect::kVarHeapChain: {
            const PageType want =
                visit.expect == Expect::kHeapChain ? PageType::kHeap : PageType::kVarHeap;
            if (type != TypeByte(want) || owner != visit.owner) {
                Skip(job, visit.page, "is not a page of this relation's chain");
                return true;
            }
            if (!Take(job, visit, visit.page)) return false;
            job.chains[visit.chain].push_back(visit.page);
            const PageId next = visit.expect == Expect::kHeapChain
                                    ? heap::PageView(bytes).next_page_id()
                                    : varheap::PageNextPageId(bytes);
            if (next != kInvalidPageId) {
                job.frontier.push_back(Visit{next, visit.expect, visit.owner, 0, visit.chain});
            }
            return true;
        }
    }
    return true;
}

void PageReclaimer::BuildPlan(Job& job) {
    // Children before parents, one level at a time, with the map synced
    // after each: a parent's cleared bit never reaches the device ahead of a
    // child's, so a re-drive can always descend to whatever is left.
    for (const std::vector<PageId>& level : job.levels) {
        for (const PageId page : level) job.plan.push_back(Free{page, false});
        if (!level.empty()) job.plan.back().sync_after = true;
    }
    // Each chain tail-first, a sync per page (BF-Q18 (c)): a crash leaves a
    // prefix still linked from the head the tombstone names.
    for (const std::vector<PageId>& chain : job.chains) {
        for (auto page = chain.rbegin(); page != chain.rend(); ++page) {
            job.plan.push_back(Free{*page, true});
        }
    }
    // The anchor last: an anchor that is gone means every other free is
    // durable, which is what lets a re-drive read it that way.
    if (job.anchor_live) job.plan.push_back(Free{job.roots.anchor, true});
}

bool PageReclaimer::FreeSome(Job& job, std::size_t& budget) {
    while (job.next < job.plan.size() && budget > 0) {
        const Free& free = job.plan[job.next];
        auto freed = store_.FreePage(free.page);
        if (freed.ok() && freed.value() == storage::PageStore::FreeOutcome::kDeferred) {
            // A frame someone holds: asked again at the next step, and the
            // plan does not move past it, so no parent goes ahead of it.
            counters_.deferred.fetch_add(1, std::memory_order_relaxed);
            budget = 0;
            return true;
        }
        if (!freed.ok() && freed.status().code() != StatusCode::kNotFound) {
            Refuse(job, "page " + std::to_string(free.page) +
                            " was not freed: " + freed.status().message());
            return false;
        }
        --budget;
        Cut("after a free");
        if (free.sync_after) {
            if (Status s = store_.PersistMaps(); !s.ok()) {
                Refuse(job, "the map sync failed: " + s.message());
                return false;
            }
            Cut("after a sync");
        }
        ++job.next;
    }
    return true;
}

StatusOr<PageReclaimer::Reach> PageReclaimer::ReachFrom(catalog::Oid oid,
                                                        catalog::PendingRoots roots) {
    Job job;
    job.oid = oid;
    job.roots = roots;
    job.census = true;
    job.frontier.push_back(Visit{roots.anchor, Expect::kAnchor, oid, kAnyLevel});
    std::size_t budget = static_cast<std::size_t>(-1);
    if (!WalkSome(job, budget)) return Status::Corruption(job.refusal);
    // A refused fault leaves the visit on the frontier: not the whole reach.
    if (!job.frontier.empty()) {
        return Status::ResourceExhausted("the walk met a refused fault at page " +
                                         std::to_string(job.frontier.back().page));
    }
    Reach out;
    out.pages.assign(job.seen.begin(), job.seen.end());
    std::sort(out.pages.begin(), out.pages.end());
    out.skipped = job.skipped;
    return out;
}

void PageReclaimer::Refuse(Job& job, const std::string& why) {
    job.phase = Phase::kRefused;
    job.refusal = why;
    if (job.census) return;
    counters_.refused.fetch_add(1, std::memory_order_relaxed);
    if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
        log_->Warn("reclaim", JobName(job) +
                                  " is left pending until the next mount: " + why);
    }
}

void PageReclaimer::Skip(Job& job, PageId page, const std::string& why) {
    ++job.skipped;
    if (job.census) return;
    counters_.skipped.fetch_add(1, std::memory_order_relaxed);
    if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
        log_->Warn("reclaim", JobName(job) + ": page " +
                                  std::to_string(page) + " " + why +
                                  "; neither freed nor walked through");
    }
}

}  // namespace kds::server
