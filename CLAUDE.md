# CLAUDE.md — KDS Storage Engine

Working guide for AI development agents. **This file is a guideline and a
milestone map, not a detail store**: every subsystem's decisions live in
the `docs/` file that owns them, and this file links there.
`docs/spec/heap-and-tuple.md` is the authoritative design spec; when this
file and a spec conflict, the spec wins — flag the conflict instead of
guessing.

**Compacted 2026-09-02 ahead of the big-bang change to the architecture,
rules and constraints.** This file and every document under `docs/` now
state the current contract only: what is built and how it behaves. The
history of how each rule was reached, every measurement, and every plan
for unbuilt work left the tree with `bench/`, `instructions/` and
`docs/inflight/`; the last commit holding all of it is `1769487`, and the
last commit holding the uncompacted docs is `7f0193b`.

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

One line per subsystem: what is built and enforcing, then the owning docs.
Read the owning spec before touching a subsystem — each carries rules that
are correctness statements, not style. What a subsystem does *not* do is
stated in its spec as a fact, and nowhere here as a plan.

| Subsystem | Built | Owning docs |
|---|---|---|
| Pages, semi-sorted heap, Keystone, fixed-length tuples, var-heap | Built | `docs/spec/heap-and-tuple.md` (authoritative), `docs/rules/rule-fixed-length-tuple.md`, `docs/spec/page.md` |
| Heap relations | **Suspended (SUS-1, 2026-09-05), not removed.** No new one is created — `CREATE TABLE … HEAP` is refused `Unsupported` and the storage default is `BTREE`; every existing heap relation mounts and serves, and no heap code or cell is deleted | `instructions/v3.0.0/workorder-as-sus1-heap-suspended.md`, `docs/spec/heap-and-tuple.md` §3.1b |
| Multi-page free map | Region-based, ceiling 2^31 pages / 16 TiB, no superblock bump. The map is unlogged; recovery repairs it | `docs/spec/page.md` §5 |
| Clustered B+ tree | Built. An append-split publishes the separator before the sibling link | `docs/spec/heap-and-tuple.md` §5 |
| WAL | **One stream for the instance** (AR0 M0): core 0 owns the log, peers attach and append under its latch, and the superblock's `log_topology` says which topology a volume was written with — a pre-M0 volume still mounts per-core. Every data mutation logged (heap, undo, var-heap, index, assertions, catalog/DDL) except ALLOC/FREE and the advisory Waystone classes invariant 8 exempts. Recovery runs at mount, once per log — analysis/redo/high-water/undo, a completion checkpoint, `SHOW META`'s recovery block; a torn catalog page refuses the mount | `docs/spec/wal.md` |
| Transactions & MVCC | Built. Reader leases bound a purge (`ReadHorizon()`); undo pages recycle into the log's next growth. `SnapshotTooOld` is never raised; a previous run's undo pages are not reclaimed at mount | `docs/spec/txn.md` |
| Transactional DDL | Built and durable: catalog writes logged, DDL under a real transaction, losers rolled back at mount. `CREATE TABLE` is atomic, isolated and consistent; `CREATE INDEX`/`DROP INDEX` atomic and isolated (`DROP` on core 0 only); **`DROP TABLE` is atomic but not isolated** (§5a) | `docs/spec/ddl-transactional.md` |
| Namespaces | A namespace **selects the core** that owns the relations created in it, fixed by its first relation and never rebalanced; `PlacementPolicy::kNamespace` is the default. `CREATE NAMESPACE` / `DROP NAMESPACE` (RESTRICT), `SHOW NAMESPACES`, `ns.table` everywhere a relation is named. **A qualifier declares placement, never identity** — relation names are instance-global | `docs/spec/namespace.md` (NS10) |
| Query language, parser, step chains, joins, subqueries | Built. `IN (value list)`, `WITH (...)` table options and `SET DURABILITY` are refused `NotImplemented` | `docs/spec/parser-v2.md` |
| `ORDER BY` | Any columns in a non-aggregated statement, each `ASC`/`DESC`, up to 8 keys, `sort_max_rows`-capped, a top-N heap under `LIMIT`. Refused over aggregated output | `docs/spec/parser-v2.md` |
| Aggregation (GROUP BY, COUNT/SUM/MIN/MAX/AVG) | Built | `docs/spec/aggregate.md` |
| Types | DATE, TIMESTAMP, DECIMAL, DECIMAL128, `char(N)`, `varchar(N)`; `float` refused. `varchar(N)`'s N is that column's `kds.inline_cell_width` — a width in `[16, 4096]`, not a length cap; a longer value spills. A bare `varchar` reads as the instance width | `docs/spec/types.md`, `docs/rules/rule-fixed-length-tuple.md` §4 |
| NULL | A tail bitmap sized to nullable columns; an all-`NOT NULL` relation keeps a byte-identical layout. **NOT NULL is the default and `NULL` is opt-in**; nullable index keys refused; NULLs sort largest; the bitmap is sole authority; WHERE is three-valued | `docs/spec/null.md` |
| Waystone | Recording and replay. Trails are never retired or decayed | `docs/spec/waystone-concpets.md` |
| CREATE PATTERN | Withdrawn and removed; `$name` parameters refused everywhere; `SysPatternRow`'s `origin`/`flags` stay on disk | `docs/spec/create-pattern-user-defined-patterns-v1.md` (withdrawn) |
| Cabin | Built, a store per core: the relation's owner observes, appends and serves. A set is banked only from a read view nothing can contradict (§6a). Entry sets are memory-resident; a peer-owned relation earns no `CABIN AUTO` | `docs/spec/cabin.md` |
| Secondary indexes (multi-column, covering) | Built. No index-only scan, no `UNIQUE` | `docs/spec/index.md` |
| Statement-local inner build | The walked-join shape takes a `BuildKey` annotation and buckets rows into a statement-scoped map; `join_build_max_rows` is the cap, `0` the off-switch; `ANALYZE` reports `inner_built=`/`build_rows=`/`build_probes=`. A built join owns no `sys.patterns` row and feeds no Waystone trail | `docs/spec/join-inner-build.md` |
| Foreign keys | Declared and enforced, cross-owner in both directions: the forward check probes the parent's owner from the dispatch fork, the reverse fans out over as many probe rounds as the pk set needs. CASCADE/SET NULL refused | `docs/spec/foreign-keys.md` |
| Assertions | Enforcing on every core, including immediately after a restart: the Bound Cabin is built and held by the relation's owner, and a core that knows of an assertion it cannot enforce refuses the relation's writes | `docs/spec/assertion.md` §6.1 |
| Access statistics | `SHOW ACCESS` | `docs/spec/heap-and-tuple.md` §7 |
| ALTER TABLE | Catalog-only: RENAME TO / RENAME COLUMN; assertions RESTRICT; everything data-moving refused | `docs/spec/alter.md` |
| DROP TABLE | Catalog-scoped: oid tombstoned and never reissued, pages orphan; fkeys/assertions RESTRICT | `docs/spec/drop-table.md` |
| Bulk insert | Multi-row VALUES, the full pipeline per row. **The sorted-fill fast path is heap-gated, so SUS-1 makes it unreachable for every new relation** — `bulkinsert.md` T3-2 applies only to relations created before the suspension | `docs/spec/bulkinsert.md`, `instructions/v3.0.0/workorder-as-sus1-heap-suspended.md` |
| Physical optimizer, Part I: relayout | Built, shadow-only: every move is blocked by a named §6 gate, which `SHOW RELAYOUT` reports. **The planner builds plans only for heap relations, so SUS-1 leaves it dark on a fresh volume** — `SHOW RELAYOUT` can propose nothing for a relation created since the suspension | `docs/spec/physical-optimizer.md`, `instructions/v3.0.0/workorder-as-sus1-heap-suspended.md` |
| Physical optimizer, Part II: Cabin controller | Built, with `SHOW CABIN_OPTIMIZER`; managed state is memory-resident | same, Part II |
| Buffer-pool eviction | Every accessor returns a pinned `PageRef`; the CLOCK sweep runs on the fault path under `buffer_pool_frames` (default 0 = unbounded). **One pool for the instance since AM-S2 step 3** — a peer borrows core 0's frame table rather than opening its own, so `buffer_pool_frames` is an undivided total and a page faulted on one core is served from the frame another loaded; conditional on the volume having one WAL stream, since the writeback gate is a property of the log | `docs/spec/eviction.md`, `docs/spec/page.md` §3, `instructions/v3.0.0/workorder-am-m1-shared-pool.md` |
| Cross-core execution | A two-step join executes across cores, replies byte-identical to local execution. Statement shipping carries an autocommit, single-relation statement to its owner; a lost answer is `UNKNOWN_OUTCOME`. Cross-owner transactions: writes and reads cross, a read enrols, RR gets a consistent-per-core snapshot, RC no cross-core promise; the coordinator's decision record is the durability point and a participant acks a decide at its COMMIT append. Under one stream an undecided prepare resolves in the mount's own scan — absence of a decision is abort. A typed client's shipped read is answered in rows on an answer edge (§4a); the text arm keeps its 992-byte cap | `docs/spec/cross-owner-txn.md`, `docs/spec/crosscore.md`, `docs/spec/sched.md` |
| Task representation | C++20 stackless coroutines | `docs/spec/sched.md` §3 |
| Wire protocol KWP/1 | The default port speaks length-prefixed binary frames: handshake with capability intersection, SCRAM, PARSE/BIND/EXECUTE over statement and portal handles, typed row batches, portal suspension, transaction and durability frames, a pinned error registry. `kMaxFrame` 16 MiB, 64 KiB batch target, 60 s portal-idle timeout, 64 statements and 64 portals per session; `max_rows` bounds delivery, not execution; `C_DESCRIBE` of an unexecuted statement is refused. The newline protocol is `debug_text_port`'s loopback debug surface, off unless configured; `STOP` is reachable only there. TLS 1.3 and the statement-class roles are off by default; the `COMPRESSION` bit is reserved and never offered | `docs/spec/protocol.md`, `docs/spec/client-manual.md` |
| Keystone id issue-once contract | A pk `UPDATE` is refused at compile; `sys.tables.next_id`'s bump logs and replays with every catalog write | `docs/rules/keystoneid-invariant.md`, `docs/rules/keystoneid-k0-findings.md` |
| Caller-supplied pk | Per row, an `INSERT` names the pk or the engine issues one; `sys.tables.next_id` is a high-water mark on what has been placed. A named key below the mark is btree-only and refused `OutOfRange` on a heap relation. There is no key mode; `KeyOrder` is an observation | `docs/spec/heap-and-tuple.md` §4.1 |
| Simulation harness | A whole instance on crashable in-memory devices driven through `CommandDispatcher`, crashed at a seed-chosen op, restarted and reconciled against an oracle; `scripts/sim.sh` runs the corpus | `sim/`, `scripts/sim.sh` |
| Range-granular core ownership | Directory, producer, id-based write routing and the read surface from every core are built. **Spreading ships off** (`Expeditor::Config` `kRangeSizeOff`) and there is no per-relation switch; with it off the pk is an identity and a sequence. A multi-owner relation refuses a write naming no pk, a join, `LIMIT`/`OFFSET`, any sort but `<pk> ASC`, any read inside an explicit transaction, and `CREATE INDEX`/`CABIN`/`ASSERTION`/FK once it has two or more ranges. Nothing merges or moves ranges. **Range routing keys on a heap relation omitting its pk, so SUS-1 means no relation created since the suspension can be range-split at all** | `docs/spec/crosscore.md`, `docs/spec/sched.md`, `instructions/v3.0.0/workorder-as-sus1-heap-suspended.md` |
| Stride forest | Rejected | `docs/spec/crosscore.md` §6b |
| Observability | Not a subsystem. Three `SHOW META` blocks: per-scheduling-group accounting against reactor wall time, the cross-core write refusal counters, the idle policy's counters | `docs/spec/sched.md` §4, `docs/spec/crosscore.md` §6 |
| User manual | `manual/` — SQL surface, verified against code | `manual/sql/sql.md` |

