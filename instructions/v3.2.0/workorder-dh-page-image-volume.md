# Work order DH - the page image's price: what an owed image costs where pages are written often, and which lever buys it back, measured before anything is built

Status: **OPEN since 2026-11-20; DH-S0 done.** Written on `main` at `673f025d`
(`v3.4.14-30-g673f025d`) on the operator's word of 2026-11-20
(`raft-marks-2026-11-20.md` §1). The letter DH is CLA's proposal.
**Opened on the operator's standing go-ahead of 2026-11-20
(go-ahead-achieving-milestone, `raft-marks-2026-11-20.md` §3)**, on worktree
`dh-page-image-volume` from `f4138860` (`v3.4.15-1-gf4138860`): every DH-Q
item below is CLA's proposal adopted under that word, and none is the
operator's own mark (§4.1). **Exploratory by CLA's proposal** (DH-Q0 (a)):
no lever was built before DH-S0 priced it.

DH-S0 (§7) read the four census questions and measured the price
(`bench/v3.4.15/results-dh-s0-premise-v3.4.15-1-gf4138860.md`). Its
verdicts: **L1 and L4 are dropped** (the hole is 0 to 0.8 % of an image where
the price is paid; under pool pressure the image is owed by the fault-in,
which no drain order changes), **L3 and L2 are taken up** (§2.1), L5 stays
unbuilt. Every `[source-read]` claim in §1.1 to §1.6 was read at `673f025d`;
§1.7's at `f4138860`.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[quiet-wrong]` where a wrong choice turns a
correct answer into a wrong one with no error.

Related:
- `instructions/v3.2.0/workorder-db-torn-page-healing.md` (DB-Q1 (a) as
  revised, DB-Q4, DB-Q5, DB-R1..R6, §6 "What DB does not do");
- `bench/v3.4.14/results-db-s0-premise-v3.4.14-2-g734484ca.md` (the
  projection), `bench/v3.4.14/results-db-close-v3.4.14-5-gec057c06.md`
  (the price);
- `bench/v3.4.14/summary-scenarios-v3.4.14.md` (scenarios 1 to 3 at the
  tag, which predates DB);
- `docs/spec/wal.md` §10 (torn-page protection), §11 (checkpointing),
  §12 (recovery); `docs/spec/page.md` §8, §10; `docs/spec/eviction.md` §4;
  `docs/spec/backup.md` B5;
