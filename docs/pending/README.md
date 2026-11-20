# `docs/pending/`

**Pending discussions.** Opened 2026-10-08 on the operator's word.

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
| [`v20-insert-slowdown.md`](v20-insert-slowdown.md) | xrock's trade load ran ~3x slower after the move to superblock 20; the comparison was confounded and the investigation deferred | 2026-10-08 |
| [`multicore-scaling-cores4-below-cores2.md`](multicore-scaling-cores4-below-cores2.md) | `cores = 4` delivers 21-28 % less than `cores = 2` on `point` and `insert1`; whether the engine or the client/reactor CPU overlap causes it is unresolved | 2026-10-15 |
| [`whole-engine-audit-unverified-findings.md`](whole-engine-audit-unverified-findings.md) | the whole-engine audit's first run stopped after 6 of 33 slices; its defects are fixed and its dead code removed, and 27 slices are unread | 2026-11-12 |
| [`whole-engine-audit-second-run-twelve-slices.md`](whole-engine-audit-second-run-twelve-slices.md) | the audit's second run read twelve more slices: 66 findings, the surviving logic defects fixed (its §8-§12) but two, which moved to entries of their own; two review questions are open, nine of the last pass's dead-code survivors are removed (its §13), two more and the duplication survivors are not taken up, and 15 slices are unread | 2026-11-13 |
| [`keyed-write-beside-a-subquery.md`](keyed-write-beside-a-subquery.md) | an `UPDATE` or `DELETE` whose `WHERE` holds a subquery is walked, whatever tuple key the clause names; the operator left this case out of CU-S3b | 2026-11-17 |
| [`archiver-stalls-on-a-hole-among-leftover-segments.md`](archiver-stalls-on-a-hole-among-leftover-segments.md) | the audit's G31: the WAL archiver never skips a hole among leftover segments, which stalls it for the run and pins recycling; three options | 2026-11-17 |
| [`well-known-name-refusal-in-create-namespace.md`](well-known-name-refusal-in-create-namespace.md) | the audit's G39 with G53: `CREATE NAMESPACE` refuses a well-known object's stored name only on the catalog that ran `Bootstrap()`; delete the refusal or make every catalog refuse | 2026-11-17 |
| [`lazy-id-assignment-census.md`](lazy-id-assignment-census.md) | CS-Q9's census of every reading of `Transaction::id()` a write path makes, for a later order that would draw an id at the first write instead of at `BEGIN` | 2026-11-15 |
| [`statistics-staleness-and-regather.md`](statistics-staleness-and-regather.md) | nothing decides when a gathered statistic is stale or gathers it again; five options, left out of CY by the operator | 2026-11-18 |

Each entry carries its own account. This table as it was last written in
full: `git show fb65d7bd:docs/pending/README.md`.
