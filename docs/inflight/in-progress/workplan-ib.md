# Work order IB — the secondary index lands on a heap relation

Workplan for `instructions/v2.7.2/index.md` (work order IB), carried into
v2.8.0 by `instructions/v2.8.0/ratification-ae.md` AE-6, which keeps IB
as a subject rather than a casualty of the SA/SB withdrawal: it is a
single-relation feature and §13's per-range-vs-global question does not
arise at one range per relation.

Surveyed and built on `worktree-v2.8.0-ratification-ae`; every
`path:line` below is `65338a3` (`v2.7.0-75-g65338a3`) unless a row says
otherwise.

| row | status |
|---|---|
| IB0 | **Done** — the rulings land as text, four of the six amended by what the survey found (§1) |
| IB1..IB5 | not started |

---

## 1. IB0's survey — what the six rulings look like against the code

The work order asked for IB-R1..R6 to be recorded "as ratified or
amended". Four are amended, and none of the four is a wording change:
each is a claim the order made about this engine that the engine
answers differently.

### 1.1 IB-R3 is not a new property — the btree arm already has it

IB-R3 asks the planner not to claim key-sortedness from a heap-backed
probe. **The btree arm already grants no key order either.** The probe's
phase 1 collects pks in *index key* order and then sorts them into **pk**
order before resolving (`src/exec/step_vm.cpp:1341-1353`), and the
comment there states why in the strongest terms available: without the
sort "creating an index would reorder a reply", which is an accelerator
changing a query result.

