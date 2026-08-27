# Workplan — Stride forest: parallel ascending ingest on explicit-keyed btrees

Drafted 2026-08-25 against `main` at `7fb5492` (`git describe` pending a tag on
that tree). Source citations below are `path:line` on that commit. Lineage:
`docs/spec/crosscore.md` §6b named the problem (naive range ownership spreads reads
while leaving inserts single-core) and answered it with id-block-aligned
per-range chains gated on R3/R4; this plan answers the btree half of the same
problem without R3's directory, under the key-mode decision §2 records. It is
a sibling of `docs/inflight/in-progress/workplan-peer-writer.md`, and it consumes what that plan
built: PW1b's lease pattern, PW1c's write rights, PW1c-7's stamp-carried
ownership, PW2's anchor, PW5's listeners, and the 6b-2/6b-3 request/waiter
wire shape.

Nothing in this file is built. Every task row that lands must state its
worktree and cite its review, per the discipline the PW series set.
Overhead is not measured until the row that measures it (the v2 amendment).

§9 is the review. Its amendments that needed no operator decision were
applied 2026-08-25 on worktree `docs-stride-forest-plan`, each tagged
*(§9 n)* at the sentence it changed. D1/D2's restatement against `main`
and §6's cross-class read-consistency item are the operator's: marked,
not taken.

## 1. What this closes, and why it is the binding constraint

In an ascending-key workload every INSERT descends to the **rightmost leaf**
(`include/kds/storage/btree/btree.hpp:38-41` states it as the design), and
that leaf — and the core that owns its relation — is the single serialization
point for the whole relation's ingest. PW6/PW7 established that the peer
*write path* costs nothing beyond core 0's path at equal parallelism
(0.977× vs a 0.982× control), so the remaining reason a hot relation cannot
ingest on N cores is not the wire and not the scheduler: it is that one tree
has one tail and one owner. This plan makes an ascending bulk INSERT engage
N cores by construction.

The structural law that shapes every choice: **within one btree, two leaves
cannot cover overlapping key ranges** (`InternalView::ChildFor` routes one
key to one child; `min_key` is immutable, invariant 2). N concurrent
appenders therefore require N disjoint key regions — and with the engine no
longer issuing ids (§2), disjoint regions can only come from a **computable
partition of the key space**. The second law: a core may not fault another
core's pages (buffer-pool coherence, the ground CC7's dispatch-not-assertion
stance and 6b-4's `InitTableAccess`-skips-the-anchor behavior both stand on),
so every structure a core reads at bind or insert time must be its own.

## 2. Decisions taken (operator, 2026-08-25; remainder operator-delegated)

