# Cabin — value-observed authoritative metadata store (spec, v1)

Status: **DECIDED** — C1–C7 fixed; items marked `[PROPOSED]` / `[OPEN]`
are not.
Depends on: **`docs/rules/keystoneid-invariant.md` (K1–K5, adopted)**,
Keystone pk contract (super column), B+tree pk facade (B1–B3), patterns +
fingerprint (P-series), waystone trail model, core-ownership dispatch
(D3), thread-per-core cooperative scheduling, MVCC undo-chain model.
Related docs: `waystone-concpets.md`, `pattern-tracking-levels.md`,
`physical-optimizer.md` Part II (the cabin optimizer), `assertion.md`
(the Bound class, §12).

Decisions fixed by this spec:

- **C1** — Cabin is authoritative **only for observed values**.
- **C2** — entries store **Keystone ids (pks), never tuple locations**.
- **C3** — expression (filtered-index style) cabins are **out of v1**.
- **C4** — Cabin, Waystone, and patterns are distinct structures that
  **interlock cooperatively**; none absorbs another.
- **C5** — observation may extend to **every value of the column**;
  full coverage is permitted, at which point a Cabin converges to a
  lazily built secondary index.
- **C6** — entries carry a **location hint** (page, epoch, slot)
  alongside the pk: advisory, reader-verified under the same rules as
  waystone entries, falling back to pk descent and healed in place on
  failure. Entry size 24 B.
- **C7** — a column declares at `CREATE TABLE` **who may decide** it
  carries a Cabin: `NO CABIN` (never, by any route), `CABIN AUTO` (the
  engine may — the cabin optimizer, off by default), or `CABIN`
  (created now, and its values observed on the **literal probe's** first
  selection; the correlated probe takes `n = 2` regardless, §4a). See
  §8.1.

---

## 1. What a Cabin is

A Cabin is a per-(relation, key column) metadata store that tracks the
tuples matching *observed values* of that column. Where a traditional
secondary index covers every tuple of a relation unconditionally, a
Cabin covers exactly the values that queries have touched — the record
is materialized by observation, and only there does it hold authority.

Three structures share the access path, one per trust class:

| structure | authority | write cost | droppable |
|---|---|---|---|
| clustered pk tree | always authoritative | per-write maintenance | never |
| **Cabin** | **authoritative for observed values** | probe + append, observed values only | per value |
| Waystone trail | never authoritative (advisory) | zero | wholesale |

Cabin's defining invariant (the whole spec hangs on it), stated
precisely — per snapshot, and one-sided:

> **Observed ⇒ complete (superset form).** For every observed value `v`
> and every active snapshot `S`: v's entry set ⊇ { pk : tuple visible
> in S whose key column equals v }. Missing a qualifying pk violates
> authority; a surplus entry never does — surplus is removed at read
> time by verification (§4). The empty set (after verification) is an
> authoritative "no rows".

Two corollaries fall out:

- An observed value's **empty entry set is an authoritative "no rows"** —
  negative answers and exists-checks are sound for observed values,
  something no advisory structure can offer (absence has a witness: the
  write hook of §5).
- **Un-observing is always legal.** Dropping a value's entry set together
  with its observed mark returns queries for `v` to the authoritative
  scan path — a performance loss, never a correctness one. This is the
  value-granular analog of waystone invariant 8, and it is what keeps an
  *authoritative* structure evictable.

## 2. Scope (v1)

- Key: a **single non-pk column** with an equality predicate. The pk
  column needs no Cabin (the clustered tree is its Cabin).
- **C3**: no predicate-scoped cabins (`WHERE state='closed'` subsets),
  no multi-column keys, no range observation. The v1 shape is
  `Cabin(relation, column)` with per-value entry sets, full stop.
- Single relation only. Join acceleration stays with Waystone (§7);
  a Cabin never spans relations — the write hook must remain core-local
  (§6).

## 3. Data model

```
Cabin            := (rel_oid, column_no)            — catalog object
observed set     := { value → entry-set ref }        — the directory
entry set (v)    := list of CabinEntry               — C2 + C6

CabinEntry (24 B):
  pk           u64   — Keystone id            (authoritative tier)
  page_id      u32   — last-seen heap page    (advisory tier)
  page_epoch   u32   — heap epoch at write
  slot         u16
  flags        u16   — bit 0: kHintValid
  reserved     u32
```

