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
as CLA's reading of the direction, correctable here. **Closed in place
2026-08-27** (RA1, worktree `v2.4.0-range-foundation-1`): `crosscore.md`
§6b now states the default with this reading and its §9 entry is struck;
the sweep found the `[OPEN]` indexed in two more places than the two
sites above name — `blueprint-range-ownership.md` §12 and `CLAUDE.md`'s
Open Decisions cross-core line — both amended in the same stroke so no
index contradicts the spec.

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

Intersected with §6a's gates — four at scoping, five since C2's
enumeration added assertions (§9) — what may take a second range here is
a **non-spilling, unindexed, un-cabined, FK-free, un-asserted,
heap-clustered relation** — §6a's own closing paragraph, and narrow on
purpose.

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

### 3a. C1 — the non-bootstrap alternative priced at mount, and the conclusion

Gate C1 of `instructions/v2.4.0/range-foundation.md` §3, closed 2026-08-27
in worktree `v2.4.0-range-foundation-1` at `6ce9b6e`, **before** the RA2
edit it gates. The 15 → 16 epoch is irreversible for every existing file,
so the alternative — a directory that needs no bump — is priced at
**mount**, where CC9 requires the directory readable before any statement
runs, and refused on the two costs below rather than by omission.

**(a) `sys.ranges` as an on-demand relation** — created like a user
relation on first split, root from the general page supply.

1. **A peer core could not read it, and that is fatal rather than slow.**
   A general-supply root sits at or above `kFirstUserPageId` (128); every
   core's store is armed `SetCoreOwnership(core, lease, kFirstUserPageId)`
   (`src/server/core_runtime.cpp:202`), and `DevicePageStore::MayFault`
   admits only ids below that limit plus explicitly granted leases —
   grants that exist for leased *writes* (CC7), not for a catalog every
   routing core must always read. `well_known.hpp`'s overflow-range
   comment states the rule for exactly this case: *"a peer that cannot
   read the catalog cannot resolve a relation at all."*
2. **The invalidation flush must name every catalog page, and a dynamic
   root cannot be named.** The flush before `kCatalogInvalidate` walks the
   compile-time span (`FlushPages(kEveryCatalogPage)`,
   `src/server/expeditor.cpp:1028`), as does the peer's `EvictClean`
   (`src/server/core_runtime.cpp:884`). A run-time root would need a
   durable registry of which general-supply ids are catalog pages — a
   second free-map-shaped structure invented to avoid one version bump.
3. **Existence becomes conditional on every resolution path.** A
   bootstrap root is findable with zero catalog reads; an on-demand
   relation is found by name through `sys.objects` → `sys.tables`, and
   its *absence* is a per-file fact — plan-time resolution carries an
   "is there a directory yet" branch forever, against RD3's zero-cost
   invariant on the unsplit path.

