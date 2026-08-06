#include "kds/exec/aggregate.hpp"

#include <cstring>
#include <utility>

#include "kds/exec/row_codec.hpp"

// The fold. See aggregate.hpp for why it lives outside the executor and
// what it is not allowed to allocate.

namespace kds::exec {

namespace {

// Tags for the value encoding shared by group keys and DISTINCT sets.
//
// The tag is what makes NULL a *group* rather than a comparison: two NULL
// keys land together because their encodings are the same bytes, not
// because NULL equals NULL. `CompareValues`' "NULL never matches" is
// untouched, and deliberately so - predicates compare, grouping encodes
// identity, and collapsing the two would change what a WHERE means.
constexpr char kTagNull = '\0';
constexpr char kTagInt = '\1';
constexpr char kTagStr = '\2';

// Renders one group's key values for an error message.
std::string DescribeGroup(const std::vector<parser::AstValue>& keys) {
    if (keys.empty()) return {};
    std::string out = " for group (";
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i != 0) out += ", ";
        out += FormatValue(keys[i]);
    }
    out += ')';
    return out;
}

}  // namespace

StatusOr<Aggregator> Aggregator::Create(const AggregateSpec& spec,
                                        std::span<const std::string> labels,
                                        AggregateLimits limits) {
    Aggregator agg(spec, labels, limits);

    // A non-aggregate item is a grouping column carried into the output,
    // which AG5 already enforced at compile. Resolving *which* key it is
    // once here rather than searching per group is the same trade the
    // compiler makes everywhere else: answer it where the answer is known.
    agg.item_key_index_.assign(spec.items.size(), 0);
    for (std::size_t i = 0; i < spec.items.size(); ++i) {
        const AggregateItem& item = spec.items[i];
        if (item.is_aggregate) continue;

        bool found = false;
        for (std::size_t k = 0; k < spec.group_keys.size(); ++k) {
            if (spec.group_keys[k] != item.ref) continue;
            agg.item_key_index_[i] = k;
            found = true;
            break;
        }
        if (!found) {
            // Unreachable through the compiler, and checked anyway: a spec
            // can be built by something other than a compile, and a bound
            // only one producer enforces is not a bound.
            return Status::InvalidArgument(
                "aggregate spec selects a column that is not a grouping key");
        }
    }

    // **The global form has its group before any row arrives.** §3.1 asks
    // for exactly one output row even over empty input - COUNT 0, SUM/MIN/
    // MAX NULL - and that is a different shape from the grouped form, which
    // emits zero rows over empty input. Founding it here is what makes the
    // difference structural instead of a special case in Finish().
    if (spec.group_keys.empty()) {
        agg.groups_.push_back(agg.NewGroup());
    }
    return agg;
}

bool Aggregator::NeedsDistinct(const AggregateItem& item) noexcept {
    if (!item.distinct) return false;
    // `MIN`/`MAX` accept the word and ignore it (spec §3.2): an extreme of
    // a set equals the extreme of its support, so a set here would spend
    // memory and a cap budget to reach an identical answer. `COUNT(*)`
    // cannot carry it at all - the parser refuses `COUNT(DISTINCT *)`.
    return item.func == parser::AggFunc::kCount || item.func == parser::AggFunc::kSum;
}

Status Aggregator::EncodeValue(const parser::AstValue& value, std::string& out) {
    switch (value.type) {
        case parser::ValueType::kNull:
            out.push_back(kTagNull);
            return Status::OK();

        case parser::ValueType::kInt: {
            // `int_val`'s bits, not its decimal text. The mapping from a
            // stored integer to those bits is a bijection for every type
            // the engine has - a uint64 above INT64_MAX wraps to a distinct
            // bit pattern, not a colliding one - so this is identity
            // without a string conversion per value per row.
            out.push_back(kTagInt);
            char bytes[sizeof(std::int64_t)];
            std::memcpy(bytes, &value.int_val, sizeof(bytes));
            out.append(bytes, sizeof(bytes));
            return Status::OK();
        }

        case parser::ValueType::kStr: {
            out.push_back(kTagStr);
            const std::uint32_t len = static_cast<std::uint32_t>(value.str_val.size());
            char bytes[sizeof(len)];
            std::memcpy(bytes, &len, sizeof(bytes));
            // Length-prefixed, so ('a','bc') and ('ab','c') cannot encode
            // to the same bytes and read as one group.
            out.append(bytes, sizeof(bytes));
            out.append(value.str_val);
            return Status::OK();
        }

        case parser::ValueType::kParam:
            // A declared pattern's `$name`. A chain compiled from a pattern
            // body exists to be type-checked and fingerprinted, never run,
            // so reaching a fold with one means something executed a body -
            // refused explicitly here as it is on every other consuming
            // path (row_codec.cpp, step_vm.cpp).
            return Status::InvalidArgument(
                "a pattern parameter '$" + value.param_name() +
                "' has no value; a declared pattern's body is never executed");
    }
    return Status::InvalidArgument("unknown value kind in a grouping key");
}