- `instructions/v3.2.0/workorder-dg-wal-segment-body.md` (the other open
  order on log volume's price; not amended here).

## 0. What DH is

`[operator]` *"Since the torn page heal came in, the overhead has grown a
lot."* and *"Write a work order for that as an additional milestone."*
(given in Korean; recorded in English.)

`[measured]` What the word rests on is DB's own close
(`results-db-close-v3.4.14-5-gec057c06.md`, interleaved A/B, `734484ca`
against `ec057c06`, `group`, 4 clients):

| cell | cores | throughput B vs A | log a statement A -> B |
|---|---|---|---|
| point SELECT, 10K (control) | 1, 4 | +0.3 %, -0.1 % | 0 -> 0 B |
| UPDATE 200 / 1K / 10K | 1 | -1.0 % to +0.0 % (floor about 4 %) | 249 -> 251 to 274 B |
| UPDATE 100K | 1 | -2.0 % (inside the floor) | 249 -> 492 B |
| **UPDATE 100K** | 4 | **-11.7 %** | 250 -> 1,228 B |
| **UPDATE 1M, pool 2,048 frames** | 1 | **-25.3 %** | 249 -> 6,924 B |

DB-Q5 set no throughput bar and left the deltas to the operator. The
operator's word is that ruling's first half: the price is too high where it
is paid. DH is the second half: **find what the price is on the workloads
the engine is judged by, name the levers that lower it, price each without
building it, and build only what the price supports.**

`[design]` DH does not reopen the protection. A torn page of a logged class
stays healed (DB-R1, DB-R4), and **no lever in DH may trade the heal's
exactness for volume** (DH-R1, `[quiet-wrong]`).

**What CLA told the operator, corrected.** In the conversation CLA ranked
"leave the page's free space out of the image" first and called it the
lowest-risk lever. The tree does not support the ranking as said: the
common page header carries no free-space field (§1.3), so the hole is a
per-type reading, the payload is the page verbatim by a `static_assert`
(§1.2), and DB named both elision and compression a payload change it
would not make (DB §6, DB-R5). The ranking below is therefore open until
DH-S0 measures how much of an image is hole at all.

## 1. The tree as read

### 1.1 Where an image is owed and paid `[source-read]`

- A frame owes an image from its fault-in
  (`src/storage/device_page_store.cpp:809`, `owes_image = why ==
  Inserting::kFault`) and from every writeback copy (`:2163`, set at the
  copy, not the clean).
- The write fetch pays it: `FetchPinned` for write calls `LogOwedImage`
  once the page is held exclusive and pinned (`:2496-2500`);
  `LogOwedImage` clears the bit under the structure latch, drops the debt
  of a class no record describes, appends through the gate and stamps the
  page (`:2505-2533`).
- The count is `fetch_images_logged_` (`:2529`), surfaced as
  `pool_fetch_images` (`src/server/command_dispatcher.cpp:2283`). **No
  counter of image bytes exists**; the close derived bytes as count x
  8,192.
- Images are off through the mount (`:2506`, `fetch_images_`).

So the number of images is the number of page copies that are followed by
a write of the same page: one per (page, writeback) pair in the hot set.

### 1.2 What an image is `[source-read]`

- The payload is the page verbatim, 8,192 bytes, no prefix:
  `include/kds/wal/payload.hpp:402-410` (`kFullPageImagePayloadSize =
  kPageSize`), asserted again at
  `include/kds/storage/log_page_image.hpp:39`.
- It is appended at `kNoTxnId` by `WalManager::LogPageImage`
  (`include/kds/wal/manager.hpp:308-312`).
- Redo decodes it and lays it over the page through the same gates as any
  record (`src/wal/redo.cpp:532-537`); a `PAGE_INIT` heals as an image
  does (`:396-410`).
- No compression library is linked (`CMakeLists.txt`: none of lz4, zstd,
  zlib, snappy).

### 1.3 Where a page's free space is `[source-read]`

The common header is 32 bytes - type, format version, flags, checksum,
`page_lsn`, `page_epoch`, `owner_oid` (`include/kds/storage/page_header.hpp:39-46`,
`:68`) - and names no free region. Each page type keeps its own
(`docs/spec/page.md:119`: a slotted page's `lower`/`upper`). An elision is
therefore a per-type function over every imaged class of DB-Q2's
whitelist, or a type-blind scan for zero runs. Whether a freed region is
zeroed in each type was not read; DH-S0 reads it.

### 1.4 Who writes pages back, and how often `[source-read]`, `[measured]`

- Every core submits its own cadence checkpoint
  (`src/server/core_runtime.cpp:458`, `src/server/expeditor.cpp:2115`);
  `wal::CheckpointGate` admits one run at a time for the instance
  (`src/wal/checkpointer.cpp:460`); every run walks the instance's one
  pool (`docs/spec/wal.md` §11).
- `[measured]` So at `cores = 4` the hot set is written about four times
  as often: 20 runs in 25 s against 5, and UPDATE 100K goes from 124 to
  543 pages a second with nearly the same statement rate
  (`results-db-s0-premise-...md`, finding 2). **Image volume scales with
  `cores / checkpoint_interval_ms` times the hot set.**
- `[measured]` Where the table exceeds the pool, the eviction drain
  writes 10.7 times the checkpoint's rate (finding 3), and the close
  found the images equal to the pages written to within 10 in 43,000:
  0.81 images a statement.

### 1.5 How the volume becomes a cost `[measured]`

From the close: server CPU moves only at the pressure cell (+11.6 us a
statement, about 14 us an image); the log grows by exactly the image
bytes; **throughput follows the log only through the sync's length** (the
sync is 46 % of a one-core statement and 82 % at four cores); the ring
never filled (`wal_ring_full` 0 in every cell); recovery time did not
move; the WAL directory doubled at 100K rows (64 to 128 MiB).

### 1.6 What is not measured

- **Any scenario.** Scenarios 1 to 3 were measured at the tag `35086008`,
  which predates DB (`summary-scenarios-v3.4.14.md`); the price of an
  image on the booking path, at `cores = 8`, is unknown.
- `strict`, a `cores = 4` pressure cell, and a log that replays many
  images (all cut at DB's close).
- What an image holds: the hole's share, and what a compressor makes of
  it.
- The cost at more than 4 cores, where §1.4's factor is larger.

### 1.7 What DH-S0 read `[source-read]` at `f4138860`

Four readings, each made for one lever or rule. `dps` is
`src/storage/device_page_store.cpp`.

**The hole (L1).** Nine page types are imaged (`DevicePageStore::ImagedClass`,
`dps:2535-2563`): heap, btree leaf and internal, undo, var-heap, index leaf
and internal, Bound Cabin, anchor. Every one has a free region computable
from its own bytes (a slotted page's `[lower, upper)`,
`include/kds/storage/heap/heap_page.hpp:59-75`; a fixed-stride array's tail
past its count). **Nothing reads a hole byte and acts on it.** Six types
always hold zeros there (a fresh page is zero-filled,
`src/storage/page_header.cpp:64`; a created frame is too, `dps:817`); three
can hold stale bytes: a heap or btree leaf after `UnInsertTuple`
(`src/storage/heap/heap_page.cpp:180-202`) and an index page after
`SplitInto` (`src/storage/index/index_page.cpp:239-243`, `:475-489`). The
page checksum covers all 8,192 bytes and is stamped at the writeback copy
(`dps:2168`); redo does not verify an image's checksum (`src/wal/redo.cpp:466`,
`:500-505`). So an elision that drops only **zero** bytes restores the page
byte for byte with no per-type proof, and one that drops a type's free
region changes stale bytes on three types.

**The readers (DH-R3).** Redo is the only decoder of an image's payload
(`src/wal/redo.cpp:532-537`). Analysis, the high-water scan, the log
scanner, backup's segment copy, restore's verify, PITR's cut, the archive,
the simulator and the fuzzers read the envelope or copy bytes
(`src/wal/analysis.cpp:106-111`, `src/server/backup.cpp:386-394`,
`src/server/restore.cpp:285-302`, `:505-513`, `sim/instance.cpp:430-466`).
The envelope takes a payload of any length that fits a segment and the
ring; the payload's exact length is **not** stored (a decoded record's
payload is the aligned tail, `src/wal/record.cpp:160-166`), so a
variable-length image carries its own length. **A changed kind-10 payload
is the `[quiet-wrong]` shape**: an older binary refuses a payload shorter
than 8,192 bytes by name and silently lays the first 8,192 bytes of a
longer one (`src/wal/payload.cpp:487-491`). **A new record kind is the loud
shape**: redo refuses a kind it has no applier for, by name
(`src/wal/redo.cpp:557-566`), the next free number is 35
(`include/kds/wal/record.hpp:247`), and no superblock or segment version
moves. `docs/spec/wal.md` §5.2 calls either a format-version event.

