# Lazy-decay score

The engine's one time-weighting: a score that halves over a configured
half-life, computed lazily from two stored words. Decision R1 is this
file's; it was `physical-optimizer.md` R1 and §3 (last at `418670a3`) until that file was split
(2026-10-22), and keeps its number so that citations of "R1, the lazy-decay
score" resolve here. `parser-v2.md` I15 also names an "R1" (the
no-fetch-under-span rule): cite the file with the number.

Markers: `[CONFIRMED]` is settled; `[PROPOSED]` marks a default that
shipped as written and is amendable by measurement; `[OPEN]` must not be
assumed.

Related documents: `cabin-controller.md` (the consumer, PO2's S1-S3),
`eviction.md` EV1 (the temperature variant), `pattern-tracking-levels.md`
(the retention order).

---

## 1. Decision Record

| ID | Decision |
|----|----------|
| R1 | **The lazy-decay score.** Exponential half-life decay computed lazily from a stored `{score, last_bump}` pair: a touch decays-then-increments, a read decays only, and there is no background decay pass — idle data costs nothing and is never visited. One implementation (`include/kds/stats/decay.hpp`), `sched::Clock`-injected; with no clock the score degrades to a raw count, the same best-effort stance `sys.access_stats.last_seen` already takes. Half-life is the `decay_half_life` config key, per instance, default 600 s `[PROPOSED]`. Scores are memory-resident and never persisted `[PROPOSED]`. **Two reads:** the Q24.8 `ValueAt` — which underflows to zero after ~16 half-lives, so it ranks live data only — and `Log2ValueAt`, the same state read in the log domain, where decay is a subtraction and ordering survives indefinitely. Any consumer that must order *idle* things (an eviction victim, a retirement) uses the log read (`cabin-controller.md` §II.4). |

---

## 2. The score

The stored state is two words: `score` and `last_bump` (a `sched::Clock`
reading). The decayed value at time *t* is
`score · 2^(-(t - last_bump) / half_life)`. A touch computes the decayed
value, adds 1, and stores `{decayed + 1, t}`; a read computes and does not
store. "Lazy" is the design, not an optimization: there is no sweep, so a
structure holding ten thousand cold scores pays nothing for their coldness
until something asks.

Rules:

- **One implementation.** `include/kds/stats/decay.hpp`, pure functions over
  the pair, `Clock`-injected like every time consumer (`sched/clock.hpp`'s
  contract). A second decay formula anywhere is the same defect as a second
  literal-coercion path (`types.md` §3.1).
- **No clock, no decay**: the score degrades to a raw counter. Deterministic
  tests inject `ManualClock` and get exact halving.
- Fixed-point arithmetic `[PROPOSED]`: u32 score scaled by 256, shift/mask
  only, no floating point on any statement path (`rules.md`). Exact at
  whole half-lives; between them the fractional factor is bucketed, so a
  read may overestimate by at most 2^(1/16) - 1, about 4.4%.
- The half-life is one instance-wide config key. Per-consumer half-lives do
  not exist; one would be a new key, not a parameter that silently forks the
  meaning of "hot".

---

## 3. Consumers

- **The Cabin controller** is the one caller: its signal collector
  (`stats/optimizer_signals.hpp`) keeps S1's execution count, S2's page
  sum and S3's lookup, hint-failure and coverage-miss counts as R1 pairs
  (`cabin-controller.md` PO2), and evicts its coldest fingerprint by the
  log read.
- `eviction.md` EV1 names R1 as the score a temperature variant of the
  CLOCK sweep would read; no such variant is built, and the sweep calls
  nothing here.
- `pattern-tracking-levels.md` names R1 as the order within a tracking
  level for trail retention; that design is not built, and no retention
  exists to order.

---

## 4. Config

| Key | Values | Default | Meaning |
|---|---|---|---|
| `decay_half_life` | whole seconds, 1 .. `UINT64_MAX / 10^9` | 600 `[PROPOSED]` | R1's half-life, instance-wide. `0` is refused: instant decay is "no score" (`src/server/expeditor.cpp`) |

---

## 5. Required tests

- Decay unit tests under `ManualClock`: exact halving, touch-vs-read,
  no-clock degradation to a raw count (`tests/decay_test.cpp`).

---

## 6. Out of scope

Not in this engine: score persistence, per-consumer half-lives, and
temperature-unified eviction (`eviction.md` §7).