**(b) Range rows folded into an existing structure.** Into `sys.tables`:
`SysTableRow` is a fixed-offset row keyed one-per-relation, and ranges are
variable-count per relation — either the row grows a variable-length list
(refused by the codec's shape, `rows.hpp`) or `oid` stops keying one row
and every `sys.tables` reader is rewritten. Into the **anchor page**: as
the *whole* directory it fails on shape alone — `{u64 key, u32 root}`
cells (`anchor_page.hpp:42`) have no `owner_core` field — before D2's
`index_oid == 0` / `lo = 0` collision (`catalog.cpp:1106-1111`) is even
reached; the collision's pricing as an *entry-page* store stays C4's
(RA5), for D2.

**Conclusion: the epoch is necessary; RA2 proceeds as ordered.** (a)1 and
(a)2 are structural — the fixed low root is what the peer-fault rule and
the bounded flush span are built on — and (b) fails on shape. On the other
side of the scale: every pre-16 file stops mounting (P0 policy, and the
order's §0 argues the cost is lowest *now*, before R6's recovery
fixtures exist); one overflow page off the catalog ceiling (H4 — the
range 15..127 = 113 pages becomes 16..127 = 112, ~68 `sys.columns` rows,
M2's number); one more bootstrap `CreateAt` (M1's number).

**H1 and H4, stated before the first edit** (falsifiers in the order §4).
**H1** (RD1 is inert): checkable now by source-read — no path reads page
15 today, and the one execution RA2 adds to an existing path is
`DropTable`'s sweep visiting an empty chain, answer-identical by
construction — plus the suite half of the falsifier (any delta beyond the
version pins RA2 edits); the mount-cost half is **M1's**, not this
task's. **H4**: the arithmetic above is computable now from
`well_known.hpp:313-325`; reading it against the widest scenario-bench
schema is **M2's**. Verdicts land in this section when the suite and the
cells report.

**Verdict, same day (worktree `v2.4.0-range-foundation-1`): H1's suite
half held.** In one sitting, the full suite ran **2774/2774** and
`scripts/sim.sh` **171/171** at the pre-RA2 tree (code-identical to
`b0b6e8a`), then **2775/2775** (the +1 is RD1's own new test) and
**171/171** after the edit; the only pre-existing test the edit touched is
`assertion_catalog_test.cpp`'s exact overflow pin, fixed by shape.
End-to-end with the built binary: a v15 file is refused at the door —
`startup failed: superblock version 15 is not this build's (16)`, and
bootstrap declines to treat it as a fresh database — while a fresh v16
file creates at 16 pages, remounts, and answers `SHOW TABLES` with
`ranges` in the list and `DESCRIBE ranges` with `oid=133
root_page_id=15`. The review's runtime probe found `SELECT`/`INSERT`/
`DROP TABLE`/`CREATE TABLE` against `ranges` byte-identical to the same
statements against `cabins`/`fkeys`. **H4's arithmetic stands as
written** (112 overflow pages at start 16, ~7,600-column ceiling); the
read against the widest scenario schema stays M2's. Mount *cost* stays
M1's — not measured here, stated so it is not implied.

**Verdict, same day, M1 (worktree `v2.4.0-range-foundation-1`, measured
at `v2.2.1-68-g7318e7e` against `b0b6e8a`): H1's mount half held.**
Boot-to-listener, fresh file both arms, interleaved in one sitting, the
noise band fixed by an A/A pilot *before* the A/B was read
(`bench/v2.4.0/results-m1-mount-cost-v2.2.1-68-g7318e7e.md`): first
boot's median delta is +3.56 ms against the same-binary control's
+2.18 ms — v16 the *faster* arm once slot position is netted — and
remount's paired median 1.75 ms against a 1.68 ms control, under the
driver's ~2 ms polling resolution. The falsifier fires on no subset,
including with the non-stationary first reps dropped. One mechanism
corrected at the review: the tenth relation does mount-time work only on
the *create* path — `Catalog::Bootstrap()` runs on a fresh file only
(`src/bootstrap/bootstrap.cpp:123`) and nothing between exec and the
listener enumerates catalog relations on a remount — so **first boot,
not remount, is the cell where a per-relation cost would have shown**,
and it showed none.

**Verdict, same day, M2 (worktree `v2.4.0-range-foundation-1`, source-read
at `v2.2.1-69-g3a60dc6`): H4 held; C1 does not re-open.** The overflow
range went 113 → 112 pages (`well_known.hpp:351,356`), and the ~68 held
*exactly* on re-derivation — 5-byte slot + 20-byte tuple header + 94-byte
`SysColumnRow` = 119 bytes into 8,140 usable, `floor = 68` — because a
catalog row carries **no Keystone word** (`catalog.cpp:368-374` says so in
words); with one, the answer would be 64, so the comment's number is right
for a reason and not by rounding. Ceiling 7,684 → 7,616 rows (range-only,
the comment's own framing — the root page, `kCatalogPageColumns = 5` at
`well_known.hpp:272`, is deliberately not added: first boot already puts
11 bootstrap column rows on it, `catalog.cpp:764-770`, `:865-871`). Read against
the four scenario benches (`bench/docs/README.md:71`; the fifth driver,
`scenario4_cabinopt_days.py`, is narrower at 20 columns total): widest
single relation is scenario1's `daily_stats` at 12 columns → **~635×**
under the ceiling; the widest *whole scenario* (scenario2's 68 columns
across eight relations) → **112×**. Both clear the falsifier's 10× by an
order of magnitude or more; RA2's own cost is 68 rows ≈ 0.89% of the
ceiling. Full derivation with every site:
`bench/v2.4.0/results-m2-catalog-ceiling-v2.2.1-69-g3a60dc6.md`.

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
| ~~**RD0(b)(c)**~~ | ~~**The doc half, still owed.** (b) The §1 gate reading and (c) §0's three reversed sentences, amended in place in `crosscore.md` §6a/§8 and the blueprint's R3 row. Carried as **RA1** of `instructions/v2.4.0/range-foundation.md` §5.~~ **Closed 2026-08-27 in worktree `v2.4.0-range-foundation-1` (RA1).** The three sentences are amended in place — `crosscore.md` §6a's refusal sentence, §8 test 13, the blueprint's R3 row — and §0's `[OPEN]` closure landed with it (§0 carries the full site list, the two extra indexes included). (b)'s §1 gate reading was already written into §1 at scoping and needed no further landing | none |
| ~~**RD1**~~ | ~~**`sys.ranges` exists and is empty.** Oid **133**, fixed root page **15**, `kCatalogOverflowFirst` → 16, `kSuperBlockVersion` → **16** with a ledger entry quoting the 12 → 13 precedent. Joins all five exhaustive lists and the `DropTable` sweep chain.~~ **Built 2026-08-27 in worktree `v2.4.0-range-foundation-1` (RA2), gated by §3a's C1.** Oid 133, root 15, both bumps landed with the verbatim 12 → 13 quote in the ledger. Every list joined: `kAllWellKnownOids`, `kAllCatalogPages`, `Bootstrap()`'s array (widened 9 → 10), and the `DropTable` sweep — first in the chain, held by a function-local `RangeRowNotYetDefined` whose `Decode` refuses any tuple as Corruption until RD2's codec replaces it (the row format stays RD2's, gated on D2; nothing here decided it). The review's structural finding landed with it: the hand-edited overflow `static_assert` became `CatalogRootsAreDistinctAndBelowOverflow()` over `kAllCatalogPages`, so a future root joins one list and the compiler checks distinctness and the bound — the five exhaustive lists are now four. `assertion_catalog_test.cpp:109`'s exact pin was fixed by shape as its own `:102-106` argued, then subsumed by that `static_assert`. A v15 file refuses to mount naming both versions (`superblock.cpp:60`); the empty relation answers every SQL route byte-identically to `cabins`/`fkeys` (review probe), and `SHOW TABLES` gains `ranges`, which is the relation existing, not a behavior change | none |
| **RD2** `[D2]` | **The directory row.** `SysRangeRow{rel_oid, lo, owner_core, entry_page}` per CC9. What this row adds beyond CC9's cell: the `lo = 0` and derived-`hi` rules are **enforced at the catalog door rather than assumed**, D2 is taken, and the anchor's `index_oid == 0` collision is recorded as its reason | RD1 |
| **RD3** | **Resolution and publication.** `ResolveRanges(rel_oid, predicate) -> {owner_core, entry_page}[]`, plan-time, from the session core's cache (§2a of the spec). Publication decided per §2b — and **the choice is forced by where RD5 allocates**, not preferred. §2c's plan-time-only rule enforced by shape. **The zero-cost invariant binds hardest here** (*"a one-range relation on its owner core must add zero instructions over today"*): the unsplit path gains no scan, no lookup, no allocation, and RD9 measures it rather than an inspection asserting it | RD2 |
| ~~**RD4**~~ | ~~**The gates, declined by name (§6a).** `RangeEligible(access)` over four fields already on `TableAccess` — `SchemaCanSpill(schema)` (`src/catalog/schema.cpp:29`), `indexes.empty()` (`schema.hpp:371`), the **live-id** `cabin_ids` test `CheckWriteAffinity` uses at `command_dispatcher.cpp:3631-3633` (**not** `cabin_mask != 0`, **not** emptiness — both are the wrong test, stated there), `fkeys_out.empty() && fkeys_in.empty()` (`schema.hpp:311-312`) — plus D1's btree decline. Per §0 a decline is a logged engine decision naming the gate, with no token and no byte. Built and tested **before** anything can allocate a second range.~~ **Built 2026-08-27 in worktree `v2.4.0-range-foundation-1` (RA3), gated by §9's C2 — with a fifth gate C2 found**: `RangeEligible(access, enforcer)` in `include/kds/exec/range_eligible.hpp` / `src/exec/range_eligible.cpp`, the four fields plus D1's btree decline plus the **assertion gate** (§9a row 7; the signature takes the `AssertionEnforcer` because the fact deliberately does not ride `TableAccess`). One caller: `tests/range_eligible_test.cpp` (H2 held, §9d). Gated on the order's RA2+C2 rather than this row's RD2, and rightly: the function reads `TableAccess` fields and the registry, never a `sys.ranges` row, so RD2's codec is nothing it needs — the row's RD2 gate was scoping conservatism, noted for the operator in cws issue `rd4-gate-rd2-mismatch` | ~~RD2~~ §9's C2 |
| **RD5** | **Allocation — the system's half.** A second range opens where the row-id allocator already carves a disjoint block, so the boundary is that block's `first_id` and CC10's page-boundary rule is satisfied *vacuously* — §6b's own words, *"the new range starts as its own empty sub-structure (CC8) and no existing page straddles it"*. Rides `AllocateRowIdRange`/`RowIdLeaseTable` unchanged; the row and the entry page are what is new. **The size is one named constant reached through one function and swept by config** (§2a, D6), starting at `kRowIdLeasePerGrant`. `RangeEligible` asked first, always — **on the owner core** (the assertion gate's registry is core-local, §9c) — **and re-checked where the durable row lands**: §9b's two admission windows (an index build in flight, an assertion between core 0's half and the owner's adoption) are races `RangeEligible` cannot see, serialized through core 0's catalog stream (CC10 step 3). **The converse gates land with this row too** (§9b): CREATE INDEX / CREATE CABIN (auto included) / CREATE ASSERTION / an FK naming a split relation each decline until `crosscore.md` §9's placement decisions are taken. **This row's shape is what answers §2b** — whether it runs on the drain tick (`core_runtime.cpp:1006-1016`, outside a borrow) or at the point of demand (`row_id_lease.hpp:88-95`, inside a running INSERT) | RD3, RD4 |
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

At `acb2540`, **nothing is built** — with four corrections made
2026-08-27: **RD0(a) is closed**, and was closed before this plan was
picked up, by `7148343` on `main` (§6). **RD0(b)(c) landed 2026-08-27** as
RA1 in worktree `v2.4.0-range-foundation-1` (§7's row carries what it
amended). **RD1 is built** — RA2 in the same worktree, gated by §3a's C1;
`sys.ranges` exists empty at superblock v16 and §7's row carries the
detail. **RD4 is built** — RA3 in the same worktree, gated by §9's C2,
which refuted H3: the gates are **five** plus D1's btree decline, the
fifth (assertions) found before anything could allocate; `RangeEligible`
has one caller, its test, and §9 carries the enumeration, the two
admission windows and the converse gates RD5 inherits. **What is next in
this order is RA4** (C3's observability decision — now over six gate
names) and RA5 (C4's two tables); the build resumes at RD2.
RD2 wants D2, RD7 wants D4, and **D1 removes the btree half entirely** —
it is `crosscore.md` §9's, not this plan's. D6 blocks nothing: RD5 is
built so choosing it is a config value, not a rewrite.

---

## 9. RD4 — the hypotheses, the C2 enumeration, and the fifth gate

Gate **C2** of `instructions/v2.4.0/range-foundation.md` §3, run 2026-08-27
in worktree `v2.4.0-range-foundation-1` at `87e68ce`, **before
`RangeEligible`'s first line was written**. The hypotheses are stated here
first, per the order's rule, so the surprise below is legible as one.

**H2 — RD4 costs zero because it has no caller.** `RangeEligible` is built
and tested with exactly one caller: its test. Nothing on a statement path
calls it until RD5. *Falsifier*: a caller appears in `CheckWriteAffinity`,
`InsertOneRow`, or any plan path — and if one turns out to be *needed*,
the order says stop and report, because that is RD5's shape leaking into
RD4. *Verdict*: recorded at the foot of this section, after the build.

**H3 — the four gates plus D1 are complete.** No structure the engine
builds per relation is both split-unsound and unrepresented in
`TableAccess`. *Falsifier*: the enumeration below finds one. The order
names this the hypothesis most likely to fail and its failing the useful
outcome. *Verdict, stated ahead of the table because the table is its
evidence*: **refuted — assertions are a fifth gate**, split-unsound and
deliberately absent from `TableAccess`. The detail is the enumeration's
row 7 and the two sections after it.

### 9a. The enumeration — every per-relation structure, one verdict each

Method: `TableAccess` (`include/kds/catalog/schema.hpp:175`) read field by
field — it is the per-relation catalog cache, so its fields are the
catalog's own inventory — then the engine swept for per-relation state
*outside* it, which is where a fifth gate would have to hide. Verdicts:
**gate** (a §6a decline covers it), **invariant** (split leaves it
correct, with the reason), or **fifth gate**.

| # | Structure | Where | Verdict |
|---|---|---|---|
| 1 | Clustered heap chain (`desc_page_id`, `heap_tail_hint`) | `schema.hpp:179,193` | **The unit itself**, not an auxiliary: per-range chains with per-range heads and hints are RD6's build, and §2/RD6 already carry the insert-head finding |
| 2 | Clustered btree | `schema.hpp:180` | **Gate — D1's decline.** The tree's top levels belong to whoever owns the root; the hop is the `[OPEN]` shared-structure mechanism (CC8, §1) |
| 3 | Var-heap chain (`varheap_page_id`) | `schema.hpp:199`; `SchemaCanSpill` at `src/catalog/schema.cpp:29` | **Gate.** One `kVarHeap` page may hold values referenced from both sides of a boundary; a core faults only pages it owns (§6a) |
| 4 | Secondary indexes (`indexes`, `index_mask`, anchor entries) | `schema.hpp:371,385` | **Gate.** Per-range vs global is `[OPEN]` (`index.md` §13) |
| 5 | Cabins (`cabin_mask`, `cabin_ids`, memory-resident entry sets) | `schema.hpp:269,282` | **Gate**, by the **live-id** test (`command_dispatcher.cpp:3644-3646`'s shape): `cabin_ids` is column-parallel with id 0 = none, so emptiness gates everything and `cabin_mask` misses a column past 64 — the test pins both wrong tests |
| 6 | Foreign keys (`fkeys_out`, `fkeys_in`) | `schema.hpp:311-312` | **Gate**, both directions: either end's check reads the other relation |
| 7 | **Assertions** — `LiveAssertion` + `BoundCabinChainWriter` + the registry (`by_oid_`, `unenforceable_`) | `include/kds/exec/assertion_check.hpp:80-141` | **FIFTH GATE — H3's falsifier fired.** Split-unsound three ways: **(i)** the Bound Cabin's entry pages are one core's own-stamped chain, appended by *every* write (`ReserveInsert`/`AdmitAndReserveUpdate`/`ReserveDelete`; PW1c-6c), so a second owner core's appends die on `MayWrite`; **(ii)** the live directory is memory-resident on the one owner core, and a core whose registry never heard of the assertion admits writes **unchecked** — `AnyOn` false, `CannotEnforce` false, the exact Finding 2 failure (`bench/v2.2.0/results-shipping-part-a-v2.2.0-11-g925f483.md`); **(iii)** the aggregate is keyed on group columns that **need not include the pk** — `ResolveAssertionColumns` (`src/exec/assertion_catalog.cpp:406`) refuses no column, so `GROUP BY <pk>` is legal and degenerate, and every other grouping straddles any id boundary, which is why per-range cabins do not compose. Legs (i) and (ii) each carry the gate alone; (iii) is why no per-range rebuild rescues it. Unrepresented in `TableAccess` **deliberately** — `assertion_check.hpp`'s header: the plan cache must not depend on the assertion set, so CREATE/DROP ASSERTION need no plan invalidation. The cost of that design lands here: the gate takes the enforcer. Binds split **and** migration, Cabin's reason in stronger form |
| 8 | Anchor page | `include/kds/storage/anchor_page.hpp:19-24` | **Invariant** under the gates: it holds `clustered_root` + index entries only; unindexed leaves the table empty, and per-range heads ride the directory row (CC9), not the anchor — RD6 sources heads from the directory |
| 9 | Catalog rows (`sys.tables`/`sys.columns`/`sys.objects`), `next_id`, `key_order` | `rows.hpp` | **Invariant.** `next_id` is the lease substrate ranges align to (§6b); per-relation monotonicity becoming per-range is §6b's stated consequence (R4's loud doc), and range-order concatenation preserves global pk order because ranges partition the id space (RD7 §5) |
| 10 | Row-id lease blocks (`RowIdLeaseTable`) | `row_id_lease.hpp` | **Invariant by construction** — the substrate §6b aligns ranges to |
| 11 | Waystone trails, `sys.patterns` rows | invariants 8/9 | **No gate** (§6a): a block-aligned split moves no page (CC10's vacuous case), so an existing trail stays *correct*, not merely safe; migration's trail retirement is priced by CC10, not gated here |
| 12 | Access statistics | `SHOW ACCESS`; recorder core-0-only (`core_runtime.cpp:258` — the peer dispatcher's `/*access_statistics=*/false`; premise at `core_runtime.hpp:65-67`) | **No gate** (§6a): advisory; per-core statistics gate the *mover* (R1/R5), not the substrate |
| 13 | Cabin-optimizer managed state | `cabin_optimizer_exec.hpp` | **Invariant** — memory-resident observation; the moment it *enacts* a Cabin, row 5's gate holds. Its auto-CREATE on an already-split relation is the converse direction, §9b |
| 14 | Statement-local inner build (`exec::InnerBuild`) | `inner_build.hpp:86` | **Invariant** — statement-scoped, dies with the statement, keys on rows read, not on placement |
| 15 | Undo chains, WAL streams, PL-C stamps | per core, keyed by target page id | **Invariant** — a block-aligned split moves no page across streams; PL's handoff is not invoked (CC10) |
| 16 | Buffer pool, eviction, free map, checkpoint | per page / per instance | **Not per-relation** — nothing to gate |

**The independent sweep.** A second pass ran over the whole tree —
`Catalog::CreateTable`'s registrations, `DropTable`'s teardown as the
inverse check, every `Oid`-keyed map in `server/`, `exec/`, `catalog/`,
`storage/`, `wal/`, `sched/`, `txn/` — and found **no sixth gate**. What
it added, each split-invariant or advisory: `CrossCoreWriteCounters`
(`core_affinity.hpp:80-118`, metrics keyed `(home, target, rel_oid)`,
observability not state); `RelationGrantDemand` (`core_affinity.hpp:182-193`)
and the relation-extent grant derivation (`RelationFaultExtentOf`,
`core_runtime.cpp:832`) — the write-rights *substrate*, which a second
range sidesteps entirely because its pages come from the new owner's own
lease, own-stamped, needing no grant; the durable `sys.access_stats`
rows (`catalog.cpp:2584-2630`, per `(kind, rel, column_mask)`, read-path
only, §6a's statistics verdict); the page header's `owner_oid`
(`page_header.hpp:45`, per-page attribution a block-aligned split never
moves); and `RowIdLease::denied` (`row_id_lease.hpp:66`, substrate).
Buffer pool, checkpointer, WAL, scheduler, transactions and shipping
confirmed keyed by page/txn/request, never by relation. One incidental
defect fell out of the inverse check and is recorded in
`docs/inflight/known-gaps.md` rather than here: **`DropTable` never
sweeps `sys.access_stats`**, so a dropped relation's shape rows consume
the capped shape budget forever.

### 9b. What the enumeration found beyond the fifth gate — for RD5, named now

Two **admission windows** invisible to any field read, and one **converse
direction**, all landing with RD5 rather than here:

- **The index-build window.** During an owner-side build
  (`PendingIndexBuilds::Covers`, `index_build_service.hpp`), `sys.indexes`
  has no row yet — core 0 commits the catalog half at `done` — so
  `access.indexes` is empty and `RangeEligible` answers true-at-read,
  stale-at-landing. **The assertion-build interval** between core 0's
  catalog half and the owner's adoption
  (`assertion_build_service.hpp:50-66` — deliberately no refusal window
  for writes) is the same race one level up. Both are races against a
  core-0 catalog write, and CC10 step 3 makes the range row itself a
  durable core-0 catalog write — so core 0's single stream is the
  serialization point, and the loser re-checks. RD5's obligation; a
  field here could not close a race.
- **The converse direction.** §6a gates what may *split*; nothing yet
  gates what may be *created on* a relation already split — `CREATE
  INDEX`, `CREATE CABIN` (the optimizer's auto path included), `CREATE
  ASSERTION`, an FK-declaring `CREATE TABLE` naming a split parent.
  `crosscore.md` §9's "auxiliary placement under a split relation" owns
  the eventual decisions; until each is taken, the conservative converse
  (the DDL declines on a split relation) must land **with RD5, not
  after it** — a split relation that then takes an index is the same
  unsound state RD4 exists to prevent, reached through the other door.
  RD5's row in §7 now names both obligations.

### 9c. The shape built, and C3 left where it belongs

`RangeEligible(access, enforcer)` — the signature is the finding made
code: the fifth gate cannot ride `TableAccess`, so the function takes the
`AssertionEnforcer` and asks `AnyOn || CannotEnforce`. Authoritative on
the relation's **owner core only** (the registry is core-local and the
owner holds the live directory, PW1c-6c), which is where RD5 allocates
(§6b) — authority and caller coincide by construction, and the header
says so. Fixed check order, documented in the header: btree (D1), then
§6a's own listing order — index, cabin, spill, FK — then assertion,
C2's addition, last. First tripped gate named; `RangeGateName()` gives
the decline's log token.

**C3 is untouched**: no counter, no `SHOW` field, no log line lands at
RD4 — RA4 decides, and a surface added now would read structurally 0
until RD5 exists (the shipping block's absent-rather-than-zeroed rule).
`log.hpp`'s own doctrine agrees: the decline is a decision whose caller
(RD5, on the drain tick or at demand) is the thing with no one to report
to — the *caller* logs; the function names.

### 9d. Verdicts

- **H3 refuted, and the refutation is the product**: the fifth gate is
  built into `RangeEligible` (row 7), the spec bullet is added
  (`crosscore.md` §6a, Assertions), and §6a's closing sentence now reads
  **un-asserted**. Found at RD4 with zero ranges allocated — not at RD5
  with a relation already split, which is what the order said failing
  early buys.
- **H2 held**: `grep -rn "RangeEligible\|RangeGate"` over `src/`,
  `include/`, `tests/`, `tools/` finds the symbol only in
  `range_eligible.{hpp,cpp}` and `range_eligible_test.cpp`. No caller in
  `CheckWriteAffinity`, `InsertOneRow`, or any plan path; none was
  needed, so nothing leaked from RD5's shape and §2b stays undecided.
