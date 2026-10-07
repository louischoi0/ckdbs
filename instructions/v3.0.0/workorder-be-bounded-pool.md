# Work order BE — a bounded buffer pool: the configured frames are a ceiling, reclaimed in batches

Written 2026-10-07 on `worktree-pool-budget-required` from `e352eac0`
(`v2.7.0-657-ge352eac0`). It follows the operator's words, in order:

- *"buffer_pool_frames=0은 오류. 이 값이 최대 값이 됨"*
- for the missing key, *"부팅 거부 (필수 키)"*
- *"결정만 기록"*, which `e352eac0` did
- *"작업 지시서를 작성하고 handoff를 준비해줘"*
- **W1:** *"push it, and mark BE-Q0..Q10 as proposed"*

**Opened 2026-10-07 by W1** (`raft-marks-2026-10-07.md` §16): BE-Q0..Q10
are marked as proposed, so BE-R1..R5 are rulings and BE-S1 is the next
stage. The ruling itself is marked (`raft-marks-2026-10-07.md` §14) and
recorded in `eviction.md` §6 as not built.

**BE is not part of AR0 §8's chain.** It answers an incident outside the
engine. On 2026-10-07 an xrock bulk load ran against an 11.3 GB volume
with `buffer_pool_frames` unset. It grew `kds_server` to 13.1 GB of
anonymous heap, and the process was OOM-killed on a 15.6 GB host.
Recovery was clean and the data was whole. A read-only rerun grew the
process to the volume's size again. The incident report sits untracked
under the main checkout's `docs/report/`.

BE neither depends on BA, BB or BD nor blocks them. Its one surface shared
with BA is BA's P1, the frame table, which BA-R5 partitions and BE-R1
rebuilds (BE-Q9).

## 0. What BE is

The ruling makes `buffer_pool_frames` a required key and makes `0` an
error. The value becomes the pool's **maximum**: resident frames never
exceed it. The engine at `e352eac0` does the opposite in four ways:

- the key is optional;
- `0` is unbounded;
- a nonzero value is soft;
- page creations never sweep at all.

All four are in §1.1.

Building the ruling as a bare refusal would hand every operator a pool
with a real ceiling and an unaffordable cost past it, because the sweep
that enforces the ceiling sorts the whole frame table on every miss
(§1.2). So BE builds the ceiling together with the machine that makes
living under it cheap, in this order:

- **First**, the machine: a slot array with a free list and a real CLOCK
  hand (BE-R1), and reclaim in bounded batches (BE-R2).
- **Then** the ceiling, which becomes hard for misses and creations alike
  (BE-R3, BE-R4).
- **Last**, scan resistance, so a scan larger than the pool does not
  displace the working set (BE-R5).

**BE's rules, one line each:**

- **BE-R1.** Frames live in a slot array that grows in chunks up to the
  cap. A free list hands out slots in O(1). The CLOCK hand walks slots,
  never a sorted copy of the table. A miss and a creation reserve their
  slot before they fill it.
- **BE-R2.** Reclaim runs in batches, and every batch is bounded in the
  steps it walks. An empty free list reclaims a batch inline. A
  background task holds the free count between two watermarks, and its
  drain cleans dirty frames between statements.
- **BE-R3.** `buffer_pool_frames` is required and nonzero, and the store's
  unbounded mode is deleted.
- **BE-R4.** At the cap with nothing reclaimable, a fault or a creation
  retries the sweep a bounded number of times, writing nothing back. Then
  it is refused `ResourceExhausted`. The pool never grows past the cap.
- **BE-R5.** The outermost read walk faults its pages in cold (usage 0). A
  hit bumps usage as it does today.

**BE does not do:**

