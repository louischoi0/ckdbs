# Records appended after their page is released, outside the insert path

**Found by reading, not reproduced.** Verified at `8f9a887` on
`at-s21-log-under-hold`, by AT-S21's survey (the AT-close order's §7.3: "any
[path] that does not [log under its hold] is either fixed here or recorded
as a bug entry by name"). AT-S21 fixes the insert path, its index
maintenance and its spills; these it does not, and each is named here.

## What is wrong

`wal.md` §8-1: a record is generated, appended and its page's `page_lsn`
stamped while the page is held. Each path below mutates the page, lets the
hold go, and appends later. Two costs follow from that shape, in the
entry `an-insert-is-logged-after-its-leaf-is-released.md` described for the
insert: **a writeback between the mutation and the stamp** puts the change
on disk ahead of its record (the store's gate compares against the old
`page_lsn`), and **a slot-relative record** logged after another core's
write to the same page lands out of order, which redo refuses
(`heap_page.cpp`'s dense-slot check, `varheap.cpp`'s, `redo.cpp`'s
`ApplyIndexInsert`) - a failed mount, not a quiet divergence.

| Path | Page | Record | Severity |
|---|---|---|---|
| `SortedFillInner` (`command_dispatcher.cpp`) over `heap::ChainAppendBatch` | heap tail, new chain pages, link edits | `FULL_PAGE_IMAGE` per page, after the batch | **Heap-only**, so SUS-1 limits it to relations created before the suspension; another core's per-row `HEAP_INSERT` into the same tail can be logged against a page without the batch's rows |
| `Catalog::InsertRow`, new-page arm via `AllocateCatalogPage` (`catalog.cpp`) | a new catalog overflow page | `PAGE_INIT`, `HEAP_INSERT` | `AllocateCatalogPage` returns the new page's bytes with its `PageRef` already destroyed, so the rows are written through a span to an **unpinned, unlatched** frame - a use-after-free once eviction takes it, and a flush-before-log (its `page_lsn` is 0). The old tail stays held, so no other writer reaches the page |
| `InsertAssertion` → `heap::ChainInsert` → `exec::LogChainInsert` (`assertion_catalog.cpp`, `wal_row_log.cpp`) | `sys.assertions` tail, new page, link; spills | `PAGE_INIT`, `FULL_PAGE_IMAGE`, `VARHEAP_APPEND`, `HEAP_INSERT` | Two cores' `CREATE ASSERTION` at once: out-of-order dense slots refuse the mount |
| `BuildIndexTree` / `Backfill` → `LogBuiltTree` (`index_ddl.cpp`) | every page of a new index tree | `FULL_PAGE_IMAGE` | The tree is unpublished, so no other writer; only a flush-before-log of `page_lsn` 0 pages |
| `CreateTable`'s var-heap root, `varheap::CreateChain` → `LogCatPageInit` (`catalog.cpp`) | the new var-heap root | `PAGE_INIT` | Unpublished; flush-before-log only |
| `AssertionEnforcer::AbortTxn` (`assertion_check.cpp`) | a Bound Cabin page (`MarkOrphaned`) | `ASSERT_ROLLBACK` | The inverse shape: **appended before the page is taken**, under the directory latch only, stamped after. Another core's `ReserveOne` or `CommitTxn` stamping the page in between is then followed by this stamp, which **lowers** `page_lsn` (`StampPageLsn` does not check it only rises) |

## The fix

Each path's own: keep the `PageRef` from the mutation to the stamp, as
AT-S21 does for the insert. The first row is heap-gated; the two
unpublished-tree rows cost only a flush ordering; `AllocateCatalogPage`'s
dangling span is the one that is wrong without a crash. Unscheduled.
