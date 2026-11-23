# `instructions/` — emptied 2026-09-02, reopened the same day for `v3.0.0/`

- **`v3.0.0/`** holds the big-bang change's own instructions: AR0, the
  governing draft with its verification record, and one work order per
  milestone, AL through BU (`v3.0.0/index.md`). No `v3.0.0` tag was taken;
  the first v3 tag is `v3.1.0`.
- **`v3.2.0/`** opened 2026-10-14 on the operator's naming of that version,
  with BV, and holds every work order written since - BV through DQ -
  whatever tag it landed under (`v3.2.0/index.md`). These two are the only
  version directories.
- **`milestones.md`** is the register across both: one row a milestone,
  sorted newest first, with the queries that read it by id, status and
  date. A row moves in the commit that changes its order's status.

**Nothing from `v2.x` returns here.** The operator's work orders,
ratifications and blueprints for `v2.3.0` through `v2.8.0` left ahead of
the big-bang change; what they built is in `docs/spec/`. The last commit
holding them is `1769487`:

    git ls-tree -r --name-only 1769487 instructions   # what was here
    git show 1769487:instructions/<path>              # any one file

A citation to an `instructions/...` path that is not in the working tree —
in `CLAUDE.md`, a spec, a test or a source comment — points at that commit.

This file as it was last written in full: `git show 16d84ef7:instructions/README.md`.
