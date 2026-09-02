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
| `ratification-af-namespace.md` | **AE-8's answer.** A namespace selects a relation's owner core, fixed by the namespace's first relation — a logical grouping, no physical meaning, 2PC untouched. AF-3 is the survey finding that `namespace_oid` is already on disk (no format bump); AF-4 is why DA2's 0.51× argues *for* this rather than against it; AF-5 the proposals; AF-6 the syntax, decided as `CREATE NAMESPACE` + `ns.table`; AF-7 the tasks, AF-T0 and AF-T1 built |
| `workorder-ai.md` | **The peer-writer funding gate's FK arm, narrowed by ratification.** AH-T5's finding filed as an order: the gate that made AH's crossing unreachable, why its stated reason was already removed, and why the cabined and `CannotEnforce` arms are not this order's. AI-R1/R2 were ruled and enacted before the order was filed (`956f00d`); AI-R3 (what the A5 shipped-write cell becomes) and AI-R4 (whether the end-to-end cross-owner INSERT cell lands here or at AH-T6) await the operator |
| `workorder-aj.md` | **AE-6's FK subject, its last open item.** The *reverse* fan-out — the crossing's one standing asymmetry, `fk_check.cpp:190`'s `NotImplemented`, which is why a parent in a cross-owner foreign key cannot be deleted. The design turns on what `4d520de`..`640afdf` settled: the fan-out **enrols nobody**, so the mirror state is one coordinator-local pending-delete set the existing `FkProbeServer` consults, not a second intent on the child's owner. AJ-R1..R4 await the operator; AJ-T0's survey is run and appended, and it corrects one premise in the order's own text |
| `workorder-ah.md` | **AE-6's FK subject.** The forward check crosses the owner by hoisting out of the write scope to the **dispatch fork**, the one place a write already parks — one probe round per distinct owner, never per row, leaving a row-scoped reference intent behind. AH-R6's operator mark: `CheckForeignKeyColocation` **converts** from constraint to recommendation at AH-T4, after the protocol exists and not before. Contract: `docs/spec/foreign-keys.md` §2a |

**Read AE before filing anything here.** It withdraws the *range-shaped*
half of work orders SA and SB (`instructions/v2.7.1/`) while keeping
their owner-shaped parts as this version's work, and it keeps work order
IB (`instructions/v2.7.2/index.md`) — a single-relation index feature —
as a subject rather than a casualty. The IB workplan lives at
`docs/inflight/in-progress/workplan-ib.md`.
