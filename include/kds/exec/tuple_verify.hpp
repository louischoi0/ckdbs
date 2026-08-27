#pragma once

#include <cstdint>
#include <optional>

#include "kds/base/common.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/page_store.hpp"

// **The one verifier.** Whether the tuple a remembered location names is
// still the tuple that was remembered.
//
// Two subsystems ask this question, and they are deliberately made to ask it
// through the same code. A Waystone trail entry names `(page_id, slot)` for
// a pk it recorded (`docs/spec/waystone-concpets.md` §2 rules 1-2); a Cabin entry
// carries the same triple as an advisory *hint* on top of an authoritative
// pk (`docs/spec/cabin.md` C6, which requires the check to run "under the
// same rules as waystone entries - shared validation code, not a parallel
// implementation, two verifiers would be where the bugs live").
//
// ---- What a failure means, and what it does not --------------------------
//
// Every outcome below other than kOk is a **miss**, never an error. The
// caller falls back to the authoritative path - a descent, a walk, or a pk
// resolution - and the statement's answer is unchanged. That is what makes
// both structures deletable: a trail wholesale (invariant 8), a Cabin value
// at a time (§1's corollary).
//
// The outcomes are kept distinct only so a caller can *count* them. No
// caller may treat one as more authoritative than another - a caller that
// could tell "the page went away" from "someone else's row is there" would
// be tempted to trust the second.
//
// ---- The epoch check (docs/spec/physical-optimizer.md R4, PX04) ----------
//
// **The page epoch is checked here since 2026-08-09**, which is Waystone
// spec §2's rule 2 made enforceable and the Cabin's `page_epoch` made a
// real comparison - landed *here*, once, so both callers got it at once,
// which was the whole argument for this file existing. The recorded epoch
// comes in from the caller's entry; the page's current epoch is read off
// the fetched bytes; a mismatch is a miss like every other outcome. Until
// a mover exists every comparison is between two zeros - the check is real
// and its inputs are constant, which the hand-bumped-epoch contract tests
// pin from both suites.
//
// R4's pairing rule holds in both directions: the epoch never *accepts* a
// location on its own (the Keystone-id check below still runs on an epoch
// match), it only rejects a whole page's worth of recorded locations at
// once. The stored width is the entry formats' u32; the header's u64 is
// compared truncated, which reintroduces a wrap only after 2^32 relayouts
// of one page and is harmless anyway - a wrap collision merely readmits a
// location to the id check.
//
// ---- What this deliberately does not check -------------------------------
//
// **That the page still belongs to the relation it was recorded from.**
// Nothing asks a page which relation owns it, because nothing can. Both
// callers check the relation against the *query* instead, which is
// sufficient only while pages are never freed and reallocated between
// relations - i.e. until `DROP TABLE` or page reuse exists.
//
// **Visibility.** MVCC is applied by the code that decodes the tuple,
// exactly as it would be on the authoritative path. A verifier that filtered
// by snapshot would be a second visibility rule.

namespace kds::exec {

enum class VerifyOutcome : std::uint8_t {
    // The tuple at `(page_id, slot)` carries the expected Keystone id.
    kOk,
    // The page could not be fetched.
    kPageGone,
    // The slot is retired, dead, or past the directory's end.
    kSlotGone,
    // The page's relayout epoch no longer matches the recorded one: tuples
    // on this page have moved since the location was recorded, so every
    // location recorded against it is untrusted at once (R4). Checked
    // before the slot is read - the point of an epoch is rejecting a page
    // without trusting anything on it.
    kEpochMismatch,
    // A tuple is there and it is a **different** one. This is the check that
    // makes a stale remembered location a miss rather than somebody else's
    // row, and it is the one the corrupted-trail contract test exists to
    // prove load-bearing.
    kPkMismatch,
};

struct VerifiedTuple {
    VerifyOutcome outcome = VerifyOutcome::kPageGone;

    // The page's bytes, set only when `outcome == kOk`, so the caller reads
    // the row without asking the store for a page this call just had in
    // hand. Empty otherwise.
    std::optional<heap::PageView> page;

    bool ok() const noexcept { return outcome == VerifyOutcome::kOk; }
};

// Fetches `page_id` for read, compares its relayout epoch against
// `recorded_epoch`, then reads `slot` and compares the tuple's Keystone id
// against `expected_pk`.
//
// Never returns a Status: there is no failure here that is not a miss, and
// giving one a Status would put a "the trail was corrupt" error in front of
// a user whose query is perfectly answerable.
VerifiedTuple VerifyTupleAt(storage::PageStore& store, PageId page_id, std::uint16_t slot,
                            std::uint64_t expected_pk, std::uint32_t recorded_epoch);

// The epoch to stamp on a location being *written*: `page_id`'s current
// relayout epoch, truncated to the entry formats' u32, and 0 for a page
// that cannot be read.
//
// The one producer, beside `VerifyTupleAt`'s one consumer, and here for the
// same reason that one is: a writer that stamps 0 against a bumped page
// records a hint that misses on every later resolve and is re-healed
// forever. Its callers are the three sites that mint or repair a hint - the
// read path's in-place heal, the optimizer's batch heal, and the Cabin
// write hook - and a fourth that spelled the rule again would be where the
// two came to disagree.
//
// 0 on an unreadable page is deliberately the same value "never relayouted"
// carries: the entry is about to be verified against the page anyway, so
// the harmless direction is the one that re-checks rather than the one that
// rejects.
std::uint32_t CurrentRelayoutEpoch(storage::PageStore& store, PageId page_id);

}  // namespace kds::exec
