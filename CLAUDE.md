# CLAUDE.md — KDS Storage Engine

Working guide for AI development agents. **This file is a guideline and a
milestone map, not a detail store**: every subsystem's decisions, task
breakdowns, amendments and measurements live in the `docs/` file that owns
them, and this file links there. `docs/heap-and-tuple.md` is the
authoritative design spec; when this file and a spec conflict, the spec wins
— flag the conflict instead of guessing. The verbose pre-2026-08-10 version
of this file is in git history.

## What This Project Is

KDS is a **userspace OLTP-specialized database storage engine written in
C++**, targeting financial systems. Its purpose is fast and *correct* OLTP
through two engine-native mechanisms:

1. **Engine-driven physical page optimization** — runtime statistics
   reorganize where tuples physically live, not just how queries plan.
2. **Waystone** — a record of where a previous execution of a query pattern
   found its rows, so a repeated pattern can look instead of searching.

Feature scope is deliberately narrow; do not add general-purpose DBMS
features unasked. **KDS is userspace software** — no kernel-module code,
kernel headers, or kernel-only APIs.

## Milestones

One line per subsystem: status, then the owning docs. Read the owning spec
before touching a subsystem — each carries rules that are correctness
statements, not style.

| Subsystem | Status | Owning docs |
|---|---|---|
| Pages, semi-sorted heap, Keystone, fixed-length tuples, var-heap | Built | `docs/heap-and-tuple.md` (authoritative), `docs/rule-fixed-length-tuple.md`, `docs/page.md` |
| Clustered B+ tree | Built | `docs/heap-and-tuple.md` |
| WAL | Every data mutation logged (heap, undo, var-heap, index, assertions); **catalog/DDL writes are not**, and **recovery not implemented** — nothing reads the log back | `docs/wal.md` |
| Transactions & MVCC | Built (T01-T14); no purge, and MVCC ships before recovery (see §8's gap) | `docs/txn.md`, `docs/txn-workplan.md` |
| Query language, parser, step chains, joins, subqueries | Built (V01-V19; V09 pagination — ORDER BY pk / LIMIT / OFFSET — done 2026-08-10). Open: V08's `IN (value list)`, V11 (`WITH (...)` table options), V12 (`SET DURABILITY`, the session/admin classes), V20's test, and phase V-6's blueprint parser | `docs/parser-v2.md`, `docs/parser-v2-workplan.md` |
| Aggregation (GROUP BY, COUNT/SUM/MIN/MAX/AVG) | Built (AG01-AG10) | `docs/feat-aggregate.md`, `docs/workplan-aggregate.md` |
| Aggregate performance | AP01-AP03 built, AP05 next | `docs/workplan-aggregate-perf.md` (start at "Where to pick this up") |
| Types: DATE, TIMESTAMP, DECIMAL, DECIMAL128 | Built (TY01-TY11); `float` stays refused | `docs/spec-types.md`, `docs/workplan-types.md` |
| Waystone (pattern-keyed access trails) | Recording + replay built (P01-P13); retention/decay/epoch bumps not (P15-P17) | `docs/waystone-concpets.md`, `docs/waystone-workplan.md` |
| CREATE PATTERN | Built through spec §8 step 4 | `docs/spec-create-pattern-user-defined-patterns-v1.md` |
| Cabin (value-observed authoritative store) | v1 built (CB01-CB11); entry sets memory-resident | `docs/feat-cabin.md`, `docs/cabin-workplan.md` |
| Secondary indexes (multi-column, covering) | All built (IX01-IX16) | `docs/feat-index.md`, `docs/workplan-index.md` |
| Foreign keys | Declared and enforced (FK-M1..FK-M5); CASCADE/SET NULL out of v1 | `docs/impl-foreign-keys.md` |
| Assertions (group-level constraints) | **Complete and enforcing** (AST01-AST10); recovery-side registry rebuild outside the series | `docs/feat-assertion.md`, `docs/workplan-assertion.md` |
| Access statistics | Built (`SHOW ACCESS`) | `docs/heap-and-tuple.md` §7 |
| ALTER TABLE | Built 2026-08-10 (AL1-AL9, ALT01-ALT05): catalog-only — RENAME TO / RENAME COLUMN; assertions RESTRICT; everything data-moving refused | `docs/spec-alter.md`, `docs/workplan-alter.md` |
| DROP TABLE | Built 2026-08-10 (DT1-DT6, DT01-DT05): catalog-scoped — oid tombstoned and never reissued, pages orphan (reclamation gated), fkeys/assertions RESTRICT | `docs/spec-drop-table.md`, `docs/workplan-drop-table.md` |
| Bulk insert | T1 built 2026-08-10 (BLK01-BLK05): multi-row VALUES, full pipeline per row, BI5 fingerprint rule; T2 gated on the KWP server | `docs/spec-bulkinsert.md`, `docs/workplan-bulk-insert.md` |
| Physical optimizer, Part I: relayout | Built and measured (PX01-PX08), **shadow-only as a finding** — every move blocked by a named §6 gate | `docs/feat-physical-optimizer.md`, `docs/workplan-physical-optimizer.md` |
| Physical optimizer, Part II: Cabin controller | **Complete** (PHY01-PHY08, closed 2026-08-10): controller end to end over the EVT03/EVT06 substrate, `SHOW CABIN_OPTIMIZER` observability, E2E lifecycle test, measured in `bench/results-cabin-optimizer.md` (zero-candidate overhead unmeasurable, 10.9× on the 10k improvement case); managed state is memory-resident — restart forgets, re-observation rebuilds | same docs, Part II / §II.1-§II.7 |
| Buffer-pool eviction | EVT01/EVT02 partly, EVT03 (writeback) and EVT06 (scan ring) built; full CLOCK reclamation still gated on the `PageRef` migration | `docs/spec-eviction.md`, `docs/workplan-eviction.md`, `docs/page.md` §3 |
| Cross-core execution | P0, P1, P2, P6 (catalog half + CC7 decision + P6b handoff + P6c placement), P5-shape row-id leasing, P4 restriction half — all built by 2026-08-10. **The one remaining piece is the step pipeline itself** (statement dispatch + the executor's coroutine conversion) | `docs/crosscore.md`, `docs/workplan-crosscore.md`, `docs/sched.md` |
| Task representation | Decided and built: C++20 stackless coroutines | `docs/sched.md` §3 |
| Wire protocol KWP/1 | Frame codec only; the server speaks the newline text protocol | `docs/protocol.md`, `docs/protocol-wp.md`, `docs/client-manual.md` |
| Keystone id issue-once contract | K-M1, K-M4 built; K-M3 partly (pk-update refused, but at the dispatcher and not as `Unsupported`); K1 does not hold across a crash — read the findings before quoting the invariant | `docs/keystoneid-invariant.md`, `docs/keystoneid-k0-findings.md` |
| Observability | Proposal only, nothing implemented | `docs/observability.md` |
| User manual | `manual/` — SQL surface written, verified against code | `manual/sql/sql.md` |

**Engine-wide known gaps** — what is missing, what a restart loses, and
stale claims found in docs — live in `docs/known-gaps.md`.

Superseded and kept only as history — do not build from them:
`docs/parser.md`, `docs/parser-workplan.md`, `docs/step-chains.md` (all →
`docs/parser-v2.md`); `docs/page.md` §7's eviction proposal (→
`docs/spec-eviction.md`).

Numbering collides across docs (three `P`-schemes, two `R1`s): **cite the
file, never the bare number.**

## Hard Invariants — never violate, never "temporarily" bypass

Numbered to match `docs/heap-and-tuple.md` §8.

1. 8192-byte pages; `uint32_t` page ids; `0xFFFFFFFF` reserved as invalid.
2. `min_key` is immutable after page creation.
3. No tuple with `id < min_key(page)` in that page, ever — including by relayout.
4. Tuples within a heap page are unordered.
5. The Keystone column is exactly `id:40 | flags:8 | reserved:16`.
6. The Keystone word is read/written as an atomic `uint64_t` (CAS for updates); on-disk encodings use **explicit shift/mask helpers only — compiler bitfields are forbidden** for any persisted format.
7. Ids stored outside the tuple header are zero-extended `uint64_t`; the upper 24 bits are always 0.
8. Waystone is advisory: deleting it wholesale may cost performance but must never change a query result.
9. Waystone is never **authoritative**: a reader may consult it for *where to look* only, with the miss/Keystone/MVCC/fall-through discipline `docs/heap-and-tuple.md` §8 spells out. It chooses where to look, never what is visible.
10. No single canonical in-memory tuple; consistency comes from page pin and latch discipline.
11. **Every** relation's pk is system-generated autoincrement, issued by `Catalog::AllocateRowId()`, carried only by the Keystone word, never updatable. A caller-supplied pk on insert is a defect. First column must be integer-typed.
12. The tuple MVCC header is exactly `trx_id:48 | undo_ptr | data_len | flags` = 20 bytes. There is no `xmax`.
13. **Every tuple is fixed-length**: row size is a schema constant, variable-width values occupy one tagged cell of `kds.inline_cell_width` bytes. A disagreeing length is `Corruption`, never interpreted.
14. **Var-heap values are immutable per version** and `kVarHeap` pages are never relocated. Authoritative data — advisory rules do not apply to it.

## Working Rules

- Fresh codebase: idiomatic modern C++, not kernel-style C. RAII for every
  resource; no raw `new`/`delete` in engine logic; fixed-width integer types
  and `static_assert`ed layouts for on-disk structs; named `constexpr` for
  every size/offset with the derivation in a comment. Full rules:
  `docs/rules.md`.
- Document the lock/atomic protocol at the top of each subsystem file.
- Tests accompany every subsystem; contract suites (waystone, index, cabin,
  types, assertion) compare configurations byte-for-byte — keep them green
  and extend them with the feature.
- **Measure in `build-release`, never `./build` (Debug)** — Debug has
  reported the wrong sign twice. Per-statement fixed costs: server CPU,
  interleaved A/B. **Re-measure a premise before building the fix.** Details:
  `docs/workplan-aggregate-perf.md`.
- Every refusal carries the byte position of the offending token;
  `Unsupported` means "understood and declined", `InvalidArgument` means
  "simply wrong". Truthfulness beats convenience: never accept a spelling
  and enforce something other than what was written.
- Nothing new is reserved lightly: keywords hash as identifiers, and
  `kFingerprintVersion` moves only per `fingerprint.hpp`'s bump rule (the
  golden corpus pins it).