**The cadence (L3).** One run, in order: the gate; the submitting core's
own active table and the instance pool's dirty table into
`CHECKPOINT_BEGIN`; the redo start (the minimum recLSN, floored at the
begin); a write of every snapshot page; `CHECKPOINT_END`; a second write of
every page dirty by then, the maps and the data sync; the anchor
(`src/wal/checkpointer.cpp:204-256`, `:541-667`). **Each run copies in two
passes**, so a page written continuously is copied up to twice a run.
Recovery learns a transaction that wrote nothing in range from its own
core's `CHECKPOINT_BEGIN`, from every begin record in range
(`src/wal/analysis.cpp:156-168`), and **what keeps each core's begin in
range is the anchor's fold** - the minimum over each core's latest
contributed redo start (`src/server/superblock_checkpoint_anchor.cpp:38-63`)
- not how often the core runs. So:

- *the cadence as the instance's period* holds for correctness at any
  per-core age; its price is a fold up to `cores` periods old, which is the
  on-disk redo start, the log recycle, the dropped-relation reclaim bound,
  `BACKUP START`'s start and the assertion replay start;
- *a records-only run* does not hold as §2 worded it. It owes four things
  the writeback gives a run today: `CHECKPOINT_END` is refused over an
  unflushed snapshot (`src/wal/checkpointer.cpp:350`); a record another
  core appended between the snapshot and the begin is covered by the
  second pass as well as the fold; a page another writeback wrote and did
  not sync is made durable by the run's data sync; and the unlogged free
  map must follow every dirty page before page 0 moves (`dps:2446-2462`).

**The drain (L4).** The drain is CLOCK order with no age or LSN ordering
(`dps:3195-3237`); a written frame stays resident and clean and is
reclaimed on the hand's next visit. **A fault-in owes an image whatever the
drain did** (`dps:809`), and the write fetch that faulted pays it
(`dps:2496-2500`): under uniform updates over a table three times the pool
about two write fetches in three fault, so 0.8 images a statement is the
workload's, and a drain policy can remove only the copies of a frame that
stays resident between them - the checkpoint's ground, L3's.

## 2. The levers `[design]`

None was built or measured when this section was written; each names what
it would owe. **§1.7 and §2.1 supersede it where they differ**: what
DH-S0 read and measured, and which levers stand.

**L0 - nothing.** The price stands; `known-gaps.md` and the manual say
where it is paid and how to size the pool and the cadence around it.

**L1 - the hole left out.** The image carries the page less its free
region; redo rebuilds the page with the region zeroed. Lowers bytes an
image, not images. Owes: a hole function per imaged type or a zero-run
scan (§1.3); proof that the region is dead bytes in every type, or the
heal restores a page that differs from the one written (`[quiet-wrong]`,
DH-R1); a payload that is no longer `kPageSize` - **a log format event**
(§3, DH-R3). Worth nothing on a full leaf of fixed-length rows; DH-S0
says how full the imaged pages are.

**L2 - the image compressed.** Lowers bytes an image at a CPU price on
the write fetch, which holds the page exclusive. Owes: a dependency the
tree does not have (§1.2), a format event as L1, a bound on the
compressor's worst case, and its own CPU against the 14 us an image costs
today. Composes with L1.

**L3 - the instance's writeback rate made independent of `cores`.** The
factor of §1.4 is the per-core cadence, not the protection. Candidates:
the cadence read as the instance's period, each core's turn coming every
`cores` periods; or a run that follows another within one period logs its
begin and end records and writes nothing back. Lowers images, costs
nothing on the statement path, touches no format. **`[quiet-wrong]`**:
recovery needs every live transaction's undo head in a `CHECKPOINT_BEGIN`
inside the replay range (`wal.md` §11), and the anchor must never pass a
page a run did not write; DH-S0 reads both against the checkpointer
before L3 is a candidate at all (read: §1.7; the first candidate holds,
the second does not). It also lengthens the replay range and
what the log keeps.

**L4 - fewer copies under pool pressure.** In the eviction regime every
modification is the first after a copy. A drain that prefers frames not
re-dirtied recently, or batches so a page is copied once per several
modifications, lowers images there. Owes: a reading of the drain's
selection (`eviction.md` §4) and a measured premise; CLA has read neither
(read since: §1.7; the lever is dropped).