**C2 — authority lives in the pk, never the location.** The pk is
resolved through the clustered tree when the hint fails; under the
adopted issue-once invariant (K1/K2) a stored id is a
**forever-unique, immutable name**: it can dangle, but it can never
mis-attribute. This buys four things:

1. **Relocation invariance** — the physical optimizer moves pages
   without ever touching a Cabin, exactly as decided for secondary
   indexes in B2 (value = pk indirection). An authoritative structure
   that required maintenance on relocation would couple two subsystems
   that are deliberately independent.
2. **No incarnation checks** — a pk needs no epoch or identity
   protocol; by K1 there is exactly one tuple it can ever mean. MVCC
   visibility is evaluated on the tuple as always.
3. **Dangling ⇒ skip, permanently** — a pk absent from the clustered
   tree (aborted insert, fully purged row) can never resurface under a
   new tuple, so a dangling entry is dead forever and is droppable on
   sight (§5).
4. **Compactness** — 24 B per tuple keeps C5's full-coverage limit
   affordable (§8).

**C6 — the advisory tier on top.** The location hint is waystone-class
advice riding on cabin-class truth: verified by the reader under the
**same rules as waystone entries** (shared validation code, not a
parallel implementation — two verifiers would be where the bugs live):
fetch hinted page → epoch match → Keystone id at slot equals entry pk →
then the mandatory MVCC + key re-check of §4. Any failure falls back
to pk descent — results never change — and the hint is **healed in
place** after a successful fallback (cheap under core-local
serialization, and WAL-free in the unlogged class of §9). Hints cost
nothing to produce: the recording scan and the insert hook both hold
the location in hand when they write the entry. They also enable a
serve-time optimization: sorting a value's entries by page_id batches
same-page tuples into one fetch under the no-pin R1 regime.

## 4. Read path

Compilation stays deterministic: whether `(rel, col)` has a Cabin is
catalog state, so the plan remains `f(shape, catalog)` — a non-pk
equality step on a cabined column compiles to a **cabin probe with scan
fallback**; nothing about the *data* influences the plan.

At execution, for key value `v`:

1. Probe the observed set (core-local hash over the directory).
2. **Hit** → serve the entry set authoritatively. Per entry: try the
   location hint first (C6 verification — epoch, then id-at-slot);
   on hint failure resolve the pk via the clustered tree and heal the
   hint. Either way, then **verify** — MVCC visibility for this
   snapshot, plus re-check of the key-column equality — and apply
   remaining residuals. Verification is not an optimization choice: it
   is what licenses the append-only maintenance model of §5 (entry
   sets are supersets; the read does the subtraction). A pk absent
   from the clustered tree is a **skip**, never an error, and
   duplicate pks within a set are served once (seen-set dedup —
   duplicates are expected under append-only maintenance, §5). An
   empty post-verification result returns zero rows with authority.
3. **Miss** → run the authoritative scan as today, and (policy
   permitting, §8) *record while scanning*: this execution's matches
   become `v`'s entry set, and `v` enters the observed set when — and
   only when — the scan completes.

The miss path is why the first query for a value costs nothing extra in
big-O: it was going to scan anyway; recording is a side effect. In the
constant, the recording walk decodes the filter's columns, and the pk
only for a row whose key matches — everything the recording reads and
nothing else — so what recording adds to the walk it shadows is one key
comparison per walked row.

### 4a. The correlated probe

A cabined column bound by equality to an *earlier step's* or an enclosing
chain's column — a join key, a correlated `EXISTS` — probes the Cabin
**per outer row**, with the value read from the frame instead of a
compile-time literal (`CabinProbe::key_from`). Everything downstream of
the key construction is §4's machinery unchanged: an observed value
serves its set, a miss walks and (policy permitting) records, the
hint-failure path re-records. This is IX17's shape one trust class over
(`index.md` §8a), and it is the only **banked** acceleration a **heap**
relation's join column can have — IX3 refuses it an index. The
statement-local inner build (`docs/spec/join-inner-build.md`) does not
compete for the claim: a Cabin is declared, authoritative and survives
the statement; the build is undeclared, unauthoritative and dies with it.
Ladder order encodes the economics — a converged Cabin serve beats any
per-statement rebuild — so the Cabin keeps priority and the build takes
only what nothing banked can serve.

Selection is `f(shape, catalog)`, last of the **banked**-structure arms:
after both index forms (an index is complete for every key value) and
after the literal Cabin (a compile-time key needs no per-row read), and
only across an identical `(type_val, len)` descriptor — the write hook
observes values coerced to the cabin column's type, so only an identical
descriptor makes the outer row's decoded value the form the set was keyed
on. Cross-core, the kind ships as its walk (the ship-time downgrade)
exactly as the literal form does.

