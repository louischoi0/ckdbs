# `docs/` — the map

**`instructions/` is input; `docs/` and `bench/` are output.** A work
order or a blueprint's workplan arrives from the operator in
`instructions/`; what building it produces lands here as prose — the
spec, the rule, the open plan, the report — and in `bench/<version>/` as
measurement. Nothing operator-authored lives under `docs/`, and nothing
here is written to be read as a request.

Three buckets. `/CLAUDE.md`'s milestone table says which subsystem each
document owns; this file says only where a document goes.

| | holds | test for belonging |
|---|---|---|
| `spec/` | what is confirmed **and implemented** | the code does what it says; when the spec and `/CLAUDE.md` disagree, the spec wins |
| `rules/` | concepts and constraints that hold **across the codebase** | it constrains code that does not know it exists |
| `inflight/` | what is not finished | a task in it is unbuilt, gated, broken, or unverified |

`inflight/` splits four ways, and `known-gaps.md` sits above them as the
engine-wide register of what is missing and what a restart loses:

- **`in-progress/`** — open workplans nothing external blocks. Tasks
  remain and the next one is startable today.
- **`blocked/`** — a named gate stops the next task: an operator decision,
  absent hardware, an unbuilt prerequisite. **The file says which**; a
  blocked plan with no named blocker is an in-progress one.
- **`bugs/`** — one file per defect, open until the fix lands with its
  test. See `bugs/README.md`.
- **`verified/`** — the output of a checklist run against the code: an
  audit, a sweep, a status reconciliation. A report, never a plan.

A workplan whose every task is built is **deleted**, not archived — the
spec that owns the subsystem carries everything durable, and git history
carries the rest (`git show 925f483:docs/workplan-index.md`).