## Open Decisions — DO NOT assume or silently pick

The full statements, options and constraints live in the owning docs; this
is the index. When work touches one: stop and ask, or implement behind an
interface that keeps every listed option viable.

- **Storage** (`docs/heap-and-tuple.md` §8, `docs/page.md`,
  `docs/rule-fixed-length-tuple.md`): heap page split policy;
  `inline_cell_width` default; spilled-value size cap; prefix-inlining
  trigger; purge cadence; the 16 reserved Keystone bits; id reuse; I/O
  backend; whether invariants 3/11 are ever relaxed.
- **Waystone** (`docs/waystone-concpets.md` §9): per-pattern retention and
  eviction; persistence class of trail pages; `arg_hash` collisions; decay
  and sampling; whether invariant 9 is ever amended.
- **Transactions & WAL** (`docs/txn.md` §9, `docs/wal.md` §15): undo
  retention and `SnapshotTooOld`; trx-id wraparound; `kTrxIdBlockSize`;
  cross-core commit and recovery under a changed core count.
- **Protocol** (`docs/protocol.md`): TLS mode; frame/batch/timeout sizing;
  compression; flow control; the authorization model.
- **Parser** (`docs/parser-v2.md`): statement-class ratification; slot-table
  cap; whether `kUnclassified` is production-legal.
