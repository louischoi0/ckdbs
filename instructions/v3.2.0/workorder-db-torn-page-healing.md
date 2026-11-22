# Work order DB - a torn page healed, not refused: the full-page image the log owes

Status: **CLOSED 2026-11-19, and landed on `main` the same day at `480add9d` on the operator's word "push no verify"** (the merge's message: the hook skipped, the suite 0 failed of 4859 at `0b8bc9ce`, the same build inputs; **tagged `v3.4.15` at `1eeab8ce`**) (DB-S0..S3, §7; closing code commit `ec057c06`, `v3.4.14-5-gec057c06`; close measurement `bench/v3.4.14/results-db-close-v3.4.14-5-gec057c06.md`). **OPENED 2026-11-19** on the operator's standing go-ahead
(go-ahead-achieving-milestone, scheduled by the operator at 08:38 UTC for
12:38 UTC; `raft-marks-2026-11-19.md` §13), on worktree
`db-torn-page-healing` from `734484ca` (`v3.4.14-2-g734484ca`). **DB-Q1..Q6
adopted as CLA proposed, DB-Q1 (a) and DB-Q2 (a) as revised at DB-S0, and
DB-Q7 added at DB-S0 and adopted (a)** - each on the operator's standing
go-ahead of 2026-11-19 (go-ahead-achieving-milestone), none the operator's
own mark (§4). **DB-S0 done** (§7): the census corrected §1 and §2.1 in five
places, and the premise was measured
(`bench/v3.4.14/results-db-s0-premise-v3.4.14-2-g734484ca.md`).

Written 2026-11-18 on worktree `db-fpi-cadence-dc-for-update` from
`a4d2fb3b` (`v3.4.12-42-ga4d2fb3b`) on the operator's word of 2026-11-18
(`raft-marks-2026-11-18.md` §22). The `[source-read]` claims of §1-§2 were
read at `a4d2fb3b`; DB-S0's corrections (§7) were read at `734484ca`, and
where the two disagree §7 is the tree.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured.

Related: `docs/spec/wal.md` §8 (WAL-before-data), §10 (torn-page
protection), §11 (checkpointing), §12 (recovery), §16-4 (the torn-write
test the spec asks for); `docs/spec/page.md` §1 (page classes), §8, §10
(checksums); `docs/spec/eviction.md` §4 (writeback); `docs/spec/backup.md`
B5; `docs/spec/drop-table.md` (BF-R4); `docs/inflight/known-gaps.md` "Power
cuts (BS-S4)" and "Backup and restore (BR)";
`instructions/v3.0.0/workorder-br-backup-restore.md` (BR-R5 and §7, which
names this work as a successor); `instructions/v3.0.0/br-s0-census.md`
(H5); `instructions/v3.0.0/workorder-bs-test-hardening.md` (BS-S4, BS-Q5).

## 0. What DB is

`[operator]` *"Write the work order for item 1, the FPI cadence, first."* (given in
Korean; recorded in English). It is item 1 of a survey CLA gave in the same
conversation, of what the engine lacks as an OLTP engine for financial
systems; CLA ranked it first because it is the one item that bounds the
engine's correctness claim.

`[source-read]` What stands today: a page a power cut tore is detected by
its CRC32C at load and **refused**, at the mount, the reclaim or the
integrity sweep, and what it held is lost to the instance until a restore
(`docs/inflight/known-gaps.md:2223-2231`). The spec asks for detection and
healing both (`docs/spec/wal.md:240`, `docs/spec/page.md:166`). The
healing half of recovery is built and waits for images that nothing logs
(§1.2).

`[design]` DB makes one sentence true: **a logged page whose device write
was cut short is restored at the next mount, and recovery goes on from
there.** DB adds no record kind and no page format. It adds a caller of
the image writer the tree already has, and the tests that the heal is
exact.

**The one decision is where the image is logged (DB-Q1), and it is
`[quiet-wrong]`**: today a torn page is refused, and a heal that restores
the wrong bytes is served. Two placements are laid out in §2, each with
what it owes.

**What CLA told the operator, corrected twice.** In the conversation CLA
called this "a design the spec already adopted", to be built as written.
The tree says otherwise: BR tried the spec's rule for a backup window,
found no site to decide it at and a race with no mechanism to close, and
replaced it with a device write fence (§1.3). CLA then drafted this order
around an image at the device write, and said so to the operator. The
review of that draft found five obligations it had not stated, one of them
a wrong answer after a crash during recovery (§2.2), and pointed at a
single site for the spec's placement that BR's census did not consider
(§1.5). The proposal below is therefore the spec's placement, at that
site.

## 1. The tree as read

### 1.1 What a tear does today `[source-read]`

- **Detection.** A headered page's checksum is stamped at the flush and
  verified on every load from the device, never on a hit
  (`docs/spec/page.md:165`). A mismatch is `Corruption`.
- **At the mount.** Redo meets the page through `store.Get`; on
  `Corruption` it holds the page *poisoned* and skips its records
  (`src/wal/redo.cpp:401-409`, `:443-447`) until an image in the replay
  range recreates it over the allocated id (`PageStore::CreateOverTorn`,
  `redo.cpp:410`). A page still poisoned when the scan ends refuses the
  mount (`redo.cpp:572-577`).
