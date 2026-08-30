# Workplan — catalog placement build-out (CB)

Work order: `instructions/v2.7.0/cb-catalog-placement-buildout.md`, governed
by the ratification `instructions/v2.7.0/r1-catalog-placement-ratification.md`
(CR1-CR4) and its addendum CR5-CR8. The rules the build lands under are
`docs/spec/crosscore.md` **CC12** (catalog page placement) and **CC13**
(DDL's route, and how a peer's statistics reach core 0).

Worked in worktree `cr-catalog-placement`.

## Where to pick this up

| Row | Subject | State |
|---|---|---|
| CB0 | What a peer hits on a spilled `sys.pattern_defs` definition | **Done** — §1, and the finding is not the one the order expected |
| CB1 | Mechanism: page-at-a-time grant vs a reserved sub-range | **Done** — the grant, on the order's own three grounds, with the frequency premise checked (§1a) |
| CB2 | Build it for `sys.pattern_defs` | **Done** — and it removed two copies of one walk rather than adding a third (§2) |
| CB3 | `well_known.hpp` says the placement is a rule | **Done** |
| CB4 | The peer DDL guard becomes a route | **Done** — §3 |
| CB5 | Autocommit only; a DDL in a transaction still refuses and poisons | **Done** — §3 |
| CB6 | `CREATE` has no oid to ship; the invalidation questions | **Done** — §3a, and it found a live defect the order predicted only as a question |
| CB7 | The peer-side statistics batch and its ring path | **Not started** |
| CB8 | `RecordAccess` unlogged under CR6 | **Not started** |
| CB9 | The rule text CR6 requires, in `rules.md` §5 | **Not started** |

## 1. CB0 — the failure is latent in a second sense than the order assumed

`ListPatternDefs` stages its rows and then calls `ResolveSpills`, which
faults the var-heap page (`src/stats/pattern_defs.cpp`). On a peer that page
is at or above `system_page_limit_`, so `TryClaimByStamp` runs first and
**declines** — it claims only a page carrying this core's own stream stamp,
and a catalog var-heap page carries core 0's (`device_page_store.cpp:694`).

What follows is the finding. **The `MayFault` refusal on the read path is
`#ifndef NDEBUG`** (`device_page_store.cpp:438`); only the write half is
enforced in every build, since PW1c-5. So:

- **Debug**: `InvalidArgument`, and the message is exactly
  `DevicePageStore: core 1 may not fault page <var-heap root>; it belongs to
  another core` — asserted verbatim in
  `PatternDefsPeerReadTest.WithoutAGrantTheRowsReadAndTheBodyDoesNot`, since
  the order asked for the message rather than a description of it.
- **Release**: no refusal at all. The peer faults core 0's page and answers
  correctly, and the shared-nothing violation is invisible in the build
  every measurement is taken in.

So `sys.pattern_defs` was latent twice over: no peer reads it today, **and**
the build that ships would not have reported it if one did. That is the
argument for building the grant rather than arguing about it, and it is
recorded in `known-gaps.md` as a property of the store rather than of this
relation.

### 1a. CB1's premise, checked rather than assumed

