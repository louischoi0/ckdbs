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
- Reserved-and-rejected: `>` / `>=` **and `=`** (Unsupported, AS11 as revised
  2026-08-08 - `=` was briefly accepted as meaning `aggregate <= N`, which
  enforced something other than what was written), `DEFERRABLE`, `NOT VALID`.
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

## AST04 — Bound Cabin storage engine  **[BUILT 2026-08-09]**

**Built.** `PageType::kCabinBound` (a class of its own, not a subtype flag -
`spec-eviction.md` EV3 resolves pinning from the page kind, and a flag would
put the answer one indirection past where the sweep can cheaply ask);
`storage/cabin_bound_page.hpp` - the 32 B entry codec with explicit shift/mask
packing and `static_assert`s on every offset, and the headered page holding
254 of them; `exec/bound_cabin.hpp` - the group directory, the canonical group
key encoding, the running `{count, sum}` header, `Admit`/`Apply`/`Unapply`,
and `VerifyAgainstEntries` for §7's re-summation. All five acceptance
criteria have a test in `tests/bound_cabin_test.cpp`.

**Four things decided in code, as the deliverable allows.**

*A page class, not a subtype flag* - see above.

*Cardinality moves by one per entry whatever the value.* A summed zero is
still a row; making the count depend on the value would make `COUNT(*)` wrong
for any relation containing one, and it would hide until the data had one.

*The directory holds the group key by value and confirms it.* A header is
found by hash and then checked against the stored key
(`docs/feat-cabin.md` §12.3). Trusting the hash would merge two groups, which
for a structure an admission check reads is a wrong answer rather than a slow
one.

*The directory is memory-resident, the entries are not.* Entries are on
durable checksummed `kCabinBound` pages; the directory is rebuilt from them,
which is AST05's WAL replay. Until AST05 lands a restart loses the directory,
which is why AST06's publish step and not this structure is what turns
enforcement on.

**An honest limit in the collision test.** A true 64-bit FNV-1a collision is
~2^32 work to find and is not a unit test. What is tested is the pair of
properties the isolation is made of - distinct keys never resolve to each
other, across 500 groups, and the lookup confirms rather than assumes. The
residual risk is that the confirmation loop is never entered with two
occupants.

**What this task deliberately did not do**, against its own deliverable list:
the pinned-page discipline is *asserted* but its enabling half lives
elsewhere - `EvictColdFrames` skips a `kCabinBound` frame by class, and
eviction is off engine-wide until the `PageRef` migration
(`docs/workplan-eviction.md` EVT02).

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
- Cutover protocol for writes admitted during the build. **Decided
  2026-08-08: the membership-check protocol** (spec §8.1a). A row counts as
  incorporated iff its pk is present in the Bound Cabin - never inferred from
  pk ordering or scan position. The pk-watermark scheme this replaces was
  invalidated by an engine fact: `keystoneid-invariant.md` K3 withholds any
  promise about pk ordering on purpose, so `pk <= watermark ⇒ already scanned`
  is not a predicate this engine supports. Correctness reduces to
  check-then-apply atomicity, which the home core's cooperative event loop
  already provides - no new mechanism.
  Consequences to honour when building it: scan order is correctness-
  irrelevant (page order suffices); build-time write deltas apply at **commit**
  time, so undo integration stays out of the build phase; publish is the single
  commit point (validation + catalog row + plan-cache invalidation in one
  step). A build-scoped temporary pk hash set for unbounded SUM groups is
  **reserved as a measured optimization, not a v1 default** - do not build it
  without a measurement asking for it.

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

## AST08 — Error semantics: `AssertionViolation`  **[BUILT 2026-08-09]**

**Built.** `StatusCode::kAssertionViolation` + `Status::AssertionViolation()`
(base/status.hpp, non-retryable — the enum comment carries the argument,
including why §4.3's bounded false rejection deliberately does not earn the
retryable bit); the newline-protocol spelling
`ERR ASSERTION_VIOLATION retryable=0 <msg>` in `server::ErrorReply`, which is
now declared in `command_dispatcher.hpp` so all three constraint spellings are
pinnable by a socket-free test; and `exec::AssertionViolationMessage()`
(`include/kds/exec/assertion_violation.hpp`), the one place §4.4's format
lives. Golden tests in `tests/assertion_violation_test.cpp` compare whole
strings (the spec's own example byte for byte, SUM naming its column, a
varchar key, a DATE key rendering as a date through the two-argument
`FormatValue`), and `status_test.cpp`'s every-code guards cover the new code.

**One decision made, as the format allowed.** `<N>` in the message is the
**enforced ceiling** (`AssertionStmt::enforced_max()`), not the declared
literal: a `COUNT(*) < 5` refusal fires when the count would reach 5, and
"would exceed bound 5" would then be literally false. AS11's truthfulness
rule, applied to the error surface; a test pins it.

**Deferred, recorded rather than faked.** The KWP error frame and KDS Studio
display: KWP has no caller, and `wire::ErrorCategory` gains assertion's entry
with the P12 error registry **beside `kFkViolation`'s, which is also still
absent** — the FK precedent this follows.

**The AS9 conflict is real and is AST07's to decide, with the facts now
gathered.** AG3's SUM overflow is a *read-path* error (a SELECT fold) and
reads never poison — `Session::Poison()` has exactly one caller,
`EndWrite()`, so *every* failing write statement inside an explicit
transaction poisons the session, `kFkViolation` included. An assertion check
lives on the write path, so AS9's "the transaction remains open and usable"
cannot hold as written unless AST07 special-cases the violation before
`EndWrite`'s poison — which would make an assertion refusal the first write
failure that does not poison, a per-transaction-atomicity carve-out the txn
docs would have to own. Nothing in AST08's surface encodes either answer;
the acceptance line "statement error leaves transaction open" transfers to
AST07 with this question.

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
