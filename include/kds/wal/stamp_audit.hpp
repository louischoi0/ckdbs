#pragma once

#include <cstdint>

#include "kds/base/common.hpp"
#include "kds/wal/record.hpp"

// **The fuzzy dirty table's premise, checked** (DZ-S1, the DZ order's §2
// item 2): no core yields between a record's append and its page's stamp.
// DO's §5a and its DZ-S0 re-read both rest on it - a record appended and not
// yet stamped is a change a dirty-table walk cannot see in its page's recLSN
// - and before DZ the code stated it and nothing checked it
// (`known-gaps.md`, "The frame table's structure latch (DO)").
//
// A thread notes each record it appends that names a page
// (`WalManager`'s appends), and the store's `StampPageLsn` clears the notes
// its stamp covers. The executor's suspend audit (`exec/step_vm.hpp`) asks
// `AppendAwaitsStampOnThisThread` at every coroutine suspension, so a
// suspension between the two is recorded as the audit's other findings are:
// in debug builds, read by a test, never fatal (`sched/coro.cpp`'s rule).
//
// **Not every record that names a page stamps it**: `Stamps`
// (stamp_audit.cpp) is the set noted, every kind left out with its reason.
// Release builds note nothing and answer false.

namespace kds::wal {

void NoteAppendAwaitingStamp(RecordType type, PageId page, Lsn lsn) noexcept;
void NoteStamp(PageId page, Lsn lsn) noexcept;
bool AppendAwaitsStampOnThisThread() noexcept;
// Drops this thread's notes: a test's, after it provoked one.
void ResetStampAuditForTest() noexcept;

}  // namespace kds::wal
