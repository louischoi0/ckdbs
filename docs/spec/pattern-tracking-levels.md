# Pattern tracking levels

Status: a design of four levels, **not built**, and with no statement to
select one: `CREATE PATTERN`, which carried the `tracking =` option, is
withdrawn (`create-pattern-user-defined-patterns-v1.md`). Every pattern
behaves as level 3 — recording starts on the second execution — through
`TrailRecorder`'s one threshold; there is no level field.
`SysPatternRow` carries `flags`/`kPatternPinned` and `origin`, neither of
which anything writes.

The design: every pattern carries a *tracking level* deciding how hard the
engine works to keep its instances' trails. The level replaces a `pinned`
boolean and gives retention its eviction order: victims are taken from
level 4 first, then 3, then 2 — within a level by decay score. Level 1 is
never a candidate.

Invariant 9 holds at every level: a trail is advisory, never
authoritative. Levels change how much performance the cache promises,
never what a query returns.

| # | Level | Records | Eviction | Memory |
|---|-------|---------|----------|--------|
| 1 | Full coverage | every instance, first execution | exempt | reserved at CREATE |
| 2 | Best effort | first execution | last in line | shared pool, protected |
| 3 | Less effort | second execution (n=2) | normal ranking | shared pool |
| 4 | Memory-dependent | only above free-frame watermark | first victim | surplus only |

## 1. Full coverage (`tracking = full`)

Every argument instance is trailed from its first execution and the
system never *chooses* to drop one. This is a performance SLO, not a
correctness guarantee. It costs a contract:

- `expected_instances` is **mandatory**; pages are reserved for it and
  the directory is pre-sized (no growth flush).
- Exceeding the reservation degrades the overflow to best-effort **and
  raises a health event** (visible in `SHOW PATTERNS`) — never a silent
  downgrade.
- Directory collisions may not silently overwrite: level 1 requires
  collision chaining, which the store does not do (a collision displaces,
  `waystone-concpets.md` §5).

## 2. Best effort (`tracking = best_effort`)

Trails record from the first execution — the declaration was the evidence
n=2 would otherwise wait for. Trails live in the shared waystone budget
and are evicted only after every level-3 and level-4 trail is gone.

## 3. Less effort (`tracking = relaxed`)

The level every pattern is at: recording starts on the second execution
(`docs/spec/parser-v2.md` J5), trails compete in the shared budget under
the lazy-decay score (`docs/spec/physical-optimizer.md` R1).

## 4. Depend on system memory (`tracking = opportunistic`)

Pure scavenger. Trails are recorded only while the core's buffer pool
sits above a free-frame watermark, nothing is reserved, and these pages
are the first reclaimed under pressure. A core-local decision — no
cross-core coordination. The right level for long-tail patterns that are
nice to warm but never worth paying for.
