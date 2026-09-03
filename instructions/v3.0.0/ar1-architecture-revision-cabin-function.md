# AR1 — Architecture Revision: Cabin and Function

Status: DRAFT, pending operator ratification
Author: CLA, 2026-09-03, against `6ead2a0`
Scope: `docs/spec/cabin.md` (C3, §2, §10, §12), `parser/fingerprint.hpp`,
`catalog/rows.hpp` (`SysCabinRow`, `SysPatternRow`), a function catalog
that does not yet exist, and `waystone-concpets.md` where AR1-7 touches it
Claim tags: `[source-read]` with `path:line` at `6ead2a0`; everything
else `[design]`. Nothing is `[measured]`.
Relation to AR0: independent of the pool and the view; §11 states the
one crossing.

**AR1-V is appended below**, the source-read verification at `74f971b`
(nothing under `src/` or `include/` moved between `6ead2a0` and it that
AR1 cites). **AR1-V is what the tree says wherever this body disagrees**,
and it disagrees in two places that matter: §3's quiet-wrong hazard does
not exist on the path it names, and §7's migration mechanism names a bit
that is not there. §14's ordering rests on the first.

---

## 0. Decisions proposed

- **AR1-1** Scalar functions enter the grammar through a **function
  catalog** whose entries declare a purity class and, optionally, a
  cover method (§2). No function exists without an entry.
- **AR1-2** A statement carries a **determinism class** D0/D1/D2
  computed at parse from the purity of its predicate-position functions
  (§3). D2 statements are fingerprinted but neither registered nor
  recorded nor observed.
- **AR1-3** The fingerprint yields a second shape hash, `fetch_id`,
  excluding the select list; trails key on it, statistics keep
  `pattern_id` (§4). `kFingerprintVersion` does not move.
- **AR1-4** A Cabin is defined by a **key function** `F: tuple → K` and
  an optional **fold** `G: set → value` (§5). Column, group tuple,
  expression, predicate and chain are five shapes of `F`, not five
  structures.
- **AR1-5** A Cabin's identity is `(rel_oid, expr_id)`, the expression
  fingerprint of `F` (§6). `sys.cabins` is revised accordingly (§7).
- **AR1-6** C3 is lifted; §2's "single relation only" is lifted for the
  chain shape under the condition §10 states; C7's policy moves from the
  column to the expression (§8).
- **AR1-7** §12's class table gains its true axis — **the source of
  completeness** — and the advisory class is placed outside Cabin (§9).
- **AR1-8** The witness generalises from "key column changed" to "an
  input column of `F` changed", and for the chain shape is composed
  from per-step Cabins the engine creates itself (§10).

## 1. Background — three narrowings and why each was made

`cabin.md` v1 fixed three narrowings, each with a stated reason:

- **C3** — no expression, predicate-scoped or multi-column keys
  (`cabin.md:19`, §2 `:78-80`). Reason: there were no functions in the
  grammar to key on, and one column held by value was enough to state
  the superset invariant precisely.
- **§2 "single relation only"** (`:81-83`) — a Cabin never spans
  relations because *the write hook must remain core-local* (§6).
- **C7** — the policy (`NO CABIN` / `CABIN AUTO` / `CABIN`) is declared
  per column at `CREATE TABLE` (`:26-31`).

The first reason expires when functions land. The second expires under
AR0: once ownership is affinity, no write hook is core-local, and the
argument that kept joins with Waystone (§7 of `cabin.md`) loses its
premise. The third follows the first. AR1 is what replaces the three,
and it is written so that the v1 shape — `Cabin(relation, column)`,
per-value sets, 24 B entries — is the `F = column` case of it, byte for
byte.

## 2. The function catalog

Every scalar function is a catalog entry:

```
name            folded identifier, the token the lexer hashes as shape
arity           fixed, or variadic with a minimum
purity          kImmutable | kStable | kVolatileStatement | kVolatileRow
evaluate        tuple/value → value
cover           optional: predicate on F's output or on one input → key set (§9)
monotone        optional: which input, which direction (a cover for ranges)
```

**Purity, and what each class may do:**

