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
**AR0-5-R is appended after it** (AT-S19, 2026-09-28): the amendment as
built, section by section, and the one text put for ratification. The body
and AR0-5-V stay as written.

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
| AR2 E13 | OPEN, M3 | **closed: yes**, subsumed by §2.1 — *AT-S3 answered **no**: the row's write is an unversioned in-place overwrite outside the caller's transaction, so a tuple `X` there has no contender; `workorder-at-m3-uniformity.md` AT-7 item 11* |
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

---

## AR0-5-R — the amendment as built, for ratification

Written 2026-09-28 by CLA for the operator, as AT-S19
(`workorder-at-close-ar0-5-left-open.md` §4, extended by its §7.4), on
`at-s19-ar0-5-revised` from `3de6d62` (`v2.7.0-450-g3de6d62`). **The body
above and AR0-5-V are left as written: they are the record of what was
proposed on 2026-09-05. This section is the one text put for
ratification**, and where it and the body disagree it states the tree.
Its sections carry the body's numbers, so R2 is read against §2.

Claim tags as in the body. Every claim cites the stage that built it -
its row in `workorder-at-m3-uniformity.md` AT-6 carries the commits and
cells - or the AT-7 item that corrects the body. The `[measured]` claims
are AT-S13's, quoted from its results file, and R8.3's two mutants, run
here at `3de6d62`.

**Written ahead of its gate, on the operator's word of 2026-09-28** ("Draft
now, land after S21"): §7.4 gates S19 on S14-S17, S17b and **S21 landed**.
S14-S17, S17b and S22 are on `main`; **AT-S21 is built on
`worktree-at-s21-log-under-hold` at `801aa61` and not landed**. R8's row
for it says so, and this section lands only after S21 does, with that row
re-read against S21's landed commits.

### R0. The decision, as built

§0's sentence is what was built, with two qualifications the tree adds.

- **The system pages are protected by latches, not by the lock family.**
  The catalog pages are written under the page latch across cores and, within
  one core, under the rule that no task parks under a page span (AT-R11 as
  corrected at AT-S3; `catalog.md` CT5); the free map under `map_latch_`
  (AM-S3); page 0 under the superblock latch (AT-S8, AT-S10b). No lock unit
  protects a `sys.*` row: E13 is answered no (R3). The lock family's part is
  the relation `IS`/`X` around the catalog's *readers* and DDL (R2.1).
- **Core 0 keeps placements, and none is an authority.** The one log
  stream is core 0's object, every peer's manager attached to it and
  appending under its latch (AR0 M0); the delete-mark
  purge runs there (AT-S5, `ddl-transactional.md` §5d); only core 0
  snapshots the instance's assertion registry at a checkpoint (AT-S5d's
  D-1); the Cabin controller's cadence ticks there (AT-S8); the mount's
  recovery runs once per log, on the stream's core (`wal.md`), and the
  registry's resume with it (AT-S5d); and under D19's
  fallback core 0 accepts (AT-S10c). A write on any other core is refused by
  none of them.

### R1. AR0 §2, as built (§1's table)

