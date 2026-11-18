# Work order DE - a page read that does not stop its core: the foreground miss off the reactor, the premise measured before anything is built

Status: **WRITTEN 2026-11-18, not opened.** Written on worktree
`db-fpi-cadence-dc-for-update` from `a4d2fb3b` (`v3.4.12-42-ga4d2fb3b`) on
the operator's word of 2026-11-18 (`raft-marks-2026-11-18.md` §25).
**Exploratory by CLA's proposal, not by the operator's word** (DE-Q0).

No stage is started. DE waits for the operator's word and for the marks on
§4's items. Every `[source-read]` claim below was read at `a4d2fb3b`.
Nothing was measured for this order and no suite was run: it changes no
build input.

**DE builds nothing until DE-S0 has measured its premise**, and one of its
outcomes is that nothing is built (C0, §2). The premise is not that a miss
blocks - the tree says so itself (§1.1) - but that the block costs
sessions that did not miss enough to be worth a second way of reading a
page. No results file prices that (§1.4).

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured.

Related: `docs/spec/page.md` §12 (read path), §13; `docs/spec/eviction.md`
§1, §5 (scan ring); `docs/spec/sched.md` §3 (yielding, suspension safety),
§9 (the thread rule); `docs/spec/wait-events.md` (`io.page_read`,
`io.var_heap_read`, `io.scan_ring_read`);
`instructions/v3.2.0/workorder-cj-wait-events-ash.md` (CJ-Q8);
`instructions/v3.0.0/workorder-be-bounded-pool.md` (BE-R1, BE-R5, BE-Q11);
`instructions/v3.0.0/workorder-bi-checkpoint-off-the-reactor.md` (the
shape of work moved off a reactor: a thread, a ticket, a park);
`instructions/v3.2.0/workorder-cw-optimistic-page-read.md` (the hit path,
which DE does not touch) and `workorder-cz-checkpoint-roll-wait.md` (the
exploratory shape DE borrows); `docs/inflight/known-gaps.md` "Statement
scheduling" and "Carve and checkpoint stalls";
`docs/pending/multicore-scaling-cores4-below-cores2.md`.

## 0. What DE is

`[operator]` *"Write the work order for item 4, asynchronous I/O, as
well."* (given in Korean; recorded in English). Item 4 is from a survey CLA
gave in the same conversation, of what the engine lacks as an OLTP engine;
the operator had first asked for it to be explained in more detail, and
this order is that explanation made checkable.

`[design]` On a thread-per-core engine one core runs all of its sessions
in turn. A statement that misses the pool reads its page with a blocking
call on that core's thread, so every other session on the core waits for
the device, whether or not its own pages are resident. DE asks, in this
order:

1. whether that wait is visible in the latency of sessions that did not
   miss, once the operating system's page cache is not answering the read
   (DE-S0);
2. what the most a non-blocking miss could return is - the same sessions
   with a pool that holds the whole relation (DE-S0, cell (v));
3. only then, which statements can give their core back at a miss, and by
   what mechanism (§2, DE-S1 on).

DE is about **reads on the statement path**. The checkpoint's writes on
the reactor are a different site with their own orders (§1.3).

## 1. The tree as read

### 1.1 The miss `[source-read]`

- **A miss is a blocking `pread` on the reactor thread.**
  `DevicePageStore::FaultIn` reserves a frame
  (`src/storage/device_page_store.cpp:1105`) and calls `device_.ReadPage`
  into it (`:1117`). `page.md` states it: "Miss path: a synchronous `pread`
  ... There is no prefetch, no read coalescing and no asynchronous
  submission" (`docs/spec/page.md:178-179`).
- **The scheduler's own rule forbids it.** "Blocking syscalls inside tasks
  are forbidden; all waiting is expressed as suspension"
  (`docs/spec/sched.md:42`); "blocking is not an available primitive"
  (`docs/spec/eviction.md:29`). The miss is the standing exception.