**L5 - a switch (DB-Q4 (b)).** Images off for a data device that writes
8 KiB atomically. Removes the whole price and the protection with it: a
way to be wrong by configuration. **Not CLA's proposal**; listed because
it is the only lever that returns the pressure cell to A, and the
operator may rule on it with the numbers in hand.

Not a lever in DH: moving the image to the device write (DB-Q1 (b)).
DB-S0 priced its five obligations; DH does not reopen the placement.

### 2.1 The levers after DH-S0 `[measured]`, `[source-read]`

`bench/v3.4.15/results-dh-s0-premise-v3.4.15-1-gf4138860.md` (S0) and
`bench/v3.4.15/results-dh-s1-packed-image-spike-v3.4.15-1-gf4138860.md`
(the spike). Within each file one binary serves every arm (two binaries in
all, each hashed in its file): A0 is main with the image debt never paid, B
is main (DH-Q8). The 1-core rows are the spike's; the others are S0's.

| cell | B against A0 | what L3's bound removes | what a packed image recovers |
|---|---|---|---|
| 100K-row updates, 4 cores | -8.2 % (floor 0.5 to 1.3 %; the spike's three rounds read -5.2 %, or -10.8 % without an A0 outlier, floor 6.7 to 19.5 %) | 79 % of the images | not separable from the floor |
| 100K-row updates, 8 cores | -12.7 % (single run) | 89 % of the images | 43 % of the gap, single run |
| 100K-row updates, 1 core | -5.9 % (floor 0.2 to 0.5 %) | nothing, by construction | 44 % of the gap |
| 1M rows, pool 2,048, 1 core | -28.3 % (floor 2.2 to 3.2 %) | nothing | 44 % (LZ4-class), 55 % (zstd-class) |
| 1M rows, pool 2,048, 4 cores | +5.9 % (single run, unexplained) | 3.5 % of the images | not run |
| scenario 2, every shape measured | -0.2 to -3.2 %, inside the floor where one exists | - | - |

- **L0** stands for scenario 2: the booking path does not pay a price the
  floor can see.
- **L1 is dropped** (DH-R7). On a leaf full of fixed-length rows, which is
  95 to 100 % of the images where the price is paid, the longest zero run
  is 14 to 23 bytes: 0 to 0.8 % of the image. The zero bytes of such a
  page (70 %) are the high bytes of small integers, scattered, not a free
  region.
- **L2 is taken up**, as a packed image (DH-Q11, DH-Q12). A stand-in that
  logs a shortened image and burns the packer's CPU recovers 44 to 55 % of
  the price in the two cells no other lever touches, outside the floor in
  both. No arm reaches A0: part of an image's cost is the record, not its
  bytes.
- **L3 is taken up**, as the instance's cadence (DH-Q10). Its
  configuration bound removes 79 to 89 % of the images where the hot set
  fits the pool; what that returns in throughput S0 could not establish
  (a longer warm-up alone moved A0 by 5.6 %), and the close measures it on
  the built lever.
- **L4 is dropped** (§1.7): under pool pressure the image is the
  fault-in's, and where frames stay resident the copies are the
  checkpoint's, which is L3.
- **L5 is not built** (DH-Q5 (a)).

**Not a lever in DH, recorded for the operator:** under pool pressure the
only way to log fewer images is to condition the fault-in's debt on an
image of the page already standing in the replay range. That is DB-Q1's
placement, which §6 leaves alone, and DB's own reading found a
`page_lsn` threshold unsound
(`workorder-db-torn-page-healing.md`, the BR finding). DH does not
propose it.

## 3. Rules

Binding once DH opens.

- **DH-R1 - the heal stays exact** (DB-R4). A page restored from a
  shortened or compressed image answers what the acknowledged history
  says, and recovery run twice changes nothing. `[quiet-wrong]`
- **DH-R2 - an image that cannot be decoded is refused, never guessed**:
  a bad length, a failed decompression or a hole outside the page ends
  recovery by name.
- **DH-R3 - a format event is said, not slipped in.** If L1 or L2 is
  built, DB-R5 no longer holds: the order states what an older binary
  does with a newer log and a newer binary with an older one, and backup,
  restore and PITR read both. `[quiet-wrong]` for a reader that takes a
  short payload for a whole page.
- **DH-R4 - one core pays nothing.** No lever moves a `cores = 1`
  standard cell outside the noise floor (the standing zero-overhead
  rule).
- **DH-R5 - no constant without a measurement.** A compression level, a
  hole threshold, a cadence factor is DH-S0's number or is marked
  provisional where it is set.
- **DH-R6 - the volume stays visible** (DB-R6): image bytes are counted
  as logged, not derived as count x 8,192, before any lever changes
  them.
- **DH-R7 - nothing is built for a lever DH-S0 prices under the floor.**

## 4. Items for the operator (DH-Q), CLA's proposals

(a) is CLA's proposal in each.

| | item | (a) | other |
|---|---|---|---|
| DH-Q0 | The order's standing. | **exploratory**: DH-S0 is census and measurement only, and it may close DH with L0 | (b) a build order: the operator names the lever now; (c) no order |
| DH-Q1 | What "the price" is judged on. | **scenario 2 first** (the booking path at `cores` 1 and 8, `group` and `strict`, 1 and 8 bookers), then DB's two bold cells; scenario 1's `result-insert` and load phases as the insert shape | (b) DB's cells alone; (c) a workload the operator names |
| DH-Q2 | The bar. | **named by the operator after DH-S0**, with the numbers; until then DH-R7 is the only gate. CLA proposes none: the one prior bar on this shape is DB-Q5's "no bar" | (b) a bar now, e.g. the two bold cells brought within a stated percentage of A `[provisional]` |
| DH-Q3 `[quiet-wrong]` | Whether a format event is admissible (L1, L2). | **yes, if DH-S0 shows either recovers more than L3 alone**, under DH-R3 | (b) no: DH is held to DB-R5, and only L0, L3 and L4 are candidates |
| DH-Q4 `[quiet-wrong]` | L3. | **a candidate only after the census of §2 L3**; if the census finds the begin-record premise does not hold, L3 is dropped and said so | (b) built first, as the lever that touches no format |
| DH-Q5 | L5, the switch. | **not built** (DB-Q4 (a) stands) | (b) built, default on, with the manual naming the devices it is safe to turn off for |
| DH-Q6 | A new dependency for L2. | **none added in DH-S0**: the premise is measured with a compressor outside the server, in the tool | (b) vendored now |
| DH-Q7 | The byte counter of DH-R6. | **added with the packed image** (DH-S1b), as the store's count of the record bytes each image took as logged; DH-S0 derived bytes as count x 8,192, which is exact while the payload is the page | (b) added in DH-S0 |
| DH-Q8 | DH-S0's A arm. | **main with the image debt never paid**: one binary, a measurement-only switch read at mount. The order's literal A (`12887a14`, the last commit before DB's merge) is not used: main since carries DA, which moved `cores = 8` by +13.5 %, so that pair would not isolate the image | (b) the pre-DB binary, as written |
| DH-Q9 | Which levers DH-S1 builds. | **L3, then L2**; L1 and L4 dropped under DH-R7; L5 unbuilt (§2.1) | (b) L3 alone, L2 to a later order after a census on wide and random columns; (c) nothing (L0) |
| DH-Q10 `[quiet-wrong]` | L3's shape. | **the cadence is the instance's**: every core ticks in phase and the gate admits one cadence run a period, the turn going to the core whose last run is oldest. A core's own run comes every `cores` periods, and the fold keeps its `CHECKPOINT_BEGIN` in the replay range at that age (§1.7) | (b) per-core timers at `cores` x interval: a tick that meets a run loses a whole cycle, and runs that outlast the interval starve the same cores for good (BI-S2's defect); (c) a records-only run: four obligations and an unproven single cover (§1.7); (d) the stagger kept and a spacing added: one run every interval x (1 + 1/`cores`), which is not what the setting says |
| DH-Q11 `[quiet-wrong]` | L2's record. | **a new record kind, 35, `PACKED_PAGE_IMAGE`**, written by the write fetch when the packed page is smaller than the page; kind 10 keeps its payload and meaning and is what is written otherwise. No superblock or segment version moves: a binary that predates DH refuses kind 35 at redo by name (`redo: no applier for record type`), and this binary reads every older log (DH-R3). **The house precedent is the other way** - kind 34 came with superblock 35 (CL-Q6 (a), `include/kds/wal/record.hpp`) - **and its reason does not reach kind 35**: an older build mounting a log that held no `ASSERT_PENDING` would have left a loser counted, a wrong answer with nothing to refuse it; an older build mounting a log that holds no packed image has lost nothing, since an image below the replay range is never read | (b) kind 10's payload changed: an older binary silently lays the first 8,192 bytes of a longer payload, so it needs a superblock bump, which refuses every older volume and backup and owes DA-S3; (c) segment format version 3 |
| DH-Q12 | L2's packer. | **zero suppression by 8-byte word, no dependency**: a bitmap of the page's nonzero words, then per nonzero word a mask byte and its nonzero bytes. On DH-S0's samples 0.40 to 0.42 of a full leaf, 0.36 of scenario 2's leaves, 0.34 to 0.36 of an undo page, at 13 to 15 us a page unoptimised; exact by construction (only zero bytes are left out) and bounded (a page that does not shrink is logged verbatim) | (b) LZ4 vendored: 0.31 to 0.42, 9 to 13 us, a dependency (DH-Q6); (c) zstd: 0.13 to 0.26 at 26 to 34 us - the spike's zstd-class arm was no better than the LZ4-class one at 4 and 8 cores and 11 points better under pool pressure |
| DH-Q13 | Which images are packed. | **the write fetch's owed image only** (the subject of DH). The whole-page records a DDL, an index build and an assertion build log through `LogFullPageImage`, and the pages a `BTREE_SPLIT` carries, stay verbatim | (b) every whole-page payload |

### 4.1 Marks

**Every mark below is CLA's proposal adopted on the operator's standing
go-ahead of 2026-11-20 (go-ahead-achieving-milestone); none is the
operator's own per-item mark** (`raft-marks-2026-11-20.md` §3). Each is
open to the operator's own mark.

| item | adopted | note |
|---|---|---|
| DH-Q0 | (a) | exploratory: nothing was built before DH-S0 priced it |
| DH-Q1 | (a), cut to the budget | scenario 2 at four shapes and DB's two cells ran; scenario 1 was cut, the `insert` cell standing in for its insert shape (S0's file says what else) |
| DH-Q2 | (a) | **no bar is set**: the operator names one with the close's numbers; DH-R7 is the only gate until then |
| DH-Q3 `[quiet-wrong]` | (a), premise re-read | "recovers more than L3 alone" was shown at S0 on bytes only, so the spike measured throughput before the format event was admitted: 44 to 55 % of the price in the pressure cell and 44 % at one core, where L3 removes nothing. The spike is a stand-in and some of its runs had another session's server beside them (§7); the close measures the built packer |
| DH-Q4 `[quiet-wrong]` | (a), premise re-read | the census was made (§1.7) and CLA read the two sites the premise rests on (`src/wal/analysis.cpp:156-168`, `src/server/superblock_checkpoint_anchor.cpp:38-63`): it holds for the instance's period and not for a records-only run, which is dropped |
| DH-Q5 | (a) | no switch |
| DH-Q6 | (a) | and no dependency is added at all (DH-Q12 (a)) |
| DH-Q7 | (a) | |
| DH-Q8 | (a) | a revision of DH-S0's stage as written, recorded here and in the results file |
| DH-Q9 | (a) | |
| DH-Q10 `[quiet-wrong]` | (a) | the gate and the timers decide when a core runs, never what recovery reads: a wrong choice here costs replay range or a starved core, and the fold is what keeps it from costing an answer |
| DH-Q11 `[quiet-wrong]` | (a) | |
| DH-Q12 | (a) | |
| DH-Q13 | (a) | |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DH-S0 - census and premise** | No engine code but DH-R6's byte counter, if the operator admits it; else the tool derives bytes. **Read**: the hole of every imaged page type and whether freed bytes are zeroed (L1); every reader of an image - redo, backup, restore, PITR, the sim (DH-R3); the checkpointer's begin-record and anchor rules against L3's two candidates (DH-Q4); the drain's selection (L4). **Measure**, interleaved A/B on one host, A = the last commit on `main` before DB's merge, B = `main` (as run, A is main with the image debt never paid: DH-Q8): DH-Q1's cells - statements a second, p50/p99, server CPU and log bytes a statement, images a statement. **Measure, B only**: the same cells with `checkpoint_interval_ms` scaled by `cores` (L3's upper bound, configuration only); the images of each cell read out of its log by a tool - the hole's share, and the ratio and time an image of two off-the-shelf compressors; `strict`; a `cores = 4` pressure cell. The standing budget; PostgreSQL is not a twin for this shape. | a results file under `bench/`, each lever priced in image bytes and projected throughput, and every DH-Q back to the operator |
| **DH-S1a - the instance's cadence (L3)** | The gate admits one cadence run a period by rotation (`wal::CheckpointGate`), every core ticking in phase; `cores = 1` is first-come as before (DH-R4). The gate's tests; `wal.md` §11, `sched.md` §4 and the manual's `checkpoint_interval_ms` say whose period it is. | review, the full suite |
| **DH-S1b - the packed image (L2)** | Its premise first: the spike and the packer's ratio on S0's samples. Then record kind 35 and its codec (`wal/payload`), the write fetch packing or falling back to kind 10, redo's arm with the heal's whole-page path, DH-R6's byte counter, and the refusals of DH-R2. | review, the full suite |
| **DH-S2 - the proof** | DB-S2's corpus over the new image: a tear at every page write healed and reconciled; a short, corrupt or oversized image refused by name (DH-R2); an older log read by the new binary and the reverse, as DH-R3 states; restore and PITR over both; for L3, a crash between every pair of runs with a live transaction on each core. | review, the full suite, `scripts/sim.sh`, the `bs-nightly` cells named as run or not run |
| **DH-S3 - the close** | DH-S0's A/B repeated, B against the build; `wal.md` §10, §11, `page.md` §10, `backup.md` B5, `known-gaps.md`, the manual and `CLAUDE.md`'s WAL row amended; the milestone row flipped. | closed |

## 6. What DH does not do

- No change to which pages are imaged (DB-Q2) or where the image is
  logged (DB-Q1).
- No doublewrite file (`wal.md` §10).
- No protection of page 0, the maps or headerless pages.
- Nothing for the segment body's fill (DG) or the log's recycling bound.
- No claim against PostgreSQL: its full-page writes are priced on another
  cadence and no twin cell exists.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-20

On `main` at `673f025d`, documents only. The order rests on DB's two
results files and on a reading of the sites in §1; nothing was run. Open
with the operator: DH-Q0..Q6, and whether DH-S0 may add DH-R6's counter.

### DH-S0 - census and premise (2026-11-20)

On worktree `dh-page-image-volume`, from `f4138860`
(`v3.4.15-1-gf4138860`). No engine code: the census is §1.7, the
measurement `bench/v3.4.15/results-dh-s0-premise-v3.4.15-1-gf4138860.md`
(run by `ck-tester`; drivers `tools/dh_s0.py`, `tools/dh_image_census.py`,
`tools/dh_s0_report.py`, `tools/dh_s0_project.py`).

**What it found.**

1. **The price is the sync's length.** B against A0: -8.2 % at 100K rows and
   four cores (floor 0.5 to 1.3 %), -12.7 % at eight (single run), -28.1 %
   with a table three times the pool at one core (single run; the spike
   repeated it at -28.3 %, floor 2.2 to 3.2 %), -11.2 % under `strict`
   (single run). Server CPU is 15.6 to 17.9 us an image, 6 % of the loss
   where the loss is largest.
2. **Scenario 2 pays nothing the floor can see**: -1.8 % (8 cores, `group`;
   floors 1.6 to 2.8 %), -0.2 % (1 core; floors 0.7 to 0.9 %), -0.6 % and
   -3.2 % as single runs under `strict`. A booking's log is 20 % larger at
   eight cores and the image is too small a part of a 2.7 ms booking to
   move it.
3. **One core is not free** (DH-R4's reference): 100K rows at one core
   read -5.5 % as a single run, and the spike's two rounds put it at
   -5.9 % with a floor of 0.2 to 0.5 %. DB's close read the same cell at
   -2.0 % inside a 4 % floor.
4. **The hole is not where the bytes are** (L1), and **a drain order does
   not lower the count under pressure** (L4): §1.7, §2.1.
5. **A compressor shrinks these images to 0.31 to 0.42 of a page (LZ4) or
   0.13 to 0.26 (zstd-1)**, the ratio coming from fixed-length rows of
   small integers; scenario 2's mix reaches 0.31 and 0.18.
6. **L3's bound removes 79 % of the images at four cores and 89 % at
   eight**, and 3.5 % under pool pressure. Its throughput was not
   established: L's longer warm-up is a confound (12.5 s of warm-up alone
   cost A0 5.6 % in the one cell with a control).
7. **Unexplained, left open:** the pressure cell at four cores reads
   +5.9 % in B's favour (single runs); and what in a longer warm-up costs
   A0 5.6 %.

**Cut.** Scenario 1; the 200, 1K and 10K row sweep; a second round of
every micro cell but one; L under `strict`; a steady-state L run. Build
1.3 min, runs about 16 min, the census re-runs 2 min; no quiet-gate hold.
The suite was not executed: the stage changed no build input.

**Review** (`critics-developer`, the stage's documents and tools): see
"Review" below.

### DH-S1b's premise - the packed image priced before it is built (2026-11-20)

Run ahead of DH-S1a because it decides whether DH-S1b exists.

- **The spike**
  (`bench/v3.4.15/results-dh-s1-packed-image-spike-v3.4.15-1-gf4138860.md`,
  `ck-tester`): the image record shortened to a packed image's size, with
  the packer's CPU burnt under the page's hold. It prices the sync's
  volume and a fixed CPU cost; it is not a packer, and a log it writes
  cannot be recovered. Recovered of the B-to-A0 gap: **44 % (LZ4-class)
  and 55 % (zstd-class) in the pressure cell, 44 % at one core and 100K
  rows, each outside its floor**; 43 % and 33 % at eight cores as single
  runs; not separable from the floor at four cores. No arm reaches A0.
  About 14 to 15 minutes of its 20, plus a 3.4-minute quiet-gate hold;
  ten runs were contaminated by another session's compile, discarded and
  run again. **Not clean, and kept**: another session's server (50 to
  73 % of one CPU) ran beside five of the pressure cell's eight runs and
  beside two of the four-core cell's, and a linker beside one more (the
  results file names the runs). The pressure cell's two rounds agree
  inside the stated floor and its B reads -28.3 % against DH-S0's
  -28.1 %, so the spike is read as what it was run for - whether a packed
  image is worth building - and no more: **the close measures the built
  packer, and that number replaces this one.**
