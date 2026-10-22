#include "kds/exec/fk_check.hpp"

#include "kds/storage/keystone.hpp"


#include <optional>
#include <unordered_set>
#include <vector>

#include "kds/exec/row_codec.hpp"
#include "kds/exec/tuple_verify.hpp"
#include "kds/storage/btree/btree.hpp"
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
// A tuple key is an integer by invariant 11, so this never has to consider the
// other value kinds.
parser::AstValue TupleKeyValue(std::uint64_t tuple_key) {
    parser::AstValue value;
    value.type = parser::ValueType::kInt;
    value.int_val = static_cast<std::int64_t>(tuple_key);
    return value;
}

// **A row an undecided writer may have moved off the value it is checked
// against** (AY-Q8): written by a transaction the check's view cannot see,
// over an earlier version. The undo test first: it is a field read, and the
// visibility test a latched window lookup for a recent writer.
bool UndecidedWithEarlierVersion(const txn::ReadView& view, const heap::PageView::Tuple& tuple) {
    return tuple.undo_ptr != txn::kNoUndoPtr &&
           txn::CheckVisibility(view, tuple) == txn::CheckVerdict::kBusy;
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
                                       std::uint64_t parent_tuple_key,
                                       const txn::ReadView& check_view, Budget* budget,
                                       std::uint64_t* busy_trx) {
    if (busy_trx != nullptr) *busy_trx = 0;
    if (budget != nullptr) {
        if (Status s = budget->ChargeRow(); !s.ok()) return s;
    }

    // The descent is authoritative in both directions on a clustered
    // relation: a hit is where the row lives, and a miss means there is
    // no such row - the same property that lets a point SELECT skip the
    // scan.
    auto found = btree::BtreeLookup(store, parent.desc_page_id, parent_tuple_key);
    if (!found.ok()) {
        if (found.status().code() == StatusCode::kNotFound) return FkVerdict::kViolation;
        return found.status();
    }
    // Through the held leaf (btree.hpp `Location`): with no residual
    // here, a renumbered slot would decide the verdict on another row.
    heap::PageView leaf(found.value().leaf.bytes());
    auto tuple = leaf.ReadTuple(found.value().slot);
    if (!tuple.ok()) {
        // A retired slot the index still points at - an insert this
        // transaction or another one rolled back. No row is there.
        if (tuple.status().code() == StatusCode::kNotFound) return FkVerdict::kViolation;
        return tuple.status();
    }
    const FkVerdict verdict = VerdictOf(txn::CheckVisibility(check_view, tuple.value()));
    // The holder's id, for the caller that means to wait rather than
    // refuse (AO-S3). Read from the header the verdict was decided
    // from, so the two cannot disagree.
    if (verdict == FkVerdict::kBusy && busy_trx != nullptr) {
        *busy_trx = tuple.value().trx_id;
    }
    return verdict;
}

