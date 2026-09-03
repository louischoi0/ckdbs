# Page LSN Across Streams

> **Superseded in part on 2026-09-02, by AR0 M0** (the single WAL stream;
> `instructions/v3.0.0/workorder-al-m0-single-wal.md`, AL-R6). A database
> created by this build or later has **one stream**, so the question this
> document answers — what makes redo's idempotence test meaningful when a
> page is written by more than one stream — does not arise for it. A volume
> written before the change still says `kPerCoreStreams` in its superblock
> and every rule below still governs it, which is why this document is
> marked rather than deleted.
>
> What survives under one stream, and what does not:
>
> | | under one stream |
> |---|---|
> | **§6's rejection of PL-A** (one global LSN, "a shared atomic on the append path") | **Reversed.** PL-A is what M0 built. The serialization is a **latch**, not an atomic — the reserve/copy/publish split that would have made it an atomic was tried and abandoned (AL-R1) — and it is the cost AR0-2 accepts; `wal/stream.hpp` carries its justification and AL-S8 prices it |
> | **§9 rule 4**, the stamp riding the page_lsn | **Narrowed to a claim.** The stamp still says which core owns a page — `device_page_store`'s claim-at-fault reads it — but it no longer says which log the page's records are in, because there is one. Redo does not restamp, and the mount's undo phase does not either (`SetStampSuppressed`) |
> | **§9 rules 5-6**, a foreign stamp is `Corruption` | **Not in force.** There is no other stream to have crossed from |
> | **§9 rules 1-3**, the logged handoff over a flushed page | **Still in force**, because ownership is still per core in M0. What changed is analysis: the dirty-table erase is **skipped**, because what licenses it is rule 1(a)'s *flush*, and a flush covers one core's page store while with one log the erase would speak for every core's records (AL-R6, as amended by AL-7d) |
> | **§2's invariant chain** (one stream per core, LSNs never compared across streams) | **The premise, not the conclusion.** It is what M0 removed |
>
> AR0's own §7 lists this document as "superseded"; that overstates it, and
> AL-R6 is the correction.

The contract for a page that more than one WAL stream writes over its
life: **PL-B, the logged handoff over a flushed page, with the PL-C
guard, an owning-stream stamp in the page header.** §9 is the binding
form; §§1-5 state the problem it answers. PL-A (one global LSN), PL-D
(migration only at a quiescent boundary) and PL-E (a stream per relation)
are **declined** (§6). Owning specs: `docs/spec/wal.md` §3 and §15,
`docs/spec/crosscore.md`.

## 1. The decision, in one sentence

**When a page can be written by more than one WAL stream over its life,
what makes redo's idempotence test meaningful?**

Every mechanism that moves a page between cores sits on top of this —
cross-core commit, a mover that frees a page on one core and reallocates it
on another, a changed core count. The answer decides the page header, the
record set, and the shape of recovery.

## 2. The invariant chain as built

Four facts, each with its site. Together they are why the question exists.

1. **One WAL stream per core, and an LSN is a stream-local byte offset.**
   `include/kds/wal/record.hpp` and `include/kds/wal/stream.hpp` say it;
   `docs/spec/wal.md` §3 states the consequence — *"No global LSN;
   cross-stream ordering is not required while transactions are
   core-local."* That trailing clause is the hinge: the rule is
   conditional, and this contract is the condition coming due.
2. **LSNs are never compared across cores** (`docs/spec/wal.md` §3).
   Nothing may create a cross-stream ordering dependency — that is what
   keeps recovery per-core.
3. **Redo's whole idempotence rule is one comparison against one field.**
   `include/kds/wal/redo.hpp`: a record is applied iff
   `record.lsn > page_lsn` of the page it names, and applying it stamps
   `page_lsn = record.lsn` (`src/wal/redo.cpp`).
4. **A page carries exactly one `page_lsn`, eight bytes at offset 8**
   (`include/kds/storage/page_header.hpp`), with `kNoPageLsn = 0`
   meaning "never logged". The field does not say which stream that
   number belongs to; the stamp of §9 rule 4 does.

## 3. The failure, spelled out

Page P is owned by core A and written at A-LSN 900. P migrates to core B —
by any mechanism — and B writes it at B-LSN 40, stamping `page_lsn = 40`.
The instance crashes. Recovery runs per core, each stream independently.

- **A's redo** reaches its record for P at A-LSN 900, reads `page_lsn = 40`,
  finds `900 > 40`, and **re-applies a stale mutation over B's newer
  content.** The comparison is arithmetically fine and semantically
  meaningless: the two numbers are byte offsets into different files.
- Reverse the numbers and the other failure appears: A-LSN 40 against a
  page stamped at B-LSN 900 **skips** a mutation that was never applied.

Neither is detected by the numbers alone. Nothing in the record or the
checkpoint carries the fact that would let redo notice, and the page
checksum is recomputed at flush and is valid either way, so the corruption
reads as a healthy page.

