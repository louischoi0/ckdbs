#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/parser/ast.hpp"

// The values a chain has bound so far, and the one place a `ColumnRef`
// turns into a value (docs/inflight/in-progress/parser-v2-workplan.md V16).
//
// ---- Why one buffer, not one vector per step ---------------------------
//
// A chain of n relations decodes one row per step per output row. Giving
// each step its own `std::vector<AstValue>` means n allocations per
// candidate row and n more frees, on the hot path of every join - which is
// how an O(1)-allocation scan becomes an O(rows) one without anybody
// deciding to make it so. Instead every step's values live in one buffer
// at a fixed base offset, and the buffer is sized once when the frame is
// opened. `ColumnRef{up, rel_slot, col_pos}` resolves in one add.
//
// ---- The frame stack ---------------------------------------------------
//
// `up` is a de Bruijn level: 0 is this chain, 1 its parent. A correlated
// sub-chain runs with its own frame whose `parent` is the outer one, so an
// outward reference walks up exactly `up` links. That is the whole of
// correlation at execute time - the compiler already decided what refers
// to what, and nothing here re-derives it.
//
// **Correlation values are read through this stack and never written back
// into the AST.** The AST is shared (`shared_ptr<SelectStmt>`) and would
// otherwise be mutated per outer row, which makes a statement's meaning
// depend on how far execution has got.

namespace kds::exec {

class ChainFrame {
public:
    ChainFrame() = default;

    // Sizes the buffer for `schemas` - one entry per step, in chain
    // order. Called once per chain execution, not per row.
    void Open(const std::vector<const catalog::Schema*>& schemas, const ChainFrame* parent) {
        parent_ = parent;
        bases_.clear();
        bases_.reserve(schemas.size());
        std::size_t total = 0;
        for (const catalog::Schema* schema : schemas) {
            bases_.push_back(total);
            total += schema->columns.size();
        }
        // resize, not clear+push_back: the buffer is reused for every row
        // the chain produces, so this allocates on the first row of a
        // statement and never again.
        values_.resize(total);
    }

    // Where slot `rel_slot`'s values end: the next slot's base, or the end
    // of the buffer for the last slot. One home for it, because both callers
    // below need exactly this and a second copy is a second thing to get
    // right. `std::size_t` in the parameter rather than the caller's
    // uint16_t, so `+ 1` never promotes to *int* and the comparison against
    // size() is never signed-vs-unsigned.
    std::size_t EndOfSlot(std::size_t rel_slot) const {
        const std::size_t next = rel_slot + 1;
        return next < bases_.size() ? bases_[next] : values_.size();
    }

    // The slice belonging to one step, for a decoder to fill.
    std::span<parser::AstValue> SlotsFor(std::uint16_t rel_slot) {
        const std::size_t base = bases_[rel_slot];
        const std::size_t end = EndOfSlot(rel_slot);
        return std::span<parser::AstValue>(values_.data() + base, end - base);
    }

    // Resolves a compiled reference. Walks `up` parent links, then indexes -
    // no name comparison, no search.
    const parser::AstValue& Get(const ColumnRef& ref) const {
        const ChainFrame* frame = this;
        for (std::uint16_t i = 0; i < ref.up; ++i) frame = frame->parent_;
        return frame->values_[frame->bases_[ref.rel_slot] + ref.col_pos];
    }

    // True when `ref` can be resolved from here - the execute-time half of
    // the bound the compiler already enforced. Cheap, and it turns a
    // malformed chain into a reported error rather than a read past the
    // end of a vector.
    bool CanResolve(const ColumnRef& ref) const {
        const ChainFrame* frame = this;
        for (std::uint16_t i = 0; i < ref.up; ++i) {
            if (frame->parent_ == nullptr) return false;
            frame = frame->parent_;
        }
        if (ref.rel_slot >= frame->bases_.size()) return false;
        return frame->bases_[ref.rel_slot] + ref.col_pos < frame->EndOfSlot(ref.rel_slot);
    }

private:
    std::vector<parser::AstValue> values_;
    std::vector<std::size_t> bases_;  // one per step: where its columns start
    const ChainFrame* parent_ = nullptr;
};

// Evaluates one compiled conjunct against a frame.
//
// Replaces the name-matching evaluator outright rather than sitting beside
// it. That one resolved a predicate by comparing column *names* against a
// single schema, which in a multi-relation world is silently wrong twice
// over: it cannot tell which relation a name belongs to, and an unknown
// name returns "no match" - dropping every row instead of failing. Two
// evaluators would also be two answers to "does this row qualify", and
// the whole point of `Probe` and `Scan` agreeing row-for-row is that
// there is exactly one.
//
// Fails only on a malformed chain (a reference the frame cannot resolve).
// A type mismatch is a non-match, not an error, matching what SQL's
// NULL-comparison semantics would give in the absence of real NULLs.
StatusOr<bool> EvaluatePredicate(const catalog::Schema& lhs_schema, const StepPredicate& pred,
                                 const ChainFrame& frame);

// One conjunct through the full residual discipline - the `$param` refusal
// and the outward-lhs fallback included. This is EvaluateAll's loop body,
// exposed because the inner build (workplan JB3) evaluates a step's
// correlated conjunct separately from the rest; one body, so the split
// evaluation cannot drift from the whole one.
StatusOr<bool> EvaluateConjunct(const std::vector<const catalog::Schema*>& schemas,
                                const StepPredicate& pred, const ChainFrame& frame);

// Every conjunct attached to one step. Empty always matches.
StatusOr<bool> EvaluateAll(const std::vector<const catalog::Schema*>& schemas,
                           const std::vector<StepPredicate>& predicates, const ChainFrame& frame);

// Every conjunct but the one at `skip` - the inner build's bucketing
// predicate (join-inner-build.md §2: the map holds every row passing
// the *non-correlated* residual). Together with the skipped conjunct this
// is exactly EvaluateAll, which is what keeps emission untouched.
StatusOr<bool> EvaluateAllExcept(const std::vector<const catalog::Schema*>& schemas,
                                 const std::vector<StepPredicate>& predicates,
                                 const ChainFrame& frame, std::size_t skip);

}  // namespace kds::exec
