# A catalog row placed on a chain's tail is not reported when its logging fails

**Found by reading, not reproduced.** Verified at `68d8b18` on
`ay-s7-allocate-catalog-page`, by AY-S7's review.

## What is wrong

`Catalog::InsertRow` (`src/catalog/catalog.cpp`) has two arms. The tail arm
places the row on a page that has room. The new-page arm places it on a page
`AllocateCatalogPage` just created. Both arms then fire the DDL undo hook and
append the row's `HEAP_INSERT`.

The two arms set `*where`, the row's `(page, slot)`, at different points:

- **The new-page arm** sets `*where` right after placement, before the hook.
- **The tail arm** sets it only after `LogCatInsert` succeeds.

The callers rely on `*where` on failure:

- `CreateTable` (`catalog.hpp`, `written`) says a failed create still reports
  "rows already on the page".
- `command_dispatcher.cpp`'s `CREATE INDEX` registers `created_row` "before
  the status is read: a create that failed after the catalog row went down
  still left it there".

So when the tail arm's hook or its log append fails, the row is on the page
and `*where` still reads `kInvalidPageId`. The DDL's rollback never retires
the row.

## Reproduction

A catalog-level cell: a DDL undo hook that returns an error on its first
event, while the target relation's tail still has room. The create fails.
`written` (or `created_row`) is empty, and the row stays on the page.

## What it costs

After the failed DDL rolls back, the catalog row is still live. Catalog reads
hide aborted work by compensation, not by visibility (`ddl-transactional.md`
§2), so the row is visible, which is a **quiet wrong answer**. It happens only
when an undo append or a WAL append fails, for example `OutOfSpace`.

## The fix

The fix is known and unscheduled. Set `*where` at placement in both arms: one
place-then-hook-then-log helper inside `InsertRow`, which removes the
duplicated sequence as well. It needs a cell as described above.
