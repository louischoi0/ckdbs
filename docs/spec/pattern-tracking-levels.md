# Pattern tracking levels

Status: DECIDED (4 levels) — option identifiers `[PROPOSED]`. **Nothing
here is built**, and one of its four levels lost the statement that
selected it.

**Amended 2026-08-31**: the operator withdrew user-declared patterns
(`create-pattern-user-defined-patterns-v1.md`, marked withdrawn), so
there is no `CREATE PATTERN` to carry a `tracking =` option and **no way
to select a level at all**. Every pattern is level 3 by §3's own words —
recording starts on the second execution — which is what `TrailRecorder`
does today with one threshold and no level field. Read the rest as the
proposal it always was: the four levels stand as a design, and reviving
them needs a statement to select them, which the pattern re-design would
have to supply.

Every pattern carries a *tracking level* deciding how hard the engine
works to keep its instances' trails. The level replaces the earlier
`pinned` boolean (`SysPatternRow.tracking_level u8` — a field that was
never added: the row still carries `flags`/`kPatternPinned` and `origin`,
both of which lost their only writer on 2026-08-31), and gives retention
its eviction order for free: victims are taken from level 4
first, then 3, then 2 — within a level by decay score. Level 1 is never
a candidate.

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
  the directory is pre-sized (no growth flush). It was a `CREATE PATTERN`
  option, so since the withdrawal this level's contract has no way to be
  stated at all.
- Exceeding the reservation degrades the overflow to best-effort **and
  raises a health event** (visible in `SHOW PATTERNS`) — never a silent
  downgrade.
- Directory collisions may not silently overwrite: level 1 requires
  collision chaining, which forces the `[OPEN]` collision policy to be
  decided before this level ships.

## 2. Best effort (`tracking = best_effort`)

Was the default for `CREATE PATTERN`, and is therefore the level with no
selector at all since that statement was withdrawn. Trails record from
the first execution (the declaration was the evidence n=2 would otherwise
wait for — a reason that goes with the declaration, so a revival owes
this level a new one). Trails live in the shared waystone budget and are
evicted only after every level-3 and level-4 trail is gone.

## 3. Less effort (`tracking = relaxed`)

Default for auto-registered patterns and, since 2026-08-31, the level
every pattern is at — exactly today's decided behavior: recording starts
on the second execution (J5), trails compete in the shared budget under
the lazy-decay score (`docs/spec/physical-optimizer.md` R1).

## 4. Depend on system memory (`tracking = opportunistic`)

Pure scavenger. Trails are recorded only while the core's buffer pool
sits above a free-frame watermark, nothing is reserved, and these pages
are the first reclaimed under pressure. Core-local decision (S7
per-core pools) — no cross-core coordination. The right level for
long-tail patterns that are nice to warm but never worth paying for.
