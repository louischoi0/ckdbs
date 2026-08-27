# Status audit — every workplan and spec status claim against the code

Date: 2026-08-08. Report only: **no document was corrected by this audit and no
code was touched.** The index workplan is being corrected separately; the rest
need a human to read this list before any prose moves.

> **Follow-up 2026-08-10: all three STALE findings are now fixed at the
> source**, plus a fourth this audit did not look for. `docs/spec/parser-v2.md:9`
> carries a real status with the unbuilt remainder itemized;
> `docs/rules/keystoneid-invariant.md:7` reports K-M4 done and K-M3 partly (the
> refusal exists at the dispatcher rather than the compiler);
> `docs/spec/wal.md:5` and §11a now list every logged path and name the catalog
> writes as the ones still outside the log. The fourth: a sweep of every
> cited filename found **eleven references to documents that were never in
> this repository** — `storage.md`, `assertion.md`, `cabin.md`, `fk.md`,
> `scheduler.md`, `eviction.md`, `assertion-workplan.md`,
> `testing-workplan.md`, `unique-index.md`, `analyze.md`, `step-chain.md` —
> in the eviction, assertion and README headers; each is repointed or, where
> the document genuinely does not exist (`analyze.md`, `unique-index.md`),
> dropped with the absence stated. This report is left otherwise unedited:
> it is the record of what was true on 2026-08-08.

**How this was verified, and what it is worth.** Every claim below was checked
by reading the repository — the header, source file, test file or benchmark
document the claim itself names — and by `Grep` for the named symbol where a
file alone would not settle it. **Nothing was built and no test was run**, so
no claim here is verified by a passing suite. **`AGREES` means the named code
exists, not that it works.** A claim whose evidence is a measurement or a green
test run is `UNVERIFIABLE` by construction, however plausible it is.

Classification:

| | meaning |
|---|---|
| **AGREES** | the document's claim matches what is in the tree; artifact named below |
| **STALE** | the document says a thing remains or is unbuilt; the code exists |
| **OVERCLAIMS** | the document says built; the code is absent or only partial |
| **UNVERIFIABLE** | no artifact named, or the claim is about a measurement or a test run |

---

## The damage first

**OVERCLAIMS: none found.** No document in `docs/` claims a task is built whose
artifact could not be located. This is the finding the audit existed to
produce, and it is a negative one.

**STALE — 3 findings, in severity order:**

1. **`docs/spec/parser-v2.md:9` — "Status of the work: unbuilt."** The most
   misleading line in `docs/`. It says the parser is recursive descent over
   `SELECT *` with AND-only filters, that "there is no join execution
   anywhere", and that the only piece of the document that exists is the
   fingerprint. All of that was true at V00 and is false now: the step
   compiler (`src/exec/step_compiler.cpp`), the step VM
   (`src/exec/step_vm.cpp`), joins, subqueries and the row-touch budget
   (`include/kds/exec/budget.hpp`) are all in the tree, with
   `tests/exec_chain_test.cpp`, `tests/exec_subquery_test.cpp` and
   `tests/exec_budget_test.cpp` beside them. Its own workplan marks
   `V01`-`V19` done. A reader who trusts the spec's status line and not the
   workplan's would rebuild the language.
2. **`docs/rules/keystoneid-invariant.md:7` — "K-M2..K-M6 not started."**
   `K-M4` is built: `include/kds/catalog/keystone_budget.hpp` holds
   `catalog::BudgetOf()`, `tests/keystone_budget_test.cpp` exercises it, and
   the same document's own §5 text (line 271) already describes
   `ids_remaining` / `budget_used` shipping beside `next_id`. The header
   contradicts the body of its own file.
3. **`docs/spec/wal.md:5` — "logging works for INSERT."** Understates: the
   transaction work logs `UPDATE` and `DELETE` as well
   (`UNDO_WRITE` → `HEAP_OVERWRITE`, and `HEAP_DELETE_MARK`), which
   `docs/spec/txn.md` and `include/kds/txn/undo_log.hpp` carry. The second half of
   the line — recovery is not implemented — is correct and is the important
   half, which is probably why the first half went unrevised.

**A premise correction.** This audit was commissioned on the statement that
`docs/workplan-index.md` claims `IX14`-`IX16` remain. It does not, and has not
since 2026-08-08: line 4 reads "Tasks `IX01`-`IX16` ... **All built**" and each
of the three carries its own `— **built**` heading. That document is `AGREES`
throughout. The drift this audit was called to measure was not there to find.

---

## Per document

### `docs/workplan-index.md` — secondary indexes

