# CN-3 — Concept Note: Supersede and Chain — id succession over issue-once ids

Status: CONCEPT, not a work order, not a CIP. Nothing here opens a stage.
**No work starts on this note until the operator rules on §10.** The
decisions in §11 are the operator's; they fix the concept's shape and do
not license building it. Ordering relative to other work: **after CN-2**
(`[operator]`, A2); nothing further is fixed.
Author: CLA, 2026-09-10, against `fb1367b`
Origin: operator's verbal concept of 2026-09-10 (the example in §0),
elaborated by CLA over three exchanges; the operator answered CLA's
decision list in full (§11) and corrected two of CLA's recommendations
(B5, D5).
Claim tags: `[operator]` for the operator's statements and decisions;
`[source-read]` with `path:line` at `fb1367b`; everything else
`[design]`. Nothing is `[measured]`.
Relation to `docs/rules/keystoneid-invariant.md`: this note **does not
relax K1 or K2** and depends on both (§1). Relation to CN-2: sibling
concept on the same ground; §7 names the one point where the two must
agree.

---

## 0. The concept, in the operator's example

`[operator]` A tuple can be *retired* instead of deleted, and a retired
tuple can point at a successor. For a relation `user`:

```
RETIRE user ON 1;
INSERT INTO user VALUES (7, 'louis', active) CHAIN ON 1;

SELECT * FROM user WHERE id = 1;   -- tuple 7's columns, pk shown as 1
SELECT * FROM user WHERE id = 7;   -- tuple 7's columns, pk shown as 7
```

Both queries return the same row body. The pk column echoes what was
asked for; every other column is tuple 7's. The use cases named are a
member who leaves and re-registers, and a production row that was
corrupted and must be re-expressed under a new id while the old id keeps
resolving.

`[design]` In one sentence: **retirement is closure rather than
deletion, a chain is a forward pointer from a closed id to a live one,
and pk lookup resolves through the pointer while echoing the id it was
given.** The grammar in this note uses `SUPERSEDE` for the closing verb
(§5, D1); `RETIRE` is kept for the concept in prose only.

## 1. This is not a way around K1/K2 — it stands on them

`[source-read]` `docs/rules/keystoneid-invariant.md:16-22`: K1, an id
is issued to exactly one tuple in the relation's lifetime and never
rebound by any path; K2, a tuple's id never changes.

`[design]` Under this concept tuple 1 remains tuple 1 — closed, with a
pointer — and tuple 7 remains tuple 7. No id is rebound and no id
changes. What changes is what a *lookup by id* returns, which K1/K2 say
nothing about. The concept therefore satisfies both rules rather than
bypassing them, and the operator's framing of it as a way past the
invariant is not carried into this note.

More than that: the concept is **unsound on any engine that does not
hold K1**. If id 1 could be reissued, "1's successor is 7" would collide
with a later, unrelated tuple 1. Issue-once is what makes a chain a
durable fact. CN-2 rests on the same property for the same reason
(`cn2-loose-foreign-keys-and-tuple-completeness.md` §1), and the two
notes are one argument: **this engine treats an id as a permanent
identity, and offers references, succession and completeness as
engine-held facts over it.**

## 2. Definitions

`[design]`

