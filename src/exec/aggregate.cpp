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
constexpr char kTagDecimal = '\3';
constexpr char kTagDecimalWide = '\4';

// Renders one group's key values for an error message.
std::string DescribeGroup(const std::vector<parser::AstValue>& keys) {
    if (keys.empty()) return {};
    std::string out = " for group (";
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i != 0) out += ", ";
        // type_val 0: a group label in an error message, where the
        // keys are whatever they are and no column type is at hand.
        out += FormatValue(/*type_val=*/0, keys[i]);
    }
    out += ')';
    return out;
}

}  // namespace

StatusOr<Aggregator> Aggregator::Create(const AggregateSpec& spec,
                                        std::span<const std::string> labels,
                                        AggregateLimits limits) {
    Aggregator agg;
    if (Status s = agg.Reset(spec, labels, limits); !s.ok()) return s;
    return agg;
}

Status Aggregator::Reset(const AggregateSpec& spec, std::span<const std::string> labels,
                         AggregateLimits limits) {
    spec_ = &spec;
    labels_ = labels;
    limits_ = limits;

    // `clear()` throughout, never a fresh container: every one of these
    // keeps its capacity, which is the whole point of reusing the object.
    groups_.clear();
    index_.clear();
    key_scratch_.clear();
    out_scratch_.clear();
    distinct_entries_ = 0;

    Aggregator& agg = *this;

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
    return Status::OK();
}

