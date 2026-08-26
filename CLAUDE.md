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
| WAL | Every data mutation logged (heap, undo, var-heap, index, assertions), **and every catalog/DDL mutation since 2026-08-19** (RV3: ordinary record types, no format bump; the two row-codec definition relations joined the same day, so a recovered `CREATE ASSERTION` *enforces*; still outside: ALLOC/FREE and the advisory Waystone classes invariant 8 exempts, `wal.md` §11a). **Recovery runs at mount 2026-08-12** (RC01-RC11): analysis/redo/high-water/undo per core before the listener binds, a completion checkpoint bounding the next crash, `SHOW META`'s recovery block, and assertion enforcement resumed. **The catalog recovers too (RV3 closed 2026-08-19)**: `catalog_recovered=1`, and a torn catalog page now refuses the mount instead of being served corrupt | `docs/wal.md`, `docs/workplan-wal-recovery.md`, `docs/workplan-rv3-catalog-recovery.md` |
| Transactions & MVCC | Built (T01-T14). **Reader registration built 2026-08-19** (RR2-RR4): every snapshot that can outlive a park carries a `ReaderLease`, and `ReadHorizon()` is the bound a purge retires below. **The undo purge landed the same day** (UP1-UP3, ratified horizon-only / internal-recycle / purge-on-growth): a settled page recycles into the log's next growth, this run's chain plateaus, `SHOW META` prints `undo_pages_live`/`undo_pages_recycled`; `SnapshotTooOld` stays unreachable by decision, prior-run pages leak until UP4. MVCC ships before recovery (see §8's gap) | `docs/txn.md`, `docs/txn-workplan.md`, `docs/workplan-reader-registration.md`, `docs/workplan-undo-purge.md` |
| Transactional DDL | **Built 2026-08-16** (DT1-DT7, v1 scope complete), reversing `docs/txn.md` §9's "out of scope" by direction; §7/§9 amended, and `docs/spec-drop-table.md` DT5 too. **`CREATE TABLE` is atomic, isolated and consistent** — a rollback leaves no relation, and an uncommitted one is invisible by every route (SELECT/INSERT/UPDATE/DELETE/DESCRIBE/SHOW TABLES), with one test walking all of them. **`CREATE INDEX` is atomic and isolated; `DROP INDEX` is atomic and isolated *on core 0* as of 2026-08-18** — it shipped as isolated, review disproved that (`InitTableAccess` reads the index list unfiltered, so a rolled-back drop left an index silently missing rows), it was refused inside a transaction for two days, and **DT9** fixed the read instead: an unfiltered catalog read counts a delete-mark only once its deleter is no longer in flight. Core-0-scoped because the predicate walks one core's live list. **`DROP TABLE` alone is atomic but not isolated** (§5a: its `sys.objects` *retype* is an in-place overwrite with no undo chain, so a filtered read skips the row outright and others see the relation gone before commit; DT9 does not reach it, and undo records are what would). Catalog rows take the real trx id, catalog reads filter by the reader's view, and rollback works by the existing compensation — no undo records. A view is minted **only while some transaction holds uncommitted DDL**, so the cache fast path is untouched. **Durability landed 2026-08-19** (RV3, `docs/workplan-rv3-catalog-recovery.md`): catalog writes logged, DDL under a real transaction autocommit included, losers rolled back at mount through undo records appended inside the catalog's write points; `SHOW META` prints `ddl_durable=1`. **DT10 (2026-08-18) finalizes delete-marked catalog rows at mount**, which closes the one case DT9 could answer wrongly (an unlogged id ceiling reissues a committed dropper's id after a crash); `SHOW META` reports `catalog_marks_finalized`. **§5d (2026-08-19) purges marks within a mount too**: DDL resolution retires every mark whose deleter cleared the read horizon, so a mark waits at most for its last older reader plus the next DDL resolution (the mount takes any remainder); `SHOW META` prints `catalog_marks_purged`. Open: the same-name refusal's *message*; undo records for catalog rows (the only thing that would isolate `DROP TABLE` — its exposure is the `sys.objects` in-place retype, which no delete-mark rule reaches); and a cross-core commit oracle before DT9's claim may drop the core-0 scope | `docs/spec-ddl-transactional.md`, `docs/workplan-ddl-transactional.md` |
| Query language, parser, step chains, joins, subqueries | Built (V01-V19; V09 pagination 2026-08-10). Open: V08's `IN (value list)`, V11 (`WITH (...)` table options), V12 (`SET DURABILITY`, the session/admin classes), V20's test, and phase V-6's blueprint parser | `docs/parser-v2.md`, `docs/parser-v2-workplan.md` |
| `ORDER BY` (the output sort) | **Built 2026-08-11** (OB1-OB7): any columns, pk or not, of any relation in a non-aggregated statement, each `ASC`/`DESC`, up to 8 keys. A sink decorator at the AG1 seam, `sort_max_rows`-capped, with a top-N heap under `LIMIT` and the pk-ascending form elided to zero cost. Refused and left open by decision: ordering over aggregated output | `docs/workplan-order-by.md` |
| Aggregation (GROUP BY, COUNT/SUM/MIN/MAX/AVG) | Built (AG01-AG10) | `docs/feat-aggregate.md`, `docs/workplan-aggregate.md` |
| Aggregate performance | AP01-AP03 built, AP05 next | `docs/workplan-aggregate-perf.md` (start at "Where to pick this up") |
| Types: DATE, TIMESTAMP, DECIMAL, DECIMAL128 | Built (TY01-TY11); `float` stays refused | `docs/spec-types.md`, `docs/workplan-types.md` |
| NULL storage and semantics | **Built 2026-08-20** (NU1-NU8): a tail null bitmap sized to *nullable* columns — every all-`NOT NULL` relation keeps a byte-identical layout, so no format bump and no migration. Ratified: **D1** NOT NULL default, `NULL` opt-in (divergence from standard SQL, loud note in `manual/sql/`); **D2** nullable index keys refused, covered columns included (`IS NULL` answers by scan); **D3** NULLs sort largest (ASC last / DESC first, no `NULLS FIRST/LAST` grammar). The bitmap is sole authority, tag/bitmap disagreement is Corruption; WHERE is three-valued with `IS [NOT] NULL`; aggregates skip; a NULL fk key is vacuous (`kFkNullable` stamped at declaration, display-only). Oracle's variable-length row stays refused by name — it retracts invariant 13 | `docs/spec-null.md`, `docs/workplan-null.md` |
| Waystone (pattern-keyed access trails) | Recording + replay built (P01-P13); retention/decay/epoch bumps not (P15-P17) | `docs/waystone-concpets.md`, `docs/waystone-workplan.md` |
| CREATE PATTERN | Built through spec §8 step 4 | `docs/spec-create-pattern-user-defined-patterns-v1.md` |
| Cabin (value-observed authoritative store) | v1 built (CB01-CB11); CB12-CB14 2026-08-19: the correlated probe, its EXISTS convergence, per-key observation — a cabined join column probed per outer row, spec §4a, the one *banked* acceleration a heap relation's join column can have (`docs/feat-cabin.md` §4a, amended 2026-08-20 — the statement-local inner build is the unbanked one, and the Cabin keeps ladder priority); entry sets memory-resident. **§6a added 2026-08-21**: a set is banked only from a read view nothing can contradict — no in-flight transaction, and not inside one — after the simulation harness found an entry set recorded under an uncommitted DELETE outliving the ROLLBACK that restored the row and being served as authoritative. §6's soundness argument was about a write *racing* the scan and did not reach a write the scan could not *see*; un-observing on rollback was considered and rejected, because it cannot touch the in-flight-INSERT half | `docs/feat-cabin.md`, `docs/cabin-workplan.md` |
| Secondary indexes (multi-column, covering) | All built (IX01-IX17; IX17 2026-08-18: the correlated probe — a join key's index entered per outer row, spec §8a) | `docs/feat-index.md`, `docs/workplan-index.md` |
| Statement-local inner build (hash-built join inner) | Spec ratified 2026-08-19 — the per-statement answer to the walked join `bench/results-scenario3-library.md` §7e priced; parser-v2 §5 carries the sanction. **JB1 built 2026-08-20**: the compile half — the walked-join shape takes a `BuildKey` annotation as the last ladder arm, the step stays `kScan`, every spec §8 decline refused at compile with a test naming it. **JB2-JB5 built 2026-08-20**: the map (`exec::InnerBuild`, Cabin key identity and 24-byte entry reused, walk-order replay pinned), the lazy build (the annotated step's first walk buckets every row passing the non-correlated residual; completed walks publish, stopped ones reset; sub-chains gated out until JB6), the probe (later outer rows replay their bucket through `AcceptTupleAt`'s full MVCC-and-residual re-check, in bucket order, misses conclusive), and the cap (`join_build_max_rows` riding `Budget`; past it the step declines to per-row walks for the statement, never an error; `0` is the off-switch and the A/B lever, contract-swept byte-for-byte). **The build constant halved 2026-08-20** (the JB5 gate's finding, answered): 83.7 → 43.2 ns per bucketed row by an arena-chained map and a Keystone-word pk read, so break-even falls under k = 2 at every row-set size, k=2 turns from a 26% loss into an 11% win and the acceptance cell to 8.5×; the n=2 *deferral* the gate floated is declined with its arithmetic in spec §5, which also retracts that section's "at k ≥ 2 every avoided walk is pure win". **JB6 built 2026-08-20**: the stopping sub-chain's prefix map — a statement-scoped store (a sub-chain runner is rebuilt per outer row), a resumable walk marked by page and rows-covered-in-emission-order, a cap that freezes the mark where the map stopped taking rows, and deferral shown structurally unavailable here (map and mark must advance together or a miss becomes a false absence). The acceptance cell passes — `exists-correlated` 1,681.3 → 547.2 µs (×3.07), examined 27,888 → 6,736 — but **its k=4 done-condition does not**: the crossover is k ≈ 5 and k=4 costs +8%/+11%, so spec §6's "at or below the plain walk at every k" is retracted with the numbers. **The build constant took a third cut the same day**: 43.2 → 37.2 ns/row, the map's last allocation gone with an open-addressed key table (a murmur mix over it measured worse and is recorded as rejected at `TagOf`). **JB7 built 2026-08-20**: `ANALYZE` marks the step `build on=col<N> key=<ref>` before execution and reports `inner_built=`/`build_rows=`/`build_probes=` after — `inner_built=0` printed rather than suppressed, since nothing else separates "annotated and fell back" from "never eligible"; the shipping-equivalence harness now asserts its walked-join rows really build locally (build against shipped walk, not walk against walk), a built join's reply is compared unmoved across the waystone contract's five configurations (pinning "no trail state changes a built reply") and the converse, "a build feeds no trail", is pinned directly by `ABuiltJoinFeedsNoTrail` — a built join must own no `sys.patterns` row, with a keyed control proving the recorder ran, and the four superseded prose passages are amended. **JB8 closed 2026-08-21** (`bench/results-scenario3-library.md` §7g at `aa3e26c`): EXISTS reaches spec §9's class (1,598.8 → 534.3 µs, ×2.99); the join cell runs ×9.60 but **§9's ~600 µs target was itself unreachable** — one pass of the inner relation costs 668 µs, so a *free* build still lands at ~682, and §9 is amended (the third ratified claim in that spec retracted by measurement, after §5 and §6). Constant 33.6 ns/row, break-even under k = 2 at every size; JB6's k = 4 condition still fails at +9.1%/+6.2% with the counters yielding the model that explains it — a bucketed row costs half a walked row, so the prefix wins only when it saves more than half of what it buckets. Four-way at 10k: walk 103 / build 989 / PostgreSQL 1,271 / converged Cabin 14,035, the PG gap closed 1.92× → 1.28×, and on `exists-correlated` ckdbs *leads* PostgreSQL ×2.63 — the first cell in that file won with nothing banked | `docs/spec-join-inner-build.md`, `docs/workplan-join-inner-build.md` |
| Foreign keys | Declared and enforced (FK-M1..FK-M5); CASCADE/SET NULL out of v1 | `docs/impl-foreign-keys.md` |
| Assertions (group-level constraints) | **Complete and enforcing** (AST01-AST10). The recovery-side registry rebuild — outside the AST series — **landed 2026-08-12** as `docs/workplan-wal-recovery.md` RC07: `enforcing=1` immediately after a restart | `docs/feat-assertion.md`, `docs/workplan-assertion.md` |
| Access statistics | Built (`SHOW ACCESS`) | `docs/heap-and-tuple.md` §7 |
| ALTER TABLE | Built 2026-08-10 (AL1-AL9, ALT01-ALT05): catalog-only — RENAME TO / RENAME COLUMN; assertions RESTRICT; everything data-moving refused | `docs/spec-alter.md`, `docs/workplan-alter.md` |
| DROP TABLE | Built 2026-08-10 (DT1-DT6, DT01-DT05): catalog-scoped — oid tombstoned and never reissued, pages orphan (reclamation gated), fkeys/assertions RESTRICT | `docs/spec-drop-table.md`, `docs/workplan-drop-table.md` |
| Bulk insert | T1 built 2026-08-10 (BLK01-BLK05): multi-row VALUES, full pipeline per row, BI5 fingerprint rule; T2 gated on the KWP server | `docs/spec-bulkinsert.md`, `docs/workplan-bulk-insert.md` |
| Physical optimizer, Part I: relayout | Built and measured (PX01-PX08), **shadow-only as a finding** — every move blocked by a named §6 gate | `docs/feat-physical-optimizer.md`, `docs/workplan-physical-optimizer.md` |
| Physical optimizer, Part II: Cabin controller | **Complete** (PHY01-PHY08, closed 2026-08-10): controller end to end over the EVT03/EVT06 substrate, `SHOW CABIN_OPTIMIZER` observability, E2E lifecycle test, measured once at zero-candidate overhead unmeasurable and 10.9× on the 10k improvement case (the write-up is no longer kept in `bench/`; `tools/cabin_optimizer_benchmark.py` reproduces it); managed state is memory-resident — restart forgets, re-observation rebuilds | same docs, Part II / §II.1-§II.7 |
| Buffer-pool eviction | **Armed 2026-08-13**: the `PageRef` migration is built (MG01-MG06) — every accessor returns a pinned handle, the raw seam is `protected`, and the CLOCK sweep runs on the fault path under a `buffer_pool_frames` budget (default 0 = unbounded until sized). Proven by the full suite at an 8-frame budget with poisoned reclaims, plus an ASan sim pass. Background reserve trigger still gated on EVT02's bounded pool | `docs/workplan-pageref.md`, `docs/spec-eviction.md`, `docs/workplan-eviction.md`, `docs/page.md` §3 |
| Cross-core execution | P0-P2, P6, P5-shape row-id leasing and the P4 restriction half built by 2026-08-10; P4a-P4c the same day — the first cross-core statement (single-step star SELECT). **P4d complete 2026-08-15**: the executor's coroutine conversion (4d-1/2/3), the streaming producer parking at page boundaries (4a), chained opens and the consuming stage (4b-1/4b-2), the session side (4b-3) — so a **two-step join executes across cores**, planned by the session and rendered through a typed projected reply — and 4c's gated inner walk, which bounds a walked inner and admits joins on non-pk columns. **P4e complete 2026-08-15**: equivalence (pipeline reply byte-identical to local, twelve shapes, shipping asserted) and the benchmark — the isolation re-run **cannot run** and says why (a peer-owned relation has no writer: CC3 refuses cross-core writes, DML shipping is unbuilt, core 0 alone listens), so the pipeline is priced in process instead at **2.52 µs + 0.626 µs per forwarded row**, which is 1.5× the local per-row cost of the same join and settles 4c's open question in favour of building the per-batch runner handle. Also closed: a stage used to read through in-flight writers; it now mints the owning core's committed view per CC4. **Remaining**: 4c's per-page batching (now justified), the sub-chain await decision, and a writer for peer-owned relations before any *scaling* claim is possible. **v2 rules revision 2026-08-24** (worktree `v2-crosscore-range-rules`): the cross-core rules and policy are range-granular — the ownership unit is the pk range (`docs/crosscore.md` CC8-CC10, §2a), CC3's refusal widens to multi-range writes, §5 adds the one-view-per-(statement, core) rule **gated on the peer writer (PW1c-5), before R3** — its review found the per-stage-view torn read latent in the shipped two-stages-one-peer shape, unreachable only until a peer can commit — §6a gates which relations may split (Cabin gates migration too), §6b states id-block-aligned insert spreading over per-range chains; relation ownership survives as the degenerate one-range case and nothing range-granular is built. **The statement-shipping pretasks ran 2026-08-26** (worktree `worktree-v2.2.0-pretasks-stmtshipping`, `bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md`, on an 8-logical / 4-physical host): batching a commit is worth **79×** on one core, so v2.1.0's per-writer-core `fdatasync` cap is an autocommit artifact; the one-relation serialized baseline **scales linearly with sessions** (≈490×S core 0, ≈590×S a peer, p50 pinned near 2 ms); the rotation crossover is a **step at the first core to take a second session**, not a slope, and at seven writer cores rotation runs **0.51×** while concentrating the same work on one core runs **1.98×** faster; the device overlaps `fdatasync` to a peak at **four** streams and declines after; the **four-core-server effect is not the engine** — three processes that do nothing but wake once a millisecond, beside a *one-core* server, reproduce a four-core server to within 0.2% and seven reproduce an eight-core server to within 0.1%, so `bench/v2.1.0` §11-3's three candidates are unnecessary rather than merely unseparated; a null cell also removes a ~10% harness ordering bias every prior A/B on this driver carried; **94–98% of a loaded reactor's wall time is charged to no scheduling group**; and **80–92% of write statements are refused today** when a client does not route to the owner core. A peer-path defect found on the way is in `docs/known-gaps.md` and re-confirmed after FM2-FM5: sustained shipped `CREATE INDEX` leaves a peer-owned relation permanently unwritable (`page id not found`, not retryable) after ~58 builds, where core 0 runs 279 and then refuses by name with the relation intact. The design memo the shipping workplan is to be checked against is `docs/memo-shipping-and-group-commit.md` | `docs/crosscore.md`, `docs/workplan-crosscore.md`, `docs/sched.md`, `docs/memo-shipping-and-group-commit.md` |
| Task representation | Decided and built: C++20 stackless coroutines | `docs/sched.md` §3 |
| Wire protocol KWP/1 | Frame codec only; the server speaks the newline text protocol. **Direct TLS, SCRAM-SHA-256 auth, and statement-class authorization all built 2026-08-13**: TLS 1.3 at the transport seam; connection auth behind an `AUTH` gate (`auth = scram`, `users_file`, `--add-user`); three ranked roles (readonly/readwrite/admin) enforced per statement at the dispatcher. All off by default; per-relation grants stay future (catalog-recovery-gated) | `docs/protocol.md`, `docs/protocol-wp.md`, `docs/client-manual.md` |
| Keystone id issue-once contract | K-M1, K-M3, K-M4 built (K-M3 2026-08-10: `exec::CompileAssignments` refuses a pk UPDATE at compile with `Unsupported` and a byte). **The unlogged-ceiling half of K1's crash exposure closed 2026-08-19** as an RV3 side effect: the `sys.tables.next_id` bump logs and replays with every other catalog write. Read the findings before quoting the invariant engine-wide — the owning docs decide when K1 may be called held | `docs/keystoneid-invariant.md`, `docs/keystoneid-k0-findings.md`, `docs/workplan-rv3-catalog-recovery.md` |
| Caller-supplied pk (the key mode, and its removal) | Built as a per-relation **key mode** 2026-08-11 (PK01-PK09) and **the mode deleted 2026-08-25**: there is no `ASSIGNED`, no `EXPLICIT` relation, and no `CREATE TABLE` declaration about keys. Every relation takes a caller-named pk or issues one when `INSERT` omits it, **per row**, and one statement may mix the two. `sys.tables.next_id` is a high-water mark on what has been *placed* — an issued id always clears every named one. A named key **below** the mark is admitted only on a btree relation (the descent proves it) and refused `OutOfRange` on a heap one, because §3.1b's tail append, page-wise ordering and tail-page-only duplicate check are that ascent — which is how `HEAP EXPLICIT`, refused at `CREATE` for two weeks, became an ordinary relation without opening the heap page split policy. `KeyMode` was **repurposed in place** as `KeyOrder` (an observation: has an id ever landed out of order), same byte, same offset, so **no format bump** and old data files mount unchanged; it decides only whether `ORDER BY <pk>` can be discarded, and the peer-write refusal moved from the relation to the row. `default_key_mode` removed, `ASSIGNED` refused at parse, `EXPLICIT` accepted and vacuous, `DESCRIBE` prints `key_order=`/`autoincrement=if-omitted`. The pk stays non-updatable | `docs/heap-and-tuple.md` §4.1, §3.1b; `docs/workplan-key-mode.md` (superseded) |
| Simulation harness (seed-driven, above the unit suites) | SIM01-SIM07 built. SIM01-SIM04 build a whole instance on crashable in-memory devices, drive it through `CommandDispatcher`, crash it at a seed-chosen op, restart, sweep and reconcile against an oracle; the recovery gate is armed. **SIM05-SIM07 built 2026-08-21**: injected device errors with a *quiescence probe* (an engine that survives by refusing everything afterwards fails exactly one check) and an oracle that models an errored write's outcome as unknown rather than absent; workload v2 - UPDATE/DELETE by pk and by value, transactions, `CREATE CABIN`/`CREATE PATTERN`, the three advisory switches drawn per iteration plus an on-vs-off pairing over one op stream; and the `SimPlan` seam with a `.sim` case file and a signature-guarded minimizer, plus `scripts/sim.sh`. **It found a wrong answer on its first sweep** - a Cabin entry set banked inside a transaction outlived the ROLLBACK that restored the row (shrunk 1200 ops -> 9), **fixed 2026-08-21** by `docs/feat-cabin.md` §6a; its acceptance test was written gated and is an ordinary regression test now, and `scripts/sim.sh` is 95 cells green. Declined with reasons: torn transfers (they need a `Crash(prefix)` device primitive), joins/subqueries in the generator. Unbuilt: phase S-3's history checkers (SIM08-SIM11), S-4's fuzzing and CI matrix (SIM12-SIM14) | `docs/workplan-testing.md` |
| Range-granular core ownership | The end-state for dynamic core allocation: pk-range ownership units, Waystone/Cabin as the routing layer, id-block-aligned insert spreading, CC7's handoff as the mover. **R0 closed 2026-08-24** — PL ratified (PL-B + PL-C, `docs/spec-page-lsn-cross-stream.md` §9; a same-day rule 5a was retracted by its own review and rule 6, the durable acquisition restamp, replaced it). **The rules promoted 2026-08-24** (worktree `v2-crosscore-range-rules`): `docs/crosscore.md` v2 owns the unit, directory, routing, write scope, split gates and migration contract (CC8-CC10, §2a, §5-§6b); the blueprint keeps the phasing and thesis. **R1 underway, the whole PW1c series built 2026-08-24** (worktree `pw1c1-handoff-record`): the PL contract's first consumers — the handoff record, analysis/redo honoring it, the PL-C stamp, exact-page write grants with the durable-handoff publish, and **the first funded peer INSERT running end to end** — with PW1c-6 closed as the refusal half; every task row, retraction and named debt lives in `docs/workplan-peer-writer.md` §8. **PW2 decided and built 2026-08-24** (operator-delegated): (b), the indirect anchor root — PW2-1..4 in that workplan's §7a, the btree shape gate lifted. **PW1c-7 built 2026-08-24** (worktree `pw1c7-restart-ownership`): ownership survives a restart — the probe found a restart loses every lease-owned page, not just the grants, and the PL-C stamp now carries ownership (a leased store claims own-stamped pages on the fault; an unacquired creation page is re-delivered on request through the same publish CREATE TABLE runs). **PW6 measured 2026-08-25** (worktree `pw6-rotate-benchmark`, `v2.0.0-48-g314a06d`, `bench/v2.0.0/results-multicore-writers-v2.0.0-48-g314a06d.md`): at two writers the peer write path costs nothing resolvable (0.977× against a 0.982× control); the two-writer-core speedup is unmeasurable on this 2-CPU host (`cores > hardware_concurrency()` is refused); the four-writer cell found **lease refills lagging by seconds and rows lost to the unretried extent lease** — **traced and fixed as PW7 the same day** (worktree `lease-refill-lag`): per-leg refill stamps in `SHOW META` showed the second sat *before* the request left — 395 reactor iterations without a poll — because the share-proportional pick re-polled a parked refill 64 times an iteration and the `system` group's resulting debt starved its next task; two floors under the share law (`docs/sched.md` §4: at most one poll per task and at least one per ready group per iteration) bring the wait to 2.7 ms and the four-writer cell to 0.99–1.03× with no lost rows. **PW3b built 2026-08-25** (worktree `pw3b-peer-shutdown-checkpoint`): the peer's shutdown checkpoint, published direct through core 0's superblock anchor after the worker join, so a clean stop's remount reads 2 records where it read 810 — and its test closed a pre-existing defect, an extent reserved after core 0's last flush that never reached the device, so the grant handler now persists the map before a grant leaves. **PW6 re-measured at the fixed engine 2026-08-25** (`v2.0.0-67-g952bbb9`, `bench/v2.0.0/results-multicore-writers-v2.0.0-67-g952bbb9.md`): four writers 1.030× with zero lost rows, the row-id refill's longest wait 3.0–3.3 ms, the two-writer peer path 0.990× against a 0.944× control, a point-SELECT beside a committing session 1,088/1,083 µs against 37/35 alone (the fdatasync-on-reactor gap, unchanged); `cores = 3` is still refused on the 2-CPU host, so the speedup cell is still unrun. **PW6's finding (2) closed 2026-08-25** (worktree `lease-refusal-retryable`): the three leases' spent refusals are `TxnConflict` — the one code the wire's `retryable` bit follows, by `status.hpp`'s decision — rendered through `ErrorReply` at every site a lease can refuse, and a refill core 0 has denied answers once without the bit; `docs/known-gaps.md` carries the reasoning. R2's static half built; R3-R6 remain | `docs/blueprint-range-ownership.md`, `docs/crosscore.md`, `docs/workplan-peer-writer.md` |
| Stride forest (parallel ascending ingest on btree relations) | **Plan only, nothing built** — drafted 2026-08-25, reviewed (§9 of the plan, re-checked against `9b498d0`) and censused (§8, SF-V1/SF-V2 at `952bbb9`). Each relation would hold `stride_n` independent btrees, one per class owned by one core, the high-water mark per class in an owner-written sub-anchor; an omit-key INSERT issues from the arrival core's class, a named key routes by `(key / stride_b) mod stride_n`, and a statement spanning classes is refused naming R6. **Blocked on the operator**: D1/D2 as drafted assume every user relation is btree-clustered, and `e13ad71` kept heap as the `CREATE TABLE` default — the plan carries an `[OPERATOR]` marker for their restatement (the review's reading: `STRIDE n` opt-in on `BTREE`, default 1 = today's relation). The census sized SF5 as the largest row (the pipeline's producer count is fixed at one structurally in four places; R3 is not a prerequisite) and named what a split must replicate (var-heap, grants, Waystone entries, the budget surfaces). SF-V0 — the fdatasync-overlap premise probe — cannot run on the 2-CPU host, and no build row starts before it | `docs/workplan-stride-forest.md`; lineage `docs/crosscore.md` §6b, `docs/workplan-peer-writer.md` |
| Observability | Proposal only, nothing implemented — **except two `SHOW META` blocks added 2026-08-26** by the statement-shipping pretasks: per-scheduling-group accounting against reactor wall time (`sched_wall_us`, `sched_<group>_polled_us`/`_polls`/`_consumed_us` — T4, paying `bench/v2.1.0` §11-5's debt, so the time charged to no group is computable from outside the process), and the cross-core write refusal counters `docs/crosscore.md` §6 specifies (`cross_core_write_refusals`, `_keys`, `_detail`, per core — T5, the 2PC evidence base's before-era reading). Neither is the observability subsystem; both are fields on an existing command | `docs/observability.md`, `docs/sched.md` §4, `docs/crosscore.md` §6 |
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
11. **Every** relation's pk is a unique 40-bit id, carried only by the Keystone word, never rebound, **never updatable**. First column must be integer-typed. **Amended 2026-08-11, amended again 2026-08-25** (`docs/heap-and-tuple.md` §4.1): where the id comes from is a per-**row** fact — the `INSERT` names it or omits it — and there is no key mode. `sys.tables.next_id` is a high-water mark on what has been placed; uniqueness follows from it with no page read for an omitted key and for a named key at or above it. A named key **below** the mark is btree-only, proved by the descent, and refused `OutOfRange` on a heap relation. Read §4.1 before relying on ordering — monotonicity is per-relation and now also per-*history*, never engine-wide, and `sys.tables.key_order` is the only truthful reading of it.
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
  limit it, with `docs/known-gaps.md` named as the full inventory. A tag
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

- **Storage** (`docs/heap-and-tuple.md` §8, `docs/page.md`,
  `docs/rule-fixed-length-tuple.md`): heap page split policy;
  `inline_cell_width` default; spilled-value size cap; prefix-inlining
  trigger; purge cadence; the 16 reserved Keystone bits; id reuse; I/O
  backend; whether invariant 3 is ever relaxed. (*Whether a heap relation
  may take a caller-named pk* was **closed 2026-08-25** — yes, at or above
  the high-water mark — and it turned out **not** to be the split policy,
  which is the framing this line carried: a heap never has to place a key
  that sorts inside a full page, because such a key is refused. The split
  policy above is untouched.)
- **Waystone** (`docs/waystone-concpets.md` §9): per-pattern retention and
  eviction; persistence class of trail pages; `arg_hash` collisions; decay
  and sampling; whether invariant 9 is ever amended.
- **Transactions & WAL** (`docs/txn.md` §9, `docs/wal.md` §15): trx-id
  wraparound; `kTrxIdBlockSize`; cross-core commit and recovery under a
  changed core count. (Page redo identity across streams was **ratified
  2026-08-24**: PL-B logged handoff with the PL-C stream stamp,
  `docs/spec-page-lsn-cross-stream.md` §9 — no longer open; PL-A reopens
  only if 2PC is ratified, by that decision.) (Undo retention was ratified and built 2026-08-19,
  `docs/workplan-undo-purge.md` — `SnapshotTooOld` surfacing reopens only
  with the declined byte-cap policy; UP4's mount-time reclaim of a
  previous run's pages stays open there.)
- **Protocol** (`docs/protocol.md`): frame/batch/timeout sizing;
  compression; flow control. (TLS §1, SCRAM §14 and statement-class
  authorization §14 all landed 2026-08-13; what remains open there is
  per-relation grants, gated on catalog recovery.)
- **Parser** (`docs/parser-v2.md`): statement-class ratification; slot-table
  cap; whether `kUnclassified` is production-legal.
- **Cabin** (`docs/feat-cabin.md` §11): budgets and caps; the `CABIN AUTO`
  threshold; pruning cadence; entry-set persistence; multi-column keys.
- **Join inner build** (`docs/spec-join-inner-build.md` §7, §8): cap
  scoping (statement vs step); the catalog-count decline; the peer-side
  build.
- **Aggregation** (`docs/feat-aggregate.md` §10): `aggregate_max_distinct`;
  `MIN`/`MAX` with `DISTINCT`; lifting `ORDER BY`/`HAVING`; pre-aggregation
  below a join is *rejected*, not open.
- **Cross-core** (`docs/crosscore.md` §9, `docs/sched.md` §10): the 2PC
  protocol and everything gated behind it (multi-range writes included);
  batch/credit/ring/extent sizing; the `ring_full` retry protocol;
  core-count changes; initial placement policy (`creating` | `rotate`);
  split/migrate policy and constants (the mover); auxiliary placement
  under a split relation (each §6a gate's owner); the id-block
  interleave default; the shared-structure access mechanism. (`CREATE
  INDEX` on a peer-owned relation is **decided and built 2026-08-25**:
  the owner builds, PW1c-6b complete, `docs/spec-ddl-transactional.md`
  §5e and `docs/crosscore.md` CC7's owner-builds exception.) **Placement
  policy now has a measured input** (2026-08-26,
  `bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md` §6): the
  rotation crossover is a **step at the first core to take a second
  session**, and past it rotation is negative at seven writer cores
  (0.51×). The decision is still the operator's; what is no longer open
  is the shape of the curve it would be made on.
- **Stride forest** (`docs/workplan-stride-forest.md` §2, §6): D1/D2's
  restatement against `main` — whether `STRIDE n` is opt-in on `BTREE` or
  the storage default flips (the operator's); cross-class read consistency
  (a scan is N stages reading N committed views); `stride_b`; the per-class
  mark's undefined cases and its exhaustion ceiling; `cores` at mount
  (refused on mismatch until `wal.md` §3's core-count decision).
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
