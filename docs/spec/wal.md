# KDS Write-Ahead Log (WAL)

How KDS makes a mutation durable, and how it will replay one. `[PROPOSED]` marks a default to confirm or amend before the affected part is built; `[OPEN]` must not be assumed. Companion specs: `docs/spec/page.md` (page header, flush gate, file layout), `docs/spec/txn.md` (transactions and MVCC), `docs/spec/heap-and-tuple.md`, `docs/rules/rules.md`, `docs/spec/sched.md`.

**Status: every data mutation is logged; recovery is not implemented.** Nothing reads the log back, so a restart is protected only by `PageStore::Sync()` at `SYNC` or clean shutdown. §11a states exactly which mutations are logged today; §12 specifies the replay that does not exist yet.

*(Corrected 2026-08-10. This line read "logging works for INSERT" from before the transaction work, and understated the first half by four subsystems while the second half — the important one — stayed right, which is probably why it went unrevised.)*

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

- **Redo log + undo pages.** WAL is a **physiological redo log** (page-oriented records: page_id + slot + bytes). MVCC history lives in undo chains reached via each tuple's `undo_ptr`; undo-page writes are themselves WAL-logged, so both roll-forward and roll-back state survive a crash. (InnoDB-shaped, deliberately not Postgres-shaped and not Oracle block/ITL-shaped — evaluated and settled with the MVCC header decision, §5.1.)
- **WAL-before-data:** no modified page reaches disk before the log records describing the modification are durable. Enforcement moved from caller discipline into code: the per-core `BufferPool` holds a `WalDurability` seam and **refuses to flush a frame until `durable_lsn() ≥ page_lsn`** (`docs/spec/page.md` §8). This spec defines the rule; the pool implements it.
- **Who runs it:** foreground tasks *append* records as part of their page mutations; flushing, group-commit completion, checkpointing, and segment recycling run in the **`system` scheduling group**. WAL housekeeping never runs inside foreground tasks.
- **I/O:** all WAL I/O goes through the injected `IoBackend` seam — never direct syscalls, never mmap (rejected engine-wide, `docs/spec/page.md` §15) — so every guarantee here is testable under deterministic simulation with fault injection (rules.md §4). The concrete backend remains `[OPEN]` and must not leak into WAL logic.
- **Not via `PageStore`.** WAL segments are append-only streams; `PageStore`'s random-access `PageRef` semantics are the wrong shape. WAL owns its segment files directly through `IoBackend`. `PageStore` remains the seam for data/undo/catalog/Waystone pages only.
- **Waystone: unlogged** — Waystone pages are the headerless page class (`docs/spec/page.md` §1): no `page_lsn`, no checksum, never replayed. On crash, an enabled relation's Waystone rebuilds via backfill; its advisory contract makes an empty structure correct.
- **Backpressure is legitimate here.** Unlike Waystone (advisory, drop-on-overflow), WAL is correctness: a full WAL ring suspends the appending foreground task until space frees — the one sanctioned way durability slows the foreground, visible in metrics (§13).

## 3. Log Topology — Per-Core Streams

One WAL stream per core, matching shared-nothing ownership (and the per-core buffer pools of `docs/spec/page.md` §6): a core logs only mutations to state it owns — no shared tail pointer, no lock, no atomic contention on the append path.

- **LSN** is a per-stream monotonically increasing `uint64_t` byte offset (stream-local). No global LSN; cross-stream ordering is not required while transactions are core-local.
- The **superblock** (data-file page 0, `docs/spec/page.md` §4) records, per core, the stream's segment anchor and the last checkpoint's redo start (§14).
- **Cross-core transactions `[OPEN]`:** when multi-core transactions arrive, commit becomes a coordination protocol across participating streams. Nothing in the record format precludes it; do not design it now.
- Core-count changes between runs `[OPEN]`: recovery with a different core count (stream reassignment) — flag, don't assume.

