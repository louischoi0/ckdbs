#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/function_ref.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/manager.hpp"

// **The clean of one row's dropped cells** (CD-S2,
// `instructions/v3.2.0/workorder-cd-dropped-column-cleaning.md` §2-§3).
//
// Protocol, in order, and the order is the correctness:
//
//   1. `btree::BtreeLookup(kWrite)` holds the key's leaf exclusive, and the
//      row is re-read under that hold.
//   2. A row already clean is left alone. A row whose header writer is not
//      resolved for every reader is left alone and reported: only a head
//      every live and future view takes as committed may change in place,
//      because then no reader walks its undo chain and no compensation
//      restores a dirty before-image over it (§3 point 1, CD-S0 P3). A
//      delete-marked head is judged by the same rule; its writer is the
//      deleter.
//   3. The dropped cells' spills are read, the cells zeroed and their null
//      bits set, and the row rewritten in place with `HEAP_OVERWRITE` at
//      `wal::kNoTxnId`, keeping the header's writer and undo pointer - all
//      **under that hold**, with nothing between that can park (CD-S0 P3).
//      No version is made: the bytes changed are ones no decoder reads.
//   4. The hold is released, and each spill is released with
//      `VARHEAP_RELEASE` at `wal::kNoTxnId`.
//
// **Clean first, release second, and never the reverse** (`[quiet-wrong]`):
// a crash between the two leaks the row's spills, the gap `PURGE` states
// too (BH-R7); the reverse would leave a cell naming a released slot, which
// a re-run would release a second time - by then possibly another value's.
// The cells are zeroed, not only flagged NULL, because `RowSpills` keys on
// the tag and ignores the bits (CD-S0 P2).
//
// The envelope is `kNoTxnId`, so analysis never counts the header's writer
// as a loser and undo never reverses the clean (LogCatRow's B1 note in
// `catalog.cpp`). No record kind is added.
//
// The caller holds the relation's `IX` and has passed the statement-epoch
// gate, so no statement that bound the column as live still runs (§3
// point 3); this function judges the row alone.

namespace kds::exec {

// The cells a clean zeroes: every dropped column, in position order
// (CD-OI-8 (a)). Empty for a relation with none.
std::vector<std::uint16_t> DroppedColumns(const catalog::TableAccess& access);

// Whether `payload` is clean at `targets`: each cell zero, and its null bit
// set where the column has one.
bool DroppedCellsClean(const catalog::TableAccess& access, std::span<const std::uint16_t> targets,
                       std::span<const std::byte> payload);

enum class CleanOutcome : std::uint8_t {
    kCleaned = 0,
    kAlreadyClean = 1,
    kUnresolved = 2,  // its writer is not resolved for every reader; nothing written
    kAbsent = 3,      // no slot keyed `id`: rolled back or purged since the walk
};

struct CleanResult {
    CleanOutcome outcome = CleanOutcome::kAbsent;
    std::uint64_t trx_id = 0;  // the header's writer, for kCleaned and kUnresolved
    std::uint32_t spills_released = 0;
};

// `resolved` is asked of the header's writer under the hold:
// `TransactionManager::ResolvedForEveryReader`, or always true where there
// is no manager.
StatusOr<CleanResult> CleanDroppedCells(storage::PageStore& store, wal::WalManager* wal,
                                        const catalog::TableAccess& access,
                                        std::span<const std::uint16_t> targets, std::uint64_t id,
                                        FunctionRef<bool(std::uint64_t)> resolved);

}  // namespace kds::exec