- **Cabin** (`docs/feat-cabin.md` §11): budgets and caps; the `CABIN AUTO`
  threshold; pruning cadence; entry-set persistence; multi-column keys.
- **Aggregation** (`docs/feat-aggregate.md` §10): `aggregate_max_distinct`;
  `MIN`/`MAX` with `DISTINCT`; lifting `ORDER BY`/`HAVING`; pre-aggregation
  below a join is *rejected*, not open.
- **Cross-core** (`docs/crosscore.md` §9, `docs/sched.md` §10): the 2PC
  protocol and everything gated behind it; batch/credit/ring/extent sizing;
  the `ring_full` retry protocol; core-count changes; placement policy.
- **Foreign keys** (`docs/impl-foreign-keys.md`): forward-check expression;
  the busy status code; heap parents; `kFkNullable`; CASCADE/SET NULL.
- **Indexes** (`docs/feat-index.md` §13): `kIndexStringKeyBytes`; split
  point; column caps; entry reclamation; the index-only scan (gated on a
  visibility witness — do not attempt a partial one); `UNIQUE`; whether the
  measured crossover is ever acted on.
- **Physical optimizer** (`docs/feat-physical-optimizer.md` §6, §10): the
  three enactment gates (each owned elsewhere, reported by name in
  `SHOW RELAYOUT`); the mover; `decay_half_life`.
- **Project**: C++ standard/toolchain pin, build system, test framework
  (propose, don't decide).

## Maintenance rule for this file

When a decision lands: update the spec that owns it, move the item out of
Open Decisions here, and flip the milestone row — **do not** write the
detail into this file. A finding with no owning doc gets one (or a section
in the nearest spec), never a paragraph here.
