# AR0-5 — Amendment to AR0: ownership retired completely; system relations treated uniformly

Status: DRAFT amendment to `instructions/v3.0.0/ar0-architecture-revision.md`,
pending operator ratification
Author: CLA, 2026-09-05, against `410377e`
Scope: AR0 §2 (the ownership decomposition), D3, D4, D10, D11; AM-R5;
AR2 R5/R12/E7/E13; AO-1 and the AO-S5 cell; `core_runtime.hpp`'s three
asymmetries; `crosscore.md` CC11/CC13; `page.md` §6
Claim tags: `[source-read]` with `path:line` at `410377e`; the rest
`[design]`. Nothing `[measured]`.

**AR0-5 continues AR0 §1's decision series** (AR0-1 … AR0-4), which is why
it carries that number and not a work-order letter. **AR0-5-V is appended**:
the source read of 2026-09-05 at `410377e`, which is what the tree says
wherever the body disagrees. Every `path:line` in the body below is the
corrected one; AR0-5-V records what each was in the draft and why it drifted.

---

## 0. The decision (operator, 2026-09-05, verbal)

> No page and no relation has an owner. Core 0 is not a role. Every
> core reads, writes and synchronises through the same primitives, and
> the system relations (`sys.*`, the superblock, the free map) are
> relations and pages like any other, protected by the lock family, the
> page latch and the shared pool — not by a writer's identity.

AR0 §2 decomposed ownership into three properties and retired one. This
amendment retires all three. What replaces the roles core 0 played is
not a smaller role but three **synchronisation primitives** (§2).

## 1. AR0 §2, revised

| property | AR0 §2 | AR0-5 |
|---|---|---|
| write-serialization authority | retired → row locks + page latches | unchanged |
| execution affinity | retained as optimizer hint | **retained only as a statistic** — no code path may consult it for admission, routing correctness, or the right to write. Whether the statistic survives at all is D18 |
| allocator authority — AR0 §2 says **"range-unit id issuance"** (`ar0-architecture-revision.md:39`); page 0 and the free map are **D4**, not this row, and AR0-5 retires both (§3) | retained, "Required by pre-issued id API; cheap; independent of data access" | **retired**. An allocator is a shared structure under the revised G1: a latch on the free map, `fetch_add` on each id sequence, a CAS by *whichever task* crosses the persisted-ceiling threshold to advance page 0's window. Per-core **caches** of allocated blocks remain as an optimisation (D20); they carry no authority, no refill protocol, no refusal, and losing a cached block at crash is today's behaviour |

AR0 §2's six pk-immutability derivations (client-side routing, ABA-free
external references, cascade-free FK, range-unit consistent export,
**Waystone hints**, the pre-issued id API) do not depend on any of the
three and survive; routing and Waystone become performance hints, which
is what AR0 §2 already said of them.

## 2. The uniform treatment — three primitives in place of three roles

**2.1 The schema version word.** One `std::atomic<uint64_t>` in the
instance, incremented once at the commit of any transaction that wrote a
`sys.*` row. Every core's parsed `Catalog` entry carries the version it
was built at; relation resolution compares the word (one relaxed load)
and re-parses from the shared pool's frame on mismatch. This replaces
`kCatalogInvalidate` + `InvalidateCatalog()` + the "peer that has not yet
processed the broadcast answers table-not-found" clause
(`include/kds/server/core_runtime.hpp:62-67`, `crosscore.md` §5).
**Correctness does not rest on the word.** A running statement holds
relation `IS` (AR2 R14, AO-S6); DDL takes relation `X`; a statement can
therefore never execute against a schema a DDL has changed under it,
version word or not. The word is the fast path; the lock is the
argument. Written in that order in `catalog.md` so nobody later removes
the lock because the word "covers it". `[quiet-wrong if inverted]`

**And the `IS` must be taken before the catalog read, which is not where
AR2 puts it today.** AR2's `SELECT` row
(`ar2-architecture-revision-borrow-model.md:193`) scopes the read borrow
to "the statement" but takes it on *the slice the statement is positioned
in* — that is, at first positioning, which is **after** the resolve. A DDL
committing in that window releases its `X` before the reader ever asks for
`IS`, and the stale plan then executes: the word would catch it only "when
the statement next resolves", which is too late, and this amendment
retires the `kCatalogInvalidate` broadcast that covers the window today.
So AT owes one of two things, and must pick in its ruling table rather
than inherit this paragraph: **either the relation `IS` is taken at
resolve time** (before the catalog read, released at statement end), **or
the word is re-read after the first `IS` and before execution**, with a
mismatch re-resolving. Until one is written down the `[quiet-wrong]` tag
above is a hazard named and not yet defended.