- **Closed tuple** (the operator's *retired*). A tuple that has been
  superseded. It is absent from every scan and every pk lookup that does
  not say `DEEPLY` (S1). Its body is gone (C2); what it holds is its id
  and, if chained, its successor's id.
- **Live tuple.** Any tuple that is not closed and not delete-marked.
- **Chain.** A pointer from one closed tuple to one live tuple in the
  same relation. Depth one (S2): a closed tuple names at most one
  successor, and its successor is live, never itself closed. A live
  tuple has at most one predecessor (S3).
- **Alias.** An id that resolves to a tuple other than its own — a
  closed id with a chain. `where id = 1` under a chain 1→7 is an alias
  lookup.
- **Resolution.** The step, at pk lookup, that follows a chain and
  returns the successor's body under the asked-for id. Resolution is
  **read-only** (S4) and is switched off by `DEEPLY` (S5).
- **"Retire" the word.** `heap_page.hpp` uses *retire* for physical slot
  reclamation — `RetireSlot()`, "physically retired"
  (`include/kds/storage/heap/heap_page.hpp:261,295-297`, `[source-read]`).
  The logical operation here is therefore not spelled `RETIRE` in
  grammar or code (D1); it is spelled `SUPERSEDE`, and this note says
  *closed* where it means the logical state.

## 3. The model — a resolution layer in front of pk lookup

`[design]` Everything below the resolution layer is unchanged: tuples,
the Keystone word, MVCC, undo. The layer does three things and nothing
else:

1. **Hides closed tuples.** A scan skips them; a pk lookup that lands on
   one with no chain answers *absent*; a pk lookup that lands on one
   with a chain follows it.
2. **Echoes the asked-for id.** The resolved body is returned with the
   lookup key in the pk column. Under `where id = 1` the row reads
   `(1, 'louis', active)`; under `where id = 7` it reads
   `(7, 'louis', active)`. The row body is the same physical tuple.
3. **Deduplicates by physical tuple.** `where id IN (1, 7)` returns one
   row (S5). The id echoed is the live one, 7, because it is in the
   requested set; a request naming only aliases echoes the alias.

`SELECT DEEPLY` switches the layer off (S5, D5): closed tuples appear as
rows, no chain is followed, nothing is deduplicated. `DEEPLY` is a
`[provisional]` keyword.

Writes do not pass through the layer (S4). `UPDATE ... WHERE id = 1`
and `DELETE ... WHERE id = 1` under a chain 1→7 are refused with a
non-retryable error, never applied to 7.

## 4. Semantics — as the operator decided them

`[operator]` unless marked. The letter codes are §11's.

- **S1 — Closed tuples are absent** from scans and from plain pk lookup.
  A closed id with no chain answers *no row*. Reading a closed tuple as
  itself is `DEEPLY`'s job and nothing else's.
- **S2 — Depth one.** One closed tuple, one successor, and the successor
  is live. Superseding 7 later does not carry 1 to 7's successor: 1 then
  points at a closed tuple, and `[design]` a resolution that lands on a
  closed successor answers *no row*, exactly as an unchained closed id
  does. Transitivity is a later decision, not this note's.
- **S3 — No merging.** A live tuple has at most one predecessor. A
  second `CHAIN ON` naming a successor that already has one is refused.
- **S4 — Aliases are read-only.** A write whose key resolves through a
  chain is refused, non-retryable (D4). `[design]` The refusal names the
  live id in its message so the client can retarget.
- **S5 — `IN (1, 7)` is one row; `DEEPLY` opts into the closed one.**
  Deduplication by physical tuple; live id echoed when present in the
  request. `SELECT DEEPLY` returns two rows, the closed 1 and the live 7,
  with no resolution.
- **S6 — Foreign keys resolve through a chain.** A child referencing 1
  under a chain 1→7 has a present parent for the forward check and is a
  child of 7 for the reverse check. This is the concept's central value
  (§8) and is v1. CN-2's completeness verdict follows the same rule (§7).
- **S7 — A closed tuple's values are outside uniqueness.** `[design]`
  **Forward-looking at `fb1367b`**: the engine has no unique index
  (`docs/spec/index.md:375`, "There is no unique index (IX11)",
  `[source-read]`), so today the only unique key is the pk, which S7
  cannot affect. The decision is recorded so that a unique index, when
  built, does not have to rediscover it.
- **S8 — Re-pointing is allowed.** `INSERT ... CHAIN ON 1` when 1
  already chains to 7 moves 1's pointer to the new row; 7 stays live and
  unchained (S9 forbids touching it). Unchaining — clearing the pointer
  without a new successor — is not v1.
- **S9 — Preconditions.** `CHAIN ON x` requires x closed and the new
  row live. Live→live is refused; closed→closed is refused; a chain onto
  a tuple that is itself the target of a chain is refused (S3).

## 5. Surface

`[operator]` D2, D3; `[design]` the spellings.

```
SUPERSEDE <rel> ON <pk>;                     -- close pk; no successor
INSERT INTO <rel> VALUES (...) CHAIN ON <pk>; -- new row succeeds pk
INSERT INTO <rel> (cols) VALUES (...) CHAIN ON <pk>;  -- id omitted, D3
SELECT DEEPLY ... FROM <rel> ...;            -- resolution off, closed rows in
```

Two statements, not one (D2): `SUPERSEDE` alone leaves a closed,
unchained tuple, and that is a normal end state (S1), not an
intermediate one. The two need not share a transaction; a client that
wants atomicity wraps them.

`[design]` Errors, two new codes, both `retryable=0`, names provisional:
`ERR ALIAS_READONLY` for S4; `ERR CHAIN_REFUSED` for S3/S9, with the
reason in the message. Superseding a pk that does not exist, or is
already closed, is `ERR CHAIN_REFUSED` too.

`[design]` Observability is `DEEPLY` (D5): a closed tuple's row under
`SELECT DEEPLY` carries its id and, if chained, its successor's id. How
the successor id is exposed is §10-2 — a hidden column visible only under
`DEEPLY` is CLA's proposal, and the column name is unchosen.

## 6. Storage shape

`[operator]` C1–C3 accepted CLA's recommendation; `[design]` the detail.

- **The pointer lives in the closed tuple** (C1), not in a side catalog.
  A closed tuple's columns no longer mean anything (C2), so the body is
  the room. A relation that never supersedes anything pays nothing —
  no catalog probe on lookup, no new page class — which is the
  zero-overhead rule for `cores = 1` applied to a feature instead.
- **The closed bit is a Keystone flag.** The Keystone word carries 8
  flag bits beside the 40-bit id (`include/kds/storage/keystone.hpp:37-38`,
  `[source-read]`). One of them says *closed*. Which bits are taken today
  is not surveyed here; the note assumes at least one is free and §10-3
  asks.
- **The body is gone at supersede** (C2). What existed before the
  `SUPERSEDE` is reachable only through undo, by a snapshot that
  predates it, and only until the purge takes it. A product document
  states this; a client that needs the last body copies it first.
- **`SUPERSEDE` and `CHAIN ON` are ordinary MVCC writes** (C3): a
  `trx_id` stamp and an undo record, like any overwrite. Snapshot
  semantics follow for free — a snapshot before the supersede sees the
  open tuple 1, a snapshot after it sees 1→7 — and rollback is the
  existing compensation path. Nothing new is invented for either.

## 7. Placement against existing structures

`[source-read]` at `fb1367b`, named to place the concept:

- **K1/K2** (`docs/rules/keystoneid-invariant.md:16-22`): satisfied, and
  required (§1).
- **Where an id comes from** (`docs/spec/heap-and-tuple.md:109,309`):
  the `INSERT` names the id or omits it, per row; a named id below the
  mark is admitted on a btree relation and refused on a heap one. The
  operator's example names `7`; `CHAIN ON` with an omitted id (D3) is
  the general form.
- **The Keystone word** (`include/kds/storage/keystone.hpp:37-38`): the
  8 flag bits §6 spends one of.
- **Foreign keys** (`docs/spec/foreign-keys.md:17` F1; `:62` F6; `:437`
  §4): S6 means `CheckParentPresent` treats a chained closed parent as
  present and `CheckNoChildReferences` treats a child of an alias as a
  child of the successor. **The one point CN-2 and CN-3 must agree on**:
  CN-2's completeness verdict for a child referencing a closed-and-
  chained parent is *complete*; for a closed-and-unchained parent it is
  *incomplete*. CN-2 §8 does not yet carry this item; it should, marked
  as owed to this note.
- **The reverse check's Cabin** (F6): an observed-value Cabin on the fk
  column would see the child's stored value `1`, and the reverse check
  for a delete of 7 must ask about 7's predecessors as well. Depth one
  bounds that to one extra key; `[design]` how the Cabin learns the
  chain is a §10 item.
- **Waystone and the trail** (`docs/spec/parser-v2.md` I12, `docs/spec/waystone-concpets.md:35`):
  a pk lookup may be served from the trail because completeness follows
  from pk uniqueness. That remains true — the trail leads to tuple 1's
  physical location, and resolution runs after the tuple is in hand.
  Resolution is therefore a step *after* the lookup, never a change to
  what the lookup finds.
- **Physical retire** (`heap_page.hpp:261,295-297`): unrelated. A closed
  tuple occupies its slot like any live one; the purge that would free
  it is not this note's.
- **`docs/spec/index.md:375`**: no unique index exists; S7 is recorded
  for the one that may.

## 8. Practicality and commercial position

`[design]` The problem is real and universally solved in the application
layer: a `deleted_at` column plus a `merged_into` or `successor_id`
column, `WHERE deleted_at IS NULL` on every query, and alias resolution
in an ORM hook or a service method. CRM and MDM products build entity
merge and golden records on top of the same pattern.

What an engine-held chain does that the application pattern cannot:

1. **Resolution cannot be forgotten.** The application pattern's
   recurring defect is one query path that skips the alias step. When
   resolution is the pk lookup itself, that class of defect does not
   exist.
2. **Foreign keys survive succession** (S6). Millions of child rows
   referencing 1 need no rewrite when 7 succeeds it; under the
   application pattern they must be rewritten or the FK dropped.
3. **The chain is snapshot-consistent** (C3). "Before the supersede"
   and "after" are one MVCC boundary; the application pattern has no
   such instant.
4. **It is defensible.** Without issue-once (§1) the feature is unsafe,
   so an engine that reuses keys cannot copy it as a flag.

What weighs against it:

- **A large user-visible surface.** §4 is nine semantic rules, each of
  which a user can meet. A wrong default is a quiet-wrong in the user's
  eyes — "the engine showed me a different row" — and quiet-wrong is
  this project's primary named threat.
- **Foreign tools.** ORMs, reporting and BI tools do not know a row
  with pk 1 is physically tuple 7. `JOIN` on an alias, `GROUP BY id`,
  `COUNT(DISTINCT id)` each need a stated answer, and having one does
  not stop a tool being surprised.
- **Regulatory friction.** In the re-registration use case, that a
  closed id still resolves to a person may itself be the problem in
  some jurisdictions. C2 removes the body but not the link. The
  product document states the limit; the engine does not solve it.
- **Breadth versus team.** The feature crosses the parser, the
  executor, both FK checks, the Cabin, the trail and MVCC. Alone it is
  not a purchase driver.

**Where it earns its place** is as one of three concept notes with one
argument — CN-1's operable surface, CN-2's engine-held references,
CN-3's engine-held succession — under the claim that *an id here is a
permanent identity*. Buyers for whom that claim matters: account and
customer-master systems, catalogs that merge products, domains where
"close, never delete" is already policy. Buyers for whom it does not:
generic web back ends behind an ORM, and anyone under a physical-delete
mandate.

## 9. What this note does not license

- No code: not the flag bit, not `SUPERSEDE`, not `CHAIN ON`, not
  `DEEPLY`, not the two error codes.
- No change to `docs/rules/keystoneid-invariant.md` or
  `docs/spec/foreign-keys.md`. S6 is a proposal against F1/F6, not an
  amendment.
- No claim on a Keystone flag bit. §6 assumes one is free; §10-3 is
  where that is checked before anything is written.
- No amendment to CN-2. The item §7 says CN-2 owes is recorded here for
  the operator to carry across.
- No work order, no ordering beyond "after CN-2" (A2).
- No measurement. Resolution's cost on a pk lookup is one flag test on
  the tuple already in hand plus, for an alias, one more lookup;
  `[design]`, not `[measured]`.

## 10. Operator decisions this note leaves open

1. **Go / no-go**, and if go, when — A2 fixes only "after CN-2".
2. **How `DEEPLY` exposes the successor id**: a hidden column visible
   only under `DEEPLY` (CLA's proposal; name unchosen), or a separate
   command. D5 chose the `SELECT` option over `SHOW`; the column form is
   the consequence and needs a yes.
3. **Which Keystone flag bit** is free for *closed*, surveyed against the
   tree at the time of any work order — not assumed from this note.
4. **The verb.** `SUPERSEDE` is CLA's recommendation accepted by default
   (D1). If the operator prefers another word, only §5 changes.
5. **How the Cabin on an fk column learns a chain**, so the reverse
   check for a successor's delete asks about its predecessor (§7). Depth
   one keeps this to one extra key per closed parent; the mechanism is
   unchosen.
6. **Aggregate and join semantics under resolution**: `GROUP BY id`,
   `COUNT(DISTINCT id)` and `JOIN ... ON child.user_id = user.id` when
   `user_id` holds an alias. §3's three rules answer point lookups; these
   need a stated answer before a work order, and CLA's proposal is that
   the join resolves (a child of 1 joins to 7's body) and aggregates
   count physical tuples.
7. **Transitivity later** — whether S2's depth one is a v1 limit or the
   permanent rule. Recorded so the flag bit and the pointer format are
   not chosen in a way that forecloses it.

## 11. Operator decision record — 2026-09-10

`[operator]`. CLA's recommendation is shown where the operator's answer
differed from it.

| code | question | operator's answer |
|---|---|---|
| A1 | independent note or a CN-2 section | independent (CN-3) |
| A2 | ordering | after CN-2; nothing else fixed now |
| A3 | primary use cases | re-registration and correction; merge is not v1 |
| B1 | closed tuple under plain lookup/scan | absent; a separate read surface only |
| B2 | transitivity | depth one — one closed tuple, one successor |
| B3 | many closed → one live | refused |
| B4 | writes through an alias | read-only; refused |
| B5 | `IN (1, 7)` | **one row, tuple 7** (CLA had recommended one row per requested id); closed rows opt-in via `SELECT DEEPLY` |
| B6 | FK resolves through a chain | yes |
| B7 | closed values under uniqueness | excluded |
| B8 | re-pointing a chained id | allowed |
| B9 | preconditions: target closed, successor live | both required |
| C1 | pointer in the closed tuple vs side catalog | in the tuple (CLA's recommendation) |
| C2 | body at supersede | gone (CLA's recommendation) |
| C3 | supersede/chain as ordinary MVCC writes | yes (CLA's recommendation) |
| D1 | verb, given `RetireSlot` | CLA's recommendation: `SUPERSEDE` |
| D2 | one statement or two | two (CLA's recommendation) |
| D3 | `CHAIN ON` with an omitted id | allowed |
| D4 | error for alias writes | refused, non-retryable |
| D5 | observability: `SHOW` vs a `SELECT` option | **a `SELECT` option (`DEEPLY`)** (CLA had recommended `SHOW`); DEEPLY = closed rows included, no resolution — confirmed |
| — | pk echo under plain `where id = 1` | tuple 7's body, pk shown as 1 — confirmed |
| — | re-pointing leaves the prior successor live and unchained | accepted by default (no objection) |
