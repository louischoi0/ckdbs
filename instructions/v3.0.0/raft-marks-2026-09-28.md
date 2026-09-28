# Ratification marks — 2026-09-28

**The operator's words of 2026-09-28**, recorded by CLA on
`at-s16-walk-up-revalidate` from `1f230e4` (`v2.7.0-424-g1f230e4`; the v3.0.0
tag is not cut - `raft-marks-2026-09-26.md` §4, Q3). All of it is
**verbatim**: the words as typed in the session, or, where the operator
answered a question CLA put, the question and the answer chosen, quoted as
they stood.

---

## 1. AT-S16 started

| | |
|---|---|
| **Word** | *"start AT-S16"* |
| **Mark** | AT-S16 opens, by the AT-close order's §4 and Q1's ruling (re-validate and re-descend) |
| **Recorded at** | AT-6's AT-S16 row, `workorder-at-m3-uniformity.md` |

## 2. AT-S16 at the root — the grown-over mark

The survey found that Q1 does not decide one point: how a walker learns that
the page it is about to grow a new root over is no longer the root. CLA put
it as a question with three options.

| | |
|---|---|
| **Question** | *"AT-S16 at the root: how should a walker learn that the page it is about to grow a new root over is no longer the root (another core grew over it, published or not)?"* |
| **Answer chosen** | *"Grown-over bit (Recommended)"* - described as: a bit in each tree page's body flags, set under the old root's exclusive hold when a root is grown over it and logged in its image; existing pages read 0, which claims nothing; a walker finding it re-descends for the parent, and with a stale memo is refused `TxnConflict` naming a stale root; no anchor read, no hold outside the tree |
| **Declined** | *"Anchor read + hold"* and *"Non-root only"* |
| **Mark** | `kHeaderFlagGrownOver`, `kInternalFlagGrownOver` and `kIndexFlagGrownOver`, bit `0x2` of each tree page's body `flags` |
| **Recorded at** | `heap-and-tuple.md` §5, `index.md`, AT-6's AT-S16 row |

**What CLA's reading added**, stated so it can be checked. The bit alone is
sound only if a stale root is refused **before anything is written**:
otherwise a stale internal root, discovered after the levels below it have
divided, strands their new halves. So AT-S16 finds and holds every parent a
split writes before the first write. That is an implementation of the
ruling, not a new decision: it adds no persisted field beyond the bit.

## 3. Amendment 1 to the AT-close order — the order of work

Pasted mid-stage with the amendment's text (*"Amendment 1 to the AT-close
order — the quiet-wrong windows found after issue"*):

| | |
|---|---|
| **Word** | *"ignore AT-S16 things, but follow what remains:"* |

CLA asked what that meant for the stage in progress and for the placed text:

| | |
|---|---|
| **Question** | *""Ignore AT-S16 things": what happens to the S16 work in progress? ..."* |
| **Answer chosen** | *"Finish S16 too"* |
| **Question** | *"When the amendment is placed as the order's §7, what happens to its S16 parts ...?"* |
| **Answer given** | *"finish AT-S16 first"* |
| **Mark** | AT-S16 is finished first. The amendment is placed after it, with the rest of what it orders following |

## 4. AR0 D1 — the isolation levels

| | |
|---|---|
| **Word** | *"[decision] [D1] isloation level: RU(read uncommitted) - do not support , this project only propose 2 levels (RR, RC)"* (as typed) |
| **Mark** | Two isolation levels, `READ COMMITTED` and `REPEATABLE READ`. `READ UNCOMMITTED` is **not supported**: refused by name, `Unsupported` - a client waiting for it waits forever - instead of the `InvalidArgument` "unknown isolation level" it met before, which read a declined SQL level as a typo |
| **Answers** | AO-0 item 8 (`workorder-ao-m2-lock-family.md`): no order carries RU |
| **Does not address** | D1's option (b) itself. AR0-M1 took (b) on the premise that RU, RR and RC are all deliverable; CLA reads this word as narrowing that premise to RR and RC, with (b) standing, and says so at AR0-M1 rather than inferring a withdrawal |
| **Recorded at** | `ar0-architecture-revision.md` AR0-M1, `txn.md` §1, `manual/sql/sql.md`, `client-manual.md`, `src/txn/manager.cpp`'s `ParseIsolationLevel` and its cell |

## 5. AR0 D8 — write skew, ratified as revised

| | |
|---|---|
| **Word** | *"[decision] [D8] D8 ratified as revised: write skew is closed by named units, not gap locks - the assertion group via registre reservation, the FK parent row via D9(a)`s held S. GROUP BY is the declaration; E3 retires once the two-core cells pass"* (as typed) |
| **Mark** | **No gap locks.** Write skew is closed by units the engine can name: an **assertion's group** by the registry's reservation (`assertion.md` §6, AS4 - one registry for the instance since AT-S5d), and an **FK's parent row** by D9(a)'s `S`, held by the child's writer. **The `GROUP BY` list is the declaration**: no separate locked-key clause at `CREATE ASSERTION` - AR0-M3 item 4's reading, now the mark. **E3** (AR2's child-relation `S` fence for a parent `DELETE` with no covering structure) **retires once the two-core cells pass** - a retirement with a gate, not a retirement now |
| **Answers** | AR0-M3's four inherited items: item 1 (trigger set against lock set) and item 3 (a gap needs a structure to name it in) fall away with gap locks; item 2 (the FK half named the parent's key while the phantom is child-side) is closed from the parent row instead - a child writer holding `S` on the row its forward check found blocks the parent's `DELETE`; item 4 is the mark's `GROUP BY` clause |
| **Does not settle** | Which cells are "the two-core cells" E3 waits on: CLA reads them as the D9(a) stage's own - a parent `DELETE` on one core against a child write on another, on the two-core rig - which do not exist yet, because D9(a)'s held `S` is the following letter's (the AT-close order's §7.4, #5). **Nothing is built by this mark**; it decides what the following letter builds and what it does not |
| **Recorded at** | `ar0-architecture-revision.md` AR0-M3, `ar2-architecture-revision-borrow-model.md`'s E3 row, `index.md` |

