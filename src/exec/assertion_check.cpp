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

// ---- The latched surface ------------------------------------------------
//
// Every accessor takes the directory latch, which is null unarmed; see the
// header for what it guards and the order it sits in.

bool AssertionEnforcer::empty() const {
    const LatchGuard guard(latch_.get());
    return live_.empty();
}

bool AssertionEnforcer::Holds(std::uint64_t assertion_id) const {
    const LatchGuard guard(latch_.get());
    return live_.count(assertion_id) != 0;
}

bool AssertionEnforcer::AnyOn(catalog::Oid oid) const {
    if (NothingDeclared()) return false;
    const LatchGuard guard(latch_.get());
    return by_oid_.count(oid) != 0;
}

bool AssertionEnforcer::CannotEnforce(catalog::Oid oid) const {
    if (NothingDeclared()) return false;
    const LatchGuard guard(latch_.get());
    return unenforceable_.count(oid) != 0;
}

void AssertionEnforcer::PublishDeclaredLocked() noexcept {
    declared_.store(by_oid_.size() + unenforceable_.size(), std::memory_order_release);
}

std::size_t AssertionEnforcer::unenforceable() const {
    const LatchGuard guard(latch_.get());
    return unenforceable_.size();
}

std::optional<LiveAssertion::Counters> AssertionEnforcer::CountersOf(
    std::uint64_t assertion_id) const {
    const LatchGuard guard(latch_.get());
    auto it = live_.find(assertion_id);
    if (it == live_.end()) return std::nullopt;
    return it->second->a.counters;
}

std::vector<std::shared_ptr<AssertionEnforcer::Live>> AssertionEnforcer::OnLocked(
    catalog::Oid oid) const {
    std::vector<std::shared_ptr<Live>> out;
    auto on = by_oid_.find(oid);
    if (on == by_oid_.end()) return out;
    out.reserve(on->second.size());
    for (const std::uint64_t id : on->second) out.push_back(live_.at(id));
    return out;
}

