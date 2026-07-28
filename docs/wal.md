# KDS Write-Ahead Log (WAL) — Technical Specification

**Status:** Technical specification — input for development agents and internal reference. Marker legend: `[CONFIRMED]` — inherited from settled KDS design/rules or ported deliberately from the legacy engine; `[PROPOSED]` — this document's default design, to be adopted or amended before implementation of the affected part; `[OPEN]` — must not be assumed. Consistent with `docs/rules.md`, `docs/sched.md`, `docs/page-management.md`, `docs/waystone-concept.md`, and the design spec.

Legacy lineage: the kernel-era engine already had a page-based WAL with an in-memory ring buffer, a background checkpointer, and WAL-before-data ordering wired into the heap/btree insert paths. This spec ports those ideas into the userspace, thread-per-core, deterministic-testing architecture — it does not port the code.

---

## 1. Purpose & Guarantee Model

WAL provides the engine's **atomicity and durability**. Target domain is financial OLTP, so the guarantees are stated as business commitments, not implementation conveniences:

- **No acknowledged loss:** a commit acknowledged under the synchronous classes below survives any single crash (process kill, power cut) with no data loss and no partial transaction.
- **Bounded recovery:** restart time is bounded by checkpoint cadence (RTO is a tunable, not an accident).
- **Auditability:** the log is a complete, ordered, checksummed record of every change — the substrate for point-in-time recovery and, later, replication.

### Durability classes `[PROPOSED]`

| Class | Commit acknowledgment | Loss window | Intended use |
|---|---|---|---|
| `D1 strict` | after the commit record is durable (flushed + device-synced) | zero | default for financial writes |
| `D2 group` | same durability point, but flush is batched (group commit); ack waits on the batch | zero (latency traded for throughput) | default operating mode |
| `D3 relaxed` | after the record enters the WAL ring | ≤ configured flush interval | bulk load, reconstructible data |

Class is a per-transaction property with a global default; `D1/D2` differ only in batching, never in the durability point. Only `D3` can lose acknowledged work, and only within its stated window.

## 2. Architectural Position

- **Redo log + undo pages `[CONFIRMED]`.** The design spec's MVCC carries `undo_ptr` chains in tuple headers; undo lives in undo pages, and WAL is a **physiological redo log** (page-oriented records: page_id + slot + bytes). Undo-page writes are themselves WAL-logged, so both roll-forward and roll-back state survive a crash. (InnoDB-shaped, not Postgres-shaped.)
- **WAL-before-data `[CONFIRMED]`** — carried from the legacy engine and now normative (§8): no modified page reaches disk before the log records describing the modification are durable.
- **Who runs it:** foreground tasks *append* records as part of their page mutations; flushing, group-commit completion, checkpointing, and segment recycling run in the **`system` scheduling group** (docs/sched.md §4). WAL housekeeping never runs inside foreground tasks.
- **I/O:** all WAL I/O goes through the injected `IoBackend` seam — never direct syscalls — so every guarantee in this document is testable under deterministic simulation with fault injection (rules.md §4). The concrete backend (`O_DIRECT` vs `io_uring`) remains `[OPEN]` and must not leak into WAL logic.
- **Not via `PageStore` `[PROPOSED]`.** WAL segments are append-only streams; `PageStore`'s random-access page semantics (`CreateAt/Get`) are the wrong shape. WAL owns its segment files directly through `IoBackend`. `PageStore` remains the seam for data/undo/Waystone pages only.
- **Waystone:** default persistence class **unlogged** `[PROPOSED]` — on crash, an enabled relation's Waystone rebuilds via backfill (its advisory contract makes an empty structure correct). This resolves the spec's open item as a default; logging Waystone remains possible later without format changes (it would just emit page records like any other page).
- **Backpressure is legitimate here.** Unlike Waystone (advisory, drop-on-overflow), WAL is correctness: a full WAL ring suspends the appending foreground task until space frees. This is the one sanctioned way durability slows the foreground, and it is visible in metrics (§13).

## 3. Log Topology — Per-Core Streams `[PROPOSED]`

One WAL stream per core, matching shared-nothing ownership: a core logs only mutations to state it owns, so no cross-core coordination exists on the append path — no shared tail pointer, no lock, no atomic contention.

- **LSN** is a per-stream monotonically increasing `uint64_t` byte offset (stream-local). There is no global LSN; ordering across streams is not required while transactions are core-local.
- The **superblock** records, per core, the stream's segment anchor and the last checkpoint's redo start (§14 amendment).
- **Cross-core transactions `[OPEN]`:** when multi-core transactions arrive (roadmap M5+), commit becomes a coordination protocol across the participating streams (e.g., presumed-abort 2PC records or a commit-sequence service). Nothing in the record format below precludes it; do not design it now.
- Core count changes between runs `[OPEN]`: recovery with fewer/more cores than at crash time (stream reassignment) — flag, don't assume.

## 4. Segment & Record Format

### 4.1 Segments `[PROPOSED]`

