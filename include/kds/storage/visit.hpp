#pragma once

#include <cstdint>
#include <string>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"

// What a relation walk's visitor tells the walk to do next
// (docs/spec/parser-v2.md I15 rule 4, workplan V03).
//
// Both walks over a relation's tuples - heap::ChainVisit over a page chain
// and btree::BtreeVisit over the leaf siblings - used to run to the end of
// the relation unconditionally. The only way out was a non-ok Status from
// the visitor, which made "did it fail, or did it finish early on purpose?"
// unanswerable at the call site: the walk returned kInvalidArgument either
// way, and the caller had to guess from a message.
//
// That is not a style complaint. Three things in the query language need to
// stop a walk *successfully*: an `Exists` step short-circuits at the first
// match, `LIMIT n` stops at n rows, and a cost guard stops when a scan has
// read more pages than it was allowed. All three are ordinary successful
// outcomes, and encoding them as errors would make every caller upstream
// re-derive whether an error was really an error.
//
// So control and failure travel in different channels: the value says
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

// The most hops any next-page walk may take from one origin. Far above any
// real relation (2^20 pages = 8 GiB at 8 KiB each); what it bounds is a
// cyclic or corrupt link, which without it is an infinite loop inside a
// statement. heap::kMaxChainPages aliases this - the heap chain named the
// rule first, but it was never heap-specific.
inline constexpr std::uint32_t kMaxPageWalkLength = 1u << 20;

// The guard itself, shared for ResolveVisit's reason: every loop that
// follows next-page links (the heap chain, the btree leaf siblings, and
// each caller that steps pages itself) owes the same check, and copies
// drift. Call with the count of pages already visited, before visiting the
// next; `what` names the walk for the message.
inline Status CheckPageWalkBudget(std::uint32_t pages_visited, PageId origin, const char* what) {
    if (pages_visited < kMaxPageWalkLength) return Status::OK();
    return Status::Corruption(std::string(what) + " from page " + std::to_string(origin) +
                              " exceeds " + std::to_string(kMaxPageWalkLength) +
                              " pages; the page links are cyclic or corrupt");
}

}  // namespace kds::storage
