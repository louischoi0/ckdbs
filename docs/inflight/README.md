# `docs/inflight/` — reopened 2026-09-03, on the operator's word

Emptied on 2026-09-02 ahead of the big-bang change; the old tree is at
`1769487` (`git ls-tree -r --name-only 1769487 docs/inflight`,
`git show 1769487:docs/inflight/<path>`). A citation to `docs/inflight/...`
resolves against that commit unless the path exists here.

## What this directory holds

A plan lives in `instructions/<version>/`, one work order per milestone;
this directory holds what a plan is not:

- **`known-gaps.md`** — what is missing, what does not survive a restart,
  what the code does differently from what a spec claims, and what a test
  suite does not cover. An entry is a *known, accepted* state with a named
  owner, not a bug.
- **`bugs/`** — a defect found and not yet fixed, one file each. A defect
  fixed in the session that found it gets no file; the fix's commit message
  and the owning spec carry it. A fixed bug's file is deleted.
- **`blocked/`** — work that is designed and cannot proceed, with the
  blocker named and the unblocking condition stated as a test. A block on
  an *operator decision* goes in the work order's ruling table instead.

**Not here:** open work orders (`instructions/v3.0.0/`), closed workplans
(deleted; the owning spec carries what is durable), and anything a spec
should say. A gap that turns out to be a rule moves into the owning spec
and leaves `known-gaps.md` in the same change.

## The rule an entry must satisfy

**Verified against the code, at a named commit, or it does not go in.**
An entry whose verification is older than its subsystem's last change is a
statement about an engine that no longer exists: re-verify it or strike it.
