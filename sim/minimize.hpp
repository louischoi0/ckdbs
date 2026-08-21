#pragma once

// sim/minimize.hpp — the minimizer and the `.sim` case file (bench/
// workplan-teststrategy SIM07).
//
// A failing seed at op 80,000 is a fact, not a diagnosis. This shrinks the
// failing iteration's plan by delta debugging — drop a chunk of ops,
// replay, keep the drop if the *same* failure survives — until no single
// removal preserves it, and writes what is left as a standalone case that
// replays without the generator.
//
// **Same failure, not any failure.** Dropping ops can easily produce a
// different one (drop the CREATE TABLE and everything after it errors), so
// the predicate is a normalized *signature* of the failure detail rather
// than "it failed": digit runs collapse to `#`, the bracketed SQL and the
// fault trace are cut, and what remains is the shape of the complaint. A
// minimizer without that check does not shrink a bug, it wanders to a
// different one.
//
// Two limits, both stated rather than discovered: the signature can still
// conflate two failures that read alike, and a plan whose failure needs a
// *specific* op count (a page fill, a segment roll) shrinks badly, because
// every removal changes the thing the failure depends on. Both make the
// output a lead, not a verdict — read the case, do not just run it.

#include <cstddef>
#include <string>

#include "kds/base/status.hpp"

#include "sim/loop.hpp"

namespace kds::sim {

// The failure's shape, with everything run-specific removed. Two runs that
// fail the same way share it; the empty string means "did not fail".
std::string FailureSignature(const SimVerdict& verdict);

struct MinimizeOutcome {
    bool found_failure = false;
    std::size_t iteration = 0;   // the first iteration that failed
    std::string signature;
    SimPlan plan;                // the shrunk plan, when one was found
    std::size_t ops_before = 0;
    std::size_t ops_after = 0;
    std::size_t replays = 0;     // instances built while shrinking

    std::string Summary() const;
};

// Runs `config`'s iterations until one fails, then shrinks that
// iteration's plan. `max_replays` bounds the search: a shrink that hits it
// stops where it is and says so, which is a smaller case and an honest
// one, never a wrong one.
MinimizeOutcome MinimizeFailure(const SimConfig& config, std::size_t max_replays = 4000);

// The `.sim` case file: the config header, then one line per op with its
// armed faults, replayable with no seed and no generator.
Status WriteCase(const std::string& path, const SimConfig& config, const SimPlan& plan,
                 const std::string& signature);

struct LoadedCase {
    SimConfig config;
    SimPlan plan;
    std::string signature;
};

StatusOr<LoadedCase> ReadCase(const std::string& path);

}  // namespace kds::sim
