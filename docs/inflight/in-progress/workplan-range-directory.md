# Workplan — R3, the range directory, allocation and routing

Scoped 2026-08-27 in worktree `v2.4.0-r3` on branch `worktree-v2.4.0-r3`
at `acb2540` (`v2.2.1-27-gacb2540`); the suite runs **2743/2743** there in
`build-release`. Revised the same day at its `critics-developer` review,
which **falsified this plan's own load-bearing claim** — §2 records what
it was and why it was wrong, because a retracted premise is the thing a
later reader most needs.

The phase is `docs/inflight/in-progress/blueprint-range-ownership.md` §11's **R3**.
The rules it builds are ratified and owned elsewhere: `docs/spec/crosscore.md`
CC8 (the unit), CC9 (the directory), CC10 (the split), §2a (routing), §6a
(the gates), §6b (id-block allocation), §8 tests 9 and 13. **This file
carries only what those do not**: the site inventory, the decisions, and
the two findings the review produced.

Every claim is **source-read** with its `path:line`, or a suite run at the
commit named.

---

## 0. The operator's direction, and what it reverses

> **A range is information the user does not have. The system allocates
> it and enforces it.** At the final blueprint the **physical optimizer**
> may split, set, modify and merge a range.
> — operator, 2026-08-27

The same argument `instructions/v2.4.0/2pc.md` §0 makes one level coarser
about `owner_core` — *"an engine decision. The user did not make it,
cannot see it."* **There is no user-facing range DDL, in this phase or
any later one.**

Three sentences are reversed, named rather than quietly reconciled:

| Site | What it says | Status |
|---|---|---|
| `docs/spec/crosscore.md:379` | `SPLIT RANGE` (working name) refused with the gate's name | **Surface reversed, substance kept.** The gate lives; the DDL does not. The refusal moves to the allocator's admission check (RD4) |
| `docs/spec/crosscore.md:516` | §8 test 13 — refused "with the offending token's byte position" | **Reversed in surface.** No statement, so no token and no byte. The test becomes: the allocator declines, names the gate, relation stays one range |
| `blueprint-range-ownership.md:157` | R3's row, "manual `SPLIT RANGE` DDL" | **Reversed** |

**One `[OPEN]` the direction closes.** `crosscore.md:447` and `:537` leave
*"whether interleaved blocks are the default or opt-in"* open. Opt-in is a
spelling the user would have to write. It reads as **default** — recorded
as CLA's reading of the direction, correctable here.

**What it does not reverse, and in fact points at.** §6b already
specifies system allocation, ratified: *"each core inserts from its own
leased id block, and **ranges align to block boundaries**"*. That
allocator exists — `catalog::RowIdLeaseTable`, `kRowIdLeasePerGrant = 4096`
(`include/kds/server/row_id_lease_service.hpp:27`, the measured K-M2 floor
reused rather than re-decided).

**Who owns mutation, finally.** CC10 already names *"the physical
optimizer's Part III"*; the direction extends its verbs to **split, set,
modify, merge**. Two consequences: **merge stops being open** (blueprint
§7 has it as *"`[OPEN]` and probably v2"*), and
`physical-optimizer.md` §10's out-of-scope line gains a verb set its
unwritten Part III will own. **This workplan owns no range policy.** It
builds the substrate — directory, resolution, routing, per-range chains —
with an internal API whose one caller today is the allocator and whose
second caller later is Part III.

---

## 1. The gate

R3's gate is **R1**. Its three parts are in three states; one blocks.

| R1 part | State | Blocks? |
|---|---|---|
| Per-core listeners | **Built** (PW5: `src/server/tcp_server.cpp:45`, `include/kds/server/core_runtime.hpp:197`) | No |
| Per-core statistics relations | **Not built** — a peer's dispatcher is built `/*access_statistics=*/false` with no recorder (`src/server/core_runtime.cpp:239`; premise at `core_runtime.hpp:65-66`) | **No.** `blueprint-range-ownership.md:122-124` scopes it exactly: per-core statistics are *"a prerequisite of §7"* — migration, split, merge — *"not an optimisation"*. §7 is the mover, which the direction hands to the physical optimizer. They gate **Part III's range work**, not this substrate |
| Shared-structure access rule | **`[OPEN]`** (`crosscore.md` §9, blueprint §8) | **Yes, twice** — see below |

