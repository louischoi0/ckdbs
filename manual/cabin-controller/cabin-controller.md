# KDS Cabin Controller Manual

Operating the Cabin controller: what it observes, the config keys, and
the view; with the decay score it reads and the page epoch its hints
carry. Verified against `docs/spec/cabin-controller.md` (PO1-PO10),
`docs/spec/decay.md` (R1), `docs/spec/heap-and-tuple.md` §3.1a (R4) and
§7.1, `kds.conf.sample` and `include/kds/stats/`. The specs own every
decision cited here; this manual is the operator's view.

The Cabin controller is acceleration the engine decides for itself and can
drop without changing a result: it creates and drops Observational Cabins,
and a Cabin is authoritative for the values it observed, so whether one
exists changes which path answers, never the answer.

- **The Cabin controller** (complete, PHY01-PHY08, closed 2026-08-10 —
  measured in `bench/results-cabin-optimizer.md`): the `CABIN AUTO`
  promotion pipeline, gated by the `cabin_optimizer` config key,
  **default `off`** — so out of the box a column declared `CABIN AUTO`
  still behaves as an undeclared one. `SHOW CABIN_OPTIMIZER` is the view
  (§5a below).
- **What it rests on**: the heat score (R1, `decay.md`) and the page epoch
  (R4, `heap-and-tuple.md` §3.1a). **Relayout is gone since BZ
  (2026-10-17)**: the shadow planner, `SHOW RELAYOUT`, the `sys.relayout`
  view and the `physical_optimizer` config key are deleted, because they
  planned only for the chain-stored heap, which no longer exists. A
  `kds.conf` that still sets `physical_optimizer` is refused at startup by
  name — remove the line.

There is **no mover**. Nothing moves a tuple for statistics' sake or
reclaims a page. Cabin creation *can* be autonomous — only when
`cabin_optimizer = on`.

---

## 1. Configuration

| Key | Default | Meaning |
|---|---|---|
| `decay_half_life` | `600` | Seconds for an untouched heat score to lose half its weight. One instance-wide value; `0` is refused (instant decay is "no score"). |
| `access_statistics` | `on` | Per-shape access recording into `sys.access_stats` (`SHOW ACCESS`). With it off, no new shapes are recorded. The Cabin controller does not read it; its inputs are its own signal collector and the Cabin store's counters. |

The controller's key exists since PHY04: `cabin_optimizer = off|on` (default
`off`), with tuning keys (`cabin_optimizer_page_budget`,
`cabin_optimizer_theta_create_pct` / `_drop_pct` / `_swap_pct`,
`cabin_optimizer_amort_windows`, `cabin_optimizer_cooldown_half_lives`)
documented in `kds.conf.sample`. Two of those deserve their own
sentences, because they were one number until 2026-08-10 and are two
questions:

- **`amort_windows`** (default **64** half-lives) — how long a Cabin's
  build cost is amortized over, so it sets the admission bar. Ratified
  from the business-days scenario; at 1, the original proposal, the
  lifecycle is a nightly rebuild loop
  (`bench/results-cabin-optimizer-days.md` measured exactly that).
- **`cooldown_half_lives`** (default **128**) — how long a DECAYING
  Cabin is given to rebound before being dropped. This is what actually
  provides overnight survival: 128 half-lives is 21 h 20 m at the
  default, longer than a market's ~17.5 h close-to-open gap, so a Cabin
  crosses the night and the morning rebound recovers it.

**Do not lower the cooldown expecting dead Cabins to retire sooner.** A
dead Cabin and an overnight-quiet one both emit silence; the only thing
that tells them apart is waiting longer than the quiet period. Below
about one night's worth of half-lives the knob retires *live* Cabins and
the nightly rebuild loop returns. A 24/7 workload with no quiet period
is the case that can safely lower it — and, since the decoupling, the
case that finally can.

**When a Cabin leaves ACTIVE is proportional to what it proved** — since
2026-08-10, at any window. The heat score is read in two ways: linearly
for live ranking, and logarithmically wherever two *idle* things must be
ordered. The linear read bottoms out after about 16 half-lives of
silence, which used to cap the DECAYING onset at the score's own bit
width and made the eviction of cold statistics entries arbitrary; the
log read has no floor. What it does not change is the DROP at the end of
the cooldown, which is a timeout by necessity — silence is silence, and
no precision distinguishes a dead Cabin from a sleeping one.

## 2. `SHOW RELAYOUT` — removed

`SHOW RELAYOUT` and `sys.relayout` are deleted since BZ. `SHOW RELAYOUT` is
refused as an unknown `SHOW` target. `SHOW ACCESS` still reports the raw
access shapes it weighed.

## 3. The heat score (R1)

