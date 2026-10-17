# Work order BZ — the advisory optimizer, in the background

Written 2026-10-17 on `worktree-bz-background-advisory-optimizer` from
`e1333a77` (`v3.2.0-150-ge1333a77`), on the operator's words, recorded in
English (`raft-marks-2026-10-17.md` §1):

- **W0**: "Things are now reasonably in order; this is the stage of
  optimizing performance from statistics. First, for frequently used query
  patterns, create index pages bound to the values queried most often; and
  where a query reads several tables, as a join does, add a concept that
  stores every lookup key ref."
- **W1**: "The aux structures the optimizer manages must be advisory, so
  their role is to help from the background while putting as little strain
  on transactions as possible. OLTP performance comes first; optimizing
  comes after it. So choose the structures and the design that help OLTP
  processing from the background."
- **W2**: "And now we open a real milestone, not a CN."

**Opened 2026-10-17 on W2.** BZ-Q1..Q6 (§4) are open; BZ-S0 (§5) depends
on none of them and is startable.

**Claim tags**: `[operator]` for W0-W2; `[source-read]` with `path:line` at
`e1333a77`; `[measured]` with the version it was measured at; `[design]` for
reasoning about code not yet written; `[quiet-wrong]` where a wrong choice
turns a correct answer into a wrong one with no error.

**The letter.** `[source-read]` BY (`workorder-by-reindex-drop-index-reclaim.md`)
is the last work order under `instructions/v3.2.0/`; BZ is unused. CLA's
choice.

**BZ against other orders.** BZ re-scopes Waystone (`docs/spec/waystone-concpets.md`)
and the cabin optimizer (`docs/spec/physical-optimizer.md` Part II). It
leaves the Cabin itself (`docs/spec/cabin.md`), the secondary index, the
relayout planner (Part I) and every authoritative structure as they are.
Not part of AR0 §8's chain.

---

## 0. What BZ is

W1 sets three rules, and BZ applies them to everything the optimizer manages:

1. **Advisory only** `[operator]`. A structure BZ manages falls under
   invariants 8 and 9. Deleting it never changes a result. It chooses where
   to look, never what is visible. It **may replace a lookup and may never
   replace a search** (`waystone-concpets.md` §2).
2. **As little on the transaction's path as possible** `[operator]`. BZ
   reads it as `[design]`: the statement does no work for the optimizer
   beyond one bounded, latch-free note and one latch-free probe (BZ-R2).
3. **Built in the background** `[operator]`. Building, writing, deciding and
   discarding are tasks that yield to OLTP.

Under those rules, W0's two structures become:

