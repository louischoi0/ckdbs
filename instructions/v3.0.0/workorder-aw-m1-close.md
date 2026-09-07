# Work order AW — M1 close and the read-view cutover

Written 2026-09-06 by CLA against `origin/main` at `15a57c2`. Letter AW
(AT = Uniformity, AU = ring retirement, AV = the two-core rig, all
reserved). AW is a **sequencing order**: it opens no new milestone of its
own and finishes two that are open — M1 (AM) and AN — and writes the one
document (AV) that gates what comes after them. Every `path:line` is
`[source-read]` at `15a57c2`; the rest is `[design]`; the one
`[measured]` deliverable is AW-S5's and does not exist until it is run.

**Status: AW-S0, S1, S2, S6 done and AW-S3 *half* done, plus AW-a (§10);
AW-S1b *begun and not done*, 2026-09-06/07 on `worktree-aw-m1-close`.**
§11 says which half of AN-R14 landed and why the other is a different
size. §9 is what AW-S1b found and
where it stopped; it is the stage's sizing that is wrong, not its ruling. §7 is AW-S0's record — its cell was already satisfied when
the stage opened, and it carries the one thing this order was written
without knowing. §8 is what S1 and S2 built. **AW-S1b was inserted by the
operator after AW-S0's fact-check** (§0 item 4); it, S3, S4, S5 and S6 are
not started.

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
`CoreRuntimePerCoreStreamTest` and `page-lsn-cross-stream.md` are all
still in the tree at `15a57c2`, against AM-R4a's "the two halves arrive
together or not at all" — so AM-S4 is open beside S5 and S6. **AW-S1b
carries it**, on the operator's decision of 2026-09-06 (§0 item 4); this
order carried no stage for it as first written. AN: S0/S1 landed; S2 gated until now. AO:
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
| AW-S5 | **AM-S6** — the baseline, per AW-R1 | one results file per AL-S8 cell; the delta table; H3's verdict per cell in the file's header, `[measured]` with the invocation | M | S1, **S1b**, S4, and the operator's host |
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

## 9. AW-S1b — begun, not done, and the sizing is the finding

**What landed: the refusal half is airtight now, which it was not.**
AM-R4a's chain is *D14 makes every mountable volume one this build created,
and every such volume is single-stream* — and the second clause was false.
`BootstrapDatabase` carried a `log_topology` parameter, defaulted to
`kSingleStream` but **accepting `kPerCoreStreams`**, precisely so a test
could build the one thing nothing else could: a genuine pre-M0 volume. So
after `16e6c5c` refused every non-17 image, the per-core arrangement was
still reachable — from tests, through the parameter. The parameter is gone
and the three cells that used it (`CoreRuntimePerCoreStreamTest`, its
fixture and the base's `LogTopology()` hook) with it. Nothing can now
create or mount a per-core-stream volume, so `single_stream()` is true on
every volume that exists and the machinery is provably dead.

**That is a hole in AM-R4a the ruling does not name**, and it is the
reason the refusal at `16e6c5c` was not the refusal AM-R4a described.

**What did not land: the deletion.** It is enumerated rather than
estimated:

| what | where |
|---|---|
| `lease_` and `LeasedIdSource` | 18 sites in `device_page_store.cpp` alone; 15 in `src`+`include`, 42 in tests |
| `MayFault`, `HasFaultRight` | 16 in `src`+`include`, 19 in tests |
| `TryClaimByStamp`, `GetPageStreamStamp` | 19 in `src`+`include`, 14 in tests |
| the per-core-stream mount branches | `core_runtime.cpp`'s `single_stream()` else-arms: the log device, the recovery pass, the anchor |
| `page-lsn-cross-stream.md` | leaves the tree |

**26 files.** And it is not a symbol sweep: `lease_` gates `MayWrite`'s
grant arm, `AdoptDeviceMapOnMiss`, `RefreshFreeMapFromDevice`'s peer arm
and `TryClaimByStamp`, while **AM-R2 keeps `MayWrite`** and AO-R14 keeps
it "while owner routing is in force". So the work is removing one
predicate's *lease arm* from four call paths that must keep their other
arms — surgery on the allocator and the free map, with recovery downstream
of it — not deleting a symbol and its callers.

**AW-S1b is L, not M**, and it wants its own `critics-developer` pass and
its own cells over the allocator rather than riding the suite. CLA stopped
rather than produce a large under-reviewed deletion across recovery and
allocation in one pass, and rather than leave it half-applied against
AM-R4a's own "together or not at all".

**What the tree is in the meantime**, stated so nobody reads it as done:
the machinery is dead code, unreachable from any volume and from any test.
That is the same shape `16e6c5c` left — refusal without deletion — with
the refusal now complete instead of leaky. AW-S5 still must not run before
the deletion lands: §0 item 4 is that AM-S6 measured against an engine
carrying the old guards is not M1's overhead, and dead code that still
compiles into the binary is exactly such an engine.


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

**Two findings left open rather than fixed**, both recorded:
`docs/inflight/bugs/flushmaps-lease-guard-and-unlatched-region-walk.md` is
the same lease-vacuity defect one function away, plus an unlatched walk of
the shared region map — UB rather than a stale read. And the gate now
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
