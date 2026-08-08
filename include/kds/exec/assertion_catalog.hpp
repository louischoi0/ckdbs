#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/page_store.hpp"

// `sys.assertions`: the declaration of every group-level constraint on this
// instance (docs/feat-assertion.md §8.2, workplan AST03).
//
// ---- What is stored, and what deliberately is not -----------------------
//
// One row per assertion, holding the **declaration verbatim** and four fixed
// fields. The parsed form - the group columns, the aggregate, the operator,
// the bound - is *not* stored: it is recovered by re-parsing `source_text`,
// which is the `sys.pattern_defs` model AS10 names. Two things follow.
//
// The `GROUP BY` list needs no cap, because a longer list costs text rather
// than a wider catalog row; §3 declares none and this layer invents none.
// And there is exactly one canon, so nothing can drift from it - a decoded
// sibling table would be a second copy of the same facts, written by one
// build and read by another.
//
// ---- Why this is not in catalog/ ----------------------------------------
//
// `sys.assertions` is the second catalog relation stored in ordinary user
// tuple format - a Keystone word, tagged cells, a var-heap for values too
// long to inline - because a declaration's text is neither fixed-width nor
// small. Reading such a row needs `exec::DecodeRowInto`, and `exec/` depends
// on `catalog/`, so these readers cannot live on `Catalog` without a
// dependency cycle. `stats/pattern_defs.hpp` is the same file for the same
// reason; this one lives in `exec/` because an assertion is a constraint,
// which is where `fk_check.hpp` and `index_ddl.hpp` already are.
//
// ---- Two rules every function below honours ------------------------------
//
// **Decode before descending** (docs/parser-v2.md I15 rule R1). A var-heap
// fetch must never happen while a heap-page span is live, so a scan decodes
// each row inside the walk and resolves the spilled cells *after* the walk
// has released every span. That is why the readers cannot stop early on a
// match: the name they would match on may still be an unresolved pointer
// while the walk is running.
//
// **Deletion is physical.** `DeleteAssertion` retires the slot rather than
// delete-marking it, because catalog reads have no snapshot to filter a mark
// against - so a marked row would still be found by name, and DROP followed
// by CREATE of the same name would collide with a row nobody can see.
//
// Concurrency: core-local, like the catalog it reads through. No internal
// synchronization (rules.md #3).

namespace kds::exec {

// One `sys.assertions` row, fully resolved - `name` and `source_text` have
// had any var-heap spill fetched, so a caller never holds a pointer.
struct AssertionDef {
    // The relation's own Keystone id, which **is** §8.2's `assertion_id`:
    // engine-issued, unique and never reused (K1), so it needs no second
    // sequence beside it.
    std::uint64_t id = 0;

    // The relation the assertion constrains. RESTRICT on its drop - see
    // `AssertionsOnRelation` for the state of that hook.
    catalog::Oid target_oid = 0;

    // The Bound Cabin anchor (§8.2). **Written `kInvalidPageId` by AST03**:
    // the structure is AST04's and the builder that fills this in is AST06's.
    // The field exists now because it is in §8.2's table and because adding a
    // catalog column later costs the same superblock version bump this
    // relation already cost.
    PageId cabin_root = kInvalidPageId;

    // Reserved (§8.2): the future deferrable / not-valid bits. Written 0 and
    // read by nothing, because both forms are refused at the grammar (AS3,
    // AS7) and a stored 0 means exactly "neither".
    std::uint32_t flags = 0;

    std::string name;

