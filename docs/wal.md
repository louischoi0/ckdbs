# KDS Write-Ahead Log (WAL) — Technical Specification (rev. 2)

**Status:** Technical specification — input for development agents and internal reference. **Rev. 2, 2026-07-28:** incorporates the confirmed storage/layout decisions (`docs/storage-layout.md` S1–S11: common page header with `page_lsn` + CRC32C, single growable file, per-core pools, pool-enforced flush gate, mmap rejection) and the confirmed MVCC tuple-header model (writer `trx_id` + `undo_ptr`, **no `xmax` field**, delete-mark semantics). Marker legend: `[CONFIRMED]` — settled; `[PROPOSED]` — this document's default, adopt or amend before implementing the affected part; `[OPEN]` — must not be assumed. Consistent with `docs/rules.md`, `docs/sched.md`, `docs/storage-layout.md`, `docs/waystone-concept.md`, `docs/wire-protocol.md`, and the design spec.

Legacy lineage: the kernel-era engine already had a page-based WAL with an in-memory ring buffer, a background checkpointer, and WAL-before-data ordering wired into the heap/btree insert paths. This spec ports those ideas into the userspace, thread-per-core, deterministic-testing architecture — it does not port the code.

---

## 1. Purpose & Guarantee Model

WAL provides the engine's **atomicity and durability**. Target domain is financial OLTP, so the guarantees are stated as business commitments:

- **No acknowledged loss:** a commit acknowledged under the synchronous classes below survives any single crash with no data loss and no partial transaction.
- **Bounded recovery:** restart time is bounded by checkpoint cadence (RTO is a tunable, not an accident).
- **Auditability:** the log is a complete, ordered, checksummed record of every change — the substrate for point-in-time recovery and, later, replication.

### Durability classes `[CONFIRMED — exposed on the wire per wire-protocol.md §9]`

| Class | Commit acknowledgment | Loss window | Intended use |
|---|---|---|---|
| `D1 strict` | after the commit record is durable (flushed + device-synced) | zero | default for financial writes |
| `D2 group` | same durability point; flush batched (group commit); ack waits on the batch | zero (latency traded for throughput) | default operating mode |
| `D3 relaxed` | after the record enters the WAL ring | ≤ configured flush interval | bulk load, reconstructible data |

Class is a per-transaction property (`C_TXN_BEGIN.durability`) with a session default; `S_TXN_OK` for D3 carries the `RELAXED` flag so audit logs distinguish ack semantics. `D1/D2` differ only in batching, never in the durability point.

## 2. Architectural Position

- **Redo log + undo pages `[CONFIRMED]`.** WAL is a **physiological redo log** (page-oriented records: page_id + slot + bytes). MVCC history lives in undo chains reached via each tuple's `undo_ptr`; undo-page writes are themselves WAL-logged, so both roll-forward and roll-back state survive a crash. (InnoDB-shaped, deliberately not Postgres-shaped and not Oracle block/ITL-shaped — evaluated and settled with the MVCC header decision, §5.1.)
- **WAL-before-data `[CONFIRMED — now pool-enforced]`:** no modified page reaches disk before the log records describing the modification are durable. Enforcement moved from caller discipline into code: the per-core `BufferPool` holds a `WalDurability` seam and **refuses to flush a frame until `durable_lsn() ≥ page_lsn`** (storage-layout §8). This spec defines the rule; the pool implements it.
- **Who runs it:** foreground tasks *append* records as part of their page mutations; flushing, group-commit completion, checkpointing, and segment recycling run in the **`system` scheduling group**. WAL housekeeping never runs inside foreground tasks.
- **I/O:** all WAL I/O goes through the injected `IoBackend` seam — never direct syscalls, never mmap (rejected engine-wide, storage-layout §15) — so every guarantee here is testable under deterministic simulation with fault injection (rules.md §4). The concrete backend remains `[OPEN]` and must not leak into WAL logic.
- **Not via `PageStore` `[CONFIRMED]`.** WAL segments are append-only streams; `PageStore`'s random-access `PageRef` semantics are the wrong shape. WAL owns its segment files directly through `IoBackend`. `PageStore` remains the seam for data/undo/catalog/Waystone pages only.
- **Waystone: unlogged `[CONFIRMED]`** — Waystone pages are the headerless page class (storage-layout §1): no `page_lsn`, no checksum, never replayed. On crash, an enabled relation's Waystone rebuilds via backfill; its advisory contract makes an empty structure correct.
- **Backpressure is legitimate here.** Unlike Waystone (advisory, drop-on-overflow), WAL is correctness: a full WAL ring suspends the appending foreground task until space frees — the one sanctioned way durability slows the foreground, visible in metrics (§13).

