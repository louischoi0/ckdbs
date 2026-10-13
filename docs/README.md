# `docs/` — the map

`/CLAUDE.md`'s milestone table says which subsystem each document owns;
this file says only where a document goes.

| | holds | test for belonging |
|---|---|---|
| `spec/` | what is confirmed **and implemented** | the code does what it says; when the spec and `/CLAUDE.md` disagree, the spec wins |
| `rules/` | concepts and constraints that hold **across the codebase** | it constrains code that does not know it exists |
| `inflight/` | what is **missing**: `known-gaps.md`, `bugs/` (found and not yet fixed), `blocked/` | it names a gap, not a plan — and it carries the commit it was verified at |
| `conceptnotes/` | **concept notes** (opened 2026-09-08 with CN-1): an idea recorded with its boundaries and its open decisions, before anything is built and possibly before anything ever is | nothing in it is decided, so it opens no stage and gates nothing; every note carries a status line, its claim tags, the commit it was written against, and what it does not license |
| `pending/` | **pending discussions** (opened 2026-10-08): a question the operator raised and deferred, with the tree as read and the options | it decides nothing; on the operator's ruling the entry is deleted and the ruling goes to the document that owns the question |

Outside `docs/`: **`instructions/<version>/`** holds the operator's work
orders and ratifications — an open plan lives there, never in `inflight/` —
and **`bench/<version>/`** holds every measurement.

## The citation rule for the three emptied directories

`instructions/`, `bench/` and `docs/inflight/` were emptied on 2026-09-02
ahead of the big-bang change; the last commit holding their old contents is
`1769487` (`git show 1769487:<path>`). All three reopened (`instructions/v3.0.0/`
on 2026-09-02, `bench/` and `docs/inflight/` on 2026-09-03), so:

> A citation to one of those paths resolves against `1769487` **unless the
> path exists in the working tree.**

The qualifier is load-bearing: source comments still cite
`docs/inflight/in-progress/...` (`src/parser/parser.cpp`'s HAVING
comments), and `in-progress/` is not a reopened bucket.

A workplan whose every task is built is **deleted**, not archived — the
owning spec carries everything durable; `git show 925f483:docs/workplan-index.md`
lists the plans closed before 2026-08-26.