| class | meaning | predicate | Cabin key | fingerprint |
|---|---|---|---|---|
| `kImmutable` | value depends on arguments only, forever | yes | **yes** | shape |
| `kStable` | constant within a statement, may differ across statements (`NOW()`, session settings) | yes | no — write-time and read-time evaluations differ | shape; its value folds into the instance key at BIND (§3, D1) |
| `kVolatileStatement` | one fresh value per statement, not derivable from arguments (`RANDOM_SEED()`) | yes | no | as `kStable` for the instance key; the pattern is D1 |
| `kVolatileRow` | one fresh value per row (`RANDOM()`) | yes | no | pattern is D2 |

**The default for an entry that declares nothing is `kVolatileRow`.**
This is the conservative end, and §3 says why it is the only safe one.
`[operator, quiet-wrong]` — D2 below.

Aggregate functions (`aggregate.md` AG2, `ast.hpp:486`) are not entries
here: they are folds over sets, and AR1 keeps them where they are. The
fold `G` of §5 names them.

## 3. Determinism class of a statement

Computed at parse, not at lex: the fingerprint accumulator does not know
a function from a column, and it must not — `fingerprint.hpp:36-44`
makes every identifier shape and that rule is what keeps
`kFingerprintVersion` still. The parser folds the purity of every
function in **predicate position** (`WHERE`, `ON`, `HAVING`; not the
select list, per §4) to the statement's class:

| class | statement | pattern identity | instance identity | recorded / observed |
|---|---|---|---|---|
| **D0** | only `kImmutable` functions, or none | valid | valid | yes |
| **D1** | at least one `kStable` / `kVolatileStatement` | valid | valid **after** the function's value is folded into the instance key at BIND, in argument order, as a `?` would be | yes |
| **D2** | at least one `kVolatileRow` | valid | undefined | **no** — statistics count the pattern; no `sys.patterns` instance, no sighting, no trail, no Cabin observation |

D1 is not a degradation. A time-range statement with `NOW()` is the
common case of an application dashboard, and under D1 its instance key
is complete for the execution it names, which is all a trail or an
observed set ever claims.

**Why the default purity must be D2, stated at the site it protects.**
`arg_hash` is the hash of literal **text** (`fingerprint.hpp:76-84`). A
row-volatile function treated as immutable gives two executions of
`WHERE id = F()` one instance key; the first records the location of the
pk it happened to compute, the second replays it. The replay validation
of `waystone-concpets.md` §2 is against **storage** — `rel_oid`, the pk
at the recorded slot, the epoch — and rule 0 re-derives the probe key of
a **join** step (`:39`). Whether the **driving** step's entry is checked
against the derived driving key is not stated by the spec.
`[source-read required: exec/trail_replay.hpp, the entry-0 path]`. If it
is not, storage validation passes (the slot still holds the pk that was
recorded) and the wrong row is returned. The collision-safety argument
(`fingerprint.hpp:85-96`) is about *shape* collisions, which degrade to
a miss; it says nothing about *argument incompleteness*, which does not.
AR1 therefore adds **rule 0′**: the driving step's replayed entry must
match the instance's driving key, exactly as rule 0 binds the probe
steps. With 0′ in place a misclassified function costs a miss; without
it the default class is the only line. Both are taken. `[operator,
quiet-wrong]` — D8 below.

> **AR1-V1 answers the `[source-read required]` marker in this section,
> and the answer is that the hazard does not exist.** See below.

## 4. `fetch_id` — the select list is post-processing

A trail records `(step_id, rel_oid, pk, location)` and no projected
value (`waystone-concpets.md` §2, entry layout `:113-123`); replay is
validated against storage and the probe keys, never against the select
list. Two statements differing only in their select list fetch the same
tuples by the same path. The accumulator therefore keeps a second FNV
state, `fetch_id`, that skips the tokens between `SELECT` and the first
`FROM` at parenthesis depth 0 (there are no select-list subqueries:
subqueries are predicate-position, `ast.hpp:185-202`), and a second
argument stream that skips select-list literals; a `?` in the select
list is tagged so that BIND folds its value into the statistics instance
and not the trail instance.

- `pattern_id` — unchanged, statistics and the cabin optimizer's
  identity, because `SELECT SUM(x) FROM t WHERE …` and
  `SELECT x FROM t WHERE …` are one fetch and two plans.
