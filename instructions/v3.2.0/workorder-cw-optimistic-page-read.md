# Work order CW - a page read that takes no latch: the optimistic page read, measured before it is built

Status: **OPENED 2026-11-22 on the operator's standing go-ahead**
(`/go-ahead-achieving-milestone CW`, `raft-marks-2026-11-22.md` §11), at
`56b44964` (`v3.5.0-90-g56b44964`) on branch
`worktree-cw-optimistic-page-read` in the worktree `sync-stale-records`.
**CW-Q0..Q9 adopted (a) on that go-ahead** - CLA's proposals, not the
operator's per-item mark (§4). Before that: **written 2026-11-17** on worktree
`cw-optimistic-page-read` from `7aa75760` (`v3.4.11-28-g7aa75760`) on the
operator's word of 2026-11-17 (`raft-marks-2026-11-17.md` §1): *"Then write
a milestone for the optimistic page read - the measurement, the validity
verdict, the implementation and so on."* Every `[source-read]` claim in §1
was read at `7aa75760`; CW-S0 re-reads them at its own commit (§7).

**Exploratory, and the measurement comes first.** The one number the tree
holds on this path argues against the premise (§1.1), so CW-S0 and CW-S1
can each end the order with nothing built, and that is a result, not a
failure.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured.

Related: `docs/spec/page.md` §3, §6 (the page latch, the frame table, the
orders); `docs/rules/rules.md` §3; `docs/spec/eviction.md`;
`docs/spec/heap-and-tuple.md` §5 (the descent and its coverage check);
`docs/pending/multicore-scaling-cores4-below-cores2.md` (the unattributed
tail at `cores = 4`);
`bench/v3.0.0/results-bp-bq-close-v2.7.0-924-g3e77e7d1.md` §7;
`bench/v3.0.0/results-ba-s4-census-v2.7.0-725-gc390a624.md`;
`instructions/v3.0.0/workorder-am-m1-shared-pool.md` (AM-S1, AM-S2: the
latch and the shared frame table);
`instructions/v3.0.0/workorder-be-bounded-pool.md` (BE-R1: frames are
slots); `instructions/v3.2.0/workorder-cq-column-reservation.md` (the
exploratory shape CW borrows: premise, spike on the branch, verdict).

## 0. What CW is

`[operator]` A milestone for the optimistic page read: its measurement,
its validity verdict, its implementation.

`[design]` At `cores > 1` a read of a resident page writes shared memory
five times: three holds of the instance's one frame-table latch and two
compare-and-swaps on the frame's latch word (§1.2). An **optimistic page
read** reads a version word of the frame, copies what it needs out of the
page, reads the version again, and uses the copy only if the word did not
move - writing nothing shared. CW asks, in this order:

1. whether those five writes cost anything a statement can feel (CW-S0);
2. what the most a latch-free read could return is, and whether a cheaper
   remedy returns it (CW-S1);
3. only then, what the protocol is (CW-S2) and its build (CW-S3).

The unit is the **page**, not the row. A row has no fixed address - a leaf
is in key order by placement and is compacted in place - so a row-level
version cannot answer "is this slot still that row", and the Keystone word
and the MVCC header have no room for one (invariants 5, 12). A page
version is frame memory: no on-disk format moves.

What it is not: Oracle's consistent-read clone. No page is copied whole,
no undo is applied to a page, and visibility stays the row's
(`txn.md` §4).

## 1. The tree as read

### 1.1 What is measured `[measured]`

