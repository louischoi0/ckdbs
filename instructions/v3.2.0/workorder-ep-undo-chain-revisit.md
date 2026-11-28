# Work order EP - a read refused "undo chain revisits the record": whether an undo record is recycled while a reader can still reach it, decided before anything is fixed

Status: **CLOSED 2026-11-28 on the work branch, every stage landed**
(`raft-marks-2026-11-28.md` §26, §27), opened on the operator's standing
go-ahead on worktree `ep-undo-chain-revisit` from `012820e3e`: S0, S1 and
S2 at `21dc920b4`, S3 the close. **The refusal was a false refusal**: a
stale link into a recycled undo page, never followed, was asked of the
cycle guard before the walk's verdict on it, and no wrong version could
come of it (§7). EP-Q0, Q1, Q3 (revised) and Q4 were adopted as CLA
proposed on that go-ahead; EP-Q2 was not reached. Written 2026-11-28
(§25) by CLA in the main checkout at `c43dbf46`, documents only; the
`[source-read]` claims of §1 were read there.

Tags: `[operator]`, `[source-read]`, `[measured]` (quoted), `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/inflight/bugs/undo-chain-cyclic-on-a-concurrently-updated-row.md`
(the defect, verified at `e571ae574`); `bench/v3.5.1/results-eo-close-v3.5.1-63-g9543cf720.md`
("What the numbers say", item 7); `docs/spec/txn.md` §3.7, §4.1;
`include/kds/txn/undo_log.hpp`; `include/kds/base/cycle_guard.hpp`;
EN (`workorder-en-two-core-cells.md`), which this order does not enter:
the defect is seen at `cores = 1`.

## 0. What EP is

`[measured]` EO's close counted **10 reads refused `Corruption`, "undo
chain revisits the record at <ptr>; the links are cyclic", in 5 of 69 KDS
runs**, each a read of a row other sessions were updating, all at
`cores = 1`: scenario 0's reporter walk (9, `group`) and scenario 5's new
order, a point read by key of a row the reading transaction had just
updated (1, `strict`). Every consistency check passed afterwards.

`[quiet-wrong]` **The refusal is the lucky outcome of the cause CLA reads
as likelier.** The cycle guard is sound only over a walk that never
legally revisits an id (`include/kds/base/cycle_guard.hpp:17-20`), and the
undo log's own statement of that soundness is "no record is rewritten
while a reader can reach it" (`include/kds/txn/undo_log.hpp:178-180`). A
revisit therefore means either that statement is false somewhere - an
undo page recycled under a reader, whose stale pointer then lands on a
newer record - or that some walk legally passes one record twice. In the
first case **the same cause can return a wrong version with no refusal**,
whenever the stale pointer lands on a record whose links do not loop.

EP is: prove which (S0), fix the one it is (S1), and pin it with a cell
that fails before the fix (S2). No figure of EO's is re-read until S1
lands, since an answer the defect could have produced is in every cell
that ran a concurrent update.

## 1. The tree as read `[source-read]`

- The walk: `src/txn/visibility.cpp:18` starts a `CycleGuard` at the
  header's `undo_ptr`; each step reads a record by its pointer
  (`:20`), moves to `prior_trx_id`/`prior_undo_ptr` (`:46-47`) and refuses
  a revisit (`:48-51`). `UndoLog::Walk` does the same (`src/txn/undo_log.cpp:203-218`).
- The records: addressed by byte offset within a page; **the purge unit is
  a whole page**, settled by a per-page maximum writer id kept in memory,
  reclaimed from the chain's old end before a growth allocates, and
  allocation prefers the recycle list (`undo_log.hpp:84-93`). "A freed
  page's stale `undo_ptr`s are harmless by the fact above" (`:91-92`).