## How `docs/` is organized

**Three trees left on 2026-09-02, ahead of the big-bang change: `bench/`,
`instructions/` and `docs/inflight/`.** The last commit holding them is
`1769487`; `git show 1769487:<path>` retrieves any file, and each directory
keeps a `README.md` saying what was there and why it went. **A citation to
one of those paths — in this file, a spec, the manual, a test or a source
comment — resolves against that commit *unless the path exists in the
working tree*.** All three have since reopened, so the qualifier is
load-bearing: `docs/inflight/in-progress/` is not a reopened bucket and a
source comment still cites it. `instructions/v3.0.0/` reopened
on 2026-09-02 with the change's own instructions — AR0 and one work order
per milestone (`instructions/v3.0.0/index.md`). **`bench/` reopened on
2026-09-03** with AL-R8 ratified: `bench/v3.0.0/` is a fresh series and
carries no delta against any v2.x number, and `bench/README.md` states the
five rules a run is invalid without. **`docs/inflight/` reopened on
2026-09-03 on the operator's word**, with a narrower job than it had —
`instructions/` took the plans, so this holds only what a plan is not.
Three buckets under `docs/`, one rule each:

- **`docs/spec/`** — what is confirmed and implemented. The authoritative
  specifications; when this file and a spec conflict, the spec wins.
