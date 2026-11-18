# When statistics are stale, and what gathers them again

**Status: PENDING.** Raised 2026-11-18. `[source-read]` claims read on
worktree `cx-plan-change-option` at `d4bda61f`. Related:
`instructions/v3.2.0/workorder-cy-managed-statistics.md` (CY, which leaves
this out), `instructions/v3.2.0/workorder-cx-managed-plans.md` (CX),
`docs/spec/plan-stats.md`, `docs/spec/sys-views.md` §6.2.

## What prompted it

`[operator]` CLA listed five gaps in the engine's statistics against
commercial systems. The operator took four into a work order and deferred
the fourth: *"leaving item 4 out for now ... Item 4 needs more thought"*
(`instructions/v3.2.0/raft-marks-2026-11-18.md` §5). Item 4 was: the engine
has no rule for when a statistic should be gathered again. Commercial
systems decide from a count of modified rows - SQL Server's auto-update
threshold, PostgreSQL's autovacuum analyze threshold, Oracle's 10 % rule.

## The tree as read

- `[source-read]` The only thing that notices data moving is the steward:
  it re-seeds a relation's row prior at most every 10 s and publishes when
  the prior's half-octave band moves (`docs/spec/plan-stats.md` PS4, PS5).
  It reads row counts only. A column's distribution can change completely
  under a constant row count and nothing sees it.
- `[source-read]` The write path feeds the planner nothing: "no
  commit-time deltas, no per-row counters and no change on the write path"
  (`instructions/v3.2.0/workorder-cc-plan-statistics.md:280`).
- `[source-read]` A per-relation count of written rows does exist, for
  observability: `sys.relation_stats`' `inserts`, `updates`, `deletes` and
  `purged`, per core, memory-resident, counted when a statement answers and
  not when it decides, and bounded at 4,096 relations a core
  (`docs/spec/sys-views.md` §6.2). It starts from zero at every restart
  and has no notion of "since the last gather".
- `[design]` CY makes the model change only by an explicit act (CY-R3),
  so until this is ruled on a gathered statistic is as old as its last
  `GATHER`, and `sys.statistics` shows its age and nothing else. **CY's
  one automatic write is the steward saving a relation's row prior when
  its band moves (CY-Q2 (a))** - option (d) below, applied to the
  relation object. Whether that save may ever replace a gathered row
  count is CY-Q16, and a ruling here should cover it.

## Why it is not a detail

- `[design]` **What a stale statistic costs depends on `join_order`**
  (CX). Under `fixed` a captured plan does not move, so staleness reaches
  only shapes compiled for the first time and `ADVISE PLAN`. Under
  `planned` every compile reads the model, so a stale column statistic
  misprices every plan over it - and an automatic re-gather moves plans,
  which is what the operator chose there.
- `[design]` **An automatic re-gather is engine-initiated work on the
  operator's data path.** It walks relations, competes with OLTP for the
  buffer pool, and writes the log. CA's W1 rule (a background task yields
  to OLTP) bounds how, not whether.
- `[design]` **A counter on the write path is a cost every write pays.**
  CC's measured claim that the paths that do not plan did not move rests
  on there being none.

## The options

| | what decides "stale" | what happens then | cost |
|---|---|---|---|
| (a) | nothing; age only | `sys.statistics` shows each object's age; a DBA or an agent decides | none. The state CY leaves |
| (b) | the existing `sys.relation_stats` write counts, snapshotted at each `GATHER` | a `stale` flag and the modified fraction in `sys.statistics`; **advice only**, no re-gather | no new write-path cost; the counts reset at restart, so the flag is unknown after one unless the snapshot is persisted |
| (c) | (b)'s fraction past a threshold (10 %, or a setting) | a background `GATHER`, yielding, off by default | engine-initiated walks and log writes; under `planned` it moves plans |
| (d) | the row prior's band against the band at gather time | flag or re-gather as in (b) or (c) | free (the steward already has both); blind to distribution change at constant size, updates above all |
| (e) | a cheap sampled check of a column's distribution against its stored one, on the steward's budget | flag or re-gather | page reads on core 0's tick; the only option that sees a distribution move, and the least understood |

## What a ruling has to say

1. Whether the engine ever gathers on its own, under which `join_order`
   values, and whether that is on or off by default.
2. Whether "stale" is a flag an agent reads or a trigger the engine acts
   on.
3. Whether a persisted modification count is worth a write-path cost, or
   the observability counts are enough.
4. Whether a locked statistic is exempt (CY-S4 says a lock stops every
   replacement).
