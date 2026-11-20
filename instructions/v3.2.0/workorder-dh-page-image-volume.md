# Work order DH - the page image's price: what an owed image costs where pages are written often, and which lever buys it back, measured before anything is built

Status: **WRITTEN 2026-11-20, not opened.** Written on `main` at `673f025d`
(`v3.4.14-30-g673f025d`) on the operator's word of 2026-11-20
(`raft-marks-2026-11-20.md` §1). The letter DH is CLA's proposal.
**Exploratory by CLA's proposal, not by the operator's word** (DH-Q0): the
word asked for an order on the overhead; that no lever is built before
DH-S0 prices it is CLA's.

No stage is started. DH waits for the operator's word and for the marks on
§4's items. Every `[source-read]` claim below was read at `673f025d`. No
suite was run: the order changes no build input.

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

## 2. The levers `[design]`

None is built or measured. Each names what it would owe.

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
before L3 is a candidate at all. It also lengthens the replay range and
what the log keeps.

**L4 - fewer copies under pool pressure.** In the eviction regime every
modification is the first after a copy. A drain that prefers frames not
re-dirtied recently, or batches so a page is copied once per several
modifications, lowers images there. Owes: a reading of the drain's
selection (`eviction.md` §4) and a measured premise; CLA has read neither.

**L5 - a switch (DB-Q4 (b)).** Images off for a data device that writes
8 KiB atomically. Removes the whole price and the protection with it: a
way to be wrong by configuration. **Not CLA's proposal**; listed because
it is the only lever that returns the pressure cell to A, and the
operator may rule on it with the numbers in hand.

Not a lever in DH: moving the image to the device write (DB-Q1 (b)).
DB-S0 priced its five obligations; DH does not reopen the placement.

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

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DH-S0 - census and premise** | No engine code but DH-R6's byte counter, if the operator admits it; else the tool derives bytes. **Read**: the hole of every imaged page type and whether freed bytes are zeroed (L1); every reader of an image - redo, backup, restore, PITR, the sim (DH-R3); the checkpointer's begin-record and anchor rules against L3's two candidates (DH-Q4); the drain's selection (L4). **Measure**, interleaved A/B on one host, A = the last commit on `main` before DB's merge, B = `main`: DH-Q1's cells - statements a second, p50/p99, server CPU and log bytes a statement, images a statement. **Measure, B only**: the same cells with `checkpoint_interval_ms` scaled by `cores` (L3's upper bound, configuration only); the images of each cell read out of its log by a tool - the hole's share, and the ratio and time an image of two off-the-shelf compressors; `strict`; a `cores = 4` pressure cell. The standing budget; PostgreSQL is not a twin for this shape. | a results file under `bench/`, each lever priced in image bytes and projected throughput, and every DH-Q back to the operator |
| **DH-S1 - the lever** | What the operator marks, under §3. | review, the full suite |
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