bool Aggregator::NeedsDistinct(const AggregateItem& item) noexcept {
    if (!item.distinct) return false;
    // `MIN`/`MAX` accept the word and ignore it (spec §3.2): an extreme of
    // a set equals the extreme of its support, so a set here would spend
    // memory and a cap budget to reach an identical answer. `COUNT(*)`
    // cannot carry it at all - the parser refuses `COUNT(DISTINCT *)`.
    // `AVG(DISTINCT)` is `SUM(DISTINCT) / COUNT(DISTINCT)` over **one**
    // set, which is exactly what one set on the pair state provides.
    return item.func == parser::AggFunc::kCount || item.func == parser::AggFunc::kSum ||
           item.func == parser::AggFunc::kAvg;
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

        case parser::ValueType::kDecimal: {
            // Its own tag, and the scale encoded with it. A decimal shares
            // `int_val` with an integer but is not one: grouping them
            // together would merge a `decimal(10,2)` 12.34 with a plain
            // 1234, which the tag byte exists to prevent - the same reason
            // NULL and the empty string have different tags.
            out.push_back(kTagDecimal);
            out.push_back(static_cast<char>(value.scale));
            char bytes[sizeof(std::int64_t)];
            std::memcpy(bytes, &value.int_val, sizeof(bytes));
            out.append(bytes, sizeof(bytes));
            return Status::OK();
        }

        case parser::ValueType::kDecimalWide: {
            // The 16-byte kind: tag, scale, then both halves, low first -
            // its own tag for the same reason the narrow decimal has one,
            // and 16 value bytes so two wide values can never collide with
            // anything narrower.
            out.push_back(kTagDecimalWide);
            out.push_back(static_cast<char>(value.scale));
            char bytes[2 * sizeof(std::int64_t)];
            std::memcpy(bytes, &value.int_val, sizeof(std::int64_t));
            std::memcpy(bytes + sizeof(std::int64_t), &value.dec_hi, sizeof(std::int64_t));
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

            case parser::AggFunc::kSum:
            // AVG is the `(sum, count)` pair AG-M reserved the shape for:
            // it folds exactly as SUM does, counts beside it, and the
            // divide happens once, in `Finish`. One arm for both keeps the
            // overflow discipline in one place.
            case parser::AggFunc::kAvg: {
                // AG3 restricted the argument to a signed integer column at
                // compile, so `int_val` *is* the value - no conversion, and
                // no digit-text path, which is exactly why `uint64` is
                // refused up there rather than approximated down here.
                //
                // A `DECIMAL` joins on the same terms and needs no arm of
                // its own: its `int_val` is the unscaled integer, every row
                // of one column carries the same scale, and so the sum of
                // the unscaled values is the unscaled sum (types.md
                // §3.2). The scale is re-attached once, in `Finish`. The
                // wide decimal folds beside them into its own int128
                // accumulator - the int64 one is a product contract and
                // does not widen.
                if (value.type == parser::ValueType::kDecimalWide) {
                    if (__builtin_add_overflow(state.sum_wide,
                                               Int128FromHalves(value.dec_hi, value.int_val),
                                               &state.sum_wide)) {
                        return Status::OutOfRange(
                            "SUM overflow in " + LabelOf(i) + DescribeGroup(group.keys) +
                            "; the accumulator is int128 and a wrapped sum is wrong in a way "
                            "no reader can detect");
                    }
                    if (item.func == parser::AggFunc::kAvg) ++state.count;
                    state.has_value = true;
                    break;
                }
                if (value.type != parser::ValueType::kInt &&
                    value.type != parser::ValueType::kDecimal) {
                    return Status::InvalidArgument(LabelOf(i) + " read a non-numeric value");
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
                if (item.func == parser::AggFunc::kAvg) ++state.count;
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
                    // The scale is re-attached here and nowhere else: the
                    // accumulator holds unscaled integers throughout, so
                    // this is the one point where the answer stops being a
                    // running total and becomes a decimal again.
                    if (item.type_val == catalog::kTypeValDecimalWide) {
                        out.type = parser::ValueType::kDecimalWide;
                        out.scale = item.scale;
                        out.int_val = Int128Low(state.sum_wide);
                        out.dec_hi = Int128High(state.sum_wide);
                        break;
                    }
                    if (item.type_val == catalog::kTypeValDecimal) {
                        out.type = parser::ValueType::kDecimal;
                        out.scale = item.scale;
                    } else {
                        out.type = parser::ValueType::kInt;
                    }
                    out.int_val = state.sum;
                    break;

                case parser::AggFunc::kMin:
                case parser::AggFunc::kMax:
                    if (!state.has_value) break;  // stays kNull
                    out = state.extreme;
                    break;

                case parser::AggFunc::kAvg: {
                    if (!state.has_value) break;  // stays kNull
                    // The one divide (aggregate.md §3.4): the exact
                    // quotient of the unscaled sum by the count, **rounded
                    // half to even at the column's own scale** - ties go to
                    // the even neighbor, which is sign-symmetric and
                    // bias-free under accumulation, and the reason it can
                    // be computed exactly here is that both operands are
                    // integers: no float touches the value at any point.
                    //
                    // Division truncates toward zero and the remainder
                    // carries the dividend's sign, so the comparison runs
                    // on magnitudes and the correction is applied in the
                    // sum's direction. `q` cannot overflow on correction: a
                    // nonzero remainder needs count >= 2, which bounds |q|
                    // at half the range. One body, both widths - the
                    // arithmetic is identical, only the register is wider.
                    const std::int64_t count = state.count;  // > 0: has_value
                    const auto divide = [count](auto sum) {
                        using Acc = decltype(sum);
                        // The magnitude math runs unsigned and 128 bits
                        // wide for both widths - |r| < count fits either
                        // way, and one width of scratch is one case fewer.
                        using UAcc = unsigned __int128;
                        Acc q = sum / count;
                        const Acc r = sum % count;
                        const UAcc twice_r = 2 * static_cast<UAcc>(r < 0 ? -r : r);
                        const UAcc ucount = static_cast<UAcc>(count);
                        if (twice_r > ucount || (twice_r == ucount && (q % 2) != 0)) {
                            q += sum < 0 ? -1 : 1;
                        }
                        return q;
                    };
                    if (item.type_val == catalog::kTypeValDecimalWide) {
                        const Int128 q = divide(state.sum_wide);
                        out.type = parser::ValueType::kDecimalWide;
                        out.int_val = Int128Low(q);
                        out.dec_hi = Int128High(q);
                    } else {
                        out.type = parser::ValueType::kDecimal;
                        out.int_val = divide(state.sum);
                    }
                    out.scale = item.scale;
                    break;
                }
            }
            out_scratch_.push_back(std::move(out));
        }

        if (Status s = emit(out_scratch_); !s.ok()) return s;
    }
    return Status::OK();
}

