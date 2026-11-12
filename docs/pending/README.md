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
| `v20-insert-slowdown.md` | xrock's trade load ran ~3x slower after the move to superblock 20; the first pass found the comparison confounded and the exact investigation deferred | 2026-10-08 |
| `multicore-scaling-cores4-below-cores2.md` | `cores = 4` delivers 21-28 % less than `cores = 2` on `point` and `insert1` at `9b7d53d7`; whether the engine or the client/reactor CPU overlap causes it is unresolved | 2026-10-15 |
| `whole-engine-audit-unverified-findings.md` | a whole-engine audit for logic defects, dead code and needless complexity stopped after 6 of 33 slices (the dispatcher and the expeditor) at `ce5347e6`; its 32 findings were never put through the refutation pass | 2026-11-12 |
| `read-committed-rerun-on-decided-newer.md` | under `READ COMMITTED` a writer that lost to a decided newer version is refused, on `1.4-12.8 %` of contended explicit transactions and `7.9 %` of contended autocommit statements; whether the engine runs it again instead (CQ-Q9 (a)) | 2026-11-11 |
