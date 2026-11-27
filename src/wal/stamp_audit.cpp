#include "kds/wal/stamp_audit.hpp"

#include <array>
#include <cstddef>

namespace kds::wal {

#ifndef NDEBUG
namespace {

// A thread's pages owed a stamp, each at the highest LSN appended for it: a
// stamp at or past that clears the page. A mutation appends and stamps a
// page or two before its next suspension point, so a few slots are plenty;
// past them a page is counted as an overflow that never clears, which the
// audit then reports - a thread with that many pages unstamped is the
// finding.
struct Note {
    PageId page = kInvalidPageId;
    Lsn lsn = kNoLsn;
};
constexpr std::size_t kNotes = 16;
struct Notes {
    std::array<Note, kNotes> at{};
    std::size_t used = 0;
    bool overflowed = false;
};
thread_local Notes t_notes;

// The record kinds whose page the appender stamps before it suspends -
// every kind that names a page, but one. Found by running the suite with the
// audit reporting each kind it met (DZ-S1): `PAGE_INIT` alone, 31 times.
bool Stamps(RecordType type) noexcept {
    switch (type) {
        // A page created empty: stamped by the record that first fills it,
        // or never - a var-heap body is headerless and has no page_lsn
        // (`wal/log_page_init.hpp`). Until then its frame is dirty with
        // recLSN 0, which a dirty-table walk reads as "nothing to replay",
        // so an unstamped PAGE_INIT hides nothing a stamped one would show:
        // the publish writes the page before its anchor lands either way
        // (DZ-S0's re-read of §5a, case 4).
        case RecordType::kPageInit:
            return false;
        default:
            return true;
    }
}

}  // namespace

void NoteAppendAwaitingStamp(RecordType type, PageId page, Lsn lsn) noexcept {
    if (page == kInvalidPageId || !Stamps(type)) return;
    Notes& n = t_notes;
    for (std::size_t i = 0; i < n.used; ++i) {
        if (n.at[i].page == page) {
            if (lsn > n.at[i].lsn) n.at[i].lsn = lsn;
            return;
        }
    }
    if (n.used == kNotes) {
        n.overflowed = true;
        return;
    }
    n.at[n.used++] = Note{page, lsn};
}

void NoteStamp(PageId page, Lsn lsn) noexcept {
    Notes& n = t_notes;
    for (std::size_t i = 0; i < n.used; ++i) {
        // A stamp at `lsn` covers every record of the page at or below it.
        if (n.at[i].page == page && n.at[i].lsn <= lsn) {
            n.at[i] = n.at[--n.used];
            return;
        }
    }
}

bool AppendAwaitsStampOnThisThread() noexcept { return t_notes.used != 0 || t_notes.overflowed; }

void ResetStampAuditForTest() noexcept { t_notes = Notes{}; }

#else

void NoteAppendAwaitingStamp(RecordType, PageId, Lsn) noexcept {}
void NoteStamp(PageId, Lsn) noexcept {}
bool AppendAwaitsStampOnThisThread() noexcept { return false; }
void ResetStampAuditForTest() noexcept {}

#endif

}  // namespace kds::wal
