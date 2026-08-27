#include "kds/exec/bound_cabin.hpp"

#include <algorithm>

namespace kds::exec {

namespace {

void AppendByte(std::string& out, std::uint8_t b) { out.push_back(static_cast<char>(b)); }

void AppendU64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) AppendByte(out, static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
}

}  // namespace

std::string EncodeGroupKey(const std::vector<parser::AstValue>& values) {
    std::string out;
    for (const parser::AstValue& v : values) {
        switch (v.type) {
            case parser::ValueType::kNull:
                // NULL is a *value* here, not an absence, so two NULL keys are
                // one group. That is grouping identity rather than comparison
                // - `CompareValues` is untouched by this, exactly as the
                // aggregate fold's key encoding leaves it untouched.
                AppendByte(out, 0);
                break;
            case parser::ValueType::kInt:
                AppendByte(out, 1);
                AppendU64(out, static_cast<std::uint64_t>(v.int_val));
                break;
            case parser::ValueType::kDecimal:
                // Tagged apart from a plain int and carries its scale: an
                // unscaled 1234 at scale 2 and a bare 1234 are different
                // values, and a shared tag would make them one group.
                AppendByte(out, 2);
                AppendByte(out, v.scale);
                AppendU64(out, static_cast<std::uint64_t>(v.int_val));
                break;
            case parser::ValueType::kDecimalWide:
                AppendByte(out, 3);
                AppendByte(out, v.scale);
                AppendU64(out, static_cast<std::uint64_t>(v.dec_hi));
                AppendU64(out, static_cast<std::uint64_t>(v.int_val));
                break;
            case parser::ValueType::kStr:
            case parser::ValueType::kParam:
                // **Length-prefixed**, which is what stops `('a','bc')` and
                // `('ab','c')` from encoding identically - the same rule the
                // aggregate fold's key encoding follows and for the same
                // reason. A separator byte would not do: a value may contain
                // any byte.
                AppendByte(out, 4);
                AppendU64(out, static_cast<std::uint64_t>(v.str_val.size()));
                out.append(v.str_val);
                break;
        }
    }
    return out;
}

