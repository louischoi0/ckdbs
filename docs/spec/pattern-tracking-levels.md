# Pattern tracking levels

Status: a design of four levels, **not built**, and with no statement to
select one: `CREATE PATTERN`, which carried the `tracking =` option, is
withdrawn (`create-pattern-user-defined-patterns-v1.md`). Every pattern
behaves as level 3 — recording starts on the second execution — through
`TrailRecorder`'s one threshold; there is no level field, and no retention
exists to order. `SysPatternRow` carries `flags`/`kPatternPinned` and
`origin`, neither of which anything writes.

The design: a *tracking level* per pattern decides how hard the engine
works to keep its instances' trails, replacing a `pinned` boolean and
giving retention its eviction order — level 4 first, then 3, then 2, within
a level by the lazy-decay score (`physical-optimizer.md` R1); level 1 is
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
