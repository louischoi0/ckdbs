# Cabin — value-observed authoritative metadata store (spec, v1)

Status: **DECIDED** — C1–C5 fixed; items marked `[PROPOSED]` / `[OPEN]`
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
the pattern feature's origin axis; auto-creation arrives with the
promotion pipeline (§7). Details deferred to a workplan.

## 11. Out of scope / open

- Expression / predicate-scoped cabins (C3 — revisit after v1).
- Multi-column keys; range observation.
- Budget, per-value caps, demotion of write-hot values (`[OPEN]`, §8).
- Background pruning cadence and batching (§5 fixes the gates;
  scheduling belongs to the background-group policy).
- Promotion thresholds (use_count × cardinality) — belongs to the
  retention/policy spec alongside tracking levels.
- Entry-set page layout details and directory persistence format —
  workplan (the 24 B CabinEntry of §3 is fixed; its packing into
  pages is not).
