# ASSERTION — Workplan (AST01–AST10)

Status: **READY FOR EXECUTION**
Spec: `assertion.md` (normative). Related: `cabin.md` (rev required),
`wal.md`, `txn.md`, `analyze.md`, `testing-workplan.md`.

Execution order is the numbering order unless a dependency says otherwise.
Each item lists scope, deliverables, and acceptance criteria. All new code
follows the engine rules: explicit Status error types (no throw),
thread-per-core with core-local state, deterministic tests, field-wise
memcpy page access (no reinterpret_cast overlays).

---

## AST01 — `cabin.md` revision: Bound Cabin class split  **[PREREQUISITE]**

**Scope.** Amend the Cabin spec to define the two-class model exactly as in
`assertion.md` §5: Observational (existing contract unchanged) vs Bound
(full coverage, pinned, logged/headered authority, 32 B entries with inline
aggregate value, group headers with running aggregates).

**Deliverables.**
- `cabin.md` rev: class table, entry layouts (24 B observational / 32 B
  bound), group directory + group header definition, lifecycle contracts.
- Explicit statement that Observational Cabin semantics are untouched
  (eviction, dangling-entry discard, advisory hints).

**Acceptance.** Spec review only; no code. `assertion.md` §5 and `cabin.md`
must not contradict each other on any property in the class table.

---

## AST02 — Parser: CREATE / DROP ASSERTION

**Scope.** Grammar per spec §3, bolted onto the current parser (J4
precedent — do not wait for the blueprint parser).

**Deliverables.**
- `CREATE ASSERTION name ON rel GROUP BY (cols) CHECK COUNT(*)|SUM(col) op N`
- `DROP ASSERTION name`
- Reserved-and-rejected: `>` / `>=` (Unsupported, AS11), `DEFERRABLE`,
  `NOT VALID`.
- Create-time validation, maximized (spec §3.1): relation/column existence,
  SUM column int64, operator set, non-negative integer literal bound,
  duplicate name, degenerate predicates.
- AST nodes and statement plumbing; statement class unaffected; pattern
  fingerprinting unaffected (assertion DDL is not fingerprinted; DML
  fingerprints must be byte-identical before/after this change).

**Acceptance.** Parser unit tests: every valid form round-trips; every
invalid/reserved form yields the exact specified Status; fingerprint
snapshot test proves DML fingerprints unchanged.

---

## AST03 — Catalog: `sys.assertions`

**Scope.** Bootstrap the catalog relation per spec §8.2 (source_text model,
single row per assertion, no params table — mirrors `sys.pattern_defs`;
development-phase row-format changes are permitted, no back-compat burden).

**Deliverables.**
- Fixed-page bootstrap entry; row codec; lookup by name and by target_oid.
- RESTRICT hook: `DROP TABLE` fails with `Restrict` if any assertion
  references the relation.

**Acceptance.** Deterministic tests: create → catalog row visible via
`SELECT`; drop → row gone; relation drop under assertion → `Restrict`;
restart → catalog intact.

---

## AST04 — Bound Cabin storage engine

**Scope.** Storage per spec §5 on top of existing page/buffer
infrastructure (S1 common header, S7 per-core buffer pool, S9 checksums).

**Deliverables.**
- Headered Bound Cabin page kind (new `kCabinBound` page type or subtype
  flag — decide in-code, document in cabin.md rev).
- 32 B fixed entry codec (field-wise memcpy helpers).
- Group directory: `group_key_hash → group header {count, sum, entry list}`.
  Group-key hashing over the GROUP BY column values with a canonical
  encoding (reuse KWP value encoding, D5).
- Pinned-page discipline: Bound Cabin pages are exempt from eviction;
  interaction with the frame pool documented and asserted in debug builds.
- Checked int64 aggregate arithmetic (AG3 semantics).

**Acceptance.** Unit tests: entry codec round-trip; group header updates;
hash collisions produce correct per-group isolation; eviction sweep never
touches pinned Bound Cabin frames; overflow on sum maintenance yields the
statement-error path, never wraparound.

---

## AST05 — WAL integration and recovery

**Scope.** Record types and replay per spec §7: `ASSERT_RESERVE`,
`ASSERT_COMMIT`, `ASSERT_ROLLBACK`, `ASSERT_BUILD`, `ASSERT_DROP`.
WAL-before-data ordering throughout.

**Deliverables.**
- Record encoders/decoders + replay handlers restoring group headers and
  entries exactly.
- Transaction recovery integration: crash with in-flight reservations ⇒
  compensating `ASSERT_ROLLBACK` during recovery; constraint enforceable
  immediately at restart with zero enforcement gap.
- Offline verification hook: re-sum entries vs group headers (wired into the
  harness integrity sweep, S-1).