The economics are §8's per key, multiplied by the outer side's key
distribution. **The correlated form earns observation per key at
`n = 2`, declaration or not.** C7's "declared records on first selection"
is an argument about the literal shape, where the operator named the one
value the statement probes; a join probes a value per outer row that
nobody named, and under a never-repeating key distribution a first-touch
rule would record a dead set — and its forever write-hook tax — for every
key it would never see again, with one SELECT able to flood
`cabin_max_values` on its own. Per key: the first touch costs one
sighting insert and records nothing; a genuinely repeating key — across
statements, or within one join's fan-in — records on its second touch and
serves from its third. A join whose keys mostly repeat converts its
O(outer × inner) walk into O(distinct-keys × inner + hits), one counted
miss later — provided the two touches land inside one sighting window:
the table's wholesale reset (`kMaxSightings`, the store's one crude
eviction) can keep a key oscillating below threshold when repeats are
separated by more distinct keys than the window holds. A join whose keys
never repeat pays a hash insert per key and nothing else. The literal
probe's `n = 1` is untouched. What one statement can flood is bounded to
its intra-statement repeats.

`correlated_scans` counts at the sub-chain driving step's **actual
walk**: a miss counts, a serve does not, and a converged EXISTS's counter
goes quiet exactly when the quadratic work does.

**The correlated `EXISTS` converges, by sub-chain mode.** An EXISTS whose
outer key has a qualifying match stops each walk at the first hit, and a
stopped walk's set is partial (C1), so it could never commit. Inside a
sub-chain there is no quota (V09 refuses the tail at subquery depth), so
every stop is the sub-chain's own short-circuit, whose answer is already
decided — and a walk carrying a live recording therefore runs on
*through* the stop, visiting the remaining rows for the recording block
alone, and commits a whole set. The probe that records a key pays a full
walk instead of the short-circuited prefix — the observation charge,
once — and every repeat after it serves at hit cost with the
short-circuit landing on the set's first entry. Under `n = 2` the first
touch only sights and keeps the short-circuited prefix, the second
carries the charge, the third serves. **The caps bound the license**: a
value the per-cabin value cap could never admit is refused *before* the
walk (`MayObserve`) and keeps the short-circuited cost outright, and a
set that outgrows the per-value entry cap mid-walk revokes the license
there — the walk ends at the next stop and nothing is committed. Without
both, a cap-refused value would re-arm on every probe and each doomed
attempt would be a full relation walk — an accelerator turning a
rows-returning statement into a budget refusal, which §1 forbids.
Top-level statements are untouched: there a stop can be a quota, whose
bounded-work property a completed walk would break, and the mode is never
set on a top-level runner.

**The serve emits in the walk's order, not entry order.** An UPDATE that
moves an earlier pk into an observed value appends that pk at the set's
end, and serving entry-order would reorder a reply against I12's
within-step contract — reachable by a plain single-relation probe, not
only by a join. The serve sorts to the walk's order before emission — pk
while a relation's keys have only ascended, page and slot once one has
been admitted below its high-water mark — IX8a's rule applied with
`heap-and-tuple.md` §4.1's `key_order` respected.

### 4b. Authority under a split relation

**A Cabin's entry set is authoritative for (observed value × the ranges
its core owns).** `CabinStore::NoteWrite` appends only where the value is
observed and only on the core performing the write, so a write into a
range another core owns is appended nowhere; a set therefore speaks for
the ranges its core owns and nothing else.

This is an **authority change, not a narrowing by convention.** §1's
promise is unchanged in its own scope; what the scope is has become
explicit, because a relation may have more than one owner. Three rules
follow, and none of them is a preference:

1. **A relation of one range is unchanged, byte for byte.**
   `ranges.empty()` is CC9's zero-cost invariant reaching the serve path —
   one load from an entry the step is already holding and one predictable
   branch — and it is written first so it can never pay for the split
   case. A one-range relation's owner owns every range, so the scoped
   claim and the v1 claim are the same claim.
2. **A probe resolves the ranges it needs through the range directory.**
   Ranges the serving core owns are answered from the set; any range it
   does not own falls through to that range's own stage — which is the
   fan-in the read surface already opens (`docs/spec/crosscore.md` §2a).
   The Cabin does not open the stage and never could: which cores a read
   fans out to is a plan-time fact, and the plan already splits a split
   relation into one stage per maximal contiguous run of same-owner
   ranges.
