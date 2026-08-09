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
        // answer and not a slow one (docs/feat-cabin.md §12.3).
        if (header.key == key) return &header;
    }
    return nullptr;
}

GroupHeader* BoundCabin::FindMutable(const std::string& key) {
    return const_cast<GroupHeader*>(Find(key));
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
    GroupHeader* header = FindMutable(key);
    if (header == nullptr) {
        auto& bucket = groups_by_hash_[HashGroupKey(key)];
        GroupHeader fresh;
        fresh.key = key;
        bucket.push_back(std::move(fresh));
        header = &bucket.back();
    }

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
                // disagreement that is the specification working.
                ++count;
                if (__builtin_add_overflow(sum, entry.value().value, &sum)) {
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
