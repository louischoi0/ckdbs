#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/parser/ast.hpp"

// The values a chain has bound so far, and the one place a `ColumnRef`
// turns into a value (docs/inflight/in-progress/parser-v2-workplan.md V16).
//
// ---- Why one buffer, not one vector per step ---------------------------
//
// A vector per step would cost n allocations per candidate row on every
// join's hot path. Instead every step's values live in one buffer
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
    // of the buffer for the last slot. `std::size_t` in the parameter rather than the caller's
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

// **The one evaluator of compiled conjuncts.** Two evaluators would be two
// answers to "does this row qualify", and the whole point of `Probe` and
// `Scan` agreeing row-for-row is that there is exactly one. A type
// mismatch is a non-match, not an error.
//
// **A conjunct is checked once and compared per row** (BM-R6,
// `workorder-bm-scan-row-kernel.md`; BT-R5, `workorder-bt-accept-tuple-row.md`).
// The check - the `$param` refusal and the malformed-chain checks, with
// their messages - depends only on the chain and the frame's shape, never
// on a row, so every caller runs it at its first row and then compares.
// It covers the step's `residual` and every comparison inside its
// `bool_residual` (BU-S1); the first refusal ends it.
Status CheckConjuncts(const std::vector<const catalog::Schema*>& schemas, const Step& step,
                      const ChainFrame& frame);

// One conjunct's comparison, on a conjunct CheckConjuncts has passed
// against this frame - the outward-lhs fallback included. Exposed because
// the inner build (workplan JB3) evaluates a step's correlated conjunct
// apart from the rest; one body, so the split cannot drift from the whole.
bool EvaluateConjunctChecked(const std::vector<const catalog::Schema*>& schemas,
                             const StepPredicate& pred, const ChainFrame& frame);

// Every conjunct attached to one step, each having passed CheckConjuncts
// against this frame. Empty always matches.
bool EvaluateAllChecked(const std::vector<const catalog::Schema*>& schemas,
                        const std::vector<StepPredicate>& predicates, const ChainFrame& frame);

// The scan kernel for one step (BM-R4, row_codec.hpp's `ScanKernel`): the
// conjuncts whose left side is a column of the step's own relation -
// `up == 0`, `rel_slot` the step's frame slot - and whose right side is a
// literal, each compiled if the codec holds its type. A correlated conjunct
// (a column on the right) is never a candidate, so the inner build's
// bucketing predicate is never in a kernel. Empty when nothing qualifies.
ScanKernel CompileScanKernel(const catalog::Schema& schema, const catalog::RowLayout& layout,
                             const std::vector<StepPredicate>& residual, std::uint16_t rel_slot);

// Every checked conjunct but the one at `skip` - the inner build's
// bucketing predicate (join-inner-build.md §2: the map holds every row
// passing the *non-correlated* residual). Together with the skipped
// conjunct this is exactly EvaluateAllChecked, which is what keeps
// emission untouched.
bool EvaluateAllExceptChecked(const std::vector<const catalog::Schema*>& schemas,
                              const std::vector<StepPredicate>& predicates,
                              const ChainFrame& frame, std::size_t skip);

// Every function conjunct attached to one step (step_chain.hpp's
// FunctionPredicate). Empty always matches. A NULL argument makes the call
// NULL, and a comparison with NULL is not true - the same collapse every
// other conjunct gets (null.md §4).
StatusOr<bool> EvaluateFunctionConjuncts(const std::vector<FunctionPredicate>& predicates,
                                         const ChainFrame& frame);

// ---- Three-valued logic, collapsed in exactly one place (spec I16) -------
enum class TriState : std::uint8_t { kFalse, kTrue, kUnknown };

// **The single collapse point.** UNKNOWN becomes false at the root of a
// conjunct - one function, which the sub-chain kinds (step_vm.cpp) and the
// boolean conjuncts (chain_frame.cpp) both route through.
inline bool Collapse(TriState value) noexcept { return value == TriState::kTrue; }

inline bool IsNull(const parser::AstValue& value) noexcept {
    return value.type == parser::ValueType::kNull;
}

// Every boolean conjunct attached to one step (step_chain.hpp's
// BoolPredicate), each in three-valued logic with its unknown collapsed to
// false at its root (BU-R4); its comparisons have passed CheckConjuncts
// against this frame. Empty always matches.
StatusOr<bool> EvaluateBoolConjuncts(const std::vector<const catalog::Schema*>& schemas,
                                     const std::vector<BoolPredicate>& predicates,
                                     const ChainFrame& frame);

}  // namespace kds::exec