| measurement | shape | reading |
|---|---|---|
| BP/BQ close, `v2.7.0-924-g3e77e7d1`, `cores = 4`, `relaxed`, 8 sessions (`results-bp-bq-close-...md` §7) | `point` | `0.032-0.036` page-latch waits a statement (a count; no duration is kept), frame-table wait `0.73-0.75 us` a statement, every named wait together `0.54-0.56 %` of a `136-139 us` mean latency |
| the same | `insert1` | `0.26-0.28` page-latch waits a statement, frame-table wait `0.45-0.51 us` |
| BA-S4 census, `v2.7.0-725-gc390a624` | the whole census | `98,466,594` page-latch spin turns, attributed to no page and no item |
| multicore re-measurement, `v3.2.0-64-g9b7d53d7` (the pending entry) | `point`, `insert1` | `cores = 4` is `21-28 %` under `cores = 2`; p95 and p99 are 2-4 times `cores = 2`'s; the `contention_*` deltas were kept per run and never analysed |
| DO, `v3.5.0-55-g48a3276c` to `v3.5.0-61-g7116b668` (`bench/v3.5.0/results-do-s0-census-v3.5.0-55-g48a3276c.md`, `results-do-s1-e1-e3-v3.5.0-56-g47f7016c.md`, `results-do-close-v3.5.0-61-g7116b668.md`; entered here by DO-Q8 (a)) | read and write hits, one store; `point`, `insert1`, `pkmix` at `cores = 2` | structure-latch holds counted exactly: a read hit 3 to 2, a write hit 5 to 3; a `point` statement 12 holds before DO; the hold a read hit 212 to 81 ns on one thread, one reader about +19 % fetches a second over E4b-E4d (+7.9, +2.5 and +8.0 % a stage); with three readers on one store 87 % of fetches waited and throughput fell to 38 % of one reader's, before DO; the checkpoint's walk 38.7 to 15.4 ms (H1) at 1 Mi frames, the rest of it E2's |

**What this says:** a read *waits* on these latches for about half a
percent of a statement. The BP/BQ number is 619 commits old and predates
BZ, CE and CP.

**Not measured, and it is the whole premise:**

- the CPU a read spends on these words when it does **not** wait - the
  cache line of `frames_latch_`'s mutex, and of a hot frame's `pins` and
  latch word, moving between cores on every statement. A wait counter
  reads zero for it;
- which pages the page-latch waits are on (a relation's root, a hot leaf,
  a catalog page);
- how many page fetches a point statement makes;
- whether the `cores = 4` tail has anything to do with either latch.

### 1.2 What the tree does `[source-read]` at `7aa75760`

- **One read of a resident page, armed.** `FetchAndPin` takes the
  structure latch, finds the frame in `frames_`, applies the usage bump
  and pins (`src/storage/device_page_store.cpp:2439`, the armed arm); the
  page latch is then taken in the accessor's mode by compare-and-swap on
  the frame's word (`:2718`). `UnpinFrame` takes the structure latch to
  find the frame, releases the word, and takes the structure latch again
  to drop the pin (`:2793`). **Three holds of the structure latch and two
  compare-and-swaps a page.**
- **The structure latch is one `std::mutex` for the instance**
  (`include/kds/storage/device_page_store.hpp:1579`,
  `include/kds/base/latch.hpp:81`), guarding `frames_` and every frame's
  `pins`, `dirty`, `rec_lsn`, `writing` and `dirty_gen` (`:1556`). It is
  not partitioned.
- **The page latch is one 32-bit word a frame** - an exclusive bit, the
  owning core, a count - with no version in it; a waiter spins 64 turns
  and then yields, with no queue and no writer preference
  (`include/kds/storage/page_latch.hpp:16-56`, `:89`). `GetForRead` is
  shared, `Get` and the creates exclusive
  (`include/kds/storage/page_store.hpp:266-289`). Never upgraded.
- **At `cores = 1` neither is touched.** Both are armed from the
  superblock's core count (`device_page_store.hpp:119`, `:705`); an
  unarmed store's accessors never reach the word.
- **Frames are slots in chunks that never move** (`device_page_store.hpp:70`,
  `:1701-1726`): a frame's bytes stay mapped for the life of the store,
  so a read of a slot that has been evicted and reused reads another
  page's bytes, never unmapped memory.
