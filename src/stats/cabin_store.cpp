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
            // Refused, for the reasons the header gives. Both are silent
            // refusals rather than errors: a value that cannot be observed
            // simply takes the scan path, which is what it would have done
            // if no Cabin existed.
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

std::vector<CabinEntry>* CabinStore::Find(const CabinKey& key) {
    auto it = observed_.find(key);
    return it == observed_.end() ? nullptr : &it->second;
}

void CabinStore::NoteHit(std::uint64_t cabin_id) {
    ++stats_.hits;
    ++info_[cabin_id].hits;
    if (signals_ != nullptr) signals_->NoteCabinLookup(cabin_id, /*served=*/true);
}

void CabinStore::NoteMiss(std::uint64_t cabin_id) {
    ++stats_.misses;
    ++info_[cabin_id].misses;
    if (signals_ != nullptr) signals_->NoteCabinLookup(cabin_id, /*served=*/false);
}

void CabinStore::NoteHint(std::uint64_t cabin_id, bool ok) {
    if (signals_ != nullptr) signals_->NoteCabinHint(cabin_id, ok);
}

std::uint8_t CabinStore::Observe(const CabinKey& key) {
    if (sightings_.size() >= kMaxSightings && sightings_.find(key) == sightings_.end()) {
        // Wholesale, exactly as TrailRecorder does it: eviction here
        // restarts counting and nothing else, so the crudest policy is the
        // right one until something measures otherwise.
        sightings_.clear();
        // The entry-cap marks ride the same crude eviction: a wholesale
        // reset is the store's one "the world may have changed" signal.
        entry_capped_.clear();
        ++stats_.sighting_clears;
    }
    std::uint8_t& count = sightings_[key];
    if (count != std::numeric_limits<std::uint8_t>::max()) ++count;
    return count;
}

bool CabinStore::Commit(const CabinKey& key, std::vector<CabinEntry> entries) {
    // Re-observing a value that is already observed replaces its set. That
    // is the **heal** path (spec §4's heap fallback), and it is sound for
    // the same reason the first recording is: the set comes from a completed
    // authoritative walk, so it is a superset of what is visible.
    if (auto existing = observed_.find(key); existing != observed_.end()) {
        RemoveSet(key.cabin_id, existing->second.size());
        observed_.erase(existing);
    } else if (info_[key.cabin_id].values >= limits_.max_values) {
        // At the per-cabin value cap. **Refuse to observe** - never observe
        // a partial set, and never evict some other value to make room: the
        // second would be a policy decision §8 has not made, and this one is
        // reversible by the next execution.
        ++stats_.cap_refusals;
        return false;
    }

    if (entries.size() > limits_.max_entries_per_value) {
        ++stats_.cap_refusals;
        // Deliberately *not* truncated. A truncated set marked observed is
        // missing qualifying pks, which is the one thing the invariant
        // forbids outright (header rule 2).
        sightings_.erase(key);
        return false;
    }

    const std::size_t n = entries.size();
    observed_.emplace(key, std::move(entries));
    AddSet(key.cabin_id, n);
    ++stats_.recordings;
    ++info_[key.cabin_id].recordings;
    // The value is observed now, so its sighting count has no further use -
    // and leaving it would hold a slot in a table bounded by burst width.
    sightings_.erase(key);
    return true;
}

void CabinStore::Unobserve(const CabinKey& key) {
    auto it = observed_.find(key);
    if (it == observed_.end()) return;
    RemoveSet(key.cabin_id, it->second.size());
    observed_.erase(it);
    // The sighting count goes too. A value that was just un-observed for a
    // failure should have to earn its way back through the same n=2 the
    // first recording paid, rather than re-recording on the next execution.
    // The entry-cap mark goes with it: the heal path is the one signal the
    // world changed under this key.
    sightings_.erase(key);
    entry_capped_.erase(key);
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
    for (const auto& [key, count] : sightings_) {
        if (key.cabin_id != cabin_id) continue;
        if (observed_.find(key) != observed_.end()) continue;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end(), CabinKeyLess);
    return keys;
}

std::vector<CabinKey> CabinStore::ObservedValuesOf(std::uint64_t cabin_id) const {
    std::vector<CabinKey> keys;
    for (const auto& [key, entries] : observed_) {
        if (key.cabin_id == cabin_id) keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end(), CabinKeyLess);
    return keys;
}

void CabinStore::Forget(std::uint64_t cabin_id) {
    for (auto it = observed_.begin(); it != observed_.end();) {
        it = it->first.cabin_id == cabin_id ? observed_.erase(it) : std::next(it);
    }
    for (auto it = sightings_.begin(); it != sightings_.end();) {
        it = it->first.cabin_id == cabin_id ? sightings_.erase(it) : std::next(it);
    }
    info_.erase(cabin_id);
}

void CabinStore::NoteWrite(const CabinKey& key, const CabinEntry& entry) {
    auto it = observed_.find(key);
    // **The common case, and the whole reason the hook is affordable**: the
    // value is not observed, so there is nothing this write can invalidate.
    // One hash probe per cabin per write.
    if (it == observed_.end()) return;

    if (it->second.size() >= limits_.max_entries_per_value) {
        // Un-observe rather than drop the append. Dropping it would leave a
        // set marked authoritative that is missing a row - the exact break
        // C1 forbids - where un-observing costs a scan (§1's corollary).
        RemoveSet(key.cabin_id, it->second.size());
        observed_.erase(it);
        sightings_.erase(key);
        ++stats_.unobserved;
        ++stats_.cap_refusals;
        return;
    }

    it->second.push_back(entry);
    ++info_[key.cabin_id].entries;
    ++stats_.appends;
    ++info_[key.cabin_id].appends;
}

CabinStore::CabinInfo CabinStore::InfoFor(std::uint64_t cabin_id) const {
    auto it = info_.find(cabin_id);
    return it == info_.end() ? CabinInfo{} : it->second;
}

void CabinStore::AddSet(std::uint64_t cabin_id, std::size_t entries) {
    CabinInfo& info = info_[cabin_id];
    ++info.values;
    info.entries += entries;
}

void CabinStore::RemoveSet(std::uint64_t cabin_id, std::size_t entries) {
    CabinInfo& info = info_[cabin_id];
    if (info.values > 0) --info.values;
    // Saturating, so a counter that has somehow drifted cannot wrap into a
    // gigantic number and make a Cabin look full. These are inspection
    // figures; being approximately right beats being spectacularly wrong.
    info.entries -= std::min(info.entries, entries);
}

}  // namespace kds::stats
