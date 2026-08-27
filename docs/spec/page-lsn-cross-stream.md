# Page LSN Across Streams

**RATIFIED 2026-08-24 by the operator: PL-B with the PL-C guard** — the
logged handoff over a flushed page, reinforced by an owning-stream stamp in
the page header. §9 states the binding form; §§6-8 are kept as the record
the choice was made from. PL-A, PL-D and PL-E are **declined** (PL-D may
still be *implemented first* as a stepping stone, but it is not the
contract; PL-A carries the named revisit clause in §9). Owning specs:
`docs/spec/wal.md` §3 and §15, `docs/inflight/in-progress/workplan-crosscore.md` guideline 3.
Scoped 2026-08-24 in the main checkout on `main` at `a755521`; ratified the
same day at `b53cdb0`. Every claim below is a read of the source with its
site named, not a measurement.

## 1. The decision, in one sentence

**When a page can be written by more than one WAL stream over its life,
what makes redo's idempotence test meaningful?**

Everything in the dynamic-page-ownership proposal — pages assigned to cores
at runtime, reorganised and evicted on statistics, every core equivalent —
sits on top of this. So does cross-core commit, and so does any mover that
frees a page on one core and reallocates it on another. The answer decides
the page header, the record set, and the shape of recovery, which is why it
is named before any of them is built.

## 2. The invariant chain as built

Four facts, each with its site. Together they are why the question exists.

1. **One WAL stream per core, and an LSN is a stream-local byte offset.**
   `include/kds/wal/record.hpp:29` and `include/kds/wal/stream.hpp:24` say
   it; `docs/spec/wal.md` §3 states the consequence — *"No global LSN;
   cross-stream ordering is not required **while transactions are
   core-local**."* That trailing clause is the hinge: the rule was always
   conditional, and this decision is the condition coming due.
2. **LSNs are never compared across cores.** `workplan-crosscore.md`
   guideline 3, stated as an invariant for every phase of that milestone:
   *"Nothing in this milestone may create a cross-stream ordering
   dependency — that is what keeps recovery per-core and the 2PC door
   safely closed."*
3. **Redo's whole idempotence rule is one comparison against one field.**
   RV5, `include/kds/wal/redo.hpp:20`: a record is applied iff
   `record.lsn > page_lsn` of the page it names, and applying it stamps
   `page_lsn = record.lsn`. Built at `src/wal/redo.cpp:322` (the skip) and
   `:386` (the stamp).
4. **A page carries exactly one `page_lsn`, eight bytes at offset 8**
   (`include/kds/storage/page_header.hpp:52`), with `kNoPageLsn = 0`
   meaning "never logged". There is no field naming which stream that
   number belongs to, because until now there could only be one.

## 3. The failure, spelled out

Page P is owned by core A and written at A-LSN 900. P migrates to core B —
by any mechanism — and B writes it at B-LSN 40, stamping `page_lsn = 40`.
The instance crashes. Recovery runs per core (`docs/workplan-wal-recovery.md`
RC01-RC11), each stream independently.

- **A's redo** reaches its record for P at A-LSN 900, reads `page_lsn = 40`,
  finds `900 > 40`, and **re-applies a stale mutation over B's newer
  content.** The comparison is arithmetically fine and semantically
  meaningless: the two numbers are byte offsets into different files.
- Reverse the numbers and the other failure appears: A-LSN 40 against a
  page stamped at B-LSN 900 **skips** a mutation that was never applied.

Neither is detected. Nothing in the record, the page, or the checkpoint
carries the fact that would let redo notice. The page checksum is
recomputed at flush (`page_header.hpp:162`) and is valid either way, so
the corruption reads as a healthy page.

This is not a race and no locking prevents it. It is a **naming collision
between two LSN spaces**, and it survives any amount of care on the
runtime side.

## 4. What is waiting on this

| Waiting item | Why |
|---|---|
| **PW1c — a peer writing a rotated relation's creation pages (decided 2026-08-24, the contract's first consumer)** | those pages are formatted and logged by core 0 at DDL, so the first peer write is exactly §3's transition; `workplan-peer-writer.md` §8 |
| Dynamic page-to-core assignment, the statistics-driven mover | its whole premise is that a page changes owner |
| "Every core equivalent" (retiring `workplan-crosscore.md` M5) | a catalog page written by any core is written by any stream |
| Cross-core commit / 2PC (`wal.md` §3 `[OPEN]`) | one transaction's records in two streams |
| Free-map reclamation across cores (`physical-optimizer.md` §6 gate 3) | a page freed by A and reallocated to B is the same collision |
| Recovery under a changed core count (`wal.md` §3 `[OPEN]`) | stream reassignment moves every page of a stream at once |

