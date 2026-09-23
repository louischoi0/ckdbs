#include "kds/stats/cabin_store.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "kds/stats/optimizer_signals.hpp"

namespace kds::stats {

std::size_t CabinKeyHash::operator()(const CabinKey& key) const noexcept {
    // The same odd-multiplier fold InstanceKeyHash uses, and deliberately
    // **not** the FNV-1a that produces a fingerprint: this value never goes
    // on disk - it selects a bucket - so it carries none of the stability
    // obligations a persisted hash does, and tying it to one would invite
    // someone to persist it later.
    std::uint64_t h = key.cabin_id * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<std::uint64_t>(key.type) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h ^= static_cast<std::uint64_t>(key.int_val) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    // The string is folded in whatever the value type: a key's equality
    // compares every field, so its hash has to depend on every field or two
    // unequal keys land in one bucket and pay a compare each.
    for (const char c : key.str_val) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 0x100000001B3ull;
    }
    return static_cast<std::size_t>(h);
}

std::optional<CabinKey> MakeValueKey(const parser::AstValue& value) {
    switch (value.type) {
        case parser::ValueType::kInt: {
            CabinKey key;
            key.type = value.type;
            key.int_val = value.int_val;
            return key;
        }
        case parser::ValueType::kStr: {
            CabinKey key;
            key.type = value.type;
            key.str_val = value.str_val;
            return key;
        }
        case parser::ValueType::kDecimal: {
            // The unscaled integer *and* the scale, because the pair is the
            // value: 1234 at scale 2 and 1234 at scale 3 are different
            // numbers, and a Cabin keyed on the integer alone would serve
            // one value's entry set for the other. Two columns of different
            // scale cannot share a Cabin anyway (a Cabin is per column), so
            // this is belt and braces - and cheap.
            CabinKey key;
            key.type = value.type;
            key.int_val = value.int_val;
            key.str_val = std::to_string(value.scale);
            return key;
        }
        case parser::ValueType::kDecimalWide: {
            // The wide kind's high half joins the scale in the string
            // field - `int_val` holds the low 64 bits and cannot carry
            // both. The kind is part of the key, so a wide value can never
            // collide with a narrow one whatever the strings say.
            CabinKey key;
            key.type = value.type;
            key.int_val = value.int_val;
            key.str_val = std::to_string(value.scale) + "," + std::to_string(value.dec_hi);
            return key;
        }
        case parser::ValueType::kNull:
        case parser::ValueType::kParam:
            // Refused, for the reasons the header gives; kParam is
            // unreachable besides, since declared patterns were withdrawn
            // (ast.hpp). Both are silent refusals rather than errors: a
            // value that cannot be observed simply takes the scan path,
            // which is what it would have done if no Cabin existed.
            return std::nullopt;
    }
    return std::nullopt;
}

std::optional<CabinKey> MakeCabinKey(std::uint64_t cabin_id, const parser::AstValue& value) {
    if (cabin_id == 0) return std::nullopt;
    std::optional<CabinKey> key = MakeValueKey(value);
    if (key.has_value()) key->cabin_id = cabin_id;
    return key;
}

// ---- The handle ---------------------------------------------------------

CabinEntry CabinSet::At(std::size_t i) const {
    if (store_ == nullptr || entries_ == nullptr || i >= count_) return CabinEntry{};
    // The partition's latch, for the entry's own fields: a heal writes them
    // and a plain read beside it would be a data race. The `shared_ptr` is
    // what keeps the set alive if the store has dropped it meanwhile, which
    // is why this is safe to take at all.
    std::lock_guard<std::mutex> hold(store_->partitions_[partition_].latch);
    return (*entries_)[i];
}

void CabinSet::Heal(std::size_t i, PageId page_id, std::uint16_t slot,
                    std::uint32_t page_epoch) const {
    if (store_ == nullptr || entries_ == nullptr || i >= count_) return;
    std::lock_guard<std::mutex> hold(store_->partitions_[partition_].latch);
    CabinEntry& entry = (*entries_)[i];
    entry.page_id = page_id;
    entry.slot = slot;
    entry.page_epoch = page_epoch;
    entry.flags |= kCabinHintValid;
}