Aggregator::Group Aggregator::NewGroup() const {
    Group group;
    group.items.resize(spec_->items.size());
    // A DISTINCT set exists per `(group, item)` and only for an item that
    // declared the word - so a statement without DISTINCT allocates none of
    // these, which is what "pays nothing for the feature" has to mean to be
    // worth saying.
    for (std::size_t i = 0; i < spec_->items.size(); ++i) {
        if (!NeedsDistinct(spec_->items[i])) continue;
        group.items[i].distinct =
            std::make_unique<std::unordered_set<std::string, KeyHash, std::equal_to<>>>();
    }
    return group;
}

Status Aggregator::FoldInto(Group& group, const ChainFrame& frame) {
    for (std::size_t i = 0; i < spec_->items.size(); ++i) {
        const AggregateItem& item = spec_->items[i];
        ItemState& state = group.items[i];

        // A grouping column carried into the output: its value is the
        // group's, already stored when the group was founded, and folding
        // it again would be writing the same bytes over themselves.
        if (!item.is_aggregate) continue;

        // COUNT(*) counts rows and never looks at a value, which is why it
        // is the one aggregate that can never answer NULL.
        if (item.star_arg) {
            ++state.count;
            continue;
        }

        if (!frame.CanResolve(item.ref)) {
            return Status::InvalidArgument("aggregate reads a column the frame cannot resolve");
        }
        const parser::AstValue& value = frame.Get(item.ref);

        // §3.1: every aggregate skips NULLs. `COUNT(col)` counts the rows
        // whose value is not NULL; SUM/MIN/MAX fold over the non-NULL
        // values and answer NULL for a group that had none.
        if (value.type == parser::ValueType::kNull) continue;

        // ---- DISTINCT (§3.2) -----------------------------------------
        //
        // **Before the fold, and after the NULL test.** A repeated value is
        // dropped here and reaches no counter, which is the whole of what
        // the word means; and a NULL was already skipped, so no set ever
        // holds one and `COUNT(DISTINCT col)` cannot count it.
        if (state.distinct != nullptr) {
            value_scratch_.clear();
            if (Status s = EncodeValue(value, value_scratch_); !s.ok()) return s;

            // A hit allocates nothing: the set is probed with a
            // `std::string` that is already there, and only a miss builds
            // a node.
            if (state.distinct->find(value_scratch_) != state.distinct->end()) continue;

            if (distinct_entries_ >= limits_.max_distinct) {
                return Status::ResourceExhausted(
                    "this statement exceeded aggregate_max_distinct (" +
                    std::to_string(limits_.max_distinct) + ") in " + LabelOf(i) +
                    "; no partial answer is emitted, because a truncated distinct set counts "
                    "too few and is a wrong answer with a right answer's shape");
            }
            state.distinct->insert(value_scratch_);
            ++distinct_entries_;
        }

        switch (item.func) {
            case parser::AggFunc::kCount:
                ++state.count;
                state.has_value = true;
                break;

            case parser::AggFunc::kSum: {
                // AG3 restricted the argument to a signed integer column at
                // compile, so `int_val` *is* the value - no conversion, and
                // no digit-text path, which is exactly why `uint64` is
                // refused up there rather than approximated down here.
                if (value.type != parser::ValueType::kInt) {
                    return Status::InvalidArgument("SUM read a non-integer value in " +
                                                    LabelOf(i));
                }
                // Checked, always. A wrapped sum is the one output this
                // feature must never produce: it is wrong in a way no
                // reader can detect, which is the same argument the trail
                // trust model rests on.
                if (__builtin_add_overflow(state.sum, value.int_val, &state.sum)) {
                    return Status::OutOfRange("SUM overflow in " + LabelOf(i) +
                                              DescribeGroup(group.keys) +
                                              "; the accumulator is int64 and a wrapped sum is "
                                              "wrong in a way no reader can detect");
                }
                state.has_value = true;
                break;
            }

            case parser::AggFunc::kMin:
            case parser::AggFunc::kMax: {
                if (!state.has_value) {
                    state.extreme = value;
                    state.has_value = true;
                    break;
                }
                // Through `CompareValues` with the item's own `type_val`,
                // which is what makes MIN/MAX over a uint64 above
                // INT64_MAX exact: that path compares digit text rather
                // than a signed reading that cannot hold the value.
                const parser::CompareOp op = item.func == parser::AggFunc::kMin
                                                 ? parser::CompareOp::kLt
                                                 : parser::CompareOp::kGt;
                if (CompareValues(item.type_val, value, state.extreme, op)) {
                    state.extreme = value;
                }
                break;
            }
        }
    }
    return Status::OK();
}