// ---- AG-M: the merge -----------------------------------------------------

namespace {

// Reads an int back out of the value encoding. Exact by construction: the
// encoding stores `int_val`'s bits, so this is the inverse of writing them
// and not a re-parse.
//
// Only `SUM(DISTINCT)` needs it, and only at merge time - the fold itself
// never decodes, because the value is in hand when it inserts. That is why
// the set stores encodings rather than encodings *and* values: the 8 bytes
// per distinct entry a value copy would cost buys nothing on the hot path.
bool DecodeInt(std::string_view encoded, std::int64_t& out) {
    if (encoded.size() == 1 + sizeof(std::int64_t) && encoded[0] == kTagInt) {
        std::memcpy(&out, encoded.data() + 1, sizeof(out));
        return true;
    }
    // A decimal entry: tag, scale byte, then the unscaled integer - which
    // is exactly what the accumulator folds, so the scale byte is skipped
    // rather than interpreted. This arm predates nothing: SUM(DISTINCT)
    // over a decimal column always stored these entries, and a merge of one
    // failed as "non-integer" until AVG's tests forced the question.
    if (encoded.size() == 2 + sizeof(std::int64_t) && encoded[0] == kTagDecimal) {
        std::memcpy(&out, encoded.data() + 2, sizeof(out));
        return true;
    }
    return false;
}

// The wide entry: tag, scale byte, then both halves, low first - the
// int128 the wide accumulator folds.
bool DecodeInt128(std::string_view encoded, Int128& out) {
    if (encoded.size() != 2 + 2 * sizeof(std::int64_t) || encoded[0] != kTagDecimalWide) {
        return false;
    }
    std::int64_t lo = 0;
    std::int64_t hi = 0;
    std::memcpy(&lo, encoded.data() + 2, sizeof(lo));
    std::memcpy(&hi, encoded.data() + 2 + sizeof(lo), sizeof(hi));
    out = Int128FromHalves(hi, lo);
    return true;
}

}  // namespace

Status Aggregator::MergeGroup(Group& into, Group& from) {
    for (std::size_t i = 0; i < spec_->items.size(); ++i) {
        const AggregateItem& item = spec_->items[i];
        ItemState& dst = into.items[i];
        ItemState& src = from.items[i];

        if (!item.is_aggregate) continue;

        // ---- DISTINCT: union, and only the newcomers contribute -------
        //
        // The counters cannot simply be added for a distinct item: a value
        // present in both partitions was counted once on each side, and
        // adding would count it twice. So the union decides, and each entry
        // that is genuinely new adds its own contribution.
        if (dst.distinct != nullptr) {
            if (src.distinct == nullptr) {
                return Status::InvalidArgument(
                    "cannot merge a DISTINCT item against one that kept no set");
            }
            for (const std::string& entry : *src.distinct) {
                if (dst.distinct->find(entry) != dst.distinct->end()) continue;
                if (distinct_entries_ >= limits_.max_distinct) {
                    return Status::ResourceExhausted(
                        "merging exceeded aggregate_max_distinct (" +
                        std::to_string(limits_.max_distinct) + ") in " + LabelOf(i));
                }
                dst.distinct->insert(entry);
                ++distinct_entries_;

                if (item.func == parser::AggFunc::kCount) {
                    ++dst.count;
                } else {
                    // SUM(DISTINCT) and AVG(DISTINCT) both fold the
                    // newcomer's value; AVG counts it too, because its
                    // divisor is the union's size and the union is being
                    // built right here. The entry's own tag says which
                    // accumulator it belongs to, exactly as the fold's
                    // value kind did.
                    std::int64_t value = 0;
                    Int128 wide = 0;
                    if (DecodeInt(entry, value)) {
                        if (__builtin_add_overflow(dst.sum, value, &dst.sum)) {
                            return Status::OutOfRange("SUM overflow in " + LabelOf(i) +
                                                      DescribeGroup(into.keys) +
                                                      " while merging");
                        }
                    } else if (DecodeInt128(entry, wide)) {
                        if (__builtin_add_overflow(dst.sum_wide, wide, &dst.sum_wide)) {
                            return Status::OutOfRange("SUM overflow in " + LabelOf(i) +
                                                      DescribeGroup(into.keys) +
                                                      " while merging");
                        }
                    } else {
                        return Status::InvalidArgument(
                            "a DISTINCT merge met a non-integer value in " + LabelOf(i));
                    }
                    if (item.func == parser::AggFunc::kAvg) ++dst.count;
                }
                dst.has_value = true;
            }
            continue;
        }

        switch (item.func) {
            case parser::AggFunc::kCount:
                dst.count += src.count;
                break;

            // The pair state is why AVG merges at all (AG-M): two partial
            // sums add, two partial counts add, and the divide waits for
            // `Finish` - where merging two partial *quotients* would have
            // been unrecoverable rounding.
            case parser::AggFunc::kAvg:
                dst.count += src.count;
                [[fallthrough]];
            case parser::AggFunc::kSum:
                if (__builtin_add_overflow(dst.sum, src.sum, &dst.sum) ||
                    __builtin_add_overflow(dst.sum_wide, src.sum_wide, &dst.sum_wide)) {
                    // Both accumulators merge unconditionally - the one the
                    // item does not use is adding zeros - so this line does
                    // not need to know the item's width.
                    return Status::OutOfRange("SUM overflow in " + LabelOf(i) +
                                              DescribeGroup(into.keys) + " while merging");
                }
                break;

            case parser::AggFunc::kMin:
            case parser::AggFunc::kMax: {
                if (!src.has_value) break;
                if (!dst.has_value) {
                    dst.extreme = std::move(src.extreme);
                    break;
                }
                const parser::CompareOp op = item.func == parser::AggFunc::kMin
                                                 ? parser::CompareOp::kLt
                                                 : parser::CompareOp::kGt;
                if (CompareValues(item.type_val, src.extreme, dst.extreme, op)) {
                    dst.extreme = std::move(src.extreme);
                }
                break;
            }
        }
        dst.has_value = dst.has_value || src.has_value;
    }
    return Status::OK();
}

