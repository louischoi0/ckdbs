# Workplan — Stride forest: parallel ascending ingest on explicit-keyed btrees

Drafted 2026-08-25 against `main` at `7fb5492` (`git describe` pending a tag on
that tree). Source citations below are `path:line` on that commit. Lineage:
`docs/crosscore.md` §6b named the problem (naive range ownership spreads reads
while leaving inserts single-core) and answered it with id-block-aligned
per-range chains gated on R3/R4; this plan answers the btree half of the same
problem without R3's directory, under the key-mode decision §2 records. It is
a sibling of `docs/workplan-peer-writer.md`, and it consumes what that plan
built: PW1b's lease pattern, PW1c's write rights, PW1c-7's stamp-carried
ownership, PW2's anchor, PW5's listeners, and the 6b-2/6b-3 request/waiter
wire shape.

Nothing in this file is built. Every task row that lands must state its
worktree and cite its review, per the discipline the PW series set.
Overhead is not measured until the row that measures it (the v2 amendment).

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

- **D1 (operator).** `kAssigned` is removed from the user surface. Every
  user relation is `kExplicit` and therefore btree-clustered
  (`include/kds/catalog/well_known.hpp:434`'s rule, now universal).
  Omitting the pk in INSERT stays legal: the engine issues the id (§2 D5).
  System relations keep their engine-issued heap-chain form; the enum
  survives for them (SF-V1 confirms the boundary).
- **D2 (operator).** The partition is per relation, fixed at CREATE TABLE:
  `stride_n` stride classes, **default 4**. A key belongs to class
  `(key / stride_b) mod stride_n`.
- **D3 (delegated, taken).** `stride_b` (the run length before the class
  advances) is **[OPEN: size]** — a config default measured by SF-B, not
  decided here. The placement arithmetic itself (`/ B mod N`) is fixed by
  this section; only the constant is open.
- **D4 (delegated, taken).** Class→core mapping is `class mod cores` at
  runtime, core 0 included. `stride_n` is a relation fact; `cores` is an
  instance fact; the mapping is where they meet, and it is *not* persisted —
  a relation created at `stride_n = 4` runs correctly at `cores = 1` (all
  classes local, guideline 2's no-regression case) and at `cores = 8`
  (classes 0..3 on cores 0..3). The asymmetry of core 0 carrying both the
  system role and a class is accepted and measured, not designed around,
  until a mover exists.
- **D5 (delegated, taken).** The omit path issues from the **arrival
  core's own classes**: the class chosen is one this core serves under D4,
  so an omitted-pk INSERT never ships. Uniqueness stays proved by the
  descent (`include/kds/catalog/catalog.hpp:544-556`), which makes the
  issuance optimistic-safe: a collision with a caller-supplied id is
  `AlreadyExists` at the leaf, retried with the class's next id.
- **D6 (delegated, taken).** Each stride class is a **complete, independent
  btree** owned by its core: own sub-anchor, own root, own leaves, own
  splits, all from the owner's lease, all own-stamped from birth (PL §9
  rule 4; `docs/workplan-peer-writer.md` §8's growth-pages clause). No
  shared internal nodes exist, so no split ever writes a foreign page and
  no B-link machinery is needed.
- **D7 (delegated, taken).** Sub-anchors are **CREATE-fixed**: core 0
  formats `stride_n` anchor pages at CREATE TABLE and hands each off
  through the existing publish hook (flush → durable `PAGE_HANDOFF` →
  fault+write grant, `docs/workplan-peer-writer.md` §8), with PW1c-7's
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
  retryably, naming R6** — atomicity across two cores is a multi-core
  transaction, which is 2PC's door and stays closed (CC3's residue). A
  batch whose keys all fall in one class (which `stride_b`-aligned loaders
  can arrange) ships whole and works today.
- **Explicit transaction (BEGIN…COMMIT) writing multiple classes**: refused
  retryably, same ground, same wording.
- **Point SELECT**: `f(key)` → one class → local or shipped read (P4's
  existing path; `CheckReadAffinity` learns keys, SF5).
- **Range/full scan**: N subtree scans, each local to its owner, merged at
  the session core — a merge, not a sort, since every subtree is
  key-ordered. Runs on P4's pipeline; SF-V2 verifies the N-producer shape.
- **ORDER BY pk + LIMIT**: the merge consumes lazily; `STEP_CANCEL`
  upstream at the LIMIT-th row (`docs/crosscore.md` §7), which under a
  stride partition stops N producers after ~LIMIT/N rows each.

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
- **R3's `sys.ranges` is not built, consulted, or blocked on.** The
  partition function is the directory. If R3 lands later for read
  placement, stride relations opt out of range splitting (their "ranges"
  are interleaved by construction).

## 5. Task series

| # | Task | Gate |
|---|---|---|
| SF-V1 | **The kAssigned census, verify before build.** Every reader/issuer of the mode and the mark, with sites: `AllocateRowId`'s call sites ("several… only one of them is wrong", `catalog.hpp:529`), `AllocateRowIdRange`'s sorted-fill path (`catalog.hpp:485`, the PW1c-5 revision's "fifth reach point"), the row-id lease funnel (`row_id_lease_service`, PW1b) and its PW7 instrumentation, K4's budget and `DESCRIBE`/`SHOW BUDGET` reads of `next_id`, `emit_in_key_order`'s set sites (`step_chain.hpp:443-457`), `FindSlotForId`'s fallback cost sites (`btree.cpp:132-138`), and the catalog's own self-hosted relations (which stay kAssigned heap — confirm nothing user-facing shares their path). Deliverable: the census in this file's §8, each site tagged keep/retire/route | none |
| SF-V2 | **The one-root assumption census.** Everything that believes a relation has one root/one tree: `InitTableAccess` (`schema.hpp:227-230`'s `anchor_page_id`), the planner's step shapes, `ANALYZE`, `SHOW INDEXES`/`SHOW` walkers, assertion build/recover, cabin optimizer walks, 6b's foreign-arm reads. And the P4 pipeline's producer arity: whether one consumer step can take N producer stages today or R3's "pipeline over ranges" work is prerequisite — this single answer sizes SF5 | none |
| SF1 | **Catalog half.** `sys.tables` grows `stride_n` (u16, 0 = non-stride legacy/system; format bump, mount refuses old versions); CREATE TABLE accepts `STRIDE n`, default 4, refuses on heap-clustered request (which itself is gone from the user grammar — kExplicit-only per D1); `KeyMode` surface removal: user CREATE no longer takes ASSIGNED, `AllocateRowId` becomes system-internal, PW1b's lease funnel marked deprecated for user relations (not deleted — the §6 dedup note in workplan-peer-writer still owns its shape). `TableAccess` carries `stride_n` (cacheable: CREATE-fixed, no ALTER) | SF-V1 |
| SF2 | **Sub-anchor plumbing.** Anchor format bump (one day old — bump now, never cheaper): the page gains `class u16` and `high_water u64` beside `clustered_root`; `nr_index` checked-redundancy discipline (`anchor_page.hpp:53-58`, the 3f07eda C1) applies to every new count. CREATE TABLE formats `stride_n` sub-anchors from core 0's map, publishes each to its D4 core through the §8 publish hook (payload extended past six slots or issued per class — the hook is already the one publisher, two callers); `sys.tables` stores the `stride_n` anchor ids (recommended: one extent-contiguous run so a single base id names them — **the one placement-arithmetic point in this plan, flagged**: contiguity is a free-map allocation property core 0 controls at CREATE, but if the survey work on multi-page free maps forbids relying on it, the fallback is an id array column, and the row format must choose before SF2 lands). PW1c-7's demand path re-delivers a lost sub-anchor grant unchanged — a class's write refusal records `RelationGrantDemand` and the tick asks | SF1 |
| SF3 | **Per-class id issuance and the mark.** The omit path: a per-(relation, class) cursor on the owning core, seeded from the sub-anchor's `high_water`, issuing `base + k·(stride_b·stride_n)`-pattern ids inside the class's stripes; optimistic — the descent's `AlreadyExists` retries with the next stripe id. `AdmitExplicitRowId` re-homed: the mark it advances is the **class's** `high_water` in the class's own sub-anchor (owner-local, logged with `ANCHOR_UPDATE`), and it is advanced by the *serving* core during SF4's execution, never by the arrival core; `sys.tables.next_id` frozen at CREATE as the base; K4/`SHOW BUDGET` re-pointed at max-over-classes (SF-V1's sites). Catalog writes are off the insert path — state it in the header and pin it with a test that counts core-0 ring traffic during a peer's ascending burst: zero | SF2 |
| SF4 | **Write routing.** `CheckWriteAffinity` on a stride relation stops asking `owner_core` and asks `f(key)`: serving class → admit; foreign class, single-row autocommit → **ship the statement whole** to the serving core over a new request/reply pair (the 6b-2/6b-3 shape: POD payload carrying the statement's bound row, parked waiter on the arrival core, deadline, `Status::FromWire`); foreign class inside an explicit transaction, or multi-row spanning classes → retryable refusal naming R6, wire bit set (absorbing PW6 finding (2)'s fix for these sites if not already landed). The refusal/ship fork must sit **after** the shape gate so index/FK/cabin refusals keep their names. `cores = 1` short-circuits before `f(key)` — every class is local — and the single-core benchmark must not move (guideline 2's test) | SF1, SF3; PW5 for multi-listener reality |
| SF5 | **Read routing and the merge.** Point/`kLookup`: `f(key)` narrows to one class before `CheckReadAffinity`, local or one shipped step (P4 as-is). Scan/`kRange`: plan N producer stages (one per class core, each walking its own subtree) into the session core's merge; merge is streaming k-way on the pk since each input is ordered — no buffering past one batch per producer; `ORDER BY pk` is the merge itself, `emit_in_key_order`'s per-page sort still applies within a page (kExplicit slots stay unordered in-page, `btree.cpp:132-138` — unchanged by this plan). Sized by SF-V2's producer-arity answer; if P4 is single-producer today, the N-producer generalization is this row's first half and R3 inherits it later | SF-V2, SF4 |
| SF6 | **Restart and recovery, proven not asserted.** No new record types beyond SF2/SF3's anchor fields — every subtree page is ordinary, own-stamped, stream-locally redone; PW1c-7's stamp claim re-admits each class's pages to its core after restart with nothing granted. The test matrix: a 4-class relation ingested from 2 cores, restarted, read whole and written again (the PW1c-7 restart test's shape, per class); a class whose sub-anchor grant is lost to a full ring re-demanded and re-published exactly once; a crash between a class's root-leaf format and its `ANCHOR_UPDATE` (the class re-formats — the orphan page is the known CREATE-loser shape, `spec-ddl-transactional` §5e's precedent) | SF2-SF4 |
| SF-B | **The number.** `tools/multicore_benchmark.py` grows `--stride`: (1) omit-mode ascending bulk ingest, `stride_n = 4`, cores ∈ {1, 2, host-max} vs the single-core and PW6 baselines — the headline; (2) explicit-id ascending single-row stream at one arrival core — the shipped case, priced against (1); (3) the `stride_b` sweep — per-row shipping vs batch-amortized shipping (P4d-4c's batch runner becomes load-bearing here if (2) matters); (4) the scan/merge cost vs a single-tree scan of equal rows; (5) point-SELECT p50 beside 3 writers (PW7's 48 µs figure, now under stride). `build-release`, `git describe --tags` stamped, on a ≥3-CPU host per PW6's bound — its §7 fdatasync-overlap probe result gates whether (1) can exceed 1× on one ext4 volume at all, and runs first | SF1-SF6 |
| SF7 | **Docs.** `docs/heap-and-tuple.md` §4.1's second amendment: monotonicity is per-class (per-relation → per-range was §6b's concession; this is that concession made real, loudly); `docs/crosscore.md` §6b rewritten to name this plan and drop its R3/R4 coupling for btrees; CC3's cell gains the routing form (a foreign-class write is shipped or refused, never silently wrong); CC7 gains the sub-anchor as a publish-hook consumer; `docs/known-gaps.md` records the secondary-index refusal and the R6 boundary; `CLAUDE.md` open decisions updated (`stride_b` [OPEN], mover×stride noted for R5) | SF1-SF6 |

## 6. Open constants and flagged points

- **[OPEN: `stride_b`]** — SF-B(3) decides. The prior is: large enough that
  a loader's natural batch stays in one class (amortized shipping), small
  enough that a single hot writer still spreads within one bulk statement's
  lifetime. No number is written before the sweep.
- **[FLAG: sub-anchor id contiguity]** — SF2's one placement-arithmetic
  point, decided there with the free-map survey's constraints in view, not
  before.
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

Filled by SF-V1/SF-V2 before any build row starts.

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
   `docs/crosscore.md` §5 (unchanged by the delta) states the exposure — "a
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
   count is `[OPEN]` (`docs/wal.md:46`, `spec-page-lsn-cross-stream.md`
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
   `TXN_CONFLICT retryable=1`. `docs/known-gaps.md:625`'s retryable-bit
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
    physical-optimizer Part I item (`feat-physical-optimizer.md` R8, §4:
    heap-only substrate) and Waystone's heap-case win
    (`waystone-concpets.md:9`: 26–34× on heap, 3–7% slower on btree) return
    only if the operator restates D1 as "`BTREE` default". Deletions now:
    SF1's "`KeyMode` surface removal" is done by `e13ad71` (`ASSIGNED`
    refused at parse with its byte, `EXPLICIT` vacuous); SF-V1's census
    loses every `KeyMode` reader — `AllocateRowId`/`AllocateRowIdRange`
    refuse nothing for a key reason (`docs/keystoneid-invariant.md`'s
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
