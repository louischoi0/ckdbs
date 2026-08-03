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
// a pk it recorded (`docs/waystone-concpets.md` §2 rules 1-2); a Cabin entry
// carries the same triple as an advisory *hint* on top of an authoritative
// pk (`docs/feat-cabin.md` C6, which requires the check to run "under the
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
// ---- What this deliberately does not check -------------------------------
//
// **The page epoch.** There is none in this engine, so Waystone spec §2's
// rule 2 is unenforceable and Cabin's `page_epoch` is written 0. Sound only
// while a tuple's address is stable for life - invariant 13 stops an UPDATE
// migrating one and nothing relayouts - which means **the epoch must land
// with relayout, whichever comes first**. When it does, it lands *here*, and
// both callers get it at once. That is the whole argument for this file
// existing.
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

// Fetches `page_id` for read, reads `slot`, and compares the tuple's
// Keystone id against `expected_pk`.
//
// Never returns a Status: there is no failure here that is not a miss, and
// giving one a Status would put a "the trail was corrupt" error in front of
// a user whose query is perfectly answerable.
VerifiedTuple VerifyTupleAt(storage::PageStore& store, PageId page_id, std::uint16_t slot,
                            std::uint64_t expected_pk);

}  // namespace kds::exec
