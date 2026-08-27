# Bulk ingestion: multi-row VALUES and the KWP load stream

How rows arrive in KDS in quantity. Three tiers exist by design; **v1 ships
Tier 1 and Tier 2 and deliberately reserves Tier 3**, and this document says
exactly where the line sits and why. `[PROPOSED]` marks a default to confirm
or amend before the affected part is built; `[OPEN]` must not be assumed;
`[GATED]` names an obligation blocked on an unbuilt subsystem. Companion
workplan: `docs/inflight/blocked/workplan-bulk-insert.md` (to be written from this spec).
Consistent with `docs/spec/protocol.md`, `docs/spec/wal.md`, `docs/spec/assertion.md`,
`docs/spec/index.md`, `docs/spec/cabin.md`, `docs/spec/eviction.md`,
`docs/rules/rules.md`.

**Status: nothing here is built.** Today `Parser::ParseInsert()` accepts a
single row and `CommandDispatcher::InsertInner()` executes exactly one row per
statement. The measured consequence (`bench/results-scenario1-vs-pg.md`):
with the durability point fully amortized at batch-1000, throughput ceilings
at ~9K rows/s because the ~21 µs/row statement cost — parse, compile, per-row
dispatch, one round trip per statement — never amortizes. This spec is that
21 µs's eviction notice.

---

## 0. Decision Record