This is not a race and no locking prevents it. It is a **naming collision
between two LSN spaces**, and it survives any amount of care on the
runtime side.

## 4. What depends on this

Every page movement between streams takes §9's handoff:

- a peer's first write to pages core 0 formatted and logged at DDL — the
  flush-then-grant at DDL publish (`docs/spec/crosscore.md` CC7) is the
  easiest case, because the pages are durable and quiescent at the
  boundary and no peer has logged against them;
- cross-core commit, whose records for one transaction sit in two streams
  (`docs/spec/cross-owner-txn.md`);
- a page freed by one core and reallocated to another;
- any mover, and recovery under a changed core count, both of which change
  a page's owner.

## 5. Where the stamp lives — the header is full

The common header is 32 bytes and both reserved words are spent
(`docs/spec/page.md` §2 and §2a): `relayout_epoch` at offset 16,
`owner_oid` at offset 24.

| Candidate | Size | Cost |
|---|---|---|
| `page_flags` at offset 2 | 16 bits, otherwise unused — the heap/btree/varheap `flags` constants live in each type's own sub-header at `kPageBodyOffset`, not here | free; caps stream ids at 65536, far above `kMaxWalCores` = 64 |
| High bits of `page_lsn` | records are 8-byte aligned and a stream would need 2^56 bytes ≈ 72 PB to reach bit 56 | free, but overloads the field the decision is about, and every LSN read/write site must mask |
| Grow the header past 32 bytes | 8+ bytes | **a real format event**: `kPageBodySize` is 8160 and every relation's tuples-per-page is derived from it, so this is a migration, not a bump |

The stamp is in `page_flags` (§9 rule 4). The first two candidates are
format-silent in the sense `page.md` §2 states — a field carved out of
space every existing page already reads as 0.

## 6. The options

- **PL-A — one global LSN: declined.** It puts a shared atomic on the
  append path, precisely the contention point per-core streams exist to
  avoid (`wal.md` §3: *"no shared tail pointer, no lock, no atomic
  contention on the append path"*). Cross-core commit is not a reason to
  re-open it (§9).
- **PL-B — logged handoff over a flushed page: the contract** (§9).
- **PL-C — stamp the owning stream in the page: adopted as PL-B's guard**
  (§9 rule 4). Alone it is insufficient: a foreign stamp tells redo its
  number is incomparable, not whether the record was already applied.
- **PL-D — migrate only at a quiescent boundary: not the contract.** It
  needs no format change and no record, and it cannot serve a mover that
  reacts to a hot page.
- **PL-E — the stream follows the relation: declined.** A transaction
  touching two relations would write two streams and need the 2PC it was
  meant to avoid, and the 64-slot anchor table in the superblock
  (`include/kds/server/superblock.hpp`) is the wrong shape for it.

## 7. What decided it

Decided; §9 is the outcome. Detectability weighed most: every option but
PL-C fails silently when its own rule is violated, and the engine's standing
preference is `Corruption` over interpretation.

## 8. CLA's reading

Superseded by §9.

## 9. The ratified contract — PL-B with the PL-C guard

Six rules; everything else in this file is context.

1. **A page changes streams only through a logged handoff.** The outgoing
   owner (a) flushes the page durable, (b) appends a **handoff record**
   (`PAGE_HANDOFF`, `include/kds/wal/record.hpp`) to its own stream naming
   the page id, the incoming core, and the outgoing stream's LSN at the
   handoff, and (c) only after that record is durable is the incoming
   owner granted fault/write rights. Order (a) → (b) → (c) is a
   correctness statement, not a preference: the flush is what makes rule
   3's redo exclusion sound, and the durable record is what makes the
   grant recoverable.
2. **The handoff moves a fact, never an ordering.** No LSN is ever
   compared across streams; `wal.md` §3 stands unamended.
3. **Analysis processes handoff records before redo scope is fixed.** A
   page handed off at LSN *h* is removed from the outgoing stream's dirty
   page table (`include/kds/wal/analysis.hpp`, `dirty_pages`); the
   outgoing stream's redo never touches it. Sound because of rule 1(a):
   everything that stream logged for the page before *h* is already in
   the durable image. The incoming stream's records for the page replay
   normally.
4. **The PL-C guard: the owning stream is stamped in `page_flags`.** The
   16-bit word at offset 2 (`include/kds/storage/page_header.hpp`)
   carries **`core_id + 1`** of the stream that last wrote the page;
   **0 means never stamped**, which is what a page written before the
   stamp existed reads — no backfill, the same rule as `owner_oid`
   (`docs/spec/page.md` §2). `kMaxWalCores` = 64 fits with room. The
   LSN-high-bits alternative (§5) is **rejected**: it overloads the field
   under decision and taxes every LSN site with a mask.
5. **A stamp mismatch redo can reach is `Corruption`, never a skip.** If a
   stream's redo reaches a page whose stamp names another stream (both
   nonzero) and analysis saw no handoff moving that page out, a handoff
   record was lost or mis-ordered: the mount refuses, loudly. An unstamped
   page (0) takes the ordinary comparison unchanged — correct, because a
   page that never crossed streams has a meaningful `page_lsn`, and no
   page may cross without acquiring a stamp on the way. The rule holds
   with no exception because of rule 6: every legitimate crossing stamps
   the receiver before any of the receiver's records for the page exist,
   so a foreign stamp redo can reach means a lost handoff or a lost
   restamp, nothing else — a returned page (A→B→A) is just another
   acquisition. No redo bypass keyed on what analysis's scanned window saw
   may exist: the stamp is a durable, cross-mount fact, and a window-keyed
   bypass would turn a healthy receiving core's first crash into a false
   `Corruption`, since the handoff sits in the *outgoing* stream's log.
