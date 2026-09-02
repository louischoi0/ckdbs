# `docs/inflight/` — emptied 2026-09-02

`known-gaps.md` and the four buckets under it — `in-progress/`,
`blocked/`, `bugs/`, `verified/` — left ahead of the big-bang change to the
architecture, rules and constraints. Every open workplan, named blocker,
defect report and verification report described unfinished work on the
engine that change replaces, and an open task carried across would be a
task against a design that no longer exists.

The last commit holding the tree is `1769487`:

    git ls-tree -r --name-only 1769487 docs/inflight   # what was here
    git show 1769487:docs/inflight/<path>              # any one file

A citation to `docs/inflight/...` in `CLAUDE.md`, `README.md`, a spec, a
test or a source comment points at that commit. Whether an in-flight bucket
returns, and in what shape, is the big-bang change's to say.
