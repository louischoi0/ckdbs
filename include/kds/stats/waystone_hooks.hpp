#pragma once

#include <cstdint>

#include "kds/base/status.hpp"
#include "kds/stats/waystone.hpp"
#include "kds/storage/page_store.hpp"

// The coverage hooks (waystone-concpets.md sections 3 and 9): the two
// calls that keep "every tuple of an enabled relation has an entry" true.
//
// Coverage is the guarantee Waystone actually makes - location is only
// advisory - so these are the load-bearing half of the structure. Both are
// O(1): a directory walk of at most kMaxDirDepth hops and one 32-byte
// write. Anything heavier on the insert path would be a spec violation,
// because the whole justification for full coverage over a bounded pool
// was that per-tuple maintenance is arithmetic rather than policy.
//
// ---- What is deliberately not here --------------------------------------
//
// No catalog. These take the directory root and depth as data, and never
// read or write sys.tables. Two reasons, and the second is the sharper
// one: a hook that could grow the directory would have to update the
// relation's catalog row, which bumps the catalog version, which clears
// the TableAccess cache - out from under the very statement that is
// holding a `const TableAccess*` and calling the hook. Keeping growth
// outside means the caller sequences it (grow, update the row, re-acquire
// TableAccess, retry) where the hazard is visible.
//
// No relocation hook. Nothing in the engine moves a tuple today - tail
// append never splits, UPDATE is in place, DELETE is a mark - so there is
// no call site for one. It arrives with relayout, alongside
// EpochProvider::BumpFor.
//
// No uniqueness check. Waystone is not an index: ids are unique by
// construction (invariant 10, Catalog::AllocateRowId), and re-reading an
// entry to police that would put a check on the hot path for a condition
// upstream already prevents. OnInsert overwrites whatever is there.
//
// Concurrency: core-local, none of its own (rules.md section 3).

namespace kds::stats {

// A relation's Waystone, as the hooks need it: the two fields of the
// sys.tables triple that describe where its directory is. Taken by value -
// it is 8 bytes, and copying it frees the hook from the lifetime of the
// TableAccess it came from.
struct WaystoneRef {
    PageId dir_root = kInvalidPageId;
    int depth = 0;

    bool valid() const noexcept {
        return dir_root != kInvalidPageId && depth >= 1 && depth <= kMaxDirDepth;
    }

    // Whether the directory as it stands can address `pk`, or whether it
    // has to be deepened first. Callers check this *before* issuing the
    // id if they can, because growth is a catalog write and the middle of
    // an insert is an awkward place for one.
    bool covers(std::uint64_t pk) const noexcept {
        return valid() && pk < DirCoverageAtDepth(depth);
    }
};

// Records that the tuple identified by `pk` now lives at `(page_id, slot)`
// on a heap page whose current epoch is `epoch`. This is what makes the
// coverage guarantee true, and it belongs on the insert path immediately
// after the tuple lands.
//
// Heat starts at zero: the tuple exists, and nothing has read it yet.
//
// Fails with:
//   InvalidArgument  `ws` is not a valid directory, or pk is above kMaxPk
//   OutOfRange       the directory is too shallow for `pk` - grow it with
//                    GrowDirectory(), persist the new root and depth, and
//                    retry. Distinct from InvalidArgument on purpose: this
//                    one is a normal, expected event that the caller
//                    handles, not a bug.
//   ...              whatever the store reports if a page cannot be
//                    allocated, in which case the entry does not exist and
//                    coverage is broken for this pk until a backfill
Status OnInsert(storage::PageStore& store, const WaystoneRef& ws, std::uint64_t pk,
                PageId page_id, std::uint16_t slot, std::uint32_t epoch);

// Clears `kEntryLive` for `pk`. Location and heat are deliberately left
// intact: a delete-mark is not a physical retirement, and the entry still
// describes where the tuple's bytes are for anything that needs to find
// them (spec section 9).
//
// **A pk with no entry is not an error.** The entry page may never have
// been allocated - a relation still backfilling, or one whose Waystone
// pages were dropped - and failing a DELETE because an advisory structure
// is incomplete would violate the rule that dropping Waystone wholesale
// never changes a result (spec section 3, rule 2). Such a call is a no-op
// reporting OK.
Status OnDelete(storage::PageStore& store, const WaystoneRef& ws, std::uint64_t pk);

// Reads the entry for `pk` without interpreting it - the raw half of what
// the probe (W06/T16) will wrap in epoch validation. Returns NotFound when
// the entry page was never allocated, which for a kCovered relation means
// no such tuple was ever inserted.
StatusOr<WaystoneEntry> LookupEntry(storage::PageStore& store, const WaystoneRef& ws,
                                    std::uint64_t pk);

}  // namespace kds::stats
