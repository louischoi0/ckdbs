#pragma once

#include "kds/base/common.hpp"

// **A statement that gives its core back at a read that would wait** (DE,
// `instructions/v3.2.0/workorder-de-page-read-off-the-reactor.md` DE-Q9,
// DE-Q10; `docs/spec/page.md` §12).
//
// A pool miss is a blocking read on the reactor's thread, and every other
// session of the core waits it out. Two callers may be refused instead: a
// top-level `SELECT`'s head run inline, by the arm below, and the yielding
// walk's own leaf fetch, by `PageAccess::kScanNoWait`. Either way the store
// first asks the device for the page without waiting
// (`PageDevice::ReadPageNoWait`) and refuses only where it would wait, with
// `StatusCode::kNotResident`.
//
// **The arm is the calling thread's, for one statement's body.** While it is
// set, every fetch that misses the pool on this thread is such a fetch. The
// dispatcher sets it around the body of a statement it can run again - one
// that writes no page and has handed no row on - and drops it before
// anything that records or writes. A reactor runs one statement at a time
// and the body takes no suspension, so a thread-local is per statement, as
// `insert_placement.hpp`'s structural refusal is.
//
// **The note is what the dispatcher obeys, never the outcome** (DE-Q10,
// `[quiet-wrong]`). Some thirty callers on a statement's path recover from
// a failed fetch - a hint miss falls back to a descent, a failed descent to
// a walk, an unreadable undo step to "decided" - and each would turn a
// refusal into an answer. So the store notes the refusal on the thread
// where it makes it, and a body whose note is set is discarded whole and
// run again, whatever it returned. Set only by an armed refusal: the walk's
// own is read off the status by the walk, which may yield and so may not
// leave a thread's word behind.
//
// Lock protocol: none. Three thread-local words, read and written only by
// the thread they belong to; the arm is read on a fetch's miss branch and
// nowhere on a hit.

namespace kds::storage {

namespace detail {
inline thread_local bool g_miss_refusal_armed = false;
// The first page an armed fetch was refused; `kInvalidPageId` is "no note".
inline thread_local PageId g_miss_refusal_page = kInvalidPageId;
inline thread_local PageId g_parked_read_page = kInvalidPageId;
}  // namespace detail

inline void ArmMissRefusal() noexcept { detail::g_miss_refusal_armed = true; }
inline void DisarmMissRefusal() noexcept { detail::g_miss_refusal_armed = false; }
inline bool MissRefusalArmed() noexcept { return detail::g_miss_refusal_armed; }

// The store's, at the refusal. The first page refused is the one kept: it is
// the one the statement meets first when it runs again.
inline void NoteMissRefusal(PageId page_id) noexcept {
    if (detail::g_miss_refusal_page == kInvalidPageId) detail::g_miss_refusal_page = page_id;
}
inline bool MissRefusalNoted() noexcept { return detail::g_miss_refusal_page != kInvalidPageId; }
inline PageId MissRefusalPage() noexcept { return detail::g_miss_refusal_page; }

// **The page a park was just taken for**, named by whoever parked for the
// length of the fetch it makes next - the dispatcher around a statement's
// run-again, the walk around its second fetch. The store books no
// `io.page_read` for that page's read: the park was the event, and the copy
// that follows it is not a second one (DE-R9). Not cleared by
// `ClearMissRefusal`, which a run-again passes through.
class ParkedReadScope {
public:
    explicit ParkedReadScope(PageId page_id) noexcept
        : previous_(detail::g_parked_read_page) {
        detail::g_parked_read_page = page_id;
    }
    ~ParkedReadScope() { detail::g_parked_read_page = previous_; }
    ParkedReadScope(const ParkedReadScope&) = delete;
    ParkedReadScope& operator=(const ParkedReadScope&) = delete;

private:
    PageId previous_;
};
inline PageId ParkedReadPage() noexcept { return detail::g_parked_read_page; }

// Before a statement runs, and once its note has been read: drops the arm
// too, so a statement that left it set - an early return between an arm and
// its drop - cannot refuse the next one's fetches.
inline void ClearMissRefusal() noexcept {
    detail::g_miss_refusal_armed = false;
    detail::g_miss_refusal_page = kInvalidPageId;
}

}  // namespace kds::storage
