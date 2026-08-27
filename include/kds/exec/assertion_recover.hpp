#pragma once

#include <cstdint>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/exec/assertion_replay.hpp"
#include "kds/exec/bound_cabin.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/log_device.hpp"

// Assertion recovery (AS6a, `docs/workplan-wal-recovery.md` RC07 part 3): the
// pass that turns a checkpoint's group-header snapshot plus the records after it
// back into an enforcing Bound Cabin.
//
// ---- Where it starts, and why that is the whole design -------------------
//
// **From the last checkpoint**, never from the cabin's birth. Birth-relative
// replay would make RTO a function of the assertion's lifetime, but the
// disqualifier is sharper: it would make correctness depend on the WAL never
// recycling the segment holding that `ASSERT_BUILD`, and `wal.md` §13 lists
// retention as ordinary operational configuration. A retention setting that
// silently becomes a correctness setting is the wrong coupling to ship
// (`assertion.md` §7).
//
// The anchor already names the last checkpoint, so this scans from
// `checkpoint_lsn` - a **narrower** range than redo's, and one where the first
// `ASSERT_SNAPSHOT` per assertion is by construction that assertion's base.
// That is what makes this a single forward pass with nothing buffered: a second
// scan or a record buffer would both be ways of finding out something the anchor
// already said.
//
// ---- The order, per cabin ------------------------------------------------
//
//   1. **Redo has already run**, so every entry page is on the store. This pass
//      never writes a page.
//   2. `ASSERT_SNAPSHOT` restores the group headers - `{group_id, key, count,
//      sum}`, the aggregates as of the checkpoint, with empty entry lists.
//   3. The cabin's **own pages** are walked and each entry attached to the
//      group its `group_id` names. This is the linkage AS6a chose not to
//      persist, and its cost is the assertion's entry count rather than the
//      relation's row count - which is what "no rebuild scan" means in §7.
//   4. `ASSERT_*` records after the snapshot are folded through
//      `ReplayAssertionRecord`, unchanged - it was written correct for whatever
//      range a caller feeds it, and this is the range.
//
// ---- What it refuses ----------------------------------------------------
//
// An assertion whose records appear with **no snapshot** before them is not
// recovered and is reported, not guessed at: without a base, folding from
// mid-stream would produce aggregates that are too small, and an admission
// check built on those admits writes that violate the assertion. Reported per
// assertion so a caller can leave that one unenforcing (`enforcing=0`, which is
// what `SHOW ASSERTIONS` already says for a directory recovery has not
// rebuilt) rather than fail the whole mount for one.

namespace kds::exec {

// What one assertion's recovery did, and whether it may enforce.
struct AssertionRecoveryResult {
    std::uint64_t assertion_id = 0;
    bool recovered = false;  // a snapshot was found and folded onto

    std::uint32_t groups_restored = 0;   // from the snapshot
    std::uint64_t entries_attached = 0;  // from the cabin's own pages
    std::uint64_t records_folded = 0;    // ASSERT_* after the snapshot

    // Linkage pairs the page walk and the fold both attached, reconciled at the
    // end of the rebuild (`BoundCabin::DedupeEntryLinkage`). Not a fault: both
    // steps are correct in isolation, and this is the number that says how much
    // they overlapped.
    std::uint64_t duplicate_links_dropped = 0;
};

struct AssertionRecoveryReport {
    std::vector<AssertionRecoveryResult> assertions;

    // Records that named an assertion no snapshot had introduced. Counted
    // rather than folded - see the header. A nonzero value with an otherwise
    // successful mount is the honest "this assertion cannot enforce yet".
    std::uint64_t records_without_a_base = 0;
};

// The cabins this pass fills, and where their pages start.
//
// `root_page_id` is the head of the cabin's page chain, which the caller reads
// from `sys.assertions` - this pass takes it rather than the catalog, so it can
// be driven by a scripted log in a test.
struct RecoverableAssertion {
    std::uint64_t assertion_id = 0;
    PageId root_page_id = kInvalidPageId;
    BoundCabin* cabin = nullptr;  // must outlive the call
};

// Runs the pass over `device`'s stream from `from_lsn` (the anchor's
// `checkpoint_lsn`).
//
// Fails only on what makes the result untrustworthy: a snapshot that names a
// group twice, an entry whose `group_id` no restored group carries *and* whose
// group no later record creates, a page that is not a `kCabinBound` page, or a
// fold that reports Corruption. A missing snapshot is a report, not a failure.
StatusOr<AssertionRecoveryReport> RecoverAssertions(
    wal::LogDevice& device, std::uint32_t core_id, wal::Lsn from_lsn,
    storage::PageStore& store, std::span<const RecoverableAssertion> assertions, Logger* log);

}  // namespace kds::exec
