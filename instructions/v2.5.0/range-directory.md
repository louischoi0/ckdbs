# Work order R3-B — the range directory, from row to measurement

Drafted 2026-08-28 against `main` at `cefb0ef` (`v2.2.1-115-gcefb0ef`).

Resumes `docs/inflight/in-progress/workplan-range-directory.md` at RD2,
where §8 parked it: *"the build resumes at RD2 after the decision
session."* It follows `instructions/v2.4.0/range-foundation.md` (RA0–RA5,
closed 2026-08-27) and inherits a `CheckWriteAffinity` that the cross-owner
line has now settled.

## 0. Why now, and what the previous line handed over

R3-A stopped deliberately. Its §0 gave three reasons for taking R6 first,
and all three have since resolved the way it predicted:

1. **`CheckWriteAffinity` was contested between two open lines.** The RA3
   review left it untouched, recording that *"that site flips whenever
   RD5/R6-8 rewrites it."* R6-8 flipped it (`7eaa14a`). RD5's author
   inherits a settled site.
2. **R6 was granularity-agnostic where it counts.** Confirmed at the close
   (`98b0993`): a participant is a core and never a relation, no leg of the
   protocol names a relation, so ranges change owner discovery and nothing
   else. Blueprint §11's R6 row now reads *gate satisfied, R3's resolver is
   what is left* — this order builds that resolver.
3. **R3's back half had nothing to measure.** This is the one that has
   *not* resolved, and §7 below is where it is confronted rather than
   deferred again.

So the sequencing argument has paid out, and what R3 owes now is its own
substance: a directory row, a resolver, allocation, per-range chains, and
the pipeline over them.

## 1. Scope

**In.** RD2, RD3, RD5, RD6, RD7, RD8, RD9 — the workplan's remaining rows,
unchanged in substance. This order adds the layer the RD table does not
carry: what the build must conclude, what it predicts, and what it
measures.

**Out.** Insert *spreading policy* (§6b's remaining half — this builds the
chains, not the policy that fans writes across them). The optimizer's range
verbs beyond RD6's API. Range policy of any kind: split triggers, merge,
migration — D3's, and `physical-optimizer.md` Part III is unwritten. Every
R6 row; the cross-owner line is closed. Core-count change.

**Explicitly not touched**, so a diff outside is a finding: `Txn2pcService`
and the resolver; `sys.ranges`'s oid, root page and superblock version;
the free map; the reactor wake path.

## 2. Decisions taken here, per operator direction 2026-08-28

The operator directed that decisions needing a call follow CLA's proposal.
Three are taken below with their reasoning, so a later disagreement lands
on the reason. **D1 and D3 are not taken** — §7 and §1 say why.

