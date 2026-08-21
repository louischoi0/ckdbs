#pragma once

// sim/oracle.hpp — the reference model (bench/workplan-teststrategy SIM03,
// grown by SIM06). The dumbest thing that can be right: per relation a
// std::map<pk, Row>, updated on every **acknowledged** write, queried on
// every read. It never sees the engine's internals and it does not know the
// feature toggles exist — divergence from it is the definition of a wrong
// answer.
//
// MarkSynced() snapshots the acknowledged state; the crash loop reconciles
// a restarted instance against the snapshot (what SYNC promised) and the
// full state (what recovery promises).
//
// **Transactions** (SIM06) are a pending write-set and nothing more: BEGIN
// copies the committed state aside, every write lands in the copy, COMMIT
// makes the copy the committed state and ROLLBACK drops it. Reads inside
// the transaction see the copy, which is own-writes visibility, and that
// is exactly enough for the single session this harness drives. Multi-
// session semantics are S-3's job and want a history checker, not a
// smarter map.
//
// **Two kinds of unknown**, both introduced by fault injection (SIM05),
// both meaning "this row is nobody's to assert on":
//
//   *Unnamed rows* — an INSERT the engine answered with an error. Its
//   commit record may have reached the device, so the row may be there or
//   not, and **no id was ever reported**. What identifies it afterwards is
//   therefore the one thing that stays true: its id is not one the engine
//   ever named. So a relation with an errored INSERT outstanding permits
//   rows whose id it never issued, and never requires them.
//
//   Keying that on the row's *content* instead was wrong twice, and both
//   ways were live: an errored INSERT's ghost that a later acknowledged
//   `UPDATE` moved off its inserted (v, name) stopped being permitted and
//   read as a fabrication (a false alarm the fault corpus hit on four of
//   five seeds), and an accepted row whose content merely *collided* with
//   an errored insert's became excusable when the restart lost it (a real
//   loss, silently forgiven). An id the oracle issued is always the
//   oracle's to assert on; an id it never issued never is.
//
//   *Unchecked ids* — a write whose id **is** known but whose outcome is
//   not: an errored UPDATE or DELETE, or every row a transaction touched
//   when its COMMIT answered an error. Those ids drop out of both sides of
//   every comparison from then on.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
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

    // `id = key` when by_pk, else `v = v`. The two shapes UPDATE and
    // DELETE are generated with (sim/workload.hpp).
    struct Predicate {
        bool by_pk = true;
        std::uint64_t key = 0;
        std::int64_t v = 0;
    };

    // What an UPDATE's SET assigns. The pk is never assignable
    // (invariant 11), so there are exactly two columns to choose between.
    struct Assignment {
        bool set_name = false;
        std::int64_t v = 0;
        std::string name;
    };

    void CreateTable(const std::string& table) {
        tables_[table];
        if (in_txn_) working_[table];
    }
    bool HasTable(const std::string& table) const { return tables_.count(table) != 0; }

    // ---- Transactions ---------------------------------------------------

    void Begin() {
        working_ = tables_;
        touched_.clear();
        in_txn_ = true;
    }
    void Commit() {
        tables_ = std::move(working_);
        working_.clear();
        touched_.clear();
        in_txn_ = false;
    }
    void Rollback() {
        working_.clear();
        touched_.clear();
        in_txn_ = false;
    }
    // The transaction's outcome is unknown — its COMMIT answered an error.
    // Every row it touched stops being anyone's to assert on.
    void Abandon() {
        for (const auto& [table, ids] : touched_) {
            for (const std::uint64_t id : ids) unchecked_[table].insert(id);
        }
        Rollback();
    }

    // ---- Writes ---------------------------------------------------------

    void ApplyInsert(const std::string& table, std::uint64_t id, OracleRow row) {
        Live()[table][id] = std::move(row);
        // Every id the engine ever named, including one a later DELETE or a
        // ROLLBACK removed: ids are issued once and never rebound
        // (invariant 11), so this is exactly the complement of "a row the
        // engine never named", which is what an errored INSERT can leave.
        issued_[table].insert(id);
        Touch(table, id);
    }

    // Rows the predicate matches, which is what the engine's `UPDATED <n>`
    // / `DELETED <n>` is compared against.
    std::size_t ApplyUpdate(const std::string& table, const Predicate& where,
                            const Assignment& set);
    std::size_t ApplyDelete(const std::string& table, const Predicate& where);

    // ---- Unknown outcomes -----------------------------------------------

    // An INSERT the engine answered with an error: from here on this
    // relation may hold rows whose id it never named.
    void NoteIndeterminate(const std::string& table) { ++indeterminate_[table]; }
    void NoteUnchecked(const std::string& table, std::uint64_t id) {
        unchecked_[table].insert(id);
    }
    // Every id the predicate matches right now — what an errored UPDATE or
    // DELETE has to mark, since either outcome is possible.
    std::vector<std::uint64_t> Matching(const std::string& table,
                                        const Predicate& where) const;

    // Whether a mutation's reported row count can be compared at all.
    // Once a relation holds an unknown, most counts cannot: a predicate on
    // a *value* may match a row whose fate the oracle does not know. A
    // predicate on the **pk** names one row, so it stays checkable as long
    // as that one id is not itself unknown — which is 70% of the mutations
    // the generator emits, and the difference between the sharpest
    // assertion in the harness running on 6% of them and on most.
    bool CountCheckable(const std::string& table, const Predicate& where) const;

    // Whether a rendered "<id>,<v>,<name>" row is one no comparison may
    // rest on: its id is unchecked, or the relation has an errored INSERT
    // outstanding and this id is one the engine never named.
    bool Ignorable(const std::string& table, std::string_view rendered) const;

    // False on every run with no faults, which is what keeps the exact
    // comparison the common path.
    bool has_unknowns() const noexcept {
        return !indeterminate_.empty() || !unchecked_.empty();
    }
    // Per relation, which is the granularity a row count needs: one
    // relation's unknown says nothing about another's.
    bool has_unknowns(const std::string& table) const {
        return indeterminate_.count(table) != 0 || unchecked_.count(table) != 0;
    }

    void MarkSynced() { synced_ = tables_; }

    // The committed state. A transaction in flight is deliberately not
    // visible here: this is what a crash would leave and what the restart
    // is reconciled against.
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
    // The committed state of one relation, unchecked ids dropped.
    std::vector<std::string> ExpectAll(const std::string& table) const;

private:
    // The view writes land in and reads are answered from: the pending
    // copy while a transaction is open, the committed state otherwise.
    std::map<std::string, TableRows>& Live() { return in_txn_ ? working_ : tables_; }
    const std::map<std::string, TableRows>& Live() const {
        return in_txn_ ? working_ : tables_;
    }

    void Touch(const std::string& table, std::uint64_t id) {
        if (in_txn_) touched_[table].insert(id);
    }

    bool IsUnchecked(const std::string& table, std::uint64_t id) const;

    std::map<std::string, TableRows> tables_;
    std::map<std::string, TableRows> synced_;

    bool in_txn_ = false;
    std::map<std::string, TableRows> working_;
    std::map<std::string, std::set<std::uint64_t>> touched_;

    std::map<std::string, std::set<std::uint64_t>> issued_;
    std::map<std::string, std::size_t> indeterminate_;
    std::map<std::string, std::set<std::uint64_t>> unchecked_;
};

}  // namespace kds::sim