- **A frame already carries a generation, and it is not a read version.**
  `dirty_gen` moves at every dirty mark, under the structure latch, and
  the mark precedes the write's bytes (`device_page_store.hpp:934-944`);
  writeback copies a page under its shared latch and cleans the frame
  only if the generation has not moved since. The shape - copy, then ask
  whether it moved - is CW's; the word cannot be reused as it stands,
  because it moves before the write and is read under the latch CW would
  not take.
- **A descent holds one page at a time**, no latch coupling and no
  B-link, an internal node released before its child is asked for
  (`include/kds/storage/btree/btree.hpp:87-110`); what closes the gap is
  a coverage check after the fact (`src/storage/btree/btree.cpp:97`,
  `:154`), bounded at `kMaxDescentRestarts` = 4
  (`include/kds/storage/insert_placement.hpp:66`). So a point lookup
  already reads every level through a separate fetch, and a relation's
  root is fetched by every statement on every core.
- **A reader decodes in place.** A `PageRef` hands back a span into the
  frame (`page_store.hpp:116`), and the scan kernels reject on a cell's
  bytes before any decode (BM). 71 lines across 24 files name a shared
  fetch (`GetForRead`, `PageAccess::kRead`, `PageAccess::kScan`); which
  of them copy out, which decode in place and which hold the page across
  a call is not listed anywhere.
- **Some shared holds are long by design**: a scan ring's hold on a leaf
  across its consumer's var-heap fetches, a `SELECT` walk's leaf, a
  writeback's copy (`docs/spec/page.md` §6).
- **The simulator runs unarmed**, and the `tsan` build is in the nightly
  (`CLAUDE.md`, the simulation row): a read of bytes another thread is
  writing is a data race by the language's rule, whatever the validation
  after it.

## 2. The candidates `[design]`

Priced by CW-S0 and CW-S1; none is chosen here.

- **C0 - nothing.** The verdict if the words cost nothing a statement
  can feel.
- **C1 - the pin and the lookup made cheap, the latch kept** - **after
  DO** (DO-Q8 (a)): priced against the tree DO leaves, whose hit is already
  one lookup and two holds, with no free-map hold. The
  structure latch partitioned by page id, or the hit path's lookup and
  pin taken off it (an atomic pin count on the frame, the table read
  under a partition). No optimism: readers still take the page latch
  shared. The cheaper remedy if the structure latch is the cost and the
  page word is not.
- **C2 - the optimistic read with the pin kept.** A version beside the
  latch word, moved by every exclusive release; a reader pins as today,
  reads the version, copies, validates, and never writes the frame's
  word. Removes two compare-and-swaps of five shared writes. Listed to
  be priced, not because CLA expects it to pay.
