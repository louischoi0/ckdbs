# KDS Physical Optimizer Manual

Operating the physical optimizer: what it observes, how to read
`SHOW RELAYOUT`, the config keys, and why v1 enacts nothing. Verified
against `docs/feat-physical-optimizer.md`, `HandleShowRelayout`
(`src/server/command_dispatcher.cpp`), `kds.conf.sample` and
`include/kds/stats/` as of 2026-08-10. The spec owns every decision cited
here (R1-R12, PO1-PO10); this manual is the operator's view.

Engine-driven physical optimization is one of the two things KDS exists
for. The umbrella has two parts:

- **Part I — relayout** (built, PX01-PX08): observe access statistics,
  classify relations, plan physical moves, report. **Shadow-only, as a
  finding, not a hedge**: every candidate move is blocked by a named gate,
  and the report exists to price opening them.
- **Part II — the Cabin controller** (PHY01-PHY02 built): the `CABIN AUTO`
  promotion pipeline. The signal plumbing and the pure decision core
  exist; **no controller runs yet**, so a column declared `CABIN AUTO`
  behaves exactly as an undeclared one.

There is **no mover**. Nothing moves a tuple, reclaims a page, or creates
a Cabin on its own today. The optimizer observes, plans and reports.

---

## 1. Configuration

| Key | Default | Meaning |
|---|---|---|
| `physical_optimizer` | `shadow` | `off` or `shadow`. Shadow is pull-only — the planner runs when `SHOW RELAYOUT` asks and never in the background — so it costs **zero at idle** (measured at exact noise, `bench/results-physical-optimizer-shadow.md`). `off` makes `SHOW RELAYOUT` answer `RELAYOUT off (physical_optimizer=off)`. **`on` is refused at startup naming the three gates** that block every plan, so a config written for the future fails loudly today. |
| `decay_half_life` | `600` | Seconds for an untouched heat score to lose half its weight. One instance-wide value; `0` is refused (instant decay is "no score"). |
| `access_statistics` | `on` | The input feed: per-shape access recording into `sys.access_stats`. With it off, no new shapes are recorded and the report goes stale rather than wrong. |

The spec names a `kds.cabin_optimizer` key for Part II; it is **not yet a
config key** — `kds.conf.sample` does not carry it, and nothing would
consume it.

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
decision elsewhere (`docs/feat-physical-optimizer.md` §6):

| Plan | Would do | Blocked on |
|---|---|---|
| `compact` | drop delete-marked tuples past the reader horizon, reclaim pages | **Gate 1 — reader horizon**: readers are deliberately unregistered (`docs/txn.md` §9); a mover that guesses a horizon is partial recovery in different clothes |
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

Not built: the controller loop that would call `Decide` (PHY04,
hard-blocked on the eviction workplan's EVT06 scan ring) and everything
after it. Operationally that means: declare `CABIN` explicitly when you
want one now; `CABIN AUTO` is a recorded intention. Bound Cabins
(assertions') are permanently outside the controller's jurisdiction.

## 6. Operating notes

- `SHOW ACCESS` is the raw input (`sys.access_stats`, shapes keyed by
  columns, never values); `SHOW RELAYOUT` is the same data weighted,
  surveyed and classified. `SHOW CABINS` shows what Part II would manage.
- Statistics rows are never removed, and a statistic outlives its
  relation — a vanished relation prints `rel=oid=<n>`.
- Peers (`cores > 1`) run with `access_statistics` off by design, so on a
  multi-core instance the report reflects core 0's traffic — which today
  is all traffic.
- Nothing here can change a query result: the planner is read-only, the
  epoch pairing rule keeps every location hint verified, and shadow mode's
  only write is the report string.