// ---- The store ----------------------------------------------------------

CabinSet CabinStore::Find(const CabinKey& key) {
    const std::size_t index = PartitionOf(key.cabin_id);
    Partition& part = partitions_[index];
    std::lock_guard<std::mutex> hold(part.latch);
    auto it = part.observed.find(key);
    if (it == part.observed.end()) return CabinSet{};
    // **An announced build is present and unservable** (AT-S7). Its set
    // holds whatever the write hook has appended since the announce and
    // none of the walk's own matches yet, so it is not a superset of
    // anything and serving it would be the C1 break in its purest form.
    // Answering "not observed" is exactly right: the walk is what answers
    // this probe, which is what the probe that announced the build is
    // already doing.
    if (part.building.find(key) != part.building.end()) return CabinSet{};
    // The count is fixed here; `CabinSet`'s declaration says why an entry
    // appended after it is not a row this reader missed.
    return CabinSet{this, index, it->second, it->second->size()};
}

void CabinStore::NoteHit(std::uint64_t cabin_id) {
    {
        Partition& part = PartitionFor(cabin_id);
        std::lock_guard<std::mutex> hold(part.latch);
        ++part.info[cabin_id].hits;
    }
    {
        std::lock_guard<std::mutex> hold(stats_latch_);
        ++stats_.hits;
    }
    // Outside both latches: the signals sink is another structure with its
    // own rules, and calling into it under ours would order two latches
    // nothing else orders.
    if (signals_ != nullptr) signals_->NoteCabinLookup(cabin_id, /*served=*/true);
}

void CabinStore::NoteMiss(std::uint64_t cabin_id) {
    {
        Partition& part = PartitionFor(cabin_id);
        std::lock_guard<std::mutex> hold(part.latch);
        ++part.info[cabin_id].misses;
    }
    {
        std::lock_guard<std::mutex> hold(stats_latch_);
        ++stats_.misses;
    }
    if (signals_ != nullptr) signals_->NoteCabinLookup(cabin_id, /*served=*/false);
}

void CabinStore::NoteHint(std::uint64_t cabin_id, bool ok) {
    if (signals_ != nullptr) signals_->NoteCabinHint(cabin_id, ok);
}

bool CabinStore::MayObserve(const CabinKey& key) const {
    const Partition& part = PartitionFor(key.cabin_id);
    std::lock_guard<std::mutex> hold(part.latch);
    if (part.observed.contains(key)) return true;  // a heal replaces in place
    if (part.entry_capped.contains(key)) return false;
    auto info = part.info.find(key.cabin_id);
    return info == part.info.end() || info->second.values < limits_.max_values;
}

void CabinStore::NoteCapRefusal() {
    std::lock_guard<std::mutex> hold(stats_latch_);
    ++stats_.cap_refusals;
}

void CabinStore::NoteUnbankableView() {
    std::lock_guard<std::mutex> hold(stats_latch_);
    ++stats_.unbankable_views;
}

void CabinStore::NoteEntryCapRefusal(const CabinKey& key) {
    {
        Partition& part = PartitionFor(key.cabin_id);
        std::lock_guard<std::mutex> hold(part.latch);
        part.entry_capped.insert(key);
        part.sightings.erase(key);
    }
    std::lock_guard<std::mutex> hold(stats_latch_);
    ++stats_.cap_refusals;
}

CabinStore::Stats CabinStore::stats() const {
    std::lock_guard<std::mutex> hold(stats_latch_);
    return stats_;
}

std::size_t CabinStore::observed_value_count() const {
    std::size_t n = 0;
    for (const Partition& part : partitions_) {
        std::lock_guard<std::mutex> hold(part.latch);
        n += part.observed.size();
    }
    return n;
}

