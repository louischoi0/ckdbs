# A root growth's anchor publish is logged after its split, so a cut between them strands the rows the split moved

**Reproduced.** BF-S1's Census D found it by reading. A crash cell
reproduced it on `worktree-agent-a6378d577b6230ab1` at `c4115721`, a
branch based on `cd8ca91e`, which contains `df741e3c`. The cell was written
for this entry and is not in the suite.

## What is wrong

When an insert grows the clustered tree a level, the new root is published
twice. It is in the `BTREE_SPLIT` record's images, and it is in the anchor
page's clustered-root slot. The slot is written later, by `publish_root`
(`command_dispatcher.cpp:5628`), through `Catalog::UpdateRelationDescPage`
and `WriteAnchorRoot`, as its own `ANCHOR_UPDATE` with `kNoTxnId`.

That record is appended after `LogInsert` has logged the split and released
the holds. An autocommit insert that grows the root therefore logs five
records, in this order:
1. `TXN_BEGIN`
2. `UNDO_WRITE`
3. `BTREE_SPLIT`
4. `ANCHOR_UPDATE`
5. `TXN_COMMIT`

The take-back path (`:5498-5506`) logs in the same order.

Suppose the log is durable through `BTREE_SPLIT` and not through
`ANCHOR_UPDATE`. That happens when another session's commit syncs the log
between the two appends. Then:
- the mount's redo applies the split, so the old root becomes the new
  root's leftmost child and is marked grown-over;
- the anchor still names the old root, because only redo of an
  `ANCHOR_UPDATE` writes that slot (`redo.cpp:283-293`);
- `InitTableAccess` takes the root straight from the anchor
  (`catalog.cpp:2271`);
- the grown-over flag is read only on the write side
  (`btree.cpp:471-478`).

**Every committed row the split moved right is now unreachable by key.** A
point lookup answers `TXN_CONFLICT retryable=1 ... gave up after 5 attempts`,
and retrying cannot help, because no newer root exists to be found. An
insert into that range is refused the same way. A full walk still returns
every row, because it starts at the leftmost leaf and follows the sibling
chain.

The insert's own row is not lost: the transaction is a loser and is undone.
The rows lost to key access belong to other, earlier transactions, and the
damage persists across every later mount.

## The reproduction

`tests/sorted_leaf_crash_test.cpp`'s `CutEverywhere` cuts the log after
every record a statement added and mounts each cut through `Expeditor::Open`.

- **A root leaf that divides.**
  - Setup: `CREATE TABLE t (id int64, v int64) BTREE`, then 198 rows with
    ids 10, 20, ..., 1980, which is one full leaf with the root a leaf.
  - Snapshot, `INSERT INTO t VALUES (15, 0)`, snapshot, and cut everywhere.
  - Expected at each cut: `SELECT id FROM t` and a point lookup of every
    committed id both answer it.
  - Observed after the `BTREE_SPLIT` cut: the walk returns 198 of 198 rows,
    point lookups miss 197 of 198 (ids 20 to 1980), and the anchor names
    page 129, the old root.
- **An internal root that divides.**
  - Setup: rows of about 4 KB in `t (id int64, s varchar(4000))`, two to a
    leaf, 1358 rows, so the internal root is full.
  - Then `INSERT (15, pad)`.
  - Observed after the `BTREE_SPLIT` cut: the walk returns 1358 of 1358,
    point lookups miss 680 of 1358 (ids 6790 to 13580), and an insert of
    13581 is refused.
- Every other cut of both statements is clean.

## What it costs

**A quiet loss of access, not a refusal at mount.** A volume mounts and
serves. Every committed row the split moved can no longer be read by key,
updated or deleted, until something rewrites the anchor. Nothing does.

## Not checked

The secondary-index variant goes `index_maintain.cpp:179`, then
`UpdateIndexRoot` (`catalog.cpp:3525-3577`), in the same order. The existing
`ALogCutInsideAnIndexSplitLeavesEveryRowOnItsIndex` grows an index root only
by an append split, which moves no committed entry. A divide of an index
root very likely loses the same way, and no cell has shown it.

## The fix

Not scheduled, and no work order carries it. There are two candidate
shapes:
- log the anchor slot inside the split's record, so both are replayed
  together or neither is (BD-R12's whole-or-nothing, extended to the
  anchor);
- have the mount, or a reader, follow a grown-over root up to the root
  that grew over it.

Neither is BF's: BF reclaims pages and does not touch how a live tree
publishes its root. BF's reclaim walk is unaffected in the safe direction.
Started from a grown-over root, a descent misses the new root's right half,
and BF-R3's leaf-chain arm still reaches every leaf. What the walk misses is
the new root and its internal nodes, which leak and are never freed in
error.
