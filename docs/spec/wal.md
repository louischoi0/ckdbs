# KDS Write-Ahead Log (WAL)

How KDS makes a mutation durable, and how it replays one. `[PROPOSED]` marks a default to confirm or amend before the affected part is built; `[OPEN]` must not be assumed. Companion specs: `docs/spec/page.md` (page header, flush gate, file layout), `docs/spec/txn.md` (transactions and MVCC), `docs/spec/cross-owner-txn.md` (the commit across cores), `docs/spec/heap-and-tuple.md`, `docs/rules/rules.md`, `docs/spec/sched.md`.

**Status: every data mutation is logged, and recovery runs at mount.** The boundary lives in `include/kds/wal/recovery.hpp`:

- **Analysis, redo and the high-water repair run once per log** — once on core 0 under one stream (§3), per core under per-core streams — followed by a completion checkpoint; `SHOW META` prints the recovery block. The catalog recovers too, and a torn catalog page refuses the mount rather than being served corrupt.
- **The undo phase is injected.** `RecoverStream` takes an `UndoPhase`; mount installs `txn::RecoveryUndo` (`src/server/mount_recovery.cpp`), so losers are rolled back. A stream that analysis found losers in, recovered **without** one, fails the mount — deliberately, because redo without undo is worse than no recovery at all: redo restores uncommitted writes, and a surviving uncommitted row reads as *committed* on the next boot (`txn.md` §4.1). A recovery that replays and stops has published every loser's writes.

§11a states exactly which mutations are logged; §12 specifies the replay.

---

## 1. Purpose & Guarantee Model

WAL provides the engine's **atomicity and durability**. Target domain is financial OLTP, so the guarantees are stated as business commitments:

- **No acknowledged loss:** a commit acknowledged under the synchronous classes below survives any single crash with no data loss and no partial transaction.
- **Bounded recovery:** restart time is bounded by checkpoint cadence (RTO is a tunable, not an accident).
- **Auditability:** the log is a complete, ordered, checksummed record of every change — the substrate for point-in-time recovery and, later, replication.

### Durability classes

| Class | Commit acknowledgment | Loss window | Intended use |
|---|---|---|---|
| `D1 strict` | after the commit record is durable (flushed + device-synced) | zero | default for financial writes |
| `D2 group` | same durability point; flush batched (group commit); ack waits on the batch | zero (latency traded for throughput) | default operating mode |
| `D3 relaxed` | after the record enters the WAL ring | ≤ configured flush interval | bulk load, reconstructible data |

Class is a per-transaction property (`C_TXN_BEGIN.durability`) with a session default; `S_TXN_OK` for D3 carries the `RELAXED` flag so audit logs distinguish ack semantics. `D1/D2` differ only in batching, never in the durability point.

## 2. Architectural Position

- **Redo log + undo pages.** WAL is a **physiological redo log** (page-oriented records: page_id + slot + bytes). MVCC history lives in undo chains reached via each tuple's `undo_ptr`; undo-page writes are themselves WAL-logged, so both roll-forward and roll-back state survive a crash. (InnoDB-shaped, deliberately not Postgres-shaped and not Oracle block/ITL-shaped — settled with the MVCC header decision, §5.1.)
- **WAL-before-data:** no modified page reaches disk before the log records describing the modification are durable. Enforced in code, not by caller discipline: the per-core `BufferPool` holds a `WalDurability` seam and **refuses to flush a frame until `durable_lsn() ≥ page_lsn`** (`docs/spec/page.md` §8). This spec defines the rule; the pool implements it.
- **Who runs it:** foreground tasks *append* records as part of their page mutations; flushing, group-commit completion, checkpointing, and segment recycling run in the **`system` scheduling group**. WAL housekeeping never runs inside foreground tasks.
- **I/O:** all WAL I/O goes through the injected `IoBackend` seam — never direct syscalls, never mmap (rejected engine-wide, `docs/spec/page.md` §15) — so every guarantee here is testable under deterministic simulation with fault injection (rules.md §4). The concrete backend remains `[OPEN]` and must not leak into WAL logic.
- **Not via `PageStore`.** WAL segments are append-only streams; `PageStore`'s random-access `PageRef` semantics are the wrong shape. WAL owns its segment files directly through `IoBackend`. `PageStore` remains the seam for data/undo/catalog/Waystone pages only.
- **Waystone: unlogged** — Waystone pages are the headerless page class (`docs/spec/page.md` §1): no `page_lsn`, no checksum, never replayed. On crash, an enabled relation's Waystone rebuilds via backfill; its advisory contract makes an empty structure correct.
- **Backpressure is legitimate here.** Unlike Waystone (advisory, drop-on-overflow), WAL is correctness: a full WAL ring suspends the appending foreground task until space frees — the one sanctioned way durability slows the foreground, visible in metrics (§13).