std::uint8_t CabinStore::Observe(const CabinKey& key) {
    bool cleared = false;
    std::uint8_t count = 0;
    {
        Partition& part = PartitionFor(key.cabin_id);
        std::lock_guard<std::mutex> hold(part.latch);
        if (part.sightings.size() >= kMaxSightings &&
            part.sightings.find(key) == part.sightings.end()) {
            // Wholesale, exactly as TrailRecorder does it: eviction here
            // restarts counting and nothing else, so the crudest policy is
            // the right one until something measures otherwise.
            //
            // **Per partition since AT-S7**, which is the same policy at a
            // sixteenth of the blast radius: the cap it answers is this
            // partition's table, and clearing every partition's would evict
            // cabins that are not near it.
            part.sightings.clear();
            // The entry-cap marks ride the same crude eviction: a wholesale
            // reset is the store's one "the world may have changed" signal.
            part.entry_capped.clear();
            cleared = true;
        }
        std::uint8_t& held = part.sightings[key];
        if (held != std::numeric_limits<std::uint8_t>::max()) ++held;
        count = held;
    }
    if (cleared) {
        std::lock_guard<std::mutex> hold(stats_latch_);
        ++stats_.sighting_clears;
    }
    return count;
}

bool CabinStore::BeginRecording(const CabinKey& key) {
    Partition& part = PartitionFor(key.cabin_id);
    std::lock_guard<std::mutex> hold(part.latch);
    // Already observed, or already being built by another core's probe.
    // Both are "do not walk-and-commit": the first is the heal path, which
    // un-observes before it re-records, and the second would have two
    // walks merging into one set with no way to tell whose matches are
    // whose.
    if (part.observed.find(key) != part.observed.end()) return false;
    if (part.building.find(key) != part.building.end()) return false;
    // The per-cabin value cap, taken **here** rather than at the commit:
    // an announced set occupies a value slot from this instant, so two
    // builds cannot both pass a cap with one slot left.
    if (part.info[key.cabin_id].values >= limits_.max_values) return false;

    part.observed.emplace(key, std::make_shared<CabinEntrySet>());
    part.building.emplace(key, true);
    AddSetLocked(part, key.cabin_id, /*entries=*/0);
    return true;
}

void CabinStore::CancelRecording(const CabinKey& key) {
    Partition& part = PartitionFor(key.cabin_id);
    std::lock_guard<std::mutex> hold(part.latch);
    auto mark = part.building.find(key);
    if (mark == part.building.end()) return;
    part.building.erase(mark);
    if (auto it = part.observed.find(key); it != part.observed.end()) {
        RemoveSetLocked(part, key.cabin_id, it->second->size());
        part.observed.erase(it);
    }
}

