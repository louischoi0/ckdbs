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
    "PING",   "SYNC",     "BACKUP",   "STOP",   "USE",    "REINDEX", "WAYSTONE",
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
    std::uint64_t h = std::hash<std::uint64_t>{}(k.fetch_id) ^
                      (std::hash<const void*>{}(k.kind.data()) << 1);
    // The binding, folded word by word (splitmix64's finalizer constants):
    // one text's bindings differ in a namespace or an oid, each a small
    // integer, so they are spread before they are mixed in. An unbound
    // statement - every kind with no fingerprint - folds nothing.
    const auto fold = [&h](std::uint64_t word) noexcept {
        word *= 0xBF58476D1CE4E5B9ULL;
        h = (h ^ word ^ (word >> 31)) * 0x94D049BB133111EBULL;
    };
    const StatementBinding& b = k.binding;
    if (b.over) fold(~std::uint64_t{0});
    if (b.namespace_oid != 0) fold(b.namespace_oid);
    for (std::size_t i = 0; i < b.n; ++i) fold(b.oids[i]);
    return static_cast<std::size_t>(h);
}

std::array<StatementStats::Part, kStatsParts>& StatementStats::parts() noexcept {
    static std::array<Part, kStatsParts> p;
    return p;
}

void StatementStats::Record(std::uint32_t core, std::uint64_t fetch_id, std::string_view line,
                            bool refused, std::uint64_t elapsed_us, std::int64_t now_us,
                            std::string_view plan_source,
                            std::string_view plan_label,
                            const StatementCost& cost,
                            const StatementBinding& binding) noexcept {
    // Derived here, so every key's kind is `StatementKindOf`'s static
    // storage - the pointer `KeyHash` hashes by.
    const std::string_view kind = StatementKindOf(line);
    Part& part = parts()[core % kStatsParts];
    const LatchGuard held(&part.latch);
    const Key key{fetch_id, kind, binding};
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
            shape.binding = binding;
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
    ++shape.latency[sched::WaitBucketOf(elapsed_us * 1000)];
    if (cost.conflict) ++shape.conflicts;
    shape.rows += cost.rows;
    if (cost.executed) {
        ++shape.executed_calls;
        shape.rows_examined += cost.rows_examined;
        shape.pages += cost.pages;
    }
    if (cost.compiled) {
        ++shape.compiled_calls;
        shape.compile_ns += cost.compile_ns;
    }
    if (cost.timed.has_value()) {
        ++shape.timed_calls;
        shape.timed += *cost.timed;
    }
    // **The latest compile decides the source** (sys-views.md §6.1): a call
    // that compiled its own chain, or handed a source, writes the pair - an
    // empty one too, so a shape a plan stopped applying to reads NULL - and
    // a call that compiled nothing keeps what the shape had. Assigned only
    // where it moved: a shape runs one source statement after statement, so
    // this is two compares and no allocation.
    if (!cost.compiled && plan_source.empty()) return;
    if (shape.plan_source != plan_source || shape.plan_label != plan_label) {
        try {
            // Both made before either is kept, so the pair never mixes.
            std::string source(plan_source);
            std::string label(plan_label);
            shape.plan_source = std::move(source);
            shape.plan_label = std::move(label);
        } catch (const std::bad_alloc&) {
            // The counts stand; the source stays the last one written.
        }
    }
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
            into.conflicts += shape.conflicts;
            into.rows += shape.rows;
            into.executed_calls += shape.executed_calls;
            into.rows_examined += shape.rows_examined;
            into.pages += shape.pages;
            into.compiled_calls += shape.compiled_calls;
            into.compile_ns += shape.compile_ns;
            into.timed_calls += shape.timed_calls;
            into.timed += shape.timed;
            for (std::size_t k = 0; k < into.latency.size(); ++k) {
                into.latency[k] += shape.latency[k];
            }
            if (shape.first_seen_us < into.first_seen_us) {
                into.first_seen_us = shape.first_seen_us;
                into.shape_text = shape.shape_text;
            }
            // The source of the core that saw the shape last.
            if (shape.last_seen_us > into.last_seen_us && !shape.plan_source.empty()) {
                into.plan_source = shape.plan_source;
                into.plan_label = shape.plan_label;
            }
            into.last_seen_us = std::max(into.last_seen_us, shape.last_seen_us);
        }
    }
    std::vector<StatementShape> out;
    out.reserve(merged.size());
    for (auto& [key, shape] : merged) out.push_back(std::move(shape));
    std::sort(out.begin(), out.end(), [](const StatementShape& a, const StatementShape& b) {
        if (a.fetch_id != b.fetch_id) return a.fetch_id < b.fetch_id;
        if (a.kind != b.kind) return a.kind < b.kind;
        // The unbound row first, then by namespace and relations, the row
        // whose binding is not known last.
        if (a.binding.over != b.binding.over) return b.binding.over;
        if (a.binding.namespace_oid != b.binding.namespace_oid) {
            return a.binding.namespace_oid < b.binding.namespace_oid;
        }
        return std::lexicographical_compare(
            a.binding.oids.begin(), a.binding.oids.begin() + a.binding.n,
            b.binding.oids.begin(), b.binding.oids.begin() + b.binding.n);
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