- **`docs/rules/`** — concepts and constraints that hold across the whole
  codebase: `rules.md`, the fixed-length-tuple rule, the Keystone id
  invariant and its findings.
- **`docs/inflight/`** — `known-gaps.md` (what is missing, what the code
  does differently from a spec, what the suite does not cover), `bugs/`
  (a defect found and **not yet fixed** — one fixed in the session that
  found it gets none) and `blocked/`. Every entry names the commit it was
  verified at; an entry older than its subsystem's last change is a
  statement about an engine that no longer exists. Open work orders are
  **not** here — they are `instructions/<version>/`.

A closed workplan is deleted, not archived: the spec that owns the
subsystem carries everything durable. The plans closed before 2026-08-26
are at `925f483` (`git show 925f483:docs/workplan-index.md` lists them);
everything open after that was under `docs/inflight/` at `1769487`. A
citation to a `docs/workplan-*.md`, or to the superseded parser documents
`docs/spec/parser-v2.md` replaced, points at one of those two commits.

Numbering collides across docs (three `P`-schemes, two `R1`s): **cite the
file, never the bare number.**

## Hard Invariants — never violate, never "temporarily" bypass

Numbered to match `docs/spec/heap-and-tuple.md` §8.

1. 8192-byte pages; `uint32_t` page ids; `0xFFFFFFFF` reserved as invalid.
2. `min_key` is immutable after page creation.
3. No tuple with `id < min_key(page)` in that page, ever — including by relayout.
4. Tuples within a heap page are unordered.
5. The Keystone column is exactly `id:40 | flags:8 | reserved:16`.
6. The Keystone word is read/written as an atomic `uint64_t` (CAS for updates); on-disk encodings use **explicit shift/mask helpers only — compiler bitfields are forbidden** for any persisted format.
7. Ids stored outside the tuple header are zero-extended `uint64_t`; the upper 24 bits are always 0.
8. Waystone is advisory: deleting it wholesale may cost performance but must never change a query result.
9. Waystone is never **authoritative**: a reader may consult it for *where to look* only, with the miss/Keystone/MVCC/fall-through discipline `docs/spec/heap-and-tuple.md` §8 spells out. It chooses where to look, never what is visible.
10. No single canonical in-memory tuple; consistency comes from page pin and latch discipline.
11. **Every** relation's pk is a unique 40-bit id, carried only by the Keystone word, never rebound, **never updatable**. First column must be integer-typed. Where the id comes from is a per-**row** fact — the `INSERT` names it or omits it — and there is no key mode. `sys.tables.next_id` is a high-water mark on what has been placed; uniqueness follows from it with no page read for an omitted key and for a named key at or above it. A named key **below** the mark is btree-only, proved by the descent, and refused `OutOfRange` on a heap relation. Monotonicity is per-relation and per-history, never engine-wide, and `sys.tables.key_order` is the only truthful reading of it (`docs/spec/heap-and-tuple.md` §4.1). With insert spreading off — every relation, since there is no per-relation switch — the pk is an identity **and a sequence**, monotonic in issue order; with it on (§4.1a) each core issues from its own leased block into its own chain and the pk is an identity and nothing more. Uniqueness and `key_order` hold in both states, and invariant 3 holds per range.
12. The tuple MVCC header is exactly `trx_id:48 | undo_ptr | data_len | flags` = 20 bytes. There is no `xmax`.
13. **Every tuple is fixed-length**: row size is a schema constant, variable-width values occupy one tagged cell of `kds.inline_cell_width` bytes — the instance's, or the column's own where `varchar(N)` declared one (the width is per *column*, never per row). A disagreeing length is `Corruption`, never interpreted.
14. **Var-heap values are immutable per version** and `kVarHeap` pages are never relocated. Authoritative data — advisory rules do not apply to it.