std::uint64_t HashGroupKey(const std::string& encoded) noexcept {
    // FNV-1a. A mixing function, not a cryptographic one: a collision is
    // *expected* to be possible and is handled by confirming the key, so
    // strength buys nothing here and speed is on the write path.
    std::uint64_t h = 1469598103934665603ULL;
    for (const char c : encoded) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

std::size_t BoundCabin::group_count() const noexcept {
    std::size_t n = 0;
    for (const auto& [hash, headers] : groups_by_hash_) n += headers.size();
    return n;
}

const GroupHeader* BoundCabin::Find(const std::string& key) const {
    auto it = groups_by_hash_.find(HashGroupKey(key));
    if (it == groups_by_hash_.end()) return nullptr;
    for (const GroupHeader& header : it->second) {
        // The confirmation. Without it a colliding hash would return someone
        // else's group, which for an authoritative structure is a wrong
        // answer and not a slow one (docs/spec/cabin.md §12.3).
        if (header.key == key) return &header;
    }
    return nullptr;
}

GroupHeader* BoundCabin::FindMutable(const std::string& key) {
    return const_cast<GroupHeader*>(Find(key));
}

GroupHeader& BoundCabin::EnsureGroup(const std::string& key) {
    if (GroupHeader* header = FindMutable(key); header != nullptr) {
        return *header;
    }
    auto& bucket = groups_by_hash_[HashGroupKey(key)];
    GroupHeader fresh;
    fresh.key = key;
    fresh.group_id = next_group_id_++;  // AS6a: dense, from 1, never reused
    bucket.push_back(std::move(fresh));
    return bucket.back();
}

std::uint32_t BoundCabin::EnsureGroupId(const std::string& key) {
    return EnsureGroup(key).group_id;
}

GroupHeader* BoundCabin::FindById(std::uint32_t group_id) {
    for (auto& [hash, bucket] : groups_by_hash_) {
        for (GroupHeader& header : bucket) {
            if (header.group_id == group_id) return &header;
        }
    }
    return nullptr;
}

Status BoundCabin::AdoptGroupId(const std::string& key, std::uint32_t group_id) {
    if (group_id == 0) {
        // Refused rather than guessed: attributing it would need the allocation
        // order the id exists to replace.
        //
        // **Not "a pre-AS6a stream", which an earlier comment here claimed.** A
        // record written before the field existed does not decode `group_id` as
        // 0 - `kAssertEntryFixedSize` grew 16 -> 20, so the field reads the
        // *entry bytes* that used to start at offset 16, i.e. the pk's low 32
        // bits. The genuinely-zero case is a **page** entry, which AST04 wrote as
        // a literal zero, and the cabin-page walk skips those
        // (`assertion_recover.cpp`). So this branch defends a corrupt or
        // hand-built record, which is worth refusing on its own terms.
        return Status::Corruption(
            "bound cabin: a replayed record carries group id 0, which is the reserved 'no group' "
            "value (docs/spec/assertion.md §5.1)");
    }
    if (GroupHeader* existing = FindMutable(key); existing != nullptr) {
        if (existing->group_id != group_id) {
            return Status::Corruption(
                "bound cabin: replayed record names group id " + std::to_string(group_id) +
                " for a group the directory holds as " + std::to_string(existing->group_id));
        }
        return Status::OK();
    }
    if (FindById(group_id) != nullptr) {
        return Status::Corruption("bound cabin: replayed group id " + std::to_string(group_id) +
                                  " already belongs to a different group key");
    }
    return RestoreGroup(group_id, key, /*count=*/0, /*sum=*/0);
}

Status BoundCabin::RestoreGroup(std::uint32_t group_id, const std::string& key,
                               std::int64_t count, std::int64_t sum) {
    if (group_id == 0) {
        return Status::InvalidArgument(
            "bound cabin: a snapshot named group id 0, which is the reserved 'no group' value");
    }
    if (FindById(group_id) != nullptr) {
        return Status::InvalidArgument("bound cabin: snapshot names group id " +
                                       std::to_string(group_id) + " twice");
    }
    if (Find(key) != nullptr) {
        return Status::InvalidArgument(
            "bound cabin: snapshot names the same group key twice, so one of them would be "
            "unreachable by key");
    }

    auto& bucket = groups_by_hash_[HashGroupKey(key)];
    GroupHeader restored;
    restored.key = key;
    restored.group_id = group_id;
    restored.count = count;
    restored.sum = sum;
    bucket.push_back(std::move(restored));

    // Past every id the snapshot carried, so a group the fold creates after it
    // cannot be handed an id an entry on a page already means.
    if (group_id >= next_group_id_) {
        next_group_id_ = group_id + 1;
    }
    return Status::OK();
}

Status BoundCabin::AttachEntry(std::uint32_t group_id, PageId page_id, std::uint16_t index) {
    GroupHeader* header = FindById(group_id);
    if (header == nullptr) {
        return Status::NotFound("bound cabin: no restored group carries id " +
                                std::to_string(group_id));
    }
    header->entries.emplace_back(page_id, index);
    return Status::OK();
}

StatusOr<std::uint64_t> BoundCabin::AttachEntries(std::span<const ScannedEntry> entries) {
    // The id -> header index, built once. `FindById`'s per-entry walk is
    // O(entries x groups); this is O(entries + groups), which is the difference
    // between a mount and a mount nobody waits for.
    std::unordered_map<std::uint32_t, GroupHeader*> by_id;
    by_id.reserve(group_count());
    for (auto& [hash, bucket] : groups_by_hash_) {
        for (GroupHeader& header : bucket) {
            by_id.emplace(header.group_id, &header);
        }
    }

    std::uint64_t attached = 0;
    for (const ScannedEntry& entry : entries) {
        auto it = by_id.find(entry.group_id);
        if (it == by_id.end()) {
            // Its group is not in the snapshot, so it belongs to one the fold
            // has yet to create - skipped, exactly as AttachEntry answers
            // NotFound for the same case.
            continue;
        }
        it->second->entries.emplace_back(entry.page_id, entry.index);
        ++attached;
    }
    return attached;
}

std::uint64_t BoundCabin::DedupeEntryLinkage() {
    std::uint64_t dropped = 0;
    for (auto& [hash, bucket] : groups_by_hash_) {
        for (GroupHeader& header : bucket) {
            const std::size_t before = header.entries.size();
            // Sorted then uniqued rather than a per-insert scan: this runs once
            // per group at the end of a rebuild, and a linear membership test per
            // entry would be the O(entries^2) the batch attach exists to avoid.
            std::sort(header.entries.begin(), header.entries.end());
            header.entries.erase(std::unique(header.entries.begin(), header.entries.end()),
                                 header.entries.end());
            dropped += before - header.entries.size();
        }
    }
    return dropped;
}

std::vector<BoundCabin::GroupSnapshot> BoundCabin::SnapshotGroups() const {
    std::vector<GroupSnapshot> out;
    out.reserve(group_count());
    for (const auto& [hash, bucket] : groups_by_hash_) {
        for (const GroupHeader& header : bucket) {
            out.push_back(GroupSnapshot{header.group_id, header.key, header.count, header.sum});
        }
    }
    // Ordered by id, because sched.md §8 wants a checkpoint whose bytes are a
    // function of its input alone - `groups_by_hash_` iterates in bucket order,
    // which is not one.
    std::sort(out.begin(), out.end(),
              [](const GroupSnapshot& a, const GroupSnapshot& b) {
                  return a.group_id < b.group_id;
              });
    return out;
}

StatusOr<AdmissionResult> BoundCabin::Admit(const std::string& key, std::int64_t delta) const {
    const GroupHeader* header = Find(key);
    const std::int64_t current = header != nullptr ? header->aggregate(aggregate_) : 0;

    std::int64_t next = 0;
    if (__builtin_add_overflow(current, delta, &next)) {
        // AG3: a statement error, never a wraparound. A wrapped sum is a
        // number that satisfies a bound it does not satisfy, which is the one
        // answer an admission check must never produce.
        return Status::OutOfRange("bound cabin: aggregate overflow adding " +
                                  std::to_string(delta) + " to " + std::to_string(current));
    }

    AdmissionResult result;
    result.would_be = next;
    // The enforced ceiling, already reduced from the declared operator by the
    // parser - so `<` versus `<=` is not a case this has to know about.
    result.admitted = next <= bound_;
    return result;
}

Status BoundCabin::Apply(const std::string& key, std::int64_t delta, PageId page_id,
                         std::uint16_t index) {
    // One creation site (EnsureGroup), so a group cannot be born without an id.
    GroupHeader* header = &EnsureGroup(key);

    // Checked here too, and not merely in Admit. The CREATE-time builder
    // (AST06) accumulates without admitting - it validates once at the end -
    // so this is the only test standing between it and a wrapped aggregate.
    //
    // **Cardinality moves by one per entry whatever the value is.** A SUM
    // entry of 0 is still a row in the group, and making the count depend on
    // the value would make `COUNT(*)` wrong for any relation containing a
    // zero - a bug that hides until the data happens to contain one.
    std::int64_t next_count = 0;
    std::int64_t next_sum = 0;
    if (__builtin_add_overflow(header->count, std::int64_t{1}, &next_count)) {
        return Status::OutOfRange("bound cabin: group cardinality overflow");
    }
    if (__builtin_add_overflow(header->sum, delta, &next_sum)) {
        return Status::OutOfRange("bound cabin: group sum overflow adding " +
                                  std::to_string(delta) + " to " + std::to_string(header->sum));
    }

    // A COUNT assertion writes 1 per entry (§5.1), so `delta` is 1 and the
    // two counters move together; a SUM assertion moves `sum` by the value
    // and `count` by one. Maintaining both always is what lets the same
    // structure answer either predicate and lets re-summation check both.
    header->count = next_count;
    header->sum = next_sum;
    header->entries.emplace_back(page_id, index);
    return Status::OK();
}

Status BoundCabin::Unapply(const std::string& key, std::int64_t delta, PageId page_id,
                           std::uint16_t index) {
    GroupHeader* header = FindMutable(key);
    if (header == nullptr) {
        return Status::NotFound("bound cabin: no group to un-apply from");
    }

    auto at = std::find(header->entries.begin(), header->entries.end(),
                        std::pair<PageId, std::uint16_t>(page_id, index));
    if (at == header->entries.end()) {
        return Status::NotFound("bound cabin: entry (" + std::to_string(page_id) + ", " +
                                std::to_string(index) + ") is not in this group");
    }

    std::int64_t next_count = 0;
    std::int64_t next_sum = 0;
    // One per entry, mirroring Apply - see its note on why the count does not
    // depend on the value.
    if (__builtin_sub_overflow(header->count, std::int64_t{1}, &next_count) ||
        __builtin_sub_overflow(header->sum, delta, &next_sum)) {
        return Status::OutOfRange("bound cabin: aggregate underflow un-applying " +
                                  std::to_string(delta));
    }

    header->count = next_count;
    header->sum = next_sum;
    header->entries.erase(at);
    return Status::OK();
}

Status BoundCabin::ApplyDeparture(const std::string& key, std::int64_t delta, PageId page_id,
                                  std::uint16_t index) {
    GroupHeader* header = FindMutable(key);
    if (header == nullptr) {
        // The row this departure removes was incorporated by build or
        // insert, so its group exists; a missing one means lost coverage,
        // which is worth an error and not an empty group going negative.
        return Status::NotFound("bound cabin: no group for a departure to leave");
    }

    std::int64_t next_count = 0;
    std::int64_t next_sum = 0;
    if (__builtin_sub_overflow(header->count, std::int64_t{1}, &next_count) ||
        __builtin_sub_overflow(header->sum, delta, &next_sum)) {
        return Status::OutOfRange("bound cabin: aggregate underflow on a departure of " +
                                  std::to_string(delta));
    }
    header->count = next_count;
    header->sum = next_sum;
    header->entries.emplace_back(page_id, index);
    return Status::OK();
}

Status BoundCabin::UnapplyDeparture(const std::string& key, std::int64_t delta, PageId page_id,
                                    std::uint16_t index) {
    GroupHeader* header = FindMutable(key);
    if (header == nullptr) {
        return Status::NotFound("bound cabin: no group to restore a departure into");
    }
    auto at = std::find(header->entries.begin(), header->entries.end(),
                        std::pair<PageId, std::uint16_t>(page_id, index));
    if (at == header->entries.end()) {
        return Status::NotFound("bound cabin: departure entry (" + std::to_string(page_id) +
                                ", " + std::to_string(index) + ") is not in this group");
    }

    std::int64_t next_count = 0;
    std::int64_t next_sum = 0;
    if (__builtin_add_overflow(header->count, std::int64_t{1}, &next_count) ||
        __builtin_add_overflow(header->sum, delta, &next_sum)) {
        return Status::OutOfRange("bound cabin: aggregate overflow restoring a departure of " +
                                  std::to_string(delta));
    }
    header->count = next_count;
    header->sum = next_sum;
    header->entries.erase(at);
    return Status::OK();
}

Status BoundCabin::VerifyAgainstEntries(const EntryReader& read) const {
    for (const auto& [hash, headers] : groups_by_hash_) {
        for (const GroupHeader& header : headers) {
            std::int64_t count = 0;
            std::int64_t sum = 0;
            for (const auto& [page_id, index] : header.entries) {
                auto entry = read(page_id, index);
                if (!entry.ok()) {
                    return entry.status().WithContext("re-summing a bound cabin group");
                }
                // Reserved entries are counted, because the header counts
                // them: §4.1's invariant is over committed **and** reserved
                // rows, so a verification that skipped them would report a
                // disagreement that is the specification working. A
                // departure entry contributes with the opposite sign - the
                // flag's whole meaning.
                const bool departure = entry.value().departure();
                if (__builtin_add_overflow(count, departure ? -1 : 1, &count)) {
                    return Status::OutOfRange("bound cabin: overflow while re-counting a group");
                }
                if (__builtin_add_overflow(sum, departure ? -entry.value().value
                                                          : entry.value().value,
                                           &sum)) {
                    return Status::OutOfRange("bound cabin: overflow while re-summing a group");
                }
            }
            if (count != header.count || sum != header.sum) {
                return Status::Corruption(
                    "bound cabin: group header disagrees with its entries (header count=" +
                    std::to_string(header.count) + " sum=" + std::to_string(header.sum) +
                    ", entries count=" + std::to_string(count) + " sum=" + std::to_string(sum) +
                    ")");
            }
        }
    }
    return Status::OK();
}

}  // namespace kds::exec
