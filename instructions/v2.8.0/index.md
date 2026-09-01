# instructions/v2.8.0 — the version's operator input

Opened 2026-09-01 on `worktree-v2.8.0-ratification-ae` at `ea49be1`
(`v2.7.0-73-gea49be1`).

**The version in one sentence:** v2.8.0 drops the **range split** — one
range per relation, the mover (R5) not a priority — **keeps and pushes
every other kind of parallelism**, cross-owner 2PC included, and finishes
**assertions, the Cabin, foreign keys, secondary indexes and the UNIQUE
index**: completeness first, then extreme performance optimization on the
same five.

The seam that governs everything filed here: **range**-granular
parallelism (one relation cut across cores) is dropped;
**owner**-granular parallelism (two relations, two cores, one
transaction) is kept and maximized. A refusal that fires on a split
relation is withdrawn work; a refusal that fires because two relations
sit on two cores is this version's work.

| document | what it is |
|---|---|
| `ratification-ae.md` | The governing direction. The operator's words verbatim (AE-1), the range/owner seam (AE-2), what it decides (AE-3), the removal inventory and what is explicitly kept (AE-4), the line removal may not cross (AE-5), what each of the five subjects owes (AE-6), the parallelism question this version opens (AE-8, answered by AF), and the correction record for AE's first draft (AE-9) |
| `ratification-af-namespace.md` | **AE-8's answer.** A namespace selects a relation's owner core, fixed by the namespace's first relation — a logical grouping, no physical meaning, 2PC untouched. AF-3 is the survey finding that `namespace_oid` is already on disk (no format bump); AF-4 is why DA2's 0.51× argues *for* this rather than against it; AF-5 the proposals; AF-6 the syntax decision the operator still owns; AF-7 the tasks |

**Read AE before filing anything here.** It withdraws the *range-shaped*
half of work orders SA and SB (`instructions/v2.7.1/`) while keeping
their owner-shaped parts as this version's work, and it keeps work order
IB (`instructions/v2.7.2/index.md`) — a single-relation index feature —
as a subject rather than a casualty. The IB workplan lives at
`docs/inflight/in-progress/workplan-ib.md`.
