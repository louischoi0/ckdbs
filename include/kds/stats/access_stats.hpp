#pragma once

#include <cstdint>

#include "kds/catalog/catalog.hpp"
#include "kds/exec/step_chain.hpp"

// Recording what a statement's steps actually did, per access *shape*
// (`docs/spec/heap-and-tuple.md` §7).
//
// §7 has said "KDS collects access statistics and uses them to physically
// optimize tuple placement" since the spec was written, immediately followed
// by "*Nothing here is implemented*". This is the collecting half. The
// optimizing half - relayout - is still not implemented, and nothing here
// pretends otherwise: this produces the input a physical optimizer would
// need and stops there.
//
// ---- One call, no per-kind branch ---------------------------------------
//
// A `kLookup` step is recorded by exactly the code that records a
// `kFilterScan`. That is what "the same interface for every access kind"
// means in practice, and it is worth stating because the alternative is so
// easy to write by accident: a `switch` here would let the kinds drift into
// having different statistics, and the question this data answers - *which
// access shapes does this workload actually run* - only means anything if
// every shape is counted the same way.
//
// ---- Columns, not values -------------------------------------------------
//
// The shape is `(kind, rel_id, column_mask)`. `WHERE flag = 1` and
// `WHERE flag = 2` are one shape. That bounds the relation by the schema
// rather than by the data, so there is no eviction policy to invent and no
// directory to walk. Values already identify something else entirely - a
// Waystone pattern instance, via `arg_hash` - and keeping the two layers
// apart is what lets each stay simple.
//
// ---- It must never be able to fail a query -------------------------------
//
// Every function here returns void. A statistic that could not be written is
// a statistic that was not written; the query already succeeded, and failing
// it afterwards to report a counting problem would be strictly worse than
// having no counter. Failures are counted so "the statistics quietly stopped
// working" is visible rather than assumed.

namespace kds::stats {

// Counters for the recorder itself, so a silent stop is detectable.
struct AccessStatsCounters {
    std::uint64_t steps_recorded = 0;
    std::uint64_t write_failures = 0;  // includes the shape cap being reached
};

// Records one execution of every step in `chain`, including its sub-chains.
//
// Called after a **successful** statement. A statement that errored part way
// through ran some of its steps and not others, and a count that included it
// would describe a workload nobody ran.
//
// `now` is the injected clock's reading, or 0 when there is no clock -
// `last_seen` is best-effort in the same sense `sys.patterns`' is.
void RecordChainAccess(catalog::Catalog& catalog, const exec::StepChain& chain,
                       std::uint64_t now, AccessStatsCounters* counters = nullptr);

// The column bitmap for one step's access shape.
//
// Exposed for tests and for `SHOW ACCESS`'s reverse rendering. A column
// past 63 folds into no bit, which makes the shape coarser rather than
// wrong - see `catalog::SysAccessStatRow::column_mask`.
std::uint64_t ColumnMaskOf(const exec::Step& step) noexcept;

}  // namespace kds::stats
