# Auxiliaries under a split — workplan

Tasks SA-T0..SA-T9 for `instructions/v2.7.1/workorder-sa.md`. Surveyed and
built on the `xf` worktree; every path:line below is `1b27d68`
(`v2.7.0-28-g1b27d68`) unless a row says otherwise.

| row | status |
|---|---|
| SA-T0 | **Built** at `1beda80` — a participant that wrote nothing writes no `TXN_PREPARE`. Spec: `cross-owner-txn.md` §1a |
| SA-T1 | **Built** at `1b27d68` — `exec::RestructureForExecutingCore`, both remote shapes, correlated arms included |
| SA-T2 | **Ruled 2026-09-01 and built as work order SB** (`instructions/v2.7.1/workorder-sb.md`). §2's proposal was ratified as SB-R1; §2.6 records what building it found. |
| SA-T3, T5, T7, T8, T9 | not started |
| SA-T4 | not started. The only trace in the tree is a comment (`src/server/shipped_statement_executor.cpp:704`) |
| SA-T6 | **Blocked on its own gate, and its two hazards are pre-emptively closed.** §3 below |

---

## 2. SA-T2 — the gate rewrite, and what the survey found under it

### 2.1 What the order asks for

Three parts (`workorder-sa.md`, SA-T2, ratified by SA-R2/R3/R4):

1. `RangeEligible`: `indexed` → `UNIQUE-indexed`; `cabined` → `Bound`-cabined.
2. Split and CC10 migration perform the **Observational discard** (SA-R3),
   logged, with a `SHOW META` counter.
3. `RefuseAuxiliaryOnSplitRelation` drops its `CREATE CABIN` arm, "because
   observation is per-owner and needs nothing global".

### 2.2 Parts 1 and 3 of the gate are cheap and the survey confirms them

**The index arm.** `catalog::TableAccess::IndexRef`
(`include/kds/catalog/schema.hpp:481-501`) carries `index_oid`,
`root_page_id`, the two widths, the key and covered column arrays — and
**no `unique` flag**, because IX11 is unbuilt. So the narrowed gate admits
every indexed relation, exactly as SA-R2 predicted. The arm becomes a
comment naming where it returns rather than a test of a field that does not
exist.

**The two Cabin classes are already two different gates**, which makes
SA-R4 a deletion rather than a rewrite. `RangeEligible`
(`src/exec/range_eligible.cpp:21-60`) has `kCabin`, which asks
`access.AnyCabin()` — the *Observational* class, a catalog fact — and
`kAssertion`, which asks `enforcer.AnyOn(oid) || enforcer.CannotEnforce(oid)`
and whose own comment says it means "**a Bound Cabin exists whose chain is
one core's**". SA-R4's "the gate applies to the Bound class alone" is
therefore satisfied by removing the `kCabin` arm and keeping `kAssertion`
untouched. The order's parenthetical — "the enforcer answers the class, not
`TableAccess`" — is already true of the arm that survives.

### 2.3 The finding: the discard is necessary and **not sufficient**

**The hole the gate is holding shut.** A Cabin is authoritative for an
observed value: "its entry set for an observed value is a **superset** of
the qualifying pks, and the read subtracts the surplus"
(`include/kds/exec/step_chain.hpp:87-92`), and "an observed value's *empty*
set is an authoritative 'no rows'". A superset is safe — the read
re-filters. A **subset is a wrong answer**, and an empty subset is a
confident one.

**A split turns a set into a subset, and nothing in the write path
notices.** `CabinStore::NoteWrite` appends to a value's set "**if that
value is observed**" (`include/kds/stats/cabin_store.hpp:366`), and it runs
on the core performing the write. After the relation splits, rows inserted
into the new range are written by the *peer*, whose `CabinStore` holds no
observation for that value — so the entry goes nowhere, and the original
observer's set silently stops covering the relation. The next probe served
from it returns short.

That is why the gate exists, and it is why **the discard has an ordering
requirement the order does not state**: dropping the sets *after* the peer
can write is too late. The peer can write once CC10 grants it the lease, so

> **the Observational discard must complete before CC10's grant, not after
> the boundary is published.**

This is the same shape SA-T7 was given for the Bound Cabin — "between step
3 (durable directory row) and step 4 (grant)" — and it lands the two
classes on one rule. `CabinStore::Forget(cabin_id)`
(`cabin_store.hpp:355`) is the discard itself and is documented "always
legal"; what SA-T2 must build around it is a **broadcast that every core
has acknowledged before the grant goes out**, because a set may live on any
core that has read the relation (under `peer_listeners = on`, that is any
core).

### 2.4 And part 3 needs a second thing the order states as already true