- **The packer** (`bench/v3.4.15/archive/dh-s1-packed-image-spike-v3.4.15-1-gf4138860/zs_premise.cpp`,
  its output `zs_premise.out` beside it; run over S0's saved image
  samples, about ten seconds, at a host load of 15, so its times are
  loose): zero suppression by 8-byte word packs **a full btree leaf to
  3,270 to 3,459 bytes (0.40 to 0.42 of a page)** in the four update
  cells, scenario 2's leaves to 2,964 (0.36), an undo page to 2,794 to
  2,956 (0.34 to 0.36) and 1,723 in the insert cell, heap and inner pages
  to 481 to 2,120. None of the 14,043 sampled images failed to shrink
  (largest 4,106 bytes). 13 to 15 us a page to pack a full leaf and 10 to
  12 us to unpack and compare, unoptimised. LZ4's median on the same
  cells' leaves is 2,540 to 3,440 bytes: the packer is 20 to 900 bytes an
  image behind it. **The saved sample is the first images by LSN of each
  page type, not an even stride** (the census tool is since fixed), so
  these are per-type figures and no cell's mix.

So DH-Q3 (a)'s premise holds on throughput where L3 does nothing, and
DH-Q12 (a) costs 20 to 900 bytes an image against LZ4 for no dependency.

