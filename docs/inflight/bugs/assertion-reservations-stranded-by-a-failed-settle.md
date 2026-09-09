# A failed `AssertionEnforcer::CommitTxn` strands the reservations it had not settled

**Found** 2026-09-09 by the `critics-developer` pass on AO-S6d
(`7e26a65` + the AO-S6d diff, on `ao-s6d-carried-defects`). **Not fixed** —
the fix changes what a failed settle leaves behind, which is a decision the
assertion subsystem owns and this stage does not.

## The two calls

`src/exec/assertion_check.cpp:311-317` — `CommitTxn` takes the transaction's
reservation list **out of `pending_` before anything that can fail**:

```cpp
const std::vector<Reservation> reservations = std::move(pending->second);
pending_.erase(pending);
```

Every exit below that line is an error return, and the site argues for the
order: leaving the list behind means the *next* transaction under the same
id settles it a second time, and under `catalog::kBootstrapXid` — which
`CommandDispatcher::EndWrite` uses for every statement on a dispatcher with
no transaction manager — that id is not unique per transaction. Double
settling silently moves an aggregate twice; dropping the remainder leaves it
wrong in a way the returned error reports. The second is the failure the
enforcer prefers.

`src/exec/assertion_check.cpp:372-380` — `AbortTxn` looks the id up in the
same map and returns `Status::OK()` when it is not there.

So after a failed `CommitTxn`, `AbortTxn` for that transaction is a
**guaranteed no-op**: the reservations it had not reached are neither
committed (their reserved flags stand, their deltas stay applied to the
Bound Cabin) nor unapplied.

## What AO-S6d changed about it

Nothing in the enforcer, and everything about how visible it is.

Before AO-S6d, `CommandDispatcher::EndWrite`'s autocommit arm returned on a
failed `enforcer_.CommitTxn` without unwinding anything: the transaction
stayed active in `live_` for the life of the process, so its rows were
invisible to every reader for ever, and the stranded contribution described
rows nobody could see. The count was already wrong, in the same direction.

Since AO-S6d that arm calls `AbortOwnedScope`, which compensates the trail
and releases the borrows and the transaction. The rows go; the contribution
stays. The count is wrong in the same direction and by the same amount as
before, and now nothing in the transaction record explains why.

## Why it is not urgent

**Fail-closed, in both states.** A Bound Cabin that counts a contribution
whose row is gone is an aggregate that is too high, so an assertion of the
`SUM(...) <= N` shape refuses writes it could have admitted. It never admits
one it should refuse. The residue also survives a mount — the entry pages
carry the reserved flags and a rebuild scans them — so it is durable rather
than a process-lifetime artefact, which is what makes it worth a row here
rather than a comment.

**And it needs a failed settle to reach at all**: `store.Get` or a WAL
append failing part-way through `CommitTxn`'s per-`(assertion, page)` loop.
`FailNextSync` does not reach it; nothing in the suite does.

## The three ways out, none obviously right

1. **`CommitTxn` puts the un-settled remainder back on failure**, so a
   following `AbortTxn` unapplies it. This is exactly the double-settle the
   site's comment refuses, and the refusal is about `kBootstrapXid` reuse —
   which the `AbortOwnedScope` caller cannot reach, because that path always
   has a real transaction manager and a unique id. So the answer may be
   *"put it back, but only where the id is a real transaction's"*, which
   makes the enforcer's behaviour depend on its caller.
2. **`CommitTxn` returns the un-settled remainder** and the caller hands it
   to a compensating call. Honest, and it puts a second list-shaped thing on
   an interface that has one.
3. **Settle in the other order** — unapply first, clear flags second — so a
   failure part-way leaves reservations that are still `pending_` and still
   correct. This moves the WAL-before-data argument the two functions each
   spell out at their loops, which is where the real cost of the change is.

## Owner

The assertion subsystem (`docs/spec/assertion.md` §4.4 and §7, whose AS6b
decision the orphan mark implements). AO-S6d's `EndWrite` arm states the
residue at the site and points here; nothing else in the tree depends on the
choice.
