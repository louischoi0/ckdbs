# `instructions/` — emptied 2026-09-02

The operator's work orders, ratifications and blueprints for `v2.3.0`
through `v2.8.0` left ahead of the big-bang change to the architecture,
rules and constraints. Every one of them was an instruction against the
engine that change replaces; what it built is in `docs/spec/`, and what it
measured was in `bench/` (emptied the same day, see `bench/README.md`).

The last commit holding the tree is `1769487`:

    git ls-tree -r --name-only 1769487 instructions   # what was here
    git show 1769487:instructions/<path>              # any one file

A citation to `instructions/...` in `CLAUDE.md`, a spec, a test or a source
comment points at that commit. The big-bang change's own instructions are
the first thing written here again.
