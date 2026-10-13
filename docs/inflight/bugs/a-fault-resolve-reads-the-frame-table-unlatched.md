# A fault's `Resolve` reads the frame table with no latch held

**Found by reading, not reproduced, not verified reachable.** Read at
`3ee37d3a` on `worktree-compress-readme`, 2026-10-13, during the
code-comment compression pass.

## What is wrong

On the armed (multi-core) miss path of `DevicePageStore::FetchAndPin`, the
loader publishes its `loading_` entry and calls `hold.unlock()`, and only
then calls `Resolve` (`src/storage/device_page_store.cpp:~2300-2310`).
`Resolve` calls `ResidentBytes` (`:898`), which begins with
`frames_.find(page_id)`. When that find hits, it also writes the frame's
`dirty` and `dirty_gen` (`MarkDirty`) and bumps `usage` (`:905-914`). All
of this runs with the structure latch released.

Other paths in the same file treat `frames_` as a table another core may be
growing. `StampPageLsn` (`:1580-1586`) takes the latch for the `find`
alone, because "a miss on a rehash would lose the stamp of a resident page"
(`tests/frame_table_race_test.cpp`). The loader's `find` gets no such
protection. The `loading_` entry keeps other loaders of the same page out,
but it does not stop an insert of a *different* page from rehashing the
table.

Not checked: whether the miss branch reaches anything after the unlocked
`find` without retaking the latch, and whether the frame table's node
stability (BE-S6) makes an unlatched `find` safe in practice.

## What it costs

If it is reachable, it is a data race on the frame table: a lost or torn
lookup during a concurrent rehash, and an unlatched write of `dirty`,
`dirty_gen` and `usage`. A lost dirty mark would make the next writeback
skip a page. That is a quiet durability loss, which is why this entry ranks
above its probability. First step: a TSan run of
`tests/frame_table_race_test.cpp` with the fault path added, or a
cell that faults distinct pages on two cores while a third inserts.
