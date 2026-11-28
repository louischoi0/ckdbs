# Work order EP - a read refused "undo chain revisits the record": whether an undo record is recycled while a reader can still reach it, decided before anything is fixed

Status: **WRITTEN 2026-11-28, not opened** (`raft-marks-2026-11-28.md`
§25), on the operator's word *"write the milestone that resolves the newly
found bug, and push"*. Written by CLA in a claude.ai session in the main
checkout on `main` at `c43dbf46` (`v3.4.17-332-gc43dbf46`), documents
only, no worktree. **Every `[source-read]` claim with a `path:line` was
read by CLA at `c43dbf46`**; nothing was built or run, and every number
is quoted from the record it names.

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

| item | question | proposal |
|---|---|---|
| EP-Q0 | Open EP before EQ, ER and ES | **(a)** yes: a possible wrong version outranks every price |
| EP-Q1 | The diagnostic | **(a)** a debug-build dump at the guard (the chain's records, each record's page, the page's recycle epoch, the reader's view and lease, the header as copied) plus an arm with the purge disarmed, run on EO's reproduction (`eqbench --scenario s0 ... --set txn=1 --set fk=1`) |
| EP-Q2 | If H1: the bound | **(a)** the purge is handed the read horizon as well as the floor (a page is reclaimed only below both), measured; (b) the missing view registered with the floor, case by case |
| EP-Q3 | Until EP closes | **(a)** EO's concurrent-update figures are marked in their results file as measured on an engine carrying this defect; no figure is withdrawn |

### Marks

None. EP-Q2 changes what a purge may reclaim and is not adopted on the
standing go-ahead: it waits for the operator's mark.

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