- **The tree records the cost in words.** CJ-Q8: "A page fault is a
  blocking `pread` on the reactor thread: the core stops, no task is
  parked, and the whole core waits"
  (`instructions/v3.2.0/workorder-cj-wait-events-ash.md:321-331`). The
  read is inside a `WaitScope` that never suspends
  (`device_page_store.cpp:1110-1116`), and the event's dictionary entry
  says "the whole core stalls for its duration"
  (`docs/spec/wait-events.md:73`).
- **A second core that wants the page being read blocks its thread too.**
  The structure latch is not held across the read (`:2468-2473`), so a
  miss does not stop another core's hit. A fetch of the *same* page finds
  the id in `loading_` and waits on a condition variable (`:2505`). Above
  one core only: unarmed (`cores = 1`) the fetch calls `Resolve` with no
  latch and no `loading_` set (`:2450-2461`), so the one-loader protocol
  does not exist there.
- **The device is read through the operating system's cache.** The data
  file is opened without `O_DIRECT`
  (`include/kds/storage/file_page_device.hpp:36-37`, `docs/spec/page.md:161`),
  so a miss of the pool is often a copy from memory, and the page is then
  held twice. `include/kds/sched/io_backend.hpp:13-14` calls "a real disk
  I/O backend (O_DIRECT vs io_uring)" a "still-open decision".

### 1.2 Who can give the core back today `[source-read]`

- A `SELECT`'s outermost walk yields every 64 pages (16 for an `analytic`
  session), at the bottom of its page loop, holding no page; a relation-`X`
  build and an online `CREATE INDEX` do the same
  (`docs/spec/sched.md:43`).
- "A write, a tuple key descent, a key set's descents ..., a join's inner
  walk under one outer page and an index walk do not yield"
  (`sched.md:43`; `docs/inflight/known-gaps.md:865-877`).
- **A coroutine may not be parked holding a page span, a page pin of its
  thread, or an open no-refuse window**, and a debug build asserts it at
  every suspension (`sched.md:45`). A miss happens in the middle of a
  descent, where the caller holds its parent's pin.
- **A mutation may not be refused after its first page write** (BE-Q11,
  `docs/spec/eviction.md:147-150`), so it cannot be unwound, and the
  suspend audit refuses a park inside its open no-refuse window
  (`sched.md:45`).

### 1.3 What already has the shape `[source-read]`

| piece | where | what it gives DE |
|---|---|---|
| A thread that does one kind of device call, a ticket, and a caller that parks on it | `storage::DataSyncer` (`include/kds/storage/data_syncer.hpp:13-48`); the WAL writer, which kicks parked committers (`docs/spec/sched.md:117`) | the mechanism M1 of §2, already allowed by the thread rule ("One thread at startup, which `sched.md` invariant 1 allows", `data_syncer.hpp:59`) |
| A statement head run again from the top after a yield | BA-S14's structural re-run (`src/server/command_dispatcher.cpp:870-892`, `StructuralRerunDue` at `:997-1017`): the outcome is `TxnConflict` with the thread's structural note set (four `btree.cpp` sites; `insert_placement.hpp:70-85`), and `EndWrite` kept the note only for a statement that may park, failed, and left its transaction's trail at the statement's mark (`:12272-12283`); no row has streamed (CE-R4, `:1003-1007`); inside the fault-net deadline (`:1008-1009`). It covers `DispatchAndStage` only: a sliced walk runs after the loop (`:642`) and is not re-run | the control flow C2 of §2 needs |
| One loader per page | `loading_` (`device_page_store.cpp:2509`) | the place a second asker joins a read in flight |
| The slot reserved before the read | BE-R1 (`device_page_store.cpp:1105`) | a read in flight already counts against the pool |
| A walk that stops between pages holding nothing | `server::SelectRun` (`sched.md:43`) | the place a walk can ask for its next pages |
| A device seam with fault injection under the simulator | `MemoryPageDevice` (`include/kds/storage/memory_page_device.hpp:65`, `:132`) | where a deterministic completion order has to live |

