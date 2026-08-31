# CIP — Change Improvement Proposals (path-optimizer)

Open workplan. Each entry is a **numbered change improvement proposal**
(`OPT-nnn`) against a measured hot path, cut and **built** on the
`path-optimizer` worktree. The role that owns this file does not stop at
the proposal: it implements the proposal on its own branch, measures it,
pushes that branch to `origin`, and **links the branch back into the
entry** — so a proposal here is readable as code, not only as prose.

Upstream of every entry: `CLAUDE.md`'s hard invariants and the owning
spec. A proposal that needs an Open Decision is not cut; it is reported
as blocked and named.

## What an entry must carry

Seven fields, in this order. An entry missing one is not ready to build.

1. **Number and title** — `OPT-nnn`, one line saying what changes.
2. **Hypothesis** — what specifically costs, and the *predicted
   direction and magnitude* of the win. A prediction with no number
   cannot be wrong, so it is not a hypothesis.
3. **Measurement** — the existing driver, probe or counter that decides
   it, named by path, plus the A/B shape. `build-release`, interleaved
   A/B, per `CLAUDE.md`'s measurement rule; a Debug number is not a
   result. Every number carries `git describe --tags`.
4. **Reason** — why it is worth doing now, in terms of a statement shape
   the engine actually serves.
5. **Pros / cons** — both, where both exist. An entry with no cons is
   under-examined; where the cost is genuinely nil, say so plainly.
6. **Consistency and sanity** — which numbered hard invariant and which
   spec rule the change touches, what could break, and what proves it did
   not (the suite, a contract test, a `sim/` cell).
7. **Implementation** — the branch, its remote link, the commit ids, and
   what the suite said. Written back into the entry as the work lands,
   never left as an intention.

## Branch and push convention

- One branch per proposal, cut from the worktree branch:
  `opt-<nnn>-<slug>` (e.g. `opt-001-tuple-decode`).
- Pushed to `origin` as a **working branch**. `main` is never pushed from
  this role.
- The entry carries the link:
  `https://github.com/louischoi0/ckdbs/tree/<branch>`, plus the commit id
  every claim in the entry was true of.
- A proposal that is measured and *rejected* keeps its branch and its
  entry — a refuted hypothesis is a result, and deleting it invites the
  next run to re-propose it.

## Standing constraints on every entry

- **No on-disk format change**, and nothing that would need a
  `kFingerprintVersion` bump.
- **Nothing in `CLAUDE.md`'s Open Decisions** — those wait on the
  operator, and an entry here must not silently pick one.
- **No new subsystem.** A local, provable change beats a redesign.
- **A refusal stays a refusal**: an optimization may not widen what the
  engine admits. Same answers, same errors, fewer cycles.
- Advisory structures (Waystone) stay advisory: invariants 8 and 9 hold
  whatever the speedup.

## Status

| # | Title | State | Branch | Commit |
|---|---|---|---|---|
| — | (first entries land as the hot-path survey completes) | — | — | — |
