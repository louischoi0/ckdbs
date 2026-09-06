# Work order AW — M1 close and the read-view cutover

Written 2026-09-06 by CLA against `origin/main` at `15a57c2`. Letter AW
(AT = Uniformity, AU = ring retirement, AV = the two-core rig, all
reserved). AW is a **sequencing order**: it opens no new milestone of its
own and finishes two that are open — M1 (AM) and AN — and writes the one
document (AV) that gates what comes after them. Every `path:line` is
`[source-read]` at `15a57c2`; the rest is `[design]`; the one
`[measured]` deliverable is AW-S5's and does not exist until it is run.

**Status: AW-S0 done 2026-09-06 on `worktree-aw-m1-close` from
`15a57c2`.** Its cell was already satisfied when the stage opened — §7
says why, and records the one thing this order was written without
knowing. S1–S6 are not started.

## 0. Operator's decisions, 2026-09-06 (verbal)

Two decisions, both "per CLA's proposal", and one set of numbers that is
the operator's own:

1. **AN-R10 and AN-R12 are marked** as `workorder-an-read-view.md`
   `:487-534` and `:545-566` propose them (`d8574e0`).
2. **The interleaved A/B suspension is lifted for AM-S6 only**, on the
   host AL-S8 measured — `bench/v3.0.0/results-wal-single-stream-v2.7.0-157-gf6ed10c.md:27`,
   8 logical CPUs, AMD EPYC 9V74, 4 cores × 2 threads — because D15
   permits a delta only within one engine on one host, and AM-S6's whole
   deliverable is that delta. `CLAUDE.md:221`'s suspension stands for
   every other stage.
3. **Transaction lifetime** (operator, verbatim in substance): a
   transaction should complete within **10 seconds**; one that exceeds
   **60 seconds** may be aborted. Recorded as AN-R14 (§2) with the three
   exceptions CLA proposed and the operator accepted.

## 1. Where the tree is