| claim | class | evidence |
|---|---|---|
| `IX01`-`IX16` all built (line 4) | AGREES | see rows below |
| IX01 key encoding | AGREES | `include/kds/exec/index_key.hpp`, `tests/index_key_test.cpp` |
| IX02 page class and split | AGREES | `include/kds/storage/index/index_page.hpp`, `index_tree.hpp`, `tests/index_tree_test.cpp` |
| IX03-IX05 catalog and DDL | AGREES | `include/kds/exec/index_ddl.hpp`, `tests/index_ddl_test.cpp` |
| IX06-IX09 write hook, WAL, backfill | AGREES | `include/kds/exec/index_maintain.hpp`, `tests/index_maintain_test.cpp`, `tests/insert_wal_test.cpp` |
| IX10-IX12 compiler, read path, equivalence | AGREES | `src/exec/step_vm.cpp:365` dispatches `kIndexProbe`/`kIndexRange` to `RunIndexStep`; `tests/index_compile_test.cpp`, `tests/index_contract_test.cpp` |
| IX13 `indexes` switch | AGREES | `src/exec/step_vm.cpp:613` takes the walk when `!indexes_` |
| IX14 benchmark | UNVERIFIABLE | `bench/results-index.md`, `tools/index_benchmark.py`, `tools/pg_index_benchmark.py` all exist; the *numbers* are a measurement CLA cannot reproduce |
| IX15 documentation sweep | AGREES | `docs/spec/index.md` header, `CLAUDE.md` Core Architecture entry |
| IX16 access statistics, "and it was free" | AGREES | `src/stats/access_stats.cpp:22` — one `catalog.RecordAccess(exec::StoredAccessKind(step.kind), ...)` with no per-kind branch; mapping at `src/exec/plan_printer.cpp:33` |
| "the whole suite is green at 1,699 tests" (line 15) | UNVERIFIABLE | a test run |

### `docs/spec/index.md` — spec

| claim | class | evidence |
|---|---|---|
| "built (`IX01`-`IX16`, every milestone)" (line 3) | AGREES | as above; header agrees with its workplan |

### `docs/spec/parser-v2.md` / `docs/inflight/in-progress/parser-v2-workplan.md` — query language

| claim | class | evidence |
|---|---|---|
| spec line 9: "Status of the work: **unbuilt**" | **STALE** | `src/exec/step_compiler.cpp`, `src/exec/step_vm.cpp`, `include/kds/exec/budget.hpp`, `tests/exec_chain_test.cpp`, `tests/exec_subquery_test.cpp` |
| spec line 9: "no join execution anywhere" | **STALE** | `tests/parser_join_test.cpp`, `tests/exec_chain_test.cpp` |
| spec I14 resolved by `aggregate.md` (line 178) | AGREES | `include/kds/exec/aggregate.hpp` |
| workplan `V01`-`V19` done | AGREES | per-task artifacts: `tests/parser_golden_test.cpp` (V01), `include/kds/storage/visit.hpp` (V03), `tests/parser_subquery_test.cpp` (V07), `include/kds/exec/chain_frame.hpp` (V16), `tests/exec_budget_test.cpp` (V19) |
| workplan V20 **not** marked done | AGREES | correct — `tests/exec_order_test.cpp`, the test V20 names, **does not exist** |
| workplan `V21`+ unbuilt | UNVERIFIABLE | V21/V22 name `src/stats/recorder.cpp`, which does not exist under that path; the trail work landed as `include/kds/stats/trail_recorder.hpp` instead, so the workplan's file names are stale even where its status is right |

### `docs/spec/waystone-concpets.md` / `docs/inflight/in-progress/waystone-workplan.md`

| claim | class | evidence |
|---|---|---|
| "recording and replay both work (P01-P13)" | AGREES | `include/kds/stats/trail_recorder.hpp`, `trail_store.hpp`, `include/kds/exec/trail_replay.hpp`, `tests/waystone_replay_test.cpp`, `tests/waystone_contract_test.cpp` |
| "not built: retention (P15), decay (P16), epoch bump sites (P17)" | AGREES | no retention or decay symbol found; the page epoch is absent engine-wide, which `cabin.md` and `index.md` both record independently |
| workplan carries **no status header at all** | UNVERIFIABLE | *a finding in itself*: per-task state must be read from `waystone-concpets.md` §status, not from the workplan a worker would open first |

### `docs/cabin-workplan.md` / `docs/spec/cabin.md`

| claim | class | evidence |
|---|---|---|
| "v1 is complete and measured", `CB01`-`CB11` | AGREES | `include/kds/stats/cabin_store.hpp`, `include/kds/exec/cabin_ddl.hpp`, `include/kds/exec/tuple_verify.hpp`, `tests/cabin_store_test.cpp`, `tests/cabin_contract_test.cpp`; `src/exec/step_vm.cpp:362` dispatches `kCabinProbe` |
| "Numbers in `bench/results-cabin.md`" | UNVERIFIABLE | file exists; the measurement does not reproduce here |
| `CABIN AUTO` named and stored, nothing consumes it | AGREES | consistent with `CLAUDE.md`'s open-decisions entry |

