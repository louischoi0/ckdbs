# Work order ER - scenario 2's customer statement: a grouped join over a walked heap at six times PostgreSQL's server CPU, explained before it is cut

Status: **WRITTEN 2026-11-28, not opened** (`raft-marks-2026-11-28.md`
§25), on the operator's word *"write work milestones for the three causes
behind the current performance gap, and push"*; the second of the three EO
names. Written by CLA in a claude.ai session in the main checkout on
`main` at `c43dbf46` (`v3.4.17-332-gc43dbf46`), documents only, no
worktree. **Every `[source-read]` claim with a `path:line` was read by CLA
at `c43dbf46`**; nothing was built or run.

Tags: `[operator]`, `[source-read]`, `[measured]` (quoted), `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `bench/v3.5.1/results-eo-close-v3.5.1-63-g9543cf720.md` (rows at
lines 116, 170-171 and 1255; "What the numbers say", item 4);
`docs/spec/join-inner-build.md`; `docs/spec/parser-v2.md` I12, I19;
`docs/spec/aggregate.md`; `instructions/v3.2.0/workorder-cx-managed-plans.md`.

## 0. What ER is

`[measured]` EO, scenario 2, one booker: `customer-statement` is
**881.1 µs against 147.5 µs (0.17x)**; KDS's breakdown is compile 3.9 µs,
**execution 826.2 µs**, no disk read, no lock wait; PostgreSQL's execution
136.7 µs. The plan is chosen fast; the time is in running it.

`[source-read]` The statement (`tools/eqbench/src/scenarios/s2_freight.rs:1034-1035`):

```sql
SELECT s2_cargos.org_id, SUM(s2_freights.cbm)
FROM s2_freights JOIN s2_cargos ON s2_freights.cargo_id = s2_cargos.id
WHERE s2_cargos.org_id = ? GROUP BY s2_cargos.org_id
```

`s2_freights` is a HEAP walked by this statement and indexed only on
`operation_id` (`:30`, `:724`); `s2_cargos` is a BTREE keyed by `id`
(`:27`), 4,000 cargos by default (`:560`), with no index on `org_id`. The
filter is on the inner side, the grouping key is the filter's own column,
and nothing indexes the join column on the heap.

`[design]` Where 826 µs can go, none of it verified:

- **H1 the written order**: `s2_freights` outer, every freight row probing
  `s2_cargos` by key and discarding the rows of other customers - one
  descent per freight row, with `join_order = fixed` keeping whatever order
  was captured.
- **H2 the query layer**: a grouped join may be declined by the native
  compile and answered by the query layer (`parser-v2.md` I19), which walks
  each relation natively and joins and folds above them, row by row.
- **H3 no build**: the statement-local inner build (`join-inner-build.md`)
  is taken only by the walked-join shape; a grouped join may not carry the
  annotation, so neither side is bucketed once.
- **H4 the fold**: EH's native fold takes only a global fold over one
  relation (`aggregate.md` §1), so this fold reads every joined row
  through the frame.

PostgreSQL's plan for the same shape is a hash join with the filtered
`cargos` built (about 1/20 of 4,000 rows) and `freights` scanned once.

## 1. Rules

- **ER-R1** `[quiet-wrong]` Any new arm or order answers what the written
  order answers, NULLs and duplicates included, and is pinned by the
  inner-build and index contract suites' byte-for-byte comparison.
- **ER-R2** "The DB does not change a plan freely" holds: a new order or
  arm reaches a statement only through the plan sources `CX` defines
  (captured, administrator's, searched under `planned`), never by
  bypassing `join_order = fixed`.
- **ER-R3** Read is weighted 45 (`CLAUDE.md`'s CRUD weights): a remedy
  that moves cost onto writes states the move.

## 2. Items for the operator (ER-Q), CLA's proposals

| item | question | proposal |
|---|---|---|
| ER-Q0 | Open after EP | **(a)** yes; it can run beside EQ |
| ER-Q1 | The diagnosis | **(a)** `ANALYZE` of the statement on KDS at `eqbench`'s scale, with the arm, order, build and fold each relation took, and `ADVISE PLAN` beside it |
| ER-Q2 | If H1 or H3 | **(a)** the grouped join takes the inner build on the filtered side (the build keyed by `cargos.id`, the heap walked once), reachable by a captured or administrator's plan; (b) the searched order under `planned` only |
| ER-Q3 | If H2 | **(a)** the native compile admits a grouped inner equi-join whose grouping keys are one relation's columns, with the fold above the join; scope written at S0 |
| ER-Q4 | If H4 alone | **(a)** recorded, no cut: EH's bytes fold over a join is a separate order |

## 3. Stages

- **ER-S0 diagnosis**: ER-Q1; which of H1 to H4 holds, each with its
  server CPU share; the same statement at 200 and 20,000 cargos to see
  which term scales.
- **ER-S1 the remedy** for what S0 shows (ER-Q2, ER-Q3), with contract
  cells.
- **ER-S2 the close**: scenario 2 on `eqbench`, OLD against NEW
  interleaved, PostgreSQL in the same window; `manifest-scan` and
  `voyage-rollup` as controls; the standard cells inside their floor.

## 4. What ER does not do

- No index added to the scenario to make the cell pass: the scenario's
  schema is EO's.
- No general cost-based rewrite beyond the shape S0 names.
- No change to `join_order`'s default.

## 5. Stage log (CLA, appended as stages land)

### Written 2026-11-28

In the main checkout on `main` at `c43dbf46`, documents only. Not opened;
no build, no run.
