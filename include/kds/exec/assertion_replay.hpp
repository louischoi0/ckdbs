#pragma once

#include <cstdint>

#include "kds/base/status.hpp"
#include "kds/exec/bound_cabin.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/record.hpp"

// Replay of the five assertion records (docs/feat-assertion.md §7, workplan
// AST05): the fold that turns a stream of ASSERT_* records back into the
// Bound Cabin state that emitted them - entry pages restored through the
// store, the memory-resident group directory rebuilt beside them.
//
// ---- The fold contract ----------------------------------------------------
//
// One call applies one decoded record, and the caller applies each record
// **once, in stream order**. The page half is naturally idempotent (an entry
// re-written at its index writes the same bytes), but the directory half is
// not - a RESERVE folded twice counts a row twice - so the once-only rule is
// the caller's, exactly as page-level redo gating by `page_lsn` is
// recovery's. There is no LSN on a group header to gate by, which is one
// consequence of AS5's "the running aggregate is not a separate store".
//
// ---- What this deliberately does not cover --------------------------------
//
// **Page creation.** A Bound Cabin page is born through PAGE_INIT with
// `page_type = kCabinBound` (wal/payload.hpp - the payload already carries
// the type byte), which is general recovery's record, not an assertion one.
// This fold assumes the envelope's page exists and is formatted.
//
// **Where replay starts.** A directory folded from records needs the records
// from the cabin's birth - the ASSERT_BUILD run - not merely from the last
// checkpoint, because nothing durable holds the group headers a
// checkpoint-bounded replay would have to start from. Closing that needs the
// checkpoint to persist the directory (or recovery to start assertion replay
// at each cabin's build), and **no milestone owns it**, the same way nothing
// owns single-core recovery itself (docs/workplan-crosscore.md P2's fourth
// bullet). This fold is correct for whatever record range the caller feeds
// it; the range question is recovery's, recorded rather than solved here.
//
// ---- The skip rule --------------------------------------------------------
//
// A record whose assertion the context does not resolve is **skipped whole,
// page half included**, and that is a correctness rule rather than lenience:
// the one legitimate way an id becomes unknown is ASSERT_DROP further down
// the stream, whose teardown freed the cabin's pages - so a later allocation
// may have reused the envelope's page id, and touching it would corrupt an
// unrelated page. The context is where "unknown because dropped" and
// "unknown because the catalog is gone" are told apart; this fold cannot.
//
// Concurrency: core-local, like everything it touches (§6.1).

namespace kds::exec {

// What the fold needs from whoever owns the directories - recovery, or a
// test. Kept an interface so the owner's container and its policy for an
// unknown id (skip, error, rebuild from sys.assertions) stay its own.
class AssertionReplayContext {
public:
    virtual ~AssertionReplayContext() = default;

    // The live directory for `assertion_id`, or nullptr to have the record
    // skipped whole.
    virtual BoundCabin* CabinOf(std::uint64_t assertion_id) = 0;

    // ASSERT_DROP: forget everything held for `assertion_id`. Called even
    // when CabinOf answers nullptr, so dropping twice is the owner's no-op.
    virtual void Drop(std::uint64_t assertion_id) = 0;
};

// True for the five record types this fold handles.
bool IsAssertionRecord(wal::RecordType type) noexcept;

// Applies one record. InvalidArgument for a record type this fold does not
// own; Corruption for a payload that lies (an entry that is not
// kEntryBytes wide, a RESERVE whose entry is not flagged reserved, a BUILD
// whose entry is, an index past the page's append point); otherwise
// whatever the page store or the directory answered.
Status ReplayAssertionRecord(const wal::DecodedRecord& record, storage::PageStore& store,
                             AssertionReplayContext& context);

}  // namespace kds::exec