- `fetch_id` — the trail key. `SysPatternRow` (`rows.hpp:494`) gains it;
  `waystone_root`/`dir_depth` move to be keyed by `fetch_id`, so several
  pattern rows share one root. `[operator, format]` — D6.

## 5. The general Cabin

> A Cabin over relation set `R` is a key function `F: tuple → K`, an
> optional fold `G: set(pk) → value`, and for every **observed** key
> value `v` an entry set `E(v)` with the invariant, per active snapshot
> `S`: `E(v) ⊇ { pk : tuple visible in S, F(tuple) = v }`.

Surplus is removed at read time by verification; a missing pk violates
authority; the empty set after verification is an authoritative "no
rows"; un-observing a value is always legal. These are `cabin.md` §1's
sentences with `F(tuple)` in place of "key column equals". The five
shapes of `F`:

| shape | `F` | `K` | `E(v)` entry | today |
|---|---|---|---|---|
| column | `t.x` | the column's value | 24 B (`cabin_store.hpp`) | Observational |
| group | `(t.g1 … t.gn)` | group tuple, hashed to a directory | 32 B with `G`'s running value (`cabin_bound_page.hpp`) | Bound |
| expression | `f(t.x, t.y, …)`, `f` `kImmutable` | `f`'s output | 24 B | **new** |
| predicate | `p(t)`, `p` `kImmutable` | `{true}` — one key value | 24 B; `E(true)` is the filtered set | **new** (C3's "predicate-scoped") |
| chain | the driving key of a written-order chain `R₀ → R₁ → … → Rₖ` (`parser-v2.md` I12) | the driving predicate's value | 32 B **trail-shaped**: `(step_id, rel_oid, pk, location)` (`waystone-concpets.md` `:113-123`) | **new** — Waystone's entry with Cabin's authority |

`G` is independent of the shape: a `SUM` over an expression key or over
a chain is well-formed. Bound = eager coverage + `G`; that is a
lifecycle contract (§9), not a shape.

**The predicate shape is the expression shape with a boolean key**, and
it is what makes "the narrow result set of a filter" a Cabin: `E(true)`
for `p = (state = 'closed' AND amount > 1000)` is the set a filtered
index would hold, observed rather than maintained unconditionally.

**The chain shape is what makes "the narrow result set of a join" a
Cabin.** Under I12 the chain is the plan and every inner step is a
pk-probe from the outer row (`parser-v2.md:62`), so a chain instance is
a tree of pk-located tuples rooted at the driving rows for one driving
key value — exactly the shape a trail already stores. What a trail lacks
is a witness, and §10 supplies one.

## 6. `expr_id` — the identity of `F`

The statement fingerprint's accumulator, run over the token span of
`F`, with **one rule inverted**: literals inside `F` are **shape**, not
arguments. `SUBSTR(name, 1, 3)` and `SUBSTR(name, 1, 4)` are two
Cabins; a statement's `?` cannot appear inside `F` (a Cabin is a
declared structure, not a bound one). Identifiers fold as
`fingerprint.hpp:48-55` folds them; column references resolve to the
schema position at `CREATE` time and the **position**, not the name, is
what the persisted `expr_id` is computed over — so `ALTER … RENAME` does
not move it, per `alter.md:54`'s concern. For `F = t.x` the `expr_id` is
the hash of one position, and `column_no` in today's row is recoverable
from it; for the chain shape the span is the chain's `FROM … ON …`
tokens with the driving predicate's literal replaced by the marker, i.e.
its `fetch_id` restricted to the chain.

A predicate-position `f(x, y) = ?` in a statement matches a Cabin when
the statement's `expr_id` over that sub-span equals the Cabin's. This is
one FNV run over a span the parser already holds; it allocates nothing.

## 7. `sys.cabins` revised

`SysCabinRow` (`rows.hpp:745`, 28 B) becomes:

```
cabin_id      u64
rel_oid       u64      the driving relation (R₀ for a chain)
expr_id       u64      §6
observed_ct   u64      unchanged, best-effort
shape         u8       column | group | expression | predicate | chain
class         u8       observational | bound   (§9)
origin        u8       unchanged
status        u8       unchanged
key_width     u16      bytes of K held by value in the directory (D3)
expr_text     var-heap  the normalized text of F, for SHOW and for rebuild
```

