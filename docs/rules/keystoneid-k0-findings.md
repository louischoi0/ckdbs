# K0 — Keystone id audit, and what issue-once costs

Status: **findings**, 2026-08-03. No code outside `tests/` and `bench/`
changed. Answers K-M1 (`docs/rules/keystoneid-invariant.md` §5) and the question
asked alongside it — *could this feature slow the engine down dramatically?*

Evidence: `tests/keystone_id_test.cpp` (9 tests, all green) and
`bench/results-keystone-alloc.md`.

Three results, in order of how much they should change the plan:

1. **K1 does not hold across a crash today**, and the reason is durability
   rather than allocation. Bump-ahead as specified does not fix it, because
   the place it persists the ceiling — a catalog row — is exactly the place
   that does not reach the platter. §4 below.
2. **The performance fear is real but avoidable, and the escape is a
   specific number.** Crash-safe issue-once costs 2629× today's allocator
   if every id is forced durable, and 1.24× at N=4096. N=64 is a 3× INSERT
   regression. §5 below.
3. **§1.2's "(oid, pk) is a forever-unique key" is false on the oid half**,
   from a known gap the invariant document does not cite. §6 below.

---

## 1. Every path that issues an id

`Catalog::AllocateRowId` (`src/catalog/catalog.cpp:648`) has three callers,
and only one issues what §1 of the invariant describes:

| caller | what it issues |
|---|---|
| `src/server/command_dispatcher.cpp:819` | a user tuple's pk — the Keystone id |
| `src/catalog/catalog.cpp:838` (`RegisterPattern`) | an **oid** for a `sys.patterns` row: a body field, not a Keystone word |
| `src/stats/pattern_defs.cpp:172` | a real Keystone id, for a `sys.pattern_defs` row |

So **"issue-once" already names two id spaces with one implementation.**
The invariant document should say which one K1 binds. The audit's
recommendation is *both, and for the same reason*: the middle row is a
persistent oid source precisely because the general oid counter is not (§6),
and a claim that covers one and not the other will be read as covering
neither.

The sequence itself is per relation, persisted in `sys.tables.next_id`, and
issued by a scan-and-overwrite of that row. Pinned by
`EachRelationHasItsOwnSequence` and
`TheAllocatorAlsoIssuesCatalogOidsAndCatalogKeystones`.

## 2. Every path that could re-issue one

- **A free list.** There is none. §2's "no free-list of any kind exists" is
  already true, and it is true in the strong form: even a *physically
  retired slot* does not return its id
  (`RetiringATupleDoesNotReturnItsIdToTheAllocator`). There is no SQL
  `DELETE` yet, so the test retires the slot at the page — the case an
  allocator would most plausibly treat as reclaimable — and the sequence
  does not notice.
- **`CREATE TABLE`.** `catalog.cpp:350` sets `next_id = kFirstRowId` for a
  new relation. Correct: the id space is per relation.
- **A failed insert.** `AllocateRowId` bumps and persists *before* the
  caller encodes, so a failure between the two burns an id
  (`AnInsertThatFailsAfterAllocationBurnsTheIdRatherThanReusingIt`). K3
  makes the gap legal and the ordering is deliberate — the reverse would
  re-issue after a crash.
- **Exhaustion.** `id > kMaxKeystoneId` answers `OutOfRange` rather than
  wrapping (`AnExhaustedSequenceRefusesRatherThanWrapping`). K4's budget has
  exactly one enforcement point and it works.
- **A crash restart.** This one does not hold. §4.

## 3. What §2's allocator would inherit, and what it would not

`AllocateRowId` today does per issued id: fetch `sys.tables` **for write**
(dirtying the frame), decode live rows until the oid matches, overwrite one.
Three costs, and the invariant document mentions only the third:

1. O(relations) decode per insert — bounded much lower than expected, see
   the ceiling in §5.
2. The catalog page is dirtied on every insert.
3. No durability wait, because catalog writes are unlogged.

(3) is not a saving. It is §4.

## 4. K1 does not survive a crash, and bump-ahead does not fix it

`ACrashReissuesIdsThatTheDurableLogStillClaims` is green, and that is the
bad news — it pins the hazard rather than endorsing it.

Under `strict` durability the `HEAP_INSERT` records for ids 1, 2, 3 are on
the platter before the client is answered. The `sys.tables` row carrying
`next_id` is **unlogged** and reaches disk only at a checkpoint. Crash
between the two, and the durable log names three tuples whose ids the
reverted allocator is about to hand out again.

Nothing collides *today* only because nothing reads the log back. The moment
recovery redoes those `HEAP_INSERT`s, one id belongs to two tuples — which
is §1.1's snapshot hazard, arriving by the one door the invariant does not
watch.