3. **Therefore a set may be served only where the serving core's owned
   ranges cover the walk the step would otherwise do.** That is the
   predicate, stated at the serve site rather than inherited from the
   router: `TableAccess::ServableBy` is what the dispatcher asks before
   reading locally, and a serve that leaned on it from two functions away
   would be correct only because of a neighbouring invariant. When it does
   not hold, the probe **falls through to the walk** — always legal, §1's
   corollary, a performance event and never an answer.

Every core holds its own Cabin store — its listener, its peer dispatcher
and the fan-in stages it runs — so a relation's owner observes, appends
and serves whether a statement reached it shipped, as a stage, or on its
own listener. A split relation (two or more owners) is never read
locally: `HandleSelect` routes it to the fan-in, `CheckReadAffinity`
refuses whatever the fan-in's shape gate will not admit, and a stage
serves only where `ServableBy` holds for the whole walk — so on a split
relation a Cabin probe serves nothing and walks everything, and §4c's
fall-through counter reads it.

**The transition rule is the discard**, and it is `crosscore.md` CC10's:
sets banked while the relation was whole were banked under the *old*
claim, and nothing in a set records which claim it was made under. They are
therefore dropped before the grant that creates the second owner, not
after. See CC10's pre-grant window; the two halves are one rule and the
sequence is where it lives.

### 4c. What the serve path reports

Two counters, because a Cabin whose savings cannot be seen cannot be
measured and an unmeasured saving is not claimed:

- **`cabin_scope_fallthroughs`** on `SHOW META`, and `scope_declines` per
  cabin on `SHOW CABINS` beside that cabin's hits and misses — probes that
  found an observed set and declined to serve from it because rule 3 did
  not hold. It is the one number that distinguishes "this Cabin is not
  earning its write hook" from "this Cabin cannot be reached from where
  the read runs", which no other counter can tell apart.
- **`cabin_split_discard`** on `SHOW META`, keyed by relation, in the
  refusal-counter form and absent at zero — sets dropped by CC10's
  pre-grant discard. Its reading is the cost of the transition rule:
  re-observation, priced in misses that follow.

## 5. Write path — the witness

The write hook is what "observed ⇒ complete" costs. The superset
invariant (§1) makes every mandatory action an **append**; removal is
never performed on the hot path — eager removal is not merely
unnecessary but *incorrect*, because an older snapshot may still be
entitled to match the entry through the undo chain. Per Cabin:

| operation | action | why it preserves authority |
|---|---|---|
| INSERT | probe observed(new.v); on hit, **append** pk — O(1), no scan | a missed append is the only way completeness can break |
| DELETE | **nothing** — removal forbidden | older snapshots still see the row via undo; read-time visibility excludes it for newer ones |
| UPDATE, key column unchanged | nothing | — |
| UPDATE, key v→v′ | **append** pk to v′'s set if v′ observed; leave v's set untouched | v′ readers need the entry; v's entry is *required* by pre-update snapshots and is a read-filtered surplus for newer ones |

(UPDATE of the pk itself does not exist: K2 makes it Unsupported.)

Nothing prunes: a surplus entry stays until its value is un-observed.
Any physical removal of surplus entries is bound by two rules, one per
class:

- **Dangling entries** — pk not present in the clustered tree. By K1
  these are permanently dead: droppable on sight, no horizon
  reasoning.
- **Non-matching entries** — the tuple exists but no longer matches
  `v`. Droppable only past the same oldest-active horizon that undo
  truncation uses (a pre-update snapshot may still match the entry).
  No new horizon machinery is invented.

**Read-time deduplication** is required independently of any pruning: a
value round-trip (`v→v′→v`) duplicates an id under append-only
maintenance. Serving dedupes with a seen-set.

Cost honesty: a relation with cabins pays one directory probe per write
per Cabin. Core-local, in-memory, O(1) — but not zero, and it is the
price of authority. Relations with no Cabin pay nothing.

## 6. Why this is sound in KDS specifically

The classic hazard of build-by-observation is the write that slips
between the recording scan and activation. KDS deletes the hazard
structurally: statements for a relation run on its owning core (D3) to
completion, with no mid-statement interleaving — so *scan + record +
mark observed* is atomic with respect to every other statement that
could touch the relation. No build locks, no double-scan protocols.
The second classic hazard — identifier reuse corrupting stale
references — is deleted by the adopted issue-once invariant (K1).
This spec is valid **only** under both: the core-ownership execution
model and the Keystone id contract. If either changes, §4–§5 must be
redesigned, not relaxed.