bool CabinStore::Commit(const CabinKey& key, std::vector<CabinEntry> entries) {
    enum class Counted { kNone, kCapRefusal, kRecording };
    Counted counted = Counted::kNone;
    bool accepted = false;
    {
        Partition& part = PartitionFor(key.cabin_id);
        std::lock_guard<std::mutex> hold(part.latch);

        // **The announced build's half** (AT-S7). Its set already holds
        // every write the hook took during the walk, from whichever core
        // made it, so the walk's matches are *added* to it rather than
        // replacing it - a surplus entry is legal (§1) and a missing one
        // is the break. The mark is cleared either way: an announce that
        // reaches here is over.
        if (auto mark = part.building.find(key); mark != part.building.end()) {
            const bool good = mark->second;
            part.building.erase(mark);
            auto it = part.observed.find(key);
            // Unreachable: `BeginRecording` installs the set and only
            // `CancelRecording` and this line remove it while the mark
            // stands. Handled rather than asserted, because banking here
            // would bank a set with no appends in it.
            if (it == part.observed.end()) return false;
            if (!good || it->second->size() + entries.size() > limits_.max_entries_per_value) {
                // Either the store already said it could not keep this
                // value - a cap or an `Unobserve` during the walk - or the
                // merged set is over the per-value cap, which §1 answers by
                // refusing to observe rather than truncating.
                RemoveSetLocked(part, key.cabin_id, it->second->size());
                part.observed.erase(it);
                part.sightings.erase(key);
                counted = Counted::kCapRefusal;
            } else {
                const std::size_t added = entries.size();
                it->second->insert(it->second->end(), entries.begin(), entries.end());
                part.info[key.cabin_id].entries += added;
                ++part.info[key.cabin_id].recordings;
                part.sightings.erase(key);
                counted = Counted::kRecording;
                accepted = true;
            }
        } else
        // Re-observing a value that is already observed replaces its set.
        // That is the **heal** path (spec §4's heap fallback), and it is
        // sound for the same reason the first recording is: the set comes
        // from a completed authoritative walk, so it is a superset of what
        // is visible. A reader holding the old set walks on over it - the
        // `shared_ptr` keeps it alive - and what it walks is a superset
        // too, which is the only property a reader depends on.
        if (auto existing = part.observed.find(key); existing != part.observed.end()) {
            RemoveSetLocked(part, key.cabin_id, existing->second->size());
            part.observed.erase(existing);
        } else if (part.info[key.cabin_id].values >= limits_.max_values) {
            // At the per-cabin value cap. **Refuse to observe** - never
            // observe a partial set, and never evict some other value to
            // make room: the second would be a policy decision §8 has not
            // made, and this one is reversible by the next execution.
            counted = Counted::kCapRefusal;
        }

        if (counted == Counted::kNone) {
            if (entries.size() > limits_.max_entries_per_value) {
                part.sightings.erase(key);
                counted = Counted::kCapRefusal;
            } else {
                const std::size_t n = entries.size();
                auto set = std::make_shared<CabinEntrySet>(entries.begin(), entries.end());
                part.observed.emplace(key, std::move(set));
                AddSetLocked(part, key.cabin_id, n);
                ++part.info[key.cabin_id].recordings;
                // The value is observed now, so its sighting count has no
                // further use - and leaving it would hold a slot in a table
                // bounded by burst width.
                part.sightings.erase(key);
                counted = Counted::kRecording;
                accepted = true;
            }
        }
    }
    std::lock_guard<std::mutex> hold(stats_latch_);
    if (counted == Counted::kCapRefusal) ++stats_.cap_refusals;
    if (counted == Counted::kRecording) ++stats_.recordings;
    return accepted;
}

void CabinStore::Rebuild(const CabinKey& key, const CabinSet& viewed,
                         std::vector<CabinEntry> entries) {
    Partition& part = PartitionFor(key.cabin_id);
    std::lock_guard<std::mutex> hold(part.latch);
    auto it = part.observed.find(key);
    if (it == part.observed.end()) return;
    // **The same storage the heal walked, or nothing.** An `Unobserve` and
    // a re-record between the walk and this call leave a different set
    // under this key, and replacing its first entries with a rebuild of
    // the old one would drop live pks it holds there. Identity, not size:
    // two sets of one length are not one set.
    //
    // **An announced build is covered by the same test and needs no second
    // one.** `Find` hands out no handle for one, so no caller can be
    // holding its storage; and a set that *was* handed out cannot become
    // an announce without an `Unobserve` first, which takes it out of
    // `observed` and fails this test anyway.
    if (it->second != viewed.entries_) return;
    CabinEntrySet& held = *it->second;
    if (viewed.size() > held.size()) return;

    // **A new storage, never a shrink of the old one.** A reader's
    // `CabinSet` holds the set alive through its `shared_ptr` and indexes
    // it by a count taken before this ran; shrinking that deque under it
    // would index past its end. Installing a fresh one leaves that reader
    // walking exactly what it took, which is still a superset - the only
    // property a reader depends on - and is the same discipline `Commit`'s
    // heal arm follows.
    auto fresh = std::make_shared<CabinEntrySet>(entries.begin(), entries.end());
    fresh->insert(fresh->end(), held.begin() + static_cast<std::ptrdiff_t>(viewed.size()),
                  held.end());
    // At the cap the set is left as it is rather than truncated or
    // un-observed: what stands is a superset, and a heal is maintenance
    // that may always decline (§1's corollary).
    if (fresh->size() > limits_.max_entries_per_value) return;

    // Not a set arriving or leaving - the value keeps its slot - so
    // `values` does not move and only the entry count is restated.
    CabinInfo& info = part.info[key.cabin_id];
    info.entries -= std::min(info.entries, held.size());
    info.entries += fresh->size();
    it->second = std::move(fresh);
}

