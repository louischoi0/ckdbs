# Workplan — catalog placement build-out (CB)

Work order: `instructions/v2.7.0/cb-catalog-placement-buildout.md`, governed
by the ratification `instructions/v2.7.0/r1-catalog-placement-ratification.md`
(CR1-CR4) and its addendum CR5-CR8. The rules the build lands under are
`docs/spec/crosscore.md` **CC12** (catalog page placement) and **CC13**
(DDL's route, and how a peer's statistics reach core 0).

Worked in worktree `cr-catalog-placement`.

> **CB0-CB3's subject was removed the day after they landed.** Work order
> `instructions/v2.7.0/pd-remove-declared-patterns.md` withdrew
> user-declared patterns on 2026-08-31, and `sys.pattern_defs` went with
> them. Read §1-§2 below as the record of a finding rather than as a
> description of live code: **the mechanism survives and its subject does
> not.** `exec/catalog_spills.hpp`, the merged walk and
> `CoreRuntime::Open`'s grant over `kVarHeapCatalogRelations` are all
> still there; the list now holds `sys.assertions` alone, which is the
> relation CB2's merge was extracted *from*. CB0's finding — that the
> read-side `MayFault` consult is Debug-only, so a missing grant is
> invisible in a release build — was never about this relation and is in
> `known-gaps.md`.

## Where to pick this up

| Row | Subject | State |
|---|---|---|
| CB0 | What a peer hits on a spilled `sys.pattern_defs` definition | **Done** — §1, and the finding is not the one the order expected. Subject removed 2026-08-31 (see the banner) |
| CB1 | Mechanism: page-at-a-time grant vs a reserved sub-range | **Done** — the grant, on the order's own three grounds, with the frequency premise checked (§1a) |
| CB2 | Build it for `sys.pattern_defs` | **Done** — and it removed two copies of one walk rather than adding a third (§2), which is why the removal of its subject cost nothing |
| CB3 | `well_known.hpp` says the placement is a rule | **Done** |
| CB4 | The peer DDL guard becomes a route | **Done** — §3 |
| CB5 | Autocommit only; a DDL in a transaction still refuses and poisons | **Done** — §3 |
| CB6 | `CREATE` has no oid to ship; the invalidation questions | **Done** — §3a, and it found a live defect the order predicted only as a question |
| CB7 | The peer-side statistics batch and its ring path | **Built** — §4; the constants sweep is owed |
| CB8 | `RecordAccess` unlogged under CR6 | **Built** — §5, and sub-question 1's answer changed what the discard is for |
| CB9 | The rule text CR6 requires, in `rules.md` §5 | **Done** — the rule, and RV3's own text amended to point at it |

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

## 4. CB7 — the batch, and what it did *not* get a knob for

**Peers recorded nothing at all, and by construction rather than by
failure.** `CoreRuntime::Open` passes `access_statistics = false` for every
core it opens (`src/server/core_runtime.cpp:280`), so a peer never even
attempted the write that CC11 would have refused. That is
`crosscore.md` §6a's *"a peer that records nothing cannot feed the mover"*
in the source, and it is the whole of what CB7 had to close.

Four decisions, each of which could have gone the other way:

- **The batch is a *sink*, not a second recorder.** `RecordChainAccess`
  computes the shape once and chooses where it lands, so a peer's fold and
  core 0's row can never come to disagree about what a shape is. The
  alternative - a peer-side recorder walking the chain itself - is how two
  definitions of "shape" get born.
- **`RecordAccess` took a `count`, rather than the batch path taking its own
  entry point.** One row, one saturation, both paths. A separate applier
  would have been a second authority over the same ranking.
- **The buffer is derived from the ring slot** (`kAccessBatchCapacity`), the
  rule `kShippedStatementTextMax` already follows, so a batch is always one
  message and a slot resize moves the buffer rather than silently truncating
  a flush.
- **No new interval knob.** The flush rides `wal_drain_interval_ns` with the
  other peer ticks, because the engine already names "how often a core does
  its cheap background work" and a second name for one quantity is what this
  project's own rule forbids. **What CB7's sweep sizes is the buffer**, and
  that is the only constant this row leaves open.

**CR8's drop is the engine's one exception to never-drop**
(`sched/send_retry.hpp`, M7's yield-and-retry), and it is stated at three
sites - the enum, the send, and the service header - because a reader who
finds a dropped message needs to learn it was *ruled* rather than
overlooked. The batch is cleared on a drop as well as on a send: holding it
would grow one shape's count without bound and then flush a number that
names no interval, which is worse than the gap.

`SHOW META` gains one block on both ends. A peer prints what it sent, core 0
what it applied, and **the difference between the two is exactly the
drops** - which is what makes a drop diagnosable from either side rather
than only where it happened. `access_shape_overflows` is the second,
different loss: a shape that arrived with no slot between two ticks, which
is the number that says the buffer is too small for this workload.

## 5. CB8 — unlogged, and the discard that makes it safe

The write loses its WAL record on both paths (`OverwriteLogged` and
`InsertRow` take `nullptr`), and the undo hook goes with it: a row nothing
redoes has nothing to compensate.

**Sub-question 1's answer is not the one the order's framing implied.**
*"A torn or stale page 11 after a crash: is it detected, and by what?"* -
`sys.access_stats` has exactly three call sites (`catalog.cpp`: bootstrap,
`RecordAccess`, `ListAccessStats`), and **no mount path reads it at all**.
So a torn page is detected by the ordinary page checksum on fault, and the
consequence today is a dropped statistic on every statement and a failing
`SHOW ACCESS` - never a refused mount. Under CR6 that page also loses redo,
so there is no repair path and the damage is **permanent**.

That turns CB8's proposed discard from a tidy-up into the thing that makes
CR6 safe, and it is what the code does: `Catalog::ResetAccessStatsIfDamaged`
runs at mount in the window `FinalizeDeleteMarksAtMount` and
`SweepUnownedSpills` already share - after recovery, before the listener
binds, core 0's alone.

Two properties worth stating:

- **The walk is the detector, and it is the ordinary one.** `ChainVisit`
  answering non-OK *is* the damage report: a torn page fails its checksum, a
  broken link fails the traversal. A hand-rolled walk here would be a second
  opinion about what a readable chain is.
- **The growth pages leak.** Nothing reclaims a page in this engine, so a
  discard drops the chain and leaves its pages allocated. Stated rather than
  discovered; it is the same class of leak `known-gaps.md` already owns.

Sub-question 3 - whether an unlogged page can sit in a heap chain alongside
logged ones - **does not arise as posed**: the relation's writes all go
through the two call sites above, so the whole relation is unlogged rather
than a mixture. What the question was really guarding against is a
half-applied grow, and that is what the discard answers.

## 6. CB9 — the rule, and RV3's own sentence

`docs/rules/rules.md` §5 carries the exception, in the form the order gave
it, and `ddl-transactional.md` §7 now says to read RV3's *"catalog writes
are WAL-logged"* as *"every catalog write except that one"*. One line was
added to the order's text and is worth flagging as CLA's: **the test is the
content's class, not its cost** - a relation a reader may act on is not a
statistic however cheap it is to rebuild - because "resembles this one" was
the failure mode the order named, and cheapness is the resemblance most
likely to be argued.
