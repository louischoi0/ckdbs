#include "kds/wal/stamp_audit.hpp"

#include <array>
#include <cstddef>

namespace kds::wal {

#ifndef NDEBUG
namespace {

// A thread's records appended and not yet stamped. A mutation appends and
// stamps one or two records before its next suspension point, so a few
// slots are plenty; an overflow is counted as a note that never clears,
// which the audit then reports - a thread appending that many records
// unstamped is the finding.
struct Note {
    RecordType type = RecordType::kInvalid;
    PageId page = kInvalidPageId;
    Lsn lsn = kNoLsn;
};
constexpr std::size_t kNotes = 16;
struct Notes {
    std::array<Note, kNotes> at{};
    std::size_t used = 0;
    bool overflowed = false;
    RecordType overflow_type = RecordType::kInvalid;
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
    if (n.used == kNotes) {
        if (!n.overflowed) n.overflow_type = type;
        n.overflowed = true;
        return;
    }
    n.at[n.used++] = Note{type, page, lsn};
}

void NoteStamp(PageId page, Lsn lsn) noexcept {
    Notes& n = t_notes;
    std::size_t out = 0;
    for (std::size_t i = 0; i < n.used; ++i) {
        // A stamp at `lsn` covers every record of the page at or below it:
        // the page's LSN is then past them all.
        if (n.at[i].page == page && n.at[i].lsn <= lsn) continue;
        n.at[out++] = n.at[i];
    }
    n.used = out;
}

bool AppendAwaitsStampOnThisThread() noexcept { return t_notes.used != 0 || t_notes.overflowed; }

RecordType OldestUnstampedTypeOnThisThread() noexcept {
    if (t_notes.used != 0) return t_notes.at[0].type;
    return t_notes.overflowed ? t_notes.overflow_type : RecordType::kInvalid;
}

void ResetStampAuditForTest() noexcept { t_notes = Notes{}; }

#else

void NoteAppendAwaitingStamp(RecordType, PageId, Lsn) noexcept {}
void NoteStamp(PageId, Lsn) noexcept {}
bool AppendAwaitsStampOnThisThread() noexcept { return false; }
RecordType OldestUnstampedTypeOnThisThread() noexcept { return RecordType::kInvalid; }
void ResetStampAuditForTest() noexcept {}

#endif

}  // namespace kds::wal
