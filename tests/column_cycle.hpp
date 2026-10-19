#pragma once

#include <string>

#include <gtest/gtest.h>

// **The contract suites' column-cycle configuration** (CD-S4,
// `instructions/v3.2.0/workorder-cd-dropped-column-cleaning.md` §9): a
// relation whose last column reached its slot through a dropped column's -
// added, written with a spilled value, dropped, cleaned by `PURGE DROPPED
// COLUMNS`, the slot taken again - must answer every query as one whose
// column took a vacant slot. Both shapes reserve one slot, so a suite
// compares `kReused` against `kVacant` (or against its plain baseline when
// its queries name their columns).

namespace kds::testing_cycle {

enum class Added { kNone, kVacant, kReused };

// The `CREATE TABLE` clause each shape needs, placed before `BTREE`/`HEAP`.
inline const char* Spare(Added how) { return how == Added::kNone ? "" : " SPARE 1"; }

// Gives `table` its column `cd_added` the way `how` says. `run` dispatches
// one statement and returns its reply.
template <typename Run>
void AddColumn(Run&& run, const std::string& table, Added how) {
    if (how == Added::kNone) return;
    if (how == Added::kReused) {
        EXPECT_EQ(run("ALTER TABLE " + table + " ADD COLUMN cd_gone varchar(16) NULL"),
                  "ADDED COLUMN " + table + ".cd_gone");
        const std::string updated =
            run("UPDATE " + table + " SET cd_gone = 'a-dropped-value-that-spills'");
        EXPECT_EQ(updated.rfind("UPDATED", 0), 0u) << updated;
        EXPECT_EQ(run("ALTER TABLE " + table + " DROP COLUMN cd_gone"),
                  "DROPPED COLUMN " + table + ".cd_gone");
        const std::string cleaned = run("PURGE DROPPED COLUMNS FROM " + table);
        EXPECT_EQ(cleaned.rfind("PURGED DROPPED COLUMNS", 0), 0u) << cleaned;
    }
    EXPECT_EQ(run("ALTER TABLE " + table + " ADD COLUMN cd_added int64 NULL"),
              "ADDED COLUMN " + table + ".cd_added");
}

}  // namespace kds::testing_cycle
