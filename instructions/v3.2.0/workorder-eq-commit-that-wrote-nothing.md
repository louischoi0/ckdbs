# Work order EQ - the commit of a transaction that wrote nothing: no record and no sync of its own, and what a writing commit's sync is spent on

Status: **WRITTEN 2026-11-28, not opened** (`raft-marks-2026-11-28.md`
§25), on the operator's word *"write work milestones for the three causes
behind the current performance gap, and push"*; the first of the three EO
names. Written by CLA in a claude.ai session in the main checkout on
`main` at `c43dbf46` (`v3.4.17-332-gc43dbf46`), documents only, no
worktree. **Every `[source-read]` claim with a `path:line` was read by CLA
at `c43dbf46`**; nothing was built or run.

Tags: `[operator]`, `[source-read]`, `[measured]` (quoted), `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `bench/v3.5.1/results-eo-close-v3.5.1-63-g9543cf720.md` ("What
the numbers say", items 1 and 2); `docs/spec/txn.md` §1 (`READ ONLY`);
`docs/spec/wal.md` §6; EG (`workorder-eg-one-client-durable-write.md`),
DY (`workorder-dy-segment-roll-off-the-latch.md`, open for its proof).

## 0. What EQ is

`[measured]` EO: scenario 5's order status and stock level only read,
inside `C_TXN_BEGIN` ... `C_TXN_COMMIT`, and **every one books
`commit.wal_flush` once, 339.8 µs and 336.8 µs, 78 % and 70 % of its
latency**, where PostgreSQL books none: 0.19x and 0.39x on `strict`.
A writing commit's flush is also dearer (scenario 5's payment 363.6 µs
against 108.2 µs of sampled `WalSync`), and whether that is the sync's
breadth or its count is unverified.

`[source-read]` Why the read-only transaction pays:

- An explicit `READ WRITE` `BEGIN` appends `TXN_BEGIN` at once
  (`src/txn/manager.cpp:153-162`), whether or not anything is written later.
- `COMMIT` short-cuts only a declared `READ ONLY` transaction
  (`src/server/command_dispatcher.cpp:15645-15648`); every other goes to
  `CommitLocal` (`:15655-15667`), then `TransactionManager::Commit`
  (`src/txn/manager.cpp:376-404`), which appends `TXN_COMMIT` and, at
  `strict`, syncs inline (`src/wal/manager.cpp:485-506`); at `group` the
  commit parks on the writer's sync.
- Nothing on that path asks whether the transaction wrote. The
  transaction's trail (`trail_`, `include/kds/txn/transaction.hpp:173`)
  is what a rollback compensates, and `txn.md` §3.6 says every write
  leaves an undo record, the insert included.

`[design]` EQ is: **a transaction that wrote nothing ends with no record
of its own and no sync of its own** (S1, behind EQ-Q1 and EQ-Q2), and
**the writing commit's flush is attributed** - breadth against count -
before any cut is opened for it (S2, a gate).

## 1. The question that decides the design `[quiet-wrong]`

`[source-read]` At `group` and `relaxed`, a commit is published to
readers right after its append (`FinishCommitBody`,
`src/txn/manager.cpp:315`), before a sync makes it durable; only a
`strict` commit above one core is deferred until durable
(`DefersCommit`, `src/server/command_dispatcher.cpp:1592-1596`). **So a view can include commits that are not yet
durable**, and today the read-only transaction's own `COMMIT` sync is what
incidentally makes everything it read durable before the client hears
"COMMIT". Removing that sync without a replacement lets a client act on
data a crash then loses. The same question already stands, unasked, for
`BEGIN ... READ ONLY`, which drops its lease at `COMMIT` with no wait
(`command_dispatcher.cpp:15645-15648`).

The proposal (EQ-Q2 (a)): **a no-write commit waits only until the log is
durable up to the highest commit LSN its views could see**, appending
nothing. In the common case that point is already durable and the wait is
zero; under a burst of `group` commits it is a short wait on a sync
someone else asked for. PostgreSQL's read-only commit waits for nothing;
this is stronger and costs a comparison.

## 2. Rules

- **EQ-R1** `[quiet-wrong]` A transaction is "wrote nothing" only if no
  data, catalog, var-heap, index, assertion-reservation or Cabin witness
  write happened under it; the census of S0 lists every write site and
  shows each one marks the transaction.
- **EQ-R2** `[quiet-wrong]` No client is told `COMMIT` of a transaction
  whose views saw a commit that is not yet durable at the session's rung,
  for `READ ONLY` and the new path alike.
- **EQ-R3** Recovery is unchanged in meaning: a `TXN_BEGIN` without an end
  record is a loser with nothing to undo, or the begin record is not
  written until the first write (EQ-Q1).
- **EQ-R4** A writing commit's path executes nothing new beyond the
  marking of EQ-R1.

## 3. Items for the operator (EQ-Q), CLA's proposals

| item | question | proposal |
|---|---|---|
| EQ-Q0 | Open after EP | **(a)** yes |
| EQ-Q1 | The begin record | **(a)** written at the first write, so a transaction that writes nothing logs nothing; (b) kept at `BEGIN`, the no-write end appending `TXN_ABORT` with no sync |
| EQ-Q2 | The read-only end's durability | **(a)** wait until durable up to the views' highest commit LSN, for `READ ONLY` too (§1); (b) no wait, PostgreSQL's behaviour, documented as a product property |
| EQ-Q3 | The writing commit | **(a)** S2 attributes it (`strace -c` and `-T` of the log's `fdatasync` on scenario 5, syncs a commit, bytes a sync, live segments a sync) and opens a cut only on a cause that is the engine's and not DY's |

### Marks

None. EQ-Q2 decides a durability statement to the client and is not
adopted on the standing go-ahead.

## 4. Stages

- **EQ-S0 census and premise**: every write site and whether it marks the
  transaction; the order of view mint, publish and durability per rung;
  scenario 5's order status and stock level re-measured on `eqbench` at
  `strict` and `group` with `commit.wal_flush` per operation.
- **EQ-S1 the no-write end** (EQ-Q1, EQ-Q2): built, with a cell for each
  rung that a crash after `COMMIT` leaves no read the client saw undone;
  `txn.md` §1 and `wal.md` §6 revised.
- **EQ-S2 the writing commit's flush, attributed** (EQ-Q3): a results file;
  a cut only on the gate.
- **EQ-S3 the close**: scenario 5's read transactions and payment on
  `eqbench`, OLD against NEW interleaved, with PostgreSQL in the same
  window; the standard point cells inside their floor.

## 5. What EQ does not do

- It does not change `group` or `strict`'s meaning for a writing commit.
- It does not enter DY's segment roll or EG's sync breadth beyond
  attributing.
- It does not make `READ ONLY` the default of `BEGIN`.

## 6. Stage log (CLA, appended as stages land)

### Written 2026-11-28

In the main checkout on `main` at `c43dbf46`, documents only. Not opened;
no build, no run.