**Acceptance.** Deterministic crash-recovery tests (harness S-2 loop):
crash before/after each record type at every step boundary; post-recovery
invariant `header == Σ(entries)` for all groups; no gap where a violating
write could be admitted. **[GATED on recovery harness availability — same
gate as testing-workplan SIM items; if the S-2 loop is not yet ready, land
replay unit tests now and register the crash matrix as a follow-up in the
SIM series.]**

---

## AST06 — CREATE-time builder and cutover

**Scope.** Full-scan build per spec §8.1 on the relation's home core,
background scheduling group, cooperative yielding.

**Deliverables.**
- Scan → group aggregate accumulation → Bound Cabin materialization →
  `ASSERT_BUILD` WAL.
- Violation during build ⇒ CREATE fails with `AssertionViolation` naming the
  first violating group; partial build discarded (WAL'd teardown).
- Cutover protocol for writes admitted during the build. Simplest correct
  v1 scheme (recommended): because builder and writers share the home core's
  event loop, run the build as a cooperative task and have concurrent write
  steps against the relation *also* apply their deltas to the under-build
  structure once their group has been scanned, or queue them for the builder
  otherwise; enforcement begins atomically when the builder publishes the
  catalog row. The chosen scheme must satisfy the normative requirement of
  spec §8.1(5) and be documented in code comments + a short design note.

**Acceptance.** Tests: build over pre-populated relation (violating and
non-violating); interleaved writes during build land in the final structure
exactly once; post-CREATE `header == Σ(entries)`; failed CREATE leaves no
trace (catalog, pages, WAL replay all clean).

---

## AST07 — Write-path integration: reservation protocol + undo

**Scope.** Compile the admission/reserve step into statement step chains per
spec §4.2/§6.2 (FK-style step-chain compilation; no trigger machinery).

**Deliverables.**
- Step-chain insertion for INSERT, UPDATE (delta rules of §4.2, including
  group-move = departure + arrival applied atomically), DELETE explicitly
  exempt (AS11).
- Reservation: admission check → group delta → RESERVED entry → proceed.
- Commit path: RESERVED flag clear (batched per transaction).
- Abort path: undo-chain integration removing reserved entries and applying
  negative deltas (`ASSERT_ROLLBACK`).
- Statement error surface on violation (uses AST08 Status).

**Acceptance.** Deterministic multi-statement tests: race of two inserts
into a group at bound-1 admits exactly one; abort restores aggregates
exactly; UPDATE group-move charges only the destination; UPDATE with
negative sum delta is check-free; interleavings exercised via the
deterministic scheduler. Concurrent-history checks registered with the S-3
isolation checker. **[S-3 dependency GATED as in testing-workplan.]**

---

## AST08 — Error semantics: `AssertionViolation`

**Scope.** Status catalog addition per spec §4.4 (D9 error-code coherence).

**Deliverables.**
- `AssertionViolation` Status code + message format
  `assertion "<name>" group (<col>=<val>, ...): <AGG> would exceed bound <N>`.
- Group-key rendering via existing value formatting (type-correct for int
  and varchar group columns).
- Wire mapping (KWP error frame) and client (KDS Studio) display verified.

**Acceptance.** Golden-message tests; wire round-trip test; statement error
leaves transaction open and usable (explicit test).

---

## AST09 — Observability: ANALYZE + production counters

**Scope.** Spec §9.

**Deliverables.**
- ANALYZE `Assertion` line (checks / reserved / violations /
  group_dir_probes) for checked statements.
- Per-assertion production counters in the stats system: checks, violations,
  abort-rollbacks, hint-heal events.
- Dev-mode timing hooks per the dev/production profiling split.

**Acceptance.** ANALYZE snapshot tests; counters monotonic and correct under
the AST07 test scenarios.

---

## AST10 — End-to-end validation and documentation close-out

**Scope.** System-level pass + doc hygiene.

**Deliverables.**
- E2E scenario in the harness: create relation → load → CREATE ASSERTION →
  mixed workload with violations → DROP ASSERTION → verify no residue.
- Benchmark note: overhead of an enabled assertion on the INSERT path
  (target: within the documented write-amplification budget; report strict /
  group / relaxed durability classes as in the standing benchmark).
- README/feature-doc touch: position as "SQL-92 ASSERTION, restricted class,
  lock-free" consistent with the low-key positioning policy (no marketing
  overreach).
- Confirm `Unsupported` surfaces for every reserved form one final time.

**Acceptance.** Green E2E in CI; benchmark numbers recorded in the perf log;
docs cross-references (`assertion.md` ↔ `cabin.md` rev) consistent.

---

## Dependency graph

```
AST01 (spec) ──► AST04 ──► AST05 ──► AST06 ──► AST07 ──► AST10
AST02 (parser) ─┬────────────────────┘            ▲
AST03 (catalog) ┘                                 │
AST08 (errors) ───────────────────────────────────┤
AST09 (observability) ────────────────────────────┘
```

AST02/AST03/AST08 can proceed in parallel with AST04/AST05.
Gated items: AST05 crash matrix (S-2 harness), AST07 history checks (S-3).
