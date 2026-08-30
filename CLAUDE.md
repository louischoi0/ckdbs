# CLAUDE.md — KDS Storage Engine

Working guide for AI development agents. **This file is a guideline and a
milestone map, not a detail store**: every subsystem's decisions, task
breakdowns, amendments and measurements live in the `docs/` file that owns
them, and this file links there. `docs/spec/heap-and-tuple.md` is the
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
| Pages, semi-sorted heap, Keystone, fixed-length tuples, var-heap | Built | `docs/spec/heap-and-tuple.md` (authoritative), `docs/rules/rule-fixed-length-tuple.md`, `docs/spec/page.md` |
| Multi-page free map (`page.md` §5) | **Complete 2026-08-26** (FM1-FM11): the instance ceiling was one bitmap page (65,280 ids, 510 MiB), now the 2^31-page / 16 TiB design ceiling — region-based, no superblock version bump, an existing file mounts unchanged. Open: **D9** (the map stays unlogged; RC04 repairs) and **D6** (`page.md` §4/§5 claims a superblock free-map root that `superblock.hpp` has never had); owed: the `sim/` run over a two-region device | `docs/inflight/in-progress/workplan-multi-free-map.md`, `docs/spec/page.md` §5 |
| Clustered B+ tree | Built | `docs/spec/heap-and-tuple.md` |
| WAL | Every data mutation logged (heap, undo, var-heap, index, assertions, catalog/DDL); still outside: ALLOC/FREE and the advisory Waystone classes invariant 8 exempts (`wal.md` §11a). Recovery runs at mount: analysis/redo/high-water/undo per core, a completion checkpoint, `SHOW META`'s recovery block; the catalog recovers too, and a torn catalog page refuses the mount rather than being served corrupt | `docs/spec/wal.md` |
| Transactions & MVCC | Built (T01-T14): reader registration (every snapshot that can outlive a park carries a `ReaderLease`, `ReadHorizon()` bounds a purge) and the undo purge (a settled page recycles into the log's next growth, `SHOW META` prints `undo_pages_live`/`undo_pages_recycled`) are both in. `SnapshotTooOld` stays unreachable by decision; prior-run pages leak until UP4. MVCC ships before recovery (see §8's gap) | `docs/spec/txn.md`, `docs/inflight/in-progress/workplan-undo-purge.md` |
| Transactional DDL | **Built 2026-08-16** (DT1-DT7, v1 scope complete). **`CREATE TABLE` is atomic, isolated and consistent** — an uncommitted or rolled-back one is invisible by every route. **`CREATE INDEX` is atomic and isolated; `DROP INDEX` is atomic and isolated *on core 0*** (core-0-scoped because the predicate walks one core's live list). **`DROP TABLE` alone is atomic but not isolated** (§5a: its `sys.objects` retype is an in-place overwrite with no undo chain, so others see the relation gone before commit). Catalog rows take the real trx id and roll back by the existing compensation, no undo records. **Durability landed 2026-08-19** (RV3): catalog writes logged, DDL under a real transaction, losers rolled back at mount; delete-marked catalog rows are finalized and purged within a mount (`SHOW META`: `ddl_durable=1`, `catalog_marks_finalized`, `catalog_marks_purged`). Open: the same-name refusal's *message*; undo records for catalog rows (the only thing that would isolate `DROP TABLE`); a cross-core commit oracle before DT9's claim may drop the core-0 scope | `docs/spec/ddl-transactional.md`, `docs/inflight/in-progress/workplan-ddl-transactional.md` |
| Query language, parser, step chains, joins, subqueries | Built (V01-V19; V09 pagination 2026-08-10). Open: V08's `IN (value list)`, V11 (`WITH (...)` table options), V12 (`SET DURABILITY`, the session/admin classes), V20's test, and phase V-6's blueprint parser | `docs/spec/parser-v2.md`, `docs/inflight/in-progress/parser-v2-workplan.md` |
| `ORDER BY` (the output sort) | **Built 2026-08-11** (OB1-OB7): any columns, pk or not, of any relation in a non-aggregated statement, each `ASC`/`DESC`, up to 8 keys, `sort_max_rows`-capped with a top-N heap under `LIMIT`. Refused and left open by decision: ordering over aggregated output | `docs/spec/parser-v2.md` |
| Aggregation (GROUP BY, COUNT/SUM/MIN/MAX/AVG) | Built (AG01-AG10) | `docs/spec/aggregate.md` |
| Aggregate performance | AP01-AP03 built, AP05 next | `docs/inflight/in-progress/workplan-aggregate-perf.md` (start at "Where to pick this up") |
| Types: DATE, TIMESTAMP, DECIMAL, DECIMAL128 | Built (TY01-TY11); `float` stays refused | `docs/spec/types.md` |
| `char(N)` / `varchar(N)` | **Declarable 2026-08-28** (VC-A). `char(N)` is a fixed N-byte cell (`char` alone is `char(1)`; a NUL in the value is refused, since the type reads back to the first one). **`varchar(N)`'s N *is* that column's `kds.inline_cell_width`** — one concept at a narrower scope, the same `[16, 4096]` validator, and by operator rule no second name for it anywhere. N is a width, not a length cap: a longer value spills as always. A bare `varchar` stores `len = 0`, which every pre-existing column carries and which reads as the instance width — so no format bump and an existing file mounts byte-identical. This **reverses** v1's global-only decision (`rule-fixed-length-tuple.md` §4 keeps the old argument and what the build proved wrong about it: the row codec never read the instance width per cell, so the "one codec path" cost it avoided did not exist). Widening stays refused, on a corrected reason | `docs/spec/types.md` §2b, `docs/rules/rule-fixed-length-tuple.md` §4, `instructions/v2.5.0/varchar-char-architecture.md` |
| NULL storage and semantics | **Built 2026-08-20** (NU1-NU8): a tail null bitmap sized to *nullable* columns — every all-`NOT NULL` relation keeps a byte-identical layout, no format bump. Ratified: **D1** NOT NULL default, `NULL` opt-in (divergence from standard SQL, noted in `manual/sql/`); **D2** nullable index keys refused, covered columns included; **D3** NULLs sort largest (ASC last / DESC first). The bitmap is sole authority — tag/bitmap disagreement is Corruption; WHERE is three-valued with `IS [NOT] NULL`; a NULL fk key is vacuous (display-only) | `docs/spec/null.md` |
| Waystone (pattern-keyed access trails) | Recording + replay built (P01-P13); retention/decay/epoch bumps not (P15-P17) | `docs/spec/waystone-concpets.md`, `docs/inflight/in-progress/waystone-workplan.md` |
| CREATE PATTERN | Built through spec §8 step 4 | `docs/spec/create-pattern-user-defined-patterns-v1.md` |
| Cabin (value-observed authoritative store) | v1 built (CB01-CB11); CB12-CB14 add the correlated probe, EXISTS convergence and per-key observation — a cabined join column probed per outer row, the one *banked* acceleration a heap relation's join column can have; entry sets memory-resident. **§6a**: a set is banked only from a read view nothing can contradict — no in-flight transaction, and not inside one | `docs/spec/cabin.md` |
| Secondary indexes (multi-column, covering) | All built (IX01-IX17; IX17 2026-08-18: the correlated probe — a join key's index entered per outer row, spec §8a) | `docs/spec/index.md` |
| Statement-local inner build (hash-built join inner) | Built: the walked-join shape takes a `BuildKey` annotation and buckets rows on first walk into a statement-scoped map (`exec::InnerBuild`), replaying later outer rows' buckets through the full MVCC-and-residual re-check; `join_build_max_rows` (riding `Budget`) is the cap, `0` the off-switch. Build constant 33.6 ns/bucketed row, break-even under k = 2 at every row-set size; the stopping sub-chain's prefix map (JB6) still costs more than it saves at k = 4 (+9.1%/+6.2%) — a bucketed row costs half a walked row, so the prefix only wins past that. `ANALYZE` reports `inner_built=`/`build_rows=`/`build_probes=` per step; a built join owns no `sys.patterns` row and feeds no Waystone trail | `docs/spec/join-inner-build.md` |
| Foreign keys | Declared and enforced (FK-M1..FK-M5); CASCADE/SET NULL out of v1 | `docs/spec/foreign-keys.md` |
| Assertions (group-level constraints) | **Complete and enforcing** (AST01-AST10), including immediately after a restart (recovery rebuilds the registry). **Enforcing on every core**: a Bound Cabin is built from and held by the relation's **owner** core, not core 0; a core that knows of an assertion it cannot enforce refuses the relation's writes rather than admitting them. Open: the admission straddle a parked statement can open, the unacknowledged `done` legs, and a pre-PW1c-6c file's core-0-built cabin, which its owner refuses writes over until a DROP + CREATE | `docs/spec/assertion.md` §6.1, `docs/inflight/in-progress/workplan-peer-writer.md` §7d |
| Access statistics | Built (`SHOW ACCESS`) | `docs/spec/heap-and-tuple.md` §7 |
| ALTER TABLE | Built 2026-08-10 (AL1-AL9, ALT01-ALT05): catalog-only — RENAME TO / RENAME COLUMN; assertions RESTRICT; everything data-moving refused | `docs/spec/alter.md` |
| DROP TABLE | Built 2026-08-10 (DT1-DT6, DT01-DT05): catalog-scoped — oid tombstoned and never reissued, pages orphan (reclamation gated), fkeys/assertions RESTRICT | `docs/spec/drop-table.md` |
| Bulk insert | T1 built 2026-08-10 (BLK01-BLK05): multi-row VALUES, full pipeline per row, BI5 fingerprint rule; T2 gated on the KWP server | `docs/spec/bulkinsert.md`, `docs/inflight/blocked/workplan-bulk-insert.md` |
| Physical optimizer, Part I: relayout | Built and measured (PX01-PX08), **shadow-only as a finding** — every move blocked by a named §6 gate | `docs/spec/physical-optimizer.md` |
| Physical optimizer, Part II: Cabin controller | **Complete** (PHY01-PHY08): controller end to end over the EVT03/EVT06 substrate, `SHOW CABIN_OPTIMIZER` observability, measured at 10.9× on the 10k improvement case (`tools/cabin_optimizer_benchmark.py` reproduces it); managed state is memory-resident — restart forgets, re-observation rebuilds | same docs, Part II / §II.1-§II.7 |
| Buffer-pool eviction | **Armed 2026-08-13**: the `PageRef` migration is built — every accessor returns a pinned handle, and the CLOCK sweep runs on the fault path under a `buffer_pool_frames` budget (default 0 = unbounded until sized). Proven by the full suite at an 8-frame budget with poisoned reclaims. Background reserve trigger still gated on EVT02's bounded pool | `docs/spec/eviction.md`, `docs/inflight/in-progress/workplan-eviction.md`, `docs/spec/page.md` §3 |
| Cross-core execution | Built: a **two-step join executes across cores**, each stage minting its own core's latest-committed view, with pipeline replies byte-identical to local execution. **Statement shipping**: an autocommit, single-relation statement (read or write) whose relation another core owns is carried to the owner as text, parsed and bound there against the owner's own catalog, executed under the owner's ordinary implicit transaction and committed through the owner's group committer. A lost answer is `UNKNOWN_OUTCOME`, never a retryable refusal, since the engine issues primary keys; refused by scope spanning two owners or from a path that cannot park - **no longer inside an explicit transaction**, which is what cross-owner transactions converted. **Measured** (`bench/v2.3.0/`): shipping's cost is a flat ~20 µs of wire once the reactor-wake fix removed the idle-block penalty it used to carry; arrival-core CPU per shipped statement runs 0.028–0.032 busy at 1-4 parked waiters; `cores = 1` unmoved. **Ownership is range-granular** (`docs/spec/crosscore.md` CC8-CC10): a relation starts as one range owned by its creating core; nothing range-granular (splitting) is built yet. **Cross-owner transactions are built** (2026-08-28, `docs/spec/cross-owner-txn.md`): a transaction touching relations several cores own commits or aborts as a whole over a two-phase protocol whose decision lives in exactly one stream, with the arrival core coordinating and participants discovered as it runs. Both halves cross - writes since R6-8, **reads since RR1** - and a read enrols, because a transaction must see its own uncommitted writes on a peer. RR gets a consistent-per-core snapshot, RC no cross-core promise at all. **Measured** (`bench/v2.5.0/results-rr-read-half-v2.4.0-32-g2a1cdcc.md`): a realistic booking workload went from RP8's *zero* transactions on the protocol to **100/100 committed**, every one multi-participant; an enrolled read costs 2.25-9.08x an autocommit foreign read because it changes route rather than gaining a branch, and the read-only participant's decide costs 7-30x the read again; D5's 200 ms ceiling holds, chosen on writer stall alone because log retention turned out not to track the knob. Complete **at relation granularity**; multi-*range* transactions inherit the protocol unchanged and are blueprint §11's R6. Open: cross-core FK; the read-only-participant optimisation, which is now the largest measured cost the line leaves; the 992-byte reply cap that bounds a read inside a cross-owner transaction | `docs/spec/cross-owner-txn.md`, `docs/spec/crosscore.md`, `docs/inflight/in-progress/workplan-cross-owner-txn.md`, `docs/inflight/in-progress/workplan-crosscore.md`, `docs/spec/sched.md`, `docs/inflight/in-progress/memo-shipping-and-group-commit.md` |
| Task representation | Decided and built: C++20 stackless coroutines | `docs/spec/sched.md` §3 |
| Wire protocol KWP/1 | Frame codec only; the server speaks the newline text protocol. TLS 1.3, SCRAM-SHA-256 auth (behind an `AUTH` gate) and three ranked statement-class roles (readonly/readwrite/admin) are built, all off by default. Per-relation grants stay future (catalog-recovery-gated) | `docs/spec/protocol.md`, `docs/inflight/in-progress/protocol-wp.md`, `docs/spec/client-manual.md` |
| Keystone id issue-once contract | K-M1, K-M3, K-M4 built: a pk `UPDATE` is refused at compile, and `sys.tables.next_id`'s bump logs and replays with every other catalog write, closing the unlogged-ceiling half of K1's crash exposure. Read the findings before quoting the invariant engine-wide — the owning docs decide when K1 may be called held | `docs/rules/keystoneid-invariant.md`, `docs/rules/keystoneid-k0-findings.md` |
| Caller-supplied pk (the key mode, and its removal) | Every relation takes a caller-named pk or issues one when `INSERT` omits it, **per row**, and one statement may mix the two; `sys.tables.next_id` is a high-water mark on what has been *placed*. A named key **below** the mark is admitted only on a btree relation (the descent proves it) and refused `OutOfRange` on a heap one. A per-relation **key mode** existed and **was deleted 2026-08-25**: no `ASSIGNED`, no `EXPLICIT` relation, no `CREATE TABLE` declaration about keys — `KeyMode` was repurposed in place as `KeyOrder` (an observation, not a declaration), same byte, no format bump. `DESCRIBE` prints `key_order=`/`autoincrement=if-omitted`; the pk stays non-updatable | `docs/spec/heap-and-tuple.md` §4.1, §3.1b |
| Simulation harness (seed-driven, above the unit suites) | SIM01-SIM07 built: a whole instance on crashable in-memory devices, driven through `CommandDispatcher`, crashed at a seed-chosen op, restarted and reconciled against an oracle that models an errored write's outcome as unknown rather than absent (SIM05-07 add injected device errors and a quiescence probe). `scripts/sim.sh` runs 95+ cells green; the harness caught one wrong answer on its first sweep (a Cabin entry set banked under an in-flight transaction, fixed by `docs/spec/cabin.md` §6a). Declined: torn transfers (need a `Crash(prefix)` device primitive), joins/subqueries in the generator. Unbuilt: phase S-3's history checkers (SIM08-SIM11), S-4's fuzzing and CI matrix (SIM12-SIM14) | `docs/inflight/in-progress/workplan-testing.md` |
| Range-granular core ownership | The end-state for dynamic core allocation: pk-range ownership units, Waystone/Cabin as the routing layer, id-block-aligned insert spreading, CC7's handoff as the mover. **R0 closed**: PL ratified (PL-B + PL-C handoff, `docs/spec/page-lsn-cross-stream.md` §9); the range rules are promoted into `docs/spec/crosscore.md` v2 (CC8-CC10, §2a, §5-§6b), which owns the unit, directory, routing, write scope, split gates and migration contract. **R1's PW1c series built**: the handoff record, the PL-C stamp, exact-page write grants, and the first funded peer INSERT running end to end, with ownership surviving a restart; every task row and named debt lives in `docs/inflight/in-progress/workplan-peer-writer.md` §8. **R2's static half built** (global frame accounting; the dynamic arbiter is not). **R6's gate is satisfied** — cross-owner transactions are built and specified, so multi-range transactions inherit the protocol unchanged and only owner discovery changes. **R3 is the open milestone as of 2026-08-28**: the range directory, resolver, allocation, per-range chains and the pipeline over them, under `instructions/v2.5.0/range-directory.md` (R3-B, rows RB0-RB6 over the workplan's RD2-RD9). Its RA series is built (`sys.ranges` exists empty at superblock v16; `RangeEligible`'s five gates plus the btree decline exist with one caller); D2, D4 and D6's starting value are taken, **D1 is not** and it is what keeps every btree relation unsplittable — which leaves R3's own measurement subject unrepresentative, stated rather than worked around. **R3 is complete** (RB0-RB6, `86f2052`). **R4 is built 2026-08-29** (IS1-IS8, `docs/inflight/in-progress/workplan-insert-spreading.md`): R3 left the spreading mechanism with no producer — no core ever asked for a lease block of a relation it did not own — and R4 supplies it, then routes a write by the id it will issue rather than by the relation it names. Still **off by default** (`range_size_ids = kRangeSizeOff`). **Measured 2026-08-29 by R4-M** (`instructions/v2.6.0/r4-k-sweep.md`, `bench/v2.6.0/results-k-sweep-and-read-ceiling-v2.4.0-52-g5b37fec.md`, workplan §9), at k = 1..8 on an 8-logical/4-physical-core host: spreading buys **1.51x at k = 5** on the group arm and **1.196x at k = 8** on the relaxed one, `cores = 1` is unmoved, and **the two durability arms do not track each other** - IS7's 0.2% agreement was a coincidence of k = 2, and §8's prediction that the group arm would be flat is refuted. The 64-stage ceiling is confirmed (refusal at 65-72 stages, 234,776 rows at 4,096) and R4-M found it was **not** what bound a spread relation: two limits sat in front of it - the fan-in *client* existed on core 0 alone, and the route required the reader not to be the relation's `owner_core` - so under `placement = creating`, the default, a spread relation was **unreadable from every core in every shape** from its second range on, and six of this repo's twenty-four scenario relations lost every read shape at ~395 rows. **Both are closed 2026-08-29 by R4-R and RS** (`workplan-insert-spreading.md` §10-§11): every core constructs a `SessionStepClient` and the route asks `TableAccess::ServableBy` - *can a walk on this core alone answer the relation whole* - so a run of ranges the reader owns becomes a **self-directed stage**, the ordinary protocol with the ring hop being a self-send. The surface was then **5 shapes from every core** (star, `+WHERE` pk, `+WHERE` non-pk, `+BETWEEN`, free `+ORDER BY <pk> ASC`), and the route is also refused **inside an explicit transaction**, because each stage mints its own latest-committed view - snapshot forwarding is `crosscore.md` §9's. So the 64-stage ceiling is once again the binding limit. **AG3 widened it to 11 of 16, 2026-08-30** (`workplan-insert-spreading.md` §12d, enumerated at `3446666` in `bench/v2.6.0/results-ag3-read-surface-v2.2.1-140-g3446666.md`): the fan-in's shape gate now admits **any projection and every aggregate** - grouped or not - beside the star read, session-side only, because a stage already ships whole rows filtered by the residual and the widening is just which consumer the decoded row is handed to. The fold is a **local** of `FinishRemoteReads` rather than the dispatcher's hoisted `aggregator_`, whose one-statement contract a parking fan-in would break silently, and a widened shape fans in **only when no single core owns the relation whole** - where one does, the statement ships as text and is folded there, exactly as before. **5 stay refused**: `LIMIT`/`OFFSET` and any sort but `<pk> ASC`, both because a quota and a sort apply at emission while the remote side emits everything in its own order. A **join** over a spread relation is not a shape gate at all and stays unbuilt, so scenario 2 whole still does not run; its hot-path `SUM` does. Unpriced and stated: a fold ships every row it folds (AG-M's partial aggregates are the reserved answer, unbuilt). D6's value goes to the operator on a sweep whose new finding is at the *small* end - below 4,096 a block is spent as fast as it is granted and the relaxed arm falls to 0.434x at 256. One limit rides with it, stated rather than found later: a write naming no pk on a *multi-owner* relation is refused until R6. The second - the read surface, which belonged to nobody - **had an owner and is built**: §15d's self-directed stage and the fan-in client on every core, R4-R/RS. **R5 remains** | `docs/inflight/in-progress/blueprint-range-ownership.md`, `docs/inflight/in-progress/workplan-range-directory.md`, `docs/spec/crosscore.md`, `docs/inflight/in-progress/workplan-peer-writer.md` |
| Stride forest (parallel ascending ingest on btree relations) | **Rejected by the operator, 2026-08-27.** The plan (`stride_n` independent btrees per relation, one class per core, class-routed inserts) was drafted, reviewed and censused but never built; it stays out. Range-granular core ownership is the line this project pursues instead | `docs/spec/crosscore.md` §6b |
| Observability | Proposal only, nothing implemented — **except two `SHOW META` blocks added 2026-08-26** by the statement-shipping pretasks: per-scheduling-group accounting against reactor wall time (`sched_wall_us`, `sched_<group>_polled_us`/`_polls`/`_consumed_us` — T4, paying `bench/v2.1.0` §11-5's debt, so the time charged to no group is computable from outside the process), and the cross-core write refusal counters `docs/spec/crosscore.md` §6 specifies (`cross_core_write_refusals`, `_keys`, `_detail`, per core — T5, the 2PC evidence base's before-era reading). A third block landed 2026-08-27 with the reactor wake (D7 of `instructions/v2.3.0-reactor-wake.md`): the idle policy's own counters - `sched_idle_blocks`, `sched_parked_idle_blocks`, `sched_wake_race_skips`, `sched_idle_block_us`, `sched_wakes_sent`, `sched_wakes_received`, `sched_spurious_wakes`. The one that changes a reading rather than adding a count is the duration: `sched_wall_us - Σ sched_*_polled_us - sched_idle_block_us` separates sleep from uncharged work, so an arrival core reads 79.5% sleep and 10.3% unaccounted where the two used to arrive as one ~90% lump. None of the three is the observability subsystem; all are fields on an existing command | `docs/inflight/in-progress/observability.md`, `docs/spec/sched.md` §4, `docs/spec/crosscore.md` §6 |
| User manual | `manual/` — SQL surface written, verified against code | `manual/sql/sql.md` |

**Engine-wide known gaps** — what is missing, what a restart loses, and
stale claims found in docs — live in `docs/inflight/known-gaps.md`.

## How `docs/` is organized

**`instructions/` is the operator's input; `docs/` and `bench/` are the
output.** A work order or a blueprint's workplan arrives in
`instructions/`; the prose building it produces lands in `docs/`, the
measurement in `bench/<version>/`. Three buckets under `docs/`, one rule
each:

- **`docs/spec/`** — what is confirmed and implemented. The authoritative
  specifications; when this file and a spec conflict, the spec wins.
- **`docs/rules/`** — concepts and constraints that hold across the whole
  codebase: `rules.md`, the fixed-length-tuple rule, the Keystone id
  invariant and its findings.
- **`docs/inflight/`** — what is not finished. `known-gaps.md` is the
  engine-wide register; under it, **`in-progress/`** (open workplans that
  nothing external blocks), **`blocked/`** (a named gate stops the next
  task — say which, in the file), **`bugs/`** (defect reports, one file
  per defect, open until the fix lands with its test), and
  **`verified/`** (the output of a checklist run against the code — a
  report, never a plan).

**A closed workplan is deleted, not archived** (2026-08-26): a plan whose
every task is built carries nothing the spec that owns the subsystem does
not, so it leaves the tree. All of them are in git history at `925f483` —
`git show 925f483:docs/workplan-index.md` returns one — which is what a
citation to a `docs/workplan-*.md` that no longer resolves is pointing at,
in an older doc or in a source comment. The superseded parser documents
(`parser.md`, `parser-workplan.md`, `step-chains.md`,
`step-chains-workplan.md`) went the same way; `docs/spec/parser-v2.md`
replaced them, and `docs/spec/page.md` §7's eviction proposal is replaced
by `docs/spec/eviction.md`.

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
11. **Every** relation's pk is a unique 40-bit id, carried only by the Keystone word, never rebound, **never updatable**. First column must be integer-typed. **Amended 2026-08-11, amended again 2026-08-25** (`docs/spec/heap-and-tuple.md` §4.1): where the id comes from is a per-**row** fact — the `INSERT` names it or omits it — and there is no key mode. `sys.tables.next_id` is a high-water mark on what has been placed; uniqueness follows from it with no page read for an omitted key and for a named key at or above it. A named key **below** the mark is btree-only, proved by the descent, and refused `OutOfRange` on a heap relation. Read §4.1 before relying on ordering — monotonicity is per-relation and now also per-*history*, never engine-wide, and `sys.tables.key_order` is the only truthful reading of it. **Amended again 2026-08-29** (§4.1a, R4): once inserts spread, monotonicity is per **range** — each core issues from its own leased block into its own chain, so ids no longer ascend in *issue* order across a relation. Uniqueness is untouched (every block is carved from the one high-water mark), `key_order` is untouched (every block is carved above the mark), and invariant 3 holds per range structurally.
12. The tuple MVCC header is exactly `trx_id:48 | undo_ptr | data_len | flags` = 20 bytes. There is no `xmax`.
13. **Every tuple is fixed-length**: row size is a schema constant, variable-width values occupy one tagged cell of `kds.inline_cell_width` bytes — the instance's, or the column's own where `varchar(N)` declared one (2026-08-28; the width is per *column*, never per row). A disagreeing length is `Corruption`, never interpreted.
14. **Var-heap values are immutable per version** and `kVarHeap` pages are never relocated. Authoritative data — advisory rules do not apply to it.

## Working Rules

- Fresh codebase: idiomatic modern C++, not kernel-style C. RAII for every
  resource; no raw `new`/`delete` in engine logic; fixed-width integer types
  and `static_assert`ed layouts for on-disk structs; named `constexpr` for
  every size/offset with the derivation in a comment. Full rules:
  `docs/rules/rules.md`.
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
  `docs/inflight/in-progress/workplan-aggregate-perf.md`.
- Every refusal carries the byte position of the offending token;
  `Unsupported` means "understood and declined", `InvalidArgument` means
  "simply wrong". Truthfulness beats convenience: never accept a spelling
  and enforce something other than what was written.
- Nothing new is reserved lightly: keywords hash as identifiers, and
  `kFingerprintVersion` moves only per `fingerprint.hpp`'s bump rule (the
  golden corpus pins it).

