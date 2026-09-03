# `docs/` — the map

`/CLAUDE.md`'s milestone table says which subsystem each document owns;
this file says only where a document goes.

| | holds | test for belonging |
|---|---|---|
| `spec/` | what is confirmed **and implemented** | the code does what it says; when the spec and `/CLAUDE.md` disagree, the spec wins |
| `rules/` | concepts and constraints that hold **across the codebase** | it constrains code that does not know it exists |
| `inflight/` | what is **missing**: `known-gaps.md`, `bugs/` (found and not yet fixed), `blocked/` | it names a gap, not a plan — and it carries the commit it was verified at |

Outside `docs/`, and named here because the boundary is what people get
wrong: **`instructions/<version>/`** holds the operator's work orders and
ratifications — an open plan lives there, never in `inflight/` — and
**`bench/<version>/`** holds every measurement.

## The three directories that were emptied, and the citation rule

`instructions/`, `bench/` and `docs/inflight/` were emptied on 2026-09-02
ahead of the big-bang change to the architecture, rules and constraints,
because each described work against the engine that change replaces. The
last commit holding them is `1769487` — `git show 1769487:<path>` retrieves
any file, `git ls-tree -r --name-only 1769487 <dir>` lists a tree.

**All three have since reopened**: `instructions/v3.0.0/` on 2026-09-02,
`bench/` and `docs/inflight/` on 2026-09-03. So the citation rule carries a
qualifier and is wrong without it:

> A citation to one of those paths resolves against `1769487` **unless the
> path exists in the working tree.**

The qualifier is load-bearing rather than pedantic. `src/server/core_runtime.cpp`
ships a runtime error message citing
`docs/inflight/in-progress/workplan-peer-writer.md` — and `in-progress/` is
*not* a reopened bucket, so the qualified rule resolves it correctly at
`1769487` while the unqualified one is right only by accident.

A workplan whose every task is built is **deleted**, not archived — the
spec that owns the subsystem carries everything durable, and git history
carries the rest (`git show 925f483:docs/workplan-index.md` for the plans
closed before 2026-08-26; `1769487` for everything that was still open).