**D2 — the directory row, not the anchor page.** The plan already follows
CC9; this records the reason as *capacity*, not the collision. RA5 priced
the `index_oid == 0` collision at zero (`§10a`): `lo = 0` dissolves by
identification, `lo > 0` costs one tagged constant. So the collision cannot
carry the decision. Capacity can: the anchor holds **679** `{u64 key, u32
root}` entries (`anchor_page.hpp:36-42`) **shared with index roots**, and a
10 M-row relation at D6's size below needs ~2,441 ranges by itself. The
anchor cannot hold one large relation's ranges, let alone several
relations' plus their indexes. `SysRangeRow{rel_oid, lo, owner_core,
entry_page}` it is, with `lo = 0` and derived-`hi` enforced at the catalog
door rather than assumed.

**D6 — `kRowIdLeasePerGrant` (4,096 ids), as the swept starting value.**
Not chosen on the page arithmetic, which §10b computed and which is
*heap-only* and therefore silent about the relations that matter most.
Chosen on a mechanism: **R4's tail-insert spreading is id-block-aligned,
and the block is exactly `kRowIdLeasePerGrant`.** Range = lease grant means
a core inserting from its own lease stays inside one range *by
construction*. The alternative unit (`kDefaultExtentPages`, 64) does not
divide the lease evenly at any bench width, so a sequential insert would
periodically straddle a boundary and turn the most common write in the
engine into a cross-owner transaction — measured at 1.479× within one
instance (`a02a666`). RD9(b) sweeps this and may move it; the sweep is
what makes it a decision rather than a guess, and the operator owns the
final value on RD9's numbers.

**D4 — fan-in identity in the pipeline tag.** Already inside CLA's latitude
per §4; taken and recorded because it grows a wire form. §5's shape as
written: the `sibling` field, plural `InputEdge`, grouped `pending_remote`,
one `downstream_step` across siblings. The constraint that decides it is
the one SS1 established and R6 reused twice — **a reply must be matched,
not trusted**, so the tag must distinguish siblings of one fan-in from each
other, not merely from other statements.

## 3. The conclusions this milestone must produce

**CD1 — the zero-cost invariant, demonstrated rather than asserted.** RD3's
statement is *"a one-range relation on its owner core must add zero
instructions over today."* R3-A's H2 was confirmed at the linker
(sha256-identical binaries, `fd8e4da`) and that is the standard here. It
cannot be met the same way — the resolver *will* be in the binary and
*will* have callers. So CD1 must name what the unsplit path executes that
it did not, and if the answer is more than one predictable branch on a
cached field, that is the finding. RD9(a) measures; the conclusion states
the mechanism.

**CD2 — where allocation runs, forced rather than preferred.** §2b's
question — the drain tick (`core_runtime.cpp:1006-1016`, outside a borrow)
or the point of demand (`row_id_lease.hpp:88-95`, inside a running INSERT).
The workplan says *"the choice is forced by where RD5 allocates."* Reach
CD2 by writing what each forces about publication visibility, and pick the
one whose failure mode is a refusal rather than a wrong answer.

**CD3 — the insert-head defect closed at the route, not the symptom.**
RD6 carries the review's blocking finding: `desc_page_id` is CREATE-fixed
and every insert path uses it as the head, so a cut leaves it heading the
*lower* range, `ChainInsert` accepts a row whose id belongs above, no
refusal fires, and the pk routes the reader to the top range for a
zero-row answer. **The cut is what creates the route, not what closes it.**
CD3 states that the head now comes from the directory per range, and that
`heap_tail_hint` became per range with it — and names the test that would
fail if either half were missing.

**CD4 — what the range unit trades, on both axes.** RD9(b) reads the sweep
against directory rows and non-pk fan-out at the small end, and single-core
concentration at the large end — CC8's own stated reason for rejecting
relation granularity. The conclusion is the shape of that trade, not the
winning number; the number is the operator's on these figures.

## 4. Hypotheses — each with its falsifier

**HD1 — the unsplit path stays free.** A relation with one range costs what
it costs today, because the resolver short-circuits before any lookup.
*Falsifier*: RD9(a) shows a cost outside the noise band, **or** CD1 finds
work on the unsplit path beyond one cached-field branch. Six rows of the
cross-owner line asserted the analogous invariant and the last one
(`af36f24`, HR5) could not cleanly demonstrate it — so this hypothesis is
stated knowing its neighbour just failed to be confirmed.

**HD2 — range = lease grant keeps sequential inserts single-range.** A core
inserting from one lease grant never crosses a boundary.
*Falsifier*: `AllocateRowIdRange`'s actual carve does not align with the
range boundary at some grant, and an ordinary sequential insert becomes a
cross-owner transaction. This is D6's whole mechanism; if it fails, D6 was
chosen on a property it does not have.

**HD3 — the cut needs no page movement.** A range boundary falls where the
row-id allocator already carves a disjoint block, so *"the new range starts
as its own empty sub-structure and no existing page straddles it"* (§6b)
and CC10's page-boundary rule is satisfied vacuously.
*Falsifier*: any split path that must move, copy or rewrite an existing
page. If one does, RD5 is a different row than the plan describes.

**HD4 — the pipeline's four costs are additive and small.** §5's shape adds
a field, a plural edge, a grouped pending set, and a shared downstream
step; none changes the single-range path.
*Falsifier*: RD9(c)'s k-range read prices the fan-out materially above the
k separate reads it replaces.

**HD5 — the five gates hold at the allocator door.** `RangeEligible` was
built before anything could allocate (RA3), and RD5 is the first caller.
No relation that should not split will.
*Falsifier*: §9b's two admission windows — an index build in flight, an
assertion landing between core 0's half and the owner's adoption — are
races `RangeEligible` cannot see, and the serialization through core 0's
catalog stream is what is supposed to close them. If it does not, the gate
built early was necessary but not sufficient.

## 5. Task series — RB rows

The RD rows are the workplan's and their substance is unchanged; RB numbers
them for this order's gates.

| # | Task | Gate |
|---|---|---|
| **RB0** | **RD2 — the directory row.** `SysRangeRow{rel_oid, lo, owner_core, entry_page}`, D2 taken per §2 with capacity as the recorded reason. The `RangeRowNotYetDefined` stub in `DropTable`'s sweep chain (`catalog.cpp:1713-1727`) is replaced by the real codec — **that stub is the first sweep in the chain precisely so this replacement is probed before the destructive sweeps run** | **D2** (§2) |
| **RB1** | **RD3 — resolution and publication**, and **CD1**. `ResolveRanges(rel_oid, predicate) -> {owner_core, entry_page}[]`, plan-time, from the session core's cache. §2c's plan-time-only rule enforced by shape, not by comment | RB0 |
| **RB2** | **RD5 — allocation**, and **CD2**. Rides `AllocateRowIdRange`/`RowIdLeaseTable` unchanged; the row and the entry page are what is new. Size is one named constant through one function, swept by config, starting at D6's value. `RangeEligible` asked first, always, **on the owner core**, and **re-checked where the durable row lands** (§9b's two windows). The converse gates land here too: CREATE INDEX / CREATE CABIN / CREATE ASSERTION / an FK naming a split relation each decline. **C3's surface lands with this first caller** — the decline log line and per-core `range_split_declines` on `SHOW META`, deferred from RA4 under the absent-rather-than-zeroed rule | RB1, **HD5** |
| **RB3** | **RD6 — per-range chains**, and **CD3**. The insert head comes from the directory per range; `heap_tail_hint` becomes per range with it. Plus the mutation API Part III will call (split / set / modify / merge), one caller today, **no policy** | RB2 |
| **RB4** | **RD7 — the pipeline over ranges.** §5's shape with all four costs built and none assumed, **D4 taken per §2**. Range-order concatenation; key-order-requiring shapes refused as `emit_in_key_order` already is. Inherits D5 — core-0-sessioned only, and RD9's file must not read as general | RB1, RB3, **D4** |
| **RB5** | **RD8 — equivalence.** §8 test 9: every shippable shape over a split relation returns byte-identical results to the same rows unsplit on one core, the split the only variable, **matching rows straddling the boundary**. §8 test 13 in its §0-amended form. Plus the two the review owes: a post-split INSERT lands in the range its id names, and a cross-range DML meets the ratified retryable refusal rather than a wrong answer | RB4 |
| **RB6** | **RD9 — measure**, and **CD4**. Three cells per §7 | RB5 |

## 6. Correctness — ahead of any number

- **`cores = 1` unchanged.** Seventh consecutive order asserting it.
- **RD8's equivalence is the gate, not a cell.** A split that returns
  different rows is not a slow build, it is a wrong one.
- **The straddling case is required, not optional.** §8 test 9 names it
  because a boundary that no test crosses is a boundary no test checks.
- **A gated relation is declined, and the decline is read.** RB2 lands the
  counter with its first caller; a gate whose declines are invisible is a
  gate nobody can audit.
- **No new caller of the resolver on the unsplit path**, asserted by
  inspection **and** by RD9(a) — HD1's two halves.
- Full suite and `scripts/sim.sh`, baseline and re-read **in one sitting**,
  which sitting named.

## 7. Measurement — and the subject problem, confronted

`build-release`, interleaved arms in one sitting, per-arm processes, fresh
server and data file per invocation, per-rep spreads before any median,
rows in = rows out. Overhead A/B still suspended (2026-08-24); anything not
run is reported as **not run** — RP8's ceiling sweep fell through a gap
between two rows for want of that sentence.

| cell | measures | reads against |
|---|---|---|
| **RD9(a)** | The unsplit path, split build against pre-split tag, same sitting | **HD1**, CD1 |
| **RD9(b)** | Range-size sweep: 4,096 ids against the extent hypothesis and the sizes either side | **CD4**, D6's final value |
| **RD9(c)** | The k-range read against the same rows unsplit | **HD4** |

**The subject problem.** R3-A's §0 recorded it and this order inherits it:
the three named bulk bench relations are declared `BTREE`, and D1's decline
means no btree relation splits. So RD9(b) and (c) need a splittable
subject, and it must be named before the build starts rather than
improvised at measurement time.

**This order does not resolve D1 and does not build past it.** Instead RB6
runs against whichever heap relation passes all five gates at the benches'
real widths — §10b's candidate set is the starting point — and **the
results file states plainly which relation it used, that the engine's
principal bulk relations are not that relation, and what that bounds.**
A number measured on an unrepresentative subject is worth having if and
only if its unrepresentativeness is written beside it.

**D1 is handed on with a third option now on the table.** Blueprint §8
lists two candidates — partition-boundary lock, or serialization through a
rotating coordinator — and says the choice is decided by measurement.
Operator discussion 2026-08-28 raised a third: **remove the shared
structure rather than access it**, giving each range its own tree rooted in
the directory row's `entry_page`, which for a heap range is the chain head
and for a btree range would be that range's root. Two things recommend
recording it: a range is a pk range and a clustered tree is pk-ordered, so
the boundary is a natural cut rather than an imposed one; and RD2's row
already carries the field. Two things distinguish it from the rejected
stride forest: the boundary is the range rather than a hash class, and the
purpose is to avoid D1's mechanism rather than to relieve a bottleneck
T1b showed was elsewhere. **It is a design decision with nothing to
measure, which is why it does not fit §8's "decided by measurement"
framing and needs the operator rather than a bench.** Not this order's.

Results to `bench/<version>/`, `git describe --tags` in the filename, in
the v2.5.0 format: host with physical-versus-logical cores and SMT state,
filesystem, build flags, binary provenance, per-rep tables, findings tagged
**measured** or **source-read** with sites, a §"what this run does not
measure", and the PostgreSQL section — RP8 §10's reasoning about the
absence of an honest twin should be cited where it applies, not re-derived.

## 8. Improvement — what this leaves better

- **The engine gains its first sub-relation ownership unit.** Everything
  before this has been relation-granular; `owner_core` has been one field
  on a catalog row since `CreateTable` set it.
- **A defect closed at the route.** RD6's finding is the kind that produces
  a wrong answer with no refusal anywhere — the reader gets zero rows and
  nothing logs. Closing it at the head rather than at the symptom is what
  makes the class gone rather than the instance.
- **A gate audited for the first time.** `RangeEligible` has existed since
  RA3 with one caller, its own test. RB2 gives it a real one and a counter,
  and only then does the five-gate set get exercised against relations
  nobody wrote a test for.
- **D6 decided on a mechanism and then swept.** The static table (§10b)
  could not decide it; the lease-alignment argument can, and RD9(b) is what
  turns that argument into a value.

## 9. Deliverable

- RD2 … RD9 built, each row landing with its worktree name and short
  commit **inside the sentence that makes the claim**, and each with its
  `critics-developer` pass, findings applied or rejected by name.
- D2, D4, D6's starting value recorded in the workplan **as taken here,
  with the reasoning in §2**, not as inherited.
- CD1–CD4 written into `workplan-range-directory.md`.
- RD9's three cells under `bench/<version>/`, with the subject-problem
  paragraph §7 requires.
- `crosscore.md` §2a and CC9 amended where the build differs from the spec
  as written; **`docs/spec/` gains nothing until it is confirmed and
  implemented**, which for the pipeline over ranges means after RB5.

Open items handed on rather than fixed: **D1**, now with a third candidate
recorded and no measurement that would decide it; D3 (range policy, Part
III unwritten); core-count change; the read-only-participant COMMIT cost
(7–30× the read, `af36f24` — the largest number the cross-owner line left);
the missing per-leg instrument, now three times a blocked attribution
(`observability.md` §8a); the 992-byte reply cap against rule 1; the
~11 ms periodic stall; RW-C1.
