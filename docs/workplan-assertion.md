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

## AST05 — WAL integration and recovery  **[BUILT 2026-08-09, gated half registered below]**

**Built.** The five record types (`kAssertReserve = 18` … `kAssertDrop = 22`,
`kMaxAssignedRecordType` → 22, no format bump — IX08's precedent for a
record-type append); payload codecs in `wal/payload.hpp` under every house
convention (lengths from spans, Corruption for a tail that lies, offsets
pinned); and the replay fold, `exec::ReplayAssertionRecord()`
(`include/kds/exec/assertion_replay.hpp`) — one call applies one decoded
record to the store and the group directory, once, in stream order.
`tests/assertion_wal_test.cpp` proves §7's claim in its strongest form: a
live run and its replayed twin compared **byte for byte on the page**,
header by header in the directory, `header == Σ(entries)` through AST04's
`VerifyAgainstEntries`, and the admission boundary answering identically on
both sides — the "no gap where a violating write could be admitted" half.

**Five decisions made in code, as the spec left them.**

*RESERVE and BUILD share one payload* (HEAP_INSERT/OVERWRITE's precedent),
differing in the envelope's type, ownership (txn versus `kNoTxnId`) and one
bit — a RESERVE's entry carries `kEntryReserved`, a BUILD's must not — and
replay checks that bit, so the two cannot be confused on disk.

*No delta field anywhere.* The entry's inline aggregate value **is** the
group delta (a COUNT assertion writes 1, §5.1): one number, one place to be
wrong.

*The group key rides in RESERVE/BUILD/ROLLBACK payloads*, because an entry
does not carry its group and the directory cannot be rebuilt without it —
carrying it is what lets replay restore the directory without re-reading
any relation row.

*COMMIT batches per page, not per transaction*: a physiological record
describes one page, so a transaction whose reservations span N pages
commits with N records. ROLLBACK and BUILD are per entry; batched siblings
are append-only additions the day a measurement asks.

*A record whose assertion the replay context cannot resolve is skipped
whole, page half included* — after ASSERT_DROP the cabin's pages are freed
and may be reused, so touching one would corrupt an unrelated page.

**One finding, recorded rather than solved: the checkpoint-genesis gap.**
A directory folded from records needs the records from the cabin's birth
(the ASSERT_BUILD run), not merely from the last checkpoint, because
nothing durable holds the group headers a checkpoint-bounded replay would
start from — AS5's "not a separate store" has this as its price. Closing it
means the checkpoint persists the directory, or assertion replay starts at
each cabin's build; **no milestone owns that**, the same way nothing owns
single-core recovery itself, and the fold is correct for whatever record
range recovery eventually feeds it.

**The gated half, registered as follow-ups.** The S-2 crash matrix
(crash before/after each record type at every boundary) — the harness does
not exist; register with the SIM series. Transaction-recovery integration
(compensating ASSERT_ROLLBACK for in-flight reservations at crash) — the
fold handles the record; nothing emits it, because recovery is not
implemented engine-wide. The S-1 integrity-sweep wiring of
`VerifyAgainstEntries` — the sweep does not exist; the hook is built and
tested from AST04.

---

## AST06 — CREATE-time builder and cutover  **[BUILT 2026-08-09]**

**Built.** `exec::BuildBoundCabin()` (`include/kds/exec/assertion_build.hpp`)
- the full scan, the entry-page chain, the group directory, the mid-scan
violation check - orchestrated by the restructured `exec::CreateAssertion()`:
validate → allocate the id → build → publish, with the publish (the
`sys.assertions` row now carrying the real `cabin_root`) as the single
commit point. The dispatcher owns the live directories
(`CommandDispatcher::bound_cabins_`, keyed by assertion id) - the reader
AST07's admission check will consult - and `DROP ASSERTION` now emits
`ASSERT_DROP` and evicts its directory. `tests/assertion_build_test.cpp`
covers the acceptance through the statement surface, and its WAL fixture
proves the strongest AST05+AST06 joint claim: a directory folded from
nothing but the emitted stream (PAGE_INIT → ASSERT_BUILD, via
`ReplayAssertionRecord`) lands on the aggregates the live build reported,
with `header == Σ(entries)` over the replayed pages.

**Deviations and decisions, named.**

*The build is synchronous inside the CREATE statement* - not backgrounded,
not yielding. The engine has no suspendable statement path (`crosscore.md`
P4's largest remaining change), and `index_ddl.cpp`'s backfill set the
precedent. Consequence: no write can interleave, so §8.1a's membership
protocol is met trivially and stays the recorded correctness story for the
day the build learns to yield. The "interleaved writes land exactly once"
acceptance is therefore structural; what is tested instead is the in-flight
case below.

*Visibility is latest settled state, and an unsettled relation refuses.*
The scan judges by `txn::CheckVisibility` (the FK checks' predicate); a row
whose writer or deleter is in flight fails the CREATE with `kTxnConflict`,
retryable - counting it and losing the abort would overstate the group
forever (nothing prunes), skipping it and seeing the commit would
understate it, which is the one wrong answer. Delete-marked rows are
excluded; a committed DELETE's row does not count.

*A failed build leaves an unreachable page chain, a burned row id, an
`ASSERT_DROP` discard marker when a WAL is attached, and no catalog row* -
the backfill's precedent, since page reclamation does not exist. The id is
allocated **before** the build because `ASSERT_BUILD` records carry it; K3
makes a burned id free.

*`enforcing` stays 0, now by an explicit conjunction.* SHOW ASSERTIONS used
to derive it from `cabin_root` alone, which AST06 turns into a lie - the
structure exists, nothing checks a write. `kWritePathEnforcesAssertions`
(command_dispatcher.cpp) is the second conjunct, false until AST07 flips
it; the CREATE reply reports `cabin_root`/`rows`/`groups` and still says
`enforcing=0`.

*No `BumpVersion()` at publish.* It is private on purpose, and nothing
cached is derived from a `sys.assertions` row - the same recorded-no-bump
pattern two catalog mutators already use. AST07, whose check steps make a
plan a function of the assertion set, owns making the publish invalidate
plans and owns choosing the door.

*The WAL half*: `PAGE_INIT` (the payload's page_type byte says
`kCabinBound`) per allocated page, a full page image of the old tail on a
chain link edit - no record type describes a link edit, the heap chain's
exact reason - and one `ASSERT_BUILD` per entry, stamped through
`StampPageLsn` so the store's gate holds flushes behind the log.

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

## AST07 — Write-path integration: reservation protocol + undo  **[BUILT 2026-08-09]**

**Built.** `exec::AssertionEnforcer` (`include/kds/exec/assertion_check.hpp`):
the registry of live assertions (resolved once at CREATE, never a catalog
scan or re-parse per write), the admission checks, the reservations, and the
per-transaction pending set that COMMIT clears (flag transitions +
`ASSERT_COMMIT` per page) and ROLLBACK reverses (`ASSERT_ROLLBACK` per
entry, the envelope's flag byte marking a departure). Hooks: INSERT admits
before the row id (FK's ordering - a refusal burns nothing) and reserves
after placement; UPDATE checks and reserves per row before the undo record;
DELETE reserves its departure per row; `EndWrite`/`HandleCommit`/
`HandleRollback` settle the transaction's set on every path, the
no-manager configuration included (each statement is its own transaction
under `kBootstrapXid`). `enforcing` is now the three-way conjunction - root,
write-path constant (flipped true), registry-resident - so a restart's
surviving catalog row reports 0 rather than claiming a check that cannot
run. `tests/assertion_enforce_test.cpp` covers the acceptance: the bound-1
race admits exactly one, abort restores exactly (INSERT and DELETE both
directions), group-move charges only its destination, a decreasing SUM
UPDATE is check-free *and* still maintained, an aggregate-invariant UPDATE
reserves nothing, and the fresh-dispatcher case pins the honest recovery
gap.

**Three decisions, named.**

*The FK shape, for FK's reason.* The spec says step-chain compilation; that
mechanism still does not exist (INSERT compiles to no chain, UPDATE/DELETE
walk outside the step VM), so this is `fk_check.hpp`'s shape - one
implementation, three call sites, no trigger machinery. Consequence: no
compiled plan embeds a check step, so the plan cache does not depend on the
assertion set and CREATE/DROP ASSERTION need no plan invalidation - the
door AST06's publish comment left open closes itself.

*Every write is a departure, an arrival, or both.* `kEntryDeparture`
(cabin_bound_page.hpp) marks an entry contributing (-1, -value); an UPDATE
that moves the aggregate is departure + arrival (same group or not, one
code path), DELETE is a check-free departure - **required by §5's "100% of
live rows" coverage contract**, not by any check: a header that kept
counting deleted rows would overstate forever and refuse valid writes
without bound. AST04's "cardinality moves by one per entry" is amended to
"by *plus or minus* one, by the flag, never by the value"; re-summation and
AST05's replay both read the flag, and the rollback record carries it in
the envelope's per-type flags byte.

*The AS9 conflict, operator-decided: poison.* Spec §4.4 carries the
amendment and the argument; the acceptance line "statement error leaves
transaction open" is satisfied in its autocommit sense only, deliberately.

**Gap carried, not hidden**: the S-3 concurrent-history checks stay gated
with the isolation checker that does not exist, per the workplan's own
gate. Reservation *page entries* orphaned by an abort ride on purge with
everything else.

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

## AST09 — Observability: ANALYZE + production counters  **[BUILT 2026-08-09, two halves recorded]**

**Built: the production counters.** `LiveAssertion::Counters` - `checks`
(admission checks run), `violations`, `reserved` (entries applied, arrivals
and departures both), `aborted` (reservations reversed) - incremented at
the exact enforcement sites AST07 built, monotonic, registry-resident.
`SHOW ASSERTIONS` prints them **only while the registry holds the
assertion**: they live and die with the directory, so an unenforced row
prints no numbers rather than zeros that would read as "counted, and
nothing happened". Pinned under AST07's own scenarios in
`tests/assertion_enforce_test.cpp`, cold-dispatcher case included.

**Recorded, not faked - two deliverables with no vehicle.**

*The ANALYZE `Assertion` line.* ANALYZE is a dispatcher prefix that wraps
the SELECT path only; a *checked* statement is a write, and no write
statement can be ANALYZE'd. The line lands when ANALYZE learns write
statements, which is its own decision about a surface far wider than
assertions - not something to decide from here. `group_dir_probes` waits
with it.

*`hint_heals` and the dev-mode timing hooks.* No code path reads a Bound
Cabin entry's location hint (admission is O(1) against the header; no read
path walks entries), so the counter could never move - and a counter
nothing can increment is worse than none, the INDEX_PAGE_INIT argument.
The dev/production profiling split the spec references has no
infrastructure in this engine to hook into.

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

## AST10 — End-to-end validation and documentation close-out  **[BUILT 2026-08-09; bench measurement delegated]**

**Built.** The E2E scenario
(`AssertionEnforceTest.EndToEndLifecycleLeavesNoResidue`): create → load →
CREATE ASSERTION → mixed workload (admitted and refused inserts, a
group-move, an update down and up, a delete, a poisoned transaction rolled
back) → DROP → no observable residue, name free, enforcement off at once.
README gained the low-key positioning bullet ("SQL-92 CREATE ASSERTION,
restricted class, lock-free at admission"). The reserved-form refusals are
confirmed standing by the suite (`assertion_ddl_test.cpp`, all green at
close-out). The INSERT-path overhead benchmark runs via
`tools/assertion_benchmark.py` (staged at f7462e2, before enforcement
existed) and its write-up lands as `bench/results-assertion.md` -
delegated to the bench owner at close-out; the number to expect is shaped
by "a batch of one is a batch": the entry write and ASSERT_RESERVE record
amortize under fsync at strict/group and show under relaxed.

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