### 6a. A set is banked only from a view nothing can contradict

§6's argument is about a write racing the scan, and core ownership does
delete that. What it does not reach is a write the scan *could not see*,
resolving afterwards:

- a transaction **in flight** when the walk ran has rows the walk could not
  see, and the moment it commits they are live and missing from a set that
  is authoritative for every later reader. Nothing rolls back, so no event
  exists that a repair could hang from;
- the walk's **own** transaction may have hidden a row from itself — by
  delete-marking it, or by an `UPDATE` that moved it off the probed value —
  and its `ROLLBACK` restores the row while nothing restores the entry.
  `NoteCabinWrite` runs on the INSERT and UPDATE write paths and never on
  rollback, so no append repairs it.

Both are the C1 break the store's header forbids in as many words
("missing a qualifying pk violates authority"). The second, in nine
operations:

```sql
CREATE CABIN ON t(v);  INSERT INTO t VALUES (0, 'a');
BEGIN;  DELETE FROM t WHERE v = 0;
SELECT * FROM t WHERE v = 0;   -- 0 rows, and would bank the empty set for v = 0
ROLLBACK;                      -- the row is live again
INSERT INTO t VALUES (0, 'b');
SELECT * FROM t WHERE v = 0;   -- would return the second row alone
```

**The rule.** `WalkAndRecord` declines when the walk's read view carries
any in-flight transaction or belongs to one:

    view.in_flight_count == 0 && view.own_trx_id == kNoTrxId

Both facts are already in the `ReadView` the walk carries, so this costs
two comparisons on the miss path and needs nothing new. Declining is free
by §1's corollary — an unobserved value is answered by the authoritative
scan, a performance event and never a wrong one — and `SHOW CABINS`
reports `unbankable_views=` so an operator can tell "nobody probed this
column" from "every probe that would have recorded ran inside a
transaction".

**The assumption this rests on, named because nothing else names it**:
`TransactionManager::Begin` allocates an id and pushes into `live_`
*eagerly*, for read-only and `REPEATABLE READ` transactions alike, so every
one of them appears in every other view's `in_flight`. If ids ever become
lazily allocated for read-only transactions — an ordinary optimization — a
session holding a pre-record view stops being visible to this guard, the
break returns, and **no test fails**. Anyone touching that allocation owes
this rule a second look.

**What it costs, stated rather than discovered.** Two things, and the first
is wider than it sounds:

- `in_flight_count` is a property of the *manager*, not of the relation, so
  **one** session idling on an open `BEGIN` stops every Cabin on that core
  from building — including relations that transaction has never touched.
  Not "a workload that stays in transactions": one session is enough.
- the heal path (`FallBackAndReRecord`) un-observes *before* it re-records,
  so a stale-hint probe taken inside a transaction drops the set and
  declines to rebuild it. On a heap relation that erodes a Cabin rather
  than leaving it alone, which is the one place "declining is free" is not
  the whole truth.

Both are the honest price of an authoritative structure whose promise
outlives the statement that filled it, and the conservative side is the
only safe side.

**The other site that banks a set enforces the same authority by a
different test.** `CabinOptimizerExecutor::BuildSeededSets` builds under a
check view and aborts the whole build on a `kBusy` row, which is stronger
where it applies; an editor who fixes one site and not the other leaves
half the rule standing.

Two narrower rules are **rejected**. "A transaction that has delete-marked
nothing may record" is unsound, because a value-changing `UPDATE` hides a
row from the probed value just as a delete-mark does. Un-observing on
rollback repairs the second bullet and cannot touch the first — a
transaction that *commits* rows the recorder could not see never rolls
back, so there is no moment at which to drop the value; a repair that
covers half a rule leaves the rule broken.

## 7. Interlock — three structures, one story (C4)

Distinct trust classes, cooperative operation; none replaces another:

- **Patterns nominate.** Which columns are worth observing is not
  guessed from raw traffic: an auto-registered pattern's shape names the
  filtered columns, and the observed shape is the only source — there is
  no user-declared pattern.
- **Waystone senses.** Per-instance `use_count` says how often a value
  recurs; the recording scan measures its exact cardinality. Both are
  measuring instruments for promotion (`cold → trail (advisory, free) →
  Cabin (authoritative, earns its write hook)`, decided by §8.1's
  controller). Waystone is Cabin's measuring instrument, not its
  competitor.