    // **The entire `CREATE ASSERTION` statement, verbatim.** The canon: the
    // one artefact the declaration can be rebuilt from in full, which is what
    // makes the parsed form recoverable and the GROUP BY list uncapped.
    std::string source_text;
};

// Every declared assertion, in chain order.
//
// The primitive the other readers are built from, and deliberately the only
// one that touches storage: at this relation's scale - the constraints an
// operator chose to declare - one scan per lookup is cheaper than an index
// nobody maintains, and it is one place for the spill-resolution ordering
// above to be right.
StatusOr<std::vector<AssertionDef>> ListAssertions(catalog::Catalog& catalog,
                                                   storage::PageStore& store);

// The assertion named `name`, case-insensitively, or nullopt.
//
// Case-insensitive because every other identifier in this engine is: an
// assertion declared as `AcctLimit` and dropped as `acct_limit` has to be the
// same assertion, or DROP would silently miss.
StatusOr<std::optional<AssertionDef>> FindAssertionByName(catalog::Catalog& catalog,
                                                          storage::PageStore& store,
                                                          std::string_view name);

// Every assertion constraining `target_oid`, in chain order.
//
// **This is the RESTRICT predicate** (AS10, §8.3): dropping a relation with a
// non-empty answer here must fail rather than leave a constraint pointing at
// nothing. The hook itself is not wired, because **there is no DROP TABLE in
// this engine** - the catalog is append-only apart from DROP PATTERN, DROP
// CABIN, DROP INDEX and now DROP ASSERTION. This function is the whole of
// what the hook needs, and is tested as such; wiring it is a one-line call at
// the site that does not exist yet.
StatusOr<std::vector<AssertionDef>> AssertionsOnRelation(catalog::Catalog& catalog,
                                                         storage::PageStore& store,
                                                         catalog::Oid target_oid);

// Records a declaration. `source_text` is the whole `CREATE ASSERTION`
// statement verbatim.
//
// Fails with `AlreadyExists` when the name is taken - the duplicate-name
// check of §3.1, made here rather than at the parser because it is the
// catalog's question and this is the door every caller comes through.
//
// Fails with `Unsupported` when `source_text` is longer than one var-heap
// page can hold. The spilled-value size cap is an open decision and this does
// not settle it: a longer declaration is refused rather than chained.
//
// Returns the new assertion's id.
StatusOr<std::uint64_t> InsertAssertion(catalog::Catalog& catalog, storage::PageStore& store,
                                        catalog::Oid target_oid, std::string_view name,
                                        std::string_view source_text);

// Removes the assertion named `name`, case-insensitively. `NotFound` if there
// is none.
//
// The var-heap bytes its text occupied are **not** reclaimed: nothing
// reclaims var-heap space, because reclamation rides on purge and purge does
// not exist. That is the same leak every superseded varchar value already
// produces, bounded here by how often assertions are dropped.
//
// It does **not** tear down a Bound Cabin, because there is none to tear down
// until AST04 builds one. `ASSERT_DROP` (§7, AST05) is where that lands.
Status DeleteAssertion(catalog::Catalog& catalog, storage::PageStore& store,
                       std::string_view name);

// ---- The DDL entry points ------------------------------------------------

struct AssertionDdlResult {
    std::uint64_t assertion_id = 0;
    catalog::Oid target_oid = 0;
};

// `CREATE ASSERTION`'s catalog half: §3.1's remaining create-time checks -
// the ones only the catalog can answer - and then the row.
//
// The parser already refused everything decidable from the text alone (the
// operator set, the bound, a degenerate predicate, every reserved form), so
// what is left here is exactly the four questions that need a schema:
// does the relation exist, does every `GROUP BY` column exist, does the `SUM`
// column exist and is it `int64`, and is the name free. Each error carries
// the byte the parser recorded for that name, which is the only reason those
// offsets are on the statement at all.
//
// **It builds nothing.** No Bound Cabin, no scan, no enforcement: those are
// AST04, AST06 and AST07. What this ships is a declaration that is recorded,
// re-readable and droppable - and a caller that reports it as enforcing
// anything is lying.
StatusOr<AssertionDdlResult> CreateAssertion(catalog::Catalog& catalog,
                                             storage::PageStore& store,
                                             const parser::AssertionStmt& stmt);

// `DROP ASSERTION`'s catalog half. Returns the dropped assertion's id.
StatusOr<std::uint64_t> DropAssertion(catalog::Catalog& catalog, storage::PageStore& store,
                                      const parser::AssertionStmt& stmt);

}  // namespace kds::exec
