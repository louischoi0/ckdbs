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
| WAL | Every data mutation logged (heap, undo, var-heap, index, assertions); **catalog/DDL writes are not**. **Recovery runs at mount 2026-08-12** (RC01-RC11): analysis/redo/high-water/undo per core before the listener binds, a completion checkpoint bounding the next crash, `SHOW META`'s recovery block, and assertion enforcement resumed. The catalog is still not recovered (RV3) | `docs/wal.md`, `docs/workplan-wal-recovery.md` |
| Transactions & MVCC | Built (T01-T14); no purge, and MVCC ships before recovery (see §8's gap) | `docs/txn.md`, `docs/txn-workplan.md` |
| Transactional DDL | **Built 2026-08-16** (DT1-DT7, v1 scope complete), reversing `docs/txn.md` §9's "out of scope" by direction; §7/§9 amended, and `docs/spec-drop-table.md` DT5 too. **`CREATE TABLE` is atomic, isolated and consistent** — a rollback leaves no relation, and an uncommitted one is invisible by every route (SELECT/INSERT/UPDATE/DELETE/DESCRIBE/SHOW TABLES), with one test walking all of them. **`CREATE INDEX` is atomic and isolated; `DROP INDEX` is atomic and isolated *on core 0* as of 2026-08-18** — it shipped as isolated, review disproved that (`InitTableAccess` reads the index list unfiltered, so a rolled-back drop left an index silently missing rows), it was refused inside a transaction for two days, and **DT9** fixed the read instead: an unfiltered catalog read counts a delete-mark only once its deleter is no longer in flight. Core-0-scoped because the predicate walks one core's live list. **`DROP TABLE` alone is atomic but not isolated** (§5a: its `sys.objects` *retype* is an in-place overwrite with no undo chain, so a filtered read skips the row outright and others see the relation gone before commit; DT9 does not reach it, and undo records are what would). Catalog rows take the real trx id, catalog reads filter by the reader's view, and rollback works by the existing compensation — no undo records. A view is minted **only while some transaction holds uncommitted DDL**, so the cache fast path is untouched. **Durability deferred by name** — catalog writes stay unlogged and unrecovered (RV3); `SHOW META` prints `ddl_durable=0` beside the claim. Open: the same-name refusal's *message*; undo records for catalog rows (the only thing that would isolate `DROP TABLE` — its exposure is the `sys.objects` in-place retype, which no delete-mark rule reaches); and a cross-core commit oracle before DT9's claim may drop the core-0 scope | `docs/spec-ddl-transactional.md`, `docs/workplan-ddl-transactional.md` |
| Query language, parser, step chains, joins, subqueries | Built (V01-V19; V09 pagination 2026-08-10). Open: V08's `IN (value list)`, V11 (`WITH (...)` table options), V12 (`SET DURABILITY`, the session/admin classes), V20's test, and phase V-6's blueprint parser | `docs/parser-v2.md`, `docs/parser-v2-workplan.md` |
| `ORDER BY` (the output sort) | **Built 2026-08-11** (OB1-OB7): any columns, pk or not, of any relation in a non-aggregated statement, each `ASC`/`DESC`, up to 8 keys. A sink decorator at the AG1 seam, `sort_max_rows`-capped, with a top-N heap under `LIMIT` and the pk-ascending form elided to zero cost. Refused and left open by decision: ordering over aggregated output | `docs/workplan-order-by.md` |
| Aggregation (GROUP BY, COUNT/SUM/MIN/MAX/AVG) | Built (AG01-AG10) | `docs/feat-aggregate.md`, `docs/workplan-aggregate.md` |
| Aggregate performance | AP01-AP03 built, AP05 next | `docs/workplan-aggregate-perf.md` (start at "Where to pick this up") |
| Types: DATE, TIMESTAMP, DECIMAL, DECIMAL128 | Built (TY01-TY11); `float` stays refused | `docs/spec-types.md`, `docs/workplan-types.md` |
| NULL storage and semantics | **Proposal only, nothing built** (2026-08-13). A tail null bitmap sized to *nullable* columns, so every relation today keeps a byte-identical `row_size`. Oracle's variable-length row is refused by name — it retracts invariant 13. Nullability default, index treatment and `ORDER BY` position are all `[OPEN]` | `docs/spec-null.md` |
| Waystone (pattern-keyed access trails) | Recording + replay built (P01-P13); retention/decay/epoch bumps not (P15-P17) | `docs/waystone-concpets.md`, `docs/waystone-workplan.md` |
| CREATE PATTERN | Built through spec §8 step 4 | `docs/spec-create-pattern-user-defined-patterns-v1.md` |
| Cabin (value-observed authoritative store) | v1 built (CB01-CB11); entry sets memory-resident | `docs/feat-cabin.md`, `docs/cabin-workplan.md` |
| Secondary indexes (multi-column, covering) | All built (IX01-IX16) | `docs/feat-index.md`, `docs/workplan-index.md` |
| Foreign keys | Declared and enforced (FK-M1..FK-M5); CASCADE/SET NULL out of v1 | `docs/impl-foreign-keys.md` |
| Assertions (group-level constraints) | **Complete and enforcing** (AST01-AST10). The recovery-side registry rebuild — outside the AST series — **landed 2026-08-12** as `docs/workplan-wal-recovery.md` RC07: `enforcing=1` immediately after a restart | `docs/feat-assertion.md`, `docs/workplan-assertion.md` |
| Access statistics | Built (`SHOW ACCESS`) | `docs/heap-and-tuple.md` §7 |
| ALTER TABLE | Built 2026-08-10 (AL1-AL9, ALT01-ALT05): catalog-only — RENAME TO / RENAME COLUMN; assertions RESTRICT; everything data-moving refused | `docs/spec-alter.md`, `docs/workplan-alter.md` |
| DROP TABLE | Built 2026-08-10 (DT1-DT6, DT01-DT05): catalog-scoped — oid tombstoned and never reissued, pages orphan (reclamation gated), fkeys/assertions RESTRICT | `docs/spec-drop-table.md`, `docs/workplan-drop-table.md` |
| Bulk insert | T1 built 2026-08-10 (BLK01-BLK05): multi-row VALUES, full pipeline per row, BI5 fingerprint rule; T2 gated on the KWP server | `docs/spec-bulkinsert.md`, `docs/workplan-bulk-insert.md` |
| Physical optimizer, Part I: relayout | Built and measured (PX01-PX08), **shadow-only as a finding** — every move blocked by a named §6 gate | `docs/feat-physical-optimizer.md`, `docs/workplan-physical-optimizer.md` |
| Physical optimizer, Part II: Cabin controller | **Complete** (PHY01-PHY08, closed 2026-08-10): controller end to end over the EVT03/EVT06 substrate, `SHOW CABIN_OPTIMIZER` observability, E2E lifecycle test, measured in `bench/results-cabin-optimizer.md` (zero-candidate overhead unmeasurable, 10.9× on the 10k improvement case); managed state is memory-resident — restart forgets, re-observation rebuilds | same docs, Part II / §II.1-§II.7 |
| Buffer-pool eviction | **Armed 2026-08-13**: the `PageRef` migration is built (MG01-MG06) — every accessor returns a pinned handle, the raw seam is `protected`, and the CLOCK sweep runs on the fault path under a `buffer_pool_frames` budget (default 0 = unbounded until sized). Proven by the full suite at an 8-frame budget with poisoned reclaims, plus an ASan sim pass. Background reserve trigger still gated on EVT02's bounded pool | `docs/workplan-pageref.md`, `docs/spec-eviction.md`, `docs/workplan-eviction.md`, `docs/page.md` §3 |
| Cross-core execution | P0-P2, P6, P5-shape row-id leasing and the P4 restriction half built by 2026-08-10; P4a-P4c the same day — the first cross-core statement (single-step star SELECT). **P4d complete 2026-08-15**: the executor's coroutine conversion (4d-1/2/3), the streaming producer parking at page boundaries (4a), chained opens and the consuming stage (4b-1/4b-2), the session side (4b-3) — so a **two-step join executes across cores**, planned by the session and rendered through a typed projected reply — and 4c's gated inner walk, which bounds a walked inner and admits joins on non-pk columns. **P4e complete 2026-08-15**: equivalence (pipeline reply byte-identical to local, twelve shapes, shipping asserted) and the benchmark — the isolation re-run **cannot run** and says why (a peer-owned relation has no writer: CC3 refuses cross-core writes, DML shipping is unbuilt, core 0 alone listens), so the pipeline is priced in process instead at **2.52 µs + 0.626 µs per forwarded row**, which is 1.5× the local per-row cost of the same join and settles 4c's open question in favour of building the per-batch runner handle. Also closed: a stage used to read through in-flight writers; it now mints the owning core's committed view per CC4. **Remaining**: 4c's per-page batching (now justified), the sub-chain await decision, and a writer for peer-owned relations before any *scaling* claim is possible | `docs/crosscore.md`, `docs/workplan-crosscore.md`, `docs/sched.md`, `bench/results-crosscore-pipeline.md` |
| Task representation | Decided and built: C++20 stackless coroutines | `docs/sched.md` §3 |
| Wire protocol KWP/1 | Frame codec only; the server speaks the newline text protocol. **Direct TLS, SCRAM-SHA-256 auth, and statement-class authorization all built 2026-08-13**: TLS 1.3 at the transport seam; connection auth behind an `AUTH` gate (`auth = scram`, `users_file`, `--add-user`); three ranked roles (readonly/readwrite/admin) enforced per statement at the dispatcher. All off by default; per-relation grants stay future (catalog-recovery-gated) | `docs/protocol.md`, `docs/protocol-wp.md`, `docs/client-manual.md` |
| Keystone id issue-once contract | K-M1, K-M3, K-M4 built (K-M3 2026-08-10: `exec::CompileAssignments` refuses a pk UPDATE at compile with `Unsupported` and a byte); K1 does not hold across a crash — read the findings before quoting the invariant | `docs/keystoneid-invariant.md`, `docs/keystoneid-k0-findings.md` |
| Key mode (`EXPLICIT` pk) | **Built 2026-08-11** (PK01-PK07): the caller may supply a pk and it need not ascend; uniqueness is proved by the btree descent, so the mode is `BTREE`-only and a full leaf now divides. The pk stays non-updatable. **Complete** — PK09 (dividing a full internal node) landed the same day, so no part of the feature is refused for being unbuilt | `docs/heap-and-tuple.md` §4.1, `docs/workplan-key-mode.md` |
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
11. **Every** relation's pk is a unique 40-bit id, carried only by the Keystone word, never rebound, **never updatable**. First column must be integer-typed. **Amended and built 2026-08-11** (`docs/heap-and-tuple.md` §4.1): the mode is fixed at `CREATE TABLE` — `ASSIGNED` (default) issues from `sys.tables.next_id` and ids ascend; `EXPLICIT` takes the caller's id, **which need not ascend**, proves uniqueness by btree descent, and must therefore be `BTREE`-clustered. Heap chains stay `ASSIGNED`-only. Read §4.1 before relying on ordering — monotonicity is now per-relation, never engine-wide.
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
- **Never push what you have not built.** `main` has twice received commits
  that were never compiled. `scripts/githooks/pre-push` is the gate; enable
  it once per clone with `git config core.hooksPath scripts/githooks`, and
  bypass it with `git push --no-verify` only when you can say why.
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

## Session Workflow — the loop every task runs

Four steps. The first three run unprompted; the fourth stops.

**1. Work in a worktree, named for the work.** Before touching the tree,
`EnterWorktree` with a name taken from the task, not from the date or a
counter — `rc04-high-water`, `fix-varheap-checksum`, `bench-agg-p99`. A
name that says what is being attempted is what makes a stale worktree
recognizable a week later. Read-only questions and pure doc lookups need
no worktree.

**2. Every completed step gets a `critics-developer` review.** Per step,
not once at the end: the point is to catch a wrong contract before the
next step is built on it. Correctness first, then duplication,
over-engineering, bloat. Apply what it finds and sharpen where the code
gets leaner for it; say plainly which findings were rejected and why —
a review whose findings are all silently accepted was not read.

**3. Every feature gets a `ck-tester` run, and the question is
overhead.** Not only "is the suite green": does the change cost
measurable per-statement or system-level overhead? `build-release`,
interleaved A/B, per the measurement rule above. A regression is a
finding to report with its number, never a line to bury. **If the
environment cannot build or run, say so in the report** — an unrun
measurement is never reported as a pass, and a suite that was not
executed is stated as "not executed", not implied green.

**4. Land — sync unprompted, push only on the word.** Sync and resolve on
the work branch, never on `main`:

```
git fetch origin
git merge origin/main        # on the work branch; resolve conflicts here
```

Then **stop** and report: what the review found, what ck-tester measured,
what the merge resolved. On the go-ahead, and not before:

```
git checkout main
git merge --no-ff <branch>
git push origin main
```

A failed review, a measured regression, or an unrun test suite stops the
chain before the merge. Say which, and do not offer the push as though
the gate had passed.

## Every claim carries its worktree and commit — in the middle of the text

Not a header, not a footer, not a preamble: **the worktree name and the
short commit id go inside the sentence that makes the claim.** Reports
outlive the tree they were written against, and a finding quoted a week
later must still say what it was true of:

> On `rc04-high-water` at `f837e6a`, redo's `CreateAt` already covers the
> PAGE_INIT case, so the gap is the checkpoint's recLSN-of-zero entry.

Three consequences, because this is a rule about honesty and not about
formatting:

- **When a step moves the commit, the next claim carries the new id.** One
  reply may name three, and should.
- **With no worktree in use, say so by name** — *"in the main checkout on
  `feat-wal-recovery` at `f837e6a`"*. Silence reads as "a worktree", which
  the Session Workflow above requires and which would then be a false
  report of compliance.
- **A measurement names the commit it was measured at**, which
  `bench/results-*.md` already requires of a results file; this extends the
  same discipline to the reply that quotes it.

## Open Decisions — DO NOT assume or silently pick

The full statements, options and constraints live in the owning docs; this
is the index. When work touches one: stop and ask, or implement behind an
interface that keeps every listed option viable.

- **Storage** (`docs/heap-and-tuple.md` §8, `docs/page.md`,
  `docs/rule-fixed-length-tuple.md`): heap page split policy;
  `inline_cell_width` default; spilled-value size cap; prefix-inlining
  trigger; purge cadence; the 16 reserved Keystone bits; id reuse; I/O
  backend; whether invariant 3 is ever relaxed; whether a heap relation may
  ever be `EXPLICIT` (that is the split policy, not a pk rule).
- **Waystone** (`docs/waystone-concpets.md` §9): per-pattern retention and
  eviction; persistence class of trail pages; `arg_hash` collisions; decay
  and sampling; whether invariant 9 is ever amended.
- **Transactions & WAL** (`docs/txn.md` §9, `docs/wal.md` §15): undo
  retention and `SnapshotTooOld`; trx-id wraparound; `kTrxIdBlockSize`;
  cross-core commit and recovery under a changed core count.
- **Protocol** (`docs/protocol.md`): frame/batch/timeout sizing;
  compression; flow control. (TLS §1, SCRAM §14 and statement-class
  authorization §14 all landed 2026-08-13; what remains open there is
  per-relation grants, gated on catalog recovery.)
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