| property | AR0-5 §1 | as built |
|---|---|---|
| write-serialization authority | unchanged | unchanged. The last owner gate, `MayWrite`'s arm, went at AT-S5 with its replacement named (AT-R11); the predicate itself at AT-S18 (`854aefe`) |
| execution affinity | retained only as a statistic; whether it survives is D18 | **no statistic is stored** (AT-S9, R6's D18): NS10's verb *"declares the affinity of"* (E8), a declaration nothing reads until AS-E. **Nothing routes**: both remote routes retired at AT-S9, and E7 is marked **local** for every verb (the operator, 2026-09-26, `raft-marks-2026-09-26.md` §1) |
| allocator authority | retired: `fetch_add` sequences, page 0 by CAS, authority-free per-core caches | retired, in three shapes rather than one. **Trx ids**: each core carves its own 4,096-id window from the instance's one `SuperBlock`, reading and raising the ceiling **under the superblock latch** - not by CAS (AT-S10b). **Row ids**: no cache; every core bumps `sys.tables.next_id` in place under its page latch, because a per-core block breaks invariant 11's issue-order sequence (AT-S10b; invariant 11 amended on the operator's word, 2026-09-25). **Pages**: the one free map under its latch (`map_latch_`), no per-core extent cache (AW-S1b). **Object oids**, which §2.2 did not list: one `Expeditor` atomic, CAS-seeded and issued by `fetch_add` (AT-S5b, D0) |

AR0 §2's six pk-immutability derivations stand; two lost their object -
there is no route and no new range (AT-S9) - which takes away a reason and
not the rule (invariant 11).

### R2. The three primitives, as built (§2)

**R2.1 The schema version word** (AT-S2a, D21). One atomic on `Expeditor`,
memory only. It differs from §2.1 in three places.

- **Bumped at the catalog write**, before the DDL's `X` is released, and
  **moved again at the decide** of any transaction that wrote catalog rows,
  commit or abort, before it releases its borrows (AT-S5e's C1,
  `Transaction::NoteWroteCatalog`). §2.1 said "incremented once at the
  commit"; a rollback that released first let a woken writer keep a stale
  memo. A btree or index root repoint bumps it too (AT-S5c).
- **Asked at task boundaries, never inside a read** (AT-S2a's review):
  `Revalidate()` at `DispatchAndStage`'s head and the named handlers. §2.1's
  "relation resolution compares the word" was built first and freed a live
  `TableAccess*` inside another core's statement.
- **Written down in `catalog.md`** (AT-S2c; AT-7 item 3): CT1 the
  lock-then-word order, CT2 bump-at-the-write and ask-at-the-boundary, CT6
  the instance's words (AT-0 item 11 carries the struct question).

**The relation `IS` is taken at resolve time** (AT-R1, the operator's
ruling of 2026-09-09; built at AT-S1): the compiler declares every
relation it binds - `FROM`, `JOIN`, subquery - between the name and the
schema, and the three write verbs declare at resolve. **The ask is
non-blocking** (`TryAcquire`): a reader holding `IS` blocks a DDL's `X`,
and a reader arriving while a DDL holds `X` is refused and reads on. That
second direction is made right by DT1, catalog MVCC and catalog-only DDL,
not by the lock (AT-7 item 10); AT-0 item 10 kept it non-blocking on CLA's
proposal under the operator's word of 2026-09-09, and `read_borrow.hpp`
names the condition that reopens it - the first DDL that moves data.
**Retired**: `kCatalogInvalidate`, `InvalidateCatalog()` and the retryable
table-not-found clause (AT-S2a; the clause was `core_runtime.hpp`'s, not
`crosscore.md` §5's, AT-7 item 7); `InvalidateFromPeer` survives as
`DropCache` for the two post-redo drops.

**R2.2 Shared allocators** (AT-S10b, which carried AT-S4).

- **Two lease families retired, not three** (AT-3 H; AT-7 item 1): the
  extent lease went at AW-S1b (`af86026`), before AT; the trx-id and
  row-id leases, their services, refills and spent `TxnConflict`s at
  AT-S10b (`59ed9c0`).
- **After the route, not before** (AT-7 item 12): an allocator's persist
  half is a page-0 or `sys.tables` write, which `MayWrite`'s last arm
  refused to every core but 0 until AT-S5.
- **D20 as built** is R1's allocator row: 4,096 stands for trx-id windows,
  the row-id cache is superseded by invariant 11, and there is no extent
  cache.
- **AN-R13 is kept**, as the body's §3 corrected it: the idle burn is a
  carve on any core (AT-S10b).
- **Priced** `[measured]` at AT-S13 (`v2.7.0-391-gf6f2073`, cell 3):
  omitted-pk inserts spread over `cores = 8` against the same sessions
  pinned to one core cost +9.9 and +12.3 µs of ~360 µs - the one mark and
  the tail leaf together, not separated.

**R2.3 The writer thread** is unchanged. AR0-5 named nothing else about the
log's neighbours, and AT built three: **every core keeps its own
checkpointer and live table, and `wal::CheckpointGate` admits one run at a
time** for the instance, the cadences staggered (AT-S8) - one checkpoint
over one core's table would leave a peer's loser un-undone; a writeback
copies each page under its page latch and cleans only what the copy
covered (AT-S8 step 1b, `Frame::dirty_gen`); and two writebacks of one page
land in copy order (AT-S10e, `Frame::writing`).

**What stays special about system pages** is AT-R11 as corrected at AT-S3,
with page 0 under the superblock latch where AT-R11 said "CAS at S4", and
a system-range page placed by a claim under both latches (AT-S5b, D2).

### R3. Standing items, as built (§3's table)

| item | AR0-5 §3 | as built |
|---|---|---|
| AR0 D3 | struck | struck; AL-R1's every-core-appends and one writer thread |
| AR0 D4 | struck; shared allocators | struck; R1's allocator row. The free map is under `map_latch_`, not "their page latch" (AT-7 item 12) |
| AR0 D10 | the only place affinity is read | provisional 0, **and nothing is stored for it to weigh** (R1) |
| AR0 D11 | `sys.range_affinity` stays absent | unchanged |
| AM-R5 | log-core and free-map clauses struck | as written |
| AR2 R5 | struck: tuple `X` on the `sys.tables` row | **struck at AT-S5, by the page latch and not a tuple `X`**: the admission writes the row in place outside the caller's transaction, so a row lock has no contender (AT-S3; AT-7 item 11) |
| AR2 R12 / E7 | routing loses its target; E7 not closed | **routing retired** (AT-S9); **E7 marked local** after AT-S13's cell 1 (R1) |
| AR2 E13 | closed: yes | **answered no** (AT-S3; AT-7 item 11) - the body's row carries it inline |
| AO-1 | ends at M3 | ended: writes at AT-S5, reads at AT-S6 |
| AO census row 10 | retired with the leases | retired at AT-S10b; a peer's first write no longer answers `TXN_CONFLICT retryable=1` |
| AO-S5 cell | "no `MayWrite` call site exists" | stronger: **`MayWrite` does not exist** (AT-S18; `grep -rn MayWrite include src`, less `Session::MayWriteOn`, answers nothing at `3de6d62`) |
| AN-R13 | kept | kept (R2.2) |
| AN-Q1 / AN-S1 | unchanged | unchanged |
| `core_runtime.hpp` asymmetries 1-3 | struck by M3's close | 2 rewritten at AW-S1b (AT-3 A); 1 and 3 struck at AT-S12 (`f48d213`) |
| `crosscore.md` CC11, CC13 | CC11 struck; CC13 a local write | CC11 rewritten whole at AT-S5; CC13's flush retired at AT-S7 with ring kind 39 |
| the `(M5)` census | twelve sites | eight at `df8cc5f` under the body's own command, more under its rule (AT-7 item 4); **closed at AT-S12** by a grep to the rule, re-run here: `grep -rn -E '\(M5\)\|\bM5\b'` over `include src tests sim tools docs/spec docs/rules manual CLAUDE.md`, less `FK-M5`/`K-M5`/`AR0-M5`, answers nothing at `3de6d62` |
| `page.md` §6 | AM-S5's | rewritten at AT-S12 |
| `cabin.md` §4b; per-core `CabinStore` | one store partitioned by `expr_id` | **one store partitioned 16 ways by `cabin_id`** (AT-S7; AT-0 item 9, AR1 §11's second shape on the operator's word); a reader holds a `CabinSet` handle. §4b's scope rule struck at AT-S7, its span rule at AT-S10a; **§6's owner argument replaced by the announce** (AT-S7) |
| `RemoteCheckpointAnchor` | one checkpoint task, any core | retired at AT-S8 with ring kind 16, **not as one task**: R2.3 |
| Placement (NS10) | retired unless kept as the hint's initial value | retired at AT-S9; `placement` refused by name |
| `core_count` pinning | unpinned once D17 lands | unpinned at AT-S9: nothing names a core on disk, and a mount at another `cores` records the count |

### R4. The retire list, closed (§4)

Every M3 entry, and where it went:

| §4's M3 entry | retired by |
|---|---|
| `MayWrite`'s remaining arms | **one arm** (AT-7 item 2), at AT-S5 (`091be8c`); the predicate at AT-S18 |
| `TrxIdLease`/`TrxIdRefill`/`MaybeRefillTrxIds`; `RowIdLeaseTable`/`RowIdRefill`/`MaybeRefillRowIds`; both services; two spent refusals | AT-S10b |
| `LeasedIdSource`/`ExtentRefill`/`MaybeRefillLease`; `extent_lease_service.hpp`; the extent refusal | **AW-S1b, before AT** (AT-3 A) |
| `kCatalogInvalidate`, `InvalidateCatalog()`, the retryable not-found clause | AT-S2a; the kind struck at AT-S2b |
| `AccessBatch`, `kAccessStatsBatch` | AT-S7 |
| Waystone's peer default | AT-S7 |
| per-core `CabinStore`; `cabin.md` §4b | AT-S7; the span rule at AT-S10a |
| `StatementShipServer`/`Client` for writes; the named-pk ship | AT-S5; **reads too** at AT-S6 (AT-0 item 4, D18's first shape), services deleted |
| `owner_core` semantics; NS10 placement; `core_placement.hpp` | AT-S9; the columns are **reserved bytes**, not dropped (R6's D17) |
| `RemoteCheckpointAnchor` | AT-S8 |
| `core_count` pinned | AT-S9 |
| CC11, CC13, the `(M5)` comments, asymmetries 1-3, `Expeditor`'s "core 0 owns …" | AT-S5 (CC11), AT-S7 (CC13), AT-S12 (the rest) |

The M1 and M2 entries were retired on schedule by the milestones that owned
them (AT-3 A); at `3de6d62` `MayFault` survives only as the record at
`device_page_store.hpp:460`, and the grant machinery not at all.

**Retired by AT beyond §4**, each with its ownership premise: the
cross-owner transaction and 2PC (AT-S6); the foreign-key probe protocol and
its intent tables (AT-S5f); the index and assertion build services (AT-S5e,
AT-S5d); range opening and insert spreading (AT-S9); the remote-step
protocol (AT-S10a); the ring transport, AU-S6's list, AT-S11 struck
(AT-S10d); `peer_listeners` (AT-S8); `in_doubt_ceiling_ms`, re-scoped to
`lock_wait_fault_net_ms` at 1 s (AT-S6); the dead code AT-S6 left
(AT-S18).

### R5. What does not change, as built (§5)

Five of §5's six bullets hold. **The second does not**: *"the range as the
unit of id allocation and export; `range_size_ids`"* changed at AT-S9 - no
range is opened, `range_size_ids` is refused by name, and a relation split
before AT-S9 keeps its ranges, each its own chain, read and written whole.
Beside the others: `Expeditor` holds more of the database than §5 listed -
the assertion registry (AT-S5d), the Cabin store (AT-S7), the optimizer
surface (AT-S8), the oid sequence and the delete-mark count (AT-S5b); and
**G2 is `[measured]`** at AT-S13's cell 2 (`v2.7.0-391-gf6f2073`): +0.1 and
-0.3 µs of ~16.5 µs a statement at `cores = 1` against `df8cc5f`, inside
the old engine's own swing, with a ~0.4 s stall in three of seven runs that
`df8cc5f` never shows (`known-gaps.md`, open).

### R6. The operator items, as marked and as built (§6)

| # | marked 2026-09-05 (`raft-marks-2026-09-05.md` §2) | as built |
|---|---|---|
| D17 | columns dropped on AT's own format event; a pre-M3 value ignored on read | **reserved, not dropped**, on the operator's ruling at AT-S9: written 0, never read, no row size changes, **no format event**; `SysTableRow` gained the offset asserts it lacked. A pre-AT volume mounts unchanged (`SysTableRowTest.TheRetiredOwnerCoreWordIsWrittenZeroAndNeverRead`) |
| D18 | affinity kept as a statistic and optimizer hint, weight 0 until AS-E; NS10's placement deleted | placement deleted; **the statistic is not stored**, because nothing reads it until AS-E - CLA's proposal, stated to the operator at AT-S9 and ratified with this text |
| D19 | every core listens; fallback: core 0 accepts and hands off, a ring consumer AU-S5 must list | every core listens (AT-S8), the TLS/SCRAM refusal removed (AT-0 item 8). **The fallback is built on an inbox and a kick, not a ring** (AT-S10c, `ConnectionHandoff`), AR0-6 having retired the ring (AT-S10d) |
| D20 | 4,096 for trx-id and row-id caches; one extent | R1's allocator row |
| D21 | word in `Expeditor`, memory only, bumped before `X` releases | as marked, and moved again at the decide (R2.1) |
| D22 | M3 is "Uniformity", letter AT | as marked |

### R7. The order, as built (§7)

§7 read *word → catalog borrow → allocators → leases → statistics and Cabin
→ placement → prose*. Built: the `IS` (S1) → the word (S2a-c) → E13, no
unit (S3) → the route (S5) and its five sub-stages (S5b-f) → 2PC (S6) →
statistics and Cabin (S7) → checkpoint and listener (S8) → placement (S9)
→ the ring's consumers, the allocators among them (S10a-e) → prose (S12) →
prices (S13) → the family's windows (S14-S17, S17b, S21, S22) → this
section. **Two reorderings, each for a reason in the tree**: E13 built no
unit, so it gates nothing (AT-7 item 11); and the allocators follow the
route (AT-7 item 12). **AR1's AQ/AR did not ride AT's tail**; they are the
following letter's (AT-0 item 6). The Cabin store's unification did
(AT-S7).

### R8. The quiet-wrong surface is a family (§8, rewritten)

§8 named one surface - a statement executing against a stale schema - and
defended it with the relation `IS`. The defence holds, and the surface is
not one. **The route (AT-S5) opened a family, all of one shape**: a
structure whose soundness rested on one core running a write to completion
is now written by two cores (`workorder-at-close-ar0-5-left-open.md` §1).
§8 also overstated what the lock alone gives: the `IS` defends the
reader-wins direction only (AT-7 item 10, R2.1).

**R8.1 The family**, each member with its defence and its cell, or its
owner if open. The cells named are at `3de6d62` unless the row says
otherwise.

| # | surface | defence | cells | stage |
|---|---|---|---|---|
| 1 | a stale schema executed | the relation `IS` at the bind (AT-R1); the word as fast path (CT1); DDL-wins by DT1, catalog MVCC and catalog-only DDL | **none pins the bind** - R8.3. The word's: `CatalogTest.AReaderDropsOnlyAtItsBoundaryNeverInsideARead`, `AReaderDropsItsCacheWhenTheWordMovesAndOnlyThen`, `TxnSessionTest.ARollbackAndACommitOfADdlBothBumpTheSchemaWord`, `ReadBorrowRigTest.APeerCreatesARelationAndCoreZeroResolvesItAtItsBoundary`; the lock's, below any statement: `ReadBorrowRigTest.ADropOnCoreZeroWaitsForAPositionedReaderOnAPeer` | AT-S1, AT-S2a |
| 2 | the leaf descent | coverage asked of the chain after the exclusive fetch, restarts bounded; the lookup checks coverage on a miss | `BtreeRaceTest.TheLeafChainStillAscendsAfterConcurrentSplits`, `AScanOfTheChainStillReturnsEveryRowInOrder`, `ALookupDoesNotMissARowADivideMovedUnderIt` | AT-S5c |
| 3 | the lookup's payload | `BtreeLookup` returns the held leaf; `VerifiedTuple` keeps its hold | `BtreeTest.ALookupsSlotIsStillItsRowWhenTheCallerReadsIt`, `BtreeRaceTest.ALookupsSlotStillHoldsItsRowWhenTheCallerReadsIt`, `AWriteLookupHoldsItsLeafExclusiveAndItsMissIsAuthoritative` | AT-S14 |
| 4 | the index leaf | `LeafStillCoversKey`, restarts bounded; a stale index root refused. Premise: no index entry is removed (`index.md` IX14) | `IndexTreeTest.ALeafDividedBetweenTheSharedReadAndTheExclusiveRefetchIsNotWrittenPast`, `ALeafDividedBeforeTheDescentReadsItIsNotWrittenPast`, `AnInsertFromARootThatHasSinceGrownIsRefusedOutsideItsSubtree`; `IndexRaceTest.TwoCoresDividingOneIndexLeaveEveryEntryWhereAProbeFindsIt` | AT-S15 |
| 5 | the walk up, both trees | every parent a split writes found and held before it writes, re-descent on a miss (Q1); a root grown over is marked (the operator's choice, `raft-marks-2026-09-28.md` §2) | `IndexTreeTest.APromotionIntoAParentAnotherCoreDividedLandsInTheHalfHoldingItsLeaf`, `ARootAnotherCoreGrewOverIsNotGrownOverASecondTime`, `ALeafRootAnotherCoreGrewOverIsNotGrownOverASecondTime` and `btree_test.cpp`'s twins; `BtreeTest.AnOldRootKeepsItsMarkWhenItIsDividedLater`, `AGrowthRecordsTheOldRootItMarks`; `BtreeRaceTest.TwoCoresPromotingIntoOneParentLeaveEverySeparatorOverItsSubtree`, `IndexRaceTest.TwoCoresDividingOneParentLeaveEverySeparatorOverItsSubtree` | AT-S16 |
| 6 | one name, one row | check and write under one exclusive hold of the relation's root page (Q2, CT7); the check sees another transaction's open drop | `CatalogNameRaceTest`'s seven write cells; `CatalogNameRigTest`'s six | AT-S17 |
| 7 | a relation's namespace | `CreateTable` re-checks the namespace live under page 6; `DROP NAMESPACE` holds page 6 across a RESTRICT over `sys.objects` | `CatalogNameRaceTest.ACreateThatResolvedANamespaceAnotherCoreDroppedIsRefused`, `ADropBetweenACreatesTwoRowsIsRefused`, `ACreateAndADropOfItsNamespaceAtOneInstantLeaveNoOrphan`; `CatalogNameRigTest.ANamespaceDropOnOneCoreIsRefusedWhileAnotherCoresDropOfItsLastRelationIsOpen`, `ACreateOnOneCoreIsRefusedIntoAnotherCoresUncommittedNamespace`. **Two holds unkilled** by any cell (0/10 each), the seam named and not built (AT-S17b's row) | AT-S17b |
| 8 | the assertion directory | one registry for the instance; its directory latch spans the header change and its record; an admission holds its contribution to its reservation | `AssertionRaceTest`'s seven; `ExpeditorTest.APeerThatOwnsAnAssertionMountsAndComesUpEnforcingIt` | AT-S5d |
| 9 | the index build and the assertion build | `CREATE`/`DROP INDEX` and the `CREATE ASSERTION` build take the relation `X`; a writer's `IX` ahead of its admission; the word moved before the decide releases | `DdlFenceRigTest`'s five. **Two of AT-S5e's cells are gone** (R8.3) | AT-S5e |
| 10 | a record logged after its page is released | an insert appends and stamps under the holds that placed it, index entries under the index tree's hold | `insert_log_crash_rig_test.cpp` - windows 1-3, the mid-chain link, two cores spilling into one var-heap page. **On `worktree-at-s21-log-under-hold` at `801aa61`, not landed** | AT-S21 |
| 11 | the FK forward window | **open**. D9(a)'s `S` on the parent tuple at the hoist, held to the decide - ratified as a design, not built (`raft-marks-2026-09-28.md` §6) | `known-gaps.md`, Foreign keys | owner: the following letter (AT-0 item 6) |
| 12 | the in-flight predicate | **open**. `IsInFlight` is the core's live set. A write meeting an undecided holder on another core is **refused**, retryably - a refusal, not a wrong answer (`known-gaps.md`, Locks). `ScanAll`'s DT9 gate is core-local too, and each consumer AT reached is defended beside it: `DROP INDEX` by the relation `X` and the word's move (AT-S5e), a name by `CheckNameFree`'s instance check view (AT-S17), a namespace's RESTRICT by `CheckObjects`' stamp classification (AT-S17b) | `known-gaps.md`, Locks | owner: AX (AX-S1/S2, AX-Q1/Q2) |

**Outside the family, found in its code**: a covering-index probe claimed
a pk before the covered filter ran and dropped a row whose current entry
passed - one core, not AT-S5's - defended by claiming only an entry that
passes (AT-S22;
`IndexContractTest.ARowWhoseKeyAndCoveredColumnMovedTogetherSurvivesItsStaleEntry`
and `AnOldSnapshotKeepsItsRowWhenOnlyTheCoveredColumnMoved`).

**R8.2 Closed inside AT's stages and named by no list.** The family is
wider than the AT-close order's §1 table. These were found and closed by
the stages that opened or met them; the row in AT-6 carries each.

| surface | defence | stage |
|---|---|---|
| two cores' creates issuing one object oid | the instance's oid sequence (`ReadBorrowRigTest.TwoCoresCreatingRelationsIssueDistinctOids`) | AT-S5b, D0 |
| a peer's catalog chain unable to grow | a system-range placement claimed under both latches (`DevicePageStoreOwnershipTest.APeerPlacesAPageAtAChosenIdInTheSystemRange`) | AT-S5b, D2 |
| a peer's delete-marks never purged | the instance's mark count, resettled by a delta (`ReadBorrowRigTest.APeersDeleteMarksAreSweptByTheCoreThatSweeps`) | AT-S5b, D7 |
| a peer descending a relation's CREATE-time root; an index root repoint staled nowhere | the anchor read by every core; both repoints bump the word | AT-S5c |
| an assertion checkpoint snapshot from every core | only core 0 snapshots the registry | AT-S5d, D-1 |
| one transaction under two ids, a read-back seeing none of its writes | a read runs where the session is (`ShippedReadOwnWrite.ATransactionReadsItsOwnUncommittedWriteOnAPeer`) | AT-S6 |
| a Cabin set banked by one core and served short by another | one store; the build announces; the banking gate is the instance's (`CabinStoreTest.AWriteDuringAnAnnouncedBuildIsInTheCommittedSet`, `CabinServeAcrossCores.ASetBankedByAPeerServesThisCoresQuery`) | AT-S7 |
| one access shape admitted twice | `RecordAccess` holds `sys.access_stats`' root page (`AccessStatsTest.TwoCoresRecordingOneShapeShareItsRow`) | AT-S7 |
| a pattern's Waystone directory claimed twice | `ClaimPatternWaystoneRoot` keeps the first (`PatternCatalogTest.ASecondDirectoryIsRefusedAndTheFirstIsAnswered`) | AT-S7 |
| a checkpoint persisting half of another core's write | the copy under the page latch, cleaned only for what it covered (`WritebackUnderAWriterTest`'s three) | AT-S8 step 1b |
| a trx-id ceiling read outside its latch | `Carve` reads and raises it under the superblock latch (`TrxIdSequenceTest.SequencesCarvingFromOneCeilingOnManyThreadsIssueNoIdTwice`) | AT-S10b |
| two writebacks of one page landing out of order | `Frame::writing` (`EvictionWritebackTest.TheOlderOfTwoWritebacksOfOnePageNeverLandsLast`) | AT-S10e |
| a join inside `BEGIN` on a peer blind to its own writes | the pipeline route retired (`ShippedReadOwnWrite.AJoinInsideATransactionOnAPeerSeesItsOwnWrites`, not run against the engine before) | AT-S9 |

**R8.3 The cells the family lost** `[measured]`. AT-S6 (`ac4bd64`) deleted
`tests/txn_2pc_protocol_test.cpp` whole with the 2PC service. The file
also hosted fixtures that test no 2PC: `LockDeadlockTest` (32 cells),
`MidWalkWaitTest` (11), `LockCapTest` (5), `LockCapOfOneTest` (3),
`FailedCommitTest` (2), and part of `Txn2pcBlockedWriterTest`, all five
derived from it and it from `Txn2pcParticipantTest` - the participant
machinery the stage deleted, which is why they went with the file. The
commit's message counts twelve cells retired with their premise, and none
of these is among them. At `3de6d62`, 31 of the 32 `LockDeadlockTest`
names and every one of the others appears nowhere under `tests/`
(`AReadDeclaresItsPositionAndGivesItBack` survives in
`read_borrow_rig_test.cpp`), and no test calls `LockTable::NoteWaitFor`
or `WaitEdgeCount`. Among the lost cells are AT-S1's four bind cells
(family #1), AT-S5e's two (#9), AT-S3's one, the deadlock detector's, the
borrow cap's, the mid-walk park's and `DROP TABLE`'s wait for a reader;
the premise of each of these still holds. **Two mutants, one full suite each, on the
Debug tree built here, both survive at 2991/2991** (one pre-existing
disabled cell):

- **M1**: `step_compiler.cpp`'s bind declares nothing - family #1's
  defence removed. No cell fails.
- **M2**: `LockTable::NoteWaitFor` never finds a cycle - AO-R7's victim
  rule removed. No cell fails.

So at `3de6d62` family #1's defence is **built and pinned by no cell**,
and so is `CLAUDE.md`'s *"a waiter that would close a cycle is refused
naming deadlock"*. `known-gaps.md` (Testing) records this. Restoring the
cells is not this section's work: the operator's.

**R8.4 Open at this writing, with owners.** The AT-close order's §7.2
names AT's quiet-wrong ledger at its close as #5, #6 and #7, which are
R8.1's #11, `a-chunked-assertion-snapshot-can-be-split-by-another-cores-record.md`
(`assertion.md` §7, since AR0 M0) and
`two-cores-growing-one-heap-chain-can-orphan-a-page.md`
(`heap-and-tuple.md` §4.1a, unscheduled while SUS-1 holds; existing heap
relations stay exposed). **AT-S21's survey adds entries** once it lands -
`records-appended-after-their-page-is-released.md` and three beside it -
and one of that entry's rows, `AllocateCatalogPage` writing through a span
whose pin is gone, is wrong without a crash. §7.2's *"nothing else"* is
therefore AT-S20's to re-read and AT-9's to carry; this section does not
carry them.

### R9. AT-7's corrections to the body

The body is corrected by AT-7 items **1** (§4's M3 list is stale in four
entries), **2** (`MayWrite` is one arm), **3** (§2.1's `catalog.md` did not
exist), **4** (the `(M5)` census), **6** (D17 against AR2's E8; D17's
bytes, E8's verb), **7** (§2.1's retryable clause is `core_runtime.hpp`'s),
**8** (AR0 §8 step 6 and D22 define M3 differently; split, not amended),
**10** (§8 overstates what the lock alone gives), **11** (§3's R5 and E13
assume a transactional row) and **12** (§7's allocators before the route).
Items 5 and 9 correct other documents (AU's AU-S5 count; the D19 mark's
premise), and items 13-17 correct AT's own rows.

### R10. For ratification

What the operator is asked to ratify is R0-R9: the amendment as built at
`3de6d62`, with AT-S21's row as it reads once S21 lands.

**Where the tree departs from the body or a mark on it, the departure was
either the operator's already or is ratified here.** Already the
operator's, and recorded rather than re-decided: D17's reserved bytes and
the ranges' retirement (AT-S9's rulings), the row-id cache's removal
(2026-09-25), the Cabin store's shape (AT-0 item 9), the checkpoint's shape
(AT-S8), 2PC's retirement and the `IS` at resolve (2026-09-09), E7 local
(2026-09-26). **Ratified by this text and by no earlier word**: D18's
statistic not stored (R6); the trx-id ceiling under a latch rather than a
CAS (R1); D19's fallback on an inbox and a kick rather than a ring (R6);
the word moved at the decide and asked only at task boundaries (R2.1); and
the core-0 placements R0 lists.

It does not ratify R8.3's lost cells or R8.4's open entries as acceptable;
it records them. The word is recorded verbatim in a `raft-marks-*.md`, and
AT-0 item 1 closes there.