## 4. Segment & Record Format

### 4.1 Segments `[PROPOSED]`

- A stream is a sequence of fixed-size **segment files** (default 64 MiB `[OPEN: size]`), named by `(core_id, segment_no)`.
- Segment header (one 4 KiB block): magic, format version, `core_id`, `segment_no`, `start_lsn`, header CRC32C.
- **Format version is 2 since 2026-08-12, and this build reads version 2 only** (`kSegmentFormatVersion`, `kMinReadableSegmentFormatVersion`). Two record payloads had moved under AS6a's licence "free today — no WAL stream has ever been read back": `AssertEntryPayload` gained `group_id` (16 → 20 bytes, so every byte after offset 16 shifted) and RC03's `UNDO_WRITE` correction. Recovery running at mount is what expired that licence, so the version moved with it. **A floor and not just a bump**: `DecodeSegmentHeader` refuses only what is *newer* than this build, so a version bump alone would have left a v1 segment accepted and mis-decoded rather than refused. The floor tracks the current version while no compatibility promise exists (pre-1.0); the day one does, it stops tracking and a decoder per supported version replaces it — with a migration story, which is a decision to take then and not a default to inherit.
- The **"free today" argument may not be reused without re-checking that it is still true.** It was sound when written and false eight commits later; what makes a format touch free is that nothing reads the format back, and that property now has an expiry date.
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

The tuple header carries exactly **`trx_id` (writer, 48-bit) + `undo_ptr`** — there is **no `xmax` field**. A version's death is the next version's birth: walking the undo chain, the reader already knows the overwriting transaction when it arrives at an older version, so the chain itself encodes validity intervals; storing the boundary twice is redundant. DELETE is a **delete-mark** (slot/Keystone flag) plus the deleter's `trx_id` in the writer field — no separate field. Row locking is the Keystone lock byte, not a header field (so the Postgres-style secondary role of `xmax` as a lock slot is also covered). Consequences for WAL: every heap mutation record carries the writer `trx_id` it stamps; undo records carry the *prior* writer id, which is what makes the no-`xmax` reconstruction work. The 48-bit width bounds `txn_id` in §4.2; wraparound/epoch policy `[OPEN]` (owned by the transaction spec).

### 5.2 Types `[PROPOSED]`

Physiological redo: each record targets one page and is idempotently replayable (§9).

