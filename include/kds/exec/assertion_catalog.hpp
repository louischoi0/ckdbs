#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/assertion_check.hpp"
#include "kds/exec/bound_cabin.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/read_view.hpp"

namespace kds::wal {
class WalManager;
}

// `sys.assertions`: the declaration of every group-level constraint on this
// instance (docs/spec/assertion.md §8.2, workplan AST03).
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
// **Decode before descending** (docs/spec/parser-v2.md I15 rule R1). A var-heap
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

// The same rows with **`name` and `source_text` left empty**: the fixed
// columns only - id, target oid, cabin root.
//
// It exists for one caller and one property. `sys.assertions`' heap pages
// are all below `kFirstUserPageId`, which `well_known.hpp` calls a
// correctness requirement precisely so that a peer core can read the
// catalog; its **var-heap is not**, because a spilled value takes its page
// from the general supply. So a peer that resolves the spills may be
// refused the fetch (`may not fault page N`) and lose the whole scan, while
// this form cannot be: it reads exactly the pages the walk already holds.
//
// What that buys is the fail-closed answer at a peer's mount (PW1c-6c): a
// core that cannot read *what* an assertion says can still learn *that* one
// exists on a relation it owns, and refuse the relation's writes rather
// than admit them unchecked.
StatusOr<std::vector<AssertionDef>> ListAssertionTargets(catalog::Catalog& catalog,
                                                          storage::PageStore& store);

// The var-heap pages the stored declarations spill into - **the ids the
// rows name, with no fetch of any of them**.
//
// A peer's mount reads this before it reads the declarations, and grants
// itself fault rights over exactly these pages (`CoreRuntime::Open`,
// PW1c-6c). Exactly these, page by page, and never the extent around them:
// a range grant covers pages this core may *own*, and a page that answers
// `MayFault` from a grant never reaches `TryClaimByStamp`, so the peer
// would silently lose the write rights PW1c-7 restores to it on the fault -
// which is a restarted owner unable to write its own relation. That was
// measured, not reasoned: an extent-wide grant here failed
// `APeersOwnPagesSurviveARestartByTheirStamp`.
StatusOr<std::vector<PageId>> AssertionSpillPages(catalog::Catalog& catalog,
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
// `id` is allocated by the caller (`Catalog::AllocateRowId`), **before the
// build rather than here**, because the builder's `ASSERT_BUILD` records
// carry it - a record for an id that does not exist yet could never be
// written first. A build that fails burns the id, which K3 makes free.
// `cabin_root` is the built chain's root, or `kInvalidPageId` from a caller
// that built nothing (tests of the row alone).
//
// Fails with `AlreadyExists` when the name is taken - the duplicate-name
// check of §3.1, made here rather than at the parser because it is the
// catalog's question and this is the door every caller comes through.
//
// Fails with `Unsupported` when `source_text` is longer than one var-heap
// page can hold. The spilled-value size cap is an open decision and this does
// not settle it: a longer declaration is refused rather than chained.
Status InsertAssertion(catalog::Catalog& catalog, storage::PageStore& store,
                       wal::WalManager* wal, std::uint64_t id,
                       catalog::Oid target_oid, std::string_view name,
                       std::string_view source_text, PageId cabin_root);

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
                       wal::WalManager* wal, std::string_view name);

// ---- The DDL entry points ------------------------------------------------

struct AssertionDdlResult {
    std::uint64_t assertion_id = 0;
    catalog::Oid target_oid = 0;
    PageId cabin_root = kInvalidPageId;
    std::size_t rows_incorporated = 0;
    std::size_t group_count = 0;

    // Everything the write hook needs, resolved here where the statement,
    // the schema and the build are all in hand - the dispatcher moves it
    // into its `AssertionEnforcer`. An optional rather than a value because
    // a moved-from one held beside a published root would look exactly like
    // an empty relation's.
    std::optional<LiveAssertion> live;
};

// The live half of a **surviving** assertion, rebuilt from its stored
// declaration (RC07's mount wiring, `docs/workplan-wal-recovery.md`).
//
// §8.2 keeps `source_text` as the canon precisely so this is possible: the
// group columns are not stored as positions, they are "recovered by re-parsing
// it", which is what this does. Everything else - the aggregate, the enforced
// ceiling, the SUM column - comes from the same parse, resolved against the
// relation's current schema exactly as `CreateAssertion` resolves it.
//
// **The directory is left empty.** This builds the shell a Bound Cabin needs -
// the aggregate, the bound, the chain writer over the stored root - and nothing
// else; filling the group headers is `exec::RecoverAssertions`' job, from the
// checkpoint snapshot and the records after it (AS6a). A caller that adopted the
// result of this alone would be enforcing against zero, which admits every
// write.
//
// Fails the way `CreateAssertion` fails for the same declaration: a source text
// that no longer parses, a group column the relation no longer has (an `ALTER`
// that renamed one), a SUM column that is not int64. Each is a reason this
// assertion cannot enforce, and a caller reports it per assertion rather than
// failing the mount - one unenforceable constraint is not a database that must
// not open.
StatusOr<LiveAssertion> ReviveAssertion(catalog::Catalog& catalog, storage::PageStore& store,
                                       const AssertionDef& def);

