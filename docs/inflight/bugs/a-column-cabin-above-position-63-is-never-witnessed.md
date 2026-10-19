# A column Cabin above position 63 is served but never witnessed

**Wrong behaviour.** A column Cabin can be created at any position, but a
write witnesses only positions 1..63. A set banked for a Cabin at position 64
or above therefore misses every row written after it was banked, and two
readers serve that stale set as authoritative. `[quiet-wrong]`.

**Verified at** `789e77e1` (source identical to `f3ec25ea`, `v3.3.0`), on
`worktree-cd-column-slot-cleaning`, by source trace during CD-S0 premise P6
(`instructions/v3.2.0/workorder-cd-dropped-column-cleaning.md` §11). Two
independent skeptics re-derived it. Nothing was built or run, and no test
reproduces it yet.

## The trace

- `Catalog::CreateCabin` refuses position 0, a position past the schema and a
  dropped one, but has no `< 64` guard (`src/catalog/catalog.cpp:3278-3283`).
  Two user doors reach it at any position: `CREATE CABIN`
  (`src/exec/cabin_ddl.cpp:173-187`) and the `CABIN` column clause of
  `CREATE TABLE` (`src/server/command_dispatcher.cpp:4410-4417`). The
  controller's auto-create cannot pick a position >= 64: its filter-scan
  candidate is limited to `< 64` (`command_dispatcher.cpp:7955-7958`).
- `cabin_mask` takes a bit only below 64 (`catalog.cpp:2640-2643`), while
  `cabin_ids` holds every position.
- The one witness loop, which INSERT (`command_dispatcher.cpp:5987`) and
  UPDATE (`:9177`) both reach, runs `col = 1 .. 63` over the mask
  (`:7786-7787`). A §6 announce set fills through the same loop, so it does
  not cover the gap either.
- **Reader 1, SELECT.** `CabinProbeOf` and `CorrelatedCabinProbeOf` test only
  `cabin_mask != 0`, then read `CabinOn(col)` at any position
  (`src/exec/step_compiler.cpp:1254-1261`, `:1455-1463`).
- **Reader 2, the FK reverse check.** `CheckNoChildrenBeforeDelete` takes
  `child->CabinOn(fk.column_no)` and hands it to the check whenever its id is
  non-zero, with no mask test (`command_dispatcher.cpp:3903-3907`).
  `CheckNoChildReferences` serves from it, and an exhausted set clears the
  parent (`src/exec/fk_check.cpp:252-263`, AY-S6).

## Smallest reproduction (not yet run)

Child relation `C` with 70 columns: an fk column `c70` referencing `P`, a
column Cabin on `c70`, and a column Cabin on any of `c1..c63`.

1. `SELECT ... FROM C WHERE c70 = 7` banks the set for 7.
2. `INSERT INTO C (..., c70) VALUES (..., 7)` commits; it is not witnessed.
3. `SELECT ... FROM C WHERE c70 = 7` is served from the stale set and misses
   the new row.
4. `DELETE FROM P WHERE <tuple key> = 7` is served from the stale set, passes,
   and deletes the parent while a committed child still references it.

Step 4 also works after the low Cabin is dropped: the FK reverse check needs
no mask bit, so a set banked while a low Cabin existed is served after the
mask goes to 0 (`DROP CABIN` forgets only the dropped Cabin's own sets,
`command_dispatcher.cpp:2439`).

## Cost

A quiet wrong answer on two paths: a SELECT that misses committed rows, and a
parent DELETE that leaves an orphaned child durably on the volume, breaking
`docs/spec/foreign-keys.md`'s "an exhausted set clears the parent". Only an
operator-created Cabin reaches it.

The comment on `cabin_mask` (`include/kds/catalog/schema.hpp:356-362`), "an
unnoticed Cabin simply means the step compiles to the scan ... can never lose
a row", is false for both readers.

## Fix options (unruled)

- (a) Refuse `col_pos >= 64` in `Catalog::CreateCabin`, the one door every
  Cabin passes. Closes both readers.
- (b) Witness every position by driving the loop from `cabin_ids` rather than
  the mask (`command_dispatcher.cpp:7786`). Closes both readers.
- (c) Gate `CabinProbeOf` and `CorrelatedCabinProbeOf` on the column's bit.
  **This does not close the defect alone**: the FK site at
  `command_dispatcher.cpp:3903-3907` must take the same gate, or a set banked
  by any route is still served.

No work order carries it. CD (`workorder-cd-dropped-column-cleaning.md`) reuses
a slot at its own position and adds none, so it does not widen this.

## Adjacent defect on the same path

`RecordFkAccess(..., std::uint64_t{1} << fk.column_no)`
(`command_dispatcher.cpp:3916-3918`) shifts by the fk column's position. For
a position >= 64 that is undefined behaviour, reached by every parent DELETE
that runs the reverse check while access statistics are on, Cabin or not.
`Catalog::CreateForeignKey` has no `< 64` guard; the filter-scan arm guards its
own shift (`:7955-7958`). Not reproduced.
