# An index split pins past `kPinCeiling`

**Reproduced** on `fix-catalog-page-ceiling` at `8ae05f6a`, 2026-10-19, in a
Debug build at `cores = 1`, both through SQL and through a direct
`IndexInsert` gtest. **Read unchanged** at `f3ec25ea` (`origin/main`): no
commit between the two touches the cited files. First recorded by reading at
`3ee37d3a`, where the code comment had kept it open since AT-S16's review
(finding F1).

## What is wrong

Two causes add up, and only the second needs a wide entry.

**1. `LogBtreeSplit` pins every split page a second time.**
`include/kds/storage/log_page_image.hpp`, `LogBtreeSplit`, calls
`store.Get(page_id)` for each page it images, while the split's own holds,
which `IndexInsert` hands out, and the staged anchor
(`CommandDispatcher::LogTreeSplit`) are still live. Only the anchor is
imaged through the caller's hold (the function's anchor paragraph, which
gives the ceiling as the reason); the split pages are fetched again although
the caller holds each one. Logging a split
therefore roughly doubles the split's pins, so the first split that grows a
secondary index to a third level, with only one parent secured, already goes
past 8. The gdb stack at the abort is `DevicePageStore::CountPin` <-
`PageStore::Get` <- `LogBtreeSplit` (5 pages) <- `LogTreeSplit` <-
`LogIndexWrite` <- `AppendIndexEntry` <- `InsertOneRow`. A clustered tree
logs through the same function, but its bounded `k` kept it under the ceiling
in the control run (5,000 rows, two to a leaf).

**2. The number of parents a split secures has no constant bound.** An index-tree split holds
the leaf, the `k` parents it secured and the one node each level creates:
`2 + k` pins, or `3 + k` when the root grows and its anchor is pinned
(`src/storage/index/index_tree.cpp`, `SecureParents` and "Peak pins").
Unlike the clustered tree's, an index node's separator count follows its
entry width, and a separator is a whole leaf entry, covered bytes included
(`docs/spec/index.md` IX4b). So `k` is bounded only by `kMaxBtreeDepth`, and
a wide entry can take it past the five pins that
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
CREATE TABLE t (id int64, k int64, c varchar(N)) BTREE
CREATE INDEX by_k ON t (k) COVERING (c)      -- or no COVERING clause
INSERT INTO t VALUES (i, i, 'x')              -- i = 1, 2, ... in key order
```

The server dies with exit status 134 and `DevicePageStore: 9 live pins
exceeds the ceiling 8`:

| index | entry width | dies at |
|---|---|---|
| covered `varchar(4000)` | 4,017 | row 5 |
| covered `varchar(1000)` | 1,017 | row 37 |
| covered `varchar(16)` | 33 | rows 27,001-28,000 |
| `ON t (k)`, no covered column | 17 | rows 92,001-93,000 |

In the controls the server survives: with no index at `varchar(4000)` it
places 1,000 and then 5,000 rows, and with a covered `varchar(16)` index it
places 1,000 rows.

At the tree level, without the WAL and so isolating cause 2, an
`IndexInsert` key-order fill at a covered width of 4,048 bytes, two
separators to a node with an anchor page, reaches a pin high water of 7 at
row 64 and 8 at row 128, then aborts before row 192. It did so in 4 of 4 runs.

## What it costs

A Debug-build abort from ordinary SQL at `cores = 1`, nothing refusing it
first. It is reached by a plain secondary index at about 92k key-ordered
rows. No wrong answer: in a release build the extra pins are held and
released correctly, and `pin_ceiling_` just stops being a true bound on pins
held at once. Restart after the abort was not tested.

## The fix

Not scheduled, and no work order carries it. Cause 1: imaging the split
pages through the caller's holds, as the anchor already is, would remove the
second pins. Cause 2 is the operator's to choose: re-derive the
ceiling from the widest legal index entry, or bound `k`. Fixing cause 2
alone would leave the abort on the SQL path.
