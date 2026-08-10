#include "sim/oracle.hpp"

namespace kds::sim {

std::vector<std::string> Oracle::ExpectPk(const std::string& table, std::uint64_t key) const {
    std::vector<std::string> out;
    auto t = tables_.find(table);
    if (t == tables_.end()) return out;
    auto row = t->second.find(key);
    if (row != t->second.end()) out.push_back(Render(row->first, row->second));
    return out;
}

std::vector<std::string> Oracle::ExpectRange(const std::string& table, std::uint64_t lo,
                                             std::uint64_t hi) const {
    std::vector<std::string> out;
    auto t = tables_.find(table);
    if (t == tables_.end()) return out;
    for (auto it = t->second.lower_bound(lo); it != t->second.end() && it->first <= hi; ++it) {
        out.push_back(Render(it->first, it->second));
    }
    return out;
}

std::vector<std::string> Oracle::ExpectFilter(const std::string& table, std::int64_t v) const {
    std::vector<std::string> out;
    auto t = tables_.find(table);
    if (t == tables_.end()) return out;
    for (const auto& [id, row] : t->second) {
        if (row.v == v) out.push_back(Render(id, row));
    }
    return out;
}

std::vector<std::string> Oracle::ExpectAll(const TableRows& rows) const {
    std::vector<std::string> out;
    out.reserve(rows.size());
    for (const auto& [id, row] : rows) out.push_back(Render(id, row));
    return out;
}

}  // namespace kds::sim