Status Aggregator::Accumulate(const ChainFrame& frame) {
    // ---- The global form: no key, no hash, no map --------------------
    if (spec_->group_keys.empty()) {
        return FoldInto(groups_[0], frame);
    }

    key_scratch_.clear();
    for (const ColumnRef& key : spec_->group_keys) {
        if (!frame.CanResolve(key)) {
            return Status::InvalidArgument("GROUP BY reads a column the frame cannot resolve");
        }
        if (Status s = EncodeValue(frame.Get(key), key_scratch_); !s.ok()) return s;
    }

    // Heterogeneous, so the probe never materialises a string. This is the
    // one lookup on the per-row path and it must stay allocation-free.
    if (auto it = index_.find(std::string_view(key_scratch_)); it != index_.end()) {
        return FoldInto(groups_[it->second], frame);
    }

    // ---- A new group. The only place this class allocates. ------------
    //
    // A cap **refuses the statement**; it never truncates the group set and
    // never spills. A truncated set is a wrong answer with a right answer's
    // shape, which is exactly the failure Cabin's caps refuse - and the
    // error names the key so an operator knows which number to raise.
    if (groups_.size() >= limits_.max_groups) {
        return Status::ResourceExhausted(
            "this statement exceeded aggregate_max_groups (" +
            std::to_string(limits_.max_groups) +
            "); no partial answer is emitted, because a truncated group set is a wrong answer "
            "with a right answer's shape");
    }

    Group group = NewGroup();
    group.keys.reserve(spec_->group_keys.size());
    for (const ColumnRef& key : spec_->group_keys) group.keys.push_back(frame.Get(key));

    const std::size_t at = groups_.size();
    groups_.push_back(std::move(group));
    index_.emplace(key_scratch_, at);
    return FoldInto(groups_[at], frame);
}

Status Aggregator::Finish(const AggregateSink& emit) {
    for (const Group& group : groups_) {
        out_scratch_.clear();
        out_scratch_.reserve(spec_->items.size());

        for (std::size_t i = 0; i < spec_->items.size(); ++i) {
            const AggregateItem& item = spec_->items[i];
            const ItemState& state = group.items[i];

            if (!item.is_aggregate) {
                out_scratch_.push_back(group.keys[item_key_index_[i]]);
                continue;
            }

            parser::AstValue out;
            switch (item.func) {
                case parser::AggFunc::kCount:
                    // Never NULL, in either form: counting rows or counting
                    // non-NULL values both answer a number, and zero is an
                    // answer rather than an absence.
                    out.type = parser::ValueType::kInt;
                    out.int_val = state.count;
                    break;

                case parser::AggFunc::kSum:
                    if (!state.has_value) break;  // stays kNull
                    out.type = parser::ValueType::kInt;
                    out.int_val = state.sum;
                    break;

                case parser::AggFunc::kMin:
                case parser::AggFunc::kMax:
                    if (!state.has_value) break;  // stays kNull
                    out = state.extreme;
                    break;
            }
            out_scratch_.push_back(std::move(out));
        }

        if (Status s = emit(out_scratch_); !s.ok()) return s;
    }
    return Status::OK();
}

std::string Aggregator::LabelOf(std::size_t item_index) const {
    if (item_index < labels_.size()) return labels_[item_index];
    return "aggregate #" + std::to_string(item_index);
}

}  // namespace kds::exec
