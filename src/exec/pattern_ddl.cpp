#include "kds/exec/pattern_ddl.hpp"

#include <algorithm>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "kds/catalog/well_known.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/stats/pattern_defs.hpp"
#include "kds/stats/waystone_dir.hpp"

namespace kds::exec {

namespace {

using catalog::kTypeValBool;
using catalog::kTypeValChar;
using catalog::kTypeValInt16;
using catalog::kTypeValInt32;
using catalog::kTypeValInt64;
using catalog::kTypeValInt8;
using catalog::kTypeValUint64;
using catalog::kTypeValVarchar;

char FoldAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string Fold(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) out.push_back(FoldAscii(c));
    return out;
}

std::string At(std::uint32_t byte_offset) {
    return " (byte " + std::to_string(byte_offset) + ")";
}

// ---- Check 6's coercibility matrix ---------------------------------------
//
// Ratified from the spec's `[PROPOSED]` v1 baseline: numeric<->numeric and
// bool<->int coerce (warning), string<->numeric does not (error).
//
// Expressed as three categories rather than a pairwise table, because the
// baseline is really a statement about *families*: two spellings of a number
// convert, a number and a truth value convert, and text and a number do not.
// A table would have to answer int8-vs-uint64 and varchar-vs-char
// separately, and every entry is a chance to disagree with the sentence
// above.
enum class TypeFamily { kNumeric, kBool, kText, kUnknown };

TypeFamily FamilyOf(std::uint32_t type_val) noexcept {
    switch (type_val) {
        case kTypeValInt8:
        case kTypeValInt16:
        case kTypeValInt32:
        case kTypeValInt64:
        case kTypeValUint64: return TypeFamily::kNumeric;
        case kTypeValBool: return TypeFamily::kBool;
        case kTypeValVarchar:
        case kTypeValChar: return TypeFamily::kText;
        default: return TypeFamily::kUnknown;
    }
}

enum class Coercion { kExact, kWarn, kError };

Coercion CoercionBetween(std::uint32_t declared, std::uint32_t column) noexcept {
    if (declared == column) return Coercion::kExact;

    const TypeFamily a = FamilyOf(declared);
    const TypeFamily b = FamilyOf(column);
    // A type with no on-disk encoding cannot be reasoned about, and guessing
    // would be worse than saying so. float/decimal are refused as columns
    // (catalog::RowLayout::Build), so this is reachable only from a declared
    // type naming one - which is a comparison that could never evaluate.
    if (a == TypeFamily::kUnknown || b == TypeFamily::kUnknown) return Coercion::kError;

    // Text on one side and a value on the other. The comparison could never
    // evaluate - CompareValues answers false for a type mismatch - so the
    // pattern could never match its own intent, which is the definition of
    // an error rather than a cost.
    if ((a == TypeFamily::kText) != (b == TypeFamily::kText)) return Coercion::kError;

    // Same family, different width; or bool against an integer. Both work
    // and both convert on every execution.
    return Coercion::kWarn;
}

// ---- Walking a compiled chain for parameter occurrences -------------------

// One `$param` where it appears, with the type the catalog says the column
// on the other side of its predicate holds.
struct ParamOccurrence {
    std::string name;
    std::uint32_t step_id = 0;
    std::string column;    // the column's own name
    std::string relation;  // the plan's display name for its relation
    std::uint32_t column_type = 0;
};

// The chains enclosing the one being walked, innermost last. `up` in a
// ColumnRef counts outward from the back, which is exactly the de Bruijn
// level step_chain.hpp describes.
using ChainStack = std::vector<const std::vector<Step>*>;

Status CollectFrom(catalog::Catalog& catalog, const std::vector<Step>& steps, ChainStack& stack,
                   std::vector<ParamOccurrence>& out);

Status CollectFromSubChains(catalog::Catalog& catalog, const std::vector<SubChain>& subs,
                            ChainStack& stack, std::vector<ParamOccurrence>& out) {
    for (const SubChain& sub : subs) {
        if (Status s = CollectFrom(catalog, sub.steps, stack, out); !s.ok()) return s;
    }
    return Status::OK();
}

Status CollectFrom(catalog::Catalog& catalog, const std::vector<Step>& steps, ChainStack& stack,
                   std::vector<ParamOccurrence>& out) {
    stack.push_back(&steps);
    for (const Step& step : steps) {
        for (const StepPredicate& pred : step.residual) {
            if (pred.rhs.kind != OperandKind::kLiteral) continue;
            if (pred.rhs.literal.type != parser::ValueType::kParam) continue;
            // A conjunct equality propagation derived, not one the client
            // wrote. Skipped for the same reason `Step::key` is (below): a
            // check-6 line must name a predicate the client can find in
            // their text, and the derived occurrence's verdict is never
            // different - propagation only crosses an identical type
            // descriptor, so `CoercionBetween` answers the same both ways.
            if (pred.derived) continue;

            // The lhs is always a column (spec I10: no expression on the
            // left), so its catalog type is the parameter's context type.
            // Resolving it through the *compiled* reference rather than the
            // parsed name is the point: name resolution happened once, in
            // the compiler, and doing it a second time here is how the two
            // come to disagree about which relation a name belongs to.
            if (pred.lhs.up >= stack.size()) {
                return Status::Corruption("a compiled predicate references " +
                                          std::to_string(pred.lhs.up) +
                                          " chains outward from a stack of " +
                                          std::to_string(stack.size()));
            }
            const std::vector<Step>& owner = *stack[stack.size() - 1 - pred.lhs.up];
            if (pred.lhs.rel_slot >= owner.size()) {
                return Status::Corruption("a compiled predicate references relation slot " +
                                          std::to_string(pred.lhs.rel_slot) + " of a chain with " +
                                          std::to_string(owner.size()) + " step(s)");
            }
            const Step& lhs_step = owner[pred.lhs.rel_slot];

            auto access = catalog.InitTableAccess(lhs_step.rel_oid);
            if (!access.ok()) return access.status();
            const catalog::Schema& schema = access.value()->schema;
            if (pred.lhs.col_pos >= schema.columns.size()) {
                return Status::Corruption("a compiled predicate references column " +
                                          std::to_string(pred.lhs.col_pos) + " of a relation with " +
                                          std::to_string(schema.columns.size()) + " column(s)");
            }
            const catalog::SysColumnRow& col = schema.columns[pred.lhs.col_pos];

            ParamOccurrence occurrence;
            occurrence.name = pred.rhs.literal.param_name();
            occurrence.step_id = step.step_id;
            // Kept apart rather than joined into `relation.column`:
            // `Step::rel_name` is a *plan* display name and already carries
            // the alias ("account AS a"), so gluing a column onto it reads
            // as "account AS a.flag" - a qualifier that appears nowhere in
            // the statement the client wrote.
            occurrence.column = std::string(catalog::NameView(col.name));
            occurrence.relation = lhs_step.rel_name;
            occurrence.column_type = col.type_val;
            out.push_back(std::move(occurrence));
        }
        if (Status s = CollectFromSubChains(catalog, step.sub_chains, stack, out); !s.ok()) {
            return s;
        }
    }
    stack.pop_back();
    return Status::OK();
}

// Every `$param` occurrence in a compiled chain, with its context type.
//
// `Step::key` is deliberately not inspected: step_chain.hpp guarantees a
// lookup/probe key is *also* kept in `residual`, which is the same property
// that makes invariant 9's fall-through safe. Reading both would double
// every key parameter's warning.
StatusOr<std::vector<ParamOccurrence>> CollectParamOccurrences(catalog::Catalog& catalog,
                                                               const StepChain& chain) {
    std::vector<ParamOccurrence> out;
    ChainStack stack;
    // Hoisted sub-chains are uncorrelated by construction, so nothing in
    // them refers outward - but they are walked with the outer chain on the
    // stack anyway, because "nothing refers outward" is a property of the
    // chain, not a licence for the walk to be unable to follow one.
    stack.push_back(&chain.steps);
    if (Status s = CollectFromSubChains(catalog, chain.hoisted, stack, out); !s.ok()) return s;
    stack.pop_back();

    if (Status s = CollectFrom(catalog, chain.steps, stack, out); !s.ok()) return s;
    return out;
}

// ---- Options --------------------------------------------------------------

struct Options {
    bool pinned = true;  // spec section 4.1: a declaration is pinned by default
    std::uint8_t dir_depth = 1;
};

// `expected_instances = N` -> the directory depth that addresses N instances
// without growing, clamped to what a 64-bit key can use.
//
// The option exposes an instance *count* rather than a depth on purpose: an
// operator should not have to know the 2048 fanout, and a "hash_table_size"
// would wrongly suggest arbitrary granularity when the real knob is an
// integer in [1, 6].
std::uint8_t DepthForInstances(std::uint64_t expected) noexcept {
    std::uint8_t depth = 1;
    std::uint64_t covered = stats::kDirFanout;
    while (depth < catalog::kMaxPatternDirDepth && covered < expected) {
        covered *= stats::kDirFanout;
        ++depth;
    }
    return depth;
}

StatusOr<Options> ParseOptions(const std::vector<parser::PatternOption>& options) {
    Options out;
    for (const parser::PatternOption& option : options) {
        const std::string key = Fold(option.key);
        if (key == "pinned") {
            const std::string value = Fold(option.value);
            if (value == "on") {
                out.pinned = true;
            } else if (value == "off") {
                out.pinned = false;
            } else {
                return Status::InvalidArgument("check 11: option 'pinned' takes on or off, got '" +
                                                option.value + "'" + At(option.byte_offset));
            }
            continue;
        }
        if (key == "expected_instances") {
            std::uint64_t n = 0;
            if (option.value.empty()) {
                return Status::InvalidArgument("check 11: option 'expected_instances' takes an "
                                                "integer" + At(option.byte_offset));
            }
            for (char c : option.value) {
                if (c < '0' || c > '9') {
                    return Status::InvalidArgument(
                        "check 11: option 'expected_instances' takes an integer, got '" +
                        option.value + "'" + At(option.byte_offset));
                }
                // Saturating: the value is only used to pick a depth in
                // [1, 6], so anything past the maximum coverage means "as
                // deep as it goes" and overflowing to a small number would
                // silently mean the opposite.
                if (n > (~std::uint64_t{0}) / 10) {
                    n = ~std::uint64_t{0};
                    continue;
                }
                n = n * 10 + static_cast<std::uint64_t>(c - '0');
            }
            if (n == 0) {
                return Status::InvalidArgument("check 11: option 'expected_instances' must be at "
                                                "least 1" + At(option.byte_offset));
            }
            out.dir_depth = DepthForInstances(n);
            continue;
        }
        // Unknown keys are refused, never ignored: a silently dropped option
        // is a setting an operator believes is in effect.
        return Status::InvalidArgument("check 11: unknown option '" + option.key +
                                        "' (known: pinned, expected_instances)" +
                                        At(option.byte_offset));
    }
    return out;
}

}  // namespace

