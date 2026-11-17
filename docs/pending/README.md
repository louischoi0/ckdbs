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
| `whole-engine-audit-unverified-findings.md` | a whole-engine audit for logic defects, dead code and needless complexity stopped after 6 of 33 slices (the dispatcher and the expeditor) at `ce5347e6`; of its 32 findings fourteen defects are fixed and the dead code removed, F4 (an `UPDATE`/`DELETE` judged its WHERE on an undecided writer's bytes) was ruled and fixed on 2026-11-15, and what waits is the 27 unread slices | 2026-11-12 |
| `whole-engine-audit-second-run-twelve-slices.md` | the audit's second run read twelve more slices (transactions, WAL, storage, catalog, the row/index and assertion executors, superblock and backup) at `0b98bc4f`: 66 findings, 15 survived verification and are fixed or applied since 2026-11-14 (its §8), G16-G20 survived a resumed verification the same day and are fixed since (§9), G21 and G23-G26 survived its next pass on 2026-11-15 and are fixed since (§10), two review questions open, 2 refuted; its last 37 were verified on 2026-11-17 (§11: 31 survived, 1 moot, 5 refuted) and thirteen logic defects fixed the same day (§12), the two that needed a ruling moved to entries of their own, the dead-code and duplication survivors not taken up; 15 slices remain unread | 2026-11-13 |
| `keyed-write-beside-a-subquery.md` | an `UPDATE` or `DELETE` whose `WHERE` holds a subquery collects its rows in a read walk and writes them in a write walk, whatever tuple key the clause names; CU-S3b keyed every other write by its key and the operator left this case out of it | 2026-11-17 |
| `archiver-stalls-on-a-hole-among-leftover-segments.md` | the audit's G31: the WAL archiver starts at the lowest segment file on disk and never skips, so a hole among leftover segments stalls it for the run and pins recycling; three options, split on what B11 covers | 2026-11-17 |
| `well-known-name-refusal-in-create-namespace.md` | the audit's G39 with G53: `CREATE NAMESPACE` refuses a well-known object's stored name only on the catalog that ran `Bootstrap()`, and the refusal protects nothing; delete it with the registry (NS7 as written) or refuse on every catalog | 2026-11-17 |
| `lazy-id-assignment-census.md` | CS-Q9's census: every reading of `Transaction::id()` and every `kNoTrxId` comparison a write path makes, for a later order that would draw an undeclared transaction's id at its first write instead of at `BEGIN` | 2026-11-15 |
