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

std::optional<Profile> ParseProfile(std::string_view name) {
    for (const Profile profile : {Profile::kUniform, Profile::kZipfian, Profile::kColliding}) {
        if (name == ProfileName(profile)) return profile;
    }
    return std::nullopt;
}

const char* OpKindName(Op::Kind kind) {
    switch (kind) {
        case Op::Kind::kCreateTable: return "create-table";
        case Op::Kind::kInsert: return "insert";
        case Op::Kind::kSelectPk: return "select-pk";
        case Op::Kind::kSelectRange: return "select-range";
        case Op::Kind::kFilterScan: return "filter-scan";
        case Op::Kind::kSync: return "sync";
        case Op::Kind::kUpdate: return "update";
        case Op::Kind::kDelete: return "delete";
        case Op::Kind::kBegin: return "begin";
        case Op::Kind::kCommit: return "commit";
        case Op::Kind::kRollback: return "rollback";
        case Op::Kind::kCreateCabin: return "create-cabin";
        case Op::Kind::kCreatePattern: return "create-pattern";
    }
    return "unknown";
}

std::optional<Op::Kind> ParseOpKind(std::string_view name) {
    for (const Op::Kind kind :
         {Op::Kind::kCreateTable, Op::Kind::kInsert, Op::Kind::kSelectPk,
          Op::Kind::kSelectRange, Op::Kind::kFilterScan, Op::Kind::kSync, Op::Kind::kUpdate,
          Op::Kind::kDelete, Op::Kind::kBegin, Op::Kind::kCommit, Op::Kind::kRollback,
          Op::Kind::kCreateCabin, Op::Kind::kCreatePattern}) {
        if (name == OpKindName(kind)) return kind;
    }
    return std::nullopt;
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

Workload::Table& Workload::PickTableMutable() { return tables_[rng_.Below(tables_.size())]; }

const Workload::Table& Workload::PickTable() { return PickTableMutable(); }

// Mostly hits, sometimes an honest miss just past the end.
std::uint64_t Workload::GuessKey(const Table& table) {
    return 1 + rng_.Below(table.inserted + 3);
}

Op Workload::Insert(Table& table) {
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

Op Workload::Update(const Table& table) {
    Op op;
    op.kind = Op::Kind::kUpdate;
    op.table = table.name;
    op.by_pk = rng_.Chance(70);
    op.set_name = rng_.Chance(40);
    op.sql = "UPDATE " + table.name + " SET ";
    if (op.set_name) {
        op.name = NextName();
        op.sql += "name = '" + op.name + "'";
    } else {
        op.v = NextValue();
        op.sql += "v = " + std::to_string(op.v);
    }
    if (op.by_pk) {
        op.key = GuessKey(table);
        op.sql += " WHERE id = " + std::to_string(op.key);
    } else {
        op.pred_v = NextValue();
        op.sql += " WHERE v = " + std::to_string(op.pred_v);
    }
    return op;
}

Op Workload::Delete(const Table& table) {
    Op op;
    op.kind = Op::Kind::kDelete;
    op.table = table.name;
    op.by_pk = rng_.Chance(70);
    op.sql = "DELETE FROM " + table.name;
    if (op.by_pk) {
        op.key = GuessKey(table);
        op.sql += " WHERE id = " + std::to_string(op.key);
    } else {
        op.pred_v = NextValue();
        op.sql += " WHERE v = " + std::to_string(op.pred_v);
    }
    return op;
}

Op Workload::DataOp() {
    const std::uint64_t roll = rng_.Below(100);
    if (roll < 42) return Insert(PickTableMutable());
    if (roll < 57) return Update(PickTable());
    if (roll < 64) return Delete(PickTable());
    if (roll < 79) {
        const Table& table = PickTable();
        Op op;
        op.kind = Op::Kind::kSelectPk;
        op.table = table.name;
        op.key = GuessKey(table);
        op.sql = "SELECT * FROM " + table.name + " WHERE id = " + std::to_string(op.key);
        return op;
    }
    if (roll < 88) {
        const Table& table = PickTable();
        Op op;
        op.kind = Op::Kind::kSelectRange;
        op.table = table.name;
        op.lo = GuessKey(table);
        op.hi = op.lo + rng_.Below(20);
        op.sql = "SELECT * FROM " + table.name + " WHERE id BETWEEN " + std::to_string(op.lo) +
                 " AND " + std::to_string(op.hi);
        return op;
    }
    const Table& table = PickTable();
    Op op;
    op.kind = Op::Kind::kFilterScan;
    op.table = table.name;
    op.v = NextValue();
    op.sql = "SELECT * FROM " + table.name + " WHERE v = " + std::to_string(op.v);
    return op;
}

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

    if (in_txn_) {
        if (txn_ops_left_ == 0) {
            in_txn_ = false;
            Op op;
            // Rollback often enough to be a shape rather than a curiosity:
            // it is the half that has to leave *nothing* behind, and the
            // oracle checks that by dropping the whole pending set.
            const bool commit = rng_.Chance(70);
            op.kind = commit ? Op::Kind::kCommit : Op::Kind::kRollback;
            op.sql = commit ? "COMMIT" : "ROLLBACK";
            return op;
        }
        --txn_ops_left_;
        return DataOp();
    }

    const std::uint64_t roll = rng_.Below(100);
    if (roll < 82) return DataOp();
    if (roll < 86) {
        Op op;
        op.kind = Op::Kind::kSync;
        op.sql = "SYNC";
        return op;
    }
    if (roll < 89) {
        // A Cabin on the only column that can carry one here: the pk is
        // refused by name, and `name` is the spill-prone varchar the
        // harness deliberately keeps in the var-heap's hands.
        const Table& table = PickTable();
        const std::string key = table.name + ".v";
        if (cabins_.insert(key).second) {
            Op op;
            op.kind = Op::Kind::kCreateCabin;
            op.table = table.name;
            op.sql = "CREATE CABIN ON " + table.name + "(v)";
            return op;
        }
        return DataOp();  // already declared; the roll is spent, not wasted
    }
    if (roll < 92) {
        // A pattern is identified by its **shape**, not its name: a second
        // declaration of a body that fingerprints the same is refused
        // ("this shape is already declared as ..."), so the generator
        // tracks shapes exactly as it tracks cabins. Two per relation, the
        // pk point read and the non-key filter, which are the two access
        // kinds a trail can be recorded for here.
        const Table& table = PickTable();
        const bool by_pk = rng_.Chance(50);
        const std::string shape = table.name + (by_pk ? ":id" : ":v");
        if (cabins_.insert("pattern/" + shape).second) {
            Op op;
            op.kind = Op::Kind::kCreatePattern;
            op.table = table.name;
            op.sql = "CREATE PATTERN p" + std::to_string(patterns_++) +
                     (by_pk ? " ($k int64) OF SELECT * FROM " + table.name + " WHERE id = $k"
                            : " ($x int64) OF SELECT * FROM " + table.name + " WHERE v = $x");
            return op;
        }
        return DataOp();
    }

    in_txn_ = true;
    txn_ops_left_ = 1 + rng_.Below(8);
    Op op;
    op.kind = Op::Kind::kBegin;
    op.sql = rng_.Chance(50) ? "BEGIN" : "BEGIN ISOLATION LEVEL REPEATABLE READ";
    return op;
}

}  // namespace kds::sim
