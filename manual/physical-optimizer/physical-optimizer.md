# KDS Physical Optimizer Manual

Operating the physical optimizer: what it observes, how to read
`SHOW RELAYOUT`, the config keys, and why v1 enacts nothing. Verified
against `docs/spec/physical-optimizer.md`, `HandleShowRelayout`
(`src/server/command_dispatcher.cpp`), `kds.conf.sample` and
`include/kds/stats/` as of 2026-08-10. The spec owns every decision cited
here (R1-R12, PO1-PO10); this manual is the operator's view.

Engine-driven physical optimization is one of the two things KDS exists
for. The umbrella has two parts:

- **Part I — relayout** (built, PX01-PX08): observe access statistics,
  classify relations, plan physical moves, report. **Shadow-only, as a
  finding, not a hedge**: every candidate move is blocked by a named gate,
  and the report exists to price opening them.
- **Part II — the Cabin controller** (complete, PHY01-PHY08, closed
  2026-08-10 — measured in `bench/results-cabin-optimizer.md`): the
  `CABIN AUTO` promotion pipeline, gated by the `cabin_optimizer` config
  key, **default `off`** — so out of the box a column declared
  `CABIN AUTO` still behaves as an undeclared one.
  `SHOW CABIN_OPTIMIZER` is the view (§5a below).

There is **no mover** (Part I). Nothing moves a tuple or reclaims a page;
relayout observes, plans and reports. Cabin creation *can* now be
autonomous — only when `cabin_optimizer = on`.

---

## 1. Configuration

| Key | Default | Meaning |
|---|---|---|
| `physical_optimizer` | `shadow` | `off` or `shadow`. Shadow is pull-only — the planner runs when `SHOW RELAYOUT` asks and never in the background — so it costs **zero at idle** (measured at exact noise, `bench/results-physical-optimizer-shadow.md`). `off` makes `SHOW RELAYOUT` answer `RELAYOUT off (physical_optimizer=off)`. **`on` is refused at startup naming the three gates** that block every plan, so a config written for the future fails loudly today. |
| `decay_half_life` | `600` | Seconds for an untouched heat score to lose half its weight. One instance-wide value; `0` is refused (instant decay is "no score"). |
| `access_statistics` | `on` | The input feed: per-shape access recording into `sys.access_stats`. With it off, no new shapes are recorded and the report goes stale rather than wrong. |

Part II's key exists since PHY04: `cabin_optimizer = off|on` (default
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

## 2. `SHOW RELAYOUT` — the shadow report

```
SHOW RELAYOUT              # all relations; reads sys.access_stats + catalog only, never walks
SHOW RELAYOUT <table>      # one relation; additionally runs a read-only page census
```

The per-relation form's census walk is priced through the ordinary
statement budget (`max_rows_touched`) — a spent budget refuses the survey
rather than serving a half-count. The all-relations form takes no
`PageStore` at all: "never walks a relation" is enforced by signature.
Report cost is ~60 µs + 24 ns per slot surveyed (measured).

Reply shape (verified in the handler; one wire line, `\n`-escaped
sections):

```
relayout_relations=<n>
rel=<name> clustered=<btree|heap> shapes=<n> walk_weight_q8=<n>
shape kind=<Lookup|Probe|Range|CabinProbe|IndexProbe|IndexRange|FilterScan|Scan> columns_mask=0x<hex> uses=<n> weight_q8=<n>
survey pages=<n> live=<n> delete_marked=<n> tuples_per_page=<n>      (per-relation form only)
plan=<compact|cluster|defrag> blocked_on=<gate> surveyed=<0|1> predicted_pages_saved=<n> predicted_benefit=<n> measured_pages_saved=<n>
plans=none reason=<btree-outside-v1-mover-scope|catalog-relation-outside-mover-jurisdiction>
```

How to read it:

- **`shape` lines** are what the workload actually ran, per
  `(access kind, columns)` — raw `use_count` plus the decayed weight
  (Q24.8 fixed point, so `weight_q8=256` is a weight of 1.0). Walk-class
  weight concentrated on one relation is the signal a mover would act on.