Status Aggregator::Merge(Aggregator&& other) {
    if (spec_->items.size() != other.spec_->items.size() ||
        spec_->group_keys.size() != other.spec_->group_keys.size()) {
        return Status::InvalidArgument(
            "cannot merge two folds of different shapes; a merge is only defined over "
            "partitions of one statement's rows");
    }

    // **In `other`'s own group order**, which is what makes the merged
    // result's order defined: this side keeps its groups where they are and
    // the far side's newcomers arrive behind them, in the order that side
    // founded them. Iterating the far side's *index* instead would append in
    // hash order and make the output depend on a seed.
    for (Group& incoming : other.groups_) {
        std::size_t at = 0;
        if (!spec_->group_keys.empty()) {
            // Re-encoded from the group's own values rather than carried
            // across, so a merge depends on nothing but the two groups.
            key_scratch_.clear();
            for (const parser::AstValue& value : incoming.keys) {
                if (Status s = EncodeValue(value, key_scratch_); !s.ok()) return s;
            }

            if (auto it = index_.find(std::string_view(key_scratch_)); it != index_.end()) {
                at = it->second;
            } else {
                if (groups_.size() >= limits_.max_groups) {
                    return Status::ResourceExhausted(
                        "merging exceeded aggregate_max_groups (" +
                        std::to_string(limits_.max_groups) + ")");
                }
                at = groups_.size();
                Group fresh = NewGroup();
                fresh.keys = incoming.keys;
                groups_.push_back(std::move(fresh));
                index_.emplace(key_scratch_, at);
            }
        }
        if (Status s = MergeGroup(groups_[at], incoming); !s.ok()) return s;
    }

    // Left empty rather than merely moved-from: an aggregator whose groups
    // have been folded elsewhere must not be finishable, and an empty one
    // that is asked to finish emits nothing instead of a duplicate.
    other.groups_.clear();
    other.index_.clear();
    other.distinct_entries_ = 0;
    return Status::OK();
}

std::string Aggregator::LabelOf(std::size_t item_index) const {
    if (item_index < labels_.size()) return labels_[item_index];
    return "aggregate #" + std::to_string(item_index);
}

}  // namespace kds::exec