**2.2 Shared allocators, per-core caches.**
- Trx ids: one `fetch_add` sequence. `TrxIdLease`, `TrxIdRefill`,
  `MaybeRefillTrxIds` and the spent
  refusal at `include/kds/txn/trx_id_lease.hpp:65` retire. Visibility is
  already commit-LSN ordered (AN-R7 B), so nothing depends on id order;
  the window/floor mechanism of AN-S1 is unchanged.
- Row ids: `fetch_add` per range on a shared table; `RowIdLeaseTable`,
  `RowIdRefill`, the refusal at `include/kds/catalog/row_id_lease.hpp:114`
  retire. The range stays as the **unit** of id space (K-series, export),
  not as a unit of authority.
- Extents / free map: the free-map pages are shared frames; allocation
  is under their page latch; `LeasedIdSource`, `ExtentRefill`,
  `MaybeRefillLease`, the refusal at `src/storage/extent_lease.cpp:139`
  and `include/kds/storage/extent_lease.hpp:194`'s lease retire. Page 0's
  persisted ceilings advance by CAS from whichever task crosses the
  threshold.
- A per-core cache takes a block of ids by one `fetch_add(block)`. That
  is Oracle's `SEQUENCE CACHE n` shape: no authority, block lost at
  crash, no refill *protocol* because a miss is another `fetch_add`.

**2.3 The writer thread.** Already the case since AL-S1a/S1b: every
core stages under the stream latch, one writer thread syncs. "Log core =
core 0" (AR0 D3, AM-R5) was a sentence, not a mechanism; struck.

**What stays special about system pages** — two things, neither an
authority: their frames are **pinned** (read on every statement), and
page 0's address and the fixed catalog page numbers are bootstrap
layout.

## 3. Impact on standing items

