# The suspend audit counts other cores' pins

**Found by reading, not reproduced.** Read at `3ee37d3a` on
`worktree-compress-readme`, 2026-10-13, during the code-comment compression
pass.

## What is wrong

`CoreRuntime` installs the pin half of the suspend audit only on the core
that owns the store, which is core 0
(`src/server/core_runtime.cpp:469`, `owned_store_ != nullptr ? store_ :
nullptr`). The audit then checks `g_audit_store->live_pins() != 0`
(`src/exec/step_vm.cpp:2951`).

`live_pins_` (`include/kds/storage/device_page_store.hpp:1579`) is the
store's total across **every** thread that pins it: peer cores, the
data-sync thread and the backup copier. The comment at
`core_runtime.cpp:463-468` gives this as the reason a *borrowing* core
installs null. The owning core has the same exposure. When core 0 suspends
while a peer holds a pin, the audit records "a coroutine suspended while
holding a page pin", but core 0 holds none. The read of `live_pins_` is
also unlatched.

## Smallest reproduction (not yet run)

`cores = 2`: core 1 runs a long write that holds a pin, while core 0's
`SELECT` yields at a 64-page slice boundary. Afterwards
`sched::SuspendAuditTripped()` reads true on core 0's thread.

## What it costs

Diagnostic only. A trip sets a thread-local flag that tests read
(`src/sched/coro.cpp`). Nothing aborts and no answer changes. The cost is a
possible flaky test that asserts the flag on a multi-core rig, and an audit
that cannot tell a real park-with-pin on core 0 from a peer's ordinary pin.
A fix would count pins per thread rather than per store.
