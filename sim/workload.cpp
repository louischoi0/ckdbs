#include "sim/workload.hpp"

namespace kds::sim {

const char* ProfileName(Profile profile) {
    switch (profile) {
        case Profile::kUniform: return "uniform";
        case Profile::kZipfian: return "zipfian";
        case Profile::kColliding: return "colliding";
    }
    return "unknown";
}

Workload::Workload(Rng rng, Profile profile) : rng_(std::move(rng)), profile_(profile) {
    // 1-3 tables, each independently heap or btree. Decided up front so
    // the table set is stable however many ops are drawn.
    const std::size_t count = 1 + rng_.Below(3);
    for (std::size_t i = 0; i < count; ++i) {
        tables_.push_back(Table{"t" + std::to_string(i), rng_.Chance(50), 0});
    }
}

std::int64_t Workload::NextValue() {
    switch (profile_) {
        case Profile::kUniform:
            return rng_.Range(0, 999);
        case Profile::kZipfian: {
            // Cubing a uniform [0,1) sample skews hard toward 0 — enough
            // of a zipf stand-in to make some values much hotter than
            // others, with no floating point in the op stream.
            const std::uint64_t u = rng_.Below(1000);
            return static_cast<std::int64_t>(u * u * u / (1000 * 1000));
        }
        case Profile::kColliding:
            return rng_.Range(0, 4);
    }
    return 0;
}

std::string Workload::NextName() {
    // Two length bands, straddling the inline capacity of the default
    // 64-byte tagged cell (61 bytes): short stays inline, long spills to
    // the var-heap. Alphabet [a-z] only — string values render bare on the
    // wire, so a comma or a backslash in a value would make the reply
    // ambiguous, which is a protocol property, not a harness choice.
    const std::size_t len = rng_.Chance(35)
                                ? 80 + rng_.Below(240)   // spilled
                                : rng_.Below(45);        // inline, empty included
    std::string out;
    out.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(static_cast<char>('a' + rng_.Below(26)));
    }
    return out;
}

const Workload::Table& Workload::PickTable() { return tables_[rng_.Below(tables_.size())]; }

Workload::Table& Workload::PickTableMutable() { return tables_[rng_.Below(tables_.size())]; }

Op Workload::Next() {
    if (created_ < tables_.size()) {
        const Table& table = tables_[created_];
        ++created_;
        Op op;
        op.kind = Op::Kind::kCreateTable;
        op.table = table.name;
        op.btree = table.btree;
        op.sql = "CREATE TABLE " + table.name + " (id int64, v int64, name varchar)" +
                 (table.btree ? " BTREE" : " HEAP");
        return op;
    }

    const std::uint64_t roll = rng_.Below(100);
    if (roll < 50) {
        Table& table = PickTableMutable();
        Op op;
        op.kind = Op::Kind::kInsert;
        op.table = table.name;
        op.v = NextValue();
        op.name = NextName();
        op.sql = "INSERT INTO " + table.name + " VALUES (" + std::to_string(op.v) + ", '" +
                 op.name + "')";
        ++table.inserted;
        return op;
    }
    if (roll < 70) {
        const Table& table = PickTable();
        Op op;
        op.kind = Op::Kind::kSelectPk;
        op.table = table.name;
        // Mostly hits, sometimes an honest miss just past the end.
        op.key = 1 + rng_.Below(table.inserted + 3);
        op.sql = "SELECT * FROM " + table.name + " WHERE id = " + std::to_string(op.key);
        return op;
    }
    if (roll < 80) {
        const Table& table = PickTable();
        Op op;
        op.kind = Op::Kind::kSelectRange;
        op.table = table.name;
        op.lo = 1 + rng_.Below(table.inserted + 3);
        op.hi = op.lo + rng_.Below(20);
        op.sql = "SELECT * FROM " + table.name + " WHERE id BETWEEN " + std::to_string(op.lo) +
                 " AND " + std::to_string(op.hi);
        return op;
    }
    if (roll < 95) {
        const Table& table = PickTable();
        Op op;
        op.kind = Op::Kind::kFilterScan;
        op.table = table.name;
        op.v = NextValue();
        op.sql = "SELECT * FROM " + table.name + " WHERE v = " + std::to_string(op.v);
        return op;
    }
    Op op;
    op.kind = Op::Kind::kSync;
    op.sql = "SYNC";
    return op;
}

}  // namespace kds::sim