| item | before | AR0-5 |
|---|---|---|
| AR0 D3 (log appender) | dedicated log core = core 0 | **struck**; writer thread, stream latch (AL-S1a) |
| AR0 D4 (free map / superblock rule) | allocator authority on the log core | **struck**; shared allocators (§2.2) |
| AR0 D10 (affinity weight) | provisional 0 | unchanged, and now the only place affinity is read |
| AR0 D11 (`sys.range_affinity` from access stats) | affinity table updated by the log core | any core's collector writes it as an ordinary row under lock; or dropped per D18. **`sys.range_affinity` does not exist and stays absent** — AR0-M5 re-scoped `sys.ranges.owner_core` instead (`ar0-architecture-revision.md:455-465`, and the AR2-V source read at `ar2-architecture-revision-borrow-model.md:746`) |
| AM-R5 (`workorder-am-m1-shared-pool.md:189-194`) | **"No change in M1"** — the pool is shared, the log core and the free-map write authority stay where they are | the log-core clause is **struck** by §2.3, the free-map clause by §2.2. "The expeditor owns the pool" is **not** AM-R5 and the first draft put it there wrongly: it is AM-R1 (`:143`, "M1 shares the *cache*, never the *authority*"). Nor does AM-S2 assume this — the `MayFault` removal is **AM-R2's** (`:156-163`, `:225`), and AM-R2 keeps the extent leases and `MayWrite` in the same breath, which is the opposite of assuming the authority moved |
| AR2 R5 (named-pk `INSERT` ships to core 0) | catalog not borrowable | **struck**: tuple `X` on the `sys.tables` row, uniformly |
| AR2 R12 / E7 (execution default) | "local" vs "routed to owner" | "routed" loses its *ownership* target. **E7 does not close here**, and the first draft was wrong to say it did: E7 is `[measurement-gated]` (`ar2-architecture-revision-borrow-model.md:597`) and AO-S7 (`workorder-ao-m2-lock-family.md:440`) says its default is read from the C3 numbers, "not decided" — while this document declares nothing `[measured]`. What AR0-5 settles is narrower: routing is no longer a *correctness* condition. Whether a read is still *placed* is D18's |
| AR2 E13 | OPEN, M3 | **closed: yes**, subsumed by §2.1 |
| AO-1 "owner routing still in force" | scoping sentence for M2 | holds until M3; M3 is where it ends (§5) |
| AO census row 10 (three spent leases → `TxnConflict`) | **kept** — `workorder-ao-m2-lock-family.md:108` deliberately did *not* turn these three into waits: "allocator authority, retained by AR0-4. The range's id block is R5's borrow and its refill is a ring ask — a message wait, not a lock wait" | **retired with the leases**, which is a change to the census's *premise* rather than to its ruling: AR0-4 retained allocator authority and AR0-5 §1 retires it, so the row's ground goes and a miss becomes another `fetch_add` |
| AO-S5 cell ("`MayWrite` admits a page its lease/grant arm refused") | grant arm retired, lease arm kept | both arms retired; the cell's assertion becomes "no `MayWrite` call site exists" |
| AN-R13 (idle burn) | needed because a leased block pins the floor | **kept, and the first draft was wrong to retire it.** The floor is pinned by the **block**, not by the lease protocol: `CoreVisibilitySlot::issue_cursor` is `TrxIdSequence::peek()` (`include/kds/txn/instance_visibility.hpp:124`), so a core that runs no transactions freezes its cursor at its block's start. §2.2 keeps per-core cached blocks and D20 sizes them at 4,096, so an idle core still freezes inside its cache and `MaybeBurnIdleTrxIdBlock` is still load-bearing. **Either the cache goes or AN-R13 stays**, and this amendment keeps the cache — so it keeps AN-R13, re-scoped from "lease" to "cache" in wording only |
| AN-Q1/AN-S1 window & floor | as landed | unchanged |
| `include/kds/server/core_runtime.hpp:59-82` asymmetries 1–3 | catalog read-only on peers; allocation by lease; Waystone records nothing on peers | 1: M1 (read side) + M3 (write side); 2: M3; 3: M3 — all three struck by M3's close |
| `crosscore.md` CC11, CC13 (CR7) | every core reads with the same authority, core 0 alone writes the three fixed structures; access stats fold-and-flush | CC11 **struck** at M3; CC13's flush becomes a local write under lock; the `AccessBatch` ring path retires |
| the "one writer for the fixed pages" rule cited in code as **`(M5)`** | **twelve** sites at `410377e`: `include/kds/catalog/core_placement.hpp:37`, `include/kds/storage/extent_lease.hpp:17,89`, `include/kds/server/range_alloc.hpp:31`, `include/kds/server/extent_lease_service.hpp:41`, `include/kds/server/expeditor.hpp:876`, `include/kds/server/core_runtime.hpp:63`, `src/server/core_runtime.cpp:255,283,322`, `src/server/expeditor.cpp:1607,1649` (`grep -rn '(M5)' include/ src/` is the census; bare `M5` rule references are more) | **struck** at M3, and each comment rewritten. `M5` there is the pre-compaction milestone (resolving against `1769487`), **not** AR0-M5 (`ar0-architecture-revision.md:455`, "D11: the R5 mover is retired") — the two are different items and neither is in `crosscore.md`; CC11 is where the rule is written down |
| `page.md` §6 "multi-core adds instances, not synchronization" (`docs/spec/page.md:106`) | design fact | already false after AM-S2; §6's rewrite is **already AM-S5's** (`workorder-am-m1-shared-pool.md:228` lists `page.md` §6 in its prose deliverable), so this amendment adds nothing to schedule |
| `cabin.md` §4b scope rule (`docs/spec/cabin.md:255`); AK-S2 per-core `CabinStore` | owner's store is the only superset-preserving one | one instance store partitioned by `expr_id` (AR1 §11); scope rule struck |
| `RemoteCheckpointAnchor`, per-core anchors | peers report anchors to core 0 | one checkpoint task, any core, "at most one running" flag; anchors already folded (AL-S3/S4) |
| Placement (`placement = creating / namespace / rotate`, NS10) | chooses the owner | chooses nothing; **retired** at M3 unless kept as the affinity hint's initial value (D18) |

## 4. The retire list, by milestone at which each goes

**M1 (AM), read side**
- `MayFault` (`src/storage/device_page_store.cpp:670`, declared
  `include/kds/storage/device_page_store.hpp:442`), CC11's "system range
  faultable" arm — every page is a shared frame.

