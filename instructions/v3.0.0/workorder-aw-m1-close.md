# Work order AW — M1 close and the read-view cutover

Written 2026-09-06 by CLA against `origin/main` at `15a57c2`. Letter AW
(AT = Uniformity, AU = ring retirement, AV = the two-core rig, all
reserved). AW is a **sequencing order**: it opens no new milestone of its
own and finishes two that are open — M1 (AM) and AN — and writes the one
document (AV) that gates what comes after them. Every `path:line` is
`[source-read]` at `15a57c2`; the rest is `[design]`; the one
`[measured]` deliverable is AW-S5's and does not exist until it is run.

**Status: AW-S0, S1, S1b, S2, S6 done, and AM-S4's slice (d) with them
(§9.4, 2026-09-07 — which is what makes "AM has no open stage" true for
every stage but S6); AW-S3 *half* done and reviewed;
AW-S4 *surveyed, one piece landed*, and the operator has since marked its
open watermark question (§12.3); plus AW-a (§10).
2026-09-06/07 on `worktree-aw-m1-close`.** §11 says which half of
AN-R14 landed, §11.4 what its review changed, and §12 what AW-S4's survey
found — including that AN-S3 is **not** the separable stage the AN order
says it is. §9 is AW-S1b's record — what it deleted, why its
four planned slices are one surgery, and the fixture change it forced. §7 is AW-S0's record — its cell was already satisfied when
the stage opened, and it carries the one thing this order was written
without knowing. §8 is what S1 and S2 built. **AW-S1b was inserted by the
operator after AW-S0's fact-check** (§0 item 4).

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
4. **AM-S4's deletion half becomes AW-S1b, before S5** (operator,
   2026-09-06, after AW-S0's fact-check found the half missing). The
   reason is the measurement's, and it is the one that settles it: **an
   AM-S6 baseline taken on an engine that still carries `MayFault` and the
   lease measures "half of M1 plus the old guards", not M1's overhead** —
   the number AW-R1 exists to produce would be attributable to neither
   arrangement. It also restores AM-R4a's atomicity rule, which the split
   commit broke. Size M, and a precondition of AW-S5 rather than a stage
   beside it.

## 1. Where the tree is

