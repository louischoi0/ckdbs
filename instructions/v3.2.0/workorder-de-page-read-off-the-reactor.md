# Work order DE - a page read that does not stop its core: the foreground miss off the reactor, the premise measured before anything is built

Status: **OPENED 2026-11-19 on the operator's standing go-ahead
(go-ahead-achieving-milestone), on worktree `de-page-read-off-the-reactor`
from `35086008` (`v3.4.13-19-g35086008`); DE-S0, DE-S1 and DE-S2 done.** Written 2026-11-18
on worktree `db-fpi-cadence-dc-for-update` from `a4d2fb3b`
(`v3.4.12-42-ga4d2fb3b`) on the operator's word of 2026-11-18
(`raft-marks-2026-11-18.md` §25). **Exploratory by CLA's proposal, not by
the operator's word** (DE-Q0). **No DE-Q item is the operator's own mark**:
DE-Q0..Q6 are each adopted (a), as CLA proposed them, on the operator's
standing go-ahead of 2026-11-19 (`raft-marks-2026-11-19.md` §8), and so is
every item a stage raises (§4, DE-Q7 on).

**DE built nothing until DE-S0 had measured its premise**, and one of its
outcomes was that nothing is built (C0, §2). The premise is not that a miss
blocks - the tree says so itself (§1.1) - but that the block costs
sessions that did not miss enough to be worth a second way of reading a
page. **DE-S0 measured it and it holds** (§7,
`bench/v3.4.13/results-de-s0-premise-v3.4.13-19-g35086008.md`): with the
page cache kept from answering, sessions that never miss wait behind the
ones that do, and beside one scanning session their schedule falls seconds
behind. DE-Q1 (a) leaves the ruling on those numbers to the operator; under
the standing go-ahead it is DE-Q7, adopted (a): **build, narrow** - a read
thread, the yielding walk parked at a miss, a read statement's head run
again; a write reads synchronously as today.

Every `[source-read]` claim in §1 and §2 was read at `a4d2fb3b`; §7's
DE-S0 entry re-reads the ones DE builds on at `35086008`.

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

**None is the operator's own mark.** (a) is CLA's proposal in each, and each
is **adopted (a) on the operator's standing go-ahead of 2026-11-19
(go-ahead-achieving-milestone)**, recorded in `raft-marks-2026-11-19.md` §8:
DE-Q0..Q6 at the opening, DE-Q7..Q14 as DE-S0 and its review raised them,
DE-Q15 as DE-S1's review did (§7), each written
here with its options before it was adopted. An option not taken stays open
to the operator's own mark.

