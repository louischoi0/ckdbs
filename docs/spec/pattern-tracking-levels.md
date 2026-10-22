# Pattern tracking levels

Status: a design of four levels, **not built**, and with no statement to
select one: `CREATE PATTERN`, which carried the `tracking =` option, is
withdrawn (`create-pattern-user-defined-patterns-v1.md`). **Two states are
built since CF-S1b, as the level field's two-state encoding**: bit 0 of
`SysPatternRow.flags`, `kPatternWaystone`, which `WAYSTONE ON` sets and
`WAYSTONE OFF` clears (`waystone-concpets.md` §4a). **0 is untracked** - the
shape records and replays nothing, and is never sighted - and **1 is level
3** - recording starts on the second execution of an instance, through
`TrailRecorder`'s one threshold, `kRecordThreshold = 2`. A level field
would widen this encoding in place, in `flags`, rather than sit beside it.
No retention exists to order, and levels 1, 2 and 4 have no encoding.
`origin` is a reserved byte, written 0 and read by nothing.

The design: a *tracking level* per pattern decides how hard the engine
works to keep its instances' trails, replacing the `pinned` boolean the
bit was re-scoped from and giving retention its eviction order — level 4
first, then 3, then 2, within a level by the lazy-decay score (`decay.md` R1); level 1 is
never a candidate. Invariant 9 holds at every level: levels change how much
performance the cache promises, never what a query returns.

| # | Level | Records | Eviction | Memory |
|---|-------|---------|----------|--------|
| 1 | Full coverage (`full`) | every instance, first execution | exempt | reserved at CREATE |
| 2 | Best effort (`best_effort`) | first execution | last in line | shared pool, protected |
| 3 | Less effort (`relaxed`) | second execution (n=2, `parser-v2.md` J5) | normal ranking | shared pool |
| 4 | Memory-dependent (`opportunistic`) | only above a free-frame watermark | first victim | surplus only |

Level 1 is a performance SLO, not a correctness guarantee, and would need:
a mandatory `expected_instances` with pages reserved and the directory
pre-sized; overflow degrading to best effort with a health event, never
silently; and collision chaining, which the store does not do (a collision
displaces, `waystone-concpets.md` §5). Level 4 is a scavenger for
long-tail patterns: nothing reserved, recorded only while the buffer pool
is above the watermark, reclaimed first under pressure.