## 3. Log Topology — Per-Core Streams `[CONFIRMED]`

One WAL stream per core, matching shared-nothing ownership (and the per-core buffer pools of storage-layout §6): a core logs only mutations to state it owns — no shared tail pointer, no lock, no atomic contention on the append path.

- **LSN** is a per-stream monotonically increasing `uint64_t` byte offset (stream-local). No global LSN; cross-stream ordering is not required while transactions are core-local.
- The **superblock** (data-file page 0, storage-layout §4) records, per core, the stream's segment anchor and the last checkpoint's redo start (§14).
- **Cross-core transactions `[OPEN]`:** when multi-core transactions arrive, commit becomes a coordination protocol across participating streams. Nothing in the record format precludes it; do not design it now.
- Core-count changes between runs `[OPEN]`: recovery with a different core count (stream reassignment) — flag, don't assume.

## 4. Segment & Record Format

### 4.1 Segments `[PROPOSED]`

- A stream is a sequence of fixed-size **segment files** (default 64 MiB `[OPEN: size]`), named by `(core_id, segment_no)`.
- Segment header (one 4 KiB block): magic, format version, `core_id`, `segment_no`, `start_lsn`, header CRC32C.
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

### 5.1 MVCC model the records serve `[CONFIRMED 2026-07-28]`

The tuple header carries exactly **`trx_id` (writer, 48-bit) + `undo_ptr`** — there is **no `xmax` field**. A version's death is the next version's birth: walking the undo chain, the reader already knows the overwriting transaction when it arrives at an older version, so the chain itself encodes validity intervals; storing the boundary twice is redundant. DELETE is a **delete-mark** (slot/Keystone flag) plus the deleter's `trx_id` in the writer field — no separate field. Row locking is the Keystone lock byte, not a header field (so the Postgres-style secondary role of `xmax` as a lock slot is also covered). Consequences for WAL: every heap mutation record carries the writer `trx_id` it stamps; undo records carry the *prior* writer id, which is what makes the no-`xmax` reconstruction work. The 48-bit width bounds `txn_id` in §4.2; wraparound/epoch policy `[OPEN]` (owned by the transaction spec).

### 5.2 Types `[PROPOSED]`

Physiological redo: each record targets one page and is idempotently replayable (§9).

- **Transaction:** `TXN_BEGIN`, `TXN_COMMIT`, `TXN_ABORT`.
- **Heap:** `HEAP_INSERT` (slot, tuple bytes incl. Keystone word, writer `trx_id`, `undo_ptr`), `HEAP_OVERWRITE` (in-place new version; payload includes new writer id + new `undo_ptr`), `HEAP_DELETE_MARK` (sets the delete flag + writer id — the DELETE of §5.1), `SLOT_RETIRE` (physical retirement after purge — distinct from delete-mark), `PAGE_INIT` (new heap page: common header + `min_key`; `min_key` is immutable thereafter, so it appears only here).
- **B+ tree:** `BTREE_INSERT`, `BTREE_SPLIT` (one record per affected page).
- **Undo:** `UNDO_WRITE` (undo-page append; payload = before-image + prior writer `trx_id` + prior `undo_ptr` — the chain link).
- **Allocation:** `ALLOC`, `FREE` — emitted by the SpaceManager (storage-layout §5); free-map pages are a **logged, headered page class** replayed like any other. `ALLOC` precedes file extension (growth ordering, storage-layout §14).
- **Catalog:** catalog-page mutations (DDL, Waystone flag changes) as ordinary page records.
- **Control:** `CHECKPOINT_BEGIN` (payload: active-txn table + dirty-page table with recovery LSNs), `CHECKPOINT_END`, `FULL_PAGE_IMAGE` (§10), `PAD`.

Adding a type is a format-version event; unknown types on replay are a hard recovery error, never skipped.

## 6. Write Path `[PROPOSED]`

Port of the legacy in-memory ring, reshaped for the reactor:

1. A foreground task mutating a page first appends its record(s) into the **core-local WAL ring** (preallocated; append = memcpy + cursor bump — no allocation, no I/O). The frame's `page_lsn` mirror is set to the record's LSN (the on-page header field is stamped by the same mutation).
2. Reactor **phase 5** drains the ring into segment writes through `IoBackend`, batched.
3. `TXN_COMMIT` under `D1/D2` suspends the committing task on a flush future; the `system`-group flush completion resumes every task whose commit LSN ≤ durable LSN (**group commit**). `D3` resumes immediately.
4. Ring full ⇒ the appending task suspends until drain (§2 backpressure); stall time is metered.

