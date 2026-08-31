# Ratification ask XD1 — the participant's ack, and the third durable sync

> **RATIFIED AND ENACTED 2026-08-31.** The operator accepted all three
> questions in §"What the operator is being asked" and directed the
> enactment, `instructions/v2.7.1/workorder-xd.md` (work order XE). Built
> at **`8e76417`** (spec amendments and the code), tested at `f979cd1`,
> reviewed and hardened at `e310f8e`; the contract is now
> `docs/spec/cross-owner-txn.md` §2, §2c and §4, and the retention
> obligation is indexed at `wal.md` §15. **This file is kept as the
> argument the decision was made on, not as an open ask.**
>
> **What the enactment measured, and it is not what this ask predicted.**
> §"What it would be worth" guessed a third of the b=1 chain, ~1 ms.
> Measured (`bench/v2.7.0/results-xe-ack-at-append-v2.7.0-17-ge310f8e.md`
> §4.3): **serially there is no saving to resolve** — 88 µs of p50, inside
> a 16.4% noise floor — because the deferred sync is still in flight when
> the same connection's next transaction asks its prepare for durability,
> so the wait leaves the ack and comes back one iteration later. **Under
> eight concurrent coordinators the saving is real and larger, 25.9% of
> commit p50.** The estimate was wrong in both directions at once, and the
> reason is that it counted syncs in a chain without asking what else was
> running while one of them drained.

Drafted 2026-08-31 by CLA on the worktree `measure-v2.7.1` at `3c42d74`
(`v2.7.0-1-g3c42d74`), as row XD1 of
`instructions/v2.7.1/measurement-xd.md`. **Nothing is built under this
ask.** It is a source-read verdict and the amendment that verdict would
need, put to the operator; the enactment is a later order.

## The question, as the order asked it

> May the participant ack a decide after the COMMIT **append**, letting
> its durability ride the next drain?

## Verdict: sound under D2 `group`, on two conditions, and **not** at that point under D1 `strict`

The outcome is preserved at every crash point by redo-or-resolution, and
the coordinator's answer to the client does not today depend on any
participant's half being durable. Two conditions and one class exclusion
ride with it; all four claims below are source-read at `3c42d74`.

### 1. The coordinator already tells the client COMMIT with **no** ack at all

`src/server/command_dispatcher.cpp:493-505`: the coordinator parks on
`Settled`, and where not every participant acknowledged it writes a
**log line** — "not an outcome change, which is why it is a log line and
not a refusal: the decision is durable and the client's transaction is
settled." The client is answered from the decision record alone.

So a participant's durable half is **not** a term in what the client is
promised. An ack that arrives before that half is durable is strictly
more information than the zero acks the protocol already answers on. This
is the load-bearing observation: the third sync is not paying for the
client's D1/D2 promise, because the protocol already forgoes it in the
case where the ack never comes.

### 2. Every crash point re-resolves to the same outcome

The participant's PREPARE is durable before the promise is made
(`shipped_statement_executor.cpp:575` `RequestDurable`, `:596`
`AwaitPrepared` parks on `IsDurable`) — *"the promise is made after the
record is durable, never at the append"*
(`docs/spec/cross-owner-txn.md` §2 step 1). The coordinator's decision is
durable before it is sent (`command_dispatcher.cpp:445-461`). With the
participant's COMMIT appended but not durable, a crash leaves exactly two
readings, and both give COMMIT:

- **The COMMIT record survived** — the participant's own stream carries a
  terminal record, analysis reads it as a winner, redo applies it. The
  resolver is never reached.
- **The COMMIT record was lost** — the stream holds a `TXN_PREPARE` with
  no decision, which is `cross-owner-txn.md` §2c's *fourth outcome*. The
  resolver opens the coordinator's stream as a **file**
  (`src/server/prepared_resolver.cpp:55`), scans it whole from LSN 0
  (`:76-90`, deliberately with no lower bound), finds the durable COMMIT
  and answers `kWinner` (`:132-135`).

**The checkpoint cannot fall between them.** `TransactionManager::Commit`
clears `active_` at the *append* (`src/txn/manager.cpp:243`), and
`OldestPreparedLsn` skips an inactive transaction
(`src/txn/manager.cpp:503`), so the §11-3 floor on the prepare record is
released before the commit is durable — **today, already, and not by this
proposal**. It is safe for a reason that is worth stating because it is
not obvious: advancing the redo start requires `CHECKPOINT_END` to be
durable first (`wal.md` §11 step 3), and that sync is a stream-wide
`fdatasync` which necessarily carries the earlier COMMIT record with it.
A checkpoint that could strand the prepare has already made the commit
durable.

### 3. Condition A — log retention, which does not exist yet and must be written before it does

`prepared_resolver.cpp:132-135` resolves **no decision found ⇒ ABORT**.
Its soundness is that the coordinator's stream still holds the decision.
Today that holds trivially: **nothing recycles a WAL segment** — `wal.md`
§11 is `[PROPOSED]`, retention is an open item in §15, and
`FileLogDevice` unlinks a segment only on a failed *create*
(`src/wal/file_log_device.cpp:264-281`).

What the ack proves today is precisely what a retention policy would want
to key on: *this participant's half is on the platter, the decision is no
longer needed by it.* An early ack destroys that proof. So the amendment
must carry the obligation explicitly, or a future retention policy will
silently acquire a hole:

> A coordinator's stream may not recycle a segment holding a decision
> until every participant of that transaction has made **its own**
> terminal record durable. A pre-durable ack does not discharge this;
> either the ack carries the participant's durable point, or retention
> is floored by something other than the acks.

### 4. Condition B, and the class exclusion — D1 `strict` gains nothing here

Under D2 the participant's COMMIT registers `pending_group_commits_` at
the append (`src/wal/manager.cpp:203`), so **the next `DrainOnce` syncs
it whether or not anyone is parked** (`:225-231`). Riding the next drain
is therefore already the mechanism; the wait removed is a
*serialization*, not a durability.

Under D1 the same call syncs **inline, on the task's stack**
(`src/wal/manager.cpp:194-200`). There is no ack to move: the sync
happens inside `Commit` before it returns. Buying the same slack under
strict would mean committing a participant's half at *group* class while
the session asked for *strict* — a change to what `strict` means, and a
separate decision this ask does not make. **D1 keeps three syncs.**

## What it would be worth

Under D2, the chain goes from three serialized device syncs to two. The
order's own increments put the fixed part at **~1.9 ms at b = 1**
(`bench/v2.7.0/results-scenario2-cores-v2.4.0-83-g57110cf.md`), so a
third of the chain is the first-order estimate — to be replaced by XD2's
and XD3's measured numbers before anything is built on it.

## What the operator is being asked

1. Is the re-statement in §1 accepted — that a cross-owner transaction's
   durability point is the **coordinator's decision record**, and a
   participant's own terminal record is a redo shortcut rather than part
   of the client's D1/D2 promise? This is already the protocol's
   behaviour in the no-ack case; the ask is to make it the *stated*
   contract, in `cross-owner-txn.md` §2 and §4.
2. Is the retention obligation in §3 accepted as a rule that lands
   **now**, in the spec, ahead of the retention policy it constrains?
3. D1 `strict` keeps three syncs and is out of scope — accepted?

Nothing changes in the tree until these are answered.
