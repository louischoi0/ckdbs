# `docs/inflight/bugs/` — defect reports

One file per defect, named for the defect and not for the date:
`peer-index-build-page-not-found.md`, not `bug-2026-08-26.md`.

A report is open work, so it lives here until the fix lands **with its
test**; then it is deleted and what it taught goes into the spec that owns
the subsystem, or into `docs/inflight/known-gaps.md` if a residue outlives
the fix.

What a report carries, in this order:

1. **The symptom**, as observed — the statement, the reply, the counter.
2. **The worktree and short commit it reproduces on**, inside the sentence
   that makes the claim (the standing rule in `/CLAUDE.md`).
3. **The mechanism**, once known: what the code actually does, not what it
   was meant to do. A report with no mechanism yet says so.
4. **The reproduction** — a seed for `scripts/sim.sh`, a test name, or the
   exact sequence.
5. **What it is *not*** — the neighbouring defect it was mistaken for.

A defect that is understood and deliberately *not* fixed is not a bug
report: it belongs in `docs/inflight/known-gaps.md`, with the reason.