AM: S0–S3 landed, and AM-S4a with them (`16e6c5c`: the superblock at 17,
which is D14's mount refusal); AM-S2-P (the ring port) closed at
`15a57c2`. **AM-S4's other half did not land beside it** — the stamp
field, the lease, `MayFault`, the CC7 fault grants, `TryClaimByStamp`,
`CoreRuntimePerCoreStreamTest` and `page-lsn-cross-stream.md` were all
still in the tree at `15a57c2`, against AM-R4a's "the two halves arrive
together or not at all". **AW-S1b carries it**, on the operator's decision
of 2026-09-06 (§0 item 4), and has now landed all of it; the per-core-stream
mount branches and `page-lsn-cross-stream.md`'s file were slice (d), which
landed separately as AM-S4(d) on 2026-09-07 and closed AM-S4 (§9.4). AN: S0/S1 landed; S2 gated until now. AO:
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
| AW-S1b | **AM-S4's deletion half** (AM-R4a), which landed at `16e6c5c` without its other half: the pre-M1 mount refusal is what makes the lease, `MayFault`, the CC7 fault grants, `TryClaimByStamp` and `CoreRuntimePerCoreStreamTest` unreachable, so they leave in one commit, and `page-lsn-cross-stream.md` leaves the tree with them. **A precondition of AW-S5, not a parallel stage**: AM-S6 measured against an engine still carrying the old guards is not M1's overhead (§0 item 4) | AM-S4's own cells: a pre-M1 volume is refused at mount naming why; no reader of `flags` at offset 2 remains; **no `LeasedIdSource` is constructed on any mountable volume**. Plus the one AM-R4a implies: `grep -rn "MayFault\|TryClaimByStamp\|LeasedIdSource"` over `src/` and `include/` returns nothing | M | S1 |
| AW-S2 | **AN-R12** — the latched pair on `InstanceVisibility`; `Visible` branch 3 reads it; `Floor()` kept for reclamation's own callers | the straddle cell and its mutation (§2); AN-R9's publication cell still green beside it | S | — |
| AW-S3 | **AN-R14** — `kds.txn_lifetime_ceiling` replaces `kShippedTxnIdleCeilingNs`; the sweep keys on `began_at_ns`; DDL and prepared exempt; background tasks re-mint and resume; `txn.md` §1 envelope sentence and §4.1 rewritten *per instance* | (1) a transaction idle past 60 s is aborted and its next statement sees an ordinary abort, never `SnapshotTooOld`; (2) a busy transaction past 60 s likewise — **the cell CLA's first proposal did not have**; (3) `CREATE INDEX` on a relation whose build takes > 60 s completes; (4) a Cabin build past 60 s re-mints and finishes with a correct result; (5) a prepared context is not swept; (6) `grep kShippedTxnIdleCeilingNs` returns nothing. **Mutation** for (2): exempt busy transactions and the cell fails | M | S2 |
| AW-S4 | **AN-S2** — the cutover as its row (`:711`) states: `ReadView` gains `snapshot_lsn`, loses `up_to_trx_id`/`in_flight`; four-branch `Visible` with the pair from S2; `MintReadView` reads AN-R9's ceiling; `ReadHorizon()` answers an LSN; `Everything()` replaced (AN-R3) | the row's list, in its order: H1, H2 (§3 H1 here — both fail before, pass after); pinned view across a peer's commit; RC re-mint; visibility exactly at in-flight departure (AN-R9); rollback finds its own undo after a purge pass; the floor cell from AN-R13 | L | S2, S3 |
| AW-S5 | **AM-S6** — the baseline, per AW-R1. **Attempted 2026-09-07 and stopped: the box CLA runs on is not the AL-S8 host** - 4 logical CPUs against AL-S8's 8, same processor model, and AL-S8 ran `cores = 8` in every cell. AW-R1's own sentence settles it. AN-S2 is unbuilt besides. `workorder-am-m1-shared-pool.md`'s AM-S6 row carries the numbers and the two choices. **Two corrections from AN-S5, 2026-09-07**: the box now reads as AL-S8's shape (`lscpu`: 8 logical CPUs, 1 socket × 4 cores × 2 threads, EPYC 9V74), so the host gate as stated above no longer describes it; and a gate this row never had is the one that binds — AL-S8's two scenario drivers declare `HEAP` tables and SUS-1 refuses them, so no AL-S8 cell can run on any post-SUS-1 commit until AS-Q6 decides how the tools name storage (`docs/inflight/known-gaps.md`, Testing; `results-an-s5-scenario0-v2.7.0-265-g13b6b55.md`) | one results file per AL-S8 cell; the delta table; H3's verdict per cell in the file's header, `[measured]` with the invocation | M | S1, **S1b**, S4, the operator's host, **and AS-Q6** |
| AW-S6 | **Done 2026-09-07** — `workorder-av-two-core-rig.md`. Its §1 records two citation corrections this row forced: **there is no AU-R6** (the AU order stops at AU-R5; the ruling meant is AU-R3), and `SimWaker` is owed by the unbuilt AU-S1c rather than by a ruling. Its AV-R1 is the finding that blocks the rig: `Waker::Wake()` is not virtual, so AU-S1c's "the sim one substitutes where the table is built" cannot compile. **AV drafted** — the two-core rig order: what the rig is (two reactors, one store, one stream, `WakerTable::Kick` as the only cross-core primitive), its first three cells (a kick wakes a parked peer; a lost kick is detected by the cadence; a page `X` on core 0 blocks a `Fetch` on core 1 — the S-P2 cell promoted to two reactors), and which AU-S2 and AO-S5 cells it must host | the document exists, cites AR0-6 D26 and AU-R6, and lists every AU-S2/AO-S5 cell by name; nothing built | S | — (parallel) |

Done when: AM has no open stage; AN-S2 landed; `bench/v3.0.0/` carries
the AL-S8 → AM-S6 delta; `instructions/v3.0.0/workorder-av-two-core-rig.md`
exists. At that point AO-S5's gates are AU-S2, and AU-S2's are AV and
AU-S1c's `SimWaker`.

**"AM has no open stage" was unreachable as this order was first
written** — AW-S0's fact-check found AM-S4's deletion half still in the
tree with no stage carrying it — and the operator's decision of
2026-09-06 made it reachable by inserting AW-S1b rather than by re-scoping
the condition (§0 item 4). Recorded because the alternative was live: a
"Done when" quietly narrowed to what the stages happen to cover is how a
milestone closes over an open half.

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


---

## 8. AW-S1 and AW-S2 as built — 2026-09-06 on `worktree-aw-m1-close`

### 8.1 AW-S1 (= AM-S5)

The row listed six documents; the sweep needed eight. `page.md` §6 was
titled *Per-Core Buffer Pools* and opened with "multi-core adds instances,
not synchronization", which is the sentence `2663001` falsified; it now
states what synchronizes the one table, and that sharing is conditional on
the **log topology** rather than the core count, so a pre-M0 volume reads
the old text as live. `eviction.md` §1 and EV4 keep their lock-free
argument rather than dropping it, per AM-R7 — it survives as the price now
being paid, **with the second column stated as empty**, since AM-S6 has
not run. `heap-and-tuple.md` §6's "adding pages to `rules.md` §3's
declared shared list would be a spec change there before a code change
here" turned out to describe exactly the order it happened in. CC7's
flush-then-grant no longer ends "and the owner faults fresh frames".

**Two sites the row did not list.** `wal.md`'s fuzzy-checkpoint sentence
claimed each core checkpoints "its own pools"; a peer's checkpoint target
is the shared store (`core_runtime.cpp:954`), so under one stream N cores
walk one table. Corrected to what the code does, with *how many*
checkpointers a shared pool should have left to AM-S3 rather than answered
in prose — it is a behaviour question and this was a prose stage. And
`device_page_store.hpp` still described the self-deadlock check as a
`pins` proxy "sound only while pools are per core", which step 3b had
already replaced with a thread-local multiset.

### 8.2 AW-S2 (= AN-R12)

`LookupCommit(trx_id)` answers `{commit_lsn, floor}` under one hold of the
window latch, which is sound because `Reclaim()` erases entries and raises
the floor under that same latch — so a reader sees the pass wholly before
or wholly after, never half of each.

**`CommitLsnOf` is retired rather than kept beside it**, which the ruling
did not ask for and the ruling's own argument requires. AN-R12 chose the
latched pair over a re-read rule because it "removes the straddle rather
than explaining why it is harmless"; leaving a window-only accessor in the
header leaves the straddle one call site away, and the explaining kind is
where this session's defects have been. `Floor()` **stays** as the ruling
says — reclamation's own accounting and `SHOW META` need only the floor —
with the header stating that a caller deciding whether a transaction
committed asks `LookupCommit`.

**The cell** is `AReclaimedWinnerIsNeverAnsweredUncommitted`, and the
invariant it asserts is the one hold buys: for a transaction that has
committed, an absent window entry is an entry **below the floor**. A
reader sweeps 3,000 committed ids while the floor climbs through them in
120 passes; a violation is `commit_lsn == kNoCommitLsn && id >= floor`.
**Mutation**: split the pair into a `Floor()` read outside the latch and a
separately latched lookup — 5 runs, 5 failures, each on the violation
itself rather than on a liveness check.

**Its liveness took three tries, and the first two are worth recording.**
The cell first asserted the reader outlived one sweep — it failed on a
*correct* implementation, because 120 passes over 3,000 ids finish inside
a single sweep. Rewritten to assert the reader saw both live and reclaimed
entries, it failed again: thread construction outran the whole climb, so
the reader started after every pass and saw nothing live. A start barrier
fixed that in isolation but **flaked once in five under `ctest -j8`** —
the flag proves the thread is alive, not that it has read anything, and
under load it can be descheduled for the entire climb. Both liveness
assertions are structural now: the writer waits for the reader's first
*observation* rather than its first instruction, and the reader takes one
unconditional sweep after `done`. Four armed suite runs since, clean.

### 8.3 Suite

**3364/3364 plain and 3364/3364 armed** (`KDS_TEST_PAGE_LATCH=1`), the
armed run repeated four times after the flake was closed and the new cell
run eight times standalone. `KDS_TEST_FRAME_BUDGET=8` not run: neither
stage touches the store. **Overhead not measured** — the suspension is
lifted for AW-S5 alone.


---

## 9. AW-S1b — done, and it was one surgery rather than four slices

**The first half, landed 2026-09-06: the refusal is airtight now, which it
was not.** AM-R4a's chain is *D14 makes every mountable volume one this
build created, and every such volume is single-stream* — and the second
clause was false. `BootstrapDatabase` carried a `log_topology` parameter,
defaulted to `kSingleStream` but **accepting `kPerCoreStreams`**, precisely
so a test could build the one thing nothing else could: a genuine pre-M0
volume. So after `16e6c5c` refused every non-17 image, the per-core
arrangement was still reachable — from tests, through the parameter. The
parameter is gone and the three cells that used it
(`CoreRuntimePerCoreStreamTest`, its fixture and the base's `LogTopology()`
hook) with it. **That is a hole in AM-R4a the ruling does not name**, and it
is the reason the refusal at `16e6c5c` was not the refusal AM-R4a described.

**The second half, landed 2026-09-07: the deletion.** The record below is
what it actually removed and what it cost, because both differ from the
plan this section carried when it was written.

### 9.1 The slicing does not exist — proved, not judged

CLA planned four green-at-each-step slices: (a) `MayFault` and the CC7
fault grants, (b) `TryClaimByStamp` and the stamp readers, (c) the lease
and its dependents, (d) the per-core-stream mount branches. **(a) and (b)
are not separable from (c), and the suite says so rather than a design
argument.**

- Cutting `MayFault` alone left two cells red
  (`APeersOwnPagesSurviveARestartByTheirStamp`,
  `APageStampedByThisStreamIsClaimedWithoutAGrant`), because `MayFault` was
  `TryClaimByStamp`'s *read* trigger — the claim probe read
  `mark_dirty ? !MayWrite : !MayFault`.
- Cutting `TryClaimByStamp` with it then left a **third** cell red,
  `AnOwnerBuiltAssertionIsEnforcingAgainAfterTheOwnersRestart`, whose
  subject is assertion revival and which merely *depended* on the claim:
  `ReviveAssertion` walks the cabin chain and writes it, and on a restarted
  peer's store nothing but the stamp said those pages were that core's.

So the claim is load-bearing exactly while the lease exists, and the lease
is what (c) removes. (d) is genuinely separate and is **not** in this
commit: the per-core-stream mount branches and `page-lsn-cross-stream.md`
stay, and `single_stream()` is still tested in `Expeditor` even though no
volume can answer false.

**One correction to this section as first written.** It said AW-S1b was
"coupled to AU-S0's frozen ring-kind count". It is not:
`grep -n static_assert include/kds/sched/ring_message.hpp` shows only
`sizeof(MessageHeader)`/`alignof` — AU-R4's count freeze is unbuilt, so the
enumerators for the struck kinds stay in place for AU-R5 to strike and
nothing here waits on AU.

### 9.2 What left the tree

Eight files deleted outright — `storage/extent_lease.{hpp,cpp}`,
`server/extent_lease_service.{hpp,cpp}`,
`server/relation_grant_service.{hpp,cpp}` and the two lease suites — plus,
inside the files that stay:

| what | where it was |
|---|---|
| `MayFault`, `HasFaultRight`, `GrantFaultPages`, `RightsRegion::fault`, the `#ifndef NDEBUG` shared-nothing check | `DevicePageStore` |
| `TryClaimByStamp` and its probe, `stamp_claims_` | `DevicePageStore` |
| `lease_`, `SetCoreOwnership`, `GrantWritePages`, `rights_regions_`, `HasWriteRight`, `RefreshFreeMapFromDevice`, `AdoptDeviceMapOnMiss`, `map_refreshes_on_miss_`, `FreeMapBytesForRegion`, `NoteAllocated` | `DevicePageStore` |
| eight `lease_ != nullptr` arms — `EnsureHeaderlessMap`, `EnsureRegionResident`, `FlushMaps`, `IsAllocated`, `CreateAtUnpinned`, `CreateNewUnpinned`, `RaiseAllocationFloor`, `NotAllocated` | `DevicePageStore` |
| `GrantRelationFault`, `GrantRelationWrite`, `AdmitWritePages`, `MaybeRequestRelationGrants`, `MaybeRefillLease`, `PrepareRelationHandoff`, `RelationFaultExtentOf`, `Config::lease`, `lease_`, `refill_`, `grant_demand_`, `grant_request_in_flight_` | `CoreRuntime` |
| the `kRelationFaultGrant` / `kRelationWriteGrant` handlers | `CoreRuntime::AttachTransport` |
| `extents_`, the per-peer reservation, `RegisterExtentGrantHandler`, the whole CC7 publish hook (flush → handoff → two grants → `EvictClean`), `RegisterRelationGrantHandler`, the recovery-seeded extent hint | `Expeditor` |
| PW1c-7's rights probe, `grant_demand_`, `SetRelationGrantDemand`, `RelationWriteRightsPending`, `RelationGrantDemand`, the `extent` refill block in `SHOW META` | `CommandDispatcher`, `core_affinity` |

**`MayWrite` survives as AM-R2 and AO-R14 require, and is now one
question**: `page_id < first_evictable_page_id_ ? CurrentCore() == 0 :
true`. Which core may write a *user* page is the Expeditor's routing
decision; the store answers for the system range alone.

**`storage::Extent` went too**, which the plan did not anticipate: its only
remaining users were `RelationFaultExtentOf` and the grant payloads, and
all three died together.

### 9.3 What it cost the suite, and the fixture change nobody planned

**44 cells** left with the machinery (3359 registered before, 3315 after). Six more had to be restated rather
than deleted, and the reason is worth recording because it is the same
reason four times: **`CoreRuntimeTest` was the last place in the tree still
in the pre-AM-S2-step-3 arrangement.** Its `ConfigFor` gave every peer a
`DevicePageStore` of its own over core 0's device — a private free-map copy
taken at that peer's `Open` and stale from the next thing core 0
allocated — and the lease, the fault grants and `AdoptDeviceMapOnMiss` were
exactly what papered over that. Deleting them turned seventeen cells red
with `page id N not found`. The fixture now passes
`c.shared_store = core0_store_.get()`, which is what `Expeditor` does on
every volume this build can mount.

The six restated cells:

- `APeerDoesNotSeeADdlThatWasNotFlushed` → `APeerSeesADdlThatWasNotFlushedBecauseItReadsTheSameFrame`. The property **inverted** at AM-S2 step 3 and nothing had noticed.
- `InvalidatingTheCatalogRefreshesThePeersFreeMap` → deleted; its mechanism is gone.
- `APeerStoreTakesItsConfiguredFrameBudgetShare` → `APeerOnASharedPoolTakesNoBudgetOfItsOwn`, which is EV4's actual contract.
- `APeersOwnPagesSurviveARestartByTheirStamp` → `APeersOwnPagesSurviveARestart`: it keeps its end-to-end half (600 rows, restart, read and write again) and loses the stamp-claim assertions. **This line was false when first written** — see §9.5 finding 1.
- `APeerReadsTheCatalogAndCannotWriteIt`, `APeerIsWiredWithRecordingOff`, `APeersDdlRunsOnCoreZeroAndItsOwnNextStatementSeesIt` each install the system boundary and ask under a `CurrentCoreGuard`. **This is AW-a's change of meaning arriving in the cells**: `MayWrite` answers "who is asking", so a question put from the test thread is core 0's question whichever runtime's store it names.
- `AnAllocatedPageNeverWrittenIsNotFoundNotCorrupt` and `TheAllocatedCountIsMaintainedNotSwept` lost the `ExtentAllocator` that set up their state; the first now reaches it with `CreateNew` + `PersistMaps` + a remount, which is the same state by the same route.

### 9.4 What did not land, and what changed under it

- **(d) landed 2026-09-07** on `worktree-am-s4d-s3-close` (`5e8b041`,
  `40510cc`), and it closes AM-S4 and with it AM: the per-core-stream mount
  branches, `page-lsn-cross-stream.md`'s file, and every `single_stream()`
  conditional. The finding that shaped it is that **(d)'s own door was not
  shut**: `Decode` still accepted `kPerCoreStreams` inside a version-17
  image, and `CreateFresh` *defaulted* to it, so fifty-odd test superblocks
  claimed a topology no volume can have. See
  `workorder-am-m1-shared-pool.md`'s AM-S4(d) row.
- The **stamp field stays**, and its spec section is corrected rather than deleted: `SetPageStreamStamp` still records which stream's records may name a page, which is redo's business; what went is the *ownership* reading of it (`page-lsn-cross-stream.md` §9 rule 6).
- `docs/inflight/bugs/flushmaps-lease-guard-and-unlatched-region-walk.md`: **defect 1 closed** — the lease guard is gone and the writeback every core now runs writes the one live map, so there is nothing stale to publish. **Defect 2 (the unlatched `map_regions_` walk) was open** and still AM-S3's, because taking the latch means restructuring a loop that calls `device_.WritePage` inside it. **AM-S3 closed it on 2026-09-07** and the file left the tree with it; the walk turned out to be the least dangerous reader in it, and the fix is the free map's own latch (`docs/spec/page.md` §5).

**Suite at the commit: 3314/3314**; 3316/3316 after §9.5's restorations.
Overhead not measured — `CLAUDE.md`'s suspension, and AM-S6 is the stage
that lifts it.


### 9.5 What the `critics-developer` pass found, at `af86026`

The review ran against the committed stage. It found no place where a
deleted guard removed a check something still needed — the eight `lease_`
arms, the publish hook's flush/handoff/grant/evict sequence and
`AdmitWritePages` each carried only the lease's job — and two coverage
holes, both closed. Everything else it found is a claim the tree makes and
no longer earns.

**Two cells it restored.** (1) The record above said
`APeersOwnPagesSurviveARestartByTheirStamp` was *restated*; the `TEST_F` had
in fact been deleted and only its 90-line helper kept, which
`-Wall -Wextra` reported as defined-but-not-used. The whole restart path —
600 rows across several pages, flush, destroy, reopen, write again,
`COUNT(*) = 601` — had no cell. Restored as `APeersOwnPagesSurviveARestart`.
(2) `ASpentLeaseRefusesWithTheWiresRetryableBit` was over-deletion: its
subject is PW6's closed finding about the **row-id and transaction-id**
leases, both of which survive — `status.hpp` was edited in this very stage
to say so — and its only dependence on the struck machinery was two setup
lines. Nothing else asserts that both spellings carry `retryable=1`.

**One live defect, fixed.** The assertion resume's "cannot enforce"
detector (`mount_recovery.cpp`) tested `MayWrite` on a revived Bound Cabin's
chain root. A cabin root is a *user* page, so that arm became unconditionally
false — the reviewer proved it by mutating the condition to `false && …` and
finding the suite still green. What it detected was the PW1c-6c case, a
cabin core 0 built for a peer-owned relation, and **the deletion makes those
enforceable rather than silently unenforced**: the owner appends to the
chain core 0 built, through the frame table they share. The arm is deleted,
`assertion.md` §6.1 gains the fact, and the dispatcher's refusal message no
longer offers a repair (`DROP` then `CREATE`) for a case that cannot occur.
`CannotEnforce` keeps its other callers — a revive that failed, a checkpoint
whose snapshots do not cover the base — so the refusal itself is unchanged.

**Three things the deletion left alive that should not have been.**
`CreateAtUnpinned` lost the only statement in the store that placing a page
at a chosen id is core 0's; re-added keyed the way `MayWrite` now is, on
`CurrentCore()`. `ResidentBytes` kept a `TxnConflict` arm that `!MayWrite`
can no longer reach and whose message named four deleted mechanisms;
collapsed to the one reachable code, and the prefetched-page plumbing the
stamp claim fed goes with it. `exec::CatalogSpillPages` was left with its
one call site inside an `if (false)` block held alive by thirty lines of
justification; the block, the function and the argument are gone, and
`ReferencedSpills` — the sweep's, and a different function — stays.

**One decision filed rather than taken.**
`MaterializeIndexDefinition`'s PW1c-6 refusal is keyed on
`Catalog::on_publish_` being installed, deliberately: an installed publisher
was what made ownership a *handoff* fact, and the P4e harness builds indexed
fixtures through a hook-less catalog. Deleting `Expeditor`'s installer
inverted the predicate — the gate is now **off in production and on in one
test**. Re-keying it on ownership alone would refuse the harness the third
conjunct exists to admit; deleting it removes the catalog-level door this
file's own doctrine keeps. Filed as
`docs/inflight/bugs/publish-hook-gate-is-test-only.md` with the three
options, and the site and the hook's declaration both say plainly that the
refusal no longer fires in production.

**Roughly twenty comments and six spec sentences** described grants, leases
and stamp claims in the present tense and are corrected: CC7's two
"own lease, own-stamped" clauses, CC12's `CatalogSpillPages` citation,
`assertion_build_service.hpp` and `assertion_check.hpp`'s premise that the
owner *cannot* write a core-0-built cabin, `crosscore.md` §6 and
`manual/sql/sql.md`'s `RelationWriteRightsPending`, `core_placement.hpp`'s
"core 0 owns … extent leasing", `lease_refill_stats.hpp`'s three lease
kinds, `keystoneid-k0-findings.md`'s `MayFault`, an orphaned nineteen-line
doc block where `MayFault`'s declaration had been, and two cells that
asserted `MayWrite` has "four callers outside the store" — this stage
deleted three of the four, and the live consumer is the store's own write
gate.

**And one more contradiction the stage created**: CC11 was rewritten here to
say file growth is no longer core 0's alone, while `page.md` §6 and
`page_device.hpp` still said it was — the second using it as the reason the
class needs no internal synchronization. Both corrected, and
`device-growth-is-not-core-0s-any-more.md` now names all four sites.

**Rejected: nothing.** Two findings are deferred by the order rather than by
CLA — the unlatched `map_regions_` walk (AM-S3's, filed) and slice (d) — and
one is deliberately left as a filed decision rather than a patch, above.
`range_alloc.cpp`'s surviving `EvictClean` was removed on the reviewer's
argument that it now evicts the *owner's* frame, and the PL §9 rule 1
ordering beside it is kept whole with its comment narrowed to say its
consumer is gone.

**Suite after the review: 3316/3316.** Overhead not measured.

---

## 10. AW-a — the `MayWrite` repair, split out of AW-S1b

**Why it is its own item.** The L attempt at AW-S1b (§9) got the store-side
deletion written and compiling before it stopped, and on the way it found a
live defect that does not need the deletion to fix. The operator split it
out; this is it.

**The defect had two halves, and the second is the one that matters.**

1. `MayWrite` opened with `if (lease_ == nullptr) return true` — "core 0's
   store, which may write anything" — written when only a peer's store
   carried an extent lease. AM-S2 step 3 made a peer *borrow core 0's
   store*, and `SetCoreOwnership` runs only on an **owned** store
   (`core_runtime.cpp:348`), so every core reached the predicate with a
   null lease and was told yes to everything.
2. Worse: the store carried **two members for one boundary**.
   `system_page_limit_`, which `MayWrite` read, was set only by
   `SetCoreOwnership`; `first_evictable_page_id_`, the EV3 resident floor,
   is *also* set directly by `Expeditor` for core 0's store. On a shared
   store the first was **0**, so `MayWrite`'s system arm was unreachable
   before the early return even got to it.

`MayWrite` has four callers outside the store — `mount_recovery.cpp:303`,
`core_runtime.cpp:1116`, `command_dispatcher.cpp:6641` and `:6643` — that
read it as a real gate, and AM-R2 and AO-R14 both keep it as one. It has
not been one since `2663001`.

**The same shape had already been found once and half-fixed.**
`Expeditor`'s own comment beside `SetResidentLimit` records it: "every
*peer* got this through `SetCoreOwnership`'s `system_page_limit`, and core
0 got it nowhere, so the protection depended on whether a lease was
installed". That was written *about the eviction floor* while the
identical bug sat in the writability boundary one member away.

**The repair.** The two members collapse into one, per `CLAUDE.md`'s "never
add a second name for a quantity an existing setting expresses" — the store
already said they were the same boundary. And the system arm distinguishes
the two arrangements: a **leased** store is a peer's by construction,
whatever thread asks, so it answers `false` exactly as before; a **shared**
store is every core's, so only `CurrentCore()` can say.

**Three existing cells caught CLA's first attempt at this.**
`APeerReadsTheCatalogAndCannotWriteIt` and two others assert the property
and query a peer's store *from the test thread*, where `CurrentCore()` is
0. A repair that answered from the running core unconditionally broke the
leased case while fixing the shared one — the right answer for the wrong
reason. That is why the arms are split rather than unified.

**Cell**: `APeerMayNotWriteTheSystemRangeOnASharedStore`, modelling the
production arrangement (no `SetCoreOwnership`, the boundary installed the
way `Expeditor` installs it). **Two mutations, both killed**: restoring the
null-lease early return, and reinstating a second boundary member.

**What this does not do.** `MayFault` is vacuous in the same way and is
left alone — AM-R4a deletes it at AW-S1b, and giving it a gate days before
removing it would be work with a known expiry. It is a debug-only check
whose call site pre-filters the system range, so unlike `MayWrite` it
gates nothing in a release build.

### What the `critics-developer` pass changed

It confirmed all four steps of the diagnosis and **corrected the mechanism
CLA gave for the third**. The comment claimed nothing caught the defect
because "the one store-side call site pre-filters the system range out" —
there are *two* store-side sites, and the one that is the actual gate,
`ResidentBytes`' `mark_dirty && !MayWrite`, does **not** pre-filter. What
hid it was the other half of the same defect: with `system_page_limit_`
reading 0 the arm could not have fired from any call site even with the
early return gone. A cost note beside it was stale for the same reason —
"core 0 has no lease, so `MayWrite` returns at its first test" stopped
being true when the range test moved above the lease test.

**The cell pinned the predicate, not the refusal.** `MayWrite`'s
consequence in production is `ResidentBytes`' write gate, and the leased
store already had a refusal cell for it
(`AMissingGrantIsRetryableAndASystemPageIsNot`) where the shared store had
none. `ASharedStoreRefusesAPeersSystemWriteAndNotItsUserWrite` is that
cell: a peer's `Get(system page)` is refused non-retryably, its
`Get(user page)` is not.

**The collapse is safe today and was a footgun tomorrow.**
`SetResidentLimit` is public and additive, and it now sets an
*authorization* boundary that is tested before `lease_->Owns()` — so a
later raise would make pages unwritable by the core that owns them. Its
own declaration invited exactly that, naming AST04's Bound Cabin pages,
a use already served by `IsPinnedClass`'s kind half. The contract is
narrowed: volume layout only, install-time only.

**Two findings left open rather than fixed**, both recorded: the same
lease-vacuity defect one function away, plus an unlatched walk of the
shared region map — UB rather than a stale read. **Both closed at AM-S3**
(2026-09-07), and the second turned out to be the smaller half of a wider
defect: the map was *written* unlatched too, so a `find` raced an `insert`. And the gate now
**fails open on a missing identity**: `CurrentCore()` defaults to 0, so a
future thread that touches the store without declaring itself is told it
may write the system range. That is the failure just fixed, reached by a
different omission, and nothing asserts it.

**Suite: 3363/3363 plain, armed, and armed with `KDS_TEST_FRAME_BUDGET=8`.**
Overhead not measured.


---

## 11. AW-S3 — the half that landed, and the half that is a different order

AN-R14 reads as one ruling and is two mechanisms. **The participant half
landed; the instance-wide half is not built**, and the gap is not effort
but the absence of anything to attach to.

### 11.1 What landed

`kShippedTxnIdleCeilingNs` (300 s of **idleness**) is retired into
`kTxnLifetimeCeilingNs` (60 s of **lifetime**), one quantity with one name,
across all 19 references in 7 files — cell (6)'s grep returns only the one
historical mention inside the constant's own doc block. The sweep reads a
new `Enrolled::began_at_ns`, which never moves, instead of
`touched_at_ns`, which moved on every statement.

**The `busy` guard is a safety guard and it now says so.** It was easy to
read as leniency — and cell (2)'s mutation is written against exactly that
reading. It is not: tearing a context down while a statement or a phase is
in flight would pull the session out from under a coroutine holding a
pointer into it. So it **defers** a sweep to the next tick on which nothing
is in flight; it does not exempt. A transaction that keeps issuing
statements is reached between them.

**One existing cell was the old policy, stated.**
`AStatementKeepsItsTransactionAliveAcrossTheCeiling` asserted "idleness,
not age: a transaction still receiving statements is not the thing the
sweep looks for, however old it is" — which is precisely what AN-R14
inverts. It is rewritten as
`ABusyTransactionIsSweptAtItsLifetimeNotItsIdleness`, with
`AnIdleTransactionUnderItsLifetimeIsNotSwept` beside it for the other side
of the predicate.

**Its first version discriminated nothing**, and the mutation said so: it
advanced a full ceiling after the last statement, so *idleness* fired too
and keying the sweep back on `touched_at_ns` left it green. The arithmetic
now puts the context at 1.25 ceilings old and 0.5 ceilings idle, where the
two keys disagree. Mutation kills it.

`txn.md` §1 carries the envelope, the ceiling, the cost and the three
exemptions; §4.1 says the price is per instance now and that the ceiling
is what bounds it — ending the holder rather than failing the read, which
is the difference between the ruling taken and the `SnapshotTooOld` one
declined.

### 11.2 What did not, and why it is a different size

The ceiling reaches **cross-owner participant contexts only**, because
`ShippedStatementExecutor::Expire` is the only sweep in the engine. A
plain local `BEGIN` on one core is reached by nothing:
`include/kds/txn/manager.hpp` has **no clock, no start timestamp and no
sweep** — the AW-S0 fact-check said this and the source read confirms it.
Making the rule instance-wide needs, in order: a clock on
`TransactionManager`, a start per live transaction, a sweep with a cadence,
an abort that surfaces at the next statement, and a way for the sweep to
know which transactions are DDL.

That reshapes the remaining cells:

- **(1) and (2)** hold for participant contexts and are green. For a
  local transaction they are **unbuilt**, not failing.
- **(3) `CREATE INDEX` past 60 s completes** would be **vacuous today**:
  DDL is core 0's and is never an enrolled context, so the ceiling cannot
  reach it and the cell would assert an exemption that nothing enforces.
  It becomes meaningful only with the instance-wide half.
- **(4) a Cabin build re-mints and resumes** is not this mechanism at all.
  The build holds a **read view** (`MintCheckView`), not a transaction;
  "the ceiling applies but the response is re-mint" is a second mechanism
  on a different object, and nothing in the sweep touches it.
- **(5) a prepared context is not swept** was true before this stage and
  is true after it (D4).
- **(6)** is done.

**Sizing.** The order gives AW-S3 M and a three-site consumer list; the
consumer list was 19 sites in 7 files, which is the part that landed. The
instance-wide half is a new field, a new sweep, a new cadence and an abort
path through the dispatcher, with the DDL exemption needing a fact the
manager does not currently carry. That is its own stage and probably its
own letter, since it lands in `txn/` rather than in AM or AN.

### 11.3 Suite

**3364/3364 plain, armed (`KDS_TEST_PAGE_LATCH=1`), and armed with
`KDS_TEST_FRAME_BUDGET=8`.** Overhead not measured.


### 11.4 What AW-S3's `critics-developer` pass changed

**The mechanism is correct on every path; the contract around it was not.**
The pass found no executable defect and rewrote nine places that still
described the retired policy — including `client-manual.md`, which is
user-facing, and `cross-owner-txn.md`, which had taken the rename and left
the semantics ("`kTxnLifetimeCeilingNs`, **300 s of idleness**", with 300 s
still in its settings table).

**CLA's in-doubt hypothesis was wrong, and the answer is better than an
assert.** `kTxnInDoubtCeilingNs` is **200 ms**, three orders below the
ceiling, *and* the prepared arm `continue`s out of the sweep before the age
test is reached — so the two apply to disjoint populations and an ordering
`static_assert` would encode an obligation that does not exist.

**The magnitude dependency is not a constant.** The sweep is registered
under `if (wal_drain_interval_ns > 0)`, so **setting the drain interval to
0 silently disables the lifetime ceiling**. Under the retired constant that
was a backstop going dark; under AN-R14 it is a ruled policy going dark.
Recorded in `txn.md` §1; a code-side warning is proposed, not built.

**`touched_at_ns` had no reader left, and CLA's comment named two that do
not exist** — the in-doubt cadence reads `asked_at_ns` (whose own comment
says it is deliberately separate), and `SHOW META` prints counters, never a
stamp. The commit turned a live field into write-only state and justified
it falsely. The field and its write are deleted here.

**The order's mutation for cell (2) is an equivalent mutant.**
`if (busy || age < ceiling) skip` and `if (busy) skip; if (age < ceiling)
skip;` are the same program by short-circuit, so "exempt busy transactions"
cannot be written as a change and nothing can kill it. §11's earlier
account — that the mutation was written against a misreading — was the
wrong correction. The property *is* testable, as a **sticky** deferral: a
flag that skips a context once deferred. `TheCeilingSkipsAContextAStatement`
`IsRunningOn` now carries the four lines that kill it, which is the half of
"defers, not exempts" that had no cell.

**And one cell CLA added discriminated nothing.**
`AnIdleTransactionUnderItsLifetimeIsNotSwept` was line-for-line the first
half of `AnAbandonedTransactionIsRolledBackAtTheIdleCeiling`. Deleted.

**`kds.txn_lifetime_ceiling` is a name, not a key.** It is not in
`expeditor.cpp`'s registered key set, so the WARN log naming it told an
operator to set something the config loader would refuse. The log names the
constant now; `txn.md` §1 records the key as unbuilt, and registering it is
a config-surface change with its own cell.

---

## 12. AW-S4 — surveyed, one piece landed, and AN-S3 is not separable

### 12.1 What landed

**AN-R9's published commit ceiling**, on `InstanceVisibility`: the highest
commit LSN this instance has *published*, raised inside `PublishCommit`'s
hold and **after** the window entry. That order is the ruling's point — a
snapshot that reads a ceiling covering some commit must be able to find
that commit's entry, and the reverse order would let one snapshot answer a
commit invisible and then visible. `AN-Q3` names that interval as the one
place this design can be implemented wrongly and pass every test.

It is **additive**: nothing mints against it yet, so the cutover becomes a
change to the predicate rather than to the predicate and its input at once.
Its cell pins the ceiling's three properties — covers what is published,
monotone under out-of-order publication, unmoved by an abort.

### 12.2 What the survey corrected

**The cutover's surface is 8 sites, not 70.** A `grep` for the fields being
removed returns ~70 hits, and most are unrelated: `tcp_server`'s
`conn.in_flight`, `lease_refill_stats`, `sim_ring_transport`. The real
consumers of `ReadView::up_to_trx_id` / `in_flight` outside `read_view.hpp`
are `manager.cpp`'s mint and horizon, `step_vm.cpp:851`,
`visibility.hpp:173`'s `sees_everything()`, and the watermark below.

**AN-R9's ceiling did not exist** before this commit, and AN-S2's row reads
"`MintReadView` reads AN-R9's ceiling" as though it did.

**`Visible` cannot stay a pure function on a POD.** Branches 3 and 4 need
the live floor and the window — AN-R3 rules the floor is read live, not
copied — so the view must reach `InstanceVisibility`. AW-S2's
`LookupCommit` is exactly the pair those two branches need, in one hold.
There is no include cycle. The design decision AN-S2 must make and does not
state: the view carries a `const InstanceVisibility*`, or every call site
passes one.

### 12.3 The finding: AN-S3 is not separable

The AN order says AN-S3 "is separable and can be declined without
disturbing the rest". **It cannot be.** The cross-owner watermark **is**
`up_to_trx_id` — `shipped_statement_executor.cpp:263` reads
`held->view().up_to_trx_id` and ships it, and the coordinator compares it
(`command_dispatcher.cpp:6197`, `session.hpp:486`,
`statement_ship_service.hpp:318`). AN-S2 removes the field that value comes
from. So at S2 the watermark must either become the `snapshot_lsn` — which
is most of AN-S3's mechanism — or be removed, which is AN-S3's other half.

That is a decision for the operator or for AN-S2's own read, and it changes
the wire either way. AN-S2's row does not mention the watermark at all.

**Decided 2026-09-07 (operator, verbal): remove it.** Recorded as AN-R5a in
`workorder-an-read-view.md`, which is where AN-R5 already owned the
watermark's retirement. The removal half of AN-S3 therefore lands inside
AN-S2, and what remains of AN-S3 is the coordinator's `snapshot_lsn`
adoption. One thing the mark does not decide is flagged there: between the
removal and that adoption, `cross-owner-txn.md` §3's stated-possible case
has no check standing over it, which is a removed guard reading as a
passing one.

### 12.4 What remains, and its size

`ReadView` reshaped and the four-branch `Visible`; `MintReadView` taking
the ceiling; `ReadHorizon()` changing unit to an LSN with `min_snapshot_lsn`
published and both consumers moved; `Everything()`'s flag with its 13 sites
verified; the watermark decision; and seven cells, **two of which
(H1 and H2) must be shown failing on the current mechanism before they
pass** — which means writing them against a two-core assembly first.

It is the predicate at the heart of MVCC, and every one of the 3364 cells
depends on it. CLA landed the additive half and stopped there rather than
begin a cutover it could not finish and could not leave half-applied: a
half-done MVCC change does not compile, so unlike AW-S1b's deletion there
is no dead-code state to rest in.
