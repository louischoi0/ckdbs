#pragma once

// sim/workload.hpp — the workload generator (bench/workplan-teststrategy
// SIM03, grown to v2 by SIM06). Emits SQL **text** — the same front door
// every client uses, so the parser, the compiler and the step VM are all
// inside the tested surface — from a seeded grammar:
//
//     CREATE TABLE (heap and btree clustered_type; int columns plus one
//     varchar, sized to land on both sides of inline_cell_width)
//     INSERT, pk point SELECT, pk BETWEEN range, non-pk FilterScan, SYNC
//     UPDATE and DELETE, each by pk and by a non-key predicate   (v2)
//     BEGIN / COMMIT / ROLLBACK, isolation level drawn per transaction (v2)
//     CREATE CABIN                                                (v2)
//
// The generator is engine-independent on purpose: it never reads a reply.
// Point-lookup keys are guessed from its own insert counter (ids are
// issued 1, 2, 3, … per relation, so the guess is usually a hit and
// sometimes an honest miss — both are states worth generating). The
// oracle, not the generator, learns the *actual* ids from the replies.
//
// **What v2's two mutation shapes are for**, since they look like one op
// with a flag: an UPDATE that assigns `v` rewrites the column a Cabin and
// a secondary index are keyed on, and an UPDATE that assigns `name`
// rewrites a tagged cell that may spill to the var-heap. The engine's
// rules differ across that line (the Cabin's append-only rule, var-heap
// immutability per version, invariant 13's fixed row size) and a generator
// that only ever wrote one of them would exercise half of it. The primary
// key is never assigned: invariant 11 makes that a compile-time refusal,
// not a case to generate — SIM12's job, not this one's.
//
// Transactions are generated as whole units — BEGIN, a seeded number of
// data ops, then COMMIT or ROLLBACK — and never nest, because a second
// BEGIN is an error rather than a shape. DDL and SYNC stay outside them:
// `CREATE TABLE` is emitted up front, and cabins and patterns are not
// transactional at all (`manual/sql/sql.md` §5).
//
// Determinism contract (SIM01): the op sequence is a pure function of the
// Rng handed in. Same seed, same ops, byte for byte.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "sim/rng.hpp"

namespace kds::sim {

enum class Profile : std::uint8_t {
    kUniform = 0,    // v uniform over [0, 999]
    kZipfian = 1,    // v skewed toward small values
    kColliding = 2,  // v over [0, 4]: what makes FilterScan sets interesting
};

const char* ProfileName(Profile profile);
std::optional<Profile> ParseProfile(std::string_view name);

struct Op {
    enum class Kind : std::uint8_t {
        kCreateTable,
        kInsert,
        kSelectPk,
        kSelectRange,
        kFilterScan,
        kSync,
        kUpdate,
        kDelete,
        kBegin,
        kCommit,
        kRollback,
        kCreateCabin,
        // A named key (BD, `instructions/v3.0.0/workorder-bd-sorted-leaf-
        // named-keys.md`): `key` is the pk the INSERT names. Placed where it
        // sorts, or refused as a duplicate or as exhausted (BD-R5) - the
        // oracle says which.
        kInsertNamed,
        // DROP TABLE (BF, `instructions/v3.0.0/workorder-bf-drop-table-page-
        // reclaim.md` BF-R12), inside a transaction and out of one. A
        // dropped name is never created again; a fresh one takes its place,
        // so the relation count holds and the next mount's reclaim has pages
        // a later creation can reuse.
        kDropTable,
    };
    Kind kind;
    std::string table;
    std::string sql;
    // Semantic fields the oracle needs to build its expectation.
    std::uint64_t key = 0;       // kSelectPk, kInsertNamed; kUpdate/kDelete when by_pk
    std::uint64_t lo = 0;        // kSelectRange
    std::uint64_t hi = 0;        // kSelectRange
    std::int64_t v = 0;          // kInsert(Named), kFilterScan; kUpdate's new v
    std::string name;            // kInsert(Named); kUpdate's new name
    bool btree = false;          // kCreateTable

    // kUpdate / kDelete: the predicate is `id = key` when by_pk, else
    // `v = pred_v`. kUpdate assigns `name` when set_name, else `v`.
    bool by_pk = false;
    std::int64_t pred_v = 0;
    bool set_name = false;
};

// The kind's spelling in a `.sim` case file (sim/minimize.hpp), and the
// reverse. An unknown spelling yields nullopt rather than a guess.
const char* OpKindName(Op::Kind kind);
std::optional<Op::Kind> ParseOpKind(std::string_view name);

class Workload {
public:
    Workload(Rng rng, Profile profile);

    // The next operation. The first calls yield the CREATE TABLEs; after
    // that the mix is seed-driven.
    Op Next();

private:
    struct Table {
        std::string name;
        bool btree;
        std::uint64_t inserted = 0;  // the id-guessing counter, not truth
        // Keys a named INSERT reuses (BD-R9): ones a DELETE by pk named,
        // and ones a rolled-back transaction named - a deleted key is a
        // duplicate for good, a rolled-back one is free (W12).
        std::vector<std::uint64_t> deleted;
        std::vector<std::uint64_t> rolled_back;
        // Inside a transaction a named key is drawn from here, descending:
        // below 2^39 no issue reaches (issues start low and jump above the
        // first such key), so it is always free there - a refusal inside a
        // transaction would poison it and end the stream's transaction.
        std::uint64_t fresh = std::uint64_t{1} << 39;
        // The first fresh key moves the mark to just above it, so every id
        // issued after it lands in a high band: `high_base` is its first id,
        // and `high_inserted` counts the omitted-pk inserts into it, so the
        // pk-targeted ops can still aim at the rows issued there.
        std::uint64_t high_base = 0;
        std::uint64_t high_inserted = 0;
        // Dropped, or dropped by the open transaction: no op names it again.
        bool dropped = false;
    };

    std::int64_t NextValue();
    std::string NextName();
    const Table& PickTable();
    Table& PickTableMutable();
    std::uint64_t GuessKey(const Table& table);
    // A key below 2^39 no issue reaches (`Table::fresh`), noting the band
    // issued ids jump to once the first one moves the mark.
    std::uint64_t FreshKey(Table& table);

    // The data-op half of the mix: everything legal inside a transaction
    // and out of one.
    Op DataOp();
    Op Insert(Table& table);
    Op InsertNamed(Table& table);
    Op Update(const Table& table);
    Op Delete(Table& table);
    // DROP TABLE of a live relation, never the last one. Drawn from its own
    // stream (`drop_rng_`), so a seed's other ops are the ops it always drew.
    Op Drop();
    std::vector<std::size_t> LiveIndices() const;
    void AddReplacement();

    Rng rng_;
    Profile profile_;
    std::vector<Table> tables_;
    std::size_t created_ = 0;

    bool in_txn_ = false;
    std::size_t txn_ops_left_ = 0;
    // The named keys the open transaction placed, by table index: a
    // rollback frees them for reuse.
    std::vector<std::pair<std::size_t, std::uint64_t>> txn_named_;

    // Cabins are named by (table, column) and a second CREATE on the same
    // pair is an error, so the generator remembers what it declared.
    std::set<std::string> cabins_;

    // The drops (BF-R12): their own stream, a budget of two an iteration,
    // and the ones the open transaction made, which a rollback undoes.
    Rng drop_rng_;
    std::size_t drops_left_ = 2;
    std::vector<std::size_t> txn_drops_;
};

}  // namespace kds::sim