## Version Management

The version of record is an **annotated git tag on `main`**. `v1.0.0` at
`ed03b44` set the form; **`v2.0.0` starts at `a755521` (2026-08-24)** and
every version from here follows the rules below.

- **The operator names the version; CLA may move only `z`.** A version is
  set by the operator saying it — *"it is version x.y.z"* — and nothing
  else. No number is ever inferred from a merged milestone, a green suite,
  or the size of a diff. **`x` and `y` are the operator's alone**: CLA does
  not choose them, propose them unasked, or round up to them. The one
  component CLA may move is **`z`**, the third — a tag CLA creates on its
  own initiative differs from the last operator-named version in `z` alone,
  and CLA says plainly in the reply that it moved it. Pushing any tag still
  waits for the word, like every other push.
- **A tag message is a durability claim, so it carries what bounds it.**
  v1.0.0's message says why: *"Known gaps at this tag, stated because a
  version number is a durability claim and these bound it."* Every tag
  therefore has two halves — what is built and enforcing, then the gaps that
  limit it, with `docs/inflight/known-gaps.md` named as the full inventory. A tag
  carrying only the first half overstates the engine.
- **Every measurement names its version, and `git describe --tags` is how** —
  `v2.0.0-37-gaa3e26c`. One string carries the version and the commit, which
  is what keeps a run made *after* the tag from reading as the tag itself.
  Binding on every results file and on any reply that quotes a number.