### DH-S1a - the instance's cadence (2026-11-20)

On `dh-page-image-volume`, over `df0dcd70`. **Overhead not measured;
measured at the milestone's close.**

**Built** (DH-Q10 (a)). `checkpoint_interval_ms` is the instance's period:
one cadence run starts each period at any `cores`, the cores taking turns.

- `wal::CheckpointGate` (`include/kds/wal/checkpointer.hpp`): a cadence
  attempt is refused inside half a period of the last one the gate
  admitted - compared under the run flag, which every cadence entry stamps
  under - and "a core that asked recently" is two periods, where it was
  one. The rotation itself (the turn goes to the asking core whose last
  entry is oldest) is BI-S2's, unchanged.
- `src/server/core_runtime.cpp`: a peer's cadence is no longer started
  `core_id / cores` of a period late. Every core ticks in phase, so each
  period every core asks and the gate admits the one whose turn it is.
- `cores = 1` is first-come as before: the gate is rotated only above one
  core (DH-R4).
- Nothing in recovery, the checkpointer's run, the anchor or the log
  changed.

**What it costs**, stated in `wal.md` §11 and `known-gaps.md` (WAL): a
core's own run comes every `cores` periods, so the fold's oldest
contribution, and with it the replay range, the log kept from recycling,
the reclaim bound and a backup's start, is up to about `cores + 1` periods
old where it was about two.