| # | Decision | Choice |
|---|---|---|
| BI1 | Scope | Three tiers: **T1** multi-row `VALUES`, **T2** KWP binary load stream, **T3** sorted fast-path build. **v1 = T1 + T2. T3 is reserved** (§8): documented, prerequisites named, not built |
| BI2 | Correctness shortcut | **None, ever.** Every row through T1/T2 runs the full single-row write pipeline — FK check, assertion admission, Keystone allocation, encode + var-heap spill, placement, Cabin witness, index maintenance, WAL — **in the same per-row order** as `InsertInner` today (§5). Bulk buys amortization of parse/framing/round-trip, never of authority |
| BI3 | T1 grammar | `INSERT INTO t VALUES (…), (…) [, …]` — comma-separated row lists; per-statement row cap `kds.max_insert_rows` (default **1024** `[PROPOSED]`) |
| BI4 | Atomicity | A bulk statement (T1) and a load session (T2) are **atomic**: all rows or none, unwound by the existing transaction scope. Any per-row refusal fails the whole statement with the 1-based row ordinal in the message. No partial-accept mode, no error table, no slow path |
| BI5 | Fingerprint | An N-row INSERT fingerprints **identically to the 1-row INSERT** on the same relation — row count is not part of the template. Existing 1-row fingerprints are byte-stable (the recurring constraint, cf. `types.md` TY3). T2 never parses, so it never fingerprints |
| BI6 | T2 protocol | New KWP frames `C_LOAD_BEGIN / C_LOAD_CHUNK / C_LOAD_END / C_LOAD_ABORT` and `S_LOAD_READY / S_LOAD_ACK`, gated by a `BULK_LOAD` capability bit — no version break. Chunk rows use the **D5 row encoding already implemented in `wire/row_codec`**, the same codec below `S_ROW_BATCH` and crosscore `STEP_BATCH` (§4) |
| BI7 | Flow control | Windowed chunk acknowledgment: at most `window` unacknowledged chunks in flight (server-announced in `S_LOAD_READY`, default **4** `[PROPOSED]`), chunk payload ≤ announced byte cap (default **256 KiB** `[PROPOSED]`). Explicit and deterministic, in the spirit of §7 portal suspension — no TCP-buffer guesswork |
| BI8 | Durability | Orthogonal, unchanged: the transaction's WAL class applies (`C_TXN_BEGIN.durability`). **D3 relaxed is the documented recommendation for bulk load** — this is the use `wal.md` §1 named for it. No new class |
| BI9 | Keystone budget | Per-row `AllocateRowId` as today; a refused row still burns no id (admission precedes allocation, per row). An **aborted** bulk statement burns the ids of rows placed before the failure — accepted and documented as K1 40-bit budget consumption, same class of product constraint as the budget itself. No id pre-reservation in v1 (that is T3's, §8) |
| BI10 | WAL records | Per-row redo records exactly as today. A batched multi-row record type is `[OPEN]`, reserved for T3 — recovery (`wal.md` §12) stays untouched by v1 |
| BI11 | T2 transactions | A load session inside an explicit transaction joins it; outside one, the load is a single implicit transaction committing at `C_LOAD_END`. `C_LOAD_ABORT` (or connection loss mid-load) unwinds it. Cross-core: the target relation's home core, CC3 rules verbatim — a session write-bound to another core is refused before the first chunk |
| BI12 | Buffer pool | Load-filled pages run in the **EV6 small dedicated ring**: bounded frames, usage counters not bumped, filled pages handed to background writeback eagerly. A bulk load must never displace the foreground OLTP working set |
| BI13 | Observability | `S_COMPLETE` tag `LOAD` with `rows_affected`; per-core production counters (rows loaded, chunks, refusals, WAL-stall time); an ANALYZE line for T1 statements (T2 has no statement text to analyze) |
| BI14 | Resume / dedup | **None in v1.** A load has no resume token and the engine deduplicates nothing — replaying chunks after a crash duplicates rows. Restart-safety is the client's (truncate-and-reload) until a resume protocol `[OPEN]` earns its place |

---

## 1. What this is, and what it refuses to be

Bulk ingestion is a **maintenance-shaped write path**, not a query feature.
It does not touch the step-chain model, the "query is the plan" contract, or
any read path. It exists because the engine's primary user journey begins
with a migration — data arrives *before* the first pattern is ever observed —
and because the engine's own operations already queue up as consumers: the
reserved offline re-key pass (`docs/rules/keystoneid-invariant.md`), test-harness
seeding (`docs/inflight/in-progress/workplan-testing.md` S-1), benchmark preparation, and
crash-matrix data construction.

The one-sentence trust model, stated once and enforced everywhere below:

> **Bulk ingestion amortizes overhead, never authority.** Every structure
> that is authoritative on the single-row path — the clustered tree,
> secondary indexes, Bound Cabins and their assertions, FK enforcement, the
> Cabin completeness invariant C1, WAL-before-data — sees every bulk row
> exactly as it would have seen a single-row INSERT, in the same order.

This is BI2, and it is why the tiers are ordered the way they are: T1 and T2
remove costs that sit *around* the write pipeline (parsing, framing, round
trips); only T3 would reach *into* it, and that is precisely why T3 waits.

### Where the 21 µs goes, and which tier removes which part

| Cost | Removed by |
|---|---|
| one client round trip per row | T1 (N rows per statement), T2 (N rows per chunk, windowed) |
| one parse + text decode per row | T1 partially (one parse for N rows), T2 fully (binary, no parser) |
| one dispatcher entry + catalog lookup per row | T1/T2 (one `TableAccess` borrow per statement / load session) |
| per-row pipeline (FK, admission, id, encode, place, witness, index, log) | **nothing in v1** — this is the authority, BI2 |
| per-tuple tree descent + node splits | **T3 only** (§8) |
| one durability point per transaction | already solved: durability classes + explicit transactions |

---

## 2. Tier 1 — multi-row VALUES

### 2.1 Grammar

```
INSERT INTO <relation> VALUES ( <value> [, <value>]* ) [, ( … )]*
```

`ParseInsert()` grows one loop: after the closing `)` of a row, a comma
begins the next row. `InsertStmt.values` becomes `InsertStmt.rows`
(`std::vector<std::vector<AstValue>>`); the single-row case is a rows vector
of size one, and every existing caller reads `rows[0]` — the AST change is
mechanical and the 1-row parse is byte-identical in behavior.

Refused at parse, truthfully:

- more than `kds.max_insert_rows` rows → `InvalidArgument` naming the cap
  and the count. The cap bounds statement memory (all rows are held before
  execution begins) and bounds the id burn of BI9's abort case. It is a
  config key, not a compile-time constant `[PROPOSED: default 1024]`.
- an empty row list, or an empty row → `InvalidArgument`, as today.

### 2.2 Per-row validation, statement-scoped errors

Arity and the pk-column rule ("do not supply a value for primary-key
column…") are checked **per row**, and every error message carries the
1-based row ordinal: `row 37: expected 4 value(s)`. A statement that fails
on row 37 inserted nothing (BI4) — rows 1–36 are unwound by the same
`WriteScope` verdict rule `HandleInsert` applies today, and the ordinal is
what makes a 1,000-row refusal debuggable instead of a guessing game.

### 2.3 Execution

`InsertInner` becomes a loop over `rows` inside **one** write scope, one
catalog resolution, one affinity check. Each iteration is the existing
pipeline verbatim — order preserved per BI2, including admission *before*
allocation so a refused row burns no id (BI9), and including the assertion
reservation *after* placement so intra-statement group accumulation is
enforced: a statement inserting three rows into a group whose assertion
allows two more must fail on its own third row, and it does, because
reservation is per-row and admission row *k* sees rows 1..k−1's
reservations. This is why BI2 forbids validate-all-then-place-all: batched
validation would let a statement violate a group bound it satisfies row by
row.

### 2.4 Fingerprint (BI5)

The insert template hashes the relation and the shape, never the row count:
`INSERT INTO t VALUES (1, 'a')` and a 500-row insert into `t` are the same
pattern. Two reasons. Fragmenting `sys.patterns` by row count would turn one
workload fact into hundreds of rows of noise; and inserts have no trail
replay to begin with — the fingerprint's only consumer here is workload
statistics, which want the aggregate. The existing 1-row fingerprint bytes
do not change; `tests/` gets a pinned-fingerprint case saying so.

---

## 3. Tier 2 — the KWP load stream

The parser is the remaining per-row cost T1 cannot shed, and the migration
tool should not be paying text-encoding costs to talk to a binary protocol.
T2 is `COPY`-shaped: a framed binary row stream, decoded by the same
`wire/row_codec` that already encodes result rows and crosscore batches —
one codec, now with three readers, still knowing nothing about frames or
cores.

### 3.1 Frames

Gated by capability bit `BULK_LOAD` (client and server must both set it);
absent the bit, the frames are unknown types under the §4 rule.

| Frame | Payload | Notes |
|---|---|---|
| `C_LOAD_BEGIN` | `{relation str, flags u16, declared_rows u64}` | `declared_rows` 0 = unknown; informational (pre-sizing hint), never enforced. `flags` reserved 0 |
| `S_LOAD_READY` | `{load_id u64, window u16, max_chunk_bytes u32, field_count u16, fields: …}` | field descriptors are `S_ROW_DESC` fields for the columns **after the pk** — the schema the client must encode, stated by the server so drift is impossible |
| `C_LOAD_CHUNK` | `{load_id u64, chunk_seq u32, row_count u16, rows…}` | rows in D5 encoding per the announced fields; `chunk_seq` starts at 0, strictly increasing |
| `S_LOAD_ACK` | `{load_id u64, chunk_seq u32, rows_accepted u64}` | cumulative count; the window advances |
| `C_LOAD_END` | `{load_id u64}` | → `S_COMPLETE {tag "LOAD", rows_affected}` after the transaction's WAL ack point (implicit-txn case), or immediately inside an explicit txn (durability then rides `C_TXN_COMMIT`, as ever) |
| `C_LOAD_ABORT` | `{load_id u64}` | unwinds; `S_TXN_OK`-shaped ack |

One active load per session `[PROPOSED]`. A load session is modal: between
`C_LOAD_BEGIN` and `C_LOAD_END/ABORT`, the only frames accepted are load
frames, `C_PING`, and `C_TERMINATE`; anything else is `ERROR(PROTOCOL)`.
Errors follow the §5 contract exactly: on any `S_ERROR` the server discards
frames to the next `C_SYNC`, the load is dead, and the transaction is in
failed-txn state — chunks already accepted are unwound with it (BI4).

### 3.2 Flow control (BI7)

The client may have at most `window` chunks unacknowledged. This bounds
server-side buffering deterministically and gives WAL backpressure a place
to bite: a full WAL ring suspends the loading task (`wal.md` §2's one
sanctioned foreground stall), acks stop, the window closes, the client
stops. Backpressure propagates end to end with no special machinery.

### 3.3 What T2 skips, and what it cannot

Skipped: the lexer, the parser, per-statement dispatch, text→value
decoding, per-row round trips, fingerprinting (nothing parsed, nothing to
fingerprint — a bulk load is not workload evidence and must not pollute
`sys.patterns`).

Not skipped — BI2 verbatim: each decoded row enters the same per-row
pipeline as §2.3, same order, same assertion accumulation semantics, same
per-row ordinal in errors (`chunk 12, row 3: …`).

---

## 4. Shared execution pipeline

Both tiers converge on one loop (`ExecuteBulkRows` `[PROPOSED name]`), per
target relation, per write scope:

```
for each row:
    1. FK forward check          (before id — a refused row costs nothing)
    2. assertion admission       (before id, sees prior rows' reservations)
    3. AllocateRowId             (Keystone, engine-issued, per row)
    4. EncodeRow + var-heap spill
    5. InsertIntoRelation        (heap chain or clustered tree; min_key and
                                  duplicate enforcement live inside, as today)
    6. Cabin witness             (before the log — C1's ordering argument)
    7. index maintenance         (fails the statement on error, as today)
    8. WAL append                (per-row records, BI10)
any failure → statement/load error with row ordinal → scope unwinds all
```

This is `InsertInner`'s body factored into a function both the dispatcher
(T1) and the load session (T2) call — **a refactor, not a second write
path**. There is exactly one place a row becomes durable state, and it is
the same place for one row and for a million.

Buffer-pool conduct is BI12: the load's append frontier and freshly filled
pages cycle in the EV6 ring, filled pages are queued for background
writeback immediately (they will not be re-read by the load), and the
foreground working set's usage counters never see the load happen.

---

## 5. Interactions, stated one by one

- **Assertions / Bound Cabin** — full per-row admission + reservation, in
  order (§2.3). A load that would break a group bound fails at the exact
  row that breaks it. Bound Cabin's running aggregates are updated per row;
  abort unwinds them through the existing statement-error path.
- **Secondary indexes** — per-row maintenance, failure fails the load
  (`index.md` §2's argument applies row by row). Note for operators,
  not for the engine: dropping indexes before a large load and re-creating
  them after (the IX backfill) is often faster; the engine does not do this
  implicitly.
- **Cabin (observational)** — per-row witness before the log, C1
  preserved. A load into a relation with active observational Cabins pays
  the witness cost; that is the price of C1 and is not negotiable.
- **FK** — forward checks per row against the same read view as today. A
  parent row inserted earlier *in the same statement/load* is visible to a
  later child row's check iff today's single-statement visibility says so —
  no special intra-load visibility is introduced.
- **Waystone / patterns** — T1 fingerprints as BI5; T2 not at all. No
  trail is ever recorded for a write path; nothing changes.
- **Physical optimizer** — no direct coupling. A bulk load lands cold data;
  the optimizer's decayed scores treat it as exactly that. `[OPEN]` whether
  a completed load should hint the optimizer to schedule an early survey.
- **Crosscore** — one relation, one home core, CC3 verbatim (BI11).
  A future multi-relation load is a client loop, not a protocol feature.
- **Eviction** — EV6 ring per BI12; the load is EV6's second consumer after
  the assertion builder.
- **Recovery** — per-row WAL records mean v1 bulk writes replay under the
  same (unbuilt) §12 rules as single-row writes; nothing new to specify,
  which is BI10's point.

---

## 6. Observability & limits

- `S_COMPLETE` tag `LOAD`, `rows_affected` = rows committed.
- Per-core counters: `bulk_rows_loaded`, `bulk_chunks`, `bulk_refusals`,
  `bulk_wal_stall_us` — production metrics per the EV9/observability
  convention, ANALYZE-adjacent for T1.
- Limits, all config keys, all refused truthfully when exceeded:
  `kds.max_insert_rows` (T1), `max_chunk_bytes` and `window` (T2,
  server-announced), `kMaxFrame` still bounds any single frame.

---

## 7. Testing obligations

- **Pipeline equivalence**: a T1 N-row statement and N single-row
  statements in one transaction leave byte-identical relation state
  (modulo Keystone ids), verified by the integrity sweep.
- **Fingerprint pin**: 1-row and N-row templates hash equal; the 1-row hash
  equals its pre-BI value.
- **Assertion accumulation**: a statement violating a group bound only in
  aggregate fails at the correct row ordinal; the same rows split across
  two statements behave per plain admission.
- **Atomicity**: mid-statement and mid-chunk failure leaves zero rows,
  including var-heap spills and index entries, across all durability
  classes.
- **Flow control**: a stalled WAL ring closes the T2 window; no unbounded
  buffering under a fast client (deterministic sim, fault injection).
- **Crash mid-load** `[GATED on recovery, wal.md §12]`: restart after a
  crash between chunks recovers to the transaction boundary — no partial
  load visible. Until replay exists, the test asserts the on-disk log
  shape only.
- **EV12-style small-pool profile**: a load larger than the buffer pool
  completes without evicting a pinned page class and without foreground
  usage-counter movement.

---

## 8. Tier 3 — the sorted fast path, reserved

**Not in v1, by decision BI1.** Stated here so the reservation is a design,
not an omission.

T3 is the only tier that changes *how* a row becomes durable: pre-reserve a
contiguous Keystone id range, sort the batch by assigned id (arrival order,
if the range is issued monotonically), fill heap pages directly at the
append frontier — each page's `min_key` set once, exactly, from the sorted
stream, which is the semi-sorted heap's best case — and build the clustered
tree **bottom-up** from the filled pages instead of descending per tuple.
Expected effect: removes step 5's per-tuple descent and split churn, the
last per-row cost standing after T1/T2.

Prerequisites, each currently absent, which is the whole argument for
reserving it:

1. **Id-range reservation** — an `AllocateRowIdRange(n)` catalog operation
   with monotone issuance inside the range. Today ids are per-row and
   monotonicity is deliberately not an invariant (`keystoneid-invariant.md`);
   a range API must state its burn semantics on abort against the K1 budget.
2. **Batched WAL record** — a page-image or multi-row record type (BI10's
   `[OPEN]`), or T3 loads are logged per row and the win shrinks; interacts
   with FPI and torn-write rules, so it waits for recovery to exist first.
3. **Page epoch** — already the gating item for relayout and trail replay
   (`waystone-concpets.md`); bottom-up build republishes tree structure and
   must participate in the same epoch discipline.
4. **Assertion/index/Cabin ordering under batch placement** — BI2's per-row
   order must be re-derived for page-at-a-time placement or T3 must be
   refused on relations carrying assertions/indexes/Cabins
   `[OPEN: which]`.
5. **Restriction surface** — empty relation only, or any append frontier?
   `[OPEN]` — the empty-relation form is the migration case and the simpler
   correctness argument.

T3 arrives with the offline re-key operation, its natural first consumer:
re-key is by definition a sorted full-relation rewrite, exactly this shape.

---

## 9. What this spec deliberately does not settle

- The `kds.max_insert_rows`, `window`, `max_chunk_bytes` defaults
  (`[PROPOSED]` values above; confirm at build time).
- A resume/dedup protocol for interrupted loads (BI14 `[OPEN]`).
- A batched WAL record format (BI10, T3 prerequisite 2).
- Whether a completed load should nudge the physical optimizer (§5).
- Client-side ergonomics — the KDS Studio import UI and the migration
  tool's use of `BULK_LOAD` are client-manual material, not engine spec.