### `docs/txn-workplan.md` / `docs/spec/txn.md`

| claim | class | evidence |
|---|---|---|
| "built. `T01`-`T14` are done." | AGREES | `include/kds/txn/undo_page.hpp`, `undo_log.hpp`, `manager.hpp`, `read_view.hpp`, `visibility.hpp`, `trx_id.hpp`; `include/kds/server/session.hpp`; `tests/txn_manager_test.cpp`, `tests/txn_session_test.cpp`, `tests/visibility_test.cpp`, `tests/undo_page_test.cpp` |
| §8's gap: MVCC ships before recovery | AGREES | no recovery entry point exists; `docs/spec/wal.md` §12 says the same |
| §9: nothing purges, `SnapshotTooOld` unreachable | AGREES | no purge symbol found |

### `docs/spec/wal.md`

| claim | class | evidence |
|---|---|---|
| "logging works for INSERT" | **STALE** | UPDATE and DELETE are logged too — `include/kds/txn/undo_log.hpp`, `tests/insert_wal_test.cpp`, `tests/wal_payload_test.cpp` |
| "recovery is not implemented" | AGREES | nothing reads the log back; no replay entry point found |

### `docs/workplan-aggregate.md` / `docs/spec/aggregate.md`

| claim | class | evidence |
|---|---|---|
| "`AG01`–`AG10` are built (2026-08-06)" | AGREES | `include/kds/exec/aggregate.hpp`, `tests/aggregate_test.cpp`, `tests/aggregate_contract_test.cpp`, `tests/parser_aggregate_test.cpp` |
| "One task was added, `AG11`" | UNVERIFIABLE | the added task's own artifact was not resolved within this audit's search; `AVG` is described as built in `CLAUDE.md` and `aggregate.md` §3.4 |
| AG1: chain byte-identical with and without the fold | AGREES | `tests/aggregate_contract_test.cpp` is the suite named for it; the *assertion* is a test run |

### `docs/inflight/in-progress/workplan-aggregate-perf.md`

| claim | class | evidence |
|---|---|---|
| "AP01, AP02 (partly) and AP03 are built; AP04 and AP06 still open" | UNVERIFIABLE | every task in this plan is a *measurement*, and its own text says the numbers of AP04/AP05 are stale. Nothing here can be settled by reading code |
| "`./build` is Debug — use `build-release`" | UNVERIFIABLE | a build instruction, not a status claim |

### `docs/workplan-types.md` / `docs/spec/types.md`

| claim | class | evidence |
|---|---|---|
| "`TY01`–`TY09`, all built as of 2026-08-07" | AGREES | `include/kds/exec/type_literals.hpp`, `tests/type_literals_test.cpp`, `tests/types_contract_test.cpp`, `tests/types_e2e_test.cpp`, `tests/types_predicate_test.cpp` |
| "`TY10` and `TY11` built the same day" | AGREES | `include/kds/base/int128.hpp` is TY11's representation; `tests/lexer_test.cpp` covers TY10's numeric token |
| "`float` still refused" | AGREES | consistent across `types.md`, `protocol-wp.md:53` and `CLAUDE.md` |

### `docs/inflight/in-progress/workplan-crosscore.md` / `docs/spec/crosscore.md` / `docs/spec/sched.md`

| claim | class | evidence |
|---|---|---|
| P0 built (superblock `cores`, `owner_core`) | AGREES | `include/kds/server/superblock.hpp`, `include/kds/catalog/core_placement.hpp`, `tests/superblock_test.cpp` |
| P1 built (transport) — `sched.md:58` | AGREES | `include/kds/sched/spsc_ring.hpp`, `ring_transport.hpp`, `sim_ring_transport.hpp`, `ring_message.hpp`, `send_retry.hpp`; `tests/spsc_ring_test.cpp`, `tests/ring_transport_test.cpp`, `tests/sim_ring_transport_test.cpp`, `tests/send_retry_test.cpp` |
| P2 built (`CoreRuntime`, leases) | AGREES | `include/kds/server/core_runtime.hpp`, `include/kds/storage/extent_lease.hpp`, `include/kds/server/extent_lease_service.hpp`, `include/kds/sched/coro.hpp`; `tests/core_runtime_test.cpp`, `tests/extent_lease_test.cpp`, `tests/extent_lease_service_test.cpp`, `tests/coro_test.cpp` |
| "the restriction half is built; the pipeline is blocked" (line 240) | AGREES | `include/kds/server/core_affinity.hpp`, `tests/core_affinity_test.cpp`; no pipeline symbol found |
| "verified under ThreadSanitizer" | UNVERIFIABLE | a test run |
| the status is stated **inline at line 240**, not in a header | UNVERIFIABLE | a finding: a worker opening this file reads 240 lines before learning what is built |

