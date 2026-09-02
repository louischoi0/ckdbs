# `docs/` — the map

**Two buckets since 2026-09-02.** `instructions/` (the operator's work
orders and ratifications), `bench/` (every measurement) and
`docs/inflight/` (every open workplan, blocker, defect report and
verification report, and `known-gaps.md`) were emptied ahead of the
big-bang change to the architecture, rules and constraints, because each
described work against the engine that change replaces. The last commit
holding them is `1769487` — `git show 1769487:<path>` retrieves any file,
`git ls-tree -r --name-only 1769487 <dir>` lists a tree — and each of the
three directories keeps a `README.md` saying so. **A citation to one of
those paths anywhere in the tree points at that commit.**

`/CLAUDE.md`'s milestone table says which subsystem each remaining
document owns; this file says only where a document goes.

| | holds | test for belonging |
|---|---|---|
| `spec/` | what is confirmed **and implemented** | the code does what it says; when the spec and `/CLAUDE.md` disagree, the spec wins |
| `rules/` | concepts and constraints that hold **across the codebase** | it constrains code that does not know it exists |

A workplan whose every task is built is **deleted**, not archived — the
spec that owns the subsystem carries everything durable, and git history
carries the rest (`git show 925f483:docs/workplan-index.md` for the plans
closed before 2026-08-26; `1769487` for everything that was still open).
