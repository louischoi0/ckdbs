# Cabin — value-observed authoritative metadata store (spec, v1)

Status: **DECIDED** — C1–C7 fixed; items marked `[PROPOSED]` / `[OPEN]`
are not.
Depends on: **keystone-id-invariant.md (K1–K5, adopted)**, Keystone pk
contract (super column), B+tree pk facade (B1–B3), patterns +
fingerprint (P-series), waystone trail model, core-ownership dispatch
(D3), thread-per-core cooperative scheduling, MVCC undo-chain model.
Related docs: waystone-concpets.md, create-pattern.md,
tracking-levels.md.

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
  engine may, at a threshold — *specified, not built*), or `CABIN`
  (created now, and its values observed on first selection rather than
  second). See §8.1.

---

## 1. What a Cabin is

A Cabin is a per-(relation, key column) metadata store that tracks the
tuples matching *observed values* of that column. Where a traditional
secondary index covers every tuple of a relation unconditionally, a
Cabin covers exactly the values that queries have touched — the record
is materialized by observation, and only there does it hold authority.

Three structures now share the access path, one per trust class:

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
big-O: it was going to scan anyway; recording is a side effect.

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

**Pruning** (physically removing surplus entries) runs lazily in the
background, in two classes:

- **Dangling entries** — pk not present in the clustered tree. By K1
  these are permanently dead: droppable on sight, no horizon
  reasoning.
- **Non-matching entries** — the tuple exists but no longer matches
  `v`. Droppable only past the same oldest-active horizon that undo
  truncation uses (a pre-update snapshot may still match the entry).
  No new horizon machinery is invented.

**Read-time deduplication** is required independently of pruning: a
value round-trip (`v→v′→v`) duplicates an id under append-only
maintenance. Serving dedupes with a seen-set; pruning collapses
duplicates as it runs.

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

## 7. Interlock — three structures, one story (C4)

Distinct trust classes, cooperative operation; none replaces another:

- **Patterns nominate.** Which columns are worth observing is not
  guessed from raw traffic: pattern definitions (user-declared via
  CREATE PATTERN, or auto-registered) name the filtered columns, and a
  pattern's tracking level bounds how aggressively its columns may
  materialize cabins `[PROPOSED]`.
- **Waystone senses.** Per-instance `use_count` says how often a value
  recurs; the recording scan measures its exact cardinality. Together
  they are the promotion signal: `cold → trail (advisory, free) →
  Cabin (authoritative, earns its write hook)`. Waystone is Cabin's
  measuring instrument, not its competitor.
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

## 8. Materialization policy and the full-coverage limit (C5)

Whether a value's set *should* materialize is policy, fed by the column
density classification (density levels 1–5): per-value match counts are
measured exactly by the recording scan, the column class is their
aggregated summary, and dense verdicts suppress futile recording
attempts (level-5 columns never materialize per-value sets).

### 8.1 The per-column policy (C7) — decided 2026-08-03

Above all of that sits one declaration, made per column at `CREATE TABLE`
and fixed for the relation's life. It answers a question the density
classification cannot, because it is not a measurement: **who is allowed
to decide that this column carries a Cabin?**

| written | policy | meaning |
|---|---|---|
| `col type NO CABIN` | disabled | No Cabin on this column, ever, by any route. `CREATE CABIN` is refused; auto-creation will never consider it. |
| `col type CABIN AUTO` | auto | The engine may create one when its own signals say the column has earned it. **Consumed since 2026-08-10** by the cabin optimizer (`docs/feat-physical-optimizer.md` Part II) — see below. |
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
second. This is the rule `CREATE PATTERN` already settled (spec
`create-pattern` §7: n=1 for a declared pattern, n=2 for an auto-observed
one) and it rests on the same argument — *a declaration is the evidence
that waiting exists to gather*. An operator who wrote `CABIN` on a column
has already said it is probed by value; asking traffic to prove it again
asks a question that was answered.