- **Transaction:** `TXN_BEGIN`, `TXN_COMMIT`, `TXN_ABORT`.
- **Heap:** `HEAP_INSERT` (slot, tuple bytes incl. Keystone word, writer `trx_id`, `undo_ptr`), `HEAP_OVERWRITE` (in-place new version; payload includes new writer id + new `undo_ptr`), `HEAP_DELETE_MARK` (sets the delete flag + writer id — the DELETE of §5.1), `SLOT_RETIRE` (physical retirement after purge — distinct from delete-mark), `PAGE_INIT` (new heap page: common header + `min_key`; `min_key` is immutable thereafter, so it appears only here). **Amended and built 2026-08-13** (`docs/spec/page.md` §2a): the payload gains `owner_oid` — the owning relation's oid, stamped into the common header at initialization — growing it 12 → 24 bytes (`owner_oid` at offset 16; four reserved bytes at 12 keep the codec's mirror struct naturally aligned, so the 12-byte legacy prefix is unchanged). Not a new type, a length-discriminated extension: the decoder accepts both forms, and the 12-byte legacy one decodes as owner 0 (unattributed), mirroring §2a's on-page zero-default. **The discriminator is a floor, not an equality** (corrected by review the same day): `DecodeRecord` returns the record's 8-byte-aligned tail rather than the exact payload — a 12-byte payload comes back as 16 bytes, a 24-byte one as 24 — so `>=` is the only test that reads a legacy record correctly, and it is the rule every payload codec in `wal/payload.cpp` already used.
- **B+ tree:** `BTREE_INSERT`, `BTREE_SPLIT` (one record per affected page).
- **Undo:** `UNDO_WRITE` (undo-page append; payload = before-image + prior writer `trx_id` + prior `undo_ptr` — the chain link).
- **Var-heap:** `VARHEAP_APPEND` (a spilled value landing in a `kVarHeap` page; payload = slot + value bytes, target page in the envelope — `docs/rules/rule-fixed-length-tuple.md` §5). The slot is recorded rather than re-derived because replay must reproduce the *exact* pointer the tuple's cell already carries; a pointer resolving to a different slot after recovery would be a value silently swapped for another. **Write ordering:** `VARHEAP_APPEND` precedes the `HEAP_INSERT`/`HEAP_OVERWRITE` whose cell points at it, in the same transaction, replayed by ordinary winner/loser machinery. A crash between the two leaves an unreferenced value for purge's sweep — the harmless direction, and the reason the ordering is that way round. **There is deliberately no var-heap-specific recovery logic, and none may be added.** Logged at all because a var-heap value is *authoritative data*: losing one loses a committed value, not a hint, which is what separates this class from the advisory waystone family.
- **Allocation:** `ALLOC`, `FREE` — emitted by the SpaceManager (`docs/spec/page.md` §5); free-map pages are a **logged, headered page class** replayed like any other. `ALLOC` precedes file extension (growth ordering, `docs/spec/page.md` §14).
- **Catalog:** catalog-page mutations (DDL, Waystone flag changes) as ordinary page records.
- **Control:** `CHECKPOINT_BEGIN` (payload: active-txn table + dirty-page table with recovery LSNs), `CHECKPOINT_END`, `FULL_PAGE_IMAGE` (§10), `PAD`.

Adding a type is a format-version event; unknown types on replay are a hard recovery error, never skipped.

## 6. Write Path `[PROPOSED]`

Shaped for the reactor:

1. A foreground task mutating a page first appends its record(s) into the **core-local WAL ring** (preallocated; append = memcpy + cursor bump — no allocation, no I/O). The frame's `page_lsn` mirror is set to the record's LSN (the on-page header field is stamped by the same mutation).
2. Reactor **phase 5** drains the ring into segment writes through `IoBackend`, batched.
3. `TXN_COMMIT` under `D1/D2` suspends the committing task on a flush future; the `system`-group flush completion resumes every task whose commit LSN ≤ durable LSN (**group commit**). `D3` resumes immediately.
4. Ring full ⇒ the appending task suspends until drain (§2 backpressure); stall time is metered.

## 7. (reserved)

Section intentionally reserved to keep §8–§14 numbering stable across revisions.

## 8. Flush-Ordering Rules — normative

The whole correctness contract between WAL, `BufferPool`, and the checkpointer:

1. **WAL-before-data:** a dirty page may be written to the data file only when its stream is durable up to that page's `page_lsn`. **Enforced inside `BufferPool::Flush` via the `WalDurability` seam** (`docs/spec/page.md` §8); `MarkClean` is reachable only as flush completion. `DevicePageStore` — the store the server runs on — holds the same seam (`SetWalGate()`) and applies it in `Flush`, `Sync` and `FlushPages`, gating on the highest `page_lsn` in the batch. An ungated store flushing a logged page is exactly the violation this rule names.
2. **Commit-before-ack:** a `D1/D2` commit is acknowledged (`S_TXN_OK`) only after its commit record is durable.
3. **Checkpoint honesty:** `CHECKPOINT_END` is written only after every page in the checkpoint's dirty-page table has been flushed under rule 1, or remains listed with its recovery LSN.

## 9. Page LSN & Idempotent Redo

`page_lsn` lives at offset 8 of the **common 32-byte page header** on every headered page (`docs/spec/page.md` §2) — heap, B+ tree, undo, catalog, superblock, free-map. Redo replays a record iff `record.lsn > page_lsn`, making replay idempotent and restartable. A headerless page has no `page_lsn` and is never a replay target (§2).

