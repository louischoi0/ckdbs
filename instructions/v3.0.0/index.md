# instructions/v3.0.0 — the version's operator input

Opened 2026-09-02 on `worktree-v3.0.0-arch-revision` at `d15b5ac`
(`v2.7.0-134-gd15b5ac`), the first thing written under `instructions/`
since the tree was emptied at `7f0193b`.

**The version in one sentence:** v3.0.0 is the big-bang change — one WAL
stream for the instance, shared memory in place of shared-nothing,
ownership decomposed into affinity and allocation — and it is built
milestone by milestone from AR0's §8, each milestone under one work
order, the `cores = 1` path byte-identical throughout.

**The operator named the version on 2026-09-03** (D15, marked — AR0-M6):
it is **v3.0.0**, and `bench/v3.0.0/` is the version's series rather than a
proposal's. The **annotated tag is a separate act and has not been taken**,
so `git describe --tags` still reads `v2.7.0-*` and every measurement
carries that until one is cut. AR0-M6 says what a tag cut at this commit
would and would not be able to claim.

**Two governing drafts, not one.** AR0 is the big-bang change and its §8
is the milestone chain every work order here belongs to. **AR1, added
2026-09-03, is not part of that chain**: its subject is Cabin and the
function catalog, it depends on neither the pool nor the view, and its
own §11 states the single place the two cross. A work order cites the
draft it serves; neither draft's numbering reaches the other's.

| document | what it is |
|---|---|
| `ar0-architecture-revision.md` | The governing draft, verbatim as the operator supplied it on 2026-09-02, with AR0-V appended: the source-read verification at `d15b5ac`, which is what the tree says wherever the body disagrees, and **AR0-M appended**: the operator's marks of 2026-09-03 on D1 (b, conditionally), D2 (a), D8 (as proposed), D9 (a), D11 (mover retired) and D15 (v3.0.0). AR0-M lists what stays pending; **M0 has the go-ahead and AL-S8's baseline is measured** (`6ead2a0`, three files under `bench/v3.0.0/`) |
| `workorder-al-m0-single-wal.md` | **AR0 M0 — the single WAL stream.** The survey of every per-core-stream assumption at `d15b5ac` (AL-3), the rulings M0 needs (AL-4, with AL-R1 the append mechanism as the one that decides the shape), and the stages AL-S0..S9 with their cells and sizes (AL-5). AL-S0 is the hour the operator ordered |
| `ratification-an-commit-order.md` | **A global commit sequence is admissible.** Rules that `cross-owner-txn.md` §1 rejected a global counter and **never justified it** — its stated reason reaches the shared transaction id beside it, and only on a per-core volume — so §3's cross-reference inherits an empty justification. AN-D4 makes the commit record's LSN the ordering authority under `kSingleStream`, invariant 12 untouched. **Marked 2026-09-03** (AN-D10): AN-Q1, Q2 and Q4 are settled by the operator's mark of `workorder-an-read-view.md` AN-R7(B) and built there; Q3, Q5 and Q6 moved into that work order as AN-R9, AN-R10 and AN-R11 — **not** into M2's table, as this document originally wrote its own exit. AN-D10 also carries the correction AN-R8 makes to AN-Q1's floor. **A prerequisite to D1's condition, not a milestone**: no code, six sites across four specs. AN-7 records the four claims a `critics-developer` review refuted in the first draft |
| `workorder-am-m1-shared-pool.md` | **AR0 M1 — the shared buffer pool, page latches, scalar page LSN.** Written 2026-09-03 while M0's baseline was still measuring, and **not started**: AM-1 says why AL-S8's numbers gate it. Two survey findings reshape the milestone — "scalar page LSN" is already done, and "page latches" is a first build rather than a widening, since `base/latch.hpp` has exactly one includer in the tree |
| `ar1-architecture-revision-cabin-function.md` | **A second governing draft, and it is not AR0's successor** — scalar functions, a purity class per function, a determinism class per statement, and a Cabin generalised from `(relation, column)` to a key function `F`, with C3 struck and the chain shape making a join's result set a witnessed Cabin. **DRAFT, pending ratification**, and independent of the pool and the view but for §11's one crossing. **AR1-V appended**: the source read at `74f971b`, which is what the tree says wherever the body disagrees. It disagrees twice that matters — §3's quiet-wrong hazard is refuted (rule 0 is structural over `(step_id, pk)` for the *driving* step too, so D8 is a wording change and costs nothing), and with it §14's reason for sequencing AP first; §7's migration bit does not exist |
| `workorder-an-read-view.md` | **The instance read view**, ordered by the operator on 2026-09-03 to be built *now*, between AM-S0 and AM-S1. AN-1 is the argument that a per-core view plus a shared pool is a dirty read rather than an error, and that whichever milestone first lets a core read another core's data page locally must carry this one. The AN-S0 source read found that trx ids are leased in per-core blocks, so **no bound on trx ids can order commits across cores** (AN-3 E's H1 and H2), and the operator marked AN-R7 **(B)**: the view is the commit-LSN window of `ratification-an-commit-order.md`, and the in-flight set retires rather than being shared. **AN-S0 and AN-S1 landed at `b5abab6` with the suite green; AN-S2 is gated on AN-R10** — the retention policy the mark was taken without. AN-7 records both review passes: CLA's source read (four findings, seven corrections) and a `critics-developer` pass that refuted AN-R1's lock-free justification for a non-monotone field and AN-R9's durability ground, neither of which survives as written |

**Read AR0-V before citing AR0's body.** Three of the body's numbers are
already attributed in the tree to causes other than the ones it names,
and two of its D-items name values the tree never shipped.

## What is *not* here

Two directories reopened on 2026-09-03 and each took a job this one used
to cover: **`bench/`**, where a v3 measurement goes (AL-R8; its README has
the rules), and **`docs/inflight/`**, which holds what is *missing*
(its README has the buckets). Open work orders stay here.

The one boundary worth stating twice, because getting it wrong is how a
list goes stale: a **decision** awaiting the operator lives in its work
order's ruling table — AR0's D1-D16 are indexed there, per milestone — and
a **gap** lives in `docs/inflight/known-gaps.md`. Never both.

**One form sits beside that boundary rather than inside it**, added
2026-09-03 with AN: a ruling whose subject is a **spec sentence** and whose
milestone has no work order yet gets its own `ratification-*.md` here. It
is not a third bucket — it is a holding shape with an exit written into it:
when the milestone's work order is opened, the ruling's `[OPEN]` items move
into that table and the document keeps only what it settled. A ruling that
would fit an existing work order's table does **not** take this form.

**That exit was taken on 2026-09-03, against a different milestone than the
one it named.** `ratification-an-commit-order.md` wrote its exit as *"when
M2's work order exists"*, on the reading that a commit sequence is
lock-manager work. `workorder-an-read-view.md` opened first and its subject
is the same object, so the operator's mark of AN-R7(B) is what made it that
work order: AN-Q1, Q2 and Q4 are settled by the mark and built there, and
AN-Q3, Q5 and Q6 moved into its ruling table as AN-R9, AN-R10 and AN-R11.
`ratification-an-commit-order.md` keeps AN-D1..D4, D6, D7 and D9, and its
AN-D10 records the move and one correction the mark inherits. **Two
documents still carry the letter AN**, which is why `CLAUDE.md`'s rule is
the whole protection here: **cite the file, never the bare number.**