**`auto` is a name, not a behaviour** — *as written 2026-08-03; amended
2026-08-10, when it became one.* The promotion pipeline exists now: the
**cabin optimizer** (`docs/feat-physical-optimizer.md` Part II, PO1-PO10)
is a per-core background controller that CREATEs, EXTENDs, HEALs and
DROPs Observational Cabins under a fixed-point cost-benefit core — its
threshold is the θ_create hysteresis margin over measured scan cost and
frequency, not the `use_count` × cardinality pair this section originally
imagined. It creates through the single `Catalog::CreateCabin` door (so
the policy check below could not be forgotten — exactly the future
auto-creator that sentence anticipated), stamps its rows **origin
`kCabinOriginAuto`** — the ownership tag its jurisdiction rule and
ANALYZE's `cabin_optimizer=true` mark both read, and what separates a
Cabin the engine may drop on its own judgement from one an operator
declared — and is **off by default** (`cabin_optimizer`, experimental,
PO8's kill switch). With the controller off, a column declared `auto`
still behaves exactly as an undeclared one.

A policy on the **primary-key column is refused**, not ignored: the pk's
Cabin is the clustered tree (§2), so any of the three would be a statement
about something that cannot exist, and silently dropping the clause would
leave an operator believing they had said something.

The policy is stored on the `sys.columns` row and enforced in exactly two
places: `CREATE TABLE` creates the Cabin an *enabled* column asks for, and
`Catalog::CreateCabin` refuses a *disabled* one whoever asks — the single
door every Cabin comes through, so a future auto-creator cannot forget the
check.

**C5 changes the ceiling, not the policy.** Full observation of a
column is *permitted*: with 24 B entries (C6), a fully observed Cabin
weighs roughly three times a secondary index's leaf level — still far
from a data copy — and that is functionally what it has become, built lazily, paid for value by value, each increment
individually evictable. The traditional index is thus the limit case
of a Cabin, not a competing feature. What C5 does **not** do is make
full coverage a goal: materialization remains demand-driven, and the
budget (`[OPEN]`: per-cabin page budget, per-value set caps, demotion
of write-hot values whose sets churn) decides how far toward the limit
any Cabin actually travels.

## 9. Durability class `[PROPOSED]`

Cabin pages are **unlogged authoritative**: a new storage class. The
completeness promise holds only while the system is up and the write
hook is live, so crash recovery declares every Cabin fully unobserved
and traffic rebuilds them — invariant-preserving by C1's own terms, and
it keeps the write hook WAL-free (the probe+append never touches the
log). Directory lives core-locally in memory; entry-set pages get a new
`PageType::kCabin` `[PROPOSED]`, headered, checksummed, but excluded
from redo.

## 10. Catalog `[PROPOSED]`

```
sys.cabins
  cabin_id     u64
  rel_oid      u64
  column_no    u16
  origin       u8      (auto / user)
  status       u8      (active / building / demoted)
  observed_ct  u64     (values currently observed)
```

`CREATE CABIN ON trade(sym)` / `DROP CABIN` for the user path, mirroring
the pattern feature's origin axis; auto-creation arrived 2026-08-10 with
the cabin optimizer (`docs/feat-physical-optimizer.md` Part II), whose
rows carry `origin = auto` — the tag that marks a Cabin the engine may
drop on its own judgement.

## 11. Out of scope / open

- Expression / predicate-scoped cabins (C3 — revisit after v1).
- ~~The `CABIN AUTO` threshold (§8.1)~~ — **consumed 2026-08-10** by the
  cabin optimizer (`docs/feat-physical-optimizer.md` Part II): earning is
  the θ_create margin sustained over N confirm snapshots, un-earning the
  θ_drop cooldown, both against measured scan cost × decayed frequency.
  What stays open moved with it (§II.7's tuning and PHY08's follow-ups).
- Multi-column keys; range observation.
- Budget, per-value caps, demotion of write-hot values (`[OPEN]`, §8).
- Background pruning cadence and batching (§5 fixes the gates;
  scheduling belongs to the background-group policy).
- Promotion thresholds (use_count × cardinality) — belongs to the
  retention/policy spec alongside tracking levels.
- Entry-set page layout details and directory persistence format —
  workplan (the 24 B CabinEntry of §3 is fixed; its packing into
  pages is not).

---

## 12. Cabin classes — Observational and Bound (rev. 2026-08-08, AST01)

This section is the revision `docs/feat-assertion.md` §5 requires (AS6's
prerequisite), and it is **normative for the class boundary only**. Its
authority is derived: where this section and `feat-assertion.md` §5 disagree
on any property in the table below, the assertion spec wins and this file is
wrong — the two are meant to be readable as one.

**One sentence: a Bound Cabin is a Cabin that is required to have observed
everything, forever.** The same relationship `feat-index.md` IX1 already
states for a secondary index ("an index is a Cabin that observed everything"),
with two properties added that an index does not have — the coverage is
*pinned* rather than merely maintained, and each group carries a **running
aggregate** that a check reads instead of iterating the set. The page format
and the lookup machinery are shared; the **lifecycle contracts are not**.

### 12.1 The class table

| Property | Observational Cabin (existing, unchanged) | Bound Cabin (new, AST04) |
|---|---|---|
| Population | Lazy — observed values only (§5's witness, n=2/n=1) | Eager — full coverage of the group-column combination, built at `CREATE ASSERTION` |
| Coverage contract | Partial by design; a value is either observed or it is not | 100% of the target relation's live rows, with no per-value opt-out |
| Eviction | Allowed — §1's corollary, un-observing is always legal | **Forbidden.** Pages are pinned; un-observing is unavailable |
| Failure response | Un-observe and fall back to the scan: a performance event | **Fail the statement.** Same line `feat-index.md` draws for index maintenance, and for the same reason: a Bound Cabin missing an entry is *wrong*, not slow |
| Durability | Unlogged authoritative (§9); memory-resident in v1; rebuilt by traffic after a restart | **Logged, headered authority class** (var-heap V3 / unique-index U5 tier): WAL-before-data, checksummed, crash-consistent, enforceable at restart with no rebuild scan |
| Role | Advisory acceleration — chooses where to look | Authoritative constraint substrate — decides whether a write is admitted |
| Entry size | 24 B (`stats::CabinEntry`, C6) | **32 B** — the same fields plus the row's aggregate value inline |
| Key | One non-pk column's value, held by value | The `GROUP BY` column list, hashed into a group directory |
| Instances | One per `(relation, column)` | One per **assertion** (§5.3); never shared in v1 |

**Observational Cabin semantics are untouched by this revision.** Every
property §§1-11 of this document states — the superset invariant, append-only
maintenance, "removal is forbidden", read-time verification by MVCC plus the
key re-check, caps that refuse to observe and never truncate, un-observing as
the response to any failure, dangling entries droppable on sight (K1), and the
`[PROPOSED]` numbers in §11 — continues to hold verbatim for the observational
class, and no observational code path may consult the class table above. In
particular §11's open item on **persistence of the entry sets** stays open: a
Bound Cabin being logged decides nothing about `PageType::kCabin`, because the
two classes' durability follows from their coverage contracts, not from a
shared implementation choice.

### 12.2 Entry layouts

Observational, 24 B — unchanged, `include/kds/stats/cabin_store.hpp`, pinned
by its own `static_assert`: `pk` (u64) | `page_id` (u32) | `page_epoch` (u32)
| `slot` (u16) | `flags` (u16) | `reserved` (u32).

Bound, 32 B — the observational layout plus one 64-bit field:

| Field | Width | Notes |
|---|---|---|
| pk | 40 bit of a u64 | Keystone id. **Authoritative**, under K1: it may dangle, it can never mis-attribute |
| flags | 8 bit | includes the `RESERVED` bit for an in-flight entry (assertion §6) |
| reserved | 16 bit | written 0, ignored |
| location hint (page id / epoch / slot) | 64 bit | **advisory**, verified through the one verifier `exec/tuple_verify.hpp`; on failure fall back to a pk descent and heal in place |
| aggregate value | 64 bit | the row's `SUM` column value, inline (int64). For a `COUNT(*)` assertion it is written **1** |

The normative facts are: **fixed 32 B, pk authoritative, hint advisory, value
inline.** The exact bit packing is AST04's, and is bound by the engine's
on-disk rules — `static_assert` on size and offsets, fixed-width integers,
explicit shift/mask serialization, no compiler bitfields. Whether the class is
a new `PageType::kCabinBound` or a subtype flag on `PageType::kCabin` is
likewise AST04's decision, to be recorded here when it is made; nothing in
this section depends on the answer.

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
