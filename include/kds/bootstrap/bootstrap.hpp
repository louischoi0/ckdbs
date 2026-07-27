#pragma once

#include <cstdint>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/page_store.hpp"

// Whole-database bring-up: given a PageStore, load an existing database or
// create a fresh one, mirroring the legacy kernel engine's boot order
// (kds_init_meta_system() -> kds_catalog_bootstrap() "only if fresh",
// documented in kernel/kds/main.c/kds_bootstrap()) - the same care that
// comment takes matters here too: kds_catalog_bootstrap()'s Catalog::
// Bootstrap() unconditionally (re)creates the fixed catalog pages, so
// running it against an existing database would clobber real data. This
// file's fresh/existing branch (based on whether a valid SuperBlock image
// is already at kds::server::kSuperBlockPageId) exists specifically to
// make that mistake impossible to make by accident.

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
// Fails if `store` reports an unexpected error at any step, or if
// Catalog::Bootstrap() fails on the fresh path.
StatusOr<BootstrapResult> BootstrapDatabase(storage::PageStore& store,
                                             std::uint64_t now_unix_seconds);

}  // namespace kds::bootstrap