- A stream is a sequence of fixed-size **segment files** (default 64 MiB `[OPEN: size]`), named by `(core_id, segment_no)`.
- Segment header (one 4 KiB block): magic, format version, `core_id`, `segment_no`, `start_lsn`, header CRC32C.
- Records are 8-byte aligned and **never span segments**: if a record does not fit, the tail is padded with a `PAD` record and the segment is sealed. Max record size is therefore segment-bounded; oversized payloads (should not exist for physiological records) are a design error, not a spanning case.
- A sealed segment is immutable — the unit of archiving (§13) and recycling (§11).

### 4.2 Record header `[PROPOSED]`

Fixed 32-byte header followed by payload; all multi-byte fields little-endian; codec is field-wise memcpy with `static_assert`ed offsets (rules.md §2, §5 — bitfields forbidden):

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `total_len` | header + payload + padding to 8B |
| 4 | 4 | `crc32c` | over bytes 8..total_len (header-after-crc + payload) |
| 8 | 8 | `lsn` | this record's stream offset |
| 16 | 8 | `txn_id` | 0 for non-transactional records (checkpoint, pad) |
| 24 | 1 | `type` | §5 |
| 25 | 1 | `flags` | per-type |
| 26 | 2 | `reserved` | 0 |
| 28 | 4 | `page_id` | target page; `kInvalidPageId` where N/A |

Torn-tail detection needs no commit marker: recovery walks records forward; the first record with a bad CRC or an impossible `total_len` is the durable end of the stream.

## 5. Record Catalog `[PROPOSED]`

Physiological redo: each record targets one page and is idempotently replayable against it (§9).

- **Transaction:** `TXN_BEGIN`, `TXN_COMMIT`, `TXN_ABORT`.
- **Heap:** `HEAP_INSERT` (slot, tuple bytes incl. Keystone word + MVCC header), `HEAP_OVERWRITE`, `SLOT_RETIRE`, `PAGE_INIT` (new heap page: `min_key`, header — note `min_key` is immutable thereafter, so it appears only here).
- **B+ tree:** `BTREE_INSERT`, `BTREE_SPLIT` (one record per affected page; a split is multiple records inside one txn).
- **Undo:** `UNDO_WRITE` (undo-page append).
- **Allocation:** `ALLOC`, `FREE` — emitted by the future SpaceManager (docs/page-management.md §6.2); reserved now so allocation durability needs no format bump.
- **Catalog:** catalog-page mutations (DDL, Waystone flag changes) as ordinary page records.
- **Control:** `CHECKPOINT_BEGIN` (payload: active-txn table + dirty-page table with recovery LSNs), `CHECKPOINT_END`, `FULL_PAGE_IMAGE` (§10), `PAD`.

Adding a type is a format-version event; unknown types on replay are a hard recovery error, never skipped.

## 6. Write Path `[PROPOSED]`

Port of the legacy in-memory ring, reshaped for the reactor:

