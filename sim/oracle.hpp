#pragma once

// sim/oracle.hpp — the reference model (bench/workplan-teststrategy SIM03).
// The dumbest thing that can be right: per relation a std::map<pk, Row>,
// updated on every **acknowledged** write, queried on every read. It never
// sees the engine's internals and it does not know the feature toggles
// exist — divergence from it is the definition of a wrong answer.
//
// MarkSynced() snapshots the acknowledged state; the crash loop reconciles
// a restarted instance against the snapshot (what SYNC promised) and the
// full state (what recovery will promise — the [GATED: recovery] half).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kds::sim {

struct OracleRow {
    std::int64_t v = 0;
    std::string name;

    bool operator==(const OracleRow&) const = default;
};

class Oracle {
public:
    using TableRows = std::map<std::uint64_t, OracleRow>;

    void CreateTable(const std::string& table) { tables_[table]; }
    bool HasTable(const std::string& table) const { return tables_.count(table) != 0; }

    void ApplyInsert(const std::string& table, std::uint64_t id, OracleRow row) {
        tables_[table][id] = std::move(row);
    }

    void MarkSynced() { synced_ = tables_; }

    const std::map<std::string, TableRows>& tables() const { return tables_; }
    const std::map<std::string, TableRows>& synced() const { return synced_; }

    // Expected result rows, rendered exactly as the wire renders them for
    // `SELECT *` over (id, v, name): "<id>,<v>,<name>", ascending id.
    static std::string Render(std::uint64_t id, const OracleRow& row) {
        return std::to_string(id) + "," + std::to_string(row.v) + "," + row.name;
    }

    std::vector<std::string> ExpectPk(const std::string& table, std::uint64_t key) const;
    std::vector<std::string> ExpectRange(const std::string& table, std::uint64_t lo,
                                         std::uint64_t hi) const;
    std::vector<std::string> ExpectFilter(const std::string& table, std::int64_t v) const;
    std::vector<std::string> ExpectAll(const TableRows& rows) const;

private:
    std::map<std::string, TableRows> tables_;
    std::map<std::string, TableRows> synced_;
};

}  // namespace kds::sim