**Writes on the reactor are not DE's.** A checkpoint step's and a publish
batch's `pwrite`s stay on the reactor, "single writes up to about 300 ms
in the kernel's writeback throttle" (`known-gaps.md:1207-1214`); BI moved
the syncs and CZ is written for the latch wait. `io.page_read` and its two
siblings are always counted (`wait-events.md:65`, `:78`, `:91`), so DE-S0
needs no new instrumentation.

### 1.4 What is not measured

CLA found no results file that runs a working set larger than the pool
with the page cache kept from answering. The standard cells run resident.
The nearest are BE's close, whose §2 ran relations 2x and 4x the pool
through a warm page cache
(`bench/v3.0.0/results-be-close-v2.7.0-665-geef442cf.md:94`), CC's prior
measurement, which says "cold" there "means a cold engine pool, not a cold
device" (`bench/v3.2.0/results-cc-close-prior-v3.2.0-186-g1a9168d3.md:34`),
and `bench/pool_sweep_bench.cpp`, whose reads "mostly hit the kernel's
cache".
`io.page_read` appears in the views captured by one close measurement's
archive (`bench/v3.4.9/archive/audit-g16-close-v3.4.9-5-g83375075/`), not
as a cell of its own. DE-S0 repeats this search before it measures.

The host this order was written on has 8 cores, 31 GiB of memory and an
ext4 file system on a cloud persistent disk presented as NVMe
(`/sys/block/nvme0n1/device/model` reads `nvme_card-pd`; BR's close
measured it writing 165-173 MB/s fsynced,
`bench/v3.0.0/results-br-close-backup-restore-v2.7.0-913-ga8110b05.md:8`),
so a device read here is not a local NVMe read; `sudo` asks for a password
(`instructions/v3.0.0/br-s0-census.md:23`), so the page cache cannot be
dropped globally and DE-Q1 names another way.

## 2. The candidates `[design]`

Priced by DE-S0; none is chosen here.

**What a statement does at a miss:**

- **C0 - nothing.** The verdict if the stall does not reach sessions that
  did not miss (DE-Q1), or if the operator rules that a working set beyond
  the pool is not a case the engine serves (DE-Q6).
- **C1 - read ahead for the yielding walk.** At a slice boundary the walk
  hands on the leaf it will reach next and goes on; it never parks. It
  knows one leaf only, its current leaf's sibling
  (`src/exec/step_vm.cpp:2172-2182`); more needs a descent from a parent
  each slice. While the file is read through the operating system's cache
  the kernel's own read-ahead already serves leaves that are contiguous in
  the file, so C1's return is what is left after that.
- **C2 - miss, park, run again.** A statement head that meets a page that
  is not resident unwinds with a refusal that names the page. Its
  dispatcher, holding nothing, asks for the page, parks on the request,
  and runs the head again from the top - BA-S14's control flow with a
  different cause, under BA-S14's conditions (§1.3). What C2 owes:
  - **The last attempt reads synchronously.** BA-S14 answers its refusal
    at the deadline. Under a pool near its floor each run can evict what
    the last one loaded, and a statement that succeeds slowly today must
    not become one that is refused. `[quiet-wrong]`
  - **No caller may swallow the refusal.** Some callers turn a failed
    fetch into another plan - `step_vm.cpp:2114-2122` turns a failed
    descent into a full walk - and one that read "not resident" as
    "absent" would answer wrongly. DE-S0 lists every caller that recovers
    from a fetch error. `[quiet-wrong]`
  - **What a run did before its miss is done again or is harmless**: a
    lock ask already granted, a statistic already counted, a view already
    minted. DE-S0 reads each against BA-S14's own answer.
- **C3 - suspend in place.** The descent becomes a coroutine that drops
  its pins at a miss and re-validates. **Not available as the rules
  stand**: a coroutine frame is "never the per-tuple path"
  (`docs/spec/sched.md:47`) and a statement's head is synchronous by
  contract (`src/server/command_dispatcher.cpp:842-844`). Listed so that
  it is rejected by name.

A mutation past its first page write is outside all of them (§1.2), and
so is every caller that is not a statement's coroutine - recovery, a
rollback, the 50 ms tick. They read synchronously as today.