SA-T2 admits `CREATE CABIN` on a split relation "because observation is
per-owner and needs nothing global". **A cabin created after the split has
the same subset problem as one banked before it**: a core observes a value,
banks a set from what it can see, and a peer's write to a range that core
does not own is never appended. The discard does not help — there is
nothing stale to discard; the set is born incomplete.

So "observation is per-owner" is not a description of today's behaviour. It
is a **change to what a Cabin's entry set claims authority over**, and it
has to be written into `cabin.md` §7's serve rules before it is true.

**CLA's proposal, offered under SA's standing instruction rather than
assumed:**

> A Cabin's entry set is authoritative for **(observed value × the ranges
> its core owns)**. A probe resolves the ranges it needs through the range
> directory; ranges the serving core owns are answered from the set, and
> any range it does not own falls through to that range's own stage — which
> is the fan-in the read surface already opens. A relation of one range is
> the case that exists today and is unchanged, byte for byte.

That keeps the authority claim true rather than narrowing it by convention,
and it uses the unit SA already works in — the range and its owner. It also
makes §2.3's discard *only* a transition rule: sets banked when the relation
was whole claim authority over ranges their core no longer solely owns, so
they go; everything banked afterwards is scoped correctly by construction.

**Why this is surfaced rather than built.** It changes an authority
statement, and a Cabin that is wrong is wrong *quietly* — an authoritative
"no rows" that is false leaves nothing in a log and nothing in a test that
does not already know to look. The gate lift and this scoping have to land
together or neither: lifting `kCabin` without it converts a refusal into a
wrong answer, which is the one trade this engine's rules never make.

### 2.5 What SA-T2 becomes

- **T2a** — `RangeEligible`: drop the `kIndex` and `kCabin` arms, keeping
  both enum values and naming where each returns (IX11 for the first,
  never for the second). Small, and **gated on T2c/T2d landing with it**.
- **T2b** — the per-owner authority scoping (§2.4), in `cabin.md` §7 and
  the serve path. **Needs the operator's eye on §2.4's proposal**, because
  it is an authority change and not a mechanism.
- **T2c** — the discard, ordered **before CC10's grant** (§2.3): the
  broadcast, its acknowledgement, `Forget` on each core, the decline/discard
  counter in `SHOW META`, and the log line.
- **T2d** — `RefuseAuxiliaryOnSplitRelation`
  (`src/catalog/catalog.cpp:1094-1110`) drops its `"a Cabin"` arm
  (`:2762`), manual and optimizer paths alike. Gated on T2b.

Nothing in T2 is landed until T2b's answer exists, because every part of it
is a step toward serving a Cabin on a split relation and the serving rule is
the part that is open.

---

## 3. What the 2026-08-31 spreading amendment changes here

`instructions/v2.7.1/amendment-spreading-per-relation.md` makes insert
spreading a per-relation option shipping **off**. It changes SA's urgency
and none of its content: every gate SA narrows is still reachable the
moment a relation asks to spread, and SA-R2's "today the narrowed gate
admits every indexed relation" holds unchanged. What it does change is the
**consequence of getting §2.3 or §2.4 wrong** — a wrong Cabin answer would
now reach only relations whose owner opted into spreading, rather than
every relation on a multi-core instance.


### 2.6 Ruled, built, and what building it found `[2026-09-01 — work order SB]`

§2.4's proposal is ratified verbatim as **SB-R1**: an Observational
Cabin's entry set is authoritative for (observed value × the ranges its
core owns). SB-R2 ordered the discard before CC10's grant, SB-R3 made the
scoping, the discard and the two gate drops one merge, and SB-R5 kept the
Bound class and migration out. T2a/T2b/T2c/T2d landed together;
`docs/spec/cabin.md` §4b and `crosscore.md` CC10/§6a are where the rules
now live.

**Two findings from the code, each of which contradicts a premise this
survey stated, and both are why the built shape differs from the order's
letter.**

**Finding A — there is one Cabin store, and it is core 0's.** §2.3 said "a
set may live on any core that has read the relation (under
`peer_listeners = on`, that is any core)". That is false against the tree:
`Expeditor::cabin_store_` is the only `stats::CabinStore` the engine
constructs; every peer dispatcher passes `/*cabins=*/nullptr`
(`src/server/core_runtime.cpp`) and so does every fan-in stage
(`src/server/remote_step_service.cpp`, three sites). `known-gaps.md`
already recorded the same fact from the other direction — the four
unbroadcast catalog relations are safe "only because a peer's dispatcher
is built with no recorder, no replay, no access statistics and no cabins".
So SB-R2's acknowledgement set is one core, and that core is the one
performing the split. The operator ruled on 2026-09-01 that the discard is
therefore a direct core-local `Forget` before the grant — no window at
all, rather than a window closed by acknowledgements — with the broadcast
named in `cabin.md` §4b and CC10 as what the obligation becomes the day a
peer or a stage holds a store.

