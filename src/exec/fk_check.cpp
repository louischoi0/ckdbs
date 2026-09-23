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
                                       const txn::ReadView& check_view, Budget* budget,
                                       std::uint64_t* busy_trx) {
    if (busy_trx != nullptr) *busy_trx = 0;
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
        const FkVerdict verdict = VerdictOf(txn::CheckVisibility(check_view, tuple.value()));
        // The holder's id, for the caller that means to wait rather than
        // refuse (AO-S3). Read from the header the verdict was decided
        // from, so the two cannot disagree.
        if (verdict == FkVerdict::kBusy && busy_trx != nullptr) {
            *busy_trx = tuple.value().trx_id;
        }
        return verdict;
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
    // ---- Scope: this check answers for the whole child (AT-S5f) ---------
    //
    // **There is no scope question left to ask.** What stood here was a
    // refusal - a child whose ranges this core did not own was answered by
    // nobody, because the walk below covered the chains this core owned
    // (RD6: one chain per range) and would have reported *"no children"*
    // for a child row in another core's range, which is not a slow answer
    // but a **dropped constraint**, the one degraded mode
    // `foreign-keys.md` §1 says a constraint may not have. The fan-out
    // that replaced the refusal (AJ-T2) asked each range owner in turn.
    //
    // Both are gone with the route. Every core reads every page through
    // the one pool since AM-S2 step 3, so the walk covers **every** chain
    // of the child here (`AllWalkHeads` below), and an answer from this
    // core is an answer for the whole relation. The Cabin is the one thing
    // still scoped to a core, and it is handled where it is read rather
    // than by a guard over the whole function: a per-core set may *find* a
    // child and may not *clear* one (AT-R15, D4's first half).

    FkReverseOutcome outcome;

    // ---- The Cabin, if this value has been observed (F6) ----------------
    std::optional<stats::CabinKey> key;
    if (options.cabins != nullptr && options.cabin_id != 0) {
        key = stats::MakeCabinKey(options.cabin_id, PkValue(parent_pk));
    }

    if (key.has_value()) {
        if (const stats::CabinSet set = options.cabins->Find(*key); set.valid()) {
            options.cabins->NoteHit(options.cabin_id);

            // An observed value's set is a **superset** of the pks that
            // carry it, so every entry has to be checked and none may be
            // trusted on sight. A live match found here is therefore
            // authoritative and returns without walking; an exhausted scan
            // is not, and the loop's exit says why.
            const bool is_btree = child.clustered_type == catalog::ClusteredType::kBtree;
            std::unordered_set<std::uint64_t> seen;
            std::vector<parser::AstValue> scratch;

            for (std::size_t i = 0; i < set.size(); ++i) {
                // By value, under the partition's latch (AT-S7): the set is
                // the instance's, and another core may be healing this
                // entry while this loop reads it.
                const stats::CabinEntry entry = set.At(i);
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
                        // set for this check - `ServeFromCabin`'s answer
                        // for the same reason - and take the walk, which
                        // is where every exit from this loop goes now.
                        options.cabins->Unobserve(*key);
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
                    at_page = found.value().page_id;
                    at_slot = found.value().slot;
                }

                if (budget != nullptr) {
                    if (Status s = budget->ChargeRow(); !s.ok()) return s;
                }

                auto bytes = store.GetForRead(at_page);
                if (!bytes.ok()) return bytes.status();
                heap::PageView page(bytes.value().bytes());
                // **The heal, by index** (AT-S7): the store writes the
                // hint under its partition's latch, so a page read never
                // happens under one. A no-op in substance for a verified
                // entry - the location it writes back is the location that
                // just verified - and the real heal for a descended one,
                // whose epoch has to come from the page this fetch just
                // read rather than from a 0 that would miss forever.
                set.Heal(i, at_page, at_slot,
                         static_cast<std::uint32_t>(page.RelayoutEpoch()));
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

            // **An exhausted set no longer clears the parent** (AT-R15,
            // D4's first half). The argument above is sound about the set
            // and wrong about the *store*: `stats::CabinStore` is a
            // dispatcher's own, so a child row inserted on another core
            // never reached this set, and a drained loop here would clear
            // a parent that has a child - `foreign-keys.md` §1's one
            // forbidden answer, from the structure that exists to give the
            // opposite one. So a set may **find** a child, which is what
            // the two returns above do, and may not clear one: the walk
            // below is what answers "no children" until the store becomes
            // the instance's at AT-S7 (AT-0 item 9), which is what restores
            // this return rather than removing it.
            //
            // `served_from_cabin` therefore stays true only on a hit, which
            // is what `SHOW ACCESS` has always meant by it.
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

    // **One walk per chain the relation has** (RD6: one chain per range),
    // and not per chain this core owns, which is what it read until
    // AT-S5f. `WalkHeadsFor` is the *read path's* rule - a stage of a
    // fan-in covers the ranges it owns because the session concatenates
    // the stages - and a constraint check has no second stage to
    // concatenate: it answers for the whole child or it drops the
    // constraint. Every page is faultable from every core since AM-S2
    // step 3, so the heads another core's ranges name are walked here.
    // **No range arm on the btree side**, and D1 is what makes that an
    // absence rather than an omission: a btree relation never splits, so a
    // btree child never has a directory.
    Status walked = Status::OK();
    if (child.clustered_type == catalog::ClusteredType::kBtree) {
        walked = btree::BtreeVisit(store, child.desc_page_id, storage::PageAccess::kRead, visitor);
    } else {
        const std::vector<PageId> heads = child.AllWalkHeads();
        // **Checked rather than assumed**, `TableAccess::RangeFor`'s reason
        // in this function's terms: no heads would run no loop body, leave
        // `verdict` at `kPass`, and report "no children" having read
        // nothing - the silent drop this check may never produce.
        // `AllWalkHeads` answers `desc_page_id` for an unsplit relation and
        // one entry per range otherwise, so it is empty for no relation
        // this engine can build - which is exactly when this engine checks
        // instead of trusting.
        if (heads.empty()) {
            return Status::Corruption(
                "relation oid " + std::to_string(child.oid) +
                " yielded no chain heads; a foreign key's reverse check cannot answer 'no "
                "children' from no walk");
        }
        for (const PageId head : heads) {
            walked = heap::ChainVisit(store, head, storage::PageAccess::kRead, visitor);
            // A match ends the whole check, not merely this chain: the
            // visitor stops at the first referencing row and `verdict`
            // carries which kind it was.
            if (!walked.ok() || verdict != FkVerdict::kPass) break;
        }
    }
    if (!inner.ok()) return inner;
    if (!walked.ok()) return walked;

    outcome.verdict = verdict;
    return outcome;
}

}  // namespace kds::exec