- The bound the purge is handed (`include/kds/txn/manager.hpp:306-321`):
  **the commit-order floor, not the read horizon**. The argument: below the
  floor every writer is resolved and visible to every live and future
  snapshot, and "the horizon's other half - committed at or below the
  oldest live snapshot - ... reaches the undo purge only through the floor,
  which reclamation holds below any commit a live snapshot cannot see".
  The purge is therefore exactly as sound as that last clause.
- The reader that hit it most: scenario 0's reporter is a walking
  `SELECT` over a BTREE relation, which **yields every 64 pages holding
  its view** (`CLAUDE.md`, Cross-core execution; `sched.md` §3). A view
  held across yields while four traders commit updates to the same rows is
  the case the floor's clause must cover.
- The other shape: a point read by key, inside the explicit transaction
  that has just updated the row. Its own write should be visible to its
  own view without a walk (`txn.md` §5's verdict table, "`T` itself"); that
  it walked at all is unexplained.

## 2. Hypotheses `[design]`

- **H1 (premature recycle)** `[quiet-wrong]`: some live view is not held
  by the floor - a yielding walk's, a keyed read's, a view minted between a
  commit's append and its publication - so an undo page a reader can still
  reach is recycled and rewritten. Predicts: the refusal needs an undo
  growth (a recycle) between the reader's header read and its walk; a
  pages-recycled counter moves in every failing run; disarming the purge
  makes the refusal vanish.
- **H2 (a walk that legally revisits)**: a record is legally reached twice
  - for instance a row updated twice by one transaction whose second
  record's prior points at its first, read by a walk that starts from a
  stale header copy. Predicts: refusals with the purge disarmed; the chain
  dumped at the guard holds no record of a different row.
- **H3 (a header read torn from its walk)**: the reader copies the header
  and the leaf is released before the walk, and a writer's in-place change
  of the same slot's header is seen half old, half new. Predicts: the
  dumped header's `trx_id` and `undo_ptr` disagree with every committed
  state of the row.

The scenario 5 shape (a transaction reading its own update) weighs against
H1 alone and is why H2 and H3 stay open.

## 3. Rules

- **EP-R1** `[quiet-wrong]` No undo record is rewritten while any live
  view, lease or walk can reach it; the rule is proved for every reader
  kind the census of S0 lists, not argued for one.
- **EP-R2** The cycle guard stays a cycle check (no length limit,
  `cycle_guard.hpp`); it is not loosened to make the refusal go away.
- **EP-R3** A fix that changes what the purge may reclaim is measured for
  undo growth on scenario 0 and scenario 5 (pages live, pages recycled),
  and the price is stated.
- **EP-R4** Zero overhead at one core where no concurrent writer exists;
  nothing new on a reader that does not walk into undo.

## 4. Items for the operator (EP-Q), CLA's proposals

| item | question | proposal | mark |
|---|---|---|---|
| EP-Q0 | Open EP before EQ, ER and ES | **(a)** yes: a possible wrong version outranks every price | (a), adopted on the standing go-ahead (§7) |
| EP-Q1 | The diagnostic | **(a)** a debug-build dump at the guard (the chain's records, each record's page, the page's recycle epoch, the reader's view and lease, the header as copied) plus an arm with the purge disarmed, run on EO's reproduction (`eqbench --scenario s0 ... --set txn=1 --set fk=1`) | (a), adopted on the standing go-ahead; the Debug arm cut to 10 runs (§7, S0) |
| EP-Q2 | If H1: the bound | **(a)** the purge is handed the read horizon as well as the floor (a page is reclaimed only below both), measured; (b) the missing view registered with the floor, case by case | **not reached**: S0 disproved H1, so the purge's bound is unchanged and no mark is needed |
| EP-Q3 | Until EP closes | **(a)** EO's concurrent-update figures are marked in their results file as measured on an engine carrying this defect; no figure is withdrawn | **(a) revised, then adopted** on the standing go-ahead: the premise "an answer the defect could have produced is in every cell" no longer holds after S0, so the mark says instead that the refusals were false refusals, now closed, and that no figure is in doubt; no figure is withdrawn |
| EP-Q4 | S2's sim oracle extension (a walked version compared with its committed history) | **(a)** not built in EP: it is a detector for H1's wrong version, which S0 excludes by proof and by the disarmed arm, and the cause is pinned by a deterministic cell; (b) built anyway, as a standing guard | written at S2; (a), adopted on the standing go-ahead |

### Marks

None by the operator. EP-Q0, EP-Q1, EP-Q3 (revised) and EP-Q4 were
adopted as CLA proposed, on the operator's standing go-ahead of 2026-11-28
(go-ahead-achieving-milestone, `raft-marks-2026-11-28.md` §26). EP-Q2
changes what a purge may reclaim and would have waited for the operator's
mark. It was not reached.

## 5. Stages

- **EP-S0 census and proof**: every reader that walks into undo (walking
  and keyed `SELECT`, a write's qualification, `FOR UPDATE`, the index
  backfill, the query layer), each with what holds its view against the
  floor; EP-Q1's dump and disarmed arm on EO's reproduction, Release and
  Debug, 30 runs each; H1, H2 or H3 named with the run that shows it.
- **EP-S1 the fix**, for the hypothesis S0 proves; EP-Q2 if H1.
- **EP-S2 the cell**: a deterministic rig or sim seed that produces the
  refusal (or, for H1, the wrong version) before S1 and not after; the
  sim's oracle extended to compare a walked version with its committed
  history.
- **EP-S3 the close**: EO's reproduction 0 refusals in 60 runs; scenarios
  0 and 5 on `eqbench` with undo pages live and recycled beside EO's; the
  bug file closed with the commit that closed it.

## 6. What EP does not do

- It does not change the undo record format, the tuple header (Hard
  Invariant 12) or the guard's algorithm.
- It does not touch the two-core wake or the first-updater-wins loss (EN).
- It does not re-run EO's whole matrix; EP-Q3 marks it instead.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-28

In the main checkout on `main` at `c43dbf46`, documents only. Not opened;
no build, no run.

### Opened 2026-11-28

On worktree `ep-undo-chain-revisit` (branch `worktree-ep-undo-chain-revisit`)
from `012820e3e` (`v3.5.1-78-g012820e3e`), on
`/go-ahead-achieving-milestone EP` (`raft-marks-2026-11-28.md` §26).
EP-Q0 (a), EP-Q1 (a) and EP-Q3 (revised, below) adopted on the operator's
standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone); EP-Q2 not
reached, since S0 disproved H1.