`docs/spec/crosscore.md` CC7's flush-then-grant handoff is the one place the
engine already moves pages between cores, and it survives today only
because it happens at **DDL publish** on a relation whose pages core 0 just
flushed — the pages are durable and quiescent at the boundary, and no peer
has ever logged against them. That is not a general property; it is the
special case, and §6 below is partly about generalising it.

## 5. Where a new field could go — the header is full

Relevant if the answer needs to record something per page. The common
header is 32 bytes and **both reserved words are already spent**
(`docs/spec/page.md` §2 and §2a): `relayout_epoch` took offset 16 in PX03,
`owner_oid` took offset 24 on 2026-08-13.

| Candidate | Size | Cost |
|---|---|---|
| `page_flags` at offset 2 | 16 bits, **entirely unused** — no bit is defined in this build (the heap/btree/varheap `flags` constants live in each type's own sub-header at `kPageBodyOffset`, not here) | free; caps stream ids at 65536, far above `kMaxWalCores` = 64 |
| High bits of `page_lsn` | records are 8-byte aligned (`record.hpp:228`) and a stream would need 2^56 bytes ≈ 72 PB to reach bit 56 | free, but overloads the field the decision is about, and every LSN read/write site must mask |
| Grow the header past 32 bytes | 8+ bytes | **a real format event**: `kPageBodySize` is 8160 and every relation's tuples-per-page is derived from it, so this is a migration, not a bump |

The first two are format-silent in the sense §2a established — a field
carved out of space every existing page already reads as 0.

## 6. The options — the record the ratification was made from

Kept as written before the choice. §9 is the binding form.

### PL-A — One global LSN

Retire per-stream LSN spaces; a single monotonic counter across all cores.
Every comparison becomes meaningful everywhere, permanently.

- **Costs** a shared atomic on the append path — precisely the contention
  point per-core streams were built to avoid (`wal.md` §3: *"no shared tail
  pointer, no lock, no atomic contention on the append path"*). Retracts
  `workplan-crosscore.md` guideline 1's "no atomics outside ring indices".
- Streams may stay separate *files*; only the number space is shared.
- **Subsumes** this decision, the 2PC ordering question, and the
  core-count-change question in one mechanism.
- Foreclosed by nothing; forecloses the append-path claim above.

### PL-B — Logged handoff over a flushed page

Generalise CC7. A migration is: the outgoing owner flushes the page,
appends a **handoff record**, and only then does the incoming owner take
it. Recovery's analysis pass learns from that record that the page left the
stream at LSN *h*, and **removes it from that stream's dirty page table**
(`include/kds/wal/analysis.hpp`, `dirty_pages`) so redo never touches it.

- **Soundness rests on the flush**: everything the outgoing stream logged
  for that page before *h* is already in the durable image, so its redo has
  nothing left to contribute. No LSN is ever compared across streams — only
  the *fact* of the handoff crosses, which guideline 3 permits and an
  ordering dependency would not.
- **Costs** one new record type, an analysis change, and an ordering rule:
  handoff records must be processed before redo scope is decided.
- **Costs a flush per migration**, which prices the mover's policy — a
  page that moves often pays a device write every time.
- Leaves `wal.md` §3 intact.

### PL-C — Stamp the owning stream in the page

Record which stream's space `page_lsn` lives in (§5 has two free
locations). Redo applies a record only when the stamp names its own stream.

- Alone it is **not sufficient**: when the stamp names another stream,
  redo learns that its number is incomparable but not whether the record
  was already applied. It needs PL-B's flush, or PL-A, to answer that.
- As a **reinforcement of PL-B** it is cheap and valuable: it turns a lost
  or mis-ordered handoff record from silent corruption into a detectable
  `Corruption`, which is the discipline invariant 13 already applies to a
  disagreeing length.

### PL-D — Migrate only at a quiescent boundary

Permit migration only where both streams are checkpointed and idle — at
mount, at a checkpoint, or under an explicit drain.

- **Cheapest by a wide margin**; needs no format change and no new record.
- **Undercuts the premise**: a statistics-driven mover reacting to a hot
  page cannot wait for a checkpoint. This is an interim that makes
  relation-granular rebalancing and core-count changes legal, not the
  fine-grained dynamic assignment the proposal describes.

### PL-E — The stream follows the relation, not the core

A relation's records always land in that relation's stream, whichever core
wrote them. A page's records are then single-stream by construction.

- Removes the problem rather than managing it.
- **Costs** a stream per relation (or per relation group) — file count,
  per-stream checkpoint state, and the 64-slot anchor table in the
  superblock (`include/kds/server/superblock.hpp`) becomes the wrong shape.
- A transaction touching two relations writes two streams, so it **needs
  the 2PC it was meant to avoid**. Named here so it is not rediscovered as
  novel.

## 7. What should decide it

1. **Append-path cost.** PL-A puts an atomic on the hottest path in the
   engine; the others do not. Measured, not argued — `build-release`,
   interleaved A/B, per the standing rule.
2. **Migration frequency the mover actually wants.** PL-B's flush-per-move
   is cheap at relation granularity and expensive at page granularity with
   a hot page. That number comes from the mover's policy, which does not
   exist yet.
3. **Whether cross-core commit is wanted.** If 2PC is on the road, PL-A
   pays for two decisions at once and the comparison changes.
4. **Detectability.** Every option except PL-C fails silently when its
   own rule is violated. That is worth weighting: the failure in §3 is
   invisible, and the engine's standing preference is `Corruption` over
   interpretation.

## 8. CLA's reading — superseded by §9, kept as the record

**PL-B is the answer that fits the engine as built, with PL-C as its
guard-rail, and PL-A is the honest end state if cross-core commit is ever
wanted.** PL-B generalises a handoff CC7 already ships, keeps `wal.md` §3
and guideline 3 intact, and moves only a *fact* across streams rather than
an ordering. PL-C costs 16 free bits and converts the silent failure into a
loud one. PL-D is a legitimate first step — it makes core-count changes and
relation rebalancing legal without deciding anything — but it should be
adopted knowing it is not the destination.

This reading was put to the operator and **ratified 2026-08-24**; §9 is
what binds.

## 9. The ratified contract — PL-B with the PL-C guard

Binding from 2026-08-24. Five rules; everything else in this file is
context.

1. **A page changes streams only through a logged handoff.** The outgoing
   owner (a) flushes the page durable, (b) appends a **handoff record** to
   its own stream naming the page id, the incoming core, and the
   outgoing stream's LSN at the handoff, and (c) only after that record is
   durable is the incoming owner granted fault/write rights. Order (a) →
   (b) → (c) is a correctness statement, not a preference: the flush is
   what makes rule 3's redo exclusion sound, and the durable record is
   what makes the grant recoverable.
2. **The handoff moves a fact, never an ordering.** No LSN is ever
   compared across streams; `wal.md` §3 and `workplan-crosscore.md`
   guideline 3 stand unamended.
3. **Analysis processes handoff records before redo scope is fixed.** A
   page handed off at LSN *h* is removed from the outgoing stream's dirty
   page table (`include/kds/wal/analysis.hpp`, `dirty_pages`); the outgoing stream's
   redo never touches it. Sound because of rule 1(a): everything that
   stream logged for the page before *h* is already in the durable image.
   The incoming stream's records for the page replay normally.
4. **The PL-C guard: the owning stream is stamped in `page_flags`.** The
   16-bit word at offset 2 (`include/kds/storage/page_header.hpp:50`,
   verified unused in this build — every heap/btree/varheap flag constant
   lives in its type's own sub-header) carries **`core_id + 1`** of the
   stream that last wrote the page; **0 means never stamped**, which is
   what every existing page already reads — the exact no-backfill
   precedent `owner_oid` set in `docs/spec/page.md` §2a. `kMaxWalCores` = 64
   fits with room. The LSN-high-bits alternative (§5) is **rejected**: it
   overloads the field under decision and taxes every LSN site with a
   mask.
5. **A stamp mismatch redo can reach is `Corruption`, never a skip.** If a
   stream's redo reaches a page whose stamp names another stream (both
   nonzero) and analysis saw no handoff moving that page out, a handoff
   record was lost or mis-ordered: the mount refuses, loudly. An unstamped
   page (0) takes today's comparison unchanged — correct, because a page
   that never crossed streams has a meaningful `page_lsn`, and no page may
   cross without acquiring a stamp on the way.

5a. **Retracted the day it was written (2026-08-24).** A first form of
   the returned-page rule keyed a redo bypass on
   `AnalysisResult::handed_off` — pages whose departure this stream's
   *scanned window* saw. Its own review (the f19ead1 review's C2) showed
   the key is wrong in principle: the stamp is a durable, cross-mount
   fact, and any case where the fact outlives the window that explains
   it became a false `Corruption` — above all the plain *receiving*
   core, whose own log never holds the handoff at all (rule 1b puts it
   in the outgoing stream), and whose healthy first-crash state was
   byte-identical to the violation the rule refused. Rule 6 replaces it;
   kept here because a ratified-form retraction is recorded, never
   erased.

6. **The acquisition restamp (2026-08-24, from the f19ead1 review's C2;
   built with PW1c-3's rework and PW1c-4's grant path).** The incoming
   owner, after the grant and **before its first logged write** to the
   page, appends a PAGE_HANDOFF to its *own* stream naming itself as the
   incoming core — the acquisition record — and durably rewrites the
   header pair: `page_flags := own core_id + 1`, `page_lsn := that
   record's LSN` — and flushes the page. The LSN must name a logged
   record, not the bare append point: the WAL gate refuses a page
   claiming a record that was never logged, which is what forced the
   acquisition to be a record at all — and the record doubles as the
   receiver's durable acquisition fact. Analysis's rule-3 erase reads
   the record correctly in either direction: below its LSN, this
   stream's redo owes the page nothing. Three consequences carry the
   design:
   - **Rule 5 holds at full strength with no exception**: every
     legitimate crossing stamps the receiver before any of the
     receiver's records for the page exist, so a foreign stamp redo can
     reach means a lost handoff or a lost restamp, nothing else. The
     returned page (A→B→A) is just another acquisition.
   - **`page_lsn` is always an offset in its owner's space**, at a value
     at or above everything already reflected and below every future
     record — so RV5 governs every page, always, and no bypass exists.
   - **Crash-safe by construction**: a crash between the giver's rule-1a
     flush and the receiver's restamp leaves the page durably the
     giver's, which is the pre-grant state — the receiver logged
     nothing, so its redo owes the page nothing.
   **Precondition, stated because rule 3's erase reads the acquisition
   record too**: an acquisition record is legal only when everything its
   stream logged for the page below it is already durable. The receive
   path makes that true **by construction**: it flushes the granted
   pages *before* appending the acquisition (free at first contact,
   load-bearing on a re-grant after a remount, where replayed-but-
   unflushed writes can sit on the frame), and a page already holding
   write rights takes no second acquisition at all — rights granted this
   run, or **a stamp that already names the receiver**, which is the
   durable form of the same fact (amended 2026-08-24 with PW1c-7: a
   re-delivery after a remount names pages whose stamp already says so,
   and restamping them again would only dirty and flush them for a fact
   the page already states).
   Cost: one page write and flush per handoff, beside the flush rule 1a
   already pays. This also closes the unstamped-crossing gap: creation
   pages cross at DDL publish carrying stamp 0 today (`LogPageInit` does
   not stamp), and the restamp is what stamps them — the crossing
   itself, not the writer's goodwill.
   - **The stamp is the durable form of ownership** (2026-08-24,
     `docs/inflight/in-progress/workplan-peer-writer.md` PW1c-7). Every lease and grant a
     core holds is memory-resident, so after a restart the stamp is the
     only statement of whose page this is — complete by rule 4 (every
     write stamps) and exact by this rule (no page leaves a stream
     unrestamped). A leased store therefore **claims** a page whose
     stamp names its own stream, for reads and writes alike, when no
     lease or grant covers it (`DevicePageStore::TryClaimByStamp`); a
     foreign stamp or 0 claims nothing, and a creation page never
     acquired is re-delivered by the giver on request. Binding on the
     mover: a migration must **revoke** the giver's lease ownership of
     the page as well as restamp it, or the giver's `LeasedIdSource`
     keeps admitting a write the stamp no longer allows.

Consequences that bind other work:

- **Cross-core free-map reclamation is a handoff** (`docs/
  physical-optimizer.md` §6 gate 3): a page freed by one core and
  reallocated to another crosses streams and takes rules 1-5 like any
  migration.
- The mover's policy pays **one flush per migration** (rule 1a); pricing
  that against migration frequency is the blueprint's R5 concern.
- The handoff record's kind, payload layout and its `kPad`-style envelope
  details are **workplan items**, not open decisions — they follow
  `wal.md` §4's record discipline when built.
- **Revisit clause**: if cross-core commit (2PC) is ever ratified, PL-A is
  re-opened *by that decision* — one global LSN may then pay for both.
  Until then it is declined, not deferred.

## 10. Explicitly not in scope

- The mover's policy, the frame directory, and the statistics that would
  drive them. Those are downstream of this and are argued elsewhere.
- 2PC itself (`wal.md` §3, `crosscore.md` §9).
- Whether page ownership becomes page-granular at all. This decision is
  required by that proposal but is **not an argument for it** — it is
  equally required by cross-core commit, by free-map reclamation across
  cores, and by a changed core count.