- **Route executor scans through the scan ring** (EV6's full form). §1.6
  says why that costs more than BE-R5. If BE's close measures that the
  working set is still displaced, it becomes its own order.
- **Write back from the fault path** (BE-Q4). The store's header forbids
  it as built (§1.5).
- **Resize the pool at runtime.** The cap is fixed at boot, as
  `eviction.md` §7 already says.
- **Return memory below the high-water mark.** Slots are kept once
  allocated. The ceiling is the promise, not a shrink.
- **Add configuration keys** (BE-Q5).
- **Change anything on disk.** The superblock does not move.

## 1. Survey at `e352eac0`

This section was read, not run, except for §1.2's standalone timing. It
was corrected by BE-S0's review (§6).

### 1.1 The key is optional, `0` is unbounded, a value is soft, and creations never sweep

- **Parsing.** `Expeditor::Config::ApplyFile` reads the key only
  `if (file.Has("buffer_pool_frames"))` (`expeditor.cpp:190-199`).
  `Expeditor::Config` defaults it to `0` (`expeditor.hpp:130`).
- **No config file.** `main.cpp:197-216` applies a file only when
  `--config` is passed, so a server started without one runs unbounded.
- **Where a budget is applied.** `expeditor.cpp:724` and `:1680` apply a
  budget only when it is nonzero. The store keeps `frame_budget_ = 0` as
  "unbounded (pre-eviction behaviour)" (`device_page_store.hpp:1517`).
- **A dead per-core budget.** `CoreRuntime::Config::buffer_pool_frames`
  (`core_runtime.hpp:134`, applied at `core_runtime.cpp:191-193`) is dead
  in production. Every peer is given the shared store
  (`expeditor.cpp:1715`) and a budget of `0` (`:1720`).
- **The struck per-core share is still in the code.** `CheckFrameBudget`
  (`expeditor.cpp:107-115`) refuses a nonzero total below `cores`,
  arguing that "a share of zero means unbounded". `:725` applies
  `FrameBudgetShare`, and `:1681` overwrites it with the total.
- **Soft.** A sweep that reclaims nothing lets the insert proceed past the
  budget (`known-gaps.md`, the EV8 entry).
- **Creations never sweep.** `CreateNewUnpinned` and `CreateAtUnpinned`
  reach `InsertFrame` with `sweep = false` (`device_page_store.cpp:1139`,
  `:1187`; the header at `:1262-1266`). A bulk load mostly creates pages,
  so even a configured budget does not bound the incident's own workload.
  Creations also run under a split's exclusive holds.
- **The debug override.** `KDS_TEST_FRAME_BUDGET` sets a budget on every
  debug-build store (`device_page_store.cpp:318-325`), so the whole suite
  can run under a brutal budget.

### 1.2 The sweep sorts the whole table per miss

- **The table.** `std::unordered_map<PageId, Frame>`
  (`device_page_store.hpp:1529`). Each frame owns a separately allocated
  `std::array<std::byte, 8192>` (`Frame::bytes`, `:844`). There is no
  free list: a miss allocates and a reclaim frees.
- **The sweep.** `EvictColdFramesLocked` (`device_page_store.cpp:2546`)
  copies every resident id into a vector and `std::sort`s it, then
  `lower_bound`s the clock hand. It walks up to
  `size * (kClockUsageCap + 1)` steps (`:2570`), all under the structure
  latch.
- **A quadratic step inside it.** Each dirty victim costs a `std::find`
  over the dirty queue (`:2603`). That is quadratic when the pool is
  mostly dirty.
- **The trigger.** In production its only trigger is `InsertFrame`'s
  `sweep` arm (`:690`), asking for the excess. Past the budget the excess
  is one frame, so **every miss pays one whole-table sort**.
- **The timing.** A standalone `-O2` copy of the collect-and-sort loop,
  with no engine and no latch, took 7.5 ms at 131,072 frames and 89.9 ms
  at 786,432. This was not measured in the engine (BE-S1 does that).
- **What that means.** At 786,432 frames, a 600,000-page scan past the cap
  would spend about 15 hours sorting. Every other core's page lookup
  waits on the latch for that time.

### 1.3 Nothing reclaims ahead of demand

- **The watermark loop exists but is not wired.** `MaintainFreeReserve`
  (`:1684`) is EV5's watermark loop: sweep, drain the dirty queue, repeat.
  It has no caller outside tests, and `expeditor.cpp:1945` defers it.
  As written, its sweep asks for the whole deficit in one latch hold.
- **The 50 ms writeback tick** (`expeditor.cpp:1947-1948`) drains the
  dirty-eviction queue and reclaims nothing.
- **The tick cannot help a long statement.** At `cores = 1` a long
  statement holds the reactor (BA's P10, "no yield inside a statement"),
  so the tick does not run during a long scan or a long build. **The
  inline path has to be cheap on its own.**
- **`page.md` says there is no background writer.** §7 (`:166`) states
  there is "no background sweep and no background writer", but the 50 ms
  drain is a background writer.

### 1.4 Who erases a frame, and who walks the table

- **The table is touched in many places.** `frames_` appears 66 times on
  62 lines of `device_page_store.cpp`, plus 6 times in the header, one of
  them code (`:766`).
- **Three erasers, all under the structure latch:**
  - `ReleaseScanSlot` (`:799`, erase at `:828`)
  - `EvictClean` (`:1782`, erase at `:1822`)
  - `EvictColdFramesLocked` (erase at `:2617`)
- **The debug poisoner.** Before the erase, a debug build memsets the
  frame to `0xEF`, MG05's poisoner (`:2610-2616`). ASan catches a stale
  span because the bytes are freed.
- **Four whole-table walks besides the sweep:**
  - `Flush` (`:1727`)
  - `DirtyPageIds` (`:1763`)
  - `DirtyPagesWithRecLsn` (`:1775`)
  - `pinned_frames` (`:2527`)
  They are O(resident), once per checkpoint or flush, not once per miss.
- **What a slot array falsifies.** A returned span points into `*bytes`,
  so it survives a rehash. `page.md` §6 says "`Frame::bytes` is a
  `unique_ptr<Page>` the table never moves", and §9 calls frames
  "separate heap allocations". BE-R1 makes both statements false, so
  both are restated.

### 1.5 The fault path cannot write back

The store's lock-protocol header (`device_page_store.hpp:171-188`) says
two things:

- **No path holds a page latch across a durability wait.**
- **A fault does not reach `WriteBack`.**

One wired in would be sound only in `kSkip`, and only if it skipped every
frame its own core holds. The reason is that a shared try re-enters this
core's own exclusive hold (`page_latch.hpp`'s `Next`), and at
`cores = 1` it takes nothing at all (`device_page_store.cpp:2421`). So a
fault-path drain could copy a page halfway through this core's write. It
would see `dirty_gen` unmoved, because that generation moves at the
fetch, not when the write finishes, and it would clean the frame. That is
a lost update.

Faults and creations also run under the very holds such a drain would
carry into `EnsureDurable`. A split creates its sibling under the
parents' exclusive holds.

`live_pins()` (`device_page_store.hpp:715`) counts the store's pins, not
one core's, so "this core holds nothing" has no test in the tree today.

`WriteBack` itself (`:1397`) is sound from where it is called today,
through `AwaitWalGate` (`:1328`) and the `dirty_gen` check.

### 1.6 The ring is not the cheap way to scan resistance

These findings come from a read-only analysis at `e352eac0`.

- **The executor's read walk.** `RunWalkStep` (`step_vm.cpp:1654-1950`)
  owns the page loop. Each page is one `BtreeVisitLeafPage` or
  `ChainVisitOnePage` call (`:1906-1909`), holding its pin only for the
  call.
- **The executor's only legal suspension point** is the loop's bottom,
  described at `:1872-1879`, where the suspend audit asks
  `live_pins() == 0`.
- **The ring breaks that.** A ring holds its last page's pin and shared
  latch until the next `Fetch` (`page_store.hpp:61-80`), and
  `heap_chain.hpp:262-272` forbids passing a fetcher to a caller that
  suspends between pages.
- **The btree leaf walk has no fetcher seam.**
- **Routing scans through the ring would therefore cost:**
  - a fetcher on three btree signatures;
  - a `ReleaseHeld` on `ScanFetcher`, or a ban on parking in a walk;
  - a shared latch held across row emission and nested joins at
    `cores > 1`;
  - a re-fault on every repeat of one range, which is exactly xrock's
    per-day probes.
- **The cold-insert alternative is mostly plumbed.**
  - `FetchPinned`'s `bump_usage` reaches `InsertFrame(warm = ...)` (`:796`,
    `:656`).
  - The same flag also suppresses the hit's bump (`:729`), so a cold
    fault and a warm hit need two answers where today there is one flag.
  - `Get` and `GetForRead` both hard-code `true` (`page_store.hpp:193`,
    `:204-209`).
- **Only `SELECT` reaches the walk.** `RunWalkStep` is reached only from
  `SELECT`. The outermost-walk predicate
  `index == 0 && parent_ == nullptr` exists (`step_vm.cpp:1950`).
- **Some walks stay warm under BE-R5.** The reverse foreign-key walk
  (`fk_check.cpp:407`) and the `UPDATE`/`DELETE` walks
  (`command_dispatcher.cpp:5556`, `:5661`).
- **No executor-level ring test exists.**

### 1.7 What a refused fault meets

**[quiet-wrong] surface.** Under BE-R4 a fault or a creation can be
refused `ResourceExhausted` where today it grows the pool. Every
`FetchPinned` caller already handles a `StatusOr`, because a device read
can fail. But no test refuses a fetch or a creation *after* the same
mutation has written a page. Three such places:

- an undo write after the leaf write;
- a spill allocation after the row;
- a split's sibling creation, or an index entry after the clustered
  insert.

AT-S16 orders a split's parent holds before any write. Nothing states the
same for creations or the other mutations.

The refusal's scope is `txn.md`'s, not `eviction.md` §3.3's. §3.3 says
"the transaction survives". `txn.md` (`:952-955`) makes failure atomicity
per transaction:
- inside `BEGIN`, a failed statement leaves the session `failed-txn`;
- in autocommit, the abort is automatic.

## 2. Rulings — CLA's proposals, marked as proposed (W1)

### BE-R1 — Slots, a free list, and a hand that walks slots

**Storage.**
- `std::vector<Slot>` reserved to the cap and filled in chunks of a named
  `constexpr` (proposed 1,024 frames, 8 MiB).
- A chunk's page bytes are one allocation that never moves while the store
  lives.
- The page table becomes `PageId → slot index`, and the free list is a
  vector of slot indices.
- `Slot` carries `Frame`'s pin, latch word, usage, `writing`, `dirty_gen`
  and `rec_lsn` unchanged.
- A `queued` bit replaces the dirty queue's `std::find`.

**Reservation.** A miss **reserves its slot before the device read**, and
a creation reserves its slot before it builds the page. The order is:

1. pop a free slot;
2. else add a chunk while fewer than the cap exist;
3. else reclaim a batch (BE-R2);
4. else refuse (BE-R4).

A fault that loses the `loading_` race returns its slot to the free list.
Reserving first is what makes the cap hard under concurrent faults.

**The hand.** A slot index, advancing modulo the slots in use. Every step
is O(1), and each batch is bounded (BE-R2).

**Erasers.** The three erasers push the slot back onto the free list.

