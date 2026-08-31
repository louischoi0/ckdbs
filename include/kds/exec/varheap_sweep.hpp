#pragma once

#include <cstdint>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/manager.hpp"

// H7 — the mount-time sweep that collects a spill nothing points at.
//
// ---- Why one relation's spills leak and every other's does not -----------
//
// Since 2026-08-28 a rolled-back spill is released like any other write: the
// `VARHEAP_APPEND` is a link in the loser's own undo chain, so a live
// rollback and recovery's undo phase both reach it. That closed the general
// case and left one hole, which is what this file is for.
//
// `exec::LogChainInsert` logs its spills under `wal::kNoTxnId`, and **one**
// caller takes that path: the assertion catalog, which stores a
// declaration's source text. `kNoTxnId` means *no transaction owns this
// write*, so there is no undo chain to link the append into and no
// compensation to run: a rolled-back `CREATE ASSERTION`'s spilled body
// stays in the var-heap, referenced by nothing, forever.
//
// It was two callers until 2026-08-31 - `sys.pattern_defs` held `CREATE
// PATTERN`'s body text the same way - and the operator's withdrawal of
// declared patterns removed the second. The sweep is unchanged by that: the
// hole is the `kNoTxnId` logging path, not the relation, so one caller
// leaves it exactly as open as two did.
//
// **The answer is a sweep rather than an undo record**, and the reason is
// the envelope rather than convenience. Giving these writes a real
// transaction id would put catalog rows into the undo chain, which is
// `ddl-transactional.md` §5a's open question and not this row's to answer -
// the DDL series deliberately compensates catalog rows in place instead.
// So the leak is collected where a leak of this shape is always collected:
// once, at mount, by comparing what the pages hold against what the rows
// point at.
//
// ---- What makes the comparison sound -------------------------------------
//
// The sweep runs **at mount, before the listener binds** - the window RV1
// establishes and recovery already owns. Nothing is executing, so "the set
// of live references" is a closed question rather than a race: every row of
// the relation is on disk, and no statement can add a spill while the walk
// is between two pages.
//
// It is deliberately **not** a general var-heap collector. It sweeps the
// relations whose spills are made with `kNoTxnId`, named explicitly in
// `catalog_spills.hpp`, because every other relation's spills are already
// released by the mechanism above and sweeping them would be a second
// authority over the same bytes - the thing `varheap_release.hpp` exists to
// prevent for the release step itself.
//
// Concurrency: mount-time, single-threaded, core 0. It writes catalog-owned
// var-heap pages, which is core 0's by M5.

namespace kds::exec {

// What one sweep found, for the mount's report and for a test to assert on.
struct VarHeapSweepReport {
    // Slots the pages held that no row pointed at, and that are now
    // tombstoned. This is the leak, measured.
    std::uint64_t released = 0;

    // Slots that were live and stayed live - reported so a sweep that
    // collected everything (a referenced-set that came back empty because
    // the walk failed, say) is distinguishable from one that collected a
    // leak. A sweep with `released > 0` and `retained == 0` on a relation
    // that has rows is a bug in this file, not a large leak.
    std::uint64_t retained = 0;

    // Var-heap pages walked, so an empty report can say whether there was
    // anything to sweep.
    std::uint64_t pages = 0;
};

// Sweeps the `kNoTxnId` spill relations named above. `wal` may be null (an
// unlogged store), in which case the releases are unlogged like every other
// write on such a store.
//
// Returns the totals across every relation on that list. One with no
// var-heap chain - nothing has spilled into it yet - contributes nothing
// and is not an error.
StatusOr<VarHeapSweepReport> SweepUnownedSpills(catalog::Catalog& catalog,
                                                storage::PageStore& store,
                                                wal::WalManager* wal);

}  // namespace kds::exec