`column_no` is dropped: it is `expr_id`'s degenerate case and keeping
both invites the two to disagree. A row with `shape = column` written
by a pre-AR1 build is recognised by `status`'s existing version bit and
its `expr_id` computed on read from `column_no` — no rebuild scan.
`[operator, format]` — D7.

> **AR1-V4: there is no version bit in `status`.** See below.

## 8. C3 lifted, §2 and C7 restated

- **C3** is struck. §2 reads: *Key: any `kImmutable` `F` over the
  relation's columns (§5). Predicate keys are `F` with a boolean
  output. Chain keys span the relations of one written-order chain and
  are admitted under §10's composition condition.*
- **C7** moves to the expression. `CREATE TABLE` keeps `NO CABIN` /
  `CABIN AUTO` / `CABIN` per **column** as the default policy for
  `F = column`; expression, predicate and chain Cabins are created by
  `CREATE CABIN ON <rel> (<F>)` and, for `CABIN AUTO` relations, by the
  cabin optimizer when a D0 pattern's predicate-position `expr_id`
  crosses the sighting threshold — the same n=2 the column shape uses.
  `[operator, spec]` — D4.
- The pk still has no Cabin (`F = pk` is the clustered tree), and a
  Cabin whose `F` reads only the pk is refused at `CREATE`.

## 9. §12 revised — the axis is the source of completeness

| Property | Observational | Bound | *(Advisory — not a Cabin, §9.1)* |
|---|---|---|---|
| Source of completeness | **witness** (§10): every write that could make a tuple match an observed key appends | **eager**: full coverage built at `CREATE`, pinned | none: observed from query results, epoch-validated |
| Shape of `F` | any of §5 | any of §5; `G` required | any |
| Population | lazy, observed keys only | 100 % of live rows | lazy |
| Serving rule | `K(P)` (§9.2) every key of which is observed, else fall through | always | never alone; prefetch and probe order for the authoritative path |
| Failure | un-observe; performance event | fail the statement | drop the trail |
| Eviction | allowed | forbidden | wholesale |
| Durability | unlogged, rebuilt by traffic | logged, headered | unlogged |
| Entry | 24 B; 32 B trail-shaped for chain | 32 B with `G`'s value; chain adds `step_id`/`rel_oid` per entry | 32 B trail entry |
| Identity | `(rel_oid, expr_id)` | one per assertion | `(fetch_id, arg)` |

Everything §12 states about the boundary between the first two columns —
"a Bound Cabin is a Cabin required to have observed everything, forever",
IX1's "an index is a Cabin that observed everything", the lifecycle table
`:704-720` — stands. What AR1 adds is that the boundary is one of three,
and that the third does not belong in this file.

### 9.1 Where the advisory class lives — `[operator, spec]`, D1

"Non-authoritative superset" is a superset whose completeness nobody
witnesses: correct until the next write, and unable to say which write.
That is Waystone's contract exactly (`cabin.md:47`: *never authoritative*,
*zero write cost*, *droppable wholesale*), and C4 says the three structures
do not absorb each other. **Proposal: the advisory class stays Waystone,
and Waystone's key generalises from `(pattern_id, arg_hash)` to
`(fetch_id, arg_hash)` (§4) — which already is "an expression key with a
value"** for the chain shape. Putting an unwitnessed set inside Cabin
would give the serve path a set it must not serve alone next to sets it
may, distinguished by a flag, in a structure whose whole soundness
argument (§6 of `cabin.md`) is that a set present is a set complete.

### 9.2 Serving by cover

A predicate `P` is servable from a Cabin with key `F` when the engine can
compute `K(P) ⊆ K` with `P(t) ⇒ F(t) ∈ K(P)`; the answer is
`verify(⋃_{v ∈ K(P)} E(v))`, verified by MVCC and by re-evaluating `P`
on each tuple — the key re-check `cabin.md` §4 already performs, with
`P` in place of the equality. Three sources of `K(P)`:

| `P` | `K(P)` | needs |
|---|---|---|
| `F(t) = c` | `{c}` | nothing — today's case |
| `F(t) ∈ [a, b]`, `F` `monotone` in one input | observed keys in `[a, b]` | a sortable key directory (D3) |
| `x ∈ [a, b]`, `F = f(x)` with `f.cover` | `f.cover([a, b])` — the keys any `x` in the range can map to (`DATE(ts)` → the covering days) | the function's `cover` entry (§2) |

