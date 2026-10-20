#include "kds/stats/statement_stats.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <new>

namespace kds::stats {

namespace {

// Every first word `DispatchInner` routes, in the spelling a row carries.
// A word not here is `OTHER`.
constexpr std::string_view kKinds[] = {
    "SELECT", "WITH",     "INSERT",   "UPDATE", "DELETE", "PURGE",  "CREATE",
    "DROP",   "ALTER",    "SHOW",     "DESCRIBE", "DESC", "ANALYZE", "BEGIN",
    "START",  "COMMIT",   "ROLLBACK", "ABORT",  "SAVEPOINT", "RELEASE", "SET",
    "PING",   "SYNC",     "BACKUP",   "STOP",
};
constexpr std::string_view kOther = "OTHER";

bool IEquals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) ==
                      std::toupper(static_cast<unsigned char>(y));
           });
}

}  // namespace

std::string_view StatementKindOf(std::string_view line) noexcept {
    std::size_t at = 0;
    while (at < line.size() && std::isspace(static_cast<unsigned char>(line[at]))) ++at;
    // The first whitespace-separated token, as `DispatchInner` splits it:
    // `COMMIT;` routes nowhere, so it is no `COMMIT`.
    std::size_t end = at;
    while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) ++end;
    const std::string_view word = line.substr(at, end - at);
    for (std::string_view kind : kKinds) {
        if (IEquals(word, kind)) return kind;
    }
    return kOther;
}

std::size_t StatementStats::KeyHash::operator()(const Key& k) const noexcept {
    // The kind's pointer is its identity: every key's kind is a `kKinds`
    // entry or `kOther`.
    return std::hash<std::uint64_t>{}(k.fetch_id) ^
           (std::hash<const void*>{}(k.kind.data()) << 1);
}

std::array<StatementStats::Part, kStatsParts>& StatementStats::parts() noexcept {
    static std::array<Part, kStatsParts> p;
    return p;
}

void StatementStats::Record(std::uint32_t core, std::uint64_t fetch_id, std::string_view line,
                            bool refused, std::uint64_t elapsed_us, std::int64_t now_us) noexcept {
    // Derived here, so every key's kind is `StatementKindOf`'s static
    // storage - the pointer `KeyHash` hashes by.
    const std::string_view kind = StatementKindOf(line);
    Part& part = parts()[core % kStatsParts];
    const LatchGuard held(&part.latch);
    const Key key{fetch_id, kind};
    auto found = part.shapes.find(key);
    if (found == part.shapes.end()) {
        if (part.shapes.size() >= kShapesPerCore) {
            ++part.dropped;
            return;
        }
        // The one allocation, on a shape's first statement; a failure drops
        // the row rather than the statement (the header's rule).
        try {
            StatementShape shape;
            shape.fetch_id = fetch_id;
            shape.kind = std::string(kind);
            shape.first_seen_us = now_us;
            // Cut at a character boundary: a UTF-8 continuation byte
            // (`10xxxxxx`) at the cut moves it back, so the text stays
            // decodable by a client that decodes strictly.
            std::size_t cut = std::min(line.size(), kShapeTextBytes);
            while (cut < line.size() && cut > 0 &&
                   (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            shape.shape_text = std::string(line.substr(0, cut));
            found = part.shapes.emplace(key, std::move(shape)).first;
        } catch (const std::bad_alloc&) {
            ++part.dropped;
            return;
        }
    }
    StatementShape& shape = found->second;
    ++shape.calls;
    if (refused) ++shape.refused;
    shape.total_us += elapsed_us;
    shape.max_us = std::max(shape.max_us, elapsed_us);
    shape.last_seen_us = now_us;
}

std::vector<StatementShape> StatementStats::Snapshot() {
    std::unordered_map<Key, StatementShape, KeyHash> merged;
    for (Part& part : parts()) {
        const LatchGuard held(&part.latch);
        for (const auto& [key, shape] : part.shapes) {
            auto [at, fresh] = merged.try_emplace(key, shape);
            if (fresh) continue;
            StatementShape& into = at->second;
            into.calls += shape.calls;
            into.refused += shape.refused;
            into.total_us += shape.total_us;
            into.max_us = std::max(into.max_us, shape.max_us);
            if (shape.first_seen_us < into.first_seen_us) {
                into.first_seen_us = shape.first_seen_us;
                into.shape_text = shape.shape_text;
            }
            into.last_seen_us = std::max(into.last_seen_us, shape.last_seen_us);
        }
    }
    std::vector<StatementShape> out;
    out.reserve(merged.size());
    for (auto& [key, shape] : merged) out.push_back(std::move(shape));
    std::sort(out.begin(), out.end(), [](const StatementShape& a, const StatementShape& b) {
        return a.fetch_id != b.fetch_id ? a.fetch_id < b.fetch_id : a.kind < b.kind;
    });
    return out;
}

std::uint64_t StatementStats::Dropped() noexcept {
    std::uint64_t total = 0;
    for (Part& part : parts()) {
        const LatchGuard held(&part.latch);
        total += part.dropped;
    }
    return total;
}

void StatementStats::ResetForTest() noexcept {
    for (Part& part : parts()) {
        const LatchGuard held(&part.latch);
        part.shapes.clear();
        part.dropped = 0;
    }
}

}  // namespace kds::stats