**M2 (AO-S5), data pages — the grant arm only**
- `MayWrite`'s **grant arm** (`src/storage/device_page_store.cpp:823`,
  declared `include/kds/storage/device_page_store.hpp:460`);
  `RelationWriteRightsPending` (`include/kds/server/core_affinity.hpp:173`,
  `src/server/core_affinity.cpp:52`), `RelationGrantDemand`
  (`include/kds/server/core_affinity.hpp:184`),
  `MaybeRequestRelationGrants` (`include/kds/server/core_runtime.hpp:420`),
  and the DDL-publish write grants `GrantWritePages`
  (`include/kds/storage/device_page_store.hpp:476-487` — **not**
  `GrantFaultPages` at `:462-474`, which is CC7's read-rights path and goes
  with the read side).
- **`MayWrite` itself stays past M2, and this amendment does not touch
  that.** AM-R2 (`workorder-am-m1-shared-pool.md:156-163`) keeps it "for as
  long as AM-R1 holds, because it is the enforcement of the very rule AM-R1
  keeps — and a debug assertion is not enough: it is the thing that turns a
  routing bug into a refusal instead of corruption", and AO-R14
  (`workorder-ao-m2-lock-family.md:412-414`) says the same in the other
  direction: "the guard goes in M2, the route in M3". Owner routing is in
  force until R12 at M3, so the lease arm is the only thing still enforcing
  it and retiring it at M2 would leave a lost update with no gate — the
  class §8 exists to guard. The **lease arm and the system-range arm**
  (census row 9, `workorder-ao-m2-lock-family.md:107`) retire at M3 with
  the route, in the list below.

**M3 — renamed "Uniformity"** (a work order to be written; provisional letter AT)
- `MayWrite`'s remaining arms — the lease arm and the system-range arm — with the route they enforce (R12, AO-R14).
- `TrxIdLease` / `TrxIdRefill` / `MaybeRefillTrxIds`; `RowIdLeaseTable` / `RowIdRefill` / `MaybeRefillRowIds`; `LeasedIdSource` / `ExtentRefill` / `MaybeRefillLease`; the three `TxnConflict` spent-lease refusals; `extent_lease_service.hpp`, `row_id_lease_service.hpp`, `trx_id_lease_service.hpp`.
- `kCatalogInvalidate`, `InvalidateCatalog()`, the retryable table-not-found clause.
- `AccessBatch` fold-and-flush (CR7/CC13), `RingMessageKind::kAccessStatsBatch` (`include/kds/sched/ring_message.hpp:198`).
- "Waystone records nothing on a peer" — `waystone_recording` peer default.
- Per-core `CabinStore`; `cabin.md` §4b.
- `StatementShipServer`/`StatementShipClient` (`include/kds/server/statement_ship_service.hpp:423,571`) for **writes**; the named-pk ship (AR2 R5). Shipping remains available as a *read* placement choice only if D18 keeps affinity; otherwise retired whole.
- `sys.tables.owner_core`, `sys.ranges.owner_core` semantics; placement NS10; `core_placement.hpp`. The **column** is D17.
- `RemoteCheckpointAnchor`; `Checkpoint()` becomes an instance task.
- `core_count` pinned in the superblock (AL-S2 kept it for `owner_core`) — unpinned once D17 lands; online core-count change stays a non-goal for the pool's sizing reason only.
- CC11 and CC13 in `crosscore.md`, and every `(M5)` comment §3's row lists; asymmetries 1–3 in `include/kds/server/core_runtime.hpp:59-82`; `Expeditor`'s "core 0 owns the superblock, the free map, the catalog pages and the listener" (`include/kds/server/core_runtime.hpp:110-112`) — the listener per D19.

## 5. What does not change

- `Expeditor` holds the database (devices, pool, stream, writer, lock
  table, visibility, schema word); `CoreRuntime` becomes a reactor plus
  caches. The threading rule
  (`include/kds/server/core_runtime.hpp:114-120`) stands.
- The range as the unit of id allocation and export; `range_size_ids`.
- Keystone K1–K5; the three trust classes; AO's refusal census as the
  contract of what waits and what refuses; R4 as the one cap refusal.
