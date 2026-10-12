# A strict commit's marker caps every core's snapshot across its sync

**Narrowed by BA-S1c** to another session's commit: a session's own commit
is now waited for. The entry as it stood before is at
`git show dfabae1:docs/inflight/bugs/a-strict-commits-marker-caps-every-cores-snapshot-across-its-sync.md`.

## What is wrong

While a `strict` commit syncs, no new snapshot on any core can cover a
commit appended after that strict commit's. That includes a commit already
acknowledged to its client.

Verified against the source at `8bf26a91` (origin/main `0c1b27b7` merged
into this branch).

**A commit holds a marker from before its append until after its publish.**

- `Commit` stores its snapshot marker before its append (`pending.emplace`,
  `src/txn/manager.cpp:398`); `CommitDeferred` stores
  `deferred_marker_` before it stages its record (`:366`).
- `FinishCommitBody` publishes (`PublishCommit`, `:324`) and then lifts the
  marker (`pending.reset()`, `:334`).
- **A marker must sit below its own commit's LSN** (AN-R9,
  `include/kds/txn/instance_visibility.hpp`), so every commit appended after
  the strict one, on any core, stays above the cap whatever the bound is.
  The bound's looseness - `BeginCommit`
  (`src/txn/instance_visibility.cpp:73`) stores the published ceiling as
  this core sees it at the commit's start, and an earlier open marker on the
  core keeps the slot - only widens that to the commits appended between the
  marker's store and the strict append.

**Every new snapshot is capped at the lowest held marker.**
`SnapshotCeiling()` returns that cap (`src/txn/instance_visibility.cpp:135`),
and every mint reads it (`src/txn/manager.cpp:96`, `:139`).

**Under `strict` (D1), the marker's span includes the sync.** Under D2 the
span between append and publish is the append alone. Under D1:

- when the dispatcher defers the commit (`DefersCommit`,
  `src/server/command_dispatcher.cpp:1089`: `strict`, a reactor that may
  park, a WAL writer that kicks waiters, and not a DDL transaction - BA-S7,
  above one core), the commit stages its record with its marker held
  (`CommitDeferred`, `src/txn/manager.cpp:357`), parks, and publishes and
  lifts in `FinishDeferredCommit` (`:377`) after the WAL writer's sync;
- otherwise - one core, the synchronous `Dispatch` path, a DDL transaction -
  `Commit` syncs inline, inside the marker.

**What BA-S1c closed.** A session records its last commit's LSN, and a
statement whose bound sits above the ceiling waits at the statement boundary
until the markers below it lift (`CommandDispatcher::UncoveredCommit`, asked
by `DispatchAsync`, `src/server/command_dispatcher.cpp:715`, and by
`Dispatch`, `:1134`; `docs/spec/txn.md` §4.1). The session that committed
therefore always sees its own commit.

**What is left.** A commit acknowledged to **another** session before a
statement began, yet above the cap, is still not seen by that statement.
That contradicts `docs/spec/txn.md`'s level table's promise that under `READ
COMMITTED` a statement *"sees everything committed before it began"* - which
the table carries as a disclosed exception (`txn.md:92`).

The header's design note (`include/kds/txn/instance_visibility.hpp`) argues
the marker for AN-Q3 and for monotonicity, and since BA-S1c names this
remainder.

**Classes mix on one instance.** The durability class is per transaction
(`docs/spec/wal.md` §1), so `strict`, `group` and `relaxed` commits can run
side by side.

**Reproduced** at `6b0d8412` on `worktree-fix-strict-marker-other-session`, on
every run (3 of 3), by the two-core rig: the cells
`StrictMarkerSnapshotRigTest.DISABLED_AnotherSessions*`
(`tests/strict_marker_snapshot_rig_test.cpp`), disabled until the fix below
lands. Session B's `SELECT` answered no row. **Reproduced again** with the
cells asserting the answer alone, at `8bf26a91` plus this entry's commit,
`--gtest_also_run_disabled_tests --gtest_repeat=3`: both red 3 of 3 (`id`,
no row), the four enabled cells green 3 of 3. The own-session case was
reproduced and fixed by BA-S1c at `fb94070`.

## Smallest reproduction of what is left