## 6. AR0 D9 — D9(a), ratified

| | |
|---|---|
| **Word** | *"[decision] [D9] D9(a) ratified as built: the child`s check takes IS on the parent relation and S on the parent tuple at the hoist, held to decide; no escalation, no persisted bit; cross-core wait is AX`s, not a gate."* (as typed) |
| **What CLA found, and asked** | At `67a0030` the tree does not do this: the hoist (`ResolveForeignKeyParents`) takes no `IS` and holds no `S`; `WaitForParentRowWriter` asks for the parent tuple's `S` only on a busy verdict and releases a grant at once, and `known-gaps.md` (Foreign keys) and `foreign-keys.md` §3a record the cross-core window as open for D9(a)'s fence. Put as a question - ratify the design and build later, build it now, or something else is built - the answer chosen was *"Ratify design, build later (Recommended)"* |
| **Mark** | **D9(a) is ratified in this shape**: the child's forward check takes **`IS` on the parent relation and `S` on the parent tuple at the hoist, held to its decide**; no escalation; no persisted bit; the **cross-core wait is AX's**, not a gate on D9(a). **Building it is the following letter's** (the AT-close order's §7.4, #5), and the window stays open in `known-gaps.md` until then |
| **Bears on** | D8's mark (§5): the FK parent row is closed by exactly this held `S`, and E3 retires once its two-core cells pass - so E3 waits on this build |
| **Recorded at** | `ar0-architecture-revision.md` AR0-M4, `index.md` |

## 7. AR0-5-R — ratified

Recorded on `worktree-ratify-ar0-5-r` from `5dc4081`
(`v2.7.0-460-g5dc4081`). AT-S19's draft was already on `main` and its
gate - S21 landed - was not met:

| | |
|---|---|
| **Word** | *"start AT-S19"* |
| **Question** | *"AT-S19 is already drafted and on origin/main ... Its work-order gate is 'S21 landed, and the word'. AT-S21 is built and reviewed ... but not merged. What should CLA do?"* |
| **Answer chosen** | *"Land S21, then close S19 (Recommended)"* |

AT-S21 landed on `main` at `5dc4081` on the operator's *"merge and push"*,
and then:

| | |
|---|---|
| **Word** | *"ratify AR0-5-R"* |
| **Mark** | **AR0-5-R is ratified**: R0-R9, the amendment as built at `3de6d62` with AT-S21's row as landed, and R10's split with it - the departures R10 lists as already the operator's stay recorded rather than re-decided, and those it lists as *"ratified by this text and by no earlier word"* are ratified by this word - among them **AR0 D3**, answered in practice by the log stream's core-0 placement (`known-gaps.md`, Decisions the revision has not taken). **The body and AR0-5-V are not ratified**: they stay as the record of what was proposed on 2026-09-05, and where they and AR0-5-R disagree AR0-5-R governs |
| **Answers** | AT-0 item 1 (`workorder-at-m3-uniformity.md`): AR0-5 is no longer a governing draft. AT-S19's gate - S14-S17, S17b, S21 landed, and the word - is met |
| **Does not settle** | By R10's own terms, R8.3's lost cells and R8.4's open entries are recorded, not accepted: two mutants survived the full suite at `3de6d62` (`known-gaps.md`, Testing), and restoring the cells is still the operator's decision; R8.4's entries stay AT-S20's to re-read and AT-9's to carry. R8.1's two open members stay with their owners - the FK forward window with the following letter (§6 here), the in-flight predicate with AX |
| **Recorded at** | `ar0-5-amendment-uniformity.md` (status and R10), `known-gaps.md`'s D3 entry, `CLAUDE.md`'s open-decisions line, AT-0 item 1 and AT-6's AT-S19 rows in `workorder-at-m3-uniformity.md`, `workorder-at-close-ar0-5-left-open.md`'s AT-S19, `index.md` |

## 8. AT-0 item 3 — the borrow cap stays 65,536

| | |
|---|---|
| **Word** | *"[decision] AT-0 item 3: maintainconstratintmaxcapnumber:65,536."* (as typed) |
| **Mark** | **`max_locks_per_txn` stays 65,536** (`kMaxLocksPerTxnDefault`), and its unit is **the transaction**: with no participants since AT-S6 there is one `Transaction` per transaction, so the per-`Transaction` count AO-R10 built at AO-S2 is now per transaction across cores. The narrowing - one cap per transaction where a cross-owner transaction used to have one per participant - is stated, not changed: the value is kept as CLA's proposal put it |
| **Answers** | AT-0 item 3 (`workorder-at-m3-uniformity.md`); the question `raft-marks-2026-09-08.md` §1 left open ("per transaction across cores ... or per local transaction") until AT's uniformity work asked it again |
| **Builds nothing** | The value and the counting site are unchanged; `lock_table_test.cpp`'s pin of 65,536 stands |
| **Recorded at** | AT-0 item 3 and AT-6's AT-S6 row in `workorder-at-m3-uniformity.md`, `txn.md` §5, `lock_table.hpp`'s cap note, `index.md` |