- **Cabin serves; Waystone still accelerates.** In a chain like
  `…FROM account JOIN trade ON trade.account_id = account.id WHERE
  trade.sym = $sym`, the sym step is served by the Cabin
  (authoritative), the pk join step by probe/trail replay (advisory),
  and location hints on cabin entries (C6) are waystone-class advice
  riding on cabin-class truth. Each layer degrades independently:
  hints go stale → pk descent; value evicted → scan; trail evicted →
  descent. Correctness never depends on any of them.
- **ANALYZE narrates all three.** Per step: cabin hit/miss, trail
  replay/fallback, and the recording events themselves.
- **And all three are scoped to one core's ranges.** Cabin's scope is
  §4b's — observed value × the ranges its core owns — Waystone's is its
  trail's own core by invariant 8, and the clustered tree's is the range
  its owner holds. The three degrade independently *within* a scope; none
  of them speaks for a range its core does not own, and the fan-in is
  where the ranges are put back together.

## 8. Materialization policy and the full-coverage limit (C5)

Materialization is demand-driven — a value's set is built by the read
path's witness (§4) and never ahead of a probe — and bounded by two caps
that **refuse to observe rather than truncate**: `cabin_max_values`
(`CabinLimits::max_values`, default 4096) per Cabin and
`cabin_max_entries_per_value` (default 4096) per set. Nothing may depend
on either number.

### 8.1 The per-column policy (C7)

Above all of that sits one declaration, made per column at `CREATE TABLE`
and fixed for the relation's life. It answers a question no measurement
can: **who is allowed to decide that this column carries a Cabin?**

| written | policy | meaning |
|---|---|---|
| `col type NO CABIN` | disabled | No Cabin on this column, ever, by any route. `CREATE CABIN` is refused; auto-creation will never consider it. |
| `col type CABIN AUTO` | auto | The engine may create one when its own signals say the column has earned it — the cabin optimizer (`docs/spec/physical-optimizer.md` Part II), see below. |
| `col type CABIN` | enabled | A Cabin is created at `CREATE TABLE`, and its values are observed on **first** selection. |
| `col type` | unset | Read as *auto* by every reader, and stored distinctly so "nothing was said" stays distinguishable from "the engine may decide". |

Three things this settles.

**The axis is authority, not on/off.** A Cabin is a standing cost — a
directory probe on every write to the relation — paid against a benefit
that depends entirely on the workload. So the useful question is not
"should this column have one" but "whose judgement decides", and the three
answers are the operator's, the engine's, and nobody's.

**`enabled` implies n=1, not just creation.** A declared Cabin observes a
value on its first selection where an engine-created one waits for the
second: *a declaration is the evidence that waiting exists to gather*. An
operator who wrote `CABIN` on a column has already said it is probed by
value; asking traffic to prove it again asks a question that was
answered. **n=1 covers the literal probe and not the correlated one.**
That argument is about a value the *statement named*; §4a's correlated
form probes a value per outer row that nobody named, so a declaration is
no evidence about it and it takes `n = 2` whatever this column says — the
same sentence `fk_check`'s reverse check reasons by, that a value asked
once must not buy a standing write-hook cost.

**`auto` is consumed by the cabin optimizer**
(`docs/spec/physical-optimizer.md` Part II): a per-core background
controller that CREATEs, EXTENDs, HEALs and DROPs Observational Cabins
under a fixed-point cost-benefit core — its threshold is the θ_create
hysteresis margin over measured scan cost and frequency. It creates
through the single `Catalog::CreateCabin` door, stamps its rows **origin
`kCabinOriginAuto`** — the ownership tag its jurisdiction rule and
ANALYZE's `cabin_optimizer=true` mark both read, and what separates a
Cabin the engine may drop on its own judgement from one an operator
declared — and is **off by default** (`cabin_optimizer`). With the
controller off, a column declared `auto` behaves exactly as an undeclared
one. The controller's signals are core 0's, so a peer-owned relation
earns no `CABIN AUTO`.

A policy on the **primary-key column is refused**, not ignored: the pk's
Cabin is the clustered tree (§2), so any of the three would be a statement
about something that cannot exist, and silently dropping the clause would
leave an operator believing they had said something.

The policy is stored on the `sys.columns` row and enforced in exactly two
places: `CREATE TABLE` creates the Cabin an *enabled* column asks for, and
`Catalog::CreateCabin` refuses a *disabled* one whoever asks — the single
door every Cabin comes through, so no creator can skip the check.