So IB-R3 is **ratified as a restatement, not as a new fence**. What
changes for the heap arm is only the reason: the btree arm emits in pk
order because it sorts and then descends per pk, the heap arm because a
chain walk *is* pk order (`heap-and-tuple.md` §3.1's page ordering). The
amendment says both, because a reader who meets IB-R3 as a heap-only rule
would reasonably infer that the btree arm grants sortedness — and it does
not.

### 1.2 IB-R1's "bounded walk" is bounded on one side only

The order's mechanism sentence — "resolves it with one walk bounded to
`[min(pk), max(pk)]`" — reads as a two-sided bound. The walk it would use
prunes **the tail and not the head**, and `RunWalkStep` says so at
`src/exec/step_vm.cpp:1664-1681`: a page whose `min_key` exceeds the high
bound ends the walk, while "a page whose `min_key` is below `low` may
still hold qualifying rows, and nothing here can tell without looking".
The head seek that fixes this for a range predicate is
`btree::BtreeSeekLeaf`, and it is explicitly btree-only
(`:1850-1862`): **"A heap relation still starts at the head, and that is
not an oversight: it has no index to descend, so finding the low bound
*is* the walk."**

A secondary index does not change that. IB gives a heap relation an index
over *some other column*; it still has no **pk** index, which is what a
head seek would need. So:

> **IB-R1 amended**: the resolution is one chain walk **tail-bounded at
> `max(pk)`**. `min(pk)` bounds nothing — it is carried by the residual
> like every other range bound, and skipping the pages before it needs a
> structure this relation does not have.

The refutation of IX3's cost sentence survives the amendment intact: "one
full scan into N partial ones" becomes **one** scan, which is the whole
claim. What does not survive is any expectation that a selective probe
near the end of the relation is cheap. IB1's matrix must therefore
control **where in the pk space the matching rows sit**, not only how
many there are — a cell the order did not ask for and that would
otherwise average the two cases into one meaningless number.

### 1.3 The mechanism's real saving is decode avoidance, and it was unnamed

IB-R1 justified itself on page count, which §1.2 has just cut back to the
tail. The saving that does not depend on where the rows sit was not
stated in the order at all:

- A **filter scan** evaluates the predicate against every live tuple,
  which means decoding the predicate's columns out of every row.
- A **pk-set walk** reads the Keystone id alone — `KeystoneIdOfPayload`
  on the raw payload, the same call the key-order emitter already makes
  at `src/exec/step_vm.cpp:1806-1811` — tests membership in the sorted
  set, and **decodes only the rows that match**.

At selectivity 1-in-1000 that is a thousandfold reduction in row decode
against an unchanged page count. It is also the honest reason the probe
can win on a relation where the tail bound saves nothing, and it is what
IB1 must instrument: `rows_examined` counts tuples *visited*, and the
number that moves here is rows *decoded*, which no counter reports today.

**Recorded as an addition to IB-R1 rather than a seventh ruling**, since
it is the same mechanism described at the level where it actually pays.

### 1.4 IB re-opens a gate SB closed, and closing it again is IB's own work

This is the finding that changes IB's task list, and it is a correctness
matter rather than a scoping one.

`RangeEligible`'s `kIndex` arm was **removed** by SB
(`src/exec/range_eligible.cpp:31-45`), and the removal is recorded as
behaviour-preserving — in that file, in `docs/inflight/known-gaps.md:955-961`
and in `crosscore.md` §574 — on **exactly one premise**:

> "a secondary index is btree-only (IX3), so every relation that could
> trip it was already declined by the D1 btree arm one line earlier."

**IB lifts IX3.** The premise dies with it. After IB2(a), a
heap-clustered relation may carry an index; heap is the *only* kind D1
lets split; so an indexed heap relation reaches `RangeEligible` with no
arm left to decline it. The two gates were a pair — `RangeEligible`
refuses "split a relation that has an index",
`RefuseAuxiliaryOnSplitRelation` refuses "index a relation that is split"
(`src/catalog/catalog.cpp:3046`) — and IB would leave the second standing
alone while the first silently admits, which is the *index-then-split*
order.

What that costs if it is missed: index maintenance is owner-local
(`AppendIndexEntry`, owner-stamped pages), and there is one index root.
A peer writing into its own range either does not append at all or
becomes the second writer of a page the handoff granted elsewhere. Either
way the index stops being a superset of the qualifying rows, and IX1's
superset rule — the one property that makes an index safe to serve from —
is gone. An index missing an entry is not slower, it is wrong (§2.1).

> **IB-R7 (new, and this workplan's own):** IB2 re-adds `RangeEligible`'s
> `kIndex` arm in the same commit that lifts IX3. Not afterwards, not in
> IB5's closure. The arm returns `RangeGate::kIndex` for
> `!access.indexes.empty()`, and its comment names IB as the reason it is
> live code again rather than the dead code SB removed.

This is `ratification-ae.md` AE-5.1 applied to the first order filed
under it: **AE withdraws the plan to take a gate down; it does not let a
feature take one down by accident.**

### 1.5 The two rulings that stand unamended

- **IB-R4 (the superset rule crosses unchanged)** — ratified. The btree
  arm already skips a pk whose row is retired or invisible, counting it
  rather than erroring (`src/exec/step_vm.cpp:1385-1392`, the dangling-pk
  skip), and the heap arm's walk applies the same visibility filter every
  scan applies.
- **IB-R6 (planner admission is the btree rule)** — ratified, with §1.2's
  amendment noted in its safety argument: the degradation is to the
  filter scan **plus** the tail the scan would also have walked, so it is
  bounded above by the scan itself and not merely close to it.
- **IB-R5 (the fences)** — ratified as written. F1's heap-parent FK
  refusal, `UNIQUE` (IX11), the index-only scan's double gate, and the
  split gate all stay. §1.4 adds that "the split gate stays" now takes
  **two** arms to remain true, not one.

---

## 2. IB0's amendments, as landed

- `docs/spec/index.md` §3 — IX3 rewritten: the refusal becomes an
  admission with the mechanism, the one-sided bound, the decode-avoidance
  saving, the order property, and the F1 fence in its own words.
- `docs/spec/index.md` §13 — the per-range-vs-global item restated under
  AE: not this version's question, and the gate that depends on it now
  needs both arms.
- `docs/inflight/known-gaps.md` — the `kIndex`-arm entry rewritten: its
  "behaviour-preserving because dead code" premise is retired by IB, and
  the arm is owed **at IB2**, not at D1.
- `docs/spec/cabin.md` §186 and `docs/spec/physical-optimizer.md:25`
  carry IX3 citations that survive the amendment; checked, not changed.

## 3. Where IB1 picks up

The order's IB1 (harness plus the filter-scan baseline arm) with **one
added dimension** from §1.2: matching rows drawn near the **start**, the
**middle** and the **end** of the pk space, so the tail bound's
contribution is separated from decode avoidance's rather than averaged
with it. Cells otherwise as the order states them — {1k, 10k, 100k} rows
× N ∈ {1, 10, 100, 1000} × trail state ∈ {cold, warm} — under
`bench/v2.8.0/`, named by `git describe --tags`.

A counter IB1 needs and the engine does not have: **rows decoded**,
distinct from `rows_examined`. §1.3 is the hypothesis it tests, and
without it the mechanism is inferred from a mean.