- G2: every primitive of §2 is null or a plain increment at `cores = 1`.
- AN-S1's window/floor; AL-S1a/S1b's stream and writer.
- The refusal-census method: every retired refusal in §4 is struck from
  the census with the mechanism that replaces it, not deleted.

## 6. New items for the operator

| # | item | class | CLA proposal |
|---|---|---|---|
| D17 | `owner_core` columns in `sys.tables` / `sys.ranges` (`include/kds/catalog/rows.hpp:104,955`) | format | drop at M3; a pre-M3 volume's value is ignored on read. **Not on AM-S4's event**, which the first draft assumed: AM-S4 is the page-header stamp (`workorder-am-m1-shared-pool.md:227`, "a pre-M1 volume is refused at mount"), and dropping a column is a catalog **row** layout change under its own `static_assert` (`rows.hpp:978`). AT owes its own format event |
| D18 | affinity: keep as a pure statistic + optimizer hint, or delete the concept | spec | **keep as statistic** (D10 weight 0 until AS-E); delete placement NS10; the hint's only consumer is the optimizer |
| D19 | the listener | networking | every core listens (`SO_REUSEPORT`); a session lives on the core that accepted it; no handoff. If the platform lacks it, core 0 accepts and hands off by ring — a networking fallback, not an ownership |
| D20 | per-core allocation cache block size | constant | 4,096 for trx ids and row ids — today's grant, `txn::kTrxIdBlockSize` (`include/kds/server/trx_id_lease_service.hpp:33,50`) and `kRowIdLeasePerGrant` (`include/kds/server/row_id_lease_service.hpp:32`); extents: one extent |
| D21 | the schema version word's home | design | in-memory only, in `Expeditor`; every cache is empty at mount so no persisted value is needed; the word is bumped **before** the DDL's relation `X` is released |
| D22 | M3's name and letter | naming | "Uniformity", work order **AT** |

## 7. Sequencing

No stage before M3 changes. M1 and M2 already remove the read and data
arms of `MayFault`/`MayWrite`. M3 (AT) is where §2's primitives land and
§4's M3 list retires, in this order: schema word (§2.1, with the relation
`IS`/`X` already in from AO-S6) → catalog rows borrowable (E13) → shared
allocators with caches (§2.2) → leases retired → statistics and Cabin
local → placement and `owner_core` (D17/D18) → prose. AR1's AQ/AR and
the Cabin store unification ride AT's tail.

## 8. The one quiet-wrong surface this opens, and its defence

A statement executing against a stale schema is a wrong answer, not a
refusal. The defence is the relation `IS` a statement holds for its
positioned span (AR2 R14): DDL's `X` cannot be granted while it is held,
so a stale parse cannot be *executed*, only *rejected* at the version
check when the statement next resolves. The version word removes a
re-parse; the lock removes the wrong answer. AT's first cell is the
inverted case — the word deliberately not bumped — asserting that the
lock alone still blocks the DDL. That cell is what keeps §2.1's order
true in code, not only in prose.

---

## AR0-5-V — the source read, 2026-09-05 at `410377e`

Every `[source-read]` citation in the draft of this document was checked
against the tree on `worktree-ar2-borrow-model-2` at `410377e`. **Six
were exact, seven had drifted, and one named a rule in a file that does
not contain it.** A `critics-developer` pass over this section then found
that the section itself was the least reliable part of the document — it
had miscounted its own table as six, given a false cause for two rows,
and "corrected" `RelationWriteRightsPending` to a line that is prose in a
comment. Those are fixed above and recorded below; the lesson is that a
verification section needs verifying like anything else. The body above carries the corrected form; this section is
the record of what changed, because a citation that drifts silently is
how a retire list strikes the wrong thing.

**Exact, unchanged.** `extent_lease.cpp:139` (the spent-lease
`TxnConflict`), `extent_lease.hpp:194` (`class LeasedIdSource`),
`row_id_lease.hpp:114` and `trx_id_lease.hpp:65` (the other two spent
refusals) each land on the line the draft named. `page.md` §6 is
"Per-Core Buffer Pools" and carries the sentence quoted, at
`docs/spec/page.md:106`. `cabin.md` §4b is "Authority under a split
relation", at `docs/spec/cabin.md:255`.

**Drifted, corrected.**

