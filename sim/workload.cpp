#include "sim/workload.hpp"

#include <algorithm>

#include "kds/storage/keystone.hpp"

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
        case Op::Kind::kSelectTupleKey: return "select-tuple-key";
        case Op::Kind::kSelectRange: return "select-range";
        case Op::Kind::kFilterScan: return "filter-scan";
        case Op::Kind::kSync: return "sync";
        case Op::Kind::kUpdate: return "update";
        case Op::Kind::kDelete: return "delete";
        case Op::Kind::kBegin: return "begin";
        case Op::Kind::kCommit: return "commit";
        case Op::Kind::kRollback: return "rollback";
        case Op::Kind::kCreateCabin: return "create-cabin";
        case Op::Kind::kInsertNamed: return "insert-named";
        case Op::Kind::kDropTable: return "drop-table";
        case Op::Kind::kPurge: return "purge";
        case Op::Kind::kAlterColumn: return "alter-column";
        case Op::Kind::kSelectJoin: return "select-join";
        case Op::Kind::kSelectAggregate: return "select-aggregate";
        case Op::Kind::kSelectOrdered: return "select-ordered";
        case Op::Kind::kSelectSubquery: return "select-subquery";
        case Op::Kind::kCreateIndex: return "create-index";
        case Op::Kind::kCreateAssertion: return "create-assertion";
    }
    return "unknown";
}

std::optional<Op::Kind> ParseOpKind(std::string_view name) {
    for (const Op::Kind kind :
         {Op::Kind::kCreateTable, Op::Kind::kInsert, Op::Kind::kSelectTupleKey,
          Op::Kind::kSelectRange, Op::Kind::kFilterScan, Op::Kind::kSync, Op::Kind::kUpdate,
          Op::Kind::kDelete, Op::Kind::kBegin, Op::Kind::kCommit, Op::Kind::kRollback,
          Op::Kind::kCreateCabin, Op::Kind::kInsertNamed, Op::Kind::kDropTable,
          Op::Kind::kPurge, Op::Kind::kAlterColumn, Op::Kind::kSelectJoin,
          Op::Kind::kSelectAggregate, Op::Kind::kSelectOrdered, Op::Kind::kSelectSubquery,
          Op::Kind::kCreateIndex, Op::Kind::kCreateAssertion}) {
        if (name == OpKindName(kind)) return kind;
    }
    return std::nullopt;
}