**Finding B — a Cabin probe on a split relation is unreachable, before
the lift and after it.** A relation with two or more owners is never read
locally: `HandleSelect`'s fan-in route is taken exactly when
`TableAccess::ServableBy(core_id)` is false, and `CheckReadAffinity`
refuses every shape the fan-in gate will not admit. Every stage of that
fan-in — core 0's own included, since a run of ranges the reader owns
becomes a self-directed stage — runs with no Cabin store. So dropping the
two gates cannot produce the wrong answer §2.3 feared, and it cannot
produce a saving either. That is worth stating twice, because it changes
what the bundle *is*: it is a correctness statement made structural and a
refusal removed, not an acceleration. `SHOW META`'s
`cabin_scope_fallthroughs` counts it rather than leaving it to be
inferred.

**What Finding B does not excuse.** The serve-site predicate is built
anyway, and deliberately: today it is true because of a router two
functions away, and `schema.hpp`'s own rule — "a predicate correct only
because of a neighbouring invariant is what this line refuses to be" — is
what the scoping is for. The day a stage is given a store, the predicate
is what keeps the answer right; without it the same day is a silent
subset.

**One more site of the same shape, found by SB's review and left gated.**
`exec::CheckNoChildReferences` (`src/exec/fk_check.cpp`) returns an
authoritative `FkVerdict::kPass` from an exhausted Cabin entry set with
**no scope check**, and its fall-back walks `heap::ChainVisit(store,
child.desc_page_id, ...)` — the same `desc_page_id`-only walk SB had to
fix in `BuildSeededSets`, which on a split heap relation covers the
`lo = 0` range and stops. Both are unreachable because `RangeEligible`'s
`kForeignKey` arm gates a split in either direction, and **both become
live the moment SA-T6 lifts it**. SA-T6 therefore owns two changes it did
not know about: the serve needs §4b rule 3, and the walk needs
`WalkHeadsFor`.

**Still open here, unchanged by SB:** SA-T3 (per-owner index builds),
SA-T5a (the router), SA-T6 (FK, and the two above), SA-T7 (the Bound
Cabin's migration, and with it the Cabin bullet's surviving migration
gate), and **D1**, which is where `RangeEligible`'s index arm returns —
not IX11, which shapes the re-added arm rather than triggering it (the
review's C1: the arm was dead code, since IX3 makes an index btree-only
and D1 declines every btree relation first).


## 3. SA-T6 — surveyed 2026-09-01, blocked, and defused in advance

### 3.1 Both halves are gated, and the gate is real

`workorder-sa.md` states SA-T6's gate as **SA-T4, SA-T5**. Neither is
started: the only occurrence of either in the tree is a comment at
`src/server/shipped_statement_executor.cpp:704` saying where SA-T4's
intents will go. SA-T6 has two halves and the gate binds both:

- **The `CREATE`-time pair.** `catalog::CheckForeignKeyColocation`
  (`src/catalog/foreign_key.cpp:31`) refuses `parent.owner_core !=
  child.owner_core`. SA-R6 relaxes F5 from "same core" to
  "2PC-reachable owner" — but SA-R6's own sentence is *"The correctness
  path is SA-T4/T5's protocol."* Admitting the pair without that
  protocol declares a constraint whose forward check
  (`exec::CheckParentPresent`) does a **local** `BtreeLookup` on
  `parent.desc_page_id` with no ownership question anywhere in it. On a
  foreign parent that is a page this core may not fault: the check
  fails or faults rather than answering, and no path exists that answers
  it correctly. **The reverse direction has the same shape and the guard
  built below does not cover it**: the guard is keyed on
  `!child.ranges.empty()`, so an *unsplit* child owned by another core is
  walked at `desc_page_id` with no scope question asked. That is correct
  today because F5 forces parent and child onto one core; the day SA-R6
  relaxes it, SA-T4/T5 owe both directions, not just the forward one.
- **The split gate.** `RefuseAuxiliaryOnSplitRelation`'s FK arm (both
  ends, `src/catalog/catalog.cpp`) and `RangeEligible`'s `kForeignKey`
  arm. What lifting these costs is §3.2, and it is worse than a fault.

**Nothing in SA-T6 is buildable today**, and the order says so itself.
This entry records that rather than leaving the next reader to re-derive
it.

### 3.2 What the split gate was holding shut — measured by breaking it

SB's review named two sites; building the guards proved both, by
reverting each and watching a cell fail. Neither is a hypothetical.

