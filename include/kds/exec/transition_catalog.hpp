#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/storage/page_store.hpp"

namespace kds::wal {
class WalManager;
}

// `sys.transitions`: the declaration of every Transition on this instance
// (`instructions/v3.2.0/workorder-bv-transition-constraints.md`, BV-Q1 (a)).
//
// One row per Transition: the `CREATE TRANSITION` statement verbatim and
// the fixed columns a reader needs without parsing it - the relation and
// the governed column's position. The state sets and the edges are
// recovered by re-parsing `source_text`. The storage is the shared
// declaration relation (`declaration_relation.hpp`), which states the
// concurrency and deletion rules.

namespace kds::exec {

struct TransitionDef {
    // The relation's own Keystone id: the Transition's id.
    std::uint64_t id = 0;
    catalog::Oid target_oid = 0;
    // The governed column's schema position. Stored by number, so
    // `ALTER TABLE RENAME COLUMN` changes nothing here.
    std::uint16_t column_no = 0;
    std::string name;
    // The whole `CREATE TRANSITION` statement, verbatim: the canon.
    std::string source_text;
};

// Every declared Transition, in chain order.
StatusOr<std::vector<TransitionDef>> ListTransitions(catalog::Catalog& catalog,
                                                     storage::PageStore& store);

// The Transition named `name`, case-insensitively, or nullopt.
StatusOr<std::optional<TransitionDef>> FindTransitionByName(catalog::Catalog& catalog,
                                                            storage::PageStore& store,
                                                            std::string_view name);

// Every Transition on `target_oid`, in chain order.
StatusOr<std::vector<TransitionDef>> TransitionsOnRelation(catalog::Catalog& catalog,
                                                           storage::PageStore& store,
                                                           catalog::Oid target_oid);

// Records a declaration (`id` from `Catalog::AllocateRowId`).
// `AlreadyExists` when the name is taken.
Status InsertTransition(catalog::Catalog& catalog, storage::PageStore& store,
                        wal::WalManager* wal, const TransitionDef& def);

// Retires the Transition named `name` and returns it; `NotFound` if there
// is none.
StatusOr<TransitionDef> DeleteTransition(catalog::Catalog& catalog, storage::PageStore& store,
                                         wal::WalManager* wal, std::string_view name);

}  // namespace kds::exec