The order made the page-at-a-time grant conditional: *"if CB0 shows the read
is per-statement rather than per-DDL, stop and report."* Every reader of
`sys.pattern_defs` was enumerated from source — `exec/pattern_ddl.cpp`
(`CREATE PATTERN`'s duplicate-name probe and `DROP PATTERN`) and
`command_dispatcher.cpp`'s `SHOW PATTERNS`. Nothing reads it per statement,
so the condition does not fire and the grant stands. The format change — a
reserved low sub-range for catalog var-heap — stays available and CR1 stays
the rule that would justify it.

## 2. CB2 — one walk, where there were two, and now three consumers

The grant needs *"the ids the rows name, with no fetch of any of them"*.
Two implementations of that walk already existed:

- `exec::AssertionSpillPages` (`assertion_catalog.cpp`), decoding each row
  and reading its `PendingSpill` list;
- `ReferencedSpills` (`varheap_sweep.cpp`, file-private), decoding only the
  `varchar` cells directly.

A third copy for `sys.pattern_defs` is what the order's shape invited, and
what the review rule forbids. Both are replaced by
`include/kds/exec/catalog_spills.hpp` — `ReferencedSpills` and
`CatalogSpillPages(catalog, store, relations)` — and `AssertionSpillPages`
is deleted. `CoreRuntime::Open` now grants over `kVarHeapCatalogRelations`,
which is `sys.pattern_defs` **and** `sys.assertions`.

**One behaviour changed with the merge, stated because it is not
cosmetic.** The sweep's walker counts **delete-marked rows as live
references** and the assertion version skipped them. The sweep's semantics
are the ones kept, in both directions: for the sweep, collecting a value a
readable row points at would turn a leak into a wrong answer; for the grant,
a peer can still be asked to resolve a marked row's spill. The cost is at
most one more granted page.

The list carries both consumers' tests, because they are not the same test:
the **sweep** needs every entry's spills to be logged under `wal::kNoTxnId`,
the **grant** needs only that a peer reads the relation. Every entry
satisfies both today; one that satisfies one and not the other needs its own
list, which is what `catalog_spills.hpp` says where the list is defined.

## 3. CB4-CB6 — the route

`MayShip`'s four conditions turned out to be exactly this row's admission
test, so they are reused rather than restated: a ship client exists, this
path can park, the statement is **autocommit** (CB5), and the session has
not already shipped. A DDL inside an explicit transaction therefore falls
through to `PeerDdlRefused` and poisons, exactly as before — CB5's reason
stands as the order wrote it, and `known-gaps.md`'s list of relations that
are still non-transactional is what it rests on.

**CB6's oid question, verified before it was relied on.** The owner's dedup
record keys on `(requester, session_id, sequence)`
(`statement_ship_service.hpp:398`), and the SS3 review **retracted** the
cross-check of `target_oid` against the relation the text resolves to, on
the ground that the owner's own catalog is the only authoritative
resolution. `target_oid` is *"the relation the arrival core routed on, and
nothing more"*. So two `CREATE`s from one session are distinguished by their
sequence, not by an oid neither of them has, and `kSysTablesTable` is
carried for **all three verbs** rather than for `CREATE` alone — a DDL names
an object the arrival core may not be able to resolve, and the field is a
routing and logging aid on this path.

### 3a. The invalidation race the order asked about — and it is real

The order required two things be established rather than assumed. The first
is narrow: `catalog.cpp`'s *"this is `kCatalogInvalidate`, and it is not all
of CC10"* names two limits — that an insert **on a peer** invalidates
locally and broadcasts nothing, and that the bump fires before commit. The
first is closed by CR5's construction rather than by code (a peer never
writes the catalog at all, so the broadcasting core is always the writing
core); the second is untouched by CR5 and applies to a shipped DDL exactly
as to a typed one.

The second is not narrow. `core_runtime.cpp`'s argument — *"a statement
racing the invalidate answers retryably"* — was written when only core 0
issued DDL, so the racing statement always belonged to a **different**
session. Under CR5 it can be the DDL's **own** session, and two facts decide
whether that matters:

- `Expeditor::BroadcastCatalogInvalidation` flushes the catalog pages
  **inline**, so the device is current before core 0 answers;
- but it sends the invalidate as a **task**
  (`core0_scheduler.Submit(MakeSendRetryTask(...))`), and nothing orders
  that task against the reply to the ship. The ring is an N² SPSC matrix, so
  FIFO would have ordered them had both been sent inline. One is not.

**Measured, not argued**: with the mechanism reverted, the peer answers
`ERR no table with this name` to a `DESCRIBE` of the table it was told one
statement earlier had been created.

The fix is local and needs nothing from the broadcast: the shipping peer
drops **its own** catalog cache when a DDL ship returns success
(`PendingShippedStatement::ddl`, and a `SetCatalogInvalidate` seam wired to
the same `InvalidateCatalog()` the ring handler runs, so the two paths
cannot come to mean different things). Core 0's inline flush is what makes
that sufficient. The alternative — sending the invalidate inline — would put
a send-retry loop inside `BumpVersion`, which is a worse trade for a window
this closes exactly.

### 3b. The vacuity matrix

§6's requirement: revert each mechanism, count what catches it.

| reversion | what it undoes | caught by |
|---|---|---|
| **B1** | the route: back to `ErrorReply(PeerDdlRefused)` for every peer DDL | **1** — `APeersDdlRunsOnCoreZeroAndItsOwnNextStatementSeesIt` |
| **B2** | the cache drop on a successful DDL ship (CB6) | **1** — the same test's `DESCRIBE`, which answers `ERR no table with this name`. **Run, not reasoned** |
| **B3** | the grant list back to `sys.assertions` alone | **1** — `TheGrantTheMountTakesMakesTheBodyReadable`, Debug only, which is §1's finding restated as a test property |
| **B4** | the grant widened to the extent around the named pages | **1** — `TheGrantIsExactlyThePagesTheRowsNameAndNoExtent`, plus PW1c-7's `APeersOwnPagesSurviveARestartByTheirStamp` |

**B3's Debug-only catch is the honest reading**, and it is why §1's finding
matters more than the relation it was found on: a release build cannot
distinguish a granted read from an ungranted one, so no test in that
configuration can catch B3. The suite runs in both.