### `docs/spec/protocol.md` / `docs/inflight/in-progress/protocol-wp.md`

| claim | class | evidence |
|---|---|---|
| "only the frame codec exists ... nothing calls it" | AGREES | `include/kds/wire/kwp.hpp`, `src/wire/frame_codec.cpp`, `tests/kwp_frame_test.cpp`; the server speaks the newline protocol |
| P09's row codec "predates it, built 2026-08-05" (line 54) | AGREES | `include/kds/wire/row_codec.hpp`, `tests/wire_row_codec_test.cpp` |
| P01 (client manual rewritten for KWP/1) implicitly outstanding | AGREES | `docs/spec/client-manual.md` still documents the newline protocol, as `protocol.md`'s own status says it should |
| workplan carries **no status header**; per-task state is absent except where a task's text was amended | UNVERIFIABLE | a finding: `P01`-`P17` here have no done markers at all |

### `docs/rules/keystoneid-invariant.md` / `docs/rules/keystoneid-k0-findings.md`

| claim | class | evidence |
|---|---|---|
| "K-M1 done 2026-08-03" | AGREES | `tests/keystone_id_test.cpp`, `docs/rules/keystoneid-k0-findings.md` |
| **"K-M2..K-M6 not started"** | **STALE** | K-M4 is built: `include/kds/catalog/keystone_budget.hpp` (`catalog::BudgetOf()`), `tests/keystone_budget_test.cpp`; the same file's line 271 already describes it shipping |
| "K1 does not hold across a crash today" | AGREES | `sys.tables.next_id` is unlogged and no recovery exists; consistent with `docs/spec/wal.md` |
| findings' measurement of allocator cost | UNVERIFIABLE | `bench/results-keystone-alloc.md` exists; the numbers are a measurement |

### `docs/spec/foreign-keys.md`

| claim | class | evidence |
|---|---|---|
| "FK-M1 through FK-M5 are built (2026-08-05)" | AGREES | `include/kds/exec/fk_check.hpp`, `include/kds/catalog/foreign_key.hpp`, `tests/foreign_key_test.cpp` |
| "FK-M6 is out of v1 by F2" | AGREES | a scope statement, not a build claim |
| its grounding note: txn, DELETE and Cabin all built | AGREES | as per those sections above |

### `docs/spec/heap-and-tuple.md` — authoritative spec

| claim | class | evidence |
|---|---|---|
| §7 "collection is built; no policy consumes it" (line 174) | AGREES | `include/kds/stats/access_stats.hpp`, `src/stats/access_stats.cpp`; no consumer found |
| §3.3-3.4 fixed-length tuple and var-heap in code (line 86) | AGREES | `include/kds/storage/tagged_cell.hpp`, `include/kds/storage/varheap.hpp`, `tests/fixed_length_tuple_test.cpp`, `tests/varheap_test.cpp` |
| the page epoch does not exist | AGREES | no epoch field found; three documents record waiting on it |

### `docs/inflight/in-progress/scenario2-freight.md`

| claim | class | evidence |
|---|---|---|
| "`S2-01` and `S2-02` are built" | UNVERIFIABLE | the artifacts are scenario drivers under `tools/`, not engine code; not resolved within this audit's scope |

### Superseded documents — status claims that are false but harmless

`docs/parser.md:9` ("Status: unbuilt"), `docs/parser-workplan.md:7` ("the
blueprint's items are built as stated") and `docs/step-chains.md:9` all carry
status claims that no longer describe the tree. **Not classified as STALE**:
each carries a superseded banner and `CLAUDE.md` names all three as traps a
worker must not build from. They are history and their status lines are part
of the history. Do not fix them; the banner is the fix.

---

## What this audit suggests, without doing it

1. **`docs/spec/parser-v2.md:9` is the one line worth changing today.** It is the
   status header of an authoritative-adjacent spec and it says the opposite of
   the truth.
2. **Two workplans have no status header** (`waystone-workplan.md`,
   `protocol-wp.md`) and one buries it 240 lines in
   (`workplan-crosscore.md`). Every workplan that *does* have one — index,
   txn, types, aggregate, cabin — was accurate. That correlation is the
   actionable finding: the header is what gets maintained.
3. **A workplan's file paths go stale before its status does.**
   `parser-v2-workplan.md` V21 names `src/stats/recorder.cpp`; the code landed
   as `include/kds/stats/trail_recorder.hpp`. The status was right and the
   path was wrong, which is the harder error to notice.

Nothing in this report settles any `[OPEN]` or `[PROPOSED]` item, and no
document was edited.