### EP-S0: census and proof

**Verdict: neither H1 nor H3. The cause is a form of H2: a stale link that
no walk follows, refused because the guard was asked before the
verdict.** The refusal was a false refusal, and no wrong version could
come of it.

`[source-read]` at `012820e3e`. **The census** (every reader that walks
into undo, and how):

| reader | walk | where the guard is asked |
|---|---|---|
| the step VM's walk and point reads (walking and keyed `SELECT`, `FOR UPDATE`, the query layer's native walks) | `ResolveThroughUndo` | **before** `Classify` decides whether the link is followed (`src/txn/visibility.cpp:48`) |
| a write's qualification (`UPDATE`/`DELETE` over a snapshot) | `ResolveThroughUndo` (`src/server/command_dispatcher.cpp:13804`) | the same |
| the foreign key check's moved rows | `ResolveThroughUndo` (`src/exec/fk_check.cpp:360`) | the same |
| the index backfill | `UndoLog::Walk` (`src/exec/index_ddl.cpp:379`) | after the visitor returns `kContinue`, so only of a followed link |
| recovery's undo phase | the transaction chain (`txn_prev_undo_ptr`, `src/txn/recovery_undo.cpp:271`) | a loser's own records, which no purge takes |

**The proof that no followed link lands on a recycled record.** A walk
reads the record at `undo_ptr` only when the view cannot see `trx_id`,
the writer of the pair `(trx_id, undo_ptr)`. That record was appended by
`trx_id` (`AppendUndo` appends under `txn.id_`, and every header pair is
the new writer's or a pair copied from a record). So its page's in-memory
maximum is at least `trx_id`. The page is recycled only once the floor is
above that maximum (`ReclaimSettledPages`). The floor only rises, and
`Visible(t)` answers true for every `t` below it. So after a recycle,
every view sees `trx_id`, and no walk reads the record. **A link into a
recycled page exists, and is never followed.** But it can equal an
address the walk has already visited. The first record a growth writes
into a recycled page lands at the offset that page's first record had.
When that growth supersedes the version whose record was that first
record, the new record's `prior_undo_ptr` is its own address. The guard's
mark starts at that address, so asking it about the link before the
verdict reads as a cycle.

`[measured]` On EO's reproduction (`eqbench --scenario s0 --set users=300
--set assets=100 --set seconds=6 --set traders=3 --set profit_users=20
--set txn=1 --set fk=1`, `cores = 1`, `group`, a fresh server a run), each
binary built from `012820e3e` in this worktree:

| arm | binary (sha256 prefix) | runs | runs refused | refusals dumped |
|---|---|---|---|---|
| Release, unmodified | `33fb581da4380d4c` | 12 | 1 (5 lines) | - |
| Release, a dump at the guard | `c162c02932825f3d` | 30 | 13 | 22, **every one `start == at == next`** (a self-link), its prior writer visible to the view and below the floor |
| Release, the dump and the purge disarmed | `152af2cb9e441217` | 30 | **0** | - |
| Debug, the dump | `b93df50cca311f36` | 10 (cut from 30 to keep the stage in its 20-minute budget) | 6 | 12, every one a self-link of the same kind |

The refusals came from the reporter's walk and the profit scan, both
walks over `s0_accounts`. The scenario 5 shape (a new order's read) was
not run. It is the same walk over another relation, and `fk_check`'s and
the write path's walks share the same `ResolveThroughUndo`. **Unverified**:
that the scenario 5 refusal was this cause. The S3 run is what would show
it.

`[quiet-wrong]` re-read: H1's wrong version needs a followed link into a
recycled page, which the proof above excludes, and the disarmed arm's 0
of 30 against the armed arm's 13 of 30 shows the recycle is the trigger.
The diagnostic code (an `fprintf` at the guard, an environment switch on
the purge) was never committed.

### EP-S1: the fix

`ResolveThroughUndo` asks the guard only of a link it follows: the
verdict on the stepped-back writer comes first, and the guard is asked
only when that verdict is `kNeedsUndoWalk`, which `ClassifyFor` answers
only for a link that is not `kNoUndoPtr`. The guard's algorithm, the
purge's bound and the record format are unchanged (EP-R2, §6). EP-R3 does
not apply, since nothing the purge may reclaim changes. EP-R4: the same
work per step, in another order. The soundness statements in
`include/kds/txn/undo_log.hpp` now say that a stale link is never followed
and never asked of the guard.

### EP-S2: the cell

`tests/visibility_test.cpp`: `StaleLinkTest.AStalePriorPointerIntoARecycledPageIsNotARevisit`
builds the self-link deterministically (four records a page, the horizon
past page A's writer, the next growth recycling A under a write that
supersedes A's first record), and fails before S1 with the server's
message, "undo chain revisits the record at 8388664". After S1 it reads
writer 50's version. `ACycleTheWalkWouldFollowIsStillRefused`, an ordering guard that passes before S1 too, keeps the
same self-link refused for a view that cannot see writer 50. The sim
oracle's extension (§5, "a walked version compared with its committed
history") is **not built**: it was asked for the wrong version H1 would
give, and S0 excludes that.
This is EP-Q4 (a).

**Review of S0 to S2** (`critics-developer`, one pass over the fix, the
cells and the comments, before the commit): no correctness defect. It
confirmed that every real cycle is still refused (`ClassifyFor` answers
`kNeedsUndoWalk` only for a link that is not `kNoUndoPtr`). It confirmed
S0's proof on every path read: every write path stamps the appender's pair
or keeps a pair whole; rollback and redo restore a pair that was a valid
header; slot reuse stamps `undo_ptr = 0`; and catalog rows, the one pair
mixing a new id with an old pointer, carry `undo_ptr = 0` by contract. It
confirmed that `UndoLog::Walk` and recovery's walk need no change.
**Applied**: the two cells' shared setup made one fixture helper (it now
asserts the recycle in both); the mechanism explained once, in
`undo_log.hpp`'s purge note, with a pointer from `visibility.cpp`; the
second cell labelled an ordering guard. **Not applied, recorded**: R1, the
foreign key check's moved rows read under a lease-less check view after
the leaf visit, a cross-core window outside EP's cause and §6's scope,
written into `known-gaps.md` (Foreign keys) with R2, a `tuple_key`
comparison in `ResolveThroughUndo`, as CLA's proposal for it.

**Suite at S0 to S2** (`ctest -j4 -LE bs-nightly`, Debug, on the tree
committed with these stages): **5,442 of 5,446 passed**; not a green run.
The four failures each passed 3 of 3 when run alone:
`DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter` (the flake in
CLA's memory), `IndexReclaimJobTest.ACrashAtEveryRecordOfAReindexMountsTheOldIndexOrTheNewNeverNeither`,
`KwpLoadServerTest.ALoadSessionOutlivesTheLifetimeCeilingThatEndsATextTransaction`,
and `DropTableReclaimCrashTest.AReclaimCutAnywhereIsFinishedByTheNextMount`.
The last one failed alone on `/tmp` with "Disk quota exceeded" (other
sessions' scratch held about 8 GB of the host's quota) and passed 3 of 3
with `TMPDIR` on disk. Overhead not measured; measured at the milestone's
close.

### EP-S3: the close

`[measured]` `bench/v3.5.1/results-ep-close-v3.5.1-79-g21dc920b4.md`
(`ck-tester`): base `012820e3e` against close `21dc920b4`, interleaved,
`cores = 1`, server on CPUs 0 to 7, data on disk, 180 runs, none
invalidated.

- **Refusals.** On EO's reproduction, the base refused in 16 of 70 runs
  (18 refusals) and the close in **0 of 70**. Adding CLA's 60 runs of the
  close's source (§7, S0), the close refused 0 of 130 runs, which bounds
  the rate below 2.3 % at 95 %. Scenario 0 `txn=0` and scenario 5: 0 on
  both arms in 10 runs each. **Scenario 5's one EO refusal was not
  reproduced on the base**, so its cause stays unverified.
- **Overhead: none resolvable.** On the fast runs of scenario 0, the rate
  was +1.1 %, p50 -0.6 %, p99 -1.0 % and server CPU per transaction
  -2.3 %, against a 20 % run-to-run spread. Scenario 0 `txn=0` (-4.6 %)
  and scenario 5 (-4.0 %, Mann-Whitney p 0.45) are inside their spreads.
  The host ran at two speeds in both arms. The order-reversed pairs showed
  that the slow runs follow order, not binary.
- **Undo growth (EP-R3, as a statement).** Two pages live through
  scenario 0. Recycled pages per 1,000 operations were 32.8 on both arms in
  scenario 0, and 329 against 333 in scenario 5. EO recorded neither
  counter.
- **Cut**: scenario 0 `txn=0` and scenario 5 were run shortened, in 10
  pairs each, with no delta against EO's figures for those cells; no row
  sweep; no PostgreSQL floor and no two-core cell.

EP-Q3 is applied: a note heads `results-eo-close-v3.5.1-63-g9543cf720.md`.
The bug file `docs/inflight/bugs/undo-chain-cyclic-on-a-concurrently-updated-row.md`
is deleted in the close commit; `git show 21dc920b4:<path>` retrieves it.