| draft | tree at `410377e` | why |
|---|---|---|
| `device_page_store.cpp:651` (`MayFault`) | `src/storage/device_page_store.cpp:670` | +19, and **not** for the reason first given here. The draft blamed AO-S3/S4a; no AO stage touched this file (`git log -- src/storage/device_page_store.cpp` ends at `c985d37`). The shift is **AM-S1's**, the page latch: at `b0157ef`, immediately before it, `MayFault` is at 651 and `MayWrite` at 804 — the draft's exact numbers. So the citations were taken against a pre-`c985d37` tree, which "against `410377e`" obscured |
| `device_page_store.cpp:804` (`MayWrite`) | `src/storage/device_page_store.cpp:823` | same +19, same cause |
| `device_page_store.hpp:372-382` (the grant machinery) | `MayFault` declared `:442`, `MayWrite` `:460`; `GrantFaultPages` `:462-474` and `GrantWritePages` `:476-487`; **`RelationWriteRightsPending` is not in this file at all** — `include/kds/server/core_affinity.hpp:173`, defined `src/server/core_affinity.cpp:52`, which is where AO's own census puts it (`workorder-ao-m2-lock-family.md:103`) | the drafted range is unrelated prose, and the machinery was never one contiguous block. The first correction of this row was itself wrong: it gave `:794`, which is prose inside a comment, and `:465-483`, which starts and ends mid-comment across two different grant functions |
| `core_runtime.hpp:60-66` (asymmetry 1) | `:62-67` | +2 |
| `core_runtime.hpp:60-81` (asymmetries 1–3) | `:59-82` | the block starts one line earlier and ends one later |
| `core_runtime.hpp:117-123` (the threading rule) | `:114-120` | −3 |
| `trx_id_lease.hpp:22` ("today's lease block", 4,096) | `include/kds/server/trx_id_lease_service.hpp:33,50` and `include/kds/server/row_id_lease_service.hpp:32` | **the constant is not in that file at all.** `4096` never appears in `include/kds/txn/trx_id_lease.hpp`; line 22 there is prose about monotonicity. `kTrxIdLeasePerGrant` is `txn::kTrxIdBlockSize`, and `kRowIdLeasePerGrant` is its own `constexpr` |

Every drifted row was a bare basename, and **three of the document's
basenames resolve to a directory the draft implied wrongly** — two of
them in the exact list above, which is why a citation being on the right
line is not the same as it being usable: `row_id_lease.hpp` is
`include/kds/catalog/`, not `exec/`; `core_runtime.hpp` is
`include/kds/server/`; `trx_id_lease.hpp` is `include/kds/txn/` while the
*service* beside it is `include/kds/server/`. The body now carries full
paths.

**The one that named the wrong file.** The draft's §3 and §4 struck
"`crosscore.md` M5" for "one writer for fixed pages". **`M5` does not
appear in `crosscore.md`** — nor anywhere in `docs/spec/`. The rule the
draft meant is **CC11** (`docs/spec/crosscore.md:38`), "Every core reads
with the same authority; core 0 alone writes", and the `(M5)` label is a
**pre-compaction milestone** surviving only in five code comments
(`core_placement.hpp:37`, `extent_lease.hpp:17,89`, `range_alloc.hpp:31`,
`extent_lease_service.hpp:41`), resolving against `1769487`. Meanwhile
`instructions/v3.0.0/ar0-architecture-revision.md:455` defines a *different*
**AR0-M5** — "D11: the R5 mover is retired". Two unrelated M5s, and the
draft's retire list would have pointed a reader at the wrong one. §3 now
carries a row per referent and §4 strikes the comments by path. This is
exactly the collision `CLAUDE.md` means by **cite the file, never the bare
number**.

**One claim strengthened rather than corrected.** §3's D11 row said the
affinity table "is updated by the log core". `sys.range_affinity` does not
exist in the tree and AR0-M5 already ruled it stays absent
(`ar2-architecture-revision-borrow-model.md:746` records the source read
at `183b956`); the row now says so, so AR0-5 cannot be read as reviving a
relation two documents have already declined.

**§6's D20 changed with the citations** — the 4,096 row's correction
landed there, and `txn::kTrxIdBlockSize` is *defined* at
`include/kds/txn/trx_id.hpp:74`, the service header only aliasing it.
**Nothing in §§2.2's design, 5 or 7 changed.** The
corrections are citations only: no primitive, no retire decision and no
operator item was altered by the read.
