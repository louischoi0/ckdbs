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
| Multi-page free map (`page.md` §5) | **Complete 2026-08-26** (FM1-FM11): region-based, ceiling raised from one bitmap page to the 2^31-page / 16 TiB design ceiling, no superblock bump, an existing file mounts unchanged. Open: **D9** (the map stays unlogged; RC04 repairs) and **D6** (`page.md` §4/§5 claims a superblock free-map root `superblock.hpp` has never had); owed: a `sim/` run over a two-region device | `docs/inflight/in-progress/workplan-multi-free-map.md`, `docs/spec/page.md` §5 |
| Clustered B+ tree | Built. **H9 fixed 2026-08-30**: an append-split publishes the separator *before* the sibling link, so a failed grow cannot leave a leaf routed by nothing; the leaf **division**'s identical exposure is open | `docs/spec/heap-and-tuple.md` §5, `docs/inflight/known-gaps.md` |
| WAL | Every data mutation logged (heap, undo, var-heap, index, assertions, catalog/DDL); still outside: ALLOC/FREE and the advisory Waystone classes invariant 8 exempts (§11a). Recovery runs at mount — analysis/redo/high-water/undo per core, a completion checkpoint, `SHOW META`'s recovery block; the catalog recovers too, and a torn catalog page refuses the mount rather than being served corrupt | `docs/spec/wal.md` |
| Transactions & MVCC | Built (T01-T14): reader leases bound a purge (`ReadHorizon()`), undo pages recycle into the log's next growth. `SnapshotTooOld` stays unreachable by decision; prior-run pages leak until UP4 | `docs/spec/txn.md`, `docs/inflight/in-progress/workplan-undo-purge.md` |
| Transactional DDL | **Built 2026-08-16** (DT1-DT7), **durable since 2026-08-19** (RV3): catalog writes logged, DDL under a real transaction, losers rolled back at mount, delete-marked rows finalized and purged within a mount. `CREATE TABLE` is atomic, isolated and consistent; `CREATE INDEX`/`DROP INDEX` are atomic and isolated (`DROP` on core 0 only). **`DROP TABLE` is atomic but *not* isolated** (§5a) — others see the relation gone before commit. Open: the same-name refusal's message; undo records for catalog rows (the only thing that would isolate `DROP TABLE`); DT9's cross-core commit oracle | `docs/spec/ddl-transactional.md`, `docs/inflight/in-progress/workplan-ddl-transactional.md` |
| Query language, parser, step chains, joins, subqueries | Built (V01-V19). Open: V08's `IN (value list)`, V11 (`WITH (...)` table options), V12 (`SET DURABILITY`, the session/admin classes), V20's test, and phase V-6's blueprint parser | `docs/spec/parser-v2.md`, `docs/inflight/in-progress/parser-v2-workplan.md` |
| `ORDER BY` (the output sort) | **Built 2026-08-11** (OB1-OB7): any columns, pk or not, in a non-aggregated statement, each `ASC`/`DESC`, up to 8 keys, `sort_max_rows`-capped with a top-N heap under `LIMIT`. Refused by decision: ordering over aggregated output | `docs/spec/parser-v2.md` |
| Aggregation (GROUP BY, COUNT/SUM/MIN/MAX/AVG) | Built (AG01-AG10) | `docs/spec/aggregate.md` |
| Aggregate performance | AP01-AP03 built, AP05 next | `docs/inflight/in-progress/workplan-aggregate-perf.md` (start at "Where to pick this up") |
| Types: DATE, TIMESTAMP, DECIMAL, DECIMAL128 | Built (TY01-TY11); `float` stays refused | `docs/spec/types.md` |
| `char(N)` / `varchar(N)` | **Declarable 2026-08-28** (VC-A). `char(N)` is a fixed N-byte cell (`char` alone is `char(1)`; a NUL in the value is refused). **`varchar(N)`'s N *is* that column's `kds.inline_cell_width`** — one concept at a narrower scope, the same `[16, 4096]` validator, and by operator rule no second name for it anywhere. N is a **width, not a length cap**: a longer value spills as always. A bare `varchar` stores `len = 0` and reads as the instance width, so no format bump. Reverses v1's global-only decision; widening stays refused | `docs/spec/types.md` §2b, `docs/rules/rule-fixed-length-tuple.md` §4, `instructions/v2.5.0/varchar-char-architecture.md` |
| NULL storage and semantics | **Built 2026-08-20** (NU1-NU8): a tail bitmap sized to *nullable* columns, so every all-`NOT NULL` relation keeps a byte-identical layout and no format bump. Ratified: **NOT NULL is the default and `NULL` is opt-in — a deliberate divergence from standard SQL**; nullable index keys refused; NULLs sort largest. The bitmap is sole authority (tag/bitmap disagreement is Corruption); WHERE is three-valued | `docs/spec/null.md` |
| Waystone (pattern-keyed access trails) | Recording + replay built (P01-P13); retention/decay/epoch bumps not (P15-P17) | `docs/spec/waystone-concpets.md`, `docs/inflight/in-progress/waystone-workplan.md` |
| CREATE PATTERN (user-declared patterns) | **Withdrawn and removed 2026-08-31** (operator). A pattern is a fingerprint-identified case tracked by statistics; the declaration path was a second model of it. The auto path is untouched — `kFingerprintVersion` stands and no stored waystone was retired. `SysPatternRow`'s `origin`/`flags` stay on disk by decision; `$name` parameters are refused everywhere. The spec is **kept, marked withdrawn**, as the record a re-design starts from | `docs/spec/create-pattern-user-defined-patterns-v1.md` (withdrawn), `instructions/v2.7.0/pd-remove-declared-patterns.md` |
| Cabin (value-observed authoritative store) | v1 built (CB01-CB11); CB12-CB14 add the correlated probe, EXISTS convergence and per-key observation — the one *banked* acceleration a heap relation's join column can have; entry sets memory-resident. **§6a**: a set is banked only from a read view nothing can contradict — no in-flight transaction, and not inside one | `docs/spec/cabin.md` |
| Secondary indexes (multi-column, covering) | All built (IX01-IX17) | `docs/spec/index.md` |
| Statement-local inner build (hash-built join inner) | Built: the walked-join shape takes a `BuildKey` annotation and buckets rows on first walk into a statement-scoped map (`exec::InnerBuild`), replaying later outer rows' buckets through the full MVCC-and-residual re-check; `join_build_max_rows` is the cap, `0` the off-switch. `ANALYZE` reports `inner_built=`/`build_rows=`/`build_probes=` per step; a built join owns no `sys.patterns` row and feeds no Waystone trail. **The build constant and every crossover live in the spec's §5-§6**, amended by measurement | `docs/spec/join-inner-build.md` |
| Foreign keys | Declared and enforced (FK-M1..FK-M5); CASCADE/SET NULL out of v1 | `docs/spec/foreign-keys.md` |
| Assertions (group-level constraints) | **Complete and enforcing** (AST01-AST10), including immediately after a restart. **Enforcing on every core**: the Bound Cabin is built and held by the relation's *owner*, not core 0, and a core that knows of an assertion it cannot enforce refuses the relation's writes rather than admitting them. Open: the admission straddle a parked statement can open, the unacknowledged `done` legs, and a pre-PW1c-6c file's core-0-built cabin (its owner refuses writes until a DROP + CREATE) | `docs/spec/assertion.md` §6.1, `docs/inflight/in-progress/workplan-peer-writer.md` §7d |
| Access statistics | Built (`SHOW ACCESS`) | `docs/spec/heap-and-tuple.md` §7 |
| ALTER TABLE | Built 2026-08-10: catalog-only — RENAME TO / RENAME COLUMN; assertions RESTRICT; everything data-moving refused | `docs/spec/alter.md` |
| DROP TABLE | Built 2026-08-10: catalog-scoped — oid tombstoned and never reissued, pages orphan (reclamation gated), fkeys/assertions RESTRICT | `docs/spec/drop-table.md` |
| Bulk insert | T1 built 2026-08-10 (multi-row VALUES, full pipeline per row, BI5's fingerprint rule); T2 gated on the KWP server | `docs/spec/bulkinsert.md`, `docs/inflight/blocked/workplan-bulk-insert.md` |
| Physical optimizer, Part I: relayout | Built and measured (PX01-PX08), **shadow-only as a finding** — every move blocked by a named §6 gate | `docs/spec/physical-optimizer.md` |
| Physical optimizer, Part II: Cabin controller | **Complete** (PHY01-PHY08) with `SHOW CABIN_OPTIMIZER`; managed state is memory-resident — a restart forgets and re-observation rebuilds | same docs, Part II / §II.1-§II.7 |
| Buffer-pool eviction | **Armed 2026-08-13**: every accessor returns a pinned `PageRef` and the CLOCK sweep runs on the fault path under a `buffer_pool_frames` budget (default 0 = unbounded until sized), proven by the full suite at an 8-frame budget with poisoned reclaims. Background reserve trigger still gated on EVT02's bounded pool | `docs/spec/eviction.md`, `docs/inflight/in-progress/workplan-eviction.md`, `docs/spec/page.md` §3 |
| Cross-core execution | Built: a **two-step join executes across cores**, each stage minting its own core's latest-committed view, replies byte-identical to local execution. **Statement shipping** carries an autocommit, single-relation statement to its owner as text, executed and committed there; a lost answer is `UNKNOWN_OUTCOME`, never a retryable refusal. **Cross-owner transactions built 2026-08-28** — both halves cross, writes since R6-8 and **reads since RR1**, and a read *enrols*, because a transaction must see its own uncommitted writes on a peer; RR gets a consistent-per-core snapshot, RC no cross-core promise at all. Complete at **relation** granularity; multi-*range* inherits the protocol unchanged. **The commit is decomposed 2026-08-31 by XD** (`cross-owner-txn.md` §5a carries the sizing): three device syncs against a one-owner commit's one - *counted* through XD0's new `SHOW META` `wal_syncs`, not inferred - and **two of the three legs are unconditional on the durability class**, so the increment is +2 syncs under `strict`, `group` *and* `relaxed`. Additive at one transaction (3.077x strict / 3.111x group, against R6-B's 1.975x, which prices a shape shipping one row per participant); under load the device is shared but the queue is not. **It is a device cost** - WAL on tmpfs collapses the increment 51-62x - which corrects `results-scenario2-cores-*`'s "no device in it", amended there. Open: cross-core FK; ~~the read-only-participant optimisation~~ - **half closed 2026-08-31 by SA-T0**: a participant that wrote nothing writes no `TXN_PREPARE` and takes no sync for it, because a transaction with no rows in this stream has a record whose replay is a no-op (`cross-owner-txn.md` §1a); the decide leg is still walked, and `bench/v2.5.0/results-rr-read-half-*` still prices what is left; ~~the 992-byte reply cap on a read inside a cross-owner transaction~~ - **closed 2026-09-01 (XG1)**, specified at `crosscore.md` §4a: a typed client's shipped read is answered in **rows on an answer edge over the existing step wire** - a fifth producer on the pipeline, reusing its codec, credit, cancel and EOF - so the cap moves from the whole reply to the **widest row** rather than vanishing. The description crosses first on its own kind, chunked, which bounds a result with no column cap without naming a ceiling; the rows reach the sink unre-encoded because a `ResultSink` says whether it reads that encoding; the owner **buffers and sends under credit** rather than streaming, since a `ResultSink` has no suspension point, so the terminator can outrun the rows and the arrival core waits for the edge's EOF too. Four refusals survive, named in §4a. The text arm is byte-identical and keeps its own 992-byte cap; and **XD1's ask** - **ratified and enacted 2026-08-31** (`instructions/v2.7.1/workorder-xd.md`, work order XE, at `8e76417`/`f979cd1`/`e310f8e`): a participant now acks a decide at its COMMIT **append** under D2, so the chain's *waited* syncs go 3 -> 2 while the three performed syncs stay. `cross-owner-txn.md` §2/§2c/§4 state the contract - **the transaction's durability point is the coordinator's decision record**, a participant's own terminal record being a redo shortcut - and carry the retention obligation that lands ahead of the policy it constrains: a decision-bearing segment may not be recycled until every participant's own terminal record is durable, which a pre-durable ack no longer proves. **Measured** (`bench/v2.7.0/results-xe-ack-at-append-v2.7.0-17-ge310f8e.md`): 45/45 crash-matrix cells hold including the new pre-durable-ack window, and **the saving is not where it was predicted** - serially there is none to resolve (88 us of p50, inside a 16.4% floor, because the deferred sync returns on the next transaction's prepare), while at eight concurrent coordinators it is **25.9% of commit p50**, reversing the prediction that it would shrink under load. What the line keeps from it: a sync counted is not a sync waited for. Costs: `bench/v2.3.0/`, `bench/v2.5.0/`, `bench/v2.7.0/results-scenario2-cores-*`, `bench/v2.7.0/results-xd-commit-decomposition-*` and `bench/v2.7.0/results-xe-ack-at-append-*` | `docs/spec/cross-owner-txn.md`, `docs/spec/crosscore.md`, `docs/inflight/in-progress/workplan-cross-owner-txn.md`, `docs/inflight/in-progress/workplan-crosscore.md`, `docs/spec/sched.md` |
| Task representation | Decided and built: C++20 stackless coroutines | `docs/spec/sched.md` §3 |
| Wire protocol KWP/1 | **Built and spoken 2026-08-31** (milestone KW, P01-P16 less P17): the default port speaks length-prefixed binary frames — handshake with version and capability intersection, SCRAM in `C_AUTH` frames, PARSE/BIND/EXECUTE over server-side statement and portal handles, typed `S_ROW_DESC`/`S_ROW_BATCH`, portal suspension, transaction and durability frames, a structured error registry whose numbering a golden list pins. **The newline protocol is not deleted** — it is `debug_text_port`'s loopback debug surface (§12), off unless configured, and `tcp_server` speaks either by one `set_protocol` call rather than by a second listener class. Ratified with it: `kMaxFrame` 16 MiB, the 64 KiB batch target, a 60 s portal-idle timeout, 64 statements and 64 portals per session (KW-D2/D3, `instructions/v2.7.0/kw-ratification.md`). Three things a client sees that the spec did not originally say, all amended there rather than glossed: a `C_BIND` parameter carries its own `type_oid`/`type_mod` (without one, eight bytes are four different values); **`max_rows` bounds delivery, not execution** — this engine has no suspension point at a row boundary, so a suspended portal holds memory rather than pins; and `C_DESCRIBE` of an *unexecuted statement* is refused, because a result's shape here comes from compiling it. TLS 1.3 and the three statement-class roles are unchanged and still off by default. **Measured, as KW-D6 required** (`bench/v2.7.0/results-kwp-cutover-v2.2.1-160-geecda94.md`, both arms built from a clean `git archive` of their own commit and run on one quiet host): the write path shows **no cutover cost** — `insert` and `update` are fsync-bound under `durability = group` and sit inside the run's own 4.5% noise floor at every size — and the read path shows a real one that is **entirely client-side**: a flat ~35-46 us per statement plus ~4 us per row returned, which is 9-11x throughput on a full scan and shrinks to +7.1% on a point select at 10,000 rows because the tax is flat while the base grows. The finding under it is the one worth carrying: **the server got faster** (223 us against 247 us of engine time on a 1,000-row scan) because it no longer formats a reply string, and **42% of the client-side delta is not the wire at all** but the compatibility shim `ckdbs_cli.py` runs to reproduce the newline protocol's string shape for the thirty drivers that read it. Every earlier `bench/` number is a newline-protocol number and must not be diffed against a KWP one. Open: `STOP` as a capability-gated admin statement (KW-D4 deferred it, so `STOP` is reachable only on the debug port and §106's "one surface, one auth story" is amended to say so), the `COMPRESSION` bit (KW-D5), the idle-*session* timeout, per-relation grants, and **§15-5's crash-injection half, which is owed and not run** | `docs/spec/protocol.md`, `docs/inflight/in-progress/protocol-wp.md`, `docs/spec/client-manual.md`, `instructions/v2.7.0/kw-kwp.md` |
| Keystone id issue-once contract | K-M1, K-M3, K-M4 built: a pk `UPDATE` is refused at compile, and `sys.tables.next_id`'s bump logs and replays with every other catalog write. Read the findings before quoting the invariant engine-wide — the owning docs decide when K1 may be called held | `docs/rules/keystoneid-invariant.md`, `docs/rules/keystoneid-k0-findings.md` |
| Caller-supplied pk (the key mode, and its removal) | Every relation takes a caller-named pk or issues one when `INSERT` omits it, **per row**, and one statement may mix the two; `sys.tables.next_id` is a high-water mark on what has been *placed*. A named key **below** the mark is btree-only (the descent proves it) and refused `OutOfRange` on a heap relation. The per-relation **key mode was deleted 2026-08-25** — `KeyMode` repurposed in place as `KeyOrder`, an observation rather than a declaration, same byte, no format bump. The pk stays non-updatable | `docs/spec/heap-and-tuple.md` §4.1, §3.1b |
| Simulation harness (seed-driven, above the unit suites) | SIM01-SIM07 built: a whole instance on crashable in-memory devices driven through `CommandDispatcher`, crashed at a seed-chosen op, restarted and reconciled against an oracle that models an errored write as *unknown* rather than absent. `scripts/sim.sh` runs 95+ cells green. **It has caught two real defects** — a Cabin entry set banked under an in-flight transaction, and H9. Declined: torn transfers (need a `Crash(prefix)` primitive), joins/subqueries in the generator. Unbuilt: S-3's history checkers (SIM08-SIM11), S-4's fuzzing and CI matrix (SIM12-SIM14) | `docs/inflight/in-progress/workplan-testing.md` |
| Range-granular core ownership | **R0-R4 built; spreading ships OFF and is a per-relation option** (operator amendment 2026-08-31, which **reverses DA1's arming**): a relation spreads only where the user asks, so `Expeditor::Config` ships `kRangeSizeOff` and `kRangeSizeIdsDefault` (65,536) is now the *size* a range measures rather than a default. **The id promise is the switch's other half**: spreading off, the pk is an identity **and a sequence** — monotonic in issue order; spreading on, an identity and nothing more. **Not built, and named**: the per-relation flag itself, which has no spare byte on `SysTableRow` (a superblock 15 → 16 event) and no syntax (V11's `WITH (...)`, unbuilt). R3 built the directory (RB0-RB6), R4 the producer and id-based write routing (IS1-IS8), R4-R/RS the read surface from every core (a run of ranges the reader owns becomes a self-directed stage), and AG3 widened the fan-in's shape gate to 11 of 16 shapes. **DA2** closed initial placement as `creating`; **DA3** raised `kMaxFanInUpstreams` 64 → 255. **The two traps DA1's arming created are no longer reachable by default, and neither is fixed.** `RefuseAuxiliaryOnSplitRelation` still **does not recover** — `CREATE INDEX`/`CABIN`/`ASSERTION`/FK are refused *forever* on a relation of two or more ranges and nothing merges ranges (the mover is R5, unbuilt) — but write-then-index is no longer the *default* order, so reaching it now takes an operator or a relation that asked. Work order SA narrows the gate itself (SA-T2/T3/T6). The second trap stands unchanged: **no test exercises a spreading `Expeditor` end to end**, so a green suite says nothing about the armed configuration. Also refused on a multi-owner relation: a write naming no pk (R6's), a **join** over a spread relation (unbuilt — scenario 2 whole does not run at `cores ≥ 2`), `LIMIT`/`OFFSET`, any sort but `<pk> ASC`, and any read inside an explicit transaction. **D1 is still not taken**, which keeps every btree relation unsplittable and every range measurement's subject unrepresentative. **R5 (the mover) is the open milestone**; **R1 was declined as a stage 2026-08-31** — a peer *ships* DDL to core 0 rather than gaining the right to execute it — and what R5 needed from it, a peer's accesses reaching `sys.access_stats`, comes from CR7's batching to core 0 instead (CC12-CC13). Measured in `bench/v2.6.0/` (k sweep, read surface, fold cost) and `bench/v2.7.0/` (DA-b; scenario 2 at 1/2/4/8 cores, where spreading is **0.72× of single-core** once sessions sit on peer cores) — every one of those numbers is now a number for a **configuration an operator sets**, not for a default, and reading them as the shipped engine's behaviour would overstate it | `docs/inflight/in-progress/blueprint-range-ownership.md`, `docs/inflight/in-progress/workplan-range-directory.md`, `docs/inflight/in-progress/workplan-insert-spreading.md`, `docs/spec/crosscore.md`, `docs/inflight/in-progress/workplan-peer-writer.md` |
| Stride forest (parallel ascending ingest on btree relations) | **Rejected by the operator 2026-08-27**, and closed in its own file 2026-08-31 (the file keeps its path so four citations resolve; the status is its banner, not its directory). Range-granular core ownership answers the same rightmost-leaf serialisation without a second per-relation structure or a second routing rule | `docs/spec/crosscore.md` §6b, `docs/inflight/blocked/workplan-stride-forest.md` |
| Observability | Proposal only. Three `SHOW META` blocks sit in front of it — per-scheduling-group accounting against reactor wall time, the cross-core write refusal counters, and the idle policy's counters — and all three are fields on an existing command, **not** the subsystem | `docs/inflight/in-progress/observability.md`, `docs/spec/sched.md` §4, `docs/spec/crosscore.md` §6 |
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
11. **Every** relation's pk is a unique 40-bit id, carried only by the Keystone word, never rebound, **never updatable**. First column must be integer-typed. **Amended 2026-08-11, amended again 2026-08-25** (`docs/spec/heap-and-tuple.md` §4.1): where the id comes from is a per-**row** fact — the `INSERT` names it or omits it — and there is no key mode. `sys.tables.next_id` is a high-water mark on what has been placed; uniqueness follows from it with no page read for an omitted key and for a named key at or above it. A named key **below** the mark is btree-only, proved by the descent, and refused `OutOfRange` on a heap relation. Read §4.1 before relying on ordering — monotonicity is per-relation and now also per-*history*, never engine-wide, and `sys.tables.key_order` is the only truthful reading of it. **Amended again 2026-08-29** (§4.1a, R4): once inserts spread, monotonicity is per **range** — each core issues from its own leased block into its own chain, so ids no longer ascend in *issue* order across a relation. Uniqueness is untouched (every block is carved from the one high-water mark), `key_order` is untouched (every block is carved above the mark), and invariant 3 holds per range structurally. **Amended again 2026-08-31 (operator): whether that happens is the relation's own declared option, default off.** With spreading **off** — which is every relation until one asks — the pk is an identity *and a sequence*, monotonic in issue order, and a client may rely on it; with it **on** the pk is an identity and nothing more. The behaviour is unchanged in both states; what changed is that the promise is now **declared per relation** rather than emerging from an instance-wide key, so a relation can be asked which one it makes. `sys.tables.key_order` stays the truthful *observation* and does not become the declaration — the two are different facts and the amendment adds the second.
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
- **Never push what you have not built — unless the operator says to.**
  `main` has twice received commits that were never compiled.
  `scripts/githooks/pre-push` is the gate; enable it once per clone with
  `git config core.hooksPath scripts/githooks`. On CLA's own initiative the
  gate runs, and `--no-verify` needs a reason CLA can state.
  **Amended 2026-08-31 (operator):** when the operator asks for a push with
  the tests skipped, or for a force push, CLA does it — no re-litigating, no
  waiting for a suite the operator has waived. Two things CLA still owes in
  that case, because they are what keep the exception from becoming a
  silent lowering of the bar: **say plainly in the reply what was skipped**
  (the hook, the suite, or both) and **never report an unrun suite as a
  pass** — a landed commit whose tests were waived carries "not run", not
  an implied green. The Session Workflow's step 4 gate below is a gate on
  CLA proceeding unasked, never on the operator.
- **Measure in `build-release`, never `./build` (Debug)** — Debug has
  reported the wrong sign twice. Per-statement fixed costs: server CPU,
  interleaved A/B. **Re-measure a premise before building the fix.** Details:
  `docs/inflight/in-progress/workplan-aggregate-perf.md`.
- Every refusal carries the byte position of the offending token, and
  "understood and declined" is **two** codes since 2026-08-31 (operator
  rule): `Unsupported` is what the architecture cannot admit, `NotImplemented`
  is what nobody built yet, and each reaches a client as its own token.
  `InvalidArgument` stays "simply wrong". The test between the pair, and the
  full statement, are in `include/kds/base/status.hpp` and
  `docs/spec/protocol.md` §11. Truthfulness beats convenience: never accept
  a spelling and enforce something other than what was written.
- Nothing new is reserved lightly: keywords hash as identifiers, and
  `kFingerprintVersion` moves only per `fingerprint.hpp`'s bump rule (the
  golden corpus pins it).

## Version Management

The version of record is an **annotated git tag on `main`**. `v1.0.0` at
`ed03b44` set the form; **`v2.0.0` starts at `a755521` (2026-08-24)** and
every version from here follows the rules below. **The current version of
record is `v2.7.0` at `d840a30` (2026-08-31)** — insert spreading armed by
default; read its tag message, which names the gaps that bound it.

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
the gate had passed. **This stops CLA, not the operator** (2026-08-31): an
operator who asks for the push anyway gets it, with what was skipped named
in the reply and never reported as a pass.

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
is the index, and it carries **only what is still open** — a decision that
lands leaves this list and is recorded where it belongs. When work touches
one: stop and ask, or implement behind an interface that keeps every listed
option viable.

- **Storage** (`docs/spec/heap-and-tuple.md` §8, `docs/spec/page.md`,
  `docs/rules/rule-fixed-length-tuple.md`): heap page split policy;
  `inline_cell_width` default (**now the default a bare `varchar` takes**,
  not the only width — `varchar(N)` overrides it per column, which sharpens
  this question rather than closing it); spilled-value size cap;
  prefix-inlining trigger; purge cadence; the 16 reserved Keystone bits; id
  reuse; I/O backend; whether invariant 3 is ever relaxed.
- **Waystone** (`docs/spec/waystone-concpets.md` §9): per-pattern retention
  and eviction; persistence class of trail pages; `arg_hash` collisions;
  decay and sampling; whether invariant 9 is ever amended.
- **Transactions & WAL** (`docs/spec/txn.md` §9, `docs/spec/wal.md` §15):
  trx-id wraparound; `kTrxIdBlockSize`; UP4's mount-time reclaim of a
  previous run's undo pages (`workplan-undo-purge.md`); and **recovery
  under a changed core count**, narrowed 2026-08-28 by operator direction
  — the count may change in both directions and the reorganisation is a
  **mount-time** operation (online change is out of scope, not
  architecturally excluded), so *when* is settled and *how* is open
  (`wal.md` §3, `superblock.hpp`'s pin, `blueprint-range-ownership.md`
  §12, and the three constraints it rides with).
- **Protocol** (`docs/spec/protocol.md` §14): ~~frame/batch/timeout
  sizing~~ — **closed 2026-08-31** by KW-D2/KW-D3 (`kMaxFrame` 16 MiB, the
  64 KiB batch target, a 60 s portal-idle timeout, 64 statements and 64
  portals per session; the last is defensible rather than measured and §14
  says so). Still open: the **idle-session** timeout, a different quantity
  — nothing bounds an authenticated connection sitting with no statement
  and no portal; ~~compression~~ — **deferred by KW-D5**, the bit reserved
  and unoffered, and adding it later is not a version break; credit/window
  flow control, which §7's explicit `max_rows` suspension replaces until
  something needs more; `STOP` as a capability-gated admin statement
  (**deferred by KW-D4**, with the consequence that `STOP` now lives only
  on the debug port); and per-relation grants, gated on catalog recovery.
- **Parser** (`docs/spec/parser-v2.md`): statement-class ratification;
  slot-table cap; whether `kUnclassified` is production-legal.
- **Cabin** (`docs/spec/cabin.md` §11): budgets and caps; the `CABIN AUTO`
  threshold; pruning cadence; entry-set persistence; multi-column keys.
- **Join inner build** (`docs/spec/join-inner-build.md` §7, §8): cap
  scoping (statement vs step); the catalog-count decline; the peer-side
  build.
- **Aggregation** (`docs/spec/aggregate.md` §10): `aggregate_max_distinct`;
  `MIN`/`MAX` with `DISTINCT`; lifting `ORDER BY`/`HAVING`; pre-aggregation
  below a join is *rejected*, not open.
- **Cross-core** (`docs/spec/crosscore.md` §9, `docs/spec/sched.md` §10):
  batch/credit/ring/extent sizing; the `ring_full` retry protocol;
  core-count changes; **split/migrate policy and constants — the mover,
  R5**; **auxiliary placement under a split relation** (each §6a gate's
  owner; until one is lifted the refusal is permanent, per the milestone
  row); **multi-range transactions** (they inherit 2PC unchanged and need
  RD3's resolver); **cross-core FK**; snapshot forwarding to the
  remote-step pipeline; and — narrowed to the **btree's top-of-tree hop**
  alone — the shared-structure access mechanism, since a split btree's top
  levels are written by the root's owner core and are the one shared
  structure CC11's rule does not reach (`workplan-range-directory.md` D1).
  The candidate that would remove the structure instead — per-range trees —
  is cited by `crosscore.md` §9 and `instructions/v2.7.0/ratification-da.md`
  as `instructions/v2.6.0/v2.6.0-per-range-trees.md`, **which has never
  existed in this repository**: the candidate is real, the file is not
  written, and the citation is stale in both docs.
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