// ---- The three halves `CREATE ASSERTION` is made of (PW1c-6c) ------------
//
// A relation another core owns has its Bound Cabin built **there**, because
// the cabin is written on every write to that relation and only its owner
// may write the owner's pages (`docs/inflight/in-progress/workplan-peer-writer.md` §7d).
// So the statement splits where the index build splits (`exec::PrepareIndexDef`
// / `exec::BuildIndexTree`, PW1c-6b-1): the catalog half stays on core 0, the
// page half moves to the owner, and the local `CreateAssertion` below is the
// three back to back - one implementation, two callers, and a peer-owned
// relation's assertion is checked by exactly the rules a core-0-owned one is.

// What core 0 resolves before anything is built: the relation, and the id
// the build's `ASSERT_BUILD` records will carry.
struct AssertionPrepared {
    catalog::Oid target_oid = 0;
    std::uint64_t assertion_id = 0;
};

// §3.1's create-time checks and the id, and **not one page written**. The
// four questions that need a schema (does the relation exist, does every
// `GROUP BY` column exist, does the `SUM` column exist and is it int64),
// then the two the publish would fail on anyway - a taken name, an
// over-long declaration - and then `AllocateRowId`, which is a catalog
// write and so core 0's by construction.
//
// Refused before the id is issued, all of it: a refusal here burns nothing
// and leaks nothing, which is what lets core 0 ask a peer to build only
// for a declaration that would have been accepted locally.
StatusOr<AssertionPrepared> PrepareAssertionDef(catalog::Catalog& catalog,
                                                storage::PageStore& store,
                                                const parser::AssertionStmt& stmt);

// What the page half hands back: the root the publish names, the two
// numbers the reply reports, and the live directory the **building core's**
// registry will own.
struct AssertionCabinBuild {
    PageId cabin_root = kInvalidPageId;
    std::size_t rows_incorporated = 0;
    std::size_t group_count = 0;
    LiveAssertion live;
};

// The page half: the AST06 scan into a Bound Cabin of this core's own
// pages, the live directory resolved from this core's schema, and AS6a's
// base logged into **this core's stream** - which is what makes the cabin
// recoverable by the core that will be appending to it.
//
// It re-resolves the relation and the columns rather than taking them from
// `PrepareAssertionDef`: on the cross-core path the two run on different
// cores against different catalog views, and a build that trusted a
// resolution it did not make would be building against a schema it never
// read.
//
// **The base is logged before the publish**, where the single-core order
// logs it after (§8.1a's commit point). The owner cannot wait for core 0's
// row - it must reply first - so the order is forced there and made the
// same here rather than left to differ by path. What it costs is an
// `ASSERT_SNAPSHOT` for an assertion whose publish then fails: a base for a
// cabin no catalog row names, which no mount ever folds, because a mount
// folds only what `ListAssertions` returns.
StatusOr<AssertionCabinBuild> BuildAssertionCabin(catalog::Catalog& catalog,
                                                  storage::PageStore& store,
                                                  const parser::AssertionStmt& stmt,
                                                  std::uint64_t assertion_id,
                                                  const txn::ReadView& check_view,
                                                  wal::WalManager* wal);

// `CREATE ASSERTION`: §3.1's remaining create-time checks - the ones only
// the catalog can answer - then the AST06 build, then the publish.
//
// The parser already refused everything decidable from the text alone (the
// operator set, the bound, a degenerate predicate, every reserved form), so
// the checks here are exactly the four questions that need a schema: does
// the relation exist, does every `GROUP BY` column exist, does the `SUM`
// column exist and is it `int64`, and is the name free. Each error carries
// the byte the parser recorded for that name, which is the only reason those
// offsets are on the statement at all.
//
// **The order is validate → build → publish** (§8.1): the full scan runs
// before the `sys.assertions` row exists, so an assertion is published
// complete or not at all, and the publish - the row carrying `cabin_root`,
// plus the catalog version bump - is the single commit point §8.1a
// requires. A failed build leaves an unreachable page chain, a burned row
// id, an `ASSERT_DROP` discard marker in the log when one is attached, and
// **no catalog row** - `index_ddl.cpp`'s precedent.
//
// `check_view` is the visibility the scan reads under (latest settled
// state; `ReadView::Everything()` from a caller with no transaction
// manager, which is the pre-MVCC engine exactly). `wal` may be null: the
// build is then unlogged, like every other DDL today.
//
// **Publishing does not enforce.** The write-path check is AST07's; a
// caller that reports `enforcing=1` because a root exists is lying.
StatusOr<AssertionDdlResult> CreateAssertion(catalog::Catalog& catalog,
                                             storage::PageStore& store,
                                             const parser::AssertionStmt& stmt,
                                             const txn::ReadView& check_view,
                                             wal::WalManager* wal);

// `DROP ASSERTION`: the catalog row retired, `ASSERT_DROP` logged when
// `wal` is attached (before the row goes - WAL before data), and the
// catalog version bumped. Returns the dropped assertion's id so the caller
// can evict its live directory; the entry pages are not reclaimed, because
// page reclamation does not exist.
StatusOr<std::uint64_t> DropAssertion(catalog::Catalog& catalog, storage::PageStore& store,
                                      const parser::AssertionStmt& stmt, wal::WalManager* wal);

}  // namespace kds::exec