A function without `cover` and without `monotone` serves equality only.
For the predicate shape `K(P) = {true}` when `P` is the Cabin's own
predicate or implies it — implication is recognised only **syntactically**
(the Cabin's `p` is a conjunct of `P`), never by theorem. For the chain
shape `K(P)` is the driving key set, and serving is trail replay under
rules 0 and 0′ with the fall-through removed for observed keys.

Observational serving is all-or-nothing over `K(P)`: one unobserved key
in `K(P)` and the whole statement falls through, because a scan that
excludes the observed keys costs what a scan costs.

## 10. The witness, generalised

`cabin.md` §5's table with `F` in place of the key column:

| operation | action |
|---|---|
| INSERT | `v = F(new)`; observed(`v`) → append |
| UPDATE, no input column of `F` changed | nothing |
| UPDATE, some input changed, `F(old) = v → F(new) = v′` | observed(`v′`) → append to `E(v′)`; `E(v)` untouched |
| DELETE | nothing (removal forbidden) |

`F` is evaluated on the write path, which is why §2 admits only
`kImmutable`: `kStable` would make `F(new)` at write time and `F(t)` at
read time two functions. The cost is one evaluation per write per Cabin
whose input set intersects the changed columns — `cabin.md` §5's "cost
honesty" paragraph with the intersection test in front.

**The chain shape.** An insert into the driving relation `R₀` with
driving key `v` observed: append `R₀`'s pk, then probe `R₁ … Rₖ` by pk
from the new row exactly as the chain would, appending each — k pk
lookups, no scan. An insert into an inner relation `Rᵢ` with pk `p`: the
chain is incomplete iff some observed chain has an `Rᵢ₋₁` row whose join
column equals `p`, which is the question *"is `p` an observed value of a
Cabin on `Rᵢ₋₁.join_col`?"*. So:

> **AR1-8 composition condition.** A chain Cabin over `R₀ → … → Rₖ`
> requires, for each `i ≥ 1`, an Observational Cabin on
> `Rᵢ₋₁.join_colᵢ` in which every driving-key-observed chain's
> `Rᵢ₋₁` join values are observed. The engine creates these as
> `origin = auto` when the chain Cabin is created and observes into them
> from the chain's own witness. A chain Cabin whose supporting Cabin
> un-observes a value un-observes every chain through it.

Under this condition the inner-side witness is a probe of a per-step
Cabin, O(1), and the chain shape stays inside `cabin.md` §6's argument.
Without it a chain Cabin would need a scan on every inner insert, and
AR1 does not admit that.

## 11. The one crossing with AR0

§4b's scope rule — a set is authoritative for *(observed value × the
ranges its core owns)* — and the per-core `CabinStore` (AK-S2) rest on
the write being core-local. Under AR0 M2 any core writes any row, so the
witness must reach one store, or a store partitioned so that every write
to a given key reaches the same partition. `expr_id` is the natural
partition prefix and AR1 fixes nothing further; the store's topology is
M3's, and AR1's shapes are designed so that M3 changes where a set lives
and not what it holds.

## 12. Items for operator judgement

| # | Item | Class | CLA proposal |
|---|---|---|---|
| D1 | Advisory class placement | spec | Waystone, keyed by `(fetch_id, arg)` (§9.1) |
| D2 | Default purity of an undeclared function | quiet-wrong | `kVolatileRow` (§2) |
| D3 | Key width held by value in the directory; sortable directory for monotone serving | constant | 16 B fixed; keys wider are hashed and lose range serving, stated in `SHOW CABINS` |
| D4 | `CREATE CABIN ON <rel> (<F>)` grammar; column policy stays per column | spec | as §8 |
| D5 | Chain Cabin's auto-created supporting Cabins | cost, not correctness | admit; count them under the relation's Cabin cap |
| D6 | `fetch_id` on `SysPatternRow`; `waystone_root` re-keyed | format | one row change, no `kFingerprintVersion` bump |
| D7 | `SysCabinRow` revision, `column_no` dropped | format | as §7, old rows recognised on read |
| D8 | Rule 0′ — driving-step entry checked against the instance's driving key | quiet-wrong | take it, independent of D2 |

## 13. Retired and amended

