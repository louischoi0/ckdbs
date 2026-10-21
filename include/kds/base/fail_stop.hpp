#pragma once

// Stopping the process where an allocation or an exception leaves a statement
// in a state nothing can continue from (`docs/spec/sched.md` §3).
//
// **Where the stop happens matters.** An exception unwinds before anything
// can catch it: page latches and pins are released, holds are given back, and
// a writeback on another core may copy a page whose mutation has no log
// record yet. Stopping at the allocation itself, with every latch still held,
// leaves nothing for that writeback to find - **on a path that logs under
// its holds** (`docs/spec/wal.md`, AT-S21). The paths named in
// `git show d83bd502:docs/inflight/bugs/records-appended-after-their-page-is-released.md` do
// not, and a stop on one has only the window a crash there already has.

namespace kds::base {

// Names `what` on stderr and aborts.
[[noreturn]] void FailStop(const char* what) noexcept;

// Makes a failed `operator new` stop the process at the allocation
// (`std::set_new_handler`), before `std::bad_alloc` is thrown and unwound.
// Called once, first thing in a server's `main`.
void InstallAllocationFailStop() noexcept;

}  // namespace kds::base