StatusOr<PatternDdlResult> CreatePattern(catalog::Catalog& catalog, storage::PageStore& store,
                                         wal::WalManager* wal,
                                          const parser::CreatePatternStmt& stmt) {
    PatternDdlResult result;

    // Check 1 (body parses) is the parser's, and it already ran: this
    // function is only reached with a CreatePatternStmt in hand.

    // ---- Check 3: the parameter list is well formed ----------------------
    //
    // Ahead of check 2 in code, though not in the spec's numbering, because
    // check 2 needs the declared set to compare against and building it is
    // where its duplicates surface. Neither can fail in a way the other
    // would have reported differently.
    std::unordered_map<std::string, std::uint32_t> declared;  // folded name -> type_val
    for (const parser::PatternParam& param : stmt.params) {
        const std::string folded = Fold(param.name);
        if (declared.count(folded) != 0) {
            return Status::InvalidArgument("check 3: parameter '$" + param.name +
                                            "' is declared more than once" +
                                            At(param.byte_offset));
        }
        auto type = catalog.ResolveTypeByName(param.type_name);
        if (!type.ok()) {
            // Resolved now, not deferred: an unknown type name means the
            // declared contract names nothing, and check 6 would have
            // nothing to check against.
            return Status::InvalidArgument("check 3: parameter '$" + param.name +
                                            "' has unknown type '" + param.type_name + "'" +
                                            At(param.byte_offset));
        }
        declared.emplace(folded, type.value().type_val);
    }

    // ---- Check 2: every `$ident` in the body is declared ------------------
    std::unordered_map<std::string, bool> used;
    for (const parser::ParamUse& use : stmt.param_uses) {
        const std::string folded = Fold(use.name);
        if (declared.count(folded) == 0) {
            return Status::InvalidArgument("check 2: '$" + use.name +
                                            "' is used in the body but not declared" +
                                            At(use.byte_offset));
        }
        used[folded] = true;
    }

    // ---- Check 4: every declared parameter is used ------------------------
    for (const parser::PatternParam& param : stmt.params) {
        if (used.count(Fold(param.name)) != 0) continue;
        // Unused today changes nothing, but it desynchronizes the declared
        // arity from the body's value-slot count - which is the number
        // stored as the definition's arity.
        return Status::InvalidArgument("check 4: parameter '$" + param.name +
                                        "' is declared but never used in the body" +
                                        At(param.byte_offset));
    }

    // ---- Check 5: the body compiles ---------------------------------------
    auto chain = Compile(catalog, *stmt.body);
    if (!chain.ok()) {
        // The compiler's own message and position, unchanged: it knows which
        // relation or column failed, and rewording it here would lose the
        // byte offset that makes it actionable.
        return chain.status().WithContext("check 5: the pattern body does not compile");
    }

    // ---- Check 7: the statement class is patternable ----------------------
    //
    // Before check 6, because an unclassifiable chain is not a body whose
    // parameter types are worth discussing.
    if (chain.value().klass == StatementClass::kUnclassified) {
        return Status::Unsupported("check 7: only SELECT-class statements can be declared as "
                                   "patterns in v1");
    }

    // ---- Check 6: implicit-conversion analysis ---------------------------
    auto occurrences = CollectParamOccurrences(catalog, chain.value());
    if (!occurrences.ok()) return occurrences.status();

    for (const ParamOccurrence& occurrence : occurrences.value()) {
        auto it = declared.find(Fold(occurrence.name));
        if (it == declared.end()) continue;  // check 2 already refused this

        const Coercion verdict = CoercionBetween(it->second, occurrence.column_type);
        if (verdict == Coercion::kExact) continue;

        auto declared_type = catalog.ResolveTypeByVal(it->second);
        auto column_type = catalog.ResolveTypeByVal(occurrence.column_type);
        const std::string declared_name =
            declared_type.ok() ? std::string(catalog::NameView(declared_type.value().name))
                               : "?";
        const std::string column_name =
            column_type.ok() ? std::string(catalog::NameView(column_type.value().name)) : "?";
        const std::string where = "$" + occurrence.name + " " + declared_name + " vs column " +
                                  occurrence.column + " " + column_name + " of " +
                                  occurrence.relation + " at step " +
                                  std::to_string(occurrence.step_id);

        if (verdict == Coercion::kError) {
            return Status::InvalidArgument("check 6: " + where +
                                            ": these types cannot be compared, so this predicate "
                                            "could never match");
        }
        // A conversion is a per-execution cost and a likely mistake, not an
        // invalid pattern. Every occurrence gets its own line: the declared
        // type is one contract and each predicate has to satisfy it
        // separately, so which one is wrong is exactly what the operator
        // needs to know.
        result.warnings.push_back(where + ": implicit conversion on every execution");
    }

    // ---- Check 8: replayability, as a warning ----------------------------
    if (!HasReplayableStep(chain.value())) {
        // Legal to declare. Being surprised later is not: a scan-class chain
        // has no step a trail may replace (invariant 9), so its waystone can
        // only ever prefetch.
        result.warnings.push_back(
            "this body has no lookup or probe step, so its trail can never replay - only prefetch");
    }

    // ---- Check 11: options ------------------------------------------------
    auto options = ParseOptions(stmt.options);
    if (!options.ok()) return options.status();

    // ---- Check 9: the name is unique --------------------------------------
    auto existing_name = stats::FindPatternDefByName(catalog, store, stmt.name);
    if (!existing_name.ok()) return existing_name.status();
    if (existing_name.value().has_value()) {
        return Status::InvalidArgument("check 9: a pattern named '" + stmt.name +
                                        "' already exists" + At(stmt.byte_offset));
    }

    // ---- Check 10: pattern_id reconciliation ------------------------------
    //
    // The **body** is fingerprinted, never the whole declaration: the
    // leading clauses are not part of the shape any live statement has, so
    // hashing them would guarantee the pattern matched nothing (spec 3.2).
    auto fingerprint = parser::FingerprintOf(stmt.body_text);
    if (!fingerprint.has_value()) {
        return Status::Unsupported("check 10: the pattern body has no fingerprint");
    }
    result.pattern_id = fingerprint->pattern_id;
    // The body's arity, per spec section 3.3: every value slot, whether it
    // came from a `$param` or an inline literal. Not the number of declared
    // parameters - a parameter written twice fills two slots.
    result.param_count = fingerprint->literal_count + fingerprint->param_count;

    const std::uint16_t flags = options.value().pinned ? catalog::kPatternPinned : 0;
    const std::uint8_t stmt_class = StoredStatementClass(chain.value().klass);

    auto row = catalog.GetSysPatternRow(result.pattern_id);
    if (row.ok() && row.value().origin == catalog::kOriginUser) {
        // Already declared, possibly under another name - which is what the
        // message has to say, or the operator sees "duplicate" and cannot
        // find the duplicate.
        auto other = stats::FindPatternDefByPatternId(catalog, store, result.pattern_id);
        const std::string named =
            other.ok() && other.value().has_value() ? " as '" + other.value()->name + "'" : "";
        return Status::InvalidArgument("check 10: this shape is already declared" + named);
    }

    if (row.ok()) {
        // **Adoption.** An auto-registered row for this exact shape exists;
        // upgrade it in place. The waystone root and depth are deliberately
        // untouched - the trails recorded under it are trails for the same
        // shape, and discarding a warm cache to honour a declaration would
        // make declaring a hot pattern a regression. An operator who wants a
        // deeper directory drops and re-creates, which is the only way to
        // pay the flush knowingly.
        if (Status s = catalog.SetPatternOrigin(result.pattern_id, catalog::kOriginUser, flags);
            !s.ok()) {
            return s;
        }
        result.adopted = true;
        result.dir_depth = row.value().dir_depth;
    } else if (row.status().code() == StatusCode::kNotFound) {
        auto registered =
            catalog.RegisterPattern(result.pattern_id, stmt_class, catalog::kOriginUser, flags);
        if (!registered.ok()) return registered.status();

        // The directory, pre-sized. This is what `expected_instances` buys:
        // growth is a cache flush (waystone_dir.hpp), so creating the
        // directory at the depth the pattern needs is the mitigation, not a
        // convenience. Interior levels stay lazy - only the root is
        // allocated here.
        auto root = stats::CreateDirPage(store);
        if (!root.ok()) return root.status();
        if (Status s = catalog.SetPatternWaystoneRoot(result.pattern_id, root.value(),
                                                       options.value().dir_depth);
            !s.ok()) {
            return s;
        }
        result.dir_depth = options.value().dir_depth;
    } else {
        return row.status();
    }

    // ---- The definition row ----------------------------------------------
    //
    // Last, so a failed check leaves no half-declared pattern behind. It is
    // not last enough to be atomic - there is no transaction to make it so,
    // and a failure here leaves a sys.patterns row with no name. That is the
    // same exposure every other DDL path in this engine has until the
    // transaction manager exists, and it is recorded rather than hidden.
    if (Status s = stats::InsertPatternDef(catalog, store, wal, result.pattern_id, stmt.name,
                                            stmt.source_text, result.param_count);
        !s.ok()) {
        return s;
    }

    // Check 12 (the fingerprint version is stamped) is RegisterPattern's, by
    // construction: it stamps the running build's version and takes no
    // parameter that could carry a different one.
    return result;
}

StatusOr<std::uint64_t> DropPattern(catalog::Catalog& catalog, storage::PageStore& store,
                                    wal::WalManager* wal,
                                    std::string_view name) {
    auto def = stats::FindPatternDefByName(catalog, store, name);
    if (!def.ok()) return def.status();
    if (!def.value().has_value()) {
        return Status::NotFound("no pattern named '" + std::string(name) + "'");
    }

    const std::uint64_t pattern_id = def.value()->pattern_id;
    if (Status s = stats::DeletePatternDef(catalog, store, wal, pattern_id); !s.ok()) return s;

    // The sys.patterns row goes with it. A pattern row with no definition is
    // exactly an auto-registered pattern, so leaving it would silently
    // convert the declaration into an observation - and the operator asked
    // for the shape to stop being declared, not to be adopted back.
    if (Status s = catalog.RetirePattern(pattern_id); !s.ok()) return s;

    return pattern_id;
}

}  // namespace kds::exec