Two tests bound the claim. `ASyncedShutdownLeavesTheSequenceAboveEveryLoggedId`
is the control: same fixture, same log, one extra `SYNC`, and the allocator
resumes at 4. So the failure mode is reuse, not a broken harness. And
`AnUnsyncedCrashLosesTheSequenceAndTheTuplesTogether` shows why nobody has
tripped over it: without a WAL the catalog page (id 4) and the heap pages
(ids ≥ 128) are lost as one unit, so a reverted sequence re-issues ids to a
relation that has also forgotten the tuples holding them. That is a
consequence of page-id ordering plus "recovery does not exist" — not a
designed guarantee, and not one to keep relying on.

**The consequence for the workplan.** §2 puts the persisted HWM in "the
relation's catalog metadata row, updated through the normal logged catalog
write path". There is no logged catalog write path. K-M2's acceptance
criterion — "K1 holds across simulated crash/restart cycles" — is therefore
not reachable by K-M2 alone, at any chunk size, and the suggested order
`K-M1 → K-M2 → ...` is missing a dependency:

> **logged catalog writes → recovery → K-M2.**

Proposed as an amendment, not applied.

## 5. The performance question

Full numbers and method: `bench/results-keystone-alloc.md`. The three that
decide anything:

**The allocator is 4.3–4.9% of an unlogged INSERT** (0.38 µs against 7.9 µs).
That is the ceiling on what any allocator change can win. K-M2 is a
correctness change; it should not be sold as a performance one.

**Per-id durability is the disaster, and it is what someone would write.**
Forcing the sequence to the platter per id costs 1054 µs — **2629× today's
allocator**, capping INSERT at ~949/s. That lines up with the 802/s already
measured for strict-durability inserts (CLAUDE.md): the same fsync, counted
twice. This is the dramatic slowdown the question was about, and the way to
reach it is to close §4 the obvious way instead of §2's way.

**N=4096 is the design, not a default.** Crash-safe bump-ahead at N=4096
costs 0.48 µs/id — 1.24× today's *non*-durable allocator, about 0.6% of an
insert. At N=64 it costs 17.2 µs/id, a **3× INSERT regression**, because one
fsync per 64 rows is still one fsync every 64 rows. §2 lists
`[PROPOSED: 4096]` without a reason; the reason is the fsync cadence, and any
future move to make N tunable needs a floor rather than a default.

**So: no, this feature does not slow the engine down dramatically** — as
specified. Implemented without bump-ahead, it makes inserts roughly 8× slower
than they are today and forecloses the relaxed durability class entirely.

**One thing found while measuring, unrelated to K1 and more serious than
it — since fixed.** The catalog could not hold more than **~62 columns in
total across every user relation** — 31 two-column tables, 15 four-column
tables, 7 eight-column tables, measured. The fixed catalog pages were single
pages that did not chain, and `sys.columns` filled first.

**Fixed 2026-08-06: the catalog relations chain.** Each fixed page id is now
a chain *root*; a full page links to the next, taken from a reserved range
of low page ids (`kCatalogOverflowFirst`..`kCatalogOverflowLimit`, ~114
pages). A page holds 68 `sys.columns` rows measured on disk, so the ceiling
is ~7,800 columns for the instance. It is still a ceiling, and the range is
still reserved rather than unbounded, for a reason worth keeping: a catalog
page has to sit below the first user page or a peer core may not fault it
(`DevicePageStore::MayFault`, workplan-crosscore.md P6).

Two things this did **not** change. Nothing reclaims a catalog row - there
is no `DROP TABLE` - so the ceiling is on columns ever created, not on
columns live. And §3's O(relations) scan is now genuinely O(relations)
across pages rather than bounded by one page filling, so the scaling cliff
this finding said "cannot happen" now can; that is the cost of removing the
limit and it belongs to whoever owns the catalog's lookup path.

## 6. The oid half of §1.2 is false

§1.2 claims: *"(oid, pk) becomes a forever-unique key ... a stored (oid, pk)
can dangle, but it can never mis-attribute."* The pk half is sound. The oid
half is not.

`Catalog::GenerateUserOid()` is `next_user_oid_++` over an in-memory counter
seeded at `kUserOidStart` and never read back from the catalog. Every boot
re-issues the same oids. No crash is needed — a clean restart plus one
`CREATE TABLE` is enough, and
`ObjectOidsAreReissuedAcrossABootAndCollide` demonstrates it end to end,
including the consequence: the new relation's oid resolves, through
`GetSysTableRow`, to the **old** relation, because the scan takes the first
row carrying that oid.

