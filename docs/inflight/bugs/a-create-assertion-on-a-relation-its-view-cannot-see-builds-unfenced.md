# A CREATE ASSERTION on a relation its view cannot see builds unfenced

**Found by reading, not reproduced.** Verified at `38bb8a8` on
`worktree-az-s2-peer-create-assertion`, by the review of AZ-S2
(`instructions/v3.0.0/workorder-az-ay-carry-forward.md` §6).

## What is wrong

The build's relation `X` (AT-S5e) is taken only for a name the session's
view resolves: the dispatcher looks the relation up under `ViewFor(session)`
and takes no borrow when the lookup fails
(`src/server/command_dispatcher.cpp`, `HandleAssertion`'s create arm, the
`BuildLock` block). The build then resolves the same name **unfiltered**
(`AssertionTargetOid`, `src/exec/assertion_catalog.cpp`), so a relation the
view cannot see - one created by another session's still-open transaction -
is built with no `X` held.

Without the `X`, that transaction's writers are not fenced. A row it writes
after the scan has passed its position and before the adoption
(`AdoptLogged`) is in neither the cabin nor any check.

## What it costs

**A quiet wrong answer, if reached.** The cabin under-counts the group, so a
later write the assertion forbids can be admitted. A row in flight when the
scan reads it refuses the create `TxnConflict`, so only rows written after
the scan passes them escape.

## Reproduction

Not reproduced. A cell would need two sessions: one `BEGIN; CREATE TABLE t
...; INSERT ...` left open, the other `CREATE ASSERTION ... ON t`, with a
write by the first between the scan and the adoption (the seam
`SetAfterAssertionPublishRunForTest` runs just after the adoption, so the
write has to land inside the build's scan).

## The fix

Not settled. Either refuse the create when the viewed lookup fails (the
relation is not the session's to read), or take the `X` on the unfiltered
oid. The first matches `HandleIndex`'s resolution; the second waits for the
creator's transaction.
