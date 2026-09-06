# Ratification — operator marks of 2026-09-05 on the pending decision list

2026-09-05, operator (verbal, chat session): *"the pending decision list
follows CLA's proposals; write the work order."* Recorded by CLA against
`103a6b8`. Nothing here is code. Every row names the document and item it
marks, and what the mark obliges. Where CLA had **no** proposal on file,
the row says so and the item stays open — the mark cannot adopt a
proposal that was never made.

The list marked is the one CLA presented at session open from the state of
the tree at `103a6b8`. Items not on that list are not marked by this
document.

**Landed on `main` 2026-09-06, a day late and from a branch that was
retired** (AM-S2-P S-P4). This file was written on `worktree-am-s2` and
sat there while a second session built AM-S2 step 3 on `main`; §1 is the
section that collision reached, and its rows now say where each mark
ended up rather than what it obliged. **§2, §3, §4 and §5 were never
touched by that collision** and are the reason this document had to be
carried across before the branch was deleted: AR0-5's D17–D22, AR0-6's
D24–D26 and AS's Q1–Q6 are operator input that exists nowhere else in the
tree, and §5's AN-R10 paragraph is what gates AN-S2.

## 1. AM-S2 step 3 — `workorder-am-m1-shared-pool.md`

| item | mark | where it ended up |
|---|---|---|
| `ScanRing` under sharing (`device_page_store.cpp:703-712`, "neither is decided") | **pin the one page handed out** — AM-R8, R8a, R8b, R8c | **held, by a different route.** A second session decided the same question the same day the other way (`a21fe3f`, pin every slot) and landed it on `main` first. The operator's decision of 2026-09-06 combined the two per item, and `workorder-am-s2-p-scan-ring-port.md` is the merged ruling: AM-R8 final is one page-latched pin, which is this mark's shape plus the latch the mark did not name |
| R3 — armed/unarmed CLOCK usage disagree after a contended fault | AM-R9: re-check on wake without re-fetching | **superseded.** `main` closed R3 at `dd833d6` by pinning where the load finished, so the second fetch never runs — better than the suppressed charge AM-R9 proposed, and better than the letter of AM-R9 itself, which would have skipped a re-faulted frame's dirty mark |
| R6 — hit path's `loading_` test uncovered, fixture budget 0 | AM-R10: budgeted fixture variant; the data-race half stays recorded, not covered | **superseded.** `main` moved MG06's sweep inside `InsertFrame`'s own hold and covered it with `EvictionInsertSweepTest`, which is AM-R10's rig by another name; the data-race half stays recorded, as the mark says |
| R8 — no `PageDevice` declares thread-safety | AM-R11: the declaration; file-backed device source-read against it | **taken as marked**, at `7804a33`. The read found `FilePageDevice` needed no change; its own header's "core-local, like every PageDevice" was the defect |

**This section is history now, and the mark it records is not what shipped
verbatim.** It is kept rather than rewritten because a mark is a record of
what the operator said on the day, and because three of its four items were
overtaken by a parallel session rather than by a change of mind. The live
ruling is `workorder-am-s2-p-scan-ring-port.md` §2. The two documents this
section cites as obliged — `workorder-am-s2-step3-scan-ring.md` and its S3a/S3b
— never reached `main`; they lived on `worktree-am-s2` and were superseded by
the port order before that branch was retired.

## 2. AR0-5 — `ar0-5-amendment-uniformity.md` §6, D17–D22

| # | mark | obliges |
|---|---|---|
| D17 | `owner_core` columns dropped at M3 on AT's own format event; a pre-M3 value ignored on read. **Not** AM-S4's event | AT's format event; `rows.hpp:978`'s `static_assert` moves then |
| D18 | affinity kept as a statistic and optimizer hint, D10 weight 0 until AS-E; placement NS10 deleted | AT prose; `physical-optimizer.md` keeps its consumer |
| D19 | every core listens (`SO_REUSEPORT`); a session lives where it was accepted; no handoff. Platform without it: core 0 accepts and hands off — a networking fallback, not ownership | AT; the fallback's handoff is a ring consumer AU-S5 must list, not one it may strike |
| D20 | 4,096 for trx-id and row-id caches (today's `kTrxIdBlockSize`, `kRowIdLeasePerGrant`); one extent | **constant carried, not decided**: the mark adopts today's values as the starting point and the "no constant without measurement" rule applies to any change from them |
| D21 | schema version word in `Expeditor`, memory only; bumped **before** the DDL's relation `X` is released | AT; AR0-5 §8's read-order hazard is the cell |
| D22 | M3 is "Uniformity", letter **AT** | `index.md` when AT's order is written |

AR0-5 remains a **DRAFT** as a body — this marks its §6 items, not the
amendment; AR0-M's pattern applies (a mark on items, ratification of the
document as its own act).

