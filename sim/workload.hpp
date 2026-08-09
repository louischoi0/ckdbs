#pragma once

// sim/workload.hpp — workload generator v1 (bench/workplan-teststrategy
// SIM03). Emits SQL **text** — the same front door every client uses, so
// the parser, the compiler and the step VM are all inside the tested
// surface — from a seeded grammar:
//
//     CREATE TABLE (heap and btree clustered_type; int columns plus one
//     varchar, sized to land on both sides of inline_cell_width)
//     INSERT, pk point SELECT, pk BETWEEN range, non-pk FilterScan, SYNC
//
// The generator is engine-independent on purpose: it never reads a reply.
// Point-lookup keys are guessed from its own insert counter (ids are
// issued 1, 2, 3, … per relation, so the guess is usually a hit and
// sometimes an honest miss — both are states worth generating). The
// oracle, not the generator, learns the *actual* ids from the replies.
//
// Determinism contract (SIM01): the op sequence is a pure function of the
// Rng handed in. Same seed, same ops, byte for byte.

#include <cstdint>
#include <string>
#include <vector>

#include "sim/rng.hpp"

namespace kds::sim {

enum class Profile : std::uint8_t {
    kUniform = 0,    // v uniform over [0, 999]
    kZipfian = 1,    // v skewed toward small values
    kColliding = 2,  // v over [0, 4]: what makes FilterScan sets interesting
};

const char* ProfileName(Profile profile);

struct Op {
    enum class Kind : std::uint8_t {
        kCreateTable,
        kInsert,
        kSelectPk,
        kSelectRange,
        kFilterScan,
        kSync,
    };
    Kind kind;
    std::string table;
    std::string sql;
    // Semantic fields the oracle needs to build its expectation.
    std::uint64_t key = 0;       // kSelectPk
    std::uint64_t lo = 0;        // kSelectRange
    std::uint64_t hi = 0;        // kSelectRange
    std::int64_t v = 0;          // kInsert, kFilterScan
    std::string name;            // kInsert
    bool btree = false;          // kCreateTable
};

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
    };

    std::int64_t NextValue();
    std::string NextName();
    const Table& PickTable();
    Table& PickTableMutable();

    Rng rng_;
    Profile profile_;
    std::vector<Table> tables_;
    std::size_t created_ = 0;
};

}  // namespace kds::sim