- **The value-bound index page** (W0's first half) becomes a **base-fetch
  hint**. For a hot pattern instance whose step searches through a
  secondary index, BZ records where each row the search returned
  physically lives. The search still runs and still decides the set. What
  the hint replaces is the per-row descent from the entry's tuple key to the
  base row. That descent is a tuple-key lookup, so the hint is within
  invariant 9.
- **The join's lookup-key refs** (W0's second half) become the **route**:
  for one pattern instance, every lookup step's `(step_id, rel_oid, tuple
  key, location)` across every relation the chain touched, plus the
  base-fetch hints of its search steps. This is what a Waystone trail already
  is (§1.1), moved off the statement and widened to search steps' fetches.

Both are Waystone. BZ adds no subsystem and no second name. The trail
becomes the route, and the base-fetch hint is a new entry kind of it.

**What W1 rules out, stated so it is not re-proposed.**

- An **authoritative join ref set**, served instead of the search, needs one
  of two things. One is invariant 9 amended, together with a per-relation
  stamp bumped at commit. The other is a write hook on every relation of the
  chain. The first puts a commit-time write on every writer; the second
  puts a probe on every writer.
- An **optimizer-created Cabin** is authoritative for its observed values
  and charges every write to its relation (§1.3). It is BZ-Q1.

---

## 1. The tree as read `[source-read]` at `e1333a77`

### 1.1 Everything advisory runs on the statement today

- **Recording is the statement's own work, synchronous, on its reactor.**
  - `SelectRun::Finish` calls `RecordExecution` after the walk
    (`src/server/command_dispatcher.cpp:5082-5112`); `RunAggregated` and
    `RunAnalyze` call it too (`:7543`, `:7641`).
  - `RecordExecution` calls `RecordTrail`, then `RecordAccessShapes`, then
    `RecordOptimizerSignals` (`:8033-8040`).
  - `TrailRecorder::OnPatternResult` has no `co_await`
    (`src/stats/trail_recorder.cpp:92-148`). In order, it counts the
    sighting and, from the second sighting (`WouldRecord`), registers the
    pattern, creates the directory and writes the trail.
- **A first recording writes the WAL from the statement.**
  - `RegisterPattern` holds `kCatalogPagePatterns` exclusive and inserts
    through the WAL (`src/catalog/catalog.cpp:3095-3125`).
  - `ClaimPatternWaystoneRoot` is a logged overwrite (`:3156`, `:3166-3203`).
- **A hot instance takes and dirties pages exclusive on every execution,
  even when its trail has not changed.** `LookupOrCreateWaystonePage` calls
  the exclusive, dirty-marking `Get` at every directory level
  (`src/stats/waystone_dir.cpp:82-124`; `src/storage/device_page_store.cpp:1578-1580`).
  The unchanged-trail comparison comes after that (`src/stats/trail_store.cpp:87-108`).
- **Replay is paid at the head of every trailable statement.**
  - `FindPattern` runs first, a catalog-cache hash lookup; a pattern with
    no directory stops there. For one with a directory, `depth` shared
    page reads of the directory, one read of the trail page, and its
    decode into a fresh vector (`command_dispatcher.cpp:7902-7921`; `waystone_dir.cpp:55-76`;
    `trail_store.cpp:147-189`).
  - Then `TrailReplay::Build` allocates a step map besides its index
    (`src/exec/trail_replay.cpp:17-21`). That contradicts the comment at
    `command_dispatcher.cpp:7898-7901`, which calls the index "the only
    allocation".
- **A point `SELECT` by tuple key on a btree records and replays.**
  - The step is `kLookup`. The memo is checked first, then `TryReplay`,
    then `BtreeLookup` (`src/exec/step_vm.cpp:691-718`).
  - A replay hit re-collects through `AcceptTupleAt` and records again
    (`:2694-2707`).
- **`sys.access_stats` takes a catalog page exclusive per step of every
  successful `SELECT`.**
  - `RecordChainAccess` makes one `RecordAccess` per step
    (`src/stats/access_stats.cpp:17-47`).
  - Each call holds `kCatalogPageAccessStats` exclusive across a chain walk
    and an overwrite or insert (`catalog.cpp:3206-3303`). The writes are
    unlogged.
  - The page is the instance's, so above one core every `SELECT` on every
    core serializes on it.
  - **Writes take it too.** A foreign-key check records its parent and
    child accesses through the same `RecordAccess` (`RecordFkAccess`,
    `command_dispatcher.cpp:3300-3313`, called at `:3679`, `:3753`,
    `:3878`), so an `INSERT`/`UPDATE`/`DELETE` on a relation with a
    foreign key takes the page exclusive as well.
  - Its readers are `SHOW ACCESS` and the `sys.access_stats` view
    (`src/server/sys_views.cpp:588`), the relayout planner
    (`src/stats/relayout_planner.cpp:189`, `:228`), and `CREATE CABIN`'s
    "nothing filters this column" warning (`src/exec/cabin_ddl.cpp:65-79`).
- **The switches are boot-only.** `waystone_recording`, `waystone_replay`
  and `access_statistics` are config keys (`src/server/expeditor.cpp:354-388`).
  `SET` admits only ISOLATION, DURABILITY and CABIN_OPTIMIZER
  (`command_dispatcher.cpp:1301-1308`).

### 1.2 What it costs `[measured]`

- **Point `SELECT` by tuple key.**
  - Waystone and access statistics take **1.6-1.7 us of 25.7-26.9 us of
    server CPU (6-7 %)**.
  - The B+ tree descent they shadow takes **0.3-0.4 us**.
  - At BN's close, `v2.7.0-787-g8679367e`
    (`bench/v3.0.0/archive/bn-close-v2.7.0-787-g8679367e/partsB.md`,
    `select-pk` rows).
- **Replay against a descent.** At `1769487`, before v3, a replay was 3-7 %
  slower than a descent on a B+ tree relation (`waystone-concpets.md` §7).
- **Neither number has been re-measured on v3.2.0.** BZ-S0 re-measures both.

### 1.3 The cabin optimizer creates authoritative structures, and builds them on a timer

- **An optimizer-created Cabin costs every write what a declared one does.**
  - `ApplyCreate` creates `kCabinOriginAuto` Cabins
    (`src/exec/cabin_optimizer_exec.cpp:263-265`).
  - The write hook's gate counts every serving Cabin, whatever its origin
    (`catalog.cpp:2645-2660`; `include/kds/server/command_dispatcher.hpp:2051-2053`).
  - Per written row and cabined column, the hook coerces the value, builds
    a key and takes a partition latch (a `std::mutex`) above one core to
    ask whether the value is observed (`command_dispatcher.cpp:8055-8206`;
    `src/stats/cabin_store.cpp:522-566`).
  - Once per written row with a keyable cabined value, it reads the written
    page's epoch through a page fetch, before that question is asked
    (`command_dispatcher.cpp:8074-8076`).
- **The tick runs inside a timer callback, not as a task.**
  - `Tick` runs in core 0's `SubmitEvery` callback under the view latch
    (`src/server/expeditor.cpp:2037-2056`).
  - `Tick` is synchronous: snapshot, decide, apply. Its build has no
    `co_await` (`cabin_optimizer_exec.cpp:40-47`, `:136`).
  - So a build's scan holds core 0's reactor for its length.
  - The scheduler asks a timer callback only to `Submit()`
    (`include/kds/sched/scheduler.hpp:150-153`). Time outside a task poll is
    charged to no group (`docs/spec/sched.md:57`).
- **It is off by default** (`include/kds/server/expeditor.hpp:314`).
- **Stale comments.** `include/kds/catalog/rows.hpp:346-358` and
  `src/server/command_dispatcher.cpp:4361-4365` say no code creates a Cabin
  on `CABIN AUTO`, and `rows.hpp:374-378` speaks of "the day auto-creation
  exists". The optimizer's `ApplyCreate` does create auto-origin Cabins.
  Fixed in BZ-S4 whichever way BZ-Q1 goes.

### 1.4 What the background has to work with

- **Scheduling groups.** There are three: `kForeground`, `kMaintenance` and
  `kSystem`, with shares `{1000, 100, 50}`
  (`include/kds/sched/task.hpp:14-18`; `scheduler.hpp:60`).
  **`kMaintenance` is defined, and nothing submits to it.** It is BZ's group.
- **Yield and task wrapping.** A task yields by `co_await sched::Yield{}`
  (`include/kds/sched/coro.hpp:403-411`). A coroutine becomes a task through
  `MakeCoroTask(group, coro)` (`:395-399`).
- **The scan ring.** EV6 has 32 frames, inserts at usage 0 and never bumps
  (`include/kds/storage/page_store.hpp:93`, `:690`; `docs/spec/eviction.md:214-248`).
- **Prefetch.** No API exists; the eviction spec places it outside its own
  scope, in the scan/executor layer (`eviction.md:265-266`).
- **The search's base fetch is a descent per surviving tuple key.** The
  index step's phase 1 walks the entries, dedups by tuple key and applies
  the covered filter (AT-S22), then sorts the surviving tuple keys
  (`step_vm.cpp:1588`); phase 2 calls `BtreeLookup(tuple key)` once for
  each (`:1613`, in `RunIndexStep` from `:1426`). That is IX7: the base row
  is always fetched (`docs/spec/index.md` §7). The Cabin serve first tries
  each entry's own location hint and heals it on a miss (`ServeFromCabin`,
  `step_vm.cpp:1704-1871`).
- **The statement epoch.** Each core publishes the **schema version word**
  its running statements revalidated against (held across a `SELECT`'s
  yields), and the BF reclaim frees a drop's pages once every core's word
  has reached the drop's (`include/kds/server/statement_epoch.hpp`,
  `docs/spec/drop-table.md`). The word moves only on DDL. The Cabin
  optimizer's tick takes no slot, because it runs synchronously on core
  0's reactor (BF-Q16 (a), `statement_epoch.hpp:34-37`). BZ-R3 and BZ-R4
  say where it applies and where it does not.

---

## 2. Rulings proposed `[design]`

**BZ-R1 — the advisory class.** Every structure BZ manages has these
properties:

- It is unlogged and discarded at mount (`server::DiscardAdvisoryState`, as
  today).
- It is consulted at one of two places only: a `kLookup`/`kProbe` step, or
  phase 2 of an index step, the per-tuple-key base fetch, after phase 1 has
  decided the set.
- It is verified by Waystone §2's rules 0-4. That means the derived key
  equals the entry's tuple key, the relation and epoch match, and
  visibility and the residual are decided by the same `AcceptTupleAt` as on
  the authoritative path.
- A miss falls through for that row alone.

`IsTrailReplayable` stays as it is (`include/kds/exec/step_chain.hpp:172-174`).
The base-fetch hint is not a replay of the index step. It is a lookup the
index step makes after its search, and the comment at
`step_chain.hpp:176-181` gains that sentence.

**Why the hint cannot change a result.** Phase 1 decides the set. That
covers the dedup by tuple key, AT-S22's any-entry rule and the sort
(`step_vm.cpp:1588`). The hint then replaces only the step from a tuple key
to a location. `AcceptTupleAt` still applies MVCC and the residual, which
keeps the index's equalities (`:1428-1432`). So each of the following is
rejected exactly as it would be after a descent:

- a row whose indexed column moved since recording;
- a stale or duplicate entry;
- a slot reused after `PURGE` or a rollback. Such a slot holds either
  another key, which is a miss, or the very row a descent would find.

The epoch recorded with a hint is the base leaf's, stamped while the
fetch's descent holds that leaf (`CurrentRelayoutEpoch(PageView)`).

**BZ-R2 — the foreground's whole share.** A statement's work for the
optimizer is at most two things.

**(i) One note.** A record pushed into its own core's single-producer ring,
with no latch, no allocation and no page access.

- **What it carries:** `fetch_id`, `arg_hash`, the step shapes `sys.access_stats`
  folds, the counters `RecordOptimizerSignals` reads today, and the
  per-route hit/miss counts of this execution.
- **What it copies:** only the entries actually collected. The lookup
  entries are capped at `kMaxTrailEntries` (253) as today. The base-fetch
  hints have their own cap, so an index step returning many rows never
  costs a join its lookup route. A full collector records no trail at all
  (`trail_recorder.cpp:105-109`), and that rule stays per kind.
- **Size:** the ring's size is fixed in BZ-S1, as a byte bound per core.
- **Overflow:** a full ring drops the note and counts the drop.

**(ii) One probe.** A hash probe into the published route table, reached
through one atomic pointer read with no latch.

- A route found yields per-step locations, verified as in BZ-R1.
- No catalog lookup, no directory walk and no page read happen before the
  first verified location.

Nothing else is permitted on the statement:

- no page write;
- no WAL record;
- no exclusive latch, `RecordFkAccess` included (§1.1);
- no lock-family unit;
- no wait on the background.

**BZ-R3 — the background's discipline.** All of BZ's background work runs
as `kMaintenance` tasks:

- **Drain.** One drain task per core folds its ring into the steward's
  input.
- **Steward.** One steward task, on core 0's cadence, owns four jobs: heat,
  budget, publication and withdrawal.
- **Yielding.** Each task yields at a bounded count of notes.
- **Under BZ-Q1 (a) and BZ-Q2 (a), the background reads no data page.** It
  folds notes, takes the `sys.access_stats` catalog page under BZ-Q6 (a),
  and builds memory. Nothing it holds names a page across a yield, so it
  needs no statement-epoch slot. BF-Q16 (a)'s reason for the tick's
  exemption (`statement_epoch.hpp:34-37`) is then moot rather than broken.
- **A background task that ever carries a page id across a yield** — under
  BZ-Q1 (b), a build's walk — holds its core's statement-epoch word for
  that slice (`StatementEpochs::Hold`, as `SelectRun` does). This delays a
  drop's reclaim and never makes a DDL wait. Without it, a walk can land on
  a page the reclaim freed and another relation reused, and the owner check
  in `tuple_verify.hpp:57-62` is a backstop, not the authority.

**BZ-R4 — publication.** The route table is immutable once published:

- The steward builds the next table and swaps the one atomic pointer to it.
- **Freeing.** The table it replaced is freed once every core has finished
  one reactor iteration begun after the swap.
  - A reader takes the pointer and copies the locations it needs inside
    the statement head (`DispatchAndStage`), which does not yield. So no
    reader holds the table across an iteration boundary.
  - The statement epoch is the wrong clock here, because it moves only on
    DDL (`statement_epoch.hpp:6-13`).
  - A per-core iteration count does exist (`Scheduler::iterations()`), but
    it is a plain field its own reactor writes (`scheduler.hpp:36-40`).
    BZ-S1 publishes it as an atomic.
- A reader never sees a table being written.

**BZ-R5 — consulted only where it pays.** Two gates decide what is
published, one fixed and one live:

- **The per-kind gate is a constant, fixed in code at BZ-S2.** BZ-S0 decides
  which step kinds a verified hint beats a descent on.
- **The live gate is per route.** Counters travel in the note (BZ-R2), never
  as writes into the published table. A route whose verified-hit rate falls
  below a threshold is withdrawn by the steward.

A withdrawn route costs exactly the absence of the hint (invariant 8).

**BZ-R6 — the heat.** A pattern instance's heat is the R1 decay score
(`include/kds/stats/decay.hpp`), held per instance in the steward's memory.
The budget (BZ-Q4) admits the hottest instances. An instance that falls out
costs its hints and nothing else.

---

## 3. What does not move

- The trust model. BZ moves where the work runs, not what it may claim.
- The Cabin, its own location hint and heal (`cabin.md` C6), the secondary
  index and the clustered tree. No authoritative path changes.
- The waystone contract suite's five configurations. They become BZ's
  witness, extended as §5 lists.
- `kFingerprintVersion`. No hash moves.

---

## 4. Questions — open

**BZ-Q1 — the cabin optimizer** `[quiet-wrong]`. An auto-created Cabin is
authoritative and charges every write (§1.3). W1 says the optimizer's
structures are advisory.

- **(a) Delete the controller.** This covers:
  - Part II's controller and its decision log;
  - `SHOW` and `SET CABIN_OPTIMIZER`, each refused by name;
  - the `cabin_optimizer*` config keys other than the budget BZ-Q4
    re-scopes, each refused by name (`include/kds/server/expeditor.hpp:314-334`);
  - `OptimizerSurface`.

  An auto-origin Cabin left on a volume is dropped once, at mount. The
  declared `CABIN` policy stays the operator's. `CABIN AUTO` keeps parsing
  and is read as `unset`. `HEAL` is not kept: declared Cabins are outside
  the optimizer's jurisdiction (`physical-optimizer.md:487-489`), and
  `ServeFromCabin` already heals inline (`step_vm.cpp:1786-1800`). The
  decay score and the fixed-point discipline (PO3, PO10) carry over to the
  steward as rules; no controller code does.
- (b) Keep auto-created Cabins, with their tick and build moved into a
  `kMaintenance` task under BZ-R3's epoch hold.
- (c) Leave Part II as it is.

The `[quiet-wrong]` mark is on (b) and (c): an auto-created Cabin with a
witness bug gives a wrong answer, where a hint with the same bug gives a
miss. **CLA proposes (a)**. W1 decides it, and the operator has twice
preferred deleting a source to flagging it.

**BZ-Q2 — where the route lives.** Decided before BZ-S1, because S1's drain
writes into whichever it is.

- **(a) Memory, published per BZ-R4.**
  - Delete the directory and the trail pages, with their creator
    `CreateNewHeaderless`.
  - **Keep the headerless map and its reader.** A volume written before BZ
    keeps its old directory pages allocated (`docs/inflight/known-gaps.md`,
    "Every restart leaves the previous run's Waystone pages allocated").
    The data-file check behind backup and restore skips them by that map
    (`src/server/restore.cpp:169-187`). Without the reader they fail their
    checksum, and a backup is reported damaged.
  - With the reader kept, nothing on disk changes meaning, and there is no
    superblock bump.
  - `sys.patterns.waystone_root` and `dir_depth` become reserved words,
    written invalid and never read. `PageType::kWaystone` stays reserved.
- (b) Keep waystone pages, written by the drain task.

Every mount already discards every trail (§1.1), so the pages buy no
durability. They cost the foreground `depth` + 1 page reads per statement
and the pool frames they occupy. **CLA proposes (a).**

**BZ-Q3 — `sys.patterns` registration.** Under BZ-Q2 (a), its remaining
readers are the `SHOW PATTERNS` view (`src/server/sys_views.cpp:456`) and
`DiscardAdvisoryState`'s root clearing. The second becomes dead code.

- (a) The drain registers a new shape: a logged write, off the statement
  but on the one log.
- (b) **Stop registering.** `SHOW PATTERNS` reads the steward's table, the
  hot instances with their heat. The relation and its rows stay on disk,
  and nothing writes them.

**CLA proposes (b)**. A logged write kept only for a view is the same WAL
coupling BL-S4 deleted `Catalog::TouchPattern` for (`waystone-concpets.md`
§4).

**BZ-Q4 — the budget's key.** The steward's memory is bounded, and PO6
prices it in 8 KiB page units as `cabin_optimizer_page_budget`.

- (a) Under BZ-Q1 (a), that key is the one budget left. Re-scope it to the
  steward, rename it `waystone_page_budget`, and refuse the old name by
  name.
- (b) Under BZ-Q1 (b) or (c), keep it for Cabins and add no second budget:
  the route shares it.

**CLA proposes (a)**, which follows BZ-Q1 (a).

**BZ-Q5 — the point `SELECT` by tuple key.**

- (a) **No route.** A chain that is one `kLookup` step on a btree relation
  takes no probe, and its note carries counters and step shapes only, with
  no entries. So `SHOW ACCESS` and the steward's heat still count it.
- (b) Routed when BZ-R5's per-kind gate says it pays.

**CLA proposes (a)**. A descent costs 0.3-0.4 us, a figure BZ-S0 re-prices
with replay off (§1.2). A probe plus a verified read has not been shown to
cost less.

**BZ-Q6 — `sys.access_stats`.**

- (a) Its fold moves to the drain: the note carries the step shapes, and
  `RecordFkAccess` becomes a note as well.
- (b) Delete it.

Its readers are `SHOW ACCESS`, its view, the relayout planner and
`CREATE CABIN`'s warning (§1.1). **CLA proposes (a)**: (b) reaches Part I
and the Cabin DDL, both outside W0's ask.

The two boot keys `waystone_recording` and `waystone_replay` stay the only
switches, re-scoped to the route. No measurement asks for a runtime one.

---

## 5. Stages

Each stage is reviewed by `critics-developer` and gated by the full suite.
**The overhead is measured once, at BZ-S5**, within the time budget of
2026-10-13: about one hour, building included. **BZ-S0 is a premise
measurement**, inside 20 minutes, and comes before any code.

**BZ-S0 — re-measure the premises** (`build-release`, interleaved A/B,
`git describe --tags` in the results file). There are four cells:

1. `select-pk` with recording and replay both on, against both off. The
   descent is priced in the off arm.
2. A three-relation tuple-key join chain, on against off.
3. A secondary-index probe returning 10 rows and one returning 100, with
   each base fetch's descent priced by the sampler (`tools/kds_sampler.c`).
4. `select-pk` at `cores=4` with `access_statistics` on, against off. This
   prices the catalog page's serialization in §1.1.

The results set BZ-R5's per-kind constant and test BZ-Q5's premise. A cell
cut for time is named in the results file. Startable now.

**BZ-S1 — the note, the drain and the table.** Gated on BZ-Q2, BZ-Q3 and
BZ-Q6.

- Add the per-core ring and the drain tasks.
- `RecordTrail`, `RecordAccessShapes`, `RecordOptimizerSignals` and
  `RecordFkAccess` become one note.
- Under BZ-Q2 (a), the steward builds the table and publishes it per BZ-R4,
  and replay probes it. The directory, the trail pages and their writer
  are deleted in the same stage, so no drain ever writes a page that the
  next stage deletes.
- Cells:
  - the contract suite's five configurations, the corrupted route against
    memory;
  - a full ring dropping notes and changing no result;
  - a swap under a running statement;
  - a table freed only after every core's iteration has passed the swap;
  - a `DROP TABLE` while its relation's routes are published (a miss, never
    a row).

**BZ-S2 — the gate.** Gated on BZ-S0 and BZ-Q5.

- The per-kind constant and the per-route withdrawal (BZ-R5).
- BZ-Q5 (a)'s exclusion.
- Cells:
  - a route withdrawn and re-admitted, with no result changed;
  - the exclusion's server CPU, priced against S0's cell 1.

**BZ-S3 — the base-fetch hint (W0's first half).** The index step only. A
Cabin step already carries its own hint and heal (`cabin.md` C6).

- The note carries an index step's surviving tuple keys with their base
  locations, under their own cap (BZ-R2).
- Phase 2's `BtreeLookup` (`step_vm.cpp:1613`) first asks the route,
  verifies per BZ-R1, and falls through on a miss.
- Cells:
  - a row inserted since recording is returned (by the search);
  - a row deleted since recording is not returned;
  - a row whose indexed column moved is not returned;
  - a row moved by a leaf split misses and is fetched by descent;
  - results byte-identical across the five configurations;
  - instrumentation showing phase 1 ran on every execution
    (`waystone-concpets.md` §11 test 4, extended).

**BZ-S4 — the cabin optimizer per BZ-Q1.**

- Under (a): the deletion and the mount-time drop of auto-origin Cabins.
- Under (b): the move and BZ-R3's epoch hold.
- Either way, §1.3's stale comments are corrected.

**BZ-S5 — the text and the close.**

- Docs:
  - `waystone-concpets.md` §1, §4-§6 and §9, restated for the route and
    the hint;
  - `physical-optimizer.md` Part II per BZ-Q1;
  - `sched.md`, naming `kMaintenance`'s first user;
  - the manual;
  - `CLAUDE.md`'s Waystone and Part II rows.
- The milestone measurement: `e1333a77` against the close, every S0 cell
  plus a mixed OLTP run at `cores=4`, inside about an hour.

---

## 6. Where to start

BZ-S0. It needs no mark, and its numbers are the premise of BZ-R5 and
BZ-Q5. BZ-S1 follows on BZ-Q2, Q3 and Q6.

---

## 7. Stage log

None yet.