## 3. AR0-6 — `ar0-6-amendment-ring-retirement.md` §6, D24–D26

D23 was already taken (AU-S3 built).

| # | mark | obliges |
|---|---|---|
| D24 | `sched.md:43`'s deferred-inbox sentence kept, with the "no caller" note | AU-S0's remaining prose edits |
| D25 | kind enum frozen by `static_assert` on the count, comment naming AR0-6 — count **34** since AU-S3, not 35 as §6's row still reads | AU-S0/S1 follow-up; AR0-6-V's number is the one to carry |
| D26 | the two-core rig order takes letter **AV** | the rig order, unwritten; AU-S2 cites it |

## 4. AS — `workorder-as-sus1-heap-suspended.md` AS-0, Q1–Q6

| # | mark | obliges |
|---|---|---|
| AS-Q1 | both defaults flip (`ast.hpp:415`, `command_dispatcher.cpp:5431`), per AS-R2 | landed at `1f77e87`; the mark confirms |
| AS-Q2 | resume-condition citation corrected to parser-v2 §9 item 8 (AS-R6) | landed; confirms |
| AS-Q3 | `Unsupported` kept; the message names SUS-1 | landed; confirms |
| AS-Q4 | `heap-suspended` means **"excluded from the gate, run on request"** until a nightly runner exists in the tree | the label's application is unblocked. AS-S1 left the labelled set empty because applying it would have removed heap cells from the only thing that runs them; the mark resolves that by making the *gate*, not the ctest invocation, the thing that excludes the label: label the cells, keep them in the default invocation, and have `CLAUDE.md`'s green criterion exclude `heap-suspended`. If the gate cannot express a label exclusion, the label is applied, nothing is excluded, and the row records that |
| AS-Q5 | banner the three specs (relayout planner, bulk sorted fill, range splitting) and qualify the three `CLAUDE.md` milestone rows, in §3.1b's shape | a prose stage, **AS-S2**, not started; sized S. Bigger than SUS-1 itself, which is why it is a stage of its own rather than a fold-in |
| AS-Q6 | `tools/benchmark.py`'s `--clustered heap` made unconditional so it fails loudly with the SUS-1 message; the six tools sharing the shape the same; `bench/v3.0.0/` headers that read "heap" for a btree measurement are corrected **in prose only** — the numbers are btree numbers and stay | a tools stage, **AS-S3**, sized S; and one correction pass over `bench/v3.0.0/` under AL-R8's rules |

## 5. Items the mark does **not** reach

**AN-R10 — undo retention under a global horizon** (`workorder-an-read-view.md:487-499`).
CLA filed **no proposal** — the ruling says so in its own words — and
proposed only that AN-S2 not land without one. A blanket "follow CLA"
therefore adopts nothing here, and CLA is not going to manufacture a
proposal under the cover of the mark. The quiet-wrong is real: one idle
`BEGIN` on any core holds the whole instance's undo, where today it holds
one core's. What CLA can say now is the shape of the candidates, so the
operator's next word can be a choice rather than a request for options:

- (a) **accept the cost and name it**: `txn.md` §4.1's sentence rewritten
  from *per core* to *per instance*, no mechanism, `shipped_statement_executor.hpp:157`'s
  five-minute abandoned-transaction case promoted from a comment to the
  stated mitigation;
- (b) **a retention bound with `SnapshotTooOld`**, which §4.1 declined and
  which is a user-visible error class this engine has never raised;
- (c) **AN-R11 first** — mint at first read, so `BEGIN`-then-idle costs
  nothing until the session reads; narrows the exposure without bounding
  it, and is itself a user-visible semantic change the ruling reserved
  for the operator.

(a) is the smallest and is what CLA would propose if asked; it is
*not* proposed by this document. **AN-S2 stays gated.**

**AN-R12** gates AN-S2 on its own and has a fix stated in the ruling; it
was not on the list presented and is not marked here.

**AN-R11** is user-visible and reserved to the operator by its own text.

## 6. Sequencing after the mark

1. ~~`workorder-am-s2-step3-scan-ring.md` S3a → S3b → S3c~~ — **done by
   another route.** AM-S2 step 3 completed on `main` (`2663001`, `dd0bfe9`)
   while this sequence was being worked on a branch, and the two were merged
   per item by `workorder-am-s2-p-scan-ring-port.md`, S-P0..S-P4.
2. AS-S2 and AS-S3 (prose and tools) at any point; they gate nothing.
3. AU-S0's remaining prose (D24, D25's count) with the next AU touch.
4. AN-R10: operator's word, from §5's three shapes or a fourth.
5. AT (Uniformity) and AV (the rig) remain unwritten; D17–D22 and D26 are
   their inputs, and neither is opened by this document.