6. **The acquisition restamp.** The incoming owner, after the grant and
   **before its first logged write** to the page, appends a `PAGE_HANDOFF`
   to its *own* stream naming itself as the incoming core — the
   acquisition record — and durably rewrites the header pair:
   `page_flags := own core_id + 1`, `page_lsn := that record's LSN` — and
   flushes the page. The LSN must name a logged record, not the bare
   append point: the WAL gate refuses a page claiming a record that was
   never logged, and the record doubles as the receiver's durable
   acquisition fact. Analysis's rule-3 erase reads the record correctly
   in either direction: below its LSN, this stream's redo owes the page
   nothing. Consequences:
   - **`page_lsn` is always an offset in its owner's space**, at a value
     at or above everything already reflected and below every future
     record — so redo's rule governs every page, always, and no bypass
     exists.
   - **Crash-safe by construction**: a crash between the giver's rule-1a
     flush and the receiver's restamp leaves the page durably the
     giver's, which is the pre-grant state — the receiver logged
     nothing, so its redo owes the page nothing.
   - **Precondition, because rule 3's erase reads the acquisition record
     too**: an acquisition record is legal only when everything its
     stream logged for the page below it is already durable. The receive
     path makes that true by construction: it flushes the granted pages
     *before* appending the acquisition (free at first contact,
     load-bearing on a re-grant after a remount, where replayed-but-
     unflushed writes can sit on the frame), and a page already holding
     write rights takes no second acquisition — rights granted this run,
     or a stamp that already names the receiver, which is the durable
     form of the same fact; restamping such a page would only dirty and
     flush it for a fact it already states.
   - Cost: one page write and flush per handoff, beside the flush rule 1a
     already pays.
   - Creation pages cross at DDL publish carrying stamp 0 (`LogPageInit`
     does not stamp); the restamp is what stamps them — the crossing
     itself, not the writer's goodwill.
   - **The stamp is the durable form of ownership.** Every lease and grant
     a core holds is memory-resident, so after a restart the stamp is the
     only statement of whose page this is — complete by rule 4 (every
     write stamps) and exact by this rule (no page leaves a stream
     unrestamped). A leased store therefore **claims** a page whose stamp
     names its own stream, for reads and writes alike, when no lease or
     grant covers it (`DevicePageStore::TryClaimByStamp`); a foreign
     stamp or 0 claims nothing, and a creation page never acquired is
     re-delivered by the giver on request. Binding on any mover: a
     migration must **revoke** the giver's lease ownership of the page
     as well as restamp it, or the giver's `LeasedIdSource` keeps
     admitting a write the stamp no longer allows.

Consequences that bind other work:

- **Cross-core free-map reclamation is a handoff**: a page freed by one
  core and reallocated to another crosses streams and takes rules 1-6
  like any migration.
- A migration pays **one flush** (rule 1a) plus the restamp's page write
  and flush (rule 6).
- The handoff record's payload and envelope follow `wal.md` §4's record
  discipline.
- **PL-A stays declined, and cross-core commit is not a reason to re-open
  it.** 2PC as built (`docs/spec/cross-owner-txn.md`) makes no
  cross-stream comparison: the decision lives in exactly one stream, the
  protocol's wire payloads carry no LSN, per-participant trx ids stay
  stream-local, and recovery-time resolution is a lookup of one id in one
  file. The one comparison it makes is within the coordinator's own
  stream — the scan must reach the durable point that stream's own anchor
  was published with (`src/server/prepared_resolver.cpp`) — and a within-stream
  completeness check another core happens to run is not a crossing. A
  prepared transaction lowers its core's redo start, so a scan meets
  *more* handoff records in order, which rule 3's forward scan and rule
  6's restamp already handle; nothing in 2PC raises a redo start past a
  handoff. Re-opening PL-A takes a fresh operator decision with a reason
  that is not 2PC, against its unchanged price: an atomic on the engine's
  hottest append path.

## 10. Explicitly not in scope

- The mover's policy, the frame directory, and the statistics that would
  drive them.
- 2PC itself (`wal.md` §3, `crosscore.md` §9, `cross-owner-txn.md`).
- Whether page ownership becomes page-granular at all. This contract is
  required by that question but is **not an argument for it** — it is
  equally required by cross-core commit, by free-map reclamation across
  cores, and by a changed core count.