| | item | (a) | other |
|---|---|---|---|
| DE-Q0 | Whether DE is exploratory. The operator's word asks for the order and does not say. | exploratory: DE-S0 measures, and DE may close having built nothing | (b) a build order: DE-S0 is a census only, and DE-S1 follows it |
| DE-Q1 | The verdict, on DE-S0's cells, and the bar at the close. | **C0** if the never-miss sessions' p99 in (iii) is inside the spread of (v) - the stall does not reach them. Otherwise the excess of (iii) over (ii), the fraction of the core's time inside `io.page_read`, and the device's own read time are reported and **the operator rules** build, narrow or nothing: CLA proposes no factor, because the p99 ratio follows the session mix. At the close: the resident cells no slower outside their spread (DE-R4), and the same three numbers after | (b) a factor fixed in advance, named by the operator |
| DE-Q2 | How the page cache is kept from answering, with no root. | a memory-limited scope for the server (`systemd-run --user --scope -p MemoryMax=...`; the review found `memory` delegated to the user's service manager), confirmed in DE-S0's first minutes by `/proc/<pid>/io`'s `read_bytes`; if it is refused, `posix_fadvise(POSIX_FADV_DONTNEED)` on the data file on a short period, on read-only cells after a checkpoint | (b) the `fadvise` loop only; (c) a data file larger than memory - outside the stage budget |
| DE-Q3 | The candidate, marked after DE-S0. | C2 for a statement's head; C1 only if DE-S0's scan cell shows a stall the kernel's read-ahead leaves | (b) C1 alone; (c) C2 alone |
| DE-Q4 | `O_DIRECT`. | not in DE. With it every miss reaches the device, so it is worth asking only after a miss no longer stops a core; cells (iii) and (iv) show what it would expose | (b) decided with DE - an alignment rule for every frame and scratch buffer, and the write path's too |
| DE-Q5 | The mechanism. `io_backend.hpp` leaves it open. | M1, the number of threads fixed and not a setting; the simulator replaces it with a queue | (b) M2, `io_uring`, a new dependency on a kernel interface; (c) M1 with a boot key for the thread count |
| DE-Q6 | The served case. `buffer_pool_frames` is required and is the pool's hard ceiling, sized by the operator. | a working set beyond the pool is served, and a cold start is too: the engine does not get to assume its operator sized the pool right | (b) not served - a miss is a sizing fault the wait event already reports - and DE closes at C0 without measuring |
| DE-Q7 | **The ruling DE-Q1 (a) leaves to the operator, on DE-S0's numbers** (§7): the never-miss sessions' p99 in (iii) is outside (v)'s spread, so C0 is not the verdict. | **build, narrow**: the read thread; the yielding walk parks at a miss (DE-Q8); a **read** statement's head - a `SELECT` without a locking clause, run inline - is run again after its page is read (C2). A write statement, a locking read and a DDL read synchronously as today. The reasons are the census's: an `INSERT` opens its no-refuse window before its clustered descent and an issued key's first page write (the `sys.tables` mark) comes before it, so it has no re-runnable read phase that one refusing point could cover; an `UPDATE`'s and a `DELETE`'s locate falls back to a walk on any failed descent and a named-key `INSERT` reads a failed undo read as "decided" (§7, list B) - on a write path a swallowed refusal can reach a page write before the dispatcher sees it, and on a read path it cannot | (b) build as §2 wrote it, a write's read phase included, each swallowing site on the write path changed to test the code first; (c) the walk alone - (iv)'s seconds, without (iii)'s factor of two to three; (d) nothing |
| DE-Q8 | **What the walk does at a miss.** §2's C1 hands on one leaf and never parks. DE-S0's cell (iv): the walk reads about 19,700 pages a second - the disk's sequential rate - and the core is inside the read 93 % of the time; reading one leaf ahead does not give the core back. | **the walk parks at a read that would wait**: its own next-leaf fetch, made at the top of its page loop where it holds no pin and no span (`src/exec/step_vm.cpp:2158-2163`), refuses a page the device cannot hand over at once (DE-Q9); the walk hands the page to the read thread, parks, and makes that fetch again. **The refusal is a property of that one fetch - an access kind of its own - never of the thread**, since the walk yields; and the retry re-issues the fetch alone, without passing the walk's cycle guard or its fetch count a second time (`step_vm.cpp:2159-2162`), so a refused leaf is visited once. Fetches under a leaf - an undo chain, a spilled value, a join's inner step - read synchronously as today. This is §6's own rule, "a statement gives its core back at a miss", applied to the one place a walk holds nothing | (b) C1 as written; (c) nothing for the walk |
| DE-Q9 `[quiet-wrong]` | **How a page read off the reactor reaches the pool, and which read leaves the reactor.** §2's M1 has the thread read into the reserved frame and the asker's core publish it. | **Revised at DE-S0's review, before anything was built.** (1) **A fetch that may be refused first asks the device for the page without waiting** (`preadv2` with `RWF_NOWAIT` on the file device). A page the operating system holds is copied into the reserved frame where the statement stands, exactly as today: cell (ii)'s 4 us copy and the 92 % of cell (iv)'s reads that return in under 4 us never leave the reactor and are never refused. Only a read that would wait is refused - and on this host's kernel the refused call has already started the device read (read at `35086008`'s build: 200 of 200 uncached pages answered `EAGAIN` and were readable 468 us later with no other reader). (2) **The thread then reads the page into a buffer of its own and drops it.** What the read leaves is the page in the operating system's cache, and the statement's next fetch finds it there by (1) - one reservation, one checksum check, one `InsertFrame`, on a reactor. No second image of a page exists outside the pool's protocol, no frame is owned across a cancelled asker, and nobody waits for a publish a reactor owes, at any core count. A device that cannot answer without waiting - any but the file device - is read as today. It rests on DE-Q4 (a): with `O_DIRECT` nothing is left behind. The threads are joined before the device's descriptor closes | (b) the thread's buffer adopted into a frame on the asker's core - saves the second copy, and owes a guard on every path that writes back, frees or re-creates the page while the buffer is in flight: a buffer read before a write-back and adopted after the eviction is a stale image `[quiet-wrong]`; it is what `O_DIRECT` would need; (c) §2's M1 as written, the read into the reserved frame; (d) the first draft of (a): every pool miss refused - a run-again would meet the same miss, and a page the operating system holds would pay a park for a 4 us copy |
| DE-Q10 `[quiet-wrong]` | **The refusal's form, and what stops a caller swallowing it.** DE-S0's passes listed some thirty statement-path sites that recover from any failed fetch (§7, list B). | **a status code of its own, `kNotResident`, that never reaches a client, and a per-thread note set where the refusal is made.** The dispatcher reads the note, not the outcome: a head whose note is set is discarded whole and run again, whatever it answered. So a caller that read the refusal as "absent", or took another plan, produced an answer nobody sees. Sound only under four conditions, each built in DE-S1: **(i) the arm covers a top-level `SELECT`'s body and nothing else** - not a nested `SELECT` (`RunToEnd`, an `INSERT ... SELECT`, a write's scalar subquery), not a locking read; **(ii) the tails obey the note** - `SelectRun::Finish` records nothing (no trail, no access statistics, no optimizer signal) for a run whose note is set, and the arm is dropped before the recording's own fetches (§7, list C); **(iii) the arm is dropped when the sink first hands a row on**, so no refusal is made once a re-run is impossible (CE-R4); **(iv) the note is read before the head's coroutine can yield**, as BA-S14's is. A specific-code test anywhere in the tree does not match the new code | (b) every swallowing site changed to test the code; (c) `TxnConflict` under BA-S14's structural note - seven retry loops test that code; (d) the arm on the two descents' node fetches only, by a parameter threaded through them - it leaves a cold row's undo and spill reads on the reactor, and the body writes nothing the discard does not already cover |
| DE-Q11 | **The bound on running a head again** (DE-R7). | at most `kMaxMissReruns` = 8 runs refused, and BA-S14's deadline (the lock family's fault net, 1 s); the run after either reads every page synchronously, as today. A cancel ends the park (DE-Q15). Eight covers a clustered descent and an index descent missed at every level; a head with more cold pages than that - a long key set, a join probing a cold inner relation - reads the rest on its core as it does today. **What a run-again costs is stated, not hidden**: the statement is parsed and compiled again each time, and the per-shape page-read and row counters of `sys.statement_stats` include the discarded runs, as BA-S14's re-runs already do | (b) the deadline alone; (c) another number |
| DE-Q12 | **DE-R5 on the simulator, which drives `CommandDispatcher::Dispatch` and has no reactor to park on** (`sim/instance.cpp:473`). | the synchronous `Dispatch` runs the same refuse, discard and run-again loop when its instance was given a reader, and the simulator's reader has no thread: a request completes where it is made and reads nothing. So the corpus exercises the refusal and the re-run under its crashes on one thread, and the device sees the reads the statements make, in statement order | (b) the simulator reads synchronously and the path is not in the corpus |
| DE-Q13 | **Whether the path has a switch.** DE-Q5 (a) fixes the thread count and makes it no setting. | no key. A server always has the reader; a dispatcher built without one - a test, a tool - reads synchronously. The thread count is `kPageReadThreads` = 4, `[provisional]`, until a cell prices another | (b) a boot key that turns the path off |
| DE-Q14 | **How the read thread ends a reactor's idle block at `cores = 1`**, where the server builds no wake registry and starts no thread (`src/server/expeditor.cpp:1837-1851`, "G2 asks for zero overhead on the single-core path") - a parked statement would wait out `max_idle_block_ms`, 10 ms, for a 0.5 ms read. Raised after §4 was written, by CLA's reading and the review's. | the wake registry is built at one core too and core 0 registers in it; the reader **writes the wake whatever the destination's sleeping flag says**, because a read can complete inside the window a best-effort kick leaves open and the price there, one idle block, is twenty times the read. **Two departures from G2's "build nothing" at one core, named here**: the registry with its descriptor in core 0's readiness set, and DE-R8's read threads. The close prices both on the resident `cores = 1` cells (DE-R4) | (b) the best-effort kick every other waiter uses, priced at one idle block; (c) no park at one core: the path is built above one core only |
| DE-Q15 | **What a cancel does to a statement parked on a read.** DE-Q11 (a) first said "a cancel is taken at the park"; DE-S1 built, and its review pointed out, something else. Today a descent takes no cancel: one is taken at a walk's slice boundary or at the next frame (`protocol.md` §7). | **the park ends, no later run of the statement is refused, and the statement answers what it answers today** - a descent's rows, read synchronously: a descent takes no cancel, parked or not (DE-R1). **A walk takes one where it gives its core back, and its park at a read is such a place** (added at DE-S2's review): it ends `CANCELLED` there, as it would at the slice boundary that follows | (b) the statement is answered `CANCELLED` at a descent's park - a new place a cancel is taken, and a point `SELECT` that today returns its row would return the cancel; (c) a walk's park takes no cancel and the slice boundary after it does |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DE-S0 - census and premise** | No engine code. **Read**: every caller that can reach `FaultIn` and what it holds there, sorted into a statement head BA-S14 could run again, a walk between pages, a mutation past its first write, recovery, a background task; every caller that recovers from a fetch error (`[quiet-wrong]`, §2); what a head has done before its first fetch; what `MemoryPageDevice` needs for DE-R5. **Measure**, `build-release`, by `tools/bl_mixed.py` extended, an open-loop driver, sessions pinned so that placement does not move them (BL-S3), a pool a quarter of the relation unless said: (i) sessions reading keys that stay resident, alone; (ii) the same beside sessions reading keys uniformly over the relation, page cache as the host leaves it; (iii) as (ii) under DE-Q2; (iv) as (iii) with the cold sessions scanning; (v) as (ii) with a pool that holds the relation - the ceiling. `cores = 1`, and (iii) once at `cores = 4` with the cold sessions on another core than the never-miss ones. Beside them, the device's own time for an 8 KiB read at queue depth 1 with `O_DIRECT`, and the server's `read_bytes`. Read off the never-miss sessions' p0/p25/p50/p95/p99, `sys.wait_stats` and `sys.wait_histogram` for `io.page_read`, and `DB time`. **The 20-minute budget holds only with repeats cut first**; the results file says what was cut. | the lists, the results file, DE-Q1's verdict or its three numbers, and DE-Q3..Q5 back to the operator |
| **DE-S1 - the read off the reactor, and the statement head** | Under the marks: the mechanism behind the `PageDevice` seam and its simulator twin; §2's publish and ownership rules written down first; the refusal that names a page; the dispatcher's park and re-run for the heads DE-S0 listed, the last attempt synchronous; the second asker. | review, the full suite |
| **DE-S2 - the walk** | C1, if DE-Q3 marks it. | review, the full suite |
| **DE-S3 - the close** | DE-S0's cells before against after, interleaved; the documents below; the milestone row. | closed, or closed narrow |

**The stages as DE-S0 re-aimed them** (DE-Q7..Q13, §7). DE-S1 is the reader
(threads, a ticket, a kick, and an arm with no thread), the refusal (the
code, the per-thread note, the fetch's refusing arm on its miss branch only)
and the park and re-run of a **read** statement's head, in `DispatchAsync`
and in the synchronous `Dispatch`. "The second asker" needs nothing: under
DE-Q9 (a) a second asker reads the page itself. DE-S2 is the walk's park at
a miss, not C1. The tests below stand where their subject was kept; the
ones written for a publish a reactor owes, a frame returned after the device
call and a write's head have no subject under DE-Q7 (a) and DE-Q9 (a), and
DE-S3 says so beside each.

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

### DE-S0, 2026-11-19 - the census and the premise

On worktree `de-page-read-off-the-reactor` from `35086008`
(`v3.4.13-19-g35086008`), no engine code. Opened on the operator's standing
go-ahead (go-ahead-achieving-milestone, `raft-marks-2026-11-19.md` §8):
DE-Q0..Q6 adopted (a) as CLA proposed them, none the operator's own mark.

**The census**, read at `35086008` by two read-only passes and spot-checked
by CLA at the lines cited. Line numbers are that commit's.

*A. Who reaches `FaultIn`, and what it holds.* Every accessor of
`page_store.hpp` goes through `FetchPinned` (`:255`) to
`DevicePageStore::FetchAndPin` (`src/storage/device_page_store.cpp:2456`),
which reaches `FaultIn` (`:1089`) unarmed through `Resolve` (`:2474`) and
armed directly (`:2544`). Pages of the pinned class fault only at their
first touch after a mount.

| class | who | holds at the fetch |
|---|---|---|
| a statement head that has written nothing | the bind (`catalog.cpp:2688`, `step_vm.cpp:320`); a tuple key, key set, probe or index `SELECT`, always run inline (`command_dispatcher.cpp:10139-10140`), descending through `btree.cpp:169/183` with **no latch coupling** (`btree.cpp:139-141`: a node is released before its child is asked for); an `INSERT`'s foreign-key resolve and unique check; an `UPDATE`'s or `DELETE`'s locate of its first row (`command_dispatcher.cpp:8925`) | nothing at a descent; a sibling fetch holds the leaf (`btree.cpp:101`, `:1205`); a reverse foreign-key check holds the parent leaf exclusive across a whole child walk |
| a walk between pages | `SelectRun`'s outermost walk (`step_vm.cpp:2163`), an index build's backfill, an assertion's and a Transition's build, `GATHER STATISTICS` | **nothing** at the top of the page loop (`step_vm.cpp:2158-2163`); under a leaf - an undo chain, a spilled value, a join's inner step - the leaf, pinned and shared |
| a mutation past its first write | an `INSERT` from its no-refuse window on (`command_dispatcher.cpp:8107`) - **an issued key's first page write is the `sys.tables` mark bump, before its clustered descent** (`:8169` issues, `:8181` descends); a named key's may be a spill (`:8137`), the mark (`:8147`, written only when the key is at or above it) or the leaf itself, after the descent (`:8151`), and its `NamedKeyVerdict` lookup and key borrow come before the window; a division; index maintenance; undo placement; an `UPDATE`'s or `DELETE`'s second row on; a statement's tail (`Catalog::RecordAccess`, `catalog.cpp:3494`, under the access-statistics root held exclusive) | the leaf, the secured parents, an open no-refuse window |
| recovery and the mount | redo, undo, the spill sweep, the unlogged chains' discard | the page being fetched |
| a background task | the reclaim tick, the plan-statistics steward, the Cabin controller's build (the scan ring's one user, under its view latch), a runtime rollback | as listed |

No fetch is made under a lock-table partition latch or the WAL stream latch,
and the store's structure latch is dropped before `FaultIn`
(`device_page_store.cpp:2534-2544`). There is no catalog mutex; what a
catalog reader holds is a root page's latch.

*B. Who recovers from a failed fetch* (§2's `[quiet-wrong]` list). The two
read passes listed 31 statement-path sites that recover from any failure -
19 falling back to another path, 12 reading it as absent, empty or false -
and 11 more that ignore one on an advisory path; **those counts are the
passes', not re-enumerated**, and only the rows below were re-read at the
lines cited. The ones a refusal would turn into a wrong answer or a wrong
write:

| site | what a refusal would become |
|---|---|
| `UndecidedInsert` (`command_dispatcher.cpp:7438-7446`) | a failed undo read is "not an undecided insert", so a named key reads as placed: `INSERT ... ON CONFLICT DO NOTHING` drops the row, a plain named-key `INSERT` is refused as a duplicate |
| `StillOwned` (`step_vm.cpp:317-330`) | `NotFound`: "relation ... no longer exists" |
| `LocateByTupleKey` (`command_dispatcher.cpp:8925-8943`) | any failure but `NotFound` falls to a whole walk, which then writes |
| `NamedKeyVerdict` (`command_dispatcher.cpp:7466-7472`) | a failed lookup admits the key, left to the placement's own descent |
| the expression-Cabin DDL's resolve (`command_dispatcher.cpp:4044-4048`) | the relation `X` borrow is skipped |
| an online index build's version walk (`index_ddl.cpp:332-339`, `:400-407`) | a version read as resolved |
| `VerifyTupleAt` (`tuple_verify.cpp:15-18`) and its six callers | a hint miss: the descent answers, the hint statistics are skewed |
| the walk's head seek (`step_vm.cpp:2114-2122`) | a walk from the leftmost leaf |
| the Waystone trail read (`command_dispatcher.cpp:10022-10027`), the access statistics (`access_stats.cpp:37`), the trail recorder | no replay; nothing recorded |

Specific-code tests - `NotFound` at recovery's four sites, `TxnConflict` at
the seven stale-descent restarts, `ResourceExhausted` at the pool's retries
- do not match a new code. The pool-full arm inside a no-refuse window
fail-stops (`device_page_store.cpp:955-964`), which is BE-Q11 and the reason
no refusal may be made there.

*C. What a head has done before its first fetch, against BA-S14's answer.*
`StageStatement` clears every per-statement member before a run
(`command_dispatcher.cpp:1112-1147`) and enters and leaves the core's
statement epoch around it (`:1119`); a re-run goes through
`DispatchAndStage` whole (`:920-927`), so the bind, the view of a
`READ COMMITTED` statement and the borrow are made again. `EndWrite` decides
that a statement is re-runnable from its transaction's trail
(`:13444-13468`), which a read never grows; a sink that has streamed ends
the re-runs (`:1044`) and one that has not is rewound (`:1052`). Counted
twice by a re-run, as BA-S14 leaves them: the executor's counters and the
hint statistics. **The tails are not the dispatcher's**: a `SELECT`'s
`RecordExecution` - the Waystone trail, `Catalog::RecordAccess`'s
`sys.access_stats` write under its root held exclusive, the optimizer
signals and the plan-statistics fold - runs in `SelectRun::Finish`
(`command_dispatcher.cpp:6955-6988`, "the recording below writes catalog
and Waystone pages"), inside the inline head, before the dispatcher sees an
outcome. So a run whose refusal a caller swallowed has already recorded,
and written pages, by the time the note is read; DE-Q10 (a) holds only if
`Finish` reads the note and skips the recording, and the refusing arm is
disarmed before the recording's own fetches (a refusal inside
`RecordAccess`'s chain insert would come after a page write). **Not read at
DE-S0 and owed before DE-S1 touches the dispatcher**: where an inline head's
rows can leave before it returns.

*D. The simulator.* It drives `CommandDispatcher::Dispatch` on one thread
(`sim/instance.cpp:473`) over `MemoryPageDevice`, whose reads are already
safe against each other (`memory_page_device.hpp`, "Concurrency"). It has no
reactor to park on and runs no thread, so DE-R5 is met by a reader with no
thread, not by a device change (DE-Q12).

**The measurement** (`bench/v3.4.13/results-de-s0-premise-v3.4.13-19-g35086008.md`,
raw output under `bench/v3.4.13/archive/de-s0-premise-v3.4.13-19-g35086008/`;
`tools/de_premise.py`, `tools/bl_mixed.py`'s helpers extended into a driver
of its own because one load is shared by every cell). `build-release` at
`v3.4.13-19-g35086008`, `cores = 1`, a relation of 128,960 pages, a pool of
32,768 frames, four sessions reading 700 resident keys at 1,000 statements a
second open-loop, four reading uniformly at 648 a second - 0.3 of a core
over the device's own 463 us for an 8 KiB read. The commit was described
`v3.4.13-19-g35086008` when the run started and has carried the tag
`v3.4.14` since 08:02 UTC that day; the file keeps the name it was measured
under. Two runs a cell; each pair
below is the two runs, the never-miss sessions' latency from their
schedule's due instant.

| cell | never-miss p50 | p95 | p99 | the core inside the read |
|---|---|---|---|---|
| (i) alone | 251 us, 248 us | 353 us, 359 us | 415 us, 889 us | 0 % |
| (ii) beside cold sessions, page cache warm | 230 us, 256 us | 327 us, 381 us | 383 us, 580 us | 0.22 %, 0.24 % (4.4 us, 4.8 us a read) |
| (iii) the same, page cache kept from answering | 261 us, 258 us | 797 us, 793 us | 1,088 us, 1,263 us | 27.5 %, 27.0 % (551 us, 540 us a read) |
| (v) the same, a pool that holds the relation | 234 us, 233 us | 334 us, 336 us | 387 us, 545 us | 0 % |
| (iv) beside one scanner, page cache kept from answering | 7.2 s, 5.8 s | 9.9 s, 8.8 s | 10.2 s, 9.1 s | 92.8 %, 93.0 % (47 us a read) |
| (vscan) the same, a pool that holds the relation | 268 us, 277 us | 336 us, 348 us | 378 us, 391 us | 0 % |
| (iii) at `cores = 4`, the cold sessions on core 0, these on 1 to 3 | 285 us, 280 us | 379 us, 368 us | 552 us, 421 us | 28.4 %, 28.7 % of core 0 |

- **A warm page cache hides the whole cost**: in (ii) a miss is a 4 to 5 us
  copy and the never-miss sessions are where (i) and (v) have them.
- **With the device answering, the never-miss sessions wait behind the
  reads**: DE-Q1 (a)'s test fails - (iii)'s p99 is 1,088 us and 1,263 us
  against (v)'s 387 us and 545 us - and its three numbers are: (iii) over
  (ii), run by run, p50 +31 us and +1 us, p95 +470 us and +412 us (2.4 and
  2.1 times), p99 +704 us and +684 us (2.8 and 2.2 times); 27 % of the
  core's time inside
  `io.page_read`; 463 us for the device's own read at queue depth 1 and 540
  to 551 us for the server's. A p99 moves by up to a factor of two between
  two runs of one cell ((i): 415 us, 889 us), so the p95 is the number that
  carries the claim.
- **Beside a scan it is not a factor**: in (iv) the walk reads 19,700 pages
  a second, each a 47 us blocking read, so a 64-page slice is about 3 ms
  where (vscan)'s is tens of microseconds; a never-miss statement waits
  6.4 ms from its send, the four sessions together get about half of the
  1,000 statements a second their schedule asks for, and the schedule falls
  behind for as long as the scan runs. The same scan over a resident
  relation leaves their p95 and p99 where (i) and (v) have them and their
  p50 34 to 43 us above (v)'s, unresolved at two runs.
- **The stall does not cross cores**, as §1.1 read: at `cores = 4` the
  never-miss sessions on other cores keep their p95. That cell has no
  resident twin at `cores = 4`, and its clients ran on CPUs 2, 3, 6 and 7,
  two of which the reactors of cores 2 and 3 were pinned to, so it says no
  more.
- The never-miss sessions' lifetime counts of `io.page_read` (18, 24, 27
  and 32 or 33) are, to one read, the same in (i), whose window has no pool
  miss, as in (ii) and (iii): their reads are their first touches, before
  the window. The view is cumulative, so this bounds a session's misses
  inside a window at one and does not show it is none.

What was cut for the 20-minute budget, and that the stage spent about 23
with its build and an aborted first attempt, is in the results file.

**The verdict.** DE-Q1 (a): not C0. The ruling it leaves to the operator is
written as **DE-Q7 and adopted (a) on the standing go-ahead: build,
narrow** - with DE-Q8 (the walk parks at a miss; C1 as written cannot give
back a core that is 93 % inside sequential reads), DE-Q9 (the read thread
leaves the page in the operating system's cache and the pool reads it as it
reads every page), DE-Q10 (a code of its own and a note the dispatcher
obeys), DE-Q11 (eight runs and BA-S14's deadline, then a synchronous run),
DE-Q12 (the simulator's reader has no thread), DE-Q13 (no switch) and
DE-Q14 (the wake at one core), each (a), each CLA's proposal and none the
operator's mark; DE-Q8 to DE-Q11 stand as the review left them (below). **The two
`[quiet-wrong]` items were adopted after their premises were read on the
tree at `35086008`**: DE-Q9 (a)'s, that a fetch has one path to a frame
(`FaultIn`, `device_page_store.cpp:1089-1168`) and that nothing but a
reactor touches the frame table; DE-Q10 (a)'s, that the heads DE-Q7 (a)
admits write no page before the dispatcher reads their outcome - true of a
`SELECT`'s body, and **false of its tails as the tree stands**: they record
inside the head (list C), so DE-Q10 (a) needs `SelectRun::Finish` to obey
the note too, which is DE-S1's first step.

DE-Q3 (a) is met by DE-Q7 (a) and DE-Q8 (a) together: C2 for a read
statement's head, and for the walk the park that cell (iv) asks for in
place of C1. DE-Q4 (a) and DE-Q5 (a) stand as proposed.

**Not executed at DE-S0**: the test suite - the stage changes no build
input (`tools/` and documents only).

**Reviewed by `critics-developer` before the commit**; it opened the cited
lines, checked the headline numbers against the JSON, and built and ran
nothing. **Applied**, in place by the review: list C and the verdict - a
`SELECT`'s tails record inside the head, so DE-Q10 (a)'s premise was false
of them as written; list A's and DE-Q7's `INSERT` claim, true of an issued
key only; the census counts softened to what the passes listed;
`NamedKeyVerdict`'s row; (vscan)'s p50 gap; two caveats in the results file
(the `cores = 4` cell's client CPUs, and that cell (iv)'s percentiles cover
only the requests sent). **Applied by CLA from its findings**: DE-Q9 (a)
revised - a fetch that may be refused asks first without waiting, so only a
read that would wait is refused, a run-again is admitted by the page being
in the operating system's cache, and a page already there never pays a park
(the review's first two findings, and CLA's own test of `RWF_NOWAIT` on the
host made while the review ran); DE-Q8 (a)'s retry made exact - the fetch
re-issued alone, the refusal a property of the fetch and not of the thread;
DE-Q10 (a)'s four conditions - the top-level `SELECT`'s body only, the
tails obeying the note, the arm dropped at the sink's first hand-on, the
note read before a yield; DE-Q11's costs stated; DE-Q14 written for
`cores = 1`, with G2's two departures named; the driver hashing its binary
once and recording each session's last core. **Rejected**: arming only the
two descents' node fetches (the review's narrowing of DE-Q7) - it is
DE-Q10 (d), and it leaves a cold row's undo and spill reads on the reactor
while the body writes nothing the discard does not cover; `proc.pid` in
place of the driver's `/proc` scan - that `systemd-run --scope` runs the
server in its own process was not checked on this host and the scan is
known to work; folding the results file's closing section into its tables -
it is the insight the bench rule asks for.

### DE-S1, 2026-11-19 - the read off the reactor, and a `SELECT`'s head

On worktree `de-page-read-off-the-reactor`, on `721d0732`
(`v3.4.14-4-g721d0732`). **Overhead not measured; measured at the
milestone's close.**

**List C's owed item, read first.** An inline head's rows leave a wire sink
only at `WireResultSink::TakeSealed`, which a walk reaches at a slice
boundary (`src/server/kwp_session.cpp`, `AtSliceBoundary`) and a statement
at its end; a descent meets no slice boundary (`src/exec/step_vm.cpp`, the
`outermost && slice_` test), so an armed body hands no row on before it
returns. The tails are `SelectRun::Finish`'s, as the DE-S0 review found.

**Built.**

- **The device** (`storage/page_device.hpp`). `ReadPageNoWait` reads a page
  only where the device can hand it over without waiting; `WarmPage` brings
  one to where that read finds it and keeps nothing. The file device is
  `preadv2` with `RWF_NOWAIT` and a `pread` into a buffer it drops; a kernel
  or file system that refuses the flag is remembered and read with `pread`.
  The memory device has the twin a test can set
  (`MemoryPageDevice::NoWait`): never, until warmed and spent by one read,
  or always. Every other device reads, as before.
- **The refusal** (`storage/miss_refusal.hpp`, `StatusCode::kNotResident`,
  `DevicePageStore::FaultIn`). On the miss path only, a fetch made under the
  thread's arm, or with `PageAccess::kScanNoWait` (DE-S2's, its store half
  built here), asks without waiting; a read that would wait gives its
  reserved slot back and is refused, an armed one noted on the thread. A
  hit reads and writes no word it did not before (DE-R4).
- **The reader** (`storage/page_reader.hpp`): four threads started at boot,
  a queue under one mutex, a ticket the asker polls, a wake written
  whatever the destination's flag says (`WakeRegistry::Wake`). With no
  thread a request is served where it is made.
- **The dispatcher.** A statement that is itself a plain `SELECT`, whose
  head is a descent run inline, runs its body armed (`SelectRun::Execute`),
  the arm dropped as the walk returns. `StageStatement` reads the thread's
  note: a body that was refused is replaced whole by the refusal carrying
  its page (`DispatchOutcome::page_wait`), whatever it answered, and
  `SelectRun::Finish` records nothing for it. `AwaitStatementWaits` takes
  the page as a third wait: `AwaitPageRead` rewinds the sink, hands the
  page to the reader, parks under `io.page_read` until the read is made,
  the statement's deadline passes or its cancel flag is raised, and runs
  the statement again. `Dispatch` does the same with nowhere to park, where
  the reader has no thread. Eight refused runs, or a wait that ended
  without its read, and no later run of the statement is refused
  (`Session::miss_refusals`, `kMaxMissReruns`).
- **The server** (`Expeditor::Start`). The reader is built before a core can
  run a statement and stopped beside the WAL writer's quiesce, before a
  reactor leaves the wake table. At `cores = 1` core 0 registers in a table
  of one entry that only the reader writes to, and publishes no load word
  through it (`Scheduler::AttachWakerTable`'s `publish_load`).
- **The simulator** (DE-Q12): a fourth toggle, `read_off_reactor`, drawn
  after the three it had and paired with them
  (`RunTogglePairing`), so half of every seed's iterations run their point
  reads refused once a miss and run again, under every crash and fault
  mode the corpus has.
- `sys.meta` gains `pool_would_wait` and `contention_miss_reruns`.

**Tests** (`tests/de_page_read_test.cpp`, `tests/de_page_read_rig_test.cpp`,
one cell each in `tests/expeditor_test.cpp` and `tests/kwp_error_test.cpp`):
the two devices; the store's arm - refused, noted, the slot returned, a hit
never asked, the walk's access kind leaving no note, an I/O error, a torn
page and a full pool answering as unarmed (DE-R6); the reader with and
without threads, and `Wake` against `Kick`; through the dispatcher over a
simulator instance - a refused head answers what a synchronous one does
(DE-R1), a discarded run records nothing, **a run whose refusal a caller
swallowed is discarded whole** (planted by the row hook: the row taken
back, the statement made again, the access counts those of one run),
nothing but a top-level `SELECT` is ever refused (a write, a locking read,
an `INSERT ... SELECT`, a write's subquery, a traced `SELECT`; DE-R2), the
run after the eighth refused one reads synchronously (DE-R7), and every
device read is one `io.page_read` on either path (DE-R9); on the two-core
rig, under the suspend audit and strict wait scopes (DE-R3) - a read that
would wait parks, is read by a thread and answers on both cores over the
same keys, a wait past its deadline ends in a synchronous read, and a
cancel ends the park. One mutant was run: with `Finish`'s note test
removed the swallowed-refusal cell fails, the access count one too many.

**§5's cells with no subject under DE-Q7 (a) and DE-Q9 (a)**: a synchronous
caller meeting a read in flight for a parked asker (nobody waits for a
publish; a second asker reads the page itself); a frame returned after the
device call (no frame is held across a refusal); a statement that has
streamed a row meeting a refusal (an armed body is a descent and hands no
row on; `BeginMissRerun` keeps the guard and nothing reaches it). **Not
written**: a re-run over a real KWP sink (the cell above pins the rewind
through a counting session sink), the park's own `io.page_read` count (the
synchronous loop's is pinned), a cell for a session closed mid-read. **Not
executed**: a `tsan` build of the rig cells, and the nightly corpus
(`scripts/bs/nightly.sh`).

**Reviewed by `critics-developer`**, read-only, while the suite ran; it
found no wrong answer on the paths it traced and six things to change.
**Applied**: at `cores = 1` the reader's wake table made core 0 publish a
load word each iteration, against BL-R9 - `AttachWakerTable` takes
`publish_load` and the reader's table passes false; **a refused body commits
no Cabin set** (`step_vm.cpp`, before `cabins_->Commit`) - the review could
not show a swallowed refusal that makes a recording walk miss a row, and
the guard closes the premise instead of resting on that; the rig cells'
reader and gate moved to the fixture, whose teardown opens the gate, stops
the reader and takes it from both dispatchers; a wait that ended on its
deadline or a cancel no longer has the run after it book no event for the
page (DE-R9); DE-Q11's "a cancel is taken at the park" was not what was
built or what DE-R1 allows, and is now **DE-Q15, adopted (a)** as built;
the top-level word became a parameter of `HandleSelect`, the note's flag
merged into its page, `Finish`'s refusal text cut, three comments
shortened; cells for four surviving mutants (the tail ignoring the note,
the arm left set, a leaked slot, `Wake` as `Kick`), a full pool under the
arm, the one-core wiring and the wire registry's row. **Stated, not
changed**: a `SELECT` that spent its deadline on page parks has none left
for a relation wait the same statement meets, so that wait is refused at
once - DE-Q11 (a) as adopted; DE-S3 enters it in `known-gaps.md`. On XFS
`RWF_NOWAIT` can answer "would wait" for an inode lock, a spurious park,
and a short read at the end of a file is "would wait" until the eighth
run: both bounded by DE-Q11, neither a wrong answer. **Rejected**:
shrinking `ParkedReadScope` to a set and a clear - DE-S2's walk nests it
inside nothing, but the scope is what makes the clear unmissable on every
exit; a cell that the cap's removal fails by assertion - it fails by
ctest's timeout, which is a failure.

**The suite**: 0 failed of 4813 at this stage's tree, every cell `scripts/test.sh`
runs.

**A smoke run, not a measurement**: `tools/de_premise.py`'s cells (ii) and
(iii) once on `build-release` at this stage's tree, on a host building at
the time. It showed the park, the thread's read and the wake working at
`cores = 1` on the real device; no number is taken from it.

### DE-S2, 2026-11-19 - the walk

On worktree `de-page-read-off-the-reactor`, on `0d3ee8fe`. **Overhead not
measured; measured at the milestone's close.**

**Built** (DE-Q8 (a)). `exec::SlicePolicy::hand_page`: set, a `SELECT`'s
outermost walk fetches each leaf with `PageAccess::kScanNoWait`; a leaf the
device cannot hand over at once comes back `kNotResident` before a slot is
visited, and the walk - at the top of its page loop, holding no pin and no
span - ships what its sink has sealed, hands the leaf to the page reader,
parks under `io.page_read`, takes a cancel if one was raised, and fetches
that leaf again with `kScan`, synchronously, past the cycle guard and the
fetch count it had already passed (`src/exec/step_vm.cpp`). The park ends at
the read, at the fault net's deadline, or at a cancel. `SelectRun` sets
`hand_page` in `ParkAtReads`, called by `CompleteSelect` alone. A fetch that
may be refused is also refused where another core is reading the page
(`DevicePageStore::FetchAndPin`'s `loading_` wait), instead of waiting that
read out on its thread. A build's walk, `GATHER STATISTICS` and a walk
driven to its end where it stands read as before.

**Tests** (`tests/de_page_read_rig_test.cpp`, six cells on the two-core rig
under the suspend audit): a walk on each core parks at leaves that would
wait and answers every row once, and is never run again from its head;
while a walk is parked on a read the device does not finish, its core
creates a relation, writes it and reads it back; a cancel is taken at the
park, the walk asking the reader for no second leaf; with every no-wait
read refused whatever a thread has read, the walk still gets its leaves
(the second fetch is the blocking one); with the reader never finishing a
read, each park ends at its deadline and the walk answers (DE-R7); a walk
handed on and then driven synchronously - a `GROUP BY` through the query
layer, an `UPDATE`'s subquery - never parks and answers. Two mutants were
run: the park armed at the hand-off fails the last cell ("a SELECT's walk
parked on a wait under a synchronous dispatch"), as the review said it
would.

**Reviewed by `critics-developer`**, read-only, while the suite ran. It
found the retry exact and nothing held at the park, and four things to
change. **Applied**: **the park was armed where a walk is handed on, and a
walk handed on can still be driven to its end where it stands**
(`RunToEnd`: the query layer's walks, a write's subquery) - on a server,
such a statement over a relation that is not resident would have been
refused; armed now by the one driver that parks, with a cell that fails
without the fix; **the park had no bound** (DE-R7) - each is now under the
fault net, followed by a synchronous read; a no-wait fetch waited on
another core's read of the same page - refused there too; sealed batches
are shipped before the park; cells for the two mutants its table found
alive (a second fetch that refuses again; a cancel left for the next
boundary). **DE-Q15 amended, and adopted as amended**: a walk takes a
cancel at its park, which the first wording ("nowhere it is not taken
today") did not allow. **Rejected**: making `PageReader::threaded()` a
relaxed atomic read - one uncontended lock per handed-on `SELECT`, on no
row's path.

**The suite**: 0 failed of 4819 at this stage's tree, every cell `scripts/test.sh`
runs. **Not executed**: a `tsan` build, and the nightly corpus.

**A smoke run, not a measurement**: cells (iii), (iv) and (vscan) once on
`build-release` at this stage's tree before the review's fixes, the host
building at the time. It showed a parked walk's core serving its other
sessions on the real device; no number is taken from it.
