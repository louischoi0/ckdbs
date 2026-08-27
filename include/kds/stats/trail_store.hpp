#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/exec/trail_collector.hpp"
#include "kds/stats/instance_key.hpp"
#include "kds/stats/waystone.hpp"
#include "kds/storage/page_store.hpp"

// Writing and reading one pattern instance's trail
// (docs/inflight/in-progress/waystone-workplan.md P08).
//
// This is the layer that turns what the executor saw
// (`exec::TouchedTuple`, five integers) into the on-disk entry format
// (`WaystoneEntry`) and back. It owns the format; the executor does not
// know it exists, and the recorder above does not know the byte layout.
//
// ---- One page, and a trail that would not fit is not written -----------
//
// A trail lives in exactly one waystone page: 253 entries
// (`kEntriesPerWaystonePage`). The `next_page_id` continuation the format
// reserves is **never used** - every trail this writes ends its chain.
//
// Spec section 9 left the per-instance cap open; it is settled here at one
// page, and the failure mode is the reason. A trail longer than a page
// could be truncated, but a truncated trail is one that silently covers
// only the first rows of an execution and **no reader can tell it from a
// complete one** - replay would serve the first N rows from the trail and
// have no idea the rest existed. Not recording is the honest failure: the
// instance simply has no trail, which is the state every instance starts
// in and which every consumer already handles.
//
// ---- What a trail is not ------------------------------------------------
//
// Re-recording an instance **replaces its trail wholesale**, never merges.
// A merge would accumulate rows from earlier executions that no longer
// qualify, and nothing at this layer can tell those from rows that still
// do - the trail records where an execution looked, not what is true.
//
// ---- Two things worth knowing before building on this ------------------
//
// **`page_epoch` is recorded for real since 2026-08-09** (workplan PX04):
// the executor observes the page's relayout epoch at access
// (`storage/page_header.hpp`, `docs/spec/physical-optimizer.md` R4), this
// layer narrows it to the entry format's u32, and replay compares it in
// `exec/tuple_verify.hpp` - spec section 2's rule 2, enforceable at last.
// The entry layout did not change: the field was always here, written 0
// while the engine had no epoch. Until a mover exists every comparison is
// between two zeros - real check, constant inputs - and the hand-bumped
// contract test is what proves it would fire.
//
// **Trails are unlogged.** No WAL record describes one, so a crash loses
// whatever the last checkpoint did not carry. That costs replays and never
// results (spec section 9, invariant 8). The pages are ordinary headered
// `PageType::kWaystone` pages, so they keep their checksum and page_lsn.
//
// Concurrency: core-local, no synchronization of its own (rules.md #3).

namespace kds::stats {

// The most entries one trail can hold. A collector sized to this can never
// gather more than a trail could store.
inline constexpr std::size_t kMaxTrailEntries = kEntriesPerWaystonePage;

// Records `touched` as the trail for `key`, replacing whatever was there.
//
// `root`/`depth` are the pattern's directory pair, straight off its
// `sys.patterns` row - this layer never touches the catalog.
//
// Fails with:
//   InvalidArgument  an empty trail, or a depth outside [1, kMaxPatternDirDepth]
//   OutOfSpace       more than kMaxTrailEntries entries; nothing is written
//   (store errors)   propagated from the directory walk or the page fetch
//
// **The colliding-page policy is `[PROPOSED]` displace** - see
// `TrailDisplacesOnCollision()` below.
Status WriteTrail(storage::PageStore& store, PageId root, int depth, const InstanceKey& key,
                  std::span<const exec::TouchedTuple> touched, std::uint64_t recorded_ts);

// The trail recorded for `key`, or an **empty vector** when there is none.
//
// An absent trail is not an error, and neither is a foreign one: the
// directory is keyed by a hash, so a walk can land on a page belonging to a
// different instance, and `WaystonePageHolds()` turns that into the same
// empty answer a cold directory gives. Every reason to answer "nothing"
// folds into one, because a caller must do the same thing in all of them -
// fall through to the authoritative path.
StatusOr<std::vector<WaystoneEntry>> ReadTrail(storage::PageStore& store, PageId root, int depth,
                                               const InstanceKey& key);

// Whether a trail write may take over a page already holding a *different*
// instance's trail.
//
// Spec section 9 leaves `arg_hash` collision handling open - chain,
// displace, or drop - and this is the one place the choice is made, so the
// other two stay a one-line change.
//
// `[PROPOSED]` **displace**, i.e. true. The reasoning: with *drop*, the
// loser of a collision can never record at all, permanently, and it has no
// way to find out why. With displace the two instances take turns - each
// eviction costs the victim a header mismatch, a miss, and a re-record on
// its next execution, which is self-healing and safe by invariant 8.
// Chaining is the only option that serves both, and it needs an eviction
// policy that does not exist yet (P15).
constexpr bool TrailDisplacesOnCollision() noexcept { return true; }

}  // namespace kds::stats