## Working Rules

- Fresh codebase: idiomatic modern C++, not kernel-style C. RAII for every
  resource; no raw `new`/`delete` in engine logic; fixed-width integer types
  and `static_assert`ed layouts for on-disk structs; named `constexpr` for
  every size/offset with the derivation in a comment. Full rules:
  `docs/rules/rules.md`.
- Document the lock/atomic protocol at the top of each subsystem file.
- Tests accompany every subsystem; the contract suites that compare
  configurations byte-for-byte are **waystone, index, cabin and inner-build**
  — keep them green and extend them with the feature. The other contract
  suites are a different shape, pinning claims rather than configurations:
  types and aggregate one test per numbered spec item, scram and tls against
  their RFC vectors. **Assertion has no contract suite** —
  `tests/assertion_*_test.cpp` are what carry its checks.
- **Never push what you have not built — unless the operator says to.**
  `scripts/githooks/pre-push` is the gate; enable it once per clone with
  `git config core.hooksPath scripts/githooks`. On CLA's own initiative the
  gate runs, and `--no-verify` needs a reason CLA can state. When the
  operator asks for a push with the tests skipped, or for a force push, CLA
  does it — no re-litigating, no waiting for a suite the operator has
  waived — and still owes two things: **say plainly in the reply what was
  skipped** (the hook, the suite, or both) and **never report an unrun
  suite as a pass**. The Session Workflow's step 4 gate below is a gate on
  CLA proceeding unasked, never on the operator.
