#pragma once

// The four words the instance keeps for every core's catalog
// (`docs/spec/catalog.md` CT6): one struct since CF-S1c, when the opted
// count made the fourth and tipped CT6's threshold (AT-0 item 11).
//
// Ownership and lifetime: `Expeditor` owns the one `InstanceWords`, every
// core's catalog borrows a pointer through `Catalog::SetInstanceWords` -
// core 0's from `Expeditor::Open`, a peer's through
// `CoreRuntime::Config::instance_words` - and a null pointer leaves a bare
// catalog on its local copies (`Catalog::SetInstanceWords` lists them),
// which is what a fixture and the sim hold.
//
// Atomic protocol: the struct is only a carrier. Each word keeps its own
// rule, its own writers and its own memory orders, stated at the field and
// owned by the `catalog.md` row it names; no word orders another, and no
// operation reads two of them as one fact. Every member is trivially
// destructible and `~Catalog` reads none, which is what makes a catalog
// that outlives the struct harmless (`Expeditor`'s declaration order).

#include <atomic>
#include <cstdint>
#include <type_traits>

#include "kds/catalog/oid.hpp"

namespace kds::catalog {

struct InstanceWords {
    // **The schema version word** (AT-S2; CT1, CT2): the memo's generation
    // counter. Moved by `fetch_add(acq_rel)` at every catalog write that can
    // stale a peer's memo (`Catalog::BumpWord`) and by a catalog-writing
    // decide (`TransactionManager::MoveSchemaWordIfCatalogWriter`, AT-S5e);
    // read with `acquire` at a task boundary (`Catalog::Revalidate`) and at
    // a writer's first relation `IX` (`Catalog::MemoIsCurrent`).
    // Memory-resident and never persisted: every cache is empty at mount.
    std::atomic<std::uint64_t> schema_word{0};

    // **The object-oid sequence** (AT-S5b): the next oid `CREATE TABLE` and
    // `CREATE NAMESPACE` issue. Zero is the unseeded value (no oid is zero);
    // whichever catalog asks first seeds it by `compare_exchange_strong
    // (acq_rel, acquire)` from `HighestIssuedUserOid() + 1`, and every issue
    // after that is a `fetch_add(acq_rel)` (`Catalog::GenerateUserOid`).
    std::atomic<Oid> oid_sequence{0};

    // **The delete-mark count** (AT-S5b; `ddl-transactional.md` §5d): the
    // purge's gate. Grown by `fetch_add(1, acq_rel)` at each mark
    // (`Catalog::NoteMark`), resettled by a `fetch_add` of a *delta* at the
    // sweep (`Catalog::ResettleMarks`), read with `acquire`
    // (`Catalog::MarkCount`). `acq_rel` because a mark's row is a page write
    // another core reads and the count is what tells that core to look.
    std::atomic<std::uint64_t> mark_count{0};

    // **The opted count** (CF-S1c): how many current-version `sys.patterns`
    // rows carry `kPatternWaystone`; when it moves and when it is set is
    // `Catalog::opted_patterns`' contract. A 0->1 move is a
    // `fetch_add(acq_rel)` and a 1->0 one a saturating CAS loop
    // (`acq_rel`/`acquire`, `Catalog::MoveOptedCount`); the boot's set is a
    // `release` store (`Catalog::StoreOptedCount`); a read is `acquire`.
    // It publishes nothing beside itself - what a peer's opted map holds is
    // the schema word's to order.
    std::atomic<std::uint64_t> opted_count{0};
};

static_assert(std::is_trivially_destructible_v<InstanceWords>,
              "a catalog may outlive the words it borrows; see the header comment");

}  // namespace kds::catalog
