# Work order AM — AR0 M1: the shared buffer pool, page latches, scalar page LSN

Written 2026-09-03 on `worktree-v3.0.0-arch-revision` at `f6ed10c`
(`v2.7.0-157-gf6ed10c`), while M0's baseline (AL-S8) was still measuring.
The survey below is a source read at that commit.

**Status: AM-S0 (a) and (b) and AM-S1 are done; AM-S2..S6 have not
started.** AM-1's "must not start before AL-S8's numbers are read" was
satisfied by AL-S8's three files and lifted for AM-S1 by the operator's
word in plan mode on 2026-09-03 (the AM-6 row). M1 removes the structural argument `eviction.md` §1 is built on,
and the only honest way to remove it is to know what the per-core pool was
worth. AM-1 says what would change that.

---

## AM-1 — The direction, and what it is not

AR0 §8 step 4: *"M1: shared buffer pool, page latches, scalar page LSN."*
AR0-4 retires **write-serialization authority**; §3's revised G1 admits
latch primitives, and G2 keeps `cores = 1` at zero overhead.

**The title overstates the work in one place and understates it in
another, and the survey is how CLA found both.**

- **"Scalar page LSN" is already done.** `page_lsn` at offset 8 of the
  common header has always been a scalar `uint64_t`, and G3 — "LSN is
  stream-local, never compared across streams" — was retired by M0. What
  is left of the phrase is the *stamp*, the 16-bit `flags` word at offset
  2 carrying `core_id + 1` (`page_header.hpp:137-158`), which D4 proposes
  to drop. That is a page-header format question, not an LSN question, and
  AM-R4 takes it.
