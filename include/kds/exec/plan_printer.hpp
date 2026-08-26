#pragma once

#include <string>
#include <vector>

#include "kds/exec/step_chain.hpp"
#include "kds/exec/step_vm.hpp"

// Renders a compiled StepChain, and optionally what executing it cost, as
// text a person reads. For `ANALYZE <statement>` (command_dispatcher.hpp)
// and for tests that want to assert on a plan rather than on the rows it
// produced.
//
// ---- Why this is only a printer -----------------------------------------
//
// There is no plan *selection* in this engine to explain. Written order is
// execution order and is a documented client contract (docs/spec/parser-v2.md
// §1: "the statement is the chain", never silently reordered), so a plan
// is not a choice the optimizer made - it is the statement, restated with
// the one thing the compiler did decide made visible: each step's
// `AccessKind`. This file therefore describes; it never justifies, and
// there is no cost estimate to print because nothing computes one.
//
// ---- Names ---------------------------------------------------------------
//
// A compiled chain carries no identifiers on any execute path (spec I11).
// The two exceptions are display-only and filled at compile time -
// `StepChain::column_names` and `Step::rel_name` - and this file reads
// exactly those. Columns *inside* a plan are printed structurally, as
// `<up>:<rel_slot>.<col_pos>`, because a ColumnRef is what the executor
// actually holds and inventing a name for it here would mean resolving one
// against a catalog, which is the thing the rule forbids.
//
// ---- Concurrency ---------------------------------------------------------
//
// Pure functions over caller-owned values. No store, no catalog, no I/O.

namespace kds::exec {

// The plan alone: one line per step, plus its sub-chains, indented.
//
// Sections are joined with '\n'. The dispatcher's one-line wire contract
// is *not* applied here - escaping is the caller's job, because the same
// text goes to a test's assertion unescaped and to a client escaped.
std::string FormatPlan(const StepChain& chain);

// The per-step counters an execution left behind, one line per step that
// did anything.
//
// `stats` is indexed by step_id (step_vm.hpp), and so is the chain's own
// numbering, so the two line up without any lookup. A step with no
// counters is omitted rather than printed as zeros: for a chain with
// sub-chains the interesting fact is usually which steps ran at all - a
// false hoisted EXISTS leaves the outer steps untouched, and a row of
// zeros would bury that.
std::string FormatStepStats(const StepChain& chain, const ExecStats& stats);

// Human names for the two enums, so a plan does not print integers.
const char* AccessKindName(AccessKind kind) noexcept;
const char* StatementClassName(StatementClass klass) noexcept;

}  // namespace kds::exec
