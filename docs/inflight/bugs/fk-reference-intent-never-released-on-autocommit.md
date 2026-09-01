# The cross-owner FK reference intent is never released, and its holder has no identity

**Opened 2026-09-02 by AI-T2** (`instructions/v2.8.0/workorder-ai.md`), the
end-to-end cross-owner INSERT cell that work orders AH and AI each deferred.
Found on `worktree-v2.8.0-ratification-ae` at `ab5cc5d`, by driving the
statement the unit cells never drove. Contract: `docs/spec/foreign-keys.md`
§2a, `include/kds/server/fk_intent.hpp`,
`src/server/command_dispatcher.cpp`'s `SendForeignKeyProbes`.

**Status: open.** Three findings below, one root cause, and the fix shape for
the first is a decision rather than an oversight — it changes what an
autocommit cross-owner write costs.

## What is supposed to happen

A passing probe grants a **reference intent** on the parent's owner: *"I am
relying on `(relation, pk)` existing."* A parent-side `DELETE` meeting a live
intent answers busy rather than racing it. AH-R5 says the intent is released
by the transaction's **decide** and by nothing else — "which every
cross-owner transaction already sends, so there is no third message". The
holder key is `(coordinator core, session id)`, and `SendForeignKeyProbes`'s
own comment states the autocommit case as settled:

> Autocommit enrols nobody, which is right and is also what makes the intent
> safe there: an autocommit statement's transaction ends on this core, and
> the decide that releases the intent is the one this core's commit sends to
> every participant it has.

## F1 — an autocommit cross-owner INSERT leaks its intent, forever

The sentence above is the defect: with **no participant enrolled there is no
decide**, so nothing ever releases what the probe granted. Measured on the
AI-T2 rig (parent on core 0, child on the peer, `INSERT INTO child VALUES
(7)` autocommit, driven to completion and pumped sixteen further turns):

```
live_rows after autocommit = 1
DELETE FROM parent WHERE id = 7
  -> ERR TXN_CONFLICT retryable=1 row id=7 of 'pparent' is relied on by a
     foreign key check running on another core, so it cannot be deleted yet
```

**The parent row is un-deletable for the life of the process**, and the code
saying so is `retryable=1` — a client's retry loop spins on a condition that
never clears, which is worse than a terminal refusal. Only a restart clears
it, because the table is memory-resident.

## F2 — a transaction whose only cross-owner contact is an FK probe cannot commit

`session.ship_id()` is minted in **one** place, `ShipStatement`
(`command_dispatcher.cpp`): *"minted on the first ship and kept for the
session's life"*. An FK probe is a cross-core contact that is not a ship, so
a session that has never shipped probes with `ship_id() == 0` — and then
enrols the owner as a participant. At `COMMIT` the coordinator finds
participants and no identity:

```
BEGIN; INSERT INTO child VALUES (9); COMMIT
  -> ERR a cross-owner transaction has participants but no shipping
     identity; its participants cannot be addressed
```

So **the cross-owner FK write inside an explicit transaction cannot commit at
all**, which is not a fixture limit: production reaches it by the same route
whenever the session's first cross-core contact is the probe rather than a
ship.

**The refusal's own comment is the marker.** It reads: *"A participant is
enrolled only by a statement this session shipped, and shipping mints the id
— so this cannot happen without the two having come apart."* AH's probe
enrolment is what came apart from shipping, and the branch written as
unreachable is now the ordinary path for a cross-owner foreign key inside a
transaction.

## F3 — every un-shipped session shares one intent holder

The same zero. The holder is `(coordinator core, session id)`, so on one core
*every* session that has never shipped holds intents under
`(core, 0)` — and `FkIntentTable::Release` frees **by holder**, so one
session's decide would release another's intents. Latent today only because
F1 and F2 mean no decide arrives; it becomes live the moment either is fixed
without the other.

## The root cause, and why the fixes are not symmetric

One root: **the probe path uses the shipping identity without being one of
the things that mints it**, and the autocommit path assumes a decide that its
own enrolment rule prevents.

- **F2 and F3 fix together and unambiguously**: mint the id at the first
  cross-core *contact* rather than at the first ship — the same rule the
  comment already states, applied to the other contact. It converts a refusal
  into a working commit, so under the order's standing-rule note it is the
  operator's word rather than CLA's default.
- **F1 does not.** Keeping the intent (it is what closes the window between
  the probe's reply and the child's commit — a window autocommit has too,
  only shorter) means something must release it, and the candidates differ in
  what they cost:
  1. **Enrol in autocommit too**, so the existing decide leg releases it. No
     new message, but an autocommit cross-owner INSERT gains a decide round
     it does not have today. Under `cross-owner-txn.md` §1a the participant
     wrote nothing, so it writes no `TXN_PREPARE` and takes no sync for it —
     the cost is the round trip, not a device sync.
  2. **A release message at statement end**, which is the "third message"
     AH-R5 says the design does not have.
  3. **A deadline on the intent**, which trades a permanent pin for a
     bounded one and needs a number nobody has measured.

## What holds today, so the report is not read as wider than it is

The end-to-end path itself works and is now pinned by
`CoreRuntimeTest.ACrossOwnerInsertProbesTheParentsOwnerAndWritesTheChildRow`:
the fork parks before any row work, one probe crosses per distinct owner, the
owner answers, the statement resumes and the child row is written and
readable on its owner. The intent's *grant* is correct; only its end is
missing.
