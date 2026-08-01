#pragma once

#include <cstdint>

#include "kds/base/status.hpp"

// What a relation walk's visitor tells the walk to do next
// (docs/parser-v2.md I15 rule 4, workplan V03).
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

}  // namespace kds::storage