## 10. Torn-Page Protection

Adopted with the checksum decision (`docs/spec/page.md`): **full-page images.** The first modification of a headered page after each checkpoint logs a `FULL_PAGE_IMAGE`; recovery restores the image before replaying deltas. The doublewrite-buffer alternative is rejected (extra file, second write path; FPI composes cleanly with per-core streams). Division of labor: the header **CRC32C detects** a torn/corrupt page at load; the **FPI heals** it during recovery. Cost is log volume proportional to checkpoint cadence — an explicit RTO/volume trade on the same knob (§11).

## 11. Checkpointing `[PROPOSED]`

Fuzzy checkpoints, run as a `system`-group task per core:

1. Emit `CHECKPOINT_BEGIN` carrying the active-transaction table and the dirty-page table (`BufferPool::DirtyTable()` — `{page_id → recLSN}`, `docs/spec/page.md` §8).
2. Flush dirty pages under §8-1, paced across the checkpoint window (`docs/spec/page.md` §13 checkpoint spreading) and SLO-throttled — the checkpointer never floods the foreground.
3. Emit `CHECKPOINT_END`; **after it is durable**, persist the redo start (`min(recLSN)`) into the superblock anchor (§14-3). recLSN 0 — a page dirtied but described by no record — is skipped, not `min()`ed in; with no logged page in the snapshot the redo start is the `CHECKPOINT_BEGIN` LSN itself.
4. Segments wholly below the redo start are recyclable once archived (§13).

Cadence is the RTO knob: more frequent ⇒ shorter recovery + more FPI volume.

## 11a. What logs today

**Every data mutation, and — since 2026-08-19 (RV3,
`docs/workplan-rv3-catalog-recovery.md`) — every catalog mutation too**,
as the ordinary record types the "Catalog" line above always planned:
`kHeapInsert`/`kHeapOverwrite`/`kHeapDeleteMark`/`kSlotRetire` per row,
`PAGE_INIT` for a catalog overflow page, a relation's root and its
var-heap root, a `FULL_PAGE_IMAGE` for a chain-link edit and for each
page of a backfilled index tree. No new record type, no format bump.
*(Corrected 2026-08-10 — this section read "`INSERT` and nothing else",
which was true before `docs/spec/txn.md`'s work and has not been since;
corrected again 2026-08-19 when "no catalog mutation" fell.)*

Verified against the emission sites:

