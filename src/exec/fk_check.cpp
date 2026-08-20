#include "kds/exec/fk_check.hpp"


#include <optional>
#include <unordered_set>
#include <vector>

#include "kds/exec/row_codec.hpp"
#include "kds/exec/tuple_verify.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/visit.hpp"
#include "kds/txn/visibility.hpp"

namespace kds::exec {

namespace {

FkVerdict VerdictOf(txn::CheckVerdict verdict) noexcept {
    switch (verdict) {
        case txn::CheckVerdict::kLive:
            return FkVerdict::kPass;
        case txn::CheckVerdict::kBusy:
            return FkVerdict::kBusy;
        case txn::CheckVerdict::kAbsent:
            break;
    }
    return FkVerdict::kViolation;
}

// The value a Cabin on the child's fk column is keyed by, for this parent.
// A pk is an integer by invariant 11, so this never has to consider the
// other value kinds.
parser::AstValue PkValue(std::uint64_t pk) {
    parser::AstValue value;
    value.type = parser::ValueType::kInt;
    value.int_val = static_cast<std::int64_t>(pk);
    return value;
}

// Reads the foreign-key column of one tuple as an id.
//
// Returns nullopt for a column that is not an integer in this row - which
// since NULL storage includes a stored NULL, decoded as kNull by the bitmap.
// Either way it is a row that cannot match an id whatever else is true of
// it, so a NULL child never blocks its parent's delete: MATCH SIMPLE's
// vacuous direction, falling out of the same bail. **No spill is resolved**:
// the fk column is an integer and therefore always inline, so this makes no
// page fetch and is safe under the walk's own page span.
StatusOr<std::optional<std::uint64_t>> ForeignKeyValue(const catalog::TableAccess& child,
                                                       std::uint16_t column_no,
                                                       std::span<const std::byte> payload,
                                                       std::vector<parser::AstValue>& scratch) {
    if (scratch.size() != child.schema.columns.size()) {
        scratch.assign(child.schema.columns.size(), parser::AstValue{});
    }
    std::vector<PendingSpill> spills;
    if (Status s = DecodeRowInto(child.schema, child.layout, payload, scratch, &spills); !s.ok()) {
        return s;
    }
    if (column_no >= scratch.size()) {
        return Status::Corruption("a foreign key names column " + std::to_string(column_no) +
                                   " of a relation with " + std::to_string(scratch.size()) +
                                   " column(s)");
    }
    const parser::AstValue& value = scratch[column_no];
    if (value.type != parser::ValueType::kInt || value.int_val < 0) {
        return std::optional<std::uint64_t>{};
    }
    return std::optional<std::uint64_t>{static_cast<std::uint64_t>(value.int_val)};
}

}  // namespace

StatusOr<FkVerdict> CheckParentPresent(storage::PageStore& store,
                                       const catalog::TableAccess& parent,
                                       std::uint64_t parent_pk,
                                       const txn::ReadView& check_view, Budget* budget) {
    if (budget != nullptr) {
        if (Status s = budget->ChargeRow(); !s.ok()) return s;
    }

    if (parent.clustered_type == catalog::ClusteredType::kBtree) {
        // The descent is authoritative in both directions on a clustered
        // relation: a hit is where the row lives, and a miss means there is
        // no such row - the same property that lets a point SELECT skip the
        // scan, and the reason the declaration refuses a heap parent.
        auto found = btree::BtreeLookup(store, parent.desc_page_id, parent_pk);
        if (!found.ok()) {
            if (found.status().code() == StatusCode::kNotFound) return FkVerdict::kViolation;
            return found.status();
        }
        // Re-fetched by id rather than carried out of the lookup: the span
        // Location used to carry outlived the descent's pin
        // (workplan-pageref.md Shape C), where this fetch is a hash hit on
        // a still-resident frame and the ref holds it for the read below.
        auto leaf_page = store.GetForRead(found.value().page_id);
        if (!leaf_page.ok()) return leaf_page.status();
        heap::PageView leaf(leaf_page.value().bytes());
        auto tuple = leaf.ReadTuple(found.value().slot);
        if (!tuple.ok()) {
            // A retired slot the index still points at - an insert this
            // transaction or another one rolled back. No row is there.
            if (tuple.status().code() == StatusCode::kNotFound) return FkVerdict::kViolation;
            return tuple.status();
        }
        return VerdictOf(txn::CheckVisibility(check_view, tuple.value()));
    }

    // Heap: no index to descend, so the chain walk is the authoritative
    // path. Unreachable through the DDL surface (a heap parent is refused at
    // declaration), and correct if it ever is reached.
    FkVerdict verdict = FkVerdict::kViolation;
    Status inner = Status::OK();
    auto visitor = [&](PageId, heap::PageView& page,
                       std::uint16_t slot) -> StatusOr<storage::VisitControl> {
        auto tuple = page.ReadTuple(slot);
        if (!tuple.ok()) {
            if (tuple.status().code() == StatusCode::kNotFound) {
                return storage::VisitControl::kContinue;
            }
            inner = tuple.status();
            return tuple.status();
        }
        auto id = RowKeystoneId(tuple.value().payload);
        if (!id.ok()) {
            inner = id.status();
            return id.status();
        }
        if (id.value() != parent_pk) return storage::VisitControl::kContinue;
        verdict = VerdictOf(txn::CheckVisibility(check_view, tuple.value()));
        return storage::VisitControl::kStop;
    };

    Status walked =
        heap::ChainVisit(store, parent.desc_page_id, storage::PageAccess::kRead, visitor);
    if (!inner.ok()) return inner;
    if (!walked.ok()) return walked;
    return verdict;
}

StatusOr<FkReverseOutcome> CheckNoChildReferences(storage::PageStore& store,
                                                  const catalog::TableAccess& child,
                                                  std::uint16_t child_column_no,
                                                  std::uint64_t parent_pk,
                                                  const txn::ReadView& check_view,
                                                  const FkReverseOptions& options,
                                                  Budget* budget) {
    FkReverseOutcome outcome;

    // ---- The Cabin, if this value has been observed (F6) ----------------
    std::optional<stats::CabinKey> key;
    if (options.cabins != nullptr && options.cabin_id != 0) {
        key = stats::MakeCabinKey(options.cabin_id, PkValue(parent_pk));
    }

    if (key.has_value()) {
        if (std::vector<stats::CabinEntry>* entries = options.cabins->Find(*key);
            entries != nullptr) {
            options.cabins->NoteHit(options.cabin_id);

            // An observed value's set is a **superset** of the pks that
            // carry it, so every entry has to be checked and none may be
            // trusted on sight - but the set being a superset is exactly
            // what makes an exhausted, all-non-matching scan of it an
            // authoritative "no children". That is the answer this whole
            // structure exists to give.
            const bool is_btree = child.clustered_type == catalog::ClusteredType::kBtree;
            std::unordered_set<std::uint64_t> seen;
            std::vector<parser::AstValue> scratch;
            bool usable = true;

            for (stats::CabinEntry& entry : *entries) {
                if (!seen.insert(entry.pk).second) continue;  // v→v′→v round trip

                PageId at_page = kInvalidPageId;
                std::uint16_t at_slot = 0;
                if (entry.hint_valid()) {
                    VerifiedTuple verified = VerifyTupleAt(store, entry.page_id, entry.slot,
                                                           entry.pk, entry.page_epoch);
                    options.cabins->NoteHint(options.cabin_id, verified.ok());
                    if (verified.ok()) {
                        at_page = entry.page_id;
                        at_slot = entry.slot;
                    }
                }
                if (at_page == kInvalidPageId) {
                    if (!is_btree) {
                        // No descent to heal the hint with. Abandon the
                        // Cabin for this check and walk - the same answer
                        // `ServeFromCabin` gives for the same reason, and
                        // safe here because nothing has been concluded yet.
                        options.cabins->Unobserve(*key);
                        usable = false;
                        break;
                    }
                    auto found = btree::BtreeLookup(store, child.desc_page_id, entry.pk);
                    if (!found.ok()) {
                        // Dangling: by K1 the pk can never name a different
                        // row, so this entry is dead forever - a skip, not
                        // an error.
                        if (found.status().code() == StatusCode::kNotFound) continue;
                        return found.status();
                    }
                    entry.page_id = found.value().page_id;
                    entry.slot = found.value().slot;
                    // Stamped from the fetch below - a heal that wrote 0
                    // against a bumped page would miss and re-heal forever.
                    entry.page_epoch = 0;
                    entry.flags |= stats::kCabinHintValid;
                    at_page = entry.page_id;
                    at_slot = entry.slot;
                }

                if (budget != nullptr) {
                    if (Status s = budget->ChargeRow(); !s.ok()) return s;
                }

                auto bytes = store.GetForRead(at_page);
                if (!bytes.ok()) return bytes.status();
                heap::PageView page(bytes.value().bytes());
                // The hint's epoch tracks the page it names. A no-op for a
                // verified entry (equality was just checked); the real
                // stamp for a healed one.
                entry.page_epoch = static_cast<std::uint32_t>(page.RelayoutEpoch());
                auto tuple = page.ReadTuple(at_slot);
                if (!tuple.ok()) {
                    if (tuple.status().code() == StatusCode::kNotFound) continue;
                    return tuple.status();
                }

                // The key re-check: the surplus an append-only set carries
                // is subtracted here, not by the store.
                auto value =
                    ForeignKeyValue(child, child_column_no, tuple.value().payload, scratch);
                if (!value.ok()) return value.status();
                if (!value.value().has_value() || *value.value() != parent_pk) continue;

                const txn::CheckVerdict seen_as = txn::CheckVisibility(check_view, tuple.value());
                if (seen_as == txn::CheckVerdict::kLive) {
                    outcome.verdict = FkVerdict::kViolation;
                    outcome.served_from_cabin = true;
                    return outcome;
                }
                if (seen_as == txn::CheckVerdict::kBusy) {
                    outcome.verdict = FkVerdict::kBusy;
                    outcome.served_from_cabin = true;
                    return outcome;
                }
            }

            if (usable) {
                // Every entry accounted for and none of them a live child.
                // No walk happened.
                outcome.verdict = FkVerdict::kPass;
                outcome.served_from_cabin = true;
                return outcome;
            }
        } else {
            options.cabins->NoteMiss(options.cabin_id);
        }
    }

    // ---- The walk (§3) --------------------------------------------------
    //
    // **The walk records nothing into the Cabin, and that is a correction to
    // F6 rather than an omission.** F6 expects a reverse check's observation
    // to be "naturally driven by exactly the parents that get deleted" - but
    // the value a reverse check would record is *the pk being deleted*, and a
    // given pk is deleted once. The set would be taught a value no check can
    // ever ask about again, while `cabin_max_values` is a cap that **refuses
    // to observe** once full: recording here would spend a bounded budget on
    // dead values and could crowd out the live ones a query would have used.
    //
    // A Cabin on a child's fk column still pays for the reverse check - the
    // hit path above is real - but its values arrive the ordinary way, from
    // queries that filter children by parent id. That is the workload where
    // such a Cabin is justified anyway, so the two now agree instead of one
    // subsidizing the other.
    FkVerdict verdict = FkVerdict::kPass;
    Status inner = Status::OK();
    std::vector<parser::AstValue> scratch;

    auto visitor = [&](PageId, heap::PageView& page,
                       std::uint16_t slot) -> StatusOr<storage::VisitControl> {
        auto tuple = page.ReadTuple(slot);
        if (!tuple.ok()) {
            if (tuple.status().code() == StatusCode::kNotFound) {
                return storage::VisitControl::kContinue;
            }
            inner = tuple.status();
            return tuple.status();
        }

        if (budget != nullptr) {
            if (Status s = budget->ChargeRow(); !s.ok()) {
                inner = s;
                return s;
            }
        }

        auto value = ForeignKeyValue(child, child_column_no, tuple.value().payload, scratch);
        if (!value.ok()) {
            inner = value.status();
            return value.status();
        }
        if (!value.value().has_value() || *value.value() != parent_pk) {
            return storage::VisitControl::kContinue;
        }

        const txn::CheckVerdict seen_as = txn::CheckVisibility(check_view, tuple.value());
        if (seen_as == txn::CheckVerdict::kAbsent) return storage::VisitControl::kContinue;

        verdict = seen_as == txn::CheckVerdict::kBusy ? FkVerdict::kBusy : FkVerdict::kViolation;
        return storage::VisitControl::kStop;
    };

    Status walked = child.clustered_type == catalog::ClusteredType::kBtree
                        ? btree::BtreeVisit(store, child.desc_page_id, storage::PageAccess::kRead,
                                            visitor)
                        : heap::ChainVisit(store, child.desc_page_id, storage::PageAccess::kRead,
                                           visitor);
    if (!inner.ok()) return inner;
    if (!walked.ok()) return walked;

    outcome.verdict = verdict;
    return outcome;
}

}  // namespace kds::exec