**The shared-structure rule bites in two places, and naming only btree
would be this plan letting itself off.**

- **btree, and it is fatal to that half.** `crosscore.md` §9: the
  mechanism *"gates R3's btree ranges, not only 'every core
  equivalent'"*. CC8: a btree range's *"top levels belong to whoever owns
  the root — that hop is the shared-structure access mechanism, still
  `[OPEN]`"*. So **heap only** in this build, inherited not chosen.
- **The catalog, and it is survivable.** The blueprint states the rule
  over *"**superblock, free map and catalog**"* (`:114-117`), and R3 adds a
  catalog relation every core reads on every routed statement, over the
  residue `docs/inflight/known-gaps.md:697` already names — *a peer reads
  core 0's catalog bytes off the device with no coherence protocol*. R3
  may proceed on it because it changes nothing about that residue: the
  directory is read through the same `ScanAll` over the same fixed pages
  as `sys.tables`, invalidated by the same broadcast, and a peer that can
  resolve a relation today can resolve its ranges tomorrow by the same
  mechanism and with the same staleness. **Stated so that when the
  residue is fixed, `sys.ranges` is remembered as one of its readers.**

Intersected with §6a's four gates, what may take a second range here is a
**non-spilling, unindexed, un-cabined, FK-free, heap-clustered relation** —
§6a's own closing paragraph, and narrow on purpose.

---

## 2. The claim this plan made and the review destroyed

**The first draft argued: a split heap relation's lower ranges can never
take a write, because `sys.tables.next_id` is a per-relation high-water
mark and a heap relation refuses any id below it — therefore R3 is the
read path by force of the storage rules.** It is wrong three ways, and
all three matter to what gets built.

**(a) The mechanism was wrong even for inserts.** A *leased* id is
**below** `next_id` by construction: `AllocateRowIdRange` advances the
mark past the whole block before any id is placed
(`src/catalog/catalog.cpp:2060-2065`), and `AllocateRowId`'s lease branch
then draws from inside it (`:2082-2089`). What actually keeps an insert in
the top range is `ChainInsert`'s tail-append plus `id >= tail.min_key()`
(`src/storage/heap/heap_chain.cpp:99-104`) — a fact about the **chain**,
not about the mark. And the leased path is precisely the cross-core one
R3 is about.

**(b) Writes reach any page in the chain, on four routes.**

- **UPDATE overwrites in place, anywhere.** `src/server/command_dispatcher.cpp:6280-6287`
  walks the whole relation with `PageAccess::kWrite`; the pk fast path
  writes through `page_store_.Get(found.at.page_id)` at `:6264-6267`.
  `include/kds/storage/heap/heap_chain.hpp:203-204` says it outright:
  *"The PageView handed to `fn` is mutable so a scan can overwrite in
  place - UPDATE's HOT path does exactly that."*
- **DELETE delete-marks in place** — `command_dispatcher.cpp:6821-6828`, `:6810-6813`.
- **Rollback writes the page the undo record names** — `src/txn/manager.cpp:220,352-360`,
  `src/txn/recovery_undo.cpp:33` (`store.Get(rec.target_page_id)`).
- **Redo** applies per target page id at mount.

**(c) The spec already ratifies the refusal the claim said was
unnecessary.** `crosscore.md:311-314`: *"a DML on a split relation whose
predicate does not bound its rows to one owned range is a cross-core
write, refused retryably until 2PC — the widened CC3 refusal."* The draft
would have licensed R3 to skip building it.

**What survives, and it is what §6b already said.** Per-range chains are
the mechanism, not the mark: each range is its own chain with its own
tail, *"the per-range sub-structures CC8 names, which are the real work
here"* — CC8 calls them *"R3's largest piece, named here so they are
built, not assumed"*. The chain is friendly to this: it has **no object,
only entry points that each take their head as a parameter** —
`heap_chain.hpp:100` (`ChainTail`), `:104` (`ChainLength`), `:138`
(`ChainInsert`), `:172` (`ChainAppendBatch`), `:205` (`ChainVisit`),
`:226` (`ChainVisitOnePage`). Plural chains cost a plural head list.