StatusOr<FkReverseOutcome> CheckNoChildReferences(storage::PageStore& store,
                                                  const catalog::TableAccess& child,
                                                  std::uint16_t child_column_no,
                                                  std::uint64_t parent_tuple_key,
                                                  const txn::ReadView& check_view,
                                                  const FkReverseOptions& options,
                                                  Budget* budget) {
    // ---- Scope: this check answers for the whole child (AT-S5f) ---------
    //
    // Every core reads every page through the one pool (AM-S2), so the walk
    // covers the child's one structure and answers for the whole relation
    // (the range-scope refusal retired with the split relation; history at
    // cf889b63). A Cabin set may *find* a child and, since AY-S6, *clear* a
    // parent (below).

    FkReverseOutcome outcome;

    // ---- The Cabin, if this value has been observed (F6) ----------------
    std::optional<stats::CabinKey> key;
    if (options.cabins != nullptr && options.cabin_id != 0) {
        key = stats::MakeCabinKey(options.cabin_id, TupleKeyValue(parent_tuple_key));
    }

    if (key.has_value()) {
        if (const stats::CabinSet set = options.cabins->Find(*key); set.valid()) {
            options.cabins->NoteHit(options.cabin_id);

            // An observed value's set is a **superset** of the tuple keys that
            // carry it, so every entry has to be checked and none may be
            // trusted on sight. A live match found here is authoritative and
            // returns without walking; so is an exhausted loop that checked
            // every entry (below). `usable` is whether it did.
            std::unordered_set<std::uint64_t> seen;
            std::vector<parser::AstValue> scratch;
            bool usable = true;

            for (std::size_t i = 0; i < set.size(); ++i) {
                // By value, under the partition's latch (AT-S7): the set is
                // the instance's, and another core may be healing this
                // entry while this loop reads it.
                const stats::CabinEntry entry = set.At(i);
                if (!seen.insert(entry.tuple_key).second) continue;  // v→v′→v round trip

                // Held from the moment the slot is vouched for - by the
                // verifier or by the descent - to the read (btree.hpp
                // `Location`).
                PageId at_page = kInvalidPageId;
                std::uint16_t at_slot = 0;
                storage::PageRef held;
                if (entry.hint_valid()) {
                    VerifiedTuple verified = VerifyTupleAt(store, entry.page_id, entry.slot,
                                                           entry.tuple_key, entry.page_epoch, child.oid);
                    options.cabins->NoteHint(options.cabin_id, verified.ok());
                    if (verified.ok()) {
                        at_page = entry.page_id;
                        at_slot = entry.slot;
                        held = std::move(verified.ref);
                    }
                }
                if (at_page == kInvalidPageId) {
                    auto found = btree::BtreeLookup(store, child.desc_page_id, entry.tuple_key);
                    if (!found.ok()) {
                        // Dangling: the tuple key names no row now - a skip, not an
                        // error. A committed key names another row only
                        // after a `PURGE` freed it (K1); a rolled-back or
                        // purged one may be named again (W12, BD-R4, BH),
                        // and a row found under it is re-checked against the
                        // column below like any other.
                        if (found.status().code() == StatusCode::kNotFound) continue;
                        return found.status();
                    }
                    at_page = found.value().page_id;
                    at_slot = found.value().slot;
                    held = std::move(found.value().leaf);
                }

                if (budget != nullptr) {
                    if (Status s = budget->ChargeRow(); !s.ok()) return s;
                }

                heap::PageView page(held.bytes());
                // **The heal, by index** (AT-S7): the store writes the
                // hint under its partition's latch, so a page read never
                // happens under one. A no-op in substance for a verified
                // entry - the location it writes back is the location that
                // just verified - and the real heal for a descended one,
                // whose epoch has to come from the page this fetch just
                // read rather than from a 0 that would miss forever.
                set.Heal(i, at_page, at_slot, CurrentPageEpoch(page));
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
                if (!value.value().has_value() || *value.value() != parent_tuple_key) {
                    // **A row an undecided writer moved off the parent**
                    // keeps its tuple key in this set and holds the writer's value
                    // in its page (AY-Q8): skipping it would clear a parent
                    // the writer's rollback references again. The set is
                    // given up for this check and the walk, which reads such
                    // a row's earlier version, answers.
                    if (UndecidedWithEarlierVersion(check_view, tuple.value())) {
                        usable = false;
                        break;
                    }
                    continue;
                }

                const txn::CheckVerdict seen_as = txn::CheckVisibility(check_view, tuple.value());
                if (seen_as == txn::CheckVerdict::kLive) {
                    outcome.verdict = FkVerdict::kViolation;
                    outcome.served_from_cabin = true;
                    return outcome;
                }
                if (seen_as == txn::CheckVerdict::kBusy) {
                    outcome.verdict = FkVerdict::kBusy;
                    outcome.served_from_cabin = true;
                    outcome.busy_trx = tuple.value().trx_id;
                    outcome.busy_tuple_key = entry.tuple_key;
                    return outcome;
                }
            }

            // **An exhausted set clears the parent** (AY-S6): every entry
            // checked, none a live or undecided child. `foreign-keys.md` §3a
            // states what that rests on - the instance's one store, D9(a)'s
            // `S` under the caller's `X`, the banking gate - and a loop that
            // gave the set up above is not exhausted.
            if (usable) {
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
    // **The walk records nothing into the Cabin** - a correction to F6 the
    // header states: a deleted tuple key is dead by construction and would spend
    // `cabin_max_values`' bounded budget.
    FkVerdict verdict = FkVerdict::kPass;
    Status inner = Status::OK();
    std::vector<parser::AstValue> scratch;

    // **A row an undecided writer moved off the parent** (AY-Q8; found by
    // AY-S4's cell `AParentDeleteWaitsOutAChildMovedOffItAndRefusesAtItsRollback`).
    // Its page holds the writer's value and not the parent's, so the key
    // test below skips it - and the writer's rollback would put the
    // reference back after this check said "no children". Such a row is
    // copied out here and read back through its undo after the walk, once
    // the child's span is released (the parent `DELETE`'s own is still
    // live, the exposure the header names): busy if the version the check's
    // view can see
    // references the parent. A fresh insert has no earlier version and is
    // not collected.
    struct Moved {
        std::uint64_t trx_id = 0;
        bool deleted = false;
        std::uint64_t undo_ptr = 0;
        std::vector<std::byte> payload;
    };
    std::vector<Moved> moved;

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
        if (!value.value().has_value() || *value.value() != parent_tuple_key) {
            if (UndecidedWithEarlierVersion(check_view, tuple.value())) {
                const auto bytes = tuple.value().payload;
                moved.push_back(Moved{tuple.value().trx_id, tuple.value().deleted,
                                      tuple.value().undo_ptr,
                                      std::vector<std::byte>(bytes.begin(), bytes.end())});
            }
            return storage::VisitControl::kContinue;
        }
        const txn::CheckVerdict seen_as = txn::CheckVisibility(check_view, tuple.value());
        if (seen_as == txn::CheckVerdict::kAbsent) return storage::VisitControl::kContinue;

        verdict = seen_as == txn::CheckVerdict::kBusy ? FkVerdict::kBusy : FkVerdict::kViolation;
        if (verdict == FkVerdict::kBusy) {
            auto tuple_key = KeystoneIdOfPayload(tuple.value().payload);
            if (!tuple_key.ok()) {
                inner = tuple_key.status();
                return tuple_key.status();
            }
            outcome.busy_trx = tuple.value().trx_id;
            outcome.busy_tuple_key = tuple_key.value();
        }
        return storage::VisitControl::kStop;
    };

    const Status walked =
        btree::BtreeVisit(store, child.desc_page_id, storage::PageAccess::kRead, visitor);
    if (!inner.ok()) return inner;
    if (!walked.ok()) return walked;

    // The moved rows, only when the walk found nothing: a match already
    // answers, and a moved row can only turn "no children" into busy.
    for (Moved& row : moved) {
        if (verdict != FkVerdict::kPass) break;
        auto tuple_key = KeystoneIdOfPayload(row.payload);
        if (!tuple_key.ok()) return tuple_key.status();
        bool references = true;  // with no log to read, the row may reference it
        if (options.undo != nullptr) {
            auto seen = txn::ResolveThroughUndo(check_view, *options.undo, row.trx_id, row.deleted,
                                                row.undo_ptr, row.payload);
            if (!seen.ok()) return seen.status();
            references = false;
            if (seen.value() == txn::Visibility::kVisible) {
                auto value = ForeignKeyValue(child, child_column_no, row.payload, scratch);
                if (!value.ok()) return value.status();
                references = value.value().has_value() && *value.value() == parent_tuple_key;
            }
        }
        if (references) {
            verdict = FkVerdict::kBusy;
            outcome.busy_trx = row.trx_id;
            outcome.busy_tuple_key = tuple_key.value();
        }
    }

    outcome.verdict = verdict;
    return outcome;
}

}  // namespace kds::exec