**Tests.** `CheckpointGateCadenceTest`: one run a period with each core in
turn when a run is shorter than the skew between two cores' ticks; the
order of arrival does not choose the core; a tick that fires late does
not take another core's turn (the case a one-period window fails); a run
that outlasts the period costs turns, not a core; an attempt naming no
core is not spaced. Two `CheckpointGateRotationTest` cases re-pinned for
the wider window and the spacing.

**Suite**: 0 failed of 4,869 run (4,871 listed, 2 disabled, 2 skipped;
546 s) on the stage's tree before the review's edits. Those edits are two
test cases, one new one and comments in the gate's header; after them the
tree was rebuilt and the gate's, the checkpoint task's and the core
runtime's cadence cases run again, all passing. The full suite was not
run again for them; it runs again at DH-S1b.

### Review (`critics-developer`), per step

**DH-S0 and DH-S1b's premise** (documents, results files and tools; no
engine code). It checked every §1.7 source claim against the tree and
every quoted number against the raw output, and found the source reading
correct. Applied:

- five of the pressure cell's eight spike runs ran beside another
  session's server and the file called the cell clean: stated in the
  results file and above, the runs kept, the close's number to replace
  the spike's;
- the packer's figures had no archived source and its sample mixed page
  types (the saved sample is the first images by LSN of each type, not an
  even stride): the program now reports by page type and its output is
  archived beside it; the figures above are the per-type ones;