**Debug cover.** A freed slot is memset `0xEF` in debug builds, as today.
Under ASan it is also `ASAN_POISON_MEMORY_REGION`'d and unpoisoned on
reuse, which keeps the cover a reused slot would otherwise lose (§1.4).

**Partitioning (BE-Q9).** The shape must survive BA-R5's 128
partitions: a partition can own a slot range with its own free list and
hand, and nothing in BE-R1 assumes one global hand beyond its own code.

### BE-R2 — Bounded batches, inline and in the background

**Inline.** A reservation that finds no free slot, with the array at the
cap, reclaims a batch inline under one structure-latch hold.
- The batch is a named `constexpr` (proposed 64 frames).
- **The batch walks at most a named step bound** (proposed `8 * batch`
  slots).
- It stops at the bound whether or not it filled. A partial batch is
  still progress, and an empty one goes to BE-R4.
- Dirty victims get their `queued` bit and are left in place.

**Background.**
- `MaintainFreeReserve` is wired into the 50 ms tick with
  `low = cap / 16` (the spec's `free_watermark`) and `high = cap / 8`.
- It runs as a loop of bounded batches with the latch released between
  them, never as one hold for the deficit.
- It drains the dirty queue between batches through `WriteBack(kSkip)`,
  which is sound there because the tick holds no page latch (§1.5).
- It runs between statements only (§1.3).

**Counters in `SHOW META`.** EV9's production set, without EV4's
retired per-core split:
- hits and misses;
- reclaims, inline and background;
- inline batches, partial batches and steps;
- queued dirty victims and drained victims;
- refusals.

### BE-R3 — The key is required, and unbounded is deleted

**Refusals.**
- A config file without `buffer_pool_frames` is refused by `ApplyFile`.
- A server started without `--config` is refused by `Expeditor::Open`.
- `0` is refused by both.
- Each refusal names the key and the reason (`InvalidArgument`, BE-Q3).

**The store.**
- `DevicePageStore::Open` takes the capacity as a required argument
  (51 call sites across `src/`, `tests/` and `sim/`; BE-Q2).
- `frame_budget_ = 0`, every "nonzero only" branch, `SetFrameBudget`,
  `CoreRuntime::Config::buffer_pool_frames` and `core_runtime.cpp:191-193`
  are deleted.
- The `Expeditor::Config` sites in tests get the key. `config_file_test.cpp`
  alone has 42 references.

**The share.** `CheckFrameBudget`'s "below `cores`" refusal and
`FrameBudgetShare` are deleted with the share they describe.

**The mount floor (BE-Q6).** A cap below the volume's resident-class pages
at mount, plus a named working minimum, is refused at mount, naming both
numbers.
- Resident-class pages are those below `first_evictable_page_id_` plus the
  Cabin bound pages.
- Map pages never enter the pool, per `IsPinnedClass`'s D4(a) note.
- **The floor is a snapshot.** Bound Cabin pages grow after mount, and one
  that cannot get a slot is refused like any creation (BE-R4).

**The debug override.** `KDS_TEST_FRAME_BUDGET` can only lower the
capacity, and it is clamped up to the floor rather than refused. So
EV10's tiny-pool run is the floor itself.

### BE-R4 — At the cap: retry the sweep, then refuse

When a reservation finds no free slot, the array at the cap, and a batch
that reclaimed nothing:

1. Retry the bounded batch up to a named `constexpr` (proposed 8, the
   spec's `evict_retry_budget`). The background drain may have cleaned
   frames from another core meanwhile.
2. Then refuse `ResourceExhausted`. The message names the cap and how many
   frames were pinned, dirty and resident by class.

The refusal's scope is `txn.md`'s (§1.7). `eviction.md` §3.3's "the
transaction survives" is restated to it.

**Nothing on this path writes back or waits.** This matches EV8's "no
waiting, ever" and the store's rule that no path holds a page latch
across a durability wait (§1.5). §3.3's "yield to the event loop" is not
reachable from synchronous fault code, so the retry replaces it.

**The stated cost.** A single statement whose dirty pages exceed what the
cap can hold is refused rather than slowed. This applies to a
`CREATE INDEX` or an assertion build over a relation larger than the
pool, and to one huge multi-row `INSERT`. The cap is the operator's
answer to how much may be resident, and a statement that needs more is
the "pool undersized" signal EV8 names. An inline drain that would lift
this is BE-Q4 (b).

### BE-R5 — The outermost read walk is cold on a fault

- `PageAccess` gains `kScan`. Both visitors already take a `PageAccess`,
  so no second parameter is needed.
- `kScan` fetches the page shared, inserts a faulted frame with usage 0,
  and bumps usage on a hit as `kRead` does.
- `FetchPinned`'s one `bump_usage` flag splits into its two answers:
  warm on insert, and bump on hit.
- `RunWalkStep` passes `kScan` only for the outermost walk
  (`index == 0 && parent_ == nullptr`, `step_vm.cpp:1950`).
- Nested inner walks, the reverse foreign-key walk, and `UPDATE`/`DELETE`
  walks stay `kRead`/`kWrite` (§1.6).

The effect:
- A page a scan faults once is the hand's first victim.
- A page the scan touches twice, such as a repeated range, warms like any
  other hit and stays.

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BE-S0 | **The order** | <ul><li>This file</li><li>Its review (§6)</li><li>The ruling marked (`raft-marks-2026-10-07.md` §14)</li><li>The index row</li></ul> | S |
| BE-S1 | **The census and the premise**, before anything is built | <ul><li>**Premise.** In `build-release`, time the per-miss sweep inside the engine: a read-only scan over a relation 2× the budget, at budgets 131,072 and 786,432. Report per-miss latency p50/p99, total time and RSS. If the in-engine cost is less than a tenth of §1.2's standalone figure, BE stops at this row and the operator rules.</li><li>**Census A.** Every `frames_` reference, in both the `.cpp` and the header, classified as lookup, insert, erase, walk or field, with its slot form.</li><li>**Census B.** §1.7's sites: every fetch **and every creation** reachable after a page write in the same mutation, each with what an error there does today.</li><li>**The suite under `KDS_TEST_FRAME_BUDGET=64`**, still soft here, listing what a tight budget already breaks.</li></ul> | M |
| BE-S2 | **The slot array** (BE-R1) | <ul><li>**Code.**<ul><li>`Slot`, the chunked array, `PageId → index` and the free list;</li><li>reservation before read and before create;</li><li>the hand over slots and the `queued` bit;</li><li>the erasers to the free list;</li><li>the sort deleted;</li><li>poison on free.</li></ul></li><li>**The budget stays soft in this stage.** With no budget reached, every existing cell passes unchanged.</li><li>**Cells.**<ul><li>A full rotation reclaims without allocating.</li><li>RSS stays within the budget × 8 KiB plus metadata over a scan 4× the budget.</li><li>Span stability across chunk growth.</li><li>A ring slot release returns its slot.</li><li>A lost `loading_` race returns its slot.</li></ul></li><li>**Mutations, each repeated:**<ul><li>the hand reset to 0 per sweep (killed by the rotation cell);</li><li>an eraser that forgets the free list (killed by the RSS cell);</li><li>a chunk that moves bytes (killed by the stability cell).</li></ul></li><li>**The suite green**, also under `KDS_TEST_FRAME_BUDGET=64` and `KDS_TEST_PAGE_LATCH=1`.</li></ul> | L |
| BE-S3 | **Bounded batches and the background** (BE-R2) | <ul><li>**Code.** The inline batch with its step bound, the batched `MaintainFreeReserve` on the tick, and the `SHOW META` counters.</li><li>**Cells.**<ul><li>A batch never walks past its bound over an all-dirty pool.</li><li>The tick lifts the free count from below `low` to `high` across several latch holds.</li><li>Dirty victims are reclaimed on the visit after their drain.</li><li>On the two-core rig, a scan on one core and point writes on the other, with no lost dirty mark.</li></ul></li><li>**The premise cell from BE-S1 re-run.** Per-miss p99 must fall by orders of magnitude, or the stage is not green.</li></ul> | M |
| BE-S4 | **The hard cap** (BE-R3, BE-R4) | <ul><li>**Code.** The required key, `0` refused at both doors, the store's capacity at its 51 sites, the dead peer budget and the share deleted, the mount floor, the clamp on the debug override, and the refusal path for misses and creations.</li><li>**`kds.conf.sample`** carries the key uncommented, with a stated example and its arithmetic.</li><li>**Cells.**<ul><li>Missing key, `0`, and below the floor, each refused with its message.</li><li>Every frame pinned: the fault is refused and the pool never passes the cap.</li><li>A bulk create past the cap is refused, never grown.</li><li>Each Census B site, given a refused fault or creation, leaves a volume the sim's oracle reconciles after recovery.</li><li>In a `BEGIN`, a refusal leaves the session `failed-txn` per `txn.md`.</li></ul></li><li>**Mutations, each repeated:**<ul><li>the cap check dropped (killed by the pinned cell);</li><li>the creation path's reservation skipped (killed by the bulk-create cell);</li><li>a `0` accepted (killed by the refusal cell).</li></ul></li><li>**The suite green**, and under `KDS_TEST_FRAME_BUDGET` at the floor.</li></ul> | L |
| BE-S5 | **Cold scans** (BE-R5) | <ul><li>**Code.** `PageAccess::kScan`, the split of `bump_usage` into its two answers, and the outermost-walk site.</li><li>**Cells.**<ul><li>A hot working set survives a scan 4× the cap, with its hit rate after the scan within a stated bound of before.</li><li>A nested inner walk stays warm.</li><li>A repeated range under the cap hits on its second pass.</li></ul></li><li>**The waystone, index, cabin and inner-build contract suites byte-identical.**</li><li>**The suite green.**</li></ul> | M |
| BE-S6 | **The close** | <ul><li>**The measurement** (§5).</li><li>**Text restated:**<ul><li>`eviction.md` §3.1-§3.3, with §3.3's scope set to `txn.md`'s and its yield replaced;</li><li>`eviction.md` §4 and §6, where the row reads as built and `free_watermark` and `evict_retry_budget` become constants;</li><li>`page.md` §6 and §9 (frame storage), §7 `:166` and §11 `:198`;</li><li>the store's lock-protocol header;</li><li>`CLAUDE.md`'s eviction row flipped.</li></ul></li><li>**`known-gaps.md`:**<ul><li>the first two Eviction entries `e352eac0` added, and the EV8 entry, are deleted;</li><li>the EV6 entry is restated (BE declines the ring) rather than deleted.</li></ul></li></ul> | S |

## 4. Items for the operator

| item | question | kind | CLA's proposal | mark |
|---|---|---|---|---|
| BE-Q0 | **Open BE**, with BE-S0..S6 and BE-R1..R5 as written | process | Yes | **as proposed**, W1 |
| BE-Q1 | **How frames are stored.**<br>(a) A slot array in chunks to the cap, a free list, and a hand over slots (BE-R1; `eviction.md` §3.1-§3.2 as written).<br>(b) Keep the map and add a separate ring of ids for the hand | design | (a). (b) keeps the per-frame allocation and the RSS ratchet, and adds a second structure that has to agree with the map | **as proposed: (a)**, W1 |
| BE-Q2 | **Where unbounded dies.**<br>(a) At the server's doors only; the store keeps `0 = unbounded` for unit tests and the sim.<br>(b) Everywhere: the store's `Open` takes a required capacity at its 51 sites | scope | (b). An unbounded store reachable only from tests is a mode no server test exercises, and a sim choosing a capacity per seed runs eviction on every seed | **as proposed: (b)**, W1 |
| BE-Q3 | **The refusal's code**, for a missing key, `0`, and a cap below the floor | user-visible | `InvalidArgument`: each is "simply wrong" configuration, not an unbuilt feature or an architectural limit (`status.hpp`'s test) | **as proposed**, W1 |
| BE-Q4 | **At the cap with nothing reclaimable.**<br>(a) Retry the bounded sweep, then `ResourceExhausted`; nothing writes back on the fault path (BE-R4).<br>(b) Also drain inline, which needs a per-core "holds no page latch" test that does not exist (§1.5) and **waits on an fsync**, against EV8's "no waiting, ever".<br>(c) Block until a frame frees | user-visible, **[quiet-wrong] if Census B is skipped**; (b) **[quiet-wrong]** if its per-core test is wrong (lost update, §1.5) | (a). (b) can lose a committed update if its test is wrong, and (c) can deadlock a statement against its own pins. The cost of (a) is stated in BE-R4: a single statement dirtying more than the cap is refused | **as proposed: (a)**, W1 |
| BE-Q5 | **No new keys.** The chunk (1,024), the batch (64), the step bound (`8 * batch`), `low` (`cap/16`), `high` (`cap/8`), the retry bound (8) and the working minimum are named `constexpr`; §6's `free_watermark` and `evict_retry_budget` rows become constants | user-visible | Yes. Each is a function of the one quantity the operator sets, so a key for any of them would be a second name for the cap | **as proposed**, W1 |
| BE-Q6 | **The mount floor**: resident-class pages at mount plus a working minimum (proposed 256 frames), refused at mount; the debug override clamped up to it | user-visible | Yes. A cap the volume's own pinned pages exhaust would refuse every statement. Refusing at mount names the number while the operator is still at the config | **as proposed**, W1 |
| BE-Q7 | **Scan resistance.**<br>(a) Cold on fault for the outermost read walk (BE-R5).<br>(b) The scan ring for it (EV6 in full).<br>(c) Neither, in BE | design | (a), for §1.6's reasons. (b) becomes its own order if BE-S6 measures displacement anyway | **as proposed: (a)**, W1 |
| BE-Q8 | **The measurement** (§5), at BE's close, per `CLAUDE.md`'s Session Workflow step 3 | process | As written | **as proposed**, W1 |
| BE-Q9 | **Order against BA and BD.** BE-S2 rebuilds the frame table that BA-R5 partitions (BA-S8) | sequencing | BE first, with BE-R1's per-partition-safe shape. BA-S8 then partitions slots rather than a map, and BA's P1 census would otherwise measure a structure BE replaces. BD touches no store code and runs in parallel | **as proposed**, W1 |
| BE-Q10 | **`eviction.md` §3.3 against `txn.md`** on a refused statement's scope | spec conflict | `txn.md` governs: per-transaction failure atomicity is a transaction rule, and §3.3 restates to it | **as proposed**, W1 |

## 5. Measurement

Once, at BE's close, in `build-release` (`ck-tester`). It compares the
commit BE opened at against the commit that closes it, names both by
`git describe --tags`, and goes in `bench/v3.0.0/`.

**The overhead A/B.** Interleaved, on the series' OLTP shapes, with a cap
larger than the working set so neither side evicts. B must not regress,
because BE-R1 changes every lookup's indirection.

**Past the cap.** A relation 2× the cap, scanned three ways:
- as one statement;
- as per-range probes in the shape of xrock's per-day
  `COUNT(*), MIN, MAX`;
- with those probes repeated.

Report for each:
- scan time;
- per-miss latency from p0 through p99.9;
- reclaims, inline and background;
- RSS sampled through the run, which must stay within `cap × 8 KiB` plus
  the stated metadata.

A is expected to be unusable here (§1.2). That is a prediction, which
BE-S1 replaces with a number.

**Scan resistance.** A hot set's hit rate before and after a scan 4× the
cap, with BE-R5 on (B) and off (BE-S5's parent).

**Bulk load.** Multi-row inserts at `durability = group`, with the cap
below the inserted volume, in xrock's shape of 1,024-row statements.
Report RSS bounded and zero `ResourceExhausted` at a cap above the floor.

## 6. Row status

### BE-S0 — written 2026-10-07, reviewed the same day

Written on `worktree-pool-budget-required` from `e352eac0`. The
`critics-developer` review found two defects, a step bound, and a set of
smaller corrections. All of them are applied; none was rejected.

**The two defects:**
- **The first draft's BE-R4 drained dirty frames inline from the fault
  path.** The store's header forbids that (§1.5): a shared try re-enters
  this core's own exclusive hold, so the drain could copy a page
  mid-write and clean it, which is a lost update. It would also carry
  page latches into `EnsureDurable`. BE-R4 now writes nothing back, and
  the inline drain is BE-Q4 (b) with its risk named.
- **Creations escaped the cap** (`sweep = false`, §1.1). BE-R1 now
  reserves a slot for a creation as for a miss, and BE-S4 has a
  bulk-create cell.

**The step bound.** A batch over a dirty or pinned pool walked up to six
laps, `std::find` made it quadratic, and the background loop asked for the
whole deficit in one hold. BE-R2 now bounds every batch, and the `queued`
bit replaces the `find`.

**BE-R5.** The draft suppressed the hit's bump, so a repeated range never
warmed, which contradicted the order's own reason for declining the ring.
`kScan` now loads cold and bumps on a hit, and replaces the draft's
`ScanHeat` parameter.

**Scope.** "The transaction survives" conflicted with `txn.md`. It is
now BE-Q10, with the refusal's scope taken from `txn.md`.

**The floor.** `KDS_TEST_FRAME_BUDGET=64` could not survive the floor, so
the override is now clamped to it. Map pages were removed from the floor,
since they never enter the pool. Cabin bound pages make the floor a
snapshot, which is now stated.

**BE-S2's two gaps.** Poisoning on reuse, and reserving a slot before the
read.

**Citations corrected:**
- `page.md` §7 and §11, not §3, and §6 and §9 added to the close;
- 66 `frames_` uses plus the header's;
- 51 `Open` sites;
- `Get` also hard-codes the bump;
- the suspension point is described at `:1872-1879`;
- the outermost predicate is at `:1950`;
- the walks that stay warm are named;
- the dead `CoreRuntime` budget is deleted rather than kept as an apply
  site;
- the EV6 gap is restated rather than deleted;
- BA-R5's partitions are named in BE-R1 and BE-Q9.

### BE-S1 — the census and the premise, 2026-10-07

Run on `worktree-pool-budget-required` at `e4b107af`
(`v2.7.0-659-ge4b107af`). No engine file moved. The stage adds
`bench/pool_sweep_bench.cpp` (`kds_pool_sweep_bench`, under
`KDS_BUILD_BENCH`) and one `docs/inflight/bugs/` entry.

**Premise: it holds, so BE goes on.** `kds_pool_sweep_bench` reads the
pages of a `2 × budget` file in order through `GetForRead` on one unarmed
`DevicePageStore`, `build-release`. It samples the first 2,000 misses past
the budget. "Below" means a fault into a pool with room; "past" means a
fault that pays the inline sweep.

| budget | below p50 / p99 | past p0 / p25 / p50 / p90 / p99 / max | the whole half, extrapolated |
|---|---|---|---|
| 16,384 | 5.4 / 10.0 µs | 162 / 225 / 266 / 291 / 332 / 1,237 µs | 4.2 s |
| 65,536 | 5.6 / 10.9 µs | 792 / 890 / 980 / 1,062 / 1,371 / 4,619 µs | 64 s |
| 131,072 | 5.6 / 9.8 µs | 1,960 / 2,156 / 2,222 / 2,353 / 3,119 / 9,241 µs | 294 s |

- At 131,072 frames the engine pays 2.2 ms per miss. That is 30% of §1.2's
  standalone 7.5 ms, against the stop line of 10%.
- The cost grows about linearly with the resident count over this range
  (×3.7 for ×4 frames, ×2.3 for ×2): every miss copies, sorts and re-finds
  the table.
- A miss past the budget costs about 400 times one below it.
- RSS after the 131,072 run was 1,042 MiB, the budget's 1,024 MiB plus the
  process.
- **The 786,432 cell was not run.** It needs about 6 GiB for the pool. The
  host had about 4 GiB available because xrock's `kds_server` (restarted at
  11:48 UTC with `buffer_pool_frames = 1450000`) and its loader held the
  rest. The loader also used about one of the eight CPUs throughout, so
  every figure above was taken next to it. The order of magnitude is what
  the gate asks, and the loader cannot move it.

**Census A: every `frames_` site at `e4b107af`.** The slot form is what
BE-S2 changes it to: the table maps `PageId → Frame*` into chunked slots,
so every lookup gains one pointer hop and keeps its shape.

| kind | sites (`device_page_store.cpp` unless named) | slot form |
|---|---|---|
| insert | `InsertFrame` `:620` (lost race), `:662` (`try_emplace`) | publish a reserved slot; a lost race returns it to the free list |
| erase | `ReleaseScanSlot` `:808/:828`, `EvictClean` `:1794/:1822`, sweep `:2573/:2617` | one tail: unmap, poison, reset, push free |
| walk | `Flush` `:1727`, `DirtyPageIds` `:1763`, `DirtyPagesWithRecLsn` `:1775`, `pinned_frames` `:2529`, the sweep's sorted copy `:2556-2558` | the map for the first four (O(resident), per checkpoint); the sweep walks slots from the hand and the sort is deleted |
| lookup | `ResidentBytes` `:722`, `ClaimNamedIdLocked` `:1021`, `StampPageLsn` `:1254`, `AwaitWalGate` `:1346`, `AwaitWritebackClaim` `:1386`, `WriteBack` `:1492/:1510/:1522`, `FetchAndPin` `:1923/:1987`, `PinResidentAndRelease` `:2069`, `CreatePinned` `:2128/:2140`, `PinFrame` `:2197`, `UnpinFrame` `:2362`, `MarkFrameDirty` `:2438`, the three test hooks `:2443/:2457/:2467`, `IsPinnedClass` `:2498` | `it->second->` for `it->second.`; `WriteBack`'s `Frame*` across its phases stays valid, because a slot never moves |
| field | the sweep trigger `:690/:693`, the sweep's empty test `:2547`, `MaintainFreeReserve` `:1697`, the log line `:2625`, the header's `resident_pages()` `:766` | `frames_.size()` is the resident count, unchanged |

`pinned_frames` walks the table unlatched, a gap that predates BE. It is
read only by cells, and BE-S2 latches it when it touches the line.

**Census B: refusals after a write. This is what BE-S4 has to answer.**
A read-only census found 27 sites where a fetch or a creation follows a
page write in one mutation. Twenty are safe: their error leaves nothing
that rollback or recovery cannot undo. Seven groups are not, and each one
is reachable **today** on a device error, before any cap.
`docs/inflight/bugs/a-fetch-refused-after-a-page-write-leaves-the-mutation-half-done.md`
lists them, worst first:
- an `INSERT` refused between placing its row and writing its undo. This
  leaves an orphan row that becomes visible past the floor, missing some or
  all of its index entries;
- a refused rollback compensation;
- an index split torn after the leaf divides;
- a root re-publish refused after the new root is built;
- the bulk fill's three sites;
- var-heap growth before its undo;
- the assertion commit and abort loops.

Census B also found two things that are not a crash and not a refusal
after a write:
- recovery redo reads any non-`Corruption` fetch error as "page absent"
  on a `PAGE_INIT` or full-image record and re-creates the page; on any
  other record it refuses the mount (`redo.cpp:335-368`);
- `command_dispatcher.cpp:7933`'s comment says the pending set is untouched,
  and that is false.

**What this does to BE-R4, and CLA's proposal (BE-Q11).** BE-Q4 (a)'s
refusal is safe only where no write of the mutation precedes it. Census B
shows that "the fault refuses" reaches seven unsafe groups. So BE-S4 gates
the refusal:
- **A no-refuse window, per row and explicit.** An RAII guard opens at the
  first page write of each Census B group, for example the row's placement.
  It closes when the trail covers that write (`NoteInsert`,
  `NoteOverwrite`), because from there `Abort` can undo it. Outside every
  window, a fault or creation may be refused (BE-R4). Inside one, it may
  not.
- **The window is per task, and no window holds a suspension point.** The
  guard is a thread-local depth, which is sound only because a task cannot
  interleave with another on its reactor without suspending. The suspend
  audit (`exec::InstallSuspendAudit`) records a park inside a window, as it
  records one under a pin. BE-S4 confirms this for each window it opens.
- **The reserve is carved inside the cap**, so the pool never exceeds it.
  Only a fault inside a window draws on it. Ordinary reservations stop at
  `cap - reserve`. The reserve is `kWindowReserveFrames` (proposed 64,
  eight times `kPinCeiling`) multiplied by the instance's core count,
  because every core can be inside a window at once.
- **An exhausted reserve fail-stops the store.** Returning an error into a
  half-done mutation is exactly what the window exists to prevent, so the
  answer is the one `wal.md` gives a write it cannot complete. The cost:
  one row's mutation that needs more than 64 frames that cannot be
  reclaimed, beyond the cap, stops the instance.
- **Recovery is not a mutation.** A mount pass (redo and undo) holds no
  page latch between records, and the log it replays is durable. So a
  reservation on the mount thread that finds nothing reclaimable drains
  the dirty queue through `WriteBack` and retries, and the cap holds at
  mount too.
- **Writers outside a statement need their own census in BE-S4:** access
  statistics and Waystone trails written by reads, the delete-mark purge,
  Cabin and assertion builds, and DDL page creation. Each one is either
  shown to refuse before its first write or given a window.

BE-S1's review replaced the first draft of this rule. That draft keyed the
window to "the thread has dirtied a page since its statement began".
That made the window a whole statement where the danger lasts one row, so
one large `UPDATE` under pressure could fail-stop the instance. It also
used a thread-local flag that coroutines interleave across, and a single
reserve shared by every core.

The alternative, fixing each of the seven groups to undo what it wrote, is
the order of a milestone, and it does not close the device-error half any
better. W1 marked BE-Q0..Q10 as proposed and told CLA to follow its
proposals where a decision is needed. BE-Q11 is taken on that word and
recorded as such, not as a mark.

**The suite at `e4b107af`, Debug, `-j8`:**
- plain: 3191/3192;
- under `KDS_TEST_FRAME_BUDGET=64`: 3191/3192.

The one failure is the same cell both times:
`TcpServerListenTest.ReusePortAdmitsASecondListenerAndItsAbsenceRefusesOne`
binds port 25432, which xrock's running `kds_server` holds (`ss -ltnp`).
That is the environment, not the tree. So a 64-frame budget breaks nothing
**while it is soft**. What a hard cap breaks is BE-S4's to find.

**Not measured; measured at the milestone's close.**

### BE-S2 — the slot array, 2026-10-07

Built at `0da17f9f` on `worktree-pool-budget-required`.
- Frames live in chunks of `kFrameChunk` (1,024) slots. Each chunk's page
  bytes are one `make_unique_for_overwrite` allocation that never moves.
- The table maps `PageId → Frame*`, and a free list hands out slots.
- A miss and a creation reserve their slot before filling it, through a
  `ReservedFrame` guard that returns the slot on every early exit.
- The hand is a slot index that keeps its place between sweeps, and the
  sort is deleted.
- A `queued` bit replaces the `std::find`.
- The three erasers share one tail, `ReleaseFrameLocked`, which poisons the
  slot in debug builds and, under ASan, poisons it until it is reused.
- `Frame` grows from 40 to 48 bytes (`page_id`, `queued`).

**Its review found four defects, fixed in the BE-S3 commit. Three
suggestions were applied and two declined:**
- **A new cell failed under `KDS_TEST_FRAME_BUDGET=64`.**
  `ASpanSurvivesTheArraysGrowth` inherited the override's budget, so the
  array never grew. **`0da17f9f`'s message claims "3191/3192 under a
  64-frame budget" for the suite as it was before the cells existed. For
  the cells it added, the claim is false.** The cell now sets its own
  budget.
- **The budget counted only published frames**, so concurrent fills each
  saw room. The budget now bounds the slots in use, which include every
  reserved fill (BE-S3's `SlotsInUseLocked`), as BE-R1 requires.
- **Creations past an all-dirty pool walked six laps each.** BE-S3 bounds
  them (below).
- **The order's "a lost `loading_` race returns its slot" cell was not
  there.** A loser of the `loading_` race reserves nothing in this design.
  So the cell is the slot census on `ConcurrentMissesOnOnePageIssueOneDeviceRead`,
  where eight faulters per round use one slot. `InsertFrame`'s lost-race
  arm is reachable only by an unlatched race on the raw `*Unpinned`
  accessors, which would be undefined behaviour on the table, so no cell
  drives it. **This is recorded as a gap, not claimed as covered.** A
  census on the failed-fill (`NotFound`) path and on `EvictClean` was added
  instead.
- **Applied:**
  - `InsertFrame` takes the `ReservedFrame&&`, so a throwing `emplace`
    gives the slot back;
  - the two creations share `PublishFreshPage`;
  - the sweep asks `IsPinnedClassFrame(frame)`, so it does no second lookup
    per step;
  - a dead null test was deleted;
  - the stale comments it listed were rewritten. The header's
    `loading_`/`unique_ptr<Page>` text, `FetchAndPin`'s, and the
    "references into an `unordered_map`" lines all described a table that
    is gone.
- **Declined:**
  - **Moving `kFrameChunk`/`frame_slots()` into the first public block** is
    cosmetic.
  - **A FIFO free list** would keep a freed slot poisoned longer. It is
    declined because LIFO reuse keeps the hot slots' cache lines warm, and
    the debug poison already catches a stale read on reuse.

`docs/spec/page.md` §6 and §9 still say frames are separate heap
allocations. BE-S6 restates them, as the order says, and until then they
describe the engine before `0da17f9f`.

### BE-S3 — bounded batches and the background, 2026-10-07

Built on `worktree-pool-budget-required` from `0da17f9f`.

**Code:**
- **Reservation.** Under the budget, `ReserveFrame` takes a slot. At the
  budget it reclaims one bounded batch per structure-latch hold, releasing
  the latch between batches:
  - `ReclaimBatch()` frames, which is 64, or budget / 16 when that is
    smaller;
  - in at most `kBatchStepsPerFrame` (8) steps per frame.
- **`MaintainFreeReserve()`** reads the budget: `low` = budget / 16 and
  `high` = budget / 8. It runs on the 50 ms writeback tick before the
  drain, as bounded batches with a drain between them.
- **`SHOW META`** prints `pool_budget`, `pool_resident`, `pool_slots`,
  hits, misses, inline and background reclaims, inline batches, partial
  batches, steps, queued, drained and refused. The counters are
  `PageStore::pool_counters()`, zeros for a store with no pool.

**A deviation from BE-R2 as written, and why.** BE-R2 says "a partial batch
is still progress, and an empty one goes to BE-R4". That would refuse
falsely. When the pool first fills, every frame is at usage ≥ 1, and no
512-step batch finds a victim until a whole lap has brought the counters
down. Eight empty batches (BE-R4's retry bound) would then refuse a fault
in a pool that is entirely clean and reclaimable. So:
- A batch that lowers a usage counter counts as progress.
- A reservation gives up only after **one whole lap with nothing freed and
  nothing lowered**. That means every frame is pinned, latched,
  resident-class or dirty, and another lap cannot change it.
- The step bound still bounds every latch hold. **Every walk is also
  bounded by `kClockUsageCap + 1` laps in all** (`ReclaimWalk`, shared by
  the reservation and the tick). This was added by the stage's review: with
  the latch dropped between batches, other cores' hits can keep raising
  the counters a walk lowers, so "a batch lowered something" alone need
  never end.
- On one core, a reservation walks about two laps at most. The cell
  `NoBatchWalksPastItsBoundOverAnAllDirtyPool` explains the second lap: a
  creation enters warm, and lowering the previous fill's counter restarts
  the idle lap once.
- Under BE-S3 the give-up still grows the array, because the budget is
  soft. BE-S4 turns it into BE-R4's retries and refusal.

**The batch scales below 1,024 frames.** A fixed 64 would take a quarter of
a 256-frame pool per batch. `budget / 16` is a function of the one
quantity the operator sets, so BE-Q5 holds.

**Cells:**
- `NoBatchWalksPastItsBoundOverAnAllDirtyPool`: steps per batch never pass
  the bound, nothing is reclaimed from an all-dirty pool, and each fill
  walks at most two laps.
- `MaintainFreeReserveRestoresTheWatermarkThroughDirt`, rewritten for the
  no-argument loop:
  - from a free count of 0 with every frame dirty, the loop queues, drains
    and reclaims to `high` in more than one batch (`batches_background`,
    one per latch hold);
  - with every frame pinned, it ends rather than spins.
- `AScanOnOneCoreLosesNoWriteFromAnother`, armed:
  - core 1 scans four times the budget and runs the background loop, while
    core 0 writes each of 256 pages once;
  - every write reads back through a fresh store after a sync.
  - **Its first version wrote a few hot pages every round and survived the
    mutant "reclaim dirty frames".** Those pages never cooled to usage
    zero, so the hand never offered them. In the rewrite each page is
    written once and cools under the scan, so the mutant fails it ("written
    pages lost their write"). It was green 20 times out of 20 unmutated.
- **The premise cell re-run** (`kds_pool_sweep_bench`, `build-release`,
  the whole `2 × 131,072` pass, no sample), next to the same xrock load as
  BE-S1:

| | below p50 / p99 | past p0 / p25 / p50 / p90 / p99 / max | past total |
|---|---|---|---|
| BE-S1, `e4b107af` | 5.6 / 9.8 µs | 1,960 / 2,156 / 2,222 / 2,353 / 3,119 / 9,241 µs | 294 s (extrapolated) |
| BE-S3 | 5.1 / 9.1 µs | 2.1 / 2.4 / 2.5 / 2.8 / 5.2 / 2,891 µs | 0.34 s |

  - Per-miss p99 falls by a factor of 600, almost three orders of
    magnitude, so the done condition holds.
  - A miss past the budget is now cheaper than one below it, because it
    reuses a slot whose memory is already mapped.
  - The 2.9 ms maximum is the first reservation past the budget walking one
    lap of warm frames.
  - RSS after the pass was 1,042 MiB, the same as BE-S1.

**BE-S3's review found two loops that could fail to end, plus one older
defect. All three are fixed:**
- **C1.** A reservation could spin while other cores kept the pool warm.
  It is now bounded by `ReclaimWalk`'s total.
- **C2.** One tick's `MaintainFreeReserve` could hold core 0's reactor
  while peers took freed slots. It now does at most one deficit
  (`high`) of reclaim per call, within the same total bound.
- **C3, from BE-S2.** If a chunk's `push_back` threw, `free_frames_` was
  left pointing into the dead chunk. The chunk is now pushed first.

**Also taken from the review:**
- `SlotCountLocked()` replaces seven spellings of
  `chunks_.size() * kFrameChunk`.
- `batches_background` is counted, so the "across several holds" claim
  has a witness.
- The watermark cell's no-budget line now sets its own budget. Before,
  it passed under `KDS_TEST_FRAME_BUDGET` only by coincidence.
- Three stale comments that named `EvictColdFramesLocked` or placed the
  sweep inside `InsertFrame` were corrected.

**What has no cell, stated rather than implied:**
- **C1's bound.** A two-core cell that warmed every frame while the other
  core faulted survived the mutant "no total bound". The warming thread
  cannot re-touch a lap's worth of frames between the hand's visits, so
  the spin never formed. Forcing it needs a seam between batches, so the
  cell was deleted rather than kept as a pass that proves nothing. The
  bound is argued, not tested.
- **`AScanOnOneCoreLosesNoWriteFromAnother` does not cover a drain under
  the writer's own hold.** Its writer releases before draining.
- **`pool_refused` is printed and stays 0 until BE-S4 writes it.**

**Known costs while the budget is soft:**
- With the budget at or below the resident-class pages, every fill walks
  one idle lap before it grows the array.
- At `cores = 1`, a long write transaction past the budget does the same,
  because the tick cannot run. BE-S4's mount floor removes the first case;
  the second is BE-R4's stated cost.

### BE-S4 — the hard cap, 2026-10-07

Built on `worktrees/pool-budget-required` from `8ef588e0`.

**The key, at both doors (BE-R3).**
- `Expeditor::Config::ApplyFile` refuses a file without
  `buffer_pool_frames`, and refuses a 0.
- `Expeditor::Open` refuses a 0, so a server started with no `--config` is
  refused too, because its `Config` carries the 0 it was built with.
- Each refusal is `InvalidArgument` and names the key (BE-Q3), through one
  `CheckBufferPoolFrames`.
- `CheckFrameBudget`, `FrameBudgetShare`, the peer's share and the
  peer-budget overwrite in `Expeditor::Start` are deleted. The one pool is
  opened at the whole value.
- `kds.conf.sample` carries the key uncommented, with an example
  (131,072 frames = 1 GiB) and its arithmetic.

**The store (BE-R3, BE-Q2 (b)).**
- `DevicePageStore::Open` takes a required `FrameCapacity`, a type of its
  own so that an old `Open(device, first_id)` call fails to compile rather
  than becoming a pool of that many frames. 0 is refused.
- `SetFrameBudget` and `frame_budget()` are deleted. `frame_capacity()`
  reads the capacity.
- The slot array grows in chunks cut at the capacity, so the pool cannot
  pass it structurally.
- All the `Open` call sites carry a capacity: 58 in `src/`, `tests/`,
  `sim/` and `bench/`. The order counted 51 at `e352eac0`.
- **`CoreRuntime::Config::buffer_pool_frames` is re-scoped, not deleted.**
  The order deletes it, but a runtime that opens a store of its own (a
  fixture's) needs a capacity for it. A second key for the same quantity
  is what BE-Q5 forbids, so the field became that store's required
  capacity and is ignored where the pool is shared, which is every
  production core.
- **The sim chooses a capacity per seed:** 512, 1,024 or 8,192 frames.
- **The debug override** `KDS_TEST_FRAME_BUDGET` only lowers the capacity,
  and never below `kWorkingMinimumFrames`.

**The mount floor (BE-Q6).** `DevicePageStore::ApplyMountFloor()` runs after
the completion checkpoint. The floor is:
- every id below the resident limit, faulted or not;
- plus the Bound Cabin pages resident now;
- plus 256.

A configured capacity below the floor is refused `InvalidArgument`, naming
both numbers. A capacity that only the override lowered is raised to the
floor.

**The refusal (BE-R4) and where it may happen (BE-Q11, taken on W1).**
- A reservation's limit is the capacity, less every open window's unspent
  share.
- At the limit it reclaims in bounded walks (BE-S3) and retries
  `kRefuseRetries` (8) times. Then it is refused `ResourceExhausted`,
  naming the capacity and how many frames are resident, pinned and dirty.
  Nothing on that path writes back or waits.
- A creation reserves its slot **before it claims an id**, so a refused
  creation claims nothing.
- **Windows:** `NoRefuseWindow::Open(store, frames)` promises a share of
  the capacity. That is the refusal point, with nothing written. Inside the
  window, fills spend the share, and a fill past the share that the pool
  cannot serve fail-stops the process (`FailStop`).
- **Windows are opened at:**
  - each `INSERT` row, before its spills and placement and through the
    root re-publish;
  - each `UPDATE` row, before its spills;
  - the carved bulk fill, sized to its pages.
- **Drain mode:** `DrainOnPressure` covers mount recovery (`Expeditor::Open`
  and the sim), `TransactionManager::Abort`, and the assertion `CommitTxn`
  and `AbortTxn` loops. On a thread holding no pin, a reservation in drain
  mode writes the dirty queue back and walks again before it would refuse.
- **The suspend audit** records a park inside a window, as it records one
  under a pin.

**Deviations from the order as written, each with its reason:**
- **The windows are BE-Q11's, which the order did not contain.** Census B
  found that BE-Q4 (a)'s refusal reaches seven groups of sites that a
  refusal leaves half done.
- **The reserve is promised when a window opens, not drawn from a
  standing pool.** BE-S1's draft opened windows per row, and a long
  `INSERT` at `cores = 1` could fill the pool with dirty frames and fail-stop
  on the reserve. Promising the share at the open makes that case a
  refusal, at the one point where a refusal is safe.
- **Drain mode extends BE-Q11's recovery arm to rollback and the assertion
  loops.** Each holds no pin between its steps, and a refusal inside any of
  them is permanent damage (Census B #11-#13). BE-Q4 (a)'s "nothing writes
  back on the fault path" still holds for every fill outside drain mode.

**Cells:**
- `ExpeditorConfigTest.BufferPoolFramesIsRequiredAndNonzero`: a missing
  key, 0, and both doors. It kills the mutant "a 0 accepted".
- `ExpeditorTest.APoolBelowTheVolumesFloorIsRefusedAtMountNamingBothNumbers`:
  the floor minus one is refused, naming 383 and 384; the floor mounts; a
  0 is refused.
- `BoundedPoolTest.AFaultIntoAPoolOfPinnedFramesIsRefusedAndThePoolNeverGrows`:
  every frame pinned, the fault refused, the slots exactly the capacity, and
  one released pin lets the fault through. It kills the mutant "the cap
  check dropped".
- `SlotArrayTest.AFullDirtyPoolRefusesTheNextFillAfterBoundedWalks`: the
  bulk create past the cap is refused and never grows the pool, with each
  walk bounded. It kills the mutant "the creation's reservation skipped".
- `BoundedPoolTest.AWindowIsRefusedWhenThePoolCannotPromiseItsShare`.
- `BoundedPoolTest.AnInsertRefusedForAFullPoolLeavesNoRowBehind`, through
  the sim harness: wide rows fill a 512-frame pool inside `BEGIN` until a
  row's window is refused ("buffer pool full ... another mutation's 64
  frames"). After `ROLLBACK` the relation holds exactly its 3 committed
  rows, and again after a crash and a reboot's recovery. **With
  `InsertOneRow`'s window deleted the cell fails**: the count query
  itself errors.

**What has no cell, stated rather than implied:**
- The `UPDATE` and carved-fill windows.
- Drain mode under `Abort` or recovery at the cap.
- `FailStop`.
- The census's writers outside a statement (access statistics, Waystone
  trails, the purge, Cabin and assertion builds, DDL page creation). Each
  of these meets the ordinary refusal before its first write, as Census B
  classified them (SAFE), and no cell drives one into a full pool.
- A rollback compensation refused while its thread holds a pin. Drain mode
  cannot run there, so it would meet the refusal, which is Census B's #11
  damage. No path into `Abort` that holds a pin was found, but nothing
  enforces that either.

**Cells that opt out of the floor.** Under the override, four cells dirty
more pages in one burst than the 256-frame floor holds with no checkpoint
between, which is BE-R4's refusal working as ruled:
- `AllocRaceTest.ConcurrentCreatesNeverHandTwoCallersTheSameId`;
- the two `FreeMapRaceTest` region cells;
- `BtreeRaceTest.TwoCoresPromotingIntoOneParentLeaveEverySeparatorOverItsSubtree`.

Each holds `WithoutFrameBudgetOverride` (`tests/frame_budget_override.hpp`)
with the reason beside it. So do the `SlotArrayTest` cells, whose capacity
is their subject. Every other cell runs at the floor.

**The suite:** 3203/3203 plain, under `KDS_TEST_FRAME_BUDGET=64` (the floor, 256) and armed. **The sim corpus** (`scripts/sim.sh 8`, Debug binary): 266 runs, 0 failures, every seed now under a cap.

**BE-S4's review found one defect and fixed it.**
- **B1.** `TakeSlotLocked` cut each chunk at the current capacity. A
  capacity that the debug override lowered and the mount floor then raised
  left a short chunk that was not the last one (for example 256 then 128).
  `SlotAt`'s `index / kFrameChunk` then read past it. Chunks are now cut at
  the configured capacity, which never moves. No cell catches a read past
  the end without ASan.

**Applied from the review:**
- **B2.** The `UPDATE` window now opens above the assertion reservation.
  That reservation is the row's first write, and the comment that said
  "nothing of this row written" was false.
- **B3.** `Expeditor::Open` asks the floor right after `SetResidentLimit`
  as well as after the mount. A pool below the system pages was refused
  with a pool-full message from inside the mount; it now gets the floor's
  numbers.
- The window's reclaim walk is `InlineBatchLocked`, shared with the
  reservation. Before, the window's copy booked only one counter.
- `FailStop` lost its always-invalid page argument.
- Two debug asserts were added: a second store's window opened on a thread
  that already has one, and a pin released on a thread that never took it.
- The stale wording in the store's header and in a `CoreRuntime` cell was
  rewritten. `eviction.md`'s and `known-gaps.md`'s are BE-S6's.

**Suite after the review:** 3203/3203 plain, at the floor, and armed.