- C3 — struck. `cabin.md` §2 and §11's first sentence rewritten.
- `cabin.md` §7 "join acceleration stays with Waystone" — amended: the
  *advisory* class stays with Waystone; the witnessed chain is a Cabin.
- `cabin.md` §12.1 key row — replaced by §9's table.
- `waystone-concpets.md` §1/§3 key — `(fetch_id, arg_hash)`.
- `fingerprint.hpp` "What is shape and what is argument" — gains the
  select-list clause (§4) and the expression-span rule (§6).
- `parser-v2.md` I17 — gains rule 0′.

## 14. Sequencing — three work orders

| order | content | depends on |
|---|---|---|
| **AP** | function catalog, purity, determinism class, `fetch_id` + rule 0′, `SysPatternRow` change | nothing — can start now, in parallel with AM/AN |
| **AQ** | expression and predicate shapes, `expr_id`, `SysCabinRow` revision, cover-based serving, C3/C7/§12 prose | AP; AN for any cross-core serve |
| **AR** | chain shape, composition witness, Waystone re-key | AQ; AR0 M2 (write authority) for the inner-side witness under affinity |

AP first because it is the only one of the three with a quiet-wrong
item on the *existing* engine: rule 0′ closes a hole that is reachable
today by a literal that happens to differ in text and not in value
(`fingerprint.hpp:76-84`'s "harmless failure" is a miss; the case rule 0′
covers is the other one).

---

# AR1-V — Source-read verification

CLA, 2026-09-03, on `worktree-v3.0.0-read-view` at `74f971b`. Every
`path:line` the body cites was read. Most verify. **Four findings
follow, and the first two change what AR1 is.**

## AR1-V1 — §3's hazard does not exist, and D8 is not a hole

§3 marks its own `[source-read required: exec/trail_replay.hpp, the
entry-0 path]` and reasons from the possibility that the driving step is
unchecked. **It is checked, structurally, and by the same mechanism rule
0 uses for probe steps.**