AM: S0–S3 landed, and AM-S4a with them (`16e6c5c`: the superblock at 17,
which is D14's mount refusal); AM-S2-P (the ring port) closed at
`15a57c2`. **AM-S4's other half did not land beside it** — the stamp
field, the lease, `MayFault`, the CC7 fault grants, `TryClaimByStamp`,
`CoreRuntimePerCoreStreamTest` and `page-lsn-cross-stream.md` are all
still in the tree at `15a57c2`, against AM-R4a's "the two halves arrive
together or not at all" — so AM-S4 is open beside S5 and S6, and this
order carries no stage for it. AN: S0/S1 landed; S2 gated until now. AO:
S0–S4a within one core; S5 gated on AM-S6, AN-S2 and AU-S2. AU:
S1/S1b/S3; **S2 is gated on two things, not one** — the rig order AV,
which does not exist, and `SimWaker`, which is AU-S1c, filed at `1e4e446`
when AU-S1 reported itself built while dropping it, and still absent from
`include/kds/sched/`. AO-S5's own row asks for "a deterministic
two-`CoreRuntime` rig over a `SimWaker`, first". AU-S0 is also not fully
landed: its status row says AO-R4's text, the AO-S5 row and the rig
order's letter are still unamended. AN: S1b landed at `9ddcb58` beside
S0/S1; AO: S3b landed at `07c94c7`, which the AO order sequences *after*
S4a. `origin/worktree-am-s2` still exists although S-P4's cell said it
would be deleted after the push.

The critical path to the next milestone (AO-S5, cross-core waits) is
`AM-S6 → AN-S2 → AO-S5 ← AU-S2 ← {AV, AU-S1c}`. AW clears every gate on
it except AU-S2 and the `SimWaker` AU-S1c owes; the closing line of §5
said "AU-S2 alone" and is corrected there.

## 2. Rulings

**AN-R10 (marked) — the bound is on the transaction, not the snapshot;
the existing constant is re-scoped, not joined by a second.** As
proposed, with one change forced by AN-R14: the quantity it re-scopes is
no longer *idleness* but *lifetime* (below). `txn.md` §4.1 stays literally
true — no reader is ever told its snapshot expired; a transaction is
aborted for exceeding its lifetime and learns it at its next statement as
an ordinary abort.

**AN-R12 (marked) — the latched `{commit_lsn, floor}` pair.** One
accessor on `InstanceVisibility` answering both under the window latch;
`Floor()` (`instance_visibility.hpp:171`) stays for the callers that need
only the floor and is **not** used by `Visible`'s branch 3. The cell is
the straddle itself: a reader that read the floor before a `Reclaim()`
pass and takes the latch after it must answer *committed* for a
transaction the pass erased. **Mutation:** split the pair into two reads
and the cell fails.

**AN-R14 — Transaction lifetime: a 10-second envelope and a 60-second
ceiling. [operator numbers, CLA structure; both constants provisional]**

- **10 s is a design envelope, not a mechanism.** It is the shape of
  transaction this engine serves and the input every retention-sized
  quantity is derived from: undo retention, AN-R13's window, lock-wait
  fault nets, and later the WAL-replication ack timeout. It goes into
  `txn.md` §1 as a scope sentence. It already exists in the tree as
  `kShippedStatementDeadlineNs` = 10 s — in `statement_ship_service.hpp:360`,
  not `shipped_statement_executor.hpp`, which is only where `:172`'s
  `static_assert` compares against it; AN-R14 makes
  the coincidence a statement: the per-statement deadline *is* the
  envelope, and when statement shipping retires at AT the envelope
  survives under its own name.
- **60 s is the mechanism, and it is a new field, not a renamed constant.**
  Wall-clock from `BEGIN` (from the first read if AN-R11 is later taken); a
  transaction past it is aborted by the sweep and the abort surfaces at its
  next statement. **The quantity does not exist yet**: the participant
  context carries only `touched_at_ns` (`shipped_statement_executor.hpp:532`),
  `txn/manager.hpp` carries no wall-clock start for a transaction at all,
  and the name `began_at_ns` is already taken for something else
  (`command_dispatcher.hpp:364`, the coordinator's pending cross-owner
  commit latency). So AN-R10's "re-scopes rather than adds" is right about
  the *ceiling* — one quantity, one name — and wrong about the work: a
  start timestamp per transaction plus a sweep over local transactions is
  new mechanism. **Provisional** —
  set by the operator, not measured; AS-E measures the lifetime
  distribution under the four scenarios and this row is re-read then.
- **`kShippedTxnIdleCeilingNs` (`:171`, 300 s of idleness) is absorbed.**
  Under a 60 s lifetime an idle bound of 300 s is unreachable — a dead
  constant — and "one quantity, one name" retires it into the lifetime
  ceiling: one config key, `kds.txn_lifetime_ceiling`, default 60 s; the
  `static_assert` at `:172` keeps its meaning (lifetime > statement
  deadline, 60 > 10). Consumers to move: `shipped_statement_executor.cpp:1377`
  (the sweep's test becomes a transaction-start comparison),
  `statement_ship_service.hpp:263`, `cross-owner-txn.md:196` — **and that
  list is a third of it.** `grep -rn kShippedTxnIdleCeilingNs` finds 20
  references across 8 files, including `shipped_statement_executor.hpp:307`,
  `:414`, `:422`, `cross-owner-txn.md:414`'s settings table and seven test
  call sites; cell (6) is a grep returning nothing, so every one is in
  scope and AW-S3's M sizing is against 20, not 3.
- **Three exemptions**, each with its reason, so nobody later removes
  them as special cases:
  1. **DDL** — a single statement holding relation `X`; aborting a
     `CREATE INDEX` at 60 s gains nothing and forbids large indexes. No
     ceiling.
  2. **Background read views** (Cabin build, checkpoint, relayout) — they
     hold undo like any view. Ceiling applies, but the response is not
     abort: the task **re-mints its view and resumes** at its next
     resumable point (the PO series is already built resumable). A
     background task that cannot resume is a task defect, not a
     ceiling exemption.
  3. **Prepared contexts** — D4's exclusion, inherited unchanged; it
     leaves with 2PC at AT.
- **What AN-R14 costs, stated:** a long *busy* transaction is now aborted
  where CLA's original R10 proposal accepted it. The operator's numbers
  say this engine does not serve that shape; `txn.md` §1 says so in those
  words.

**AW-R1 — AM-S6 is the one A/B run under the lifted suspension.** Same
cells as AL-S8, same host, `build-release`, `ck-tester` interleaved; the
results file named by `git describe --tags`; the number that matters is
the AL-S8 → AM-S6 delta per cell. A host that is not the AL-S8 host is
not this stage — it is a new baseline and needs its own order.

**AW-R2 — AV is written, not built, in this order.** AU-S2 and AO-S5 both
cite the two-core rig; neither can start until the rig's order exists.
CLA drafts AV against AR0-6 D26 and AU-R6; building it is AV's own
stages, after AW.

## 3. Hypotheses

| # | claim | how it fails if wrong |
|---|---|---|
| H1 | AN-S2's H1/H2 (a higher-window commit visible to a lower core's next view; a post-mint transaction from a lower core's range invisible to the pinned view) both **fail on the drafted mechanism and pass after** — that pair is the cutover's reason to exist (`workorder-an-read-view.md:711`) | if either passes before the cutover the stage has nothing to prove and stops |
| H2 | The lifetime sweep costs one clock read per tracked transaction per cadence, nothing on the statement path | a `began_at_ns` read on every statement would be a G2 cost at `cores = 1`; the sweep, not the statement, checks |
| H3 | AM-S6's delta to AL-S8 is within the noise AL-S8 reported per cell | a cell outside it is the M1 overhead the milestone has carried as "not measured" since AM-S1, and is reported as such, not as regression to fix here |
| H4 | No document under `docs/spec/` claims a core-local pool after AM-S5 | `grep -rni "core-local\|per-core pool\|its own pool" docs/` is the cell |

## 4. Measurement

One stage measures: AW-S5 (= AM-S6). Everything else is a correctness
cell, mutation-checked before it counts (F3). Results under
`bench/v3.0.0/`, one file per cell, `v2.7.0-*` naming until the v3.0.0
tag exists (AR0-M6).

## 5. Stages

Branch: `main`, in sequence, one session at a time on
`instructions/v3.0.0/` and `src/txn/`. Every stage: `critics-developer`
review; suite plain and armed (`KDS_TEST_PAGE_LATCH=1`), plus
`KDS_TEST_FRAME_BUDGET=8` where the store is touched; stop.

| # | Stage | Cells (definition of done) | Size | Gate |
|---|---|---|---|---|
| AW-S0 | **Housekeeping.** `origin/worktree-am-s2` deleted; the AM-S2 row says so; `index.md` gains this order | `git ls-remote` shows no `worktree-am-s2` | XS | — |
| AW-S1 | **AM-S5** prose as its row lists: `eviction.md` §1/EV4 (AM-R7), `page.md` §6, `heap-and-tuple.md` §6, `rules.md` §3 fourth row, `crosscore.md` CC7, `CLAUDE.md` | H4's grep returns nothing; every struck sentence is replaced by the mechanism, not deleted | M | S0 |
| AW-S2 | **AN-R12** — the latched pair on `InstanceVisibility`; `Visible` branch 3 reads it; `Floor()` kept for reclamation's own callers | the straddle cell and its mutation (§2); AN-R9's publication cell still green beside it | S | — |
| AW-S3 | **AN-R14** — `kds.txn_lifetime_ceiling` replaces `kShippedTxnIdleCeilingNs`; the sweep keys on `began_at_ns`; DDL and prepared exempt; background tasks re-mint and resume; `txn.md` §1 envelope sentence and §4.1 rewritten *per instance* | (1) a transaction idle past 60 s is aborted and its next statement sees an ordinary abort, never `SnapshotTooOld`; (2) a busy transaction past 60 s likewise — **the cell CLA's first proposal did not have**; (3) `CREATE INDEX` on a relation whose build takes > 60 s completes; (4) a Cabin build past 60 s re-mints and finishes with a correct result; (5) a prepared context is not swept; (6) `grep kShippedTxnIdleCeilingNs` returns nothing. **Mutation** for (2): exempt busy transactions and the cell fails | M | S2 |
| AW-S4 | **AN-S2** — the cutover as its row (`:711`) states: `ReadView` gains `snapshot_lsn`, loses `up_to_trx_id`/`in_flight`; four-branch `Visible` with the pair from S2; `MintReadView` reads AN-R9's ceiling; `ReadHorizon()` answers an LSN; `Everything()` replaced (AN-R3) | the row's list, in its order: H1, H2 (§3 H1 here — both fail before, pass after); pinned view across a peer's commit; RC re-mint; visibility exactly at in-flight departure (AN-R9); rollback finds its own undo after a purge pass; the floor cell from AN-R13 | L | S2, S3 |
| AW-S5 | **AM-S6** — the baseline, per AW-R1 | one results file per AL-S8 cell; the delta table; H3's verdict per cell in the file's header, `[measured]` with the invocation | M | S1, S4, and the operator's host |
| AW-S6 | **AV drafted** — the two-core rig order: what the rig is (two reactors, one store, one stream, `WakerTable::Kick` as the only cross-core primitive), its first three cells (a kick wakes a parked peer; a lost kick is detected by the cadence; a page `X` on core 0 blocks a `Fetch` on core 1 — the S-P2 cell promoted to two reactors), and which AU-S2 and AO-S5 cells it must host | the document exists, cites AR0-6 D26 and AU-R6, and lists every AU-S2/AO-S5 cell by name; nothing built | S | — (parallel) |

Done when: AN-S2 landed; `bench/v3.0.0/` carries the AL-S8 → AM-S6 delta;
`instructions/v3.0.0/workorder-av-two-core-rig.md` exists.

**"AM has no open stage" stood here and is unreachable as this order is
written** (AW-S0's fact-check). AW carries stages for AM-S5 and AM-S6
only, and AM-S4's deletion half — `MayFault`, the lease, the CC7 fault
grants, `TryClaimByStamp`, `CoreRuntimePerCoreStreamTest` and
`page-lsn-cross-stream.md` — is still in the tree against AM-R4a's "the
two halves arrive together or not at all". Either AW gains a stage for it
or the condition is re-scoped; **that is the operator's, and it is left
open rather than decided here.** At that point AO-S5's gates are AU-S2,
and AU-S2's are AV and AU-S1c's `SimWaker`.

## 6. What this order does not do

- It does not take AN-R11 (mint at first read). AN-R14's "from `BEGIN`"
  becomes "from the first read" if R11 is later marked; nothing else
  changes.
- It does not decide the borrow units (tuple/page/relation vs five).
  That is `raft-ar2-B`, needed before AO-S6, not before anything here.
- It does not lift the A/B suspension generally. AW-S5 is the one run;
  `CLAUDE.md:221` is otherwise unchanged.
- It does not build AV, AU-S2, or AO-S5.
- It does not tag v3.0.0. AR0-M6's condition is unchanged.

---

## 7. AW-S0 as built — 2026-09-06 on `worktree-aw-m1-close` from `15a57c2`

**The cell was already green when the stage opened, and §1 is one commit
stale about it.** `origin/worktree-am-s2` was deleted on 2026-09-06 after
`15a57c2` was pushed — S-P4's cell, executed on the operator's word
rather than left pending — so `git ls-remote --heads origin
worktree-am-s2` returns nothing and did before this stage began. §1's
"still exists" reads against the tree as it was when this order was
written, and is left standing rather than corrected: a work order is a
record of what was true when it was ordered.

**What this order was written without knowing: the branch's history is
tagged.** Deleting the branch would have dangled the citation `index.md` makes to
`c7c3a67` as the retrieval point for the superseded
`workorder-am-s2-step3-scan-ring.md` (the port order cites `c7c3a67`
throughout as the *source* of the port, which is a different use) — CLAUDE.md's own
pattern for removed content, the way `bench/` and `instructions/` resolve
against `1769487`. So the operator's word was taken for an **annotated
archival tag**, `archive/am-s2-c7c3a67` on `272a46e`, pushed 2026-09-06.

Three things about it that matter to a later reader:

- **It is not a version and moves nothing.** The version of record is
  `v2.7.0` at `d840a30`. `272a46e` is not an ancestor of `main` — `main`
  took the port route — so nothing reachable from `main` can describe
  against it, and `git describe --tags origin/main` still reads
  `v2.7.0-234-g15a57c2`. The name is deliberately outside the version
  space so it cannot be mistaken for one.
- **Its message is mostly what bounds it**, per the tag rule: none of
  that code is on `main`, none of it should be revived from there, and
  §7 of the port order classifies every hunk — the ring model and the
  `PageDevice` declaration ported, `charged`, `inline_sweeps`,
  `frame_usage_for_test` and the eraser latching superseded.
- **It does not hold `raft-marks-2026-09-05.md` in its live form.** That
  document crossed onto `main` at `28e2be1` before the branch was
  retired, with its §1 rewritten there from what each mark obliged to
  where each mark ended up, a preamble added and §6's item 1 struck; §2–§5
  were untouched. The tag's tree *does* hold the file — what it does not
  hold is the post-crossing version, and **the tag's own message says
  "does not hold" flatly, which overstates it.** Not fixable without
  re-cutting the tag; recorded here so a reader of both is not misled.