One decay implementation for the whole engine
(`include/kds/stats/decay.hpp`): stored state is `{score, last_bump}`, the
decayed value is `score · 2^(-(t-last_bump)/half_life)`, computed lazily at
touch and read — **no background pass**, so ten thousand cold scores cost
nothing until asked. Fixed-point (Q24.8), no floating point on any
statement path; exact at whole half-lives, ≤4.4% overestimate between.
Consumer: the Cabin controller's S1-S3 signals (§5).

## 4. Why nothing moves a tuple

No relayout mover exists; `docs/spec/heap-and-tuple.md` §7.1 states the
facts that stand in front of one (the reader horizon, the between-pages
key order a descent and range pruning read, and cross-relation page reuse,
answered only for a dropped relation's pages).

The page epoch (R4) is built: every page carries `relayout_epoch`, moved
only when a B+ tree leaf divides or is compacted in place - a row placed
mid-leaf shifts the slots after it without moving the epoch, and the
Keystone-id check at the slot catches the shift;
Waystone replay and Cabin hints record and compare it, and no consumer may
accept a location on epoch equality alone — the Keystone-id check stays
the identity test.

## 5. The Cabin controller (status)

The `CABIN AUTO` promotion pipeline: one background controller for the instance (since AT-S8) that
would CREATE/EXTEND/HEAL/DROP Observational Cabins under a pure
cost-benefit core with hysteresis. Built so far:

- **PHY01** — signal plumbing: S1/S2 per-fingerprint `{executions, pages}`
  decay pairs, S3 per-cabin counters, a versioned `Snapshot()`;
  `pages=` shows in `ANALYZE` output.
- **PHY02** — the decision core: `Decide(Snapshot) → ActionSet`, unsigned
  16.16 fixed point, deterministic (golden sets, hysteresis, budget
  invariant and bit-identical traces are tested).

- **PHY04** (2026-08-10) — the controller loop runs end to end over the
  EVT03/EVT06 substrate, gated by `cabin_optimizer` (default `off`).
- **PHY06** (2026-08-10) — observability, below.
- **PHY08** (2026-08-10) — the E2E close-out. Measured
  (`bench/results-cabin-optimizer.md`): with zero eligible candidates the
  controller is unmeasurable (the tick costs 2-3 µs CPU — sub-ppm of a
  core at the default 10 s cadence); on a hot 10-row equality over
  10,000 rows it created a Cabin autonomously 3 ticks after switch-on
  and served at **10.9×** the walk (1.97× at 1,000 rows, 1.17× at 200 —
  the familiar crossover shape).

Operationally: with the key at its default `off`, declare `CABIN`
explicitly when you want one — `CABIN AUTO` acts only under
`cabin_optimizer = on`. Bound Cabins (assertions') are permanently
outside the controller's jurisdiction.

### 5a. `SHOW CABIN_OPTIMIZER` — the controller's view

One header line, then one `\n`-escaped line per managed candidate:

```
cabin_optimizer=<on|off> managed=<n> pages_committed=<n> page_budget=<n>
  ticks=<n> creates=<n> extends=<n> heals=<n> drops=<n> deferred=<n> failures=<n>
rel=<name> column=<name> state=<CANDIDATE|BUILDING|ACTIVE|DECAYING>
  cabin_id=<n> pages=<n> streak=<n> benefit_q16=<n> cost_q16=<n>
  [hint_fail_pct=<n> coverage_miss_pct=<n>] last_action=<s> reason=<s> epoch=<n>
```

How to read it: the counters are **applied** actions — what the executor
did — while `last_action` is the newest logged **decision**, so a decided
CREATE beside `creates=0` means the effectful half deferred (busy row,
kill switch) or was refused by policy. `benefit_q16`/`cost_q16` are the
last Decide pass's scores in 16.16 fixed point, stamped every pass — a
quiet entry still shows the numbers keeping it quiet. The quality
percentages are the rates the θ_heal and θ_extend rules compare. A server
without the controller (`cabins = off`) answers
`CABIN_OPTIMIZER absent (cabins = off)` rather than a zero-filled table.

`ANALYZE` marks a served optimizer-owned probe with
`cabin_optimizer=true`, so a plan always says when the structure serving
it is one the engine may drop on its own judgement. `SET CABIN_OPTIMIZER
ON|OFF` is the runtime switch; reading the view never advances the
controller's snapshot sequence.

## 6. Operating notes

- `SHOW ACCESS` shows the access shapes the workload ran (`sys.access_stats`,
  keyed by columns, never values); the controller does not read it. `SHOW CABINS` lists every Cabin declared or
  created; `SHOW CABIN_OPTIMIZER` shows the ones the controller manages, with
  the scores and decisions behind them.
- Statistics rows are never removed, and a statistic outlives its
  relation — a vanished relation prints `rel=oid=<n>`.
- Every core records into the instance's one `sys.access_stats`, so on a
  multi-core instance `SHOW ACCESS` reflects every core's traffic.
- Nothing here can change a query result: the epoch pairing rule keeps
  every location hint verified, and the Cabin controller manages only
  Observational Cabins, and dropping one returns its queries to the tree
  walk.