- **C3 - the optimistic read with no pin.** The version also moves when
  the frame is evicted, freed or reused, so a reader needs no pin: it
  finds the slot (a per-core memo of page id to slot, checked against the
  slot's owner and version; the frame table on a miss), reads, copies,
  validates. Sound only because slots never unmap. Removes all five.
  - **C3a** - a descent's internal nodes only: read `ChildFor(key)`,
    validate, go on. The root is the page every core shares.
  - **C3b** - a point lookup's leaf: the fixed-length row copied out,
    validated, then judged for visibility on the copy.

In every candidate a writer stays as it is - exclusive, pinned, logged
under its hold - and a reader that fails validation a bounded number of
times, or that holds a page across a call, takes the shared latch as
today.

## 3. Rules

Binding from CW-S2 on, if the order reaches it.

- **CW-R1 `[quiet-wrong]` - nothing read optimistically is acted on
  before it is validated.** No row is returned, no `Status` is raised
  (`Corruption` included: invariant 13's "a disagreeing length is
  `Corruption`" is asked of validated bytes only), and no page id read
  from an unvalidated node is fetched.
- **CW-R2 `[quiet-wrong]` - an unvalidated page is untrusted input.**
  Every offset and count read from it is bounds-checked against
  compile-time constants before it indexes anything, and every loop over
  it is bounded by a constant.
- **CW-R3 `[quiet-wrong]` - the version moves at every act that can
  change what a reader of that slot sees**: an exclusive hold's release,
  and - for C3 - eviction, `FreePage`, slot reuse and a redo's
  `CreateAt`. CW-S0's census is the list; a site missing from it is a
  wrong answer, not a slow one.
- **CW-R4 - the latched read stays whole and is the fallback.** A
  bounded number of failed validations, then the shared latch. The two
  paths answer byte-for-byte alike, and a contract suite compares them
  as the waystone, index, cabin and inner-build suites compare theirs.
- **CW-R5 - `cores = 1` is untouched.** An unarmed store reads no
  version and writes none (AM's G2).
- **CW-R6 - one audited copy routine.** Every optimistic read of page
  bytes goes through it; nothing decodes a live frame in place without
  the latch. It is the one site the `tsan` build is told about.
- **CW-R7 - nothing on disk moves.** No page-header word, no record
  kind, no superblock bump.
- **CW-R8 - an optimistic read holds nothing, so it has no place in a
  latch order.** `page.md` §6 says so where it states BB-R4's order.
- **CW-R9 - the cache keeps its signal.** A pin-less read still tells
  CLOCK the page was used (C3), or the root every statement reads is the
  sweep's first victim.

## 4. Items for the operator (CW-Q), CLA's proposals

(a) is CLA's proposal in each. **CW-Q0..Q9 (a) adopted on the operator's
standing go-ahead of 2026-11-22 (go-ahead-achieving-milestone)**
(`raft-marks-2026-11-22.md` §11); none is the operator's own mark.

| | item | (a) | other |
|---|---|---|---|
| CW-Q0 | Is CW exploratory - may S0 or S1 close it with nothing built? | yes | (b) build C3 whatever S0 shows |
| CW-Q1 | The bar. | **V0** (after S0): go on only if the read path's shared words are at least `5 %` of server CPU a `point` statement at the clean multi-core cell, or the same-relation cell is at least `5 %` under the distinct-relations cell (M4). **V1** (after S1): build only if the ceiling is at least `10 %` stmt/s on `point`, median of 3, above the cell's own spread | (b) the operator's numbers |
| CW-Q2 | Which candidate is built when V1 passes? | the cheapest whose own spike reaches two thirds of the ceiling; C3 only if C1 does not | (b) C3 regardless; (c) C1 only |
| CW-Q3 | Which readers go optimistic under C3? | C3a and C3b: a descent's internal nodes and a point lookup's leaf. Scans, walks, the scan ring and every writer stay latched | (b) C3a only; (c) every shared fetch |
| CW-Q4 | An off-switch? | a boot key, `page_read = latched | optimistic`, default `optimistic` above one core once CW closes; it is what the contract suite flips | (b) a test hook only, no key |
| CW-Q5 | The data race. | the copy routine alone carries the `tsan` exemption, with a rig test that writes under the copy and checks every answer | (b) no optimistic read is built unless it is race-free by the language's rule (atomic word reads) |
| CW-Q6 | The fallback bound. | 3 failed validations, then the shared latch; S3 measures how often it is reached | (b) another number |
| CW-Q7 | The pending `cores = 4` question. | S0 uses its option 1 (the clean cell) and option 2 (the `contention_*` deltas) for the read shapes and writes what it finds into that entry; the entry stays the operator's | (b) CW does not touch it |
| CW-Q8 | Fairness of the page latch (no queue, no writer preference; `page.md` §6 says it is owed). | out of CW | (b) in |
| CW-Q9 | The measurement budget. | the standing one: 20 minutes a stage, about an hour at the close, cuts named in the results file | (b) lifted for S0 |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **CW-S0 - premise and census** | No engine code. **Census**: every shared fetch site classed copy-out / in-place decode / held across a call; every act that changes a frame's bytes or identity, and under what; page fetches a statement for `point` and `insert1`. **Measurement** (M1-M5 below), `build-release`, inside the budget. | **V0**: go to S1, or close with the numbers |
| **CW-S1 - the ceiling, a spike that stays on the branch** | A measurement-only patch, never landed: on a loaded, warmed, read-only instance with a pool large enough that nothing is evicted, a read skips (i) the page word, (ii) the page word and the structure latch. That is the most C2 and C3 can return. C1 is spiked the same way. | **V1** against CW-Q1: C0, C1 or C3 |
| **CW-S2 - the model, on paper** | The version protocol against the code: where the word lives, every site that moves it (S0's list), the reader's steps, the fallback, the slot memo, CLOCK's signal, the `tsan` treatment, the contract suite's design, the amendments owed (`page.md` §3 and §6, `rules.md` §3, `eviction.md`, `heap-and-tuple.md` §5). | items back to the operator before any code |
| **CW-S3 - the build** | S3a the version word and every move site, read by nothing (its cost on a writer measured at the close); S3b C3a; S3c C3b. Each with its review and the full suite, the armed rigs and the `tsan` and `asan` cells beside it. | a branch at the push gate |
| **CW-S4 - the close** | The interleaved A/B over the whole change: `cores = 1` (nil is the claim), `point`, `insert1` and a mixed read-write shape at the clean multi-core cell; the fallback rate; the specs amended, the row flipped. | closed, or closed narrow |

**CW-S0's cells.**

- **M1** - shared writes a statement: fetches, structure-latch holds and
  word compare-and-swaps for `point` and `insert1`, counted, not
  inferred.
- **M2** - the `contention_*` deltas and server CPU at `cores` 1, 2 and
  the largest clean cell (on this host `cores = 3` on CPUs 0, 2, 4 with
  the clients on 6-7), for `point` read-only, `point` beside writers,
  and `insert1`.
- **M3** - where the cycles are: a sampled profile of the server at the
  clean cell, attributed to `frames_latch_`'s lock and unlock and to
  `PageLatch`'s acquire and release.
- **M4** - root sharing apart from table sharing: every session on one
  relation against each session on its own relation. The first shares a
  root's word and `pins`; both share `frames_latch_`.
- **M5** - the words' cost with no sharing: one core armed against one
  core unarmed, if a release build can arm a one-core store
  (`latch_forced_`, `device_page_store.hpp:1545`; S0 reads whether one
  can).

**Tests CW-S3 owes.** A contract suite, latched against optimistic,
byte-for-byte; a seam between a reader's copy and its validation, as
writeback's has between its copy and its clean, so a write, an eviction
and a slot reuse can each be placed there; `btree_race_test.cpp` extended
to the optimistic descent; the fallback reached and counted. The
simulator runs unarmed and proves nothing here; the armed two-core rigs
carry it.

## 6. What CW does not do

- No whole-page clone and no undo applied to a page.
- No row-level version, and no change to the Keystone word, the MVCC
  header or any page layout.
- No optimistic writer: every write keeps its exclusive latch, its pin
  and its logging under the hold.
- No latch coupling, B-link or merge; the descent's coverage check
  stands.
- No change to visibility or to the lock family.
- No fairness queue for the page latch (CW-Q8).
- No index-only scan.
- Nothing at `cores = 1`.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-17

Written on worktree `cw-optimistic-page-read` from `7aa75760`
(`v3.4.11-28-g7aa75760`), documents only. The letter CW is CLA's
proposal. No review was run and no suite was executed: the commit
changes no build input. Pushed to `main` with `--no-verify` on the
operator's word given while the order was being written
(`raft-marks-2026-11-17.md` §2).

### Opened 2026-11-22

Opened at `56b44964` (`v3.5.0-90-g56b44964`) on branch
`worktree-cw-optimistic-page-read`, worktree `sync-stale-records`, on the
operator's standing go-ahead (`/go-ahead-achieving-milestone CW`,
`raft-marks-2026-11-22.md` §11). CW-Q0..Q9 adopted (a) on that go-ahead,
CLA's proposals and not the operator's mark. The worktree is the session's
own: a second worktree cannot be entered from inside one, so the branch is
made in place, as DM's was.
