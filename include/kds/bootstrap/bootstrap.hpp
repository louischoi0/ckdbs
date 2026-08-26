#pragma once

#include <cstdint>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/storage/tagged_cell.hpp"

// Whole-database bring-up: given a PageStore, load an existing database or
// create a fresh one - and bootstrap the catalog **only if fresh**.
//
// That condition is the whole point of this file. Catalog::Bootstrap()
// unconditionally (re)creates the fixed catalog pages, so running it
// against an existing database would clobber real data. The fresh/existing
// branch here - decided by whether a valid SuperBlock image already sits
// at kds::server::kSuperBlockPageId - exists to make that mistake
// impossible to make by accident.

namespace kds::bootstrap {

// Everything a caller needs after bringing a database up: the loaded or
// freshly-created SuperBlock, and a Catalog ready for use. `catalog` holds
// a reference to the `store` passed to BootstrapDatabase() - the caller
// must keep that PageStore alive at least as long as this result.
struct BootstrapResult {
    server::SuperBlock superblock;
    catalog::Catalog catalog;
};

// Brings a database up on `store`:
//   - If a valid SuperBlock image (correct magic) is already at
//     kds::server::kSuperBlockPageId, loads it, stamps last_mount_time to
//     `now_unix_seconds`, and persists that stamp immediately. The
//     catalog's fixed pages are assumed to already exist and are left
//     untouched - Catalog::Bootstrap() is NOT called on this path.
//   - Otherwise (no page there yet, or one that doesn't decode as a
///    valid SuperBlock), treats this as a fresh database: creates and
//     persists a brand-new SuperBlock, then runs Catalog::Bootstrap() to
//     allocate the fixed catalog pages and populate their bootstrap rows.
//
// `inline_cell_width` is the running configuration's kds.inline_cell_width
// (storage/tagged_cell.hpp). On the fresh path it is **pinned** into the
// new superblock; on the existing path it is **validated** against the
// pinned one and a disagreement refuses the mount with InvalidArgument,
// naming both values (docs/rules/rule-fixed-length-tuple.md section 4). The check
// is not pedantry: on-disk tuple layout is computed from the width, so a
// database opened under a different one decodes every row at the wrong
// offsets rather than failing.
//
// `cores` is the running configuration's `cores`
// (docs/inflight/in-progress/workplan-crosscore.md M6), and is pinned and validated by exactly
// the same rule and at exactly the same two moments. Its check is not about
// tuple layout but about the WAL: streams are per core, so a database
// created for N cores holds N streams, and mounting it at M leaves the
// difference with nothing to replay them. Recovery under a changed core
// count is [OPEN] (wal.md section 3) - the refusal is what keeps this
// function from settling it silently.
//
// Fails if `store` reports an unexpected error at any step, if
// `inline_cell_width` or `cores` is outside its legal range or disagrees
// with the pinned one, or if Catalog::Bootstrap() fails on the fresh path.
//
// `log` (optional, component tag "bootstrap") receives the one fact this
// function decides and nothing else can recover afterwards: whether this
// was a fresh database or an existing one. The returned Catalog carries
// the same logger, so catalog writes are reported from here on. It must
// outlive both the call and the returned result.
StatusOr<BootstrapResult> BootstrapDatabase(
    storage::PageStore& store, std::uint64_t now_unix_seconds,
    std::uint32_t inline_cell_width = storage::kDefaultInlineCellWidth,
    std::uint32_t cores = 1, Logger* log = nullptr);

}  // namespace kds::bootstrap