Workload::Workload(Rng rng, Profile profile, bool shapes)
    : rng_(std::move(rng)),
      profile_(profile),
      drop_rng_(rng_.Fork("drop")),
      purge_rng_(rng_.Fork("purge")),
      column_rng_(rng_.Fork("column")),
      shapes_(shapes),
      query_rng_(rng_.Fork("query")),
      constraint_rng_(rng_.Fork("constraint")) {
    for (const char* name : {kFkParent, kFkChild, kAsserted}) {
        constrained_.emplace_back();
        constrained_.back().name = name;
    }
    // 1-3 tables, decided up front so the table set is stable however many
    // ops are drawn. All btree: a heap's tuple key is hidden (BZ-R1), and the
    // oracle reconciles by tuple key; the heap's surface is
    // `heap_hidden_key_test.cpp`'s, its storage this btree's.
    const std::size_t count = 1 + rng_.Below(3);
    for (std::size_t i = 0; i < count; ++i) {
        tables_.push_back(Table{"t" + std::to_string(i), 0});
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

Workload::Table& Workload::PickTableMutable() {
    // A run with nothing dropped draws exactly as it always did.
    const std::vector<std::size_t> live = LiveIndices();
    if (live.size() == tables_.size()) return tables_[rng_.Below(tables_.size())];
    return tables_[live[rng_.Below(live.size())]];
}

// The tables no drop has taken. Every one has been created: `Next()` emits
// a CREATE for each before any other op.
std::vector<std::size_t> Workload::LiveIndices() const {
    std::vector<std::size_t> live;
    for (std::size_t i = 0; i < tables_.size(); ++i) {
        if (!tables_[i].dropped) live.push_back(i);
    }
    return live;
}

// A fresh name in a dropped one's place, created by the next `Next()`.
void Workload::AddReplacement() {
    tables_.push_back(Table{"t" + std::to_string(tables_.size()), 0});
}

Op Workload::Drop() {
    const std::vector<std::size_t> live = LiveIndices();
    const std::size_t index = live[drop_rng_.Below(live.size())];
    tables_[index].dropped = true;
    --drops_left_;
    if (in_txn_) {
        txn_drops_.push_back(index);
    } else {
        AddReplacement();
    }
    Op op;
    op.kind = Op::Kind::kDropTable;
    op.table = tables_[index].name;
    op.sql = "DROP TABLE " + op.table;
    return op;
}

const Workload::Table& Workload::PickTable() { return PickTableMutable(); }

// Mostly hits, sometimes an honest miss just past the end.
std::uint64_t Workload::GuessKey(const Table& table) {
    if (table.high_base != 0 && rng_.Chance(50)) {
        return table.high_base + rng_.Below(table.high_inserted + 3);
    }
    return 1 + rng_.Below(table.inserted + 3);
}

std::uint64_t Workload::FreshKey(Table& table) {
    if (table.high_base == 0) table.high_base = table.fresh + 1;
    return table.fresh--;
}

Op Workload::Insert(Table& table) {
    if (rng_.Chance(30)) return InsertNamed(table);
    Op op;
    op.kind = Op::Kind::kInsert;
    op.table = table.name;
    op.v = NextValue();
    op.name = NextName();
    // Named columns, so a column cycle (`AlterColumn`) never shifts them.
    op.sql = "INSERT INTO " + table.name + " (v, name) VALUES (" + std::to_string(op.v) + ", '" +
             op.name + "')";
    ++(table.high_base != 0 ? table.high_inserted : table.inserted);
    return op;
}

// A named key on a btree (BD-R9): ascending, below the mark, at random, a
// deleted key again, a rolled-back key again, or one outside the id space.
// Inside a transaction only a fresh key, which is always placed: a refusal
// there would poison the transaction the stream goes on writing in.
Op Workload::InsertNamed(Table& table) {
    const auto pick = [&](const std::vector<std::uint64_t>& keys) {
        return keys[rng_.Below(keys.size())];
    };
    std::uint64_t key = 0;
    if (in_txn_) {
        key = FreshKey(table);
        txn_named_.emplace_back(static_cast<std::size_t>(&table - tables_.data()), key);
    } else {
        const std::uint64_t roll = rng_.Below(100);
        if (roll < 20) {
            key = (table.high_base != 0 ? table.high_base + table.high_inserted
                                        : table.inserted + 1) +
                  rng_.Below(3);  // near the top
        } else if (roll < 45) {
            key = 1 + rng_.Below(table.inserted + 1);  // below the mark
        } else if (roll < 60) {
            key = 1 + rng_.Below(table.inserted * 3 + 20);
        } else if (roll < 68 && !table.purged.empty()) {
            key = pick(table.purged);
        } else if (roll < 75 && !table.deleted.empty()) {
            key = pick(table.deleted);
        } else if (roll < 92 && !table.rolled_back.empty()) {
            key = pick(table.rolled_back);
        } else if (roll < 96) {
            key = FreshKey(table);
        } else {
            key = rng_.Chance(50) ? 0 : kIdSpaceEnd;  // exhausted
        }
    }
    Op op;
    op.kind = Op::Kind::kInsertNamed;
    op.table = table.name;
    op.key = key;
    op.v = NextValue();
    op.name = NextName();
    op.sql = "INSERT INTO " + table.name + " (id, v, name) VALUES (" + std::to_string(key) +
             ", " + std::to_string(op.v) + ", '" + op.name + "')";
    return op;
}

Op Workload::Update(const Table& table) {
    Op op;
    op.kind = Op::Kind::kUpdate;
    op.table = table.name;
    op.by_tuple_key = rng_.Chance(70);
    op.set_name = rng_.Chance(40);
    op.sql = "UPDATE " + table.name + " SET ";
    if (op.set_name) {
        op.name = NextName();
        op.sql += "name = '" + op.name + "'";
    } else {
        op.v = NextValue();
        op.sql += "v = " + std::to_string(op.v);
    }
    if (op.by_tuple_key) {
        op.key = GuessKey(table);
        op.sql += " WHERE id = " + std::to_string(op.key);
    } else {
        op.pred_v = NextValue();
        op.sql += " WHERE v = " + std::to_string(op.pred_v);
    }
    return op;
}

Op Workload::Delete(Table& table) {
    Op op;
    op.kind = Op::Kind::kDelete;
    op.table = table.name;
    op.by_tuple_key = rng_.Chance(70);
    op.sql = "DELETE FROM " + table.name;
    if (op.by_tuple_key) {
        op.key = GuessKey(table);
        op.sql += " WHERE id = " + std::to_string(op.key);
        // A key a named INSERT names again later: a duplicate for good if
        // this delete matched and committed (BD-R4), the oracle decides.
        if (table.deleted.size() < 64) table.deleted.push_back(op.key);
    } else {
        op.pred_v = NextValue();
        op.sql += " WHERE v = " + std::to_string(op.pred_v);
    }
    return op;
}

// A key a PURGE names: a deleted one mostly, sometimes any key the table
// may hold - live, never placed, or past the end - all from `purge_rng_`,
// so the main stream draws what it always drew.
Op Workload::Purge(Table& table) {
    const auto guess = [&] { return 1 + purge_rng_.Below(table.inserted + 3); };
    const std::uint64_t roll = purge_rng_.Below(100);
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    if (roll < 55 && !table.deleted.empty()) {
        lo = hi = table.deleted[purge_rng_.Below(table.deleted.size())];
    } else if (roll < 85) {
        lo = guess();
        hi = lo + purge_rng_.Below(64);
    } else {
        lo = hi = guess();
    }
    for (const std::uint64_t k : table.deleted) {
        if (k >= lo && k <= hi && table.purged.size() < 64) table.purged.push_back(k);
    }
    Op op;
    op.kind = Op::Kind::kPurge;
    op.table = table.name;
    op.lo = lo;
    op.hi = hi;
    op.sql = "PURGE FROM " + table.name + " WHERE " +
             (lo == hi ? "id = " + std::to_string(lo)
                       : "id BETWEEN " + std::to_string(lo) + " AND " + std::to_string(hi));
    return op;
}

Op Workload::DataOp() {
    const std::uint64_t roll = rng_.Below(100);
    if (roll < 42) return Insert(PickTableMutable());
    if (roll < 57) return Update(PickTable());
    if (roll < 64) {
        // **A PURGE takes a DELETE's place, after the DELETE drew** (BH-S4):
        // the main stream draws what it always drew and every later op keeps
        // its position, so a seed's op-indexed faults and crashes land where
        // they did (the H9 pin's among them). Autocommit only (BH-Q4), btree
        // only (BH-Q9).
        Table& table = PickTableMutable();
        Op del = Delete(table);
        if (in_txn_ || !purge_rng_.Chance(60)) return del;
        // The DELETE never runs, so its key is no deleted key to name again.
        if (del.by_tuple_key && !table.deleted.empty() && table.deleted.back() == del.key) {
            table.deleted.pop_back();
        }
        return Purge(table);
    }
    if (roll < 79) {
        const Table& table = PickTable();
        Op op;
        op.kind = Op::Kind::kSelectTupleKey;
        op.table = table.name;
        op.key = GuessKey(table);
        op.sql = std::string("SELECT ") + kReadColumns + " FROM " + table.name + " WHERE id = " + std::to_string(op.key);
        return op;
    }
    if (roll < 88) {
        const Table& table = PickTable();
        Op op;
        op.kind = Op::Kind::kSelectRange;
        op.table = table.name;
        op.lo = GuessKey(table);
        op.hi = op.lo + rng_.Below(20);
        op.sql = std::string("SELECT ") + kReadColumns + " FROM " + table.name + " WHERE id BETWEEN " + std::to_string(op.lo) +
                 " AND " + std::to_string(op.hi);
        return op;
    }
    const Table& table = PickTable();
    Op op;
    op.kind = Op::Kind::kFilterScan;
    op.table = table.name;
    op.v = NextValue();
    op.sql = std::string("SELECT ") + kReadColumns + " FROM " + table.name + " WHERE v = " + std::to_string(op.v);
    return op;
}

Op Workload::AlterColumn() {
    const std::vector<std::size_t> live = LiveIndices();
    const Table& table = tables_[live[column_rng_.Below(live.size())]];
    Op op;
    op.kind = Op::Kind::kAlterColumn;
    op.table = table.name;
    op.key = column_rng_.Below(4);
    switch (op.key) {
        case kStepAdd: op.sql = "ALTER TABLE " + table.name + " ADD COLUMN x varchar(16) NULL"; break;
        case kStepFill:
            op.sql = "UPDATE " + table.name + " SET x = 'a-value-longer-than-thirteen-" +
                     std::to_string(column_rng_.Below(1000)) + "'";
            break;
        case kStepDrop: op.sql = "ALTER TABLE " + table.name + " DROP COLUMN x"; break;
        default: op.sql = "PURGE DROPPED COLUMNS FROM " + table.name; break;
    }
    return op;
}

Op Workload::Next() {
    if (created_ < tables_.size()) {
        const Table& table = tables_[created_];
        ++created_;
        Op op;
        op.kind = Op::Kind::kCreateTable;
        op.table = table.name;
        // One slot, for the column cycle (`AlterColumn`).
        op.sql = "CREATE TABLE " + table.name + " (id int64, v int64, name varchar) SPARE 1";
        return op;
    }
    if (shapes_ && constrained_ddl_ < 4 && !in_txn_) return ConstrainedDdl();

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
            // A rolled-back named key is free again (W12): a later named
            // INSERT takes it.
            if (!commit) {
                for (const auto& [index, key] : txn_named_) {
                    tables_[index].rolled_back.push_back(key);
                }
            }
            txn_named_.clear();
            // A committed drop's name gets its replacement; a rolled-back
            // one is live again.
            for (const std::size_t index : txn_drops_) {
                if (commit) {
                    AddReplacement();
                } else {
                    tables_[index].dropped = false;
                    ++drops_left_;
                }
            }
            txn_drops_.clear();
            return op;
        }
        --txn_ops_left_;
        if (drops_left_ != 0 && LiveIndices().size() >= 2 && drop_rng_.Chance(3)) return Drop();
        if (shapes_ && query_rng_.Chance(8)) return Query(/*ddl=*/false);
        return DataOp();
    }

    if (drops_left_ != 0 && LiveIndices().size() >= 2 && drop_rng_.Chance(2)) return Drop();

    if (column_rng_.Chance(4)) return AlterColumn();

    if (shapes_ && query_rng_.Chance(8)) return Query(/*ddl=*/true);
    if (shapes_ && constraint_rng_.Chance(12)) return ConstrainedOp();

    // 85 data ops, 4 syncs, 3 Cabin declarations and 8 transaction starts
    // per hundred rolls. Three of the data ops were `CREATE PATTERN` until
    // the operator withdrew declared patterns on 2026-08-31; the band went
    // to data rather than to any other kind so that the transaction rate -
    // which is what the crash cases are drawn against - did not move with
    // it.
    const std::uint64_t roll = rng_.Below(100);
    if (roll < 85) return DataOp();
    if (roll < 89) {
        Op op;
        op.kind = Op::Kind::kSync;
        op.sql = "SYNC";
        return op;
    }
    if (roll < 92) {
        // A Cabin on the only column that can carry one here: the tuple key is
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
    in_txn_ = true;
    txn_ops_left_ = 1 + rng_.Below(8);
    Op op;
    op.kind = Op::Kind::kBegin;
    op.sql = rng_.Chance(50) ? "BEGIN" : "BEGIN ISOLATION LEVEL REPEATABLE READ";
    return op;
}

Op Workload::Query(bool ddl) {
    const std::vector<std::size_t> live = LiveIndices();
    const Table& a = tables_[live[query_rng_.Below(live.size())]];
    const Table& b = tables_[live[query_rng_.Below(live.size())]];
    Op op;
    op.table = a.name;
    const std::uint64_t roll = query_rng_.Below(ddl ? 5 : 4);
    if (roll == 4 && indexed_.insert(a.name).second) {
        op.kind = Op::Kind::kCreateIndex;
        op.sql = "CREATE INDEX " + a.name + "_v ON " + a.name + " (v)";
        return op;
    }
    switch (roll) {
        case 0:
            // A window of ids keeps the join's answer small whatever the
            // profile: a colliding `v` pairs every row with a fifth of the other.
            op.kind = Op::Kind::kSelectJoin;
            op.name = b.name;
            op.lo = 1 + query_rng_.Below(std::max<std::uint64_t>(a.inserted, 1));
            op.hi = op.lo + 20;
            op.sql = "SELECT x.id, y.id FROM " + a.name + " AS x JOIN " + b.name +
                     " AS y ON x.v = y.v WHERE x.id BETWEEN " + std::to_string(op.lo) + " AND " +
                     std::to_string(op.hi);
            return op;
        case 2:
            op.kind = Op::Kind::kSelectOrdered;
            op.key = 1 + query_rng_.Below(10);
            op.sql = "SELECT id, v FROM " + a.name + " ORDER BY v DESC, id LIMIT " +
                     std::to_string(op.key);
            return op;
        case 3:
            op.kind = Op::Kind::kSelectSubquery;
            op.name = b.name;
            op.sql = "SELECT id FROM " + a.name + " WHERE v IN (SELECT v FROM " + b.name + ")";
            return op;
        default:
            op.kind = Op::Kind::kSelectAggregate;
            op.sql = "SELECT v, COUNT(*), MIN(id), MAX(id) FROM " + a.name + " GROUP BY v";
            return op;
    }
}

Op Workload::ConstrainedDdl() {
    Op op;
    switch (constrained_ddl_++) {
        case 0:
            op.kind = Op::Kind::kCreateTable;
            op.table = kFkParent;
            op.sql = std::string("CREATE TABLE ") + kFkParent +
                     " (id int64, v int64, name varchar) SPARE 1";
            return op;
        case 2:
            op.kind = Op::Kind::kCreateTable;
            op.table = kAsserted;
            op.sql = std::string("CREATE TABLE ") + kAsserted +
                     " (id int64, v int64, name varchar) SPARE 1";
            return op;
        case 1:
            op.kind = Op::Kind::kCreateTable;
            op.table = kFkChild;
            op.sql = std::string("CREATE TABLE ") + kFkChild + " (id int64, v int64 REFERENCES " +
                     kFkParent + ", name varchar) SPARE 1";
            return op;
        default:
            op.kind = Op::Kind::kCreateAssertion;
            op.table = kAsserted;
            op.sql = std::string("CREATE ASSERTION ") + kAsserted + "_cap ON " + kAsserted +
                     " GROUP BY (v) CHECK COUNT(*) <= " + std::to_string(kAssertBound);
            return op;
    }
}

Op Workload::ConstrainedOp() {
    Rng& r = constraint_rng_;
    Table& parent = constrained_[0];
    Table& child = constrained_[1];
    Table& asserted = constrained_[2];
    const auto guess = [&](const Table& t) { return 1 + r.Below(t.inserted + 2); };
    // Two draws in two statements: as constructor arguments their order is
    // unspecified, and gcc and clang would draw different names (rng.hpp's
    // cross-platform byte-identity).
    const auto name = [&] {
        const std::size_t len = 1 + r.Below(6);
        return std::string(len, static_cast<char>('a' + r.Below(26)));
    };
    const auto insert = [&](Table& t, std::int64_t v) {
        Op op;
        op.kind = Op::Kind::kInsert;
        op.table = t.name;
        op.v = v;
        op.name = name();
        op.sql = "INSERT INTO " + t.name + " (v, name) VALUES (" + std::to_string(op.v) + ", '" +
                 op.name + "')";
        ++t.inserted;
        return op;
    };
    const auto remove = [&](const Table& t) {
        Op op;
        op.kind = Op::Kind::kDelete;
        op.table = t.name;
        op.by_tuple_key = true;
        op.key = guess(t);
        op.sql = "DELETE FROM " + t.name + " WHERE id = " + std::to_string(op.key);
        return op;
    };
    const std::uint64_t roll = r.Below(100);
    if (roll < 15) return insert(parent, r.Range(0, 999));
    // Mostly a parent that exists, sometimes one that does not.
    if (roll < 40) {
        return insert(child, r.Chance(80) ? static_cast<std::int64_t>(guess(parent))
                                          : r.Range(0, 999));
    }
    if (roll < 50) return remove(parent);
    if (roll < 55) return remove(child);
    if (roll < 75) return insert(asserted, r.Range(0, 4));
    if (roll < 90) {
        Op op;
        op.kind = Op::Kind::kUpdate;
        op.table = asserted.name;
        op.v = r.Range(0, 4);
        op.by_tuple_key = roll < 85;
        op.sql = "UPDATE " + asserted.name + " SET v = " + std::to_string(op.v);
        if (op.by_tuple_key) {
            op.key = guess(asserted);
            op.sql += " WHERE id = " + std::to_string(op.key);
        } else {
            op.pred_v = r.Range(0, 4);
            op.sql += " WHERE v = " + std::to_string(op.pred_v);
        }
        return op;
    }
    if (roll < 94) return remove(asserted);
    if (roll < 98 || child_cabin_) {
        const Table& t = constrained_[r.Below(constrained_.size())];
        Op op;
        op.kind = Op::Kind::kSelectRange;
        op.table = t.name;
        op.lo = 1 + r.Below(t.inserted + 1);
        op.hi = op.lo + 10;
        op.sql = std::string("SELECT ") + kReadColumns + " FROM " + t.name + " WHERE id BETWEEN " +
                 std::to_string(op.lo) + " AND " + std::to_string(op.hi);
        return op;
    }
    // A Cabin over the foreign key's column: a parent's delete may then be
    // answered from the child's set (AY-S6).
    child_cabin_ = true;
    Op op;
    op.kind = Op::Kind::kCreateCabin;
    op.table = child.name;
    op.sql = "CREATE CABIN ON " + child.name + "(v)";
    return op;
}

}  // namespace kds::sim
