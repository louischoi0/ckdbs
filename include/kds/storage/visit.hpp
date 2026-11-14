#pragma once

#include <cstdint>
#include <string>

#include "kds/base/cycle_guard.hpp"
#include "kds/base/common.hpp"
#include "kds/base/status.hpp"

// What a relation walk's visitor tells the walk to do next
// (docs/spec/parser-v2.md I15 rule 4).
//
// Both walks over a relation's tuples - heap::ChainVisit and
// btree::BtreeVisit - must be stoppable *successfully*: an `Exists` step
// short-circuits at the first match, `LIMIT n` stops at n rows, and a cost
// guard stops when a scan has read its page allowance. So control and
// failure travel in different channels: the value says
// whether to keep going, the Status says whether anything went wrong. A
// visitor returns StatusOr<VisitControl> - kContinue for "next slot",
// kStop for "I have what I came for", or a non-ok Status for a real
// failure. kStop ends the walk with Status::OK().

namespace kds::storage {

enum class VisitControl : std::uint8_t {
    // Deliberately the zero value: it is the answer for the overwhelming
    // majority of slots, and the one a value-initialized VisitControl
    // carries.
    kContinue = 0,
    kStop = 1,
};

// The one place the "visitor returned Status::OK()" mistake is caught, so
// the two walk loops do not each spell it out. `what` names the walk for
// the message; the caller has nothing else to identify the callback by.
//
// A valueless-but-ok StatusOr is a defect in the visitor, not in the data,
// hence kInvalidArgument: the `fn` argument handed to the walk did not
// honour its contract. Reporting it beats the alternative, which is
// value() dereferencing an empty optional in the middle of a scan.
inline StatusOr<VisitControl> ResolveVisit(StatusOr<VisitControl> outcome, const char* what) {
    if (!outcome.ok()) return outcome.status();
    if (!outcome.has_value()) {
        return Status::InvalidArgument(std::string(what) +
                                       " visitor returned an ok Status carrying no VisitControl; "
                                       "return VisitControl::kContinue, not Status::OK()");
    }
    return outcome;
}

// The most hops a **catalog or heap chain** walk may take from one origin
// (`catalog.md`, `heap-and-tuple.md` §3): 2^20 pages, 8 GiB at 8 KiB each,
// a budget no system relation nears. What it bounds is a cyclic or corrupt
// link, which without it is an infinite loop inside a statement.
// heap::kMaxChainPages aliases this. **A user relation's or an index's leaf
// chain is not bounded by it** - it grows with the data past any budget -
// but by `LeafChainGuard` below (the audit's G19).
inline constexpr std::uint32_t kMaxPageWalkLength = 1u << 20;

// The guard itself, shared for ResolveVisit's reason: every catalog- or
// heap-chain loop (the heap chain, the catalog chains, and each caller that
// steps their pages itself) owes the same check, and copies drift; a
// leaf-chain walk owes `LeafChainGuard` below instead. Call with the count of pages already visited, before visiting the
// next; `what` names the walk for the message.
inline Status CheckPageWalkBudget(std::uint32_t pages_visited, PageId origin, const char* what) {
    if (pages_visited < kMaxPageWalkLength) return Status::OK();
    return Status::Corruption(std::string(what) + " from page " + std::to_string(origin) +
                              " exceeds " + std::to_string(kMaxPageWalkLength) +
                              " pages; the page links are cyclic or corrupt");
}

// **The bound of a leaf-chain walk**: a btree's or an index's leaves, from
// any leaf rightward. A cycle check (`CycleGuard`), never a length, because
// a relation's leaf chain is as long as its data - a budget refused every
// whole walk of a relation past 8 GiB of leaves as `Corruption` (the audit's
// G19). Sound because no legal walk revisits a leaf: a live tree never
// frees one (a drop or a rebuild waits out every walk that could bind it),
// a split keeps a leaf's id and links the new page after it, and a sliced
// walk that yields between leaves resumes at an id it has not visited.
//
// Call `Visit` with each page the walk is about to read, its origin first;
// the first call names the origin. `what` names the walk for the message.
class LeafChainGuard {
public:
    explicit LeafChainGuard(const char* what = "leaf chain") noexcept : what_(what) {}

    Status Visit(PageId page) {
        if (origin_ == kInvalidPageId) {
            origin_ = page;
            cycle_ = CycleGuard<PageId>(page);
            return Status::OK();
        }
        if (!cycle_.Revisits(page)) return Status::OK();
        return Status::Corruption(std::string(what_) + " from page " + std::to_string(origin_) +
                                  " revisits page " + std::to_string(page) +
                                  "; the page links are cyclic or corrupt");
    }

private:
    const char* what_;
    PageId origin_ = kInvalidPageId;
    CycleGuard<PageId> cycle_{kInvalidPageId};
};

}  // namespace kds::storage