## 7. (reserved)

Section intentionally reserved to keep §8–§14 numbering stable across revisions.

## 8. Flush-Ordering Rules — normative `[CONFIRMED]`

The whole correctness contract between WAL, `BufferPool`, and the checkpointer:

1. **WAL-before-data:** a dirty page may be written to the data file only when its stream is durable up to that page's `page_lsn`. **Enforced inside `BufferPool::Flush` via the `WalDurability` seam** (storage-layout §8); `MarkClean` is reachable only as flush completion.
2. **Commit-before-ack:** a `D1/D2` commit is acknowledged (`S_TXN_OK`) only after its commit record is durable.
3. **Checkpoint honesty:** `CHECKPOINT_END` is written only after every page in the checkpoint's dirty-page table has been flushed under rule 1, or remains listed with its recovery LSN.

## 9. Page LSN & Idempotent Redo `[CONFIRMED]`

`page_lsn` lives at offset 8 of the **common 32-byte page header** on every headered page (storage-layout §2) — heap, B+ tree, undo, catalog, superblock, free-map. Redo replays a record iff `record.lsn > page_lsn`, making replay idempotent and restartable. Headerless (Waystone) pages have no `page_lsn` and are never replay targets (§2). The rev-1 amendment requesting this field is **satisfied** by storage-layout S1.

## 10. Torn-Page Protection `[CONFIRMED — FPI]`

Adopted with the checksum decision (storage-layout S9): **full-page images.** The first modification of a headered page after each checkpoint logs a `FULL_PAGE_IMAGE`; recovery restores the image before replaying deltas. The doublewrite-buffer alternative is rejected (extra file, second write path; FPI composes cleanly with per-core streams). Division of labor: the header **CRC32C detects** a torn/corrupt page at load; the **FPI heals** it during recovery. Cost is log volume proportional to checkpoint cadence — an explicit RTO/volume trade on the same knob (§11).

## 11. Checkpointing `[PROPOSED]`

Fuzzy checkpoints, run as a `system`-group task per core:

1. Emit `CHECKPOINT_BEGIN` carrying the active-transaction table and the dirty-page table (`BufferPool::DirtyTable()` — `{page_id → recLSN}`, storage-layout §8).
2. Flush dirty pages under §8-1, paced across the checkpoint window (storage-layout §13 checkpoint spreading) and SLO-throttled — the checkpointer never floods the foreground.
3. Emit `CHECKPOINT_END`; **after it is durable**, persist the redo start (`min(recLSN)`) into the superblock anchor (§14-3). recLSN 0 — a page dirtied but described by no record — is skipped, not `min()`ed in; with no logged page in the snapshot the redo start is the `CHECKPOINT_BEGIN` LSN itself.
4. Segments wholly below the redo start are recyclable once archived (§13).

Cadence is the RTO knob: more frequent ⇒ shorter recovery + more FPI volume.

## 12. Recovery `[PROPOSED]`

Per-core, parallel, restartable — each stream recovers independently (valid while transactions are core-local):

1. **Analysis:** from the superblock's redo start, scan to the durable end (§4.2 torn-tail rule); rebuild dirty-page and transaction tables; classify winners (commit record seen) and losers.
2. **Redo:** replay idempotently (§9), restoring `FULL_PAGE_IMAGE`s first per page; a checksum-failed page (storage-layout §10) with an available FPI is restored from it. Redo reconstructs crash-time state including uncommitted changes and undo pages.
3. **Undo:** roll back losers through their undo chains (`undo_ptr`), emitting compensation as ordinary logged page mutations so undo itself is crash-restartable. Delete-marks by loser transactions are cleared the same way.
4. Recovery completion writes a checkpoint, bounding the next crash's work.

A crash during recovery at any point resumes correctly — enforced by the test matrix (§16).

## 13. Business & Operational Features

- **Configuration:** global durability class + per-transaction override (wire-protocol §9); `D3` flush interval; segment size; checkpoint cadence; retention; **undo retention** (§15 — the snapshot-too-old knob).
- **Archiving / PITR `[PROPOSED]`:** sealing a segment fires an archive hook (callback seam). Archived streams + a base backup give point-in-time recovery; the log is designed to be sufficient for it (complete, checksummed, self-delimiting).
- **Replication readiness:** the sealed-segment/stream tap is the future log-shipping source. No format concession needed now; the constraint is only "never break §4 self-description".
- **Observability (ship with the first flush path):** durable-vs-appended LSN per core, flush latency percentiles, group-commit batch sizes, ring-full stall time, checkpoint duration, FPI volume share, undo retention headroom, recovery phase timings. Financial operators tune RPO/RTO with these; they are product features, not debug aids.