**(1) The reverse check's walk covered one range and reported success.**
`CheckNoChildReferences` walked `heap::ChainVisit(store,
child.desc_page_id, ...)`, and since RD6 a split heap relation is **one
chain per range** — so `desc_page_id` is the `lo = 0` chain and nothing
else. With a referencing child row in the second range, the reverse
check found nothing, answered `FkVerdict::kPass`, and the parent DELETE
replied **`DELETED 1`**. A dangling foreign key, the constraint
reporting success, nothing logged. `foreign-keys.md` §1 already names
this class: *"A constraint that silently does not run is not a degraded
mode."*

**(2) The Cabin serve had no scope check.** An exhausted entry set
returns an authoritative `kPass` — which is the whole of F6 — but since
SB-R1 a set speaks for *(observed value × the ranges its core owns)*
(`cabin.md` §4b). On a child whose ranges are not all this core's, an
exhausted set means "no children **here**", and answering `kPass` from
it drops the constraint the same way (1) does.

### 3.3 What was built instead, and why it is not SA-T6

Both are closed **while the gate is still up**, which is the same order
SB took with `BuildSeededSets`: fix the hazard on the safe side of a
refusal, so lifting the refusal later is a decision about capability
rather than a bet on correctness.

- **One scope refusal, asked before the Cabin and before the walk**
  (`src/exec/fk_check.cpp`): if the child has a directory and is not
  `ServableBy(options.core_id)`, the check returns `NotImplemented`
  naming SA-T5's fan-out. `ServableBy` is the read surface's own
  predicate and `step_vm.cpp`'s, not a third spelling. **A refusal, not
  a partial answer**, because RESTRICT's contract is an authoritative
  "no children".
- **`WalkHeadsFor(core_id)` for the walk**, so a child that *is* wholly
  this core's is covered across every chain it has. The btree arm keeps
  `desc_page_id` and says why: D1 declines every btree relation a
  directory, so a btree child never has one.
- `FkReverseOptions` gains `core_id`, set from `CommandDispatcher::core_id_`.

Behaviour on every shipped shape is unchanged — an unsplit child has no
directory, `WalkHeadsFor` answers the one head it always did, and the
refusal is unreachable. **Three cells** pin it
(`tests/foreign_key_test.cpp`), one per hazard plus the ordering
between them, and each was verified to fail with its own guard reverted
or moved:

- `AChildInASecondOwnedRangeStillBlocksTheParent` — the walk hazard. With
  the walk reverted to `desc_page_id`, the parent DELETE answers
  `DELETED 1`.
- `ACabinCannotAnswerForAChildRangeOnAnotherCore` — the Cabin hazard,
  **added by review**: the first two cells declared no Cabin, so both
  reached the guard through the walk, and §3.2's hazard (2) was closed by
  the code and proven by nothing. Moving the guard *below* the Cabin serve
  left the whole suite green, which is what a missing cell looks like.
- `AChildRangeOnAnotherCoreRefusesRatherThanPassing` — the refusal itself,
  and that it is a refusal rather than a deletion reporting an error.

A fourth thing the review established and this file records because it is
easy to mistake for an optimisation: the loop's `break` on
`verdict != kPass` is **load-bearing**. Without it a later chain's
`kBusy` would overwrite an earlier chain's `kViolation`, turning a
`FK_VIOLATION` into a retryable `TXN_CONFLICT` — a wrong code, not a slow
one.

**This is not SA-T6 and must not be counted as it.** SA-T6 is the two
gates coming down; this is the work that makes taking them down a
question about SA-T4/T5 rather than about whether foreign keys still
enforce.

### 3.4 One site deliberately left

`CheckParentPresent`'s **heap** arm walks `parent.desc_page_id` with the
same one-chain exposure. It is gated by a different and *permanent*
refusal — a heap parent is `Unsupported` at declaration, which SA-T6
explicitly leaves unchanged ("Heap-parent `Unsupported` unchanged") —
and closing it would mean threading a core id through a signature for a
path the DDL forbids. Named here so it is a decision rather than an
oversight; it becomes live only if heap parents are ever admitted, which
is `foreign-keys.md`'s own open item.

### 3.5 Named, not grown: the seventh split-a-relation test helper

`SplitChild` in `tests/foreign_key_test.cpp` is the seventh spelling of
the same three calls (`CreateRangeEntryPage` → `OpenRangeRows`, plus the
oid lookup): `SplitRelation` in `tests/cabin_contract_test.cpp`, `SplitAt`
in `tests/range_chain_test.cpp`, and inline copies in
`tests/cabin_optimizer_exec_test.cpp`, `tests/relayout_planner_test.cpp`
and four sites in `tests/core_runtime_test.cpp`. Consolidating it is out
of this change's scope and is recorded here so the next one does not add
an eighth silently; `SplitChild`'s `catalog::Catalog&` signature is the
most reusable of the set and is the shape to lift.