- **`survey`** is the census: chain length, live vs delete-marked tuples,
  fill. Delete-marks are counted without MVCC — an upper bound on what
  compaction could reclaim, which is exactly what gate 1 needs priced.
- **`plan` lines** are candidate moves, and in v1 every one carries
  `blocked_on=<gate>` — see §4. `predicted_*` is the planner's estimate
  (R9); `measured_pages_saved` ships unpopulated and fills in only when a
  mover exists, so promotion comparisons will need no format change.
- **`plans=none reason=...`** is *jurisdiction*, not a clean bill of
  health: a btree relation has no v1 mover candidate (R5 — its mover would
  maintain nothing but the epoch anyway), and a catalog relation is outside
  the mover's jurisdiction **permanently** (§4's must-not list). The
  var-heap is exempt by construction (invariant 14).

## 3. The heat score (R1)

One decay implementation for the whole engine
(`include/kds/stats/decay.hpp`): stored state is `{score, last_bump}`, the
decayed value is `score · 2^(-(t-last_bump)/half_life)`, computed lazily at
touch and read — **no background pass**, so ten thousand cold scores cost
nothing until asked. Fixed-point (Q24.8), no floating point on any
statement path; exact at whole half-lives, ≤4.4% overestimate between.
Consumers: the relayout planner's shape weights (Part I) and the Cabin
controller's S1/S2 signals (Part II) — same score, their own names.

## 4. Why nothing is enacted — the three gates

Every v1 plan kind is blocked by a named gate, each owned by an open
decision elsewhere (`docs/spec/physical-optimizer.md` §6):

| Plan | Would do | Blocked on |
|---|---|---|
| `compact` | drop delete-marked tuples past the reader horizon, reclaim pages | **Gate 1 — reader horizon**: readers are deliberately unregistered (`docs/spec/txn.md` §9); a mover that guesses a horizon is partial recovery in different clothes |
| `cluster` | co-locate a hot set on fewer pages | **Gate 2 — ordered-between**: it would break the between-pages ordering `kRange` tail pruning reads; the legal form is `[OPEN]`, to be chosen from shadow data |
| `defrag` | rewrite a chain onto contiguous page ids | **Gate 3 — cross-relation page reuse**: a reallocated page can hold a colliding per-relation Keystone id at a recorded slot, and `PAGE_INIT` resets the epoch, so trail validation would pass wrongly |

The report is the deliverable: it turns "should a gate be opened" from
taste into a number per relation on a live workload. First real-workload
finding (`bench/results-physical-optimizer-shadow.md`): the hot walk
shapes sit on **btree** relations while the mover-eligible heap relations
are write-only — pointing at R5's btree scoping before any gate.

The page epoch (R4) is built: every page carries `relayout_epoch`
(bumped only by a mover, never by DML), Waystone replay and Cabin hints
record and compare it, and no consumer may accept a location on epoch
equality alone — the Keystone-id check stays the identity test.

## 5. Part II — the Cabin controller (status)

The `CABIN AUTO` promotion pipeline: a per-core background controller that
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

- `SHOW ACCESS` is the raw input (`sys.access_stats`, shapes keyed by
  columns, never values); `SHOW RELAYOUT` is the same data weighted,
  surveyed and classified. `SHOW CABINS` lists every Cabin declared or
  created; `SHOW CABIN_OPTIMIZER` shows the ones Part II manages, with
  the scores and decisions behind them.
- Statistics rows are never removed, and a statistic outlives its
  relation — a vanished relation prints `rel=oid=<n>`.
- Peers (`cores > 1`) run with `access_statistics` off by design, so on a
  multi-core instance the report reflects core 0's traffic — which today
  is all traffic.
- Nothing here can change a query result: the planner is read-only, the
  epoch pairing rule keeps every location hint verified, and shadow mode's
  only write is the report string.