- **Outside the mount.** A page no record in the replay range names is not
  read by redo; a tear there is met by whoever reads it next.
- **The sim asserts the refusal and only that** (BS-Q5 (a),
  `sim/loop.cpp:1691-1724`; `sim/instance.hpp:141`, `:160`): over seeds 1-5
  and page writes 1-40, 34 of 65 tears were refused and the rest left
  nothing a read reached or recovery rebuilt
  (`known-gaps.md:2229-2231`).
- **Backups.** Path B's copy cannot tear, because the copier reads through
  a device write fence (`docs/spec/backup.md:40-46`). Path D on a snapshot
  that does not freeze the filesystem is a crash image, and a torn page in
  it is named at `verify` and not repaired (`known-gaps.md:1281-1287`).

### 1.2 What is already built `[source-read]`

| piece | where | state |
|---|---|---|
| The record | `RecordType::kFullPageImage = 10` (`include/kds/wal/record.hpp:59`); payload one page (`include/kds/wal/payload.hpp:409`) | built |
| The writer | `storage::LogFullPageImage` - fetch the page, append its bytes, stamp the page with the record's LSN (`include/kds/storage/log_page_image.hpp:27-43`) | built; called at six sites on five paths, each a path with no delta record of its own: a var-heap link (`src/exec/wal_row_log.cpp:50`, `:94`), a Bound Cabin chain's old tail (`src/exec/assertion_build.cpp:170`), an index build's tree and root (`src/exec/index_ddl.cpp:170`, `:836`), a catalog chain's old tail (`src/catalog/catalog.cpp:357`) |
| The heal | the poison set, `CreateOverTorn`, the LSN gate applied to an image too, the page stamped with the image's LSN, `pages_healed` counted by what the image did (`src/wal/redo.cpp:400-481`) | built |
| The report | `recovery_pages_healed` in `SHOW META`'s recovery block (`src/server/command_dispatcher.cpp:2254`, `src/server/mount_recovery.cpp:55`, `:82`) | built |
| The cadence | "the first modification of a headered page after each checkpoint" (`docs/spec/wal.md:141`) | **not built** |

Redo applies a record to a page only if analysis entered the page as
dirty at or below the record's LSN (`redo.cpp:376-380`), and analysis
enters every page a record names. A split's `BTREE_SPLIT` carries whole
images of every page it writes (`log_page_image.hpp:45-111`). Undo pages
are declared exempt "on the merits": one is rebuilt from its `PAGE_INIT`
and its `UNDO_WRITE`s (`include/kds/txn/undo_log.hpp:62-65`) - which holds
only while that `PAGE_INIT` is in the replay range.

### 1.3 What BR found about the spec's rule `[source-read]`

BR-R5 was the spec's rule armed for a backup window: the first modification
of each headered page whose `page_lsn` is below an arm LSN logs an image
(`instructions/v3.0.0/workorder-br-backup-restore.md:116-134`). BR-S0's
census marked its premise false:

> There is no single decision site: every mutation appends, then stamps
> (`DevicePageStore::StampPageLsn`, ...), and the decision cannot be re-checked
> where the LSN is assigned - the stream latch is inner to the page latch
> and the stream cannot see a page (...)
> (`instructions/v3.0.0/br-s0-census.md:19`, H5; the elided citations name
> line numbers of BR's base, since moved)

BR-R5 names two conditions a threshold on `page_lsn` does not give
(`workorder-br-backup-restore.md:125-132`): (i) no mutation that read the
window clear may take an LSN at or above the arm LSN; (ii) a page dirtied
before the arming and written back afterwards with no later modification
gets no image. BR closed neither; it removed the need (`backup.md:46`),
and listed "the general FPI cadence ... armed per checkpoint" as a
successor (`workorder-br-backup-restore.md:381`).

The census looked for the site where a mutation logs. It did not look at
the site where a mutation takes its page (§1.5).

### 1.4 Where a page reaches the device `[source-read]`

**One function writes every frame: `DevicePageStore::WriteBack`**
(`src/storage/device_page_store.cpp:1930`). Its callers are the eviction
drain (`:2235`), `Flush` (`:2299`), `PersistPage` (`:2380`), `FlushPages`
(`:2387`) and the checkpoint's `WritePages`
(`include/kds/storage/page_store_checkpoint_target.hpp:47`), through
`WriteBackPages` (`include/kds/storage/device_page_store.hpp:530`).
`PersistPage`'s one caller is the transaction-id carve's write of page 0
(`src/server/expeditor.cpp:1392`). The store's other device writes are the
free-map and headerless-map regions (`:447`, `:573`, `:597`), which are
not frames and carry no record (`docs/spec/page.md:22`).

For each run of up to eight consecutive pages
(`device_page_store.hpp:395`) `WriteBack` claims the frames
(`Frame::writing`); copies each page under its own page latch, shared,
with its dirty generation and its checksum (`device_page_store.cpp:2086`,
`:2110-2124`); waits for the log to be durable up to the highest
`page_lsn` it copied, or returns that LSN to a caller that passed `defer`
(`:2157`, `:2161`); writes (`:2176`); and cleans only a frame whose
generation did not move since the copy - one written to meanwhile stays
dirty, its recLSN moved up to the lowest LSN stamped since the copy
(`:2209-2218`).

**A checkpoint's publish writes every page dirty at its collection,
waiting out other writebacks' claims, before it syncs the data file and
writes the anchor** (`src/wal/checkpointer.cpp:539-567`,
`device_page_store.cpp:2037-2075`; `wal.md` §11 step 3).

### 1.5 Where a page is taken for a write `[source-read]`

- **`PageStore::Get` is the fetch "for read or in-place mutation"**, and
  it takes the page latch exclusive
  (`include/kds/storage/page_store.hpp:264-271`).
- **An exclusive hold marks its frame dirty once it has the latch**, under
  the structure latch, and the code's own comment says "Every exclusive
  hold is a write fetch (`Get`, the creates)"
  (`device_page_store.cpp:2775-2789`). At that point the holder has the
  page to itself, has changed nothing yet, and the frame's state before
  the mark is known.
- **Three ways a frame is dirtied that are not an exclusive fetch of a
  logged page's bytes**: a stamp, which also moves the generation
  (`:1784-1815`); `MarkFrameDirty` (`:2872-2879`); and page 0, a headered
  frame whose content no record describes (`:2780-2781`).
- **A change is in its frame when its record is appended, or is made under
  a hold taken before the append and kept to the stamp** - the premise the
  writeback's clean already rests on (`:2198-2204`), with the paths that
  log after releasing their page named there. A record "logged after its
  page is released can be stamped after a higher one" (`:1804-1806`).

### 1.6 What is not measured

Nothing in the tree prices an image. The default checkpoint cadence is
5,000 ms (`manual/server/server.md:154`), the ring is 1 MiB
(`include/kds/wal/stream.hpp:146`) and an image is 8 KiB of it, so a
checkpoint step's 64 pages are half a ring if each is imaged at once.
Pages written are counted (`Tally::kCheckpointPagesWritten`,
`src/wal/checkpointer.cpp:272`, `:276`; `dirty_drained_`,
`device_page_store.cpp:2236`).

## 2. The two placements `[design]`

Both log one image per page per device write of it, so their log volume
is the same order. They differ in who logs it, and in what else has to be
true.

### 2.1 F - at the first exclusive fetch after a copy (DB-Q1 (a))

Each frame carries one bit, *owes an image*, set when the frame is loaded
from the device and each time a writeback copies it. When a task takes a
logged page exclusive and the bit is set, the store logs the page as it
stands - **before** the holder changes it - by the existing writer, which
stamps it, and clears the bit. The holder's records follow in the log.

This is the spec's placement, with "after each checkpoint" replaced by
"after each copy for a write". A checkpoint copies every dirty page, so
the two agree when nothing else writes back; the bit also covers eviction's
writes, which a per-checkpoint rule does not.

**Why it heals.** A torn page is restored to what the device held before
the write that tore it, and every later change of the page is a record
after the image, in order.

**Why the image is in the replay range.** Let checkpoint K's anchor be the
durable one. A write that can be torn was issued after K's publish synced
the data file. If the page was dirty at K's collection, the publish copied
it (§1.4), which set the bit; the page's next exclusive fetch is after
that copy, so its image is above K's beginning, and K's redo start is not.
If the page was clean at K's collection, its first exclusive fetch after
is too.

**Why BR's two conditions do not arise.** The decision reads a frame's
bit under the frame's own latch, not a threshold on LSNs, so (i) has no
window. (ii) is a page written back with no later change: its write after
K carries the bytes of its write before, apart from a stamp in the header,
and a tear of identical bytes is not a tear. DB-S0 confirms the header is
within one device sector.

**What F owes** (each read at DB-S0):

- **Redo logs no image.** Redo's own `Get` would otherwise append a page
  as it stands mid-replay, at the log's end, behind that page's own later
  records - which a later mount would then skip. A page redo writes back
  is covered by the image its pre-crash dirtying logged. `[quiet-wrong]`
- **Every change of a logged page's bytes follows an exclusive fetch**
  (§1.5's three exceptions sorted: which are logged content).
- **A class test at the fetch**: page 0 and frames whose content no record
  describes log nothing.
- **A fetch that changes nothing still logs** if the bit is set. DB-S0
  lists the callers of `Get` that only read.
- **The append is on the statement path**, 8 KiB under the page's
  exclusive hold, inside a mutation's no-refuse window when the page is
  not the first it takes. A refused append is then what a refused delta is
  today (`src/wal/manager.cpp:383-406`).

### 2.2 W - at the device write (DB-Q1 (b))

`WriteBack` appends each page's copy as an image under the page's share,
gates the write on that image's durability, and writes the same bytes.
The image is the write, so the heal is exactly an uncut write.

**What W owes**, as the review of CLA's first draft found:

- **A recovery hole.** A page redo's drain writes back would be imaged at
  the log's end, as it stood mid-replay. After a second crash the page's
  earlier records are skipped as poisoned and the image restores a state
  older than the log holds: changes lost, silently. Writes during redo
  must be exempt, and a tear of one stays a refusal. `[quiet-wrong]`
- **BF-R4 breaks.** A dropped relation's page is freed once the durable
  redo start has passed the drop, "so that no replay can name it"
  (`docs/spec/wal.md:105`, `docs/spec/drop-table.md:34-35`). A writeback of
  that page's dirty frame after the drop logs an image above the drop, and
  a replay then recreates a freed or reused id (`redo.cpp:417-424`). Every
  gate that frees or reuses a page id needs the image counted.
  `[quiet-wrong]`
- **An append that does not stop the instance.** A refused append
  fail-stops the log once its drains are spent (`manager.cpp:389-406`); a
  background image needs one that does not, which the stream does not
  have.
- **A log sync before every write.** The image is never durable when its
  write is ready. With `defer` the caller parks; without it the wait is
  inline, on core 0's eviction drain, the carve, and drain mode inside a
  rollback (`device_page_store.hpp:160-172`).
- **An unstamped image, and a memo.** The image may not stamp its frame -
  a stamp moves the generation and the frame would never come clean - so
  it is a second writer beside the one `log_page_image.hpp:16-19` calls
  the only one. A deferred write must not image again, so the frame
  remembers the generation it was imaged at.
- **The same class test** as F, page 0 included.

### 2.3 What neither covers

- **Page 0 and the map regions.** The redo start is read from page 0, and
  the maps carry no record, so no log image can heal either. DB-S0 reads
  what a tear of each does today (DB-Q3).
- **Headerless pages** - the Waystone directory's interior pages only
  (`page.md:23`) - have no checksum to detect a tear with.
- **A page with no image in the replay range**: one last written by a
  binary before DB, or damaged at rest. It is refused, as today.

## 3. Rules

Binding once DB opens, under the marked DB-Q1.

- **DB-R1 - a logged page the device may hold torn has an image in the
  replay range, and every record of the page after that image follows it
  in the log.** `[quiet-wrong]`
- **DB-R2 - nothing is imaged while redo runs.** `[quiet-wrong]`
- **DB-R3 - no image names a page id after the id is freed**: no replay
  recreates a freed or reused page from an image. `[quiet-wrong]`
- **DB-R4 - the heal is exact.** After a mount that healed, every relation
  answers what the acknowledged history says, and recovery run a second
  time changes nothing (`wal.md` §16-3). The refusal stays the fallback
  for a page with no image, and is never replaced by a guess.
- **DB-R5 - no record kind, no page format, no superblock bump.** An
  older binary reads a log that holds more images.
- **DB-R6 - the volume is visible**: images logged and their bytes are
  counted, and `recovery_pages_healed` keeps its meaning.

## 4. Items for the operator (DB-Q), CLA's proposals

(a) is CLA's proposal in each. **Every item is adopted (a) on the operator's
standing go-ahead of 2026-11-19 (go-ahead-achieving-milestone)**, none as the
operator's own mark; DB-Q1 and DB-Q2 as DB-S0 revised them against the tree
(§7, "DB-S0"), and DB-Q7 added by DB-S0. The options not taken stay open to
the operator's own mark.

| | item | (a) | other |
|---|---|---|---|
| DB-Q1 `[quiet-wrong]` | Where the image is logged. | **F**, at the first exclusive fetch after a copy (§2.1): the spec's placement at one site, no change to recovery, to the gates that free a page, or to the stream; a missed image is a refusal. Its cost is on the statement that first touches a page after it was written. **Revised at DB-S0 (F')**: the debt is set at the fault-in as well as the copy; the decision is taken in `FetchPinned` once the page latch is granted, which is the one site at `cores = 1` and above; the image carries `kNoTxnId` and lowers the frame's recLSN; images are off for the whole mount, not redo alone (DB-R2); and a PAGE_INIT heals a torn page as an image does, for the page created and never copied, which owes no image. **Adopted** | (b) **W**, at the device write (§2.2): nothing on the statement path, and five obligations, three of which touch recovery, the reclaim and the stream; (c) neither until DB-S0 has priced F's statement-path cost |
| DB-Q2 | Which frames are imaged. | only frames whose content records describe, by a class test DB-S0 lists; a class left out keeps today's refusal. **Revised at DB-S0**: the page type alone cannot tell them (§7), so the test is page 0, a headerless page, a whitelist of logged types, and a registry of unlogged owners and page ids the layer that knows them fills - the two unlogged catalog chains at the mount, a fenced index build's oid for the build's life. **Adopted** | (b) every headered frame but page 0 - less to get wrong in the test, more volume, and images of content recovery discards |
| DB-Q3 | Page 0 and the maps (§2.3). | read at DB-S0 and entered in `known-gaps.md` with what a tear does; no remedy in DB | (b) a second copy of page 0 added to DB - a format event |
| DB-Q4 | A switch. | none: the protection is always on | (b) a boot key that turns it off for a data device that writes 8 KiB atomically - a way to be wrong by configuration |
| DB-Q5 | The measurement and its bar. | **DB-S0, no code**: over the standard point and insert cells at `cores` 1 and 4, `group`, the pages written per second by the checkpoint and by writeback - the image volume of either placement - beside the log bytes per second today. **The close**: interleaved A/B before against after over the same cells: statements per second, p50/p99, server CPU a statement, log bytes a commit, checkpoint duration, and the recovery time of one fixed log. **No throughput bar**: the deltas are reported and the operator rules on them, the protection being a correctness item. The standing budget | (b) a bar named by the operator before DB-S1; (c) the close alone |
| DB-Q6 | The sim's assertion (BS-Q5 (a) asserts the refusal). | flipped: a tear at any page write of a logged page is healed and the oracle reconciles; the refusal is asserted only where §2.3 says it stays | (b) both kept behind a draw |
| DB-Q7 | Added at DB-S0. A path D backup on a snapshot that does not freeze (§1.1): `BACKUP START` syncs nothing, and the copy's every page is checked and a torn one refused at STOP, `--verify-backup` and the restore's layout (`src/server/restore.cpp:150-209`). | **left as it stands**: DB heals a crash; a torn page in a path D copy stays refused by the backup, and `known-gaps.md`'s entry says so. The restore's mount would heal it where its image is in `[start, stop]`, which no cell shows; relaxing the checks is a successor's. The test row "a crash-image backup restored with a planted tear" leaves §5 with it. **Adopted** | (b) relax the three checks for a page whose image the backup's log holds, in DB |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DB-S0 - census and premise** | No engine code. **Read**, for F: every exclusive fetch and whether its caller changes the page (§2.1); §1.5's three exceptions; redo's fetches and what a second crash finds (DB-R2); the header's place within a device sector; whether the publish's copy and sync cover every writer as §1.4 says; every append site against §1.5's last premise. **Read, for either**: the classes `WriteBack` writes and which are logged (DB-Q2); every gate that frees or reuses a page id (DB-R3) - the drop reclaim, the index job, undo recycling; whether an undo page outlives its `PAGE_INIT` (§1.2); what a tear of page 0 and of a map region does (DB-Q3); whether `BACKUP START` follows a data-file sync, so a crash-image snapshot's torn pages have their images inside the backup's log. **Measure**: DB-Q5 (a)'s counters. | the lists, the volume, and every item back to the operator; DB-Q1 marked before S1 |
| **DB-S1 - the image** | Under DB-Q1 (a): the bit, the image at the exclusive fetch, the class test, redo's exemption, the counters. Under (b): §2.2's list. | review, the full suite |
| **DB-S2 - the proof** | The sim flipped (DB-Q6) and run over the corpus; the tests below; a crash-image backup restored with a planted tear. | review, the full suite, `scripts/sim.sh`, and the `bs-nightly` cells named in the report as run or not run |
| **DB-S3 - the close** | DB-Q5's A/B; the documents below amended; the milestone row flipped. | closed |

**Tests DB owes.** A page torn at each sector boundary, at every page
write of a seeded run, mounts and reconciles against the oracle (`wal.md`
§16-4, `page.md` §18-5); recovery run twice is a no-op; a crash during
redo's own writeback, then a second mount, loses nothing (DB-R2); a
relation dropped, its pages freed and reused, then a crash with images in
range: no freed page comes back and no reused page is overwritten (DB-R3);
a page written back twice between two checkpoints is healable after
either write; two cores, one dirtying a page while the other writes it
back, under `tsan`; a torn page with no image in range - one planted in a
volume written before DB - is still refused, by name; an unlogged class
and page 0 log no image (DB-Q2); `recovery_pages_healed` counts each heal
once; restore and PITR over a log that holds images.

**Documents DB-S3 owes**, each read at the close and amended where it
states the behaviour: `wal.md` §10 (`:141`, whose "a bulk fill's pages" is
stale since BZ), §11 (`:156`, the cadence as the volume knob), §16-4
(`:240`) and the observability list (`:223`); `page.md` §10 (`:166`) and
§18-5; `backup.md` B5 (`:46`) and its path D text; `known-gaps.md`
(`:2223`, `:1281`, and DB-Q3's entry); `include/kds/txn/undo_log.hpp:62`
if S0 finds the exemption does not hold; `log_page_image.hpp:16-19`;
`sim/instance.hpp:141`, `:160`, `sim/loop.cpp:1691` and
`sim/faults.hpp:21-32`; `workorder-bs-test-hardening.md`'s BS-Q5;
`manual/server/server.md` and `manual/server/backup-restore.md`;
`CLAUDE.md`'s WAL row; and the next tag message, which today must carry
the refusal as a bound.

## 6. What DB does not do

- No doublewrite file: `wal.md` §10 rejects it and DB does not reopen it.
- No protection of page 0, the map regions or headerless pages (§2.3).
- No change to the checkpoint's cadence, pacing or gate, or to the log's
  recycling bound. More log is written between two anchors; the rule that
  bounds what is kept does not move.
- No compression of an image and no elision of a page's free space:
  either is a payload change and a successor.
- Nothing for a standby. The unlanded branch
  `worktree-cm-physical-standby` ships the log; images would ride it, and
  that order is not amended here.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-18

Written on worktree `db-fpi-cadence-dc-for-update` from `a4d2fb3b`
(`v3.4.12-42-ga4d2fb3b`), documents only. The letter DB is CLA's proposal.
No suite was executed: the commit changes no build input. Nothing was
measured.

**Reviewed by `critics-developer` before the commit, as a draft that
proposed W.** It opened the cited lines and corrected seven in place (the
writer's and the split's line ranges, BR-R5's, the six sites on five
paths, the checkpoint's route into `WriteBack`, page 0 as a headered
frame, the elisions in H5's quotation). Its findings on the design, each
taken: redo's drain imaged at the log's end loses changes after a second
crash; an image of a dropped relation's page breaks BF-R4; page 0 goes
through `WriteBack`; a refused image append fail-stops the log; every
writeback would wait for a log sync, inline on three callers; the image
could not stamp its frame; and the comparison was unfair to the spec's
placement, which has one site at the exclusive fetch and no
`checkpoint_interval_ms = 0` problem. **On those findings CLA rewrote §2
and moved the proposal from W to F.** Also applied: the placement stated
once; the format item folded into DB-R5; the measurement and the bar made
one item; the unreproduced counts of append and stamp sites removed.
Rejected: none.

**§2.1's argument for F was written after the review and has not been
reviewed.** The review named the site; the bit, the replay-range argument
and the claim that BR's two conditions do not arise are CLA's, `[design]`,
and are what DB-S0 reads the tree against before DB-Q1 is marked.

### DB-S0 - census and premise, 2026-11-19

On worktree `db-torn-page-healing` at `734484ca` (`v3.4.14-2-g734484ca`), no
engine code. Four read-only censuses (every write fetch; every append against
its page hold; page classes and every gate that frees an id; the checkpoint,
page 0, the backup and the sim), and the volume measured.

**What held.** The checkpoint writes and syncs every page dirty at its publish
before the anchor's page (`src/wal/checkpointer.cpp:619-678`; the synchronous
path `src/server/superblock_checkpoint_anchor.cpp:158-174`), with `kWait` at
every step, so no durable anchor leaves a page dirty at its publish unwritten.
Every page-changing record at a production site is appended under the
exclusive hold that changed the page, or under one taken before the append
and kept to the stamp (the undo recycle's PAGE_INIT, `ASSERT_ROLLBACK`); no
site appends first and takes the page after. The sites that log after
release are fenced or unpublished: a CREATE TABLE's var-heap root
(`src/catalog/catalog.cpp:1451`), a declaration chain's growth page, a split's
created pages (a whole-page record), the fenced index build's tree and the
online build's root (images), and the manager-less dispatcher's spills and
index entries, which no production server runs (`src/server/command_dispatcher.cpp:12795`).
No page byte is written through a read fetch; `PageRef::MarkDirty` has no
caller. So the image's argument (§2.1) stands on the tree: a page's next
write fetch after a copy is the first holder since the copy, and every change
after it is a record after the image.

**What §1-§2 got wrong, corrected in the build (DB-Q1 (a) as revised):**

1. **The decision site.** The exclusive hold's re-mark (`device_page_store.cpp:2797-2811`)
   is armed only above one core; at `cores = 1` the fetch is `Resolve` then
   `PinFrame`. The site is `DevicePageStore::FetchPinned` with `for_read`
   false, after `FetchAndPin` returns with the latch granted, which both
   paths reach. The debt is set at the fault-in too, not only at the copy:
   an evicted clean frame re-faulted has lost its bit.
2. **The page type is no class test** (DB-Q2 as revised). The two unlogged
   catalog chains, `sys.access_stats` and `sys.statistics`, are `kHeap`
   pages like every logged chain, their heads owned by oid 0 like every
   bootstrap root (`include/kds/catalog/well_known.hpp:263-272`, `:378`, `:407`);
   a fenced index build's pages carry the types of a published index; a
   nonzero `page_lsn` fails both ways (an unstamped PAGE_INIT is logged at
   0, an image would stamp an unlogged page). Built: a whitelist of logged
   types, page 0 and headerless pages by the store, and a registry the
   catalog fills at the mount and the fenced build holds for its life
   (`IndexBuild::ReleaseUnlogged`) - the build frees its pages ungated
   (`FreeUnbuiltTree`, `src/exec/index_ddl.cpp:189-196`), so an image of one
   would break DB-R3.
3. **The undo exemption does not hold** (`include/kds/txn/undo_log.hpp:62-65`):
   a tail page takes about 92 records and stays the tail across
   checkpoints, so it routinely outlives its PAGE_INIT in the replay range,
   and a torn one is refused today even with its PAGE_INIT in range, since
   redo let only an image heal (`src/wal/redo.cpp:401-409` at the base). Undo
   pages are imaged like every logged class.
4. **A page created and never copied owes no image, so its PAGE_INIT heals
   it.** It was created after the last landed anchor's publish listed the
   dirty pages - or that publish wrote and synced it - so its PAGE_INIT is
   above the anchor's redo start. Redo now lets a PAGE_INIT heal a torn page
   as an image does. A PAGE_INIT that re-formats in place (the undo recycle,
   `src/txn/undo_log.cpp:98-127`) meets a torn page only after the image its
   own write fetch logged or the creation's PAGE_INIT healed it.
5. **DB-R2 is the whole mount, not redo.** Mount undo, the assertion fold
   (which writes Bound Cabin pages ungated and unstamped,
   `src/exec/assertion_replay.cpp:21-28`, `:65-90`), the unlogged chains'
   discard and the completion checkpoint all fetch for write after redo; an
   image logged mid-fold would sit above records the fold re-applies. Images
   are off until the end of `Expeditor::Open` and `SimInstance::Boot`; the
   debt set meanwhile is paid at the first write fetch after.

Also found: the image's envelope must be `kNoTxnId` (analysis makes a real id
a loser when its commit is below the scan start, `src/wal/analysis.cpp:53-61`;
two existing `LogFullPageImage` callers pass real ids, `src/exec/wal_row_log.cpp:50`,
`src/exec/index_ddl.cpp:170`, `:836` - not DB's to change, and named here);
and the image must lower the frame's recLSN, which `StampPageLsn` does.

**Not reachable by F, stated as gaps** (`known-gaps.md`): page 0 and the two
map regions (read before recovery; a tear refuses the mount); headerless
pages (no checksum); a page written back during a mount and torn by a crash
before the completion checkpoint lands, when its pre-crash image is below the
scan start; and a path D copy (DB-Q7). A candidate race independent of DB, an
index job's `ANCHOR_SLOT_REMOVE` on a relation a concurrent `DROP TABLE` will
free (`src/catalog/catalog.cpp:990`, `src/server/page_reclaim.cpp:122-127`),
was found by reading and not verified; it is not DB's and goes to
`known-gaps.md` as unverified.

**The premise measured** (`bench/v3.4.14/results-db-s0-premise-v3.4.14-2-g734484ca.md`,
`ck-tester`, inside the budget, no hold): the image volume is the page-write
rate, set by the hot set and by the checkpoint cadence, which runs per core.
At the standard setup the log would grow 1.01x to 2.0x at `cores = 1` and
1.19x to 4.4x at `cores = 4` (100,000-row uniform UPDATE the top); under pool
pressure - a 6,060-page table in a 2,048-frame pool - eviction writes 2,534
pages a second and the log would grow **27.5x**. One run a cell; no `strict`
or `relaxed` cell.

### DB-S1 - the image, 2026-11-19

On worktree `db-torn-page-healing` at `c802fac9` (`v3.4.14-4-gc802fac9`),
under DB-Q1 (a) as revised: `Frame::owes_image` (set at the fault-in and at
every writeback copy, in the frame's padding, the 56-byte frame unchanged);
`DevicePageStore::FetchPinned` for write logs the owed image once the page
latch is granted (`LogOwedImage`, through the gate's new
`WalDurability::LogPageImage`, at `kNoTxnId`, stamping the page); the class
test `ImagedClass` (page 0, headerless, the whitelist, the unlogged
registry `AddUnloggedOwner` / `AddUnloggedPage`, filled by
`catalog::RegisterUnloggedClasses` and by a fenced `IndexBuild` for its
life); images enabled at the end of `Expeditor::Open` and
`SimInstance::Boot` (DB-R2); redo heals at a `PAGE_INIT` as at an image;
`pool_fetch_images` in `sys.meta`; `LogFullPageImage` appends the held
bytes without a copy. The sim's DB-Q6 flip landed in the same commit.

**Reviewed by `critics-developer`.** Applied: D1, `CreatePinned`'s resident
branch now clears the debt under the page latch (a create over a resident
frame owed an image of bytes it then formats); D2's text; D3's redo
comment; cut #2 (the identity copy in `LogFullPageImage`), #4 (comment
trims), #5 (dead test lines). Rejected: #1, skipping the image on the
undo recycle - a behaviour change for one class, and the rule stays one
rule; #3, folding the bit test into the re-mark - measured first, at the
close; #6, kept. D4 (the documents that now contradict the code) is
DB-S3's; D5 (the overhead) the close's.

**Suite**: the first run found five cells red, each the premise holding and
the consequence moved: `WalGoldenLog` re-pinned (`0x0124c937u`), the drop
reclaim's premise cell (images re-allocate the leaf at `cores = 2`), two
backup cells (the fuzzy image's rows put in a new table), and
`SortedLeafRigTest.NamedKeysFromTwoCoresInterleaveIntoOneLeafInKeyOrder`, a
retryable `TXN_CONFLICT` that passed 50 of 50 on repeat and whose rig never
enables images. A second run built while the S2 sim edits were landing
mixed two trees (66 SEGFAULTs and one cell hung 3.5 h on a futex); every one
passed on a consistent rebuild, and the full suite then ran **0 failed of
4802**. Overhead not measured; measured at the milestone's close.

### DB-S2 - the proof, 2026-11-19

On worktree `db-torn-page-healing` at `ec057c06` (`v3.4.14-5-gec057c06`).
The sim fails a run that refuses a logged-class tear made outside a mount
(DB-Q6); the verdict merge had dropped `tears_healed`, so a summed run read
0 heals, and now carries it. A tear is classed off the bytes the torn write
carried (`PowerCut::torn_written`), not the resident frame. Cells, in
`tests/sim_loop_test.cpp`: a healed mount crashed and mounted again heals
nothing more and serves the same rows (recovery twice a no-op); a page
written back twice heals after the second write; a healing mount cut at its
first write heals at the next (DB-R2); a tear with no image or `PAGE_INIT`
in range is still refused (the pre-DB volume); and every power-loss point
of a seeded run heals or refuses within §2.3. The DB-Q2 cells (page 0 and
an unlogged class log no image) and the counter's once-per-heal are
`tests/fetch_image_test.cpp` and `wal_redo_test.cpp` (DB-S1).

**Reviewed by `critics-developer`.** It fixed in place: the written-twice
cell's premise (checkpoints put its `PAGE_INIT` and first image behind the
redo start, and the cell checks the torn page is the first write's; a
probe clearing the copy's re-arm makes it fail), the twice-mounted cell's
"no-op" asserted (`pages_healed == 0` at the second mount), the grid cell's
`healed + refused == tears` loosened to `<=` (an unhealable tear nothing
reads is counted in neither), and it added the DB-R2 cell. Applied by CLA
from its findings: the class read off the written bytes (a page freed and
re-created after the cut read as the new class), the counter's comment.
Rejected: a fixture helper for the shared tail of four cells, a returned
pair in place of `last_tear_imaged_`, and one helper for the
`RegisterUnloggedClasses` + `EnableFetchImages` pair - each a few lines,
and the pair sits in two modules (`server`, `sim`) that share no header
for it. Not taken up: a `"/mount"` cut that tears nothing forgets the first
cut's tear (pre-DB; a refused unhealable first tear then fails the run -
never seen).

**Not built of §5's list**: the two-core `tsan` cell (a `bs-nightly` build,
not run), restore and PITR over a log holding images (the backup cells of
DB-S1 restore over such a log, no PITR cell added), and "a crash-image
backup restored with a planted tear", which DB-Q7 (a) removed. DB-R3 (a
dropped and reused id) is carried by the sim's `ReconcileDrops` and the
amended `drop_table_reclaim_test.cpp`, with no new cell.

**Gates**: suite **0 failed of 4807**; `scripts/bs/io_sweep.py --kinds
power-loss-at-page-write --ops 300 --jobs 6` 252 runs, 172 points fired, 0
failures (over seeds 1-5 and 20260826003, page writes 1-14: 28 tears, 23
healed, 1 refused at a mount-phase tear, 4 unread); `scripts/sim.sh` (Release)
280 runs, 0 failures. `bs-nightly` cells: none run. Overhead not measured;
measured at the milestone's close.

### DB-S3 - the close, 2026-11-19

On worktree `db-torn-page-healing`, the code at `ec057c06`
(`v3.4.14-5-gec057c06`). **DB-Q5's A/B** (`ck-tester`,
`bench/v3.4.14/results-db-close-v3.4.14-5-gec057c06.md`, driver
`tools/db_close.py`): `734484ca` against `ec057c06`, Release, interleaved
ABAB, two rounds, 20 s windows, `cores` 1 and 4, `group`; about 37 minutes,
no quiet-gate hold. The noise floor between two runs of one binary is
about 4 %. **No throughput delta outside it** on the standard cells (update
200/1K/10K, spill, insert, the select control) at either core count. **Two
outside it**: update over 100,000 rows at `cores = 4`, -11.7 % (log a
statement 250 -> 1,228 B); and the pool-pressure cell (1,000,000 rows,
2,048 frames, `cores = 1`), -25.3 %, p50 996 -> 1,329 us, p99 1,860 -> 2,276
us, server CPU +11.6 us a statement, the log 28x. The log grows by the image
volume to within 1 %, `pool_fetch_images` equals the pages written in the
pressure cell, and the wait that moves is `commit.wal_flush`. Checkpoint
duration and the mount time of one fixed log (kill -9, 10K and 100K rows,
two cadences) show no change readable above their noise. **No bar is
applied (DB-Q5 (a)): the deltas are the operator's to rule on.** Cut:
`strict`/`relaxed` cells, a `cores = 4` pressure cell, a recovery over a
log dense with images.

**Documents amended**: `wal.md` :7, §10, §16-4, the observability list;
`page.md` §10, §18-5; `backup.md` B5 and §5; `known-gaps.md` (the power-cut
entry rewritten with what stays refused, the backup path D entry, the
unverified `ANCHOR_SLOT_REMOVE` race); `undo_log.hpp`'s FPI comment (undo
pages are now imaged); `log_page_image.hpp:16-19`; `sim/instance.hpp`,
`sim/faults.hpp`, `sim/loop.hpp`; BS-Q5's text in
`workorder-bs-test-hardening.md`; `manual/server/backup-restore.md`;
`CLAUDE.md`'s WAL row. Not amended: `sys-views.md` lists no `pool_` field
(the counter is named in `wal.md`'s observability list);
`manual/server/server.md` states nothing of torn pages - its §6 says WAL
recovery is not implemented, which is stale beyond DB's scope and left for
the operator. The next tag message owes the bound that page 0, the maps,
headerless and unlogged pages and the mount's own writes still refuse.