1. A foreground task mutating a page first appends its record(s) into the **core-local WAL ring** (preallocated at startup; append is a memcpy + cursor bump — no allocation, no I/O, rules-compliant for the reactor's steady state). The page's in-memory `page_lsn` is set to the record's LSN.
2. Reactor **phase 5** (submit pending I/O) drains the ring into segment writes through `IoBackend`, batched.
3. `TXN_COMMIT` under `D1/D2` suspends the committing task on a flush future; the `system`-group flush completion resumes every task whose commit LSN ≤ durable LSN (**group commit** falls out of the batching naturally). `D3` resumes immediately.
4. Ring full ⇒ the appending task suspends until drain (§2 backpressure); stall time is metered.

## 7. (reserved)

Section intentionally reserved to keep §8–§14 numbering stable across the amendment cycle.

## 8. Flush-Ordering Rules — normative `[CONFIRMED]`

These three rules are the whole correctness contract between WAL, `BufferPool`, and the checkpointer:

1. **WAL-before-data:** a dirty page may be written to the data files only when the stream is durable up to that page's `page_lsn`. `BufferPool::MarkClean` semantics in `docs/page-management.md` are hereby bound to this rule.
2. **Commit-before-ack:** a `D1/D2` commit is acknowledged to the client only after its commit record is durable.
3. **Checkpoint honesty:** `CHECKPOINT_END` is written only after every page in the checkpoint's dirty-page table has been flushed under rule 1, or remains listed with its recovery LSN.

## 9. Page LSN & Idempotent Redo `[PROPOSED]`

Every page type (heap, B+ tree, undo, catalog, Waystone-if-logged) gains a **`page_lsn: uint64_t`** header field: the LSN of the last record applied to it. Redo replays a record iff `record.lsn > page_lsn` — making replay idempotent and restartable. This is an on-disk header amendment (§14) and follows all format rules.

## 10. Torn-Page Protection `[OPEN — recommendation recorded]`

An 8 KiB page spans device atomic-write units; a crash mid-page-write corrupts the page beyond what physiological redo can fix. Options: **(a) full-page images** — first modification of a page after each checkpoint logs a `FULL_PAGE_IMAGE`, recovery restores it before replaying deltas (Postgres FPW); **(b) doublewrite buffer** (InnoDB). **Recommendation: (a)** — no extra file, no second write path, cost is log volume proportional to checkpoint cadence, and it composes cleanly with per-core streams. Decision also fixes whether pages carry their own checksum field (recommended alongside (a); another header field if adopted).

## 11. Checkpointing `[PROPOSED]`

Fuzzy checkpoints, run as a `system`-group task per core:

1. Emit `CHECKPOINT_BEGIN` carrying the active-transaction table and the dirty-page table (page_id → recovery LSN) snapshotted from the `BufferPool`.
2. Flush dirty pages under §8-1, respecting the SLO controller (the checkpointer never floods the foreground; docs/sched.md §4).
3. Emit `CHECKPOINT_END`; persist the redo start (`min(recovery LSNs)`) into the superblock anchor.
4. Segments wholly below the redo start are recyclable once archived (§13).

Cadence is the RTO knob: more frequent ⇒ shorter recovery + more FPI volume.

## 12. Recovery `[PROPOSED]`

Per-core, parallel, restartable — each stream recovers independently (valid while transactions are core-local):

1. **Analysis:** from the superblock's redo start, scan forward to the durable end (§4.2 torn-tail rule); rebuild the dirty-page and transaction tables; classify winners (commit record seen) and losers.
2. **Redo:** replay everything idempotently (§9), restoring `FULL_PAGE_IMAGE`s first per page. Redo reconstructs the crash-time state including uncommitted changes and undo pages.
3. **Undo:** roll back losers through their undo chains (`undo_ptr`), emitting compensation as ordinary logged page mutations so undo itself is crash-restartable.
4. Recovery completion writes a checkpoint, bounding the next crash's work.

A crash during recovery at any point resumes correctly — enforced by the test matrix (§15).

## 13. Business & Operational Features

- **Configuration:** global durability class + per-transaction override; flush interval for `D3`; segment size; checkpoint cadence; retention.
- **Archiving / PITR `[PROPOSED]`:** sealing a segment fires an archive hook (callback seam). Archived streams + a base backup give point-in-time recovery; PITR tooling itself is out of scope here but the log is designed to be sufficient for it (complete, checksummed, self-delimiting).
- **Replication readiness:** the same sealed-segment/stream tap is the future log-shipping source. No format concession is needed now; the constraint is only "never break §4 self-description".
- **Observability (ship with the first flush path):** durable LSN vs appended LSN per core, flush latency percentiles, group-commit batch sizes, ring-full stall time, checkpoint duration, FPI volume share, recovery phase timings. Financial operators tune RPO/RTO with these; they are product features, not debug aids.

## 14. Required Amendments (gate for implementation)

1. **Design spec — page headers:** add `page_lsn` (all page types); add page checksum if §10(a) is adopted. On-disk format rules apply. Stamp dates.
2. **Design spec — heap section:** note `PAGE_INIT` as the sole logger of `min_key` (immutability preserved by construction).
3. **Superblock spec:** per-core WAL anchors (current segment, durable LSN, redo start).
4. **`docs/page-management.md`:** bind `MarkClean`/flush to §8-1; note WAL bypasses `PageStore`; SpaceManager emits `ALLOC/FREE`.
5. **`docs/waystone-concept.md`:** record unlogged-by-default (§2) against its open persistence item once adopted.
6. **`CLAUDE.md`:** add WAL opens (§15) to the open-decision list; summary line for the durability model.

## 15. Open Decisions — do not assume

- Torn-page protection choice (§10) and the page-checksum field.
- Segment size; ring capacity; `D3` flush interval defaults.
- Cross-core transaction commit protocol; recovery under changed core counts (§3).
- I/O backend (inherited open); whether flush uses device flush (FUA/fsync) semantics exposed by `IoBackend` — define the seam's durability verb precisely when the backend lands.
- Undo-page layout details (owned by the transaction/MVCC spec, referenced here only as `UNDO_WRITE` targets).
- Archive hook transport (filesystem copy vs pluggable).

## 16. Testing Requirements

All deterministic (injected clock + `IoBackend` fault injection; rules.md §4). Crash-consistency tests are the shipping condition for every WAL-touching change:

1. **Format:** record/segment codec round-trips; alignment; torn-tail detection (truncate at every byte boundary of the last record).
2. **Ordering:** instrumented backend proves §8-1..3 hold under randomized scheduling — no data write ever precedes its log durability.
3. **Crash matrix:** inject crashes at every phase boundary (append / partial segment write / between commit-durable and ack / mid-checkpoint / each recovery phase); recovery yields exactly the acknowledged-commit state; replaying recovery twice is a no-op (idempotency via `page_lsn`).
4. **Torn writes:** partial-page and partial-record corruption injected; §10 mechanism restores; without FPI (pre-decision builds) the test documents the exposure rather than hiding it.
5. **Durability classes:** `D1/D2` never lose an acked commit under any injected crash; `D3` loss is bounded by the configured window — bound asserted, not assumed.
6. **Group commit:** N concurrent committers, one flush; all resume with durable LSN ≥ their commit LSN.
7. **Backpressure:** ring saturation suspends producers without deadlock; stall metrics visible.
8. **Per-core:** multi-stream recovery equals the union of independent single-stream recoveries.
