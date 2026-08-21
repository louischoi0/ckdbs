#include "sim/oracle.hpp"

#include <charconv>

namespace kds::sim {

namespace {

// The id prefix of a rendered "<id>,<v>,<name>" row.
std::uint64_t IdOf(std::string_view rendered) {
    std::uint64_t id = 0;
    std::from_chars(rendered.data(), rendered.data() + rendered.size(), id);
    return id;
}

}  // namespace

std::size_t Oracle::ApplyUpdate(const std::string& table, const Predicate& where,
                                const Assignment& set) {
    auto t = Live().find(table);
    if (t == Live().end()) return 0;

    std::size_t updated = 0;
    const auto assign = [&](std::uint64_t id, OracleRow& row) {
        if (set.set_name) {
            row.name = set.name;
        } else {
            row.v = set.v;
        }
        Touch(table, id);
        ++updated;
    };

    if (where.by_pk) {
        auto row = t->second.find(where.key);
        if (row != t->second.end()) assign(row->first, row->second);
        return updated;
    }
    for (auto& [id, row] : t->second) {
        if (row.v == where.v) assign(id, row);
    }
    return updated;
}

std::size_t Oracle::ApplyDelete(const std::string& table, const Predicate& where) {
    auto t = Live().find(table);
    if (t == Live().end()) return 0;

    if (where.by_pk) {
        auto row = t->second.find(where.key);
        if (row == t->second.end()) return 0;
        Touch(table, where.key);
        t->second.erase(row);
        return 1;
    }

    std::size_t deleted = 0;
    for (auto it = t->second.begin(); it != t->second.end();) {
        if (it->second.v != where.v) {
            ++it;
            continue;
        }
        Touch(table, it->first);
        it = t->second.erase(it);
        ++deleted;
    }
    return deleted;
}

std::vector<std::uint64_t> Oracle::Matching(const std::string& table,
                                            const Predicate& where) const {
    std::vector<std::uint64_t> out;
    auto t = Live().find(table);
    if (t == Live().end()) return out;
    if (where.by_pk) {
        if (t->second.count(where.key) != 0) out.push_back(where.key);
        return out;
    }
    for (const auto& [id, row] : t->second) {
        if (row.v == where.v) out.push_back(id);
    }
    return out;
}

bool Oracle::IsUnchecked(const std::string& table, std::uint64_t id) const {
    const auto it = unchecked_.find(table);
    return it != unchecked_.end() && it->second.count(id) != 0;
}

bool Oracle::CountCheckable(const std::string& table, const Predicate& where) const {
    if (!has_unknowns(table)) return true;
    if (!where.by_pk) return false;
    // A pk predicate names exactly one row, so the only unknown that can
    // change its count is that row's own. An outstanding errored INSERT
    // matters only if *this* id is one the engine never named — an id it
    // did name is the oracle's to assert on whatever else is unknown here.
    if (IsUnchecked(table, where.key)) return false;
    if (indeterminate_.count(table) == 0) return true;
    const auto it = issued_.find(table);
    return it != issued_.end() && it->second.count(where.key) != 0;
}

bool Oracle::Ignorable(const std::string& table, std::string_view rendered) const {
    const std::uint64_t id = IdOf(rendered);
    if (IsUnchecked(table, id)) return true;
    if (indeterminate_.count(table) == 0) return false;
    // An errored INSERT is outstanding here, so a row the engine never
    // named an id for may exist. One it *did* name stays fully checkable —
    // an insert that failed cannot make an acknowledged row anyone's
    // else's business, whatever it now contains.
    const auto it = issued_.find(table);
    return it == issued_.end() || it->second.count(id) == 0;
}

std::vector<std::string> Oracle::ExpectPk(const std::string& table, std::uint64_t key) const {
    std::vector<std::string> out;
    auto t = Live().find(table);
    if (t == Live().end()) return out;
    auto row = t->second.find(key);
    if (row != t->second.end() && !IsUnchecked(table, key)) {
        out.push_back(Render(row->first, row->second));
    }
    return out;
}

std::vector<std::string> Oracle::ExpectRange(const std::string& table, std::uint64_t lo,
                                             std::uint64_t hi) const {
    std::vector<std::string> out;
    auto t = Live().find(table);
    if (t == Live().end()) return out;
    for (auto it = t->second.lower_bound(lo); it != t->second.end() && it->first <= hi; ++it) {
        if (IsUnchecked(table, it->first)) continue;
        out.push_back(Render(it->first, it->second));
    }
    return out;
}

std::vector<std::string> Oracle::ExpectFilter(const std::string& table, std::int64_t v) const {
    std::vector<std::string> out;
    auto t = Live().find(table);
    if (t == Live().end()) return out;
    for (const auto& [id, row] : t->second) {
        if (row.v == v && !IsUnchecked(table, id)) out.push_back(Render(id, row));
    }
    return out;
}

std::vector<std::string> Oracle::ExpectAll(const std::string& table) const {
    std::vector<std::string> out;
    auto t = tables_.find(table);
    if (t == tables_.end()) return out;
    out.reserve(t->second.size());
    for (const auto& [id, row] : t->second) {
        if (IsUnchecked(table, id)) continue;
        out.push_back(Render(id, row));
    }
    return out;
}

}  // namespace kds::sim
