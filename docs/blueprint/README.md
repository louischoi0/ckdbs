# `docs/blueprint/`

**Concept notes.** Opened 2026-09-08 on the operator's word, with CN-1.

A blueprint is what a plan is not and what a spec is not: an idea recorded
with its reasoning, its boundaries and the decisions it leaves open,
before anything is built and possibly before anything ever is. It opens no
stage, gates nothing, and licenses no code.

One rule, and it is the reason this bucket is separate from the three
`CLAUDE.md` already names:

- **`docs/spec/`** states what is built. **`docs/rules/`** states what
  holds everywhere. **`docs/inflight/`** states what is missing from what
  is built. **`docs/blueprint/`** states what has not been decided to
  build at all. A reader who cannot tell which of the four they are in
  will read a concept as a contract.

So every file here carries, in its own header: a **status line** saying it
is a concept, the **claim tags** its sentences run under (`[operator]`,
`[source-read]` with `path:line`, `[design]`, `[measured]` — a concept
note usually has none of the last), the **commit** it was written against,
and a section listing **what it does not license**. A note whose claims
are not separable into those tags is not ready to be filed.

**A note is not amended into truth.** When the tree moves past it, the
note stays as written and its header's commit is what makes that legible;
what changes is a marked answer beside the open item, the way CN-1 §8
carries the operator's choice of this directory. When a note is taken up,
the work order cites it and the note says so — the work order is then the
authority and the note is history.

| note | subject | status |
|---|---|---|
| `cn-1-agent-operable-optimization-surface.md` | An external agent as an ordinary SQL client, and the levers a database would have to expose for a machine operator to drive it safely | CONCEPT; five operator decisions open, and a measurement gate before any of it is built |