- **Measure in `build-release`, never `./build` (Debug)** — Debug has
  reported the wrong sign. Per-statement fixed costs: server CPU,
  interleaved A/B. **Re-measure a premise before building the fix.**
- Every refusal carries the byte position of the offending token, and
  "understood and declined" is **two** codes: `Unsupported` is what the
  architecture cannot admit, `NotImplemented` is what nobody built, and
  each reaches a client as its own token. `InvalidArgument` stays "simply
  wrong". The test between the pair is in `include/kds/base/status.hpp`
  and `docs/spec/protocol.md` §11. Truthfulness beats convenience: never
  accept a spelling and enforce something other than what was written.
- Nothing new is reserved lightly: keywords hash as identifiers, and
  `kFingerprintVersion` moves only per `fingerprint.hpp`'s bump rule (the
  golden corpus pins it).
- Never add a second name for a quantity an existing setting expresses;
  re-scope the existing one.

## Version Management

The version of record is an **annotated git tag on `main`**. **The current
version of record is `v2.7.0` at `d840a30`**; read its tag message.

- **The operator names the version; CLA may move only `z`.** A version is
  set by the operator saying it — *"it is version x.y.z"* — and nothing
  else. No number is ever inferred from a merged milestone, a green suite,
  or the size of a diff. A tag CLA creates on its own initiative differs
  from the last operator-named version in `z` alone, and CLA says plainly
  in the reply that it moved it. Pushing any tag waits for the word.
- **A tag message is a durability claim, so it carries what bounds it**:
  what is built and enforcing, then the gaps that limit it. A tag carrying
  only the first half overstates the engine.
- **Every measurement names its version, and `git describe --tags` is
  how** — `v2.0.0-37-gaa3e26c`. Binding on every results file and on any
  reply that quotes a number. A results file lives at
  `bench/<version>/<benchmark>-<describe>.md`; `bench/README.md` carries
  the rules, and a v3 number is never set beside a v2.x one.
- **Nothing is back-filled.** A result measured before a version existed
  keeps its bare commit id.

