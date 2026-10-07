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
