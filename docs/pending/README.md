# `docs/pending/`

**Pending discussions.** Opened 2026-10-08 on the operator's word
(*"for now, record it as pending, as something to discuss later, in a separate directory"*).

A pending entry records a question the operator has raised and deferred:
what prompted it, what the tree does today, the options on the table and
what each would cost. It decides nothing, opens no stage and licenses no
code. It is not a concept note (`docs/conceptnotes/` holds an idea worked
out with its boundaries), not a gap (`docs/inflight/known-gaps.md` holds
what is missing from what is built) and not a work order
(`instructions/<version>/`).

Every entry carries, in its header: a **status line** (`PENDING`), the
**date** it was raised, the **commit** its `[source-read]` claims were read
at, and the **related documents** that already touch the question. Claims
are tagged `[operator]`, `[source-read]` (with `path:line`) or `[design]`.

**An entry leaves this directory when the operator rules on it**: it is
deleted, and the ruling goes where it belongs - a mark in the work order or
concept note that owns the question, or a new work order. Git keeps the
entry.

| entry | subject | raised |
|---|---|---|
| `session-load-balancing.md` | Moving a session between cores after it is accepted, instead of pinning it to the accepting core for its life | 2026-10-08 |
| `v20-insert-slowdown.md` | xrock's trade load ran ~3x slower after the move to superblock 20; the first pass found the comparison confounded and the exact investigation deferred | 2026-10-08 |