**How the page is read off the reactor** (orthogonal to the above):

- **M1 - a read thread.** A queue, a fixed number of threads started at
  boot, `pread` into the reserved frame, a kick to the asker's core:
  `DataSyncer`'s thread and ticket with the WAL writer's kick
  (`DataSyncer` itself kicks no one, `data_syncer.hpp:71-73`). What M1
  owes, none of it in the tree today:
  - **The thread reads and does nothing else.** No thread but a reactor
    touches the frame table or a slot's bytes today; the backup's copier
    reads the device only (`src/server/backup.cpp:183`). The frame is
    published on the asker's core, after the completion, with the
    checksum verified there. `[quiet-wrong]`
  - **The frame outlives a cancelled asker until the device call
    returns**, and is given back on a reactor. The reservation and the
    loading guard are stack objects tied to one call today
    (`device_page_store.cpp:1105`, `:49-78`).
  - **A synchronous caller never waits for a publish a reactor owes.** A
    recovery step or a tick that finds the page in `loading_` waits on a
    condition variable (`:2505`); if the publish is owed to its own
    reactor, which it is blocking, it never comes. At `cores = 1` the
    frame table has no latch and no `loading_` at all (`:2450-2461`). The
    design states who publishes in each case before any code.
  - **Its lock is enumerated** in `sched.md` §9's invariant 2 and
    `rules.md` §3.
- **M2 - a completion queue** (`io_uring`) drained in the reactor's I/O
  phase. `io_backend.hpp` reserves no seam for it: it is the socket
  readiness interface and says a disk backend "is not this" (`:8-14`).
- `O_DIRECT` is neither. It decides whether the operating system also
  caches the page, and is DE-Q4.

## 3. Rules

Binding once DE opens, whatever candidate is marked.

- **DE-R1 - the answer does not change.** Every statement returns what it
  returns today, and refuses only what it refuses today, with the
  off-reactor read on or off, at every core count. `[quiet-wrong]`
- **DE-R2 - a statement that has written a page or sent a row is never run
  again** (CE-R4 kept). `[quiet-wrong]`
- **DE-R3 - nothing parks holding a page span, a pin or an open no-refuse
  window.** `sched.md` §3's audit stays armed and gains no exemption.
- **DE-R4 - a hit pays nothing new**: no word read or written on the hit
  path that is not there today.
- **DE-R5 - the simulator stays single-threaded and seeded.** A read's
  completion order is the seed's; no real thread runs under the sim.
- **DE-R6 - a read's failure reaches the statement that asked, as the
  status a synchronous read gives**: an I/O error, `Corruption` from the
  checksum, `NotFound` for a page never written, `ResourceExhausted` from
  the reservation.
- **DE-R7 - every wait is bounded, and ends in a synchronous read, not a
  refusal.** A parked statement is under its deadline and the cancel
  flag.
- **DE-R8 - no thread is created after startup** (`sched.md` §9's
  invariant 1), and what is added states its lock protocol at the top of
  its file.
- **DE-R9 - the event keeps its name.** `io.page_read` becomes a
  suspension where the read is off the reactor and stays a synchronous
  scope where it is not, and the dictionary says which.

## 4. Items for the operator (DE-Q), CLA's proposals

None is marked. (a) is CLA's proposal in each.

