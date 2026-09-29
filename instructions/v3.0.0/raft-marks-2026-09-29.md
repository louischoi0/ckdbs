# Ratification marks — 2026-09-29

**The operator's words of 2026-09-29**, recorded by CLA on
`ax-s3-inflight-prose` from `b54a769` (`v2.7.0-470-gb54a769`; the
v3.0.0 tag is not cut - `raft-marks-2026-09-26.md` §4, Q3). All of it is
**verbatim**: the words as typed in the session.

---

## 1. AX-S2b landed, and AX-S3 started

Recorded after a session report on `ax-s2b-row-wait-wake` at
`b54a769` put three items to the operator: the push, the behaviour change
for a first encounter inside the holder's retire-to-release window, and
AX-S3.

| | |
|---|---|
| **Word** | *"merge and push, then start AX-S3"* |
| **Mark** | **AX-S2b lands**: `b54a769` pushed to `main` (`043aee7..b54a769`, the pre-push gate green). **AX-S3 starts**: the prose (`workorder-ax-inflight-publication.md` §3) |
| **Does not settle** | The first-encounter behaviour change (`known-gaps.md`, Locks, AX-S2b's second entry) - put in the same report and not answered, so it stays open |
| **Recorded at** | `workorder-ax-inflight-publication.md` §6, `index.md` |

## 2. AX closed

Recorded on `worktree-ax-close` from `f83f574` (`v2.7.0-473-gf83f574`), after AX-S3
landed on `main` at `f83f574` on the operator's *"merge and push"*.

| | |
|---|---|
| **Word** | *"close AX, and list all decision needed making to ship next milestone"* |
| **Mark** | **AX-S3 lands** (`f83f574` on `main`) and **AX closes** (`workorder-ax-inflight-publication.md` §7) |
| **Does not settle** | Any item §7 carries - the first-encounter behaviour change among them. The list of decisions is a report, not a mark |
| **Recorded at** | `workorder-ax-inflight-publication.md` §7, `index.md` |

## 3. AR0 D7 — as proposed, its condition re-read

§3-§6 recorded on `worktree-ax-close` at `e52ba9b` (`v2.7.0-474-ge52ba9b`),
after CLA listed the decisions the next milestone needs (items A1-A4).

| | |
|---|---|
| **Word** | *"A1: follow CLA proposal"*; and, to the question CLA put because the proposal's condition no longer exists - *"D7's proposal waited for gap locks, which D8 as ratified replaced with named units. When should the remaining gate (RefuseAuxiliaryOnSplitRelation) be removed?"* - the answer **"Following letter (Recommended)"**: *"Remove it in the same letter as D9(a), which completes D8's named units (the assertion group has had its unit since AT-S5d, the FK parent row gets it from D9(a)). Each of the four auxiliaries is admitted only once a cell shows its build and maintenance cover every chain of a split relation. This keeps the proposal's rule: nothing is removed before its lock protection exists."* |
| **Mark** | **D7 is taken as proposed** (`ar0-architecture-revision.md` §5): the Cabin invariant and its gates are removed, **not before the lock protection exists**. Since D8 was ratified without gap locks, "D1(b) gap locking on Cabin keys" reads as **D8's named units**: the assertion group's reservation, built at AT-S5d, and D9(a)'s held parent-row `S`. What remains of the gates is `RefuseAuxiliaryOnSplitRelation` (`catalog.cpp`), the `NotImplemented` answer to an index, an assertion or a foreign key on a relation split before AT-S9. **The option's "four auxiliaries" is CLA's miscount**: a Cabin on a split relation has been admitted, walked whole, since SB3 (`71f92f6`; `catalog.cpp`'s `CreateCabin`, `cabin_contract_test.cpp`), before any lock protection existed - found by this record's review and told to the operator; the mark covers the three that remain. It is **lifted in the following letter (AT-0 item 6), the letter of D9(a)**, each auxiliary admitted only once a cell shows its build and its maintenance cover every chain of a split relation |
| **Answers** | AR0 D7, the last `[quiet-wrong]` D-item unmarked; AT-0 item 6's "D7 with D1(b)'s gap locking", re-read |
| **Does not settle** | The letter itself - it is not opened or named (item A0). Whether an auxiliary that fails its cell stays refused or blocks the letter |
| **Recorded at** | `ar0-architecture-revision.md` AR0-M8, `CLAUDE.md`'s Open Decisions paragraph, `index.md` |