### 2a. How big a range is — an operator hypothesis, settled by measurement

> *"Ideally range size is up to extent size — it is just my hypothesis,
> later by benchmarking choose basic range size."* — operator, 2026-08-27

The argument behind it is real: the **extent is already the per-core page
allocator's unit** (`kDefaultExtentPages = 64` at
`include/kds/storage/page_device.hpp:44` — 64 × 8 KiB = **512 KiB**), so a
range whose pages come from one extent lease keeps allocation, faulting
and write rights core-local with nothing straddling — the granularity
`MayFault` and CC7's grants already use.

**The unit mismatch is what the measurement must resolve**, stated so
nobody hardcodes past it:

| Candidate | Unit | Constant | Rows covered |
|---|---|---|---|
| Row-id lease block | **id space** | `kRowIdLeasePerGrant = 4096` | exactly 4,096, any width |
| Extent | **page space** | `kDefaultExtentPages = 64` | 64 × rows-per-page, varies with row size |

A boundary is an **id**, so an extent-sized range is a *derived* boundary,
exact for one row width only. Invariant 13 makes row size a schema
constant (`RowLayout::Build`), so it is computable per relation — what it
cannot be is one engine constant. Therefore: **one named `constexpr` with
its derivation in a comment, reached through one function, swept by
config**, starting at `kRowIdLeasePerGrant`. Never a literal at a call
site. Measured by RD9 (**D6**).

### 2b. Publication — which mechanism, and the constraint that decides it

`docs/inflight/known-gaps.md:650-660` names four catalog relations written
with **no `BumpVersion` and no broadcast**, safe only because no peer
reads them, closing *"Enable any one of them on a peer and this bug
returns on a chain nothing invalidates"*. `sys.ranges` is read by every
routing core, so it may not join them.

Two mechanisms exist and the review's finding is that the choice turns on
**where allocation runs**, not on taste:

- **Plain `BumpVersion`** (`src/catalog/catalog.cpp:918-930`) — what every
  catalog DDL ends in (`:1059,1136,1156,1516,1558,1739,2674,2697,2781,3012,3079`).
  It drops the whole cache, which is fine at a statement's end and fatal
  mid-statement.
- **The `key_order` in-place shape** (`:2259-2263`) — the only place a
  non-DDL fact that another core reads publishes itself, `++catalog_version_`
  inline plus the hook, precisely because the flip *"runs inside an
  ordinary INSERT"* holding a `const TableAccess*` (`:2237-2243`). Its
  first form did bump and the dangling access re-read a freed pk
  (`heap-and-tuple.md` §4.1 records the symptom).

**Under the operator's direction there is no DDL, so this is a live
question rather than a settled one.** The refill that would open a range
runs on the drain tick as a `kSystem` task
(`src/server/core_runtime.cpp:1006-1016`), *outside* any statement's
borrow — which admits the plain bump. But `RowIdLeaseTable::Next` records
demand from **inside** a running INSERT (`row_id_lease.hpp:88-95`), so a
design that opened a range at the point of demand would be in `key_order`'s
situation exactly. **RD3 decides, and RD5's shape is what forces the
answer** — the constraint, not the preference, is what this section
exists to hand it.

### 2c. The cache-generation prerequisite is the spec's own scope, not a
      narrowing

`crosscore.md:107-113` already says it: the counter must exist *"before
any code caches a resolved range **across a park**"*. `InvalidateFromPeer`
clears content without advancing `catalog_version_`
(`src/catalog/catalog.cpp:1034-1044`), deliberately. So the rule this
phase holds itself to — **a resolved range set is a plan-time value,
re-resolved after any park, never re-validated with `catalog_version()`**
— is compliance with §2a, not an amendment of it. The draft presented it
as a disagreement; it was not.

---

## 3. The format cost, stated before it is built