## 14. Required Amendments (gate for implementation)

Satisfied by `docs/storage-layout.md` (S1/S9): ~~page headers gain `page_lsn` + checksum~~ — landed via the common header; free-map logged class and ALLOC-before-extend ordering are specified there and referenced here.

Still required:

1. ~~**Design spec — tuple header (MVCC):** replace `xmin/xmax/undo_ptr` with **`trx_id` (48-bit writer) + `undo_ptr` + delete-mark flag** per §5.1~~ — **satisfied 2026-07-29**: `docs/heap-and-tuple.md` §3.2 amended and invariant 12 added; implemented in `include/kds/storage/heap/heap_page.hpp` (20-byte header, `kSlotFlagDeleted`, `PageView::DeleteMark`). The lock role stays in the Keystone lock byte.
2. **Design spec — heap section:** `PAGE_INIT` as the sole logger of `min_key`.
3. ~~**Superblock spec:** per-core WAL anchors (current segment, durable LSN, redo start).~~ — **satisfied 2026-07-29**: superblock v3 carries a fixed `kMaxWalCores`-slot anchor table indexed by `core_id`, each entry `{checkpoint_lsn, redo_start_lsn, durable_lsn, segment_no}` (32 B), implemented in `include/kds/server/superblock.hpp` (`WalAnchorFields`, `SetWalAnchor`/`wal_anchor`) with `SuperBlockCheckpointAnchor` as the `wal::CheckpointAnchor` implementation. An all-zero entry means "never checkpointed" and sends recovery to the start of the stream; `wal_anchor_count` records the last run's core count so §3's changed-core-count question stays open rather than being decided by reindexing.
4. **Transaction/MVCC spec (when written):** undo-page layout, undo retention policy, snapshot-too-old semantics; 48-bit txn-id wraparound/epoch.
5. **`docs/wire-protocol.md` / error registry:** add `SnapshotTooOld` (retryable = context-dependent — define with §15) to the error taxonomy.
6. **`CLAUDE.md`:** WAL opens (§15) in the open list; durability-model and MVCC-header summary lines.

## 15. Open Decisions — do not assume

- Segment size; ring capacity; `D3` flush interval defaults.
- Cross-core transaction commit protocol; recovery under changed core counts (§3).
- I/O backend (inherited); the seam's durability verb (FUA/fsync semantics) — define with the backend.
- **Undo retention policy** and `SnapshotTooOld` surfacing (error class, retryability) — undo-based MVCC's structural trade (the ORA-01555 family), owned by the transaction spec but constraining WAL segment/undo recycling here.
- 48-bit `trx_id` wraparound/epoch handling.
- Undo-page layout details (`UNDO_WRITE` targets).
- Archive hook transport (filesystem copy vs pluggable).

## 16. Testing Requirements

All deterministic (injected clock + `IoBackend` fault injection; rules.md §4). Crash-consistency tests are the shipping condition for every WAL-touching change:

1. **Format:** record/segment codec round-trips; alignment; torn-tail detection (truncate at every byte boundary of the last record); 48-bit txn_id upper-bits-zero assertion.
2. **Ordering:** instrumented backend proves §8-1..3 under randomized scheduling — no data write ever precedes its log durability; `MarkClean` unreachable outside flush completion (shared test with storage-layout §18-4).
3. **Crash matrix:** crashes injected at every phase boundary (append / partial segment write / between commit-durable and ack / mid-checkpoint / each recovery phase); recovery yields exactly the acknowledged-commit state; replaying recovery twice is a no-op.
4. **Torn writes:** partial-page and partial-record corruption injected; checksum detects at load, FPI restores in recovery (composition test with storage-layout §18-5).
5. **MVCC records:** delete-mark by a winner survives; delete-mark by a loser is cleared by undo; `UNDO_WRITE` chain links (prior writer id + prior undo_ptr) reconstruct validity intervals with no `xmax` anywhere — a reader fixture verifies visibility across a rebuilt chain.
6. **Durability classes:** `D1/D2` never lose an acked commit under any injected crash; `D3` loss bounded by the configured window — bound asserted; `RELAXED` flag present on D3 acks.
7. **Group commit:** N concurrent committers, one flush; all resume with durable LSN ≥ their commit LSN.
8. **Backpressure:** ring saturation suspends producers without deadlock; stall metrics visible.
9. **Per-core:** multi-stream recovery equals the union of independent single-stream recoveries.