## 4. AR2 E3 — the two-core cells named

| | |
|---|---|
| **Word** | *"A2, A3, A4 is too"* - item A2, read with A1's *"follow CLA proposal"* |
| **Mark** | E3 retires once **D9(a)'s own two-core cells** pass - CLA's reading at `raft-marks-2026-09-28.md` §5 (Does not settle), now the mark: a parent `DELETE` on one core against a child write on another, on the two-core rig. They are the following letter's, like D9(a) |
| **Answers** | What §5 of the 2026-09-28 marks left open: which cells "the two-core cells" are |
| **Does not settle** | Which interleavings those cells drive - the letter's work order states them |
| **Recorded at** | `ar2-architecture-revision-borrow-model.md`'s E3 row, `workorder-at-m3-uniformity.md` AT-0 item 6 |

## 5. AR1 — ratified as proposed, with AR1-V's corrections

| | |
|---|---|
| **Word** | *"A2, A3, A4 is too"* - item A3, read with A1's *"follow CLA proposal"* |
| **Reading** | CLA reads "CLA proposal" as §12 as AR1-V amends it: AR1-V1's explicit recommendation for D8, and AR1-V4's finding carried into D7. AR1-V2 and AR1-V3 are findings with no proposal, and are left unsettled below |
| **Mark** | **AR1 is ratified**, its §12 items as CLA proposed them, and where AR1-V corrected a proposal the correction is the proposal: **D1** the advisory class in Waystone, keyed by `(fetch_id, arg)`; **D2** (`[quiet-wrong]`) an undeclared function is `kVolatileRow`; **D3** 16 B held by value, wider keys hashed and losing range serving, stated in `SHOW CABINS`; **D4** `CREATE CABIN ON <rel> (<F>)` as §8; **D5** a chain Cabin's supporting Cabins admitted and counted under the relation's cap; **D6** `fetch_id` on `SysPatternRow` with no `kFingerprintVersion` bump; **D7** (AR1's, not AR0's) `SysCabinRow` revised as §7 with old rows recognised on read - **by a mechanism the work order must supply**, because the `status` version bit §7 names does not exist (AR1-V4); **D8** (`[quiet-wrong]`) rule 0′ taken **as a wording change** - rule 0's scope widened to every replay, the mechanism stated as the index key - since the hazard it was to close does not exist (AR1-V1) |
| **Does not settle** | **AP's order**, which AQ depends on (§14) and no carry list names: AR1-V2 took away the reason AP goes first, and AT-9 carries AQ/AR without it. The divergence from `alter.md` AL3 that §6's position-keyed `expr_id` is (AR1-V3) - to be declared in `alter.md` by the work order that lands it, not decided here |
| **Recorded at** | `ar1-architecture-revision-cabin-function.md`'s status line and §12, `index.md` |

## 6. The foreign-key Cabin's "no children" answer

| | |
|---|---|
| **Word** | *"A2, A3, A4 is too"* - item A4. There was **no CLA proposal on record** (`foreign-keys.md` §2: "not decided here"), so CLA put one: *"Should the Cabin's authoritative 'no children' answer come back?"* - answered **"Restore in letter, cells (Recommended)"**: *"Restore it in the following letter as its own stage, next to D9(a), which touches the same reverse check. Two-core cells first: a child written on one core and a parent DELETE served from the set on another, a set built while a child insert is open, and a mutation showing the walk fallback is what the cells catch. Until then the walk stays, which costs a scan and never gives a wrong answer."* |
| **Mark** | **The clearing return is to be restored, in the following letter (AT-0 item 6), as its own stage beside D9(a)**, and only behind those cells. Until it lands, an exhausted set falls through to the walk, as now |
| **Does not settle** | The letter itself (item A0) |
| **Recorded at** | `foreign-keys.md` §2, `workorder-at-m3-uniformity.md` AT-0 item 6, `index.md` |