## Session Workflow — the loop every task runs

Four steps. The first three run unprompted; the fourth stops.

**1. Work in a worktree, named for the work.** Before touching the tree,
`EnterWorktree` with a name taken from the task, not from the date or a
counter — `rc04-high-water`, `fix-varheap-checksum`. Read-only questions
and pure doc lookups need no worktree.

**2. Every completed step gets a `critics-developer` review.** Per step,
not once at the end. Correctness first, then duplication,
over-engineering, bloat. Apply what it finds; say plainly which findings
were rejected and why — a review whose findings are all silently accepted
was not read.

**3. Every feature runs the full test suite; the overhead measurement is
suspended.** The suite gates every step. The interleaved A/B overhead
measurement (`ck-tester`, `build-release`) is suspended by operator
decision, so a landed change carries "overhead not measured" as a stated
fact, never an implied pass. **If the environment cannot build or run,
say so in the report** — an unrun measurement is never reported as a
pass, and a suite that was not executed is stated as "not executed".

**4. Land — sync unprompted, push only on the word.** Sync and resolve on
the work branch, never on `main`:

```
git fetch origin
git merge origin/main        # on the work branch; resolve conflicts here
```

Then **stop** and report: what the review found, what the suite showed,
what the merge resolved. On the go-ahead, and not before:

```
git checkout main
git merge --no-ff <branch>
git push origin main
```

A failed review, a measured regression, or an unrun test suite stops the
chain before the merge. Say which, and do not offer the push as though
the gate had passed. This stops CLA, not the operator: an operator who
asks for the push anyway gets it, with what was skipped named in the reply
and never reported as a pass.

**Workflow mode** is an outer loop — `intermediary-agent` pulling tasks
from `cws` and `reporter-agent` syncing outcomes back — that wraps this
four-step process unmodified; it never relaxes a gate, and it activates
only on an explicit request. `docs/rules/rule-workflow-mode.md` is the
full rule.

## Every claim carries its worktree and commit — in the middle of the text

Not a header, not a footer, not a preamble: **the worktree name and the
short commit id go inside the sentence that makes the claim.** Reports
outlive the tree they were written against, and a finding quoted a week
later must still say what it was true of:

> On `rc04-high-water` at `f837e6a`, redo's `CreateAt` already covers the
> PAGE_INIT case, so the gap is the checkpoint's recLSN-of-zero entry.

- **When a step moves the commit, the next claim carries the new id.** One
  reply may name three, and should.
- **With no worktree in use, say so by name** — *"in the main checkout on
  `feat-wal-recovery` at `f837e6a`"*. Silence reads as "a worktree", which
  the Session Workflow requires and which would then be a false report of
  compliance.
- **A measurement names the version and the commit it was measured at** —
  `git describe --tags`, so `v2.0.0-37-gaa3e26c` rather than `aa3e26c`
  alone.

## Open Decisions — DO NOT assume or silently pick

None are recorded here or in the specs: the lists that stood here and in
each spec's open-decisions section were plans against the engine the
big-bang change replaces, and they went with `docs/inflight/` (at
`1769487`, and in this file at `7f0193b`). **The open list for v3 is
AR0's D1–D16**, in `instructions/v3.0.0/ar0-architecture-revision.md` §5,
each with CLA's proposal beside it and each unratified but D15 — and three
of them (D7, D8, D9) are marked `[quiet-wrong]` by AR0 itself, meaning a
wrong choice converts a refusal into a wrong answer. `docs/inflight/known-gaps.md`
tracks what is *missing*, which is a different list and does not decide
anything. The rule stands whatever the lists say: **when work touches a
matter the owning spec does not decide, stop
and ask, or implement behind an interface that keeps every option
viable.** Never infer a decision from a green suite, a merged diff, or
the absence of a rule. The C++ standard, toolchain, build system and test
framework are proposed, never decided, by CLA.

## Maintenance rule for this file

When a decision lands: update the spec that owns it and flip the
milestone row — **do not** write the detail into this file. A finding
with no owning doc gets one (or a section in the nearest spec), never a
paragraph here. This file states what is; a plan for what will be lives
in the operator's instructions, and a plan's history lives in git.