- **D1 (operator) — [OPERATOR: restate against `main`, §9 finding 1.**
  `e13ad71` deleted the key mode but kept heap relations as the `CREATE
  TABLE` default; "every user relation is `kExplicit`" no longer names
  anything on `main`.]** `kAssigned` is removed from the user surface. Every
  user relation is `kExplicit` and therefore btree-clustered
  (`include/kds/catalog/well_known.hpp:434`'s rule, now universal).
  Omitting the pk in INSERT stays legal: the engine issues the id (§2 D5).
  System relations keep their engine-issued heap-chain form; the enum
  survives for them (SF-V1 confirms the boundary).
- **D2 (operator) — [OPERATOR: restate with D1, §9 finding 1.** A default
  of 4 on every relation, with §4's refusals, switches off secondary
  indexes, FK/Cabin/assertion writes on peers and every by-value write at
  `cores > 1`; and D6 needs a btree per class, so the default cannot apply
  to a heap relation. The reading consistent with `main`: `STRIDE n` opt-in
  on `BTREE` relations, default 1 = today's relation.]** The partition is
  per relation, fixed at CREATE TABLE: `stride_n` stride classes, **default
  4**. A key belongs to class
  `(key / stride_b) mod stride_n`.
- **D3 (delegated, taken).** `stride_b` (the run length before the class
  advances) is **[OPEN: size]** — a config default measured by SF-B, not
  decided here. The placement arithmetic itself (`/ B mod N`) is fixed by
  this section; only the constant is open.
- **D4 (delegated, taken; corrected at §9 finding 4).** Class→core
  mapping is `(owner_core + class) mod cores`, fixed at CREATE TABLE and
  **persisted per class in its sub-anchor** with the creating `cores`
  (SF2), core 0 included. `stride_n = 1` is therefore exactly today's
  relation under today's placement (`rotate` kept); a bare `class mod
  cores` would have pinned every single-class relation to core 0. The
  first draft's "not persisted — runs correctly at `cores = 1` and at
  `cores = 8`" is **retracted**: only a stream's own stamp claims a page
  (`src/storage/device_page_store.cpp:501-506`; a foreign stamp waits for
  rule 6's acquisition restamp and `:275` refuses the write), so a class's
  pages are unreadable by any core but the one that stamped them, in
  either direction of a core-count change. A mount whose `cores` differs
  from the creating value is refused by name until `wal.md` §3's
  core-count decision lands (SF6). The asymmetry of core 0 carrying both
  the system role and a class is accepted and measured, not designed
  around, until a mover exists.
- **D5 (delegated, taken).** The omit path issues from the **arrival
  core's own classes**: the class chosen is one this core serves under D4,
  so an omitted-pk INSERT never ships. The class is the **lowest** one this
  core serves — one hot tail per core, never two *(§9 finding 10)*. A
  session's core is `SO_REUSEPORT`'s choice (PW6: a session cannot choose
  its core), so **one connection never spreads**: the headline is N
  connections on N cores, and SF-B(1)'s driver is built for that. Since
  `e13ad71` omitting the key is every relation's rule per row, so what D5
  adds is only *which* class issues. Uniqueness stays proved by the
  descent (`include/kds/catalog/catalog.hpp:544-556`), which makes the
  issuance optimistic-safe: a collision with a caller-supplied id is
  `AlreadyExists` at the leaf, retried with the class's next id.
- **D6 (delegated, taken).** Each stride class is a **complete, independent
  btree** owned by its core: own sub-anchor, own root, own leaves, own
  splits, all from the owner's lease, all own-stamped from birth (PL §9
  rule 4; `docs/inflight/in-progress/workplan-peer-writer.md` §8's growth-pages clause). No
  shared internal nodes exist, so no split ever writes a foreign page and
  no B-link machinery is needed.
- **D7 (delegated, taken).** Sub-anchors are **CREATE-fixed**: core 0
  formats `stride_n` anchor pages at CREATE TABLE and hands each off
  through the existing publish hook (flush → durable `PAGE_HANDOFF` →
  fault+write grant, `docs/inflight/in-progress/workplan-peer-writer.md` §8), with PW1c-7's
  demand path (`kRelationGrantRequest`) as the re-delivery for a class
  whose grant never arrived or did not survive a restart. Each class's
  first INSERT formats its root leaf from its **own** lease and records it
  in its **own** sub-anchor — a local, logged `ANCHOR_UPDATE`. Root moves
  stay local forever after (PW2's property, per class).
- **D8 (delegated, taken).** The per-relation high-water mark moves into
  the sub-anchor, per class (§5 SF3): `AdmitExplicitRowId`'s mark
  advancement is a catalog write on core 0 today
  (`include/kds/catalog/catalog.hpp:558-567` — above-the-mark moves it,
  and only core 0 writes catalog pages), which would put a core-0 write on
  every peer's ascending INSERT — exactly the PW-B2 class of defect.
  Per-class marks in owner-written pages take the catalog off the insert
  path entirely; `sys.tables.next_id` becomes a CREATE-time base, and K4's
  budget / `SHOW BUDGET` read the max over class marks (SF-V1 confirms the
  read sites).

## 3. What a statement does under this plan

- **INSERT, pk omitted** (the fast path): arrival core picks its next id in
  a class it serves, descends its own subtree, appends. No ring message on
  the row path at all — no lease, no ship, no catalog. Multi-row VALUES:
  all ids drawn from one local class, so the statement is single-core and
  atomic under the ordinary local transaction.
- **INSERT, pk supplied, single row**: `f(key)` names the class; if this
  core serves it, local; otherwise the **whole statement ships** to the
  serving core (SF4) and the reply carries the result — the
  `IndexBuildClient` parked-waiter shape (6b-2/6b-3), not a page-level
  mechanism. Retryable refusals keep their wire bit (the PW6 finding (2)
  fix is assumed landed or is absorbed into SF4).
- **INSERT, pk supplied, multi-row spanning classes**: **refused
  `Unsupported`, naming R6, with the byte of the first foreign key** —
  deterministic, so never the wire's retryable bit, which stays for lease
  and window races *(§9 finding 9)*. Atomicity across two cores is a
  multi-core transaction, which is 2PC's door and stays closed (CC3's
  residue). A batch whose keys all fall in one class (which
  `stride_b`-aligned loaders can arrange) ships whole and works today. A
  statement that **mixes arities** — some rows named, some omitted, legal
  per row since `e13ad71` — is judged by its named rows alone: all local,
  or refused the same way; its omitted rows draw from the arrival core's
  class as the fast path does *(§9 finding 10)*.
- **Explicit transaction (BEGIN…COMMIT) writing multiple classes**: refused
  `Unsupported` at the first statement that names a foreign class, same
  ground, same wording; the transaction is not poisoned.
- **UPDATE / DELETE by pk**: `f(key)` names the class — local, or the whole
  statement ships as the single-row INSERT does (SF4). **UPDATE / DELETE by
  value** (a non-pk `WHERE`) touches every class and is a multi-core write:
  refused `Unsupported` naming R6 *(§9 finding 2)*. Under D2's default that
  is every by-value write on every relation at `cores > 1` — the cost the
  operator's restatement of D1/D2 weighs.
- **Point SELECT**: `f(key)` → one class → local or shipped read (P4's
  existing path; `CheckReadAffinity` learns keys, SF5).
- **DESCRIBE / SHOW BUDGET / `kds_tables`**: today these fault the anchor
  and read `next_id` on the session's core with no affinity check
  (`command_dispatcher.cpp:1135-1144`, `:872-926`); under stride they
  render per-class figures gathered over the ring (SF3), never a foreign
  fault *(§8.0 C2, C8)*.
- **Range/full scan**: N subtree scans, each local to its owner, merged at
  the session core — a merge, not a sort, since every subtree is
  key-ordered. Runs on P4's pipeline; SF-V2 verifies the N-producer shape.
  Each producer reads its class core's committed view (CC4) and nothing
  today makes the N views one snapshot of the relation — §6's `[OPEN]`
  *(§9 finding 3)*.
- **ORDER BY pk + LIMIT**: the merge consumes lazily; `STEP_CANCEL`
  upstream at the LIMIT-th row (`docs/spec/crosscore.md` §7). Under a stride
  partition the first `stride_b` keys sit in one class, so a `LIMIT` below
  `stride_b` drains one producer and cancels the others after their first
  batch — not ~LIMIT/N each *(§9 finding 10)*.

## 4. Deliberately out of scope, stated so nothing assumes otherwise

- **Secondary indexes on stride relations are refused at CREATE INDEX.**
  PW1c-6b's soundness argument is single-owner: every index page is *the*
  owner's own-stamped and maintenance is a local write. N writer classes
  maintaining one index tree is the two-writer route. The sound extension
  is a per-class index forest (each class indexes its own rows), and that
  is its own plan once this one has a number. Until then the refusal names
  this file. FK-linked, cabined and assertion-covered relations stay gated
  exactly as PW1c-5's shape gate has them.
- **No mover, no rebalance.** `stride_n` is CREATE-fixed; a mis-sized
  relation is recreated. The R5 mover's stride story (re-classing = moving
  every Nth key) is noted as hostile to page-boundary migration (CC10) and
  left to R5.
- **No migration of existing stores.** SF1 bumps the format; a pre-stride
  store is refused at mount by version. (Recommended over a converter: the
  engine is pre-release and the converter would outlive its one use.)
- **Waystone and access statistics on a stride relation are partial by
  construction.** A trail entry is a raw page id with no class
  (`include/kds/stats/waystone.hpp:118-159`); a foreign-class entry
  replayed on the session core is a refused fault — a miss under
  invariant 8 — but under CC7's extent-superset assertion it could read a
  page another core is writing, so `TrailReplay::Build` drops entries
  whose page this core does not own. Peers record nothing
  (`core_runtime.cpp:219-221`), so trail and `sys.access_stats` coverage
  of a stride relation is `1/N`, and a results file says so *(§8.0 C7)*.
- **A schema that can spill is refused `STRIDE n > 1`** until a per-class
  var-heap is designed — the var-heap is one chain per relation *(§8.0
  C6)*.
- **R3's `sys.ranges` is not built, consulted, or blocked on.** The
  partition function is the directory. If R3 lands later for read
  placement, stride relations opt out of range splitting (their "ranges"
  are interleaved by construction).

## 5. Task series

| # | Task | Gate |
|---|---|---|
| SF-V1 | **The id-issuance and mark census, verify before build** *(scope revised at §9 finding 11: no `KeyMode` and no reader of one exists since `e13ad71`)*. Every reader/writer of `sys.tables.next_id` and every issuer: `AllocateRowId`'s call sites (`catalog.hpp:532`), `AllocateRowIdRange`'s sorted fill, now gated per statement (`command_dispatcher.cpp:3340-3392`), `AdmitExplicitRowId`'s two catalog writes — the mark and the once-ever `key_order` flip (`catalog.cpp:2114-2225`) — and `InsertOneRow`'s per-row peer refusal of a named key (`command_dispatcher.cpp:3563-3571`), the row-id lease funnel (`row_id_lease_service`, PW1b) and its PW7 instrumentation, K4's budget and `DESCRIBE`/`SHOW BUDGET` reads of `next_id`, `key_order`'s readers (`step_compiler.cpp:1856-1858` sets `emit_in_key_order` from it; `MarkKeysUnordered`'s cross-core publish), `FindSlotForId`'s fallback cost sites (`btree.cpp:132-138`), and the catalog's own self-hosted relations (heap, engine-issued — confirm nothing user-facing shares their path). Deliverable: the census in this file's §8, each site tagged keep/retire/route | none |
| SF-V2 | **The one-root assumption census.** Everything that believes a relation has one root/one tree: `InitTableAccess` (`schema.hpp:227-230`'s `anchor_page_id`), the planner's step shapes, `ANALYZE`, `SHOW INDEXES`/`SHOW` walkers, assertion build/recover, cabin optimizer walks, 6b's foreign-arm reads. The Waystone recorder/replayer and the JB inner build over a shipped stream join the list *(§9 finding 5)*. And the P4 pipeline's producer arity: `PipelineTag` carries one `step_id` (`include/kds/server/step_pipeline.hpp:38-44`), so whether one consumer step can take N producer stages today or R3's "pipeline over ranges" work is prerequisite is answered there — this single answer sizes SF5 | none |
| SF-V0 | **The premise probe, before any build row** *(§9 finding 6; CLAUDE.md: re-measure a premise before building the fix)*. `bench/v2.0.0/results-multicore-writers-v2.0.0-48-g314a06d.md` §7's fdatasync-on-a-second-file probe and PW6's driver at `--cores 3 --tables 2` on a ≥3-CPU host: whether two writer cores can overlap their syncs on one ext4 volume decides whether SF-B(1) can exceed 1× at all. Needs no stride code. No multi-writer-core speedup has been measured anywhere yet (PW6: unmeasurable on the 2-CPU host, which cannot run this row either); a build row does not start on an unmeasured premise | none |
| SF1 | **Catalog half.** `sys.tables` grows `stride_n` (u16, 0 = non-stride legacy/system; format bump, mount refuses old versions); CREATE TABLE accepts `STRIDE n` — its default and whether it is `BTREE`-only or flips the storage default is D1/D2's restatement **[OPERATOR, §9 finding 1]** — and refuses it on a heap-clustered relation naming D6 (a class needs a descent). The key-mode surface is already gone (`e13ad71`: `ASSIGNED` refused at parse with its byte, `EXPLICIT` vacuous, `default_key_mode` refused by name), so nothing is removed here *(§9 finding 11)*; `AllocateRowId` and PW1b's lease funnel stay the omit path's source, re-homed per class by SF3. `TableAccess` carries `stride_n` (cacheable: CREATE-fixed, no ALTER) | SF-V1, SF-V0 |
| SF2 | **Sub-anchor plumbing.** Anchor format bump (one day old — bump now, never cheaper): the page gains `class u16` and `high_water u64` beside `clustered_root`; `nr_index` checked-redundancy discipline (`anchor_page.hpp:53-58`, the 3f07eda C1) applies to every new count. CREATE TABLE formats `stride_n` sub-anchors from core 0's map, publishes each to its D4 core through the §8 publish hook (payload extended past six slots or issued per class — the hook is already the one publisher, two callers); `sys.tables` stores the sub-anchor ids as a **fixed array** under a `stride_n` cap (`kMaxStrideClasses`, proposed 16: N sub-anchors, N producers and N grants each need a bound, and u16 is not one); the array rides the row-format bump `stride_n` needs anyway, and it deletes the contiguity option the first draft flagged *(§9 finding 12)*. Each sub-anchor also records its class→core assignment and the creating `cores` (D4), and its `high_water` seeds at `base + class·stride_b` — seeding every class from the base would make class 0's first id collide with it *(§8.0 C3)*. **The var-heap is one chain per relation** (`include/kds/storage/varheap.hpp:67`), so a schema that can spill is refused `STRIDE n > 1` until a per-class var-heap is designed; `RelationWriteGrantPayload` carries six page slots (`include/kds/server/extent_lease_service.hpp:67-73`), so grants are issued **per class** — root and sub-anchor — never as one relation-wide payload *(§8.0 C6)*. A `CREATE TABLE` rollback after the N handoffs leaves N own-stamped orphan pages on N cores — harmless by DROP TABLE's orphan precedent, named here. PW1c-7's demand path re-delivers a lost sub-anchor grant unchanged — a class's write refusal records `RelationGrantDemand` and the tick asks | SF1 |
| SF3 | **Per-class id issuance and the mark.** The omit path: a per-(relation, class) cursor on the owning core, seeded from the sub-anchor's `high_water`, issuing `base + k·(stride_b·stride_n)`-pattern ids inside the class's stripes; optimistic — the descent's `AlreadyExists` retries with the next stripe id. `AdmitExplicitRowId` re-homed: the mark it advances is the **class's** `high_water` in the class's own sub-anchor (owner-local, logged with `ANCHOR_UPDATE`), and it is advanced by the *serving* core during SF4's execution, never by the arrival core; `sys.tables.next_id` frozen at CREATE as the base. The cursor branches on `stride_n` **inside `Catalog::AllocateRowId`**, before the lease lookup: the function serves eight callers, seven self-hosted, two of which place rows through `heap::ChainInsert` and would refuse a striped id (`tests/keystone_id_test.cpp:164` is the tripwire) *(§8.0 C1)*. **The durable mark is logged ahead by a stripe, not per row** *(§9 finding 7)*: per-row logging would make the sub-anchor the hottest page of every class (a WAL record and a dirty page per insert); K3 makes a burned id free, so the record carries `cursor + stride_b` and the in-memory cursor walks under it — `RowIdLeaseTable`'s block shape, with `docs/rules/keystoneid-k0-findings.md` owning the crash rule (a logged-ahead ceiling never reissues). This is a **WAL change**: `AnchorUpdatePayload{index_oid, root}` (`include/kds/wal/log_anchor_update.hpp:24`) carries no mark, so the payload widens or a `kAnchorMark` record joins it — listed here, not "no new record types". K4/`SHOW BUDGET`/`DESCRIBE`'s max-over-classes cannot fault another core's sub-anchor (§1's second law): served by per-class figures gathered over the ring at `SHOW` time, or reported per core *(§9 finding 8)* — four readers run off-owner today (`SHOW BUDGET`, `DESCRIBE`, the `kds_tables` view, the lease grant handler) and the arithmetic is `Σ issued_c` / `min_c remaining_c`, never max *(§8.0 C2)*. Catalog writes are off the insert path — state it in the header and pin it with a test that counts core-0 ring traffic during a peer's ascending burst: zero, on a new counter, since `SHOW META`'s refill block prints nothing for a cursor that never leases *(§8.0 C10)* | SF2 |
| SF4 | **Write routing.** `CheckWriteAffinity` on a stride relation stops asking `owner_core` and asks `f(key)`: serving class → admit; foreign class, single-row autocommit → **ship the statement whole** to the serving core over a new request/reply pair (the 6b-2/6b-3 shape: POD payload carrying the statement's bound row, parked waiter on the arrival core, deadline, `Status::FromWire`); foreign class inside an explicit transaction, or multi-row spanning classes → `Unsupported` naming R6 with the offending token's byte — deterministic, so never the retryable bit; that bit stays for lease and window races *(§9 finding 9)*. **The `key_order` flip needs its own route** *(§9 finding 7)*: a named key below the class's mark on a serving peer flips `sys.tables.key_order`, a core-0 catalog write with a version bump and peer notification (`catalog.cpp:2114-2225`) that `InsertOneRow` refuses on a peer today (`command_dispatcher.cpp:3563-3571`); once per relation ever, so the serving core requests it from core 0 through the same parked-waiter shape and places the row only after the bump is acknowledged — a stale `kAscending` on the session core is a wrong answer, not a lost optimization. UPDATE/DELETE by pk take the same fork; by-value forms are §3's R6 refusal *(§9 finding 2)*. The refusal/ship fork must sit **after** the shape gate so index/FK/cabin refusals keep their names. `cores = 1` short-circuits before `f(key)` — every class is local — and the single-core benchmark must not move (guideline 2's test) | SF1, SF3; PW5 for multi-listener reality |
| SF5 | **Read routing and the merge — the largest row** *(§9 finding 5)*. Today's shipped read admits only a star, non-aggregated, non-sorted, no-`LIMIT` single step (`command_dispatcher.cpp:4682-4689`) and a projected two-stage join without sort, quota or aggregate (`session_step_client.cpp:198-213`); under stride every relation's rows are cross-core at `cores > 1`, so each exclusion must be lifted into the session core's merge-then-sink, and `emit_in_key_order` must travel in the descriptor (the wire-version bump the dispatcher's comment defers) because a `kUnordered` relation's producers must emit in key order for the merge to be a merge. Point/`kLookup`: `f(key)` narrows to one class before `CheckReadAffinity`, local or one shipped step (P4 as-is). Scan/`kRange`: plan N producer stages (one per class core, each walking its own subtree) into the session core's merge; merge is streaming k-way on the pk since each input is ordered — no buffering past one batch per producer; `ORDER BY pk` is the merge itself, `emit_in_key_order`'s per-page sort still applies within a page (kExplicit slots stay unordered in-page, `btree.cpp:132-138` — unchanged by this plan). **Sized by §8B.5's arity answer**: the producer count is fixed at one structurally in four places (`Pipeline::consumer`, `StepOpenUpstream`, `PipelineTag`, `RemoteRead::done`) and R3 is not a code prerequisite. N producers are **N genuine steps** — a distinct `step_id` per class minted at plan time, which `Find`, `FindByInputTag`, `EdgeCredit` and `InnerBuildStore` are already keyed for — plus N credit edges, N EOFs counted, an envelope of N enclosed opens with a defined partial-forward unwind, a streaming k-way merge sink at the session core (the pk forced into a projected layout's forwarded set), the descriptor-version bump, and the five exclusion lifts. The `ORDER BY <pk>` elision (`step_compiler.cpp:1832-1859`) becomes merge-conditional — on a stride relation without the merge it is a wrong answer; `CheckReadAffinity` learns the "some classes local, some foreign" verdict `InitTableAccess`'s own-relation-only anchor resolution (`catalog.cpp:1863`) cannot express; `kProbe` chooses its class per outer row at execution, a shape with no wire today *(§8.0 C4, C5, C8)* | SF-V2, SF4 |
| SF6 | **Restart and recovery, proven not asserted.** No new record types beyond SF2/SF3's anchor fields — every subtree page is ordinary, own-stamped, stream-locally redone; PW1c-7's stamp claim re-admits each class's pages to its core after restart with nothing granted. The test matrix: a 4-class relation ingested from 2 cores, restarted, read whole and written again (the PW1c-7 restart test's shape, per class); a class whose sub-anchor grant is lost to a full ring re-demanded and re-published exactly once; a crash between a class's root-leaf format and its `ANCHOR_UPDATE` (the class re-formats — the orphan page is the known CREATE-loser shape, `spec-ddl-transactional` §5e's precedent). The post-recovery audit (`mount_recovery.cpp:139-156`) covers N sub-anchors and N roots, and K-M2a — the durable mark can fall behind the log after a crash (`tests/keystone_id_test.cpp:341`) — is inherited per class and stated *(§8.0 C9)* | SF2-SF4 |
| SF-B | **The number.** `tools/multicore_benchmark.py` grows `--stride`: (1) omit-mode ascending bulk ingest, `stride_n = 4`, cores ∈ {1, 2, host-max} vs the single-core and PW6 baselines — the headline; (2) explicit-id ascending single-row stream at one arrival core — the shipped case, priced against (1); (3) the `stride_b` sweep — per-row shipping vs batch-amortized shipping (P4d-4c's batch runner becomes load-bearing here if (2) matters); (4) the scan/merge cost vs a single-tree scan of equal rows; (5) point-SELECT p50 beside 3 writers (PW7's 48 µs figure, now under stride). (6) core 0's per-extent free-map fsync (PW3b, `250cd3b`) scales with the number of growing classes — a term in the model, read as core-0 CPU in (1) *(§9 finding 12)*. `build-release`, `git describe --tags` stamped, on a ≥3-CPU host per PW6's bound; SF-V0's probe result gates whether (1) can exceed 1× on one ext4 volume at all and is quoted first | SF-V0, SF1-SF6 |
| SF7 | **Docs.** `docs/spec/heap-and-tuple.md` §4.1's **third** amendment (the second removed the key mode, `e13ad71`): monotonicity is per-class (per-relation → per-range was §6b's concession; this is that concession made real, loudly), and `docs/rules/keystoneid-invariant.md`'s 2026-08-25 "one monotone mark" paragraph restated per class *(§9 finding 11)*; `docs/spec/crosscore.md` §5's cross-class read consistency written as §6 decides it *(§9 finding 3)*; `docs/spec/crosscore.md` §6b rewritten to name this plan and drop its R3/R4 coupling for btrees; CC3's cell gains the routing form (a foreign-class write is shipped or refused, never silently wrong); CC7 gains the sub-anchor as a publish-hook consumer; `docs/inflight/known-gaps.md` records the secondary-index refusal and the R6 boundary; `CLAUDE.md` open decisions updated (`stride_b` [OPEN], mover×stride noted for R5) | SF1-SF6 |

## 6. Open constants and flagged points

- **[OPEN: `stride_b`]** — SF-B(3) decides. The prior is: large enough that
  a loader's natural batch stays in one class (amortized shipping), small
  enough that a single hot writer still spreads within one bulk statement's
  lifetime. No number is written before the sweep.
- ~~**[FLAG: sub-anchor id contiguity]**~~ — closed at §9 finding 12: the
  fixed array under `kMaxStrideClasses` removes the placement-arithmetic
  point (SF2).
- **[OPEN: cross-class read consistency — the operator's, §9 finding 3]** —
  a scan is N stages reading N committed views (`docs/spec/crosscore.md` §5); the
  one-view-per-(statement, core) rule is unbuilt and per core even when
  built, so two sequential autocommits from one client can land in two
  classes and be observed in the wrong order. Either accepted and written
  down (RC across classes, in §3 and SF7) or gated on the cross-core commit
  oracle DT9 and R6 wait on. Not taken here.
- **[OPEN: the per-class mark's undefined cases]** — a named key below
  the relation's base but above its own class's mark (the one test
  `AdmitExplicitRowId` makes, `catalog.cpp:2137`, has no per-class
  answer), and the per-class exhaustion ceiling — `2^40/(stride_b·stride_n)`
  stripes, not ids — with K4's only enforcement point being
  `AllocateRowId`'s `OutOfRange` *(§8.0 C3)*.
- **[FLAG: `cores` at mount]** — a class's pages are readable only by the
  core that stamped them, so a relation runs at the `cores` it was created
  under until `wal.md` §3's core-count decision lands; D4 persists the value
  and SF6 refuses a mismatch by name *(§9 finding 4)*.
- **[FLAG: PW6 §7 fdatasync overlap]** — if two cores cannot overlap syncs
  on one volume, SF-B(1)'s ceiling is the I/O backend decision's, not this
  plan's; the probe runs before the matrix so the number is read correctly.

## 7. What this plan deliberately reuses, so the diff stays small

The publish hook (one publisher, two callers — becomes three), the
`PAGE_HANDOFF`/grant pair and the demand-tick re-delivery (PW1c-4/-7,
unchanged), stamp-carried restart ownership (PW1c-7, unchanged), the anchor
page and `ANCHOR_UPDATE` (PW2, format-bumped once), the parked-waiter
request/reply shape and `Status::FromWire` (6b-2/6b-3), P4's shipped read
step, PW7's scheduler floors and refill instrumentation (the `SHOW META`
lease counters grow class-cursor lines for free), and the shape gate's
refusal spelling. New machinery is exactly: the partition function, the
per-class cursor/mark, the statement-shipping pair, and the N-way merge.

## 8. Census results

SF-V1 and SF-V2 ran 2026-08-25 on worktree `docs-stride-forest-plan` at
`952bbb9`, as two read-only surveys; every `path:line` in §8A and §8B is
on that commit. §8.0 is what they change in the rows above — each change
is applied there with a *(§8.0 Cn)* tag. §8A and §8B are the surveys as
delivered.

### 8.0 What the census changes

- **C1 — `AllocateRowId` is one function for eight callers** (§8A.2): seven
  are self-hosted, and two of them — `sys.pattern_defs`, `sys.assertions` —
  place rows through `heap::ChainInsert`, which refuses a striped id
  `OutOfRange` on the second stripe. SF3's cursor therefore branches on
  `stride_n` *inside* `Catalog::AllocateRowId`, before the lease lookup
  (which is already oid-blind); `tests/keystone_id_test.cpp:164` is the
  tripwire. Applied to SF3.
- **C2 — four `next_id` readers run off-owner today** (§8A.8(b)): `SHOW
  BUDGET`, `DESCRIBE`, the `kds_tables` view and the row-id lease grant
  handler (which *writes*), legal only by the catalog-range read exemption
  — which does not extend to sub-anchors. And "max over class marks" is the
  wrong arithmetic: issuance is `Σ issued_c`, remaining is `min_c
  remaining_c`. Applied to SF3 and §3; §9 finding 8 is sharpened, not
  changed.
- **C3 — the per-class mark has two undefined cases and one unstated
  bound** (§8A.1): the seed (class `c` seeds at `base + c·stride_b`, or
  class 0 collides with the base — applied to SF2); "below the mark" per
  class (a key below the relation's base but above its class's mark has
  no answer — §6 `[OPEN]`); and the per-class exhaustion ceiling
  (`2^40/(stride_b·stride_n)` stripes, not ids; K4's only enforcement
  point is `AllocateRowId`'s `OutOfRange` — §6 `[OPEN]`).
- **C4 — the producer count is fixed at one structurally in four places**
  (§8B.5): `Pipeline::consumer` is an `optional`, `StepOpenUpstream` names
  one upstream, `PipelineTag` has no discriminator, `RemoteRead::done` is
  set by the first EOF. R3 is **not** a code prerequisite. The
  recommended discriminator is a distinct `step_id` per class minted at
  plan time — N genuine steps, which `Find`, `FindByInputTag`, `EdgeCredit`
  and `InnerBuildStore` are already keyed for. The `ORDER BY <pk>` elision
  is a wrong answer on a stride relation until the merge exists. Applied
  to SF5.
- **C5 — `InitTableAccess` resolves the anchor only for own relations**
  (`catalog.cpp:1863`, §8B.1b): under stride the session core walks some
  classes locally and ships the rest — the "some local, some foreign"
  verdict `CheckReadAffinity` cannot express. Applied to SF5.
- **C6 — the var-heap is one chain per relation** (`varheap.hpp:67`,
  §8B.4): N classes appending to it is the two-writer route, so a schema
  that can spill is refused `STRIDE n > 1` until a per-class var-heap is
  designed; and `RelationWriteGrantPayload` carries six page slots, so
  grants are issued per class. Applied to SF2 and §4.
- **C7 — Waystone entries carry raw page ids with no class** (§8B.2g): a
  foreign-class entry replayed on the session core is a refused fault (a
  miss under invariant 8), but under CC7's extent-superset assertion it
  could read a page another core is writing; and peers record nothing, so
  trail and `sys.access_stats` coverage of a stride relation is `1/N`.
  Applied to §4.
- **C8 — `DESCRIBE` faults the anchor directly** (`command_dispatcher.cpp:
  1135-1144`) — the first read-only surface to break §1's second law; and
  `kProbe` chooses its class per outer row at execution, a shape with no
  wire today. Applied to §3 and SF5.
- **C9 — SF6 inherits K-M2a per class**: the durable mark can fall behind
  the log after a crash (`tests/keystone_id_test.cpp:341`), and the
  post-recovery audit covers N sub-anchors. Applied to SF6.
- **C10 — `SHOW META` has no counter for SF3's zero-traffic test**: the
  refill block prints nothing for a cursor that never leases. Applied to
  SF3.

### 8A. SF-V1 — id issuance and the mark

Surveyed at `952bbb9` (worktree `docs-stride-forest-plan`), against the post-`e13ad71` tree: there is no `KeyMode`; `KeyOrder {kAscending,kUnordered}` occupies its byte and is flipped once ever by `AdmitExplicitRowId`. Every `path:line` below is on `952bbb9`.

**Tag legend.** `KEEP` — survives unchanged. `RETIRE` — goes away under the plan. `ROUTE` — must be re-homed to the owning class's core / sub-anchor, or served over the ring. `DECIDE` — the plan has not made the decision this site needs. Where a site splits by relation kind the primary tag is given and the split is in the note.

---

#### 8A.1 `sys.tables.next_id` / `SysTableRow::next_id`

##### 8A.1a Writers — every one of them

| file:line | what | tag | note |
|---|---|---|---|
| `src/catalog/catalog.cpp:1102` | `CreateTable` seeds `row.next_id = kFirstRowId` (via `InsertRelationRow`) | KEEP | This is exactly D8's "CREATE-time base". SF2 must additionally seed each sub-anchor's `high_water`; **unstated**: whether all N classes seed from the same base or from `base + class·stride_b`. Since D5 issues `base + k·(stride_b·stride_n)` ids, seeding all N from `kFirstRowId` makes class 0's first id collide with the base. → also DECIDE. |
| `src/catalog/catalog.cpp:2041` | `AllocateRowIdRange`: `row.next_id = first + count`, the single carve write | ROUTE | The only write on the bulk/lease path. For `stride_n > 1` a carve must come from the class's `high_water`; for `stride_n ∈ {0,1}` and system relations it stays core 0's catalog write. |
| `src/catalog/catalog.cpp:2094` | `AllocateRowId`: `row.next_id = id + 1`, one durable catalog write per issued id | RETIRE (user) / KEEP (system) | The omit path becomes SF3's per-(relation, class) cursor. The bump stays for the self-hosted relations of §8A.6c. |
| `src/catalog/catalog.cpp:2196` | `AdmitExplicitRowId`, at-or-above branch: `row.next_id = id + 1` + `OverwriteLogged` | ROUTE | **This is D8's target.** One core-0 catalog write per ascending named key, on the peer's insert path — the PW-B2 defect the plan names. Becomes the class's `high_water` in the class's own sub-anchor. |
| `src/catalog/catalog.cpp:2171-2177` | `AdmitExplicitRowId`, below-mark branch: `row.key_order = kUnordered` + `OverwriteLogged` — a **second, distinct** catalog write a named key can trigger | ROUTE | Finding 7's "grown by `e13ad71`". D8 re-homes only the mark; this one needs its own route (§8A.4). Guarded on the current value, so once per relation ever. |
| `src/catalog/rows.cpp:56` | `SysTableRow::Encode` writes `next_id` at `kNextIdOffset` | KEEP | Format unchanged; SF1's `stride_n` column appends after `anchor_page_id`. |
| `src/catalog/catalog.cpp:222-243` (`OverwriteLogged`) | the logging envelope every `next_id` bump rides — `kNoTxnId` envelope, ordinary `HEAP_OVERWRITE` of the catalog page | ROUTE | The mark's move becomes an `ANCHOR_UPDATE` on an owner-written page instead. Note the B1 rule preserved here (`env_trx == kBootstrapXid → kNoTxnId`) must carry over. |
| `include/kds/wal/log_anchor_update.hpp:21-27` | `LogAnchorUpdate` encodes `AnchorUpdatePayload{index_oid, root}` — **no mark field** | ROUTE | Finding 7 confirmed in code: §7's "no new record types" is false. Either widen this payload or add a record. `include/kds/storage/anchor_page.hpp:19-20,36-42` is the page side (`clustered_root u32`, `nr_index u16`, entries at 12 B each) — the `class u16` + `high_water u64` SF2 wants have no bytes reserved. |

**There is no other writer.** Nothing in `src/wal/`, `src/txn/`, `src/server/mount_recovery.cpp` or bootstrap writes `next_id`; the field is redone only as a side effect of ordinary catalog-page redo. **There is also no mount-time or recovery-time check of `next_id`** — the post-recovery audit (`src/server/mount_recovery.cpp:119`) reads the row for `desc_page_id`/`varheap_page_id` and never looks at the sequence. `docs/rules/keystoneid-invariant.md:344` and `tests/keystone_id_test.cpp:341` record that the durable mark can fall *behind* the log after a crash (K-M2a, open). SF6 inherits that gap per class and should say so.

##### 8A.1b Readers — every one of them

| file:line | what | tag | note |
|---|---|---|---|
| `src/catalog/catalog.cpp:2033` | `AllocateRowIdRange` exhaustion: `row.next_id > kMaxKeystoneId - (count-1)`, refuse whole | DECIDE | A class's ceiling is not `kMaxKeystoneId`: a striped cursor issuing `base + k·(stride_b·stride_n)` exhausts the 40-bit field after `2^40/(stride_b·stride_n)` *stripes*, not ids. The plan never states a per-class budget. |
| `src/catalog/catalog.cpp:2036` | `first = row.next_id` — the carve's first id | ROUTE | Same as the write above. |
| `src/catalog/catalog.cpp:2081-2082` | `AllocateRowId`: `id = row.next_id`, then `id > kMaxKeystoneId → OutOfRange` | RETIRE (user) / KEEP (system) | The exhaustion refusal is the only enforcement point of K4 (`tests/keystone_id_test.cpp:194`). Its per-class replacement must exist before this is retired. |
| `src/catalog/catalog.cpp:2137` | `AdmitExplicitRowId`: `if (id < row.next_id)` — the one test that decides heap `OutOfRange` vs btree admit-and-flip | ROUTE + DECIDE | **The hardest single site.** "Below the mark" becomes per class, and the plan does not say which mark a key is measured against: a key below the relation's base but above its own class's `high_water` is a *new* case with no answer. It is also the reason SF4 must ship the row before this runs — the serving core owns the mark. |
| `src/catalog/catalog.cpp:2153`, `:2183`, `:2204` | `next_id` inlined into three operator-facing messages (heap refusal, the flip trace, the admit trace) | KEEP | Must name the class, or the message points at the wrong number. |
| `src/server/command_dispatcher.cpp:906` | **SHOW BUDGET**: `catalog::BudgetOf(table_row.value().next_id)` per relation, in a loop over `ListTables()` | ROUTE | Finding 8, in code. Runs on the session's core for *every* relation including ones it does not own (there is no affinity check on this path — see (b) below). |
| `src/server/command_dispatcher.cpp:1155` | **DESCRIBE**: `<< " next_id=" << table_row.value().next_id` | ROUTE | Same. `tests/command_dispatcher_test.cpp` and `tests/keystone_budget_test.cpp:143` pin the token. |
| `src/server/command_dispatcher.cpp:1167` | **DESCRIBE**: K4 budget, `BudgetOf(next_id)` → `ids_issued`/`ids_remaining`/`budget_used` | ROUTE | Same. |
| `src/exec/catalog_view.cpp:54`, `:72` | `kds_tables` catalog view declares and emits a `next_id` column | ROUTE | A *SELECT* surface, so it runs wherever the session is, and it is not covered by `CheckReadAffinity` (it is a view, not a step chain). Under stride it renders a base, not an issuance count. |
| `src/catalog/keystone_budget.cpp:5-31` | `BudgetOf`: `issued = next_id - kFirstRowId`, `remaining = kMaxKeystoneId - next_id + 1`, warn/exhausted flags | KEEP + DECIDE | The arithmetic is pure and the header (`include/kds/catalog/keystone_budget.hpp:29-31`) already says only the *source* moves. But **"max over class marks" is the wrong arithmetic**: with striped ids the max mark massively overstates issuance. The honest figure is `Σ issued_c` over classes, and `remaining` is `min_c remaining_c`. SF3 must state which; the plan said "max". |
| `src/server/row_id_lease_service.cpp:33-40` | the grant handler on core 0 reads/bumps `next_id` through `AllocateRowIdRange` **for a relation core 0 does not own** | ROUTE | See (b). |
| `src/catalog/catalog.cpp:1813` (`InitTableAccess` → `GetSysTableRow`) | reads the whole row to build the cached `TableAccess` | KEEP | Must gain `stride_n` (SF1: cacheable, CREATE-fixed) and must **not** gain the marks — they are not cacheable and they are foreign pages. |
| `src/server/mount_recovery.cpp:112-119` | post-recovery audit reads `GetSysTableRow` **deliberately uncached** because "`next_id` forbids caching" | KEEP | The comment's reason survives; SF6 should extend the audit to the N sub-anchors. |
| `src/server/index_build_service.cpp:128`, `src/server/relation_grant_service.cpp:38`, `src/server/command_dispatcher.cpp:1373`, `:1408` | `GetSysTableRow` for `owner_core` only | KEEP | Not mark readers, but they are the "the row is the authority on ownership" sites SF4 changes the meaning of (`f(key)` replaces `owner_core` for placement). SF-V2's territory. |
| `include/kds/catalog/catalog_cache.hpp:35-45` | the rule: `next_id` is never cached, therefore `GetSysTableRow()`/`AllocateRowId()` always read the page, and `AllocateRowId()` bumps no version | KEEP | The rule must be restated to cover per-class marks; a cached mark is the same hazard. |
| `include/kds/catalog/catalog.hpp:72-74`, `:355`, `:480-483`, `:511-513`, `:524`, `:551`, `:567`, `:1024` | the header's contract prose for the mark | KEEP | Rewrite, not delete: `:567` ("a ceiling on what has been placed… DESCRIBE and SHOW BUDGET both read it") is the sentence findings 7/8 contradict. |
| `include/kds/catalog/rows.hpp:71`, `:143` (`kNextIdOffset`) | the field and its on-disk offset | KEEP | Meaning changes to "CREATE-time base"; bytes do not. |
| `include/kds/storage/heap/heap_chain.hpp:57`, `include/kds/server/superblock.hpp:219`, `include/kds/txn/trx_id.hpp:35,51`, `include/kds/txn/undo_page.hpp:236`, `include/kds/storage/keystone.hpp:20`, `include/kds/catalog/well_known.hpp:428`, `include/kds/server/command_dispatcher.hpp:358,398`, `include/kds/exec/row_codec.hpp:182` | prose that cites `next_id` as *the* per-relation sequence | KEEP | Doc-level corrections; SF7's list. `command_dispatcher.hpp:398` still says "the pk is not supplied", which `e13ad71` already falsified. |

---

#### 8A.2 `AllocateRowId` / `AllocateRowIdRange` / `AdmitExplicitRowId` — every caller

`AllocateRowId` has **eight** call sites in `src/` today, not the three the old comment claims (`tests/keystone_id_test.cpp:165` still says three).

| file:line | statement path served | tag | note |
|---|---|---|---|
| `src/server/command_dispatcher.cpp:3579` | **single-row / per-row INSERT, pk omitted** (`InsertOneRow`'s else arm; `explicit_key` decided at `:3487`) | ROUTE | D5's fast path. Becomes the arrival core's own class cursor. This is the one caller that is "wrong" in the plan's sense. |
| `src/server/command_dispatcher.cpp:3574` | **INSERT, pk supplied** — `AdmitExplicitRowId(oid, supplied_id)` | ROUTE | SF4 must ship the whole statement before this runs; today a peer refuses it two lines above (`:3567`, `catalog_read_only_`) precisely because it is a core-0 catalog write. |
| `src/server/command_dispatcher.cpp:3392` | **multi-row VALUES sorted fill** — `AllocateRowIdRange(oid, stmt.rows.size())`, gated by `:3294-3298` (`every_row_omits_pk`) + `SortedFillEligible` (`:3332-3348`: not `catalog_read_only_`, heap-clustered, no varheap, no indexes, no cabins, no assertions) | KEEP | Heap-only and core-0-only, so a stride (btree) relation never reaches it. Under finding 1's restatement (`STRIDE` opt-in on `BTREE`) it stays alive for heap relations untouched. The per-statement half of the gate (`:3295`) is the "fifth reach point" the plan cites and is already per-row, not per-relation. |
| `src/server/row_id_lease_service.cpp:33` | **the row-id lease funnel** — core 0 carves a peer's block | KEEP (system/legacy) / RETIRE (stride) | Stays the omit path's source for non-stride relations. |
| `src/catalog/catalog.cpp:2380` | `RegisterPattern` — `AllocateRowId(kSysPatternsTable)` for a sys.patterns **oid** (a body field, not a Keystone word) | KEEP | Self-hosted; see (c). |
| `src/catalog/catalog.cpp:2625` | `CreateCabin` — `AllocateRowId(kSysCabinsTable)` for `cabin_id` | KEEP | Self-hosted. |
| `src/catalog/catalog.cpp:2729` | `CreateForeignKey` — `AllocateRowId(kSysFkeysTable)` for `fk_id` | KEEP | Self-hosted. |
| `src/catalog/catalog.cpp:2917` | `Catalog::CreateIndex` — `AllocateRowId(kSysIndexesTable)` when the caller pre-issued none | KEEP | Self-hosted. |
| `src/exec/index_ddl.cpp:286` | **CREATE INDEX** — `PrepareIndexDef` pre-issues the index oid *before any page exists*, so root and split pages carry their owner from birth (page.md §2a) | KEEP | §4 refuses CREATE INDEX on stride relations, so this path is untouched — but only if finding 1's restatement lands. Under D1-as-written it is refused everywhere. |
| `src/exec/assertion_catalog.cpp:411` | **CREATE ASSERTION** — `AllocateRowId(kSysAssertionsTable)` before the Bound Cabin build (`ASSERT_BUILD` records carry it) | KEEP | Self-hosted, and a **heap chain** — see (c). |
| `src/stats/pattern_defs.cpp:171` | **CREATE PATTERN** — `AllocateRowId(kSysPatternDefsTable)`, a real Keystone id | KEEP | Self-hosted, and a **heap chain** — see (c). |
| `src/catalog/catalog.cpp:2059-2065` | `AllocateRowId`'s **lease branch**: `if (row_id_leases_ != nullptr) return row_id_leases_->Next(table_oid)` | RETIRE (stride) / KEEP | Oid-blind: it would answer for `kSysIndexesTable` as readily as for a user relation. Peers refuse DDL (`command_dispatcher.cpp:437-446`) so it is unreachable for system oids today, but SF3's cursor must branch on `stride_n` *before* this, not after. |

`AdmitExplicitRowId` has exactly **one** caller in `src/`: `command_dispatcher.cpp:3574`. `AllocateRowIdRange` has exactly **two**: `command_dispatcher.cpp:3392` and `row_id_lease_service.cpp:33`.

Declarations and contract prose: `include/kds/catalog/catalog.hpp:480-484` (range), `:505-532` (single), `:534-577` (admit). `include/kds/exec/assertion_catalog.hpp:134`, `include/kds/exec/index_ddl.hpp:65`, `include/kds/catalog/rows.hpp:389,713,812`, `include/kds/catalog/catalog.hpp:615-621` each document a self-hosted caller — all KEEP.

---

#### 8A.3 `RowIdLeaseTable`, `row_id_lease_service`, `LeaseRefillStats`

**How a peer obtains ids today, end to end:** `AllocateRowId` → `RowIdLeaseTable::Next` (`row_id_lease.hpp:83`) → on a spent/absent lease, *inserts the entry* and returns a retryable refusal → `CoreRuntime::MaybeRefillRowIds` (`core_runtime.cpp:815`) picks `NeediestRelation()` → `RequestRowIdLease` coroutine (`row_id_lease_service.cpp:112`) sends `kRowIdLease` to core 0 → core 0's handler (`row_id_lease_service.cpp:10-62`) carves via `AllocateRowIdRange` (4096 default) and replies → the peer's receiver (`:64-110`) applies `Grant`/`Deny` and releases the waiter. **The block is consumed at `row_id_lease.hpp:104` (`return lease.next++`) and nowhere else.**

| file:line | what | tag | note |
|---|---|---|---|
| `include/kds/catalog/row_id_lease.hpp:36-73` | `RowIdLease{next,end,window,denied}`, `spent()`, `remaining()`, `low_water()` (quarter-window) | KEEP | SF3's per-class cursor is *this shape* at a different layer — finding 7 says so explicitly. The `window` = run-in-hand rule (`:44-52`) is the arithmetic to copy, not re-derive. |
| `include/kds/catalog/row_id_lease.hpp:83-105` | `Next()`: hands out `lease.next++`; **records demand on a miss** | ROUTE | Becomes the per-(relation, class) cursor for stride relations; stays for legacy/system. |
| `include/kds/catalog/row_id_lease.hpp:108-116` | `NeediestRelation()`, oid-ordered for determinism | ROUTE | A class cursor is keyed on (oid, class); the map key and the stable-order rule both widen. |
| `include/kds/catalog/row_id_lease.hpp:118-136` | `Deny()` / `Grant()` (contiguous top-up extends, anything else replaces and burns) | KEEP | |
| `include/kds/catalog/row_id_lease.hpp:57-63` | `denied`'s comment still says "a relation whose **key mode** names its own ids" | RETIRE | Stale text from `e13ad71`; delete with SF1. |
| `src/server/row_id_lease_service.cpp:10-62` | core 0's grant handler; zero-count grant = "none" | KEEP | |
| `src/server/row_id_lease_service.cpp:64-110` | the peer's receiver; `Grant`, `Deny`, `stats.NoteGrant`, release | KEEP | |
| `src/server/row_id_lease_service.cpp:112-152` | `RequestRowIdLease` coroutine, one in flight per core | KEEP | |
| `include/kds/server/row_id_lease_service.hpp:27` | `kRowIdLeasePerGrant = 4096` (K-M2's measured floor) | KEEP | The prior for `stride_b`'s durable-advance chunk, per finding 7. |
| `include/kds/server/row_id_lease_service.hpp:31-42` | `RowIdLeaseRequestPayload{oid,count}` (16 B), `RowIdLeaseGrantPayload{oid,first,count}` (24 B), both `static_assert`ed | DECIDE | If a stride class ever leases (rather than owning its mark), these need a `class` field and the `static_assert`s move. The plan's D8 says it does not — state it. |
| `include/kds/sched/ring_message.hpp:91` (`kRowIdLease = 22`), `:138`, `src/sched/spsc_ring.cpp:34` (`"ROWID_LEASE"`) | the wire kind | KEEP | SF4's statement-shipping pair is a *new* kind beside it. |
| `src/server/core_runtime.cpp:183` | `SetRowIdLeases` — installed on peers only (`is_peer`) | ROUTE | Under stride every core owns classes, so the peer/core-0 asymmetry here is exactly what SF3 removes. |
| `src/server/core_runtime.cpp:345` | `RegisterRowIdGrantReceiver`, peers only | KEEP | |
| `src/server/core_runtime.cpp:755`, `:815-845` | the drain tick and the in-flight flag | ROUTE | |
| `src/server/expeditor.cpp:1418-1425` | core 0 installs `RegisterRowIdGrantHandler` in production | KEEP | |
| `src/server/core_runtime.cpp:237-241` | `set_lease_refill_stats(extent, trx_id, row_id)` | KEEP | §7 promises the `SHOW META` counters "grow class-cursor lines for free" — they do not, this is a fixed 3-pointer wiring. Small, but name it. |
| `include/kds/server/lease_refill_stats.hpp:28-30,52,59-60,68` | `LeaseRefillStats{requests, grants, …}` + the three-leg timing | KEEP | |
| `src/server/command_dispatcher.cpp:612-633` | **SHOW META**: `refill_block("rowid", row_id_refill_stats_)` prints `rowid_refill_requests`, `_grants`, `_wait_max_us`, `_submit_lag_max_us`, `_grant_lag_max_us`, `_resume_lag_max_us`, and three `_iters` variants. **Peers only** — null on core 0 | KEEP | This is the whole of what SHOW META says about row ids today. A per-class cursor that never leases prints *nothing*, so SF3's "count core-0 ring traffic during a peer's ascending burst: zero" has no existing counter to read; the test needs a new one. |

---

#### 8A.4 `KeyOrder` / `key_order` / `MarkKeysUnordered` / `emit_in_key_order`

##### 8A.4a The flag: set, publish, read

| file:line | what | tag | note |
|---|---|---|---|
| `include/kds/catalog/well_known.hpp:452-458` | `enum class KeyOrder {kAscending=0,kUnordered=1}` + `:424-450` the whole argument | KEEP | Survives `e13ad71`'s byte reuse; no format bump. |
| `include/kds/catalog/well_known.hpp:461-463` | `KeyOrderName` — the one spelling, so DESCRIBE and tests cannot drift | KEEP | |
| `include/kds/catalog/rows.hpp:121`, `:146` (`kKeyOrderOffset`) | the on-disk byte | KEEP | |
| `src/catalog/rows.cpp:59-60`, `:81-82` | encode/decode | KEEP | |
| `src/catalog/catalog.cpp:1108` | `CreateTable` sets `key_order = kAscending`; "nothing may pass this in" | KEEP | |
| `src/catalog/catalog.cpp:2170` | `if (row.key_order == kUnordered) return true;` — the once-ever guard | ROUTE | Read of a core-0 catalog page on a peer's insert path. |
| `src/catalog/catalog.cpp:2171-2177` | **the set site**: `row.key_order = kUnordered` + `OverwriteLogged` | ROUTE | Finding 7's second core-0 write. Needs its own route in SF3/SF4: a request to core 0 that *completes* — version bumped, peers notified — **before** the row is placed. |
| `src/catalog/catalog.cpp:2235-2239` | **the cross-core invalidation that publishes the flip**: `cache_.MarkKeysUnordered(oid)` (in place, local) **+** `++catalog_version_` **+** `on_invalidate_()` | ROUTE | The in-place/bump split is load-bearing and the comment at `:2207-2234` carries the whole argument (a `BumpVersion` here dangles the running INSERT's `const TableAccess*`). Under SF4 the flip runs on the *serving* core, which cannot do any of the three — all of it re-homes to core 0. |
| `src/server/expeditor.cpp:1446` | `SetInvalidationHook` on core 0; `:1044` broadcasts `kCatalogInvalidate` | KEEP | The delivery mechanism the flip rides. `include/kds/sched/ring_message.hpp:64` is the kind. |
| `src/catalog/catalog_cache.cpp:136-140` | `MarkKeysUnordered` implementation (no-op when uncached) | KEEP | |
| `include/kds/catalog/catalog_cache.hpp:170-188` | the fifth in-place updater and its one-way argument (no value parameter) | KEEP | |
| `include/kds/catalog/schema.hpp:211-234` | `TableAccess::key_order` — "the one cached field that is not a DDL fact"; stale here is a **wrong answer** | KEEP | Under stride this stays a per-*relation* fact even though the marks go per class. Worth stating in SF3 so nobody makes it per class by symmetry. |
| `src/catalog/catalog.cpp:1834` | `access.key_order = table_row.value().key_order` in `InitTableAccess` | KEEP | |
| `src/exec/step_compiler.cpp:1857-1859` | **read site 1**: `access->key_order == kUnordered → chain.steps[0].emit_in_key_order = true`, inside the `ORDER BY <pk>` elision (`:1831-1836`) | KEEP | Finding 5's narrowing, in code. Unchanged by this plan per SF5. |
| `src/exec/step_vm.cpp:1501` | **read site 2**: Cabin `WalkAndRecord`'s served-set ordering — `kUnordered` → sort by (page, slot); else sort by pk | KEEP | §4 keeps cabined relations gated on peers (`command_dispatcher.cpp:3037-3042`), so this stays core-local. |
| `src/server/command_dispatcher.cpp:1154` | **read site 3**: DESCRIBE prints `key_order=<ascending|unordered>` | KEEP | |
| `include/kds/exec/inner_build.hpp:70-79` | the InnerBuild bucket contract, correct "for either key order" | KEEP | |
| `include/kds/server/superblock.hpp:167-176` | the no-bump-on-2026-08-25 record | KEEP | SF1/SF2's `stride_n` + anchor format bump adds the next entry here. |

Read sites are exactly **three** in `src/` (`step_compiler.cpp:1857`, `step_vm.cpp:1501`, `command_dispatcher.cpp:1154`), plus the cache fill at `catalog.cpp:1834`. Set sites are exactly **two** (`catalog.cpp:1108` initial, `catalog.cpp:2171` the flip) plus the cache mirror at `catalog_cache.cpp:139`.

##### 8A.4b `emit_in_key_order`

| file:line | what | tag | note |
|---|---|---|---|
| `include/kds/exec/step_chain.hpp:442-458` | the field and its contract ("within one page only; pages stay key-ordered by `min_key`") | KEEP | SF5 says so explicitly. |
| `src/exec/step_compiler.cpp:1858` | the **only** set site | KEEP | |
| `src/exec/step_vm.cpp:1612`, `:1710-1747` | the per-page sort: collect `(id, slot)`, `std::sort`, mark page done, emit | KEEP | The N-way merge sits *above* this, per class. |
| `src/exec/step_vm.cpp:176-186` | the `WalkMark` "rows, not slots" rule, which depends on this flag | KEEP | |
| `src/server/command_dispatcher.cpp:4676-4690` | the single-step shipped read excludes `emit_in_key_order` (`!step.emit_in_key_order` at `:4689`) | DECIDE | Finding 5: under `stride_n > 1` every relation's rows are cross-core, so this exclusion plus `star()/!aggregated()/!sorted()/!limit` at `:4682-4685` decides how much of SF5 is actually free. Not a routing tweak. |
| `src/server/session_step_client.cpp:189-215` | the two-stage shipped form refuses `outer.emit_in_key_order || inner.emit_in_key_order` — "does not travel in the descriptor" | DECIDE | The deferred wire-version bump. A k-way merge needs each producer emitting in key order *once the flag is set*, so this refusal has to be lifted, not routed around. |

---

#### 8A.5 `FindSlotForId` and everything that assumes slot order == key order

| file:line | what | tag | note |
|---|---|---|---|
| `src/storage/btree/btree.cpp:122-142` | the comment that states the assumption and why the fallback exists | KEEP | |
| `src/storage/btree/btree.cpp:144-176` | `FindSlotForId`: binary search over slots (with dead-slot stepping) — the **optimization** | KEEP | Unchanged: within one class's leaf the same rule holds. A striped-but-ascending class keeps the binary search on its fast path. |
| `src/storage/btree/btree.cpp:172-179` | the **linear fallback** — "what makes the answer correct in every case"; cost is a wasted `log2(n)` probes | KEEP | This is the fallback cost site SF-V1 asks for. It is paid per lookup on an unsorted leaf, not per row inserted. |
| `src/storage/btree/btree.cpp:737` | `BtreeLookup` — the only `FindSlotForId` caller | KEEP | |
| `src/storage/btree/btree.cpp:626-645` | `BtreeInsert`'s duplicate check is **deliberately linear, not `FindSlotForId`** ("this check expects to find nothing") | KEEP | This is where D5's optimistic `AlreadyExists` retry lands. Note it is O(slots) per insert on every class. |
| `src/storage/btree/btree.cpp:111-121` | `MaxLiveId(leaf)` — full slot scan to decide append vs divide | KEEP | Per split, not per row. |
| `src/storage/btree/btree.cpp:660-670` | `id < MaxLiveId → SplitLeafAndInsert` (a genuine division) vs the append-split | KEEP | A per-class ascending stripe always takes the append arm — which is the plan's point. |
| `src/storage/btree/btree.cpp:188-213` (`SeparatorAboveEveryEntry`), `:215-...` (`DivideInternalNode`), `:334-356` | the full-internal-node promotion: append-split when `sep` sorts above every separator, real division otherwise | KEEP | D6 makes every class's tree independent, so no split ever writes a foreign page — this machinery is unchanged and stays local. |
| `src/storage/btree/btree.cpp:417-473` | `SplitLeafAndInsert`: collect live versions, `std::sort` by key, median split; `OutOfSpace` when fewer than two live tuples | KEEP | |
| `src/storage/btree/btree.cpp:613-621` | `id < leaf.min_key() → OutOfRange "the relation's id sequence has gone backwards"` | KEEP + DECIDE | Still correct per class. But the message names "the relation", and under stride the sequence that went backwards is a *class's* — reword, or an operator debugs the wrong thing. |
| `include/kds/storage/btree/btree.hpp:36-60` | "Invariant 10 makes every pk system-issued, monotonically increasing… the descent always ends at the **rightmost** leaf" | KEEP | Already falsified by `e13ad71`; SF7 must rewrite it per class. This is the paragraph §1 of the plan cites as "the design". |

---

#### 8A.6 Everything that assumes ONE monotone id sequence per relation

##### 8A.6a The heap path (`min_key` tail checks)

| file:line | what | tag | note |
|---|---|---|---|
| `src/storage/heap/heap_chain.cpp:94-104` | `ChainInsert`: `id < tail.min_key() → OutOfRange` — the tail-only legality check | KEEP | Heap relations are non-stride under finding 1's restatement. Under D1 **as written** there are no heap user relations and this serves system relations only. |
| `src/storage/heap/heap_chain.cpp:107-122` | the O(1)-pages duplicate check, sound **only** while every earlier page's ids sit below the tail's bound | KEEP | The single most fragile consumer of "one monotone sequence". Nothing striped may ever reach it. |
| `src/storage/heap/heap_chain.cpp:134-144` | growth: new page's `min_key` = this tuple's id, "since ids only increase from here" | KEEP | |
| `src/storage/heap/heap_chain.cpp:213-218` | `ChainAppendBatch`: `first_id < page.min_key() → OutOfRange` (the sorted-fill entry check) | KEEP | |
| `src/storage/heap/heap_chain.cpp:236-248` | batch growth, `min_key = first_id + i` | KEEP | |
| `include/kds/storage/heap/heap_chain.hpp:23-66`, `:129-130`, `:162-165` | the invariant prose: "ids are system-issued, unique and monotonically increasing per relation (invariant 10, `Catalog::AllocateRowId`)"; "a rolled-back or corrupted `sys.tables.next_id`" | RETIRE (prose) | Stale twice over — `e13ad71` already made ids caller-nameable, and stride makes "per relation" wrong. SF7. |

##### 8A.6b The planner, executor and comments

| file:line | what | tag | note |
|---|---|---|---|
| `src/exec/step_compiler.cpp:1808-1840` | **ORDER BY pk elision**: one ascending key on the driving relation's pk, driving step not `kCabinProbe` → `chain.sort_keys.clear()` | ROUTE | The premise "step 0 emits in pk order" is exactly what N producers break. SF5's merge is what re-establishes it; until then the elision on a stride relation is a wrong answer, not a lost optimization. |
| `src/exec/step_compiler.cpp:1831-1834` | `one_ascending_pk` / `driving_emits_pk_order` | ROUTE | |
| `src/exec/step_vm.cpp:1486-1508` | Cabin served-set ordering premise ("across pages by `min_key`, within a page because slots are appended") | KEEP | Core-local; cabined relations stay gated. |
| `src/server/command_dispatcher.cpp:3294-3299` | the sorted fill's `every_row_omits_pk` gate | KEEP | Already per-statement, already per-row-arity — the plan's "fifth reach point" is in a good state. |
| `src/server/command_dispatcher.cpp:3311-3320` | the bulk reply reports `first_id`/`last_id` and "**no contiguity is promised**" | KEEP | Fortunate: a per-class cursor makes a multi-row omit statement's ids a stripe, and this reply already refuses to promise otherwise. |
| `src/server/command_dispatcher.cpp:3480-3500` | `InsertOneRow`'s two-arity dispatch (`ncols` vs `ncols-1`) | KEEP | Finding 10's mixed-arity case lives here: one statement's rows may individually name or omit, so `f(key)` and "arrival core's class" can disagree row to row inside one `VALUES`. |
| `src/server/command_dispatcher.cpp:3559-3572` | the peer's per-row refusal of a caller-supplied key, naming the catalog write | RETIRE | SF4 replaces it with the ship. Also `:2963-2970` and `:449-458`, which document it. |
| `src/server/command_dispatcher.cpp:2946-2954` | `CheckWriteAffinity`'s `access.owner_core != core_id_` refusal | ROUTE | SF4's `f(key)`. |
| `src/server/command_dispatcher.cpp:5167`, `:5872` | `HandleUpdate` / `HandleDelete` pass the same `CheckWriteAffinity` | DECIDE | Finding 2: §3 now names the by-pk ship and the by-value R6 refusal. |
| `src/server/command_dispatcher.cpp:3164-3183` | `CheckReadAffinity` — every step's `owner_core != core_id_` → refuse | ROUTE | SF5. Note it does **not** cover `SHOW`/`DESCRIBE`/catalog views. |
| `include/kds/storage/keystone.hpp:19-23` | "the catalog issues it from the relation's `next_id` sequence… unique and monotonic by construction… Callers do not supply it (invariant 10)" | RETIRE (prose) | Stale since `e13ad71`. |
| `include/kds/exec/row_codec.hpp:180-184` | "`id` is the system-generated key (`Catalog::AllocateRowId`)" | RETIRE (prose) | Same. |
| `include/kds/catalog/catalog.hpp:349-357` | `GenerateUserOid`'s stated contract — "patterns, cabins, foreign keys and indexes each use their own persistent per-relation `next_id` sequence — so the contract holds today, and the scan is where to extend it if that changes" | KEEP | **Load-bearing for (c)**: this sentence is what a per-class cursor would falsify if it were applied to system relations. |
| `include/kds/storage/index/index_tree.hpp:24-27` | "a secondary key is not monotonic" | KEEP | Unrelated to stride; noted so it is not mistaken for a hit. |

##### 8A.6c The self-hosted relations, and where they place rows

| file:line | what | tag | note |
|---|---|---|---|
| `src/stats/pattern_defs.cpp:171` + `:197` | `AllocateRowId(kSysPatternDefsTable)` then **`heap::ChainInsert`** on `rel.desc_page_id` | KEEP — **must not stripe** | A striped id in this chain is `OutOfRange` at `heap_chain.cpp:99` on the second stripe. |
| `src/exec/assertion_catalog.cpp:411` + `:223` | `AllocateRowId(kSysAssertionsTable)` then **`heap::ChainInsert`** | KEEP — **must not stripe** | Same. |
| `src/catalog/catalog.cpp:548-556` | the bootstrap table list's comment: sys.cabins and sys.fkeys "issue Keystone ids… from this relation's own `next_id`, like sys.patterns' oid" | KEEP | |
| `src/catalog/catalog.cpp:559-562` | catalog heap pages are formatted `min_key = 0` — "never pruned by key range" | KEEP | This is why sys.cabins/sys.fkeys/sys.indexes tolerate any id order; sys.pattern_defs and sys.assertions, which use real chains, do not. |
| `include/kds/catalog/well_known.hpp:226` | `kSysPatternDefsTable`/`kSysAssertionsTable` in the bootstrap list | KEEP | |

---

#### 8A.7 Tests that pin a contract this plan touches

| file:line | pins | tag |
|---|---|---|
| `tests/catalog_test.cpp:663` `AllocateRowIdAndReadsDoNotBumpTheCatalogVersion` | issuing an id publishes nothing | KEEP — SF3 must preserve it for the cursor |
| `tests/catalog_test.cpp:1144` `AHeapRelationTakesASuppliedKeyAtOrAboveTheMark` | heap admits ≥ mark, refuses below, stays `kAscending` | KEEP |
| `tests/catalog_test.cpp:1179` `ABtreeRelationTakesABelowMarkKeyAndTurnsUnordered` | the flip, and that it survives a reopen | ROUTE (the flip's route changes; the assertion does not) |
| `tests/catalog_test.cpp:1211` `ATableAccessCarriesTheOrderAndIsInvalidatedByTheFlip` | the cache/invalidation half of §8A.4a | ROUTE |
| `tests/catalog_test.cpp:1237` `AnIssuedIdRisesAboveEverySuppliedOne` | **the one-mark contract** — `Admit(900)` then `AllocateRowId() == 901` | RETIRE/DECIDE — false per class under D5+D8; this test is the plan's tripwire |
| `tests/catalog_test.cpp:1261` `ACatalogRelationStartsAscending` | bootstrap rows set the field | KEEP |
| `tests/catalog_test.cpp:1361` `ASequenceOnALaterPageStillIssuesIds` | the mutator walks the sys.tables overflow chain and the bump persists | KEEP |
| `tests/keystone_id_test.cpp:164` `TheAllocatorAlsoIssuesCatalogOidsAndCatalogKeystones` | **the shared-path fact (c) asks about**, and asserts sequences are per relation and independent | KEEP — the guard against striping system oids |
| `tests/keystone_id_test.cpp:194` `AnExhaustedSequenceRefusesRatherThanWrapping` | K4's only enforcement point | ROUTE |
| `tests/keystone_id_test.cpp:341` `ACrashReissuesIdsThatTheDurableLogStillClaims` | K1 does not hold across a crash today | KEEP — SF6 inherits it per class |
| `tests/keystone_budget_test.cpp:143` `DescribeReportsConsumptionBesideTheSequence`, `:157` `ShowBudgetListsEveryRelationIncludingTheCatalogsOwn` | the two K4 surfaces, and that SHOW BUDGET covers **the catalog's own relations** | ROUTE |
| `tests/command_dispatcher_test.cpp:192,205,210,430` | DESCRIBE's exact `clustered_type=… key_order=…` token order | KEEP |
| `tests/catalog_row_test.cpp:254-258,355,358,398-401` | the key-order byte's offset and isolation | KEEP |
| `tests/supplied_key_test.cpp:309` `OrderByEmitsKeyOrderAfterADescendingLoad`, `:705` `TheKeyOrderSurvivesAcrossDispatchers` | the flip's user-visible effect and its durability | ROUTE |
| `tests/core_runtime_test.cpp:541` / `:946` `RowIdLeaseTableTest` | the lease's issue/exhaust and the window-at-run-in-hand rule | KEEP |
| `tests/core_runtime_test.cpp:564` `APeerIssuesLeasedRowIdsWithoutWritingTheCatalog` | the peer id path end to end | ROUTE |
| `tests/core_runtime_test.cpp:860` `APeerAsksForRowIdsItWasNeverGrantedAndTheRetrySucceeds`, `:976` `ARelationCoreZeroCannotGrantIsAskedForOnceAndStarvesNoOther` | the demand/deny discipline | KEEP |
| `tests/core_runtime_test.cpp:1922` `APeerRefusesACallerSuppliedKeyAndTakesTheSameRowWithout` | the exact refusal SF4 replaces with a ship | RETIRE |
| `tests/core_runtime_test.cpp:1978` `AFundedPeerGrowsItsOwnBtreeWritingNoCatalogPage` | **the closest existing analogue of SF3's zero-core-0-traffic test** | KEEP — extend, don't rewrite |

---

#### 8A.8 The three questions, answered

##### (a) How many distinct catalog writes can a named-key INSERT trigger today, and where?

**Per row: at most one; per statement: unbounded, and two *kinds*.** All of them are `OverwriteLogged` on the sys.tables page, on core 0, inside `Catalog::AdmitExplicitRowId` (`src/catalog/catalog.cpp:2114-2241`), reached from the single call site `src/server/command_dispatcher.cpp:3574`:

1. **The mark advance** — `row.next_id = id + 1` + `OverwriteLogged` (`catalog.cpp:2196-2201`). Fires on **every** named key at or above the mark, i.e. once per row of an ascending named-key load.
2. **The `key_order` flip** — `row.key_order = kUnordered` + `OverwriteLogged` (`catalog.cpp:2171-2177`), plus `cache_.MarkKeysUnordered` + `++catalog_version_` + `on_invalidate_()` (`:2236-2238`), the last of which broadcasts `kCatalogInvalidate` to every peer (`expeditor.cpp:1044,1446`). Fires at most **once per relation, ever**, and only on a btree relation; guarded at `:2170`.
3. **Neither** — a below-mark key on an already-`kUnordered` btree relation returns having written nothing (`:2170`); a below-mark key on a heap relation is `OutOfRange` before any write (`:2148-2158`).

The two are mutually exclusive *within one call* (the below-mark branch returns at `:2186`), but **not within one statement**: a multi-row `VALUES` mixing an above-mark key and a below-mark key triggers both, and the per-row loop (`command_dispatcher.cpp:3300-3310`) never enters the sorted fill when any row names a key (`:3295-3298`), so an N-row ascending named-key INSERT makes **N** catalog writes. Nothing else on the INSERT path writes a catalog page: the omit arm draws from the lease on a peer (`catalog.cpp:2059`) and bumps `next_id` on core 0 (`:2094`); root moves write the anchor, not the row (`catalog.cpp:2243-…`, PW2-3/PW2-4); `NoteCabinWrite` (`command_dispatcher.cpp:3620`) writes Cabin entry pages, not catalog rows. (Adjacent, off the INSERT path: `Catalog::RegisterPattern` from `src/stats/trail_recorder.cpp:57` writes two catalog pages on a statement's trail-recording path — sys.patterns' own `next_id` plus the sys.patterns row.)

##### (b) Does any reader of `next_id` run on a core that does not own the relation?

**Yes — four of them, and one of them is a writer.**

1. **`SHOW BUDGET`** (`command_dispatcher.cpp:872-926`) loops `ListTables()` and reads every relation's `next_id`. It is dispatched inline on the session's core (`:413`) with **no affinity check at all** — `CheckReadAffinity` only guards step chains (`:4740`).
2. **`DESCRIBE`** (`:1092-1176`, `next_id` at `:1155`, `BudgetOf` at `:1167`) — same, dispatched at `:423`.
3. **`SELECT … FROM kds_tables`** (`src/exec/catalog_view.cpp:51-74`) — emits `next_id` as a column, on the session's core.
4. **The lease grant handler** (`src/server/row_id_lease_service.cpp:33`) runs on **core 0** and calls `AllocateRowIdRange` — i.e. core 0 *writes* `next_id` for relations owned by peers, on demand, on the peer's insert path.

This is legal today only because the catalog pages sit below `server::kFirstUserPageId` and a peer may **read** the system range and never write it (`src/catalog/catalog.cpp:29-32`, `src/storage/device_page_store.cpp:249-278`, pinned by `tests/core_runtime_test.cpp:342 APeerReadsTheCatalogAndCannotWriteIt`). **That exemption does not extend to sub-anchors.** Sub-anchors are relation pages above `kFirstUserPageId`, own-stamped by their class's core, and `MayFault`/`MayWrite` refuse a foreign core outright. So finding 8 is confirmed and is stronger than "a ring request or a per-core figure": **all four sites above break the moment the mark moves into a sub-anchor**, and three of them are user-facing surfaces with tests pinning their exact tokens (`tests/keystone_budget_test.cpp:143,157`, `tests/command_dispatcher_test.cpp:192`). SF3 must name the mechanism (a fan-out request from the session core to N class cores, or a per-class rendering), and SF3 must also settle the arithmetic — "max over class marks" is not `issued`; with striped ids the honest figure is `Σ issued_c` and `remaining = min_c remaining_c`.

##### (c) Do the catalog's self-hosted relations share an id-issuance path with user relations that a per-class cursor would break?

**Yes, completely, and in two independent ways.**

**The shared function.** `Catalog::AllocateRowId` (`src/catalog/catalog.cpp:2054-2113`) is one function with no relation-kind branch. It serves the user omit path (`command_dispatcher.cpp:3579`) and, verbatim, seven self-hosted callers: sys.patterns oids (`catalog.cpp:2380`), sys.cabins `cabin_id` (`:2625`), sys.fkeys `fk_id` (`:2729`), sys.indexes oids (`:2917` and `src/exec/index_ddl.cpp:286`), sys.assertions ids (`src/exec/assertion_catalog.cpp:411`), sys.pattern_defs ids (`src/stats/pattern_defs.cpp:171`). If SF3 replaces this function's body with a per-(relation, class) cursor, all seven change behaviour unless the cursor branches on `stride_n` **before** the lease/cursor lookup — note the existing lease branch at `:2059` is already oid-blind and would answer for `kSysIndexesTable` as readily as for a user relation.

**The breakage is not hypothetical.** Two of those relations place their rows through `heap::ChainInsert`: sys.pattern_defs (`pattern_defs.cpp:197`) and sys.assertions (`assertion_catalog.cpp:223`). A striped cursor issues `base, base+stride_b·stride_n, …` — descending relative to a sibling class and, on a single-class cursor, still gapped — and the chain refuses any id below the tail page's `min_key` with `OutOfRange` (`heap_chain.cpp:94-104`). The first `CREATE PATTERN` or `CREATE ASSERTION` after such a change fails. (The other five — sys.patterns, sys.cabins, sys.fkeys, sys.indexes — live on fixed catalog pages formatted `min_key = 0` (`catalog.cpp:559-562`) and would survive gapping, but sys.patterns' value is an **oid**, not a Keystone id, and `Catalog::GenerateUserOid`'s stated contract at `include/kds/catalog/catalog.hpp:349-357` explicitly rests on "patterns, cabins, foreign keys and indexes each use their own persistent per-relation `next_id` sequence".)

**The boundary the plan assumes is real and is already in the code**, which is the good news §2 D1 asks SF-V1 to confirm: system relations carry `anchor_page_id = kInvalidPageId` (`include/kds/catalog/rows.hpp:122-131`), `owner_core = kSystemCore` (`catalog.hpp:883-885`), and peers refuse every DDL verb before any handler (`command_dispatcher.cpp:437-446`). So a `stride_n = 0` sentinel on system relations, checked *before* the cursor, is sufficient — but it must be checked in `Catalog::AllocateRowId` itself, not at the dispatcher, because five of the seven callers never pass through the dispatcher at all. `tests/keystone_id_test.cpp:164` is the test that will catch it.

### 8B. SF-V2 — the one-root assumption

*(Inside §8B, a bare §1–§4 names §8B's own sections; the plan's sections are cited as §2/§3/§4 only where a decision is named.)*

Read at `952bbb9` over `include/` and `src/` only. Every row is `file:line` on that tree. Tags: **KEEP** (unchanged under stride) · **PER-CLASS** (must be replicated per stride class) · **MERGE** (session-core work over N producers) · **REFUSE** (must be refused on a stride relation until built) · **DECIDE** (needs a decision §1–§6 has not taken).

Read against §9's amended reading (finding 1): `stride_n = 1` is today's relation byte-for-byte, so every KEEP below is also "correct at `stride_n = 1`", and every PER-CLASS/REFUSE row bites only at `stride_n > 1`.

---

#### 8B.1 Readers of the root, the anchor and the owner

##### 8B.1a The field declarations

| file:line | what | tag | note |
|---|---|---|---|
| `include/kds/storage/anchor_page.hpp:36-42` | Anchor layout: `clustered_root u32` at body offset 0, `nr_index u16`, `reserved u16`, index entries | PER-CLASS | SF2's format bump. `reserved` is the natural home for `class u16`; `high_water u64` has no room without pushing `kAnchorEntriesOffset` — a bump either way |
| `include/kds/storage/anchor_page.hpp:46-50` | `FormatAnchorPage` / `AnchorClusteredRoot` / `SetAnchorClusteredRoot` — **one** root per page | PER-CLASS | Signatures survive verbatim if the sub-anchor is a whole anchor page per class (D6/D7). N roots in *one* page would change all three |
| `include/kds/catalog/schema.hpp:179-180` | `TableAccess::desc_page_id` + `clustered_type` — the one resolved root | PER-CLASS | Becomes `std::array/vector<PageId>` indexed by class, or a `RootOf(class)` accessor. **Every** row in §1b and §2 reads this field |
| `include/kds/catalog/schema.hpp:240` | `TableAccess::anchor_page_id` — one anchor, "cacheable: fixed at CREATE" | PER-CLASS | The cacheability argument survives (CREATE-fixed, no ALTER); only the arity changes |
| `include/kds/catalog/schema.hpp:211` | `TableAccess::owner_core` — "the statement planner reads this to pick §2's fast path" | DECIDE | Under stride a relation has no single owner core. Either it stays as the *class-0 / creating* core and every router asks `f(key)` first (SF4/SF5), or it is removed and its ~14 readers below each get a class argument. The plan has not said which |
| `include/kds/catalog/schema.hpp:193` | `heap_tail_hint`, `kInvalidPageId` for btree | KEEP | Heap-only; stride relations are BTREE by construction |
| `include/kds/catalog/schema.hpp:234` | `key_order`, flipped once ever by `AdmitExplicitRowId` | DECIDE | §9 finding 7: the flip is a core-0 catalog write + version bump + peer notify. Under stride it must be routed once per relation before any class places a below-mark key |
| `include/kds/catalog/rows.hpp:62,71,81,104,121,134` | `SysTableRow{desc_page_id, next_id, varheap_page_id, owner_core, key_order, anchor_page_id}` | PER-CLASS | SF1 adds `stride_n`; SF2 adds the anchor id array (§9 finding 12 deletes the contiguity flag in favour of a fixed array + a cap on `stride_n`) |
| `include/kds/server/superblock.hpp:160` | Format 14→15 was `SysTableRow` growing `anchor_page_id` | KEEP | SF1/SF2 own the next bump; the precedent is exactly this line |

##### 8B.1b `InitTableAccess`'s root resolution

| file:line | what | tag | note |
|---|---|---|---|
| `src/catalog/catalog.cpp:1808` | `Catalog::InitTableAccess` entry; cache hit returns the shared entry | PER-CLASS | One cache entry per (core, oid) must carry N roots — or N entries, which breaks `FindTableAccess(oid)`'s key |
| `src/catalog/catalog.cpp:1830-1836` | Copies `desc_page_id`, `clustered_type`, `varheap_page_id`, `owner_core`, `key_order`, `anchor_page_id` off the row | PER-CLASS | The literal fill site |
| `src/catalog/catalog.cpp:1863` | **`if (anchor_page_id != kInvalidPageId && owner_core == core_id_)`** — the anchor is resolved only for *this core's own* relations | DECIDE | This is the load-bearing line. Under stride the predicate becomes "for each class this core serves", and a class this core does *not* serve keeps the CREATE-time root — which is a root it may never walk. The comment's whole argument ("a foreign relation's root is never walked here, execution ships to the owner") is what SF5's merge invalidates: the session core now walks *some* of a relation's classes locally and ships the rest |
| `src/catalog/catalog.cpp:1868-1878` | Validates `PageType::kAnchor` and `GetOwnerOid == oid` (checked redundancy, the f5686f8 C5) | PER-CLASS | Repeat per sub-anchor. A `class` field in the anchor gives a second checked redundancy for free (SF2's `nr_index` discipline) |
| `src/catalog/catalog.cpp:1881` | `access.desc_page_id = AnchorClusteredRoot(anchor)` — the single resolve | PER-CLASS | Becomes N resolves, one held ref per class. The 96b0343 C2 "one read, one failure decision, no tear expressible" argument must be restated for N: a fill that anchors 3 of 4 classes is exactly the half-anchored tear that review closed |
| `src/catalog/catalog.cpp:1889-1897` | Unfaultable own anchor falls back to the row (the pre-grant window, f5686f8 C1) | PER-CLASS | Per class; PW1c-7's demand path re-delivers per class (D7) |

##### 8B.1c Root writers

| file:line | what | tag | note |
|---|---|---|---|
| `src/catalog/catalog.cpp:1063-1090` | `Catalog::WriteAnchorRoot(anchor_page, expected_owner_oid, index_oid, root, trx)` — validates header + owner oid, then sets clustered (`index_oid == 0`) or index root, logs `ANCHOR_UPDATE` | PER-CLASS | The one anchor mutator. Takes the anchor page id already, so it generalizes by the caller passing the *class's* sub-anchor. `expected_owner_oid` no longer distinguishes sub-anchors of one relation — add the class to the check |
| `src/catalog/catalog.cpp:2243-2275` | `UpdateRelationDescPage(table_oid, new_desc_page_id, anchor_page_id)` — root move: anchor write + `cache_.UpdateDescPage` in place, **no catalog write** (PW2-4) | PER-CLASS | Needs a class argument for both halves. The "in place, no BumpVersion" license (the caller holds a `const TableAccess*`) must survive the arity change |
| `src/catalog/catalog.cpp:2257-2261` | `anchor_page_id == kInvalidPageId` → the system-relation arm | KEEP | System relations stay non-stride (`stride_n = 0`, SF1) |
| `src/catalog/catalog_cache.cpp:133` | `it->second.desc_page_id = root` — the in-place single-root cache update | PER-CLASS | |
| `src/catalog/catalog.cpp:3065-3090` | `UpdateIndexRoot(rel_oid, index_oid, new_root, anchor_page_id)` — "`UpdateRelationDescPage`'s exact mirror" | REFUSE | §4 refuses secondary indexes on stride relations. Under the amended reading (finding 1) `stride_n = 1` keeps this path alive unchanged; `stride_n > 1` must refuse at CREATE INDEX and this function is then unreachable for those relations |
| `src/catalog/catalog_cache.cpp:120` | `CatalogCache::UpdateIndexRoot` in-place | REFUSE | Same |
| `src/exec/index_maintain.cpp:199-201` | Root republish after an index split, from inside the per-row write hook | REFUSE | The site whose soundness argument (`schema.hpp:332-340`) is single-owner |
| `src/catalog/catalog.cpp:2950-2965` | `CreateIndex`'s `AnchorSeed::kHere` arm writes the relation's anchor | REFUSE | |
| `src/server/index_build_service.cpp:195-201` | Owner-built index seeds **its own** anchor slot (`AnchorSeed::kByOwner`) | REFUSE | PW1c-6b's single-owner premise; N writer classes is the two-writer route §4 names |
| `src/wal/redo.cpp:414-436` | `ANCHOR_UPDATE` redo: type-checks the page, `SetAnchorClusteredRoot` when `index_oid == 0` | KEEP | Keyed on `page_id`, stream-local; N sub-anchors redo independently in N streams (SF6's claim) |
| `include/kds/wal/payload.hpp:109-117` | `AnchorUpdatePayload{index_oid u64, root u32}`, 12 bytes | PER-CLASS | **§9 finding 7 confirmed in code**: no class, no high-water. D8's per-class mark is a *widened or new record*, so the first draft's "no new record types beyond SF2/SF3's anchor fields" was false as written |
| `src/catalog/catalog.cpp:1252-1270` | CREATE TABLE formats **one** anchor, `FormatAnchorPage(…, root_id)`, `PAGE_INIT` + `ANCHOR_UPDATE` | PER-CLASS | Becomes N formats, N `PAGE_INIT`s, N `ANCHOR_UPDATE`s, N publishes (SF2) |
| `src/catalog/catalog.cpp:1192-1228` | CREATE TABLE formats **one** clustered root (`btree::FormatRoot`) + `PAGE_INIT` | PER-CLASS | D7 says the class's *first INSERT* formats its root leaf instead — so this site either moves to N eager roots or becomes a lazy per-class format with its own crash rule (SF6's third test) |

##### 8B.1d `owner_core` readers, every site

| file:line | what | tag | note |
|---|---|---|---|
| `src/catalog/catalog.cpp:1289` | `AssignOwnerCore(placement_, kSystemCore, core_count_, nrelations)` at CREATE | DECIDE | A bare `class mod cores` discards `rotate`; D4's `(owner_core + class) mod cores` keeps it. This is the line that decides |
| `src/catalog/catalog.cpp:1346-1348` | `if (owner_core != kSystemCore && on_publish_) on_publish_(oid, owner_core, root, varheap, anchor)` — the publish hook, one call, five page ids | PER-CLASS | SF2's "payload extended past six slots or issued per class". Per class is the smaller diff: the hook already has two callers |
| `src/catalog/catalog.cpp:1863` | Anchor resolution gate (above) | DECIDE | |
| `src/catalog/catalog.cpp:2832-2839` | `CheckIndexDef`: refuses seeding a peer-owned relation's anchor | REFUSE | |
| `src/catalog/foreign_key.cpp:31-36` | An FK across two owner cores is refused at declaration | REFUSE/DECIDE | Under stride *both* ends span N cores; the predicate has no meaning. FK-linked relations must be `stride_n = 1` |
| `src/server/command_dispatcher.cpp:2950-2955` | `CheckWriteAffinity`: `owner_core != core_id_` → `CrossCoreWriteRefused` | PER-CLASS | SF4 replaces this with `f(key)`; the `cross_core_writes_` stat records `(home, owner, oid)` and gains no class |
| `src/server/command_dispatcher.cpp:2959-2962` | `session.MayWriteOn(owner_core)` — CC3's one-stream binding | REFUSE | §3's "explicit transaction writing multiple classes" refusal lives here. §9 finding 9: `Unsupported`, not retryable |
| `src/server/command_dispatcher.cpp:3081` | `session.BindHomeCore(access.owner_core)` | DECIDE | Under stride the home core is the *serving class's* core, decided per row |
| `src/server/command_dispatcher.cpp:3069-3078` | PW1c-7 rights probe over `{desc_page_id, varheap_page_id, anchor_page_id}` — three pages | PER-CLASS | Becomes `{class root, class anchor}` for each class this core serves, plus the shared varheap (§4) |
| `src/server/command_dispatcher.cpp:3171-3174` | `CheckReadAffinity`: any step whose `owner_core != core_id_` → `CrossCoreReadUnsupported` | MERGE | SF5's fork. Today it is a whole-relation verdict; under stride the verdict is per class and the "some local, some foreign" case has no representation |
| `src/server/command_dispatcher.cpp:4688-4691` | Single-step remote read: `owner_access->owner_core != core_id_` → `remote_reads_->Open(step, owner_core, …)` | MERGE | See §3 |
| `src/server/command_dispatcher.cpp:4725-4731` | Two-step pipeline: either relation foreign → `BuildTwoStepPipeline(…, outer_core, inner_core, …)` | MERGE | Two scalars where stride needs two *sets* |
| `src/server/command_dispatcher.cpp:1374-1387` | `CREATE INDEX` on a foreign relation → `BeginForeignIndexBuild(stmt, owner_core, session)` | REFUSE | See §2 |
| `src/server/command_dispatcher.cpp:1409-1418` | `DROP INDEX` in a txn on a foreign relation → refused by name | REFUSE | |
| `src/server/command_dispatcher.cpp:1156` | `DESCRIBE` prints `owner_core=` | DECIDE | Must print what, for N classes? |
| `src/server/index_build_service.cpp:133-141` | Build server re-reads the row: "the row is the authority on who owns the relation (CC7)" | REFUSE | |
| `src/server/relation_grant_service.cpp:39-57` | Grant re-delivery: `row.owner_core != header.src_core` → drop; else `publish(oid, owner_core, desc, varheap, anchor)` | PER-CLASS | The demand path D7 reuses. Must become per class: the requester names a class, the responder checks *that class's* mapped core |
| `src/server/core_runtime.cpp:638-672` | `PrepareRelationHandoff(wal, owner_core, pages)` — logs `PAGE_HANDOFF` per page, syncs, returns the grant | KEEP | Page-granular already; call it N times |
| `src/server/expeditor.cpp:1459-1510` | The publish hook body: `RelationFaultExtentOf` → flush → prepare → fault grant → write grant → `EvictClean` | PER-CLASS | See §4 |
| `src/exec/catalog_view.cpp:52-72` | `sys.tables` view: one `desc_page_id` column | DECIDE | |

---

#### 8B.2 Planner, compiler, walkers, optimizers, Waystone

##### 8B.2a Step shapes

| file:line | what | tag | note |
|---|---|---|---|
| `include/kds/exec/step_chain.hpp:406-412` | `Step{step_id u32, rel_oid Oid, …}` — **a step names a relation, never a subtree** | PER-CLASS | The single structural fact SF5 turns on. A stride scan is N steps over one `rel_oid`, so either `Step` gains a `class` field or `step_id` carries it (and then §3's tag question is answered by construction) |
| `src/exec/step_compiler.cpp:1628` | `kLookup` — pk equality against a pre-known literal | PER-CLASS | SF5's easy half: `f(literal)` narrows to one class before affinity. One tree per statement, so the executor is unchanged |
| `src/exec/step_compiler.cpp:1637` | `kProbe` — pk equality against an earlier step's column | PER-CLASS | Same shape, but the key is only known per outer row: the class is chosen *at execution*, not at compile. A probe whose rows land in classes on N cores is N descents in N cores' pages — this is the shape §3 has no wire for at all |
| `src/exec/step_compiler.cpp:1651` | `kRange` — `BETWEEN low AND high` on the pk | MERGE | A stride range is **not** a contiguous class set: `(key / B) mod N` interleaves, so any range wider than `B` touches every class. N producers, always |
| `src/exec/step_compiler.cpp:1661,1672` | `kIndexProbe` / `kIndexRange` | REFUSE | §4's CREATE INDEX refusal makes these unreachable on a stride relation |
| `src/exec/step_compiler.cpp:1680,1693` | `kCabinProbe` | REFUSE | Cabined relations already refuse peer writes (`command_dispatcher.cpp:3036`); a stride relation's Cabin entry sets would name pages on N cores |
| `src/exec/step_compiler.cpp:1210,1373,1701` | `kScan` (default) and `kFilterScan` | MERGE | Every full walk becomes N producers |
| `src/exec/step_compiler.cpp:1832-1859` | `ORDER BY <pk> ASC` elision: `one_ascending_pk && driving_emits_pk_order` → `sort_keys.clear()`, then `emit_in_key_order = true` when `key_order == kUnordered` | MERGE + DECIDE | **The elision is wrong under stride.** One class's subtree is key-ordered; the *relation* is not, because class 1's keys interleave class 0's. Discarding the sort is only safe if the merge restores the order — so the elision must become conditional on the merge existing, or `ORDER BY pk` on a stride relation must be refused until SF5 lands |
| `include/kds/exec/step_chain.hpp:458` | `Step::emit_in_key_order` | MERGE | Must travel in the descriptor (§3) *and* be honoured per producer for the k-way merge to have ordered inputs |
| `src/exec/step_vm.cpp:1717-1745` | The per-page key-order emission (first slot stands in for a page hook, sorts live slots by Keystone id) | KEEP | Within-page only; unchanged by this plan, as SF5 says |
| `src/exec/step_vm.cpp:571` | `kLookup`/`kProbe`: `BtreeLookup(store_, access.desc_page_id, key)` | PER-CLASS | |
| `src/exec/step_vm.cpp:1315` | Index-probe pk resolve: `BtreeLookup(store_, access.desc_page_id, pk)` | REFUSE | |
| `src/exec/step_vm.cpp:1444` | Cabin-hint heal: `BtreeLookup(store_, access.desc_page_id, entry.pk)` | REFUSE | |
| `src/exec/step_vm.cpp:1793-1805` | The walk's start: `BtreeSeekLeaf(desc_page_id, range->low)`, else `BtreeLeftmostLeaf(desc_page_id)`, else `cur = desc_page_id` (heap) | PER-CLASS | One walk origin per class. `BtreeSeekLeaf` on a class's subtree with the *relation's* `range->low` is still correct (an accelerator that cannot change the answer) |
| `src/exec/step_vm.cpp:1826-…` | The page loop with `CheckPageWalkBudget(pages, walk_origin, …)` and the park point at the boundary | KEEP | Per-producer; the park point is what P4d-4a needs and what a merge producer needs too |
| `src/exec/step_vm.cpp:2419,2483-2484` | `InnerBuild* map` / `InnerBuildStore owned_builds_` — statement-lifetime, core-local, keyed by `step_id` | DECIDE | See 2e |

##### 8B.2b ANALYZE

| file:line | what | tag | note |
|---|---|---|---|
| `src/server/command_dispatcher.cpp:4551` | `AppendEscaped(os, exec::FormatPlan(chain))` | DECIDE | |
| `src/exec/plan_printer.cpp:140` | Prints `step.rel_name` or `oid=<rel_oid>` per step | DECIDE | An N-producer plan prints as N identical lines naming one relation unless the printer learns the class. ANALYZE's contract ("the run it describes is the run that happened") makes this mandatory, not cosmetic |
| `src/server/command_dispatcher.cpp:4682` | `!analyze` gates *both* remote paths | KEEP | ANALYZE never ships today and needn't under stride — but then ANALYZE of a stride scan describes a run the merge did not perform. **DECIDE** |

##### 8B.2c `SHOW` / `DESCRIBE` walkers that touch relation pages

| file:line | what | tag | note |
|---|---|---|---|
| `src/server/command_dispatcher.cpp:1135-1144` | `DESCRIBE`: reads `sys.tables.desc_page_id`, then **faults the anchor directly** (not through `InitTableAccess`) and prints `root_page_id` | PER-CLASS | On a stride relation this must fault N sub-anchors — N of which this core may not own (`GetForRead` of a foreign-stamped page: `device_page_store.cpp:251` refuses the fault for a leased store). **The first read-only surface that breaks the second law** |
| `src/server/command_dispatcher.cpp:1152-1160` | Prints `root_page_id`, `key_order`, `next_id`, `owner_core`, `columns` | DECIDE | |
| `src/server/command_dispatcher.cpp:1166-1177` | K4 budget from `BudgetOf(row.next_id)` | MERGE/DECIDE | §9 finding 8: `next_id` is frozen at CREATE under D8, so this prints a lie. Max-over-classes is a ring request |
| `src/server/command_dispatcher.cpp:872-928` | `SHOW BUDGET`: `BudgetOf(next_id)` per relation, `capacity` in the summary | MERGE/DECIDE | Same. And "one capacity for every relation" (`kKeystoneBudgetCapacity`) becomes N stripes of one space |
| `src/server/command_dispatcher.cpp:1628-1700` | `SHOW INDEXES`: `InitTableAccess(row.table_oid)` per index, prints the **anchored** root from `access->indexes` | REFUSE | Unreachable for `stride_n > 1` |
| `src/server/command_dispatcher.cpp:933-949` | `SHOW TABLES`: names only, view-filtered | KEEP | Touches no relation page |
| `src/server/command_dispatcher.cpp:585-640…` | `SHOW META`: superblock, `core=`, purge counts, lease refill blocks | KEEP | §7 notes the class-cursor lines come free here |
| `src/server/command_dispatcher.cpp:797-855` | `SHOW ACCESS`: `InitTableAccess(row.rel_id)` per stat row, for names only | KEEP | No page walk. But see §4 on the stat row's own arity |
| `src/server/command_dispatcher.cpp:951-…` | `SHOW PAGE` | KEEP | Page id in, bytes out; already class-agnostic |
| `src/server/command_dispatcher.cpp:2083` | `SHOW RELAYOUT` → the planner's per-relation walk | KEEP | Heap-only (see 2f) |
| `src/exec/catalog_view.cpp:52-72` | `SELECT … FROM sys.tables` | DECIDE | |

##### 8B.2d Assertions — build, enforce, recover (RC07)

| file:line | what | tag | note |
|---|---|---|---|
| `src/exec/assertion_build.cpp:157` | `BuildBoundCabin(store, access, …)` — one build per assertion | REFUSE | |
| `src/exec/assertion_build.cpp:172-178` | `PageId leaf = access.desc_page_id;` then `BtreeLeftmostLeaf(desc_page_id)` — **one leaf chain is the whole relation** | REFUSE | N subtrees means N leftmost leaves and N chains; the aggregate is only correct over all of them |
| `include/kds/exec/assertion_check.hpp:123,193` | `AssertionEnforcer::AnyOn(oid)` over `by_oid_` — per-relation, core-local | REFUSE | An enforcer on the session core cannot see a peer class's writes; the aggregate would admit violating rows |
| `src/exec/assertion_check.cpp:97-116,180,200` | Admit / reserve / delete paths keyed on `by_oid_[oid]` | REFUSE | |
| `src/server/command_dispatcher.cpp:3043-3049` | The shape gate's assertion refusal ("the assertion's entry pages are the system core's") | KEEP | Already the right refusal; SF4's fork must sit **after** it so the name survives (SF4 says this) |
| `src/server/mount_recovery.cpp:167-253` | `ResumeAssertionsAfterRecovery`: `ListAssertions` → `ReviveAssertion` per def → `RecoverAssertions(device, core_id, from_lsn, …)` | REFUSE | Single-core, single-stream: `core_id` and one `from_lsn`. A stride relation's writes are in N streams, so a rebuild would fold one stream's records onto a snapshot covering N |
| `src/exec/assertion_catalog.cpp:527-545` | `ReviveAssertion`: `InitTableAccess(def.target_oid)`, columns resolved against the *current* schema | REFUSE | |
| `include/kds/exec/assertion_recover.hpp:34-46` | The pass walks "the cabin's **own pages**" from the last checkpoint | DECIDE | Entry pages are the assertion's, not the relation's — so they are core 0's. Not per-class, but not reachable from a peer class either |

##### 8B.2e Cabin optimizer walks and the join inner build

| file:line | what | tag | note |
|---|---|---|---|
| `src/exec/cabin_optimizer_exec.cpp:86` | `CabinOptimizerExecutor::BuildSeededSets` | REFUSE | |
| `src/exec/cabin_optimizer_exec.cpp:105-110` | `PageId leaf = access.desc_page_id;` → `BtreeLeftmostLeaf` — the same one-chain assumption as the assertion builder, and it says so | REFUSE | |
| `src/exec/cabin_optimizer_exec.cpp:100` | `store_.OpenScanRing()` — the build's pages come from the scan ring | KEEP | Per-core already; N class builds would each open their own |
| `src/exec/cabin_optimizer_exec.cpp:324-340` | Hint heal: `BtreeLookup(desc_page_id, entry.pk)` + `CurrentRelayoutEpoch(store_, entry.page_id)` | REFUSE | A heal on the session core of an entry pointing into a peer class's page is a foreign fault |
| `include/kds/stats/cabin_store.hpp:174-213` | `CabinStore` — core-local, in-memory, keyed by `cabin_id`; no relation, no class | REFUSE | |
| `include/kds/exec/inner_build.hpp:86-130` | `InnerBuild` — statement-lifetime, one executor frame, no lock ("nothing else can reach the map") | DECIDE | **Over a shipped stream it is already fine and stays fine**: `RunConsumer` builds nothing — it decodes one upstream row at a time into a one-slot parent frame (`remote_step_service.cpp:570-600`). The build lives only on the *local* walked-join path (`step_vm.cpp:879,960`). Under stride an inner relation with N classes has N producers feeding one build — which the container tolerates (`Add` is append-only, buckets hold indices) but whose **walk-order property** (`inner_build.hpp:68-84`: "buckets append in walk order and replay front to back") is broken: N interleaved producers give an arrival order that is neither key order nor any single walk's order. The plan must either merge before the build or state that a stride inner's probe replies in merge order |
| `src/exec/step_vm.cpp:220-242,879-1000` | `InnerBuildState` / `InnerBuildStore` keyed by `step_id` | DECIDE | If a stride scan is N steps, the store is keyed correctly by accident; if it is one step with N producers, N states collide on one key |

##### 8B.2f The physical optimizer's relayout shadow

| file:line | what | tag | note |
|---|---|---|---|
| `include/kds/stats/relayout_planner.hpp:16-53` | Shadow-only: every plan is blocked, `PlanAllRelations` takes no `PageStore` so it structurally cannot walk | KEEP | |
| `src/stats/relayout_planner.cpp:231-237` | `if (clustered_type != kHeap \|\| system_relation) return report;` — **btree relations are skipped before any walk** | KEEP | This is why the relayout shadow costs the stride plan nothing today. §9 finding 11 is right that it returns only if the operator restates D1 as "BTREE default" |
| `src/stats/relayout_planner.cpp:240-273` | `InitTableAccess` → `ChainVisit(desc_page_id, …)` census under a scan ring and a budget | KEEP | Heap-only, so unreachable on a stride relation |
| `include/kds/storage/page_header.hpp:44` | `relayout_epoch` is **per page**, not per relation | KEEP | Nothing to replicate |
| `src/exec/tuple_verify.cpp:51-55` | `CurrentRelayoutEpoch(store, page_id)` does a `GetForRead` | DECIDE | Callable on a page this core does not own once a trail or a Cabin hint crosses a class (below) |

##### 8B.2g Waystone — what a trail entry records, and where replay lands

| file:line | what | tag | note |
|---|---|---|---|
| `include/kds/exec/trail_collector.hpp:45-71` | `TouchedTuple{rel_oid, pk, **page_id**, page_epoch, slot, step_id}` | DECIDE | **The answer to "page ids?" is yes.** A trail entry is a raw `PageId` with no core and no class |
| `include/kds/stats/waystone.hpp:118-159` | `WaystoneEntry{pk, rel_oid, **page_id**, page_epoch u32, slot, flags, step_id}`, 32 bytes on disk | DECIDE | Same, persisted. There is no room for a class without a format bump |
| `include/kds/exec/trail_replay.hpp:66-73,110` | `TrailLocation{page_id, page_epoch, slot}`; `Find(step_id, pk)` | DECIDE | |
| `src/exec/step_vm.cpp:622-650` | `TryReplay`: `replay_->Find(step.step_id, key)` → `VerifyTupleAt(store_, at->page_id, at->slot, key, at->page_epoch)` | DECIDE | `VerifyTupleAt` faults `at->page_id` on **this** core. Under stride, one pattern instance's trail can name pages of N classes on N cores, and a replay on the session core of a foreign class's page id is a fault the store refuses (`device_page_store.cpp:240-258`) — a *miss*, not a crash (invariant 8 holds: it falls through to the descent, which then ships) — but it is a per-statement wasted fault and, worse, it is only benign because faults are refused. **If the extent happens to be granted read-only** (CC7's superset assertion: "a granted extent may carry pages of other core-0 relations") the fault succeeds and reads a page another core is actively writing. Decide: put the class in the entry and drop foreign-class entries at `TrailReplay::Build` |
| `src/server/command_dispatcher.cpp:4771` | `waystone_usable = (recorder_ != nullptr \|\| replay_enabled_) && HasReplayableStep(compiled)` | KEEP | |
| `src/server/command_dispatcher.cpp:4795-4815` | Replay read: `FindPattern` → `ReadTrail(page_store_, pattern->waystone_root, dir_depth, instance)` | KEEP | Pattern pages, not relation pages; core 0's |
| `src/server/command_dispatcher.cpp:4822-4826,5072-5074` | Collector reuse; `recorder_->OnPatternResult(instance, trail, klass)` after success | DECIDE | |
| `src/server/command_dispatcher.cpp:5056` | `write_epoch = CurrentRelayoutEpoch(page_store_, page_id)` on the write path | KEEP | Own page, own core |
| `src/server/core_runtime.cpp:219-221` | **Peers get `recorder = nullptr`, `replay_enabled = false`, `access_statistics = false`** | KEEP → DECIDE | Today this is what makes the hazard above unreachable: only core 0 records, and core 0 only walks its own relations. Under stride core 0 walks *its classes* of every relation and ships the rest, so a trail recorded on core 0 names only core-0 pages — safe by accident — while the shipped producers record nothing at all, so a stride relation's Waystone coverage silently drops to `1/N`. Name it or fix it |
| `src/server/expeditor.cpp:744,842` | The recorder exists only on core 0 | KEEP | |

##### 8B.2h PW1c-6b's foreign-arm reads in `HandleIndex`

| file:line | what | tag | note |
|---|---|---|---|
| `src/server/command_dispatcher.cpp:1371-1378` | Reads `GetSysTableRow(rel_oid)`, and on `owner_core != core_id_` routes to `BeginForeignIndexBuild(stmt, owner_core, session)` | REFUSE | One owner core, one build, one tree. Under stride there is no single core to ask |
| `src/server/command_dispatcher.cpp:1379-1388` | No client → refuse by name, citing §7c | REFUSE | The refusal §4 wants is the sibling of this one; reuse the spelling |
| `src/server/command_dispatcher.cpp:1505-1545` | `BeginForeignIndexBuild`: `index_builds_->Request(owner_core, request_id, def)`, parked waiter, deadline | KEEP (as a *shape*) | This is the 6b-2/6b-3 pattern SF4 copies for statement shipping — the one thing in this section the plan reuses rather than refuses |
| `src/server/command_dispatcher.cpp:1409-1424` | `DROP INDEX` inside a txn on a foreign relation, refused (DT9 is core-local) | REFUSE | |
| `src/server/index_build_service.cpp:133-141,163-201` | Server side: owner re-checks the row, opens a one-per-relation window, builds from `access->desc_page_id`, seeds its own anchor, `SyncAll` before the reply | REFUSE | `pending_.Covers(table_oid)` is per relation, not per class |
| `src/exec/index_ddl.cpp:139-215` | `Backfill`: `BtreeLeftmostLeaf(access.desc_page_id)` then one leaf chain, undo-walked | REFUSE | The third "one chain is the relation" site |

---

#### 8B.3 The P4 cross-core pipeline — and the arity answer

##### 8B.3a Where the producer count is fixed at one

| file:line | what | tag | note |
|---|---|---|---|
| `include/kds/server/step_pipeline.hpp:36-45` | `PipelineTag{request_id u64, session_core u32, step_id u32}`, `static_assert(sizeof == 16)` | MERGE | **No producer discriminator.** Two producers of one logical step must share a tag |
| `include/kds/server/step_pipeline.hpp:54-59` | `StepBatchHeader{tag, seq, row_count}`, `static_assert(sizeof == 24)`; "`seq` is per-edge … a gap means a lost batch, asserted not handled" | MERGE | N producers under one tag interleave two `seq` sequences into one stream — the gap assertion breaks |
| `include/kds/server/step_pipeline.hpp:62-78` | `StepEofPayload` / `StepCreditPayload` / `StepErrorPayload` all carry the tag alone | MERGE | |
| `include/kds/server/step_pipeline.hpp:110-148` | `kInitialCreditsPerEdge = 4`; `EdgeCredit` holds one `available_`/`ceiling_` — **one edge** | MERGE | N edges need N `EdgeCredit`s and an N× preallocation ceiling |
| `include/kds/server/remote_step_service.hpp:62-73` | `StepOpenHead{tag, downstream_core u32, downstream_step u32}`, 24 bytes | MERGE | One downstream, and `downstream_step = 0` means "the session's own read" |
| `include/kds/server/remote_step_service.hpp:94-103` | **`StepOpenUpstream{upstream_core u32, forwarded[], enclosed_open}`** — one upstream core, one enclosed open | MERGE | The envelope has no room for a second producer |
| `include/kds/server/remote_step_service.hpp:199-237` | `Pipeline{tag, downstream, **EdgeCredit credit**, batches, seq, producing, cancelled, draining, **std::optional<InputEdge> consumer**}` | MERGE | **`std::optional`, not a vector.** One input tag, one upstream core, one input deque, one `input_eof` bool |
| `src/server/remote_step_service.cpp:213-218` | `Find(tag)` — exact-tag linear match over `pipelines_` | MERGE | Two stages with one tag alias |
| `src/server/remote_step_service.cpp:486-494` | `OpenConsumingStage` refuses an enclosed open whose `downstream_step != head.tag.step_id` | MERGE | The wiring check is 1:1 by construction |
| `src/server/remote_step_service.cpp:496-504` | `pipe.consumer.emplace(edge); pipelines_.push_back(pipe)` — one edge installed | MERGE | |
| `src/server/remote_step_service.cpp:507-521` | "State first, upstream last": one forwarded open | MERGE | Becomes N forwards, and a partial-forward failure has no defined unwind |
| `src/server/remote_step_service.cpp:578-582` | `actionable` parks until `input.empty()` is false **or** `input_eof` | MERGE | With N edges the predicate is "any edge has a batch"; the merge additionally needs "every edge has a batch or is at EOF" to pick the minimum key |
| `src/server/remote_step_service.cpp:751-756` | `FindByInputTag` — returns the **first** pipeline whose single `input_tag` matches | MERGE | |
| `src/server/remote_step_service.cpp:758-772` | `OnStepBatch` / `OnStepEof` push into `pipe->consumer->input` / set `input_eof = true` | MERGE | One bool; N EOFs need a count |
| `include/kds/server/remote_step_service.hpp:296-302` | Each stage mints its **own** `AutocommitSnapshot` — "no view crosses a core" | DECIDE | §9 finding 3: N producers = N views. The anomaly is real and unfixed |
| `include/kds/server/remote_step_service.hpp:303-309` | `budget_` is **this core's** limit, applied per stage | DECIDE | N producers each get the full row-touch budget, so a stride scan's budget is N× a local scan's |
| `include/kds/server/session_step_client.hpp:48-71` | `RemoteRead{tag, **owner_core u32**, rel_oid, done **bool**, error, batches, rows, stages[], output_layout, …}` | MERGE | One owner core, one `done` |
| `src/server/session_step_client.cpp:13-32` | `Open`: `head.tag = {request_id, core_id_, step.step_id}`; one `StageAddress`; `plan.final_core = owner_core` | MERGE | |
| `src/server/session_step_client.cpp:34-71` | `OpenPipeline`: register before send, then one `send_(final_core, kStepOpen, …)` | MERGE | |
| `src/server/session_step_client.cpp:73-78` | `Find(tag)` exact match | MERGE | |
| `src/server/session_step_client.cpp:87-99` | Grant-on-receive: `send_(read->owner_core, kStepCredit, …)` — **credit always returns to one core** | MERGE | |
| `src/server/session_step_client.cpp:102-108` | `OnStepEof` → `read->done = true` on the **first** EOF | MERGE | Under N producers under one tag the reply truncates at whichever finishes first — a silently short answer, the worst available failure |
| `src/server/session_step_client.cpp:110-139` | `OnStepError` matches by `(request_id, session_core)` — deliberately not by exact tag | KEEP | The one part that already generalizes: an error anywhere is the statement's error |
| `src/server/session_step_client.cpp:141-162` | `Close` cancels **every** `StageAddress` | KEEP | The other part that generalizes free; §3's `STEP_CANCEL`-at-LIMIT story rests on it |
| `src/server/command_dispatcher.cpp:3131-3159` | `FinishRemoteRead` renders `read->batches` **in arrival order**, concatenated | MERGE | No comparator, no key, no interleave. The k-way merge is a new operator here (or a new streaming sink), not a parameter |
| `src/server/command_dispatcher.cpp:3105-3122` | Layout resolution: `output_layout` empty → whole schema of `rel_oid` | KEEP | The star form gives the merge the pk at column 0 for free; the projected form does not unless the pk is forced into the forwarded set |

##### 8B.3b What is excluded from shipping today — every exclusion, with its line

**Single-step path** (`src/server/command_dispatcher.cpp:4682-4691`):

| exclusion | file:line | tag |
|---|---|---|
| no client wired | `command_dispatcher.cpp:4682` (`remote_reads_ != nullptr`) | KEEP |
| `ANALYZE` | `command_dispatcher.cpp:4682` (`!analyze`) | DECIDE |
| more than one step | `command_dispatcher.cpp:4682` (`steps.size() == 1`) | MERGE |
| hoisted sub-chains | `command_dispatcher.cpp:4683` (`hoisted.empty()`) | REFUSE |
| **projection present** (only `star()` ships) | `command_dispatcher.cpp:4683` (`chain.value().star()`) | MERGE |
| aggregate | `command_dispatcher.cpp:4684` (`!aggregated()`) | MERGE |
| `ORDER BY` (not elided) | `command_dispatcher.cpp:4684` (`!sorted()`) | MERGE |
| `LIMIT` | `command_dispatcher.cpp:4685` (`!limit.has_value()`) | MERGE |
| `OFFSET` | `command_dispatcher.cpp:4685` (`offset == 0`) | MERGE |
| step-level sub-chains | `command_dispatcher.cpp:4689` (`step.sub_chains.empty()`) | REFUSE |
| `emit_in_key_order` | `command_dispatcher.cpp:4689` (`!step.emit_in_key_order`) | MERGE |
| descriptor refusals (structure aux, `kParam`) | `include/kds/server/step_descriptor.hpp:26-31`, `:48`, `:57` (`ShipsAsWalk`/`ShippedForm` downgrade) | KEEP |
| a column-keyed probe as a leaf | `src/server/remote_step_service.cpp:293-298` | KEEP |

**Two-stage path** (`src/server/session_step_client.cpp:164-265`):

| exclusion | file:line | tag |
|---|---|---|
| not exactly two steps | `session_step_client.cpp:165-167` | MERGE |
| aggregate | `session_step_client.cpp:198-200` | MERGE |
| sort **or** `LIMIT` **or** `OFFSET` | `session_step_client.cpp:201-204` | MERGE |
| **star / no projection** (a projection is *required* — the exact inverse of the single-step rule) | `session_step_client.cpp:205-207` | MERGE |
| sub-chains (chain-level or either step's) | `session_step_client.cpp:208-210` | REFUSE |
| `emit_in_key_order` on either step | `session_step_client.cpp:211-215` | MERGE |
| inner kind not in the allow-list (probe-by-outer-column, or a walk that references the outer row) | `session_step_client.cpp:244-264` | KEEP |
| an upstream `uint64` column on a conjunct's lhs | `session_step_client.cpp:370-375` | KEEP |
| edge forwards no columns | `session_step_client.cpp:316-321` | KEEP |
| reactorless server (a consuming stage needs one) | `src/server/remote_step_service.cpp:396-401` | KEEP |
| wire version | `src/server/step_descriptor.cpp:226`, `:261-267` (`kStepDescriptorVersion = 1`) | MERGE |

Note the shape of the hole this leaves: **a projected single-step read and a star join both ship nowhere today.** Under stride at `cores > 1` every relation's rows are cross-core, so both hit `CheckReadAffinity` (`command_dispatcher.cpp:3164-3183`, `:4740`) and refuse.

---

#### 8B.4 Other per-relation structures a class split must replicate

| file:line | what | tag | note |
|---|---|---|---|
| `include/kds/storage/varheap.hpp:67` | "**One chain per relation**, rooted at `sys.tables.varheap_page_id`" | PER-CLASS | N classes on N cores appending to one chain is the two-writer route. Either N var-heaps (one per class, in the class's sub-anchor) or spillable columns are refused on stride relations |
| `include/kds/catalog/schema.hpp:199` · `src/catalog/catalog.cpp:1232-1250` | One var-heap root, allocated eagerly at CREATE so it stays a DDL fact | PER-CLASS | |
| `src/server/command_dispatcher.cpp:3589` · `:3405` · `:5366` | `VarHeapSink{&page_store_, ta.varheap_page_id, …}` on INSERT, batch-fill and UPDATE | PER-CLASS | The three write sites |
| `src/exec/row_codec.hpp:121` | `root` is `TableAccess::varheap_page_id` | PER-CLASS | |
| `src/server/core_runtime.cpp:673-689` | `RelationFaultExtentOf(row, extent_pages)` — **one** contiguous extent spanning `{desc, varheap, anchor}` | PER-CLASS | With N roots and N anchors on N cores this must become N extents, one per serving core. The current form would grant every core the union |
| `include/kds/server/extent_lease_service.hpp:67-73` | `RelationWriteGrantPayload{count, page_ids[**6**]}`, 28 bytes | PER-CLASS / REFUSE | Six page slots. `stride_n = 4` needs 4 roots + 4 anchors + 1 varheap = 9. **This is the concrete cap §9 finding 12 asks for**: either per-class grants (recommended — one grant of 2-3 pages per class) or a payload change |
| `src/server/core_runtime.cpp:646-651` | `PrepareRelationHandoff` refuses a handoff naming more pages than the grant carries — "refused whole, never truncated" | KEEP | The refusal is already correct; it will simply start firing |
| `src/server/expeditor.cpp:1459-1510` | The publish hook: flush → prepare → fault grant → write grant → `EvictClean` the departed pages, **once, to one core** | PER-CLASS | "One publisher, two callers → three" (§7) understates it: it becomes one publisher, three callers, N sends |
| `src/server/relation_grant_service.cpp:39-57` | Demand re-delivery, keyed on `owner_core == src_core` | PER-CLASS | Must key on "this core serves this class" |
| `src/server/mount_recovery.cpp:139-156` | Post-recovery audit of exactly `{descriptor, var-heap root, anchor}` per relation | PER-CLASS | N sub-anchors and N roots to audit; a missing one is one class unusable, not the relation |
| `include/kds/storage/extent_lease.hpp:102-206` | `ExtentAllocator` — per-core lease, `Reserve` marks allocated, `IsAllocated`/`MayWrite` consult it | KEEP | Already per core: each class allocates from its owner's lease, which is exactly D6. §9 finding 12's cost note stands — core 0 fsyncs its free map per granted extent, so N growing classes cost N× |
| `include/kds/storage/free_map.hpp:29-32` | One bitmap page, 65,280 ids (510 MiB) — the single-page ceiling | KEEP | SF2's contiguity flag is deleted by finding 12's fixed array; nothing here needs to change |
| `src/storage/device_page_store.cpp:240-258` | `MayFault` / `MayWrite` on the frame-load path | KEEP | The backstop for every mis-routed class read or write |
| `src/storage/device_page_store.cpp:500-506` | Stamp claim: "**only this stream's own stamp claims**"; a foreign stamp is refused | KEEP | §9 finding 4's contradiction lives here — a class created under `cores = 2` and mounted at `cores = 4` re-maps to a core whose stamp does not match. Persist the creating `cores` per sub-anchor and refuse the mount |
| `include/kds/exec/assertion_check.hpp:193` | `by_oid_` — the enforcer's per-relation state, core-local | REFUSE | |
| `include/kds/stats/cabin_store.hpp:174-213` | `CabinStore` — per-core, keyed by `cabin_id`, no relation dimension | REFUSE | |
| `src/exec/fk_check.cpp:89` | Forward check: `BtreeLookup(parent.desc_page_id, parent_pk)` | REFUSE | One tree; a stride parent needs `f(pk)` then a possibly-foreign descent |
| `src/exec/fk_check.cpp:137` | Heap parent fallback: `ChainVisit(parent.desc_page_id, …)` | KEEP | Unreachable through DDL |
| `src/exec/fk_check.cpp:198` | Reverse check, Cabin-hint heal: `BtreeLookup(child.desc_page_id, entry.pk)` | REFUSE | |
| `src/exec/fk_check.cpp:318-324` | Reverse check, full walk: `BtreeVisit(child.desc_page_id, …)` — **one whole-relation walk** | REFUSE | Would be N walks on N cores per deleted parent row |
| `src/catalog/foreign_key.cpp:31-36` | An FK across owner cores refused at declaration | REFUSE | |
| `src/server/command_dispatcher.cpp:3024-3050` | The PW1c-5 shape gate: `funded_shape = fkeys_out.empty() && fkeys_in.empty() && !any_cabin && !enforcer_.AnyOn(oid)`, with a named refusal each | KEEP | SF4's fork must sit **after** this so FK/Cabin/assertion refusals keep their names — and the same gate is the natural home for the stride refusals |
| `include/kds/stats/access_stats.hpp:61` · `src/stats/access_stats.cpp` | `sys.access_stats` rows are `{rel_id, kind, column_mask, use_count, last_seen}` — **no core, no class** | DECIDE | And peers record nothing (`core_runtime.cpp:220`, `access_statistics=false`), so a stride relation's statistics cover only the session core's classes. `SHOW ACCESS` would under-report by `(N-1)/N` |
| `src/server/command_dispatcher.cpp:797-855` | `SHOW ACCESS` renders those rows | DECIDE | |
| `include/kds/storage/page_header.hpp:44` · `src/exec/tuple_verify.cpp:24,51` | The relayout epoch is per page; `VerifyTupleAt` compares it, `CurrentRelayoutEpoch` re-reads it | KEEP (+ DECIDE) | Nothing per-relation to replicate. The only stride exposure is a *foreign-class page id* reaching either function through a trail or a Cabin hint (§2e/§2g) |
| `src/server/command_dispatcher.cpp:3331-3348` | `SortedFillEligible`: `clustered_type == kHeap && varheap == invalid && indexes.empty() && cabin_mask == 0 && !enforcer_.AnyOn(oid)` and `!catalog_read_only_` | KEEP | Heap-only, so a BTREE stride relation never enters it. `AllocateRowIdRange`'s contiguous block is exactly the shape a stride partition would scatter — the existing gate already excludes it |
| `src/server/command_dispatcher.cpp:5167` · `:5872` | `HandleUpdate` / `HandleDelete` both pass `CheckWriteAffinity` | DECIDE | §9 finding 2: by-pk forms are SF4's ship-whole shape; by-value forms touch N classes and are R6's door. §3 now says which |
| `src/server/kwp_load_server.cpp:307-320` | The bulk-load path resolves one `TableAccess` per `C_LOAD_BEGIN` and runs the session's own BEGIN/INSERT | DECIDE | A load stream is exactly the "multi-row spanning classes" shape §3 refuses. SF-B(3)'s `stride_b` sweep depends on this path batching within a class |
| `src/exec/cabin_ddl.cpp:28` · `src/exec/assertion_catalog.cpp:347,527` | DDL resolvers that take one `TableAccess` for schema only | KEEP | No page walk |
| `src/exec/assertion_catalog.cpp:97,223,259` · `src/stats/pattern_defs.cpp:93,197,223` | `desc_page_id` walks of `sys.assertions` / `sys.pattern_defs` | KEEP | System relations, `stride_n = 0` |

---

#### 8B.5 The arity answer (SF-V2's deliverable, and what sizes SF5)

**No. One consuming stage cannot take N producer stages today. The producer count is fixed at exactly one in four independent places, and it is fixed structurally — as a type, not as a constant.**

1. **One input edge per consuming pipeline.** `Pipeline::consumer` is `std::optional<InputEdge>`, holding one `input_tag`, one `upstream_core`, one input deque and one `input_eof` bool (`include/kds/server/remote_step_service.hpp:230-236`), installed once at `src/server/remote_step_service.cpp:496-504`. `FindByInputTag` (`:751-756`) is a first-match over that single field.
2. **One producer per open envelope.** `StepOpenUpstream` carries one `upstream_core` and one `enclosed_open` (`remote_step_service.hpp:94-103`); `StepOpenHead` carries one `downstream_core`/`downstream_step` (`:62-73`), and `OpenConsumingStage` refuses any enclosed open that does not address exactly this stage (`remote_step_service.cpp:486-494`).
3. **No discriminator in the tag.** `PipelineTag` is `(request_id, session_core, step_id)` with `static_assert(sizeof == 16)` (`include/kds/server/step_pipeline.hpp:36-45`), and both `Find`s are exact-tag (`remote_step_service.cpp:213`, `session_step_client.cpp:73`). Two producers of one logical step must share a tag and would alias.
4. **The session completes on the first EOF.** `RemoteRead` holds one `owner_core` — the only address credit is returned to (`session_step_client.hpp:52`, `.cpp:94`) — and one `done` bool set by whichever EOF arrives first (`.cpp:102-108`). N producers under one tag would return a silently truncated answer, and their `seq` streams would interleave into the gap assertion `step_pipeline.hpp:53` calls impossible.

**What an N-producer merge needs, in order of cost:**

- **A discriminator.** Either a `producer_ix`/`class` field in `PipelineTag` (a 16-byte layout change, so every `static_assert` in `step_pipeline.hpp` and every payload move) or a distinct `step_id` per class minted at plan time (cheaper on the wire; costs the compiler a `class` on `Step`, `step_chain.hpp:406-412`). **The second is recommended** — it makes N producers N genuine steps, which is what `Find`, `FindByInputTag`, `EdgeCredit` and `InnerBuildStore` are all already keyed for.
- **N credit edges.** `EdgeCredit` is one member of `Pipeline` (`remote_step_service.hpp:202`); the merge needs one per input, and the preallocation ceiling (`kInitialCreditsPerEdge = 4`, `step_pipeline.hpp:110`) becomes `4 × N` batches of buffer at the consumer.
- **N EOFs.** `input_eof` (`remote_step_service.hpp:234`) and `read->done` (`session_step_client.cpp:107`) each become a count-to-N; `actionable` (`remote_step_service.cpp:578-582`) becomes "every live edge has a batch or is at EOF", which is what a k-way merge must block on.
- **An envelope carrying N enclosed opens**, and the "state first, upstream last" ordering (`remote_step_service.cpp:507-521`) repeated N times with a defined unwind for a partial forward.
- **A real merge operator at the session core.** `FinishRemoteRead` concatenates batches in arrival order with no comparator (`command_dispatcher.cpp:3131-3159`). Streaming k-way on the pk needs the pk in the decoded row — free in the P4c star layout (schema order, pk at column 0), not free in the projected layout, which must force the pk into the forwarded set.
- **`emit_in_key_order` on the wire.** It is refused rather than encoded at both `session_step_client.cpp:211-215` and `command_dispatcher.cpp:4689` because `step_descriptor.cpp` has no field for it; a merge whose inputs are not key-ordered is a sort, not a merge. This is a `kStepDescriptorVersion` bump (`step_descriptor.cpp:226,261-267`).
- **Five exclusion lifts** so the shape is reachable at all: `steps.size() == 1`, `star()`-only (single-step) vs projection-required (two-stage), `aggregated()`, `sorted()`, `limit`/`offset`.

**Teardown is the one half that already generalizes:** the session holds every stage and cancels all of them (`session_step_client.cpp:141-162`), and errors match by `request_id` rather than exact tag (`:110-139`) — so §3's "`STEP_CANCEL` upstream at the LIMIT-th row" works over N producers the day the N producers exist.

**Sizing verdict for SF5.** §9 finding 5 is confirmed and can be sharpened: R3 is **not a code prerequisite** — nothing in `step_pipeline.hpp`, `remote_step_service.*` or `session_step_client.*` mentions ranges or `sys.ranges`, and the N-producer generalization is buildable here in full. What SF5 inherits from R3 is only the *work*, not a dependency. Priced: **one wire-version bump (descriptor + envelope), four struct arity changes (`Pipeline::consumer`, `StepOpenUpstream`, `EdgeCredit`, `RemoteRead::done`), one new session-core streaming k-way merge sink, and five exclusion lifts** — plus the two things that are not pipeline work at all but land in the same row: the `ORDER BY <pk>` elision at `step_compiler.cpp:1832-1859` becoming merge-conditional, and `CheckReadAffinity` (`command_dispatcher.cpp:3164-3183`) learning the "some classes local, some foreign" verdict it cannot express today. SF5 is the largest row in the series, not a routing tweak, and the plan's "P4 as-is" sentence must be struck.

## 9. Review of this plan (2026-08-25), re-checked against `9b498d0`

The review was delivered against `main` at `250cd3b` from the worktree
`feat-stride-forest` (removed the same day; it held nothing of its own) and
re-checked in the main checkout against `9b498d0`, read through `git show`.
Between the two lies `e13ad71` ("delete the key mode"), which is **not** §2
D1 as written: `KeyMode` is deleted, but heap relations survive and remain
the `CREATE TABLE` default (`manual/sql/sql.md`: "`EXPLICIT` does not change
the storage default"); who names the key is a per-**row** arity, mixable in
one statement; a heap relation takes a named key at or above its mark and
refuses one below it `OutOfRange`; `KeyOrder {kAscending, kUnordered}`
occupies the mode's byte, flipped once ever by `AdmitExplicitRowId`;
`default_key_mode` is refused at startup by name; and the peer-write
refusal moved from the relation to the row
(`src/server/command_dispatcher.cpp:3563-3571`). Every citation below is a
`path:line` on `9b498d0` unless it names another commit.

**What holds up.** D6 — N complete trees, no shared internal node, no
B-link — is the right structural answer to "one leaf, one owner". D8's
diagnosis is confirmed in code: a named key's admission is a core-0 catalog
write (`src/catalog/catalog.cpp:2114-2225`), and a peer refuses the row for
exactly that reason (`command_dispatcher.cpp:3563-3571`). The census-first
ordering, the publish-hook reuse, the 6b-2/6b-3 parked-waiter shape for
statement shipping, and the fdatasync-overlap flag are sound.

**Findings, ranked. The verdict at `9b498d0` opens each.**

1. **Stands, and D1 is contradicted by `main`.** D1 says every user
   relation is `kExplicit` and therefore btree-clustered; `main` has no mode
   and defaults to heap. D6 needs a btree per class, so a default `STRIDE 4`
   (D2) cannot apply to a heap relation unless this plan also flips the
   storage default — which `e13ad71` deliberately did not. And with every
   relation a stride relation, §4 refuses `CREATE INDEX` everywhere
   (IX01–IX17 unusable), keeps FK/Cabin/assertion relations gated on every
   peer class, and (finding 2) leaves by-value writes with no route. D1 and
   D2 are the operator's; they must be restated against `main`. The reading
   consistent with the tree: `STRIDE n` is opt-in on `BTREE` relations,
   default 1, refusals only when `stride_n > 1`, and class→core is
   `(owner_core + class) mod cores` so `stride_n = 1` is byte-for-byte
   today's relation under today's placement (D4's bare `class mod cores`
   pins every single-class relation to core 0 and discards `rotate`).

2. **Stands.** §3 has no UPDATE or DELETE. `HandleUpdate` and `HandleDelete`
   pass `CheckWriteAffinity` like INSERT (`command_dispatcher.cpp:5167`,
   `:5872`) and DML shipping is unbuilt. By-pk forms are SF4's ship-whole
   shape; by-value forms touch N classes and are a multi-core write — R6's
   door. With default 4 at `cores > 1` that is every `UPDATE … WHERE
   non-pk` on every relation. §3 and SF4 must say which.

3. **Stands.** The N-producer scan is not a snapshot of the relation.
   `docs/spec/crosscore.md` §5 (unchanged by the delta) states the exposure — "a
   scan spanning k ranges is k stages", each minting its own view — and its
   one-view-per-(statement, core) rule is unbuilt (it appears nowhere in
   `workplan-peer-writer.md`); even built it is per core, and the cross-core
   RC weakening "stands". Under stride, two sequential autocommits from one
   client can land in classes on two cores and a scan can show the later
   without the earlier — for a ledger, a visible anomaly. Either name it as
   accepted in §2/§6 and SF7, or gate on the cross-core commit oracle DT9
   and R6 wait on. An operator decision, not a plan-internal one.

4. **Stands.** D4's "runs correctly at `cores = 1`" contradicts the PL
   contract as built: `src/storage/device_page_store.cpp:501-506` — only the
   stream's own stamp claims; a foreign stamp is settled by rule 6's
   acquisition restamp, never a claim — and `:275` refuses the write. Class
   1's pages carry stream 1's stamp; at `cores = 1` core 0 can neither claim
   nor write them, and the reverse (created at 2, mounted at 4: class 2
   stamped by core 0, mapped to core 2) fails the same way. A changed core
   count is `[OPEN]` (`docs/spec/wal.md:46`, `page-lsn-cross-stream.md`
   §9's table). Honest v1: persist the creating `cores` in each sub-anchor
   and refuse a mount at a different value, naming `wal.md` §3.

5. **Stands, narrowed.** SF5's "P4 as-is" understates the read half by most
   of its size. The single-step shipped read admits only star,
   non-aggregated, non-sorted, no-LIMIT statements
   (`command_dispatcher.cpp:4682-4685`); the two-stage form refuses sort,
   quota and aggregate and requires a projection
   (`src/server/session_step_client.cpp:198-207`); `emit_in_key_order` does
   not travel in the descriptor (`session_step_client.cpp:211`,
   `command_dispatcher.cpp:4689`, the wire-version bump deferred by name).
   Narrowed by `e13ad71`: the flag is now set on `key_order == kUnordered`
   (`src/exec/step_compiler.cpp:1856-1858`), so a stride relation fed only
   issued keys ships as today and the refusal bites after the first
   below-mark named key. Under stride at `cores > 1` every relation's rows
   are cross-core, so `COUNT(*)`, `ORDER BY` and `LIMIT` on any relation are
   refused until the merge lands and every exclusion is lifted; the k-way
   merge also needs each producer emitting in key order once the flag is
   set. `PipelineTag` carries one `step_id`
   (`include/kds/server/step_pipeline.hpp:38-44`), so N producers of one
   step need a discriminator — R3's work, as SF-V2 suspects. SF5 is the
   largest row, not a routing tweak.

6. **Stands.** The premise probe runs last; CLAUDE.md says re-measure a
   premise before building the fix.
   `bench/v2.0.0/results-multicore-writers-v2.0.0-48-g314a06d.md:544-547`:
   whether two fdatasyncs overlap on one ext4 device decides whether
   aggregate INSERT is 2× or shared, and PW6 recorded that no multi-writer-
   core speedup has been measured at all. The probe needs no stride code
   (PW6's driver, `--cores 3 --tables 2`, a ≥3-CPU host). Make it SF-V0,
   before SF1.

7. **Stands and grows.** D8's per-insert mark in the sub-anchor makes the
   anchor the hottest page of each class, and the record does not exist:
   `AnchorUpdatePayload{index_oid, root}`
   (`include/kds/wal/log_anchor_update.hpp:24`) carries no mark, so §6's "no
   new record types" is false — a widened or new record is a WAL change to
   list. K3 makes a burned id free, so advance the durable mark by a stripe
   with an in-memory cursor (the `RowIdLeaseTable` block shape); the K0
   findings own the crash rule (logged-ahead never reissues). **Grown by
   `e13ad71`:** `AdmitExplicitRowId` (`catalog.cpp:2114-2225`) now makes
   *two* core-0 catalog writes a named key can trigger — the mark
   (`OverwriteLogged`, per ascending key) and the once-ever `key_order` flip
   (`OverwriteLogged` + `++catalog_version_` + `on_invalidate_()`, a peer
   notification, required because a stale `kAscending` on the session core
   elides `ORDER BY <pk>` wrongly — `include/kds/catalog/schema.hpp`'s
   comment on the field). D8 re-homes only the mark. SF4 ships named-key
   rows to serving peers, where `InsertOneRow` refuses them today for
   exactly these writes, so the flip needs its own route: once per relation,
   a request to core 0 that completes — version bumped, peers notified —
   *before* the row is placed. Add to SF3/SF4.

8. **Stands.** "Max over class marks" for K4, `SHOW BUDGET` and `DESCRIBE`
   breaks this plan's own second law — the marks live in sub-anchors other
   cores own, and `next_id` is still the `sys.tables` field those readers
   take (`include/kds/catalog/catalog.hpp:524-567`). A ring request or a
   per-core figure; state it in SF3.

9. **Stands.** "Retryable" is the wrong class for the R6 refusals. A foreign
   class inside a transaction, or a batch spanning classes, fails
   identically on retry: `Unsupported` with the byte — the class the moved
   peer refusal already uses (`command_dispatcher.cpp:3563`) — not
   `TXN_CONFLICT retryable=1`. `docs/inflight/known-gaps.md:625`'s retryable-bit
   finding is about lease `ResourceExhausted`, not this.

10. **Narrowed, one new case.** "Omitting the pk stays legal" is now every
    relation's rule, so D5 reduces to "issue from the arrival core's own
    class". Unspecified: which class, when a core serves several (at
    `cores = 2`, core 0 serves 0 and 2 — alternating keeps two hot tails for
    nothing; pick the lowest). Unstated: a session's core is
    `SO_REUSEPORT`'s choice (PW6), so the headline needs N connections on N
    cores and a single loader gains nothing — SF-B(1)'s driver must be
    designed for it. **New:** one statement may mix arities per row, so a
    multi-row `VALUES` can span classes by arity alone (named rows route by
    `f(key)`, omitted rows local); §3's multi-row rule must cover mixed
    arity. §3's `LIMIT` sentence is also wrong: the first `stride_b` keys
    sit in one class, so `LIMIT < stride_b` drains one producer and cancels
    the rest — not ~LIMIT/N each.

11. **Largely moot; plan-text deletions remain.** The blast-radius list the
    review gave against `250cd3b` assumed D1 as written. `main` kept the
    heap default, the 639 `CREATE TABLE` sites in 59 test files keep their
    substrate, `default_key_mode` is already gone, and `emit_in_key_order`'s
    per-scan Keystone read now falls only on `kUnordered` relations. The
    physical-optimizer Part I item (`physical-optimizer.md` R8, §4:
    heap-only substrate) and Waystone's heap-case win
    (`waystone-concpets.md:9`: 26–34× on heap, 3–7% slower on btree) return
    only if the operator restates D1 as "`BTREE` default". Deletions now:
    SF1's "`KeyMode` surface removal" is done by `e13ad71` (`ASSIGNED`
    refused at parse with its byte, `EXPLICIT` vacuous); SF-V1's census
    loses every `KeyMode` reader — `AllocateRowId`/`AllocateRowIdRange`
    refuse nothing for a key reason (`docs/rules/keystoneid-invariant.md`'s
    2026-08-25 note) and the sorted fill's gate is per statement
    (`command_dispatcher.cpp:3340-3392`); SF7 targets §4.1's *third*
    amendment; and `keystoneid-invariant.md`'s new "the two readings share
    one monotone mark" paragraph must become per-class under D8.

12. **Stands.** SF2: take the fixed id array and cap `stride_n` (u16 is not
    a cap; N sub-anchors, N producers and N grants need one), which deletes
    the flagged contiguity point. A `CREATE TABLE` rollback after N handoffs
    leaves N own-stamped orphans on N cores — harmless by DROP TABLE's
    precedent, but name it. PW3b (`250cd3b`) has core 0 fsync its free map
    per granted extent, so N growing classes cost core 0 N× that — a term
    for SF-B's model.

**Order of amendment.** 1 and 11 need the operator (D1/D2 restated against
`main`); 3 and 4 need a decision named as open; 2, 5, 7, 8, 9, 10 are plan
text to rewrite before SF-V1 starts; 6 reorders the series. Nothing in this
file changed at the review except this section.