1. Start with `cores = 2`.
2. On core 0, a session commits `strict`, paused inside its `fdatasync`
   (the rig's `gated_log_sync`).
3. On core 1, session A commits `relaxed` and is acknowledged.
4. Session B, also on core 1 or on any core, which learned of A's commit
   from outside the engine, selects the row. It is missing until core 0's
   sync returns.

## What was tried, and why it did not land

A statement boundary that waits for the instance's published commit ceiling
(`max(session's last commit, CommitCeiling())`, centred on
`CommandDispatcher::UncoveredCommit`) turns both cells green and passes the
suite. It is `d3fe7a3a` and its review `a80de010` - `6b0d8412..a80de010`
touches five files outside the rig test and this entry, +109/-46:
`command_dispatcher.cpp/.hpp`, `instance_visibility.hpp`, `manager.hpp`,
`docs/spec/txn.md` - reverted by `57e55308`. It was measured, `cores = 2`,
`relaxed`, against `6b0d8412`
(`bench/v3.0.0/results-boundary-wait-overhead-v2.7.0-749-ga80de010.md`):

- a write costs about 1 percent more (+0.6 to +1.0 us), the statement after a
  commit paying for the ceiling walk;
- **a read-only session beside a `strict` committer and a `relaxed` one lost
  89 percent of its throughput** - 13,401 to 1,402 QPS, p99 128 us to
  4.7 ms - about 38 percent of its reads waiting roughly 1.7 ms each.
  **None of those waits was needed**: in that cell the bystander read only
  `t`, the `strict` commit wrote `w` and the `relaxed` one `m` (the results
  file's "Rows" paragraph: "no cell writes the relation it reads"), so no
  wait could have changed an answer.

The wait is the method's, not the marker's looseness: tightening a marker to
its commit's own LSN would cut the waits that are not needed, but a statement
that begins while a `strict` commit is unpublished below a published one must
wait for it under any snapshot that is one number. It is not a change CLA
makes on its own; the answer is the fix below.

## What it costs

- **A quiet wrong answer.** A statement misses a commit acknowledged to
  another session before it began: a stale read, never an uncommitted or
  torn row, for as long as the marker is held - one `strict` sync, or longer
  when that commit queues behind the sync the writer is already running
  (`instance_visibility.hpp`; `txn.md` §4.1). Scenario 0's `cores = 1`
  `strict` cell implies about 1.5 ms per sync
  (`bench/v3.0.0/results-scenario0-stockmarket-v2.7.0-531-g9a0525d.md` §5, at `36449d28`);
  the measured bystander's p99 sat near two of those.
- **The same miss in a foreign-key check, found by reading, not run.** A
  check view mints through `MintCheckView` (`CheckView`,
  `src/server/command_dispatcher.cpp:3614`), capped by the same
  `SnapshotCeiling()`, so a child `INSERT` can be refused for a parent row
  acknowledged to another session.
- **A parallelism cost.** For the length of every `strict` sync, every
  core's snapshots stop advancing. The marker also holds back the window's
  reclaim (`instance_visibility.hpp`), so the window grows, and more rows
  sit above the floor and take its latch. Since BA-S1c a session's statement
  after its own commit can also wait out that sync.

## The fix

**BA-Q3 (c)**: snapshots carry their in-flight set, so no commit holds a
marker across its sync. That is a `txn.md` §4 redesign and would be its own
order (`instructions/v3.0.0/workorder-ba-parallelism.md`). Unmarked. Its
scope is itself a decision: every in-flight transaction, or only the commits
appended and not yet published - a mint covering `CommitCeiling()` and
carrying those commits as exceptions, bounded by cores times open commits,
consulted only when non-empty. A marker carries an LSN bound, not a trx id
(`open_bounds`, `src/txn/instance_visibility.cpp`), so that exception set
comes from the in-flight table or from a marker extended to carry one.

**An interim, the operator's to take or not: a wait scoped to the relations
the statement reads.** Not built and not verified. A commit publishes the
set of relation oids it wrote beside its window entry (`PublishCommit`,
`src/txn/manager.cpp:324`); the statement boundary waits only for the
highest commit at or below the `CommitCeiling()` read at the statement's
start - a fixed bound, as BA-S1c's is, so a stream of later commits cannot
keep it waiting - that wrote a relation the statement reads. The measured
cell above would then wait never. Its limits and open questions:

- **A `SELECT` mints its snapshot before it parses** (`HandleSelect`,
  `src/server/command_dispatcher.cpp:7262`, ahead of the parser) and binds
  its schema through that view (`include/kds/exec/read_borrow.hpp`), so the
  wait cannot sit between the bind and the mint. It has to bind under one
  view, wait, and mint a second for the data walk; that split, and the
  bind's `IS` refusal under it, is the option's main open question.
- Its oid set is more than the relations a statement names: every bind
  reads the catalog relations (a relation another session's acknowledged
  DDL created cannot even be resolved to an oid), and a foreign-key check
  reads a parent the statement never names. `ReadBorrow` does nothing where
  there is no lock table, so it cannot be the set's source.
- It holds for a statement-scoped snapshot only (`READ COMMITTED`,
  autocommit). A transaction-scoped snapshot is read by later statements
  that may read other relations, so it cannot be narrowed this way.
- Each commit carries a small oid list.