void CabinStore::Unobserve(const CabinKey& key) {
    bool dropped = false;
    {
        Partition& part = PartitionFor(key.cabin_id);
        std::lock_guard<std::mutex> hold(part.latch);
        auto it = part.observed.find(key);
        if (it != part.observed.end()) {
            RemoveSetLocked(part, key.cabin_id, it->second->size());
            part.observed.erase(it);
            dropped = true;
        }
        // An announced build whose set is dropped here is no longer good.
        // The mark stays so its `Commit` refuses; erasing it would let the
        // commit take the "not building" arm and bank the walk's matches
        // alone, without the appends this just discarded (AT-S7).
        if (auto mark = part.building.find(key); mark != part.building.end()) {
            mark->second = false;
        }
        if (!dropped) return;
        // The sighting count goes too. A value that was just un-observed
        // for a failure should have to earn its way back through the same
        // n=2 the first recording paid, rather than re-recording on the
        // next execution. The entry-cap mark goes with it: the heal path is
        // the one signal the world changed under this key.
        part.sightings.erase(key);
        part.entry_capped.erase(key);
    }
    std::lock_guard<std::mutex> hold(stats_latch_);
    ++stats_.unobserved;
}

namespace {

// Deterministic worklist order: a build's Commit sequence and a heal's
// walk order must not depend on hash-map iteration.
bool CabinKeyLess(const CabinKey& a, const CabinKey& b) noexcept {
    if (a.type != b.type) return a.type < b.type;
    if (a.int_val != b.int_val) return a.int_val < b.int_val;
    return a.str_val < b.str_val;
}

}  // namespace

std::vector<CabinKey> CabinStore::SightedUnobservedOf(std::uint64_t cabin_id) const {
    std::vector<CabinKey> keys;
    const Partition& part = PartitionFor(cabin_id);
    std::lock_guard<std::mutex> hold(part.latch);
    for (const auto& [key, count] : part.sightings) {
        if (key.cabin_id != cabin_id) continue;
        if (part.observed.find(key) != part.observed.end()) continue;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end(), CabinKeyLess);
    return keys;
}

std::vector<CabinKey> CabinStore::ObservedValuesOf(std::uint64_t cabin_id) const {
    std::vector<CabinKey> keys;
    const Partition& part = PartitionFor(cabin_id);
    std::lock_guard<std::mutex> hold(part.latch);
    for (const auto& [key, entries] : part.observed) {
        if (key.cabin_id == cabin_id) keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end(), CabinKeyLess);
    return keys;
}

std::size_t CabinStore::Discard(std::uint64_t cabin_id) {
    std::size_t sets = 0;
    Partition& part = PartitionFor(cabin_id);
    std::lock_guard<std::mutex> hold(part.latch);
    for (auto it = part.observed.begin(); it != part.observed.end();) {
        if (it->first.cabin_id != cabin_id) {
            ++it;
            continue;
        }
        ++sets;
        it = part.observed.erase(it);
    }
    for (auto it = part.sightings.begin(); it != part.sightings.end();) {
        it = it->first.cabin_id == cabin_id ? part.sightings.erase(it) : std::next(it);
    }
    for (auto it = part.entry_capped.begin(); it != part.entry_capped.end();) {
        it = it->cabin_id == cabin_id ? part.entry_capped.erase(it) : std::next(it);
    }
    // **An announced build loses its set above and keeps its mark** (AT-S7),
    // set false so its `Commit` refuses. Erasing the mark instead would let
    // the commit take the un-announced arm and bank the walk's matches over
    // a discard whose whole point is that they are no longer trustworthy -
    // CC10's pre-grant discard being exactly that case.
    for (auto& [key, good] : part.building) {
        if (key.cabin_id == cabin_id) good = false;
    }
    // Every one of this Cabin's sets has gone, so the two live figures are
    // zero exactly - not decremented set by set, which would be the same
    // arithmetic done in a way that can drift. `RemoveSet`'s saturation
    // exists for counters that have drifted; this one cannot.
    if (auto info = part.info.find(cabin_id); info != part.info.end()) {
        info->second.values = 0;
        info->second.entries = 0;
    }
    return sets;
}

