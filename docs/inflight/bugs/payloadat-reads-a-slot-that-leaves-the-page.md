# `PayloadAt` reads a slot that leaves the page

**Found by reading, not reproduced.** Verified at `67b23a28` on
`worktree-bt-accept-tuple-row`, by BT-S2's `critics-developer` pass
(`instructions/v3.0.0/workorder-bt-accept-tuple-row.md` §6).

## What is wrong

`PageView::PayloadAt` (`src/storage/heap/heap_page.cpp:248`) makes
`ReadTupleIn`'s range and retired checks and none of the others. It reads
`data_len` at `slot.offset + kTupleDataLenOffset` and returns a span of
`data_len` bytes from `slot.offset + kTupleHeaderOnDiskSize` without asking
whether the slot lies inside the page (BF-R10) or whether `data_len` fits
the slot. `slot.offset` is a `uint16_t`, so a directory entry can name any
offset up to 65,535 of an 8,192-byte frame.

`ReadTupleIn` refuses exactly that slot `Corruption` ("leaves the page"):
BF-R10's reason is that a stale location on a reused id can name a page of
another class - a var-heap or an index page - whose bytes read as a
directory entry pointing anywhere. `PayloadAt` was not given the check.

Callers: `src/storage/btree/btree.cpp:24` (a leaf's key read),
`src/exec/step_vm.cpp:1967` (a prefixed walk's key mark),
`src/txn/manager.cpp:464`, `src/txn/recovery_undo.cpp:26`,
`src/server/command_dispatcher.cpp:6542`.

## Smallest reproduction

The page edit of `tests/scan_row_walk_test.cpp`'s
`ASlotWhoseOffsetLeavesThePageIsNoRow` (slot 3's offset set to
`kPageSize - 4`), then `PayloadAt(3, slot_count())` on that page: it reads
two bytes past the frame for `data_len` and returns a span that starts
past it. Not run.

## What it costs

A read past the frame on a corrupt or stale page: undefined behaviour, and
a key or payload taken from bytes that are not this page's. On a page the
store describes correctly it costs nothing.

## The fix, known and unscheduled

Since BT-S2, `PageView::TryReadTupleIn` makes every check `ReadTupleIn`
makes with no `Status`. `PayloadAt` becomes `TryReadTupleIn` and, on any
reason but `kRead`, `ReadTupleIn`'s wording - one body for the checks. It
turns a garbage span into `Corruption` for the slots above, a behaviour
change on corrupt pages only, so it waits for the operator's word; no work
order carries it.