## 3. Log Topology — One Stream Per Instance

**A volume records its own topology and both are readable.** `SuperBlockFields::log_topology` holds `kSingleStream` (1) or `kPerCoreStreams` (0); a fresh volume bootstraps `kSingleStream`, and every image written before AR0 M0 holds the zero that means the other, so reading the field is not a format event. The mount branches on it — there is no runtime switch and no way to convert a volume between the two.

**Under one stream** (the current engine, AR0 M0): the instance has one log. Core 0 opens it and owns it; every peer **attaches** to that stream and to core 0's writer thread rather than opening `wal-<core>-*` of its own. A peer appends through the stream's latch and asks core 0's writer for a sync instead of issuing one — so every `fdatasync` this instance pays is issued once, over one file — on core 0's reactor where somebody is parked on the result, and on core 0's writer thread for D3's loss-window tick and for every sync a peer asked for. The append path therefore carries a lock, which per-core streams did not; AR0-2 accepts that cost deliberately and it is what §6's ring bookkeeping is written around.

- **LSN** is instance-wide: one monotonic `uint64_t` byte offset over the one log, comparable everywhere. Records from different cores interleave in it freely.
- **The superblock anchor is a fold, not a per-core array.** Slot 0 holds the record of whichever core published the lowest `redo_start_lsn`; slots 1..N−1 are unused, and `SetWalAnchor` refuses any core but 0 so no caller can bypass the fold. Until **every** core has published at least once, the fold holds at the mount anchor rather than at the minimum over the cores that have — a warm-up, because a core that has published nothing has not yet said its redo start is high. `superblock_checkpoint_anchor.hpp` owns it.
- **Recovery runs once**, on core 0, before any peer exists (§12). A peer performs no recovery pass; its `SHOW META` recovery block says so rather than reporting an empty one.
- **A prepared cross-owner transaction resolves in the stream.** With one log there is no coordinator's *file* to read: the coordinator's decision, if it was made durable, is in the same log the participant's `TXN_PREPARE` is in — so **absence of a decision is abort**, licensed by §11-3's prepare floor, which keeps every undecided prepare inside the replay range. `docs/spec/cross-owner-txn.md` §2c carries the rule.
- **A page's stream stamp says nothing under one stream.** PL-C's stamp is a *claim of ownership*, not a statement about which log a page's records are in, so redo neither refuses a foreign stamp nor restamps what it applies, and `PAGE_INIT` stamps from the core named in the record envelope's `flags` byte rather than from the replaying core. `docs/spec/page-lsn-cross-stream.md` is superseded in part and says rule by rule which of its rules survive.
- **`PAGE_HANDOFF` no longer removes its page from the dirty-page table**, because what licensed that removal was a *flush*, and a flush covers one core's page store while with one log the removal would speak for every core's records. §12-1 and `wal/analysis.cpp` carry the case.
- **Counters are per manager, so a peer's are honestly zero.** A peer performs no device sync, so its `wal_syncs` and `wal_interval_syncs` are structurally 0 and its durability cost is read on core 0. `wal_sync_failures` is the exception: a peer's flush of the shared ring and its wait on the writer can both fail and are counted where they happen.