void CabinStore::Forget(std::uint64_t cabin_id) {
    Discard(cabin_id);
    Partition& part = PartitionFor(cabin_id);
    std::lock_guard<std::mutex> hold(part.latch);
    // The announce marks go too: the Cabin is gone, so the walks that made
    // them will find no `sys.cabins` row to commit against and the marks
    // would be the one thing of this Cabin that outlived it. `Commit` on a
    // key with no mark and no set banks a fresh one, which is the same
    // answer a probe compiled before the DROP already gets.
    for (auto it = part.building.begin(); it != part.building.end();) {
        it = it->first.cabin_id == cabin_id ? part.building.erase(it) : std::next(it);
    }
    part.info.erase(cabin_id);
}

void CabinStore::NoteScopeDecline(std::uint64_t cabin_id) {
    {
        Partition& part = PartitionFor(cabin_id);
        std::lock_guard<std::mutex> hold(part.latch);
        ++part.info[cabin_id].scope_declines;
    }
    std::lock_guard<std::mutex> hold(stats_latch_);
    ++stats_.scope_declines;
}

void CabinStore::NoteWrite(const CabinKey& key, const CabinEntry& entry) {
    enum class Counted { kNone, kAppend, kCap };
    Counted counted = Counted::kNone;
    {
        Partition& part = PartitionFor(key.cabin_id);
        std::lock_guard<std::mutex> hold(part.latch);
        auto it = part.observed.find(key);
        // **The common case, and the whole reason the hook is affordable**:
        // the value is not observed, so there is nothing this write can
        // invalidate. One hash probe per cabin per write.
        if (it == part.observed.end()) return;

        if (it->second->size() >= limits_.max_entries_per_value) {
            // Un-observe rather than drop the append. Dropping it would
            // leave a set marked authoritative that is missing a row - the
            // exact break C1 forbids - where un-observing costs a scan
            // (§1's corollary).
            RemoveSetLocked(part, key.cabin_id, it->second->size());
            part.observed.erase(it);
            part.sightings.erase(key);
            // An announced build loses its set here, so its commit must
            // not bank one: the mark stays, saying the build is no longer
            // good (AT-S7).
            if (auto mark = part.building.find(key); mark != part.building.end()) {
                mark->second = false;
            }
            counted = Counted::kCap;
        } else {
            // **Appended, and a reader walking this set is unharmed**: a
            // `deque` never moves an entry it already holds, and a
            // `CabinSet`'s count was fixed before this append, so the
            // reader neither sees a moved entry nor a row younger than its
            // own snapshot.
            it->second->push_back(entry);
            ++part.info[key.cabin_id].entries;
            ++part.info[key.cabin_id].appends;
            counted = Counted::kAppend;
        }
    }
    std::lock_guard<std::mutex> hold(stats_latch_);
    if (counted == Counted::kAppend) ++stats_.appends;
    if (counted == Counted::kCap) {
        ++stats_.unobserved;
        ++stats_.cap_refusals;
    }
}

CabinStore::CabinInfo CabinStore::InfoFor(std::uint64_t cabin_id) const {
    const Partition& part = PartitionFor(cabin_id);
    std::lock_guard<std::mutex> hold(part.latch);
    auto it = part.info.find(cabin_id);
    return it == part.info.end() ? CabinInfo{} : it->second;
}

// **`Locked` in the name, and it is the contract**: both are called from
// inside a partition's latch and take neither, which is how the one place
// that maintains `values`/`entries` stays one place now that every caller
// holds a latch of its own (AT-S7).
void CabinStore::AddSetLocked(Partition& part, std::uint64_t cabin_id, std::size_t entries) {
    CabinInfo& info = part.info[cabin_id];
    ++info.values;
    info.entries += entries;
}

void CabinStore::RemoveSetLocked(Partition& part, std::uint64_t cabin_id, std::size_t entries) {
    CabinInfo& info = part.info[cabin_id];
    if (info.values > 0) --info.values;
    // Saturating, so a counter that has somehow drifted cannot wrap into a
    // gigantic number and make a Cabin look full. These are inspection
    // figures; being approximately right beats being spectacularly wrong.
    info.entries -= std::min(info.entries, entries);
}

}  // namespace kds::stats