A bootstrap catalog relation is a **superblock bump, 15 → 16, and every
pre-existing data file stops mounting** (P0's development-stage policy).

- `include/kds/server/superblock.hpp:176` — `kSuperBlockVersion = 15`.
- The root claims fixed page **15**, today's `kCatalogOverflowFirst`
  (`include/kds/catalog/well_known.hpp:326`), which moves to 16.
- The 12 → 13 ledger entry is the same situation letter for letter:
  *"A version-12 file that ever outgrew one of its nine catalog roots put
  a `sys.columns` or `sys.tables` overflow page at id 14 — and this build
  would create sys.assertions on top of it. That is not a missing
  relation, it is a silently overwritten one, which is the worst failure
  any bump on this list has protected against."*
- The 7 → 8 entry states the general form: *"a **new bootstrap relation**
  is as breaking as a row layout change and less obvious about it."*

Priced alongside: each new root shrinks the catalog overflow range by one
page, ~68 `sys.columns` rows off the instance ceiling (`well_known.hpp:313-325`).

---

## 4. Decisions this plan does not take

| # | Decision | Owner | Blocks |
|---|---|---|---|
| **D1** | **The shared-structure access mechanism** (§1) | `crosscore.md` §9, blueprint §8 | **btree ranges entirely** |
| **D2** | **Where a range's entry page is recorded.** CC9 says the directory row. The anchor page already holds `{u64 key, u32 root}` entries, capacity **679** (`anchor_page.hpp:42`), plus one bare clustered-root slot at offset 32 — but keyed on `index_oid`, whose `0` is that slot's sentinel (`catalog.cpp:1106-1111`), which a range at `lo = 0` collides with | `crosscore.md` CC9 | RD2. **This plan follows CC9**; the anchor is recorded as declined with the collision as its reason |
| **D3** | **Range policy** — when to split, when to merge, the migration trigger | `physical-optimizer.md` Part III (unwritten) | Nothing. RD6 exposes the API; policy has no caller here |
| **D4** | **Fan-in identity in the pipeline tag** (§5). Inside CLA's latitude; listed because it grows a wire form | this plan | RD7 |
| **D5** | Whether a **peer session** may open a multi-range read. `SessionStepClient` is built **on core 0 only** (`expeditor.hpp:660`, `expeditor.cpp:1374`); `CoreRuntime` never builds one | `crosscore.md` §2, PW5 | Nothing — inherited. Named so RD9's file does not read as general |
| **D6** | **The basic range size** (§2a) — extent hypothesis against `kRowIdLeasePerGrant`, different units | operator, on RD9's numbers | Nothing; RD5 builds it as one swept constant |

---

## 5. The fan-in — the review killed the cheap answer

`crosscore.md` §2's four clauses are all true at the code: first EOF ends
the read (`src/server/session_step_client.cpp:102-108`, no counter);
exact-tag matching everywhere (`session_step_client.cpp:73`,
`src/server/remote_step_service.cpp:213`, `:751`, `:975`); `seq` is
producer-side only (incremented `remote_step_service.cpp:815`, **no
receiver reads it**, so `step_pipeline.hpp:52`'s *"asserted, not handled"*
is in fact neither); and no fan-in open.

**The draft proposed giving each sibling its own `step_id`. That is
wrong and the review proved it.** `step_id` is not a free field: the
compiler assigns it (`src/exec/step_compiler.cpp:1363`), it is *"global
across the whole statement, in compile order"*
(`include/kds/exec/step_chain.hpp:406-409`), it rides the descriptor body
a second time (`src/server/step_descriptor.cpp:227`), and it indexes
`StepStats` (`include/kds/exec/step_vm.hpp:204-217`), the trail-replay key
where it occupies the top 24 bits (`include/kds/exec/trail_replay.hpp:41,125-126`),
the collector where it narrows to `uint16_t`
(`include/kds/exec/trail_collector.hpp:70`), and `ANALYZE`'s printer.

**The shape RD7 builds instead: `PipelineTag` grows a `sibling` field.**
It is an in-process POD under `ring_message.hpp`'s stated exception, so
growing it is allowed where growing a persisted struct would not be.
`step_id` keeps its compiler meaning — which is the whole reason this
beats the overload. Four costs, none hidden:

1. **`PipelineTag` 16 → 24 bytes**, `StepBatchHeader` 24 → 32
   (`step_pipeline.hpp:36-45,53-58`). Every exact-tag site then keeps
   working *because* siblings differ in the tag: `Find`, `FindByInputTag`,
   `Erase`.
2. **`InputEdge` must become plural.** A consuming stage holds
   `std::optional<InputEdge>` — one `input_tag`, one `upstream_core`, one
   `input_eof` (`remote_step_service.hpp:230-236`), set once from the single
   enclosed open (`remote_step_service.cpp:501`). With k upstreams,
   siblings 2..k's batches hit `FindByInputTag`'s *"no consuming pipeline
   wants it: §3's silent discard"* (`:751-756`) and vanish. This is the
   fourth clause the draft verified and then omitted from its own cost
   list. Credit and cancel are single-tag too (`:740`, `:547`, `:605`).
3. **`DispatchOutcome::pending_remote` must become a group.** It is
   `std::optional<PipelineTag>` (`include/kds/server/command_dispatcher.hpp:206`);
   the park predicate resolves one tag and completes on `read->done`
   (`command_dispatcher.cpp:229-240`); `FinishRemoteRead(tag)` frames one
   read; the synchronous path at `:342-355` likewise.
4. **`OpenConsumingStage`'s cross-check survives** (`remote_step_service.cpp:486-487`)
   provided every sibling's enclosed open names the same `downstream_step`.

What stays free: `OnStepError` already groups by `(request_id,
session_core)` ignoring `step_id` (`session_step_client.cpp:110-127`), so
"an error anywhere is the statement's error" needs no change; and no ring
**header** change is needed. Cross-sibling row order is **range order**,
which §2 prices, and this build sidesteps the price by shipping only
shapes that do not require key order — as `command_dispatcher.cpp:5382`
already refuses `emit_in_key_order`.

---

## 6. A defect found on the way — reproduced, fixed, retired

**Closed 2026-08-27, and this section is now the closure note rather than
the report.** `docs/inflight/bugs/README.md`'s rule is that a report lives
only until the fix lands with its test, then is deleted and what it taught
goes to the spec that owns the subsystem or to `known-gaps.md`. `dcdc5e5`
performed that deletion, so re-telling the mechanism here would restore in
a workplan precisely what the rule removed from `docs/inflight/bugs/` — and
the retelling had already begun to rot: every `path:line` in it was
pre-`7148343` and none still resolved. The pre-fix narrative is in git at
`29593ac` and in `7148343`'s message, with the citations that were true
when they were written.

**What happened.** RD0(a)'s probe ran and reproduced: a cross-core read of
**42 rows or more answered zero rows, silently**, the batch having exceeded
the 1,024-byte ring slot. So the framing question this section used to pose
was answered in favour of `docs/inflight/bugs/` — a report with a fix, not
a `known-gaps.md` entry. The reproducer landed with the report at `5a9bfd0`
(`CoreRuntimeTest.AStepBatchWiderThanTheRingSlotStillDeliversEveryRow`) and
the fix at `7148343`; three commits then **corrected and completed** it
rather than polishing it — `beec260` replaced `7148343`'s *predictive* seal,
which turned the silent loss into a wrong `ERR` on any schema whose rows
vary in width, with an exact place-or-rollback; `f448e1f` made the sender
and its ceiling one required argument; `44bdd2f` applied that review.
`dcdc5e5` then deleted the report.

**What outlived the fix is not here.** The conditions that let a silent
wrong answer survive, and the two transferable shapes the incidental
defects left, are in `docs/inflight/known-gaps.md`; the subsystem rules are
in `docs/spec/crosscore.md` §7 and `docs/spec/sched.md` §5.

**What R3 inherits: nothing.** The defect is fixed on `main`, and no
remaining row of this plan depends on it.

---

## 7. Task series

**`[D]`** marks a row a §4 decision blocks.

| # | Task | Gate |
|---|---|---|
| ~~**RD0(a)**~~ | ~~**Probe and record, before building.** The §6 reachability probe at production sizing — a cross-core read whose batch exceeds `kCoreRingPayloadBytes`, asserting on row count against the same statement locally; its outcome decides `docs/inflight/bugs/` versus `known-gaps.md`.~~ **Closed 2026-08-27 — it was already done before this row was picked up.** The probe ran and reproduced at 42 rows under the test that landed with the report at `5a9bfd0`; `7148343` fixed it and pinned the pipeline leak it exposed with a second test; `dcdc5e5` retired the report. The framing question was answered **bug report**, and the residue is in `known-gaps.md`. Not owed work | none |
| **RD0(b)(c)** | **The doc half, still owed.** (b) The §1 gate reading and (c) §0's three reversed sentences, amended in place in `crosscore.md` §6a/§8 and the blueprint's R3 row. Carried as **RA1** of `instructions/v2.4.0/range-foundation.md` §5 | none |
| **RD1** | **`sys.ranges` exists and is empty.** Oid **133** (verified free: table oids are 100, 110-116, 130-132; the column-oid bases run 120-123 and 140-145), fixed root page **15**, `kCatalogOverflowFirst` → 16, `kSuperBlockVersion` → **16** with a ledger entry quoting the 12 → 13 precedent. Joins all five exhaustive lists: `kAllWellKnownOids` (`well_known.hpp:215-230`, compile-gated), `kAllCatalogPages` (`:291-296`), the `static_assert` at `:333`, `Bootstrap()`'s `kSysTables` (`catalog.cpp:532-557` — note the hard-coded `std::array<…, 9>`), and the `DropTable` sweep chain (`:1704-1736`, or rows outlive the relation). Fixed-offset row per `SysCabinRow`'s template — every field fixed-width. **`tests/assertion_catalog_test.cpp:109` is `EXPECT_EQ(kCatalogOverflowFirst, 15u)` — an exact pin this row breaks and must edit.** That file's own comment at `:102-106` argues exact pins are the wrong shape (which is why `:107` is `>=`); the same reasoning applies to `:109`, and it was missed once already at 13 → 14 | none |
| **RD2** `[D2]` | **The directory row.** `SysRangeRow{rel_oid, lo, owner_core, entry_page}` per CC9. What this row adds beyond CC9's cell: the `lo = 0` and derived-`hi` rules are **enforced at the catalog door rather than assumed**, D2 is taken, and the anchor's `index_oid == 0` collision is recorded as its reason | RD1 |
| **RD3** | **Resolution and publication.** `ResolveRanges(rel_oid, predicate) -> {owner_core, entry_page}[]`, plan-time, from the session core's cache (§2a of the spec). Publication decided per §2b — and **the choice is forced by where RD5 allocates**, not preferred. §2c's plan-time-only rule enforced by shape. **The zero-cost invariant binds hardest here** (*"a one-range relation on its owner core must add zero instructions over today"*): the unsplit path gains no scan, no lookup, no allocation, and RD9 measures it rather than an inspection asserting it | RD2 |
| **RD4** | **The gates, declined by name (§6a).** `RangeEligible(access)` over four fields already on `TableAccess` — `SchemaCanSpill(schema)` (`src/catalog/schema.cpp:29`), `indexes.empty()` (`schema.hpp:371`), the **live-id** `cabin_ids` test `CheckWriteAffinity` uses at `command_dispatcher.cpp:3631-3633` (**not** `cabin_mask != 0`, **not** emptiness — both are the wrong test, stated there), `fkeys_out.empty() && fkeys_in.empty()` (`schema.hpp:311-312`) — plus D1's btree decline. Per §0 a decline is a logged engine decision naming the gate, with no token and no byte. Built and tested **before** anything can allocate a second range | RD2 |
| **RD5** | **Allocation — the system's half.** A second range opens where the row-id allocator already carves a disjoint block, so the boundary is that block's `first_id` and CC10's page-boundary rule is satisfied *vacuously* — §6b's own words, *"the new range starts as its own empty sub-structure (CC8) and no existing page straddles it"*. Rides `AllocateRowIdRange`/`RowIdLeaseTable` unchanged; the row and the entry page are what is new. **The size is one named constant reached through one function and swept by config** (§2a, D6), starting at `kRowIdLeasePerGrant`. `RangeEligible` asked first, always. **This row's shape is what answers §2b** — whether it runs on the drain tick (`core_runtime.cpp:1006-1016`, outside a borrow) or at the point of demand (`row_id_lease.hpp:88-95`, inside a running INSERT) | RD3, RD4 |
| **RD6** | **Per-range chains, and the insert head (§2's survivor, CC8's "largest piece").** Each range is its own chain with its own head and tail. **The review's blocking finding lives here**: `sys.tables.desc_page_id` is CREATE-fixed (`catalog.cpp:2266`) and *every* insert path uses it as the head — `ChainInsert(page_store_, access.desc_page_id, …)` at `command_dispatcher.cpp:4466`, `ChainAppendBatch(…, ta.desc_page_id, …)` at `:4091`. A cut that clears the predecessor's `next_page_id` leaves `desc_page_id` heading the **lower** range, `ChainTail` returns that range's last page, and since every issued id is above its `min_key`, `ChainInsert` **accepts the row there** — no refusal fires, and the pk then routes the reader to the top range for a zero-row answer. `heap_tail_hint` cannot mask it: a hint from another chain *"is a logic error upstream that this layer cannot detect"* (`heap_chain.hpp:120-125`) and it dies with the cache entry (`schema.hpp:191-192`). **So this row's substance is that the insert head comes from the directory, per range, and `heap_tail_hint` becomes per range with it** — the cut is what *creates* the route, not what closes it. Plus the mutation API Part III will call (split / set / modify / merge, §0), one caller today, no policy | RD5 |
| **RD7** `[D4]` | **The pipeline over ranges** — §5's shape, with all four of its costs built and none assumed: the `sibling` field, the plural `InputEdge`, the grouped `pending_remote`, the same `downstream_step` across siblings. Range-order concatenation; key-order-requiring shapes refused as `emit_in_key_order` already is. Inherits D5 — core-0-sessioned only | RD3, RD6, D4 |
| **RD8** | **§8 test 9 — range equivalence**: every shippable shape over a split relation returns byte-identical results to the same rows unsplit on one core, the split the only variable, **matching rows straddling the boundary**; test 1's discipline, home `tests/core_runtime_test.cpp:1397+`. **§8 test 13** in its §0-amended form. **Plus the two the review's findings owe**: a post-split INSERT lands in the range its id names (§2, RD6), and a cross-range DML meets `crosscore.md:311-314`'s ratified retryable refusal rather than a wrong answer | RD7 |
| **RD9** | **Measure, and choose D6.** Three cells. (a) RD3's zero-cost and RD8's fast-path claims. (b) **The range-size sweep** — 4,096 ids against the extent hypothesis and the sizes either side, read on both axes the size trades between: directory rows and non-pk fan-out at the small end, single-core concentration at the large end (CC8's stated reason for rejecting relation granularity). (c) The k-range read against the same rows unsplit, which prices the fan-out §2a says the gating discipline makes unavoidable in the first build. `build-release`, interleaved A/B, per `ck-tester`; results to `bench/<version>/` naming `git describe --tags` | RD8 |

**Not in this phase**: insert *spreading policy* (§6b's remaining half —
this builds the chains, not the policy that fans writes across them); the
optimizer's range verbs beyond RD6's API (Part III, D3); multi-range
transactions (R6, `instructions/v2.4.0/2pc.md`). §8 test 10 lands with the
peer writer — §8's row 10 says so.

---

## 8. Where to pick this up

At `acb2540`, **nothing is built** — with one correction made 2026-08-27:
**RD0(a) is closed**, and was closed before this plan was picked up, by
`7148343` on `main` (§6). **RD0(b)(c), RD1 and RD4 are unblocked**, and
under `instructions/v2.4.0/range-foundation.md` they are RA1, RA2 and RA3.
RD2 wants D2, RD7 wants D4, and **D1 removes the btree half entirely** —
it is `crosscore.md` §9's, not this plan's. D6 blocks nothing: RD5 is
built so choosing it is a config value, not a rewrite.
