#pragma once

#include <cstdint>
#include <string>

#include "kds/base/status.hpp"

// A per-statement bound on how much work one statement may do
// (docs/inflight/in-progress/parser-v2-workplan.md V19).
//
// ---- Why a statement needs a bound at all -------------------------------
//
// Nothing suspends mid-statement. The engine is thread-per-core and
// cooperative (docs/rules/rules.md §3): a statement runs to completion on the
// core it started on, and no scheduler takes the core away from it. So a
// statement that reads an unbounded number of rows does not merely run
// slowly - it holds a core for as long as it takes, and every other client
// on that core waits.
//
// A correlated subquery over a non-pk column is the easy way to write one.
// `WHERE EXISTS (SELECT ... WHERE inner.col = outer.col)` with a non-pk
// `col` walks the whole inner relation once per outer row: two relations of
// n rows is n² tuple decodes from a statement that fits on one line and
// looks entirely reasonable. At a million rows each that is 10¹² decodes,
// which is not a slow query, it is an outage.
//
// **Failing it is the kinder answer.** A client that gets
// `ResourceExhausted` after a bounded amount of work can rewrite the
// statement; a client whose connection hangs, taking the core with it,
// cannot tell what happened and neither can the operator.
//
// ---- What is counted ----------------------------------------------------
//
// Tuples *decoded*, across every step and every sub-chain of the statement,
// which is the unit that tracks real work: a page fetch amortizes over the
// tuples on it, but every tuple is decoded and filtered individually. It is
// deliberately not a timer - a wall-clock budget makes a statement's
// success depend on what else the machine was doing, so the same statement
// on the same data would pass and fail at random.

namespace kds::exec {

// Default row-touch ceiling for one statement.
//
// Derivation: a full scan of a relation that fills a 64 MB buffer pool is
// on the order of 10⁶ tuples at the row sizes this engine targets, and a
// legitimate join over two such relations touches a few multiples of that.
// 100 million leaves two orders of magnitude of headroom above anything
// deliberate while still catching an accidental n² over relations of any
// interesting size - at ~10⁷ decodes/s that is a bounded ~10 s, not an
// unbounded wait.
//
// A ceiling, not a target. It is `[PROPOSED]`: the right number comes from
// measuring the workloads this engine is actually pointed at, and until
// then it is set where it can only catch accidents.
inline constexpr std::uint64_t kDefaultRowTouchBudget = 100'000'000;

// 0 means unlimited, which the config key documents and which exists for
// one honest use: measuring the cost of the very statement the budget
// would refuse.
inline constexpr std::uint64_t kUnlimitedRowTouchBudget = 0;

// `join_build_max_rows`' ratified default (join-inner-build.md §7,
// workplan JB5). Rows, not bytes, following `aggregate_max_groups`'
// argument: an entry count is the number an operator can reason about
// against an entry width they know (24 bytes, C6). **Refusal semantics
// are the Cabin's, not the aggregate's**: past the cap the step reverts
// to per-row walks for the rest of the statement - always legal, never an
// error, because the map is a shortcut and the walk is always there. 0
// disables the build outright: no map can hold a row - **the opposite of
// the row-touch limit's zero above, deliberately**, because "unlimited
// map" has a number (the relation's size) where "unlimited work" does
// not. Scoping across a chain with two walked inners is `[OPEN]`
// (CLAUDE.md) - one knob, checked per build, which keeps both readings
// implementable.
inline constexpr std::size_t kDefaultJoinBuildMaxRows = 65536;

class Budget {
public:
    explicit Budget(std::uint64_t limit = kDefaultRowTouchBudget) noexcept : limit_(limit) {}

    // The statement-local inner build's cap (workplan JB5). Semantics at
    // `kDefaultJoinBuildMaxRows` above - the one home three sites point at.
    std::size_t join_build_max_rows() const noexcept { return join_build_max_rows_; }
    void set_join_build_max_rows(std::size_t rows) noexcept { join_build_max_rows_ = rows; }

    // A copy of the limits and knobs with the spend counter at zero - what
    // an entry point takes, so a Budget stays reusable across statements
    // without a caller resetting it. A new field is carried by being a
    // member; no entry point can forget one (one already did: the
    // limit-only copy this replaced silently dropped `join_build_max_rows`
    // until a contract test caught it). Deliberately not the copy
    // constructor - a copy that silently zeroes a counter is a lie.
    Budget Fresh() const noexcept {
        Budget out = *this;
        out.touched_ = 0;
        return out;
    }

    // Charges one decoded tuple. Fails once the statement has spent its
    // budget, and keeps failing - the caller stops at the first refusal,
    // and a caller that does not must not see the count wrap.
    Status ChargeRow() noexcept {
        ++touched_;
        if (limit_ == kUnlimitedRowTouchBudget || touched_ <= limit_) return Status::OK();
        return Exhausted();
    }

    std::uint64_t touched() const noexcept { return touched_; }
    std::uint64_t limit() const noexcept { return limit_; }
    bool unlimited() const noexcept { return limit_ == kUnlimitedRowTouchBudget; }

private:
    // Out of line in the .cpp: building the message allocates, and this is
    // called from a branch on the hot path that must stay cheap when the
    // budget is not exceeded.
    Status Exhausted() const;

    std::uint64_t limit_;
    std::uint64_t touched_ = 0;
    std::size_t join_build_max_rows_ = kDefaultJoinBuildMaxRows;
};

}  // namespace kds::exec
