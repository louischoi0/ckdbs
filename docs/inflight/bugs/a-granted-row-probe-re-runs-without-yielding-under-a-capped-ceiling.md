# A granted row probe re-runs without yielding under a capped ceiling

**The wrong behaviour.** A parent `DELETE`'s reverse foreign-key walk
(`CommandDispatcher::CheckNoChildrenBeforeDelete`) can meet a child row
whose writer has already **committed and released** while every fresh
check view still answers it `kBusy`. That happens when another commit's
marker caps `SnapshotCeiling()`: the marker is held from before that
commit's append until after its publish, which under `strict` covers its
`fdatasync` (`instance_visibility.hpp`, "the acknowledged-commit bound").
`WaitForRowWriter`'s `U` probe on the child row is then **granted**,
because the writer has released, and the grant arm installs a slot that is
already flipped. `NoteBlockingWriter`'s `woken_at_release` test reads that
wake's holder and records the block. `DispatchAsync`'s `WaitUntil` then
finds a true predicate and does not suspend, and the statement re-runs at
once and lands in the same place. This repeats, without the reactor ever
yielding, until the ceiling lifts or the 1 s net refuses the statement
`TxnConflict`.

- If the capping marker is a `strict` committer parked on **the same
  core**, its publish cannot run while the reactor spins, so the ceiling
  never lifts there. That core is stalled for the whole net, and the
  client is refused anyway.
- If the marker is on **another core**, the spin lasts up to one sync and
  starves every other session on the waiter's core.
- An autocommit re-run also begins a fresh transaction each round, which
  logs a `kTxnBegin` record each time.

**Verified at** `f1ce9953` on `worktree-unique-busy-tuple-probe` **by
reading only** (the `critics-developer` review of that commit; re-read by
CLA). It has not been reproduced, and no rig cell drives it: the cell
needs a `strict` commit on a third session whose marker overlaps the
child writer's commit.

**Smallest reproduction (as read).** Use two cores and a child relation
`c` with a foreign key to parent `p`.

1. Session A on core 0 runs `BEGIN; UPDATE c SET note = 1 WHERE id = 1;`
   (an update of other columns, which takes no `S` on the parent).
2. Session S on core 1 starts a `strict` `COMMIT` of any write, so its
   marker is held across its sync.
3. Session A runs `COMMIT`. It is published and released, but it sits
   above S's marker.
4. Session B on core 1 runs `DELETE FROM p WHERE id = <c.1's parent>`.
   The reverse walk answers busy, and the probe is granted.

**What it costs.** A slow answer, a refusal, and a stalled core. It is
never a wrong answer: the walk still reads latest state, and the statement
ends either refused or right.

**The unique check had the same exposure and is closed** at the commit
that added this file. `CheckUniqueKeyLatest` probes only a writer
`IsInFlight` still names, and refuses a decided-but-capped writer
retryably, as it did before its probe existed.

**The fix is known and not scheduled.** There are two options.

- **(A) The same guard at the reverse walk.** Probe only an in-flight
  child writer. The cost is that a child writer which decided between the
  walk and the probe, committed or aborted, gets a retryable refusal where
  today it gets an immediate re-run.
- **(B) Inside `WaitForRowWriter`.** Pre-flip the granted slot only when a
  fresh check view sees the holder's decide.

Either one changes the grant-arm behaviour AY-Q8 pinned
(`instructions/v3.0.0/workorder-ay-following-letter.md`), so the choice
is the operator's.