`TrailReplay::Find(step_id, pk)` is keyed on `(step_id, pk)` packed into
one `uint64` (`trail_replay.hpp:121-128`), and the header states the
consequence outright at `:41-44`: *"the index is keyed on `(step_id,
pk)`, and the caller's lookup key is the key its step just computed from
the current outer row. An entry can only be found by matching it. There
is no separate check to forget."*

The two call sites are both inside `RunPointStep`
(`step_vm.cpp:578` and `:612`), and both pass `key.value()` from
`KeyFromOperand(*step.key, frame_)` (`:551`) — **evaluated from the
current statement's frame on every execution, driving step included**.
`TryReplay` then re-checks the pk at the recorded location against that
same `key` through `VerifyTupleAt(store_, at->page_id, at->slot, key,
at->page_epoch)` (`step_vm.cpp:659-660`).

So §3's scenario — *"the first records the location of the pk it
happened to compute, the second replays it"* — cannot occur. For the
second execution to reach the first's entry, `Find` must be called with
the same key; a row-volatile `F()` computing a different value produces a
different lookup key and a miss. The failure mode of a misclassified
function on this path is a wasted descent, which is `fingerprint.hpp:85-96`'s
own structural argument reaching one case further than the body credits
it for.

**Two narrower things are true and are what survive of D8.**

1. Replay is confined to keyed lookups: `IsTrailReplayable(kind)` is
   `kind == kLookup || kind == kProbe` (`step_chain.hpp:165-167`), and
   `TryReplay` is called from nowhere but `RunPointStep`. A driving step
   that searches is never replayed at all.
2. Rule 0's **normative sentence** is narrower than the code:
   *"Mandatory before any join replay"* (`waystone-concpets.md:39`). Its
   next sentence already states the general mechanism — *"The replay
   index … is keyed on `(step_id, pk)`, so an entry can only be found by
   matching the freshly derived key and there is no separate check to
   forget"* — so the spec contains both the narrow claim and the wide
   fact, one sentence apart.

**Recommendation: D8 becomes a wording change, not a rule.** Widen rule
0's scope clause from "any join replay" to "any replay, driving step
included", and state that the mechanism is the index key rather than a
check. Nothing in the engine changes. The body's *"Both are taken"* is
correct in outcome and wrong in cost: 0′ costs nothing because it is
already there.

## AR1-V2 — §14's ordering premise falls with V1

§14 puts AP first *"because it is the only one of the three with a
quiet-wrong item on the existing engine: rule 0′ closes a hole that is
reachable today."* There is no such hole (V1), so **AP's priority has
to be argued from something else or given up.** The parenthetical is
also confused on its own terms: `fingerprint.hpp:76-84`'s two spellings
of one number (`42` and `042`) hash differently and *miss* — the
document calls that "the harmless failure" and then says "the case rule
0′ covers is the other one", but the other one is a *collision*, and
`:85-96` argues collisions degrade to a miss by the same structural
route V1 describes.

What can still argue AP first: it is the only order with no dependency,
and D2's default purity does protect real surfaces — the Cabin witness
(§10 evaluates `F` on the write path) and the instance identity a
sighting counts. Neither is trail replay.

## AR1-V3 — `alter.md:54` is a decision against §6's use of it

§6 keeps `expr_id` over schema **positions** so that `ALTER … RENAME`
does not move it, *"per `alter.md:54`'s concern"*. `alter.md` §3 at that
line is **AL3 — Patterns are allowed to die**: *"A fingerprint hashes the
statement's tokens, names included. After `RENAME TO`, traffic written
against the new name is a different shape: stored patterns and their
Waystone trails for old-name statements simply"* — die. That is not a
concern to be avoided; it is a ruling that the death is acceptable.

AR1 may still choose position-based `expr_id` — a Cabin is a declared
structure with a maintained witness, and losing one to a rename is a
different cost from losing a trail. **But it is a divergence from AL3 to
be declared, not a corollary of it**, and `alter.md` needs the sentence
saying why the two structures answer differently.

## AR1-V4 — `status` has no version bit

§7 migrates pre-AR1 rows by saying a `shape = column` row *"is
recognised by `status`'s existing version bit"*. `SysCabinRow`'s
`status` is a plain enumeration — `kCabinStatusUnset = 0`,
`kCabinStatusActive = 1`, `kCabinStatusBuilding = 2`,
`kCabinStatusDemoted = 3` (`rows.hpp:800-812`) — with no spare bit
declared and no version meaning anywhere. `origin` is the same shape
(`:796-798`). **D7's stated migration mechanism does not exist**, and the
work order that lands it owes a real one: the row's on-disk length
(`kOnDiskSize` is 28 today, `rows.hpp:775`), a new status value, or a
catalog format version outside the row.

## What verifies

`cabin.md:19` (C3, verbatim), `:26-31` (C7's three spellings), `:47`
(the authority table's Cabin row), `:78-83` (C3's v1 shape and "single
relation only … the write hook must remain core-local"); `SysCabinRow`
at `rows.hpp:745`, 28 B by `kOnDiskSize`, `column_no` at offset 24 and
its "**Never 0**" comment matching §8's "the pk still has no Cabin";
`SysPatternRow` at `rows.hpp:494`; `fingerprint.hpp:36-44` (identifiers
are shape), `:48-55` (the fold), `:76-84` (literal text, not decoded
value), `:85-96` (collisions degrade to a miss); `ast.hpp:486` (the
aggregate enum, citing `aggregate.md` AG2); `ast.hpp:185-202`
(`kMaxSubqueryDepth` and `PredicateKind`, which supports §4's "subqueries
are predicate-position" by construction rather than by statement);
`waystone-concpets.md:113-123` (the 32 B entry, `pk`/`rel_oid`/`page_id`/
`page_epoch`/`slot`/`flags`/`step_id`); `parser-v2.md:62` (the
replayability table — note that `:62` is the `Lookup` row and the
`Probe` row §5 leans on is `:63`).

Not verified, because the citation names no line and the claim is
structural: `cabin_store.hpp`'s 24 B entry and `cabin_bound_page.hpp`'s
32 B, `cabin.md` `:704-720`, `parser-v2.md` I12/I17, `aggregate.md` AG2.

## What AR1-V does not touch

Every `[design]` proposal — the function catalog's shape, the five
shapes of `F`, `fetch_id`, `expr_id`, the composition condition, the
§12 axis, and D1–D7 — is unverified by construction: none of it exists
in the tree. AR1-V is only about what the body says the tree already
says.