- **Every results file lives under its version's directory** (operator rule,
  2026-08-25): `bench/<version>/<benchmark>-<git describe>.md` — `bench/v2.0.0/`
  today, a new operator tag opens the next — and a *scenario* run archives its
  raw driver output (JSON summaries and logs, never data files) beside it under
  `bench/<version>/archive/`; narrower measurements archive nothing. The three
  scenario documents at `bench/` top level predate the rule and stay as
  history. `.claude/agents/ck-tester.md` rule 1b carries the detail.
- **Nothing is back-filled.** Results measured before v2.0.0 keep their bare
  commit id: a version was not a fact when they were taken, and stamping one
  on afterwards would date a claim to a build that never carried it.

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
overhead.** *Operator amendment 2026-08-24: suspended for v2-stage
development — the full test suite still gates every step, but the
interleaved A/B overhead measurement is skipped until the operator
reinstates it. A landed v2 change therefore carries "overhead not
measured" as a stated fact, never an implied pass.* Not only "is the suite green": does the change cost
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

**Workflow mode** is an outer loop — `intermediary-agent` pulling tasks
from `cws` and `reporter-agent` syncing outcomes back — that wraps this
four-step process unmodified; it never relaxes a gate, and it activates
only on an explicit request, never by default. `docs/rules/rule-workflow-mode.md`
is the full rule.

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
- **A measurement names the version and the commit it was measured at** —
  `git describe --tags`, so `v2.0.0-37-gaa3e26c` rather than `aa3e26c`
  alone — which `bench/results-*.md` already requires of a results file;
  this extends the same discipline to the reply that quotes it. The rules
  are in Version Management above.

