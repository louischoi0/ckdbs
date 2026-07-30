#pragma once

#include <cstdint>
#include <optional>

#include "kds/base/status.hpp"
#include "kds/stats/waystone.hpp"
#include "kds/stats/waystone_hooks.hpp"
#include "kds/storage/page_store.hpp"

// The read side (waystone-concpets.md sections 3.1 and 9): given a pk,
// where should a reader look first?
//
// This is the piece the 2026-07-30 amendment to invariant 8 exists for. It
// is also the piece most able to turn an advisory structure into a wrong
// answer, so the contract is narrow and stated twice - once here and once
// at the top of ProbeResult in waystone.hpp.
//
// ---- What Probe() decides, and what it does not -------------------------
//
// It decides **where to look**. It does not decide what exists, what is
// visible, or what a query returns. Concretely, the caller still owes
// three things after a trusted result, and the probe cannot do any of
// them because it does not know the heap's tuple format or the reader's
// snapshot:
//
//   1. Read the tuple at (page_id, slot) and compare its Keystone id
//      against the pk asked for. A mismatch is a *stale entry*, not
//      corruption, and it means fall through - not fail.
//   2. Apply MVCC visibility exactly as the authoritative path would.
//   3. On any miss or mismatch, fall through to that authoritative path:
//      the B+ tree when it exists, the heap chain scan until then.
//
// ProbeAndVerify() below does step 1 for callers that hand it a way to
// read a Keystone id, which is most of them - the intent is that nobody
// hand-rolls the comparison and gets its sense backwards.
//
// ---- Why a miss is not "no such row" ------------------------------------
//
// Only for a kCovered relation does a missing entry mean the tuple does
// not exist, and even then this layer will not say so: the caller knows
// the relation's WaystoneState and this function does not take one. A
// probe reports "I cannot help", never "there is nothing there". Treating
// a miss as an answer is the single mistake that would break rule 2 of the
// advisory contract, and it is worth the lost optimization to make it
// unavailable here.
//
// Concurrency: core-local, none of its own (rules.md section 3).

namespace kds::stats {

// Looks up `pk` and validates the recorded location against the heap
// page's current epoch.
//
// Never fails for an absent entry: a pk with no entry page, a cleared
// kEntryLive, an unset location, or an epoch mismatch all come back as
// `trusted == false`, because a caller must treat them identically and
// four different Statuses would invite handling three of them. Status is
// reserved for a caller error (an unusable directory, a pk outside the
// 40-bit range or past the directory's depth) and for a store that cannot
// hand over a page.
//
// `use_count` rides along for consumers that want heat; it is best-effort
// and zero until the aggregator (T14) exists to raise it.
StatusOr<ProbeResult> Probe(storage::PageStore& store, const WaystoneRef& ws,
                            const EpochProvider& epochs, std::uint64_t pk);

// What a caller must supply to close the last hole: given a page and slot,
// the Keystone id of the tuple actually living there, or nullopt if the
// slot holds nothing readable (retired, out of range, empty).
//
// A function rather than an interface because the one implementation the
// engine needs is three lines over PageView, and an interface would only
// add a vtable between the probe and its own safety check.
using KeystoneIdAt = std::optional<std::uint64_t> (*)(storage::PageStore&, PageId,
                                                      std::uint16_t, void*);

// Probe(), plus the Keystone-id check of spec section 3.1 rule 2. Returns
// a result whose `trusted` is true only if the tuple at the reported
// location really does carry `pk`.
//
// This exists so the check has exactly one implementation. A stale entry
// pointing at a live tuple with a *different* pk is the failure mode that
// produces a wrong row rather than a slow one, and it is not hypothetical
// the moment anything relocates a tuple without bumping an epoch.
//
// `ctx` is passed through to `id_at` untouched.
StatusOr<ProbeResult> ProbeAndVerify(storage::PageStore& store, const WaystoneRef& ws,
                                     const EpochProvider& epochs, std::uint64_t pk,
                                     KeystoneIdAt id_at, void* ctx);

// Records a fresh observation of where `pk` lives, refreshing the entry's
// location and epoch. The write half of the read path: a probe that
// missed on an epoch mismatch, followed by an authoritative lookup, should
// leave the entry usable again rather than permanently stale.
//
// A no-op reporting OK when the entry does not exist or is not live -
// re-observing a tuple Waystone does not cover is not this function's job
// to fix (that is backfill's, T17), and silently resurrecting a cleared
// entry would undo a delete-mark.
Status Observe(storage::PageStore& store, const WaystoneRef& ws, std::uint64_t pk,
               PageId page_id, std::uint16_t slot, std::uint32_t epoch);

}  // namespace kds::stats