| Path | Records |
|---|---|
| `INSERT` | `TXN_BEGIN` → (`FULL_PAGE_IMAGE` + `PAGE_INIT` when the heap chain grows) → `VARHEAP_APPEND` per spilled cell → `INDEX_INSERT` per index → `HEAP_OVERWRITE` of the `sys.tables` id bump (RV3 — the record that makes K1's ceiling durable, and a per-INSERT cost the measurement prices) → `HEAP_INSERT` → `TXN_COMMIT` |
| `UPDATE` | `UNDO_WRITE` (before-image) → `INDEX_INSERT` per touched index → `HEAP_OVERWRITE` |
| `DELETE` | `UNDO_WRITE` → `HEAP_DELETE_MARK` |
| rollback | the compensations of `docs/spec/txn.md` §6 — `SLOT_RETIRE` / `HEAP_OVERWRITE` / `HEAP_DELETE_MARK` — then `TXN_ABORT` |
| assertions | `ASSERT_BUILD` at CREATE, `ASSERT_RESERVE` / `ASSERT_COMMIT` / `ASSERT_ROLLBACK` on the write paths, `ASSERT_DROP` at teardown |
| checkpointer | `CHECKPOINT_BEGIN` / `CHECKPOINT_END` |

Ordering rules that are load-bearing and already enforced: `VARHEAP_APPEND` and `INDEX_INSERT` both precede the heap record whose cell or entry points at them (§5.2, `docs/spec/index.md` §12.1), for opposite pointer directions and the same reason — the surviving direction is the harmless one. RV3 adds two more: a catalog write's `UNDO_WRITE` precedes its row record (redo alone must never resurrect a row the undo phase has no record to retire), and a catalog record's **envelope names the acting transaction or `kNoTxnId`, never the header's writer** — analysis notes every named envelope as a loser until a commit in range says otherwise, so a `next_id` bump logged under the relation's long-committed creator invented phantom crash losers (the RV3 review's B1). One safety note the relation-root `PAGE_INIT`s rest on: they are deliberately unstamped (the first row record stamps the page), and a root that never receives a row is protected by the **checkpointer flushing every page in its snapshot** before `CHECKPOINT_END` — the safety lives in that flush, not in a stamp.

~~**Still outside the log**: `CREATE TABLE` and every other DDL~~ —
**closed 2026-08-19** (RV3): DDL runs under a real transaction (autocommit
included), its catalog writes log the ordinary types with undo records a
crash loser's mount rolls back through, and `SHOW META` prints
`ddl_durable=1 catalog_recovered=1`. The `sys.tables.next_id` bump logs
with everything else, which closes the unlogged-ceiling half of
`docs/rules/keystoneid-k0-findings.md` §4's K1 exposure. Still outside the log,
precisely: `ALLOC`/`FREE`, reserved in the record enum and emitted by
nothing (the SpaceManager of `docs/spec/page.md` §5 is unbuilt), and the
**advisory Waystone classes** — trail pages (`stats/trail_store.cpp`)
and directory pages (`stats/waystone_dir.cpp`) — which invariant 8
exempts by construction: deleting them wholesale must never change a
result, so they owe the log nothing. Nothing *authoritative* is outside.
The two **row-codec definition relations** (`sys.pattern_defs`,
`sys.assertions`' source rows) joined on 2026-08-19 through
`exec/wal_row_log.hpp` — the same order rules, `kNoTxnId` envelopes —
which closed the last silent crash loss: an acknowledged
`CREATE ASSERTION` now survives and **enforces** after a crash. Two
pre-existing holes fell out of proving that end to end (both
unobservable while the row itself always died with the crash): every
transactionless DDL statement — pattern, assertion, cabin, ALTER — had
no commit record for the durability class to ride, so `kStrict` **and
`kGroup`, whose documented durability point is D1's**, now sync at the
acknowledgement (`AwaitDdlDurability`); and redo gained a `kCabinBound`
arm (`BoundCabinPage::Format` writes a body whose `next_page_id` a
zeroed page misreads as page 0 — `AdoptChain` on a redone root walked
into the superblock). A genesis arm for assertion recovery was built and
deleted the same day — the publish-time `ASSERT_SNAPSHOT`
(`assertion_catalog.cpp`, AS6a) already covers a declaration born after
the last checkpoint, and the arm's ordering could adopt an under-counted
base over that better one; the refusal site records the reasoning.

Three properties of the INSERT path are worth stating, because they are the shape the remaining paths should copy or deliberately not copy:

- **The `FULL_PAGE_IMAGE` on chain growth is a placeholder for a missing record type.** Growth mutates two pages: the new page, and the *old* tail whose `next_page_id` now reaches it. §5.2 has no record for a link edit, so the old tail is logged whole. It costs one page of log per page of heap — about +50% log volume on small rows, paid once per 8 KB of tuples, never per tuple. A `HEAP_CHAIN_LINK` record type would remove it and is the obvious first entry the next time the record enum is extended (record.hpp's enum is frozen and append-only, so it is a format-version event and not something the insert path decides on its own).
- **Records are appended after the page is mutated, not while it is latched.** §8-1 asks for the latter. What makes the former sound *here* is narrow: the server is a single cooperative thread, no flush can interleave between the mutation and the `page_lsn` stamp, and the store's gate covers every instant after it. Any path that suspends mid-statement must generate its record under the latch instead.
- **A failed append used to leave the tuple in the page.** The client got an `ERR`, and with no transaction manager to unwind the heap insert the row existed and was unlogged — a lost write on a crash, not a wrong answer now. **`docs/spec/txn.md` closed this**: the statement runs inside a write scope, and a failed append aborts it, which retires the slot through the ordinary rollback compensation. It holds only where a `TransactionManager` is wired in; a dispatcher built without one still leaves the row.

Transaction ids come from `docs/spec/txn.md` §2's block-reserved allocator over a superblock field, and the abort path lives there too (`docs/spec/txn.md` §6). Because recovery does not exist, an uncommitted row surviving a crash reads as *committed* on the next boot — stated precisely in `docs/spec/txn.md` §8.

## 12. Recovery `[PROPOSED]`

Per-core, parallel, restartable — each stream recovers independently (valid while transactions are core-local):

1. **Analysis:** from the superblock's redo start, scan to the durable end (§4.2 torn-tail rule); rebuild dirty-page and transaction tables; classify winners (commit record seen) and losers.
2. **Redo:** replay idempotently (§9), restoring `FULL_PAGE_IMAGE`s first per page; a checksum-failed page (`docs/spec/page.md` §10) with an available FPI is restored from it. Redo reconstructs crash-time state including uncommitted changes and undo pages.
3. **Undo:** roll back losers through their undo chains (`undo_ptr`), emitting compensation as ordinary logged page mutations so undo itself is crash-restartable. Delete-marks by loser transactions are cleared the same way.
4. Recovery completion writes a checkpoint, bounding the next crash's work.

A crash during recovery at any point resumes correctly — enforced by the test matrix (§16).

## 13. Business & Operational Features

- **Configuration:** global durability class + per-transaction override (wire-protocol §9); `D3` flush interval; segment size; checkpoint cadence; retention; **undo retention** (§15 — the snapshot-too-old knob).
- **Archiving / PITR `[PROPOSED]`:** sealing a segment fires an archive hook (callback seam). Archived streams + a base backup give point-in-time recovery; the log is designed to be sufficient for it (complete, checksummed, self-delimiting).
- **Replication readiness:** the sealed-segment/stream tap is the future log-shipping source. No format concession needed now; the constraint is only "never break §4 self-description".
- **Observability (ship with the first flush path):** durable-vs-appended LSN per core, flush latency percentiles, group-commit batch sizes, ring-full stall time, checkpoint duration, FPI volume share, undo retention headroom, recovery phase timings. Financial operators tune RPO/RTO with these; they are product features, not debug aids.

## 14. What this spec owes other documents

The tuple MVCC header, `PAGE_INIT` as the sole logger of `min_key`, the per-core superblock anchors, and the undo page layout are all specified and built — see `docs/spec/heap-and-tuple.md` §3.2, §11a here, `include/kds/server/superblock.hpp`, and `docs/spec/txn.md` §3 respectively.

What remains outstanding is carried in `docs/spec/txn.md` §9: undo retention policy, snapshot-too-old semantics, and 48-bit txn-id wraparound.

## 15. Open Decisions — do not assume

- Segment size; ring capacity; `D3` flush interval defaults.
- Cross-core transaction commit protocol; recovery under changed core counts (§3).
- I/O backend (inherited); the seam's durability verb (FUA/fsync semantics) — define with the backend.
- **Undo retention policy** and `SnapshotTooOld` surfacing (error class, retryability) — undo-based MVCC's structural trade, owned by `docs/spec/txn.md` §9 but constraining WAL segment and undo recycling here.
- 48-bit `trx_id` wraparound and epoch handling. Exhaustion is `OutOfRange`, never wrapped (`docs/spec/txn.md` §9).
- Archive hook transport (filesystem copy vs pluggable).

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
9. **Per-core:** multi-stream recovery equals the union of independent single-stream recoveries.