| | item | (a) | other |
|---|---|---|---|
| DE-Q0 | Whether DE is exploratory. The operator's word asks for the order and does not say. | exploratory: DE-S0 measures, and DE may close having built nothing | (b) a build order: DE-S0 is a census only, and DE-S1 follows it |
| DE-Q1 | The verdict, on DE-S0's cells, and the bar at the close. | **C0** if the never-miss sessions' p99 in (iii) is inside the spread of (v) - the stall does not reach them. Otherwise the excess of (iii) over (ii), the fraction of the core's time inside `io.page_read`, and the device's own read time are reported and **the operator rules** build, narrow or nothing: CLA proposes no factor, because the p99 ratio follows the session mix. At the close: the resident cells no slower outside their spread (DE-R4), and the same three numbers after | (b) a factor fixed in advance, named by the operator |
| DE-Q2 | How the page cache is kept from answering, with no root. | a memory-limited scope for the server (`systemd-run --user --scope -p MemoryMax=...`; the review found `memory` delegated to the user's service manager), confirmed in DE-S0's first minutes by `/proc/<pid>/io`'s `read_bytes`; if it is refused, `posix_fadvise(POSIX_FADV_DONTNEED)` on the data file on a short period, on read-only cells after a checkpoint | (b) the `fadvise` loop only; (c) a data file larger than memory - outside the stage budget |
| DE-Q3 | The candidate, marked after DE-S0. | C2 for a statement's head; C1 only if DE-S0's scan cell shows a stall the kernel's read-ahead leaves | (b) C1 alone; (c) C2 alone |
| DE-Q4 | `O_DIRECT`. | not in DE. With it every miss reaches the device, so it is worth asking only after a miss no longer stops a core; cells (iii) and (iv) show what it would expose | (b) decided with DE - an alignment rule for every frame and scratch buffer, and the write path's too |
| DE-Q5 | The mechanism. `io_backend.hpp` leaves it open. | M1, the number of threads fixed and not a setting; the simulator replaces it with a queue | (b) M2, `io_uring`, a new dependency on a kernel interface; (c) M1 with a boot key for the thread count |
| DE-Q6 | The served case. `buffer_pool_frames` is required and is the pool's hard ceiling, sized by the operator. | a working set beyond the pool is served, and a cold start is too: the engine does not get to assume its operator sized the pool right | (b) not served - a miss is a sizing fault the wait event already reports - and DE closes at C0 without measuring |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DE-S0 - census and premise** | No engine code. **Read**: every caller that can reach `FaultIn` and what it holds there, sorted into a statement head BA-S14 could run again, a walk between pages, a mutation past its first write, recovery, a background task; every caller that recovers from a fetch error (`[quiet-wrong]`, §2); what a head has done before its first fetch; what `MemoryPageDevice` needs for DE-R5. **Measure**, `build-release`, by `tools/bl_mixed.py` extended, an open-loop driver, sessions pinned so that placement does not move them (BL-S3), a pool a quarter of the relation unless said: (i) sessions reading keys that stay resident, alone; (ii) the same beside sessions reading keys uniformly over the relation, page cache as the host leaves it; (iii) as (ii) under DE-Q2; (iv) as (iii) with the cold sessions scanning; (v) as (ii) with a pool that holds the relation - the ceiling. `cores = 1`, and (iii) once at `cores = 4` with the cold sessions on another core than the never-miss ones. Beside them, the device's own time for an 8 KiB read at queue depth 1 with `O_DIRECT`, and the server's `read_bytes`. Read off the never-miss sessions' p0/p25/p50/p95/p99, `sys.wait_stats` and `sys.wait_histogram` for `io.page_read`, and `DB time`. **The 20-minute budget holds only with repeats cut first**; the results file says what was cut. | the lists, the results file, DE-Q1's verdict or its three numbers, and DE-Q3..Q5 back to the operator |
| **DE-S1 - the read off the reactor, and the statement head** | Under the marks: the mechanism behind the `PageDevice` seam and its simulator twin; §2's publish and ownership rules written down first; the refusal that names a page; the dispatcher's park and re-run for the heads DE-S0 listed, the last attempt synchronous; the second asker. | review, the full suite |
| **DE-S2 - the walk** | C1, if DE-Q3 marks it. | review, the full suite |
| **DE-S3 - the close** | DE-S0's cells before against after, interleaved; the documents below; the milestone row. | closed, or closed narrow |

**Tests DE owes.** Each statement kind DE-S1 covers answers the same rows
with the pool at its floor and at its ceiling (DE-R1); a statement at the
pool's floor whose every run evicts the last run's page still answers
(DE-R7); a caller that falls back on a fetch error does not fall back on
"not resident"; a statement that has written, and one that has streamed a
row, meets a miss and is not run again (DE-R2); the suspend audit fires on
a planted park under a pin (DE-R3); two cores asking for one absent page
issue one read, and both answer; a synchronous caller meeting a read in
flight for a parked asker on its own core completes; a read that fails,
one that fails its checksum and one that finds the pool full each reach
their statement as today's status (DE-R6); a cancel and a deadline end a
parked statement and its frame is returned after the device call; a
session that closes while its read is in flight leaks no frame; the
simulation corpus with the read path on, crashes included, and the same
seed giving the same trace twice (DE-R5); a two-core rig under `tsan`;
`io.page_read` counted once a read on both paths (DE-R9).

**Documents DE-S3 owes**, each read at the close and amended where it
states the behaviour: `page.md` §12 (`:178-179`); `sched.md` §3 (`:42`,
`:43`) and §9 (invariants 1 and 2); `rules.md` §3; `eviction.md` §1
(`:29`); `wait-events.md` (`:73`, and the two sibling events);
`workorder-cj-wait-events-ash.md`'s CJ-Q8, by a note;
`include/kds/sched/io_backend.hpp:13-14`; `known-gaps.md` "Statement
scheduling" (`:865`); `docs/pending/multicore-scaling-cores4-below-cores2.md`
if a cell bears on it; `manual/server/server.md`; `CLAUDE.md`'s rows for
buffer-pool eviction and cross-core execution.

## 6. What DE does not do

- No asynchronous write and no change to the WAL's I/O, the checkpoint's
  writes or the data file's sync: DE is the statement path's reads.
- No `O_DIRECT`, under DE-Q4 (a).
- Nothing to the hit path: that is CW's question.
- No read-ahead beyond the leaf a yielding walk is about to reach - no
  access-pattern detection, no prefetch for a descent.
- No change to the pool's size rule, its eviction, the scan ring, or the
  `ResourceExhausted` refusal.
- No yield added to a write, an index walk or a join's inner walk for its
  own sake: a statement gives its core back at a miss, or as it does today.
- No claim about multi-core scaling. The pending entry's tail may or may
  not be this; DE-S0 has one cell that can say, and no more.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-18

Written on worktree `db-fpi-cadence-dc-for-update` from `a4d2fb3b`
(`v3.4.12-42-ga4d2fb3b`), documents only. The letter DE is CLA's proposal.
No suite was executed: the commit changes no build input. Nothing was
measured.

**Reviewed by `critics-developer` before the commit.** It opened every
cited line; the citations stood but for five it corrected in place: the
second-asker protocol exists above one core only; BE-Q11 forbids a refusal
after the first page write, not a wait; BA-S14's condition written out as
the code has it, with "wrote nothing" meaning the transaction's trail did
not grow; §1.4 given the three nearest existing measurements and the host's
disk named as a cloud persistent disk, not local NVMe; `DataSyncer` kicks
no one and `io_backend.hpp` reserves no disk seam. Applied from its
findings: C2's last attempt reads synchronously and never turns a slow
success into a refusal; the census of callers that recover from a fetch
error, `[quiet-wrong]`; M1's publish, frame-ownership and lock rules; the
deadlock of a synchronous caller waiting on its own reactor's publish; C3
rejected by name against the two rules that forbid it; C1 cut to the one
leaf a walk knows, with the kernel's read-ahead stated; the ceiling cell
(v), the device probe and the placement pin added to DE-S0; the verdict's
factor of two removed in favour of the operator's ruling on three numbers;
a memory-limited scope proposed ahead of the `fadvise` loop; whether DE is
exploratory made an item (DE-Q0), CLA having decided it unasked; the bar
folded into the verdict item; the item on the reactor's writes dropped,
§1.3 and §6 already excluding them. **Rejected**: merging §0's three
questions into its paragraph - the order of the questions is the order of
the stages, as in CZ.

**A side finding of the review, not DE's**: `BACKUP COPY` and `BACKUP
STOP` create threads after startup (`src/server/backup.cpp:166`, `:340`),
against `sched.md` §9's invariant 1; entered in `known-gaps.md` under
"Backup and restore (BR)" in the same commit.
