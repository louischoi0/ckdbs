# `docs/inflight/` — reopened 2026-09-03, on the operator's word

Emptied on 2026-09-02 ahead of the big-bang change; reopened for v3.0.0.
The old tree's last commit is `1769487`:

    git ls-tree -r --name-only 1769487 docs/inflight   # what was here
    git show 1769487:docs/inflight/<path>              # any one file

A citation to `docs/inflight/...` in `CLAUDE.md`, a spec, a test or a
source comment resolves against that commit unless the path exists here.

## What this directory is now, and what it is not

**It reopens with a narrower job than it had, because `instructions/` took
half of it.** Under v2 this held both *plans* and *gaps*; under v3 a plan
lives in `instructions/<version>/`, one work order per milestone, and this
directory holds what a plan is not:

- **`known-gaps.md`** — the engine-wide list: what is missing, what does
  not survive a restart, what the code does differently from what a spec
  claims, and what a test suite does not actually cover. An entry is a
  *known, accepted* state with a named owner. It is not a bug list.
- **`bugs/`** — a defect found and not yet fixed, one file each. A defect
  fixed in the same session it was found does not get a file; the fix's
  commit message and the owning spec carry it.
- **`blocked/`** — work that is designed and cannot proceed, with the
  blocker named and the condition that would unblock it stated as a test.
  Blocked on an *operator decision* belongs in the work order's ruling
  table instead, where the decision is already framed with CLA's proposal
  beside it.

**Not here, deliberately:** open work orders (they are
`instructions/v3.0.0/`), closed workplans (deleted, not archived — the
owning spec carries everything durable), and anything a spec should say.
A gap that turns out to be a rule gets moved into the owning spec and
struck from `known-gaps.md` in the same change.

## The rule an entry must satisfy

**Verified against the code, at a named commit, or it does not go in.** The
list this replaces went stale by accumulating entries nobody re-checked;
every entry in `known-gaps.md` carries the commit it was verified at, and
an entry whose verification is older than the subsystem's last change is a
lie about the engine rather than a note about it.