**C5 changes the ceiling, not the policy.** Full observation of a
column is *permitted*: with 24 B entries (C6), a fully observed Cabin
weighs roughly three times a secondary index's leaf level — still far
from a data copy — and that is functionally what it has become, built
lazily, paid for value by value, each increment individually evictable.
The traditional index is thus the limit case of a Cabin, not a competing
feature. What C5 does **not** do is make full coverage a goal:
materialization remains demand-driven, and the caps above decide how far
toward the limit any Cabin actually travels.

## 9. Durability class

An Observational Cabin is **unlogged authoritative**: the completeness
promise holds only while the system is up and the write hook is live, so
a restart declares every Cabin fully unobserved and traffic rebuilds them
— invariant-preserving by C1's own terms, and it keeps the write hook
WAL-free (the probe+append never touches the log). The directory and the
entry sets are core-local and memory-resident; no page class holds them.
The Bound class is logged (§12).

## 10. Catalog

`sys.cabins` (`SysCabinRow`, 28 bytes on disk, `include/kds/catalog/rows.hpp`):

```
sys.cabins
  cabin_id     u64     (AllocateRowId(kSysCabinsTable))
  rel_oid      u64
  observed_ct  u64     (values currently observed; best-effort)
  column_no    u16     (schema position; never 0 — the pk has no Cabin)
  origin       u8      (unset / auto / user)
  status       u8
```

`CREATE CABIN ON trade(sym)` / `DROP CABIN` for the user path; the cabin
optimizer's rows carry `origin = auto`, the tag that marks a Cabin the
engine may drop on its own judgement.

## 11. Out of scope / open

Expression and predicate-scoped cabins (C3), multi-column keys, range
observation, budgets and demotion of write-hot values, pruning cadence,
the controller's tuning, and a Cabin's fate under a migrated relation are
decisions unrecorded here. Facts that stand in their place: a Bound Cabin
keeps `crosscore.md` §6a's gate on split and migration both; an
Observational Cabin's sets are discarded on migration, not carried, and
traffic rebuilds them.

---

## 12. Cabin classes — Observational and Bound

This section is the revision `docs/spec/assertion.md` §5 requires, and it
is **normative for the class boundary only**. Its authority is derived:
where this section and `assertion.md` §5 disagree on any property in the
table below, the assertion spec wins and this file is wrong — the two are
meant to be readable as one.

**One sentence: a Bound Cabin is a Cabin that is required to have observed
everything, forever.** The same relationship `index.md` IX1 states for a
secondary index ("an index is a Cabin that observed everything"), with two
properties added that an index does not have — the coverage is *pinned*
rather than merely maintained, and each group carries a **running
aggregate** that a check reads instead of iterating the set. The page
format and the lookup machinery are shared; the **lifecycle contracts are
not**.

### 12.1 The class table