## Open Decisions — DO NOT assume or silently pick

The full statements, options and constraints live in the owning docs; this
is the index. When work touches one: stop and ask, or implement behind an
interface that keeps every listed option viable.

- **Storage** (`docs/spec/heap-and-tuple.md` §8, `docs/spec/page.md`,
  `docs/rules/rule-fixed-length-tuple.md`): heap page split policy;
  `inline_cell_width` default (**now the default a bare `varchar` takes**,
  not the only width — `varchar(N)` overrides it per column, which sharpens
  this question rather than closing it); spilled-value size cap; prefix-inlining
  trigger; purge cadence; the 16 reserved Keystone bits; id reuse; I/O
  backend; whether invariant 3 is ever relaxed. (*Whether a heap relation
  may take a caller-named pk* was **closed 2026-08-25** — yes, at or above
  the high-water mark — and it turned out **not** to be the split policy,
  which is the framing this line carried: a heap never has to place a key
  that sorts inside a full page, because such a key is refused. The split
  policy above is untouched.)
- **Waystone** (`docs/spec/waystone-concpets.md` §9): per-pattern retention and
  eviction; persistence class of trail pages; `arg_hash` collisions; decay
  and sampling; whether invariant 9 is ever amended.
- **Transactions & WAL** (`docs/spec/txn.md` §9, `docs/spec/wal.md` §15): trx-id
  wraparound; `kTrxIdBlockSize`; ~~cross-core commit~~ — **built 2026-08-28**,
  `docs/spec/cross-owner-txn.md`, and `wal.md` §3's cross-core `[OPEN]`
  closes with it — and recovery under a
  changed core count, which **narrowed 2026-08-28** by operator
  direction: the count may change in both directions and the reorganisation
  is a **mount-time** operation (online change is out of scope, and not
  architecturally excluded), so *when* is settled and *how* is open.
  `wal.md` §3, `superblock.hpp`'s pin and `blueprint-range-ownership.md` §12
  carry it with the three constraints it rides with. (Page redo identity across streams was **ratified
  2026-08-24**: PL-B logged handoff with the PL-C stream stamp,
  `docs/spec/page-lsn-cross-stream.md` §9 — no longer open; PL-A reopens
  only if 2PC is ratified, by that decision. **That trigger fired
  2026-08-28** when D1–D7 were ratified, so **PL-A is open and awaiting the
  operator**: R6-7 executed the revisit and found 2PC changes nothing about
  page identity across streams — the decision lives in one stream, no wire
  payload carries an LSN, and the recovery-time resolution is a lookup
  rather than a cross-stream comparison — so CLA's proposal is to decline
  PL-A again.
  The verdict is the operator's to rule on; `page-lsn-cross-stream.md` §9
  and `workplan-cross-owner-txn.md`'s R6-7 carry the argument.) (Undo retention was ratified and built 2026-08-19,
  `docs/inflight/in-progress/workplan-undo-purge.md` — `SnapshotTooOld` surfacing reopens only
  with the declined byte-cap policy; UP4's mount-time reclaim of a
  previous run's pages stays open there.)
- **Protocol** (`docs/spec/protocol.md`): frame/batch/timeout sizing;
  compression; flow control. (TLS §1, SCRAM §14 and statement-class
  authorization §14 all landed 2026-08-13; what remains open there is
  per-relation grants, gated on catalog recovery.)
- **Parser** (`docs/spec/parser-v2.md`): statement-class ratification; slot-table
  cap; whether `kUnclassified` is production-legal.
- **Cabin** (`docs/spec/cabin.md` §11): budgets and caps; the `CABIN AUTO`
  threshold; pruning cadence; entry-set persistence; multi-column keys.
- **Join inner build** (`docs/spec/join-inner-build.md` §7, §8): cap
  scoping (statement vs step); the catalog-count decline; the peer-side
  build.
- **Aggregation** (`docs/spec/aggregate.md` §10): `aggregate_max_distinct`;
  `MIN`/`MAX` with `DISTINCT`; lifting `ORDER BY`/`HAVING`; pre-aggregation
  below a join is *rejected*, not open.
- **Cross-core** (`docs/spec/crosscore.md` §9, `docs/spec/sched.md` §10):
  ~~the 2PC protocol~~ — **decided and built 2026-08-28**,
  `docs/spec/cross-owner-txn.md`, at relation granularity; what stays open
  behind it is **multi-range** transactions (they inherit the protocol
  unchanged and need RD3's resolver), cross-core FK, and snapshot
  forwarding to the remote-step pipeline. Still open here:
  batch/credit/ring/extent sizing; the `ring_full` retry protocol;
  core-count changes; initial placement policy (`creating` | `rotate`);
  split/migrate policy and constants (the mover); auxiliary placement
  under a split relation (each §6a gate's owner); the shared-structure
  access mechanism. (The id-block interleave default was closed
  2026-08-27 as **default** — CLA's reading of the operator's range
  direction, correctable — `docs/spec/crosscore.md` §6b.) (`CREATE
  INDEX` on a peer-owned relation is **decided and built 2026-08-25**:
  the owner builds, PW1c-6b complete, `docs/spec/ddl-transactional.md`
  §5e and `docs/spec/crosscore.md` CC7's owner-builds exception.) **Placement
  policy now has a measured input** (2026-08-26,
  `bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md` §6): the
  rotation crossover is a **step at the first core to take a second
  session**, and past it rotation is negative at seven writer cores
  (0.51×). The decision is still the operator's; what is no longer open
  is the shape of the curve it would be made on.
- **Foreign keys** (`docs/spec/foreign-keys.md`): forward-check expression;
  the busy status code; heap parents; `kFkNullable`; CASCADE/SET NULL.
- **Indexes** (`docs/spec/index.md` §13): `kIndexStringKeyBytes`; split
  point; column caps; entry reclamation; the index-only scan (gated on a
  visibility witness — do not attempt a partial one); `UNIQUE`; whether the
  measured crossover is ever acted on.
- **Physical optimizer** (`docs/spec/physical-optimizer.md` §6, §10): the
  three enactment gates (each owned elsewhere, reported by name in
  `SHOW RELAYOUT`); the mover; `decay_half_life`.
- **Project**: C++ standard/toolchain pin, build system, test framework
  (propose, don't decide).

## Maintenance rule for this file

When a decision lands: update the spec that owns it, move the item out of
Open Decisions here, and flip the milestone row — **do not** write the
detail into this file. A finding with no owning doc gets one (or a section
in the nearest spec), never a paragraph here.