- a single-run cell printed as "outside the floor" by the spike's report
  tool; "13,043 sampled images" (14,043); "one binary serves every arm"
  across two files' rows; the four-core cell quoted with S0's floor only;
  a 96 % that is 95 %: corrected by the reviewer;
- sections written before DH-S0 that §1.7, §2.1 and DH-Q8 supersede now
  say so; DH-Q11 says why kind 35 needs no superblock where kind 34 did;
- units moved into every cell of both results files; the checkpoint-run
  columns, which counted by a per-core sequence, relabelled; dead and
  duplicated code in the drivers cut (`ck-tester`).

Rejected: **re-running the pressure cell.** The spike decides only
whether DH-S1b is built, the two rounds agree with each other and with
DH-S0, and the close measures the real packer on the same cell.

**DH-S1a.** No bug found in the gate: it traced the exclusion, the races
of the relaxed stamps (a stale read costs a turn, never a core), clock
readings out of order, a core that stops asking (the rest wait two
periods, where they waited one), start-up skew at a tiny interval (extra
runs, no starvation) and every consumer of a per-core run (each slows
with the fold; none breaks; recovery reads only the fold). Applied:

- the documents understated the replay range - the oldest core's redo
  start lies below its run by the age of the oldest dirty page, about a
  period more: `wal.md` §11, `known-gaps.md` and the gate's header now say
  so (the reviewer's edits, and one of its sentences reworded: after a
  mount the fold stays at the completion checkpoints, which is not the
  warm-up);
- `AnAskStampedAfterThisCoresReadingStillCounts` had become vacuous (the
  spacing refused what the turn was meant to): re-timed so the turn
  refuses it;
- nothing pinned the two-period window, and one case's comment claimed
  it did: `ATickThatFiresLateDoesNotTakeAnotherCoresTurn` added, the
  comment corrected;
- the header's BI paragraph still described the stagger and a one-period
  window in the present tense.

Rejected: **a test of the in-phase arming itself** (two reactors on one
rotated gate at a 20 ms period, counting runs by time). On a shared host
that is a timing test of two threads' timers, and a put-back stagger
would show as a rate of 1 / (1 + 1/`cores`) of the expected, inside what
load does to such a count. The gap is recorded in `known-gaps.md` as not
covered. Also left: merging the gate's two refusal exits (cosmetic).