**Under per-core streams** (a pre-M0 volume, still mountable): one stream per core, matching the shared-nothing ownership the engine had then and the per-core buffer pools of `docs/spec/page.md` §6 — a core logged only mutations to state it owned, with no shared tail pointer on the append path. LSNs are stream-local and never compared across streams; the superblock holds one anchor slot per core; recovery runs per core in parallel; a participant's prepared transaction is resolved by reading the coordinator's file; and the PL-C stamp rules of `page-lsn-cross-stream.md` apply in full.

- **The core count is pinned under both.** The superblock records `core_count` at bootstrap and a mount whose running `cores` disagrees is refused, naming both numbers (`superblock.hpp`). Under per-core streams a mount at M would leave |N − M| streams with nothing to replay them. Under one stream the reason is different and no weaker: the anchor's warm-up is defined over "every core has published", so a changed count would either park the fold at the mount anchor forever or advance it past a core that no longer runs. Online core-count change is not supported.

## 4. Segment & Record Format

### 4.1 Segments `[PROPOSED]`

- A stream is a sequence of fixed-size **segment files** (default 64 MiB `[OPEN: size]`), named by `(core_id, segment_no)`.
- Segment header (one 4 KiB block): magic, format version, `core_id`, `segment_no`, `start_lsn`, header CRC32C.
- **`core_id` names the stream's owner, not the record's author.** Under one stream (§3) it is 0 on every segment, because core 0 owns the log and the peers attach to it; a record's own logging core, where it matters, is in the record envelope. Under per-core streams the two coincided, which is why the field reads as if it were the author's.
- **Format version is 2, and this build reads version 2 only** (`kSegmentFormatVersion`, `kMinReadableSegmentFormatVersion`). The minimum is a **floor and not just a version**: `DecodeSegmentHeader` refuses what is *newer* than this build, and the floor is what refuses what is older rather than accepting and mis-decoding it. The floor tracks the current version while no compatibility promise exists (pre-1.0); the day one does, a decoder per supported version replaces it, with a migration story.
- **Any change to a record payload is a format-version event**, because recovery reads the log back. "Nothing reads this format back" is not an argument that may be reused without re-checking that it is still true.
- Records are 8-byte aligned and **never span segments**: a non-fitting record pads the tail with `PAD` and seals the segment. Oversized payloads are a design error, not a spanning case.
- A sealed segment is immutable — the unit of archiving (§13) and recycling (§11).

### 4.2 Record header `[PROPOSED]`

Fixed 32-byte header + payload; little-endian; field-wise memcpy codec with `static_assert`ed offsets (rules.md §2/§5):

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `total_len` | header + payload + padding to 8 B |
| 4 | 4 | `crc32c` | over bytes 8..total_len |
| 8 | 8 | `lsn` | this record's stream offset |
| 16 | 8 | `txn_id` | zero-extended **48-bit** transaction id (§5.1); upper 16 bits 0; 0 = non-transactional (checkpoint, pad) |
| 24 | 1 | `type` | §5 |
| 25 | 1 | `flags` | per-type |
| 26 | 2 | `reserved` | 0 |
| 28 | 4 | `page_id` | target page; `kInvalidPageId` where N/A |

Torn-tail detection needs no commit marker: recovery walks forward; the first record with a bad CRC or impossible `total_len` is the durable end of the stream.

## 5. Record Catalog

### 5.1 MVCC model the records serve

The tuple header carries exactly **`trx_id` (writer, 48-bit) + `undo_ptr`** — there is **no `xmax` field**. A version's death is the next version's birth: walking the undo chain, the reader already knows the overwriting transaction when it arrives at an older version, so the chain itself encodes validity intervals; storing the boundary twice is redundant. DELETE is a **delete-mark** (slot/Keystone flag) plus the deleter's `trx_id` in the writer field — no separate field. Row locking is the Keystone lock byte, not a header field (so the Postgres-style secondary role of `xmax` as a lock slot is also covered). Consequences for WAL: every heap mutation record carries the writer `trx_id` it stamps; undo records carry the *prior* writer id, which is what makes the no-`xmax` reconstruction work. The 48-bit width bounds `txn_id` in §4.2; exhaustion is `OutOfRange`, never wrapped (`docs/spec/txn.md` §4.2).

