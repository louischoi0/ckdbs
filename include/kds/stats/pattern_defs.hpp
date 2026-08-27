#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/storage/page_store.hpp"

// `sys.pattern_defs`: the name and source text of a *declared* pattern
// (docs/spec/create-pattern-user-defined-patterns-v1.md section 4.2).
//
// One row per user-declared pattern, joined to `sys.patterns` by
// `pattern_id`. An auto-registered pattern has no row here at all - it holds
// only a hash, which is also why a fingerprint version bump retires it while
// a declared pattern can be re-fingerprinted from the text stored below.
//
// ---- Why this is not in catalog/ ----------------------------------------
//
// Every other catalog relation is a fixed-offset typed row codec
// (catalog/rows.hpp): `SysTableRow::Encode()` memcpy's its fields at
// constant offsets, no Keystone word, names in a fixed 64-byte array. This
// one is an **ordinary user relation** - a Keystone word, tagged cells, a
// var-heap for values too long to inline - because a pattern's source text
// is neither fixed-width nor small, and the fixed-length rule already
// answers "where do arbitrary-length values go". Inventing a second answer
// for one catalog row would be inventing a second var-heap protocol.
//
// The price is this file's existence. Reading these rows needs
// `exec::DecodeRowInto`, and `exec/` depends on `catalog/`, so the readers
// cannot live on `Catalog` without a dependency cycle. They live here,
// beside the rest of Waystone, where a pattern definition belongs anyway.
//
// ---- Two rules every function below honours ------------------------------
//
// **Decode before descending** (docs/spec/parser-v2.md I15 rule R1). A var-heap
// fetch must never happen while a heap-page span is live, so the scan
// decodes each row inside the walk - which touches no page but the one it is
// already on - and resolves the spilled cells *after* the walk has released
// every span. That is why nothing here can stop early on a match: the name
// it would match on may still be an unresolved pointer while the walk is
// running.
//
// **Deletion is physical.** `DeletePatternDef` retires the slot rather than
// delete-marking it, because catalog scans do not filter delete-marks -
// there is no transaction manager to give them a snapshot to filter against
// - so a marked row would still be found by name and DROP PATTERN followed
// by CREATE PATTERN of the same name would fail on a row nobody can see.
//
// Concurrency: core-local, like the catalog it reads through. No internal
// synchronization (rules.md #3).

namespace kds::wal {
class WalManager;
}  // namespace kds::wal

namespace kds::stats {

// One `sys.pattern_defs` row, fully resolved - `name` and `source_text`
// have had any var-heap spill fetched, so a caller never holds a pointer.
struct PatternDef {
    // The relation's own Keystone id. Carried because it is the row's
    // identity and every relation has one, not because anything keys on it:
    // callers arrive holding a `pattern_id` or a name.
    std::uint64_t id = 0;

    std::uint64_t pattern_id = 0;

    // The declaration's **materialized arity**: how many value slots the
    // body has (spec section 3.3). Not the number of declared parameters -
    // a parameter written twice contributes two slots, and a literal in the
    // body contributes one without being a parameter at all.
    //
    // Stored rather than recomputed, because recomputing it means
    // re-fingerprinting `source_text` and the two could then disagree for a
    // row an older build wrote.
    std::uint32_t param_count = 0;

    std::string name;

    // **The entire `CREATE PATTERN` statement, verbatim** - not just the
    // body. This is the canon: the one artefact from which the declaration
    // can be rebuilt in full, parameters and options included, which is what
    // a fingerprint version bump needs at boot (spec section 7) and what
    // `SHOW` prints back. Storing only the body would make the declared
    // types and the `WITH` options unrecoverable, and they are exactly what
    // a re-registration has to preserve.
    //
    // This is also why there is no sibling relation for the parameters: the
    // canon already carries them, and a second copy is a second thing that
    // can drift from it.
    std::string source_text;
};

// Every declared pattern, in chain order.
//
// The primitive the other readers are built from, and deliberately the only
// one that touches storage: at the scale this relation lives at - the
// patterns an operator chose to declare, tens rather than millions - one
// scan per lookup is cheaper than an index nobody maintains, and it is one
// place for the spill-resolution ordering above to be right.
StatusOr<std::vector<PatternDef>> ListPatternDefs(catalog::Catalog& catalog,
                                                  storage::PageStore& store);

// The definition named `name`, case-insensitively, or nullopt.
//
// Case-insensitive because every other identifier in this engine is: a
// pattern declared as `AcctTrades` and dropped as `acct_trades` has to be
// the same pattern, or DROP would silently miss.
StatusOr<std::optional<PatternDef>> FindPatternDefByName(catalog::Catalog& catalog,
                                                         storage::PageStore& store,
                                                         std::string_view name);

// The definition for `pattern_id`, or nullopt. This is the join
// `SHOW PATTERNS` does to print a name beside a hash.
StatusOr<std::optional<PatternDef>> FindPatternDefByPatternId(catalog::Catalog& catalog,
                                                              storage::PageStore& store,
                                                              std::uint64_t pattern_id);

// Records a declaration. `source_text` is the whole `CREATE PATTERN`
// statement **verbatim as declared**, `$` sigils included - see the field's
// note above for why the whole statement and not the body.
//
// `param_count` is the body's value-slot count, which the caller has already
// computed from the fingerprint it took of the body.
//
// Fails with Unsupported when `source_text` is longer than one var-heap page
// can hold (8144 bytes). The spilled-value size cap is an open decision and
// this does not settle it: a longer declaration is refused rather than
// chained.
// `wal` (null = unlogged, the tests' path): since 2026-08-19 the row, its
// spilled body and any grown page are logged (exec/wal_row_log.hpp),
// closing RV3's remainder for this relation.
Status InsertPatternDef(catalog::Catalog& catalog, storage::PageStore& store,
                        wal::WalManager* wal, std::uint64_t pattern_id, std::string_view name,
                        std::string_view source_text, std::uint32_t param_count);

// Removes the definition for `pattern_id`. NotFound if there is none.
//
// The var-heap bytes its text occupied are **not** reclaimed: nothing
// reclaims var-heap space, because reclamation rides on purge and purge does
// not exist. That is a leak of the size of the dropped body, bounded by how
// often patterns are dropped, and it is the same leak every superseded
// varchar value already produces.
Status DeletePatternDef(catalog::Catalog& catalog, storage::PageStore& store,
                        wal::WalManager* wal, std::uint64_t pattern_id);

}  // namespace kds::stats