- **"Page latches" is not a widening. It is a first build.**
  `include/kds/base/latch.hpp` is included by **exactly one file** in the
  tree, `include/kds/wal/stream.hpp` — verified at `f6ed10c`. There is no
  page latch. `heap-and-tuple.md` §6 specifies one ("pinned for the
  duration of any access and **latched** — shared for reads, exclusive for
  structural mutation"), and what makes that specification true today is
  not an implementation: it is that a page belongs to one core and the
  reactor is run-to-completion between suspension points. M1 has to write
  the thing the spec has been describing.

**What M1 is not:** it is not the lock manager. AR0 §8 puts that in M2,
and the ordering is not cosmetic — see AM-R1, which is the ruling the
whole milestone turns on.

---

## AM-2 — Where this sits against AR0's D-items

| D-item | Bearing on M1 |
|---|---|
| D4 (free-map / superblock access rule; "page LSN becomes scalar; drop the stream id field") | **AM-R4 and AM-R5.** The `[source-read required: page header layout]` tag is discharged in AM-3 E |
| D14 (no in-place migration; a new major version mounts only its own volumes) | **AM-R4 depends on it.** Dropping the stamp field is a format event, and D14 is what makes it payable |
| D2, D8, D9, D12, D13 (lock manager, gap locks, FK, deadlock, async waits) | **M2, not here.** AM-R1 is what keeps them out |
| D1 (isolation target) | Not needed by M1, and M1 must not presuppose it |
| D10, D6 (affinity weight, spreading default) | Measurement-gated on RW-C1; untouched |

---

## AM-3 — The survey: what a shared pool would meet, at `f6ed10c`

**A. There is no page latch, and no other engine code takes a lock.**
`base/latch.hpp` has one includer (`wal/stream.hpp`). A grep for `Latch`
across `include/` and `src/` finds the WAL and nothing else. The other
locks a reactor thread can reach are `FileLogDevice::segments_mutex_` and
`WalWriter::mutex_`, both inside the WAL (`sched.md` §9-2). So M1 adds the
**first** lock outside the log, and `rules.md` §3's declared-sharing table
gains its fourth row.

**B. The pool is per core, and `eviction.md` argues for it in the strongest
terms the tree contains.** §1: *"Per-core pools (S7) + thread-per-core ⇒
the entire replacement mechanism is core-local. There are no latches, no
lock-free tricks, and no cross-core coordination anywhere in this design.
This is the structural advantage over shared-pool engines (PostgreSQL's
buffer mapping and clock sweep contend on locks; KDS simply has nothing to
contend on)."* EV4: *"Strict per-core pools. No cross-core frame stealing,
no rebalancing in v1 … any stealing path reintroduces cross-core
synchronization, forfeiting the lock-free property."*

**This is the single largest thing M1 reverses**, and it is reversed by
argument, not by deletion: the spec's claim is *true*, and M1 is the
decision to pay that cost for something else. `eviction.md` §1 and EV4 get
rewritten to say what was bought, or the spec becomes a lie about the
engine.

**C. `DevicePageStore` declares itself core-local.** `device_page_store.hpp:97`:
*"Concurrency: core-local, no internal synchronization (rules.md #3)."*
Pin counts and frame state are plain non-atomic fields (`page.md` §6),
which is stated as a *feature*: "multi-core adds instances, not
synchronization."

**D. The store carries an ownership apparatus that a shared pool does not
obviously need.** `MayFault` / `MayWrite`, extent leases, write grants
(PW1c-4) and stamp claims (PW1c-7); `CreateAtUnpinned` refused on a leased
store; `FlushMaps` refused on a leased store; the system range readable
everywhere and writable only by core 0. Whether any of it survives a
shared pool is AM-R2, and the answer is not "delete it".

**E. The page header, for D4's `[source-read required]`.** Common 32-byte
header (`page_header.hpp`): `page_type` at 0, `format_version` at 1,
**`flags` at 2 (2 bytes) — the PL-C stream stamp, `core_id + 1`, 0 =
never stamped**, `checksum` at 4, `page_lsn` at 8. So D4's "drop the stream
id field" is a **2-byte hole at offset 2 in every headered page**, and
`FormatPage`, `SetPageStreamStamp`, `StreamStampFor`, `StampIsForeign` and
`device_page_store`'s claim-at-fault are its users. Not a bump-free change.

**F. `buffer_pool_frames` is an instance total already.** `expeditor.cpp:204`:
*"the budget is an instance total divided evenly per core, remainder to
core 0"*, and a value below `cores` is refused at boot because a share of
zero is meaningless. Under a shared pool the division disappears and the
refusal with it — the setting keeps its name and its meaning gets
*simpler*, which is what `CLAUDE.md`'s "never add a second name for a
quantity an existing setting expresses" asks for.

**G. `cores = 1` is where G2 has to be enforced, and the WAL shows how.**
`WalStream` takes `Latch*`; null means unshared and costs two branches.
A page latch has no such luxury: it would sit in the hot accessor path,
not once per append. AM-R3.

**H. Eviction's exhaustion protocol assumes a core owns its frames.** EV8's
bounded cooperative retry ends in `ResourceExhausted` on the *statement*;
under a shared pool the frame a statement waits for may be pinned by
another core's task, and "yield and retry" is no longer bounded by this
core's own progress. Untouched by AR0, and a real gap — AM-R6.

**I. The multi-core assembly is not under test, in two layers**
(`docs/inflight/known-gaps.md`). No test and no `sim/` cell reaches
`Expeditor::Open` — the only construction is production's
`src/server/main.cpp:344`. And the layer below it is worse than untested:
`CoreRuntimeTest` bootstraps a **single-stream** volume and models a peer
under **per-core** streams, running today only because `SetUp` overrides
the topology on its own copy. Removing that override failed **123 cells**
at `f027a3c`, every refusal correct.

M1 is a multi-core memory-model change and both holes are waiting for it.
AM-S0 closes them before anything else, rather than discovering them a
third time.

---

## AM-4 — Rulings AM-R1..AM-R7 (CLA's proposals)

**AM-R1 — M1 shares the *cache*, never the *authority*.**
This is the ruling the milestone turns on. A shared pool means two cores
can hold the same frame; it must not mean two cores can *write* the same
relation, because what makes concurrent writers safe is row locks and
those are M2. So through M1, **statement dispatch still routes a write to
the relation's owner** — unchanged from today — and the shared pool changes
only *where the bytes live*, not who may mutate them. The page latch M1
builds is therefore doing one job: protecting a frame from a concurrent
*reader* on another core, plus the writeback and sweep paths. Reversing
this — sharing the authority in M1 and deferring the locks — would convert
every lost-update into a quiet wrong answer with no gate anywhere, which
is exactly the risk class AR0 §4.1 warns about.

**AM-R2 — The ownership apparatus is narrowed by *proof*, not by
deletion.** `MayFault` becomes vacuous under a shared pool (any core may
fault any page) and goes. `MayWrite` **stays** for as long as AM-R1 holds,
because it is the enforcement of the very rule AM-R1 keeps — and a debug
assertion is not enough: it is the thing that turns a routing bug into a
refusal instead of corruption. Extent leases stay (allocator authority,
retained by AR0-4). Stamp claims are AM-R4's.

**AM-R3 — The page latch: one word in the frame, compiled out at
`cores = 1`.** Not a `std::mutex` per frame — a pool of 8192 frames would
carry 8192 of them. Proposal: a 32-bit word per frame holding the shared
count and an exclusive bit, acquired with CAS; and at `cores = 1` the
acquire/release pair is a **no-op the compiler removes**, satisfying G2's
"zero overhead" literally rather than "two branches". Whether `cores` is
a compile-time or a run-time zero is the sub-decision: a run-time branch
in the hot accessor is measurable, a compile-time one means two builds.
**CLA proposes the run-time branch and a measurement**, because two builds
is a testing burden the engine has never carried and the branch is
perfectly predicted. `[measurement-gated]`

**AM-R4 — The stamp field goes, and it goes in M1 rather than being left
to rot.** Under a shared pool no core "owns" a frame, so PL-C's claim has
no reader left — its last one is `device_page_store`'s claim-at-fault,
which AM-R2 removes with `MayFault`. Leaving a 2-byte field that nothing
writes and nothing reads is worse than removing it: the next reader
assumes it means something. D14 makes the format event payable, so
`flags` at offset 2 becomes reserved-and-zero, `StampIsForeign` and
`StreamStampFor` go, and `docs/spec/page-lsn-cross-stream.md` finally
leaves the tree — which `docs/inflight/known-gaps.md` records as undated
today. **A pre-M1 volume then does not mount**, per D14, and that is the
first time in this revision that has been true: M0 kept them mountable.
Say it out loud in the milestone row rather than discovering it.

**AM-R5 — The free map and the superblock stay core 0's.** D4 proposes
moving allocator authority to "the log core"; under M0 the log core *is*
core 0, so the proposal is already satisfied and needs no work. CC11's
"every core reads with the same authority; core 0 alone writes" survives
a shared pool unchanged — sharing the cache does not make a second writer.
**No change in M1.**

**AM-R6 — EV8's exhaustion protocol needs a new bound, and this is the
gap AR0 does not mention.** Today "yield and retry, then
`ResourceExhausted`" is bounded because the frames a core waits on are its
own and its own tasks release them. Under a shared pool a statement can
exhaust its budget waiting on a frame another core's task holds pinned,
and the retry budget stops being a statement-local fact. Proposal: keep
the budget and the truthful error, and **count the cross-core case
separately** so an undersized pool is distinguishable from a contended
one — an operator told `ResourceExhausted` needs to know which. New
counter, not a new setting.

**AM-R7 — `eviction.md` §1 and EV4 are rewritten as a trade, not
deleted.** The spec's argument against a shared pool is correct and must
survive as the reason the cost is being paid. §1's "KDS simply has nothing
to contend on" becomes a statement about what was given up and what for,
with AL-S8's numbers and M1's own beside it. A spec that quietly drops an
argument it used to make cannot be audited.

---

## AM-5 — Stages

Sizes: S ≤ ½ day, M ≤ 2 days, L more. Every stage: `critics-developer`
review, the full suite, sync with `origin/main` on the branch, stop.

| # | Stage | Cells (definition of done) | Size |
|---|---|---|---|
| AM-S0 | **The assembly under test, first and alone.** Two halves, and the second was found after this order was written. (a) A cell that boots a real `Expeditor` at `cores = 2` and asserts what each core came up holding — log, attach, superblock image, recovery report, catalog cache. (b) **Rebuild `CoreRuntimeTest` so a peer attaches to core 0's shared stream**, the way `Expeditor` wires it: the fixture bootstraps a single-stream volume and then models per-core streams, a combination no instance can be in, and it currently only runs because `SetUp` overrides the topology on its copy (`git show 30e0377:docs/inflight/bugs/core-runtime-fixture-models-per-core-streams.md`, closed by this row) | (a) the two M0 defects each reproduce against a deliberately reverted fix and are caught. (b) the override is **deleted** and `ctest -R CoreRuntime` is green — which is the whole cost of the stage, since removing it failed 123 cells at `f027a3c` and several of them assert on a peer's own recovery, which does not run under one stream. A per-core arm needs a volume that says so, and `BootstrapDatabase` has no parameter for it today | L |
| AM-S1 | The page latch (AM-R3): the primitive, its `cores = 1` compile-out, and its acquisition order against the WAL latch | contention cell at 8 cores; a `cores = 1` A/B showing the acquire/release pair costs nothing measurable | M |
| AM-S2 | The shared pool: one frame table, one CLOCK hand, `buffer_pool_frames` an undivided instance total (AM-R2's `MayFault` removal here) | a page faulted on core 1 is served from the frame core 0 loaded; the boot refusal for `frames < cores` goes | L |
| AM-S3 | Writeback and the WAL gate under sharing; EV8's new bound and counter (AM-R6) | flush-before-evict holds with two cores dirtying one page; the cross-core exhaustion counter is nonzero exactly when it should be | M |
| AM-S4 | The stamp field (AM-R4) and the format event; `page-lsn-cross-stream.md` leaves the tree | a pre-M1 volume is refused at mount, naming why; no reader of `flags` at offset 2 remains | M |
| AM-S5 | Prose: `eviction.md` §1/EV4 as AM-R7 asks, `page.md` §6, `heap-and-tuple.md` §6, `rules.md` §3's fourth row, `crosscore.md` CC7, `CLAUDE.md` | no spec claims a core-local pool | M |
| AM-S6 | The baseline against AL-S8's, same cells, same host | one results file per cell under `bench/v3.0.0/`, and **the delta to AL-S8 is the point** — that pair is within one engine and is exactly what D15 permits | M |

## AM-6 — Row status (CLA, appended as rows land)

| row | status |
|---|---|
| AM-S0 (b) | **done.** The override is deleted and `ctest -R CoreRuntime` is green; the legacy cells run on `CoreRuntimePerCoreStreamTest`, over a volume `BootstrapDatabase` genuinely wrote as `kPerCoreStreams`, and the fixture keeps the `CoreRuntime` prefix so that selector still reaches them. The bug file is closed and deleted. **No engine change:** of the 23 cells the override was hiding, 19 were the rig bounding a wait by a round count where M0 made durability core 0's writer thread's `fsync`, three were legacy, and one restarted an owner without flushing the pages its own revival reads off the device. The finding worth carrying into S1-S3: **a peer's commit is now durable in wall-clock time, not on the tick that staged it**, so any test that waits for one waits on a deadline (`ForeignIndexRig::TurnUntil`) |
| AM-S0 (a) | **done.** `Serve()` split into `Start()` + `RunUntilStopped()` on the operator's word, its nine locals moved verbatim into a `ServeRuntime` the .cpp defines, and `tests/expeditor_test.cpp` runs a real instance at `cores = 2` over real loopback sockets — the assembly asserted between the halves (one log, the peer attached to it, the volume's own image, the mount report, the catalog cache), then served and stopped by `STOP` like an operator stops it. **Two of AL-7c's three defects reproduce against a reverted fix**: the null `log_device_` dumps core in the assertion cell while the other three stay green (the short-circuit on an empty list, which is why nothing caught it), and the `cores = 1` latch fails `AtOneCoreTheStreamsLatchIsNeverArmed` by name. **The third does not, and the reason is the finding**: the scan floor needs the fold's winner to be a *cadence* record, whose `redo_start_lsn` is an old recLSN while its `checkpoint_lsn` is recent, beside a core whose shutdown checkpoint never published — a crash. A clean stop cannot produce it because each core's `ShutdownCheckpoint` syncs its own store first, so the fold's minimum is core 1's earliest `CHECKPOINT_BEGIN`. Filed in `docs/inflight/known-gaps.md` as a defect fixed with no regression test under it, **with the unit-level harness that would pin it** — two synthetic anchor records against `SuperBlockCheckpointAnchor::Publish` and the floor choice, no instance and no crash. **And the split surfaced a live one**: five of `Start()`'s twenty early returns are after the peer threads are spawned, and unwinding a `std::vector<std::thread>` with joinable threads calls `std::terminate` — pre-existing, now `StopStartedCores()` |
| AM-S1 | **Built 2026-09-03** on `ar2-borrow-model` from `a68dbc3`, on the operator's word in plan mode — which is what lifted AM-1's "not before AL-S8's numbers are read" gate; the numbers were in `bench/v3.0.0/` by then. **The primitive**: `include/kds/storage/page_latch.hpp`, one `uint32` per frame — an exclusive bit, the owning core in seven bits, a 24-bit count — driven by CAS through `std::atomic_ref` so `Frame` stays a movable aggregate (AM-R3's "one word in the frame"; `sizeof(Frame)` is unchanged at 32 and asserted for the first time). Shared readers, one exclusive owner, **re-entrant for the owning core** — the census found that one task holds a page twice on every chain-growth and split path — and **never upgraded**: a shared holder asking for exclusive is a self-deadlock the store aborts in debug naming the page. **The compile-out** is AM-R3's run-time branch: `SetLatchArmed(core_count > 1)` from the superblock, set by `Expeditor::Open` before recovery and by `CoreRuntime::Open` after the stream identity; an unarmed store never touches the word, asserted at `cores = 1` on the assembly (`AtOneCoreThePageLatchIsNeverArmed`). **The order, and the survey's brief was wrong about it**: the page latch is **outer** to the WAL stream latch, not inner — `LogFullPageImage` and the catalog and Bound Cabin chain growth append under a page latch, and no WAL path asks for a page latch *while holding the stream latch* (recovery's redo does take page latches, on the mount thread with no stream latch held and no second thread alive — the `critics-developer` pass caught the first draft's "nothing in `wal/` asks for one" as false and corrected it in three places); writeback takes no page latch at all today, but the fault path may hold *other* frames latched across the writeback it triggered, which is sound (the writer thread takes no page latch) and a latency cost AM-S3 prices; **page against page is unordered through M1** and AM-S2 owes the rule for the shared pool, which the review found the first draft had omitted. `rules.md` §3 gained the row and `page.md` §6 the declaration in the same change. **The census**: `KDS_TEST_PAGE_LATCH=1` arms every debug store and wins over the assembly's own `SetLatchArmed` (the review found the first form was silently disarmed at one core by `Expeditor::Open`, so the assembly-driven fixtures had run unarmed); the whole suite run armed named exactly two S-then-X sites — `btree.cpp`'s and `index_tree.cpp`'s leaf-for-write re-fetch, each holding a read handle while asking for the page exclusive — fixed by releasing the read handle first; nothing else in 3,294 cells nests wrongly, and the two cells whose subject is the unarmed path skip under the override rather than fake it. Cells: 8 on the word (`tests/page_latch_test.cpp`, the 8-thread contention cell asserting contention happened, the waiter cell publishing its first *refused* attempt), 4 on the store (unarmed word stays 0; modes per accessor; nested holds; the sweep and `EvictClean` refuse a frame another core holds — the scan ring's refusal at `ReleaseScanSlot` has no cell, a gap found on review 2026-09-04), 2 on the assembly. Dropped on review: a contention gauge nothing read but the cells' own zero-assertions. **Suite: 3294/3294 plain and 3294/3294 armed** (`ctest -j8`, Debug; the armed run skips the two by-design cells). The `cores = 1` A/B is the row below |
| AM-S1 A/B | **Measured 2026-09-03** on `ar2-borrow-model` at `v2.7.0-183-gc985d37`, on the operator's word despite `CLAUDE.md`'s suspension of the interleaved A/B; the file is `bench/v3.0.0/results-am-s1-page-latch-v2.7.0-183-gc985d37.md` and the raw run is beside it under `archive/`. `tools/scenario0_stockmarket.py` at `cores = 1`, `group` and `strict`, A = the parent engine (the hashed copy built at `92cb654`, whose engine is byte-identical to `a68dbc3`'s) against B = `c985d37`, twelve cells in three passes, each pair interleaved with the order reversed. **Nothing measurable — and the number bounds the compile-out, not the latch.** At `cores = 1` the store is unarmed and never touches the word, so what the A/B prices is the `latch_armed_` branch per pin and unpin, the mode argument through `PinFrame`, the `Frame` layout, and the two release-before-refetch fixes. Clean group cells: A 668.4 / 731.1 / 732.6 TPS (`txn` p50 11,173.5 / 10,575.7 / 10,503.5 µs), B 711.9 / 723.9 (10,840.1 / 10,643.5). Clean strict cells: A 186.8 / 191.6 TPS (p50 37,891.9 / 37,724.6 µs), B 194.2 / 193.3 (37,172.9 / 37,267.0). **The file was corrected 2026-09-04 for how the degraded cells were excluded**: the first reading dropped them cell-wise, which kept the unpaired `g1-A` (668.4, the slowest clean cell) in A's group mean and produced both the 9.6% A spread and the "B inside it" conclusion. Under the project's own "repeat the pair, not the cell" rule the whole `g1` pair drops, and then **group flips sign — A ahead 1.9%**, with both surviving group pairs agreeing (A faster by 2.7% and 1.2%) and A's own spread 0.2%; strict is unchanged at **B ahead 2.4%**, both pairs agreeing. So the compile-out's cost at `cores = 1` is bounded at a couple of percent with opposite signs in the two durability classes, not "nothing measurable inside a 9.6% spread". Three cells (g1-B, g3-A, g3-B) are degraded by host-level device stalls and excluded from every comparison: their mount-time completion checkpoint took 434 / 168 / 61 ms against 5.2–7.4 ms in the other nine, before the driver's first statement, so the thermometer is arm-independent; their medians are normal and only the tails moved. **The armed cost is not measured**: the CAS pair per pin and contention at `cores > 1` have only the 8-thread unit cell behind them, and the multi-core number is AM-S6's against AL-S8. Two further gaps the file names: `relaxed` (D3) was not run, so the durability axis is swept from `strict` only as far as the `group` default, and the row-set size is not swept. |
| AM-S2 | **Surveyed and step 1 of 6 built, 2026-09-05** on `worktree-ar2-borrow-model-2` from `36ed0fd`. **The survey moved the stage's shape.** At `cores = N` there are N frame tables over one device (`core_runtime.cpp:163` beside `expeditor.cpp:741`), and inside a store only the page-latch word is atomic: `frames_`, `clock_hand_`, `live_pins_`, `pin_high_water_`, `dirty_eviction_queue_` and `Frame::pins` are plain. So AM-S2 is not "share the table", it is **"give the table a concurrency protocol it has never had"**, and every ruling is about that rather than about sharing. **The constraint that looked hardest is absent**: a pinned page's bytes survive any table reorganisation, because `PageRef` holds `store_`, `page_id_` and a raw `data_` pointer rather than a `Frame*` (`page_store.hpp:93-100`) and `Frame::bytes` is a `unique_ptr<Page>` the table never moves — so the representation stays free, including EV05's open-addressed form, and the protocol covers `Frame` *metadata* only. **Step 1: the structure latch and the pin protocol.** `Latch frames_latch_` with `structure_latch()` returning null unless armed, which is `LatchGuard`'s shape and what keeps G2 — at `cores = 1` the guard is a null test and no atomic. **The substance is an ordering, not a lock**: `PinFrame` takes the pin *under* the structure latch and waits for the page latch *after* releasing it, because a frame with `pins > 0` is never an eviction victim (EV4), so the pin itself is what keeps the frame alive during the wait. Holding the structure latch across that wait would put every core's lookup behind one page's contention; holding it across a device read would put them behind a disk. `UnpinFrame` mirrors it — release the page latch, then drop the pin. **Two things the design did not anticipate, both found by building it.** (a) **`Frame::pins` does not need to be atomic**, though the plan said so and the field's own comment names this stage as the owner of that change: every mutation is now under the structure latch, so an atomic is redundant. Dropped as premature. (b) **The never-upgraded check silently inverted.** It reads `pins != 0` to mean "this core already holds a share and is now asking for exclusive"; moving the pin ahead of it makes that true on *every* exclusive pin of an unshared frame, which is a debug abort on ordinary traffic. It is `> 1` now, with the reason at the site. **And one correction that is worth more than the change it corrects**: CLA split `UnpinFrame`'s pin and gauge decrements, called it a defect, fixed it, and wrote a cell to prove the fix mattered — **the cell passed with the fix reverted**. The early return already filters `pins == 0`, so an uncoupled pair misbehaves only if two unpins race at `pins == 1`, and one pin means one handle, so only one unpin can be in flight. The window is unreachable under correct usage. The coupling stays because it is the right semantics; the justification was wrong and both the comment and the cell now say what was established rather than what was assumed. **The cell** is `tests/am_s2_pin_protocol_test.cpp`: eight threads, sixteen real frames of one armed store, and it retires a stated limitation — `page_latch_test.cpp`'s eight-thread case says "the store's frame table is not thread-safe and cannot host it, so the words stand in for frames", which step 1 is exactly what removes. It carries the ordering **structurally**: hold either latch across the other and it deadlocks rather than fails, so a run that finishes is the evidence, and the gauge check turns that into an assertion about the accounting. **Steps 2-6 not started**, and the order matters: step 2 adds the per-frame *loading* state so a miss releases the latch before the device read instead of holding one across it; step 3 makes `CoreRuntime` hold a reference rather than its own store; **steps 3 and 4 cannot be split**, because the moment the store is shared `MayFault` is answering about a table with no owner. Nothing is shared until step 3, so step 1's unlatched lookup path is incomplete rather than unsound. **Suite: 3345/3345 plain and 3343/3343 armed** (`KDS_TEST_PAGE_LATCH=1`, the run that exercises the new ordering). Overhead not measured **The `critics-developer` pass on `881987f` broke three of this row's own claims, and one of them changes the remaining plan.** (H1) **The pin is taken *after* the fetch, so the ordering argument does not cover the window that needs it.** Every accessor is `bytes = *Unpinned(id)` then `PinFrame(id)`, which re-looks-up (`include/kds/storage/page_store.hpp:151-196`); between the two a frame can be evicted and its `Page` freed, and `PinFrame`'s not-found branch then hands `PageRef` a dangling `data_` — or worse, an evict-and-re-fault in that window pins a **different** frame while `data_` points at the freed page, **with `live_pins()` and `pinned_frames()` balancing perfectly**. `page_store.hpp:140-147` already records the obligation verbatim: "the shared pool AM-S2 builds must latch the frame table **across the pair**". Step 1 latched the pin alone. So "the pin keeps the frame alive" is true **from the pin onward** and says nothing before it, and **step 2 as designed does not close this** — the window is in `PageStore`, not this class, so closing it makes fetch-and-pin one operation and is an interface change. Steps 2–6 all inherit it. Related: this row's own framing — that pinned bytes survive reorganisation *because* `PageRef` holds a raw `data_` rather than a `Frame*` — is **inverted as a safety argument**: true of rehashing, false of eviction, since a `Frame*` dangles loudly where a `data_` into a freed page dangles quietly. (H2) **`pins > 0` does exclude every erase path, but not because of the new latch**: all three erasers (`ReleaseScanSlot` `:619`, `EvictClean` `:1322`, `EvictColdFrames` `:1638`) read `pins` unlatched, and it is *mutated* unlatched at `device_page_store.cpp:603-605` (`ResidentBytes`' inline-sweep guard pin). So this row's stated reason for dropping the atomic — "every mutation is now under the structure latch" — is **false as written**, with a counterexample in the same file; the conclusion holds for the reason that actually applies, one thread. (B3) **`kPinCeiling` became a live abort.** It is the *per-operation* bound and `live_pins_` was a proxy for it only while one core ran one operation; this stage turned it into a cross-thread sum behind `std::abort()`, and the new cell passed only because eight threads times one handle equalled the bound exactly — **nine readers abort the process**, and at step 3 with N cores each running a 7-deep descent it fires in every debug build. Fixed by re-scoping `SetLatchArmed` to take the concurrent-pinner count (no second knob, per `CLAUDE.md`), and the cell now runs at **twelve** threads so it cannot pass by arithmetic coincidence again. **The cell also failed its own claim**, by mutation: taking the pin *after* the page latch **passes**, and holding the structure latch across the acquire **passes** — it takes shared holds only, and a shared acquire never blocks against other shared holders, so there is nothing being waited for that could be held across. It discriminates that pin counters are under *some* mutual exclusion, which is real and is all it is; an ordering cell needs an exclusive acquirer per page and a concurrent eviction path, which step 2 can give it. **Claim 2 held for the engine and failed for the suite**: no production path reaches one store from two threads (verified — the only `std::thread` spawns are `WalWriter`'s, which holds no store, and `Expeditor`'s per-core workers, each with its own store), but the new cell is the suite's **first unsynchronized access to a `DevicePageStore` field** — `ResidentBytes` bumps `Frame::usage` unlatched at `:514`, so twelve threads race on the field the sweep reads. Benign on x86, UB by the standard, TSan-visible; latching the hit path's usage bump belongs with step 2 and is **not fixed here**. Two review fixes applied on top: the never-upgraded diagnostic printed the caller's own pin (`> 1` is the right threshold, but the message printed the raw count), and the file's protocol header still quoted `pins != 0`. **Step 2a built the same day, and it is the half of step 2 that separates cleanly.** The review showed fetch-and-pin must be atomic and that step 2 as designed did not close it; writing the two down together then showed the ordering question was vacuous — **bracketing the pair holds the structure latch across the raw fetch, which reads the device on a miss, so the only way to make the pair atomic *without* latch-across-I/O is the loading state itself.** They are one change. What splits is not the mechanism but the two costs: the unprotected window is a **wrong answer**, the hold across a read is a **slow** one, and no store is shared until step 3, so the slow one is unreachable in production today. So 2a lands the bracket: `PageStore::FetchPinned` is one overridable operation whose default is exactly the fetch-then-pin the accessors used to inline — leaving `InMemoryPageStore` and `page_mgr`'s `BufferPool` untouched — and `DevicePageStore` overrides it, unarmed keeping today's shape and cost, armed running the pair under one hold. The comment at the site says outright that the hold spans a device read and that this is the thing the design forbids, kept for one release. **Step 2b built the same day, and it is not the design above.** The design assumed a per-frame *loading* flag and a placeholder frame with invalid bytes; the code reads into a standalone `unique_ptr<Page>` and calls `InsertFrame` only afterwards, so there is no frame to mark while the read runs and a placeholder would have had to be invented for every other reader of the table to skip. What shipped is a **set of in-flight page ids** (`loading_`) plus one condition variable: a hit pins under the latch, an id already in flight waits and re-checks from the top, a miss records the id, drops the latch, runs the raw fetch outside it, then re-takes, erases and broadcasts. `Frame` and `EnsureResident` are unchanged. **The `critics-developer` pass found three defects in 2a/2b, all fixed.** (F1, highest) 2a's armed path wrote the pin accounting by hand instead of calling `PinFrame` — which is where MG04's pin ceiling and AM-S1's never-upgrade census both live — so from 2a onward **every armed store skipped both debug detectors**, including the whole `KDS_TEST_PAGE_LATCH=1` census run, and AM-S1's headline claim about naming exactly two S-then-X sites stopped being reproducible. A green armed suite is what a dark detector looks like. Fixed by extracting `CountPin`/`AcquirePageLatch` so there is one definition of each. (F2) An **exception** in the load window left the id in `loading_` forever, so every later fetch of that page parked for the life of the process — the commit's "the broadcast fires on both arms" covered ok and not-ok and missed the third arm; the build has no `-fno-exceptions` and the raw fetch allocates. Fixed with a `LoadingGuard` whose destructor erases and broadcasts. (F3) **CLA's own cell discriminated nothing**: with the wait arm compiled out it passed **40/40**, because `std::thread` construction costs more than a `MemoryPageDevice` read, so the first faulter finished before the second started and the trace showed one read because there was only ever one faulter. CLA's single mutation run had caught it once, by luck, and CLA reported that as discrimination. Fixed with a start barrier plus 200 rounds and an eviction between them. **R1, taken here**: 2b moved `InsertFrame` out from under the one hold that covered it at 2a, which under sharing is an `unordered_map` rehashing while other cores are inside `find`. `InsertFrame` now takes the structure latch itself — safe because all three callers reach it with the latch not held. **R5, taken here**: both production sites passed the default pinner count of 1, so step 3 would have reinstated the ceiling abort from a different direction; both now pass `core_count`. **Reported and open**: `ScanRing::Fetch` faults outside `loading_` entirely and `ReleaseScanSlot` is a third unlatched eraser, so a ring fetch racing a load can still clobber a `Frame` through `insert_or_assign` (R2); armed and unarmed disagree on CLOCK usage after a fault, since rounding the loop runs the raw fetch twice (R3); the `Create*` trio still does fetch-then-pin inline, and its frames are **dirty**, not clean, so the window needs a concurrent writeback first — narrower than `Get`'s but real, and `page_store.hpp` announces the obligation discharged directly above three accessors that did not discharge it (R4); the hit path's `loading_` test is load-bearing and untested, because the fixture's `frame_budget_` is 0 so the new cell never runs the inline sweep at all (R6); and **no `PageDevice` declares itself thread-safe** while 2b makes concurrent reads of different pages possible for the first time (R8) — step 3 needs that written down before it needs anything else here. **Suite: 3345/3345 plain, 3343 + 2 by-design skips armed.** **Erasers, R2 and R3 landed at `dd833d6`.** `EvictClean` and `EvictColdFrames` take the latch (the second split into a public door and an `EvictColdFramesLocked` body, since `base/latch.hpp` is not recursive), and **the plan for the inline sweep was the wrong way round**: this file and `FetchPinned` both said the sweep running *outside* the latch is "what makes latching the erasers possible at all", and that shape leaves the fresh frame unprotected between the insert's hold and the sweep's, and races the sweep's hand-pin against any latched pin. The sweep moved *inside* `InsertFrame`'s hold behind a `sweep` flag only the miss path passes. R2 is closed by losing the race rather than winning it (a resident frame outranks bytes read second; `try_emplace` makes it structural), R3 by pinning where the load finished — the armed miss path used to round the loop through the *hit* path, applying the CLOCK usage bump twice, so **armed and unarmed disagreed about how many rotations a page survives its own fault** and nothing could see it. The review found the equivalence cell CLA wrote for it **could not fail under `KDS_TEST_PAGE_LATCH=1`** (the census override arms every store and wins over `SetLatchArmed`, so both arms were armed), and measured that **the block the sweep moved into executes 0 times in either gate configuration** — `EvictionInsertSweepTest` is the rig that reaches it, 8 executions and 8 for the whole binary. **Step 3a at `e37ebae`**: `base/current_core.hpp`, a thread-local declared in `Scheduler::RunOnce` and by `CoreRuntime::Open`'s guard, because the page latch records the core that *asked* and one store cannot answer that from a member; the two alternatives both land on `store()`'s **337** call sites. Instrumented, the two identities disagree **14,965** times in the armed suite — all fixtures driving several cores' stores from one thread, none in production. **Step 3b at `ac6cc3d`**: the never-upgrade detector was `pins > 1 && HasSharedHolders`, on `page_latch.hpp`'s own argument that "its pins are this core's through M1" — which step 3 ends, since `pins > 1` becomes two cores holding one pin each. It is a debug thread-local multiset of the pages this thread holds shared now, verified by firing it rather than by a green suite. **Remaining for S2 — steps 3c and 4, which cannot be split.** The survey for them turned up three constraints. (i) `extent_lease.hpp` states the premise the sharing removes: leases exist because "per-core page stores do not work without it", so with one store the free map is simply the store's and the lease, the CC7 fault grants, `MayFault`, `MayWrite`'s ownership arm and `TryClaimByStamp` lose their reason together — which is also where AU-S4's `kRelationFaultGrant` is struck. (ii) **The WAL gate is sound under one stream only**: `WalDurability` is a property of the *log*, and every core's manager attaches to core 0's stream under AR0 M0, so any of them answers for all — but a pre-M0 volume mounts per-core, where a shared store would check a page logged in core 1's stream against core 0's watermark. Sharing is therefore conditional on `single_stream()` until AM-S4 refuses the volume outright. (iii) The fixtures that drive several cores from one thread need a `CurrentCoreGuard` per core at that point, because a thread holding one *shared* table's word while claiming to be several cores is a shape production cannot reach. **Step 3c-ii — the sharing itself — was written, measured, reviewed and NOT landed** (2026-09-05; stashed `35b85a9`). It works and all three configurations are green, which is exactly why it is not landed: the review measured two defects no cell can see.

**(1) Allocation is unsynchronised, and sharing is what reaches it.** A probe in `CreateNewUnpinned`'s free-map arm over `ExpeditorTest` printed one store object and three OS threads (`core=0 tid=552691`, `core=1 tid=552690`); with sharing forced off, only core 0's lines appear. Four threads x 400 `CreateNew()` on one unleased armed store gave **~14% duplicate ids** — 218 of 1600, then 221, 230, 235, 229, 237 — because `FreeMapFindFirstFree` sits in `CreateNewUnpinned` and `FreeMapAllocate` two calls away in `CreateAtUnpinned`, a gap no latch that fails to span it can close. The in-use guard catches 0–3 of the ~230. **The free-map byte race CLA expected to be the durable half measured `lost_bits=0` in six runs**: two threads mostly collide on the same bit, which loses nothing.

**(2) Sharing the table before step 3 latched its readers is a segfault, not a slow path.** This header's own list names them and says step 3 takes them one at a time; 3c-ii took none. With allocation removed from the picture (disjoint id stripes), two inserters plus two `StampPageLsn` callers **segfaulted in 3 of 5 runs**; latching `StampPageLsn` made it 8 of 8 clean. That call is on every logged page mutation on every core.

**The review corrected CLA on the fix's shape.** The lock-order inversion is real but is an order to *declare*, not a wall: **structure → allocation**, allocation the inner leaf, with one edge to move (`CreateAtUnpinned` must end its allocation hold before calling `InsertFrame`). `AdoptDeviceMapOnMiss` returns `false` on its first line when `lease_ == nullptr`, so on a shared store the hit path never touches the device — stronger than the argument the header already makes for it. `map_regions_` is a `std::map`, not an `unordered_map`, and is not the frequent race. **Recommended**: one latch with the four device calls hoisted out (`EnsureCapacity`, `LoadRegionIfPresent`, `EnsureAddressable`, `DeviceHoldsOnlyZeros`), resolved before the hold and re-validated under it the way `FetchPinned`'s `loading_` retry already does — leaving a bitmap scan over one 8 KiB page with no I/O — **and the claim and the mark become a single step**. Keeping peers on leases is not available: `lease_` is a member of the store and there is now one store.

**Four side-effects the step also owes**, all measured: `RefreshFreeMapFromDevice` refuses outright when `lease_ == nullptr`, so it fails at every peer call site (14 refusals over `ExpeditorTest`) and `AdmitWritePages` treats that as fatal and abandons the write grant; `SetResidentLimit`'s only caller is `SetCoreOwnership`, which a borrowing peer skips, so EV3's resident-by-class floor is 0 for the instance's one pool; `InstallSuspendAudit`'s `live_pins() != 0` becomes an instance-wide sum, so a peer parking while core 0 holds a pin trips it; and each peer still `Reserve`s a 64-page extent for a lease it no longer installs, which nothing frees.

**One defect the review fixed in the stash**: `Expeditor::Open` applies `FrameBudgetShare` at store open and 3c-ii did not touch it, so the instance's only pool came out a factor of `cores` smaller than configured — `configured=100 cores=2` measured `pool_budget=50`, where the two-table arrangement it replaced held 2 x 50. **And CLA's cell was one assertion wide**: of the three added, only the pointer identity discriminates. `frame_budget()` is equal under either arrangement, and `resident_pages()` was read on *core 0's* store on both sides of a peer dispatch, which two tables do not move either. The discriminating form is a peer reading something core 0 has **not** faulted, asserting core 0's count grows. |
| AM-S3..S6 | not started; AM-1 says why AL-S8 gates them |
