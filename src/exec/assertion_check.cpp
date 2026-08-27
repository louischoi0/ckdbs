#include "kds/exec/assertion_check.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <utility>

#include "kds/exec/assertion_violation.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/payload.hpp"

namespace kds::exec {
namespace {

using storage::cabin::BoundCabinEntry;
using storage::cabin::BoundCabinPage;
using storage::cabin::kEntryDeparture;
using storage::cabin::kEntryHintValid;
using storage::cabin::kEntryReserved;

// Schema position -> the caller's value. INSERT statements carry columns
// after the pk (first_col_pos 1); UPDATE/DELETE hand the decoded row
// (first_col_pos 0) - the same convention NoteCabinWrite and MaintainIndexes
// take, for the same reason.
const parser::AstValue& ValueAt(std::span<const parser::AstValue> values,
                                std::size_t first_col_pos, std::uint16_t schema_pos) {
    return values[schema_pos - first_col_pos];
}

std::string KeyFor(const LiveAssertion& a, std::span<const parser::AstValue> values,
                   std::size_t first_col_pos) {
    std::vector<parser::AstValue> group(a.group_cols.size());
    for (std::size_t i = 0; i < a.group_cols.size(); ++i) {
        group[i] = ValueAt(values, first_col_pos, a.group_cols[i]);
    }
    return EncodeGroupKey(group);
}

std::int64_t ContributionOf(const LiveAssertion& a, std::span<const parser::AstValue> values,
                            std::size_t first_col_pos) {
    // `sum_col` names a column only for a SUM assertion, so the value is
    // read only then. What it contributes - including that a NULL
    // contributes nothing - is bound_cabin.hpp's one rule, which CREATE
    // ASSERTION's backfill folds by too.
    if (a.aggregate != BoundAggregate::kSum) return 1;  // COUNT(*) counts rows
    return SumContribution(ValueAt(values, first_col_pos, a.sum_col));
}

// The §4.4 refusal, from the one place the format lives.
Status Refuse(const LiveAssertion& a, std::span<const parser::AstValue> values,
              std::size_t first_col_pos) {
    std::vector<GroupKeyPart> parts(a.group_cols.size());
    for (std::size_t i = 0; i < a.group_cols.size(); ++i) {
        parts[i].column = a.group_col_names[i];
        parts[i].type_val = a.group_type_vals[i];
        parts[i].value = ValueAt(values, first_col_pos, a.group_cols[i]);
    }
    return Status::AssertionViolation(AssertionViolationMessage(
        a.name, parts, a.aggregate, a.sum_col_name, a.cabin.bound()));
}

}  // namespace

std::vector<wal::AssertionCabinSnapshot> AssertionEnforcer::SnapshotAssertions() const {
    // AS6a's base, one record's worth per cabin: headers only, keys owned, and
    // never the entry lists - those are O(all writes, forever) and are rebuilt
    // at recovery from the entries' own `group_id` instead.
    std::vector<wal::AssertionCabinSnapshot> out;
    out.reserve(live_.size());
    for (const auto& [assertion_id, assertion] : live_) {
        wal::AssertionCabinSnapshot cabin;
        cabin.assertion_id = assertion_id;
        for (const BoundCabin::GroupSnapshot& group : assertion.cabin.SnapshotGroups()) {
            wal::AssertionSnapshotGroup entry;
            entry.group_id = group.group_id;
            entry.count = group.count;
            entry.sum = group.sum;
            entry.key = group.key;
            cabin.groups.push_back(std::move(entry));
        }
        out.push_back(std::move(cabin));
    }
    // Ordered by assertion id, for the reason SnapshotGroups() orders by group
    // id: a checkpoint's bytes must be a function of its input, and an
    // unordered_map's iteration order is not one (sched.md §8).
    std::sort(out.begin(), out.end(),
              [](const wal::AssertionCabinSnapshot& a, const wal::AssertionCabinSnapshot& b) {
                  return a.assertion_id < b.assertion_id;
              });
    return out;
}

void AssertionEnforcer::Adopt(LiveAssertion assertion) {
    const std::uint64_t id = assertion.assertion_id;
    const catalog::Oid oid = assertion.target_oid;
    live_.insert_or_assign(id, std::move(assertion));
    auto& ids = by_oid_[oid];
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    // An id that is now enforced is no longer one this core merely knows
    // about, whatever a previous mount concluded. Cleared per id rather
    // than per relation: a relation may carry both, and a write it admits
    // on the strength of this one would still be unchecked against the
    // other (PW1c-6c).
    auto stale = unenforceable_.find(oid);
    if (stale == unenforceable_.end()) return;
    auto& blocked = stale->second;
    blocked.erase(std::remove(blocked.begin(), blocked.end(), id), blocked.end());
    if (blocked.empty()) unenforceable_.erase(stale);
}

void AssertionEnforcer::NoteUnenforceable(catalog::Oid oid, std::uint64_t assertion_id) {
    // Never for something this core is already enforcing: the two states
    // are exclusive, and the enforced one is the truthful answer.
    if (live_.count(assertion_id) != 0) return;
    auto& ids = unenforceable_[oid];
    if (std::find(ids.begin(), ids.end(), assertion_id) == ids.end()) {
        ids.push_back(assertion_id);
    }
}

void AssertionEnforcer::Evict(std::uint64_t assertion_id) {
    // The unenforceable record first, and **before the early return**: an
    // assertion this core could not enforce is exactly the one an operator
    // drops in order to re-create it (PW1c-6c's repair for a file written
    // before it), and leaving the record behind would keep refusing the
    // relation's writes for an assertion that no longer exists. Nothing is
    // in both maps - `NoteUnenforceable` refuses an id `live_` holds - so
    // this touches at most one of the two.
    for (auto blocked = unenforceable_.begin(); blocked != unenforceable_.end(); ++blocked) {
        auto& ids = blocked->second;
        const auto found = std::find(ids.begin(), ids.end(), assertion_id);
        if (found == ids.end()) continue;
        ids.erase(found);
        if (ids.empty()) unenforceable_.erase(blocked);
        break;
    }
    auto it = live_.find(assertion_id);
    if (it == live_.end()) return;
    auto& ids = by_oid_[it->second.target_oid];
    ids.erase(std::remove(ids.begin(), ids.end(), assertion_id), ids.end());
    if (ids.empty()) by_oid_.erase(it->second.target_oid);
    live_.erase(it);
    // Pending reservations against the evicted assertion stay in their
    // transactions' lists and are skipped at commit/abort by the lookup
    // below - the same rule replay's skip has, and for the same reason.
}

Status AssertionEnforcer::AdmitInsert(catalog::Oid oid,
                                      std::span<const parser::AstValue> values) {
    auto on = by_oid_.find(oid);
    if (on == by_oid_.end()) return Status::OK();
    for (const std::uint64_t id : on->second) {
        LiveAssertion& a = live_.at(id);
        const std::string key = KeyFor(a, values, /*first_col_pos=*/1);
        ++a.counters.checks;
        auto admitted = a.cabin.Admit(key, ContributionOf(a, values, 1));
        if (!admitted.ok()) return admitted.status();
        if (!admitted.value().admitted) {
            ++a.counters.violations;
            return Refuse(a, values, 1);
        }
    }
    return Status::OK();
}

Status AssertionEnforcer::ReserveOne(storage::PageStore& store, wal::WalManager* wal,
                                     std::uint64_t txn_id, LiveAssertion& a,
                                     const std::string& key, bool departure,
                                     std::int64_t value, std::uint64_t pk, PageId row_page,
                                     std::uint16_t row_slot) {
    BoundCabinEntry entry;
    entry.pk = pk;
    entry.flags = static_cast<std::uint8_t>(kEntryReserved | kEntryHintValid |
                                            (departure ? kEntryDeparture : 0));
    entry.page_id = row_page;
    entry.page_epoch = 0;
    entry.slot = row_slot;
    entry.value = value;
    // AS6a, and the departure case is why this is not `EnsureGroupId`
    // unconditionally: a departure's group must already exist (the row it
    // removes was incorporated by build or insert), and creating one here would
    // create a group to immediately go negative in - which `ApplyDeparture`
    // answers NotFound for, deliberately.
    if (departure) {
        const GroupHeader* header = a.cabin.Find(key);
        entry.group_id = header != nullptr ? header->group_id : 0;
    } else {
        entry.group_id = a.cabin.EnsureGroupId(key);
    }

    auto at = a.chain.Append(store, wal, entry, key, wal::RecordType::kAssertReserve, txn_id);
    if (!at.ok()) return at.status();
    Status applied = departure
                         ? a.cabin.ApplyDeparture(key, value, at.value().first, at.value().second)
                         : a.cabin.Apply(key, value, at.value().first, at.value().second);
    if (!applied.ok()) return applied;

    Reservation r;
    r.assertion_id = a.assertion_id;
    r.key = key;
    r.departure = departure;
    r.value = value;
    r.page = at.value().first;
    r.index = at.value().second;
    pending_[txn_id].push_back(std::move(r));
    ++a.counters.reserved;
    return Status::OK();
}

Status AssertionEnforcer::ReserveInsert(storage::PageStore& store, wal::WalManager* wal,
                                        std::uint64_t txn_id, catalog::Oid oid,
                                        std::span<const parser::AstValue> values,
                                        std::uint64_t pk, PageId row_page,
                                        std::uint16_t row_slot) {
    auto on = by_oid_.find(oid);
    if (on == by_oid_.end()) return Status::OK();
    for (const std::uint64_t id : on->second) {
        LiveAssertion& a = live_.at(id);
        const std::string key = KeyFor(a, values, 1);
        if (Status s = ReserveOne(store, wal, txn_id, a, key, /*departure=*/false,
                                  ContributionOf(a, values, 1), pk, row_page, row_slot);
            !s.ok()) {
            return s;
        }
    }
    return Status::OK();
}

Status AssertionEnforcer::AdmitAndReserveUpdate(storage::PageStore& store, wal::WalManager* wal,
                                                std::uint64_t txn_id, catalog::Oid oid,
                                                std::span<const parser::AstValue> old_row,
                                                std::span<const parser::AstValue> new_row,
                                                std::uint64_t pk, PageId row_page,
                                                std::uint16_t row_slot) {
    auto on = by_oid_.find(oid);
    if (on == by_oid_.end()) return Status::OK();
    for (const std::uint64_t id : on->second) {
        LiveAssertion& a = live_.at(id);
        const std::string old_key = KeyFor(a, old_row, 0);
        const std::string new_key = KeyFor(a, new_row, 0);
        const std::int64_t old_contrib = ContributionOf(a, old_row, 0);
        const std::int64_t new_contrib = ContributionOf(a, new_row, 0);

        if (old_key == new_key) {
            // §4.2 rows 2 and 3: the aggregate is invariant (nothing at
            // all), or only the SUM moved - checked iff the delta is
            // positive, because a decrease cannot violate an upper bound.
            if (old_contrib == new_contrib) continue;
            std::int64_t delta = 0;
            if (__builtin_sub_overflow(new_contrib, old_contrib, &delta)) {
                return Status::OutOfRange("assertion \"" + a.name +
                                          "\": SUM delta overflows int64");
            }
            if (delta > 0) {
                ++a.counters.checks;
                auto admitted = a.cabin.Admit(new_key, delta);
                if (!admitted.ok()) return admitted.status();
                if (!admitted.value().admitted) {
                    ++a.counters.violations;
                    return Refuse(a, new_row, 0);
                }
            }
        } else {
            // §4.2 row 4: departure (no check) + arrival (checked, at its
            // full contribution - leaving one group buys nothing in
            // another).
            ++a.counters.checks;
            auto admitted = a.cabin.Admit(new_key, new_contrib);
            if (!admitted.ok()) return admitted.status();
            if (!admitted.value().admitted) {
                ++a.counters.violations;
                return Refuse(a, new_row, 0);
            }
        }

        // The mutation is uniform whatever the case above decided: the old
        // contribution leaves, the new one arrives, atomically on this core
        // - and both halves are entries, so header == Σ(entries) survives.
        if (Status s = ReserveOne(store, wal, txn_id, a, old_key, /*departure=*/true,
                                  old_contrib, pk, row_page, row_slot);
            !s.ok()) {
            return s;
        }
        if (Status s = ReserveOne(store, wal, txn_id, a, new_key, /*departure=*/false,
                                  new_contrib, pk, row_page, row_slot);
            !s.ok()) {
            return s;
        }
    }
    return Status::OK();
}

Status AssertionEnforcer::ReserveDelete(storage::PageStore& store, wal::WalManager* wal,
                                        std::uint64_t txn_id, catalog::Oid oid,
                                        std::span<const parser::AstValue> old_row,
                                        std::uint64_t pk, PageId row_page,
                                        std::uint16_t row_slot) {
    auto on = by_oid_.find(oid);
    if (on == by_oid_.end()) return Status::OK();
    for (const std::uint64_t id : on->second) {
        LiveAssertion& a = live_.at(id);
        if (Status s = ReserveOne(store, wal, txn_id, a, KeyFor(a, old_row, 0),
                                  /*departure=*/true, ContributionOf(a, old_row, 0), pk,
                                  row_page, row_slot);
            !s.ok()) {
            return s;
        }
    }
    return Status::OK();
}

Status AssertionEnforcer::CommitTxn(storage::PageStore& store, wal::WalManager* wal,
                                    std::uint64_t txn_id) {
    auto pending = pending_.find(txn_id);
    if (pending == pending_.end()) return Status::OK();

    // **Taken out of the map before anything can fail**, so a transaction's
    // reservations are settled exactly once whatever happens below. Every exit
    // from here is an error return, and leaving the list behind on one means the
    // *next* transaction under this id settles them a second time - reachable,
    // because `CommandDispatcher::EndWrite` uses `catalog::kBootstrapXid` for
    // every statement when there is no transaction manager, so the id is not
    // unique per transaction there. Double-settling silently moves an aggregate
    // twice; dropping the remainder of a failed settle leaves it wrong in a way
    // the returned error already reports. The second is the failure to prefer.
    const std::vector<Reservation> reservations = std::move(pending->second);
    pending_.erase(pending);

    // Batched per (assertion, page), the physiological unit ASSERT_COMMIT
    // describes.
    std::map<std::pair<std::uint64_t, PageId>, std::vector<std::uint16_t>> by_page;
    for (const Reservation& r : reservations) {
        if (!Holds(r.assertion_id)) continue;  // dropped mid-transaction
        by_page[{r.assertion_id, r.page}].push_back(r.index);
    }

    for (const auto& [group, indexes] : by_page) {
        auto page = store.Get(group.second);
        if (!page.ok()) return page.status();
        auto view = BoundCabinPage::Open(page.value().bytes());
        if (!view.ok()) return view.status();
        // Log, then clear, then stamp - the order `AbortTxn` documents below,
        // and the same window it closes: between a flag move and its
        // `StampPageLsn` the frame still carries an already-durable `page_lsn`,
        // so the flush gate would let the move out ahead of the record that
        // describes it.
        wal::Lsn stamp = wal::kNoLsn;
        if (wal != nullptr) {
            std::vector<std::byte> payload(wal::kAssertCommitFixedSize + indexes.size() * 2);
            wal::AssertCommitPayload fields{};
            fields.assertion_id = group.first;
            auto used = wal::EncodeAssertCommit(payload, fields, indexes);
            if (!used.ok()) return used.status();
            auto rec = wal->Append(
                wal::RecordSpec{wal::RecordType::kAssertCommit, txn_id, group.second}, payload);
            if (!rec.ok()) return rec.status();
            stamp = rec.value();
        }

        for (const std::uint16_t index : indexes) {
            if (Status s = view.value().ClearReserved(index); !s.ok()) {
                return s.WithContext("assert commit: clearing the reserved flag");
            }
        }

        if (stamp != wal::kNoLsn) {
            if (Status s = store.StampPageLsn(group.second, stamp); !s.ok()) return s;
        }
    }
    return Status::OK();
}

Status AssertionEnforcer::AbortTxn(storage::PageStore& store, wal::WalManager* wal,
                                   std::uint64_t txn_id) {
    auto pending = pending_.find(txn_id);
    if (pending == pending_.end()) return Status::OK();

    // Taken out of the map first, for the reason `CommitTxn` states: every exit
    // below is an error return, and a reservation left pending is one the next
    // transaction under this id would settle a second time.
    const std::vector<Reservation> reservations = std::move(pending->second);
    pending_.erase(pending);

    // Reverse order, the undo trail's own convention - not load-bearing for
    // these (removal is by name), but it keeps the compensation stream
    // reading as an unwind.
    for (auto it = reservations.rbegin(); it != reservations.rend(); ++it) {
        auto held = live_.find(it->assertion_id);
        if (held == live_.end()) continue;  // dropped mid-transaction
        LiveAssertion& a = held->second;
        Status undone = it->departure
                            ? a.cabin.UnapplyDeparture(it->key, it->value, it->page, it->index)
                            : a.cabin.Unapply(it->key, it->value, it->page, it->index);
        if (!undone.ok()) return undone;
        ++a.counters.aborted;

        // **Log, then mark, then stamp** - WAL-before-data, and here the order
        // is the correctness argument rather than a convention. Marking first
        // leaves a window where the frame carries its *old*, already-durable
        // `page_lsn`, so the flush gate would let the mark out ahead of the
        // record describing it; a failing `Append` then makes that permanent,
        // and the next recovery meets a header counting a reservation whose
        // entry the page says is orphaned - `VerifyAgainstEntries` reporting
        // Corruption for a directory that this time really is wrong.
        //
        // `Unapply` above stays ahead of both: its missing-pair refusal is the
        // check that proves `(page, index)` names *this* group's entry, and it
        // must run before a flag is OR'd into whatever lives at that slot.
        wal::Lsn stamp = wal::kNoLsn;
        if (wal != nullptr) {
            std::vector<std::byte> payload(wal::kAssertRollbackFixedSize + it->key.size());
            wal::AssertRollbackPayload fields{};
            fields.assertion_id = it->assertion_id;
            fields.delta = it->value;
            fields.index = it->index;
            auto used = wal::EncodeAssertRollback(
                payload, fields,
                std::as_bytes(std::span<const char>(it->key.data(), it->key.size())));
            if (!used.ok()) return used.status();
            wal::RecordSpec spec{wal::RecordType::kAssertRollback, txn_id, it->page};
            spec.flags = it->departure ? wal::kAssertRollbackFlagDeparture : 0;
            auto rec = wal->Append(spec, payload);
            if (!rec.ok()) return rec.status();
            stamp = rec.value();
        }

        // The entry bytes stay - the slot is the recorded leak that rides on
        // purge - but they are marked, so a rebuild scanning only these pages
        // reaches the same directory this `Unapply` just produced
        // (`storage/cabin_bound_page.hpp`'s `kEntryOrphaned`, the AS6b decision
        // in `docs/spec/assertion.md` §7). Unconditional, not `wal != nullptr`:
        // the mark is a data change, and skipping it without a log would leave
        // a page the next scan misreads.
        auto page = store.Get(it->page);
        if (!page.ok()) return page.status().WithContext("assert abort: fetching the entry page");
        auto view = BoundCabinPage::Open(page.value().bytes());
        if (!view.ok()) return view.status().WithContext("assert abort: opening the entry page");
        if (Status s = view.value().MarkOrphaned(it->index); !s.ok()) {
            return s.WithContext("assert abort: marking the entry orphaned");
        }

        if (stamp != wal::kNoLsn) {
            if (Status s = store.StampPageLsn(it->page, stamp); !s.ok()) return s;
        }
    }
    return Status::OK();
}

}  // namespace kds::exec