| Property | Observational Cabin | Bound Cabin |
|---|---|---|
| Population | Lazy — observed values only (§5's witness, n=2/n=1) | Eager — full coverage of the group-column combination, built at `CREATE ASSERTION` |
| Coverage contract | Partial by design; a value is either observed or it is not | 100% of the target relation's live rows, with no per-value opt-out |
| Eviction | Allowed — §1's corollary, un-observing is always legal | **Forbidden.** Pages are pinned; un-observing is unavailable |
| Failure response | Un-observe and fall back to the scan: a performance event | **Fail the statement.** Same line `index.md` draws for index maintenance, and for the same reason: a Bound Cabin missing an entry is *wrong*, not slow |
| Durability | Unlogged authoritative (§9); memory-resident; rebuilt by traffic after a restart | **Logged, headered authority class**: WAL-before-data, checksummed, crash-consistent, enforceable at restart with no rebuild scan |
| Role | Advisory acceleration — chooses where to look | Authoritative constraint substrate — decides whether a write is admitted |
| Entry size | 24 B (`stats::CabinEntry`, C6) | **32 B** — the same fields plus the row's aggregate value inline |
| Key | One non-pk column's value, held by value | The `GROUP BY` column list, hashed into a group directory |
| Instances | One per `(relation, column)` | One per **assertion** (§5.3); never shared |

**The scope of the two classes differs and §4b is where the
Observational one is stated.** An Observational set is authoritative for
(observed value × the ranges its core owns) and falls through for the
rest; a Bound Cabin's coverage contract is 100% of the target relation's
live rows, and a scope narrower than the relation would be a contract
change rather than an authority one — which is why `crosscore.md` §6a
gates it from split and migration both.

**Observational Cabin semantics are untouched by this section.** Every
property §§1-11 of this document states — the superset invariant, append-only
maintenance, "removal is forbidden", read-time verification by MVCC plus the
key re-check, caps that refuse to observe and never truncate, un-observing as
the response to any failure, dangling entries droppable on sight (K1) —
continues to hold verbatim for the observational class, and no observational
code path may consult the class table above. A Bound Cabin being logged
decides nothing about the Observational class's durability, because the two
classes' durability follows from their coverage contracts, not from a shared
implementation choice.

### 12.2 Entry layouts

Observational, 24 B — `include/kds/stats/cabin_store.hpp`, pinned by its
own `static_assert`: `pk` (u64) | `page_id` (u32) | `page_epoch` (u32)
| `slot` (u16) | `flags` (u16) | `reserved` (u32).

Bound, 32 B (`BoundCabinEntry`, `include/kds/storage/cabin_bound_page.hpp`,
`kEntryBytes = 32`) — the observational layout plus one 64-bit field:

| Field | Width | Notes |
|---|---|---|
| pk | 40 bit of a u64 | Keystone id. **Authoritative**, under K1: it may dangle, it can never mis-attribute |
| flags | 8 bit | includes the `RESERVED` bit for an in-flight entry (assertion §6) |
| reserved | 16 bit | written 0, ignored |
| location hint (page id / epoch / slot) | 64 bit | **advisory**, verified through the one verifier `exec/tuple_verify.hpp`; on failure fall back to a pk descent and heal in place |
| aggregate value | 64 bit | the row's `SUM` column value, inline (int64). For a `COUNT(*)` assertion it is written **1** |

The normative facts are: **fixed 32 B, pk authoritative, hint advisory, value
inline.** The packing is bound by the engine's on-disk rules —
`static_assert` on size and offsets, fixed-width integers, explicit
shift/mask serialization, no compiler bitfields. The class has its own page
type, `PageType::kCabinBound = 13`.

### 12.3 Group directory and group header

A Bound Cabin is keyed by a **group** — a tuple of values over the assertion's
`GROUP BY` column list — where an observational Cabin is keyed by one column's
value. The directory maps `group_key_hash → group header`, and a group header
holds:

- `count` (int64) — committed **plus reserved** cardinality of the group;
- `sum` (int64, checked arithmetic) — committed plus reserved sum, for a `SUM`
  assertion. Overflow is a statement error, never wraparound (AG3);
- the entry-list linkage into this Cabin's headered pages.

Two consequences worth stating because they are what the structure is for.
**An admission check reads only the group header: O(1), no entry iteration** —
entries exist for violation diagnostics, for re-summation against the header
during verification, and for the abort path, and are not on the check hot
path. And **the running aggregate is not a separate store** (AS5): it is a
field of this header, restored by WAL replay and checkable against the entry
list, so there is exactly one number per group and no reconciliation problem
between two.

A group-key hash collision must isolate the colliding groups — a header is
found by hash and then confirmed against the stored group key, never trusted
on the hash alone. A Cabin's answer is authoritative here, so the
value-held-by-value rule §3 gives for the observational class carries over
with the group key in place of the value.

### 12.4 Lifecycle contracts

| Phase | Observational | Bound |
|---|---|---|
| Creation | A value becomes observed by the witness on the read path (n=2), or by `CREATE CABIN` (n=1) | `CREATE ASSERTION` full-scans the relation, builds every group, and **fails the CREATE** on any pre-existing violation, discarding the partial build (AS7) |
| Steady state | Append-only; a cap refuses to observe a new value | Append-only for entries plus a header delta; admission may **refuse the write** |
| In-flight | No notion — a Cabin never gates a statement | An entry may be `RESERVED`; commit clears the flag, abort removes the entry and subtracts its delta through the undo chain |
| Removal | Any failure, cap, or pressure may un-observe a value at any time | Only `DROP ASSERTION` removes one, which tears the structure down and unpins its pages (`ASSERT_DROP`). `DROP TABLE` on a relation carrying an assertion is `Restrict` |
| Restart | Every value reads as unobserved; traffic rebuilds | Replay restores headers and entries exactly; the constraint is enforceable immediately, with **no enforcement gap** |

`DELETE` calls neither class's write hook: for the observational class because
removal is forbidden (§5 — an older snapshot may still match through the undo
chain), for the bound class because v1 constrains upper bounds only and a
deletion is strictly decreasing (AS11). The two reasons are unrelated and
neither may be cited for the other.