The gap itself is known and documented (`catalog.hpp`'s header comment,
`well_known.hpp`'s `kUserOidStart`), and `sys.patterns` rows take a
persistent sequence specifically to avoid it. What is new is that
`docs/rules/keystoneid-invariant.md` builds a stated guarantee on top of it without
naming it, and that the consumers §1.2 lists — the access statistics, any
future trail entry, any change feed — are keyed on the half that does not
hold.

It is also the cheapest thing in this document to fix: seed `next_user_oid_`
from the catalog at load, or persist it in the superblock. Neither is K0's
to do.

**Fixed 2026-08-08 (the first option).** `GenerateUserOid()` recovers its
position on first use from the highest oid `sys.objects` and `sys.columns`
carry, then increments in memory - so a boot costs one scan and a
`CREATE TABLE` costs nothing extra. `ObjectOidsAreReissuedAcrossABootAndCollide`
is inverted and renamed `ObjectOidsAreUniqueAcrossABoot`, and both of its
"invert this test" messages are gone.

Recovering beat persisting for a reason worth keeping: **there is no durable
counter that can fall behind the rows it describes.** The rows *are* the
counter, so a crash between issuing an oid and writing its row loses the oid
rather than duplicating it, and a lost oid is free under K3's no-density
promise. A superblock field would have needed a format bump and would have
introduced exactly the write-ordering question `sys.tables.next_id` already
has with the WAL (§4 of this document).

Two things this does **not** fix. The scan reads `sys.objects` and
`sys.columns` because those are the only two relations `GenerateUserOid()`'s
results are written to; an oid written only to some third relation would be
invisible to the recovery, so that contract is stated at the function and is
what to check before adding a caller. And it says nothing about the *pk* half
of §1.2, which §§1-5 of this document are about and which still depends on a
durable `next_id` (K-M2a).

## 7. Amendments to `docs/rules/keystoneid-invariant.md`

**Approved and applied 2026-08-03.** All four are in that document now; what
follows is the record of what changed and why, so the reasoning does not
have to be reconstructed from a diff.

**K3 — wording.** As written, *"Nothing in the engine may rely on ids being
sequential or contiguous"* is contradicted by four subsystems that rely on
ids being **ordered**:

> **This list is a 2026-08-03 finding and is preserved as one.** Three of
> the four were re-settled on 2026-08-11 when the `EXPLICIT` key mode
> dropped monotonicity for btree-clustered relations — notably the second,
> whose "refuses a non-monotonic id outright" is no longer true: a full leaf
> now divides. `docs/rules/keystoneid-invariant.md` §1 carries the corrected list
> and says which dependency was kept, paid off, or replaced. Quote that one,
> not this one.


- the semi-sorted heap chain refuses an id below the tail page's `min_key`
  (`heap_chain.hpp:32-35`), which is invariant 3 enforced at the one place
  tuples enter;
- the clustered btree **refuses a non-monotonic id outright**, with
  `OutOfSpace` naming the open split-policy decision rather than guessing
  (`btree.hpp:38-54`) — the strongest dependency of the four, since it is a
  hard failure and not a lost optimization;
- `keystone.hpp:19-22` states the sequence is "unique and monotonic by
  construction rather than by a uniqueness check", i.e. uniqueness is
  *derived* from monotonicity;
- `kRange`'s `min_key` pruning stops a walk on the strength of it
  (`src/exec/step_vm.cpp`).

§2's own allocator never moves its cursor backward, so the *behaviour* was
already monotonic-with-gaps and only the wording was absolute. K3 is now
**"No density promise"**, forbidding reliance on contiguity and stating
plainly that ordering is provided-but-not-promised, with removing it named
as its own decision rather than something K3 licenses. §6's out-of-scope
line was narrowed to match — it said "any ordering/density guarantee", which
read as permission to break the four dependencies above.

**§1's closing aside** — *"The min_key semi-sorted heap keys off values, not
issuance order, and remains unaffected"* — is struck. `ChainInsert` refuses
an id below the tail's `min_key`, so it depends on issuance order directly.
The four dependencies replace it, so the correction is visible rather than
merely deleted.

**§1.2** now states an objective rather than a property, and names the oid
gap, the tests that demonstrate it, and the fix that closes it — while
recording that the fix belongs to the catalog, not here.

**§5** gained **K-M2a ("make the ceiling durable")** as a milestone of its
own, and K-M2's acceptance criterion was rewritten: it no longer claims K1
holds across a crash, since with an unlogged catalog write it can only
promise "never re-issues *given* that the ceiling reached the platter" — a
conditional whose condition is false today. The order is now
**K-M1 → K-M3 → K-M2a → K-M2 → K-M4 → K-M5**, moving K-M3 ahead because it
is the only unblocked milestone left. The measured inputs (the 4.3–4.9%
share, N=4096 as a floor, the 2629× per-id figure) are recorded against
K-M2 so they are read at implementation time and not looked up.

**§1.2** should either cite the oid gap or narrow its claim to the pk.
