# An index split can pin past `kPinCeiling`

**Found by reading, not reproduced.** Read at `3ee37d3a` on
`worktree-compress-readme`, 2026-10-13. The code comment has recorded it as
open since AT-S16's review (finding F1).

## What is wrong

An index-tree split holds the leaf, the `k` parents it secured and the one
node each level creates: `2 + k` pins, or `3 + k` when the root grows and
its anchor is pinned (`src/storage/index/index_tree.cpp:492-499`). Unlike
the clustered tree, `k` has no constant bound. An index node holds as few
separators as its key width allows (eight at a 1,000-byte key), so with
wide keys `k` can exceed the five pins that
`DevicePageStore::kPinCeiling = 8` leaves room for
(`include/kds/storage/device_page_store.hpp:804`).

In a debug build the pin check (`src/storage/device_page_store.cpp:2460`)
aborts the process when `live_pins_ > pin_ceiling_`. A release build does
not check.

## Smallest reproduction (not yet run)

`cores = 1`, Debug build: an index on a wide `varchar(1000)` column, with
enough inserts in key order to grow the index tree past six levels, so one
split secures more than five parents.

## What it costs

A debug-build abort. No wrong answer. In a release build the ceiling is
advisory and nothing breaks, but `pin_ceiling_` stops being a true bound on
pins held at once. The fix is the operator's to choose: either re-derive
the ceiling from the widest legal index key, or bound `k`.