std::vector<wal::AssertionCabinSnapshot> AssertionEnforcer::SnapshotLocked() const {
    // AS6a's base, one record's worth per cabin: headers only, keys owned, and
    // never the entry lists - those are O(all writes, forever) and are rebuilt
    // at recovery from the entries' own `group_id` instead. Never a hold
    // either: a hold is in no header until its record is appended.
    std::vector<wal::AssertionCabinSnapshot> out;
    out.reserve(live_.size());
    for (const auto& [assertion_id, live] : live_) {
        wal::AssertionCabinSnapshot cabin;
        cabin.assertion_id = assertion_id;
        for (const BoundCabin::GroupSnapshot& group : live->a.cabin.SnapshotGroups()) {
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

std::vector<wal::AssertionCabinSnapshot> AssertionEnforcer::SnapshotAssertions() const {
    const LatchGuard guard(latch_.get());
    return SnapshotLocked();
}

Status AssertionEnforcer::VisitSnapshots(const wal::SnapshotVisitor& visit) const {
    // Held across `visit`, which appends the records: directory latch then
    // WAL stream latch, the order every reservation takes them in too.
    const LatchGuard guard(latch_.get());
    return visit(SnapshotLocked());
}

void AssertionEnforcer::Adopt(LiveAssertion assertion) {
    auto live = std::make_shared<Live>();
    live->a = std::move(assertion);
    if (latch_ != nullptr) live->chain_latch = std::make_unique<Latch>();
    const std::uint64_t id = live->a.assertion_id;
    const catalog::Oid oid = live->a.target_oid;

    const LatchGuard guard(latch_.get());
    live_.insert_or_assign(id, std::move(live));
    auto& ids = by_oid_[oid];
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    // An id that is now enforced is no longer one the registry merely knows
    // about, whatever a previous mount concluded. Cleared per id rather
    // than per relation: a relation may carry both, and a write it admits
    // on the strength of this one would still be unchecked against the
    // other (PW1c-6c).
    if (auto stale = unenforceable_.find(oid); stale != unenforceable_.end()) {
        auto& blocked = stale->second;
        blocked.erase(std::remove(blocked.begin(), blocked.end(), id), blocked.end());
        if (blocked.empty()) unenforceable_.erase(stale);
    }
    PublishDeclaredLocked();
}

void AssertionEnforcer::NoteUnenforceable(catalog::Oid oid, std::uint64_t assertion_id) {
    const LatchGuard guard(latch_.get());
    // Never for something the registry is already enforcing: the two states
    // are exclusive, and the enforced one is the truthful answer.
    if (live_.count(assertion_id) != 0) return;
    auto& ids = unenforceable_[oid];
    if (std::find(ids.begin(), ids.end(), assertion_id) == ids.end()) {
        ids.push_back(assertion_id);
    }
    PublishDeclaredLocked();
}

void AssertionEnforcer::Evict(std::uint64_t assertion_id) {
    const LatchGuard guard(latch_.get());
    // The unenforceable record first, and **before the early return**: an
    // assertion that could not be enforced is exactly the one an operator
    // drops in order to re-create it, and leaving the record behind would
    // keep refusing the relation's writes for an assertion that no longer
    // exists. Nothing is in both maps - `NoteUnenforceable` refuses an id
    // `live_` holds - so this touches at most one of the two.
    for (auto blocked = unenforceable_.begin(); blocked != unenforceable_.end(); ++blocked) {
        auto& ids = blocked->second;
        const auto found = std::find(ids.begin(), ids.end(), assertion_id);
        if (found == ids.end()) continue;
        ids.erase(found);
        if (ids.empty()) unenforceable_.erase(blocked);
        break;
    }
    if (auto it = live_.find(assertion_id); it != live_.end()) {
        auto& ids = by_oid_[it->second->a.target_oid];
        ids.erase(std::remove(ids.begin(), ids.end(), assertion_id), ids.end());
        if (ids.empty()) by_oid_.erase(it->second->a.target_oid);
        live_.erase(it);
    }
    // Its holds stay until their `Hold`s release them, and count toward
    // nothing meanwhile: no admission asks about an evicted id. Pending
    // reservations stay in their transactions' lists and are skipped at
    // commit/abort by the lookup below - the same rule replay's skip has,
    // and for the same reason. A reservation already past `OnLocked`
    // finishes on its own `shared_ptr`, into a chain nothing names any more.
    PublishDeclaredLocked();
}

// ---- Holds (AT-S5d) -------------------------------------------------------

StatusOr<std::uint64_t> AssertionEnforcer::AdmitLocked(
    LiveAssertion& a, const std::string& key, std::optional<std::int64_t> check,
    std::int64_t hold, std::uint64_t txn_id, std::span<const parser::AstValue> row,
    std::size_t first_col_pos, std::uint64_t* reserver) {
    if (check.has_value()) {
        // Every held contribution to the group counts, **positive ones
        // only**: a negative one held would read as room to another core's
        // admission, and the statement that held it may still fail and take
        // the room back - a false admission, the one outcome §6.2 rules
        // out. Summed by a scan: `holds_` is bounded by the rows in flight,
        // one per statement per assertion.
        std::int64_t asked = *check;
        for (const auto& [serial, held] : holds_) {
            if (held.assertion_id != a.assertion_id || held.key != key || held.value <= 0) {
                continue;
            }
            if (__builtin_add_overflow(asked, held.value, &asked)) {
                return Status::OutOfRange("assertion \"" + a.name +
                                          "\": the held contributions and this row's overflow "
                                          "int64");
            }
        }
        ++a.counters.checks;
        auto admitted = a.cabin.Admit(key, asked);
        if (!admitted.ok()) return admitted.status();
        if (!admitted.value().admitted) {
            ++a.counters.violations;
            if (reserver != nullptr) *reserver = ReserverOnLocked(a.assertion_id, key, txn_id);
            return Refuse(a, row, first_col_pos);
        }
    }
    const std::uint64_t serial = ++next_hold_serial_;
    holds_.emplace(serial, HeldContribution{txn_id, a.assertion_id, key, hold});
    return serial;
}

void AssertionEnforcer::ReleaseHoldLocked(std::uint64_t serial) {
    holds_.erase(serial);  // a converted or never-taken serial is not there
}

void AssertionEnforcer::Hold::Release() {
    if (owner_ != nullptr && !items_.empty()) {
        const LatchGuard guard(owner_->latch_.get());
        for (const Item& item : items_) owner_->ReleaseHoldLocked(item.serial);
    }
    items_.clear();
}

std::uint64_t AssertionEnforcer::ReserverOnLocked(std::uint64_t assertion_id,
                                                  const std::string& key,
                                                  std::uint64_t exclude_txn) const {
    // **The candidate's *net* on this group, not any arrival on it.** An
    // `UPDATE` always writes the departure-and-arrival pair, so a
    // transaction that *lowered* the group by 49 would still be found by
    // its +1 arrival if arrivals alone were read - and waiting for that one
    // is worse than useless in both arms: its abort makes the group larger,
    // and the futile wait puts a live edge in the wait-for graph, where it
    // can make the innocent holder a deadlock victim.
    //
    // A positive net is not a proof that this transaction's decide admits
    // the waiter; several reservers may each have to go, and the re-run
    // meets the next one. It is a proof that its abort moves the group in
    // the direction the waiter needs, which is what the wait is for. A
    // hold counts as the arrival it is about to become.
    std::unordered_map<std::uint64_t, std::int64_t> net;
    for (const auto& [txn_id, reservations] : pending_) {
        if (txn_id == exclude_txn) continue;  // its own reservations are its own
        for (const Reservation& r : reservations) {
            if (r.assertion_id != assertion_id || r.key != key) continue;
            net[txn_id] += r.departure ? -r.value : r.value;
        }
    }
    for (const auto& [serial, held] : holds_) {
        if (held.txn_id == exclude_txn || held.assertion_id != assertion_id || held.key != key) {
            continue;
        }
        net[held.txn_id] += held.value;
    }
    for (const auto& [txn_id, sum] : net) {
        if (sum > 0) return txn_id;
    }
    return 0;
}

// ---- The write path -------------------------------------------------------

Status AssertionEnforcer::AdmitInsert(catalog::Oid oid,
                                      std::span<const parser::AstValue> values,
                                      std::uint64_t writer_txn, Hold& hold,
                                      std::uint64_t* reserver) {
    if (NothingDeclared()) return Status::OK();
    const LatchGuard guard(latch_.get());
    auto on = by_oid_.find(oid);
    if (on == by_oid_.end()) return Status::OK();
    hold.owner_ = this;
    // What this call held is given back if a later assertion refuses or
    // fails: a refused row holds nothing.
    const std::size_t before = hold.items_.size();
    for (const std::uint64_t id : on->second) {
        LiveAssertion& a = live_.at(id)->a;
        const std::int64_t contribution = ContributionOf(a, values, /*first_col_pos=*/1);
        auto serial = AdmitLocked(a, KeyFor(a, values, 1), contribution, contribution,
                                  writer_txn, values, 1, reserver);
        if (!serial.ok()) {
            for (std::size_t i = before; i < hold.items_.size(); ++i) {
                ReleaseHoldLocked(hold.items_[i].serial);
            }
            hold.items_.resize(before);
            return serial.status();
        }
        hold.items_.push_back(Hold::Item{serial.value(), a.assertion_id});
    }
    return Status::OK();
}

Status AssertionEnforcer::ReserveOne(storage::PageStore& store, wal::WalManager* wal,
                                     std::uint64_t txn_id, Live& live, const std::string& key,
                                     bool departure, std::int64_t value, std::uint64_t pk,
                                     PageId row_page, std::uint16_t row_slot,
                                     std::uint64_t serial) {
    LiveAssertion& a = live.a;
    // The chain is one writer's state: its tail and its growth. Held across
    // the page work below and nothing else serialises it.
    const LatchGuard chain(live.chain_latch.get());

    BoundCabinEntry entry;
    entry.pk = pk;
    entry.flags = static_cast<std::uint8_t>(kEntryReserved | kEntryHintValid |
                                            (departure ? kEntryDeparture : 0));
    entry.page_id = row_page;
    entry.page_epoch = 0;
    entry.slot = row_slot;
    entry.value = value;
    {
        // AS6a, and the departure case is why this is not `EnsureGroupId`
        // unconditionally: a departure's group must already exist (the row it
        // removes was incorporated by build or insert), and creating one here
        // would create a group to immediately go negative in - which
        // `ApplyDeparture` answers NotFound for, deliberately.
        const LatchGuard guard(latch_.get());
        if (departure) {
            const GroupHeader* header = a.cabin.Find(key);
            entry.group_id = header != nullptr ? header->group_id : 0;
        } else {
            entry.group_id = a.cabin.EnsureGroupId(key);
        }
    }

    // The page work, under the chain latch alone. The tail comes back held,
    // so it cannot reach the device before the record below describes it.
    auto placed = a.chain.Place(store, wal, entry);
    if (!placed.ok()) return placed.status();
    const PageId page_id = placed.value().page_id;
    const std::uint16_t index = placed.value().index;

    // The record, the header and the hold's conversion as one step against
    // a snapshot (the header's paragraph): no page is touched in here.
    wal::Lsn stamp = wal::kNoLsn;
    {
        const LatchGuard guard(latch_.get());
        auto logged =
            a.chain.Log(wal, placed.value(), entry, key, wal::RecordType::kAssertReserve, txn_id);
        if (!logged.ok()) return logged.status();
        stamp = logged.value();
        Status applied = departure ? a.cabin.ApplyDeparture(key, value, page_id, index)
                                   : a.cabin.Apply(key, value, page_id, index);
        if (!applied.ok()) return applied;
        if (serial != 0) ReleaseHoldLocked(serial);

        Reservation r;
        r.assertion_id = a.assertion_id;
        r.key = key;
        r.departure = departure;
        r.value = value;
        r.page = page_id;
        r.index = index;
        pending_[txn_id].push_back(std::move(r));
        ++a.counters.reserved;
    }
    if (stamp != wal::kNoLsn) return store.StampPageLsn(page_id, stamp);
    return Status::OK();
}

Status AssertionEnforcer::ReserveInsert(storage::PageStore& store, wal::WalManager* wal,
                                        std::uint64_t txn_id, Hold& hold, catalog::Oid oid,
                                        std::span<const parser::AstValue> values,
                                        std::uint64_t pk, PageId row_page,
                                        std::uint16_t row_slot) {
    struct Work {
        std::shared_ptr<Live> live;
        std::string key;
        std::int64_t value = 0;
        std::uint64_t serial = 0;
    };
    if (NothingDeclared()) return Status::OK();
    std::vector<Work> work;
    {
        const LatchGuard guard(latch_.get());
        for (std::shared_ptr<Live>& live : OnLocked(oid)) {
            LiveAssertion& a = live->a;
            Work w{live, KeyFor(a, values, 1), ContributionOf(a, values, 1), 0};
            const auto covered =
                std::find_if(hold.items_.begin(), hold.items_.end(),
                             [&](const Hold::Item& item) { return item.assertion_id == a.assertion_id; });
            if (hold.owner_ == this && covered != hold.items_.end()) {
                w.serial = covered->serial;
            } else {
                // Adopted after this row's admission: admitted here, and
                // held in `hold` so a failure below gives it back too. Not
                // waitable - the row is placed by now.
                auto serial = AdmitLocked(a, w.key, w.value, w.value, txn_id, values, 1,
                                          /*reserver=*/nullptr);
                if (!serial.ok()) return serial.status();
                w.serial = serial.value();
                hold.owner_ = this;
                hold.items_.push_back(Hold::Item{w.serial, a.assertion_id});
            }
            work.push_back(std::move(w));
        }
    }
    for (Work& w : work) {
        if (Status s = ReserveOne(store, wal, txn_id, *w.live, w.key, /*departure=*/false,
                                  w.value, pk, row_page, row_slot, w.serial);
            !s.ok()) {
            return s;  // `hold` gives back what was not converted
        }
    }
    hold.items_.clear();  // every one converted
    return Status::OK();
}

Status AssertionEnforcer::AdmitAndReserveUpdate(storage::PageStore& store, wal::WalManager* wal,
                                                std::uint64_t txn_id, catalog::Oid oid,
                                                std::span<const parser::AstValue> old_row,
                                                std::span<const parser::AstValue> new_row,
                                                std::uint64_t pk, PageId row_page,
                                                std::uint16_t row_slot,
                                                std::uint64_t* reserver) {
    const std::vector<std::shared_ptr<Live>> lives = [&] {
        const LatchGuard guard(latch_.get());
        return OnLocked(oid);
    }();
    // **Once this call has reserved, its refusal is not waitable** (the
    // AO-S6e-c review's B1). This loop is per *assertion* and is not atomic
    // across them: assertion #1's departure and arrival are applied to its
    // cabin and appended to its chain before assertion #2 is even asked. A
    // refusal used to poison the transaction and the mandatory `ROLLBACK`
    // unapplied them; a wait re-runs the whole statement instead, and
    // assertion #1 is reserved a **second** time.
    //
    // `EndWrite`'s re-runnability test cannot see it: that test reads the
    // transaction's *trail*, and a reservation never enters the trail -
    // only heap and var-heap mutations do. So a statement that moved a
    // Bound Cabin and wrote no row reads as "wrote nothing" and is judged
    // re-runnable when it is not, and the double count is durable: both
    // entries are `kAssertReserve` records, `header == Σ(entries)` still
    // holds, and a rebuild reproduces the wrong aggregate after a mount.
    //
    // Withheld here rather than taught to `EndWrite`, which would need the
    // enforcer's pending count marked at every statement boundary: this is
    // the one call that reserves before it can refuse, and the honest
    // answer for it is the violation it always gave.
    bool reserved_any = false;
    for (const std::shared_ptr<Live>& live : lives) {
        LiveAssertion& a = live->a;
        const std::string old_key = KeyFor(a, old_row, 0);
        const std::string new_key = KeyFor(a, new_row, 0);
        const std::int64_t old_contrib = ContributionOf(a, old_row, 0);
        const std::int64_t new_contrib = ContributionOf(a, new_row, 0);
        // §4.2 row 2: the aggregate is invariant - nothing at all.
        if (old_key == new_key && old_contrib == new_contrib) continue;

        // What is checked. Row 3, same group and only the SUM moved: the
        // delta, and only if it is positive, because a decrease cannot
        // violate an upper bound. Row 4, the group moved: the arrival at its
        // full contribution - leaving one group buys nothing in another.
        std::optional<std::int64_t> check;
        if (old_key == new_key) {
            std::int64_t delta = 0;
            if (__builtin_sub_overflow(new_contrib, old_contrib, &delta)) {
                return Status::OutOfRange("assertion \"" + a.name +
                                          "\": SUM delta overflows int64");
            }
            if (delta > 0) check = delta;
        } else {
            check = new_contrib;
        }

        // What is held is the arrival's **full** contribution whatever was
        // checked. The departure below is applied at its own record, ahead of
        // the arrival's, and a header lowered by it with nothing held for the
        // arrival would read low to another core's admission in between -
        // room this row takes back a moment later. Held in full, the group
        // reads high by the departing value until the departure lands:
        // conservative, and never a false admission.
        Hold hold;
        {
            const LatchGuard guard(latch_.get());
            auto serial = AdmitLocked(a, new_key, check, new_contrib, txn_id, new_row, 0,
                                      reserved_any ? nullptr : reserver);
            if (!serial.ok()) return serial.status();
            hold.owner_ = this;
            hold.items_.push_back(Hold::Item{serial.value(), a.assertion_id});
        }

        // The mutation is uniform whatever the case above decided: the old
        // contribution leaves, the new one arrives - and both halves are
        // entries, so header == Σ(entries) survives.
        if (Status s = ReserveOne(store, wal, txn_id, *live, old_key, /*departure=*/true,
                                  old_contrib, pk, row_page, row_slot, /*serial=*/0);
            !s.ok()) {
            return s;
        }
        if (Status s = ReserveOne(store, wal, txn_id, *live, new_key, /*departure=*/false,
                                  new_contrib, pk, row_page, row_slot, hold.items_.front().serial);
            !s.ok()) {
            return s;
        }
        hold.items_.clear();  // converted
        reserved_any = true;
    }
    return Status::OK();
}

Status AssertionEnforcer::ReserveDelete(storage::PageStore& store, wal::WalManager* wal,
                                        std::uint64_t txn_id, catalog::Oid oid,
                                        std::span<const parser::AstValue> old_row,
                                        std::uint64_t pk, PageId row_page,
                                        std::uint16_t row_slot) {
    const std::vector<std::shared_ptr<Live>> lives = [&] {
        const LatchGuard guard(latch_.get());
        return OnLocked(oid);
    }();
    for (const std::shared_ptr<Live>& live : lives) {
        if (Status s = ReserveOne(store, wal, txn_id, *live, KeyFor(live->a, old_row, 0),
                                  /*departure=*/true, ContributionOf(live->a, old_row, 0), pk,
                                  row_page, row_slot, /*serial=*/0);
            !s.ok()) {
            return s;
        }
    }
    return Status::OK();
}

Status AssertionEnforcer::CommitTxn(storage::PageStore& store, wal::WalManager* wal,
                                    std::uint64_t txn_id) {
    // Batched per (assertion, page), the physiological unit ASSERT_COMMIT
    // describes. Built under the latch, settled outside it: a commit moves
    // no header, so its records need no atomicity against a snapshot, and
    // its page work may not run under the directory latch.
    std::map<std::pair<std::uint64_t, PageId>, std::vector<std::uint16_t>> by_page;
    {
        const LatchGuard guard(latch_.get());
        auto pending = pending_.find(txn_id);
        if (pending == pending_.end()) return Status::OK();

        // **Taken out of the map before anything can fail**, so a
        // transaction's reservations are settled exactly once whatever
        // happens below. Every exit from here is an error return, and leaving
        // the list behind on one means the *next* transaction under this id
        // settles them a second time - reachable, because
        // `CommandDispatcher::EndWrite` uses `catalog::kBootstrapXid` for
        // every statement when there is no transaction manager, so the id is
        // not unique per transaction there. Double-settling silently moves an
        // aggregate twice; dropping the remainder of a failed settle leaves it
        // wrong in a way the returned error already reports. The second is
        // the failure to prefer.
        const std::vector<Reservation> reservations = std::move(pending->second);
        pending_.erase(pending);
        for (const Reservation& r : reservations) {
            if (live_.count(r.assertion_id) == 0) continue;  // dropped mid-transaction
            by_page[{r.assertion_id, r.page}].push_back(r.index);
        }
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
    std::vector<Reservation> reservations;
    {
        const LatchGuard guard(latch_.get());
        auto pending = pending_.find(txn_id);
        if (pending == pending_.end()) return Status::OK();
        // Taken out of the map first, for the reason `CommitTxn` states: every
        // exit below is an error return, and a reservation left pending is one
        // the next transaction under this id would settle a second time.
        reservations = std::move(pending->second);
        pending_.erase(pending);
    }

    // Reverse order, the undo trail's own convention - not load-bearing for
    // these (removal is by name), but it keeps the compensation stream
    // reading as an unwind.
    for (auto it = reservations.rbegin(); it != reservations.rend(); ++it) {
        // **Log, then mark, then stamp** - WAL-before-data, and here the order
        // is the correctness argument rather than a convention. Marking first
        // leaves a window where the frame carries its *old*, already-durable
        // `page_lsn`, so the flush gate would let the mark out ahead of the
        // record describing it; a failing `Append` then makes that permanent,
        // and the next recovery meets a header counting a reservation whose
        // entry the page says is orphaned - `VerifyAgainstEntries` reporting
        // Corruption for a directory that this time really is wrong.
        //
        // `Unapply` stays ahead of both: its missing-pair refusal is the check
        // that proves `(page, index)` names *this* group's entry, and it must
        // run before a flag is OR'd into whatever lives at that slot. It and
        // the record are one step under the directory latch, for the snapshot
        // (the header's paragraph); the mark is page work and runs after.
        wal::Lsn stamp = wal::kNoLsn;
        {
            const LatchGuard guard(latch_.get());
            auto held = live_.find(it->assertion_id);
            if (held == live_.end()) continue;  // dropped mid-transaction
            LiveAssertion& a = held->second->a;
            Status undone = it->departure
                                ? a.cabin.UnapplyDeparture(it->key, it->value, it->page, it->index)
                                : a.cabin.Unapply(it->key, it->value, it->page, it->index);
            if (!undone.ok()) return undone;
            ++a.counters.aborted;

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
