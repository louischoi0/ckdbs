#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/exec/chain_frame.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/parser/ast.hpp"

// The fold (docs/feat-aggregate.md §5, workplan AG03).
//
// ---- It is a consumer of the row stream, not a step -------------------
//
// AG1 places aggregation **outside the executor**. The dispatcher already
// owns a statement's `RowSink`; an aggregated statement's sink body becomes
// `Accumulate(frame)`, and `Finish(emit)` produces the output rows once the
// chain has run. The executor never learns that aggregation exists, which
// is what makes the compiled chain byte-identical to the same statement
// without the fold - and therefore what lets every property already proved
// of a chain hold for an aggregated statement with no new proof.
//
// The seam is deliberately the same one Waystone lives outside of, for the
// same reason: a second place that reasons about statement shape is a
// second answer to "what does this statement do".
//
// ---- Zero allocations per row -----------------------------------------
//
// Folding a row into a group that already exists must allocate nothing.
// The group key is encoded into a **reused scratch buffer**, probed
// heterogeneously so the lookup never materialises a `std::string`, and the
// item states are folded in place. Allocation happens exactly when a row
// founds a new group, which is bounded by `AggregateLimits::max_groups`.
//
// The no-GROUP-BY form skips the map entirely: one state row, folded in
// place, no key encoded and no hash computed.
//
// ---- Mergeable state is an invariant, not a feature (AG-M) -------------
//
// Every aggregate's running state supports `Merge`, such that folding a row
// stream in one pass and folding two disjoint partitions of it then merging
// give the same output rows. Nothing in v1 calls it. It exists because it
// is what lets `docs/crosscore.md`'s step pipeline ship *partial
// aggregates* - group count on the wire, not row count - without touching
// the step VM, and a v1 that quietly broke it would take that option away
// silently. `AVG`, when it lands, must be carried as a `(sum, count)` pair
// for the same reason.

namespace kds::exec {

// What a completed fold hands back: one output row, in the spec's item
// order. The span views the aggregator's own buffer and is valid only for
// the duration of the call - a consumer that keeps it copies it.
using AggregateSink = std::function<Status(std::span<const parser::AstValue>)>;

// AG11's caps. **A cap refuses, never truncates or spills** - the same
// discipline Cabin's caps state, and for the same reason: a truncated group
// set is a wrong answer with a right answer's shape.
//
// The defaults are spec §6's `[PROPOSED]` numbers. Nothing may depend on
// either value, only on the rule they enforce; AG07 makes them config keys.
struct AggregateLimits {
    std::size_t max_groups = 65536;
    std::size_t max_distinct = 1048576;
};

class Aggregator {
public:
    // `spec` and `labels` are **borrowed** and must outlive the aggregator.
    // Both live on the `StepChain` the statement is executing, which the
    // dispatcher holds for exactly that long.
    //
    // `labels` is the chain's `column_names` - the one place a name
    // survives compilation - and is used only to make an overflow error
    // name the aggregate that overflowed.
    static StatusOr<Aggregator> Create(const AggregateSpec& spec,
                                       std::span<const std::string> labels,
                                       AggregateLimits limits = {});

    // Folds one row of the chain's output. Called from the statement's row
    // sink, once per row the chain emits.
    Status Accumulate(const ChainFrame& frame);

    // Emits the output rows in **first-seen order** - the order the chain's
    // deterministic row stream founded each group (AG6). Hash-iteration
    // order would vary by seed and growth history, which the
    // deterministic-test rule forbids; the groups live in a vector and this
    // walks it, so the order is by construction rather than by sorting.
    Status Finish(const AggregateSink& emit);

    // How many groups the fold founded. The global form is always 1.
    std::size_t group_count() const noexcept { return groups_.size(); }

private:
    // One aggregate's running state.
    //
    // The three counters are not exclusive by kind on purpose: `COUNT`
    // needs only `count`, `SUM` needs `sum` and `has_value`, `MIN`/`MAX`
    // need `extreme` and `has_value`. Sharing one struct costs a few bytes
    // per item per group and keeps `Merge` a single switch instead of a
    // union to discriminate.
    struct ItemState {
        std::int64_t count = 0;

        // SUM's accumulator. int64 with checked addition (AG3): an
        // overflow is a statement error, never a wrapped number.
        std::int64_t sum = 0;

        // Whether any **non-NULL** argument was seen. This is what makes
        // §3.1's "NULL when the group has none" a property of the fold
        // rather than of the values: `SUM`/`MIN`/`MAX` emit NULL for a
        // group they never saw a value in, which is not the same as a
        // group whose values summed to zero.
        bool has_value = false;

        // MIN/MAX's running extreme, valid when `has_value`.
        parser::AstValue extreme;
    };

    struct Group {
        // The grouping values, kept so the output row can carry them
        // back. Founding a group allocates; folding into one does not.
        std::vector<parser::AstValue> keys;
        std::vector<ItemState> items;
    };

    // Heterogeneous lookup, which is the whole reason the index is keyed by
    // `std::string` and probed by `std::string_view`: without
    // `is_transparent` every probe would materialise a string, and the
    // per-row allocation this class exists to avoid would be back.
    struct KeyHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view v) const noexcept {
            return std::hash<std::string_view>{}(v);
        }
    };

    Aggregator(const AggregateSpec& spec, std::span<const std::string> labels,
               AggregateLimits limits)
        : spec_(&spec), labels_(labels), limits_(limits) {}

    // Appends one value to `key_scratch_` in the encoding both the group
    // key and DISTINCT use: a tag byte (null / int / str) then the value's
    // own bytes, length-prefixed for a string so `('a','bc')` and
    // `('ab','c')` cannot collide.
    Status EncodeValue(const parser::AstValue& value);

    // Folds one row into one group's item states.
    Status FoldInto(Group& group, const ChainFrame& frame);

    // The label an error message names an item by.
    std::string LabelOf(std::size_t item_index) const;

    const AggregateSpec* spec_ = nullptr;
    std::span<const std::string> labels_;
    AggregateLimits limits_;

    // First-seen order, by construction. `index_` maps an encoded key to a
    // position in here.
    std::vector<Group> groups_;
    std::unordered_map<std::string, std::size_t, KeyHash, std::equal_to<>> index_;

    // Reused across every row. `clear()` keeps the capacity, so the encode
    // allocates on the first row of a statement and never again.
    std::string key_scratch_;

    // Reused across every emitted row, for the same reason.
    std::vector<parser::AstValue> out_scratch_;

    // For each item, which group key it reads - meaningful only for a
    // non-aggregate item, which AG5 guarantees is a grouping column.
    // Resolved once in Create() rather than searched per group.
    std::vector<std::size_t> item_key_index_;
};

}  // namespace kds::exec
