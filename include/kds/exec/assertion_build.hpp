#pragma once

#include <cstdint>
#include <span>

#include "kds/base/status.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/exec/bound_cabin.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/cabin_bound_page.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/read_view.hpp"
#include "kds/wal/record.hpp"

// The CREATE-time Bound Cabin builder (docs/spec/assertion.md §8.1, workplan
// AST06): one full scan of the target relation, accumulated into entry pages
// and a group directory, refused whole if the data already violates the
// declared bound.
//
// ---- Built before it is published, index backfill's shape -----------------
//
// The build runs before the `sys.assertions` row exists, so an assertion is
// published complete or not at all. A failure leaves an unreachable page
// chain and a burned row id, and no catalog row - `index_ddl.cpp`'s
// precedent exactly, and K3 makes the id free. Nothing reclaims the pages,
// because page reclamation does not exist in this engine; the leak is
// bounded by how often a CREATE fails.
//
// ---- The cutover, and why the membership protocol is trivially met --------
//
// §8.1a's decided protocol: a row counts as incorporated iff its pk is in
// the Bound Cabin, never inferred from scan position. This build runs
// **synchronously inside the CREATE statement on one cooperative core** -
// the same fact `index_ddl.cpp` states as "nothing can observe the
// half-built tree in between" - so no write can interleave and the
// membership question never has a contested answer. The spec's "background
// scheduling group, cooperative yielding" needs the suspendable statement
// path that `docs/spec/crosscore.md` P4 still lacks (nothing in the engine
// yields mid-statement); when the build learns to yield, the membership
// protocol is the correctness story it resumes under, which is why it is
// decided now and recorded here.
//
// ---- Visibility: latest settled state, refuse the unsettled ---------------
//
// The aggregate constrains *live rows*, so the build reads latest committed
// state through `txn::CheckVisibility` - the FK checks' predicate, not a
// snapshot. A row whose writer (or deleter) is still in flight is refused
// with `kTxnConflict`, retryable, F3's fail-fast shape: counting it and
// then losing the abort would overstate the group forever (nothing prunes),
// and skipping it and seeing the commit would understate it, which for an
// admission structure is the one wrong answer. The operator retries when
// the relation is settled.
//
// Concurrency: core-local, one statement, no latches (§6.1).

namespace kds::wal {
class WalManager;
}

namespace kds::exec {

// The entry-page chain writer: allocation, formatting, linking and the WAL
// record for each append, kept together so the emission order cannot drift
// from the mutation order. **Two callers, one implementation**: the builder
// appends with `kAssertBuild`/`kNoTxnId`, and AST07's write hook appends
// reservations with `kAssertReserve` and the writing transaction - the same
// bytes through the same code, which is what keeps a built entry and a
// reserved one from disagreeing about what an entry is.
//
// Plain state (root, tail, page count) with the store and WAL taken per
// call, so a registry can hold one for an assertion's lifetime without
// holding references.
class BoundCabinChainWriter {
public:
    BoundCabinChainWriter() = default;
    explicit BoundCabinChainWriter(std::uint64_t assertion_id) noexcept
        : assertion_id_(assertion_id) {}

    PageId root() const noexcept { return root_; }
    std::size_t pages() const noexcept { return pages_; }

    // Allocates and formats the first page. Called unconditionally by the
    // builder, because the publish step names a root even for an empty
    // relation.
    Status EnsureRoot(storage::PageStore& store, wal::WalManager* wal);

    // Takes over an **existing** chain, for an assertion that survived a
    // restart (RC07's mount wiring): the root comes from `sys.assertions` and
    // the tail is found by following the links, because the writer appends
    // there and a fresh writer would otherwise start growing a second chain
    // beside the one the entries are already on.
    //
    // Fails with Corruption on a page that is not a `kCabinBound` page or a
    // link cycle - both mean the root is not the chain it claims to be, and
    // appending into it would scatter entries no directory could relink.
    Status AdoptChain(storage::PageStore& store, PageId root);

    // Appends one entry, growing the chain when the tail is full, and logs
    // it as `type` owned by `txn_id` against the page it landed in.
    StatusOr<std::pair<PageId, std::uint16_t>> Append(storage::PageStore& store,
                                                      wal::WalManager* wal,
                                                      const storage::cabin::BoundCabinEntry& entry,
                                                      const std::string& key,
                                                      wal::RecordType type,
                                                      std::uint64_t txn_id);

private:
    Status Grow(storage::PageStore& store, wal::WalManager* wal);

    std::uint64_t assertion_id_ = 0;
    PageId root_ = kInvalidPageId;
    PageId tail_ = kInvalidPageId;
    std::size_t pages_ = 0;
};

// What a completed build hands back: the root the publish step names, the
// directory the caller's registry will own, the chain writer AST07's write
// hook keeps appending through, and the two numbers the reply reports.
struct BoundCabinBuild {
    BoundCabinBuild(BoundAggregate aggregate, std::int64_t enforced_max,
                    std::uint64_t assertion_id)
        : chain(assertion_id), cabin(aggregate, enforced_max) {}

    PageId cabin_root = kInvalidPageId;
    std::size_t rows_incorporated = 0;
    std::size_t pages_allocated = 0;
    BoundCabinChainWriter chain;
    BoundCabin cabin;
};

// Scans `access`'s relation and materializes the Bound Cabin for `stmt`.
//
// `group_cols` and `sum_col` are the schema positions the caller already
// resolved (`CreateAssertion` validates them; re-resolving here would be a
// third copy of the same loop). `sum_col` is read only for a SUM assertion.
//
// Fails with `AssertionViolation` naming the first group the scan takes
// past the bound - deterministic in scan order, because the check runs as
// each row lands. Fails with `TxnConflict` on an in-flight writer, see
// above. When `wal` is non-null, every allocated page is logged `PAGE_INIT`
// (kCabinBound), every entry `ASSERT_BUILD`, and a chain link edit takes a
// full page image of the old tail - the heap chain's rule, because no
// record type describes a link edit.
StatusOr<BoundCabinBuild> BuildBoundCabin(storage::PageStore& store,
                                          const catalog::TableAccess& access,
                                          const parser::AssertionStmt& stmt,
                                          std::uint64_t assertion_id,
                                          std::span<const std::uint16_t> group_cols,
                                          std::uint16_t sum_col,
                                          const txn::ReadView& check_view,
                                          wal::WalManager* wal);

}  // namespace kds::exec
