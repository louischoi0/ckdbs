# An index split pins past `kPinCeiling`

**Reproduced** on `fix-index-split-pin-ceiling` at `247bd192`, 2026-10-19,
in a Debug `kds_server` at `cores = 1`. That commit stops `LogBtreeSplit`
re-pinning every split page, a second cause that aborted much narrower
indexes and is no longer part of this defect. The tree-level reproduction
was made on `fix-catalog-page-ceiling` at `8ae05f6a`. First recorded by reading at
`3ee37d3a`, where the code comment had kept it open since AT-S16's review
(finding F1).

## What is wrong

An index-tree split holds the leaf, the `k` parents it secured and the one
node each level creates: `2 + k` pins, or `3 + k` when the root grows and
its anchor is pinned (`src/storage/index/index_tree.cpp`, `SecureParents`
and "Peak pins"). Logging the split adds at most one more pin
(`storage::LogBtreeSplit`). Unlike the clustered tree's, an index node's
separator count follows its entry width, and a separator is a whole leaf
entry, covered bytes included (`docs/spec/index.md` IX4b). So `k` is bounded
only by `kMaxBtreeDepth`, and a wide entry takes it past the five pins that
`DevicePageStore::kPinCeiling = 8` leaves room for
(`include/kds/storage/device_page_store.hpp`). At the widest legal entry
(4,068 bytes) a node holds three separators, which also puts
`kMaxBtreeDepth` (16) within reach.

In a Debug build the pin check (`DevicePageStore::CountPin`,
`src/storage/device_page_store.cpp`) aborts the process once
`live_pins_ > pin_ceiling_`. A release build has no check. That build was not
run.

## Smallest reproduction

`cores = 1`, Debug `kds_server`, against a fresh data file:

```text
CREATE TABLE t (id int64, k int64, c varchar(4000)) BTREE
CREATE INDEX by_k ON t (k) COVERING (c)
INSERT INTO t VALUES (i, i, 'x')              -- i = 1, 2, ... in key order
```

At row 33 the server dies with exit status 134 and `DevicePageStore: 9 live
pins exceeds the ceiling 8`. On the same build these places all their rows: a
covered `varchar(1000)` index (500 rows), a covered `varchar(16)` index
(40,000 rows) and a plain `ON t (k)` index (120,000 rows).

At the tree level, without the WAL, an `IndexInsert` key-order fill at a
covered width of 4,048 bytes, two separators to a node with an anchor page,
reaches a pin high water of 7 at row 64 and 8 at row 128, then aborts before
row 192. It did so in 4 of 4 runs.

## What it costs

A Debug-build abort from ordinary SQL at `cores = 1` on an index whose
entries are a few thousand bytes wide, nothing refusing it first. No wrong
answer: in a release build the extra pins are held and released correctly,
and `pin_ceiling_` just stops being a true bound on pins held at once.

## The fix

Not scheduled, and no work order carries it. The operator chooses between
re-deriving the ceiling from the widest legal index entry and bounding `k`.
