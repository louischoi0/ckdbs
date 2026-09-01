# CIP — Change Improvement Proposals (path-optimizer)

Open workplan. Each entry is a **numbered change improvement proposal**
(`OPT-nnn`) against a measured hot path, cut and **built** on the
`path-optimizer` worktree. The role that owns this directory does not
stop at the proposal: it implements the proposal on its own branch,
measures it, pushes that branch to `origin`, and **links the branch back
into the entry** — so a proposal here is readable as code, not only as
prose.

Upstream of every entry: `CLAUDE.md`'s hard invariants and the owning
spec. A proposal that needs an Open Decision is not cut; it is reported
as blocked and named.

## How this directory is laid out

**One directory per proposal**, and everything that proposal produced
lives in it: the proposal document itself and the measurement that
priced it, side by side. A reader who opens `OPT-002-string-slot-assign/`
finds the claim and the number that tested it without following a link
into `docs/` or `bench/`, and a proposal that is deleted takes its own
evidence with it rather than leaving an orphaned results file behind.

```
CIP/
  README.md                              this file - the register
  OPT-001-update-decode-order/
    proposal.md
  OPT-002-string-slot-assign/
    proposal.md
    results-opt002-string-slot-assign-v2.7.0-30-g55d2c0b.md
  ...
```

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
| [OPT-001](OPT-001-update-decode-order/proposal.md) | UPDATE/DELETE decode every scanned row before testing the WHERE | **built, reviewed, measured** | `opt-001-update-decode-order` | `e156b3d` |
| [OPT-002](OPT-002-string-slot-assign/proposal.md) | Every decoded string costs a malloc+free the codec's own header says it should not | **built, measured** | `opt-002-string-slot-assign` | `55d2c0b` |
| [OPT-003](OPT-003-walk-read-access/proposal.md) | UPDATE/DELETE walk the relation with `kWrite` and dirty every page they read | **built, measured** | `opt-003-walk-read-access` | `31bc482` |
| [OPT-004](OPT-004-decoderowinto-preconditions/proposal.md) | `DecodeRowInto` still pays the Status-constructing preconditions AP02 removed | **built**, A/B owed | `opt-004-decoderowinto-preconditions` | `b05925f` |
| [OPT-005](OPT-005-btree-leaf-ref/proposal.md) | `BtreeLookup` drops the leaf pin and every caller re-fetches the same page | proposed | — | — |
| [OPT-006](OPT-006-subchain-runner-reuse/proposal.md) | A correlated sub-chain rebuilds its whole runner per outer row | proposed | — | — |

Each row's number links to that proposal's own directory. Survey that
produced OPT-001..OPT-006: `worktree-path-optimizer` at
`1beda80` (`v2.7.0-27-g1beda80`). Ordering is by (expected win) /
(risk x size), which is why the two smallest entries are built first: a
change that cannot regress is worth landing before a change that could.

## Not cut, and why

- **Heap/btree INSERT's duplicate scan is O(slots per page)**
  (`heap_chain.cpp:109-122`, `btree.cpp:632-645`): removable in
  principle, since invariant 11's 2026-08-25 amendment says an omitted
  key or a named key at or above the high-water mark needs **no page
  read** — but it means threading a "uniqueness proven" flag from the id
  issuer, and a wrong flag admits a duplicate pk silently. A
  backwards-scan short-circuit would lean on within-page id ordering,
  which **invariant 4 explicitly disclaims**. Needs the owner's
  decision, not a patch.
- **A last-frame memo in `GetForRead`**: real, but per *page* rather than
  per row, and OPT-005 removes a whole fetch on the same shapes for less
  risk.
- **`stats::MakeValueKey` copies a `std::string` per bucketed row and
  per probe** (`cabin_store.hpp:132-139`): only for string join keys,
  and a `string_view`-keyed form is a wider `CabinKey` refactor.
- **AP05** is already owned by `workplan-aggregate-perf.md`; it is not
  re-reported here as a discovery.
- **A compiled-plan cache** is excluded as a new subsystem, and it
  collides with the catalog-invalidation soundness gap in
  `catalog_version()`.