### 5.2 Types `[PROPOSED]`

Physiological redo: each record targets one page and is idempotently replayable (§9).

- **Transaction:** `TXN_BEGIN`, `TXN_COMMIT`, `TXN_ABORT`, and `TXN_PREPARE` (a participant's promise in a cross-owner commit, `docs/spec/cross-owner-txn.md` §2).
- **Heap:** `HEAP_INSERT` (slot, tuple bytes incl. Keystone word, writer `trx_id`, `undo_ptr`), `HEAP_OVERWRITE` (in-place new version; payload includes new writer id + new `undo_ptr`), `HEAP_DELETE_MARK` (sets the delete flag + writer id — the DELETE of §5.1), `SLOT_RETIRE` (physical retirement after purge — distinct from delete-mark), `PAGE_INIT` (new heap page: common header + `min_key` + `owner_oid`, the owning relation's oid stamped into the common header at initialization, `docs/spec/page.md` §2a; `min_key` is immutable thereafter, so it appears only here). The `PAGE_INIT` payload is 24 bytes, `owner_oid` at offset 16 with four reserved bytes at 12 that keep the codec's mirror struct naturally aligned; a 12-byte legacy payload decodes as owner 0 (unattributed), mirroring §2a's on-page zero-default. **The length discriminator is a floor, not an equality**: `DecodeRecord` returns the record's 8-byte-aligned tail rather than the exact payload — a 12-byte payload comes back as 16 bytes, a 24-byte one as 24 — so `>=` is the only test that reads a legacy record correctly, and it is the rule every payload codec in `wal/payload.cpp` uses.
- **B+ tree:** `BTREE_INSERT`, `BTREE_SPLIT` (one record per affected page).
- **Undo:** `UNDO_WRITE` (undo-page append; payload = the record's tail + prior writer `trx_id` + prior `undo_ptr` — the chain link; `docs/spec/txn.md` §3.5).
- **Var-heap:** `VARHEAP_APPEND` (a spilled value landing in a `kVarHeap` page; payload = slot + value bytes, target page in the envelope — `docs/rules/rule-fixed-length-tuple.md` §5) and `VARHEAP_RELEASE` (its rollback compensation, `docs/spec/txn.md` §6). The slot is recorded rather than re-derived because replay must reproduce the *exact* pointer the tuple's cell already carries; a pointer resolving to a different slot after recovery would be a value silently swapped for another. **Write ordering:** `VARHEAP_APPEND` precedes the `HEAP_INSERT`/`HEAP_OVERWRITE` whose cell points at it, in the same transaction, replayed by ordinary winner/loser machinery. **There is deliberately no var-heap-specific recovery logic, and none may be added.** Logged at all because a var-heap value is *authoritative data*: losing one loses a committed value, not a hint, which is what separates this class from the advisory waystone family.
- **Allocation:** `ALLOC`, `FREE` — reserved for the SpaceManager (`docs/spec/page.md` §5); free-map pages are a **logged, headered page class** replayed like any other. `ALLOC` precedes file extension (growth ordering, `docs/spec/page.md` §14).
- **Catalog:** catalog-page mutations (DDL, Waystone flag changes) as ordinary page records.
- **Control:** `CHECKPOINT_BEGIN` (payload: active-txn table + dirty-page table with recovery LSNs), `CHECKPOINT_END`, `FULL_PAGE_IMAGE` (§10), `PAD`.

Adding a type is a format-version event; unknown types on replay are a hard recovery error, never skipped. `record.hpp`'s enum is frozen and append-only.

## 6. Write Path `[PROPOSED]`

Shaped for the reactor:

1. A foreground task mutating a page first appends its record(s) into the **WAL ring** — the instance's under one stream, this core's under per-core streams (§3); preallocated, so an append is a memcpy and a cursor bump, no allocation and no I/O. Under one stream the **whole** append runs under the stream's latch — the size checks, any segment roll, the encode into the ring and the cursor bump; the reserve/copy/publish split that would have left the copy outside it was tried and abandoned (AL-R1, `page-lsn-cross-stream.md`'s superseded table). The frame's `page_lsn` mirror is set to the record's LSN (the on-page header field is stamped by the same mutation).
2. Reactor **phase 5** drains the ring into segment writes through `IoBackend`, batched.
3. `TXN_COMMIT` under `D1/D2` suspends the committing task on a flush future; the `system`-group flush completion resumes every task whose commit LSN ≤ durable LSN (**group commit**). `D3` resumes immediately.
4. Ring full ⇒ the appending task suspends until drain (§2 backpressure); stall time is metered.

## 7. (reserved)

Section intentionally reserved to keep §8–§14 numbering stable across revisions.

## 8. Flush-Ordering Rules — normative

The whole correctness contract between WAL, `BufferPool`, and the checkpointer:

1. **WAL-before-data:** a dirty page may be written to the data file only when the log it is logged in is durable up to that page's `page_lsn` — the instance's log under one stream, this core's stream under per-core streams (§3). **Enforced inside `BufferPool::Flush` via the `WalDurability` seam** (`docs/spec/page.md` §8); `MarkClean` is reachable only as flush completion. `DevicePageStore` — the store the server runs on — holds the same seam (`SetWalGate()`) and applies it in `Flush`, `Sync` and `FlushPages`, gating on the highest `page_lsn` in the batch. An ungated store flushing a logged page is exactly the violation this rule names.
2. **Commit-before-ack:** a `D1/D2` commit is acknowledged (`S_TXN_OK`) only after its commit record is durable.
3. **Checkpoint honesty:** `CHECKPOINT_END` is written only after every page in the checkpoint's dirty-page table has been flushed under rule 1, or remains listed with its recovery LSN.

## 9. Page LSN & Idempotent Redo

`page_lsn` lives at offset 8 of the **common 32-byte page header** on every headered page (`docs/spec/page.md` §2) — heap, B+ tree, undo, catalog, superblock, free-map. Redo replays a record iff `record.lsn > page_lsn`, making replay idempotent and restartable. A headerless page has no `page_lsn` and is never a replay target (§2).

## 10. Torn-Page Protection

Adopted with the checksum decision (`docs/spec/page.md`): **full-page images.** The first modification of a headered page after each checkpoint logs a `FULL_PAGE_IMAGE`; recovery restores the image before replaying deltas. The doublewrite-buffer alternative is rejected (extra file, second write path; FPI composes cleanly with either topology). Division of labor: the header **CRC32C detects** a torn/corrupt page at load; the **FPI heals** it during recovery. Cost is log volume proportional to checkpoint cadence — an explicit RTO/volume trade on the same knob (§11).

## 11. Checkpointing `[PROPOSED]`

Fuzzy checkpoints, run as a `system`-group task per core — each core checkpoints its own pools and its own active transactions under both topologies. Under one stream the two checkpoint records name their logging core in the record envelope's `flags` byte, since the log now carries every core's, and step 3 publishes into the fold of §3 rather than into a slot of this core's own:

1. Emit `CHECKPOINT_BEGIN` carrying the active-transaction table (each entry with the transaction's `last_undo_ptr`, `docs/spec/txn.md` §3.3) and the dirty-page table (`BufferPool::DirtyTable()` — `{page_id → recLSN}`, `docs/spec/page.md` §8).
2. Flush dirty pages under §8-1, paced across the checkpoint window (`docs/spec/page.md` §13 checkpoint spreading) and SLO-throttled — the checkpointer never floods the foreground.
3. Emit `CHECKPOINT_END`; **after it is durable**, persist the redo start (`min(recLSN)`) into the superblock anchor (§14-3). recLSN 0 — a page dirtied but described by no record — is skipped, not `min()`ed in; with no logged page in the snapshot the redo start is the `CHECKPOINT_BEGIN` LSN itself. **The redo start also floors at the oldest live `TXN_PREPARE`** (`ActiveTransactions::OldestPreparedLsn`), so a prepared cross-owner participant's record stays inside every future replay range until the transaction is decided (`docs/spec/cross-owner-txn.md` §2c). Without the floor, that record leaves the range as soon as the transaction's pages are written back; the next mount then reads the active-list entry as an ordinary loser and **rolls back a transaction its coordinator may have committed**. An in-doubt transaction therefore pins the log, and `cross-owner-txn.md` §2b's ceiling bounds how much.
4. Segments wholly below the redo start are recyclable once archived (§13) — **except that a coordinator's stream may not recycle a segment holding a cross-owner decision until every participant of that transaction has made its own terminal record durable**, and a participant's pre-durable acknowledgement does not discharge this (`docs/spec/cross-owner-txn.md` §2c). A retention policy keyed on a core's own checkpoint alone is locally correct and silently recovers another core's committed transaction as aborted.

Cadence is the RTO knob: more frequent ⇒ shorter recovery + more FPI volume.

## 11a. What logs today

**Every data mutation and every catalog mutation**, as the ordinary record types: `kHeapInsert`/`kHeapOverwrite`/`kHeapDeleteMark`/`kSlotRetire` per row, `PAGE_INIT` for a catalog overflow page, a relation's root and its var-heap root, a `FULL_PAGE_IMAGE` for a chain-link edit and for each page of a backfilled index tree. There is no catalog-specific record type.

Verified against the emission sites:

| Path | Records |
|---|---|
| `INSERT` | `TXN_BEGIN` → (`FULL_PAGE_IMAGE` + `PAGE_INIT` when the heap chain grows) → **`UNDO_WRITE{kVarHeapAppend}` per spilled cell** → `VARHEAP_APPEND` per spilled cell → `INDEX_INSERT` per index → `HEAP_OVERWRITE` of the `sys.tables` id bump (the record that makes the id ceiling durable, `docs/rules/keystoneid-k0-findings.md`) → `HEAP_INSERT` → `TXN_COMMIT` |
| `UPDATE` | `UNDO_WRITE` (before-image) → **`UNDO_WRITE{kVarHeapAppend}` per spilled cell** → `VARHEAP_APPEND` per spilled cell → `INDEX_INSERT` per touched index → `HEAP_OVERWRITE` |
| `DELETE` | `UNDO_WRITE` → `HEAP_DELETE_MARK` |
| rollback | the compensations of `docs/spec/txn.md` §6 — `SLOT_RETIRE` / `HEAP_OVERWRITE` / `HEAP_DELETE_MARK` / `VARHEAP_RELEASE` — then `TXN_ABORT` |
| assertions | `ASSERT_BUILD` at CREATE, `ASSERT_RESERVE` / `ASSERT_COMMIT` / `ASSERT_ROLLBACK` on the write paths, `ASSERT_DROP` at teardown |
| checkpointer | `CHECKPOINT_BEGIN` / `CHECKPOINT_END` |

**Ordering rules**, each load-bearing and enforced:

1. `VARHEAP_APPEND` and `INDEX_INSERT` both precede the heap record whose cell or entry points at them (§5.2, `docs/spec/index.md` §12.1), for opposite pointer directions and the same reason — the surviving direction is the harmless one.
2. An `UNDO_WRITE{kVarHeapAppend}` precedes the `VARHEAP_APPEND` it can undo, so redo alone can never resurrect an append the undo phase has no record to release; a crash between a spill and its tuple write rolls back like any other loser write. **Gap:** a spill logged with `kNoTxnId` (`LogChainInsert`'s path, taken by the assertion catalog) has no transaction to chain to and no record, so a rolled-back one leaks; nothing sweeps it.
3. A catalog write's `UNDO_WRITE` precedes its row record — redo alone must never resurrect a row the undo phase has no record to retire.
4. A catalog record's **envelope names the acting transaction or `kNoTxnId`, never the header's writer**: analysis notes every named envelope as a loser until a commit in range says otherwise, so a `next_id` bump logged under the relation's long-committed creator would invent phantom crash losers.

One safety note the relation-root `PAGE_INIT`s rest on: they are deliberately unstamped (the first row record stamps the page), and a root that never receives a row is protected by the **checkpointer flushing every page in its snapshot** before `CHECKPOINT_END` — the safety lives in that flush, not in a stamp.

**DDL logs.** DDL runs under a real transaction (autocommit included), its catalog writes log the ordinary types with undo records a crash loser's mount rolls back through, and `SHOW META` prints `ddl_durable=1 catalog_recovered=1` (`docs/spec/txn.md` §7, `docs/spec/ddl-transactional.md`). The **row-codec definition relations** (`sys.assertions`' source rows) log through `exec/wal_row_log.hpp` — the same order rules, `kNoTxnId` envelopes — so an acknowledged `CREATE ASSERTION` survives and **enforces** after a crash. Two rules ride with that: every transactionless DDL statement — assertion, cabin, ALTER — has no commit record for the durability class to ride, so `kStrict` **and `kGroup`, whose durability point is D1's**, sync at the acknowledgement (`AwaitDdlDurability`); and redo has a `kCabinBound` arm, because `BoundCabinPage::Format` writes a body whose `next_page_id` a zeroed page misreads as page 0. There is no genesis arm for assertion recovery: the publish-time `ASSERT_SNAPSHOT` (`assertion_catalog.cpp`) covers a declaration born after the last checkpoint, and an arm ordering itself before it could adopt an under-counted base over that better one.

**Outside the log, precisely:** `ALLOC`/`FREE`, reserved in the record enum and emitted by nothing, and the **advisory Waystone classes** — trail pages (`stats/trail_store.cpp`) and directory pages (`stats/waystone_dir.cpp`) — which invariant 8 exempts by construction: deleting them wholesale must never change a result, so they owe the log nothing. Nothing *authoritative* is outside.

Three properties of the INSERT path are the shape the other paths copy or deliberately do not copy:

- **The `FULL_PAGE_IMAGE` on chain growth stands in for a missing record type.** Growth mutates two pages: the new page, and the *old* tail whose `next_page_id` now reaches it. §5.2 has no record for a link edit, so the old tail is logged whole. It costs one page of log per page of heap — about +50% log volume on small rows, paid once per 8 KB of tuples, never per tuple. A link-edit record type would be a format-version event.
- **Records are appended after the page is mutated, not while it is latched.** §8-1 asks for the latter. What makes the former sound *here* is narrow: the server is a single cooperative thread, no flush can interleave between the mutation and the `page_lsn` stamp, and the store's gate covers every instant after it. Any path that suspends mid-statement must generate its record under the latch instead.
- **A failed append aborts the write scope.** The statement runs inside one, and a failed append aborts it, which retires the slot through the ordinary rollback compensation — so the row does not survive unlogged. It holds only where a `TransactionManager` is wired in; a dispatcher built without one leaves the row.

Transaction ids come from `docs/spec/txn.md` §4.2's block-reserved allocator over a superblock field, and the abort path lives there too (`docs/spec/txn.md` §6).

## 12. Recovery `[PROPOSED]`

Restartable, and once per log. **Under one stream** (§3) core 0 runs the three phases over the whole log before any peer is constructed, and a prepared participant is resolved from the same log — absence of a decision is abort (`docs/spec/cross-owner-txn.md` §2c). **Under per-core streams** the passes are per-core and parallel, each stream recovering independently, and a prepared participant is resolved by reading its coordinator's file. Either way:

1. **Analysis:** from the superblock's redo start — the fold's under one stream, this core's slot under per-core streams — scan to the durable end (§4.2 torn-tail rule); rebuild dirty-page and transaction tables; classify winners (commit record seen) and losers. A `PAGE_HANDOFF` removes its page from the dirty-page table under per-core streams only (§3).
2. **Redo:** replay idempotently (§9), restoring `FULL_PAGE_IMAGE`s first per page; a checksum-failed page (`docs/spec/page.md` §10) with an available FPI is restored from it. Redo reconstructs crash-time state including uncommitted changes and undo pages. Under per-core streams it also enforces PL-C: a page carrying another core's stream stamp inside this stream's redo scope is `Corruption`, and every applied page is restamped. Under one stream it does neither (§3).
3. **Undo:** roll back losers through their undo chains (`undo_ptr`), emitting compensation as ordinary logged page mutations so undo itself is crash-restartable. Delete-marks by loser transactions are cleared the same way.
4. Recovery completion writes a checkpoint, bounding the next crash's work.

A crash during recovery at any point resumes correctly — enforced by the test matrix (§16).

## 13. Business & Operational Features

- **Configuration:** global durability class + per-transaction override (wire-protocol §9); `D3` flush interval; segment size; checkpoint cadence; retention.
- **Archiving / PITR `[PROPOSED]`:** sealing a segment fires an archive hook (callback seam). Archived streams + a base backup give point-in-time recovery; the log is designed to be sufficient for it (complete, checksummed, self-delimiting).
- **Replication readiness:** the sealed-segment/stream tap is the future log-shipping source. No format concession needed now; the constraint is only "never break §4 self-description".
- **Observability (ship with the first flush path):** durable-vs-appended LSN per log, flush latency percentiles, group-commit batch sizes, ring-full stall time, checkpoint duration, FPI volume share, undo retention headroom, recovery phase timings. Financial operators tune RPO/RTO with these; they are product features, not debug aids.

## 14. What this spec owes other documents

The tuple MVCC header, `PAGE_INIT` as the sole logger of `min_key`, the superblock's WAL anchors (§3), and the undo page layout are all specified and built — see `docs/spec/heap-and-tuple.md` §3.2, §11a here, `include/kds/server/superblock.hpp`, and `docs/spec/txn.md` §3 respectively.

## 15. Open Decisions — do not assume

The open decisions of this subsystem are unrecorded here. The one retention rule that is *not* open — a coordinator's stream may not recycle a segment holding a decision until every participant's terminal record is durable — is §11-4.

## 16. Testing Requirements

All deterministic (injected clock + `IoBackend` fault injection; rules.md §4). Crash-consistency tests are the shipping condition for every WAL-touching change:

1. **Format:** record/segment codec round-trips; alignment; torn-tail detection (truncate at every byte boundary of the last record); 48-bit txn_id upper-bits-zero assertion.
2. **Ordering:** instrumented backend proves §8-1..3 under randomized scheduling — no data write ever precedes its log durability; `MarkClean` unreachable outside flush completion (shared test with `docs/spec/page.md` §18-4).
3. **Crash matrix:** crashes injected at every phase boundary (append / partial segment write / between commit-durable and ack / mid-checkpoint / each recovery phase); recovery yields exactly the acknowledged-commit state; replaying recovery twice is a no-op.
4. **Torn writes:** partial-page and partial-record corruption injected; checksum detects at load, FPI restores in recovery (composition test with `docs/spec/page.md` §18-5).
5. **MVCC records:** delete-mark by a winner survives; delete-mark by a loser is cleared by undo; `UNDO_WRITE` chain links (prior writer id + prior undo_ptr) reconstruct validity intervals with no `xmax` anywhere — a reader fixture verifies visibility across a rebuilt chain.
6. **Durability classes:** `D1/D2` never lose an acked commit under any injected crash; `D3` loss bounded by the configured window — bound asserted; `RELAXED` flag present on D3 acks.
7. **Group commit:** N concurrent committers, one flush; all resume with durable LSN ≥ their commit LSN.
8. **Backpressure:** ring saturation suspends producers without deadlock; stall metrics visible.
9. **Topology:** under per-core streams, multi-stream recovery equals the union of independent single-stream recoveries. Under one stream the corresponding cell is that a peer's writes are recovered by core 0's single pass and that a peer runs none — plus the shared-stream cells: concurrent appenders each landing at the LSN they were handed, no LSN twice, the durable watermark never retreating, and a peer's `strict` commit made durable by core 0's writer with the peer's own sync count at zero.
